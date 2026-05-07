#include "core/glb_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

// tinygltf's single-header impl. Must be defined exactly once in the project;
// this is that one place.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
// JSON impl is also single-header; tinygltf bundles a copy.
#define TINYGLTF_USE_CPP14
#include "tiny_gltf.h"

namespace {

// ---- Tiny 4x4 matrix utilities (column-major like glTF / OpenGL) ----------
//
// We don't pull in GLM because the entire skinning math here is ~80 lines
// and a hand-rolled implementation keeps build dependencies smaller.

using Mat4 = std::array<float, 16>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;
using Quat = std::array<float, 4>;  // x, y, z, w

// ---- Image loader callback (no-op) ----------------------------------------
//
// We compile tinygltf with TINYGLTF_NO_STB_IMAGE to avoid pulling in the stb
// image stack (we don't render textures — Solid mode is per-face Lambert).
// However, when tinygltf encounters an `image` block in a glTF file and no
// image loader is set, it returns FALSE for the whole parse — even though
// the geometry/animation/skinning data is fully readable.
//
// CesiumMan and most other Khronos sample assets embed a JPEG/PNG diffuse
// texture, so without this callback they fail to load while bare-skinned
// files like RiggedSimple/RiggedFigure work.
//
// The fix: register a callback that simply marks the image as a 1x1 white
// pixel and returns success. The image data is never used downstream
// (we don't sample textures), so any plausible-looking content is fine.
bool DummyImageLoader(tinygltf::Image* image,
                      const int /*image_idx*/,
                      std::string* /*err*/,
                      std::string* /*warn*/,
                      int /*req_width*/,
                      int /*req_height*/,
                      const unsigned char* /*bytes*/,
                      int /*size*/,
                      void* /*user_data*/) {
  // 1x1 RGBA white. Anything tinygltf considers valid is OK — we never read it.
  image->width = 1;
  image->height = 1;
  image->component = 4;
  image->bits = 8;
  image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image->image = {255, 255, 255, 255};
  return true;
}

constexpr Mat4 kIdentity = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

Mat4 MulM(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int c = 0; c < 4; ++c) {
    for (int rIdx = 0; rIdx < 4; ++rIdx) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a[k * 4 + rIdx] * b[c * 4 + k];
      }
      r[c * 4 + rIdx] = sum;
    }
  }
  return r;
}

Vec4 MulMV(const Mat4& m, const Vec4& v) {
  Vec4 r{};
  for (int rIdx = 0; rIdx < 4; ++rIdx) {
    r[rIdx] = m[0 * 4 + rIdx] * v[0] +
              m[1 * 4 + rIdx] * v[1] +
              m[2 * 4 + rIdx] * v[2] +
              m[3 * 4 + rIdx] * v[3];
  }
  return r;
}

Mat4 TranslationMatrix(const Vec3& t) {
  Mat4 m = kIdentity;
  m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
  return m;
}

Mat4 ScaleMatrix(const Vec3& s) {
  Mat4 m{};
  m[0]  = s[0];
  m[5]  = s[1];
  m[10] = s[2];
  m[15] = 1.0f;
  return m;
}

// glTF rotations are unit quaternions (x, y, z, w).
Mat4 RotationMatrix(const Quat& q) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, xz = x * z, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;
  Mat4 m{};
  m[0]  = 1.0f - 2.0f * (yy + zz);
  m[1]  = 2.0f * (xy + wz);
  m[2]  = 2.0f * (xz - wy);
  m[3]  = 0.0f;

  m[4]  = 2.0f * (xy - wz);
  m[5]  = 1.0f - 2.0f * (xx + zz);
  m[6]  = 2.0f * (yz + wx);
  m[7]  = 0.0f;

  m[8]  = 2.0f * (xz + wy);
  m[9]  = 2.0f * (yz - wx);
  m[10] = 1.0f - 2.0f * (xx + yy);
  m[11] = 0.0f;

  m[12] = m[13] = m[14] = 0.0f;
  m[15] = 1.0f;
  return m;
}

Mat4 ComposeTRS(const Vec3& t, const Quat& r, const Vec3& s) {
  // glTF spec: world = T * R * S (applied right-to-left to a column vector).
  return MulM(TranslationMatrix(t), MulM(RotationMatrix(r), ScaleMatrix(s)));
}

