// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/aot/macho_emit.cpp
// ============================================================================
#include "macho_emit.h"

namespace aotdroid::macho {

namespace {
constexpr uint32_t kSegNameText   = 0;  // indexes into a name table (built below)
}  // namespace

bool emit_object(const ObjectSpec& spec, std::vector<uint8_t>* out,
                 std::string* err) {
  // Layout plan (MH_OBJECT):
  //   [ MachHeader64 ][ Section64[] xN ][ SymtabCmd ]
  //   [ section file data (aligned) ]
  //   [ relocation_info[] per section ]
  //   [ MachNList64[] ][ string table ]
  out->clear();
  const size_t nSect = spec.sections.size();
  if (nSect == 0 || nSect > 255) { *err = "emit_object: section count"; return false; }

  const size_t headerSize = sizeof(MachHeader64) + nSect * sizeof(Section64)
                            + sizeof(SymtabCmd);
  // ---- assign file offsets to section data ----
  struct SecInfo { uint64_t fileOff = 0; bool hasFile = false; uint64_t relocOff = 0; };
  std::vector<SecInfo> si(nSect);
  uint64_t cursor = headerSize;
  for (size_t i = 0; i < nSect; ++i) {
    const SectionSpec& s = spec.sections[i];
    uint32_t align = std::max<uint32_t>(s.align, 2);
    cursor = (cursor + ((1ull << align) - 1)) & ~((1ull << align) - 1);
    if (s.zerofill) { si[i].hasFile = false; continue; }
    si[i].fileOff = cursor; si[i].hasFile = true;
    cursor += s.data.size();
  }
  // ---- relocations per section ----
  uint64_t relocCursor = (cursor + 7) & ~7ull;
  for (size_t i = 0; i < nSect; ++i) si[i].relocOff = relocCursor;
  // count reloc words (addend pairs included)
  uint64_t totalReloc = 0;
  {
    std::vector<uint32_t> perSect(nSect, 0);
    for (const FixupSpec& f : spec.fixups) {
      if (f.siteSection == 0 || f.siteSection > nSect) { *err = "fixup bad section"; return false; }
      perSect[f.siteSection - 1] += (f.addend != 0 && f.kind != FixupSpec::kUnsigned8) ? 2 : 1;
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
  mh.ncmds = 1;  // only LC_SYMTAB (sections live directly in the header space)
  mh.sizeofcmds = (uint32_t)(nSect * sizeof(Section64) + sizeof(SymtabCmd));
  mh.flags = 0x00200000;  // MH_SUBSECTIONS_VIA_SYMBOLS
  put(&mh, sizeof(mh), 0);

  // ---- section headers ----
  for (size_t i = 0; i < nSect; ++i) {
    const SectionSpec& s = spec.sections[i];
    Section64 sc{};
    memset(sc.sectname, 0, 16); memset(sc.segname, 0, 16);
    strncpy(sc.sectname, s.sectName.c_str(), 15);
    strncpy(sc.segname, s.segName.c_str(), 15);
    sc.addr = s.vaddr;
    sc.size = s.zerofill ? s.zsize : s.data.size();
    sc.offset = (uint32_t)(si[i].hasFile ? si[i].fileOff : 0);
    sc.align = std::max<uint32_t>(s.align, 2);
    sc.reloff = (uint32_t)si[i].relocOff;
    // recount nreloc for this section
    uint32_t n = 0;
    for (const FixupSpec& f : spec.fixups) {
      if (f.siteSection - 1 == i)
        n += (f.addend != 0 && f.kind != FixupSpec::kUnsigned8) ? 2 : 1;
    }
    sc.nreloc = n;
    sc.flags = s.flags;
    put(&sc, sizeof(sc), sizeof(MachHeader64) + i * sizeof(Section64));
  }

  // ---- symtab command ----
  SymtabCmd stc{};
  stc.cmd = kLC_SYMTAB;
  stc.cmdsize = sizeof(SymtabCmd);
  stc.symoff = (uint32_t)symOff;
  stc.nsyms = (uint32_t)spec.symbols.size();
  stc.stroff = (uint32_t)strOff;
  stc.strsize = (uint32_t)(strCursor - strOff);
  put(&stc, sizeof(stc), sizeof(MachHeader64) + nSect * sizeof(Section64));

  // ---- section data ----
  for (size_t i = 0; i < nSect; ++i) {
    if (si[i].hasFile && !spec.sections[i].data.empty())
      put(spec.sections[i].data.data(), spec.sections[i].data.size(), si[i].fileOff);
  }

  // ---- relocations ----
  {
    std::vector<uint64_t> wcur(nSect);
    for (size_t i = 0; i < nSect; ++i) wcur[i] = si[i].relocOff;
    for (const FixupSpec& f : spec.fixups) {
      uint32_t s = f.siteSection - 1;
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
            emitReloc(site, f.targetSection, false, 3, false, kARM64_RELOC_UNSIGNED);
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
      memcpy(base + sc, spec.symbols[i].name.c_str(), spec.symbols[i].name.size() + 1);
      sc += spec.symbols[i].name.size() + 1;
    }
    for (size_t i = 0; i < spec.symbols.size(); ++i) {
      const SymbolSpec& s = spec.symbols[i];
      MachNList64 n{};
      n.n_strx = (uint32_t)stroffs[i] - (uint32_t)strOff;
      n.n_type = s.sectIdx == 0 ? kNTypeUndf
                                : (uint8_t)(s.external ? (kNTypeExt | kNTypeSect) : kNTypeSect);
      n.n_sect = (uint8_t)s.sectIdx;
      n.n_desc = 0;
      n.n_value = s.value;
      put(&n, sizeof(n), symOff + i * sizeof(MachNList64));
    }
    base[strOff] = 0; base[strOff + 1] = 0;  // safety pair (strOff maps the "" entry)
    if (sc != strCursor) { /* trailing pad only */ }
  }
  (void)kSegNameText;
  return true;
}

}  // namespace aotdroid::macho
