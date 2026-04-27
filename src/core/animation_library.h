#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/mdd_data_manager.h"
#include "core/obj_index_loader.h"

// Single loaded animation: one MDD (per-frame point cache) paired with one OBJ
// (topology used by Wireframe / Solid render modes).
struct LoadedAnimation {
  std::string basename;
  std::string mddPath;
  std::string objPath;         // empty if OBJ was missing
  std::unique_ptr<MDDDataManager> mdd;
  std::unique_ptr<ObjIndexLoader> obj;

  // Bounding-box centre of the rest pose (frame 0). Used as the default
  // pivot for object rotation and camera orbit. The user can additionally
  // shift the pivot via `ViewportPose::pivotOffset*`.
  float autoPivot[3] = {0.0f, 0.0f, 0.0f};

  // Approximate object size (max bbox extent of frame 0). Used to scale the
  // rotation gizmo so it stays visible regardless of the model's scale.
  float autoExtent = 1.0f;

  double DurationSeconds(double fps) const;
};

struct TimelineRegion {
  std::size_t animationIndex = 0;
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  std::string regionName;
};

class AnimationLibrary {
 public:
  std::size_t ScanFolder(const std::string& directory, std::string* logOut = nullptr);

  void Clear();

  std::size_t Count() const { return animations_.size(); }
  const LoadedAnimation& At(std::size_t i) const { return *animations_[i]; }

  std::size_t FindAnimationIndexByBasename(const std::string& basename) const;

  const std::string& Directory() const { return directory_; }

  void BuildRegions(double fps, double gapSeconds, double startSeconds = 0.0);

  const std::vector<TimelineRegion>& Regions() const { return regions_; }
  double TotalSpanSeconds() const;

  bool ResolvePlayhead(double playheadSeconds,
                       double fps,
                       const std::vector<TimelineRegion>& liveRegions,
                       std::size_t* outAnimationIndex,
                       std::uint32_t* outFrameIndex) const;

  static constexpr int kRegionColorRgb = 0x7F4FBF;
  static int RegionColorReaper() {
    return 0x01000000 | kRegionColorRgb;
  }

  static constexpr const char* kRegionNamePrefix = "MDD: ";

 private:
  std::string directory_;
  std::vector<std::unique_ptr<LoadedAnimation>> animations_;
  std::vector<TimelineRegion> regions_;
};
