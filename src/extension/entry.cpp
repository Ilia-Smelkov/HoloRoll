// windows.h MUST come before shellapi.h / shlobj.h.
#include <windows.h>
#include <shellapi.h>

#include <limits>
#include <string>
#include <vector>

#include "core/animation_library.h"
#include "core/config_store.h"
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
reaper_plugin_info_t* g_rec = nullptr;
HMODULE g_dllHandle = nullptr;

constexpr std::size_t kNoActiveAnim = std::numeric_limits<std::size_t>::max();
std::size_t g_activeAnimIdx = kNoActiveAnim;

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

// Command names form REAPER's stable action identifiers — keep them for
// backwards compatibility with existing user keybindings.
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

// ----- Path helpers ---------------------------------------------------------

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

void ConsoleLog(const std::string& msg) {
  if (g_bridge.Api().showConsoleMsg) g_bridge.Api().showConsoleMsg(msg.c_str());
}

// ----- Config ↔ ExtState round-trip -----------------------------------------

void EnsureConfigDefaults() {
  if (!g_config.Has(kCfgKeyFps)) g_config.SetDouble(kCfgKeyFps, kDefaultFps);
  if (!g_config.Has(kCfgKeyGap)) g_config.SetDouble(kCfgKeyGap, kDefaultGapSeconds);
  if (!g_config.Has(kCfgKeyAnimDir)) g_config.SetString(kCfgKeyAnimDir, "");
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

// ----- Live regions read-back from REAPER -----------------------------------

std::vector<TimelineRegion> ReadLiveRegionsFromReaper() {
  std::vector<TimelineRegion> out;
  const auto& api = g_bridge.Api();
  if (!api.enumProjectMarkers3) return out;

  const int ourColor = AnimationLibrary::RegionColorReaper();
  const std::string prefix = AnimationLibrary::kRegionNamePrefix;

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
      std::string basename;
      if (regionName.rfind(prefix, 0) == 0) {
        basename = regionName.substr(prefix.size());
      } else {
        basename = regionName;  // user renamed — try whole name
      }
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

// ----- Library ↔ regions ----------------------------------------------------

void RebuildLibraryAndRegions(const std::string& dir) {
  std::string log;
  const std::size_t loaded = g_lib.ScanFolder(dir, &log);
  g_lib.BuildRegions(GetFps(), GetGap(), 0.0);
  if (!log.empty()) ConsoleLog(log);
  ConsoleLog("[holoroll] library: " + std::to_string(loaded) +
             " animation(s), " + std::to_string(g_lib.Regions().size()) + " region(s) prepared.\n");

  g_poses.Clear();
  g_activeAnimIdx = kNoActiveAnim;
}

void DeleteOurRegions() {
  const auto& api = g_bridge.Api();
  if (!api.enumProjectMarkers3 || !api.deleteProjectMarker) return;

  const int ourColor = AnimationLibrary::RegionColorReaper();
  const std::string prefix = AnimationLibrary::kRegionNamePrefix;

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
      const bool prefixMatch = name && std::string(name).rfind(prefix, 0) == 0;
      if (colorMatch || prefixMatch) {
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

// ----- Folder dialog --------------------------------------------------------

void RunFolderPicker() {
  HWND owner = g_viewport.IsOpen() ? g_viewport.Hwnd() : nullptr;
  const std::string current = g_lib.Directory();
  const std::string chosen = folder_picker::BrowseForFolder(owner, "Select MDD animations folder", current);
  if (chosen.empty()) return;
  PersistAnimationsDir(chosen);
  RebuildLibraryAndRegions(chosen);
}

// ----- Viewport open/close --------------------------------------------------

void OpenViewportIfNeeded() {
  if (!g_viewport.IsOpen()) {
    if (!g_viewport.Open()) return;
    const auto& api = g_bridge.Api();
    if (api.dockWindowAddEx && api.dockWindowActivate) {
      api.dockWindowAddEx(g_viewport.Hwnd(), kViewportDockTitle, kViewportDockIdent, true);
      api.dockWindowActivate(g_viewport.Hwnd());
    }
  }
}

void CloseViewportIfNeeded() {
  if (g_viewport.IsOpen()) {
    const auto& api = g_bridge.Api();
    if (api.dockWindowRemove) api.dockWindowRemove(g_viewport.Hwnd());
    g_viewport.Close();
  }
}

// ----- Config-driven actions ------------------------------------------------

void OpenConfigInEditor() {
  const std::string path = ConfigFilePath();
  g_config.Save();
  ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ReloadConfigFromDisk() {
  g_config.Load(ConfigFilePath());
  EnsureConfigDefaults();
  const std::string dir = ResolveAnimationsDir();
  if (!dir.empty() && dir != g_lib.Directory()) {
    RebuildLibraryAndRegions(dir);
  } else {
    g_lib.BuildRegions(GetFps(), GetGap(), 0.0);
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

// ----- Per-tick logic -------------------------------------------------------

void OnTimer() {
  g_bridge.OnTimerTick();
  g_viewport.Tick();

  if (!g_viewport.IsOpen()) return;

  const double timelineTime = g_bridge.TimelineTimeSeconds();
  const std::vector<TimelineRegion> liveRegions = ReadLiveRegionsFromReaper();

  GlViewport::OverlayStatus status;
  status.animationsDir = g_lib.Directory();
  status.loadedAnimationCount = g_lib.Count();
  status.regionCount = liveRegions.size();
  status.topologyAvailable = true;

  static const std::vector<float> kEmptyFloats;
  static const std::vector<std::uint32_t> kEmptyIndices;

  const std::vector<float>* vertices = &kEmptyFloats;
  const std::vector<std::uint32_t>* indices = &kEmptyIndices;
  std::uint32_t frame = 0;
  std::uint32_t totalFrames = 0;

  std::size_t animIdx = 0;
  const bool resolved =
      g_lib.ResolvePlayhead(timelineTime, GetFps(), liveRegions, &animIdx, &frame);

  if (resolved) {
    if (g_activeAnimIdx != animIdx) {
      if (g_activeAnimIdx != kNoActiveAnim) {
        ViewportPose& prev = g_poses.Get(g_activeAnimIdx);
        g_viewport.CapturePose(prev);
      }
      ViewportPose& target = g_poses.Get(animIdx);
      if (target.initialized) {
        g_viewport.ApplyPose(target);
      } else {
        g_viewport.CapturePose(target);
      }
      g_activeAnimIdx = animIdx;
    }

    const LoadedAnimation& anim = g_lib.At(animIdx);
    if (anim.mdd && anim.mdd->IsLoaded()) {
      vertices = &anim.mdd->VerticesForFrame(frame);
      totalFrames = anim.mdd->TotalFrames();
      status.currentAnimation = anim.basename;
    }
    if (anim.obj && anim.obj->IsLoaded()) {
      indices = &anim.obj->TriangleIndices();
      status.topologyAvailable = true;
    } else {
      status.topologyAvailable = false;
    }

    for (const auto& r : liveRegions) {
      if (r.animationIndex == animIdx &&
          timelineTime >= r.startSeconds && timelineTime <= r.endSeconds) {
        status.activeRegionStart = r.startSeconds;
        status.activeRegionEnd = r.endSeconds;
        break;
      }
    }
  }

  g_viewport.Render(*vertices, *indices, timelineTime, frame, totalFrames, status);

  if (g_activeAnimIdx != kNoActiveAnim) {
    ViewportPose& live = g_poses.Get(g_activeAnimIdx);
    g_viewport.CapturePose(live);
  }

  const GlViewport::OverlayRequests req = g_viewport.ConsumeRequests();
  if (req.chooseFolder) RunFolderPicker();
  if (req.reloadFolder) {
    if (!g_lib.Directory().empty()) RebuildLibraryAndRegions(g_lib.Directory());
  }
  if (req.placeRegions) PlaceOurRegions();
  if (req.openConfig) OpenConfigInEditor();
  if (req.reloadConfig) ReloadConfigFromDisk();
}

// ----- Action registration helper ------------------------------------------

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
  g_config.Save();

  std::string animDir = ResolveAnimationsDir();
  if (animDir.empty()) {
    animDir = folder_picker::BrowseForFolder(nullptr, "Select MDD animations folder", "");
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
