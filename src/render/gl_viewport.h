#pragma once

#include <cstdint>
#include <string>
#include <windows.h>
#include <vector>

#include "core/viewport_poses.h"

class GlViewport {
 public:
  enum class RenderMode {
    Points = 0,
    Wireframe = 1,
    Solid = 2,
  };

  struct OverlayStatus {
    std::string animationsDir;
    std::string currentAnimation;
    std::size_t loadedAnimationCount = 0;
    std::size_t regionCount = 0;
    double activeRegionStart = 0.0;
    double activeRegionEnd = 0.0;
    bool topologyAvailable = true;  // false -> only Points actually renders
  };

  struct OverlayRequests {
    bool chooseFolder = false;
    bool reloadFolder = false;
    bool placeRegions = false;
    bool openConfig = false;
    bool reloadConfig = false;
  };

  bool Open();
  void Close();
  bool IsOpen() const { return hwnd_ != nullptr; }
  HWND Hwnd() const { return hwnd_; }

  void Tick();

  void Render(const std::vector<float>& vertices,
              const std::vector<std::uint32_t>& triangleIndices,
              double playPositionSeconds,
              std::uint32_t frameIndex,
              std::uint32_t totalFrames,
              const OverlayStatus& status);

  OverlayRequests ConsumeRequests();

  // ---- Pose IO (per-animation memory) ------------------------------------
  // Apply a stored pose to the live viewport state.
  void ApplyPose(const ViewportPose& p);
  // Capture the current live viewport state into `out`.
  void CapturePose(ViewportPose& out) const;

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  bool CreateContext();
  void DestroyContext();
  void UpdateCameraFromInput();
  void EnsureGpuBuffers(const std::vector<float>& vertices,
                       const std::vector<std::uint32_t>& triangleIndices);
  void UploadFrameToVbo(const std::vector<float>& vertices);
  void DrawScene(const std::vector<float>& vertices,
                 const std::vector<std::uint32_t>& triangleIndices,
                 double playPositionSeconds,
                 const OverlayStatus& status);
  void DrawOverlay(double playPositionSeconds,
                   std::uint32_t frameIndex,
                   std::uint32_t totalFrames,
                   std::size_t vertexCount,
                   const OverlayStatus& status);

  HWND hwnd_ = nullptr;
  HDC hdc_ = nullptr;
  HGLRC hglrc_ = nullptr;
  OverlayRequests pendingRequests_{};
  bool imguiInitialized_ = false;

  RenderMode renderMode_ = RenderMode::Solid;
  float pointSize_ = 2.0f;
  float amplitudeScale_ = 1.0f;

  // Camera (orbit around origin).
  float cameraYaw_ = 35.0f;
  float cameraPitch_ = -20.0f;
  float cameraDistance_ = 0.8f;

  // Object's own rotation (applied AFTER camera, to the model itself).
  float objectYaw_ = 0.0f;
  float objectPitch_ = 0.0f;

  POINT lastMousePos_{};
  bool rotatingCamera_ = false;   // LMB drag
  bool rotatingObject_ = false;   // RMB drag
  float wheelDeltaSteps_ = 0.0f;
  double smoothedFrameMs_ = 0.0;
  ULONGLONG lastFrameTick_ = 0;

  unsigned int vboId_ = 0;
  unsigned int eboId_ = 0;
  std::size_t vboCapacityBytes_ = 0;
  std::size_t eboCapacityBytes_ = 0;
  std::size_t lastIndexCount_ = 0;
  bool glBuffersAvailable_ = false;
};
