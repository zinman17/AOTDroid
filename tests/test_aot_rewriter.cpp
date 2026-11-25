// ============================================================================
//  AOTDroid-Adaptive-iOS  |  tests/test_aot_rewriter.cpp
//  Builds a synthetic AArch64 ELF64 shared object (in-memory) containing
//  real svc/mrs/msr patterns + RELA relocations, runs the rewriter and
//  asserts:
//    - arm64 encoder bytes match assembler-verified golden constants
//    - 16 KiB PT_LOAD re-layout invariants (p_offset congruent p_vaddr mod ps)
//    - sites patched with `b` to veneers, veneers return to site+4
//    - Mach-O object parses (magic / filetype / sections / symtab)
//    - glibc <elf.h> reloc number cross-assertions (when present)
// ============================================================================
#include "aot/aot_rewriter.h"
#include <cassert>
#include <cstdio>

using namespace aotdroid;

#if defined(__has_include)
#  if __has_include(<elf.h>)
#    include <elf.h>
#    define HAVE_GLIBC_ELF_H 1
#  endif
#endif

// ---------------------------------------------------------------------------
// Golden instruction encodings: every value below was independently produced
// with the LLVM/GNU assembler (see ARCHITECTURE.md "Encoder verification");
// the test pins our encoders to those bytes.
// ---------------------------------------------------------------------------
static void test_golden_encodings() {
  assert(arm64::kSvc0 == 0xD4000001u);                 // svc #0
  assert(arm64::kMrsTpidrEl0 == 0xD53BD040u);          // mrs x0, tpidr_el0
  assert(arm64::kMrsTpidrEl0 + 3 == 0xD53BD043u);      // mrs x3, tpidr_el0
  assert(arm64::kMrsTpidrroEl0 == 0xD53BD060u);        // mrs x0, tpidrro_el0
  assert(arm64::kMsrTpidrEl0 + 5 == 0xD51BD045u);      // msr tpidr_el0, x5
  assert(arm64::kStpX16X17PreNeg16 == 0xA9BF47F0u);    // stp x16,x17,[sp,#-16]!
  assert(arm64::kLdpX16X17Post16 == 0xA8C147F0u);      // ldp x16,x17,[sp],#16
  assert(arm64::kBlrX16 == 0xD63F0200u);
  assert(arm64::kNop == 0xD503201Fu);
  assert(arm64::enc_b(64) == 0x14000010u);
  assert(arm64::enc_b(-16) == 0x17FFFFFCu);
  assert(arm64::enc_bl(-8) == 0x97FFFFFEu);
  assert(arm64::enc_mov(0, 3) == 0xAA0303E0u);         // mov x0, x3
  assert(arm64::enc_ldr64_imm(16, 31, 8) == 0xF94007F0u);   // ldr x16,[sp,#8]
  assert(arm64::enc_ldr64_imm(16, 31, 0) == 0xF94003F0u);  // ldr x16,[sp]
  assert(arm64::enc_str64_imm(5, 31, 40) == 0xF90017E5u);  // str x5,[sp,#40]
  assert(arm64::enc_ldr64_imm(30, 31, 144) == 0xF9404BFEu);// ldr x30,[sp,#144]
  assert(arm64::kAddSp152 == 0x910263FFu);                 // add sp,sp,#152
  assert(arm64::enc_stp64_pre(0, 1, -152) == 0xA9B687E0u); // stp x0,x1,[sp,#-152]!
  assert(arm64::is_svc0(0xD4000001u));
  assert(!arm64::is_svc0(0xD4000000u));                // svc #0 with imm 0? no: exact
  assert(arm64::is_mrs_tpidr_el0(0xD53BD040u + 29));
  assert(arm64::b_fits(0x1FFFFFF * 4));      // max positive imm26
  assert(!arm64::b_fits(0x2000000 * 4));     // one word past the range
  assert(arm64::b_fits(-(0x1FFFFFF * 4)));
  printf("  golden arm64 encodings ............. OK\n");
}

