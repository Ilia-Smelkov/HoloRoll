#include "render/gl_viewport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <gl/GL.h>

#include "extension/drop_target.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"

namespace {
constexpr const wchar_t* kWindowClassName = L"HoloRollViewportWindow";
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kRad2Deg = 180.0f / kPi;

// ---- GL 1.5 buffer-object entry points -------------------------------------
using GLsizeiptr_t = std::ptrdiff_t;
using GLintptr_t = std::ptrdiff_t;

constexpr unsigned int kGL_ARRAY_BUFFER = 0x8892;
constexpr unsigned int kGL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr unsigned int kGL_DYNAMIC_DRAW = 0x88E8;
constexpr unsigned int kGL_BLEND = 0x0BE2;
constexpr unsigned int kGL_SRC_ALPHA = 0x0302;
constexpr unsigned int kGL_ONE_MINUS_SRC_ALPHA = 0x0303;

using PFN_glGenBuffers = void (APIENTRY*)(GLsizei, unsigned int*);
using PFN_glDeleteBuffers = void (APIENTRY*)(GLsizei, const unsigned int*);
using PFN_glBindBuffer = void (APIENTRY*)(unsigned int, unsigned int);
using PFN_glBufferData = void (APIENTRY*)(unsigned int, GLsizeiptr_t, const void*, unsigned int);
using PFN_glBufferSubData = void (APIENTRY*)(unsigned int, GLintptr_t, GLsizeiptr_t, const void*);

PFN_glGenBuffers p_glGenBuffers = nullptr;
PFN_glDeleteBuffers p_glDeleteBuffers = nullptr;
PFN_glBindBuffer p_glBindBuffer = nullptr;
PFN_glBufferData p_glBufferData = nullptr;
PFN_glBufferSubData p_glBufferSubData = nullptr;

bool LoadBufferEntryPoints() {
  if (p_glGenBuffers && p_glDeleteBuffers && p_glBindBuffer && p_glBufferData && p_glBufferSubData)
    return true;
  p_glGenBuffers = reinterpret_cast<PFN_glGenBuffers>(wglGetProcAddress("glGenBuffers"));
  p_glDeleteBuffers = reinterpret_cast<PFN_glDeleteBuffers>(wglGetProcAddress("glDeleteBuffers"));
  p_glBindBuffer = reinterpret_cast<PFN_glBindBuffer>(wglGetProcAddress("glBindBuffer"));
  p_glBufferData = reinterpret_cast<PFN_glBufferData>(wglGetProcAddress("glBufferData"));
  p_glBufferSubData = reinterpret_cast<PFN_glBufferSubData>(wglGetProcAddress("glBufferSubData"));
  return p_glGenBuffers && p_glDeleteBuffers && p_glBindBuffer && p_glBufferData && p_glBufferSubData;
}

bool ProjectToScreen(const float worldXYZ[3],
                     const float mv[16],
                     const float proj[16],
                     int viewportW,
                     int viewportH,
                     float* outX,
                     float* outY) {
  float ev[4];
  for (int i = 0; i < 4; ++i) {
    ev[i] = mv[i + 0] * worldXYZ[0] + mv[i + 4] * worldXYZ[1] + mv[i + 8] * worldXYZ[2] + mv[i + 12];
  }
  float cv[4];
  for (int i = 0; i < 4; ++i) {
    cv[i] = proj[i + 0] * ev[0] + proj[i + 4] * ev[1] + proj[i + 8] * ev[2] + proj[i + 12] * ev[3];
  }
  if (cv[3] <= 0.0001f) return false;
  const float ndcX = cv[0] / cv[3];
  const float ndcY = cv[1] / cv[3];
  *outX = (ndcX * 0.5f + 0.5f) * viewportW;
  *outY = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportH;
  return true;
}

float ScreenDistanceToArc(int ringAxis,
                          const float centre[3],
                          float radius,
                          const float mv[16],
                          const float proj[16],
                          int viewportW,
                          int viewportH,
                          float mouseX,
                          float mouseY,
                          float* outScreenAngleAtClosest) {
  constexpr int kSamples = 64;
  float bestDist = 1e30f;
  float bestScreenAngle = 0.0f;

  float cx, cy;
  if (!ProjectToScreen(centre, mv, proj, viewportW, viewportH, &cx, &cy)) {
    if (outScreenAngleAtClosest) *outScreenAngleAtClosest = 0.0f;
    return 1e30f;
  }

  for (int i = 0; i < kSamples; ++i) {
    const float t = (static_cast<float>(i) / static_cast<float>(kSamples)) * 2.0f * kPi;
    float p[3];
    const float c = std::cos(t) * radius;
    const float s = std::sin(t) * radius;
    if (ringAxis == 0) {
      p[0] = centre[0]; p[1] = centre[1] + c; p[2] = centre[2] + s;
    } else if (ringAxis == 1) {
      p[0] = centre[0] + c; p[1] = centre[1]; p[2] = centre[2] + s;
    } else {
      p[0] = centre[0] + c; p[1] = centre[1] + s; p[2] = centre[2];
    }
    float sx, sy;
    if (!ProjectToScreen(p, mv, proj, viewportW, viewportH, &sx, &sy)) continue;
    const float dx = sx - mouseX;
    const float dy = sy - mouseY;
    const float d2 = dx * dx + dy * dy;
    if (d2 < bestDist) {
      bestDist = d2;
      bestScreenAngle = std::atan2(sy - cy, sx - cx);
    }
  }

  if (outScreenAngleAtClosest) *outScreenAngleAtClosest = bestScreenAngle;
  return std::sqrt(bestDist);
}

bool ImGuiOwnsMouse() {
  if (!ImGui::GetCurrentContext()) return false;
  return ImGui::GetIO().WantCaptureMouse;
}

constexpr int kGizmoAxisToRing[3] = {0, 1, 2};
constexpr float kLightDir[3] = {0.4f, 0.85f, 0.35f};

// Forward decl: defined further down, near GlViewport::Render where the
// other rendering helpers live. Both DrawOverlay and Render call it; this
// forward decl lets DrawOverlay see it without reordering the file.
void DrawDropOverlayImGui(const drop_target::DragState& state);

// v0.10.0 scale-awareness helpers. All defined further down; forward-
// declared here so DrawOverlay can call them.
//
// ComputeBboxFromVertices: scan every vertex of the current frame for
//   min/max XYZ. Cheap (linear, no allocations).
// DrawBboxDimensionsImGui: render an ImGui plate in the top-right corner
//   showing "width x height x depth m".
// DrawGridLabelsImGui: project each major grid intersection within the
//   visible radius to screen and draw "<n>m" text. Uses MVP matrices that
//   GlViewport already caches for the gizmo hit-test.
struct Bbox3 {
  float minX, minY, minZ;
  float maxX, maxY, maxZ;
  bool valid;  // false when vertices is empty
};
Bbox3 ComputeBboxFromVertices(const std::vector<float>& vertices);
void DrawBboxDimensionsImGui(const Bbox3& bbox);
void DrawGridLabelsImGui(float gridStep, float radius,
                         float camPosX, float camPosZ,
                         const float mv[16], const float proj[16],
                         int viewportW, int viewportH);
void DrawReferenceHumanGL(float anchorX, float anchorZ);
}  // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK GlViewport::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* self = reinterpret_cast<GlViewport*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (self && self->imguiInitialized_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
    return true;
  }

  switch (msg) {
    case WM_LBUTTONDOWN:
      if (self && !ImGuiOwnsMouse()) {
        self->lmbPressed_ = true;
        SetCapture(hwnd);
        GetCursorPos(&self->lastMousePos_);
      }
      return 0;
    case WM_LBUTTONUP:
      if (self) {
        self->lmbPressed_ = false;
        if (self->gizmoDragAxis_ >= 0) self->gizmoDragAxis_ = -1;
        if (!self->flyMouseLook_) ReleaseCapture();
      }
      return 0;

    case WM_RBUTTONDOWN:
      if (self && !ImGuiOwnsMouse()) {
        self->flyMouseLook_ = true;
        SetCapture(hwnd);
        GetCursorPos(&self->lastMousePos_);
      }
      return 0;
    case WM_RBUTTONUP:
      if (self) {
        self->flyMouseLook_ = false;
        if (!self->lmbPressed_) ReleaseCapture();
      }
      return 0;

    case WM_MOUSEWHEEL:
      if (self && !ImGuiOwnsMouse()) {
        self->wheelDeltaSteps_ +=
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
      }
      return 0;

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (self) {
        self->DestroyContext();
        self->hwnd_ = nullptr;
      }
      return 0;
    default:
      return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

bool GlViewport::CreateContext() {
  hdc_ = GetDC(hwnd_);
  if (!hdc_) return false;

  PIXELFORMATDESCRIPTOR pfd{};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 24;
  pfd.cDepthBits = 24;
  pfd.iLayerType = PFD_MAIN_PLANE;

  const int format = ChoosePixelFormat(hdc_, &pfd);
  if (format == 0 || !SetPixelFormat(hdc_, format, &pfd)) return false;

  hglrc_ = wglCreateContext(hdc_);
  if (!hglrc_) return false;
  if (!wglMakeCurrent(hdc_, hglrc_)) return false;

  glBuffersAvailable_ = LoadBufferEntryPoints();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  if (!ImGui_ImplWin32_Init(hwnd_)) return false;
  if (!ImGui_ImplOpenGL3_Init("#version 130")) return false;
  imguiInitialized_ = true;

  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  return true;
}

void GlViewport::DestroyContext() {
  if (imguiInitialized_) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    imguiInitialized_ = false;
  }

  if (glBuffersAvailable_ && p_glDeleteBuffers) {
    if (vboId_ != 0) { p_glDeleteBuffers(1, &vboId_); vboId_ = 0; }
    if (eboId_ != 0) { p_glDeleteBuffers(1, &eboId_); eboId_ = 0; }
  }
  vboCapacityBytes_ = 0;
  eboCapacityBytes_ = 0;
  lastIndexCount_ = 0;

  if (hglrc_) {
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hglrc_);
    hglrc_ = nullptr;
  }
  if (hdc_ && hwnd_) {
    ReleaseDC(hwnd_, hdc_);
    hdc_ = nullptr;
  }
}

