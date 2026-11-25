// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/sparse_image.cpp
// ============================================================================
#include "sparse_image.h"
#include <algorithm>
#include <cstring>

namespace aotdroid {

namespace {
constexpr uint32_t kSparseMagic = 0xED26FF3A;   // little-endian on disk
// chunk types (full 16-bit on-disk values)
constexpr uint16_t kChunkRaw = 0xCAC1;
constexpr uint16_t kChunkFill = 0xCAC2;
constexpr uint16_t kChunkDontCare = 0xCAC3;
constexpr uint16_t kChunkCrc32 = 0xCAC4;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t* p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
}  // namespace

bool sparseProbe(BlockSource* src) {
  uint8_t m[4] = {};
  if (!src || src->size() < 32) return false;
  if (src->pread(0, 4, m) != 4) return false;
  return rd32(m) == kSparseMagic;
}

bool SparseReader::open(BlockSource* src, std::string* err) {
  src_ = src;
  if (!src) { *err = "sparse: null source"; return false; }
  if (src->size() < 4) { *err = "sparse: image too small"; return false; }
  uint8_t hdr[28] = {};
  if (src->pread(0, 28, hdr) != 28) { *err = "sparse: header read"; return false; }
  if (rd32(hdr) != kSparseMagic) {
    // plain raw image: 1:1 passthrough
    sparse_ = false;
    logicalSize_ = src->size();
    return true;
  }
  uint16_t fileHdrSz = rd16(hdr + 8);     // sparse_header: magic(4) major(2) minor(2) file_hdr_sz(2)
  if (rd16(hdr + 4) != 1 || fileHdrSz < 28) { *err = "sparse: bad header"; return false; }
  blockSize_ = rd32(hdr + 12);
  uint64_t totalBlks = rd32(hdr + 16);
  uint64_t totalChunks = rd32(hdr + 20);
  uint16_t chunkHdrSz = rd16(hdr + 10);
  if (blockSize_ == 0 || blockSize_ % 512 != 0 || chunkHdrSz < 8) {
    *err = "sparse: bad geometry"; return false;
  }
  logicalSize_ = totalBlks * blockSize_;
  chunks_.reserve((size_t)totalChunks);

  uint64_t fileOff = fileHdrSz;
  uint64_t logical = 0;
  for (uint64_t c = 0; c < totalChunks; ++c) {
    uint8_t ch[12] = {};
    size_t chRead = std::min<size_t>(chunkHdrSz, sizeof(ch));
    if (src->pread(fileOff, chRead, ch) != (ssize_t)chRead) {
      *err = "sparse: chunk header read"; return false;
    }
    uint16_t type = rd16(ch);
    uint64_t nrBlks = rd32(ch + 4);
    uint64_t dataLen = nrBlks * blockSize_;
    if (type != kChunkRaw && type != kChunkFill && type != kChunkDontCare &&
        type != kChunkCrc32) {
      *err = "sparse: unknown chunk type"; return false;
    }
    Chunk ck{};
    ck.type = type;
    ck.logicalStart = logical;
    ck.len = dataLen;
    ck.fileOff = fileOff + chunkHdrSz;
    uint64_t advance = chunkHdrSz;
    if (type == kChunkRaw) advance += dataLen;
    else if (type == kChunkFill) advance += 4;
    else if (type == kChunkCrc32) advance += 4;
    // dont-care: no payload
    fileOff += advance;
    logical += dataLen;
    chunks_.push_back(ck);
  }
  sparse_ = true;
  return true;
}

ssize_t SparseReader::preadLogical(uint64_t off, size_t n, uint8_t* out) const {
  if (!sparse_) return src_->pread(off, n, out);
  if (off >= logicalSize_) return 0;
  if (off + n > logicalSize_) n = (size_t)(logicalSize_ - off);
  size_t done = 0;
  while (done < n) {
    uint64_t cur = off + done;
    // binary search: last chunk with logicalStart <= cur
    size_t lo = 0, hi = chunks_.size();
    while (lo + 1 < hi) {
      size_t mid = (lo + hi) / 2;
      if (chunks_[mid].logicalStart <= cur) lo = mid; else hi = mid;
    }
    const Chunk& c = chunks_[lo];
    if (cur >= c.logicalStart + c.len) { return (ssize_t)done > 0 ? (ssize_t)done : -1; }
    uint64_t inChunk = cur - c.logicalStart;
    size_t take = (size_t)std::min<uint64_t>(c.len - inChunk, n - done);
    switch (c.type) {
      case kChunkRaw: {
        ssize_t r = src_->pread(c.fileOff + inChunk, take, out + done);
        if (r < 0) return (ssize_t)done > 0 ? (ssize_t)done : -1;
        take = (size_t)r;
        break;
      }
      case kChunkFill: {
        uint8_t f[4];
        if (src_->pread(c.fileOff, 4, f) != 4) return (ssize_t)done > 0 ? (ssize_t)done : -1;
        for (size_t i = 0; i < take; ++i) out[done + i] = f[i & 3];
        break;
      }
      case kChunkDontCare:
        memset(out + done, 0, take);
        break;
      case kChunkCrc32:
        // crc chunk carries no data; treat as dont-care for read purposes
        memset(out + done, 0, take);
        break;
      default:
        return (ssize_t)done > 0 ? (ssize_t)done : -1;
    }
    done += take;
    if (take == 0) break;
  }
  return (ssize_t)done;
}

}  // namespace aotdroid
