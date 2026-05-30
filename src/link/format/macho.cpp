#include "link/format/macho.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"
#include "link/format/string_table.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Mach-O 64-bit relocatable (.o, MH_OBJECT) writer — plan 14 LK3.
//
// Byte layout (Apple OS X ABI Mach-O File Format Reference +
// <mach-o/loader.h> + <mach-o/nlist.h> + <mach-o/reloc.h>):
//   [0x00]      mach_header_64                  (32 B)
//   [0x20]      LC_SEGMENT_64 + section_64[N]   (72 + 80*N B)
//   [...]       LC_SYMTAB                       (24 B)
//   [...]       __text bytes
//   [...]       per-section relocation_info[]   (8 B each)
//   [symoff]    nlist_64[]                      (16 B each)
//   [stroff]    string table (NUL-seeded)
//
// The walker is target-blind in shape — every Mach-O-specific
// number (cputype, cpusubtype, filetype, section flags, reloc
// nativeId, section/segment names) is read from the format
// schema. Only the binary record layout is hardcoded.

namespace dss::macho {

namespace {

using lir_pass_util::report;
using link::format::detail::appendU8;
using link::format::detail::appendU16LE;
using link::format::detail::appendU32LE;
using link::format::detail::appendU64LE;
using link::format::detail::emit;
using link::format::detail::requireSection;
using link::format::detail::StringTable;

// ── Mach-O constants ────────────────────────────────────────────

constexpr std::uint32_t MH_MAGIC_64 = 0xFEEDFACFu;
constexpr std::uint32_t LC_SEGMENT_64 = 0x19u;
constexpr std::uint32_t LC_SYMTAB     = 0x02u;
constexpr std::int32_t  kVmProtRwx    = 7;   // R|W|X

// nlist_64.n_type bits (<mach-o/nlist.h>)
constexpr std::uint8_t N_EXT  = 0x01;
constexpr std::uint8_t N_UNDF = 0x00;
constexpr std::uint8_t N_SECT = 0x0E;

constexpr std::size_t kMachHeader64Size   = 32;
constexpr std::size_t kSegmentCommand64Size = 72;
constexpr std::size_t kSection64Size      = 80;
constexpr std::size_t kSymtabCommandSize  = 24;
constexpr std::size_t kNlist64Size        = 16;
constexpr std::size_t kRelocationInfoSize = 8;

// ── Fixed-width name field (16 chars, NUL-padded) ──────────────

void appendName16(std::vector<std::uint8_t>& out, std::string_view name) {
    std::array<std::uint8_t, 16> buf{};  // zero-initialized
    std::size_t const n = std::min(name.size(), std::size_t{16});
    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<std::uint8_t>(name[i]);
    }
    for (auto b : buf) out.push_back(b);
}

void padTo(std::vector<std::uint8_t>& out, std::uint64_t alignment) {
    if (alignment <= 1) return;
    while (out.size() % alignment != 0) out.push_back(0);
}

} // namespace

