# Changelog

All notable changes to HoloRoll are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.14.0-alpha.1] — 2026-05-08

Start of the 0.14.0 cycle. Single feature this release:
`build_regions` now polls REAPER's item table until each new item is
visibly on the timeline before reading its geometry. One verb call =
"placed, observed, wrapped" (or honestly skipped); no more silent
race where the region lands at the requested-but-not-final position.

### Added — new arg
- **`item_wait_s`** (default `2.0`) — per-unit polling budget in
  seconds. For each unit we:
  1. Snapshot `CountTrackMediaItems(track)` BEFORE creation.
  2. Call `CreateNamedItemWithRolls(...)`.
  3. Write motion envelopes (sync).
  4. Poll every 75 ms until BOTH
     - `CountTrackMediaItems` has incremented, AND
     - an item with `name == anim` is findable on the track
       (pointer match preferred when duplicates exist).
  5. Once found, read `D_POSITION` / `D_LENGTH` off THAT item and
     build the region.
- If the budget expires without the item showing up, the unit lands
  in `skipped` with reason
  `"item did not appear on timeline within item_wait_s seconds"`,
  the region is NOT created, and `pos` does NOT advance — the next
  unit gets the same slot, no gap in the sequence.

### Why
- Caller reported regions occasionally drifting away from where the
  item actually landed. Root cause hypothesis (per their spec):
  REAPER's item-table updates can lag a tick or two behind the
  `AddMediaItemToTrack` return, especially with `PreventUIRefresh`
  active and motion-envelope writes happening in the same critical
  section. Polling closes the race.
- Spec is explicit that this must happen inside a SINGLE
  `build_regions` call — retrying from the caller side would
  double-place items and leave orphan regions.

### Failure modes preserved
- `CreateNamedItemWithRolls` returning null is no longer surfaced as
  a separate skip reason — null pointer just means polling falls
  back to enumeration-by-name. If neither path finds an item within
  the budget, the timeout reason fires instead.
- If `CountTrackMediaItems` / `GetTrackMediaItem` aren't available
  in the resolved API (very old REAPER), we trust the pointer
  from `CreateNamedItemWithRolls` and skip the poll. Best effort
  for ancient builds.


## [0.13.0-alpha.4] — 2026-05-08

CI was silently mis-tagging every alpha release. Fixed, plus the
running build now shows its version in the overlay so you can
confirm at a glance.

### Fixed
- **Release workflow accepted only clean semver tags.** The regex
  in `.github/workflows/release.yml` was `^v(\d+\.\d+\.\d+)$` —
  matched `v0.13.0` but NOT `v0.13.0-alpha.3`. Every alpha tag
  fell through to the CMakeLists fallback, which derived
  `version=0.13.0`, tagged the GitHub Release as `v0.13.0`, and
  named the installer `HoloRoll-Setup-0.13.0.exe`. Each new
  alpha push silently OVERWROTE the previous Release on the same
  tag, and the in-app updater always saw "v0.13.0" as the latest
  available version regardless of how many alphas had been
  published. Hence "не последнюю версию ставит" — it was
  re-installing the same `0.13.0` installer every time.

  alpha.4 widens the regex to
  `^v(\d+\.\d+\.\d+(?:-[\w.]+)?)$`, so `v0.13.0-alpha.3` matches
  and the release pipeline preserves the full version through to
  the installer filename, the GitHub Release tag, and the
  updater-visible `tag_name`.

- **`VersionInfoVersion` no longer rejects pre-release tails.**
  Windows' file-version metadata is strict X.Y.Z[.W] — Inno Setup
  fails if `MyAppVersion` has a `-alpha.N` suffix and you feed it
  straight to `VersionInfoVersion`. alpha.4 introduces a separate
  `MyFileVersion` define (defaults to `MyAppVersion` for backward
  compat with local builds). The release workflow now derives it
  by stripping the pre-release tail and passes both to ISCC:
  `MyAppVersion=0.13.0-alpha.4`, `MyFileVersion=0.13.0`.

### Added
- **Version label in the HoloRoll overlay.** A small disabled-text
  line at the bottom of the overlay window reads
  `HoloRoll v0.13.0-alpha.4`. Single source: `HOLOROLL_VERSION_STRING`
  in `src/extension/version.h`. Gives the user a way to verify
  that an update actually took effect (vs. the watchdog silently
  failing).

### Migration
- Previous alpha tags (`v0.13.0-alpha.1`, `.2`, `.3`) on GitHub
  are stuck pointing at the old `v0.13.0` release. The next push
  to `v0.13.0-alpha.4` will create a NEW, properly-tagged Release.
  If you want to clean up, manually delete the stranded
  `v0.13.0` Release from the Releases page; the tag itself can
  stay as a marker.
- After this fix lands, the updater's `releases/latest` lookup
  will return whatever the most recent alpha tag is (alpha.4
  onward). Until alpha.4 is published, the updater keeps seeing
  the old broken `v0.13.0` and will appear stuck on it.


## [0.13.0-alpha.3] — 2026-05-08

New socket verb `build_regions`. Item-anchored region creation:
caller hands us a list of `{anim, name}` pairs, we place each item
using the existing animation→media mapping, then build the region
around the item's ACTUAL position and length — not the values we
asked for. Geometry sticks to real items.

### Added — new bridge verb
- **`build_regions`** — args:
  ```
  { "units": [ {"anim":"...", "name":"..."}, ... ],
    "region_pad": <float>,
    "gap": <float>,
    "start_mode": "after_last" | "cursor",
    "clear_existing": <bool> }
  ```
  Returns:
  ```
  { "created": [ {"name","start","end"}, ... ],
    "skipped": [ {"anim","reason"}, ... ],
    "count": <int> }
  ```
  Logic:
  1. Starting `pos` = `GetCursorPosition()` if `start_mode="cursor"`,
     else end of last region across the project (0 if none). Computed
     BEFORE the clear step.
  2. If `clear_existing`: wipe ALL regions (`DeleteAllRegions`).
     Markers are untouched.
  3. For each unit in order:
     - resolve animation by name via the same path playback uses
       (`ResolveAnimationByItemName` — handles variation suffixes);
     - place the media item at `pos` (same logic as the overlay
       Place all button: track find-or-create + `CreateNamedItemWith
       Rolls` + motion envelope write);
     - if the item doesn't get created, record `{anim, reason}` in
       skipped and continue — NO region for this unit;
     - read `D_POSITION` and `D_LENGTH` back from REAPER, build the
       region around those values + `region_pad`;
     - `pos = ip + il + region_pad + gap`.
  4. Whole batch wrapped in `Undo_BeginBlock` / `PreventUIRefresh`
     so it lands as one undo entry and doesn't re-paint between
     items.