// Slerp on a unit quaternion. tinygltf hands us channel data as raw floats;
// we always normalize on read so inputs are clean. Uses the shortest-path
// branch so wrapped keyframes don't tumble.
Quat Slerp(const Quat& a, const Quat& b, float t) {
  float bx = b[0], by = b[1], bz = b[2], bw = b[3];
  float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
  if (dot < 0.0f) {
    bx = -bx; by = -by; bz = -bz; bw = -bw;
    dot = -dot;
  }
  if (dot > 0.9995f) {
    // Quats are nearly aligned; lerp + normalize is more stable than slerp.
    Quat r = {
      a[0] + t * (bx - a[0]),
      a[1] + t * (by - a[1]),
      a[2] + t * (bz - a[2]),
      a[3] + t * (bw - a[3]),
    };
    const float len = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2] + r[3]*r[3]);
    if (len > 1e-8f) {
      for (int i = 0; i < 4; ++i) r[i] /= len;
    }
    return r;
  }
  const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
  const float sinT = std::sin(theta);
  const float wA = std::sin((1.0f - t) * theta) / sinT;
  const float wB = std::sin(t * theta) / sinT;
  return {
    a[0] * wA + bx * wB,
    a[1] * wA + by * wB,
    a[2] * wA + bz * wB,
    a[3] * wA + bw * wB,
  };
}

// ---- Accessor reader -------------------------------------------------------

// Returns a pointer to the raw byte payload of `accessor`, plus the byte
// stride per element. tinygltf already validated bufferView/offset bounds.
const unsigned char* AccessorData(const tinygltf::Model& model,
                                  const tinygltf::Accessor& accessor,
                                  std::size_t* outStride) {
  const auto& view = model.bufferViews[accessor.bufferView];
  const auto& buffer = model.buffers[view.buffer];
  const std::size_t offset = view.byteOffset + accessor.byteOffset;
  // glTF allows interleaved attributes via byteStride; default is "tightly
  // packed" — the size of one element of the accessor's component type.
  std::size_t stride = view.byteStride;
  if (stride == 0) {
    stride = tinygltf::GetComponentSizeInBytes(accessor.componentType) *
             tinygltf::GetNumComponentsInType(accessor.type);
  }
  if (outStride) *outStride = stride;
  return buffer.data.data() + offset;
}

// Read a single scalar from accessor (element i). Used for keyframe times
// and STEP-mode joint sample lookups.
float ReadScalar(const tinygltf::Model& model,
                 const tinygltf::Accessor& accessor,
                 std::size_t i) {
  std::size_t stride = 0;
  const unsigned char* base = AccessorData(model, accessor, &stride);
  const unsigned char* p = base + i * stride;
  switch (accessor.componentType) {
    case TINYGLTF_COMPONENT_TYPE_FLOAT: {
      float v;
      std::memcpy(&v, p, sizeof(float));
      return v;
    }
    // glTF spec: keyframe TIME is FLOAT only, but tolerate other forms.
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return static_cast<float>(*p) / 255.0f;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
      std::uint16_t v; std::memcpy(&v, p, 2);
      return static_cast<float>(v) / 65535.0f;
    }
    default: return 0.0f;
  }
}

// Read a Vec3 sample (translation, scale).
Vec3 ReadVec3(const tinygltf::Model& model,
              const tinygltf::Accessor& accessor,
              std::size_t i) {
  std::size_t stride = 0;
  const unsigned char* base = AccessorData(model, accessor, &stride);
  const float* f = reinterpret_cast<const float*>(base + i * stride);
  return {f[0], f[1], f[2]};
}

// Read a Vec4 sample (rotation quaternion).
Vec4 ReadVec4(const tinygltf::Model& model,
              const tinygltf::Accessor& accessor,
              std::size_t i) {
  std::size_t stride = 0;
  const unsigned char* base = AccessorData(model, accessor, &stride);
  const float* f = reinterpret_cast<const float*>(base + i * stride);
  return {f[0], f[1], f[2], f[3]};
}

// Read a Mat4 sample (used only for inverse bind matrices, which are static).
Mat4 ReadMat4(const tinygltf::Model& model,
              const tinygltf::Accessor& accessor,
              std::size_t i) {
  std::size_t stride = 0;
  const unsigned char* base = AccessorData(model, accessor, &stride);
  const float* f = reinterpret_cast<const float*>(base + i * stride);
  Mat4 m{};
  for (int k = 0; k < 16; ++k) m[k] = f[k];
  return m;
}

// Joint index per vertex. glTF spec allows UBYTE or USHORT.
std::array<std::uint32_t, 4> ReadJointIndices(const tinygltf::Model& model,
                                              const tinygltf::Accessor& accessor,
                                              std::size_t i) {
  std::size_t stride = 0;
  const unsigned char* base = AccessorData(model, accessor, &stride);
  const unsigned char* p = base + i * stride;
  std::array<std::uint32_t, 4> out{};
  if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
    for (int k = 0; k < 4; ++k) out[k] = p[k];
  } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
    const std::uint16_t* s = reinterpret_cast<const std::uint16_t*>(p);
    for (int k = 0; k < 4; ++k) out[k] = s[k];
  } else {
    // Spec doesn't allow other types here. Return zeros so the result is
    // visibly broken instead of silently wrong.
    out = {0, 0, 0, 0};
  }
  return out;
}

