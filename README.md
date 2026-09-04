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

## 0.9.13 — why every converted level went black

0.7 rendered converted maps. Everything from 0.8.0 onwards rendered them black,
with only the particles still visible. The whole functional difference between
those two builds is one feature: the on-device BC/DXT texture decode added in
0.8.0. Nothing else changed — 0.8.1 through 0.8.3 are a diagnostic log line and
two build fixes.

Here is how a decoder makes a level black.

A Quest's Adreno GPU cannot sample the BC1/BC3/BC7 textures a PC-built
AssetBundle carries, so 0.8.0 decoded them to RGBA32 on the CPU using the
texture's own bytes, fetched with `GetRawTextureData`. But a texture loaded from
an AssetBundle only still *has* its bytes if the map author ticked Read/Write
Enabled in Unity, which almost nobody does: the pixels go to the GPU and the CPU
copy is dropped. Ask that texture for its data anyway and you do not necessarily
get an error. You can get an array of exactly the right length with nothing in
it.

That array decodes. It decodes *correctly*: an all-zero BC1 block is a valid
block and it means opaque black. So the pass did precisely what it was written
to do, on data that was not there, and handed every material in the map a black
texture in place of one that had merely been unsampleable. An unsampleable
texture reads as flat white, which is why 0.7's defects were "some levels are
white, and the blocks are white" — and why 0.8.0 turned those same levels black.
The particles survived because their materials are untextured, so there was
nothing for the decoder to replace.

The decoder was never wrong; its test suite passes and still does. What was
missing was any check that the bytes it was handed were real. There are three
now, and every one of them leaves the texture exactly as it was:

- the texture must report `isReadable`, or there is no CPU copy to decode;
- the raw bytes must not be uniformly zero, which is the signature of a copy
  that has already been dropped;
- the decoded result must have something visible in it — some colour and some
  opacity.

A texture that fails any of them draws untextured, the way it did in 0.7 and the
way it did before this pass existed. `tools/texturedecode/` gained the cases
that pin this down, including the one that matters: an all-zero BC1 block
decoding without complaint into a texture that is recognised as blank.

The session log now says how many textures each level refused and why, and the
same two counts are in the report file. That is the number to read next: a map
whose textures are all "not readable on CPU" is one whose textures have to be
made available at conversion time instead, which is the next piece of work
rather than something the device can fix on its own.

Everything else in this build stays as it was. The stand-in shader ordering, the
shader index that resolves a map's shaders to the real ones, the DXBC translator
and the conversion cache versioning are all unchanged — the only thing 0.7 did
better was not turning the textures black, and that is what this restores.

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
  …), and level selection always logs one line to `VivifySession.txt` recording the
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

**Why converted assets came out white.** The stand-in was carrying the wrong
colour across. `ReadMaterialFallbackColor` returned the first colour property
that *existed* on the material — and that is nearly always `_Color`, a property
almost every shader declares and almost every material leaves at its default of
white. So it dutifully copied white while the material's real colour sat in
`_BaseColor`, `_EmissionColor` or one of the map's own named properties.

Material property *values* live in the SerializedFile and are entirely
platform-independent, so those colours survive conversion perfectly — they were
only ever being looked up wrong. Every candidate is now collected and the first
*informative* one wins (skipping near-white and fully transparent), falling back
to whatever was found if they are all blank.

Texture recovery had the same shape of bug: it used `Material.mainTexture`,
which only ever resolves `_MainTex`, so any shader naming its albedo `_BaseMap`
or `_Albedo` came through untextured. It now scans the material's texture
properties, skipping normal/mask/metallic-style maps that would look wrong as an
albedo, and writes the result to whichever of `_MainTex`/`_BaseMap` the stand-in
declares.

