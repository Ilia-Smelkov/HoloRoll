#include "core/animation_library.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <windows.h>

namespace {
std::string LowerExt(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot);
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

std::string Stem(const std::string& fileName) {
  const auto slash = fileName.find_last_of("\\/");
  const std::string base = slash == std::string::npos ? fileName : fileName.substr(slash + 1);
  const auto dot = base.find_last_of('.');
  return dot == std::string::npos ? base : base.substr(0, dot);
}

bool FileExists(const std::string& path) {
  const DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void Append(std::string* log, const std::string& line) {
  if (log) {
    log->append(line);
    log->push_back('\n');
  }
}

void ComputeRestPoseBoundingBox(LoadedAnimation& anim) {
  const auto& frame = anim.VerticesForFrame(0);
  if (frame.empty()) return;

  float minX = frame[0], minY = frame[1], minZ = frame[2];
  float maxX = minX, maxY = minY, maxZ = minZ;
  for (std::size_t i = 0; i + 2 < frame.size(); i += 3) {
    const float x = frame[i + 0];
    const float y = frame[i + 1];
    const float z = frame[i + 2];
    if (x < minX) minX = x; else if (x > maxX) maxX = x;
    if (y < minY) minY = y; else if (y > maxY) maxY = y;
    if (z < minZ) minZ = z; else if (z > maxZ) maxZ = z;
  }
  anim.autoPivot[0] = 0.5f * (minX + maxX);
  anim.autoPivot[1] = 0.5f * (minY + maxY);
  anim.autoPivot[2] = 0.5f * (minZ + maxZ);

  const float ex = maxX - minX;
  const float ey = maxY - minY;
  const float ez = maxZ - minZ;
  anim.autoExtent = std::max({ex, ey, ez, 0.05f});
}

// Compute a face normal per triangle of the OBJ, evaluated against the rest
// pose (frame 0) of the MDD. Layout: 3 floats per triangle, in the same order
// as the index buffer iterates by groups of 3.
void ComputeRestNormals(LoadedAnimation& anim) {
  anim.restNormals.clear();
  const auto& positions = anim.VerticesForFrame(0);
  if (positions.empty()) return;
  const auto* indicesPtr = anim.TriangleIndicesPtr();
  if (!indicesPtr || indicesPtr->size() < 3) return;
  const auto& indices = *indicesPtr;
  const std::uint32_t pointCount = anim.TotalPoints();

  const std::size_t triangleCount = indices.size() / 3;
  anim.restNormals.resize(triangleCount * 3);

  for (std::size_t t = 0; t < triangleCount; ++t) {
    const std::uint32_t i0 = indices[t * 3 + 0];
    const std::uint32_t i1 = indices[t * 3 + 1];
    const std::uint32_t i2 = indices[t * 3 + 2];

    if (i0 >= pointCount || i1 >= pointCount || i2 >= pointCount) {
      anim.restNormals[t * 3 + 0] = 0.0f;
      anim.restNormals[t * 3 + 1] = 1.0f;
      anim.restNormals[t * 3 + 2] = 0.0f;
      continue;
    }

    const float ax = positions[i0 * 3 + 0];
    const float ay = positions[i0 * 3 + 1];
    const float az = positions[i0 * 3 + 2];
    const float bx = positions[i1 * 3 + 0];
    const float by = positions[i1 * 3 + 1];
    const float bz = positions[i1 * 3 + 2];
    const float cx = positions[i2 * 3 + 0];
    const float cy = positions[i2 * 3 + 1];
    const float cz = positions[i2 * 3 + 2];

    const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
    const float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;

    float nx = e1y * e2z - e1z * e2y;
    float ny = e1z * e2x - e1x * e2z;
    float nz = e1x * e2y - e1y * e2x;

    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
      nx /= len; ny /= len; nz /= len;
    } else {
      nx = 0.0f; ny = 1.0f; nz = 0.0f;
    }
    anim.restNormals[t * 3 + 0] = nx;
    anim.restNormals[t * 3 + 1] = ny;
    anim.restNormals[t * 3 + 2] = nz;
  }
}

// Copy motion analysis data (joint names + per-frame world/local motion
// magnitudes) from the loaded GlbLoader into the LoadedAnimation. MDD
// animations have no skeleton, so this is a no-op for them.
void CopyMotionDataFromGlb(LoadedAnimation& anim) {
  if (!anim.glb || !anim.glb->IsLoaded()) return;
  anim.jointNames = anim.glb->JointNames();
  anim.worldMotion = anim.glb->WorldMotion();
  anim.localMotion = anim.glb->LocalMotion();
}

// Format a single-line summary of the top-N most-active joints by total
// motion across the animation. `motion` is one of `anim.worldMotion` or
// `anim.localMotion` — the function is agnostic to which metric it ranks.
// Returns something like:
//   "Bip01 R Hand=12.345, Bip01 L Hand=11.812, Bip01 Head=4.231"
// or an empty string if there's no skeleton or all joints are static.
std::string SummarizeTopActiveBones(const LoadedAnimation& anim,
                                    const std::vector<std::vector<float>>& motion,
                                    std::size_t topN = 3) {
  if (anim.jointNames.empty() || motion.empty()) return {};

  struct Pair { std::size_t idx; float total; };
  std::vector<Pair> rank;
  rank.reserve(anim.jointNames.size());
  for (std::size_t j = 0; j < anim.jointNames.size() && j < motion.size(); ++j) {
    float sum = 0.0f;
    for (float v : motion[j]) sum += v;
    rank.push_back({j, sum});
  }
  std::partial_sort(rank.begin(),
                    rank.begin() + std::min(topN, rank.size()),
                    rank.end(),
                    [](const Pair& a, const Pair& b) { return a.total > b.total; });

  std::string out;
  bool first = true;
  for (std::size_t k = 0; k < std::min(topN, rank.size()); ++k) {
    // Show all top-N entries even if total is 0 — a static bone in the
    // ranking is informative ("this rig has 18 joints but only 3 actually
    // move"). Only suppress trailing zero-motion entries below an outright
    // numerical-noise threshold.
    if (rank[k].total < 1e-6f) break;
    if (!first) out += ", ";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s=%.3f", anim.jointNames[rank[k].idx].c_str(),
                  rank[k].total);
    out += buf;
    first = false;
  }
  return out;
}
}  // namespace

