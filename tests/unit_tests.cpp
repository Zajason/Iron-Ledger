// Hand-rolled test harness. Two CHECK macros and a static registry is the
// entire framework; a crash-consistency repo that needs a dependency to run
// its own tests has already lost the argument about dependencies.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "il/crc32c.h"
#include "il/device.h"
#include "il/ledger.h"
#include "il/wal.h"

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------
namespace {

int g_checks = 0;
int g_failures_in_test = 0;

using TestFn = void (*)();
struct Entry {
  const char* name;
  TestFn fn;
};
std::vector<Entry>& Registry() {
  static std::vector<Entry> r;
  return r;
}
struct Register {
  Register(const char* name, TestFn fn) { Registry().push_back({name, fn}); }
};

template <typename T>
std::string Show(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

}  // namespace

#define TEST(name)                                    \
  static void name();                                 \
  static Register register_##name(#name, name);       \
  static void name()

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures_in_test;                                             \
      std::fprintf(stderr, "    FAIL %s:%d: CHECK(%s)\n", __FILE__,     \
                   __LINE__, #cond);                                    \
    }                                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    ++g_checks;                                                              \
    const auto va__ = (a);                                                   \
    const auto vb__ = (b);                                                   \
    if (!(va__ == vb__)) {                                                   \
      ++g_failures_in_test;                                                  \
      std::fprintf(stderr, "    FAIL %s:%d: CHECK_EQ(%s, %s) -> %s vs %s\n", \
                   __FILE__, __LINE__, #a, #b, Show(va__).c_str(),           \
                   Show(vb__).c_str());                                      \
    }                                                                        \
  } while (0)

using namespace il;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

// Collects everything a scan accepts, so tests can assert on the exact prefix.
struct Collector {
  std::vector<uint64_t> lsns;
  std::vector<uint32_t> types;
  std::vector<std::string> payloads;

