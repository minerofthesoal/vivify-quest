#include "main.hpp"
#include "VivifyRuntime.hpp"
#include "VivifyReport.hpp"
#include <string>
#include <string_view>
#include <fstream>
#include <chrono>
#include <mutex>
#include <filesystem>
#include "HMUI/ViewController.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "bsml/shared/BSML-Lite/Creation/Buttons.hpp"
#include "bsml/shared/BSML-Lite/Creation/Layout.hpp"
#include "bsml/shared/BSML-Lite/Creation/Settings.hpp"
#include "bsml/shared/BSML-Lite/Creation/Text.hpp"
#include "beatsaber-hook/shared/utils/typedefs-wrappers.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "bsml/shared/BSML/Settings/BSMLSettings.hpp"
#include "custom-types/shared/register.hpp"
#include "scotland2/shared/modloader.h"

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

namespace {
constexpr std::string_view kMultipassRenderingConfigKey = "multipassRendering";
constexpr std::string_view kVivifyDebugLoggingConfigKey = "vivifyDebugLogging";
constexpr std::string_view kDisableBeat0FilmgrainBlitConfigKey = "disableBeat0FilmgrainBlit";
constexpr std::string_view kDisableAllBlitsConfigKey = "disableAllBlits";
constexpr std::string_view kDisableCreateCameraDepthConfigKey = "disableCreateCameraDepth";
// Ported from the rbatteries1-design/Lars27110 base.
constexpr std::string_view kDisableCustomNoteVisualsConfigKey = "disableCustomNoteVisuals";
constexpr std::string_view kDisableVisualsInMultiplayerConfigKey = "disableVisualsInMultiplayer";
constexpr std::string_view kDisableVRCenterAdjustConfigKey = "disableVRCenterAdjust";
constexpr std::string_view kConvertPcBundlesOnDeviceConfigKey = "convertPcBundlesOnDevice";
constexpr std::string_view kStandInShadingConfigKey = "standInShading";
bool gMultipassRenderingEnabled = true;
bool gVivifyDebugLogging = false;
bool gDisableBeat0FilmgrainBlit = false;
bool gDisableAllBlits = false;
bool gDisableCreateCameraDepth = false;
bool gDisableCustomNoteVisuals = false;
// Defaults to true, matching the base this was ported from: Vivify's world-space
// note/saber/debris replacements aren't validated for multiplayer lobbies, so they
// stay off there unless the player opts back in.
bool gDisableVisualsInMultiplayer = true;
bool gDisableVRCenterAdjust = false;
// Replaces the old "allowUnsafeWindowsBundleFallback" toggle. That one handed a
// PC-built AssetBundle straight to Unity, which simply reports no assets on
// Android. This one instead retargets the archive to Android on device first
// (see VivifyBundleConvert), so the geometry/prefabs in it actually load.
// Defaults on: it only ever runs when a map has no Android bundle at all and no
// downloadable one, i.e. when the alternative is an unplayable map.
bool gConvertPcBundlesOnDevice = true;
// When a bundle's shader cannot run on this GPU, Vivify swaps in a generic
// stand-in so the mesh is at least visible. That trades "invisible" for
// "visible but wrong", and for a converted PC bundle "wrong" often means flat
// white. Turning this off leaves such meshes undrawn instead, which also means
// notes and sabers keep the game's own visuals rather than a white stand-in.
bool gStandInShading = true;

// Both diagnostic files live beside the mod's own data, and both are .txt.
//
// This used to be Logs/Vivify.log. A .log file has no default handler on Android
// or Windows, so tapping it does nothing and it looks like no log exists at all
// -- and it sat in a different directory from the per-level report, so there
// were two places to look. One directory, two .txt files, both openable.
constexpr std::string_view kVivifyLogDir = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify";
constexpr std::string_view kVivifyLogPath =
    "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifySession.txt";

// A session log must not fill a headset, and it is truncated at launch anyway,
// so this only has to bound one play session.
constexpr std::streamoff kVivifyLogMaxBytes = 8 * 1024 * 1024;

std::ofstream gVivifyLogFile;
std::mutex gVivifyLogMutex;
bool gVivifyLogSinkInstalled = false;
bool gVivifyLogCapped = false;
std::chrono::steady_clock::time_point gVivifyLogLastFlush{};

void InstallVivifyFileLogSink() {
  if (gVivifyLogSinkInstalled) return;
  gVivifyLogSinkInstalled = true;
  std::error_code ec;
  std::filesystem::create_directories(std::string(kVivifyLogDir), ec);

  gVivifyLogFile.open(std::string(kVivifyLogPath), std::ios::out | std::ios::trunc);
  if (!gVivifyLogFile.is_open()) {
    PaperLogger.warn("Vivify: could not open log file at {} (logging to logcat only)", kVivifyLogPath);
    return;
  }
  gVivifyLogFile << "=== Vivify " << VERSION << " session log ===\n";
  gVivifyLogFile.flush();
  gVivifyLogLastFlush = std::chrono::steady_clock::now();

  Paper::Logger::AddLogSink([](Paper::LogData const& data) {
    if (!data.tag.has_value() || *data.tag != std::string_view(MOD_ID)) return;
    std::lock_guard<std::mutex> lock(gVivifyLogMutex);
    if (!gVivifyLogFile.is_open() || gVivifyLogCapped) return;

    gVivifyLogFile << '[' << Paper::format_as(data.level) << "] " << data.message << '\n';

    if (gVivifyLogFile.tellp() > kVivifyLogMaxBytes) {
      gVivifyLogFile << "=== log capped at " << (kVivifyLogMaxBytes / (1024 * 1024))
                     << "MB; further lines go to logcat only ===\n";
      gVivifyLogFile.flush();
      gVivifyLogCapped = true;
      return;
    }

    // Flushing every line meant an sdcard write per log line, on whichever
    // thread logged -- including the main thread mid-gameplay, where Vivify can
    // be noisy. Warnings and errors still flush immediately, because those are
    // the lines that matter if the game stops before the buffer is written;
    // ordinary lines are flushed at most a few times a second.
    auto const now = std::chrono::steady_clock::now();
    bool const important = data.level >= Paper::LogLevel::WRN;
    if (important || now - gVivifyLogLastFlush > std::chrono::milliseconds(250)) {
      gVivifyLogFile.flush();
      gVivifyLogLastFlush = now;
    }
  });
}

void EnsureConfigObject() {
  auto& doc = getConfig().config;
  if (!doc.IsObject()) {
    doc.SetObject();
  }
}

bool EnsureBoolConfigValue(std::string_view key, bool defaultValue, bool& value) {
  auto& doc = getConfig().config;
  auto it = doc.FindMember(key.data());
  if (it != doc.MemberEnd() && it->value.IsBool()) {
    value = it->value.GetBool();
    return false;
  }

  auto& allocator = doc.GetAllocator();
  value = defaultValue;
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), rapidjson::Value(defaultValue), allocator);
  } else {
    it->value.SetBool(defaultValue);
  }
  return true;
}

