#pragma once

#include <array>
#include <cstdint>
#include <functional>
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

  // v0.16.0-alpha.2 camera attach (simplified from alpha.1).
  //
  // Two modes:
  //   Free      - existing Fly camera (WASD + RMB mouse look).
  //   Attached  - same Fly controls, but camera POSITION is locked to
  //               a chosen bone's world transform + offset (smoothed
  //               via damping). User still rotates view freely with
  //               RMB. WASD modifies the offset in camera-local axes
  //               (so flying around tunes the camera placement). When
  //               the bone moves the camera rides along.
  //
  // The three "rotation submodes" from alpha.1 (Match bone / Yaw only
  // / Free orbit) were removed in alpha.2 — they made offset tuning
  // confusing and the FreeOrbit case is the only one that's actually
  // useful for animation review.
  //
  // boneName matched against LoadedAnimation::jointNames at render
  // time; missing bone → fallback to Free behaviour for that frame.
  //
  // offsetLocal=true means offset rotates with the bone (offset in
  // bone-local space). false means offset is fixed in model space.
  //
  // damping smooths position changes. 0 = instant, 1 ≈ 0.5s settle.
  struct CameraConfig {
    enum class Mode { Free, Attached };

    Mode mode = Mode::Free;
    std::string boneName;
    float offsetX = 0.0f;
    float offsetY = 0.5f;
    float offsetZ = -2.0f;
    bool offsetLocal = true;
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

    // v0.16.0-alpha.2: parent joint indices for skeleton visualisation
    // (DrawSkeleton renders joint dots + bone-to-parent lines, +
    // spring-arm dashes from camera to attach point when Attached).
    // -1 in a slot means "no parent" (root). Null pointer = no
    // skeleton info available (MDD or no anim).
    const std::vector<int>* jointParents = nullptr;
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

    // v0.16.0-alpha.7: user pressed the in-overlay Close button. When
    // the viewport is docked, the default window-frame close path is
    // hidden under REAPER's dock chrome (no titlebar X), so we surface
    // an explicit Close button in the Config section.
    bool closeViewport = false;
  };

  bool Open();
  void Close();
  bool IsOpen() const { return hwnd_ != nullptr; }
  HWND Hwnd() const { return hwnd_; }

  // v0.16.0-alpha.9: invoked from WndProc's WM_DESTROY BEFORE hwnd_ is
  // cleared. Receives the live HWND so REAPER's DockWindowRemove can
  // run synchronously while it's still valid (the docker chrome's tab
  // X click flows here — see SWS' sws_wnd.cpp). entry.cpp registers a
  // callback that calls DockWindowRemove + drop-target unregister +
  // RefreshToolbar. Must NOT call back into GlViewport.
  using OnDestroyCallback = std::function<void(HWND)>;
  void SetOnDestroyCallback(OnDestroyCallback cb) { onDestroy_ = std::move(cb); }

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

  // ---- v0.16.0-alpha.7 open-on-startup toggle ---------------------------
  // Mirror of the global config key `viewport.open_on_startup`. The
  // checkbox is in the overlay's Config section; entry.cpp persists
  // dirty changes to holoroll_config.ini.
  void SetOpenOnStartup(bool enabled);
  bool GetOpenOnStartup() const;
  bool ConsumeOpenOnStartupDirty();

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

  // ---- v0.16.0-alpha.2 skeleton visualisation ---------------------------
  // Global toggle (persisted to viewport.show_skeleton in config). When
  // on, DrawScene renders joint dots + bone-to-parent lines on top of
  // the mesh, plus a spring-arm dashed line from the camera to the
  // attach point when Attached mode is active. Debug aid for offset
  // tuning + bone selection.
  void SetSkeletonVisible(bool visible);
  bool GetSkeletonVisible() const;
  bool ConsumeSkeletonDirty();

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

  // v0.16.0-alpha.1: compute attachedTargetPos_ from the configured
  // bone + offset + current frame, applying the same object-rotation
  // transform that the mesh gets in DrawScene so the camera lands at
  // the rendered bone position. attachedActive_ ends up false if no
  // bone resolved (Free mode, missing bone, MDD anim).
  void ResolveAttachedTarget(const OverlayStatus& status,
                              std::uint32_t frameIndex);

  // v0.16.0-alpha.2: render joint dots + bone-to-parent lines on top
  // of the mesh. When Attached mode is active, also draws a dashed
  // line ("spring arm") from the smoothed camera position to the
  // attach anchor, plus a marker at the anchor itself. Called from
  // DrawScene after the mesh draw so the lines render in front.
  void DrawSkeleton(const OverlayStatus& status, std::uint32_t frameIndex);

  HWND hwnd_ = nullptr;
  HDC hdc_ = nullptr;
  HGLRC hglrc_ = nullptr;
  OverlayRequests pendingRequests_{};
  bool imguiInitialized_ = false;

  // v0.16.0-alpha.9: see SetOnDestroyCallback above.
  OnDestroyCallback onDestroy_;

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

  // v0.16.0-alpha.7: mirror of viewport.open_on_startup config key.
  bool openOnStartup_ = true;
  bool openOnStartupDirty_ = false;

  // v0.16.0-alpha.1/.2 camera attach state. cameraConfig_ is the
  // authoritative config; cameraDirty_ fires on user edit (UI panel or
  // viewport-relative WASD adjusting offset). attachedActive_ /
  // attachedTargetPos_ are computed each Render() from the bone matrix
  // + offset, then transformed through the model-rotation
  // (objectYaw/Pitch/Roll around pivot) so the camera lands at the
  // RENDERED bone position. UpdateInput drives the smoothed camera
  // position toward this target with damping-controlled tau.
  CameraConfig cameraConfig_;
  bool cameraDirty_ = false;
  bool attachedActive_ = false;        // bone resolved this frame.
  float attachedTargetPos_[3] = {0, 0, 0};
  // Bone world position (without offset) in rendered space — used by
  // DrawSpringArm to mark the attach anchor and connect a dashed line.
  float attachedBoneRenderedPos_[3] = {0, 0, 0};

  // v0.16.0-alpha.3: store the bone's raw world matrix at the current
  // frame so UpdateInput can inverse-transform WASD deltas back into
  // offset-frame (otherwise WASD directions appear "skewed" because
  // the offset is interpreted through bone+object rotations).
  float attachedBoneMatrix_[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

  // v0.16.0-alpha.2 skeleton visualisation.
  bool showSkeleton_ = false;
  bool skeletonDirty_ = false;

  // v0.16.0-alpha.3: per-frame projected joint positions in viewport
  // pixel coordinates. Populated by DrawSkeleton; consumed by the
  // tooltip in DrawOverlay. Empty (cleared) when showSkeleton_ is off
  // or no animation is active. Index matches jointNames[] order.
  std::vector<std::array<float, 2>> jointScreenPos_;

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
