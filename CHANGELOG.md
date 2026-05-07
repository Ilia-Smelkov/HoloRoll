# Changelog

All notable changes to HoloRoll are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.12.0-alpha.3] — 2026-05-07

Bundles a JSFX placeholder plugin (`holoroll_motion`) so the upcoming
C++ side can host per-bone motion envelopes on it. No C++ changes yet —
this release is purely about getting the JSFX file into the right place
on disk.

### Added
- **`assets/effects/HoloRoll/holoroll_motion.jsfx`** — a 16-slider JSFX
  plugin that does no audio processing (`in_pin:none` / `out_pin:none`).
  Each slider is a `[0..1]` range parameter named "Bone 1" through
  "Bone 16". HoloRoll's C++ side will rename them and write envelopes
  onto them at motion-export time. Zero CPU cost during playback.
  - Why JSFX, not a track Volume / Pan envelope hack: REAPER envelopes
    must attach to a parameter; volume hijack would dirty audio routing
    and limit us to one curve per track. JSFX sliders are the canonical
    way to expose N independent envelope targets on a single track.
  - Why 16 sliders: 5 for top-active world bones, 5 for top-active
    local bones, 6 reserved for future manual-selection. Cheap to grow
    (JSFX supports up to 256) but 16 keeps the FX UI uncluttered.
