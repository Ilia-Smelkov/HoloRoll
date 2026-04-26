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

  // 1) Enumerate .mdd and .obj separately.
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

  // 2) Pre-load every OBJ once. Used both for direct-name pairing and for
  //    vertex-count fallback pairing.
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

  // 3) For every MDD, try direct-stem match first, then fall back to
  //    vertex-count match.
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

    // 3a) Direct: <basename>.obj
    bool paired = false;
    const std::string directObj = directory + "\\" + entry->basename + ".obj";
    if (FileExists(directObj)) {
      if (entry->obj->LoadFromFile(directObj)) {
        entry->objPath = directObj;
        paired = true;
      }
    }

    // 3b) Vertex-count fallback: pick the first OBJ with same vertex count.
    if (!paired) {
      for (const auto& candidate : loadedObjs) {
        if (candidate.obj->VertexCount() == mddPoints) {
          // Copy the topology data into our owned loader (cheap — same indices).
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

  // Find earliest start to handle "before any region".
  double earliestStart = src.front().startSeconds;
  for (const auto& r : src) earliestStart = std::min(earliestStart, r.startSeconds);

  if (playheadSeconds < earliestStart) {
    // Before any region — show the first animation, frame 0.
    if (outAnimationIndex) *outAnimationIndex = src.front().animationIndex;
    if (outFrameIndex) *outFrameIndex = 0;
    return true;
  }

  // Inside a region? Pick the first match.
  // (If user manages to overlap regions, first wins — predictable.)
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

  // Outside any region — pick the most recent region whose end <= playhead,
  // show its last frame.
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
