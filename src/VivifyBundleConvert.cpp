#include "VivifyBundleConvert.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Vivify::BundleConvert {

namespace {

// ---------------------------------------------------------------------------
// Small endian-aware readers over an in-memory buffer.
// ---------------------------------------------------------------------------

class ByteReader {
 public:
  ByteReader(uint8_t const* data, size_t size) : _data(data), _size(size) {}

  bool ok() const { return _ok; }
  size_t position() const { return _pos; }
  size_t remaining() const { return _pos <= _size ? _size - _pos : 0; }

  void seek(size_t pos) {
    if (pos > _size) {
      _ok = false;
      return;
    }
    _pos = pos;
  }

  void align(size_t alignment) {
    if (alignment == 0) return;
    size_t const rem = _pos % alignment;
    if (rem != 0) skip(alignment - rem);
  }

  void skip(size_t count) {
    if (count > remaining()) {
      _ok = false;
      _pos = _size;
      return;
    }
    _pos += count;
  }

  uint8_t u8() {
    if (remaining() < 1) {
      _ok = false;
      return 0;
    }
    return _data[_pos++];
  }

  uint16_t u16be() {
    uint8_t b[2];
    if (!raw(b, 2)) return 0;
    return static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
  }

  uint32_t u32be() {
    uint8_t b[4];
    if (!raw(b, 4)) return 0;
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  }

  uint64_t u64be() {
    uint8_t b[8];
    if (!raw(b, 8)) return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | b[i];
    return value;
  }

  // Reads a NUL-terminated string. A missing terminator is a parse failure
  // rather than a silent truncation.
  std::string cstring(size_t maxLength = 4096) {
    std::string out;
    while (out.size() <= maxLength) {
      if (remaining() < 1) {
        _ok = false;
        return out;
      }
      uint8_t const c = _data[_pos++];
      if (c == 0) return out;
      out.push_back(static_cast<char>(c));
    }
    _ok = false;
    return out;
  }

  bool raw(void* dest, size_t count) {
    if (count > remaining()) {
      _ok = false;
      _pos = _size;
      return false;
    }
    std::memcpy(dest, _data + _pos, count);
    _pos += count;
    return true;
  }

 private:
  uint8_t const* _data;
  size_t _size;
  size_t _pos = 0;
  bool _ok = true;
};

uint32_t ReadU32(uint8_t const* p, bool bigEndian) {
  if (bigEndian) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  }
  return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[0]);
}

void WriteU32(uint8_t* p, uint32_t value, bool bigEndian) {
  if (bigEndian) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
  } else {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
  }
}