- **Bootstrap deploys JSFX too** (`scripts/bootstrap.ps1`). Running
  `bootstrap.ps1 -DeployToReaper` now also copies
  `assets/effects/HoloRoll/*` into `%APPDATA%\REAPER\Effects\HoloRoll\`
  alongside the DLL deployment. New optional parameter
  `-ReaperEffectsDir` to override the destination.
- **Installer bundles JSFX** (`installer/holoroll.iss`). New
  `DefaultEffectsDir()` Pascal function resolves REAPER's Effects/
  directory (portable-aware, mirrors `DefaultPluginsDir`). New `[Files]`
  entry copies `payload\effects\HoloRoll\*` to that location.
  `[UninstallDelete]` removes the HoloRoll subfolder on uninstall —
  user-created JSFX in the main Effects/ folder are not touched.
- **`build_installer.ps1`** stages JSFX assets into
  `installer\payload\effects\HoloRoll\` before invoking ISCC.

### Note for users
- After installing this version, REAPER may need an FX rescan to see
  `HoloRoll/holoroll_motion`. Either restart REAPER, or run
  Options → Preferences → Plug-ins → Re-scan. We'll automate this from
  C++ in alpha.4 when the extension actually starts inserting the JSFX.
- Until alpha.4, this plugin sits idle on disk. Inserting it manually
  on a track is harmless (it does nothing) but won't show motion data —
  HoloRoll's C++ side has to write envelopes to populate the sliders.

### Internal
- New repo directory `assets/effects/HoloRoll/` (this is canonical —
  the same layout will host other bundled REAPER assets if we add them
  later).
- `installer/payload/` (gitignored) now also receives an `effects/`
  subdirectory at build time.


## [0.12.0-alpha.2] — 2026-05-07

Adds local-motion computation alongside world motion. Both metrics are now
logged to console at GLB load time, so users can compare which bones are
"first movers" (high local motion) vs which bones simply inherit motion
from rotating parents (high world motion, low local motion).

Still no UI — that's alpha.2.b.

### Added
- **Per-bone local-motion computation.** For each joint, transform a probe
  point (0,1,0) through the joint's own local TRS only (no parent chain),
  diff parent-space-to-parent-space across frames. Captures "this joint
  initiated motion" semantics: a child bone whose parent rotates but whose
  own local TRS is fixed shows zero local motion, even though its world
  position is changing.
- **Two-line console summary** on GLB load:
  ```
  loaded GLB 'door_open' (frames=36, points=1280, joints=12;
      top world: Door_Body=4.532, Door_Handle=4.218, Door_Hinge=2.104;
      top local: Door_Handle=2.110, Door_Body=1.846, Door_Hinge=0.000)
  ```
  The world ranking shows what's visually moving most; the local ranking
  shows what's actually being driven by animation channels.
  Quick door-anim sanity check: handle should dominate local before body
  starts (handle initiates), but in world both end up high (handle inherits
  body's rotation once body starts swinging).

### Changed
- `SummarizeTopActiveBones` is now generic over which motion vector to
  rank — takes a `motion` parameter instead of always using
  `anim.worldMotion`. Called twice per animation in `ScanFolder` (once
  for world, once for local).

### Internal
- New scratch buffer `prevJointLocalProbe` in the bake loop, parallel to
  `prevJointWorldPos`. Both Vec3 vectors of length jointCount.
- Local probe uses `ComposeTRS(currentTRS[n].t, .r, .s)` directly — the
  result lives in the joint's parent coordinate frame, which is exactly
  what we want for parent-space delta. No conversion to world needed.


## [0.12.0-alpha.1] — 2026-05-07

First pre-release toward v0.12.0 motion analysis. Lays the foundation:
per-bone world-motion magnitude is computed at GLB load time and surfaced
as a console summary, so we can sanity-check the data on real animations
before wiring it into UI / native REAPER envelopes.

This is a pre-release. Local motion, overlay UI, and envelope integration
are planned for subsequent alphas.

### Added
- **Per-bone world-motion computation.** Every GLB animation now produces
  a `worldMotion[joint][frame]` curve at load time — the Euclidean
  distance each joint's world position travelled between consecutive
  frames. Frame 0 is always 0 (no previous frame). The curve captures
  inherited motion: a foot bone whose hip parent rotates will show large
  world motion even if the foot's local transform is fixed.
- **Joint-name passthrough.** `LoadedAnimation::jointNames` now mirrors
  `skin.joints[].name` from the glTF, with `joint_<index>` fallback for
  unnamed channels.
- **Top-3 most-active bones in the load log.** The library scan log now
  includes a per-animation summary of the three joints with the highest
  total world-motion. Surfaced via `SpikeLog` so it's visible in REAPER's
  console without flipping the verbose-log constant. Format:
  `joints=N; top: BoneA=12.345, BoneB=11.812, BoneC=4.231`.
  Quick sanity-check that the data is plausible (e.g. on a walk cycle,
  feet/hands should dominate; on a door-open animation, the handle bone
  and door-body bone should appear).

### Internal
- New fields in `GlbLoader` (and matching getters `JointNames` /
  `LocalMotion` / `WorldMotion`): `jointNames_`, `localMotion_`,
  `worldMotion_`. `localMotion_` is allocated and zero-filled in this
  alpha — the actual local-TRS-delta computation lands in alpha.2.
- New fields in `LoadedAnimation`: `jointNames`, `localMotion`,
  `worldMotion`. Populated from the GLB after load via
  `CopyMotionDataFromGlb` (anonymous helper in `animation_library.cpp`).
- `SummarizeTopActiveBones` (anonymous helper) computes the top-N
  ranking by sum-of-world-motion across all frames and formats it as a
  single-line summary.
- World-motion magnitudes are computed inside the existing per-frame
  bake loop in `GlbLoader::LoadFromFileAtIndex`, reusing the already-
  computed `nodeWorld[skin.joints[j]]` translation column. No extra
  passes; the cost is one `std::sqrt` per joint per frame.
- Recentering (the v0.10.0 XZ origin pass) runs *after* motion
  computation. Since recentering is a constant offset, frame-to-frame
  deltas are unaffected — the order doesn't matter, but capturing
  motion before recentering keeps the math obvious.

### Not yet (alpha.2+)
- Local motion (TRS delta vs previous frame, captures "this bone
  initiated motion" semantics).
- Overlay "Motion" section with multi-line ImGui plot, top-N selector
  with group-diversity filter, local/world toggle, vertical playhead
  indicator.
- Native REAPER envelope integration (Track FX placeholder JSFX).
- Marker generation with threshold tuning.


## [0.11.1] — 2026-05-07

Refines the v0.11.0 placement semantics after testing. Items now stay
clean (length = animation duration only) and pre/post-roll act as global
visual playback buffers, not as item-baked length extensions.

### Changed
- **Variation suffix is now zero-padded.** `_2`, `_3` -> `_02`, `_03`,
  `_04`, ..., `_09`, `_10`, `_11`. Sort-friendly and visually consistent.
  Variation matching is unaffected (the `_<digits>` strip already accepts
  any digit count, including padded forms).
- **Item length = animation duration only.** Previously items were
  `preRoll + animation + postRoll` long, which baked the playback buffers
  into the item geometry. Now items are clean: their start matches frame
  0, their end matches the last frame.
- **Region length = item length + region overhang.** Region no longer
  includes pre/post-roll. The 0.5 s default overhang sits past the item
  end as a visual handle.
- **Pre/post-roll are now GLOBAL playback buffers.** They live in the
  current placement settings (config-stored, edited in the inline
  fields). At playback time, the resolver expands its match window by
  these values: while the playhead sits in the buffer zone before an
  item, frame 0 is shown (held); after an item, the last frame is shown
  (held). Editing pre/post-roll updates all items immediately — no
  re-place needed.
- Per-item P_EXT keys for pre/post-roll (introduced in v0.11.0) are no
  longer written. They're still read by older v0.11.0 timelines but
  ignored — the global current settings always win.

### Behaviour
- Visually on the timeline:
  ```
            [pre-roll buffer]   [== item ==]   [region overhang]
                                                   [post-roll buffer]
            playhead shows      normal play        playhead shows last
            frame 0 here                           frame here
  ```
  Pre-roll and post-roll buffers have no visible representation in the
  timeline — they're just zones where the viewport keeps showing the
  appropriate held frame.
- Region overhang stays as a separate, visible handle past item end.

### Internal
- `MakeVariationName` uses `"_%02d"` for padding.
- `CreateNamedItemWithRolls` no longer adds pre/post-roll to length.
  Signature kept for source compatibility; the params are ignored.
- `ResolvePlayheadFromItems` takes `globalPreRollSec` /
  `globalPostRollSec` and expands the match window by these.
- `EnumProjectItems` still populates `DiscoveredItem.preRollSec` and
  `postRollSec` but always sets them to 0 (legacy P_EXT values are
  ignored). Both fields will be removed in a later cleanup.

## [0.11.0] — 2026-05-07

Placement workflow gets four UX upgrades: variations, de-dup, hold-frame
buffers, and region overhang. Each item now contains its own pre/post-roll
so playback respects the held frames.

### Added
- **Variations count.** Inline field next to "Place all" (1–20). When > 1,
  each animation produces N items with `_2`, `_3`, ... suffixes — same
  geometry, separate items so the user can layer different sounds.
  Variation suffix matching from v0.6.0 already handles resolution.
- **De-dup on Place all.** Items with the same name are no longer
  duplicated when "Place all" runs more than once. The new logic
  snapshots existing HoloRoll item names before placement and skips any
  that are already present. Skip count is logged to the console.
- **Hold-frame buffers (pre-roll / post-roll).** Each placed item now
  has configurable hold-frame regions:
  ```
  [pre-roll | animation | post-roll]
  ```
  - Pre-roll: frame 0 is held for N seconds before the animation begins.
    Useful for anticipation sounds or visual breathing room.
  - Post-roll: last frame is held for N seconds after the animation
    completes. Useful for reverb tails, settling motion sound, etc.
  - Defaults: 1.0 s each. Configurable per-placement via inline fields.
- **Region overhang.** The region that accompanies each item now extends
  past the item's end by a configurable amount (default 0.5 s). Just a
  visual handle for grouping; doesn't affect playback.
- All four options live in inline fields under the "Place all" button in
  the Library section. Edits are auto-persisted to
  `holoroll_config.ini` (debounced ~500 ms).
- Per-item P_EXT keys: `P_EXT:holoroll_pre`, `P_EXT:holoroll_post`.
  Stamped at placement time; read at playback time so the hold-frame
  semantics survive project saves and reloads.

### Changed
- `ResolvePlayheadFromItems` now subtracts pre-roll from local time and
  clamps to last frame on overrun. Items created in v0.10.x and earlier
  have no P_EXT pre/post values, default to 0, and behave identically
  to before.
- New helper `CreateNamedItemWithRolls` returns the created MediaItem*
  so callers can stamp regions/etc. directly. Old `CreateNamedItem`
  signature kept as a thin wrapper for the v0.6.0 spike test.
- `MakeVariationName(basename, n)` builds variation suffixes consistently
  across all three placement paths.

### Behaviour notes
- A second "Place all" run with no library changes is now a no-op (skips
  everything). Editing variations, then running again, will fill in only
  the missing variations.
- Items can be deleted manually in REAPER and re-placed with a fresh
  "Place all" run; nothing prevents that.
- The hot-reload modal ("new animations detected") also creates
  variations — the modal asks you to place N animations, you confirm,
  and each one fans out into the configured variation count.
- The region overhang is purely visual. Playback still resolves entirely
  off item bounds, not region bounds.

### Internal
- Four new persisted config keys: `placement.variations`,
  `placement.pre_roll_seconds`, `placement.post_roll_seconds`,
  `placement.region_overhang_seconds`.
- Two new helper APIs on `GlViewport`: `SetPlacementOptions`,
  `GetPlacementOptions`, plus `ConsumePlacementDirty` matching the
  Scene-settings pattern.
- `DiscoveredItem` carries `preRollSec` / `postRollSec`; `EnumProjectItems`
  reads them from P_EXT during enumeration.

## [0.10.1] — 2026-05-07

Fixes a real bug: when two REAPER projects sit in the same directory, they
shared a single `Animations/` folder and could see each other's content.
Libraries get scoped per-project starting now.

### Changed
- **Animations folder is now `<project_dir>/Animations/<project_name>/`**,
  not `<project_dir>/Animations/`. Two `.rpp` files in the same directory
  produce two completely separate libraries, side by side under one
  `Animations/` umbrella:
  ```
  <project_dir>/
    MyLevel.rpp
    BossFight.rpp
    Animations/
      MyLevel/      <-- only seen by MyLevel.rpp
        frog_jump.glb
      BossFight/    <-- only seen by BossFight.rpp
        enemy_hit.glb
  ```
- The Incoming auto-move pipeline still works: dropped files land in
  whichever project is currently active, in that project's own subfolder.
- Drag-n-drop onto the viewport behaves the same way — files end up in
  the per-project subfolder.

### Migration
- **Breaking change with no auto-migration.** Existing v0.7–v0.10 projects
  that relied on `<project_dir>/Animations/` will see an empty library
  after upgrade. Two ways to recover, pick whichever fits:
  1. **Move the files**: create `<project_dir>/Animations/<project_name>/`
     and move the contents of the old `Animations/` into it.
  2. **Use an override**: hit `Choose folder...` in the overlay and point
     it at the old shared path. The override is saved into the `.rpp` so
     it survives reopens. Other projects in the same directory continue
     using the new layout.

### Internal
- New helper `ProjectBasenameFromPath` extracts the basename without
  extension. Used only by `ResolveActiveAnimationsFolder`.
- `EnsureFolderExists` already recursed into missing parents, so the new
  two-deep path (`Animations/<name>/`) auto-creates correctly.

## [0.10.0] — 2026-05-07

Scale awareness: models land in the centre of the grid, the viewport tells
you how big they are, and an optional 2-metre stick figure stands next to
them for human reference.

### Added
- **Auto-recentering on load.** Every animation, GLB or MDD, is recentered
  on its first-frame bbox center (X and Z; Y is left alone so the model
  still rests on the ground plane). Models that used to appear off to
  one side of the grid now land in the middle every time, regardless of
  how the artist set up the export.
- **Bbox dimensions plate.** A small "X x Y x Z m" label in the top-right
  corner of the viewport, recomputed every frame from the live skinned
  bbox. Assumes 1 unit == 1 metre (Blender default). Toggle in the Scene
  section; on by default. Tooltip explains the unit assumption.
- **Grid labels.** Number labels (e.g. `1m`, `2m`, `5m`) on every major
  grid intersection within the visible radius. Anchored to the camera so
  they stay dense as you fly around. Capped at 64 labels per frame so a
  huge grid doesn't tank the draw cost. Toggle in Scene; on by default.
- **1.80-metre reference human.** A translucent grey stick figure (head
  with eyes and a smile, torso, arms hanging down, legs) drawn 0.5 m to
  the right of the user's bbox. 1.80 m tall by Vitruvian proportions.
  Procedurally generated from cylinders — no embedded asset, no extra
  disk space. Toggle in Scene; on by default.
- Three new persisted config keys: `scene.show_bbox_dimensions`,
  `scene.show_grid_labels`, `scene.show_reference_human`. Survive
  restart and `Reload config`.

### Behaviour
- Recentering happens once at load time. Per-animation pivot offset slider
  in the Object section still works on top of the recentered geometry.
- Reference human is anchored to the **frame-0 bbox edge**, not the live
  bbox, so it stays put while the animation plays. The model can move
  around freely; the human stays as a static size yardstick.
- Multi-animation GLBs are each recentered independently on their own
  frame 0. Switching between animations may shift the apparent "world"
  origin, but each animation always starts centered.

### Internal
- New helpers in `gl_viewport.cpp` (anonymous namespace):
  `ComputeBboxFromVertices`, `DrawBboxDimensionsImGui`,
  `DrawGridLabelsImGui`, `DrawStickFigureCylinder`, `DrawReferenceHumanGL`.
- `GlViewport::SetSceneSettings`/`GetSceneSettings` extended with three
  new bool parameters; old call sites updated. Config layer mirrors.
- Recentering pass added at the tail of `GlbLoader::LoadFromFileAtIndex`
  and `MDDDataManager::LoadFromFile` — both subtract the same XZ offset
  from every frame so the runtime sees pre-centered geometry. No render
  path changes.

## [0.9.1] — 2026-05-07

Two placement fixes that surfaced after v0.9.0 shipped.

### Fixed
- **Items can no longer overlap existing regions on placement.** Previously
  `Place all` and the hot-reload modal placed items starting at the play
  cursor, which produced visible overlap when the user dropped files
  multiple times in a row or when the cursor sat in the middle of
  existing content. Placement is now deterministic: items always go
  *after* `max(region.end) + region_gap_seconds`, regardless of where
  the cursor is. New items also land on a freshly-created track at the
  top of the project so the user keeps a clean separation between
  HoloRoll content and their own tracks.
- **Audio / video / midi items are no longer mistaken for HoloRoll items.**
  Previously, the playhead resolver walked every item in the project,
  read its name, and tried to match it against the animation library.
  An audio file the user named `frog_jump.wav` would accidentally drive
  playback if `frog_jump.glb` happened to be in the library. Now every
  item HoloRoll creates carries a per-item ext-state marker
  (`P_EXT:holoroll=1`); resolution and "+ Place" actions only consider
  marked items. Anything else REAPER displays on the timeline is
  invisible to HoloRoll.

### Behaviour
- New track at top: `Place all` and the hot-reload modal now insert a
  fresh track at index 0 each time. The selected-track logic from v0.9.0
  is gone — placement is no longer affected by which track happens to
  be selected.
- Placement origin: starts at `max(region.end) + region_gap_seconds` if
  any regions exist; at `0` otherwise.

### Migration
- **Items created in v0.9.0 and earlier do not carry the new marker.**
  After upgrade, those old items will no longer drive the viewport.
  Re-run `Place all` (or use the hot-reload modal) to recreate them in
  the new format. Old regions are recognised on cleanup as before.

### Internal
- New REAPER API binding: `InsertTrackAtIndex`.
- New helpers in `entry.cpp`: `MarkAsHoloRollItem`, `IsHoloRollItem`,
  `FindLastRegionEnd`, `EnsureTrackOnTop`. `CreateNamedItem` now tags
  every item it creates.
- `EnumProjectItems` filters by P_EXT marker; `PlaceOurItemsAndRegions`,
  `PlacePendingAtCursor`, `PlaceSingleAtCursor` rewritten to use the
  new top-track + post-region placement rule.

## [0.9.0] — 2026-05-06

A big release rolling up three internal milestones (project-relative
folders, global Incoming auto-routing, drag-n-drop from Explorer) into a
single workflow: drop animation files anywhere reasonable, watch them
appear on the timeline.

v0.7.0 and v0.8.0 were never tagged — their content is included here.

## Headline features

### Project-relative animations folder

The animations folder follows whatever REAPER project is active.

- `<project>/Animations/` is the new default folder. Open a saved REAPER
  project, and HoloRoll auto-creates `Animations/` next to the `.rpp`
  file (if it doesn't exist) and watches it. Drop `.mdd` / `.glb` files
  in there and they appear in the library.
- Per-project folder override via `Choose folder...`. The chosen path is
  saved into the `.rpp` (REAPER `SetProjExtState`), so it travels with
  the project. Open the same project on another machine and the override
  is preserved (assuming the path exists there too).
- `Reset to default folder` button shows up when an override is active.
  Clears the override and goes back to `<project>/Animations/`.
- Untitled-project hint: when no project is saved, the viewport shows a
  single centered "Save the REAPER project to enable HoloRoll" message.
  No 3D scene, no library/playback/camera/render UI sections — the user
  immediately understands the plugin is intentionally idle. The moment
  you save, the next OnTimer tick picks up the new path and creates
  `Animations/`.
- Project-change detection: switching between open projects, or closing
  the project entirely, repoints the library and watcher automatically.
  No restart needed.

### Global Incoming folder

A single well-known location that auto-routes files into the active
project. Engine bridges, scripts, or the user can target this one path
without knowing which REAPER project is currently open.

- Path: `%APPDATA%\REAPER\UserPlugins\HoloRollIncoming\`. Created
  automatically on plugin startup.
- Files dropped here (`.mdd` / `.glb` / `.obj`) are watched and
  auto-moved into the active project's `Animations/` folder on the next
  OnTimer tick after a 500ms debounce.
- Drain-on-project-change: when the user saves an Untitled project (or
  switches between projects), any files left over in Incoming/
  immediately move to the new project's Animations/. Files that arrived
  while the user had no saved project are not lost.
- Collision-renaming on move: if a file with the same name already
  exists in the project's `Animations/`, the incoming file is renamed
  with a `_<N>` suffix (`frog_jump_2.glb`, `frog_jump_3.glb`...). The
  variation-suffix matcher already strips these for playback, so the
  new file naturally becomes another variation of the same animation
  — no data loss, no overwrite.

### Drag-n-drop from Windows Explorer

Drag `.mdd` / `.glb` / `.obj` files from Explorer onto the HoloRoll 3D
viewport. Files land in the active project's Animations/ folder, with
the regular hot-reload modal following.

- OLE drop target on the viewport window. Anything else (audio, midi,
  video) is silently ignored — those are REAPER's domain.
- Files move directly into `<project>/Animations/` (skipping Incoming/
  for a faster path).
- Cross-disk drops work (`MoveFileExA` with `MOVEFILE_COPY_ALLOWED`).
- New module `src/extension/drop_target.{h,cpp}` isolates the OLE
  plumbing (`IDropTarget` impl, `RegisterDragDrop`, lifecycle) from
  `entry.cpp`.

### Drop-zone visual feedback

While a drag is in progress over the viewport, the scene dims, an 8-pixel
coloured border appears around the viewport, and a centered text plate
explains what will happen. Three states:

- **Green** — valid files + saved project. Plate text:
  *"Drop here to add to project"*.
- **Amber** — valid files + Untitled project. Plate text:
  *"Save the REAPER project first"*. Drop is rejected.
- **Red** — unrecognised file types. Plate text:
  *"Unsupported file type"*. Drop is rejected.

The overlay disappears the instant the cursor leaves the viewport or
the drop completes. Implemented via `std::atomic<bool>` flags published
by the OLE callbacks and read lock-free from the render path.

## Pipeline summary

```
engine bridge / user / script / Explorer drag
           |
           v
   HoloRollIncoming/    (global, well-known)         OR direct viewport drop
           |                                                |
           v  (auto-move on watcher event or project change) v  (immediate move)
   <project>/Animations/    <-------------------------------+
           |
           v  (project watcher, 500ms debounce)
   library scan -> hot-reload modal -> items at cursor on selected track
