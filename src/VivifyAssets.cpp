#include "VivifyRuntimeInternal.hpp"
#include "VivifyComponents.hpp"
#include "VivifyBundleConvert.hpp"
#include "VivifyTextureDecode.hpp"
#include "VivifyReport.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace Vivify {

namespace {

// What conversion produces for a given input. Bumped whenever the converter
// starts writing a different bundle from the same source.
//
// A converted bundle is cached on the headset and reused forever, keyed on the
// source file. Without this, the shader-translating conversion would never run
// for any map already converted by an earlier build: the cache would answer
// first, with a bundle whose shaders are still DirectX, and the fix would look
// like it had done nothing until someone found the reconvert button.
//
//   1  retarget only (every build up to 0.9.6)
//   2  retarget plus DirectX -> GLSL ES shader translation
//   3  the same, but actually finding the DXBC: version 2 looked for the
//      container at offset zero, where Unity's own program header sits, so it
//      translated nothing at all and its caches are worth no more than a
//      version 1 one
constexpr int kBundleConversionVersion = 3;

std::string ConversionMarkerPath(std::string const& destPath) {
  return destPath + ".version";
}

// A cached conversion counts only if it was produced by this converter. A
// bundle with no marker beside it came from a build that predates them.
bool CachedConversionIsCurrent(std::string const& destPath) {
  std::error_code ec;
  if (!std::filesystem::exists(destPath, ec) || ec) return false;
  std::ifstream marker(ConversionMarkerPath(destPath));
  int version = 0;
  if (!(marker >> version)) return false;
  return version == kBundleConversionVersion;
}

void MarkConversionCurrent(std::string const& destPath) {
  std::ofstream marker(ConversionMarkerPath(destPath), std::ios::out | std::ios::trunc);
  if (!marker) {
    PaperLogger.warn("Vivify could not record the converter version beside '{}'; the bundle will "
                     "be reconverted every launch", destPath);
    return;
  }
  marker << kBundleConversionVersion << "\n";
}

// True for a shader whose job is to cover the view rather than to shade a
// surface: a full-screen blit, a skybox, a stencil mask, a fog volume.
//
// The distinction matters only when nothing of the original look can be carried
// across. A stand-in on a piece of scenery is a worse-looking mesh; a stand-in
// on one of these is an opaque quad between the player and everything else.
bool IsScreenSpaceEffectShader(std::string_view shaderName) {
  std::string lowered(shaderName);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  // Every word here has to name the shader's *job*, not merely appear in its
  // name, because a false positive deletes real scenery.
  //
  // "fog" was on this list and matched Swifter/SimpleTerrainFog -- the shader
  // 743Aether puts on its terrain -- so the ground, the spikes and the terrain
  // notes were all silently dropped and the map rendered as a few white shapes
  // over black. "screen" and "mask" were the same kind of mistake waiting to
  // happen; a real stencil mask is caught by "stencil" without them.
  static constexpr std::string_view screenEffects[] = {
      "blit"sv, "skybox"sv, "stencil"sv, "postpro"sv, "bokeh"sv,
      "shadowcaster"sv, "depthonly"sv, "cleardepth"sv, "vignette"sv,
  };
  for (auto effect : screenEffects) {
    if (lowered.find(effect) != std::string_view::npos) return true;
  }
  return false;
}

// Runs whichever conversion the settings ask for and flattens the two result
// shapes into one.
//
// The shader-translating path is the full conversion: it cross-compiles each
// DirectX program to GLSL ES and rebuilds the archive around the shaders that
// changed size. The retarget-only path is what this mod did before, kept as the
// setting's off position so a translation that turns out worse than an unshaded
// mesh can be backed out by reconverting rather than by waiting for a build.
struct BundleConversionOutcome {
  BundleConvert::Status status = BundleConvert::Status::Corrupt;
  std::string message;
};

BundleConversionOutcome RunBundleConversion(std::string const& source, std::string const& dest) {
  if (!GetTranslateShadersOnConversion()) {
    auto const result = BundleConvert::ConvertToAndroid(source, dest);
    // Deliberately not marked current: a retarget-only bundle is what the
    // marker exists to invalidate, so turning the setting back on has to
    // reconvert rather than reuse this.
    return {result.status, result.message};
  }
  auto const conversion = BundleConvert::ConvertShadersToGles(source, dest);
  if (conversion.status == BundleConvert::Status::Success) MarkConversionCurrent(dest);
  // Logged here, on the worker, rather than folded into the message: a bundle
  // can refuse several shaders and each reason is a line worth reading on its
  // own when working out why a converted map still looks wrong.
  for (auto const& refusal : conversion.refusals) {
    PaperLogger.info("Vivify shader translation left a shader as it was -- {}", refusal);
  }
  if (conversion.shadersRefused > static_cast<int>(conversion.refusals.size())) {
    PaperLogger.info("Vivify shader translation left {} further shader(s) as they were",
                     conversion.shadersRefused - static_cast<int>(conversion.refusals.size()));
  }
  return {conversion.status, conversion.message};
}

std::string ResolveBundlePath(std::string const& levelPath) {
  std::string bundlePath = JoinPath(levelPath, kBundleFile);
  if (std::filesystem::exists(bundlePath)) {
    return bundlePath;
  }
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator(levelPath, ec)) {
    if (ec) break;
    auto const& p = entry.path();
    if (p.extension() == ".vivify") {
      return p.string();
    }
  }
  return {};
}

// Finds a PC-built Vivify AssetBundle in a song folder, by content.
//
// Upstream Vivify names its bundle from VivifyController.BUNDLE_FILE,
// $"bundle{BUNDLE_SUFFIX}.vivify" where BUNDLE_SUFFIX is "Windows2021" (or
// "Windows2019" on 1.29.1), so a PC map normally ships bundleWindows2021.vivify
// -- extension included. This port's own download path writes
// bundleAndroid2021.vivify, and ResolveBundlePath already finds either by
// extension.
//
// The reason this scan is content-based rather than name-based anyway is that
// the name is only a convention: re-zipped map downloads, hand-built bundles
// and the Unity exporter's own output all turn up under other names, and a
// bundle this function fails to find is a map that can never be converted and
// so can never be played. Every candidate is checked for the UnityFS signature
// instead. Names are used only to rank equally-valid candidates: a "windows"
// name wins over a generic "bundle" name, which wins over anything else that
// happens to be a Unity archive.
std::string ResolvePcBundlePath(std::string const& levelPath) {
  std::error_code ec;
  std::string best;
  int bestScore = -1;
  for (auto const& entry : std::filesystem::directory_iterator(levelPath, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    auto const& path = entry.path();
    std::string lower = path.filename().string();
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Skip the file types a song folder is otherwise made of, then content-check
    // whatever is left. Being permissive here is the point: over-restrictive
    // name matching is what hid these bundles in the first place.
    static constexpr std::string_view kNonBundleExtensions[] = {
        ".dat", ".json", ".ogg", ".egg", ".wav", ".mp3", ".jpg", ".jpeg", ".png", ".bmp", ".txt", ".md",
    };
    std::string const extension = path.extension().string();
    std::string lowerExtension = extension;
    std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (std::find(std::begin(kNonBundleExtensions), std::end(kNonBundleExtensions), lowerExtension) !=
        std::end(kNonBundleExtensions)) {
      continue;
    }

    int score = 1;
    if (lower.find("windows") != std::string::npos) score = 3;
    else if (lower.find("bundle") != std::string::npos || path.extension() == ".vivify") score = 2;
    if (score <= bestScore) continue;
    if (!BundleConvert::IsUnityBundleFile(path.string())) continue;
    best = path.string();
    bestScore = score;
  }
  return best;
}

// Where on-device-converted bundles are cached. Kept out of the song folder so
// syncing or re-downloading a map never trips over a file Vivify generated, and
// so a converted bundle is never mistaken for one the mapper shipped.
std::string ConvertedBundleCacheDir() {
  return "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/ConvertedBundles";
}

// Cache key for a converted bundle.
//
// Deliberately built from the song folder name, the bundle file name, and the
// bundle's size and mtime -- NOT from its absolute path. The bulk pass walks
// SongCore's level roots while level selection uses
// CustomBeatmapLevel::customLevelPath, and on Android the same directory is
// reachable as /sdcard/..., /storage/emulated/0/... and
// /storage/self/primary/... . Keying on the absolute path meant those two
// routes could hash the same file differently, so a bundle converted by the
// bulk pass was not found again at level selection and the map stayed
// unplayable as though nothing had been converted.
//
// Size and mtime still invalidate the entry when the source bundle changes,
// without having to hash hundreds of megabytes.
std::string ConvertedBundlePath(std::string const& sourceBundlePath) {
  std::filesystem::path const source(sourceBundlePath);
  std::string const fileName = source.filename().string();
  std::string const folderName = source.parent_path().filename().string();

  std::error_code ec;
  uint64_t size = std::filesystem::file_size(source, ec);
  if (ec) size = 0;
  ec.clear();
  auto const writeTime = std::filesystem::last_write_time(source, ec);
  uint64_t stamp = 0;
  if (!ec) stamp = static_cast<uint64_t>(writeTime.time_since_epoch().count());

  uint64_t hash = 1469598103934665603ull;
  auto mix = [&hash](std::string_view bytes) {
    for (char c : bytes) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
  };
  mix(folderName);
  mix("\x1f");
  mix(fileName);
  mix("\x1f");
  mix(std::to_string(size));
  mix("\x1f");
  mix(std::to_string(stamp));

  // Keep a readable prefix so the cache directory can be eyeballed against the
  // song list when something looks wrong.
  std::string prefix;
  for (char c : folderName) {
    if (prefix.size() >= 48) break;
    bool const safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      c == '-' || c == '_';
    prefix.push_back(safe ? c : '_');
  }

  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "_%016llx.vivify", static_cast<unsigned long long>(hash));
  return JoinPath(ConvertedBundleCacheDir(), prefix + suffix);
}

