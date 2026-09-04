#include "VivifySerializedFile.hpp"

#include <algorithm>
#include <cstring>

namespace SerializedFileParse {
namespace {

// Unity ClassID for UnityEngine.Shader.
constexpr int32_t kClassIdShader = 48;
constexpr int32_t kClassIdTexture2D = 28;

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
  int64_t pathID = 0;
  int64_t byteStart = 0;
  uint32_t byteSize = 0;
  int32_t typeIndex = 0;
  // Where this entry's byteStart and byteSize fields sit in the file. The
  // rewriter patches them in place rather than rebuilding the metadata, which
  // is what keeps the type tree, externals and everything else byte-identical.
  size_t byteStartFieldOffset = 0;
  size_t byteSizeFieldOffset = 0;
};

// Everything about a SerializedFile that both reading its shaders and rewriting
// its objects need. Parsed once, by ParseLayout.
struct Layout {
  bool isSerializedFile = false;
  bool parsed = false;
  uint32_t version = 0;
  bool bigEndian = false;
  size_t headerLength = 0;
  uint64_t metadataSize = 0;
  uint64_t fileSize = 0;
  uint64_t dataOffset = 0;
  bool typeTreePresent = false;
  std::string unityVersion;
  std::vector<SerializedTypeInfo> types;
  std::vector<ObjectInfo> objects;
  std::string message;
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

// What a walk of one named field should bring back.
//
// A field is described by the type tree, not by this code, so the same capture
// serves `platforms` (vector<int>, one group), `offsets` (vector<vector<int>>,
// one group per inner array) and `compressedBlob` (vector<UInt8>, a byte range).
// Nothing here needs to know which of those it is looking at.
struct FieldCapture {
  bool wantBytes = false;

  std::vector<std::vector<int32_t>> groups;  // one per int array encountered
  // Where each group's first element sits, relative to the start of the object
  // body. Conversion patches these tables in place -- the counts do not change,
  // only the values -- so the writer needs to know where they are rather than
  // having to re-serialize the whole object around them.
  std::vector<size_t> groupOffsets;
  bool sawBytes = false;
  size_t byteOffset = 0;  // relative to the start of the object body
  size_t byteCount = 0;
  // Whether the byte array is followed by alignment padding. A converted blob
  // is a different length, so the padding after it has to be recomputed, and
  // that is only correct if it is known to be there in the first place.
  bool bytesAligned = false;

  bool empty() const { return groups.empty() && !sawBytes; }
};

// Walks one field, advancing the reader past it. `capture`, when non-null,
// records this field's contents -- which is how `platforms`, the three length
// tables and the blob are all read without special-casing any of their layouts.
void WalkNode(Reader& reader, SerializedTypeInfo const& type, size_t nodeIndex,
              FieldCapture* capture, int depth) {
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
      if (capture != nullptr && capture->wantBytes && element.byteSize == 1) {
        // The blob is recorded as a range, not copied: it is the largest thing
        // in the file by a wide margin.
        capture->sawBytes = true;
        capture->byteOffset = reader.position();
        capture->byteCount = static_cast<size_t>(bytes);
        capture->bytesAligned = (node.metaFlag & kMetaFlagAlignBytes) != 0 ||
                                (element.metaFlag & kMetaFlagAlignBytes) != 0;
        reader.skip(static_cast<size_t>(bytes));
      } else if (capture != nullptr && !capture->wantBytes && element.byteSize == 4) {
        std::vector<int32_t> group;
        group.reserve(count);
        capture->groupOffsets.push_back(reader.position());
        for (uint32_t i = 0; i < count; i++) group.push_back(static_cast<int32_t>(reader.u32()));
        capture->groups.push_back(std::move(group));
      } else {
        reader.skip(static_cast<size_t>(bytes));
      }
    } else {
      if (count > reader.remaining()) { reader.skip(reader.remaining()); return; }
      for (uint32_t i = 0; i < count && reader.ok(); i++) {
        // Every element of a nested array belongs to the same field, so the
        // capture stays armed across all of them -- that is what turns
        // vector<vector<int>> into one group per inner array.
        WalkNode(reader, type, dataNode, capture, depth + 1);
      }
    }
  } else if (children.empty()) {
    if (node.byteSize < 0) { reader.skip(reader.remaining()); return; }
    reader.skip(static_cast<size_t>(node.byteSize));
  } else {
    for (size_t child : children) {
      if (!reader.ok()) return;
      // A field like `platforms` is a *vector* node wrapping the actual Array
      // node, so the capture has to be handed down through the wrapper or it
      // would never reach the elements. A wrapper is recognisable by having
      // exactly one child, and always passes the capture on -- which is what
      // lets vector<vector<int>> keep collecting past its first inner array.
      //
      // A real struct has several children, and there the capture stops at the
      // first thing collected so unrelated sibling arrays cannot be merged.
      bool const isWrapper = children.size() == 1;
      bool const stillLooking = capture != nullptr && (isWrapper || capture->empty());
      WalkNode(reader, type, child, stillLooking ? capture : nullptr, depth + 1);
    }
  }

  if ((node.metaFlag & kMetaFlagAlignBytes) != 0) reader.align4();
}

