// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/rootfs/rootfs_manager.cpp
// ============================================================================
#include "rootfs_manager.h"
#include "zip_reader.h"
#include "sparse_image.h"
#include "ext4_reader.h"
#include "pack_writer.h"
#include "../platform/sys_compat.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>

namespace aotdroid {

namespace {

// ---------------------------------------------------------------------------
// tiny helpers
// ---------------------------------------------------------------------------
bool fileExists(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0;
}
bool isDirPath(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
std::vector<std::string> listDirNames(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;
  while (struct dirent* de = ::readdir(d)) {
    std::string n = de->d_name;
    if (n != "." && n != "..") out.push_back(std::move(n));
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}
bool removeTree(const std::string& path) {
  if (isDirPath(path)) {
    for (const auto& n : listDirNames(path))
      if (!removeTree(path + "/" + n)) return false;
    return ::rmdir(path.c_str()) == 0;
  }
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::string nowUtcIso() {
  time_t t = time(nullptr);
  struct tm tmv {};
  gmtime_r(&t, &tmv);
  char b[32];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return b;
}

// build.prop "key=value" scan
std::map<std::string, std::string> parseBuildProp(const std::string& text) {
  std::map<std::string, std::string> kv;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (!kv.count(k)) kv[k] = v;
  }
  return kv;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}
std::string jsonField(const std::string& text, const std::string& key) {
  std::string pat = "\"" + key + "\":";
  size_t p = text.find(pat);
  if (p == std::string::npos) return "";
  p += pat.size();
  while (p < text.size() && (text[p] == ' ' || text[p] == '"')) ++p;
  std::string out;
  while (p < text.size() && text[p] != '"') {
    if (text[p] == '\\' && p + 1 < text.size()) { out += text[p + 1]; p += 2; continue; }
    out += text[p++];
  }
  return out;
}

// Zip entry served as a BlockSource (only valid for STORED entries).
class ZipStoredBlockSource final : public BlockSource {
 public:
  ZipStoredBlockSource() = default;
  ZipStoredBlockSource(const ZipReader* z, const ZipReader::Entry* e)
      : z_(z), e_(e) {}
  void set(const ZipReader* z, const ZipReader::Entry* e) { z_ = z; e_ = e; }
  ssize_t pread(uint64_t offset, size_t n, uint8_t* out) override {
    return z_ ? z_->storedRead(*e_, offset, n, out) : (ssize_t)-1;
  }
  uint64_t size() const override { return e_ ? e_->uncompressedSize : 0; }
 private:
  const ZipReader* z_ = nullptr;
  const ZipReader::Entry* e_ = nullptr;
};

bool looksElf(const uint8_t* p) { return p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F'; }

}  // namespace

// ---------------------------------------------------------------------------
// tag mapping
// ---------------------------------------------------------------------------
RootfsManager& RootfsManager::get() {
  static RootfsManager inst;
  return inst;
}

std::string RootfsManager::tagForApiLevel(int api) {
  if (api <= 0) return "android-10";
  if (api <= 25) return "android-7.1";
  if (api <= 28) return "android-8.1";
  return "android-10";
}
int RootfsManager::apiForTag(const std::string& tag) {
  if (tag == "android-7.1") return 25;
  if (tag == "android-8.1") return 27;
  if (tag == "android-10") return 29;
  return 29;
}
std::string RootfsManager::manifestPath(const std::string& containerRoot) {
  return containerRoot + "/rootfs.json";
}

// ---------------------------------------------------------------------------
// inspection
// ---------------------------------------------------------------------------
RootfsManager::Inspection RootfsManager::inspect(const std::string& srcPath) {
  Inspection ins;
  if (!fileExists(srcPath)) { ins.err = "file not found"; return ins; }

  if (zipLooksLikeZip(srcPath)) {
    ins.isZip = true;
    ZipReader z;
    std::string err;
    if (!z.open(srcPath, &err)) { ins.err = err; return ins; }
    const ZipReader::Entry* sysImg = nullptr;
    const ZipReader::Entry* venImg = nullptr;
    const ZipReader::Entry* buildProp = nullptr;
    uint64_t unpackedBytes = 0;
    for (const auto& e : z.entries()) {
      if (e.isDir) continue;
      std::string n = e.name;
      while (!n.empty() && n.front() == '/') n.erase(n.begin());
      if (n == "system.img" || n == "SYSTEM.img") sysImg = &e;
      else if (n == "vendor.img" || n == "VENDOR.img") venImg = &e;
      else if (n == "system/build.prop" || n == "SYSTEM/build.prop" ||
               n == "system/system/build.prop")
        buildProp = &e;
      if (n.rfind("system/", 0) == 0 || n.rfind("vendor/", 0) == 0)
        unpackedBytes += e.uncompressedSize;
    }
    ins.hasSystemImage = sysImg != nullptr;
    ins.hasUnpackedSystem = buildProp != nullptr || unpackedBytes > 0;
    ins.hasVendor = venImg != nullptr;
    if (!ins.hasSystemImage && !ins.hasUnpackedSystem) {
      ins.err = "zip contains neither system.img nor a system/ tree";
      return ins;
    }
    if (sysImg) ins.approxBytes += sysImg->uncompressedSize;
    if (venImg) ins.approxBytes += venImg->uncompressedSize;
    if (!sysImg) ins.approxBytes = unpackedBytes;

    // read build.prop (unpacked case) for the API level
    if (buildProp) {
      std::string text;
      z.extract(*buildProp, [&](const uint8_t* p, size_t n) {
        text.append((const char*)p, n);
        return true;
      }, &err);
      auto kv = parseBuildProp(text);
      if (kv.count("ro.build.version.sdk"))
        ins.apiLevel = atoi(kv["ro.build.version.sdk"].c_str());
      ins.androidVersion = kv.count("ro.build.version.release")
                               ? kv["ro.build.version.release"] : "";
      ins.fingerprint = kv.count("ro.build.fingerprint") ? kv["ro.build.fingerprint"] : "";
    }
    // system.img case: API level resolved during import (ext4 walk)
    ins.ok = true;
    return ins;
  }

  // bare image
  FileBlockSource fbs;
  std::string err;
  if (!fbs.open(srcPath, &err)) { ins.err = err; return ins; }
  SparseBlockSource sbs;
  if (!sbs.open(&fbs, &err)) { ins.err = err; return ins; }
  Ext4Reader fs;
  if (!fs.open(&sbs, &err)) { ins.err = err; return ins; }
  ins.hasSystemImage = true;
  ins.ok = true;
  ins.approxBytes = sbs.size();
  return ins;
}

// ---------------------------------------------------------------------------
// import (dispatch)
// ---------------------------------------------------------------------------
bool RootfsManager::importTo(const std::string& srcPath,
                             const std::string& containerRoot,
                             const std::function<void(const ImportProgress&)>& progress,
                             std::string* err) {
  Inspection ins = inspect(srcPath);
  if (!ins.ok) { *err = ins.err; return false; }
  if (!isDirPath(containerRoot)) { *err = "container root missing"; return false; }
  ImportProgress p;
  p.phase = "inspect";
  p.fraction = 0.02f;
  if (progress) progress(p);
  bool ok = ins.isZip ? importFromZip(srcPath, containerRoot, progress, err)
                      : importFromImage(srcPath, containerRoot, progress, err);
  if (!ok) return false;
  p.phase = "finalize";
  p.fraction = 1.0f;
  if (progress) progress(p);
  return true;
}

// ---------------------------------------------------------------------------
// shared: walk an opened ext4 into the pack writer
// ---------------------------------------------------------------------------
namespace {

struct WalkCounts {
  uint32_t files = 0, dirs = 0, symlinks = 0;
  uint64_t bytes = 0;
};

// Pre-scan: count entries + bytes so progress is meaningful.
bool prescanExt4(Ext4Reader* fs, uint32_t ino, const std::string& prefix,
                 WalkCounts* c, std::string* err) {
  Ext4Reader::Stat st;
  if (!fs->statInode(ino, &st, err)) return false;
  std::vector<Ext4Reader::DirEntry> des;
  if (!fs->listDirInode(ino, &des, err)) return false;
  for (const auto& de : des) {
    Ext4Reader::Stat cs;
    if (!fs->statInode(de.inode, &cs, err)) return false;
    if (cs.isDir()) {
      c->dirs++;
      if (!prescanExt4(fs, de.inode, prefix + de.name + "/", c, err)) return false;
    } else if (cs.isSymlink()) {
      c->symlinks++;
    } else if (cs.isReg()) {
      c->files++;
      c->bytes += cs.size;
    }
  }
  return true;
}

// Real walk: add everything under `ino` with pack key prefix `prefix`.
bool walkExt4Into(Ext4Reader* fs, uint32_t ino, const std::string& prefix,
                  PackWriter* w, WalkCounts* c, uint64_t* bytesDone,
                  const std::function<void(const ImportProgress&)>& progress,
                  std::string* err) {
  std::vector<Ext4Reader::DirEntry> des;
  if (!fs->listDirInode(ino, &des, err)) return false;
  for (const auto& de : des) {
    Ext4Reader::Stat st;
    if (!fs->statInode(de.inode, &st, err)) return false;
    std::string key = prefix + de.name;
    if (st.isDir()) {
      w->addDir(key, st.mode & 0777, st.uid, st.gid);
      c->dirs++;
      if (!walkExt4Into(fs, de.inode, key + "/", w, c, bytesDone, progress, err))
        return false;
    } else if (st.isSymlink()) {
      std::string target = fs->readSymlinkInode(de.inode, st, err);
      if (target.empty() && !err->empty()) return false;
      w->addSymlink(key, st.uid, st.gid, target);
      c->symlinks++;
    } else if (st.isReg()) {
      uint8_t magic[4] = {};
      bool elf = false;
      if (st.size >= 4) {
        ssize_t r = fs->readRangeInode(de.inode, 0, 4, magic, err);
        if (r < 0) return false;
        elf = r == 4 && looksElf(magic);
      }
      uint32_t inoCopy = de.inode;
      w->addFile(key, st.mode & 07777, st.uid, st.gid, st.size, elf,
                 [fs, inoCopy](uint64_t off, size_t n,
                               const std::function<bool(const uint8_t*, size_t)>& sink) {
                   std::string e2;
                   std::vector<uint8_t> buf(512 * 1024);
                   uint64_t o = off;
                   size_t left = n;
                   while (left > 0) {
                     size_t want = std::min<size_t>(left, buf.size());
                     ssize_t r = fs->readRangeInode(inoCopy, o, want, buf.data(), &e2);
                     if (r <= 0) return false;
                     if (!sink(buf.data(), (size_t)r)) return false;
                     o += (uint64_t)r;
                     left -= (size_t)r;
                   }
                   return true;
                 });
      c->files++;
      c->bytes += st.size;
    }
  }
  return true;
}

// Locate the dir that maps to linux /system on an ext4 image root, plus its
// build.prop. Returns the inode (0 = not found).
uint32_t findSystemBase(Ext4Reader* fs, std::string* buildPropOut,
                        std::string* err) {
  static const char* cands[] = {
      "system",                  // SAR: image root has system/ (phh / GSI 10+)
      "",                        // non-SAR: image root IS /system (7.1/8.1)
      "system/system",           // double-system variants
  };
  for (const char* cp : cands) {
    std::string c = cp;
    Ext4Reader::Stat st;
    std::string bp = c.empty() ? "build.prop" : c + "/build.prop";
    if (fs->lookupPath(bp, &st, err) && st.isReg()) {
      std::string text;
      text.resize((size_t)st.size);
      ssize_t r = fs->readRangeInode(st.inode, 0, text.size(), (uint8_t*)text.data(), err);
      if (r != (ssize_t)st.size) { *err = "build.prop read"; return 0; }
      *buildPropOut = text;
      if (c.empty()) return 2;   // root
      Ext4Reader::Stat bs;
      if (fs->lookupPath(c, &bs, err) && bs.isDir()) return bs.inode;
      return 0;
    }
    if (!err->empty()) err->clear();
  }
  *err = "no build.prop found (not an Android system image?)";
  return 0;
}

bool writeManifest(const std::string& path, const RootfsStatus& st) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return false;
  fprintf(f,
      "{\n"
      "  \"profileTag\": \"%s\",\n"
      "  \"apiLevel\": %d,\n"
      "  \"androidVersion\": \"%s\",\n"
      "  \"fingerprint\": \"%s\",\n"
      "  \"packPath\": \"%s\",\n"
      "  \"packBytes\": %llu,\n"
      "  \"entryCount\": %u,\n"
      "  \"fileCount\": %u,\n"
      "  \"dirCount\": %u,\n"
      "  \"symlinkCount\": %u,\n"
      "  \"importedAtUtc\": \"%s\"\n"
      "}\n",
      jsonEscape(st.profileTag).c_str(), st.apiLevel,
      jsonEscape(st.androidVersion).c_str(), jsonEscape(st.fingerprint).c_str(),
      jsonEscape(st.packPath).c_str(), (unsigned long long)st.packBytes,
      st.entryCount, st.fileCount, st.dirCount, st.symlinkCount,
      jsonEscape(st.importedAtUtc).c_str());
  fclose(f);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// import: bare .img
// ---------------------------------------------------------------------------
bool RootfsManager::importFromImage(const std::string& srcPath,
                                    const std::string& containerRoot,
                                    const std::function<void(const ImportProgress&)>& progress,
                                    std::string* err) {
  FileBlockSource fbs;
  if (!fbs.open(srcPath, err)) return false;
  SparseBlockSource sbs;
  if (!sbs.open(&fbs, err)) return false;
  Ext4Reader fs;
  if (!fs.open(&sbs, err)) return false;

  std::string buildProp;
  uint32_t base = findSystemBase(&fs, &buildProp, err);
  if (base == 0) return false;
  auto kv = parseBuildProp(buildProp);
  int api = kv.count("ro.build.version.sdk") ? atoi(kv["ro.build.version.sdk"].c_str()) : 0;
  std::string tag = tagForApiLevel(api);

  WalkCounts c;
  if (!prescanExt4(&fs, base, "", &c, err)) return false;
  ImportProgress p;
  p.phase = "scan";
  p.fraction = 0.05f;
  p.bytesTotal = c.bytes;
  p.entriesTotal = c.files + c.dirs + c.symlinks;
  if (progress) progress(p);

  std::string outPath = containerRoot + "/rootfs-" + tag + ".aotpack";
  PackWriter w;
  if (!w.begin(outPath, err)) return false;
  WalkCounts wc;
  uint64_t bytesDone = 0;
  if (!walkExt4Into(&fs, base, "system/", &w, &wc, &bytesDone,
                    [&](const ImportProgress&) {}, err)) {
    w.abort();
    return false;
  }
  p.phase = "write";
  if (!w.finish([&](uint64_t done, uint64_t total) {
        ImportProgress q;
        q.phase = "write";
        q.bytesDone = done;
        q.bytesTotal = total;
        q.fraction = 0.1f + 0.85f * (total ? (float)done / (float)total : 1.f);
        if (progress) progress(q);
      }, err)) {
    w.abort();
    return false;
  }

  RootfsStatus st;
  st.installed = true;
  st.packPath = outPath;
  st.profileTag = tag;
  st.apiLevel = api;
  st.androidVersion = kv.count("ro.build.version.release") ? kv["ro.build.version.release"] : "";
  st.fingerprint = kv.count("ro.build.fingerprint") ? kv["ro.build.fingerprint"] : "";
  st.importedAtUtc = nowUtcIso();
  st.packBytes = (uint64_t)w.totalBytes();
  st.entryCount = w.entryCount();
  st.fileCount = wc.files;
  st.dirCount = wc.dirs;
  st.symlinkCount = wc.symlinks;
  if (!writeManifest(manifestPath(containerRoot), st)) {
    *err = "manifest write";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// import: .zip (GSI)
// ---------------------------------------------------------------------------
bool RootfsManager::importFromZip(const std::string& srcPath,
                                  const std::string& containerRoot,
                                  const std::function<void(const ImportProgress&)>& progress,
                                  std::string* err) {
  ZipReader z;
  if (!z.open(srcPath, err)) return false;

  const ZipReader::Entry* sysImg = nullptr;
  const ZipReader::Entry* venImg = nullptr;
  for (const auto& e : z.entries()) {
    if (e.isDir) continue;
    std::string n = e.name;
    while (!n.empty() && n.front() == '/') n.erase(n.begin());
    if ((n == "system.img" || n == "SYSTEM.img") && !sysImg) sysImg = &e;
    if ((n == "vendor.img" || n == "VENDOR.img") && !venImg) venImg = &e;
  }

  // ---- Case A: system.img inside the zip ----
  if (sysImg) {
    FileBlockSource tmpFile;
    SparseBlockSource sbs;
    ZipStoredBlockSource zsrc;   // function-scope lifetime (chain root)
    bool haveTmp = false;
    BlockSource* base = nullptr;
    if (sysImg->stored()) {
      zsrc.set(&z, sysImg);
      base = &zsrc;
      if (!sbs.open(base, err)) return false;
    } else {
      std::string tmp = containerRoot + "/rootfs-import-tmp.img";
      ImportProgress p;
      p.phase = "unpack";
      p.fraction = 0.04f;
      if (progress) progress(p);
      if (!z.extractToFile(*sysImg, tmp, err)) return false;
      haveTmp = true;
      if (!tmpFile.open(tmp, err)) return false;
      base = &tmpFile;
      if (!sbs.open(base, err)) return false;
    }
    Ext4Reader fs;
    if (!fs.open(&sbs, err)) {
      if (haveTmp) removeTree(containerRoot + "/rootfs-import-tmp.img");
      return false;
    }
    std::string buildProp;
    uint32_t sysBase = findSystemBase(&fs, &buildProp, err);
    if (sysBase == 0) {
      if (haveTmp) removeTree(containerRoot + "/rootfs-import-tmp.img");
      return false;
    }
    auto kv = parseBuildProp(buildProp);
    int api = kv.count("ro.build.version.sdk")
                  ? atoi(kv["ro.build.version.sdk"].c_str()) : 0;
    std::string tag = tagForApiLevel(api);

    WalkCounts c;
    if (!prescanExt4(&fs, sysBase, "", &c, err)) {
      if (haveTmp) removeTree(containerRoot + "/rootfs-import-tmp.img");
      return false;
    }
    ImportProgress p;
    p.phase = "scan";
    p.fraction = 0.08f;
    p.bytesTotal = c.bytes;
    p.entriesTotal = c.files + c.dirs + c.symlinks;
    if (progress) progress(p);

    std::string outPath = containerRoot + "/rootfs-" + tag + ".aotpack";
    PackWriter w;
    if (!w.begin(outPath, err)) {
      if (haveTmp) removeTree(containerRoot + "/rootfs-import-tmp.img");
      return false;
    }
    WalkCounts wc;
    uint64_t bytesDone = 0;
    if (!walkExt4Into(&fs, sysBase, "system/", &w, &wc, &bytesDone,
                      [](const ImportProgress&) {}, err)) {
      w.abort();
      if (haveTmp) removeTree(containerRoot + "/rootfs-import-tmp.img");
      return false;
    }
    // SAR images keep apex/ at the image root: import alongside
    Ext4Reader::Stat apexSt;
    if (fs.lookupPath("apex", &apexSt, err) && apexSt.isDir()) {
      WalkCounts ac;
      if (!walkExt4Into(&fs, apexSt.inode, "apex/", &w, &ac, &bytesDone,
                        [](const ImportProgress&) {}, err)) {
        w.abort();
        if (haveTmp) removeTree(containerRoot + "/rootfs-import-tmp.img");
        return false;
      }
      wc.files += ac.files; wc.dirs += ac.dirs; wc.symlinks += ac.symlinks;
    }
    if (!err->empty()) err->clear();
    if (!w.finish([&](uint64_t done, uint64_t total) {
          ImportProgress q;
          q.phase = "write";
          q.bytesDone = done;
          q.bytesTotal = total;
          q.fraction = 0.1f + 0.85f * (total ? (float)done / (float)total : 1.f);
          if (progress) progress(q);
        }, err)) {
      w.abort();
      if (haveTmp) removeTree(containerRoot + "/rootfs-import-tmp.img");
      return false;
    }
    if (haveTmp) removeTree(containerRoot + "/rootfs-import-tmp.img");

    RootfsStatus st;
    st.installed = true;
    st.packPath = outPath;
    st.profileTag = tag;
    st.apiLevel = api;
    st.androidVersion = kv.count("ro.build.version.release")
                            ? kv["ro.build.version.release"] : "";
    st.fingerprint = kv.count("ro.build.fingerprint") ? kv["ro.build.fingerprint"] : "";
    st.importedAtUtc = nowUtcIso();
    st.packBytes = (uint64_t)w.totalBytes();
    st.entryCount = w.entryCount();
    st.fileCount = wc.files;
    st.dirCount = wc.dirs;
    st.symlinkCount = wc.symlinks;
    if (!writeManifest(manifestPath(containerRoot), st)) {
      *err = "manifest write";
      return false;
    }
    return true;
  }

  // ---- Case B: unpacked system/ tree inside the zip ----
  std::string sysPrefix;   // "system/" or "system/system/"
  for (const char* cand : {"system/system/", "system/"}) {
    std::string bp = std::string(cand) + "build.prop";
    if (z.find(bp)) { sysPrefix = cand; break; }
  }
  if (sysPrefix.empty()) { *err = "no system/build.prop in zip"; return false; }

  std::string buildProp;
  if (const ZipReader::Entry* bp = z.find(sysPrefix + "build.prop")) {
    z.extract(*bp, [&](const uint8_t* p, size_t n) {
      buildProp.append((const char*)p, n);
      return true;
    }, err);
  }
  auto kv = parseBuildProp(buildProp);
  int api = kv.count("ro.build.version.sdk") ? atoi(kv["ro.build.version.sdk"].c_str()) : 0;
  std::string tag = tagForApiLevel(api);

  uint64_t total = 0;
  uint32_t nFiles = 0, nDirs = 0, nLinks = 0;
  std::vector<const ZipReader::Entry*> sel;
  for (const auto& e : z.entries()) {
    std::string n = e.name;
    while (!n.empty() && n.front() == '/') n.erase(n.begin());
    if (n.rfind(sysPrefix, 0) != 0 && n.rfind("vendor/", 0) != 0) continue;
    if (n == sysPrefix + "build.prop" || n.rfind(sysPrefix, 0) == 0 ||
        n.rfind("vendor/", 0) == 0) {
      sel.push_back(&e);
      if (e.isDir) nDirs++;
      else if (e.isSymlink) nLinks++;
      else { nFiles++; total += e.uncompressedSize; }
    }
  }
  if (sel.empty()) { *err = "nothing to import"; return false; }

  ImportProgress p;
  p.phase = "scan";
  p.fraction = 0.08f;
  p.bytesTotal = total;
  p.entriesTotal = nFiles + nDirs + nLinks;
  if (progress) progress(p);

  std::string outPath = containerRoot + "/rootfs-" + tag + ".aotpack";
  PackWriter w;
  if (!w.begin(outPath, err)) return false;
  for (const ZipReader::Entry* e : sel) {
    std::string n = e->name;
    while (!n.empty() && n.front() == '/') n.erase(n.begin());
    std::string key = (n.rfind(sysPrefix, 0) == 0)
                          ? "system/" + n.substr(sysPrefix.size())
                          : n;   // vendor/... stays vendor/...
    if (e->isDir) {
      w.addDir(key, e->unixMode & 0777, 0, 0);
    } else if (e->isSymlink) {
      std::string target;
      target.resize((size_t)e->uncompressedSize);
      if (!e->uncompressedSize ||
          z.extract(*e, [&](const uint8_t* p, size_t sz) {
            memcpy((void*)target.data(), p, sz);
            return true;
          }, err)) {
        w.addSymlink(key, 0, 0, target);
      }
    } else {
      // exec heuristic: x-bit or ELF magic (cheap peek for stored entries)
      bool exec = (e->unixMode & 0111) != 0;
      if (!exec && e->stored() && e->uncompressedSize >= 4) {
        uint8_t m[4] = {};
        if (z.storedRead(*e, 0, 4, m) == 4 && looksElf(m)) exec = true;
      }
      const ZipReader::Entry* ep = e;
      w.addFile(key, e->unixMode & 07777 ? e->unixMode & 07777 : 0644, 0, 0,
                e->uncompressedSize, exec,
                [&z, ep](uint64_t, size_t,
                         const std::function<bool(const uint8_t*, size_t)>& sink) {
                  std::string e2;
                  return z.extract(*ep, sink, &e2);
                });
    }
  }
  if (!w.finish([&](uint64_t done, uint64_t t) {
        ImportProgress q;
        q.phase = "write";
        q.bytesDone = done;
        q.bytesTotal = t;
        q.fraction = 0.1f + 0.85f * (t ? (float)done / (float)t : 1.f);
        if (progress) progress(q);
      }, err)) {
    w.abort();
    return false;
  }

  RootfsStatus st;
  st.installed = true;
  st.packPath = outPath;
  st.profileTag = tag;
  st.apiLevel = api;
  st.androidVersion = kv.count("ro.build.version.release")
                          ? kv["ro.build.version.release"] : "";
  st.fingerprint = kv.count("ro.build.fingerprint") ? kv["ro.build.fingerprint"] : "";
  st.importedAtUtc = nowUtcIso();
  st.packBytes = (uint64_t)w.totalBytes();
  st.entryCount = w.entryCount();
  st.fileCount = nFiles;
  st.dirCount = nDirs;
  st.symlinkCount = nLinks;
  if (!writeManifest(manifestPath(containerRoot), st)) {
    *err = "manifest write";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// status / remove / resolve
// ---------------------------------------------------------------------------
RootfsStatus RootfsManager::status(const std::string& containerRoot) {
  RootfsStatus st;
  std::string tag;
  std::string pack = resolvePackPath(containerRoot, "", &tag);
  if (pack.empty()) return st;
  st.installed = true;
  st.packPath = pack;
  st.profileTag = tag;
  st.apiLevel = apiForTag(tag);
  struct stat s {};
  if (::stat(pack.c_str(), &s) == 0) st.packBytes = (uint64_t)s.st_size;
  // manifest fields (if present)
  FILE* f = fopen(manifestPath(containerRoot).c_str(), "r");
  if (f) {
    std::string text;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
    fclose(f);
    std::string v = jsonField(text, "androidVersion");
    if (!v.empty()) st.androidVersion = v;
    v = jsonField(text, "fingerprint");
    if (!v.empty()) st.fingerprint = v;
    v = jsonField(text, "importedAtUtc");
    if (!v.empty()) st.importedAtUtc = v;
    v = jsonField(text, "apiLevel");
    if (!v.empty()) st.apiLevel = atoi(v.c_str());
    v = jsonField(text, "fileCount");
    if (!v.empty()) st.fileCount = (uint32_t)atoi(v.c_str());
    v = jsonField(text, "entryCount");
    if (!v.empty()) st.entryCount = (uint32_t)atoi(v.c_str());
    v = jsonField(text, "dirCount");
    if (!v.empty()) st.dirCount = (uint32_t)atoi(v.c_str());
    v = jsonField(text, "symlinkCount");
    if (!v.empty()) st.symlinkCount = (uint32_t)atoi(v.c_str());
  }
  return st;
}

bool RootfsManager::remove(const std::string& containerRoot, std::string* err) {
  (void)err;
  bool any = false;
  for (const auto& n : listDirNames(containerRoot)) {
    if (n.rfind("rootfs-", 0) == 0 &&
        (n.find(".aotpack") != std::string::npos ||
         n.find(".aotpack.tmp") != std::string::npos)) {
      removeTree(containerRoot + "/" + n);
      any = true;
    }
  }
  removeTree(manifestPath(containerRoot));
  removeTree(containerRoot + "/rootfs-import-tmp.img");
  return any;
}

std::string RootfsManager::resolvePackPath(const std::string& containerRoot,
                                           const std::string& wantTag,
                                           std::string* actualTag) {
  std::string best;
  struct stat bestSt {};
  for (const auto& n : listDirNames(containerRoot)) {
    if (n.rfind("rootfs-", 0) != 0) continue;
    if (n.size() < 8 + 8 || n.substr(n.size() - 8) != ".aotpack") continue;
    std::string tag = n.substr(7, n.size() - 7 - 8);   // strip rootfs- and .aotpack
    std::string full = containerRoot + "/" + n;
    struct stat s {};
    if (::stat(full.c_str(), &s) != 0 || s.st_size < 64) continue;
    bool better = false;
    if (wantTag.empty()) {
      better = best.empty() || s.st_mtime > bestSt.st_mtime;
    } else {
      if (tag == wantTag) better = best.empty() || s.st_mtime > bestSt.st_mtime;
      else if (best.empty()) better = true;   // fallback candidate
    }
    if (better) {
      best = full;
      bestSt = s;
      if (actualTag) *actualTag = tag;
    }
  }
  return best;
}

}  // namespace aotdroid
