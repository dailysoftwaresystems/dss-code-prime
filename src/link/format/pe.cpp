#include "link/format/pe.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"
#include "link/format/exec_reloc_apply.hpp"
#include "link/format/string_table.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// PE/COFF writer — plan 14 LK2 cycle 1 (.obj) + cycle 2 (PE32+ .exe).
//
// .obj byte layout (PE/COFF spec §3-5):
//   [0x00]   IMAGE_FILE_HEADER (20 B)
//   [0x14]   IMAGE_SECTION_HEADER × N (40 B each)
//   [...]    section raw data (.text first)
//   [...]    per-section IMAGE_RELOCATION[] (10 B packed each)
//   [ptr]    IMAGE_SYMBOL[] (18 B packed each)
//   [...]    String table (u32 size + NUL-terminated strings)
//
// PE32+ image (.exe) byte layout (PE/COFF §3.2-3.4):
//   [0x00]   IMAGE_DOS_HEADER (64 B) — `MZ` + e_lfanew → 0x80
//   [0x40]   MS-DOS stub (~64 B "This program cannot be run in DOS")
//   [0x80]   PE signature "PE\0\0" (4 B)
//   [0x84]   IMAGE_FILE_HEADER (20 B)
//   [0x98]   IMAGE_OPTIONAL_HEADER64 (240 B = 112 fixed + 16×8 data dirs)
//   [...]    IMAGE_SECTION_HEADER × N (40 B each)
//   [pad]    pad to fileAlignment (512)
//   [...]    section raw data (.text first, etc.) padded to fileAlignment
//
// The walker is target-blind in shape — every PE-specific number
// (machine, characteristics, section Characteristics, reloc
// nativeIds, section names, optional-header fields, ImageBase, etc.)
// is read from the format schema. The only hardcoded structural
// knowledge is the PE/COFF binary record layout.

