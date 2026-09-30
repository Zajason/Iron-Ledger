# ironledger handbook

The operational reference: how to build it, how to run every tool, how to read
what comes out, how to reproduce a failure, and how to change things without
breaking the experiment.

**Related docs, so you open the right one:**

| doc | for |
|---|---|
| [README](../README.md) | the claim and the results — start there |
| [STUDY_GUIDE](STUDY_GUIDE.md) | *why* it is built this way, taught from scratch |
| **this handbook** | *how* to build, run, read and extend it |
| [PLAN](PLAN.md) | the development plan and what is left |

---

## 1. Build

Requires CMake ≥ 3.16 and a C++20 compiler. No third-party dependencies.

```bash
cmake -B build && cmake --build build
```

Targets: `libil.a`, `unit_tests`, `crash_sim`, `kill_worker`, `kill_driver`.

| option | default | effect |
|---|---|---|
| `IL_SANITIZE` | `OFF` | ASan + UBSan, `-fno-omit-frame-pointer -g` |
| `CMAKE_BUILD_TYPE` | `Release` | forced to Release when unset |

Warnings are `-Wall -Wextra -Wpedantic` and the tree is expected to build
**completely silent**. A new warning is a regression; fix it rather than
tolerating it.

```bash
cmake -B build-asan -DIL_SANITIZE=ON && cmake --build build-asan
./build-asan/unit_tests
```

---

## 2. Run everything

```bash
./build/unit_tests                                    # ~1s
./build/crash_sim --trials 10000 --ops 64             # ~7s
./build/kill_driver --trials 250                      # ~20s
```

All three exit non-zero on failure, so they chain:

```bash
./build/unit_tests && ./build/crash_sim --trials 2000 && ./build/kill_driver --trials 50
```

---

## 3. `unit_tests`

No arguments, no dependencies, two `CHECK` macros and a static registry.

```
  crc32c_rfc3720_vectors                              ok
  ...
49 tests, 87709 checks, 0 failed
```

Exit `0` if all pass, `1` otherwise. A failure prints `file:line` and, for
`CHECK_EQ`, both values. To run one test, comment out the others — there is
deliberately no filtering flag, because the whole suite takes about a second.

Adding a test is three lines:

```cpp
TEST(my_new_property) {
  CHECK(something);
  CHECK_EQ(a, b);
}
```

The `TEST` macro self-registers; there is no list to update.

---

## 4. `crash_sim` — the power-loss campaign

The centrepiece. Fully simulated and fully seeded, so results are deterministic
and identical on every machine.

```bash
./build/crash_sim [options]
```

| flag | default | meaning |
|---|---|---|
| `--trials N` | 10000 | trials per configuration |
| `--ops N` | 64 | transfers submitted per trial |
| `--seed N` | 1 | base seed; trial *i* uses `seed + i` |
| `--group N` | 8 | group-commit batch size / lazy-sync fsync interval |
| `--accounts N` | 8 | accounts in the workload |
| `--mode M` | *sweep* | `no_sync`\|`lazy_sync`\|`sync_every`\|`group_commit` |
| `--crc 0\|1` | *both* | checksum verification on recovery |
| `--p-persist F` | 0.5 | per-sector probability of surviving the cut |
| `--p-tear F` | 0.2 | per-sector probability of tearing |
| `--csv PATH` | — | append a row per configuration (writes a header if new) |
| `--quiet` | off | suppress the banner |

With neither `--mode` nor `--crc`, it sweeps all 8 configurations.
`p_persist + p_tear` must be ≤ 1.

### Reading the output

```
no_sync       crc=1   ACKS LOST=253278 / 320452   silent-corruption=0  torn-accepted=0
                                ^^^^^^   ^^^^^^                    ^                ^
                                lost     promised                   |                |
                                                 nothing caught it -+                |
                                        a ledger rule caught what the CRC let through
```

- **`ACKS LOST / total`** — the headline. Transfers the ledger said were safe
  and then could not find after recovery. **The only number that represents a
  broken promise.**
- **`silent-corruption`** — trials where recovery reported success and the state
  was wrong anyway. The worst outcome; nothing detected it.
- **`torn-accepted`** — trials where corruption got past the checksum layer and
  was caught by a domain rule (unbalanced postings, a duplicate key, a negative
  balance). A system noticing a problem, which is fine.

Durability loss is *not* counted as corruption: losing unacknowledged work is
expected behaviour for the unsafe modes, not a bug.

### Exit codes

| code | meaning |
|---|---|
| 0 | all invariants held |
| 1 | `sync_every` or `group_commit` lost an ack, or a mode broke conservation |
| 2 | bad arguments |

Code 1 is why this is a regression test and not a demo. `sync_every` and
`group_commit` acknowledge only after the data is on the platter, so an
acknowledged transfer *cannot* be lost. If one ever is, CI goes red.

