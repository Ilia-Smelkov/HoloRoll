#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/glb_loader.h"
#include "core/mdd_data_manager.h"
#include "core/obj_index_loader.h"

// Single loaded animation. Source can be either an MDD/OBJ pair OR a single
// GLB file (with skeletal animation baked into per-frame vertex positions).
// Exactly one of `mdd` or `glb` is non-null after a successful load.
struct LoadedAnimation {
  std::string basename;       // What appears in the region name.
  std::string sourcePath;     // Path of the loaded file (.mdd or .glb).
  std::string objPath;        // Only set when source is MDD with a paired OBJ.

  // MDD path retained as a separate field for backward compatibility with
  // any code (or future tools) that need it specifically.
  std::string mddPath;

  std::unique_ptr<MDDDataManager> mdd;
  std::unique_ptr<ObjIndexLoader> obj;
  std::unique_ptr<GlbLoader> glb;

  // Bounding-box centre of the rest pose (frame 0). Used as the default
  // pivot for object rotation and camera orbit.
  float autoPivot[3] = {0.0f, 0.0f, 0.0f};

  // Max bbox extent of frame 0. Used to scale the rotation gizmo and the
  // default camera distance.
  float autoExtent = 1.0f;

  // Per-triangle face normals computed from the rest pose (frame 0).
  std::vector<float> restNormals;

  // ---- v0.12.0 motion analysis (alpha.8 semantics) ----------------------
  //
  // Per-bone motion curves computed at load time. SIGNED projection of
  // probe-tip position onto each joint's principal motion axis (max
  // deviation from trajectory mean) — see glb_loader.cpp's
  // projectSigned post-process. Two metrics:
  //
  //   localMotion[j][f]  - signed projection of bone j's probe in
  //                        PARENT space. Captures "this bone actually
  //                        rotated/translated on its own": a child of
  //                        a rotating parent has localMotion ≈ 0 even
  //                        if its world position is changing. Use to
  //                        find first-mover bones (handle vs body).
  //
  //   worldMotion[j][f]  - signed projection of bone j's probe in
  //                        WORLD space. Captures "how far the bone
  //                        actually moved in the scene": a foot with
  //                        fixed local rotation but swinging hip
  //                        parent has non-zero worldMotion. Use to
  //                        find biggest-amplitude motion for sound
  //                        timing.
  //
  // 0 ≈ rest pose (mean of trajectory); positive/negative = displacement
  // along the principal axis in either direction. Pre-alpha.8 these
  // were rectified |speed| (always >= 0), but that produced 2x-frequency
  // envelopes for sinusoidal motion (two bumps per swing); the signed
  // form matches the visual frequency of the underlying motion.
  //
  // Both curves are empty for MDD animations (no skeleton). Joint name
  // comes from skin.joints[j].name in the glTF; falls back to
  // "joint_<idx>" if unnamed.
  std::vector<std::string> jointNames;
  std::vector<std::vector<float>> localMotion;
  std::vector<std::vector<float>> worldMotion;

  // ---- v0.16.0-alpha.1 camera attach ------------------------------------
  //
  // Per-joint per-frame world matrix in glTF model space. Layout:
  // jointWorldMatrices[boneIdx][frameIdx] — column-major 4x4 float.
  // Populated from GlbLoader::JointWorldMatrices() at scan time. Empty
  // for MDD animations (no skeleton). Used by gl_viewport's Attached
  // camera mode to compute camera position + look direction from a
  // chosen bone's transform at the current preview frame.
  using BoneMatrix = std::array<float, 16>;
  std::vector<std::vector<BoneMatrix>> jointWorldMatrices;

  // v0.16.0-alpha.2: parent joint indices for skeleton visualisation.
  // jointParents[j] = -1 means no parent (skeleton root); otherwise
  // an index into jointNames / jointWorldMatrices. Used by gl_viewport
  // to render bone-to-parent line segments.
  std::vector<int> jointParents;

  double DurationSeconds(double fps) const;