**Textures are decoded on the CPU.** Quest's Adreno GPUs support ETC2 and ASTC
but not S3TC/BC, and a PC-built AssetBundle stores its textures as BC1/BC3/BC7.
Unity hands back the `Texture2D` quite happily; nothing can sample it. Vivify
now decodes those blocks to RGBA32 on load, using the vendored
[`bcdec.h`](include/third_party/) (single header, no includes of its own,
MIT/public-domain), and swaps the decoded copy into every material that
referenced the original. It costs memory — BC1 is 4 bits per pixel, RGBA32 is
32 — but it is the difference between an untextured mesh and a textured one.

This needs the source texture's raw bytes to still be around. A texture
imported without read/write enabled may have had its CPU copy dropped after
upload, and there is then nothing to decode; that case is logged per texture
and the original is left alone rather than guessed at.

`src/VivifyTextureDecode.cpp` has no Unity dependency and is covered by a host
test suite (`tools/texturedecode/`) that decodes hand-built BC1/BC3 blocks and
checks the resulting pixels, including non-multiple-of-four sizes, full mip
chains and every refusal path — run in CI under ASan/UBSan.

**Stand-ins can only carry so much.** A stand-in shader is a trade: "invisible"
becomes "visible but wrong", and for a converted PC bundle "wrong" is often flat
white, because the original material's look lives in a shader that cannot run.
Two things limit the damage. The stand-in is chosen preferring shaders that
expose `_MainTex` and a colour, so whatever the original material carried is
transferred. And when neither a colour nor a texture can be recovered, the
substitution is declined outright — an arbitrary flat white shape over the
scene is worse than the object not drawing, and for notes and sabers declining
means the game's own visuals stay. The **Stand-In Shading For Unsupported
Shaders** setting (on by default) turns the whole mechanism off if you would
rather have converted maps render nothing than render white.

**Belt and braces.** A replacement prefab now only hides the note, saber or
debris it stands in for once at least one of its spawned renderers has a
material whose shader this GPU can run. If a bundle's shading cannot be
rescued, you get the default block or saber rather than nothing at all — this
class of bug can no longer make gameplay objects disappear.

Asset loading is also wrapped per-asset: a converted PC bundle carries DirectX
shader programs and BC/DXT texture data this GPU cannot consume, and one asset
throwing as it is realised no longer takes the rest of the bundle with it.

### Raymarching / depth-driven effects doing nothing