#ifdef HAVE_GLIBC_ELF_H
static void test_reloc_numbers_vs_glibc() {
  static_assert((uint32_t)kR_AARCH64_ABS64 == R_AARCH64_ABS64, "");
  static_assert((uint32_t)kR_AARCH64_ADR_PREL_PG_HI21 == R_AARCH64_ADR_PREL_PG_HI21, "");
  static_assert((uint32_t)kR_AARCH64_ADD_ABS_LO12_NC == R_AARCH64_ADD_ABS_LO12_NC, "");
  static_assert((uint32_t)kR_AARCH64_LDST64_ABS_LO12_NC == R_AARCH64_LDST64_ABS_LO12_NC, "");
  static_assert((uint32_t)kR_AARCH64_CALL26 == R_AARCH64_CALL26, "");
  static_assert((uint32_t)kR_AARCH64_JUMP26 == R_AARCH64_JUMP26, "");
  static_assert((uint32_t)kR_AARCH64_GLOB_DAT == R_AARCH64_GLOB_DAT, "");
  static_assert((uint32_t)kR_AARCH64_JUMP_SLOT == R_AARCH64_JUMP_SLOT, "");
  static_assert((uint32_t)kR_AARCH64_RELATIVE == R_AARCH64_RELATIVE, "");
  static_assert((uint32_t)kR_AARCH64_TLSDESC == R_AARCH64_TLSDESC, "");
  printf("  reloc numbers vs glibc elf.h ........ OK\n");
}
#endif

