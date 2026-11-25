// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/block_source.h
//  Abstract random-access byte source. Lets the ext4 reader run on top of a
//  plain file, a STORED zip entry (GSI .zip without unpacking) or an Android
//  sparse image without caring which.
// ============================================================================
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace aotdroid {

class BlockSource {
 public:
  virtual ~BlockSource() = default;
  // Returns bytes read (0 at EOF) or -1 on error.
  virtual ssize_t pread(uint64_t offset, size_t n, uint8_t* out) = 0;
  virtual uint64_t size() const = 0;
};

class FileBlockSource final : public BlockSource {
 public:
  bool open(const std::string& path, std::string* err) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) { *err = "open " + path; return false; }
    struct stat st {};
    if (::fstat(fd_, &st) != 0) { *err = "fstat " + path; return false; }
    size_ = (uint64_t)st.st_size;
    return true;
  }
  ~FileBlockSource() override { if (fd_ >= 0) ::close(fd_); }
  ssize_t pread(uint64_t offset, size_t n, uint8_t* out) override {
    if (offset >= size_) return 0;
    if (offset + n > size_) n = (size_t)(size_ - offset);
    return ::pread(fd_, out, n, (off_t)offset);
  }
  uint64_t size() const override { return size_; }
 private:
  int fd_ = -1;
  uint64_t size_ = 0;
};

}  // namespace aotdroid
