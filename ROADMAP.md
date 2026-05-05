# Roadmap

This document tracks planned work for HoloRoll. Items are grouped by target
release. Each item carries a rough size estimate (S / M / L) and an
acceptance criterion describing how we know it's done.

The roadmap is opinionated, not contractual — order and scope may shift as
the project evolves.

## Shipped

### v0.1.0 — MVP
First end-to-end loadable build. MDD point-cache parser, OBJ topology,
regions, OpenGL viewport, ImGui overlay, INI config.

### v0.2.0 — Camera & Comfort
Fly camera (RMB+WASD), rotation gizmo, configurable pivot, per-animation
pose memory, Inno Setup installer.

### v0.3.0 — Lit scene
Per-face Lambert in Solid mode, vertical sky gradient, infinite-feeling
ground grid (minor + major + world axes), 3/4 default camera framing
sized to bbox, persisted scene settings in `holoroll_config.ini`,
silent console by default, vertex-bounds crash fix.

---

## v0.4.0 — Library workflow

Two specific friction points in the current workflow: regions are named
`MDD: foo` (verbose), and adding new files requires manually re-scanning
the folder. This release fixes both.

### C-15. Drop the `MDD:` prefix from region names — `S`
Regions are currently named `MDD: <basename>`. Strip the prefix; basename
alone is enough. Preserves backward compat: `Place regions` still
recognises old `MDD:`-prefixed regions and updates them silently. The
constant `kRegionNamePrefix` in `AnimationLibrary` becomes empty by
default but stays exposed as a config key for users who want it back.
- *Acceptance:* fresh `Place regions` produces region names without the
  `MDD:` prefix. Old projects with `MDD: foo` regions still work
  (regions are matched by basename).
- *Files:* `core/animation_library.{h,cpp}`, `extension/entry.cpp`,
  `core/config_store` (new key `region_name_prefix`).

### C-16. Hot-reload watcher — `M`
Win32 `ReadDirectoryChangesW` running on a worker thread, posting
events to a thread-safe queue. Main thread (OnTimer) polls the queue,
debounces 500ms (so a burst of file copies appears as one batch), then:
1. AnimationLibrary rescans the folder.
2. Diff against the previously-loaded set.
3. **For genuinely new animations**: surface an ImGui modal —
   *"Found N new animations. Place regions for them after the last
   existing region?"* with `Place all` / `Skip` / `Cancel` buttons.
4. On `Place all`, regions are appended after `max(existing region.end)
   + region_gap_seconds`, in alphabetical order of basename.
5. **For removed/renamed files**: don't touch existing regions —
   leave a "missing source" warning in the overlay status. The user
   decides whether to delete the region.
- *Acceptance:* dropping 3 new `.mdd` files in the folder while REAPER
  is open triggers exactly one modal within ~1 second of the last drop.
  Accepting it appends 3 new regions at the end of the timeline,
  preserving all existing regions and their positions.
- *Files:* new `core/folder_watcher.{h,cpp}` (Win32-specific, isolated
  from cross-platform code), `extension/entry.cpp` (modal + diff
  logic), `core/animation_library` (Diff API), config key
  `hot_reload.enabled = 1`.

---

## v0.5.0 — `.glb` support

Single release covering both vertex animation and skeletal animation
in glTF binary files. Vendor `tinygltf` via FetchContent (header-only,
parses both JSON and binary buffers cleanly).

### C-17. `IAnimationSource` abstraction — `M`
Today `LoadedAnimation` directly holds `MDDDataManager*` and
`ObjIndexLoader*`. Introduce an interface with `VerticesForFrame(uint32_t)`,
`TriangleIndices()`, `TotalFrames()`, `RestNormals()`, etc. Refactor
existing MDD+OBJ pair into `MDDAnimationSource`. **Pure refactor — zero
behaviour change.** Done first to keep the GLB additions tractable.
- *Acceptance:* all existing MDD+OBJ behaviour is byte-identical, no
  visual or perf regression.

