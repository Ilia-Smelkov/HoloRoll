#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal Wavefront OBJ index loader.
// We only parse `f` lines (faces) and convert them to a flat list of
// 0-based vertex indices for triangle rendering.
//
// MDD already provides per-frame vertex positions, so this loader is used
// purely as a topology source for wireframe / solid render modes.
class ObjIndexLoader {
 public:
  // Returns true on success; false if the file is missing/unreadable.
  // Faces with more than 3 vertices are fan-triangulated (v0,vN,vN+1).
  // Negative (relative) indices and non-triangulable faces are skipped.
  bool LoadFromFile(const std::string& filePath);

  void Clear();

  bool IsLoaded() const { return !triangleIndices_.empty(); }
  const std::vector<std::uint32_t>& TriangleIndices() const { return triangleIndices_; }
  std::uint32_t VertexCount() const { return vertexCount_; }
  const std::string& LastError() const { return lastError_; }

 private:
  std::vector<std::uint32_t> triangleIndices_;
  std::uint32_t vertexCount_ = 0;
  std::string lastError_;
};
