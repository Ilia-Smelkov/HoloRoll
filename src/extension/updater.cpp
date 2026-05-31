// v0.13.0-alpha.1: in-app auto-updater implementation.
//
// Flow:
//   Start() spawns a worker thread that:
//     1. WinHTTP GET https://api.github.com/repos/.../releases/latest
//     2. Parse JSON, find the HoloRoll-Setup-*.exe asset.
//     3. Compare version vs HOLOROLL_VERSION_STRING (compiled in).
//     4. If newer: download installer to %APPDATA%/REAPER/UserPlugins/
//        HoloRollUpdates/HoloRoll-Setup-X.Y.Z.exe.
//     5. Set g_state.{availableVersion, pendingInstallerPath, ready=true}.
//
//   Stop() — called when plugin is being unloaded (REAPER closing):
//     If ready + auto-install enabled + not dismissed → spawn a
//     detached PowerShell process that:
//       a. waits for reaper.exe to vanish from the process table,
//       b. runs the staged installer with /VERYSILENT /SUPPRESSMSGBOXES,
//       c. exits.
//     Our DLL is unloaded a moment later. Next REAPER start = new
//     version.
//
//   Tick() reserved; currently a no-op.
//
// All REAPER C API interaction stays out of this module — we touch
// only WinHTTP, Win32 file I/O, config, and process spawning. The
// updater is decoupled from REAPER's main thread because none of its
// work needs the REAPER API.

#include <winsock2.h>  // must precede windows.h on this codebase

#include "extension/updater.h"

#include <winhttp.h>
#include <windows.h>
#include <shlobj.h>  // SHGetFolderPathA

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"

#include "extension/version.h"
#include "core/config_store.h"

#pragma comment(lib, "winhttp.lib")

// ---- Linkage shims into entry.cpp ----------------------------------------
//
// Same pattern as socket_server: entry.cpp owns config + log; we
// reach them through tiny file-scope forwarders defined at the end
// of entry.cpp (outside its anonymous namespace).
extern void holoroll_bridge_log(const std::string& msg);
extern ConfigStore& holoroll_config_ref();

// ---- Config keys (kept in sync with entry.cpp's EnsureConfigDefaults) ---
namespace {
constexpr char kCfgEnabled[]              = "update.enabled";
constexpr char kCfgAutoInstallOnClose[]   = "update.auto_install_on_close";
constexpr char kCfgLastCheckUnix[]        = "update.last_check_unix";
constexpr char kCfgPendingInstaller[]     = "update.pending_installer_path";
constexpr char kCfgPendingVersion[]       = "update.pending_version";
constexpr char kCfgDismissedVersion[]     = "update.dismissed_version";

constexpr char kRepoOwner[] = "Ilia-Smelkov";
constexpr char kRepoName[]  = "HoloRoll";

// Subdir under %APPDATA%/REAPER/UserPlugins/ where we stage installers.
constexpr char kStagingSubdir[] = "HoloRollUpdates";
}  // namespace

// ---- Shared state (worker thread → main thread reads via getters) -------
namespace {

struct State {
  std::mutex mtx;
  std::string availableVersion;       // e.g. "0.13.0-alpha.2"
  std::string pendingInstallerPath;   // absolute path to staged .exe
  bool ready = false;                 // downloaded + new + not dismissed
};

State g_state;
std::atomic<bool> g_workerStarted{false};
std::atomic<bool> g_workerDone{false};
std::thread g_workerThread;

}  // namespace

// ---- Version comparator ---------------------------------------------------
//
// Parses MAJOR.MINOR.PATCH[-pre.N] strings and orders them per the
// SemVer 2.0 pre-release rule: any pre-release sorts BELOW the clean
// release. So "1.2.3-alpha.5" < "1.2.3", and within the same x.y.z
// pre-releases compare lexicographically with numeric components
// compared numerically.
//
// We don't support build metadata (+suffix) or chained pre-release
// labels beyond the simple "alpha.N" / "beta.N" / "rc.N" pattern —
// adequate for HoloRoll's tag scheme.
namespace {

struct ParsedVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
  // Empty for clean releases. For pre-releases, this is the suffix
  // AFTER the `-` (e.g. "alpha.15"). Comparison falls back to
  // numeric-aware lexicographic via SemVer rules.
  std::string preRelease;
};