void SetBoolConfigValue(std::string_view key, bool enabled, bool& value) {
  auto& config = getConfig();
  auto& doc = config.config;
  EnsureConfigObject();
  auto& allocator = doc.GetAllocator();
  auto it = doc.FindMember(key.data());
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), rapidjson::Value(enabled), allocator);
  } else {
    it->value.SetBool(enabled);
  }
  value = enabled;
  config.Write();
}

// Label under the bulk-convert button. The settings view controller is
// destroyed and rebuilt as the player navigates and conversion progress
// arrives asynchronously, so SafePtrUnity is used for its destroyed-object
// aware liveness check -- a plain SafePtr is not even permitted for Unity
// types.
SafePtrUnity<HMUI::CurvedTextMeshPro> gConvertStatusText;

void SetConvertStatusText(std::string const& text) {
  if (!gConvertStatusText) return;
  gConvertStatusText->set_text(StringW(text));
}

void RegisterModSettings() {
  BSML::BSMLSettings::get_instance()->TryAddSettingsMenu(
      [](HMUI::ViewController* viewController, bool firstActivation, bool, bool) {
        if (!firstActivation || viewController == nullptr) return;
        auto* container = BSML::Lite::CreateScrollableSettingsContainer(viewController->get_transform());
        if (container == nullptr) return;
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Debug logging", GetVivifyDebugLogging(),
            [](bool value) { SetBoolConfigValue(kVivifyDebugLoggingConfigKey, value, gVivifyDebugLogging); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable Beat 0 Filmgrain Blit", GetDisableBeat0FilmgrainBlit(),
            [](bool value) { SetBoolConfigValue(kDisableBeat0FilmgrainBlitConfigKey, value, gDisableBeat0FilmgrainBlit); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable All Blits", GetDisableAllBlits(),
            [](bool value) { SetBoolConfigValue(kDisableAllBlitsConfigKey, value, gDisableAllBlits); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable CreateCamera Depth", GetDisableCreateCameraDepth(),
            [](bool value) { SetBoolConfigValue(kDisableCreateCameraDepthConfigKey, value, gDisableCreateCameraDepth); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable Custom Note Visuals", GetDisableCustomNoteVisuals(),
            [](bool value) { SetBoolConfigValue(kDisableCustomNoteVisualsConfigKey, value, gDisableCustomNoteVisuals); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable Vivify Visuals In Multiplayer", GetDisableVisualsInMultiplayer(),
            [](bool value) { SetBoolConfigValue(kDisableVisualsInMultiplayerConfigKey, value, gDisableVisualsInMultiplayer); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Disable VR Center Adjust Handling", GetDisableVRCenterAdjust(),
            [](bool value) { SetBoolConfigValue(kDisableVRCenterAdjustConfigKey, value, gDisableVRCenterAdjust); });
        BSML::Lite::CreateToggle(
            container->get_transform(), u"Convert PC Bundles On Device",
            GetConvertPcBundlesOnDevice(),
            [](bool value) {
              SetBoolConfigValue(kConvertPcBundlesOnDeviceConfigKey, value, gConvertPcBundlesOnDevice);
            });

        BSML::Lite::CreateToggle(
            container->get_transform(), u"Stand-In Shading For Unsupported Shaders",
            GetStandInShading(),
            [](bool value) { SetBoolConfigValue(kStandInShadingConfigKey, value, gStandInShading); });

        // A map whose only asset bundle is a PC build has its play button
        // disabled, so it can never be selected into -- which also means the
        // per-level conversion that runs on level select can never fire for it.
        // This button converts every installed map in one pass instead, so
        // those levels become playable without having to be playable first.
        gConvertStatusText = BSML::Lite::CreateText(container->get_transform(), u"Idle");
        if (Vivify::IsBulkPcBundleConversionRunning()) SetConvertStatusText("Converting...");
        // One progress callback for both buttons; they differ only in whether an
        // already-cached conversion is reused or thrown away first.
        static auto const startConversion = [](bool force) {
          if (Vivify::IsBulkPcBundleConversionRunning()) return;
          SetConvertStatusText(force ? "Reconverting..." : "Scanning...");
          Vivify::StartBulkPcBundleConversion(
              [](Vivify::BulkConversionProgress const& progress) {
                if (progress.finished) {
                  SetConvertStatusText(progress.status);
                  return;
                }
                SetConvertStatusText(std::to_string(progress.levelsScanned) + "/" +
                                     std::to_string(progress.levelsTotal) + "  " + progress.status);
              },
              force);
        };

        BSML::Lite::CreateUIButton(
            container->get_transform(), u"Convert All PC Bundles Now",
            []() { startConversion(false); });

        // A cached conversion is keyed on the source bundle, so it is reused
        // even after the converter itself has been fixed. This is the way to
        // pick those fixes up without deleting the cache directory by hand.
        BSML::Lite::CreateUIButton(
            container->get_transform(), u"Force Reconvert All (ignore cache)",
            []() { startConversion(true); });

        // paperlog output is not reachable without adb, so Vivify writes its own
        // plain-text report next to its data. Showing the path here means the
        // file can be found without being told where to look.
        BSML::Lite::CreateText(container->get_transform(),
                               u"Diagnostics (both plain .txt, same folder):");
        BSML::Lite::CreateText(
            container->get_transform(),
            StringW("<size=70%>" + Vivify::Report::FilePath() + "</size>"));
        BSML::Lite::CreateText(
            container->get_transform(),
            StringW("<size=70%>" + std::string(kVivifyLogPath) + "</size>"));
      },
      "Vivify", false);
}
}

Configuration &getConfig() {
  static Configuration config(modInfo);
  return config;
}

bool GetMultipassRenderingEnabled() {
  // Hard-disabled regardless of gMultipassRenderingEnabled / the stored config
  // value. When enabled, MultipassKeywordController::OnPreRender() (see
  // VivifyComponents.cpp) calls Shader::SetGlobalInt("_StereoActiveEye", eye)
  // every frame a level is active, using Camera::get_stereoActiveEye(). That
  // value is only meaningful under legacy multi-pass XR rendering; Quest
  // renders single-pass-instanced, so this writes a near-arbitrary value into
  // a GLOBAL shader property that isn't scoped to Vivify's own materials --
  // any other shader reading it (note materials, UI/menu shaders, saber and
  // arc effects) gets corrupted for as long as a level is active, including
  // while paused. That's what was causing notes to go invisible after
  // entering a level and arcs/saber effects/menus to go invisible in some
  // levels. Confirmed by removing the initializer/EnsureConfigDefaults default
  // mismatch that let a stale or manually-enabled config value take effect;
  // the previous ref1 base avoided this entirely by hardcoding it off. Leave
  // this hardcoded false until MultipassKeywordController is actually fixed
  // for single-pass-instanced rendering and verified on-device.
  return false;
}

bool GetVivifyDebugLogging() {
  return gVivifyDebugLogging;
}

bool GetDisableBeat0FilmgrainBlit() {
  return gDisableBeat0FilmgrainBlit;
}

bool GetDisableAllBlits() {
  return gDisableAllBlits;
}

bool GetDisableCreateCameraDepth() {
  return gDisableCreateCameraDepth;
}

bool GetDisableCustomNoteVisuals() {
  return gDisableCustomNoteVisuals;
}

bool GetDisableVisualsInMultiplayer() {
  return gDisableVisualsInMultiplayer;
}

bool GetDisableVRCenterAdjust() {
  return gDisableVRCenterAdjust;
}

bool GetConvertPcBundlesOnDevice() {
  return gConvertPcBundlesOnDevice;
}

bool GetStandInShading() {
  return gStandInShading;
}

void EnsureConfigDefaults() {
  auto& config = getConfig();
  auto& doc = config.config;
  EnsureConfigObject();
  bool needsWrite = false;

  needsWrite |= EnsureBoolConfigValue(kMultipassRenderingConfigKey, false, gMultipassRenderingEnabled);

  needsWrite |= EnsureBoolConfigValue(kVivifyDebugLoggingConfigKey, false, gVivifyDebugLogging);
  needsWrite |= EnsureBoolConfigValue(kDisableBeat0FilmgrainBlitConfigKey, false, gDisableBeat0FilmgrainBlit);
  needsWrite |= EnsureBoolConfigValue(kDisableAllBlitsConfigKey, false, gDisableAllBlits);
  needsWrite |= EnsureBoolConfigValue(kDisableCreateCameraDepthConfigKey, false, gDisableCreateCameraDepth);
  needsWrite |= EnsureBoolConfigValue(kDisableCustomNoteVisualsConfigKey, false, gDisableCustomNoteVisuals);
  needsWrite |= EnsureBoolConfigValue(kDisableVisualsInMultiplayerConfigKey, true, gDisableVisualsInMultiplayer);
  needsWrite |= EnsureBoolConfigValue(kDisableVRCenterAdjustConfigKey, false, gDisableVRCenterAdjust);
  needsWrite |= EnsureBoolConfigValue(kConvertPcBundlesOnDeviceConfigKey, true, gConvertPcBundlesOnDevice);
  needsWrite |= EnsureBoolConfigValue(kStandInShadingConfigKey, true, gStandInShading);
  if (needsWrite) {
    config.Write();
  }
}

MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
  *info = modInfo.to_c();
  InstallVivifyFileLogSink();
  getConfig().Load();
  EnsureConfigDefaults();
  // Note: gMultipassRenderingEnabled / gVivifyDebugLogging are intentionally NOT
  // reset here. EnsureConfigDefaults() above already loaded the saved values (or
  // wrote the defaults on first run); hardcoding them back to a fixed value here
  // would silently discard the player's saved settings-menu choice on every launch.
  PaperLogger.info("Vivify file logging active -> {}", kVivifyLogPath);
}
MOD_EXTERN_FUNC void late_load() noexcept {
  il2cpp_functions::Init();
  custom_types::Register::AutoRegister();
  RegisterModSettings();
  Vivify::LateLoad();
}