  RecordVisitor visitor() {
    return [this](uint64_t lsn, uint32_t type, const uint8_t* p, uint32_t n) {
      lsns.push_back(lsn);
      types.push_back(type);
      payloads.emplace_back(reinterpret_cast<const char*>(p), n);
      return true;
    };
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// crc32c
// ---------------------------------------------------------------------------

TEST(crc32c_rfc3720_vectors) {
  std::vector<uint8_t> zeros(32, 0x00);
  std::vector<uint8_t> ones(32, 0xff);
  const std::string digits = "123456789";

  CHECK_EQ(Crc32c(zeros.data(), zeros.size()), 0x8A9136AAu);
  CHECK_EQ(Crc32c(ones.data(), ones.size()), 0x62A8AB43u);
  CHECK_EQ(Crc32c(digits.data(), digits.size()), 0xE3069283u);
}

TEST(crc32c_composes_over_adjacent_buffers) {
  const std::string all = "the quick brown fox jumps over the lazy dog";
  const uint32_t whole = Crc32c(all.data(), all.size());
  for (size_t split = 0; split <= all.size(); ++split) {
    const uint32_t part = Crc32c(all.data(), split);
    CHECK_EQ(Crc32c(all.data() + split, all.size() - split, part), whole);
  }
}

TEST(crc32c_empty_is_zero) {
  CHECK_EQ(Crc32c(nullptr, 0), 0u);
}

// ---------------------------------------------------------------------------
// SimDevice
// ---------------------------------------------------------------------------

TEST(device_basic_read_write_short_read) {
  SimDevice dev;
  const std::string a = "hello";
  dev.WriteAt(0, a.data(), a.size());
  CHECK_EQ(dev.Size(), 5u);

  char buf[16] = {};
  CHECK_EQ(dev.ReadAt(0, buf, 16), 5u);  // short read == EOF
  CHECK_EQ(std::string(buf, 5), a);
  CHECK_EQ(dev.ReadAt(5, buf, 16), 0u);
  CHECK_EQ(dev.ReadAt(99, buf, 16), 0u);
}

TEST(powercut_p0_loses_everything_unsynced) {
  SimDevice dev;
  std::vector<uint8_t> data(2048, 0xAB);
  dev.WriteAt(0, data.data(), data.size());
  std::mt19937_64 rng(1);
  auto image = dev.PowerCut(rng, CutPolicy{/*p_persist=*/0.0, /*p_tear=*/0.0});
  CHECK_EQ(image.size(), 0u);  // nothing was ever stable
}

TEST(powercut_p1_keeps_everything) {
  SimDevice dev;
  std::vector<uint8_t> data(2048, 0xAB);
  dev.WriteAt(0, data.data(), data.size());
  std::mt19937_64 rng(1);
  auto image = dev.PowerCut(rng, CutPolicy{/*p_persist=*/1.0, /*p_tear=*/0.0});
  CHECK_EQ(image.size(), data.size());
  CHECK(image == data);
}

TEST(powercut_synced_bytes_always_survive) {
  // Synced data must survive every policy and every seed, including the
  // all-lose policy: Sync() is the only promise this device makes.
  std::vector<uint8_t> synced(1536, 0x11);
  for (uint64_t seed = 0; seed < 200; ++seed) {
    SimDevice dev;
    dev.WriteAt(0, synced.data(), synced.size());
    dev.Sync();
    std::vector<uint8_t> unsynced(1024, 0x22);
    dev.WriteAt(synced.size(), unsynced.data(), unsynced.size());

    std::mt19937_64 rng(seed);
    auto image = dev.PowerCut(rng, CutPolicy{0.5, 0.3});
    CHECK(image.size() >= synced.size());
    CHECK(std::memcmp(image.data(), synced.data(), synced.size()) == 0);
  }
}

TEST(powercut_can_leave_a_hole_of_zeros) {
  // A lost sector followed by a surviving one leaves a hole in the middle of
  // the image. This is the failure mode the self-locating lsn exists for, so
  // the simulator had better be able to produce it.
  std::vector<uint8_t> s0(kSectorSize, 0xAA);
  std::vector<uint8_t> s1(kSectorSize, 0xBB);
  bool found_hole = false;
  for (uint64_t seed = 0; seed < 1000 && !found_hole; ++seed) {
    SimDevice dev;
    dev.WriteAt(0, s0.data(), s0.size());
    dev.WriteAt(kSectorSize, s1.data(), s1.size());
    std::mt19937_64 rng(seed);
    auto image = dev.PowerCut(rng, CutPolicy{/*p_persist=*/0.5, /*p_tear=*/0.0});
    if (image.size() != 2 * kSectorSize) continue;
    const bool first_lost =
        std::all_of(image.begin(), image.begin() + kSectorSize,
                    [](uint8_t b) { return b == 0; });
    const bool second_kept =
        std::memcmp(image.data() + kSectorSize, s1.data(), kSectorSize) == 0;
    if (first_lost && second_kept) found_hole = true;
  }
  CHECK(found_hole);
}

TEST(powercut_tear_keeps_a_prefix) {
  std::vector<uint8_t> data(kSectorSize, 0xCD);
  bool saw_partial = false;
  for (uint64_t seed = 0; seed < 200; ++seed) {
    SimDevice dev;
    dev.WriteAt(0, data.data(), data.size());
    std::mt19937_64 rng(seed);
    auto image = dev.PowerCut(rng, CutPolicy{/*p_persist=*/0.0, /*p_tear=*/1.0});
    CHECK(image.size() >= 1 && image.size() < kSectorSize);
    for (uint8_t b : image) CHECK_EQ(b, 0xCDu);
    if (image.size() < kSectorSize) saw_partial = true;
  }
  CHECK(saw_partial);
}

TEST(device_post_write_hook_fires_per_write) {
  SimDevice dev;
  int calls = 0;
  dev.set_post_write_hook([&calls](const SimDevice&) { ++calls; });
  const char c = 'x';
  dev.WriteAt(0, &c, 1);
  dev.WriteAt(1, &c, 1);
  dev.WriteAt(2, &c, 1);
  CHECK_EQ(calls, 3);
  CHECK_EQ(dev.write_count(), 3u);
}

TEST(filedevice_round_trip_on_a_real_file) {
  // FileDevice is the one path that touches the kernel, so it gets its own
  // test even though every other test runs against the simulator.
  const auto path = std::filesystem::temp_directory_path() /
                    ("il_filedevice_test_" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  {
    FileDevice dev(path.string(), /*truncate=*/true);
    CHECK_EQ(dev.Size(), 0u);
    const std::string a = "durable-bytes";
    dev.WriteAt(0, a.data(), a.size());
    dev.WriteAt(4096, a.data(), a.size());  // sparse write past the end
    dev.Sync();
    CHECK_EQ(dev.Size(), 4096u + a.size());
    CHECK_EQ(dev.fsync_count(), 1u);

    char buf[64] = {};
    CHECK_EQ(dev.ReadAt(0, buf, a.size()), a.size());
    CHECK_EQ(std::string(buf, a.size()), a);
    CHECK_EQ(dev.ReadAt(dev.Size() - 3, buf, 64), 3u);  // short read at EOF
    CHECK_EQ(dev.ReadAt(dev.Size(), buf, 64), 0u);
    // The hole reads back as zeros, same as a lost sector in the simulator.
    char hole[16] = {'x'};
    CHECK_EQ(dev.ReadAt(2048, hole, 16), 16u);
    for (char ch : hole) CHECK_EQ(ch, '\0');
  }
  {
    // Reopening must see exactly what was synced -- this is the recovery path.
    FileDevice dev(path.string());
    CHECK_EQ(dev.Size(), 4096u + std::string("durable-bytes").size());
    std::vector<uint8_t> all(dev.Size());
    CHECK_EQ(dev.ReadAt(0, all.data(), all.size()), all.size());
  }
  std::filesystem::remove(path);
}

TEST(filedevice_log_round_trip) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("il_filelog_test_" + std::to_string(::getpid()) + ".log");
  std::filesystem::remove(path);
  std::vector<std::string> payloads;
  {
    FileDevice dev(path.string(), true);
    LogWriter w(dev, 0);
    for (int i = 0; i < 6; ++i) {
      payloads.push_back("file-record-" + std::to_string(i));
      w.Append(3, payloads.back().data(),
               static_cast<uint32_t>(payloads.back().size()));
    }
    w.Sync();
  }
  {
    FileDevice dev(path.string());
    Collector c;
    auto res = ScanLog(dev, true, c.visitor());
    CHECK_EQ(res.records, payloads.size());
    CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kCleanEnd));
    CHECK(c.payloads == payloads);
  }
  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// wal
// ---------------------------------------------------------------------------

TEST(wal_round_trip) {
  SimDevice dev;
  LogWriter w(dev, 0);
  std::vector<std::string> payloads = {"alpha", "", "gamma-payload", "d"};
  std::vector<uint64_t> lsns;
  for (size_t i = 0; i < payloads.size(); ++i) {
    lsns.push_back(w.Append(static_cast<uint32_t>(i + 1), payloads[i].data(),
                            static_cast<uint32_t>(payloads[i].size())));
  }
  w.Sync();

  Collector c;
  auto res = ScanLog(dev, /*verify_crc=*/true, c.visitor());
  CHECK_EQ(res.records, payloads.size());
  CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kCleanEnd));
  CHECK_EQ(res.valid_bytes, dev.Size());
  CHECK(c.payloads == payloads);
  CHECK(c.lsns == lsns);
  for (size_t i = 0; i < payloads.size(); ++i) {
    CHECK_EQ(c.types[i], static_cast<uint32_t>(i + 1));
    // The lsn is the record's own offset.
    CHECK_EQ(c.lsns[i], lsns[i]);
  }
}

