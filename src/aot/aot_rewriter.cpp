// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/aot/aot_rewriter.cpp   (MODULE 2)
//  Multi-version ELF64 / OAT AOT rewriter for Android 7.1 / 8.1 / 10.
// ============================================================================
#include "aot_rewriter.h"

namespace aotdroid {

// ---------------------------------------------------------------------------
// aotmeta::Builder
// ---------------------------------------------------------------------------
namespace aotmeta {

uint32_t Builder::addString(const std::string& s) {
  auto it = strIdx_.find(s);
  if (it != strIdx_.end()) return it->second;
  uint32_t off = (uint32_t)strs_.size();
  strs_ += s; strs_ += '\0';
  strIdx_[s] = off;
  return off;
}
int32_t Builder::addSlot(const std::string& soname, const std::string& sym,
                         uint16_t kind) {
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].kind == kind) {
      const char* base = strs_.c_str();
      if (strcmp(base + slots_[i].so, soname.c_str()) == 0 &&
          strcmp(base + slots_[i].nm, sym.c_str()) == 0)
        return (int32_t)i;
    }
  }
  SlotRec r{addString(soname), addString(sym), kind};
  slots_.push_back(r);
  return (int32_t)(slots_.size() - 1);
}
void Builder::addFixup(uint32_t sectIdx, uint64_t off, uint32_t origReloc,
                       int32_t slot, const std::string& soname,
                       const std::string& sym, int64_t addend) {
  fixups_.push_back(FixRec{sectIdx, off, origReloc, slot,
                           addString(soname), addString(sym), addend});
}
std::vector<uint8_t> Builder::serialize(const ImageInfo& info,
                                        const std::vector<SectEntry>& sections) const {
  const size_t strOff = sizeof(Header) + sizeof(ImageInfo)
                        + sections.size() * sizeof(SectEntry);
  const size_t slotOff = strOff + strs_.size();
  const size_t fixOff = slotOff + slots_.size() * sizeof(SlotRecord);
  const size_t total = fixOff + fixups_.size() * sizeof(FixupRecord);
  std::vector<uint8_t> out(total, 0);
  uint8_t* p = out.data();
  auto wr = [&](const void* src, size_t n, size_t off) { memcpy(p + off, src, n); };
  Header h{};
  h.magic = kMagic; h.version = kVersion;
  h.strTabOff = (uint32_t)strOff; h.strTabLen = (uint32_t)strs_.size();
  h.slotOff = (uint32_t)slotOff; h.slotCount = (uint32_t)slots_.size();
  h.fixupOff = (uint32_t)fixOff; h.fixupCount = (uint32_t)fixups_.size();
  h.sectOff = (uint32_t)(sizeof(Header) + sizeof(ImageInfo));
  h.sectCount = (uint32_t)sections.size();
  h.imageInfoOff = (uint32_t)sizeof(Header);
  wr(&h, sizeof(h), 0);
  wr(&info, sizeof(info), sizeof(Header));
  if (!sections.empty())
    wr(sections.data(), sections.size() * sizeof(SectEntry), h.sectOff);
  if (!strs_.empty()) wr(strs_.data(), strs_.size(), strOff);
  for (size_t i = 0; i < slots_.size(); ++i) {
    SlotRecord r{};
    r.sonameOff = slots_[i].so; r.nameOff = slots_[i].nm; r.kind = slots_[i].kind;
    wr(&r, sizeof(r), slotOff + i * sizeof(SlotRecord));
  }
  for (size_t i = 0; i < fixups_.size(); ++i) {
    const FixRec& f = fixups_[i];
    FixupRecord r{};
    r.sectIdx = f.sectIdx; r.offset = f.off; r.origReloc = f.reloc; r.slotIdx = f.slot;
    r.symSonameOff = f.so; r.symNameOff = f.nm; r.addend = f.addend;
    wr(&r, sizeof(r), fixOff + i * sizeof(FixupRecord));
  }
  return out;
}

}  // namespace aotmeta

