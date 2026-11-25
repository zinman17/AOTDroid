// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/aot/elf_oat_abi.h
//  Self-contained ELF64 / OAT / ARM64-encoding ABI definitions (module 2).
//
//  Deliberately does NOT include <elf.h> or <mach-o/loader.h>: the rewriter
//  must produce byte-identical output on Linux hosts (CI) and Darwin/iOS
//  (install-time path), and the two system headers disagree on packing and
//  availability. Every constant below is cross-checked by tests/ and by the
//  CI job `.github/workflows/build-ipa.yml` (job `rewrite-fixture`) against
//  a real assembler and a real cross-compiled .so.
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"

namespace aotdroid {

// ===========================================================================
//  ELF64 (little-endian, EM_AARCH64)
// ===========================================================================
static_assert(sizeof(unsigned long) == 8 || sizeof(void*) == 8, "LP64 required");

constexpr uint16_t kEM_AARCH64 = 183;
constexpr uint16_t kET_EXEC    = 2;
constexpr uint16_t kET_DYN     = 3;

constexpr uint32_t kPT_LOAD    = 1;
constexpr uint32_t kPT_DYNAMIC = 2;
constexpr uint32_t kPT_TLS     = 7;
constexpr uint32_t kPF_X       = 1, kPF_W = 2, kPF_R = 4;

constexpr uint32_t kSHT_PROGBITS = 1, kSHT_SYMTAB = 2, kSHT_STRTAB = 3,
                   kSHT_RELA     = 4, kSHT_NOBITS  = 8, kSHT_DYNSYM = 11,
                   kSHT_INIT_ARRAY = 14, kSHT_FINI_ARRAY = 15,
                   kSHT_PREINIT_ARRAY = 16, kSHT_GROUP = 17,
                   kSHT_SYMTAB_SHNDX = 18, kSHT_RELR = 19,
                   kSHT_ANDROID_RELA = 0x60000002,   // SHT_ANDROID_RELA
                   kSHT_ANDROID_REL  = 0x60000003;

#pragma pack(push, 1)
struct Ehdr64 {
  uint8_t  e_ident[16];      // 0x7f 'E' 'L' 'F', class=2 (64), data=1 (LSB)
  uint16_t e_type, e_machine;
  uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Phdr64 {
  uint32_t p_type, p_flags;
  uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct Sym64 {
  uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
  uint64_t st_value, st_size;
};
struct Rela64 { uint64_t r_offset; uint64_t r_info; int64_t r_addend; };
struct Dyn64  { int64_t d_tag; uint64_t d_val; };
#pragma pack(pop)
// Elf64_Shdr is 64 bytes WITH trailing alignment padding (spec size); it must
// NOT be pack(1).
struct Shdr64 {
  uint32_t sh_name, sh_type;
  uint64_t sh_flags;
  uint64_t sh_addr, sh_offset, sh_size;
  uint32_t sh_link, sh_info;
  uint64_t sh_addralign, sh_entsize;   // Elf64_Xword per the spec
};

static_assert(sizeof(Ehdr64) == 64, "ELF64 header must be 64 bytes");
static_assert(sizeof(Phdr64) == 56, "ELF64 phdr must be 56 bytes");
static_assert(sizeof(Shdr64) == 64, "ELF64 shdr must be 64 bytes");
static_assert(sizeof(Sym64) == 24, "ELF64 sym must be 24 bytes");
static_assert(sizeof(Rela64) == 24, "ELF64 rela must be 24 bytes");
static_assert(sizeof(Dyn64) == 16, "ELF64 dyn must be 16 bytes");

inline uint32_t rela_type(uint64_t r_info) { return (uint32_t)(r_info & 0xffffffffULL); }
inline uint32_t rela_sym(uint64_t r_info)  { return (uint32_t)(r_info >> 32); }
inline uint64_t rela_info(uint32_t sym, uint32_t type) { return ((uint64_t)sym << 32) | type; }

// AArch64 dynamic relocation types we translate (per AAELF64; numeric values
// below are cross-asserted against glibc's <elf.h> in tests/test_aot_rewriter.cpp
// when the host provides it). Anything outside this set makes the rewriter fail
// loudly at build time rather than producing a broken image.
enum AArch64Reloc : uint32_t {
  kR_AARCH64_ABS64               = 257,
  kR_AARCH64_ADR_PREL_LO21       = 274,
  kR_AARCH64_ADR_PREL_PG_HI21    = 275,
  kR_AARCH64_ADD_ABS_LO12_NC     = 277,
  kR_AARCH64_LDST8_ABS_LO12_NC   = 278,
  kR_AARCH64_CONDBR19            = 280,
  kR_AARCH64_JUMP26              = 282,
  kR_AARCH64_CALL26              = 283,
  kR_AARCH64_LDST16_ABS_LO12_NC  = 284,
  kR_AARCH64_LDST32_ABS_LO12_NC  = 285,
  kR_AARCH64_LDST64_ABS_LO12_NC  = 286,
  kR_AARCH64_LDST128_ABS_LO12_NC = 299,
  kR_AARCH64_ADR_GOT_PAGE        = 311,
  kR_AARCH64_LD64_GOT_LO12_NC    = 312,
  kR_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 = 541,
  kR_AARCH64_TLSLE_ADD_TPREL_HI12      = 549,
  kR_AARCH64_TLSLE_ADD_TPREL_LO12_NC   = 551,
  kR_AARCH64_TLSDESC_ADR_PAGE21  = 562,
  kR_AARCH64_TLSDESC_LD64_LO12   = 563,
  kR_AARCH64_TLSDESC_ADD_LO12    = 564,
  kR_AARCH64_TLSDESC_CALL        = 569,
  kR_AARCH64_GLOB_DAT            = 1025,
  kR_AARCH64_JUMP_SLOT           = 1026,
  kR_AARCH64_RELATIVE            = 1027,
  kR_AARCH64_TLS_TPREL64         = 1030,
  kR_AARCH64_TLSDESC             = 1031,
  kR_AARCH64_IRELATIVE           = 1032,
};

// Dynamic tags used by the rewriter / linker.
enum DynTag : int64_t {
  kDT_NULL = 0, kDT_NEEDED = 1, kDT_PLTRELSZ = 2, kDT_PLTGOT = 3,
  kDT_HASH = 4, kDT_STRTAB = 5, kDT_SYMTAB = 6, kDT_RELA = 7,
  kDT_RELASZ = 8, kDT_RELAENT = 9, kDT_STRSZ = 10, kDT_SYMENT = 11,
  kDT_INIT = 12, kDT_FINI = 13, kDT_SONAME = 14, kDT_RPATH = 15,
  kDT_REL = 17, kDT_RELSZ = 18, kDT_RELENT = 19, kDT_PLTREL = 20, kDT_JMPREL = 23,
  kDT_INIT_ARRAY = 25, kDT_FINI_ARRAY = 26, kDT_INIT_ARRAYSZ = 27,
  kDT_FINI_ARRAYSZ = 28, kDT_RUNPATH = 29, kDT_FLAGS = 30,
  kDT_PREINIT_ARRAY = 32, kDT_PREINIT_ARRAYSZ = 33,
  kDT_GNU_HASH = 0x6ffffef5, kDT_VERSYM = 0x6ffffff0,
  kDT_FLAGS_1 = 0x6ffffffb, kDT_VERNEED = 0x6ffffffe, kDT_VERNEEDNUM = 0x6fffffff,
  kDT_ANDROID_REL = 0x6000000d, kDT_ANDROID_RELSZ = 0x6000000e,
  kDT_ANDROID_RELA = 0x6000000f, kDT_ANDROID_RELASZ = 0x60000010,
  kDT_RELR = 0x6fffe000, kDT_RELRSZ = 0x6fffe001, kDT_RELRENT = 0x6fffe003,
};

// Android packed-relocation group flags (bionic linker, relocation_packer).
constexpr uint32_t kRELOCATION_GROUPED_BY_OFFSET_FLAG = 1;
constexpr uint32_t kRELOCATION_GROUPED_BY_INFO_FLAG   = 2;
constexpr uint32_t kRELOCATION_GROUP_HAS_ADDEND_FLAG  = 4;
constexpr uint32_t kRELOCATION_GROUPED_BY_ADDEND_FLAG = 8;

// ===========================================================================
//  OAT container ABI (ART). boot.oat / services.odex are ELF64 objects whose
//  ".oatdata" section begins with the OatHeader. Version table is taken from
//  art/runtime/oat.h of the corresponding AOSP tags:
//    android-7.0  -> 079      android-7.1.2 -> 088
//    android-8.0  -> 124      android-8.1.0 -> 131
//    android-9    -> 138      android-10    -> 170
// ===========================================================================
constexpr uint8_t kOatMagic[4] = {'o', 'a', 't', '\n'};   // 0x6f 61 74 0a
constexpr uint8_t kArtMagic[4] = {'a', 'r', 't', '\n'};   // boot.art images

struct OatVersionInfo { const char* version; uint32_t apiLevel; const char* release; };
inline const OatVersionInfo* oat_version_table() {
  static const OatVersionInfo kTable[] = {
    {"079", 24, "7.0"}, {"088", 25, "7.1.2"}, {"124", 26, "8.0"},
    {"131", 27, "8.1"}, {"138", 28, "9"},     {"170", 29, "10"},
    {nullptr, 0, nullptr},
  };
  return kTable;
}
// Returns API level for an OAT version string ("088" -> 25), or 0 if unknown.
inline uint32_t oat_version_to_api(const char v[5]) {
  for (auto* t = oat_version_table(); t->version; ++t)
    if (memcmp(v, t->version, 3) == 0) return t->apiLevel;
  return 0;
}

// art::InstructionSet values (art/libartbase/instruction_set.h).
constexpr uint32_t kArtInstructionSetArm64 = 3;

// Common prefix of OatHeader across 079..170: magic, version, checksum,
// instruction_set, dex_file_count, then a run of trampoline offsets. Only the
// prefix below is consumed by the rewriter; per-version trailing fields are
// validated by the CI job against the pinned AOSP tags.
#pragma pack(push, 1)
struct OatHeaderPrefix {
  uint8_t  magic[4];
  uint8_t  version[4];        // NUL-terminated ASCII, e.g. "088"
  uint32_t adler32_checksum;
  uint32_t instruction_set;
  uint32_t dex_file_count;
  uint32_t executable_offset;
  uint32_t jni_dlsym_lookup_offset;      // 079..131 (renamed in 170)
  uint32_t quick_generic_jni_trampoline_offset;
  uint32_t quick_imt_conflict_trampoline_offset;
  uint32_t quick_resolution_trampoline_offset;
  uint32_t quick_to_interpreter_bridge_offset;
};
static_assert(sizeof(OatHeaderPrefix) == 44, "OatHeader prefix layout");
#pragma pack(pop)

// ===========================================================================
//  ARM64 encoders. Every constant below was verified against the system
//  assembler (LLVM/GNU) -- see tests/test_aot_rewriter.cpp, which asserts
//  byte equality with independently assembled instructions.
// ===========================================================================
namespace arm64 {

constexpr uint32_t kSvc0             = 0xD4000001u;               // svc #0
constexpr uint32_t kMrsTpidrEl0      = 0xD53BD040u;               // mrs xN, tpidr_el0    (+ Rt)
constexpr uint32_t kMrsTpidrroEl0    = 0xD53BD060u;               // mrs xN, tpidrro_el0  (+ Rt)
constexpr uint32_t kMsrTpidrEl0      = 0xD51BD040u;               // msr tpidr_el0, xN    (+ Rt)
constexpr uint32_t kStpX16X17PreNeg16 = 0xA9BF47F0u;              // stp x16,x17,[sp,#-16]!
constexpr uint32_t kLdpX16X17Post16  = 0xA8C147F0u;               // ldp x16,x17,[sp],#16
constexpr uint32_t kBlrX16           = 0xD63F0200u;               // blr x16
constexpr uint32_t kBrX16            = 0xD61F0200u;               // br  x16
constexpr uint32_t kNop              = 0xD503201Fu;
constexpr uint32_t kAddSp152         = 0x910263FFu;   // add sp, sp, #152
constexpr uint32_t kBBase            = 0x14000000u;               // b  imm26
constexpr uint32_t kBlBase           = 0x94000000u;               // bl imm26

inline bool is_svc0(uint32_t w)            { return w == kSvc0; }
inline bool is_mrs_tpidr_el0(uint32_t w)   { return (w & 0xFFFFFFE0u) == kMrsTpidrEl0; }
inline bool is_mrs_tpidrro_el0(uint32_t w) { return (w & 0xFFFFFFE0u) == kMrsTpidrroEl0; }
inline bool is_msr_tpidr_el0(uint32_t w)   { return (w & 0xFFFFFFE0u) == kMsrTpidrEl0; }
inline uint32_t regfield(uint32_t w)       { return w & 31u; }

// b/bl with byte offset (signed, multiple of 4, +/-128 MiB).
inline uint32_t enc_b(int64_t byteOff) {
  return kBBase  | (uint32_t)((byteOff >> 2) & 0x03FFFFFFu);
}
inline uint32_t enc_bl(int64_t byteOff) {
  return kBlBase | (uint32_t)((byteOff >> 2) & 0x03FFFFFFu);
}
inline bool b_fits(int64_t byteOff) {
  int64_t i = byteOff >> 2;
  return i >= -(1 << 25) && i < (1 << 25);
}
// mov xD, xM  (orr xD, xzr, xM)
inline uint32_t enc_mov(uint32_t d, uint32_t m) {
  return 0xAA0003E0u | (m << 16) | d;
}
// ldr xT, [xN, #imm]  (unsigned offset, 64-bit: imm must be 8-aligned, < 32K)
inline uint32_t enc_ldr64_imm(uint32_t t, uint32_t n, uint32_t imm) {
  return 0xF9400000u | ((imm >> 3) << 10) | (n << 5) | t;
}
// str xT, [xN, #imm]
inline uint32_t enc_str64_imm(uint32_t t, uint32_t n, uint32_t imm) {
  return 0xF9000000u | ((imm >> 3) << 10) | (n << 5) | t;
}
// stp xA, xB, [sp, #imm]  (pre-index writeback when imm<0 is folded by caller)
inline uint32_t enc_stp64_pre(uint32_t a, uint32_t b, int32_t immBytes) {
  int32_t imm7 = immBytes / 8;
  return 0xA9800000u | ((uint32_t)(imm7 & 0x7F) << 15) | (b << 10) | (31u << 5) | a;
}
inline uint32_t enc_ldp64_post(uint32_t a, uint32_t b, int32_t immBytes) {
  int32_t imm7 = immBytes / 8;
  return 0xA8C00000u | ((uint32_t)(imm7 & 0x7F) << 15) | (b << 10) | (31u << 5) | a;
}

// TLS emulation bridge entry points (implemented in syscall_bridge.cpp).
// The rewriter emits `bl` to these; on device they resolve inside the signed
// app binary, so no unsigned code is ever executed.
constexpr const char* kSymSyscallBridge = "aotdroid_syscall_bridge";
constexpr const char* kSymGetAndroidTls = "aotdroid_get_android_tls_c";
constexpr const char* kSymSetAndroidTls = "aotdroid_set_android_tls_c";

}  // namespace arm64

// ===========================================================================
//  Mach-O 64 object ABI (emitter output, consumed by ld64 on the macOS CI).
// ===========================================================================
constexpr uint32_t kMH_MAGIC_64     = 0xfeedfacf;
constexpr uint32_t kMH_OBJECT       = 1;            // Mach-O filetype MH_OBJECT
constexpr uint32_t kLC_SEGMENT_64   = 0x19;         // LC_SEGMENT_64
constexpr uint32_t kLC_SYMTAB       = 0x2;

// arm64 Mach-O relocation types (mach-o/arm64/reloc.h).
enum MachOArm64Reloc : uint32_t {
  kARM64_RELOC_UNSIGNED = 0, kARM64_RELOC_SUBTRACTOR = 1,
  kARM64_RELOC_BRANCH26 = 2, kARM64_RELOC_PAGE21 = 3,
  kARM64_RELOC_PAGEOFF12 = 4, kARM64_RELOC_GOT_LOAD_PAGE21 = 5,
  kARM64_RELOC_GOT_LOAD_PAGEOFF12 = 6, kARM64_RELOC_POINTER_TO_GOT = 7,
  kARM64_RELOC_TLVP_LOAD_PAGE21 = 8, kARM64_RELOC_TLVP_LOAD_PAGEOFF12 = 9,
  kARM64_RELOC_TLV = 10, kARM64_RELOC_ADDEND = 10,
};

#pragma pack(push, 1)
struct MachHeader64 {
  uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct SymtabCmd { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct SegmentCommand64 {
  uint32_t cmd, cmdsize;
  char segname[16];
  uint64_t vmaddr, vmsize, fileoff, filesize;
  uint32_t maxprot, initprot, nsects, flags;
};
struct Section64 {
  char sectname[16], segname[16];
  uint64_t addr, size;
  // canonical mach-o/loader.h layout: offset..reserved3 are all uint32_t,
  // giving exactly 80 bytes -- any wider field makes ld64 reject the load
  // command ("unaligned size").
  uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct MachReloc {
  uint32_t r_address;
  uint32_t r_symbolnum : 24, r_pcrel : 1, r_length : 2, r_extern : 1, r_type : 4;
};
struct MachNList64 {
  uint32_t n_strx; uint8_t n_type, n_sect; uint16_t n_desc; uint64_t n_value;
};
#pragma pack(pop)

// ld64 hard-rejects objects whose load-command structures deviate from the
// canonical sizes; lock them in at compile time.
static_assert(sizeof(MachHeader64) == 32, "Mach-O 64 header must be 32 bytes");
static_assert(sizeof(SegmentCommand64) == 72, "LC_SEGMENT_64 must be 72 bytes");
static_assert(sizeof(Section64) == 80, "section_64 must be 80 bytes");
static_assert(sizeof(SymtabCmd) == 24, "LC_SYMTAB must be 24 bytes");
static_assert(sizeof(MachNList64) == 16, "nlist_64 must be 16 bytes");
static_assert(sizeof(MachReloc) == 8, "relocation_info must be 8 bytes");

constexpr uint32_t kMachCpuTypeArm64 = 0x0100000C;
constexpr uint32_t kMachCpuSubtypeArm64All = 0;
constexpr uint32_t kNTypeExt = 0x01, kNTypeSect = 0x0e, kNTypeUndf = 0x00;
constexpr uint32_t kSectAttrs = 0x80000000;  // S_ATTR_PURE_INSTRUCTIONS|SOME_INSTRUCTIONS
constexpr uint32_t kSZerofill = 1;

}  // namespace aotdroid
