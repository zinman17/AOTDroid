// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/vfs.cpp
// ============================================================================
#include "vfs.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace aotdroid {

// ---------------------------------------------------------------------------
// Wakeup pipe shared by virtual nodes (readiness -> epoll emulation).
// ---------------------------------------------------------------------------
namespace {
class WakeupPipe {
 public:
  WakeupPipe() {
#ifdef AOTDROID_APPLE
    if (::pipe(w_) == 0) {
      ::fcntl(w_[0], F_SETFL, O_NONBLOCK);
      ::fcntl(w_[1], F_SETFL, O_NONBLOCK);
    } else { w_[0] = w_[1] = -1; }
#else
    w_[0] = w_[1] = -1;
#endif
  }
  void kick() { if (w_[1] >= 0) { char c = 1; (void)!::write(w_[1], &c, 1); } }
  int readEnd() const { return w_[0]; }
 private:
  int w_[2] = {-1, -1};
};
WakeupPipe& wakeup() { static WakeupPipe w; return w; }
}  // namespace

void VNode::armWakeup(VNode* n) { (void)n; wakeup().kick(); }

// ---------------------------------------------------------------------------
// FDTable
// ---------------------------------------------------------------------------
FDTable& fds() { static FDTable t; return t; }

int FDTable::alloc(const VNodeRef& n) {
  std::lock_guard<std::mutex> lk(mu_);
  int fd = next_++;
  while (fds_.count(fd) && fd < kMax) fd = next_++;
  if (fd >= kMax) return -1;
  fds_[fd] = n;
  return fd;
}
VNodeRef FDTable::get(int fd) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = fds_.find(fd);
  return it == fds_.end() ? nullptr : it->second;
}
VNodeRef FDTable::release(int fd) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = fds_.find(fd);
  if (it == fds_.end()) return nullptr;
  VNodeRef r = it->second;
  fds_.erase(it);
  return r;
}
bool FDTable::close(int fd) { return release(fd) != nullptr; }
int FDTable::dup(int oldFd, int newFd) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = fds_.find(oldFd);
  if (it == fds_.end()) return -1;
  if (newFd < 0) {
    newFd = next_++;
    while (fds_.count(newFd) && newFd < kMax) newFd = next_++;
    if (newFd >= kMax) return -1;
  } else {
    fds_.erase(newFd);
  }
  fds_[newFd] = it->second;
  return newFd;
}
void FDTable::forEach(const std::function<void(int, const VNodeRef&)>& fn) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& kv : fds_) fn(kv.first, kv.second);
}
size_t FDTable::count() const {
  std::lock_guard<std::mutex> lk(mu_);
  return fds_.size();
}

// ---------------------------------------------------------------------------
// PathTranslator
// ---------------------------------------------------------------------------
PathTranslator& paths() { static PathTranslator p; return p; }