uint32_t ReadAndroidChecksumFromInfoDat(std::string const& levelPath) {
  std::string infoPath = JoinPath(levelPath, "Info.dat");
  if (!std::filesystem::exists(infoPath)) infoPath = JoinPath(levelPath, "info.dat");
  if (!std::filesystem::exists(infoPath)) return 0;
  std::ifstream ifs(infoPath);
  if (!ifs.is_open()) return 0;
  std::string str((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
  rapidjson::Document doc;
  doc.Parse(str.c_str());
  if (doc.HasParseError()) return 0;
  rapidjson::Value const* customData = nullptr;
  if (doc.HasMember("_customData")) customData = &doc["_customData"];
  else if (doc.HasMember("customData")) customData = &doc["customData"];
  if (customData == nullptr || !customData->IsObject()) return 0;
  rapidjson::Value const* assetBundle = nullptr;
  if (customData->HasMember("_assetBundle")) assetBundle = &(*customData)["_assetBundle"];
  else if (customData->HasMember("assetBundle")) assetBundle = &(*customData)["assetBundle"];
  if (assetBundle == nullptr || !assetBundle->IsObject()) return 0;
  if (assetBundle->HasMember("_android2021") && (*assetBundle)["_android2021"].IsUint()) {
    return (*assetBundle)["_android2021"].GetUint();
  }
  if (assetBundle->HasMember("android2021") && (*assetBundle)["android2021"].IsUint()) {
    return (*assetBundle)["android2021"].GetUint();
  }
  return 0;
}

struct MaterialFallbackState {
  std::optional<UnityEngine::Color> color;
  UnityEngine::Texture* mainTexture = nullptr;
  int renderQueue = -1;
};

// True for a colour that would leave the mesh looking blank.
bool IsUninformativeColor(UnityEngine::Color const& color) {
  bool const nearWhite = color.r > 0.97f && color.g > 0.97f && color.b > 0.97f;
  bool const invisible = color.a <= 0.01f;
  return nearWhite || invisible;
}

// Recovers the colour a material was actually tinted with.
//
// This used to return the first property that *existed*, which is almost always
// `_Color` -- a property nearly every shader declares and nearly every material
// leaves at its default of white. So the stand-in faithfully carried white
// across and every converted asset came out flat white, even though the
// material's real colour was sitting in `_BaseColor`, `_EmissionColor` or one of
// the map's own named properties. Material property *values* are stored in the
// SerializedFile and are entirely platform-independent, so that colour survives
// conversion perfectly -- it was only ever being looked up wrong.
//
// Every candidate is now collected and the first informative one wins, falling
// back to whatever was found if they are all blank.
std::optional<UnityEngine::Color> ReadMaterialFallbackColor(UnityEngine::Material* material) {
  if (!IsManagedAlive(material)) return std::nullopt;
  static int const colorIds[] = {
      UnityEngine::Shader::PropertyToID(u"_BaseColor"),
      UnityEngine::Shader::PropertyToID(u"_MainColor"),
      UnityEngine::Shader::PropertyToID(u"_TintColor"),
      UnityEngine::Shader::PropertyToID(u"_Tint"),
      UnityEngine::Shader::PropertyToID(u"_EmissionColor"),
      UnityEngine::Shader::PropertyToID(u"_Emission"),
      UnityEngine::Shader::PropertyToID(u"_HorizonCol"),
      UnityEngine::Shader::PropertyToID(u"_SkyCol"),
      UnityEngine::Shader::PropertyToID(u"_Color"),
  };

  std::optional<UnityEngine::Color> firstFound;
  auto consider = [&firstFound](UnityEngine::Color const& color) {
    if (!firstFound.has_value()) firstFound = color;
    return !IsUninformativeColor(color);
  };

  for (int id : colorIds) {
    if (!material->HasProperty(id)) continue;
    auto color = material->GetColor(id);
    if (consider(color)) return color;
  }

  // Then anything colour-shaped the material declares itself. Colours live in
  // the Vector bucket -- MaterialPropertyType has no separate Color member.
  auto names = material->GetPropertyNames(UnityEngine::MaterialPropertyType::Vector);
  if (names) {
    for (auto name : names) {
      if (!name) continue;
      std::string key = NormalizeAssetKey(ToStdString(name));
      if (key.find("color") == std::string::npos && key.find("colour") == std::string::npos &&
          key.find("col") == std::string::npos && key.find("tint") == std::string::npos &&
          key.find("emis") == std::string::npos) {
        continue;
      }
      auto color = material->GetColor(name);
      if (consider(color)) return color;
    }
  }
  return firstFound;
}

// Recovers a texture to hand to the stand-in shader.
//
// Material.mainTexture only ever resolves `_MainTex`. Custom shaders routinely
// name their albedo something else (`_BaseMap`, `_Albedo`, `_Tex`), so any
// material not using the conventional name came through untextured.
UnityEngine::Texture* ReadMaterialFallbackTexture(UnityEngine::Material* material) {
  if (!IsManagedAlive(material)) return nullptr;
  if (auto* mainTexture = material->get_mainTexture().unsafePtr(); IsManagedAlive(mainTexture)) {
    return mainTexture;
  }
  auto names = material->GetPropertyNames(UnityEngine::MaterialPropertyType::Texture);
  if (!names) return nullptr;
  for (auto name : names) {
    if (!name) continue;
    std::string key = NormalizeAssetKey(ToStdString(name));
    // Skip the maps that would look wrong as an albedo.
    if (key.find("bump") != std::string::npos || key.find("normal") != std::string::npos ||
        key.find("mask") != std::string::npos || key.find("metallic") != std::string::npos ||
        key.find("occlusion") != std::string::npos || key.find("smoothness") != std::string::npos ||
        key.find("height") != std::string::npos || key.find("detail") != std::string::npos) {
      continue;
    }
    if (auto* texture = material->GetTexture(name).unsafePtr(); IsManagedAlive(texture)) {
      return texture;
    }
  }
  return nullptr;
}

MaterialFallbackState CaptureMaterialFallbackState(UnityEngine::Material* material) {
  MaterialFallbackState state;
  if (!IsManagedAlive(material)) return state;
  state.color = ReadMaterialFallbackColor(material);
  state.mainTexture = ReadMaterialFallbackTexture(material);
  state.renderQueue = material->get_renderQueue();
  return state;
}

void RestoreMaterialFallbackState(UnityEngine::Material* material, MaterialFallbackState const& state) {
  if (!IsManagedAlive(material)) return;
  if (state.color.has_value()) {
    static int const fallbackColorIds[] = {
        UnityEngine::Shader::PropertyToID(u"_Color"),
        UnityEngine::Shader::PropertyToID(u"_BaseColor"),
        UnityEngine::Shader::PropertyToID(u"_TintColor"),
   };
    for (int id : fallbackColorIds) {
      if (material->HasProperty(id)) {
        material->SetColor(id, state.color.value());
      }
    }
  }
  if (IsManagedAlive(state.mainTexture)) {
    // set_mainTexture only writes _MainTex; a stand-in using URP-style naming
    // wants _BaseMap instead, so write whichever the shader actually declares.
    static int const textureIds[] = {
        UnityEngine::Shader::PropertyToID(u"_MainTex"),
        UnityEngine::Shader::PropertyToID(u"_BaseMap"),
    };
    for (int id : textureIds) {
      if (material->HasProperty(id)) {
        material->SetTexture(id, state.mainTexture);
      }
    }
  }
  if (state.renderQueue >= 0) {
    material->set_renderQueue(state.renderQueue);
  }
}
}

void Runtime::HandleLevelSelected(SongCore::API::LevelSelect::LevelWasSelectedEventArgs const& event) {

  std::string incomingLevelPath;
  if (event.isCustom && event.customBeatmapLevel != nullptr) {
    incomingLevelPath = std::string(event.customBeatmapLevel->customLevelPath);
  }
  if (!incomingLevelPath.empty() && incomingLevelPath != _selectedLevelPath) {
    if (_mainBundle != nullptr && UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
      _mainBundle->Unload(true);
      _mainBundle = nullptr;
    }
    _preloadedBundlePath.clear();
  }
  ResetRuntime("left the level");

  _activeSabers.clear();
  _selectedLevelPath.clear();
  _selectedBundlePath.clear();
  _selectedMapHasVivifyRequirement = false;
  CancelPendingDownload();
  if (!event.isCustom || event.customBeatmapLevel == nullptr) {
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    return;
  }
  _selectedLevelPath = std::string(event.customBeatmapLevel->customLevelPath);
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify level selected: path='{}' isCustom={} hasDetails={}",
                     _selectedLevelPath, BoolText(event.isCustom), BoolText(event.customLevelDetails.has_value()));
  }
  if (event.customLevelDetails) {
    auto const& requirements = event.customLevelDetails->difficultyDetails.requirements;
    _selectedMapHasVivifyRequirement = std::any_of(requirements.begin(), requirements.end(), [](std::string const& requirement) {
      return requirement == kCapability;
    });
  }
  if (!_selectedMapHasVivifyRequirement) {
    MetaCore::Game::SetScoreSubmission("Vivify", true);
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    return;
  }

  // Vivify changes how a map looks, not how it plays: no note timing, no
  // scoring, nothing a leaderboard measures. Disabling submission for every
  // Vivify map -- which is what this did unconditionally -- also stopped
  // BeatLeader and ScoreSaber recording a replay, which is why Vivify maps had
  // no replays to watch or render. Submission is on by default now, with a
  // setting for anyone who wants the old behaviour.
  bool const submit = GetSubmitScoresOnVivifyMaps();
  MetaCore::Game::SetScoreSubmission("Vivify", submit);
  PaperLogger.info("Vivify score submission: {} for this Vivify map",
                   submit ? "enabled" : "disabled by setting");
  // The settings that decide whether a map renders at all, recorded once per
  // level. Chasing "geometry stopped being repaired halfway through a session"
  // took a guess at which toggle had moved, because nothing in the log said
  // what any of them were set to at the time.
  PaperLogger.info("Vivify settings for this level: standInShading={} convertPcBundlesOnDevice={} "
                   "disableCustomNoteVisuals={} disableAllBlits={} multipassRendering={}",
                   BoolText(GetStandInShading()), BoolText(GetConvertPcBundlesOnDevice()),
                   BoolText(GetDisableCustomNoteVisuals()), BoolText(GetDisableAllBlits()),
                   BoolText(GetMultipassRenderingEnabled()));

  std::string const androidBundlePath = JoinPath(_selectedLevelPath, std::string(kBundleFile));

  if (std::filesystem::exists(androidBundlePath)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify bundle selection: using local Android bundle '{}'", androidBundlePath);
    }
    _selectedBundlePath = androidBundlePath;
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    PreloadBundle(androidBundlePath);
    return;
  }

  // No Android bundle in the song folder. Work out what else is available
  // before deciding, and log the whole picture unconditionally -- when a map
  // will not start, this one line says exactly which branch was taken and why.
  std::string const pcBundleFallback = ResolvePcBundlePath(_selectedLevelPath);
  uint32_t const androidChecksum = ReadAndroidChecksumFromInfoDat(_selectedLevelPath);
  std::string const cachedConversion =
      pcBundleFallback.empty() ? std::string() : ConvertedBundlePath(pcBundleFallback);
  bool const haveCachedConversion =
      !cachedConversion.empty() && CachedConversionIsCurrent(cachedConversion);

  PaperLogger.info(
      "Vivify bundle selection: level='{}' androidBundle=no android2021={} pcBundle='{}' convertedCache='{}' cached={}",
      _selectedLevelPath, androidChecksum,
      pcBundleFallback.empty() ? std::string("<none>") : pcBundleFallback,
      cachedConversion.empty() ? std::string("<none>") : cachedConversion,
      BoolText(haveCachedConversion));

  // An already-converted bundle is on disk and ready, so use it now instead of
  // going to the network.
  //
  // Checking the download first was wrong: a map that ships a PC bundle
  // usually has no Android build in the bundle repo to download -- that is why
  // it only ships a PC bundle -- so the request fails, or worse hangs, and the
  // play button sits on "Downloading assets..." while a perfectly good
  // converted bundle is sitting in the cache unused. That is what made
  // already-converted levels stay unplayable.
  if (haveCachedConversion) {
    _selectedBundlePath = cachedConversion;
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    PreloadBundle(cachedConversion);
    return;
  }

  if (androidChecksum != 0) {
    BeginBundleDownload(androidChecksum, _selectedLevelPath, pcBundleFallback);
    return;
  }
  if (!pcBundleFallback.empty()) {
    ConvertPcBundleAsync(_selectedLevelPath, pcBundleFallback);
    return;
  }
  PaperLogger.warn("Vivify: '{}' has no Android bundle, no PC bundle to convert, and no android2021 checksum",
                   _selectedLevelPath);
  SongCore::API::PlayButton::DisablePlayButton("Vivify", "No Vivify assets found for this map.");
}