// Reads a Shader object: walks its top-level fields in order, capturing the one
// named "platforms" and the shader's name if it is reachable by name.
ShaderObject ReadShaderObject(uint8_t const* data, size_t size, size_t fileOffset,
                              SerializedTypeInfo const& type) {
  ShaderObject shader;
  if (type.nodes.empty()) return shader;

  // The five fields that describe the compiled program store. Everything else
  // in a Shader object is walked only to stay in step with the byte stream.
  auto captureInts = [&](Reader& reader, size_t child, std::vector<std::vector<int32_t>>& into,
                         std::vector<size_t>& locations) {
    FieldCapture capture;
    WalkNode(reader, type, child, &capture, 1);
    into = std::move(capture.groups);
    locations.clear();
    for (size_t offset : capture.groupOffsets) locations.push_back(fileOffset + offset);
  };

  shader.bodyFileOffset = fileOffset;
  shader.bodySize = size;
  Reader reader(data, size);
  for (size_t child : ChildIndices(type.nodes, 0)) {
    if (!reader.ok()) break;
    std::string const name = NodeName(type, type.nodes[child]);
    if (name == "platforms") {
      FieldCapture capture;
      WalkNode(reader, type, child, &capture, 1);
      // platforms is flat, so there is exactly one group; older bundles that
      // nest it would give several, and concatenating them is still correct.
      for (auto const& group : capture.groups) {
        shader.platforms.insert(shader.platforms.end(), group.begin(), group.end());
      }
      for (size_t offset : capture.groupOffsets) {
        shader.platformsTableOffsets.push_back(fileOffset + offset);
      }
    } else if (name == "offsets") {
      captureInts(reader, child, shader.offsets, shader.offsetsTableOffsets);
    } else if (name == "compressedLengths") {
      captureInts(reader, child, shader.compressedLengths, shader.compressedLengthsTableOffsets);
    } else if (name == "decompressedLengths") {
      captureInts(reader, child, shader.decompressedLengths,
                  shader.decompressedLengthsTableOffsets);
    } else if (name == "compressedBlob") {
      FieldCapture capture;
      capture.wantBytes = true;
      WalkNode(reader, type, child, &capture, 1);
      if (capture.sawBytes) {
        shader.blobPresent = true;
        shader.blobFileOffset = fileOffset + capture.byteOffset;
        shader.blobSize = capture.byteCount;
        shader.blobAligned = capture.bytesAligned;
      }
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

// Reads a Texture2D object: walks its top-level fields in order, keeping the
// handful that decide whether its pixels can be reached on device.
//
// Nothing here is positional. Unity has moved Texture2D's fields around several
// times -- m_MipsStripped, m_IsAlphaChannelOptional and m_IgnoreMipmapLimit all
// arrived in different versions -- so every field is found by the name the
// file's own type tree gives it, and anything unrecognised is walked past to
// stay in step with the byte stream.
TextureObject ReadTextureObject(uint8_t const* data, size_t size, size_t fileOffset,
                                SerializedTypeInfo const& type) {
  TextureObject texture;
  if (type.nodes.empty()) return texture;
  texture.bodyFileOffset = fileOffset;
  texture.bodySize = size;

  auto readI32 = [&](size_t at) -> int32_t {
    if (at + 4 > size) return 0;
    uint32_t value = 0;
    std::memcpy(&value, data + at, 4);
    return static_cast<int32_t>(value);
  };

  Reader reader(data, size);
  for (size_t child : ChildIndices(type.nodes, 0)) {
    if (!reader.ok()) break;
    std::string const name = NodeName(type, type.nodes[child]);
    size_t const before = reader.position();

    if (name == "m_StreamData") {
      // Walked child by child rather than in one step, because which of them is
      // "size" and which is "path" is the whole question: a texture whose
      // pixels live in a companion .resS node has nothing inline to decode.
      for (size_t sub : ChildIndices(type.nodes, child)) {
        if (!reader.ok()) break;
        std::string const subName = NodeName(type, type.nodes[sub]);
        size_t const subBefore = reader.position();
        WalkNode(reader, type, sub, nullptr, 2);
        if (reader.position() <= subBefore) continue;
        if (subName == "size") {
          texture.streamDataSize = static_cast<uint32_t>(readI32(subBefore));
        } else if (subName == "path") {
          // A string is a length-prefixed byte array; a non-empty one means the
          // pixels are somewhere else.
          if (readI32(subBefore) > 0) texture.streamed = true;
        }
      }
      // WalkNode would have applied this after its children; walking them by
      // hand means applying it by hand.
      if ((type.nodes[child].metaFlag & kMetaFlagAlignBytes) != 0) reader.align4();
      continue;
    }

    if (name == "image data") {
      FieldCapture capture;
      capture.wantBytes = true;
      WalkNode(reader, type, child, &capture, 1);
      if (capture.sawBytes) {
        texture.imageDataFileOffset = fileOffset + capture.byteOffset;
        texture.imageDataSize = capture.byteCount;
      }
      continue;
    }

    WalkNode(reader, type, child, nullptr, 1);
    if (reader.position() <= before) continue;

    if (name == "m_Name") {
      uint32_t const length = static_cast<uint32_t>(readI32(before));
      if (length > 0 && length <= 256 && before + 4 + length <= size) {
        texture.name.assign(reinterpret_cast<char const*>(data + before + 4), length);
      }
    } else if (name == "m_Width") {
      texture.width = readI32(before);
    } else if (name == "m_Height") {
      texture.height = readI32(before);
    } else if (name == "m_MipCount") {
      texture.mipCount = readI32(before);
    } else if (name == "m_TextureFormat") {
      texture.textureFormat = readI32(before);
    } else if (name == "m_CompleteImageSize") {
      texture.completeImageSize = readI32(before);
    } else if (name == "m_IsReadable") {
      // Only a genuine one-byte bool is claimed. A field of any other width
      // under this name is something this code does not understand, and writing
      // to it would corrupt the object.
      if (type.nodes[child].byteSize == 1 && before < size) {
        texture.isReadablePresent = true;
        texture.isReadableFileOffset = fileOffset + before;
        texture.isReadable = data[before] != 0;
      }
    }
  }
  return texture;
}

}  // namespace

bool TextureFormatNeedsDecodingOnQuest(int32_t unityTextureFormat) {
  switch (unityTextureFormat) {
    case 10:  // DXT1 / BC1
    case 11:  // DXT3
    case 12:  // DXT5 / BC3
    case 24:  // BC6H
    case 25:  // BC7
    case 26:  // BC4
    case 27:  // BC5
      return true;
    default:
      return false;
  }
}

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

std::string_view GpuProgramTypeName(int32_t programType) {
  switch (programType) {
    case kGpuProgramGLLegacy: return "GL legacy";
    case kGpuProgramGLES31AEP: return "GLES 3.1 AEP";
    case kGpuProgramGLES31: return "GLES 3.1";
    case kGpuProgramGLES3: return "GLES 3.0";
    case kGpuProgramGLES: return "GLES 2.0";
    case kGpuProgramGLCore32: return "GL core 3.2";
    case kGpuProgramGLCore41: return "GL core 4.1";
    case kGpuProgramGLCore43: return "GL core 4.3";
    case kGpuProgramDX11VertexSM40: return "D3D11 vertex sm4.0";
    case kGpuProgramDX11VertexSM50: return "D3D11 vertex sm5.0";
    case kGpuProgramDX11PixelSM40: return "D3D11 pixel sm4.0";
    case kGpuProgramDX11PixelSM50: return "D3D11 pixel sm5.0";
    case kGpuProgramDX11GeometrySM40: return "D3D11 geometry sm4.0";
    case kGpuProgramDX11GeometrySM50: return "D3D11 geometry sm5.0";
    case kGpuProgramDX11HullSM50: return "D3D11 hull sm5.0";
    case kGpuProgramDX11DomainSM50: return "D3D11 domain sm5.0";
    case kGpuProgramMetalVS: return "Metal vertex";
    case kGpuProgramMetalFS: return "Metal fragment";
    case kGpuProgramSPIRV: return "SPIR-V";
    default: return "unknown";
  }
}

bool GpuProgramIsGlslSource(int32_t programType) {
  switch (programType) {
    case kGpuProgramGLLegacy:
    case kGpuProgramGLES31AEP:
    case kGpuProgramGLES31:
    case kGpuProgramGLES3:
    case kGpuProgramGLES:
    case kGpuProgramGLCore32:
    case kGpuProgramGLCore41:
    case kGpuProgramGLCore43:
      return true;
    default:
      return false;
  }
}

size_t Lz4CompressBound(size_t srcSize) {
  // One token plus a length extension byte per 255, plus the literals.
  return srcSize + srcSize / 255 + 16;
}

// LZ4 block format, compression.
//
// A single hash table of recent positions, one probe per position: the same
// shape as LZ4's "fast" mode. Shader blobs are GLSL text and DXBC, both of
// which this finds plenty of matches in, and conversion happens once per map on
// a device that would rather not spend a minute searching for a better one.
//
// Every write is bounds-checked and the function gives up rather than
// overrunning, because the caller sizes the buffer from Lz4CompressBound and a
// mistake there must not become a heap overflow on a headset.
size_t Lz4CompressBlock(uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
  if (src == nullptr || dst == nullptr || srcSize == 0) return 0;

  constexpr size_t kMinMatch = 4;
  constexpr size_t kLastLiterals = 5;
  // The format forbids a match starting inside the last 12 bytes.
  constexpr size_t kMatchFindLimit = 12;
  constexpr uint32_t kEmpty = 0xffffffffu;
  constexpr int kHashBits = 12;

  auto read32 = [src](size_t at) {
    return static_cast<uint32_t>(src[at]) | (static_cast<uint32_t>(src[at + 1]) << 8) |
           (static_cast<uint32_t>(src[at + 2]) << 16) | (static_cast<uint32_t>(src[at + 3]) << 24);
  };
  auto hash = [](uint32_t value) { return (value * 2654435761u) >> (32 - kHashBits); };

  size_t op = 0;
  size_t anchor = 0;

  // Writes a token's length nibble plus any extension bytes. Returns false if
  // the output buffer is full.
  auto writeLength = [&](size_t length, uint8_t& nibble) {
    if (length < 15) {
      nibble = static_cast<uint8_t>(length);
      return true;
    }
    nibble = 15;
    size_t rest = length - 15;
    while (rest >= 255) {
      if (op >= dstCapacity) return false;
      dst[op++] = 255;
      rest -= 255;
    }
    if (op >= dstCapacity) return false;
    dst[op++] = static_cast<uint8_t>(rest);
    return true;
  };

  size_t const matchLimit = srcSize > kLastLiterals ? srcSize - kLastLiterals : 0;
  size_t const findLimit = srcSize > kMatchFindLimit ? srcSize - kMatchFindLimit : 0;

  if (findLimit > 0) {
    std::vector<uint32_t> table(static_cast<size_t>(1) << kHashBits, kEmpty);
    table[hash(read32(0))] = 0;
    size_t ip = 1;
    while (ip < findLimit) {
      uint32_t const h = hash(read32(ip));
      uint32_t const candidate = table[h];
      table[h] = static_cast<uint32_t>(ip);
      if (candidate == kEmpty || ip - candidate > 65535 || read32(candidate) != read32(ip)) {
        ip++;
        continue;
      }

      size_t matchLength = kMinMatch;
      while (ip + matchLength < matchLimit && src[candidate + matchLength] == src[ip + matchLength]) {
        matchLength++;
      }

      size_t const literalLength = ip - anchor;
      size_t const tokenPos = op;
      if (op >= dstCapacity) return 0;
      op++;

      uint8_t literalNibble = 0;
      if (!writeLength(literalLength, literalNibble)) return 0;
      if (literalLength > dstCapacity - op) return 0;
      std::memcpy(dst + op, src + anchor, literalLength);
      op += literalLength;

      size_t const offset = ip - candidate;
      if (dstCapacity - op < 2) return 0;
      dst[op++] = static_cast<uint8_t>(offset & 0xff);
      dst[op++] = static_cast<uint8_t>(offset >> 8);

      uint8_t matchNibble = 0;
      if (!writeLength(matchLength - kMinMatch, matchNibble)) return 0;
      dst[tokenPos] = static_cast<uint8_t>((literalNibble << 4) | matchNibble);

      ip += matchLength;
      anchor = ip;
      // Seeding one position back picks up matches that start mid-run, which
      // costs nothing and is what LZ4 itself does.
      if (ip >= 2 && ip < findLimit) table[hash(read32(ip - 2))] = static_cast<uint32_t>(ip - 2);
    }
  }

  // The block always ends with a literals-only sequence: the format requires
  // the last five bytes to be literals, and a trailing match has nowhere to
  // put its token.
  size_t const literalLength = srcSize - anchor;
  size_t const tokenPos = op;
  if (op >= dstCapacity) return 0;
  op++;
  uint8_t literalNibble = 0;
  if (!writeLength(literalLength, literalNibble)) return 0;
  if (literalLength > dstCapacity - op) return 0;
  std::memcpy(dst + op, src + anchor, literalLength);
  op += literalLength;
  dst[tokenPos] = static_cast<uint8_t>(literalNibble << 4);
  return op;
}

// LZ4 block format, decompression only.
//
// A block is a sequence of: one token byte, high nibble = literal length, low
// nibble = match length - 4; optional length-extension bytes for either nibble
// (0xf means "add the following bytes until one is not 0xff"); the literals
// themselves; then a 2-byte little-endian offset back into the output followed
// by the match. The final sequence ends after its literals with no match.
//
// Written out here rather than pulled in as a dependency: it is small, it has
// to run inside a Beat Saber mod on a headset, and it is fed untrusted bundle
// bytes, so every read and write is bounds-checked against the two buffers.
size_t Lz4DecodeBlock(uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
  if (src == nullptr || dst == nullptr) return 0;
  size_t sp = 0;
  size_t dp = 0;

  while (sp < srcSize) {
    uint8_t const token = src[sp++];

    size_t literalLength = token >> 4;
    if (literalLength == 15) {
      while (true) {
        if (sp >= srcSize) return 0;
        uint8_t const add = src[sp++];
        literalLength += add;
        if (add != 0xff) break;
        // A run of 0xff bytes could otherwise be used to overflow the length.
        if (literalLength > dstCapacity) return 0;
      }
    }
    if (literalLength > srcSize - sp) return 0;
    if (literalLength > dstCapacity - dp) return 0;
    std::memcpy(dst + dp, src + sp, literalLength);
    sp += literalLength;
    dp += literalLength;

    // The last sequence stops here: no offset follows its literals.
    if (sp == srcSize) break;
    if (srcSize - sp < 2) return 0;
    size_t const offset = static_cast<size_t>(src[sp]) | (static_cast<size_t>(src[sp + 1]) << 8);
    sp += 2;
    if (offset == 0 || offset > dp) return 0;

    size_t matchLength = static_cast<size_t>(token & 0x0f);
    if (matchLength == 15) {
      while (true) {
        if (sp >= srcSize) return 0;
        uint8_t const add = src[sp++];
        matchLength += add;
        if (add != 0xff) break;
        if (matchLength > dstCapacity) return 0;
      }
    }
    matchLength += 4;
    if (matchLength > dstCapacity - dp) return 0;

    // Overlapping matches are legal and common (offset 1 means "repeat the last
    // byte"), so this copies forward one byte at a time rather than memmove.
    size_t from = dp - offset;
    for (size_t i = 0; i < matchLength; i++) dst[dp++] = dst[from++];
  }
  return dp;
}

namespace {

// Reads the [offset, length] table at the start of a decompressed sub-blob.
//
// Unity has used both 8-byte and 12-byte entries here depending on version. The
// entry size is worked out from the data rather than from a version rule: only
// one of the two lays every program inside the blob and clear of the table
// itself, so the layout is checked instead of assumed.
bool ReadProgramTable(uint8_t const* blob, size_t blobSize,
                      std::vector<std::pair<uint32_t, uint32_t>>& out, int32_t& entryWidth) {
  if (blobSize < 4) return false;
  auto readU32 = [blob](size_t at) {
    return static_cast<uint32_t>(blob[at]) | (static_cast<uint32_t>(blob[at + 1]) << 8) |
           (static_cast<uint32_t>(blob[at + 2]) << 16) | (static_cast<uint32_t>(blob[at + 3]) << 24);
  };

  uint32_t const count = readU32(0);
  // A count large enough to overflow the header would be rejected below anyway,
  // but bail early rather than sizing a vector from a hostile number.
  if (count > blobSize / 8) return false;

  for (size_t entrySize : {size_t(8), size_t(12)}) {
    size_t const header = 4 + static_cast<size_t>(count) * entrySize;
    if (header > blobSize) continue;

    std::vector<std::pair<uint32_t, uint32_t>> candidate;
    candidate.reserve(count);
    bool good = true;
    for (uint32_t i = 0; i < count && good; i++) {
      size_t const at = 4 + static_cast<size_t>(i) * entrySize;
      uint32_t const offset = readU32(at);
      uint32_t const length = readU32(at + 4);
      if (offset < header || length == 0) { good = false; break; }
      if (offset > blobSize || length > blobSize - offset) { good = false; break; }
      candidate.emplace_back(offset, length);
    }
    if (good) {
      out = std::move(candidate);
      entryWidth = static_cast<int32_t>(entrySize);
      return true;
    }
  }
  return false;
}

// Parses one compiled sub-program's header.
//
// Layout, as Unity writes it: format version, ShaderGpuProgramType, three ints
// of statistics, a fourth int from 2016.08 onwards, then the keyword table as
// aligned strings, then the program bytes as a byte array.
bool ReadSubProgram(uint8_t const* data, size_t size, ShaderSubProgram& out) {
  Reader reader(data, size);
  out.blobVersion = static_cast<int32_t>(reader.u32());
  out.programType = static_cast<int32_t>(reader.u32());

  // The statistics block is stepped over rather than interpreted, but it is
  // kept: a re-encoded program has to be the one Unity wrote, not a nearly
  // identical one, or a conversion bug and a re-encoding bug look the same.
  size_t const statsSize = out.blobVersion >= 201608170 ? 16u : 12u;
  if (statsSize > reader.remaining()) return false;
  out.stats.resize(statsSize);
  if (!reader.raw(out.stats.data(), statsSize)) return false;

  auto readKeywords = [&reader](std::vector<std::string>& into) {
    uint32_t const count = reader.u32();
    if (!reader.ok() || count > reader.remaining() / 4) return false;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t const length = reader.u32();
      if (!reader.ok() || length > reader.remaining()) return false;
      std::string keyword(length, '\0');
      if (length > 0 && !reader.raw(reinterpret_cast<uint8_t*>(keyword.data()), length)) return false;
      reader.align4();
      into.push_back(std::move(keyword));
    }
    return true;
  };

  if (!readKeywords(out.keywords)) return false;
  // 2018.06 through 2020.12 wrote a second, local keyword table in the same
  // shape immediately after the first. It is kept separate from the first so
  // re-encoding puts each back where it came from.
  if (out.blobVersion >= 201806140 && out.blobVersion < 202012090) {
    if (!readKeywords(out.localKeywords)) return false;
  }

  uint32_t const codeLength = reader.u32();
  if (!reader.ok() || codeLength > reader.remaining()) return false;
  out.code.resize(codeLength);
  if (codeLength > 0 && !reader.raw(out.code.data(), codeLength)) return false;

  // Whatever Unity left after the code -- alignment padding, and any trailing
  // field this code does not model -- travels with the program.
  size_t const trailing = reader.remaining();
  out.trailing.resize(trailing);
  if (trailing > 0 && !reader.raw(out.trailing.data(), trailing)) return false;
  return true;
}

// Serializes a sub-program back into the bytes it was read from. Exactly the
// inverse of ReadSubProgram, field for field.
void WriteSubProgram(ShaderSubProgram const& program, std::vector<uint8_t>& out) {
  size_t const start = out.size();
  auto putU32 = [&out](uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  };
  auto align4 = [&out, start]() {
    while ((out.size() - start) % 4 != 0) out.push_back(0);
  };

  putU32(static_cast<uint32_t>(program.blobVersion));
  putU32(static_cast<uint32_t>(program.programType));
  out.insert(out.end(), program.stats.begin(), program.stats.end());

  auto putKeywords = [&](std::vector<std::string> const& keywords) {
    putU32(static_cast<uint32_t>(keywords.size()));
    for (auto const& keyword : keywords) {
      putU32(static_cast<uint32_t>(keyword.size()));
      out.insert(out.end(), keyword.begin(), keyword.end());
      align4();
    }
  };
  putKeywords(program.keywords);
  if (program.blobVersion >= 201806140 && program.blobVersion < 202012090) {
    putKeywords(program.localKeywords);
  }

  putU32(static_cast<uint32_t>(program.code.size()));
  out.insert(out.end(), program.code.begin(), program.code.end());
  out.insert(out.end(), program.trailing.begin(), program.trailing.end());
}

}  // namespace

namespace {

void WriteLittleU32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xffu);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

}  // namespace

