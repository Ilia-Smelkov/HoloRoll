// windows.h MUST come before shellapi.h / shlobj.h.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "core/animation_library.h"
#include "core/motion_events.h"
#include "extension/socket_server.h"
#include "extension/updater.h"

// v0.13.0-alpha.3: build_regions verb's payload is JSON-shaped, easier
// to keep the implementation here (where all placement internals live)
// than to surface a dozen shims through socket_server. json.hpp is
// already on the include path through tinygltf — no extra dependency.
#include "json.hpp"
#include "core/config_store.h"
#include "core/folder_watcher.h"
#include "core/viewport_poses.h"
#include "extension/drop_target.h"
#include "extension/folder_picker.h"
#include "extension/reaper_bridge.h"
#include "render/gl_viewport.h"

#include "reaper_plugin.h"

namespace {
ReaperBridge g_bridge;
AnimationLibrary g_lib;
ConfigStore g_config;
GlViewport g_viewport;
ViewportPoses g_poses;
FolderWatcher g_watcher;          // Watches the active project's Animations/.
FolderWatcher g_incomingWatcher;  // Watches the global Incoming/ folder.
reaper_plugin_info_t* g_rec = nullptr;
HMODULE g_dllHandle = nullptr;

// v0.12.0-alpha.13: runtime debug flag. When false (default), ConsoleLog
// and SpikeLog silently no-op — the REAPER console stays quiet. Toggling
// the "Debug log" checkbox in the overlay's Config section flips this
// flag and persists `debug.enabled` to holoroll_config.ini. Atomic
// because the socket bridge logs from worker threads.
std::atomic<bool> g_debugEnabled{false};

constexpr std::size_t kNoActiveAnim = std::numeric_limits<std::size_t>::max();
std::size_t g_activeAnimIdx = kNoActiveAnim;

// Hot-reload state.
// `g_pendingNewAnimations` is populated by ProcessWatcherEvents() once the
// watcher's burst of events has settled (see kWatcherDebounceMs). It's read
// by OnTimer when constructing OverlayStatus, and cleared after the user
// picks Place all / Skip in the modal.
std::vector<std::string> g_pendingNewAnimations;
ULONGLONG g_lastWatcherEventTick = 0;
bool g_watcherEventsAccumulating = false;

// Same shape but for g_incomingWatcher — separate state because the
// incoming folder operates independently of the project folder.
ULONGLONG g_lastIncomingEventTick = 0;
bool g_incomingEventsAccumulating = false;

constexpr ULONGLONG kWatcherDebounceMs = 500;

int g_toggleViewportCommandId = 0;
int g_chooseFolderCommandId = 0;
int g_placeRegionsCommandId = 0;
int g_openConfigCommandId = 0;
int g_reloadConfigCommandId = 0;
gaccel_register_t g_toggleViewportAction{};
gaccel_register_t g_chooseFolderAction{};
gaccel_register_t g_placeRegionsAction{};
gaccel_register_t g_openConfigAction{};
gaccel_register_t g_reloadConfigAction{};

constexpr double kDefaultFps = 24.0;
constexpr double kDefaultGapSeconds = 1.0;

constexpr char kExtStateSection[] = "holoroll";
// Project ext-state keys (saved inside .rpp via SetProjExtState).
constexpr char kProjExtStateAnimDirOverride[] = "animations_dir_override";
constexpr char kConfigFileName[] = "holoroll_config.ini";

constexpr char kCfgKeyFps[] = "fps";
constexpr char kCfgKeyGap[] = "region_gap_seconds";
constexpr char kCfgKeyRegionPrefix[] = "region_name_prefix";
constexpr char kCfgKeyHotReload[] = "hot_reload.enabled";
// Scene settings (ground plane). Persisted between sessions.
constexpr char kCfgKeySceneShowGround[] = "scene.show_ground_plane";
constexpr char kCfgKeySceneRadius[]     = "scene.ground_radius";
constexpr char kCfgKeySceneGridStep[]   = "scene.grid_step";
// v0.10.0 scale-awareness toggles. All default off-by-default for clean
// install except bbox dimensions and grid labels which are visually subtle
// enough that turning them on by default helps new users orient.
constexpr char kCfgKeySceneShowBboxDims[]   = "scene.show_bbox_dimensions";
constexpr char kCfgKeySceneShowGridLabels[] = "scene.show_grid_labels";
constexpr char kCfgKeySceneShowRefHuman[]   = "scene.show_reference_human";

// v0.11.0 placement options. Persisted in holoroll_config.ini and read
// once on viewport open + on Reload config. The user sets them via inline
// fields next to "Place all".
// alpha.10: kCfgKeyPlacementVariations and kCfgKeyPlacementRegionOverhang
// constants removed along with the corresponding UI. Stale values in the
// user's config file are ignored (no reader, no writer).
constexpr char kCfgKeyPlacementPreRoll[]       = "placement.pre_roll_seconds";
constexpr char kCfgKeyPlacementPostRoll[]      = "placement.post_roll_seconds";

// v0.12.0-alpha.13: runtime debug-log toggle persistence.
constexpr char kCfgKeyDebugEnabled[]           = "debug.enabled";

// v0.12.0-alpha.14: fallback animations folder used when the active
// REAPER project is Untitled (never saved). Resolves third in the
// chain after the project-level override and the project-relative
// folder. Default location is created lazily on first use.
constexpr char kCfgKeyDefaultAnimationsFolder[] = "default_animations_folder";

// v0.13.0-alpha.1: in-app auto-updater persistence. See
// src/extension/updater.cpp for what each key means; entry.cpp only
// seeds defaults via EnsureConfigDefaults and exposes the config store
// to updater via holoroll_config_ref().
constexpr char kCfgKeyUpdateEnabled[]            = "update.enabled";
constexpr char kCfgKeyUpdateAutoInstallOnClose[] = "update.auto_install_on_close";

// Default subfolder name for project-relative animations storage. Convention
// matches game-industry layouts (Animations/ alongside Audio/, Materials/,
// etc.). Created automatically when a saved project is opened.
constexpr char kProjectAnimationsSubdir[] = "Animations";

// v0.9.1: marker stored on every item we create, via REAPER's per-item
// extended state (P_EXT:<key>). When resolving the playhead and placing
// new content, we ONLY consider items carrying this marker — that way an
// audio file the user named "frog_jump.wav" doesn't accidentally match an
// animation called "frog_jump.glb" in the library. Backwards-compatibility
// note: items created in v0.9.0 and earlier don't carry this marker; they
// won't be recognised as ours after upgrade. Re-run "Place all" to recreate
// them in the new format.
constexpr char kHoloRollItemMarkerKey[]   = "P_EXT:holoroll";
constexpr char kHoloRollItemMarkerValue[] = "1";

// v0.11.0: per-item pre/post-roll, stamped at placement time. Read during
// playback resolution to compute correct frame index inside hold-frame
// regions. Stored as seconds in plain decimal form ("1.00", "0.5") so
// users editing .rpp by hand can read them.
constexpr char kHoloRollItemPreRollKey[]  = "P_EXT:holoroll_pre";
constexpr char kHoloRollItemPostRollKey[] = "P_EXT:holoroll_post";

// Global "incoming" folder where engine bridges, scripts, or the user can
// drop new animation files. Files appearing here are auto-moved into the
// active project's Animations/ folder on next OnTimer tick. Path is
// %APPDATA%\REAPER\UserPlugins\HoloRollIncoming — i.e. next to the DLL.
constexpr char kIncomingSubdir[] = "HoloRollIncoming";

// v0.12.0-alpha.14: default fallback for Untitled projects. Same parent
// directory as Incoming/ — sits next to the DLL under
// %APPDATA%\REAPER\UserPlugins\HoloRollDefault. Used by
// ResolveActiveAnimationsFolder() as the third-priority fallback when
// there's no project-level override and no saved .rpp. User can edit
// `default_animations_folder` in holoroll_config.ini to point it
// somewhere else.
constexpr char kDefaultAnimationsSubdir[] = "HoloRollDefault";

constexpr char kToggleViewportCommandName[] = "MDDVIEWPORT_TOGGLE";
constexpr char kToggleViewportActionDesc[] = "HoloRoll: Toggle Viewport";
constexpr char kChooseFolderCommandName[] = "MDDVIEWPORT_CHOOSE_FOLDER";
constexpr char kChooseFolderActionDesc[] = "HoloRoll: Choose Animations Folder";
constexpr char kPlaceRegionsCommandName[] = "MDDVIEWPORT_PLACE_REGIONS";
constexpr char kPlaceRegionsActionDesc[] = "HoloRoll: Place All Animation Regions";
constexpr char kOpenConfigCommandName[] = "MDDVIEWPORT_OPEN_CONFIG";
constexpr char kOpenConfigActionDesc[] = "HoloRoll: Open Config File";
constexpr char kReloadConfigCommandName[] = "MDDVIEWPORT_RELOAD_CONFIG";
constexpr char kReloadConfigActionDesc[] = "HoloRoll: Reload Config";

constexpr char kViewportDockTitle[] = "HoloRoll";
constexpr char kViewportDockIdent[] = "holoroll_docker";

std::string DirOfModule(HMODULE module) {
  char buffer[MAX_PATH] = {};
  const DWORD len = GetModuleFileNameA(module, buffer, MAX_PATH);
  if (len == 0) return {};
  std::string path(buffer, len);
  const auto pos = path.find_last_of("\\/");
  return pos == std::string::npos ? std::string{} : path.substr(0, pos);
}

std::string ConfigFilePath() {
  const std::string dir = DirOfModule(g_dllHandle);
  return dir.empty() ? std::string(kConfigFileName) : dir + "\\" + kConfigFileName;
}

// v0.12.0-alpha.13: both loggers gate on g_debugEnabled. By default the
// flag is false → REAPER console stays clean during normal operation.
// The "Debug log" checkbox in the overlay's Config section flips it.
//
// Through alpha.12 we kept two log paths: ConsoleLog (gated by a build-
// time kVerboseLog=false) and SpikeLog (always on, used for "show this
// to the user no matter what" paths like the v0.6 spike test and the
// alpha.7 envelope-debug). With a runtime flag both collapse into the
// same gate; we keep the two names because there are many callers and
// renaming them is pure churn.
void ConsoleLog(const std::string& msg) {
  if (!g_debugEnabled.load()) return;
  if (g_bridge.Api().showConsoleMsg) g_bridge.Api().showConsoleMsg(msg.c_str());
}

void SpikeLog(const std::string& msg) {
  if (!g_debugEnabled.load()) return;
  if (g_bridge.Api().showConsoleMsg) g_bridge.Api().showConsoleMsg(msg.c_str());
}

// v0.12.0-alpha.1: force the REAPER console window to be visible so the
// user can see motion-analysis sanity-check output even if they had the
// console hidden from a previous session. Action 40078 is REAPER's native
// "View: Show console output" command — it's a toggle, so we only call it
// when the console isn't already visible (otherwise we'd close it).
//
// Action ID 40078 is a built-in REAPER command. GetToggleCommandState
// returns 1 when the console is open, 0 when closed, -1 if the action
// doesn't have toggle state. We only fire on 0. If GetToggleCommandState
// is unavailable in the resolved API (very old REAPER), we skip the
// auto-show entirely — better than risking a close.
void ForceShowReaperConsole() {
  const auto& api = g_bridge.Api();
  if (!api.mainOnCommand || !api.getToggleCommandState) return;
  const int state = api.getToggleCommandState(40078);
  if (state == 0) {
    // Console is currently closed — toggle it open.
    api.mainOnCommand(40078, 0);
  }
}

void EnsureConfigDefaults() {
  if (!g_config.Has(kCfgKeyFps)) g_config.SetDouble(kCfgKeyFps, kDefaultFps);
  if (!g_config.Has(kCfgKeyGap)) g_config.SetDouble(kCfgKeyGap, kDefaultGapSeconds);
  if (!g_config.Has(kCfgKeyRegionPrefix)) g_config.SetString(kCfgKeyRegionPrefix, "");
  if (!g_config.Has(kCfgKeyHotReload)) g_config.SetDouble(kCfgKeyHotReload, 1.0);
  if (!g_config.Has(kCfgKeySceneShowGround)) g_config.SetDouble(kCfgKeySceneShowGround, 1.0);
  if (!g_config.Has(kCfgKeySceneRadius))     g_config.SetDouble(kCfgKeySceneRadius, 20.0);
  if (!g_config.Has(kCfgKeySceneGridStep))   g_config.SetDouble(kCfgKeySceneGridStep, 1.0);
  if (!g_config.Has(kCfgKeySceneShowBboxDims))   g_config.SetDouble(kCfgKeySceneShowBboxDims, 1.0);
  if (!g_config.Has(kCfgKeySceneShowGridLabels)) g_config.SetDouble(kCfgKeySceneShowGridLabels, 1.0);
  if (!g_config.Has(kCfgKeySceneShowRefHuman))   g_config.SetDouble(kCfgKeySceneShowRefHuman, 1.0);
  // v0.11.0 / alpha.10: placement defaults. Variations and region overhang
  // keys were removed in alpha.10; we no longer seed defaults for them.
  // Old configs that already have these keys keep them as harmless dead
  // values (no reader, no writer).
  if (!g_config.Has(kCfgKeyPlacementPreRoll))   g_config.SetDouble(kCfgKeyPlacementPreRoll, 1.0);
  if (!g_config.Has(kCfgKeyPlacementPostRoll))  g_config.SetDouble(kCfgKeyPlacementPostRoll, 1.0);
  // v0.12.0-alpha.13: debug log off by default. The user opts in via
  // the "Debug log" checkbox in the overlay's Config section.
  if (!g_config.Has(kCfgKeyDebugEnabled))       g_config.SetDouble(kCfgKeyDebugEnabled, 0.0);
  // v0.12.0-alpha.14: default animations folder for Untitled projects.
  // Seeded once on first run; the user can edit the .ini to change it.
  // We resolve the actual path lazily (DefaultAnimationsFolder() below)
  // because GetIncomingFolder relies on g_dllHandle which may not be
  // set yet at the time EnsureConfigDefaults runs.
  if (!g_config.Has(kCfgKeyDefaultAnimationsFolder)) {
    g_config.SetString(kCfgKeyDefaultAnimationsFolder, std::string{});
  }
  // v0.13.0-alpha.1: auto-updater defaults — both ON. Users can
  // opt out of network checks via update.enabled=0, or of automatic
  // install via update.auto_install_on_close=0.
  if (!g_config.Has(kCfgKeyUpdateEnabled))            g_config.SetDouble(kCfgKeyUpdateEnabled, 1.0);
  if (!g_config.Has(kCfgKeyUpdateAutoInstallOnClose)) g_config.SetDouble(kCfgKeyUpdateAutoInstallOnClose, 1.0);
}

// Read the configured region-name prefix and apply it to the AnimationLibrary
// static state. Called on plugin startup and on Reload config.
//
// Default is empty (clean basenames). Old projects with "MDD: foo" regions
// keep working because AnimationLibrary::StripPrefix() always recognises the
// legacy "MDD: " prefix even when the configured one differs.
void ApplyRegionPrefixFromConfig() {
  AnimationLibrary::SetRegionNamePrefix(g_config.GetString(kCfgKeyRegionPrefix, ""));
}

bool HotReloadEnabled() {
  return g_config.GetDouble(kCfgKeyHotReload, 1.0) >= 0.5;
}

// Apply scene settings from config to the viewport. Called on startup and
// after `Reload config`.
void ApplySceneSettingsToViewport() {
  const bool show     = g_config.GetDouble(kCfgKeySceneShowGround, 1.0) >= 0.5;
  const float radius  = static_cast<float>(g_config.GetDouble(kCfgKeySceneRadius, 20.0));
  const float gridStep = static_cast<float>(g_config.GetDouble(kCfgKeySceneGridStep, 1.0));
  const bool bbox     = g_config.GetDouble(kCfgKeySceneShowBboxDims, 1.0) >= 0.5;
  const bool labels   = g_config.GetDouble(kCfgKeySceneShowGridLabels, 1.0) >= 0.5;
  const bool refHuman = g_config.GetDouble(kCfgKeySceneShowRefHuman, 1.0) >= 0.5;
  g_viewport.SetSceneSettings(show, radius, gridStep, bbox, labels, refHuman);
}

// Pull current scene state from viewport and write it into the config + disk.
// Called when the overlay UI dirties the values.
void PersistSceneSettingsFromViewport() {
  bool show = true;
  float radius = 20.0f;
  float gridStep = 1.0f;
  bool bbox = true, labels = true, refHuman = false;
  g_viewport.GetSceneSettings(&show, &radius, &gridStep, &bbox, &labels, &refHuman);
  g_config.SetDouble(kCfgKeySceneShowGround, show ? 1.0 : 0.0);
  g_config.SetDouble(kCfgKeySceneRadius,     static_cast<double>(radius));
  g_config.SetDouble(kCfgKeySceneGridStep,   static_cast<double>(gridStep));
  g_config.SetDouble(kCfgKeySceneShowBboxDims,   bbox ? 1.0 : 0.0);
  g_config.SetDouble(kCfgKeySceneShowGridLabels, labels ? 1.0 : 0.0);
  g_config.SetDouble(kCfgKeySceneShowRefHuman,   refHuman ? 1.0 : 0.0);
  g_config.Save();
}

// v0.11.0: placement options round-tripped between holoroll_config.ini and
// the viewport's inline fields. Read once on viewport open and on Reload
// config; persisted whenever the user touches a field.
//
// alpha.10: variations + region overhang removed (only pre/post-roll
// remain). The two legacy config keys are still recognised but no
// longer surfaced to UI — existing configs upgrade silently.
void ApplyPlacementOptionsToViewport() {
  const float preRoll  = static_cast<float>(g_config.GetDouble(kCfgKeyPlacementPreRoll, 1.0));
  const float postRoll = static_cast<float>(g_config.GetDouble(kCfgKeyPlacementPostRoll, 1.0));
  g_viewport.SetPlacementOptions(preRoll, postRoll);
}

void PersistPlacementOptionsFromViewport() {
  float preRoll = 1.0f, postRoll = 1.0f;
  g_viewport.GetPlacementOptions(&preRoll, &postRoll);
  g_config.SetDouble(kCfgKeyPlacementPreRoll, static_cast<double>(preRoll));
  g_config.SetDouble(kCfgKeyPlacementPostRoll, static_cast<double>(postRoll));
  g_config.Save();
}

// v0.12.0-alpha.13: round-trip the debug flag between config, viewport
// checkbox, and the g_debugEnabled atomic. Same shape as the placement
// pair above.
void ApplyDebugFlagFromConfig() {
  const bool enabled = g_config.GetDouble(kCfgKeyDebugEnabled, 0.0) >= 0.5;
  g_debugEnabled.store(enabled);
  g_viewport.SetDebugEnabled(enabled);
}

void PersistDebugFlagFromViewport() {
  const bool enabled = g_viewport.GetDebugEnabled();
  g_debugEnabled.store(enabled);
  g_config.SetDouble(kCfgKeyDebugEnabled, enabled ? 1.0 : 0.0);
  g_config.Save();
  // When the user just turned the flag ON, pop the REAPER console window
  // so they immediately see the lines that will follow. Turning it OFF
  // does not auto-close the console (the user may have other plugins'
  // output they want to keep visible).
  if (enabled) ForceShowReaperConsole();
}

double GetFps() { return g_config.GetDouble(kCfgKeyFps, kDefaultFps); }
double GetGap() { return g_config.GetDouble(kCfgKeyGap, kDefaultGapSeconds); }

// ---- v0.7.0 project-relative animations folder -----------------------------
//
// The active animations folder is now resolved per REAPER project, not
// per global config. Resolution order on every OnTimer tick:
//
//   1. If active project has an override saved in its ext-state, use that.
//   2. Else if active project is saved (has a .rpp path on disk), use
//      <project_dir>/Animations/. Auto-created on first sight.
//   3. Else (Untitled, never saved) - no active folder. Viewport shows hint.
//
// `g_currentProjectPath` caches the .rpp path we last saw; OnTimer compares
// it to the current EnumProjects(-1) value and triggers OnProjectChanged()
// when it differs. This handles Open / Save As / Switch Project / Close All.
//
// Initialised to a sentinel value that no real path would equal, so the
// very first OnTimer comparison always fires OnProjectChanged() and
// performs the initial library scan — even if the active project is
// Untitled (real path "").
std::string g_currentProjectPath = "\x01HOLOROLL_UNINIT";
std::string g_currentAnimationsFolder;  // Resolved active folder, or empty.

// Read the .rpp path of REAPER's currently-active project. Returns empty
// string if the project is Untitled or the API is unavailable.
std::string GetActiveProjectPath() {
  const auto& api = g_bridge.Api();
  if (!api.enumProjects) return {};
  char buf[4096] = {};
  api.enumProjects(-1, buf, sizeof(buf));
  return std::string(buf);
}

// Strip the file portion of `\path\to\project.rpp` -> `\path\to`. Returns
// empty string if there's no separator (e.g. just a filename).
std::string DirOfPath(const std::string& fullPath) {
  const auto pos = fullPath.find_last_of("\\/");
  return pos == std::string::npos ? std::string{} : fullPath.substr(0, pos);
}

// v0.10.1: extract "<basename>" from `\path\to\<basename>.rpp`. Returns
// empty if no filename portion. Used to scope the animations folder per
// project so two .rpps in the same directory don't share the same folder.
std::string ProjectBasenameFromPath(const std::string& projPath) {
  if (projPath.empty()) return {};
  const auto slash = projPath.find_last_of("\\/");
  const std::string filename = (slash == std::string::npos) ? projPath
                                                             : projPath.substr(slash + 1);
  const auto dot = filename.find_last_of('.');
  return (dot == std::string::npos) ? filename : filename.substr(0, dot);
}

// Read project-level override for animations folder. Returns empty if no
// override is set on the active project.
std::string GetProjectAnimationsOverride() {
  const auto& api = g_bridge.Api();
  if (!api.getProjExtState) return {};
  char buf[4096] = {};
  // Per REAPER docs: nullptr ReaProject* means the active project. Return
  // value > 0 means the key existed.
  const int rv = api.getProjExtState(nullptr, kExtStateSection,
                                     kProjExtStateAnimDirOverride,
                                     buf, sizeof(buf));
  if (rv > 0 && buf[0] != '\0') return std::string(buf);
  return {};
}

// Write project-level override for animations folder. Pass empty string to
// clear the override (returns the project to default <project>/Animations/).
void SetProjectAnimationsOverride(const std::string& dir) {
  const auto& api = g_bridge.Api();
  if (!api.setProjExtState) return;
  api.setProjExtState(nullptr, kExtStateSection,
                      kProjExtStateAnimDirOverride, dir.c_str());
}

// Recursive Win32 directory creation. Returns true if the directory exists
// after the call (already existed or was newly created).
bool EnsureFolderExists(const std::string& path) {
  if (path.empty()) return false;
  const DWORD attr = GetFileAttributesA(path.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

  // CreateDirectoryA fails if a parent doesn't exist; recurse up first.
  const std::string parent = DirOfPath(path);
  if (!parent.empty() && parent != path) {
    if (!EnsureFolderExists(parent)) return false;
  }
  return CreateDirectoryA(path.c_str(), nullptr) != 0 ||
         GetLastError() == ERROR_ALREADY_EXISTS;
}

// v0.12.0-alpha.14: resolve the default animations folder used when
// neither a project-level override nor a saved-project path is
// available. Reads from the `default_animations_folder` config key;
// if that's empty, falls back to %APPDATA%\REAPER\UserPlugins\HoloRollDefault.
std::string DefaultAnimationsFolder() {
  const std::string fromConfig = g_config.GetString(kCfgKeyDefaultAnimationsFolder, std::string{});
  if (!fromConfig.empty()) return fromConfig;

  // Same parent directory as Incoming/ — sits next to the DLL.
  const std::string base = DirOfModule(g_dllHandle);
  if (base.empty()) return {};
  return base + "\\" + kDefaultAnimationsSubdir;
}

// Resolve the animations folder for the current project state. Always
// returns a non-empty path as of alpha.14 — Untitled projects fall back
// to the per-user default folder instead of being a hard-stop.
//
// Resolution order:
//   1. Project-level override (set via "Choose folder..." on a saved
//      project, stored in the .rpp). Wins even when the project is
//      Untitled — the user explicitly picked it.
//   2. <project_dir>/Animations/<project_basename>/ — auto-derived from
//      the saved .rpp path (v0.10.1 layout: each project gets its own
//      subfolder so two .rpps in the same dir don't share a library).
//   3. Default fallback (alpha.14+). Lets the plugin work on Untitled
//      projects, drag-n-drop, and Incoming/ drain when no project is
//      saved yet. When the user eventually saves the project, future
//      operations switch to (2); files that accumulated in the default
//      folder STAY there — no auto-migration.
std::string ResolveActiveAnimationsFolder() {
  // Override always wins, even if the project hasn't been saved yet (the
  // user explicitly pointed Choose folder... somewhere).
  const std::string override = GetProjectAnimationsOverride();
  if (!override.empty()) return override;

  const std::string proj = GetActiveProjectPath();
  if (!proj.empty()) {
    const std::string projDir = DirOfPath(proj);
    const std::string projBasename = ProjectBasenameFromPath(proj);
    if (!projDir.empty() && !projBasename.empty()) {
      return projDir + "\\" + kProjectAnimationsSubdir + "\\" + projBasename;
    }
  }

  // alpha.14: third-priority fallback for Untitled projects.
  return DefaultAnimationsFolder();
}

// ---- v0.8.0 Incoming folder + auto-move ----------------------------------
//
// Resolve the path to the global Incoming folder. Sits next to the DLL in
// %APPDATA%\REAPER\UserPlugins\HoloRollIncoming. Created on plugin startup.
// This folder is independent of any project; it's where engine bridges,
// scripts, or the user can drop files for HoloRoll to pick up.
std::string GetIncomingFolder() {
  const std::string base = DirOfModule(g_dllHandle);
  if (base.empty()) return {};
  return base + "\\" + kIncomingSubdir;
}

// Lowercase extension of a file name (".glb", ".mdd", ".obj", or "").
std::string LowerExtOf(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot);
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// Stem of a file name without extension. "foo/bar.glb" -> "bar".
std::string StemOf(const std::string& path) {
  const auto slash = path.find_last_of("\\/");
  const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const auto dot = base.find_last_of('.');
  return (dot == std::string::npos) ? base : base.substr(0, dot);
}

bool FileExistsOnDisk(const std::string& path) {
  const DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Find a non-colliding destination path. If `<destDir>\<stem><ext>` doesn't
// exist, return that. Otherwise try `<destDir>\<stem>_2<ext>`, `_3`, etc.
// Caps at 999 attempts (defensive; collisions deeper than that mean
// something is very wrong and we'd rather fail than spin).
std::string GenerateUniqueDestPath(const std::string& destDir,
                                   const std::string& stem,
                                   const std::string& ext) {
  std::string candidate = destDir + "\\" + stem + ext;
  if (!FileExistsOnDisk(candidate)) return candidate;
  for (int i = 2; i < 1000; ++i) {
    candidate = destDir + "\\" + stem + "_" + std::to_string(i) + ext;
    if (!FileExistsOnDisk(candidate)) return candidate;
  }
  return {};  // Truly out of options.
}

// Move `src` to a non-colliding path under `destDir`. Returns the resulting
// destination path on success, or empty string on failure. Collisions are
// resolved by appending _<N> suffix to the stem (which then becomes a
// natural variation — ResolveAnimationByItemName strips the suffix during
// playback resolution, so the user gets two items playing the same
// animation with potentially different sounds on top).
std::string MoveFileWithCollisionRename(const std::string& src,
                                        const std::string& destDir) {
  if (!EnsureFolderExists(destDir)) return {};
  const std::string stem = StemOf(src);
  const std::string ext = LowerExtOf(src);
  const std::string dest = GenerateUniqueDestPath(destDir, stem, ext);
  if (dest.empty()) return {};

  // MoveFileExA without REPLACE_EXISTING: we already chose a non-colliding
  // path. COPY_ALLOWED handles cross-disk moves (e.g. Incoming on C:,
  // project on D:). The OS does copy+delete in that case, which is not
  // atomic but acceptable for our use — watcher debounce hides the gap.
  if (!MoveFileExA(src.c_str(), dest.c_str(),
                   MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
    return {};
  }
  return dest;
}

// Drain the Incoming folder of any animation files, moving them into the
// active project's Animations/ folder. Called when the project changes
// (e.g. Untitled -> saved) and any time we rebuild the library, so files
// that arrived while the user had no saved project get picked up.
std::size_t DrainIncomingToProject() {
  const std::string incoming = GetIncomingFolder();
  const std::string projAnims = ResolveActiveAnimationsFolder();
  if (incoming.empty() || projAnims.empty()) return 0;
  if (incoming == projAnims) return 0;  // Defensive: never move into self.

  WIN32_FIND_DATAA fd{};
  const std::string pattern = incoming + "\\*.*";
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;

  std::size_t moved = 0;
  std::vector<std::string> filesToMove;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::string name = fd.cFileName;
    const std::string ext = LowerExtOf(name);
    // Only move files we actually understand. .obj is paired with .mdd so
    // it's also legitimate (artists may drop both at once).
    if (ext != ".mdd" && ext != ".glb" && ext != ".obj") continue;
    filesToMove.push_back(incoming + "\\" + name);
  } while (FindNextFileA(h, &fd));
  FindClose(h);

  for (const std::string& src : filesToMove) {
    const std::string dest = MoveFileWithCollisionRename(src, projAnims);
    if (!dest.empty()) {
      ++moved;
      ConsoleLog("[holoroll] moved " + src + " -> " + dest + "\n");
    } else {
      ConsoleLog("[holoroll] FAILED to move " + src +
                 " (GetLastError=" + std::to_string(GetLastError()) + ")\n");
    }
  }
  return moved;
}

// Callback for the OLE drop target attached to our viewport window. Called
// on the main thread when the user drops files from Windows Explorer onto
// the HoloRoll viewport. We move the files into the active project's
// Animations/ folder — the project-folder watcher then picks them up via
// the regular hot-reload path (modal -> place items at cursor).
//
// alpha.14: ResolveActiveAnimationsFolder now always returns a usable
// folder (project-relative when saved, per-user default fallback when
// Untitled), so the only way this path bails is if the resolver itself
// returns empty — a very-early-init edge case where g_dllHandle isn't
// set yet. Log + skip silently rather than crashing.
void OnViewportFilesDropped(const std::vector<std::string>& paths) {
  if (paths.empty()) return;

  const std::string projAnims = ResolveActiveAnimationsFolder();
  if (projAnims.empty()) {
    SpikeLog("[holoroll] drop ignored: save the REAPER project first.\n");
    return;
  }

  std::size_t moved = 0;
  for (const std::string& src : paths) {
    // The user might have dragged a file that's ALREADY inside the
    // project's Animations/ (rare, but Win32 doesn't prevent it). In
    // that case skip silently — nothing to move.
    const std::string srcDir = DirOfPath(src);
    if (_stricmp(srcDir.c_str(), projAnims.c_str()) == 0) continue;

    const std::string dest = MoveFileWithCollisionRename(src, projAnims);
    if (!dest.empty()) {
      ++moved;
      ConsoleLog("[holoroll] drop: moved " + src + " -> " + dest + "\n");
    } else {
      // Move failed. Most common reason: source is on a network drive or
      // user lacks delete permission. Fall back to copy in that case.
      // We don't auto-copy silently though — the user explicitly dragged,
      // and "why is the original still there" is a worse UX than a log line.
      const DWORD err = GetLastError();
      SpikeLog("[holoroll] drop: FAILED to move " + src +
               " (GetLastError=" + std::to_string(err) + ")\n");
    }
  }

  if (moved > 0) {
    ConsoleLog("[holoroll] drop: " + std::to_string(moved) + " file(s) moved.\n");
  }
}

std::vector<TimelineRegion> ReadLiveRegionsFromReaper() {
  std::vector<TimelineRegion> out;
  const auto& api = g_bridge.Api();
  if (!api.enumProjectMarkers3) return out;

  const int ourColor = AnimationLibrary::RegionColorReaper();

  int idx = 0;
  while (true) {
    bool isrgn = false;
    double pos = 0.0, rgnend = 0.0;
    const char* name = nullptr;
    int markrgnindexnumber = 0;
    int color = 0;
    const int next = api.enumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend, &name, &markrgnindexnumber, &color);
    if (next == 0) break;
    if (isrgn && color == ourColor) {
      const std::string regionName = name ? name : "";
      // StripPrefix recognises both the currently-configured prefix and the
      // legacy "MDD: " prefix, so v0.3.0 projects keep working.
      const std::string basename = AnimationLibrary::StripPrefix(regionName);
      const std::size_t animIdx = g_lib.FindAnimationIndexByBasename(basename);
      if (animIdx != std::numeric_limits<std::size_t>::max()) {
        TimelineRegion r;
        r.animationIndex = animIdx;
        r.startSeconds = pos;
        r.endSeconds = rgnend;
        r.regionName = regionName;
        out.push_back(r);
      }
    }
    idx = next;
  }
  return out;
}

// Refresh the library + regions from the currently-active animations folder.
// `dir` is passed only for compatibility with existing callers; the actual
// folder used is whatever ResolveActiveAnimationsFolder() returns now. If
// no folder is active (Untitled), this becomes a no-op clear.
void RebuildLibraryAndRegions(const std::string& /*ignored*/) {
  const std::string dir = ResolveActiveAnimationsFolder();
  g_currentAnimationsFolder = dir;

  // Restart watcher even if folder is empty (Stop is idempotent).
  g_watcher.Stop();
  g_pendingNewAnimations.clear();
  g_watcherEventsAccumulating = false;

  if (dir.empty()) {
    // No active project, nothing to scan. Clear the library so we don't
    // keep showing stale animations from the previous project.
    g_lib.Clear();
    g_poses.Clear();
    g_activeAnimIdx = kNoActiveAnim;
    return;
  }

  // Auto-create the folder if it's the project-default path. We assume the
  // user wants it; if the override is set to a missing folder we don't
  // create that one (overrides are explicit user choices and shouldn't be
  // silently materialised in odd places).
  const std::string override = GetProjectAnimationsOverride();
  if (override.empty()) {
    EnsureFolderExists(dir);
  }

  std::string log;
  const std::size_t loaded = g_lib.ScanFolder(dir, GetFps(), &log);
  g_lib.BuildRegions(GetFps(), GetGap(), 0.0);
  // v0.12.0-alpha.1: scan log includes per-animation "top active bones"
  // summaries. v0.12.0-alpha.6: kept the log itself (visible if the
  // console is open) but removed the ForceShowReaperConsole pop — it was
  // intrusive on every project load / hot-reload and the data is now
  // surfaced via the placed envelopes anyway.
  if (!log.empty()) {
    SpikeLog(log);
  }
  ConsoleLog("[holoroll] library: " + std::to_string(loaded) +
             " animation(s), " + std::to_string(g_lib.Regions().size()) + " region(s) prepared.\n");

  g_poses.Clear();
  g_activeAnimIdx = kNoActiveAnim;

  if (HotReloadEnabled()) {
    if (!g_watcher.Start(dir)) {
      ConsoleLog("[holoroll] FolderWatcher failed to start on: " + dir + "\n");
    }
  }
}

// Called by OnTimer when the active project has changed (open / save-as /
// switch / close). Repoints the library and watcher at the new project's
// animations folder. Also drains the Incoming folder so any orphan files
// that arrived while the user had no saved project get picked up by the
// new one.
void OnProjectChanged() {
  const std::string newPath = GetActiveProjectPath();
  ConsoleLog("[holoroll] project changed: " +
             (newPath.empty() ? std::string("(Untitled)") : newPath) + "\n");
  g_currentProjectPath = newPath;

  // Move any pending files from Incoming/ into the new project's
  // Animations/. This must run BEFORE RebuildLibraryAndRegions so the
  // initial scan picks them up. RebuildLibraryAndRegions itself may
  // produce hot-reload events on files we just moved — that's fine,
  // they'll surface as "new animations" in the modal.
  DrainIncomingToProject();

  RebuildLibraryAndRegions("");
}

void DeleteOurRegions() {
  const auto& api = g_bridge.Api();
  if (!api.enumProjectMarkers3 || !api.deleteProjectMarker) return;

  const int ourColor = AnimationLibrary::RegionColorReaper();
  // Recognise legacy-prefixed regions on cleanup too, so re-running
  // "Place regions" on a v0.3.0 project doesn't accumulate duplicates.
  const std::string legacyPrefix = AnimationLibrary::kLegacyRegionNamePrefix;
  const std::string& currentPrefix = AnimationLibrary::RegionNamePrefix();

  std::vector<int> toDelete;
  int idx = 0;
  while (true) {
    bool isrgn = false;
    double pos = 0.0, rgnend = 0.0;
    const char* name = nullptr;
    int markrgnindexnumber = 0;
    int color = 0;
    const int next = api.enumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend, &name, &markrgnindexnumber, &color);
    if (next == 0) break;
    if (isrgn) {
      const bool colorMatch = (color == ourColor);
      const std::string nm = name ? name : "";
      const bool legacyMatch = nm.rfind(legacyPrefix, 0) == 0;
      const bool currentMatch = !currentPrefix.empty() && nm.rfind(currentPrefix, 0) == 0;
      if (colorMatch || legacyMatch || currentMatch) {
        toDelete.push_back(markrgnindexnumber);
      }
    }
    idx = next;
  }
  for (const int id : toDelete) {
    api.deleteProjectMarker(nullptr, id, true);
  }
}

// v0.13.0-alpha.3: wipe ALL regions (not just HoloRoll-coloured ones).
// Used by the build_regions socket verb when clear_existing=true. The
// external app explicitly asked for a clean slate; we honour that
// without filtering by colour/prefix. Markers (isrgn=false) are
// untouched — only regions are deleted.
void DeleteAllRegions() {
  const auto& api = g_bridge.Api();
  if (!api.enumProjectMarkers3 || !api.deleteProjectMarker) return;

  std::vector<int> toDelete;
  int idx = 0;
  while (true) {
    bool isrgn = false;
    double pos = 0.0, rgnend = 0.0;
    const char* name = nullptr;
    int markrgnindexnumber = 0;
    int color = 0;
    const int next = api.enumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend,
                                              &name, &markrgnindexnumber, &color);
    if (next == 0) break;
    if (isrgn) toDelete.push_back(markrgnindexnumber);
    idx = next;
  }
  for (const int id : toDelete) {
    api.deleteProjectMarker(nullptr, id, /*isrgn=*/true);
  }
}