void Runtime::CancelPendingDownload() {
  _downloadGeneration++;
  _downloadDeadline = -1.0f;
  _pendingDownloadLevelPath.clear();
  _pendingDownloadPcFallback.clear();
}

// WebUtils does not promise a callback on every failure mode (a dropped
// connection or a stalled request can simply never resolve), and a level whose
// play button is waiting on one would stay unplayable for the rest of the
// session. Time it out and take the conversion path instead.
void Runtime::CheckDownloadTimeout() {
  if (_downloadDeadline < 0.0f) return;
  if (UnityEngine::Time::get_realtimeSinceStartup() < _downloadDeadline) return;

  std::string const levelPath = _pendingDownloadLevelPath;
  std::string const pcBundleFallback = _pendingDownloadPcFallback;
  CancelPendingDownload();
  PaperLogger.warn("Vivify asset download timed out for '{}'", levelPath);
  if (levelPath != _selectedLevelPath) return;
  if (!pcBundleFallback.empty()) {
    ConvertPcBundleAsync(levelPath, pcBundleFallback);
    return;
  }
  SongCore::API::PlayButton::DisablePlayButton("Vivify", "Asset download timed out.");
}

void Runtime::BeginBundleDownload(uint32_t checksum, std::string const& levelPath,
                                  std::string const& pcBundleFallback) {
  SongCore::API::PlayButton::DisablePlayButton("Vivify", "Downloading assets...");
  CancelPendingDownload();
  int const generation = _downloadGeneration;
  _downloadDeadline = UnityEngine::Time::get_realtimeSinceStartup() + kAssetDownloadTimeoutSeconds;
  _pendingDownloadLevelPath = levelPath;
  _pendingDownloadPcFallback = pcBundleFallback;

  DownloadBundle(checksum, levelPath, [this, generation, levelPath, pcBundleFallback](bool success) {
    // A newer selection (or the timeout) already moved on from this download.
    if (generation != _downloadGeneration) return;
    CancelPendingDownload();
    if (levelPath != _selectedLevelPath) return;
    if (success) {
      std::string downloaded = ResolveBundlePath(levelPath);
      if (!downloaded.empty()) {
        _selectedBundlePath = downloaded;
        PreloadBundle(downloaded);
      }
      SongCore::API::PlayButton::EnablePlayButton("Vivify");
      return;
    }
    // The download is the preferred path, but a map that ships a PC bundle is
    // still recoverable without it.
    if (!pcBundleFallback.empty()) {
      PaperLogger.warn("Vivify asset download failed; falling back to converting the PC bundle '{}'",
                       pcBundleFallback);
      ConvertPcBundleAsync(levelPath, pcBundleFallback);
      return;
    }
    SongCore::API::PlayButton::DisablePlayButton("Vivify", "Failed to download assets.");
  });
}

// Converts a PC-built AssetBundle into an Android-loadable one on the device.
//
// This replaces the old "Allow Unsafe Windows Bundle Fallback" toggle, which
// handed the Windows bundle straight to UnityEngine.AssetBundle.LoadFromFile.
// Unity does not reject that outright -- it returns a bundle object whose
// GetAllAssetNames() is empty, which is exactly the reported "the experimental
// Windows bundle doesn't have any assets" behaviour. Retargeting the archive
// first is what actually makes Unity enumerate and load its contents.
void Runtime::ConvertPcBundleAsync(std::string const& levelPath, std::string const& sourceBundlePath) {
  if (!GetConvertPcBundlesOnDevice()) {
    PaperLogger.warn("Vivify found a PC asset bundle but on-device conversion is disabled: '{}'", sourceBundlePath);
    SongCore::API::PlayButton::DisablePlayButton("Vivify",
                                                 "PC bundle found; enable Convert PC Bundles On Device in settings.");
    return;
  }

  if (_bundleConversionSource == sourceBundlePath) {
    // Already running for this bundle; its completion handler will re-enable
    // the play button.
    SongCore::API::PlayButton::DisablePlayButton("Vivify", "Converting PC assets...");
    return;
  }

  std::string const destPath = ConvertedBundlePath(sourceBundlePath);
  if (CachedConversionIsCurrent(destPath)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify using cached converted bundle: '{}'", destPath);
    }
    _selectedBundlePath = destPath;
    SongCore::API::PlayButton::EnablePlayButton("Vivify");
    PreloadBundle(destPath);
    return;
  }

  SongCore::API::PlayButton::DisablePlayButton("Vivify", "Converting PC assets...");
  PaperLogger.info("Vivify converting PC asset bundle on device: '{}' -> '{}'", sourceBundlePath, destPath);
  _bundleConversionSource = sourceBundlePath;

  // Conversion decompresses the whole archive, so it must not run on the main
  // thread. Results are handed back the same way the download path does it.
  std::thread([this, levelPath, sourceBundlePath, destPath]() {
    BundleConversionOutcome const result = RunBundleConversion(sourceBundlePath, destPath);
    // A bundle that was already Android-targeted needs no rewrite; load it as-is.
    std::string loadPath = result.status == BundleConvert::Status::AlreadyAndroid ? sourceBundlePath : destPath;
    bool const usable = result.status == BundleConvert::Status::Success ||
                        result.status == BundleConvert::Status::AlreadyAndroid;
    std::string const message = result.message;
    std::string const statusText{BundleConvert::StatusText(result.status)};

    // Report what the source bundle's shaders were actually compiled for. This
    // is the one place with the answer to "why does this map render with
    // stand-in shading" that is not a guess: it reads the Shader assets and
    // names their target platforms. Done on the worker, since it unpacks the
    // archive a second time.
    std::string shaderScanText;
    if (usable) {
      auto const scan = BundleConvert::ScanShaders(sourceBundlePath);
      shaderScanText = BundleConvert::DescribeShaderScan(scan);
    }

    BSML::MainThreadScheduler::Schedule([this, levelPath, sourceBundlePath, loadPath, usable, message, statusText, shaderScanText]() {
      if (!shaderScanText.empty()) {
        _sourceBundleScanText = shaderScanText;
        PaperLogger.info("Vivify source bundle shaders: {}", shaderScanText);
      }
      // Only clear the in-flight marker if a newer conversion has not claimed it.
      if (_bundleConversionSource == sourceBundlePath) _bundleConversionSource.clear();
      if (usable) {
        PaperLogger.info("Vivify bundle conversion succeeded: {}", message);
      } else {
        PaperLogger.warn("Vivify bundle conversion failed ({}): {}", statusText, message);
      }
      if (levelPath != _selectedLevelPath) return;
      if (!usable) {
        SongCore::API::PlayButton::DisablePlayButton("Vivify", "Convert failed: " + statusText);
        return;
      }
      _selectedBundlePath = loadPath;
      SongCore::API::PlayButton::EnablePlayButton("Vivify");
      PreloadBundle(loadPath);
    });
  }).detach();
}

void Runtime::DownloadBundle(uint32_t checksum, std::string const& levelPath, std::function<void(bool)> callback) {
  std::string url = "https://repo.totalbs.dev/api/v1/bundles/" + std::to_string(checksum);
  std::string bundlePath = JoinPath(levelPath, kBundleFile);
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify bundle download: android2021={} metadataUrl='{}' cachePath='{}'",
                     checksum, url, bundlePath);
  }
  WebUtils::GetAsync<WebUtils::StringResponse>(WebUtils::URLOptions(url), [bundlePath, callback, url](WebUtils::StringResponse res) {
    if (!res.IsSuccessful() || !res.responseData.has_value()) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify bundle download failed: metadata request unsuccessful url='{}'", url);
      }
      BSML::MainThreadScheduler::Schedule([callback] { callback(false); });
      return;
    }
    rapidjson::Document doc;
    doc.Parse(res.responseData->c_str());
    if (doc.HasParseError() || !doc.HasMember("downloadUrl") || !doc["downloadUrl"].IsString()) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify bundle download failed: metadata response did not contain downloadUrl");
      }
      BSML::MainThreadScheduler::Schedule([callback] { callback(false); });
      return;
    }
    std::string downloadUrl = doc["downloadUrl"].GetString();
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify bundle download URL resolved: '{}'", downloadUrl);
    }
    WebUtils::GetAsync<WebUtils::DataResponse>(WebUtils::URLOptions(downloadUrl), [bundlePath, callback](WebUtils::DataResponse dataRes) {
      if (!dataRes.IsSuccessful() || !dataRes.responseData.has_value()) {
        if (GetVivifyDebugLogging()) {
          PaperLogger.warn("Vivify bundle download failed: data request unsuccessful path='{}'", bundlePath);
        }
        BSML::MainThreadScheduler::Schedule([callback] { callback(false); });
        return;
      }
      std::ofstream os(bundlePath, std::ios::binary);
      bool const written = os.is_open();
      if (written) {
        os.write(reinterpret_cast<char const*>(dataRes.responseData->data()),
                 static_cast<std::streamsize>(dataRes.responseData->size()));
        os.close();
      }
      if (GetVivifyDebugLogging()) {
        PaperLogger.info("Vivify bundle download complete: path='{}' bytes={} written={}",
                         bundlePath, dataRes.responseData->size(), BoolText(written));
      }
      BSML::MainThreadScheduler::Schedule([callback, written] { callback(written); });
    });
  });
}


// Why a bundle shader cannot draw on this device.
//
// The port used to record a single bit per shader -- Shader.isSupported -- which
// collapses two completely different failures into one indistinguishable
// "unsupported", and every question about a broken map ("is this a geometry
// shader problem?") stalls on not being able to tell them apart:
//
//   NoProgram       the bundle carries no shader program for this platform at
//                   all. This is every shader in a converted PC bundle: the
//                   archive was built by Unity for Windows, so its programs are
//                   DirectX bytecode. Conversion rewrites the container's target
//                   platform so the assets, meshes and materials load, but it
//                   cannot invent GLES/Vulkan programs that were never compiled.
//                   No device could run these, and no setting changes that.
//
//   DeviceRejected  the bundle does carry programs -- so it was built for
//                   Android -- but this GPU accepts none of the subshaders.
//                   THIS is the bucket a geometry-shader shader lands in.
//                   Unity records each subshader's hardware requirements and
//                   refuses the ones the device cannot meet; a geometry stage is
//                   unavailable under Vulkan on Adreno (Qualcomm has never
//                   exposed VkPhysicalDeviceFeatures.geometryShader), though it
//                   is available under OpenGL ES 3.2 via GL_EXT_geometry_shader.
//
// Unity picks the highest-LOD subshader whose requirements the device meets, so
// a shader that ships a geometry-shader subshader *and* a plain one is already
// handled automatically and lands in Runnable. Only a shader whose every
// subshader needs a stage this device lacks reaches DeviceRejected.
namespace {
enum class ShaderVerdict { Runnable, NoProgram, DeviceRejected, Dead };

ShaderVerdict ClassifyBundleShader(UnityEngine::Shader* shader) {
  if (!IsManagedAlive(shader)) return ShaderVerdict::Dead;
  if (shader->get_isSupported()) return ShaderVerdict::Runnable;
  // subshaderCount counts the subshaders that survived compilation *for this
  // build target*. Zero means the serialised shader has no program for Android,
  // which is the converted-PC-bundle case; non-zero means programs exist and the
  // device turned every one of them down.
  return shader->get_subshaderCount() > 0 ? ShaderVerdict::DeviceRejected : ShaderVerdict::NoProgram;
}
}  // namespace

