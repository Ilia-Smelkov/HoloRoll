#include "core/obj_index_loader.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
// Parse a single face token like "12", "12/34", "12/34/56", or "12//56".
// Returns 1-based vertex index; 0 on failure.
std::int64_t ParseFaceToken(const char* token) {
  // Token ends at slash, whitespace, or null.
  std::int64_t v = 0;
  bool any = false;
  bool neg = false;
  const char* p = token;
  if (*p == '-') {
    neg = true;
    ++p;
  } else if (*p == '+') {
    ++p;
  }
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    ++p;
    any = true;
  }
  if (!any) return 0;
  return neg ? -v : v;
}
}  // namespace

void ObjIndexLoader::Clear() {
  triangleIndices_.clear();
  vertexCount_ = 0;
}

bool ObjIndexLoader::LoadFromFile(const std::string& filePath) {
  Clear();
  lastError_.clear();

  std::FILE* file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, filePath.c_str(), "r") != 0 || !file) {
    lastError_ = "fopen failed: " + filePath;
    return false;
  }
#else
  file = std::fopen(filePath.c_str(), "r");
  if (!file) {
    lastError_ = "fopen failed: " + filePath;
    return false;
  }
#endif

  std::uint32_t vCount = 0;

  // OBJ lines are usually short, but we use a generous static buffer.
  char line[1024];
  while (std::fgets(line, sizeof(line), file)) {
    // Skip leading whitespace.
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;

    if (*p == 'v' && (p[1] == ' ' || p[1] == '\t')) {
      ++vCount;
      continue;
    }

    if (*p != 'f' || (p[1] != ' ' && p[1] != '\t')) {
      continue;
    }

    // Parse face: collect 0-based vertex indices for each face token.
    p += 2;
    std::vector<std::uint32_t> faceVerts;
    faceVerts.reserve(4);

    while (*p) {
      while (*p == ' ' || *p == '\t') ++p;
      if (*p == '\0' || *p == '\n' || *p == '\r') break;

      const std::int64_t raw = ParseFaceToken(p);
      // Advance past this token (skip non-space/non-newline).
      while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;

      if (raw == 0) {
        // Bad token — abandon this face.
        faceVerts.clear();
        break;
      }
      std::int64_t idx = raw;
      if (idx < 0) {
        // Relative index. We don't know the running count here without
        // pre-pass; require absolute indices for this minimal loader.
        faceVerts.clear();
        break;
      }
      // OBJ is 1-based.
      faceVerts.push_back(static_cast<std::uint32_t>(idx - 1));
    }

    if (faceVerts.size() < 3) continue;

    // Fan triangulation: (v0, v1, v2), (v0, v2, v3), ...
    for (std::size_t i = 1; i + 1 < faceVerts.size(); ++i) {
      triangleIndices_.push_back(faceVerts[0]);
      triangleIndices_.push_back(faceVerts[i]);
      triangleIndices_.push_back(faceVerts[i + 1]);
    }
  }

  std::fclose(file);
  vertexCount_ = vCount;

  if (triangleIndices_.empty()) {
    lastError_ = "OBJ contained no usable face data: " + filePath;
    return false;
  }
  return true;
}