namespace dss::pe {

namespace {

using lir_pass_util::report;
using link::format::detail::appendU8;
using link::format::detail::appendU16LE;
using link::format::detail::appendU32LE;
using link::format::detail::appendU64LE;
using link::format::detail::appendI16LE;
using link::format::detail::emit;
using link::format::detail::requireSection;

// ── PE/COFF constants (spec §5) ─────────────────────────────────

// IMAGE_SYM_CLASS_*
constexpr std::uint8_t IMAGE_SYM_CLASS_EXTERNAL = 2;
// IMAGE_SECTION_NUMBER specials
constexpr std::int16_t IMAGE_SYM_UNDEFINED = 0;
// IMAGE_SYM_TYPE_*
constexpr std::uint16_t IMAGE_SYM_DTYPE_FUNCTION = 0x20;

constexpr std::uint16_t kFileHeaderSize    = 20;
constexpr std::uint16_t kSectionHeaderSize = 40;
constexpr std::size_t   kSymbolRecordSize  = 18;
constexpr std::size_t   kRelocRecordSize   = 10;

// Byte-emit helpers + emit() + requireSection() now hoisted to
// `src/link/format/byte_emit.hpp` (simplifier fold-in #1+#3).

// ── PE/COFF name encoding ───────────────────────────────────────
//
// PE/COFF section names + symbol names have a dual encoding: names
// ≤ 8 chars are inlined into a fixed 8-byte field NUL-padded.
// Longer names use `[u32 zero][u32 offset]` (symbol form) where
// `offset` is the byte offset into the string table. Section names
// use `/N` ASCII-decimal offset.

struct NameField {
    std::array<std::uint8_t, 8> bytes{};
};

// Symbol name encoding: NUL-pad to 8 chars OR set offset form.
[[nodiscard]] NameField encodeSymbolName(std::string_view name,
                                          std::uint32_t strtabOffset) {
    NameField out;
    if (name.size() <= 8) {
        for (std::size_t i = 0; i < name.size(); ++i) {
            out.bytes[i] = static_cast<std::uint8_t>(name[i]);
        }
        // remaining bytes already zero
    } else {
        // First 4 bytes = 0; last 4 bytes = strtab offset LE
        for (int i = 0; i < 4; ++i) {
            out.bytes[4 + i] =
                static_cast<std::uint8_t>(strtabOffset >> (i * 8));
        }
    }
    return out;
}

// Section name encoding: `.text` fits in 8 chars; long names use
// `/N` ASCII-decimal offset.
[[nodiscard]] NameField encodeSectionName(std::string_view name,
                                           std::uint32_t strtabOffset) {
    NameField out;
    if (name.size() <= 8) {
        for (std::size_t i = 0; i < name.size(); ++i) {
            out.bytes[i] = static_cast<std::uint8_t>(name[i]);
        }
    } else {
        std::string const slashForm = "/" + std::to_string(strtabOffset);
        for (std::size_t i = 0; i < slashForm.size() && i < 8; ++i) {
            out.bytes[i] = static_cast<std::uint8_t>(slashForm[i]);
        }
    }
    return out;
}

// String-table builder hoisted to `src/link/format/string_table.hpp`
// (D-LK4-9 closure). PE uses the U32SizePrefix init: bytes 0..3 hold
// an inclusive u32 size prefix stamped at release() time. Smallest
// legal offset returned by `add()` is 4 (just past the size).
using link::format::detail::StringTable;

// ── Section header record (in-memory) ───────────────────────────

struct PeSectionHeader {
    NameField     name{};
    std::uint32_t virtualSize           = 0;
    std::uint32_t virtualAddress        = 0;
    std::uint32_t sizeOfRawData         = 0;
    std::uint32_t pointerToRawData      = 0;
    std::uint32_t pointerToRelocations  = 0;
    std::uint32_t pointerToLinenumbers  = 0;
    std::uint16_t numberOfRelocations   = 0;
    std::uint16_t numberOfLinenumbers   = 0;
    std::uint32_t characteristics       = 0;
};

void writeSectionHeader(std::vector<std::uint8_t>& out,
                         PeSectionHeader const& h) {
    for (auto b : h.name.bytes) appendU8(out, b);
    appendU32LE(out, h.virtualSize);
    appendU32LE(out, h.virtualAddress);
    appendU32LE(out, h.sizeOfRawData);
    appendU32LE(out, h.pointerToRawData);
    appendU32LE(out, h.pointerToRelocations);
    appendU32LE(out, h.pointerToLinenumbers);
    appendU16LE(out, h.numberOfRelocations);
    appendU16LE(out, h.numberOfLinenumbers);
    appendU32LE(out, h.characteristics);
}

} // namespace

// Forward declaration: EXEC arm lives below the Obj body so the
// .obj path (LK2 cycle 1) keeps its top-of-file position.
namespace {
[[nodiscard]] std::vector<std::uint8_t>
encodeExec(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& fmt,
           ObjectFormatSectionInfo const& secText,
           DiagnosticReporter&       reporter);
} // namespace

std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter) {
    auto const& fmt = objectFormatSchema;
    if (fmt.kind() != ObjectFormatKind::Pe) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"pe::encode called with non-PE format '"}
                 + std::string{fmt.name()}
                 + "' (kind="
                 + std::string{objectFormatKindName(fmt.kind())}
                 + ")");
        return {};
    }

    // PE/COFF has NO IMAGE_SECTION_HEADER for `symtab` / `strtab` —
    // the symbol table lives at `IMAGE_FILE_HEADER.PointerToSymbolTable`
    // and the string table immediately follows. The walker therefore
    // only REQUIRES `text`; `symtab` / `strtab` are looked up
    // optionally (never an error if a PE JSON omits them — architect
    // convergence on the prior cycle's spurious-failure trap).
    auto const* secText = requireSection(fmt, SectionKind::Text,
                                          "PE writer", reporter);
    if (!secText) return {};

    // Dispatch between .obj (Obj) and PE32+ image (Exec / Dll). The
    // two paths share only `secText` lookup + the schema kind check
    // — every other byte differs (.obj has no MS-DOS stub / PE sig /
    // optional header; image-side has no IMAGE_RELOCATION[] /
    // IMAGE_SYMBOL[] tables). validate() guarantees the optional
    // header is populated for Exec/Dll, so the EXEC arm doesn't
    // re-check those fields.
    if (fmt.pe().objectType != PeObjectType::Obj) {
        if (fmt.pe().objectType == PeObjectType::Dll) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 "pe::encode: PE .dll arm not yet implemented; "
                 "anchored at a future cycle paired with LK6 dynamic "
                 "linking (same shape as ELF ET_DYN's D-LK1-4).");
            return {};
        }
        return encodeExec(module, targetSchema, fmt, *secText, reporter);
    }
    (void)targetSchema;  // Obj path does not apply relocations — the
                         // assembler stamped the bytes and the .obj
                         // writer just serializes them.

    // ── Build .text + per-function symbols ─────────────────────
    std::vector<std::uint8_t> text;
    struct FuncSymRecord {
        SymbolId      symId{};
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

    // ── Build .text relocation table ───────────────────────────
    //
    // PE/COFF stores relocations per-section, immediately after the
    // section's raw data. Each IMAGE_RELOCATION is 10 bytes packed.

    // Index every symbol the writer will emit so the relocation
    // records can reference them by SymbolTableIndex.
    //
    // Symbol indices are minted strictly with `emplace.second` —
    // duplicates do not advance the index counter (silent-failure
    // H3 fix: prior version `nextSymIdx++` ran unconditionally,
    // desynchronizing the index map from the appended symtab).
    // O(1) lookup against an `unordered_set<SymbolId>` of defined
    // symbols replaces the prior O(n²) linear scans (simplifier
    // fold-in #5).
    std::unordered_map<SymbolId, std::uint32_t> symIdxBySymbol;
    symIdxBySymbol.reserve(module.functions.size() * 2);

    std::unordered_set<SymbolId> definedSet;
    definedSet.reserve(module.functions.size());
    for (auto const& f : funcSyms) definedSet.insert(f.symId);

    std::vector<SymbolId> externSyms;
    std::unordered_set<SymbolId> externSeen;
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (definedSet.contains(rel.target)) continue;
            // Externs become IMAGE_SYM_UNDEFINED entries (SectionNumber=0).
            if (externSeen.insert(rel.target).second) {
                externSyms.push_back(rel.target);
            }
        }
    }

    // Assign symbol-table indices: defined functions first (mirrors
    // LK1's discipline), then externs. PE doesn't require this
    // ordering but the ELF writer uses it and consistency simplifies
    // the test fixtures. Index advances ONLY when emplace succeeds.
    std::uint32_t nextSymIdx = 0;
    for (auto const& f : funcSyms) {
        auto const [it, fresh] =
            symIdxBySymbol.emplace(f.symId, nextSymIdx);
        if (!fresh) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"PE writer: duplicate defined symbol #"}
                     + std::to_string(f.symId.v)
                     + " (assembler emitted the same SymbolId twice "
                       "for distinct AssembledFunctions)");
            return {};
        }
        ++nextSymIdx;
    }
    for (auto const& e : externSyms) {
        if (symIdxBySymbol.emplace(e, nextSymIdx).second) ++nextSymIdx;
    }

    // Per-section relocation table (only `.text` has relocations
    // in this cycle scope).
    //
    // PE/COFF convention: the relocation addend lives IN THE PATCH
    // BYTES (the section's raw data), NOT as a separate field on
    // the IMAGE_RELOCATION record. ELF Rela carries `r_addend`
    // explicitly; PE has no such column. If an `AssembledModule`
    // arrives with `rel.addend != 0`, the PE walker would silently
    // drop the addend — that's exactly the silent-failure class the
    // substrate discipline rejects. Fail loud so the caller fixes
    // the assembler (or pre-stamps the addend into the patch bytes).
    std::vector<std::uint8_t> textRelocs;
    std::uint32_t textRelocCount = 0;
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        auto const& fn = module.functions[fi];
        std::uint64_t const fnStart = funcTextStart[fi];
        for (auto const& rel : fn.relocations) {
            if (rel.addend != 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"PE writer: relocation in symbol #"}
                         + std::to_string(fn.symbol.v)
                         + " carries addend=" + std::to_string(rel.addend)
                         + " but PE/COFF stores addends in the section's "
                           "patch bytes, not on IMAGE_RELOCATION. The "
                           "assembler must pre-stamp the addend into "
                           ".text (or emit addend=0 for the call/jmp "
                           "rel32 case where link.exe applies the RIP "
                           "bias intrinsically).");
                return {};
            }
            auto const* fmtReloc = fmt.relocationByKind(rel.kind);
            if (fmtReloc == nullptr) continue;  // unreachable: linker
                                                // engine pre-validated
            auto const it = symIdxBySymbol.find(rel.target);
            if (it == symIdxBySymbol.end()) continue;  // ditto
            std::uint32_t const va =
                static_cast<std::uint32_t>(fnStart + rel.offset);
            appendU32LE(textRelocs, va);
            appendU32LE(textRelocs, it->second);
            appendU16LE(textRelocs,
                static_cast<std::uint16_t>(fmtReloc->nativeId));
            ++textRelocCount;
        }
    }

    // Silent-failure C1 guard: PE/COFF spec §4 says when relocation
    // count > 65534, the writer must set IMAGE_SCN_LNK_NRELOC_OVFL
    // (0x01000000) in section Characteristics AND put the real count
    // in the first IMAGE_RELOCATION's VirtualAddress field. That
    // path is not implemented in this cycle; emit a hard diagnostic
    // rather than silently truncating to u16. Anchored at plan 14
    // §3.1 as a deferred item (overflow path arrives with the first
    // module that needs it).
    if (textRelocCount > 0xFFFEu) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"PE writer: relocation count "}
                 + std::to_string(textRelocCount)
                 + " exceeds u16 capacity; PE/COFF NRELOC_OVFL overflow "
                   "path is anchored as plan 14 §3.1 D-LK2-3 (LK6 trigger). "
                   "Module too large for cycle-1 PE writer.");
        return {};
    }

    // ── Build string table + symbol table records ─────────────
    //
    // String table starts pre-reserved with 4 bytes for its size
    // prefix; `add()` returns the offset (always ≥ 4). For each
    // symbol name ≤ 8 chars we inline; longer names get appended
    // to the string table.

    StringTable strtab{StringTable::Init::U32SizePrefix};
    std::vector<std::uint8_t> symtab;

    auto appendSym = [&](NameField const& nameField, std::uint32_t value,
                          std::int16_t sectionNumber, std::uint16_t type,
                          std::uint8_t storageClass,
                          std::uint8_t numAuxSymbols) {
        for (auto b : nameField.bytes) appendU8(symtab, b);
        appendU32LE(symtab, value);
        appendI16LE(symtab, sectionNumber);
        appendU16LE(symtab, type);
        appendU8(symtab, storageClass);
        appendU8(symtab, numAuxSymbols);
    };

    auto emitSymWithName = [&](std::string_view name, std::uint32_t value,
                                std::int16_t sectionNumber,
                                std::uint16_t type,
                                std::uint8_t storageClass) {
        std::uint32_t offset = 0;
        if (name.size() > 8) offset = strtab.add(name);
        appendSym(name.size() <= 8
                      ? encodeSymbolName(name, 0)
                      : encodeSymbolName(name, offset),
                  value, sectionNumber, type, storageClass, /*aux=*/0);
    };

    constexpr std::int16_t kTextSectionNumber = 1;

    // Defined function symbols (GLOBAL EXTERNAL, type=FUNCTION,
    // SectionNumber=1 for `.text`).
    for (auto const& f : funcSyms) {
        std::string const symName = "sym_" + std::to_string(f.symId.v);
        emitSymWithName(symName,
                        static_cast<std::uint32_t>(f.valueInText),
                        kTextSectionNumber,
                        IMAGE_SYM_DTYPE_FUNCTION,
                        IMAGE_SYM_CLASS_EXTERNAL);
    }
    // Undefined extern symbols (SectionNumber=0=UNDEF, type=0,
    // value=0).
    for (auto const& e : externSyms) {
        std::string const symName = "sym_" + std::to_string(e.v);
        emitSymWithName(symName, /*value=*/0, IMAGE_SYM_UNDEFINED,
                        /*type=*/0, IMAGE_SYM_CLASS_EXTERNAL);
    }

    // ── Layout the file: header → section headers → section data
    //    → per-section relocs → symbol table → string table ─────
    //
    // Section count is DERIVED from the section-header vector built
    // below (architect D-LK2-5 convergence): pre-fix hardcoded
    // literal `1` would silently corrupt the file when LK6 adds
    // .data/.rdata. Same fix that closed B-LK1-2 on the ELF side.
    std::vector<PeSectionHeader> sectionHeaders;
    std::size_t const kNumSectionsEmitted = 1;  // .text only, current
                                                 // cycle — sized at
                                                 // emit time below.
    std::size_t const sectionDataOffsetBase =
        kFileHeaderSize + kNumSectionsEmitted * kSectionHeaderSize;

    std::uint32_t const textRawPointer =
        static_cast<std::uint32_t>(sectionDataOffsetBase);
    std::uint32_t const textRawSize    =
        static_cast<std::uint32_t>(text.size());
    std::uint32_t const textRelocPointer =
        textRelocCount > 0 ? textRawPointer + textRawSize : 0u;
    std::uint32_t const textRelocSize =
        static_cast<std::uint32_t>(textRelocs.size());

    std::uint32_t const symtabPointer =
        textRawPointer + textRawSize + textRelocSize;
    std::uint32_t const symtabSizeBytes =
        static_cast<std::uint32_t>(symtab.size());
    // Substrate invariant: every `appendSym` writes exactly 18
    // bytes. A future bug that dropped one byte would silently
    // produce fewer symbols than expected (the integer division
    // truncates the trailing partial record). Surface the
    // violation rather than letting the symtab silently shrink
    // (silent-failure-hunter H5).
    if (symtab.size() % kSymbolRecordSize != 0) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"PE writer: symbol-table byte size "}
                 + std::to_string(symtab.size())
                 + " is not a multiple of IMAGE_SYMBOL size (18) — "
                   "substrate invariant violation in appendSym path");
        return {};
    }
    std::uint32_t const numberOfSymbols =
        static_cast<std::uint32_t>(symtab.size() / kSymbolRecordSize);

    // ── Emit ──
    std::vector<std::uint8_t> bytes;
    bytes.reserve(symtabPointer + symtabSizeBytes + strtab.size());

    // Build the section header for .text (the only emitted section
    // this cycle); push onto the vector so NumberOfSections derives
    // from `.size()`.
    {
        PeSectionHeader hText{};
        hText.name                  = encodeSectionName(secText->name, 0);
        hText.virtualSize           = 0;        // 0 for .obj
        hText.virtualAddress        = 0;        // 0 for .obj
        hText.sizeOfRawData         = textRawSize;
        hText.pointerToRawData      = textRawPointer;
        hText.pointerToRelocations  = textRelocPointer;
        hText.pointerToLinenumbers  = 0;
        hText.numberOfRelocations   = static_cast<std::uint16_t>(textRelocCount);
        hText.numberOfLinenumbers   = 0;
        hText.characteristics       = secText->type;  // PE uses substrate
                                                       // `type` field for
                                                       // Characteristics
        sectionHeaders.push_back(hText);
    }

    // IMAGE_FILE_HEADER
    auto const& id = fmt.pe();
    appendU16LE(bytes, id.machine);
    appendU16LE(bytes, static_cast<std::uint16_t>(sectionHeaders.size()));
    appendU32LE(bytes, 0);  // TimeDateStamp = 0 (deterministic)
    appendU32LE(bytes, symtabPointer);
    appendU32LE(bytes, numberOfSymbols);
    appendU16LE(bytes, 0);  // SizeOfOptionalHeader = 0 for .obj
    appendU16LE(bytes, id.characteristics);

    // IMAGE_SECTION_HEADER table
    for (auto const& h : sectionHeaders) {
        writeSectionHeader(bytes, h);
    }

    // Section data + relocations
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.insert(bytes.end(), textRelocs.begin(), textRelocs.end());

    // Symbol table
    bytes.insert(bytes.end(), symtab.begin(), symtab.end());

    // String table (with size prefix)
    auto strtabBytes = std::move(strtab).release();
    bytes.insert(bytes.end(), strtabBytes.begin(), strtabBytes.end());

    return bytes;
}