bool GlViewport::Open() {
  if (hwnd_) return true;

  WNDCLASSW wc{};
  wc.lpfnWndProc = &GlViewport::WndProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = kWindowClassName;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  hwnd_ = CreateWindowExW(
      0, kWindowClassName, L"HoloRoll",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      CW_USEDEFAULT, CW_USEDEFAULT, 1200, 760,
      nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
  if (!hwnd_) return false;
  SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  if (!CreateContext()) { Close(); return false; }
  return true;
}

void GlViewport::Close() {
  if (hwnd_) DestroyWindow(hwnd_);
}

void GlViewport::Tick() {
  MSG msg{};
  while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

// ---- Default 3/4-perspective camera frame ----------------------------------
//
// Position the camera so the model's bbox occupies a consistent share of the
// viewport, regardless of the model's actual world scale, and frame it from
// a "Blender front-right-top" angle (yaw=-35, pitch=-25). The look target is
// the pivot point (bbox centre + user offset), so the model ends up in the
// centre of the screen.
//
// Distance is derived from the pinhole projection so that the apparent size
// of the bbox matches `kViewportFillFraction` of the viewport's vertical
// half-extent. With autoExtent in world units D and field-of-view fovY:
//
//   tan(fovY/2) = (D/2 / fillFraction) / dist
//   dist = D / (2 * tan(fovY/2) * fillFraction)
//
// fillFraction = 0.6 puts the bbox at ~60% of viewport height. Smaller values
// would leave more blank space; larger values risk clipping with rotation.
void GlViewport::ResetCameraToDefault(const OverlayStatus& status) {
  // Default 3/4 angles. Negative yaw rotates the camera to the right of the
  // model (so we see its left-front face). Negative pitch tilts the camera
  // downward to look slightly down on the model.
  constexpr float kDefaultYawDeg   = -35.0f;
  constexpr float kDefaultPitchDeg = -25.0f;
  constexpr float kFovYDeg         =  55.0f;
  constexpr float kFillFraction    =  0.60f;

  // Pivot we want to look at = autoPivot + user offset (matches DrawScene).
  const float pivotX = status.autoPivot[0] + pivotOffset_[0];
  const float pivotY = status.autoPivot[1] + pivotOffset_[1];
  const float pivotZ = status.autoPivot[2] + pivotOffset_[2];

  const float bboxDiameter = std::max(0.05f, status.autoExtent);
  const float halfFovTan = std::tan((kFovYDeg * 0.5f) * kDeg2Rad);
  const float distance = std::max(
      0.5f,
      bboxDiameter / (2.0f * halfFovTan * kFillFraction));

  // Forward vector from camera angles, matching the formula in UpdateInput()
  // and the matrix order in ApplyCameraTransform():
  //   forward = (-sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch))
  //
  // Camera position = pivot - forward * distance (camera placed behind the
  // model along its forward direction).
  const float yawR   = kDefaultYawDeg   * kDeg2Rad;
  const float pitchR = kDefaultPitchDeg * kDeg2Rad;
  const float fx = -std::sin(yawR) * std::cos(pitchR);
  const float fy =  std::sin(pitchR);
  const float fz = -std::cos(yawR) * std::cos(pitchR);

  cameraPosTargetX_ = pivotX - fx * distance;
  cameraPosTargetY_ = pivotY - fy * distance;
  cameraPosTargetZ_ = pivotZ - fz * distance;

  // Snap immediately so reset feels decisive (no smoothing animation).
  cameraPosX_ = cameraPosTargetX_;
  cameraPosY_ = cameraPosTargetY_;
  cameraPosZ_ = cameraPosTargetZ_;

  cameraYawTarget_   = kDefaultYawDeg;
  cameraPitchTarget_ = kDefaultPitchDeg;
  cameraYaw_   = kDefaultYawDeg;
  cameraPitch_ = kDefaultPitchDeg;

  // Fly speed scales with model size so WASD always feels brisk regardless
  // of whether the bbox is 0.3 units (small character) or 50 units (huge).
  flySpeed_ = std::max(0.5f, bboxDiameter * 2.0f);
}

void GlViewport::UpdateInput(float dtSeconds) {
  if (!hwnd_) return;

  int dx = 0, dy = 0;
  if (flyMouseLook_ || gizmoDragAxis_ >= 0) {
    POINT mousePos{};
    GetCursorPos(&mousePos);
    dx = mousePos.x - lastMousePos_.x;
    dy = mousePos.y - lastMousePos_.y;
    lastMousePos_ = mousePos;
  }

  if (flyMouseLook_) {
    cameraYawTarget_ -= static_cast<float>(dx) * 0.2f;
    cameraPitchTarget_ = std::clamp(cameraPitchTarget_ - static_cast<float>(dy) * 0.2f, -89.0f, 89.0f);
  }

  if (std::abs(wheelDeltaSteps_) > 0.001f) {
    flySpeed_ = std::clamp(flySpeed_ * std::pow(1.15f, wheelDeltaSteps_), 0.05f, 50.0f);
    wheelDeltaSteps_ = 0.0f;
  }

  if (flyMouseLook_) {
    auto isDown = [](int vk) {
      return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };

    const float yawR = cameraYaw_ * kDeg2Rad;
    const float pitchR = cameraPitch_ * kDeg2Rad;
    const float fx = -std::sin(yawR) * std::cos(pitchR);
    const float fy =  std::sin(pitchR);
    const float fz = -std::cos(yawR) * std::cos(pitchR);
    const float rx =  std::cos(yawR);
    const float rz = -std::sin(yawR);

    const float speed = flySpeed_ * dtSeconds;
    float moveX = 0.0f, moveY = 0.0f, moveZ = 0.0f;

    if (isDown('W')) { moveX += fx * speed; moveY += fy * speed; moveZ += fz * speed; }
    if (isDown('S')) { moveX -= fx * speed; moveY -= fy * speed; moveZ -= fz * speed; }
    if (isDown('D')) { moveX += rx * speed; moveZ += rz * speed; }
    if (isDown('A')) { moveX -= rx * speed; moveZ -= rz * speed; }
    if (isDown('E')) { moveY += speed; }
    if (isDown('Q')) { moveY -= speed; }

    cameraPosTargetX_ += moveX;
    cameraPosTargetY_ += moveY;
    cameraPosTargetZ_ += moveZ;
  }

  constexpr float kTau = 0.08f;
  const float alpha = 1.0f - std::exp(-dtSeconds / kTau);

  cameraYaw_   += (cameraYawTarget_   - cameraYaw_)   * alpha;
  cameraPitch_ += (cameraPitchTarget_ - cameraPitch_) * alpha;
  cameraPosX_  += (cameraPosTargetX_  - cameraPosX_)  * alpha;
  cameraPosY_  += (cameraPosTargetY_  - cameraPosY_)  * alpha;
  cameraPosZ_  += (cameraPosTargetZ_  - cameraPosZ_)  * alpha;
}

void GlViewport::EnsureGpuBuffers(const std::vector<float>& vertices,
                                  const std::vector<std::uint32_t>& triangleIndices) {
  if (!glBuffersAvailable_) return;

  const std::size_t neededVboBytes = vertices.size() * sizeof(float);
  if (vboId_ == 0) p_glGenBuffers(1, &vboId_);
  p_glBindBuffer(kGL_ARRAY_BUFFER, vboId_);
  if (neededVboBytes > vboCapacityBytes_) {
    p_glBufferData(kGL_ARRAY_BUFFER, static_cast<GLsizeiptr_t>(neededVboBytes), nullptr, kGL_DYNAMIC_DRAW);
    vboCapacityBytes_ = neededVboBytes;
  }

  const std::size_t neededEboBytes = triangleIndices.size() * sizeof(std::uint32_t);
  if (neededEboBytes > 0) {
    if (eboId_ == 0) p_glGenBuffers(1, &eboId_);
    p_glBindBuffer(kGL_ELEMENT_ARRAY_BUFFER, eboId_);
    if (neededEboBytes != eboCapacityBytes_) {
      p_glBufferData(kGL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr_t>(neededEboBytes),
                     triangleIndices.data(), kGL_DYNAMIC_DRAW);
      eboCapacityBytes_ = neededEboBytes;
    }
    lastIndexCount_ = triangleIndices.size();
  } else {
    lastIndexCount_ = 0;
  }
}

void GlViewport::UploadFrameToVbo(const std::vector<float>& vertices) {
  if (!glBuffersAvailable_ || vboId_ == 0 || vertices.empty()) return;
  const std::size_t bytes = vertices.size() * sizeof(float);
  p_glBindBuffer(kGL_ARRAY_BUFFER, vboId_);
  p_glBufferSubData(kGL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr_t>(bytes), vertices.data());
}

void GlViewport::ApplyCameraTransform() {
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glRotatef(-cameraPitch_, 1.0f, 0.0f, 0.0f);
  glRotatef(-cameraYaw_,   0.0f, 1.0f, 0.0f);
  glTranslatef(-cameraPosX_, -cameraPosY_, -cameraPosZ_);
}

void GlViewport::DrawBackgroundGradient() {
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  const float topR = 0.18f, topG = 0.30f, topB = 0.46f;
  const float botR = 0.07f, botG = 0.12f, botB = 0.18f;

  glBegin(GL_TRIANGLE_STRIP);
    glColor3f(botR, botG, botB); glVertex2f(-1.0f, -1.0f);
    glColor3f(botR, botG, botB); glVertex2f( 1.0f, -1.0f);
    glColor3f(topR, topG, topB); glVertex2f(-1.0f,  1.0f);
    glColor3f(topR, topG, topB); glVertex2f( 1.0f,  1.0f);
  glEnd();

  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);

  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

void GlViewport::DrawGroundPlane() {
  if (!showGroundPlane_) return;

  const float radius = std::max(2.0f, groundSize_);
  const float minorStep = std::max(0.05f, groundGridStep_);
  const float majorStep = minorStep * 10.0f;

  const float camX = cameraPosX_;
  const float camY = cameraPosY_;
  const float camZ = cameraPosZ_;

  const float fadeStart = radius * 0.5f;
  const float fadeEnd   = radius * 1.0f;
  const float fadeRange = std::max(0.001f, fadeEnd - fadeStart);

  auto distFade = [&](float x, float z) {
    const float dx = x - camX;
    const float dy = -camY;
    const float dz = z - camZ;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d <= fadeStart) return 1.0f;
    if (d >= fadeEnd) return 0.0f;
    return 1.0f - (d - fadeStart) / fadeRange;
  };

  glDisable(GL_CULL_FACE);
  glEnable(kGL_BLEND);
  glBlendFunc(kGL_SRC_ALPHA, kGL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);

  constexpr int kSegmentsPerLine = 16;
  auto drawFadingLine = [&](float x0, float z0, float x1, float z1,
                            float r, float g, float b, float intensity) {
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= kSegmentsPerLine; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(kSegmentsPerLine);
      const float x = x0 + (x1 - x0) * t;
      const float z = z0 + (z1 - z0) * t;
      const float a = distFade(x, z) * intensity;
      glColor4f(r, g, b, a);
      glVertex3f(x, 0.0f, z);
    }
    glEnd();
  };

  {
    const float originX = std::floor(camX / minorStep) * minorStep;
    const float originZ = std::floor(camZ / minorStep) * minorStep;
    const int halfSteps = static_cast<int>(std::ceil(radius / minorStep));

    glLineWidth(1.0f);
    constexpr float minorR = 0.45f, minorG = 0.50f, minorB = 0.58f;
    constexpr float minorIntensity = 0.55f;

    for (int i = -halfSteps; i <= halfSteps; ++i) {
      const float t = static_cast<float>(i) * minorStep;

      const float lineX = originX + t;
      drawFadingLine(lineX, originZ - radius, lineX, originZ + radius,
                     minorR, minorG, minorB, minorIntensity);

      const float lineZ = originZ + t;
      drawFadingLine(originX - radius, lineZ, originX + radius, lineZ,
                     minorR, minorG, minorB, minorIntensity);
    }
  }

  {
    const float originX = std::floor(camX / majorStep) * majorStep;
    const float originZ = std::floor(camZ / majorStep) * majorStep;
    const int halfSteps = static_cast<int>(std::ceil(radius / majorStep));

    glLineWidth(1.5f);
    constexpr float majorR = 0.70f, majorG = 0.75f, majorB = 0.85f;
    constexpr float majorIntensity = 0.85f;

    for (int i = -halfSteps; i <= halfSteps; ++i) {
      const float t = static_cast<float>(i) * majorStep;

      const float lineX = originX + t;
      drawFadingLine(lineX, originZ - radius, lineX, originZ + radius,
                     majorR, majorG, majorB, majorIntensity);

      const float lineZ = originZ + t;
      drawFadingLine(originX - radius, lineZ, originX + radius, lineZ,
                     majorR, majorG, majorB, majorIntensity);
    }
  }

  {
    glLineWidth(2.0f);
    drawFadingLine(-radius, 0.0f,  radius, 0.0f,  0.85f, 0.40f, 0.40f, 1.0f);
    drawFadingLine( 0.0f, -radius,  0.0f,  radius, 0.40f, 0.80f, 0.40f, 1.0f);
  }

  glLineWidth(1.0f);
  glDepthMask(GL_TRUE);
  glDisable(kGL_BLEND);
}

