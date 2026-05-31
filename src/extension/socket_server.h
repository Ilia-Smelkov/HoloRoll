// v0.12.0-alpha.11: WAAPI-style TCP socket bridge for external command
// senders (Python apps, custom tooling, etc.).
//
// Protocol: line-delimited JSON over TCP on 127.0.0.1:58271 (default;
// override via REAPERBRIDGE_PORT env var).
//   request : {"method":"<verb>","args":{...}}\n
//   reply   : {"ok":true,"result":{...}}\n
//             or
//             {"ok":false,"error":"..."}\n
// One connection = one command. Client connects, sends request, reads
// reply, closes. Server accepts in a worker thread; the parsed request
// is queued and processed on REAPER's main thread (REAPER C API is NOT
// thread-safe — every reaper.* call must happen there). The worker
// thread then writes the reply and closes.
//
// Verbs implemented: ping, get_selection, get_regions, clear_regions,
// create_regions, run_script. See socket_server.cpp for shapes.
//
// HoloRoll itself does NOT manage user-script libraries — this is just
// a transport layer. The script-loader app on the other side of the
// socket is responsible for telling HoloRoll which script to run.
#pragma once

namespace socket_server {

// Start the Winsock listener + accept worker thread. Idempotent; calling
// twice without a Stop() in between is a no-op (we log a warning).
// Safe to call even if the port is already in use by another process —
// we log the failure and just don't start the bridge.
void Start();

// Stop the listener, join the worker, and clean up Winsock. Pending
// per-connection threads are detached and will exit naturally once
// REAPER unloads the DLL (or their socket timeouts fire).
void Stop();

// Drain the request queue and execute handlers on the calling thread.
// MUST be called from REAPER's main thread (we use that to call into
// the REAPER C API). Typical wiring: invoke from OnTimer.
void Tick();

}  // namespace socket_server
