# Roadmap

This document tracks planned work for HoloRoll. Items are grouped by target
release. Each item carries a rough size estimate (S / M / L) and an
acceptance criterion describing how we know it's done.

The roadmap is opinionated, not contractual — order and scope may shift as
the project evolves. For shipped-version detail, see `CHANGELOG.md`.

## Shipped

### v0.1.0 — MVP (2026-04-26)
First end-to-end loadable build. MDD point-cache parser, OBJ topology,
regions, OpenGL viewport, ImGui overlay, INI config.

### v0.2.0 — Camera & Comfort (2026-04-28)
Fly camera (RMB+WASD), rotation gizmo, configurable pivot, per-animation
pose memory, Inno Setup installer.

### v0.3.0 — Lit scene (2026-04-28)
Per-face Lambert in Solid mode, vertical sky gradient, infinite-feeling
ground grid, 3/4 default camera framing sized to bbox, persisted scene
settings, silent console by default, vertex-bounds crash fix.

### v0.4.0 — Library workflow (2026-05-05)
Region prefix dropped from default name. Hot-reload watcher
(`ReadDirectoryChangesW`) detects new files in the animations folder and
prompts to place them.

### v0.5.0 / v0.5.1 — `.glb` support (2026-05-05)
Vertex morph targets and skeletal animation in glTF binary files. CPU
linear-blend skinning, baked frames at load time. v0.5.1 patched a
tinygltf interaction with embedded textures (CesiumMan etc.).

### v0.6.0 — Items as the playback driver (2026-05-05)
Items (not regions) drive playback. Variation suffix matching
(`frog_jump_2` → `frog_jump`). `Place all` creates one item + region per
animation; hot-reload modal places at cursor.

