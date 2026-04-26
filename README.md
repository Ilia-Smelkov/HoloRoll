# HoloRoll

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-blue.svg)](#)
[![REAPER 6+](https://img.shields.io/badge/REAPER-6.0%2B-orange.svg)](https://www.reaper.fm)

**Visual reference for REAPER cinematics: scrub a 3D character in lock-step with the timeline.**

HoloRoll is a native REAPER extension that loads MDD (Blender Point Cache)
animations alongside their OBJ topology and renders them in a docked OpenGL
viewport synchronized with the project playhead. Each animation gets its own
timeline region and remembers its camera and object orientation between visits.

> **Status: 0.1.0 — initial release.** Core playback loop, library scanner,
> region binding and per-animation pose memory are all in place. See the
> [roadmap](ROADMAP.md) for what's coming next.

<!-- TODO: drop a GIF here once the visualizer pass is in -->

## Features

- Loads MDD point caches (Big-Endian, Blender Point Cache layout) with a
  zero-dependency parser. VBO-backed playback path uses `glBufferSubData` for
  per-frame updates — no reallocation, no streaming hiccups.
- Pairs each MDD with an OBJ for full mesh display. If the basenames don't
  match, falls back to vertex-count matching so a single `character.obj` can
  serve `character_idle.mdd`, `character_walk.mdd`, etc.
- Auto-creates colored REAPER regions for the loaded library (default: muted
  purple). The region's start position drives the animation — drag a region
  on the timeline and playback follows immediately.
- Three render modes: Points, Wireframe, Solid. Animations without topology
  fall back to Points with a clear UI hint.
- Per-animation pose memory: orbit the camera and rotate the model with
  LMB / RMB drag — the orientation sticks to that animation. Switching
  regions restores the pose you set last time.
- Configuration through a plain-text INI file you can open from the viewport
  itself (`Open config` button) and reload without restarting REAPER.

## Installation

### From installer (recommended)

> *Coming with v0.1.0 release.* For now, build from source.

1. Download `HoloRoll-Setup-<version>.exe` from the
   [Releases page](https://github.com/Ilia-Smelkov/HoloScrubber/releases).
2. Run it. The installer detects your REAPER plugins folder automatically.
3. Restart REAPER. Open `Extensions → HoloRoll → Toggle Viewport`,
   or run the action `HoloRoll: Toggle Viewport` from the Action List.

The installer supports headless invocation for integration with other
tooling — see [docs/headless_install.md](docs/headless_install.md).

### From source

Requirements:

- Windows 10/11 x64
- Visual Studio 2022 (Desktop C++ workload)
- CMake 3.24+

Build and deploy in one go:

```powershell
git clone https://github.com/Ilia-Smelkov/HoloScrubber.git
cd HoloScrubber
.\scripts\bootstrap.ps1 -Preset x64-Release -DeployToReaper -KillReaper -RestartReaper
```

The script copies `reaper_holoroll.dll` to `%APPDATA%\REAPER\UserPlugins`
and (optionally) restarts REAPER for you.

## Usage

1. Open the viewport: `Extensions → HoloRoll → Toggle Viewport` or via Action
   List → `HoloRoll: Toggle Viewport`.
2. On first run, a folder picker asks where your `.mdd` + `.obj` files live.
   The choice is persisted to the config file and REAPER's project state.
3. Click `Place regions` in the viewport overlay (or run
   `HoloRoll: Place All Animation Regions`) to create one colored region per
   animation on the timeline.
4. Press Play. Each region locks the playback to its animation:
   - Inside a region: `frame = (playhead - region.start) × fps`, clamped to
     the last frame.
   - Between regions: shows the last frame of the most recent animation.
   - Before the first region: shows frame 0 of the first animation.

### Camera & object controls

| Input | Action |
|---|---|
| LMB drag | Orbit camera |
| RMB drag | Rotate object |
| Wheel | Zoom |
| `Reset camera` button | Restore default camera |
| `Reset object` button | Restore default object orientation |

### Actions registered in REAPER

| Command | Description |
|---|---|
| `HoloRoll: Toggle Viewport` | Show / hide the docked viewport |
| `HoloRoll: Choose Animations Folder` | Open the folder picker |
| `HoloRoll: Place All Animation Regions` | Wipe HoloRoll's regions and re-create them |
| `HoloRoll: Open Config File` | Open `holoroll_config.ini` in the default text editor |
| `HoloRoll: Reload Config` | Re-read the config file from disk |

### Config

The config file lives next to the DLL in `%APPDATA%\REAPER\UserPlugins\holoroll_config.ini`:

```ini
animations_dir=C:\Path\To\Your\MDD\Folder
fps=24
region_gap_seconds=1
```

Edit, save, then click `Reload config` in the viewport. To apply a new
`fps` or `region_gap_seconds` to existing regions, click `Place regions`
afterwards.

## File format expectations

- **MDD** — Big-Endian Blender Point Cache. Layout: `int32 totalFrames`,
  `int32 totalPoints`, then `float32 times[totalFrames]`, then
  `float32 coords[totalFrames × totalPoints × 3]` (XYZ interleaved per point).
- **OBJ** — Wavefront OBJ. Only `v` and `f` lines are read; `vt` / `vn` are
  ignored. Faces with more than 3 vertices are fan-triangulated.

The pairing strategy is, in order:

1. `<basename>.mdd` ↔ `<basename>.obj` (e.g. `walk.mdd` ↔ `walk.obj`)
2. Fallback: any OBJ in the folder whose vertex count matches the MDD's
   `totalPoints`.

## Roadmap

See [ROADMAP.md](ROADMAP.md) for the full plan. Highlights:

- **v0.2** — Persistent poses across project reopens; basic grayscale material;
  pre-roll at region start.
- **v0.3** — Folder watcher with new-file prompts; external converter hook;
  in-overlay region manager table.
- **v0.4** — Alembic / PC2 import via a separate CLI utility.

## Contributing

Bug reports and PRs are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) — Copyright (c) 2026 Ilia Smelkov / Muted Games.

The bundled REAPER SDK in `third_party/reaper-sdk/` is © Cockos Inc. and
distributed under its own permissive license — see that folder for details.
ImGui is © Omar Cornut and contributors, also MIT-licensed.