void PlaceOurRegions() {
  const auto& api = g_bridge.Api();
  if (!api.addProjectMarker2) {
    ConsoleLog("[holoroll] AddProjectMarker2 unavailable; cannot create regions.\n");
    return;
  }

  DeleteOurRegions();
  g_lib.BuildRegions(GetFps(), GetGap(), 0.0);

  const int ourColor = AnimationLibrary::RegionColorReaper();
  for (const auto& r : g_lib.Regions()) {
    api.addProjectMarker2(nullptr, true, r.startSeconds, r.endSeconds, r.regionName.c_str(), -1, ourColor);
  }
  ConsoleLog("[holoroll] placed " + std::to_string(g_lib.Regions().size()) + " region(s).\n");
}

void RunFolderPicker() {
  HWND owner = g_viewport.IsOpen() ? g_viewport.Hwnd() : nullptr;
  const std::string current = g_currentAnimationsFolder;
  const std::string chosen = folder_picker::BrowseForFolder(owner, "Override animations folder for this project", current);
  if (chosen.empty()) return;
  // Save as project-level override. Default project folder is no longer
  // a global config knob — the only way to change it is per-project.
  SetProjectAnimationsOverride(chosen);
  RebuildLibraryAndRegions("");
}

// Reset the project override and go back to <project>/Animations/.
void ResetFolderToProjectDefault() {
  SetProjectAnimationsOverride("");
  RebuildLibraryAndRegions("");
}