TEST(wal_lsn_is_own_offset) {
  SimDevice dev;
  LogWriter w(dev, 0);
  uint64_t expected = 0;
  for (int i = 0; i < 10; ++i) {
    const std::string p(static_cast<size_t>(i), 'z');
    const uint64_t lsn = w.Append(1, p.data(), static_cast<uint32_t>(p.size()));
    CHECK_EQ(lsn, expected);
    expected += kHeaderSize + p.size();
  }
  w.Sync();
  CHECK_EQ(dev.Size(), expected);
}

TEST(wal_truncated_tail_recovers_strict_prefix) {
  SimDevice full;
  LogWriter w(full, 0);
  std::vector<std::string> payloads;
  std::vector<uint64_t> boundaries{0};
  for (int i = 0; i < 8; ++i) {
    payloads.push_back("record-" + std::to_string(i));
    w.Append(7, payloads.back().data(),
             static_cast<uint32_t>(payloads.back().size()));
    boundaries.push_back(w.next_lsn());
  }
  w.Sync();
  const auto image = full.volatile_image();

  // Truncating anywhere must yield a prefix of the records, never a phantom.
  for (size_t cut = 0; cut <= image.size(); ++cut) {
    std::vector<uint8_t> partial(image.begin(), image.begin() + static_cast<long>(cut));
    SimDevice dev(partial);
    Collector c;
    auto res = ScanLog(dev, true, c.visitor());

    size_t expect = 0;
    while (expect + 1 < boundaries.size() && boundaries[expect + 1] <= cut) ++expect;
    CHECK_EQ(res.records, expect);
    CHECK_EQ(res.valid_bytes, boundaries[expect]);
    for (size_t i = 0; i < expect; ++i) CHECK_EQ(c.payloads[i], payloads[i]);

    const bool on_boundary = (cut == boundaries[expect]);
    if (on_boundary) {
      CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kCleanEnd));
    } else {
      CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kShortRead));
    }
  }
}

TEST(wal_flipped_bit_caught_with_crc_and_missed_without) {
  SimDevice full;
  LogWriter w(full, 0);
  for (int i = 0; i < 4; ++i) {
    const std::string p = "payload-" + std::to_string(i);
    w.Append(1, p.data(), static_cast<uint32_t>(p.size()));
  }
  w.Sync();
  auto image = full.volatile_image();

  // Flip one bit inside the payload of the second record.
  const size_t rec1_payload = 2 * kHeaderSize + std::string("payload-0").size() + 1;
  image[rec1_payload] ^= 0x01;

  {
    SimDevice dev(image);
    Collector c;
    auto res = ScanLog(dev, /*verify_crc=*/true, c.visitor());
    CHECK_EQ(res.records, 1u);  // stops at the damaged record
    CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kBadCrc));
  }
  {
    // Checksums off: the same bit flip sails straight through and the caller
    // is handed corrupt bytes as though they were committed data.
    SimDevice dev(image);
    Collector c;
    auto res = ScanLog(dev, /*verify_crc=*/false, c.visitor());
    CHECK_EQ(res.records, 4u);
    CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kCleanEnd));
    CHECK(c.payloads[1] != "payload-1");
  }
}

TEST(wal_hole_stops_scan_with_bad_magic) {
  // Trap 3. A lost sector in the middle of the log zeroes a record. The scan
  // must stop AT the hole -- not resynchronise onto the intact records after
  // it, which would replay later transfers as though the missing ones had
  // never been submitted.
  SimDevice full;
  LogWriter w(full, 0);
  std::vector<uint64_t> lsns;
  for (int i = 0; i < 5; ++i) {
    const std::string p = "rec" + std::to_string(i);
    lsns.push_back(w.Append(1, p.data(), static_cast<uint32_t>(p.size())));
  }
  w.Sync();
  auto image = full.volatile_image();

  const size_t hole_begin = lsns[2];
  const size_t hole_end = lsns[3];
  std::memset(image.data() + hole_begin, 0, hole_end - hole_begin);

  SimDevice dev(image);
  Collector c;
  auto res = ScanLog(dev, /*verify_crc=*/true, c.visitor());
  CHECK_EQ(res.records, 2u);
  CHECK_EQ(res.valid_bytes, lsns[2]);
  CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kBadMagic));

  // And with checksums off it must STILL stop: the magic check is what catches
  // a hole, so this failure mode is caught even in the crc=0 arm.
  SimDevice dev2(image);
  Collector c2;
  auto res2 = ScanLog(dev2, /*verify_crc=*/false, c2.visitor());
  CHECK_EQ(res2.records, 2u);
  CHECK_EQ(static_cast<int>(res2.stop), static_cast<int>(ScanStop::kBadMagic));
}

