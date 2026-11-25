// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/zip_reader.cpp
// ============================================================================
#include "zip_reader.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <zlib.h>

namespace aotdroid {

namespace {
constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kEocd64Sig = 0x06064b50;
constexpr uint32_t kEocd64LocatorSig = 0x07064b50;
constexpr uint32_t kCdeSig = 0x02014b50;   // central directory entry
constexpr uint32_t kLfhSig = 0x04034b50;   // local file header
constexpr uint32_t kZip64ExtraId = 0x0001;
constexpr size_t kEocdMin = 22;
constexpr size_t kMaxComment = (1u << 16) + kEocdMin;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t* p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
}  // namespace

bool zipLooksLikeZip(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  uint8_t m[4] = {};
  ssize_t n = ::read(fd, m, 4);
  ::close(fd);
  if (n != 4) return false;
  return m[0] == 'P' && m[1] == 'K' && (m[2] == 3 || m[2] == 5 || m[2] == 7);
}

bool ZipReader::open(const std::string& path, std::string* err) {
  close();
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ < 0) { *err = "open zip: " + std::string(strerror(errno)); return false; }
  struct stat st {};
  if (::fstat(fd_, &st) != 0 || st.st_size < (off_t)kEocdMin) {
    *err = "zip too small"; return false;
  }
  fileSize_ = (uint64_t)st.st_size;
  if (!parseCentralDirectory(err)) return false;
  dataOff_.assign(entries_.size(), UINT64_MAX);
  return true;
}

void ZipReader::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  entries_.clear();
  dataOff_.clear();
  fileSize_ = 0;
}

bool ZipReader::parseCentralDirectory(std::string* err) {
  // 1. scan backwards for EOCD
  size_t win = (size_t)std::min<uint64_t>(fileSize_, kMaxComment);
  std::vector<uint8_t> buf(win);
  off_t base = (off_t)(fileSize_ - win);
  if (::pread(fd_, buf.data(), win, base) != (ssize_t)win) { *err = "zip read tail"; return false; }
  ssize_t eocdAt = -1;
  for (ssize_t i = (ssize_t)win - (ssize_t)kEocdMin; i >= 0; --i) {
    if (rd32(&buf[(size_t)i]) == kEocdSig) { eocdAt = i; break; }
  }
  if (eocdAt < 0) { *err = "zip: EOCD not found"; return false; }
  uint64_t cdSize = rd32(&buf[(size_t)eocdAt + 12]);
  uint64_t cdOff = rd32(&buf[(size_t)eocdAt + 16]);
  uint64_t totalEntries = rd16(&buf[(size_t)eocdAt + 10]);

  // 2. ZIP64? (locator sits immediately before the EOCD)
  if (eocdAt >= 20 && rd32(&buf[(size_t)eocdAt - 20]) == kEocd64LocatorSig) {
    uint64_t eocd64Off = rd64(&buf[(size_t)eocdAt - 20 + 8]);
    uint8_t e[56] = {};
    if (eocd64Off + 56 > fileSize_ ||
        ::pread(fd_, e, 56, (off_t)eocd64Off) != 56 || rd32(e) != kEocd64Sig) {
      *err = "zip: bad ZIP64 EOCD"; return false;
    }
    totalEntries = rd64(e + 32);
    cdSize = rd64(e + 40);
    cdOff = rd64(e + 48);
  }
  if (cdOff + cdSize > fileSize_) { *err = "zip: central directory out of range"; return false; }

  // 3. parse central directory entries
  std::vector<uint8_t> cd((size_t)cdSize);
  if (::pread(fd_, cd.data(), (size_t)cdSize, (off_t)cdOff) != (ssize_t)cdSize) {
    *err = "zip: central directory read"; return false;
  }
  size_t off = 0;
  entries_.reserve((size_t)totalEntries);
  while (off + 46 <= cd.size() && rd32(&cd[off]) == kCdeSig) {
    Entry e;
    e.method = rd16(&cd[off + 10]);
    e.crc32 = rd32(&cd[off + 16]);
    e.compressedSize = rd32(&cd[off + 20]);
    e.uncompressedSize = rd32(&cd[off + 24]);
    uint16_t nameLen = rd16(&cd[off + 28]);
    uint16_t extraLen = rd16(&cd[off + 30]);
    uint16_t commentLen = rd16(&cd[off + 32]);
    uint32_t extAttr = rd32(&cd[off + 38]);
    e.localHeaderOffset = rd32(&cd[off + 42]);
    if (off + 46 + nameLen > cd.size()) { *err = "zip: truncated CDE name"; return false; }
    e.name.assign((const char*)&cd[off + 46], nameLen);

    // ZIP64 extra (fields present only where the base field is 0xFFFFFFFF)
    size_t xo = off + 46 + nameLen, xe = xo + extraLen;
    while (xo + 4 <= xe) {
      uint16_t id = rd16(&cd[xo]), sz = rd16(&cd[xo + 2]);
      if (id == kZip64ExtraId) {
        size_t fo = xo + 4;
        auto take64 = [&](uint64_t* dst) {
          if (fo + 8 <= xo + 4 + sz) { *dst = rd64(&cd[fo]); fo += 8; }
        };
        if (e.uncompressedSize == 0xFFFFFFFFu) take64(&e.uncompressedSize);
        if (e.compressedSize == 0xFFFFFFFFu) take64(&e.compressedSize);
        if (e.localHeaderOffset == 0xFFFFFFFFu) take64(&e.localHeaderOffset);
      }
      xo += 4 + sz;
    }

    e.unixMode = extAttr >> 16;
    e.isDir = (!e.name.empty() && e.name.back() == '/') ||
              ((e.unixMode & 0170000) == 0040000);
    e.isSymlink = (e.unixMode & 0170000) == 0120000;
    if (e.method != 0 && e.method != 8) {
      *err = "zip: unsupported method " + std::to_string(e.method) + " for " + e.name;
      return false;
    }
    entries_.push_back(std::move(e));
    off += 46 + (size_t)nameLen + extraLen + commentLen;
  }
  return true;
}

