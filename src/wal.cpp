#include "il/wal.h"

#include <cstring>

#include "il/crc32c.h"

namespace il {
namespace {

// Explicit little-endian codecs: the on-disk format must not depend on the
// host, and a log written on x86 has to replay on arm64.
void PutU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

void PutU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t GetU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t GetU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

// Fills the 24-byte header at |h| and returns the CRC that covers it + payload.
uint32_t FillHeader(uint8_t* h, uint64_t lsn, uint32_t type, const void* payload,
                    uint32_t len) {
  PutU32(h + 0, kRecordMagic);
  PutU64(h + 8, lsn);
  PutU32(h + 16, type);
  PutU32(h + 20, len);
  // Checksum order is header-tail first, then payload, so it composes the same
  // way the scanner recomputes it.
  const uint32_t c = Crc32c(payload, len, Crc32c(h + 8, 16, 0));
  PutU32(h + 4, c);
  return c;
}

}  // namespace

const char* ScanStopName(ScanStop s) {
  switch (s) {
    case ScanStop::kCleanEnd: return "clean_end";
    case ScanStop::kShortRead: return "short_read";
    case ScanStop::kBadMagic: return "bad_magic";
    case ScanStop::kBadLsn: return "bad_lsn";
    case ScanStop::kBadLength: return "bad_length";
    case ScanStop::kBadCrc: return "bad_crc";
    case ScanStop::kBadPayload: return "bad_payload";
  }
  return "unknown";
}

void EncodeRecord(std::vector<uint8_t>& out, uint64_t lsn, uint32_t type,
                  const void* payload, uint32_t len) {
  const size_t base = out.size();
  out.resize(base + kHeaderSize + len);
  uint8_t* h = out.data() + base;
  FillHeader(h, lsn, type, payload, len);
  if (len > 0) std::memcpy(h + kHeaderSize, payload, len);
}

uint64_t LogWriter::Append(uint32_t type, const void* payload, uint32_t len) {
  const uint64_t lsn = next_lsn();
  EncodeRecord(buffer_, lsn, type, payload, len);
  ++buffered_records_;
  return lsn;
}

void LogWriter::Flush() {
  if (buffer_.empty()) return;
  // One WriteAt for the whole buffer. Group commit depends on this being a
  // single device write: it is what makes the batch cost one fsync, and it is
  // also what gives the crash harness a single well-defined moment to cut at.
  dev_.WriteAt(tail_, buffer_.data(), buffer_.size());
  tail_ += buffer_.size();
  buffer_.clear();
  buffered_records_ = 0;
}

void LogWriter::Sync() {
  Flush();
  dev_.Sync();
}

ScanResult ScanLog(const Device& dev, bool verify_crc, const RecordVisitor& visitor) {
  ScanResult res;
  uint64_t off = 0;
  uint8_t hdr[kHeaderSize];
  std::vector<uint8_t> payload;

  for (;;) {
    const size_t got = dev.ReadAt(off, hdr, kHeaderSize);
    if (got == 0) {
      res.stop = ScanStop::kCleanEnd;  // ran off the end between records
      break;
    }
    if (got < kHeaderSize) {
      res.stop = ScanStop::kShortRead;  // header truncated by end of device
      break;
    }

    const uint32_t magic = GetU32(hdr + 0);
    if (magic != kRecordMagic) {
      // Zeros from a lost sector land here, and so does anything else that is
      // not a record. The scan stops; it does not go looking for the next
      // plausible header further on.
      res.stop = ScanStop::kBadMagic;
      break;
    }

    const uint32_t stored_crc = GetU32(hdr + 4);
    const uint64_t lsn = GetU64(hdr + 8);
    const uint32_t type = GetU32(hdr + 16);
    const uint32_t len = GetU32(hdr + 20);

    if (lsn != off) {
      // A structurally valid record that belongs somewhere else: proof that
      // bytes went missing between here and there. See the lsn note in wal.h.
      res.stop = ScanStop::kBadLsn;
      break;
    }
    if (len > kMaxPayloadSize) {
      res.stop = ScanStop::kBadLength;
      break;
    }

    payload.resize(len);
    const size_t pgot = len == 0 ? 0 : dev.ReadAt(off + kHeaderSize, payload.data(), len);
    if (pgot < len) {
      res.stop = ScanStop::kShortRead;  // payload truncated by end of device
      break;
    }

    if (verify_crc) {
      const uint32_t want = Crc32c(payload.data(), len, Crc32c(hdr + 8, 16, 0));
      if (want != stored_crc) {
        res.stop = ScanStop::kBadCrc;
        break;
      }
    }

    if (!visitor(lsn, type, payload.data(), len)) {
      res.stop = ScanStop::kBadPayload;
      break;
    }

    off += kHeaderSize + len;
    res.valid_bytes = off;
    ++res.records;
  }

  return res;
}

}  // namespace il
