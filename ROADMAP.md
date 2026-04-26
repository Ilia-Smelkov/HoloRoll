# Roadmap

This document tracks planned work for HoloRoll. Items are grouped by target
release. Each item carries a rough size estimate (S / M / L) and an
acceptance criterion describing how we know it's done.

The roadmap is opinionated, not contractual — order and scope may shift as
the project evolves.

## v0.2.0 — Render & Comfort

Tightening the visual pass and remembering more state across sessions.

### C-1. Persist viewport poses across project reopens — `M`
Camera orbit + object rotation + render mode are kept per animation in
memory today, but lost on REAPER restart. Move the pose store into REAPER's
project ext-state so reopening a project restores everything as the user
left it.
- *Acceptance:* close REAPER on an open project with custom camera angles,
  reopen, verify each animation's view matches what was saved.

### C-2. Basic grayscale material with directional light — `M`
Solid mode currently uses flat color tinting. Add a single fixed-function
directional light + ambient term so model silhouette and depth are readable
without textures. Keeps render path on `glLight*`, no shaders.
- *Acceptance:* Solid mode shows N·L shading; rotating the object visibly
  changes the lighting.

### C-3. Pre-roll: model appears N frames before region start — `S`
Show the rest pose a few frames *before* the region begins so the model
doesn't pop in suddenly.
- *Acceptance:* config key `pre_roll_frames=8`. When playhead is in
  `[region.start - preRoll/fps, region.start)`, render frame 0 of the
  upcoming animation. At `region.start`, normal playback resumes.

### C-4. Visual warning for stretched regions in overlay — `S`
If the user dragged a region past the animation length, today playback just
clamps to the last frame silently. Surface this in the overlay as a non-modal
warning icon next to the region info.
- *Acceptance:* dragging a region's right edge beyond `animation.duration`
  shows a `⚠` glyph and a tooltip explaining the clamp.

### C-5. UI presentation pass — `?` (decision pending)
Reminder to revisit visual style: custom font, palette, iconography. Stay
on ImGui (no platform UI rewrite) — this is a polish pass, not a tech pivot.
Concrete scope to be defined when we get there.

## v0.3.0 — Library Management

Make working with growing animation libraries painless.

### C-6. Folder watcher with new-file prompt — `M`
Detect when files appear in the configured folder and surface a non-blocking
banner: "3 new files detected. [Reload library]". Don't reload silently.
- *Acceptance:* dropping a new `.mdd` into the folder shows the prompt
  within ~1s. Clicking the button rescans without restarting REAPER.

### C-7. External hooks for new-file events — `L`
When the watcher (C-6) detects a new file, optionally invoke a user-defined
executable to process it. Designed as the integration entry point for engine
bridges (Unreal, Unity, etc.) that produce HoloRoll-ready files from native
formats.
- Config keys: `on_new_file_command`, `on_new_file_args`,
  `on_new_file_timeout_seconds`.
- The external tool is expected to drop its result back into the watched
  folder; the watcher picks it up on the next pass.
- *Acceptance:* HoloRoll launches the configured exe with `--input <path>`,
  logs its exit code in the overlay log pane, and reloads the library when
  the resulting file appears.

### C-8. Region manager table — `M`
Collapsible "Regions" section in the overlay listing every loaded animation
with: name, start, duration, animation linked, OBJ status. Per-row buttons:
`Go to` (place play cursor), `Refresh` (re-create from animation length).
- *Acceptance:* table updates live as regions are dragged; per-row actions
  work without going through the Action List.

### C-9. Region merge with foreign regions — `S`
Today `Place regions` only deletes regions matching our color/prefix, which
is good. Extend it: if a non-HoloRoll region overlaps a target slot, log a
warning and shift our region forward by one `region_gap_seconds` so we don't
overwrite the user's data.
- *Acceptance:* placing regions next to existing user regions doesn't
  silently consume them.

## v0.4.0 — Format support

Expand beyond MDD without bloating the plugin itself.

### C-10. Alembic / PC2 import via separate CLI tool — `L`
Stand-alone utility `holoroll-tools convert <in> --out <out.mdd>` ships
alongside the plugin. The plugin stays MDD-only — keeps the runtime small
and the conversion pipeline scriptable. PC2 first (simple, ~200 LOC),
Alembic later (heavier dependency).
- *Acceptance:* `.pc2` and `.abc` cache files convert cleanly to MDD that
  loads in HoloRoll without distortion.

## v0.5.0 and beyond — Polish

Pieces that need both v0.2 foundations and time to design properly.

### C-11. 3D scene with ground plane and lighting — `L`
Depends on C-2. Add a simple ground plane (with optional shadow), keep the
single directional light, do not regress frame time. Worth measuring on
larger meshes (50k+ triangles) before committing.

### C-12. Texture / material support — `L`
Real material pass: vertex shader + fragment shader path, diffuse texture,
normal map. Gated behind a perf-first design — must not slow down
animation-only workloads.

### C-13. UI presentation upgrade (full pass) — `L`
The full version of C-5 once we have a clearer feature set: theme, font,
icons, layout, Help affordances.

## Non-goals (for now)

- **macOS / Linux ports.** REAPER extension SDK supports them, but our
  Win32-specific OpenGL context, file dialogs, and config path logic don't.
  Cross-platform is a v1.0+ conversation.
- **Replacing ImGui with a native UI toolkit.** Native (Win32 / Qt) would
  cost weeks and gain very little for an in-DCC tool.
- **Bundling REAPER SDK as a Git submodule.** Keeping it vendored as plain
  files trades a slightly larger repo for predictable builds. We may revisit
  later.
