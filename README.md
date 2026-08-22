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

### PC ("Windows") bundle maps being unplayable, and the on-device converter

Two separate defects stacked here.

**Discovery is now by content.** Bundle discovery previously matched only files
with a `.vivify` extension.

*Correction: an earlier version of this section claimed PC bundles ship without
that extension and so were never found at all. That was wrong.* Upstream Vivify
names them `bundle{Windows2019,Windows2021}.vivify`
([`VivifyController.cs`](https://github.com/Aeroluna/Vivify/blob/master/Vivify/VivifyController.cs)),
so the old scan did find them. Discovery is now by **content** anyway — any file
in the song folder starting with the `UnityFS` signature qualifies, names only
rank candidates — because that is robust to a mapper renaming a bundle, and it
also picks up an *Android* bundle that is not named exactly
`bundleAndroid2021.vivify`. It was not the reason PC maps would not start.

**The fallback could not have worked.** The old "Allow Unsafe Windows
Bundle Fallback" toggle handed the PC bundle straight to
`AssetBundle.LoadFromFile`. Unity does not reject that outright on Android: it
hands back a bundle object whose `GetAllAssetNames()` is empty — exactly the
reported "the experimental Windows bundle doesn't have any assets" behaviour.

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
  keyed by the song folder name, bundle file name, size and mtime — pointedly
  *not* by absolute path. The bulk pass walks SongCore's level roots while
  level selection uses `customLevelPath`, and on Android the same directory is
  reachable as `/sdcard/…`, `/storage/emulated/0/…` and
  `/storage/self/primary/…`; keying on the path let those two routes hash the
  same file differently, so a bulk-converted bundle was not found again at
  level selection and the map stayed unplayable as if nothing had converted.
  Cached files are named after the song folder so the directory can be
  eyeballed against the song list.
  Song folders are never modified. Writes go to a `.part` file and are renamed
  into place, so an interrupted conversion can't leave a truncated file that a
  later run mistakes for a finished one.
- Bundle resolution order is: the map's own Android bundle → a bundle already
  converted on this device → download the real Android build by its
  `android2021` checksum → convert the PC bundle. An already-converted bundle
  deliberately outranks the download: a map that ships a PC bundle usually has
  no Android build in the repo to fetch (that is *why* it only ships a PC
  bundle), so checking the network first meant a converted map could sit on
  "Downloading assets..." with a perfectly good converted bundle unused in the
  cache.
- Downloads now time out after 45s and fall back to conversion. WebUtils does
  not promise a callback on every failure mode, so a request that never
  resolved previously left the play button disabled for the rest of the
  session.
- Vivify logs which mods are blocking the play button whenever that changes.
  SongCore aggregates the decision across every installed mod, so a map held
  by an unrelated requirement looks identical to one Vivify is holding; the
  log now names the mod and its reason.
- Every path that leaves the play button disabled now names its own reason
  ("Convert failed: unsupported bundle compression", "Asset download timed
  out", "PC bundle found; enable Convert PC Bundles On Device in settings",
  …), and level selection always logs one line to `Vivify.log` recording the
  Android bundle, PC bundle, checksum, cache path and decision taken.

**Convert All PC Bundles Now.** Per-level conversion runs on level *selection*,
not on play, so it does not need a playable map — but it does need you to be
able to reach the level. The Vivify settings menu therefore also has a **Convert
All PC Bundles Now** button that walks every installed custom level (including
WIP levels), converts everything convertible in one background pass, and reports
progress under the button. Nothing needs to be selected or playable for it to
run, and already-converted maps are skipped.

### Converted bundles rendering nothing — no models, invisible notes and sabers

A converted bundle loaded and its assets enumerated, but nothing it contained
drew: no models, invisible blocks, invisible sabers. Two independent bugs in the
shader-repair path, either of which alone is enough to cause it.

**Broken shaders were classified as fine.** `RepairMaterialShader` left a
material alone when its shader reported `isSupported == false` but
`Material.passCount > 0`. `passCount` counts the passes *declared* in the
shader's subshaders, which a DirectX-only shader still has on Android even
though it carries no GLES program — so every unusable shader took that early
return, was recorded as repaired, and kept a shader that draws nothing. The
check now requires `isSupported`.

**The fallback shader never existed.** When a material did get as far as being
repaired, the replacement came from `Shader.Find` over `"Unlit/Texture"`,
`"Unlit/Color"`, `"Sprites/Default"` and `"Standard"`. `Shader.Find` only
resolves shaders included in the build, and Unity strips built-in shaders
nothing references, so in Beat Saber's IL2CPP build all four return null and the
repair silently gave up. Vivify now enumerates the shaders the process has
actually loaded (`Resources.FindObjectsOfTypeAll<Shader>`), scores them, and
caches the best stand-in; the chosen shader is logged.

**Belt and braces.** A replacement prefab now only hides the note, saber or
debris it stands in for once at least one of its spawned renderers has a
material whose shader this GPU can run. If a bundle's shading cannot be
rescued, you get the default block or saber rather than nothing at all — this
class of bug can no longer make gameplay objects disappear.

Asset loading is also wrapped per-asset: a converted PC bundle carries DirectX
shader programs and BC/DXT texture data this GPU cannot consume, and one asset
throwing as it is realised no longer takes the rest of the bundle with it.

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

I don't have an Android NDK toolchain in the environment this was built in, so **the mod itself has
not been compiled**. To build it yourself:

1. Install [QPM.CLI](https://github.com/QuestPackageManager/QPM.CLI) and the Beat
   Saber Quest modding toolchain (Android NDK, CMake/Ninja) — see the
   [BSMG modding docs](https://bsmg.wiki/quest/quest-modding-intro.html) if
   you don't already have this set up.
2. From this project's root: `qpm restore`
3. `qpm s build` (or your usual `pwsh scripts/build.ps1`)
4. Package with `scripts/createqmod.ps1`, or `qpm s qmod`.
5. Test on-device (`adb`/QuestPatcher install).

For packages unavailable through QPM, download the dependency from its GitHub
repository and place it in your project's `extern` directory, then update your
`qpm.json`/build files manually.

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