void OpenViewportIfNeeded() {
  if (!g_viewport.IsOpen()) {
    if (!g_viewport.Open()) return;
    const auto& api = g_bridge.Api();
    if (api.dockWindowAddEx && api.dockWindowActivate) {
      api.dockWindowAddEx(g_viewport.Hwnd(), kViewportDockTitle, kViewportDockIdent, true);
      api.dockWindowActivate(g_viewport.Hwnd());
    }
    // Apply persisted scene settings to the freshly-opened viewport.
    ApplySceneSettingsToViewport();
    // v0.11.0: same for placement options.
    ApplyPlacementOptionsToViewport();
    // v0.12.0-alpha.13: hydrate debug-log toggle from config.
    ApplyDebugFlagFromConfig();

    // v0.9.0 / alpha.14: register OLE drop target so files dragged from
    // Explorer onto the viewport land in the active animations folder.
    // alpha.14 dropped the Untitled-project rejection — drops now always
    // accept because ResolveActiveAnimationsFolder() falls back to the
    // per-user default folder when the project is Untitled.
    drop_target::RegisterOnHwnd(
        g_viewport.Hwnd(),
        OnViewportFilesDropped,
        []() { return !ResolveActiveAnimationsFolder().empty(); });
  }
}

void CloseViewportIfNeeded() {
  if (g_viewport.IsOpen()) {
    drop_target::UnregisterFromHwnd(g_viewport.Hwnd());
    const auto& api = g_bridge.Api();
    if (api.dockWindowRemove) api.dockWindowRemove(g_viewport.Hwnd());
    g_viewport.Close();
  }
}

void OpenConfigInEditor() {
  const std::string path = ConfigFilePath();
  g_config.Save();
  ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ReloadConfigFromDisk() {
  g_config.Load(ConfigFilePath());
  EnsureConfigDefaults();
  ApplyRegionPrefixFromConfig();
  // The folder is now project-relative, not config-driven, so changing
  // the config doesn't trigger a folder change. We do still rebuild
  // regions in case `fps` or `region_gap_seconds` changed.
  g_lib.BuildRegions(GetFps(), GetGap(), 0.0);
  // Pick up scene settings the user may have edited in the file.
  if (g_viewport.IsOpen()) {
    ApplySceneSettingsToViewport();
    ApplyPlacementOptionsToViewport();
    // alpha.13: pick up debug flag if the user edited the .ini directly.
    ApplyDebugFlagFromConfig();
  }
  // Hot-reload toggle may have flipped.
  if (HotReloadEnabled() && !g_currentAnimationsFolder.empty() && !g_watcher.IsRunning()) {
    g_watcher.Start(g_currentAnimationsFolder);
  } else if (!HotReloadEnabled() && g_watcher.IsRunning()) {
    g_watcher.Stop();
    g_pendingNewAnimations.clear();
    g_watcherEventsAccumulating = false;
  }
  ConsoleLog("[holoroll] config reloaded.\n");
}

bool OnMainAction(int command, int flag) {
  (void)flag;
  if (command == g_toggleViewportCommandId) {
    if (g_viewport.IsOpen()) CloseViewportIfNeeded(); else OpenViewportIfNeeded();
    return true;
  }
  if (command == g_chooseFolderCommandId) { RunFolderPicker(); return true; }
  if (command == g_placeRegionsCommandId) { PlaceOurRegions(); return true; }
  if (command == g_openConfigCommandId) { OpenConfigInEditor(); return true; }
  if (command == g_reloadConfigCommandId) { ReloadConfigFromDisk(); return true; }
  return false;
}

// ---- v0.6.0 items workflow ------------------------------------------------
//
// Items are now the primary unit on the timeline; regions are added as a
// label/grouping convenience that REAPER will move with the items by
// default. Item NAME (take.P_NAME) carries the animation reference — we
// look it up in AnimationLibrary at playback time, with variation suffixes
// (_2, _3, ...) stripped to share one animation across multiple item copies.
//
// All API pointers must be checked before use; if a build of REAPER lacks
// any of them the helper returns failure and logs which one was missing.

// Buffer size for GetSetMediaItem*Info_String reads/writes. REAPER's spec
// doesn't formally guarantee the buffer size needed for reads, but 1024
// bytes is more than enough for any reasonable basename.
constexpr std::size_t kItemNameBufferSize = 1024;

// Read an item's name. Tries the active take's P_NAME first; falls back to
// item P_NOTES if no take name is set. Returns empty string if neither is
// readable or both are empty.
std::string ReadItemName(MediaItem* item) {
  if (!item) return {};
  const auto& api = g_bridge.Api();

  if (api.getActiveTake && api.getSetMediaItemTakeInfo_String) {
    if (MediaItem_Take* take = api.getActiveTake(item)) {
      char buf[kItemNameBufferSize] = {};
      if (api.getSetMediaItemTakeInfo_String(take, "P_NAME", buf, false) && buf[0] != '\0') {
        return std::string(buf);
      }
    }
  }
  if (api.getSetMediaItemInfo_String) {
    char buf[kItemNameBufferSize] = {};
    if (api.getSetMediaItemInfo_String(item, "P_NOTES", buf, false) && buf[0] != '\0') {
      return std::string(buf);
    }
  }
  return {};
}

// v0.9.1: tag this item as ours via per-item ext-state. Idempotent.
bool MarkAsHoloRollItem(MediaItem* item) {
  if (!item) return false;
  const auto& api = g_bridge.Api();
  if (!api.getSetMediaItemInfo_String) return false;
  // GetSet wants a writable buffer even for set; copy the literal in.
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%s", kHoloRollItemMarkerValue);
  return api.getSetMediaItemInfo_String(item, kHoloRollItemMarkerKey, buf, true);
}

// v0.11.0: stamp the pre/post-roll seconds onto an item we created.
// Both functions are idempotent and silent on failure.
void MarkItemPreRoll(MediaItem* item, float seconds) {
  if (!item) return;
  const auto& api = g_bridge.Api();
  if (!api.getSetMediaItemInfo_String) return;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", seconds);
  api.getSetMediaItemInfo_String(item, kHoloRollItemPreRollKey, buf, true);
}
void MarkItemPostRoll(MediaItem* item, float seconds) {
  if (!item) return;
  const auto& api = g_bridge.Api();
  if (!api.getSetMediaItemInfo_String) return;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", seconds);
  api.getSetMediaItemInfo_String(item, kHoloRollItemPostRollKey, buf, true);
}

// v0.11.0: read pre/post-roll back. Returns 0.0 if the item doesn't carry
// the key (i.e. it was created before v0.11.0 — backwards compatible:
// playback simply has no hold frames).
float ReadItemPreRoll(MediaItem* item) {
  if (!item) return 0.0f;
  const auto& api = g_bridge.Api();
  if (!api.getSetMediaItemInfo_String) return 0.0f;
  char buf[32] = {};
  if (!api.getSetMediaItemInfo_String(item, kHoloRollItemPreRollKey, buf, false)) return 0.0f;
  if (buf[0] == '\0') return 0.0f;
  return static_cast<float>(std::atof(buf));
}
float ReadItemPostRoll(MediaItem* item) {
  if (!item) return 0.0f;
  const auto& api = g_bridge.Api();
  if (!api.getSetMediaItemInfo_String) return 0.0f;
  char buf[32] = {};
  if (!api.getSetMediaItemInfo_String(item, kHoloRollItemPostRollKey, buf, false)) return 0.0f;
  if (buf[0] == '\0') return 0.0f;
  return static_cast<float>(std::atof(buf));
}

// v0.9.1: read back the marker. Returns true iff the item carries our tag.
// Cheap: one GetSet call returning a one-byte string. Used by
// EnumProjectItems to filter out non-HoloRoll content (audio, midi, video,
// etc.) before resolving names against the animation library.
bool IsHoloRollItem(MediaItem* item) {
  if (!item) return false;
  const auto& api = g_bridge.Api();
  if (!api.getSetMediaItemInfo_String) return false;
  char buf[8] = {};
  if (!api.getSetMediaItemInfo_String(item, kHoloRollItemMarkerKey, buf, false)) {
    return false;
  }
  return buf[0] != '\0';
}

// v0.9.1: find the latest region end across the entire project. Used to
// place new items right after existing content so we never overlap.
// Returns 0.0 if no regions exist (placement starts at timeline origin).
//
// As of alpha.10 we no longer create regions ourselves (see CHANGELOG),
// so this helper is kept around only for legacy compatibility (older
// projects may still have HoloRoll regions). New placement code calls
// FindLastHoloRollItemEnd instead.
double FindLastRegionEnd() {
  const auto& api = g_bridge.Api();
  if (!api.enumProjectMarkers3) return 0.0;

  double last = 0.0;
  int idx = 0;
  while (true) {
    bool isrgn = false;
    double pos = 0.0, rgnend = 0.0;
    const char* nm = nullptr;
    int rgnIdx = 0, color = 0;
    const int next = api.enumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend,
                                             &nm, &rgnIdx, &color);
    if (next == 0) break;
    if (isrgn && rgnend > last) last = rgnend;
    idx = next;
  }
  return last;
}


