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

struct ShaderObject {
  std::string name;                // empty when the name resolved only via the common string table
  std::vector<int32_t> platforms;  // ShaderCompilerPlatform values, in file order
};

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