TEST(wal_misplaced_record_stops_scan_with_bad_lsn) {
  // The other half of trap 3: a record that verifies perfectly but belongs at
  // a different offset. Only the self-locating lsn catches this -- magic and
  // crc both pass.
  SimDevice full;
  LogWriter w(full, 0);
  const std::string a = "first", b = "second";
  w.Append(1, a.data(), static_cast<uint32_t>(a.size()));
  const uint64_t lsn_b = w.Append(1, b.data(), static_cast<uint32_t>(b.size()));
  w.Sync();
  const auto image = full.volatile_image();

  // Drop the first record entirely, so record two now sits at offset 0.
  std::vector<uint8_t> shifted(image.begin() + static_cast<long>(lsn_b), image.end());
  SimDevice dev(shifted);
  Collector c;
  auto res = ScanLog(dev, /*verify_crc=*/true, c.visitor());
  CHECK_EQ(res.records, 0u);
  CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kBadLsn));
}

TEST(wal_visitor_rejection_stops_scan) {
  SimDevice dev;
  LogWriter w(dev, 0);
  for (int i = 0; i < 4; ++i) {
    const std::string p = "x" + std::to_string(i);
    w.Append(1, p.data(), static_cast<uint32_t>(p.size()));
  }
  w.Sync();

  int seen = 0;
  auto res = ScanLog(dev, true, [&seen](uint64_t, uint32_t, const uint8_t*, uint32_t) {
    return ++seen < 3;  // reject the third
  });
  CHECK_EQ(res.records, 2u);
  CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kBadPayload));
}

TEST(wal_flush_reaches_device_sync_reaches_platter) {
  // The distinction the whole project rests on: after Flush() the bytes are
  // readable through the device but would not survive a power cut; only after
  // Sync() are they stable.
  SimDevice dev;
  LogWriter w(dev, 0);
  const std::string p = "committed?";
  w.Append(1, p.data(), static_cast<uint32_t>(p.size()));

  CHECK_EQ(dev.Size(), 0u);         // still buffered in the writer
  w.Flush();
  CHECK(dev.Size() > 0);            // in the device's volatile cache
  CHECK_EQ(dev.stable_image().size(), 0u);
  CHECK(dev.dirty_sectors() > 0);

  std::mt19937_64 rng(4);
  CHECK_EQ(dev.PowerCut(rng, CutPolicy{0.0, 0.0}).size(), 0u);  // would be lost

  w.Sync();
  CHECK_EQ(dev.stable_image().size(), dev.Size());
  CHECK_EQ(dev.dirty_sectors(), 0u);
  std::mt19937_64 rng2(4);
  CHECK_EQ(dev.PowerCut(rng2, CutPolicy{0.0, 0.0}).size(), dev.Size());  // survives
}

TEST(wal_bad_length_is_rejected) {
  // A corrupt length field must not drive a huge allocation.
  std::vector<uint8_t> image;
  const std::string p = "ok";
  EncodeRecord(image, 0, 1, p.data(), static_cast<uint32_t>(p.size()));
  const uint64_t lsn1 = image.size();
  EncodeRecord(image, lsn1, 1, p.data(), static_cast<uint32_t>(p.size()));
  // Smash the length of the second record (offset lsn1 + 20).
  image[lsn1 + 20] = 0xFF;
  image[lsn1 + 21] = 0xFF;
  image[lsn1 + 22] = 0xFF;
  image[lsn1 + 23] = 0x7F;

  SimDevice dev(image);
  Collector c;
  auto res = ScanLog(dev, /*verify_crc=*/false, c.visitor());
  CHECK_EQ(res.records, 1u);
  CHECK_EQ(static_cast<int>(res.stop), static_cast<int>(ScanStop::kBadLength));
}

TEST(wal_recovers_from_simulated_power_cut) {
  // End to end: run writes through a SimDevice, cut the power, and check that
  // whatever survives is always a clean prefix -- never a phantom record.
  for (uint64_t seed = 0; seed < 300; ++seed) {
    SimDevice dev;
    LogWriter w(dev, 0);
    std::vector<std::string> submitted;
    for (int i = 0; i < 12; ++i) {
      submitted.push_back("txn-" + std::to_string(i) + std::string(40, '.'));
      w.Append(1, submitted.back().data(),
               static_cast<uint32_t>(submitted.back().size()));
      w.Flush();
      if (i % 4 == 3) w.Sync();
    }
    std::mt19937_64 rng(seed);
    auto image = dev.PowerCut(rng, CutPolicy{0.6, 0.2});

    SimDevice recovered(image);
    Collector c;
    auto res = ScanLog(recovered, /*verify_crc=*/true, c.visitor());
    CHECK(res.records <= submitted.size());
    for (size_t i = 0; i < c.payloads.size(); ++i) {
      CHECK_EQ(c.payloads[i], submitted[i]);  // strict prefix, in order
    }
  }
}

// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Step 1: transfer wire format
// ---------------------------------------------------------------------------

namespace {

Transfer MakeTransfer(uint64_t key, std::vector<Posting> postings) {
  Transfer t;
  t.idem_key = key;
  t.postings = std::move(postings);
  return t;
}

}  // namespace

TEST(transfer_wire_round_trip) {
  // Every legal posting count, including the boundaries.
  for (size_t n = kMinPostings; n <= kMaxPostings; ++n) {
    std::vector<Posting> ps;
    int64_t running = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
      const int64_t amt = static_cast<int64_t>(100 + i);
      ps.push_back({i + 1, amt});
      running += amt;
    }
    ps.push_back({999, -running});  // balancing leg

    const Transfer in = MakeTransfer(7000 + n, ps);
    std::vector<uint8_t> buf;
    EncodeTransfer(buf, in);
    CHECK_EQ(buf.size(), 12u + 16u * n);

    Transfer out;
    CHECK(DecodeTransfer(buf.data(), static_cast<uint32_t>(buf.size()), &out));
    CHECK_EQ(out.idem_key, in.idem_key);
    CHECK_EQ(out.postings.size(), in.postings.size());
    for (size_t i = 0; i < n; ++i) {
      CHECK_EQ(out.postings[i].account, in.postings[i].account);
      CHECK_EQ(out.postings[i].amount, in.postings[i].amount);
    }
  }
}

TEST(transfer_wire_preserves_negative_amounts) {
  // Two's complement round-trip through the unsigned wire field, including the
  // extremes where a sloppy cast would go wrong.
  const std::vector<int64_t> amounts = {
      -1, -1000, INT64_MIN + 1, INT64_MAX, 0,
  };
  for (int64_t a : amounts) {
    const Transfer in = MakeTransfer(1, {{1, a}, {2, 0}});
    std::vector<uint8_t> buf;
    EncodeTransfer(buf, in);
    Transfer out;
    CHECK(DecodeTransfer(buf.data(), static_cast<uint32_t>(buf.size()), &out));
    CHECK_EQ(out.postings[0].amount, a);
  }
}

TEST(transfer_wire_rejects_truncation) {
  const Transfer in = MakeTransfer(42, {{1, -50}, {2, 50}});
  std::vector<uint8_t> buf;
  EncodeTransfer(buf, in);

  // Every proper prefix must be rejected. None of them may be mistaken for a
  // shorter but valid transfer.
  for (uint32_t len = 0; len < buf.size(); ++len) {
    Transfer out;
    CHECK(!DecodeTransfer(buf.data(), len, &out));
  }
  Transfer ok;
  CHECK(DecodeTransfer(buf.data(), static_cast<uint32_t>(buf.size()), &ok));
}

TEST(transfer_wire_rejects_trailing_bytes) {
  // A torn write can leave a valid transfer followed by stale bytes. Ignoring
  // the tail would quietly accept that, so the length check is exact.
  const Transfer in = MakeTransfer(42, {{1, -50}, {2, 50}});
  std::vector<uint8_t> buf;
  EncodeTransfer(buf, in);
  buf.push_back(0xAB);
  Transfer out;
  CHECK(!DecodeTransfer(buf.data(), static_cast<uint32_t>(buf.size()), &out));
}

TEST(transfer_wire_rejects_bad_posting_count) {
  // Hand-build headers claiming counts outside [kMinPostings, kMaxPostings].
  // A corrupt count field must not drive a huge allocation or an over-read.
  for (uint32_t n : {0u, 1u, 9u, 1000u, 0xFFFFFFFFu}) {
    std::vector<uint8_t> buf;
    for (int i = 0; i < 8; ++i) buf.push_back(0);          // idem_key
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(n >> (8 * i)));
    buf.resize(buf.size() + 16 * 2);                        // two plausible postings
    Transfer out;
    CHECK(!DecodeTransfer(buf.data(), static_cast<uint32_t>(buf.size()), &out));
  }
}

TEST(transfer_wire_is_byte_stable_little_endian) {
  // Golden bytes: pins the format so a log written on arm64 replays on x86.
  const Transfer in = MakeTransfer(0x0102030405060708ull,
                                   {{0x1122334455667788ull, -2}, {1, 2}});
  std::vector<uint8_t> buf;
  EncodeTransfer(buf, in);

  const std::vector<uint8_t> want = {
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // key, LE
      0x02, 0x00, 0x00, 0x00,                          // n = 2
      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // account
      0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // -2
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // account 1
      0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // +2
  };
  CHECK_EQ(buf.size(), want.size());
  CHECK(buf == want);
}


// ---------------------------------------------------------------------------
// Steps 2 & 3: ledger domain, recovery, and the ack point
// ---------------------------------------------------------------------------

namespace {

// Books |amount| into |account| from the world account.
SubmitStatus Fund(Ledger& l, uint64_t account, int64_t amount, uint64_t key) {
  return l.Submit({key, {{kWorldAccount, -amount}, {account, amount}}});
}

SubmitStatus Pay(Ledger& l, uint64_t from, uint64_t to, int64_t amount, uint64_t key) {
  return l.Submit({key, {{from, -amount}, {to, amount}}});
}

// The property every durability mode is defined by: at this instant, would a
// ledger recovered from *the platter* contain this key? Rebuilding from
// stable_image() is exactly the check the crash campaign performs, so the ack
// point is tested the same way it will be measured.
bool KeyDurableNow(const SimDevice& dev, uint64_t key, bool verify_crc) {
  SimDevice platter(dev.stable_image());
  LedgerOptions o;
  o.verify_crc = verify_crc;
  Ledger recovered(platter, o);
  return recovered.HasKey(key);
}

LedgerOptions Opts(DurabilityMode m, uint32_t group = 32, bool crc = true) {
  LedgerOptions o;
  o.mode = m;
  o.group_size = group;
  o.verify_crc = crc;
  return o;
}

}  // namespace

