#include "extension/drop_target.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ole2.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>

namespace drop_target {
namespace {

// Global drag state. Updated by IDropTarget callbacks on the main thread
// during a drag operation; read by GetDragState() from the renderer.
// Atomics give us lock-free reads at ~30 Hz from the render path without
// stalling the message loop.
std::atomic<bool> g_isDragging{false};
std::atomic<bool> g_hasValidFiles{false};
std::atomic<bool> g_hostAccepts{false};

void ResetDragState() {
  g_isDragging.store(false);
  g_hasValidFiles.store(false);
  g_hostAccepts.store(false);
}

// Track OLE init state so Shutdown matches Initialize. REAPER may have
// already called OleInitialize on this thread, in which case our call
// returns S_FALSE (already initialised) and we MUST NOT call OleUninitialize
// on shutdown \u2014 doing so would unbalance REAPER's reference count.
enum class OleState { NotInit, InitByUs, InitByOther };
OleState g_oleState = OleState::NotInit;

// Lowercase extension of a file name, e.g. ".glb". Empty if no dot.
std::string LowerExt(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot);
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

bool IsKnownExtension(const std::string& path) {
  const std::string ext = LowerExt(path);
  return ext == ".mdd" || ext == ".glb" || ext == ".obj";
}

// Single IDropTarget implementation. One instance per registered hwnd; the
// instance is owned by a global map (hwnd -> instance) so RevokeDragDrop
// can find and destroy it.
class HoloRollDropTarget : public IDropTarget {
 public:
  explicit HoloRollDropTarget(DropCallback cb, AcceptanceQuery query)
      : callback_(std::move(cb)), acceptanceQuery_(std::move(query)) {}

  // ---- IUnknown ----
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *ppvObj = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&refCount_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refCount_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
  }

  // ---- IDropTarget ----
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD /*grfKeyState*/,
                                      POINTL /*pt*/, DWORD* pdwEffect) override {
    // Snapshot what's being dragged. Doing the expensive parsing once
    // here (rather than on every DragOver) keeps the message loop happy.
    const bool hasFileFmt = HasFileFormat(pDataObj);
    bool anyValid = false;
    if (hasFileFmt) {
      const std::vector<std::string> all = ExtractFilePaths(pDataObj);
      for (const std::string& p : all) {
        if (IsKnownExtension(p)) { anyValid = true; break; }
      }
    }
    const bool hostOk = !acceptanceQuery_ || acceptanceQuery_();

    canAccept_ = anyValid && hostOk;

    // Publish to global state for the renderer's overlay.
    g_isDragging.store(true);
    g_hasValidFiles.store(anyValid);
    g_hostAccepts.store(hostOk);

    *pdwEffect = canAccept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD /*grfKeyState*/, POINTL /*pt*/,
                                     DWORD* pdwEffect) override {
    *pdwEffect = canAccept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override {
    canAccept_ = false;
    ResetDragState();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD /*grfKeyState*/,
                                 POINTL /*pt*/, DWORD* pdwEffect) override {
    *pdwEffect = DROPEFFECT_NONE;
    canAccept_ = false;
    ResetDragState();

    std::vector<std::string> paths = ExtractFilePaths(pDataObj);
    if (paths.empty()) return S_OK;

    // Filter to known extensions. Anything else (audio, midi, video) is
    // intentionally ignored \u2014 those are REAPER's domain, not ours.
    std::vector<std::string> filtered;
    filtered.reserve(paths.size());
    for (const std::string& p : paths) {
      if (IsKnownExtension(p)) filtered.push_back(p);
    }

    if (callback_) callback_(filtered);
    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
  }

 private:
  static bool HasFileFormat(IDataObject* obj) {
    if (!obj) return false;
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return obj->QueryGetData(&fmt) == S_OK;
  }

  static std::vector<std::string> ExtractFilePaths(IDataObject* obj) {
    std::vector<std::string> out;
    if (!obj) return out;

    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (obj->GetData(&fmt, &medium) != S_OK) return out;

    HDROP hdrop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (hdrop) {
      const UINT count = DragQueryFileA(hdrop, 0xFFFFFFFF, nullptr, 0);
      out.reserve(count);
      char buf[MAX_PATH];
      for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileA(hdrop, i, buf, MAX_PATH);
        if (len > 0) out.emplace_back(buf, len);
      }
      GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return out;
  }

  LONG refCount_ = 1;
  bool canAccept_ = false;
  DropCallback callback_;
  AcceptanceQuery acceptanceQuery_;
};

// hwnd -> drop_target instance owned by us. We keep this so Unregister
// can safely call RevokeDragDrop and Release on the right pointer.
struct RegisteredTarget {
  HWND hwnd = nullptr;
  HoloRollDropTarget* target = nullptr;
};
std::vector<RegisteredTarget> g_registered;

}  // namespace

bool Initialize() {
  if (g_oleState != OleState::NotInit) return true;
  const HRESULT hr = OleInitialize(nullptr);
  if (hr == S_OK) {
    g_oleState = OleState::InitByUs;
    return true;
  }
  if (hr == S_FALSE) {
    // Already initialised \u2014 typical case in REAPER. Do not call
    // OleUninitialize in our Shutdown.
    g_oleState = OleState::InitByOther;
    return true;
  }
  // RPC_E_CHANGED_MODE or worse: OLE refuses our STA. Drag-n-drop won't
  // work but we don't crash anything.
  g_oleState = OleState::NotInit;
  return false;
}

void Shutdown() {
  // Revoke any still-registered hwnds first.
  for (auto& reg : g_registered) {
    if (reg.hwnd) RevokeDragDrop(reg.hwnd);
    if (reg.target) reg.target->Release();
  }
  g_registered.clear();

  if (g_oleState == OleState::InitByUs) {
    OleUninitialize();
  }
  g_oleState = OleState::NotInit;
}

bool RegisterOnHwnd(HWND hwnd, DropCallback onDrop, AcceptanceQuery acceptanceQuery) {
  if (!hwnd || g_oleState == OleState::NotInit) return false;

  // If hwnd is already registered (ours), drop it first.
  UnregisterFromHwnd(hwnd);

  HoloRollDropTarget* target = new HoloRollDropTarget(std::move(onDrop),
                                                     std::move(acceptanceQuery));
  const HRESULT hr = RegisterDragDrop(hwnd, target);
  if (FAILED(hr)) {
    target->Release();
    return false;
  }

  g_registered.push_back({hwnd, target});
  return true;
}

void UnregisterFromHwnd(HWND hwnd) {
  if (!hwnd) return;
  for (auto it = g_registered.begin(); it != g_registered.end(); ++it) {
    if (it->hwnd != hwnd) continue;
    RevokeDragDrop(hwnd);
    if (it->target) it->target->Release();
    g_registered.erase(it);
    return;
  }
}

DragState GetDragState() {
  DragState s;
  s.isDragging = g_isDragging.load();
  s.hasValidFiles = g_hasValidFiles.load();
  s.hostAccepts = g_hostAccepts.load();
  return s;
}

}  // namespace drop_target
