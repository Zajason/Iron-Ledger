#include "il/device.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace il {

// ---------------------------------------------------------------------------
// FileDevice
// ---------------------------------------------------------------------------

FileDevice::FileDevice(const std::string& path, bool truncate) : path_(path) {
  int flags = O_RDWR | O_CREAT;
  if (truncate) flags |= O_TRUNC;
  fd_ = ::open(path.c_str(), flags, 0644);
  if (fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "open " + path);
  }
  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    int e = errno;
    ::close(fd_);
    throw std::system_error(e, std::generic_category(), "fstat " + path);
  }
  size_ = static_cast<uint64_t>(st.st_size);
}

FileDevice::~FileDevice() {
  if (fd_ >= 0) ::close(fd_);
}

void FileDevice::WriteAt(uint64_t off, const void* buf, size_t n) {
  const auto* p = static_cast<const uint8_t*>(buf);
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::pwrite(fd_, p + done, n - done, static_cast<off_t>(off + done));
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(), "pwrite " + path_);
    }
    if (w == 0) throw std::runtime_error("pwrite made no progress on " + path_);
    done += static_cast<size_t>(w);
  }
  size_ = std::max(size_, off + n);
}

size_t FileDevice::ReadAt(uint64_t off, void* buf, size_t n) const {
  auto* p = static_cast<uint8_t*>(buf);
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd_, p + done, n - done, static_cast<off_t>(off + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(), "pread " + path_);
    }
    if (r == 0) break;  // EOF: a short read is how the caller learns the end.
    done += static_cast<size_t>(r);
  }
  return done;
}

void FileDevice::Sync() {
  if (::fsync(fd_) != 0) {
    // Do NOT swallow this and carry on.
    //
    // On Linux, a failed writeback marks the dirty pages clean anyway and
    // reports the error exactly once, to whichever fd happens to call fsync
    // next. A second fsync will cheerfully return success even though the data
    // is gone forever. So there is no such thing as "retry the fsync": once
    // fsync has reported failure, the only honest thing a durability layer can
    // do is refuse to acknowledge anything further. This is the 2018 PostgreSQL
    // "fsyncgate" bug -- see README references.
    throw std::system_error(errno, std::generic_category(),
                            "fsync " + path_ + " (data is unrecoverable; see fsyncgate)");
  }
  ++fsync_count_;
}

uint64_t FileDevice::Size() const { return size_; }

// ---------------------------------------------------------------------------
// SimDevice
// ---------------------------------------------------------------------------

SimDevice::SimDevice(std::vector<uint8_t> image)
    : volatile_(std::move(image)), stable_(volatile_) {}

void SimDevice::WriteAt(uint64_t off, const void* buf, size_t n) {
  if (n > 0) {
    const uint64_t need = off + n;
    if (volatile_.size() < need) volatile_.resize(need, 0);
    std::memcpy(volatile_.data() + off, buf, n);

    // A write dirties every sector it touches, including bytes in those sectors
    // that were already stable. That is the point of sector granularity: an
    // append that lands in the same 512-byte sector as an older record puts
    // that older record back into the volatile cache. (It can never destroy it
    // -- a lost or torn sector falls back to the stable bytes -- but it does
    // mean the sector is in play again.)
    const uint64_t first = off / kSectorSize;
    const uint64_t last = (off + n - 1) / kSectorSize;
    for (uint64_t s = first; s <= last; ++s) dirty_.insert(s);
  }
  ++write_count_;
  if (post_write_hook_) post_write_hook_(*this);
}

size_t SimDevice::ReadAt(uint64_t off, void* buf, size_t n) const {
  if (off >= volatile_.size()) return 0;
  const size_t avail = std::min<size_t>(n, volatile_.size() - off);
  std::memcpy(buf, volatile_.data() + off, avail);
  return avail;
}

void SimDevice::Sync() {
  for (uint64_t s : dirty_) {
    const uint64_t begin = s * kSectorSize;
    const uint64_t end = std::min<uint64_t>(begin + kSectorSize, volatile_.size());
    if (begin >= end) continue;
    if (stable_.size() < end) stable_.resize(end, 0);
    std::memcpy(stable_.data() + begin, volatile_.data() + begin,
                static_cast<size_t>(end - begin));
  }
  dirty_.clear();
  ++sync_count_;
}

uint64_t SimDevice::Size() const { return volatile_.size(); }

std::vector<uint8_t> SimDevice::PowerCut(std::mt19937_64& rng,
                                         const CutPolicy& policy) const {
  // Everything already synced survives by construction.
  std::vector<uint8_t> out = stable_;
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  auto grow_to = [&out](uint64_t need) {
    if (out.size() < need) out.resize(need, 0);  // holes read back as zeros
  };

  for (uint64_t s : dirty_) {
    const uint64_t begin = s * kSectorSize;
    const uint64_t end = std::min<uint64_t>(begin + kSectorSize, volatile_.size());
    if (begin >= end) continue;
    const uint64_t len = end - begin;

    // Each sector is resolved independently -- no write ordering is honoured.
    const double u = unit(rng);
    if (u < policy.p_persist) {
      grow_to(end);
      std::memcpy(out.data() + begin, volatile_.data() + begin, static_cast<size_t>(len));
    } else if (u < policy.p_persist + policy.p_tear) {
      // Torn: a random non-empty prefix reached the platter, the rest is stale.
      uint64_t k = len;
      if (len > 1) {
        std::uniform_int_distribution<uint64_t> pick(1, len - 1);
        k = pick(rng);
      }
      grow_to(begin + k);
      std::memcpy(out.data() + begin, volatile_.data() + begin, static_cast<size_t>(k));
    }
    // else: lost entirely. out keeps whatever was stable here -- which, if this
    // sector was never synced and a *later* sector did persist, is a hole of
    // zeros in the middle of the log. Recovery has to notice that.
  }
  return out;
}

}  // namespace il
