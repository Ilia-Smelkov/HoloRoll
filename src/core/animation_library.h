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
  std::string objPath;
  std::unique_ptr<MDDDataManager> mdd;
  std::unique_ptr<ObjIndexLoader> obj;

  // Bounding-box centre of the rest pose (frame 0). Used as the default
  // pivot for object rotation and camera orbit.
  float autoPivot[3] = {0.0f, 0.0f, 0.0f};

  // Max bbox extent of frame 0. Used to scale the rotation gizmo and the
  // default camera distance.
  float autoExtent = 1.0f;

  // Per-triangle face normals computed from the rest pose (frame 0).
  // Layout: 3 floats per triangle, same order as `obj->TriangleIndices()`
  // in groups of 3 indices. Empty if no OBJ is paired.
  //
  // We compute normals once on rest pose rather than per-frame because:
  //   - per-frame is expensive (~1k triangles x 24 fps = constant CPU work)
  //   - for typical character animation, face orientation drifts only
  //     mildly over time, so rest-pose normals look fine
  //   - if shading artefacts ever become a problem we can switch to per-frame
  std::vector<float> restNormals;

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
