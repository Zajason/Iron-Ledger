// CRC-32C (Castagnoli), reflected polynomial 0x82F63B78.
//
// Software table-driven implementation only: no SSE4.2 / ARMv8 CRC intrinsics.
// The point of this repo is crash behaviour, not checksum throughput, and a
// portable implementation keeps the CRC bit-identical across the machines that
// run the campaign.
#ifndef IL_CRC32C_H_
#define IL_CRC32C_H_

#include <cstddef>
#include <cstdint>

namespace il {

// Returns the CRC-32C of |n| bytes at |buf|.
//
// |prev| is a previously returned CRC, so checksums compose over adjacent
// buffers:  Crc32c(b, nb, Crc32c(a, na)) == Crc32c(ab, na + nb).
// This is what lets the WAL checksum a header field range and a payload that
// live in two different buffers without copying them together first.
uint32_t Crc32c(const void* buf, size_t n, uint32_t prev = 0);

}  // namespace il

#endif  // IL_CRC32C_H_