void Runtime::CacheBundleAssets() {
  if (_mainBundle == nullptr || !UnityEngine::Object::op_Implicit_bool(_mainBundle)) return;
  auto assetNames = _mainBundle->GetAllAssetNames();
  if (!assetNames) return;
  _assets.clear();
  _assetsByName.clear();
  _supportedShadersByName.clear();
  int shadersSeen = 0;
  int shadersRunnable = 0;
  int shadersNoProgram = 0;
  int shadersDeviceRejected = 0;
  std::vector<std::string> deviceRejectedNames;
  for (auto assetName : assetNames) {
    if (!assetName) continue;
    std::string originalAssetPath = il2cpp_utils::detail::to_string(assetName);
    std::string key = NormalizeAssetKey(originalAssetPath);

    // One bad asset must not take the whole bundle (or the game) down. A
    // converted PC bundle in particular carries DirectX shader programs and
    // BC/DXT texture data that this GPU cannot consume, and those surface here
    // as the asset is realised.
    UnityEngine::Object* asset = nullptr;
    try {
      asset = _mainBundle->LoadAsset(assetName).unsafePtr();
    } catch (std::exception const& ex) {
      PaperLogger.warn("Vivify asset load threw, skipping: path='{}' error={}", originalAssetPath, ex.what());
      continue;
    } catch (...) {
      PaperLogger.warn("Vivify asset load threw, skipping: path='{}'", originalAssetPath);
      continue;
    }
    if (!IsAlive(asset)) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify asset load failed: path='{}'", originalAssetPath);
      }
      continue;
    }
    if (!key.empty()) _assets[key] = asset;
    auto name = asset->get_name();
    if (name) {
      auto nameKey = NormalizeAssetKey(il2cpp_utils::detail::to_string(name));
      if (!nameKey.empty() && !_assetsByName.contains(nameKey)) {
        _assetsByName[nameKey] = asset;
      }
      if (auto* shader = il2cpp_utils::try_cast<UnityEngine::Shader>(asset).value_or(nullptr);
          IsAlive(shader)) {
        shadersSeen++;
        switch (ClassifyBundleShader(shader)) {
          case ShaderVerdict::Runnable:
            shadersRunnable++;
            if (!nameKey.empty()) _supportedShadersByName[nameKey] = shader;
            break;
          case ShaderVerdict::NoProgram:
            shadersNoProgram++;
            break;
          case ShaderVerdict::DeviceRejected:
            shadersDeviceRejected++;
            // Named individually: this is the bucket that a map author can act
            // on, by shipping a subshader without the stage this device lacks.
            if (deviceRejectedNames.size() < 12) {
              deviceRejectedNames.push_back(fmt::format("'{}' (subshaders={} passes={} maxLOD={})",
                                                        ShaderNameForLog(shader),
                                                        shader->get_subshaderCount(),
                                                        shader->get_passCount(),
                                                        shader->get_maximumLOD()));
            }
            break;
          case ShaderVerdict::Dead:
            break;
        }
      }
    }
    if (GetVivifyDebugLogging()) {
      if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr);
          IsAlive(material)) {
        LogMaterialShader("bundle-load", originalAssetPath, material);
      } else if (auto* shader = il2cpp_utils::try_cast<UnityEngine::Shader>(asset).value_or(nullptr);
                 IsAlive(shader)) {
        PaperLogger.info("Vivify shader asset: path='{}' shader='{}' supported={} subshaders={}",
                         originalAssetPath, ShaderNameForLog(shader), BoolText(shader->get_isSupported()),
                         shader->get_subshaderCount());
      }
    }
  }

  LogBundleShaderAudit(shadersSeen, shadersRunnable, shadersNoProgram, shadersDeviceRejected,
                       deviceRejectedNames);
}

// One unconditional verdict per bundle, so a report of "the map is invisible" or
// "geometry shaders don't work" can be answered from the log without a repro.
void Runtime::LogBundleShaderAudit(int seen, int runnable, int noProgram, int deviceRejected,
                                   std::vector<std::string> const& deviceRejectedNames) {
  _auditShadersSeen = seen;
  _auditShadersRunnable = runnable;
  _auditShadersNoProgram = noProgram;
  _auditShadersDeviceRejected = deviceRejected;
  _auditRejectedNames = deviceRejectedNames;
  if (seen == 0) return;

  bool const converted = !_preloadedBundlePath.empty() &&
                         _preloadedBundlePath.rfind(ConvertedBundleCacheDir(), 0) == 0;
  PaperLogger.info(
      "Vivify shaders: bundle='{}' converted={} total={} runnable={} noAndroidProgram={} deviceRejected={}",
      _preloadedBundlePath, BoolText(converted), seen, runnable, noProgram, deviceRejected);

  if (noProgram > 0) {
    PaperLogger.warn(
        "Vivify: {} of {} shaders in this bundle carry no Android program, so they cannot run on any Quest. "
        "{} Materials using them fall back to stand-in shading, which keeps the models visible but loses their "
        "real shading -- raymarching, post-processing and geometry-stage effects included.",
        noProgram, seen,
        converted ? "This bundle was converted from a PC build: its shader programs are DirectX bytecode, which "
                    "conversion cannot translate. Only a bundle built for Android carries runnable programs."
                  : "The bundle was not built for Android.");
  }

  if (deviceRejected > 0) {
    // Programs exist, so the bundle really was built for Android and the GPU is
    // the one refusing. A geometry or tessellation stage is the usual reason.
    PaperLogger.warn(
        "Vivify: {} of {} shaders were built for Android but this GPU accepts none of their subshaders. "
        "A shader stage the device lacks is the usual cause -- under Vulkan on Adreno there is no geometry or "
        "tessellation stage at all. A shader that also ships a subshader without that stage is selected "
        "automatically and does not appear here.",
        deviceRejected, seen);
    for (auto const& name : deviceRejectedNames) {
      PaperLogger.warn("Vivify shader rejected by device: {}", name);
    }
    if (deviceRejected > static_cast<int>(deviceRejectedNames.size())) {
      PaperLogger.warn("Vivify: ... and {} more",
                       deviceRejected - static_cast<int>(deviceRejectedNames.size()));
    }
  }
}

// Formats the on-device report for the current level.
//
// Written twice per level -- once at load, once at the end -- so that a level
// which never finishes still leaves the load block behind. Everything a
// "why is this map broken" question needs is here, because asking a player to
// retrieve paperlog output is not reasonable.
std::string Runtime::BuildLevelReport(std::string_view outcome) const {
  std::string text;
  auto line = [&text](std::string const& value) { text += value + "\n"; };
  auto yesNo = [](bool value) { return value ? "yes" : "no"; };

  line(fmt::format("Vivify {}   outcome: {}", VERSION, outcome));
  line(fmt::format("Device:  {}", _graphicsSummary.empty() ? std::string("(not yet probed)") : _graphicsSummary));
  line("");

  line("Level");
  line(fmt::format("  folder:        {}", _selectedLevelPath.empty() ? std::string("(none)") : _selectedLevelPath));
  line(fmt::format("  bundle loaded: {}", _preloadedBundlePath.empty() ? std::string("(none)") : _preloadedBundlePath));
  line(fmt::format("  converted:     {}",
                   yesNo(!_preloadedBundlePath.empty() &&
                         _preloadedBundlePath.rfind(ConvertedBundleCacheDir(), 0) == 0)));
  line("");

  // The numbers to read first for a freeze: all of this runs on the main thread
  // while the level loads.
  line("Level load timings (main thread)");
  line(fmt::format("  cache bundle assets: {:8.0f} ms", _loadMsCacheAssets));
  line(fmt::format("  decode textures:     {:8.0f} ms", _loadMsDecodeTextures));
  line(fmt::format("  repair shaders:      {:8.0f} ms", _loadMsRepairShaders));
  line(fmt::format("  total:               {:8.0f} ms", _loadMsTotal));
  line("");

  line("Frame watchdog");
  line(fmt::format("  worst frame:        {:8.1f} ms", _worstFrameMs));
  line(fmt::format("  stood down:         {}", yesNo(_selfDisabledThisLevel)));
  if (_selfDisabledThisLevel) {
    line("  Vivify disabled itself for this level after a sustained run of slow");
    line("  frames, so the game kept running instead of freezing.");
  }
  line("");

  line("Shaders in the loaded bundle");
  line(fmt::format("  total:            {}", _auditShadersSeen));
  line(fmt::format("  runnable here:    {}", _auditShadersRunnable));
  line(fmt::format("  no Android build: {}  (DirectX-only; cannot run on any Quest)", _auditShadersNoProgram));
  line(fmt::format("  refused by GPU:   {}  (built for Android, this GPU took none of them)",
                   _auditShadersDeviceRejected));
  for (auto const& name : _auditRejectedNames) {
    line(fmt::format("    refused: {}", name));
  }
  line("");

  line("Shader repair");
  line(fmt::format("  unusable materials: {}", _shaderRepairAttempts));
  line(fmt::format("  given a stand-in:   {}", _shaderRepairSucceeded));
  line(fmt::format("  left broken:        {}", _shaderRepairFailed));
  line("");

  line("Textures");
  line(fmt::format("  decoded:            {}", _texturesDecoded));
  line(fmt::format("  skipped (over time budget): {}", _texturesSkipped));

  if (!_sourceBundleScanText.empty()) {
    line("");
    line("Source bundle before conversion");
    line(fmt::format("  {}", _sourceBundleScanText));
  }
  return text;
}

void Runtime::WriteLevelStartReport() {
  _levelReportOpen = true;
  Report::Append("LEVEL STARTED", BuildLevelReport("still playing (this block is written at load)"));
}

void Runtime::WriteLevelEndReport(std::string_view outcome) {
  // Only for levels that actually opened a report, so leaving the menu does not
  // append an empty block every time something calls ResetRuntime.
  if (!_levelReportOpen) return;
  _levelReportOpen = false;
  Report::Append("LEVEL ENDED", BuildLevelReport(outcome));
}