void GlViewport::DrawScene(const std::vector<float>& vertices,
                           const std::vector<std::uint32_t>& triangleIndices,
                           double playPositionSeconds,
                           const OverlayStatus& status) {
  (void)playPositionSeconds;
  if (vertices.empty()) return;

  const std::size_t pointCount = vertices.size() / 3;
  if (pointCount == 0) return;

  const float px = status.autoPivot[0] + pivotOffset_[0];
  const float py = status.autoPivot[1] + pivotOffset_[1];
  const float pz = status.autoPivot[2] + pivotOffset_[2];

  glPushMatrix();
  glTranslatef(px, py, pz);
  glRotatef(objectYaw_,   0.0f, 1.0f, 0.0f);
  glRotatef(objectPitch_, 1.0f, 0.0f, 0.0f);
  glRotatef(objectRoll_,  0.0f, 0.0f, 1.0f);
  glTranslatef(-px, -py, -pz);

  const bool noTopology = triangleIndices.empty() || !status.topologyAvailable;
  const RenderMode effectiveMode = noTopology ? RenderMode::Points : renderMode_;

  if (effectiveMode == RenderMode::Points) {
    const bool useVbo = glBuffersAvailable_ && vboId_ != 0;
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColor3f(0.85f, 0.90f, 0.95f);
    glPointSize(pointSize_);

    if (useVbo) {
      p_glBindBuffer(kGL_ARRAY_BUFFER, vboId_);
      glEnableClientState(GL_VERTEX_ARRAY);
      glVertexPointer(3, GL_FLOAT, 0, nullptr);
      glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(pointCount));
      glDisableClientState(GL_VERTEX_ARRAY);
      p_glBindBuffer(kGL_ARRAY_BUFFER, 0);
    } else {
      glBegin(GL_POINTS);
      for (std::size_t i = 0; i < pointCount; ++i) {
        glVertex3f(vertices[i * 3 + 0], vertices[i * 3 + 1] * amplitudeScale_, vertices[i * 3 + 2]);
      }
      glEnd();
    }
  } else if (effectiveMode == RenderMode::Wireframe) {
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glColor3f(0.80f, 0.85f, 0.90f);

    const bool useVbo = glBuffersAvailable_ && vboId_ != 0 && eboId_ != 0 && lastIndexCount_ > 0;

    if (useVbo) {
      p_glBindBuffer(kGL_ARRAY_BUFFER, vboId_);
      glEnableClientState(GL_VERTEX_ARRAY);
      glVertexPointer(3, GL_FLOAT, 0, nullptr);
      p_glBindBuffer(kGL_ELEMENT_ARRAY_BUFFER, eboId_);
      glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(lastIndexCount_), GL_UNSIGNED_INT, nullptr);
      glDisableClientState(GL_VERTEX_ARRAY);
      p_glBindBuffer(kGL_ARRAY_BUFFER, 0);
      p_glBindBuffer(kGL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
      glBegin(GL_TRIANGLES);
      for (const auto idx : triangleIndices) {
        if (idx >= pointCount) continue;
        glVertex3f(vertices[idx * 3 + 0], vertices[idx * 3 + 1] * amplitudeScale_, vertices[idx * 3 + 2]);
      }
      glEnd();
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  } else {
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    auto rotateY = [](float ang, float v[3]) {
      const float c = std::cos(ang * kDeg2Rad);
      const float s = std::sin(ang * kDeg2Rad);
      const float x = v[0] * c + v[2] * s;
      const float z = -v[0] * s + v[2] * c;
      v[0] = x; v[2] = z;
    };
    auto rotateX = [](float ang, float v[3]) {
      const float c = std::cos(ang * kDeg2Rad);
      const float s = std::sin(ang * kDeg2Rad);
      const float y = v[1] * c - v[2] * s;
      const float z = v[1] * s + v[2] * c;
      v[1] = y; v[2] = z;
    };
    auto rotateZ = [](float ang, float v[3]) {
      const float c = std::cos(ang * kDeg2Rad);
      const float s = std::sin(ang * kDeg2Rad);
      const float x = v[0] * c - v[1] * s;
      const float y = v[0] * s + v[1] * c;
      v[0] = x; v[1] = y;
    };

    float lightLocal[3] = {kLightDir[0], kLightDir[1], kLightDir[2]};
    rotateZ(-objectRoll_,  lightLocal);
    rotateX(-objectPitch_, lightLocal);
    rotateY(-objectYaw_,   lightLocal);
    {
      const float L = std::sqrt(lightLocal[0]*lightLocal[0] + lightLocal[1]*lightLocal[1] + lightLocal[2]*lightLocal[2]);
      if (L > 1e-6f) { lightLocal[0] /= L; lightLocal[1] /= L; lightLocal[2] /= L; }
    }

    const std::vector<float>* nrmPtr = status.restNormals;
    const std::size_t triCount = triangleIndices.size() / 3;
    const bool haveNormals = nrmPtr && nrmPtr->size() >= triCount * 3;

    constexpr float kAmbient = 0.30f;
    constexpr float kDiffuse = 0.70f;

    glBegin(GL_TRIANGLES);
    for (std::size_t t = 0; t < triCount; ++t) {
      const std::uint32_t i0 = triangleIndices[t * 3 + 0];
      const std::uint32_t i1 = triangleIndices[t * 3 + 1];
      const std::uint32_t i2 = triangleIndices[t * 3 + 2];

      if (i0 >= pointCount || i1 >= pointCount || i2 >= pointCount) continue;

      float intensity;
      if (haveNormals) {
        const float nx = (*nrmPtr)[t * 3 + 0];
        const float ny = (*nrmPtr)[t * 3 + 1];
        const float nz = (*nrmPtr)[t * 3 + 2];
        float ndotl = nx * lightLocal[0] + ny * lightLocal[1] + nz * lightLocal[2];
        if (ndotl < 0.0f) ndotl = 0.0f;
        intensity = kAmbient + kDiffuse * ndotl;
      } else {
        intensity = 0.7f;
      }
      const float r = std::min(1.0f, intensity * 1.00f);
      const float g = std::min(1.0f, intensity * 0.98f);
      const float b = std::min(1.0f, intensity * 0.94f);
      glColor3f(r, g, b);

      glVertex3f(vertices[i0 * 3 + 0], vertices[i0 * 3 + 1] * amplitudeScale_, vertices[i0 * 3 + 2]);
      glVertex3f(vertices[i1 * 3 + 0], vertices[i1 * 3 + 1] * amplitudeScale_, vertices[i1 * 3 + 2]);
      glVertex3f(vertices[i2 * 3 + 0], vertices[i2 * 3 + 1] * amplitudeScale_, vertices[i2 * 3 + 2]);
    }
    glEnd();
  }

  glPopMatrix();
}

