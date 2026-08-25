#include "VivifyRuntimeInternal.hpp"
#include "VivifyComponents.hpp"
#include "VivifyBundleConvert.hpp"
#include "VivifyTextureDecode.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace Vivify {

namespace {

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
// Bundle file names are not standardised. This port's own download path writes
// "bundleAndroid2021.vivify", but a map authored for PC ships whatever Vivify's
// Unity exporter produced -- commonly "bundleWindows2019" or
// "bundleWindows2021" with NO extension at all. The previous ".vivify"-only
// scan therefore found nothing on exactly the maps the conversion path exists
// to rescue: they were reported as "does not support your game version", the
// play button stayed disabled, and no bundle was ever offered for conversion.
//
// Every candidate is checked for the UnityFS signature instead, so the name
// does not matter. Names are used only to rank equally-valid candidates: a
// "windows" name wins over a generic "bundle" name, which wins over anything
// else that happens to be a Unity archive.
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
  ResetRuntime();

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

  MetaCore::Game::SetScoreSubmission("Vivify", false);

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
  bool const haveCachedConversion = !cachedConversion.empty() && std::filesystem::exists(cachedConversion);

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
  if (std::filesystem::exists(destPath)) {
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
    BundleConvert::Result result = BundleConvert::ConvertToAndroid(sourceBundlePath, destPath);
    // A bundle that was already Android-targeted needs no rewrite; load it as-is.
    std::string loadPath = result.status == BundleConvert::Status::AlreadyAndroid ? sourceBundlePath : destPath;
    bool const usable = result.ok() || result.status == BundleConvert::Status::AlreadyAndroid;
    std::string const message = result.message;
    std::string const statusText{BundleConvert::StatusText(result.status)};

    BSML::MainThreadScheduler::Schedule([this, levelPath, sourceBundlePath, loadPath, usable, message, statusText]() {
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

void Runtime::CacheBundleAssets() {
  if (_mainBundle == nullptr || !UnityEngine::Object::op_Implicit_bool(_mainBundle)) return;
  auto assetNames = _mainBundle->GetAllAssetNames();
  if (!assetNames) return;
  _assets.clear();
  _assetsByName.clear();
  _supportedShadersByName.clear();
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
          IsAlive(shader) && shader->get_isSupported() && !nameKey.empty()) {
        _supportedShadersByName[nameKey] = shader;
      }
    }
    if (GetVivifyDebugLogging()) {
      if (auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr);
          IsAlive(material)) {
        LogMaterialShader("bundle-load", originalAssetPath, material);
      } else if (auto* shader = il2cpp_utils::try_cast<UnityEngine::Shader>(asset).value_or(nullptr);
                 IsAlive(shader)) {
        PaperLogger.info("Vivify shader asset: path='{}' shader='{}' supported={}",
                         originalAssetPath, ShaderNameForLog(shader), BoolText(shader->get_isSupported()));
      }
    }
  }
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
  CacheBundleAssets();
  DecodeUnsupportedBundleTextures();
  RepairLoadedMaterialShaders();
}

UnityEngine::Object* Runtime::GetAssetObject(std::string_view assetName) const {
  auto it = _assets.find(NormalizeAssetKey(assetName));
  if (it != _assets.end()) {
    return it->second;
  }
  auto nameIt = _assetsByName.find(NormalizeAssetKey(assetName));
  if (nameIt != _assetsByName.end()) {
    return nameIt->second;
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.warn("Vivify asset lookup miss: '{}'", std::string(assetName));
  }
  return nullptr;
}

