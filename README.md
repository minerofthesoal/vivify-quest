# Vivify Quest 

A personal Quest port of [Vivify](https://github.com/Aeroluna/Vivify) for Beat Saber,
built by merging three existing community ports. This is **not** an original
port from scratch — see Credits below.

## What this is built from

- **Base architecture:** [axo-lotl/Vivifhy-Quest](https://github.com/Gay-Axolotl/Vivifhy-Quest)
  (a.k.a. "Gay-Axolotl" on GitHub). Chosen as the base because it implements
  real per-`CameraEvent` command buffers (`BeforeSkybox`/`AfterSkybox`/
  `BeforeForwardOpaque`/`AfterForwardOpaque`/`BeforeForwardAlpha`/`AfterForwardAlpha`),
  matching PC Vivify's `PostProcessingController.cs` render ordering. The other
  two ports only support a single post-hoc `OnRenderImage` blit that always
  fires after the entire frame (including notes/blocks) is rendered, so any
  Blit effect on those ports always draws over gameplay regardless of what
  order the mapper specified. That's the "blocks rendering on top of
  everything" behavior this build avoids.
- **Merged in from [rbatteries1-design/Vivify-Quest-Port](https://github.com/rbatteries1-design/Vivify-Quest-Port)**
  (Braxed's build on top of an earlier axo-lotl version) **and
  [Lars27110/Vivify-Quest](https://github.com/Lars27110/Vivify-Quest)** (a
  near-identical fork of the same codebase):
  - Legacy custom-event aliases (`PostProcess`, `PostProcessing`, `ScreenEffect`)
    for older maps authored before the community settled on `Blit`.
  - The `VRCenterAdjust` hook suite, which stops the automatic room-scale
    recenter from desyncing Vivify's world-space cameras/effects, plus its
    settings toggle.
  - `DisableCustomNoteVisuals` and `DisableVisualsInMultiplayer` settings,
    wired into note/saber/debris visual replacement.
  - An optional (off-by-default) fallback to load a Windows-built AssetBundle
    on Quest when no Android bundle exists yet, gated behind an explicit
    "unsafe" settings toggle since it's an unsupported platform mismatch.
  - **The fix for arcs / saber-clash / burn-mark effects breaking on some
    levels:** note-visual replacement prefabs are cosmetic and now get forced
    onto Unity's "Ignore Raycast" layer (matching the base). Previously the
    replacement mesh inherited whatever layer the AssetBundle happened to
    export, which put it on a layer that saber-collision/clash-detection
    raycasts scan — colliding with those raycasts is what was making arcs and
    saber effects intermittently stop registering on maps that also used
    Vivify note replacement.
  - A defensive fix alongside it: `MaterialPropertyBlockController::ApplyChanges()`
    calls are now wrapped in `try`/`catch`, since that native call has been
    observed to throw on-device; previously an uncaught exception there could
    unwind into whatever native call triggered it and disrupt other systems
    processing in the same frame.
  - Fixed two settings (`Multipass Rendering`, `Debug logging`) being silently
    reset to a hardcoded value on every launch instead of respecting what was
    saved in the settings menu.
  - Full settings-menu parity: every toggle the runtime already had a config
    key for is now actually exposed in the in-game settings UI.

## What's unverified

None of this has been compiled or run on-device — see "Building" below for
why. The arc/saber-effects fix in particular is a strong, well-evidenced
hypothesis backed by a genuine code-level gap (base has the layer-assignment
call, this port didn't), not something confirmed by reproducing the bug.
**Please test it in-game before relying on it**, especially on maps you know
previously had the issue.

## Building

This was assembled by reading and editing source only. I don't have an
Android NDK toolchain or access to the QPM package registry (`qpackages.com`)
in the environment this was built in, so **none of this has been compiled**.
To build it yourself:

1. Install [QPM](https://github.com/QuestPackageManager/QPM.CLI) and the Beat
   Saber Quest modding toolchain (Android NDK, CMake/Ninja) — see the
   [BSMG modding docs](https://bsmg.wiki/quest/quest-modding-intro.html) if
   you don't already have this set up.
2. From this project's root: `qpm restore`
3. `qpm s build` (or your usual `pwsh scripts/build.ps1`)
4. Package with `scripts/createqmod.ps1`, or `qpm s qmod`.
5. Test on-device (`adb`/QuestPatcher install) before trusting it, especially
   on maps that previously showed the arc/saber-effects bug or blocks
   rendering over the top of Vivify effects.

Before publishing anywhere, update the placeholder `url` field in `qpm.json`
to point at your own repo.

## Credits

- [Aeroluna](https://github.com/Aeroluna) — original PC/PCVR Vivify, and the
  reference this build's render-ordering was checked against.
- axo-lotl ([Gay-Axolotl](https://github.com/Gay-Axolotl) on GitHub) —
  primary base of this build.
- Braxed ([rbatteries1-design](https://github.com/rbatteries1-design)) —
  Vivify-Quest-Port, source of the merged fixes above.

although made by claude it is so far iv gotten the most performance from this port