// ---- v0.9.1 / v0.12.0-alpha.6 items + motion track ----------------------
//
// HoloRoll places everything on a single persistent track named
// "HoloRoll": the animation media items, the holoroll_motion JSFX, and
// (via the JSFX) the per-bone motion-analysis envelopes. v0.9.1 used to
// create a fresh top track per placement call (no name); alpha.5 added a
// separate "HoloRoll Motion" track at the bottom for the JSFX +
// envelopes. alpha.6 collapses both into one persistent track — the
// layout users actually expect.
//
// EnsureTrackOnTop is now find-or-create-by-name: subsequent placements
// land on the same row, envelopes carry over, no track stacking.
constexpr char kItemsTrackName[] = "HoloRoll";
// Names by which TrackFX_AddByName might find our JSFX. We try them in
// order; the first one that succeeds wins.
//
// IMPORTANT: order matters. We put the desc: line first because that's
// the canonical "display name" REAPER uses, and that resolution path is
// the one that actually loads the plugin file properly. The filename
// forms below it sometimes succeed but resolve to a broken alias entry
// in REAPER's FX index (observed empirically on REAPER 7+) — the JSFX
// loads but is reported as "could not be loaded". Match desc: first to
// avoid that path entirely.
constexpr const char* kMotionFxNameCandidates[] = {
    "HoloRoll Motion",              // desc: line bare — PREFERRED
    "JS: HoloRoll Motion",          // desc: line with the JS prefix REAPER displays
    "HoloRoll/holoroll_motion",     // relative path inside Effects/
    "holoroll_motion.jsfx",         // filename with extension
    "holoroll_motion",              // filename without extension — LAST RESORT
};

// Read a track's display name. Returns empty string on failure or if the
// track has no name set.
std::string ReadTrackName(MediaTrack* track) {
  if (!track) return {};
  const auto& api = g_bridge.Api();
  if (!api.getSetMediaTrackInfo_String) return {};
  // P_NAME buffer is read-back as a string; REAPER docs don't give a hard
  // size limit but track names are conventionally short. 256 is generous.
  char buf[256] = {};
  if (!api.getSetMediaTrackInfo_String(track, "P_NAME", buf, false)) return {};
  return std::string(buf);
}

// Walk all tracks in the active project, find the first one whose P_NAME
// matches `name` exactly. Case-sensitive by design — a stray manual rename
// (e.g. "HoloRoll motion" lowercase) shouldn't silently match and confuse
// the user. Returns nullptr if no match.
MediaTrack* FindTrackByName(const std::string& name) {
  const auto& api = g_bridge.Api();
  if (!api.countTracks || !api.getTrack) return nullptr;
  const int trackCount = api.countTracks(nullptr);
  for (int i = 0; i < trackCount; ++i) {
    MediaTrack* track = api.getTrack(nullptr, i);
    if (!track) continue;
    if (ReadTrackName(track) == name) return track;
  }
  return nullptr;
}

// Find the existing "HoloRoll" track, or create one at the top of the
// project track list and name it. Returns nullptr if the track can't be
// created (API missing, etc.).
//
// v0.9.1 always created a new top track per call (anonymous); alpha.6
// switched to find-or-create-by-name so items + envelopes accumulate on a
// single persistent row instead of stacking new tracks. Risk: if the user
// has their own track literally named "HoloRoll" we'd hijack it — name is
// specific enough that this is acceptable.
MediaTrack* EnsureTrackOnTop() {
  if (MediaTrack* existing = FindTrackByName(kItemsTrackName)) return existing;

  const auto& api = g_bridge.Api();
  if (!api.insertTrackAtIndex || !api.getTrack ||
      !api.getSetMediaTrackInfo_String) {
    return nullptr;
  }

  // Insert at index 0 (top). The new track becomes track 0; existing
  // tracks shift down by one.
  api.insertTrackAtIndex(0, true);
  MediaTrack* track = api.getTrack(nullptr, 0);
  if (!track) return nullptr;

  // Set the name so subsequent calls find it by name. P_NAME setter wants
  // a writable buffer.
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s", kItemsTrackName);
  api.getSetMediaTrackInfo_String(track, "P_NAME", buf, true);

  return track;
}

// Insert the holoroll_motion JSFX on the given track if it isn't already
// there. Returns the FX index on success, or -1 on failure (e.g. JSFX
// not installed, REAPER hasn't rescanned plug-ins).
//
// We try multiple name forms (see kMotionFxNameCandidates) because
// REAPER's JSFX name-resolution behaviour for TrackFX_AddByName varies
// between versions and on whether the JSFX lives in a subfolder.
//
// instantiate=-1 means "add only if not already present, return existing
// index if found." Idempotent: calling repeatedly is safe and a no-op
// after the first successful insert.
int EnsureMotionFx(MediaTrack* track) {
  if (!track) return -1;
  const auto& api = g_bridge.Api();
  if (!api.trackFX_AddByName) return -1;

  // First pass: query-only (instantiate=0) for each candidate name. If
  // any returns >=0, the FX is already on the track — we're done.
  for (const char* name : kMotionFxNameCandidates) {
    const int existing = api.trackFX_AddByName(track, name, false, 0);
    if (existing >= 0) return existing;
  }

  // Second pass: actually add. instantiate=-1 = add-or-find. The first
  // candidate that succeeds is our winner.
  for (const char* name : kMotionFxNameCandidates) {
    const int idx = api.trackFX_AddByName(track, name, false, -1);
    if (idx >= 0) return idx;
  }
  return -1;
}

// ---- v0.12.0-alpha.5/.6/.7 motion envelope writing ----------------------
//
// Pipeline:
//   1. EnsureItemsTrackAndFx()                  -> (track, fxIdx)
//   2. For each placed item, pick top-N active world bones, compute the
//      item's shared min/max across those bones, then for each:
//      WriteMotionEnvelopeForBone(..., sharedMin, sharedMax)
//   3. WriteMotionEnvelopeForBone surgically clears [item.start, item.end]
//      before writing, then flips VIS=1 on the envelope chunk so the
//      lane appears immediately (alpha.7 — REAPER otherwise leaves the
//      envelope hidden until the user opens the FX dialog).
//
// Slider semantics:
//   slider 1 = top-1 most-active world-motion bone of THIS item
//   slider 2 = top-2 same
//   slider 3 = top-3 same
// Different items may map different bones onto the same slider; that's by
// design. Sliders 4..16 are reserved for future uses (local motion,
// manual selection).
//
// Normalisation history:
//   - alpha.5: per-bone peak. Every slider hit 1.0 regardless of how
//     active its bone actually was — visually all three were identical
//     and made comparison impossible.
//   - alpha.6: shared peak across the item's selected bones. Preserved
//     relative magnitudes (slider 1 highest, 2/3 smaller) but values
//     clustered near 1.0 when motion was relatively uniform — curve
//     looked flat, dynamic range wasted.
//   - alpha.7: shared min/max stretching. Slider 1 spans the full
//     [0, 1] range; sliders 2/3 stay in proportion to bone 1 but use
//     the same scale. Envelope visually fills the lane and reads
//     usefully even when the underlying motion is quasi-uniform.
constexpr int kMotionTopN = 3;  // sliders 1..3 used; 4..16 reserved.
// JSFX slider numbers are 1-based in the .jsfx file but the FX parameter
// indices we pass to GetFXEnvelope are 0-based. slider1 == paramIdx 0.
constexpr int kMotionFirstParamIdx = 0;

// Combined wrapper used by all placement paths: find-or-create the
// "HoloRoll" track and ensure the holoroll_motion JSFX is on it. Returns
// {track, fxIdx}; track is nullptr only if track creation itself failed.
// If the track succeeds but the JSFX fails to load (e.g. user hasn't
// rescanned plug-ins after install), out.fxIdx stays -1 — callers can
// still place items, just no envelopes.
struct MotionFxLocation {
  MediaTrack* track = nullptr;
  int fxIdx = -1;
};
MotionFxLocation EnsureItemsTrackAndFx() {
  MotionFxLocation out;
  out.track = EnsureTrackOnTop();
  if (!out.track) return out;
  out.fxIdx = EnsureMotionFx(out.track);
  return out;
}

// Pick the top-N most-active bones from a motion vector (worldMotion or
// localMotion). Sorted descending by absolute-sum of per-frame motion.
// Returns indices into `motion`. Bones with zero motion are excluded.
//
// alpha.8: rank by sum of |motion[f]|, not raw sum, because the new
// signed-projection metric can have sum ≈ 0 for symmetric oscillation
// (positive frames cancel negative ones), which would mark active
// bones as static. Sum of absolutes is order-of-magnitude equivalent
// to total displacement and works for both signed and unsigned data.
std::vector<std::size_t> TopNActiveBones(const std::vector<std::vector<float>>& motion,
                                          std::size_t topN) {
  struct Pair { std::size_t idx; float total; };
  std::vector<Pair> rank;
  rank.reserve(motion.size());
  for (std::size_t j = 0; j < motion.size(); ++j) {
    float sum = 0.0f;
    for (float v : motion[j]) sum += std::fabs(v);
    if (sum > 1e-6f) rank.push_back({j, sum});
  }
  if (rank.empty()) return {};
  // Partial-sort top-N descending.
  const std::size_t take = std::min(topN, rank.size());
  std::partial_sort(rank.begin(), rank.begin() + take, rank.end(),
                    [](const Pair& a, const Pair& b) { return a.total > b.total; });
  std::vector<std::size_t> out;
  out.reserve(take);
  for (std::size_t i = 0; i < take; ++i) out.push_back(rank[i].idx);
  return out;
}

// v0.12.0-alpha.7: flip the envelope's VIS flag to 1 so the lane shows
// up on the track without the user having to open the FX window.
// GetFXEnvelope(create=true) creates the envelope but defaults VIS to 0
// — the data is there, the lane just isn't drawn. We modify the state
// chunk's "VIS X Y Z" line in place (X = visibility, Y = lane in track
// view, Z = height factor) and write it back.
//
// alpha.7 hotfix details (lessons-learned):
//   - 1MB buffer (64KB was too small for accumulated envelopes).
//   - VIS line search is CR/LF agnostic (REAPER on Windows uses CRLF).
//   - Insert a fresh VIS line after <PARMENV if the chunk doesn't have
//     one yet — terse newly-created envelopes lack it.
//   - ALWAYS overwrite VIS, even if it's already "1 1 1": empirically
//     the SetEnvelopeStateChunk call itself is what triggers REAPER's
//     redraw. Skipping the rewrite hides the visibility fix.
bool FindLineStart(const std::string& s, const std::string& token, std::size_t& outPos) {
  std::size_t p = 0;
  while (true) {
    p = s.find(token, p);
    if (p == std::string::npos) return false;
    if (p == 0 || s[p - 1] == '\n' || s[p - 1] == '\r') {
      outPos = p;
      return true;
    }
    ++p;
  }
}

void EnsureEnvelopeVisible(TrackEnvelope* env, int paramIdx) {
  const auto& api = g_bridge.Api();
  if (!env || !api.getEnvelopeStateChunk || !api.setEnvelopeStateChunk) return;

  static constexpr int kBuf = 1024 * 1024;  // 1 MB — handles big chunks.
  std::vector<char> buf(kBuf);
  if (!api.getEnvelopeStateChunk(env, buf.data(), kBuf, /*isundo=*/false)) {
    SpikeLog("[holoroll] envelope p" + std::to_string(paramIdx) +
             ": GetEnvelopeStateChunk failed (chunk > 1MB?)\n");
    return;
  }

  std::string s(buf.data());
  std::size_t visPos;
  if (FindLineStart(s, "VIS ", visPos)) {
    // Always overwrite VIS line — empirically the SetEnvelopeStateChunk
    // call itself is what triggers REAPER to redraw the lane, even when
    // VIS was already 1. An "already 1, skip" optimisation here hid the
    // visibility fix during alpha.7 testing.
    std::size_t lineEnd = visPos;
    while (lineEnd < s.size() && s[lineEnd] != '\r' && s[lineEnd] != '\n') ++lineEnd;
    s.replace(visPos, lineEnd - visPos, "VIS 1 1 1.0");
  } else {
    std::size_t hdrEnd = s.find('\n');
    if (hdrEnd == std::string::npos) return;
    s.insert(hdrEnd + 1, "VIS 1 1 1.0\n");
  }

  if (!api.setEnvelopeStateChunk(env, s.c_str(), /*isundo=*/false)) {
    SpikeLog("[holoroll] envelope p" + std::to_string(paramIdx) +
             ": SetEnvelopeStateChunk failed\n");
  }
}

