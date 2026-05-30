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
    for (auto const& fn : module.functions) {
        std::uint64_t const start = text.size();
        funcTextStart.push_back(start);
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
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

    // ── (c) Synthesize .idata section for extern imports (LK6
    //         cycle 2a). PE32+ import-table layout per PE/COFF §6.4:
    //         ImageImportDescriptor[N+1] (terminator) → ILT[] → IAT[]
    //         → HINT/NAME table → DLL name strings. Each descriptor
    //         is 20 B; each ILT/IAT slot is u64; HINT/NAME is u16
    //         hint=0 + NUL-terminated symbol name (padded to even);
    //         DLL names are NUL-terminated. The loader uses the IAT
    //         slot at runtime: it overwrites IAT[i] with the
    //         resolved function pointer (for PE32+, IAT and ILT are
    //         identical at file-image time; loader patches IAT
    //         in-place).
    //
    // Build a map `libraryPath → ordered list of extern symbols`
    // (preserves declaration order within each library; deterministic
    // build outputs). One ImageImportDescriptor per library.
    std::vector<std::string> libraryOrder;  // libs in declaration order
    std::unordered_map<std::string, std::vector<std::size_t>>
        externsByLib;  // libraryPath → indices into module.externImports
    for (std::size_t i = 0; i < module.externImports.size(); ++i) {
        auto const& ext = module.externImports[i];
        auto const it = externsByLib.find(ext.libraryPath);
        if (it == externsByLib.end()) {
            libraryOrder.push_back(ext.libraryPath);
            externsByLib.emplace(ext.libraryPath, std::vector<std::size_t>{i});
        } else {
            it->second.push_back(i);
        }
    }

    bool const hasImports = !module.externImports.empty();
    std::uint32_t const sectionAlignE = oh.sectionAlignment;
    std::uint32_t const textVirtualSizeE =
        static_cast<std::uint32_t>(
            (text.size() + sectionAlignE - 1) & ~(sectionAlignE - 1ull));
    std::uint32_t const idataRva =
        hasImports
            ? static_cast<std::uint32_t>(secText.virtualAddress)
                + textVirtualSizeE
            : 0u;

    // Lay out .idata bytes (synthesized, NOT raw-data padded yet):
    //   [0]               ImageImportDescriptor[N+1]
    //   [...]             ILT/IAT (PE32+ uses identical layout at
    //                     file-image time — loader patches IAT)
    //   [hintNameStart]   HINT/NAME table
    //   [dllNamesStart]   DLL name strings
    //
    // Compute offsets first so we know each IAT slot's RVA before
    // building bytes (the symbol-VA map needs it for reloc apply).
    constexpr std::size_t kImportDescriptorSize = 20;
    constexpr std::size_t kThunkSize            = 8;   // PE32+
    std::size_t const numLibs        = libraryOrder.size();
    std::size_t const descriptorBlockSize =
        (numLibs + 1) * kImportDescriptorSize;
    // Pad to kThunkSize so u64 ILT/IAT slots stay naturally aligned.
    // (numLibs+1)*20 is 8-aligned only when (numLibs+1) is even —
    // breaks at numLibs≥2 (code-reviewer #1 convergence). The pad
    // bytes stay zero in the idata buffer; the data-directory size
    // for Import Table remains `descriptorBlockSize` (excludes
    // pad).
    std::size_t const thunkBlockStart =
        (descriptorBlockSize + kThunkSize - 1) & ~(kThunkSize - 1);
    // Per-library ILT and IAT counts include a u64 zero terminator.
    std::vector<std::size_t> iltOffsets(numLibs);
    std::vector<std::size_t> iatOffsets(numLibs);
    std::size_t thunkCursor = thunkBlockStart;
    for (std::size_t li = 0; li < numLibs; ++li) {
        iltOffsets[li] = thunkCursor;
        std::size_t const slots = externsByLib[libraryOrder[li]].size() + 1;
        thunkCursor += slots * kThunkSize;
    }
    for (std::size_t li = 0; li < numLibs; ++li) {
        iatOffsets[li] = thunkCursor;
        std::size_t const slots = externsByLib[libraryOrder[li]].size() + 1;
        thunkCursor += slots * kThunkSize;
    }
    // HINT/NAME table starts here. Per extern: 2 bytes hint + name +
    // NUL + optional padding to even.
    std::size_t const hintNameStart = thunkCursor;
    std::vector<std::uint32_t> hintNameRvaBySym;  // per externImport index
    hintNameRvaBySym.resize(module.externImports.size(), 0u);
    std::size_t hintCursor = hintNameStart;
    for (std::size_t li = 0; li < numLibs; ++li) {
        for (auto extIdx : externsByLib[libraryOrder[li]]) {
            hintNameRvaBySym[extIdx] =
                idataRva + static_cast<std::uint32_t>(hintCursor);
            std::size_t const nameBytes =
                module.externImports[extIdx].mangledName.size() + 1; // +NUL
            hintCursor += 2 + nameBytes;                  // 2 = hint
            if ((hintCursor - hintNameStart) & 1u) ++hintCursor;
        }
    }
    std::size_t const dllNameStart = hintCursor;
    std::vector<std::uint32_t> dllNameRvaByLib(numLibs, 0u);
    for (std::size_t li = 0; li < numLibs; ++li) {
        dllNameRvaByLib[li] =
            idataRva + static_cast<std::uint32_t>(hintCursor);
        hintCursor += libraryOrder[li].size() + 1; // +NUL
    }
    std::size_t const idataSize = hintCursor;

    // Per-extern IAT slot RVA — populated for symbol-VA map.
    std::unordered_map<SymbolId, std::uint64_t> externIatVaBySym;
    externIatVaBySym.reserve(module.externImports.size());
    for (std::size_t li = 0; li < numLibs; ++li) {
        std::size_t slotIdx = 0;
        for (auto extIdx : externsByLib[libraryOrder[li]]) {
            std::uint32_t const iatSlotRva =
                idataRva
                + static_cast<std::uint32_t>(iatOffsets[li])
                + static_cast<std::uint32_t>(slotIdx * kThunkSize);
            externIatVaBySym.emplace(
                module.externImports[extIdx].symbol,
                oh.imageBase + iatSlotRva);
            ++slotIdx;
        }
    }

    // ── (d) Apply intra-module + extern relocations in-place ──
    //
    // Delegated to the shared `applyExecRelocations` kernel
    // (`link/format/exec_reloc_apply.hpp`). PE-specific input:
    // patchSectionVa = ImageBase + secText.virtualAddress (RVA).
    // The symbolVa map merges intra-module function VAs (.text +
    // funcOffset) with extern import VAs (.idata + iatSlotOffset),
    // so REL32 calls to either kind work uniformly. Format-side
    // fmt.relocationByKind(rel.kind) check stays here because its
    // diagnostic wording cites the PE format name.
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
    std::unordered_map<SymbolId, std::uint64_t> symbolVa;
    symbolVa.reserve(module.functions.size()
                     + module.externImports.size());
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        symbolVa.emplace(module.functions[i].symbol,
                         sectionVa + funcTextStart[i]);
    }
    for (auto const& [sym, va] : externIatVaBySym) {
        symbolVa.emplace(sym, va);
    }
    if (!link::format::applyExecRelocations(
            text, module, funcTextStart, symbolVa, targetSchema,
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
    sectionHeaders.reserve(hasImports ? 2 : 1);
    std::size_t const headerBytesUnpaddedInitial =
        kDosHeaderSize + kDosStubSize + kPeSigSize
        + kFileHeaderSize + kOptionalHeader64Size;

    // .text section header (always present).
    {
        PeSectionHeader hText{};
        hText.name                  = encodeSectionName(secText.name, 0);
        hText.virtualSize           = static_cast<std::uint32_t>(text.size());
        hText.virtualAddress        = static_cast<std::uint32_t>(secText.virtualAddress);
        // sizeOfRawData / pointerToRawData filled below.
        hText.pointerToRelocations  = 0;
        hText.pointerToLinenumbers  = 0;
        hText.numberOfRelocations   = 0;
        hText.numberOfLinenumbers   = 0;
        hText.characteristics       = secText.type;
        sectionHeaders.push_back(hText);
    }
    // .idata section header (when externImports non-empty).
    // Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA (0x40) |
    //                   IMAGE_SCN_MEM_READ (0x40000000) |
    //                   IMAGE_SCN_MEM_WRITE (0x80000000)
    //                 = 0xC0000040. PE32+ images keep the import
    // table writable so the loader can patch IAT slots in-place.
    constexpr std::uint32_t kIDataCharacteristics = 0xC0000040u;
    if (hasImports) {
        PeSectionHeader hIData{};
        hIData.name                  = encodeSectionName(".idata", 0);
        hIData.virtualSize           = static_cast<std::uint32_t>(idataSize);
        hIData.virtualAddress        = idataRva;
        // sizeOfRawData / pointerToRawData filled below.
        hIData.pointerToRelocations  = 0;
        hIData.pointerToLinenumbers  = 0;
        hIData.numberOfRelocations   = 0;
        hIData.numberOfLinenumbers   = 0;
        hIData.characteristics       = kIDataCharacteristics;
        sectionHeaders.push_back(hIData);
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
    std::uint32_t const idataRawSize = hasImports
        ? static_cast<std::uint32_t>(roundUp(idataSize, fileAlign))
        : 0u;
    std::uint32_t const idataRawPointer = hasImports
        ? textRawPointer + textRawSize
        : 0u;
    if (hasImports) {
        sectionHeaders[1].sizeOfRawData    = idataRawSize;
        sectionHeaders[1].pointerToRawData = idataRawPointer;
    }
    // SizeOfImage = roundUp(highest_section_va + virtualSize,
    // sectionAlignment) per PE/COFF §3.4. With .text + optional
    // .idata, the highest VA-extent is .idata when present.
    std::uint32_t const textVirtualSize = textVirtualSizeE;
    std::uint32_t const idataVirtualSize = hasImports
        ? static_cast<std::uint32_t>(roundUp(idataSize, sectionAlign))
        : 0u;
    std::uint32_t const sizeOfImage = hasImports
        ? static_cast<std::uint32_t>(roundUp(
              idataRva + idataVirtualSize, sectionAlign))
        : static_cast<std::uint32_t>(roundUp(
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
    // PE/COFF §3.4: SizeOfInitializedData is the sum of file-aligned
    // sizes of all sections carrying IMAGE_SCN_CNT_INITIALIZED_DATA.
    // The synthesized .idata section is the only such section in
    // cycle scope.
    appendU32LE(bytes, idataRawSize);
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
    // 16 data directories. PE/COFF §3.4 indices we populate:
    //   [1] Import Table — RVA to first ImageImportDescriptor;
    //                       Size covers all descriptors + terminator.
    //  [12] Import Address Table — RVA to IAT block; Size covers
    //                       all IAT slots across libraries.
    // Other directories (Export, Resource, Exception, etc.) remain
    // zero — minimum-loadable image needs only Import for FFI.
    std::uint32_t const importDirRva  = hasImports ? idataRva : 0u;
    std::uint32_t const importDirSize = hasImports
        ? static_cast<std::uint32_t>(descriptorBlockSize)
        : 0u;
    std::uint32_t const iatDirRva = hasImports
        ? idataRva + static_cast<std::uint32_t>(iatOffsets[0])
        : 0u;
    // Sum IAT block sizes explicitly rather than subtracting cursors.
    // Subtraction-based size silently misreports if a future cycle
    // inserts data between IATs and the HINT/NAME block (silent-
    // failure H1 fold). Each library's IAT spans (externCount+1) *
    // kThunkSize bytes (including the u64-zero terminator slot).
    std::size_t iatTotal = 0;
    for (auto const& libName : libraryOrder) {
        iatTotal +=
            (externsByLib[libName].size() + 1) * kThunkSize;
    }
    std::uint32_t const iatDirSize = hasImports
        ? static_cast<std::uint32_t>(iatTotal)
        : 0u;
    for (std::size_t i = 0; i < kNumberOfRvaAndSizes; ++i) {
        if (i == 1u) {
            appendU32LE(bytes, importDirRva);
            appendU32LE(bytes, importDirSize);
        } else if (i == 12u) {
            appendU32LE(bytes, iatDirRva);
            appendU32LE(bytes, iatDirSize);
        } else {
            appendU32LE(bytes, 0);
            appendU32LE(bytes, 0);
        }
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

    // .idata body (when externImports non-empty), padded to
    // fileAlignment. Layout precomputed above; emit ImageImport
    // Descriptors[N+1] + ILT/IAT thunks + HINT/NAME table + DLL
    // name strings.
    if (hasImports) {
        std::vector<std::uint8_t> idata(idataSize, 0);
        auto putU32 = [&](std::size_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                idata[off + i] =
                    static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
        };
        auto putU64 = [&](std::size_t off, std::uint64_t v) {
            for (int i = 0; i < 8; ++i)
                idata[off + i] =
                    static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
        };
        // ImageImportDescriptor[i] for each library; entry [numLibs]
        // is the all-zero terminator.
        for (std::size_t li = 0; li < numLibs; ++li) {
            std::size_t const dOff = li * kImportDescriptorSize;
            std::uint32_t const iltRva =
                idataRva + static_cast<std::uint32_t>(iltOffsets[li]);
            std::uint32_t const iatRva =
                idataRva + static_cast<std::uint32_t>(iatOffsets[li]);
            putU32(dOff +  0, iltRva);                  // OriginalFirstThunk
            putU32(dOff +  4, 0);                       // TimeDateStamp
            putU32(dOff +  8, 0);                       // ForwarderChain
            putU32(dOff + 12, dllNameRvaByLib[li]);     // Name (DLL path RVA)
            putU32(dOff + 16, iatRva);                  // FirstThunk (IAT RVA)
        }
        // ILT + IAT thunks (PE32+: u64 each; bit 63 = ordinal flag,
        // we use by-name imports only — set RVA to HINT/NAME entry).
        for (std::size_t li = 0; li < numLibs; ++li) {
            std::size_t slotIdx = 0;
            auto const& externs = externsByLib[libraryOrder[li]];
            for (auto extIdx : externs) {
                std::uint64_t const thunk =
                    static_cast<std::uint64_t>(hintNameRvaBySym[extIdx]);
                putU64(iltOffsets[li] + slotIdx * kThunkSize, thunk);
                putU64(iatOffsets[li] + slotIdx * kThunkSize, thunk);
                ++slotIdx;
            }
            // Zero terminator already in place (idata was 0-initialised).
        }
        // HINT/NAME table entries: u16 hint=0 + NUL-terminated name.
        for (std::size_t li = 0; li < numLibs; ++li) {
            for (auto extIdx : externsByLib[libraryOrder[li]]) {
                std::uint32_t const rva = hintNameRvaBySym[extIdx];
                std::size_t const off = rva - idataRva;
                // hint = 0 (no symbol-table hint); idata already
                // zero-initialised so we just write the name.
                auto const& name = module.externImports[extIdx].mangledName;
                for (std::size_t i = 0; i < name.size(); ++i) {
                    idata[off + 2 + i] = static_cast<std::uint8_t>(name[i]);
                }
                // NUL + optional pad already zero.
            }
        }
        // DLL name strings.
        for (std::size_t li = 0; li < numLibs; ++li) {
            std::uint32_t const rva = dllNameRvaByLib[li];
            std::size_t const off = rva - idataRva;
            auto const& dll = libraryOrder[li];
            for (std::size_t i = 0; i < dll.size(); ++i) {
                idata[off + i] = static_cast<std::uint8_t>(dll[i]);
            }
        }
        bytes.insert(bytes.end(), idata.begin(), idata.end());
        while (bytes.size()
                < static_cast<std::size_t>(idataRawPointer) + idataRawSize) {
            bytes.push_back(0);
        }
    }

    return bytes;
}

} // namespace

} // namespace dss::pe
