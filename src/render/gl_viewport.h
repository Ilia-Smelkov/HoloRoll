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
    bool topologyAvailable = true;

    // Auto-pivot of the current animation (bbox centre of frame 0). Used as
    // the rotation centre and the gizmo position. Zero-initialised when no
    // animation is active.
    float autoPivot[3] = {0.0f, 0.0f, 0.0f};
    float autoExtent = 1.0f;
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

  // Per-animation pose memory.
  void ApplyPose(const ViewportPose& p);
  void CapturePose(ViewportPose& out) const;

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  bool CreateContext();
  void DestroyContext();
  void UpdateInput(float dtSeconds);
  void EnsureGpuBuffers(const std::vector<float>& vertices,
                       const std::vector<std::uint32_t>& triangleIndices);
  void UploadFrameToVbo(const std::vector<float>& vertices);
  void ApplyCameraTransform();
  void ResetCameraToDefault(const OverlayStatus& status);
  void DrawScene(const std::vector<float>& vertices,
                 const std::vector<std::uint32_t>& triangleIndices,
                 double playPositionSeconds,
                 const OverlayStatus& status);
  void DrawGizmo(const float pivotWorld[3], float screenRadiusWorld);
  void GizmoHitTestAndDrag(const float pivotWorld[3], float screenRadiusWorld);
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

  // -------- Camera (Fly only) --------
  // Two values per axis: a *target* set by input, and a *current* that
  // exponentially chases the target. Smoothing strength is in UpdateInput().
  // Position in world space.
  float cameraPosX_ = 0.0f;
  float cameraPosY_ = 0.0f;
  float cameraPosZ_ = 1.5f;
  float cameraPosTargetX_ = 0.0f;
  float cameraPosTargetY_ = 0.0f;
  float cameraPosTargetZ_ = 1.5f;
  // Look direction (Euler angles, degrees).
  float cameraYaw_ = 0.0f;
  float cameraPitch_ = 0.0f;
  float cameraYawTarget_ = 0.0f;
  float cameraPitchTarget_ = 0.0f;
  // Movement speed (units / second).
  float flySpeed_ = 1.0f;

  // -------- Object orientation --------
  float objectYaw_ = 0.0f;
  float objectPitch_ = 0.0f;
  float objectRoll_ = 0.0f;

  // -------- Pivot offset (relative to autoPivot supplied each frame) --------
  float pivotOffset_[3] = {0.0f, 0.0f, 0.0f};

  // -------- Input state --------
  POINT lastMousePos_{};
  bool flyMouseLook_ = false;      // RMB held -> mouse-look + WASD active
  bool lmbPressed_ = false;        // LMB currently down (for gizmo drag)
  float wheelDeltaSteps_ = 0.0f;
  ULONGLONG lastTickMs_ = 0;

  // -------- Gizmo state --------
  bool showGizmo_ = true;
  // 0=X, 1=Y, 2=Z, -1=none
  int gizmoHoverAxis_ = -1;
  int gizmoDragAxis_ = -1;
  float gizmoDragStartAngle_ = 0.0f;
  float gizmoDragStartRotation_ = 0.0f;

  // Cached matrices from last Render so gizmo hit-test can project to screen.
  float matModelView_[16] = {0};
  float matProjection_[16] = {0};
  int viewportWidth_ = 1;
  int viewportHeight_ = 1;

  // -------- Frame timing --------
  double smoothedFrameMs_ = 0.0;
  ULONGLONG lastFrameTick_ = 0;

  // -------- GPU buffers (GL 1.5+) --------
  unsigned int vboId_ = 0;
  unsigned int eboId_ = 0;
  std::size_t vboCapacityBytes_ = 0;
  std::size_t eboCapacityBytes_ = 0;
  std::size_t lastIndexCount_ = 0;
  bool glBuffersAvailable_ = false;
};