ParsedVersion ParseVersion(const std::string& s) {
  ParsedVersion v;
  std::string core = s;
  const auto dash = core.find('-');
  if (dash != std::string::npos) {
    v.preRelease = core.substr(dash + 1);
    core = core.substr(0, dash);
  }
  // Strip an optional leading 'v'.
  if (!core.empty() && (core[0] == 'v' || core[0] == 'V')) core.erase(0, 1);

  // MAJOR.MINOR.PATCH
  const auto parseInt = [](const std::string& part) -> int {
    if (part.empty()) return 0;
    return std::atoi(part.c_str());
  };
  const auto firstDot  = core.find('.');
  const auto secondDot = (firstDot == std::string::npos) ? std::string::npos
                                                          : core.find('.', firstDot + 1);
  if (firstDot != std::string::npos) {
    v.major = parseInt(core.substr(0, firstDot));
    if (secondDot != std::string::npos) {
      v.minor = parseInt(core.substr(firstDot + 1, secondDot - firstDot - 1));
      v.patch = parseInt(core.substr(secondDot + 1));
    } else {
      v.minor = parseInt(core.substr(firstDot + 1));
    }
  } else {
    v.major = parseInt(core);
  }
  return v;
}

// Compare two pre-release id strings per SemVer:
//   - split on '.'
//   - numeric components compare numerically
//   - alphanumeric components compare lexicographically
//   - numeric < alphanumeric (when the kinds differ)
//   - shorter prefix < longer one when all preceding components equal
int ComparePreRelease(const std::string& a, const std::string& b) {
  if (a == b) return 0;
  std::vector<std::string> partsA, partsB;
  const auto split = [](const std::string& s, std::vector<std::string>& out) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, '.')) out.push_back(item);
  };
  split(a, partsA);
  split(b, partsB);
  const std::size_t n = std::min(partsA.size(), partsB.size());
  for (std::size_t i = 0; i < n; ++i) {
    const std::string& pa = partsA[i];
    const std::string& pb = partsB[i];
    const bool aNum = !pa.empty() && std::all_of(pa.begin(), pa.end(),
                                                  [](char c){ return c >= '0' && c <= '9'; });
    const bool bNum = !pb.empty() && std::all_of(pb.begin(), pb.end(),
                                                  [](char c){ return c >= '0' && c <= '9'; });
    if (aNum && bNum) {
      const int ia = std::atoi(pa.c_str()), ib = std::atoi(pb.c_str());
      if (ia != ib) return (ia < ib) ? -1 : 1;
    } else if (aNum) {
      return -1;  // numeric < alphanumeric
    } else if (bNum) {
      return 1;
    } else {
      if (pa != pb) return (pa < pb) ? -1 : 1;
    }
  }
  if (partsA.size() != partsB.size()) {
    return (partsA.size() < partsB.size()) ? -1 : 1;
  }
  return 0;
}

// -1 if a<b, 0 if equal, +1 if a>b.
int CompareVersions(const ParsedVersion& a, const ParsedVersion& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  // Clean release > pre-release.
  if (a.preRelease.empty() && b.preRelease.empty()) return 0;
  if (a.preRelease.empty()) return 1;
  if (b.preRelease.empty()) return -1;
  return ComparePreRelease(a.preRelease, b.preRelease);
}

}  // namespace

