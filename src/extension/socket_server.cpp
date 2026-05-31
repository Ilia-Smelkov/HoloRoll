// v0.12.0-alpha.11: WAAPI-style TCP socket bridge implementation.
//
// See socket_server.h for the protocol and rationale. Architecture:
//
//   accept loop (worker thread)
//       │
//       ▼
//   per-connection handler (detached thread)
//       │  1. read one '\n'-terminated request line from socket
//       │  2. parse JSON; if invalid, write error reply + close
//       │  3. push PendingRequest{method, args, std::promise<reply>}
//       │     onto g_queue (mutex-guarded)
//       │  4. wait on the request's std::future (30s timeout)
//       │  5. write reply line back to socket, close
//       ▼
//   Tick() on REAPER's main thread
//       │  pops every pending request, executes its handler (which
//       │  calls into the REAPER C API — main thread only!), sets
//       │  the promise value; per-connection thread wakes up.
//       ▼
//   reply sent, socket closed

// CRITICAL: winsock2.h must come before windows.h on Windows. WIN32_LEAN_
// AND_MEAN (defined at the build-system level in CMakeLists.txt) keeps
// windows.h from pulling in the legacy winsock 1 headers itself, but the
// safest pattern is still "winsock2 first".
#include <winsock2.h>
#include <ws2tcpip.h>

#include "extension/socket_server.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// nlohmann/json comes bundled with tinygltf 2.x — both end up under
// ${tinygltf_SOURCE_DIR}, which is already on our include path
// (see CMakeLists.txt target_include_directories). No extra FetchContent
// dependency needed.
#include "json.hpp"

#include "extension/reaper_bridge.h"

// ---- Linkage shims --------------------------------------------------------
//
// entry.cpp owns the live ReaperBridge instance and the ConsoleLog
// helper, but both sit inside its anonymous namespace (internal
// linkage). Two file-scope forwarders are defined at the bottom of
// entry.cpp; their declarations:
extern const ReaperApi& holoroll_bridge_api();
extern void holoroll_bridge_log(const std::string& msg);

namespace {

using nlohmann::json;

// Local proxies so handler code below can keep using `g_bridge.Api()`
// and `ConsoleLog(...)` exactly as it would inside entry.cpp itself.
// Zero runtime cost — both inline to the shim call.
struct BridgeProxy {
  const ReaperApi& Api() const { return holoroll_bridge_api(); }
};
BridgeProxy g_bridge;
void ConsoleLog(const std::string& msg) { holoroll_bridge_log(msg); }

// ---- Pending request queue (worker -> main thread) ------------------------

struct PendingRequest {
  std::string method;
  json args = json::object();
  // The per-connection worker waits on this future. Main-thread Tick sets
  // the value. Carrying a serialized string (not just a json) means the
  // worker doesn't have to touch nlohmann from a possibly-different
  // thread once the handler finishes — simpler ownership story.
  std::promise<std::string> response;
};

std::mutex g_queueMutex;
std::queue<std::shared_ptr<PendingRequest>> g_queue;

// ---- Listener state -------------------------------------------------------

std::atomic<bool> g_running{false};
SOCKET g_listenerSock = INVALID_SOCKET;
std::thread g_acceptThread;
int g_listenPort = 0;
bool g_wsaStarted = false;

// Verb dispatch returns the serialized reply line (without trailing '\n';
// the caller appends it before send). Any std::exception thrown by a
// handler is converted to a {"ok":false,"error":"..."} reply. The
// distinction between handler-thrown VerbError and other exceptions is
// only used for the error message: VerbError messages are surfaced
// verbatim; std::exception messages get prefixed with "internal:".
struct VerbError : std::runtime_error { using std::runtime_error::runtime_error; };

// ---- Verb handlers --------------------------------------------------------
//
// Each handler runs on the REAPER main thread (called from Tick()). They
// can freely call g_bridge.Api().* — that's the whole point of the
// queue-based hand-off. Failures throw VerbError; the wrapper turns
// throws into JSON error replies.

json HandlePing(const json& /*args*/) {
  return json::object();
}

json HandleGetSelection(const json& /*args*/) {
  const auto& api = g_bridge.Api();
  json items = json::array();
  if (!api.countSelectedMediaItems || !api.getSelectedMediaItem ||
      !api.getMediaItemInfo_Value) {
    return json{{"items", items}};
  }
  const int n = api.countSelectedMediaItems(nullptr);
  for (int i = 0; i < n; ++i) {
    MediaItem* item = api.getSelectedMediaItem(nullptr, i);
    if (!item) continue;
    MediaItem_Take* take = api.getActiveTake ? api.getActiveTake(item) : nullptr;
    // Skip MIDI takes — caller asked for media items, MIDI doesn't fit
    // the {name, position, length} shape meaningfully.
    if (take && api.takeIsMIDI && api.takeIsMIDI(take)) continue;
    std::string name;
    if (take && api.getTakeName) {
      const char* nm = api.getTakeName(take);
      if (nm) name = nm;
    }
    const double pos = api.getMediaItemInfo_Value(item, "D_POSITION");
    const double len = api.getMediaItemInfo_Value(item, "D_LENGTH");
    items.push_back({
        {"name", name},
        {"position", pos},
        {"length", len},
    });
  }
  return json{{"items", items}};
}

json HandleGetRegions(const json& /*args*/) {
  const auto& api = g_bridge.Api();
  json regions = json::array();
  if (!api.enumProjectMarkers3) return json{{"regions", regions}};
  int idx = 0;
  while (true) {
    bool isrgn = false;
    double pos = 0.0, rgnend = 0.0;
    const char* nm = nullptr;
    int rgnIdx = 0, color = 0;
    const int next = api.enumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend,
                                             &nm, &rgnIdx, &color);
    if (next == 0) break;
    if (isrgn) {
      regions.push_back({
          {"name", nm ? std::string(nm) : std::string()},
          {"start", pos},
          {"end", rgnend},
          {"index", rgnIdx},
      });
    }
    idx = next;
  }
  return json{{"regions", regions}};
}

