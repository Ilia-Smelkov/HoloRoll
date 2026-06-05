#pragma once

#include <array>
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

  // v0.16.0-alpha.1 camera attach.
  //
  // Two modes: Free (existing Fly camera, WASD + RMB mouse look) and
  // Attached (camera position derived from a chosen bone's world
  // transform per frame). Attached splits further into three rotation
  // sub-modes — see RotMode.
  //
  // boneName is matched against LoadedAnimation::jointNames at render
  // time; if the current animation doesn't have a bone by that name we
  // fall back to Free. Empty boneName == no attach configured.
  //
  // offset is the camera's position relative to the bone. If
  // offsetLocal is true, the offset is in the bone's local space
  // (rotates with the bone); otherwise it's in world space.
  //
  // damping smooths position changes. 0 = instant follow, 1 = heavy
  // smoothing (≈0.5s settle time). Default 0.15 ≈ comfortable
  // follow-cam feel.
  struct CameraConfig {
    enum class Mode { Free, Attached };
    enum class RotMode { Full, YawOnly, FreeOrbit };

    Mode mode = Mode::Free;
    std::string boneName;
    float offsetX = 0.0f;
    float offsetY = 0.5f;
    float offsetZ = -2.0f;
    bool offsetLocal = true;
    RotMode rotMode = RotMode::YawOnly;
    float damping = 0.15f;
  };

  struct OverlayStatus {
    std::string animationsDir;
    std::string currentAnimation;
    std::size_t loadedAnimationCount = 0;
    std::size_t regionCount = 0;
    double activeRegionStart = 0.0;
    double activeRegionEnd = 0.0;
    bool topologyAvailable = true;

    float autoPivot[3] = {0.0f, 0.0f, 0.0f};
    float autoExtent = 1.0f;

    // Per-triangle face normals computed by AnimationLibrary on rest pose.
    const std::vector<float>* restNormals = nullptr;

    // v0.12.0-alpha.10: the "new animations detected" modal was removed
    // in favour of automatic placement. This field still exists for
    // API compatibility but should normally be empty by the time the
    // UI renders (PlacePendingAtCursor consumes the queue synchronously
    // in ProcessWatcherEvents).
    std::vector<std::string> pendingNewAnimations;

    // Non-empty iff an item is under the playhead but its name does not
    // resolve to any animation in the library (either directly or via
    // variation-suffix stripping). Overlay shows a warning when set.
    std::string missingAnimationName;

    // Animation name to show under the "+ Place" buttons in the library
    // section. Used when the user hits Place to create another item; the
    // name comes back to entry.cpp through the request's animationToPlace
    // field and is then created on the selected track at cursor position.
    std::string requestPlaceAnimation;

    // v0.7.0: true when the active animations folder is a project-level
    // override (set via Choose folder...). Drives the visibility of the
    // "Reset to default folder" button in the overlay.
    bool folderIsOverride = false;

    // v0.7.0: true when no project is saved (Untitled). Overlay shows a
    // hint instead of the regular library info.
    bool projectUntitled = false;

    // v0.16.0-alpha.1 camera attach: pointers to the active animation's
    // joint world-matrix table + name list. Both null for MDD-style
    // animations without a skeleton, or when no animation is active.
    // ApplyCameraTransform looks up CameraConfig.boneName here at the
    // current `frameIndex` (passed separately to Render) to compute
    // the bone-anchored camera basis.
    using BoneMatrix = std::array<float, 16>;
    const std::vector<std::vector<BoneMatrix>>* jointWorldMatrices = nullptr;
    const std::vector<std::string>* jointNames = nullptr;
  };

  struct OverlayRequests {
    bool chooseFolder = false;
    bool reloadFolder = false;
    bool placeRegions = false;
    bool openConfig = false;
    bool reloadConfig = false;
    // v0.6.0 spike: try creating an empty media item with a name on the
    // first selected track (or first track if nothing selected). Validates
    // that the REAPER API path we plan to use for the items workflow
    // actually works in this build.
    bool spikeTestCreateItem = false;

  // v0.7.0: clear project's animations-folder override (return to
  // <project>/Animations/). Only meaningful when the override is set.
  bool resetFolderOverride = false;

    // v0.12.0-alpha.10: previously: choice from "new animations detected"
    // modal. Modal is gone (auto-placement). Field kept as dead for now
    // to avoid breaking external code that might read it; safe to drop
    // in a follow-up cleanup along with DrawNewAnimationsModal().
    int newAnimationsChoice = 0;

    // Non-empty if the user pressed "+ Place" next to a library entry.
    // entry.cpp uses this to call PlaceSingleAtCursor(name).
    std::string placeSingleAtCursor;

    // v0.12.0-alpha.9: user pressed "Generate motion markers". entry.cpp
    // walks every placed HoloRoll item, runs the active motion-event
    // detector on the item's top-1 active bone, and writes REAPER
    // project markers at the detected event times.
    bool generateMotionMarkers = false;
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

  void ApplyPose(const ViewportPose& p);
  void CapturePose(ViewportPose& out) const;

  // Snap the camera to a 3/4-perspective default view that frames the
  // current model. Used by the "Reset camera" button in the overlay UI
  // and by entry.cpp the first time an animation becomes active.
  void ResetCameraToDefault(const OverlayStatus& status);

  // ---- v0.11.0 placement options (transient state owned by GlViewport,
  //      persisted via Set/GetPlacementOptions — mirrors Scene settings) ---
  // alpha.10: signature reduced to pre/post-roll only. Variations and
  // region overhang were dropped along with their UI fields.
  void SetPlacementOptions(float preRollSec, float postRollSec);
  void GetPlacementOptions(float* preRollSec, float* postRollSec) const;
  bool ConsumePlacementDirty();

  // ---- Scene settings (persisted in holoroll_config.ini) -----------------
  void SetSceneSettings(bool showGround, float radius, float gridStep,
                        bool showBboxDims, bool showGridLabels, bool showRefHuman);
  void GetSceneSettings(bool* showGround, float* radius, float* gridStep,
                        bool* showBboxDims, bool* showGridLabels, bool* showRefHuman) const;
  bool ConsumeSceneDirty();

  // ---- v0.12.0-alpha.13 debug flag --------------------------------------
  // Mirror of the global g_debugEnabled atomic in entry.cpp. The
  // overlay's Config-section checkbox edits this; OnTimer rolls dirty
  // changes back into the global atomic and persists to config.
  void SetDebugEnabled(bool enabled);
  bool GetDebugEnabled() const;
  bool ConsumeDebugDirty();

  // ---- v0.16.0-alpha.1 camera attach state ------------------------------
  //
  // Owned by GlViewport, edited by entry.cpp's overlay UI via
  // SetCameraConfig / GetCameraConfig. ConsumeCameraDirty signals
  // entry.cpp to persist to config (per-animation, sanitised name).
  // The active animation name is needed for proper per-anim
  // persistence — entry.cpp tracks it separately based on what's
  // resolved under the playhead.
  void SetCameraConfig(const CameraConfig& cfg);
  const CameraConfig& GetCameraConfig() const;
  bool ConsumeCameraDirty();

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  bool CreateContext();
  void DestroyContext();
  void UpdateInput(float dtSeconds);
  void EnsureGpuBuffers(const std::vector<float>& vertices,
                       const std::vector<std::uint32_t>& triangleIndices);
  void UploadFrameToVbo(const std::vector<float>& vertices);
  void ApplyCameraTransform();
  void DrawBackgroundGradient();
  void DrawGroundPlane();
  void DrawScene(const std::vector<float>& vertices,
                 const std::vector<std::uint32_t>& triangleIndices,
                 double playPositionSeconds,
                 const OverlayStatus& status);
  void DrawGizmo(const float pivotWorld[3], float screenRadiusWorld);
  void GizmoHitTestAndDrag(const float pivotWorld[3], float screenRadiusWorld);
  void DrawOverlay(double playPositionSeconds,
                   std::uint32_t frameIndex,
                   std::uint32_t totalFrames,
                   const std::vector<float>& vertices,
                   const OverlayStatus& status);
  // Renders the modal dialog inside the current ImGui frame.
  // Returns: 0 if no choice was made this frame, 1 = Place all, 2 = Skip.
  int DrawNewAnimationsModal(const std::vector<std::string>& pending);

  // v0.16.0-alpha.1: compute attachedTargetPos_/Yaw_/Pitch_ from the
  // configured bone + offset + current frame. attachedActive_ ends up
  // false if no bone resolved (Free mode, missing bone, MDD anim).
  void ResolveAttachedTarget(const OverlayStatus& status,
                              std::uint32_t frameIndex);

  HWND hwnd_ = nullptr;
  HDC hdc_ = nullptr;
  HGLRC hglrc_ = nullptr;
  OverlayRequests pendingRequests_{};
  bool imguiInitialized_ = false;

  RenderMode renderMode_ = RenderMode::Solid;
  float pointSize_ = 2.0f;
  float amplitudeScale_ = 1.0f;

  // -------- Camera (Fly only) --------
  float cameraPosX_ = 0.0f;
  float cameraPosY_ = 0.0f;
  float cameraPosZ_ = 1.5f;
  float cameraPosTargetX_ = 0.0f;
  float cameraPosTargetY_ = 0.0f;
  float cameraPosTargetZ_ = 1.5f;
  float cameraYaw_ = 0.0f;
  float cameraPitch_ = 0.0f;
  float cameraYawTarget_ = 0.0f;
  float cameraPitchTarget_ = 0.0f;
  float flySpeed_ = 1.0f;

  // -------- Object orientation --------
  float objectYaw_ = 0.0f;
  float objectPitch_ = 0.0f;
  float objectRoll_ = 0.0f;

  // -------- Pivot offset --------
  float pivotOffset_[3] = {0.0f, 0.0f, 0.0f};

  // -------- Scene (background + ground plane) --------
  bool showGroundPlane_ = true;
  float groundSize_ = 20.0f;
  float groundGridStep_ = 1.0f;
  bool sceneDirty_ = false;

  // v0.10.0 scale awareness: three opt-in scale aids.
  //   showBboxDimensions_  - text plate "X x Y x Z m" in viewport corner
  //   showGridLabels_      - small text labels on major grid intersections
  //   showReferenceHuman_  - 1.80m stick figure to the right of the model
  // All persisted via SetSceneSettings/GetSceneSettings to holoroll_config.
  bool showBboxDimensions_ = true;
  bool showGridLabels_ = true;
  bool showReferenceHuman_ = true;

  // v0.11.0 placement options. Edited via the "Place all" overlay row;
  // entry.cpp persists them to holoroll_config.ini.
  //   placementPreRollSec_   - hold-frame seconds before the animation
  //   placementPostRollSec_  - hold-frame seconds after the animation
  // alpha.10: placementVariations_ / placementRegionOverhang_ removed
  // along with their UI fields and the region-creation step.
  float placementPreRollSec_ = 1.0f;
  float placementPostRollSec_ = 1.0f;
  bool  placementDirty_ = false;

  // v0.12.0-alpha.13: runtime debug-log toggle (mirrors entry.cpp's
  // g_debugEnabled atomic). debugDirty_ fires when the user clicks the
  // overlay checkbox; OnTimer in entry.cpp consumes it.
  bool debugEnabled_ = false;
  bool debugDirty_ = false;

  // v0.16.0-alpha.1 camera attach state. cameraConfig_ is the
  // authoritative config; cameraDirty_ fires on user edit (UI panel or
  // viewport drag) so entry.cpp can persist. attachedActive_ /
  // attachedTargetPos_ are computed each Render() from the current
  // bone matrix + offset; ApplyCameraTransform reads them to override
  // the Free camera path. Smoothed position follows targets via the
  // existing camera-smoothing exp filter, with tau driven by damping.
  CameraConfig cameraConfig_;
  bool cameraDirty_ = false;
  bool attachedActive_ = false;        // bone resolved this frame.
  float attachedTargetPos_[3] = {0, 0, 0};
  float attachedTargetYaw_ = 0.0f;     // degrees, used in Match/Yaw modes.
  float attachedTargetPitch_ = 0.0f;   // degrees, used in Match mode only.

  // -------- Input state --------
  POINT lastMousePos_{};
  bool flyMouseLook_ = false;
  bool lmbPressed_ = false;
  float wheelDeltaSteps_ = 0.0f;
  ULONGLONG lastTickMs_ = 0;

  // -------- Gizmo state --------
  bool showGizmo_ = true;
  int gizmoHoverAxis_ = -1;
  int gizmoDragAxis_ = -1;
  float gizmoDragStartAngle_ = 0.0f;
  float gizmoDragStartRotation_ = 0.0f;

  float matModelView_[16] = {0};
  float matProjection_[16] = {0};
  int viewportWidth_ = 1;
  int viewportHeight_ = 1;

  double smoothedFrameMs_ = 0.0;
  ULONGLONG lastFrameTick_ = 0;

  unsigned int vboId_ = 0;
  unsigned int eboId_ = 0;
  std::size_t vboCapacityBytes_ = 0;
  std::size_t eboCapacityBytes_ = 0;
  std::size_t lastIndexCount_ = 0;
  bool glBuffersAvailable_ = false;
};
