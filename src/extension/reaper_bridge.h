#pragma once

#include <atomic>
#include "extension/reaper_api.h"

class ReaperBridge {
 public:
  bool Initialize(reaper_plugin_info_t* rec);
  void Shutdown(reaper_plugin_info_t* rec);
  void OnTimerTick();

  double TimelineTimeSeconds() const { return timelineTimeSeconds_.load(); }
  const ReaperApi& Api() const { return api_; }

 private:
  ReaperApi api_;
  std::atomic<double> timelineTimeSeconds_{0.0};
};
