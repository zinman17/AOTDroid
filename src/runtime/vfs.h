// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/vfs.h
//  Virtual filesystem + file-descriptor namespace for the Linux-to-Darwin
//  syscall bridge (module 4). Every "fd" the emulated Linux kernel hands to
//  Android user space is routed through here: real host files, binder
//  domains, ashmem areas, evdev, epoll-over-kqueue, eventfd, timerfd and
//  synthetic /proc nodes.
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"

namespace aotdroid {

enum class VNKind : uint8_t {
  None, HostFile, HostDir, Binder, Ashmem, Evdev, PropArea,
  Epoll, Eventfd, Timerfd, Synth, NullDev, RandomDev
};

struct VNode;
using VNodeRef = std::shared_ptr<VNode>;

struct VNode {
  VNKind kind = VNKind::None;
  int hostFd = -1;          // HostFile / HostDir / Epoll (kqueue fd) / Timerfd pipe
  uint32_t openFlags = 0;
  void* ctx = nullptr;      // BinderDomain* / AshmemArea* / InputHub* / EpollState* ...
  uint64_t cookie = 0;      // aux: 1 = pack-backed HostFile; binder context id
  uint64_t pos = 0;
  uint64_t ino = 0;
  std::string name;         // diagnostic + synthetic path
  // Poll readiness (virtual nodes): returns EPOLL-style bits.
  uint32_t (*pollFn)(VNode*) = nullptr;
  // Readiness change notification pipe (shared, watched by epoll emulation).
  static void armWakeup(VNode* n);
};

// ---------------------------------------------------------------------------
class FDTable {
 public:
  static constexpr int kMax = 4096;
  int alloc(const VNodeRef& n);
  VNodeRef get(int fd);
  VNodeRef release(int fd);         // for dup2-style replace
  bool close(int fd);
  int dup(int oldFd, int newFd = -1);
  void forEach(const std::function<void(int, const VNodeRef&)>& fn);
  size_t count() const;
 private:
  mutable std::mutex mu_;
  std::unordered_map<int, VNodeRef> fds_;
  int next_ = 10;   // 0..9 reserved (stdio mirrors)
};

FDTable& fds();

// ---------------------------------------------------------------------------
// Path translation: Linux paths -> iOS container paths, with special nodes.
// ---------------------------------------------------------------------------
class PathTranslator {
 public:
  void addMount(const std::string& linuxPrefix, const std::string& hostPrefix);
  void setContainerRoot(const std::string& root) { container_ = root; }
  const std::string& containerRoot() const { return container_; }

