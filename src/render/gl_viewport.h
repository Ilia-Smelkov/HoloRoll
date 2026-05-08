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

    float autoPivot[3] = {0.0f, 0.0f, 0.0f};
    float autoExtent = 1.0f;

    // Per-triangle face normals computed by AnimationLibrary on rest pose.
    const std::vector<float>* restNormals = nullptr;

    // When non-empty, the overlay shows a modal dialog asking the user
    // whether to place regions for these newly-discovered animations.
    // entry.cpp populates this from the folder watcher; the user's
    // response comes back via OverlayRequests.newAnimationsChoice.
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

    // Choice from the "new animations detected" modal.
    // 0 = no choice yet (modal still open or never shown).
    // 1 = "Place all" — append regions for all pending new animations.
    // 2 = "Skip" — dismiss the modal, leave regions alone.
    int newAnimationsChoice = 0;

    // Non-empty if the user pressed "+ Place" next to a library entry.
    // entry.cpp uses this to call PlaceSingleAtCursor(name).
    std::string placeSingleAtCursor;

    // v0.12.0-alpha.4: user pressed "Setup motion track". entry.cpp
    // creates (or finds) the dedicated "HoloRoll Motion" track and
    // inserts the holoroll_motion JSFX placeholder on it. No envelope
    // generation yet — alpha.5 will fill in motion data.
    bool setupMotionTrack = false;
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
  void SetPlacementOptions(int variations, float preRollSec, float postRollSec, float regionOverhangSec);
  void GetPlacementOptions(int* variations, float* preRollSec, float* postRollSec, float* regionOverhangSec) const;
  bool ConsumePlacementDirty();

  // ---- Scene settings (persisted in holoroll_config.ini) -----------------
  void SetSceneSettings(bool showGround, float radius, float gridStep,
                        bool showBboxDims, bool showGridLabels, bool showRefHuman);
  void GetSceneSettings(bool* showGround, float* radius, float* gridStep,
                        bool* showBboxDims, bool* showGridLabels, bool* showRefHuman) const;
  bool ConsumeSceneDirty();

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
  // entry.cpp persists them to holoroll_config.ini and stamps the actual
  // pre/post-roll values on each created item via P_EXT.
  //   placementVariations_   - 1..N copies per animation (1 = no variations)
  //   placementPreRollSec_   - hold-frame seconds before the animation
  //   placementPostRollSec_  - hold-frame seconds after the animation
  //   placementRegionOverhang_ - region extends this far past item end
  int   placementVariations_ = 1;
  float placementPreRollSec_ = 1.0f;
  float placementPostRollSec_ = 1.0f;
  float placementRegionOverhang_ = 0.5f;
  bool  placementDirty_ = false;

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
