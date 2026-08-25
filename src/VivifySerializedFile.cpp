#include "VivifySerializedFile.hpp"

#include <algorithm>
#include <cstring>

namespace SerializedFileParse {
namespace {

// Unity ClassID for UnityEngine.Shader.
constexpr int32_t kClassIdShader = 48;

// Type-tree node flags. Bit 0 of typeFlags marks a node whose children are the
// [size, data] pair of an array; bit 14 of metaFlag requests 4-byte alignment
// after the field has been read.
constexpr uint8_t kTypeFlagIsArray = 0x1;
constexpr uint32_t kMetaFlagAlignBytes = 0x4000;

// A string offset with the high bit set indexes Unity's built-in common string
// table rather than this file's own buffer.
constexpr uint32_t kCommonStringBit = 0x80000000u;

// Reader over serialized-file metadata. The header itself is big-endian; every
// field after it follows the file's endianness flag, which is little-endian in
// practice for every platform Unity still ships.
class Reader {
 public:
  Reader(uint8_t const* data, size_t size) : _data(data), _size(size) {}

  bool ok() const { return _ok; }
  size_t position() const { return _pos; }
  size_t remaining() const { return _pos <= _size ? _size - _pos : 0; }
  void setBigEndian(bool big) { _big = big; }

  void seek(size_t pos) {
    if (pos > _size) { _ok = false; return; }
    _pos = pos;
  }

  void skip(size_t count) {
    if (count > remaining()) { _ok = false; _pos = _size; return; }
    _pos += count;
  }

  void align4() {
    size_t const rem = _pos % 4;
    if (rem != 0) skip(4 - rem);
  }

  uint8_t u8() {
    if (remaining() < 1) { _ok = false; return 0; }
    return _data[_pos++];
  }

  uint16_t u16() {
    uint8_t b[2];
    if (!raw(b, 2)) return 0;
    return _big ? static_cast<uint16_t>((b[0] << 8) | b[1])
                : static_cast<uint16_t>((b[1] << 8) | b[0]);
  }

  uint32_t u32() {
    uint8_t b[4];
    if (!raw(b, 4)) return 0;
    if (!_big) std::swap(b[0], b[3]), std::swap(b[1], b[2]);
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  }

  uint64_t u64() {
    uint8_t b[8];
    if (!raw(b, 8)) return 0;
    if (!_big) for (int i = 0; i < 4; i++) std::swap(b[i], b[7 - i]);
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | b[i];
    return value;
  }

  uint32_t u32be() {
    bool const saved = _big;
    _big = true;
    uint32_t const value = u32();
    _big = saved;
    return value;
  }

  uint64_t u64be() {
    bool const saved = _big;
    _big = true;
    uint64_t const value = u64();
    _big = saved;
    return value;
  }

  // NUL-terminated string; a missing terminator within `limit` is a failure.
  std::string cstring(size_t limit) {
    std::string out;
    while (out.size() < limit) {
      if (remaining() < 1) { _ok = false; return {}; }
      char const c = static_cast<char>(_data[_pos++]);
      if (c == '\0') return out;
      out.push_back(c);
    }
    _ok = false;
    return {};
  }

  bool raw(uint8_t* dest, size_t count) {
    if (remaining() < count) { _ok = false; _pos = _size; return false; }
    std::memcpy(dest, _data + _pos, count);
    _pos += count;
    return true;
  }