// Write one bone's motion curve into one FX-parameter envelope.
//
// Pipeline as of alpha.8:
//   - Caller passes shared min/max bounds (5th and 95th percentile of
//     the item's selected-bone motion values). We map every frame via
//     (motion[f] - sharedMin) / (sharedMax - sharedMin), clamped to
//     [0, 1]. Outliers below p5 or above p95 hit the clamp limits.
//   - motion[f] is the SIGNED principal-axis projection produced by
//     glb_loader's projectSigned post-process (alpha.8): one sine wave
//     per actual oscillation, not the rectified 2x metric we used
//     before.
//   - Frame 0 is now valid data (no bake artifact), so no forward-fill.
//   - Surgical clear of [item.start, item.end] before writing.
//   - Calls EnsureEnvelopeVisible after writing so the lane shows up
//     without the user manually opening the FX window.
// Linear shape (shape=0). Returns false on missing API or bad inputs.
bool WriteMotionEnvelopeForBone(MediaTrack* track, int fxIdx, int paramIdx,
                                const std::vector<float>& motion,
                                double itemStartSec, double fps,
                                float sharedMin, float sharedMax) {
  const auto& api = g_bridge.Api();
  if (!api.getFXEnvelope || !api.insertEnvelopePoint ||
      !api.envelope_SortPoints || !api.deleteEnvelopePointRange) {
    return false;
  }
  if (!track || fxIdx < 0 || motion.empty() || fps <= 0.0) return false;

  TrackEnvelope* env = api.getFXEnvelope(track, fxIdx, paramIdx, /*create=*/true);
  if (!env) return false;

  const std::size_t nFrames = motion.size();
  const double durationSec = static_cast<double>(nFrames) / fps;
  const double clearStart = itemStartSec;
  const double clearEnd = itemStartSec + durationSec;

  // Wipe the target region first.
  api.deleteEnvelopePointRange(env, clearStart, clearEnd);

  // Min-max stretch denominator. If degenerate (all selected bones
  // static or all frames identical), write 0s — nothing meaningful to
  // display.
  const float range = sharedMax - sharedMin;
  const bool degenerate = range < 1e-6f;
  const float invRange = degenerate ? 0.0f : (1.0f / range);

  // alpha.8: motion is now SIGNED projection onto the joint's principal
  // axis (computed in glb_loader's post-process pass). Frame 0 has a
  // valid value (no longer the spurious 0 from rectified |speed|), so
  // the alpha.6/.7 forward-fill workaround is gone.

  // Batch-insert all points with noSortIn=true; sort once at the end.
  bool noSort = true;
  for (std::size_t f = 0; f < nFrames; ++f) {
    const double t = itemStartSec + static_cast<double>(f) / fps;
    // Min-max stretch into [0, 1]. Clamp defensively for outlier
    // frames (those below 5th or above 95th percentile).
    double v = degenerate ? 0.0
                          : static_cast<double>((motion[f] - sharedMin) * invRange);
    if (v < 0.0) v = 0.0;
    else if (v > 1.0) v = 1.0;
    api.insertEnvelopePoint(env, t, v, /*shape=*/0, /*tension=*/0.0,
                            /*selected=*/false, &noSort);
  }
  api.envelope_SortPoints(env);
  // alpha.7: make the envelope lane visible immediately.
  EnsureEnvelopeVisible(env, paramIdx);
  return true;
}

// For one placed item, write top-3 world-motion envelopes onto sliders
// 1..3. All three sliders share the SAME min/max bounds (computed
// across the three selected bones for this item), so:
//   - the envelope visually spans the slider's full [0, 1] range
//     (alpha.6's pure-peak normalisation clustered values near 1 when
//     motion was relatively uniform — alpha.7 stretches dynamic range
//     to the slider edges);
//   - relative magnitudes between bones are still preserved (slider 1
//     reaches the top, sliders 2/3 stay proportional within the same
//     scale).
//
// Note: frame 0 is handled inside WriteMotionEnvelopeForBone (forward-
// filled with motion[1]) but is NOT included in min/max computation
// here — its value is 0 from the bake loop and would skew sharedMin
// downward by exactly that artefact we're trying to ignore.
void WriteMotionEnvelopesForItem(MediaTrack* motionTrack, int fxIdx,
                                 const LoadedAnimation& anim,
                                 double itemStartSec, double fps) {
  if (anim.worldMotion.empty()) return;
  const std::vector<std::size_t> topBones = TopNActiveBones(anim.worldMotion, kMotionTopN);
  if (topBones.empty()) return;

  // alpha.7 hotfix: percentile-based bounds instead of raw min/max.
  // Empirically rest-pose frames (motion=0 at start of animation) pulled
  // sharedMin to 0 and the min-max stretch collapsed back to peak
  // normalisation. 5th/95th percentiles skip those outliers and give
  // the bulk of the curve usable dynamic range. Frames outside the
  // bounds get clamped to [0, 1] in WriteMotionEnvelopeForBone.
  std::vector<float> all;
  for (std::size_t boneIdx : topBones) {
    const auto& m = anim.worldMotion[boneIdx];
    for (std::size_t f = 1; f < m.size(); ++f) {  // skip frame 0
      all.push_back(m[f]);
    }
  }
  if (all.empty()) return;
  std::sort(all.begin(), all.end());

  const std::size_t loIdx = (all.size() * 5) / 100;
  const std::size_t hiIdx = (all.size() * 95) / 100;
  const float sharedMin = all[std::min(loIdx, all.size() - 1)];
  const float sharedMax = all[std::min(hiIdx, all.size() - 1)];
  if (!std::isfinite(sharedMin) || !std::isfinite(sharedMax)) return;

  for (std::size_t k = 0; k < topBones.size() && k < static_cast<std::size_t>(kMotionTopN); ++k) {
    const std::size_t boneIdx = topBones[k];
    const int paramIdx = kMotionFirstParamIdx + static_cast<int>(k);
    WriteMotionEnvelopeForBone(motionTrack, fxIdx, paramIdx,
                               anim.worldMotion[boneIdx],
                               itemStartSec, fps,
                               sharedMin, sharedMax);
  }
}

// v0.11.1: build a variation name with zero-padded 2-digit suffix.
// Index 1 returns the bare basename (no suffix). Indices 2,3,...,9 produce
// `_02`, `_03` ... `_09`. Index 10+ produces `_10`, `_11` ... naturally.
//
// Why padded: 1) sort-friendly in any file/region listing (`_02` < `_10`
// alphabetically), 2) visual consistency in the timeline label.
// ResolveAnimationByItemName already strips any `_<digits>` suffix when
// matching against the library, so `_02` resolves to the same animation
// as `_2` would have — backward-compat with v0.11.0 timelines is fine.
std::string MakeVariationName(const std::string& basename, int variationIndex) {
  if (variationIndex <= 1) return basename;
  char buf[8];
  // %02d pads to two digits with leading zero; for 10+ it just prints normally.
  std::snprintf(buf, sizeof(buf), "_%02d", variationIndex);
  return basename + buf;
}

// v0.11.1 (refactored): create one named item with length = animation
// duration ONLY. Pre/post-roll are no longer baked into item length —
// they're global playback-time visualisations now (current settings,
// stored in config), so the item itself is clean.
//
// The preRollSec/postRollSec parameters are kept in the signature for
// source compatibility but are unused inside. Returns the created item
// pointer for further customisation by callers (e.g. region creation).
MediaItem* CreateNamedItemWithRolls(MediaTrack* track, double position,
                                    double animationDuration,
                                    float /*preRollSec*/, float /*postRollSec*/,
                                    const std::string& name) {
  if (!track) return nullptr;
  const auto& api = g_bridge.Api();
  if (!api.addMediaItemToTrack || !api.setMediaItemInfo_Value ||
      !api.addTakeToMediaItem || !api.getSetMediaItemTakeInfo_String) {
    return nullptr;
  }

  MediaItem* item = api.addMediaItemToTrack(track);
  if (!item) return nullptr;
  api.setMediaItemInfo_Value(item, "D_POSITION", position);
  api.setMediaItemInfo_Value(item, "D_LENGTH", std::max(0.001, animationDuration));

  MediaItem_Take* take = api.addTakeToMediaItem(item);
  if (take) {
    char buf[kItemNameBufferSize];
    std::snprintf(buf, sizeof(buf), "%s", name.c_str());
    api.getSetMediaItemTakeInfo_String(take, "P_NAME", buf, true);
  }

  MarkAsHoloRollItem(item);
  // No more per-item pre/post-roll P_EXT — globals control playback now.
  return item;
}

// Legacy single-arg form. Kept so SpikeTestCreateItem (and any future
// caller that doesn't care about hold frames) doesn't have to thread
// pre/post-roll through. New placement code paths use
// CreateNamedItemWithRolls directly.
bool CreateNamedItem(MediaTrack* track, double position, double length, const std::string& name) {
  return CreateNamedItemWithRolls(track, position, length, 0.0f, 0.0f, name) != nullptr;
}

// One item discovered on the timeline, normalised to what ResolvePlayhead
// needs. trackIndex preserves source order: lower index = higher in REAPER's
// track list = wins resolution conflicts.
struct DiscoveredItem {
  int trackIndex = 0;
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  std::string name;
  // v0.11.0: hold-frame buffers stamped into the item via P_EXT at
  // placement time. Both default to 0 so items created in v0.10.x and
  // earlier behave exactly as before — no hold frames, frame 0 starts at
  // item start.
  float preRollSec = 0.0f;
  float postRollSec = 0.0f;
};

// Walk every track in the project, then every item on each track, and return
// items that have a non-empty name AND carry our P_EXT marker. The marker
// check (added in v0.9.1) prevents accidental matches with audio/midi/video
// items the user happens to have named the same as one of our animations.
std::vector<DiscoveredItem> EnumProjectItems() {
  std::vector<DiscoveredItem> out;
  const auto& api = g_bridge.Api();
  if (!api.countTracks || !api.getTrack || !api.countTrackMediaItems ||
      !api.getTrackMediaItem || !api.getMediaItemInfo_Value) {
    return out;
  }

  const int trackCount = api.countTracks(nullptr);
  for (int t = 0; t < trackCount; ++t) {
    MediaTrack* track = api.getTrack(nullptr, t);
    if (!track) continue;
    const int itemCount = api.countTrackMediaItems(track);
    for (int i = 0; i < itemCount; ++i) {
      MediaItem* item = api.getTrackMediaItem(track, i);
      if (!item) continue;
      if (!IsHoloRollItem(item)) continue;
      const std::string name = ReadItemName(item);
      if (name.empty()) continue;
      const double pos = api.getMediaItemInfo_Value(item, "D_POSITION");
      const double len = api.getMediaItemInfo_Value(item, "D_LENGTH");
      DiscoveredItem di;
      di.trackIndex = t;
      di.startSeconds = pos;
      di.endSeconds = pos + len;
      di.name = name;
      // v0.11.1: per-item pre/post-roll deprecated. Items created in
      // v0.11.0 still have the P_EXT keys but we ignore them — the
      // global current settings drive playback now.
      di.preRollSec = 0.0f;
      di.postRollSec = 0.0f;
      out.push_back(std::move(di));
    }
  }
  return out;
}

// v0.12.0-alpha.10: latest end-time across all HoloRoll items. Replaces
// FindLastRegionEnd as the "where does the next item go" anchor — since
// alpha.10 we stopped creating regions, region-end is no longer a usable
// proxy for "end of placed content".
double FindLastHoloRollItemEnd() {
  const auto items = EnumProjectItems();
  double last = 0.0;
  for (const auto& di : items) {
    if (di.endSeconds > last) last = di.endSeconds;
  }
  return last;
}

// Resolution result. The two flags help OnTimer decide what to surface in
// the overlay: missing means "item exists but I can't find that animation
// in the library", which is a useful warning state distinct from "no
// items at all" (foundAny=false).
struct ItemResolveResult {
  std::size_t animationIndex = std::numeric_limits<std::size_t>::max();
  std::uint32_t frameIndex = 0;
  double itemStart = 0.0;
  double itemEnd = 0.0;
  std::string missingAnimationName;  // Non-empty iff item found but anim missing.
  bool foundAny = false;             // True if any item overlapped the playhead.
};

// Resolve which item is under the playhead. Walks tracks top-down (lower
// trackIndex = higher in REAPER UI = wins). The matching window is
// expanded by the GLOBAL pre/post-roll settings so the playhead "finds"
// an item even when sitting in the visual buffer zone before/after.
//
// v0.11.1 hold-frame semantics (item itself stays clean, no extension):
//   |--globalPreRoll--|====item====|--globalPostRoll--|
//      shows frame 0   normal play   shows last frame
//
// The item.start ... item.end range still maps 1:1 to animation time;
// the pre-roll just extends WHERE we'll show frame 0, and post-roll
// extends WHERE we'll show the last frame.
ItemResolveResult ResolvePlayheadFromItems(double playheadSeconds,
                                           double fps,
                                           const std::vector<DiscoveredItem>& items,
                                           float globalPreRollSec,
                                           float globalPostRollSec) {
  ItemResolveResult result;

  // Tracks are visited in order, so the first overlap we hit is by
  // definition the topmost track's item.
  for (const auto& di : items) {
    // Match window expanded by global pre/post-roll — these are the
    // visual buffers where we still want to show frame 0 / last frame.
    const double matchStart = di.startSeconds - static_cast<double>(globalPreRollSec);
    const double matchEnd   = di.endSeconds   + static_cast<double>(globalPostRollSec);
    if (playheadSeconds < matchStart || playheadSeconds > matchEnd) continue;

    result.foundAny = true;
    result.itemStart = di.startSeconds;
    result.itemEnd = di.endSeconds;

    const std::size_t animIdx = g_lib.ResolveAnimationByItemName(di.name);
    if (animIdx == std::numeric_limits<std::size_t>::max()) {
      result.missingAnimationName = di.name;
      return result;
    }

    result.animationIndex = animIdx;
    const LoadedAnimation& anim = g_lib.At(animIdx);
    const std::uint32_t totalFrames = anim.TotalFrames();
    if (totalFrames == 0) {
      result.frameIndex = 0;
    } else {
      // Localise to item-start time. Negative -> still in pre-roll buffer
      // (clamp to frame 0). Beyond duration -> in post-roll buffer
      // (clamp to last frame).
      const double localTime = playheadSeconds - di.startSeconds;
      const double lastFrame = static_cast<double>(totalFrames - 1);
      double f = std::floor(localTime * fps);
      if (f < 0.0) f = 0.0;
      if (f > lastFrame) f = lastFrame;
      result.frameIndex = static_cast<std::uint32_t>(f);
    }
    return result;
  }

  return result;
}

