#include "core/mdd_data_manager.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/byte_swap.h"

namespace {
const std::vector<float>& EmptyFrame() {
  static const std::vector<float> kEmpty;
  return kEmpty;
}
}  // namespace

void MDDDataManager::Clear() {
  totalFrames_ = 0;
  totalPoints_ = 0;
  frames_.clear();
  frameTimes_.clear();
  loadedPath_.clear();
  // lastError_ kept intentionally to surface the most recent failure.
}

bool MDDDataManager::LoadFromFile(const std::string& filePath) {
  Clear();
  lastError_.clear();

  std::FILE* file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, filePath.c_str(), "rb") != 0 || !file) {
    lastError_ = "fopen failed: " + filePath;
    return false;
  }
#else
  file = std::fopen(filePath.c_str(), "rb");
  if (!file) {
    lastError_ = "fopen failed: " + filePath;
    return false;
  }
#endif

  // --- Header: 2 * int32 BE ---
  std::uint8_t headerBytes[8];
  if (std::fread(headerBytes, 1, sizeof(headerBytes), file) != sizeof(headerBytes)) {
    std::fclose(file);
    lastError_ = "MDD header read failed (file too small).";
    return false;
  }

  const std::int32_t totalFramesSigned = mdd_endian::ReadBeInt32(headerBytes);
  const std::int32_t totalPointsSigned = mdd_endian::ReadBeInt32(headerBytes + 4);

  if (totalFramesSigned <= 0 || totalPointsSigned <= 0) {
    std::fclose(file);
    lastError_ = "MDD header invalid (non-positive frame/point count).";
    return false;
  }

  totalFrames_ = static_cast<std::uint32_t>(totalFramesSigned);
  totalPoints_ = static_cast<std::uint32_t>(totalPointsSigned);

  // --- Body layout (Blender's .mdd):
  //   float32 times[totalFrames]
  //   float32 coords[totalFrames][totalPoints * 3]
  // All Big-Endian. Times are NOT interleaved with coords.

  // Read all timestamps in one block.
  const std::size_t timesBytes = static_cast<std::size_t>(totalFrames_) * sizeof(float);
  std::vector<std::uint8_t> rawTimes(timesBytes);
  if (std::fread(rawTimes.data(), 1, timesBytes, file) != timesBytes) {
    Clear();
    std::fclose(file);
    lastError_ = "MDD body: failed to read frame-time block.";
    return false;
  }

  frameTimes_.resize(totalFrames_);
  for (std::uint32_t f = 0; f < totalFrames_; ++f) {
    frameTimes_[f] = mdd_endian::ReadBeFloat(rawTimes.data() + f * sizeof(float));
  }

  // Read coords frame by frame.
  const std::size_t componentsPerFrame = static_cast<std::size_t>(totalPoints_) * 3u;
  const std::size_t coordBytesPerFrame = componentsPerFrame * sizeof(float);

  frames_.resize(totalFrames_);
  std::vector<std::uint8_t> rawCoords(coordBytesPerFrame);

  for (std::uint32_t f = 0; f < totalFrames_; ++f) {
    if (std::fread(rawCoords.data(), 1, coordBytesPerFrame, file) != coordBytesPerFrame) {
      Clear();
      lastError_ = "MDD body: failed to read coords at frame " + std::to_string(f);
      std::fclose(file);
      return false;
    }

    auto& frame = frames_[f];
    frame.resize(componentsPerFrame);
    const std::uint8_t* src = rawCoords.data();
    for (std::size_t i = 0; i < componentsPerFrame; ++i) {
      frame[i] = mdd_endian::ReadBeFloat(src + i * sizeof(float));
    }
  }

  std::fclose(file);
  loadedPath_ = filePath;
  return true;
}

const std::vector<float>& MDDDataManager::VerticesForFrame(std::uint32_t frameIndex) const {
  if (frames_.empty()) {
    return EmptyFrame();
  }
  if (frameIndex >= totalFrames_) {
    frameIndex = totalFrames_ - 1;  // clamp
  }
  return frames_[frameIndex];
}

float MDDDataManager::TimeForFrame(std::uint32_t frameIndex) const {
  if (frameTimes_.empty()) {
    return 0.0f;
  }
  if (frameIndex >= totalFrames_) {
    frameIndex = totalFrames_ - 1;
  }
  return frameTimes_[frameIndex];
}
