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

// What a walk of one named field should bring back.
//
// A field is described by the type tree, not by this code, so the same capture
// serves `platforms` (vector<int>, one group), `offsets` (vector<vector<int>>,
// one group per inner array) and `compressedBlob` (vector<UInt8>, a byte range).
// Nothing here needs to know which of those it is looking at.
struct FieldCapture {
  bool wantBytes = false;

  std::vector<std::vector<int32_t>> groups;  // one per int array encountered
  bool sawBytes = false;
  size_t byteOffset = 0;  // relative to the start of the object body
  size_t byteCount = 0;

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
        reader.skip(static_cast<size_t>(bytes));
      } else if (capture != nullptr && !capture->wantBytes && element.byteSize == 4) {
        std::vector<int32_t> group;
        group.reserve(count);
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
  auto captureInts = [&](Reader& reader, size_t child, std::vector<std::vector<int32_t>>& into) {
    FieldCapture capture;
    WalkNode(reader, type, child, &capture, 1);
    into = std::move(capture.groups);
  };

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
    } else if (name == "offsets") {
      captureInts(reader, child, shader.offsets);
    } else if (name == "compressedLengths") {
      captureInts(reader, child, shader.compressedLengths);
    } else if (name == "decompressedLengths") {
      captureInts(reader, child, shader.decompressedLengths);
    } else if (name == "compressedBlob") {
      FieldCapture capture;
      capture.wantBytes = true;
      WalkNode(reader, type, child, &capture, 1);
      if (capture.sawBytes) {
        shader.blobPresent = true;
        shader.blobFileOffset = fileOffset + capture.byteOffset;
        shader.blobSize = capture.byteCount;
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
                      std::vector<std::pair<uint32_t, uint32_t>>& out) {
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
  reader.skip(12);
  if (out.blobVersion >= 201608170) reader.skip(4);
  if (!reader.ok()) return false;

  uint32_t const keywordCount = reader.u32();
  if (!reader.ok() || keywordCount > reader.remaining() / 4) return false;
  for (uint32_t i = 0; i < keywordCount; i++) {
    uint32_t const length = reader.u32();
    if (!reader.ok() || length > reader.remaining()) return false;
    std::string keyword(length, '\0');
    if (length > 0 && !reader.raw(reinterpret_cast<uint8_t*>(keyword.data()), length)) return false;
    reader.align4();
    out.keywords.push_back(std::move(keyword));
  }

  // 2018.06 through 2020.12 wrote a second, local keyword table in the same
  // shape immediately after the first.
  if (out.blobVersion >= 201806140 && out.blobVersion < 202012090) {
    uint32_t const localCount = reader.u32();
    if (!reader.ok() || localCount > reader.remaining() / 4) return false;
    for (uint32_t i = 0; i < localCount; i++) {
      uint32_t const length = reader.u32();
      if (!reader.ok() || length > reader.remaining()) return false;
      std::string keyword(length, '\0');
      if (length > 0 && !reader.raw(reinterpret_cast<uint8_t*>(keyword.data()), length)) return false;
      reader.align4();
      out.keywords.push_back(std::move(keyword));
    }
  }

  uint32_t const codeLength = reader.u32();
  if (!reader.ok() || codeLength > reader.remaining()) return false;
  out.code.resize(codeLength);
  if (codeLength > 0 && !reader.raw(out.code.data(), codeLength)) return false;
  return true;
}

}  // namespace

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
      if (!ReadProgramTable(plain.data(), plain.size(), table)) { failed++; continue; }

      for (size_t i = 0; i < table.size(); i++) {
        ShaderSubProgram program;
        program.platform = platform;
        program.blobIndex = static_cast<int32_t>(entry);
        program.programIndex = static_cast<int32_t>(i);
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
    report.shaders.push_back(
        ReadShaderObject(data + start, object.byteSize, static_cast<size_t>(start), type));
  }

  if (!enableTypeTree && report.shaderObjectCount > 0) {
    report.message = "type tree stripped: shader objects found but their contents cannot be decoded";
  }
  return report;
}

}  // namespace SerializedFileParse
