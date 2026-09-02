// The power-loss campaign. This is the centrepiece of the repo.
//
// Per trial: build a ledger on a SimDevice, run the deterministic workload, cut
// the power *mid-commit*, recover from the resulting bytes, and check five
// things. Everything is seeded, so any violation replays exactly with
// --seed X --trials 1.
//
// WHERE THE CUT LANDS IS THE WHOLE EXPERIMENT.
//
// The obvious harness runs the workload and then cuts the power. But "after a
// submit returned" is a quiescent moment: buffers flushed, records whole,
// nothing in flight. Cut only there and you never produce a single torn record,
// and the checksum arm of the experiment then compares crc=1 against crc=0 on
// data that is never corrupted -- reporting, with great confidence, nothing at
// all.
//
// So the cut has to land after a device write and before the fsync that
// follows it. Two passes:
//
//   pass 1: run the workload, count device writes            -> W
//           choose a crash index uniformly from [1, W+1]
//   pass 2: replay identically (same seeds); on write #k the post-write hook
//           fires, snapshots PowerCut() and how many acks had fired by then
//
// The range is [1, W+1], not [1, W]: the extra slot is the quiescent crash,
// which should be sampled -- just not exclusively.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "il/device.h"
#include "il/ledger.h"
#include "il/wal.h"
#include "workload.h"

using namespace il;