// ── PE32+ executable image (.exe) walker — LK2 cycle 2 ──────────
//
// Closes plan 14 §3.1 D-LK2-1. Emits a minimal-valid PE32+ .exe
// the Windows loader will accept: MS-DOS stub + PE signature +
// IMAGE_FILE_HEADER (with EXECUTABLE_IMAGE flag) +
// IMAGE_OPTIONAL_HEADER64 + section headers + section data
// (fileAlignment-padded). Intra-module relocations are applied
// in-place via the LK6 cycle 1 structured-formula triple
// (`pcRelative + addendBias + widthBytes`). Extern symbols fail
// loud (anchored D-LK6-2 — same boundary as ELF ET_EXEC).
//
// The validate() pass enforces the optional-header field
// population, so this function never has to re-check those
// invariants — read directly from `fmt.peOptionalHeader()`.

namespace {

[[nodiscard]] std::vector<std::uint8_t>
encodeExec(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& fmt,
           ObjectFormatSectionInfo const& secText,
           DiagnosticReporter&       reporter) {
    auto const& id = fmt.pe();
    auto const& oh = fmt.peOptionalHeader();

    // ── (a) Build .text body + per-function start map ─────────
    std::vector<std::uint8_t> text;
    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());
    std::unordered_map<SymbolId, std::uint64_t> textOffsetBySym;
    textOffsetBySym.reserve(module.functions.size());
    for (auto const& fn : module.functions) {
        std::uint64_t const start = text.size();
        funcTextStart.push_back(start);
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
        textOffsetBySym.emplace(fn.symbol, start);
    }

