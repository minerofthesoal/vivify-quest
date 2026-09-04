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
// Everything in a sub-program is kept, including the parts nothing here reads.
//
// A write-back path that dropped the statistics block, or merged the two
// keyword tables, or lost the trailing alignment, would produce a program that
// is *nearly* the one Unity wrote. Decoding and re-encoding an untouched
// program has to give back exactly the bytes it started as, or there is no way
// to tell a conversion bug from a re-encoding bug.
struct ShaderSubProgram {
  int32_t platform = 0;     // ShaderCompilerPlatform of the blob it came from
  int32_t groupIndex = 0;   // which platform group of the store
  int32_t blobIndex = 0;    // which sub-blob of that group
  int32_t programIndex = 0; // position within that sub-blob
  int32_t blobVersion = 0;  // Unity's sub-program format version
  int32_t programType = 0;  // ShaderGpuProgramType
  int32_t entrySize = 8;    // program-table entry width of the sub-blob it came from

  std::vector<uint8_t> stats;              // the fixed block this code steps over
  std::vector<std::string> keywords;
  std::vector<std::string> localKeywords;  // 2018.06 - 2020.12 only
  std::vector<uint8_t> code;               // GLSL source text for GLES targets; DXBC for D3D11
  std::vector<uint8_t> trailing;           // padding after the code, kept verbatim
};

struct ShaderObject {
  int64_t pathID = 0;              // identifies this object inside its SerializedFile
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
  // True when the blob is followed by alignment padding, which a converted
  // blob of a different length has to have recomputed.
  bool blobAligned = false;

  // Where this object's body sits in the buffer that was parsed, and where the
  // int32 tables inside it start (one entry per group, file-absolute, pointing
  // at the group's first element).
  //
  // Conversion does not re-serialize a Shader object. It rewrites the values of
  // tables whose element counts have not changed and splices a new blob in
  // place of the old one, so everything else in the object -- m_ParsedForm, the
  // dependency lists, the fields this parser only walks past -- survives byte
  // for byte. Rebuilding the object from a partial understanding of its layout
  // would lose exactly the parts nothing here reads.
  size_t bodyFileOffset = 0;
  size_t bodySize = 0;
  std::vector<size_t> platformsTableOffsets;
  std::vector<size_t> offsetsTableOffsets;
  std::vector<size_t> compressedLengthsTableOffsets;
  std::vector<size_t> decompressedLengthsTableOffsets;
};

// One Texture2D object, as far as making its pixels reachable on device needs.
//
// A converted PC bundle's textures are BC1/BC3/BC7, which an Adreno cannot
// sample. The mod decodes them on the CPU at level load, but that needs the
// texture's own bytes, and Unity only keeps a CPU copy of a texture whose
// m_IsReadable is set -- which a map author almost never sets, because on PC
// nothing needs it. Ask an unreadable texture for its data and what comes back
// can be an array of the right length full of zeros, which decodes into a
// perfectly black texture; that is what turned every converted level black.
//
// So conversion sets the flag. It is a single serialized bool, in a fixed-size
// field, so it can be written where it sits without the object changing size
// and without anything else in the file moving.
struct TextureObject {
  int64_t pathID = 0;
  std::string name;

  int32_t width = 0;
  int32_t height = 0;
  int32_t mipCount = 0;
  int32_t textureFormat = 0;
  int32_t completeImageSize = 0;

  // Where the pixels live. A bundle usually keeps them in a companion .resS
  // node and references them through m_StreamData; `imageDataSize` is then 0.
  size_t imageDataFileOffset = 0;
  size_t imageDataSize = 0;
  uint32_t streamDataSize = 0;
  bool streamed = false;

  // The m_IsReadable byte: where it is, relative to the buffer the file was
  // parsed from, and what it currently says.
  bool isReadablePresent = false;
  size_t isReadableFileOffset = 0;
  bool isReadable = false;

  size_t bodyFileOffset = 0;
  size_t bodySize = 0;
};

// True for the block-compressed formats no Quest GPU can sample, which are the
// only ones worth making readable: an ETC2 or RGBA32 texture already works.
bool TextureFormatNeedsDecodingOnQuest(int32_t unityTextureFormat);

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

// Replaces one object's serialized body.
struct ObjectEdit {
  int64_t pathID = 0;
  std::vector<uint8_t> body;
};