TEST(ledger_transfer_moves_money_and_conserves) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));

  CHECK(Fund(l, 1, 1000, 1) == SubmitStatus::kOk);
  CHECK(Pay(l, 1, 2, 250, 2) == SubmitStatus::kOk);

  CHECK_EQ(l.Balance(1), int64_t{750});
  CHECK_EQ(l.Balance(2), int64_t{250});
  // The world holds the negative of all real money in the system.
  CHECK_EQ(l.Balance(kWorldAccount), int64_t{-1000});
  CHECK_EQ(l.TotalBalance(), int64_t{0});
  CHECK(l.ConservationHolds());
}

TEST(ledger_multi_leg_transfer_settles_atomically) {
  // A card payment: customer, merchant, processor, network in one event.
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  CHECK(Fund(l, 1, 5000, 1) == SubmitStatus::kOk);

  CHECK(l.Submit({2, {{1, -1000}, {2, 971}, {3, 25}, {4, 4}}}) == SubmitStatus::kOk);
  CHECK_EQ(l.Balance(1), int64_t{4000});
  CHECK_EQ(l.Balance(2), int64_t{971});
  CHECK_EQ(l.Balance(3), int64_t{25});
  CHECK_EQ(l.Balance(4), int64_t{4});
  CHECK(l.ConservationHolds());
}

TEST(ledger_duplicate_key_is_a_noop) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  CHECK(Fund(l, 1, 1000, 1) == SubmitStatus::kOk);

  const uint64_t size_before = dev.Size();
  CHECK(Fund(l, 1, 1000, 1) == SubmitStatus::kDuplicate);
  CHECK_EQ(dev.Size(), size_before);     // nothing was logged
  CHECK_EQ(l.Balance(1), int64_t{1000});  // and nothing was applied twice
}

TEST(ledger_insufficient_funds_rejected_and_not_logged) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  CHECK(Fund(l, 1, 100, 1) == SubmitStatus::kOk);

  const uint64_t size_before = dev.Size();
  CHECK(Pay(l, 1, 2, 101, 2) == SubmitStatus::kInsufficientFunds);
  CHECK_EQ(dev.Size(), size_before);
  CHECK_EQ(l.Balance(1), int64_t{100});
  CHECK_EQ(l.Balance(2), int64_t{0});
  CHECK(!l.HasKey(2));  // a rejected key stays unseen, so a retry can succeed
}

TEST(ledger_world_account_may_go_negative) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  // No funding at all: the world starts at zero and is allowed below it.
  CHECK(Fund(l, 1, 10'000, 1) == SubmitStatus::kOk);
  CHECK_EQ(l.Balance(kWorldAccount), int64_t{-10'000});
  CHECK(l.ConservationHolds());
}

TEST(ledger_unbalanced_postings_never_reach_the_log) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  const uint64_t size_before = dev.Size();

  CHECK(l.Submit({1, {{1, -100}, {2, 99}}}) == SubmitStatus::kUnbalanced);
  CHECK(l.Submit({2, {{1, -100}}}) == SubmitStatus::kBadPostingCount);
  CHECK(l.Submit({3, {}}) == SubmitStatus::kBadPostingCount);
  CHECK(l.Submit({4, {{1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1},
                      {6, 1}, {7, 1}, {8, 1}, {9, -8}}}) == SubmitStatus::kBadPostingCount);

  CHECK_EQ(dev.Size(), size_before);
  CHECK(l.ConservationHolds());
}

TEST(ledger_overflowing_postings_are_rejected) {
  // Two amounts that wrap to a sum of zero in wrapping arithmetic must not be
  // mistaken for a balanced transfer.
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  CHECK(l.Submit({1, {{1, INT64_MAX}, {2, INT64_MAX}, {3, 2}}}) !=
        SubmitStatus::kOk);
  CHECK(l.ConservationHolds());
}

TEST(ledger_empty_device_recovers_to_nothing) {
  SimDevice dev;
  RecoveryReport r;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery), &r);
  CHECK_EQ(r.records_applied, uint64_t{0});
  CHECK_EQ(r.valid_bytes, uint64_t{0});
  CHECK(r.stop == ScanStop::kCleanEnd);
  CHECK(!r.corruption_admitted());
  CHECK_EQ(l.TotalBalance(), int64_t{0});
}

