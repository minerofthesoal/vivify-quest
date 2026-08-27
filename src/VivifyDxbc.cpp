#include "VivifyDxbc.hpp"

#include <algorithm>
#include <climits>
#include <deque>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>

namespace Vivify::Dxbc {
namespace {

// A cursor over a byte range that cannot be walked off the end.
//
// Every field in a DXBC container is a length or an offset read out of the
// container itself, and this parser is handed whatever a downloaded map put in
// its bundle. So there is no "trusted" region: the reader is the only thing
// standing between a malformed chunk table and a read past the buffer.
class Reader {
 public:
  Reader(uint8_t const* data, size_t size) : _data(data), _size(size) {}

  bool Ok() const { return _ok; }
  size_t Position() const { return _position; }
  size_t Size() const { return _size; }
  size_t Remaining() const { return _position <= _size ? _size - _position : 0; }

  void Seek(size_t position) {
    if (position > _size) {
      _ok = false;
      return;
    }
    _position = position;
  }

  void Skip(size_t bytes) {
    if (bytes > Remaining()) {
      _ok = false;
      return;
    }
    _position += bytes;
  }

  uint32_t U32() {
    if (!_ok || Remaining() < 4) {
      _ok = false;
      return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, _data + _position, 4);
    _position += 4;
    return value;
  }

  uint16_t U16() {
    if (!_ok || Remaining() < 2) {
      _ok = false;
      return 0;
    }
    uint16_t value = 0;
    std::memcpy(&value, _data + _position, 2);
    _position += 2;
    return value;
  }

  uint8_t U8() {
    if (!_ok || Remaining() < 1) {
      _ok = false;
      return 0;
    }
    return _data[_position++];
  }

  // A NUL-terminated string at an absolute offset, without moving the cursor.
  // A string whose terminator is missing is a truncated chunk, not a string
  // that runs to the end of the buffer.
  std::string StringAt(size_t offset) const {
    if (offset >= _size) return {};
    size_t end = offset;
    while (end < _size && _data[end] != 0) end++;
    if (end >= _size) return {};
    return std::string(reinterpret_cast<char const*>(_data + offset), end - offset);
  }

