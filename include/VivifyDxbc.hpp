#pragma once

// DirectX shader bytecode -> GLSL ES, the third and last piece of PC -> Quest
// shader conversion.
//
// A PC Vivify bundle stores its shaders as DXBC: the container Microsoft's
// compiler emits, holding Shader Model 4/5 bytecode plus reflection chunks. A
// Quest cannot run any of it. For OpenGL ES targets Unity does not store a
// binary at all -- the shader blob holds GLSL ES *source text* -- so the output
// side is writable. What is missing is the translation, and that is what lives
// here.
//
// Unity's own cross-compiler (HLSLcc) is roughly thirty thousand lines and is
// not what this is. This is a direct translator for Shader Model 4/5: vertex,
// fragment, geometry and compute programs; every arithmetic, bit-manipulation
// and control-flow instruction including subroutines; the whole sampling family
// with texel offsets, shadow samplers and integer samplers; and structured, raw
// and read/write buffers with their atomics.
//
// What it does not do, in each case because a wrong answer would be worse than
// none: tessellation, double precision, per-sample evaluation, msad,
// append/consume buffers, and a geometry shader that passes a semantic straight
// through. Anything outside the subset fails by name rather than by producing
// plausible-looking wrong GLSL: a shader that does not translate keeps the
// stand-in path it has today, which is a worse look but a working frame.
// Silently emitting a shader that compiles and draws the wrong thing would be
// worse than both.
//
// Everything here is fed untrusted bundle bytes, so every read is bounds-checked
// against the buffer it was handed and every count is validated before it is
// used to size anything.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Vivify::Dxbc {

// Which pipeline stage a program is. DXBC records this in the top half of the
// version token rather than in the container.
enum class Stage : uint8_t {
  Pixel = 0,
  Vertex = 1,
  Geometry = 2,
  Hull = 3,
  Domain = 4,
  Compute = 5,
  Unknown = 0xff,
};

std::string_view StageName(Stage stage);

// One entry of an ISGN/OSGN/OSG5 signature chunk: a semantic bound to a
// register. The register index is what the bytecode addresses; the semantic
// name is what the host pipeline binds to.
struct SignatureElement {
  std::string semanticName;
  uint32_t semanticIndex = 0;
  uint32_t systemValueType = 0;  // D3D_NAME_*: 0 = undefined (a plain varying)
  uint32_t componentType = 3;    // 1 = uint, 2 = int, 3 = float
  uint32_t registerIndex = 0;
  uint32_t stream = 0;
  uint8_t mask = 0;    // components this element occupies
  uint8_t rwMask = 0;  // components actually read/written
};

// One variable inside a constant buffer, from the RDEF chunk. Unity names these
// after the material properties they came from (_Color, _MainTex_ST, unity_
// ObjectToWorld), which is exactly what the GLSL side has to declare for the
// engine to bind anything to them.
struct ConstantBufferVariable {
  std::string name;
  uint32_t startOffset = 0;  // bytes from the start of the buffer
  uint32_t size = 0;         // bytes
  uint32_t variableClass = 0;
  uint32_t variableType = 0;
  uint32_t rows = 0;
  uint32_t columns = 0;
  uint32_t elements = 0;
};

struct ConstantBufferInfo {
  std::string name;
  uint32_t size = 0;  // bytes
  uint32_t bindPoint = 0;
  std::vector<ConstantBufferVariable> variables;
};

// A bound resource: a texture, a sampler, or a constant buffer's binding.
struct ResourceBinding {
  std::string name;
  uint32_t type = 0;       // D3D_SHADER_INPUT_TYPE: 0 = cbuffer, 2 = texture, 3 = sampler
  uint32_t returnType = 0;
  uint32_t dimension = 0;  // D3D_SRV_DIMENSION: 2 = Texture2D, 4 = Texture3D, 5 = TextureCube
  uint32_t bindPoint = 0;
  uint32_t bindCount = 0;
};

