// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/linker/bionic_linker.h   (MODULE 4)
//  Custom ARM64 ELF64 dynamic linker (linker64 replacement) + the runtime
//  image registry for Mach-O-converted binaries.
//
//  Two load paths:
//   1. PRODUCTION (iOS, signed): each AOSP ELF was converted offline into a
//      signed dylib by aot_rewriter. The registry dlopen()s it, then:
//        - parses __AOTDATA,__aotmeta
//        - fills __AOTDATA,__aotgot slots by resolving (soname, symbol)
//          against already-loaded images (Android namespace rules)
//        - applies runtime data fixups (GLOB_DAT/JUMP_SLOT/TPREL/TLSDESC)
//          at getsectiondata()-derived addresses
//        - runs .init_array before returning
//   2. DEV (jailbroken / macOS simulator / unit tests): full ELF64 loader
//      with RELA + packed ANDROID_RELA + RELR decoding, DT_NEEDED walk,
//      SysV/GNU hash symbol lookup and lazy-binding-free eager binding.
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"
#include "../aot/aot_rewriter.h"

namespace aotdroid {

struct LoadedElfImage {
  std::string soname;
  uint8_t* base = nullptr;          // load bias applied
  size_t size = 0;
  std::vector<uint64_t> needed;     // DT_NEEDED string offsets (strtab-based)
  // dynamic table views
  const Dyn64* dyn = nullptr;
  size_t dynCount = 0;
  const char* strtab = nullptr;
  const Sym64* symtab = nullptr;
  size_t symCount = 0;
};

class BionicLinker {
 public:
  static BionicLinker& get();

  // namespace-scoped library loading
  int loadElf(const std::string& linuxPath, const std::string& soname);
  const LoadedElfImage* findElf(const std::string& soname) const;
  uint64_t resolveSymbol(const std::string& soname, const std::string& name) const;

  // Production path: register a converted (Mach-O dylib) image.
  bool attachConverted(const std::string& soname, void* machHeader);
  void* dlopenConverted(const std::string& soname);   // returns handle for dlsym

  // heaps trim hook (memory pressure)
  void trimAllRuntimes(int level);

  size_t imageCount() const { return images_.size(); }

 private:
  BionicLinker() = default;
  bool applyRelocations(LoadedElfImage* img);
  bool applyElfRela(LoadedElfImage* img, const Rela64* relas, size_t count);
  bool applyAndroidPacked(LoadedElfImage* img, const uint8_t* data, size_t len);
  bool applyRelr(LoadedElfImage* img, const uint64_t* entries, size_t count);
  const Sym64* elfHashLookup(const LoadedElfImage& img, const char* name) const;
  const Sym64* gnuHashLookup(const LoadedElfImage& img, const char* name) const;

  struct Image {
    std::string soname;
    bool elf = true;
    LoadedElfImage elfView;
    void* machoHandle = nullptr;    // dlopen handle (converted path)
    const void* machHeader = nullptr;
    std::vector<void*> initArray;
    bool initialized = false;
  };
  std::vector<std::unique_ptr<Image>> images_;
  mutable std::mutex mu_;
};

// helper used by container_boot.cpp
void linkerTrimAllRuntimes(int level);

// SLEB128 reader for packed relocations (bionic format).
struct Sleb128Reader {
  const uint8_t* p;
  explicit Sleb128Reader(const uint8_t* data) : p(data) {}
  int64_t read() {
    int64_t result = 0;
    unsigned shift = 0, byte;
    do {
      byte = *p++;
      result |= (int64_t)(byte & 0x7f) << shift;
      shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) result |= -((int64_t)1 << shift);
    return result;
  }
};

}  // namespace aotdroid