 private:
  uint8_t const* _data;
  size_t _size;
  size_t _position = 0;
  bool _ok = true;
};

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

constexpr uint32_t kFourCCDxbc = FourCC('D', 'X', 'B', 'C');
constexpr uint32_t kChunkRdef = FourCC('R', 'D', 'E', 'F');
constexpr uint32_t kChunkIsgn = FourCC('I', 'S', 'G', 'N');
constexpr uint32_t kChunkIsg1 = FourCC('I', 'S', 'G', '1');
constexpr uint32_t kChunkOsgn = FourCC('O', 'S', 'G', 'N');
constexpr uint32_t kChunkOsg1 = FourCC('O', 'S', 'G', '1');
constexpr uint32_t kChunkOsg5 = FourCC('O', 'S', 'G', '5');
constexpr uint32_t kChunkPcsg = FourCC('P', 'C', 'S', 'G');
constexpr uint32_t kChunkShdr = FourCC('S', 'H', 'D', 'R');
constexpr uint32_t kChunkShex = FourCC('S', 'H', 'E', 'X');

// Instruction opcode token, D3D10_SB_OPCODE_TYPE. Only the numbers matter to
// the decoder; the names exist so a failure can say which instruction it was.
enum : uint32_t {
  OP_ADD = 0, OP_AND = 1, OP_BREAK = 2, OP_BREAKC = 3, OP_CALL = 4, OP_CALLC = 5,
  OP_CASE = 6, OP_CONTINUE = 7, OP_CONTINUEC = 8, OP_CUT = 9, OP_DEFAULT = 10,
  OP_DERIV_RTX = 11, OP_DERIV_RTY = 12, OP_DISCARD = 13, OP_DIV = 14, OP_DP2 = 15,
  OP_DP3 = 16, OP_DP4 = 17, OP_ELSE = 18, OP_EMIT = 19, OP_EMITTHENCUT = 20,
  OP_ENDIF = 21, OP_ENDLOOP = 22, OP_ENDSWITCH = 23, OP_EQ = 24, OP_EXP = 25,
  OP_FRC = 26, OP_FTOI = 27, OP_FTOU = 28, OP_GE = 29, OP_IADD = 30, OP_IF = 31,
  OP_IEQ = 32, OP_IGE = 33, OP_ILT = 34, OP_IMAD = 35, OP_IMAX = 36, OP_IMIN = 37,
  OP_IMUL = 38, OP_INE = 39, OP_INEG = 40, OP_ISHL = 41, OP_ISHR = 42, OP_ITOF = 43,
  OP_LABEL = 44, OP_LD = 45, OP_LD_MS = 46, OP_LOG = 47, OP_LOOP = 48, OP_LT = 49,
  OP_MAD = 50, OP_MIN = 51, OP_MAX = 52, OP_CUSTOMDATA = 53, OP_MOV = 54,
  OP_MOVC = 55, OP_MUL = 56, OP_NE = 57, OP_NOP = 58, OP_NOT = 59, OP_OR = 60,
  OP_RESINFO = 61, OP_RET = 62, OP_RETC = 63, OP_ROUND_NE = 64, OP_ROUND_NI = 65,
  OP_ROUND_PI = 66, OP_ROUND_Z = 67, OP_RSQ = 68, OP_SAMPLE = 69, OP_SAMPLE_C = 70,
  OP_SAMPLE_C_LZ = 71, OP_SAMPLE_L = 72, OP_SAMPLE_D = 73, OP_SAMPLE_B = 74,
  OP_SQRT = 75, OP_SWITCH = 76, OP_SINCOS = 77, OP_UDIV = 78, OP_ULT = 79,
  OP_UGE = 80, OP_UMUL = 81, OP_UMAD = 82, OP_UMAX = 83, OP_UMIN = 84,
  OP_USHR = 85, OP_UTOF = 86, OP_XOR = 87,
  OP_DCL_RESOURCE = 88, OP_DCL_CONSTANT_BUFFER = 89, OP_DCL_SAMPLER = 90,
  OP_DCL_INDEX_RANGE = 91, OP_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY = 92,
  OP_DCL_GS_INPUT_PRIMITIVE = 93, OP_DCL_MAX_OUTPUT_VERTEX_COUNT = 94,
  OP_DCL_INPUT = 95, OP_DCL_INPUT_SGV = 96, OP_DCL_INPUT_SIV = 97,
  OP_DCL_INPUT_PS = 98, OP_DCL_INPUT_PS_SGV = 99, OP_DCL_INPUT_PS_SIV = 100,
  OP_DCL_OUTPUT = 101, OP_DCL_OUTPUT_SGV = 102, OP_DCL_OUTPUT_SIV = 103,
  OP_DCL_TEMPS = 104, OP_DCL_INDEXABLE_TEMP = 105, OP_DCL_GLOBAL_FLAGS = 106,
  OP_RESERVED0 = 107, OP_LOD = 108, OP_GATHER4 = 109, OP_SAMPLE_POS = 110,
  OP_SAMPLE_INFO = 111, OP_RESERVED1 = 112,
  OP_HS_DECLS = 113, OP_HS_CONTROL_POINT_PHASE = 114, OP_HS_FORK_PHASE = 115,
  OP_HS_JOIN_PHASE = 116, OP_EMIT_STREAM = 117, OP_CUT_STREAM = 118,
  OP_EMITTHENCUT_STREAM = 119, OP_INTERFACE_CALL = 120, OP_BUFINFO = 121,
  OP_DERIV_RTX_COARSE = 122, OP_DERIV_RTX_FINE = 123, OP_DERIV_RTY_COARSE = 124,
  OP_DERIV_RTY_FINE = 125, OP_GATHER4_C = 126, OP_GATHER4_PO = 127,
  OP_GATHER4_PO_C = 128, OP_RCP = 129, OP_F32TOF16 = 130, OP_F16TOF32 = 131,
  OP_UADDC = 132, OP_USUBB = 133, OP_COUNTBITS = 134, OP_FIRSTBIT_HI = 135,
  OP_FIRSTBIT_LO = 136, OP_FIRSTBIT_SHI = 137, OP_UBFE = 138, OP_IBFE = 139,
  OP_BFI = 140, OP_BFREV = 141, OP_SWAPC = 142,
  OP_DCL_STREAM = 143, OP_DCL_FUNCTION_BODY = 144, OP_DCL_FUNCTION_TABLE = 145,
  OP_DCL_INTERFACE = 146, OP_DCL_INPUT_CONTROL_POINT_COUNT = 147,
  OP_DCL_OUTPUT_CONTROL_POINT_COUNT = 148, OP_DCL_TESS_DOMAIN = 149,
  OP_DCL_TESS_PARTITIONING = 150, OP_DCL_TESS_OUTPUT_PRIMITIVE = 151,
  OP_DCL_HS_MAX_TESSFACTOR = 152, OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT = 153,
  OP_DCL_HS_JOIN_PHASE_INSTANCE_COUNT = 154, OP_DCL_THREAD_GROUP = 155,
  OP_DCL_UAV_TYPED = 156, OP_DCL_UAV_RAW = 157, OP_DCL_UAV_STRUCTURED = 158,
  OP_DCL_TGSM_RAW = 159, OP_DCL_TGSM_STRUCTURED = 160, OP_DCL_RESOURCE_RAW = 161,
  OP_DCL_RESOURCE_STRUCTURED = 162,
  OP_LD_UAV_TYPED = 163, OP_STORE_UAV_TYPED = 164, OP_LD_RAW = 165, OP_STORE_RAW = 166,
  OP_LD_STRUCTURED = 167, OP_STORE_STRUCTURED = 168,
  OP_ATOMIC_AND = 169, OP_ATOMIC_OR = 170, OP_ATOMIC_XOR = 171, OP_ATOMIC_CMP_STORE = 172,
  OP_ATOMIC_IADD = 173, OP_ATOMIC_IMAX = 174, OP_ATOMIC_IMIN = 175, OP_ATOMIC_UMAX = 176,
  OP_ATOMIC_UMIN = 177,
  OP_IMM_ATOMIC_ALLOC = 178, OP_IMM_ATOMIC_CONSUME = 179, OP_IMM_ATOMIC_IADD = 180,
  OP_IMM_ATOMIC_AND = 181, OP_IMM_ATOMIC_OR = 182, OP_IMM_ATOMIC_XOR = 183,
  OP_IMM_ATOMIC_EXCH = 184, OP_IMM_ATOMIC_CMP_EXCH = 185, OP_IMM_ATOMIC_IMAX = 186,
  OP_IMM_ATOMIC_IMIN = 187, OP_IMM_ATOMIC_UMAX = 188, OP_IMM_ATOMIC_UMIN = 189,
  OP_SYNC = 190,
  OP_DADD = 191, OP_DMAX = 192, OP_DMIN = 193, OP_DMUL = 194, OP_DEQ = 195, OP_DGE = 196,
  OP_DLT = 197, OP_DNE = 198, OP_DMOV = 199, OP_DMOVC = 200, OP_DTOF = 201, OP_FTOD = 202,
  OP_EVAL_SNAPPED = 203, OP_EVAL_SAMPLE_INDEX = 204, OP_EVAL_CENTROID = 205,
  OP_DCL_GS_INSTANCE_COUNT = 206, OP_ABORT = 207, OP_DEBUG_BREAK = 208, OP_RESERVED2 = 209,
  OP_DDIV = 210, OP_DFMA = 211, OP_DRCP = 212, OP_MSAD = 213, OP_DTOI = 214, OP_DTOU = 215,
  OP_ITOD = 216, OP_UTOD = 217,
  OP_COUNT = 218,
};

struct OpcodeEntry {
  char const* name;
  int8_t operandCount;  // -1: not in the table / decoded by length only
};

// name + operand count for every opcode the decoder walks. An operand count of
// -1 means "skip by the instruction length": the instruction is still traversed
// correctly, it just is not broken into operands, so the translator will report
// it as unsupported rather than silently mistranslating it.
OpcodeEntry const& OpcodeInfo(uint32_t opcode) {
  static OpcodeEntry const unknown{"unknown", -1};
  static OpcodeEntry const table[] = {
      {"add", 3},          {"and", 3},          {"break", 0},       {"breakc", 1},
      {"call", 1},         {"callc", 2},        {"case", 1},        {"continue", 0},
      {"continuec", 1},    {"cut", 0},          {"default", 0},     {"deriv_rtx", 2},
      {"deriv_rty", 2},    {"discard", 1},      {"div", 3},         {"dp2", 3},
      {"dp3", 3},          {"dp4", 3},          {"else", 0},        {"emit", 0},
      {"emitthencut", 0},  {"endif", 0},        {"endloop", 0},     {"endswitch", 0},
      {"eq", 3},           {"exp", 2},          {"frc", 2},         {"ftoi", 2},
      {"ftou", 2},         {"ge", 3},           {"iadd", 3},        {"if", 1},
      {"ieq", 3},          {"ige", 3},          {"ilt", 3},         {"imad", 4},
      {"imax", 3},         {"imin", 3},         {"imul", 4},        {"ine", 3},
      {"ineg", 2},         {"ishl", 3},         {"ishr", 3},        {"itof", 2},
      {"label", 1},        {"ld", 3},           {"ld_ms", 4},       {"log", 2},
      {"loop", 0},         {"lt", 3},           {"mad", 4},         {"min", 3},
      {"max", 3},          {"customdata", -1},  {"mov", 2},         {"movc", 4},
      {"mul", 3},          {"ne", 3},           {"nop", 0},         {"not", 2},
      {"or", 3},           {"resinfo", 3},      {"ret", 0},         {"retc", 1},
      {"round_ne", 2},     {"round_ni", 2},     {"round_pi", 2},    {"round_z", 2},
      {"rsq", 2},          {"sample", 4},       {"sample_c", 5},    {"sample_c_lz", 5},
      {"sample_l", 5},     {"sample_d", 6},     {"sample_b", 5},    {"sqrt", 2},
      {"switch", 1},       {"sincos", 3},       {"udiv", 4},        {"ult", 3},
      {"uge", 3},          {"umul", 4},         {"umad", 4},        {"umax", 3},
      {"umin", 3},         {"ushr", 3},         {"utof", 2},        {"xor", 3},
      {"dcl_resource", 1}, {"dcl_constantbuffer", 1}, {"dcl_sampler", 1},
      {"dcl_indexrange", 1}, {"dcl_gs_output_primitive_topology", 0},
      {"dcl_gs_input_primitive", 0}, {"dcl_maxout", 0},
      {"dcl_input", 1},    {"dcl_input_sgv", 1}, {"dcl_input_siv", 1},
      {"dcl_input_ps", 1}, {"dcl_input_ps_sgv", 1}, {"dcl_input_ps_siv", 1},
      {"dcl_output", 1},   {"dcl_output_sgv", 1}, {"dcl_output_siv", 1},
      {"dcl_temps", 0},    {"dcl_indexableTemp", 0}, {"dcl_globalFlags", 0},
      {"reserved0", -1},   {"lod", 4},          {"gather4", 4},     {"samplepos", 3},
      {"sampleinfo", 2},   {"reserved1", -1},
      {"hs_decls", 0},     {"hs_control_point_phase", 0}, {"hs_fork_phase", 0},
      {"hs_join_phase", 0}, {"emit_stream", 1}, {"cut_stream", 1},
      {"emitthencut_stream", 1}, {"interface_call", 1}, {"bufinfo", 2},
      {"deriv_rtx_coarse", 2}, {"deriv_rtx_fine", 2}, {"deriv_rty_coarse", 2},
      {"deriv_rty_fine", 2}, {"gather4_c", 5}, {"gather4_po", 5},
      {"gather4_po_c", 6}, {"rcp", 2},          {"f32tof16", 2},    {"f16tof32", 2},
      {"uaddc", 4},        {"usubb", 4},        {"countbits", 2},   {"firstbit_hi", 2},
      {"firstbit_lo", 2},  {"firstbit_shi", 2}, {"ubfe", 4},        {"ibfe", 4},
      {"bfi", 5},          {"bfrev", 2},        {"swapc", 5},
      {"dcl_stream", 1},   {"dcl_function_body", -1}, {"dcl_function_table", -1},
      {"dcl_interface", -1}, {"dcl_input_control_point_count", 0},
      {"dcl_output_control_point_count", 0}, {"dcl_tessdomain", 0},
      {"dcl_tesspartitioning", 0}, {"dcl_tessoutputprimitive", 0},
      {"dcl_hs_max_tessfactor", 0}, {"dcl_hs_fork_phase_instance_count", 0},
      {"dcl_hs_join_phase_instance_count", 0}, {"dcl_thread_group", 0},
      {"dcl_uav_typed", 1}, {"dcl_uav_raw", 1}, {"dcl_uav_structured", 1},
      {"dcl_tgsm_raw", 1}, {"dcl_tgsm_structured", 1}, {"dcl_resource_raw", 1},
      {"dcl_resource_structured", 1},
      {"ld_uav_typed", 3},        {"store_uav_typed", 3},   {"ld_raw", 3},
      {"store_raw", 3},           {"ld_structured", 4},     {"store_structured", 4},
      {"atomic_and", 3},          {"atomic_or", 3},         {"atomic_xor", 3},
      {"atomic_cmp_store", 4},    {"atomic_iadd", 3},       {"atomic_imax", 3},
      {"atomic_imin", 3},         {"atomic_umax", 3},       {"atomic_umin", 3},
      {"imm_atomic_alloc", 2},    {"imm_atomic_consume", 2}, {"imm_atomic_iadd", 4},
      {"imm_atomic_and", 4},      {"imm_atomic_or", 4},     {"imm_atomic_xor", 4},
      {"imm_atomic_exch", 4},     {"imm_atomic_cmp_exch", 5}, {"imm_atomic_imax", 4},
      {"imm_atomic_imin", 4},     {"imm_atomic_umax", 4},   {"imm_atomic_umin", 4},
      {"sync", 0},
      {"dadd", 3},                {"dmax", 3},              {"dmin", 3},
      {"dmul", 3},                {"deq", 3},               {"dge", 3},
      {"dlt", 3},                 {"dne", 3},               {"dmov", 2},
      {"dmovc", 4},               {"dtof", 2},              {"ftod", 2},
      {"eval_snapped", 3},        {"eval_sample_index", 3}, {"eval_centroid", 2},
      {"dcl_gs_instance_count", 0}, {"abort", 0},           {"debug_break", 0},
      {"reserved2", -1},
      {"ddiv", 3},                {"dfma", 4},              {"drcp", 2},
      {"msad", 4},                {"dtoi", 2},              {"dtou", 2},
      {"itod", 2},                {"utod", 2},
  };
  constexpr size_t tableSize = sizeof(table) / sizeof(table[0]);
  if (opcode >= tableSize) return unknown;
  return table[opcode];
}

}  // namespace

std::string_view StageName(Stage stage) {
  switch (stage) {
    case Stage::Pixel: return "pixel";
    case Stage::Vertex: return "vertex";
    case Stage::Geometry: return "geometry";
    case Stage::Hull: return "hull";
    case Stage::Domain: return "domain";
    case Stage::Compute: return "compute";
    default: return "unknown";
  }
}

std::string OpcodeName(uint32_t opcode) {
  auto const& entry = OpcodeInfo(opcode);
  if (std::string_view(entry.name) == "unknown") {
    return "unknown(" + std::to_string(opcode) + ")";
  }
  return entry.name;
}

bool LooksLikeDxbc(uint8_t const* data, size_t size) {
  if (data == nullptr || size < 32) return false;
  uint32_t magic = 0;
  std::memcpy(&magic, data, 4);
  if (magic != kFourCCDxbc) return false;
  uint32_t totalSize = 0;
  std::memcpy(&totalSize, data + 24, 4);
  // Unity stores the container with nothing after it, but a blob that claims to
  // be longer than the buffer it sits in is the corruption case this rejects.
  return totalSize <= size;
}

namespace {

// ---------------------------------------------------------------------------
// Signature chunks (ISGN / OSGN / OSG5 / PCSG)
// ---------------------------------------------------------------------------

bool ParseSignatureChunk(uint8_t const* chunk, size_t chunkSize, bool hasStreamField,
                         std::vector<SignatureElement>& out) {
  Reader reader(chunk, chunkSize);
  uint32_t const count = reader.U32();
  reader.U32();  // offset to the first element; always 8 in practice
  if (!reader.Ok()) return false;
  // An element is 24 bytes (28 with the stream field). Rejecting an impossible
  // count up front stops a corrupt header from asking for a huge reserve.
  size_t const elementSize = hasStreamField ? 28u : 24u;
  if (count > (chunkSize - 8) / elementSize) return false;

  out.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    SignatureElement element;
    if (hasStreamField) element.stream = reader.U32();
    uint32_t const nameOffset = reader.U32();
    element.semanticIndex = reader.U32();
    element.systemValueType = reader.U32();
    element.componentType = reader.U32();
    element.registerIndex = reader.U32();
    element.mask = reader.U8();
    element.rwMask = reader.U8();
    reader.U16();  // padding
    if (!reader.Ok()) return false;
    element.semanticName = reader.StringAt(nameOffset);
    out.push_back(std::move(element));
  }
  return true;
}

// ---------------------------------------------------------------------------
// RDEF chunk: constant buffers and bound resources
// ---------------------------------------------------------------------------

bool ParseRdefChunk(uint8_t const* chunk, size_t chunkSize, Program& program) {
  Reader reader(chunk, chunkSize);
  uint32_t const constantBufferCount = reader.U32();
  uint32_t const constantBufferOffset = reader.U32();
  uint32_t const resourceBindingCount = reader.U32();
  uint32_t const resourceBindingOffset = reader.U32();
  reader.U32();  // target version (shader model), duplicated in the version token
  reader.U32();  // flags
  uint32_t const creatorOffset = reader.U32();
  if (!reader.Ok()) return false;
  program.creator = reader.StringAt(creatorOffset);

  if (resourceBindingCount > chunkSize / 32) return false;
  program.resourceBindings.reserve(resourceBindingCount);
  reader.Seek(resourceBindingOffset);
  for (uint32_t i = 0; i < resourceBindingCount && reader.Ok(); i++) {
    ResourceBinding binding;
    uint32_t const nameOffset = reader.U32();
    binding.type = reader.U32();
    binding.returnType = reader.U32();
    binding.dimension = reader.U32();
    reader.U32();  // sample count
    binding.bindPoint = reader.U32();
    binding.bindCount = reader.U32();
    reader.U32();  // shader input flags
    if (!reader.Ok()) return false;
    binding.name = reader.StringAt(nameOffset);
    program.resourceBindings.push_back(std::move(binding));
  }

  if (constantBufferCount > chunkSize / 24) return false;
  program.constantBuffers.reserve(constantBufferCount);
  for (uint32_t i = 0; i < constantBufferCount; i++) {
    reader.Seek(static_cast<size_t>(constantBufferOffset) + static_cast<size_t>(i) * 24u);
    ConstantBufferInfo buffer;
    uint32_t const nameOffset = reader.U32();
    uint32_t const variableCount = reader.U32();
    uint32_t const variableOffset = reader.U32();
    buffer.size = reader.U32();
    reader.U32();  // flags
    reader.U32();  // buffer type
    if (!reader.Ok()) return false;
    buffer.name = reader.StringAt(nameOffset);

    if (variableCount > chunkSize / 24) return false;
    buffer.variables.reserve(variableCount);
    for (uint32_t v = 0; v < variableCount; v++) {
      // Shader model 5 grew the variable record from 24 to 40 bytes. The extra
      // fields are all trailing, so both are read the same way and the stride
      // is the only difference.
      size_t const stride = program.majorVersion >= 5 ? 40u : 24u;
      reader.Seek(static_cast<size_t>(variableOffset) + static_cast<size_t>(v) * stride);
      ConstantBufferVariable variable;
      uint32_t const variableNameOffset = reader.U32();
      variable.startOffset = reader.U32();
      variable.size = reader.U32();
      reader.U32();  // flags
      uint32_t const typeOffset = reader.U32();
      reader.U32();  // default value offset
      if (!reader.Ok()) return false;
      variable.name = reader.StringAt(variableNameOffset);

      // The type record says whether this is a float4, a float4x4 or an array
      // of them, which is what decides the GLSL declaration.
      Reader typeReader(chunk, chunkSize);
      typeReader.Seek(typeOffset);
      variable.variableClass = typeReader.U16();
      variable.variableType = typeReader.U16();
      variable.rows = typeReader.U16();
      variable.columns = typeReader.U16();
      variable.elements = typeReader.U16();
      if (!typeReader.Ok()) {
        variable.variableClass = 0;
        variable.variableType = 0;
        variable.rows = 0;
        variable.columns = 0;
        variable.elements = 0;
      }
      buffer.variables.push_back(std::move(variable));
    }
    program.constantBuffers.push_back(std::move(buffer));
  }

  // The binding of a constant buffer lives in the resource table, not in the
  // buffer record, so they are matched up by name here rather than at every use.
  for (auto& buffer : program.constantBuffers) {
    for (auto const& binding : program.resourceBindings) {
      if (binding.type == 0 && binding.name == buffer.name) {
        buffer.bindPoint = binding.bindPoint;
        break;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Bytecode: operands
// ---------------------------------------------------------------------------

constexpr uint32_t kIndexImmediate32 = 0;
constexpr uint32_t kIndexImmediate64 = 1;
constexpr uint32_t kIndexRelative = 2;
constexpr uint32_t kIndexImmediate32PlusRelative = 3;
constexpr uint32_t kIndexImmediate64PlusRelative = 4;

// Decodes one operand, recursing for the register-relative part of an index.
// `depth` stops a crafted stream from recursing without bound: real bytecode
// nests one level (cb0[r0.x]), never more.
bool DecodeOperand(Reader& reader, Operand& operand, int depth) {
  if (depth > 3) return false;
  uint32_t const token = reader.U32();
  if (!reader.Ok()) return false;

  uint32_t const componentSelection = token & 0x3u;
  operand.numComponents = componentSelection == 0 ? 0 : (componentSelection == 1 ? 1 : 4);
  operand.type = (token >> 12) & 0xffu;
  uint32_t const indexDimension = (token >> 20) & 0x3u;

  if (operand.numComponents == 4) {
    operand.selectionMode = (token >> 2) & 0x3u;
    uint32_t const selection = (token >> 4) & 0xffu;
    if (operand.selectionMode == 0) {
      operand.mask = static_cast<uint8_t>(selection & 0xfu);
    } else if (operand.selectionMode == 1) {
      for (int i = 0; i < 4; i++) {
        operand.swizzle[i] = static_cast<uint8_t>((selection >> (i * 2)) & 0x3u);
      }
      operand.mask = 0xf;
    } else {
      uint8_t const component = static_cast<uint8_t>(selection & 0x3u);
      for (int i = 0; i < 4; i++) operand.swizzle[i] = component;
      operand.mask = static_cast<uint8_t>(1u << component);
    }
  } else if (operand.numComponents == 1) {
    operand.selectionMode = 0;
    operand.mask = 0x1;
    for (int i = 0; i < 4; i++) operand.swizzle[i] = 0;
  } else {
    operand.selectionMode = 0;
    operand.mask = 0;
  }

  // Extended operand tokens carry the source modifier (neg/abs) and, in SM5,
  // the minimum-precision request. They chain: each sets bit 31 if another
  // follows.
  bool extended = (token & 0x80000000u) != 0;
  while (extended) {
    uint32_t const extendedToken = reader.U32();
    if (!reader.Ok()) return false;
    uint32_t const extendedType = extendedToken & 0x3fu;
    if (extendedType == 1) {  // D3D10_SB_EXTENDED_OPERAND_MODIFIER
      operand.modifier = static_cast<Modifier>((extendedToken >> 6) & 0xffu);
      operand.minPrecision = (extendedToken >> 14) & 0x7u;
    }
    extended = (extendedToken & 0x80000000u) != 0;
  }

  if (operand.type == kOperandImmediate32) {
    uint32_t const count = operand.numComponents == 1 ? 1u : 4u;
    operand.immediates.resize(count);
    for (uint32_t i = 0; i < count; i++) operand.immediates[i] = reader.U32();
    if (!reader.Ok()) return false;
    return true;
  }
  if (operand.type == kOperandImmediate64) {
    uint32_t const count = operand.numComponents == 1 ? 2u : 8u;
    for (uint32_t i = 0; i < count; i++) reader.U32();
    return reader.Ok();
  }

  operand.indices.resize(indexDimension);
  for (uint32_t i = 0; i < indexDimension; i++) {
    uint32_t const representation = (token >> (22 + i * 3)) & 0x7u;
    OperandIndex& index = operand.indices[i];
    switch (representation) {
      case kIndexImmediate32:
        index.immediate = reader.U32();
        index.hasImmediate = true;
        break;
      case kIndexImmediate64: {
        uint64_t const low = reader.U32();
        uint64_t const high = reader.U32();
        index.immediate = low | (high << 32);
        index.hasImmediate = true;
        break;
      }
      case kIndexRelative:
        index.hasRelative = true;
        break;
      case kIndexImmediate32PlusRelative:
        index.immediate = reader.U32();
        index.hasImmediate = true;
        index.hasRelative = true;
        break;
      case kIndexImmediate64PlusRelative: {
        uint64_t const low = reader.U32();
        uint64_t const high = reader.U32();
        index.immediate = low | (high << 32);
        index.hasImmediate = true;
        index.hasRelative = true;
        break;
      }
      default:
        return false;
    }
    if (!reader.Ok()) return false;
    if (index.hasRelative) {
      index.relative = std::make_shared<Operand>();
      if (!DecodeOperand(reader, *index.relative, depth + 1)) return false;
    }
  }
  return true;
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Bytecode: instructions
// ---------------------------------------------------------------------------

constexpr uint32_t kMaxInstructions = 1u << 20;

bool DecodeInstructions(uint8_t const* code, size_t codeSize, Program& program) {
  Reader reader(code, codeSize);
  uint32_t const versionToken = reader.U32();
  uint32_t const lengthDwords = reader.U32();
  if (!reader.Ok()) return false;

  program.minorVersion = versionToken & 0xfu;
  program.majorVersion = (versionToken >> 4) & 0xfu;
  uint32_t const programType = (versionToken >> 16) & 0xffffu;
  program.stage = programType <= 5 ? static_cast<Stage>(programType) : Stage::Unknown;

  // The declared length is the authority on where the stream ends -- a chunk
  // can be padded -- but it can also be a lie, so it is clamped to the chunk.
  size_t end = codeSize;
  if (lengthDwords >= 2 && static_cast<size_t>(lengthDwords) * 4u <= codeSize) {
    end = static_cast<size_t>(lengthDwords) * 4u;
  }

  while (reader.Position() + 4 <= end) {
    size_t const start = reader.Position();
    uint32_t const token = reader.U32();
    if (!reader.Ok()) return false;

    Instruction instruction;
    instruction.opcode = token & 0x7ffu;
    instruction.tokenOffset = static_cast<uint32_t>(start);

    if (instruction.opcode == OP_CUSTOMDATA) {
      // Custom data is the one instruction whose length is a full dword rather
      // than seven bits, because it carries a whole immediate constant buffer.
      uint32_t const customClass = (token >> 11) & 0x1fffffu;
      uint32_t const customLength = reader.U32();
      if (!reader.Ok() || customLength < 2) return false;
      size_t const customEnd = start + static_cast<size_t>(customLength) * 4u;
      if (customEnd > end || customEnd <= start) return false;
      if (customClass == 3) {  // D3D10_SB_CUSTOMDATA_DCL_IMMEDIATE_CONSTANT_BUFFER
        uint32_t const dwords = customLength - 2;
        program.immediateConstantBuffer.reserve(dwords);
        for (uint32_t i = 0; i < dwords; i++) {
          program.immediateConstantBuffer.push_back(reader.U32());
        }
        if (!reader.Ok()) return false;
      }
      reader.Seek(customEnd);
      if (!reader.Ok()) return false;
      continue;
    }

    instruction.controls = (token >> 11) & 0x1fffu;
    instruction.saturate = ((token >> 13) & 0x1u) != 0;
    instruction.extended = (token & 0x80000000u) != 0;
    instruction.lengthDwords = (token >> 24) & 0x7fu;
    if (instruction.lengthDwords == 0) return false;
    size_t const instructionEnd = start + static_cast<size_t>(instruction.lengthDwords) * 4u;
    if (instructionEnd > end || instructionEnd <= start) return false;

    // Extended instruction tokens: sample offsets, resource dimensions and
    // return types. They are stepped over rather than acted on -- a texture
    // fetch with a compile-time offset is translated without the offset, which
    // would be wrong, so the translator refuses one instead (see below).
    bool extended = instruction.extended;
    uint32_t extendedCount = 0;
    while (extended) {
      uint32_t const extendedToken = reader.U32();
      if (!reader.Ok()) return false;
      extendedCount++;
      if (extendedCount > 8) return false;
      instruction.extendedTokens.push_back(extendedToken);
      // D3D10_SB_EXTENDED_OPCODE_TYPE 1 is SAMPLE_CONTROLS: a compile-time
      // texel offset on a fetch, held as three 4-bit signed fields. It changes
      // which texel is read, so it is decoded here rather than stepped over.
      if ((extendedToken & 0x3fu) == 1) {
        auto signExtend4 = [](uint32_t value) -> int32_t {
          value &= 0xfu;
          return static_cast<int32_t>(value >= 8u ? value - 16u : value);
        };
        instruction.hasSampleOffsets = true;
        instruction.sampleOffsetU = signExtend4(extendedToken >> 9);
        instruction.sampleOffsetV = signExtend4(extendedToken >> 13);
        instruction.sampleOffsetW = signExtend4(extendedToken >> 17);
        if (instruction.sampleOffsetU == 0 && instruction.sampleOffsetV == 0 &&
            instruction.sampleOffsetW == 0) {
          instruction.hasSampleOffsets = false;
        }
      }
      extended = (extendedToken & 0x80000000u) != 0;
    }

    auto const& info = OpcodeInfo(instruction.opcode);
    if (info.operandCount > 0) {
      instruction.operands.resize(static_cast<size_t>(info.operandCount));
      for (auto& operand : instruction.operands) {
        if (!DecodeOperand(reader, operand, 0)) return false;
        if (reader.Position() > instructionEnd) return false;
      }
    }

    // Whatever the opcode leaves behind inside its own length: dcl_temps' count,
    // dcl_resource's return-type token, dcl_thread_group's three sizes.
    while (reader.Position() + 4 <= instructionEnd) {
      instruction.extra.push_back(reader.U32());
      if (!reader.Ok()) return false;
    }

    switch (instruction.opcode) {
      case OP_DCL_TEMPS:
        if (!instruction.extra.empty()) {
          program.tempCount = std::max(program.tempCount, instruction.extra[0]);
        }
        break;
      case OP_DCL_INDEXABLE_TEMP:
        if (instruction.extra.size() >= 3) {
          Program::IndexableTemp temp;
          temp.index = instruction.extra[0];
          temp.arraySize = instruction.extra[1];
          temp.components = instruction.extra[2];
          program.indexableTemps.push_back(temp);
        }
        break;
      case OP_DCL_GLOBAL_FLAGS:
        program.globalFlags = instruction.controls;
        break;
      case OP_DCL_MAX_OUTPUT_VERTEX_COUNT:
        if (!instruction.extra.empty()) program.maxOutputVertexCount = instruction.extra[0];
        break;
      default:
        break;
    }

    reader.Seek(instructionEnd);
    if (!reader.Ok()) return false;
    if (program.instructions.size() >= kMaxInstructions) return false;
    program.instructions.push_back(std::move(instruction));
  }
  return true;
}

}  // namespace

Program ParseProgram(uint8_t const* data, size_t size) {
  Program program;
  if (!LooksLikeDxbc(data, size)) {
    program.error = "not a DXBC container";
    return program;
  }

  Reader reader(data, size);
  reader.Skip(4);   // magic
  reader.Skip(16);  // checksum
  reader.U32();     // always 1
  uint32_t const totalSize = reader.U32();
  uint32_t const chunkCount = reader.U32();
  if (!reader.Ok()) {
    program.error = "truncated DXBC header";
    return program;
  }
  size_t const limit = std::min(static_cast<size_t>(totalSize), size);
  // Every chunk needs at least its own 8-byte header plus a 4-byte offset entry.
  if (chunkCount > limit / 12) {
    program.error = "DXBC chunk count of " + std::to_string(chunkCount) + " cannot fit the container";
    return program;
  }

  std::vector<uint32_t> chunkOffsets(chunkCount);
  for (uint32_t i = 0; i < chunkCount; i++) chunkOffsets[i] = reader.U32();
  if (!reader.Ok()) {
    program.error = "truncated DXBC chunk table";
    return program;
  }

  bool sawBytecode = false;
  for (uint32_t i = 0; i < chunkCount; i++) {
    size_t const offset = chunkOffsets[i];
    if (offset + 8 > limit) {
      program.error = "DXBC chunk " + std::to_string(i) + " starts past the end of the container";
      return program;
    }
    uint32_t fourCC = 0;
    uint32_t chunkSize = 0;
    std::memcpy(&fourCC, data + offset, 4);
    std::memcpy(&chunkSize, data + offset + 4, 4);
    if (chunkSize > limit - offset - 8) {
      program.error = "DXBC chunk " + std::to_string(i) + " claims more bytes than remain";
      return program;
    }
    uint8_t const* chunk = data + offset + 8;

    switch (fourCC) {
      case kChunkIsgn:
      case kChunkIsg1:
        if (!ParseSignatureChunk(chunk, chunkSize, false, program.inputSignature)) {
          program.error = "malformed input signature chunk";
          return program;
        }
        break;
      case kChunkOsgn:
      case kChunkOsg1:
        if (!ParseSignatureChunk(chunk, chunkSize, false, program.outputSignature)) {
          program.error = "malformed output signature chunk";
          return program;
        }
        break;
      case kChunkOsg5:
        if (!ParseSignatureChunk(chunk, chunkSize, true, program.outputSignature)) {
          program.error = "malformed stream-output signature chunk";
          return program;
        }
        break;
      case kChunkPcsg:
        ParseSignatureChunk(chunk, chunkSize, false, program.patchSignature);
        break;
      case kChunkShdr:
      case kChunkShex:
        // The bytecode carries the shader model, and RDEF's variable stride
        // depends on it, so the version is read before anything else uses it.
        if (chunkSize >= 4) {
          uint32_t versionToken = 0;
          std::memcpy(&versionToken, chunk, 4);
          program.majorVersion = (versionToken >> 4) & 0xfu;
          program.minorVersion = versionToken & 0xfu;
        }
        sawBytecode = true;
        break;
      default:
        break;
    }
  }

  // RDEF is parsed in a second pass because its layout depends on the shader
  // model, which only the bytecode chunk states, and chunk order is not fixed.
  for (uint32_t i = 0; i < chunkCount; i++) {
    size_t const offset = chunkOffsets[i];
    uint32_t fourCC = 0;
    uint32_t chunkSize = 0;
    std::memcpy(&fourCC, data + offset, 4);
    std::memcpy(&chunkSize, data + offset + 4, 4);
    if (fourCC != kChunkRdef) continue;
    if (!ParseRdefChunk(data + offset + 8, chunkSize, program)) {
      program.error = "malformed resource definition chunk";
      return program;
    }
  }

  if (!sawBytecode) {
    program.error = "DXBC container has no SHDR/SHEX bytecode chunk";
    return program;
  }
  for (uint32_t i = 0; i < chunkCount; i++) {
    size_t const offset = chunkOffsets[i];
    uint32_t fourCC = 0;
    uint32_t chunkSize = 0;
    std::memcpy(&fourCC, data + offset, 4);
    std::memcpy(&chunkSize, data + offset + 4, 4);
    if (fourCC != kChunkShdr && fourCC != kChunkShex) continue;
    if (!DecodeInstructions(data + offset + 8, chunkSize, program)) {
      program.error = "malformed shader bytecode";
      return program;
    }
  }

  program.ok = true;
  return program;
}

namespace {

char const kComponentNames[5] = "xyzw";

std::string FormatMaskOrSwizzle(Operand const& operand) {
  if (operand.numComponents != 4) return {};
  if (operand.selectionMode == 0) {
    if (operand.mask == 0 || operand.mask == 0xf) return {};
    std::string text = ".";
    for (int i = 0; i < 4; i++) {
      if (operand.mask & (1u << i)) text += kComponentNames[i];
    }
    return text;
  }
  std::string text = ".";
  for (int i = 0; i < 4; i++) text += kComponentNames[operand.swizzle[i] & 0x3u];
  if (text == ".xyzw") return {};
  return text;
}

std::string FormatHex32(uint32_t value) {
  static char const digits[] = "0123456789abcdef";
  std::string text = "0x";
  for (int shift = 28; shift >= 0; shift -= 4) {
    text += digits[(value >> shift) & 0xfu];
  }
  return text;
}

std::string FormatOperand(Operand const& operand);

std::string FormatIndex(OperandIndex const& index) {
  std::string text;
  if (index.hasRelative && index.relative) {
    text += FormatOperand(*index.relative);
    if (index.hasImmediate && index.immediate != 0) {
      text += " + " + std::to_string(index.immediate);
    }
  } else {
    text += std::to_string(index.immediate);
  }
  return text;
}

std::string FormatOperand(Operand const& operand) {
  std::string text;
  switch (operand.type) {
    case kOperandImmediate32: {
      text = "l(";
      for (size_t i = 0; i < operand.immediates.size(); i++) {
        if (i != 0) text += ", ";
        text += FormatHex32(operand.immediates[i]);
      }
      text += ")";
      break;
    }
    case kOperandNull:
      text = "null";
      break;
    case kOperandOutputDepth:
      text = "oDepth";
      break;
    case kOperandOutputCoverageMask:
      text = "oMask";
      break;
    case kOperandInputPrimitiveID:
      text = "vPrim";
      break;
    default: {
      char const* prefix = "?";
      switch (operand.type) {
        case kOperandTemp: prefix = "r"; break;
        case kOperandInput: prefix = "v"; break;
        case kOperandOutput: prefix = "o"; break;
        case kOperandIndexableTemp: prefix = "x"; break;
        case kOperandSampler: prefix = "s"; break;
        case kOperandResource: prefix = "t"; break;
        case kOperandConstantBuffer: prefix = "cb"; break;
        case kOperandImmediateConstantBuffer: prefix = "icb"; break;
        case kOperandLabel: prefix = "label"; break;
        default: break;
      }
      text = prefix;
      if (std::string_view(prefix) == "?") {
        text = "operand" + std::to_string(operand.type);
      }
      for (size_t i = 0; i < operand.indices.size(); i++) {
        if (i == 0 && (operand.type == kOperandConstantBuffer ||
                       operand.type == kOperandIndexableTemp)) {
          text += FormatIndex(operand.indices[i]);
          continue;
        }
        text += "[" + FormatIndex(operand.indices[i]) + "]";
      }
      break;
    }
  }
  text += FormatMaskOrSwizzle(operand);
  switch (operand.modifier) {
    case Modifier::Neg: text = "-" + text; break;
    case Modifier::Abs: text = "|" + text + "|"; break;
    case Modifier::AbsNeg: text = "-|" + text + "|"; break;
    default: break;
  }
  return text;
}

}  // namespace

std::string DisassembleProgram(Program const& program) {
  std::string out;
  out += std::string(StageName(program.stage)) + "_" + std::to_string(program.majorVersion) + "_" +
         std::to_string(program.minorVersion) + "\n";
  for (auto const& instruction : program.instructions) {
    out += OpcodeName(instruction.opcode);
    // Bit 13 of the opcode token is saturate only for a value-producing
    // instruction. A declaration uses the whole controls field for its own
    // meaning, and printing _sat for one would read as a flag that is not there.
    bool const isDeclaration = instruction.opcode >= OP_DCL_RESOURCE &&
                               instruction.opcode <= OP_DCL_GLOBAL_FLAGS;
    if (instruction.saturate && !isDeclaration) out += "_sat";
    for (size_t i = 0; i < instruction.operands.size(); i++) {
      out += i == 0 ? " " : ", ";
      out += FormatOperand(instruction.operands[i]);
    }
    switch (instruction.opcode) {
      case OP_DCL_TEMPS:
      case OP_DCL_INDEXABLE_TEMP:
      case OP_DCL_THREAD_GROUP:
        for (auto value : instruction.extra) out += " " + std::to_string(value);
        break;
      default:
        break;
    }
    out += "\n";
  }
  return out;
}


// ---------------------------------------------------------------------------
// GLSL ES emission
// ---------------------------------------------------------------------------
//
// The register model is deliberately the naive one: every DXBC temp becomes a
// vec4 and integer operations round-trip through floatBitsToInt/intBitsToFloat.
// DXBC registers are typeless -- the same four bytes are read as float by one
// instruction and as int by the next -- and any model that tries to infer a
// type per register has to be right every time or it silently miscompiles. The
// bit-cast form is always right, and the driver's optimiser removes the casts.
//
// The output version is not fixed. GLSL ES 3.00 covers an ordinary vertex or
// fragment shader; textureGather, uaddCarry, imulExtended, multisample fetches
// and storage buffers are 3.10; geometry shaders and textureGatherOffset are
// 3.20. The emitter raises the version it asks for as it meets an instruction
// that needs one, so a plain shader stays at 300 and a compute or geometry
// shader gets what it requires. Quest's Adreno parts expose GLES 3.2 and Unity
// compiles the source on the device, so this costs nothing where it is not
// needed.

namespace {

std::string FormatFloatLiteral(uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, 4);
  // A NaN or infinity cannot be written as a GLSL literal at all. They do turn
  // up in real shaders as "unused component" filler, so they become the largest
  // finite value of the same sign rather than stopping the translation.
  if (!(value == value)) return "0.0";
  if (value > 3.4e38f) return "3.402823466e+38";
  if (value < -3.4e38f) return "-3.402823466e+38";
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
  std::string text = buffer;
  if (text.find('.') == std::string::npos && text.find('e') == std::string::npos) {
    text += ".0";
  }
  return text;
}

std::string FormatIntLiteral(uint32_t bits) {
  int32_t value = 0;
  std::memcpy(&value, &bits, 4);
  // INT_MIN cannot be written as a negative literal in GLSL: the minus is a
  // unary operator applied to a positive constant that does not fit.
  if (value == INT32_MIN) return "(-2147483647 - 1)";
  return std::to_string(value);
}

int PopCount4(uint8_t mask) {
  int count = 0;
  for (int i = 0; i < 4; i++) {
    if (mask & (1u << i)) count++;
  }
  return count;
}

std::string VecType(int components, char const* prefix) {
  if (components <= 1) {
    if (std::string_view(prefix) == "i") return "int";
    if (std::string_view(prefix) == "u") return "uint";
    if (std::string_view(prefix) == "b") return "bool";
    return "float";
  }
  return std::string(prefix) + "vec" + std::to_string(components);
}

// One constant-buffer variable as it will be declared in GLSL, plus the byte
// range of the D3D buffer it covers so a cb0[k].c reference can be resolved
// back to it.
struct MappedVariable {
  std::string declaration;   // the whole "uniform vec4 _Color;" line
  std::string name;          // the GLSL identifier
  uint32_t startOffset = 0;
  uint32_t size = 0;
  bool isArray = false;      // declared as name[N] with a 16-byte stride
  uint32_t elementStride = 16;
  uint32_t componentCount = 4;
  bool scalarDeclaration = false;  // declared as a bare float/int, no swizzle
  // HLSL cbuffer ints/bools are declared as GLSL ints, so a read of one in the
  // float register model has to bit-cast rather than convert.
  bool integerTyped = false;
};

// A bound texture, as declared. D3D separates textures from samplers; GLSL ES
// has only the combined form, and Unity resolves it the same way -- one
// sampler named after the texture -- so a material's _MainTex still binds.
struct MappedTexture {
  std::string name;
  uint32_t dimension = 0;   // D3D_SRV_DIMENSION
  int coordinateComponents = 0;
  bool comparison = false;  // declared as a shadow sampler
  bool multisample = false;
};

// A read/write resource: a D3D UAV, or a structured/raw shader resource. These
// only appear in compute shaders in practice, and GLSL ES expresses them as
// images (typed) or storage buffers (raw and structured).
struct MappedStorage {
  std::string name;
  std::string blockName;
  uint32_t bindPoint = 0;
  bool typedImage = false;   // image2D/image3D rather than a buffer
  uint32_t dimension = 0;
  uint32_t stride = 0;       // structured buffers: bytes per element
  bool writable = false;
};

class GlslEmitter {
 public:
  GlslEmitter(Program const& program, GlslOptions const& options)
      : _program(program), _options(options), _version(options.version) {}

  GlslResult Run();

 private:
  // Anything that cannot be translated stops the whole shader. The first reason
  // is the one reported: later ones are usually consequences of it.
  void Fail(std::string reason) {
    if (_failed) return;
    _failed = true;
    _error = std::move(reason);
  }

  // Raises the GLSL ES version the output will declare. `why` names the
  // construct, so a shader that needs more than the ceiling says what pushed it
  // over rather than just failing.
  bool Require(int version, char const* why) {
    if (version <= _version) return true;
    if (version > _options.maximumVersion) {
      Fail(std::string(why) + " needs GLSL ES " + std::to_string(version / 100) + "." +
           std::to_string((version % 100) / 10) + ", above the ceiling this build allows");
      return false;
    }
    _version = version;
    return true;
  }

  bool BuildConstantBuffers();
  bool BuildResources();
  bool BuildSignatures();
  bool BuildStageDeclarations();
  bool EmitBody();
  bool EmitInstruction(Instruction const& instruction);

  std::string SrcBase(Operand const& operand, uint8_t mask);
  std::string SrcFloat(Operand const& operand, uint8_t mask);
  std::string SrcInt(Operand const& operand, uint8_t mask);
  std::string SrcUint(Operand const& operand, uint8_t mask);
  std::string ScalarFloat(Operand const& operand);  // first selected component
  std::string ScalarInt(Operand const& operand);
  std::string ScalarUint(Operand const& operand);
  std::string RegisterName(Operand const& operand);
  std::string ConstantComponent(Operand const& operand, int component);
  std::string DestName(Operand const& operand, uint8_t& mask);
  void WriteDest(Instruction const& instruction, Operand const& dest, std::string const& expression);
  void Line(std::string const& text);

  MappedTexture const* TextureFor(Operand const& resource);
  MappedStorage const* StorageFor(Operand const& operand);
  std::string SampleOffsetArgument(Instruction const& instruction, int components);
  std::string ResourceSwizzle(Operand const& resource, uint8_t mask);

  SignatureElement const* FindSignature(std::vector<SignatureElement> const& signature,
                                        uint32_t registerIndex) const;
  std::string VaryingName(SignatureElement const& element, bool vertexInput) const;
  std::string InterpolationQualifier(uint32_t registerIndex) const;

  Program const& _program;
  GlslOptions _options;

  std::string _declarations;
  std::string _body;       // main()'s statements
  std::string _functions;  // subroutine bodies, emitted before main()
  std::string _subroutineBody;
  std::string* _target = &_body;
  int _indent = 1;
  bool _failed = false;
  std::string _error;
  int _version = 300;

  std::vector<std::vector<MappedVariable>> _constantBuffers;  // by cb bind point
  std::vector<MappedTexture> _textures;                       // by t# register
  std::vector<MappedStorage> _storage;                        // by u# register
  std::vector<MappedStorage> _rawResources;                   // by t# register
  // Shared-memory blocks, built on first use because the instruction stream
  // declares them rather than the reflection data. A deque, not a vector, so a
  // pointer handed out earlier survives a later insertion.
  std::deque<MappedStorage> _sharedStorage;
  std::vector<std::string> _uniformNames;
  std::vector<std::string> _samplerList;
  // Which GLSL built-ins the body reached for. Each becomes a vec4 alias
  // declared at the top of main(), so the rest of the emitter can treat
  // gl_VertexID and r0 as the same kind of thing -- a typeless four-component
  // register -- instead of special-casing every read of one.
  bool _usedImmediateConstantBuffer = false;
  bool _usedFrontFace = false;
  bool _usedVertexID = false;
  bool _usedInstanceID = false;
  bool _usedPrimitiveID = false;
  bool _usedSampleIndex = false;
  bool _usedThreadID = false;
  bool _usedThreadGroupID = false;
  bool _usedThreadIDInGroup = false;
  bool _usedThreadIDFlattened = false;
  bool _usedGsInstanceID = false;
  // Thread-group shared memory, one entry per declared block.
  struct SharedBlock {
    uint32_t index = 0;
    uint32_t elements = 0;
    uint32_t stride = 0;
  };
  std::vector<SharedBlock> _sharedBlocks;
};

void GlslEmitter::Line(std::string const& text) {
  _target->append(static_cast<size_t>(_indent) * 2, ' ');
  *_target += text;
  *_target += '\n';
}

SignatureElement const* GlslEmitter::FindSignature(std::vector<SignatureElement> const& signature,
                                                   uint32_t registerIndex) const {
  for (auto const& element : signature) {
    if (element.registerIndex == registerIndex) return &element;
  }
  return nullptr;
}

// Unity's GLES shaders name vertex attributes in_SEMANTIC# and inter-stage
// varyings vs_SEMANTIC#, and the engine binds them back by exactly those names.
// A translated shader that named them anything else would link and then receive
// nothing.
std::string GlslEmitter::VaryingName(SignatureElement const& element, bool vertexInput) const {
  std::string name = element.semanticName;
  for (auto& c : name) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '_';
  }
  return (vertexInput ? "in_" : "vs_") + name + std::to_string(element.semanticIndex);
}

// dcl_input_ps carries the interpolation mode in its opcode controls. Getting
// this wrong is not cosmetic: an integer varying interpolated linearly is
// garbage on arrival, and D3D marks those "constant" for exactly that reason.
std::string GlslEmitter::InterpolationQualifier(uint32_t registerIndex) const {
  for (auto const& instruction : _program.instructions) {
    if (instruction.opcode != OP_DCL_INPUT_PS && instruction.opcode != OP_DCL_INPUT_PS_SGV &&
        instruction.opcode != OP_DCL_INPUT_PS_SIV) {
      continue;
    }
    if (instruction.operands.empty() || instruction.operands[0].indices.empty()) continue;
    if (static_cast<uint32_t>(instruction.operands[0].indices.back().immediate) != registerIndex) {
      continue;
    }
    // D3D10_SB_INTERPOLATION_MODE lives in bits 11..14 of the opcode token,
    // which is bits 0..3 of the controls field.
    switch (instruction.controls & 0xfu) {
      case 1: return "flat ";        // CONSTANT
      case 2: return "";             // LINEAR
      case 3: return "centroid ";    // LINEAR_CENTROID
      case 4: return "";             // LINEAR_NOPERSPECTIVE (GLSL ES has none)
      case 5: return "centroid ";    // LINEAR_NOPERSPECTIVE_CENTROID
      case 6: return "";             // LINEAR_SAMPLE
      case 7: return "";             // LINEAR_NOPERSPECTIVE_SAMPLE
      default: return "";
    }
  }
  return "";
}

}  // namespace

namespace {

bool GlslEmitter::BuildConstantBuffers() {
  uint32_t highestBindPoint = 0;
  for (auto const& buffer : _program.constantBuffers) {
    highestBindPoint = std::max(highestBindPoint, buffer.bindPoint);
  }
  if (highestBindPoint > 64) {
    Fail("constant buffer bound at register b" + std::to_string(highestBindPoint));
    return false;
  }
  _constantBuffers.resize(highestBindPoint + 1);

  for (auto const& buffer : _program.constantBuffers) {
    auto& mapped = _constantBuffers[buffer.bindPoint];
    for (auto const& variable : buffer.variables) {
      MappedVariable entry;
      entry.startOffset = variable.startOffset;
      entry.size = variable.size;

      bool const isMatrix = variable.variableClass == 2 || variable.variableClass == 3;
      char const* prefix = "";
      bool integerTyped = false;
      if (variable.variableType == 2) {
        prefix = "i";
        integerTyped = true;
      } else if (variable.variableType == 19) {
        prefix = "u";
        integerTyped = true;
      } else if (variable.variableType == 1) {
        // HLSL stores a cbuffer bool as a 4-byte int, and every read of it in
        // the bytecode is an integer read, so it is declared as one.
        prefix = "i";
        integerTyped = true;
      }

      if (isMatrix) {
        uint32_t const slots = (variable.size + 15u) / 16u;
        if (slots == 0 || slots > 4096) {
          Fail("constant buffer matrix '" + variable.name + "' has an impossible size");
          return false;
        }
        // The hlslcc_mtxRxC prefix is not decoration: it is the form Unity's
        // own GLES shaders use, and the engine strips it when matching a
        // material's matrix property to a uniform. A matrix named anything else
        // never gets bound.
        entry.name = "hlslcc_mtx" + std::to_string(variable.rows) + "x" +
                     std::to_string(variable.columns) + variable.name;
        entry.declaration = "uniform vec4 " + entry.name + "[" + std::to_string(slots) + "];";
        entry.isArray = true;
        entry.elementStride = 16;
        entry.componentCount = 4;
      } else if (variable.elements > 0) {
        // An HLSL cbuffer array always starts each element on a 16-byte
        // boundary, so anything narrower than a float4 would be declared in
        // GLSL with a stride the engine does not use. Refusing is the honest
        // answer; guessing would bind the wrong rows.
        if (variable.columns != 4 || variable.rows > 1) {
          Fail("constant buffer array '" + variable.name + "' is not an array of float4");
          return false;
        }
        if (variable.elements > 65536) {
          Fail("constant buffer array '" + variable.name + "' is implausibly long");
          return false;
        }
        entry.name = variable.name;
        entry.declaration = "uniform " + VecType(4, prefix) + " " + entry.name + "[" +
                            std::to_string(variable.elements) + "];";
        entry.isArray = true;
        entry.elementStride = 16;
        entry.componentCount = 4;
      } else {
        uint32_t const columns = variable.columns == 0 ? 1u : variable.columns;
        if (columns > 4) {
          Fail("constant buffer variable '" + variable.name + "' has " +
               std::to_string(columns) + " columns");
          return false;
        }
        entry.name = variable.name;
        entry.componentCount = columns;
        entry.scalarDeclaration = columns == 1;
        entry.declaration = "uniform " + VecType(static_cast<int>(columns), prefix) + " " +
                            entry.name + ";";
      }
      entry.elementStride = entry.elementStride == 0 ? 16 : entry.elementStride;
      entry.componentCount = entry.componentCount == 0 ? 1 : entry.componentCount;
      entry.integerTyped = integerTyped;
      mapped.push_back(std::move(entry));
    }
    std::sort(mapped.begin(), mapped.end(),
              [](MappedVariable const& a, MappedVariable const& b) {
                return a.startOffset < b.startOffset;
              });
    for (auto const& entry : mapped) {
      _declarations += entry.declaration + "\n";
      _uniformNames.push_back(entry.name);
    }
  }
  return !_failed;
}

bool GlslEmitter::BuildResources() {
  // Which textures are read through a comparison fetch, and which through a
  // multisample one. RDEF does not say; the instruction stream does, and a
  // shadow sampler declared as a plain one silently returns the wrong thing.
  std::set<uint32_t> comparisonTextures;
  for (auto const& instruction : _program.instructions) {
    bool const isComparison = instruction.opcode == OP_SAMPLE_C ||
                              instruction.opcode == OP_SAMPLE_C_LZ ||
                              instruction.opcode == OP_GATHER4_C ||
                              instruction.opcode == OP_GATHER4_PO_C;
    if (!isComparison) continue;
    size_t const resourceIndex = instruction.opcode == OP_GATHER4_PO_C ? 3u : 2u;
    if (instruction.operands.size() <= resourceIndex) continue;
    auto const& resource = instruction.operands[resourceIndex];
    if (resource.type != kOperandResource || resource.indices.empty()) continue;
    comparisonTextures.insert(static_cast<uint32_t>(resource.indices.back().immediate));
  }

  for (auto const& binding : _program.resourceBindings) {
    switch (binding.type) {
      case 2:  // D3D_SIT_TEXTURE
        break;
      case 0:  // cbuffer, handled by BuildConstantBuffers
      case 3:  // sampler, folded into the texture below
        continue;
      case 5:   // STRUCTURED
      case 7:   // BYTEADDRESS
      case 4:   // UAV_RWTYPED
      case 6:   // UAV_RWSTRUCTURED
      case 8:   // UAV_RWBYTEADDRESS
      case 11: {  // UAV_RWSTRUCTURED_WITH_COUNTER
        if (!Require(310, "a read/write or structured buffer")) return false;
        bool const writable = binding.type == 4 || binding.type == 6 || binding.type == 8 ||
                              binding.type == 11;
        MappedStorage storage;
        storage.name = binding.name;
        storage.blockName = binding.name + "_block";
        storage.bindPoint = binding.bindPoint;
        storage.writable = writable;
        storage.dimension = binding.dimension;
        storage.typedImage = binding.type == 4;
        if (storage.typedImage) {
          char const* format = nullptr;
          char const* type = nullptr;
          switch (binding.returnType) {
            case 3: format = "rgba32i"; type = "iimage"; break;   // SINT
            case 4: format = "rgba32ui"; type = "uimage"; break;  // UINT
            case 1: case 2: case 5: format = "rgba32f"; type = "image"; break;
            default: break;
          }
          char const* shape = nullptr;
          switch (binding.dimension) {
            case 4: shape = "2D"; break;
            case 5: shape = "2DArray"; break;
            case 8: shape = "3D"; break;
            default: break;
          }
          if (format == nullptr || shape == nullptr) {
            Fail("read/write texture '" + binding.name +
                 "' has a format or dimension GLSL ES has no image type for");
            return false;
          }
          _declarations += "layout(binding = " + std::to_string(binding.bindPoint) + ", " +
                           format + ") uniform highp " + type + shape + " " + binding.name + ";\n";
        } else {
          // Raw and structured buffers both become a storage buffer of uints:
          // the bytecode addresses them by byte or element offset and casts
          // whatever it finds, exactly as it does with registers.
          _declarations += "layout(std430, binding = " + std::to_string(binding.bindPoint) +
                           ") " + (writable ? "buffer " : "readonly buffer ") + storage.blockName +
                           " { uint " + binding.name + "[]; };\n";
        }
        auto& into = (binding.type == 5 || binding.type == 7) ? _rawResources : _storage;
        if (into.size() <= binding.bindPoint) into.resize(binding.bindPoint + 1);
        into[binding.bindPoint] = std::move(storage);
        continue;
      }
      default:
        Fail("bound resource '" + binding.name + "' is of a kind (" +
             std::to_string(binding.type) + ") this translator has no GLSL ES form for");
        return false;
    }

    if (binding.bindPoint > 64) {
      Fail("texture bound at register t" + std::to_string(binding.bindPoint));
      return false;
    }
    if (_textures.size() <= binding.bindPoint) _textures.resize(binding.bindPoint + 1);

    MappedTexture texture;
    texture.name = binding.name;
    texture.dimension = binding.dimension;
    texture.comparison = comparisonTextures.count(binding.bindPoint) != 0;

    std::string shape;
    switch (binding.dimension) {
      case 4: shape = "2D"; texture.coordinateComponents = 2; break;
      case 5: shape = "2DArray"; texture.coordinateComponents = 3; break;
      case 6:
        shape = "2DMS";
        texture.coordinateComponents = 2;
        texture.multisample = true;
        if (!Require(310, "a multisample texture")) return false;
        break;
      case 7:
        shape = "2DMSArray";
        texture.coordinateComponents = 3;
        texture.multisample = true;
        if (!Require(320, "a multisample texture array")) return false;
        break;
      case 8: shape = "3D"; texture.coordinateComponents = 3; break;
      case 9: shape = "Cube"; texture.coordinateComponents = 3; break;
      case 10:
        shape = "CubeArray";
        texture.coordinateComponents = 4;
        if (!Require(320, "a cube texture array")) return false;
        break;
      case 1:
        shape = "Buffer";
        texture.coordinateComponents = 1;
        if (!Require(320, "a texture buffer")) return false;
        break;
      default:
        Fail("texture '" + binding.name + "' has a dimension GLSL ES has no sampler for (" +
             std::to_string(binding.dimension) + ")");
        return false;
    }
    if (texture.comparison && texture.multisample) {
      Fail("texture '" + binding.name + "' is fetched both as a shadow map and a multisample "
           "target, which no GLSL ES sampler type covers");
      return false;
    }

    // The sampler's scalar type follows the resource's return type: an integer
    // texture read through a float sampler would convert rather than reinterpret.
    char const* typePrefix = "";
    if (!texture.comparison) {
      switch (binding.returnType) {
        case 3: typePrefix = "i"; break;  // SINT
        case 4: typePrefix = "u"; break;  // UINT
        default: break;                   // UNORM/SNORM/FLOAT/MIXED all read as float
      }
    }
    std::string const samplerType =
        std::string(typePrefix) + "sampler" + shape + (texture.comparison ? "Shadow" : "");
    _declarations += "uniform highp " + samplerType + " " + binding.name + ";\n";
    _samplerList.push_back(binding.name);
    _textures[binding.bindPoint] = std::move(texture);
  }
  return !_failed;
}

}  // namespace

namespace {

// D3D_NAME (system value) numbers that appear in a signature.
constexpr uint32_t kSvUndefined = 0;
constexpr uint32_t kSvPosition = 1;
constexpr uint32_t kSvClipDistance = 2;
constexpr uint32_t kSvCullDistance = 3;
constexpr uint32_t kSvRenderTargetArrayIndex = 4;
constexpr uint32_t kSvVertexID = 6;
constexpr uint32_t kSvPrimitiveID = 7;
constexpr uint32_t kSvInstanceID = 8;
constexpr uint32_t kSvIsFrontFace = 9;
constexpr uint32_t kSvSampleIndex = 10;
constexpr uint32_t kSvTarget = 64;
constexpr uint32_t kSvDepth = 65;
constexpr uint32_t kSvCoverage = 66;
constexpr uint32_t kSvDepthGreaterEqual = 67;
constexpr uint32_t kSvDepthLessEqual = 68;

bool GlslEmitter::BuildSignatures() {
  Stage const stage = _program.stage;
  bool const isVertex = stage == Stage::Vertex;
  bool const isPixel = stage == Stage::Pixel;
  bool const isGeometry = stage == Stage::Geometry;
  bool const isCompute = stage == Stage::Compute;

  if (stage == Stage::Hull || stage == Stage::Domain) {
    // A hull shader is not one program: it is a control-point phase plus fork
    // and join phases with their own instruction streams and their own
    // register spaces. Translating it wrong would be worse than not translating
    // it, and no Vivify map has ever needed one on this target.
    Fail("tessellation (" + std::string(StageName(stage)) +
         ") programs are not translated; the shader keeps its DirectX programs");
    return false;
  }
  if (stage == Stage::Unknown) {
    Fail("the program header does not name a pipeline stage this translator knows");
    return false;
  }
  if (isCompute) return true;  // compute has no signature; see BuildStageDeclarations
  if (isGeometry && !Require(320, "a geometry shader")) return false;

  for (auto const& element : _program.inputSignature) {
    switch (element.systemValueType) {
      case kSvUndefined:
        break;
      case kSvPosition:
        if (isPixel || isGeometry) continue;  // gl_FragCoord / gl_in[].gl_Position
        Fail("a vertex program declares SV_Position as an input");
        return false;
      case kSvIsFrontFace:
      case kSvVertexID:
      case kSvInstanceID:
      case kSvPrimitiveID:
        continue;  // GLSL built-ins, bound in the prologue
      case kSvSampleIndex:
        if (!Require(320, "SV_SampleIndex")) return false;
        continue;
      default:
        Fail("input semantic '" + element.semanticName + "' is system value " +
             std::to_string(element.systemValueType) +
             ", which this translator has no GLSL ES equivalent for");
        return false;
    }
    std::string const name = VaryingName(element, isVertex);
    if (isGeometry) {
      // A geometry shader reads its inputs per vertex, so every varying is an
      // array sized by the input primitive.
      //
      // Its outputs feed the fragment stage, which was compiled separately and
      // expects the same vs_SEMANTIC# names the vertex stage writes -- so a
      // geometry shader that passes a semantic straight through would have to
      // declare one name as both an input and an output. That is not legal
      // GLSL, and renaming either side breaks the link with a program this
      // translator never sees. Programs are translated one at a time, so the
      // pipeline-wide rename the name scheme would need is not available; the
      // collision is reported instead of guessed at.
      for (auto const& output : _program.outputSignature) {
        if (output.systemValueType != kSvUndefined) continue;
        if (VaryingName(output, false) != name) continue;
        Fail("this geometry program passes '" + element.semanticName +
             std::to_string(element.semanticIndex) +
             "' through, which would need one varying name to be both its input and its "
             "output; translating stages separately cannot rename it on both sides");
        return false;
      }
      _declarations += "in vec4 " + name + "[];\n";
    } else {
      _declarations += InterpolationQualifier(element.registerIndex) + "in vec4 " + name + ";\n";
    }
  }

  for (auto const& element : _program.outputSignature) {
    switch (element.systemValueType) {
      case kSvUndefined:
        break;
      case kSvPosition:
        continue;  // gl_Position
      case kSvTarget:
        break;
      case kSvDepth:
      case kSvDepthGreaterEqual:
      case kSvDepthLessEqual:
      case kSvCoverage:
        continue;  // gl_FragDepth / gl_SampleMask, written through operand types
      case kSvRenderTargetArrayIndex:
        if (!Require(320, "SV_RenderTargetArrayIndex")) return false;
        continue;  // gl_Layer
      case kSvClipDistance:
      case kSvCullDistance:
        // gl_ClipDistance is not in GLSL ES without an extension, and silently
        // dropping a clip plane draws geometry that should have been cut away.
        Fail("output semantic '" + element.semanticName +
             "' needs clip/cull distances, which GLSL ES does not have");
        return false;
      default:
        Fail("output semantic '" + element.semanticName + "' is system value " +
             std::to_string(element.systemValueType) +
             ", which this translator has no GLSL ES equivalent for");
        return false;
    }

    if (isPixel) {
      _declarations += "layout(location = " + std::to_string(element.registerIndex) +
                       ") out vec4 " + element.semanticName +
                       std::to_string(element.semanticIndex) + ";\n";
      continue;
    }
    _declarations += "out vec4 " + VaryingName(element, false) + ";\n";
  }
  return !_failed;
}

// The per-stage layout declarations that are not signatures: a compute
// shader's work-group size and shared memory, a geometry shader's input and
// output primitives.
bool GlslEmitter::BuildStageDeclarations() {
  for (auto const& instruction : _program.instructions) {
    switch (instruction.opcode) {
      case OP_DCL_THREAD_GROUP: {
        if (instruction.extra.size() < 3) {
          Fail("dcl_thread_group without its three sizes");
          return false;
        }
        if (!Require(310, "a compute shader")) return false;
        _declarations += "layout(local_size_x = " + std::to_string(instruction.extra[0]) +
                         ", local_size_y = " + std::to_string(instruction.extra[1]) +
                         ", local_size_z = " + std::to_string(instruction.extra[2]) + ") in;\n";
        break;
      }
      case OP_DCL_TGSM_RAW:
      case OP_DCL_TGSM_STRUCTURED: {
        if (!Require(310, "thread group shared memory")) return false;
        if (instruction.operands.empty() || instruction.operands[0].indices.empty()) {
          Fail("a shared memory declaration with no register");
          return false;
        }
        SharedBlock block;
        block.index = static_cast<uint32_t>(instruction.operands[0].indices.back().immediate);
        if (instruction.opcode == OP_DCL_TGSM_RAW) {
          if (instruction.extra.empty()) {
            Fail("dcl_tgsm_raw without a byte count");
            return false;
          }
          block.stride = 4;
          block.elements = instruction.extra[0] / 4u;
        } else {
          if (instruction.extra.size() < 2) {
            Fail("dcl_tgsm_structured without a stride and count");
            return false;
          }
          block.stride = instruction.extra[0];
          block.elements = instruction.extra[1] * (block.stride / 4u);
        }
        if (block.elements == 0 || block.elements > (1u << 20)) {
          Fail("a shared memory block of an impossible size");
          return false;
        }
        _declarations += "shared uint g" + std::to_string(block.index) + "[" +
                         std::to_string(block.elements) + "];\n";
        _sharedBlocks.push_back(block);
        break;
      }
      case OP_DCL_GS_INPUT_PRIMITIVE: {
        char const* primitive = nullptr;
        switch (instruction.controls & 0x3fu) {
          case 1: primitive = "points"; break;
          case 2: primitive = "lines"; break;
          case 3: primitive = "triangles"; break;
          case 6: primitive = "lines_adjacency"; break;
          case 7: primitive = "triangles_adjacency"; break;
          default: break;
        }
        if (primitive == nullptr) {
          Fail("a geometry shader input primitive GLSL ES has no layout for");
          return false;
        }
        _declarations += "layout(" + std::string(primitive) + ") in;\n";
        break;
      }
      case OP_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY: {
        char const* topology = nullptr;
        switch (instruction.controls & 0x3fu) {
          case 1: topology = "points"; break;
          case 3: topology = "line_strip"; break;
          case 5: topology = "triangle_strip"; break;
          default: break;
        }
        if (topology == nullptr) {
          Fail("a geometry shader output topology GLSL ES has no layout for");
          return false;
        }
        _declarations += "layout(" + std::string(topology) + ", max_vertices = " +
                         std::to_string(_program.maxOutputVertexCount == 0
                                            ? 1u
                                            : _program.maxOutputVertexCount) + ") out;\n";
        break;
      }
      case OP_DCL_GS_INSTANCE_COUNT:
        if (!instruction.extra.empty() && instruction.extra[0] > 1) {
          _declarations += "layout(invocations = " + std::to_string(instruction.extra[0]) + ") in;\n";
        }
        break;
      case OP_DCL_STREAM:
        // Multiple stream output is a D3D-only concept; a shader using more
        // than the default stream cannot be expressed here.
        if (!instruction.operands.empty() && !instruction.operands[0].indices.empty() &&
            instruction.operands[0].indices.back().immediate != 0) {
          Fail("a geometry shader writes to a stream other than the first, which GLSL ES "
               "has no form for");
          return false;
        }
        break;
      default:
        break;
    }
    if (_failed) return false;
  }
  return true;
}

}  // namespace

namespace {

std::string GlslEmitter::RegisterName(Operand const& operand) {
  switch (operand.type) {
    case kOperandTemp:
      if (operand.indices.empty()) {
        Fail("a temp register with no index");
        return {};
      }
      return "r" + std::to_string(operand.indices[0].immediate);
    case kOperandIndexableTemp: {
      if (operand.indices.size() < 2) {
        Fail("an indexable temp with fewer than two indices");
        return {};
      }
      std::string index;
      if (operand.indices[1].hasRelative && operand.indices[1].relative) {
        index = ScalarInt(*operand.indices[1].relative);
        if (operand.indices[1].immediate != 0) {
          index += " + " + std::to_string(operand.indices[1].immediate);
        }
      } else {
        index = std::to_string(operand.indices[1].immediate);
      }
      return "x" + std::to_string(operand.indices[0].immediate) + "[" + index + "]";
    }
    case kOperandInput: {
      if (operand.indices.empty()) {
        Fail("an input register with no index");
        return {};
      }
      // A geometry shader addresses its inputs as v[vertex][register]; every
      // other stage has one index.
      bool const perVertex = operand.indices.size() >= 2;
      OperandIndex const& registerPart = operand.indices.back();
      uint32_t const registerIndex = static_cast<uint32_t>(registerPart.immediate);
      auto const* element = FindSignature(_program.inputSignature, registerIndex);
      if (element == nullptr) {
        Fail("input register v" + std::to_string(registerIndex) +
             " is not in the input signature");
        return {};
      }
      if (_program.stage == Stage::Pixel && element->systemValueType == kSvPosition) {
        return "gl_FragCoord";
      }
      switch (element->systemValueType) {
        case kSvIsFrontFace: _usedFrontFace = true; return "vFrontFace";
        case kSvVertexID: _usedVertexID = true; return "vVertexID";
        case kSvInstanceID: _usedInstanceID = true; return "vInstanceID";
        case kSvPrimitiveID: _usedPrimitiveID = true; return "vPrimitiveID";
        case kSvSampleIndex: _usedSampleIndex = true; return "vSampleIndex";
        default: break;
      }
      std::string name = VaryingName(*element, _program.stage == Stage::Vertex);
      if (perVertex) {
        if (_program.stage == Stage::Geometry &&
            element->systemValueType == kSvPosition) {
          name = "gl_in";
        }
        std::string vertex;
        OperandIndex const& vertexPart = operand.indices[0];
        if (vertexPart.hasRelative && vertexPart.relative) {
          vertex = ScalarInt(*vertexPart.relative);
          if (vertexPart.immediate != 0) vertex += " + " + std::to_string(vertexPart.immediate);
        } else {
          vertex = std::to_string(vertexPart.immediate);
        }
        if (name == "gl_in") return "gl_in[" + vertex + "].gl_Position";
        return name + "[" + vertex + "]";
      }
      return name;
    }
    case kOperandOutput: {
      if (operand.indices.empty()) {
        Fail("an output register with no index");
        return {};
      }
      uint32_t const registerIndex = static_cast<uint32_t>(operand.indices.back().immediate);
      auto const* element = FindSignature(_program.outputSignature, registerIndex);
      if (element == nullptr) {
        Fail("output register o" + std::to_string(registerIndex) +
             " is not in the output signature");
        return {};
      }
      switch (element->systemValueType) {
        case kSvPosition: return "gl_Position";
        case kSvDepth:
        case kSvDepthGreaterEqual:
        case kSvDepthLessEqual: return "gl_FragDepth";
        case kSvRenderTargetArrayIndex: return "gl_Layer";
        case kSvCoverage: return "gl_SampleMask[0]";
        default: break;
      }
      if (_program.stage == Stage::Pixel) {
        return element->semanticName + std::to_string(element->semanticIndex);
      }
      return VaryingName(*element, false);
    }
    case kOperandOutputDepth:
    case kOperandOutputDepthGreaterEqual:
    case kOperandOutputDepthLessEqual:
      return "gl_FragDepth";
    case kOperandOutputCoverageMask:
      return "gl_SampleMask[0]";
    case kOperandInputCoverageMask:
      return "gl_SampleMaskIn[0]";
    case kOperandInputPrimitiveID:
      _usedPrimitiveID = true;
      return "vPrimitiveID";
    case kOperandInputGsInstanceID:
      _usedGsInstanceID = true;
      return "vGsInstanceID";
    case kOperandThreadGroupSharedMemory:
    case kOperandUnorderedAccessView:
      // These are addressed by the buffer instructions, which resolve them
      // through StorageFor rather than as a register.
      Fail("a buffer register used where a value register was expected");
      return {};
    case kOperandInputThreadID:
      _usedThreadID = true;
      return "vThreadID";
    case kOperandInputThreadGroupID:
      _usedThreadGroupID = true;
      return "vThreadGroupID";
    case kOperandInputThreadIDInGroup:
      _usedThreadIDInGroup = true;
      return "vThreadIDInGroup";
    case kOperandInputThreadIDInGroupFlattened:
      _usedThreadIDFlattened = true;
      return "vThreadIDInGroupFlattened";
    case kOperandNull:
      return "null";
    default:
      Fail("operand type " + std::to_string(operand.type) + " has no GLSL equivalent here");
      return {};
  }
}

// Resolves one component of a cb#[k] reference back to the named uniform that
// covers it. Unity binds material properties by uniform name, so a translated
// shader that kept D3D's flat "constant buffer of float4s" view would link and
// then receive nothing.
std::string GlslEmitter::ConstantComponent(Operand const& operand, int component) {
  if (operand.type == kOperandImmediateConstantBuffer) {
    _usedImmediateConstantBuffer = true;
    if (operand.indices.empty()) {
      Fail("an immediate constant buffer reference with no index");
      return {};
    }
    std::string index;
    if (operand.indices[0].hasRelative && operand.indices[0].relative) {
      index = ScalarInt(*operand.indices[0].relative);
      if (operand.indices[0].immediate != 0) {
        index += " + " + std::to_string(operand.indices[0].immediate);
      }
    } else {
      index = std::to_string(operand.indices[0].immediate);
    }
    return "ImmCB[" + index + "]." + kComponentNames[component];
  }

  if (operand.indices.size() < 2) {
    Fail("a constant buffer reference with fewer than two indices");
    return {};
  }
  uint32_t const bindPoint = static_cast<uint32_t>(operand.indices[0].immediate);
  if (bindPoint >= _constantBuffers.size() || _constantBuffers[bindPoint].empty()) {
    Fail("the shader reads constant buffer b" + std::to_string(bindPoint) +
         ", which its reflection data does not describe");
    return {};
  }
  auto const& variables = _constantBuffers[bindPoint];
  OperandIndex const& rowIndex = operand.indices[1];
  uint32_t const byteOffset =
      static_cast<uint32_t>(rowIndex.immediate) * 16u + static_cast<uint32_t>(component) * 4u;

  MappedVariable const* found = nullptr;
  for (auto const& variable : variables) {
    if (byteOffset >= variable.startOffset && byteOffset < variable.startOffset + variable.size) {
      found = &variable;
      break;
    }
  }
  if (found == nullptr) {
    Fail("the shader reads b" + std::to_string(bindPoint) + " at byte " +
         std::to_string(byteOffset) + ", which no declared variable covers");
    return {};
  }

  std::string expression;
  if (rowIndex.hasRelative && rowIndex.relative) {
    // A dynamically indexed constant buffer only makes sense as an array
    // variable; anything else would be reading across variable boundaries at
    // run time, which the named-uniform model cannot express.
    if (!found->isArray) {
      Fail("the shader indexes b" + std::to_string(bindPoint) +
           " dynamically, but the variable at that offset ('" + found->name +
           "') is not an array");
      return {};
    }
    std::string index = ScalarInt(*rowIndex.relative);
    int64_t const base = static_cast<int64_t>(found->startOffset / 16u);
    int64_t const bias = static_cast<int64_t>(rowIndex.immediate) - base;
    if (bias != 0) {
      index += (bias > 0 ? " + " : " - ") + std::to_string(bias > 0 ? bias : -bias);
    }
    expression = found->name + "[" + index + "]." + kComponentNames[component];
  } else if (found->isArray) {
    uint32_t const withinVariable = byteOffset - found->startOffset;
    expression = found->name + "[" + std::to_string(withinVariable / found->elementStride) + "]." +
                 kComponentNames[(withinVariable % found->elementStride) / 4u];
  } else if (found->scalarDeclaration) {
    expression = found->name;
  } else {
    uint32_t const withinVariable = byteOffset - found->startOffset;
    expression = found->name + "." + kComponentNames[withinVariable / 4u];
  }

  if (found->integerTyped) expression = "intBitsToFloat(" + expression + ")";
  return expression;
}

std::string GlslEmitter::SrcBase(Operand const& operand, uint8_t mask) {
  int const count = PopCount4(mask);
  if (count == 0) {
    Fail("an instruction reads no components of a source");
    return {};
  }

  if (operand.type == kOperandImmediate32) {
    std::vector<std::string> parts;
    for (int i = 0; i < 4; i++) {
      if (!(mask & (1u << i))) continue;
      uint32_t const component = operand.numComponents == 1 ? 0u : operand.swizzle[i] & 0x3u;
      uint32_t const bits = component < operand.immediates.size() ? operand.immediates[component] : 0u;
      parts.push_back(FormatFloatLiteral(bits));
    }
    if (count == 1) return parts[0];
    std::string text = "vec" + std::to_string(count) + "(";
    for (size_t i = 0; i < parts.size(); i++) {
      if (i != 0) text += ", ";
      text += parts[i];
    }
    return text + ")";
  }

  if (operand.type == kOperandConstantBuffer || operand.type == kOperandImmediateConstantBuffer) {
    std::vector<std::string> parts;
    for (int i = 0; i < 4; i++) {
      if (!(mask & (1u << i))) continue;
      int const component = operand.numComponents == 1 ? 0 : static_cast<int>(operand.swizzle[i] & 0x3u);
      parts.push_back(ConstantComponent(operand, component));
      if (_failed) return {};
    }
    if (count == 1) return parts[0];
    // Components resolve one at a time, because consecutive components of one
    // cb register can belong to different variables. When they do all land in
    // the same one -- the common case, a float4 -- the pieces are put back
    // together as a swizzle rather than left as vec4(_M[0].x, _M[0].y, ...).
    bool collapsible = true;
    std::string prefix;
    std::string components;
    for (auto const& part : parts) {
      if (part.size() < 3 || part[part.size() - 2] != '.') {
        collapsible = false;
        break;
      }
      char const component = part.back();
      if (component != 'x' && component != 'y' && component != 'z' && component != 'w') {
        collapsible = false;
        break;
      }
      std::string const partPrefix = part.substr(0, part.size() - 2);
      if (components.empty()) {
        prefix = partPrefix;
      } else if (partPrefix != prefix) {
        collapsible = false;
        break;
      }
      components += component;
    }
    if (collapsible) return prefix + "." + components;

    std::string text = "vec" + std::to_string(count) + "(";
    for (size_t i = 0; i < parts.size(); i++) {
      if (i != 0) text += ", ";
      text += parts[i];
    }
    return text + ")";
  }

  std::string const name = RegisterName(operand);
  if (_failed || name.empty()) return {};
  if (operand.numComponents == 0) return name;
  // Scalar built-ins take no swizzle; a mask on one is D3D asking for the same
  // value in every component.
  if (name == "gl_FragDepth" || name == "gl_Layer" || name == "gl_SampleMask[0]" ||
      name == "gl_SampleMaskIn[0]") {
    if (count == 1) return name;
    return "vec" + std::to_string(count) + "(intBitsToFloat(" + name + "))";
  }

  std::string swizzle = ".";
  for (int i = 0; i < 4; i++) {
    if (!(mask & (1u << i))) continue;
    swizzle += kComponentNames[operand.numComponents == 1 ? 0 : (operand.swizzle[i] & 0x3u)];
  }
  return name + swizzle;
}

std::string GlslEmitter::SrcFloat(Operand const& operand, uint8_t mask) {
  std::string text = SrcBase(operand, mask);
  if (_failed) return {};
  switch (operand.modifier) {
    case Modifier::Neg: return "(-" + text + ")";
    case Modifier::Abs: return "abs(" + text + ")";
    case Modifier::AbsNeg: return "(-abs(" + text + "))";
    default: return text;
  }
}

std::string GlslEmitter::SrcInt(Operand const& operand, uint8_t mask) {
  int const count = PopCount4(mask);
  std::string text;
  if (operand.type == kOperandImmediate32) {
    std::vector<std::string> parts;
    for (int i = 0; i < 4; i++) {
      if (!(mask & (1u << i))) continue;
      uint32_t const component = operand.numComponents == 1 ? 0u : operand.swizzle[i] & 0x3u;
      uint32_t const bits = component < operand.immediates.size() ? operand.immediates[component] : 0u;
      parts.push_back(FormatIntLiteral(bits));
    }
    if (count == 1) {
      text = parts[0];
    } else {
      text = "ivec" + std::to_string(count) + "(";
      for (size_t i = 0; i < parts.size(); i++) {
        if (i != 0) text += ", ";
        text += parts[i];
      }
      text += ")";
    }
  } else {
    std::string const base = SrcBase(operand, mask);
    if (_failed) return {};
    text = "floatBitsToInt(" + base + ")";
  }
  switch (operand.modifier) {
    case Modifier::Neg: return "(-" + text + ")";
    case Modifier::Abs: return "abs(" + text + ")";
    case Modifier::AbsNeg: return "(-abs(" + text + "))";
    default: return text;
  }
}

std::string GlslEmitter::SrcUint(Operand const& operand, uint8_t mask) {
  int const count = PopCount4(mask);
  if (operand.type == kOperandImmediate32) {
    std::vector<std::string> parts;
    for (int i = 0; i < 4; i++) {
      if (!(mask & (1u << i))) continue;
      uint32_t const component = operand.numComponents == 1 ? 0u : operand.swizzle[i] & 0x3u;
      uint32_t const bits = component < operand.immediates.size() ? operand.immediates[component] : 0u;
      parts.push_back(std::to_string(bits) + "u");
    }
    if (count == 1) return parts[0];
    std::string text = "uvec" + std::to_string(count) + "(";
    for (size_t i = 0; i < parts.size(); i++) {
      if (i != 0) text += ", ";
      text += parts[i];
    }
    return text + ")";
  }
  std::string const base = SrcBase(operand, mask);
  if (_failed) return {};
  return "floatBitsToUint(" + base + ")";
}

std::string GlslEmitter::ScalarFloat(Operand const& operand) {
  return SrcFloat(operand, 0x1);
}

std::string GlslEmitter::ScalarInt(Operand const& operand) {
  return SrcInt(operand, 0x1);
}

std::string GlslEmitter::ScalarUint(Operand const& operand) {
  return SrcUint(operand, 0x1);
}

std::string GlslEmitter::DestName(Operand const& operand, uint8_t& mask) {
  mask = operand.numComponents == 4 ? operand.mask : 0x1;
  if (operand.type == kOperandNull) return "null";
  std::string const name = RegisterName(operand);
  if (_failed) return {};
  return name;
}

void GlslEmitter::WriteDest(Instruction const& instruction, Operand const& dest,
                            std::string const& expression) {
  if (_failed) return;
  uint8_t mask = 0;
  std::string const name = DestName(dest, mask);
  if (_failed) return;
  if (name == "null") return;

  std::string value = expression;
  if (instruction.saturate) value = "clamp(" + value + ", 0.0, 1.0)";

  if (name == "gl_FragDepth") {
    Line(name + " = " + value + ";");
    return;
  }
  // gl_Layer and the coverage mask are ints in GLSL and typeless registers in
  // DXBC, so the bits have to be reinterpreted rather than converted.
  if (name == "gl_Layer" || name == "gl_SampleMask[0]") {
    Line(name + " = floatBitsToInt(" + value + ");");
    return;
  }
  std::string swizzle = ".";
  for (int i = 0; i < 4; i++) {
    if (mask & (1u << i)) swizzle += kComponentNames[i];
  }
  Line(name + swizzle + " = " + value + ";");
}

}  // namespace

namespace {

MappedTexture const* GlslEmitter::TextureFor(Operand const& resource) {
  if (resource.indices.empty()) {
    Fail("a texture fetch with no resource register");
    return nullptr;
  }
  uint32_t const index = static_cast<uint32_t>(resource.indices.back().immediate);
  if (index >= _textures.size() || _textures[index].name.empty()) {
    Fail("the shader samples t" + std::to_string(index) +
         ", which its reflection data does not name");
    return nullptr;
  }
  return &_textures[index];
}

MappedStorage const* GlslEmitter::StorageFor(Operand const& operand) {
  if (operand.indices.empty()) {
    Fail("a buffer access with no register");
    return nullptr;
  }
  uint32_t const index = static_cast<uint32_t>(operand.indices.back().immediate);
  if (operand.type == kOperandThreadGroupSharedMemory) {
    // Shared memory is declared by the instruction stream rather than by the
    // reflection data, so its entry is built here on first use.
    for (auto const& existing : _sharedStorage) {
      if (existing.bindPoint == index) return &existing;
    }
    for (auto const& block : _sharedBlocks) {
      if (block.index != index) continue;
      MappedStorage storage;
      storage.name = "g" + std::to_string(index);
      storage.bindPoint = index;
      storage.stride = block.stride;
      storage.writable = true;
      _sharedStorage.push_back(std::move(storage));
      return &_sharedStorage.back();
    }
    Fail("the shader uses shared memory g" + std::to_string(index) + ", which it never declared");
    return nullptr;
  }
  std::vector<MappedStorage> const& table =
      operand.type == kOperandUnorderedAccessView ? _storage : _rawResources;
  if (index >= table.size() || table[index].name.empty()) {
    Fail("the shader accesses " +
         std::string(operand.type == kOperandUnorderedAccessView ? "u" : "t") +
         std::to_string(index) + ", which its reflection data does not name");
    return nullptr;
  }
  return &table[index];
}

// The compile-time texel offset on a fetch, as GLSL's separate offset argument.
// Dropping this changes which texel is read, so a fetch that carries one is
// translated to the *Offset form rather than the plain one.
std::string GlslEmitter::SampleOffsetArgument(Instruction const& instruction, int components) {
  if (!instruction.hasSampleOffsets) return {};
  std::string text = ", ";
  if (components <= 1) return text + std::to_string(instruction.sampleOffsetU);
  text += "ivec" + std::to_string(components) + "(" + std::to_string(instruction.sampleOffsetU);
  if (components >= 2) text += ", " + std::to_string(instruction.sampleOffsetV);
  if (components >= 3) text += ", " + std::to_string(instruction.sampleOffsetW);
  return text + ")";
}

// The resource operand's swizzle says which channel of the fetched texel feeds
// each written component, so t0.yyyy in the bytecode has to become .yyyy on the
// GLSL fetch.
std::string GlslEmitter::ResourceSwizzle(Operand const& resource, uint8_t mask) {
  std::string swizzle = ".";
  for (int i = 0; i < 4; i++) {
    if (mask & (1u << i)) swizzle += kComponentNames[resource.swizzle[i] & 0x3u];
  }
  return swizzle;
}

bool GlslEmitter::EmitInstruction(Instruction const& instruction) {
  auto const& operands = instruction.operands;
  uint32_t const opcode = instruction.opcode;

  // Declarations carry no code; everything they say has already been read off
  // the reflection chunks, the program header, or BuildStageDeclarations.
  switch (opcode) {
    case OP_DCL_RESOURCE:
    case OP_DCL_CONSTANT_BUFFER:
    case OP_DCL_SAMPLER:
    case OP_DCL_INDEX_RANGE:
    case OP_DCL_INPUT:
    case OP_DCL_INPUT_SGV:
    case OP_DCL_INPUT_SIV:
    case OP_DCL_INPUT_PS:
    case OP_DCL_INPUT_PS_SGV:
    case OP_DCL_INPUT_PS_SIV:
    case OP_DCL_OUTPUT:
    case OP_DCL_OUTPUT_SGV:
    case OP_DCL_OUTPUT_SIV:
    case OP_DCL_TEMPS:
    case OP_DCL_INDEXABLE_TEMP:
    case OP_DCL_GLOBAL_FLAGS:
    case OP_DCL_GS_INPUT_PRIMITIVE:
    case OP_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
    case OP_DCL_GS_INSTANCE_COUNT:
    case OP_DCL_MAX_OUTPUT_VERTEX_COUNT:
    case OP_DCL_STREAM:
    case OP_DCL_THREAD_GROUP:
    case OP_DCL_UAV_TYPED:
    case OP_DCL_UAV_RAW:
    case OP_DCL_UAV_STRUCTURED:
    case OP_DCL_TGSM_RAW:
    case OP_DCL_TGSM_STRUCTURED:
    case OP_DCL_RESOURCE_RAW:
    case OP_DCL_RESOURCE_STRUCTURED:
    case OP_NOP:
      return true;
    default:
      break;
  }

  auto destMask = [&]() -> uint8_t {
    if (operands.empty()) return 0;
    return operands[0].numComponents == 4 ? operands[0].mask : 0x1;
  };
  auto maskOf = [&](size_t index) -> uint8_t {
    if (operands.size() <= index) return 0x1;
    return operands[index].numComponents == 4 ? operands[index].mask : 0x1;
  };

  // The per-component ALU shape: every source is read with the destination's
  // mask, so a masked write only evaluates the components it writes.
  auto unary = [&](std::string const& function) {
    uint8_t const mask = destMask();
    WriteDest(instruction, operands[0], function + "(" + SrcFloat(operands[1], mask) + ")");
  };
  auto binary = [&](char const* op) {
    uint8_t const mask = destMask();
    std::string const a = SrcFloat(operands[1], mask);
    std::string const b = SrcFloat(operands[2], mask);
    WriteDest(instruction, operands[0], "(" + a + " " + op + " " + b + ")");
  };
  auto binaryFunction = [&](char const* function) {
    uint8_t const mask = destMask();
    std::string const a = SrcFloat(operands[1], mask);
    std::string const b = SrcFloat(operands[2], mask);
    WriteDest(instruction, operands[0], std::string(function) + "(" + a + ", " + b + ")");
  };
  auto intBinary = [&](char const* op) {
    uint8_t const mask = destMask();
    std::string const a = SrcInt(operands[1], mask);
    std::string const b = SrcInt(operands[2], mask);
    WriteDest(instruction, operands[0], "intBitsToFloat(" + a + " " + op + " " + b + ")");
  };
  auto intBinaryFunction = [&](char const* function) {
    uint8_t const mask = destMask();
    std::string const a = SrcInt(operands[1], mask);
    std::string const b = SrcInt(operands[2], mask);
    WriteDest(instruction, operands[0],
              "intBitsToFloat(" + std::string(function) + "(" + a + ", " + b + "))");
  };
  auto uintBinary = [&](char const* op) {
    uint8_t const mask = destMask();
    std::string const a = SrcUint(operands[1], mask);
    std::string const b = SrcUint(operands[2], mask);
    WriteDest(instruction, operands[0], "uintBitsToFloat(" + a + " " + op + " " + b + ")");
  };
  auto uintBinaryFunction = [&](char const* function) {
    uint8_t const mask = destMask();
    std::string const a = SrcUint(operands[1], mask);
    std::string const b = SrcUint(operands[2], mask);
    WriteDest(instruction, operands[0],
              "uintBitsToFloat(" + std::string(function) + "(" + a + ", " + b + "))");
  };
  // A DXBC comparison writes 0xFFFFFFFF or 0, not a bool. Negating the ivec
  // built from the bvec turns true into all-bits-set, which is what every
  // following and/movc expects to find there.
  auto compare = [&](char const* vectorFunction, char const* scalarOperator, int kind) {
    uint8_t const mask = destMask();
    int const count = PopCount4(mask);
    std::string a;
    std::string b;
    if (kind == 0) {
      a = SrcFloat(operands[1], mask);
      b = SrcFloat(operands[2], mask);
    } else if (kind == 1) {
      a = SrcInt(operands[1], mask);
      b = SrcInt(operands[2], mask);
    } else {
      a = SrcUint(operands[1], mask);
      b = SrcUint(operands[2], mask);
    }
    std::string expression;
    if (count == 1) {
      expression = "intBitsToFloat((" + a + " " + scalarOperator + " " + b + ") ? -1 : 0)";
    } else {
      expression = "intBitsToFloat(-ivec" + std::to_string(count) + "(" +
                   std::string(vectorFunction) + "(" + a + ", " + b + ")))";
    }
    WriteDest(instruction, operands[0], expression);
  };
  auto dotProduct = [&](int components) {
    uint8_t const mask = destMask();
    int const count = PopCount4(mask);
    uint8_t const sourceMask = static_cast<uint8_t>((1u << components) - 1u);
    std::string const a = SrcFloat(operands[1], sourceMask);
    std::string const b = SrcFloat(operands[2], sourceMask);
    std::string expression = "dot(" + a + ", " + b + ")";
    if (count > 1) expression = "vec" + std::to_string(count) + "(" + expression + ")";
    WriteDest(instruction, operands[0], expression);
  };
  auto broadcast = [&](std::string const& scalar, int count, char const* prefix) {
    if (count == 1) return scalar;
    return VecType(count, prefix) + "(" + scalar + ")";
  };

  // ---- texture fetches ----------------------------------------------------
  auto emitSample = [&](char const* function, char const* offsetFunction, int extraOperand,
                        bool comparison) {
    if (operands.size() < 4) {
      Fail("a texture fetch with too few operands");
      return;
    }
    Operand const& resource = operands[2];
    MappedTexture const* texture = TextureFor(resource);
    if (texture == nullptr) return;
    if (texture->comparison != comparison) {
      Fail("texture '" + texture->name +
           "' is fetched both with and without depth comparison, which needs two sampler types");
      return;
    }
    // A buffer texture and a multisample texture have no sampler state and no
    // mip chain; GLSL ES offers only texelFetch for them, and D3D only reaches
    // them through ld. A filtered fetch from one is bytecode this cannot mean.
    if (texture->dimension == 1 || texture->multisample) {
      Fail("'" + OpcodeName(opcode) + "' filters texture '" + texture->name +
           "', which GLSL ES can only fetch by texel");
      return;
    }
    int coordinateComponents = texture->coordinateComponents;
    if (comparison) {
      // A shadow sampler folds the comparison value into the coordinate, so
      // sampler2DShadow takes a vec3 where sampler2D takes a vec2.
      coordinateComponents += 1;
      if (coordinateComponents > 4) {
        Fail("texture '" + texture->name +
             "' is a shadow map of a shape GLSL ES has no comparison sampler for");
        return;
      }
    }
    std::string call;
    if (instruction.hasSampleOffsets) {
      if (offsetFunction == nullptr) {
        Fail("a " + OpcodeName(opcode) +
             " carries a texel offset, and GLSL ES has no offset form of it");
        return;
      }
      call = offsetFunction;
    } else {
      call = function;
    }
    call += "(" + texture->name + ", ";
    if (comparison) {
      // The comparison operand is the last one; it becomes the extra coordinate
      // component.
      uint8_t const coordinateMask =
          static_cast<uint8_t>((1u << (coordinateComponents - 1)) - 1u);
      call += "vec" + std::to_string(coordinateComponents) + "(" +
              SrcFloat(operands[1], coordinateMask) + ", " +
              ScalarFloat(operands[operands.size() - 1]) + ")";
    } else {
      call += SrcFloat(operands[1], static_cast<uint8_t>((1u << coordinateComponents) - 1u));
    }
    if (extraOperand >= 0) {
      if (operands.size() <= static_cast<size_t>(extraOperand)) {
        Fail("a texture fetch is missing its lod/bias operand");
        return;
      }
      call += ", " + ScalarFloat(operands[static_cast<size_t>(extraOperand)]);
    }
    // The offset is a separate argument in GLSL and rides on the instruction
    // in DXBC. Its component count follows the texture's addressable
    // dimensions, not the coordinate's -- an array layer takes no offset.
    int offsetComponents = texture->coordinateComponents;
    if (texture->dimension == 5) offsetComponents = 2;   // Texture2DArray
    if (texture->dimension == 7) offsetComponents = 2;   // Texture2DMSArray
    if (texture->dimension == 10) offsetComponents = 3;  // TextureCubeArray
    call += SampleOffsetArgument(instruction, offsetComponents);
    call += ")";

    uint8_t const mask = destMask();
    if (comparison) {
      // A shadow fetch returns one float; D3D writes it to every selected
      // component.
      WriteDest(instruction, operands[0], broadcast(call, PopCount4(mask), ""));
      return;
    }
    WriteDest(instruction, operands[0], call + ResourceSwizzle(resource, mask));
  };

  auto emitGather = [&](bool comparison) {
    if (operands.size() < 4) {
      Fail("a gather4 with too few operands");
      return;
    }
    if (!Require(310, "textureGather")) return;
    Operand const& resource = operands[2];
    MappedTexture const* texture = TextureFor(resource);
    if (texture == nullptr) return;
    if (texture->comparison != comparison) {
      Fail("texture '" + texture->name +
           "' is gathered both with and without depth comparison, which needs two sampler types");
      return;
    }
    if (texture->dimension == 1 || texture->multisample) {
      Fail("gather4 reads texture '" + texture->name +
           "', which GLSL ES can only fetch by texel");
      return;
    }
    if (instruction.hasSampleOffsets && !Require(320, "textureGatherOffset")) return;

    std::string call = instruction.hasSampleOffsets ? "textureGatherOffset(" : "textureGather(";
    call += texture->name + ", " +
            SrcFloat(operands[1], static_cast<uint8_t>((1u << texture->coordinateComponents) - 1u));
    if (instruction.hasSampleOffsets) {
      int offsetComponents = texture->coordinateComponents;
      if (texture->dimension == 5) offsetComponents = 2;   // Texture2DArray
      if (texture->dimension == 9) {
        Fail("a cube map gather cannot carry a texel offset");
        return;
      }
      call += SampleOffsetArgument(instruction, offsetComponents);
    }
    if (comparison) {
      call += ", " + ScalarFloat(operands[operands.size() - 1]);
    } else {
      // Without a comparison, the channel gathered is the resource operand's
      // first swizzle slot: gather4 reads one channel of four neighbouring
      // texels, and the swizzle is how D3D says which.
      call += ", " + std::to_string(resource.swizzle[0] & 0x3u);
    }
    call += ")";
    // textureGather returns the four texels in the order (0,1) (1,1) (1,0)
    // (0,0), which is the same order D3D's gather4 writes, so the destination
    // mask maps straight across.
    uint8_t const mask = destMask();
    int const count = PopCount4(mask);
    std::string swizzle = ".";
    for (int i = 0; i < 4; i++) {
      if (mask & (1u << i)) swizzle += kComponentNames[i];
    }
    WriteDest(instruction, operands[0], count == 4 ? call : call + swizzle);
  };

  // ---- buffers ------------------------------------------------------------
  //
  // Raw and structured buffers are addressed in bytes or in (element, offset)
  // pairs and become an array of uints, exactly as the bytecode treats them:
  // it reads whatever is there and casts.
  auto bufferElementExpression = [&](Operand const& resourceOperand, Operand const& first,
                                     Operand const* second, uint32_t stride,
                                     int component) -> std::string {
    MappedStorage const* storage = StorageFor(resourceOperand);
    if (storage == nullptr) return {};
    std::string index;
    if (second == nullptr) {
      // Raw: a single byte address.
      index = "((" + ScalarUint(first) + " >> 2u) + " + std::to_string(component) + "u)";
    } else {
      std::string const element = ScalarUint(first);
      std::string const byteOffset = ScalarUint(*second);
      index = "((" + element + " * " + std::to_string(stride == 0 ? 4u : stride) + "u + " +
              byteOffset + ") >> 2u) + " + std::to_string(component) + "u";
      index = "(" + index + ")";
    }
    return storage->name + "[" + index + "]";
  };

  auto emitBufferLoad = [&](bool structured) {
    size_t const resourceIndex = structured ? 3u : 2u;
    if (operands.size() <= resourceIndex) {
      Fail("a buffer load with too few operands");
      return;
    }
    if (!Require(310, "a storage buffer")) return;
    Operand const& resourceOperand = operands[resourceIndex];
    MappedStorage const* storage = StorageFor(resourceOperand);
    if (storage == nullptr) return;
    uint8_t const mask = destMask();
    int const count = PopCount4(mask);
    std::vector<std::string> parts;
    for (int i = 0; i < 4; i++) {
      if (!(mask & (1u << i))) continue;
      int const channel = resourceOperand.swizzle[i] & 0x3;
      std::string const element =
          structured ? bufferElementExpression(resourceOperand, operands[1], &operands[2],
                                               storage->stride, channel)
                     : bufferElementExpression(resourceOperand, operands[1], nullptr, 0, channel);
      if (_failed) return;
      parts.push_back(element);
    }
    std::string expression = count == 1 ? parts[0] : "uvec" + std::to_string(count) + "(";
    if (count > 1) {
      for (size_t i = 0; i < parts.size(); i++) {
        if (i != 0) expression += ", ";
        expression += parts[i];
      }
      expression += ")";
    }
    WriteDest(instruction, operands[0], "uintBitsToFloat(" + expression + ")");
  };

  auto emitBufferStore = [&](bool structured) {
    size_t const valueIndex = structured ? 3u : 2u;
    if (operands.size() <= valueIndex) {
      Fail("a buffer store with too few operands");
      return;
    }
    if (!Require(310, "a storage buffer")) return;
    Operand const& destination = operands[0];
    MappedStorage const* storage = StorageFor(destination);
    if (storage == nullptr) return;
    if (!storage->writable) {
      Fail("the shader writes to '" + storage->name + "', which is bound read-only");
      return;
    }
    uint8_t const mask = destination.numComponents == 4 ? destination.mask : 0x1;
    Operand const& value = operands[valueIndex];
    for (int i = 0; i < 4; i++) {
      if (!(mask & (1u << i))) continue;
      std::string const element =
          structured ? bufferElementExpression(destination, operands[1], &operands[2],
                                               storage->stride, i)
                     : bufferElementExpression(destination, operands[1], nullptr, 0, i);
      if (_failed) return;
      Line(element + " = " + SrcUint(value, static_cast<uint8_t>(1u << i)) + ";");
    }
  };

  // ---- atomics ------------------------------------------------------------
  //
  // D3D's atomic_* discard the previous value and imm_atomic_* return it;
  // GLSL's atomic functions always return it, so the two differ only in whether
  // the result is written anywhere. Both address a buffer the same way an
  // ordinary load does.
  auto emitAtomic = [&](char const* function, bool returnsOld) {
    size_t const destinationIndex = returnsOld ? 1u : 0u;
    size_t const addressIndex = destinationIndex + 1u;
    if (operands.size() <= addressIndex + 1u) {
      Fail(OpcodeName(opcode) + " with too few operands");
      return;
    }
    if (!Require(310, "an atomic operation")) return;
    Operand const& destination = operands[destinationIndex];
    if (destination.type == kOperandUnorderedAccessView) {
      MappedStorage const* storage = StorageFor(destination);
      if (storage != nullptr && storage->typedImage) {
        // imageAtomic* needs an r32ui/r32i image, and a typed UAV declared from
        // reflection data is an rgba32 one. Guessing the format would compile
        // and corrupt whatever it wrote.
        Fail("an atomic operation on a typed image is not translated");
        return;
      }
    }
    MappedStorage const* storage = StorageFor(destination);
    if (storage == nullptr) return;
    if (!storage->writable) {
      Fail("the shader performs an atomic operation on '" + storage->name +
           "', which is bound read-only");
      return;
    }
    // The address is a byte offset for a raw buffer and an element index plus
    // byte offset for a structured one; both reduce to a dword index.
    Operand const& address = operands[addressIndex];
    std::string index;
    if (address.numComponents == 4 && PopCount4(address.mask == 0 ? 0xf : address.mask) > 1 &&
        storage->stride > 4) {
      index = "(((" + SrcUint(address, 0x1) + " * " + std::to_string(storage->stride) + "u) + " +
              SrcUint(address, 0x2) + ") >> 2u)";
    } else {
      index = "(" + SrcUint(address, 0x1) + " >> 2u)";
    }
    std::string const slot = storage->name + "[" + index + "]";
    std::string call;
    if (opcode == OP_IMM_ATOMIC_CMP_EXCH || opcode == OP_ATOMIC_CMP_STORE) {
      call = "atomicCompSwap(" + slot + ", " + SrcUint(operands[addressIndex + 1], 0x1) + ", " +
             SrcUint(operands[addressIndex + 2], 0x1) + ")";
    } else {
      call = std::string(function) + "(" + slot + ", " +
             SrcUint(operands[addressIndex + 1], 0x1) + ")";
    }
    if (!returnsOld) {
      Line(call + ";");
      return;
    }
    WriteDest(instruction, operands[0], "uintBitsToFloat(" + call + ")");
  };

  switch (opcode) {
    // ---- moves and float arithmetic ----------------------------------------
    case OP_MOV:
      WriteDest(instruction, operands[0], SrcFloat(operands[1], destMask()));
      break;
    case OP_ADD: binary("+"); break;
    case OP_MUL: binary("*"); break;
    case OP_DIV: binary("/"); break;
    case OP_MIN: binaryFunction("min"); break;
    case OP_MAX: binaryFunction("max"); break;
    case OP_FRC: unary("fract"); break;
    case OP_EXP: unary("exp2"); break;
    case OP_LOG: unary("log2"); break;
    case OP_RSQ: unary("inversesqrt"); break;
    case OP_SQRT: unary("sqrt"); break;
    case OP_ROUND_NE: unary("roundEven"); break;
    case OP_ROUND_NI: unary("floor"); break;
    case OP_ROUND_PI: unary("ceil"); break;
    case OP_ROUND_Z: unary("trunc"); break;
    case OP_DERIV_RTX:
    case OP_DERIV_RTX_COARSE:
    case OP_DERIV_RTX_FINE: unary("dFdx"); break;
    case OP_DERIV_RTY:
    case OP_DERIV_RTY_COARSE:
    case OP_DERIV_RTY_FINE: unary("dFdy"); break;
    case OP_RCP: {
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      std::string const one = count == 1 ? "1.0" : "vec" + std::to_string(count) + "(1.0)";
      WriteDest(instruction, operands[0], "(" + one + " / " + SrcFloat(operands[1], mask) + ")");
      break;
    }
    case OP_MAD: {
      uint8_t const mask = destMask();
      std::string const a = SrcFloat(operands[1], mask);
      std::string const b = SrcFloat(operands[2], mask);
      std::string const c = SrcFloat(operands[3], mask);
      WriteDest(instruction, operands[0], "(" + a + " * " + b + " + " + c + ")");
      break;
    }
    case OP_DP2: dotProduct(2); break;
    case OP_DP3: dotProduct(3); break;
    case OP_DP4: dotProduct(4); break;
    case OP_SINCOS: {
      if (operands[0].type != kOperandNull) {
        WriteDest(instruction, operands[0], "sin(" + SrcFloat(operands[2], maskOf(0)) + ")");
      }
      if (operands[1].type != kOperandNull) {
        WriteDest(instruction, operands[1], "cos(" + SrcFloat(operands[2], maskOf(1)) + ")");
      }
      break;
    }

    // ---- comparisons -------------------------------------------------------
    case OP_EQ: compare("equal", "==", 0); break;
    case OP_NE: compare("notEqual", "!=", 0); break;
    case OP_LT: compare("lessThan", "<", 0); break;
    case OP_GE: compare("greaterThanEqual", ">=", 0); break;
    case OP_IEQ: compare("equal", "==", 1); break;
    case OP_INE: compare("notEqual", "!=", 1); break;
    case OP_ILT: compare("lessThan", "<", 1); break;
    case OP_IGE: compare("greaterThanEqual", ">=", 1); break;
    case OP_ULT: compare("lessThan", "<", 2); break;
    case OP_UGE: compare("greaterThanEqual", ">=", 2); break;
    case OP_MOVC: {
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      std::string const selector = SrcInt(operands[1], mask);
      std::string const a = SrcFloat(operands[2], mask);
      std::string const b = SrcFloat(operands[3], mask);
      std::string expression;
      if (count == 1) {
        expression = "((" + selector + " != 0) ? " + a + " : " + b + ")";
      } else {
        expression = "mix(" + b + ", " + a + ", notEqual(" + selector + ", ivec" +
                     std::to_string(count) + "(0)))";
      }
      WriteDest(instruction, operands[0], expression);
      break;
    }

    // ---- integer arithmetic ------------------------------------------------
    case OP_IADD: intBinary("+"); break;
    case OP_AND: intBinary("&"); break;
    case OP_OR: intBinary("|"); break;
    case OP_XOR: intBinary("^"); break;
    case OP_ISHL: intBinary("<<"); break;
    case OP_ISHR: intBinary(">>"); break;
    case OP_IMAX: intBinaryFunction("max"); break;
    case OP_IMIN: intBinaryFunction("min"); break;
    case OP_UMAX: uintBinaryFunction("max"); break;
    case OP_UMIN: uintBinaryFunction("min"); break;
    case OP_USHR: uintBinary(">>"); break;
    case OP_NOT: {
      WriteDest(instruction, operands[0],
                "intBitsToFloat(~" + SrcInt(operands[1], destMask()) + ")");
      break;
    }
    case OP_INEG: {
      WriteDest(instruction, operands[0],
                "intBitsToFloat(-" + SrcInt(operands[1], destMask()) + ")");
      break;
    }
    case OP_IMAD: {
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0],
                "intBitsToFloat(" + SrcInt(operands[1], mask) + " * " + SrcInt(operands[2], mask) +
                    " + " + SrcInt(operands[3], mask) + ")");
      break;
    }
    case OP_UMAD: {
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0],
                "uintBitsToFloat(" + SrcUint(operands[1], mask) + " * " +
                    SrcUint(operands[2], mask) + " + " + SrcUint(operands[3], mask) + ")");
      break;
    }
    case OP_ITOF: {
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0],
                VecType(PopCount4(mask), "") + "(" + SrcInt(operands[1], mask) + ")");
      break;
    }
    case OP_UTOF: {
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0],
                VecType(PopCount4(mask), "") + "(" + SrcUint(operands[1], mask) + ")");
      break;
    }
    case OP_FTOI: {
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0],
                "intBitsToFloat(" + VecType(PopCount4(mask), "i") + "(" +
                    SrcFloat(operands[1], mask) + "))");
      break;
    }
    case OP_FTOU: {
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0],
                "uintBitsToFloat(" + VecType(PopCount4(mask), "u") + "(" +
                    SrcFloat(operands[1], mask) + "))");
      break;
    }
    case OP_IMUL:
    case OP_UMUL: {
      bool const wantHigh = operands[0].type != kOperandNull;
      bool const wantLow = operands[1].type != kOperandNull;
      if (wantHigh) {
        // The high half of a 32x32 multiply needs imulExtended/umulExtended,
        // which are GLSL ES 3.10. Below that there is no correct expression
        // for it at all.
        if (!Require(310, OpcodeName(opcode) == "imul" ? "imul with a high half"
                                                       : "umul with a high half")) {
          return false;
        }
        uint8_t const highMask = maskOf(0);
        uint8_t const lowMask = wantLow ? maskOf(1) : highMask;
        int const count = PopCount4(highMask);
        bool const isSigned = opcode == OP_IMUL;
        std::string const type = VecType(count, isSigned ? "i" : "u");
        std::string const a = isSigned ? SrcInt(operands[2], highMask) : SrcUint(operands[2], highMask);
        std::string const b = isSigned ? SrcInt(operands[3], highMask) : SrcUint(operands[3], highMask);
        std::string const highTemp = "mulHi" + std::to_string(instruction.tokenOffset);
        std::string const lowTemp = "mulLo" + std::to_string(instruction.tokenOffset);
        Line(type + " " + highTemp + ", " + lowTemp + ";");
        Line(std::string(isSigned ? "imulExtended(" : "umulExtended(") + a + ", " + b + ", " +
             highTemp + ", " + lowTemp + ");");
        WriteDest(instruction, operands[0],
                  std::string(isSigned ? "intBitsToFloat(" : "uintBitsToFloat(") + highTemp + ")");
        if (wantLow) {
          (void)lowMask;
          WriteDest(instruction, operands[1],
                    std::string(isSigned ? "intBitsToFloat(" : "uintBitsToFloat(") + lowTemp + ")");
        }
        break;
      }
      if (!wantLow) break;
      uint8_t const mask = maskOf(1);
      if (opcode == OP_IMUL) {
        WriteDest(instruction, operands[1],
                  "intBitsToFloat(" + SrcInt(operands[2], mask) + " * " +
                      SrcInt(operands[3], mask) + ")");
      } else {
        WriteDest(instruction, operands[1],
                  "uintBitsToFloat(" + SrcUint(operands[2], mask) + " * " +
                      SrcUint(operands[3], mask) + ")");
      }
      break;
    }
    case OP_UDIV: {
      if (operands[0].type != kOperandNull) {
        uint8_t const mask = maskOf(0);
        WriteDest(instruction, operands[0],
                  "uintBitsToFloat(" + SrcUint(operands[2], mask) + " / " +
                      SrcUint(operands[3], mask) + ")");
      }
      if (operands[1].type != kOperandNull) {
        uint8_t const mask = maskOf(1);
        WriteDest(instruction, operands[1],
                  "uintBitsToFloat(" + SrcUint(operands[2], mask) + " % " +
                      SrcUint(operands[3], mask) + ")");
      }
      break;
    }
    case OP_UADDC:
    case OP_USUBB: {
      if (!Require(310, opcode == OP_UADDC ? "uaddCarry" : "usubBorrow")) return false;
      uint8_t const mask = maskOf(0);
      int const count = PopCount4(mask);
      std::string const carry = "carry" + std::to_string(instruction.tokenOffset);
      Line(VecType(count, "u") + " " + carry + ";");
      std::string const call = std::string(opcode == OP_UADDC ? "uaddCarry(" : "usubBorrow(") +
                               SrcUint(operands[2], mask) + ", " + SrcUint(operands[3], mask) +
                               ", " + carry + ")";
      WriteDest(instruction, operands[0], "uintBitsToFloat(" + call + ")");
      if (operands[1].type != kOperandNull) {
        WriteDest(instruction, operands[1], "uintBitsToFloat(" + carry + ")");
      }
      break;
    }
    case OP_SWAPC: {
      // swapc writes src1 and src2 to its two destinations, swapped where the
      // selector is non-zero. The two writes have to read the sources before
      // either destination is touched, so both go through a temporary.
      uint8_t const maskA = maskOf(0);
      uint8_t const maskB = maskOf(1);
      int const countA = PopCount4(maskA);
      std::string const selector = SrcInt(operands[2], maskA);
      std::string const first = "swapA" + std::to_string(instruction.tokenOffset);
      std::string const second = "swapB" + std::to_string(instruction.tokenOffset);
      Line(VecType(countA, "") + " " + first + " = " + SrcFloat(operands[3], maskA) + ";");
      Line(VecType(PopCount4(maskB), "") + " " + second + " = " + SrcFloat(operands[4], maskB) +
           ";");
      auto select = [&](std::string const& whenSet, std::string const& whenClear, int count) {
        if (count == 1) return "((" + selector + " != 0) ? " + whenSet + " : " + whenClear + ")";
        return "mix(" + whenClear + ", " + whenSet + ", notEqual(" + selector + ", ivec" +
               std::to_string(count) + "(0)))";
      };
      if (operands[0].type != kOperandNull) {
        WriteDest(instruction, operands[0], select(second, first, countA));
      }
      if (operands[1].type != kOperandNull) {
        WriteDest(instruction, operands[1], select(first, second, PopCount4(maskB)));
      }
      break;
    }

    // ---- bit manipulation ---------------------------------------------------
    case OP_COUNTBITS: {
      // bitCount returns a signed count; D3D's result is an unsigned one, so
      // it is converted rather than bit-cast.
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      WriteDest(instruction, operands[0],
                "uintBitsToFloat(" + VecType(count, "u") + "(bitCount(" +
                    SrcUint(operands[1], mask) + ")))");
      break;
    }
    case OP_BFREV: {
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0],
                "uintBitsToFloat(bitfieldReverse(" + SrcUint(operands[1], mask) + "))");
      break;
    }
    case OP_FIRSTBIT_HI:
    case OP_FIRSTBIT_SHI:
    case OP_FIRSTBIT_LO: {
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      std::string const value = opcode == OP_FIRSTBIT_SHI ? SrcInt(operands[1], mask)
                                                          : SrcUint(operands[1], mask);
      // findMSB counts from the low end and D3D's firstbit_hi counts from the
      // high end, so the result is mirrored; findLSB matches firstbit_lo
      // directly. Both return -1 for "no bits set", which is what D3D reports
      // as 0xFFFFFFFF.
      std::string expression;
      if (opcode == OP_FIRSTBIT_LO) {
        expression = "findLSB(" + value + ")";
      } else {
        expression = "(" + broadcast("31", count, "i") + " - findMSB(" + value + "))";
      }
      WriteDest(instruction, operands[0], "intBitsToFloat(" + expression + ")");
      break;
    }
    case OP_UBFE:
    case OP_IBFE: {
      // D3D takes width and offset as separate operands and masks them to five
      // bits; GLSL's bitfieldExtract takes them as ints in the same order.
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      bool const isSigned = opcode == OP_IBFE;
      std::string const width = SrcInt(operands[1], mask);
      std::string const offset = SrcInt(operands[2], mask);
      std::string const value = isSigned ? SrcInt(operands[3], mask) : SrcUint(operands[3], mask);
      std::string const call = "bitfieldExtract(" + value + ", (" + offset + ") & " +
                               broadcast("31", count, "i") + ", (" + width + ") & " +
                               broadcast("31", count, "i") + ")";
      WriteDest(instruction, operands[0],
                std::string(isSigned ? "intBitsToFloat(" : "uintBitsToFloat(") + call + ")");
      break;
    }
    case OP_BFI: {
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      std::string const width = SrcInt(operands[1], mask);
      std::string const offset = SrcInt(operands[2], mask);
      std::string const insert = SrcUint(operands[3], mask);
      std::string const base = SrcUint(operands[4], mask);
      WriteDest(instruction, operands[0],
                "uintBitsToFloat(bitfieldInsert(" + base + ", " + insert + ", (" + offset +
                    ") & " + broadcast("31", count, "i") + ", (" + width + ") & " +
                    broadcast("31", count, "i") + "))");
      break;
    }
    case OP_F32TOF16: {
      // D3D packs one float into the low sixteen bits of each component;
      // packHalf2x16 packs two, so the second is zero and the result is the
      // low half.
      uint8_t const mask = destMask();
      std::vector<std::string> parts;
      for (int i = 0; i < 4; i++) {
        if (!(mask & (1u << i))) continue;
        parts.push_back("(packHalf2x16(vec2(" + SrcFloat(operands[1], static_cast<uint8_t>(1u << i)) +
                        ", 0.0)) & 0xffffu)");
      }
      std::string expression = parts.size() == 1 ? parts[0]
                                                 : "uvec" + std::to_string(parts.size()) + "(";
      if (parts.size() > 1) {
        for (size_t i = 0; i < parts.size(); i++) {
          if (i != 0) expression += ", ";
          expression += parts[i];
        }
        expression += ")";
      }
      WriteDest(instruction, operands[0], "uintBitsToFloat(" + expression + ")");
      break;
    }
    case OP_F16TOF32: {
      uint8_t const mask = destMask();
      std::vector<std::string> parts;
      for (int i = 0; i < 4; i++) {
        if (!(mask & (1u << i))) continue;
        parts.push_back("unpackHalf2x16(" + SrcUint(operands[1], static_cast<uint8_t>(1u << i)) +
                        " & 0xffffu).x");
      }
      std::string expression = parts.size() == 1 ? parts[0]
                                                 : "vec" + std::to_string(parts.size()) + "(";
      if (parts.size() > 1) {
        for (size_t i = 0; i < parts.size(); i++) {
          if (i != 0) expression += ", ";
          expression += parts[i];
        }
        expression += ")";
      }
      WriteDest(instruction, operands[0], expression);
      break;
    }

    // ---- texture fetches ----------------------------------------------------
    case OP_SAMPLE: emitSample("texture", "textureOffset", -1, false); break;
    case OP_SAMPLE_L: emitSample("textureLod", "textureLodOffset", 4, false); break;
    case OP_SAMPLE_B:
      if (_program.stage != Stage::Pixel) {
        Fail("sample_b outside a fragment program has no GLSL ES form");
        return false;
      }
      emitSample("texture", "textureOffset", 4, false);
      break;
    case OP_SAMPLE_C: emitSample("texture", "textureOffset", -1, true); break;
    case OP_SAMPLE_C_LZ:
      // There is no textureLod for a 2D shadow sampler in GLSL ES, and
      // sample_c_lz always reads mip zero. In a fragment shader the ordinary
      // fetch is the closest thing and is what Unity's own translator emits.
      emitSample("texture", "textureOffset", -1, true);
      break;
    case OP_SAMPLE_D: {
      if (operands.size() < 6) {
        Fail("sample_d with too few operands");
        return false;
      }
      Operand const& resource = operands[2];
      MappedTexture const* texture = TextureFor(resource);
      if (texture == nullptr) return false;
      if (texture->comparison) {
        Fail("sample_d on a shadow map has no GLSL ES form");
        return false;
      }
      uint8_t const coordinateMask =
          static_cast<uint8_t>((1u << texture->coordinateComponents) - 1u);
      // The gradients have one component per addressable dimension, which for
      // an array texture is one fewer than the coordinate.
      int gradientComponents = texture->coordinateComponents;
      if (texture->dimension == 5 || texture->dimension == 7) gradientComponents = 2;
      if (texture->dimension == 10) gradientComponents = 3;
      uint8_t const gradientMask = static_cast<uint8_t>((1u << gradientComponents) - 1u);
      std::string call = instruction.hasSampleOffsets ? "textureGradOffset(" : "textureGrad(";
      call += texture->name + ", " + SrcFloat(operands[1], coordinateMask) + ", " +
              SrcFloat(operands[4], gradientMask) + ", " + SrcFloat(operands[5], gradientMask);
      call += SampleOffsetArgument(instruction, gradientComponents);
      call += ")";
      WriteDest(instruction, operands[0], call + ResourceSwizzle(resource, destMask()));
      break;
    }
    case OP_GATHER4: emitGather(false); break;
    case OP_GATHER4_C: emitGather(true); break;
    case OP_GATHER4_PO:
    case OP_GATHER4_PO_C:
      // A per-fetch programmable offset is not a constant expression, and GLSL
      // ES only guarantees textureGatherOffset for constant ones.
      Fail("gather4 with a programmable offset has no GLSL ES form");
      return false;
    case OP_LOD:
      // textureQueryLod is desktop GLSL only.
      Fail("lod (texture level-of-detail query) has no GLSL ES form");
      return false;
    case OP_LD:
    case OP_LD_MS: {
      size_t const resourceIndex = opcode == OP_LD_MS ? 2u : 2u;
      if (operands.size() <= resourceIndex) {
        Fail(OpcodeName(opcode) + " with too few operands");
        return false;
      }
      Operand const& resource = operands[resourceIndex];
      MappedTexture const* texture = TextureFor(resource);
      if (texture == nullptr) return false;
      if (texture->comparison || texture->dimension == 9 || texture->dimension == 10) {
        Fail("a texel fetch from '" + texture->name +
             "' is on a shape GLSL ES cannot texelFetch");
        return false;
      }
      if (opcode == OP_LD_MS && !Require(310, "a multisample texel fetch")) return false;
      int const coordinateComponents = texture->coordinateComponents;
      uint8_t const coordinateMask = static_cast<uint8_t>((1u << coordinateComponents) - 1u);
      std::string call = instruction.hasSampleOffsets ? "texelFetchOffset(" : "texelFetch(";
      call += texture->name + ", " + VecType(coordinateComponents, "i") + "(" +
              SrcInt(operands[1], coordinateMask) + ")";
      if (opcode == OP_LD_MS) {
        // The sample index is a separate operand; a multisample fetch has no
        // mip level.
        call += ", " + ScalarInt(operands[3]);
      } else if (texture->dimension != 1) {
        // A buffer texture has no mip chain, and texelFetch takes no level for
        // one. Every other shape carries the level in the coordinate's w.
        call += ", " + SrcInt(operands[1], 0x8);
      }
      int offsetComponents = coordinateComponents;
      if (texture->dimension == 5 || texture->dimension == 7) offsetComponents = 2;
      call += SampleOffsetArgument(instruction, offsetComponents);
      call += ")";
      WriteDest(instruction, operands[0], call + ResourceSwizzle(resource, destMask()));
      break;
    }
    case OP_RESINFO: {
      if (operands.size() < 3) {
        Fail("resinfo with too few operands");
        return false;
      }
      Operand const& resource = operands[2];
      MappedTexture const* texture = TextureFor(resource);
      if (texture == nullptr) return false;
      int sizeComponents = 0;
      switch (texture->dimension) {
        case 4: case 9: case 6: sizeComponents = 2; break;
        case 5: case 8: case 7: sizeComponents = 3; break;
        case 10: sizeComponents = 3; break;
        case 1: sizeComponents = 1; break;
        default: break;
      }
      if (sizeComponents == 0) {
        Fail("resinfo on a texture whose dimension has no GLSL ES textureSize");
        return false;
      }
      // resinfo returns width, height, depth/elements, mip count. GLSL ES has
      // no query for the mip count, so a shader that reads .w gets 1 rather
      // than a wrong number.
      std::string const size = texture->multisample || texture->dimension == 1
                                   ? "textureSize(" + texture->name + ")"
                                   : "textureSize(" + texture->name + ", " +
                                         ScalarInt(operands[1]) + ")";
      std::string components[4];
      for (int i = 0; i < 4; i++) {
        if (i < sizeComponents) {
          components[i] = sizeComponents == 1 ? size : size + "." + kComponentNames[i];
        } else {
          components[i] = i == 3 ? "1" : "0";
        }
      }
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      std::vector<std::string> parts;
      for (int i = 0; i < 4; i++) {
        if (!(mask & (1u << i))) continue;
        parts.push_back(components[resource.swizzle[i] & 0x3]);
      }
      uint32_t const returnMode = instruction.controls & 0x3u;
      std::string expression;
      char const* prefix = returnMode == 1 ? "i" : "";
      if (count == 1) {
        expression = VecType(1, prefix) + "(" + parts[0] + ")";
      } else {
        expression = VecType(count, prefix) + "(";
        for (size_t i = 0; i < parts.size(); i++) {
          if (i != 0) expression += ", ";
          expression += parts[i];
        }
        expression += ")";
      }
      if (returnMode == 1) {          // resinfo_uint
        expression = "intBitsToFloat(" + expression + ")";
      } else if (returnMode == 2) {   // resinfo_rcpFloat
        expression = "(" + broadcast("1.0", count, "") + " / " + expression + ")";
      }
      WriteDest(instruction, operands[0], expression);
      break;
    }
    case OP_BUFINFO: {
      if (operands.size() < 2) {
        Fail("bufinfo with too few operands");
        return false;
      }
      if (!Require(310, "a storage buffer length query")) return false;
      MappedStorage const* storage = StorageFor(operands[1]);
      if (storage == nullptr) return false;
      if (storage->typedImage) {
        Fail("bufinfo on a typed image has no GLSL ES form");
        return false;
      }
      // The array is declared as uints, so its length is in dwords; D3D wants
      // elements for a structured buffer and bytes for a raw one.
      std::string length = storage->name + ".length()";
      if (storage->stride > 4) {
        length = "(" + length + " / " + std::to_string(storage->stride / 4u) + ")";
      } else if (storage->stride == 0) {
        length = "(" + length + " * 4)";
      }
      WriteDest(instruction, operands[0],
                "intBitsToFloat(" + broadcast("int(" + length + ")", PopCount4(destMask()), "i") +
                    ")");
      break;
    }
    case OP_SAMPLE_INFO:
    case OP_SAMPLE_POS:
    case OP_EVAL_SNAPPED:
    case OP_EVAL_SAMPLE_INDEX:
    case OP_EVAL_CENTROID:
      Fail("'" + OpcodeName(opcode) +
           "' needs per-sample evaluation, which GLSL ES does not provide");
      return false;

    // ---- read/write buffers and images --------------------------------------
    case OP_LD_RAW: emitBufferLoad(false); break;
    case OP_LD_STRUCTURED: emitBufferLoad(true); break;
    case OP_STORE_RAW: emitBufferStore(false); break;
    case OP_STORE_STRUCTURED: emitBufferStore(true); break;
    case OP_LD_UAV_TYPED:
    case OP_STORE_UAV_TYPED: {
      if (operands.size() < 3) {
        Fail(OpcodeName(opcode) + " with too few operands");
        return false;
      }
      if (!Require(310, "a read/write image")) return false;
      bool const isStore = opcode == OP_STORE_UAV_TYPED;
      Operand const& imageOperand = isStore ? operands[0] : operands[2];
      MappedStorage const* storage = StorageFor(imageOperand);
      if (storage == nullptr) return false;
      if (!storage->typedImage) {
        Fail("'" + storage->name + "' is accessed as an image but is not a typed resource");
        return false;
      }
      int coordinateComponents = 0;
      switch (storage->dimension) {
        case 4: coordinateComponents = 2; break;
        case 5: case 8: coordinateComponents = 3; break;
        default: break;
      }
      if (coordinateComponents == 0) {
        Fail("a read/write image of a shape GLSL ES has no coordinate form for");
        return false;
      }
      uint8_t const coordinateMask = static_cast<uint8_t>((1u << coordinateComponents) - 1u);
      std::string const coordinate = VecType(coordinateComponents, "i") + "(" +
                                     SrcInt(operands[1], coordinateMask) + ")";
      if (isStore) {
        Line("imageStore(" + storage->name + ", " + coordinate + ", " +
             SrcFloat(operands[2], 0xf) + ");");
        break;
      }
      WriteDest(instruction, operands[0],
                "imageLoad(" + storage->name + ", " + coordinate + ")" +
                    ResourceSwizzle(operands[2], destMask()));
      break;
    }
    case OP_SYNC: {
      if (!Require(310, "a compute barrier")) return false;
      // The controls say which memory is being synchronised and whether the
      // group is being joined; GLSL ES splits that across three calls.
      uint32_t const flags = instruction.controls;
      if (flags & 0x1u) Line("groupMemoryBarrier();");      // TGSM
      if (flags & 0x2u) Line("groupMemoryBarrier();");      // TGSM group
      if (flags & 0x4u) Line("memoryBarrierBuffer();");     // UAV group
      if (flags & 0x8u) Line("memoryBarrier();");           // UAV global
      if (flags & 0x10u) Line("barrier();");                // thread group sync
      if (flags == 0) Line("memoryBarrier();");
      break;
    }

    // ---- double precision ---------------------------------------------------
    case OP_DADD: case OP_DMAX: case OP_DMIN: case OP_DMUL: case OP_DEQ: case OP_DGE:
    case OP_DLT: case OP_DNE: case OP_DMOV: case OP_DMOVC: case OP_DTOF: case OP_FTOD:
    case OP_DDIV: case OP_DFMA: case OP_DRCP: case OP_DTOI: case OP_DTOU: case OP_ITOD:
    case OP_UTOD:
      Fail("'" + OpcodeName(opcode) + "' is double-precision, which GLSL ES does not have");
      return false;

    // ---- control flow -------------------------------------------------------
    case OP_IF: {
      bool const testNonZero = ((instruction.controls >> 7) & 0x1u) != 0;
      Line("if (" + ScalarInt(operands[0]) + (testNonZero ? " != 0) {" : " == 0) {"));
      _indent++;
      break;
    }
    case OP_ELSE:
      _indent = std::max(1, _indent - 1);
      Line("} else {");
      _indent++;
      break;
    case OP_ENDIF:
    case OP_ENDLOOP:
    case OP_ENDSWITCH:
      _indent = std::max(1, _indent - 1);
      Line("}");
      break;
    case OP_LOOP:
      Line("while (true) {");
      _indent++;
      break;
    case OP_BREAK: Line("break;"); break;
    case OP_CONTINUE: Line("continue;"); break;
    case OP_RET: Line("return;"); break;
    case OP_BREAKC:
    case OP_CONTINUEC:
    case OP_RETC:
    case OP_DISCARD: {
      bool const testNonZero = ((instruction.controls >> 7) & 0x1u) != 0;
      char const* action = opcode == OP_BREAKC      ? "break;"
                           : opcode == OP_CONTINUEC ? "continue;"
                           : opcode == OP_RETC      ? "return;"
                                                    : "discard;";
      Line("if (" + ScalarInt(operands[0]) + (testNonZero ? " != 0) " : " == 0) ") + action);
      break;
    }
    case OP_SWITCH:
      Line("switch (" + ScalarInt(operands[0]) + ") {");
      _indent++;
      break;
    case OP_CASE:
      _indent = std::max(1, _indent - 1);
      Line("case " + ScalarInt(operands[0]) + ":");
      _indent++;
      break;
    case OP_DEFAULT:
      _indent = std::max(1, _indent - 1);
      Line("default:");
      _indent++;
      break;
    case OP_LABEL:
      // Handled by EmitBody, which splits the stream into subroutines before
      // any of it reaches here.
      break;
    case OP_CALL:
    case OP_CALLC: {
      size_t const labelIndex = opcode == OP_CALLC ? 1u : 0u;
      if (operands.size() <= labelIndex || operands[labelIndex].indices.empty()) {
        Fail("a call with no label");
        return false;
      }
      std::string const target =
          "subroutine" + std::to_string(operands[labelIndex].indices.back().immediate) + "();";
      if (opcode == OP_CALL) {
        Line(target);
        break;
      }
      bool const testNonZero = ((instruction.controls >> 7) & 0x1u) != 0;
      Line("if (" + ScalarInt(operands[0]) + (testNonZero ? " != 0) " : " == 0) ") + target);
      break;
    }

    // ---- geometry shader output ---------------------------------------------
    case OP_EMIT:
    case OP_EMIT_STREAM:
      Line("EmitVertex();");
      break;
    case OP_CUT:
    case OP_CUT_STREAM:
      Line("EndPrimitive();");
      break;
    case OP_EMITTHENCUT:
    case OP_EMITTHENCUT_STREAM:
      Line("EmitVertex();");
      Line("EndPrimitive();");
      break;

    case OP_ABORT:
    case OP_DEBUG_BREAK:
      // Debug-only instructions with no runtime meaning.
      break;

    // ---- atomics -------------------------------------------------------------
    case OP_ATOMIC_IADD: emitAtomic("atomicAdd", false); break;
    case OP_ATOMIC_AND: emitAtomic("atomicAnd", false); break;
    case OP_ATOMIC_OR: emitAtomic("atomicOr", false); break;
    case OP_ATOMIC_XOR: emitAtomic("atomicXor", false); break;
    case OP_ATOMIC_UMAX: emitAtomic("atomicMax", false); break;
    case OP_ATOMIC_UMIN: emitAtomic("atomicMin", false); break;
    case OP_ATOMIC_CMP_STORE: emitAtomic("atomicCompSwap", false); break;
    case OP_IMM_ATOMIC_IADD: emitAtomic("atomicAdd", true); break;
    case OP_IMM_ATOMIC_AND: emitAtomic("atomicAnd", true); break;
    case OP_IMM_ATOMIC_OR: emitAtomic("atomicOr", true); break;
    case OP_IMM_ATOMIC_XOR: emitAtomic("atomicXor", true); break;
    case OP_IMM_ATOMIC_EXCH: emitAtomic("atomicExchange", true); break;
    case OP_IMM_ATOMIC_CMP_EXCH: emitAtomic("atomicCompSwap", true); break;
    case OP_IMM_ATOMIC_UMAX: emitAtomic("atomicMax", true); break;
    case OP_IMM_ATOMIC_UMIN: emitAtomic("atomicMin", true); break;
    case OP_ATOMIC_IMAX:
    case OP_ATOMIC_IMIN:
    case OP_IMM_ATOMIC_IMAX:
    case OP_IMM_ATOMIC_IMIN:
      // A signed atomic min/max needs an int-typed storage buffer; these are
      // declared as uints because every other access reads them as bits.
      Fail("'" + OpcodeName(opcode) + "' needs a signed storage buffer, and this shader's "
           "buffers are declared unsigned");
      return false;
    case OP_IMM_ATOMIC_ALLOC:
    case OP_IMM_ATOMIC_CONSUME:
      Fail("append/consume buffers have no GLSL ES form");
      return false;
    case OP_MSAD:
      Fail("'msad' (sum of absolute differences) has no GLSL ES form");
      return false;

    default:
      Fail("instruction '" + OpcodeName(opcode) + "' is outside the translated subset");
      return false;
  }
  return !_failed;
}

}  // namespace

