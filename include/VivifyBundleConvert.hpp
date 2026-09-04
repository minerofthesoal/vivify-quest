#pragma once

// On-device Unity AssetBundle platform converter.
//
// Vivify maps ship their assets as a Unity AssetBundle. A bundle built for
// Windows/PCVR cannot be loaded by the Android/ARM64 Unity runtime on Quest:
// UnityEngine.AssetBundle.LoadFromFile either returns null or returns a bundle
// whose GetAllAssetNames() is empty, which is the "the experimental Windows
// bundle doesn't have any assets" symptom.
//
// The part of the bundle that actually makes it platform-specific at *load*
// time is a single int32 -- m_TargetPlatform -- in the header of each
// SerializedFile inside the archive. This converter unpacks the archive,
// rewrites that field to Android, and repacks it uncompressed under a new
// name, so Unity will accept and enumerate it.
//
// WHAT THIS CAN AND CANNOT FIX
//   Meshes, prefabs, GameObject hierarchies, animations, animator controllers,
//   audio, text assets and material *definitions* are stored in a
//   platform-independent form and come through intact.
//   Shaders and block-compressed textures do NOT: a Windows bundle carries
//   DirectX shader bytecode and BC/DXT texture data, neither of which an
//   Adreno GPU can consume. Converted bundles therefore render with Vivify's
//   fallback-shader path (see Runtime::RepairMaterialShader) rather than the
//   mapper's intended shading. It is a rescue path for maps that have no
//   Android bundle yet, not a replacement for one.
//
// The implementation is deliberately free of Unity/il2cpp dependencies so it
// can be unit-tested on a host compiler.

#include <cstdint>
#include <string>
#include <string_view>

namespace Vivify::BundleConvert {

// Unity BuildTarget ids that show up in SerializedFile headers.
inline constexpr int32_t kBuildTargetStandaloneOSX = 2;
inline constexpr int32_t kBuildTargetStandaloneWindows = 5;
inline constexpr int32_t kBuildTargetiOS = 9;
inline constexpr int32_t kBuildTargetAndroid = 13;
inline constexpr int32_t kBuildTargetStandaloneWindows64 = 19;
inline constexpr int32_t kBuildTargetWebGL = 20;
inline constexpr int32_t kBuildTargetStandaloneLinux64 = 24;

enum class Status {
  Success,
  // Every SerializedFile in the archive already targets Android; nothing to do.
  AlreadyAndroid,
  SourceUnreadable,
  // Not a UnityFS archive (wrong signature, or a legacy UnityWeb/UnityRaw one).
  NotAUnityBundle,
  // LZMA/LZHAM or an unknown compression id we have no decoder for.
  UnsupportedCompression,
  // Well-formed signature but the structure did not parse.
  Corrupt,
  DestUnwritable,
  OutOfMemory,
};

struct Result {
  Status status = Status::Corrupt;
  // Human-readable detail, safe to log or show in a UI.
  std::string message;
  // Name of the platform the source bundle was built for, when known.
  std::string sourcePlatform;
  int serializedFilesSeen = 0;
  int serializedFilesRetargeted = 0;
  uint64_t outputBytes = 0;

  bool ok() const { return status == Status::Success; }
};

// Reads the UnityFS bundle at sourcePath, retargets every SerializedFile in it
// to Android, and writes an uncompressed UnityFS bundle to destPath.
//
// destPath is only created when the conversion succeeds; on any failure the
// (partial) destination is removed so a later run never sees a truncated file.
// Converting a bundle that is already Android-targeted returns
// Status::AlreadyAndroid and writes nothing.
Result ConvertToAndroid(std::string const& sourcePath, std::string const& destPath);

// True if the file at path begins with the UnityFS archive signature.
//
// Reads 8 bytes; it does not validate the rest of the archive. Vivify maps do
// not agree on a file name for their bundles -- PC maps commonly ship
// "bundleWindows2019"/"bundleWindows2021" with no extension at all -- so
// content is the only dependable way to find one in a song folder.
bool IsUnityBundleFile(std::string const& path);

std::string_view StatusText(Status status);
std::string BuildTargetName(int32_t target);

}
