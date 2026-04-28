# Changelog

All notable changes to HoloRoll are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/Ilia-Smelkov/HoloRoll/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.3.0
[0.2.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.2.0
[0.1.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.1.0