  // Source-agnostic accessors. Always prefer these over poking at
  // `mdd` / `glb` directly.
  std::uint32_t TotalFrames() const;
  std::uint32_t TotalPoints() const;
  const std::vector<float>& VerticesForFrame(std::uint32_t frame) const;
  const std::vector<std::uint32_t>* TriangleIndicesPtr() const;
  bool HasTopology() const;

  // ---- v0.16.0-alpha.1 camera helpers -----------------------------------
  //
  // FindBoneByName: case-sensitive linear search over jointNames.
  // Returns std::nullopt if no bone matches.
  std::optional<std::size_t> FindBoneByName(const std::string& name) const;

  // GetBoneWorldMatrix: copy bone's world matrix at the given frame into
  // out[16]. Returns false (and leaves out untouched) if boneIdx is
  // out of range or jointWorldMatrices is empty (MDD animation). Frame
  // is clamped to [0, TotalFrames-1] so callers don't need to.
  bool GetBoneWorldMatrix(std::size_t boneIdx, std::uint32_t frame,
                           float out[16]) const;
};

struct TimelineRegion {
  std::size_t animationIndex = 0;
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  std::string regionName;
};

class AnimationLibrary {
 public:
  std::size_t ScanFolder(const std::string& directory, double fps, std::string* logOut = nullptr);

  void Clear();

  std::size_t Count() const { return animations_.size(); }
  const LoadedAnimation& At(std::size_t i) const { return *animations_[i]; }

  std::size_t FindAnimationIndexByBasename(const std::string& basename) const;

  // Resolve a REAPER item/region label back to an animation index. Tries
  // direct basename match first; if that fails, strips a trailing "_<digits>"
  // variation suffix and tries again. Returns std::numeric_limits<size_t>::max()
  // if neither attempt finds an animation.
  //
  // Example: with library entries ["frog_jump", "enemy_hit"]
  //   "frog_jump"   -> direct hit, returns frog_jump.
  //   "frog_jump_2" -> no direct hit, strip _2 -> frog_jump, returns frog_jump.
  //   "frog_jump_alt" -> no direct hit, no numeric suffix to strip, missing.
  //   With library entries ["frog_jump", "frog_jump_2"]:
  //   "frog_jump_2" -> direct hit, returns frog_jump_2 (real anim, not variation).
  std::size_t ResolveAnimationByItemName(const std::string& itemName) const;

  const std::string& Directory() const { return directory_; }

  void BuildRegions(double fps, double gapSeconds, double startSeconds = 0.0);

  const std::vector<TimelineRegion>& Regions() const { return regions_; }
  double TotalSpanSeconds() const;

  bool ResolvePlayhead(double playheadSeconds,
                       double fps,
                       const std::vector<TimelineRegion>& liveRegions,
                       std::size_t* outAnimationIndex,
                       std::uint32_t* outFrameIndex) const;

  static constexpr int kRegionColorRgb = 0x7F4FBF;
  static int RegionColorReaper() {
    return 0x01000000 | kRegionColorRgb;
  }

  // Region naming.
  //
  // Historically all region names were prefixed with "MDD: " (legacy). As
  // of v0.4.0 the default prefix is empty; users who want it back can set
  // `region_name_prefix=` in the INI config.
  //
  // Prefix is stored as runtime state (static) so that AnimationLibrary
  // and entry.cpp share a single source of truth without piping it through
  // every function call. ScanFolder/BuildRegions read the current value;
  // entry.cpp sets it from config on startup and on Reload config.
  //
  // The legacy "MDD: " prefix is recognised on read regardless — see
  // StripLegacyAndCurrentPrefix() — so old REAPER projects with
  // `MDD: foo` regions still match basenames after this change.
  static const char* kLegacyRegionNamePrefix;  // "MDD: "
  static void SetRegionNamePrefix(const std::string& prefix);
  static const std::string& RegionNamePrefix();

  // Strip either the current configured prefix or the legacy "MDD: "
  // prefix from a region name, whichever matches first. Used by entry.cpp
  // to resolve REAPER region names back to animation basenames.
  static std::string StripPrefix(const std::string& regionName);

 private:
  std::string directory_;
  std::vector<std::unique_ptr<LoadedAnimation>> animations_;
  std::vector<TimelineRegion> regions_;

  static std::string s_regionNamePrefix_;
};
