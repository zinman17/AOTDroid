// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/ext4_reader.cpp
// ============================================================================
#include "ext4_reader.h"
#include <cstring>
#include <algorithm>

namespace aotdroid {

namespace {
constexpr uint16_t kExt4Magic = 0xEF53;
// incompat flags
constexpr uint32_t kIncompatFiletype = 0x0002;
constexpr uint32_t kIncompatExtents = 0x0040;
constexpr uint32_t kIncompat64Bit = 0x0080;
constexpr uint32_t kIncompatFlexBg = 0x0200;
constexpr uint32_t kIncompatMetaCsum = 0x0400;
constexpr uint32_t kIncompatBigalloc = 0x8000;
// ro_compat
constexpr uint32_t kRoSparseSuper = 0x0001;
constexpr uint32_t kRoMetadataCsum = 0x0400;
// inode flags
constexpr uint32_t kFlExtents = 0x00080000;
constexpr uint32_t kFlHugeFile = 0x00040000;
constexpr uint32_t kFlEncrypt = 0x00000800;
constexpr uint32_t kFlInlineData = 0x10000000;
constexpr uint32_t kFlIndexDir = 0x00001000;
constexpr uint16_t kExtentMagic = 0xF30A;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t* p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
}  // namespace

bool Ext4Reader::open(BlockSource* src, std::string* err) {
  src_ = src;
  if (!src || src->size() < 2048) { *err = "ext4: image too small"; return false; }
  uint8_t sb[1024] = {};
  if (src_->pread(1024, 1024, sb) != 1024) { *err = "ext4: superblock read"; return false; }
  if (rd16(sb + 56) != kExt4Magic) { *err = "ext4: bad magic (not an ext4 image)"; return false; }
  uint32_t logBs = rd32(sb + 24);
  if (logBs > 6) { *err = "ext4: bad block size log"; return false; }
  bs_ = 1024u << logBs;
  firstDataBlock_ = rd32(sb + 20);
  blocksPerGroup_ = rd32(sb + 32);
  inodesPerGroup_ = rd32(sb + 40);
  inodeSize_ = rd16(sb + 88);
  uint32_t incompat = rd32(sb + 96);
  uint32_t roCompat = rd32(sb + 100);
  (void)roCompat;   // sparse_super / metadata_csum don't change read paths
  if (inodeSize_ < 128 || inodesPerGroup_ == 0 || blocksPerGroup_ == 0) {
    *err = "ext4: bad geometry"; return false;
  }
  if (incompat & kIncompatBigalloc) { *err = "ext4: bigalloc unsupported"; return false; }
  if (incompat & (1u << 12 /* ENCRYPT */)) { *err = "ext4: encrypted image unsupported"; return false; }
  descSize_ = (incompat & kIncompat64Bit) ? rd16(sb + 254) : 32;
  if (descSize_ < 32) descSize_ = 32;
  uint64_t fsBlocks = rd32(sb + 4) | ((uint64_t)rd32(sb + 336) << 32);
  fsBlocks_ = fsBlocks;
  groupCount_ = (uint32_t)((fsBlocks_ - firstDataBlock_ + blocksPerGroup_ - 1) /
                           blocksPerGroup_);
  if (groupCount_ == 0) { *err = "ext4: no block groups"; return false; }
  (void)kIncompatFiletype; (void)kIncompatFlexBg; (void)kIncompatMetaCsum;
  (void)kRoSparseSuper; (void)kRoMetadataCsum; (void)kFlIndexDir; (void)kFlHugeFile;
  return true;
}

bool Ext4Reader::readBlock(uint64_t phys, uint32_t n, uint8_t* out, std::string* err) {
  if (n == 0 || n > bs_) { *err = "ext4: bad read len"; return false; }
  ssize_t r = src_->pread(phys * bs_, n, out);
  if (r != (ssize_t)n) { *err = "ext4: block read"; return false; }
  return true;
}

bool Ext4Reader::inodeTableForGroup(uint32_t group, uint64_t* tableBlock,
                                    std::string* err) {
  if (inodeTables_.empty()) inodeTables_.resize(groupCount_, UINT64_MAX);
  if (group >= groupCount_) { *err = "ext4: group out of range"; return false; }
  if (inodeTables_[group] != UINT64_MAX) { *tableBlock = inodeTables_[group]; return true; }
  uint64_t gdo = (uint64_t)(firstDataBlock_ + 1) * bs_ + (uint64_t)group * descSize_;
  uint8_t d[64] = {};
  if (descSize_ > sizeof(d)) { *err = "ext4: desc too large"; return false; }
  if (gdo + descSize_ > src_->size()) { *err = "ext4: BGD out of range"; return false; }
  if (src_->pread(gdo, descSize_, d) != (ssize_t)descSize_) {
    *err = "ext4: BGD read"; return false;
  }
  uint64_t t = rd32(d + 8);
  if (descSize_ >= 64) t |= (uint64_t)rd32(d + 40) << 32;
  if (t == 0) { *err = "ext4: null inode table"; return false; }
  inodeTables_[group] = t;
  *tableBlock = t;
  return true;
}

bool Ext4Reader::readInode(uint32_t ino, Inode* out, std::string* err) {
  if (ino == 0 || ino == 0xFFFFFFFFu) { *err = "ext4: bad inode"; return false; }
  uint32_t group = (ino - 1) / inodesPerGroup_;
  uint32_t idx = (ino - 1) % inodesPerGroup_;
  uint64_t table;
  if (!inodeTableForGroup(group, &table, err)) return false;
  uint64_t off = table * bs_ + (uint64_t)idx * inodeSize_;
  uint8_t buf[256] = {};
  if (inodeSize_ > sizeof(buf)) { *err = "ext4: inode too large"; return false; }
  if (src_->pread(off, inodeSize_, buf) != (ssize_t)inodeSize_) {
    *err = "ext4: inode read"; return false;
  }
  out->st.inode = ino;
  out->st.mode = rd16(buf);
  out->st.uid = rd16(buf + 2) | ((uint32_t)rd16(buf + 120) << 16);
  out->st.size = rd32(buf + 4) | ((uint64_t)rd32(buf + 108) << 32);
  out->st.gid = rd16(buf + 24) | ((uint32_t)rd16(buf + 122) << 16);
  out->st.flags = rd32(buf + 32);
  memcpy(out->block, buf + 40, 60);
  if (out->st.flags & kFlEncrypt) { *err = "ext4: encrypted inode"; return false; }
  return true;
}

bool Ext4Reader::statInode(uint32_t ino, Stat* out, std::string* err) {
  Inode in;
  if (!readInode(ino, &in, err)) return false;
  *out = in.st;
  return true;
}

// ---------------------------------------------------------------------------
// extent tree
// ---------------------------------------------------------------------------
bool Ext4Reader::extentNode(const uint8_t* node, uint32_t ino, uint64_t lb,
                            uint64_t* phys, std::string* err) {
  uint16_t entries = rd16(node + 2);
  uint16_t depth = rd16(node + 6);
  if (rd16(node) != kExtentMagic) { *err = "ext4: extent magic"; return false; }
  if (depth == 0) {
    for (uint16_t i = 0; i < entries; ++i) {
      const uint8_t* e = node + 12 + (size_t)i * 12;
      uint32_t eeBlock = rd32(e);
      uint16_t eeLen = rd16(e + 4) & 0x7FFF;
      uint64_t start = rd32(e + 8) | ((uint64_t)rd16(e + 6) << 32);
      if (eeLen == 0) continue;
      if (lb >= eeBlock && lb < (uint64_t)eeBlock + eeLen) {
        cacheIno_ = ino; cacheEeBlock_ = eeBlock; cacheEeStart_ = start;
        cacheEeLen_ = eeLen;
        *phys = start + (lb - eeBlock);
        return true;
      }
    }
    return false;   // hole
  }
  // interior: find the index covering lb, recurse into the leaf block
  uint32_t bestBlock = 0; uint64_t bestLeaf = 0; bool have = false;
  for (uint16_t i = 0; i < entries; ++i) {
    const uint8_t* ix = node + 12 + (size_t)i * 12;
    uint32_t eiBlock = rd32(ix);
    uint64_t leaf = rd32(ix + 4) | ((uint64_t)rd16(ix + 8) << 32);
    if (lb >= eiBlock && (!have || eiBlock >= bestBlock)) {
      bestBlock = eiBlock; bestLeaf = leaf; have = true;
    }
  }
  if (!have) return false;
  std::vector<uint8_t> child(bs_);
  if (!readBlock(bestLeaf, bs_, child.data(), err)) return false;
  return extentNode(child.data(), ino, lb, phys, err);
}

bool Ext4Reader::extentLookup(const Inode& in, uint64_t lb, uint64_t* phys,
                              std::string* err) {
  if (cacheIno_ == in.st.inode && lb >= cacheEeBlock_ &&
      lb < cacheEeBlock_ + cacheEeLen_) {
    *phys = cacheEeStart_ + (lb - cacheEeBlock_);
    return true;
  }
  return extentNode(in.block, in.st.inode, lb, phys, err);
}

// ---------------------------------------------------------------------------
// indirect-block fallback (pre-extents images)
// ---------------------------------------------------------------------------
bool Ext4Reader::physBlockIndirect(const Inode& in, uint64_t lb, int level,
                                   uint64_t ptrBlock, uint64_t* phys,
                                   std::string* err) {
  uint32_t perBlock = bs_ / 4;
  if (level == 0) {
    if (ptrBlock == 0) { *phys = 0; return true; }   // hole
    *phys = ptrBlock;
    return true;
  }
  if (ptrBlock == 0) { *phys = 0; return true; }
  std::vector<uint8_t> b(bs_);
  if (!readBlock(ptrBlock, bs_, b.data(), err)) return false;
  uint64_t div = 1;
  for (int i = 1; i < level; ++i) div *= perBlock;
  uint64_t idx = (lb / div) % perBlock;
  uint64_t next = rd32(b.data() + (size_t)idx * 4);
  return physBlockIndirect(in, lb, level - 1, next, phys, err);
}

bool Ext4Reader::physBlockFor(const Inode& in, uint64_t lb, uint64_t* phys,
                              std::string* err) {
  if (in.st.flags & kFlExtents) return extentLookup(in, lb, phys, err);
  uint32_t perBlock = bs_ / 4;
  if (lb < 12) {
    uint64_t p = rd32(in.block + (size_t)lb * 4);
    *phys = p;
    return true;
  }
  uint64_t b = lb - 12;
  if (b < perBlock)
    return physBlockIndirect(in, b, 1, rd32(in.block + 48), phys, err);
  b -= perBlock;
  if (b < (uint64_t)perBlock * perBlock)
    return physBlockIndirect(in, b, 2, rd32(in.block + 52), phys, err);
  b -= (uint64_t)perBlock * perBlock;
  return physBlockIndirect(in, b, 3, rd32(in.block + 56), phys, err);
}

// ---------------------------------------------------------------------------
// data reads
// ---------------------------------------------------------------------------
ssize_t Ext4Reader::readRangeInode(uint32_t ino, uint64_t off, size_t n,
                                   uint8_t* out, std::string* err) {
  Inode in;
  if (!readInode(ino, &in, err)) return -1;
  if ((in.st.flags & kFlInlineData) && in.st.size <= 60) {
    if (off >= in.st.size) return 0;
    if (off + n > in.st.size) n = (size_t)(in.st.size - off);
    memcpy(out, in.block + off, n);
    return (ssize_t)n;
  }
  if (off >= in.st.size) return 0;
  if (off + n > in.st.size) n = (size_t)(in.st.size - off);
  size_t done = 0;
  uint64_t lb = off / bs_;
  uint32_t inBlockOff = (uint32_t)(off % bs_);
  std::vector<uint8_t> b(bs_);
  while (done < n) {
    uint64_t phys;
    if (!physBlockFor(in, lb, &phys, err)) return done ? (ssize_t)done : -1;
    size_t take = std::min<size_t>(bs_ - inBlockOff, n - done);
    if (phys == 0) {
      memset(out + done, 0, take);              // hole
    } else {
      if (inBlockOff == 0 && take == bs_) {
        if (!readBlock(phys, bs_, out + done, err)) return done ? (ssize_t)done : -1;
      } else {
        if (!readBlock(phys, bs_, b.data(), err)) return done ? (ssize_t)done : -1;
        memcpy(out + done, b.data() + inBlockOff, take);
      }
    }
    done += take;
    inBlockOff = 0;
    ++lb;
  }
  return (ssize_t)done;
}

bool Ext4Reader::readFileInode(uint32_t ino, const Stat& st,
                               const std::function<bool(const uint8_t*, size_t)>& sink,
                               std::string* err) {
  (void)st;
  std::vector<uint8_t> buf(512 * 1024);
  uint64_t off = 0;
  Inode in;
  if (!readInode(ino, &in, err)) return false;
  while (off < in.st.size) {
    size_t want = (size_t)std::min<uint64_t>(in.st.size - off, buf.size());
    ssize_t r = readRangeInode(ino, off, want, buf.data(), err);
    if (r <= 0) { if (!err->empty()) return false; break; }
    if (!sink(buf.data(), (size_t)r)) return false;
    off += (uint64_t)r;
  }
  return true;
}

std::string Ext4Reader::readSymlinkInode(uint32_t ino, const Stat& st,
                                         std::string* err) {
  Inode in;
  if (!readInode(ino, &in, err)) return {};
  // fast symlink: target inline in i_block
  if (!(in.st.flags & kFlExtents) && in.st.size <= 60) {
    return std::string((const char*)in.block, (size_t)in.st.size);
  }
  std::string out;
  out.resize((size_t)in.st.size);
  ssize_t r = readRangeInode(ino, 0, out.size(), (uint8_t*)out.data(), err);
  if (r != (ssize_t)out.size()) { *err = "ext4: symlink read"; return {}; }
  return out;
}

bool Ext4Reader::listDirInode(uint32_t ino, std::vector<DirEntry>* out,
                              std::string* err) {
  Inode in;
  if (!readInode(ino, &in, err)) return false;
  if (!in.st.isDir()) { *err = "ext4: not a directory"; return false; }
  // directory data stream: read block by block (dirents never span blocks)
  uint64_t nBlocks = (in.st.size + bs_ - 1) / bs_;
  if (in.st.flags & kFlInlineData) { *err = "ext4: inline dir unsupported"; return false; }
  std::vector<uint8_t> b(bs_);
  for (uint64_t lb = 0; lb < nBlocks; ++lb) {
    uint64_t phys;
    if (!physBlockFor(in, lb, &phys, err)) return false;
    if (phys == 0) continue;
    if (!readBlock(phys, bs_, b.data(), err)) return false;
    size_t off = 0;
    while (off + 8 <= bs_) {
      uint32_t eIno = rd32(b.data() + off);
      uint16_t recLen = rd16(b.data() + off + 4);
      uint8_t nameLen = b[off + 6];
      uint8_t ftype = b[off + 7];
      if (recLen < 8 || off + recLen > bs_) break;   // corrupt / csum tail
      if (eIno != 0 && nameLen > 0 && nameLen <= recLen - 8) {
        const char* nm = (const char*)b.data() + off + 8;
        if (memchr(nm, 0, nameLen) == nullptr &&
            !(nameLen == 1 && nm[0] == '.') &&
            !(nameLen == 2 && nm[0] == '.' && nm[1] == '.')) {
          DirEntry de;
          de.name.assign(nm, nameLen);
          de.inode = eIno;
          de.type = ftype;
          out->push_back(std::move(de));
        }
      }
      off += recLen;
    }
  }
  return true;
}

bool Ext4Reader::lookupPath(const std::string& path, Stat* out, std::string* err) {
  std::string p = path;
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  while (!p.empty() && p.back() == '/') p.pop_back();
  Stat cur;
  if (!statInode(2, &cur, err)) return false;
  size_t start = 0;
  while (start < p.size()) {
    size_t slash = p.find('/', start);
    std::string comp = p.substr(start, slash == std::string::npos
                                          ? std::string::npos : slash - start);
    size_t compStart = start;
    start = slash == std::string::npos ? p.size() : slash + 1;
    if (comp.empty() || comp == "." || comp == "..") continue;
    if (!cur.isDir()) { *err = "ext4: not a dir in path " + path; return false; }
    // resolve symlink components
    std::vector<DirEntry> des;
    if (!listDirInode(cur.inode, &des, err)) return false;
    const DirEntry* hit = nullptr;
    for (const auto& de : des) if (de.name == comp) { hit = &de; break; }
    if (!hit) return false;
    Stat next;
    if (!statInode(hit->inode, &next, err)) return false;
    if (next.isSymlink()) {
      std::string target = readSymlinkInode(next.inode, next, err);
      if (target.empty()) return false;
      std::string base = compStart == 0 ? "" : p.substr(0, compStart - 1);
      std::string rest = p.substr(start);
      std::string merged = target.front() == '/'
                               ? target + (rest.empty() ? "" : "/" + rest)
                               : base + (base.empty() ? "" : "/") + target +
                                     (rest.empty() ? "" : "/" + rest);
      return lookupPath(merged, out, err);
    }
    cur = next;
  }
  *out = cur;
  return true;
}

}  // namespace aotdroid
