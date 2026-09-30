// The victim process for the SIGKILL campaign.
//
// Runs the shared deterministic workload against a REAL file with real fsyncs,
// and reports every acknowledgement to fd 1 as 8 raw little-endian bytes via
// write(2). It expects to be killed at an arbitrary moment and makes no attempt
// to shut down cleanly.
//
// The acks are written UNBUFFERED, and that detail is load-bearing. With stdio
// they would sit in a FILE* buffer and die with the process, so the parent would
// believe fewer transfers had been promised than actually were -- and would then
// fail to check exactly the transfers most at risk. The harness would
// under-report losses and make a broken system look correct, which is the worst
// direction for a testing bug to fail in.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include "il/device.h"
#include "il/ledger.h"
#include "workload.h"

using namespace il;

namespace {

// Writes exactly 8 bytes to fd 1, looping on short writes and retrying EINTR.
// The parent's record of what was promised must never lag reality.
void EmitAck(uint64_t key) {
  uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(key >> (8 * i));
  size_t off = 0;
  while (off < sizeof(b)) {
    const ssize_t n = ::write(STDOUT_FILENO, b + off, sizeof(b) - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      _exit(3);  // the parent can no longer trust its own record; die loudly
    }
    off += static_cast<size_t>(n);
  }
}

[[noreturn]] void Die(const char* msg) {
  std::fprintf(stderr, "kill_worker: %s\n", msg);
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = nullptr;
  LedgerOptions opts;
  WorkloadOptions wopts;
  uint64_t ops = 2'000'000;  // effectively "until killed"

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const char* next = (i + 1 < argc) ? argv[++i] : nullptr;
    if (!next) Die("missing value");
    if (a == "--path") path = next;
    else if (a == "--mode") { if (!ParseDurabilityMode(next, &opts.mode)) Die("bad --mode"); }
    else if (a == "--crc") opts.verify_crc = std::strtol(next, nullptr, 10) != 0;
    else if (a == "--group") opts.group_size = static_cast<uint32_t>(std::strtoul(next, nullptr, 10));
    else if (a == "--seed") wopts.seed = std::strtoull(next, nullptr, 10);
    else if (a == "--accounts") wopts.accounts = static_cast<uint32_t>(std::strtoul(next, nullptr, 10));
    else if (a == "--ops") ops = std::strtoull(next, nullptr, 10);
    else Die("unknown argument");
  }
  if (!path) Die("--path is required");

  FileDevice dev(path, /*truncate=*/true);
  Ledger ledger(dev, opts);
  ledger.set_ack_sink(EmitAck);

  Workload w(wopts);
  for (uint64_t i = 0; i < ops; ++i) {
    if (ledger.Submit(w.Next()) != SubmitStatus::kOk) Die("workload was rejected");
  }

  // Only reached if the kill never arrived.
  ledger.Commit();
  return 0;
}
