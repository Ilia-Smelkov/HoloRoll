# Changelog

All notable changes to HoloRoll are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.5.1] — 2026-05-05

Patch fixing GLB files with embedded textures.

### Fixed
- **CesiumMan and other textured GLB samples now load correctly.** The
  v0.5.0 build set `TINYGLTF_NO_STB_IMAGE` to skip the stb image stack
  (we don't render textures), but tinygltf's default behaviour is to
  return an error from the *whole parse* if it encounters an `image`
  block with no loader registered. Geometry and animation data were
  fully extractable but never reached our code. The loader now
  registers a no-op image callback that succeeds with a placeholder
  1x1 white pixel; image data is never sampled downstream so the
  placeholder is harmless.

## [0.5.0] — 2026-05-05

GLB skeletal-animation support. Drop a Blender-exported skinned `.glb`
into your animations folder and HoloRoll renders it the same way it
renders MDD: regions, playhead sync, Lambert shading, all of it. No
separate companion files needed (topology and skin live inside the GLB).

### Added
- **GLB import** for skeletal animation. CPU linear-blend skinning baked
  to per-frame vertex positions on load, in the same memory layout as
  MDD so the render pipeline is unchanged.
- Multiple animations per file: a `.glb` containing N animations becomes
  N separate `LoadedAnimation` entries with basenames
  `<filestem>.<animname>` (or `<filestem>.<index>` for unnamed channels).
  A single-animation `.glb` keeps the simple `<filestem>` basename.
- Interpolation support: LINEAR + STEP. CUBICSPLINE channels fall back
  to LINEAR sampling.
- New dependency: [tinygltf](https://github.com/syoyo/tinygltf) v2.9.3
  via FetchContent. Header-only, image stack disabled
  (`TINYGLTF_NO_STB_IMAGE`).

### Changed
- `AnimationLibrary::ScanFolder` now takes `fps` as a second argument
  (needed because GLB animations are continuous-time and must be
  resampled at the configured frame rate when loaded).
- `LoadedAnimation` is now source-agnostic. New accessors `TotalFrames()`,
  `TotalPoints()`, `VerticesForFrame()`, `TriangleIndicesPtr()`,
  `HasTopology()` hide the MDD-vs-GLB distinction from the rest of the
  codebase. Direct access to `anim.mdd` / `anim.obj` / `anim.glb` is no
  longer needed in render or timer code.
- Folder picker title now reads "Select animations folder (.mdd / .glb)".

### Limitations
- **One mesh, one primitive per GLB.** If the file contains multiple
  meshes or a mesh with multiple primitives (per-material splits), only
  the first is loaded; others are ignored.
- **No morph targets yet.** GLB files exported with shape-key animations
  but no skin won't render anything animated. Add a skinned variant if
  this matters; full morph-target support is on the roadmap.
- **CUBICSPLINE channels degrade to LINEAR.** Common with auto-tangent
  exports from Blender; visible only on tightly-curved channels.
- **Files with multiple animations and a non-identity mesh node
  transform** assume that the mesh node sits at the world origin.
  Blender's default exports satisfy this; non-default rigs may show
  scale or offset.

### Known issues
- ~~Some GLB files (e.g. CesiumMan.glb from the Khronos sample assets)
  fail to load or render incorrectly.~~ Fixed in v0.5.1.
- Hot-reload watcher may not pick up new GLB files dropped into the
  watched folder after the initial scan. Re-selecting the same folder
  via `Choose folder...` works around this. Tracking for v0.5.2.

## [0.4.0] — 2026-05-05

Library workflow upgrade. Two specific friction points addressed:
verbose region names, and having to manually re-scan the folder when
drops new files into it.

### Added
- **Hot-reload watcher.** A Win32 `ReadDirectoryChangesW` worker thread
  watches the configured animations folder. When new `.mdd` files appear,
  events are debounced for ~500ms (so a burst of file copies registers as
  one batch), the library is rescanned, and the user is prompted via an
  ImGui modal: *"Found N new animations — place regions for them after
  the last existing region?"* with `Place all` / `Skip`. Regions are
  appended after `max(any region.end) + region_gap_seconds` so existing
  regions (ours or the user's) are never overwritten.
- New config key `hot_reload.enabled` (default `1`). Set to `0` to
  disable the watcher; takes effect on `Reload config`.
- New config key `region_name_prefix` (default empty). Lets the user
  restore a custom prefix on region names if they want one.

### Changed
- **Region names no longer carry the `MDD: ` prefix by default.** Fresh
  `Place regions` produces names like `frog_jump` instead of `MDD: frog_jump`.
  Old projects with `MDD: foo` regions keep working: `Place regions`,
  `ReadLiveRegionsFromReaper`, and `DeleteOurRegions` all recognise both
  the legacy prefix and any currently-configured one.
- `AnimationLibrary::kRegionNamePrefix` (`constexpr` static) replaced
  with runtime-mutable static state via `SetRegionNamePrefix()` and
  `RegionNamePrefix()`. The legacy value `"MDD: "` is exposed as
  `kLegacyRegionNamePrefix` for compat-matching.

### Notes
- Hot-reload triggers a full library rescan, which **resets per-animation
  pose memory** (camera angle, pivot offset, etc.) for animations that
  were already loaded. This is a known limitation — the pose store keys
  by index, which is invalidated by rescan. Future work (C-1 in roadmap)
  will move pose storage into project ext-state and key by basename.

## [0.3.0] — 2026-04-28

Lit shading, sky background, infinite-feeling ground grid. The viewport
now looks like a real 3D scene rather than a debug widget.

### Added
- **Per-face Lambert shading** in Solid mode. Each triangle gets its own
  flat-shaded grayscale tone derived from its normal and a fixed light
  direction; shape and orientation are now legible at a glance.
- Per-triangle face normals computed once on rest pose during animation
  load (`LoadedAnimation::restNormals`).
- **Vertical sky gradient** as the viewport background, replacing the
  flat dark clear colour. Matches Unreal-ish editor tones.
- **Infinite-feeling ground grid**: minor + major lines, both follow the
  camera in XZ, fade with 3D distance, never show a hard edge. World
  axis lines (X red, Z green) pass through the actual world origin so
  the user can see where (0,0,0) is.
- **3/4-perspective default camera framing.** Newly-loaded animations
  now snap to a Blender-style front-right-top view (yaw=-35°, pitch=-25°)
  with the camera distance derived from the model's bbox so the model
  appears at a consistent ~60% of viewport height regardless of its
  world-scale. Same logic powers the `Reset camera` button.
- **Persisted scene settings** in `holoroll_config.ini`:
  `scene.show_ground_plane`, `scene.ground_radius`, `scene.grid_step`.
  Toggling the checkbox or moving the sliders now survives a REAPER
  restart. Editing the file directly and using `Reload config` also
  applies the new values.

### Changed
- Camera far-plane increased from 200 to 500 units so distant grid lines
  remain in the frustum.
- Render-mode tinting that varied with playhead time has been removed.
  Solid is now per-face Lambert, Wireframe and Points are flat off-white.
  The old time-tint was a debug hack from the prototype.
- REAPER console output silenced by default (`kVerboseLog = false` in
  `entry.cpp`). The plugin no longer spams the console on every folder
  scan, region build, or config reload. Flip the constant to re-enable
  for debugging.

### Fixed
- **Hard crash** (`vector subscript out of range`) when an OBJ paired by
  vertex-count happened to reference indices past the end of the MDD's
  per-frame vertex list. Solid and Wireframe immediate-mode paths now
  bounds-check every triangle and skip bad ones instead of crashing.

## [0.2.0] — 2026-04-28

Camera rework, rotation gizmo, configurable pivot. First end-to-end usable
build for browsing model libraries, not just verifying playback works.

### Added
- **Fly camera** (Unreal-style): hold RMB for mouse-look + WASD/QE
  movement. Wheel changes fly speed. Reset camera button restores a sane
  default position relative to the active model's bbox.
- Light camera smoothing (~80ms exponential decay) so view changes feel
  weighted instead of stepping in 1-pixel jumps.
- **Rotation gizmo**: three colored arcs (X red, Y blue, Z green —
  Blender convention) drawn around the object. LMB-drag any arc to rotate
  the model around that axis. Hover highlights the arc. Toggleable.
- **Adjustable pivot**: every loaded animation gets an auto-computed pivot
  from its frame-0 bbox centre. The user can offset that pivot per-axis
  with sliders, or reset to the auto value. The pivot is what the gizmo
  rotates around and what `Reset camera` looks at.
- Per-animation pose memory now stores camera position, look direction,
  fly speed, three-axis rotation, and pivot offset.

### Changed
- Inno Setup installer no longer asks "Folder exists, install anyway?"
  for `%APPDATA%\REAPER\UserPlugins`. The directory always exists for
  REAPER users; the prompt was just noise.
- Installer's *Select Destination Location* page is hidden by default.
  The plugin folder is auto-detected from the registry; users who need a
  different folder can pass `/DIR=...` on the command line.
- Custom installer exit codes (100/101) removed — Inno Setup's `[Code]`
  section can't actually emit them. Standard exit codes (0, 1, 3, 5)
  cover all real cases.
- `LoadedAnimation` carries `autoPivot[3]` and `autoExtent` so the
  viewport can size the gizmo and place the camera relative to the model.

### Fixed
- CMake build presets now explicitly select `Release` / `Debug`
  configuration so MSBuild doesn't silently emit Debug binaries when the
  user asked for Release.

### Added (in [Unreleased] from 0.1.0 → 0.2.0)
- Inno Setup installer (`installer/holoroll.iss`).
- `scripts/build_installer.ps1` — local installer build.
- `.github/workflows/release.yml` — CI release pipeline.
- `docs/headless_install.md`.

## [0.1.0] — 2026-04-26

Initial public release.

### Added
- MDD point-cache loader with Big-Endian binary parser (`MDDDataManager`).
- Companion OBJ loader for triangle topology (`ObjIndexLoader`), with
  vertex-count fallback when basenames don't match.
- Animation library that scans a folder, loads every MDD/OBJ pair,
  and lays out timeline regions per animation (`AnimationLibrary`).
- OpenGL viewport docked into REAPER, rendering Points / Wireframe / Solid
  modes via VBO + `glBufferSubData` per-frame uploads.
- ImGui overlay with library / config / playback / render sections.
- Per-animation pose memory (camera + object rotation + render mode) kept
  in memory for the session.
- Auto-coloured REAPER regions; playback locks to `region.start` so dragging
  a region updates playback immediately.
- Plain-text INI config with `Open config` and `Reload config` actions.
- First-run folder picker, persisted to both config file and REAPER ext-state.

### Removed (relative to MVP prototype)
- `TestMeshProvider` (placeholder mesh used during early development).
- `TimeToFrameMapper` (logic absorbed by `AnimationLibrary::ResolvePlayhead`).
- `ImGuiPanelState` (was an unused wrapper around a hardcoded action ID).
- `ActionBridge` and the F9 / F10 viewport hotkeys.

[Unreleased]: https://github.com/Ilia-Smelkov/HoloRoll/compare/v0.5.1...HEAD
[0.5.1]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.5.1
[0.5.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.5.0
[0.4.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.4.0
[0.3.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.3.0
[0.2.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.2.0
[0.1.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.1.0
