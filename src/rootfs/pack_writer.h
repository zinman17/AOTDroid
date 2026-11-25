// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/pack_writer.h
//  Streaming .aotpack writer (the format src/runtime/vfs.h PackVFS reads).
//  Entries are staged in RAM (paths + metadata only); blob contents are
//  streamed lazily at finish() time so multi-GB GSIs never sit in memory.
//  Executable blobs are 16 KiB aligned (match the rewriter's PT_AOTPATCH
//  congruence rule); entries are written sorted by name so PackVFS can
//  binary-search.
// ============================================================================
#pragma once
#include "../runtime/vfs.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aotdroid {

class PackWriter {
 public:
  // Content provider: stream [off, off+n) of the logical file to `sink`.
  using ContentFn = std::function<bool(uint64_t off, size_t n,
                                       const std::function<bool(const uint8_t*, size_t)>& sink)>;

  bool begin(const std::string& outPath, std::string* err);
  bool addDir(const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid);
  bool addFile(const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid,
               uint64_t size, bool executable, const ContentFn& content);
  bool addSymlink(const std::string& name, uint32_t uid, uint32_t gid,
                  const std::string& target);
  // finish: writes header + strings + entries + blob area, fsync, close.
  // progress(bytesDone, bytesTotal) is invoked periodically.
  bool finish(const std::function<void(uint64_t, uint64_t)>& progress,
              std::string* err);
  void abort();

  uint32_t entryCount() const { return (uint32_t)items_.size(); }
  uint64_t totalBytes() const { return totalBytes_; }

 private:
  struct Item {
    std::string name;
    uint32_t mode = 0, uid = 0, gid = 0;
    uint64_t size = 0;
    uint32_t flags = 0;         // kPackFlagExec / kPackFlagDir / kPackFlagSymlink
    ContentFn content;          // null for dirs
    std::string inlineData;     // symlink target
    uint64_t blobOff = 0;
    uint32_t nameOff = 0;
  };
  int fd_ = -1;
  std::string outPath_;
  std::vector<Item> items_;
  uint64_t totalBytes_ = 0;
  std::string tmpPath_;
};

}  // namespace aotdroid
