#include "render/gl_viewport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <gl/GL.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"

namespace {
constexpr const wchar_t* kWindowClassName = L"HoloRollViewportWindow";
constexpr float kPi = 3.14159265358979323846f;

using GLsizeiptr_t = std::ptrdiff_t;
using GLintptr_t = std::ptrdiff_t;

constexpr unsigned int kGL_ARRAY_BUFFER = 0x8892;
constexpr unsigned int kGL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr unsigned int kGL_DYNAMIC_DRAW = 0x88E8;

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
  if (p_glGenBuffers && p_glDeleteBuffers && p_glBindBuffer && p_glBufferData && p_glBufferSubData) {
    return true;
  }
  p_glGenBuffers = reinterpret_cast<PFN_glGenBuffers>(wglGetProcAddress("glGenBuffers"));
  p_glDeleteBuffers = reinterpret_cast<PFN_glDeleteBuffers>(wglGetProcAddress("glDeleteBuffers"));
  p_glBindBuffer = reinterpret_cast<PFN_glBindBuffer>(wglGetProcAddress("glBindBuffer"));
  p_glBufferData = reinterpret_cast<PFN_glBufferData>(wglGetProcAddress("glBufferData"));
  p_glBufferSubData = reinterpret_cast<PFN_glBufferSubData>(wglGetProcAddress("glBufferSubData"));
  return p_glGenBuffers && p_glDeleteBuffers && p_glBindBuffer && p_glBufferData && p_glBufferSubData;
}
}  // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK GlViewport::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* self = reinterpret_cast<GlViewport*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (self && self->imguiInitialized_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
    return true;
  }

  switch (msg) {
    // LMB drag = orbit camera.
    case WM_LBUTTONDOWN:
      if (self) {
        self->rotatingCamera_ = true;
        SetCapture(hwnd);
        GetCursorPos(&self->lastMousePos_);
      }
      return 0;
    case WM_LBUTTONUP:
      if (self) {
        self->rotatingCamera_ = false;
        if (!self->rotatingObject_) ReleaseCapture();
      }
      return 0;

    // RMB drag = rotate object.
    case WM_RBUTTONDOWN:
      if (self) {
        self->rotatingObject_ = true;
        SetCapture(hwnd);
        GetCursorPos(&self->lastMousePos_);
      }
      return 0;
    case WM_RBUTTONUP:
      if (self) {
        self->rotatingObject_ = false;
        if (!self->rotatingCamera_) ReleaseCapture();
      }
      return 0;

    case WM_MOUSEWHEEL:
      if (self) {
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

void GlViewport::UpdateCameraFromInput() {
  if (!hwnd_) return;

  if (rotatingCamera_ || rotatingObject_) {
    POINT mousePos{};
    GetCursorPos(&mousePos);
    const int dx = mousePos.x - lastMousePos_.x;
    const int dy = mousePos.y - lastMousePos_.y;
    lastMousePos_ = mousePos;

    if (rotatingCamera_) {
      cameraYaw_ += static_cast<float>(dx) * 0.35f;
      cameraPitch_ = std::clamp(cameraPitch_ + static_cast<float>(dy) * 0.25f, -89.0f, 89.0f);
    }
    if (rotatingObject_) {
      objectYaw_ += static_cast<float>(dx) * 0.5f;
      objectPitch_ = std::clamp(objectPitch_ + static_cast<float>(dy) * 0.5f, -89.0f, 89.0f);
    }
  }

  if (std::abs(wheelDeltaSteps_) > 0.001f) {
    const float step = std::max(0.05f, cameraDistance_ * 0.1f);
    cameraDistance_ = std::clamp(cameraDistance_ - (wheelDeltaSteps_ * step), 0.05f, 50.0f);
    wheelDeltaSteps_ = 0.0f;
  }
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

void GlViewport::DrawScene(const std::vector<float>& vertices,
                           const std::vector<std::uint32_t>& triangleIndices,
                           double playPositionSeconds,
                           const OverlayStatus& status) {
  if (vertices.empty()) return;

  const float tint = static_cast<float>(std::fmod(playPositionSeconds, 1.0));
  const std::size_t pointCount = vertices.size() / 3;
  const bool useVbo = glBuffersAvailable_ && vboId_ != 0;

  if (useVbo) {
    p_glBindBuffer(kGL_ARRAY_BUFFER, vboId_);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, nullptr);
  }

  // Effective render mode: if topology is unavailable for this animation,
  // fall back to Points regardless of the dropdown selection.
  const bool noTopology = triangleIndices.empty() || !status.topologyAvailable;
  const RenderMode effectiveMode = noTopology ? RenderMode::Points : renderMode_;

  if (effectiveMode == RenderMode::Points) {
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColor3f(0.2f + tint * 0.6f, 0.85f, 0.3f);
    glPointSize(pointSize_);

    if (useVbo) {
      glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(pointCount));
    } else {
      glBegin(GL_POINTS);
      for (std::size_t i = 0; i < pointCount; ++i) {
        const float x = vertices[i * 3 + 0];
        const float y = vertices[i * 3 + 1];
        const float z = vertices[i * 3 + 2];
        glVertex3f(x, y * amplitudeScale_, z);
      }
      glEnd();
    }
  } else {
    if (effectiveMode == RenderMode::Wireframe) {
      glDisable(GL_CULL_FACE);
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
      glColor3f(0.3f, 0.9f, 0.95f);
    } else {
      glDisable(GL_CULL_FACE);
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      glColor3f(0.2f + tint * 0.6f, 0.75f, 0.35f);
    }

    if (useVbo && eboId_ != 0 && lastIndexCount_ > 0) {
      p_glBindBuffer(kGL_ELEMENT_ARRAY_BUFFER, eboId_);
      glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(lastIndexCount_), GL_UNSIGNED_INT, nullptr);
    } else {
      glBegin(GL_TRIANGLES);
      for (const auto idx : triangleIndices) {
        const float x = vertices[idx * 3 + 0];
        const float y = vertices[idx * 3 + 1];
        const float z = vertices[idx * 3 + 2];
        glVertex3f(x, y * amplitudeScale_, z);
      }
      glEnd();
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  }

  if (useVbo) {
    glDisableClientState(GL_VERTEX_ARRAY);
    p_glBindBuffer(kGL_ARRAY_BUFFER, 0);
    if (eboId_ != 0) p_glBindBuffer(kGL_ELEMENT_ARRAY_BUFFER, 0);
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
    ImGui::Text("VBO: %s", glBuffersAvailable_ ? "active (glBufferSubData)" : "fallback (immediate mode)");
    if (smoothedFrameMs_ > 0.0) {
      ImGui::Text("Frame time: %.2f ms (%.1f FPS)", smoothedFrameMs_, 1000.0 / smoothedFrameMs_);
    }
  }

  if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen)) {
    int mode = static_cast<int>(renderMode_);
    const char* modeNames[] = {"Points", "Wireframe", "Solid"};
    if (ImGui::Combo("Render mode", &mode, modeNames, 3)) {
      renderMode_ = static_cast<RenderMode>(mode);
    }
    if (!status.topologyAvailable && renderMode_ != RenderMode::Points) {
      ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                         "(no OBJ topology - falls back to Points)");
    }
    ImGui::SliderFloat("Point size", &pointSize_, 1.0f, 6.0f, "%.1f");
    ImGui::SliderFloat("Amplitude", &amplitudeScale_, 0.1f, 3.0f, "%.2f");
    ImGui::SliderFloat("Camera distance", &cameraDistance_, 0.1f, 10.0f, "%.2f");
    if (ImGui::Button("Reset camera")) {
      cameraYaw_ = 35.0f;
      cameraPitch_ = -20.0f;
      cameraDistance_ = 0.8f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset object")) {
      objectYaw_ = 0.0f;
      objectPitch_ = 0.0f;
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled("LMB drag = orbit camera, RMB drag = rotate object, wheel = zoom");
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
  UpdateCameraFromInput();

  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int width = (rc.right - rc.left) > 0 ? (rc.right - rc.left) : 1;
  const int height = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) : 1;
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const float fovYDeg = 55.0f;
  const float nearZ = 0.01f;
  const float farZ = 100.0f;
  const float top = nearZ * std::tan((fovYDeg * 0.5f) * (kPi / 180.0f));
  const float right = top * aspect;

  glViewport(0, 0, width, height);
  glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glFrustum(-right, right, -top, top, nearZ, farZ);

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  // Camera (world -> eye).
  glTranslatef(0.0f, 0.0f, -cameraDistance_);
  glRotatef(cameraPitch_, 1.0f, 0.0f, 0.0f);
  glRotatef(cameraYaw_, 0.0f, 1.0f, 0.0f);
  // Object's own rotation (model -> world).
  glRotatef(objectPitch_, 1.0f, 0.0f, 0.0f);
  glRotatef(objectYaw_, 0.0f, 1.0f, 0.0f);

  if (!vertices.empty()) {
    EnsureGpuBuffers(vertices, triangleIndices);
    UploadFrameToVbo(vertices);
  }

  DrawScene(vertices, triangleIndices, playPositionSeconds, status);
  DrawOverlay(playPositionSeconds, frameIndex, totalFrames, vertices.size(), status);
  SwapBuffers(hdc_);
}

GlViewport::OverlayRequests GlViewport::ConsumeRequests() {
  OverlayRequests out = pendingRequests_;
  pendingRequests_ = {};
  return out;
}

void GlViewport::ApplyPose(const ViewportPose& p) {
  cameraYaw_ = p.cameraYaw;
  cameraPitch_ = p.cameraPitch;
  cameraDistance_ = p.cameraDistance;
  objectYaw_ = p.objectYaw;
  objectPitch_ = p.objectPitch;
  switch (p.renderMode) {
    case 0: renderMode_ = RenderMode::Points; break;
    case 1: renderMode_ = RenderMode::Wireframe; break;
    default: renderMode_ = RenderMode::Solid; break;
  }
}

void GlViewport::CapturePose(ViewportPose& out) const {
  out.cameraYaw = cameraYaw_;
  out.cameraPitch = cameraPitch_;
  out.cameraDistance = cameraDistance_;
  out.objectYaw = objectYaw_;
  out.objectPitch = objectPitch_;
  out.renderMode = static_cast<int>(renderMode_);
  out.initialized = true;
}
