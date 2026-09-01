// Block-device abstraction.
//
// Everything above this header (the WAL, the ledger, recovery) talks only to
// Device. That is what makes crashes reproducible: the exact same ledger code
// runs against a real file in the benchmark and against a simulated volatile
// write cache in the crash campaign.
#ifndef IL_DEVICE_H_
#define IL_DEVICE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <random>
#include <string>
#include <vector>

namespace il {

class Device {
 public:
  virtual ~Device() = default;

  // Writes |n| bytes. Writing past the end extends the device.
  virtual void WriteAt(uint64_t off, const void* buf, size_t n) = 0;

  // Reads up to |n| bytes; a short read (including 0) means end of device.
  virtual size_t ReadAt(uint64_t off, void* buf, size_t n) const = 0;

  // Moves everything written so far to stable storage.
  virtual void Sync() = 0;

  virtual uint64_t Size() const = 0;
};

// ---------------------------------------------------------------------------
// FileDevice: a real file. pwrite/pread, and Sync() is a real fsync(2).
// ---------------------------------------------------------------------------
class FileDevice : public Device {
 public:
  // Opens (creating if needed) |path|. Throws std::runtime_error on failure.
  explicit FileDevice(const std::string& path, bool truncate = false);
  ~FileDevice() override;

  FileDevice(const FileDevice&) = delete;
  FileDevice& operator=(const FileDevice&) = delete;

  void WriteAt(uint64_t off, const void* buf, size_t n) override;
  size_t ReadAt(uint64_t off, void* buf, size_t n) const override;
  void Sync() override;
  uint64_t Size() const override;

  uint64_t fsync_count() const { return fsync_count_; }

 private:
  int fd_ = -1;
  std::string path_;
  uint64_t size_ = 0;
  uint64_t fsync_count_ = 0;
};

// ---------------------------------------------------------------------------
// SimDevice: the power-loss simulator.
//
// Model: a write lands in a volatile cache, tracked at 512-byte sector
// granularity. Only Sync() moves dirty sectors to stable storage. PowerCut()
// asks: if the plug came out right now, what bytes would be on the platter?
//
// Each dirty sector is resolved INDEPENDENTLY -- persisted in full, lost
// entirely, or torn (a random prefix persisted, the tail still stale). That is
// deliberately harsher than a real drive, which has a good deal of internal
// ordering. Independent resolution breaks the write-ordering assumption that
// unsynced logs quietly rely on, which is exactly the assumption under test.
// ---------------------------------------------------------------------------
constexpr size_t kSectorSize = 512;

struct CutPolicy {
  // Per dirty sector: persist with p_persist; else tear with p_tear; else the
  // sector is lost entirely. Requires p_persist + p_tear <= 1.
  double p_persist = 0.5;
  double p_tear = 0.2;
};

class SimDevice : public Device {
 public:
  SimDevice() = default;

  // Starts from an existing byte image, all of it already stable. This is how
  // recovery is run against post-crash bytes.
  explicit SimDevice(std::vector<uint8_t> image);

  void WriteAt(uint64_t off, const void* buf, size_t n) override;
  size_t ReadAt(uint64_t off, void* buf, size_t n) const override;
  void Sync() override;
  uint64_t Size() const override;

  // The byte image that would be on the platter if power were lost now.
  // Const, so it can be called from the post-write hook.
  std::vector<uint8_t> PowerCut(std::mt19937_64& rng, const CutPolicy& policy) const;

  // Called at the end of every WriteAt. The crash harness uses this to cut
  // power *between a device write and the fsync that follows it*, which is the
  // only window in which a torn record can exist.
  void set_post_write_hook(std::function<void(const SimDevice&)> hook) {
    post_write_hook_ = std::move(hook);
  }

  uint64_t write_count() const { return write_count_; }
  uint64_t sync_count() const { return sync_count_; }
  size_t dirty_sectors() const { return dirty_.size(); }

  // The bytes a reader in this process sees (cache included).
  const std::vector<uint8_t>& volatile_image() const { return volatile_; }
  // The bytes currently on the platter.
  const std::vector<uint8_t>& stable_image() const { return stable_; }

 private:
  std::vector<uint8_t> volatile_;  // what this process reads back
  std::vector<uint8_t> stable_;    // what survives a power cut for free
  std::set<uint64_t> dirty_;       // sector indices written but not yet synced

  std::function<void(const SimDevice&)> post_write_hook_;
  uint64_t write_count_ = 0;
  uint64_t sync_count_ = 0;
};

}  // namespace il

#endif  // IL_DEVICE_H_