void Runtime::PreloadBundle(std::string const& bundlePath) {
  if (_preloadedBundlePath == bundlePath && _mainBundle != nullptr &&
      UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify bundle already preloaded: '{}'", bundlePath);
    }
    return;
  }
  if (_mainBundle != nullptr && UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    _mainBundle->Unload(true);
    _mainBundle = nullptr;
  }
  _preloadedBundlePath = bundlePath;
  _mainBundle = UnityEngine::AssetBundle::LoadFromFile(StringW(bundlePath));
  if (_mainBundle == nullptr) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify bundle preload failed: '{}'", bundlePath);
    }
    _preloadedBundlePath.clear();
    return;
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify bundle preloaded: '{}'", bundlePath);
  }
  CacheBundleAssets();
}

void Runtime::LoadMainBundle() {
  LogUnityPlatformInfoOnce();
  if (_selectedLevelPath.empty()) {
    if (_selectedMapHasVivifyRequirement && GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify bundle load skipped: selected level path is empty");
    }
    return;
  }
  // Level selection may have settled on a bundle outside the song folder (a
  // downloaded one, or one converted on device), so that choice wins over
  // re-scanning the folder.
  std::string bundlePath = _selectedBundlePath;
  if (bundlePath.empty() || !std::filesystem::exists(bundlePath)) {
    bundlePath = ResolveBundlePath(_selectedLevelPath);
  }
  if (bundlePath.empty()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify bundle not found in '{}' (no *.vivify file)", _selectedLevelPath);
    }
    return;
  }
  if (!_preloadedBundlePath.empty() && _preloadedBundlePath == bundlePath &&
      _mainBundle != nullptr && UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify bundle preloaded, rebuilding asset caches: '{}'", bundlePath);
    }
    CacheBundleAssets();
    DecodeUnsupportedBundleTextures();
    RepairLoadedMaterialShaders();
    return;
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify loading asset bundle: path='{}'", bundlePath);
  }
  _mainBundle = UnityEngine::AssetBundle::LoadFromFile(StringW(bundlePath));
  if (_mainBundle == nullptr) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify asset bundle load failed: path='{}'", bundlePath);
    }
    return;
  }
  _preloadedBundlePath = bundlePath;

  // Time each phase unconditionally. All of this runs on the main thread while
  // the level is loading, so when someone reports a freeze these three numbers
  // say which phase to look at without needing a repro.
  auto phase = [](char const* name, auto&& work) {
    auto const start = std::chrono::steady_clock::now();
    work();
    double const ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms > 250.0) {
      PaperLogger.warn("Vivify level load: {} took {:.0f}ms", name, ms);
    } else {
      PaperLogger.info("Vivify level load: {} took {:.0f}ms", name, ms);
    }
    return ms;
  };

  _loadMsCacheAssets = phase("cache bundle assets", [this]() { CacheBundleAssets(); });
  _loadMsDecodeTextures = phase("decode textures", [this]() { DecodeUnsupportedBundleTextures(); });
  _loadMsRepairShaders = phase("repair shaders", [this]() { RepairLoadedMaterialShaders(); });
  _loadMsTotal = _loadMsCacheAssets + _loadMsDecodeTextures + _loadMsRepairShaders;
  PaperLogger.info("Vivify level load: {:.0f}ms total", _loadMsTotal);

  // Written now, not at the end of the level: if this map is about to freeze,
  // this block is the only one that will ever reach disk.
  WriteLevelStartReport();
}

UnityEngine::Object* Runtime::LookUpAsset(std::string_view assetName) const {
  auto key = NormalizeAssetKey(assetName);
  if (auto it = _assets.find(key); it != _assets.end()) {
    return it->second;
  }
  if (auto nameIt = _assetsByName.find(key); nameIt != _assetsByName.end()) {
    return nameIt->second;
  }
  return nullptr;
}

UnityEngine::Object* Runtime::GetAssetObject(std::string_view assetName) const {
  auto* asset = LookUpAsset(assetName);
  if (asset == nullptr && GetVivifyDebugLogging()) {
    PaperLogger.warn("Vivify asset lookup miss: '{}'", std::string(assetName));
  }
  return asset;
}

// Names the graphics API, which decides whether a shader stage like a geometry
// shader can exist on this device at all.
//
// Adreno exposes GL_EXT_geometry_shader under OpenGL ES 3.2, but reports
// VkPhysicalDeviceFeatures.geometryShader as false under Vulkan -- Qualcomm has
// never supported geometry or tessellation stages in their Vulkan driver. So on
// Vulkan a geometry shader cannot run here no matter what the bundle contains,
// and no mod-side setting changes that: the graphics API is baked into the
// game's APK at build time.
std::string_view GraphicsApiName(int32_t graphicsDeviceType) {
  switch (graphicsDeviceType) {
    case 0x0b: return "OpenGLES3";
    case 0x10: return "Metal";
    case 0x11: return "OpenGLCore";
    case 0x15: return "Vulkan";
    default: return "other";
  }
}

void Runtime::LogUnityPlatformInfoOnce() {
  if (_loggedUnityPlatformInfo) return;
  _loggedUnityPlatformInfo = true;
  auto stereoMode = UnityEngine::XR::XRSettings::get_stereoRenderingMode();
  auto graphicsType = UnityEngine::SystemInfo::get_graphicsDeviceType();
  int const shaderLevel = UnityEngine::SystemInfo::get_graphicsShaderLevel();

  // Shader level is reported as 10x the shader model: 45 is SM4.5. Geometry
  // stages need SM4.0, so anything below 40 rules them out outright; at or
  // above 40 it comes down to the API above.
  _graphicsSummary = fmt::format(
      "api={} ({}) shaderLevel={} (SM{}.{}) geometryShaderStagePossible={} gpu='{}'",
      GraphicsApiName(graphicsType.value__), graphicsType.value__, shaderLevel, shaderLevel / 10,
      shaderLevel % 10,
      BoolText(shaderLevel >= 40 && graphicsType.value__ != 0x15),
      ToStdString(UnityEngine::SystemInfo::get_graphicsDeviceName()));
  PaperLogger.info("Vivify graphics: {}", _graphicsSummary);

  if (!GetVivifyDebugLogging()) return;

  PaperLogger.info(
      "Vivify platform: os='{}' device='{}' gpu='{}' vendor='{}' api={} stereoMode={} xrOcclusionMesh={} supportsInstancing={} supportsR8={} supportsDepthRT={}",
      ToStdString(UnityEngine::SystemInfo::get_operatingSystem()),
      ToStdString(UnityEngine::SystemInfo::get_deviceModel()),
      ToStdString(UnityEngine::SystemInfo::get_graphicsDeviceName()),
      ToStdString(UnityEngine::SystemInfo::get_graphicsDeviceVendor()),
      graphicsType.value__,
      stereoMode.value__,
      BoolText(UnityEngine::XR::XRSettings::get_useOcclusionMesh()),
      BoolText(UnityEngine::SystemInfo::get_supportsInstancing()),
      BoolText(UnityEngine::SystemInfo::SupportsRenderTextureFormat(UnityEngine::RenderTextureFormat::R8)),
      BoolText(UnityEngine::SystemInfo::SupportsRenderTextureFormat(UnityEngine::RenderTextureFormat::Depth)));
}

UnityEngine::RenderTextureFormat Runtime::SupportedRenderTextureFormat(UnityEngine::RenderTextureFormat requested,
                                                                       std::string_view context) const {
  if (UnityEngine::SystemInfo::SupportsRenderTextureFormat(requested)) {
    return requested;
  }
  auto fallback = UnityEngine::RenderTextureFormat::ARGB32;
  if (GetVivifyDebugLogging()) {
    PaperLogger.warn("Vivify RT format unsupported: context={} requested={} fallback={}",
                     context, requested.value__, fallback.value__);
  }
  return fallback;
}

void Runtime::LogMaterialShader(std::string_view context, std::string_view assetPath, UnityEngine::Material* material) const {
  if (!GetVivifyDebugLogging()) return;
  if (!IsAlive(material)) {
    PaperLogger.warn("Vivify material missing: context={} asset={}", context, assetPath);
    return;
  }
  auto* shader = material->get_shader().unsafePtr();
  auto shaderName = ShaderNameForLog(shader);
  PaperLogger.info("Vivify material: context={} asset={} material='{}' shader='{}' supported={} internalError={}",
                   context,
                   assetPath,
                   ToStdString(material->get_name()),
                   shaderName,
                   BoolText(IsAlive(shader) && shader->get_isSupported()),
                   BoolText(IsInternalErrorShaderName(shaderName)));
}

// Indexes every supported shader loaded in this process by name, once per level.
//
// This is what makes a map's own shader names mean something. A PC bundle does
// not only carry shaders the map author wrote: it carries a *copy* of every
// shader its materials referenced, including the game's own and Unity's
// built-ins, each compiled for DirectX and useless here. A real session log
// shows the result:
//
//   material='snail' shader='BeatSaber/Standard' supported=false
//   material='tube'  shader='BeatSaber/Tube_OptimizedNoise_FastMath' supported=false
//   material='Disco_Lights' shader='Legacy Shaders/Particles/Additive' supported=false
//
// Beat Saber has BeatSaber/Standard. Unity has Legacy Shaders/Particles/
// Additive. Both are sitting in the process, working, and the map wants exactly
// them -- but Shader.Find returns the bundle's broken copy of the same name, it
// fails the isSupported test, and the material was handed a generic stand-in
// instead of the shader it actually asked for.
//
// Scanning for a shader that both matches the name and runs finds the real one.
void Runtime::EnsureGameShaderIndex() {
  if (_gameShaderIndexBuilt) return;
  _gameShaderIndexBuilt = true;

  auto allShaders = UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::Shader*>();
  if (!allShaders) return;
  for (int i = 0; i < allShaders.size(); i++) {
    auto* candidate = allShaders[i];
    // Only shaders that run are indexed, so a bundle's dead copy of a name can
    // never shadow the working one.
    if (!IsAlive(candidate) || !candidate->get_isSupported()) continue;
    auto name = candidate->get_name();
    if (!name) continue;
    std::string key = NormalizeAssetKey(ToStdString(name));
    if (key.empty()) continue;
    _gameShadersByName.emplace(std::move(key), candidate);
  }
  PaperLogger.info("Vivify shader index: {} runnable shader name(s) available in this process",
                   _gameShadersByName.size());
  // Which shaders a Quest build actually ships decides which stand-in Vivify can
  // pick, and that list is not knowable from a PC checkout -- it has to come off
  // a headset. Logging the names once per level costs one line and makes a
  // session log enough to tune the fallback ordering.
  if (!_gameShadersByName.empty()) {
    std::string names;
    for (auto const& entry : _gameShadersByName) {
      if (!names.empty()) names += ", ";
      names += entry.first;
      // One line, not a log flood: the tail is only ever more of the same.
      if (names.size() > 4000) {
        names += ", ...";
        break;
      }
    }
    PaperLogger.info("Vivify shader index contents: {}", names);
  }
}