void GlViewport::DrawGizmo(const float pivotWorld[3], float screenRadiusWorld) {
  if (!showGizmo_) return;

  glDisable(GL_DEPTH_TEST);
  glLineWidth(2.5f);

  constexpr int kSamples = 64;

  auto drawArc = [&](int gizmoAxis, float r, float g, float b) {
    const int ringAxis = kGizmoAxisToRing[gizmoAxis];
    glColor3f(r, g, b);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < kSamples; ++i) {
      const float t = (static_cast<float>(i) / static_cast<float>(kSamples)) * 2.0f * kPi;
      const float c = std::cos(t) * screenRadiusWorld;
      const float s = std::sin(t) * screenRadiusWorld;
      float p[3] = {pivotWorld[0], pivotWorld[1], pivotWorld[2]};
      if (ringAxis == 0) { p[1] += c; p[2] += s; }
      else if (ringAxis == 1) { p[0] += c; p[2] += s; }
      else { p[0] += c; p[1] += s; }
      glVertex3fv(p);
    }
    glEnd();
  };

  auto colourFor = [&](int gizmoAxis, float baseR, float baseG, float baseB,
                       float* outR, float* outG, float* outB) {
    const bool active = (gizmoAxis == gizmoDragAxis_) ||
                        (gizmoAxis == gizmoHoverAxis_ && gizmoDragAxis_ < 0);
    if (active) {
      *outR = std::min(1.0f, baseR + 0.4f);
      *outG = std::min(1.0f, baseG + 0.4f);
      *outB = std::min(1.0f, baseB + 0.4f);
    } else {
      *outR = baseR; *outG = baseG; *outB = baseB;
    }
  };

  float r, g, b;
  colourFor(0, 0.95f, 0.25f, 0.25f, &r, &g, &b); drawArc(0, r, g, b);
  colourFor(1, 0.30f, 0.45f, 1.00f, &r, &g, &b); drawArc(1, r, g, b);
  colourFor(2, 0.25f, 0.95f, 0.25f, &r, &g, &b); drawArc(2, r, g, b);

  glLineWidth(1.0f);
  glEnable(GL_DEPTH_TEST);
}

void GlViewport::GizmoHitTestAndDrag(const float pivotWorld[3], float screenRadiusWorld) {
  if (!showGizmo_) {
    gizmoHoverAxis_ = -1;
    gizmoDragAxis_ = -1;
    return;
  }

  if (ImGuiOwnsMouse() && gizmoDragAxis_ < 0) {
    gizmoHoverAxis_ = -1;
    return;
  }

  POINT cursor{};
  GetCursorPos(&cursor);
  ScreenToClient(hwnd_, &cursor);
  const float mx = static_cast<float>(cursor.x);
  const float my = static_cast<float>(cursor.y);

  constexpr float kPickThresholdPx = 14.0f;

  if (gizmoDragAxis_ >= 0) {
    if (!lmbPressed_) { gizmoDragAxis_ = -1; return; }
    const int ringAxis = kGizmoAxisToRing[gizmoDragAxis_];
    float curAngle;
    ScreenDistanceToArc(ringAxis, pivotWorld, screenRadiusWorld,
                        matModelView_, matProjection_,
                        viewportWidth_, viewportHeight_, mx, my, &curAngle);
    float deltaDeg = (curAngle - gizmoDragStartAngle_) * kRad2Deg;
    while (deltaDeg > 180.0f) deltaDeg -= 360.0f;
    while (deltaDeg < -180.0f) deltaDeg += 360.0f;

    const float newRot = gizmoDragStartRotation_ + deltaDeg;
    if (gizmoDragAxis_ == 0) objectPitch_ = newRot;
    else if (gizmoDragAxis_ == 1) objectYaw_ = newRot;
    else objectRoll_ = newRot;
    return;
  }

  float bestDist = 1e30f;
  int bestGizmoAxis = -1;
  float bestAngle = 0.0f;

  for (int gizmoAxis = 0; gizmoAxis < 3; ++gizmoAxis) {
    const int ringAxis = kGizmoAxisToRing[gizmoAxis];
    float angle;
    const float d = ScreenDistanceToArc(ringAxis, pivotWorld, screenRadiusWorld,
                                        matModelView_, matProjection_,
                                        viewportWidth_, viewportHeight_, mx, my, &angle);
    if (d < bestDist) {
      bestDist = d;
      bestGizmoAxis = gizmoAxis;
      bestAngle = angle;
    }
  }

  if (bestDist < kPickThresholdPx) {
    gizmoHoverAxis_ = bestGizmoAxis;
    if (lmbPressed_ && gizmoDragAxis_ < 0) {
      gizmoDragAxis_ = bestGizmoAxis;
      gizmoDragStartAngle_ = bestAngle;
      if (bestGizmoAxis == 0) gizmoDragStartRotation_ = objectPitch_;
      else if (bestGizmoAxis == 1) gizmoDragStartRotation_ = objectYaw_;
      else gizmoDragStartRotation_ = objectRoll_;
    }
  } else {
    gizmoHoverAxis_ = -1;
  }
}