ShaderBodyRewrite BuildShaderObjectBody(uint8_t const* data, size_t size,
                                        ShaderObject const& shader,
                                        std::vector<int32_t> const& platforms,
                                        EncodedProgramStore const& store) {
  ShaderBodyRewrite result;
  if (data == nullptr) {
    result.message = "no buffer to rebuild from";
    return result;
  }
  if (shader.bodySize == 0 || shader.bodyFileOffset > size ||
      shader.bodySize > size - shader.bodyFileOffset) {
    result.message = "the shader's body lies outside the file";
    return result;
  }
  if (!store.ok) {
    result.message = "the program store to write back did not encode";
    return result;
  }
  if (!shader.blobPresent) {
    result.message = "the shader carries no compressedBlob to replace";
    return result;
  }
  if (platforms.size() != shader.platforms.size()) {
    result.message = "the converted platform list is a different length than the shader's";
    return result;
  }
  if (shader.platformsTableOffsets.size() != 1) {
    result.message = "the shader's platforms field is not one flat array";
    return result;
  }
  if (store.offsets.size() != shader.offsets.size() ||
      store.compressedLengths.size() != shader.compressedLengths.size() ||
      store.decompressedLengths.size() != shader.decompressedLengths.size()) {
    result.message = "the converted store has a different number of platform groups";
    return result;
  }
  if (shader.offsetsTableOffsets.size() != shader.offsets.size() ||
      shader.compressedLengthsTableOffsets.size() != shader.compressedLengths.size() ||
      shader.decompressedLengthsTableOffsets.size() != shader.decompressedLengths.size()) {
    result.message = "the shader's length tables were not located while parsing";
    return result;
  }
  for (size_t group = 0; group < store.offsets.size(); group++) {
    if (store.offsets[group].size() != shader.offsets[group].size() ||
        store.compressedLengths[group].size() != shader.compressedLengths[group].size() ||
        store.decompressedLengths[group].size() != shader.decompressedLengths[group].size()) {
      result.message = "the converted store has a different number of sub-blobs in group " +
                       std::to_string(group);
      return result;
    }
  }

  size_t const bodyStart = shader.bodyFileOffset;
  size_t const bodyEnd = bodyStart + shader.bodySize;
  if (shader.blobFileOffset < bodyStart + 4 || shader.blobFileOffset > bodyEnd ||
      shader.blobSize > bodyEnd - shader.blobFileOffset) {
    result.message = "the shader's compressedBlob is not inside its own body";
    return result;
  }

  std::vector<uint8_t> body(data + bodyStart, data + bodyEnd);

  auto patchTable = [&](size_t fileOffset, std::vector<int32_t> const& values) -> bool {
    if (fileOffset < bodyStart) return false;
    size_t const relative = fileOffset - bodyStart;
    uint64_t const bytes = static_cast<uint64_t>(values.size()) * 4u;
    if (relative > body.size() || bytes > body.size() - relative) return false;
    for (size_t i = 0; i < values.size(); i++) {
      WriteLittleU32(body, relative + i * 4, static_cast<uint32_t>(values[i]));
    }
    return true;
  };

  if (!patchTable(shader.platformsTableOffsets[0], platforms)) {
    result.message = "the platforms array does not fit where it was found";
    return result;
  }
  for (size_t group = 0; group < store.offsets.size(); group++) {
    if (!patchTable(shader.offsetsTableOffsets[group], store.offsets[group]) ||
        !patchTable(shader.compressedLengthsTableOffsets[group], store.compressedLengths[group]) ||
        !patchTable(shader.decompressedLengthsTableOffsets[group],
                    store.decompressedLengths[group])) {
      result.message = "a length table does not fit where it was found";
      return result;
    }
  }

  size_t const relativeBlob = shader.blobFileOffset - bodyStart;
  size_t const oldPadding = shader.blobAligned ? (4 - (shader.blobSize % 4)) % 4 : 0;
  if (relativeBlob + shader.blobSize + oldPadding > body.size()) {
    result.message = "the compressedBlob and its padding run past the end of the object";
    return result;
  }
  size_t const newPadding = shader.blobAligned ? (4 - (store.blob.size() % 4)) % 4 : 0;

  std::vector<uint8_t> rebuilt;
  rebuilt.reserve(body.size() - shader.blobSize + store.blob.size() + 4);
  rebuilt.insert(rebuilt.end(), body.begin(), body.begin() + static_cast<long>(relativeBlob) - 4);
  // The array's element count sits immediately before its data.
  uint8_t countBytes[4];
  uint32_t const count = static_cast<uint32_t>(store.blob.size());
  countBytes[0] = static_cast<uint8_t>(count & 0xffu);
  countBytes[1] = static_cast<uint8_t>((count >> 8) & 0xffu);
  countBytes[2] = static_cast<uint8_t>((count >> 16) & 0xffu);
  countBytes[3] = static_cast<uint8_t>((count >> 24) & 0xffu);
  rebuilt.insert(rebuilt.end(), countBytes, countBytes + 4);
  rebuilt.insert(rebuilt.end(), store.blob.begin(), store.blob.end());
  rebuilt.insert(rebuilt.end(), newPadding, 0u);
  rebuilt.insert(rebuilt.end(), body.begin() + static_cast<long>(relativeBlob + shader.blobSize +
                                                                 oldPadding),
                 body.end());

  result.ok = true;
  result.body = std::move(rebuilt);
  return result;
}