### Reproducing a single trial

Every trial is seeded, so any violation replays exactly. The failure message
prints the command. Trial *i* of a run uses seed `base + i`:

```bash
./build/crash_sim --mode sync_every --crc 0 --trials 1 --ops 64 --seed 8490
```

To find which trial in a large run misbehaved, bisect by seed range:

```bash
for s in $(seq 1 1000 10000); do
  ./build/crash_sim --mode sync_every --crc 0 --trials 1000 --seed $s --quiet
done
```

### The two controls

Run these when you change the device model. A fault injector that finds nothing
looks exactly like one that is broken, so validate it in both directions:

```bash
./build/crash_sim --trials 200 --p-persist 1 --p-tear 0 --crc 0   # must be all zeros
./build/crash_sim --trials 200 --p-tear 0 --crc 0                 # torn records still appear
```

The first must be clean everywhere: if nothing is lost, everything must be
correct. The second is the subtle one — torn records still show up with
`p_tear=0`, because a record spanning several sectors can have some sectors
persist and others not. Sector-level loss produces partial records without any
intra-sector tearing.

**A campaign where `torn-accepted` is zero everywhere is broken**, because it
means the cut is never landing mid-commit and the checksum comparison is
measuring nothing.

---

## 5. `kill_driver` + `kill_worker` — the SIGKILL campaign

The control that shows why the simulator has to exist. Forks real processes
against a real filesystem.

```bash
./build/kill_driver [options]
```

| flag | default | meaning |
|---|---|---|
| `--trials N` | 250 | trials per mode |
| `--seed N` | 1 | base seed |
| `--group N` | 8 | batch size / fsync interval |
| `--accounts N` | 8 | accounts in the workload |
| `--min-ms F` | 0.2 | shortest run before the kill |
| `--max-ms F` | 25 | longest run before the kill |
| `--worker PATH` | beside the driver | the `kill_worker` binary |
| `--dir PATH` | `$TMPDIR` | where log files go (each is unlinked after its trial) |
| `--csv PATH` | — | append a row per mode |
| `--quiet` | off | suppress the banner |

Always sweeps all four modes, with checksums on.

### Reading the output

```
no_sync       crc=1   ACKS LOST=0 / 1710573   killed=250/250  prefix-violations=0  state-mismatches=0
```

`killed=250/250` confirms the worker really died to the signal rather than
finishing its work first. If that ratio drops, raise `--max-ms` — a worker that
completes normally is not testing a crash.

**Expect `ACKS LOST=0` for every mode, including `no_sync`.** That is the
finding, not a pass. Compare against `crash_sim`, where the same mode loses 79%.

### The protocol, and why it matters

The worker writes each ack to fd 1 as **8 raw little-endian bytes via
`write(2)`** — no stdio. You can see it directly:

```bash
./build/kill_worker --path /tmp/x.log --mode sync_every --ops 5 | xxd
# 0100000000000000 0200000000000000 ... keys 1..5
```

If the worker used buffered I/O, its acks would sit in a `FILE*` buffer and die
with the process. The parent would then believe *fewer* transfers had been
promised than actually were, and would fail to check exactly the transfers most
at risk — under-reporting losses and making a broken system look correct. **If
you ever change the worker's output, keep it unbuffered.**

The driver drains the pipe while it waits, rather than after the kill, so a fast
worker never blocks on a full pipe. A blocked worker stops making progress and
quietly changes the workload under measurement.

### Exit codes

Same scheme as `crash_sim`. Note what is deliberately **not** asserted: that
`no_sync` loses anything. Under this crash model it does not.

---

## 6. Interpreting a failure

| symptom | what it means | where to look |
|---|---|---|
| `sync_every`/`group_commit` lost an ack | the ack fired before the data was durable | `Ledger::Submit`, the mode's `Flush`/`Sync`/ack order |
| prefix violation | recovery accepted a record it should not have, or skipped one | `ScanLog` — especially the `lsn` check |
| state mismatch, prefix fine | replay applied the right records to the wrong effect | `Ledger::Apply`, `Validate` |
| conservation broken | a posting set that does not sum to zero was applied | `Validate` — this should be unreachable |
| `silent-corruption` with `crc=1` | the checksum missed something | `Crc32c`, the CRC range in `EncodeRecord` |
| `torn-accepted` is 0 everywhere | **the harness is broken**, not the ledger | the post-write hook and `crash_at` selection |

The `ScanStop` reason in `RecoveryReport::stop` narrows it fast:
`kCleanEnd`, `kShortRead`, `kBadMagic`, `kBadLsn`, `kBadLength`, `kBadCrc`,
`kBadPayload`.

`RecoveryReport::corruption_admitted()` means the checksum layer let something
through that a domain rule caught: `payload_parse_error`,
`duplicate_key_in_log`, `unbalanced_in_log`, `negative_balance_in_log`.