// ---- Static state -----------------------------------------------------------

// Default: empty prefix (v0.4.0+). Set via SetRegionNamePrefix() from
// entry.cpp on startup based on the `region_name_prefix` config key.
std::string AnimationLibrary::s_regionNamePrefix_;

const char* AnimationLibrary::kLegacyRegionNamePrefix = "MDD: ";

void AnimationLibrary::SetRegionNamePrefix(const std::string& prefix) {
  s_regionNamePrefix_ = prefix;
}

const std::string& AnimationLibrary::RegionNamePrefix() {
  return s_regionNamePrefix_;
}

std::string AnimationLibrary::StripPrefix(const std::string& regionName) {
  // Try the currently-configured prefix first (it could be empty).
  if (!s_regionNamePrefix_.empty() &&
      regionName.size() >= s_regionNamePrefix_.size() &&
      regionName.compare(0, s_regionNamePrefix_.size(), s_regionNamePrefix_) == 0) {
    return regionName.substr(s_regionNamePrefix_.size());
  }

  // Legacy prefix: regions written by v0.3.0 and earlier always carry "MDD: ".
  // Recognise them on read so old projects keep working after the user
  // upgrades to v0.4.0.
  const std::string legacy = kLegacyRegionNamePrefix;
  if (regionName.size() >= legacy.size() &&
      regionName.compare(0, legacy.size(), legacy) == 0) {
    return regionName.substr(legacy.size());
  }

  return regionName;
}

// ---- LoadedAnimation --------------------------------------------------------

double LoadedAnimation::DurationSeconds(double fps) const {
  if (fps <= 0.0) return 0.0;
  if (mdd && mdd->IsLoaded()) {
    return static_cast<double>(mdd->TotalFrames()) / fps;
  }
  if (glb && glb->IsLoaded()) {
    return static_cast<double>(glb->TotalFrames()) / fps;
  }
  return 0.0;
}

std::uint32_t LoadedAnimation::TotalFrames() const {
  if (mdd && mdd->IsLoaded()) return mdd->TotalFrames();
  if (glb && glb->IsLoaded()) return glb->TotalFrames();
  return 0;
}

std::uint32_t LoadedAnimation::TotalPoints() const {
  if (mdd && mdd->IsLoaded()) return mdd->TotalPoints();
  if (glb && glb->IsLoaded()) return glb->TotalPoints();
  return 0;
}

