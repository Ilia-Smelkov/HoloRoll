#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

// Watches a single directory for file additions, modifications, renames and
// deletions. Implementation uses Win32 ReadDirectoryChangesW on a worker
// thread; events are accumulated in an internal queue that the main thread
// drains via Drain().
//
// Filename filtering and animation-vs-other-file logic live in entry.cpp /
// AnimationLibrary; this class is intentionally dumb and reports every file
// event in the watched directory.
//
// Lifecycle: construct -> Start(dir) -> (poll Drain() periodically) -> Stop().
// Stop() blocks until the worker has unwound. Calling Start() again after
// Stop() with the same or different directory is supported.
class FolderWatcher {
 public:
  enum class EventKind {
    Added,       // File appeared.
    Removed,     // File deleted.
    Modified,    // File contents written.
    RenamedOld,  // Old name in a rename pair.
    RenamedNew,  // New name in a rename pair.
  };

  struct Event {
    EventKind kind;
    std::string filename;  // basename only (no directory), as reported by Win32.
  };

  FolderWatcher() = default;
  ~FolderWatcher();

  FolderWatcher(const FolderWatcher&) = delete;
  FolderWatcher& operator=(const FolderWatcher&) = delete;

  // Start watching `directory`. Returns false if the directory cannot be
  // opened. If already running, this stops the existing watch first.
  bool Start(const std::string& directory);

  // Stop the watcher and join the worker thread. Safe to call repeatedly.
  // Idempotent — calling on a stopped watcher is a no-op.
  void Stop();

  bool IsRunning() const { return running_.load(); }

  const std::string& Directory() const { return directory_; }

  // Move accumulated events out of the queue and return them. The caller
  // owns the returned vector; the internal queue is left empty.
  std::vector<Event> Drain();

 private:
  void WorkerLoop();

  std::string directory_;
  HANDLE dirHandle_ = INVALID_HANDLE_VALUE;
  HANDLE cancelEvent_ = nullptr;       // Manual-reset event used to wake worker for shutdown.
  std::thread worker_;
  std::atomic<bool> running_{false};

  std::mutex queueMutex_;
  std::vector<Event> queue_;
};