    if (text.empty()) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             "pe::encodeExec: `.text` is empty — every "
             "AssembledFunction contributed zero bytes. An exec "
             "with no instructions would crash at entry.");
        return {};
    }

    // ── (b) Resolve entry-function index from schema.entryPoint
    //
    // Mirrors the ELF ET_EXEC arm (D-LK1-1 follow-up): empty
    // entryPoint defaults to functions[0]; non-empty looks up by
    // synthesized `sym_<id>` name today (real-name resolution
    // closes with the HIR→AssembledFunction symbol-name thread).
    std::size_t entryFnIdx = 0;
    if (auto const ep = fmt.entryPoint(); !ep.empty()) {
        bool found = false;
        for (std::size_t i = 0; i < module.functions.size(); ++i) {
            std::string const cand =
                "sym_" + std::to_string(module.functions[i].symbol.v);
            if (cand == ep) { entryFnIdx = i; found = true; break; }
        }
        if (!found) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"pe::encodeExec: entryPoint '"}
                     + std::string{ep}
                     + "' not found among module function symbols.");
            return {};
        }
    }

    // ── (c) Apply intra-module relocations in-place ───────────
    //
    // Delegated to the shared `applyExecRelocations` kernel
    // (`link/format/exec_reloc_apply.hpp`). PE-specific input:
    // sectionVa = ImageBase + secText.virtualAddress (RVA). Format-
    // side fmt.relocationByKind(rel.kind) check stays here because
    // its diagnostic wording cites the PE format name.
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (fmt.relocationByKind(rel.kind) == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("pe::encodeExec: kind {} not declared "
                                 "by object format '{}' — substrate-"
                                 "invariant violation.",
                                 rel.kind.v, fmt.name()));
                return {};
            }
        }
    }
    std::uint64_t const sectionVa = oh.imageBase + secText.virtualAddress;
    if (!link::format::applyExecRelocations(
            text, module, funcTextStart, textOffsetBySym, targetSchema,
            sectionVa, "pe::encodeExec", reporter)) {
        return {};
    }

    // ── (d) Layout constants ──────────────────────────────────
    //
    // Header layout for PE32+ (spec §3.2-3.4):
    //   [0x00]  IMAGE_DOS_HEADER     (64 B)
    //   [0x40]  MS-DOS stub program  (64 B — fixed)
    //   [0x80]  PE signature "PE\0\0" (4 B)
    //   [0x84]  IMAGE_FILE_HEADER     (20 B)
    //   [0x98]  IMAGE_OPTIONAL_HEADER (240 B = 112 fixed + 16×8 data dirs)
    //   [0x188] IMAGE_SECTION_HEADER  (40 B × N)
    //
    // After all headers, pad to fileAlignment (typical 0x200) and
    // emit section raw data. Section virtualAddress is the RVA;
    // sizeOfRawData is the file-aligned byte length of `.text`.
    constexpr std::size_t kDosHeaderSize          = 64;
    constexpr std::size_t kDosStubSize            = 64;
    constexpr std::size_t kPeSigSize              = 4;
    constexpr std::size_t kFileHeaderSize         = 20;
    constexpr std::size_t kOptionalHeader64Fixed  = 112;
    constexpr std::size_t kNumberOfRvaAndSizes    = 16;
    constexpr std::size_t kDataDirectoryEntrySize = 8;
    constexpr std::size_t kOptionalHeader64Size   =
        kOptionalHeader64Fixed
        + kNumberOfRvaAndSizes * kDataDirectoryEntrySize;
    constexpr std::size_t kSectionHeaderSize = 40;

    // Build the section-header vector NOW so NumberOfSections,
    // headerBytesUnpadded, sizeOfImage, etc. ALL derive from
    // `sectionHeaders.size()` — same B-LK1-2 / D-LK2-5 discipline
    // the .obj arm + ELF walker adopted (architect O3 + code-
    // reviewer #5 convergence). A future cycle adding .rdata /
    // .data simply pushes onto this vector; counts update.
    auto roundUp = [](std::uint64_t v, std::uint64_t a) {
        return (v + a - 1) & ~(a - 1);
    };
    std::uint32_t const fileAlign    = oh.fileAlignment;
    std::uint32_t const sectionAlign = oh.sectionAlignment;

    std::vector<PeSectionHeader> sectionHeaders;
    sectionHeaders.reserve(1);  // .text only this cycle;
                                 // future cycles push .rdata / .data here
    std::size_t const headerBytesUnpaddedInitial =
        kDosHeaderSize + kDosStubSize + kPeSigSize
        + kFileHeaderSize + kOptionalHeader64Size;
    // sizeOfHeaders depends on numSections; build the section
    // headers first then compute below.

    // .text section header (single-section cycle scope; the
    // derived-from-vector machinery is built so future sections
    // just push back).
    {
        PeSectionHeader hText{};
        hText.name                  = encodeSectionName(secText.name, 0);
        hText.virtualSize           = static_cast<std::uint32_t>(text.size());
        hText.virtualAddress        = static_cast<std::uint32_t>(secText.virtualAddress);
        // sizeOfRawData filled below once textRawSize is known.
        hText.pointerToRelocations  = 0;
        hText.pointerToLinenumbers  = 0;
        hText.numberOfRelocations   = 0;
        hText.numberOfLinenumbers   = 0;
        hText.characteristics       = secText.type;
        sectionHeaders.push_back(hText);
    }
    std::uint32_t const numSections =
        static_cast<std::uint32_t>(sectionHeaders.size());
    std::size_t const headerBytesUnpadded =
        headerBytesUnpaddedInitial
        + numSections * kSectionHeaderSize;
    std::uint32_t const sizeOfHeaders =
        static_cast<std::uint32_t>(roundUp(headerBytesUnpadded, fileAlign));
    std::uint32_t const textRawSize =
        static_cast<std::uint32_t>(roundUp(text.size(), fileAlign));
    std::uint32_t const textRawPointer = sizeOfHeaders;
    sectionHeaders[0].sizeOfRawData    = textRawSize;
    sectionHeaders[0].pointerToRawData = textRawPointer;
    // SizeOfImage = MAX over all sections of (section.virtualAddress
    // + ceil(virtualSize / sectionAlignment) * sectionAlignment),
    // rounded up to sectionAlignment per PE/COFF §3.4. With one
    // section this collapses to ceil(secText.va + size, sectAlign).
    // (code-reviewer C3 + silent-failure C3 convergence: must be
    // multiple of sectionAlignment.)
    std::uint32_t const textVirtualSize =
        static_cast<std::uint32_t>(roundUp(text.size(), sectionAlign));
    std::uint32_t const sizeOfImage =
        static_cast<std::uint32_t>(roundUp(
            secText.virtualAddress + textVirtualSize, sectionAlign));
    std::uint32_t const addressOfEntryPoint =
        static_cast<std::uint32_t>(secText.virtualAddress
                                    + funcTextStart[entryFnIdx]);
    std::uint32_t const baseOfCode =
        static_cast<std::uint32_t>(secText.virtualAddress);

    // ── (e) Emit bytes ────────────────────────────────────────
    std::vector<std::uint8_t> bytes;
    bytes.reserve(sizeOfHeaders + textRawSize);

    // IMAGE_DOS_HEADER: "MZ" + zeros + e_lfanew at offset 0x3C
    // pointing to the PE signature at 0x80.
    bytes.resize(kDosHeaderSize, 0);
    bytes[0] = 'M'; bytes[1] = 'Z';
    constexpr std::size_t kELfaNewOffset = 0x3C;
    std::uint32_t const peSigFileOff =
        static_cast<std::uint32_t>(kDosHeaderSize + kDosStubSize);
    bytes[kELfaNewOffset + 0] = static_cast<std::uint8_t>(peSigFileOff);
    bytes[kELfaNewOffset + 1] = static_cast<std::uint8_t>(peSigFileOff >> 8);
    bytes[kELfaNewOffset + 2] = static_cast<std::uint8_t>(peSigFileOff >> 16);
    bytes[kELfaNewOffset + 3] = static_cast<std::uint8_t>(peSigFileOff >> 24);

    // MS-DOS stub: 64 zero bytes (the kernel doesn't read it; modern
    // toolchains ship the legacy "This program cannot be run in DOS
    // mode" message, but a zero-filled stub is loader-legal — the
    // Windows loader never executes this region).
    bytes.resize(bytes.size() + kDosStubSize, 0);

    // PE signature "PE\0\0"
    appendU8(bytes, 'P'); appendU8(bytes, 'E');
    appendU8(bytes, 0);   appendU8(bytes, 0);

    // IMAGE_FILE_HEADER
    appendU16LE(bytes, id.machine);
    appendU16LE(bytes, static_cast<std::uint16_t>(numSections));
    appendU32LE(bytes, 0);  // TimeDateStamp = 0 (deterministic)
    appendU32LE(bytes, 0);  // PointerToSymbolTable = 0 (image has no symtab)
    appendU32LE(bytes, 0);  // NumberOfSymbols = 0
    appendU16LE(bytes, static_cast<std::uint16_t>(kOptionalHeader64Size));
    appendU16LE(bytes, id.characteristics);

    // IMAGE_OPTIONAL_HEADER64 (PE/COFF §3.4)
    appendU16LE(bytes, oh.magic);
    appendU8(bytes, 0);  // MajorLinkerVersion (not load-bearing)
    appendU8(bytes, 0);  // MinorLinkerVersion
    appendU32LE(bytes, textRawSize);           // SizeOfCode
    appendU32LE(bytes, 0);                     // SizeOfInitializedData
    appendU32LE(bytes, 0);                     // SizeOfUninitializedData
    appendU32LE(bytes, addressOfEntryPoint);   // AddressOfEntryPoint
    appendU32LE(bytes, baseOfCode);            // BaseOfCode
    // (PE32+ omits BaseOfData)
    appendU64LE(bytes, oh.imageBase);
    appendU32LE(bytes, sectionAlign);
    appendU32LE(bytes, fileAlign);
    appendU16LE(bytes, oh.majorOperatingSystemVersion);
    appendU16LE(bytes, oh.minorOperatingSystemVersion);
    appendU16LE(bytes, 0);                     // MajorImageVersion
    appendU16LE(bytes, 0);                     // MinorImageVersion
    appendU16LE(bytes, oh.majorSubsystemVersion);
    appendU16LE(bytes, oh.minorSubsystemVersion);
    appendU32LE(bytes, 0);                     // Win32VersionValue (reserved)
    appendU32LE(bytes, sizeOfImage);
    appendU32LE(bytes, sizeOfHeaders);
    appendU32LE(bytes, 0);                     // CheckSum (loader allows 0)
    appendU16LE(bytes, oh.subsystem);
    appendU16LE(bytes, oh.dllCharacteristics);
    appendU64LE(bytes, oh.sizeOfStackReserve);
    appendU64LE(bytes, oh.sizeOfStackCommit);
    appendU64LE(bytes, oh.sizeOfHeapReserve);
    appendU64LE(bytes, oh.sizeOfHeapCommit);
    appendU32LE(bytes, 0);                     // LoaderFlags (reserved)
    appendU32LE(bytes,
        static_cast<std::uint32_t>(kNumberOfRvaAndSizes));
    // 16 data directories, all zero (minimum-loadable .exe needs none).
    for (std::size_t i = 0; i < kNumberOfRvaAndSizes; ++i) {
        appendU32LE(bytes, 0);  // RVA
        appendU32LE(bytes, 0);  // Size
    }

    // IMAGE_SECTION_HEADER table (derives from sectionHeaders vector)
    for (auto const& h : sectionHeaders) {
        writeSectionHeader(bytes, h);
    }

    // Pad headers area to fileAlignment.
    while (bytes.size() < sizeOfHeaders) bytes.push_back(0);

    // .text body, padded to fileAlignment.
    bytes.insert(bytes.end(), text.begin(), text.end());
    while (bytes.size() < static_cast<std::size_t>(textRawPointer)
                            + textRawSize) {
        bytes.push_back(0);
    }

    return bytes;
}

} // namespace

} // namespace dss::pe