void GlViewport::DrawOverlay(double playPositionSeconds,
                             std::uint32_t frameIndex,
                             std::uint32_t totalFrames,
                             const std::vector<float>& vertices,
                             const OverlayStatus& status) {
  if (!imguiInitialized_) return;

  const ULONGLONG now = GetTickCount64();
  if (lastFrameTick_ != 0) {
    const double frameMs = static_cast<double>(now - lastFrameTick_);
    smoothedFrameMs_ = (smoothedFrameMs_ <= 0.0) ? frameMs : smoothedFrameMs_ * 0.9 + frameMs * 0.1;
  }
  lastFrameTick_ = now;

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGui::Begin("HoloRoll", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

  if (ImGui::CollapsingHeader("Library", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (status.projectUntitled) {
      ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                         "Save the REAPER project to enable HoloRoll.");
      ImGui::TextWrapped("HoloRoll uses a project-relative folder "
                         "(<project>/Animations/) to know where animations live. "
                         "Until you save the project, there is no place to look.");
    } else {
      ImGui::TextWrapped("Folder: %s", status.animationsDir.empty() ? "(not set)" : status.animationsDir.c_str());
      if (status.folderIsOverride) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f),
                           "(override - not the project default)");
      }
      ImGui::Text("Loaded: %u animation(s), %u item(s) on timeline",
                  static_cast<unsigned>(status.loadedAnimationCount),
                  static_cast<unsigned>(status.regionCount));
      ImGui::Text("Active: %s", status.currentAnimation.empty() ? "(none)" : status.currentAnimation.c_str());
      if (status.activeRegionEnd > status.activeRegionStart) {
        ImGui::Text("Item time: %.3fs .. %.3fs", status.activeRegionStart, status.activeRegionEnd);
      }

      // Warning: an item is under the playhead but its name does not match any
      // animation in the current library. Shows in red so it's hard to miss.
      if (!status.missingAnimationName.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                           "⚠ Animation '%s' not found",
                           status.missingAnimationName.c_str());
      }
    }

    if (ImGui::Button("Choose folder...")) pendingRequests_.chooseFolder = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload folder")) pendingRequests_.reloadFolder = true;
    ImGui::SameLine();
    if (ImGui::Button("Place all")) pendingRequests_.placeRegions = true;

    // v0.11.0: placement options. Inline so they're visible right next to
    // the Place all button — the value is what "Place all" will use.
    // The dirty flag fires once any of these fires; entry.cpp picks that
    // up via ConsumePlacementDirty and writes to holoroll_config.ini.
    ImGui::PushItemWidth(80.0f);
    if (ImGui::InputInt("Variations", &placementVariations_)) {
      placementVariations_ = std::max(1, std::min(20, placementVariations_));
      placementDirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("How many copies of each animation to place. >1 creates _2, _3 ... "
                        "variation suffixes — same animation, separate items so you can\n"
                        "layer different sounds.");
    }
    if (ImGui::InputFloat("Pre-roll (s)", &placementPreRollSec_, 0.0f, 0.0f, "%.2f")) {
      placementPreRollSec_ = std::max(0.0f, std::min(10.0f, placementPreRollSec_));
      placementDirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Hold-frame buffer BEFORE the animation. Frame 0 is shown during\n"
                        "this section so the user has space for anticipation sounds.");
    }
    if (ImGui::InputFloat("Post-roll (s)", &placementPostRollSec_, 0.0f, 0.0f, "%.2f")) {
      placementPostRollSec_ = std::max(0.0f, std::min(10.0f, placementPostRollSec_));
      placementDirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Hold-frame buffer AFTER the animation. Last frame is shown during\n"
                        "this section so reverbs/tails have room.");
    }
    if (ImGui::InputFloat("Region overhang (s)", &placementRegionOverhang_, 0.0f, 0.0f, "%.2f")) {
      placementRegionOverhang_ = std::max(0.0f, std::min(10.0f, placementRegionOverhang_));
      placementDirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Region extends this many seconds past the end of the item.\n"
                        "Visual handle for grouping; doesn't affect playback.");
    }
    ImGui::PopItemWidth();

    // "Reset to default folder" only shows when an override is active — it
    // would be a no-op confusion otherwise.
    if (status.folderIsOverride) {
      if (ImGui::Button("Reset to default folder")) pendingRequests_.resetFolderOverride = true;
    }

    // v0.6.0 SPIKE: validates we can create empty named items via REAPER API.
    // Will be removed (or repurposed) once the items workflow lands.
    ImGui::Separator();
    ImGui::TextDisabled("DEV: items API spike");
    if (ImGui::Button("Test create item")) pendingRequests_.spikeTestCreateItem = true;

    // v0.12.0-alpha.4: motion track setup. Creates (or finds) a dedicated
    // "HoloRoll Motion" track and inserts the holoroll_motion JSFX on it.
    // Envelope generation comes in alpha.5; this button just lays the
    // groundwork.
    ImGui::Separator();
    ImGui::TextDisabled("v0.12.0-alpha.4: motion envelope groundwork");
    if (ImGui::Button("Setup motion track")) pendingRequests_.setupMotionTrack = true;
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Creates a dedicated 'HoloRoll Motion' track at the bottom of the\n"
                        "track list and inserts the holoroll_motion JSFX placeholder on it.\n"
                        "Idempotent \u2014 safe to press multiple times.\n"
                        "Envelope writing for selected bones lands in v0.12.0-alpha.5.");
    }
  }

  if (ImGui::CollapsingHeader("Config", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Open config")) pendingRequests_.openConfig = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload config")) pendingRequests_.reloadConfig = true;
  }

  if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Playhead: %.3f s", playPositionSeconds);
    ImGui::Text("Frame: %u / %u", frameIndex, totalFrames > 0 ? totalFrames - 1 : 0);
    ImGui::Text("Points: %u", static_cast<unsigned>(vertices.size() / 3));
    if (smoothedFrameMs_ > 0.0) {
      ImGui::Text("Frame time: %.2f ms (%.1f FPS)", smoothedFrameMs_, 1000.0 / smoothedFrameMs_);
    }
  }

  if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextDisabled("Hold RMB + WASD/QE to fly. Wheel = speed.");
    ImGui::SliderFloat("Fly speed", &flySpeed_, 0.05f, 10.0f, "%.2f");
    if (ImGui::Button("Reset camera")) {
      ResetCameraToDefault(status);
    }
  }

  if (ImGui::CollapsingHeader("Object", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Show rotation gizmo", &showGizmo_);
    constexpr float kRotMin = -360.0f;
    constexpr float kRotMax =  360.0f;
    constexpr float kRotSpeed = 0.5f;
    ImGui::DragFloat("Yaw (Y)",   &objectYaw_,   kRotSpeed, kRotMin, kRotMax, "%.1f");
    ImGui::DragFloat("Pitch (X)", &objectPitch_, kRotSpeed, kRotMin, kRotMax, "%.1f");
    ImGui::DragFloat("Roll (Z)",  &objectRoll_,  kRotSpeed, kRotMin, kRotMax, "%.1f");
    if (ImGui::Button("Reset rotation")) {
      objectYaw_ = objectPitch_ = objectRoll_ = 0.0f;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Pivot offset (relative to bbox centre)");
    const float pivotRange = std::max(0.5f, status.autoExtent * 1.5f);
    ImGui::SliderFloat("X##pivot", &pivotOffset_[0], -pivotRange, pivotRange, "%.3f");
    ImGui::SliderFloat("Y##pivot", &pivotOffset_[1], -pivotRange, pivotRange, "%.3f");
    ImGui::SliderFloat("Z##pivot", &pivotOffset_[2], -pivotRange, pivotRange, "%.3f");
    if (ImGui::Button("Reset pivot")) {
      pivotOffset_[0] = pivotOffset_[1] = pivotOffset_[2] = 0.0f;
    }
  }

  if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Checkbox("Show ground plane", &showGroundPlane_)) sceneDirty_ = true;
    if (ImGui::SliderFloat("Visible radius", &groundSize_, 5.0f, 200.0f, "%.0f")) sceneDirty_ = true;
    if (ImGui::SliderFloat("Grid step", &groundGridStep_, 0.05f, 5.0f, "%.2f")) sceneDirty_ = true;

    ImGui::Separator();
    if (ImGui::Checkbox("Show bbox dimensions", &showBboxDimensions_)) sceneDirty_ = true;
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Top-right plate showing the model's current X x Y x Z size in metres.\n"
                        "Assumes 1 scene unit == 1 metre (Blender default).");
    }
    if (ImGui::Checkbox("Show grid labels", &showGridLabels_)) sceneDirty_ = true;
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Number labels on major grid intersections (every 10 minor steps).");
    }
    if (ImGui::Checkbox("Show 1.80m reference", &showReferenceHuman_)) sceneDirty_ = true;
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("1.80-metre stick figure to the right of the model for size reference.");
    }
  }

  if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen)) {
    int rmode = static_cast<int>(renderMode_);
    const char* rmodeNames[] = {"Points", "Wireframe", "Solid"};
    if (ImGui::Combo("Render mode", &rmode, rmodeNames, 3)) {
      renderMode_ = static_cast<RenderMode>(rmode);
    }
    if (!status.topologyAvailable && renderMode_ != RenderMode::Points) {
      ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                         "(no OBJ topology - falls back to Points)");
    }
    ImGui::SliderFloat("Point size", &pointSize_, 1.0f, 6.0f, "%.1f");
    ImGui::SliderFloat("Amplitude", &amplitudeScale_, 0.1f, 3.0f, "%.2f");
  }

  ImGui::Separator();
  ImGui::TextDisabled("RMB + WASD/QE = fly | wheel = speed | LMB-drag arcs = rotate");
  ImGui::End();

  // "New animations detected" modal. Shown when the folder watcher reports
  // additions (entry.cpp populates status.pendingNewAnimations). The user's
  // response is folded into pendingRequests_.newAnimationsChoice so
  // entry.cpp can act on it after ConsumeRequests() returns.
  if (!status.pendingNewAnimations.empty()) {
    const int choice = DrawNewAnimationsModal(status.pendingNewAnimations);
    if (choice != 0) {
      pendingRequests_.newAnimationsChoice = choice;
    }
  }

  // Drop-zone visual feedback. Drawn last so it goes on top of the
  // overlay, the modal, and the 3D scene. Cheap when no drag is active
  // (just an atomic load + an if).
  DrawDropOverlayImGui(drop_target::GetDragState());

  // v0.10.0 scale aids: bbox dimensions plate and grid labels. Both opt-in,
  // both go on the foreground draw list so they sit above the regular
  // panels but below the drop overlay (drop overlay was drawn last above).
  if (showBboxDimensions_) {
    DrawBboxDimensionsImGui(ComputeBboxFromVertices(vertices));
  }
  if (showGridLabels_) {
    DrawGridLabelsImGui(groundGridStep_, groundSize_,
                        cameraPosX_, cameraPosZ_,
                        matModelView_, matProjection_,
                        viewportWidth_, viewportHeight_);
  }

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

