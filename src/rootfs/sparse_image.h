// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/sparse_image.h
//  Android sparse image (simg) reader: magic 0x3AFF26ED, chunk-based mapping
//  of logical blocks -> raw / fill / dont-care runs. GSI zips ship sparse
//  system.img files; this layer restores the logical view without ever
//  materializing the raw image on disk.
// ============================================================================
#pragma once
#include "block_source.h"
#include <functional>
#include <vector>

namespace aotdroid {

// Stateless one-shot probe: is the image at `src` an Android sparse image?
bool sparseProbe(BlockSource* src);

class SparseReader {
 public:
  // Parses the sparse header + chunk map. On a non-sparse image this behaves
  // as a 1:1 passthrough (open still succeeds, isSparse() == false).
  bool open(BlockSource* src, std::string* err);

  bool isSparse() const { return sparse_; }
  uint64_t logicalSize() const { return logicalSize_; }

  // Read at a logical offset. Handles chunk boundaries internally; returns
  // bytes read or -1 (dont-care and fill chunks produce zeros / pattern).
  ssize_t preadLogical(uint64_t off, size_t n, uint8_t* out) const;

 private:
  struct Chunk {
    uint64_t logicalStart;   // in bytes
    uint64_t len;            // in bytes
    uint64_t fileOff;        // raw: offset into backing source
    uint32_t fill;           // fill chunk pattern
    uint16_t type;           // 0 raw, 1 fill, 2 dont-care, 3 crc32
  };
  BlockSource* src_ = nullptr;
  bool sparse_ = false;
  uint64_t blockSize_ = 0;
  uint64_t logicalSize_ = 0;
  std::vector<Chunk> chunks_;   // ordered by logicalStart
};

// BlockSource that presents the logical view of a (possibly sparse) image.
class SparseBlockSource final : public BlockSource {
 public:
  bool open(BlockSource* backing, std::string* err) {
    src_ = backing;
    if (!rd_.open(backing, err)) return false;
    return true;
  }
  ssize_t pread(uint64_t offset, size_t n, uint8_t* out) override {
    return rd_.preadLogical(offset, n, out);
  }
  uint64_t size() const override { return rd_.logicalSize(); }
 private:
  BlockSource* src_ = nullptr;
  SparseReader rd_;
};

}  // namespace aotdroid