json HandleClearRegions(const json& /*args*/) {
  const auto& api = g_bridge.Api();
  if (!api.enumProjectMarkers3 || !api.deleteProjectMarker) {
    return json{{"deleted", 0}};
  }
  std::vector<int> toDelete;
  int idx = 0;
  while (true) {
    bool isrgn = false;
    double pos = 0.0, rgnend = 0.0;
    const char* nm = nullptr;
    int rgnIdx = 0, color = 0;
    const int next = api.enumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend,
                                             &nm, &rgnIdx, &color);
    if (next == 0) break;
    if (isrgn) toDelete.push_back(rgnIdx);
    idx = next;
  }
  // Marker IDs are stable inside REAPER's marker table; deleting in any
  // order is safe (unlike position-indexed enumeration).
  for (const int id : toDelete) {
    api.deleteProjectMarker(nullptr, id, /*isrgn=*/true);
  }
  return json{{"deleted", static_cast<int>(toDelete.size())}};
}

json HandleCreateRegions(const json& args) {
  const auto& api = g_bridge.Api();
  if (!api.addProjectMarker2) {
    throw VerbError("REAPER API 'AddProjectMarker2' not available");
  }

  // Wrap the whole thing in an undo block + UI-refresh suppression so
  // bulk creates land as one undo step and don't repaint per-region.
  // If either undo/refresh API is unavailable we degrade gracefully —
  // the work still happens, just without the wrapping.
  if (api.undo_BeginBlock) api.undo_BeginBlock();
  if (api.preventUIRefresh) api.preventUIRefresh(1);

  // Reposition selected items FIRST so subsequent region positions can
  // be expressed relative to the moved items.
  if (args.contains("reposition") && args["reposition"].is_array()) {
    for (const auto& r : args["reposition"]) {
      if (!r.is_object()) continue;
      if (!r.contains("index") || !r.contains("position")) continue;
      const int selIdx = r["index"].is_number_integer()
                             ? r["index"].get<int>()
                             : static_cast<int>(r["index"].get<double>());
      const double pos = r["position"].get<double>();
      if (api.getSelectedMediaItem && api.setMediaItemInfo_Value) {
        MediaItem* item = api.getSelectedMediaItem(nullptr, selIdx);
        if (item) api.setMediaItemInfo_Value(item, "D_POSITION", pos);
      }
    }
  }

  // Create regions.
  int created = 0;
  if (args.contains("regions") && args["regions"].is_array()) {
    for (const auto& rg : args["regions"]) {
      if (!rg.is_object()) continue;
      if (!rg.contains("name") || !rg.contains("start") || !rg.contains("end")) continue;
      const std::string name = rg["name"].get<std::string>();
      const double start = rg["start"].get<double>();
      const double end   = rg["end"].get<double>();
      api.addProjectMarker2(nullptr, /*isrgn=*/true, start, end,
                            name.c_str(), /*wantidx=*/-1, /*color=*/0);
      ++created;
    }
  }

  if (api.preventUIRefresh) api.preventUIRefresh(-1);
  if (api.updateArrange) api.updateArrange();
  if (api.undo_EndBlock) api.undo_EndBlock("HoloRoll bridge: create regions", -1);

  return json{{"created", created}};
}

