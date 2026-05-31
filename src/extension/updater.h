// v0.13.0-alpha.1: in-app auto-updater. Polls GitHub Releases for
// newer versions, downloads the installer to a staging directory,
// and (if the user hasn't dismissed and auto-install is enabled)
// spawns a detached PowerShell watchdog on plugin shutdown that
// waits for REAPER to exit, then runs the installer in silent mode.
//
// Architecture rationale (see ROADMAP.md / CHANGELOG for the long
// version): a loaded DLL can't replace itself, so we hand the
// install off to an out-of-process helper that runs AFTER REAPER
// has fully exited. PowerShell is shipped with Windows — no extra
// helper binary to maintain.
//
// All state is owned internally; entry.cpp only knows about
// Start / Stop / Tick / Trigger and the small "what should the UI
// show right now" snapshot.
#pragma once

#include <string>

namespace updater {

// Initialize. Spawns a background worker thread that does the
// GitHub poll + download, then exits. Safe to call repeatedly —
// subsequent calls are no-ops (idempotent).
void Start();

// Stop. If a pending installer exists and auto-install is enabled
// (and the user hasn't dismissed this version), spawn the detached
// watchdog HERE so the installer fires when REAPER finishes
// closing. Otherwise just cleans up worker thread state. Idempotent.
void Stop();

// Called every OnTimer tick on the main thread. alpha.2: if no worker
// is currently running and 24+ hours have passed since the last
// successful check, kicks off a re-poll. Otherwise no-op.
void Tick();

// Manual "Check for updates now" — forces a poll regardless of the
// 24h cooldown. Still respects `update.enabled` (master toggle):
// if updates are off in config, this is a logged no-op.
// Idempotent: clicking the button while a check is already in
// progress is a no-op.
void CheckNow();

// ---- UI snapshot ----
//
// Read-only state for the overlay banner. All getters return
// immediately (atomic reads); safe to call every render frame.

// True iff a newer version has been detected AND its installer
// has been successfully downloaded AND the user has not dismissed
// this specific version. The banner shows iff this returns true.
bool HasReadyUpdate();

// E.g. "0.13.0-alpha.2". Empty if HasReadyUpdate() == false.
std::string AvailableVersion();

// "Update will install when REAPER closes." OR diagnostic text
// for the rare state where the user has disabled auto-install
// but still has a pending update. Empty if HasReadyUpdate() == false.
std::string StatusText();

// User clicked "Dismiss for this version" in the banner. The
// pending update stays staged (we might re-prompt for it next
// run) but the banner hides until the next NEWER version arrives.
void DismissCurrentVersion();

}  // namespace updater