std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter) {
    (void)targetSchema;  // Reserved: future formula-application
                         // path (LK6).

    auto const& fmt = objectFormatSchema;
    if (fmt.kind() != ObjectFormatKind::MachO) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"macho::encode called with non-Mach-O format '"}
                 + std::string{fmt.name()}
                 + "' (kind="
                 + std::string{objectFormatKindName(fmt.kind())}
                 + ")");
        return {};
    }

    // Mach-O MH_OBJECT requires `__text`; symtab/strtab live inside
    // LC_SYMTAB, not as separate section headers.
    auto const* secText =
        requireSection(fmt, SectionKind::Text, "Mach-O writer", reporter);
    if (!secText) return {};

    // ── Build .text + per-function symbols ─────────────────────
    std::vector<std::uint8_t> textBody;
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
        std::uint64_t const start = textBody.size();
        funcTextStart.push_back(start);
        textBody.insert(textBody.end(), fn.bytes.begin(), fn.bytes.end());
        funcSyms.push_back({fn.symbol, start,
                            static_cast<std::uint64_t>(fn.bytes.size())});
    }

    // ── Build symbol-table indices (same discipline as PE) ─────
    //
    // Order: defined externs (N_SECT|N_EXT) followed by undefined
    // externs (N_UNDF|N_EXT). Mach-O doesn't require local-then-
    // global when LC_DYSYMTAB is absent, but defined-then-undefined
    // is the convention Apple's ld64 produces.

    std::unordered_set<SymbolId> definedSet;
    definedSet.reserve(funcSyms.size());
    for (auto const& f : funcSyms) definedSet.insert(f.symId);

    std::vector<SymbolId> externSyms;
    std::unordered_set<SymbolId> externSeen;
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (definedSet.contains(rel.target)) continue;
            if (externSeen.insert(rel.target).second) {
                externSyms.push_back(rel.target);
            }
        }
    }

    std::unordered_map<SymbolId, std::uint32_t> symIdxBySymbol;
    symIdxBySymbol.reserve(funcSyms.size() + externSyms.size());
    std::uint32_t nextSymIdx = 0;
    for (auto const& f : funcSyms) {
        auto const [it, fresh] = symIdxBySymbol.emplace(f.symId, nextSymIdx);
        if (!fresh) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"Mach-O writer: duplicate defined symbol #"}
                     + std::to_string(f.symId.v));
            return {};
        }
        ++nextSymIdx;
    }
    for (auto const& e : externSyms) {
        if (symIdxBySymbol.emplace(e, nextSymIdx).second) ++nextSymIdx;
    }

    // ── Build per-section relocation_info table ────────────────
    //
    // Each relocation_info is 8 bytes packed: i32 r_address +
    // u32 r_info. r_info packing (bit positions 0..31, LE):
    //   bits 0..23  : r_symbolnum (24 bits)
    //   bit  24     : r_pcrel
    //   bits 25..26 : r_length
    //   bit  27     : r_extern
    //   bits 28..31 : r_type
    //
    // The format JSON's `nativeId` packs (type<<28)|(length<<25)
    // |(pcrel<<24); the walker ORs in (1<<27) for r_extern + the
    // 24-bit symbol index.
    //
    // Same discipline as PE: Mach-O's `relocation_info` has no
    // addend column (per `<mach-o/reloc.h>`); addends live in the
    // section's patch bytes. ELF Rela is the outlier with its
    // explicit `r_addend`. Fail loud on non-zero addend so an
    // ELF-shaped input cannot silently drop the addend here.

    std::vector<std::uint8_t> textRelocs;
    std::uint32_t textRelocCount = 0;
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        auto const& fn = module.functions[fi];
        std::uint64_t const fnStart = funcTextStart[fi];
        for (auto const& rel : fn.relocations) {
            if (rel.addend != 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"Mach-O writer: relocation in symbol #"}
                         + std::to_string(fn.symbol.v)
                         + " carries addend=" + std::to_string(rel.addend)
                         + " but Mach-O stores addends in the section's "
                           "patch bytes, not on relocation_info");
                return {};
            }
            // The `nullptr` / missing-symbol branches mirror ELF's
            // fail-loud discipline. `link()`'s cross-reference gate
            // makes these "unreachable", but silent `continue` is
            // the silent-failure class the substrate rejects — when
            // the gate ever drifts, the walker must surface the
            // problem (silent-failure C2 convergence).
            auto const* fmtReloc = fmt.relocationByKind(rel.kind);
            if (fmtReloc == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"Mach-O writer: relocation kind "}
                         + std::to_string(rel.kind.v)
                         + " is not declared by object format '"
                         + std::string{fmt.name()}
                         + "' — linker pre-walker gate should have "
                           "caught this; substrate-invariant violation");
                return {};
            }
            auto const it = symIdxBySymbol.find(rel.target);
            if (it == symIdxBySymbol.end()) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::string{"Mach-O writer: relocation target "
                                 "symbol #"}
                         + std::to_string(rel.target.v)
                         + " has no symtab entry — substrate-invariant "
                           "violation");
                return {};
            }
            std::uint32_t const symIdx = it->second;
            std::uint32_t const rAddress =
                static_cast<std::uint32_t>(fnStart + rel.offset);
            // Static portion of r_info (type|length|pcrel) lives in
            // nativeId; OR in r_extern (bit 27) + r_symbolnum
            // (low 24 bits). All Mach-O relocs in cycle scope are
            // extern (point at a symbol), so r_extern = 1.
            std::uint32_t const rInfo =
                fmtReloc->nativeId | (1u << 27) | (symIdx & 0x00FFFFFFu);
            appendU32LE(textRelocs, rAddress);
            appendU32LE(textRelocs, rInfo);
            ++textRelocCount;
        }
    }

    // ── Build nlist_64[] + string table ────────────────────────

    StringTable strtab;  // NulByte init — Mach-O n_strx=0 means "no name"
    std::vector<std::uint8_t> nlistBytes;

    auto appendNlist = [&](std::uint32_t nStrx, std::uint8_t nType,
                            std::uint8_t nSect, std::uint16_t nDesc,
                            std::uint64_t nValue) {
        appendU32LE(nlistBytes, nStrx);
        appendU8(nlistBytes, nType);
        appendU8(nlistBytes, nSect);
        appendU16LE(nlistBytes, nDesc);
        appendU64LE(nlistBytes, nValue);
    };

    constexpr std::uint8_t kTextSectionNumber = 1;

    // Defined function symbols: N_SECT|N_EXT, n_sect=1, n_value=offset.
    for (auto const& f : funcSyms) {
        std::string const symName =
            std::string{"_sym_"} + std::to_string(f.symId.v);
        std::uint32_t const nameOff = strtab.add(symName);
        appendNlist(nameOff,
                    static_cast<std::uint8_t>(N_SECT | N_EXT),
                    kTextSectionNumber,
                    /*n_desc=*/0,
                    f.valueInText);
    }
    // Undefined extern symbols: N_UNDF|N_EXT, n_sect=0, n_value=0.
    for (auto const& e : externSyms) {
        std::string const symName =
            std::string{"_sym_"} + std::to_string(e.v);
        std::uint32_t const nameOff = strtab.add(symName);
        appendNlist(nameOff,
                    static_cast<std::uint8_t>(N_UNDF | N_EXT),
                    /*n_sect=*/0,
                    /*n_desc=*/0,
                    /*n_value=*/0);
    }

    // Substrate invariant: every appendNlist writes exactly 16 bytes.
    if (nlistBytes.size() % kNlist64Size != 0) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"Mach-O writer: nlist table byte size "}
                 + std::to_string(nlistBytes.size())
                 + " is not a multiple of nlist_64 size (16)");
        return {};
    }
    std::uint32_t const numberOfSymbols =
        static_cast<std::uint32_t>(nlistBytes.size() / kNlist64Size);

    // ── Layout: header + load commands + section data + relocs
    //    + symtab + strtab ─────────────────────────────────────
    //
    // Section count is DERIVED from a per-emit vector below
    // (architect D-LK2-5 precedent — pre-fix LK2 hardcoded `1` and
    // had to be folded into a derived size; ELF was rewritten the
    // same way at LK1). The cycle-1 walker emits exactly one
    // section (`__text`), but the count flows through the
    // `mach_header_64.sizeofcmds` AND `LC_SEGMENT_64.cmdsize` AND
    // `LC_SEGMENT_64.nsects` derivations from `numSections`; a
    // hardcoded literal would silently desync those three when a
    // future cycle adds `__data`/`__const`.
    std::size_t const numSections = 1;  // __text only this cycle;
                                         // future cycles enumerate
                                         // emitted sections here.
    std::size_t const headerAndCommands =
        kMachHeader64Size
        + kSegmentCommand64Size + numSections * kSection64Size
        + kSymtabCommandSize;

    std::uint64_t const textRawOffset =
        static_cast<std::uint64_t>(headerAndCommands);
    std::uint64_t const textRawSize =
        static_cast<std::uint64_t>(textBody.size());
    std::uint64_t const textRelocOffset =
        textRelocCount > 0 ? textRawOffset + textRawSize : 0;
    std::uint64_t const textRelocSize =
        static_cast<std::uint64_t>(textRelocs.size());

    std::uint64_t const symtabOffset =
        textRawOffset + textRawSize + textRelocSize;
    std::uint64_t const stringTableOffset =
        symtabOffset + nlistBytes.size();
    std::uint64_t const stringTableSize =
        static_cast<std::uint64_t>(strtab.size());

    // ── Emit bytes ────────────────────────────────────────────
    std::vector<std::uint8_t> bytes;
    bytes.reserve(stringTableOffset + stringTableSize);

    auto const& id = fmt.macho();

    // mach_header_64
    appendU32LE(bytes, MH_MAGIC_64);
    appendU32LE(bytes, id.cputype);
    appendU32LE(bytes, id.cpusubtype);
    appendU32LE(bytes, id.filetype);
    appendU32LE(bytes, 2);  // ncmds: LC_SEGMENT_64 + LC_SYMTAB
    appendU32LE(bytes,
        static_cast<std::uint32_t>(kSegmentCommand64Size
                                    + numSections * kSection64Size
                                    + kSymtabCommandSize));
    appendU32LE(bytes, id.flags);
    appendU32LE(bytes, 0);  // reserved (64-bit padding)

    // LC_SEGMENT_64 (anonymous catch-all segment for MH_OBJECT)
    appendU32LE(bytes, LC_SEGMENT_64);
    appendU32LE(bytes,
        static_cast<std::uint32_t>(kSegmentCommand64Size
                                    + numSections * kSection64Size));
    appendName16(bytes, "");  // segname empty for MH_OBJECT
    appendU64LE(bytes, 0);    // vmaddr
    appendU64LE(bytes, textRawSize);  // vmsize
    appendU64LE(bytes, textRawOffset);  // fileoff
    appendU64LE(bytes, textRawSize);  // filesize
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRwx));  // maxprot
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRwx));  // initprot
    appendU32LE(bytes, static_cast<std::uint32_t>(numSections));  // nsects
    appendU32LE(bytes, 0);  // segment flags

    // section_64 for __text
    appendName16(bytes, secText->name);
    appendName16(bytes, secText->segment);
    appendU64LE(bytes, 0);  // addr (loaded later)
    appendU64LE(bytes, textRawSize);  // size
    appendU32LE(bytes, static_cast<std::uint32_t>(textRawOffset));
    appendU32LE(bytes, static_cast<std::uint32_t>(secText->addrAlign));
    appendU32LE(bytes, static_cast<std::uint32_t>(textRelocOffset));
    appendU32LE(bytes, textRelocCount);
    appendU32LE(bytes, secText->type);  // section flags from JSON
                                         // (cycle 1 ships S_REGULAR |
                                         // S_ATTR_PURE_INSTRUCTIONS |
                                         // S_ATTR_SOME_INSTRUCTIONS;
                                         // future cycles read JSON-
                                         // declared values verbatim)
    appendU32LE(bytes, 0);  // reserved1
    appendU32LE(bytes, 0);  // reserved2
    appendU32LE(bytes, 0);  // reserved3

    // LC_SYMTAB
    appendU32LE(bytes, LC_SYMTAB);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSymtabCommandSize));
    appendU32LE(bytes, static_cast<std::uint32_t>(symtabOffset));
    appendU32LE(bytes, numberOfSymbols);
    appendU32LE(bytes, static_cast<std::uint32_t>(stringTableOffset));
    appendU32LE(bytes, static_cast<std::uint32_t>(stringTableSize));

    // section data
    bytes.insert(bytes.end(), textBody.begin(), textBody.end());
    bytes.insert(bytes.end(), textRelocs.begin(), textRelocs.end());

    // symbol table
    bytes.insert(bytes.end(), nlistBytes.begin(), nlistBytes.end());

    // string table
    auto strtabBytes = std::move(strtab).release();
    bytes.insert(bytes.end(), strtabBytes.begin(), strtabBytes.end());

    return bytes;
}

} // namespace dss::macho