void Runtime::LogUnityPlatformInfoOnce() {
  if (!GetVivifyDebugLogging() || _loggedUnityPlatformInfo) return;
  _loggedUnityPlatformInfo = true;
  auto stereoMode = UnityEngine::XR::XRSettings::get_stereoRenderingMode();
  auto graphicsType = UnityEngine::SystemInfo::get_graphicsDeviceType();
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

UnityEngine::Shader* Runtime::FindUsableShader(std::string const& shaderName) const {
  if (shaderName.empty()) return nullptr;
  if (auto it = _supportedShadersByName.find(NormalizeAssetKey(shaderName));
      it != _supportedShadersByName.end() && IsAlive(it->second) && it->second->get_isSupported()) {
    return it->second;
  }
  auto* bundled = il2cpp_utils::try_cast<UnityEngine::Shader>(GetAssetObject(shaderName)).value_or(nullptr);
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
  _fallbackShader = nullptr;

  // Names worth trying directly, cheapest first. Beat Saber's own shaders come
  // before Unity's built-ins because they are the ones actually present.
  static constexpr std::string_view preferredNames[] = {
      "Custom/SimpleLit"sv, "Custom/Glowing"sv,     "BeatSaber/Unlit Glow"sv,
      "Unlit/Texture"sv,    "Unlit/Color"sv,        "Sprites/Default"sv,
      "UI/Default"sv,       "Standard"sv,
  };
  for (auto name : preferredNames) {
    auto* candidate = UnityEngine::Shader::Find(StringW(std::string(name))).unsafePtr();
    if (IsAlive(candidate) && candidate->get_isSupported()) {
      _fallbackShader = candidate;
      PaperLogger.info("Vivify fallback shader: using '{}'", ShaderNameForLog(candidate));
      return _fallbackShader;
    }
  }

  // Nothing by name -- score every shader currently loaded and take the best.
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
    if (lowerName.find("unlit") != std::string::npos) return 100;
    if (lowerName.find("simplelit") != std::string::npos) return 90;
    if (lowerName.find("standard") != std::string::npos) return 80;
    if (lowerName.find("glow") != std::string::npos) return 70;
    if (lowerName.find("lit") != std::string::npos) return 60;
    if (lowerName.find("sprite") != std::string::npos) return 30;
    if (lowerName.find("ui/") != std::string::npos) return 20;
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
      int const score = scoreShader(lowerName);
        int score = scoreShader(lowerName);
      // A stand-in is only useful if the original material's look can be
      // carried across. One that exposes neither _MainTex nor a colour renders
      // everything flat white, which is what made converted maps come up
      // partially or fully white.
      if (score > 0) {
        if (candidate->FindPropertyIndex(StringW("_MainTex")) >= 0) score += 50;
        if (candidate->FindPropertyIndex(StringW("_Color")) >= 0 ||
            candidate->FindPropertyIndex(StringW("_BaseColor")) >= 0) {
          score += 50;
        }
      }
      if (score <= bestScore) continue;
      bestScore = score;
      _fallbackShader = candidate;
    }
  }

  if (IsAlive(_fallbackShader)) {
    PaperLogger.info("Vivify fallback shader: scanned loaded shaders, using '{}' (score {})",
                     ShaderNameForLog(_fallbackShader), bestScore);
  } else {
    PaperLogger.error("Vivify fallback shader: no usable shader found; assets with unsupported "
                      "shaders will not render");
  }
  return _fallbackShader;
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
  // Substituting a stand-in trades "invisible" for "visible but wrong". That is
  // only a good trade when something of the original look survives: if neither
  // a colour nor a texture could be recovered, the stand-in paints an arbitrary
  // flat white shape over the scene, which for a large or full-screen mesh is
  // considerably worse than the object simply not drawing. Leave the dead
  // shader in place instead -- for notes and sabers ReplacementCanRender then
  // keeps the game's own visuals, which look right.
  bool const canCarryLook = fallbackState.color.has_value() || IsManagedAlive(fallbackState.mainTexture);
  bool const declineStandIn = IsAlive(replacement) && replacement == _fallbackShader &&
                              (!canCarryLook || !GetStandInShading());
  if (declineStandIn) {
    _shaderRepairFailed++;
    PaperLogger.warn("Vivify shader stand-in declined: material '{}' (shader '{}') -- {}",
                     ToStdString(material->get_name()), originalShaderName,
                     !GetStandInShading() ? "stand-in shading is turned off in settings"
                                          : "no colour or texture to carry over, so it would render flat white");
    _repairedMaterials.emplace(material);
    return;
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
  for (auto const& [path, asset] : _assets) {
    auto* material = il2cpp_utils::try_cast<UnityEngine::Material>(asset).value_or(nullptr);
    if (!IsAlive(material)) continue;
    auto names = material->GetPropertyNames(UnityEngine::MaterialPropertyType::Texture);
    if (!names) continue;
    for (auto name : names) {
      if (!name) continue;
      auto* current = material->GetTexture(name).unsafePtr();
      if (!IsAlive(current)) continue;
      auto* usable = ResolveUsableTexture(current);
      if (IsAlive(usable) && usable != current) {
        material->SetTexture(name, usable);
        swapped++;
      }
    }
  }
  if (swapped > 0) {
    PaperLogger.info("Vivify texture decode: replaced {} unsupported texture reference(s) across bundle materials",
                     swapped);
  }
}

void Runtime::RepairLoadedMaterialShaders() {
  _shaderRepairAttempts = 0;
  _shaderRepairSucceeded = 0;
  _shaderRepairFailed = 0;
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

void StartBulkPcBundleConversion(std::function<void(BulkConversionProgress const&)> onProgress) {
  bool expected = false;
  if (!gBulkConversionRunning.compare_exchange_strong(expected, true)) {
    return;
  }

  // SongCore's level roots are enumerated here, on the caller's (main) thread,
  // rather than inside the worker: a song refresh can rewrite them, and the
  // worker only needs the snapshot.
  auto levels = CollectCustomLevelDirectories();

  std::thread([onProgress = std::move(onProgress), levels = std::move(levels)]() {
    auto report = [&onProgress](BulkConversionProgress progress) {
      if (!onProgress) return;
      BSML::MainThreadScheduler::Schedule([onProgress, progress]() { onProgress(progress); });
    };

    BulkConversionProgress progress;
    try {
      progress.levelsTotal = static_cast<int>(levels.size());
      progress.status = "Scanning " + std::to_string(progress.levelsTotal) + " level(s)...";
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
          progress.alreadyDone++;
          PaperLogger.info("Vivify bulk convert: '{}' already cached at '{}'", source, dest);
          continue;
        }

        progress.status = level.filename().string();
        report(progress);

        auto const result = BundleConvert::ConvertToAndroid(source, dest);
        if (result.ok()) {
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