// v0.12.0-alpha.10: place one media item per animation on the persistent
// HoloRoll track. Auto-region creation was removed in alpha.10 — see
// CHANGELOG. De-duplicates against existing HoloRoll items: if an item
// with the exact same name already exists anywhere on the timeline, it
// gets skipped (no duplicate).
//
// Item length = animation duration. Pre/post-roll are global playback-
// time visual buffers, not part of item geometry. The cursor advances
// by `duration + gap` between items.
void PlaceOurItemsAndRegions(MediaTrack* /*ignored*/, double /*ignored*/) {
  const auto& api = g_bridge.Api();
  if (!api.updateArrange) {
    SpikeLog("[holoroll] cannot place items: REAPER API incomplete.\n");
    return;
  }
  // Wipe any legacy HoloRoll regions left over from < alpha.10 projects.
  // No new regions get created from this point on, so on a fresh project
  // this is a no-op.
  DeleteOurRegions();

  // Snapshot existing item names BEFORE creating the new track. We don't
  // want our own freshly-empty top track to count as "existing".
  std::vector<std::string> existingNames;
  {
    const auto items = EnumProjectItems();
    existingNames.reserve(items.size());
    for (const auto& di : items) existingNames.push_back(di.name);
  }
  auto alreadyPlaced = [&](const std::string& name) {
    for (const auto& n : existingNames) {
      if (n == name) return true;
    }
    return false;
  };

  // v0.12.0-alpha.6: items, JSFX and envelopes all live on the same
  // persistent "HoloRoll" track now. Find-or-create + add JSFX in one go.
  MotionFxLocation loc = EnsureItemsTrackAndFx();
  MediaTrack* targetTrack = loc.track;
  if (!targetTrack) {
    SpikeLog("[holoroll] cannot place items: failed to create track.\n");
    return;
  }

  const double fps = GetFps();
  const double gap = GetGap();

  // Start after the last placed HoloRoll item, so re-running Place all
  // appends new content past existing items instead of overlapping.
  const double lastEnd = FindLastHoloRollItemEnd();
  double cursor = (lastEnd > 0.0) ? (lastEnd + gap) : 0.0;

  std::size_t placed = 0;
  std::size_t skipped = 0;

  for (std::size_t i = 0; i < g_lib.Count(); ++i) {
    const LoadedAnimation& anim = g_lib.At(i);
    const double duration = anim.DurationSeconds(fps);
    if (duration <= 0.0) continue;

    // alpha.10: variations setting removed — one item per animation.
    const std::string& itemName = anim.basename;
    if (alreadyPlaced(itemName)) {
      ++skipped;
      continue;
    }
    MediaItem* item = CreateNamedItemWithRolls(targetTrack, cursor, duration,
                                               0.0f, 0.0f, itemName);
    if (item) {
      // v0.12.0-alpha.5/.6: write motion envelopes for this item.
      // Surgical clear in [item.start, item.end] keeps envelopes for
      // previously-placed items intact (track is persistent).
      if (loc.fxIdx >= 0) {
        WriteMotionEnvelopesForItem(targetTrack, loc.fxIdx,
                                    anim, cursor, fps);
      }
      ++placed;
      existingNames.push_back(itemName);
      cursor += duration + gap;
    }
  }

  api.updateArrange();
  ConsoleLog("[holoroll] placed " + std::to_string(placed) +
             " item(s) (skipped " + std::to_string(skipped) + " already-placed).\n");
}

// v0.12.0-alpha.10: place items for animations newly detected by the
// folder watcher. Auto-called from ProcessWatcherEvents (no modal anymore).
// De-dups against existing items. No regions, no variations.
void PlacePendingAtCursor() {
  if (g_pendingNewAnimations.empty()) return;
  const auto& api = g_bridge.Api();

  // Snapshot existing item names so we don't duplicate.
  std::vector<std::string> existingNames;
  {
    const auto items = EnumProjectItems();
    existingNames.reserve(items.size());
    for (const auto& di : items) existingNames.push_back(di.name);
  }
  auto alreadyPlaced = [&](const std::string& name) {
    for (const auto& n : existingNames) {
      if (n == name) return true;
    }
    return false;
  };

  // v0.12.0-alpha.6: items + JSFX on the same persistent "HoloRoll" track.
  MotionFxLocation loc = EnsureItemsTrackAndFx();
  MediaTrack* track = loc.track;
  if (!track) {
    SpikeLog("[holoroll] cannot place: failed to create track.\n");
    g_pendingNewAnimations.clear();
    return;
  }

  const double fps = GetFps();
  const double gap = GetGap();

  const double lastEnd = FindLastHoloRollItemEnd();
  double pos = (lastEnd > 0.0) ? (lastEnd + gap) : 0.0;

  std::size_t placed = 0;
  std::size_t skipped = 0;

  for (const std::string& basename : g_pendingNewAnimations) {
    const std::size_t idx = g_lib.FindAnimationIndexByBasename(basename);
    if (idx == std::numeric_limits<std::size_t>::max()) continue;
    const LoadedAnimation& anim = g_lib.At(idx);
    const double duration = anim.DurationSeconds(fps);
    if (duration <= 0.0) continue;

    if (alreadyPlaced(basename)) {
      ++skipped;
      continue;
    }
    MediaItem* item = CreateNamedItemWithRolls(track, pos, duration,
                                               0.0f, 0.0f, basename);
    if (item) {
      if (loc.fxIdx >= 0) {
        WriteMotionEnvelopesForItem(track, loc.fxIdx, anim, pos, fps);
      }
      ++placed;
      existingNames.push_back(basename);
      pos += duration + gap;
    }
  }

  if (api.updateArrange) api.updateArrange();
  ConsoleLog("[holoroll] hot-reload: placed " + std::to_string(placed) +
             " item(s), skipped " + std::to_string(skipped) + " already-placed.\n");
  g_pendingNewAnimations.clear();
}

// v0.12.0-alpha.10: create one item for a single named animation
// ("+ Place" button next to a library row). No region, no variation.
void PlaceSingleAtCursor(const std::string& basename) {
  const auto& api = g_bridge.Api();

  // v0.12.0-alpha.6: items + JSFX on the same persistent "HoloRoll" track.
  MotionFxLocation loc = EnsureItemsTrackAndFx();
  MediaTrack* track = loc.track;
  if (!track) return;

  const std::size_t idx = g_lib.FindAnimationIndexByBasename(basename);
  if (idx == std::numeric_limits<std::size_t>::max()) return;
  const LoadedAnimation& anim = g_lib.At(idx);
  const double duration = anim.DurationSeconds(GetFps());
  if (duration <= 0.0) return;

  const double lastEnd = FindLastHoloRollItemEnd();
  const double pos = (lastEnd > 0.0) ? (lastEnd + GetGap()) : 0.0;

  MediaItem* item = CreateNamedItemWithRolls(track, pos, duration,
                                             0.0f, 0.0f, basename);
  if (item && loc.fxIdx >= 0) {
    WriteMotionEnvelopesForItem(track, loc.fxIdx, anim, pos, GetFps());
  }
  if (api.updateArrange) api.updateArrange();
}

// ---- v0.12.0-alpha.9 motion-event marker generation -----------------------
//
// Walk every placed HoloRoll item, run the active motion-event detector on
// the item's top-1 active world-motion bone, and write REAPER project
// markers at the detected event times.
//
// Marker naming: "<itemname>:<eventtype>", e.g. "door_open:start",
// "door_open:peak_hi", "door_open:zero_cross". The "<itemname>:" prefix is
// what we use as the surgical-clear key — re-running the action wipes
// previous HoloRoll markers in each item's range and rewrites them
// without disturbing user-created markers anywhere else on the timeline.
//
// Uses project markers (isrgn=false), not regions — regions are reserved
// for the placement workflow and have a different visual lane in REAPER.

// Helper: delete project markers whose name starts with `namePrefix` and
// whose time falls inside [startSec, endSec]. We delete in two phases
// (collect indices first, then call DeleteProjectMarker) so we don't
// invalidate the marker enumeration mid-walk.
void DeleteHoloRollMarkersInRange(double startSec, double endSec,
                                  const std::string& namePrefix) {
  const auto& api = g_bridge.Api();
  if (!api.enumProjectMarkers3 || !api.deleteProjectMarker) return;

  std::vector<int> toDelete;
  int idx = 0;
  while (true) {
    bool isrgn = false;
    double pos = 0.0, rgnend = 0.0;
    const char* name = nullptr;
    int markrgnindexnumber = 0;
    int color = 0;
    const int next = api.enumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend,
                                             &name, &markrgnindexnumber, &color);
    if (next == 0) break;
    if (!isrgn && pos >= startSec && pos <= endSec) {
      const std::string nm = name ? name : "";
      if (nm.rfind(namePrefix, 0) == 0) {
        toDelete.push_back(markrgnindexnumber);
      }
    }
    idx = next;
  }
  // Third arg to DeleteProjectMarker: the marker/region index to remove.
  // Note: marker indices are stable IDs assigned by REAPER, not loop
  // positions, so deleting them in any order is safe.
  for (const int id : toDelete) {
    api.deleteProjectMarker(nullptr, id, false);  // false = marker, not region
  }
}

void GenerateAllMotionMarkers() {
  const auto& api = g_bridge.Api();
  if (!api.addProjectMarker2 || !api.enumProjectMarkers3 ||
      !api.deleteProjectMarker) {
    SpikeLog("[holoroll] Generate motion markers: required REAPER APIs missing.\n");
    return;
  }

  const auto items = EnumProjectItems();
  if (items.empty()) {
    ConsoleLog("[holoroll] Generate motion markers: no HoloRoll items placed yet.\n");
    return;
  }

  // For now: hardcoded default detector. alpha.10 will add a UI dropdown
  // to pick from AllMotionEventDetectorNames().
  const IMotionEventDetector* detector = DefaultMotionEventDetector();
  if (!detector) {
    SpikeLog("[holoroll] Generate motion markers: no detectors registered.\n");
    return;
  }

  const double fps = GetFps();
  const int markerColor = AnimationLibrary::RegionColorReaper();

  std::size_t totalEvents = 0;
  std::size_t itemsProcessed = 0;
  std::size_t itemsSkipped = 0;

  for (const DiscoveredItem& di : items) {
    const std::size_t animIdx = g_lib.ResolveAnimationByItemName(di.name);
    if (animIdx == std::numeric_limits<std::size_t>::max()) {
      ++itemsSkipped;
      continue;
    }
    const LoadedAnimation& anim = g_lib.At(animIdx);
    if (anim.worldMotion.empty()) {
      ++itemsSkipped;
      continue;
    }

    // Top-1 active bone: same selection metric as envelope writing
    // uses for slider 1, so markers and envelope share the same
    // "primary bone" view. (Multi-bone marker generation is alpha.10.)
    const auto topBones = TopNActiveBones(anim.worldMotion, 1);
    if (topBones.empty()) {
      ++itemsSkipped;
      continue;
    }
    const std::vector<float>& motion = anim.worldMotion[topBones[0]];

    // Surgical clear: scrub previous HoloRoll markers for THIS item only.
    const std::string namePrefix = di.name + ":";
    DeleteHoloRollMarkersInRange(di.startSeconds, di.endSeconds, namePrefix);

    // Run detector and write markers.
    const auto events = detector->Detect(motion, di.startSeconds, fps);
    for (const MotionEvent& ev : events) {
      const std::string markerName = di.name + ":" + ev.eventType;
      // For a marker, REAPER's AddProjectMarker2 ignores rgnend; pass 0.
      api.addProjectMarker2(nullptr, /*isrgn=*/false,
                            ev.timeSec, 0.0,
                            markerName.c_str(), -1, markerColor);
    }

    totalEvents += events.size();
    ++itemsProcessed;
  }

  if (api.updateArrange) api.updateArrange();
  ConsoleLog("[holoroll] Generated " + std::to_string(totalEvents) +
             " motion marker(s) across " + std::to_string(itemsProcessed) +
             " item(s) using detector '" + detector->Name() +
             "' (skipped " + std::to_string(itemsSkipped) + " item(s) without resolvable animation).\n");
}

// ---- v0.6.0 spike: create an empty named item on a track ------------------
//
// Validates the REAPER API path we plan to use for the items workflow:
//   1. Find a target track (first selected, or first track if none selected).
//   2. AddMediaItemToTrack       → empty item.
//   3. SetMediaItemInfo_Value    → D_POSITION + D_LENGTH.
//   4. AddTakeToMediaItem        → take needed for naming.
//   5. GetSetMediaItemTakeInfo_String P_NAME → set the take's display name.
//   6. UpdateArrange             → force REAPER to repaint the timeline so
//                                  the new item is visible immediately.
//
// alpha.13: output goes via SpikeLog, which is gated by the runtime
// g_debugEnabled flag. To see the spike's report, turn on "Debug log"
// in the overlay's Config section before pressing the spike button.
// If any of the API pointers are missing the helper bails (logged
// only when debug is on) and the rest of the plugin keeps working.
void SpikeTestCreateItem() {
  const auto& api = g_bridge.Api();

  // Phase 1: required API symbols.
  struct ApiCheck { const char* name; const void* fn; };
  const ApiCheck required[] = {
    {"AddMediaItemToTrack",          reinterpret_cast<const void*>(api.addMediaItemToTrack)},
    {"AddTakeToMediaItem",           reinterpret_cast<const void*>(api.addTakeToMediaItem)},
    {"SetMediaItemInfo_Value",       reinterpret_cast<const void*>(api.setMediaItemInfo_Value)},
    {"GetSetMediaItemTakeInfo_String",reinterpret_cast<const void*>(api.getSetMediaItemTakeInfo_String)},
    {"UpdateArrange",                reinterpret_cast<const void*>(api.updateArrange)},
  };
  for (const auto& ck : required) {
    if (ck.fn == nullptr) {
      SpikeLog(std::string("[holoroll-spike] FAIL: missing REAPER API: ") + ck.name + "\n");
      return;
    }
  }

  // Phase 2: pick a track. Prefer first selected; fall back to first track.
  MediaTrack* track = nullptr;
  if (api.getSelectedTrack) {
    track = api.getSelectedTrack(nullptr, 0);
    if (track) SpikeLog("[holoroll-spike] using first selected track\n");
  }
  if (!track && api.getTrack) {
    track = api.getTrack(nullptr, 0);
    if (track) SpikeLog("[holoroll-spike] no selected track; using track 0\n");
  }
  if (!track) {
    SpikeLog("[holoroll-spike] FAIL: no tracks in project. Add a track and retry.\n");
    return;
  }

  // Phase 3: pick a position. Use the play cursor position if available,
  // otherwise 0. Length is 2 seconds (arbitrary, just visible).
  const double position = api.getCursorPositionEx ? api.getCursorPositionEx(nullptr) : 0.0;
  constexpr double length = 2.0;

  // Phase 4: create the item.
  MediaItem* item = api.addMediaItemToTrack(track);
  if (!item) {
    SpikeLog("[holoroll-spike] FAIL: AddMediaItemToTrack returned null\n");
    return;
  }
  api.setMediaItemInfo_Value(item, "D_POSITION", position);
  api.setMediaItemInfo_Value(item, "D_LENGTH", length);

  // Phase 5: add a take and set its name. Empty items show the take name as
  // their on-timeline label, so this is what the user will see.
  MediaItem_Take* take = api.addTakeToMediaItem(item);
  if (!take) {
    SpikeLog("[holoroll-spike] FAIL: AddTakeToMediaItem returned null\n");
    return;
  }
  // GetSetMediaItemTakeInfo_String wants a writable buffer even when setting.
  // Spec: pass the new value via a non-const buffer.
  char nameBuffer[256] = "holoroll_test";
  const bool nameSet = api.getSetMediaItemTakeInfo_String(take, "P_NAME", nameBuffer, true);
  if (!nameSet) {
    SpikeLog("[holoroll-spike] WARN: P_NAME set returned false (item created, name may be empty)\n");
  }

  // Phase 6: ask REAPER to redraw.
  api.updateArrange();

  char summary[512];
  std::snprintf(summary, sizeof(summary),
                "[holoroll-spike] OK: created item at %.3fs, length %.1fs, name=\"holoroll_test\"\n",
                position, length);
  SpikeLog(summary);
}