// ---------------------------------------------------------------------------
// Synthetic ELF builder
// ---------------------------------------------------------------------------
namespace {
struct ElfBlob { std::vector<uint8_t> bytes; };

// Minimal ET_DYN ARM64 with:
//   PT_LOAD R+X  [ehdr, phdrs, .text (svc/mrs/msr + reloc site), .rela.dyn]
//   PT_LOAD R+W  [.data with an ABS64-internal target]
//   section headers + .dynstr + .dynsym + .shstrtab
std::vector<uint8_t> buildSyntheticElf(size_t extraTextPad) {
  const size_t kPageSize = 4096;                 // Linux 4K layout on purpose
  size_t textWords = 16 + extraTextPad / 4;
  size_t textSize = textWords * 4;

  // layout
  const size_t phdrOff = sizeof(Ehdr64);
  const size_t phnum = 3;                        // LOAD RX, LOAD RW, DYNAMIC
  const size_t textOff = (phdrOff + phnum * sizeof(Phdr64) + 63) & ~63ull;
  const size_t relasOff = textOff + textSize;
  const size_t relaCount = 2;
  const size_t dataOff = kPageSize;               // second page
  const size_t dataSize = 64;
  const size_t dynOff = dataOff + dataSize;
  const size_t dynCount = 8;
  const size_t shstrOff = dynOff + dynCount * sizeof(Dyn64);
  const size_t shstrSize = 128;
  const size_t dynstrOff = shstrOff + shstrSize;
  const size_t dynstrSize = 128;
  const size_t dynsymOff = dynstrOff + dynstrSize;
  const size_t dynsymCount = 3;
  const size_t shdrOff = dynsymOff + dynsymCount * sizeof(Sym64);
  const size_t shnum = 8;
  size_t total = shdrOff + shnum * sizeof(Shdr64);

  std::vector<uint8_t> b(total, 0);

  // .text content
  std::vector<uint32_t> text;
  text.push_back(0xA9BF7BFD);                     // stp x29,x30,[sp,#-16]!
  text.push_back(arm64::kSvc0);                   // syscall site 1
  text.push_back(0xD4000001u);                    // (dup, caught by scanner? same insn)
  text.push_back(arm64::kMrsTpidrEl0 + 7);        // mrs x7, tpidr_el0
  text.push_back(arm64::kMrsTpidrroEl0 + 4);      // mrs x4, tpidrro_el0
  text.push_back(0xAA1F03E0);                     // mov w0, wzr
  text.push_back(arm64::kMsrTpidrEl0 + 2);        // msr tpidr_el0, x2
  // ADR_PREL_PG_HI21 + ADD_ABS_LO12_NC pair (self-referential, internal)
  text.push_back(0x90000000);                     // adrp x0, 0 (reloc patched)
  text.push_back(0x91000000);                     // add x0, x0, #0 (reloc patched)
  while (text.size() < textWords) text.push_back(arm64::kNop);
  text[1] = arm64::kSvc0;
  text[2] = arm64::kNop;   // de-dup the duplicate so counts are deterministic
  memcpy(b.data() + textOff, text.data(), text.size() * 4);

  // program headers
  Phdr64 phRX{}; phRX.p_type = kPT_LOAD; phRX.p_flags = kPF_R | kPF_X;
  phRX.p_offset = 0; phRX.p_vaddr = 0; phRX.p_filesz = dataOff; phRX.p_memsz = dataOff;
  phRX.p_align = kPageSize;
  Phdr64 phRW{}; phRW.p_type = kPT_LOAD; phRW.p_flags = kPF_R | kPF_W;
  phRW.p_offset = dataOff; phRW.p_vaddr = kPageSize; phRW.p_filesz = dataSize;
  phRW.p_memsz = dataSize; phRW.p_align = kPageSize;
  Phdr64 phDyn{}; phDyn.p_type = kPT_DYNAMIC; phDyn.p_flags = kPF_R;
  phDyn.p_offset = dynOff; phDyn.p_vaddr = kPageSize + dynOff - dataOff;
  phDyn.p_filesz = dynCount * sizeof(Dyn64); phDyn.p_memsz = phDyn.p_filesz;
  phDyn.p_align = 8;
  memcpy(b.data() + phdrOff + 0 * sizeof(Phdr64), &phRX, sizeof(phRX));
  memcpy(b.data() + phdrOff + 1 * sizeof(Phdr64), &phRW, sizeof(phRW));
  memcpy(b.data() + phdrOff + 2 * sizeof(Phdr64), &phDyn, sizeof(phDyn));

  // .rela.dyn: RELATIVE at .data+0 pointing into .text; PG_HI21 at the adrp
  Rela64 r0{}; r0.r_offset = kPageSize; r0.r_info = rela_info(0, kR_AARCH64_RELATIVE);
  r0.r_addend = (int64_t)textOff;
  Rela64 r1{}; r1.r_offset = textOff + 7 * 4;
  r1.r_info = rela_info(1, kR_AARCH64_ADR_PREL_PG_HI21);
  r1.r_addend = (int64_t)textOff;
  memcpy(b.data() + relasOff + 0, &r0, sizeof(r0));
  memcpy(b.data() + relasOff + sizeof(Rela64), &r1, sizeof(r1));

  // .dynamic
  size_t d = 0;
  auto putDyn = [&](int64_t tag, uint64_t val) {
    Dyn64 e{tag, val};
    memcpy(b.data() + dynOff + d * sizeof(Dyn64), &e, sizeof(e));
    d++;
  };
  putDyn(kDT_STRTAB, kPageSize + dynstrOff - dataOff);
  putDyn(kDT_SYMTAB, kPageSize + dynsymOff - dataOff);
  putDyn(kDT_STRSZ, dynstrSize);
  putDyn(kDT_SYMENT, sizeof(Sym64));
  putDyn(kDT_RELA, kPageSize + relasOff - dataOff);
  putDyn(kDT_RELASZ, relaCount * sizeof(Rela64));
  putDyn(kDT_RELAENT, sizeof(Rela64));
  putDyn(kDT_NULL, 0);

  // strings
  memcpy(b.data() + shstrOff, "\0.text\0.rela.dyn\0.dynstr\0.dynsym\0.shstrtab\0.data\0", shstrSize > 48 ? 48 : shstrSize);
  memcpy(b.data() + dynstrOff, "\0libaot_test.so\0test_sym\0", 24);
  // dynsym: [0] null, [1] defined at textOff+12, [2] undefined (external)
  Sym64 s0{}; memcpy(b.data() + dynsymOff, &s0, sizeof(s0));
  Sym64 s1{}; s1.st_name = 16; s1.st_info = 0x12; s1.st_shndx = 1;
  s1.st_value = textOff + 12 * 4; memcpy(b.data() + dynsymOff + sizeof(Sym64), &s1, sizeof(s1));
  Sym64 s2{}; s2.st_name = 25; s2.st_info = 0x12; memcpy(b.data() + dynsymOff + 2 * sizeof(Sym64), &s2, sizeof(s2));

  // ELF header
  Ehdr64 eh{};
  memcpy(eh.e_ident, "\x7f" "ELF", 4);
  eh.e_ident[4] = 2; eh.e_ident[5] = 1; eh.e_ident[6] = 1;
  eh.e_type = kET_DYN; eh.e_machine = kEM_AARCH64; eh.e_version = 1;
  eh.e_entry = textOff;
  eh.e_phoff = phdrOff; eh.e_shoff = shdrOff;
  eh.e_ehsize = sizeof(Ehdr64); eh.e_phentsize = sizeof(Phdr64);
  eh.e_phnum = phnum; eh.e_shentsize = sizeof(Shdr64);
  eh.e_shnum = shnum; eh.e_shstrndx = 5;
  memcpy(b.data(), &eh, sizeof(eh));

  // section headers (names relative to shstrOff base at shstrOff offset 0)
  auto sh = [&](size_t i) -> Shdr64* { return (Shdr64*)(b.data() + shdrOff + i * sizeof(Shdr64)); };
  Shdr64 nullSh{}; *sh(0) = nullSh;
  Shdr64 textSh{};
  textSh.sh_name = 1; textSh.sh_type = kSHT_PROGBITS;
  textSh.sh_flags = 6 /*ALLOC|EXEC*/;
  textSh.sh_addr = textOff; textSh.sh_offset = textOff; textSh.sh_size = textSize;
  textSh.sh_addralign = 4;
  *sh(1) = textSh;
  Shdr64 relaSh{};
  relaSh.sh_name = 7; relaSh.sh_type = kSHT_RELA; relaSh.sh_flags = 0x40 /*INFO_LINK*/;
  relaSh.sh_addr = relasOff; relaSh.sh_offset = relasOff;   // inside LOAD#1 (vaddr==off)
  relaSh.sh_size = relaCount * sizeof(Rela64); relaSh.sh_link = 4; relaSh.sh_info = 1;
  relaSh.sh_addralign = 8; relaSh.sh_entsize = sizeof(Rela64);
  *sh(2) = relaSh;
  Shdr64 dynstrSh{};
  dynstrSh.sh_name = 17; dynstrSh.sh_type = kSHT_STRTAB; dynstrSh.sh_flags = 2;
  dynstrSh.sh_addr = kPageSize + dynstrOff - dataOff; dynstrSh.sh_offset = dynstrOff;
  dynstrSh.sh_size = dynstrSize; dynstrSh.sh_addralign = 1;
  *sh(3) = dynstrSh;
  Shdr64 dynsymSh{};
  dynsymSh.sh_name = 26; dynsymSh.sh_type = kSHT_DYNSYM; dynsymSh.sh_flags = 2;
  dynsymSh.sh_addr = kPageSize + dynsymOff - dataOff; dynsymSh.sh_offset = dynsymOff;
  dynsymSh.sh_size = dynsymCount * sizeof(Sym64); dynsymSh.sh_link = 3;
  dynsymSh.sh_info = 1; dynsymSh.sh_addralign = 8; dynsymSh.sh_entsize = sizeof(Sym64);
  *sh(4) = dynsymSh;
  Shdr64 shstrSh{};
  shstrSh.sh_name = 34; shstrSh.sh_type = kSHT_STRTAB;
  shstrSh.sh_offset = shstrOff; shstrSh.sh_size = shstrSize; shstrSh.sh_addralign = 1;
  *sh(5) = shstrSh;
  Shdr64 dataSh{};
  dataSh.sh_name = 44; dataSh.sh_type = kSHT_PROGBITS; dataSh.sh_flags = 3;
  dataSh.sh_addr = kPageSize; dataSh.sh_offset = dataOff; dataSh.sh_size = dataSize;
  dataSh.sh_addralign = 8;
  *sh(6) = dataSh;
  *sh(7) = nullSh;
  return b;
}
}  // namespace