int GlViewport::DrawNewAnimationsModal(const std::vector<std::string>& pending) {
  // The modal is opened on first frame after `pending` becomes non-empty;
  // ImGui keeps it open until we pass false. We track that by always calling
  // OpenPopup whenever pending is non-empty — calling it on an already-open
  // popup is a no-op.
  constexpr const char* kPopupId = "New animations detected##holoroll";
  ImGui::OpenPopup(kPopupId);

  ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);

  int choice = 0;
  if (ImGui::BeginPopupModal(kPopupId, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped("Found %d new animation(s) in the watched folder:",
                       static_cast<int>(pending.size()));
    ImGui::Spacing();

    // Show the list in a scrollable child if there are many.
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float maxListHeight = lineHeight * 8.0f;
    const float wantedHeight = std::min(maxListHeight,
                                        lineHeight * static_cast<float>(pending.size()) + 8.0f);
    ImGui::BeginChild("##new_animations_list", ImVec2(0, wantedHeight), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& name : pending) {
      ImGui::BulletText("%s", name.c_str());
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextWrapped("Place regions for these after the last existing region?");
    ImGui::Spacing();

    if (ImGui::Button("Place all", ImVec2(120, 0))) {
      choice = 1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(120, 0))) {
      choice = 2;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }
  return choice;
}

// Render the drag-n-drop visual feedback over everything else. Called
// from inside the active ImGui frame (so the resulting draw commands
// land on top of the regular overlay/3D scene) but before ImGui::Render.
//
// Three states drive the visual:
//   - valid + host accepts          → green, "Drop here to add to project"
//   - valid + host rejects (Untitled) → amber, "Save the REAPER project first"
//   - not valid (wrong extensions)  → red,   "Unsupported file type"
//
// The overlay consists of three layers, drawn into ImGui's foreground draw
// list so they sit above any window:
//   1. A 35% black fullscreen rectangle (dims the scene/UI underneath)
//   2. An 8-pixel coloured border around the viewport
//   3. A centered text plate with the per-state message
//
// Defined inside the anonymous namespace at the top of the file (forward-
// declared there so DrawOverlay can call it). Linkage matches the
// declaration: internal, namespace-scoped, no `static` needed.
namespace {
void DrawDropOverlayImGui(const drop_target::DragState& state) {
  if (!state.isDragging) return;

  // Pick colours and message based on state.
  ImU32 borderColor;
  ImU32 plateColor;
  ImU32 textColor;
  const char* message;
  if (state.hasValidFiles && state.hostAccepts) {
    borderColor = IM_COL32(70, 220, 110, 255);   // green
    plateColor  = IM_COL32(20, 50, 30, 235);
    textColor   = IM_COL32(220, 255, 230, 255);
    message     = "Drop here to add to project";
  } else if (state.hasValidFiles && !state.hostAccepts) {
    borderColor = IM_COL32(240, 180, 50, 255);   // amber
    plateColor  = IM_COL32(60, 40, 10, 235);
    textColor   = IM_COL32(255, 230, 180, 255);
    message     = "Save the REAPER project first";
  } else {
    borderColor = IM_COL32(220, 80, 80, 255);    // red
    plateColor  = IM_COL32(60, 20, 20, 235);
    textColor   = IM_COL32(255, 220, 220, 255);
    message     = "Unsupported file type";
  }

  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const ImVec2 size = ImGui::GetIO().DisplaySize;

  // Layer 1: dim the scene underneath.
  draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 90));

  // Layer 2: thick border. Drawn as 4 filled rects rather than AddRect's
  // line so the corners are clean and the thickness is exact.
  constexpr float kBorder = 8.0f;
  draw->AddRectFilled(ImVec2(0, 0),                ImVec2(size.x, kBorder),         borderColor);
  draw->AddRectFilled(ImVec2(0, size.y - kBorder), ImVec2(size.x, size.y),          borderColor);
  draw->AddRectFilled(ImVec2(0, 0),                ImVec2(kBorder, size.y),         borderColor);
  draw->AddRectFilled(ImVec2(size.x - kBorder, 0), ImVec2(size.x, size.y),          borderColor);

  // Layer 3: centered text plate. Sized to the message with generous padding;
  // never wider than 80% of the viewport so very narrow docks still look OK.
  const ImVec2 textSize = ImGui::CalcTextSize(message);
  const float plateW = std::min(size.x * 0.8f, textSize.x + 64.0f);
  const float plateH = textSize.y + 32.0f;
  const ImVec2 plateMin(size.x * 0.5f - plateW * 0.5f,
                        size.y * 0.5f - plateH * 0.5f);
  const ImVec2 plateMax(plateMin.x + plateW, plateMin.y + plateH);
  draw->AddRectFilled(plateMin, plateMax, plateColor, 6.0f);
  draw->AddRect(plateMin, plateMax, borderColor, 6.0f, 0, 2.0f);

  // Center the text inside the plate (use measured size rather than
  // re-calling CalcTextSize for the same string).
  const ImVec2 textPos(plateMin.x + (plateW - textSize.x) * 0.5f,
                       plateMin.y + (plateH - textSize.y) * 0.5f);
  draw->AddText(textPos, textColor, message);
}

// v0.10.0: compute axis-aligned bbox of the current frame's vertices.
// Cheap O(N) scan; called every frame from DrawOverlay (so we can show
// the live bbox dimensions, which change during animation).
Bbox3 ComputeBboxFromVertices(const std::vector<float>& vertices) {
  Bbox3 b{};
  b.valid = false;
  if (vertices.size() < 3) return b;
  b.minX = b.maxX = vertices[0];
  b.minY = b.maxY = vertices[1];
  b.minZ = b.maxZ = vertices[2];
  const std::size_t pointCount = vertices.size() / 3;
  for (std::size_t i = 1; i < pointCount; ++i) {
    const float x = vertices[i * 3 + 0];
    const float y = vertices[i * 3 + 1];
    const float z = vertices[i * 3 + 2];
    if (x < b.minX) b.minX = x; else if (x > b.maxX) b.maxX = x;
    if (y < b.minY) b.minY = y; else if (y > b.maxY) b.maxY = y;
    if (z < b.minZ) b.minZ = z; else if (z > b.maxZ) b.maxZ = z;
  }
  b.valid = true;
  return b;
}

// v0.10.0: small "X x Y x Z m" plate in the top-right of the viewport.
// Sits below the regular HoloRoll panel and the drop overlay (z-order via
// foreground draw list ordering). Assumes 1 unit == 1 metre, which is
// Blender's default; comments explain the caveat in the toggle's tooltip.
void DrawBboxDimensionsImGui(const Bbox3& bbox) {
  if (!bbox.valid) return;
  const float w = bbox.maxX - bbox.minX;
  const float h = bbox.maxY - bbox.minY;
  const float d = bbox.maxZ - bbox.minZ;

  char buf[96];
  std::snprintf(buf, sizeof(buf), "%.2f x %.2f x %.2f m", w, h, d);

  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
  const ImVec2 textSize = ImGui::CalcTextSize(buf);

  // Position: top-right, with a small margin. Keep clear of the regular
  // HoloRoll panel which lives top-left.
  constexpr float kMargin = 12.0f;
  constexpr float kPad = 8.0f;
  const ImVec2 plateMax(displaySize.x - kMargin, kMargin + textSize.y + kPad * 2.0f);
  const ImVec2 plateMin(plateMax.x - textSize.x - kPad * 2.0f, kMargin);
  draw->AddRectFilled(plateMin, plateMax, IM_COL32(20, 25, 30, 200), 4.0f);
  draw->AddRect(plateMin, plateMax, IM_COL32(120, 140, 160, 220), 4.0f, 0, 1.0f);

  const ImVec2 textPos(plateMin.x + kPad, plateMin.y + kPad);
  draw->AddText(textPos, IM_COL32(220, 230, 240, 255), buf);
}

