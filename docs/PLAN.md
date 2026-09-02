# Development plan

Agreed before implementation started. Ten steps, each ending in a green build
and a real commit. The organising principle: **no step depends on a later step
being correct**, and every step is independently verifiable. That matters more
than usual here, because a bug in the ledger would silently invalidate every
number the campaign produces.

## Status

- [x] **0.** Storage core — `crc32c`, `Device`, `wal`
- [x] **1.** Transfer wire format
- [x] **2.** Ledger domain + recovery-on-open
- [x] **3.** Four durability modes + ack sink
- [x] **4.** `tools/workload.h`
- [x] **5.** `tools/crash_sim.cpp` — the campaign
- [ ] **6.** `kill_driver` + `kill_worker`
- [ ] **7.** `tools/bench.cpp`
- [ ] **8.** Run experiments, commit CSVs + charts
- [ ] **9.** `findings.md`, real numbers into the docs
- [ ] **10.** CI + clean-room verification

## The steps

### A. Ledger

| # | Deliverable | Verified by |
|---|---|---|
| 1 | `u64 key \| u32 n \| n×(u64 acct, i64 amt)` encode/decode | round-trip; truncated / oversized / trailing-garbage payloads rejected |
| 2 | accounts, world account, validation, `RecoveryReport` | balances, dup key no-op, insufficient funds, unbalanced never logged, recovery reproduces state exactly |
| 3 | `kNoSync / kLazySync / kSyncEvery / kGroupCommit`, ack sink | ack-point assertions per mode: exactly which records are durable at the instant the ack fires |

### B. Harnesses

| # | Deliverable | Verified by |
|---|---|---|
| 4 | deterministic seeded transfer stream, dense keys `1,2,3…` | same seed → identical stream; nothing ever rejected |
| 5 | two-pass mid-commit crash injection, 5 checks, 3-way classification, CLI + 8-config sweep | self-checking: exits non-zero if `sync_every` / `group_commit` ever lose an ack |
| 6 | fork → SIGKILL → recover; raw 8-byte unbuffered acks on fd 1 | same invariants as step 5 |
| 7 | submit→ack latency, p50/p99/p99.9/max, fsync counts, `--groups` sweep | sanity: `sync_every` fsync count == record count |

### C. Results

| # | Deliverable | Verified by |
|---|---|---|
| 8 | `run_experiments.sh`, `plot.py`, committed CSVs + SVGs | campaign exit code 0 |
| 9 | `findings.md`; real numbers into README + study guide, status banners removed | every number traceable to a committed CSV |
| 10 | `ci.yml`: build, unit tests, short campaign, ASan build | fresh clone → `cmake -B build` → tests pass |

Step 5 is the centrepiece and gets disproportionate effort. The two-pass
structure (count writes → pick index in `[1, W+1]` → replay identically,
snapshot at the post-write hook) is where the experiment either measures
something or silently measures nothing.

## Checkpoints

Stop and report at **step 3** (last point where a design change is cheap) and
**step 5** (first real numbers — where we find out if the hypothesis holds).

## Standing assumptions

1. **No tuning to hit the predicted numbers.** The `sync_every` /
   `group_commit` zeros are hard invariants, asserted in code. But
   `no_sync ≈ 20003` and `lazy_sync ≈ 4588` are *emergent* from trial count,
   ops per trial, and `p_persist` / `p_tear`. Reproduce the **shape** — no_sync
   catastrophic, lazy_sync several-fold better but still thousands, silent
   corruption only ever with `crc=0` — and report the actual counts whatever
   they are. Reverse-engineering parameters to hit 20003 would make the
   campaign a self-fulfilling prophecy.
2. **Results provenance.** `crash_sim` is fully simulated and seeded, so its
   numbers are deterministic and portable; generated locally and committed.
   `bench` and the SIGKILL campaign are platform-sensitive and get labelled
   with the machine and OS that produced them.
3. `plot.py` fails gracefully without matplotlib, and the SVGs are committed so
   a reader never needs to run it.
4. **Development is on macOS; the build claim is Ubuntu.** Everything stays
   POSIX-only, and step 10's CI is what substantiates the Ubuntu claim. Until
   it is green, the README does not assert it.
5. `--ops` per trial stays small (tens, not thousands) so 8 configs × 10,000
   trials × 2 passes stays in the seconds-to-minutes range.

## Known risk

Step 5 may show `lazy_sync` losing far more or far less than expected,
depending on how the sync interval N interacts with crash timing. If the number
comes out qualitatively different from the prediction, report the number and
the reasoning rather than adjusting N until it matches.