// Drain Incoming-folder events. When the burst settles, move all eligible
// files into the active project's Animations/. The project-folder watcher
// will then naturally pick up the new files and surface the
// "new animations" modal in OnTimer's regular flow. This means the user
// only sees ONE modal per drop, regardless of which folder it lands in.
//
// If no project is active (Untitled), files stay in Incoming/. They'll be
// drained when the user saves the project (OnProjectChanged calls
// DrainIncomingToProject too).
void ProcessIncomingWatcherEvents() {
  if (!g_incomingWatcher.IsRunning()) return;

  auto events = g_incomingWatcher.Drain();
  const ULONGLONG now = GetTickCount64();
  if (!events.empty()) {
    g_lastIncomingEventTick = now;
    g_incomingEventsAccumulating = true;
  }
  if (!g_incomingEventsAccumulating) return;
  if (now - g_lastIncomingEventTick < kWatcherDebounceMs) return;

  g_incomingEventsAccumulating = false;

  const std::size_t moved = DrainIncomingToProject();
  if (moved > 0) {
    ConsoleLog("[holoroll] Incoming: drained " + std::to_string(moved) +
               " file(s) into project.\n");
  }
}

// Drain folder-watcher events, debounce them, and once the burst settles
// (no new events for kWatcherDebounceMs), rescan the library and pick out
// genuinely new basenames. Those go into g_pendingNewAnimations; the modal
// in DrawNewAnimationsModal then asks the user what to do with them.
//
// Removed/renamed/modified events are noted in the console log but don't
// prompt the user — leaving stale regions is the safer default (the user
// can re-run "Place regions" to clean up).
void ProcessWatcherEvents() {
  if (!g_watcher.IsRunning()) return;
  // If a previous batch is still waiting on user action, don't pile on more.
  if (!g_pendingNewAnimations.empty()) return;

  auto events = g_watcher.Drain();
  const ULONGLONG now = GetTickCount64();
  if (!events.empty()) {
    g_lastWatcherEventTick = now;
    g_watcherEventsAccumulating = true;
  }

  // Wait for the burst to settle before rescanning. Without this, copying
  // 10 files into the folder would trigger 10 rescans in quick succession.
  if (!g_watcherEventsAccumulating) return;
  if (now - g_lastWatcherEventTick < kWatcherDebounceMs) return;

  g_watcherEventsAccumulating = false;

  // Snapshot the current basenames before rescan.
  std::vector<std::string> previousBasenames;
  previousBasenames.reserve(g_lib.Count());
  for (std::size_t i = 0; i < g_lib.Count(); ++i) {
    previousBasenames.push_back(g_lib.At(i).basename);
  }

  // Rescan. ScanFolder() rebuilds animations_ from scratch, so we have to
  // re-issue BuildRegions and clear pose state for any animations that have
  // disappeared. Existing per-animation pose data is keyed by index, which
  // is now invalid after rescan — clear all of it. (User-set camera angles
  // do reset for existing animations on hot-reload; tradeoff for simplicity.
  // Future: key g_poses by basename instead of index to preserve them.)
  std::string log;
  const std::string dir = g_lib.Directory();
  if (dir.empty()) return;
  g_lib.ScanFolder(dir, GetFps(), &log);
  g_lib.BuildRegions(GetFps(), GetGap(), 0.0);
  if (!log.empty()) ConsoleLog(log);

  g_poses.Clear();
  g_activeAnimIdx = kNoActiveAnim;

  // Diff: which basenames are new?
  std::vector<std::string> newBasenames;
  for (std::size_t i = 0; i < g_lib.Count(); ++i) {
    const std::string& bn = g_lib.At(i).basename;
    bool wasPresent = false;
    for (const auto& prev : previousBasenames) {
      if (prev == bn) { wasPresent = true; break; }
    }
    if (!wasPresent) newBasenames.push_back(bn);
  }

  if (!newBasenames.empty()) {
    g_pendingNewAnimations = std::move(newBasenames);
    ConsoleLog("[holoroll] hot-reload: " + std::to_string(g_pendingNewAnimations.size()) +
               " new animation(s) detected; auto-placing.\n");
    // v0.12.0-alpha.10: auto-place instead of waiting for a modal. The
    // confirm/skip popup added in v0.4.0 is gone — placement was always
    // the desired action in practice. PlacePendingAtCursor consumes
    // g_pendingNewAnimations and clears the queue itself.
    PlacePendingAtCursor();
  } else {
    ConsoleLog("[holoroll] hot-reload: rescan found no new animations.\n");
  }
}

// Append regions for the pending new animations after the latest existing
// region (whether it's one of ours or not), separated by region_gap_seconds.
// Existing regions are not touched.
void PlacePendingNewAnimations() {
  const auto& api = g_bridge.Api();
  if (!api.addProjectMarker2) {
    ConsoleLog("[holoroll] AddProjectMarker2 unavailable; cannot place regions.\n");
    g_pendingNewAnimations.clear();
    return;
  }

  // Find the latest end time among ALL regions in the project (not just
  // ours), so we don't overlap with the user's other regions.
  double cursor = 0.0;
  if (api.enumProjectMarkers3) {
    int idx = 0;
    while (true) {
      bool isrgn = false;
      double pos = 0.0, rgnend = 0.0;
      const char* nm = nullptr;
      int rgnIdx = 0, color = 0;
      const int next = api.enumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend, &nm, &rgnIdx, &color);
      if (next == 0) break;
      if (isrgn && rgnend > cursor) cursor = rgnend;
      idx = next;
    }
  }
  cursor += GetGap();

  const int ourColor = AnimationLibrary::RegionColorReaper();
  const double fps = GetFps();
  const std::string& prefix = AnimationLibrary::RegionNamePrefix();

  std::size_t placed = 0;
  for (const std::string& basename : g_pendingNewAnimations) {
    const std::size_t animIdx = g_lib.FindAnimationIndexByBasename(basename);
    if (animIdx == std::numeric_limits<std::size_t>::max()) continue;
    const LoadedAnimation& anim = g_lib.At(animIdx);
    const double duration = anim.DurationSeconds(fps);
    if (duration <= 0.0) continue;

    const std::string regionName = prefix + basename;
    api.addProjectMarker2(nullptr, true, cursor, cursor + duration, regionName.c_str(), -1, ourColor);
    cursor += duration + GetGap();
    ++placed;
  }

  ConsoleLog("[holoroll] hot-reload: placed " + std::to_string(placed) + " new region(s).\n");
  g_pendingNewAnimations.clear();
}

void OnTimer() {
  g_bridge.OnTimerTick();
  g_viewport.Tick();

  // v0.12.0-alpha.11: pump the socket bridge regardless of viewport
  // visibility. External commands should work even when the user has
  // collapsed / closed the HoloRoll panel — the bridge isn't tied to
  // the UI lifecycle.
  socket_server::Tick();

  // v0.13.0-alpha.1: pump the updater. Currently a no-op; placeholder
  // for future periodic re-check / progress UI.
  updater::Tick();

  if (!g_viewport.IsOpen()) return;

  // v0.7.0: detect REAPER project changes (open / save-as / switch / close).
  // EnumProjects(-1) returns the .rpp path of the active project; when it
  // differs from our cached value we rebuild library + watcher to point at
  // the new project's Animations/ folder.
  {
    const std::string activePath = GetActiveProjectPath();
    if (activePath != g_currentProjectPath) {
      OnProjectChanged();
    }
  }

  // Hot-reload: drain folder watcher and (after debounce settles) scan for
  // newly-added animations. Populates g_pendingNewAnimations on success;
  // the modal renders once status.pendingNewAnimations is non-empty.
  ProcessWatcherEvents();

  // v0.8.0: also drain the Incoming watcher and move files into the
  // active project's Animations/ folder. After the move, the regular
  // project-folder watcher (above) picks up the new file as a normal
  // hot-reload event.
  ProcessIncomingWatcherEvents();

  const double timelineTime = g_bridge.TimelineTimeSeconds();

  // v0.6.0: items are now the primary playback driver. Walk every track,
  // every item, and resolve which one is under the playhead. Region
  // metadata is no longer used for resolution — it's just decoration
  // that follows items around (REAPER's default behavior).
  //
  // v0.11.1: pull GLOBAL pre/post-roll from current placement settings;
  // the resolver expands the match window by these values so the playhead
  // visually "holds" frame 0 / last frame in the buffer zones around items.
  const std::vector<DiscoveredItem> items = EnumProjectItems();
  float globalPreRoll = 1.0f, globalPostRoll = 1.0f;
  g_viewport.GetPlacementOptions(&globalPreRoll, &globalPostRoll);
  const ItemResolveResult itemResult = ResolvePlayheadFromItems(
      timelineTime, GetFps(), items, globalPreRoll, globalPostRoll);

  GlViewport::OverlayStatus status;
  status.animationsDir = g_currentAnimationsFolder;
  status.loadedAnimationCount = g_lib.Count();
  status.regionCount = items.size();  // Reusing the field as item count.
  status.topologyAvailable = true;
  status.pendingNewAnimations = g_pendingNewAnimations;
  status.missingAnimationName = itemResult.missingAnimationName;
  status.folderIsOverride = !GetProjectAnimationsOverride().empty();
  status.projectUntitled = GetActiveProjectPath().empty() && !status.folderIsOverride;

  static const std::vector<float> kEmptyFloats;
  static const std::vector<std::uint32_t> kEmptyIndices;

  const std::vector<float>* vertices = &kEmptyFloats;
  const std::vector<std::uint32_t>* indices = &kEmptyIndices;
  std::uint32_t frame = 0;
  std::uint32_t totalFrames = 0;

  const std::size_t animIdx = itemResult.animationIndex;
  const bool resolved = animIdx != std::numeric_limits<std::size_t>::max();
  if (resolved) {
    frame = itemResult.frameIndex;
    // IMPORTANT: status.autoPivot / autoExtent must be populated BEFORE
    // we touch pose state, because the first-time "fresh pose" branch
    // calls ResetCameraToDefault(status), which needs those values to
    // place the camera at the correct distance from the bbox.
    const LoadedAnimation& anim = g_lib.At(animIdx);
    const std::uint32_t totalPoints = anim.TotalPoints();
    if (totalPoints > 0) {
      vertices = &anim.VerticesForFrame(frame);
      totalFrames = anim.TotalFrames();
      status.currentAnimation = anim.basename;
    }
    if (anim.HasTopology()) {
      indices = anim.TriangleIndicesPtr();
      status.topologyAvailable = true;
    } else {
      status.topologyAvailable = false;
    }

    status.autoPivot[0] = anim.autoPivot[0];
    status.autoPivot[1] = anim.autoPivot[1];
    status.autoPivot[2] = anim.autoPivot[2];
    status.autoExtent = anim.autoExtent;
    status.restNormals = &anim.restNormals;

    // Surface the active item's time range to the overlay (it used to come
    // from regions; items now drive this).
    status.activeRegionStart = itemResult.itemStart;
    status.activeRegionEnd = itemResult.itemEnd;

    // Now that status has correct bbox info, handle pose switching.
    if (g_activeAnimIdx != animIdx) {
      if (g_activeAnimIdx != kNoActiveAnim) {
        ViewportPose& prev = g_poses.Get(g_activeAnimIdx);
        g_viewport.CapturePose(prev);
      }
      ViewportPose& target = g_poses.Get(animIdx);
      if (target.initialized) {
        g_viewport.ApplyPose(target);
      } else {
        // Fresh animation: snap camera to a 3/4 view that frames the
        // model's bbox at a consistent on-screen size, then capture
        // that as the pose to remember.
        g_viewport.ResetCameraToDefault(status);
        g_viewport.CapturePose(target);
      }
      g_activeAnimIdx = animIdx;
    }
  }

  g_viewport.Render(*vertices, *indices, timelineTime, frame, totalFrames, status);

  if (g_activeAnimIdx != kNoActiveAnim) {
    ViewportPose& live = g_poses.Get(g_activeAnimIdx);
    g_viewport.CapturePose(live);
  }

  // Throttle scene-setting persistence: only check & write at most once per
  // ~500ms of OnTimer ticks. Without this the user dragging a slider would
  // hammer the disk every frame.
  static ULONGLONG lastSceneSaveTick = 0;
  const ULONGLONG nowTicks = GetTickCount64();
  if (nowTicks - lastSceneSaveTick > 500) {
    if (g_viewport.ConsumeSceneDirty()) {
      PersistSceneSettingsFromViewport();
    }
    if (g_viewport.ConsumePlacementDirty()) {
      PersistPlacementOptionsFromViewport();
    }
    // v0.12.0-alpha.13: debug flag round-trip. Same debounce window.
    if (g_viewport.ConsumeDebugDirty()) {
      PersistDebugFlagFromViewport();
    }
    lastSceneSaveTick = nowTicks;
  }

  const GlViewport::OverlayRequests req = g_viewport.ConsumeRequests();
  if (req.chooseFolder) RunFolderPicker();
  if (req.reloadFolder) {
    if (!g_lib.Directory().empty()) RebuildLibraryAndRegions(g_lib.Directory());
  }
  if (req.placeRegions) PlaceOurItemsAndRegions(nullptr, 0.0);
  if (req.openConfig) OpenConfigInEditor();
  if (req.reloadConfig) ReloadConfigFromDisk();
  if (req.spikeTestCreateItem) SpikeTestCreateItem();
  if (req.resetFolderOverride) ResetFolderToProjectDefault();

  // v0.12.0-alpha.9: motion event marker generation.
  if (req.generateMotionMarkers) GenerateAllMotionMarkers();

  // v0.12.0-alpha.10: new-animations modal dispatch removed. Placement is
  // now automatic — ProcessWatcherEvents calls PlacePendingAtCursor
  // directly when new files are detected, no user confirmation step.
}

bool RegisterAction(reaper_plugin_info_t* rec,
                    const char* cmdName,
                    const char* desc,
                    int* outCmdId,
                    gaccel_register_t* outAccel) {
  *outCmdId = rec->Register("command_id", reinterpret_cast<void*>(const_cast<char*>(cmdName)));
  if (*outCmdId == 0) return false;
  outAccel->accel = {};
  outAccel->accel.cmd = static_cast<WORD>(*outCmdId);
  outAccel->desc = desc;
  rec->Register("gaccel", outAccel);
  return true;
}
}  // namespace

// ---- v0.12.0-alpha.11 socket-bridge linkage shims -------------------------
//
// The socket bridge lives in src/extension/socket_server.cpp (separate
// translation unit). It needs access to two things owned by entry.cpp:
//   - the REAPER API table (ReaperBridge::Api())
//   - the ConsoleLog helper for status messages
// Both are defined inside this file's anonymous namespace above, which
// gives them internal linkage and makes them invisible to other TUs.
// These two file-scope wrappers have external linkage and forward to the
// internal symbols. Anonymous-namespace members are accessible from the
// enclosing global namespace without qualification, so the body just
// calls them by name.
const ReaperApi& holoroll_bridge_api() {
  return g_bridge.Api();
}
void holoroll_bridge_log(const std::string& msg) {
  ConsoleLog(msg);
}
// v0.13.0-alpha.1: updater module needs read/write access to the
// shared config store. Anonymous-ns instance + file-scope forwarder
// = same pattern as the bridge accessors above.
ConfigStore& holoroll_config_ref() {
  return g_config;
}

