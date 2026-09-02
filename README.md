# ironledger

**A crash-consistent, event-sourced double-entry ledger in C++20 — and a fault-injection harness that cuts its power ten thousand times to find out which durability shortcuts actually survive.**

No third-party dependencies. CMake, a hand-rolled test harness, and a deterministic power-loss simulator.

```bash
cmake -B build && cmake --build build && ./build/unit_tests
```

> **Status:** the storage core, the ledger, and **the power-loss campaign are complete** — the results table below is measured, not predicted. 49 tests, 87,709 assertions, zero warnings under `-Wall -Wextra -Wpedantic`, clean under ASan + UBSan. The `SIGKILL` campaign and the benchmark are still to come; see [Roadmap](#roadmap).

---

## The thing I want you to read

Here is the most useful lesson in this repository, and it is a negative result.

The obvious way to crash-test a storage system is to kill it:

```bash
./worker & sleep 0.01; kill -9 $!    # a few hundred times
```

Do that to a ledger that **never calls `fsync` at all**, and it passes. Perfect score. Zero lost transactions, hundreds of kills.

That is not a bug in the harness. **That is the finding.**

*(The `SIGKILL` campaign is the one piece not yet built — this claim is the prediction it exists to test, and the number goes here when it runs. The power-loss half **is** measured, below, and `no_sync` loses 79% of what it acknowledged. The gap between those two numbers is the entire point.)*

`kill -9` destroys a *process*. It does not touch the **kernel page cache**. Every byte the dying process wrote is still in kernel memory, and the kernel flushes it to disk at its leisure, entirely indifferent to the fact that the process is gone. The data lands. Recovery finds it. Everything looks fine.

> **A `SIGKILL` campaign proves your recovery code can parse a truncated log. It tells you nothing whatsoever about durability, because process death is not the failure mode that loses data.**

To lose data you have to lose the page cache, and no process can ask the kernel to do that. So this repo builds a **simulated block device** where the cache is mine to destroy — and then runs *both* campaigns, precisely so the gap between them is visible in the numbers.

A great many systems that describe themselves as crash-tested have only ever been tested the first way.

---

## The experiment

Four durability policies × checksums on/off × 10,000 simulated power failures each. One number per cell: **transfers the ledger acknowledged and then lost.**

An *acknowledgement* is the moment you tell a client "this money moved." It is a promise you cannot retract. Losing unacknowledged data is disappointing; losing acknowledged data is a broken contract. That distinction is the only thing the table measures.

| mode | ack point | expectation |
|---|---|---|
| `no_sync` | `write()`, ack immediately | loses nearly everything |
| `lazy_sync` | ack immediately, `fsync` every N | **loses thousands — and looks perfect on any machine that never loses power** |
| `sync_every` | `fsync`, *then* ack | zero. Correct, slow. |
| `group_commit` | batch N, one write, one `fsync`, ack all N | zero. Correct, and amortises the `fsync`. |

**The ack point is already tested directly, and this part is measured.** At the instant of every acknowledgement, the test rebuilds a ledger from `stable_image()` — the bytes that would be on the platter — and asks whether the key is there:

| mode | acks backed by the platter | fsyncs for 32 records |
|---|---|---|
| `sync_every` | **30 / 30** | 30 |
| `group_commit` (N=8) | **32 / 32** | **4** |
| `lazy_sync` (N=8) | 0 / 32 | 4 |
| `no_sync` | 0 / 30 | 0 |

Group commit reaches *exactly* the durability of `sync_every` at a quarter of the fsyncs. `lazy_sync` acks ahead of its own `fsync` every single time — the window is `N` records wide, and it is the same `4` fsyncs as group commit, just spent in the wrong order relative to the promise.

`lazy_sync` is the one that matters. It is fast, it passes every test you run on a healthy machine, it passes the `kill -9` campaign flawlessly, and it is the shape of an enormous number of real-world "yes, we have durability" claims. The gap between its ack and its `fsync` is real, and it is exactly N records wide.

### Results: 10,000 power failures per configuration

Measured, reproducible in 6.7 seconds with `./build/crash_sim --trials 10000 --ops 64`:

```
no_sync       crc=1   ACKS LOST=253278 / 320452   silent-corruption=0     torn-accepted=0
lazy_sync     crc=1   ACKS LOST=18053  / 320452   silent-corruption=0     torn-accepted=0
sync_every    crc=1   ACKS LOST=0      / 320452   silent-corruption=0     torn-accepted=0
group_commit  crc=1   ACKS LOST=0      / 321408   silent-corruption=0     torn-accepted=0
no_sync       crc=0   ACKS LOST=251507 / 320452   silent-corruption=1752  torn-accepted=2545
lazy_sync     crc=0   ACKS LOST=18002  / 320452   silent-corruption=49    torn-accepted=300
sync_every    crc=0   ACKS LOST=0      / 320452   silent-corruption=1     torn-accepted=12
group_commit  crc=0   ACKS LOST=0      / 321408   silent-corruption=141   torn-accepted=784
```

**Durability.** `no_sync` loses **79%** of everything it promised. `lazy_sync` — the mode that looks perfect on any machine that never loses power — loses **18,053 acknowledged transfers**, 5.6%. `sync_every` and `group_commit` lose **exactly zero**, in both checksum arms. Not "few". Zero. The campaign exits non-zero if either row is ever non-zero, so this is a regression test, not a demo.

**Group commit is not a compromise.** Same zero as `sync_every`, at a quarter of the fsyncs. Correctness and throughput are not in tension here; `lazy_sync` is simply the wrong optimisation, trading away the one thing you cannot recover for speed you could have had anyway.

**Integrity is a different axis entirely.** Every `crc=1` row has **zero** silent corruption. Every `crc=0` row has some. Look at the bottom row:

```
group_commit  crc=0   ACKS LOST=0      silent-corruption=141
```

Group commit kept **every single promise** — and still handed back silently corrupted balances 141 times, because the records weren't checksummed. Recovery reported success. The data is wrong.

> **Durability and integrity are two different problems, and fixing one does nothing for the other.** `fsync` answers *"did the bytes survive?"* Only a checksum answers *"are they the bytes I wrote?"* A system that fsyncs religiously and skips checksums will faithfully preserve your corruption forever.

**And it reaches the "correct" mode too.** `sync_every crc=0` shows one silent corruption — a case I expected to be zero. It is real and it reproduces:

```bash
./build/crash_sim --mode sync_every --crc 0 --trials 1 --ops 64 --seed 8490   # silent-corruption=1
./build/crash_sim --mode sync_every --crc 1 --trials 1 --ops 64 --seed 8490   # clean
```

Same trial, same crash, 0 acknowledged transfers lost either way. The only difference is the checksum. The record being written when the power failed was never acknowledged, so no promise was broken — but with checksums off it parsed as a valid transfer and got replayed, and the recovered balances no longer match a clean replay of the true prefix. Perfect durability is not a defence against a torn record you never promised anything about.

---

## Design decisions worth defending

Three places where the obvious implementation silently measures nothing, and what I did instead.

### 1. Cut the power mid-commit, not between transactions

The natural harness runs some transfers and then cuts power. But "after a submit returned" is a **quiescent** moment — buffers flushed, records whole, nothing in flight. Cut only there and you will never produce a single torn record. The checksum arm of the experiment then compares `crc=1` against `crc=0` on data that is never corrupted, gets identical results, and reports nothing at all with great confidence.

The cut has to land in the dangerous window: **after a device write, before the `fsync` that follows it.** So `SimDevice` exposes a post-write hook, and the campaign runs two passes:

```
pass 1:  run the workload, count device writes            → W
         choose a crash index uniformly from [1, W+1]
pass 2:  replay identically (same seeds); on write #k the hook fires:
             snapshot PowerCut()  +  how many acks had fired by that instant
```

`[1, W+1]`, not `[1, W]` — the extra slot is the quiescent crash. It should be *sampled*, just not exclusively.

### 2. The LSN is the record's own byte offset, and the scanner checks it

The 24-byte record header is `magic | crc | lsn | type | len`, and `lsn` is the offset the record is stored at. That is load-bearing, not decoration.

After a power cut, sector N can be missing while N+1 survived — reads of a never-written region return zeros, so recovery sees valid data, a hole, then an **intact record with a perfect checksum** sitting after the gap. A scanner that resynchronises onto it replays it as though nothing were missing. Two transfers have vanished, every checksum passed, and nothing logged an error.

The CRC cannot help here: *record #4's bytes were never corrupted.* They were simply not supposed to be reachable yet. **A checksum verifies bytes that are present; it has nothing to say about bytes that are absent.**

With a self-locating LSN the record announces "I live at offset 4096", the scanner is at 2048, they disagree, and the scan stops with `bad_lsn`. Pinned by two tests — `wal_hole_stops_scan_with_bad_magic` (asserted with checksums **both on and off**, because the magic check is structural) and `wal_misplaced_record_stops_scan_with_bad_lsn` (magic passes, CRC passes, only the LSN catches it).

> A recovery routine that salvages as much as possible is more dangerous than one that stops at the first sign of trouble. A loud failure gets escalated to a human; silent partial recovery gets served to customers.

### 3. `Flush()` and `Sync()` are different verbs

```cpp
class LogWriter {
  void Flush();  // buffered bytes → device (page cache).  Survives kill -9.
  void Sync();   // Flush(), then fsync.        Survives power loss.
};
```

One letter of difference in the API; an entire category of bug in between. All four durability modes are just different arrangements of these two calls and the acknowledgement. Collapsing them into one `Commit()` would delete the experiment.

### Also worth a look

- **`SimDevice` resolves each dirty sector independently** — persisted / lost / torn — which is *harsher* than a real drive on purpose. Write ordering is exactly the assumption unsynced logs quietly rely on, and an assumption you never violate is an assumption you never tested.
- **Opening a ledger *is* recovery.** There is no separate repair path that only runs in production at 3am; a fresh device simply replays nothing. The dangerous code runs in every single test.
- **Idempotency keys are rebuilt from the log**, so crash-safe deduplication falls out for free — no second durable structure to drift out of sync with the first.
- **Replay applies the same validation as submit.** Replay is deterministic and in submit order, so a record that validated when written *must* validate again — meaning a rejection during replay can only mean the bytes changed. Ordinary business rules become a second corruption detector underneath the checksums.
- **The zero-sum invariant.** Every transfer's postings sum to zero, so the sum over all accounts is always zero — one `int64_t`, checked everywhere, that detects corruption without knowing anything about what went wrong.
- **Latency is measured submit → ack**, not submit → return. Under group commit the ack arrives several submissions later, and timing only the call would flatter it enormously.

---

## Build and reproduce

Requires CMake ≥ 3.16 and a C++20 compiler. Nothing else.

```bash
cmake -B build && cmake --build build
./build/unit_tests
```

With sanitizers:

```bash
cmake -B build-asan -DIL_SANITIZE=ON && cmake --build build-asan && ./build-asan/unit_tests
```

Current output:

```
49 tests, 87709 checks, 0 failed
```

Every trial in the campaign is seeded, so any violation replays exactly:

```bash
./build/crash_sim --seed 12345 --trials 1 --mode lazy_sync --crc 1
```

---

## Roadmap

| component | status |
|---|---|
| `crc32c` — table-driven CRC-32C, RFC 3720 vectors | **done, tested** |
| `Device` — `FileDevice` + `SimDevice` power-loss simulator | **done, tested** |
| `wal` — record format, `ScanLog`, `LogWriter` | **done, tested** |
| `ledger` — double-entry, four durability modes, recovery | **done, tested** |
| `crash_sim` — the 10,000-trial power-loss campaign | **done, measured** |
| `kill_driver` / `kill_worker` — the `SIGKILL` campaign | next |
| `bench` — throughput and commit-latency per mode | pending |
| CI, charts, `findings.md` | pending (campaign CSV committed) |

---

## What this deliberately does not do

Scope discipline matters more than feature count, so: no concurrency (single-threaded throughout), no checkpointing or log truncation (the log grows forever, and startup replays all of it), no `O_DIRECT`, no modelling of a real disk's internal write-cache ordering, and no cryptographic integrity — CRC-32C defends against a failing disk, not an adversary.

Checkpointing is the interesting omission. It is genuinely necessary in production, and it would mean adding exactly the kind of on-disk metadata this log deliberately does not have — *"a log with no metadata has no metadata to corrupt"* is what makes recovery small enough to reason about after a power cut. That trade-off deserves its own project rather than a footnote in this one.

**On benchmark numbers:** `fsync` on a container's overlayfs is dramatically cheaper than on real storage, and on macOS `fsync(2)` does not flush the drive's own write cache at all (that needs `F_FULLFSYNC`). Point `--path` at real storage or the latency figures are fiction.

---

## Documentation

**[docs/STUDY_GUIDE.md](docs/STUDY_GUIDE.md)** — a ~1,000-line ground-up walkthrough for someone who can write C++ and has taken an OS course but has never worked on financial infrastructure. Why ledgers are double-entry and event-sourced, the four places your data lives between `write()` and the platter, why a failed `fsync` cannot be retried, torn writes, and every design decision above worked through in full.

## References

- **fsyncgate (2018)** — the PostgreSQL thread on `fsync` error handling. On Linux, a failed writeback reports the error *once* and then marks the page clean, so a retried `fsync` returns success over data that no longer exists. Twenty years of a wrong assumption. This is why `FileDevice::Sync()` throws instead of retrying, with a comment saying so.
- **Pillai et al., "All File Systems Are Not Created Equal" (OSDI 2014)** — the ALICE work on crash-consistency testing; the intellectual ancestor of this project.
- **Jepsen** (Kyle Kingsbury) — the same methodology one layer up: state your invariants, break the system on purpose, check the invariants.
- **TigerBeetle** — a production financial transactions database; their documentation on why ledgers are built this way is excellent.
- **"Files are hard"** (Dan Luu) — a survey of everything that can go wrong between your `write()` and the platter.

## License

MIT — see [LICENSE](LICENSE).
