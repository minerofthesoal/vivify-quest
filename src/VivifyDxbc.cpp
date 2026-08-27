#include "VivifyDxbc.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

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
  OP_COUNT = 216,
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
      instruction.extra.push_back(extendedToken);
      extended = (extendedToken & 0x80000000u) != 0;
    }
    // Extended tokens are recorded separately from the trailing dwords below.
    size_t const extendedTokens = instruction.extra.size();

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
        if (instruction.extra.size() > extendedTokens) {
          program.tempCount = std::max(program.tempCount, instruction.extra[extendedTokens]);
        }
        break;
      case OP_DCL_INDEXABLE_TEMP:
        if (instruction.extra.size() >= extendedTokens + 3) {
          Program::IndexableTemp temp;
          temp.index = instruction.extra[extendedTokens];
          temp.arraySize = instruction.extra[extendedTokens + 1];
          temp.components = instruction.extra[extendedTokens + 2];
          program.indexableTemps.push_back(temp);
        }
        break;
      case OP_DCL_GLOBAL_FLAGS:
        program.globalFlags = instruction.controls;
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
    if (instruction.saturate) out += "_sat";
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

namespace {

std::string FormatFloatLiteral(uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, 4);
  // A NaN or infinity cannot be written as a GLSL literal at all. They do turn
  // up in real shaders as "unused component" filler, so they become zero rather
  // than stopping the translation -- but only when nothing reads them would
  // that be safe, and that is not knowable here, so it is recorded as a
  // literal 0.0 and the shader is still emitted.
  if (!(value == value) || value > 3.4e38f || value < -3.4e38f) {
    return value > 0.0f ? "3.402823466e+38" : (value < 0.0f ? "-3.402823466e+38" : "0.0");
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
  std::string text = buffer;
  if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
      text.find("inf") == std::string::npos && text.find("nan") == std::string::npos) {
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

class GlslEmitter {
 public:
  GlslEmitter(Program const& program, GlslOptions const& options)
      : _program(program), _options(options) {}

  GlslResult Run();

 private:
  // Anything that cannot be translated stops the whole shader. The first reason
  // is the one reported: later ones are usually consequences of it.
  void Fail(std::string reason) {
    if (_failed) return;
    _failed = true;
    _error = std::move(reason);
  }

  bool BuildConstantBuffers();
  bool BuildResources();
  bool BuildSignatures();
  bool EmitBody();
  bool EmitInstruction(Instruction const& instruction);

  std::string SrcBase(Operand const& operand, uint8_t mask);
  std::string SrcFloat(Operand const& operand, uint8_t mask);
  std::string SrcInt(Operand const& operand, uint8_t mask);
  std::string SrcUint(Operand const& operand, uint8_t mask);
  std::string ScalarFloat(Operand const& operand);  // first selected component
  std::string ScalarInt(Operand const& operand);
  std::string RegisterName(Operand const& operand);
  std::string ConstantComponent(Operand const& operand, int component);
  std::string DestName(Operand const& operand, uint8_t& mask);
  void WriteDest(Instruction const& instruction, Operand const& dest, std::string const& expression);
  void Line(std::string const& text);

  SignatureElement const* FindSignature(std::vector<SignatureElement> const& signature,
                                        uint32_t registerIndex) const;
  std::string VaryingName(SignatureElement const& element, bool vertexInput) const;

  Program const& _program;
  GlslOptions _options;

  std::string _declarations;
  std::string _body;
  int _indent = 1;
  bool _failed = false;
  std::string _error;

  std::vector<std::vector<MappedVariable>> _constantBuffers;  // by cb bind point
  std::vector<std::string> _samplerNames;                     // by t# register
  std::vector<uint32_t> _samplerDimensions;                   // by t# register
  std::vector<std::string> _uniformNames;
  std::vector<std::string> _samplerList;
  bool _usedImmediateConstantBuffer = false;
};

void GlslEmitter::Line(std::string const& text) {
  _body.append(static_cast<size_t>(_indent) * 2, ' ');
  _body += text;
  _body += '\n';
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
// A translated shader that named them anything else would link but receive
// nothing.
std::string GlslEmitter::VaryingName(SignatureElement const& element, bool vertexInput) const {
  std::string name = element.semanticName;
  for (auto& c : name) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '_';
  }
  return (vertexInput ? "in_" : "vs_") + name + std::to_string(element.semanticIndex);
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
      entry.scalarDeclaration = false;

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
        entry.componentCount = static_cast<int>(columns);
        entry.scalarDeclaration = columns == 1;
        entry.declaration =
            "uniform " + VecType(static_cast<int>(columns), prefix) + " " + entry.name + ";";
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
  for (auto const& binding : _program.resourceBindings) {
    if (binding.type != 2) continue;  // D3D_SIT_TEXTURE
    if (binding.bindPoint > 64) {
      Fail("texture bound at register t" + std::to_string(binding.bindPoint));
      return false;
    }
    if (_samplerNames.size() <= binding.bindPoint) {
      _samplerNames.resize(binding.bindPoint + 1);
      _samplerDimensions.resize(binding.bindPoint + 1, 0);
    }
    char const* samplerType = nullptr;
    switch (binding.dimension) {
      case 4: samplerType = "sampler2D"; break;
      case 5: samplerType = "sampler2DArray"; break;
      case 8: samplerType = "sampler3D"; break;
      case 9: samplerType = "samplerCube"; break;
      default: break;
    }
    if (samplerType == nullptr) {
      Fail("texture '" + binding.name + "' has a dimension GLSL ES has no sampler for (" +
           std::to_string(binding.dimension) + ")");
      return false;
    }
    // D3D separates textures from samplers; GLSL ES 3.00 has only the combined
    // form. Unity resolves this the same way -- one combined sampler named
    // after the texture -- so a material's _MainTex still binds.
    _samplerNames[binding.bindPoint] = binding.name;
    _samplerDimensions[binding.bindPoint] = binding.dimension;
    _declarations += "uniform highp " + std::string(samplerType) + " " + binding.name + ";\n";
    _samplerList.push_back(binding.name);
  }
  return !_failed;
}

bool GlslEmitter::BuildSignatures() {
  bool const isVertex = _program.stage == Stage::Vertex;
  bool const isPixel = _program.stage == Stage::Pixel;
  if (!isVertex && !isPixel) {
    Fail("only vertex and fragment programs are translated, this is a " +
         std::string(StageName(_program.stage)) + " program");
    return false;
  }

  for (auto const& element : _program.inputSignature) {
    // System-value inputs come from GLSL's own built-ins rather than a
    // declared variable.
    if (isPixel && element.systemValueType == 1) continue;   // SV_Position -> gl_FragCoord
    if (element.systemValueType == 9) continue;              // SV_IsFrontFace
    if (element.systemValueType != 0) {
      Fail("input semantic '" + element.semanticName + "' is a system value this translator has "
           "no GLSL equivalent for");
      return false;
    }
    std::string const name = VaryingName(element, isVertex);
    std::string const qualifier = isVertex ? "in " : "in ";
    _declarations += qualifier + "vec4 " + name + ";\n";
  }

  for (auto const& element : _program.outputSignature) {
    if (isVertex && element.systemValueType == 1) continue;  // SV_Position -> gl_Position
    if (isPixel) {
      // A pixel shader's outputs are render targets; SV_Depth arrives as an
      // operand type rather than a signature entry.
      if (element.systemValueType != 0) {
        Fail("output semantic '" + element.semanticName + "' is a system value this translator "
             "has no GLSL equivalent for");
        return false;
      }
      _declarations += "layout(location = " + std::to_string(element.registerIndex) +
                       ") out vec4 " + element.semanticName +
                       std::to_string(element.semanticIndex) + ";\n";
      continue;
    }
    if (element.systemValueType != 0) {
      Fail("vertex output semantic '" + element.semanticName +
           "' is a system value this translator has no GLSL equivalent for");
      return false;
    }
    _declarations += "out vec4 " + VaryingName(element, false) + ";\n";
  }
  return !_failed;
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
      uint32_t const registerIndex = static_cast<uint32_t>(operand.indices.back().immediate);
      auto const* element = FindSignature(_program.inputSignature, registerIndex);
      if (element == nullptr) {
        Fail("input register v" + std::to_string(registerIndex) + " is not in the input signature");
        return {};
      }
      if (_program.stage == Stage::Pixel && element->systemValueType == 1) return "gl_FragCoord";
      if (element->systemValueType == 9) return "vFrontFace";
      if (element->semanticName == "SV_VertexID") return "vVertexID";
      if (element->semanticName == "SV_InstanceID") return "vInstanceID";
      return VaryingName(*element, _program.stage == Stage::Vertex);
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
      if (_program.stage == Stage::Vertex) {
        if (element->systemValueType == 1) return "gl_Position";
        return VaryingName(*element, false);
      }
      return element->semanticName + std::to_string(element->semanticIndex);
    }
    case kOperandOutputDepth:
    case kOperandOutputDepthGreaterEqual:
    case kOperandOutputDepthLessEqual:
      return "gl_FragDepth";
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
  if (name == "vVertexID" || name == "vInstanceID") {
    return "intBitsToFloat(" + name + ")";
  }
  if (operand.numComponents == 0) return name;

  std::string swizzle = ".";
  for (int i = 0; i < 4; i++) {
    if (!(mask & (1u << i))) continue;
    swizzle += kComponentNames[operand.numComponents == 1 ? 0 : (operand.swizzle[i] & 0x3u)];
  }
  // gl_FragDepth is a bare float; a swizzle on it would not compile.
  if (name == "gl_FragDepth") return name;
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
  uint8_t mask = 0x1;
  if (operand.numComponents == 4) {
    // A scalar read of a four-component operand takes its first swizzle slot.
    mask = 0x1;
  }
  return SrcFloat(operand, mask);
}

std::string GlslEmitter::ScalarInt(Operand const& operand) {
  return SrcInt(operand, 0x1);
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
  std::string swizzle = ".";
  for (int i = 0; i < 4; i++) {
    if (mask & (1u << i)) swizzle += kComponentNames[i];
  }
  Line(name + swizzle + " = " + value + ";");
}

}  // namespace