json HandleRunScript(const json& args) {
  const auto& api = g_bridge.Api();
  if (!api.addRemoveReaScript || !api.mainOnCommand) {
    throw VerbError("REAPER API 'AddRemoveReaScript' or 'Main_OnCommand' not available");
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    throw VerbError("missing or invalid 'path' argument (expected string)");
  }
  const std::string path = args["path"].get<std::string>();

  // Section 0 = main scripts section. commit=true persists the registration
  // (we don't unregister afterwards — REAPER will pick up the script in
  // the action list, which the user can re-run from there if desired).
  const int cmd = api.addRemoveReaScript(/*isAdd=*/true, /*sectionID=*/0,
                                          path.c_str(), /*commit=*/true);
  if (cmd == 0) {
    throw VerbError("AddRemoveReaScript returned 0 — script missing or "
                    "could not be registered: " + path);
  }
  api.mainOnCommand(cmd, 0);
  return json{{"ran", true}};
}

// ---- v0.12.0-alpha.15 verbs ----------------------------------------------
//
// register_action / script_shortcut / assign_shortcut all share the same
// "ensure registered, get numeric cmd id" prelude. AddRemoveReaScript
// is idempotent: re-registering the same path returns the same command
// id, so calling it from every verb is cheap and safe.

// Section 0 == main keyboard section. Hardcoded here because that's where
// scripts land via AddRemoveReaScript(sectionID=0).
constexpr int kMainSectionUniqueId = 0;

// REAPER native action ID for "Show action list". Hardcoded — there's no
// stable named-command equivalent. If a future REAPER renumbers this we'll
// need to adjust; verified against REAPER 6.x / 7.x as of authoring.
constexpr int kShowActionListCmd = 40605;

// Shared helper: register the script and return the numeric command id.
// Throws VerbError on missing API / failed registration.
int EnsureScriptRegistered(const std::string& path) {
  const auto& api = g_bridge.Api();
  if (!api.addRemoveReaScript) {
    throw VerbError("REAPER API 'AddRemoveReaScript' not available");
  }
  const int cmd = api.addRemoveReaScript(/*isAdd=*/true,
                                          /*sectionID=*/kMainSectionUniqueId,
                                          path.c_str(), /*commit=*/true);
  if (cmd == 0) {
    throw VerbError("AddRemoveReaScript returned 0 — script missing or "
                    "could not be registered: " + path);
  }
  return cmd;
}

// Get the named command id ("_RS<hash>") for a numeric command id, or
// empty string if the command isn't a named one. ReverseNamedCommandLookup
// returns the name WITHOUT the leading underscore; we prepend it so the
// returned string matches the form used in reaper-kb.ini and the form
// callers usually paste into their own configs.
std::string NamedIdForCommand(int cmd) {
  const auto& api = g_bridge.Api();
  if (!api.reverseNamedCommandLookup) return {};
  const char* raw = api.reverseNamedCommandLookup(cmd);
  if (!raw || !raw[0]) return {};
  return std::string("_") + raw;
}

json HandleRegisterAction(const json& args) {
  if (!args.contains("path") || !args["path"].is_string()) {
    throw VerbError("missing or invalid 'path' argument (expected string)");
  }
  const std::string path = args["path"].get<std::string>();
  const int cmd = EnsureScriptRegistered(path);
  const std::string namedId = NamedIdForCommand(cmd);
  return json{{"command_id", namedId}};
}