UnityEngine::Shader* Runtime::FindUsableShader(std::string const& shaderName) {
  if (shaderName.empty()) return nullptr;
  auto const key = NormalizeAssetKey(shaderName);
  if (auto it = _supportedShadersByName.find(key);
      it != _supportedShadersByName.end() && IsAlive(it->second) && it->second->get_isSupported()) {
    return it->second;
  }
  // The map asked for this shader by name and something in the process answers
  // to it. That is a far better answer than a generic stand-in: same name means
  // same properties, so the material's colours and textures land where they
  // were meant to.
  EnsureGameShaderIndex();
  if (auto it = _gameShadersByName.find(key);
      it != _gameShadersByName.end() && IsAlive(it->second) && it->second->get_isSupported()) {
    return it->second;
  }
  // Deliberately the quiet lookup: this searches by *shader* name
  // ("Swifter/VFX/Star"), and the asset maps are keyed by asset path and file
  // name, so a miss here is the normal case rather than a problem. Routing it
  // through GetAssetObject logged one "asset lookup miss" warning per attempt
  // -- 332 of them in a single session, one for every shader the repair pass
  // tried to find a supported twin for.
  auto* bundled = il2cpp_utils::try_cast<UnityEngine::Shader>(LookUpAsset(shaderName)).value_or(nullptr);
  if (IsAlive(bundled) && bundled->get_isSupported()) {
    return bundled;
  }
  auto found = UnityEngine::Shader::Find(StringW(shaderName));
  auto* foundShader = found.unsafePtr();
  if (IsAlive(foundShader) && foundShader->get_isSupported()) {
    return foundShader;
  }
  return nullptr;
}

// Picks a shader that can stand in for one the GPU cannot run.
//
// This used to ask Shader.Find for "Unlit/Texture", "Unlit/Color",
// "Sprites/Default" and "Standard". Shader.Find only resolves shaders that are
// actually included in the build (or already loaded from a bundle), and Unity
// strips built-in shaders nothing references -- so in Beat Saber's IL2CPP build
// every one of those lookups returns null, the repair silently gave up, and the
// material kept a shader that draws nothing. That is why converted bundles came
// up with no models: the meshes and renderers were all there, but every
// material was bound to a dead shader.
//
// Enumerating the shaders the process has actually loaded finds something real.
UnityEngine::Shader* Runtime::FindFallbackShader() {
  if (IsAlive(_fallbackShader) && _fallbackShader->get_isSupported()) {
    return _fallbackShader;
  }
  // A failed search has to be remembered too. This walks every shader object
  // loaded in the process -- thousands, in Beat Saber -- calling get_isSupported,
  // get_name and FindPropertyIndex on each. RepairMaterialShader calls it for
  // every material it cannot fix, and prefab instances bring fresh materials, so
  // without a negative cache one unfixable bundle turns into a full shader-database
  // scan per material per spawn. That is not a slow frame, it is a stopped game.
  if (_fallbackShaderSearchFailed) return nullptr;
  _fallbackShader = nullptr;
  EnsureGameShaderIndex();

  // Shader.Find only resolves shaders included in the build or already loaded
  // from a bundle, and on Quest almost none of Beat Saber's own shaders answer
  // to it -- which is why a by-name search kept falling through to Unity's
  // built-ins. The index built from Resources.FindObjectsOfTypeAll does answer,
  // so it is asked first.
  auto resolveByName = [this](std::string_view name) -> UnityEngine::Shader* {
    auto const key = NormalizeAssetKey(std::string(name));
    if (auto it = _gameShadersByName.find(key); it != _gameShadersByName.end()) {
      if (IsAlive(it->second) && it->second->get_isSupported()) return it->second;
    }
    auto* found = UnityEngine::Shader::Find(StringW(std::string(name))).unsafePtr();
    if (IsAlive(found) && found->get_isSupported()) return found;
    return nullptr;
  };

  // A stand-in that cannot be tinted is why notes came out white. Note and
  // saber replacements are coloured by writing _Color into a
  // MaterialPropertyBlock, so a shader is only fully acceptable here if the
  // colour can actually land on it.
  auto carriesColour = [](UnityEngine::Shader* shader) {
    return shader->FindPropertyIndex(StringW("_Color")) >= 0 ||
           shader->FindPropertyIndex(StringW("_BaseColor")) >= 0;
  };

  // Names worth trying directly, best first. Every entry here shades opaque 3D
  // geometry.
  //
  // Sprites/Default and UI/Default used to be on this list, and that is what
  // turned converted maps black: a sprite shader carries _Color, so requiring
  // _Color promoted it over Unlit/Texture, and then it was handed 3D meshes.
  // Sprites/Default multiplies by the vertex COLOR stream, blends against the
  // frame, and writes no depth -- a mesh with no vertex-colour channel (which
  // is most map geometry) reads whatever the driver leaves in that register,
  // and on the Quest's GLES driver that is zero. Black geometry, blended over
  // a black frame. Neither shader belongs anywhere near a mesh.
  // An explicit choice wins over every heuristic below.
  //
  // Which shader stands in acceptably depends on what a given Beat Saber build
  // ships and how the map lights its scene, and neither is knowable from
  // anywhere but a headset. Rather than another round of guessing, a name from
  // the shader index this logs at level start can be put in
  // standInShaderName in the mod's config and tried at once.
  std::string const requested = GetStandInShaderName();
  if (!requested.empty()) {
    auto* chosen = resolveByName(requested);
    if (chosen != nullptr) {
      _fallbackShader = chosen;
      PaperLogger.info("Vivify fallback shader: using '{}', named by the standInShaderName setting",
                       ShaderNameForLog(chosen));
      return _fallbackShader;
    }
    PaperLogger.warn("Vivify fallback shader: the standInShaderName setting asks for '{}', which is "
                     "not among the runnable shaders on this device; choosing automatically instead",
                     requested);
  }

  // Unlit first, lit last, and that order is the whole point.
  //
  // A Vivify map replaces the environment, and the environment is where Beat
  // Saber's lights live. A lit shader in a scene with no lights returns black
  // no matter what colour or texture is fed to it -- which is what a converted
  // level looked like for several builds while Custom/SimpleLit, a lit shader,
  // sat at the top of this list. The giveaway was that particles still showed:
  // particle materials are unlit and additive, so they were the only things a
  // missing light source could not switch off.
  //
  // The scored scan below has always ranked "unlit" above "simplelit"; this
  // list was overriding it before the scan ever ran.
  static constexpr std::string_view preferredNames[] = {
      "BeatSaber/Unlit Glow"sv,  "Custom/UnlitGlow"sv,     "Unlit/Texture"sv,
      "Unlit/Color"sv,           "Custom/Glowing"sv,       "Custom/GlowingInstancedHD"sv,
      "Custom/OpaqueNeonLight"sv,
      // Everything past here needs a light to show anything at all, and is only
      // reached when the device has none of the above.
      "Custom/SimpleLit"sv,      "Standard"sv,             "Mobile/Diffuse"sv,
      "Legacy Shaders/Diffuse"sv,
  };
  // A named 3D shader that cannot be tinted still beats a sprite shader, so a
  // colourless one is kept as a runner-up rather than discarded outright.
  UnityEngine::Shader* colourlessRunnerUp = nullptr;
  for (auto name : preferredNames) {
    auto* candidate = resolveByName(name);
    if (candidate == nullptr) continue;
    if (carriesColour(candidate)) {
      _fallbackShader = candidate;
      PaperLogger.info("Vivify fallback shader: using '{}' (named candidate, tintable, "
                       "mainTex={})",
                       ShaderNameForLog(candidate),
                       BoolText(candidate->FindPropertyIndex(StringW("_MainTex")) >= 0));
      return _fallbackShader;
    }
    if (colourlessRunnerUp == nullptr) colourlessRunnerUp = candidate;
  }

  // Nothing by name -- score every shader currently loaded and take the best.
  //
  // The category decides the winner and the property bonuses only break ties
  // within a category. They used to be worth 50 each against category scores
  // 10 apart, so "some sprite shader with a texture and a colour" outranked
  // every real lit shader in the process.
  auto scoreShader = [](std::string const& lowerName) -> int {
    // Shaders that exist but would draw nothing useful for arbitrary geometry.
    static constexpr std::string_view excluded[] = {
        "hidden/"sv,   "internal"sv, "text"sv,   "font"sv,    "skybox"sv,
        "shadow"sv,    "depth"sv,    "blit"sv,   "postpro"sv, "compositor"sv,
        "cursor"sv,    "mask"sv,     "stencil"sv, "occlusion"sv,
    };
    for (auto bad : excluded) {
      if (lowerName.find(bad) != std::string::npos) return -1;
    }
    // Sprite and UI shaders sort *below* an unrecognised shader, not above it:
    // they are 2D shaders and drawing a mesh with one is the failure this
    // ordering exists to avoid. They stay on the list only as a last resort.
    if (lowerName.find("sprite") != std::string::npos) return 5;
    if (lowerName.find("ui/") != std::string::npos) return 4;
    if (lowerName.find("unlit") != std::string::npos) return 100;
    if (lowerName.find("simplelit") != std::string::npos) return 90;
    if (lowerName.find("standard") != std::string::npos) return 80;
    if (lowerName.find("glow") != std::string::npos) return 70;
    if (lowerName.find("lit") != std::string::npos) return 60;
    if (lowerName.find("diffuse") != std::string::npos) return 50;
    if (lowerName.find("particle") != std::string::npos) return 15;
    return 10;
  };

  int bestScore = 0;
  auto allShaders = UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::Shader*>();
  if (allShaders) {
    for (int i = 0; i < allShaders.size(); i++) {
      auto* candidate = allShaders[i];
      if (!IsAlive(candidate) || !candidate->get_isSupported()) continue;
      auto name = candidate->get_name();
      if (!name) continue;
      std::string lowerName = ToStdString(name);
      std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      int score = scoreShader(lowerName);
      if (score <= 0) continue;
      // A stand-in is only useful if the original material's look can be
      // carried across. One that exposes neither _MainTex nor a colour renders
      // everything flat white, which is what made converted maps come up
      // partially or fully white. These break ties inside a category; they
      // never promote one category over another.
      score *= 100;
      if (candidate->FindPropertyIndex(StringW("_MainTex")) >= 0) score += 30;
      if (carriesColour(candidate)) score += 40;
      if (score <= bestScore) continue;
      bestScore = score;
      _fallbackShader = candidate;
    }
  }

  if (IsAlive(_fallbackShader)) {
    PaperLogger.info("Vivify fallback shader: scanned loaded shaders, using '{}' (score {})",
                     ShaderNameForLog(_fallbackShader), bestScore);
    return _fallbackShader;
  }

  if (IsAlive(colourlessRunnerUp)) {
    _fallbackShader = colourlessRunnerUp;
    PaperLogger.info("Vivify fallback shader: using '{}' (named candidate; it has no colour "
                     "property, so stand-ins wear their texture untinted)",
                     ShaderNameForLog(_fallbackShader));
    return _fallbackShader;
  }

  _fallbackShaderSearchFailed = true;
  PaperLogger.warn(
      "Vivify found no usable stand-in shader among the shaders loaded in this process. "
      "Materials with unsupported shaders will be left alone rather than rescanning every time.");
  return nullptr;
}