Effects that need scene depth — raymarchers above all — produced nothing. Three
bugs in the `CreateCamera` depth path, checked against upstream's
[`SecondaryCameraController.cs`](https://github.com/Aeroluna/Vivify/blob/master/Vivify/PostProcessing/SecondaryCameraController.cs)
and [`PostProcessingController.cs`](https://github.com/Aeroluna/Vivify/blob/master/Vivify/PostProcessing/PostProcessingController.cs).

**Wrong texture format.** The port wrote depth into a
`RenderTextureFormat.Depth` render texture. Upstream writes
`RenderTextureFormat.RFloat` — its `DepthBlit` material samples
`_CameraDepthTexture` and stores it as a plain single-channel float. A map's
shader samples the texture it named in `CreateCamera` expecting exactly that;
a depth-format texture reads back as something else entirely. Depth is now
captured as `RFloat`, copied from `BuiltinRenderTextureType.Depth` (upstream's
`DepthBlit` shader ships as a Windows-built AssetBundle and cannot be reused
here, but the built-in identifier names the same source and the default blit
copies it without needing a shader).

**Depth-only cameras were never wired up.** A raymarching map creates a camera
that declares *only* a `depthTexture` — it wants scene depth, not a colour
copy. That path never attached a `SecondaryCameraController`, so nothing ever
captured anything; and `BindSecondaryCameraTextures` bailed out on
`!texturePropertyId.has_value()`, so even a captured depth texture was never
bound to the name the map's shader samples. Both now handle colour and depth
independently, and the depth pass is forced on via `depthTextureMode` so
`_CameraDepthTexture` actually exists.

**`targetTexture` breaks stereo.** Secondary cameras were given a target
texture (or explicit target buffers). Upstream deliberately does not, with the
reason in a comment at the top of its controller: assigning a target texture
disables stereo on the camera. Capture now happens per-eye in `OnRenderImage`,
matching upstream.

### Geometry shaders

Not something this port can enable, and worth being clear about rather than
leaving as an open request.

Whether a geometry stage can run at all is decided by the graphics API the game
was built against, which is baked into the APK — there is no runtime switch a
mod can flip. Adreno exposes `GL_EXT_geometry_shader` under OpenGL ES 3.2, but
reports `VkPhysicalDeviceFeatures.geometryShader` as false under Vulkan;
Qualcomm has never supported geometry or tessellation stages in their Vulkan
driver. So under Vulkan a geometry shader cannot execute on this hardware no
matter what a bundle contains.

Vivify now logs the answer on the first level load, unconditionally:

```
Vivify graphics: api=Vulkan (21) shaderLevel=45 (SM4.5) geometryShaderStagePossible=false
```

(`SystemInfo.supportsGeometryShaders` cannot be used for this — Unity removed it,
and it is absent from the codegen headers this port builds against. The graphics
API plus shader level is the reliable substitute: a geometry stage needs SM4.0,
so below 40 rules it out outright, and at or above 40 it comes down to the API.)

Even where the API allows it, a *converted* PC bundle still cannot supply one:
its shaders are DirectX bytecode, and no geometry stage can be recovered from
that or recompiled on device. A map relying on geometry shaders needs a bundle
built for Android by the mapper, on a build of the game using an API that
supports them. Where neither holds, the shader reports itself unsupported and
the stand-in path takes over, so the level stays playable with wrong visuals
rather than failing outright.

**Converted PC bundles cannot raymarch.** Worth stating plainly: none of the
above rescues a screen effect from a converted Windows bundle, because the
shader is DirectX bytecode with no GLES program and cannot be recompiled on
device. Vivify now detects this case and *skips* the blit rather than running
the material's stand-in shader, which would smear an unrelated shader across
the whole frame. The effect is unavailable; the frame passes through untouched.
Depth-driven effects need a bundle actually built for Android.

**What conversion can and cannot rescue.** Meshes, prefabs, GameObject
hierarchies, animations, animator controllers, audio, text assets and material
*definitions* are stored platform-independently and come through intact.
Shaders and block-compressed textures do not: a Windows bundle carries DirectX
shader bytecode and BC/DXT texture data, neither of which an Adreno GPU can
consume. (Textures are now decoded on device; see the BC/DXT section. Shaders
are not -- see "Converting shaders PC -> Quest" below for why that is a project
rather than an impossibility.) Converted bundles therefore fall back to Vivify's replacement-shader
path rather than the mapper's intended shading. It is a rescue path for maps
that have no Android bundle yet — not a substitute for one.

## 0.8.8 — the mod would not load at all

`libVivify.so` failed to `dlopen` on device, so nothing in the mod ran. The
cause was a regression from the move off qpackages.com, in this repo, not in any
dependency.

`config-utils` is a **headers-only** dependency: qpm resolves it with
`headersOnly: true` and links no library for it. Its GitHub release does publish
a `.so` — `libconfig-utils_test.so`, the repo's *test* binary, which is also what
`overrideSoName` names in `qpm.shared.json`. `scripts/dependencies.json` copied
that `overrideSoName` across and gave the entry a `soLink`, so
`restore-deps.py` downloaded it into `extern/libs/`.

`extern.cmake` links **every** `.so` in `extern/libs`, so the mod picked up
`DT_NEEDED[libconfig-utils_test.so]`. It linked cleanly and built a perfectly
valid library that no Quest can load, because no Quest has that file. The
failure surfaces only as "libVivify.so failed", with no indication of which
library was missing.

Three guards now stand between that mistake and a shipped build:

- `restore-deps.py` refuses to place a library for a dependency marked
  `headersOnly`, and says so.
- After a restore it compares `extern/libs` against the manifest in both
  directions — a missing library is a link error, and a stray one is a load
  failure, so both fail loudly.
- `scripts/check-so-dependencies.py` reads the built `libVivify.so`'s actual
  `DT_NEEDED` list and checks every entry is an Android system library, provided
  by the modloader, or installed by a real qpm dependency. It runs in the build
  workflow between compiling and packaging, so a mod that cannot load never
  becomes a `.qmod`. Libraries that arrive transitively (`libtinyxml2.so` comes
  with BSML) are reported as notes rather than failures.

## 0.8.9 — freezes, and a watchdog so they cannot happen again

Every level froze on start and the game had to be force-quit. 0.8.8 was the
first build that ran on device since 0.5.1, so the cause could be anything in
that window; rather than guess, this release makes a freeze impossible and makes
the next report decisive.

**Vivify now stands down instead of hanging the game.** Per-frame work is timed.
Thirty consecutive frames over 50ms (a frame is 11-14ms at 72-90Hz) and Vivify
disables itself for the rest of the level, logging the worst frame time. The map
loses its Vivify visuals, which is bad, but the game keeps running and you do
not have to restart it. A new beatmap clears the flag.

**Level-load phases are timed unconditionally.** All of this runs on the main
thread while the level loads:

```
Vivify level load: cache bundle assets took 120ms
Vivify level load: decode textures took 3400ms
Vivify level load: repair shaders took 80ms
Vivify level load: 3600ms total
```

Whichever number is large is the cause. Please send these lines.

Two concrete hazards found while looking, both real regardless of whether they
caused this:

- **`FindFallbackShader` never cached a failed search.** It walks every shader
  object loaded in the process — thousands, in Beat Saber — calling
  `isSupported`, `name` and `FindPropertyIndex` on each. `RepairMaterialShader`
  calls it for every material it cannot fix, and prefab instances bring fresh
  materials each spawn, so one bundle with no usable stand-in meant a full
  shader-database scan per material per spawn. That is not a slow frame, it is a
  stopped game. The failure is now remembered.

- **Texture decoding was unbounded on the main thread.** A 2048x2048 BC7 texture
  is four million pixels and a bundle can hold dozens. There is now a 2-second
  budget per level; textures past it keep their original format and render
  untextured, and the count is logged. Already-decoded textures are cache hits
  and do not consume budget.

## Force reconvert

The settings menu has a second button, **Force Reconvert All (ignore cache)**.

A converted bundle is cached under a key derived from the *source* bundle's
identity, so once a map has been converted the cached file is reused forever —
including a conversion produced by an older or buggier converter. Short of
deleting the cache directory by hand there was no way to pick up converter
fixes. The forced pass removes each cached file before reconverting. Conversion
writes through a `.part` file and renames, so a failure mid-pass leaves no
cached bundle rather than a truncated one.

## 0.9.1 — both diagnostic files are .txt, in one folder

The full session log was at `Logs/Vivify.log`. A `.log` file has no default
handler on Android or Windows, so tapping it does nothing and it looks like no
log exists at all -- and it lived in a different directory from the per-level
report, so there were two places to look. Both files are now plain `.txt` in the
mod's own folder, and both paths are shown in the settings menu:

```
/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyReport.txt    per-level report
/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifySession.txt   full session log
```

Two things about the session log were worth fixing while renaming it:

- **It flushed on every line.** That is an sdcard write per log line, on
  whichever thread logged -- including the main thread during gameplay, where
  Vivify can be noisy. Warnings and errors still flush immediately, since those
  are the lines that matter if the game stops before the buffer reaches disk;
  ordinary lines are now flushed at most a few times a second.
- **It had no size limit.** Capped at 8MB per session, after which lines go to
  logcat only and the file says so. It is truncated at launch, so this only has
  to bound a single play session.

## 0.9.0 — a report file you can actually find

paperlog output lives where a player cannot reach it without adb, so "send me
the log" was never a reasonable thing to ask. Vivify now writes its own
plain-text report to a fixed path under its own data directory, visible to any
file browser or over MTP:

```
/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyReport.txt
```

The path is also shown in the Vivify settings menu.

**Two blocks per level.** One when the level starts, one when it ends. The
start block is written at load, before gameplay, *specifically* so that a level
which then freezes still leaves its diagnostics on disk — a frozen game never
reaches the end-of-level write, so anything recorded only at the end would be
lost exactly when it matters most.

Each block carries the mod version, the graphics API and GPU name, the level
and bundle paths, whether the bundle was converted, the main-thread level-load
timings, the frame watchdog's worst frame and whether it stood down, the shader
audit (how many shaders are runnable, DirectX-only, or refused by this GPU, with
the refused ones named), shader-repair counts, texture decode counts, and the
source bundle's shader platforms.

The end block records why the level ended — quit or finished, song restarted, or
left — along with how far into the song it got. It reports the song position
rather than guessing "quit" versus "beaten", because by the time the reset runs
the `AudioTimeSyncController` is usually already gone.

The file is capped at 512KB and trimmed to the newest 256KB on a line boundary,
so leaving the mod installed cannot fill a headset. A write failure is swallowed
entirely: a diagnostic file must never be the reason the game breaks.

Covered by `tools/report/` — missing directories, appending rather than
overwriting, bodies without trailing newlines, the size cap and its trim notice,
and an unwritable path that must not throw. Ten checks under ASan/UBSan.

## Converting shaders PC -> Quest

Earlier versions of this README said conversion "cannot translate" DirectX
bytecode, which overstated it. It is not impossible; it is a substantial project
that has not been done. The honest state of it:

**Why it is possible in principle.** Unity's own DXBC cross-compiler,
[HLSLcc](https://github.com/Unity-Technologies/HLSLcc), turns DirectX bytecode
into GLSL, GLSL ES, Metal and Vulkan GLSL -- it is the tool Unity uses to build
GLES shaders in the first place. And for OpenGL ES targets Unity does not store
a binary at all: the shader blob holds **GLSL source text**. So the output format
is writable, not a proprietary binary blob.

**Why it has not been done here.** Four pieces are needed, and only the first
exists today:

1. **Locate and decode Shader assets in the bundle.** Done --
   `VivifySerializedFile.cpp` parses the SerializedFile object table and walks
   Shader objects through the embedded type tree. Covered by `tools/shaderscan/`.
2. **Decode Unity's shader blob.** Done -- `DecodeShaderPrograms` in
   `VivifySerializedFile.cpp`. `offsets`/`compressedLengths`/
   `decompressedLengths` are read out of the type tree as the nested tables
   Unity 2019.3+ writes (one group per platform), each sub-blob is
   LZ4-decompressed, and its `[offset, length]` program table is split into
   individual sub-programs -- format version, `ShaderGpuProgramType`, keyword
   tables and program bytes.

   The LZ4 block decoder is written out here rather than vendored: it is small,
   it runs on a headset, and it is fed untrusted bundle bytes, so every read and
   write is bounds-checked. Unity has used both 8-byte and 12-byte program-table
   entries; the entry size is determined from the data (only one of the two lays
   every program inside the blob and clear of the table) rather than from a
   version rule.

   This is also where the size of step 3 gets settled per bundle, because the
   scan now reports what the programs *are*:

   ```
   Vivify source bundle shaders: unity=2021.3.16f1 serializedFiles=1 shaders=24
     runnableOnQuest=0 platforms=[Direct3D 11(4)] programs=61 glslSource=0
     binary=61 programTypes=[D3D11 vertex sm5.0, D3D11 pixel sm5.0]
   ```

   `glslSource` counts programs stored as GLSL text, which are writable by
   string manipulation. `binary` counts the ones that need a real
   cross-compiler.
3. **Cross-compile** each DXBC program to GLSL ES. Done -- `VivifyDxbc.cpp`.

   HLSLcc was the obvious route and is not the one taken. It is roughly 30k
   lines built around Unity's own build system, and it would still have needed
   the reflection-to-uniform mapping below bolted on afterwards. What is here
   instead is a direct translator for the subset of Shader Model 4/5 that
   Unity's compiler emits for the unlit, effect, raymarch and blit shaders a
   Vivify map ships: the DXBC container (RDEF, ISGN/OSGN/OSG5, SHDR/SHEX), the
   token stream, and a GLSL ES 3.00 emitter.

   Two decisions carry most of the weight.

   *Registers are typeless, so the translation is too.* Every temp becomes a
   vec4 and integer work round-trips through `floatBitsToInt`/`intBitsToFloat`.
   The same four bytes are read as float by one instruction and as int by the
   next, so any model that infers a type per register has to be right every
   time or it silently miscompiles. The bit-cast form is always right and the
   driver's optimiser removes the casts.

   *Constant buffers are rebuilt as named uniforms.* Unity binds material
   properties by uniform name, so a shader that kept D3D's flat array of
   float4s would link and then receive nothing. Every `cb0[k].c` is resolved
   through the reflection data back to the variable covering that byte --
   `_Color.y`, `_Points[3].x`, `hlslcc_mtx4x4unity_ObjectToWorld[2]` -- and the
   components of one register are put back together as a swizzle when they all
   land in the same variable. Matrices keep HLSLcc's `hlslcc_mtxRxC` prefix and
   attributes and varyings are named `in_SEMANTIC#`/`vs_SEMANTIC#` for the same
   reason: those are the names Unity's own GLES shaders use and the ones the
   engine matches against.

   *Coverage.* Vertex, fragment, geometry and compute programs; every Shader
   Model 4/5 arithmetic, bit-manipulation and control-flow instruction,
   including subroutines (`label`/`call`), `switch`, and the two-destination
   forms (`sincos`, `imul`, `udiv`, `uaddc`, `swapc`); the whole sampling
   family -- `sample`, `_l`, `_b`, `_d`, `_c`, `_c_lz`, `gather4`, `ld`,
   `ld_ms`, `resinfo`, `bufinfo` -- with compile-time texel offsets, shadow
   samplers and integer samplers; structured, raw and read/write buffers with
   their atomics; thread-group shared memory and barriers.

   The output version is not fixed. A plain vertex or fragment shader stays at
   GLSL ES 3.00; `textureGather`, `uaddCarry`, `imulExtended`, multisample
   fetches and storage buffers raise it to 3.10, geometry shaders and
   `textureGatherOffset` to 3.20. Quest's Adreno parts expose 3.2 and Unity
   compiles the source on the device, so this costs nothing where it is not
   needed.

   What is deliberately *not* translated, in each case because a wrong answer
   would be worse than none: tessellation (a hull program is several
   instruction streams with their own register spaces); double precision, which
   GLSL ES does not have; per-sample evaluation (`eval_*`, `sample_pos`);
   `msad`; append/consume buffers; and a geometry shader that passes a semantic
   straight through, which would need one varying name to be both an input and
   an output -- programs are translated one at a time, so the pipeline-wide
   rename that needs is not available.

   Anything outside the subset fails by name -- "instruction 'msad' is outside
   the translated subset" -- rather than emitting plausible wrong GLSL, and a
   shader that does not translate is left exactly as it was. It keeps its
   DirectX programs, does not run here, and falls to the stand-in path, which
   is what it would have done anyway.

   `ConvertShadersToGles` is the whole thing end to end, and it is what the
   on-device conversion runs (settings: "Translate Shaders On Conversion").

   A D3D11 sub-program is *not* a bare DXBC container: Unity writes its own
   binding header in front of the bytecode, so the container is located by its
   header rather than assumed to be at offset zero. Getting that wrong made the
   first version of this reject every shader in every bundle before decoding a
   single instruction, and it is why the fixtures carry the prefix too.

   Tested by `tools/dxbc/`: 128 checks under ASan/UBSan against hand-assembled
   containers, plus five end-to-end cases in `tools/bundleconvert/`. There is
   no DirectX compiler on a Linux host and no Quest here, so the fixtures are
   written from the format documentation rather than captured from fxc. That
   proves the decoder reads what the format *says*, not what Microsoft's
   compiler happens to emit -- a real limit, and the reason the setting has an
   off position.
4. **Re-serialize.** Done, both halves.

   `RewriteSerializedFile` rebuilds one file with objects' bodies replaced. The
   metadata is copied verbatim -- type tree, externals, script types, user
   information, padding -- and only the object table's `byteStart`/`byteSize`
   fields are patched, because nothing about an object's *body* changes any of
   the rest. Anything the parser does not understand survives untouched, and a
   file whose object table is empty or unfamiliar keeps its whole payload rather
   than rebuilding to nothing.

   `ReplaceNodeData` does the same one level out: a SerializedFile that changes
   length moves every archive node stored after it, and the directory table
   records absolute offsets into the unpacked data.

   Both preserve the gaps around what they move, so a rewrite with no edits
   reproduces its input byte for byte -- which is the only property that can be
   checked before there is a real converted shader to write. `conv --repack`
   runs a whole bundle through the path and the tests require the payload back
   unchanged.

All four steps are now written. What cannot be claimed is that the output is
*correct*: the only way to know whether a translated shader draws what the
mapper intended is to run a converted map on a headset. The parts that can be
checked from here are checked -- every framing and bounds path, every
truncation, hundreds of corruption trials, and the requirement that a converted
bundle still parses and converts to nothing on a second pass -- and the parts
that cannot are behind a setting that turns the translation off again.

**What to check on a real map.** Every conversion now logs what the source
bundle's shaders were actually built for:

```
Vivify source bundle shaders: unity=2021.3.16f1 serializedFiles=1 shaders=24 runnableOnQuest=0 platforms=[Direct3D 11(4)]
```

`runnableOnQuest` counts shaders carrying a GLES3 or Vulkan program. If it is 0
and `platforms` is Direct3D-only, the map's own shading -- raymarching included
-- cannot run until steps 2-4 exist or the mapper ships an Android bundle. If it
is non-zero, the shaders are present and something else is wrong, which is a
different and much smaller problem.

## Geometry shaders

Short answer: a geometry shader can run on Quest only if the map ships a bundle
built for **Android**, and only if the game's graphics API exposes a geometry
stage. Neither is something this mod can arrange.

Two separate walls stand between a Vivify map and a working geometry shader, and
they need completely different fixes:

1. **No Android program exists.** Upstream Vivify only ever builds one bundle
   per map: `VivifyController.BUNDLE_FILE` is `$"bundle{BUNDLE_SUFFIX}.vivify"`
   and `BUNDLE_SUFFIX` is `Windows2021` (or `Windows2019` on 1.29.1) — there is
   no Android suffix in the upstream source at all. So a PC-authored map's
   shader programs are DirectX bytecode. The on-device converter rewrites the
   archive's target platform, which is what makes the meshes, prefabs, materials
   and property values load; it cannot invent GLES or Vulkan programs that were
   never compiled. Every shader in a converted bundle is dead on arrival, and a
   geometry shader is no more or less dead than a plain one. The only real fix
   is an Android bundle — built by the mapper, or fetched from the community
   bundle repo when someone has published one for that map's checksum.

2. **The device has no geometry stage.** Even with real Android programs, a
   geometry stage is only available under OpenGL ES 3.2, via
   `GL_EXT_geometry_shader`. Adreno's Vulkan driver reports
   `VkPhysicalDeviceFeatures.geometryShader` as false and always has — Qualcomm
   has never shipped geometry or tessellation stages on Vulkan. Which API Beat
   Saber uses is baked into its APK at build time, so no mod can switch it.
   Unity's `SystemInfo` in this build exposes no `supportsGeometryShaders` to
   ask directly, so the mod logs `Vivify graphics: api=… geometryShaderStagePossible=…`
   once per session instead.

Unity picks the highest-LOD subshader whose hardware requirements the device
meets. A shader that ships a geometry-shader subshader **and** a plain fallback
subshader therefore already works — Unity selects the fallback silently. Only a
shader whose every subshader needs a stage the device lacks actually fails.

### Reading the audit

Because those two walls look identical from the outside ("the map is invisible"),
every bundle load now logs which one it hit:

```
Vivify shaders: bundle='…' converted=true total=24 runnable=0 noAndroidProgram=24 deviceRejected=0
```

- `noAndroidProgram` — the shader has zero subshaders for this platform. Wall 1.
  Expect this to equal `total` for any converted bundle.
- `deviceRejected` — subshaders exist, so the bundle *was* built for Android, and
  this GPU turned every one of them down. Wall 2, and the bucket a geometry
  shader lands in. Each such shader is logged by name with its subshader count,
  pass count and maximum LOD, so a mapper can see exactly what to add a fallback
  subshader for.
- `runnable` — shaders that will actually draw.

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

I don't have an Android NDK toolchain in the environment this was built in, and
`qpackages.com` is blocked by its egress policy, so **the mod itself has not
been compiled**. To build it yourself:

1. Install [QPM.CLI](https://github.com/QuestPackageManager/QPM.CLI) and the Beat
   Saber Quest modding toolchain (Android NDK, CMake/Ninja) — see the
   [BSMG modding docs](https://bsmg.wiki/quest/quest-modding-intro.html) if
   you don't already have this set up.
2. From this project's root: `python3 scripts/restore-deps.py`
3. `qpm s build` (or your usual `pwsh scripts/build.ps1`)
4. Package with `scripts/createqmod.ps1`, or `qpm s qmod`.
5. Test on-device (`adb`/QuestPatcher install).

For packages unavailable through QPM, download the dependency from its GitHub
repository and place it in your project's `extern` directory, then update your
`qpm.json`/build files manually.

The GitHub Actions workflow in `.github/workflows/build.yml` does all of this
on push to `main` and on pull requests.

### No qpackages.com

`qpm restore` resolves every dependency through **qpackages.com**; if that
registry is down or blocked, the project cannot be built at all. Step 2 above
uses `scripts/restore-deps.py` instead, which resolves everything from
**github.com only** using the manifest in `scripts/dependencies.json`, and
regenerates `extern.cmake` from it. The generated `extern.cmake` is verified to
carry exactly the same include directories and compile flags as the one qpm
produces. CI does the same; the `qpm-action` step is kept only for the NDK.

Thirteen of the nineteen dependencies were recoverable straight from
`qpm.shared.json`, because they publish their native library as a GitHub release
asset. Six headers-only packages record their repository *only* on
qpackages.com, so `dependencies.json` currently leaves those `null` and
`restore-deps.py` refuses to run until they are filled in. Run
`python3 scripts/discover-deps.py` once against any `extern/` tree a previous
`qpm restore` produced — it reads each package's own `qpm.json` and writes the
repositories back into the manifest. Commit the result and the registry is out
of the loop permanently.

The build workflow takes a `dependency_source` input when run manually:
`auto` (default — GitHub where the manifest allows, qpackages.com for the
remainder), `github` (GitHub only; fails rather than touching the registry),
or `qpackages` (`qpm restore` only). Push and pull-request runs use `auto`.
The source used is printed in the run summary.

See [`scripts/README-deps.md`](scripts/README-deps.md) for the details,
including how to vendor `extern/` outright for a build that needs no network.

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