### C-18. `.glb` with morph targets (vertex animation) — `M`
The simpler of the two glTF animation modes. Load mesh + morph target
deltas, sample the animation channel that drives morph weights at the
current FPS, sum `base_position + Σ(weight_i * delta_i)` per frame, and
**bake** all frames once on load (same shape as MDD's per-frame array).
- Render-pipeline unchanged — Solid/Wireframe/Points/Lambert all just
  work because the output is the same `vector<float>` per frame.
- *Acceptance:* a Blender-exported `.glb` with shape-key animation loads
  and plays identically to its MDD-equivalent.
- *Files:* new `core/glb_morph_source.{h,cpp}`, `core/animation_library`
  (file-extension dispatch).

### C-19. `.glb` with skinning (skeletal animation) — `L`
The harder mode. Read joint hierarchy, inverse-bind matrices, animation
channels (translation/rotation/scale per joint, with LINEAR/STEP/CUBICSPLINE
interpolation modes), per-vertex `JOINTS_0` + `WEIGHTS_0` accessors.
**CPU skinning** — for each frame, compute joint world-matrices, then for
each vertex `Σ_i weight_i * (joint_matrix_i * inverse_bind_i * base_pos)`.
Bake all frames once on load.
- Linear-blend skinning only. Dual-quat / morph-+-skin combos are out of
  scope until someone needs them.
- Rest-pose normals from baked frame 0 (same as MDD pipeline).
- *Acceptance:* a Blender-exported skinned `.glb` (e.g. rigged character
  with armature) loads and plays correctly. Rotation/translation/scale
  channels all sample at the right time.
- *Files:* new `core/glb_skinned_source.{h,cpp}`, shared glTF
  parsing helpers in `core/glb_common.{h,cpp}`.

### C-20. File-format dispatch in folder scan — `S`
`AnimationLibrary::ScanFolder` recognises `.glb` alongside `.mdd`. For
`.glb`, no companion OBJ is needed (topology is inside the file).
Fallback: if both `foo.mdd` and `foo.glb` exist, prefer `.glb` and log
a warning. (Reasoning: `.glb` carries topology for free; MDD does not.)
- *Acceptance:* mixed folder with `.mdd`+`.obj` pairs and `.glb` files
  loads everything, regions are placed for all.

---

## v0.6.0+ — Polish (target dates uncommitted)

### C-3. Pre-roll: model appears N frames before region start — `S`
Show the rest pose a few frames before the region begins so the model
doesn't pop in suddenly.
- *Acceptance:* config key `pre_roll_frames=8`. When playhead is in
  `[region.start - preRoll/fps, region.start)`, render frame 0 of the
  upcoming animation. At `region.start`, normal playback resumes.

### C-4. Visual warning for stretched regions — `S`
If the user dragged a region past the animation length, today playback
just clamps to the last frame silently. Surface this in the overlay as
a non-modal warning glyph next to the region info.

### C-1. Persist viewport poses across project reopens — `M`
Camera orbit + object rotation + render mode are kept per animation in
memory today, but lost on REAPER restart. Move the pose store into
REAPER's project ext-state so reopening a project restores everything.
- *Acceptance:* close REAPER on an open project with custom camera
  angles, reopen, verify each animation's view matches what was saved.

### C-7. External hooks for new-file events — `L`
When the watcher (C-16) detects a new file, optionally invoke a
user-defined executable to process it. Designed as the integration entry
point for engine bridges (Unreal, Unity, etc.) that produce HoloRoll-
ready files from native formats.
- Config keys: `on_new_file_command`, `on_new_file_args`,
  `on_new_file_timeout_seconds`.
- The external tool is expected to drop its result back into the watched
  folder; the watcher picks it up on the next pass.

### C-8. Region manager table — `M`
Collapsible "Regions" section in the overlay listing every loaded
animation: name, start, duration, animation linked, source-file status.
Per-row buttons: `Go to` (place play cursor), `Refresh`
(re-create from animation length).

### C-9. Region merge with foreign regions — `S`
Today `Place regions` only deletes regions matching our color/prefix,
which is good. Extend it: if a non-HoloRoll region overlaps a target
slot, log a warning and shift our region forward by one
`region_gap_seconds` so we don't overwrite the user's data.

### C-10. PC2 / Alembic via standalone CLI tool — `L`
Stand-alone utility `holoroll-tools convert <in> --out <out.mdd>`
ships alongside the plugin. Plugin stays MDD/GLB-only — keeps the
runtime small and the conversion pipeline scriptable. PC2 first
(simple, ~200 LOC), Alembic later (heavier dependency).

### C-12. Texture / material support — `L`
Real material pass: vertex shader + fragment shader path, diffuse
texture, normal map. Gated behind a perf-first design — must not slow
down animation-only workloads.

### C-13. UI presentation upgrade — `L`
Full pass: custom theme, font, icons, layout, Help affordances.
Stays on ImGui — no platform UI rewrite.

---

## Non-goals (for now)

- **macOS / Linux ports.** REAPER extension SDK supports them, but our
  Win32-specific OpenGL context, file dialogs, folder watcher, and
  config path logic don't. Cross-platform is a v1.0+ conversation.
- **Replacing ImGui with a native UI toolkit.** Native (Win32 / Qt)
  would cost weeks and gain very little for an in-DCC tool.
- **Bundling REAPER SDK as a Git submodule.** Keeping it vendored as
  plain files trades a slightly larger repo for predictable builds.
- **GPU skinning for `.glb`.** CPU skinning is fast enough for typical
  character rigs (<100 joints, <50k vertices); we bake frames anyway,
  so per-frame cost is amortised. Revisit only if real-world files
  push frame time over budget.