const std::vector<float>& LoadedAnimation::VerticesForFrame(std::uint32_t frame) const {
  static const std::vector<float> empty;
  if (mdd && mdd->IsLoaded()) return mdd->VerticesForFrame(frame);
  if (glb && glb->IsLoaded()) return glb->VerticesForFrame(frame);
  return empty;
}

const std::vector<std::uint32_t>* LoadedAnimation::TriangleIndicesPtr() const {
  if (obj && obj->IsLoaded()) return &obj->TriangleIndices();
  if (glb && glb->IsLoaded()) return &glb->TriangleIndices();
  return nullptr;
}

bool LoadedAnimation::HasTopology() const {
  return TriangleIndicesPtr() != nullptr;
}

// ---- AnimationLibrary -------------------------------------------------------

void AnimationLibrary::Clear() {
  animations_.clear();
  regions_.clear();
  directory_.clear();
}

std::size_t AnimationLibrary::FindAnimationIndexByBasename(const std::string& basename) const {
  for (std::size_t i = 0; i < animations_.size(); ++i) {
    if (animations_[i]->basename == basename) return i;
  }
  return std::numeric_limits<std::size_t>::max();
}

std::size_t AnimationLibrary::ResolveAnimationByItemName(const std::string& itemName) const {
  // First try direct match. This intentionally takes priority over suffix
  // stripping so that a real animation actually called "frog_jump_2" wins
  // over the "variation" interpretation. Real animation names are rare and
  // user-controlled; variations are common and ambiguity matters.
  const std::size_t direct = FindAnimationIndexByBasename(itemName);
  if (direct != std::numeric_limits<std::size_t>::max()) return direct;

  // Try stripping a trailing "_<digits>" variation suffix. This matches
  // REAPER's default duplicate naming pattern for items ("foo", "foo_2",
  // "foo_3"...). We only strip a single such suffix — chained variations
  // like "frog_jump_2_3" would resolve through the second strip too if the
  // intermediate "frog_jump_2" doesn't exist, but that's caller-driven
  // behaviour, not part of this method.
  const auto underscore = itemName.find_last_of('_');
  if (underscore == std::string::npos || underscore + 1 >= itemName.size()) {
    return std::numeric_limits<std::size_t>::max();
  }
  const std::string tail = itemName.substr(underscore + 1);
  const bool allDigits = !tail.empty() && std::all_of(tail.begin(), tail.end(),
      [](unsigned char c) { return std::isdigit(c); });
  if (!allDigits) {
    return std::numeric_limits<std::size_t>::max();
  }
  const std::string base = itemName.substr(0, underscore);
  return FindAnimationIndexByBasename(base);
}