### Internal
- New `nlohmann::json holoroll_build_regions(const nlohmann::json&)`
  at file scope in `entry.cpp`. Lives there because the
  implementation needs `g_lib`, `EnsureItemsTrackAndFx`,
  `CreateNamedItemWithRolls`, `WriteMotionEnvelopesForItem`,
  `FindLastRegionEnd`, `GetFps`, `DeleteAllRegions` — all in
  entry.cpp's anonymous namespace. Surfacing each through a shim
  would be more code than just including `json.hpp` and writing
  the verb here.
- `socket_server.cpp` declares the function via `extern` and
  forwards. The error convention is a top-level `_error` field in
  the returned JSON; `HandleBuildRegions` unpacks it into a
  `VerbError` so error replies match every other verb's shape.
- New helper `DeleteAllRegions()` in `entry.cpp`'s anon namespace.
  Counterpart to `DeleteOurRegions()`, but doesn't filter by
  colour/prefix.

### Design notes
- **Region pad is geometry, not buffer.** It extends the region past
  the item end by exactly `region_pad` seconds — useful for tails
  or hover-zones. Pre/post-roll playback buffers (from alpha.11.x)
  are a SEPARATE feature; they don't affect what `build_regions`
  produces.
- **Per-unit failure is non-fatal.** Skipping a bad anim doesn't
  abort the batch. The caller gets back exactly which units failed
  and why, and the surviving items are already placed.
- **Item name == anim basename.** Region name is whatever the
  caller chose. The two are decoupled on purpose: item-name has to
  match HoloRoll's playback resolution; region-name is free-form
  user-facing text.

### Deprecation note
- The earlier `create_regions` verb (alpha.11) is **no longer the
  recommended path** for region creation tied to item geometry —
  use `build_regions` instead. `create_regions` stays in place
  for the explicit `{start, end}` use case (raw region marker
  insertion without any item).


## [0.13.0-alpha.2] — 2026-05-08

Auto-updater gets the two missing pieces from alpha.1: periodic
re-poll and a manual "Check for updates" button. No more reliance on
plugin reload to discover new releases.

### Added
- **Periodic re-poll** in `updater::Tick()`. Every OnTimer tick we
  check elapsed time since `update.last_check_unix`; if 24 hours
  have passed and no worker is currently in flight, kick off a
  background re-poll. Steady-state cost is one atomic load + one
  double compare per tick — cheap.
- **Manual "Check for updates" button** in the overlay's Config
  section, next to "Open config" / "Reload config". Calls
  `updater::CheckNow()`, which bypasses the 24-hour cooldown but
  still respects the `update.enabled` master toggle and the
  in-flight guard. Tooltip explains the cadence.

### Changed
- **Worker spawn refactored** to support being called more than
  once per session. Replaced the alpha.1 `g_workerStarted` /
  `g_workerDone` pair with a single `g_workerActive` atomic;
  `g_sessionStarted` becomes a one-shot guard for the
  hydration-from-config block in `Start()`. New internal
  `LaunchWorker()` helper joins the previous (finished) thread
  before spawning a new one — no leaked std::thread objects across
  re-poll cycles.

### Known limitations (carried over from alpha.1)
- Re-poll cadence is hardcoded at 24 hours. Could be
  `update.check_interval_hours` if we ever need finer control.
- A failed network round-trip resets nothing — next Tick (a moment
  later) will try again. Acceptable noise for now since failures
  are logged only when debug is on. Could add exponential backoff
  if it bites.
