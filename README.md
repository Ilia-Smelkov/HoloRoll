# HoloRoll

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-blue.svg)](#)
[![REAPER 6+](https://img.shields.io/badge/REAPER-6.0%2B-orange.svg)](https://www.reaper.fm)

**See your animation while you score it.**

HoloRoll plugs into REAPER and shows your 3D animation right inside
the DAW. The animation plays in lock-step with the playhead, so when
you're picking a sound for a footstep, a sword swing, or a UI flourish,
you can actually *watch* the moment instead of guessing from a waveform.
Drop a `.glb` from Blender into your project's animations folder, and
it shows up on the timeline as a media item you can move, copy, and
layer sounds on top of.

> **Status: 0.11.1.** Drop animations from Blender into a folder, get
> them on the timeline, design sound to match. Everything below works
> today on Windows.
>
> **Coming next: Unreal Engine integration.** A live bridge from your
> Unreal project so HoloRoll can pull animations directly from
> sequences without exporting `.glb` files manually. This is the
> headline feature we're building toward — the file-based workflow
> here is the foundation it'll sit on top of.
>
> See [CHANGELOG.md](CHANGELOG.md) for what's shipped and
> [ROADMAP.md](ROADMAP.md) for what's next.

<!-- TODO: drop a GIF here once the visualizer pass is in -->

## What it does

- **Watch animations on the timeline.** Each animation is a media item.
  The viewport renders whatever item is under the playhead, so you scrub
  motion the same way you scrub audio.
- **Drop and go.** Save a `.glb` from Blender into
  `<project>/Animations/<project_name>/`, drag it onto the viewport, or
  drop it in the global Incoming folder — HoloRoll picks it up within a
  second and asks if you want to place it.
- **Variations for sound design.** One animation, many items. Set
  Variations to 3 and you get `door_open`, `door_open_02`,
  `door_open_03` — same motion, different items, layer different
  sounds on each.
- **Pre-roll / post-roll buffers.** When the playhead sits a second
  before an item, the viewport already shows frame 0. After the item,
  the last frame holds. Gives you breathing room for anticipation
  whooshes and tail reverbs without affecting the item's actual length.
- **Scale awareness.** A 1.80 m reference figure stands next to your
  model so you can eyeball whether you're scoring a person, a Godzilla,
  or a doorknob. Dimensions plate in the corner shows the live bbox in
  metres.
- **3/4 default framing.** New animations snap to a Blender-style
  front-right-top view, with the camera distance auto-fit to the model.

## Formats

**`.glb` (skinned glTF) is the primary format.** Export from Blender's
File → Export → glTF 2.0 with Animation enabled. Multiple animations
in one file become separate library entries.

**`.mdd` (Blender Point Cache)** is also supported, for vertex-level
animation that doesn't fit a skinned rig (cloth sims, soft bodies,
shape-deforming effects). Pair it with a matching `.obj` for full
shading; without an `.obj` you get a points-only render.

## Installation

### From installer (recommended)

1. Download `HoloRoll-Setup-<version>.exe` from the
   [Releases page](https://github.com/Ilia-Smelkov/HoloRoll/releases).
2. Run it. The installer detects your REAPER plugin folder automatically.
3. Restart REAPER. Run the action `HoloRoll: Toggle Viewport` from the
   Action List (or assign it a hotkey).

The installer supports headless invocation — see
[docs/headless_install.md](docs/headless_install.md).

### From source

```powershell
git clone https://github.com/Ilia-Smelkov/HoloRoll.git
cd HoloRoll
.\scripts\bootstrap.ps1 -Preset x64-Release -DeployToReaper -KillReaper -RestartReaper
```

Requirements: Windows 10/11 x64, Visual Studio 2022, CMake 3.24+.

## Quick start

1. Save your REAPER project (HoloRoll uses the project folder).
2. Action List → `HoloRoll: Toggle Viewport`.
3. Drag a `.glb` from Explorer onto the HoloRoll viewport.
4. Click `Place all` in the modal that appears.
5. Press Play. The item under the playhead drives the viewport.

## Camera controls

| Input | Action |
|---|---|
| RMB hold + drag | Mouse-look |
| RMB hold + WASD | Fly forward / back / strafe |
| RMB hold + Q / E | Move down / up |
| Mouse wheel | Adjust fly speed |
| LMB drag on rotation gizmo arc | Rotate object around that axis |
| `Reset camera` button | Snap to 3/4 default framing |

## Roadmap

See [ROADMAP.md](ROADMAP.md) for the full plan.

**The big one: Unreal Engine bridge.** Live link between an Unreal
project and HoloRoll, so animations and sequences come straight from
the game without going through `.glb` exports. This is what HoloRoll
is ultimately built for — game audio designers scoring directly
against the actual in-engine motion. The current `.glb` workflow is
the groundwork for that pipeline.

Other planned work: Unity bridge, motion analysis (auto-detect
bone movement keypoints to suggest hit timings), region manager UI,
full UI presentation pass.

---

## Format details

If you just want to use HoloRoll, you don't need this section.

### GLB
Standard glTF 2.0 binary. HoloRoll loads the first skinned mesh's
first primitive, walks the joint hierarchy, samples animation channels
at the configured fps, and bakes per-frame vertex positions via linear
blend skinning on load.

Supported:
- LINEAR and STEP interpolation
- UBYTE / USHORT joint indices, FLOAT / UBYTE / USHORT joint weights
- glTF default coordinate space (Y-up, right-handed)

Not supported:
- Morph targets / vertex animation without skinning
- CUBICSPLINE keyframes (silently degraded to LINEAR)
- Multiple meshes / multiple primitives per mesh (first wins)
- Embedded textures (stripped on load — HoloRoll uses Lambert grayscale)

Multiple animations inside one `.glb` produce separate library entries:
`<filestem>.<animname>` (or `<filestem>.<index>` if unnamed).

### MDD
Big-Endian Blender Point Cache. Layout: `int32 totalFrames`,
`int32 totalPoints`, then `float32 times[totalFrames]`, then
`float32 coords[totalFrames × totalPoints × 3]`.

OBJ pairing:
1. Same basename: `walk.mdd` ↔ `walk.obj`
2. Fallback: any OBJ in the folder whose vertex count matches the MDD's
   `totalPoints`. One `character.obj` can serve `character_idle.mdd`,
   `character_walk.mdd`, etc.

### Folder layout
```
<project_dir>/
  MyLevel.rpp
  Animations/
    MyLevel/             <-- per-project subfolder, isolated
      door_open.glb
      character_idle.glb
```

Global Incoming folder (auto-routes into the active project):
`%APPDATA%\REAPER\UserPlugins\HoloRollIncoming\`

### Config

`%APPDATA%\REAPER\UserPlugins\holoroll_config.ini`:

```ini
fps=24
region_gap_seconds=1
region_name_prefix=
hot_reload.enabled=1
scene.show_ground_plane=1
scene.ground_radius=20
scene.grid_step=1
scene.show_bbox_dimensions=1
scene.show_grid_labels=1
scene.show_reference_human=1
placement.variations=1
placement.pre_roll_seconds=1
placement.post_roll_seconds=1
placement.region_overhang_seconds=0.5
```

### Actions registered in REAPER

| Command | Description |
|---|---|
| `HoloRoll: Toggle Viewport` | Show / hide the docked viewport |
| `HoloRoll: Choose Animations Folder` | Override the project's default folder |
| `HoloRoll: Place All Animation Regions` | Place one item + region per animation |
| `HoloRoll: Open Config File` | Open `holoroll_config.ini` in editor |
| `HoloRoll: Reload Config` | Re-read the config from disk |

## Contributing

Bug reports and PRs are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) — Copyright (c) 2026 Ilia Smelkov / Muted Games.

Third-party components:

- **REAPER SDK** in `third_party/reaper-sdk/` — © Cockos Inc.,
  distributed under its own permissive license.
- **ImGui** — © Omar Cornut and contributors, MIT-licensed.
- **tinygltf** — © Syoyo Fujita and contributors, MIT-licensed.