 private:
  uint8_t const* _data;
  size_t _size;
  size_t _pos = 0;
  bool _ok = true;
  bool _big = false;
};

struct TypeTreeNode {
  uint16_t version = 0;
  uint8_t level = 0;
  uint8_t typeFlags = 0;
  uint32_t typeStrOffset = 0;
  uint32_t nameStrOffset = 0;
  int32_t byteSize = 0;
  int32_t index = 0;
  uint32_t metaFlag = 0;
};

struct SerializedTypeInfo {
  int32_t classID = 0;
  std::vector<TypeTreeNode> nodes;
  std::string stringBuffer;
};

struct ObjectInfo {
  int64_t byteStart = 0;
  uint32_t byteSize = 0;
  int32_t typeIndex = 0;
};

// Resolves a node's name. Offsets into Unity's built-in common table (high bit
// set) come back empty: traversal never needs them, and reproducing that table's
// exact byte offsets from memory would be a guess.
std::string NodeName(SerializedTypeInfo const& type, TypeTreeNode const& node) {
  if ((node.nameStrOffset & kCommonStringBit) != 0) return {};
  if (node.nameStrOffset >= type.stringBuffer.size()) return {};
  char const* base = type.stringBuffer.data() + node.nameStrOffset;
  size_t const maxLen = type.stringBuffer.size() - node.nameStrOffset;
  size_t len = 0;
  while (len < maxLen && base[len] != '\0') len++;
  return std::string(base, len);
}

// Indices of the direct children of nodes[parent] in the flat, depth-ordered
// node array: every node deeper than the parent belongs to it, and the run ends
// at the first node back at the parent's level or shallower.
std::vector<size_t> ChildIndices(std::vector<TypeTreeNode> const& nodes, size_t parent) {
  std::vector<size_t> children;
  if (parent >= nodes.size()) return children;
  uint8_t const parentLevel = nodes[parent].level;
  for (size_t i = parent + 1; i < nodes.size(); i++) {
    if (nodes[i].level <= parentLevel) break;
    if (nodes[i].level == parentLevel + 1) children.push_back(i);
  }
  return children;
}

// Walks one field, advancing the reader past it. `collectInts`, when non-null,
// receives the elements of this field if it is an array of 4-byte primitives --
// which is how the `platforms` array is read without special-casing its layout.
void WalkNode(Reader& reader, SerializedTypeInfo const& type, size_t nodeIndex,
              std::vector<int32_t>* collectInts, int depth) {
  if (!reader.ok() || nodeIndex >= type.nodes.size()) return;
  // Bundles are untrusted input; a cyclic or absurdly deep tree must not
  // recurse without bound.
  if (depth > 48) { reader.skip(reader.remaining()); return; }

  TypeTreeNode const& node = type.nodes[nodeIndex];
  std::vector<size_t> const children = ChildIndices(type.nodes, nodeIndex);

  if ((node.typeFlags & kTypeFlagIsArray) != 0) {
    // Children are exactly [size, data]. The size is a plain int; the element
    // layout is the data node, repeated.
    if (children.size() < 2) { reader.skip(reader.remaining()); return; }
    uint32_t const count = reader.u32();
    if (!reader.ok()) return;
    size_t const dataNode = children[1];
    TypeTreeNode const& element = type.nodes[dataNode];
    bool const elementIsFlat = element.byteSize > 0 && ChildIndices(type.nodes, dataNode).empty();

    if (elementIsFlat) {
      // A flat element array is a contiguous run; read it in one step rather
      // than looping, and refuse a count the remaining bytes cannot support.
      uint64_t const bytes = static_cast<uint64_t>(count) * static_cast<uint64_t>(element.byteSize);
      if (bytes > reader.remaining()) { reader.skip(reader.remaining()); return; }
      if (collectInts != nullptr && element.byteSize == 4) {
        collectInts->reserve(count);
        for (uint32_t i = 0; i < count; i++) collectInts->push_back(static_cast<int32_t>(reader.u32()));
      } else {
        reader.skip(static_cast<size_t>(bytes));
      }
    } else {
      if (count > reader.remaining()) { reader.skip(reader.remaining()); return; }
      for (uint32_t i = 0; i < count && reader.ok(); i++) {
        WalkNode(reader, type, dataNode, nullptr, depth + 1);
      }
    }
  } else if (children.empty()) {
    if (node.byteSize < 0) { reader.skip(reader.remaining()); return; }
    reader.skip(static_cast<size_t>(node.byteSize));
  } else {
    for (size_t child : children) {
      if (!reader.ok()) return;
      // A field like `platforms` is a *vector* node wrapping the actual Array
      // node, so the collector has to be handed down through the wrapper or it
      // would never reach the elements. Stop propagating once something has
      // been collected, so a struct containing several int arrays cannot merge
      // them together.
      bool const stillLooking = collectInts != nullptr && collectInts->empty();
      WalkNode(reader, type, child, stillLooking ? collectInts : nullptr, depth + 1);
    }
  }

  if ((node.metaFlag & kMetaFlagAlignBytes) != 0) reader.align4();
}

// Reads a Shader object: walks its top-level fields in order, capturing the one
// named "platforms" and the shader's name if it is reachable by name.
ShaderObject ReadShaderObject(uint8_t const* data, size_t size, SerializedTypeInfo const& type) {
  ShaderObject shader;
  if (type.nodes.empty()) return shader;

  Reader reader(data, size);
  for (size_t child : ChildIndices(type.nodes, 0)) {
    if (!reader.ok()) break;
    std::string const name = NodeName(type, type.nodes[child]);
    if (name == "platforms") {
      WalkNode(reader, type, child, &shader.platforms, 1);
    } else {
      size_t const before = reader.position();
      WalkNode(reader, type, child, nullptr, 1);
      // m_ParsedForm carries the shader's name. Rather than decode that whole
      // nested structure, take the first NUL-free run of printable bytes at its
      // start, which is where the serialized m_Name string lands.
      if (shader.name.empty() && name == "m_ParsedForm" && reader.position() > before) {
        Reader nameReader(data + before, std::min<size_t>(reader.position() - before, 512));
        uint32_t const length = nameReader.u32();
        if (nameReader.ok() && length > 0 && length <= 256 && length <= nameReader.remaining()) {
          std::string candidate;
          bool printable = true;
          for (uint32_t i = 0; i < length; i++) {
            char const c = static_cast<char>(nameReader.u8());
            if (c < 0x20 || c > 0x7e) { printable = false; break; }
            candidate.push_back(c);
          }
          if (printable && nameReader.ok()) shader.name = candidate;
        }
      }
    }
  }
  return shader;
}

}  // namespace

std::string_view ShaderPlatformName(int32_t platform) {
  switch (platform) {
    case kShaderPlatformGL: return "OpenGL";
    case kShaderPlatformD3D9: return "Direct3D 9";
    case 2: return "Xbox360";
    case 3: return "PS3";
    case kShaderPlatformD3D11: return "Direct3D 11";
    case kShaderPlatformGLES20: return "OpenGL ES 2.0";
    case 8: return "Direct3D 11 9.x";
    case kShaderPlatformGLES3Plus: return "OpenGL ES 3.x";
    case 11: return "PS4";
    case 12: return "Xbox One";
    case kShaderPlatformMetal: return "Metal";
    case kShaderPlatformOpenGLCore: return "OpenGL Core";
    case kShaderPlatformVulkan: return "Vulkan";
    case 19: return "Switch";
    case 23: return "PS5";
    default: return "unknown";
  }
}

bool ShaderPlatformRunsOnQuest(int32_t platform) {
  // Quest's Unity player is built against GLES3 and/or Vulkan; nothing else in
  // the list can be handed to an Adreno driver.
  return platform == kShaderPlatformGLES3Plus || platform == kShaderPlatformVulkan;
}

FileReport InspectSerializedFile(uint8_t const* data, size_t size) {
  FileReport report;
  if (data == nullptr || size < 32) return report;

  Reader reader(data, size);
  uint64_t metadataSize = reader.u32be();
  uint64_t fileSize = reader.u32be();
  uint32_t const version = reader.u32be();
  uint64_t dataOffset = reader.u32be();
  if (!reader.ok() || version < 8 || version > 100) return report;

  bool bigEndian = true;
  if (version >= 9) {
    bigEndian = reader.u8() != 0;
    reader.skip(3);
  }
  if (version >= 22) {
    metadataSize = reader.u32be();
    fileSize = reader.u64be();
    dataOffset = reader.u64be();
    reader.skip(8);  // reserved
  }
  if (!reader.ok()) return report;
  // Same consistency test the converter uses to tell a serialized file apart
  // from a raw .resS payload node that happens to sit in the same archive.
  if (fileSize == 0 || fileSize > size) return report;
  if (dataOffset == 0 || dataOffset > fileSize) return report;
  if (metadataSize == 0 || metadataSize > fileSize) return report;

  reader.setBigEndian(bigEndian);
  report.isSerializedFile = true;

  if (version >= 7) {
    report.unityVersion = reader.cstring(64);
    if (!reader.ok() || report.unityVersion.empty()) {
      report.message = "unreadable Unity version string";
      return report;
    }
    // This string goes straight into a log line, and a corrupted or hostile
    // bundle can put arbitrary bytes here. Keep it to printable ASCII.
    for (char& c : report.unityVersion) {
      if (c < 0x20 || c > 0x7e) c = '?';
    }
  }
  if (version >= 8) reader.skip(4);  // m_TargetPlatform

  bool enableTypeTree = true;
  if (version >= 13) enableTypeTree = reader.u8() != 0;
  report.typeTreePresent = enableTypeTree;

  int32_t const typeCount = static_cast<int32_t>(reader.u32());
  if (!reader.ok() || typeCount < 0 || static_cast<uint32_t>(typeCount) > size) {
    report.message = "implausible type count";
    return report;
  }

  std::vector<SerializedTypeInfo> types;
  types.reserve(static_cast<size_t>(typeCount));
  for (int32_t i = 0; i < typeCount && reader.ok(); i++) {
    SerializedTypeInfo type;
    type.classID = static_cast<int32_t>(reader.u32());
    if (version >= 16) reader.u8();                       // isStrippedType
    if (version >= 17) reader.u16();                      // scriptTypeIndex
    if (version >= 13) {
      bool const hasScriptHash = (version < 16 && type.classID < 0) ||
                                 (version >= 16 && type.classID == 114);
      if (hasScriptHash) reader.skip(16);
      reader.skip(16);                                    // old type hash
    }
    if (enableTypeTree) {
      int32_t const nodeCount = static_cast<int32_t>(reader.u32());
      int32_t const stringBufferSize = static_cast<int32_t>(reader.u32());
      if (!reader.ok() || nodeCount < 0 || stringBufferSize < 0 ||
          static_cast<uint32_t>(nodeCount) > size || static_cast<uint32_t>(stringBufferSize) > size) {
        report.message = "implausible type tree size";
        return report;
      }
      type.nodes.reserve(static_cast<size_t>(nodeCount));
      for (int32_t n = 0; n < nodeCount && reader.ok(); n++) {
        TypeTreeNode node;
        node.version = reader.u16();
        node.level = reader.u8();
        node.typeFlags = reader.u8();
        node.typeStrOffset = reader.u32();
        node.nameStrOffset = reader.u32();
        node.byteSize = static_cast<int32_t>(reader.u32());
        node.index = static_cast<int32_t>(reader.u32());
        node.metaFlag = reader.u32();
        if (version >= 19) reader.skip(8);  // ref type hash
        type.nodes.push_back(node);
      }
      if (!reader.ok()) { report.message = "truncated type tree"; return report; }
      type.stringBuffer.resize(static_cast<size_t>(stringBufferSize));
      if (stringBufferSize > 0) {
        reader.raw(reinterpret_cast<uint8_t*>(type.stringBuffer.data()),
                   static_cast<size_t>(stringBufferSize));
      }
      if (version >= 21) {
        int32_t const dependencyCount = static_cast<int32_t>(reader.u32());
        if (reader.ok() && dependencyCount > 0 && static_cast<uint32_t>(dependencyCount) < size) {
          reader.skip(static_cast<size_t>(dependencyCount) * 4);
        }
      }
    }
    types.push_back(std::move(type));
  }
  if (!reader.ok()) { report.message = "truncated type table"; return report; }

  int32_t const objectCount = static_cast<int32_t>(reader.u32());
  if (!reader.ok() || objectCount < 0 || static_cast<uint32_t>(objectCount) > size) {
    report.message = "implausible object count";
    return report;
  }
  report.objectCount = objectCount;

  std::vector<ObjectInfo> objects;
  objects.reserve(static_cast<size_t>(objectCount));
  for (int32_t i = 0; i < objectCount && reader.ok(); i++) {
    if (version >= 14) reader.align4();
    ObjectInfo object;
    reader.u64();  // pathID
    object.byteStart = version >= 22 ? static_cast<int64_t>(reader.u64())
                                     : static_cast<int64_t>(reader.u32());
    object.byteSize = reader.u32();
    object.typeIndex = static_cast<int32_t>(reader.u32());
    objects.push_back(object);
  }
  if (!reader.ok()) { report.message = "truncated object table"; return report; }

  report.parsed = true;
  for (auto const& object : objects) {
    if (object.typeIndex < 0 || static_cast<size_t>(object.typeIndex) >= types.size()) continue;
    SerializedTypeInfo const& type = types[static_cast<size_t>(object.typeIndex)];
    if (type.classID != kClassIdShader) continue;
    report.shaderObjectCount++;
    if (!enableTypeTree || type.nodes.empty()) continue;

    uint64_t const start = static_cast<uint64_t>(object.byteStart) + dataOffset;
    if (start >= size || object.byteSize > size - start) continue;
    report.shaders.push_back(ReadShaderObject(data + start, object.byteSize, type));
  }

  if (!enableTypeTree && report.shaderObjectCount > 0) {
    report.message = "type tree stripped: shader objects found but their contents cannot be decoded";
  }
  return report;
}

}  // namespace SerializedFileParse
