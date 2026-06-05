#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Loads a single skinned-mesh animation out of a glTF binary (.glb) file
// and bakes it into per-frame vertex positions in the same layout as
// MDDDataManager (3 floats per vertex per frame). The render pipeline is
// agnostic to where vertices come from, so a fully-baked GlbLoader is a
// drop-in source.
//
// Scope:
//   - Skinning: linear-blend (LBS) on CPU, baked at load time.
//   - Interpolation: LINEAR + STEP. CUBICSPLINE channels are sampled with
//     LINEAR fallback and a console warning.
//   - Mesh: the first mesh referenced by a skinned node, first primitive.
//   - Animations: ONE animation per loaded GlbLoader. If the file contains
//     multiple animations, the caller (AnimationLibrary) creates one
//     LoadedAnimation per animation and points each one at a separate
//     GlbLoader instance — see `LoadFromFileAtIndex`.
//   - No morph targets, no static-mesh-only fallback files yet (those will
//     come later when we widen the format support).
//
// All vertex data is in glTF's coordinate space (Y-up, right-handed),
// which matches what HoloRoll's viewport expects.
class GlbLoader {
 public:
  // Discover how many animations live in `path`. Returns 0 if the file
  // can't be opened/parsed, or if it contains no animations. Does NOT
  // perform skinning — call this first to know how many GlbLoader
  // instances to create.
  //
  // outAnimationNames (optional): on success, filled with the glTF
  // animation `name` field (or "" for unnamed channels), one per index.
  static std::size_t CountAnimations(
      const std::string& path,
      std::vector<std::string>* outAnimationNames = nullptr,
      std::string* outError = nullptr);

  // Load + bake animation #animationIndex from the file. fps controls
  // the temporal sampling rate (same fps that drives MDD playback).
  // Returns true on success. Errors land in LastError().
  //
  // After success: IsLoaded() == true, TotalFrames() / TotalPoints()
  // are valid, VerticesForFrame() returns baked positions, and
  // TriangleIndices() returns the mesh's triangle list.
  bool LoadFromFileAtIndex(const std::string& path,
                           std::size_t animationIndex,
                           double fps);

  bool IsLoaded() const { return loaded_; }
  const std::string& LastError() const { return lastError_; }

  std::uint32_t TotalFrames() const { return totalFrames_; }
  std::uint32_t TotalPoints() const { return totalPoints_; }

  // Per-frame baked positions, layout: [x0,y0,z0, x1,y1,z1, ...].
  // Length per frame == TotalPoints() * 3.
  const std::vector<float>& VerticesForFrame(std::uint32_t frameIndex) const;

  // Triangle index buffer (uint32, 3 indices per triangle). Same buffer
  // for every frame.
  const std::vector<std::uint32_t>& TriangleIndices() const { return triangleIndices_; }

  // Source-of-truth animation name, e.g. "walk" or "idle". Empty if the
  // glTF channel had no `name` field.
  const std::string& AnimationName() const { return animationName_; }

  // ---- v0.12.0 motion analysis -----------------------------------------
  //
  // Per-bone motion data computed during the bake pass. Same semantics as
  // documented in animation_library.h's LoadedAnimation — see there.
  //
  // jointNames.size() == localMotion.size() == worldMotion.size() == jointCount.
  // Each inner vector has length TotalFrames().
  const std::vector<std::string>& JointNames() const { return jointNames_; }
  const std::vector<std::vector<float>>& LocalMotion() const { return localMotion_; }
  const std::vector<std::vector<float>>& WorldMotion() const { return worldMotion_; }

  // ---- v0.16.0-alpha.1 camera attach -----------------------------------
  //
  // Per-joint per-frame world matrix in glTF model space. Captured during
  // the bake pass alongside motion data (it's the same nodeWorld[j][f]
  // intermediate that drives skinning). Used by the camera-attach feature
  // to compute the camera basis from a chosen bone — see gl_viewport.cpp.
  //
  // Layout: jointWorldMatrices_[boneIdx][frameIdx] is a column-major 4x4
  // float matrix. boneIdx range is [0, JointNames().size()), frameIdx
  // range is [0, TotalFrames()).
  //
  // Memory cost: ~64 bytes per (bone, frame). For a typical 50-bone,
  // 1000-frame rig that's ~3 MB per animation — fits comfortably.
  using BoneMatrix = std::array<float, 16>;
  const std::vector<std::vector<BoneMatrix>>& JointWorldMatrices() const {
    return jointWorldMatrices_;
  }

 private:
  bool loaded_ = false;
  std::string lastError_;
  std::string animationName_;

  std::uint32_t totalFrames_ = 0;
  std::uint32_t totalPoints_ = 0;

  // bakedFrames_[frameIndex] is a flat list of 3 floats per vertex.
  std::vector<std::vector<float>> bakedFrames_;

  // Triangle indices into the per-frame vertex buffer. Stable across frames.
  std::vector<std::uint32_t> triangleIndices_;

  // v0.12.0: motion curves, populated alongside bakedFrames_.
  std::vector<std::string> jointNames_;
  std::vector<std::vector<float>> localMotion_;
  std::vector<std::vector<float>> worldMotion_;

  // v0.16.0-alpha.1: per-joint per-frame world matrix, captured during
  // bake alongside the motion analysis. Indexed [boneIdx][frameIdx].
  std::vector<std::vector<BoneMatrix>> jointWorldMatrices_;
};
