// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/ext4_reader.h
//  Read-only ext4 walker over a BlockSource. Covers what real GSI system
//  images use: extents (incl. depth>0 trees), indirect-block fallback,
//  64-bit feature set (s_desc_size=64), inline + fast symlinks, htree dirs
//  (walked linearly -- the htree is only an index over the dirent stream)
//  and metadata_csum images (checksum tails are skipped as dirents).
// ============================================================================
#pragma once
#include "block_source.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aotdroid {

class Ext4Reader {
 public:
  struct Stat {
    uint32_t inode = 0;
    uint32_t mode = 0;         // raw unix mode (incl. S_IFMT)
    uint32_t uid = 0, gid = 0;
    uint64_t size = 0;
    uint32_t flags = 0;        // ext4 inode flags
    bool isDir() const { return (mode & 0170000) == 0040000; }
    bool isReg() const { return (mode & 0170000) == 0100000; }
    bool isSymlink() const { return (mode & 0170000) == 0120000; }
  };
  struct DirEntry {
    std::string name;
    uint32_t inode = 0;
    uint8_t type = 0;          // ext4 dirent file_type
  };

  bool open(BlockSource* src, std::string* err);

  // inode 2 == root
  bool statInode(uint32_t ino, Stat* out, std::string* err);
  bool listDirInode(uint32_t ino, std::vector<DirEntry>* out, std::string* err);
  // Read [off, off+n) of a regular file / slow symlink. Returns bytes read.
  ssize_t readRangeInode(uint32_t ino, uint64_t off, size_t n, uint8_t* out,
                         std::string* err);
  // Streaming whole-file read (chunked; sink returning false aborts).
  bool readFileInode(uint32_t ino, const Stat& st,
                     const std::function<bool(const uint8_t*, size_t)>& sink,
                     std::string* err);
  // Symlink target (fast inline or slow block-backed).
  std::string readSymlinkInode(uint32_t ino, const Stat& st, std::string* err);
  // Path lookup from root ('/'-separated, no leading slash needed).
  bool lookupPath(const std::string& path, Stat* out, std::string* err);

  uint32_t blockSize() const { return bs_; }
  uint64_t fsBlocks() const { return fsBlocks_; }

 private:
  struct Inode {
    Stat st;
    uint8_t block[60];
  };
  bool readInode(uint32_t ino, Inode* out, std::string* err);
  bool inodeTableForGroup(uint32_t group, uint64_t* tableBlock, std::string* err);
  // Physical block for logical block lb of inode (via extents or indirects).
  bool physBlockFor(const Inode& ino, uint64_t lb, uint64_t* phys, std::string* err);
  bool physBlockIndirect(const Inode& in, uint64_t lb, int level, uint64_t ptrBlock,
                         uint64_t* phys, std::string* err);
  bool readBlock(uint64_t phys, uint32_t n, uint8_t* out, std::string* err);
  // extent walk helper: fills phys for lb; caches last extent (most reads are
  // sequential, so a 1-entry cache removes ~all tree walks)
  bool extentLookup(const Inode& in, uint64_t lb, uint64_t* phys, std::string* err);
  bool extentNode(const uint8_t* node, uint32_t ino, uint64_t lb, uint64_t* phys,
                  std::string* err);

  BlockSource* src_ = nullptr;
  uint32_t bs_ = 0;                 // block size
  uint64_t fsBlocks_ = 0;
  uint32_t blocksPerGroup_ = 0, inodesPerGroup_ = 0, inodeSize_ = 0;
  uint32_t descSize_ = 32, firstDataBlock_ = 0, groupCount_ = 0;
  std::vector<uint64_t> inodeTables_;   // cached per group
  // 1-entry extent cache {inode, ee_block, ee_start, ee_len}
  uint32_t cacheIno_ = 0;
  uint64_t cacheEeBlock_ = 0, cacheEeStart_ = 0, cacheEeLen_ = 0;
};

}  // namespace aotdroid
