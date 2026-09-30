// The SIGKILL campaign -- and the control that proves why the simulator exists.
//
// Fork the worker against a real file, let it run for 0.2-25ms, SIGKILL it,
// recover the file it left behind, and check the same invariants the power-loss
// campaign checks.
//
// EXPECT THIS TO PASS. Even for no_sync, which never calls fsync at all.
//
// SIGKILL destroys a process; it does not touch the kernel page cache. Every
// byte the dying worker had written is still in kernel memory, and the kernel
// flushes it to disk at its leisure, entirely indifferent to the fact that the
// process that produced it no longer exists. The data lands. Recovery finds it.
// Everything looks fine.
//
// That is not a bug in this harness. It is the finding. A SIGKILL campaign
// proves your recovery code can parse a truncated log; it says nothing about
// durability, because process death is not the failure mode that loses data.
// Compare this output against crash_sim's: same ledger, same modes, same
// invariants, wildly different answers.
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "il/device.h"
#include "il/ledger.h"
#include "workload.h"

using namespace il;

namespace {

struct Params {
  uint64_t trials = 250;
  uint64_t seed = 1;
  uint32_t group = 8;
  uint32_t accounts = 8;
  double min_ms = 0.2;
  double max_ms = 25.0;
  const char* worker = nullptr;
  const char* dir = nullptr;
  const char* csv = nullptr;
  bool quiet = false;
};

struct Tally {
  uint64_t trials = 0;
  uint64_t killed = 0;       // died to our signal rather than finishing
  uint64_t acks = 0;
  uint64_t acks_lost = 0;
  uint64_t prefix_violations = 0;
  uint64_t state_mismatches = 0;
  uint64_t conservation_violations = 0;
  uint64_t corruption_admitted = 0;
};

void SleepMs(double ms) {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ms / 1000.0);
  ts.tv_nsec = static_cast<long>((ms - static_cast<double>(ts.tv_sec) * 1000.0) * 1e6);
  ::nanosleep(&ts, nullptr);
}

double NowMs() {
  struct timespec ts;
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

// Drains whatever the worker has written so far without blocking, appending
// whole 8-byte keys to |acked| and carrying any partial key in |carry|.
void Drain(int fd, std::vector<uint8_t>& carry, std::vector<uint64_t>& acked) {
  uint8_t buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      carry.insert(carry.end(), buf, buf + n);
      size_t off = 0;
      while (carry.size() - off >= 8) {
        uint64_t key = 0;
        for (int i = 0; i < 8; ++i) key |= static_cast<uint64_t>(carry[off + i]) << (8 * i);
        acked.push_back(key);
        off += 8;
      }
      carry.erase(carry.begin(), carry.begin() + static_cast<long>(off));
      continue;
    }
    if (n == 0) return;  // worker's end closed
    if (errno == EINTR) continue;
    return;              // EAGAIN: nothing available right now
  }
}

std::map<uint64_t, int64_t> CleanReplay(const Params& p, uint64_t seed, uint64_t n) {
  SimDevice dev;
  LedgerOptions o;
  o.mode = DurabilityMode::kSyncEvery;
  Ledger l(dev, o);
  WorkloadOptions w;
  w.seed = seed;
  w.accounts = p.accounts;
  Workload gen(w);
  for (uint64_t i = 0; i < n; ++i) l.Submit(gen.Next());
  return l.balances();
}

