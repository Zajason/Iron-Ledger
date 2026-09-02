// One deterministic transfer stream, shared by every harness.
//
// Shared so the SIGKILL driver can reconstruct what its worker was doing
// without any IPC beyond the raw ack bytes, and so the crash simulator's second
// pass can replay the first one exactly.
//
// The parameters are chosen so that NOTHING IS EVER REJECTED: every account is
// funded with 10^12 and transfer amounts are capped at 1000, so no account can
// run dry within any campaign we run. That matters because it makes the
// idempotency keys come out dense -- 1, 2, 3, ... -- which turns
//
//     "the recovered log is a prefix of what was submitted"
//
// into a check a harness can make entirely on its own: the recovered key
// sequence must be exactly 1..k. Any gap, duplicate or reordering is instantly
// visible, with no need to ask the ledger what it thinks happened.
#ifndef IL_WORKLOAD_H_
#define IL_WORKLOAD_H_

#include <cstdint>
#include <random>
#include <vector>

#include "il/ledger.h"

namespace il {

struct WorkloadOptions {
  uint64_t seed = 1;
  uint32_t accounts = 8;                  // real accounts, numbered 1..accounts
  int64_t funding = 1'000'000'000'000;    // 10^12 into each, from the world
  int64_t max_amount = 1000;              // so balances cannot be exhausted
  uint32_t max_postings = 4;              // legs per transfer, >= kMinPostings
};

// Generates transfers with dense keys 1, 2, 3, ... Constructing two Workloads
// with the same options yields byte-identical streams; that is the whole point.
class Workload {
 public:
  explicit Workload(const WorkloadOptions& opts)
      : opts_(opts), rng_(opts.seed) {}

  // The first |accounts| transfers fund each account from the world; the rest
  // move money between funded accounts.
  Transfer Next() {
    const uint64_t key = ++issued_;
    Transfer t;
    t.idem_key = key;

    if (key <= opts_.accounts) {
      const uint64_t account = key;  // accounts 1..N
      t.postings = {{kWorldAccount, -opts_.funding}, {account, opts_.funding}};
      return t;
    }

    // 2..max_postings legs: one payer, the rest receive. Distinct accounts, so
    // a transfer never nets against itself.
    const uint32_t legs = PickLegs();
    const std::vector<uint64_t> chosen = PickAccounts(legs);

    int64_t total = 0;
    for (uint32_t i = 1; i < legs; ++i) {
      const int64_t amount =
          1 + static_cast<int64_t>(rng_() % static_cast<uint64_t>(opts_.max_amount));
      t.postings.push_back({chosen[i], amount});
      total += amount;
    }
    t.postings.insert(t.postings.begin(), {chosen[0], -total});  // the payer
    return t;
  }

  uint64_t issued() const { return issued_; }

 private:
  uint32_t PickLegs() {
    const uint32_t lo = static_cast<uint32_t>(kMinPostings);
    uint32_t hi = opts_.max_postings < lo ? lo : opts_.max_postings;
    // Never ask for more distinct accounts than exist, or PickAccounts would
    // spin forever looking for one it cannot find.
    if (hi > opts_.accounts) hi = opts_.accounts;
    return lo + static_cast<uint32_t>(rng_() % (hi - lo + 1));
  }

  std::vector<uint64_t> PickAccounts(uint32_t n) {
    std::vector<uint64_t> out;
    out.reserve(n);
    while (out.size() < n) {
      const uint64_t a = 1 + rng_() % opts_.accounts;
      bool dup = false;
      for (uint64_t seen : out) dup = dup || seen == a;
      if (!dup) out.push_back(a);
    }
    return out;
  }

  WorkloadOptions opts_;
  std::mt19937_64 rng_;
  uint64_t issued_ = 0;
};

}  // namespace il

#endif  // IL_WORKLOAD_H_