json HandleScriptShortcut(const json& args) {
  const auto& api = g_bridge.Api();
  if (!args.contains("path") || !args["path"].is_string()) {
    throw VerbError("missing or invalid 'path' argument (expected string)");
  }
  const std::string path = args["path"].get<std::string>();
  const int cmd = EnsureScriptRegistered(path);
  const std::string namedId = NamedIdForCommand(cmd);

  // Look up the shortcut text. We grab the FIRST registered shortcut
  // (shortcutidx=0) — REAPER supports multiple bindings per action but
  // most users assign just one; surfacing all of them would force the
  // caller into an awkward array shape for the common case.
  std::string shortcutText;
  if (api.sectionFromUniqueID && api.countActionShortcuts && api.getActionShortcutDesc) {
    KbdSectionInfo* section = api.sectionFromUniqueID(kMainSectionUniqueId);
    if (section && api.countActionShortcuts(section, cmd) > 0) {
      char buf[256] = {};
      if (api.getActionShortcutDesc(section, cmd, /*shortcutidx=*/0,
                                    buf, sizeof(buf))) {
        shortcutText = buf;
      }
    }
  }

  return json{{"shortcut", shortcutText}, {"command_id", namedId}};
}

json HandleAssignShortcut(const json& args) {
  const auto& api = g_bridge.Api();
  if (!args.contains("path") || !args["path"].is_string()) {
    throw VerbError("missing or invalid 'path' argument (expected string)");
  }
  const std::string path = args["path"].get<std::string>();
  // Register so the script appears in the action list with a stable id;
  // user can then assign / clear shortcuts directly via REAPER's dialog.
  EnsureScriptRegistered(path);

  if (!api.mainOnCommand) {
    throw VerbError("REAPER API 'Main_OnCommand' not available");
  }
  // Just open the action list. We deliberately don't try to filter or
  // pre-select — REAPER's own shortcut-assignment / conflict-resolution
  // dialog flow takes over from here. Optional "copy script name to
  // clipboard for quick filter" — skipping for now, can be added if
  // it turns out to be friction in practice.
  api.mainOnCommand(kShowActionListCmd, 0);
  return json{{"opened", true}};
}

json HandleGetCursor(const json& /*args*/) {
  const auto& api = g_bridge.Api();
  // Prefer the non-Ex form (no project arg, always current project) —
  // matches the spec verbatim. Fall back to GetCursorPositionEx(nullptr)
  // for older REAPER builds that somehow only export the Ex variant.
  double pos = 0.0;
  if (api.getCursorPosition) {
    pos = api.getCursorPosition();
  } else if (api.getCursorPositionEx) {
    pos = api.getCursorPositionEx(nullptr);
  } else {
    throw VerbError("REAPER API 'GetCursorPosition' not available");
  }
  return json{{"position", pos}};
}

// Build the reply line for a request. Catches handler exceptions and
// converts them to {"ok":false,"error":"..."} replies.
std::string DispatchVerb(const std::string& method, const json& args) {
  try {
    json result;
    if      (method == "ping")             result = HandlePing(args);
    else if (method == "get_selection")    result = HandleGetSelection(args);
    else if (method == "get_regions")      result = HandleGetRegions(args);
    else if (method == "clear_regions")    result = HandleClearRegions(args);
    else if (method == "create_regions")   result = HandleCreateRegions(args);
    else if (method == "run_script")       result = HandleRunScript(args);
    // v0.12.0-alpha.15:
    else if (method == "register_action")  result = HandleRegisterAction(args);
    else if (method == "script_shortcut")  result = HandleScriptShortcut(args);
    else if (method == "assign_shortcut")  result = HandleAssignShortcut(args);
    else if (method == "get_cursor")       result = HandleGetCursor(args);
    else throw VerbError("unknown_method: " + method);

    return json{{"ok", true}, {"result", result}}.dump();
  } catch (const VerbError& e) {
    return json{{"ok", false}, {"error", e.what()}}.dump();
  } catch (const std::exception& e) {
    return json{{"ok", false},
                {"error", std::string("internal: ") + e.what()}}.dump();
  } catch (...) {
    return json{{"ok", false}, {"error", "internal: unknown exception"}}.dump();
  }
}