namespace {

// Splits the instruction stream into main() and one function per label, then
// emits each.
//
// DXBC puts subroutines in the same stream as the main body: everything before
// the first `label` is main, and each `label` starts a routine that runs to the
// next one. `call` jumps to a label and `ret` comes back. Emitting the stream
// as one block would run every subroutine inline, in order, which is not what
// any of it means.
bool GlslEmitter::EmitBody() {
  auto const& instructions = _program.instructions;

  // Forward declarations first: a subroutine can call one declared after it.
  std::vector<uint64_t> labels;
  for (auto const& instruction : instructions) {
    if (instruction.opcode != OP_LABEL) continue;
    if (instruction.operands.empty() || instruction.operands[0].indices.empty()) {
      Fail("a label with no index");
      return false;
    }
    labels.push_back(instruction.operands[0].indices.back().immediate);
  }
  for (uint64_t label : labels) {
    _functions += "void subroutine" + std::to_string(label) + "();\n";
  }

  size_t index = 0;
  _target = &_body;
  _indent = 1;
  for (; index < instructions.size(); index++) {
    if (instructions[index].opcode == OP_LABEL) break;
    if (!EmitInstruction(instructions[index])) return false;
    if (_failed) return false;
  }

  for (; index < instructions.size(); index++) {
    Instruction const& instruction = instructions[index];
    if (instruction.opcode == OP_LABEL) {
      if (instruction.operands.empty() || instruction.operands[0].indices.empty()) {
        Fail("a label with no index");
        return false;
      }
      _functions += "void subroutine" +
                    std::to_string(instruction.operands[0].indices.back().immediate) + "() {\n";
      _subroutineBody.clear();
      _target = &_subroutineBody;
      _indent = 1;
      // Emit this routine's instructions, then close it.
      size_t next = index + 1;
      for (; next < instructions.size(); next++) {
        if (instructions[next].opcode == OP_LABEL) break;
        if (!EmitInstruction(instructions[next])) return false;
        if (_failed) return false;
      }
      _functions += _subroutineBody;
      _functions += "}\n";
      index = next - 1;
      continue;
    }
    // Unreachable: the loop above only stops on a label.
    Fail("an instruction outside both main and a subroutine");
    return false;
  }
  _target = &_body;
  return true;
}

GlslResult GlslEmitter::Run() {
  GlslResult result;
  if (!_program.ok) {
    result.error = _program.error.empty() ? "the program did not parse" : _program.error;
    return result;
  }
  if (_program.majorVersion < 4 || _program.majorVersion > 5) {
    result.error = "shader model " + std::to_string(_program.majorVersion) + "." +
                   std::to_string(_program.minorVersion) + " is not shader model 4 or 5";
    return result;
  }

  if (!BuildSignatures() || !BuildStageDeclarations() || !BuildConstantBuffers() ||
      !BuildResources()) {
    result.error = _error;
    return result;
  }
  if (!EmitBody()) {
    result.error = _error.empty() ? "translation stopped without a reason" : _error;
    return result;
  }

  std::string prologue;
  auto addPrologue = [&prologue](std::string const& line) { prologue += "  " + line + "\n"; };
  if (_program.tempCount > 0) {
    if (_program.tempCount > 4096) {
      result.error = "the shader declares " + std::to_string(_program.tempCount) +
                     " temporary registers";
      return result;
    }
    std::string line = "vec4 ";
    for (uint32_t i = 0; i < _program.tempCount; i++) {
      if (i != 0) line += ", ";
      line += "r" + std::to_string(i);
    }
    addPrologue(line + ";");
  }
  for (auto const& temp : _program.indexableTemps) {
    if (temp.arraySize == 0 || temp.arraySize > 65536) {
      result.error = "indexable temp x" + std::to_string(temp.index) + " has an impossible size";
      return result;
    }
    addPrologue("vec4 x" + std::to_string(temp.index) + "[" + std::to_string(temp.arraySize) + "];");
  }
  // The built-in aliases. Every one of these is a GLSL built-in of some scalar
  // or integer type, and the register model reads registers as vec4s, so each
  // is bit-cast into one here instead of at every use.
  if (_usedFrontFace) {
    // HLSL's front-face input is a bool that the bytecode reads as an
    // all-bits-set integer, which is not what a GLSL bool converts to.
    addPrologue("vec4 vFrontFace = vec4(intBitsToFloat(gl_FrontFacing ? -1 : 0));");
  }
  if (_usedVertexID) addPrologue("vec4 vVertexID = intBitsToFloat(ivec4(gl_VertexID));");
  if (_usedInstanceID) addPrologue("vec4 vInstanceID = intBitsToFloat(ivec4(gl_InstanceID));");
  if (_usedPrimitiveID) addPrologue("vec4 vPrimitiveID = intBitsToFloat(ivec4(gl_PrimitiveID));");
  if (_usedSampleIndex) addPrologue("vec4 vSampleIndex = intBitsToFloat(ivec4(gl_SampleID));");
  if (_usedGsInstanceID) {
    addPrologue("vec4 vGsInstanceID = intBitsToFloat(ivec4(gl_InvocationID));");
  }
  if (_usedThreadID) {
    addPrologue("vec4 vThreadID = uintBitsToFloat(uvec4(gl_GlobalInvocationID, 0u));");
  }
  if (_usedThreadGroupID) {
    addPrologue("vec4 vThreadGroupID = uintBitsToFloat(uvec4(gl_WorkGroupID, 0u));");
  }
  if (_usedThreadIDInGroup) {
    addPrologue("vec4 vThreadIDInGroup = uintBitsToFloat(uvec4(gl_LocalInvocationID, 0u));");
  }
  if (_usedThreadIDFlattened) {
    addPrologue("vec4 vThreadIDInGroupFlattened = uintBitsToFloat(uvec4(gl_LocalInvocationIndex));");
  }

  std::string immediateBuffer;
  if (_usedImmediateConstantBuffer) {
    if (_program.immediateConstantBuffer.empty() ||
        _program.immediateConstantBuffer.size() % 4 != 0) {
      result.error = "the shader reads an immediate constant buffer it does not declare";
      return result;
    }
    size_t const rows = _program.immediateConstantBuffer.size() / 4;
    immediateBuffer = "const vec4 ImmCB[" + std::to_string(rows) + "] = vec4[" +
                      std::to_string(rows) + "](\n";
    for (size_t row = 0; row < rows; row++) {
      immediateBuffer += "  vec4(";
      for (size_t component = 0; component < 4; component++) {
        if (component != 0) immediateBuffer += ", ";
        immediateBuffer += FormatFloatLiteral(_program.immediateConstantBuffer[row * 4 + component]);
      }
      immediateBuffer += row + 1 == rows ? ")\n" : "),\n";
    }
    immediateBuffer += ");\n";
  }

  std::string source;
  source += "#version " + std::to_string(_version) + " es\n";
  source += "precision highp float;\n";
  source += "precision highp int;\n";
  source += _declarations;
  source += immediateBuffer;
  // Subroutines have to be able to reach the temps, and DXBC gives them one
  // shared register file rather than a stack frame, so the registers are file
  // scope and main() only initialises what needs initialising.
  if (!_functions.empty()) {
    source += prologue;
    source += _functions;
    source += "void main() {\n";
    source += _body;
    source += "}\n";
  } else {
    source += "void main() {\n";
    source += prologue;
    source += _body;
    source += "}\n";
  }

  result.ok = true;
  result.source = std::move(source);
  result.uniforms = _uniformNames;
  result.samplers = _samplerList;
  result.version = _version;
  return result;
}

}  // namespace

GlslResult TranslateToGlsl(Program const& program, GlslOptions const& options) {
  GlslEmitter emitter(program, options);
  return emitter.Run();
}

GlslResult TranslateDxbcToGlsl(uint8_t const* data, size_t size, GlslOptions const& options) {
  Program program = ParseProgram(data, size);
  if (!program.ok) {
    GlslResult result;
    result.error = program.error;
    return result;
  }
  return TranslateToGlsl(program, options);
}

}  // namespace Vivify::Dxbc