// ---- Helpers --------------------------------------------------------------
namespace {

// Resolve %APPDATA%/REAPER/UserPlugins/. Same logic the rest of the
// plugin uses; duplicated here so updater doesn't depend on entry.cpp.
std::string GetUserPluginsDir() {
  char buf[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return {};
  return std::string(buf) + "\\REAPER\\UserPlugins";
}

std::string GetStagingDir() {
  const std::string base = GetUserPluginsDir();
  if (base.empty()) return {};
  return base + "\\" + kStagingSubdir;
}

bool EnsureDirExists(const std::string& dir) {
  if (dir.empty()) return false;
  const DWORD attr = GetFileAttributesA(dir.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
  // Walk parent first.
  const auto pos = dir.find_last_of("\\/");
  if (pos != std::string::npos && pos != dir.size() - 1) {
    EnsureDirExists(dir.substr(0, pos));
  }
  return CreateDirectoryA(dir.c_str(), nullptr) != 0 ||
         GetLastError() == ERROR_ALREADY_EXISTS;
}

// ---- WinHTTP helpers -----------------------------------------------------
//
// Both helpers return true on success and fill `out` with the response
// body (text variant) or write the response stream to a file (binary
// variant). On any failure we log a single line via the bridge log and
// return false — the worker's caller treats that as "skip this cycle,
// try again next plugin load".
//
// We always follow redirects (GitHub asset URLs redirect to S3) and
// always send a User-Agent header (GitHub API rejects requests without
// one).

bool ParseHttpsUrl(const std::wstring& url, std::wstring& host, std::wstring& path) {
  // Very simple parse: expects "https://<host>/<path>".
  constexpr wchar_t kPrefix[] = L"https://";
  const std::size_t plen = wcslen(kPrefix);
  if (url.size() < plen || url.compare(0, plen, kPrefix) != 0) return false;
  const std::size_t slash = url.find(L'/', plen);
  if (slash == std::wstring::npos) {
    host = url.substr(plen);
    path = L"/";
  } else {
    host = url.substr(plen, slash - plen);
    path = url.substr(slash);
  }
  return !host.empty();
}

bool WinHttpGetText(const std::wstring& url, std::string& outBody) {
  std::wstring host, path;
  if (!ParseHttpsUrl(url, host, path)) {
    holoroll_bridge_log("[holoroll-updater] bad URL: <hidden>\n");
    return false;
  }

  HINTERNET hSession = WinHttpOpen(L"HoloRoll-Updater/1.0",
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) return false;
  // Follow redirects unconditionally — GitHub asset URLs go through S3.
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

  HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  const wchar_t hdrs[] =
      L"Accept: application/vnd.github+json\r\n"
      L"X-GitHub-Api-Version: 2022-11-28\r\n";

  BOOL ok = WinHttpSendRequest(hRequest, hdrs, (DWORD)-1,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

  outBody.clear();
  if (ok) {
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
      std::vector<char> buf(avail);
      DWORD read = 0;
      if (!WinHttpReadData(hRequest, buf.data(), avail, &read) || read == 0) break;
      outBody.append(buf.data(), read);
    }
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return ok && !outBody.empty();
}

bool WinHttpDownloadToFile(const std::wstring& url, const std::string& destPath) {
  std::wstring host, path;
  if (!ParseHttpsUrl(url, host, path)) return false;

  HINTERNET hSession = WinHttpOpen(L"HoloRoll-Updater/1.0",
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) return false;
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

  HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

  bool wrote = false;
  if (ok) {
    std::ofstream f(destPath, std::ios::binary | std::ios::trunc);
    if (f) {
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf.data(), avail, &read) || read == 0) break;
        f.write(buf.data(), read);
      }
      f.flush();
      wrote = static_cast<bool>(f);
    }
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return ok && wrote;
}

// ---- Worker thread body --------------------------------------------------

void WorkerMain() {
  const std::string releasesUrl =
      std::string("https://api.github.com/repos/") + kRepoOwner + "/" +
      kRepoName + "/releases/latest";

  // Convert to wide for WinHTTP.
  std::wstring wUrl(releasesUrl.begin(), releasesUrl.end());

  std::string body;
  if (!WinHttpGetText(wUrl, body)) {
    holoroll_bridge_log("[holoroll-updater] no network or GitHub unreachable; "
                        "will retry on next plugin start.\n");
    g_workerDone.store(true);
    return;
  }

  // Parse the latest release JSON.
  std::string latestTag;
  std::string installerUrl;
  std::string installerName;
  try {
    auto j = nlohmann::json::parse(body);
    if (j.contains("tag_name") && j["tag_name"].is_string()) {
      latestTag = j["tag_name"].get<std::string>();
    }
    if (j.contains("assets") && j["assets"].is_array()) {
      for (const auto& a : j["assets"]) {
        if (!a.contains("name") || !a["name"].is_string()) continue;
        const std::string name = a["name"].get<std::string>();
        // Match HoloRoll-Setup-*.exe — case-sensitive, simple prefix.
        if (name.rfind("HoloRoll-Setup-", 0) == 0 &&
            name.size() >= 4 &&
            name.compare(name.size() - 4, 4, ".exe") == 0) {
          if (a.contains("browser_download_url") &&
              a["browser_download_url"].is_string()) {
            installerUrl = a["browser_download_url"].get<std::string>();
            installerName = name;
            break;
          }
        }
      }
    }
  } catch (const std::exception& e) {
    holoroll_bridge_log(std::string("[holoroll-updater] JSON parse failed: ") +
                        e.what() + "\n");
    g_workerDone.store(true);
    return;
  }

  if (latestTag.empty() || installerUrl.empty()) {
    holoroll_bridge_log("[holoroll-updater] latest release missing tag_name or "
                        "matching installer asset.\n");
    g_workerDone.store(true);
    return;
  }

  // Compare to compiled-in version.
  const ParsedVersion installed = ParseVersion(HOLOROLL_VERSION_STRING);
  const ParsedVersion latest = ParseVersion(latestTag);
  const int cmp = CompareVersions(latest, installed);
  if (cmp <= 0) {
    holoroll_bridge_log(std::string("[holoroll-updater] up to date (installed=") +
                        HOLOROLL_VERSION_STRING + ", latest=" + latestTag + ")\n");
    g_workerDone.store(true);
    return;
  }

  // Newer version available. Download installer to staging.
  const std::string stagingDir = GetStagingDir();
  if (!EnsureDirExists(stagingDir)) {
    holoroll_bridge_log("[holoroll-updater] could not create staging dir.\n");
    g_workerDone.store(true);
    return;
  }
  const std::string destPath = stagingDir + "\\" + installerName;
  std::wstring wInstallerUrl(installerUrl.begin(), installerUrl.end());
  if (!WinHttpDownloadToFile(wInstallerUrl, destPath)) {
    holoroll_bridge_log("[holoroll-updater] installer download failed.\n");
    // Wipe partial file if any.
    DeleteFileA(destPath.c_str());
    g_workerDone.store(true);
    return;
  }

  // Update config + state. The "ready" flag only flips if user hasn't
  // already dismissed THIS specific version.
  ConfigStore& cfg = holoroll_config_ref();
  cfg.SetString(kCfgPendingInstaller, destPath);
  cfg.SetString(kCfgPendingVersion, latestTag);
  cfg.SetDouble(kCfgLastCheckUnix, static_cast<double>(std::time(nullptr)));
  cfg.Save();

  const std::string dismissed = cfg.GetString(kCfgDismissedVersion, std::string{});
  const bool isDismissed = (dismissed == latestTag);

  {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.availableVersion = latestTag;
    g_state.pendingInstallerPath = destPath;
    g_state.ready = !isDismissed;
  }

  holoroll_bridge_log(std::string("[holoroll-updater] downloaded ") + installerName +
                      " (latest=" + latestTag + "); will install on REAPER close" +
                      (isDismissed ? " (dismissed by user)" : "") + ".\n");

  g_workerDone.store(true);
}

// ---- Watchdog spawn ------------------------------------------------------
//
// PowerShell oneliner that:
//   1. Sleeps a brief moment to let REAPER unload our DLL cleanly.
//   2. Polls Get-Process reaper until it's gone (max ~120 s).
//   3. Runs the installer in silent mode.
//   4. Exits.
//
// We spawn detached with DETACHED_PROCESS so it survives our plugin
// unload. CREATE_NO_WINDOW keeps the console hidden.
void SpawnInstallerWatchdog(const std::string& installerPath) {
  if (installerPath.empty()) return;

  // PowerShell-escape single quotes in the path by doubling them.
  std::string escaped;
  escaped.reserve(installerPath.size() + 8);
  for (char c : installerPath) {
    if (c == '\'') escaped += "''";
    else escaped += c;
  }

  std::string script;
  script.reserve(512 + escaped.size());
  script +=
      "Start-Sleep -Seconds 2; "
      "$deadline = (Get-Date).AddSeconds(120); "
      "while ((Get-Process reaper -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { "
      "  Start-Sleep -Milliseconds 500 "
      "}; "
      "if (-not (Get-Process reaper -ErrorAction SilentlyContinue)) { "
      "  Start-Process -FilePath '";
  script += escaped;
  script += "' -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait "
            "}";

  std::string cmd = "powershell.exe -NoProfile -WindowStyle Hidden -Command \"";
  cmd += script;
  cmd += "\"";

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  // Mutable cmd buffer required by CreateProcessA.
  std::vector<char> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back('\0');

  const BOOL ok = CreateProcessA(
      nullptr,
      cmdBuf.data(),
      nullptr, nullptr,
      FALSE,
      DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
      nullptr, nullptr,
      &si, &pi);

  if (ok) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    holoroll_bridge_log("[holoroll-updater] watchdog spawned; installer will run "
                        "after REAPER exits.\n");
  } else {
    holoroll_bridge_log(std::string("[holoroll-updater] failed to spawn watchdog (") +
                        std::to_string(GetLastError()) + ")\n");
  }
}

}  // namespace