// ---- Socket I/O helpers ---------------------------------------------------

// Read one line (terminated by '\n', not included in returned string)
// from `sock`. Caps at maxBytes to avoid memory blow-up from a hostile
// or buggy client. Returns empty string on EOF / error before any data
// arrived; partial-line-then-EOF returns whatever we got.
std::string RecvLine(SOCKET sock, std::size_t maxBytes) {
  std::string out;
  char buf[4096];
  while (out.size() < maxBytes) {
    const int want = static_cast<int>(std::min<std::size_t>(sizeof(buf), maxBytes - out.size()));
    const int n = recv(sock, buf, want, 0);
    if (n <= 0) break;  // EOF or error
    for (int i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        out.append(buf, i);
        return out;
      }
    }
    out.append(buf, n);
  }
  return out;
}

// Send all bytes; loop until done or error. Returns true on full send.
bool SendAll(SOCKET sock, const char* data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
    const int n = send(sock, data + sent,
                       static_cast<int>(len - sent), 0);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// ---- Per-connection handler (runs on detached worker thread) -------------

void HandleConnection(SOCKET sock) {
  // Recv timeout — abandons clients that connect and never send anything.
  // Send timeout same idea.
  DWORD timeoutMs = 30000;  // 30 seconds
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

  // Read the request line. 1 MB cap is generous for any reasonable
  // command — far above the size of any of our six verbs.
  const std::string line = RecvLine(sock, 1024 * 1024);
  if (line.empty()) {
    closesocket(sock);
    return;
  }

  // Parse JSON. Bad input -> immediate error reply.
  std::string method;
  json args = json::object();
  try {
    json req = json::parse(line);
    if (!req.is_object()) throw std::runtime_error("request must be a JSON object");
    if (!req.contains("method") || !req["method"].is_string()) {
      throw std::runtime_error("missing 'method' (string)");
    }
    method = req["method"].get<std::string>();
    if (req.contains("args")) {
      if (!req["args"].is_object()) {
        throw std::runtime_error("'args' must be an object if present");
      }
      args = req["args"];
    }
  } catch (const std::exception& e) {
    const std::string reply = json{{"ok", false},
                                    {"error", std::string("parse_error: ") + e.what()}}
                                  .dump() + "\n";
    SendAll(sock, reply.data(), reply.size());
    closesocket(sock);
    return;
  }

  // Hand off to main thread via the queue. The handler runs in Tick();
  // we wait here on the future for up to 30 s.
  auto pending = std::make_shared<PendingRequest>();
  pending->method = std::move(method);
  pending->args = std::move(args);
  auto future = pending->response.get_future();
  {
    std::lock_guard<std::mutex> lk(g_queueMutex);
    g_queue.push(pending);
  }

  std::string reply;
  if (future.wait_for(std::chrono::seconds(30)) == std::future_status::ready) {
    try {
      reply = future.get();
    } catch (const std::exception& e) {
      reply = json{{"ok", false},
                   {"error", std::string("internal: ") + e.what()}}.dump();
    } catch (...) {
      reply = json{{"ok", false},
                   {"error", "internal: unknown exception"}}.dump();
    }
  } else {
    reply = json{{"ok", false},
                 {"error", "timeout: main thread did not dispatch within 30s"}}.dump();
  }
  reply.push_back('\n');

  SendAll(sock, reply.data(), reply.size());
  closesocket(sock);
}

// ---- Accept loop (runs on the single worker thread) -----------------------

void AcceptLoop() {
  while (g_running.load()) {
    sockaddr_in clientAddr{};
    int addrLen = sizeof(clientAddr);
    SOCKET client = accept(g_listenerSock,
                           reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
    if (client == INVALID_SOCKET) {
      // Listener was closed by Stop(), or accept failed transiently.
      if (!g_running.load()) break;
      // Brief sleep so a persistently-erroring listener doesn't peg CPU.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    // Detached per-connection thread. We don't track them — if the
    // plugin unloads while a connection is in flight, REAPER's process
    // teardown ends them; otherwise they exit on their own after
    // send/close.
    std::thread(HandleConnection, client).detach();
  }
}

// ---- Helpers --------------------------------------------------------------

int ReadPortFromEnv(int defaultPort) {
  char buf[16] = {};
  const DWORD n = GetEnvironmentVariableA("REAPERBRIDGE_PORT", buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return defaultPort;
  const int p = std::atoi(buf);
  if (p <= 0 || p >= 65536) return defaultPort;
  return p;
}

}  // namespace

namespace socket_server {

void Start() {
  if (g_running.load()) {
    ConsoleLog("[holoroll-bridge] Start: already running, ignoring.\n");
    return;
  }

  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    ConsoleLog("[holoroll-bridge] WSAStartup failed (code " +
               std::to_string(WSAGetLastError()) + ").\n");
    return;
  }
  g_wsaStarted = true;

  const int port = ReadPortFromEnv(58271);

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    ConsoleLog("[holoroll-bridge] socket() failed (code " +
               std::to_string(WSAGetLastError()) + ").\n");
    WSACleanup();
    g_wsaStarted = false;
    return;
  }

  // SO_REUSEADDR lets us re-bind quickly across plugin reloads. Without
  // it, a recent close on the same (addr, port) tuple can keep the slot
  // in TIME_WAIT for ~2 minutes.
  BOOL reuse = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
  addr.sin_port = htons(static_cast<u_short>(port));

  if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    ConsoleLog("[holoroll-bridge] bind to 127.0.0.1:" + std::to_string(port) +
               " failed (code " + std::to_string(WSAGetLastError()) +
               "). Another process probably holds the port — set "
               "REAPERBRIDGE_PORT env var to override.\n");
    closesocket(listener);
    WSACleanup();
    g_wsaStarted = false;
    return;
  }

  if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
    ConsoleLog("[holoroll-bridge] listen() failed (code " +
               std::to_string(WSAGetLastError()) + ").\n");
    closesocket(listener);
    WSACleanup();
    g_wsaStarted = false;
    return;
  }

  g_listenerSock = listener;
  g_listenPort = port;
  g_running.store(true);
  g_acceptThread = std::thread(AcceptLoop);

  ConsoleLog("[holoroll-bridge] listening on 127.0.0.1:" +
             std::to_string(port) + " (override via REAPERBRIDGE_PORT).\n");
}