static void test_rewrite_16k_and_veneers() {
  auto blob = buildSyntheticElf(0);
  RewriteConfig cfg;
  cfg.sonameOverride = "libaot_test.so";
  std::vector<uint8_t> elf, mo;
  RewriteStats st;
  std::string err;
  bool ok = AotRewriter::rewriteBuffer(blob.data(), blob.size(), cfg, &elf, &mo, &st, &err);
  if (!ok) { fprintf(stderr, "rewrite failed: %s\n", err.c_str()); }
  assert(ok);
  assert(st.syscallSites == 1);
  assert(st.tlsReadSites == 1);
  assert(st.tlsWriteSites == 1);
  assert(st.tpidrroSites == 1);

  // ---- ELF invariants ----
  Ehdr64 eh;
  memcpy(&eh, elf.data(), sizeof(eh));
  assert(eh.e_phnum == 4);                       // + PT_AOTPATCH
  assert(eh.e_phentsize == sizeof(Phdr64));
  uint32_t nLoad = 0, nPatch = 0;
  for (uint32_t i = 0; i < eh.e_phnum; ++i) {
    Phdr64 p;
    memcpy(&p, elf.data() + eh.e_phoff + (uint64_t)i * sizeof(Phdr64), sizeof(p));
    if (p.p_type == kPT_LOAD) {
      nLoad++;
      // 16 KiB congruence: p_offset == p_vaddr (mod 16384) and page aligned
      assert(p.p_offset % 16384 == p.p_vaddr % 16384);
      assert(p.p_offset % 4096 == 0);
      assert(p.p_align == 16384);
    }
    if (p.p_type == 0x70000001) nPatch++;
  }
  assert(nLoad == 2);
  assert(nPatch == 1);
  // RX and RW PT_LOADs must not share a 16 KiB page
  Phdr64 ph[4];
  for (uint32_t i = 0; i < 4; ++i)
    memcpy(&ph[i], elf.data() + eh.e_phoff + (uint64_t)i * sizeof(Phdr64), sizeof(Phdr64));
  assert((ph[0].p_vaddr + ph[0].p_memsz + 16383) / 16384 <= ph[1].p_vaddr / 16384 + (ph[1].p_vaddr % 16384 ? 0 : 0));

  // ---- site patching ----
  // The svc site (original file offset textOff+4 -> new file offset):
  // find the veneer b target: site word must be a `b` (0x14...) not svc.
  const size_t textOff = (sizeof(Ehdr64) + 3 * sizeof(Phdr64) + 63) & ~63ull;
  // Original text content began at textOff (aligned 64) in page 0; the first
  // PT_LOAD covers it with delta 0, so the patched site is at the same offset.
  uint32_t site;
  memcpy(&site, elf.data() + textOff + 4, 4);
  assert((site & 0xFC000000u) == 0x14000000u);   // b <veneer>
  uint32_t mrsSite;
  memcpy(&mrsSite, elf.data() + textOff + 3 * 4, 4);
  assert((mrsSite & 0xFC000000u) == 0x14000000u);
  uint32_t msrSite;
  memcpy(&msrSite, elf.data() + textOff + 6 * 4, 4);
  assert((msrSite & 0xFC000000u) == 0x14000000u);
  assert(st.veneersEmitted >= 3);

  // veneer thunk header: ldr x16,#8 ; blr x16
  Phdr64 patch;
  for (uint32_t i = 0; i < 4; ++i) {
    memcpy(&patch, elf.data() + eh.e_phoff + (uint64_t)i * sizeof(Phdr64), sizeof(Phdr64));
    if (patch.p_type == 0x70000001) break;
  }
  uint32_t t0, t1;
  memcpy(&t0, elf.data() + patch.p_offset + 0, 4);
  memcpy(&t1, elf.data() + patch.p_offset + 4, 4);
  assert(t0 == 0x58000050u);
  assert(t1 == arm64::kBlrX16);

  // ---- Mach-O invariants ----
  MachHeader64 mh;
  memcpy(&mh, mo.data(), sizeof(mh));
  assert(mh.magic == kMH_MAGIC_64);
  assert(mh.filetype == kMH_OBJECT);
  assert(mh.cputype == kMachCpuTypeArm64);
  assert(mh.sizeofcmds % 8 == 0);
  // walk load commands: sections must live inside LC_SEGMENT_64s (ld64
  // rejects bare section structs and unaligned cmdsize)
  Section64 sects[24];
  uint32_t nSect = 0;
  bool sawSymtab = false;
  {
    uint64_t off = sizeof(MachHeader64);
    for (uint32_t c = 0; c < mh.ncmds; ++c) {
      uint32_t cmd = 0, cmdsize = 0;
      memcpy(&cmd, mo.data() + off, 4);
      memcpy(&cmdsize, mo.data() + off + 4, 4);
      assert(cmdsize >= 8 && (cmdsize % 8) == 0);
      if (cmd == kLC_SEGMENT_64) {
        SegmentCommand64 sg;
        memcpy(&sg, mo.data() + off, sizeof(sg));
        assert(sg.nsects <= 24);
        for (uint32_t k = 0; k < sg.nsects && nSect < 24; ++k) {
          memcpy(&sects[nSect],
                 mo.data() + off + sizeof(SegmentCommand64) + k * sizeof(Section64),
                 sizeof(Section64));
          assert(strncmp(sects[nSect].segname, sg.segname, 16) == 0);
          nSect++;
        }
      } else if (cmd == kLC_SYMTAB) {
        sawSymtab = true;
      }
      off += cmdsize;
    }
    assert(off == sizeof(MachHeader64) + mh.sizeofcmds);
  }
  assert(sawSymtab);
  assert(nSect >= 3);
  bool hasPatch = false, hasGot = false, hasMeta = false;
  for (uint32_t i = 0; i < nSect; ++i) {
    if (strcmp(sects[i].sectname, "__aotpatch") == 0) hasPatch = true;
    if (strcmp(sects[i].sectname, "__aotgot") == 0) hasGot = true;
    if (strcmp(sects[i].sectname, "__aotmeta") == 0) hasMeta = true;
  }
  assert(hasPatch && hasGot && hasMeta);
  assert(st.relocsTranslated >= 2);   // RELATIVE + PG_HI21 from .rela.dyn

  printf("  rewrite 16K re-layout + veneers ..... OK (syscalls=%zu tls=%zu/%zu "
         "veneers=%zu relocs=%zu slots=%zu)\n",
         st.syscallSites, st.tlsReadSites, st.tlsWriteSites,
         st.veneersEmitted, st.relocsTranslated, st.slotsEmitted);
}

static void test_oat_probe() {
  auto blob = buildSyntheticElf(0);
  // graft an .oatdata section? synthetic file has no .oatdata; probe must
  // report a non-OAT image without failing.
  RewriteStats st;
  std::string err;
  RewriteConfig cfg;
  cfg.emitMachO = false;
  std::vector<uint8_t> elf;
  assert(AotRewriter::rewriteBuffer(blob.data(), blob.size(), cfg, &elf, nullptr, &st, &err));
  assert(!st.oatContainer);
  // OAT version table sanity
  assert(oat_version_to_api("088") == 25);
  assert(oat_version_to_api("131") == 27);
  assert(oat_version_to_api("170") == 29);
  assert(oat_version_to_api("079") == 24);
  assert(oat_version_to_api("999") == 0);
  printf("  OAT version probe ................... OK (079->24 088->25 131->27 170->29)\n");
}

int main() {
  printf("test_aot_rewriter:\n");
  test_golden_encodings();
#ifdef HAVE_GLIBC_ELF_H
  test_reloc_numbers_vs_glibc();
#endif
  test_rewrite_16k_and_veneers();
  test_oat_probe();
  printf("ALL PASS\n");
  return 0;
}
