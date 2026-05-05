#include "core/folder_watcher.h"

#include <algorithm>
#include <cstring>

namespace {
// Buffer big enough for ~50 typical filename events without overflow.
// FILE_NOTIFY_INFORMATION is variable-length (2 bytes per UTF-16 char in the
// filename, plus 12 bytes header). 32KB covers a burst of ~500 short names,
// which is more than we'll realistically see in this plugin's workload.
constexpr DWORD kBufferSizeBytes = 32 * 1024;

constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME |
    FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_SIZE;

std::string Utf16ToUtf8(const wchar_t* wide, size_t wideLen) {
  if (wideLen == 0) return {};
  const int needed = WideCharToMultiByte(
      CP_UTF8, 0, wide, static_cast<int>(wideLen), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(wideLen), out.data(), needed, nullptr, nullptr);
  return out;
}

FolderWatcher::EventKind ActionToKind(DWORD action) {
  switch (action) {
    case FILE_ACTION_ADDED:            return FolderWatcher::EventKind::Added;
    case FILE_ACTION_REMOVED:          return FolderWatcher::EventKind::Removed;
    case FILE_ACTION_MODIFIED:         return FolderWatcher::EventKind::Modified;
    case FILE_ACTION_RENAMED_OLD_NAME: return FolderWatcher::EventKind::RenamedOld;
    case FILE_ACTION_RENAMED_NEW_NAME: return FolderWatcher::EventKind::RenamedNew;
    default:                           return FolderWatcher::EventKind::Modified;
  }
}
}  // namespace

FolderWatcher::~FolderWatcher() {
  Stop();
}

bool FolderWatcher::Start(const std::string& directory) {
  if (running_.load()) Stop();

  directory_ = directory;

  dirHandle_ = CreateFileA(
      directory.c_str(),
      FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (dirHandle_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  cancelEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!cancelEvent_) {
    CloseHandle(dirHandle_);
    dirHandle_ = INVALID_HANDLE_VALUE;
    return false;
  }

  running_.store(true);
  worker_ = std::thread(&FolderWatcher::WorkerLoop, this);
  return true;
}

void FolderWatcher::Stop() {
  if (!running_.load()) {
    // Even if not running, clean up any leftover handles.
    if (cancelEvent_) { CloseHandle(cancelEvent_); cancelEvent_ = nullptr; }
    if (dirHandle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(dirHandle_); dirHandle_ = INVALID_HANDLE_VALUE;
    }
    return;
  }

  running_.store(false);
  if (cancelEvent_) SetEvent(cancelEvent_);

  // CancelIoEx unblocks any pending ReadDirectoryChangesW. Without this the
  // worker would sit forever inside its WaitForMultipleObjects (we do wake
  // via cancelEvent_, but CancelIoEx also makes GetOverlappedResult return
  // immediately so the worker can exit cleanly).
  if (dirHandle_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(dirHandle_, nullptr);
  }

  if (worker_.joinable()) worker_.join();

  if (cancelEvent_) { CloseHandle(cancelEvent_); cancelEvent_ = nullptr; }
  if (dirHandle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(dirHandle_); dirHandle_ = INVALID_HANDLE_VALUE;
  }
}

std::vector<FolderWatcher::Event> FolderWatcher::Drain() {
  std::lock_guard<std::mutex> lock(queueMutex_);
  std::vector<Event> out;
  out.swap(queue_);
  return out;
}

void FolderWatcher::WorkerLoop() {
  // Buffer must be DWORD-aligned (FILE_NOTIFY_INFORMATION requirement).
  alignas(DWORD) unsigned char buffer[kBufferSizeBytes];

  while (running_.load()) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) break;

    DWORD bytesReturnedSync = 0;
    const BOOL issued = ReadDirectoryChangesW(
        dirHandle_,
        buffer,
        kBufferSizeBytes,
        FALSE,                  // bWatchSubtree: only the top-level folder
        kNotifyFilter,
        &bytesReturnedSync,     // unused with overlapped, but harmless
        &overlapped,
        nullptr);

    if (!issued) {
      CloseHandle(overlapped.hEvent);
      break;
    }

    HANDLE waits[2] = {overlapped.hEvent, cancelEvent_};
    const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

    if (waitResult == WAIT_OBJECT_0 + 1) {
      // Cancellation requested.
      CancelIoEx(dirHandle_, &overlapped);
      DWORD discarded = 0;
      GetOverlappedResult(dirHandle_, &overlapped, &discarded, TRUE);
      CloseHandle(overlapped.hEvent);
      break;
    }

    if (waitResult != WAIT_OBJECT_0) {
      // Unexpected wait failure; back out.
      CloseHandle(overlapped.hEvent);
      break;
    }

    DWORD bytesTransferred = 0;
    if (!GetOverlappedResult(dirHandle_, &overlapped, &bytesTransferred, FALSE)) {
      CloseHandle(overlapped.hEvent);
      break;
    }
    CloseHandle(overlapped.hEvent);

    // Walk the FILE_NOTIFY_INFORMATION linked list.
    std::vector<Event> batch;
    DWORD offset = 0;
    while (offset < bytesTransferred) {
      const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
      const size_t nameLen = info->FileNameLength / sizeof(wchar_t);
      Event e;
      e.kind = ActionToKind(info->Action);
      e.filename = Utf16ToUtf8(info->FileName, nameLen);
      batch.push_back(std::move(e));

      if (info->NextEntryOffset == 0) break;
      offset += info->NextEntryOffset;
    }

    if (!batch.empty()) {
      std::lock_guard<std::mutex> lock(queueMutex_);
      queue_.insert(queue_.end(),
                    std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
    }
    // Loop back: re-issue ReadDirectoryChangesW for the next batch.
  }

  running_.store(false);
}