bool RunTrial(const Params& p, DurabilityMode mode, bool crc, uint64_t trial_seed,
              const std::string& path, Tally* t) {
  int fds[2];
  if (::pipe(fds) != 0) { std::perror("pipe"); return false; }

  const pid_t pid = ::fork();
  if (pid < 0) { std::perror("fork"); ::close(fds[0]); ::close(fds[1]); return false; }

  if (pid == 0) {
    // Child: acks go down the pipe as fd 1.
    ::close(fds[0]);
    if (::dup2(fds[1], STDOUT_FILENO) < 0) _exit(4);
    ::close(fds[1]);

    char s_seed[32], s_group[32], s_crc[8], s_acct[32];
    std::snprintf(s_seed, sizeof(s_seed), "%" PRIu64, trial_seed);
    std::snprintf(s_group, sizeof(s_group), "%u", p.group);
    std::snprintf(s_crc, sizeof(s_crc), "%d", crc ? 1 : 0);
    std::snprintf(s_acct, sizeof(s_acct), "%u", p.accounts);

    const char* args[] = {p.worker, "--path", path.c_str(),
                          "--mode", DurabilityModeName(mode),
                          "--crc", s_crc, "--group", s_group,
                          "--seed", s_seed, "--accounts", s_acct, nullptr};
    ::execv(p.worker, const_cast<char* const*>(args));
    _exit(5);  // exec failed
  }

  // Parent.
  ::close(fds[1]);
  ::fcntl(fds[0], F_SETFL, O_NONBLOCK);

  std::mt19937_64 rng(trial_seed ^ 0xD1B54A32D192ED03ull);
  const double run_ms =
      p.min_ms + (p.max_ms - p.min_ms) *
                     (static_cast<double>(rng() >> 11) / 9007199254740992.0);

  std::vector<uint8_t> carry;
  std::vector<uint64_t> acked;

  // Drain while waiting, so a fast worker never blocks on a full pipe -- that
  // would stall it and quietly change the workload under measurement.
  const double deadline = NowMs() + run_ms;
  while (NowMs() < deadline) {
    Drain(fds[0], carry, acked);
    SleepMs(0.05);
  }

  const bool killed = ::kill(pid, SIGKILL) == 0;

  int status = 0;
  ::waitpid(pid, &status, 0);
  Drain(fds[0], carry, acked);  // whatever was still in the pipe
  ::close(fds[0]);

  ++t->trials;
  if (killed && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) ++t->killed;

  // ---- recover the file the worker left behind ----
  LedgerOptions opts;
  opts.mode = mode;
  opts.verify_crc = crc;
  opts.group_size = p.group;

  FileDevice dev(path);
  RecoveryReport report;
  Ledger recovered(dev, opts, &report);

  for (uint64_t key : acked) {
    if (!recovered.HasKey(key)) ++t->acks_lost;
  }
  t->acks += acked.size();
  if (report.corruption_admitted()) ++t->corruption_admitted;

  const std::vector<uint64_t>& keys = recovered.applied_keys();
  bool prefix_ok = true;
  for (size_t i = 0; i < keys.size() && prefix_ok; ++i) prefix_ok = keys[i] == i + 1;
  if (!prefix_ok) ++t->prefix_violations;

  if (prefix_ok) {
    if (recovered.balances() != CleanReplay(p, trial_seed, keys.size())) {
      ++t->state_mismatches;
    }
  }
  if (!recovered.ConservationHolds()) ++t->conservation_violations;
  return true;
}

void Usage() {
  std::printf(
      "usage: kill_driver [options]\n"
      "  --trials N     trials per mode (default 250)\n"
      "  --seed N       base seed (default 1)\n"
      "  --group N      group-commit batch / lazy-sync interval (default 8)\n"
      "  --accounts N   accounts in the workload (default 8)\n"
      "  --min-ms F     shortest run before the kill (default 0.2)\n"
      "  --max-ms F     longest run before the kill (default 25)\n"
      "  --worker PATH  kill_worker binary (default: next to this one)\n"
      "  --dir PATH     where to put the log files (default $TMPDIR)\n"
      "  --csv PATH     append results as CSV\n"
      "  --quiet        only print the summary table\n");
}

}  // namespace