// Operand types, D3D10_SB_OPERAND_TYPE. Only the ones this translator can act
// on are named; the rest are still parsed and still carry their numeric type,
// so an instruction using one fails with a readable message instead of being
// misread as something else.
enum : uint32_t {
  kOperandTemp = 0,
  kOperandInput = 1,
  kOperandOutput = 2,
  kOperandIndexableTemp = 3,
  kOperandImmediate32 = 4,
  kOperandImmediate64 = 5,
  kOperandSampler = 6,
  kOperandResource = 7,
  kOperandConstantBuffer = 8,
  kOperandImmediateConstantBuffer = 9,
  kOperandLabel = 10,
  kOperandInputPrimitiveID = 11,
  kOperandOutputDepth = 12,
  kOperandNull = 13,
  kOperandRasterizer = 14,
  kOperandOutputCoverageMask = 15,
  kOperandStream = 16,
  kOperandFunctionBody = 17,
  kOperandFunctionTable = 18,
  kOperandInterface = 19,
  kOperandFunctionInput = 20,
  kOperandFunctionOutput = 21,
  kOperandOutputControlPointID = 22,
  kOperandInputForkInstanceID = 23,
  kOperandInputJoinInstanceID = 24,
  kOperandInputControlPoint = 25,
  kOperandOutputControlPoint = 26,
  kOperandInputPatchConstant = 27,
  kOperandInputDomainPoint = 28,
  kOperandThisPointer = 29,
  kOperandUnorderedAccessView = 30,
  kOperandThreadGroupSharedMemory = 31,
  kOperandInputThreadID = 32,
  kOperandInputThreadGroupID = 33,
  kOperandInputThreadIDInGroup = 34,
  kOperandInputCoverageMask = 35,
  kOperandInputThreadIDInGroupFlattened = 36,
  kOperandInputGsInstanceID = 37,
  kOperandOutputDepthGreaterEqual = 38,
  kOperandOutputDepthLessEqual = 39,
  kOperandCycleCounter = 40,
};

// Source modifiers, from an operand's extended token.
enum class Modifier : uint8_t { None = 0, Neg = 1, Abs = 2, AbsNeg = 3 };

// One index of an operand. DXBC operands carry up to three, each either an
// immediate, a register-relative expression, or both added together
// (cb0[r1.x + 3] is the common one).
struct OperandIndex {
  uint64_t immediate = 0;
  bool hasImmediate = false;
  bool hasRelative = false;
  // Set when hasRelative: the operand holding the dynamic part. Held by
  // pointer because an operand can contain an operand.
  std::shared_ptr<struct Operand> relative;
};

struct Operand {
  uint32_t type = kOperandTemp;
  uint32_t numComponents = 4;  // 0, 1 or 4
  uint32_t selectionMode = 0;  // 0 = mask, 1 = swizzle, 2 = select one component
  uint8_t mask = 0xf;          // selectionMode 0
  uint8_t swizzle[4] = {0, 1, 2, 3};
  Modifier modifier = Modifier::None;
  uint32_t minPrecision = 0;
  std::vector<OperandIndex> indices;
  // Populated for kOperandImmediate32: one to four raw 32-bit values, still
  // typeless -- whether they are floats or integers is decided by the
  // instruction that reads them.
  std::vector<uint32_t> immediates;
};

// One decoded instruction. Operands are whatever the opcode takes, in bytecode
// order (destinations first). `extra` carries the raw dwords a declaration
// stores outside its operands (dcl_temps' count, dcl_resource's return-type
// token, and so on).
struct Instruction {
  uint32_t opcode = 0;
  uint32_t controls = 0;  // opcode-specific bits 11..23 of the opcode token
  bool saturate = false;
  bool extended = false;
  uint32_t lengthDwords = 0;
  std::vector<Operand> operands;
  // Trailing dwords the opcode carries outside its operands, and outside its
  // extended tokens: dcl_temps' count, dcl_resource's return-type token,
  // dcl_thread_group's three sizes.
  std::vector<uint32_t> extra;
  // The extended opcode tokens, kept apart from `extra` because they are a
  // different thing that happens to sit next to it. Dropping a sample-offset
  // token silently would translate a texture fetch to the wrong texel.
  std::vector<uint32_t> extendedTokens;
  // Decoded from an extended SAMPLE_CONTROLS token: the compile-time texel
  // offset applied to a fetch.
  bool hasSampleOffsets = false;
  int32_t sampleOffsetU = 0;
  int32_t sampleOffsetV = 0;
  int32_t sampleOffsetW = 0;
  // Byte offset of this instruction's first token inside the bytecode chunk,
  // for error messages that have to point somewhere.
  uint32_t tokenOffset = 0;
};

