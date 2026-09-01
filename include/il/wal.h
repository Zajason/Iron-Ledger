// Write-ahead log: records and nothing else.
//
// There is no superblock, no free list, no index. Recovery is exactly "replay
// every record that verifies, and stop at the first one that does not". A log
// with no metadata has no metadata to corrupt, and the recovery rule fits in
// one sentence, which is the only reason it can be reasoned about after a
// power cut.
#ifndef IL_WAL_H_
#define IL_WAL_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "il/device.h"

namespace il {

// Record header: packed, little-endian, 24 bytes.
//
//   off  size  field
//     0     4  magic  = kRecordMagic
//     4     4  crc    = CRC-32C over bytes [8, 24) of the header, then payload
//     8     8  lsn    = byte offset of this header (see below)
//    16     4  type
//    20     4  len    = payload length
//
// The lsn is the record's own byte offset. That is load-bearing, not
// decoration. After a power cut, sector N can be missing while sector N+1
// survived intact -- a hole in the middle of the log. A scanner that only
// checks magic and CRC will happily resynchronise onto the later record and
// replay it as though nothing were missing, silently skipping the hole. A
// self-locating lsn makes that impossible: the surviving record announces the
// offset it belongs at, the scanner is somewhere else, and the scan stops.
constexpr uint32_t kRecordMagic = 0x474F4C49u;  // "ILOG" little-endian
constexpr size_t kHeaderSize = 24;

// Sanity bound so a corrupt length field can't drive a huge allocation.
constexpr uint32_t kMaxPayloadSize = 1u << 20;

// Why a scan stopped where it did.
enum class ScanStop {
  kCleanEnd,     // ran off the end of the device with nothing in progress
  kShortRead,    // header or payload truncated by the end of the device
  kBadMagic,     // not a record header (zeros from a lost sector, or garbage)
  kBadLsn,       // header is fine but claims to live at a different offset
  kBadLength,    // length field beyond kMaxPayloadSize
  kBadCrc,       // checksum mismatch
  kBadPayload,   // structurally fine, but the visitor rejected it
};

const char* ScanStopName(ScanStop s);

struct ScanResult {
  uint64_t valid_bytes = 0;  // total bytes of records that were accepted
  uint64_t records = 0;      // how many were accepted
  ScanStop stop = ScanStop::kCleanEnd;
};

// Returns false to reject the record and stop the scan (-> kBadPayload).
using RecordVisitor =
    std::function<bool(uint64_t lsn, uint32_t type, const uint8_t* payload, uint32_t len)>;

// Replays |dev| from offset 0. With |verify_crc| false the checksum field is
// written but never checked -- that is the "checksums off" arm of the
// experiment, not an optimisation anyone should ship.
ScanResult ScanLog(const Device& dev, bool verify_crc, const RecordVisitor& visitor);

// ---------------------------------------------------------------------------
// LogWriter
//
// Flush() and Sync() are deliberately separate verbs, and collapsing them is
// the one change that would destroy the point of this repo:
//
//   Flush() -- hand the buffered bytes to the device (the page cache). The data
//              now survives the process dying. It does NOT survive power loss.
//   Sync()  -- fsync. The data is on stable storage.
//
// Every durability mode in this project is just a different arrangement of
// these two calls and the acknowledgement, and every interesting result comes
// from the gap between them.
// ---------------------------------------------------------------------------
class LogWriter {
 public:
  LogWriter(Device& dev, uint64_t start_offset)
      : dev_(dev), tail_(start_offset) {}

  // Buffers one record; returns its lsn (== the offset it will occupy).
  uint64_t Append(uint32_t type, const void* payload, uint32_t len);

  void Flush();  // buffered bytes -> device, in a single WriteAt
  void Sync();   // Flush(), then the device's fsync

  uint64_t next_lsn() const { return tail_ + buffer_.size(); }
  uint64_t durable_tail() const { return tail_; }
  size_t buffered_bytes() const { return buffer_.size(); }
  size_t buffered_records() const { return buffered_records_; }

 private:
  Device& dev_;
  std::vector<uint8_t> buffer_;
  uint64_t tail_;  // device offset the buffer will be written at
  size_t buffered_records_ = 0;
};

// Serialises one record (header + payload) onto the end of |out|. Exposed so
// tests can hand-build logs, including deliberately damaged ones.
void EncodeRecord(std::vector<uint8_t>& out, uint64_t lsn, uint32_t type,
                  const void* payload, uint32_t len);

}  // namespace il

#endif  // IL_WAL_H_