int main(int argc, char** argv) {
  Params p;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--quiet") { p.quiet = true; continue; }
    if (a == "--help" || a == "-h") { Usage(); return 0; }
    const char* next = (i + 1 < argc) ? argv[++i] : nullptr;
    if (!next) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); return 2; }
    if (a == "--trials") p.trials = std::strtoull(next, nullptr, 10);
    else if (a == "--seed") p.seed = std::strtoull(next, nullptr, 10);
    else if (a == "--group") p.group = static_cast<uint32_t>(std::strtoul(next, nullptr, 10));
    else if (a == "--accounts") p.accounts = static_cast<uint32_t>(std::strtoul(next, nullptr, 10));
    else if (a == "--min-ms") p.min_ms = std::strtod(next, nullptr);
    else if (a == "--max-ms") p.max_ms = std::strtod(next, nullptr);
    else if (a == "--worker") p.worker = next;
    else if (a == "--dir") p.dir = next;
    else if (a == "--csv") p.csv = next;
    else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); Usage(); return 2; }
  }

  // Default to the kill_worker sitting beside this binary.
  std::string worker_path;
  if (p.worker) {
    worker_path = p.worker;
  } else {
    const std::string self = argv[0];
    const size_t slash = self.find_last_of('/');
    worker_path = (slash == std::string::npos) ? "kill_worker"
                                               : self.substr(0, slash + 1) + "kill_worker";
  }
  p.worker = worker_path.c_str();
  if (::access(p.worker, X_OK) != 0) {
    std::fprintf(stderr, "cannot execute worker at %s (use --worker)\n", p.worker);
    return 2;
  }

  const char* tmp = p.dir ? p.dir : (std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
  std::string dir = tmp;
  if (!dir.empty() && dir.back() == '/') dir.pop_back();

  std::FILE* csv = nullptr;
  if (p.csv) {
    const bool fresh = std::fopen(p.csv, "r") == nullptr;
    csv = std::fopen(p.csv, "a");
    if (!csv) { std::perror("csv"); return 2; }
    if (fresh) {
      std::fprintf(csv,
                   "mode,crc,trials,killed,acks,acks_lost,prefix_violations,"
                   "state_mismatches,conservation_violations,corruption_admitted\n");
    }
  }

  if (!p.quiet) {
    std::printf("ironledger SIGKILL campaign: %" PRIu64 " trials per mode, "
                "kill after %.1f-%.1fms, group=%u\n"
                "real file, real fsyncs, in %s\n\n",
                p.trials, p.min_ms, p.max_ms, p.group, dir.c_str());
  }

  int exit_code = 0;
  const DurabilityMode modes[] = {DurabilityMode::kNoSync, DurabilityMode::kLazySync,
                                  DurabilityMode::kSyncEvery, DurabilityMode::kGroupCommit};
  for (DurabilityMode mode : modes) {
    Tally t;
    for (uint64_t i = 0; i < p.trials; ++i) {
      char name[256];
      std::snprintf(name, sizeof(name), "%s/ironledger_kill_%d_%" PRIu64 ".log",
                    dir.c_str(), static_cast<int>(::getpid()), i);
      const std::string path = name;
      if (!RunTrial(p, mode, /*crc=*/true, p.seed + i, path, &t)) { exit_code = 2; break; }
      ::unlink(path.c_str());
    }

    std::printf("%-13s crc=1   ACKS LOST=%-7" PRIu64 " / %-8" PRIu64
                " killed=%" PRIu64 "/%" PRIu64 "  prefix-violations=%" PRIu64
                "  state-mismatches=%" PRIu64 "\n",
                DurabilityModeName(mode), t.acks_lost, t.acks, t.killed, t.trials,
                t.prefix_violations, t.state_mismatches);

    if (csv) {
      std::fprintf(csv, "%s,1,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                   DurabilityModeName(mode), t.trials, t.killed, t.acks, t.acks_lost,
                   t.prefix_violations, t.state_mismatches, t.conservation_violations,
                   t.corruption_admitted);
    }

    // These two must hold under any crash model. Note what is NOT asserted:
    // that no_sync loses acks. Under SIGKILL it does not, and that is the point.
    if (mode == DurabilityMode::kSyncEvery || mode == DurabilityMode::kGroupCommit) {
      if (t.acks_lost != 0) {
        std::fprintf(stderr, "FAIL: %s lost %" PRIu64 " acknowledged transfers\n",
                     DurabilityModeName(mode), t.acks_lost);
        exit_code = 1;
      }
    }
    if (t.conservation_violations != 0 || t.prefix_violations != 0) {
      std::fprintf(stderr, "FAIL: %s broke conservation (%" PRIu64 ") or prefix (%" PRIu64 ")\n",
                   DurabilityModeName(mode), t.conservation_violations, t.prefix_violations);
      exit_code = 1;
    }
  }

  if (csv) std::fclose(csv);
  return exit_code;
}