void Runtime::RepairMaterialShader(UnityEngine::Material* material, std::string_view context) {
  if (!IsAlive(material)) return;
  if (_repairedMaterials.contains(material)) return;
  auto shader = material->get_shader();
  auto* rawShader = shader.unsafePtr();
  auto originalShaderName = ShaderNameForLog(rawShader);
  int const originalPassCount = IsAlive(rawShader) ? material->get_passCount() : 0;
  if (GetVivifyDebugLogging() &&
      (!IsAlive(rawShader) || IsInternalErrorShaderName(originalShaderName) ||
       (IsAlive(rawShader) && !rawShader->get_isSupported()))) {
    PaperLogger.warn("Vivify shader diagnostic: context={} material='{}' shader='{}' supported={} internalError={} passes={}",
                     context,
                     ToStdString(material->get_name()),
                     originalShaderName,
                     BoolText(IsAlive(rawShader) && rawShader->get_isSupported()),
                     BoolText(IsInternalErrorShaderName(originalShaderName)),
                     originalPassCount);
  }
  // A shader is only left alone if the GPU says it can actually run it.
  //
  // The "|| originalPassCount > 0" escape hatch that used to be here made every
  // converted PC bundle render nothing: Material.passCount reports the passes
  // declared in the shader's subshaders, which a DirectX-only shader still has
  // on Android even though it carries no GLES program. So every broken shader
  // took this early return, was recorded as repaired, and kept a shader that
  // draws nothing.
  if (IsAlive(rawShader) && rawShader->get_isSupported() &&
      !IsInternalErrorShaderName(originalShaderName)) {
    ApplyStereoKeywords(material);
    _repairedMaterials.emplace(material);
    return;
  }
  _shaderRepairAttempts++;
  auto fallbackState = CaptureMaterialFallbackState(material);
  UnityEngine::Shader* replacement = nullptr;
  if (IsAlive(rawShader)) {
    auto shaderName = rawShader->get_name();
    if (shaderName) {
      replacement = FindUsableShader(ToStdString(shaderName));
    }
  }
  if (!IsAlive(replacement)) {
    replacement = FindFallbackShader();
  }
  // A material with nothing to carry over used to be left with its dead shader,
  // on the reasoning that a flat white mesh is worse than none. In practice it
  // is the other way round, and this is why models go missing on maps whose
  // shaders otherwise work: a material with no colour-shaped property and no
  // texture -- which is most of a raymarch or effect material, whose properties
  // are things like _Speed and _Iterations -- simply never drew. The 0.8.9 log
  // from a real session declined 299 of them in one sitting, and the issue
  // thread describes the result exactly: "blank for some of the intro, then it
  // has the graphics for some of the sections".
  //
  // The stand-in is taken now, tinted a dim neutral grey rather than left at
  // white. That keeps the original worry honest -- a large or full-screen mesh
  // no longer flashes blinding white -- while the geometry is at least present.
  // Anyone who prefers the old behaviour has the "Stand-In Shading" toggle,
  // which is the one case still declined here.
  bool const canCarryLook = fallbackState.color.has_value() || IsManagedAlive(fallbackState.mainTexture);
  bool const usingGenericStandIn = IsAlive(replacement) && replacement == _fallbackShader;
  if (usingGenericStandIn && !GetStandInShading()) {
    _shaderRepairFailed++;
    PaperLogger.warn("Vivify shader stand-in declined: material '{}' (shader '{}') -- "
                     "stand-in shading is turned off in settings",
                     ToStdString(material->get_name()), originalShaderName);
    _repairedMaterials.emplace(material);
    return;
  }
  if (usingGenericStandIn && !canCarryLook && IsScreenSpaceEffectShader(originalShaderName)) {
    // A grey stand-in is right for a mesh and wrong for a screen effect.
    //
    // A blit, a skybox, a stencil mask and a fog volume are all geometry that
    // covers the view: their own shader is what makes them subtle or invisible,
    // and none of that survives the substitution. Painting them opaque grey
    // does not approximate the effect, it hangs a wall in front of the map --
    // which is what a converted level looked like even after two hundred
    // materials were "repaired". These are left undrawn instead, which is what
    // they would have been before the stand-in existed.
    _shaderRepairFailed++;
    _screenEffectsDeclined++;
    PaperLogger.warn("Vivify shader stand-in declined: material '{}' (shader '{}') is a screen or "
                     "masking effect, and a stand-in for one covers the view instead of "
                     "approximating it",
                     ToStdString(material->get_name()), originalShaderName);
    _repairedMaterials.emplace(material);
    return;
  }
  if (usingGenericStandIn && !canCarryLook) {
    // Dim, opaque, and deliberately unlike anything a map would author, so it
    // reads as "this is a stand-in" rather than as the intended look.
    fallbackState.color = UnityEngine::Color(0.25f, 0.25f, 0.28f, 1.0f);
    PaperLogger.warn("Vivify shader stand-in: material '{}' (shader '{}') had no colour or texture "
                     "to carry over, so it is drawn in neutral grey rather than not at all",
                     ToStdString(material->get_name()), originalShaderName);
  }

  if (IsAlive(replacement)) {
    material->set_shader(replacement);
    RestoreMaterialFallbackState(material, fallbackState);
    ApplyStereoKeywords(material);
    _shaderRepairSucceeded++;
    if (replacement == _fallbackShader) {
      // Remember that this material is only wearing a stand-in. Substituting a
      // generic shader lets a mesh be seen, but doing the same for a
      // full-screen blit would smear an unrelated shader over the whole frame,
      // so CanUseBlitMaterial refuses these outright and the blit is skipped.
      _fallbackShadedMaterials.emplace(material);
    }
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify shader repaired: context={} material='{}' from='{}' to='{}' preservedColor={} preservedTexture={}",
                       context, ToStdString(material->get_name()), originalShaderName, ShaderNameForLog(replacement),
                       BoolText(fallbackState.color.has_value()), BoolText(IsManagedAlive(fallbackState.mainTexture)));
    }
  } else {
    _shaderRepairFailed++;
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify shader repair failed: context={} material='{}' original='{}'",
                       context, ToStdString(material->get_name()), originalShaderName);
    }
  }
  _repairedMaterials.emplace(material);
}

void Runtime::RepairGameObjectMaterials(UnityEngine::GameObject* gameObject, std::string_view context) {
  if (!IsAlive(gameObject)) return;
  auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
  for (int i = 0; i < renderers.size(); i++) {
    auto* renderer = renderers[i];
    if (!IsAlive(renderer)) continue;
    auto materials = renderer->get_sharedMaterials();
    if (!materials) continue;
    for (int j = 0; j < materials.size(); j++) {
      RepairMaterialShader(materials[j].unsafePtr(), context);
    }
  }
}

void Runtime::SetMaterialKeyword(UnityEngine::Material* material, ::StringW keyword, bool enabled) const {
  if (!IsAlive(material)) return;
  if (enabled) {
    material->EnableKeyword(keyword);
  } else {
    material->DisableKeyword(keyword);
  }
}

void Runtime::ApplyStereoKeywords(UnityEngine::Material* material) const {
  if (!IsAlive(material)) return;

  SetMaterialKeyword(material, u"MULTIPASS_ENABLED", GetMultipassRenderingEnabled());
}

void Runtime::ApplyGameObjectStereoKeywords(UnityEngine::GameObject* gameObject) {
  if (!IsAlive(gameObject)) return;
  auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
  for (int i = 0; i < renderers.size(); i++) {
    auto* renderer = renderers[i];
    if (!IsAlive(renderer)) continue;
    auto materials = renderer->get_sharedMaterials();
    if (!materials) continue;
    for (int j = 0; j < materials.size(); j++) {
      ApplyStereoKeywords(materials[j].unsafePtr());
    }
  }
}

void Runtime::RefreshLoadedMaterialStereoKeywords() {
  for (auto const& [_, asset] : _assets) {
    if (!IsAlive(asset)) continue;
    if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr); IsAlive(material)) {
      ApplyStereoKeywords(material);
    } else if (auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr); IsAlive(gameObject)) {
      ApplyGameObjectStereoKeywords(gameObject);
    }
  }
}


