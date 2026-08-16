#include "VivifyRuntimeInternal.hpp"
#include "VivifyComponents.hpp"
#include "GlobalNamespace/UserInfo.hpp"

using namespace Vivify;
using namespace std::string_view_literals;

namespace {

MAKE_HOOK_MATCH(SaberModelController_Init, &GlobalNamespace::SaberModelController::Init, void, GlobalNamespace::SaberModelController* self, UnityEngine::Transform* parent, GlobalNamespace::Saber* saber, UnityEngine::Color trailTintColor) {
  SaberModelController_Init(self, parent, saber, trailTintColor);
  Runtime::Instance().TrackSaberModel(self, saber, parent);
}

MAKE_HOOK_MATCH(GameNoteController_Init, &GlobalNamespace::GameNoteController::Init, void, GlobalNamespace::GameNoteController* self, GlobalNamespace::NoteData* noteData, ByRef<GlobalNamespace::NoteSpawnData> noteSpawnData, GlobalNamespace::NoteVisualModifierType noteVisualModifierType, float cutAngleTolerance, float uniformScale) {
  GameNoteController_Init(self, noteData, noteSpawnData, noteVisualModifierType, cutAngleTolerance, uniformScale);
  Runtime::Instance().ApplyNotePrefabFor(self);
}

MAKE_HOOK_MATCH(BombNoteController_Init, &GlobalNamespace::BombNoteController::Init, void, GlobalNamespace::BombNoteController* self, GlobalNamespace::NoteData* noteData, ByRef<GlobalNamespace::NoteSpawnData> noteSpawnData) {
  BombNoteController_Init(self, noteData, noteSpawnData);
  Runtime::Instance().ApplyNotePrefabFor(self);
}

MAKE_HOOK_MATCH(BurstSliderGameNoteController_Init, &GlobalNamespace::BurstSliderGameNoteController::Init, void, GlobalNamespace::BurstSliderGameNoteController* self, GlobalNamespace::NoteData* noteData, ByRef<GlobalNamespace::NoteSpawnData> noteSpawnData, GlobalNamespace::NoteVisualModifierType noteVisualModifierType, float uniformScale) {
  BurstSliderGameNoteController_Init(self, noteData, noteSpawnData, noteVisualModifierType, uniformScale);
  Runtime::Instance().ApplyNotePrefabFor(self);
}

MAKE_HOOK_MATCH(NoteCutCoreEffectsSpawner_SpawnNoteCutEffect, &GlobalNamespace::NoteCutCoreEffectsSpawner::SpawnNoteCutEffect, void, GlobalNamespace::NoteCutCoreEffectsSpawner* self, ByRef<GlobalNamespace::NoteCutInfo> noteCutInfo, GlobalNamespace::NoteController* noteController, int32_t sparkleParticlesCount, int32_t explosionParticlesCount) {

  bool const customDebris = Runtime::Instance().GetCurrentBeatmapData() != nullptr &&
                            !Runtime::Instance().IsResetting() && !Runtime::Instance().IsReduceDebrisEnabled();
  if (customDebris) {
    Runtime::Instance().PushActiveDebrisPrefabs(Runtime::Instance().FindAssignedDebrisPrefabs(noteCutInfo->noteData));
  }
  NoteCutCoreEffectsSpawner_SpawnNoteCutEffect(self, noteCutInfo, noteController, sparkleParticlesCount, explosionParticlesCount);
  if (customDebris) {
    Runtime::Instance().PopActiveDebrisPrefabs();
  }
}

MAKE_HOOK_MATCH(NoteDebris_Init, &GlobalNamespace::NoteDebris::Init, void, GlobalNamespace::NoteDebris* self, GlobalNamespace::ColorType colorType, UnityEngine::Vector3 notePos, UnityEngine::Quaternion noteRot, UnityEngine::Vector3 noteMoveVec, UnityEngine::Vector3 noteScale, UnityEngine::Vector3 positionOffset, UnityEngine::Quaternion rotationOffset, UnityEngine::Vector3 cutPoint, UnityEngine::Vector3 cutNormal, UnityEngine::Vector3 force, UnityEngine::Vector3 torque, float lifeTime, UnityEngine::Vector3 cutoutOffset, bool forceOnlySimplePhysics) {
  NoteDebris_Init(self, colorType, notePos, noteRot, noteMoveVec, noteScale, positionOffset, rotationOffset, cutPoint, cutNormal, force, torque, lifeTime, cutoutOffset, forceOnlySimplePhysics);
  Runtime::Instance().RestoreDebrisVisuals(self);
  if (Runtime::Instance().GetCurrentBeatmapData() == nullptr || Runtime::Instance().IsResetting()) return;
  Runtime::Instance().ReplaceDebrisVisuals(self);
}

MAKE_HOOK_MATCH(AlwaysVisibleQuad_OnEnable, &GlobalNamespace::AlwaysVisibleQuad::OnEnable, void, GlobalNamespace::AlwaysVisibleQuad* self) {
  AlwaysVisibleQuad_OnEnable(self);
  if (Runtime::Instance().GetCurrentBeatmapData() == nullptr || Runtime::Instance().IsResetting()) return;
  if (!IsManagedAlive(self)) return;
  auto transform = self->get_transform();
  if (!IsManagedAlive(transform.unsafePtr())) return;
  transform->set_position(UnityEngine::Vector3(0.0f, -1000.0f, 0.0f));
}

MAKE_HOOK_MATCH(ImageEffectController_OnRenderImage, &GlobalNamespace::ImageEffectController::OnRenderImage, void, GlobalNamespace::ImageEffectController* self, UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest) {
  if (Runtime::Instance().ShouldBypassImageEffect(self)) {
    UnityEngine::Graphics::Blit(static_cast<UnityEngine::Texture*>(src), dest);
    return;
  }
  ImageEffectController_OnRenderImage(self, src, dest);
}

MAKE_HOOK_MATCH(PauseController_Pause, &GlobalNamespace::PauseController::Pause, void, GlobalNamespace::PauseController* self) {
  Runtime::Instance().SetPauseMenuActive(true);
  PauseController_Pause(self);
}

MAKE_HOOK_MATCH(PauseMenuManager_ShowMenu, &GlobalNamespace::PauseMenuManager::ShowMenu, void, GlobalNamespace::PauseMenuManager* self) {
  Runtime::Instance().SetPauseMenuActive(true);
  PauseMenuManager_ShowMenu(self);
}

MAKE_HOOK_MATCH(PauseMenuManager_MenuButtonPressed, &GlobalNamespace::PauseMenuManager::MenuButtonPressed, void, GlobalNamespace::PauseMenuManager* self) {
  Runtime::Instance().SetPauseMenuActive(true);
  PauseMenuManager_MenuButtonPressed(self);
}

MAKE_HOOK_MATCH(PauseController_HandlePauseMenuManagerDidPressMenuButton, &GlobalNamespace::PauseController::HandlePauseMenuManagerDidPressMenuButton, void, GlobalNamespace::PauseController* self) {
  Runtime::Instance().SetPauseMenuActive(true);
  PauseController_HandlePauseMenuManagerDidPressMenuButton(self);
}

MAKE_HOOK_MATCH(PauseController_HandlePauseMenuManagerDidFinishResumeAnimation, &GlobalNamespace::PauseController::HandlePauseMenuManagerDidFinishResumeAnimation, void, GlobalNamespace::PauseController* self) {
  PauseController_HandlePauseMenuManagerDidFinishResumeAnimation(self);
  Runtime::Instance().SetPauseMenuActive(false);
}

MAKE_HOOK_MATCH(GamePause_Resume, &GlobalNamespace::GamePause::Resume, void, GlobalNamespace::GamePause* self) {
  GamePause_Resume(self);
  Runtime::Instance().SetPauseMenuActive(false);
}

// Ported from the rbatteries1-design/Lars27110 base. VRCenterAdjust drives the
// automatic room-scale recenter/floor-offset. Its methods touch
// _settingsManager / _settingsApplicator fields that aren't always populated
// this early in scene setup, and Vivify's own world-space cameras/effects are
// positioned relative to room space -- letting a recenter run underneath them
// (or running these methods against a not-yet-initialized instance) can shift
// world-space visuals out of alignment with gameplay. bailing out here (and
// via the "Disable VR Center Adjust Handling" settings toggle) keeps that from
// happening; skipped calls just fall through to Beat Saber's stock behavior on
// the next legitimate call once the fields are populated.
bool ShouldSkipVRCenterAdjust(GlobalNamespace::VRCenterAdjust* self, std::string_view method) {
  if (!IsManagedAlive(self)) return true;
  bool const missingSettingsManager = self->____settingsManager == nullptr;
  bool const missingSettingsApplicator = !IsManagedAlive(self->____settingsApplicator.unsafePtr());
  if (GetDisableVRCenterAdjust()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("VRCenterAdjust.{} skipped: isolation toggle enabled", method);
    }
    return true;
  }
  if (missingSettingsManager || missingSettingsApplicator) {
    if (method != "Update"sv || GetVivifyDebugLogging()) {
      PaperLogger.warn("VRCenterAdjust.{} skipped: missing _settingsManager={} _settingsApplicator={}",
                       method, BoolText(missingSettingsManager), BoolText(missingSettingsApplicator));
    }
    return true;
  }
  return false;
}