namespace {

// Unwinds pass 2 the instant the snapshot is taken. The ledger it tears through
// is discarded immediately afterwards, so its half-finished state does not
// matter -- and stopping here keeps a 10,000-trial campaign fast.
struct CrashSignal {};

struct Config {
  DurabilityMode mode = DurabilityMode::kNoSync;
  bool crc = true;
};

struct Params {
  uint64_t trials = 10000;
  uint32_t ops = 64;
  uint64_t seed = 1;
  uint32_t group = 8;
  uint32_t accounts = 8;
  double p_persist = 0.5;
  double p_tear = 0.2;
  bool quiet = false;
  const char* csv = nullptr;
};

enum class Outcome { kClean, kTornAccepted, kSilentCorruption };

struct Tally {
  uint64_t trials = 0;
  uint64_t acks = 0;            // acknowledged before the cut, summed
  uint64_t acks_lost = 0;       // ...and then not found after recovery
  uint64_t clean = 0;
  uint64_t torn_accepted = 0;
  uint64_t silent_corruption = 0;
  uint64_t prefix_violations = 0;
  uint64_t state_mismatches = 0;
  uint64_t conservation_violations = 0;
};

LedgerOptions MakeOptions(const Config& c, const Params& p) {
  LedgerOptions o;
  o.mode = c.mode;
  o.verify_crc = c.crc;
  o.group_size = p.group;
  return o;
}

WorkloadOptions MakeWorkload(const Params& p, uint64_t seed) {
  WorkloadOptions w;
  w.seed = seed;
  w.accounts = p.accounts;
  return w;
}

// Balances produced by cleanly replaying the first |n| transfers of the stream.
// The recovered state has to equal this exactly -- it is not enough for
// recovery to produce *a* plausible ledger, it must produce *the* right one.
std::map<uint64_t, int64_t> CleanReplay(const Params& p, uint64_t seed, uint64_t n) {
  SimDevice dev;
  LedgerOptions o;
  o.mode = DurabilityMode::kSyncEvery;
  Ledger l(dev, o);
  Workload w(MakeWorkload(p, seed));
  for (uint64_t i = 0; i < n; ++i) l.Submit(w.Next());
  return l.balances();
}

// One trial. Returns the outcome and fills in the per-trial counters.
Outcome RunTrial(const Config& cfg, const Params& p, uint64_t trial_seed,
                 uint64_t* acks_out, uint64_t* lost_out, bool* prefix_bad,
                 bool* state_bad, bool* conservation_bad) {
  const LedgerOptions opts = MakeOptions(cfg, p);

  // ---- pass 1: how many device writes does this workload perform? ----
  uint64_t writes = 0;
  {
    SimDevice dev;
    Ledger l(dev, opts);
    Workload w(MakeWorkload(p, trial_seed));
    for (uint32_t i = 0; i < p.ops; ++i) l.Submit(w.Next());
    writes = dev.write_count();
  }

  // Uniform over [1, writes + 1]; the last slot is the quiescent crash.
  std::mt19937_64 pick(trial_seed ^ 0x9E3779B97F4A7C15ull);
  const uint64_t crash_at = 1 + pick() % (writes + 1);

  // ---- pass 2: identical replay, cut the power at write #crash_at ----
  std::vector<uint8_t> image;
  std::vector<uint64_t> acked;
  uint64_t acks_at_cut = 0;
  {
    SimDevice dev;
    std::mt19937_64 cut_rng(trial_seed * 6364136223846793005ull + 1);
    CutPolicy policy;
    policy.p_persist = p.p_persist;
    policy.p_tear = p.p_tear;

    uint64_t seen = 0;
    bool captured = false;
    dev.set_post_write_hook([&](const SimDevice& d) {
      if (captured) return;
      if (++seen == crash_at) {
        // Right here: the write has landed in the volatile cache, and the fsync
        // that would make it durable has not run yet.
        image = d.PowerCut(cut_rng, policy);
        acks_at_cut = acked.size();
        captured = true;
        throw CrashSignal{};
      }
    });

    Ledger l(dev, opts);
    l.set_ack_sink([&](uint64_t key) { acked.push_back(key); });
    Workload w(MakeWorkload(p, trial_seed));
    try {
      for (uint32_t i = 0; i < p.ops; ++i) l.Submit(w.Next());
    } catch (const CrashSignal&) {
      // snapshot taken inside the hook
    }
    if (!captured) {  // crash_at == writes + 1: the quiescent case
      image = dev.PowerCut(cut_rng, policy);
      acks_at_cut = acked.size();
    }
  }

  // ---- recover from the post-crash bytes ----
  SimDevice after(image);
  RecoveryReport report;
  Ledger recovered(after, opts, &report);

  // 1. Durability: every acknowledged key must still be there.
  uint64_t lost = 0;
  for (uint64_t i = 0; i < acks_at_cut; ++i) {
    if (!recovered.HasKey(acked[i])) ++lost;
  }
  *acks_out = acks_at_cut;
  *lost_out = lost;

  // 2. Prefix: the workload issues dense keys, so the recovered sequence must
  //    be exactly 1..k. No phantoms, no holes, no reordering.
  const std::vector<uint64_t>& keys = recovered.applied_keys();
  bool prefix_ok = keys.size() <= p.ops;
  for (size_t i = 0; i < keys.size() && prefix_ok; ++i) {
    prefix_ok = keys[i] == i + 1;
  }
  *prefix_bad = !prefix_ok;

  // 3. State: balances must equal a clean replay of that same prefix.
  bool state_ok = false;
  if (prefix_ok) {
    state_ok = recovered.balances() == CleanReplay(p, trial_seed, keys.size());
  }
  *state_bad = !state_ok;

  // 4. Conservation: the free invariant.
  const bool conservation_ok = recovered.ConservationHolds();
  *conservation_bad = !conservation_ok;

  // 5. Classify. Durability loss is NOT corruption -- losing unacknowledged
  //    work is expected for the unsafe modes. This axis is about integrity.
  const bool integrity_violated = !prefix_ok || !state_ok || !conservation_ok;
  if (report.corruption_admitted()) {
    // Corruption reached the domain layer: a ledger rule caught what the
    // checksum layer let through.
    return Outcome::kTornAccepted;
  }
  if (integrity_violated) {
    // Nothing caught it. Recovery reported success and the state is wrong.
    return Outcome::kSilentCorruption;
  }
  return Outcome::kClean;
}

Tally RunCampaign(const Config& cfg, const Params& p) {
  Tally t;
  for (uint64_t i = 0; i < p.trials; ++i) {
    const uint64_t trial_seed = p.seed + i;
    uint64_t acks = 0, lost = 0;
    bool prefix_bad = false, state_bad = false, conservation_bad = false;
    const Outcome o = RunTrial(cfg, p, trial_seed, &acks, &lost, &prefix_bad,
                               &state_bad, &conservation_bad);
    ++t.trials;
    t.acks += acks;
    t.acks_lost += lost;
    t.prefix_violations += prefix_bad ? 1 : 0;
    t.state_mismatches += state_bad ? 1 : 0;
    t.conservation_violations += conservation_bad ? 1 : 0;
    switch (o) {
      case Outcome::kClean: ++t.clean; break;
      case Outcome::kTornAccepted: ++t.torn_accepted; break;
      case Outcome::kSilentCorruption: ++t.silent_corruption; break;
    }
  }
  return t;
}

void Usage() {
  std::printf(
      "usage: crash_sim [options]\n"
      "  --trials N      trials per configuration (default 10000)\n"
      "  --ops N         transfers per trial (default 64)\n"
      "  --seed N        base seed (default 1)\n"
      "  --group N       group-commit batch / lazy-sync interval (default 8)\n"
      "  --accounts N    accounts in the workload (default 8)\n"
      "  --mode M        no_sync|lazy_sync|sync_every|group_commit\n"
      "                  (omitted: sweep all four, crc on and off)\n"
      "  --crc 0|1       checksums (default: both when --mode is omitted)\n"
      "  --p-persist F   per-sector probability of surviving the cut (0.5)\n"
      "  --p-tear F      per-sector probability of tearing (0.2)\n"
      "  --csv PATH      append results as CSV\n"
      "  --quiet         only print the summary table\n");
}

}  // namespace

