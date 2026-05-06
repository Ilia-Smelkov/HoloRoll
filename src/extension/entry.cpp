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
FolderWatcher g_watcher;
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
constexpr char kExtStateAnimDir[] = "animations_dir";
constexpr char kConfigFileName[] = "holoroll_config.ini";

constexpr char kCfgKeyAnimDir[] = "animations_dir";
constexpr char kCfgKeyFps[] = "fps";
constexpr char kCfgKeyGap[] = "region_gap_seconds";
constexpr char kCfgKeyRegionPrefix[] = "region_name_prefix";
constexpr char kCfgKeyHotReload[] = "hot_reload.enabled";
// Scene settings (ground plane). Persisted between sessions.
constexpr char kCfgKeySceneShowGround[] = "scene.show_ground_plane";
constexpr char kCfgKeySceneRadius[]     = "scene.ground_radius";
constexpr char kCfgKeySceneGridStep[]   = "scene.grid_step";

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

void EnsureConfigDefaults() {
  if (!g_config.Has(kCfgKeyFps)) g_config.SetDouble(kCfgKeyFps, kDefaultFps);
  if (!g_config.Has(kCfgKeyGap)) g_config.SetDouble(kCfgKeyGap, kDefaultGapSeconds);
  if (!g_config.Has(kCfgKeyAnimDir)) g_config.SetString(kCfgKeyAnimDir, "");
  if (!g_config.Has(kCfgKeyRegionPrefix)) g_config.SetString(kCfgKeyRegionPrefix, "");
  if (!g_config.Has(kCfgKeyHotReload)) g_config.SetDouble(kCfgKeyHotReload, 1.0);
  if (!g_config.Has(kCfgKeySceneShowGround)) g_config.SetDouble(kCfgKeySceneShowGround, 1.0);
  if (!g_config.Has(kCfgKeySceneRadius))     g_config.SetDouble(kCfgKeySceneRadius, 20.0);
  if (!g_config.Has(kCfgKeySceneGridStep))   g_config.SetDouble(kCfgKeySceneGridStep, 1.0);
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
  g_viewport.SetSceneSettings(show, radius, gridStep);
}

// Pull current scene state from viewport and write it into the config + disk.
// Called when the overlay UI dirties the values.
void PersistSceneSettingsFromViewport() {
  bool show = true;
  float radius = 20.0f;
  float gridStep = 1.0f;
  g_viewport.GetSceneSettings(&show, &radius, &gridStep);
  g_config.SetDouble(kCfgKeySceneShowGround, show ? 1.0 : 0.0);
  g_config.SetDouble(kCfgKeySceneRadius,     static_cast<double>(radius));
  g_config.SetDouble(kCfgKeySceneGridStep,   static_cast<double>(gridStep));
  g_config.Save();
}

std::string ResolveAnimationsDir() {
  const std::string fromCfg = g_config.GetString(kCfgKeyAnimDir, "");
  if (!fromCfg.empty()) return fromCfg;
  if (g_bridge.Api().getExtState && g_bridge.Api().hasExtState) {
    if (g_bridge.Api().hasExtState(kExtStateSection, kExtStateAnimDir)) {
      const char* value = g_bridge.Api().getExtState(kExtStateSection, kExtStateAnimDir);
      if (value && *value) return value;
    }
  }
  return {};
}

void PersistAnimationsDir(const std::string& dir) {
  g_config.SetString(kCfgKeyAnimDir, dir);
  g_config.Save();
  if (g_bridge.Api().setExtState) {
    g_bridge.Api().setExtState(kExtStateSection, kExtStateAnimDir, dir.c_str(), true);
  }
}

double GetFps() { return g_config.GetDouble(kCfgKeyFps, kDefaultFps); }
double GetGap() { return g_config.GetDouble(kCfgKeyGap, kDefaultGapSeconds); }

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

void RebuildLibraryAndRegions(const std::string& dir) {
  std::string log;
  const std::size_t loaded = g_lib.ScanFolder(dir, GetFps(), &log);
  g_lib.BuildRegions(GetFps(), GetGap(), 0.0);
  if (!log.empty()) ConsoleLog(log);
  ConsoleLog("[holoroll] library: " + std::to_string(loaded) +
             " animation(s), " + std::to_string(g_lib.Regions().size()) + " region(s) prepared.\n");

  g_poses.Clear();
  g_activeAnimIdx = kNoActiveAnim;

  // Restart the watcher on this directory. Stop() is idempotent.
  // If hot-reload is disabled, we still call Stop to be sure no stale watcher
  // is running from a previous folder.
  g_watcher.Stop();
  g_pendingNewAnimations.clear();
  g_watcherEventsAccumulating = false;
  if (HotReloadEnabled() && !dir.empty()) {
    if (!g_watcher.Start(dir)) {
      ConsoleLog("[holoroll] FolderWatcher failed to start on: " + dir + "\n");
    }
  }
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
  const std::string current = g_lib.Directory();
  const std::string chosen = folder_picker::BrowseForFolder(owner, "Select animations folder (.mdd / .glb)", current);
  if (chosen.empty()) return;
  PersistAnimationsDir(chosen);
  RebuildLibraryAndRegions(chosen);
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
  }
}

