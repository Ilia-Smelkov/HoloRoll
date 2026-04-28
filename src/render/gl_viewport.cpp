#include "render/gl_viewport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <gl/GL.h>

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
                             std::size_t vertexCount,
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
    ImGui::TextWrapped("Folder: %s", status.animationsDir.empty() ? "(not set)" : status.animationsDir.c_str());
    ImGui::Text("Loaded: %u animation(s), %u region(s)",
                static_cast<unsigned>(status.loadedAnimationCount),
                static_cast<unsigned>(status.regionCount));
    ImGui::Text("Active: %s", status.currentAnimation.empty() ? "(none)" : status.currentAnimation.c_str());
    if (status.activeRegionEnd > status.activeRegionStart) {
      ImGui::Text("Region: %.3fs .. %.3fs", status.activeRegionStart, status.activeRegionEnd);
    }
    if (ImGui::Button("Choose folder...")) pendingRequests_.chooseFolder = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload folder")) pendingRequests_.reloadFolder = true;
    ImGui::SameLine();
    if (ImGui::Button("Place regions")) pendingRequests_.placeRegions = true;
  }

  if (ImGui::CollapsingHeader("Config", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Open config")) pendingRequests_.openConfig = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload config")) pendingRequests_.reloadConfig = true;
  }

  if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Playhead: %.3f s", playPositionSeconds);
    ImGui::Text("Frame: %u / %u", frameIndex, totalFrames > 0 ? totalFrames - 1 : 0);
    ImGui::Text("Points: %u", static_cast<unsigned>(vertexCount / 3));
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

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

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
  DrawGizmo(pivotWorld, gizmoRadiusWorld);
  DrawOverlay(playPositionSeconds, frameIndex, totalFrames, vertices.size(), status);
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

void GlViewport::SetSceneSettings(bool showGround, float radius, float gridStep) {
  showGroundPlane_ = showGround;
  groundSize_ = radius;
  groundGridStep_ = gridStep;
}

void GlViewport::GetSceneSettings(bool* showGround, float* radius, float* gridStep) const {
  if (showGround) *showGround = showGroundPlane_;
  if (radius) *radius = groundSize_;
  if (gridStep) *gridStep = groundGridStep_;
}

bool GlViewport::ConsumeSceneDirty() {
  const bool was = sceneDirty_;
  sceneDirty_ = false;
  return was;
}
