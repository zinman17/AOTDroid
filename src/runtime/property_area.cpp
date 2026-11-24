// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/property_area.cpp   (MODULE 3)
// ============================================================================
#include "property_area.h"

namespace aotdroid {

// ---------------------------------------------------------------------------
// Trie internals (Android 8.0+ serialized format)
// ---------------------------------------------------------------------------
namespace {

struct TrieWriter {
  std::vector<uint8_t> area;
  size_t dataSize = 0;
  uint32_t rootOff = 0;

  explicit TrieWriter(size_t capacity) { area.assign(capacity, 0); }

  void begin() { dataSize = sizeof(PropAreaHeaderTrie); }   // header at [0,32)

  size_t allocBytes(size_t n) {
    size_t off = dataSize;
    dataSize += (n + 7) & ~7ull;   // 8-byte stride alignment
    if (dataSize + 256 > area.size()) area.resize(area.size() + 4096, 0);
    return off;
  }
  size_t newPropBt(const char* name) {
    PropBt bt{};
    snprintf(bt.name, kPropNameMax, "%s", name);
    size_t off = allocBytes(sizeof(bt));
    memcpy(area.data() + off, &bt, sizeof(bt));
    return off;
  }
  size_t newPropInfo(const char* name, const char* value) {
    PropInfoTrie pi{};
    snprintf(pi.name, kPropNameMax, "%s", name);
    snprintf(pi.value, kPropValueMax, "%s", value);
    pi.serial = prop_serial_make((uint32_t)strnlen(value, kPropValueMax));
    size_t off = allocBytes(sizeof(pi));
    memcpy(area.data() + off, &pi, sizeof(pi));
    return off;
  }
  PropBt* btAt(uint32_t off) { return (PropBt*)(area.data() + off); }

