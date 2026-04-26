#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

// MDD files are stored as Big-Endian.
// Windows / x86_64 are Little-Endian, so we swap bytes after reading raw bytes.
// We use std::memcpy + integer swap to avoid strict-aliasing UB on float values.

namespace mdd_endian {

inline std::uint32_t SwapUint32(std::uint32_t v) noexcept {
#if defined(_MSC_VER)
  return _byteswap_ulong(v);
#elif defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap32(v);
#else
  return ((v & 0x000000FFu) << 24) |
         ((v & 0x0000FF00u) << 8)  |
         ((v & 0x00FF0000u) >> 8)  |
         ((v & 0xFF000000u) >> 24);
#endif
}

// Read big-endian std::int32_t from a 4-byte buffer.
inline std::int32_t ReadBeInt32(const std::uint8_t* src) noexcept {
  std::uint32_t raw = 0;
  std::memcpy(&raw, src, sizeof(raw));
  raw = SwapUint32(raw);
  std::int32_t out = 0;
  std::memcpy(&out, &raw, sizeof(out));
  return out;
}

// Read big-endian float from a 4-byte buffer.
inline float ReadBeFloat(const std::uint8_t* src) noexcept {
  static_assert(sizeof(float) == 4, "MDD parser expects 32-bit float.");
  std::uint32_t raw = 0;
  std::memcpy(&raw, src, sizeof(raw));
  raw = SwapUint32(raw);
  float out = 0.0f;
  std::memcpy(&out, &raw, sizeof(out));
  return out;
}

}  // namespace mdd_endian