namespace {

bool GlslEmitter::EmitInstruction(Instruction const& instruction) {
  auto const& operands = instruction.operands;
  uint32_t const opcode = instruction.opcode;

  // Declarations carry no code; everything they say has already been read off
  // the reflection chunks or the program header.
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
    case OP_NOP:
      return true;
    default:
      break;
  }

  auto destMask = [&]() -> uint8_t {
    if (operands.empty()) return 0;
    return operands[0].numComponents == 4 ? operands[0].mask : 0x1;
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

  auto sampleCoordinateMask = [&](uint32_t resourceRegister) -> uint8_t {
    if (resourceRegister >= _samplerDimensions.size()) return 0;
    switch (_samplerDimensions[resourceRegister]) {
      case 4: return 0x3;  // Texture2D
      case 5: return 0x7;  // Texture2DArray
      case 8: return 0x7;  // Texture3D
      case 9: return 0x7;  // TextureCube
      default: return 0;
    }
  };
  // The resource operand's swizzle says which channel of the fetched texel
  // feeds each written component, so t0.yyyy in the bytecode has to become
  // .yyyy on the GLSL fetch.
  auto resourceSwizzle = [&](Operand const& resource, uint8_t mask) {
    std::string swizzle = ".";
    for (int i = 0; i < 4; i++) {
      if (mask & (1u << i)) swizzle += kComponentNames[resource.swizzle[i] & 0x3u];
    }
    return swizzle;
  };
  auto samplerFor = [&](Operand const& resource) -> std::string {
    if (resource.indices.empty()) {
      Fail("a texture fetch with no resource register");
      return {};
    }
    uint32_t const index = static_cast<uint32_t>(resource.indices.back().immediate);
    if (index >= _samplerNames.size() || _samplerNames[index].empty()) {
      Fail("the shader samples t" + std::to_string(index) +
           ", which its reflection data does not name");
      return {};
    }
    return _samplerNames[index];
  };
  auto emitSample = [&](char const* function, int extraOperand) {
    if (operands.size() < 4) {
      Fail("a texture fetch with too few operands");
      return;
    }
    Operand const& resource = operands[2];
    std::string const sampler = samplerFor(resource);
    if (_failed) return;
    uint32_t const resourceRegister = static_cast<uint32_t>(resource.indices.back().immediate);
    uint8_t const coordinateMask = sampleCoordinateMask(resourceRegister);
    if (coordinateMask == 0) {
      Fail("the shader samples t" + std::to_string(resourceRegister) +
           ", whose dimension has no GLSL ES sampler");
      return;
    }
    std::string call = std::string(function) + "(" + sampler + ", " +
                       SrcFloat(operands[1], coordinateMask);
    if (extraOperand >= 0) {
      if (operands.size() <= static_cast<size_t>(extraOperand)) {
        Fail("a texture fetch is missing its lod/bias operand");
        return;
      }
      call += ", " + ScalarFloat(operands[static_cast<size_t>(extraOperand)]);
    }
    call += ")";
    uint8_t const mask = destMask();
    WriteDest(instruction, operands[0], call + resourceSwizzle(resource, mask));
  };

  switch (opcode) {
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
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0], "intBitsToFloat(~" + SrcInt(operands[1], mask) + ")");
      break;
    }
    case OP_INEG: {
      uint8_t const mask = destMask();
      WriteDest(instruction, operands[0], "intBitsToFloat(-" + SrcInt(operands[1], mask) + ")");
      break;
    }
    case OP_IMAD: {
      uint8_t const mask = destMask();
      std::string const a = SrcInt(operands[1], mask);
      std::string const b = SrcInt(operands[2], mask);
      std::string const c = SrcInt(operands[3], mask);
      WriteDest(instruction, operands[0], "intBitsToFloat(" + a + " * " + b + " + " + c + ")");
      break;
    }
    case OP_UMAD: {
      uint8_t const mask = destMask();
      std::string const a = SrcUint(operands[1], mask);
      std::string const b = SrcUint(operands[2], mask);
      std::string const c = SrcUint(operands[3], mask);
      WriteDest(instruction, operands[0], "uintBitsToFloat(" + a + " * " + b + " + " + c + ")");
      break;
    }
    case OP_ITOF: {
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      WriteDest(instruction, operands[0],
                VecType(count, "") + "(" + SrcInt(operands[1], mask) + ")");
      break;
    }
    case OP_UTOF: {
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      WriteDest(instruction, operands[0],
                VecType(count, "") + "(" + SrcUint(operands[1], mask) + ")");
      break;
    }
    case OP_FTOI: {
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      WriteDest(instruction, operands[0],
                "intBitsToFloat(" + VecType(count, "i") + "(" + SrcFloat(operands[1], mask) + "))");
      break;
    }
    case OP_FTOU: {
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      WriteDest(instruction, operands[0],
                "uintBitsToFloat(" + VecType(count, "u") + "(" + SrcFloat(operands[1], mask) + "))");
      break;
    }
    case OP_IMUL:
    case OP_UMUL: {
      // The high half of a 32x32 multiply has no GLSL ES 3.00 form, and a
      // shader that asks for it is doing integer maths this model cannot carry.
      if (operands[0].type != kOperandNull) {
        Fail(OpcodeName(opcode) + " writes the high half of the product, which GLSL ES 3.00 "
             "cannot compute");
        return false;
      }
      uint8_t const mask = operands[1].numComponents == 4 ? operands[1].mask : 0x1;
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
        uint8_t const mask = operands[0].numComponents == 4 ? operands[0].mask : 0x1;
        WriteDest(instruction, operands[0],
                  "uintBitsToFloat(" + SrcUint(operands[2], mask) + " / " +
                      SrcUint(operands[3], mask) + ")");
      }
      if (operands[1].type != kOperandNull) {
        uint8_t const mask = operands[1].numComponents == 4 ? operands[1].mask : 0x1;
        WriteDest(instruction, operands[1],
                  "uintBitsToFloat(" + SrcUint(operands[2], mask) + " % " +
                      SrcUint(operands[3], mask) + ")");
      }
      break;
    }
    case OP_SINCOS: {
      if (operands[0].type != kOperandNull) {
        uint8_t const mask = operands[0].numComponents == 4 ? operands[0].mask : 0x1;
        WriteDest(instruction, operands[0], "sin(" + SrcFloat(operands[2], mask) + ")");
      }
      if (operands[1].type != kOperandNull) {
        uint8_t const mask = operands[1].numComponents == 4 ? operands[1].mask : 0x1;
        WriteDest(instruction, operands[1], "cos(" + SrcFloat(operands[2], mask) + ")");
      }
      break;
    }
    case OP_SAMPLE: emitSample("texture", -1); break;
    case OP_SAMPLE_L: emitSample("textureLod", 4); break;
    case OP_SAMPLE_B:
      if (_program.stage != Stage::Pixel) {
        Fail("sample_b outside a fragment program has no GLSL ES form");
        return false;
      }
      emitSample("texture", 4);
      break;
    case OP_SAMPLE_D: {
      if (operands.size() < 6) {
        Fail("sample_d with too few operands");
        return false;
      }
      Operand const& resource = operands[2];
      std::string const sampler = samplerFor(resource);
      if (_failed) return false;
      uint32_t const resourceRegister = static_cast<uint32_t>(resource.indices.back().immediate);
      uint8_t const coordinateMask = sampleCoordinateMask(resourceRegister);
      if (coordinateMask == 0) {
        Fail("sample_d on a texture whose dimension has no GLSL ES sampler");
        return false;
      }
      uint8_t const mask = destMask();
      std::string const call = "textureGrad(" + sampler + ", " +
                               SrcFloat(operands[1], coordinateMask) + ", " +
                               SrcFloat(operands[4], coordinateMask) + ", " +
                               SrcFloat(operands[5], coordinateMask) + ")";
      WriteDest(instruction, operands[0], call + resourceSwizzle(resource, mask));
      break;
    }
    case OP_LD: {
      if (operands.size() < 3) {
        Fail("ld with too few operands");
        return false;
      }
      Operand const& resource = operands[2];
      std::string const sampler = samplerFor(resource);
      if (_failed) return false;
      uint32_t const resourceRegister = static_cast<uint32_t>(resource.indices.back().immediate);
      uint8_t const coordinateMask = sampleCoordinateMask(resourceRegister);
      if (coordinateMask == 0 || _samplerDimensions[resourceRegister] == 9) {
        Fail("ld on a texture whose dimension GLSL ES cannot texelFetch");
        return false;
      }
      int const coordinateCount = PopCount4(coordinateMask);
      uint8_t const mask = destMask();
      std::string const call = "texelFetch(" + sampler + ", " + VecType(coordinateCount, "i") +
                               "(" + SrcInt(operands[1], coordinateMask) + "), " +
                               SrcInt(operands[1], 0x8) + ")";
      WriteDest(instruction, operands[0], call + resourceSwizzle(resource, mask));
      break;
    }
    case OP_RESINFO: {
      if (operands.size() < 3) {
        Fail("resinfo with too few operands");
        return false;
      }
      Operand const& resource = operands[2];
      std::string const sampler = samplerFor(resource);
      if (_failed) return false;
      uint32_t const resourceRegister = static_cast<uint32_t>(resource.indices.back().immediate);
      uint32_t const dimension = resourceRegister < _samplerDimensions.size()
                                     ? _samplerDimensions[resourceRegister]
                                     : 0u;
      int sizeComponents = 0;
      switch (dimension) {
        case 4: case 9: sizeComponents = 2; break;
        case 5: case 8: sizeComponents = 3; break;
        default: break;
      }
      if (sizeComponents == 0) {
        Fail("resinfo on a texture whose dimension has no GLSL ES textureSize");
        return false;
      }
      // resinfo returns width, height, depth/elements, mip count. GLSL ES has
      // no query for the mip count, so a shader that reads .w gets 1.0 rather
      // than a wrong number.
      std::string size = "textureSize(" + sampler + ", " + ScalarInt(operands[1]) + ")";
      uint32_t const returnMode = (instruction.controls >> 0) & 0x3u;
      std::string components[4];
      for (int i = 0; i < 4; i++) {
        if (i < sizeComponents) {
          components[i] = size + "." + kComponentNames[i];
        } else {
          components[i] = i == 3 ? "1" : "0";
        }
      }
      uint8_t const mask = destMask();
      int const count = PopCount4(mask);
      std::vector<std::string> parts;
      for (int i = 0; i < 4; i++) {
        if (!(mask & (1u << i))) continue;
        int const source = resource.swizzle[i] & 0x3;
        parts.push_back(components[source]);
      }
      std::string expression;
      if (returnMode == 1) {  // resinfo_uint
        expression = count == 1 ? "intBitsToFloat(int(" + parts[0] + "))" : std::string();
        if (count > 1) {
          expression = "intBitsToFloat(ivec" + std::to_string(count) + "(";
          for (size_t i = 0; i < parts.size(); i++) {
            if (i != 0) expression += ", ";
            expression += parts[i];
          }
          expression += "))";
        }
      } else {
        expression = count == 1 ? "float(" + parts[0] + ")" : std::string();
        if (count > 1) {
          expression = "vec" + std::to_string(count) + "(";
          for (size_t i = 0; i < parts.size(); i++) {
            if (i != 0) expression += ", ";
            expression += parts[i];
          }
          expression += ")";
        }
        if (returnMode == 2) expression = "(1.0 / " + expression + ")";  // resinfo_rcpFloat
      }
      WriteDest(instruction, operands[0], expression);
      break;
    }
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
    default:
      Fail("instruction '" + OpcodeName(opcode) + "' is outside the translated subset");
      return false;
  }
  return !_failed;
}

}  // namespace