std::size_t AnimationLibrary::ScanFolder(const std::string& directory,
                                         double fps,
                                         std::string* logOut) {
  Clear();
  directory_ = directory;

  if (directory.empty()) {
    Append(logOut, "[AnimationLibrary] empty directory.");
    return 0;
  }

  std::vector<std::string> mddFiles;
  std::vector<std::string> objFiles;
  std::vector<std::string> glbFiles;

  WIN32_FIND_DATAA fd{};
  const std::string pattern = directory + "\\*.*";
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    Append(logOut, "[AnimationLibrary] FindFirstFile failed for: " + directory);
    return 0;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::string name = fd.cFileName;
    const std::string ext = LowerExt(name);
    const std::string full = directory + "\\" + name;
    if      (ext == ".mdd") mddFiles.push_back(full);
    else if (ext == ".obj") objFiles.push_back(full);
    else if (ext == ".glb") glbFiles.push_back(full);
  } while (FindNextFileA(h, &fd));
  FindClose(h);

  // Sort each group alphabetically by stem so region order is stable.
  auto byStem = [](const std::string& a, const std::string& b) { return Stem(a) < Stem(b); };
  std::sort(mddFiles.begin(), mddFiles.end(), byStem);
  std::sort(glbFiles.begin(), glbFiles.end(), byStem);

  // ---- Load OBJs once into a vertex-count lookup table for MDD pairing ---
  struct LoadedObj {
    std::string path;
    std::shared_ptr<ObjIndexLoader> obj;
  };
  std::vector<LoadedObj> loadedObjs;
  loadedObjs.reserve(objFiles.size());
  for (const std::string& objPath : objFiles) {
    auto loader = std::make_shared<ObjIndexLoader>();
    if (loader->LoadFromFile(objPath)) {
      loadedObjs.push_back({objPath, loader});
    } else {
      Append(logOut, "[AnimationLibrary] OBJ unreadable, ignored: " + objPath);
    }
  }

  // ---- Pass 1: MDD files (with optional OBJ pairing) -----------------------
  for (const std::string& mddPath : mddFiles) {
    auto entry = std::make_unique<LoadedAnimation>();
    entry->basename = Stem(mddPath);
    entry->sourcePath = mddPath;
    entry->mddPath = mddPath;
    entry->mdd = std::make_unique<MDDDataManager>();
    entry->obj = std::make_unique<ObjIndexLoader>();

    if (!entry->mdd->LoadFromFile(mddPath)) {
      Append(logOut, "[AnimationLibrary] FAILED MDD: " + mddPath +
                         " (" + entry->mdd->LastError() + ")");
      continue;
    }

    const std::uint32_t mddPoints = entry->mdd->TotalPoints();

    bool paired = false;
    const std::string directObj = directory + "\\" + entry->basename + ".obj";
    if (FileExists(directObj)) {
      if (entry->obj->LoadFromFile(directObj)) {
        entry->objPath = directObj;
        paired = true;
      }
    }
    if (!paired) {
      for (const auto& candidate : loadedObjs) {
        if (candidate.obj->VertexCount() == mddPoints) {
          if (entry->obj->LoadFromFile(candidate.path)) {
            entry->objPath = candidate.path;
            paired = true;
            Append(logOut, "[AnimationLibrary] '" + entry->basename + "' -> '" +
                              Stem(candidate.path) + ".obj' by vertex-count match (" +
                              std::to_string(mddPoints) + ")");
            break;
          }
        }
      }
    }
    if (!paired) {
      // Drop the unloaded ObjIndexLoader so HasTopology() returns false.
      entry->obj.reset();
      Append(logOut, "[AnimationLibrary] no OBJ matched for '" + entry->basename +
                         "' (" + std::to_string(mddPoints) + " points). Points-only.");
    }

    ComputeRestPoseBoundingBox(*entry);
    ComputeRestNormals(*entry);

    Append(logOut, "[AnimationLibrary] loaded MDD '" + entry->basename + "' (frames=" +
                       std::to_string(entry->mdd->TotalFrames()) + ", points=" +
                       std::to_string(entry->mdd->TotalPoints()) + ", obj=" +
                       (paired ? "yes" : "no") + ")");
    animations_.push_back(std::move(entry));
  }

  // ---- Pass 2: GLB files ----------------------------------------------------
  // Each animation in the file becomes its own LoadedAnimation. Naming:
  //   1 animation in file:   basename = file stem
  //   N animations in file:  basename = "<stem>.<animName-or-index>"
  for (const std::string& glbPath : glbFiles) {
    const std::string stem = Stem(glbPath);
    std::vector<std::string> animNames;
    std::string countErr;
    const std::size_t animCount = GlbLoader::CountAnimations(glbPath, &animNames, &countErr);
    if (animCount == 0) {
      Append(logOut, "[AnimationLibrary] GLB '" + stem + "' has no animations or failed to parse: " + countErr);
      continue;
    }

    for (std::size_t a = 0; a < animCount; ++a) {
      auto entry = std::make_unique<LoadedAnimation>();
      entry->sourcePath = glbPath;
      entry->glb = std::make_unique<GlbLoader>();

      if (!entry->glb->LoadFromFileAtIndex(glbPath, a, fps)) {
        Append(logOut, "[AnimationLibrary] FAILED GLB '" + stem + "' anim #" +
                           std::to_string(a) + ": " + entry->glb->LastError());
        continue;
      }

      // Build basename. Single-animation files keep the simple stem; files
      // with multiple animations get a "<stem>.<animName>" composite, with
      // a numeric fallback for unnamed channels.
      if (animCount == 1) {
        entry->basename = stem;
      } else {
        const std::string& animName = animNames[a];
        entry->basename = stem + "." + (animName.empty() ? std::to_string(a) : animName);
      }

      ComputeRestPoseBoundingBox(*entry);
      ComputeRestNormals(*entry);
      CopyMotionDataFromGlb(*entry);

      const std::string topWorld = SummarizeTopActiveBones(*entry, entry->worldMotion);
      const std::string topLocal = SummarizeTopActiveBones(*entry, entry->localMotion);
      std::string motionSummary;
      if (!topWorld.empty()) motionSummary += "; top world: " + topWorld;
      if (!topLocal.empty()) motionSummary += "; top local: " + topLocal;

      Append(logOut, "[AnimationLibrary] loaded GLB '" + entry->basename + "' (frames=" +
                         std::to_string(entry->glb->TotalFrames()) + ", points=" +
                         std::to_string(entry->glb->TotalPoints()) + ", joints=" +
                         std::to_string(entry->jointNames.size()) +
                         motionSummary + ")");

      animations_.push_back(std::move(entry));
    }
  }

  return animations_.size();
}