void PathTranslator::addMount(const std::string& linuxPrefix,
                              const std::string& hostPrefix) {
  mounts_.emplace_back(linuxPrefix, hostPrefix);
  std::sort(mounts_.begin(), mounts_.end(),
            [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
}
bool PathTranslator::findMount(const std::string& linuxPath,
                               std::string* rest) const {
  for (const auto& m : mounts_) {
    if (linuxPath.compare(0, m.first.size(), m.first) == 0) {
      if (rest) *rest = linuxPath.substr(m.first.size());
      return true;
    }
  }
  return false;
}

PathTranslator::Special PathTranslator::classify(const std::string& p) {
  struct Rule { const char* path; Special s; };
  static const Rule kRules[] = {
    {"/dev/binder", Special::Binder},
    {"/dev/hwbinder", Special::HwBinder},
    {"/dev/vndbinder", Special::VndBinder},
    {"/dev/ashmem", Special::Ashmem},
    {"/dev/ashmem_legacy", Special::AshmemLegacy},
    {"/dev/input/event0", Special::InputEvent0},
    {"/dev/input/event1", Special::InputEvent1},
    {"/dev/urandom", Special::Urandom},
    {"/dev/random", Special::Random},
    {"/dev/null", Special::Null},
    {"/dev/zero", Special::Zero},
    {"/dev/kmsg", Special::DevKmsg},
    {"/dev/graphics/fb0", Special::Fb0},
    {"/dev/__properties__", Special::DevProperties},
    {"/dev/__properties__/properties_serialized", Special::DevProperties},
    {"/proc/self/maps", Special::ProcSelfMaps},
    {"/proc/self/cmdline", Special::ProcSelfCmdline},
    {"/proc/self/status", Special::ProcSelfStatus},
    {"/proc/self/stat", Special::ProcSelfStat},
    {"/proc/cpuinfo", Special::ProcCpuinfo},
    {"/sys/devices/system/cpu/present", Special::SysCpuPresent},
    {"/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", Special::SysCpuMaxFreq},
  };
  for (const auto& r : kRules)
    if (p == r.path) return r.s;
  // /dev/__properties__/<context files> are all property areas
  if (p.rfind("/dev/__properties__/", 0) == 0) return Special::DevProperties;
  return Special::None;
}

std::string PathTranslator::translate(const std::string& linuxPath) const {
  std::string rest;
  if (findMount(linuxPath, &rest)) {
    // find the matching mount's host prefix
    for (const auto& m : mounts_)
      if (linuxPath.compare(0, m.first.size(), m.first) == 0) {
        std::string host = m.second;
        while (!host.empty() && host.back() == '/') host.pop_back();
        while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
        return host + "/" + rest;
      }
  }
  return linuxPath;  // relative / unknown: pass through
}

// ---------------------------------------------------------------------------
// PackVFS
// ---------------------------------------------------------------------------
PackVFS& rootfsPack() { static PackVFS p; return p; }

bool PackVFS::open(const std::string& hostPath, std::string* err) {
  FILE* f = fopen(hostPath.c_str(), "rb");
  if (!f) { *err = "open pack"; return false; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= (long)sizeof(PackHeader)) { fclose(f); *err = "pack too small"; return false; }
  map_.resize((size_t)sz);
  size_t rd = fread(map_.data(), 1, map_.size(), f);
  fclose(f);
  if (rd != map_.size()) { *err = "pack read"; return false; }
  memcpy(&hdr_, map_.data(), sizeof(hdr_));
  if (memcmp(hdr_.magic, "AOTPACK1", 8) != 0) { *err = "pack magic"; return false; }
  entries_ = (const PackEntry*)(map_.data() + hdr_.entryOff);
  strs_ = (const char*)(map_.data() + hdr_.strOff);
  return true;
}

bool PackVFS::lookup(const std::string& linuxPath, PackEntry* out,
                     const uint8_t** data) const {
  // normalize: strip leading '/' for pack-internal keys
  std::string key = linuxPath;
  while (!key.empty() && key.front() == '/') key.erase(key.begin());
  for (uint32_t i = 0; i < hdr_.entryCount; ++i) {
    const PackEntry& e = entries_[i];
    if (strcmp(strs_ + e.nameOff, key.c_str()) == 0) {
      if (out) *out = e;
      if (data) *data = map_.data() + e.blobOff;
      return true;
    }
  }
  return false;
}

std::vector<std::string> PackVFS::listDir(const std::string& linuxDir) const {
  std::string key = linuxDir;
  while (!key.empty() && key.front() == '/') key.erase(key.begin());
  while (!key.empty() && key.back() == '/') key.pop_back();
  std::string pfx = key.empty() ? "" : key + "/";
  std::unordered_map<std::string, bool> seen;
  for (uint32_t i = 0; i < hdr_.entryCount; ++i) {
    const char* nm = strs_ + entries_[i].nameOff;
    if (pfx.empty() ? !strchr(nm, '/') : (strncmp(nm, pfx.c_str(), pfx.size()) == 0
        && strchr(nm + pfx.size(), '/') == nullptr)) {
      seen[std::string(pfx.empty() ? nm : nm + pfx.size())] = true;
    } else if (!pfx.empty() && strncmp(nm, pfx.c_str(), pfx.size()) == 0) {
      const char* slash = strchr(nm + pfx.size(), '/');
      if (slash) seen[std::string(nm + pfx.size(), slash)] = true;  // subdirs
    }
  }
  std::vector<std::string> out;
  out.push_back("."); out.push_back("..");
  for (auto& kv : seen) out.push_back(kv.first);
  std::sort(out.begin() + 2, out.end());
  return out;
}

// ---------------------------------------------------------------------------
// Synthetic files
// ---------------------------------------------------------------------------
namespace {
std::unordered_map<std::string, SynthGen>& synthTable() {
  static std::unordered_map<std::string, SynthGen> t;
  return t;
}
}  // namespace

void registerSynth(const std::string& linuxPathPrefix, SynthGen gen) {
  synthTable()[linuxPathPrefix] = std::move(gen);
}

std::string genProcSelfMaps() {
  std::string out;
  static std::atomic<uint64_t> nextIno{0x2a00};
  for (const auto& m : mmapRegistry().snapshot()) {
    char line[512];
    char perms[5] = {
      (char)(m.prot & 1 ? 'r' : '-'), (char)(m.prot & 2 ? 'w' : '-'),
      (char)(m.prot & 4 ? 'x' : '-'), 'p', 0};
    snprintf(line, sizeof(line), "%012llx-%012llx %s %08llx 00:00 %llu %s\n",
             (unsigned long long)m.start,
             (unsigned long long)(m.start + m.len),
             perms, (unsigned long long)(m.pgoff & 0xffffffffULL),
             (unsigned long long)nextIno.fetch_add(1),
             m.path.empty() ? "[anon:aotdroid]" : m.path.c_str());
    out += line;
  }
  return out;
}

std::string genProcCpuinfo() {
  // A synthesized arm64 /proc/cpuinfo matching the profiled core count.
  std::string out;
  char block[512];
  for (int i = 0; i < 6; ++i) {
    snprintf(block, sizeof(block),
      "processor\t: %d\nBogoMIPS\t: 38.40\nFeatures\t: fp asimd aes pmull sha1 sha2 crc32\n"
      "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
      "CPU part\t: 0x%d\nCPU revision\t: %d\n\n",
      i, i < 4 ? 0xd0c : 0xd0d, i < 4 ? 3 : 2);
    out += block;
  }
  out += "Hardware\t: AOTDroid Virtual SoC\n";
  return out;
}
std::string genProcSelfCmdline() {
  return std::string("system_server\0", 15);
}
std::string genSysCpuPresent() { return "0-5\n"; }
std::string genSysCpuMaxFreq() { return "2419200000\n"; }

// ---------------------------------------------------------------------------
// MmapRegistry
// ---------------------------------------------------------------------------
MmapRegistry& mmapRegistry() { static MmapRegistry r; return r; }

void MmapRegistry::add(uint64_t start, uint64_t len, uint32_t prot,
                       uint32_t flags, uint64_t pgoff, const std::string& path,
                       bool oatCode) {
  std::lock_guard<std::mutex> lk(mu_);
  MmapRec r{start, len, prot, flags, pgoff, path, oatCode};
  recs_.push_back(r);
  lastTouchMs_[start] = 0;
}
void MmapRegistry::remove(uint64_t start, uint64_t len) {
  std::lock_guard<std::mutex> lk(mu_);
  for (size_t i = 0; i < recs_.size(); ) {
    if (recs_[i].start >= start && recs_[i].start + recs_[i].len <= start + len) {
      std::swap(recs_[i], recs_.back());
      recs_.pop_back();
    } else ++i;
  }
  lastTouchMs_.erase(start);
}
std::vector<MmapRec> MmapRegistry::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return recs_;
}
size_t MmapRegistry::evictColdOatPages(uint64_t minAgeMs) {
  size_t pages = 0;
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t nowMs = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& r : recs_) {
    if (!r.oatCode) continue;
    auto it = lastTouchMs_.find(r.start);
    if (it == lastTouchMs_.end()) continue;
    if (nowMs - it->second >= minAgeMs) {
#ifdef AOTDROID_APPLE
      if (madvise((void*)(uintptr_t)r.start, r.len, MADV_DONTNEED) == 0)
        pages += (size_t)((r.len + 16383) / 16384);
#else
      (void)0;
#endif
    }
  }
  return pages;
}

}  // namespace aotdroid
