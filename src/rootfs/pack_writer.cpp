// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/pack_writer.cpp
// ============================================================================
#include "pack_writer.h"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace aotdroid {

namespace {
// PackEntry / PackHeader are serialized with their NATIVE struct layout
// (incl. tail padding) because PackVFS casts the mapped bytes directly.
// Both writer and reader targets are LP64 little-endian (arm64 iOS,
// x86_64/arm64 hosts); a static_assert guards the assumption.
static_assert(sizeof(PackEntry) == 40, "PackEntry ABI drifted");
static_assert(sizeof(PackHeader) == 40, "PackHeader ABI drifted");

bool writeAll(int fd, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(fd, b + done, n - done);
    if (w <= 0) return false;
    done += (size_t)w;
  }
  return true;
}
}  // namespace

bool PackWriter::begin(const std::string& outPath, std::string* err) {
  outPath_ = outPath;
  tmpPath_ = outPath + ".tmp";
  fd_ = ::open(tmpPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ < 0) {
    *err = "pack: create " + tmpPath_ + ": " + strerror(errno);
    return false;
  }
  return true;
}

bool PackWriter::addDir(const std::string& name, uint32_t mode, uint32_t uid,
                        uint32_t gid) {
  Item it;
  it.name = name; it.mode = mode | 0040755; it.uid = uid; it.gid = gid;
  it.flags = kPackFlagDir;
  items_.push_back(std::move(it));
  return true;
}

bool PackWriter::addFile(const std::string& name, uint32_t mode, uint32_t uid,
                         uint32_t gid, uint64_t size, bool executable,
                         const ContentFn& content) {
  Item it;
  it.name = name; it.mode = mode & 07777777; it.uid = uid; it.gid = gid;
  it.size = size;
  it.flags = executable ? kPackFlagExec : 0;
  it.content = content;
  items_.push_back(std::move(it));
  totalBytes_ += size;
  return true;
}

bool PackWriter::addSymlink(const std::string& name, uint32_t uid, uint32_t gid,
                            const std::string& target) {
  Item it;
  it.name = name; it.mode = 0120777; it.uid = uid; it.gid = gid;
  it.size = target.size();
  it.flags = kPackFlagSymlink;
  it.inlineData = target;
  items_.push_back(std::move(it));
  return true;
}

void PackWriter::abort() {
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  if (!tmpPath_.empty()) ::unlink(tmpPath_.c_str());
}

bool PackWriter::finish(const std::function<void(uint64_t, uint64_t)>& progress,
                        std::string* err) {
  if (fd_ < 0) { *err = "pack: not begun"; return false; }
  auto fail = [&](const std::string& m) {
    *err = m; abort(); return false;
  };

  // 1. sort by name (PackVFS binary-searches the entries)
  std::sort(items_.begin(), items_.end(),
            [](const Item& a, const Item& b) { return a.name < b.name; });

  // 2. layout: [header 40][strings][entries (40 each)][pad][blobs]
  const uint64_t strOff = sizeof(PackHeader);
  uint64_t cur = strOff;
  for (auto& it : items_) {
    it.nameOff = (uint32_t)(cur - strOff);
    cur += it.name.size() + 1;
  }
  const uint64_t entryOff = cur;
  cur += (uint64_t)items_.size() * sizeof(PackEntry);
  for (auto& it : items_) {
    if (it.flags & kPackFlagExec) cur = (cur + 16383) & ~(uint64_t)16383;
    it.blobOff = cur;
    cur += it.size;
  }
  const uint64_t blobOff0 = cur;

  // 3. header + strings + entries
  PackHeader hdr{};
  memcpy(hdr.magic, "AOTPACK1", 8);
  hdr.version = 1;
  hdr.entryCount = (uint32_t)items_.size();
  hdr.strOff = strOff;
  hdr.entryOff = entryOff;
  hdr.blobOff = blobOff0;
  if (!writeAll(fd_, &hdr, sizeof(hdr))) return fail("pack: header write");
  for (auto& it : items_)
    if (!writeAll(fd_, it.name.data(), it.name.size() + 1))
      return fail("pack: strings write");
  for (auto& it : items_) {
    PackEntry e{};
    e.nameOff = it.nameOff; e.mode = it.mode; e.uid = it.uid; e.gid = it.gid;
    e.blobOff = it.blobOff; e.size = it.size; e.flags = it.flags;
    if (!writeAll(fd_, &e, sizeof(e))) return fail("pack: entry write");
  }

  // 4. blob area (streaming; exec entries zero-padded to 16 KiB alignment)
  uint64_t pos = entryOff + (uint64_t)items_.size() * sizeof(PackEntry);
  uint64_t done = 0;
  for (auto& it : items_) {
    if (pos < it.blobOff) {
      size_t pad = (size_t)(it.blobOff - pos);
      std::vector<uint8_t> z(pad, 0);
      if (!writeAll(fd_, z.data(), pad)) return fail("pack: align pad");
      pos = it.blobOff;
    }
    if (it.flags & kPackFlagDir) continue;
    if (it.flags & kPackFlagSymlink) {
      if (!writeAll(fd_, it.inlineData.data(), it.inlineData.size()))
        return fail("pack: symlink write");
      pos += it.size;
      continue;
    }
    if (!it.content) return fail("pack: missing content for " + it.name);
    if (!it.content(0, (size_t)it.size,
                    [&](const uint8_t* p, size_t n) { return writeAll(fd_, p, n); }))
      return fail("pack: blob stream for " + it.name);
    pos += it.size;
    done += it.size;
    if (progress) progress(done, totalBytes_);
  }
  if (fd_ >= 0) {
    (void)::fsync(fd_);
    ::close(fd_);
    fd_ = -1;
  }
  if (::rename(tmpPath_.c_str(), outPath_.c_str()) != 0) {
    *err = std::string("pack: rename: ") + strerror(errno);
    ::unlink(tmpPath_.c_str());
    return false;
  }
  return true;
}

}  // namespace aotdroid
