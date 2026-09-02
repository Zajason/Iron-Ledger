// Double-entry ledger, event-sourced onto the WAL.
//
// The log is the truth; balances are a derived view rebuilt by replaying it.
// Opening a ledger IS recovery -- a fresh device simply replays nothing -- so
// the recovery path is exercised by every single test, rather than being a
// rarely-taken branch that first runs in production at 3am.
#ifndef IL_LEDGER_H_
#define IL_LEDGER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il/device.h"
#include "il/wal.h"

namespace il {

// Account 0 is the outside world. External money is booked against it, and it
// is the only account permitted to go negative: a deposit is not money
// appearing from nowhere, it is a transfer *from the world*. That keeps the
// sum over all accounts at exactly zero without exception, which is what lets
// us assert the invariant unconditionally.
constexpr uint64_t kWorldAccount = 0;

// One leg of a transfer. Signed: negative leaves the account, positive arrives.
struct Posting {
  uint64_t account = 0;
  int64_t amount = 0;
};

// Postings must sum to exactly zero. Real transactions are rarely two-sided --
// a card payment splits across customer, merchant, processor and network in one
// atomic event -- so 2..8 legs are allowed. Modelling that as several two-sided
// transfers would create moments where a fee had been taken but not yet
// credited, and a crash in such a moment leaves money in limbo. Atomicity here
// is a domain requirement, not a performance optimisation.
constexpr size_t kMinPostings = 2;
constexpr size_t kMaxPostings = 8;

struct Transfer {
  uint64_t idem_key = 0;
  std::vector<Posting> postings;
};

// Wire format, little-endian:
//   u64 idem_key | u32 n | n x (u64 account, i64 amount)
// Written out byte by byte rather than memcpy-ing a struct, so a log written
// on arm64 replays on x86.
void EncodeTransfer(std::vector<uint8_t>& out, const Transfer& t);

// Returns false if the bytes are not a well-formed transfer: truncated, a
// posting count outside [kMinPostings, kMaxPostings], or trailing bytes left
// over. Trailing bytes matter -- a torn write can leave a valid-looking prefix
// followed by stale data, and silently ignoring the tail would accept it.
bool DecodeTransfer(const uint8_t* data, uint32_t len, Transfer* out);

// Record types in the log.
constexpr uint32_t kRecTransfer = 1;

// ---------------------------------------------------------------------------
// Durability modes
//
// The ACK is the moment we tell the outside world "this money moved". It is a
// promise that cannot be retracted, so the only durability question that
// matters is whether the ack is ever ahead of the data. Losing unacknowledged
// data is survivable -- no promise was made, the client retries, and the
// idempotency key makes the retry safe. Losing acknowledged data is a broken
// contract, and that is the number the campaign counts.
//
// Each mode is just a different arrangement of LogWriter::Flush(),
// LogWriter::Sync() and the ack.
// ---------------------------------------------------------------------------
enum class DurabilityMode {
  // write(2) and hope. Never fsyncs. Survives kill -9 (the page cache is
  // intact); loses everything in flight on power loss.
  kNoSync,

  // Acknowledge immediately, fsync every N records. The one that looks perfect
  // in every test you run on a machine that never loses power -- the ack runs
  // ahead of the data by up to N records, and that gap is the whole finding.
  kLazySync,

  // fsync, then acknowledge. Correct by construction, and slow: one fsync per
  // transfer caps throughput regardless of how fast the CPU is.
  kSyncEvery,

  // Buffer N records, one WriteAt, one fsync, then acknowledge all N. Nobody is
  // acked before their data is durable, so this is exactly as correct as
  // kSyncEvery, with the fsync amortised across the batch.
  kGroupCommit,
};

const char* DurabilityModeName(DurabilityMode m);
bool ParseDurabilityMode(const char* s, DurabilityMode* out);

struct LedgerOptions {
  DurabilityMode mode = DurabilityMode::kSyncEvery;

  // False is the "checksums off" arm of the experiment, not an optimisation
  // anyone should ship.
  bool verify_crc = true;