void AppendU16BE(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void AppendU32BE(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void AppendU64BE(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void AppendCString(std::vector<uint8_t>& out, std::string const& value) {
  out.insert(out.end(), value.begin(), value.end());
  out.push_back(0);
}

// ---------------------------------------------------------------------------
// LZ4 block decompression (covers both LZ4 and LZ4HC -- same block format).
// ---------------------------------------------------------------------------

bool Lz4DecompressBlock(uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
  size_t sp = 0;
  size_t dp = 0;
  while (sp < srcSize) {
    uint8_t const token = src[sp++];

    size_t literalLength = token >> 4;
    if (literalLength == 15) {
      for (;;) {
        if (sp >= srcSize) return false;
        uint8_t const add = src[sp++];
        literalLength += add;
        if (add != 255) break;
        if (literalLength > dstSize) return false;
      }
    }
    if (literalLength > srcSize - sp) return false;
    if (literalLength > dstSize - dp) return false;
    std::memcpy(dst + dp, src + sp, literalLength);
    sp += literalLength;
    dp += literalLength;

    // The last sequence in a block is literals only and stops here.
    if (sp == srcSize) break;
    if (srcSize - sp < 2) return false;

    size_t const offset = static_cast<size_t>(src[sp]) | (static_cast<size_t>(src[sp + 1]) << 8);
    sp += 2;
    if (offset == 0 || offset > dp) return false;

    size_t matchLength = token & 0x0F;
    if (matchLength == 15) {
      for (;;) {
        if (sp >= srcSize) return false;
        uint8_t const add = src[sp++];
        matchLength += add;
        if (add != 255) break;
        if (matchLength > dstSize) return false;
      }
    }
    matchLength += 4;
    if (matchLength > dstSize - dp) return false;

    // Matches may overlap the bytes they produce, so this must stay a byte
    // copy rather than a memcpy/memmove.
    size_t matchPos = dp - offset;
    for (size_t i = 0; i < matchLength; i++) {
      dst[dp++] = dst[matchPos++];
    }
  }
  return dp == dstSize;
}

// ---------------------------------------------------------------------------
// LZMA1 decompression, following the reference LZMA specification decoder.
// Unity stores 5 property bytes followed by the raw stream, with the
// uncompressed size known from the archive tables rather than from the stream.
// ---------------------------------------------------------------------------

class LzmaDecoder {
 public:
  // props: the 5-byte LZMA property header (lclppb byte + 4-byte dictionary size).
  bool Decode(uint8_t const* props, uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    uint32_t d = props[0];
    if (d >= 9 * 5 * 5) return false;
    _lc = static_cast<unsigned>(d % 9);
    d /= 9;
    _lp = static_cast<unsigned>(d % 5);
    _pb = static_cast<unsigned>(d / 5);

    _src = src;
    _srcSize = srcSize;
    _srcPos = 0;
    _dst = dst;
    _dstSize = dstSize;
    _dstPos = 0;

    if (!RangeInit()) return false;

    _litProbs.assign(static_cast<size_t>(0x300) << (_lc + _lp), kProbInit);
    _isMatch.assign(kNumStates << kNumPosBitsMax, kProbInit);
    _isRep.assign(kNumStates, kProbInit);
    _isRepG0.assign(kNumStates, kProbInit);
    _isRepG1.assign(kNumStates, kProbInit);
    _isRepG2.assign(kNumStates, kProbInit);
    _isRep0Long.assign(kNumStates << kNumPosBitsMax, kProbInit);
    _posSlot.assign(static_cast<size_t>(kNumLenToPosStates) * (1u << 6), kProbInit);
    _specPos.assign(kNumFullDistances, kProbInit);
    _align.assign(1u << kNumAlignBits, kProbInit);
    _lenDecoder.Reset();
    _repLenDecoder.Reset();

    unsigned state = 0;
    uint32_t rep0 = 0;
    uint32_t rep1 = 0;
    uint32_t rep2 = 0;
    uint32_t rep3 = 0;

    while (_dstPos < _dstSize) {
      unsigned const posState = static_cast<unsigned>(_dstPos) & ((1u << _pb) - 1);
      if (DecodeBit(_isMatch[(state << kNumPosBitsMax) + posState]) == 0) {
        if (!DecodeLiteral(state, rep0)) return false;
        state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
        continue;
      }

      uint32_t len;
      if (DecodeBit(_isRep[state]) != 0) {
        if (_dstPos == 0) return false;
        if (static_cast<uint64_t>(rep0) >= _dstPos) return false;
        if (DecodeBit(_isRepG0[state]) == 0) {
          if (DecodeBit(_isRep0Long[(state << kNumPosBitsMax) + posState]) == 0) {
            state = state < 7 ? 9 : 11;
            _dst[_dstPos] = _dst[_dstPos - rep0 - 1];
            _dstPos++;
            continue;
          }
        } else {
          uint32_t dist;
          if (DecodeBit(_isRepG1[state]) == 0) {
            dist = rep1;
          } else {
            if (DecodeBit(_isRepG2[state]) == 0) {
              dist = rep2;
            } else {
              dist = rep3;
              rep3 = rep2;
            }
            rep2 = rep1;
          }
          rep1 = rep0;
          rep0 = dist;
        }
        if (static_cast<uint64_t>(rep0) >= _dstPos) return false;
        len = _repLenDecoder.Decode(*this, posState);
        state = state < 7 ? 8 : 11;
      } else {
        rep3 = rep2;
        rep2 = rep1;
        rep1 = rep0;
        len = _lenDecoder.Decode(*this, posState);
        state = state < 7 ? 7 : 10;
        rep0 = DecodeDistance(len);
        if (rep0 == 0xFFFFFFFFu) {
          // End-of-stream marker. Only valid if we already produced everything.
          return _dstPos == _dstSize;
        }
        if (static_cast<uint64_t>(rep0) >= _dstPos) return false;
      }

      len += kMatchMinLen;
      if (len > _dstSize - _dstPos) return false;
      size_t matchPos = _dstPos - rep0 - 1;
      for (uint32_t i = 0; i < len; i++) {
        _dst[_dstPos++] = _dst[matchPos++];
      }
      if (_corrupted) return false;
    }
    return !_corrupted && _dstPos == _dstSize;
  }

 private:
  static constexpr unsigned kNumBitModelTotalBits = 11;
  static constexpr unsigned kNumMoveBits = 5;
  static constexpr uint16_t kProbInit = (1u << kNumBitModelTotalBits) / 2;
  static constexpr unsigned kNumPosBitsMax = 4;
  static constexpr unsigned kNumStates = 12;
  static constexpr unsigned kNumLenToPosStates = 4;
  static constexpr unsigned kNumAlignBits = 4;
  static constexpr unsigned kEndPosModelIndex = 14;
  static constexpr unsigned kNumFullDistances = 1u << (kEndPosModelIndex >> 1);
  static constexpr uint32_t kMatchMinLen = 2;
  static constexpr uint32_t kTopValue = 1u << 24;

  uint8_t NextByte() {
    if (_srcPos >= _srcSize) {
      _corrupted = true;
      return 0;
    }
    return _src[_srcPos++];
  }

  bool RangeInit() {
    if (_srcSize < 5) return false;
    if (NextByte() != 0) return false;
    _code = 0;
    _range = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) _code = (_code << 8) | NextByte();
    return _code != _range;
  }

  void Normalize() {
    if (_range < kTopValue) {
      _range <<= 8;
      _code = (_code << 8) | NextByte();
    }
  }

  unsigned DecodeBit(uint16_t& prob) {
    uint32_t const bound = (_range >> kNumBitModelTotalBits) * prob;
    unsigned symbol;
    if (_code < bound) {
      prob = static_cast<uint16_t>(prob + (((1u << kNumBitModelTotalBits) - prob) >> kNumMoveBits));
      _range = bound;
      symbol = 0;
    } else {
      prob = static_cast<uint16_t>(prob - (prob >> kNumMoveBits));
      _code -= bound;
      _range -= bound;
      symbol = 1;
    }
    Normalize();
    return symbol;
  }

  uint32_t DecodeDirectBits(unsigned count) {
    uint32_t result = 0;
    do {
      _range >>= 1;
      _code -= _range;
      uint32_t const t = 0u - (_code >> 31);
      _code += _range & t;
      if (_code == _range) _corrupted = true;
      Normalize();
      result <<= 1;
      result += t + 1;
    } while (--count);
    return result;
  }

  uint32_t BitTreeDecode(uint16_t* probs, unsigned numBits) {
    uint32_t m = 1;
    for (unsigned i = 0; i < numBits; i++) m = (m << 1) + DecodeBit(probs[m]);
    return m - (1u << numBits);
  }

  uint32_t BitTreeReverseDecode(uint16_t* probs, unsigned numBits) {
    uint32_t m = 1;
    uint32_t symbol = 0;
    for (unsigned i = 0; i < numBits; i++) {
      unsigned const bit = DecodeBit(probs[m]);
      m = (m << 1) + bit;
      symbol |= static_cast<uint32_t>(bit) << i;
    }
    return symbol;
  }

  struct LenDecoder {
    uint16_t choice = kProbInit;
    uint16_t choice2 = kProbInit;
    uint16_t low[1u << kNumPosBitsMax][8];
    uint16_t mid[1u << kNumPosBitsMax][8];
    uint16_t high[256];

    void Reset() {
      choice = kProbInit;
      choice2 = kProbInit;
      for (auto& row : low) std::fill(std::begin(row), std::end(row), kProbInit);
      for (auto& row : mid) std::fill(std::begin(row), std::end(row), kProbInit);
      std::fill(std::begin(high), std::end(high), kProbInit);
    }

    uint32_t Decode(LzmaDecoder& rc, unsigned posState) {
      if (rc.DecodeBit(choice) == 0) return rc.BitTreeDecode(low[posState], 3);
      if (rc.DecodeBit(choice2) == 0) return 8 + rc.BitTreeDecode(mid[posState], 3);
      return 16 + rc.BitTreeDecode(high, 8);
    }
  };

  bool DecodeLiteral(unsigned state, uint32_t rep0) {
    if (_dstPos >= _dstSize) return false;
    unsigned prevByte = _dstPos > 0 ? _dst[_dstPos - 1] : 0;
    size_t const litState =
        ((_dstPos & ((1u << _lp) - 1)) << _lc) + (prevByte >> (8 - _lc));
    uint16_t* probs = _litProbs.data() + (static_cast<size_t>(0x300) * litState);

    unsigned symbol = 1;
    if (state >= 7) {
      if (static_cast<uint64_t>(rep0) + 1 > _dstPos) return false;
      unsigned matchByte = _dst[_dstPos - rep0 - 1];
      do {
        unsigned const matchBit = (matchByte >> 7) & 1;
        matchByte = static_cast<unsigned>(matchByte << 1) & 0xFF;
        unsigned const bit = DecodeBit(probs[((1 + matchBit) << 8) + symbol]);
        symbol = (symbol << 1) | bit;
        if (matchBit != bit) break;
      } while (symbol < 0x100);
    }
    while (symbol < 0x100) symbol = (symbol << 1) | DecodeBit(probs[symbol]);
    _dst[_dstPos++] = static_cast<uint8_t>(symbol);
    return !_corrupted;
  }

  uint32_t DecodeDistance(uint32_t len) {
    unsigned lenState = len < kNumLenToPosStates ? static_cast<unsigned>(len) : kNumLenToPosStates - 1;
    uint32_t const posSlot = BitTreeDecode(_posSlot.data() + (static_cast<size_t>(lenState) << 6), 6);
    if (posSlot < 4) return posSlot;

    unsigned const numDirectBits = static_cast<unsigned>((posSlot >> 1) - 1);
    uint32_t dist = (2 | (posSlot & 1)) << numDirectBits;
    if (posSlot < kEndPosModelIndex) {
      dist += BitTreeReverseDecode(_specPos.data() + dist - posSlot, numDirectBits);
    } else {
      dist += DecodeDirectBits(numDirectBits - kNumAlignBits) << kNumAlignBits;
      dist += BitTreeReverseDecode(_align.data(), kNumAlignBits);
    }
    return dist;
  }

  uint8_t const* _src = nullptr;
  size_t _srcSize = 0;
  size_t _srcPos = 0;
  uint8_t* _dst = nullptr;
  size_t _dstSize = 0;
  size_t _dstPos = 0;

  uint32_t _range = 0;
  uint32_t _code = 0;
  bool _corrupted = false;

  unsigned _lc = 0;
  unsigned _lp = 0;
  unsigned _pb = 0;

  std::vector<uint16_t> _litProbs;
  std::vector<uint16_t> _isMatch;
  std::vector<uint16_t> _isRep;
  std::vector<uint16_t> _isRepG0;
  std::vector<uint16_t> _isRepG1;
  std::vector<uint16_t> _isRepG2;
  std::vector<uint16_t> _isRep0Long;
  std::vector<uint16_t> _posSlot;
  std::vector<uint16_t> _specPos;
  std::vector<uint16_t> _align;
  LenDecoder _lenDecoder;
  LenDecoder _repLenDecoder;
};

// ---------------------------------------------------------------------------
// UnityFS archive structures.
// ---------------------------------------------------------------------------

// Archive-level flag bits (UnityFS header "flags" field).
constexpr uint32_t kArchiveCompressionMask = 0x3F;
constexpr uint32_t kArchiveBlocksAndDirectoryInfoCombined = 0x40;
constexpr uint32_t kArchiveBlocksInfoAtTheEnd = 0x80;
constexpr uint32_t kArchiveBlockInfoNeedPaddingAtStart = 0x200;

// Compression ids shared by the archive header and per-block flags.
constexpr uint32_t kCompressionNone = 0;
constexpr uint32_t kCompressionLzma = 1;
constexpr uint32_t kCompressionLz4 = 2;
constexpr uint32_t kCompressionLz4Hc = 3;
constexpr uint32_t kCompressionLzham = 4;

// Blocks we emit when repacking. Matches Unity's own chunk size.
constexpr uint32_t kOutputBlockSize = 128 * 1024;

// A bundle bigger than this is refused rather than risking an allocation that
// takes the game down. Vivify bundles are a few hundred MB at the very most.
constexpr uint64_t kMaxUncompressedBytes = 2ull * 1024 * 1024 * 1024;

struct StorageBlock {
  uint32_t uncompressedSize = 0;
  uint32_t compressedSize = 0;
  uint16_t flags = 0;
};

struct DirectoryNode {
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t flags = 0;
  std::string path;
};

struct ArchiveHeader {
  std::string signature;
  uint32_t version = 0;
  std::string unityVersion;
  std::string unityRevision;
  uint64_t size = 0;
  uint32_t compressedBlocksInfoSize = 0;
  uint32_t uncompressedBlocksInfoSize = 0;
  uint32_t flags = 0;
  // Offset of the first storage block within the file.
  uint64_t blocksStart = 0;
};

bool Decompress(uint32_t compression, uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstSize,
                Status& outStatus) {
  switch (compression) {
    case kCompressionNone:
      if (srcSize != dstSize) {
        outStatus = Status::Corrupt;
        return false;
      }
      std::memcpy(dst, src, dstSize);
      return true;
    case kCompressionLz4:
    case kCompressionLz4Hc:
      if (!Lz4DecompressBlock(src, srcSize, dst, dstSize)) {
        outStatus = Status::Corrupt;
        return false;
      }
      return true;
    case kCompressionLzma: {
      if (srcSize < 5) {
        outStatus = Status::Corrupt;
        return false;
      }
      LzmaDecoder decoder;
      if (!decoder.Decode(src, src + 5, srcSize - 5, dst, dstSize)) {
        outStatus = Status::Corrupt;
        return false;
      }
      return true;
    }
    case kCompressionLzham:
    default:
      // LZHAM is legal in the format but Unity has not produced it for years,
      // and shipping a decoder for it would be dead weight.
      outStatus = Status::UnsupportedCompression;
      return false;
  }
}

// Seeking reader over the source bundle. Blocks are pulled in one at a time so
// the compressed archive is never resident alongside the expanded one -- on a
// large bundle that halves peak memory, which matters when this runs on device
// next to the game.
class FileSource {
 public:
  bool Open(std::string const& path, Result& result) {
    std::error_code ec;
    auto const fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
      result.status = Status::SourceUnreadable;
      result.message = "could not stat '" + path + "'";
      return false;
    }
    _stream.open(path, std::ios::binary);
    if (!_stream.is_open()) {
      result.status = Status::SourceUnreadable;
      result.message = "could not open '" + path + "'";
      return false;
    }
    _size = static_cast<uint64_t>(fileSize);
    return true;
  }

  uint64_t size() const { return _size; }

  bool ReadAt(uint64_t offset, size_t count, std::vector<uint8_t>& out) {
    if (offset > _size || _size - offset < count) return false;
    out.resize(count);
    if (count == 0) return true;
    _stream.clear();
    _stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!_stream) return false;
    return static_cast<bool>(_stream.read(reinterpret_cast<char*>(out.data()),
                                          static_cast<std::streamsize>(count)));
  }

 private:
  std::ifstream _stream;
  uint64_t _size = 0;
};