```

## Behaviour notes

- Saved project + drop: files move into `<project>/Animations/`,
  hot-reload picks them up within ~500 ms, modal appears.
- Untitled project + Explorer drop: drop is rejected with a console log
  message. Drop overlay shows amber state with the reason.
- Untitled project + Incoming/ drop: files stay in Incoming/ until you
  save a project. There is no auto-cleanup of stale Incoming files yet
  — if you decide not to save, you'll need to manually delete them
  from Incoming/.
- File already inside `<project>/Animations/` and dragged onto the
  viewport: silent no-op.
- Move failure (e.g. network drive, permissions): logged to console with
  `GetLastError`, file stays where it is.

## Removed / Changed

- **`animations_dir` config key removed.** v0.6.0 used a global config
  ini for the folder; this is gone. The folder is now strictly
  project-relative (or per-project override). Old config files keep
  loading; the orphan `animations_dir=` line is just ignored.
- The folder picker title was renamed to "Override animations folder
  for this project" to make the per-project nature obvious.
- Library section in overlay distinguishes default-vs-override folders
  with a small subtitle when an override is active.
- OLE is initialised at plugin startup. If REAPER has already done
  `OleInitialize` (the typical case), our call returns `S_FALSE` and
  we record "already init by other" so we don't unbalance the ref
  count on shutdown.

## Migration from v0.6.0

There is no automatic migration. v0.6.0 projects that relied on a global
animations folder will load with an empty library after upgrade. To
migrate:

1. Copy your animation files into `<project>/Animations/`, OR
2. Click `Choose folder...` and point it at your existing folder —
   this saves an override into the project, OR
3. Drop the files in `%APPDATA%\REAPER\UserPlugins\HoloRollIncoming\`
   and they'll auto-move into the current project.

## Not in this release

- **Drop directly onto the REAPER timeline** (arrange window) was
  considered but skipped. Reason: REAPER already registers its own OLE
  drop target on arrange for native audio / midi import; our drop target
  would either lose those drops or require a forwarding wrapper that's
  fragile across REAPER versions. The viewport drop + Incoming folder
  cover ~95% of real workflows without that risk.
- **Drag-detection on the REAPER main window** (highlighting the
  viewport when a drag enters REAPER but not yet the viewport itself)
  was prototyped but skipped. The Win32 OLE API only delivers DragEnter
  events to the window the cursor is over; system-wide hooks or
  registering competing drop targets on REAPER's own windows would
  break native REAPER drag-n-drop. Standard behaviour (overlay appears
  when the cursor crosses the viewport) matches Photoshop / Blender /
  OBS conventions.

## Internal

- New REAPER API bindings: `EnumProjects`, `SetProjExtState`,
  `GetProjExtState`.
- New globals `g_currentProjectPath` (sentinel-initialised so first
  OnTimer always triggers a project-changed handler),
  `g_currentAnimationsFolder` (cached resolved path), and
  `g_incomingWatcher` (second `FolderWatcher` instance).
- New helpers: `GetActiveProjectPath`, `DirOfPath`,
  `GetProjectAnimationsOverride`, `SetProjectAnimationsOverride`,
  `EnsureFolderExists` (recursive Win32 mkdir),
  `ResolveActiveAnimationsFolder`, `OnProjectChanged`,
  `ResetFolderToProjectDefault`, `GetIncomingFolder`,
  `MoveFileWithCollisionRename`, `DrainIncomingToProject`,
  `OnViewportFilesDropped`.
- `oleaut32` linked in CMake target.

## [0.6.0] — 2026-05-05

Items become the primary unit on the timeline. Regions are still created
(as decoration) but no longer drive playback resolution — the playhead
looks at items, top-down across tracks, and renders whichever animation
the top item references by name.

### Added
- **Items as the playback driver.** `OnTimer` now enumerates all items
  on every track, finds the one under the playhead (top track wins on
  multi-track overlap), reads its name (take's `P_NAME`, fallback
  item `P_NOTES`), and resolves it back to an animation in the
  library.
- **Variation suffix matching.** An item named `frog_jump_2` resolves
  to the `frog_jump` animation if no animation literally called
  `frog_jump_2` exists. The numeric suffix (`_<digits>`) is stripped
  and the lookup is retried. Direct matches always win to avoid
  surprising behavior with real files like `enemy_v2.glb`.
- **`Place all` action** creates an item + matching region for every
  loaded animation, in sequence, on the first track. Replaces the
  v0.5.x `Place regions` (which only placed regions). The button has
  been renamed in the overlay.
- **Hot-reload modal places items at cursor.** When new files appear
  in the watched folder, the "Place all" button creates items at the
  play cursor on the first selected track, each one placed after the
  previous, with a `region_gap_seconds` separator. Existing items are
  not touched.
- **Missing-animation warning.** If an item under the playhead names
  an animation that's no longer in the library (file deleted, folder
  changed), the overlay's Library section shows a red warning
  "⚠ Animation 'X' not found" instead of silently rendering the
  previous frame.
- New REAPER API bindings for item enumeration and creation:
  `AddMediaItemToTrack`, `AddTakeToMediaItem`, `SetMediaItemInfo_Value`,
  `GetSetMediaItemTakeInfo_String`, `GetSetMediaItemInfo_String`,
  `GetTrack`, `GetSelectedTrack`, `CountTracks`, `CountTrackMediaItems`,
  `GetTrackMediaItem`, `GetMediaItemInfo_Value`, `GetMediaItemTake`,
  `GetActiveTake`, `UpdateArrange`.

### Changed
- **Region-only projects no longer play.** v0.5.x projects relied on
  regions for resolution; this is gone. After upgrading, run
  `Place all` (or recreate items manually) to get items on the
  timeline. The CHANGELOG promised "backward compat with v0.5.x is
  not a goal" — this is the consequence.
- Regions created by `Place all` still get the same purple color and
  same name as the corresponding item, so the visual lookup is
  unchanged — they just don't drive playback anymore.
- Library section in overlay relabels region-related text as items:
  "Loaded N animation(s), M item(s) on timeline", "Item time: X..Y".

### Internal
- Spike helper `SpikeTestCreateItem` and its overlay button retained
  for ad-hoc API smoke-testing during development. Will be removed
  in v0.7.0 once the items workflow has settled.

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

[Unreleased]: https://github.com/Ilia-Smelkov/HoloRoll/compare/v0.12.0-alpha.3...HEAD
[0.12.0-alpha.3]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.3
[0.12.0-alpha.2]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.2
[0.12.0-alpha.1]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.1
[0.11.1]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.11.1
[0.11.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.11.0
[0.10.1]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.10.1
[0.10.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.10.0
[0.9.1]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.9.1
[0.9.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.9.0
[0.6.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.6.0
[0.5.1]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.5.1
[0.5.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.5.0
[0.4.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.4.0
[0.3.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.3.0
[0.2.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.2.0
[0.1.0]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.1.0