### v0.9.0 — Project-relative folders + global Incoming (2026-05-06)
`<project>/Animations/` is the new default folder, follows whichever
REAPER project is active. Global `%APPDATA%\REAPER\UserPlugins\HoloRollIncoming\`
auto-routes files into the active project. Drag-n-drop from Explorer
onto the viewport. Project-change detection. (v0.7.0 / v0.8.0 were
internal milestones rolled into 0.9.0.)

### v0.9.1 — Track on top (2026-05-07)
`Place all` and friends create items on a fresh top track instead of
mixing with the user's existing tracks.

### v0.10.0 / v0.10.1 — Scale awareness (2026-05-07)
Models recentered onto the grid origin (XZ), 2-metre stick-figure
reference toggle, viewport bbox readout. v0.10.1 scoped the animations
folder per-project (`Animations/<project_name>/`) so two `.rpp` files in
the same directory don't share a library.

### v0.11.0 / v0.11.1 — Placement workflow polish (2026-05-07)
Variations count, de-dup on `Place all`, hold-frame buffers (pre/post
roll), region overhang. v0.11.1 reshaped the geometry: items are now
exactly animation-length, pre/post-roll act as global playback buffers
(not baked into items).

---

## v0.12.0 — Motion analysis & per-bone envelopes (in progress)

The headline feature of v0.12: every placed item gets per-bone motion
envelopes on a dedicated `HoloRoll` track. Sound designers can drive
synth params, gain rides, sends, etc. directly from bone activity
without writing a line of automation by hand.

### Shipped (alpha.1 → alpha.8)

- **alpha.1** (2026-05-07): per-joint motion-magnitude curves baked at
  load time using a probe-point trick `(0,1,0,1)` transformed through
  the joint's world matrix. Top-N active bones logged on scan.
- **alpha.2** (2026-05-07): local motion alongside world motion (probe
  in parent-local space — captures "this bone moved on its own" vs
  "carried by parent").
- **alpha.3** (2026-05-07): `holoroll_motion.jsfx` — silent 16-slider
  JSFX placeholder bundled with the installer, hosts the per-bone
  envelopes on a track's FX chain.
- **alpha.4** (2026-05-08): auto-insert the JSFX onto a dedicated
  `HoloRoll Motion` track when the user hits a "Setup motion track"
  button. Idempotent.
- **alpha.5** (2026-05-08): auto envelope generation. Every Place all /
  hot-reload / +Place writes top-3 world-motion envelopes on sliders
  1..3 for each placed item. Manual setup button retired.
- **alpha.6** (2026-05-08): single-track layout. Items + JSFX +
  envelopes all live on one persistent `HoloRoll` track instead of
  the alpha.5 split (top track for items, bottom track for JSFX).
  Shared-peak normalisation across the item's selected bones (was
  per-bone — visually indistinguishable curves).
- **alpha.7** (2026-05-08): envelopes appear on the track immediately
  (state-chunk `VIS` flag rewrite + REAPER redraw trigger). Min-max
  stretching with 5/95 percentile bounds for usable dynamic range.
  Frame-0 forward-fill workaround for the bake artifact.
- **alpha.8** (2026-05-08): motion metric switched from rectified
  `|speed|` to **signed projection** along each joint's principal motion
  axis. Sinusoidal bone oscillation now produces one sine wave on the
  envelope (was two bumps per swing). Forward-fill workaround retired
  — frame 0 is valid signed data, not a bake artifact.
- **alpha.9** (2026-05-08): pluggable motion-event detector framework
  (`IMotionEventDetector` + registry). First detector
  `RigidMechanismDetector` produces 5 event types (start / peak_hi /
  peak_lo / zero_cross / end) with hysteresis-gated activity
  detection. New "Generate motion markers" overlay button walks all
  placed items, runs the detector on top-1 bone, writes REAPER
  project markers as `<itemname>:<eventtype>`. Surgical clear on
  re-run. Pattern follows Unreal's Animation Modifiers (offline asset
  bake), inspired by Epic's `FootstepAnimEventsModifier` and
  community `FootSyncMarkers` modifier.
- **alpha.10** (2026-05-08): placement simplification — auto-region
  creation removed, variations count removed, "new animations
  detected" modal removed (placement is now automatic on watcher
  events). One item per animation, no decoration regions.
- **alpha.11** (2026-05-08): WAAPI-style TCP socket bridge for
  external command senders (`127.0.0.1:58271`, line-delimited JSON).
  Six verbs: `ping`, `get_selection`, `get_regions`, `clear_regions`,
  `create_regions`, `run_script`. Worker thread + main-thread
  `Tick()` to keep REAPER C API calls on the main thread. New
  helpers in `src/extension/socket_server.{h,cpp}` — independent of
  the rest of HoloRoll's domain logic (transport layer only).

### Pending for v0.12.0 final

#### M-1. Bone selection UI in overlay — `M`
Auto-pick of top-3 world bones works but leaves no manual override.
Sound designers often want a specific bone (jaw, finger, foot) on a
specific slider, even if it isn't the most active. The overlay needs:
- A per-animation list of joints with checkboxes (`worldMotion[j]`'s
  `sum(|v|)` ranking shown next to each name).
- A "manual mode" switch that overrides the auto top-N pick.
- Persisted per-animation in REAPER project ext-state so selections
  travel with the project.
- *Acceptance:* in a multi-bone rig, a designer can lock slider 1 to
  the jaw bone (instead of the most-active spine), close + reopen
  REAPER, and the slider 1 → jaw mapping survives.

#### M-2. World/local motion toggle — `S`
`localMotion[j][f]` is computed and stored but never surfaced. Add an
overlay switch "World / Local" that picks which motion source feeds
the envelopes. Local is what you want for "this bone moved on its
own" — useful when a parent carries a child around and you're trying
to find the exact moment the child *acts*.
- *Acceptance:* switching to Local on a rig where parent+child swing
  together produces a non-zero parent envelope but a near-zero child
  envelope (and vice versa for World).

#### M-3. Sliders 4..16 in use — `S`
Currently we burn slider 1..3 for top-3 world bones and reserve the
rest. Once M-1 and M-2 land, expose:
- Slider 1..3: top-3 selected bones (world OR local per M-2).
- Slider 4..6: same bones but in the OTHER motion source (e.g., if
  M-2 is set to World, sliders 4..6 mirror Local for the same bones).
- Slider 7..16: free for additional manually-assigned bones.
- *Acceptance:* the JSFX shows useful, distinct envelope shapes on
  all 6 first sliders for a rig where world ≠ local motion.

#### M-4. Validation on real-world rigs — `M`
Everything was tested on `RiggedSimple` (1 active bone). We need to
confirm the pipeline holds up on:
- A Mixamo character (50+ joints, full-body anim).
- A face rig (small-amplitude motion, many sub-bones).
- A vehicle / mechanical rig (translation, not just rotation).
- A multi-axis joint (something with X+Y+Z motion to stress the
  principal-axis projection).
- *Acceptance:* on each of the four, the envelopes look plausible
  to a sound designer (not flat, not pegged at extremes, sign matches
  visible motion direction).

#### M-5. Diagnostic-log cleanup — `S`
The few `[holoroll]` console messages that survived alpha.7 / alpha.8
clean-up need a final pass. Goal: console stays silent on success
paths, only fires on missing APIs / failed FX-chain inserts / failed
chunk writes.
- *Acceptance:* a clean `Place all` on a healthy project produces
  exactly one summary line (`[holoroll] placed N items+regions`).

#### M-6. Optional: forward-PCA for multi-axis trajectories — `M`
The alpha.8 max-deviation reference picks ONE axis. For trajectories
that are essentially planar (e.g., a hand drawing a circle in front
of the body), one axis throws away half the motion. A 2D PCA — find
the two principal directions, take magnitude of the projection onto
the plane — would handle that. Defer until a real-world animation
demands it; max-deviation is good enough for hinge-like bones.
- *Acceptance:* a circular or figure-eight bone trajectory produces a
  visually-credible envelope (not an arbitrary axis).

---

## v0.13.0 — Marker generation extensions

The MVP path (`RigidMechanismDetector` + button) shipped in alpha.9.
Next milestones extend the detector library and the UX around it:

#### M-13.1. Footstep detector — `M`
Second concrete `IMotionEventDetector`. Algorithm follows Epic's
`FootstepAnimEventsModifier`: detect local minima of foot-bone speed,
optionally gated by ground proximity. Tuned for biped locomotion
(walk / run / sprint cycles); separate from the rigid-mechanism
detector because the underlying signal we want is the |velocity|
metric (alpha.7) rather than the signed projection (alpha.8).
- *Acceptance:* Mixamo walk loop produces alternating `:foot_strike_l`
  / `:foot_strike_r` markers at footstep frames.

#### M-13.2. Detector dropdown in overlay — `S`
Once two+ detectors exist, replace the hardcoded default with a
dropdown next to the "Generate motion markers" button. Selection
persists in `holoroll_config.ini`.

#### M-13.3. Per-bone marker generation — `M`
Generate markers from top-2 and top-3 bones (currently top-1 only).
Marker names include the bone identifier so multiple bone streams
don't collide. Useful for animations where two bones move
independently and each produces meaningful events.
- *Acceptance:* on a hand-and-door rig, `door_handle` and
  `door_body` produce two separate marker streams that don't overlap
  in name space.

#### M-13.4. Manual threshold tuning UI — `M`
Inline fields in the overlay for the active detector's parameters
(enter threshold %, exit threshold %, smoothing window, min
separation). Live re-run on parameter change. Persisted per-detector
in config.

#### M-13.5. Marker travels with item — `L`
Markers currently strand themselves when the user moves an item.
Investigate auto-resync via either:
- (a) Re-run on every project change tick (cheap, but noisy in undo
  history).
- (b) Store events as item P_EXT and rebuild markers on every
  `Place all` / hot-reload pass (cleaner but couples item lifecycle
  to marker lifecycle).
Pick after testing on real workflows.

---

## v0.14.0+ — Engine bridges (uncommitted target dates)

#### M-14.1. Unreal Engine bridge — `L`
Standalone Python / Editor-script tool inside Unreal that exports
selected `USkeletalMeshComponent` animations to `.glb` and drops them
into HoloRoll's Incoming folder. Closes the loop: animator picks an
anim in Unreal, hits "Send to HoloRoll", sound designer is editing
in REAPER seconds later.

#### M-14.2. Unity Engine bridge — `L`
Same idea, Editor-script form. Lower priority unless someone asks.

---

## v1.0+ candidates (pulled from earlier roadmap, still relevant)

These were planned earlier but kept slipping because v0.12 / v0.13
got priority. Not formally targeted to a release yet.

#### C-1. Persist viewport poses in REAPER project ext-state — `M`
Camera orbit + object rotation + render mode survive REAPER restarts.

#### C-7. External hooks for new-file events — `L`
On new file detected, optionally invoke a user-defined executable.
Becomes redundant once M-14.1 / M-14.2 land but cheap to do
generically first.

#### C-8. Region manager table — `M`
Collapsible "Regions" section in the overlay: name, start, duration,
animation linked, source-file status. Per-row `Go to` / `Refresh`
buttons.

#### C-9. Region merge with foreign regions — `S`
Don't overwrite non-HoloRoll regions during placement. Shift our
region forward by `region_gap_seconds` if there's a collision.

#### C-10. PC2 / Alembic via standalone CLI tool — `L`
`holoroll-tools convert <in> --out <out.mdd>`. PC2 first, Alembic
later.

#### C-12. Texture / material support — `L`
Diffuse + normal map. Gated behind perf — must not slow animation-only
workloads.

#### C-13. UI presentation upgrade — `L`
Custom theme, font, icons, layout, Help. Stays on ImGui.

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
- **Auto-update / in-app installer.** HoloRoll itself doesn't manage
  upgrades; the Inno installer is the canonical install path. Engine
  bridges may bundle the installer and trigger a silent install on
  the user's behalf, but that orchestration lives in the bridge tool,
  not in the plugin.