---

## 7. The invariants

These hold everywhere and are asserted constantly. If you break one, you have
broken the project.

1. **Conservation.** The sum over all accounts is exactly `0`, always, after any
   sequence of transfers and after any recovery. `Ledger::ConservationHolds()`.
2. **Durability.** Under `sync_every` and `group_commit`, an acknowledged
   transfer is never lost — under any crash model, in either checksum arm.
3. **Prefix.** Recovery yields a *prefix* of what was submitted: keys `1..k`
   with no gaps, no duplicates, no reordering.
4. **Exact state.** Recovered balances equal a clean replay of that prefix. Not
   merely plausible — identical.
5. **Nothing invalid is ever logged.** Validation precedes the write, so the log
   only contains transfers that were legal when written. This is what makes a
   replay rejection a *corruption* signal.
6. **`lsn == own byte offset`**, checked by the scanner. Without this, recovery
   can resynchronise past a hole and silently skip records.

---

## 8. Changing things safely

**The three things not to do:**

1. **Do not merge `LogWriter::Flush()` and `Sync()`.** `Flush` reaches the page
   cache; `Sync` reaches the platter. All four durability modes are just
   different arrangements of those two calls and the ack. Collapsing them
   deletes the experiment.
2. **Do not let `kNoSync` fsync**, not even on a clean shutdown. `Ledger::Commit`
   has an explicit guard. Forcing it would quietly turn `no_sync` into a correct
   mode and destroy the comparison.
3. **Do not make recovery "helpful".** A scanner that searches forward for the
   next valid record after a bad one will silently skip a hole in the middle of
   the log. Stop at the first record that does not verify. A loud failure gets
   escalated to a human; silent partial recovery gets served to customers.

**If you change the workload** (`tools/workload.h`), keep two properties: the
stream must be deterministic for a seed, and nothing may ever be rejected. Both
are covered by tests. Dense keys `1,2,3…` are what let a harness verify the
prefix property without asking the ledger anything; a workload that gets
rejections breaks that check.

**If you change the record format**, update the golden-bytes test
(`transfer_wire_is_byte_stable_little_endian`) deliberately, not reflexively. It
exists so a log written on arm64 still replays on x86.

**If you add a durability mode**, add it to `DurabilityModeName`,
`ParseDurabilityMode`, the `switch` in `Submit` (which is exhaustive, so the
compiler will find it), the mode list in both harnesses, and an ack-point test
asserting exactly when its acks become durable.

**Before committing:** silent build, `unit_tests` green, both campaigns exit 0,
and the sanitizer build green.

```bash
cmake -B build && cmake --build build 2>&1 | grep -i warning   # expect nothing
./build/unit_tests && ./build/crash_sim --trials 2000 && ./build/kill_driver --trials 50
cmake -B build-asan -DIL_SANITIZE=ON && cmake --build build-asan && ./build-asan/unit_tests
```

---

## 9. File map

```
include/il/crc32c.h   src/crc32c.cpp    CRC-32C, composes across buffers
include/il/device.h   src/device.cpp    Device; FileDevice (real fsync); SimDevice (power loss)
include/il/wal.h      src/wal.cpp       record format, ScanLog, LogWriter
include/il/ledger.h   src/ledger.cpp    double-entry, durability modes, recovery-on-open
tools/workload.h                        the deterministic transfer stream
tools/crash_sim.cpp                     power-loss campaign
tools/kill_worker.cpp                   the victim process
tools/kill_driver.cpp                   SIGKILL campaign
tests/unit_tests.cpp                    all 49 tests
results/*.csv                           committed campaign output
```

**Where to start reading:** `include/il/device.h` for the crash model, then
`include/il/wal.h` for the LSN rationale, then `Ledger::Submit` for the ack
points, then `tools/crash_sim.cpp` for how the cut is timed. The comments carry
the reasoning; they are not decoration.

---

## 10. Gotchas

- **`fsync` on macOS does not flush the drive's write cache.** That needs
  `fcntl(fd, F_FULLFSYNC)`. Benchmark numbers from a Mac are optimistic.
- **`fsync` on a container's overlayfs is far cheaper than on real storage.**
  Point any benchmark at real storage or the latency figures are fiction.
- **The `SIGKILL` campaign's ack counts are host-dependent**; the
  `crash_sim` numbers are not, because it is fully simulated and seeded.
- **`kill_driver` writes real files** to `$TMPDIR`, one per trial, unlinked
  afterwards. A crash of the driver itself can leave `ironledger_kill_*.log`
  behind.
- **Two build directories.** `build/` and `build-asan/` are both gitignored;
  the sanitizer build is much slower, so do not benchmark in it.
- **`--ops` is per trial, `--trials` is per configuration.** `crash_sim` sweeps
  8 configurations by default, so `--trials 10000` is 80,000 trials and runs the
  workload twice per trial.
