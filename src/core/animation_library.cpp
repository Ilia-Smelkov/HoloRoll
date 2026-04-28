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
  if (!anim.mdd || !anim.mdd->IsLoaded()) return;
  const auto& frame = anim.mdd->VerticesForFrame(0);
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
  if (!anim.mdd || !anim.mdd->IsLoaded()) return;
  if (!anim.obj || !anim.obj->IsLoaded()) return;

  const auto& positions = anim.mdd->VerticesForFrame(0);
  const auto& indices = anim.obj->TriangleIndices();
  if (indices.size() < 3) return;
  const std::uint32_t pointCount = anim.mdd->TotalPoints();

  const std::size_t triangleCount = indices.size() / 3;
  anim.restNormals.resize(triangleCount * 3);

  for (std::size_t t = 0; t < triangleCount; ++t) {
    const std::uint32_t i0 = indices[t * 3 + 0];
    const std::uint32_t i1 = indices[t * 3 + 1];
    const std::uint32_t i2 = indices[t * 3 + 2];

    // Bounds-check; if anything is off, leave (0,0,0) and continue.
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

    // Edge vectors
    const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
    const float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;

    // Cross product e1 x e2 (counter-clockwise = outward-facing assumption)
    float nx = e1y * e2z - e1z * e2y;
    float ny = e1z * e2x - e1x * e2z;
    float nz = e1x * e2y - e1y * e2x;

    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
      nx /= len; ny /= len; nz /= len;
    } else {
      nx = 0.0f; ny = 1.0f; nz = 0.0f;  // degenerate triangle -> arbitrary
    }
    anim.restNormals[t * 3 + 0] = nx;
    anim.restNormals[t * 3 + 1] = ny;
    anim.restNormals[t * 3 + 2] = nz;
  }
}
}  // namespace

double LoadedAnimation::DurationSeconds(double fps) const {
  if (!mdd || !mdd->IsLoaded() || fps <= 0.0) return 0.0;
  return static_cast<double>(mdd->TotalFrames()) / fps;
}

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

std::size_t AnimationLibrary::ScanFolder(const std::string& directory, std::string* logOut) {
  Clear();
  directory_ = directory;

  if (directory.empty()) {
    Append(logOut, "[AnimationLibrary] empty directory.");
    return 0;
  }

  std::vector<std::string> mddFiles;
  std::vector<std::string> objFiles;

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
    if (ext == ".mdd") mddFiles.push_back(full);
    else if (ext == ".obj") objFiles.push_back(full);
  } while (FindNextFileA(h, &fd));
  FindClose(h);

  std::sort(mddFiles.begin(), mddFiles.end(),
            [](const std::string& a, const std::string& b) { return Stem(a) < Stem(b); });

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

  for (const std::string& mddPath : mddFiles) {
    auto entry = std::make_unique<LoadedAnimation>();
    entry->basename = Stem(mddPath);
    entry->mddPath = mddPath;
    entry->mdd = std::make_unique<MDDDataManager>();
    entry->obj = std::make_unique<ObjIndexLoader>();

    if (!entry->mdd->LoadFromFile(mddPath)) {
      Append(logOut, "[AnimationLibrary] FAILED MDD: " + mddPath + " (" + entry->mdd->LastError() + ")");
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
      Append(logOut, "[AnimationLibrary] no OBJ matched for '" + entry->basename +
                         "' (" + std::to_string(mddPoints) + " points). Points-only.");
    }

    ComputeRestPoseBoundingBox(*entry);
    ComputeRestNormals(*entry);

    Append(logOut, "[AnimationLibrary] loaded '" + entry->basename + "' (frames=" +
                       std::to_string(entry->mdd->TotalFrames()) + ", points=" +
                       std::to_string(entry->mdd->TotalPoints()) + ", obj=" +
                       (paired ? "yes" : "no") + ")");
    animations_.push_back(std::move(entry));
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
    r.regionName = std::string(kRegionNamePrefix) + animations_[i]->basename;
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
      if (!anim.mdd || !anim.mdd->IsLoaded()) {
        if (outAnimationIndex) *outAnimationIndex = r.animationIndex;
        if (outFrameIndex) *outFrameIndex = 0;
        return true;
      }
      const std::uint32_t lastFrame = anim.mdd->TotalFrames() - 1;
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
  const std::uint32_t lastFrame =
      (anim.mdd && anim.mdd->IsLoaded()) ? anim.mdd->TotalFrames() - 1 : 0;
  if (outAnimationIndex) *outAnimationIndex = last.animationIndex;
  if (outFrameIndex) *outFrameIndex = lastFrame;
  return true;
}