// ---- Public API -----------------------------------------------------------

namespace updater {

void Start() {
  if (g_workerStarted.exchange(true)) return;  // already started this session
  ConfigStore& cfg = holoroll_config_ref();
  if (cfg.GetDouble(kCfgEnabled, 1.0) < 0.5) {
    holoroll_bridge_log("[holoroll-updater] update.enabled=0 — skipping check.\n");
    g_workerDone.store(true);
    return;
  }
  // Hydrate UI state from previously-persisted "pending" record so the
  // banner can show immediately on plugin load (no need to wait for
  // the network round-trip). Worker will re-confirm in a moment.
  const std::string pendVer = cfg.GetString(kCfgPendingVersion, std::string{});
  const std::string pendPath = cfg.GetString(kCfgPendingInstaller, std::string{});
  if (!pendVer.empty() && !pendPath.empty()) {
    // Sanity: file still exists? (Could be wiped by user between runs.)
    const DWORD attr = GetFileAttributesA(pendPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
      // Compare to compiled version — pending might already be
      // installed (user updated some other way) or older than us.
      const ParsedVersion installed = ParseVersion(HOLOROLL_VERSION_STRING);
      const ParsedVersion pending = ParseVersion(pendVer);
      if (CompareVersions(pending, installed) > 0) {
        const std::string dismissed = cfg.GetString(kCfgDismissedVersion, std::string{});
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.availableVersion = pendVer;
        g_state.pendingInstallerPath = pendPath;
        g_state.ready = (dismissed != pendVer);
      } else {
        // Stale — clean up the persisted "pending" pointer.
        cfg.SetString(kCfgPendingInstaller, std::string{});
        cfg.SetString(kCfgPendingVersion, std::string{});
        cfg.Save();
        DeleteFileA(pendPath.c_str());
      }
    }
  }

  g_workerThread = std::thread(WorkerMain);
}

void Stop() {
  // Join worker first so it's not racing with our shutdown decision.
  if (g_workerThread.joinable()) g_workerThread.join();

  ConfigStore& cfg = holoroll_config_ref();
  if (cfg.GetDouble(kCfgAutoInstallOnClose, 1.0) < 0.5) return;

  std::string installer;
  bool ready = false;
  {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    installer = g_state.pendingInstallerPath;
    ready = g_state.ready;
  }
  if (ready && !installer.empty()) {
    SpawnInstallerWatchdog(installer);
  }
}

void Tick() {
  // Reserved for future use (periodic re-check, progress UI updates).
  // alpha.1 only polls once at Start.
}

bool HasReadyUpdate() {
  std::lock_guard<std::mutex> lk(g_state.mtx);
  return g_state.ready;
}

std::string AvailableVersion() {
  std::lock_guard<std::mutex> lk(g_state.mtx);
  return g_state.ready ? g_state.availableVersion : std::string{};
}

std::string StatusText() {
  std::lock_guard<std::mutex> lk(g_state.mtx);
  if (!g_state.ready) return {};
  ConfigStore& cfg = holoroll_config_ref();
  if (cfg.GetDouble(kCfgAutoInstallOnClose, 1.0) >= 0.5) {
    return "Will install silently when you close REAPER.";
  }
  return "Auto-install disabled; run the installer manually.";
}

void DismissCurrentVersion() {
  std::string ver;
  {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    ver = g_state.availableVersion;
    g_state.ready = false;
  }
  if (!ver.empty()) {
    ConfigStore& cfg = holoroll_config_ref();
    cfg.SetString(kCfgDismissedVersion, ver);
    cfg.Save();
  }
}

}  // namespace updater