MAKE_HOOK_MATCH(VRCenterAdjust_Start, &GlobalNamespace::VRCenterAdjust::Start, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "Start")) return;
  VRCenterAdjust_Start(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_OnEnable, &GlobalNamespace::VRCenterAdjust::OnEnable, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "OnEnable")) return;
  VRCenterAdjust_OnEnable(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_OnDisable, &GlobalNamespace::VRCenterAdjust::OnDisable, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "OnDisable")) return;
  VRCenterAdjust_OnDisable(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_Update, &GlobalNamespace::VRCenterAdjust::Update, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "Update")) return;
  VRCenterAdjust_Update(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_ResetRoom, &GlobalNamespace::VRCenterAdjust::ResetRoom, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "ResetRoom")) return;
  VRCenterAdjust_ResetRoom(self);
}

MAKE_HOOK_MATCH(VRCenterAdjust_SetRoomTransformOffset, &GlobalNamespace::VRCenterAdjust::SetRoomTransformOffset, void, GlobalNamespace::VRCenterAdjust* self) {
  if (ShouldSkipVRCenterAdjust(self, "SetRoomTransformOffset")) return;
  VRCenterAdjust_SetRoomTransformOffset(self);
}

}

namespace Vivify {

void LateLoad() {
  Runtime::Instance().LateLoad();
  INSTALL_HOOK(PaperLogger, SaberModelController_Init);
  INSTALL_HOOK(PaperLogger, GameNoteController_Init);
  INSTALL_HOOK(PaperLogger, BombNoteController_Init);
  INSTALL_HOOK(PaperLogger, BurstSliderGameNoteController_Init);
  INSTALL_HOOK(PaperLogger, NoteCutCoreEffectsSpawner_SpawnNoteCutEffect);
  INSTALL_HOOK(PaperLogger, NoteDebris_Init);
  INSTALL_HOOK(PaperLogger, AlwaysVisibleQuad_OnEnable);
  INSTALL_HOOK(PaperLogger, ImageEffectController_OnRenderImage);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_Start);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_OnEnable);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_OnDisable);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_Update);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_ResetRoom);
  INSTALL_HOOK(PaperLogger, VRCenterAdjust_SetRoomTransformOffset);
  INSTALL_HOOK(PaperLogger, PauseController_Pause);
  INSTALL_HOOK(PaperLogger, PauseMenuManager_ShowMenu);
  INSTALL_HOOK(PaperLogger, PauseMenuManager_MenuButtonPressed);
  INSTALL_HOOK(PaperLogger, PauseController_HandlePauseMenuManagerDidPressMenuButton);
  INSTALL_HOOK(PaperLogger, PauseController_HandlePauseMenuManagerDidFinishResumeAnimation);
  INSTALL_HOOK(PaperLogger, GamePause_Resume);
}

void RefreshMultipassRendering() {
  Runtime::Instance().RefreshMultipassRendering();
}

void RefreshIsolationSettings() {
  Runtime::Instance().RefreshIsolationSettings();
}

}