// v0.13.0-alpha.3: implementation of the build_regions socket verb.
// Item-anchored: for every unit we place the media item first, read
// its actual position/length back from REAPER, then build the region
// around THAT geometry. Spec source: socket bridge verb description
// in CHANGELOG.md.
//
// On top-level argument errors (missing/malformed `units`, etc.) the
// returned JSON has a single `_error` field — socket_server unpacks
// that and throws VerbError. Per-unit failures (missing animation,
// zero-length anim, item-create returning null) are normal: they
// land in the `skipped` array with a human-readable reason.
//
// Lives at file scope so unqualified name lookup finds all the
// anon-ns symbols above (g_bridge, g_lib, EnsureItemsTrackAndFx,
// CreateNamedItemWithRolls, WriteMotionEnvelopesForItem, GetFps,
// FindLastRegionEnd, DeleteAllRegions).
nlohmann::json holoroll_build_regions(const nlohmann::json& args) {
  const auto& api = g_bridge.Api();

  auto errReply = [](const std::string& msg) {
    return nlohmann::json{{"_error", msg}};
  };

  if (!api.addProjectMarker2 || !api.getMediaItemInfo_Value) {
    return errReply("required REAPER APIs unavailable");
  }
  if (!args.contains("units") || !args["units"].is_array()) {
    return errReply("missing or invalid 'units' (expected array)");
  }

  const double regionPad = args.value("region_pad", 0.0);
  const double gap       = args.value("gap", 0.0);
  const std::string startMode = args.value("start_mode", std::string("after_last"));
  const bool clearExisting    = args.value("clear_existing", false);
  // v0.13.0-alpha.5: per-unit polling budget. Items can take a moment
  // to "land" on the timeline — REAPER's internal item table updates
  // are normally synchronous, but with PreventUIRefresh active +
  // motion-envelope writes happening alongside, the caller has
  // observed item geometry occasionally not being readable on the
  // very next API call. Default 2.0s is generous; callers can bump
  // or lower per their workload.
  const double itemWaitS = args.value("item_wait_s", 2.0);

  // Step 1: starting position. Per spec: compute BEFORE clearing, so
  // start_mode=after_last + clear_existing=true still anchors to where
  // the (now-deleted) regions used to end. Niche but well-defined.
  //
  // alpha.2 fix #1: after_last now adds `gap` as a lead-gap from the
  // last region's end. Otherwise the first new region would butt
  // directly against the last existing one.
  double pos = 0.0;
  if (startMode == "cursor") {
    if (api.getCursorPosition) pos = api.getCursorPosition();
    else if (api.getCursorPositionEx) pos = api.getCursorPositionEx(nullptr);
  } else {
    // Default = "after_last". Unknown values fall through here too —
    // safer than rejecting on an arbitrary string the caller may
    // mistype.
    pos = FindLastRegionEnd() + gap;
  }

  // One big undo block for the whole batch + suppress UI refresh
  // between items. Same wrapping the existing create_regions verb
  // uses; the spec emphasises atomicity-per-item but a single outer
  // block is the natural granularity for an undo entry the user
  // sees in REAPER's history.
  if (api.undo_BeginBlock)  api.undo_BeginBlock();
  if (api.preventUIRefresh) api.preventUIRefresh(1);

  // Step 2: clear regions if requested.
  if (clearExisting) DeleteAllRegions();

  // Ensure items track + JSFX up front so we don't re-resolve it on
  // every iteration.
  MotionFxLocation loc = EnsureItemsTrackAndFx();
  if (!loc.track) {
    if (api.preventUIRefresh) api.preventUIRefresh(-1);
    if (api.undo_EndBlock)    api.undo_EndBlock("HoloRoll bridge: build_regions", -1);
    return errReply("could not create or find items track");
  }
  const double fps = GetFps();

  nlohmann::json created = nlohmann::json::array();
  nlohmann::json skipped = nlohmann::json::array();

  for (const auto& unit : args["units"]) {
    if (!unit.is_object()) {
      skipped.push_back({{"anim", ""}, {"reason", "unit is not an object"}});
      continue;
    }
    const std::string anim = unit.value("anim", std::string{});
    const std::string regionBase = unit.value("name", std::string{});
    int variations = unit.value("variations", 1);
    if (variations < 1) variations = 1;
    if (anim.empty()) {
      skipped.push_back({{"anim", anim}, {"reason", "empty 'anim'"}});
      continue;
    }

    // Resolve animation in our library — we need the LoadedAnimation
    // object to rewrite motion envelopes after the reposition and on
    // every duplicate variation.
    const std::size_t animIdx = g_lib.ResolveAnimationByItemName(anim);
    if (animIdx == std::numeric_limits<std::size_t>::max()) {
      skipped.push_back({{"anim", anim}, {"reason", "animation not found in library"}});
      continue;
    }
    const LoadedAnimation& animObj = g_lib.At(animIdx);

    // alpha.2 semantics change: build_regions does NOT place the
    // initial item — the export pipeline does that. We POLL for an
    // existing item named `anim` to appear on ANY track, then
    // reposition + wrap it. If nothing shows up within item_wait_s,
    // skip the unit (no region, no pos advance).
    MediaItem* discoveredItem = nullptr;
    MediaTrack* discoveredTrack = nullptr;
    if (api.countTracks && api.getTrack && api.countTrackMediaItems &&
        api.getTrackMediaItem) {
      const DWORD startTick = GetTickCount();
      const DWORD budgetMs = static_cast<DWORD>(itemWaitS * 1000.0);
      while (true) {
        const int trackCount = api.countTracks(nullptr);
        for (int t = 0; t < trackCount && !discoveredItem; ++t) {
          MediaTrack* track = api.getTrack(nullptr, t);
          if (!track) continue;
          const int itemCount = api.countTrackMediaItems(track);
          for (int i = 0; i < itemCount; ++i) {
            MediaItem* it = api.getTrackMediaItem(track, i);
            if (!it) continue;
            if (ReadItemName(it) != anim) continue;
            discoveredItem = it;
            discoveredTrack = track;
            break;
          }
        }
        if (discoveredItem) break;
        if (GetTickCount() - startTick >= budgetMs) break;
        Sleep(75);
      }
    }

    if (!discoveredItem) {
      skipped.push_back({
          {"anim", anim},
          {"reason", "item did not appear on timeline within item_wait_s seconds"},
      });
      // Do NOT advance pos: the slot stays free for the next unit so
      // we don't end up with a missing-item gap in the sequence.
      continue;
    }

    // Item length is queried once — used for the original repositioned
    // item AND all duplicates. We assume variations share the same
    // length (which is the case: HoloRoll items carry an animation's
    // exact duration via CreateNamedItemWithRolls).
    const double il = api.getMediaItemInfo_Value(discoveredItem, "D_LENGTH");

    // Reposition the discovered item to current pos. Re-write envelopes
    // at the new range so they follow the item.
    if (api.setMediaItemInfo_Value) {
      api.setMediaItemInfo_Value(discoveredItem, "D_POSITION", pos);
    }
    if (loc.fxIdx >= 0) {
      WriteMotionEnvelopesForItem(loc.track, loc.fxIdx, animObj, pos, fps);
    }

    auto formatSuffix = [](int v) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "_%02d", v);
      return std::string(buf);
    };

    // Variation 1: wrap the repositioned original item.
    {
      const std::string regionName = regionBase + formatSuffix(1);
      const double regionStart = pos;
      const double regionEnd   = pos + il + regionPad;
      api.addProjectMarker2(nullptr, /*isrgn=*/true, regionStart, regionEnd,
                            regionName.c_str(), /*wantidx=*/-1, /*color=*/0);
      created.push_back({
          {"name",  regionName},
          {"start", regionStart},
          {"end",   regionEnd},
      });
      pos = regionEnd + gap;
    }

    // Variations 2..N: duplicate the item onto the same track at the
    // running pos and wrap each one. Item name carries the same _NN
    // suffix as the region so HoloRoll's variation-aware playback
    // (`ResolveAnimationByItemName` strips trailing _<digits>) still
    // resolves them back to the source animation.
    for (int v = 2; v <= variations; ++v) {
      const std::string suffix = formatSuffix(v);
      const std::string itemName = anim + suffix;
      const std::string regionName = regionBase + suffix;

      // Duplicate = new media item on the SAME track as the original
      // (preserves whatever placement choice the export made; we don't
      // force everything onto the HoloRoll items track).
      MediaItem* dup = CreateNamedItemWithRolls(discoveredTrack, pos, il,
                                                 0.0f, 0.0f, itemName);
      if (!dup) {
        skipped.push_back({
            {"anim", anim + suffix},
            {"reason", "duplicate item creation failed"},
        });
        // Do NOT advance pos for a failed duplicate.
        continue;
      }

      if (loc.fxIdx >= 0) {
        WriteMotionEnvelopesForItem(loc.track, loc.fxIdx, animObj, pos, fps);
      }

      const double regionStart = pos;
      const double regionEnd   = pos + il + regionPad;
      api.addProjectMarker2(nullptr, /*isrgn=*/true, regionStart, regionEnd,
                            regionName.c_str(), /*wantidx=*/-1, /*color=*/0);
      created.push_back({
          {"name",  regionName},
          {"start", regionStart},
          {"end",   regionEnd},
      });
      pos = regionEnd + gap;
    }
  }

  if (api.preventUIRefresh) api.preventUIRefresh(-1);
  if (api.updateArrange)    api.updateArrange();
  if (api.undo_EndBlock)    api.undo_EndBlock("HoloRoll bridge: build_regions", -1);

  return nlohmann::json{
      {"created", created},
      {"skipped", skipped},
      {"count",   static_cast<int>(created.size())},
  };
}

extern "C" {
REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec) {
  g_dllHandle = reinterpret_cast<HMODULE>(hInstance);

  if (!rec) {
    if (g_rec) {
      g_rec->Register("-hookcommand", reinterpret_cast<void*>(OnMainAction));
      if (g_toggleViewportCommandId) g_rec->Register("-gaccel", &g_toggleViewportAction);
      if (g_chooseFolderCommandId) g_rec->Register("-gaccel", &g_chooseFolderAction);
      if (g_placeRegionsCommandId) g_rec->Register("-gaccel", &g_placeRegionsAction);
      if (g_openConfigCommandId) g_rec->Register("-gaccel", &g_openConfigAction);
      if (g_reloadConfigCommandId) g_rec->Register("-gaccel", &g_reloadConfigAction);
      g_rec->Register("-timer", reinterpret_cast<void*>(OnTimer));
    }
    // v0.12.0-alpha.11: stop the socket bridge before any other cleanup
    // so worker thread can't queue new requests against teardowned state.
    socket_server::Stop();
    // v0.13.0-alpha.1: stop the updater. If a pending installer is
    // staged + auto-install enabled + not dismissed, this is where
    // the detached PowerShell watchdog gets spawned. Must run before
    // bridge shutdown so config save (if any) completes.
    updater::Stop();
    CloseViewportIfNeeded();
    g_watcher.Stop();
    g_incomingWatcher.Stop();
    g_pendingNewAnimations.clear();
    drop_target::Shutdown();
    g_bridge.Shutdown(g_rec);
    g_lib.Clear();
    g_poses.Clear();
    g_activeAnimIdx = kNoActiveAnim;
    g_rec = nullptr;
    g_toggleViewportCommandId = 0;
    g_chooseFolderCommandId = 0;
    g_placeRegionsCommandId = 0;
    g_openConfigCommandId = 0;
    g_reloadConfigCommandId = 0;
    return 0;
  }

  if (rec->caller_version != REAPER_PLUGIN_VERSION) return 0;
  if (!g_bridge.Initialize(rec)) return 0;
  g_rec = rec;

  g_config.Load(ConfigFilePath());
  EnsureConfigDefaults();
  ApplyRegionPrefixFromConfig();
  g_config.Save();

  // v0.9.0: initialise OLE so we can register drop targets on our viewport.
  // Returns true if OLE is usable; false here doesn't fail plugin load,
  // it just means drag-n-drop won't work (the user's existing workflow
  // through Incoming/ folder still works fine).
  if (!drop_target::Initialize()) {
    ConsoleLog("[holoroll] WARNING: OLE drag-n-drop init failed; viewport drops disabled.\n");
  }

  // The animations folder is resolved on the first OnTimer tick from the
  // active project's path. No initial folder picker (v0.7.0 ditched the
  // global animations_dir concept). Untitled projects show a hint in the
  // overlay; saving the project triggers folder creation automatically.

  // v0.8.0: ensure the global Incoming folder exists and start watching it.
  // Files arriving here are auto-moved into the active project's
  // Animations/ folder by ProcessIncomingWatcherEvents() on the next tick.
  {
    const std::string incoming = GetIncomingFolder();
    if (!incoming.empty()) {
      EnsureFolderExists(incoming);
      if (!g_incomingWatcher.Start(incoming)) {
        ConsoleLog("[holoroll] WARNING: failed to start Incoming watcher on " + incoming + "\n");
      } else {
        ConsoleLog("[holoroll] watching Incoming folder: " + incoming + "\n");
      }
    }
  }

  if (!RegisterAction(rec, kToggleViewportCommandName, kToggleViewportActionDesc,
                      &g_toggleViewportCommandId, &g_toggleViewportAction)) {
    g_bridge.Shutdown(rec); g_rec = nullptr; return 0;
  }
  if (!RegisterAction(rec, kChooseFolderCommandName, kChooseFolderActionDesc,
                      &g_chooseFolderCommandId, &g_chooseFolderAction)) {
    rec->Register("-gaccel", &g_toggleViewportAction);
    g_bridge.Shutdown(rec); g_rec = nullptr; return 0;
  }
  if (!RegisterAction(rec, kPlaceRegionsCommandName, kPlaceRegionsActionDesc,
                      &g_placeRegionsCommandId, &g_placeRegionsAction)) {
    rec->Register("-gaccel", &g_toggleViewportAction);
    rec->Register("-gaccel", &g_chooseFolderAction);
    g_bridge.Shutdown(rec); g_rec = nullptr; return 0;
  }
  if (!RegisterAction(rec, kOpenConfigCommandName, kOpenConfigActionDesc,
                      &g_openConfigCommandId, &g_openConfigAction)) {
    rec->Register("-gaccel", &g_toggleViewportAction);
    rec->Register("-gaccel", &g_chooseFolderAction);
    rec->Register("-gaccel", &g_placeRegionsAction);
    g_bridge.Shutdown(rec); g_rec = nullptr; return 0;
  }
  if (!RegisterAction(rec, kReloadConfigCommandName, kReloadConfigActionDesc,
                      &g_reloadConfigCommandId, &g_reloadConfigAction)) {
    rec->Register("-gaccel", &g_toggleViewportAction);
    rec->Register("-gaccel", &g_chooseFolderAction);
    rec->Register("-gaccel", &g_placeRegionsAction);
    rec->Register("-gaccel", &g_openConfigAction);
    g_bridge.Shutdown(rec); g_rec = nullptr; return 0;
  }

  rec->Register("hookcommand", reinterpret_cast<void*>(OnMainAction));

  OpenViewportIfNeeded();
  if (!g_viewport.IsOpen()) {
    rec->Register("-hookcommand", reinterpret_cast<void*>(OnMainAction));
    rec->Register("-gaccel", &g_toggleViewportAction);
    rec->Register("-gaccel", &g_chooseFolderAction);
    rec->Register("-gaccel", &g_placeRegionsAction);
    rec->Register("-gaccel", &g_openConfigAction);
    rec->Register("-gaccel", &g_reloadConfigAction);
    g_bridge.Shutdown(rec); g_rec = nullptr; return 0;
  }

  rec->Register("timer", reinterpret_cast<void*>(OnTimer));

  // v0.12.0-alpha.11: TCP socket bridge for external command senders.
  // Worker thread + accept loop run independently; OnTimer below
  // drains the request queue on the main thread (REAPER C API rule).
  // Failure to start (port busy, etc.) is non-fatal — logged and
  // continues without the bridge.
  socket_server::Start();

  // v0.13.0-alpha.1: in-app auto-updater. Background polls GitHub
  // Releases on a worker thread; on REAPER close (Stop below) it
  // spawns the detached PowerShell watchdog that runs the staged
  // installer silently after our DLL is unloaded.
  updater::Start();

  return 1;
}
}
