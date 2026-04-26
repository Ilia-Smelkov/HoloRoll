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
  std::string basename;        // file stem, e.g. "toad_idle01"
  std::string mddPath;
  std::string objPath;         // empty if OBJ was missing
  std::unique_ptr<MDDDataManager> mdd;
  std::unique_ptr<ObjIndexLoader> obj;  // never null after construction; may be empty if file missing

  double DurationSeconds(double fps) const;
};

// A timeline region for a particular animation.
// Used both for the initial region layout and for live region info we read
// back from REAPER each tick.
struct TimelineRegion {
  std::size_t animationIndex = 0;
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  std::string regionName;
};

class AnimationLibrary {
 public:
  // Scan a directory for .mdd files and pair each with a topology OBJ.
  // Pairing strategy:
  //   1) <basename>.obj next to the .mdd
  //   2) Otherwise, any .obj in the same folder whose vertex count matches
  //      the .mdd's totalPoints. This lets a single mesh (e.g. "toad.obj")
  //      serve multiple animations like "toad_idle01.mdd", "toad_walk.mdd".
  std::size_t ScanFolder(const std::string& directory, std::string* logOut = nullptr);

  void Clear();

  std::size_t Count() const { return animations_.size(); }
  const LoadedAnimation& At(std::size_t i) const { return *animations_[i]; }

  // Find by file stem (e.g. "toad_idle01"). Returns SIZE_MAX if not found.
  std::size_t FindAnimationIndexByBasename(const std::string& basename) const;

  const std::string& Directory() const { return directory_; }

  // Build the region layout for placing markers on the REAPER timeline.
  // First region starts at `startSeconds`, adjacent regions are separated by
  // `gapSeconds`.
  void BuildRegions(double fps, double gapSeconds, double startSeconds = 0.0);

  const std::vector<TimelineRegion>& Regions() const { return regions_; }
  double TotalSpanSeconds() const;

  // Resolve playhead position using a CALLER-SUPPLIED region list (typically
  // re-read from REAPER each tick to reflect manual region edits).
  //
  // Rules (matches the "lock to region start, ignore length" requirement):
  //   - Inside a region: frame = floor((playhead - region.start) * fps), clamped to [0, frames-1].
  //     This means stretching a region simply shows the last frame for the
  //     remainder; shortening it cuts the animation off.
  //   - Before any region: animation 0, frame 0.
  //   - After / between regions: last frame of the most recent region's animation.
  //
  // `liveRegions` may be empty — in that case falls back to internal regions_.
  bool ResolvePlayhead(double playheadSeconds,
                       double fps,
                       const std::vector<TimelineRegion>& liveRegions,
                       std::size_t* outAnimationIndex,
                       std::uint32_t* outFrameIndex) const;

  // --- Region identity ------------------------------------------------------
  //
  // We tag every region we create with this color so we can identify "ours"
  // robustly even if the user renames them. REAPER expects the color in
  // 0x01RRGGBB form (the high bit distinguishes user-set color from default).
  //
  // Chosen tone: muted purple, distinct from REAPER defaults.
  static constexpr int kRegionColorRgb = 0x7F4FBF;  // RRGGBB
  static int RegionColorReaper() {
    // 0x01000000 flag = "color is set" in REAPER's color format.
    return 0x01000000 | kRegionColorRgb;
  }

  static constexpr const char* kRegionNamePrefix = "MDD: ";

 private:
  std::string directory_;
  std::vector<std::unique_ptr<LoadedAnimation>> animations_;
  std::vector<TimelineRegion> regions_;
};