// Largest fixed archive header we will ever need to inspect: signature, version,
// two version strings, three sizes and the flags, plus the version-7 alignment.
constexpr size_t kMaxHeaderScan = 512;

// Parses the archive header and the (possibly compressed) blocks/directory
// table. Leaves header.blocksStart pointing at the first storage block.
bool ParseArchive(FileSource& source, ArchiveHeader& header, std::vector<StorageBlock>& blocks,
                  std::vector<DirectoryNode>& nodes, Result& result) {
  std::vector<uint8_t> head;
  if (!source.ReadAt(0, static_cast<size_t>(std::min<uint64_t>(source.size(), kMaxHeaderScan)), head)) {
    result.status = Status::SourceUnreadable;
    result.message = "could not read the archive header";
    return false;
  }
  ByteReader reader(head.data(), head.size());
  header.signature = reader.cstring(32);
  if (!reader.ok()) {
    result.status = Status::NotAUnityBundle;
    result.message = "file does not start with a Unity archive signature";
    return false;
  }
  if (header.signature != "UnityFS") {
    // The signature came from an arbitrary file, so scrub it before it reaches
    // a log line.
    std::string printable;
    for (char c : header.signature) {
      printable.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    result.status = Status::NotAUnityBundle;
    result.message = "unsupported archive signature '" + printable + "' (only UnityFS is handled)";
    return false;
  }
  header.version = reader.u32be();
  header.unityVersion = reader.cstring(64);
  header.unityRevision = reader.cstring(64);
  header.size = reader.u64be();
  header.compressedBlocksInfoSize = reader.u32be();
  header.uncompressedBlocksInfoSize = reader.u32be();
  header.flags = reader.u32be();
  if (!reader.ok()) {
    result.status = Status::Corrupt;
    result.message = "truncated UnityFS header";
    return false;
  }
  if (header.version >= 7) reader.align(16);

  if (header.uncompressedBlocksInfoSize == 0 || header.uncompressedBlocksInfoSize > kMaxUncompressedBytes) {
    result.status = Status::Corrupt;
    result.message = "implausible blocks-info size";
    return false;
  }

  // Position just past the fixed header (and its version-7 padding) -- where
  // the blocks-info sits in the common layout, and where the storage blocks
  // start when the table is stored at the end instead.
  uint64_t const afterHeader = reader.position();

  uint64_t blocksInfoOffset;
  if ((header.flags & kArchiveBlocksInfoAtTheEnd) != 0) {
    if (source.size() < header.compressedBlocksInfoSize) {
      result.status = Status::Corrupt;
      result.message = "blocks-info claims to sit past the end of the file";
      return false;
    }
    blocksInfoOffset = source.size() - header.compressedBlocksInfoSize;
    header.blocksStart = afterHeader;
  } else {
    blocksInfoOffset = afterHeader;
    header.blocksStart = afterHeader + header.compressedBlocksInfoSize;
  }
  if ((header.flags & kArchiveBlockInfoNeedPaddingAtStart) != 0) {
    header.blocksStart += (16 - (header.blocksStart % 16)) % 16;
  }

  std::vector<uint8_t> blocksInfoRaw;
  if (!source.ReadAt(blocksInfoOffset, header.compressedBlocksInfoSize, blocksInfoRaw)) {
    result.status = Status::Corrupt;
    result.message = "blocks-info is out of bounds";
    return false;
  }

  std::vector<uint8_t> blocksInfo(header.uncompressedBlocksInfoSize);
  Status failure = Status::Corrupt;
  if (!Decompress(header.flags & kArchiveCompressionMask, blocksInfoRaw.data(), blocksInfoRaw.size(),
                  blocksInfo.data(), blocksInfo.size(), failure)) {
    result.status = failure;
    result.message = failure == Status::UnsupportedCompression
                         ? "blocks-info uses compression id " +
                               std::to_string(header.flags & kArchiveCompressionMask) + ", which is not supported"
                         : "blocks-info failed to decompress";
    return false;
  }

  ByteReader info(blocksInfo.data(), blocksInfo.size());
  info.skip(16);  // uncompressed data hash
  uint32_t const blockCount = info.u32be();
  if (!info.ok() || blockCount > (1u << 22)) {
    result.status = Status::Corrupt;
    result.message = "implausible storage block count";
    return false;
  }
  blocks.resize(blockCount);
  for (uint32_t i = 0; i < blockCount; i++) {
    blocks[i].uncompressedSize = info.u32be();
    blocks[i].compressedSize = info.u32be();
    blocks[i].flags = info.u16be();
  }
  uint32_t const nodeCount = info.u32be();
  if (!info.ok() || nodeCount > (1u << 20)) {
    result.status = Status::Corrupt;
    result.message = "implausible directory node count";
    return false;
  }
  nodes.resize(nodeCount);
  for (uint32_t i = 0; i < nodeCount; i++) {
    nodes[i].offset = info.u64be();
    nodes[i].size = info.u64be();
    nodes[i].flags = info.u32be();
    nodes[i].path = info.cstring();
  }
  if (!info.ok()) {
    result.status = Status::Corrupt;
    result.message = "truncated blocks/directory table";
    return false;
  }
  return true;
}

bool ReadBlocks(FileSource& source, ArchiveHeader const& header,
                std::vector<StorageBlock> const& blocks, std::vector<uint8_t>& out, Result& result) {
  uint64_t total = 0;
  for (auto const& block : blocks) total += block.uncompressedSize;
  if (total == 0 || total > kMaxUncompressedBytes) {
    result.status = total == 0 ? Status::Corrupt : Status::OutOfMemory;
    result.message = total == 0 ? "archive contains no data" : "uncompressed archive exceeds the 2 GiB limit";
    return false;
  }
  out.resize(static_cast<size_t>(total));

  uint64_t srcOffset = header.blocksStart;
  size_t dstOffset = 0;
  std::vector<uint8_t> compressed;
  for (auto const& block : blocks) {
    if (!source.ReadAt(srcOffset, block.compressedSize, compressed)) {
      result.status = Status::Corrupt;
      result.message = "storage block runs past the end of the file";
      return false;
    }
    Status failure = Status::Corrupt;
    if (!Decompress(block.flags & kArchiveCompressionMask, compressed.data(), compressed.size(),
                    out.data() + dstOffset, block.uncompressedSize, failure)) {
      result.status = failure;
      result.message = failure == Status::UnsupportedCompression
                           ? "a storage block uses compression id " +
                                 std::to_string(block.flags & kArchiveCompressionMask) + ", which is not supported"
                           : "a storage block failed to decompress";
      return false;
    }
    srcOffset += block.compressedSize;
    dstOffset += block.uncompressedSize;
  }
  return true;
}

// ---------------------------------------------------------------------------
// SerializedFile target-platform patching.
// ---------------------------------------------------------------------------

struct TargetPlatformField {
  bool found = false;
  size_t offset = 0;   // absolute offset of the int32 within `data`
  bool bigEndian = false;
  int32_t value = 0;
};

// Locates m_TargetPlatform in the SerializedFile that starts at `base`.
// Layout per the SerializedFile header:
//   u32be metadataSize, u32be fileSize, u32be version, u32be dataOffset
//   [version >= 9]  u8 endianness + 3 reserved bytes
//   [version >= 22] u32be metadataSize, u64be fileSize, u64be dataOffset, u64be unknown
//   [version >= 7]  cstring unityVersion
//   [version >= 8]  int32 m_TargetPlatform      <- in the file's own endianness
TargetPlatformField FindTargetPlatform(uint8_t const* data, size_t dataSize, size_t base, uint64_t nodeSize) {
  TargetPlatformField field;
  if (base >= dataSize) return field;
  size_t const available = std::min<uint64_t>(nodeSize, dataSize - base);
  if (available < 32) return field;

  ByteReader reader(data + base, static_cast<size_t>(available));
  uint64_t metadataSize = reader.u32be();
  uint64_t fileSize = reader.u32be();
  uint32_t const version = reader.u32be();
  uint64_t dataOffset = reader.u32be();
  if (!reader.ok() || version < 8 || version > 100) return field;

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
  if (!reader.ok()) return field;

  // Bundles also carry raw payload nodes (.resS/.resource streams). Those are
  // not serialized files and must not be "patched"; a serialized file always
  // describes its own extent consistently, which raw data effectively never
  // does by chance.
  if (fileSize == 0 || fileSize > nodeSize) return field;
  if (dataOffset == 0 || dataOffset > fileSize) return field;
  if (metadataSize == 0 || metadataSize > fileSize) return field;
  if (version >= 7) {
    std::string const unityVersion = reader.cstring(64);
    if (!reader.ok() || unityVersion.empty()) return field;
  }
  size_t const fieldOffset = reader.position();
  if (!reader.ok() || available - fieldOffset < 4) return field;

  field.found = true;
  field.offset = base + fieldOffset;
  field.bigEndian = bigEndian;
  field.value = static_cast<int32_t>(ReadU32(data + field.offset, bigEndian));
  return field;
}

// ---------------------------------------------------------------------------
// Repacking.
// ---------------------------------------------------------------------------

bool WriteConverted(std::string const& destPath, ArchiveHeader const& header,
                    std::vector<DirectoryNode> const& nodes, std::vector<uint8_t> const& data, Result& result) {
  // Blocks/directory table for the uncompressed output.
  std::vector<uint8_t> blocksInfo;
  blocksInfo.insert(blocksInfo.end(), 16, 0);  // hash: Unity does not verify this for uncompressed archives

  uint64_t remaining = data.size();
  uint32_t blockCount = static_cast<uint32_t>((remaining + kOutputBlockSize - 1) / kOutputBlockSize);
  AppendU32BE(blocksInfo, blockCount);
  while (remaining > 0) {
    uint32_t const chunk = static_cast<uint32_t>(std::min<uint64_t>(remaining, kOutputBlockSize));
    AppendU32BE(blocksInfo, chunk);  // uncompressed size
    AppendU32BE(blocksInfo, chunk);  // compressed size (identical: stored)
    AppendU16BE(blocksInfo, static_cast<uint16_t>(kCompressionNone));
    remaining -= chunk;
  }

  AppendU32BE(blocksInfo, static_cast<uint32_t>(nodes.size()));
  for (auto const& node : nodes) {
    AppendU64BE(blocksInfo, node.offset);
    AppendU64BE(blocksInfo, node.size);
    AppendU32BE(blocksInfo, node.flags);
    AppendCString(blocksInfo, node.path);
  }

  // Header. Compression is cleared, the table is stored up front, and no
  // padding flag is set, so the reader path stays the simplest one Unity has.
  uint32_t const outFlags = kArchiveBlocksAndDirectoryInfoCombined;

  std::vector<uint8_t> out;
  AppendCString(out, "UnityFS");
  AppendU32BE(out, header.version);
  AppendCString(out, header.unityVersion);
  AppendCString(out, header.unityRevision);
  size_t const sizeFieldOffset = out.size();
  AppendU64BE(out, 0);  // patched below once the total is known
  AppendU32BE(out, static_cast<uint32_t>(blocksInfo.size()));
  AppendU32BE(out, static_cast<uint32_t>(blocksInfo.size()));
  AppendU32BE(out, outFlags);
  if (header.version >= 7) {
    while (out.size() % 16 != 0) out.push_back(0);
  }
  out.insert(out.end(), blocksInfo.begin(), blocksInfo.end());

  uint64_t const totalSize = static_cast<uint64_t>(out.size()) + data.size();
  for (int i = 0; i < 8; i++) {
    out[sizeFieldOffset + i] = static_cast<uint8_t>(totalSize >> (56 - 8 * i));
  }

  std::error_code ec;
  auto const parent = std::filesystem::path(destPath).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);

  // Write to a scratch name and rename into place, so a conversion that is cut
  // short (the game is closed, the device runs out of space) can never leave a
  // truncated file that a later run would treat as a finished, cached bundle.
  std::string const tempPath = destPath + ".part";
  ec.clear();
  std::filesystem::remove(tempPath, ec);

  {
    std::ofstream os(tempPath, std::ios::binary | std::ios::trunc);
    if (!os.is_open()) {
      result.status = Status::DestUnwritable;
      result.message = "could not open '" + tempPath + "' for writing";
      return false;
    }
    os.write(reinterpret_cast<char const*>(out.data()), static_cast<std::streamsize>(out.size()));
    os.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    os.flush();
    if (!os.good()) {
      os.close();
      ec.clear();
      std::filesystem::remove(tempPath, ec);
      result.status = Status::DestUnwritable;
      result.message = "write to '" + tempPath + "' failed (out of space?)";
      return false;
    }
  }

  ec.clear();
  std::filesystem::rename(tempPath, destPath, ec);
  if (ec) {
    ec.clear();
    std::filesystem::remove(tempPath, ec);
    result.status = Status::DestUnwritable;
    result.message = "could not move the converted bundle into '" + destPath + "'";
    return false;
  }
  result.outputBytes = totalSize;
  return true;
}

