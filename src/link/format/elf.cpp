#include "link/format/elf.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"
#include "link/format/exec_reloc_apply.hpp"
#include "link/format/string_table.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ELF64 relocatable (.o) writer — plan 14 LK1 cycle 1.
//
// Byte layout (gABI Ch. 4 + AMD64 psABI §4.4):
//   [0x00]      Elf64_Ehdr (64 B)
//   [0x40]      .text body                  (align 16, already at 64)
//   [...]       pad to 8
//   [...]       .rela.text                  (n_relocs × 24)
//   [...]       pad to 8
//   [...]       .symtab                     (n_syms × 24)
//   [...]       .strtab
//   [...]       .shstrtab
//   [e_shoff]   Section Header Table        (n_sections × 64)
//
// The walker is target-blind in shape — every ELF-specific number
// (machine, class, data encoding, section flags, reloc nativeIds,
// section names) is read from the format schema. The only
// hardcoded structural knowledge is the ELF64 binary record layout.

namespace dss::elf {

namespace {

using lir_pass_util::report;
using link::format::detail::appendU8;
using link::format::detail::appendU16LE;
using link::format::detail::appendU32LE;
using link::format::detail::appendU64LE;
using link::format::detail::appendI64LE;
using link::format::detail::emit;
using link::format::detail::requireSection;

// ── Elf64 type constants (gABI Ch. 4 + <elf.h>) ─────────────────

constexpr std::uint16_t ET_REL = 1;
constexpr std::uint32_t EV_CURRENT = 1;

// Elf64_Sym st_info encoding (gABI 4.18).
constexpr std::uint8_t STB_LOCAL  = 0;
constexpr std::uint8_t STB_GLOBAL = 1;
constexpr std::uint8_t STT_NOTYPE = 0;
constexpr std::uint8_t STT_FUNC   = 2;
constexpr std::uint8_t STT_SECTION = 3;
constexpr std::uint16_t SHN_UNDEF = 0;

// Elf64 sh_type / sh_flags (gABI 4.7-4.8) — used by the dynamic
// walker (cycle 2b.2). Named to match `<elf.h>`; type-design #2
// convergence (LK6 cycle 2b.2 review).
constexpr std::uint32_t SHT_PROGBITS = 1;
constexpr std::uint32_t SHT_SYMTAB   = 2;
constexpr std::uint32_t SHT_STRTAB   = 3;
constexpr std::uint32_t SHT_RELA     = 4;
constexpr std::uint32_t SHT_HASH     = 5;
constexpr std::uint32_t SHT_DYNAMIC  = 6;
constexpr std::uint32_t SHT_DYNSYM   = 11;
constexpr std::uint64_t SHF_WRITE     = 1;
constexpr std::uint64_t SHF_ALLOC     = 2;
constexpr std::uint64_t SHF_EXECINSTR = 4;

// Elf64 p_type / p_flags (gABI Fig. 5-2).
constexpr std::uint32_t PT_LOAD    = 1;
constexpr std::uint32_t PT_DYNAMIC = 2;
constexpr std::uint32_t PT_INTERP  = 3;
constexpr std::uint32_t PT_PHDR    = 6;
constexpr std::uint32_t PF_X = 1;
constexpr std::uint32_t PF_W = 2;
constexpr std::uint32_t PF_R = 4;

// Elf64 d_tag (gABI 5.10).
constexpr std::uint64_t DT_NULL    = 0;
constexpr std::uint64_t DT_NEEDED  = 1;
constexpr std::uint64_t DT_HASH    = 4;
constexpr std::uint64_t DT_STRTAB  = 5;
constexpr std::uint64_t DT_SYMTAB  = 6;
constexpr std::uint64_t DT_RELA    = 7;
constexpr std::uint64_t DT_RELASZ  = 8;
constexpr std::uint64_t DT_RELAENT = 9;
constexpr std::uint64_t DT_STRSZ   = 10;
constexpr std::uint64_t DT_SYMENT  = 11;
constexpr std::uint64_t DT_FLAGS_1 = 0x6ffffffb;
constexpr std::uint64_t DF_1_NOW   = 1;

// x86_64 psABI reloc type.
constexpr std::uint32_t R_X86_64_GLOB_DAT = 6;

constexpr std::uint8_t makeStInfo(std::uint8_t bind, std::uint8_t type) {
    return static_cast<std::uint8_t>((bind << 4) | (type & 0xF));
}

// Elf64_Rela r_info encoding (gABI 4.13).
constexpr std::uint64_t makeRelaInfo(std::uint32_t symIdx,
                                      std::uint32_t type) {
    return (static_cast<std::uint64_t>(symIdx) << 32)
         | static_cast<std::uint64_t>(type);
}

// Byte-emit helpers + emit() + requireSection() now hoisted to
// `src/link/format/byte_emit.hpp` (substrate shared with PE walker
// and future Mach-O walker — simplifier fold-in #1+#3).

void padTo(std::vector<std::uint8_t>& out, std::uint64_t alignment) {
    if (alignment <= 1) return;
    while (out.size() % alignment != 0) out.push_back(0);
}

// String-table builder hoisted to `src/link/format/string_table.hpp`
// (D-LK4-9 closure, 3rd-consumer trigger from Mach-O). ELF uses
// the NulByte init: byte 0 is the empty-name sentinel.
using link::format::detail::StringTable;

// ── Section header record (in-memory before serialization) ──────

struct SectionHeader {
    std::uint32_t name_offset = 0;   // offset into .shstrtab
    std::uint32_t type = 0;          // sh_type
    std::uint64_t flags = 0;         // sh_flags
    std::uint64_t addr = 0;          // sh_addr (0 in ET_REL)
    std::uint64_t offset = 0;        // sh_offset (filled after layout)
    std::uint64_t size = 0;          // sh_size
    std::uint32_t link = 0;          // sh_link
    std::uint32_t info = 0;          // sh_info
    std::uint64_t addr_align = 0;    // sh_addralign
    std::uint64_t entry_size = 0;    // sh_entsize
};

// File-local helpers used by the dynamic walker (simplifier #1+#4
// convergence). `padToOffset` extends `bytes` to an absolute file
// position with zero-fill (NOT an alignment round-up — that's the
// existing `padTo(out, alignment)`). `appendBytes` collapses the
// `insert(end, X.begin(), X.end())` boilerplate to a named call.
inline void padToOffset(std::vector<std::uint8_t>& out, std::uint64_t offset) {
    if (out.size() < offset) out.resize(offset, 0);
}
inline void appendBytes(std::vector<std::uint8_t>& out,
                         std::span<std::uint8_t const> body) {
    out.insert(out.end(), body.begin(), body.end());
}

void writeSectionHeader(std::vector<std::uint8_t>& out, SectionHeader const& h) {
    appendU32LE(out, h.name_offset);
    appendU32LE(out, h.type);
    appendU64LE(out, h.flags);
    appendU64LE(out, h.addr);
    appendU64LE(out, h.offset);
    appendU64LE(out, h.size);
    appendU32LE(out, h.link);
    appendU32LE(out, h.info);
    appendU64LE(out, h.addr_align);
    appendU64LE(out, h.entry_size);
}

// ── LK6 cycle 2b.2: ELF dynamic-linker image emission ───────────
//
// Closes the walker half of plan 14 §3.1 D-LK6-4. Emits an ET_EXEC
// with PT_INTERP + PT_DYNAMIC + writable PT_LOAD #2 covering
// `.got` + `.dynamic`. Uses **eager binding** (DT_FLAGS_1 =
// DF_1_NOW + R_X86_64_GLOB_DAT in `.rela.dyn`) — simpler than
// lazy binding with PLT0 + R_X86_64_JUMP_SLOT, produces a binary
// dyld resolves before transferring control. PLT stubs are 6-byte
// `FF 25 disp32` (jmp [rip+disp32]) bridging direct `call rel32`
// → indirect dispatch via GOT.
//
// Section layout (and on-disk order):
//   PT_LOAD #1 (R+X, page-aligned VA + file offset):
//     [0]               Ehdr                              (64 B)
//     [64]              Program Header Table  (PT_PHDR + PT_INTERP
//                                              + PT_LOAD#1 + PT_LOAD#2
//                                              + PT_DYNAMIC = 5×56=280)
//     [phtEnd]          .interp     (NUL-terminated path)
//     [pad to pageAlign]
//     [pageAlign]       .text       (sh_addr = secText.virtualAddress)
//     [16-aligned]      .plt        (6 B per extern)
//     [8-aligned]       .dynsym     (24 B per entry — STN_UNDEF + N)
//     [1-aligned]       .dynstr     (NUL + extern names + lib paths)
//     [8-aligned]       .hash       (16 + 4*(N+1) bytes)
//     [8-aligned]       .rela.dyn   (24 B per extern — GLOB_DAT)
//   PT_LOAD #2 (R+W, next pageAlign-aligned VA + file offset):
//     [pageAligned]     .got        (8 B per extern, zero-init —
//                                    dyld writes resolved fn ptrs)
//     [8-aligned]       .dynamic    (16 B per DT_* entry)
//   Non-loaded (after PT_LOAD #2):
//     [8-aligned]       .symtab     (intra-module function symbols)
//     [...]             .strtab
//     [...]             .shstrtab
//     [8-aligned]       Section Header Table (13 entries × 64 B)
//
// PLT-stub-internal `jmp [rip+disp32]` is computed inline (not
// pushed through `applyExecRelocations`) — the kernel sig reshape
// for multi-patch-section is anchored at D-LK6-7 and not needed
// for cycle 2b.2.
[[nodiscard]] std::vector<std::uint8_t>
encodeElfExecDynamic(
    AssembledModule const&         module,
    TargetSchema const&            targetSchema,
    ObjectFormatSchema const&      fmt,
    ObjectFormatSectionInfo const& secText,
    DiagnosticReporter&            reporter) {
    auto const& elfId = fmt.elf();
    std::uint64_t const pageAlign = elfId.pageAlign;

    // Pre-conditions (caller dispatches on these; re-checked here so
    // the helper is callable from future entry paths too —
    // silent-failure H2 + C1 convergence).
    if (module.externImports.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: called with zero "
             "externImports — dynamic-image emission requires at "
             "least one extern; static images route through the "
             "non-dynamic ET_EXEC arm.");
        return {};
    }
    if (module.functions.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: zero functions — ET_EXEC "
             "needs at least one entry function.");
        return {};
    }
    if (secText.virtualAddress < pageAlign) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"elf::encodeElfExecDynamic: .text "
                         "virtualAddress (0x"} +
             std::to_string(secText.virtualAddress) +
             ") is below pageAlign (0x" +
             std::to_string(pageAlign) +
             ") — baseImageVa would underflow. ET_EXEC needs "
             "headers + PHT + .interp to fit in one page below "
             ".text.");
        return {};
    }
    // baseImageVa is one page below .text (so Ehdr + PHT + .interp
    // fit in the first page of PT_LOAD #1, .text starts at
    // secText.virtualAddress = baseImageVa + pageAlign).
    std::uint64_t const baseImageVa = secText.virtualAddress - pageAlign;
    auto const roundUp = [](std::uint64_t v, std::uint64_t a) {
        return (v + a - 1) & ~(a - 1);
    };

    // ── (a) Build .text + per-function offset table ────────────
    std::vector<std::uint8_t> text;
    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());
    for (auto const& fn : module.functions) {
        funcTextStart.push_back(text.size());
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
    }
    if (text.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: every AssembledFunction "
             "contributed zero bytes — `.text` is empty; ET_EXEC "
             "requires at least one entry instruction.");
        return {};
    }

    // ── (b) Group externs by library (preserve declaration order)
    std::vector<std::string> libraryOrder;
    std::unordered_map<std::string, std::vector<std::size_t>>
        externsByLib;
    for (std::size_t i = 0; i < module.externImports.size(); ++i) {
        auto const& ext = module.externImports[i];
        auto const it = externsByLib.find(ext.libraryPath);
        if (it == externsByLib.end()) {
            libraryOrder.push_back(ext.libraryPath);
            externsByLib.emplace(ext.libraryPath,
                                 std::vector<std::size_t>{i});
        } else {
            it->second.push_back(i);
        }
    }
    std::size_t const numExterns = module.externImports.size();
    std::size_t const numLibs = libraryOrder.size();

    // ── (c) .interp body (NUL-terminated dynamic-linker path)
    std::vector<std::uint8_t> interp;
    for (char c : elfId.interpreter)
        interp.push_back(static_cast<std::uint8_t>(c));
    interp.push_back(0);

    // ── (d) .dynstr body (NUL + extern names + library paths)
    std::vector<std::uint8_t> dynstr;
    dynstr.push_back(0);
    std::vector<std::uint32_t> externNameOff(numExterns);
    for (std::size_t i = 0; i < numExterns; ++i) {
        externNameOff[i] = static_cast<std::uint32_t>(dynstr.size());
        for (char c : module.externImports[i].mangledName)
            dynstr.push_back(static_cast<std::uint8_t>(c));
        dynstr.push_back(0);
    }
    std::vector<std::uint32_t> libNameOff(numLibs);
    for (std::size_t i = 0; i < numLibs; ++i) {
        libNameOff[i] = static_cast<std::uint32_t>(dynstr.size());
        for (char c : libraryOrder[i])
            dynstr.push_back(static_cast<std::uint8_t>(c));
        dynstr.push_back(0);
    }

    // ── (e) .dynsym body (STN_UNDEF + N extern symbols)
    std::vector<std::uint8_t> dynsym;
    auto appendDynsymEntry = [&](std::uint32_t nameOff,
                                  std::uint8_t info,
                                  std::uint16_t shndx,
                                  std::uint64_t value,
                                  std::uint64_t size) {
        appendU32LE(dynsym, nameOff);
        appendU8(dynsym, info);
        appendU8(dynsym, 0);
        appendU16LE(dynsym, shndx);
        appendU64LE(dynsym, value);
        appendU64LE(dynsym, size);
    };
    appendDynsymEntry(0, 0, 0, 0, 0);  // STN_UNDEF
    std::vector<std::uint32_t> dynsymIdx(numExterns);
    for (std::size_t i = 0; i < numExterns; ++i) {
        dynsymIdx[i] =
            static_cast<std::uint32_t>(dynsym.size() / 24);
        appendDynsymEntry(externNameOff[i],
                          makeStInfo(STB_GLOBAL, STT_NOTYPE),
                          SHN_UNDEF, 0, 0);
    }

    // ── (f) .hash body (DT_HASH single-bucket)
    std::vector<std::uint8_t> hashSec;
    appendU32LE(hashSec, 1);  // nbucket
    appendU32LE(hashSec, static_cast<std::uint32_t>(numExterns + 1));  // nchain
    appendU32LE(hashSec,
        numExterns > 0 ? 1u : 0u);                     // bucket[0]
    appendU32LE(hashSec, 0);                            // chain[0]=STN_UNDEF
    for (std::size_t i = 0; i < numExterns; ++i) {
        std::uint32_t const next =
            (i + 1 < numExterns) ? static_cast<std::uint32_t>(i + 2) : 0u;
        appendU32LE(hashSec, next);
    }

    // ── (g) .plt body (placeholder; filled after layout)
    std::vector<std::uint8_t> plt(numExterns * 6, 0);

    // ── (h) .got body (zero-init; dyld writes resolved fn ptrs)
    std::vector<std::uint8_t> got(numExterns * 8, 0);

    // ── (i) Layout: compute file offsets + VAs ─────────────────
    constexpr std::uint64_t kEhdrSize = 64;
    constexpr std::uint64_t kPhdrSize = 56;
    constexpr std::uint32_t kNumPhdrs = 5;  // PHDR + INTERP + LOAD×2 + DYNAMIC
    std::uint64_t const phtOff = kEhdrSize;
    std::uint64_t const phtSize = kNumPhdrs * kPhdrSize;
    std::uint64_t const interpOff = phtOff + phtSize;
    std::uint64_t const interpVa  = baseImageVa + interpOff;

    std::uint64_t const textOff = pageAlign;   // .text at page boundary
    std::uint64_t const textVa  = secText.virtualAddress;

    // Subsequent sections in PT_LOAD #1; VA = baseImageVa + fileOff
    // (PT_LOAD with fileoff=0 and vaddr=baseImageVa maps verbatim).
    std::uint64_t const pltOff = roundUp(textOff + text.size(), 16);
    std::uint64_t const pltVa  = baseImageVa + pltOff;
    std::uint64_t const pltSize = plt.size();

    std::uint64_t const dynsymOff = roundUp(pltOff + pltSize, 8);
    std::uint64_t const dynsymVa  = baseImageVa + dynsymOff;
    std::uint64_t const dynsymSz  = dynsym.size();

    std::uint64_t const dynstrOff = dynsymOff + dynsymSz;
    std::uint64_t const dynstrVa  = baseImageVa + dynstrOff;
    std::uint64_t const dynstrSz  = dynstr.size();

    std::uint64_t const hashOff = roundUp(dynstrOff + dynstrSz, 8);
    std::uint64_t const hashVa  = baseImageVa + hashOff;
    std::uint64_t const hashSz  = hashSec.size();

    std::uint64_t const relaDynOff = roundUp(hashOff + hashSz, 8);
    std::uint64_t const relaDynVa  = baseImageVa + relaDynOff;
    std::uint64_t const relaDynSz  = numExterns * 24;

    std::uint64_t const ptLoad1End = relaDynOff + relaDynSz;

    // PT_LOAD #2 (R+W) — page-aligned in both file + VA.
    std::uint64_t const ptLoad2Start = roundUp(ptLoad1End, pageAlign);
    std::uint64_t const ptLoad2VaStart = baseImageVa + ptLoad2Start;
    std::uint64_t const gotOff = ptLoad2Start;
    std::uint64_t const gotVa  = ptLoad2VaStart;
    std::uint64_t const gotSz  = got.size();

    std::uint64_t const dynamicOff = roundUp(gotOff + gotSz, 8);
    std::uint64_t const dynamicVa  = baseImageVa + dynamicOff;

    // ── (j) Build .plt bytes (now we have VAs)
    for (std::size_t i = 0; i < numExterns; ++i) {
        std::uint64_t const stubVa = pltVa + i * 6;
        std::uint64_t const stubEndVa = stubVa + 6;
        std::uint64_t const slotVa = gotVa + i * 8;
        std::int64_t const disp =
            static_cast<std::int64_t>(slotVa)
            - static_cast<std::int64_t>(stubEndVa);
        if (disp < std::numeric_limits<std::int32_t>::min()
         || disp > std::numeric_limits<std::int32_t>::max()) {
            emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                 "elf::encodeElfExecDynamic: PLT→GOT displacement "
                 "(" + std::to_string(disp) +
                 ") does not fit in 32 bits — module too large.");
            return {};
        }
        plt[i*6 + 0] = 0xFFu;
        plt[i*6 + 1] = 0x25u;
        auto const u = static_cast<std::uint32_t>(disp);
        plt[i*6 + 2] = static_cast<std::uint8_t>(u & 0xFF);
        plt[i*6 + 3] = static_cast<std::uint8_t>((u >> 8)  & 0xFF);
        plt[i*6 + 4] = static_cast<std::uint8_t>((u >> 16) & 0xFF);
        plt[i*6 + 5] = static_cast<std::uint8_t>((u >> 24) & 0xFF);
    }

    // ── (k) Build .rela.dyn (R_X86_64_GLOB_DAT for each GOT slot)
    constexpr std::uint32_t R_X86_64_GLOB_DAT = 6;
    std::vector<std::uint8_t> relaDyn;
    relaDyn.reserve(relaDynSz);
    for (std::size_t i = 0; i < numExterns; ++i) {
        std::uint64_t const slotVa = gotVa + i * 8;
        std::uint64_t const rInfo =
            (static_cast<std::uint64_t>(dynsymIdx[i]) << 32)
            | static_cast<std::uint64_t>(R_X86_64_GLOB_DAT);
        appendU64LE(relaDyn, slotVa);
        appendU64LE(relaDyn, rInfo);
        appendI64LE(relaDyn, 0);
    }

    // ── (l) Build .dynamic
    std::vector<std::uint8_t> dynamicSec;
    auto appendDyn = [&](std::uint64_t tag, std::uint64_t val) {
        appendU64LE(dynamicSec, tag);
        appendU64LE(dynamicSec, val);
    };
    // DT_NEEDED per library, then the resolution-side metadata,
    // then DF_1_NOW for eager binding, then DT_NULL terminator.
    for (std::size_t i = 0; i < numLibs; ++i) {
        appendDyn(DT_NEEDED, libNameOff[i]);
    }
    appendDyn(DT_STRTAB,  dynstrVa);
    appendDyn(DT_STRSZ,   dynstrSz);
    appendDyn(DT_SYMTAB,  dynsymVa);
    appendDyn(DT_SYMENT,  24);
    appendDyn(DT_HASH,    hashVa);
    appendDyn(DT_RELA,    relaDynVa);
    appendDyn(DT_RELASZ,  relaDynSz);
    appendDyn(DT_RELAENT, 24);
    appendDyn(DT_FLAGS_1, DF_1_NOW);
    appendDyn(DT_NULL,    0);
    std::uint64_t const dynamicSz = dynamicSec.size();

    std::uint64_t const ptLoad2End = dynamicOff + dynamicSz;
    std::uint64_t const ptLoad2Size = ptLoad2End - ptLoad2Start;

    // Non-loaded .symtab / .strtab / .shstrtab + SHT.
    std::vector<std::uint8_t> symtab;
    StringTable strtab;
    auto appendSymtabEntry = [&](std::uint32_t nameOff,
                                  std::uint8_t info,
                                  std::uint16_t shndx,
                                  std::uint64_t value,
                                  std::uint64_t size) {
        appendU32LE(symtab, nameOff);
        appendU8(symtab, info);
        appendU8(symtab, 0);
        appendU16LE(symtab, shndx);
        appendU64LE(symtab, value);
        appendU64LE(symtab, size);
    };
    appendSymtabEntry(0, 0, 0, 0, 0);                  // STN_UNDEF
    appendSymtabEntry(0, makeStInfo(STB_LOCAL, STT_SECTION),
                      2 /*shndx=.text*/, 0, 0);
    std::uint32_t const firstNonLocal = 2;
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        auto const& fn = module.functions[i];
        std::string const name =
            std::string{"sym_"} + std::to_string(fn.symbol.v);
        std::uint32_t const nameOff = strtab.add(name);
        appendSymtabEntry(nameOff,
                          makeStInfo(STB_GLOBAL, STT_FUNC),
                          2 /*shndx=.text*/,
                          textVa + funcTextStart[i],
                          fn.bytes.size());
    }

    StringTable shstrtab;
    auto const shsInterp   = shstrtab.add(".interp");
    auto const shsText     = shstrtab.add(".text");
    auto const shsPlt      = shstrtab.add(".plt");
    auto const shsDynsym   = shstrtab.add(".dynsym");
    auto const shsDynstr   = shstrtab.add(".dynstr");
    auto const shsHash     = shstrtab.add(".hash");
    auto const shsRelaDyn  = shstrtab.add(".rela.dyn");
    auto const shsGot      = shstrtab.add(".got");
    auto const shsDynamic  = shstrtab.add(".dynamic");
    auto const shsSymtab   = shstrtab.add(".symtab");
    auto const shsStrtab   = shstrtab.add(".strtab");
    auto const shsShStrtab = shstrtab.add(".shstrtab");

    std::uint64_t const symtabOff = roundUp(ptLoad2End, 8);
    std::uint64_t const symtabSz  = symtab.size();
    std::uint64_t const strtabOff = symtabOff + symtabSz;
    std::uint64_t const strtabSz  = strtab.size();
    std::uint64_t const shstrtabOff = strtabOff + strtabSz;
    std::uint64_t const shstrtabSz  = shstrtab.size();
    std::uint64_t const shtOff = roundUp(shstrtabOff + shstrtabSz, 8);

    // ── (m) Apply intra-module + extern relocations.
    //
    // Pre-flight: every reloc kind must be declared on the format
    // schema (symmetric with ET_REL + PE — silent-failure C3
    // convergence: kernel only checks target-schema row).
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (fmt.relocationByKind(rel.kind) == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"elf::encodeElfExecDynamic: kind "} +
                     std::to_string(rel.kind.v) +
                     " not declared by ELF format '" +
                     std::string{fmt.name()} +
                     "' — substrate-invariant violation.");
                return {};
            }
        }
    }
    // symbolVa[intra] = textVa + funcOffset; symbolVa[extern] =
    // PLT stub VA. The shared kernel patches REL32 calls
    // (call rel32) → PLT stub for externs, → other function for
    // intra-module. PLT stub then jumps through GOT to resolved fn.
    // emplace().second checked so an extern SymbolId that collides
    // with an intra-module function symbol fails loud (silent-
    // failure C2 convergence — same defect exists in PE walker
    // and is anchored for parallel fold).
    std::unordered_map<SymbolId, std::uint64_t> symbolVa;
    symbolVa.reserve(module.functions.size() + numExterns);
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        symbolVa.emplace(module.functions[i].symbol,
                         textVa + funcTextStart[i]);
    }
    for (std::size_t i = 0; i < numExterns; ++i) {
        auto const [it, inserted] = symbolVa.emplace(
            module.externImports[i].symbol, pltVa + i * 6);
        if (!inserted) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"elf::encodeElfExecDynamic: extern "
                             "symbol #"} +
                 std::to_string(module.externImports[i].symbol.v) +
                 " ('" + module.externImports[i].mangledName +
                 "') collides with an intra-module function symbol "
                 "— an extern declared as a local definition would "
                 "silently bypass dyld resolution.");
            return {};
        }
    }
    if (!link::format::applyExecRelocations(
            text, module, funcTextStart, symbolVa,
            targetSchema, textVa,
            "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }

    // ── (n) Emit bytes ────────────────────────────────────────
    std::vector<std::uint8_t> bytes;
    bytes.reserve(shtOff + 13 * 64);

    // Ehdr at [0..64] — fill at end (need shtOff first); start with 64 zero bytes.
    bytes.resize(kEhdrSize, 0);

    // Program Header Table at [64..64+phtSize].
    auto appendPhdrEntry = [&](std::uint32_t pType,
                                std::uint32_t pFlags,
                                std::uint64_t pOffset,
                                std::uint64_t pVaddr,
                                std::uint64_t pFilesz,
                                std::uint64_t pMemsz,
                                std::uint64_t pAlign) {
        appendU32LE(bytes, pType);
        appendU32LE(bytes, pFlags);
        appendU64LE(bytes, pOffset);
        appendU64LE(bytes, pVaddr);
        appendU64LE(bytes, pVaddr);   // p_paddr
        appendU64LE(bytes, pFilesz);
        appendU64LE(bytes, pMemsz);
        appendU64LE(bytes, pAlign);
    };
    appendPhdrEntry(PT_PHDR,    PF_R,        phtOff,       baseImageVa + phtOff,
                    phtSize,        phtSize,        8);
    appendPhdrEntry(PT_INTERP,  PF_R,        interpOff,    interpVa,
                    interp.size(), interp.size(), 1);
    // PT_LOAD #1 R+X — Ehdr + PHT + .interp + .text + .plt + .dynsym
    //                  + .dynstr + .hash + .rela.dyn
    appendPhdrEntry(PT_LOAD,    PF_X | PF_R, 0,            baseImageVa,
                    ptLoad1End,    ptLoad1End,    pageAlign);
    // PT_LOAD #2 R+W — .got + .dynamic
    appendPhdrEntry(PT_LOAD,    PF_W | PF_R, ptLoad2Start, ptLoad2VaStart,
                    ptLoad2Size,   ptLoad2Size,   pageAlign);
    appendPhdrEntry(PT_DYNAMIC, PF_W | PF_R, dynamicOff,   dynamicVa,
                    dynamicSz,     dynamicSz,     8);

    // Section bodies: pad-to-offset + append per section (simplifier
    // #1 + #4 fold using local padToOffset / appendBytes helpers).
    padToOffset(bytes, interpOff);    appendBytes(bytes, interp);
    padToOffset(bytes, textOff);      appendBytes(bytes, text);
    padToOffset(bytes, pltOff);       appendBytes(bytes, plt);
    padToOffset(bytes, dynsymOff);    appendBytes(bytes, dynsym);
                                       appendBytes(bytes, dynstr);  // align 1
    padToOffset(bytes, hashOff);      appendBytes(bytes, hashSec);
    padToOffset(bytes, relaDynOff);   appendBytes(bytes, relaDyn);
    padToOffset(bytes, ptLoad2Start);                                // PT_LOAD #2 boundary
                                       appendBytes(bytes, got);
    padToOffset(bytes, dynamicOff);   appendBytes(bytes, dynamicSec);
    padToOffset(bytes, symtabOff);    appendBytes(bytes, symtab);
                                       appendBytes(bytes, strtab.view());
                                       appendBytes(bytes, shstrtab.view());
    padToOffset(bytes, shtOff);

    // ── (o) Section Header Table — 13 entries ─────────────────
    //   0: SHT_NULL, 1: .interp, 2: .text, 3: .plt,
    //   4: .dynsym, 5: .dynstr, 6: .hash, 7: .rela.dyn,
    //   8: .got, 9: .dynamic, 10: .symtab, 11: .strtab, 12: .shstrtab
    constexpr std::uint16_t IDX_INTERP   = 1;
    constexpr std::uint16_t IDX_TEXT     = 2;
    constexpr std::uint16_t IDX_DYNSYM   = 4;
    constexpr std::uint16_t IDX_DYNSTR   = 5;
    constexpr std::uint16_t IDX_STRTAB   = 11;
    constexpr std::uint16_t IDX_SHSTRTAB = 12;
    constexpr std::uint16_t kNumSections = 13;
    (void)IDX_INTERP; (void)IDX_TEXT;

    // Designated initializers per `SectionHeader` field — type-design
    // #1 + simplifier #3 fold: 10-arg positional pushShdr lambda
    // dropped (silent u32/u64 swap-bug surface) in favor of named
    // fields at every call site.
    writeSectionHeader(bytes, SectionHeader{});  // SHT_NULL (slot 0)
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsInterp, .type = SHT_PROGBITS, .flags = SHF_ALLOC,
        .addr = interpVa, .offset = interpOff, .size = interp.size(),
        .addr_align = 1});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsText, .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_EXECINSTR,
        .addr = textVa, .offset = textOff, .size = text.size(),
        .addr_align = 16});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsPlt, .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_EXECINSTR,
        .addr = pltVa, .offset = pltOff, .size = pltSize,
        .addr_align = 16});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsDynsym, .type = SHT_DYNSYM, .flags = SHF_ALLOC,
        .addr = dynsymVa, .offset = dynsymOff, .size = dynsymSz,
        .link = IDX_DYNSTR, .info = 1 /*first non-local symtab idx*/,
        .addr_align = 8, .entry_size = 24});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsDynstr, .type = SHT_STRTAB, .flags = SHF_ALLOC,
        .addr = dynstrVa, .offset = dynstrOff, .size = dynstrSz,
        .addr_align = 1});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsHash, .type = SHT_HASH, .flags = SHF_ALLOC,
        .addr = hashVa, .offset = hashOff, .size = hashSz,
        .link = IDX_DYNSYM, .addr_align = 8, .entry_size = 4});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsRelaDyn, .type = SHT_RELA, .flags = SHF_ALLOC,
        .addr = relaDynVa, .offset = relaDynOff, .size = relaDynSz,
        .link = IDX_DYNSYM, .addr_align = 8, .entry_size = 24});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsGot, .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_WRITE,
        .addr = gotVa, .offset = gotOff, .size = gotSz,
        .addr_align = 8, .entry_size = 8});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsDynamic, .type = SHT_DYNAMIC, .flags = SHF_ALLOC | SHF_WRITE,
        .addr = dynamicVa, .offset = dynamicOff, .size = dynamicSz,
        .link = IDX_DYNSTR, .addr_align = 8, .entry_size = 16});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsSymtab, .type = SHT_SYMTAB,
        .offset = symtabOff, .size = symtabSz,
        .link = IDX_STRTAB, .info = firstNonLocal,
        .addr_align = 8, .entry_size = 24});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsStrtab, .type = SHT_STRTAB,
        .offset = strtabOff, .size = strtabSz, .addr_align = 1});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsShStrtab, .type = SHT_STRTAB,
        .offset = shstrtabOff, .size = shstrtabSz, .addr_align = 1});

    // ── (p) Fill in Elf64_Ehdr ─────────────────────────────────
    //
    // Resolve entry function index from fmt.entryPoint() —
    // symmetric with the existing ET_EXEC arm (silent-failure
    // HIGH-1 fold from LK1 cycle 2; code-reviewer #1 regression
    // catch on LK6 cycle 2b.2 review). Empty entryPoint defaults
    // to functions[0]; non-empty resolves by synthesized
    // `sym_<id>` name today (real-name resolution closes with
    // D-LK1-1 / LK7).
    std::size_t entryFnIdx = 0;
    if (auto const ep = fmt.entryPoint(); !ep.empty()) {
        bool found = false;
        for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
            std::string const synthesized =
                "sym_" + std::to_string(module.functions[fi].symbol.v);
            if (synthesized == ep) {
                entryFnIdx = fi;
                found = true;
                break;
            }
        }
        if (!found) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"elf::encodeElfExecDynamic: entryPoint '"}
                     + std::string{ep}
                     + "' does not match any function in the module. "
                       "Leave entryPoint empty to default to the "
                       "first function (D-LK1-1 carries real names).");
            return {};
        }
    }
    std::uint64_t const entryVa = textVa + funcTextStart[entryFnIdx];
    std::vector<std::uint8_t> ehdr;
    ehdr.reserve(kEhdrSize);
    ehdr.push_back(0x7F); ehdr.push_back('E');
    ehdr.push_back('L');  ehdr.push_back('F');
    ehdr.push_back(elfId.fileClass);
    ehdr.push_back(elfId.dataEncoding);
    ehdr.push_back(static_cast<std::uint8_t>(EV_CURRENT));
    ehdr.push_back(elfId.osabi);
    ehdr.push_back(elfId.abiVersion);
    for (int i = 0; i < 7; ++i) ehdr.push_back(0);
    appendU16LE(ehdr, 2);  // ET_EXEC
    appendU16LE(ehdr, elfId.machine);
    appendU32LE(ehdr, EV_CURRENT);
    appendU64LE(ehdr, entryVa);
    appendU64LE(ehdr, phtOff);
    appendU64LE(ehdr, shtOff);
    appendU32LE(ehdr, 0);  // e_flags
    appendU16LE(ehdr, static_cast<std::uint16_t>(kEhdrSize));
    appendU16LE(ehdr, static_cast<std::uint16_t>(kPhdrSize));
    appendU16LE(ehdr, static_cast<std::uint16_t>(kNumPhdrs));
    appendU16LE(ehdr, 64);  // sizeof(Elf64_Shdr)
    appendU16LE(ehdr, kNumSections);
    appendU16LE(ehdr, IDX_SHSTRTAB);
    std::memcpy(bytes.data(), ehdr.data(), kEhdrSize);

    return bytes;
}

} // namespace

