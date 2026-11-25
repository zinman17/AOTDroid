// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/aot/macho_emit.cpp
// ============================================================================
#include "macho_emit.h"

namespace aotdroid::macho {

bool emit_object(const ObjectSpec& spec, std::vector<uint8_t>* out,
                 std::string* err) {
  // Layout plan (MH_OBJECT, conformant for ld64):
  //   [ MachHeader64 ]
  //   [ LC_SEGMENT_64 (per segment, grouped) ][ Section64[] of that segment ]
  //   [ LC_SYMTAB ]
  //   [ section file data (aligned) ]
  //   [ relocation_info[] per section ]
  //   [ MachNList64[] ][ string table ]
  //
  // ld64 requires every load command's cmdsize to be 8-byte aligned and every
  // section header to live inside an LC_SEGMENT_64 -- a bare Section64 after
  // the header is parsed as a load command and rejected ("unaligned size").
  out->clear();
  const size_t nSect = spec.sections.size();
  if (nSect == 0 || nSect > 255) { *err = "emit_object: section count"; return false; }

  // ---- group sections by segment name (stable first-appearance order) ----
  std::vector<std::string> segOrder;
  std::vector<size_t> sectSeg(nSect);
  for (size_t i = 0; i < nSect; ++i) {
    const std::string g = spec.sections[i].segName.empty()
                              ? std::string("__TEXT")
                              : spec.sections[i].segName;
    size_t gi = 0;
    for (; gi < segOrder.size(); ++gi)
      if (segOrder[gi] == g) break;
    if (gi == segOrder.size()) segOrder.push_back(g);
    sectSeg[i] = gi;
  }
  const size_t nSeg = segOrder.size();

  // spec-order (1-based) section index -> file-order (1-based) index
  std::vector<uint32_t> remap(nSect + 1, 0);
  {
    uint32_t next = 1;
    for (size_t g = 0; g < nSeg; ++g)
      for (size_t i = 0; i < nSect; ++i)
        if (sectSeg[i] == g) remap[i + 1] = next++;
  }
  // file-order (1-based) -> spec-order (1-based)
  std::vector<uint32_t> inv(nSect + 1, 0);
  for (size_t i = 1; i <= nSect; ++i) inv[remap[i]] = (uint32_t)i;

  const size_t headerSize = sizeof(MachHeader64)
                            + nSeg * sizeof(SegmentCommand64)
                            + nSect * sizeof(Section64)
                            + sizeof(SymtabCmd);
  // ---- assign file offsets to section data (file order) ----
  struct SecInfo { uint64_t fileOff = 0; bool hasFile = false; uint64_t relocOff = 0; };
  std::vector<SecInfo> si(nSect);
  uint64_t cursor = headerSize;
  for (size_t ni = 1; ni <= nSect; ++ni) {
    const SectionSpec& s = spec.sections[inv[ni] - 1];
    uint32_t align = std::max<uint32_t>(s.align, 2);
    cursor = (cursor + ((1ull << align) - 1)) & ~((1ull << align) - 1);
    if (s.zerofill) continue;                 // no file bytes
    si[ni - 1].fileOff = cursor; si[ni - 1].hasFile = true;
    cursor += s.data.size();
  }
  // ---- relocations per section (file order) ----
  uint64_t relocCursor = (cursor + 7) & ~7ull;
  for (size_t ni = 0; ni < nSect; ++ni) si[ni].relocOff = relocCursor;
  uint64_t totalReloc = 0;
  {
    std::vector<uint32_t> perSect(nSect, 0);
    for (const FixupSpec& f : spec.fixups) {
      if (f.siteSection == 0 || f.siteSection > nSect) {
        *err = "emit_object: fixup bad section"; return false;
      }
      perSect[remap[f.siteSection] - 1] +=
          (f.addend != 0 && f.kind != FixupSpec::kUnsigned8) ? 2 : 1;
    }
    for (uint32_t n : perSect) totalReloc += n;
    relocCursor += totalReloc * sizeof(MachReloc);
  }
  // ---- symbol table ----
  uint64_t symOff = relocCursor;
  uint64_t strOff = symOff + spec.symbols.size() * sizeof(MachNList64);
  uint64_t strCursor = strOff + 1 + 1;  // leading NUL + trailing NUL safety
  for (const SymbolSpec& s : spec.symbols) strCursor += s.name.size() + 1;
  uint64_t totalSize = (strCursor + 7) & ~7ull;

  out->assign(totalSize, 0);
  uint8_t* base = out->data();

  auto put = [&](const void* p, size_t n, uint64_t off) {
    memcpy(base + off, p, n);
  };

  // ---- header ----
  MachHeader64 mh{};
  mh.magic = kMH_MAGIC_64;
  mh.cputype = kMachCpuTypeArm64;
  mh.cpusubtype = kMachCpuSubtypeArm64All;
  mh.filetype = kMH_OBJECT;
  mh.ncmds = (uint32_t)(nSeg + 1);   // nSeg x LC_SEGMENT_64 + LC_SYMTAB
  mh.sizeofcmds = (uint32_t)(nSeg * sizeof(SegmentCommand64)
                              + nSect * sizeof(Section64) + sizeof(SymtabCmd));
  mh.flags = 0x00200000;  // MH_SUBSECTIONS_VIA_SYMBOLS
  put(&mh, sizeof(mh), 0);

  // ---- segment commands + section headers (grouped, file order) ----
  {
    uint64_t off = sizeof(MachHeader64);
    for (size_t g = 0; g < nSeg; ++g) {
      // sections of this segment, in file order
      std::vector<size_t> members;
      for (size_t ni = 1; ni <= nSect; ++ni)
        if (sectSeg[inv[ni] - 1] == g) members.push_back(ni);

      SegmentCommand64 sg{};
      sg.cmd = kLC_SEGMENT_64;
      sg.cmdsize = (uint32_t)(sizeof(SegmentCommand64)
                              + members.size() * sizeof(Section64));
      memset(sg.segname, 0, 16);
      strncpy(sg.segname, segOrder[g].c_str(), 15);
      sg.vmaddr = 0;
      uint64_t vmsz = 0;
      for (size_t ni : members) {
        const SectionSpec& s = spec.sections[inv[ni] - 1];
        vmsz += s.zerofill ? s.zsize : s.data.size();
      }
      sg.vmsize = vmsz;
      sg.fileoff = 0;
      sg.filesize = totalSize;   // whole object rides in the segment (clang-style)
      sg.maxprot = 7;            // RWX (object file; final link decides)
      sg.initprot = 7;
      sg.nsects = (uint32_t)members.size();
      sg.flags = 0;
      put(&sg, sizeof(sg), off);
      off += sizeof(SegmentCommand64);

      for (size_t ni : members) {
        const SectionSpec& s = spec.sections[inv[ni] - 1];
        Section64 sc{};
        memset(sc.sectname, 0, 16); memset(sc.segname, 0, 16);
        strncpy(sc.sectname, s.sectName.c_str(), 15);
        strncpy(sc.segname, segOrder[g].c_str(), 15);
        sc.addr = s.vaddr;
        sc.size = s.zerofill ? s.zsize : s.data.size();
        sc.offset = (uint32_t)(si[ni - 1].hasFile ? si[ni - 1].fileOff : 0);
        sc.align = std::max<uint32_t>(s.align, 2);
        sc.reloff = (uint32_t)si[ni - 1].relocOff;
        uint32_t n = 0;
        for (const FixupSpec& f : spec.fixups) {
          if (remap[f.siteSection] == ni)
            n += (f.addend != 0 && f.kind != FixupSpec::kUnsigned8) ? 2 : 1;
        }
        sc.nreloc = n;
        sc.flags = s.flags;
        put(&sc, sizeof(sc), off);
        off += sizeof(Section64);
      }
    }
    // ---- symtab command ----
    SymtabCmd stc{};
    stc.cmd = kLC_SYMTAB;
    stc.cmdsize = sizeof(SymtabCmd);
    stc.symoff = (uint32_t)symOff;
    stc.nsyms = (uint32_t)spec.symbols.size();
    stc.stroff = (uint32_t)strOff;
    stc.strsize = (uint32_t)(strCursor - strOff);
    put(&stc, sizeof(stc), off);
  }

  // ---- section data ----
  for (size_t ni = 1; ni <= nSect; ++ni) {
    const SectionSpec& s = spec.sections[inv[ni] - 1];
    if (si[ni - 1].hasFile && !s.data.empty())
      put(s.data.data(), s.data.size(), si[ni - 1].fileOff);
  }

  // ---- relocations ----
  {
    std::vector<uint64_t> wcur(nSect);
    for (size_t ni = 0; ni < nSect; ++ni) wcur[ni] = si[ni].relocOff;
    for (const FixupSpec& f : spec.fixups) {
      const uint32_t s = remap[f.siteSection] - 1;
      auto emitReloc = [&](uint32_t address, uint32_t symbolnum, bool pcrel,
                           uint32_t len, bool isExtern, uint32_t type) {
        MachReloc r{};
        r.r_address = address;
        r.r_symbolnum = symbolnum & 0xFFFFFF;
        r.r_pcrel = pcrel; r.r_length = len; r.r_extern = isExtern; r.r_type = type;
        put(&r, sizeof(r), wcur[s]); wcur[s] += sizeof(MachReloc);
      };
      const uint32_t site = (uint32_t)f.siteOffset;
      bool useAddendPair = (f.addend != 0 && f.kind != FixupSpec::kUnsigned8);
      if (useAddendPair) {
        // ARM64_RELOC_ADDEND immediately precedes its paired relocation.
        int32_t a24 = (int32_t)std::clamp<int64_t>(f.addend, -(1 << 23), (1 << 23) - 1);
        emitReloc(site, (uint32_t)a24, false, 2, false, kARM64_RELOC_ADDEND);
      }
      switch (f.kind) {
        case FixupSpec::kUnsigned8:
          if (f.targetSection != 0) {
            emitReloc(site, remap[f.targetSection], false, 3, false,
                      kARM64_RELOC_UNSIGNED);
          } else {
            emitReloc(site, f.targetSym, false, 3, true, kARM64_RELOC_UNSIGNED);
          }
          break;
        case FixupSpec::kPage21:
          emitReloc(site, f.targetSym, true, 2, true, kARM64_RELOC_PAGE21);
          break;
        case FixupSpec::kPage12:
          emitReloc(site, f.targetSym, true, 2, true, kARM64_RELOC_PAGEOFF12);
          break;
        case FixupSpec::kBr26:
          emitReloc(site, f.targetSym, true, 2, true, kARM64_RELOC_BRANCH26);
          break;
        default:
          *err = "emit_object: unknown fixup kind"; return false;
      }
    }
  }

  // ---- symbol table + strings ----
  {
    std::vector<uint64_t> stroffs(spec.symbols.size());
    uint64_t sc = strOff + 1;  // byte 0 is the empty string
    for (size_t i = 0; i < spec.symbols.size(); ++i) {
      stroffs[i] = sc;
      memcpy(base + sc, spec.symbols[i].name.c_str(),
             spec.symbols[i].name.size() + 1);
      sc += spec.symbols[i].name.size() + 1;
    }
    for (size_t i = 0; i < spec.symbols.size(); ++i) {
      const SymbolSpec& s = spec.symbols[i];
      MachNList64 n{};
      n.n_strx = (uint32_t)stroffs[i] - (uint32_t)strOff;
      n.n_type = s.sectIdx == 0 ? kNTypeUndf
                                : (uint8_t)(s.external ? (kNTypeExt | kNTypeSect)
                                                       : kNTypeSect);
      n.n_sect = s.sectIdx == 0 ? 0 : (uint8_t)remap[s.sectIdx];
      n.n_desc = 0;
      n.n_value = s.value;
      put(&n, sizeof(n), symOff + i * sizeof(MachNList64));
    }
    base[strOff] = 0; base[strOff + 1] = 0;  // safety pair ("" entry)
  }
  return true;
}

}  // namespace aotdroid::macho