const ZipReader::Entry* ZipReader::find(const std::string& name) const {
  for (const auto& e : entries_) if (e.name == name) return &e;
  return nullptr;
}

uint64_t ZipReader::dataOffsetOf(const Entry& e, std::string* err) const {
  size_t idx = (size_t)(&e - entries_.data());
  if (dataOff_[idx] != UINT64_MAX) return dataOff_[idx];
  uint8_t lfh[30] = {};
  if (e.localHeaderOffset + 30 > fileSize_ ||
      ::pread(fd_, lfh, 30, (off_t)e.localHeaderOffset) != 30 ||
      rd32(lfh) != kLfhSig) {
    *err = "zip: bad local header for " + e.name;
    return UINT64_MAX;
  }
  uint64_t d = e.localHeaderOffset + 30 + rd16(lfh + 26) + rd16(lfh + 28);
  if (d + e.compressedSize > fileSize_) { *err = "zip: data out of range"; return UINT64_MAX; }
  dataOff_[idx] = d;
  return d;
}

ssize_t ZipReader::storedRead(const Entry& e, uint64_t offset, size_t n,
                              uint8_t* out) const {
  if (!e.stored()) { errno = EINVAL; return -1; }
  std::string err;
  uint64_t d = dataOffsetOf(e, &err);
  if (d == UINT64_MAX) { errno = EIO; return -1; }
  if (offset >= e.uncompressedSize) return 0;
  if (offset + n > e.uncompressedSize) n = (size_t)(e.uncompressedSize - offset);
  ssize_t r = ::pread(fd_, out, n, (off_t)(d + offset));
  return r;
}

bool ZipReader::extractDeflate(const Entry& e, uint64_t dataOff, const Sink& sink,
                               std::string* err) const {
  std::vector<uint8_t> in(256 * 1024), out(256 * 1024);
  z_stream zs{};
  if (inflateInit2(&zs, -15) != Z_OK) { *err = "inflateInit"; return false; }
  uint64_t remaining = e.compressedSize;
  uint64_t off = dataOff;
  bool ok = true;
  int ret = Z_OK;
  while (ret != Z_STREAM_END && ok) {
    size_t want = (size_t)std::min<uint64_t>(remaining, in.size());
    if (want == 0) { ok = false; *err = "deflate: truncated stream"; break; }
    if (::pread(fd_, in.data(), want, (off_t)off) != (ssize_t)want) {
      ok = false; *err = "deflate: read"; break;
    }
    off += want; remaining -= want;
    zs.next_in = in.data(); zs.avail_in = (uInt)want;
    do {
      zs.next_out = out.data(); zs.avail_out = (uInt)out.size();
      ret = inflate(&zs, Z_NO_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
        ok = false; *err = std::string("inflate: ") + zs.msg; break;
      }
      size_t got = out.size() - zs.avail_out;
      if (got && !sink(out.data(), got)) { ok = false; break; }
    } while (zs.avail_out == 0 && ret != Z_STREAM_END);
  }
  if (ok && ret != Z_STREAM_END) { *err = "deflate: stream not finished"; ok = false; }
  if (ok && zs.total_out != e.uncompressedSize) {
    *err = "deflate: size mismatch"; ok = false;
  }
  inflateEnd(&zs);
  return ok;
}

bool ZipReader::extract(const Entry& e, const Sink& sink, std::string* err) const {
  if (e.isDir) return true;
  std::string lerr;
  uint64_t d = dataOffsetOf(e, &lerr);
  if (d == UINT64_MAX) { *err = lerr; return false; }
  uint32_t crc = 0;
  bool ok;
  if (e.stored()) {
    ok = true;
    std::vector<uint8_t> buf(256 * 1024);
    uint64_t off = 0;
    while (off < e.uncompressedSize && ok) {
      size_t want = (size_t)std::min<uint64_t>(e.uncompressedSize - off, buf.size());
      ssize_t r = ::pread(fd_, buf.data(), want, (off_t)(d + off));
      if (r != (ssize_t)want) { ok = false; *err = "stored: read"; break; }
      crc = (uint32_t)crc32(crc, buf.data(), (uInt)want);
      if (!sink(buf.data(), want)) { ok = false; break; }
      off += want;
    }
  } else {
    ok = extractDeflate(e, d,
        [&](const uint8_t* p, size_t n) {
          crc = (uint32_t)crc32(crc, p, (uInt)n);
          return sink(p, n);
        }, err);
  }
  if (ok && crc != e.crc32) { *err = "crc mismatch for " + e.name; ok = false; }
  return ok;
}

bool ZipReader::extractToFile(const Entry& e, const std::string& outPath,
                              std::string* err) const {
  int out = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) { *err = "create " + outPath; return false; }
  bool ok = extract(e, [&](const uint8_t* p, size_t n) {
    size_t done = 0;
    while (done < n) {
      ssize_t w = ::write(out, p + done, n - done);
      if (w <= 0) return false;
      done += (size_t)w;
    }
    return true;
  }, err);
  ::close(out);
  return ok;
}

}  // namespace aotdroid