std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter) {
    // `targetSchema` is consumed by the ET_EXEC reloc-application
    // path (LK6 cycle 1) — the structured formula on each
    // `TargetRelocationInfo` (pcRelative + addendBias + widthBytes)
    // tells the walker how to compute and write patches. ET_REL
    // mode emits Rela records and doesn't need it.

    auto const& fmt = objectFormatSchema;
    if (fmt.kind() != ObjectFormatKind::Elf) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"elf::encode called with non-ELF format '"}
                 + std::string{fmt.name()}
                 + "' (kind="
                 + std::string{objectFormatKindName(fmt.kind())}
                 + ")");
        return {};
    }
    // ELF dynamic-linker import-table emission — substrate landed
    // at LK6 cycle 2b.1 (PT_INTERP path field); walker emission
    // closed at LK6 cycle 2b.2 (this dispatch — D-LK6-4 closed).
    // When the module carries externImports, route to
    // `encodeElfExecDynamic` for the full image with .interp +
    // .dynsym + .dynstr + .hash + .rela.dyn + .plt + .got +
    // .dynamic + PT_INTERP + PT_DYNAMIC + writable PT_LOAD #2.
    // Eager binding (DF_1_NOW + R_X86_64_GLOB_DAT) — lazy binding
    // is anchored separately (architect Obs 2 future cycle).
    if (!module.externImports.empty()) {
        bool const isExecEarly =
            (fmt.elf().objectType == ElfObjectType::Exec);
        if (!isExecEarly) {
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 std::string{"elf::encode: extern imports present ("}
                     + std::to_string(module.externImports.size())
                     + " entries) but format is ET_REL. Externs flow "
                       "to the linker only via ET_EXEC / ET_DYN.");
            return {};
        }
        if (fmt.elf().interpreter.empty()) {
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 std::string{"elf::encode: extern imports present ("}
                     + std::to_string(module.externImports.size())
                     + " entries) but `elf.interpreter` is empty — "
                       "declare a PT_INTERP path on the schema (e.g. "
                       "'/lib64/ld-linux-x86-64.so.2') for a loadable "
                       "dynamic image.");
            return {};
        }
        // Section schema lookup mirrors the existing path so the
        // dynamic helper inherits the same K_NoMatchingObjectFormat
        // guard.
        auto const* secTextDyn =
            requireSection(fmt, SectionKind::Text, "ELF dynamic writer",
                           reporter);
        if (!secTextDyn) return {};
        return encodeElfExecDynamic(module, targetSchema, fmt,
                                     *secTextDyn, reporter);
    }

    // Route between ET_REL and ET_EXEC based on the schema's
    // declared objectType. ET_REL keeps its .rela.text-bearing
    // layout; ET_EXEC applies relocations in-place to `.text`
    // (LK6 cycle 1 — see `applyExecRelocations` below) and adds a
    // PT_LOAD program header.
    bool const isExec = (fmt.elf().objectType == ElfObjectType::Exec);

    // Resolve the section schema rows we need. ET_REL requires
    // RelocTable (.rela.text); ET_EXEC doesn't (relocs are either
    // applied or rejected). Both modes share text/symtab/strtab/
    // shstrtab.
    auto const* secText      = requireSection(fmt, SectionKind::Text,      "ELF writer", reporter);
    auto const* secRela      = isExec ? nullptr
                                       : requireSection(fmt, SectionKind::RelocTable, "ELF writer", reporter);
    auto const* secSymtab    = requireSection(fmt, SectionKind::Symtab,    "ELF writer", reporter);
    auto const* secStrtab    = requireSection(fmt, SectionKind::Strtab,    "ELF writer", reporter);
    auto const* secShStrtab  = requireSection(fmt, SectionKind::ShStrtab,  "ELF writer", reporter);
    if (!secText || (!isExec && !secRela) || !secSymtab || !secStrtab || !secShStrtab) {
        return {};
    }

    // ── Build .text + per-function symbols ─────────────────────
    //
    // Concatenate every AssembledFunction's bytes into one .text
    // section, recording each function's start offset for its
    // symbol's `st_value`.
    std::vector<std::uint8_t> text;
    struct FuncSymRecord {
        SymbolId     symId{};
        std::uint64_t valueInText = 0;
        std::uint64_t size = 0;
    };
    std::vector<FuncSymRecord> funcSyms;
    funcSyms.reserve(module.functions.size());

    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());

    for (auto const& fn : module.functions) {
        std::uint64_t const start = text.size();
        funcTextStart.push_back(start);
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
        funcSyms.push_back({fn.symbol, start,
                            static_cast<std::uint64_t>(fn.bytes.size())});
    }

    // ── ET_EXEC: apply intra-module relocations in-place ───────
    //
    // Delegated to the shared `applyExecRelocations` kernel in
    // `link/format/exec_reloc_apply.hpp` — same helper consumed by
    // PE PE32+ and Mach-O MH_EXECUTE walkers. Format-specific input
    // is the `sectionVa` (ELF: `secText->virtualAddress`) and the
    // diagnostic prefix.
    if (isExec) {
        // Build absolute symbol-VA map: for every function, its
        // runtime VA is `secText->virtualAddress + offsetInText`.
        // ELF cycle 2a has no extern imports (anchored D-LK6-4);
        // when externs land, they extend this same map with GOT
        // / PLT slot VAs.
        std::unordered_map<SymbolId, std::uint64_t> symbolVa;
        symbolVa.reserve(module.functions.size());
        for (std::size_t i = 0; i < module.functions.size(); ++i) {
            symbolVa.emplace(module.functions[i].symbol,
                             secText->virtualAddress + funcTextStart[i]);
        }
        if (!link::format::applyExecRelocations(
                text, module, funcTextStart, symbolVa,
                targetSchema, secText->virtualAddress,
                "elf::encode (ET_EXEC)", reporter)) {
            return {};
        }
    }

    // ── Build .strtab + .symtab ────────────────────────────────
    //
    // Symbol layout: STN_UNDEF (idx 0) → STT_SECTION for .text
    // (LOCAL) → defined function symbols (GLOBAL) → undefined extern
    // symbols (GLOBAL, SHN_UNDEF). `.symtab.sh_info` = index of
    // first non-LOCAL symbol.

    StringTable strtab;
    std::vector<std::uint8_t> symtab;

    // Helper: emit one Elf64_Sym record (24 bytes).
    auto appendSym = [&](std::uint32_t nameOff, std::uint8_t info,
                          std::uint8_t other, std::uint16_t shndx,
                          std::uint64_t value, std::uint64_t size) {
        appendU32LE(symtab, nameOff);
        appendU8(symtab, info);
        appendU8(symtab, other);
        appendU16LE(symtab, shndx);
        appendU64LE(symtab, value);
        appendU64LE(symtab, size);
    };

    // Symbol index 0: STN_UNDEF (24 zero bytes).
    appendSym(0, 0, 0, 0, 0, 0);

    // Index 1: STT_SECTION for .text — the relocation base most
    // ELF consumers expect for section-relative references. Its
    // st_shndx points at the .text section index; we pin that to
    // IDX_TEXT (=1) by the section ordering below.
    appendSym(0, makeStInfo(STB_LOCAL, STT_SECTION),
              0, /*shndx=.text*/ 1, 0, 0);
    std::uint32_t const firstNonLocalSymIdx = 2;

    // Map each function's SymbolId to its symtab index (for relocs).
    std::unordered_map<SymbolId, std::uint32_t> symIdxBySymbol;

    // Defined function symbols (GLOBAL + STT_FUNC + shndx=.text).
    for (auto const& f : funcSyms) {
        std::string const symName =
            std::string{"sym_"} + std::to_string(f.symId.v);
        std::uint32_t const nameOff = strtab.add(symName);
        std::uint32_t const idx =
            static_cast<std::uint32_t>(symtab.size() / 24);
        appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_FUNC), 0,
                  /*shndx=.text*/ 1, f.valueInText, f.size);
        symIdxBySymbol.emplace(f.symId, idx);
    }

    // Undefined extern symbols referenced by any relocation but not
    // defined by any function. ET_EXEC has no extern symbols at this
    // point — the cycle-1 reloc-application pass above failed loud on
    // any unresolved target (FFI / dynamic linking is LK6 cycle 2).
    if (!isExec) {
        for (auto const& fn : module.functions) {
            for (auto const& rel : fn.relocations) {
                if (symIdxBySymbol.contains(rel.target)) continue;
                std::string const symName =
                    std::string{"sym_"} + std::to_string(rel.target.v);
                std::uint32_t const nameOff = strtab.add(symName);
                std::uint32_t const idx =
                    static_cast<std::uint32_t>(symtab.size() / 24);
                appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_NOTYPE), 0,
                          SHN_UNDEF, 0, 0);
                symIdxBySymbol.emplace(rel.target, idx);
            }
        }
    }

    // ── Build .rela.text ───────────────────────────────────────
    //
    // For each AssembledFunction's relocations, compute the absolute
    // offset within .text (the function's range start + the local
    // offset) and translate `kind → nativeId` via the format schema.
    // ET_EXEC applies relocations in-place (above) and emits no
    // .rela.text — the build loop is skipped entirely.

    std::vector<std::uint8_t> relaText;
    if (!isExec) {
        for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
            auto const& fn = module.functions[fi];
            std::uint64_t const fnStart = funcTextStart[fi];
            for (auto const& rel : fn.relocations) {
                auto const* fmtReloc = fmt.relocationByKind(rel.kind);
                if (fmtReloc == nullptr) {
                    // Should have been caught by `link()` substrate
                    // but re-check defensively — silent skip is
                    // exactly the failure class the substrate
                    // discipline rejects.
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         "elf::encode: relocation kind "
                             + std::to_string(rel.kind.v)
                             + " not declared by ELF format '"
                             + std::string{fmt.name()} + "'");
                    continue;
                }
                auto it = symIdxBySymbol.find(rel.target);
                if (it == symIdxBySymbol.end()) {
                    emit(reporter, DiagnosticCode::K_SymbolUndefined,
                         "elf::encode: relocation target symbol #"
                             + std::to_string(rel.target.v)
                             + " has no symtab entry");
                    continue;
                }
                std::uint32_t const symIdx = it->second;
                std::uint64_t const rOffset = fnStart + rel.offset;
                appendU64LE(relaText, rOffset);
                appendU64LE(relaText, makeRelaInfo(symIdx, fmtReloc->nativeId));
                appendI64LE(relaText, rel.addend);
            }
        }
    }

    // ── Section ordering + .shstrtab ───────────────────────────
    //
    // ET_REL order: SHT_NULL, .text, .rela.text, .symtab, .strtab,
    // .shstrtab. ET_EXEC drops `.rela.text` entirely (no SHT_NULL
    // placeholder): intra-module relocations were applied in-place
    // to `.text` by `applyExecRelocations` above (LK6 cycle 1,
    // closes D-LK1-3); extern relocs (FFI / dynamic linking) are
    // anchored at D-LK6-2 and don't reach this point.
    StringTable shstrtab;
    SectionHeader hNull{};
    SectionHeader hText{};
    SectionHeader hRela{};
    SectionHeader hSymtab{};
    SectionHeader hStrtab{};
    SectionHeader hShStrtab{};
    hText.name_offset      = shstrtab.add(secText->name);
    if (secRela != nullptr) {
        hRela.name_offset  = shstrtab.add(secRela->name);
    }
    hSymtab.name_offset    = shstrtab.add(secSymtab->name);
    hStrtab.name_offset    = shstrtab.add(secStrtab->name);
    hShStrtab.name_offset  = shstrtab.add(secShStrtab->name);

    // Section indices — IDX_TEXT==1 is pinned (the STT_SECTION sym
    // emitted above hardcodes st_shndx=1). Other indices depend on
    // whether the `.rela.text` slot is present:
    //   ET_REL:  Null(0), Text(1), Rela(2), Symtab(3), Strtab(4), ShStrtab(5).
    //   ET_EXEC: Null(0), Text(1), Symtab(2), Strtab(3), ShStrtab(4).
    // The phantom SHT_NULL placeholder in ET_EXEC was an LK1-cycle-2
    // first draft; architect convergence pulled it out so `readelf
    // -S` doesn't show a blank slot at idx 2 and the index math is
    // honest.
    constexpr std::uint16_t IDX_TEXT     = 1;
    std::uint16_t const IDX_SYMTAB   = isExec ? 2u : 3u;
    std::uint16_t const IDX_STRTAB   = isExec ? 3u : 4u;
    std::uint16_t const IDX_SHSTRTAB = isExec ? 4u : 5u;

    hText.type       = secText->type;
    hText.flags      = secText->flags;
    hText.addr_align = secText->addrAlign;
    hText.entry_size = secText->entrySize;
    hText.size       = text.size();
    // sh_addr — ET_EXEC fills from schema's virtualAddress; ET_REL
    // leaves it 0 (unbound in .o).
    hText.addr       = isExec ? secText->virtualAddress : 0;

    if (secRela != nullptr) {
        hRela.type       = secRela->type;
        hRela.flags      = secRela->flags;
        hRela.addr_align = secRela->addrAlign;
        hRela.entry_size = secRela->entrySize;  // 24 for Elf64_Rela
        hRela.link       = IDX_SYMTAB;
        hRela.info       = IDX_TEXT;
        hRela.size       = relaText.size();
    } else {
        // ET_EXEC: slot remains SHT_NULL (all zeros). Section index
        // stays in the header table to preserve IDX_* parity.
    }

    hSymtab.type       = secSymtab->type;
    hSymtab.flags      = secSymtab->flags;
    hSymtab.addr_align = secSymtab->addrAlign;
    hSymtab.entry_size = secSymtab->entrySize;  // 24 for Elf64_Sym
    hSymtab.link       = IDX_STRTAB;
    hSymtab.info       = firstNonLocalSymIdx;
    hSymtab.size       = symtab.size();

    hStrtab.type       = secStrtab->type;
    hStrtab.flags      = secStrtab->flags;
    hStrtab.addr_align = std::max<std::uint64_t>(1, secStrtab->addrAlign);
    hStrtab.entry_size = secStrtab->entrySize;
    hStrtab.size       = strtab.size();

    hShStrtab.type       = secShStrtab->type;
    hShStrtab.flags      = secShStrtab->flags;
    hShStrtab.addr_align = std::max<std::uint64_t>(1, secShStrtab->addrAlign);
    hShStrtab.entry_size = secShStrtab->entrySize;
    hShStrtab.size       = shstrtab.size();

    // ── Layout pass: compute sh_offset for each section ────────
    //
    // ET_REL: [Ehdr] + section bodies + SHT at end.
    // ET_EXEC: [Ehdr] + [PHT (program headers)] + section bodies +
    //          SHT at end. The PHT lives immediately after the Ehdr
    //          so e_phoff = 64 and runtime loaders find it without
    //          a seek.
    constexpr std::uint64_t kEhdrSize = 64;
    constexpr std::uint64_t kProgramHeaderSize = 56;  // Elf64_Phdr
    constexpr std::uint64_t kPtLoadCount = 1;         // cycle-2: just .text
    std::uint64_t const phtSize = isExec ? (kPtLoadCount * kProgramHeaderSize) : 0;

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kEhdrSize + phtSize + text.size() + relaText.size()
                  + symtab.size() + strtab.size() + shstrtab.size() + 6 * 64);
    bytes.resize(kEhdrSize + phtSize);  // placeholder; rewritten below

    // Single layout lambda — `vector<uint8_t> const&` decays to
    // `span<uint8_t const>` so both the in-memory section bodies
    // (text / relaText / symtab) and the StringTable views share
    // one code path.
    auto layoutSection = [&](SectionHeader& h, std::span<std::uint8_t const> body) {
        if (h.addr_align > 1) padTo(bytes, h.addr_align);
        h.offset = bytes.size();
        bytes.insert(bytes.end(), body.begin(), body.end());
    };

    // For ET_EXEC: pad .text's file offset up to the PT_LOAD page
    // alignment declared on the format schema (`fmt.elf().pageAlign`,
    // e.g. 0x1000 for x86_64 Linux / ARM64-4K, 0x4000 for Apple
    // Silicon Asahi, 0x10000 for ARM64-64K). The Linux kernel
    // enforces `p_vaddr % p_align == p_offset % p_align` on every
    // PT_LOAD — execve() fails with ENOEXEC if violated (silent
    // from the toolchain's POV). Cycle 1 ET_REL doesn't have
    // program headers and is unaffected. `validate()` requires
    // non-zero `pageAlign` for Exec (D-LK6-3) so the field is
    // never absent here.
    std::uint64_t const pageAlign = fmt.elf().pageAlign;
    if (isExec) padTo(bytes, pageAlign);
    layoutSection(hText, text);
    if (secRela != nullptr) layoutSection(hRela, relaText);
    layoutSection(hSymtab, symtab);
    layoutSection(hStrtab, strtab.view());
    layoutSection(hShStrtab, shstrtab.view());

    padTo(bytes, 8);  // SHT alignment
    std::uint64_t const shoff = bytes.size();
    // Single source of truth for the section count so future cycles
    // (LK1 ELF executable adding .data/.rodata/.bss, LK6 dynamic
    // linking adding .dynamic/.dynsym/.dynstr) cannot drift between
    // the Ehdr's e_shnum and the actual table size.
    // ET_REL keeps `.rela.text` in slot 2; ET_EXEC drops it entirely
    // (no SHT_NULL placeholder). Section count derives from the
    // actually-emitted slots — same architect B-LK1-2 / D-LK2-5
    // discipline that LK1 cycle 1 + LK2 already adopt.
    std::vector<SectionHeader const*> headers;
    headers.reserve(6);
    headers.push_back(&hNull);
    headers.push_back(&hText);
    if (!isExec) headers.push_back(&hRela);
    headers.push_back(&hSymtab);
    headers.push_back(&hStrtab);
    headers.push_back(&hShStrtab);
    std::uint16_t const sectionCount =
        static_cast<std::uint16_t>(headers.size());
    for (auto const* h : headers) writeSectionHeader(bytes, *h);

    // ── Elf64_Ehdr (overwrite the leading 64 zero bytes) ───────
    //
    // ET_REL: e_type = 1, e_entry = 0, e_phoff = 0, e_phnum = 0.
    // ET_EXEC: e_type = 2, e_entry = virtualAddress + entry-fn
    // offset (cycle 2: entry function = module.functions[0] at
    // offset 0 in .text), e_phoff = sizeof(Ehdr) = 64, e_phnum = 1.
    auto const& id = fmt.elf();
    std::uint16_t const eType =
        static_cast<std::uint16_t>(id.objectType);
    std::uint64_t eEntry = 0;
    std::uint64_t ePhoff = 0;
    std::uint16_t ePhnum = 0;
    std::uint16_t ePhentsize = 0;
    if (isExec) {
        // e_entry: virtual address of the entry instruction.
        //
        // Resolution order:
        //   * Empty `entryPoint`: use the first function. Cycle-2
        //     default convention until real names land.
        //   * Non-empty `entryPoint`: look up the named function in
        //     the module. Function names are synthesized as
        //     `sym_<id>` today (D-LK1-1 anchored — real names from
        //     HIR→LIR→AssembledFunction land at LK7); the lookup
        //     accepts that synthesized form. Unknown name = fail
        //     loud K_SymbolUndefined.
        if (module.functions.empty()) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 "ELF ET_EXEC writer: cannot derive e_entry — the "
                 "AssembledModule has zero functions; ET_EXEC requires "
                 "at least one function to serve as the entry point");
            return {};
        }
        std::size_t entryFnIdx = 0;
        auto const ep = fmt.entryPoint();
        if (!ep.empty()) {
            bool found = false;
            for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
                std::string const synthesized =
                    "sym_" + std::to_string(module.functions[fi].symbol.v);
                if (synthesized == ep) {
                    entryFnIdx = fi;
                    found = true;
                    break;
                }
            }
            if (!found) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::string{"ELF ET_EXEC writer: entryPoint '"}
                         + std::string{ep}
                         + "' does not match any function in the module. "
                           "Function names are synthesized as "
                           "'sym_<symbolId.v>' today (real names arrive "
                           "with D-LK1-1 / LK7). Leave entryPoint empty "
                           "to default to the first function.");
                return {};
            }
        }
        std::uint64_t const entryOffsetInText = funcTextStart[entryFnIdx];
        eEntry = secText->virtualAddress + entryOffsetInText;
        ePhoff = kEhdrSize;
        ePhnum = static_cast<std::uint16_t>(kPtLoadCount);
        ePhentsize = static_cast<std::uint16_t>(kProgramHeaderSize);
    }

    std::vector<std::uint8_t> ehdr;
    ehdr.reserve(kEhdrSize);
    // e_ident
    ehdr.push_back(0x7F); ehdr.push_back('E');
    ehdr.push_back('L');  ehdr.push_back('F');
    ehdr.push_back(id.fileClass);
    ehdr.push_back(id.dataEncoding);
    ehdr.push_back(static_cast<std::uint8_t>(EV_CURRENT));
    ehdr.push_back(id.osabi);
    ehdr.push_back(id.abiVersion);
    for (int i = 0; i < 7; ++i) ehdr.push_back(0);  // EI_PAD
    // e_type, e_machine, e_version
    appendU16LE(ehdr, eType);
    appendU16LE(ehdr, id.machine);
    appendU32LE(ehdr, EV_CURRENT);
    // e_entry, e_phoff, e_shoff
    appendU64LE(ehdr, eEntry);
    appendU64LE(ehdr, ePhoff);
    appendU64LE(ehdr, shoff);
    // e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx
    appendU32LE(ehdr, 0);
    appendU16LE(ehdr, static_cast<std::uint16_t>(kEhdrSize));
    appendU16LE(ehdr, ePhentsize);
    appendU16LE(ehdr, ePhnum);
    appendU16LE(ehdr, 64);  // sizeof(Elf64_Shdr)
    appendU16LE(ehdr, sectionCount);
    appendU16LE(ehdr, IDX_SHSTRTAB);
    std::memcpy(bytes.data(), ehdr.data(), kEhdrSize);

    // ── PT_LOAD program header (ET_EXEC only) ──────────────────
    //
    // One PT_LOAD covering the .text region with R|X permissions.
    // ET_EXEC requires at least one PT_LOAD; the runtime loader
    // uses it to map the segment into the process address space.
    // PT_PHDR (pointing at the program header table itself) is
    // conventional but optional for STATIC executables — Linux's
    // kernel ELF loader accepts ET_EXEC without it. **PT_PHDR
    // becomes REQUIRED as soon as PT_INTERP appears**: the dynamic
    // loader uses PT_PHDR to locate the program headers in the
    // mapped process image (otherwise it has no way to find them).
    // Cycle 2 ships only PT_LOAD; PT_PHDR / PT_INTERP / PT_DYNAMIC
    // arrive together with LK6 dynamic linking.
    if (isExec) {
        std::vector<std::uint8_t> phdr;
        phdr.reserve(kProgramHeaderSize);
        // p_type = PT_LOAD = 1
        appendU32LE(phdr, 1);
        // p_flags = PF_X | PF_R = 5 (.text is executable + readable)
        appendU32LE(phdr, 5);
        // p_offset = file offset of .text
        appendU64LE(phdr, hText.offset);
        // p_vaddr / p_paddr = virtual address of .text
        appendU64LE(phdr, secText->virtualAddress);
        appendU64LE(phdr, secText->virtualAddress);
        // p_filesz / p_memsz = byte length of .text
        appendU64LE(phdr, text.size());
        appendU64LE(phdr, text.size());
        // p_align — declared by the format schema per (arch × OS).
        // See the `padTo(bytes, pageAlign)` call above; both must
        // use the SAME value or the kernel's congruence check
        // (p_vaddr % p_align == p_offset % p_align) fails.
        appendU64LE(phdr, pageAlign);
        std::memcpy(bytes.data() + kEhdrSize, phdr.data(),
                    kProgramHeaderSize);
    }

    return bytes;
}

} // namespace dss::elf