- Manual "Check" button doesn't surface progress while the worker
  is running; the banner just appears (or doesn't) a few seconds
  later. Visible progress would need a separate UI state.


## [0.13.0-alpha.1] — 2026-05-08

Plugin starts auto-updating itself from GitHub Releases. New versions
land silently the next time the user closes REAPER — no manual
download, no dialogs interrupting work.

### Added
- **In-app auto-updater** (`src/extension/updater.{h,cpp}`). On plugin
  load, a background worker thread:
  1. WinHTTP `GET https://api.github.com/repos/Ilia-Smelkov/HoloRoll/releases/latest`.
  2. Parses the JSON, finds the `HoloRoll-Setup-*.exe` asset, compares
     its tag to `HOLOROLL_VERSION_STRING` (compiled into the DLL).
  3. If newer: streams the installer into
     `%APPDATA%\REAPER\UserPlugins\HoloRollUpdates\` and persists
     pointer + version in the config.
  Worker then exits. State is queried lock-free by the UI banner.
- **Update banner** at the top of the overlay window:
  `Update available: <installed> -> <available>` plus a status line
  ("Will install silently when you close REAPER.") and a
  `[Dismiss for this version]` button. Banner hides immediately on
  dismiss and won't reappear until a NEWER release is published.
- **Silent install on REAPER close**: when the plugin is being
  unloaded (`Stop()` path), if there's a ready installer and the
  user hasn't disabled `update.auto_install_on_close`, we spawn a
  detached PowerShell watchdog:
  ```
  Start-Sleep -Seconds 2
  while reaper.exe still running (max 120s) sleep 500ms
  Start-Process <installer> /VERYSILENT /SUPPRESSMSGBOXES /NORESTART -Wait
  ```
  The DLL gets unloaded a moment later, REAPER closes, watchdog
  sees `reaper.exe` gone, installer fires silently. Next REAPER
  start = new HoloRoll. No user action between detect and install
  beyond "close REAPER like you normally would".
- **`HOLOROLL_VERSION_STRING`** constant in
  `src/extension/version.h` — single source of truth for the
  currently-built version. Must be bumped manually when tagging
  new releases.
- **SemVer-ish version comparator** in `updater.cpp` that handles
  `MAJOR.MINOR.PATCH[-pre.N]` correctly: any pre-release sorts
  below the clean release of the same x.y.z; numeric pre-release
  components compare numerically.

### Added — config keys
- `update.enabled` (default `1`) — master toggle. `0` disables the
  background poll entirely.
- `update.auto_install_on_close` (default `1`) — when `0`, the
  watchdog isn't spawned even if there's a pending installer.
  Status text in the banner switches to "Auto-install disabled;
  run the installer manually."
- `update.pending_installer_path`, `update.pending_version`,
  `update.dismissed_version`, `update.last_check_unix` — internal
  state, persisted so the banner can re-appear immediately on the
  next plugin load without waiting for a network round-trip.

### Added — CMake
- New source `src/extension/updater.cpp` compiled into the plugin.
- New link library `winhttp` (Windows ships it; nothing to install).

### Threading model
- The worker thread is created in `Start()` and `join()`-ed in
  `Stop()` — no detached threads holding REAPER state hostage.
- The watchdog is the only detached process; it lives independently
  of our DLL and outlives it intentionally.
- All getters (`HasReadyUpdate`, `AvailableVersion`, etc.) take a
  short lock and return — safe to call every render frame.

### Known limitations / follow-ups
- **Polling fires once per plugin load**, not on a periodic timer.
  Means: if you leave REAPER open for days, you won't see new
  releases until the next start. Adequate for alpha; will revisit
  with `update.check_interval_hours` if it bites.
- **No prerelease channel toggle.** The current release.yml tags
  alphas as `prerelease: false`, so `/releases/latest` returns them
  too. When we go stable, alphas should be marked as prerelease and
  the updater needs a channel switch (`update.channel = stable |
  prerelease | both`).
- **`HOLOROLL_VERSION_STRING` is hand-edited.** Easy to forget.
  Long-term fix is injection from CMake via `git describe --tags`
  but that needs git available at build time, which is true on CI
  but might not be locally. Deferred.
- **PowerShell required.** Ships with Windows but some hardened
  environments lock it down. If we hit that, fallback would be
  shipping our own tiny watchdog `.exe`.
- **Antivirus may flag** a downloaded `.exe` being executed
  detached from a "plugin". Code-signing the installer would
  reduce friction; not done yet.
- **No retry on failed download.** If the network drops mid-
  download, the partial file is deleted and we wait for the next
  plugin load. No exponential backoff or progress UI.

### Migration
- First time the plugin loads with this version, it will hit
  GitHub once. Users on offline machines see one log line
  ("no network or GitHub unreachable; will retry on next plugin
  start.") in the debug log and nothing else — no banner, no
  background activity.
- Set `update.enabled=0` in `holoroll_config.ini` to opt out
  entirely. Set `update.auto_install_on_close=0` to keep checking
  but never auto-install (banner becomes informational only).


## [0.12.0-alpha.15] — 2026-05-08

Socket bridge gets four more verbs to make external scripts feel
first-class: register-without-run, shortcut introspection, shortcut
assignment via REAPER's own dialog, and edit-cursor query.

### Added — 4 new bridge verbs (`src/extension/socket_server.cpp`)

- **`register_action`** — `{"path":"<absolute .lua>"}` →
  `{"command_id":"_RS<hash>"}`. Calls
  `AddRemoveReaScript(true, 0, path, true)` and resolves the named
  command id via `ReverseNamedCommandLookup`. Idempotent: re-
  registering the same path returns the same id. Does NOT run the
  script — just makes it visible in REAPER's action list with a
  stable id the caller can refer to later.
- **`script_shortcut`** — `{"path":"<...>.lua"}` →
  `{"shortcut":"<text or empty>","command_id":"_RS<hash>"}`. Registers
  if needed, then `CountActionShortcuts(main, cmd)` →
  `GetActionShortcutDesc(main, cmd, 0, ...)` returns the first bound
  shortcut text (e.g. `"Ctrl+Shift+T"`); empty if no shortcut bound.
  Only the FIRST shortcut is surfaced — REAPER allows multiple
  bindings per action but the common case is one.
- **`assign_shortcut`** — `{"path":"<...>.lua"}` →
  `{"opened":true}`. Registers if needed, then issues
  `Main_OnCommand(40605, 0)` ("Show action list"). The user
  finishes the assignment from REAPER's native dialog — conflict
  detection and shortcut removal handled there. We deliberately
  don't try to pre-select / pre-filter the row; REAPER's filter box
  takes plain text. (Optional "copy script name to clipboard"
  refinement deferred.)
- **`get_cursor`** — `{}` → `{"position":<float seconds>}`. Reads
  the edit cursor via `GetCursorPosition()` (falls back to
  `GetCursorPositionEx(nullptr)` on builds that only export the
  Ex variant).

### Added — REAPER API bindings
- `ReverseNamedCommandLookup`, `NamedCommandLookup` — convert
  between numeric command ids and `"<extension>_<action>"` named
  strings. We use the reverse direction in `register_action` /
  `script_shortcut` to surface the named id callers store.
- `GetCursorPosition` — for `get_cursor`.
- `SectionFromUniqueID`, `CountActionShortcuts`,
  `GetActionShortcutDesc` — for `script_shortcut`. Main keyboard
  section is `SectionFromUniqueID(0)`.

### Design notes
- **No `reaper-kb.ini` writing.** REAPER exposes no API for
  programmatic shortcut assignment; the keybindings file is its
  internal format. `assign_shortcut` is the canonical answer —
  open the dialog, let REAPER handle conflict resolution. A
  hypothetical "delete shortcut" verb is also unnecessary for the
  same reason: the user removes shortcuts from the same dialog.
- **Action ID `40605` ("Show action list")** is hardcoded. Verified
  against REAPER 6.x / 7.x as of writing; if a future REAPER
  renumbers it, this verb stops working and we need to update
  the constant. (No stable named-command equivalent exists for
  this native action.)
- All four verbs run on REAPER's main thread (via `Tick()`
  drain in `OnTimer`) — same rule as the alpha.11 verbs, every
  REAPER C API call lives on the main thread.

### Client integration
- External apps can now: register a script and remember its
  `_RS<hash>` id; periodically poll the shortcut text to keep their
  own UI in sync; open the REAPER dialog when the user wants to
  bind / rebind / clear a shortcut; query the edit cursor for
  marker / item placement without having to also poll the
  transport.


## [0.12.0-alpha.14] — 2026-05-08

HoloRoll now works on Untitled (never-saved) REAPER projects. The
"save the project first" architectural restriction from v0.7/v0.8 is
gone — replaced with a per-user default fallback folder.

### Added
- **Default animations folder for Untitled projects.** New third-priority
  fallback in `ResolveActiveAnimationsFolder()`. When the project is
  Untitled (no override, no .rpp on disk), HoloRoll uses
  `%APPDATA%\REAPER\UserPlugins\HoloRollDefault\`. Folder is created
  lazily on first use.
- **New config key `default_animations_folder`** (default empty →
  resolves to the path above). User can edit `holoroll_config.ini`
  to point the fallback somewhere else, e.g. a shared scratch drive.
- **Overlay banner** "(default folder - project not saved yet)" in
  yellow under the folder path when the fallback is active, so it's
  clear why files might not land next to a .rpp.

### Removed
- "Save the REAPER project to enable HoloRoll" centered message in
  the viewport. Untitled projects now render the normal scene + UI.
- The amber drop-rejection state "Save the REAPER project first"
  (the drop overlay's amber colour stays as a defensive fallback,
  but the user-facing text is now "Drops are not currently
  accepted" — it should be unreachable in normal operation).
- The Library-section if/else branch that previously hid all library
  controls on Untitled projects.

### Behaviour on project state transitions
- **Open REAPER + Untitled project** → default folder used; drops,
  Incoming/ drain, Place all all work.
- **Save Untitled → `MyProject.rpp`** → next OnTimer tick switches
  to `<project_dir>/Animations/MyProject/`. Library rescans new
  folder. Files that accumulated in the default folder STAY there —
  no auto-migration. (Manual migration UI is a possible follow-up.)
- **Open already-saved project** → behaves as before (project-relative
  folder, never touches the default).

### Rationale / history
- The Untitled-blocks-everything model came in v0.7.0/v0.8.0 (rolled
  into v0.9.0) when we switched from a global `animations_dir` config
  to per-project folders. At the time it made sense: if there's no
  .rpp, there's no place to put files. But the constraint hurt
  experimentation, auto-py integrations, and the new socket bridge
  (alpha.11) where external apps drive HoloRoll without caring about
  project state. The fallback folder fixes the symptom without
  giving up the project-relative-by-default design (saved projects
  still get their own isolated folders, like before).


## [0.12.0-alpha.13] — 2026-05-08

REAPER console stays clean by default. Debug output is opt-in via a
single checkbox.

### Added
- **Runtime debug-log toggle.** New checkbox "Debug log" in the
  overlay's Config section. When OFF (default), every `[holoroll] /
  [holoroll-bridge] / [holoroll-debug]` line is suppressed —
  including placement summaries, socket-bridge listening status,
  hot-reload diagnostics, and the alpha.7..alpha.9 motion-debug
  traces that we left in production code. When ON, all those lines
  fire as before.
- New config key `debug.enabled` (default `0`). Persisted in
  `holoroll_config.ini`, round-tripped through the viewport
  checkbox.

### Changed
- `ConsoleLog` and `SpikeLog` in `entry.cpp` now both check the
  runtime `g_debugEnabled` atomic instead of a hardcoded
  `kVerboseLog=false`. Through alpha.12, `SpikeLog` was always-on
  (used for "the user MUST see this" paths like the v0.6 spike test
  and the alpha.7 envelope-visibility debug). alpha.13 collapses
  both into the same gate — if you want to see something, turn the
  flag on.
- Turning the flag ON also auto-pops REAPER's console window via
  the existing `ForceShowReaperConsole()` helper. Turning it OFF
  leaves the console as-is (it may have other plugins' output you
  want to keep).

### Removed
- `kVerboseLog` build-time constant.

### Migration
- Headless setups / CI that scraped REAPER console for
  `[holoroll] placed ...` lines need to either: (a) set
  `debug.enabled = 1` in `holoroll_config.ini` before running, or
  (b) tick the overlay checkbox once and let it persist. Recommended:
  scrape post-state from REAPER project (items + regions) instead
  of console output.


## [0.12.0-alpha.12] — 2026-05-08

Installer policy tweak: stop force-killing REAPER in silent mode.

### Changed
- **Headless installer no longer touches REAPER.** Through alpha.11
  the silent path of `holoroll.iss` ran `taskkill /F /IM reaper.exe`
  automatically when REAPER was open — convenient for unattended
  bootstrap scripts but destructive (force-kill drops unsaved work
  with no warning). alpha.12 splits the policy:
  - **Silent / very-silent mode** (`/SILENT`, `/VERYSILENT`): do
    nothing. Files copy as-is. The caller is assumed to be either
    (a) a first-install bootstrapper running on a clean machine
    where REAPER hasn't been opened yet, or (b) an in-app
    auto-updater (planned) that handles the locked-DLL problem on
    its own. If REAPER happens to be running and files are locked,
    Inno's standard "file in use" handling fires and setup may
    return a non-zero exit code.
  - **UI mode** (interactive double-click): unchanged. Still prompts
    "Close REAPER?" and offers to taskkill on confirmation.

### Migration
- Scripts that called `HoloRoll-Setup-x.y.z.exe /VERYSILENT` while
  REAPER was running need updating: either close REAPER before
  invoking the installer (cleanest), or accept that the install may
  partially fail when files are locked. Exit code 3 no longer fires
  in silent mode — callers should rely on file-not-replaced
  detection (verify DLL version post-install) if they need to be
  sure.

### Rationale
- The destructive default mismatched the actual use case. The
  bridge / auto-py / future auto-updater flows all already have a
  "close REAPER" prompt in the calling app — letting the installer
  ALSO kill REAPER doubled the surface area for "user lost work"
  bugs without buying anything.


## [0.12.0-alpha.11] — 2026-05-08

WAAPI-style TCP socket bridge for external command senders. Inspired
by Wwise's WAAPI / Reaper Web Interface — a Python (or any) app can
now connect locally, send a JSON command, get back a JSON reply, and
control HoloRoll/REAPER from outside.

### Added
- **TCP socket bridge** on `127.0.0.1:58271` (override via
  `REAPERBRIDGE_PORT` env var). Line-delimited JSON:
  - request:  `{"method":"<verb>","args":{...}}\n`
  - reply OK: `{"ok":true,"result":{...}}\n`
  - reply err:`{"ok":false,"error":"..."}\n`
  - One connection = one command. Client sends, reads, closes.
- **Threading model:** worker thread runs the accept loop; per-
  connection threads parse and queue requests; a `Tick()` call on
  REAPER's main thread (wired into `OnTimer`) drains the queue and
  runs handlers — every REAPER C API call happens on the main
  thread, as REAPER requires.
- **Six implemented verbs:**
  - `ping` → `{}` — liveness check.
  - `get_selection` → `{items: [{name, position, length}, ...]}` for
    every selected media item; MIDI takes skipped.
  - `get_regions` → `{regions: [{name, start, end, index}, ...]}`
    from `EnumProjectMarkers3` with `isrgn=true`.
  - `clear_regions` → deletes all regions, returns `{deleted: N}`.
  - `create_regions` (args: `{regions: [{name,start,end},...],
    reposition: [{index,position},...]}`) → under
    `Undo_BeginBlock`/`PreventUIRefresh(1)`: reposition selected
    items by index, create regions; returns `{created: N}`.
  - `run_script` (args: `{path: "<absolute .lua>"}`) →
    `AddRemoveReaScript(true, 0, path, true)` then `Main_OnCommand`.
    Returns `{ran: true}` or an error.

### Internal
- New `src/extension/socket_server.{h,cpp}`. Public surface:
  `socket_server::Start()` / `Stop()` / `Tick()`.
- New REAPER API bindings: `GetSelectedMediaItem`,
  `CountSelectedMediaItems`, `GetTakeName`, `TakeIsMIDI`,
  `Undo_BeginBlock`, `Undo_EndBlock`, `PreventUIRefresh`,
  `AddRemoveReaScript`.
- CMake: new source compiled, link `ws2_32` for Winsock.
- JSON via `nlohmann/json` (already on the include path through
  tinygltf's bundled `json.hpp`); no extra FetchContent needed.

### Design rationale
- HoloRoll does NOT manage a user-script library; the script-loader
  app on the other side of the socket tells HoloRoll which `.lua`
  to run. Keeps this extension a pure transport layer.
- Loopback-only bind (`INADDR_LOOPBACK`) — no remote access.
- 30-second per-connection timeout: misbehaving clients can't park
  a worker thread forever.
- `SO_REUSEADDR` set on the listener so plugin reloads can rebind
  immediately instead of waiting for TIME_WAIT.
- `Tick()` runs in `OnTimer` regardless of viewport state — the
  bridge is decoupled from the UI lifecycle.

### Known limitations
- Per-connection threads are spawned, not pooled. For the expected
  workload (occasional external commands), this is fine. If a real
  high-throughput case emerges, a small thread pool with bounded
  queue would be the next step.
- No authentication. Loopback-only mitigates remote attack; local
  users on the same machine have full access to the bridge. Same
  trust boundary as Reaper's own Web Interface.


## [0.12.0-alpha.10] — 2026-05-08

Placement workflow simplification. Three friction points dropped:
auto-created regions, variation count, and the "new animations detected"
confirmation modal.

### Removed
- **Auto-creation of regions alongside items.** Placement (Place all /
  hot-reload / +Place) no longer calls `AddProjectMarker2(isrgn=true)`.
  Items still appear on the HoloRoll track exactly as before; the
  paired purple region is gone. The "Region overhang" placement option
  is removed from the overlay since it had no surviving consumer.
  - Legacy regions from earlier versions are still cleaned up on the
    first Place all run after upgrade (`DeleteOurRegions()` still runs
    once at the start) so users don't end up with orphaned content.
- **Variations count.** The "Variations" inline field is removed.
  Placement always creates one item per animation. `MakeVariationName`
  utility is kept (still used by suffix-stripping resolution) but no
  longer called from placement paths.
- **"New animations detected" modal.** When the folder watcher reports
  new files, placement now happens automatically — no popup, no
  Place all / Skip prompt. The detection log line still appears in
  the REAPER console.

### Changed
- `GlViewport::SetPlacementOptions` / `GetPlacementOptions` signatures
  reduced to `(preRollSec, postRollSec)` — variations and region
  overhang parameters removed.
- New helper `FindLastHoloRollItemEnd()` (in `entry.cpp`) replaces
  `FindLastRegionEnd()` as the anchor for "where does the next item
  go". Since we don't create regions anymore, region-end was no
  longer a usable proxy. The old `FindLastRegionEnd` is kept for
  legacy compatibility (and as a building block of `DeleteOurRegions`).

### Migration
- Existing projects with HoloRoll regions get them wiped on the first
  Place all run after upgrade. Items remain untouched. If you've been
  relying on the regions for grouping, this is a breaking change —
  recreate them manually via REAPER's region tools.
- Config keys `placement.variations` and `placement.region_overhang_seconds`
  are no longer read or written. Existing values in `holoroll_config.ini`
  are silently ignored; nothing to migrate.

### Internal
- `pendingNewAnimations` / `newAnimationsChoice` fields and
  `DrawNewAnimationsModal()` helper kept as dead code for now,
  documented as removable. Avoids touching too many API consumers in
  one release; final cleanup later.


## [0.12.0-alpha.9] — 2026-05-08

Motion data starts driving REAPER markers — first user-facing payoff of
the alpha.1..alpha.8 analysis pipeline.

### Added
- **Pluggable motion-event detector framework.** New module
  `src/core/motion_events.{h,cpp}` defines `IMotionEventDetector` and a
  registry. Detectors transform per-frame motion curves (alpha.8 signed
  projection) into timestamped events: `start`, `peak_hi`, `peak_lo`,
  `zero_cross`, `end`. The framework is engine-agnostic: no REAPER, no
  UI dependencies — pure float-in / events-out, trivially testable.
- **First detector: `RigidMechanismDetector`.** Tuned for purposeful
  mechanical motion — doors, levers, drawers, switches. Algorithm:
  - Hysteresis-gated activity detection: enter at 10% of bone peak,
    exit at 5%, with a 3-frame end-confirmation window.
  - Local extrema (peak_hi / peak_lo) inside active phases — three-
    point sign-change of differences.
  - Zero crossings (signed motion through trajectory mean) anywhere
    on the curve.
  - 3-frame moving-average smoothing kills single-frame noise without
    shifting extrema by more than ~1 frame.
  - 3-frame minimum separation between events of the same type
    (debounce).
- **"Generate motion markers" button** in the Library section of the
  overlay. Walks every placed HoloRoll item, runs the active detector
  on its top-1 active world-motion bone, and writes REAPER project
  markers at the detected events. Markers are named
  `<itemname>:<eventtype>` (e.g. `door_open:start`,
  `door_open:peak_hi`, `door_open:zero_cross`). Re-running surgically
  clears prior HoloRoll markers in each item's range and rewrites
  them — non-HoloRoll markers anywhere on the timeline are untouched.
- **Console summary** on each generation: number of markers, items
  processed, items skipped (no resolvable animation), and which
  detector ran.

### Internal
- New CMake source `src/core/motion_events.cpp`.
- New `OverlayRequests.generateMotionMarkers` flag, dispatched in
  `OnTimer`.
- `EnumProjectItems` (existing) is now also the input for marker
  generation, mirroring how `WriteMotionEnvelopesForItem` consumes
  `LoadedAnimation::worldMotion`.

### Inspirations / non-reinvention
- The detector contract mirrors how Unreal Engine's Animation
  Modifiers (e.g. `FootstepAnimEventsModifier`) bake event tracks
  offline from per-frame bone data. We borrowed the registry +
  pluggable-algorithm pattern, the hysteresis-gated activity
  detection from Epic's footstep modifier, and the zero-crossing-of-
  signed-signal idea from gportelli's `FootSyncMarkers` community
  modifier.

### Known limitations
- Top-1 bone only (one stream of markers per item). Top-N marker
  generation lands in alpha.10.
- Hardcoded thresholds tuned for hinge-style motion at 24-60 fps.
  Footsteps, full-body locomotion, very small movements will need a
  separate detector — the framework is now in place to add them.
- Algorithm choice is hardcoded to `rigid_mechanism`. UI dropdown
  arrives with the second detector (alpha.10+).
- Markers don't move with their items — moving an item on the
  timeline strands its markers at their original times. Re-run the
  button to re-sync.


## [0.12.0-alpha.8] — 2026-05-08

Fundamental change to what motion analysis actually MEASURES.

### Changed
- **Motion metric: |speed| → signed principal-axis projection.**
  Through alpha.7 we computed the Euclidean magnitude of the per-frame
  probe-tip displacement — i.e. instantaneous |speed|. For sinusoidal
  bone oscillation (rotate to angle, rotate back), |speed| is a
  rectified `|cos(ωt)|`: TWO bumps per actual oscillation, with sharp
  zeros at the turnaround points. Visually it looked like "two
  semicircles per swing" instead of the single sine wave the user
  expected.

  alpha.8 stores per-joint probe trajectories during the bake loop,
  then post-processes each joint:
  1. Compute the mean probe position across all frames.
  2. Find the frame whose deviation from mean has the largest
     magnitude — its direction becomes the principal motion axis (a
     cheap stand-in for the dominant eigenvector of the covariance
     matrix; full PCA would be overkill for the typical 1D-ish
     joint motions we see).
  3. For each frame, the SIGNED projection of (position − mean) onto
     that axis becomes the new `worldMotion[j][f]` /
     `localMotion[j][f]` value.

  The signal preserves the underlying motion's frequency: one
  sinusoidal swing produces one sine wave on the envelope. Rest pose
  ≈ 0 (or close to it; offset by however asymmetric the motion is
  around its mean).

### Fixed (knock-on)
- **Frame 0 is now valid data.** Through alpha.7 the bake loop wrote
  0 at frame 0 because there was no "previous" frame to diff against;
  alpha.6/.7 papered over this with a forward-fill workaround that
  often fell back to motion[1]=0 anyway. With the alpha.8 metric
  there is no diff — frame 0's projection is just as valid as any
  other frame, so the workaround is gone.

### Affected APIs
- `worldMotion_` / `localMotion_` semantics (signed instead of
  non-negative). Direct readers should expect negative values.
- `TopNActiveBones`: now ranks by sum-of-absolutes instead of raw
  sum, since signed values cancel for symmetric oscillation.

### Known follow-ups (not in this release)
- "0 = rest pose" semantic is approximate when motion is asymmetric
  around its mean. If a bone holds at one extreme for a long time,
  its mean shifts toward that extreme and rest position projects to
  a non-zero value. Acceptable for now; revisit if it bites on
  complex animations.
- Forward fall-back when the trajectory is essentially planar with
  two equally significant axes (e.g. an XY circular motion). The
  max-deviation reference picks one axis and projects the other to
  zero. A full 2D PCA + magnitude metric would handle that, if/when
  it becomes a real-world need.


## [0.12.0-alpha.7] — 2026-05-08

Polish-the-polish on motion envelopes after the alpha.6 build. Two
fixes from the first hands-on test:

### Fixed
- **Envelopes now appear on the track immediately.** alpha.6 wrote the
  envelope data correctly but its `VIS` flag stayed at 0, so the lane
  didn't render until the user manually opened the FX window (which
  triggered REAPER's auto-show). alpha.7 reads the envelope's state
  chunk after writing points, replaces the `VIS X Y Z` line with
  `VIS 1 1 1.0`, and writes it back. The motion lane is visible the
  moment items get placed.
- **Envelope values now span the full slider range.** alpha.6's
  shared-peak normalisation (motion / sharedMax) clustered values
  near 1.0 when motion was relatively uniform across the animation —
  the curve looked flat and the slider's lower half was wasted. alpha.7
  switches to min-max stretching: `(motion - sharedMin) / (sharedMax -
  sharedMin)`. Slider 1 (most-active bone) hits both 0 and 1 at the
  bone's actual valleys and peaks; sliders 2 and 3 stay proportional
  using the same scale. Frame 0 is excluded from min/max computation
  (it's a 0 from the bake loop, not a real motion sample) and is
  forward-filled at write time.

### Added
- **Two new REAPER API bindings** (`reaper_api.h` /
  `reaper_bridge.cpp`): `GetEnvelopeStateChunk` and
  `SetEnvelopeStateChunk`, used by the new `EnsureEnvelopeVisible`
  helper to flip the visibility flag. Buffer sized at 64 KB which is
  comfortably above what our envelopes need.

### Changed (signature)
- `WriteMotionEnvelopeForBone(...)`: replaced the single
  `peakNormalizer` parameter with `sharedMin, sharedMax` for min-max
  stretching. Caller now computes both bounds in
  `WriteMotionEnvelopesForItem`.

### Known limitations
- "0 = no motion" semantic is gone: a slider value of 0 now means
  "minimum motion among the selected bones for this item", not "this
  bone is stationary". On simple rigs (RiggedSimple) this is fine; on
  complex rigs where the least-active selected bone never fully
  stops, slider lanes never reach the bottom. Trade we explicitly
  picked for visualisation clarity — re-evaluate if it bites.


## [0.12.0-alpha.6] — 2026-05-08

Polish pass on the alpha.5 motion-envelope plumbing. Three fixes after
first hands-on testing:

### Changed
- **Single track for items + JSFX + envelopes.** alpha.5 created two
  tracks per placement: a fresh top track for items and a separate
  "HoloRoll Motion" track at the bottom for the JSFX and envelopes.
  alpha.6 collapses both into a single persistent track named
  "HoloRoll" — items, JSFX and motion envelopes all live on the same
  row. Subsequent placements (Place all / hot-reload / +Place) find
  the existing track by name and append, instead of stacking new
  tracks.
- **Shared-peak normalisation across the three sliders.** alpha.5
  normalised each bone to its own peak, which made every slider stretch
  to ~1.0 regardless of how active the bone was — visually identical
  curves, no information conveyed about relative magnitude. alpha.6
  computes one shared peak across all three selected bones for the
  item: slider 1 (most-active bone) hits 1.0 at its peak; sliders 2
  and 3 are visibly smaller in proportion to their own activity.
- **Frame 0 is now forward-filled with frame 1's value.** The bake
  loop legitimately writes 0 at frame 0 (no previous frame to diff
  against), but the alpha.5 envelope rendered that as a fake drop at
  the left edge of every item. alpha.6 substitutes motion[1] for the
  first sample so the curve starts smoothly.

### Removed
- `EnsureMotionTrack`, `SetupMotionTrack`, `kMotionTrackName`,
  `ClearAllMotionEnvelopes` — the bottom "HoloRoll Motion" track is
  gone, so all the helpers that managed it are gone too.
- `setupMotionTrack` field on `OverlayRequests` and the corresponding
  dead dispatch block in `OnTimer` — there is no longer a manual
  "Setup motion track" button (motion track creation is automatic and
  collapsed into items track).
- Console debug noise: `SpikeLog` for "inserted JSFX using name" and
  "motion track ready" — both fired on success during placement and
  cluttered the REAPER console. Errors and the final "placed N items"
  summary remain.
- `ForceShowReaperConsole()` after the library scan log: it popped
  the console open on every project load / hot-reload, which was
  intrusive. The log itself still appears if the user has the console
  open.

### Renamed
- `EnsureMotionTrackAndFx` → `EnsureItemsTrackAndFx`.

### Removed (signature change)
- `WriteMotionEnvelopeForBone`'s `clearStart`, `clearEnd` parameters
  collapsed into surgical [item.start, item.end] internally; gained
  `peakNormalizer` (shared across the item's bones).
- `WriteMotionEnvelopesForItem`'s `clearFullEnvelope` parameter
  removed entirely (always surgical now).

### Note for users
- If you have an old "HoloRoll Motion" track at the bottom of your
  project from alpha.5 testing, alpha.6 will not touch it — you can
  delete it manually. New placements will create/reuse the single
  "HoloRoll" track at the top.


## [0.12.0-alpha.5] — 2026-05-08

Motion data finally lands on the timeline as REAPER native envelopes!
Auto-create on every placement path: "Place all", hot-reload modal, and
"+ Place" now all materialise envelopes for the placed items, top-3
world-motion bones each, on sliders 1..3 of the holoroll_motion JSFX.

No "Setup motion track" button anymore — motion track + JSFX appear
automatically when items appear. Cleaner UX, fewer steps.

### Added
- **Envelope-writing REAPER API bindings** (`reaper_api.h` /
  `reaper_bridge.cpp`):
  - `GetFXEnvelope(track, fxIdx, paramIdx, create)` — get-or-create the
    envelope for an FX parameter.
  - `InsertEnvelopePoint(env, time, value, shape, tension, selected,
    noSortInOptional)` — single-point write. We pass `noSortIn=&true`
    during batch writes for O(N) speed.
  - `Envelope_SortPoints(env)` — final sort after batch.
  - `DeleteEnvelopePointRange(env, t0, t1)` — wipe a time range before
    re-writing.
  - `TrackFX_GetNumParams(track, fx)` — paramIdx validation.
- **Motion envelope helpers in `entry.cpp`**:
  - `EnsureMotionTrackAndFx()` — wraps EnsureMotionTrack +
    EnsureMotionFx, returns `{track, fxIdx}` or failure.
  - `TopNActiveBones(motion, N)` — partial-sort top-N most-active bone
    indices, filters out zero-motion bones.
  - `WriteMotionEnvelopeForBone(track, fxIdx, paramIdx, motion,
    itemStart, fps, clearStart, clearEnd)` — normalises against this
    bone's peak, writes one envelope point per frame, sorts at end.
  - `WriteMotionEnvelopesForItem(...)` — top-3 dispatch for one item.
  - `ClearAllMotionEnvelopes(track, fxIdx)` — full wipe of sliders 1..N
    before "Place all" (which re-creates every item).
- **Auto-integration into all three placement paths**:
  - `PlaceOurItemsAndRegions` ("Place all"): full clear, then write
    envelopes for every placed item.
  - `PlacePendingAtCursor` (hot-reload modal): surgical clear, write
    envelopes only for new items — existing envelopes untouched.
  - `PlaceSingleAtCursor` ("+ Place" button): surgical clear for the
    one item, append to existing envelopes.

### Changed
- **"Setup motion track" button removed** from the overlay. Motion track
  + JSFX are now created automatically on first placement; no separate
  user action required. `setupMotionTrack` field on `OverlayRequests`
  and `SetupMotionTrack()` helper kept for now in case we want a manual
  re-trigger later, but they're no longer wired to any UI.

### Slider semantics
- **slider 1 = top-1 most-active world-motion bone of THIS item**.
- **slider 2 = top-2** of THIS item.
- **slider 3 = top-3** of THIS item.
- Different items map different bones onto the same slider; that's by
  design — "slider 1" always means "the most-active bone in whatever's
  playing right now".
- Sliders 4..16 reserved for future uses (local motion in alpha.6,
  manual selection in v0.13+).
- Each bone is normalised to [0,1] independently against its own peak,
  so every envelope spans the full slider range — visually comparable
  across items even when raw motion magnitudes differ.

### Cleanup strategy
- **"Place all"** = full clear (`DeleteEnvelopePointRange(env, -1e9, 1e9)`)
  before placement. Items are recreated wholesale, so stale envelopes
  from a previous Place all would otherwise pile up.
- **Hot-reload / +Place** = surgical clear (`[itemStart, itemEnd]`).
  Existing items keep their envelopes; only the new item's range gets
  wiped + re-written.

### Note for users
- Motion envelopes appear on the "HoloRoll Motion" track at the bottom
  of the track list. Click the track header to expand the FX chain;
  there you'll see `JS: HoloRoll Motion` with sliders Bone 1..16.
- Right-click any slider → Show track envelope to bring its envelope
  lane into view in the main timeline.
- Edit envelope curves freely — HoloRoll only re-writes when you
  re-place items via Place all / hot-reload modal / + Place.


## [0.12.0-alpha.4] — 2026-05-08

First C++ wiring for motion envelopes: HoloRoll can now create a dedicated
"HoloRoll Motion" track and auto-insert the holoroll_motion JSFX on it.
No envelope generation yet — alpha.5 will fill in motion data.

### Added
- **Track FX REAPER API bindings.** `TrackFX_AddByName`,
  `TrackFX_GetCount`, `TrackFX_GetFXName`, `GetSetMediaTrackInfo_String`
  (resolved in `reaper_bridge.cpp`, exposed via `ReaperApi` struct).
- **Track helpers in `entry.cpp`.**
  - `ReadTrackName(track)` — read P_NAME via GetSetMediaTrackInfo_String.
  - `FindTrackByName(name)` — walk the project, find by exact-match name.
  - `EnsureMotionTrack()` — find "HoloRoll Motion" or create at the
    bottom of the track list. Idempotent.
  - `EnsureMotionFx(track)` — insert holoroll_motion JSFX with
    instantiate=-1 (add-or-find). Returns FX index, or -1 on failure.
  - `SetupMotionTrack()` — user-facing entry point. Logs success or a
    helpful failure message (e.g. "REAPER may need to rescan FX").
- **"Setup motion track" button in overlay**, under a new
  "v0.12.0-alpha.4: motion envelope groundwork" section. Hovering shows
  a tooltip explaining what the button does. Console is force-shown on
  click so the success/failure log is immediately visible.

### Design notes
- **Motion track at the bottom of the track list, not top.** The top
  track is reserved for animation items (`EnsureTrackOnTop()`). Putting
  motion at the bottom keeps the two visually separated and out of the
  way until the user wants to look at it.
- **Idempotent setup.** Pressing "Setup motion track" multiple times is
  safe — if the track already exists, it's reused; if the JSFX is
  already on it, the existing FX index is returned. No stacked copies.
- **Case-sensitive track-name match.** A stray manual rename (e.g.
  "HoloRoll motion" lowercase) won't silently match — the user gets a
  fresh correctly-named track. Better than fighting over capitalisation.
- **No envelope generation in this release.** Confirmed scope: alpha.4
  is just the plumbing (track + JSFX). alpha.5 adds REAPER envelope API
  bindings (`GetFXEnvelope`, `InsertEnvelopePoint`,
  `Envelope_SortPoints`, `DeleteEnvelopePointRangeEx`) and the actual
  motion-curve writing. Splitting the work this way means a failure in
  alpha.4 ("button does nothing") is unambiguous — it's a track/FX
  issue, not a math/envelope issue.

### Note for users
- If "Setup motion track" reports `could not insert holoroll_motion
  JSFX`, REAPER hasn't picked up the JSFX yet. Either restart REAPER
  or run `Options → Preferences → Plug-ins → Re-scan`. After that
  the button works.


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

[Unreleased]: https://github.com/Ilia-Smelkov/HoloRoll/compare/v0.14.0-alpha.1...HEAD
[0.14.0-alpha.1]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.14.0-alpha.1
[0.13.0-alpha.4]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.13.0-alpha.4
[0.13.0-alpha.3]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.13.0-alpha.3
[0.13.0-alpha.2]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.13.0-alpha.2
[0.13.0-alpha.1]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.13.0-alpha.1
[0.12.0-alpha.15]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.15
[0.12.0-alpha.14]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.14
[0.12.0-alpha.13]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.13
[0.12.0-alpha.12]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.12
[0.12.0-alpha.11]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.11
[0.12.0-alpha.10]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.10
[0.12.0-alpha.9]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.9
[0.12.0-alpha.8]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.8
[0.12.0-alpha.7]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.7
[0.12.0-alpha.6]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.6
[0.12.0-alpha.5]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.5
[0.12.0-alpha.4]: https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.12.0-alpha.4
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