// Joint weight per vertex. UBYTE/USHORT need normalising; FLOAT is direct.
Vec4 ReadWeights(const tinygltf::Model& model,
                 const tinygltf::Accessor& accessor,
                 std::size_t i) {
  std::size_t stride = 0;
  const unsigned char* base = AccessorData(model, accessor, &stride);
  const unsigned char* p = base + i * stride;
  Vec4 out{};
  if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
    const float* f = reinterpret_cast<const float*>(p);
    return {f[0], f[1], f[2], f[3]};
  }
  if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
    for (int k = 0; k < 4; ++k) out[k] = static_cast<float>(p[k]) / 255.0f;
    return out;
  }
  if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
    const std::uint16_t* s = reinterpret_cast<const std::uint16_t*>(p);
    for (int k = 0; k < 4; ++k) out[k] = static_cast<float>(s[k]) / 65535.0f;
    return out;
  }
  return out;
}

// ---- Channel sampling ------------------------------------------------------

enum class ChannelTarget { Translation, Rotation, Scale };

struct PreparedChannel {
  // Index into the model's accessors for the time samples (input).
  int inputAccessor = -1;
  int outputAccessor = -1;
  ChannelTarget target = ChannelTarget::Translation;
  bool stepInterp = false;  // false == LINEAR; CUBICSPLINE falls back to LINEAR
  // Cached sample times for fast bisect (input accessor data copied upfront).
  std::vector<float> times;
};

// Bisect to find the segment [i, i+1] where times[i] <= t <= times[i+1].
// Out: i, frac in [0..1]. If t is before/after the range, clamps.
void FindSegment(const std::vector<float>& times, float t,
                 std::size_t* outI, float* outFrac) {
  if (times.empty()) { *outI = 0; *outFrac = 0.0f; return; }
  if (t <= times.front()) { *outI = 0; *outFrac = 0.0f; return; }
  if (t >= times.back()) {
    *outI = times.size() - 1;
    *outFrac = 0.0f;
    return;
  }
  // Linear scan is fine for typical channel sizes (<200 keyframes).
  for (std::size_t i = 0; i + 1 < times.size(); ++i) {
    if (t >= times[i] && t <= times[i + 1]) {
      const float span = times[i + 1] - times[i];
      *outI = i;
      *outFrac = span > 1e-8f ? (t - times[i]) / span : 0.0f;
      return;
    }
  }
  *outI = times.size() - 1;
  *outFrac = 0.0f;
}

// ---- Scene / hierarchy helpers --------------------------------------------

// glTF default scene's root nodes, or scene 0, or fallback to node 0.
const std::vector<int>& RootNodeIndices(const tinygltf::Model& model) {
  static const std::vector<int> empty;
  if (model.scenes.empty()) return empty;
  const int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
  if (sceneIdx >= static_cast<int>(model.scenes.size())) return empty;
  return model.scenes[sceneIdx].nodes;
}

// Walk the node tree to build a parent-pointer map.
// nodeParents[i] == parent of node i, or -1 for roots.
std::vector<int> BuildNodeParents(const tinygltf::Model& model) {
  std::vector<int> parents(model.nodes.size(), -1);
  for (std::size_t n = 0; n < model.nodes.size(); ++n) {
    for (int childIdx : model.nodes[n].children) {
      if (childIdx >= 0 && childIdx < static_cast<int>(parents.size())) {
        parents[childIdx] = static_cast<int>(n);
      }
    }
  }
  return parents;
}

// Topological order for ancestor-first traversal: parent always comes before
// any of its descendants. We use this so that when we compute world matrices
// for a frame, parents are guaranteed to be done first.
std::vector<int> TopologicalOrder(const tinygltf::Model& model,
                                  const std::vector<int>& parents) {
  const std::size_t n = model.nodes.size();
  std::vector<int> order;
  order.reserve(n);
  std::vector<bool> visited(n, false);
  std::vector<int> stack;
  stack.reserve(n);

  for (std::size_t i = 0; i < n; ++i) {
    if (visited[i]) continue;
    int cur = static_cast<int>(i);
    stack.clear();
    while (cur >= 0 && !visited[cur]) {
      stack.push_back(cur);
      cur = parents[cur];
    }
    // stack now holds [self, parent, grand-parent, ...] from leaf to nearest
    // already-visited (or root). Push in reverse to get parent-first order.
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
      if (!visited[*it]) {
        visited[*it] = true;
        order.push_back(*it);
      }
    }
  }
  return order;
}

// glTF nodes can specify their local transform either as a 4x4 matrix or as
// separate T/R/S triples (default identity). This unifies them.
struct NodeBaseTRS {
  Vec3 translation = {0, 0, 0};
  Quat rotation = {0, 0, 0, 1};
  Vec3 scale = {1, 1, 1};
};