void AnimationLibrary::BuildRegions(double fps, double gapSeconds, double startSeconds) {
  regions_.clear();
  double cursor = startSeconds;
  for (std::size_t i = 0; i < animations_.size(); ++i) {
    const double duration = animations_[i]->DurationSeconds(fps);
    TimelineRegion r;
    r.animationIndex = i;
    r.startSeconds = cursor;
    r.endSeconds = cursor + duration;
    r.regionName = s_regionNamePrefix_ + animations_[i]->basename;
    regions_.push_back(r);
    cursor = r.endSeconds + gapSeconds;
  }
}

double AnimationLibrary::TotalSpanSeconds() const {
  if (regions_.empty()) return 0.0;
  return regions_.back().endSeconds - regions_.front().startSeconds;
}

bool AnimationLibrary::ResolvePlayhead(double playheadSeconds,
                                       double fps,
                                       const std::vector<TimelineRegion>& liveRegions,
                                       std::size_t* outAnimationIndex,
                                       std::uint32_t* outFrameIndex) const {
  if (animations_.empty()) return false;

  const std::vector<TimelineRegion>& src = liveRegions.empty() ? regions_ : liveRegions;

  if (src.empty()) {
    if (outAnimationIndex) *outAnimationIndex = 0;
    if (outFrameIndex) *outFrameIndex = 0;
    return true;
  }

  double earliestStart = src.front().startSeconds;
  for (const auto& r : src) earliestStart = std::min(earliestStart, r.startSeconds);

  if (playheadSeconds < earliestStart) {
    if (outAnimationIndex) *outAnimationIndex = src.front().animationIndex;
    if (outFrameIndex) *outFrameIndex = 0;
    return true;
  }

  for (const auto& r : src) {
    if (playheadSeconds >= r.startSeconds && playheadSeconds <= r.endSeconds) {
      const LoadedAnimation& anim = *animations_[r.animationIndex];
      const std::uint32_t totalFrames = anim.TotalFrames();
      if (totalFrames == 0) {
        if (outAnimationIndex) *outAnimationIndex = r.animationIndex;
        if (outFrameIndex) *outFrameIndex = 0;
        return true;
      }
      const std::uint32_t lastFrame = totalFrames - 1;
      const double localTime = playheadSeconds - r.startSeconds;
      double f = std::floor(std::max(0.0, localTime) * fps);
      if (f > static_cast<double>(lastFrame)) f = static_cast<double>(lastFrame);
      if (outAnimationIndex) *outAnimationIndex = r.animationIndex;
      if (outFrameIndex) *outFrameIndex = static_cast<std::uint32_t>(f);
      return true;
    }
  }

  std::size_t bestIdx = 0;
  bool found = false;
  double bestEnd = -1.0;
  for (std::size_t i = 0; i < src.size(); ++i) {
    if (src[i].endSeconds <= playheadSeconds && src[i].endSeconds > bestEnd) {
      bestEnd = src[i].endSeconds;
      bestIdx = i;
      found = true;
    }
  }
  if (!found) {
    if (outAnimationIndex) *outAnimationIndex = src.front().animationIndex;
    if (outFrameIndex) *outFrameIndex = 0;
    return true;
  }

  const TimelineRegion& last = src[bestIdx];
  const LoadedAnimation& anim = *animations_[last.animationIndex];
  const std::uint32_t totalFrames = anim.TotalFrames();
  const std::uint32_t lastFrame = totalFrames > 0 ? totalFrames - 1 : 0;
  if (outAnimationIndex) *outAnimationIndex = last.animationIndex;
  if (outFrameIndex) *outFrameIndex = lastFrame;
  return true;
}
