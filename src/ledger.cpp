#include "il/ledger.h"

#include <cstring>

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

}  // namespace il