void Stop() {
  if (!g_running.load()) {
    if (g_wsaStarted) { WSACleanup(); g_wsaStarted = false; }
    return;
  }
  g_running.store(false);

  // Close listener — this unblocks accept() with WSAEINTR / WSAENOTSOCK.
  if (g_listenerSock != INVALID_SOCKET) {
    closesocket(g_listenerSock);
    g_listenerSock = INVALID_SOCKET;
  }
  if (g_acceptThread.joinable()) g_acceptThread.join();

  // Per-connection threads are detached — we don't join them. Any
  // in-flight ones will time out on their future (30 s) and exit, OR
  // they'll be killed by the DLL unload when REAPER tears down.

  if (g_wsaStarted) {
    WSACleanup();
    g_wsaStarted = false;
  }

  ConsoleLog("[holoroll-bridge] stopped.\n");
}

void Tick() {
  // Drain everything in one go. The lock is held briefly — only while
  // moving requests out of the queue; handler execution happens
  // unlocked so other workers can keep enqueueing.
  std::vector<std::shared_ptr<PendingRequest>> batch;
  {
    std::lock_guard<std::mutex> lk(g_queueMutex);
    while (!g_queue.empty()) {
      batch.push_back(std::move(g_queue.front()));
      g_queue.pop();
    }
  }
  for (auto& req : batch) {
    std::string reply;
    try {
      reply = DispatchVerb(req->method, req->args);
    } catch (...) {
      // Should never happen — DispatchVerb catches everything — but
      // belt-and-braces so a stray exception doesn't deadlock the
      // worker waiting on the promise.
      try { req->response.set_exception(std::current_exception()); }
      catch (...) {}
      continue;
    }
    try { req->response.set_value(std::move(reply)); }
    catch (...) {
      // Promise already satisfied / broken — shouldn't happen, ignore.
    }
  }
}

}  // namespace socket_server