EncodedProgramStore EncodeShaderPrograms(std::vector<int32_t> const& platforms,
                                         std::vector<ShaderSubProgram> const& programs) {
  EncodedProgramStore store;
  size_t const groupCount = platforms.size();
  store.offsets.resize(groupCount);
  store.compressedLengths.resize(groupCount);
  store.decompressedLengths.resize(groupCount);

  for (size_t group = 0; group < groupCount; group++) {
    // Collect this group's programs, keyed by the sub-blob they belong to.
    int32_t highestBlob = -1;
    for (auto const& program : programs) {
      if (static_cast<size_t>(program.groupIndex) != group) continue;
      if (program.blobIndex < 0) {
        store.message = "a program carries a negative blob index";
        return store;
      }
      highestBlob = std::max(highestBlob, program.blobIndex);
    }
    if (highestBlob < 0) continue;  // a platform with no programs keeps an empty group

    for (int32_t blobIndex = 0; blobIndex <= highestBlob; blobIndex++) {
      std::vector<ShaderSubProgram const*> members;
      for (auto const& program : programs) {
        if (static_cast<size_t>(program.groupIndex) == group && program.blobIndex == blobIndex) {
          members.push_back(&program);
        }
      }
      if (members.empty()) {
        store.message = "sub-blob " + std::to_string(blobIndex) + " of platform group " +
                        std::to_string(group) + " has no programs, so the store would have a hole";
        return store;
      }
      std::stable_sort(members.begin(), members.end(),
                       [](ShaderSubProgram const* a, ShaderSubProgram const* b) {
                         return a->programIndex < b->programIndex;
                       });

      // The entry width is whatever the sub-blob was read with, so a file that
      // used 12-byte entries keeps using them.
      size_t const entrySize = members.front()->entrySize == 12 ? 12u : 8u;

      std::vector<std::vector<uint8_t>> bodies;
      bodies.reserve(members.size());
      for (auto const* program : members) {
        std::vector<uint8_t> body;
        WriteSubProgram(*program, body);
        bodies.push_back(std::move(body));
      }

      size_t const headerSize = 4 + members.size() * entrySize;
      std::vector<uint8_t> plain;
      plain.reserve(headerSize);
      auto putU32 = [&plain](uint32_t value) {
        plain.push_back(static_cast<uint8_t>(value & 0xff));
        plain.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
        plain.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
        plain.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
      };
      putU32(static_cast<uint32_t>(members.size()));
      size_t cursor = headerSize;
      for (auto const& body : bodies) {
        putU32(static_cast<uint32_t>(cursor));
        putU32(static_cast<uint32_t>(body.size()));
        if (entrySize == 12) putU32(0);
        cursor += body.size();
      }
      for (auto const& body : bodies) plain.insert(plain.end(), body.begin(), body.end());

      std::vector<uint8_t> packed(Lz4CompressBound(plain.size()));
      size_t const written =
          Lz4CompressBlock(plain.data(), plain.size(), packed.data(), packed.size());
      if (written == 0) {
        store.message = "a sub-blob could not be compressed";
        return store;
      }

      store.offsets[group].push_back(static_cast<int32_t>(store.blob.size()));
      store.compressedLengths[group].push_back(static_cast<int32_t>(written));
      store.decompressedLengths[group].push_back(static_cast<int32_t>(plain.size()));
      store.blob.insert(store.blob.end(), packed.begin(), packed.begin() + static_cast<long>(written));
    }
  }

  store.ok = true;
  return store;
}

