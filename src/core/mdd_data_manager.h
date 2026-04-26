#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Loads a Blender Point Cache (.mdd) file and provides per-frame vertex data.
//
// MDD binary layout (Big-Endian):
//   Header:
//     int32 totalFrames
//     int32 totalPoints
//   Body (totalFrames times):
//     float32 timeForFrame
//     float32 xyz[totalPoints * 3]
//
// Storage: one std::vector<float> per frame (size = totalPoints * 3, interleaved x,y,z).
class MDDDataManager {
 public:
  // Load an .mdd file from disk. Returns true on success.
  // On failure, manager is left empty and LastError() returns a description.
  bool LoadFromFile(const std::string& filePath);

  // Reset to empty state.
  void Clear();

  bool IsLoaded() const { return totalFrames_ > 0 && totalPoints_ > 0; }

  std::uint32_t TotalFrames() const { return totalFrames_; }
  std::uint32_t TotalPoints() const { return totalPoints_; }

  // Native cache FPS. MDD does not store FPS — caller (TimeToFrameMapper)
  // uses this constant to convert REAPER play position to frame index.
  static constexpr double kNativeFps = 24.0;

  // Returns interleaved x,y,z data for the requested frame.
  // If frame is out of range and the cache is non-empty, the last frame is returned (clamp).
  // If the cache is empty, returns an empty static vector.
  const std::vector<float>& VerticesForFrame(std::uint32_t frameIndex) const;

  // Time stamp stored in the file for the given frame (seconds). Useful for diagnostics.
  float TimeForFrame(std::uint32_t frameIndex) const;

  // Animation length in seconds derived from kNativeFps.
  double AnimationLengthSeconds() const {
    return totalFrames_ > 0 ? static_cast<double>(totalFrames_) / kNativeFps : 0.0;
  }

  const std::string& LastError() const { return lastError_; }
  const std::string& LoadedPath() const { return loadedPath_; }

 private:
  std::uint32_t totalFrames_ = 0;
  std::uint32_t totalPoints_ = 0;
  std::vector<std::vector<float>> frames_;  // [frameIndex][component]
  std::vector<float> frameTimes_;           // [frameIndex]
  std::string lastError_;
  std::string loadedPath_;
};
