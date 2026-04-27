#pragma once

#include <cstddef>
#include <unordered_map>

// Per-animation visual state. Camera + object + pivot + render mode are
// remembered separately for each animation so switching regions feels stable.
struct ViewportPose {
  // -------- Camera (Fly mode) --------
  float cameraPosX = 0.0f;
  float cameraPosY = 0.0f;
  float cameraPosZ = 1.5f;
  float cameraYaw = 0.0f;
  float cameraPitch = 0.0f;
  float flySpeed = 1.0f;

  // -------- Object orientation --------
  float objectYaw = 0.0f;
  float objectPitch = 0.0f;
  float objectRoll = 0.0f;

  // -------- Pivot offset (relative to bbox-centre of frame 0) --------
  float pivotOffsetX = 0.0f;
  float pivotOffsetY = 0.0f;
  float pivotOffsetZ = 0.0f;

  // -------- Render --------
  int renderMode = 2;  // Solid by default

  bool initialized = false;
};

class ViewportPoses {
 public:
  ViewportPose& Get(std::size_t animationIndex) { return poses_[animationIndex]; }
  void Set(std::size_t animationIndex, const ViewportPose& pose) { poses_[animationIndex] = pose; }
  void Clear() { poses_.clear(); }

 private:
  std::unordered_map<std::size_t, ViewportPose> poses_;
};