DecodeResult DecodeShaderPrograms(uint8_t const* data, size_t size, ShaderObject const& shader) {
  DecodeResult result;
  if (data == nullptr) { result.message = "no data"; return result; }
  if (!shader.blobPresent) { result.message = "shader carries no compressedBlob"; return result; }
  if (shader.blobFileOffset > size || shader.blobSize > size - shader.blobFileOffset) {
    result.message = "compressedBlob range lies outside the file";
    return result;
  }
  if (shader.offsets.size() != shader.compressedLengths.size() ||
      shader.offsets.size() != shader.decompressedLengths.size()) {
    result.message = "offsets, compressedLengths and decompressedLengths disagree";
    return result;
  }

  uint8_t const* const blob = data + shader.blobFileOffset;
  size_t const blobSize = shader.blobSize;

  int decoded = 0;
  int failed = 0;
  for (size_t group = 0; group < shader.offsets.size(); group++) {
    // A platform per group is the 2019.3+ layout. A bundle with a single flat
    // group and several platforms cannot say which is which, so it is reported
    // as unknown rather than guessed at.
    int32_t const platform = group < shader.platforms.size() ? shader.platforms[group]
                             : shader.platforms.size() == 1 ? shader.platforms[0]
                                                            : -1;
    auto const& groupOffsets = shader.offsets[group];
    auto const& groupCompressed = shader.compressedLengths[group];
    auto const& groupDecompressed = shader.decompressedLengths[group];
    if (groupOffsets.size() != groupCompressed.size() ||
        groupOffsets.size() != groupDecompressed.size()) {
      failed++;
      continue;
    }

    for (size_t entry = 0; entry < groupOffsets.size(); entry++) {
      int32_t const offset = groupOffsets[entry];
      int32_t const compressed = groupCompressed[entry];
      int32_t const decompressedSize = groupDecompressed[entry];
      if (offset < 0 || compressed <= 0 || decompressedSize <= 0) { failed++; continue; }
      if (static_cast<size_t>(offset) > blobSize ||
          static_cast<size_t>(compressed) > blobSize - static_cast<size_t>(offset)) {
        failed++;
        continue;
      }
      // A decompressed size a bundle claims is an allocation this code is about
      // to make on a headset, so it is capped rather than trusted.
      if (decompressedSize > 64 * 1024 * 1024) { failed++; continue; }

      std::vector<uint8_t> plain(static_cast<size_t>(decompressedSize));
      size_t const written = Lz4DecodeBlock(blob + offset, static_cast<size_t>(compressed),
                                            plain.data(), plain.size());
      if (written == 0) { failed++; continue; }
      plain.resize(written);

      std::vector<std::pair<uint32_t, uint32_t>> table;
      int32_t entryWidth = 8;
      if (!ReadProgramTable(plain.data(), plain.size(), table, entryWidth)) { failed++; continue; }

      for (size_t i = 0; i < table.size(); i++) {
        ShaderSubProgram program;
        program.platform = platform;
        program.groupIndex = static_cast<int32_t>(group);
        program.blobIndex = static_cast<int32_t>(entry);
        program.programIndex = static_cast<int32_t>(i);
        program.entrySize = entryWidth;
        if (!ReadSubProgram(plain.data() + table[i].first, table[i].second, program)) {
          failed++;
          continue;
        }
        result.programs.push_back(std::move(program));
        decoded++;
      }
    }
  }

  // Nothing decoded and nothing failed means the shader genuinely carries no
  // programs, which is not an error. Only a shader whose sub-blobs were all
  // unreadable is a failure.
  result.ok = failed == 0 || decoded > 0;
  if (!result.ok && result.message.empty()) result.message = "no programs could be decoded";
  if (failed > 0) {
    result.message = "decoded " + std::to_string(decoded) + " program(s); " +
                     std::to_string(failed) + " sub-blob(s) could not be read";
  }
  return result;
}

