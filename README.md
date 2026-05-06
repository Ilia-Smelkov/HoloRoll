# HoloRoll

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-blue.svg)](#)
[![REAPER 6+](https://img.shields.io/badge/REAPER-6.0%2B-orange.svg)](https://www.reaper.fm)

**3D animation reference for REAPER timelines: scrub a character or prop in lock-step with the playhead and design sound to match.**

HoloRoll is a native REAPER extension that loads MDD point caches and skinned
GLB files, renders them in a docked OpenGL viewport with proper lit shading
and a 3/4 default framing, and ties each animation to a media item on the
timeline. Drop new files into a folder and HoloRoll picks them up
automatically; duplicate items in REAPER to create variations driven by the
same animation but with different sounds.

> **Status: 0.9.0 — drop anywhere.** Project-relative `Animations/`
> folders, a global Incoming auto-router, and drag-n-drop from Windows
> Explorer onto the viewport. Drop a file, see it on the timeline. See
> [CHANGELOG.md](CHANGELOG.md) for the full history and
> [ROADMAP.md](ROADMAP.md) for what's coming next.

<!-- TODO: drop a GIF here once the visualizer pass is in -->

## Features

### Formats
- **MDD point caches** — Big-Endian Blender Point Cache, parsed with no
  external dependency. Pair an MDD with an OBJ for topology (Wireframe and
  Solid render modes); points-only fallback if no OBJ matches.
- **GLB skeletal animation** — full glTF Binary support via tinygltf. Linear
  blend skinning is computed on CPU and baked to per-frame vertex positions
  on load, so the render path is identical to MDD. LINEAR + STEP keyframe
  interpolation supported; CUBICSPLINE channels degrade to LINEAR. Multiple
  animations inside a single `.glb` become separate timeline entries
  (`character.idle`, `character.walk`, ...). Embedded textures are stripped
  on load — HoloRoll uses Lambert grayscale, not material textures.

### Workflow
- **Items as the playback driver.** Each animation is a media item on the
  timeline (empty, named via take). Move it, copy it, resize it — REAPER's
  native item editing just works. Regions of the same name are also created
  for visual grouping and follow items around by default.
- **Variations.** Duplicate an item in REAPER and it becomes `frog_jump_2`,
  `frog_jump_3`, etc. The variation suffix `_<digits>` is stripped during
  resolution, so all copies play the same animation. Layer different sounds
  on top to get sound variations against one source motion.
- **Project-relative folder.** HoloRoll watches `<project>/Animations/`
  next to the `.rpp`. The folder is auto-created on first save. Per-project
  override available via `Choose folder...` and saved into the `.rpp` so
  it travels with the project.
- **Three ways to add files:**
  1. Drag from Windows Explorer onto the HoloRoll viewport — visual
     feedback (green/amber/red border + status plate) shows whether the
     drop will be accepted.
  2. Drop into the global Incoming folder at
     `%APPDATA%\REAPER\UserPlugins\HoloRollIncoming\` — auto-routes
     into whichever project is currently active.
  3. Copy directly into `<project>/Animations/` and the watcher picks
     them up.
  Either way, after a 500ms debounce an ImGui modal asks whether to
  place the new animations as items at the play cursor.
- **Multi-track resolution.** When several items overlap the playhead on
  different tracks, the topmost track wins. Reorder tracks in REAPER to
  pick which animation plays.
- **Missing-animation warning.** An item with a name that no longer
  resolves to any loaded animation surfaces a red warning in the overlay
  rather than silently rendering the previous frame.

### Rendering
- **Three render modes:** Points, Wireframe, Solid. Solid uses per-face
  Lambert grayscale on a fixed light direction — gives form readability
  without the cost or fragility of real materials.
- **Sky gradient + ground grid.** Vertical sky gradient as the background;
  a major-and-minor camera-following grid that fades by 3D distance so it
  never shows a hard edge. Red and green axis lines run through the world
  origin for orientation.
- **3/4 default framing.** Newly-loaded animations snap to a Blender-style
  front-right-top view (yaw -35°, pitch -25°), with the camera distance
  derived from the model's bbox so models of any scale appear at the same
  apparent size on screen.
- **Per-animation pose memory.** Camera angles, fly speed, object rotation,
  and pivot offset are remembered per animation for the session.
- **Adjustable pivot.** Each animation gets an auto-computed pivot from
  its rest pose; user can offset it per axis. Both the rotation gizmo and
  the camera reset target use it.

### Configuration
- The animations folder is project-relative — no global config knob for
  it. Use `Choose folder...` for a per-project override (saved into the
  `.rpp`).
- Plain-text INI config opened from the overlay (`Open config` button) and
  reloadable without restarting REAPER. Persists frame rate, region gap,
  region-name prefix, hot-reload toggle, and ground plane settings
  (visibility, radius, grid step).

## Installation

### From installer (recommended)

1. Download `HoloRoll-Setup-<version>.exe` from the
   [Releases page](https://github.com/Ilia-Smelkov/HoloRoll/releases).
2. Run it. The installer detects your REAPER plugin folder automatically.
3. Restart REAPER. Run the action `HoloRoll: Toggle Viewport` from the
   Action List (or assign it a hotkey).

The installer supports headless invocation for integration with other
tooling — see [docs/headless_install.md](docs/headless_install.md).

### From source

Requirements:

- Windows 10/11 x64
- Visual Studio 2022 (Desktop C++ workload)
- CMake 3.24+

Build and deploy in one go:

```powershell
git clone https://github.com/Ilia-Smelkov/HoloRoll.git
cd HoloRoll
.\scripts\bootstrap.ps1 -Preset x64-Release -DeployToReaper -KillReaper -RestartReaper
```

The script copies `reaper_holoroll.dll` to `%APPDATA%\REAPER\UserPlugins`
and (optionally) restarts REAPER for you. The first build pulls ImGui and
tinygltf via FetchContent and takes 3–5 minutes; subsequent builds are
incremental.

## Usage

### Quick start

1. Open the viewport via Action List → `HoloRoll: Toggle Viewport`.
2. **Save your REAPER project** if you haven't already — HoloRoll uses
   `<project>/Animations/` as its default folder, so an Untitled project
   shows a hint screen until you save.
3. Add at least one track to the project.
4. Add some animations. Easiest way: drag `.glb` / `.mdd` / `.obj` files
   from Windows Explorer onto the HoloRoll viewport. They'll move into
   `<project>/Animations/` and the hot-reload modal will appear.
5. In the modal, click `Place all`. This creates one item + matching
   region per animation at the play cursor on the first selected track.
6. Press Play. The item under the playhead drives the viewport.

### Hot-reload

With the watcher running (default), drop new `.mdd` or `.glb` files
anywhere HoloRoll watches:

- Drag onto the HoloRoll viewport from Explorer (visual feedback while
  dragging)
- Drop into the global Incoming folder at
  `%APPDATA%\REAPER\UserPlugins\HoloRollIncoming\`
- Copy directly into `<project>/Animations/`

Within ~1 second the overlay shows a modal:

> Found 3 new animation(s): foo, bar, baz
>
> [Place all] [Skip]

`Place all` creates items at the play cursor on the first selected track,
each one placed after the previous with a `region_gap_seconds` separator.
Existing items are not touched.

### Variations for sound design

To create N sound variations of one animation:

1. Place an item once (e.g. `frog_jump`).
2. In REAPER, duplicate it (Ctrl+C / Ctrl+V or drag with Ctrl held).
   REAPER's default duplicate naming gives you `frog_jump_2`, `frog_jump_3`...
3. All copies play the same `frog_jump` animation — the `_N` suffix is
   stripped during resolution.
4. Add different audio takes on neighbouring tracks to get sound variety
   against one source motion.

If a real animation in your folder is actually called `foo_2.glb` (i.e. it's
not a variation of `foo`), direct matches always win over suffix stripping.

### Camera & object controls

| Input | Action |
|---|---|
| RMB hold + drag | Mouse-look (Fly camera) |
| RMB hold + WASD | Move forward / back / strafe |
| RMB hold + Q / E | Move down / up |
| Mouse wheel | Adjust fly speed |
| LMB drag on rotation gizmo arc | Rotate object around that axis |
| `Reset camera` button | Snap to 3/4 default framing for current model |
| `Reset rotation` button | Restore default object orientation |
| `Reset pivot` button | Restore auto pivot |

The rotation gizmo follows Blender's color convention: X = red, Y = blue,
Z = green.

### Actions registered in REAPER

| Command | Description |
|---|---|
| `HoloRoll: Toggle Viewport` | Show / hide the docked viewport |
| `HoloRoll: Choose Animations Folder` | Override the project's default animations folder |
| `HoloRoll: Place All Animation Regions` | Wipe regions and place one item + region per animation |
| `HoloRoll: Open Config File` | Open `holoroll_config.ini` in the default text editor |
| `HoloRoll: Reload Config` | Re-read the config file from disk |

### Config

The config file lives next to the DLL in
`%APPDATA%\REAPER\UserPlugins\holoroll_config.ini`:

```ini
fps=24
region_gap_seconds=1
region_name_prefix=
hot_reload.enabled=1
scene.show_ground_plane=1
scene.ground_radius=20
scene.grid_step=1
```

Edit, save, then click `Reload config` in the viewport. The animations
folder is **not** in this file — it's per-project (default
`<project>/Animations/`, optionally overridden via `Choose folder...`
and saved into the `.rpp`). The legacy `MDD: ` prefix is recognised on
read regardless of `region_name_prefix`, so projects from v0.3.x continue
to find their animations after upgrading.

## File format expectations

### MDD
Big-Endian Blender Point Cache. Layout: `int32 totalFrames`,
`int32 totalPoints`, then `float32 times[totalFrames]`, then
`float32 coords[totalFrames × totalPoints × 3]` (XYZ interleaved per point).

OBJ pairing strategy, in order:

1. `<basename>.mdd` ↔ `<basename>.obj` (e.g. `walk.mdd` ↔ `walk.obj`)
2. Fallback: any OBJ in the folder whose vertex count matches the MDD's
   `totalPoints`. This lets one `character.obj` serve `character_idle.mdd`,
   `character_walk.mdd`, etc.

OBJ parser only reads `v` and `f` lines; faces with more than 3 vertices
are fan-triangulated.

### GLB
Standard glTF 2.0 binary container. HoloRoll loads the first skinned mesh's
first primitive (additional meshes/primitives are ignored with a warning),
walks the joint hierarchy, samples animation channels at the configured fps,
and bakes per-frame vertex positions via linear blend skinning.

Supported:
- LINEAR and STEP interpolation
- UBYTE / USHORT joint indices, FLOAT / UBYTE / USHORT joint weights
- glTF default coordinate space (Y-up, right-handed) — same as the viewport

Not supported in v0.9.0:
- Morph targets (vertex animation in `.glb`) — files without skinning are
  not loaded
- CUBICSPLINE keyframes — silently degraded to LINEAR
- Multiple meshes / multiple primitives per mesh — first wins

If a `.glb` contains N animations, you get N separate library entries:
`<filestem>.<animname>` for named channels, or `<filestem>.<index>` if
unnamed. A single-animation file uses just `<filestem>`.

## Roadmap

See [ROADMAP.md](ROADMAP.md) for the full plan. Highlights:

- **Next** — `+ Place` buttons in overlay library list for repeated
  insertion; pose persistence by basename across project reopens;
  pre-roll for items.
- **Later** — region manager UI; foreign-region collision detection;
  engine bridges (Unreal / Unity) talking to the Incoming folder; PC2 /
  Alembic conversion via a separate CLI utility; texture and material
  support; full UI presentation pass.

## Contributing

Bug reports and PRs are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) — Copyright (c) 2026 Ilia Smelkov / Muted Games.

Third-party components:

- **REAPER SDK** in `third_party/reaper-sdk/` — © Cockos Inc., distributed
  under its own permissive license. See the folder for details.
- **ImGui** — © Omar Cornut and contributors, MIT-licensed.
- **tinygltf** — © Syoyo Fujita and contributors, MIT-licensed. Pulled in
  via FetchContent; see [docs/third_party.md](docs/third_party.md) if it
  exists.
