// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/rootfs_manager.h
//  On-device GSI import pipeline: user drops a .zip (or bare .img) on the
//  app via the document picker; this module inspects it, walks the Android
//  system image (ext4, optionally sparse, optionally still inside the zip)
//  and converts it into the mmap-friendly rootfs-<tag>.aotpack the runtime
//  boots from. Also owns the installed-rootfs manifest (rootfs.json).
// ============================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aotdroid {

struct RootfsStatus {
  bool installed = false;
  std::string packPath;
  std::string profileTag;        // android-7.1 / android-8.1 / android-10
  int apiLevel = 0;
  std::string androidVersion;    // ro.build.version.release
  std::string fingerprint;       // ro.build.fingerprint
  std::string importedAtUtc;
  uint64_t packBytes = 0;
  uint32_t entryCount = 0, fileCount = 0, dirCount = 0, symlinkCount = 0;
};

struct ImportProgress {
  std::string phase;             // inspect / unpack / scan / write / finalize
  float fraction = 0;            // 0..1 overall
  uint64_t bytesDone = 0, bytesTotal = 0;
  uint32_t entriesDone = 0, entriesTotal = 0;
};

class RootfsManager {
 public:
  static RootfsManager& get();

  struct Inspection {
    bool ok = false;
    std::string err;
    bool isZip = false;
    bool hasSystemImage = false;      // system.img (ext4, sparse or raw)
    bool hasUnpackedSystem = false;   // system/ tree inside the zip
    bool hasVendor = false;
    int apiLevel = 0;
    std::string androidVersion, fingerprint;
    uint64_t approxBytes = 0;
  };
  Inspection inspect(const std::string& srcPath);

  // Import into containerRoot: writes rootfs-<tag>.aotpack + rootfs.json.
  // Runs on a worker thread (caller's responsibility); progress is best
  // effort. Returns false + *err on failure (partial files are cleaned).
  bool importTo(const std::string& srcPath, const std::string& containerRoot,
                const std::function<void(const ImportProgress&)>& progress,
                std::string* err);

  RootfsStatus status(const std::string& containerRoot);
  bool remove(const std::string& containerRoot, std::string* err);

  // Preferred pack for the wanted tag; falls back to any installed pack.
  static std::string resolvePackPath(const std::string& containerRoot,
                                     const std::string& wantTag,
                                     std::string* actualTag);
  static std::string tagForApiLevel(int api);
  static int apiForTag(const std::string& tag);
  static std::string manifestPath(const std::string& containerRoot);

 private:
  bool importFromZip(const std::string& srcPath, const std::string& containerRoot,
                     const std::function<void(const ImportProgress&)>& progress,
                     std::string* err);
  bool importFromImage(const std::string& srcPath, const std::string& containerRoot,
                       const std::function<void(const ImportProgress&)>& progress,
                       std::string* err);
};

}  // namespace aotdroid