  enum class Special : uint32_t {
    None = 0, Binder, HwBinder, VndBinder, Ashmem, AshmemLegacy,
    InputEvent0, InputEvent1, Urandom, Random, Null, Zero, DevKmsg,
    ProcSelfMaps, ProcSelfCmdline, ProcSelfStatus, ProcCpuinfo,
    ProcSelfStat, SysCpuPresent, SysCpuMaxFreq, DevProperties, DevKvm,
    Fb0, DevGoldfishPipe, DevPtmx, DevTty
  };
  static Special classify(const std::string& linuxPath);
  // Translate a Linux path into a host path (or leave special nodes alone).
  std::string translate(const std::string& linuxPath) const;
  bool findMount(const std::string& linuxPath, std::string* rest) const;
 private:
  std::string container_;
  std::vector<std::pair<std::string, std::string>> mounts_;
};

PathTranslator& paths();

// ---------------------------------------------------------------------------
// Read-only rootfs pack (.aotpack) served from the app container.
// Format (little-endian):
//   PackHeader{ magic "AOTPACK1", u32 version, u32 entryCount,
//               u64 strOff, u64 entryOff, u64 blobOff }
//   string table (NUL-terminated paths, e.g. "system/framework/boot.oat")
//   PackEntry[] { u32 nameOff, u32 mode, u32 uid, u32 gid,
//                 u64 blobOff, u64 size, u32 flags }
//   blob area (entries that carry kFlagExec start 16 KiB-aligned)
// ---------------------------------------------------------------------------
struct PackHeader {
  char magic[8];
  uint32_t version, entryCount;
  uint64_t strOff, entryOff, blobOff;
};
struct PackEntry {
  uint32_t nameOff, mode, uid, gid;
  uint64_t blobOff, size;
  uint32_t flags;   // bit0 = exec (16 KiB aligned), bit1 = dir, bit2 = symlink
};
constexpr uint32_t kPackFlagExec = 1, kPackFlagDir = 2, kPackFlagSymlink = 4;

class PackVFS {
 public:
  bool open(const std::string& hostPath, std::string* err);
  // Path lookup with symlink resolution (open semantics): a pack symlink is
  // stored as its target string; following the chain rewrites the key.
  bool lookup(const std::string& linuxPath, PackEntry* out, const uint8_t** data) const;
  // No-symlink-follow variant (readlink / lstat semantics).
  bool lookupRaw(const std::string& linuxPath, PackEntry* out,
                 const uint8_t** data) const;
  // list directory (returns entry names)
  std::vector<std::string> listDir(const std::string& linuxDir) const;
  const PackHeader& header() const { return hdr_; }
  ~PackVFS();
  void close();
 private:
  bool lookupNormalized(std::string key, PackEntry* out, const uint8_t** data,
                        bool followSymlinks) const;
  // pack bytes served via mmap (multi-GB GSIs must not sit in heap)
  uint8_t* mapBase_ = nullptr;
  size_t mapLen_ = 0;
  int fd_ = -1;
  bool sorted_ = false;
  PackHeader hdr_{};
  const PackEntry* entries_ = nullptr;
  const char* strs_ = nullptr;
};

PackVFS& rootfsPack();

// ---------------------------------------------------------------------------
// Synthetic file support: generated on open (e.g. /proc/self/maps).
// ---------------------------------------------------------------------------
using SynthGen = std::function<std::string(const std::string& linuxPath)>;
void registerSynth(const std::string& linuxPathPrefix, SynthGen gen);
std::string genProcSelfMaps();
std::string genProcCpuinfo();
std::string genProcSelfCmdline();
std::string genSysCpuPresent();
std::string genSysCpuMaxFreq();

// ---------------------------------------------------------------------------
// mmap registry (drives /proc/self/maps + Jetsam madvise walks).
// ---------------------------------------------------------------------------
struct MmapRec {
  uint64_t start, len;
  uint32_t prot;              // Linux PROT_*
  uint32_t flags;
  uint64_t pgoff;
  std::string path;
  bool oatCode = false;       // eligible for cold-page eviction
};
class MmapRegistry {
 public:
  void add(uint64_t start, uint64_t len, uint32_t prot, uint32_t flags,
           uint64_t pgoff, const std::string& path, bool oatCode);
  void remove(uint64_t start, uint64_t len);
  std::vector<MmapRec> snapshot() const;
  // madvise(MADV_DONTNEED) on all oatCode pages not touched since `epoch`.
  size_t evictColdOatPages(uint64_t minAgeMs);
 private:
  mutable std::mutex mu_;
  std::vector<MmapRec> recs_;
  std::unordered_map<uint64_t, uint64_t> lastTouchMs_;
};

MmapRegistry& mmapRegistry();

// EPOLL_* (Linux eventpoll.h values -- these are what Android code expects).
constexpr uint32_t kEPOLLIN = 0x001, kEPOLLPRI = 0x002, kEPOLLOUT = 0x004,
                   kEPOLLERR = 0x008, kEPOLLHUP = 0x010, kEPOLLRDHUP = 0x2000,
                   kEPOLLET = 0x80000000u, kEPOLLONESHOT = 0x40000000u;

}  // namespace aotdroid