int main(int argc, char** argv) {
  Params p;
  bool have_mode = false, have_crc = false;
  DurabilityMode only_mode = DurabilityMode::kNoSync;
  bool only_crc = true;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
    auto need = [&](const char* what) -> const char* {
      if (!next) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
      ++i;
      return next;
    };
    if (a == "--trials") p.trials = std::strtoull(need("--trials"), nullptr, 10);
    else if (a == "--ops") p.ops = static_cast<uint32_t>(std::strtoul(need("--ops"), nullptr, 10));
    else if (a == "--seed") p.seed = std::strtoull(need("--seed"), nullptr, 10);
    else if (a == "--group") p.group = static_cast<uint32_t>(std::strtoul(need("--group"), nullptr, 10));
    else if (a == "--accounts") p.accounts = static_cast<uint32_t>(std::strtoul(need("--accounts"), nullptr, 10));
    else if (a == "--p-persist") p.p_persist = std::strtod(need("--p-persist"), nullptr);
    else if (a == "--p-tear") p.p_tear = std::strtod(need("--p-tear"), nullptr);
    else if (a == "--csv") p.csv = need("--csv");
    else if (a == "--quiet") p.quiet = true;
    else if (a == "--mode") {
      if (!ParseDurabilityMode(need("--mode"), &only_mode)) {
        std::fprintf(stderr, "unknown mode\n");
        return 2;
      }
      have_mode = true;
    } else if (a == "--crc") {
      only_crc = std::strtol(need("--crc"), nullptr, 10) != 0;
      have_crc = true;
    } else if (a == "--help" || a == "-h") {
      Usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      Usage();
      return 2;
    }
  }

  if (p.p_persist < 0.0 || p.p_tear < 0.0 || p.p_persist + p.p_tear > 1.0) {
    std::fprintf(stderr, "require p_persist >= 0, p_tear >= 0, p_persist + p_tear <= 1\n");
    return 2;
  }

  std::vector<Config> configs;
  const DurabilityMode all[] = {DurabilityMode::kNoSync, DurabilityMode::kLazySync,
                                DurabilityMode::kSyncEvery, DurabilityMode::kGroupCommit};
  for (bool crc : {true, false}) {
    if (have_crc && crc != only_crc) continue;
    for (DurabilityMode m : all) {
      if (have_mode && m != only_mode) continue;
      configs.push_back({m, crc});
    }
  }

  std::FILE* csv = nullptr;
  if (p.csv) {
    const bool fresh = std::fopen(p.csv, "r") == nullptr;
    csv = std::fopen(p.csv, "a");
    if (!csv) { std::perror("csv"); return 2; }
    if (fresh) {
      std::fprintf(csv,
                   "mode,crc,trials,ops,group,p_persist,p_tear,seed,acks,acks_lost,"
                   "clean,torn_accepted,silent_corruption,prefix_violations,"
                   "state_mismatches,conservation_violations\n");
    }
  }

  if (!p.quiet) {
    std::printf("ironledger crash campaign: %" PRIu64 " trials x %u ops, "
                "p_persist=%.2f p_tear=%.2f group=%u seed=%" PRIu64 "\n\n",
                p.trials, p.ops, p.p_persist, p.p_tear, p.group, p.seed);
  }

  int exit_code = 0;
  for (const Config& c : configs) {
    const Tally t = RunCampaign(c, p);

    std::printf("%-13s crc=%d   ACKS LOST=%-7" PRIu64 " / %-7" PRIu64
                "  silent-corruption=%-5" PRIu64 " torn-accepted=%" PRIu64 "\n",
                DurabilityModeName(c.mode), c.crc ? 1 : 0, t.acks_lost, t.acks,
                t.silent_corruption, t.torn_accepted);

    if (csv) {
      std::fprintf(csv,
                   "%s,%d,%" PRIu64 ",%u,%u,%.4f,%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                   ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                   DurabilityModeName(c.mode), c.crc ? 1 : 0, t.trials, p.ops, p.group,
                   p.p_persist, p.p_tear, p.seed, t.acks, t.acks_lost, t.clean,
                   t.torn_accepted, t.silent_corruption, t.prefix_violations,
                   t.state_mismatches, t.conservation_violations);
    }

    // The regression test. sync_every and group_commit acknowledge only after
    // the data is on the platter, so an acknowledged transfer cannot be lost --
    // in either checksum arm. If one ever is, that is a bug, and CI should go
    // red rather than the number quietly appearing in a table.
    const bool must_be_durable = c.mode == DurabilityMode::kSyncEvery ||
                                 c.mode == DurabilityMode::kGroupCommit;
    if (must_be_durable && t.acks_lost != 0) {
      std::fprintf(stderr,
                   "FAIL: %s with crc=%d lost %" PRIu64 " acknowledged transfers; "
                   "replay with --seed %" PRIu64 " --trials 1 --mode %s --crc %d\n",
                   DurabilityModeName(c.mode), c.crc ? 1 : 0, t.acks_lost, p.seed,
                   DurabilityModeName(c.mode), c.crc ? 1 : 0);
      exit_code = 1;
    }
    // Conservation is not negotiable for anyone: money is never created.
    if (t.conservation_violations != 0) {
      std::fprintf(stderr, "FAIL: %s crc=%d broke conservation in %" PRIu64 " trials\n",
                   DurabilityModeName(c.mode), c.crc ? 1 : 0, t.conservation_violations);
      exit_code = 1;
    }
  }

  if (csv) std::fclose(csv);
  return exit_code;
}
