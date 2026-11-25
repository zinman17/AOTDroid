// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/tools/import_main.cpp
//  aotdroid-import: GSI -> .aotpack converter CLI.
//    aotdroid-import inspect <file.zip|file.img>
//    aotdroid-import import  <file.zip|file.img> <out-dir>   (writes
//                          out-dir/rootfs-<tag>.aotpack + rootfs.json)
//    aotdroid-import verify  <pack.aotpack> [path-in-pack]   (open + lookup)
//  Used by CI (real mkfs.ext4 + sparse + stored/deflate zips) and by power
//  users who prefer importing on their desktop.
// ============================================================================
#include "../rootfs/rootfs_manager.h"
#include "../rootfs/zip_reader.h"
#include "../runtime/vfs.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace aotdroid;

static int cmdInspect(const char* file) {
  auto ins = RootfsManager::get().inspect(file);
  if (!ins.ok) {
    fprintf(stderr, "inspect FAILED: %s\n", ins.err.c_str());
    return 1;
  }
  printf("isZip             : %d\n", ins.isZip);
  printf("hasSystemImage    : %d\n", ins.hasSystemImage);
  printf("hasUnpackedSystem : %d\n", ins.hasUnpackedSystem);
  printf("hasVendor         : %d\n", ins.hasVendor);
  printf("apiLevel          : %d\n", ins.apiLevel);
  printf("androidVersion    : %s\n", ins.androidVersion.c_str());
  printf("fingerprint       : %s\n", ins.fingerprint.c_str());
  printf("approxBytes       : %llu\n", (unsigned long long)ins.approxBytes);
  printf("INSPECT OK\n");
  return 0;
}

static int cmdImport(const char* file, const char* outDir) {
  std::string err;
  bool ok = RootfsManager::get().importTo(file, outDir,
      [](const ImportProgress& p) {
        fprintf(stderr, "\r[%-8s] %5.1f%%  %llu/%llu bytes   ",
                p.phase.c_str(), p.fraction * 100.f,
                (unsigned long long)p.bytesDone,
                (unsigned long long)p.bytesTotal);
      }, &err);
  fprintf(stderr, "\n");
  if (!ok) {
    fprintf(stderr, "import FAILED: %s\n", err.c_str());
    return 1;
  }
  RootfsStatus st = RootfsManager::get().status(outDir);
  printf("pack        : %s\n", st.packPath.c_str());
  printf("tag         : %s (API %d, Android %s)\n", st.profileTag.c_str(),
         st.apiLevel, st.androidVersion.c_str());
  printf("entries     : %u (files %u, dirs %u, symlinks %u)\n",
         st.entryCount, st.fileCount, st.dirCount, st.symlinkCount);
  printf("packBytes   : %llu\n", (unsigned long long)st.packBytes);
  printf("IMPORT OK\n");
  return 0;
}

static int cmdVerify(const char* pack, const char* probePath) {
  std::string err;
  if (!rootfsPack().open(pack, &err)) {
    fprintf(stderr, "verify FAILED: %s\n", err.c_str());
    return 1;
  }
  printf("header      : %u entries, blobOff %llu\n",
         rootfsPack().header().entryCount,
         (unsigned long long)rootfsPack().header().blobOff);
  // dump root listing
  auto root = rootfsPack().listDir("system");
  printf("system/     : %zu entries:", root.size());
  int shown = 0;
  for (auto& n : root) {
    if (n == "." || n == "..") continue;
    printf(" %s", n.c_str());
    if (++shown >= 12) { printf(" ..."); break; }
  }
  printf("\n");
  if (probePath) {
    PackEntry pe;
    const uint8_t* data = nullptr;
    if (!rootfsPack().lookup(probePath, &pe, &data)) {
      fprintf(stderr, "verify FAILED: lookup(%s)\n", probePath);
      return 1;
    }
    printf("%-28s: mode %07o size %llu flags %u\n", probePath, pe.mode,
           (unsigned long long)pe.size, pe.flags);
    if ((pe.flags & kPackFlagSymlink) == 0 && pe.size < 4096) {
      printf("  first bytes: %.64s\n", (const char*)data);
    } else if (pe.flags & kPackFlagSymlink) {
      printf("  -> target  : %.*s\n", (int)pe.size, (const char*)data);
    }
  }
  // symlink resolution proof if present
  PackEntry pe;
  const uint8_t* data = nullptr;
  if (rootfsPack().lookupRaw("system/bin/sh", &pe, &data) &&
      (pe.flags & kPackFlagSymlink)) {
    PackEntry tgt;
    if (rootfsPack().lookup("system/bin/sh", &tgt, nullptr))
      printf("symlink resolve: system/bin/sh -> %.*s (ok, %llu bytes)\n",
             (int)pe.size, (const char*)data, (unsigned long long)tgt.size);
  }
  printf("VERIFY OK\n");
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: %s inspect <file>\n"
            "       %s import <file> <out-dir>\n"
            "       %s verify <pack.aotpack> [path]\n",
            argv[0], argv[0], argv[0]);
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd == "inspect") return cmdInspect(argv[2]);
  if (cmd == "import" && argc >= 4) return cmdImport(argv[2], argv[3]);
  if (cmd == "verify" && argc >= 3)
    return cmdVerify(argv[2], argc >= 4 ? argv[3] : nullptr);
  fprintf(stderr, "bad args\n");
  return 2;
}
