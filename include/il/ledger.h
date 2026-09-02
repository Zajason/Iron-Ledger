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

}  // namespace il

#endif  // IL_LEDGER_H_
