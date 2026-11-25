// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/zip_reader.h
//  Minimal read-only ZIP reader (module: on-device rootfs import).
//  Supports: EOCD + ZIP64 central directory, STORED + DEFLATE entries,
//  unix mode bits from external attributes (dirs / symlinks), random-access
//  reads into STORED entries (lets the ext4 reader pread straight out of a
//  GSI .zip without unpacking it first) and streaming extraction.
//  Pure POSIX + zlib: compiles on Linux hosts and iPhoneOS alike.
// ============================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aotdroid {

class ZipReader {
 public:
  struct Entry {
    std::string name;
    uint64_t uncompressedSize = 0;
    uint64_t compressedSize = 0;
    uint64_t localHeaderOffset = 0;
    uint16_t method = 0;        // 0 = stored, 8 = deflate
    uint32_t crc32 = 0;
    uint32_t unixMode = 0;      // external attrs >> 16 (0 if not unix)
    bool isDir = false;
    bool isSymlink = false;
    bool stored() const { return method == 0; }
  };

  bool open(const std::string& path, std::string* err);
  void close();
  ~ZipReader() { close(); }

  const std::vector<Entry>& entries() const { return entries_; }
  const Entry* find(const std::string& name) const;

  // Random access into a STORED entry. `offset` is relative to the entry's
  // uncompressed data. Returns bytes read or -1.
  ssize_t storedRead(const Entry& e, uint64_t offset, size_t n, uint8_t* out) const;

  // Streaming extraction of any entry. The sink receives contiguous chunks;
  // returning false aborts. Returns false on any error (err set).
  using Sink = std::function<bool(const uint8_t*, size_t)>;
  bool extract(const Entry& e, const Sink& sink, std::string* err) const;

  // Convenience: whole entry -> file (O_TRUNC).
  bool extractToFile(const Entry& e, const std::string& outPath, std::string* err) const;

  uint64_t fileSize() const { return fileSize_; }

 private:
  bool parseCentralDirectory(std::string* err);
  uint64_t dataOffsetOf(const Entry& e, std::string* err) const;  // abs offset of entry data
  bool extractDeflate(const Entry& e, uint64_t dataOff, const Sink& sink,
                      std::string* err) const;

  int fd_ = -1;
  uint64_t fileSize_ = 0;
  std::vector<Entry> entries_;
  // lazily computed absolute data offsets (index-aligned with entries_)
  mutable std::vector<uint64_t> dataOff_;
};

// True if the file starts with the ZIP magic ("PK\x03\x04" / "PK\x05\x06").
bool zipLooksLikeZip(const std::string& path);

}  // namespace aotdroid
