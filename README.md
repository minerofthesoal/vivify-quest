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
  - Fixed two settings (`Multipass Rendering`, `Debug logging`) being silently
    reset to a hardcoded value on every launch instead of respecting what was
    saved in the settings menu.
  - Full settings-menu parity: every toggle the runtime already had a config
    key for is now actually exposed in the in-game settings UI.

## 0.5.0 — visibility fixes and the on-device bundle converter

Four defects, all found by reading the code rather than by reproducing them
on-device. Each one is described in a comment at the site of the fix.

### Custom sabers did nothing

`ShouldDisableVisualsForMultiplayer()` gated saber and debris replacement on
`IsMultiplayerModLoaded()` — whether the **MultiplayerCore mod is installed**,
not whether you are actually in a multiplayer lobby. The
`DisableVisualsInMultiplayer` setting it guards defaults to on, and
MultiplayerCore is installed on most Quest setups, so custom sabers were being
suppressed in solo play for nearly everyone. Notes still worked, because
`ReplaceNoteVisuals` never consulted that check — which matches the symptom
exactly.

It now checks for a live `MultiplayerController`, which only exists in the
multiplayer gameplay scene (vanilla and MultiplayerCore lobbies alike). The
result is cached per beatmap.

### Notes going invisible partway through a song

Note-replacement prefabs were being forced onto Unity layer 4, in code whose
comment described layer 4 as the built-in "Ignore Raycast" layer. It isn't —
"Ignore Raycast" is layer **2**, and layer 4 is "Water", which Beat Saber's
gameplay cameras do not render. So a replaced note was moved somewhere nothing
draws it, while the note's own renderers had already been disabled: the note
vanished. It only starts once the map's `AssignObjectPrefab` has taken effect,
which is why it looks like it begins mid-song.

Note replacements now inherit the layer of the note they stand in for, so they
are culled and drawn by exactly the cameras that would have drawn the original.
The raycast concern behind the original override doesn't apply — raycasts hit
colliders, not renderers, and every collider on a spawned prefab is already
disabled.

### Arcs (and other transparent geometry) disappearing on some maps

Each mid-render `CameraEvent` command buffer ended with a `Blit` to
`BuiltinRenderTextureType.CameraTarget`. `CommandBuffer.Blit` binds its
destination as a **colour-only** render target, so after that buffer ran, the
camera was left with no depth attachment for the rest of the frame. Everything
still to be drawn — for `BeforeForwardAlpha` and earlier, that is all the
transparent geometry: arcs, saber trails, note debris — had no depth buffer to
test or write against.

That is why arcs vanish only on maps that use a mid-render `Blit`, and it is a
strong candidate for the mid-song "notes and effects stop drawing" reports too.
Each command buffer now ends with `SetRenderTarget(CameraTarget)`, whose
single-identifier overload re-binds the target as both colour *and* depth with
`LoadAction.Load`, so the camera's depth contents survive.

### PC ("Windows") bundles loading with no assets — and the on-device converter

The old "Allow Unsafe Windows Bundle Fallback" toggle handed a PC-built
AssetBundle straight to `AssetBundle.LoadFromFile`. Unity does not reject that
outright on Android: it hands back a bundle object whose `GetAllAssetNames()`
is empty. That is exactly the reported "the experimental Windows bundle doesn't
have any assets" behaviour — the fallback could never have worked.

That toggle is replaced by **Convert PC Bundles On Device** (on by default).
`src/VivifyBundleConvert.cpp` unpacks the UnityFS archive, rewrites the
`m_TargetPlatform` field in every `SerializedFile` header inside it to Android,
and repacks it uncompressed. Unity then accepts and enumerates the bundle.

- Handles uncompressed, LZ4/LZ4HC and LZMA archives, `UnityFS` versions 6/7,
  blocks-info stored at either the front or the back of the file, and
  `SerializedFile` header versions from 8 through 22+.
- Runs on a worker thread; the play button reads "Converting PC assets..."
  while it works.
- Output is cached under
  `/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/ConvertedBundles/`,
  keyed by the source path, size and mtime, so each bundle is converted once.
  Song folders are never modified. Writes go to a `.part` file and are renamed
  into place, so an interrupted conversion can't leave a truncated file that a
  later run mistakes for a finished one.
- Bundle resolution order is now: the map's own Android bundle → download the
  real Android build by its `android2021` checksum → convert a PC bundle. A
  downloaded Android build is always preferred, and a failed download now falls
  back to conversion instead of just giving up.

**What conversion can and cannot rescue.** Meshes, prefabs, GameObject
hierarchies, animations, animator controllers, audio, text assets and material
*definitions* are stored platform-independently and come through intact.
Shaders and block-compressed textures do not: a Windows bundle carries DirectX
shader bytecode and BC/DXT texture data, neither of which an Adreno GPU can
consume. Converted bundles therefore fall back to Vivify's replacement-shader
path rather than the mapper's intended shading. It is a rescue path for maps
that have no Android bundle yet — not a substitute for one.

## What's unverified

None of this has been compiled or run on-device — see "Building" below for why.

The bundle converter is the exception: it has no Unity or il2cpp dependencies,
and it is covered by a host-side test suite (see "Testing the converter") that
round-trips synthesised UnityFS archives across every compression mode, header
version and layout variant it claims to support, plus a corruption fuzz pass —
all under AddressSanitizer and UndefinedBehaviorSanitizer.

The three rendering/visibility fixes are code-level defects with clear
mechanisms, but they have **not** been confirmed by reproducing the bugs.
Please test in-game, especially on maps that previously showed them.

## Building

I don't have an Android NDK toolchain or access to the QPM package registry
(`qpackages.com`) in the environment this was built in, so **the mod itself has
not been compiled**. To build it yourself:

1. Install [QPM](https://github.com/QuestPackageManager/QPM.CLI) and the Beat
   Saber Quest modding toolchain (Android NDK, CMake/Ninja) — see the
   [BSMG modding docs](https://bsmg.wiki/quest/quest-modding-intro.html) if
   you don't already have this set up.
2. From this project's root: `qpm restore`
3. `qpm s build` (or your usual `pwsh scripts/build.ps1`)
4. Package with `scripts/createqmod.ps1`, or `qpm s qmod`.
5. Test on-device (`adb`/QuestPatcher install).

The GitHub Actions workflow in `.github/workflows/build.yml` does all of this
on push to `main` and on pull requests.

## Testing the converter

`src/VivifyBundleConvert.cpp` and `include/VivifyBundleConvert.hpp` are plain
C++20 with no Unity dependency, so they build and run on a desktop:

```sh
g++ -std=c++20 -O1 -g -fsanitize=address,undefined \
    -I include -o /tmp/conv tools/bundleconvert/main.cpp src/VivifyBundleConvert.cpp
python3 tools/bundleconvert/run_tests.py   # 32 round-trip / structure cases
python3 tools/bundleconvert/fuzz.py        # 600 mutated archives, expects no crashes
```

`tools/bundleconvert/mkbundle.py` synthesises the test archives (including a
from-scratch LZ4 block compressor, and LZMA1 streams produced by Python's own
`lzma` module, so the decoders are validated against real encoders).

## Credits

- [Aeroluna](https://github.com/Aeroluna) — original PC/PCVR Vivify, and the
  reference this build's render-ordering was checked against.
- axo-lotl ([Gay-Axolotl](https://github.com/Gay-Axolotl) on GitHub) —
  primary base of this build.
- Braxed ([rbatteries1-design](https://github.com/rbatteries1-design)) —
  Vivify-Quest-Port, source of the merged fixes above.