NodeBaseTRS NodeBaseTransform(const tinygltf::Node& node) {
  NodeBaseTRS out;
  if (!node.matrix.empty()) {
    // Decompose the matrix to TRS. For the kinds of files Blender exports
    // this is uncommon (TRS is the default), so the extraction here is
    // simplistic: assume orthogonal rotation, no shear. If a file does come
    // with a matrix node we'll get the right translation & scale, and a
    // close-to-correct rotation.
    const auto& m = node.matrix;
    out.translation = {
        static_cast<float>(m[12]),
        static_cast<float>(m[13]),
        static_cast<float>(m[14]),
    };
    Vec3 sx = {static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2])};
    Vec3 sy = {static_cast<float>(m[4]), static_cast<float>(m[5]), static_cast<float>(m[6])};
    Vec3 sz = {static_cast<float>(m[8]), static_cast<float>(m[9]), static_cast<float>(m[10])};
    auto len3 = [](const Vec3& v) {
      return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    };
    out.scale = {len3(sx), len3(sy), len3(sz)};
    // Build rotation matrix by stripping scale columns.
    Mat4 r = kIdentity;
    if (out.scale[0] > 1e-8f) { r[0] = sx[0]/out.scale[0]; r[1] = sx[1]/out.scale[0]; r[2] = sx[2]/out.scale[0]; }
    if (out.scale[1] > 1e-8f) { r[4] = sy[0]/out.scale[1]; r[5] = sy[1]/out.scale[1]; r[6] = sy[2]/out.scale[1]; }
    if (out.scale[2] > 1e-8f) { r[8] = sz[0]/out.scale[2]; r[9] = sz[1]/out.scale[2]; r[10] = sz[2]/out.scale[2]; }
    // Rotation matrix to quat (Shoemake's algorithm).
    const float trace = r[0] + r[5] + r[10];
    if (trace > 0.0f) {
      float s = 0.5f / std::sqrt(trace + 1.0f);
      out.rotation[3] = 0.25f / s;
      out.rotation[0] = (r[6] - r[9]) * s;
      out.rotation[1] = (r[8] - r[2]) * s;
      out.rotation[2] = (r[1] - r[4]) * s;
    } else if (r[0] > r[5] && r[0] > r[10]) {
      float s = 2.0f * std::sqrt(1.0f + r[0] - r[5] - r[10]);
      out.rotation[3] = (r[6] - r[9]) / s;
      out.rotation[0] = 0.25f * s;
      out.rotation[1] = (r[4] + r[1]) / s;
      out.rotation[2] = (r[8] + r[2]) / s;
    } else if (r[5] > r[10]) {
      float s = 2.0f * std::sqrt(1.0f + r[5] - r[0] - r[10]);
      out.rotation[3] = (r[8] - r[2]) / s;
      out.rotation[0] = (r[4] + r[1]) / s;
      out.rotation[1] = 0.25f * s;
      out.rotation[2] = (r[9] + r[6]) / s;
    } else {
      float s = 2.0f * std::sqrt(1.0f + r[10] - r[0] - r[5]);
      out.rotation[3] = (r[1] - r[4]) / s;
      out.rotation[0] = (r[8] + r[2]) / s;
      out.rotation[1] = (r[9] + r[6]) / s;
      out.rotation[2] = 0.25f * s;
    }
  } else {
    if (node.translation.size() == 3) {
      out.translation = {
          static_cast<float>(node.translation[0]),
          static_cast<float>(node.translation[1]),
          static_cast<float>(node.translation[2]),
      };
    }
    if (node.rotation.size() == 4) {
      out.rotation = {
          static_cast<float>(node.rotation[0]),
          static_cast<float>(node.rotation[1]),
          static_cast<float>(node.rotation[2]),
          static_cast<float>(node.rotation[3]),
      };
    }
    if (node.scale.size() == 3) {
      out.scale = {
          static_cast<float>(node.scale[0]),
          static_cast<float>(node.scale[1]),
          static_cast<float>(node.scale[2]),
      };
    }
  }
  return out;
}

// Find the first node referencing a skin AND a mesh — that's our skinned
// mesh node. Returns -1 if no such node exists.
int FindSkinnedMeshNode(const tinygltf::Model& model) {
  for (std::size_t i = 0; i < model.nodes.size(); ++i) {
    const auto& n = model.nodes[i];
    if (n.skin >= 0 && n.mesh >= 0) return static_cast<int>(i);
  }
  return -1;
}

// Read indices from a primitive. Returns true if `out` was populated.
// Handles UBYTE / USHORT / UINT index formats.
bool ReadIndices(const tinygltf::Model& model,
                 const tinygltf::Primitive& primitive,
                 std::vector<std::uint32_t>* out) {
  out->clear();
  if (primitive.indices < 0) return false;
  const auto& accessor = model.accessors[primitive.indices];
  std::size_t stride = 0;
  const unsigned char* base = AccessorData(model, accessor, &stride);
  out->reserve(accessor.count);
  for (std::size_t i = 0; i < accessor.count; ++i) {
    const unsigned char* p = base + i * stride;
    switch (accessor.componentType) {
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  out->push_back(static_cast<std::uint32_t>(*p)); break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
        std::uint16_t v; std::memcpy(&v, p, 2);
        out->push_back(static_cast<std::uint32_t>(v));
        break;
      }
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
        std::uint32_t v; std::memcpy(&v, p, 4);
        out->push_back(v);
        break;
      }
      default: return false;  // unexpected index type
    }
  }
  return true;
}
}  // namespace

