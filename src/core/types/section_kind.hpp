#pragma once

#include "core/export.hpp"
// ⚠ THIS USED TO PULL THE WHOLE `core/types/target_schema.hpp`, FOR ONE
// TEMPLATE. `EnumNameTable<E,N>` moved to its own dependency-free header when it
// was extracted, and `enum_name_table.hpp`'s own docblock names THIS file's
// neighbours as the motivating cases — so the heavy include was a stale premise,
// not a requirement. `target_schema.hpp` reaches `grammar_schema.hpp`
// (ConfigDiagnostic) and the whole target substrate; every TU that includes a
// section kind was paying for it, and a leaf enum header reached early in
// `grammar_schema.hpp`'s own include list is exactly the cycle the extraction
// exists to prevent.
#include "core/types/enum_name_table.hpp"   // EnumNameTable<E,N> + namesWhere (leaf header — no target_schema cycle)

#include <cstdint>
#include <optional>
#include <string_view>

// Canonical section taxonomy — format-blind names the substrate
// engine speaks. Each format JSON declares which platform-native
// section a given `SectionKind` maps to (e.g. ELF `.text` / PE
// `.text` / Mach-O `__TEXT,__text` all map to `SectionKind::Text`).
// The engine reads the kind; per-format JSON owns the name + flags.
//
// **Cross-tier vocabulary**: this header lives under `core/types/`
// rather than `src/link/` so the upstream assembler (`src/asm/`)
// can tag its `AssembledData` outputs with the same kind enum the
// downstream linker walkers consume. Both layers MUST speak the
// same section vocabulary so a future kind addition (e.g. a TLS
// section) lands at one point of truth — not duplicated as an
// `AssembledDataKind` on the asm side and a `SectionKind` on the
// link side. Extracted from `link/object_format_schema.hpp` at the
// D-LK4-RODATA-SUBSTRATE slice when `AssembledData` first needed the
// vocabulary.
//
// **Adding a new kind**: append to the enum AND to
// `kSectionKindTable` AND to every format JSON's `sections[]` rows
// that need it. The JSON-side `kind` field is the on-disk
// vocabulary; the C++ enum is its in-memory mirror. Drift between
// them is caught by the loader's `sectionKindFromName` lookup
// returning `nullopt` (fail-loud).
namespace dss {

enum class SectionKind : std::uint8_t {
    Text       = 0,  // executable code
    Rodata     = 1,  // read-only data
    Data       = 2,  // initialised mutable data
    Bss        = 3,  // zero-initialised mutable data
    Symtab     = 4,  // symbol table
    Strtab     = 5,  // symbol-name string table
    ShStrtab   = 6,  // section-name string table (ELF .shstrtab;
                     // distinct from Strtab — the consumer code
                     // path is "find names of OTHER sections" vs
                     // "find symbol names")
    RelocTable = 7,  // relocation entries
    Dynamic    = 8,  // ELF .dynamic / PE .idata / Mach-O LC_DYLD_INFO
    Note       = 9,  // build-id / vendor notes
    Debug      = 10, // DWARF / CodeView debug info
    Custom     = 11, // anything else the format JSON names
    // D-CSUBSET-THREAD-LOCAL (TLS C1): the thread-local section pair —
    // exactly the "future kind addition (e.g. a TLS section)" the
    // header docblock anticipated. Each format JSON maps them to its
    // native names (ELF `.tdata`/`.tbss`; PE `.tls`; Mach-O
    // `__DATA,__thread_data`/`__thread_bss`).
    ThreadData = 12, // initialised thread-local TEMPLATE data (.tdata)
    ThreadBss  = 13, // zero-fill thread-local TEMPLATE extent (.tbss)
    // D-CSUBSET-THREAD-LOCAL (TLS C4, Mach-O TLV): the thread-local
    // VARIABLE-DESCRIPTOR section. Mach-O uniquely reaches a thread-local
    // object through a per-variable 3-word `tlv_descriptor` in
    // `__DATA,__thread_vars` (S_THREAD_LOCAL_VARIABLES) — the descriptors
    // are WRITER-synthesized (never `AssembledData` producer output), so
    // this kind is NOT a `DataSectionKind` (`dataSectionKindOf` returns
    // nullopt for it). ELF/PE reach TLS with a tp-relative offset and
    // declare no such section. It exists only so the Mach-O format JSON
    // can name the section + its S_THREAD_LOCAL_VARIABLES flag config-side
    // (like the tdata/tbss rows), keeping the writer free of a hardcoded
    // section flag.
    ThreadVars = 14, // thread-local variable descriptors (Mach-O __thread_vars)
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): initialised CONST data that
    // carries load-time relocations (a const function-pointer table / `int
    // *const p = &x;` — sqlite's VFS method tables + `aSyscall[]`). It cannot
    // sit in read-only `.rodata` (the loader must write the resolved target
    // VA into the slot), yet must be READ-ONLY AFTER relocation (const
    // semantics + hardening). Every format has a "relocated-read-only"
    // placement: ELF `.data.rel.ro` (in the GNU_RELRO segment), Mach-O
    // `__DATA_CONST,__const` (dyld-rebased then mprotect'd RO), PE `.rdata`
    // (base-relocated before the page is sealed RO). Distinct from `Data`
    // (stays writable) — reloc-bearing MUTABLE data still routes to `Data`.
    RelRoConst = 15, // const data needing load-time relocations (relro)
};