bool ShaderPlatformRunsOnQuest(int32_t platform) {
  // Quest's Unity player is built against GLES3 and/or Vulkan; nothing else in
  // the list can be handed to an Adreno driver.
  return platform == kShaderPlatformGLES3Plus || platform == kShaderPlatformVulkan;
}

namespace {

// Reads a SerializedFile's header, type table and object table.
//
// This is the one place that knows the file's shape. Both InspectSerializedFile
// (which reads shaders out of it) and RewriteSerializedFile (which rebuilds it
// with new object bodies) go through here, so the two can never drift apart on
// where a field is.
bool ParseLayout(uint8_t const* data, size_t size, Layout& layout) {
  if (data == nullptr || size < 32) return false;

  Reader reader(data, size);
  layout.metadataSize = reader.u32be();
  layout.fileSize = reader.u32be();
  layout.version = reader.u32be();
  layout.dataOffset = reader.u32be();
  if (!reader.ok() || layout.version < 8 || layout.version > 100) return false;

  layout.bigEndian = true;
  if (layout.version >= 9) {
    layout.bigEndian = reader.u8() != 0;
    reader.skip(3);
  }
  if (layout.version >= 22) {
    layout.metadataSize = reader.u32be();
    layout.fileSize = reader.u64be();
    layout.dataOffset = reader.u64be();
    reader.skip(8);  // reserved
  }
  if (!reader.ok()) return false;
  layout.headerLength = reader.position();

  // Same consistency test the converter uses to tell a serialized file apart
  // from a raw .resS payload node that happens to sit in the same archive.
  if (layout.fileSize == 0 || layout.fileSize > size) return false;
  if (layout.dataOffset == 0 || layout.dataOffset > layout.fileSize) return false;
  if (layout.metadataSize == 0 || layout.metadataSize > layout.fileSize) return false;

  reader.setBigEndian(layout.bigEndian);
  layout.isSerializedFile = true;

  if (layout.version >= 7) {
    layout.unityVersion = reader.cstring(64);
    if (!reader.ok() || layout.unityVersion.empty()) {
      layout.message = "unreadable Unity version string";
      return false;
    }
    // This string goes straight into a log line, and a corrupted or hostile
    // bundle can put arbitrary bytes here. Keep it to printable ASCII.
    for (char& c : layout.unityVersion) {
      if (c < 0x20 || c > 0x7e) c = '?';
    }
  }
  if (layout.version >= 8) reader.skip(4);  // m_TargetPlatform

  bool enableTypeTree = true;
  if (layout.version >= 13) enableTypeTree = reader.u8() != 0;
  layout.typeTreePresent = enableTypeTree;

  int32_t const typeCount = static_cast<int32_t>(reader.u32());
  if (!reader.ok() || typeCount < 0 || static_cast<uint32_t>(typeCount) > size) {
    layout.message = "implausible type count";
    return false;
  }

  layout.types.reserve(static_cast<size_t>(typeCount));
  for (int32_t i = 0; i < typeCount && reader.ok(); i++) {
    SerializedTypeInfo type;
    type.classID = static_cast<int32_t>(reader.u32());
    if (layout.version >= 16) reader.u8();                       // isStrippedType
    if (layout.version >= 17) reader.u16();                      // scriptTypeIndex
    if (layout.version >= 13) {
      bool const hasScriptHash = (layout.version < 16 && type.classID < 0) ||
                                 (layout.version >= 16 && type.classID == 114);
      if (hasScriptHash) reader.skip(16);
      reader.skip(16);                                           // old type hash
    }
    if (enableTypeTree) {
      int32_t const nodeCount = static_cast<int32_t>(reader.u32());
      int32_t const stringBufferSize = static_cast<int32_t>(reader.u32());
      if (!reader.ok() || nodeCount < 0 || stringBufferSize < 0 ||
          static_cast<uint32_t>(nodeCount) > size || static_cast<uint32_t>(stringBufferSize) > size) {
        layout.message = "implausible type tree size";
        return false;
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
        if (layout.version >= 19) reader.skip(8);  // ref type hash
        type.nodes.push_back(node);
      }
      if (!reader.ok()) { layout.message = "truncated type tree"; return false; }
      type.stringBuffer.resize(static_cast<size_t>(stringBufferSize));
      if (stringBufferSize > 0) {
        reader.raw(reinterpret_cast<uint8_t*>(type.stringBuffer.data()),
                   static_cast<size_t>(stringBufferSize));
      }
      if (layout.version >= 21) {
        int32_t const dependencyCount = static_cast<int32_t>(reader.u32());
        if (reader.ok() && dependencyCount > 0 && static_cast<uint32_t>(dependencyCount) < size) {
          reader.skip(static_cast<size_t>(dependencyCount) * 4);
        }
      }
    }
    layout.types.push_back(std::move(type));
  }
  if (!reader.ok()) { layout.message = "truncated type table"; return false; }

  int32_t const objectCount = static_cast<int32_t>(reader.u32());
  if (!reader.ok() || objectCount < 0 || static_cast<uint32_t>(objectCount) > size) {
    layout.message = "implausible object count";
    return false;
  }

  layout.objects.reserve(static_cast<size_t>(objectCount));
  for (int32_t i = 0; i < objectCount && reader.ok(); i++) {
    if (layout.version >= 14) reader.align4();
    ObjectInfo object;
    object.pathID = static_cast<int64_t>(reader.u64());
    object.byteStartFieldOffset = reader.position();
    object.byteStart = layout.version >= 22 ? static_cast<int64_t>(reader.u64())
                                            : static_cast<int64_t>(reader.u32());
    object.byteSizeFieldOffset = reader.position();
    object.byteSize = reader.u32();
    object.typeIndex = static_cast<int32_t>(reader.u32());
    layout.objects.push_back(object);
  }
  if (!reader.ok()) { layout.message = "truncated object table"; return false; }

  layout.parsed = true;
  return true;
}

}  // namespace

