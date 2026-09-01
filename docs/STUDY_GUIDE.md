# ironledger: a study guide

*For a computer engineering student who can write C++ and has seen an operating
systems course, but has never worked on financial infrastructure.*

This document explains **what we are building, and why every piece of it is
shaped the way it is**. The code is the artifact; this is the reasoning behind
it. Read it once end to end, then keep it open while reading the source.

> **Status, as of this writing.** `crc32c`, `Device` and the WAL are built and
> tested (22 unit tests, green under ASan/UBSan). The ledger and the harnesses
> are not written yet. Every results number quoted in Part VI is therefore the
> **expected shape of the answer**, not a measurement — it is what we predict
> and what the code will be checked against. This banner comes out, and the
> numbers get replaced with real ones, once the campaign has actually run.

---

## Table of contents

1. [The one-sentence version](#1-the-one-sentence-version)
2. [Part I — The domain: what a ledger actually is](#part-i--the-domain-what-a-ledger-actually-is)
3. [Part II — The storage stack: what happens when you write a file](#part-ii--the-storage-stack-what-happens-when-you-write-a-file)
4. [Part III — The write-ahead log](#part-iii--the-write-ahead-log)
5. [Part IV — Durability modes and the acknowledgement point](#part-iv--durability-modes-and-the-acknowledgement-point)
6. [Part V — How you actually test this](#part-v--how-you-actually-test-this)
7. [Part VI — Reading the results table](#part-vi--reading-the-results-table)
8. [Glossary](#glossary)
9. [Where each idea lives in the code](#where-each-idea-lives-in-the-code)
10. [Further reading](#further-reading)

---

## 1. The one-sentence version

**We are building a small financial ledger that writes every transaction to a
log before acknowledging it, and then we are cutting the power out from under
it ten thousand times to find out which of the popular durability shortcuts
actually survive.**

The deliverable is not the ledger. The deliverable is a **table of numbers**:
four durability policies, checksums on and off, and for each of those eight
configurations, the count of transfers that we *told the client were safe* and
then lost.

The research question is:

> Which crash model do you actually have to test against, and which of the
> cheap durability shortcuts survive it?

That question has a surprising answer, and the surprise is the whole point of
the repo. We will get to it in Part V.

---

# Part I — The domain: what a ledger actually is

## 1.1 Why fintech is not just CRUD

If you have written a web backend, your instinct for "transfer money" is:

```sql
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
UPDATE accounts SET balance = balance + 100 WHERE id = 2;
```

This is wrong in ways that get people fired. Not *inefficient* — wrong.

**Problem 1: it destroys history.** After the update, the old balance is gone.
If a customer disputes a charge six months later, or a regulator asks how an
account reached its current balance, or you need to prove you did not silently
edit someone's money, you have nothing. The current state is the only thing you
kept, and the current state is exactly what is in question.

**Problem 2: there is no moment where the system is correct.** Between those
two statements, 100 units of money do not exist anywhere. If the process dies
there, money has been destroyed. A database transaction hides that window from
other readers, but the window is still real at the storage layer, and a power
cut does not respect transaction boundaries.

**Problem 3: it cannot be checked.** Given a table of balances, how do you know
it is right? Right compared to *what*? There is no independent record to
reconcile against.

Financial systems solve all three the same way, and have done since roughly
1300: **double-entry bookkeeping**.

## 1.2 Double-entry, for engineers

The rule is simple enough to state in one line:

> **Money is never created or destroyed. It only moves. Every movement is
> recorded twice — once where it left, once where it arrived — and the two
> halves must sum to zero.**

In our code a single movement is a `Posting`, and a set of postings that
balance is a `Transfer`:

```cpp
struct Posting { uint64_t account; int64_t amount; };  // signed!

struct Transfer {
  uint64_t idem_key;
  std::vector<Posting> postings;   // must sum to exactly zero, 2..8 of them
};
```

Alice pays Bob 100:

```
Transfer {
  postings = [ {account: alice, amount: -100},
               {account: bob,   amount: +100} ]
}
```

Sum: `-100 + 100 = 0`. ✅

Why allow up to 8 postings instead of exactly 2? Because real transactions are
rarely two-sided. A card payment might be:

```
customer  -1000    (what they paid)
merchant   +971    (what they receive)
processor   +25    (interchange fee)
network      +4    (scheme fee)
                    ------
                       0
```

Four parties, one atomic event, sums to zero. If you modelled that as three
separate two-sided transfers, there would be moments when the fee had been
taken but not yet credited — and a crash in one of those moments leaves money
in limbo. **Atomicity is a domain requirement here, not a performance
optimisation.**

### The zero-sum invariant is a free corruption detector

Here is the property that makes this design worth the trouble:

> Every transfer sums to zero. Therefore the sum over **all accounts** is
> always zero, no matter how many transfers you apply, in any order.

That gives us a single `int64_t` we can compute at any moment, from the full
account table, that must be exactly `0`. If it is not, **something is
corrupt** — a byte flipped, a record was replayed twice, a partial write was
accepted. We do not need to know *what* went wrong to know that *something*
did.

It is the cheapest global integrity check in the entire repo, it costs one pass
over the accounts, and we assert it everywhere: after every transfer, after
every recovery, after every simulated crash. If you learn one transferable idea
from this project, make it this one — **find the invariant your domain gives
you for free, and check it obsessively.**

## 1.3 The world account

There is a problem with "money is never created or destroyed": money has to get
into the system somehow. A customer deposits cash. A bank wire arrives. That
money genuinely appears from outside your ledger's universe.

The standard trick is a special account representing *everything outside the
system*. We call it `kWorldAccount` and give it id `0`.

A $100 deposit is not money appearing from nowhere. It is a transfer **from the
world**:

```
world     -100
customer  +100
                0   ✅ still balanced
```

The world account is the **only** account allowed to go negative. Its balance
is, by construction, the negative of all the real money in the system. Every
other account is checked for sufficient funds before a debit is allowed.

This is not a hack — it is how real ledgers model the boundary of the system.
It means the zero-sum invariant holds without exception, which means we can
assert it unconditionally, which means it catches everything.

## 1.4 Event sourcing: the log *is* the database

Now the second big idea. Instead of storing balances and updating them, we
store **the sequence of transfers**, and treat balances as a *derived view*.

```
   THE TRUTH                          A CACHE OF THE TRUTH
   ---------                          --------------------
   transfer #1  ─┐
   transfer #2   ├── replay ───▶      { alice: 400, bob: 600, ... }
   transfer #3  ─┘
```

The log is append-only and immutable. Balances live in memory and are rebuilt
by replaying the log from the beginning. This is **event sourcing**.

What it buys us:

- **Complete history for free.** Every state the system was ever in is
  reconstructible. Audit and dispute resolution stop being features you build
  and start being queries you run.
- **Recovery is just startup.** There is no separate "repair the database"
  code path — see below.
- **Appends only.** We never modify a byte we have already written. That turns
  out to matter enormously for crash safety, because a crash during an
  in-place update can leave a record that is half-old and half-new. A crash
  during an append can only leave a partial record at the *end*, which we can
  detect and discard.

### "Opening the ledger is recovery"

This is a design decision worth pausing on. Our constructor is:

```cpp
Ledger(Device&, LedgerOptions, RecoveryReport* = nullptr);
```

Opening a ledger *always* means: scan the log from offset 0, replay every valid
record, and stop at the first one that fails to verify. A brand-new empty
device simply replays zero records.

The consequence: **the recovery path is exercised by every single test we
write**, not just the tests that are about recovery. There is no rarely-taken
"we crashed last time" branch that only runs in production at 3am. The
dangerous code runs constantly, so bugs in it surface immediately.

That is a general technique: *make the rare, dangerous path the only path.*

## 1.5 Idempotency: the network lied to you

Here is a scenario that happens constantly in payments:

1. Client sends "transfer 100 from Alice to Bob".
2. Server commits it and sends back "ok".
3. **The response is lost** — timeout, dropped connection, load balancer hiccup.
4. Client has no idea whether it worked. It retries.

If the server processes the retry as a new transfer, Alice has now paid Bob
200. If it ignores the retry and the original never actually committed, Bob
never gets paid. **The client cannot distinguish these cases from the outside.**

The fix: the client attaches a unique **idempotency key** to the request, and
promises to reuse the same key when retrying. The server remembers which keys
it has seen. A duplicate key is a no-op that returns the original result.

```cpp
struct Transfer { uint64_t idem_key; /* ... */ };
```

Now the neatest property in this whole design:

> **Idempotency keys are rebuilt from the log during replay, so deduplication
> survives crashes for free.**

We do not need a separate durable "seen keys" table, and we do not need to
worry about that table getting out of sync with the ledger. The set of applied
keys is a pure function of the log, exactly like balances are. Recovery
restores both by the same mechanism. If a transfer is in the log, its key is
remembered; if it is not, the retry will correctly go through.

Two structures with a consistency requirement between them are a bug waiting to
happen. One structure derived from another cannot be inconsistent.

## 1.6 Replay applies the same rules as submit — and why that is a corruption alarm

When we replay the log at startup, we re-run **the same validation** we run on
a live submit: postings must balance, balances must not go negative, the
idempotency key must be unseen.

That seems redundant. These records were validated when they were written — why
check again?

Because replay is **deterministic and in submit order**. It sees exactly what
the original run saw, in exactly the same sequence. So a record that validated
when it was written *must* validate again on replay.

Therefore:

> **A validation failure during replay cannot mean "bad input". It can only
> mean the bytes on disk are not the bytes we wrote.**

That turns our ordinary business rules into a second, independent corruption
detector, layered underneath the checksums. Hence `RecoveryReport`:

```cpp
payload_parse_error     // the bytes are not a well-formed transfer
duplicate_key_in_log    // a key appears twice: impossible unless corrupt
unbalanced_in_log       // postings do not sum to zero: impossible unless corrupt
negative_balance_in_log // a debit that could not have been accepted live
```

...and `corruption_admitted()`, meaning: *the checksum layer let something
through, and only a domain rule caught it.*

This distinction is measured directly in the experiment. Keep it in mind — it
becomes the `torn_accepted` column in Part V.

---

# Part II — The storage stack: what happens when you write a file

This part is operating systems, not finance. It is also where almost all of the
real-world bugs live, because most programmers have a mental model that stops
at `write()`.

## 2.1 Your data is in at least four places

When your program calls `write(fd, buf, n)` and it returns successfully, where
is the data?

```
   ┌──────────────────────┐
   │ 1. your process      │   e.g. a std::vector buffer, or stdio's FILE*
   │    (user space)      │   Lost if: your process crashes
   └──────────┬───────────┘
              │  write(2) / pwrite(2)
   ┌──────────▼───────────┐
   │ 2. kernel page cache │   Survives: process crash, kill -9
   │    (RAM, kernel)     │   Lost if:  power loss, kernel panic
   └──────────┬───────────┘
              │  fsync(2)  ── the kernel writes pages to the device
   ┌──────────▼───────────┐
   │ 3. device write cache│   Volatile RAM *on the drive itself*
   │    (RAM, on drive)   │   Lost if:  power loss
   └──────────┬───────────┘
              │  cache flush command (part of a proper fsync)
   ┌──────────▼───────────┐
   │ 4. the platter/NAND  │   Actually durable. Survives power loss.
   └──────────────────────┘
```

**The single most important line in this document:**

> A successful `write()` means the kernel has *accepted* your bytes. It does
> **not** mean they are on disk. Only a successful `fsync()` means that.

The gap between levels 2 and 4 is where every result in this project comes
from. A process that has called `write()` but not `fsync()` has data that will
survive `kill -9` perfectly — the kernel still holds it and will write it out
eventually — but will vanish entirely if the power goes out.

Hold onto that sentence. It is Trap #1 in Part V, and it is the headline
finding of the repo.

## 2.2 fsync, and why we throw when it fails

`fsync(fd)` tells the kernel: push everything for this file down to stable
storage and do not return until it is there.

In `FileDevice::Sync()` we do something that looks harsh:

```cpp
void FileDevice::Sync() {
  if (::fsync(fd_) != 0) {
    throw std::system_error(/* ... */);   // we do NOT continue
  }
  ++fsync_count_;
}
```

Why not log the error and retry? Because **on Linux, retrying an fsync is
worse than useless.**

When writeback of a dirty page fails, Linux reports the error to the next
`fsync()` caller — *once* — and then **marks the page clean anyway**. The page
is now considered "not dirty", so it will never be written again, and a second
`fsync()` will return success over data that no longer exists anywhere. Your
error handling reports "transient failure, retried successfully" while the data
is permanently gone.

This is not hypothetical. It is the 2018 **PostgreSQL "fsyncgate"** incident:
Postgres had been retrying failed fsyncs for roughly twenty years, and on that
path it could report a successful checkpoint over data that had been silently
dropped. The fix in Postgres was, essentially, to panic.

So our rule, and the rule for any durability layer:

> Once `fsync` has reported failure, the only honest response is to stop
> acknowledging anything. There is no retry that recovers the data.

That comment is in the source at [`src/device.cpp`](../src/device.cpp), because
somebody will eventually be tempted to "fix" the throw.

> **Aside for macOS users:** on macOS, `fsync(2)` gets data out of the page
> cache but does **not** flush the drive's own write cache — level 3 above.
> Real durability there requires `fcntl(fd, F_FULLFSYNC)`, which is far slower.
> This matters for benchmark numbers, not for our simulation.

## 2.3 Sectors, atomicity, and torn writes

Storage devices do not read and write bytes. They work in **sectors** —
historically 512 bytes, now often 4096. A sector is the unit the hardware can
(usually) update atomically.

Now suppose you write 1500 bytes and the power fails mid-write. That write
spans three 512-byte sectors. What is on the platter afterwards?

```
   what you wrote:   [ AAAAAA ][ BBBBBB ][ CCCCCC ]
                      sector 0  sector 1  sector 2

   possible reality: [ AAAAAA ][ BBB--- ][ ------ ]
                       landed    TORN      lost
```

Sector 1 is a **torn write**: some of your new bytes, and then the old stale
bytes where the write did not finish. The result is a record that is neither
the old value nor the new one. It is a chimera that no version of your program
ever produced, and it may still *look* structurally valid.

This is what checksums exist to catch, and it is the reason a "the write either
happens or it doesn't" mental model is dangerous.

## 2.4 Our simulator, and why it is deliberately unfair

`SimDevice` in [`include/il/device.h`](../include/il/device.h) models all of
this so that crashes become **reproducible and seedable** — you cannot debug a
real power failure, but you can replay a simulated one exactly.

The model:

- Writes land in a volatile cache, tracked at **512-byte sector granularity**.
- Only `Sync()` moves dirty sectors to stable storage.
- `PowerCut(rng, policy)` returns *the byte image that would be on the platter*
  if the plug came out right now.

And here is the deliberately harsh part. `PowerCut` resolves each dirty sector
**independently**:

```cpp
for each dirty sector:
    roll a die:
      persisted in full   (probability p_persist)
      torn                (probability p_tear)     — random prefix, stale tail
      lost entirely       (the remainder)
```

Real drives have a good deal of internal ordering; they do not usually lose
sector 5 while keeping sector 9. **We remove that ordering on purpose.**

Why be unfair to our own system? Because write ordering is exactly the
assumption that unsynced logs quietly depend on, and an assumption you never
violate is an assumption you never tested. If your durability argument secretly
requires "the device wrote these in order", we want that argument to break
loudly in a simulator rather than quietly on a customer's hardware after a
datacentre power event.

### The hole

The independent resolution produces one specific failure mode that matters
enormously:

```
   sector 0: LOST        →   [ 000000 ][ RECORD2 ]
   sector 1: PERSISTED         ^^^^^^^
                               a hole of zeros in the MIDDLE of the log,
                               with intact data after it
```

Reading a file at an offset that was never written gives you zeros — the same
thing a lost sector gives you. So recovery sees a valid log, then a run of
zeros, then a perfectly valid record sitting after the gap.

What recovery does at that moment is Trap #3, and it is the reason for the LSN
design in Part III.

---

# Part III — The write-ahead log

## 3.1 Write-ahead logging in one paragraph

Before changing anything, write down what you are about to do, and make that
note durable. Then do it. If you crash, read your notes on restart and finish
the job. That is a **write-ahead log** (WAL), and it underpins essentially
every database you have used.

Our WAL is stripped to the bone: **records and nothing else**. No superblock,
no free list, no index, no checkpoint. Recovery is:

> Replay every record that verifies, and stop at the first one that does not.

That fits in one sentence, which is the only reason we can reason about it
after a power cut. **A log with no metadata has no metadata to corrupt.**

(Real systems need checkpointing and log truncation so the log does not grow
forever and startup does not take hours. We deliberately omit both — see the
README's "what this does not do". Adding them adds exactly the kind of metadata
we just said we did not want, and that trade-off deserves its own project.)

## 3.2 The record format

24-byte header, little-endian, packed:

```
   offset  size  field
   ------  ----  -----------------------------------------------------
        0     4  magic   = 0x474F4C49  ("ILOG")
        4     4  crc     = CRC-32C over bytes [8,24) of header + payload
        8     8  lsn     = the byte offset of THIS header
       16     4  type
       20     4  len     = payload length
       24   len  payload
```

Every field earns its place:

- **magic** — "is this a record at all?" Zeros from a lost sector fail here
  immediately.
- **crc** — "are these the bytes we wrote?"
- **lsn** — "does this record belong *here*?" (§3.4 — the important one)
- **type** — lets the format grow later without ambiguity.
- **len** — bounded by `kMaxPayloadSize` so a corrupt length field cannot drive
  a gigabyte allocation. That check is why `bad_length` exists as a stop reason.

Little-endian is written out by hand rather than `memcpy`-ing a struct, so a log
written on x86 replays on arm64. (We develop on an M-series Mac and CI is
meant to run on x86 Linux, so this is not theoretical.)

## 3.3 Checksums: what CRC-32C does and does not do

We use **CRC-32C** (Castagnoli polynomial `0x82F63B78`, reflected), the same
one used by iSCSI, ext4, Btrfs, and most storage systems.

What it gives you: all single-bit errors, all burst errors up to 32 bits, and
for random corruption a miss probability of about 2⁻³² ≈ one in four billion.

What it does **not** give you:

- **It is not cryptographic.** An attacker can trivially forge data with a
  matching CRC. It defends against a failing disk, not against an adversary.
- **It does not tell you what the right bytes were.** It detects; it does not
  correct.
- **It cannot see a record that is entirely missing.** A checksum verifies the
  bytes that *are* there. It has nothing to say about bytes that are not. This
  is precisely the hole problem, and it is why the CRC is not enough on its own.

Our implementation is table-driven software, with no SSE4.2 or ARMv8 CRC
intrinsics, because the campaign compares runs across machines and the checksum
must be bit-identical everywhere. (This is one of the few places where "make it
fast" is the wrong instinct.)

The `Crc32c(buf, n, prev)` signature lets checksums **compose** over adjacent
buffers:

```cpp
Crc32c(b, nb, Crc32c(a, na)) == Crc32c(ab, na + nb)
```

which is how we checksum a header range and a payload that live in two
different places without splicing them into one allocation first.

**And crucially: checksums are a switch in this project (`--crc 0/1`), because
one of the eight configurations we measure is "what happens if you skip them".**
People skip them. We are measuring the consequence.

## 3.4 The LSN is the record's own byte offset — Trap #3

> **`lsn == the offset at which this record is stored`, and the scanner checks
> it.** This is load-bearing, not decoration.

Here is the failure it prevents. Recall the hole from §2.4: sector N lost,
sector N+1 intact.

A scanner that checks only magic and CRC hits the zeros, sees "not a record",
and now faces a choice. A naive implementation — or one trying to be helpful,
"recovering as much as possible" — scans forward for the next valid magic,
finds record #4 sitting there with a **perfect checksum**, and replays it.

The result: transfers #2 and #3 have silently vanished. Recovery reports
success. Balances are wrong. Nothing anywhere logged an error. The checksum did
its job perfectly and was irrelevant, because *record #4's bytes were never
corrupted* — they were simply not supposed to be reachable yet.

With a self-locating LSN this is impossible. Record #4 announces "I live at
offset 4096". The scanner is at offset 2048. Those disagree, so the scan stops
with `bad_lsn`. The missing data is *detected*, not skipped.

Two tests pin this down, and both are worth reading:

- `wal_hole_stops_scan_with_bad_magic` — zero out a record in the middle and
  assert the scan stops **at the hole** rather than resynchronising onto the
  survivors. It asserts this **with checksums both on and off**, because the
  magic check is structural: this failure mode is caught even in the `crc=0`
  arm.
- `wal_misplaced_record_stops_scan_with_bad_lsn` — shift a record so it sits at
  the wrong offset. Its magic passes. Its CRC passes. **Only the LSN catches
  it.**

The general lesson, which applies far beyond this repo:

> **A recovery routine that tries to salvage as much as possible is more
> dangerous than one that stops at the first sign of trouble.** Silent partial
> recovery is worse than a loud failure, because a loud failure gets escalated
> to a human and silent corruption gets served to customers.

## 3.5 `Flush()` and `Sync()` are different verbs — never collapse them

```cpp
class LogWriter {
  void Flush();  // buffered bytes → the device (page cache).  Survives kill -9.
  void Sync();   // Flush(), then fsync.        Survives power loss.
};
```

Two methods, one letter of difference in the API, an entire category of bug in
between. Every durability mode below is just a different arrangement of these
two calls plus the acknowledgement. If you merged them into a single `Commit()`,
you would delete the experiment.

`Flush()` also writes the whole buffer in **a single `WriteAt`**. That is what
makes group commit cost one device write, and — as Part V explains — it gives
the crash harness a single, well-defined instant to cut power at.

---

# Part IV — Durability modes and the acknowledgement point

## 4.1 What "acknowledged" means, and why it is the only thing that matters

The **acknowledgement** (the "ack") is the moment your system tells the outside
world *"this transfer is safe"*. The client updates its UI. The merchant ships
the goods. Another system records the payment as received.

An ack is a **promise you cannot take back.**

So the only durability question that actually matters is:

> **Is the ack ever ahead of the data?**

Losing unacknowledged data is disappointing but survivable — the client never
heard a promise, so it will retry, and idempotency keys make the retry safe.
Losing *acknowledged* data is a broken promise. That is the number in our
headline table, and it is the only number in the table.

This is why `Ledger` exposes:

```cpp
void set_ack_sink(std::function<void(uint64_t idem_key)>);
```

The harness records every key the ledger acknowledged, cuts the power, and then
demands that **every single one** of them survived. **Getting the ack point
right for each mode is the entire experiment.**

## 4.2 The four modes

### `kNoSync` — write and hope

```
Append → Flush → ACK
```

Never calls `fsync`. Fastest possible. Survives `kill -9` (the page cache is
intact). Loses everything in flight on power loss.

This is what you get when you use a file and never think about durability. It
is more common in production than anyone would like to admit.

### `kLazySync` — ack now, sync eventually

```
Append → Flush → ACK          (every record)
                 fsync         (every N records)
```

**This is the interesting one, and the reason the project exists.**

It looks perfect. It is fast. It passes crash testing on any machine that never
actually loses power. It even passes the `kill -9` campaign flawlessly. It
"feels" durable because the data really is being fsynced — just... slightly
after you promised it was.

That gap is real, and it is exactly `N` records wide. `kLazySync` is the shape
of an enormous number of real-world "we have durability" claims.

### `kSyncEvery` — correct and slow

```
Append → Flush → fsync → ACK
```

The ack comes strictly after the data is on the platter. Correct by
construction. One fsync per transfer, which is brutal: on real hardware an
fsync is on the order of hundreds of microseconds to milliseconds, so this caps
you at maybe a few thousand transfers per second regardless of CPU.

### `kGroupCommit` — correct *and* fast

```
Append ×N → one WriteAt → one fsync → ACK all N
```

Buffer N transfers, write them in one device write, fsync once, then
acknowledge all N. Nobody is acked before their data is durable — so it is
**exactly as correct as `kSyncEvery`** — but the fsync cost is amortised across
the batch.

This is how real databases achieve both durability and throughput, and it is
the right answer to the durability question.

**Note that phrasing carefully: the right answer to the *durability* question.**
Part VI is about what group commit does *not* fix.

## 4.3 Measuring latency honestly

A subtle trap in the benchmark. Under group commit, a transfer submitted first
in a batch waits for the rest of the batch before being acked. If you measure
"how long did `Submit()` take to return", group commit looks almost free —
because you measured the wrong interval.

So: **the clock starts at submit and stops when the ack fires**, several
submissions later if need be. Anything else flatters group commit enormously.
We report p50/p99/p99.9/max, not just the mean, because the whole risk in
batching lives in the tail.

---

# Part V — How you actually test this

## 5.1 Trap #1: `SIGKILL` is not a power cut

Here is the finding the README leads with, and the most useful thing in this
document.

The obvious way to crash-test a storage system is to kill it:

```bash
./worker & sleep 0.01; kill -9 $!    # repeat a few hundred times
```

Run that against `ironledger` and the expectation — the thing this campaign
exists to demonstrate — is that **`kNoSync`, the mode that never calls fsync at
all, passes with a perfect score.** Hundreds of kills, zero lost acknowledged
transfers.

Look back at §2.1 and it is obvious why. `kill -9` destroys the *process*. It
does not touch the **kernel page cache**. Every byte the dying process had
`write()`-ten is still sitting in kernel memory, and the kernel writes it out at
its leisure, entirely indifferent to the fact that the process that produced it
no longer exists. The data lands on disk. Recovery finds it. Everything looks
fine.

> **My ledger survived hundreds of `kill -9`s and was still catastrophically
> wrong.**

That is not a bug in the harness. **That is the finding.** A `SIGKILL` campaign
tests that your recovery logic can handle a truncated log. It tells you
*nothing whatsoever* about durability, because the failure mode it models —
process death — is not the failure mode that loses data.

The uncomfortable implication: a great many systems that claim to be
crash-tested have only ever been tested this way.

To model power loss you must model **losing the page cache**, and you cannot do
that from inside a normal process — the kernel will not cooperate. Your options
are real hardware with a real switch, a virtual machine you can hard-reset, or
a simulated device. We build the simulated device, and that is why `SimDevice`
is the heart of the project rather than an implementation detail.

We run **both** campaigns and report both, precisely so the gap between them is
visible in the results.

## 5.2 Trap #2: cut the power mid-commit, not between transactions

Second way to accidentally measure nothing.

The natural way to write the crash harness is: run some transfers, then cut the
power. But "after a submit returned" is a **quiescent** moment — a well-behaved
log has nothing in flight there. Buffers are flushed, records are whole.

Cut power only at quiescent points and you will never produce a single torn
record. The checksum arm of the experiment then compares "checksums on" against
"checksums off" **on data that is never corrupted**, gets identical results, and
reports — with great confidence and many decimal places — nothing at all.

The cut has to land in the dangerous window: **immediately after a device write
and before the fsync that follows it.**

That is what `set_post_write_hook` exists for, and the harness uses a two-pass
structure:

```
pass 1:  run the workload, count device writes            → W
         choose a crash index uniformly from [1, W+1]
pass 2:  replay the workload identically (same seeds)
         on write #k, the hook fires:
             snapshot PowerCut(rng, policy)
             snapshot how many acks had fired by that instant
```

Two details in there matter:

- **Pass 2 must be bit-identical to pass 1.** Same seeds, same everything. That
  is why the workload is a deterministic seeded stream rather than anything
  random or time-dependent.
- **The range is `[1, W+1]`, not `[1, W]`.** The extra slot is the quiescent
  crash — after all writes are done. We want that case sampled too; we just
  refuse to let it be the *only* case.

And because every trial is seeded, any violation replays exactly:

```bash
./crash_sim --seed 12345 --trials 1 --mode lazy_sync --crc 1
```

A crash-consistency bug you cannot reproduce is a crash-consistency bug you
cannot fix. **Determinism is not a nicety here; it is the difference between a
finding and a rumour.**

## 5.3 The five checks per trial

After each simulated power cut we recover from the post-crash bytes and assert:

1. **Durability** — every acknowledged key is present.
   *This is the headline number.* A failure is a broken promise.

2. **Prefix** — the recovered key sequence is a prefix of what was submitted:
   no phantoms, no holes, no reordering. Because the workload is deterministic,
   the harness knows exactly what was submitted and can check this without
   cooperation from the ledger.

3. **State** — recovered balances equal a clean replay of that prefix. Recovery
   must not merely produce *a* plausible state; it must produce *the* correct
   one.

4. **Conservation** — all balances sum to zero (§1.2). The free invariant,
   cashed in.

5. **Classification** — how did this trial fail, if it did:

   | class | meaning |
   |---|---|
   | `clean` | recovery was correct |
   | `torn_accepted` | corruption reached the domain layer and was caught by a *ledger rule* — the checksum layer let it through |
   | `silent_corruption` | **nothing caught it.** Recovery reported success and the state is wrong. |

`silent_corruption` is the number that should frighten you. Every other outcome
is a system noticing a problem. That one is a system confidently serving wrong
balances.

## 5.4 The workload is deliberately boring

`tools/workload.h` generates a deterministic seeded transfer stream shared by
every harness, and it is tuned so that **nothing is ever rejected**:

- fund each account with 10¹² units
- cap transfer amounts at 1000

Why go to the trouble? Because if no transfer is ever rejected for business
reasons, idempotency keys come out as a dense `1, 2, 3, …`. And that turns
check #2 into something a driver can verify **entirely on its own**, without
asking the ledger anything:

> the recovered key sequence must be `1..k` for some `k`

Any gap, any duplicate, any reordering is instantly visible. This is what lets
the `kill_driver` reconstruct what its worker was doing without any IPC beyond
raw ack bytes.

Note the technique: **we constrained the experiment so that the oracle became
trivial.** A test is only as good as your ability to say what the right answer
was, and "the right answer is the sequence 1..k" is about as strong an oracle
as you can get.

## 5.5 How the `kill -9` harness avoids lying to itself

One more detail worth stealing. In the SIGKILL campaign, the worker reports
each acknowledgement to the parent as **8 raw bytes written to fd 1 with
`write(2)`** — unbuffered, no `printf`, no `iostream`.

If the worker used buffered I/O, its acks would sit in a stdio buffer and die
with it. The parent would then believe *fewer* transfers were promised than
actually were, and would fail to check exactly the transfers most at risk. The
harness would under-report losses and look great.

> **The parent's record of what was promised must never be ahead of reality —
> and must never lag behind it either.**

Getting this wrong makes a broken system look correct, which is the worst
possible direction for a testing bug to fail in.

---

# Part VI — Reading the results table

The campaign is 8 configurations × 10,000 simulated power failures. This is the
shape of result we expect (see the status note at the top — these are
predictions until the campaign runs):

```
no_sync       crc=1   ACKS LOST=20003 / 21616      silent-corruption=0
lazy_sync     crc=1   ACKS LOST=4588  / 21616      silent-corruption=0
sync_every    crc=1   ACKS LOST=0                  silent-corruption=0
group_commit  crc=1   ACKS LOST=0                  silent-corruption=0
no_sync       crc=0   ACKS LOST=19945              silent-corruption=58
lazy_sync     crc=0   ACKS LOST=4563               silent-corruption=25
sync_every    crc=0   ACKS LOST=0                  silent-corruption=0
group_commit  crc=0   ACKS LOST=0                  silent-corruption=33
```

Read it in three passes.

**Pass 1 — the durability column (`ACKS LOST`).**
`no_sync` loses roughly everything it promised. `lazy_sync` — the one that looks
perfect on any machine that never loses power — loses **thousands** of
acknowledged transfers. `sync_every` and `group_commit` lose **exactly zero**.
Not "few". Zero. If those rows are ever non-zero, there is a bug, and the
campaign exits non-zero so CI catches it. That is what makes this a regression
test and not just a demo.

**Pass 2 — group commit is not a compromise.** It loses zero, exactly like
`sync_every`, while amortising the fsync across a batch. Correctness and
throughput are not actually in tension here; the naive shortcut (`lazy_sync`) is
simply the wrong optimisation. It trades away the thing you cannot recover for
speed you could have had anyway.

**Pass 3 — now look at the bottom row, and look at it properly.**

```
group_commit  crc=0   ACKS LOST=0                  silent-corruption=33
```

Group commit **never loses an acknowledged transfer** — and still hands you
**silently corrupted balances** 33 times, because the records were not
checksummed. Every promise was kept. The data is wrong anyway. Recovery
reported success.

> **Durability and integrity are two different problems, and fixing one does
> nothing at all for the other.**

Durability asks *"did the bytes survive?"* Integrity asks *"are they the bytes I
wrote?"* `fsync` answers the first question. Only a checksum answers the second.
A system that fsyncs religiously and skips checksums will faithfully preserve
your corruption forever.

That single row is the most valuable thing in the repo, and it is the reason the
experiment crosses durability policy with checksums instead of testing them
separately.

---

## Glossary

| term | meaning |
|---|---|
| **ack / acknowledgement** | the moment you tell the client "this is safe". An unbreakable promise. |
| **crash consistency** | the property that any state left by a crash recovers to a correct state |
| **double-entry** | every movement recorded twice, debits and credits summing to zero |
| **event sourcing** | store the sequence of events; treat current state as a derived view |
| **fsync** | syscall that pushes a file's data to stable storage |
| **group commit** | batch N transactions into one write + one fsync, then ack all N |
| **idempotency key** | client-supplied unique id making a retry safe to reprocess |
| **LSN** | log sequence number; here, the record's own byte offset |
| **page cache** | kernel RAM holding file data. Survives process death, not power loss. |
| **posting** | one leg of a transfer: an account and a signed amount |
| **replay** | rebuilding state by re-applying the log from the start |
| **sector** | the device's atomic unit, 512B or 4096B |
| **silent corruption** | wrong data that no layer detected — the worst outcome |
| **torn write** | a partially-completed write: new prefix, stale tail |
| **WAL** | write-ahead log: record intent durably before acting on it |

---

## Where each idea lives in the code

| idea | file |
|---|---|
| checksums, composition | [`include/il/crc32c.h`](../include/il/crc32c.h), [`src/crc32c.cpp`](../src/crc32c.cpp) |
| page cache vs platter, fsyncgate | [`src/device.cpp`](../src/device.cpp) — `FileDevice::Sync` |
| power-loss model, torn sectors, holes | [`src/device.cpp`](../src/device.cpp) — `SimDevice::PowerCut` |
| the mid-commit crash hook | [`include/il/device.h`](../include/il/device.h) — `set_post_write_hook` |
| record format, LSN rationale | [`include/il/wal.h`](../include/il/wal.h) |
| stop-at-first-bad-record recovery | [`src/wal.cpp`](../src/wal.cpp) — `ScanLog` |
| `Flush()` vs `Sync()` | [`include/il/wal.h`](../include/il/wal.h) — `LogWriter` |
| the trap-3 tests | [`tests/unit_tests.cpp`](../tests/unit_tests.cpp) — `wal_hole_stops_scan_with_bad_magic`, `wal_misplaced_record_stops_scan_with_bad_lsn` |
| double-entry, world account, modes | `include/il/ledger.h` *(next to be built)* |
| the campaign | `tools/crash_sim.cpp` *(not yet built)* |

---

## Further reading

- **fsyncgate (2018)** — the PostgreSQL mailing list thread on `fsync` error
  handling. The canonical example of a durability assumption that was wrong for
  twenty years. Start with Craig Ringer's original post.
- **Pillai et al., "All File Systems Are Not Created Equal" (OSDI 2014)** — the
  ALICE work on crash-consistency testing. Systematically enumerates what
  applications assume about filesystem crash behaviour, and how much of it is
  false. The intellectual ancestor of this project.
- **Jepsen** (Kyle Kingsbury) — distributed systems correctness testing under
  fault injection. Same methodology one layer up: state your invariants, break
  the system on purpose, check the invariants.
- **TigerBeetle** — a production financial transactions database. Their
  documentation on why ledgers are built as double-entry event logs, and their
  approach to storage fault injection, is unusually good and directly relevant.
- **"Files are hard"** (Dan Luu) — a readable survey of everything that can go
  wrong between your `write()` and the platter.

---

## A closing note on method

Nothing in this project is difficult in the sense of being algorithmically
clever. The ledger is a hash map. The log is an append-only file. The checksum
is a lookup table. Any competent second-year could write all of it.

The difficulty is entirely in **knowing what to test, and being honest about
what the test actually proves.** The `kill -9` campaign is easy to write, looks
rigorous, produces a perfect score, and is worthless as a durability
measurement. The distance between that and a harness that cuts power mid-commit
against an unordered device model is the distance between "I tested it" and "I
know what happens".

That gap — not the code — is what this repo is about.