// Decodes a block-compressed texture this GPU cannot sample into RGBA32.
//
// Quest's Adreno GPUs support ETC2 and ASTC but not S3TC/BC, and a PC-built
// AssetBundle stores its textures as BC1/BC3/BC7. Unity will happily hand back
// the Texture2D object, but nothing can sample it -- which is why converted
// maps came through untextured even once their materials carried the right
// colour. Decoding on the CPU costs memory (BC1 is 4 bits per pixel, RGBA32 is
// 32) but produces something that actually renders.
//
// Requires the source texture's raw bytes to still be available; a texture
// imported without read/write enabled may have had its CPU copy dropped, in
// which case there is nothing to decode and the original is returned unchanged.
UnityEngine::Texture* Runtime::ResolveUsableTexture(UnityEngine::Texture* texture) {
  if (!IsAlive(texture)) return texture;
  if (auto cached = _decodedTextures.find(texture); cached != _decodedTextures.end()) {
    return IsAlive(cached->second) ? cached->second : texture;
  }

  auto* source = il2cpp_utils::try_cast<UnityEngine::Texture2D>(texture).value_or(nullptr);
  if (!IsAlive(source)) return texture;

  int const unityFormat = source->get_format().value__;
  if (UnityEngine::SystemInfo::SupportsTextureFormat(source->get_format())) {
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  auto const format = TextureDecode::FromUnityTextureFormat(unityFormat);
  std::string const name = ToStdString(source->get_name());
  if (format == TextureDecode::Format::Unsupported) {
    PaperLogger.warn("Vivify texture '{}': format {} is unsupported here and cannot be decoded", name, unityFormat);
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  int const width = source->get_width();
  int const height = source->get_height();
  int const mipCount = std::max(1, source->get_mipmapCount());

  ArrayW<uint8_t, Array<uint8_t>*> raw = nullptr;
  try {
    raw = source->GetRawTextureData();
  } catch (...) {
    raw = nullptr;
  }
  if (!raw || raw.size() == 0) {
    PaperLogger.warn("Vivify texture '{}' ({}, {}x{}): no raw data available to decode -- the texture was "
                     "imported without read/write enabled, so its CPU copy is gone",
                     name, TextureDecode::FormatName(format), width, height);
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  std::vector<uint8_t> decoded;
  if (!TextureDecode::DecodeToRgba32(format, raw.begin(), static_cast<size_t>(raw.size()), width, height, mipCount,
                                     decoded)) {
    PaperLogger.warn("Vivify texture '{}' ({}, {}x{}, {} mip(s)): {} byte(s) of data did not decode",
                     name, TextureDecode::FormatName(format), width, height, mipCount, raw.size());
    _decodedTextures[texture] = nullptr;
    return texture;
  }

  auto* replacement = UnityEngine::Texture2D::New_ctor(width, height, UnityEngine::TextureFormat::RGBA32,
                                                       mipCount, false);
  if (!IsAlive(replacement)) {
    _decodedTextures[texture] = nullptr;
    return texture;
  }
  auto managed = ArrayW<uint8_t, Array<uint8_t>*>(static_cast<il2cpp_array_size_t>(decoded.size()));
  std::memcpy(managed.begin(), decoded.data(), decoded.size());
  replacement->LoadRawTextureData(managed);
  replacement->Apply();
  replacement->set_wrapMode(source->get_wrapMode());
  replacement->set_filterMode(source->get_filterMode());
  replacement->set_name(StringW(name + " (decoded)"));

  PaperLogger.info("Vivify texture decoded: '{}' {} {}x{} ({} mip(s)) -> RGBA32", name,
                   TextureDecode::FormatName(format), width, height, mipCount);
  _decodedTextures[texture] = replacement;
  return replacement;
}

// Walks every material in the bundle and swaps any texture this GPU cannot
// sample for a decoded copy. Runs once per bundle load, before shader repair,
// so a material that keeps its own working shader still gets usable textures.
void Runtime::DecodeUnsupportedBundleTextures() {
  int swapped = 0;
  int skipped = 0;
  _texturesScannedMaterials.clear();

  // Block-compressed decoding is real CPU work on the main thread: a 2048x2048
  // BC7 texture is four million pixels, and a bundle can hold dozens. Left
  // unbounded it stalls the game for as long as it takes, which is
  // indistinguishable from a freeze. Decode what fits in the budget, skip the
  // rest, and say how many were skipped -- a few untextured materials beat a
  // hung game.
  // Raised from two seconds along with the prefab walk above. That walk finds
  // the materials that carry most of a map's textures, so the old budget --
  // sized for the handful of standalone Material assets -- would now be spent
  // long before the scene geometry was reached, and a skipped texture is a
  // black one. A converted map already pays seconds for the conversion itself;
  // several more on first load beat a level that cannot be seen.
  constexpr double kDecodeBudgetMs = 8000.0;
  auto const start = std::chrono::steady_clock::now();
  auto elapsedMs = [&start]() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  };

  auto decodeMaterial = [&](UnityEngine::Material* material) {
    if (!IsAlive(material)) return;
    // A material reached through several renderers is the same material; doing
    // it twice would only spend budget.
    if (!_texturesScannedMaterials.emplace(material).second) return;
    auto names = material->GetPropertyNames(UnityEngine::MaterialPropertyType::Texture);
    if (!names) return;
    for (auto name : names) {
      if (!name) continue;
      auto* current = material->GetTexture(name).unsafePtr();
      if (!IsAlive(current)) continue;
      // An already-decoded texture is a cache hit and costs nothing, so the
      // budget only gates work that has not been done yet.
      if (elapsedMs() > kDecodeBudgetMs && !_decodedTextures.contains(current)) {
        skipped++;
        continue;
      }
      auto* usable = ResolveUsableTexture(current);
      if (IsAlive(usable) && usable != current) {
        material->SetTexture(name, usable);
        swapped++;
      }
    }
  };

  // The same two kinds of asset the shader repair walks, and for the same
  // reason.
  //
  // This used to consider only Material assets. Nearly every material in a
  // Vivify map is not one: it hangs off a renderer inside a prefab
  // (assets/.../prefabs/scene1.prefab and friends), which is exactly why
  // RepairLoadedMaterialShaders walks GameObjects as well. So scene geometry
  // had its shader repaired and its textures left as DirectX block-compressed
  // data that an Adreno cannot sample -- which reads as black. A material with
  // no texture at all was unaffected and drew as a flat pale shape, so the
  // symptom was a black level with a few white objects and the particles still
  // showing.
  for (auto const& [path, asset] : _assets) {
    if (!IsAlive(asset)) continue;
    if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr);
        IsAlive(material)) {
      decodeMaterial(material);
      continue;
    }
    auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr);
    if (!IsAlive(gameObject)) continue;
    auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
    for (int i = 0; i < renderers.size(); i++) {
      auto* renderer = renderers[i];
      if (!IsAlive(renderer)) continue;
      auto materials = renderer->get_sharedMaterials();
      if (!materials) continue;
      for (int j = 0; j < materials.size(); j++) {
        decodeMaterial(materials[j].unsafePtr());
      }
    }
  }
  _texturesDecoded = swapped;
  _texturesSkipped = skipped;
  // Logged even when nothing was found, and that is the point: this pass
  // printing nothing at all is how it went unnoticed that it was looking in the
  // wrong place. "scanned 0" is a symptom; silence is not.
  PaperLogger.info(
      "Vivify texture decode: scanned {} material(s), replaced {} unsupported texture "
      "reference(s) in {:.0f}ms, skipped {} over budget",
      _texturesScannedMaterials.size(), swapped, elapsedMs(), skipped);
  if (skipped > 0) {
    PaperLogger.warn(
        "Vivify stopped decoding textures after {:.0f}ms and left {} reference(s) on their original, "
        "unsampleable format. Those materials render untextured rather than stalling the game.",
        kDecodeBudgetMs, skipped);
  }
}

void Runtime::RepairLoadedMaterialShaders() {
  _shaderRepairAttempts = 0;
  _shaderRepairSucceeded = 0;
  _shaderRepairFailed = 0;
  _screenEffectsDeclined = 0;
  for (auto const& [path, asset] : _assets) {
    if (!IsAlive(asset)) continue;
    if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr); IsAlive(material)) {
      RepairMaterialShader(material, path);
    } else if (auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr); IsAlive(gameObject)) {
      RepairGameObjectMaterials(gameObject, path);
    }
  }
  if (_shaderRepairAttempts > 0) {
    // Worth logging unconditionally: a bundle whose shaders all had to be
    // replaced is a converted PC bundle rendering with stand-in shading, and a
    // non-zero failure count means some of it will not draw at all.
    PaperLogger.info("Vivify shader repair: {} screen/masking effect(s) left undrawn on purpose",
                   _screenEffectsDeclined);
  PaperLogger.info("Vivify shader repair: {} material(s) had an unusable shader, {} replaced, {} could not be",
                     _shaderRepairAttempts, _shaderRepairSucceeded, _shaderRepairFailed);
  }
}


// ---------------------------------------------------------------------------
// Bulk conversion
//
// A map that ships only a PC bundle has its play button disabled, so there is
// no way to reach it through normal level selection -- which also means no way
// to trigger a per-level conversion. This pass walks every installed custom
// level directly and converts anything convertible, so those maps become
// playable without having to be playable first.
// ---------------------------------------------------------------------------

namespace {
std::atomic<bool> gBulkConversionRunning{false};

std::vector<std::filesystem::path> CollectCustomLevelDirectories() {
  std::vector<std::filesystem::path> roots;
  for (auto const& root : SongCore::API::Loading::GetRootCustomLevelPaths()) roots.push_back(root);
  for (auto const& root : SongCore::API::Loading::GetRootCustomWIPLevelPaths()) roots.push_back(root);

  std::vector<std::filesystem::path> levels;
  std::error_code ec;
  for (auto const& root : roots) {
    if (!std::filesystem::is_directory(root, ec) || ec) {
      ec.clear();
      continue;
    }
    for (auto const& entry : std::filesystem::directory_iterator(root, ec)) {
      if (ec) break;
      if (entry.is_directory(ec) && !ec) levels.push_back(entry.path());
      ec.clear();
    }
    ec.clear();
  }
  return levels;
}
}

bool IsBulkPcBundleConversionRunning() {
  return gBulkConversionRunning.load();
}

void StartBulkPcBundleConversion(std::function<void(BulkConversionProgress const&)> onProgress, bool force) {
  bool expected = false;
  if (!gBulkConversionRunning.compare_exchange_strong(expected, true)) {
    return;
  }

  // SongCore's level roots are enumerated here, on the caller's (main) thread,
  // rather than inside the worker: a song refresh can rewrite them, and the
  // worker only needs the snapshot.
  auto levels = CollectCustomLevelDirectories();

  std::thread([onProgress = std::move(onProgress), levels = std::move(levels), force]() {
    auto report = [&onProgress](BulkConversionProgress progress) {
      if (!onProgress) return;
      BSML::MainThreadScheduler::Schedule([onProgress, progress]() { onProgress(progress); });
    };

    BulkConversionProgress progress;
    try {
      progress.levelsTotal = static_cast<int>(levels.size());
      progress.status = std::string(force ? "Reconverting " : "Scanning ") +
                        std::to_string(progress.levelsTotal) + " level(s)...";
      report(progress);

      for (auto const& level : levels) {
        progress.levelsScanned++;
        std::string const levelPath = level.string();

        // Maps that already have an Android bundle need nothing.
        if (std::filesystem::exists(JoinPath(levelPath, std::string(kBundleFile)))) continue;

        std::string const source = ResolvePcBundlePath(levelPath);
        if (source.empty()) continue;

        std::string const dest = ConvertedBundlePath(source);
        if (std::filesystem::exists(dest)) {
          if (!force && CachedConversionIsCurrent(dest)) {
            progress.alreadyDone++;
            PaperLogger.info("Vivify bulk convert: '{}' already cached at '{}'", source, dest);
            continue;
          }
          if (!force) {
            PaperLogger.info("Vivify bulk convert: cached '{}' predates this converter, redoing it",
                             dest);
          }
          // Forced, or cached by an older converter: drop the file so the
          // conversion actually re-runs.
          // ConvertToAndroid writes through a .part file and renames, so a
          // failure after this point leaves no cached bundle rather than a
          // truncated one -- the level falls back to being unconverted, which
          // is the state it would have been in anyway.
          std::error_code ec;
          std::filesystem::remove(dest, ec);
          if (ec) {
            progress.failed++;
            PaperLogger.warn("Vivify bulk convert: could not remove cached '{}' to reconvert: {}",
                             dest, ec.message());
            continue;
          }
          PaperLogger.info("Vivify bulk convert: discarded cached '{}' to reconvert", dest);
        }

        progress.status = level.filename().string();
        report(progress);

        auto const result = RunBundleConversion(source, dest);
        if (result.status == BundleConvert::Status::Success) {
          progress.converted++;
          PaperLogger.info("Vivify bulk convert: '{}' -> '{}' ({})", source, dest, result.message);
        } else if (result.status == BundleConvert::Status::AlreadyAndroid) {
          progress.alreadyDone++;
        } else {
          progress.failed++;
          PaperLogger.warn("Vivify bulk convert failed for '{}' ({}): {}", source,
                           std::string(BundleConvert::StatusText(result.status)), result.message);
        }
        report(progress);
      }

      progress.status = "Converted " + std::to_string(progress.converted) + ", already done " +
                        std::to_string(progress.alreadyDone) + ", failed " + std::to_string(progress.failed);
      PaperLogger.info("Vivify bulk convert finished: scanned={} converted={} alreadyDone={} failed={}",
                       progress.levelsScanned, progress.converted, progress.alreadyDone, progress.failed);
    } catch (std::exception const& ex) {
      progress.status = std::string("Conversion pass failed: ") + ex.what();
      PaperLogger.error("Vivify bulk convert threw: {}", ex.what());
    } catch (...) {
      progress.status = "Conversion pass failed";
      PaperLogger.error("Vivify bulk convert threw a non-std exception");
    }

    progress.finished = true;
    report(progress);
    gBulkConversionRunning.store(false);
  }).detach();
}

}