FileReport InspectSerializedFile(uint8_t const* data, size_t size) {
  FileReport report;
  Layout layout;
  bool const ok = ParseLayout(data, size, layout);
  report.isSerializedFile = layout.isSerializedFile;
  report.typeTreePresent = layout.typeTreePresent;
  report.unityVersion = layout.unityVersion;
  report.message = layout.message;
  if (!ok) return report;

  report.parsed = true;
  report.objectCount = static_cast<int32_t>(layout.objects.size());
  for (auto const& object : layout.objects) {
    if (object.typeIndex < 0 || static_cast<size_t>(object.typeIndex) >= layout.types.size()) continue;
    SerializedTypeInfo const& type = layout.types[static_cast<size_t>(object.typeIndex)];
    bool const isShader = type.classID == kClassIdShader;
    bool const isTexture = type.classID == kClassIdTexture2D;
    if (!isShader && !isTexture) continue;
    if (isShader) report.shaderObjectCount++;
    if (isTexture) report.textureObjectCount++;
    if (!layout.typeTreePresent || type.nodes.empty()) continue;

    uint64_t const start = static_cast<uint64_t>(object.byteStart) + layout.dataOffset;
    if (start >= size || object.byteSize > size - start) continue;
    if (isShader) {
      ShaderObject shader =
          ReadShaderObject(data + start, object.byteSize, static_cast<size_t>(start), type);
      shader.pathID = object.pathID;
      report.shaders.push_back(std::move(shader));
    } else {
      TextureObject texture =
          ReadTextureObject(data + start, object.byteSize, static_cast<size_t>(start), type);
      texture.pathID = object.pathID;
      report.textures.push_back(std::move(texture));
    }
  }

  if (!layout.typeTreePresent && report.shaderObjectCount > 0) {
    report.message = "type tree stripped: shader objects found but their contents cannot be decoded";
  }
  return report;
}