inline constexpr EnumNameTable<SectionKind, 16> kSectionKindTable{{{
    { SectionKind::Text,       "text"       },
    { SectionKind::Rodata,     "rodata"     },
    { SectionKind::Data,       "data"       },
    { SectionKind::Bss,        "bss"        },
    { SectionKind::Symtab,     "symtab"     },
    { SectionKind::Strtab,     "strtab"     },
    { SectionKind::ShStrtab,   "shstrtab"   },
    { SectionKind::RelocTable, "reloc"      },
    { SectionKind::Dynamic,    "dynamic"    },
    { SectionKind::Note,       "note"       },
    { SectionKind::Debug,      "debug"      },
    { SectionKind::Custom,     "custom"     },
    { SectionKind::ThreadData, "tdata"      },
    { SectionKind::ThreadBss,  "tbss"       },
    { SectionKind::ThreadVars, "tvars"      },
    { SectionKind::RelRoConst, "relro"      },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kSectionKindTable);

[[nodiscard]] constexpr std::string_view
sectionKindName(SectionKind k) noexcept {
    return kSectionKindTable.name(k);
}
[[nodiscard]] constexpr std::optional<SectionKind>
sectionKindFromName(std::string_view s) noexcept {
    return kSectionKindTable.fromName(s);
}

// Narrow subset of `SectionKind` that a producer can legitimately
// emit via `AssembledData`. Closes D-LK4-RODATA-SECTION-NARROW.
//
// Of the 16 `SectionKind` values, 10 are walker-synthesized
// (`Text` = executable code; `Symtab`/`Strtab`/`ShStrtab` = symbol
// + name tables; `RelocTable` = relocation entries; `Dynamic` =
// dynamic linking metadata; `Note` = vendor notes; `Debug` =
// DWARF/CodeView; `ThreadVars` = the Mach-O tlv descriptors, minted
// by the WRITER and never by a producer; `Custom` = format-specific
// anything). A producer constructing
// `AssembledData{symbol, SectionKind::Symtab, ...}` is semantically
// nonsense — the assembler doesn't emit symbol tables; the linker
// walker synthesizes them.
//
// ⚠ THOSE TWO NUMBERS READ `14` AND `9` UNTIL 2026-08-23, AND THE
// PARENTHESIS OMITTED `ThreadVars`. They had been wrong since
// `ThreadVars` and `RelRoConst` landed: a count hand-typed into prose
// has no owner, so nothing moved it when the enum grew — the same
// species of defect as the projection count below, arriving through a
// comment instead of through a template argument. Both are now pinned
// by the `static_assert` beside `kDataSectionKindNames`, which fails
// the build if the 6/10 split ever moves.
//
// The SIX valid producer-emittable kinds:
//   * `Rodata` — read-only initialised data (string literals,
//                const arrays, vtables) with NO load-time relocations.
//   * `Data`   — read-write initialised data (mutable globals).
//   * `Bss`    — zero-fill mutable data (uninitialised globals).
//   * `Tdata`  — initialised THREAD-LOCAL template data: the
//                per-thread-copied initial image of a
//                `thread_local T g = init;` (D-CSUBSET-THREAD-LOCAL).
//   * `Tbss`   — zero-fill THREAD-LOCAL template extent: the
//                per-thread zero-init span of a `thread_local T g;`.
//   * `RelRoConst` — const initialised data that carries LOAD-TIME
//                RELOCATIONS (a const function-pointer table / `int
//                *const p = &x;`): relocated-then-read-only
//                (D-LK-RELRO-CONST-DATA-RELOCATABLE, c145).
//
// The walker still keys on `SectionKind` (the full enum). The
// `toSectionKind()` conversion is total — every `DataSectionKind`
// value maps to its corresponding `SectionKind`. The reverse
// direction is partial: `dataSectionKindOf()` returns nullopt for
// the 10 walker-synthesized kinds.
enum class DataSectionKind : std::uint8_t {
    Rodata     = static_cast<std::uint8_t>(SectionKind::Rodata),
    Data       = static_cast<std::uint8_t>(SectionKind::Data),
    Bss        = static_cast<std::uint8_t>(SectionKind::Bss),
    Tdata      = static_cast<std::uint8_t>(SectionKind::ThreadData),
    Tbss       = static_cast<std::uint8_t>(SectionKind::ThreadBss),
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): const data carrying load-time
    // relocations — file-backed (NOT zero-fill), read-only after relocation.
    RelRoConst = static_cast<std::uint8_t>(SectionKind::RelRoConst),
};

[[nodiscard]] constexpr SectionKind
toSectionKind(DataSectionKind d) noexcept {
    return static_cast<SectionKind>(static_cast<std::uint8_t>(d));
}

[[nodiscard]] constexpr std::optional<DataSectionKind>
dataSectionKindOf(SectionKind k) noexcept {
    switch (k) {
        case SectionKind::Rodata:     return DataSectionKind::Rodata;
        case SectionKind::Data:       return DataSectionKind::Data;
        case SectionKind::Bss:        return DataSectionKind::Bss;
        case SectionKind::ThreadData: return DataSectionKind::Tdata;
        case SectionKind::ThreadBss:  return DataSectionKind::Tbss;
        case SectionKind::RelRoConst: return DataSectionKind::RelRoConst;
        default:                      return std::nullopt;
    }
}

// D-CSUBSET-THREAD-LOCAL (TLS C1, audit fold M-3): the ONE zero-fill
// predicate. `Bss` and `Tbss` share the "reserves memory extent, stores
// NO file bytes" wire semantics (size lives in `reservedSize` /
// sh_size, `bytes` stays EMPTY by invariant); every other producer
// kind is file-backed. Before Tbss existed, three chokepoints tested
// `== DataSectionKind::Bss` EXACTLY (`AssembledData::sizeInSection`,
// `buildExecDataSection`'s layout branch, `validateAssembledData`'s
// no-bytes invariant) — each would have silently mis-handled a Tbss
// item (treating it as file-backed reads 0 bytes where reservedSize
// was the real span). All three now route through this predicate so
// a future zero-fill kind lands at ONE point of truth.
[[nodiscard]] constexpr bool isZeroFill(DataSectionKind d) noexcept {
    return d == DataSectionKind::Bss || d == DataSectionKind::Tbss;
}

// Round-trip pins (silent-failure F-1 + type-design Q5 fold,
// 8-agent audit on D-LK4-RODATA-SECTION-NARROW). `toSectionKind`
// is a raw `static_cast<SectionKind>(uint8_t)` — fast, but the
// numeric round-trip would silently break if a future maintainer
// rebased the explicit values on either enum without touching the
// other. These compile-time assertions pin both the totality of
// `toSectionKind` and the round-trip via `dataSectionKindOf`,
// catching drift at build time before any walker mis-routes bytes.
static_assert(toSectionKind(DataSectionKind::Rodata) == SectionKind::Rodata);
static_assert(toSectionKind(DataSectionKind::Data)   == SectionKind::Data);
static_assert(toSectionKind(DataSectionKind::Bss)    == SectionKind::Bss);
static_assert(toSectionKind(DataSectionKind::Tdata)  == SectionKind::ThreadData);
static_assert(toSectionKind(DataSectionKind::Tbss)   == SectionKind::ThreadBss);
static_assert(toSectionKind(DataSectionKind::RelRoConst) == SectionKind::RelRoConst);
static_assert(dataSectionKindOf(SectionKind::Rodata) == DataSectionKind::Rodata);
static_assert(dataSectionKindOf(SectionKind::Data)   == DataSectionKind::Data);
static_assert(dataSectionKindOf(SectionKind::Bss)    == DataSectionKind::Bss);
static_assert(dataSectionKindOf(SectionKind::ThreadData) == DataSectionKind::Tdata);
static_assert(dataSectionKindOf(SectionKind::ThreadBss)  == DataSectionKind::Tbss);
static_assert(dataSectionKindOf(SectionKind::RelRoConst) == DataSectionKind::RelRoConst);
// The zero-fill predicate covers EXACTLY the two no-file-bytes kinds.
static_assert(!isZeroFill(DataSectionKind::Rodata));
static_assert(!isZeroFill(DataSectionKind::Data));
static_assert( isZeroFill(DataSectionKind::Bss));
static_assert(!isZeroFill(DataSectionKind::Tdata));
static_assert( isZeroFill(DataSectionKind::Tbss));
static_assert(!isZeroFill(DataSectionKind::RelRoConst));  // file-backed (relocated RO)

[[nodiscard]] constexpr std::string_view
dataSectionKindName(DataSectionKind d) noexcept {
    return sectionKindName(toSectionKind(d));
}

// Is this section kind one a DATA declaration may name? The membership
// predicate, expressed as the existence of the narrowing — never as a second
// list of kinds.
[[nodiscard]] constexpr bool isDataSectionKind(SectionKind k) noexcept {
    return dataSectionKindOf(k).has_value();
}

// The spellings a data-section declaration may use — `kSectionKindTable`
// narrowed to the data subrange. What a loader's "the closed set is …" half
// must render.
//
// ⚠ THE COUNT IS NOT DECORATION — AND IT TAKES BOTH LINES BELOW TO BE TRUE IN
// BOTH DIRECTIONS. `namesWhere` refuses an `M` that disagrees with the number
// of accepted rows, at COMPILE TIME, so adding a seventh DATA kind (or renaming
// one) cannot leave this list — or any diagnostic rendered from it —
// advertising the old set.
//
// ★ WHAT `namesWhere` ALONE CANNOT SEE IS THE OTHER HALF OF THE SPLIT.
// D-CORE-NAMESWHERE-LITERAL-COUNT-IS-BLIND-TO-A-SECOND-SENTINEL: it only ever
// compares `M` against the ACCEPTED total, so a new WALKER-SYNTHESIZED kind — a
// row `isDataSectionKind` REJECTS — moves nothing it can observe. ✔MEASURED
// 2026-08-23 in a worktree with `g++ -std=c++23 -fsyntax-only -I src`: a
// seventeenth `SectionKind` row left on `dataSectionKindOf`'s `default:` arm
// COMPILED CLEAN before the `static_assert` below existed, and the mutation
// that DID red (a seventeenth row wired into `dataSectionKindOf`) is the one
// this comment already claimed. The assert pins the REJECTED count instead —
// the table's own row total against this projection's literal, two numbers with
// DIFFERENT OWNERS, so it is not the tautology a `rows.size() - 10` spelling
// would have been (D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY).
//
// ⓘ THE SENTINEL SHAPE ONE VOCABULARY OVER: `kSelectableObjectFormatKindNames`
// in `core/types/object_format_kind.hpp` is the same pair where the rejected
// set is a single sentinel row. Here it is a SUBRANGE of ten, which is why the
// constant is `10` and not the `1` every sibling site carries — it is derived
// from THIS table, never copied from a neighbour.
inline constexpr auto kDataSectionKindNames =
    namesWhere<6>(kSectionKindTable, isDataSectionKind);
static_assert(kSectionKindTable.rows.size()
                  == kDataSectionKindNames.size() + 10,
              "kSectionKindTable must have exactly TEN rows that are NOT "
              "producer-emittable (the walker-synthesized kinds, for which "
              "`dataSectionKindOf` returns nullopt) — an eleventh leaves "
              "`namesWhere`'s literal count matching while every 'the closed "
              "set is …' message silently stops describing the split "
              "`isDataSectionKind` actually enforces. If the new kind IS "
              "producer-emittable, widen `DataSectionKind` + `dataSectionKindOf` "
              "and the `namesWhere` count; if it is not, widen this constant.");

// ⚠ DERIVED, NOT A SECOND IF-CHAIN. This used to spell all six names again,
// beside `kSectionKindTable` which already owned them and beside
// `dataSectionKindName` which already read them from it — three owners of one
// fact, of which only the round-trip `static_assert`s above happened to keep
// two honest (they pin the ENUM VALUES, never the SPELLINGS, so a renamed
// section would have passed every one of them while
// `dataSectionKindFromName` silently stopped resolving it).
[[nodiscard]] constexpr std::optional<DataSectionKind>
dataSectionKindFromName(std::string_view s) noexcept {
    auto const k = sectionKindFromName(s);
    return k.has_value() ? dataSectionKindOf(*k) : std::nullopt;
}

// D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): the ONE chokepoint for the section a
// RELOC-BEARING global lands in. A pointer object patched by the loader (an
// `int *p = &x;` scalar, a fn-ptr-table / `&global` aggregate member, or a
// linker-minted cross-CU indirection slot) cannot sit in read-only `.rodata`;
// it routes to a thread-local template (Tdata), relocated-read-only const
// (RelRoConst — gcc's `.data.rel.ro`), or writable data (Data). The asm-tier
// global lowering AND the linker's cross-CU merge both route through here, so
// the const-vs-mutable relro decision has a SINGLE point of truth (a duplicated
// ternary is what breeds a missed site — the c154 D-LK-DYN-RODATA-ITEM-RELOC
// wall was exactly a second site minting Rodata). Thread-locality wins first —
// a reloc-bearing `thread_local` keeps its per-thread `.tdata` template (never
// demoted to a process-shared slot).
[[nodiscard]] constexpr DataSectionKind
relocBearingGlobalSection(bool isThreadLocal, bool isConst) noexcept {
    if (isThreadLocal) return DataSectionKind::Tdata;
    return isConst ? DataSectionKind::RelRoConst : DataSectionKind::Data;
}

static_assert(relocBearingGlobalSection(true,  true)  == DataSectionKind::Tdata);
static_assert(relocBearingGlobalSection(true,  false) == DataSectionKind::Tdata);
static_assert(relocBearingGlobalSection(false, true)  == DataSectionKind::RelRoConst);
static_assert(relocBearingGlobalSection(false, false) == DataSectionKind::Data);

} // namespace dss