// ---------------------------------------------------------------------------
// Internal ELF model
// ---------------------------------------------------------------------------
namespace {

struct Seg {                 // PT_LOAD
  Phdr64 ph;
  size_t newOff = 0;
  uint64_t newVaddr = 0;
  int64_t vdelta = 0, fdelta = 0;
  bool exec() const { return ph.p_flags & kPF_X; }
};

struct PatchSite {
  uint64_t vaddr;
  size_t fileOff;
  enum Kind : uint8_t { kSyscall, kTlsRead, kTlsRo, kTlsWrite } kind;
  uint32_t reg;
  size_t veneerOff = 0;
};

inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct ElfCtx {
  const uint8_t* f = nullptr; size_t fsize = 0;
  Ehdr64 eh{};
  std::vector<Phdr64> phdrs;
  std::vector<Shdr64> shdrs;
  std::vector<Seg> segs;
  std::string shstr;
  const char* shname(size_t i) const {
    if (i >= shdrs.size()) return "";
    const Shdr64& s = shdrs[i];
    if (s.sh_name >= shstr.size()) return "";
    return shstr.c_str() + s.sh_name;
  }
  const Seg* seg_of_vaddr(uint64_t va, uint64_t* offInSeg = nullptr) const {
    for (const Seg& s : segs) {
      if (va >= s.ph.p_vaddr && va < s.ph.p_vaddr + s.ph.p_filesz) {
        if (offInSeg) *offInSeg = va - s.ph.p_vaddr;
        return &s;
      }
    }
    return nullptr;
  }
  const Seg* seg_of_fileoff(size_t off) const {
    for (const Seg& s : segs)
      if (off >= s.ph.p_offset && off < s.ph.p_offset + s.ph.p_filesz) return &s;
    return nullptr;
  }
};

bool parseElf(ElfCtx& c, std::string* err) {
  if (c.fsize < sizeof(Ehdr64)) { *err = "file too small"; return false; }
  memcpy(&c.eh, c.f, sizeof(Ehdr64));
  if (memcmp(c.eh.e_ident, "\x7f" "ELF", 4) != 0) { *err = "not ELF"; return false; }
  if (c.eh.e_ident[4] != 2 || c.eh.e_ident[5] != 1) { *err = "need ELF64 LE"; return false; }
  if (c.eh.e_machine != kEM_AARCH64) { *err = "not EM_AARCH64"; return false; }
  if (c.eh.e_type != kET_DYN && c.eh.e_type != kET_EXEC) { *err = "not exec/dyn"; return false; }
  if (c.eh.e_phoff == 0 || c.eh.e_phentsize != sizeof(Phdr64)) { *err = "bad phdrs"; return false; }
  c.phdrs.resize(c.eh.e_phnum);
  for (uint32_t i = 0; i < c.eh.e_phnum; ++i) {
    if (c.eh.e_phoff + (uint64_t)(i + 1) * sizeof(Phdr64) > c.fsize) { *err = "phdr oob"; return false; }
    memcpy(&c.phdrs[i], c.f + c.eh.e_phoff + (uint64_t)i * sizeof(Phdr64), sizeof(Phdr64));
  }
  if (c.eh.e_shoff && c.eh.e_shentsize == sizeof(Shdr64)) {
    uint32_t n = c.eh.e_shnum;
    if (n && c.eh.e_shoff + (uint64_t)n * sizeof(Shdr64) <= c.fsize) {
      c.shdrs.resize(n);
      for (uint32_t i = 0; i < n; ++i)
        memcpy(&c.shdrs[i], c.f + c.eh.e_shoff + (uint64_t)i * sizeof(Shdr64), sizeof(Shdr64));
      if (c.eh.e_shstrndx < n) {
        const Shdr64& ss = c.shdrs[c.eh.e_shstrndx];
        if (ss.sh_type == kSHT_STRTAB && ss.sh_offset + ss.sh_size <= c.fsize)
          c.shstr.assign((const char*)c.f + ss.sh_offset, ss.sh_size);
      }
    }
  }
  for (const Phdr64& p : c.phdrs) if (p.p_type == kPT_LOAD) {
    Seg s{}; s.ph = p; c.segs.push_back(s);
  }
  std::sort(c.segs.begin(), c.segs.end(),
            [](const Seg& a, const Seg& b) { return a.ph.p_vaddr < b.ph.p_vaddr; });
  return true;
}

bool probeOat(const ElfCtx& c, RewriteStats* st) {
  for (size_t i = 0; i < c.shdrs.size(); ++i) {
    if (strcmp(c.shname(i), ".oatdata") != 0) continue;
    const Shdr64& s = c.shdrs[i];
    if (s.sh_offset + sizeof(OatHeaderPrefix) > c.fsize) return false;
    OatHeaderPrefix hp{};
    memcpy(&hp, c.f + s.sh_offset, sizeof(hp));
    if (memcmp(hp.magic, kOatMagic, 4) != 0) return false;
    char ver[5] = {0}; memcpy(ver, hp.version, 4);
    uint32_t api = oat_version_to_api(ver);
    if (api == 0 || hp.instruction_set != kArtInstructionSetArm64) return false;
    if (st) { st->oatContainer = true; st->oatApiLevel = api;
              snprintf(st->oatVersion, sizeof(st->oatVersion), "%s", ver); }
    return true;
  }
  return false;
}

bool scanSites(const ElfCtx& c, std::vector<PatchSite>* sites, RewriteStats* st) {
  for (const Seg& s : c.segs) {
    if (!s.exec()) continue;
    const uint8_t* base = c.f + s.ph.p_offset;
    size_t words = s.ph.p_filesz / 4;
    for (size_t w = 0; w < words; ++w) {
      uint32_t insn; memcpy(&insn, base + w * 4, 4);
      PatchSite ps{};
      ps.vaddr = s.ph.p_vaddr + (uint64_t)(w * 4);
      ps.fileOff = s.ph.p_offset + w * 4;
      if (arm64::is_svc0(insn))                       ps.kind = PatchSite::kSyscall;
      else if (arm64::is_mrs_tpidr_el0(insn))  { ps.kind = PatchSite::kTlsRead;  ps.reg = arm64::regfield(insn); }
      else if (arm64::is_mrs_tpidrro_el0(insn)){ ps.kind = PatchSite::kTlsRo;    ps.reg = arm64::regfield(insn); }
      else if (arm64::is_msr_tpidr_el0(insn))  { ps.kind = PatchSite::kTlsWrite; ps.reg = arm64::regfield(insn); }
      else continue;
      sites->push_back(ps);
    }
  }
  if (st) for (const PatchSite& p : *sites) {
    switch (p.kind) {
      case PatchSite::kSyscall:  st->syscallSites++; break;
      case PatchSite::kTlsRead: st->tlsReadSites++; break;
      case PatchSite::kTlsRo:   st->tpidrroSites++; break;
      case PatchSite::kTlsWrite: st->tlsWriteSites++; break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Veneer emitters.
//
//  Syscall veneer (the asm bridge preserves x1..x17 + x30 itself):
//      bl  aotdroid_syscall_bridge        [BR26 fixup -> bridge symbol]
//      b   .Lsite_plus_4                  [BR26 fixup -> section sym + addend]
//
//  TLS read veneer (mrs xN, tpidr_el0 / tpidrro_el0):
//      stp x0, x1, [sp, #-152]!
//      str x2..x17 -> [sp,#16..#136] ; str x30,[sp,#144]
//      bl  aotdroid_get_android_tls_c     ; x0 = Android TP
//      str x0, [sp, #N*8]                 ; result lands in xN's slot
//      ldp x0, x1, [sp] ; ldr x2..x17 ; ldr x30,[sp,#144]
//      add sp, sp, #152
//      b   .Lsite_plus_4
//
//  TLS write veneer (msr tpidr_el0, xN):
//      frame as above ; mov x0, xN ; bl aotdroid_set_android_tls_c ;
//      restore frame (x0 from slot) ; add sp ; b .Lsite_plus_4
// ---------------------------------------------------------------------------
struct Veneer {
  std::vector<uint32_t> words;
  size_t blIdx;      // index of the `bl` to the bridge
  PatchSite site;
};

Veneer makeSyscallVeneer(const PatchSite& s) {
  Veneer v; v.site = s; v.blIdx = 0;
  v.words.push_back(0);                    // bl bridge (BR26 fixup)
  v.words.push_back(0);                    // b site+4  (BR26 fixup)
  return v;
}
Veneer makeTlsReadVeneer(const PatchSite& s) {
  Veneer v; v.site = s;
  v.words.push_back(arm64::enc_stp64_pre(0, 1, -152));
  for (uint32_t r = 2; r <= 17; ++r)
    v.words.push_back(arm64::enc_str64_imm(r, 31, r * 8));
  v.words.push_back(arm64::enc_str64_imm(30, 31, 144));
  v.blIdx = v.words.size();
  v.words.push_back(0);                    // bl get_tls
  v.words.push_back(arm64::enc_str64_imm(0, 31, s.reg * 8));
  v.words.push_back(arm64::enc_ldr64_imm(0, 31, 0));
  v.words.push_back(arm64::enc_ldr64_imm(1, 31, 8));
  for (uint32_t r = 2; r <= 17; ++r)
    v.words.push_back(arm64::enc_ldr64_imm(r, 31, r * 8));
  v.words.push_back(arm64::enc_ldr64_imm(30, 31, 144));
  v.words.push_back(arm64::kAddSp152);
  v.words.push_back(0);                    // b site+4
  return v;
}
Veneer makeTlsWriteVeneer(const PatchSite& s) {
  Veneer v; v.site = s;
  v.words.push_back(arm64::enc_stp64_pre(0, 1, -152));
  for (uint32_t r = 2; r <= 17; ++r)
    v.words.push_back(arm64::enc_str64_imm(r, 31, r * 8));
  v.words.push_back(arm64::enc_str64_imm(30, 31, 144));
  v.words.push_back(arm64::enc_mov(0, s.reg));
  v.blIdx = v.words.size();
  v.words.push_back(0);                    // bl set_tls
  v.words.push_back(arm64::enc_ldr64_imm(0, 31, 0));
  v.words.push_back(arm64::enc_ldr64_imm(1, 31, 8));
  for (uint32_t r = 2; r <= 17; ++r)
    v.words.push_back(arm64::enc_ldr64_imm(r, 31, r * 8));
  v.words.push_back(arm64::enc_ldr64_imm(30, 31, 144));
  v.words.push_back(arm64::kAddSp152);
  v.words.push_back(0);                    // b site+4
  return v;
}

}  // namespace

// ===========================================================================
// rewriteBuffer
// ===========================================================================
bool AotRewriter::rewriteBuffer(const uint8_t* in, size_t inLen,
                                const RewriteConfig& cfg,
                                std::vector<uint8_t>* outElf,
                                std::vector<uint8_t>* outMacho,
                                RewriteStats* stats, std::string* err) {
  RewriteStats dummy{};
  RewriteStats* st = stats ? stats : &dummy;
  ElfCtx c;
  c.f = in; c.fsize = inLen;
  if (!parseElf(c, err)) return false;
  st->inputBytes = inLen;
  probeOat(c, st);

  std::vector<PatchSite> sites;
  if (!scanSites(c, &sites, st)) return false;
  std::vector<Veneer> veneers;
  veneers.reserve(sites.size());
  for (const PatchSite& s : sites) {
    switch (s.kind) {
      case PatchSite::kSyscall:  veneers.push_back(makeSyscallVeneer(s)); break;
      case PatchSite::kTlsRead:
      case PatchSite::kTlsRo:    veneers.push_back(makeTlsReadVeneer(s)); break;
      case PatchSite::kTlsWrite: veneers.push_back(makeTlsWriteVeneer(s)); break;
    }
  }

  // ---- dynamic info -------------------------------------------------------
  const uint8_t* dynSeg = nullptr; size_t dynCount = 0;
  for (const Phdr64& p : c.phdrs) if (p.p_type == kPT_DYNAMIC) {
    dynSeg = in + p.p_offset; dynCount = p.p_filesz / sizeof(Dyn64);
  }
  auto dynVal = [&](int64_t tag) -> uint64_t {
    for (size_t i = 0; i < dynCount; ++i) {
      Dyn64 d; memcpy(&d, dynSeg + i * sizeof(Dyn64), sizeof(Dyn64));
      if (d.d_tag == tag) return d.d_val;
    }
    return 0;
  };
  const char* strtab = nullptr; size_t strtabSize = 0;
  {
    uint64_t off = dynVal(kDT_STRTAB);
    const Seg* s = c.seg_of_vaddr(off);
    if (s) { strtab = (const char*)in + s->ph.p_offset + (off - s->ph.p_vaddr);
             strtabSize = dynVal(kDT_STRSZ); }
  }
  auto dynStr = [&](uint64_t off) -> const char* {
    if (!strtab || off >= strtabSize) return "";
    return strtab + off;
  };
  std::string soname = cfg.sonameOverride.empty()
                         ? std::string(dynStr(dynVal(kDT_SONAME)))
                         : cfg.sonameOverride;
  if (soname.empty()) soname = "<anon>";

  // ---- 16 KiB re-layout of PT_LOADs --------------------------------------
  const uint64_t ps = cfg.targetPageSize;
  uint64_t nextVa = 0;
  for (Seg& s : c.segs) {
    s.newVaddr = align_up(nextVa, ps);
    s.vdelta = (int64_t)(s.newVaddr - s.ph.p_vaddr);
    nextVa = s.newVaddr + s.ph.p_memsz;
    st->segmentsRelaid++;
  }
  {
    uint64_t cur = 0;
    for (Seg& s : c.segs) {
      s.newOff = align_up(cur, ps);   // p_offset congruent p_vaddr (mod ps)
      s.fdelta = (int64_t)(s.newOff - s.ph.p_offset);
      cur = s.newOff + s.ph.p_filesz;
    }
  }
  auto newVaddrOf = [&](uint64_t va) -> uint64_t {
    const Seg* s = c.seg_of_vaddr(va);
    return s ? va + (uint64_t)s->vdelta : va;
  };
  auto newFileOffOf = [&](uint64_t va) -> size_t {
    const Seg* s = c.seg_of_vaddr(va);
    return s ? (size_t)(s->newOff + (va - s->ph.p_vaddr)) : 0;
  };

  // =========================================================================
  // ELF output (dev / non-iOS path)
  // =========================================================================
  if (outElf) {
    size_t totalFile = 0;
    for (const Seg& s : c.segs) totalFile = std::max(totalFile, s.newOff + s.ph.p_filesz);
    const size_t patchSegOff = align_up(totalFile, ps);
    const size_t patchSegVa = align_up(nextVa, ps);

    // veneer placement (skipping the 32-byte global thunk at offset 0)
    size_t patchBytes = 32;
    for (Veneer& v : veneers) { v.site.veneerOff = patchBytes;
                                patchBytes += v.words.size() * 4; }
    const size_t patchSegSize = align_up(patchBytes + 64, 16);
    const size_t elfOutSize = patchSegOff + patchSegSize + ps;

    outElf->assign(elfOutSize, 0);
    uint8_t* o = outElf->data();
    uint8_t* pb = o + patchSegOff;

    // headers + segments (first PT_LOAD keeps offset 0 / vaddr 0)
    memcpy(o, in, std::min<size_t>(c.segs.empty() ? c.fsize : c.segs[0].ph.p_filesz, c.fsize));
    for (const Seg& s : c.segs)
      memcpy(o + s.newOff, in + s.ph.p_offset, s.ph.p_filesz);

    // global thunk: ldr x16,#8 ; blr x16 ; .quad aotdroid_syscall_bridge
    {
      uint32_t t0 = 0x58000050u;                 // ldr x16, #8
      memcpy(pb + 0, &t0, 4);
      memcpy(pb + 4, &arm64::kBlrX16, 4);
      uint64_t zero = 0;                         // runtime-filled by loader
      memcpy(pb + 8, &zero, 8);
    }
    // veneer bodies; bl -> global thunk; final b -> site+4; site -> b veneer
    for (Veneer& v : veneers) {
      PatchSite& p = v.site;
      for (size_t k = 0; k < v.words.size(); ++k)
        memcpy(pb + p.veneerOff + k * 4, &v.words[k], 4);
      int64_t blPos = (int64_t)p.veneerOff + (int64_t)(v.blIdx * 4);
      uint32_t bl = arm64::enc_bl(0 - blPos);    // to thunk at patch offset 0
      memcpy(pb + blPos, &bl, 4);
      int64_t bPos = (int64_t)p.veneerOff + (int64_t)((v.words.size() - 1) * 4);
      int64_t fromB = (int64_t)(patchSegVa + p.veneerOff + (v.words.size() - 1) * 4);
      int64_t toRet = (int64_t)(newVaddrOf(p.vaddr) + 4);
      uint32_t bIns = arm64::enc_b(toRet - fromB);
      memcpy(pb + bPos, &bIns, 4);
      // patch the original site in its new location
      size_t siteOff = newFileOffOf(p.vaddr);
      uint64_t siteVa = newVaddrOf(p.vaddr);
      uint64_t veneerVa = patchSegVa + p.veneerOff;
      int64_t delta = (int64_t)veneerVa - (int64_t)siteVa;
      uint32_t siteInsn = arm64::b_fits(delta) ? arm64::enc_b(delta) : 0xD503201Fu;
      memcpy(o + siteOff, &siteInsn, 4);
      if (arm64::b_fits(delta)) st->veneersEmitted++; else st->brkTrampolines++;
    }

    // program headers: relocate table to tail, add PT_AOTPATCH
    {
      Ehdr64* eh = (Ehdr64*)o;
      size_t newPhoff = patchSegOff + patchSegSize;
      memcpy(o + newPhoff, in + c.eh.e_phoff, c.eh.e_phnum * sizeof(Phdr64));
      Phdr64* ph = (Phdr64*)(o + newPhoff);
      for (uint32_t i = 0; i < c.eh.e_phnum; ++i) {
        if (ph[i].p_type != kPT_LOAD) continue;
        for (const Seg& s : c.segs) {
          if (s.ph.p_vaddr == ph[i].p_vaddr && s.ph.p_filesz == ph[i].p_filesz) {
            ph[i].p_offset = s.newOff; ph[i].p_vaddr = s.newVaddr;
            ph[i].p_paddr = s.newVaddr; ph[i].p_align = ps;
          }
        }
      }
      Phdr64* np = ph + c.eh.e_phnum;
      np->p_type = 0x70000001;              // PT_AOTPATCH
      np->p_flags = kPF_R | kPF_X;
      np->p_offset = patchSegOff; np->p_vaddr = patchSegVa;
      np->p_paddr = patchSegVa;
      np->p_filesz = patchSegSize; np->p_memsz = patchSegSize; np->p_align = ps;
      eh->e_phoff = newPhoff; eh->e_phnum = (uint16_t)(c.eh.e_phnum + 1);
    }
    // section header fixups
    {
      Ehdr64* eh = (Ehdr64*)o;
      if (c.eh.e_shoff) {
        const Seg* shSeg = c.seg_of_fileoff(c.eh.e_shoff);
        size_t newShoff = shSeg ? c.eh.e_shoff + (size_t)shSeg->fdelta : c.eh.e_shoff;
        if (newShoff + (uint64_t)c.eh.e_shnum * sizeof(Shdr64) < elfOutSize) {
          Shdr64* sh = (Shdr64*)(o + newShoff);
          for (uint32_t i = 0; i < c.eh.e_shnum; ++i) {
            Shdr64& s = sh[i];
            if (s.sh_addr) s.sh_addr = newVaddrOf(s.sh_addr);
            const Seg* sd = c.seg_of_fileoff(s.sh_offset);
            if (sd && s.sh_type != kSHT_NOBITS) s.sh_offset += (size_t)sd->fdelta;
          }
          eh->e_shoff = newShoff;
        }
      }
    }
    st->outputElfBytes = outElf->size();
  }

  // =========================================================================
  // Mach-O output (production iOS path)
  // =========================================================================
  if (outMacho && cfg.emitMachO) {
    macho::ObjectSpec os;
    aotmeta::Builder mb;
    aotmeta::ImageInfo info{};

    // ---- map original sections into Mach-O sections --------------------
    struct MapEnt { size_t shIdx; uint32_t machoIdx; };
    std::vector<MapEnt> map;
    std::vector<aotmeta::SectEntry> sectEntries;
    std::vector<uint32_t> shToMacho(c.shdrs.size(), 0);
    for (size_t i = 0; i < c.shdrs.size(); ++i) {
      const Shdr64& s = c.shdrs[i];
      if (!(s.sh_type == kSHT_NOBITS || s.sh_type == kSHT_PROGBITS ||
            s.sh_type == kSHT_INIT_ARRAY || s.sh_type == kSHT_FINI_ARRAY ||
            s.sh_type == kSHT_PREINIT_ARRAY))
        continue;
      const Seg* sg = c.seg_of_vaddr(s.sh_addr);
      if (!sg || s.sh_size == 0) continue;
      if (strstr(c.shname(i), ".note") == c.shname(i)) continue;
      macho::SectionSpec ms;
      std::string sname = c.shname(i);
      if (!sname.empty() && sname[0] == '.') sname = sname.substr(1);
      if (sname.empty()) sname = "sec" + std::to_string(i);
      bool exec = sg->exec();
      ms.segName = exec ? "__TEXT"
                         : (s.sh_flags & 2 ? "__DATA" : "__DATA_CONST");
      ms.sectName = sname;
      ms.align = std::max<uint32_t>(2, (uint32_t)std::countr_zero((uint64_t)std::max<uint64_t>(1, s.sh_addralign)));
      ms.vaddr = s.sh_addr + (uint64_t)sg->vdelta;
      if (s.sh_type == kSHT_NOBITS) { ms.zerofill = true; ms.zsize = s.sh_size; }
      else ms.data.assign(in + s.sh_offset, in + s.sh_offset + s.sh_size);
      ms.flags = exec ? macho::kSectProgBitsExec : macho::kSectRegular;
      if (s.sh_type == kSHT_INIT_ARRAY || s.sh_type == kSHT_FINI_ARRAY)
        ms.flags = macho::kSectInitFuncPtrs;
      os.sections.push_back(std::move(ms));
      uint32_t idx = (uint32_t)os.sections.size();
      shToMacho[i] = idx;
      map.push_back(MapEnt{i, idx});
      aotmeta::SectEntry se{};
      strncpy(se.seg, os.sections.back().segName.c_str(), 15);
      strncpy(se.sect, os.sections.back().sectName.c_str(), 15);
      sectEntries.push_back(se);
    }
    auto machoSectOfVaddr = [&](uint64_t va, uint64_t* offInSect,
                                uint32_t* idx) -> bool {
      for (const MapEnt& m : map) {
        const Shdr64& s = c.shdrs[m.shIdx];
        if (va >= s.sh_addr && va < s.sh_addr + s.sh_size) {
          *offInSect = va - s.sh_addr; *idx = m.machoIdx; return true;
        }
      }
      return false;
    };

    // ---- __aotpatch section (veneers + thunk) ----------------------------
    uint32_t patchIdx = 0;
    {
      macho::SectionSpec ps2;
      ps2.segName = "__TEXT"; ps2.sectName = "__aotpatch";
      ps2.align = 4;
      size_t cur = 16;   // 0..15: thunk (ldr/blr/quad)
      for (Veneer& v : veneers) { v.site.veneerOff = cur; cur += v.words.size() * 4; }
      ps2.data.assign(align_up(cur + 16, 16), 0);
      uint32_t t0 = 0x58000050u;
      memcpy(ps2.data.data() + 0, &t0, 4);
      memcpy(ps2.data.data() + 4, &arm64::kBlrX16, 4);
      for (Veneer& v : veneers) {
        const PatchSite& p = v.site;
        for (size_t k = 0; k < v.words.size(); ++k)
          memcpy(ps2.data.data() + p.veneerOff + k * 4, &v.words[k], 4);
      }
      os.sections.push_back(std::move(ps2));
      patchIdx = (uint32_t)os.sections.size();
      aotmeta::SectEntry se{}; strcpy(se.seg, "__TEXT"); strcpy(se.sect, "__aotpatch");
      sectEntries.push_back(se);
    }

    // ---- symbols ---------------------------------------------------------
    os.symbols.push_back({"", 0, 0, false, false});   // index 0: empty string
    std::vector<uint32_t> sectSym(os.sections.size() + 1, 0);
    for (uint32_t i = 0; i < os.sections.size(); ++i) {
      macho::SymbolSpec ss;
      ss.name = "__aot_lsec_" + std::to_string(i + 1);
      ss.sectIdx = i + 1; ss.value = 0; ss.external = false;
      os.symbols.push_back(ss);
      sectSym[i + 1] = (uint32_t)os.symbols.size() - 1;
    }
    auto addExtern = [&](const char* n) -> uint32_t {
      macho::SymbolSpec ss; ss.name = n; ss.sectIdx = 0; ss.external = true;
      os.symbols.push_back(ss);
      return (uint32_t)os.symbols.size() - 1;
    };
    uint32_t symBridge = addExtern(arm64::kSymSyscallBridge);
    uint32_t symGetTls = addExtern(arm64::kSymGetAndroidTls);
    uint32_t symSetTls = addExtern(arm64::kSymSetAndroidTls);
    // Slot symbols are created lazily and finalized into __aotgot.
    std::unordered_map<int64_t, uint32_t> slotSymIdx;
    auto slotSym = [&](int64_t slot) -> uint32_t {
      auto it = slotSymIdx.find(slot);
      if (it != slotSymIdx.end()) return it->second;
      macho::SymbolSpec ss;
      ss.name = "__aot_slot_" + std::to_string((uint64_t)slot);
      ss.sectIdx = 0; ss.value = (uint64_t)slot * 16; ss.external = true;
      os.symbols.push_back(ss);
      uint32_t idx = (uint32_t)os.symbols.size() - 1;
      slotSymIdx[slot] = idx;
      return idx;
    };
    // dynamic symbols
    std::vector<uint32_t> dynToSym;
    {
      const Shdr64* ds = nullptr; const Shdr64* dsStr = nullptr;
      for (size_t i = 0; i < c.shdrs.size(); ++i)
        if (c.shdrs[i].sh_type == kSHT_DYNSYM) ds = &c.shdrs[i];
      if (ds) for (size_t i = 0; i < c.shdrs.size(); ++i)
        if (c.shdrs[i].sh_type == kSHT_STRTAB && ds->sh_link == i) dsStr = &c.shdrs[i];
      const char* dstr = dsStr ? (const char*)in + dsStr->sh_offset : "";
      size_t n = ds ? ds->sh_size / sizeof(Sym64) : 0;
      dynToSym.resize(n, 0);
      for (size_t i = 0; i < n; ++i) {
        Sym64 s; memcpy(&s, in + ds->sh_offset + i * sizeof(Sym64), sizeof(Sym64));
        const char* nm = (dsStr && s.st_name < dsStr->sh_size) ? dstr + s.st_name : "";
        if (!nm[0]) continue;
        if (s.st_shndx != 0 && s.st_shndx < 0xff00u && shToMacho[s.st_shndx]) {
          macho::SymbolSpec ss;
          ss.name = "__aot_" + soname + "_" + nm;
          ss.sectIdx = shToMacho[s.st_shndx];
          ss.value = s.st_value - c.shdrs[s.st_shndx].sh_addr;
          ss.external = true; ss.globalDef = true;
          os.symbols.push_back(ss);
          dynToSym[i] = (uint32_t)os.symbols.size() - 1;
        }
      }
    }

    // ---- relocation translation -----------------------------------------
    auto processRela = [&](const Shdr64& rsh) -> bool {
      const Shdr64* symTab = nullptr; const Shdr64* symStr = nullptr;
      for (size_t i = 0; i < c.shdrs.size(); ++i)
        if (c.shdrs[i].sh_type == kSHT_DYNSYM && rsh.sh_link == i) symTab = &c.shdrs[i];
      if (!symTab) { *err = "rela section has no linked dynsym"; return false; }
      for (size_t i = 0; i < c.shdrs.size(); ++i)
        if (c.shdrs[i].sh_type == kSHT_STRTAB && symTab->sh_link == i) symStr = &c.shdrs[i];
      const char* snames = symStr ? (const char*)in + symStr->sh_offset : "";
      size_t n = rsh.sh_size / sizeof(Rela64);
      for (size_t i = 0; i < n; ++i) {
        Rela64 r; memcpy(&r, in + rsh.sh_offset + i * sizeof(Rela64), sizeof(Rela64));
        uint32_t type = rela_type(r.r_info), sym = rela_sym(r.r_info);
        Sym64 s64{}; const char* sname = "";
        bool external = false;
        if (sym && symTab) {
          memcpy(&s64, in + symTab->sh_offset + (size_t)sym * sizeof(Sym64), sizeof(Sym64));
          sname = (symStr && s64.st_name < symStr->sh_size) ? snames + s64.st_name : "";
          external = s64.st_shndx == 0;
        }
        uint64_t offInSect; uint32_t sidx;
        if (!machoSectOfVaddr(r.r_offset, &offInSect, &sidx)) continue;
        auto& sect = os.sections[sidx - 1];
        bool isText = (sect.flags == macho::kSectProgBitsExec);
        uint32_t insn = 0;
        if (isText && offInSect + 4 <= sect.data.size())
          memcpy(&insn, sect.data.data() + offInSect, 4);

        auto pageFixup = [&](uint32_t kind, uint32_t symIdx, int64_t addend) {
          macho::FixupSpec f{};
          f.kind = (macho::FixupSpec::Kind)kind;
          f.siteSection = sidx; f.siteOffset = offInSect;
          f.targetSym = symIdx; f.addend = addend;
          os.fixups.push_back(f);
        };

        switch (type) {
          case kR_AARCH64_RELATIVE:
          case kR_AARCH64_IRELATIVE: {
            uint64_t tOff; uint32_t tIdx;
            if (machoSectOfVaddr((uint64_t)r.r_addend, &tOff, &tIdx) &&
                sect.data.size() >= offInSect + 8) {
              const Shdr64& tsh = c.shdrs[map[tIdx - 1].shIdx];
              uint64_t content = (uint64_t)r.r_addend - tsh.sh_addr;
              memcpy(sect.data.data() + offInSect, &content, 8);
              macho::FixupSpec f{};
              f.kind = macho::FixupSpec::kUnsigned8;
              f.siteSection = sidx; f.siteOffset = offInSect;
              f.targetSection = tIdx;
              os.fixups.push_back(f);
              st->relocsTranslated++;
            } else {
              mb.addFixup(sidx, offInSect, type, -1, soname, "", r.r_addend);
              st->runtimeFixups++;
            }
            break;
          }
          case kR_AARCH64_ABS64:
          case kR_AARCH64_GLOB_DAT:
          case kR_AARCH64_JUMP_SLOT: {
            if (external) {
              if (isText) { *err = std::string("ABS64-extern site in exec section: ")
                                  + sname; return false; }
              int32_t slot = mb.addSlot(soname, sname, 0);
              mb.addFixup(sidx, offInSect, type, slot, soname, sname, r.r_addend);
              st->runtimeFixups++;
            } else if (sym != 0 && s64.st_shndx < 0xff00u && shToMacho[s64.st_shndx]) {
              uint64_t tOff; uint32_t tIdx;
              if (machoSectOfVaddr(s64.st_value, &tOff, &tIdx) &&
                  sect.data.size() >= offInSect + 8) {
                uint64_t content = (s64.st_value - c.shdrs[s64.st_shndx].sh_addr)
                                   + (uint64_t)r.r_addend;
                memcpy(sect.data.data() + offInSect, &content, 8);
                macho::FixupSpec f{};
                f.kind = macho::FixupSpec::kUnsigned8;
                f.siteSection = sidx; f.siteOffset = offInSect;
                f.targetSection = tIdx;
                os.fixups.push_back(f);
              } else {
                mb.addFixup(sidx, offInSect, type, -1, soname, "", r.r_addend);
                st->runtimeFixups++;
              }
            }
            st->relocsTranslated++;
            break;
          }
          case kR_AARCH64_ADR_PREL_PG_HI21:
          case kR_AARCH64_ADR_GOT_PAGE:
          case kR_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21:
          case kR_AARCH64_TLSDESC_ADR_PAGE21: {
            if (external) {
              int32_t slot = mb.addSlot(soname, sname, 0);
              pageFixup(macho::FixupSpec::kPage21, slotSym(slot), 0);
            } else {
              uint64_t target = sym ? s64.st_value : (uint64_t)r.r_addend;
              uint64_t tOff; uint32_t tIdx;
              if (machoSectOfVaddr(target, &tOff, &tIdx)) {
                int64_t addend = sym
                  ? (int64_t)(s64.st_value - c.shdrs[s64.st_shndx].sh_addr) + r.r_addend
                  : r.r_addend;
                pageFixup(macho::FixupSpec::kPage21, sectSym[tIdx], addend);
              }
            }
            st->relocsTranslated++;
            break;
          }
          case kR_AARCH64_ADD_ABS_LO12_NC:
          case kR_AARCH64_LDST8_ABS_LO12_NC:
          case kR_AARCH64_LDST16_ABS_LO12_NC:
          case kR_AARCH64_LDST32_ABS_LO12_NC:
          case kR_AARCH64_LDST64_ABS_LO12_NC:
          case kR_AARCH64_LDST128_ABS_LO12_NC:
          case kR_AARCH64_LD64_GOT_LO12_NC:
          case kR_AARCH64_TLSDESC_LD64_LO12:
          case kR_AARCH64_TLSDESC_ADD_LO12: {
            if (external) {
              int32_t slot = mb.addSlot(soname, sname, 0);
              // ADD -> LDR for direct data access through the slot
              if (type == kR_AARCH64_ADD_ABS_LO12_NC && isText) {
                uint32_t ldr = 0xF9400000u | (insn & 0x00FFFFFFu);
                memcpy(sect.data.data() + offInSect, &ldr, 4);
              }
              pageFixup(macho::FixupSpec::kPage12, slotSym(slot), 0);
            } else {
              uint64_t target = sym ? s64.st_value : (uint64_t)r.r_addend;
              uint64_t tOff; uint32_t tIdx;
              if (machoSectOfVaddr(target, &tOff, &tIdx)) {
                int64_t addend = sym
                  ? (int64_t)(s64.st_value - c.shdrs[s64.st_shndx].sh_addr) + r.r_addend
                  : r.r_addend;
                pageFixup(macho::FixupSpec::kPage12, sectSym[tIdx], addend);
              }
            }
            st->relocsTranslated++;
            break;
          }
          case kR_AARCH64_CALL26:
          case kR_AARCH64_JUMP26:
          case kR_AARCH64_CONDBR19: {
            if (external) {
              *err = std::string("direct branch to external symbol ") + sname +
                     " (PLT required)";
              return false;
            }
            uint64_t target = sym ? s64.st_value : (uint64_t)r.r_addend;
            uint64_t tOff; uint32_t tIdx;
            if (machoSectOfVaddr(target, &tOff, &tIdx)) {
              int64_t addend = sym
                ? (int64_t)(s64.st_value - c.shdrs[s64.st_shndx].sh_addr) + r.r_addend
                : r.r_addend;
              uint32_t tsym = (sym < dynToSym.size() && dynToSym[sym]) ? dynToSym[sym]
                                                                       : sectSym[tIdx];
              macho::FixupSpec f{};
              f.kind = macho::FixupSpec::kBr26;
              f.siteSection = sidx; f.siteOffset = offInSect;
              f.targetSym = tsym; f.addend = addend;
              os.fixups.push_back(f);
            }
            st->relocsTranslated++;
            break;
          }
          case kR_AARCH64_TLS_TPREL64:
          case kR_AARCH64_TLSDESC: {
            int32_t slot = mb.addSlot(soname, sname, 1);
            mb.addFixup(sidx, offInSect, type, slot, soname, sname, r.r_addend);
            st->runtimeFixups++; st->relocsTranslated++;
            break;
          }
          case kR_AARCH64_TLSLE_ADD_TPREL_HI12:
          case kR_AARCH64_TLSLE_ADD_TPREL_LO12_NC: {
            auto it = cfg.tlsLayout.find(soname);
            if (it == cfg.tlsLayout.end()) {
              *err = "TLS reloc in " + soname + " without frozen TLS layout";
              return false;
            }
            int64_t off = it->second + r.r_addend;
            if (off < 0 || off > 0xFFFFFF) { *err = "TLS offset range"; return false; }
            if (isText) {
              uint32_t imm = (uint32_t)off & 0xFFF;
              if (type == kR_AARCH64_TLSLE_ADD_TPREL_HI12) imm = (uint32_t)((uint64_t)off >> 12) & 0xFFF;
              uint32_t w2 = 0x91000000u | (imm << 10) | (insn & 0x000003E0u) | (insn & 0x1Fu);
              memcpy(sect.data.data() + offInSect, &w2, 4);
            }
            st->relocsTranslated++;
            break;
          }
          default:
            *err = "unhandled reloc type " + std::to_string(type) + " in " + soname;
            return false;
        }
      }
      return true;
    };

    for (size_t i = 0; i < c.shdrs.size(); ++i) {
      const char* nm = c.shname(i);
      if (c.shdrs[i].sh_type == kSHT_RELA &&
          (strcmp(nm, ".rela.dyn") == 0 || strcmp(nm, ".rela.plt") == 0)) {
        if (!processRela(c.shdrs[i])) return false;
      }
    }

    // ---- __aotgot + __aotmeta -------------------------------------------
    {
      size_t nSlots = mb.slotCount();
      macho::SectionSpec gs;
      gs.segName = "__AOTDATA"; gs.sectName = "__aotgot"; gs.align = 4;
      gs.data.resize(std::max<size_t>(nSlots * 16, 16), 0);
      os.sections.push_back(std::move(gs));
      uint32_t gotIdx = (uint32_t)os.sections.size();
      aotmeta::SectEntry se{}; strcpy(se.seg, "__AOTDATA"); strcpy(se.sect, "__aotgot");
      sectEntries.push_back(se);
      for (auto& s : os.symbols)
        if (s.name.rfind("__aot_slot_", 0) == 0) { s.sectIdx = gotIdx; s.globalDef = true; }
    }

    // image info
    info.sonameOff = mb.addString(soname);
    std::string needed;
    for (size_t i = 0; i < dynCount; ++i) {
      Dyn64 d; memcpy(&d, dynSeg + i * sizeof(Dyn64), sizeof(Dyn64));
      if (d.d_tag == kDT_NEEDED) {
        if (!needed.empty()) needed += ':';
        needed += dynStr(d.d_val);
      }
    }
    info.neededOff = mb.addString(needed);
    info.entryOff = c.eh.e_entry;
    for (size_t i = 0; i < c.shdrs.size(); ++i) {
      const char* nm = c.shname(i);
      if (strcmp(nm, ".init_array") == 0) info.initArraySect = shToMacho[i];
      if (strcmp(nm, ".fini_array") == 0) info.finiSect = shToMacho[i];
    }
    info.initArrayCount = 0;
    for (const Phdr64& p : c.phdrs) if (p.p_type == kPT_TLS) {
      info.tlsSize = p.p_memsz; info.tlsAlign = p.p_align;
    }
    info.flags = st->oatContainer ? 1u : 0u;
    info.oatApiLevel = st->oatApiLevel;

    macho::SectionSpec ms;
    ms.segName = "__AOTDATA"; ms.sectName = "__aotmeta"; ms.align = 3;
    ms.data = mb.serialize(info, sectEntries);
    os.sections.push_back(std::move(ms));
    {
      aotmeta::SectEntry se{}; strcpy(se.seg, "__AOTDATA"); strcpy(se.sect, "__aotmeta");
      sectEntries.push_back(se);
      // re-serialize with the complete section list
      for (size_t i = 0; i < os.sections.size(); ++i)
        if (os.sections[i].sectName == "__aotmeta")
          os.sections[i].data = mb.serialize(info, sectEntries);
    }

    // ---- bridge fixups for the patch thunk + veneers --------------------
    {
      macho::FixupSpec f{};
      f.kind = macho::FixupSpec::kUnsigned8;
      f.siteSection = patchIdx; f.siteOffset = 8;
      f.targetSym = symBridge; f.targetSection = 0;
      os.fixups.push_back(f);
      for (const Veneer& v : veneers) {
        const PatchSite& p = v.site;
        macho::FixupSpec bf{};
        bf.kind = macho::FixupSpec::kBr26;
        bf.siteSection = patchIdx;
        bf.siteOffset = p.veneerOff + v.blIdx * 4;
        bf.targetSym = p.kind == PatchSite::kSyscall ? symBridge
                     : (p.kind == PatchSite::kTlsWrite ? symSetTls : symGetTls);
        bf.addend = 0;
        os.fixups.push_back(bf);
        uint64_t offInSect; uint32_t sidx;
        if (machoSectOfVaddr(p.vaddr, &offInSect, &sidx)) {
          macho::FixupSpec rf{};
          rf.kind = macho::FixupSpec::kBr26;
          rf.siteSection = patchIdx;
          rf.siteOffset = p.veneerOff + (v.words.size() - 1) * 4;
          rf.targetSym = sectSym[sidx];
          rf.addend = (int64_t)offInSect + 4;
          os.fixups.push_back(rf);
          // site -> veneer branch: placeholder bytes + BR26 fixup so ld64
          // computes the final displacement (sections may be far apart in
          // the linked image)
          auto& sect = os.sections[sidx - 1];
          if (sect.flags == macho::kSectProgBitsExec && offInSect + 4 <= sect.data.size()) {
            uint32_t si = arm64::enc_b(16);
            memcpy(sect.data.data() + offInSect, &si, 4);
            macho::FixupSpec sf{};
            sf.kind = macho::FixupSpec::kBr26;
            sf.siteSection = sidx; sf.siteOffset = offInSect;
            sf.targetSym = sectSym[patchIdx];
            sf.addend = (int64_t)p.veneerOff;
            os.fixups.push_back(sf);
          }
        }
      }
    }
    st->slotsEmitted = mb.slotCount();

    std::string merr;
    if (!macho::emit_object(os, outMacho, &merr)) { *err = "macho emit: " + merr; return false; }
    st->outputMachoBytes = outMacho->size();
  }

  return true;
}

bool AotRewriter::rewriteFile(const char* inPath, const char* outElfPath,
                              const char* outMachoPath, const RewriteConfig& cfg,
                              RewriteStats* stats, std::string* err) {
  FILE* f = fopen(inPath, "rb");
  if (!f) { *err = std::string("open ") + inPath; return false; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); *err = "empty input"; return false; }
  std::vector<uint8_t> buf((size_t)sz);
  if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); *err = "read"; return false; }
  fclose(f);
  std::vector<uint8_t> elf, mo;
  if (!rewriteBuffer(buf.data(), buf.size(), cfg,
                     outElfPath ? &elf : nullptr,
                     outMachoPath ? &mo : nullptr, stats, err))
    return false;
  auto dump = [](const char* p, const std::vector<uint8_t>& b) -> bool {
    FILE* o = fopen(p, "wb");
    if (!o) return false;
    fwrite(b.data(), 1, b.size(), o);
    fclose(o);
    return true;
  };
  if (outElfPath && !dump(outElfPath, elf)) { *err = "write elf"; return false; }
  if (outMachoPath && !dump(outMachoPath, mo)) { *err = "write macho"; return false; }
  return true;
}

}  // namespace aotdroid