// v0.10.0: project major grid intersections to screen and draw small
// "<n>m" labels at each one. Uses the same major-step logic as
// DrawGroundPlane (`minorStep * 10`). Skips the origin (where the X/Z
// axis lines cross) since it's visually self-explanatory.
void DrawGridLabelsImGui(float gridStep, float radius,
                         float camPosX, float camPosZ,
                         const float mv[16], const float proj[16],
                         int viewportW, int viewportH) {
  if (gridStep <= 0.0f || viewportW <= 0 || viewportH <= 0) return;

  const float majorStep = gridStep * 10.0f;
  // Anchor the label grid to the camera so it stays dense as the user
  // flies around. Match DrawGroundPlane's snap.
  const float originX = std::floor(camPosX / majorStep) * majorStep;
  const float originZ = std::floor(camPosZ / majorStep) * majorStep;
  const int halfSteps = static_cast<int>(std::ceil(radius / majorStep));

  // Cap the number of labels we'll draw per frame so a very wide grid
  // doesn't murder ImGui with a thousand text quads. Past this, we sparse
  // out further (every other label).
  constexpr int kMaxLabels = 64;
  int labelCount = 0;

  ImDrawList* draw = ImGui::GetForegroundDrawList();

  for (int i = -halfSteps; i <= halfSteps && labelCount < kMaxLabels; ++i) {
    const float x = originX + static_cast<float>(i) * majorStep;
    if (std::abs(x) < majorStep * 0.5f) continue;  // skip origin
    const float worldXYZ[3] = {x, 0.0f, originZ};
    float sx, sy;
    if (!ProjectToScreen(worldXYZ, mv, proj, viewportW, viewportH, &sx, &sy)) continue;
    if (sx < 0 || sy < 0 || sx > viewportW || sy > viewportH) continue;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%dm", static_cast<int>(std::round(x)));
    draw->AddText(ImVec2(sx + 3, sy + 3), IM_COL32(180, 200, 220, 220), buf);
    ++labelCount;
  }
  for (int i = -halfSteps; i <= halfSteps && labelCount < kMaxLabels; ++i) {
    const float z = originZ + static_cast<float>(i) * majorStep;
    if (std::abs(z) < majorStep * 0.5f) continue;
    const float worldXYZ[3] = {originX, 0.0f, z};
    float sx, sy;
    if (!ProjectToScreen(worldXYZ, mv, proj, viewportW, viewportH, &sx, &sy)) continue;
    if (sx < 0 || sy < 0 || sx > viewportW || sy > viewportH) continue;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%dm", static_cast<int>(std::round(z)));
    draw->AddText(ImVec2(sx + 3, sy + 3), IM_COL32(180, 200, 220, 220), buf);
    ++labelCount;
  }
}
// v0.10.0: render a 2-metre tall stick figure made of cylinders, used as a
// scale reference next to the user's model. Drawn in immediate mode (slow
// in principle but cheap in practice — ~50 quads). Positioned at
// (anchorX, 0, anchorZ) with feet on the ground; faces +Z by convention.
//
// Pure OpenGL legacy. Doesn't use the VBO/EBO path — immediate mode is
// fine for this many quads, and threading it through the regular DrawScene
// pipeline would mean inventing a second mesh format.
void DrawStickFigureCylinder(float x0, float y0, float z0,
                             float x1, float y1, float z1,
                             float radius, int segments,
                             float r, float g, float b, float a) {
  // Direction & length.
  const float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
  const float length = std::sqrt(dx*dx + dy*dy + dz*dz);
  if (length < 1e-6f) return;

  // Build an orthonormal basis around the cylinder axis (dir, perp1, perp2).
  const float invLen = 1.0f / length;
  const float ax = dx * invLen, ay = dy * invLen, az = dz * invLen;
  // Pick a 'tmp' vector not parallel to axis.
  float tx = (std::abs(ay) < 0.9f) ? 0.0f : 1.0f;
  float ty = (std::abs(ay) < 0.9f) ? 1.0f : 0.0f;
  float tz = 0.0f;
  // perp1 = normalize(cross(axis, tmp))
  float p1x = ay*tz - az*ty;
  float p1y = az*tx - ax*tz;
  float p1z = ax*ty - ay*tx;
  const float p1len = std::sqrt(p1x*p1x + p1y*p1y + p1z*p1z);
  if (p1len < 1e-6f) return;
  p1x /= p1len; p1y /= p1len; p1z /= p1len;
  // perp2 = cross(axis, perp1)
  const float p2x = ay*p1z - az*p1y;
  const float p2y = az*p1x - ax*p1z;
  const float p2z = ax*p1y - ay*p1x;

  glColor4f(r, g, b, a);
  glBegin(GL_QUAD_STRIP);
  for (int i = 0; i <= segments; ++i) {
    const float t = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * kPi;
    const float c = std::cos(t) * radius;
    const float s = std::sin(t) * radius;
    const float ox = p1x * c + p2x * s;
    const float oy = p1y * c + p2y * s;
    const float oz = p1z * c + p2z * s;
    glVertex3f(x0 + ox, y0 + oy, z0 + oz);
    glVertex3f(x1 + ox, y1 + oy, z1 + oz);
  }
  glEnd();
}

void DrawReferenceHumanGL(float anchorX, float anchorZ) {
  // 1.80-metre tall stick figure with arms-down pose. Proportions roughly
  // Vitruvian, scaled to 1.80 m total height:
  //   head    0.225 m diameter, top at 1.80 m
  //   torso   from 0.90 m to 1.53 m
  //   legs    from 0.0 m to 0.90 m, separated by 0.20 m
  //   arms    hanging down from shoulder (1.48 m) to wrist (0.78 m)
  // Faces +Z (toward the user's model since we position it on the +X side).

  const float headTop     = 1.80f;
  const float headBottom  = 1.575f;  // 0.225 m head
  const float headRadius  = 0.1125f;
  const float headCenterY = 0.5f * (headTop + headBottom);
  const float torsoTop    = 1.53f;
  const float torsoBottom = 0.90f;
  const float armRadius   = 0.045f;
  const float legRadius   = 0.063f;
  const float torsoRadius = 0.108f;

  // Translucent grey so the eye reads it as "reference" not "foreground".
  constexpr float r = 0.55f, g = 0.60f, b = 0.65f, a = 0.65f;

  glEnable(kGL_BLEND);
  glBlendFunc(kGL_SRC_ALPHA, kGL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);  // don't write depth so the model still sorts in front when overlapping

  // Head (vertical capsule simplified to a short cylinder).
  DrawStickFigureCylinder(anchorX, headBottom, anchorZ,
                          anchorX, headTop,    anchorZ,
                          headRadius, 16, r, g, b, a);

  // Torso.
  DrawStickFigureCylinder(anchorX, torsoBottom, anchorZ,
                          anchorX, torsoTop,    anchorZ,
                          torsoRadius, 16, r, g, b, a);

  // Arms hanging down from shoulders. Shoulder at top of torso, slightly
  // offset outward so the arms don't visually merge with the torso
  // cylinder. Wrists hang to about hip level (~0.80 m).
  const float shoulderY = torsoTop - 0.05f;
  const float wristY    = 0.78f;
  const float shoulderXOffset = torsoRadius + armRadius * 0.5f;
  // Left arm.
  DrawStickFigureCylinder(anchorX - shoulderXOffset, shoulderY, anchorZ,
                          anchorX - shoulderXOffset, wristY,    anchorZ,
                          armRadius, 12, r, g, b, a);
  // Right arm.
  DrawStickFigureCylinder(anchorX + shoulderXOffset, shoulderY, anchorZ,
                          anchorX + shoulderXOffset, wristY,    anchorZ,
                          armRadius, 12, r, g, b, a);

  // Legs.
  DrawStickFigureCylinder(anchorX - 0.10f, 0.0f,        anchorZ,
                          anchorX - 0.10f, torsoBottom, anchorZ,
                          legRadius, 12, r, g, b, a);
  DrawStickFigureCylinder(anchorX + 0.10f, 0.0f,        anchorZ,
                          anchorX + 0.10f, torsoBottom, anchorZ,
                          legRadius, 12, r, g, b, a);

  // ---- Face: eyes + smile -------------------------------------------------
  //
  // Drawn on the +Z side of the head (model-facing). Pure point/line
  // primitives so we don't build geometry. Slightly offset out from the
  // head surface so they don't z-fight with the head cylinder.
  //
  // We disable depth test for the face so it always wins against the head
  // even if rounding pushes them to the same depth.
  glDisable(GL_DEPTH_TEST);

  const float facePlaneZ = anchorZ + headRadius + 0.005f;  // a hair in front
  const float eyeY       = headCenterY + headRadius * 0.30f;
  const float eyeOffsetX = headRadius * 0.40f;
  const float smileY     = headCenterY - headRadius * 0.25f;
  const float smileHalfW = headRadius * 0.40f;
  const float smileDip   = headRadius * 0.20f;

  // Eyes (two solid dots).
  glColor4f(0.10f, 0.12f, 0.14f, std::min(1.0f, a + 0.30f));
  glPointSize(5.0f);
  glBegin(GL_POINTS);
  glVertex3f(anchorX - eyeOffsetX, eyeY, facePlaneZ);
  glVertex3f(anchorX + eyeOffsetX, eyeY, facePlaneZ);
  glEnd();
  glPointSize(1.0f);

  // Smile (downward arc, sampled). Drawn as a line strip; line width 2 to
  // make it readable at the small head size.
  glLineWidth(2.0f);
  glBegin(GL_LINE_STRIP);
  constexpr int kSmileSamples = 12;
  for (int i = 0; i <= kSmileSamples; ++i) {
    const float t  = static_cast<float>(i) / static_cast<float>(kSmileSamples);  // 0..1
    const float u  = t * 2.0f - 1.0f;  // -1..1
    const float sx = anchorX + u * smileHalfW;
    // Parabola dipping down at the centre, returning at the corners.
    const float sy = smileY - smileDip * (1.0f - u * u);
    glVertex3f(sx, sy, facePlaneZ);
  }
  glEnd();
  glLineWidth(1.0f);

  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDisable(kGL_BLEND);
}

}  // namespace