RewriteResult RewriteSerializedFile(uint8_t const* data, size_t size,
                                    std::vector<ObjectEdit> const& edits) {
  RewriteResult result;
  Layout layout;
  if (!ParseLayout(data, size, layout)) {
    result.message = layout.message.empty() ? "not a serialized file" : layout.message;
    return result;
  }

  // The metadata is copied verbatim, padding included, and only the object
  // table's byteStart/byteSize fields are patched. That is the whole trick:
  // nothing about the type tree, externals, script types or user information
  // changes when an object's *body* changes, so none of it has to be rebuilt --
  // and anything this parser does not understand survives untouched.
  if (layout.dataOffset > size) {
    result.message = "data offset lies outside the file";
    return result;
  }
  std::vector<uint8_t> head(data, data + layout.dataOffset);

  // Objects are laid out in the order they appear in the file, not the order
  // they appear in the table, so that an unedited file rebuilds to the same
  // bytes it started as.
  std::vector<size_t> order(layout.objects.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&layout](size_t a, size_t b) {
    return layout.objects[a].byteStart < layout.objects[b].byteStart;
  });

  // The data region is rebuilt rather than copied, but everything in it that is
  // not an object body is carried over untouched: the gap that preceded each
  // object, and anything past the last one. Two reasons, and the second is the
  // one that matters.
  //
  // With nothing edited, preserving the gaps puts every object back at the
  // offset it already had, so the rewrite reproduces its input byte for byte --
  // which is the only property that can be checked before there is a real
  // shader to write.
  //
  // And a file is not required to be nothing but objects. A header this parser
  // reads but whose object table is empty or unfamiliar would otherwise rebuild
  // to an empty data region, quietly discarding the entire payload. Bytes this
  // code does not understand are kept, not dropped.
  uint64_t const regionSize = layout.fileSize - layout.dataOffset;
  uint8_t const* const region = data + layout.dataOffset;

  std::vector<uint8_t> body;
  int replaced = 0;
  uint64_t previousEnd = 0;
  for (size_t index : order) {
    ObjectInfo& object = layout.objects[index];
    if (object.byteStart < 0 || static_cast<uint64_t>(object.byteStart) < previousEnd) {
      result.message = "objects overlap or run backwards in the data region";
      return result;
    }
    if (static_cast<uint64_t>(object.byteStart) > regionSize ||
        object.byteSize > regionSize - static_cast<uint64_t>(object.byteStart)) {
      result.message = "an object's body lies outside the file";
      return result;
    }

    ObjectEdit const* edit = nullptr;
    for (auto const& candidate : edits) {
      if (candidate.pathID == object.pathID) { edit = &candidate; break; }
    }

    uint64_t const gapStart = previousEnd;
    uint64_t const gap = static_cast<uint64_t>(object.byteStart) - previousEnd;
    body.insert(body.end(), region + gapStart, region + gapStart + gap);
    // Unity puts object bodies on 8-byte boundaries. With the gaps preserved
    // this is already satisfied for an unedited file, so it only bites once a
    // resize has moved something.
    while (body.size() % 8 != 0) body.push_back(0);

    previousEnd = static_cast<uint64_t>(object.byteStart) + object.byteSize;

    uint8_t const* sourceBytes = region + object.byteStart;
    size_t sourceSize = object.byteSize;
    if (edit != nullptr) {
      sourceBytes = edit->body.data();
      sourceSize = edit->body.size();
      replaced++;
    }

    object.byteStart = static_cast<int64_t>(body.size());
    object.byteSize = static_cast<uint32_t>(sourceSize);
    if (sourceSize > 0) body.insert(body.end(), sourceBytes, sourceBytes + sourceSize);
  }

  if (previousEnd < regionSize) {
    body.insert(body.end(), region + previousEnd, region + regionSize);
  }

  if (replaced != static_cast<int>(edits.size())) {
    result.message = "an edit named a pathID this file does not contain";
    return result;
  }

  auto writeField = [&head, &layout](size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; i++) {
      size_t const shift = layout.bigEndian ? 8 * (width - 1 - i) : 8 * i;
      head[offset + i] = static_cast<uint8_t>(value >> shift);
    }
  };

  size_t const startWidth = layout.version >= 22 ? 8 : 4;
  for (auto const& object : layout.objects) {
    if (object.byteStartFieldOffset + startWidth > head.size() ||
        object.byteSizeFieldOffset + 4 > head.size()) {
      result.message = "object table extends past the metadata";
      return result;
    }
    // Before version 22 a byteStart is a 32-bit field. Growing a file past 4GB
    // is not something to discover by silently truncating an offset.
    if (startWidth == 4 && static_cast<uint64_t>(object.byteStart) > 0xffffffffull) {
      result.message = "rewritten file needs 64-bit object offsets, which this file version lacks";
      return result;
    }
    writeField(object.byteStartFieldOffset, static_cast<uint64_t>(object.byteStart), startWidth);
    writeField(object.byteSizeFieldOffset, object.byteSize, 4);
  }

  // The header's size fields are big-endian regardless of the file's own
  // endianness flag, and only the total changes: the metadata kept its length,
  // so the data offset is the same as it was.
  uint64_t const total = layout.dataOffset + body.size();
  auto writeBE = [&head](size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; i++) {
      head[offset + i] = static_cast<uint8_t>(value >> (8 * (width - 1 - i)));
    }
  };
  if (layout.version >= 22) {
    if (head.size() < 48) { result.message = "truncated header"; return result; }
    writeBE(24, total, 8);
  } else {
    if (head.size() < 20) { result.message = "truncated header"; return result; }
    if (total > 0xffffffffull) {
      result.message = "rewritten file exceeds the 32-bit size field of this file version";
      return result;
    }
    writeBE(4, total, 4);
  }

  result.data = std::move(head);
  result.data.insert(result.data.end(), body.begin(), body.end());
  result.ok = true;
  return result;
}

}  // namespace SerializedFileParse
