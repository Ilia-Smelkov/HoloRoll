#pragma once

#include <cstddef>
#include <unordered_map>

// Per-animation visual state. Camera orbit + object orientation + render mode
// are remembered separately for each animation so switching regions feels
// stable: the user can pose each animation independently.
struct ViewportPose {
  // Camera orbit (around object).
  float cameraYaw = 35.0f;
  float cameraPitch = -20.0f;
  float cameraDistance = 0.8f;

  // Object's own rotation (mesh-local).
  float objectYaw = 0.0f;
  float objectPitch = 0.0f;

  // Render mode index (matches GlViewport::RenderMode enum order).
  int renderMode = 2;  // Solid by default

  bool initialized = false;
};

class ViewportPoses {
 public:
  // Returns a reference to the stored pose for `animationIndex`.
  // Creates a default-constructed pose on first access.
  ViewportPose& Get(std::size_t animationIndex) {
    return poses_[animationIndex];
  }

  // Save (overwrite) a pose for an animation index.
  void Set(std::size_t animationIndex, const ViewportPose& pose) {
    poses_[animationIndex] = pose;
  }

  void Clear() { poses_.clear(); }

 private:
  std::unordered_map<std::size_t, ViewportPose> poses_;
};