void GlViewport::Render(const std::vector<float>& vertices,
                        const std::vector<std::uint32_t>& triangleIndices,
                        double playPositionSeconds,
                        std::uint32_t frameIndex,
                        std::uint32_t totalFrames,
                        const OverlayStatus& status) {
  if (!hwnd_ || !hdc_ || !hglrc_) return;

  wglMakeCurrent(hdc_, hglrc_);

  const ULONGLONG now = GetTickCount64();
  const float dt = lastTickMs_ == 0 ? 0.016f : std::min(0.1f, (now - lastTickMs_) / 1000.0f);
  lastTickMs_ = now;

  UpdateInput(dt);

  RECT rc{};
  GetClientRect(hwnd_, &rc);
  viewportWidth_ = (rc.right - rc.left) > 0 ? (rc.right - rc.left) : 1;
  viewportHeight_ = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) : 1;

  // v0.8.0: Untitled-project early-out. Skip the 3D scene, gizmo, overlay,
  // background gradient, ground plane — everything except a single centered
  // message. The dock window is otherwise idle / empty so the user knows
  // the plugin is intentionally inactive, not broken.
  if (status.projectUntitled) {
    glViewport(0, 0, viewportWidth_, viewportHeight_);
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (imguiInitialized_) {
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();

      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      const char* msg = "Save the REAPER project to enable HoloRoll";
      const ImVec2 textSize = ImGui::CalcTextSize(msg);
      ImGui::SetNextWindowPos(ImVec2((displaySize.x - textSize.x) * 0.5f - 16.0f,
                                     (displaySize.y - textSize.y) * 0.5f - 8.0f));
      ImGui::SetNextWindowBgAlpha(0.0f);
      ImGui::Begin("##holoroll_untitled", nullptr,
                   ImGuiWindowFlags_NoTitleBar |
                   ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "%s", msg);
      ImGui::End();

      // Drop-zone visual feedback (drawn even in Untitled state because
      // we register the drop target whenever the viewport is open).
      DrawDropOverlayImGui(drop_target::GetDragState());

      ImGui::Render();
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    SwapBuffers(hdc_);
    return;
  }

  const float aspect = static_cast<float>(viewportWidth_) / static_cast<float>(viewportHeight_);
  const float fovYDeg = 55.0f;
  const float nearZ = 0.01f;
  const float farZ = 500.0f;
  const float top = nearZ * std::tan((fovYDeg * 0.5f) * kDeg2Rad);
  const float right = top * aspect;

  glViewport(0, 0, viewportWidth_, viewportHeight_);
  glClear(GL_DEPTH_BUFFER_BIT);

  DrawBackgroundGradient();

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glFrustum(-right, right, -top, top, nearZ, farZ);
  glGetFloatv(GL_PROJECTION_MATRIX, matProjection_);

  const float pivotWorld[3] = {
    status.autoPivot[0] + pivotOffset_[0],
    status.autoPivot[1] + pivotOffset_[1],
    status.autoPivot[2] + pivotOffset_[2],
  };

  ApplyCameraTransform();
  glGetFloatv(GL_MODELVIEW_MATRIX, matModelView_);

  DrawGroundPlane();

  const float dx = pivotWorld[0] - cameraPosX_;
  const float dy = pivotWorld[1] - cameraPosY_;
  const float dz = pivotWorld[2] - cameraPosZ_;
  const float dist = std::max(0.05f, std::sqrt(dx * dx + dy * dy + dz * dz));
  const float halfFovTan = std::tan((fovYDeg * 0.5f) * kDeg2Rad);
  const float gizmoRadiusWorld = dist * halfFovTan * 0.18f;

  GizmoHitTestAndDrag(pivotWorld, gizmoRadiusWorld);

  if (!vertices.empty()) {
    EnsureGpuBuffers(vertices, triangleIndices);
    UploadFrameToVbo(vertices);
  }

  DrawScene(vertices, triangleIndices, playPositionSeconds, status);

  // v0.10.0: 1.80-metre reference human, drawn after the user's mesh so it
  // overlaps cleanly. Anchored to the FRAME-0 bbox edge — not the current
  // frame's bbox — so the human stays still while the animation plays.
  // Otherwise it jitters/follows the user's model around as the bbox shifts
  // each frame, which defeats the whole point of a static size reference.
  if (showReferenceHuman_) {
    // status.autoPivot is the frame-0 bbox centre; autoExtent is the frame-0
    // diameter. The right edge of the model is therefore pivot.x + extent/2.
    // Half-extent overestimates X width when the model is taller than wide,
    // but that's harmless: the human just sits a bit further out, never on
    // top of the model.
    const float halfExtent = std::max(0.1f, status.autoExtent * 0.5f);
    const float anchorX = status.autoPivot[0] + halfExtent + 0.5f;
    const float anchorZ = status.autoPivot[2];
    DrawReferenceHumanGL(anchorX, anchorZ);
  }

  DrawGizmo(pivotWorld, gizmoRadiusWorld);
  DrawOverlay(playPositionSeconds, frameIndex, totalFrames, vertices, status);
  SwapBuffers(hdc_);
}

GlViewport::OverlayRequests GlViewport::ConsumeRequests() {
  OverlayRequests out = pendingRequests_;
  pendingRequests_ = {};
  return out;
}

void GlViewport::ApplyPose(const ViewportPose& p) {
  cameraPosX_ = cameraPosTargetX_ = p.cameraPosX;
  cameraPosY_ = cameraPosTargetY_ = p.cameraPosY;
  cameraPosZ_ = cameraPosTargetZ_ = p.cameraPosZ;
  cameraYaw_  = cameraYawTarget_  = p.cameraYaw;
  cameraPitch_ = cameraPitchTarget_ = p.cameraPitch;
  flySpeed_ = p.flySpeed;

  objectYaw_ = p.objectYaw;
  objectPitch_ = p.objectPitch;
  objectRoll_ = p.objectRoll;

  pivotOffset_[0] = p.pivotOffsetX;
  pivotOffset_[1] = p.pivotOffsetY;
  pivotOffset_[2] = p.pivotOffsetZ;

  switch (p.renderMode) {
    case 0: renderMode_ = RenderMode::Points; break;
    case 1: renderMode_ = RenderMode::Wireframe; break;
    default: renderMode_ = RenderMode::Solid; break;
  }
}

void GlViewport::CapturePose(ViewportPose& out) const {
  out.cameraPosX = cameraPosX_;
  out.cameraPosY = cameraPosY_;
  out.cameraPosZ = cameraPosZ_;
  out.cameraYaw  = cameraYaw_;
  out.cameraPitch = cameraPitch_;
  out.flySpeed = flySpeed_;

  out.objectYaw = objectYaw_;
  out.objectPitch = objectPitch_;
  out.objectRoll = objectRoll_;

  out.pivotOffsetX = pivotOffset_[0];
  out.pivotOffsetY = pivotOffset_[1];
  out.pivotOffsetZ = pivotOffset_[2];

  out.renderMode = static_cast<int>(renderMode_);
  out.initialized = true;
}

// ---- Persisted scene settings API ------------------------------------------

void GlViewport::SetSceneSettings(bool showGround, float radius, float gridStep,
                                  bool showBboxDims, bool showGridLabels, bool showRefHuman) {
  showGroundPlane_ = showGround;
  groundSize_ = radius;
  groundGridStep_ = gridStep;
  showBboxDimensions_ = showBboxDims;
  showGridLabels_ = showGridLabels;
  showReferenceHuman_ = showRefHuman;
}

void GlViewport::GetSceneSettings(bool* showGround, float* radius, float* gridStep,
                                  bool* showBboxDims, bool* showGridLabels, bool* showRefHuman) const {
  if (showGround) *showGround = showGroundPlane_;
  if (radius) *radius = groundSize_;
  if (gridStep) *gridStep = groundGridStep_;
  if (showBboxDims) *showBboxDims = showBboxDimensions_;
  if (showGridLabels) *showGridLabels = showGridLabels_;
  if (showRefHuman) *showRefHuman = showReferenceHuman_;
}

bool GlViewport::ConsumeSceneDirty() {
  const bool was = sceneDirty_;
  sceneDirty_ = false;
  return was;
}

// ---- v0.11.0 placement options API -----------------------------------------

void GlViewport::SetPlacementOptions(int variations, float preRollSec, float postRollSec, float regionOverhangSec) {
  placementVariations_ = std::max(1, std::min(20, variations));
  placementPreRollSec_ = std::max(0.0f, std::min(10.0f, preRollSec));
  placementPostRollSec_ = std::max(0.0f, std::min(10.0f, postRollSec));
  placementRegionOverhang_ = std::max(0.0f, std::min(10.0f, regionOverhangSec));
}

void GlViewport::GetPlacementOptions(int* variations, float* preRollSec, float* postRollSec, float* regionOverhangSec) const {
  if (variations) *variations = placementVariations_;
  if (preRollSec) *preRollSec = placementPreRollSec_;
  if (postRollSec) *postRollSec = placementPostRollSec_;
  if (regionOverhangSec) *regionOverhangSec = placementRegionOverhang_;
}

bool GlViewport::ConsumePlacementDirty() {
  const bool was = placementDirty_;
  placementDirty_ = false;
  return was;
}
