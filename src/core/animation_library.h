#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/glb_loader.h"
#include "core/mdd_data_manager.h"
#include "core/obj_index_loader.h"

// Single loaded animation. Source can be either an MDD/OBJ pair OR a single
// GLB file (with skeletal animation baked into per-frame vertex positions).
// Exactly one of `mdd` or `glb` is non-null after a successful load.
struct LoadedAnimation {
  std::string basename;       // What appears in the region name.
  std::string sourcePath;     // Path of the loaded file (.mdd or .glb).
  std::string objPath;        // Only set when source is MDD with a paired OBJ.

  // MDD path retained as a separate field for backward compatibility with
  // any code (or future tools) that need it specifically.
  std::string mddPath;

  std::unique_ptr<MDDDataManager> mdd;
  std::unique_ptr<ObjIndexLoader> obj;
  std::unique_ptr<GlbLoader> glb;

  // Bounding-box centre of the rest pose (frame 0). Used as the default
  // pivot for object rotation and camera orbit.
  float autoPivot[3] = {0.0f, 0.0f, 0.0f};

  // Max bbox extent of frame 0. Used to scale the rotation gizmo and the
  // default camera distance.
  float autoExtent = 1.0f;

  // Per-triangle face normals computed from the rest pose (frame 0).
  std::vector<float> restNormals;

  double DurationSeconds(double fps) const;

  // Source-agnostic accessors. Always prefer these over poking at
  // `mdd` / `glb` directly.
  std::uint32_t TotalFrames() const;
  std::uint32_t TotalPoints() const;
  const std::vector<float>& VerticesForFrame(std::uint32_t frame) const;
  const std::vector<std::uint32_t>* TriangleIndicesPtr() const;
  bool HasTopology() const;
};

struct TimelineRegion {
  std::size_t animationIndex = 0;
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  std::string regionName;
};

class AnimationLibrary {
 public:
  std::size_t ScanFolder(const std::string& directory, double fps, std::string* logOut = nullptr);

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

  // Region naming.
  //
  // Historically all region names were prefixed with "MDD: " (legacy). As
  // of v0.4.0 the default prefix is empty; users who want it back can set
  // `region_name_prefix=` in the INI config.
  //
  // Prefix is stored as runtime state (static) so that AnimationLibrary
  // and entry.cpp share a single source of truth without piping it through
  // every function call. ScanFolder/BuildRegions read the current value;
  // entry.cpp sets it from config on startup and on Reload config.
  //
  // The legacy "MDD: " prefix is recognised on read regardless — see
  // StripLegacyAndCurrentPrefix() — so old REAPER projects with
  // `MDD: foo` regions still match basenames after this change.
  static const char* kLegacyRegionNamePrefix;  // "MDD: "
  static void SetRegionNamePrefix(const std::string& prefix);
  static const std::string& RegionNamePrefix();

  // Strip either the current configured prefix or the legacy "MDD: "
  // prefix from a region name, whichever matches first. Used by entry.cpp
  // to resolve REAPER region names back to animation basenames.
  static std::string StripPrefix(const std::string& regionName);

 private:
  std::string directory_;
  std::vector<std::unique_ptr<LoadedAnimation>> animations_;
  std::vector<TimelineRegion> regions_;

  static std::string s_regionNamePrefix_;
};