TEST(ledger_recovery_reproduces_state_exactly) {
  std::vector<uint8_t> image;
  std::map<uint64_t, int64_t> want_balances;
  std::vector<uint64_t> want_keys;

  {
    SimDevice dev;
    Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
    CHECK(Fund(l, 1, 1'000'000, 1) == SubmitStatus::kOk);
    CHECK(Fund(l, 2, 1'000'000, 2) == SubmitStatus::kOk);
    for (uint64_t k = 3; k < 40; ++k) {
      CHECK(Pay(l, 1 + (k % 2), 2 - (k % 2), static_cast<int64_t>(k * 7), k) ==
            SubmitStatus::kOk);
    }
    l.Commit();
    want_balances = l.balances();
    want_keys = l.applied_keys();
    image = dev.stable_image();
  }

  SimDevice reopened(image);
  RecoveryReport r;
  Ledger l2(reopened, Opts(DurabilityMode::kSyncEvery), &r);

  CHECK(!r.corruption_admitted());
  CHECK(r.stop == ScanStop::kCleanEnd);
  CHECK(l2.balances() == want_balances);   // exact state, not merely plausible
  CHECK(l2.applied_keys() == want_keys);   // exact order
  CHECK(l2.ConservationHolds());
}

TEST(ledger_reopened_ledger_keeps_working_and_still_dedups) {
  std::vector<uint8_t> image;
  {
    SimDevice dev;
    Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
    CHECK(Fund(l, 1, 1000, 1) == SubmitStatus::kOk);
    CHECK(Pay(l, 1, 2, 100, 2) == SubmitStatus::kOk);
    l.Commit();
    image = dev.stable_image();
  }

  SimDevice dev(image);
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));

  // Dedup survived the restart, rebuilt from the log with no separate table.
  CHECK(Pay(l, 1, 2, 100, 2) == SubmitStatus::kDuplicate);
  CHECK_EQ(l.Balance(2), int64_t{100});

  // And the reopened ledger still accepts new work, appended after the
  // recovered prefix.
  CHECK(Pay(l, 1, 2, 50, 3) == SubmitStatus::kOk);
  l.Commit();
  CHECK_EQ(l.Balance(1), int64_t{850});
  CHECK_EQ(l.Balance(2), int64_t{150});
  CHECK(l.ConservationHolds());

  // Reopening a third time sees all three transfers.
  SimDevice again(dev.stable_image());
  Ledger l3(again, Opts(DurabilityMode::kSyncEvery));
  CHECK_EQ(l3.applied_keys().size(), size_t{3});
  CHECK_EQ(l3.Balance(2), int64_t{150});
}

TEST(ledger_truncated_tail_recovers_a_strict_prefix) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  CHECK(Fund(l, 1, 100'000, 1) == SubmitStatus::kOk);
  for (uint64_t k = 2; k <= 20; ++k) CHECK(Pay(l, 1, 2, 10, k) == SubmitStatus::kOk);
  l.Commit();

  const std::vector<uint8_t> full = dev.stable_image();

  // Chop the log at every possible byte. Every truncation must yield a strict
  // prefix of the key sequence -- never a phantom, never a reordering.
  for (size_t cut = 0; cut < full.size(); cut += 7) {
    std::vector<uint8_t> shorter(full.begin(), full.begin() + cut);
    SimDevice d(shorter);
    Ledger r(d, Opts(DurabilityMode::kSyncEvery));

    const std::vector<uint64_t>& got = r.applied_keys();
    CHECK(got.size() <= size_t{20});
    for (size_t i = 0; i < got.size(); ++i) CHECK_EQ(got[i], uint64_t{i + 1});
    CHECK(r.ConservationHolds());
  }
}

TEST(ledger_corrupt_payload_caught_by_domain_rule_when_crc_is_off) {
  // The torn_accepted case: the checksum layer is disabled, so a damaged
  // payload reaches the domain layer, and an ordinary business rule is the only
  // thing that catches it. This is why replay re-validates.
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  CHECK(Fund(l, 1, 1000, 1) == SubmitStatus::kOk);
  l.Commit();

  std::vector<uint8_t> image = dev.stable_image();
  // First record: 24-byte header, then key(8) + count(4), then the first
  // posting's account(8) and amount(8). Break the amount so the sum is nonzero.
  const size_t amount_off = kHeaderSize + 8 + 4 + 8;
  CHECK(image.size() > amount_off);
  image[amount_off] ^= 0x40;

  SimDevice damaged(image);
  RecoveryReport r;
  Ledger recovered(damaged, Opts(DurabilityMode::kSyncEvery, 32, /*crc=*/false), &r);

  CHECK(r.corruption_admitted());               // something got past the checksum
  CHECK_EQ(r.unbalanced_in_log, uint64_t{1});   // caught by the balance rule
  CHECK_EQ(r.records_applied, uint64_t{0});
  CHECK(recovered.ConservationHolds());         // and we did not apply it
}

TEST(ledger_same_corruption_is_caught_by_the_checksum_when_crc_is_on) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));
  CHECK(Fund(l, 1, 1000, 1) == SubmitStatus::kOk);
  l.Commit();

  std::vector<uint8_t> image = dev.stable_image();
  image[kHeaderSize + 8 + 4 + 8] ^= 0x40;

  SimDevice damaged(image);
  RecoveryReport r;
  Ledger recovered(damaged, Opts(DurabilityMode::kSyncEvery, 32, /*crc=*/true), &r);

  CHECK(r.stop == ScanStop::kBadCrc);      // stopped one layer earlier
  CHECK(!r.corruption_admitted());         // the domain rules never saw it
  CHECK_EQ(r.records_applied, uint64_t{0});
}

// --- Step 3: the ack point -------------------------------------------------

