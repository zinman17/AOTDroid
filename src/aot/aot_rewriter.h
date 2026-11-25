// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/aot/aot_rewriter.h   (MODULE 2)
//  Multi-version ELF64 / OAT AOT rewriter for Android 7.1 / 8.1 / 10.
//
//  Two outputs per input image:
//    1. rewritten ELF64 (dev / jailbroken / macOS-sim path) with:
//         - 16 KiB PT_LOAD re-layout (iPadOS page size)
//         - `svc #0` sites patched to branch at aotdroid_syscall_bridge
//         - `mrs/msr tpidr_el0` sites patched to the emulated Android TLS
//         - trampolines in an appended PT_AOTPATCH segment; out-of-range
//           sites become `brk #idx` trampoline traps caught by the in-process
//           Mach exception server (range-free fallback)
//    2. Mach-O object (production iOS path) that ld64 links into the signed
//       app binary, with __aotgot slots + __aotmeta for the runtime linker.
//
//  OAT header versions: 079/088 (Android 7.x), 124/131 (8.x), 170 (10).
// ============================================================================
#pragma once
#include "elf_oat_abi.h"
#include "macho_emit.h"

namespace aotdroid {

struct RewriteConfig {
  uint32_t targetPageSize = 16384;     // iPadOS page size
  bool emitElf = true;
  bool emitMachO = true;
  std::string sonameOverride;          // if empty, DT_SONAME is used
  // Static TLS layout assigned by the rootfs build planner: maps DSO soname
  // -> byte offset of its TLS block inside the emulated per-thread TP area.
  // Required so TLSLE/TLSIE sequences can be patched at build time.
  std::unordered_map<std::string, int64_t> tlsLayout;
};

struct RewriteStats {
  size_t inputBytes = 0, outputElfBytes = 0, outputMachoBytes = 0;
  uint32_t oatApiLevel = 0;
  char oatVersion[8] = {0};
  size_t syscallSites = 0, tlsReadSites = 0, tlsWriteSites = 0, tpidrroSites = 0;
  size_t veneersEmitted = 0, brkTrampolines = 0;
  size_t relocsTranslated = 0, runtimeFixups = 0, slotsEmitted = 0;
  size_t segmentsRelaid = 0;
  bool oatContainer = false;
};

class AotRewriter {
 public:
  static bool rewriteBuffer(const uint8_t* in, size_t inLen,
                            const RewriteConfig& cfg,
                            std::vector<uint8_t>* outElf,
                            std::vector<uint8_t>* outMacho,
                            RewriteStats* stats, std::string* err);
  static bool rewriteFile(const char* inPath, const char* outElfPath,
                          const char* outMachoPath, const RewriteConfig& cfg,
                          RewriteStats* stats, std::string* err);
};

// Serialized runtime-linker metadata (embedded as __AOTDATA,__aotmeta).
namespace aotmeta {
constexpr uint32_t kMagic = 0x414F5441;   // "AOTA"
constexpr uint32_t kVersion = 1;

#pragma pack(push, 1)
struct Header {
  uint32_t magic, version;
  uint32_t strTabOff, strTabLen;
  uint32_t slotOff, slotCount;
  uint32_t fixupOff, fixupCount;
  uint32_t sectOff, sectCount;
  uint32_t imageInfoOff;     // ImageInfo record
  uint32_t reserved[3];
};
struct ImageInfo {
  uint32_t sonameOff;         // string table offsets
  uint32_t neededOff;         // ':'-separated needed sonames
  uint32_t entrySect, finiSect, initArraySect, finiArraySect;
  uint64_t entryOff, initArrayOff, initArrayCount;
  uint64_t tlsSize, tlsAlign, tlsInitOff;   // PT_TLS mirror
  uint32_t flags;             // bit0 = oat container
  uint32_t oatApiLevel;
};
struct SlotRecord {
  uint32_t sonameOff, nameOff;
  uint16_t kind;              // 0 = symbol (GOT/GLOB_DAT/JUMP_SLOT), 1 = TLSDESC pair
  uint16_t reserved;
  uint64_t value0, value1;    // TLSDESC: resolver, offset (symbol kind: unused)
};
struct FixupRecord {
  uint32_t sectIdx;           // index into section table (1-based like Mach-O)
  uint64_t offset;
  uint32_t origReloc;         // original AArch64 reloc type
  int32_t slotIdx;            // >= 0: fix the slot; < 0: direct symbol write
  uint32_t symSonameOff, symNameOff;
  int64_t addend;
};
struct SectEntry { char seg[16]; char sect[16]; };
#pragma pack(pop)

// Builder used by the rewriter; reader lives in bionic_linker.cpp.
class Builder {
 public:
  uint32_t addString(const std::string& s);
  int32_t addSlot(const std::string& soname, const std::string& sym, uint16_t kind);
  void addFixup(uint32_t sectIdx, uint64_t off, uint32_t origReloc, int32_t slot,
                const std::string& soname, const std::string& sym, int64_t addend);
  std::vector<uint8_t> serialize(const ImageInfo& info,
                                 const std::vector<SectEntry>& sections) const;
  size_t slotCount() const { return slots_.size(); }
  size_t fixupCount() const { return fixups_.size(); }
 private:
  struct SlotRec { uint32_t so, nm; uint16_t kind; };
  struct FixRec { uint32_t sectIdx; uint64_t off; uint32_t reloc; int32_t slot;
                  uint32_t so, nm; int64_t addend; };
  std::string strs_;
  std::unordered_map<std::string, uint32_t> strIdx_;
  std::vector<SlotRec> slots_;
  std::vector<FixRec> fixups_;
};
}  // namespace aotmeta

}  // namespace aotdroid