void CloseViewportIfNeeded() {
  if (g_viewport.IsOpen()) {
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
  const std::string dir = ResolveAnimationsDir();
  if (!dir.empty() && dir != g_lib.Directory()) {
    RebuildLibraryAndRegions(dir);
  } else {
    g_lib.BuildRegions(GetFps(), GetGap(), 0.0);
  }
  // Pick up scene settings the user may have edited in the file.
  if (g_viewport.IsOpen()) ApplySceneSettingsToViewport();
  // If the user just toggled hot_reload.enabled, restart the watcher to match.
  if (HotReloadEnabled() && !g_lib.Directory().empty() && !g_watcher.IsRunning()) {
    g_watcher.Start(g_lib.Directory());
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

// Create a single empty named item on `track` at `position` with `length`
// seconds and `name` as the take's display name. Returns true on success.
// Caller is responsible for batching UpdateArrange.
bool CreateNamedItem(MediaTrack* track, double position, double length, const std::string& name) {
  if (!track) return false;
  const auto& api = g_bridge.Api();
  if (!api.addMediaItemToTrack || !api.setMediaItemInfo_Value ||
      !api.addTakeToMediaItem || !api.getSetMediaItemTakeInfo_String) {
    return false;
  }

  MediaItem* item = api.addMediaItemToTrack(track);
  if (!item) return false;
  api.setMediaItemInfo_Value(item, "D_POSITION", position);
  api.setMediaItemInfo_Value(item, "D_LENGTH", std::max(0.001, length));

  MediaItem_Take* take = api.addTakeToMediaItem(item);
  if (take) {
    char buf[kItemNameBufferSize];
    std::snprintf(buf, sizeof(buf), "%s", name.c_str());
    api.getSetMediaItemTakeInfo_String(take, "P_NAME", buf, true);
  }
  return true;
}

// One item discovered on the timeline, normalised to what ResolvePlayhead
// needs. trackIndex preserves source order: lower index = higher in REAPER's
// track list = wins resolution conflicts.
struct DiscoveredItem {
  int trackIndex = 0;
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  std::string name;
};

// Walk every track in the project, then every item on each track, and return
// items that have a non-empty name. Empty-named items are ignored on the
// theory that they're noise (e.g. user dragged in a wav, deleted the take,
// left an empty item we shouldn't try to drive).
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
      const std::string name = ReadItemName(item);
      if (name.empty()) continue;
      const double pos = api.getMediaItemInfo_Value(item, "D_POSITION");
      const double len = api.getMediaItemInfo_Value(item, "D_LENGTH");
      DiscoveredItem di;
      di.trackIndex = t;
      di.startSeconds = pos;
      di.endSeconds = pos + len;
      di.name = name;
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
// trackIndex = higher in REAPER UI = wins). Inside the chosen item, computes
// the frame as floor((playhead - item.start) * fps), clamped to the
// animation's last frame so a too-long item shows rest pose at the end.
ItemResolveResult ResolvePlayheadFromItems(double playheadSeconds,
                                           double fps,
                                           const std::vector<DiscoveredItem>& items) {
  ItemResolveResult result;

  // Tracks are visited in order, so the first overlap we hit is by
  // definition the topmost track's item.
  for (const auto& di : items) {
    if (playheadSeconds < di.startSeconds || playheadSeconds > di.endSeconds) continue;

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
      const double localTime = std::max(0.0, playheadSeconds - di.startSeconds);
      double f = std::floor(localTime * fps);
      const double lastFrame = static_cast<double>(totalFrames - 1);
      if (f > lastFrame) f = lastFrame;
      result.frameIndex = static_cast<std::uint32_t>(f);
    }
    return result;
  }

  return result;
}

// Replacement for PlaceOurRegions. For each animation in the library,
// appends an item + matching region in sequence starting at startSeconds.
// Items go on `targetTrack` or first track if null.
void PlaceOurItemsAndRegions(MediaTrack* targetTrack, double startSeconds) {
  const auto& api = g_bridge.Api();
  if (!api.addProjectMarker2 || !api.updateArrange) {
    SpikeLog("[holoroll] cannot place items: REAPER API incomplete.\n");
    return;
  }
  // Wipe our existing regions to avoid duplicates on re-run. Items are left
  // in place by design — user-created content is never auto-deleted.
  DeleteOurRegions();

  if (!targetTrack && api.getTrack) targetTrack = api.getTrack(nullptr, 0);
  if (!targetTrack) {
    SpikeLog("[holoroll] cannot place items: no track in project.\n");
    return;
  }

  const double fps = GetFps();
  const double gap = GetGap();
  const std::string& prefix = AnimationLibrary::RegionNamePrefix();
  const int color = AnimationLibrary::RegionColorReaper();

  double cursor = startSeconds;
  std::size_t placed = 0;
  for (std::size_t i = 0; i < g_lib.Count(); ++i) {
    const LoadedAnimation& anim = g_lib.At(i);
    const double duration = anim.DurationSeconds(fps);
    if (duration <= 0.0) continue;

    if (CreateNamedItem(targetTrack, cursor, duration, anim.basename)) {
      const std::string regionName = prefix + anim.basename;
      api.addProjectMarker2(nullptr, true, cursor, cursor + duration,
                            regionName.c_str(), -1, color);
      ++placed;
    }
    cursor += duration + gap;
  }

  api.updateArrange();
  ConsoleLog("[holoroll] placed " + std::to_string(placed) + " items+regions.\n");
}

// Place pending hot-reload animations at the play cursor on the first
// selected track (or first track if nothing selected). Each animation
// goes after the previous one with a gap; existing items are not
// touched. This is the modal's "Place at cursor" path.
void PlacePendingAtCursor() {
  if (g_pendingNewAnimations.empty()) return;
  const auto& api = g_bridge.Api();

  MediaTrack* track = nullptr;
  if (api.getSelectedTrack) track = api.getSelectedTrack(nullptr, 0);
  if (!track && api.getTrack) track = api.getTrack(nullptr, 0);
  if (!track) {
    SpikeLog("[holoroll] cannot place at cursor: no track in project.\n");
    g_pendingNewAnimations.clear();
    return;
  }

  const double cursorPos = api.getCursorPositionEx ? api.getCursorPositionEx(nullptr) : 0.0;
  const double fps = GetFps();
  const double gap = GetGap();
  const std::string& prefix = AnimationLibrary::RegionNamePrefix();
  const int color = AnimationLibrary::RegionColorReaper();

  double pos = cursorPos;
  std::size_t placed = 0;
  for (const std::string& basename : g_pendingNewAnimations) {
    const std::size_t idx = g_lib.FindAnimationIndexByBasename(basename);
    if (idx == std::numeric_limits<std::size_t>::max()) continue;
    const LoadedAnimation& anim = g_lib.At(idx);
    const double duration = anim.DurationSeconds(fps);
    if (duration <= 0.0) continue;

    if (CreateNamedItem(track, pos, duration, basename)) {
      if (api.addProjectMarker2) {
        const std::string regionName = prefix + basename;
        api.addProjectMarker2(nullptr, true, pos, pos + duration,
                              regionName.c_str(), -1, color);
      }
      ++placed;
    }
    pos += duration + gap;
  }

  if (api.updateArrange) api.updateArrange();
  ConsoleLog("[holoroll] hot-reload: placed " + std::to_string(placed) +
             " item(s) starting at " + std::to_string(cursorPos) + "s.\n");
  g_pendingNewAnimations.clear();
}

// Convenience: create one item for a single named animation at the cursor.
// Used by the "+ Place" buttons in the overlay's library list.
void PlaceSingleAtCursor(const std::string& basename) {
  const auto& api = g_bridge.Api();
  MediaTrack* track = nullptr;
  if (api.getSelectedTrack) track = api.getSelectedTrack(nullptr, 0);
  if (!track && api.getTrack) track = api.getTrack(nullptr, 0);
  if (!track) return;

  const std::size_t idx = g_lib.FindAnimationIndexByBasename(basename);
  if (idx == std::numeric_limits<std::size_t>::max()) return;
  const LoadedAnimation& anim = g_lib.At(idx);
  const double duration = anim.DurationSeconds(GetFps());
  if (duration <= 0.0) return;

  const double cursorPos = api.getCursorPositionEx ? api.getCursorPositionEx(nullptr) : 0.0;
  CreateNamedItem(track, cursorPos, duration, basename);
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

  // Hot-reload: drain folder watcher and (after debounce settles) scan for
  // newly-added animations. Populates g_pendingNewAnimations on success;
  // the modal renders once status.pendingNewAnimations is non-empty.
  ProcessWatcherEvents();

  const double timelineTime = g_bridge.TimelineTimeSeconds();

  // v0.6.0: items are now the primary playback driver. Walk every track,
  // every item, and resolve which one is under the playhead. Region
  // metadata is no longer used for resolution — it's just decoration
  // that follows items around (REAPER's default behavior).
  const std::vector<DiscoveredItem> items = EnumProjectItems();
  const ItemResolveResult itemResult = ResolvePlayheadFromItems(timelineTime, GetFps(), items);

  GlViewport::OverlayStatus status;
  status.animationsDir = g_lib.Directory();
  status.loadedAnimationCount = g_lib.Count();
  status.regionCount = items.size();  // Reusing the field as item count.
  status.topologyAvailable = true;
  status.pendingNewAnimations = g_pendingNewAnimations;
  status.missingAnimationName = itemResult.missingAnimationName;

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
    g_pendingNewAnimations.clear();
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

  std::string animDir = ResolveAnimationsDir();
  if (animDir.empty()) {
    animDir = folder_picker::BrowseForFolder(nullptr, "Select animations folder (.mdd / .glb)", "");
    if (!animDir.empty()) PersistAnimationsDir(animDir);
  }

  if (!animDir.empty()) {
    RebuildLibraryAndRegions(animDir);
  } else {
    ConsoleLog("[holoroll] no animations folder configured. Use 'Choose folder' to set one.\n");
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