  // Batch size for kGroupCommit; fsync interval for kLazySync. Ignored by the
  // other two modes.
  uint32_t group_size = 32;
};

// Why a Submit did not result in a logged transfer. Invalid transfers never
// reach the log at all -- validation happens first, so the log only ever
// contains transfers that were legal when they were written. That is what makes
// a validation failure during *replay* meaningful (see below).
enum class SubmitStatus {
  kOk,
  kDuplicate,          // idempotency key already seen: a no-op, not an error
  kBadPostingCount,    // outside [kMinPostings, kMaxPostings]
  kUnbalanced,         // postings do not sum to zero
  kInsufficientFunds,  // a non-world account would go negative
};

const char* SubmitStatusName(SubmitStatus s);

// ---------------------------------------------------------------------------
// RecoveryReport
//
// Replay applies the SAME validation rules as Submit. That looks redundant --
// these records were validated when they were written -- but replay is
// deterministic and in submit order, so it sees exactly what the original run
// saw. A record that validated when written MUST validate again on replay.
//
// Therefore a validation failure during replay cannot mean "bad input". It can
// only mean the bytes on disk are not the bytes we wrote. Ordinary business
// rules become a second corruption detector layered underneath the checksums,
// and corruption_admitted() means the checksum layer let something through that
// only a domain rule caught.
// ---------------------------------------------------------------------------
struct RecoveryReport {
  uint64_t records_applied = 0;
  uint64_t valid_bytes = 0;
  ScanStop stop = ScanStop::kCleanEnd;

  uint64_t payload_parse_error = 0;
  uint64_t duplicate_key_in_log = 0;
  uint64_t unbalanced_in_log = 0;
  uint64_t negative_balance_in_log = 0;

  bool corruption_admitted() const {
    return payload_parse_error != 0 || duplicate_key_in_log != 0 ||
           unbalanced_in_log != 0 || negative_balance_in_log != 0;
  }
};

// ---------------------------------------------------------------------------
// Ledger
//
// Opening a ledger IS recovery: the constructor scans the log from offset 0,
// replays every record that verifies, and stops at the first that does not. A
// fresh device replays nothing. There is no separate repair path that only runs
// after a crash, which means the dangerous code runs in every test.
// ---------------------------------------------------------------------------
class Ledger {
 public:
  Ledger(Device& dev, const LedgerOptions& opts, RecoveryReport* report = nullptr);

  Ledger(const Ledger&) = delete;
  Ledger& operator=(const Ledger&) = delete;

  // Validates, logs, applies, and acknowledges according to the durability
  // mode. Under kGroupCommit the ack fires later, when the batch commits.
  SubmitStatus Submit(const Transfer& t);

  // Commits anything still buffered and acknowledges it. kNoSync flushes but
  // still never fsyncs -- forcing it to would make the experiment a lie.
  void Commit();

  // Fires when a transfer becomes durable, in submit order. The crash harness
  // records these and then demands that every one of them survived the cut.
  void set_ack_sink(std::function<void(uint64_t idem_key)> sink) {
    ack_sink_ = std::move(sink);
  }

  int64_t Balance(uint64_t account) const;

  // Every transfer sums to zero, so the sum over all accounts is always zero,
  // whatever sequence was applied. One int64_t that detects corruption without
  // knowing anything about what went wrong -- the cheapest global integrity
  // check in the repo.
  int64_t TotalBalance() const;
  bool ConservationHolds() const { return TotalBalance() == 0; }

  // Idempotency keys are rebuilt from the log during replay, so crash-safe
  // deduplication falls out for free: no second durable structure that could
  // drift out of sync with the first.
  bool HasKey(uint64_t idem_key) const { return seen_keys_.count(idem_key) != 0; }

  // Applied keys in order. The harness checks this is a prefix of what was
  // submitted -- no phantoms, no holes, no reordering.
  const std::vector<uint64_t>& applied_keys() const { return applied_keys_; }
  const std::map<uint64_t, int64_t>& balances() const { return balances_; }

  uint64_t durable_tail() const { return writer_->durable_tail(); }
  const LedgerOptions& options() const { return opts_; }

 private:
  SubmitStatus Validate(const Transfer& t) const;
  void Apply(const Transfer& t);
  void AckPending();
  void SyncNow();

  Device& dev_;
  LedgerOptions opts_;
  std::optional<LogWriter> writer_;

  // std::map, not unordered_map: iteration order is deterministic, so two runs
  // that applied the same transfers produce byte-identical state dumps. The
  // campaign compares recovered state against a clean replay, and that
  // comparison should not depend on hash ordering.
  std::map<uint64_t, int64_t> balances_;
  std::unordered_set<uint64_t> seen_keys_;
  std::vector<uint64_t> applied_keys_;

  std::function<void(uint64_t)> ack_sink_;
  std::vector<uint64_t> pending_acks_;
  uint32_t since_sync_ = 0;

  // Records appended since the last fsync. Lets Commit() skip a redundant
  // fsync on a clean shutdown -- which matters because fsync counts are one of
  // the numbers the benchmark reports.
  bool unsynced_ = false;
  std::vector<uint8_t> scratch_;
};

}  // namespace il

#endif  // IL_LEDGER_H_