struct RewriteResult {
  bool ok = false;
  std::vector<uint8_t> data;
  std::string message;
};

// Rebuilds a SerializedFile with some objects' bodies replaced by new ones.
//
// This is the step that makes shader conversion more than a cross-compiler
// bolt-on. The converter today rewrites a single int32 in place and never
// changes any object's size; a rewritten shader is a different length, and the
// object table stores absolute byte offsets, so every object after it moves.
//
// The metadata is copied verbatim -- type tree, externals, script types, user
// information, padding, all of it -- and only the object table's byteStart and
// byteSize fields are patched. Nothing about an object's *body* changes any of
// the rest, so none of it has to be rebuilt, and anything this parser does not
// understand survives untouched. The data region is then relaid with each body
// on an 8-byte boundary, in the order the objects appear in the file, so a
// rewrite with no edits reproduces the file it was given.
//
// Fails rather than truncating when a rewritten file would need offsets wider
// than the file's own version can store.
RewriteResult RewriteSerializedFile(uint8_t const* data, size_t size,
                                    std::vector<ObjectEdit> const& edits);

// A shader's program store, ready to be written back into a Shader object.
struct EncodedProgramStore {
  bool ok = false;
  std::vector<std::vector<int32_t>> offsets;
  std::vector<std::vector<int32_t>> compressedLengths;
  std::vector<std::vector<int32_t>> decompressedLengths;
  std::vector<uint8_t> blob;
  std::string message;
};

// Builds a compressed program store out of sub-programs -- the inverse of
// DecodeShaderPrograms.
//
// `platforms` gives the group order, which is the shader's own platforms array;
// each program is placed by its groupIndex and blobIndex, and programs sharing
// a sub-blob keep their programIndex order.
//
// The blob this produces is not byte-identical to the one Unity wrote, because
// the LZ4 encoder here is not Unity's. What must be identical is what comes
// back out: decoding this store has to yield the programs that went in.
EncodedProgramStore EncodeShaderPrograms(std::vector<int32_t> const& platforms,
                                         std::vector<ShaderSubProgram> const& programs);

// One Shader object's body, rebuilt around a converted program store.
struct ShaderBodyRewrite {
  bool ok = false;
  std::vector<uint8_t> body;
  std::string message;
};

// Rebuilds a Shader object's serialized body with new platform ids and a new
// program store.
//
// Nothing is re-serialized from scratch. The tables whose element counts do not
// change are patched where they sit, and the blob is spliced in with its length
// prefix and trailing alignment recomputed, so every other field -- including
// the ones this parser only walks past to stay in step -- comes through byte
// for byte. That is the difference between a converted bundle that still loads
// and one that is a plausible guess at Unity's layout.
//
// `platforms` must have the same length as the shader's own platforms array,
// and `store` the same group and sub-blob counts, because a different shape
// would need the object rebuilt rather than patched. Anything else is refused.
ShaderBodyRewrite BuildShaderObjectBody(uint8_t const* data, size_t size,
                                        ShaderObject const& shader,
                                        std::vector<int32_t> const& platforms,
                                        EncodedProgramStore const& store);

// LZ4 block-format compression, for writing a converted shader's programs back
// into a bundle.
//
// Returns the number of bytes written, or 0 if the result would not fit in
// dstCapacity (or the input is empty). Give it at least
// Lz4CompressBound(srcSize) to be sure of a result.
//
// This is a single-pass matcher, not a ratio-chasing one: it runs on a headset
// during conversion, and the alternative to a slightly larger blob is no
// converted shader at all.
size_t Lz4CompressBlock(uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstCapacity);

// The largest output Lz4CompressBlock can produce for an input of this size --
// the all-literals case, which is bigger than the input.
size_t Lz4CompressBound(size_t srcSize);

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
  int32_t textureObjectCount = 0;
  std::vector<ShaderObject> shaders;
  std::vector<TextureObject> textures;
  std::string unityVersion;
  std::string message;
};

// Parses one SerializedFile out of a decompressed bundle node.
//
// Returns isSerializedFile=false for a node that is not a serialized file at
// all, which is normal: bundles carry raw .resS/.resource payload nodes too.
FileReport InspectSerializedFile(uint8_t const* data, size_t size);

}  // namespace SerializedFileParse
