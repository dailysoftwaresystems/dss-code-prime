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
#include <functional>   // std::hash — specialized for SectionKindEncoding at the foot of this file
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
    // D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE:
    // PER-FUNCTION UNWIND METADATA, and the taxonomy's THIRD species -- neither
    // code nor data. Every one of the fifteen kinds above answers "what do
    // these bytes BECOME in the image": text becomes instructions, the data
    // family becomes storage, the table kinds are structure the walker
    // synthesizes. An unwind section answers a different question entirely: it
    // DESCRIBES OTHER SECTIONS' CODE. Its bytes are consumed by the LINKER (and
    // then by the runtime unwinder through whatever table the linker chose to
    // synthesize), never merged into text or data, and its own extent is not
    // addressable program state.
    //
    // ★ WHY IT IS NOT `Debug`, WHICH IS THE TEMPTING BUCKET AND IS WRONG.
    //   ✔MEASURED 2026-08-24 (Apple clang 21.0.0, macOS 26.5.2, `otool -l` on a
    //   stock `clang -c` object): `__LD,__compact_unwind` carries section flags
    //   0x02000000 = S_ATTR_DEBUG, so the wire really does tag it with the
    //   debug attribute. That attribute is a LINKER instruction ("do not load
    //   this into the image"), not a statement about what the bytes mean --
    //   compact unwind is consumed at RUNTIME by the unwinder, and stripping
    //   debug info from a binary must not remove it. Filing it under `Debug`
    //   would make "strip the debug sections" and "keep the unwind tables" the
    //   same predicate, which is exactly the silent-wrongness this taxonomy
    //   exists to prevent.
    //
    // ★ UNIVERSAL, NOT A MACH-O SPELLING. Mach-O `__LD,__compact_unwind`,
    //   ELF `.eh_frame`/`.eh_frame_hdr` and PE `.pdata`/`.xdata` are the same
    //   species; each format document names its own. Only the two Mach-O
    //   RELOCATABLE documents declare a row today, because the Mach-O reader
    //   is the only one that has been taught to consume one -- a format that
    //   declares no row keeps the old, correct refusal for a section it cannot
    //   classify.
    //
    // NOT a `DataSectionKind`: no producer in this codebase emits an unwind
    // section as `AssembledData`. DSS states a function's unwind rules in the
    // NEUTRAL `CfiFunction` vocabulary (`AssembledFunction::cfi`) and each
    // format writer encodes its own table from that, so a producer reaching for
    // this kind would be spelling a format's table by hand.
    Unwind     = 16, // per-function unwind metadata (linker-consumed)
    // D-LK-OBJECT-CARRIES-NO-SUMMARY-OR-MIR-SECTION: THE LINK-TIME-OPTIMIZATION
    // PAIR, and the taxonomy's FOURTH species. The fifteen storage/table kinds
    // answer "what do these bytes become in the image" and `Unwind` answers
    // "what do they describe about other sections". These answer neither: they
    // are the COMPILER'S OWN INPUT, carried through the object so a LATER whole-
    // program pass can read it. Nothing in the image corresponds to them, and
    // the loader never sees them — every format's row below is declared
    // non-ALLOC (ELF: no SHF_ALLOC; PE: IMAGE_SCN_MEM_DISCARDABLE and no
    // MEM_READ; Mach-O: S_ATTR_DEBUG, the same "do not load this" instruction
    // the `__compact_unwind` row already carries).
    //
    // ★★★ WHY TWO KINDS AND NOT ONE SECTION, WHICH IS THE WHOLE ECONOMY. The
    // summary is a small per-module DIGEST — which symbols this unit defines,
    // which it references, what each one costs — and the MIR is the module's
    // whole body. A global pass reads EVERY summary to decide what to import,
    // and it must do that WITHOUT PAGING IN ONE BYTE OF MIR; then it reads the
    // MIR of the few modules it chose. One combined section makes that
    // impossible: the digest could not be read without the body behind it. The
    // split IS the ThinLTO economy, and it is the reason this is a pair.
    //
    // ⚠ NOT THE `SectionEncoding` AXIS, and the difference from `Unwind` is
    // exactly what that axis is for. `__compact_unwind` and `__eh_frame` are ONE
    // role in TWO WIRE ENCODINGS — a consumer picks either and learns the same
    // facts. Summary and IR are two DIFFERENT PAYLOADS: no decoding of the MIR
    // yields the summary cheaply, which is the entire point. Two roles.
    //
    // ⚠ NOT `Debug` EITHER, for the reason the `Unwind` note above already
    // states in its own case: "strip the debug sections" must not mean "throw
    // away the link-time optimizer's input". They share the non-loaded wire
    // attribute on some formats; they are not the same species.
    //
    // ⓘ `Lto*` AND NOT `Dss*`. The ROLE is the industry's — LLVM carries
    // `.llvmbc`, GCC `.gnu.lto_*` — while the SPELLING is per-format vocabulary
    // (`.dss.summary` / `.dss.mir`, `.dsssum` / `.dssmir`, `__DSS,__summary` /
    // `__DSS,__mir`), which is where DSS's own identity belongs. Naming the
    // universal kind after this compiler's IR would put a product name in the
    // format-blind vocabulary every tier speaks.
    LtoSummary = 17, // per-module link-time-optimization summary index
    LtoIr      = 18, // the module's serialized IR body, for a link-time pass
};

