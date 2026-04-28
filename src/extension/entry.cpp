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

void EnsureConfigDefaults() {
  if (!g_config.Has(kCfgKeyFps)) g_config.SetDouble(kCfgKeyFps, kDefaultFps);
  if (!g_config.Has(kCfgKeyGap)) g_config.SetDouble(kCfgKeyGap, kDefaultGapSeconds);
  if (!g_config.Has(kCfgKeyAnimDir)) g_config.SetString(kCfgKeyAnimDir, "");
  if (!g_config.Has(kCfgKeySceneShowGround)) g_config.SetDouble(kCfgKeySceneShowGround, 1.0);
  if (!g_config.Has(kCfgKeySceneRadius))     g_config.SetDouble(kCfgKeySceneRadius, 20.0);
  if (!g_config.Has(kCfgKeySceneGridStep))   g_config.SetDouble(kCfgKeySceneGridStep, 1.0);
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
        basename = regionName;
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

void RunFolderPicker() {
  HWND owner = g_viewport.IsOpen() ? g_viewport.Hwnd() : nullptr;
  const std::string current = g_lib.Directory();
  const std::string chosen = folder_picker::BrowseForFolder(owner, "Select MDD animations folder", current);
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
  const std::string dir = ResolveAnimationsDir();
  if (!dir.empty() && dir != g_lib.Directory()) {
    RebuildLibraryAndRegions(dir);
  } else {
    g_lib.BuildRegions(GetFps(), GetGap(), 0.0);
  }
  // Pick up scene settings the user may have edited in the file.
  if (g_viewport.IsOpen()) ApplySceneSettingsToViewport();
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
    // IMPORTANT: status.autoPivot / autoExtent must be populated BEFORE
    // we touch pose state, because the first-time "fresh pose" branch
    // calls ResetCameraToDefault(status), which needs those values to
    // place the camera at the correct distance from the bbox.
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

    status.autoPivot[0] = anim.autoPivot[0];
    status.autoPivot[1] = anim.autoPivot[1];
    status.autoPivot[2] = anim.autoPivot[2];
    status.autoExtent = anim.autoExtent;
    status.restNormals = &anim.restNormals;

    for (const auto& r : liveRegions) {
      if (r.animationIndex == animIdx &&
          timelineTime >= r.startSeconds && timelineTime <= r.endSeconds) {
        status.activeRegionStart = r.startSeconds;
        status.activeRegionEnd = r.endSeconds;
        break;
      }
    }

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
  if (req.placeRegions) PlaceOurRegions();
  if (req.openConfig) OpenConfigInEditor();
  if (req.reloadConfig) ReloadConfigFromDisk();
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
