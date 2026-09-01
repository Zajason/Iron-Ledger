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