namespace {

bool GlslEmitter::EmitBody() {
  for (auto const& instruction : _program.instructions) {
    if (!EmitInstruction(instruction)) return false;
    if (_failed) return false;
  }
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

  if (!BuildSignatures() || !BuildConstantBuffers() || !BuildResources()) {
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
  for (auto const& element : _program.inputSignature) {
    if (element.systemValueType == 9) {
      // HLSL's front-face input is a bool that the bytecode reads as an
      // all-bits-set integer, which is not what a GLSL bool converts to.
      addPrologue("vec4 vFrontFace = vec4(intBitsToFloat(gl_FrontFacing ? -1 : 0));");
    } else if (element.semanticName == "SV_VertexID") {
      addPrologue("int vVertexID = gl_VertexID;");
    } else if (element.semanticName == "SV_InstanceID") {
      addPrologue("int vInstanceID = gl_InstanceID;");
    }
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
  source += "#version " + std::to_string(_options.version) + " es\n";
  source += "precision highp float;\n";
  source += "precision highp int;\n";
  source += _declarations;
  source += immediateBuffer;
  source += "void main() {\n";
  source += prologue;
  source += _body;
  source += "}\n";

  result.ok = true;
  result.source = std::move(source);
  result.uniforms = _uniformNames;
  result.samplers = _samplerList;
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