// Shared front half of ConvertToAndroid/NeedsAndroidConversion: read, unpack,
// and locate every SerializedFile's target-platform field.
bool LoadAndScan(std::string const& sourcePath, ArchiveHeader& header, std::vector<DirectoryNode>& nodes,
                 std::vector<uint8_t>& data, std::vector<TargetPlatformField>& fields, Result& result) {
  FileSource source;
  if (!source.Open(sourcePath, result)) return false;

  std::vector<StorageBlock> blocks;
  if (!ParseArchive(source, header, blocks, nodes, result)) return false;
  if (!ReadBlocks(source, header, blocks, data, result)) return false;

  for (auto const& node : nodes) {
    if (node.offset > data.size() || data.size() - node.offset < node.size) continue;
    auto field = FindTargetPlatform(data.data(), data.size(), static_cast<size_t>(node.offset), node.size);
    if (field.found) fields.push_back(field);
  }
  result.serializedFilesSeen = static_cast<int>(fields.size());
  if (fields.empty()) {
    result.status = Status::Corrupt;
    result.message = "no SerializedFile headers found inside the archive";
    return false;
  }
  for (auto const& field : fields) {
    if (field.value != kBuildTargetAndroid) {
      result.sourcePlatform = BuildTargetName(field.value);
      break;
    }
  }
  if (result.sourcePlatform.empty()) result.sourcePlatform = BuildTargetName(kBuildTargetAndroid);
  return true;
}

}  // namespace