TEST(ack_sync_every_never_acks_before_the_platter) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kSyncEvery));

  int acks = 0, durable_at_ack = 0;
  l.set_ack_sink([&](uint64_t key) {
    ++acks;
    if (KeyDurableNow(dev, key, true)) ++durable_at_ack;
  });

  CHECK(Fund(l, 1, 100'000, 1) == SubmitStatus::kOk);
  for (uint64_t k = 2; k <= 30; ++k) CHECK(Pay(l, 1, 2, 10, k) == SubmitStatus::kOk);
  l.Commit();

  CHECK_EQ(acks, 30);
  CHECK_EQ(durable_at_ack, acks);   // every single ack was already on the platter
  CHECK_EQ(dev.sync_count(), uint64_t{30});
}

TEST(ack_group_commit_never_acks_before_the_platter) {
  SimDevice dev;
  const uint32_t group = 8;
  Ledger l(dev, Opts(DurabilityMode::kGroupCommit, group));

  int acks = 0, durable_at_ack = 0;
  l.set_ack_sink([&](uint64_t key) {
    ++acks;
    if (KeyDurableNow(dev, key, true)) ++durable_at_ack;
  });

  CHECK(Fund(l, 1, 100'000, 1) == SubmitStatus::kOk);
  for (uint64_t k = 2; k <= 32; ++k) CHECK(Pay(l, 1, 2, 10, k) == SubmitStatus::kOk);
  l.Commit();

  CHECK_EQ(acks, 32);
  CHECK_EQ(durable_at_ack, acks);   // exactly as correct as sync_every...
  // ...but at a fraction of the fsyncs: 32 records / 8 per batch = 4.
  CHECK_EQ(dev.sync_count(), uint64_t{4});
}

TEST(ack_no_sync_acks_ahead_of_durability_and_never_fsyncs) {
  SimDevice dev;
  Ledger l(dev, Opts(DurabilityMode::kNoSync));

  int acks = 0, durable_at_ack = 0;
  l.set_ack_sink([&](uint64_t key) {
    ++acks;
    if (KeyDurableNow(dev, key, true)) ++durable_at_ack;
  });

  CHECK(Fund(l, 1, 100'000, 1) == SubmitStatus::kOk);
  for (uint64_t k = 2; k <= 30; ++k) CHECK(Pay(l, 1, 2, 10, k) == SubmitStatus::kOk);
  l.Commit();

  CHECK_EQ(acks, 30);
  CHECK_EQ(durable_at_ack, 0);            // not one ack was backed by the platter
  CHECK_EQ(dev.sync_count(), uint64_t{0});  // not even on Commit()
}

TEST(ack_lazy_sync_acks_ahead_of_durability_by_up_to_n_records) {
  // The mode that looks perfect on a machine that never loses power. The ack
  // runs ahead of the fsync by up to group_size records, and this test pins the
  // size of that window.
  SimDevice dev;
  const uint32_t n = 8;
  Ledger l(dev, Opts(DurabilityMode::kLazySync, n));

  int acks = 0, durable_at_ack = 0;
  l.set_ack_sink([&](uint64_t key) {
    ++acks;
    if (KeyDurableNow(dev, key, true)) ++durable_at_ack;
  });

  CHECK(Fund(l, 1, 100'000, 1) == SubmitStatus::kOk);
  for (uint64_t k = 2; k <= 32; ++k) CHECK(Pay(l, 1, 2, 10, k) == SubmitStatus::kOk);

  CHECK_EQ(acks, 32);
  CHECK_EQ(durable_at_ack, 0);  // the ack always precedes the sync it relies on
  CHECK_EQ(dev.sync_count(), uint64_t{32 / n});
}

TEST(ack_durable_modes_lose_nothing_across_a_power_cut) {
  // The campaign's headline check, in miniature: cut the power at a random
  // moment and demand that every acknowledged key survived.
  for (DurabilityMode mode :
       {DurabilityMode::kSyncEvery, DurabilityMode::kGroupCommit}) {
    for (uint64_t seed = 1; seed <= 40; ++seed) {
      SimDevice dev;
      Ledger l(dev, Opts(mode, 4));

      std::vector<uint64_t> acked;
      l.set_ack_sink([&](uint64_t key) { acked.push_back(key); });

      CHECK(Fund(l, 1, 100'000, 1) == SubmitStatus::kOk);
      for (uint64_t k = 2; k <= 25; ++k) CHECK(Pay(l, 1, 2, 10, k) == SubmitStatus::kOk);

      std::mt19937_64 rng(seed);
      CutPolicy policy;
      policy.p_persist = 0.5;
      policy.p_tear = 0.2;
      SimDevice after(dev.PowerCut(rng, policy));

      RecoveryReport r;
      Ledger recovered(after, Opts(mode, 4), &r);

      for (uint64_t key : acked) CHECK(recovered.HasKey(key));
      CHECK(recovered.ConservationHolds());
    }
  }
}

int main() {
  int failed_tests = 0;
  for (const auto& t : Registry()) {
    g_failures_in_test = 0;
    std::printf("  %-52s", t.name);
    std::fflush(stdout);
    t.fn();
    if (g_failures_in_test == 0) {
      std::printf("ok\n");
    } else {
      std::printf("FAILED (%d)\n", g_failures_in_test);
      ++failed_tests;
    }
  }
  std::printf("\n%zu tests, %d checks, %d failed\n", Registry().size(), g_checks,
              failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
