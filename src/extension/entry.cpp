// windows.h MUST come before shellapi.h / shlobj.h.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "core/animation_library.h"
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
constexpr char kCfgKeyPlacementVariations[]    = "placement.variations";
constexpr char kCfgKeyPlacementPreRoll[]       = "placement.pre_roll_seconds";
constexpr char kCfgKeyPlacementPostRoll[]      = "placement.post_roll_seconds";
constexpr char kCfgKeyPlacementRegionOverhang[] = "placement.region_overhang_seconds";

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

constexpr bool kVerboseLog = false;
void ConsoleLog(const std::string& msg) {
  if (!kVerboseLog) return;
  if (g_bridge.Api().showConsoleMsg) g_bridge.Api().showConsoleMsg(msg.c_str());
}

// Always-on console output for the v0.6.0 spike. The regular ConsoleLog is
// silenced by default to keep the REAPER console clean during normal use,
// but the spike specifically wants the user to see what happened so we use
// a dedicated path that ignores kVerboseLog.
void SpikeLog(const std::string& msg) {
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
  // v0.11.0: placement defaults match user's stated preference.
  if (!g_config.Has(kCfgKeyPlacementVariations))     g_config.SetDouble(kCfgKeyPlacementVariations, 1.0);
  if (!g_config.Has(kCfgKeyPlacementPreRoll))        g_config.SetDouble(kCfgKeyPlacementPreRoll, 1.0);
  if (!g_config.Has(kCfgKeyPlacementPostRoll))       g_config.SetDouble(kCfgKeyPlacementPostRoll, 1.0);
  if (!g_config.Has(kCfgKeyPlacementRegionOverhang)) g_config.SetDouble(kCfgKeyPlacementRegionOverhang, 0.5);
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

// v0.11.0: same shape but for placement options (variations count, pre/post
// roll, region overhang). Read once on viewport open and on Reload config;
// persisted whenever the user touches the inline fields in the overlay.
void ApplyPlacementOptionsToViewport() {
  const int variations = static_cast<int>(g_config.GetDouble(kCfgKeyPlacementVariations, 1.0));
  const float preRoll  = static_cast<float>(g_config.GetDouble(kCfgKeyPlacementPreRoll, 1.0));
  const float postRoll = static_cast<float>(g_config.GetDouble(kCfgKeyPlacementPostRoll, 1.0));
  const float overhang = static_cast<float>(g_config.GetDouble(kCfgKeyPlacementRegionOverhang, 0.5));
  g_viewport.SetPlacementOptions(variations, preRoll, postRoll, overhang);
}

void PersistPlacementOptionsFromViewport() {
  int variations = 1;
  float preRoll = 1.0f, postRoll = 1.0f, overhang = 0.5f;
  g_viewport.GetPlacementOptions(&variations, &preRoll, &postRoll, &overhang);
  g_config.SetDouble(kCfgKeyPlacementVariations, static_cast<double>(variations));
  g_config.SetDouble(kCfgKeyPlacementPreRoll, static_cast<double>(preRoll));
  g_config.SetDouble(kCfgKeyPlacementPostRoll, static_cast<double>(postRoll));
  g_config.SetDouble(kCfgKeyPlacementRegionOverhang, static_cast<double>(overhang));
  g_config.Save();
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

// Resolve the animations folder for the current project state. Returns
// empty string if no folder is currently usable (Untitled project).
//
// v0.10.1 layout: each project gets its own subfolder under a shared
// `Animations/` directory next to the .rpp:
//
//   <project_dir>/
//     MyLevel.rpp
//     BossFight.rpp
//     Animations/
//       MyLevel/      <-- belongs to MyLevel.rpp
//         frog_jump.glb
//       BossFight/    <-- belongs to BossFight.rpp
//         enemy_hit.glb
//
// This isolates assets between projects that share a directory. Replaces
// the v0.7.0 layout (`<project_dir>/Animations/` shared across all .rpps
// in the same dir) without backward compatibility — any existing v0.7–v0.10
// project will see an empty library after upgrade and needs its files
// moved into the new `Animations/<project_name>/` location, or a manual
// `Choose folder...` override pointing at the old shared path.
std::string ResolveActiveAnimationsFolder() {
  // Override always wins, even if the project hasn't been saved yet (the
  // user explicitly pointed Choose folder... somewhere).
  const std::string override = GetProjectAnimationsOverride();
  if (!override.empty()) return override;

  const std::string proj = GetActiveProjectPath();
  if (proj.empty()) return {};  // Untitled project, no default available.

  const std::string projDir = DirOfPath(proj);
  if (projDir.empty()) return {};

  const std::string projBasename = ProjectBasenameFromPath(proj);
  if (projBasename.empty()) return {};

  return projDir + "\\" + kProjectAnimationsSubdir + "\\" + projBasename;
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
// Untitled projects can't accept drops (there's no project folder to put
// them in). We log a single-line message instead of crashing or showing a
// modal; the Untitled overlay already says "Save the REAPER project".
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
  // summaries. Surface to console (always-on) so the user can sanity-check
  // motion data on freshly-loaded files. Will move to overlay UI in alpha.2.
  if (!log.empty()) {
    SpikeLog(log);
    // Force REAPER's console window open so the motion summary is
    // immediately visible. Safe even if user had the console hidden.
    ForceShowReaperConsole();
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

    // v0.9.0: register OLE drop target so files dragged from Explorer
    // onto the viewport land in the active project's Animations/ folder.
    // The acceptance query lets the drop overlay show an amber "save the
    // project first" hint instead of a green "drop here" when the project
    // is Untitled.
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

// v0.9.1: ensure there's a fresh track at index 0 (top of REAPER's track
// list) and return it. Used by all "place items" code paths so newly-imported
// animations always land on a clean dedicated row instead of mixing with
// the user's existing tracks.
//
// We always create a new one rather than reusing track 0 — the user's
// project may already have content on track 0 that we shouldn't disturb,
// and visually "new track on top" is a clear signal that something arrived.
MediaTrack* EnsureTrackOnTop() {
  const auto& api = g_bridge.Api();
  if (!api.insertTrackAtIndex || !api.getTrack) return nullptr;
  api.insertTrackAtIndex(0, true);
  return api.getTrack(nullptr, 0);
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

// Replacement for PlaceOurRegions. v0.11.1: for each animation in the
// library, append `variations` items (with `_02`, `_03` ... suffixes for
// variation indices >= 2). De-duplicates against existing HoloRoll items:
// if an item with the exact same name already exists anywhere on the
// timeline, it's skipped (no duplicate).
//
// Item length = animation duration only (clean, no embedded buffers).
// Region length = item length + region overhang.
// Pre/post-roll are global playback-time visual buffers, not part of the
// item or region geometry. The cursor advances by `duration + gap` between
// items — also independent of pre/post.
void PlaceOurItemsAndRegions(MediaTrack* /*ignored*/, double /*ignored*/) {
  const auto& api = g_bridge.Api();
  if (!api.addProjectMarker2 || !api.updateArrange) {
    SpikeLog("[holoroll] cannot place items: REAPER API incomplete.\n");
    return;
  }
  // Wipe our existing regions to avoid duplicates on re-run. Items are left
  // in place by design — user-created content is never auto-deleted.
  // De-dup logic below ensures we don't ADD duplicate items either.
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

  // Read placement options from viewport. Variations and regionOverhang
  // are placement-time values; pre/post-roll are read but unused here
  // (they only affect playback resolution, not item/region geometry).
  int variations = 1;
  float preRoll_unused = 1.0f, postRoll_unused = 1.0f, regionOverhang = 0.5f;
  g_viewport.GetPlacementOptions(&variations, &preRoll_unused,
                                 &postRoll_unused, &regionOverhang);

  MediaTrack* targetTrack = EnsureTrackOnTop();
  if (!targetTrack) {
    SpikeLog("[holoroll] cannot place items: failed to create track.\n");
    return;
  }

  const double fps = GetFps();
  const double gap = GetGap();
  const std::string& prefix = AnimationLibrary::RegionNamePrefix();
  const int color = AnimationLibrary::RegionColorReaper();

  // Start after the last existing region (regardless of who created it),
  // so we never overlap. If no regions exist, start from 0 + gap.
  const double lastEnd = FindLastRegionEnd();
  double cursor = (lastEnd > 0.0) ? (lastEnd + gap) : 0.0;

  std::size_t placed = 0;
  std::size_t skipped = 0;
  for (std::size_t i = 0; i < g_lib.Count(); ++i) {
    const LoadedAnimation& anim = g_lib.At(i);
    const double duration = anim.DurationSeconds(fps);
    if (duration <= 0.0) continue;

    for (int v = 1; v <= variations; ++v) {
      const std::string itemName = MakeVariationName(anim.basename, v);
      if (alreadyPlaced(itemName)) {
        ++skipped;
        continue;
      }
      MediaItem* item = CreateNamedItemWithRolls(targetTrack, cursor, duration,
                                                 0.0f, 0.0f, itemName);
      if (item) {
        const std::string regionName = prefix + itemName;
        // Region: from item.start to item.end + overhang. No pre-roll
        // affects the region; the visual buffer is rendered overlay-side.
        api.addProjectMarker2(nullptr, true, cursor,
                              cursor + duration + regionOverhang,
                              regionName.c_str(), -1, color);
        ++placed;
        // Add to existing-names so later variations of the same anim
        // (within this Place all run) also de-dup correctly.
        existingNames.push_back(itemName);
        cursor += duration + gap;
      }
    }
  }

  api.updateArrange();
  ConsoleLog("[holoroll] placed " + std::to_string(placed) +
             " items+regions (skipped " + std::to_string(skipped) + " already-placed).\n");
}

// Place pending hot-reload animations after the last existing region.
// v0.11.1: respects placement options (variations, region overhang) and
// de-dups against existing items. Pre/post-roll are global playback
// buffers, not part of item geometry.
void PlacePendingAtCursor() {
  if (g_pendingNewAnimations.empty()) return;
  const auto& api = g_bridge.Api();

  // Snapshot existing item names BEFORE creating the new track.
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

  int variations = 1;
  float preRoll_unused = 1.0f, postRoll_unused = 1.0f, regionOverhang = 0.5f;
  g_viewport.GetPlacementOptions(&variations, &preRoll_unused,
                                 &postRoll_unused, &regionOverhang);

  MediaTrack* track = EnsureTrackOnTop();
  if (!track) {
    SpikeLog("[holoroll] cannot place: failed to create track.\n");
    g_pendingNewAnimations.clear();
    return;
  }

  const double fps = GetFps();
  const double gap = GetGap();
  const std::string& prefix = AnimationLibrary::RegionNamePrefix();
  const int color = AnimationLibrary::RegionColorReaper();

  // Start after the last existing region. If none, start at 0.
  const double lastEnd = FindLastRegionEnd();
  double pos = (lastEnd > 0.0) ? (lastEnd + gap) : 0.0;

  std::size_t placed = 0;
  std::size_t skipped = 0;
  for (const std::string& basename : g_pendingNewAnimations) {
    const std::size_t idx = g_lib.FindAnimationIndexByBasename(basename);
    if (idx == std::numeric_limits<std::size_t>::max()) continue;
    const LoadedAnimation& anim = g_lib.At(idx);
    const double duration = anim.DurationSeconds(fps);
    if (duration <= 0.0) continue;

    for (int v = 1; v <= variations; ++v) {
      const std::string itemName = MakeVariationName(basename, v);
      if (alreadyPlaced(itemName)) {
        ++skipped;
        continue;
      }
      MediaItem* item = CreateNamedItemWithRolls(track, pos, duration,
                                                 0.0f, 0.0f, itemName);
      if (item) {
        if (api.addProjectMarker2) {
          const std::string regionName = prefix + itemName;
          api.addProjectMarker2(nullptr, true, pos,
                                pos + duration + regionOverhang,
                                regionName.c_str(), -1, color);
        }
        ++placed;
        existingNames.push_back(itemName);
        pos += duration + gap;
      }
    }
  }

  if (api.updateArrange) api.updateArrange();
  ConsoleLog("[holoroll] hot-reload: placed " + std::to_string(placed) +
             " item(s), skipped " + std::to_string(skipped) + " already-placed.\n");
  g_pendingNewAnimations.clear();
}

// Convenience: create one item for a single named animation. v0.11.1:
// respects region overhang. Variations is NOT applied here (this is the
// "+ Place" button path which always creates one). Pre/post-roll are
// global playback buffers, not part of item/region geometry.
void PlaceSingleAtCursor(const std::string& basename) {
  const auto& api = g_bridge.Api();

  int variations_unused = 1;
  float preRoll_unused = 1.0f, postRoll_unused = 1.0f, regionOverhang = 0.5f;
  g_viewport.GetPlacementOptions(&variations_unused, &preRoll_unused,
                                 &postRoll_unused, &regionOverhang);

  MediaTrack* track = EnsureTrackOnTop();
  if (!track) return;

  const std::size_t idx = g_lib.FindAnimationIndexByBasename(basename);
  if (idx == std::numeric_limits<std::size_t>::max()) return;
  const LoadedAnimation& anim = g_lib.At(idx);
  const double duration = anim.DurationSeconds(GetFps());
  if (duration <= 0.0) return;

  const double lastEnd = FindLastRegionEnd();
  const double pos = (lastEnd > 0.0) ? (lastEnd + GetGap()) : 0.0;

  MediaItem* item = CreateNamedItemWithRolls(track, pos, duration,
                                             0.0f, 0.0f, basename);
  if (item && api.addProjectMarker2) {
    const std::string& prefix = AnimationLibrary::RegionNamePrefix();
    const int color = AnimationLibrary::RegionColorReaper();
    const std::string regionName = prefix + basename;
    api.addProjectMarker2(nullptr, true, pos,
                          pos + duration + regionOverhang,
                          regionName.c_str(), -1, color);
  }
  if (api.updateArrange) api.updateArrange();
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
// All output goes to the REAPER console regardless of kVerboseLog so the user
// can see exactly what happened. If any of the API pointers are missing the
// helper bails with a useful message and the rest of the plugin keeps working.
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
               " new animation(s) detected.\n");
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
  int variations_unused = 1;
  float globalPreRoll = 1.0f, globalPostRoll = 1.0f, regionOverhang_unused = 0.5f;
  g_viewport.GetPlacementOptions(&variations_unused, &globalPreRoll,
                                 &globalPostRoll, &regionOverhang_unused);
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

  // Handle the new-animations modal response.
  if (req.newAnimationsChoice == 1) {
    PlacePendingAtCursor();
  } else if (req.newAnimationsChoice == 2) {
    ConsoleLog("[holoroll] hot-reload: user dismissed " +
               std::to_string(g_pendingNewAnimations.size()) +
               " pending animation(s).\n");
    g_pendingNewAnimations.clear();
  }
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
  return 1;
}
}