inline constexpr EnumNameTable<SectionKind, 19> kSectionKindTable{{{
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
    { SectionKind::Unwind,     "unwind"     },
    { SectionKind::LtoSummary, "lto-summary" },
    { SectionKind::LtoIr,      "lto-ir"      },
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

// ═══════════════════════════════════════════════════════════════════════
// The WIRE ENCODING of a section's contents — the SECOND half of a section
// row's identity, for the kinds whose ROLE does not determine it.
//
// D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE.
// ═══════════════════════════════════════════════════════════════════════
//
// ★★ WHY THE ENCODING IS A SECOND AXIS AND NOT TWO MORE `SectionKind` ROWS.
//   ✔MEASURED 2026-08-25 against Apple clang on real Apple Silicon (macOS
//   26.5.2), `/usr/bin/cc -arch <a> -c foreign.c` with no other flag: the
//   x86_64 object carries BOTH `__LD,__compact_unwind` AND
//   `__TEXT,__eh_frame`; the arm64 object from the SAME compiler and the SAME
//   source carries compact ONLY. Two sections, ONE role ("per-function unwind
//   metadata"), TWO encodings -- so the kind alone stopped identifying a row,
//   and the loader's kind-uniqueness rule refused the document outright.
//
//   An `UnwindCompact` / `UnwindDwarf` split of `SectionKind` is the arm THIS
//   FILE'S OWN docblock vetoes: the enum is the set of "format-blind names the
//   substrate engine speaks", and "per-format JSON owns the name + flags".
//   Compact-vs-DWARF is precisely a per-format ENCODING fact, so moving it
//   into the ROLE taxonomy would re-encode FORMAT IDENTITY into the shared
//   vocabulary every tier speaks.
//
//   ⚠ `ShStrtab` SPLITTING FROM `Strtab` LOOKS LIKE THE PRECEDENT AND IS NOT
//   ONE. That pair splits on CONSUMER PATH -- "find the names of OTHER
//   SECTIONS" versus "find SYMBOL names" -- and BOTH concepts exist in every
//   format. An unwind encoding does not divide that way, and the measurement
//   above is the proof: DWARF CFI is not an ELF fact (a Mach-O object carries
//   it) and compact unwind is not a Mach-O fact (it is an encoding a format
//   may adopt). They are encodings, and this is the encoding axis.
//
// ★ THE PAYOFF IS THE NEXT ONE, NOT THIS ONE. ARM EHABI (`.ARM.exidx`) and
//   Win64 SEH (`.pdata`/`.xdata`) are further encodings of the SAME role.
//   Each becomes a row in this table plus a decoder -- never an edit to
//   `SectionKind`, and never a reader testing a section NAME.
enum class SectionEncoding : std::uint8_t {
    // The document makes NO encoding claim about this row. EVERY section row
    // that predates this axis carries it, and it stays CORRECT for them: a
    // kind whose format declares exactly one row needs no discriminator. A
    // reader that MUST know the encoding refuses an `Unspecified` row BY NAME
    // rather than assuming whichever encoding its own format happens to use --
    // assuming is how a decoder reads a table of noise and reports success.
    Unspecified   = 0,
    // DWARF Call Frame Information: CIE + FDE records, PC-keyed, one FDE per
    // described function (ELF `.eh_frame`, Mach-O `__TEXT,__eh_frame`).
    DwarfCfi      = 1,
    // Apple compact unwind: fixed-width `{fnStart, len, encoding, personality,
    // lsda}` records with NO PC dimension -- it describes the frame in the
    // function BODY only (Mach-O `__LD,__compact_unwind`).
    CompactUnwind = 2,
};

inline constexpr EnumNameTable<SectionEncoding, 3> kSectionEncodingTable{{{
    { SectionEncoding::Unspecified,   "unspecified"    },
    { SectionEncoding::DwarfCfi,      "dwarf-cfi"      },
    { SectionEncoding::CompactUnwind, "compact-unwind" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR.
DSS_CHECK_ENUM_NAME_TABLE(kSectionEncodingTable);

[[nodiscard]] constexpr std::string_view
sectionEncodingName(SectionEncoding e) noexcept {
    return kSectionEncodingTable.name(e);
}
[[nodiscard]] constexpr std::optional<SectionEncoding>
sectionEncodingFromName(std::string_view s) noexcept {
    return kSectionEncodingTable.fromName(s);
}

// May a format document SPELL this encoding on a section row? `Unspecified`
// is the ABSENCE of the key, so spelling it would give one state two
// spellings -- the second-owner shape this schema rejects by name elsewhere.
[[nodiscard]] constexpr bool
sectionEncodingIsDeclarable(SectionEncoding e) noexcept {
    return e != SectionEncoding::Unspecified;
}

// The spellings an `encoding` key may use. Same sentinel shape as
// `kSelectableObjectFormatKindNames` one vocabulary over: the rejected set is
// the single `Unspecified` row, so the constant is `1` and the `static_assert`
// pins the REJECTED count against the table's own row total -- two numbers
// with different owners, not a tautology.
inline constexpr auto kDeclarableSectionEncodingNames =
    namesWhere<2>(kSectionEncodingTable, sectionEncodingIsDeclarable);
static_assert(kSectionEncodingTable.rows.size()
                  == kDeclarableSectionEncodingNames.size() + 1,
              "kSectionEncodingTable must have exactly ONE row that a format "
              "document may NOT spell (`Unspecified`, which IS the absence of "
              "the key). A second unspellable row leaves `namesWhere`'s "
              "literal count matching while every 'the closed set is …' "
              "message silently stops describing what the loader accepts.");

// May a format document declare MORE THAN ONE section row of this KIND,
// told apart by their declared `SectionEncoding`?
//
// ★ TRUE FOR EXACTLY THE KINDS WHOSE CONTENTS ARE A *DESCRIPTION* WITH MORE
//   THAN ONE STANDARD WIRE ENCODING -- and which therefore no WRITER resolves
//   by kind alone. That second clause is what makes this predicate load-bearing
//   rather than decorative: `ObjectFormatSchema::sectionByKind` answers "the
//   row of this kind", and it can only stay honest if every kind a writer asks
//   it about is unique. ✔MEASURED 2026-08-25 (grep over `src/`):
//   `SectionKind::Unwind` appears in the two relocatable-object READERS and
//   nowhere else -- no writer emits into an unwind row, because DSS states its
//   own functions' unwind rules in the neutral `CfiFunction` vocabulary and
//   each format writer encodes its table from that.
//
// ⚠ NOT A LIST OF FORMATS AND NOT A LIST OF ENCODINGS -- a property of the
// ROLE. A format that declares one unwind row is unaffected; the discriminator
// exists for the document that declares two.
[[nodiscard]] constexpr bool
sectionKindIsEncodingDiscriminated(SectionKind k) noexcept {
    return k == SectionKind::Unwind;
}

// The kinds a document may declare TWICE, rendered from the predicate rather
// than retyped. `namesWhere` refuses a literal that disagrees with the number
// of accepted rows AT COMPILE TIME, so promoting a second role to
// encoding-discriminated cannot leave a diagnostic naming only the old one.
inline constexpr auto kEncodingDiscriminatedKindNames =
    namesWhere<1>(kSectionKindTable, sectionKindIsEncodingDiscriminated);

// The full identity of a section ROW: its universal ROLE, plus the WIRE
// ENCODING where the role does not determine it. THIS -- never the kind alone
// -- is what a format document's rows must be unique on.
struct SectionKindEncoding {
    SectionKind     kind{};
    SectionEncoding encoding = SectionEncoding::Unspecified;

    [[nodiscard]] friend constexpr bool
    operator==(SectionKindEncoding, SectionKindEncoding) noexcept = default;
};

// Narrow subset of `SectionKind` that a producer can legitimately
// emit via `AssembledData`. Closes D-LK4-RODATA-SECTION-NARROW.
//
// Of the 19 `SectionKind` values, 13 are NOT producer-emittable
// (`Text` = executable code; `Symtab`/`Strtab`/`ShStrtab` = symbol
// + name tables; `RelocTable` = relocation entries; `Dynamic` =
// dynamic linking metadata; `Note` = vendor notes; `Debug` =
// DWARF/CodeView; `ThreadVars` = the Mach-O tlv descriptors, minted
// by the WRITER and never by a producer; `Unwind` = per-function
// unwind metadata, which arrives on the READ side from a foreign
// object and which DSS's own producers state as `CfiFunction`
// instead; `LtoSummary`/`LtoIr` = the link-time-optimization pair,
// which the SUMMARY-INDEX writer emits as whole sections rather than
// as `AssembledData` items a producer places; `Custom` =
// format-specific anything). A producer constructing
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
// the build if the 6/13 split ever moves — and it EARNED that keep
// TWICE: on 2026-08-24 when `Unwind` landed, and again on 2026-08-26
// when the `LtoSummary`/`LtoIr` pair did, the assert going red on the
// same compile that added the enumerators, which is what dragged this
// prose forward with it instead of letting it rot again.
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
// the 13 non-producer-emittable kinds.
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

// Can a DEFINED SYMBOL inside a section of this kind START AN ATOM — i.e. does
// the section contribute LINKABLE BODY BYTES that the symbol names?
//
// D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE.
// Every relocatable-object reader in `src/link/format/` asks this question, and
// each used to answer it by testing `Text` and then `dataSectionKindOf(...)`
// separately — two conditions with no name, which is how a THIRD answer becomes
// invisible. It is stated once here, DERIVED from the two facts that already
// exist (`Text` is the code kind, `isDataSectionKind` is the storage subrange),
// so a future kind is classified by construction rather than by whichever
// reader happened to be edited.
//
// ★ FALSE FOR `Unwind` IS THE WHOLE POINT, and it is not the same statement as
//   "drop the section". A clang-emitted `__LD,__compact_unwind` carries a local
//   section label (`ltmp1`) at offset 0, and under MH_SUBSECTIONS_VIA_SYMBOLS
//   that label looks exactly like an atom boundary. It is not one: the bytes it
//   labels are metadata ABOUT other sections' atoms, so slicing them into an
//   atom would mint a body the image must then place somewhere. The reader
//   handles such a section explicitly instead — never by guessing, and never by
//   silence.
//
// ⚠ ONLY MEANINGFUL FOR A KIND THAT RESOLVED. A section whose (name/segment)
// matched no format-document row has NO kind, and that case must keep reaching
// its reader's loud refusal — `false` here means "classified, and classified as
// something that holds no body", which is a completely different claim from
// "unclassified".
[[nodiscard]] constexpr bool sectionKindCarriesLinkableBody(SectionKind k) noexcept {
    return k == SectionKind::Text || isDataSectionKind(k);
}

static_assert( sectionKindCarriesLinkableBody(SectionKind::Text));
static_assert( sectionKindCarriesLinkableBody(SectionKind::Rodata));
static_assert( sectionKindCarriesLinkableBody(SectionKind::Data));
static_assert( sectionKindCarriesLinkableBody(SectionKind::Bss));
static_assert( sectionKindCarriesLinkableBody(SectionKind::ThreadData));
static_assert( sectionKindCarriesLinkableBody(SectionKind::ThreadBss));
static_assert( sectionKindCarriesLinkableBody(SectionKind::RelRoConst));
static_assert(!sectionKindCarriesLinkableBody(SectionKind::Unwind));
static_assert(!sectionKindCarriesLinkableBody(SectionKind::Debug));
static_assert(!sectionKindCarriesLinkableBody(SectionKind::Symtab));
static_assert(!sectionKindCarriesLinkableBody(SectionKind::ThreadVars));
// D-LK-OBJECT-CARRIES-NO-SUMMARY-OR-MIR-SECTION: neither LTO row holds body
// bytes a defined symbol names. A summary/IR section is the COMPILER'S input,
// so a symbol found inside one must never start an atom — the reader would
// then mint a body the image has to place, which is the exact failure the
// `Unwind` row's note above records for `__compact_unwind`'s `ltmp1` label.
static_assert(!sectionKindCarriesLinkableBody(SectionKind::LtoSummary));
static_assert(!sectionKindCarriesLinkableBody(SectionKind::LtoIr));
static_assert(!isDataSectionKind(SectionKind::LtoSummary));
static_assert(!isDataSectionKind(SectionKind::LtoIr));
// The two spellings resolve, and they resolve to DIFFERENT kinds — the pair is
// the whole point, so a table row that accidentally spelled both the same way
// would silently collapse the summary and the body into one.
static_assert(sectionKindFromName("lto-summary") == SectionKind::LtoSummary);
static_assert(sectionKindFromName("lto-ir")      == SectionKind::LtoIr);
static_assert(SectionKind::LtoSummary != SectionKind::LtoIr);

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
// set is a single sentinel row. Here it is a SUBRANGE of thirteen, which is why
// the constant is `13` and not the `1` every sibling site carries — it is
// derived from THIS table, never copied from a neighbour.
inline constexpr auto kDataSectionKindNames =
    namesWhere<6>(kSectionKindTable, isDataSectionKind);
static_assert(kSectionKindTable.rows.size()
                  == kDataSectionKindNames.size() + 13,
              "kSectionKindTable must have exactly THIRTEEN rows that are NOT "
              "producer-emittable (the walker-synthesized kinds, the "
              "linker-consumed `Unwind` metadata kind, and the "
              "`LtoSummary`/`LtoIr` link-time-optimization pair, for all of "
              "which `dataSectionKindOf` returns nullopt) — a fourteenth leaves "
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

// `SectionKindEncoding` is a MAP KEY (the format schema's row index), so it
// needs a hash. Two `std::uint8_t` fields pack losslessly into 16 bits, which
// makes this exact rather than a mix -- and the `static_assert` refuses the
// packing the day either field outgrows a byte, instead of letting two
// distinct identities silently collide into one row.
template <>
struct std::hash<dss::SectionKindEncoding> {
    [[nodiscard]] std::size_t
    operator()(dss::SectionKindEncoding v) const noexcept {
        static_assert(sizeof(dss::SectionKind) == 1
                          && sizeof(dss::SectionEncoding) == 1,
                      "the packing below is lossless only while both halves "
                      "are one byte wide");
        return static_cast<std::size_t>(
            (static_cast<unsigned>(v.kind) << 8) | static_cast<unsigned>(v.encoding));
    }
};
