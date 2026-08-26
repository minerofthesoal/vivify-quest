#pragma once

// Minimal Unity SerializedFile reader: enough to find Shader assets inside a
// bundle and report which GPU platforms their compiled programs were built for.
//
// WHY THIS EXISTS
//
// Every shader in a PC-built Vivify bundle has been assumed to carry DirectX
// bytecode and nothing else, which is why converted maps fall back to stand-in
// shading. That assumption was never actually measured -- the converter only
// ever read the SerializedFile *header* (to patch m_TargetPlatform) and never
// looked at a single object. This reads the object table and the Shader assets
// in it, so the claim can be checked against a real bundle instead of asserted.
//
// It is also the piece any real PC->Quest shader conversion has to be built on:
// you cannot rewrite a shader's programs without first being able to locate and
// decode them.
//
// HOW IT WALKS OBJECTS
//
// Unity asset bundles normally embed a type tree -- a per-type description of
// every field, its size, and its nesting. Rather than hardcode Unity 2021.3's
// Shader layout (which changes between versions and would silently misparse the
// moment it shifted), objects are walked using that tree. Two properties make
// this possible without resolving Unity's built-in "common string" table, whose
// exact byte offsets are version-specific:
//
//   - a node carries its own byteSize, and the array flag tells you when the
//     children are [size, element], so the tree can be *traversed* structurally
//     with no type names at all;
//   - field names that matter here ("platforms") are shader-specific, so they
//     live in the file's own local string buffer and always resolve.
//
// Names that resolve only through the common table come back empty; traversal
// is unaffected, which is the point.
//
// Bundles built with DisableWriteTypeTree carry no tree. That is reported
// rather than guessed at.
//
// Unity-free by design, so it is host-testable.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SerializedFileParse {

// UnityEngine ShaderCompilerPlatform. These are the values stored in a Shader
// asset's `platforms` array, one per compiled program set.
enum : int32_t {
  kShaderPlatformGL = 0,
  kShaderPlatformD3D9 = 1,
  kShaderPlatformD3D11 = 4,
  kShaderPlatformGLES20 = 5,
  kShaderPlatformGLES3Plus = 9,
  kShaderPlatformMetal = 14,
  kShaderPlatformOpenGLCore = 15,
  kShaderPlatformVulkan = 18,
};

// Names a ShaderCompilerPlatform value.
std::string_view ShaderPlatformName(int32_t platform);

// True for a platform whose programs a Quest (Adreno, Android) can execute.
bool ShaderPlatformRunsOnQuest(int32_t platform);

// UnityEngine ShaderGpuProgramType: what one compiled sub-program actually is.
// The platform says which GPU family a blob was built for; this says which
// stage and language the individual program inside it is.
enum : int32_t {
  kGpuProgramUnknown = 0,
  kGpuProgramGLLegacy = 1,
  kGpuProgramGLES31AEP = 2,
  kGpuProgramGLES31 = 3,
  kGpuProgramGLES3 = 4,
  kGpuProgramGLES = 5,
  kGpuProgramGLCore32 = 6,
  kGpuProgramGLCore41 = 7,
  kGpuProgramGLCore43 = 8,
  kGpuProgramDX11VertexSM40 = 15,
  kGpuProgramDX11VertexSM50 = 16,
  kGpuProgramDX11PixelSM40 = 17,
  kGpuProgramDX11PixelSM50 = 18,
  kGpuProgramDX11GeometrySM40 = 19,
  kGpuProgramDX11GeometrySM50 = 20,
  kGpuProgramDX11HullSM50 = 21,
  kGpuProgramDX11DomainSM50 = 22,
  kGpuProgramMetalVS = 23,
  kGpuProgramMetalFS = 24,
  kGpuProgramSPIRV = 25,
};

// Names a ShaderGpuProgramType value.
std::string_view GpuProgramTypeName(int32_t programType);

// True when a program of this type carries GLSL/GLSL ES *source text* rather
// than a compiled binary. This is the property that makes PC -> Quest
// conversion writable at all: for GLES targets Unity stores the shader as text.
bool GpuProgramIsGlslSource(int32_t programType);

// One compiled program out of a shader's blob -- a single stage (vertex,
// fragment, geometry, ...) for a single GPU platform.
struct ShaderSubProgram {
  int32_t platform = 0;     // ShaderCompilerPlatform of the blob it came from
  int32_t blobIndex = 0;    // which sub-blob of that platform
  int32_t programIndex = 0; // position within that sub-blob
  int32_t blobVersion = 0;  // Unity's sub-program format version
  int32_t programType = 0;  // ShaderGpuProgramType
  std::vector<std::string> keywords;
  std::vector<uint8_t> code;  // GLSL source text for GLES targets; DXBC for D3D11
};

struct ShaderObject {
  std::string name;                // empty when the name resolved only via the common string table
  std::vector<int32_t> platforms;  // ShaderCompilerPlatform values, in file order

  // The compressed program store, as laid out by Unity 2019.3+: one group per
  // platform, each holding one entry per sub-blob. Older bundles carry a single
  // flat group, which reads back here as one group.
  std::vector<std::vector<int32_t>> offsets;
  std::vector<std::vector<int32_t>> compressedLengths;
  std::vector<std::vector<int32_t>> decompressedLengths;

  // Where compressedBlob's bytes sit in the buffer InspectSerializedFile was
  // given. Kept as a range rather than a copy: a real bundle's blob runs to
  // megabytes and this code runs on a headset.
  bool blobPresent = false;
  size_t blobFileOffset = 0;
  size_t blobSize = 0;
};

struct DecodeResult {
  bool ok = false;
  std::vector<ShaderSubProgram> programs;
  std::string message;  // why nothing (or only part) came back
};

// Decompresses a shader's blob and splits it into individual programs.
//
// `data`/`size` must be the same buffer that produced `shader`, since the
// shader records its blob as a range into it.
//
// Unity stores the store as LZ4 blocks, one per (platform, sub-blob) pair, each
// decompressing to a small container of [offset, length] program records. Both
// levels are bounds-checked against the buffer they came from: bundles are
// untrusted input and a converted map is written from whatever this returns.
DecodeResult DecodeShaderPrograms(uint8_t const* data, size_t size, ShaderObject const& shader);

// LZ4 block-format decompression, exposed for testing.
//
// Returns the number of bytes written, or 0 for malformed input. This is the
// raw block format (no frame header, no checksum), which is what Unity writes
// into a shader blob.
size_t Lz4DecodeBlock(uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstCapacity);

struct FileReport {
  bool isSerializedFile = false;  // false means this node was a raw stream (.resS), not an error
  bool parsed = false;
  bool typeTreePresent = false;
  int32_t objectCount = 0;
  int32_t shaderObjectCount = 0;
  std::vector<ShaderObject> shaders;
  std::string unityVersion;
  std::string message;
};

// Parses one SerializedFile out of a decompressed bundle node.
//
// Returns isSerializedFile=false for a node that is not a serialized file at
// all, which is normal: bundles carry raw .resS/.resource payload nodes too.
FileReport InspectSerializedFile(uint8_t const* data, size_t size);

}  // namespace SerializedFileParse