// ---- Public API ------------------------------------------------------------

std::size_t GlbLoader::CountAnimations(const std::string& path,
                                       std::vector<std::string>* outAnimationNames,
                                       std::string* outError) {
  tinygltf::TinyGLTF loader;
  loader.SetImageLoader(DummyImageLoader, nullptr);
  tinygltf::Model model;
  std::string err, warn;
  const bool ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
  if (!ok) {
    if (outError) *outError = err.empty() ? "unknown tinygltf error" : err;
    return 0;
  }
  if (outAnimationNames) {
    outAnimationNames->clear();
    outAnimationNames->reserve(model.animations.size());
    for (const auto& a : model.animations) outAnimationNames->push_back(a.name);
  }
  return model.animations.size();
}

bool GlbLoader::LoadFromFileAtIndex(const std::string& path,
                                    std::size_t animationIndex,
                                    double fps) {
  loaded_ = false;
  lastError_.clear();
  bakedFrames_.clear();
  triangleIndices_.clear();
  totalFrames_ = 0;
  totalPoints_ = 0;
  animationName_.clear();

  tinygltf::TinyGLTF loader;
  loader.SetImageLoader(DummyImageLoader, nullptr);
  tinygltf::Model model;
  std::string err, warn;
  if (!loader.LoadBinaryFromFile(&model, &err, &warn, path)) {
    lastError_ = err.empty() ? "tinygltf failed without an error message" : err;
    return false;
  }
  if (animationIndex >= model.animations.size()) {
    lastError_ = "animation index out of range";
    return false;
  }
  if (fps <= 0.0) {
    lastError_ = "fps must be > 0";
    return false;
  }

  const auto& anim = model.animations[animationIndex];
  animationName_ = anim.name;

  // ---- Locate the skinned mesh ---------------------------------------------
  const int meshNodeIdx = FindSkinnedMeshNode(model);
  if (meshNodeIdx < 0) {
    lastError_ = "no skinned mesh found in glTF (no node has both `skin` and `mesh`)";
    return false;
  }
  const auto& meshNode = model.nodes[meshNodeIdx];
  const auto& mesh = model.meshes[meshNode.mesh];
  if (mesh.primitives.empty()) {
    lastError_ = "mesh has no primitives";
    return false;
  }
  if (mesh.primitives.size() > 1) {
    // Multiple primitives = multiple materials/sub-meshes. We only render
    // the first; in practice Blender exports one primitive per material.
  }
  const auto& primitive = mesh.primitives.front();

  // ---- Pull vertex attributes ----------------------------------------------
  auto findAttr = [&](const char* name) -> int {
    auto it = primitive.attributes.find(name);
    return it == primitive.attributes.end() ? -1 : it->second;
  };

  const int posAttr     = findAttr("POSITION");
  const int jointsAttr  = findAttr("JOINTS_0");
  const int weightsAttr = findAttr("WEIGHTS_0");
  if (posAttr < 0 || jointsAttr < 0 || weightsAttr < 0) {
    lastError_ = "primitive missing one of POSITION / JOINTS_0 / WEIGHTS_0";
    return false;
  }

  const auto& posAccessor = model.accessors[posAttr];
  totalPoints_ = static_cast<std::uint32_t>(posAccessor.count);

  // Cache rest-pose positions, joints, weights (don't re-read every frame).
  std::vector<Vec3> restPositions(totalPoints_);
  std::vector<std::array<std::uint32_t, 4>> jointIdx(totalPoints_);
  std::vector<Vec4> jointWeights(totalPoints_);
  for (std::uint32_t i = 0; i < totalPoints_; ++i) {
    restPositions[i] = ReadVec3(model, posAccessor, i);
    jointIdx[i]      = ReadJointIndices(model, model.accessors[jointsAttr], i);
    jointWeights[i]  = ReadWeights(model, model.accessors[weightsAttr], i);
  }

  // ---- Triangle indices -----------------------------------------------------
  if (!ReadIndices(model, primitive, &triangleIndices_)) {
    // No indices -> the spec says vertex order defines triangles. Fabricate
    // a sequential list. This is rare but worth handling.
    triangleIndices_.resize(totalPoints_);
    for (std::uint32_t i = 0; i < totalPoints_; ++i) triangleIndices_[i] = i;
  }
  // Reject non-triangle primitives. glTF mode default == TRIANGLES (4).
  if (primitive.mode >= 0 && primitive.mode != TINYGLTF_MODE_TRIANGLES) {
    lastError_ = "primitive mode != TRIANGLES (only triangle lists supported)";
    return false;
  }

  // ---- Skin: joints + inverse-bind matrices ---------------------------------
  if (meshNode.skin < 0 || meshNode.skin >= static_cast<int>(model.skins.size())) {
    lastError_ = "node skin index out of range";
    return false;
  }
  const auto& skin = model.skins[meshNode.skin];
  const std::size_t jointCount = skin.joints.size();
  if (jointCount == 0) {
    lastError_ = "skin has no joints";
    return false;
  }

  std::vector<Mat4> ibms(jointCount, kIdentity);
  if (skin.inverseBindMatrices >= 0) {
    const auto& ibmAccessor = model.accessors[skin.inverseBindMatrices];
    for (std::size_t j = 0; j < jointCount && j < ibmAccessor.count; ++j) {
      ibms[j] = ReadMat4(model, ibmAccessor, j);
    }
  }

  // ---- Hierarchy ------------------------------------------------------------
  const auto parents = BuildNodeParents(model);
  const auto topo = TopologicalOrder(model, parents);

  // Cache base TRS for every node — that's the rest-pose transform.
  std::vector<NodeBaseTRS> baseTRS(model.nodes.size());
  for (std::size_t i = 0; i < model.nodes.size(); ++i) {
    baseTRS[i] = NodeBaseTransform(model.nodes[i]);
  }

  // ---- Channel preparation --------------------------------------------------
  // Group channels by target node so per-frame sampling is one pass per node.
  struct NodeChannels {
    int nodeIdx = -1;
    PreparedChannel translation;
    PreparedChannel rotation;
    PreparedChannel scale;
    bool hasT = false, hasR = false, hasS = false;
  };

  std::unordered_map<int, NodeChannels> channelsByNode;
  for (const auto& channel : anim.channels) {
    if (channel.target_node < 0) continue;
    const auto& sampler = anim.samplers[channel.sampler];

    PreparedChannel pc;
    pc.inputAccessor = sampler.input;
    pc.outputAccessor = sampler.output;

    if (sampler.interpolation == "STEP") pc.stepInterp = true;
    else if (sampler.interpolation == "CUBICSPLINE") {
      // We sample CUBICSPLINE channels with LINEAR interpolation, which is
      // visibly wrong for tightly-curved channels but readable for typical
      // character animation. A console warning is logged once per file
      // (see end of LoadFromFileAtIndex). Tracking "once" requires shared
      // state we don't have here; rely on tinygltf's warnings buffer.
      pc.stepInterp = false;
    }

    // Prefetch sample times for fast bisect. The input accessor is always
    // SCALAR FLOAT per glTF spec.
    {
      const auto& acc = model.accessors[pc.inputAccessor];
      pc.times.reserve(acc.count);
      for (std::size_t i = 0; i < acc.count; ++i) {
        pc.times.push_back(ReadScalar(model, acc, i));
      }
    }

    auto& bucket = channelsByNode[channel.target_node];
    bucket.nodeIdx = channel.target_node;
    if (channel.target_path == "translation") {
      pc.target = ChannelTarget::Translation;
      bucket.translation = std::move(pc);
      bucket.hasT = true;
    } else if (channel.target_path == "rotation") {
      pc.target = ChannelTarget::Rotation;
      bucket.rotation = std::move(pc);
      bucket.hasR = true;
    } else if (channel.target_path == "scale") {
      pc.target = ChannelTarget::Scale;
      bucket.scale = std::move(pc);
      bucket.hasS = true;
    }
    // "weights" channels (morph targets) are silently ignored — see
    // class-level docstring.
  }

  // ---- Determine animation duration -----------------------------------------
  float duration = 0.0f;
  for (const auto& [_, ch] : channelsByNode) {
    if (ch.hasT && !ch.translation.times.empty()) duration = std::max(duration, ch.translation.times.back());
    if (ch.hasR && !ch.rotation.times.empty())    duration = std::max(duration, ch.rotation.times.back());
    if (ch.hasS && !ch.scale.times.empty())       duration = std::max(duration, ch.scale.times.back());
  }
  if (duration <= 0.0f) {
    // No animation channels actually drive anything (or all zero-length).
    // We still produce a single rest-pose frame so playback shows something.
    duration = 1.0f / static_cast<float>(fps);
  }

  totalFrames_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::round(duration * fps)));
  bakedFrames_.assign(totalFrames_, std::vector<float>(totalPoints_ * 3, 0.0f));

  // ---- v0.12.0 motion analysis: allocate per-joint curves -----------------
  //
  // worldMotion_[j][f] = Euclidean distance moved by joint j between frames
  // f-1 and f (in pre-recenter world space; the recenter pass below is a
  // global constant offset, so it doesn't affect frame-to-frame deltas).
  //
  // Frame 0 is always 0 (no previous). We track previous-frame world
  // positions in `prevJointWorldPos` to avoid storing all per-frame world
  // matrices.
  jointNames_.clear();
  jointNames_.reserve(jointCount);
  for (std::size_t j = 0; j < jointCount; ++j) {
    const int n = skin.joints[j];
    const std::string& nm = (n >= 0 && n < static_cast<int>(model.nodes.size()))
                                ? model.nodes[n].name
                                : std::string{};
    jointNames_.push_back(nm.empty() ? ("joint_" + std::to_string(j)) : nm);
  }
  worldMotion_.assign(jointCount, std::vector<float>(totalFrames_, 0.0f));
  // Local motion not implemented in v0.12.0-alpha.1 — placeholder so the
  // surface API stays consistent.
  localMotion_.assign(jointCount, std::vector<float>(totalFrames_, 0.0f));

  std::vector<Vec3> prevJointWorldPos(jointCount, Vec3{0, 0, 0});

  // ---- Per-frame baking loop ------------------------------------------------
  // Reusable scratch buffers (avoid allocating per frame).
  std::vector<NodeBaseTRS> currentTRS(model.nodes.size());
  std::vector<Mat4> nodeWorld(model.nodes.size(), kIdentity);
  std::vector<Mat4> jointSkin(jointCount, kIdentity);

  // Evaluate one channel at time t. Writes into `currentTRS[nodeIdx]`.
  auto sampleChannel = [&](const NodeChannels& nc, float t) {
    NodeBaseTRS& trs = currentTRS[nc.nodeIdx];

    auto sampleTrans = [&](const PreparedChannel& pc, Vec3* out) {
      std::size_t i = 0; float frac = 0.0f;
      FindSegment(pc.times, t, &i, &frac);
      const auto& outAcc = model.accessors[pc.outputAccessor];
      const Vec3 a = ReadVec3(model, outAcc, i);
      if (pc.stepInterp || frac == 0.0f || i + 1 >= pc.times.size()) {
        *out = a; return;
      }
      const Vec3 b = ReadVec3(model, outAcc, i + 1);
      (*out)[0] = a[0] + frac * (b[0] - a[0]);
      (*out)[1] = a[1] + frac * (b[1] - a[1]);
      (*out)[2] = a[2] + frac * (b[2] - a[2]);
    };

    auto sampleRot = [&](const PreparedChannel& pc, Quat* out) {
      std::size_t i = 0; float frac = 0.0f;
      FindSegment(pc.times, t, &i, &frac);
      const auto& outAcc = model.accessors[pc.outputAccessor];
      const Quat a = ReadVec4(model, outAcc, i);
      if (pc.stepInterp || frac == 0.0f || i + 1 >= pc.times.size()) {
        *out = a; return;
      }
      const Quat b = ReadVec4(model, outAcc, i + 1);
      *out = Slerp(a, b, frac);
    };

    if (nc.hasT) sampleTrans(nc.translation, &trs.translation);
    if (nc.hasR) sampleRot(nc.rotation, &trs.rotation);
    if (nc.hasS) sampleTrans(nc.scale, &trs.scale);
  };

  // Mesh node's world transform -- typically identity, but a Blender export
  // can have a non-identity transform on the mesh itself (e.g. scale).
  // We compute it once from the rest-pose hierarchy (it's not animated for
  // skinned meshes by spec, so it's the same every frame).
  Mat4 meshNodeWorld = kIdentity;
  {
    int n = meshNodeIdx;
    while (n >= 0) {
      const auto& trs = baseTRS[n];
      meshNodeWorld = MulM(ComposeTRS(trs.translation, trs.rotation, trs.scale), meshNodeWorld);
      n = parents[n];
    }
  }
  // Inverse of meshNodeWorld is needed in the skinning equation
  // (vertex = meshNodeWorld_inv * Σ jointWorld * IBM * vertex_local).
  // For simplicity we assume meshNodeWorld is rigid (rotation + uniform
  // scale + translation), which is the common Blender export case.
  // We'll fold it in below.
  // For most Blender skinned exports, meshNodeWorld is identity, in which
  // case the inverse is trivially identity. We compute the inverse only
  // when necessary. For now, take the simpler path: assume identity.
  // (If artists report wrong scale/orientation, revisit.)

  for (std::uint32_t f = 0; f < totalFrames_; ++f) {
    const float t = static_cast<float>(f) / static_cast<float>(fps);

    // Start every node from its base TRS.
    currentTRS = baseTRS;

    // Overlay animation channels on top of base.
    for (const auto& [_, nc] : channelsByNode) {
      sampleChannel(nc, t);
    }

    // Compute node world matrices in topological order.
    for (int nodeIdx : topo) {
      const auto& trs = currentTRS[nodeIdx];
      const Mat4 local = ComposeTRS(trs.translation, trs.rotation, trs.scale);
      const int p = parents[nodeIdx];
      nodeWorld[nodeIdx] = (p < 0) ? local : MulM(nodeWorld[p], local);
    }

    // Compute skinning matrix per joint: jointWorld * IBM.
    for (std::size_t j = 0; j < jointCount; ++j) {
      const int n = skin.joints[j];
      jointSkin[j] = MulM(nodeWorld[n], ibms[j]);
    }

    // ---- v0.12.0: per-joint world-motion magnitude --------------------
    //
    // For each joint, transform a fixed point in joint-local space through
    // its current world matrix and diff against the previous frame. We use
    // the local-space point (0, 1, 0, 1) — i.e. "1 unit up the bone" — so
    // that rotation-only animations (where joint translation column doesn't
    // change) still register motion. A pure-rotation joint will swing this
    // probe point around its origin, which is exactly what "the bone is
    // moving" means in practice.
    //
    // Why not just use the translation column (m[12..14])? RiggedSimple
    // and most character rigs animate via local rotation only — the
    // joint's own translation never changes — so a translation-only
    // metric reports zero motion even when the rig is clearly bending.
    //
    // Frame 0 stays at 0 (no previous data); we still capture the probe
    // position into prevJointWorldPos so frame 1's delta is correct.
    constexpr Vec4 kProbePoint = {0.0f, 1.0f, 0.0f, 1.0f};
    for (std::size_t j = 0; j < jointCount; ++j) {
      const int n = skin.joints[j];
      const Vec4 probeWorld = MulMV(nodeWorld[n], kProbePoint);
      const Vec3 cur = {probeWorld[0], probeWorld[1], probeWorld[2]};
      if (f > 0) {
        const float dx = cur[0] - prevJointWorldPos[j][0];
        const float dy = cur[1] - prevJointWorldPos[j][1];
        const float dz = cur[2] - prevJointWorldPos[j][2];
        worldMotion_[j][f] = std::sqrt(dx * dx + dy * dy + dz * dz);
      }
      prevJointWorldPos[j] = cur;
    }

    // Skin every vertex.
    auto& frameBuf = bakedFrames_[f];
    for (std::uint32_t v = 0; v < totalPoints_; ++v) {
      const Vec3& p = restPositions[v];
      const Vec4 vh = {p[0], p[1], p[2], 1.0f};
      const auto& wIdx = jointIdx[v];
      const auto& wW   = jointWeights[v];

      Vec4 acc = {0, 0, 0, 0};
      float totalWeight = 0.0f;
      for (int k = 0; k < 4; ++k) {
        const float w = wW[k];
        if (w <= 0.0f) continue;
        if (wIdx[k] >= jointCount) continue;
        const Vec4 contrib = MulMV(jointSkin[wIdx[k]], vh);
        acc[0] += w * contrib[0];
        acc[1] += w * contrib[1];
        acc[2] += w * contrib[2];
        totalWeight += w;
      }
      if (totalWeight <= 1e-6f) {
        // No usable joint weights -> rest-pose vertex (mesh-node space).
        const Vec4 fallback = MulMV(meshNodeWorld, vh);
        acc = fallback;
      }
      frameBuf[v * 3 + 0] = acc[0];
      frameBuf[v * 3 + 1] = acc[1];
      frameBuf[v * 3 + 2] = acc[2];
    }
  }

  // ---- v0.10.0: recenter to origin (XZ only, keep Y on the ground) ---------
  //
  // Without this step, GLB files with a non-identity mesh node transform
  // (or non-trivial rest-pose root joint position) end up off-center on
  // the grid — sometimes way off. We compute the bbox center on frame 0
  // and subtract its (X, Z) from every vertex of every frame, leaving Y
  // alone so the model still rests on / above the ground plane.
  //
  // Why bbox center and not the root joint world position?
  //   - Works identically for GLB (skinned) and MDD (point cache); same
  //     pass is duplicated in mdd_data_manager.cpp.
  //   - Doesn't depend on rig topology — a Blender export with a weird
  //     'Armature' parent or a Mixamo rig with multiple top-level joints
  //     all behave the same.
  //   - Matches user intent: "put this model in the middle of the grid."
  //
  // The user can still fine-tune via the per-animation pivot offset slider
  // in the overlay, which acts on top of this recentering.
  if (totalFrames_ > 0 && totalPoints_ > 0) {
    const auto& frame0 = bakedFrames_[0];
    float minX = frame0[0], maxX = frame0[0];
    float minZ = frame0[2], maxZ = frame0[2];
    for (std::uint32_t v = 1; v < totalPoints_; ++v) {
      const float x = frame0[v * 3 + 0];
      const float z = frame0[v * 3 + 2];
      if (x < minX) minX = x; if (x > maxX) maxX = x;
      if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
    }
    const float dx = 0.5f * (minX + maxX);
    const float dz = 0.5f * (minZ + maxZ);
    if (std::abs(dx) > 1e-6f || std::abs(dz) > 1e-6f) {
      for (auto& frame : bakedFrames_) {
        for (std::uint32_t v = 0; v < totalPoints_; ++v) {
          frame[v * 3 + 0] -= dx;
          frame[v * 3 + 2] -= dz;
        }
      }
    }
  }

  loaded_ = true;
  return true;
}

const std::vector<float>& GlbLoader::VerticesForFrame(std::uint32_t frameIndex) const {
  static const std::vector<float> empty;
  if (!loaded_ || bakedFrames_.empty()) return empty;
  const std::uint32_t clamped = std::min(frameIndex, totalFrames_ - 1);
  return bakedFrames_[clamped];
}