  // Find-or-create the trie node for `name` under the root. All links are
  // tracked by OFFSET, never by pointer: allocations may resize the area
  // vector and invalidate any PropBt* taken before the allocation.
  uint32_t findNode(const char* name) {
    if (rootOff == 0) rootOff = (uint32_t)newPropBt("");
    uint32_t cur = rootOff;
    const char* p = name;
    while (*p) {
      const char* dot = strchr(p, '.');
      size_t segLen = dot ? (size_t)(dot - p) : strlen(p);
      char seg[kPropNameMax];
      if (segLen >= kPropNameMax) segLen = kPropNameMax - 1;
      memcpy(seg, p, segLen); seg[segLen] = 0;
      // link location: (nodeOffset, useRight) of the field to write when
      // creating a new sibling
      uint32_t linkNode = cur;
      bool useRight = false;
      uint32_t child = btAt(cur)->children;
      while (child != 0 && strcmp(btAt(child)->name, seg) != 0) {
        linkNode = child;
        useRight = true;
        child = btAt(child)->right;
      }
      if (child == 0) {
        child = (uint32_t)newPropBt(seg);
        if (useRight) btAt(linkNode)->right = child;
        else          btAt(linkNode)->children = child;
      }
      cur = child;
      p += segLen + (dot ? 1 : 0);
    }
    return cur;
  }
  void insert(const char* name, const char* value) {
    uint32_t node = findNode(name);
    uint32_t propOff = (uint32_t)newPropInfo(name, value);
    if (btAt(node)->prop == 0) {
      btAt(node)->prop = propOff;      // re-fetch after possible resize
    } else {
      PropInfoTrie* pi = (PropInfoTrie*)(area.data() + btAt(node)->prop);
      snprintf(pi->value, kPropValueMax, "%s", value);
      pi->serial = prop_serial_make((uint32_t)strnlen(value, kPropValueMax));
    }
  }
  void finish() {
    PropAreaHeaderTrie hdr{};
    hdr.bytes_used = (uint32_t)dataSize;
    hdr.magic = kPropAreaMagic;
    hdr.version = kPropAreaVersionTrie;
    hdr.serial = 0;
    memcpy(area.data(), &hdr, sizeof(hdr));
    area.resize(dataSize);
  }
};

}  // namespace

PropertyAreaBuilder::Result PropertyAreaBuilder::build(
    uint32_t apiLevel,
    const std::vector<std::pair<std::string, std::string>>& props,
    const std::string& defaultContext) {
  Result r;
  r.apiLevel = apiLevel;

  if (apiLevel <= 27) {
    // -------- flat (7.x / 8.x) single area --------
    // Sort by name (bionic binary-searches a name-ordered toc).
    std::vector<std::pair<std::string, std::string>> sorted = props;
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
    size_t n = sorted.size();
    size_t tocBytes = (n + 1) * sizeof(uint32_t);
    size_t bodyBytes = n * sizeof(PropInfoFlat);
    size_t total = sizeof(PropAreaHeaderFlat) + tocBytes + bodyBytes;
    r.main.assign(total, 0);
    PropAreaHeaderFlat hdr{};
    hdr.magic = kPropAreaMagic;
    hdr.version = kPropAreaVersionFlat;
    hdr.bytes_used = (uint32_t)total;
    hdr.toc_size = (uint32_t)n;
    memcpy(r.main.data(), &hdr, sizeof(hdr));
    uint32_t* toc = (uint32_t*)(r.main.data() + sizeof(PropAreaHeaderFlat));
    PropInfoFlat* pis = (PropInfoFlat*)(r.main.data() + sizeof(PropAreaHeaderFlat)
                                        + tocBytes);
    for (size_t i = 0; i < n; ++i) {
      PropInfoFlat& pi = pis[i];
      snprintf(pi.name, kPropNameMax, "%s", sorted[i].first.c_str());
      snprintf(pi.value, kPropValueMax, "%s", sorted[i].second.c_str());
      pi.serial = prop_serial_make((uint32_t)strnlen(pi.value, kPropValueMax));
      toc[i] = (uint32_t)(sizeof(PropAreaHeaderFlat) + tocBytes
                          + i * sizeof(PropInfoFlat));
    }
    // terminating entry (bionic reads one past during search)
    toc[n] = (uint32_t)(sizeof(PropAreaHeaderFlat) + tocBytes + bodyBytes);
    return r;
  }

  // -------- serialized trie (Android 10) --------
  TrieWriter tw(4096);
  tw.begin();
  for (auto& kv : props) tw.insert(kv.first.c_str(), kv.second.c_str());
  tw.finish();
  r.main = std::move(tw.area);
  // context split: everything under "ro." gets the default context file;
  // SELinux-strict contexts are produced by the rootfs build from
  // plat_property_contexts; here we emit the default split.
  std::vector<std::pair<std::string, std::string>> ctxProps;
  for (auto& kv : props)
    if (kv.first.rfind("ro.", 0) == 0) ctxProps.push_back(kv);
  if (!ctxProps.empty()) {
    TrieWriter cw(4096);
    cw.begin();
    for (auto& kv : ctxProps) cw.insert(kv.first.c_str(), kv.second.c_str());
    cw.finish();
    r.contexts.emplace_back(defaultContext, std::move(cw.area));
  }
  return r;
}

bool PropertyAreaBuilder::readProp(const uint8_t* area, size_t areaLen,
                                   uint32_t areaVersion, const char* name,
                                   char* outValue, size_t outLen) {
  if (areaLen < 8) return false;
  if (areaVersion == kPropAreaVersionFlat) {
    PropAreaHeaderFlat hdr;
    memcpy(&hdr, area, sizeof(hdr));
    if (hdr.magic != kPropAreaMagic || hdr.version != 1) return false;
    const uint32_t* toc = (const uint32_t*)(area + sizeof(PropAreaHeaderFlat));
    size_t lo = 0, hi = hdr.toc_size;   // binary search over name-sorted toc
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (mid >= hdr.toc_size) return false;
      const PropInfoFlat* pi = (const PropInfoFlat*)(area + toc[mid]);
      int c = strcmp(pi->name, name);
      if (c == 0) {
        snprintf(outValue, outLen, "%s", pi->value);
        return true;
      }
      if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
  }
  if (areaVersion == kPropAreaVersionTrie) {
    PropAreaHeaderTrie hdr;
    memcpy(&hdr, area, sizeof(hdr));
    if (hdr.magic != kPropAreaMagic || hdr.version != 2) return false;
    uint32_t cur = sizeof(PropAreaHeaderTrie);   // root prop_bt
    const char* p = name;
    while (*p) {
      const char* dot = strchr(p, '.');
      size_t segLen = dot ? (size_t)(dot - p) : strlen(p);
      char seg[kPropNameMax];
      if (segLen >= kPropNameMax) return false;
      memcpy(seg, p, segLen); seg[segLen] = 0;
      uint32_t child = ((const PropBt*)(area + cur))->children;
      while (child != 0 && strcmp(((const PropBt*)(area + child))->name, seg) != 0)
        child = ((const PropBt*)(area + child))->right;
      if (child == 0) return false;
      cur = child;
      p += segLen + (dot ? 1 : 0);
    }
    uint32_t propOff = ((const PropBt*)(area + cur))->prop;
    if (propOff == 0) return false;
    const PropInfoTrie* pi = (const PropInfoTrie*)(area + propOff);
    snprintf(outValue, outLen, "%s", pi->value);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// PropertyStore
// ---------------------------------------------------------------------------
PropertyStore& PropertyStore::get() { static PropertyStore s; return s; }

void PropertyStore::setDefault(const std::string& k, const std::string& v) {
  std::lock_guard<std::mutex> lk(mu_);
  props_[k] = v;
}
std::string PropertyStore::get(const std::string& k, const std::string& def) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = props_.find(k);
  return it == props_.end() ? def : it->second;
}
bool PropertyStore::set(const std::string& k, const std::string& v) {
  std::lock_guard<std::mutex> lk(mu_);
  if (props_.count(k) && k.rfind("ro.", 0) == 0) return false;   // ro.* immutable
  props_[k] = v;
  return true;
}
std::vector<std::pair<std::string, std::string>> PropertyStore::all() const {
  std::lock_guard<std::mutex> lk(mu_);
  return {props_.begin(), props_.end()};
}
size_t PropertyStore::count() const {
  std::lock_guard<std::mutex> lk(mu_);
  return props_.size();
}

void applyBootProperties(uint32_t apiLevel, const char* abi, const char* deviceModel,
                         const char* fingerprintSuffix) {
  auto& ps = PropertyStore::get();
  struct Triple { const char* k, * v25, * v27, * v29; };
  static const Triple kTable[] = {
    {"ro.build.version.sdk",      "25",   "27",   "29"},
    {"ro.build.version.release",  "7.1.2", "8.1.0", "10"},
    {"ro.build.version.codename", "REL",  "REL",  "REL"},
    {"ro.build.id",               "AOT71", "AOT81", "AOT10"},
    {"ro.build.version.incremental", "aotdroid-71", "aotdroid-81", "aotdroid-10"},
    {"ro.build.version.security_patch", "2019-08-05", "2019-08-05", "2020-11-05"},
    {"ro.build.tags",             "release-keys", "release-keys", "release-keys"},
    {"ro.build.type",             "userdebug", "userdebug", "userdebug"},
    {"ro.treble.enabled",         "false", "true", "true"},
    {"ro.zygote",                 "zygote64", "zygote64", "zygote64"},
    {"ro.dalvik.vm.native.bridge", "0", "0", "0"},
    {"persist.sys.dalvik.vm.lib.2", "libart.so", "libart.so", "libart.so"},
    {"dalvik.vm.isa.arm64.variant", "generic", "generic", "generic"},
    {"ro.hardware",               "aotdroid", "aotdroid", "aotdroid"},
    {"ro.board.platform",         "aotdroid", "aotdroid", "aotdroid"},
    {"ro.hardware.egl",           "angle", "angle", "angle"},
    {"ro.sf.lcd_density",         "264", "264", "264"},
    {"debug.sf.hwc.canUseIC",     "0", "0", "0"},
  };
  const int col = apiLevel <= 25 ? 1 : (apiLevel <= 27 ? 2 : 3);
  for (auto& t : kTable)
    ps.setDefault(t.k, col == 1 ? t.v25 : (col == 2 ? t.v27 : t.v29));
  ps.setDefault("ro.product.cpu.abilist", abi ? abi : "arm64-v8a");
  ps.setDefault("ro.product.model", deviceModel ? deviceModel : "AOTDroid");
  ps.setDefault("ro.product.device", "aotdroid");
  ps.setDefault("ro.product.name", "AOTDroid");
  ps.setDefault("ro.product.brand", "AOTDroid");
  ps.setDefault("ro.product.manufacturer", "AOTDroid");
  std::string fp = "AOTDroid/aotdroid/aotdroid:";
  fp += apiLevel <= 25 ? "7.1.2" : (apiLevel <= 27 ? "8.1.0" : "10");
  fp += "/";
  fp += apiLevel <= 25 ? "AOT71" : (apiLevel <= 27 ? "AOT81" : "AOT10");
  fp += "/aotdroid:";
  fp += fingerprintSuffix ? fingerprintSuffix : "userdebug";
  fp += "/release-keys";
  ps.setDefault("ro.build.fingerprint", fp);
  ps.setDefault("ro.bootmode", "unknown");
  ps.setDefault("ro.secure", "1");
  ps.setDefault("ro.debuggable", "1");
  ps.setDefault("ro.allow.mock.location", "0");
  ps.setDefault("sys.usb.config", "none");
  ps.setDefault("ro.aotdroid.container", "1");
}

}  // namespace aotdroid
