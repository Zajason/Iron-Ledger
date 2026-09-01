#include "il/crc32c.h"

#include <array>

namespace il {
namespace {

constexpr uint32_t kPoly = 0x82F63B78u;  // Castagnoli, reflected.

// Standard byte-at-a-time reflected table, built at compile time.
constexpr std::array<uint32_t, 256> BuildTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (kPoly ^ (c >> 1)) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

constexpr std::array<uint32_t, 256> kTable = BuildTable();

}  // namespace

uint32_t Crc32c(const void* buf, size_t n, uint32_t prev) {
  const auto* p = static_cast<const uint8_t*>(buf);
  uint32_t c = ~prev;
  for (size_t i = 0; i < n; ++i) {
    c = kTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace il
