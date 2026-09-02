#include "il/ledger.h"

#include <cstring>
#include <unordered_map>

namespace il {
namespace {

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t GetU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t GetU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

}  // namespace

void EncodeTransfer(std::vector<uint8_t>& out, const Transfer& t) {
  PutU64(out, t.idem_key);
  PutU32(out, static_cast<uint32_t>(t.postings.size()));
  for (const Posting& p : t.postings) {
    PutU64(out, p.account);
    PutU64(out, static_cast<uint64_t>(p.amount));  // two's complement, defined in C++20
  }
}

bool DecodeTransfer(const uint8_t* data, uint32_t len, Transfer* out) {
  if (len < 12) return false;  // key + count
  const uint64_t key = GetU64(data);
  const uint32_t n = GetU32(data + 8);
  if (n < kMinPostings || n > kMaxPostings) return false;

  // Exact-length check: no trailing bytes tolerated. A torn write can leave a
  // valid prefix followed by stale bytes; accepting that would let corruption
  // through the parse layer.
  const uint64_t need = 12ull + 16ull * n;
  if (len != need) return false;

  out->idem_key = key;
  out->postings.clear();
  out->postings.reserve(n);
  const uint8_t* p = data + 12;
  for (uint32_t i = 0; i < n; ++i, p += 16) {
    Posting post;
    post.account = GetU64(p);
    post.amount = static_cast<int64_t>(GetU64(p + 8));
    out->postings.push_back(post);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
const char* DurabilityModeName(DurabilityMode m) {
  switch (m) {
    case DurabilityMode::kNoSync: return "no_sync";
    case DurabilityMode::kLazySync: return "lazy_sync";
    case DurabilityMode::kSyncEvery: return "sync_every";
    case DurabilityMode::kGroupCommit: return "group_commit";
  }
  return "?";
}

bool ParseDurabilityMode(const char* s, DurabilityMode* out) {
  const std::string v(s ? s : "");
  if (v == "no_sync") { *out = DurabilityMode::kNoSync; return true; }
  if (v == "lazy_sync") { *out = DurabilityMode::kLazySync; return true; }
  if (v == "sync_every") { *out = DurabilityMode::kSyncEvery; return true; }
  if (v == "group_commit") { *out = DurabilityMode::kGroupCommit; return true; }
  return false;
}

const char* SubmitStatusName(SubmitStatus s) {
  switch (s) {
    case SubmitStatus::kOk: return "ok";
    case SubmitStatus::kDuplicate: return "duplicate";
    case SubmitStatus::kBadPostingCount: return "bad_posting_count";
    case SubmitStatus::kUnbalanced: return "unbalanced";
    case SubmitStatus::kInsufficientFunds: return "insufficient_funds";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Validation -- shared verbatim by Submit and by replay.
// ---------------------------------------------------------------------------
SubmitStatus Ledger::Validate(const Transfer& t) const {
  if (t.postings.size() < kMinPostings || t.postings.size() > kMaxPostings) {
    return SubmitStatus::kBadPostingCount;
  }

  // Overflow-checked: a corrupt amount could otherwise wrap to a sum of zero
  // and sail through the balance rule.
  int64_t sum = 0;
  for (const Posting& p : t.postings) {
    if (__builtin_add_overflow(sum, p.amount, &sum)) return SubmitStatus::kUnbalanced;
  }
  if (sum != 0) return SubmitStatus::kUnbalanced;

  if (seen_keys_.count(t.idem_key) != 0) return SubmitStatus::kDuplicate;

  // Accumulate per account first: a transfer may touch the same account twice,
  // and only the net effect decides whether it goes negative.
  std::unordered_map<uint64_t, int64_t> delta;
  for (const Posting& p : t.postings) {
    int64_t& d = delta[p.account];
    if (__builtin_add_overflow(d, p.amount, &d)) return SubmitStatus::kUnbalanced;
  }
  for (const auto& [account, d] : delta) {
    if (account == kWorldAccount) continue;  // the only account allowed negative
    const auto it = balances_.find(account);
    const int64_t before = (it == balances_.end()) ? 0 : it->second;
    int64_t after = 0;
    if (__builtin_add_overflow(before, d, &after)) return SubmitStatus::kUnbalanced;
    if (after < 0) return SubmitStatus::kInsufficientFunds;
  }
  return SubmitStatus::kOk;
}

void Ledger::Apply(const Transfer& t) {
  for (const Posting& p : t.postings) balances_[p.account] += p.amount;
  seen_keys_.insert(t.idem_key);
  applied_keys_.push_back(t.idem_key);
}

// ---------------------------------------------------------------------------
// Recovery-on-open
// ---------------------------------------------------------------------------
Ledger::Ledger(Device& dev, const LedgerOptions& opts, RecoveryReport* report)
    : dev_(dev), opts_(opts) {
  RecoveryReport sink;
  RecoveryReport& r = report ? *report : sink;

  const RecordVisitor visitor = [&](uint64_t, uint32_t type, const uint8_t* p,
                                    uint32_t n) -> bool {
    if (type != kRecTransfer) { ++r.payload_parse_error; return false; }

    Transfer t;
    if (!DecodeTransfer(p, n, &t)) { ++r.payload_parse_error; return false; }

    // Replay is deterministic and in submit order, so anything the live path
    // accepted must be accepted again here. A rejection can only mean the bytes
    // changed underneath us.
    switch (Validate(t)) {
      case SubmitStatus::kOk:
        break;
      case SubmitStatus::kDuplicate:
        ++r.duplicate_key_in_log; return false;
      case SubmitStatus::kBadPostingCount:
      case SubmitStatus::kUnbalanced:
        ++r.unbalanced_in_log; return false;
      case SubmitStatus::kInsufficientFunds:
        ++r.negative_balance_in_log; return false;
    }
    Apply(t);
    ++r.records_applied;
    return true;
  };

  const ScanResult sr = ScanLog(dev_, opts_.verify_crc, visitor);
  r.stop = sr.stop;
  r.valid_bytes = sr.valid_bytes;

  // Append after the last record that verified. Everything past that point is
  // torn, missing or unverifiable, and gets overwritten rather than trusted.
  writer_.emplace(dev_, sr.valid_bytes);
}

// ---------------------------------------------------------------------------
// Submit -- the ack point per mode is the entire experiment
// ---------------------------------------------------------------------------
void Ledger::SyncNow() {
  writer_->Sync();
  unsynced_ = false;
  since_sync_ = 0;
}

void Ledger::AckPending() {
  if (ack_sink_) {
    for (uint64_t key : pending_acks_) ack_sink_(key);
  }
  pending_acks_.clear();
}

SubmitStatus Ledger::Submit(const Transfer& t) {
  const SubmitStatus st = Validate(t);
  if (st != SubmitStatus::kOk) return st;  // never reaches the log

  scratch_.clear();
  EncodeTransfer(scratch_, t);
  writer_->Append(kRecTransfer, scratch_.data(),
                  static_cast<uint32_t>(scratch_.size()));

  // In-memory state moves now, so later transfers in the same batch validate
  // against it. A crash simply discards the state along with the un-acked
  // records that produced it.
  Apply(t);
  pending_acks_.push_back(t.idem_key);
  unsynced_ = true;

  switch (opts_.mode) {
    case DurabilityMode::kNoSync:
      writer_->Flush();          // page cache only; no fsync, ever
      AckPending();
      break;

    case DurabilityMode::kLazySync:
      writer_->Flush();
      AckPending();              // ack BEFORE the data is durable -- the gap
      if (++since_sync_ >= opts_.group_size) SyncNow();
      break;

    case DurabilityMode::kSyncEvery:
      writer_->Flush();
      SyncNow();
      AckPending();              // ack strictly after the platter
      break;

    case DurabilityMode::kGroupCommit:
      if (pending_acks_.size() >= opts_.group_size) {
        writer_->Flush();        // one WriteAt for the whole batch
        SyncNow();               // one fsync
        AckPending();            // then everyone
      }
      break;
  }
  return SubmitStatus::kOk;
}

void Ledger::Commit() {
  writer_->Flush();
  // kNoSync never fsyncs, not even on a clean shutdown. Letting it sync here
  // would quietly turn it into a correct mode and destroy the comparison.
  // The unsynced_ guard keeps Commit() from spending a redundant fsync when
  // everything is already on the platter.
  if (opts_.mode != DurabilityMode::kNoSync && unsynced_) SyncNow();
  AckPending();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
int64_t Ledger::Balance(uint64_t account) const {
  const auto it = balances_.find(account);
  return it == balances_.end() ? 0 : it->second;
}

int64_t Ledger::TotalBalance() const {
  int64_t total = 0;
  for (const auto& [account, bal] : balances_) {
    (void)account;
    total += bal;
  }
  return total;
}

}  // namespace il
