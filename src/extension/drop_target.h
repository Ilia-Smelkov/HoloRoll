#pragma once

#include <functional>
#include <string>
#include <vector>
#include <windows.h>

// Win32 OLE drag-n-drop integration. Registers a custom IDropTarget on
// our viewport window so the user can drag .mdd / .glb / .obj files
// straight from Windows Explorer onto the 3D scene.
//
// The implementation is intentionally minimal: it only handles file-drops
// (`CF_HDROP`), filters by extension, and forwards the resulting list of
// paths to a C++ callback. All UI / file-move logic lives in the caller
// (entry.cpp); this module is the OLE plumbing only.
//
// Lifetime: call Initialize() once at plugin startup, Shutdown() once at
// plugin teardown. Both are idempotent. RegisterOnHwnd / Unregister can
// be called multiple times (e.g. when the viewport is closed and
// reopened).
namespace drop_target {

// Called on the main thread when the user releases a valid drop. Receives
// the list of file paths (full absolute paths, already filtered to known
// extensions). Empty list means the drop contained nothing we recognise;
// the caller may still want to surface a message to the user.
using DropCallback = std::function<void(const std::vector<std::string>&)>;

// Called by DragEnter to ask the host "would a drop right now be
// acceptable?". Lets entry.cpp veto drops based on project state
// (Untitled projects can't accept files). Returns true to accept,
// false to reject (in which case the visual feedback shows a warning
// state but the drop itself still no-ops).
using AcceptanceQuery = std::function<bool()>;

// Live state of an in-progress drag. Read by the renderer to draw the
// drop-zone overlay. All fields are updated on the main thread by the
// IDropTarget callbacks (which run inside the standard message loop).
struct DragState {
  bool isDragging = false;       // True between DragEnter and DragLeave/Drop.
  bool hasValidFiles = false;    // True if dragged data contains known extensions.
  bool hostAccepts = false;      // Result of AcceptanceQuery callback.
};

// Initialise OLE for this thread. Returns true on success or if OLE was
// already initialised (which is the typical case in REAPER \u2014 REAPER itself
// uses OLE for its own drag-n-drop). False means OLE refuses to come up,
// in which case Register() will silently no-op.
bool Initialize();

// Tear down OLE. Counterpart to Initialize. Calling this without a prior
// Initialize is harmless.
void Shutdown();

// Attach our IDropTarget to `hwnd`. The callback is invoked on every
// successful drop. If a drop target is already attached to this hwnd
// (ours or anyone else's), the existing one is replaced.
//
// `acceptanceQuery` is called once per DragEnter to ask whether a drop
// would currently be acceptable. May be null (in which case all drops
// with valid file types are accepted).
//
// Returns true on success. False means RegisterDragDrop refused (typically
// because OLE didn't init or hwnd is invalid).
bool RegisterOnHwnd(HWND hwnd, DropCallback onDrop, AcceptanceQuery acceptanceQuery = nullptr);

// Detach the drop target from `hwnd`. Safe to call on a window that was
// never registered.
void UnregisterFromHwnd(HWND hwnd);

// Snapshot the current drag state for rendering. Returns a copy so the
// caller doesn't have to worry about it changing mid-frame. Cheap.
DragState GetDragState();

}  // namespace drop_target
