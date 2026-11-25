// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/aot/macho_emit.h
//  Minimal but conformant Mach-O (MH_OBJECT, arm64) emitter.
//
//  Purpose: the offline AOT pipeline converts each Bionic ELF64 (.so / .oat /
//  executable) into a Mach-O object that Apple's ld64 links into the signed
//  app binary. iOS forbids mapping unsigned executable pages, so *all* AOT
//  artifacts must ride inside the code-signed image; this emitter is how.
//
//  Binding model (see docs/ARCHITECTURE.md "Relocation funnel"):
//   - internal refs      -> static Mach-O relocs, resolved by ld64
//   - external lib refs  -> 8/16-byte slots in __AOTDATA,__aotgot; ld64 gives
//                           them final addresses; the runtime Bionic linker
//                           fills the slots before running any init code
//   - runtime data fixes -> serialized records in __AOTDATA,__aotmeta consumed
//                           by bionic_linker.cpp via getsectiondata()
// ============================================================================
#pragma once
#include "../aot/elf_oat_abi.h"

namespace aotdroid::macho {

struct SectionSpec {
  std::string segName;    // "__TEXT", "__DATA", "__AOTDATA"
  std::string sectName;   // "__text", "__data", "__aotgot", "__aotmeta" ...
  std::vector<uint8_t> data;
  uint64_t vaddr = 0;     // ignored: the emitter assigns packed, aligned,
                          // non-overlapping addresses (ld64 requirement)
  uint32_t align = 3;     // 2^align
  uint32_t flags = 0;     // Mach-O section type/attrs
  bool zerofill = false;  // S_ZEROFILL: size only, no file bytes
  uint64_t zsize = 0;
};

struct SymbolSpec {
  std::string name;
  uint32_t sectIdx = 0;        // 1-based; 0 = undefined
  uint64_t value = 0;          // offset within section (or absolute for undef marker)
  bool external = true;
  bool globalDef = false;      // defined + external
};

// A single translated relocation.
struct FixupSpec {
  enum Kind : uint32_t {
    kUnsigned8 = 0,   // ARM64_RELOC_UNSIGNED, 8 bytes, section-relative (internal)
    kPage21     = 1,  // ARM64_RELOC_PAGE21 vs symbol (addend pair auto-emitted)
    kPage12     = 2,  // ARM64_RELOC_PAGEOFF12 vs symbol
    kBr26       = 3,  // ARM64_RELOC_BRANCH26 vs symbol
  } kind;
  uint32_t siteSection = 0;    // 1-based index into sections
  uint64_t siteOffset = 0;     // offset of the instruction/quad inside section
  uint32_t targetSym = 0;      // symbol index (0-based) for page/br/unsigned-extern
  uint32_t targetSection = 0;  // for kUnsigned8 with section target (0 = use sym)
  int64_t addend = 0;          // emitted as ARM64_RELOC_ADDEND pair when != 0
};

struct ObjectSpec {
  std::vector<SectionSpec> sections;
  std::vector<SymbolSpec> symbols;
  std::vector<FixupSpec> fixups;
};

// Serializes to a byte buffer. Returns false + err on inconsistency.
bool emit_object(const ObjectSpec& spec, std::vector<uint8_t>* out,
                 std::string* err);

// Section flag helpers (subset actually used).
constexpr uint32_t kSectRegular        = 0x00000000;
constexpr uint32_t kSectProgBitsExec   = 0x80000400;  // PURE_INSTR|SOME_INSTR
constexpr uint32_t kSectZerofill       = 0x00000001;
constexpr uint32_t kSectInitFuncPtrs   = 0x00000016;  // S_MOD_INIT_FUNC_POINTERS

}  // namespace aotdroid::macho