// A parsed DXBC container: its chunks decoded into the parts a translator needs.
struct Program {
  bool ok = false;
  std::string error;

  Stage stage = Stage::Unknown;
  uint32_t majorVersion = 0;
  uint32_t minorVersion = 0;
  // Where the DXBC container began inside the program bytes. Unity writes its
  // own binding header in front of it, so this is normally not zero.
  size_t containerOffset = 0;

  std::vector<SignatureElement> inputSignature;
  std::vector<SignatureElement> outputSignature;
  std::vector<SignatureElement> patchSignature;
  std::vector<ConstantBufferInfo> constantBuffers;
  std::vector<ResourceBinding> resourceBindings;
  std::string creator;

  std::vector<Instruction> instructions;

  // Filled from the declarations in the instruction stream.
  uint32_t tempCount = 0;
  uint32_t globalFlags = 0;
  // Geometry shaders only: the declared ceiling on emitted vertices, which
  // GLSL needs as a layout qualifier rather than a declaration of its own.
  uint32_t maxOutputVertexCount = 0;
  // Indexable temps: index -> {array size, component count}.
  struct IndexableTemp {
    uint32_t index = 0;
    uint32_t arraySize = 0;
    uint32_t components = 0;
  };
  std::vector<IndexableTemp> indexableTemps;
  // Immediate constant buffer contents (the `icb` block), four dwords per row.
  std::vector<uint32_t> immediateConstantBuffer;
};

// True when `data` starts with a DXBC container header that fits in `size`.
bool LooksLikeDxbc(uint8_t const* data, size_t size);

// Parses a DXBC container. Never throws and never reads outside [data, data+size).
// On failure `ok` is false and `error` says what stopped it.
Program ParseProgram(uint8_t const* data, size_t size);

// Human-readable dump of a parsed program, one line per declaration and
// instruction. This is the form the tests compare against, because a golden
// string is the only way to check a decoder without a GPU.
std::string DisassembleProgram(Program const& program);

// Names an opcode, e.g. "mad", "sample_l". Returns "unknown(NNN)" for anything
// outside the table.
std::string OpcodeName(uint32_t opcode);

// What a translation attempt produced.
struct GlslResult {
  bool ok = false;
  std::string error;        // set when ok is false: which construct stopped it
  std::string source;       // GLSL ES source, when ok
  // Uniform/sampler names the emitted source declares, in declaration order.
  std::vector<std::string> uniforms;
  std::vector<std::string> samplers;
  // The GLSL ES version actually emitted (300, 310 or 320).
  int version = 0;
};

struct GlslOptions {
  // The lowest GLSL ES version to emit. The translator raises this by itself
  // when an instruction needs a later one -- textureGather and uaddCarry are
  // 3.10, geometry shaders 3.20 -- so the default is a floor, not a target.
  // Quest's Adreno parts expose GLES 3.2, and Unity compiles the source on the
  // device at load time, so an escalated version is not a portability problem.
  int version = 300;
  // The highest version the translator may escalate to. A shader needing more
  // than this is refused rather than emitted against a version the device may
  // not have.
  int maximumVersion = 320;
  // Name given to the fragment output when the signature has no name for it.
  std::string defaultFragmentOutput = "SV_Target";
};

// Translates a parsed program to GLSL ES source.
//
// A false `ok` is a normal, expected outcome -- it means this shader uses
// something outside the supported subset -- and callers must treat it as "keep
// the existing stand-in", never as a reason to write a partial program back
// into a bundle.
GlslResult TranslateToGlsl(Program const& program, GlslOptions const& options = {});

// Convenience: parse and translate in one step.
GlslResult TranslateDxbcToGlsl(uint8_t const* data, size_t size, GlslOptions const& options = {});

}  // namespace Vivify::Dxbc