std::string_view StatusText(Status status) {
  switch (status) {
    case Status::Success: return "converted";
    case Status::AlreadyAndroid: return "already an Android bundle";
    case Status::SourceUnreadable: return "source bundle unreadable";
    case Status::NotAUnityBundle: return "not a UnityFS asset bundle";
    case Status::UnsupportedCompression: return "unsupported bundle compression";
    case Status::Corrupt: return "bundle structure could not be parsed";
    case Status::DestUnwritable: return "could not write the converted bundle";
    case Status::OutOfMemory: return "bundle too large to convert on device";
  }
  return "unknown";
}

std::string BuildTargetName(int32_t target) {
  switch (target) {
    case kBuildTargetStandaloneOSX: return "StandaloneOSX";
    case kBuildTargetStandaloneWindows: return "StandaloneWindows";
    case kBuildTargetiOS: return "iOS";
    case kBuildTargetAndroid: return "Android";
    case kBuildTargetStandaloneWindows64: return "StandaloneWindows64";
    case kBuildTargetWebGL: return "WebGL";
    case kBuildTargetStandaloneLinux64: return "StandaloneLinux64";
    default: return "BuildTarget(" + std::to_string(target) + ")";
  }
}

bool IsUnityBundleFile(std::string const& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  char signature[8] = {};
  if (!in.read(signature, sizeof(signature))) return false;
  return std::memcmp(signature, "UnityFS\0", sizeof(signature)) == 0;
}

Result ConvertToAndroid(std::string const& sourcePath, std::string const& destPath) {
  Result result;
  ArchiveHeader header;
  std::vector<DirectoryNode> nodes;
  std::vector<uint8_t> data;
  std::vector<TargetPlatformField> fields;
  if (!LoadAndScan(sourcePath, header, nodes, data, fields, result)) return result;

  for (auto const& field : fields) {
    if (field.value == kBuildTargetAndroid) continue;
    WriteU32(data.data() + field.offset, static_cast<uint32_t>(kBuildTargetAndroid), field.bigEndian);
    result.serializedFilesRetargeted++;
  }
  if (result.serializedFilesRetargeted == 0) {
    result.status = Status::AlreadyAndroid;
    result.message = "every SerializedFile already targets Android";
    return result;
  }

  if (!WriteConverted(destPath, header, nodes, data, result)) return result;

  result.status = Status::Success;
  result.message = "retargeted " + std::to_string(result.serializedFilesRetargeted) + " of " +
                   std::to_string(result.serializedFilesSeen) + " serialized file(s) from " +
                   result.sourcePlatform + " to Android";
  return result;
}

}
