#include "link/format/coff_object_reader.hpp"
#include "link/format/foreign_section_alignment.hpp"
#include "link/format/object_atom_coverage.hpp"
#include "link/format/object_format_backends.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Windows COFF `.obj` reader -- the inverse of pe.cpp's Obj-arm writer. See
// the header for the reconstruction contract + scope + the COFF-vs-ELF/
// Mach-O inversions. Every field is bounds-checked; any violation fails
// loud (F_* diagnostic + nullopt).

namespace dss::pe {

namespace {

using dss::report;

// -- PE/COFF structural constants (PE Format spec, Microsoft Learn) ----
//
// The SAME record layout the writer in pe.cpp hardcodes. Re-declared
// locally rather than #include-pulled from `ffi/binary_readers/*`: `ffi`
// already depends UP on `link` (`link/object_format_schema.hpp`), so a
// `link` -> `ffi` include would form a dependency cycle. This mirrors the
// c164 ELF + c168 Mach-O readers re-declaring the same constants to keep
// `ffi` off `link`.
constexpr std::size_t kFileHeaderSz    = 20;  // IMAGE_FILE_HEADER
constexpr std::size_t kSectionHeaderSz = 40;  // IMAGE_SECTION_HEADER
constexpr std::size_t kSymbolSz        = 18;  // IMAGE_SYMBOL
constexpr std::size_t kRelocSz         = 10;  // IMAGE_RELOCATION

// IMAGE_FILE_HEADER field offsets.
constexpr std::size_t kFhNumSectionsOff  = 2;   // u16 NumberOfSections
constexpr std::size_t kFhSymTabPtrOff    = 8;   // u32 PointerToSymbolTable
constexpr std::size_t kFhNumSymbolsOff   = 12;  // u32 NumberOfSymbols
constexpr std::size_t kFhOptHdrSizeOff   = 16;  // u16 SizeOfOptionalHeader

// IMAGE_SECTION_HEADER field offsets.
constexpr std::size_t kShNameOff          = 0;   // 8 bytes (inline or /N)
constexpr std::size_t kShSizeOfRawDataOff = 16;  // u32
constexpr std::size_t kShPtrRawDataOff    = 20;  // u32
constexpr std::size_t kShPtrRelocsOff     = 24;  // u32
constexpr std::size_t kShNumRelocsOff     = 32;  // u16
constexpr std::size_t kShCharsOff         = 36;  // u32

// IMAGE_SYMBOL field offsets.
constexpr std::size_t kSymNameOff    = 0;   // 8 bytes (inline or [0][offset])
constexpr std::size_t kSymValueOff   = 8;   // u32
constexpr std::size_t kSymSectNumOff = 12;  // i16 (read as u16, specials below)
constexpr std::size_t kSymTypeOff    = 14;  // u16
constexpr std::size_t kSymClassOff   = 16;  // u8
constexpr std::size_t kSymNumAuxOff  = 17;  // u8

// IMAGE_RELOCATION field offsets.
constexpr std::size_t kRelVirtAddrOff = 0;  // u32 (section-relative patch site)
constexpr std::size_t kRelSymIdxOff   = 4;  // u32 (symtab index)
constexpr std::size_t kRelTypeOff     = 8;  // u16 (== schema nativeId)

// IMAGE_SYM_CLASS_*
constexpr std::uint8_t kSymClassExternal = 2;
// STATIC (3) is COFF's INTERNAL-LINKAGE class -- "defined here, invisible to
// other objects". It is NOT a shape: a file-local (`static`) FUNCTION, a
// file-local DATA object, a section-definition symbol and DSS's synthetic block
// labels are all class STATIC. What separates them is the DERIVED-TYPE field
// below (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM).
constexpr std::uint8_t kSymClassStatic   = 3;
// WEAK_EXTERNAL (105 == 0x69) -- an UNDEF name that DEFERS TO ANOTHER NAME.
// PE/COFF 5.5.3's own words: a weak external is "a symbol table record with
// EXTERNAL storage class, UNDEF section number, and a value of zero" plus an
// auxiliary record naming the symbol to use instead. Decoded in `weakExternDefault`.
constexpr std::uint8_t kSymClassWeakExternal = 105;
//
// ── THE CLASSES THAT NAME NO RECONSTRUCTIBLE BODY ──────────────────────────
//
// COFF's storage class is an ON-THE-WIRE ENUM, and until this list existed the
// dispatch below tested for the two classes it modeled and let EVERY OTHER
// VALUE fall into the interior-block-label bucket. That is the partial-and-
// silent shape GATE 3 was deliberately written NOT to have, twenty lines away
// in this same file, and it is what let class 105 be read as a plain strong
// extern. The dispatch is now TOTAL: a class either resolves to a
// `CoffSymbolRole` or FAILS LOUD, so the next producer to use a class nobody
// modeled gets a diagnostic instead of a silent reclassification.
//
// These are the classes whose SPEC DEFINITION is a debug/bookkeeping record or
// a code address interior to a body -- never a body DSS reconstructs. They keep
// EXACTLY the behaviour the old open-ended fallback gave them (a bodiless
// ModuleSymbol staged for the geometry pass), which is why adding the
// enumeration changes no existing reconstruction.
// ✔VERIFIED value-by-value against this host's Windows SDK
// `um/winnt.h` (10.0.26100.0) `IMAGE_SYM_CLASS_*` block -- NOT quoted from
// memory, and NOT from the reader's own prior belief.
constexpr std::uint8_t kSymClassNull           = 0;
constexpr std::uint8_t kSymClassAutomatic      = 1;
constexpr std::uint8_t kSymClassRegister       = 4;
constexpr std::uint8_t kSymClassExternalDef    = 5;
constexpr std::uint8_t kSymClassLabel          = 6;    // a code address IN a module
constexpr std::uint8_t kSymClassUndefinedLabel = 7;
constexpr std::uint8_t kSymClassMemberOfStruct = 8;
constexpr std::uint8_t kSymClassArgument       = 9;
constexpr std::uint8_t kSymClassStructTag      = 10;
constexpr std::uint8_t kSymClassMemberOfUnion  = 11;
constexpr std::uint8_t kSymClassUnionTag       = 12;
constexpr std::uint8_t kSymClassTypeDefinition = 13;
constexpr std::uint8_t kSymClassUndefinedStatic= 14;
constexpr std::uint8_t kSymClassEnumTag        = 15;
constexpr std::uint8_t kSymClassMemberOfEnum   = 16;
constexpr std::uint8_t kSymClassRegisterParam  = 17;
constexpr std::uint8_t kSymClassBitField       = 18;
constexpr std::uint8_t kSymClassBlock          = 100;  // `.bb` / `.eb`
constexpr std::uint8_t kSymClassFunction       = 101;  // `.bf` / `.ef` / `.lf`
constexpr std::uint8_t kSymClassEndOfStruct    = 102;
constexpr std::uint8_t kSymClassFile           = 103;  // the source file name
constexpr std::uint8_t kSymClassSection        = 104;
constexpr std::uint8_t kSymClassClrToken       = 107;
constexpr std::uint8_t kSymClassEndOfFunction  = 0xFFu;  // (BYTE)-1

// ── AUXILIARY FORMAT 3 (PE/COFF 5.5.3, "Weak Externals") ───────────────────
//
// The 18-byte auxiliary record that follows a WEAK_EXTERNAL symbol:
//   [0]  u32 TagIndex         -- the symtab index of the DEFAULT symbol (sym2)
//   [4]  u32 Characteristics  -- the search policy for sym1
//   [8]  10 bytes unused
// ✔VERIFIED, and by a CROSS-CHECK rather than by trusting one source: the
// SDK's `IMAGE_AUX_SYMBOL_EX::Sym` spells the same two fields in the same order
// at the same offsets as `WeakDefaultSymIndex` (DWORD @0) + `WeakSearchType`
// (DWORD @4) -- so BOTH the ORDER and the 32-bit WIDTH are confirmed by a
// declaration independent of the prose. (`IMAGE_AUX_SYMBOL_EX` is the bigobj
// 20-byte form; the two leading DWORDs are shared with the 18-byte form.)
constexpr std::size_t kAuxWeakExternTagIndexOff        = 0;  // u32
constexpr std::size_t kAuxWeakExternCharacteristicsOff = 4;  // u32

// IMAGE_WEAK_EXTERN_SEARCH_* -- the Characteristics vocabulary, ✔VERIFIED
// against the same `winnt.h`.
constexpr std::uint32_t kWeakExternSearchNoLibrary = 1;
constexpr std::uint32_t kWeakExternSearchLibrary   = 2;
constexpr std::uint32_t kWeakExternSearchAlias     = 3;
// ANTI_DEPENDENCY(4) and every other value -> FAIL LOUD (see the decode).
//
// ⚠⚠ CHARACTERISTICS IS **NOT** THE DEFINITION-VS-UNRESOLVABLE DISCRIMINATOR,
// AND BELIEVING IT WAS IS A MEASURED ERROR THIS CYCLE ALMOST SHIPPED.
// The plan for this reader said to route on it: ALIAS(3) => bind the name to
// the canonical, SEARCH_*(1,2) => a weak undefined reference. ✔MEASURED on
// mingw gcc 13.2.0 (`objdump -t` plus a raw 18-byte aux dump, because objdump
// renders a format-3 aux as though it were a function aux and never prints
// Characteristics at all), over FOUR source shapes:
//     `__attribute__((weak))` on a FUNCTION      -> Characteristics 1
//     `__attribute__((weak))` on a DATA object   -> Characteristics 1
//     `__attribute__((weak, alias("real_fn")))`  -> Characteristics 1
//     an UNDEFINED `extern __attribute__((weak))`-> Characteristics 1
// It is CONSTANT at NOLIBRARY(1) across every shape gcc produces, so routing on
// it would have classified every gcc weak DEFINITION as an unresolvable
// reference -- i.e. left the body under the `.weak.<n>.<n>` name and kept
// reporting "undefined symbol" for the name that IS defined, which is the exact
// defect this arm exists to remove.
// ★ What the field actually states is the SEARCH POLICY FOR sym1, which is a
// different question from what sym2 IS. The role is stated by the record's OWN
// TagIndex: what the DEFAULT symbol is. A section-backed default is a body this
// name resolves to; a non-section-backed one is not. **Route on that.**
// (This is D-LK-MACHO-ISDATA-NO-CALL-SIGNAL's lesson arriving from the other
// direction -- there a reader used a relocation's arithmetic FORMULA as a proxy
// for its ROLE; here a plan proposed using a SEARCH POLICY as a proxy for a
// DEFINITION STATE. Both substitute a field that correlates for the field that
// states.)

// IMAGE_SECTION_NUMBER specials (SectionNumber is a signed i16).
constexpr std::uint16_t kSymUndefined = 0x0000u;  // extern
constexpr std::uint16_t kSymAbsolute  = 0xFFFFu;  // -1
constexpr std::uint16_t kSymDebug     = 0xFFFEu;  // -2

// IMAGE_SYM_TYPE_* -- the high byte carries the derived (DTYPE) hint;
// The IMAGE_SYMBOL derived-type (bits 4..5): DTYPE_FUNCTION marks the symbol
// as a FUNCTION (the isData signal). The full 2-bit mask distinguishes it from
// DT_ARY (0x30) / DT_PTR (0x10), which are DATA -- a bare `& DTYPE_FUNCTION`
// would misread DT_ARY as a function.
//
// ★ THIS FIELD CARRIES TWO DECISIONS, NOT ONE. It is the extern's isData class
// AND -- for a DEFINED symbol -- the ATOM-BOUNDARY discriminator that COFF's
// missing size field would otherwise leave undecidable: a class-STATIC symbol
// that DECLARES ITSELF A FUNCTION is a whole file-local function (a body, an
// atom), while one that does not is an interior block label / a data object /
// a section identity. ✔MEASURED on three independent producers of the same
// `static int helper(int); int entry(int);` source -- DSS's own `pe.cpp`, MSVC
// cl.exe 14.51.36231, and mingw gcc (Strawberry) -- ALL THREE stamp
// `(type 0x20)(class 3)` on the static function and type 0 on everything else
// class STATIC (`$unwind$`/`$pdata$` thunks, section-definition symbols, block
// labels). So this is a universal convention, not a DSS round-trip trick.
constexpr std::uint16_t kSymDtypeMask     = 0x30u;
constexpr std::uint16_t kSymDtypeFunction = 0x20u;

// Section Characteristics: a zero-fill (bss) section stores NO file bytes
// (PointerToRawData == 0, the span rides SizeOfRawData). The structural
// COFF analog of Mach-O's S_ZEROFILL -- used ONLY for the file-bounds
// exemption; the section KIND is resolved from the schema name map.
constexpr std::uint32_t kScnCntUninitializedData = 0x00000080u;

// -- IMAGE_SCN_ALIGN_*BYTES: COFF's spelling of the declared section
//    alignment (PE Format spec, "Section Flags") -----------------------------
//
// D-FORMAT-MACHO-SECTION-ALIGN-EMITTED-RAW-NOT-LOG2 (the read-side half of the
// "one right answer per format" sweep that row prescribes).
//
// Bits [23:20] hold a CLASS ORDINAL, not an exponent and not a byte count:
// class N means 2^(N-1) bytes, so class 5 = 16 and class 6 = 32, and the range
// 1..14 spans 1..8192. Class 0 means the producer declared nothing.
//
// ⚠ THIS READER USED TO DECODE NOTHING AT ALL. ✔MEASURED 2026-08-27: the word
// `Alignment` did not appear in this file, and neither `AssembledData`
// construction site set `.alignment`, so every foreign COFF data item reached
// the merge at the newtype's default of ONE byte no matter what the producer
// asked for -- silently, because dropping a constraint yields a merge that
// succeeds. Its two sibling readers (`elf_object_reader.cpp` via
// `sh_addralign`, `macho_object_reader.cpp` via `section_64.align`) had both
// carried the field since they were written.
//
// ★ THE END-TO-END CONSEQUENCE IS NARROWER THAN "EVERY OVER-ALIGNED DATUM WAS
// MISPLACED", AND THE NARROWING IS MEASURED -- three corpus shapes were built
// with this decode and without it and exited IDENTICALLY before one bit. The
// reader slices atoms by their VALUE, so a producer's inter-item PADDING is
// absorbed into the preceding atom's extent: relative offsets inside ONE
// module survive regardless, and an over-aligned object FIRST in its section
// is aligned by construction. What this field decides is where a member's
// whole block LANDS once something else already occupies the section --
// `exec_data_section.hpp` places `src` at `alignUp(into.spanSize,
// src.maxAlign)`, and `maxAlign` is the max over these values. ✔MEASURED on
// the PE leg with a consumer contributing five bytes of its own rodata: the
// archive member's over-aligned datum sits at `mod 256 == 128` with this
// decode and at `69` without it. `examples/c/staticlib_alignas_carry` is that
// witness.
//
// ✔MEASURED against the references, separately, on an `_Alignas(32) const`
// object: mingw-gcc and clang(--target=x86_64-pc-windows-msvc) BOTH stamp
// `.rdata` with IMAGE_SCN_ALIGN_32BYTES, and both stamp an explicit class on
// EVERY section -- so the dropped field was carrying real producer intent on
// ordinary input, not a theoretical corner.
//
// ★ CLASS 0 DECODES TO 16 BYTES, AND THAT IS MEASURED RATHER THAN ASSUMED.
// ✔MEASURED 2026-08-27 by zeroing the class nibble of a real `.rdata` header
// in place and re-reading it: GNU binutils `objdump -h` reports `2**5` (32)
// with class 6 and `2**4` (16) with class 0, and `lld-link` accepts the
// class-0 object (rc=0). LLVM's own `coff_section::getAlignment()` spells the
// same default. Reading class 0 as "1 byte" would put DSS BELOW every
// reference reader on identical bytes -- and in the under-aligning direction,
// which is the one that miscompiles rather than merely wastes padding.
//
// IMAGE_SCN_TYPE_NO_PAD is the legacy spelling of ALIGN_1BYTES and is honoured
// for the same reason: it is what the references do with these bytes.
constexpr std::uint32_t kScnAlignMask  = 0x00F00000u;
constexpr std::uint32_t kScnAlignShift = 20u;
constexpr std::uint32_t kScnTypeNoPad  = 0x00000008u;

// Byte alignment the producer declared for `chars`, as the shared read-side
// policy carries it (`link/format/foreign_section_alignment.hpp`): a
// re-layout hint, degrading to byte alignment above what the newtype models.
[[nodiscard]] Alignment
alignFromCharacteristics(std::uint32_t chars) noexcept {
    if ((chars & kScnTypeNoPad) != 0u) return Alignment{};
    std::uint32_t const cls = (chars & kScnAlignMask) >> kScnAlignShift;
    // Class 0 = unspecified = the references' 16-byte default = exponent 4.
    // Otherwise the class ordinal is one MORE than the log2 exponent.
    return link::format::foreignSectionAlignmentFromLog2(
        cls == 0u ? 4u : cls - 1u);
}

// IMAGE_SCN_LNK_COMDAT: this section participates in COMDAT
// duplicate-resolution. A real cl.exe/clang-cl `.obj` places each
// function-level-linked (`/Gy`) / `__declspec(selectany)` / inline / template
// body in its OWN COMDAT section; the duplicate-resolution policy lives in the
// section-definition auxiliary record's Selection byte (decoded below --
// D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION).
// ⚠ THIS BIT IS NO LONGER FOREIGN-ONLY. It said so until D-LK-OBJECT-WEAK-DEF-RELOCATABLE
// landed: COFF has no per-symbol weak-DEFINITION encoding, so DSS's
// own writer now spells every weak definition as a per-body IMAGE_SCN_LNK_COMDAT
// section with Selection = IMAGE_COMDAT_SELECT_ANY (`pe.cpp`, the COMDAT
// sections walk). The COMDAT path below therefore runs on DSS's OWN output as
// well as on a foreign `.obj` (D-LK-COFF-READER-FOREIGN-OBJECT), and is
// round-tripped as such -- `PeWriter.ObjectWeakDefinitionRoundTripsBackToWeak
// ThroughDssReader` and `PeWriter.ObjectWeakDefinedDataEmitsOwnComdatAndRound
// Trips` both write with `pe::encode` and read back through this reader.
// Weakening this decode to suit a foreign producer would now silently change
// what DSS reads back from its own objects.
constexpr std::uint32_t kScnLnkComdat = 0x00001000u;

// IMAGE_COMDAT_SELECT_* -- the COMDAT Selection byte (aux format 5, offset 14).
// Maps to the universal SymbolBinding the format-blind merge already resolves.
constexpr std::uint8_t kComdatSelNoDuplicates = 1;  // -> Strong (Global)
constexpr std::uint8_t kComdatSelAny          = 2;  // -> Weak
constexpr std::uint8_t kComdatSelSameSize     = 3;  // -> Weak
constexpr std::uint8_t kComdatSelExactMatch   = 4;  // -> Weak
// ASSOCIATIVE(5) / LARGEST(6) / 0 / unknown -> FAIL LOUD (see the decode).

// Section-definition auxiliary record (aux format 5) field offset: the
// Selection byte sits at offset 14 of the 18-byte aux record (after Length[4],
// NumberOfRelocations[2], NumberOfLinenumbers[2], CheckSum[4], Number[2]).
constexpr std::size_t kAuxSectionDefSelectionOff = 14;

// ── THE "EXTERN IS A FUNCTION" SIGNAL IS DECLARED, NOT DERIVED ──────────
//
// This file used to carry an `isCallBranchFormula` helper that answered the
// question from the TARGET row's arithmetic formula (`Aarch64Call26`). It is
// GONE (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL) and must not come back: a formula
// describes ARITHMETIC, and the question is about ROLE. It answered "no" for
// every PE row, which was the right answer for the wrong reason -- and the
// wrong reason is the one that would have generalised.
//
// ✔MEASURED 2026-08-20 against the four shipped pe64 documents: COFF x86_64
// declares IMAGE_REL_AMD64_REL32 / ADDR64 / ADDR32 / SECREL and NOT ONE of
// them is branch-only (REL32 is the call displacement AND the `lea rip+d`
// data displacement), so none may declare `isCall` and PE also declares no
// `pltNativeId`. `callSignalNativeIds` is therefore EMPTY on every shipped
// PE format -- which costs nothing HERE, because COFF is the one family of
// the three that carries an INDEPENDENT class hint in its symbol table
// (IMAGE_SYMBOL.Type's DTYPE_FUNCTION nibble), read in step (6). That is
// also why this reader has no Mach-O-style empty-set refusal: an empty set
// leaves a real hint standing rather than a guess.
//
// The consumption below is kept and now reads the DECLARED role, because the
// PE family does contain a branch-only relocation the moment a non-x86_64 PE
// target lands -- IMAGE_REL_ARM64_BRANCH26 -- and that document will declare
// `isCall` and be honoured here untouched.

// Overflow-safe [off, off+size) within [0, total) -- the c159-c168
// `rangeExceedsBuffer` shape (subtraction, never `off + size` which wraps
// on a hostile/corrupted header).
[[nodiscard]] constexpr bool
rangeExceedsBuffer(std::uint64_t off, std::uint64_t size, std::uint64_t total) noexcept {
    return off > total || size > total - off;
}

// LE scalar readers -- every call site is preceded by a rangeExceedsBuffer
// gate proving [o, o+N) is in-bounds.
[[nodiscard]] std::uint16_t rdU16(std::span<std::uint8_t const> b, std::size_t o) noexcept {
    return static_cast<std::uint16_t>(b[o]) | (static_cast<std::uint16_t>(b[o + 1]) << 8);
}
[[nodiscard]] std::uint32_t rdU32(std::span<std::uint8_t const> b, std::size_t o) noexcept {
    return  static_cast<std::uint32_t>(b[o])
         | (static_cast<std::uint32_t>(b[o + 1]) <<  8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16)
         | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

// NUL-terminated name at strtab[index], bounded by [tabStart, tabEnd).
[[nodiscard]] std::string
rdName(std::span<std::uint8_t const> b, std::uint64_t tabStart, std::uint64_t tabEnd,
       std::uint32_t index) {
    if (tabEnd > b.size()) tabEnd = b.size();  // defense-in-depth
    std::uint64_t const start = tabStart + index;
    if (start >= tabEnd) return {};
    std::uint64_t end = start;
    while (end < tabEnd && b[end] != 0u) ++end;
    return std::string{reinterpret_cast<char const*>(&b[start]),
                       static_cast<std::size_t>(end - start)};
}

// Decode a COFF 8-byte name field at `o`. Two encodings -- the inverse of
// `pe.cpp`'s "PE/COFF name encoding" block (its `NameField` / `emitSymWithName`;
// the old positional citation here had drifted onto SEH unwind-code emission):
//   * INLINE: the name (<= 8 bytes) NUL-padded in the field -- the bytes up
//     to the first NUL (or all 8 if unterminated).
//   * OFFSET: the first 4 bytes are ZERO, the next 4 are a string-table
//     offset (>= 4, past the u32 size prefix) -- the name is the
//     NUL-terminated string there. An offset < 4 (only via a corrupt/empty
//     field) yields an empty name (the writer never emits offset < 4).
// The 8-byte field's in-bounds-ness is proven by the caller's record bound;
// the string-table read is bounded by [strTabStart, strTabEnd).
[[nodiscard]] std::string
rdCoffName(std::span<std::uint8_t const> b, std::size_t o,
           std::uint64_t strTabStart, std::uint64_t strTabEnd) {
    if (rdU32(b, o) == 0u) {
        std::uint32_t const offset = rdU32(b, o + 4);
        if (offset < 4u) return {};
        return rdName(b, strTabStart, strTabEnd, offset);
    }
    std::size_t n = 0;
    while (n < 8u && b[o + n] != 0u) ++n;
    return std::string{reinterpret_cast<char const*>(&b[o]), n};
}

// Sign-extend the low `width` bytes of `raw` to a signed 64-bit value --
// the inverse of the writer truncating an int64 addend to `widthBytes` LE
// in the patched data slot (`pe.cpp`'s `buildDataRelocTable` and the ADDEND
// paragraph above it; the old positional citation here had drifted onto extern
// symbol-index assignment). width is 4 or 8 (the non-pcrel
// Linear kinds a data slot uses -- schema invariant (a)).
[[nodiscard]] std::int64_t signExtendLE(std::uint64_t raw, std::uint8_t width) noexcept {
    if (width >= 8u) return static_cast<std::int64_t>(raw);
    unsigned const bits = static_cast<unsigned>(width) * 8u;
    std::uint64_t const mask = (static_cast<std::uint64_t>(1) << bits) - 1u;
    std::uint64_t v = raw & mask;
    std::uint64_t const signBit = static_cast<std::uint64_t>(1) << (bits - 1u);
    if ((v & signBit) != 0u) v |= ~mask;  // extend the sign into the high bytes
    return static_cast<std::int64_t>(v);
}

// One parsed IMAGE_SECTION_HEADER (only the fields the reader consumes).
// The 1-based COFF ordinal is this section's index in `sections` PLUS one.
struct Section {
    std::string   name;
    std::uint64_t rawSize   = 0;   // SizeOfRawData
    std::uint64_t rawPtr    = 0;   // PointerToRawData (0 for a bss/zero-fill)
    std::uint64_t relocPtr  = 0;   // PointerToRelocations
    std::uint32_t relocCount = 0;  // NumberOfRelocations
    std::uint32_t chars     = 0;   // Characteristics
    bool          zeroFill  = false;
    std::optional<SectionKind> kind;  // resolved from the name via the schema
};

// One decoded IMAGE_SYMBOL (only the fields the reader consumes).
struct Sym {
    std::string   name;
    std::uint64_t value   = 0;   // ALREADY section-relative (COFF convention)
    std::uint16_t sectNum = 0;   // 1-based ordinal / 0 / 0xFFFF / 0xFFFE
    std::uint16_t type    = 0;
    std::uint8_t  storage = 0;
};

// ── THE STORAGE-CLASS ROLE — a TOTAL map, in GATE 3's shape ────────────────
//
// The four roles this reader's symbol model actually distinguishes. Every COFF
// storage class resolves to exactly one of them or FAILS LOUD; there is no
// "everything else" arm. `Bodiless` is a ROLE, not a fallback: it is the set of
// classes whose SPEC DEFINITION is a debug/bookkeeping record or an address
// interior to another symbol's body.
enum class CoffSymbolRole {
    External,      // 2   -- a definition or reference visible to other objects
    Static,        // 3   -- COFF's internal-linkage class (see kSymClassStatic)
    WeakExternal,  // 105 -- an UNDEF name that defers to another name (5.5.3)
    Bodiless,      // names no body DSS reconstructs (debug records, labels, ...)
};

// Resolve a storage class to its role. `nullopt` == UNMODELED -> the caller
// FAILS LOUD. Deliberately a switch over the raw wire value with no `default:`
// swallow, so adding a class is a compile-visible edit here rather than a
// silent behaviour change at every use site.
[[nodiscard]] constexpr std::optional<CoffSymbolRole>
roleForStorageClass(std::uint8_t storage) noexcept {
    switch (storage) {
        case kSymClassExternal:     return CoffSymbolRole::External;
        case kSymClassStatic:       return CoffSymbolRole::Static;
        case kSymClassWeakExternal: return CoffSymbolRole::WeakExternal;
        // Bodiless: a C-debug bookkeeping record, a type/scope tag, or a code
        // address INTERIOR to a body. None names a body; each keeps exactly the
        // treatment the pre-total dispatch's open-ended fallback gave it.
        case kSymClassNull:
        case kSymClassAutomatic:
        case kSymClassRegister:
        case kSymClassExternalDef:
        case kSymClassLabel:
        case kSymClassUndefinedLabel:
        case kSymClassMemberOfStruct:
        case kSymClassArgument:
        case kSymClassStructTag:
        case kSymClassMemberOfUnion:
        case kSymClassUnionTag:
        case kSymClassTypeDefinition:
        case kSymClassUndefinedStatic:
        case kSymClassEnumTag:
        case kSymClassMemberOfEnum:
        case kSymClassRegisterParam:
        case kSymClassBitField:
        case kSymClassBlock:
        case kSymClassFunction:
        case kSymClassEndOfStruct:
        case kSymClassFile:
        case kSymClassSection:
        case kSymClassClrToken:
        case kSymClassEndOfFunction:
            return CoffSymbolRole::Bodiless;
        default:
            return std::nullopt;
    }
}

// A defined symbol staged for atom slicing: its section-relative Value IS an
// atom boundary. Every linkage reaches here -- an EXTERNAL symbol, a
// class-STATIC one that declares DTYPE_FUNCTION (a file-local function), a
// class-STATIC one in a NON-CODE section (a file-local data object), and
// whatever the geometry fallback recovers -- and `binding` is what tells them
// apart downstream. See the classification loop and `coff_object_reader.hpp`
// clause (7).
struct DefSym {
    std::uint32_t    symIdx    = 0;
    std::uint64_t    secRelOff = 0;
    std::string      name;
    SymbolBinding    binding    = SymbolBinding::Global;
    SymbolVisibility visibility = SymbolVisibility::Default;
    // Set only for a symbol the GEOMETRY FALLBACK promoted: it was first
    // recorded as a bodiless `ModuleSymbol` and only later found to start a
    // body, so the slicing loop must not push a SECOND `ModuleSymbol` for it.
    // Suppressing the duplicate here rather than de-duplicating afterwards
    // keeps `mod.symbols` in symbol-table order, which is the order every
    // existing round-trip pin reads it in.
    bool moduleSymbolAlreadyPushed = false;
    // D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED: what this definition
    // promises about the copies it may be folded against, decoded from the
    // COMDAT Selection byte in (5.5). `Any` for every non-COMDAT symbol and for
    // IMAGE_COMDAT_SELECT_ANY, which is the pre-existing behaviour verbatim.
    DuplicateMatch duplicateMatch = DuplicateMatch::Any;
};

// A reconstructed [start, start+len) byte range within one section, plus the
// output-vector index of the AssembledFunction / AssembledData it backs --
// used to route a relocation site to its owning item.
struct Interval {
    std::uint64_t start  = 0;
    std::uint64_t len    = 0;
    std::size_t   outIdx = 0;
};

} // namespace

std::optional<AssembledModule>
readRelocatableObject(std::span<std::uint8_t const> bytes,
                      TargetSchema const&            targetSchema,
                      ObjectFormatSchema const&      objectFormatSchema,
                      DiagnosticReporter&            reporter,
                      CompilationUnitId              cuId) {
    auto fail = [&](DiagnosticCode code, std::string detail)
        -> std::optional<AssembledModule> {
        report(reporter, code, DiagnosticSeverity::Error, std::move(detail));
        return std::nullopt;
    };

    // -- (0) Format sanity: this reader speaks PE/COFF only ----------
    // ── SELF-GUARD (TF-C125) ─────────────────────────────────────────
    //    ⚠ THIS CITATION WAS ELIDED TO `D-LINK-…-KIND-IDENTITY-BRANCHES`,
    //    which matches no id and so pointed at nothing any grep could find.
    //    The id, whole and on its own line:
    //    D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES
    //
    // ★★ THIS GUARD SURVIVED THE IDENTITY-BRANCH REMOVAL, AND THE REASON IS
    // MEASURED FOR THIS SITE. The TF-C125 brief expected it to become
    // redundant: with walkers reached only through a backend the loader
    // resolved, a walker "can never be handed a schema of another kind", so
    // the guard would be unreachable by construction and safely deletable.
    //
    // That premise is FALSE here. `pe::readRelocatableObject` is a PUBLIC free function with
    // 22 direct call sites in `tests/`, none of which route through the
    // linker — and `CoffObjectReader.NonPeFormatSchemaFailsLoud`
    // (tests/link/test_coff_object_reader.cpp) hands it a FOREIGN schema on purpose and asserts this
    // exact refusal. Deleting the guard would not remove dead code; it would
    // delete tested behaviour and leave a public entry point that mis-encodes
    // silently. Refused, with evidence.
    //
    // ⚠ THE CITATION ABOVE IS PER-SITE ON PURPOSE. The first version of this
    // comment was one block pasted into all eight guards, every copy naming
    // the ELF writer's test as its proof — so seven of the eight cited a
    // measurement that was not about them. An independent audit caught it.
    // A comment stamped MEASURED that names the wrong measurement is worse
    // than no comment, under this project's own rule.
    //
    // What it stops being is an IDENTITY branch. It no longer compares an
    // enumerator; it compares the schema's resolved backend against the
    // singleton THIS TU implements — a pointer identity on an opaque handle,
    // in the sanctioned realization tier, which is exactly the tier permitted
    // to know which format it is. Unreachable from the linker (the resolver
    // cannot produce a mismatched pair), live for every direct caller.
    if (objectFormatSchema.backend() != &link::format::peBackend()) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            std::string{"pe::readRelocatableObject: object format schema '"}
                + std::string{objectFormatSchema.name()} + "' is kind "
                + std::string{link::objectFormatBackendName(objectFormatSchema.backend())}
                + ", not PE/COFF -- the COFF reader cannot parse it.");
    }

    // -- (1) IMAGE_FILE_HEADER ---------------------------------------
    if (bytes.size() < kFileHeaderSz) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "pe::readRelocatableObject: file shorter than IMAGE_FILE_HEADER "
            "(20 bytes).");
    }
    std::uint16_t const numSections   = rdU16(bytes, kFhNumSectionsOff);
    std::uint64_t const symTabPtr     = rdU32(bytes, kFhSymTabPtrOff);
    std::uint32_t const numSymbols    = rdU32(bytes, kFhNumSymbolsOff);
    std::uint16_t const optHeaderSize = rdU16(bytes, kFhOptHdrSizeOff);
    // A `.obj` relocatable has SizeOfOptionalHeader == 0. A NON-zero optional
    // header means a PE IMAGE (an .exe/.dll -- a link OUTPUT, not a
    // relocatable input) -- fail loud like the ELF reader's `e_type != ET_REL`
    // and the Mach-O reader's `filetype != MH_OBJECT`.
    if (optHeaderSize != 0u) {
        return fail(DiagnosticCode::F_UnsupportedBinaryFormat,
            "pe::readRelocatableObject: SizeOfOptionalHeader="
            + std::to_string(optHeaderSize) + " is non-zero -- this is a PE "
              "IMAGE (executable / DLL, a link OUTPUT), not a relocatable "
              ".obj; only relocatable objects are read back into a mergeable "
              "module.");
    }

    // Section-header array [20, 20 + 40*NumberOfSections) (optHdr == 0).
    std::uint64_t const sectTableOff = kFileHeaderSz;  // + optHeaderSize (== 0)
    std::uint64_t const sectTableBytes =
        static_cast<std::uint64_t>(numSections) * kSectionHeaderSz;
    if (rangeExceedsBuffer(sectTableOff, sectTableBytes, bytes.size())) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "pe::readRelocatableObject: section header table ["
            + std::to_string(sectTableOff) + ", +"
            + std::to_string(sectTableBytes) + ") for "
            + std::to_string(numSections) + " sections runs past EOF (file "
            + std::to_string(bytes.size()) + ").");
    }

    // Symbol table [PointerToSymbolTable, +18*NumberOfSymbols).
    if (numSymbols > 0u && symTabPtr == 0u) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "pe::readRelocatableObject: PointerToSymbolTable is 0 but "
            "NumberOfSymbols=" + std::to_string(numSymbols) + " -- corrupt "
            "file header.");
    }
    std::uint64_t const symTabBytes =
        static_cast<std::uint64_t>(numSymbols) * kSymbolSz;
    if (rangeExceedsBuffer(symTabPtr, symTabBytes, bytes.size())) {
        return fail(DiagnosticCode::F_CorruptedBinary,
            "pe::readRelocatableObject: symbol table (PointerToSymbolTable="
            + std::to_string(symTabPtr) + " + " + std::to_string(symTabBytes)
            + " bytes for " + std::to_string(numSymbols) + " entries) runs "
              "past EOF.");
    }
    // COFF string table: immediately after the symbol table -- a u32 total
    // size prefix (INCLUSIVE of the 4 bytes) + NUL-terminated long names. A
    // symbol/section long name is the offset (>= 4) into this table.
    std::uint64_t const strTabStart = symTabPtr + symTabBytes;
    std::uint64_t strTabEnd = strTabStart;
    if (numSymbols > 0u) {
        // The 4-byte size prefix must be present (the writer always emits at
        // least the prefix). Its declared size must not run past EOF.
        if (rangeExceedsBuffer(strTabStart, 4u, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: COFF string-table size prefix at "
                + std::to_string(strTabStart) + " runs past EOF.");
        }
        std::uint64_t const strTabSize = rdU32(bytes, static_cast<std::size_t>(strTabStart));
        if (strTabSize >= 4u
            && rangeExceedsBuffer(strTabStart, strTabSize, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: COFF string table (size="
                + std::to_string(strTabSize) + " at offset "
                + std::to_string(strTabStart) + ") runs past EOF.");
        }
        strTabEnd = strTabStart + std::max<std::uint64_t>(strTabSize, 4u);
    }

    // -- (2) Section headers: resolve names + bodies + reloc tables --
    //
    // Each file-backed section's [PointerToRawData, +SizeOfRawData) must be
    // in-bounds (except a bss/zero-fill section: PointerToRawData == 0, the
    // span rides SizeOfRawData only). Its reloc table [PointerToRelocations,
    // +10*NumberOfRelocations) must be in-bounds. The section KIND is
    // resolved from the schema name map below (agnostic).
    std::vector<Section> sections(numSections);  // 1-based ordinal = index + 1
    for (std::uint16_t i = 0; i < numSections; ++i) {
        std::size_t const so =
            static_cast<std::size_t>(sectTableOff) + static_cast<std::size_t>(i) * kSectionHeaderSz;
        Section& sec = sections[i];
        sec.rawSize    = rdU32(bytes, so + kShSizeOfRawDataOff);
        sec.rawPtr     = rdU32(bytes, so + kShPtrRawDataOff);
        sec.relocPtr   = rdU32(bytes, so + kShPtrRelocsOff);
        sec.relocCount = rdU16(bytes, so + kShNumRelocsOff);
        sec.chars      = rdU32(bytes, so + kShCharsOff);
        sec.zeroFill   = (sec.chars & kScnCntUninitializedData) != 0u;
        sec.name       = rdCoffName(bytes, so + kShNameOff, strTabStart, strTabEnd);
        // A file-backed section's body must lie within the file. A zero-fill
        // (bss) section carries no file bytes (PointerToRawData == 0), so it
        // is exempt -- exactly like the ELF reader exempts SHT_NOBITS and the
        // Mach-O reader exempts S_ZEROFILL.
        if (!sec.zeroFill
            && rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: section '" + sec.name + "' body ["
                + std::to_string(sec.rawPtr) + ", +" + std::to_string(sec.rawSize)
                + ") runs past EOF.");
        }
        std::uint64_t const relocBytes =
            static_cast<std::uint64_t>(sec.relocCount) * kRelocSz;
        if (rangeExceedsBuffer(sec.relocPtr, relocBytes, bytes.size())) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: section '" + sec.name
                + "' relocation table (PointerToRelocations="
                + std::to_string(sec.relocPtr) + ", NumberOfRelocations="
                + std::to_string(sec.relocCount) + ") runs past EOF.");
        }
    }

    // -- (3) Resolve each section's SectionKind from the NAME, agnostic --
    //
    // COFF has NO segment (unlike Mach-O's (segment,section) pair). The
    // object schema declares TWO rows named `.rdata` -- `rodata` (no
    // relocations) and `relro` (reloc-bearing const data, RelRoConst) -- that
    // are header-identical. `nameToKind` is FIRST-WINS (the same `emplace` over
    // `objectFormatSchema.sections()` that `elf::readRelocatableObject` builds
    // its own `nameToKind` with) so it
    // maps `.rdata` -> the rodata (first) row. `nameToRelroKind` holds the
    // RelRoConst-kind row per name. The disambiguator is RELOC-PRESENCE: a
    // `.rdata` section carrying its own IMAGE_RELOCATION table takes the relro
    // row, a reloc-free one the rodata row. This is the COFF analog of
    // Mach-O's segment-pair key AND the semantic essence -- relro IS "const
    // data that carries load-time relocations" -- so a re-emission routes a
    // reloc-bearing const item to a section that permits relocations. Agnostic
    // (schema rows + universal reloc-presence; no hardcoded `.rdata`).
    std::unordered_map<std::string, SectionKind> nameToKind;
    std::unordered_map<std::string, SectionKind> nameToRelroKind;
    for (auto const& row : objectFormatSchema.sections()) {
        nameToKind.emplace(row.name, row.kind);
        if (row.kind == SectionKind::RelRoConst) {
            nameToRelroKind.emplace(row.name, row.kind);
        }
    }
    for (auto& sec : sections) {
        // GATE 1 (D-LK-COFF-READER-FOREIGN-OBJECT): COFF `$`-grouped section
        // names. A real cl.exe/clang-cl object emits `.text$mn` / `.rdata$r` /
        // `.xdata` -- the `$<suffix>` groups contributions the FINAL linker
        // concatenates within the base section (`$` is the COFF group
        // separator, the analog of ELF's `.` in `.text.<fn>`). Truncate at the
        // FIRST `$` to the BASE name, then route the BASE through the EXISTING
        // two-map reloc-presence logic. DSS emits ungrouped names (no `$`), so
        // this is a strict superset -- a `$`-less name is its own base. We do
        // NOT clone ELF's single-kind longest-prefix resolver: the two
        // header-identical `.rdata` rows (rodata vs relro) are disambiguated
        // ONLY by reloc-presence, which the longest-prefix collapse would lose.
        std::string const base = sec.name.substr(0, sec.name.find('$'));
        if (auto it = nameToKind.find(base); it != nameToKind.end()) {
            sec.kind = it->second;
        }
        if (sec.relocCount > 0u) {
            if (auto it = nameToRelroKind.find(base); it != nameToRelroKind.end()) {
                sec.kind = it->second;  // reloc-bearing const -> the relro row
            }
        }
    }

    // -- (4) Reverse reloc map (nativeId -> RelocationKind), from the
    //         FORMAT SCHEMA -- no hardcoded IMAGE_REL_AMD64_* numbers --------
    //
    // `callSignalNativeIds` collects the native ids that PROVE an extern
    // reached through them is a FUNCTION: the rows the FORMAT declares
    // `"isCall": true` on. No shipped PE document declares one (COFF x86_64
    // has no branch-only relocation -- REL32 serves data too) and PE declares
    // no `pltNativeId` variant either, so this set is EMPTY on every shipped
    // PE format and the extern's isData comes from the IMAGE_SYMBOL type hint
    // (step 5/7) instead. Read from the schema rather than inferred -- see the
    // note above the byte helpers.
    //
    // Built by `ObjectFormatSchema::relocationDecodeTable()`. This reader used
    // to own the loop, and it omitted the `emitOnly` exclusion the ELF reader's
    // copy had: an EMISSION ALIAS would have entered the map, and since
    // `validate()` guarantees the alias carries a different `kind` from the row
    // owning its wire id, a PE document declaring one would have been refused
    // as an ambiguous reverse map -- rejecting every object of that format.
    // LOUD, and no shipped PE document declares one, so nothing ever
    // mis-decoded; the alias capability was simply unavailable here.
    auto decode = objectFormatSchema.relocationDecodeTable();
    if (!decode) {
        return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: " + decode.error());
    }
    auto const& nativeToKind        = decode->nativeToKind;
    auto const& callSignalNativeIds = decode->callSignalNativeIds;

    // -- (5) Decode every IMAGE_SYMBOL; assign SymbolId = symtab index ----
    //
    // A record's ordinal is NOT its symbol index, and it never was safe to
    // assume so: a record declaring NumberOfAuxSymbols>0 is followed by that
    // many AUX slots that are NOT IMAGE_SYMBOLs. They are SKIPPED (not decoded)
    // and marked in `auxSlot`, the cursor advances by `1 + numAux`, and a reloc
    // naming an aux slot fails loud below.
    // ⚠ THIS IS NOT A FOREIGN-OBJECT-ONLY CONCERN ANY MORE. This comment used to
    // read "DSS emits ZERO auxiliary records, so a record's ordinal equals its
    // symbol index" -- true when it was written and false since
    // D-LK-OBJECT-WEAK-DEF-RELOCATABLE: DSS's own writer now emits BOTH aux
    // shapes -- a section-definition aux (format 5) on every COMDAT section
    // symbol it mints for a weak definition, and an Auxiliary Format 3 record on
    // every WEAK_EXTERNAL alias. So DSS's own `.obj` shifts symbol indices past
    // its aux slots, and the `1 + numAux` walk is what keeps a relocation's
    // symbol index meaning the same record on the way back in as it did on the
    // way out (`PeWriter.RelocationToAWeakDefinitionResolvesPastTheAuxSlot`).
    std::vector<Sym>  syms(numSymbols);
    std::vector<bool> auxSlot(numSymbols, false);
    // A `std::size_t` cursor (not u32): the `+= 1 + numAux` skip past a
    // foreign record's auxiliary slots can never wrap on a hostile
    // NumberOfSymbols (which would spin the loop) -- 64-bit index arithmetic.
    for (std::size_t i = 0; i < numSymbols;) {
        std::size_t const so =
            static_cast<std::size_t>(symTabPtr) + i * kSymbolSz;
        Sym& s = syms[i];
        s.name    = rdCoffName(bytes, so + kSymNameOff, strTabStart, strTabEnd);
        s.value   = rdU32(bytes, so + kSymValueOff);
        s.sectNum = rdU16(bytes, so + kSymSectNumOff);
        s.type    = rdU16(bytes, so + kSymTypeOff);
        s.storage = bytes[so + kSymClassOff];
        std::uint8_t const numAux = bytes[so + kSymNumAuxOff];
        for (std::size_t a = 1; a <= numAux && i + a < numSymbols; ++a) {
            auxSlot[i + a] = true;
        }
        i += static_cast<std::size_t>(1) + numAux;
    }

    // -- (5.1) The two symbol predicates the passes below SHARE -----------
    //
    // Both were previously spelled inline at each use site; a decision written
    // twice is a decision that can disagree with itself, so each lives once.

    // Does this IMAGE_SYMBOL's derived type declare a FUNCTION? Drives BOTH the
    // extern's isData class (step 6) and the defined symbol's atom-boundary
    // classification (step 6, and see `kSymDtypeFunction`).
    auto declaresFunction = [](Sym const& s) {
        return (s.type & kSymDtypeMask) == kSymDtypeFunction;
    };

    // Is symtab record `k` a SECTION-DEFINITION symbol -- a section IDENTITY
    // rather than a body? PE/COFF 5.5.5 gives it two properties TOGETHER: its
    // NAME is the section's own name, and its FIRST auxiliary record is the
    // format-5 section definition. BOTH are required here.
    //
    // ⚠ THE AUX RECORD ALONE IS NOT THE RECOGNISER, and believing it was is a
    // measured bug. ✔MEASURED on mingw gcc (Strawberry), with AND without `-g`:
    // a file-local function is emitted as `(sec 1)(ty 20)(scl 3)(nx 1)` -- class
    // STATIC, DTYPE_FUNCTION, and ONE auxiliary record (COFF aux format 1, the
    // function definition). An aux-only test therefore calls gcc's static
    // FUNCTION a section identity. That is wrong twice over: it would exempt the
    // very symbol whose loss this reader must never allow, and -- because gcc
    // emits that record BEFORE the `.text` section symbol of the same ordinal --
    // it would hand Gate 3 a function-definition aux record to read a COMDAT
    // Selection byte out of. cl.exe 14.51.36231 attaches no aux to either a
    // static or an external function, so this shape is producer-specific and
    // exactly the kind a name test settles and an aux test cannot.
    auto isSectionDefinitionSymbol = [&](std::size_t k) {
        Sym const& s = syms[k];
        if (s.storage != kSymClassStatic) return false;
        if (s.sectNum < 1u || s.sectNum > numSections) return false;
        if (s.name != sections[s.sectNum - 1u].name) return false;
        std::size_t const auxIdx = k + 1u;
        return auxIdx < numSymbols && auxSlot[auxIdx];
    };

    // -- (5.2) WEAK EXTERNALS: decode Auxiliary Format 3 (PE/COFF 5.5.3) ----
    //
    // A WEAK_EXTERNAL record names sym1 (always UNDEF) and its auxiliary record
    // names sym2, the DEFAULT used when sym1 is not otherwise defined. Both
    // facts live in the AUX record, which the symbol loop used to discard whole
    // (`if (auxSlot[i]) continue;`) -- so the two fields that make the record
    // mean anything were thrown away before anything looked at them.
    //
    // ★ THE ROLE COMES FROM WHAT sym2 IS, NOT FROM Characteristics -- see the
    // measurement recorded at `kWeakExternSearchNoLibrary`. This returns sym2's
    // symtab index; the caller reads sym2's own SectionNumber to decide what the
    // record means. Every structural violation FAILS LOUD here rather than
    // yielding a half-decoded record: a weak external the reader cannot decode
    // is a name whose binding it would otherwise silently promote to strong.
    //
    // Returns `nullopt` AFTER emitting a diagnostic (the caller propagates).
    auto weakExternDefault =
        [&](std::uint32_t i) -> std::optional<std::uint32_t> {
        Sym const& s = syms[i];
        auto refuse = [&](std::string detail) -> std::optional<std::uint32_t> {
            fail(DiagnosticCode::F_CorruptedBinary,
                 "pe::readRelocatableObject: weak external '" + s.name + "' "
                 + std::move(detail)
                 + " D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB.");
            return std::nullopt;
        };
        // 5.5.3 states the record's own shape as a conjunction; check it rather
        // than assume it, because a record that is NOT that shape is not a weak
        // external and reading its aux as format 3 would decode arbitrary bytes
        // as a symbol index.
        if (s.sectNum != kSymUndefined) {
            return refuse("has SectionNumber " + std::to_string(
                              static_cast<std::int16_t>(s.sectNum))
                          + " but PE/COFF 5.5.3 requires UNDEF (0) -- a weak "
                            "external DEFINES nothing.");
        }
        if (s.value != 0u) {
            return refuse("has Value " + std::to_string(s.value)
                          + " but PE/COFF 5.5.3 requires zero.");
        }
        std::size_t const auxIdx = static_cast<std::size_t>(i) + 1u;
        if (auxIdx >= numSymbols || !auxSlot[auxIdx]) {
            return refuse("carries no auxiliary record, so the DEFAULT symbol it "
                          "defers to (Auxiliary Format 3 TagIndex) is unstated. "
                          "Refusing to read it as a plain strong extern -- that "
                          "would silently promote a name that may legally be "
                          "overridden or absent.");
        }
        std::size_t const ao =
            static_cast<std::size_t>(symTabPtr) + auxIdx * kSymbolSz;
        std::uint32_t const tagIndex =
            rdU32(bytes, ao + kAuxWeakExternTagIndexOff);
        std::uint32_t const characteristics =
            rdU32(bytes, ao + kAuxWeakExternCharacteristicsOff);
        // A TOTAL enumeration, GATE 3's shape. All three modelled values
        // reconstruct the SAME relation here, and the reader must accept all
        // three because the producers disagree about which to write: gcc emits
        // NOLIBRARY(1) for every weak shape, while DSS's own writer emits
        // ALIAS(3) -- ✔MEASURED as the only value under which a foreign linker
        // resolves the alias from another object (see `pe.cpp`'s
        // `IMAGE_WEAK_EXTERN_SEARCH_ALIAS`). The difference between them is a
        // LINKER SEARCH POLICY for sym1, which the object tier does not act on:
        // DSS's resolution set is the modules actually merged, not an import
        // library search order. ANTI_DEPENDENCY(4) is an MSVC-internal marker
        // whose whole point is that sym1 must NOT force sym2 to be pulled from
        // an archive; treating it as a plain weak external would change which
        // members a static link pulls, so it FAILS LOUD instead.
        switch (characteristics) {
            case kWeakExternSearchNoLibrary:
            case kWeakExternSearchLibrary:
            case kWeakExternSearchAlias:
                break;
            default:
                return refuse("has Auxiliary Format 3 Characteristics "
                              + std::to_string(characteristics)
                              + ", which is neither SEARCH_NOLIBRARY(1), "
                                "SEARCH_LIBRARY(2) nor SEARCH_ALIAS(3). "
                                "ANTI_DEPENDENCY(4) and unknown values are not "
                                "modeled -- refusing to default a weak-external "
                                "policy.");
        }
        if (tagIndex >= numSymbols) {
            return refuse("names default symbol index " + std::to_string(tagIndex)
                          + ", past the symbol table's " + std::to_string(numSymbols)
                          + " records.");
        }
        if (auxSlot[tagIndex]) {
            return refuse("names default symbol index " + std::to_string(tagIndex)
                          + ", which is an AUXILIARY slot, not a symbol.");
        }
        return tagIndex;
    };

    // -- (5.5) GATE 3: COMDAT selection -> the section's external-symbol binding.
    //
    // (D-LK-COFF-READER-FOREIGN-OBJECT.) A real cl.exe/clang-cl `.obj` places
    // each `/Gy` / `__declspec(selectany)` / inline / template body in its OWN
    // IMAGE_SCN_LNK_COMDAT section. COFF encodes the duplicate-resolution policy
    // NOT on the symbol but in the section-definition AUXILIARY record (aux
    // format 5) of the section's STATIC section symbol: the `Selection` byte.
    // We map it to the universal SymbolBinding the merge already resolves, so
    // the EXISTING all-weak dedup (resolveCrossCuDefs lowest-key-wins +
    // linker.cpp isShadowedDuplicate body-drop) folds cross-object COMDAT
    // duplicates with ZERO merge change. The selection switch is a TOTAL
    // enumeration -- every kind-resolved COMDAT section maps to a binding or
    // FAILS LOUD; a selection is never silently defaulted (a wrong default
    // silently mis-dedups).
    //
    // ★★ AND THE BINDING IS ONLY HALF OF WHAT THE SELECTION BYTE SAYS --
    // D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED. ANY(2), SAME_SIZE(3)
    // and EXACT_MATCH(4) all lift to Weak, but the last two additionally
    // PROMISE something about the copies -- equal LENGTH, or equal BYTES -- and
    // the format specifies a violation as "a multiply defined symbol error".
    // Folding all three lowest-key with no comparison meant DSS silently
    // accepted exactly the input the format tells it to reject. The duty
    // travels with the binding, as `DuplicateMatch`, and the cross-CU fold
    // discharges it (`link/cross_cu_resolve.cpp`).
    //
    // ⚠ ONE MAP CARRYING A PAIR, NOT TWO PARALLEL MAPS. The binding and the
    // duty are read off the SAME aux byte at the SAME moment and are consumed
    // at the same site; two maps keyed on the same ordinal is the shape that
    // acquires an entry in one and not the other, and the failure would be a
    // COMDAT lifted to Weak with its promise silently downgraded to `Any` --
    // i.e. this exact defect, reintroduced by bookkeeping.
    struct ComdatPolicy {
        SymbolBinding  binding;
        DuplicateMatch duty;
    };
    std::unordered_map<std::uint16_t, ComdatPolicy> comdatBindingBySection;
    for (std::uint16_t si = 0; si < numSections; ++si) {
        Section const& sec = sections[si];
        if ((sec.chars & kScnLnkComdat) == 0u) continue;  // not a COMDAT section
        // Only a COMDAT section whose KIND resolved (real code/data --
        // `.text$mn`/`.data`/`.rdata`) reconstructs a BODY whose external
        // symbol we lift + whose wrong-size selection is a miscompile risk. A
        // COMDAT section with UNRESOLVED kind is unmodeled metadata -- a real
        // `/Gy` object marks its `.pdata`/`.xdata` COMDAT with
        // ASSOCIATIVE(5) selection tying them to the function COMDAT -- which
        // reconstructs NO body + has NO external defined symbol to lift, and is
        // SKIPPED whole by Gate 2 (D-LK-COFF-FOREIGN-UNWIND-DROP). Reading its
        // selection would fail loud on ASSOCIATIVE for a section we drop anyway
        // (blocking every `/Gy`-compiled object). Gate 3 owns kind-RESOLVED
        // COMDAT (the miscompile surface); Gate 2 owns the kind-UNRESOLVED
        // metadata -- the same kind split both gates key on.
        if (!sec.kind.has_value()) continue;
        std::uint16_t const ordinal = static_cast<std::uint16_t>(si + 1u);
        // The section-definition symbol for THIS ordinal (the shared 5.5.5
        // recogniser above -- NAME matches the section AND an aux record
        // follows). Its Selection byte is the policy. Taking the first
        // aux-bearing STATIC symbol of the ordinal instead would read a gcc
        // static function's format-1 aux record as a Selection byte, because
        // gcc emits that record ahead of the section symbol.
        std::optional<std::uint8_t> selection;
        for (std::uint32_t k = 0; k < numSymbols; ++k) {
            if (auxSlot[k]) continue;                          // an aux slot, not a symbol
            if (syms[k].sectNum != ordinal) continue;
            if (!isSectionDefinitionSymbol(k)) continue;
            std::size_t const auxIdx = static_cast<std::size_t>(k) + 1u;
            // The aux record sits at symtab index auxIdx; [ao, ao+18) is in
            // bounds (auxIdx < numSymbols, and the symbol table [symTabPtr,
            // +18*numSymbols) was proven file-backed in (1)).
            std::size_t const ao =
                static_cast<std::size_t>(symTabPtr) + auxIdx * kSymbolSz;
            selection = bytes[ao + kAuxSectionDefSelectionOff];
            break;
        }
        if (!selection.has_value()) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: section '" + sec.name + "' is flagged "
                "IMAGE_SCN_LNK_COMDAT but carries no section-definition auxiliary "
                "record (aux format 5) to read its COMDAT Selection from -- "
                "refusing to default a selection (a wrong default would silently "
                "mis-dedup). D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION.");
        }
        switch (*selection) {
            case kComdatSelNoDuplicates:
                // NODUPLICATES: a genuine duplicate IS an error. Keep the
                // symbol STRONG (Global) so the existing all-strong merge fires
                // K_SymbolRedefinedAcrossUnits on a duplicate -- which IS the
                // NODUPLICATES contract.
                comdatBindingBySection.emplace(
                    ordinal,
                    ComdatPolicy{SymbolBinding::Global, DuplicateMatch::Any});
                break;
            case kComdatSelAny:
                // ANY: duplicates are legal and the copies need not agree about
                // anything at all -- "any section that defines the same COMDAT
                // symbol can be linked; the rest are removed". Lift to WEAK so
                // the existing all-weak merge dedup keeps one body + drops the
                // shadow (ZERO merge change), and promise NOTHING.
                //
                // ★ THIS ARM IS CORRECT AS IT STANDS AND THE FIX MUST NOT
                // WIDEN INTO IT. Attaching a size or byte comparison here would
                // start REFUSING the one selection the format says may differ
                // freely -- which is the encoding DSS's OWN writer emits for
                // every weak definition (D-LK-OBJECT-WEAK-DEF-RELOCATABLE), so
                // it would refuse DSS's own output.
                comdatBindingBySection.emplace(
                    ordinal,
                    ComdatPolicy{SymbolBinding::Weak, DuplicateMatch::Any});
                break;
            case kComdatSelSameSize:
                // SAME_SIZE: duplicates are legal ONLY IF every definition has
                // the same size; otherwise the format requires a multiply-
                // defined-symbol error. Same WEAK lift, plus the duty.
                comdatBindingBySection.emplace(
                    ordinal,
                    ComdatPolicy{SymbolBinding::Weak,
                                 DuplicateMatch::SameSize});
                break;
            case kComdatSelExactMatch:
                // EXACT_MATCH: duplicates are legal ONLY IF the definitions
                // match exactly -- the strictest of the three, and the reason
                // the duty is an ordered scale rather than a flag.
                comdatBindingBySection.emplace(
                    ordinal, ComdatPolicy{SymbolBinding::Weak,
                                          DuplicateMatch::ExactContent});
                break;
            default:
                // LARGEST(6) / ASSOCIATIVE(5) / 0 / unknown -> FAIL LOUD.
                // Lifting LARGEST to Weak would be a SILENT WRONG-SIZE
                // MISCOMPILE: cuId is minted in PULL order (compile_pipeline.cpp
                // readArchiveMemberModule), NOT size order, so weak lowest-key
                // -wins can keep the SMALLER copy. ASSOCIATIVE ties a section's
                // liveness to another section, which the symbol-atomic model
                // does not represent. Both are C++ selectany/RTTI/vtable
                // constructs essentially absent from C/SQLite -- fail-loud is
                // the correct best-long-term stance for this target (the
                // size-aware successor is D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION).
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: section '" + sec.name + "' has "
                    "COMDAT Selection " + std::to_string(*selection)
                    + " (LARGEST / ASSOCIATIVE / unknown), which the format-blind "
                    "all-weak merge cannot honor without size-aware selection: "
                    "cuId is minted in PULL order, not size order, so a weak lift "
                    "could keep the smaller copy (a silent wrong-size "
                    "miscompile). D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION.");
        }
    }

    // -- (6) Reconstruct externs, then stage defined symbols per section --
    AssembledModule mod;
    mod.cuId = cuId;

    // symtab index -> the extern's position in mod.externImports (for the
    // isData inference in step 7).
    std::unordered_map<std::uint32_t, std::size_t> externBySym;
    // Defined symbols grouped by 1-based section ordinal -- atom boundaries,
    // sliced by sorted Value in the per-section pass below. The boundary set is
    // EXTERNAL symbols, class-STATIC symbols that declare DTYPE_FUNCTION
    // (file-local functions), and class-STATIC symbols in a NON-CODE section
    // (file-local data objects). What is left -- a block label, a section
    // identity -- is NOT a boundary here (see the loop); the geometry fallback
    // in (6.4) gets the last word on it.
    std::unordered_map<std::uint16_t, std::vector<DefSym>> defsBySection;
    // The DEMOTED half of that split, staged for the shared header -- see
    // `link/format/object_atom_coverage.hpp`. `bodilessSymIdx` is the parallel
    // symbol-table index, kept HERE rather than in the shared struct because it
    // is COFF's own coordinate: the shared staging is deliberately nothing but
    // `(sectionKey, byteOffset)`, and widening it with one reader's identity
    // would be the first format-specific field in a format-neutral type.
    std::vector<link::format::BodilessDefinedSymbol> bodilessDefined;
    std::vector<std::uint32_t>                       bodilessSymIdx;

    for (std::uint32_t i = 0; i < numSymbols; ++i) {
        if (auxSlot[i]) continue;  // an auxiliary record, not a symbol
        Sym const& s = syms[i];
        // GATE: the storage class resolves to a ROLE or the object is refused.
        // The old dispatch tested `== kSymClassExternal` / `== kSymClassStatic`
        // and let every other value fall through to the block-label arm, which
        // is how class 105 became a strong extern.
        std::optional<CoffSymbolRole> const roleOpt =
            roleForStorageClass(s.storage);
        if (!roleOpt.has_value()) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: symbol '" + s.name
                + "' has storage class " + std::to_string(s.storage)
                + ", which this reader does not model. Refusing to classify it "
                  "by its section alone -- the storage class is what says "
                  "whether a record names a body, a reference, or a "
                  "bookkeeping entry, and guessing it silently mis-slices the "
                  "section ("
                  // ANCHOR, ONE LINE, DO NOT WRAP. It WAS wrapped here across
                  // two string literals: the concatenated runtime message read
                  // correctly, so nothing failed, while the SOURCE stopped
                  // matching a grep for the id -- the invisible half of the
                  // wrap rule, met in the one place it is easiest to miss.
                  "D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM"
                  ").");
        }
        CoffSymbolRole const role = *roleOpt;

        if (role == CoffSymbolRole::WeakExternal) {
            // ── A WEAK EXTERNAL: this NAME defers to THAT NAME (5.5.3) ──────
            //
            // Decode the aux record, then route on WHAT THE DEFAULT SYMBOL IS.
            //
            // ★ A NAMELESS WEAK EXTERNAL IS REFUSED, NOT SKIPPED. This arm read
            // `if (s.name.empty()) continue;` -- "names nothing and defers
            // nothing" -- which is a SILENT DROP of a record that is still
            // counted in NumberOfSymbols and can still be the target of a
            // relocation BY INDEX. Skipped, that relocation retargets to a
            // SymbolId this reader never produced, and the object surfaces much
            // later as an unresolved symbol attributed to whoever merged it.
            // PE/COFF 5.5.3 gives sym1 a name precisely so another object can
            // satisfy it; a nameless one is a malformed record, and every other
            // structural violation of 5.5.3 in `weakExternDefault` fails loud
            // for the same reason -- refusing a half-decoded weak external beats
            // guessing what its binding was.
            if (s.name.empty()) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: symbol #" + std::to_string(i)
                    + " is a WEAK_EXTERNAL record (storage class 105) with an "
                      "EMPTY name. PE/COFF 5.5.3 makes the record a NAME that "
                      "defers to its Auxiliary Format 3 default; a nameless one "
                      "defers a name nothing can refer to, and dropping it "
                      "silently leaves any relocation naming this record BY "
                      "INDEX pointing at a symbol the reader never produced -- "
                      "which surfaces as an unresolved symbol against the wrong "
                      "object, long after the malformed record that caused it. "
                      "D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB.");
            }
            auto const tag = weakExternDefault(i);
            if (!tag.has_value()) return std::nullopt;   // already diagnosed
            Sym const& def = syms[*tag];
            if (def.sectNum >= 1u && def.sectNum <= numSections) {
                // The default is SECTION-BACKED: it is a body in THIS object,
                // and `s.name` is a second name for it. That covers BOTH shapes
                // mingw gcc emits this way -- a `__attribute__((weak))`
                // DEFINITION (whose body it renames to `.weak.<n>.<n>`) and a
                // `__attribute__((weak, alias("target")))`.
                //
                // ★ BINDING IS `Weak`, AND IT IS THE RECORD'S OWN STATEMENT,
                // not an inference. A weak external is by construction the name
                // that YIELDS: 5.5.3's "if sym1 is not present at link time,
                // sym2 is used to resolve references instead" is precisely the
                // rule `SymbolBinding::Weak` already means to the format-blind
                // merge (a strong definition elsewhere shadows it;
                // `resolveCrossCuDefs` breaks an all-weak tie lowest-key).
                // Reading it as Global -- which is what happened before this
                // arm existed, via the extern path -- makes a name that may
                // legally be overridden into one that collides.
                //
                // ★ WHY THIS NEEDS NO NEW MECHANISM: two boundaries at ONE
                // section offset are already ONE atom under SEVERAL NAMES
                // (`object_atom_coverage.hpp`,
                // D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS). Its
                // `outranksAsAtomIdentity` gives the atom to the STRONG name
                // and leaves the weak one its own row at the same address,
                // which is exactly the reconstruction this shape wants, and it
                // remaps every relocation naming either index to the owner.
                defsBySection[def.sectNum].push_back(
                    DefSym{i, def.value, s.name, SymbolBinding::Weak,
                           SymbolVisibility::Default});
                continue;
            }
            // The default is NOT section-backed -- an ABSOLUTE or UNDEF
            // fallback. ✔MEASURED: this is how clang encodes a weak UNDEFINED
            // REFERENCE (`extern int ea __attribute__((weak));` with no
            // definition) for BOTH `x86_64-w64-windows-gnu` and
            // `x86_64-pc-windows-msvc` -- the default is an ABSOLUTE symbol of
            // value 0, so an unresolved `ea` tests as 0, which is the GNU
            // semantics.
            //
            // ★★ THE REFUSAL THAT STOOD HERE IS GONE, AND WHAT IT WAS WAITING
            // FOR IS THE THING THAT LANDED. It read, correctly at the time:
            // "DSS's link-tier symbol model has no way to carry it:
            // `ExternImport` declares no binding on ANY format, so reading this
            // as a plain strong extern would silently drop the one property that
            // makes it weak." `ExternImport` now carries `binding`
            // (D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE), every writer
            // spells it, and an image link resolves an unbound weak reference to
            // a null import slot -- so the fact this arm could not represent is
            // representable, and refusing would now be refusing a shape DSS
            // itself emits. It is a WEAK IMPORT, and it is read as one.
            //
            // ⓘ The fallback symbol's own name is deliberately NOT carried
            // anywhere: PE/COFF 5.5.3 makes it the thing to use INSTEAD when
            // sym1 is absent, and when it is an absolute 0 the answer it
            // supplies is "nothing" -- which `SymbolBinding::Weak` already says
            // in the format-neutral vocabulary. Keeping the synthetic
            // `.weak.<n>.default` name would be re-exporting a producer's
            // private spelling as if it named something.
            {
                ExternImport weakExt;
                weakExt.symbol      = SymbolId{i};
                weakExt.mangledName = s.name;
                weakExt.isData      = !declaresFunction(s);
                weakExt.binding     = SymbolBinding::Weak;
                externBySym.emplace(i, mod.externImports.size());
                mod.externImports.push_back(std::move(weakExt));
                continue;
            }
        }

        bool const isExt = (role == CoffSymbolRole::External);
        SymbolBinding const binding =
            isExt ? SymbolBinding::Global : SymbolBinding::Local;

        if (s.sectNum == kSymUndefined) {
            // An UNDEFINED symbol -> an extern import.
            //
            // ★ A NAMELESS UNDEF RECORD IS REFUSED, NOT SKIPPED --
            // D-LK-COFF-NAMELESS-UNDEF-EXTERN-SILENTLY-DROPPED. This arm read
            // `if (s.name.empty()) continue;` under the comment "a nameless slot
            // carries no import identity", which is TRUE and is not a reason to
            // drop it: the record still occupies a `NumberOfSymbols` slot, so a
            // relocation can name it BY INDEX, and every gate that would catch
            // such a relocation lets it through -- the bound check passes
            // (the index is real), the aux-slot check passes (it is not an aux),
            // and `rel.target = SymbolId{ownerOf(symIdx)}` then names a SymbolId
            // this reader never produced. ✔MEASURED by reading the relocation
            // loop's own conclusion at (6.44): "an id that owns no body is
            // `K_SymbolUndefined` at the linker's compound index". So the drop
            // does not vanish; it re-emerges at MERGE time as an unresolved
            // symbol with NO NAME TO PRINT, attributed to whoever merged the
            // object rather than to the malformed record that caused it.
            //
            // ⚠ THE DECISION, AND IT IS THE ONE THIS ARM'S TWIN ALREADY TOOK.
            // PE/COFF 5.4.2 makes an UNDEF record with Value 0 "a reference to
            // an external symbol defined elsewhere" -- a reference resolved BY
            // NAME, so a nameless one refers to nothing any object could
            // satisfy. It is a malformed record, not a shape with a meaning DSS
            // is failing to model. The identical skip in the WEAK_EXTERNAL arm
            // above became a refusal for exactly this hazard, and refusing here
            // too is what makes the reader's treatment of the two UNIFORM
            // instead of accidental. ✔MEASURED before changing it: the
            // `CoffForeignObjectNative` probes over real cl.exe / clang-cl
            // objects (including a `/Gy` object and a multi-member `.lib`) stay
            // green, i.e. no real producer emits this shape -- so the blast
            // radius on objects DSS did not write is measured, not assumed.
            if (s.name.empty()) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: symbol #" + std::to_string(i)
                    + " has SectionNumber UNDEF and an EMPTY name. PE/COFF "
                      "5.4.2 makes an UNDEF record a reference resolved BY "
                      "NAME, so a nameless one names nothing any object can "
                      "satisfy -- and dropping it silently leaves any "
                      "relocation naming this record BY INDEX pointing at a "
                      "symbol the reader never produced, which surfaces as an "
                      "unresolved symbol against the wrong object long after "
                      "the malformed record that caused it. "
                      "D-LK-COFF-NAMELESS-UNDEF-EXTERN-SILENTLY-DROPPED.");
            }
            // ⚠ EXCEPT WHEN IT IS A COMMON SYMBOL, WHICH IS A DEFINITION.
            // PE/COFF 5.4.2: an EXTERNAL record with SectionNumber UNDEF(0) and
            // a NON-ZERO Value is a COMMON symbol, and the Value is its SIZE in
            // bytes -- the ELF SHN_COMMON analog. Reading one as an import is a
            // SILENT WRONG ANSWER in the worst direction: the object DEFINES
            // storage the reader would then demand somebody else provide, and
            // on a relocatable re-emission the definition simply vanishes.
            // ✔MEASURED, mingw gcc 13.2.0 `-fcommon`: `int commonvar;` emits
            // `(sec 0)(ty 0)(scl 2)` with Value 4. gcc has defaulted to
            // `-fno-common` since GCC 10 and cl.exe never emits it for C, which
            // is why no shipped path has produced one -- but "no producer we
            // have run" is not "unreachable", and allocation across CUs
            // (pick-max-size into `.bss`) is a merge concern this reader must
            // not fabricate. Fail loud instead of misreading it.
            // EXTERNAL specifically -- 5.4.2 scopes the common form to that
            // class, and reading the rule off the section number alone would
            // claim a debug record's stray Value means a size.
            if (isExt && s.value != 0u) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: symbol '" + s.name
                    + "' is a COMMON symbol (SectionNumber UNDEF with non-zero "
                      "Value " + std::to_string(s.value)
                    + ", which PE/COFF 5.4.2 defines as the object's SIZE) -- a "
                      "tentative DEFINITION, not an import. DSS's link tier has "
                      "no common-block allocation pass, and reading it as an "
                      "extern import would silently discard a definition this "
                      "object makes. D-LK-COFF-READER-FOREIGN-OBJECT.");
            }
            ExternImport ext;
            ext.symbol      = SymbolId{i};
            ext.mangledName = s.name;
            // isData from the COFF derived-type hint: the canonical function
            // test is `(type & DTYPE_MASK) == DTYPE_FUNCTION` (bits 4..5 = the
            // derived type; DT_ARY=0x30 is DATA, not a function -- a plain
            // `& 0x20` would misread it). A DTYPE_FUNCTION extern -> isData
            // =false, else DATA. COFF carries this hint (UNLIKE Mach-O x86_64,
            // which cannot distinguish call from data by reloc formula), and
            // the c170 writer fold now EMITS it on function externs, so the
            // function/data class round-trips FAITHFULLY -- no silent default.
            // An extern that reaches the walker unresolved is rejected loud by
            // the linker's unbound-extern gate regardless.
            ext.isData      = !declaresFunction(s);
            externBySym.emplace(i, mod.externImports.size());
            mod.externImports.push_back(std::move(ext));
            continue;
        }
        if (s.sectNum == kSymAbsolute || s.sectNum == kSymDebug) {
            // ABSOLUTE (-1) / DEBUG (-2): not a section-backed body. Record a
            // ModuleSymbol so a reloc target still resolves by identity (the
            // ELF SHN_ABS / Mach-O N_ABS analog).
            if (!s.name.empty()) {
                mod.symbols.push_back(ModuleSymbol{SymbolId{i}, s.name, binding,
                                                   SymbolVisibility::Default});
            }
            continue;
        }
        // A DEFINED symbol: SectionNumber is a 1-based ordinal.
        if (s.sectNum < 1u || s.sectNum > numSections) {
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: defined symbol '" + s.name
                + "' names section ordinal " + std::to_string(s.sectNum)
                + " out of range [1, " + std::to_string(numSections)
                + "] -- corrupt SectionNumber.");
        }
        // Value is ALREADY section-relative (the COFF convention -- NO
        // subtraction, unlike Mach-O's flat n_value).
        if (isExt) {
            // Externally-visible defined symbol -> an atom boundary. GATE 3:
            // a COMDAT section lifts the binding per its Selection policy (Weak
            // for ANY/SAME_SIZE/EXACT_MATCH so the all-weak dedup folds
            // duplicates; Global for NODUPLICATES / a non-COMDAT section).
            // D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED: the duty rides
            // out with the binding, on the SAME lookup, so a COMDAT can never
            // arrive Weak with its promise lost on the way.
            SymbolBinding  extBinding = SymbolBinding::Global;
            DuplicateMatch extDuty    = DuplicateMatch::Any;
            if (auto it = comdatBindingBySection.find(s.sectNum);
                it != comdatBindingBySection.end()) {
                extBinding = it->second.binding;
                extDuty    = it->second.duty;
            }
            defsBySection[s.sectNum].push_back(
                DefSym{i, s.value, s.name, extBinding,
                       SymbolVisibility::Default,
                       /*moduleSymbolAlreadyPushed=*/false, extDuty});
        } else if (role == CoffSymbolRole::Static && declaresFunction(s)) {
            // A FILE-LOCAL (`static`) FUNCTION -- an atom BOUNDARY, exactly like
            // an external one. D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM:
            // the only thing internal linkage changes is WHO MAY
            // SEE the definition, never whether it has a body, so classifying it
            // by EXTERNAL-ness dropped whole functions on the archive-member
            // read-back path (loud as `K_SymbolUndefined` when called, and
            // SILENT -- bytes simply absent from the image -- when not).
            //
            // ★ The discriminator is on the wire and is producer-universal, not
            // a DSS convention: see `kSymDtypeFunction`. A section-DEFINITION
            // symbol and a block label are both class STATIC too, and both carry
            // derived type 0, so neither reaches this arm -- which is why the
            // aux-record recogniser is deliberately NOT consulted here (it
            // misfires on gcc's static functions; see
            // `isSectionDefinitionSymbol`).
            //
            // ★ BINDING IS `Local`, AND THE OTHER TWO CHOICES ARE BOTH WRONG --
            // verified against `linker.cpp` / `cross_cu_resolve.cpp`, not
            // assumed. `resolveCrossCuDefs` SKIPS `Local` outright, which is
            // precisely C's internal-linkage rule: this definition can never
            // satisfy another TU's extern. `Global` would enter the name table
            // as a STRONG def, so two members each holding a `sym_<n>` (DSS
            // renames every internal-linkage function that way) would collide
            // with `K_SymbolRedefinedAcrossUnits`. `Weak` is worse than either:
            // both would enter as weak, lowest-key would win, and
            // `isShadowedDuplicate` would DROP the loser's body -- one member's
            // private function silently replaced by another's. `Local` bodies
            // are never shadowed and `mergedIdFor` mints each a fresh id.
            //
            // ★ NO COMDAT LIFT, deliberately -- the `comdatBindingBySection`
            // consultation in the EXTERNAL arm is absent here rather than
            // forgotten. COMDAT Selection is a cross-object dedup policy keyed
            // BY NAME, and an internal-linkage symbol has no cross-object name
            // to dedup by. cl.exe `/Gy` does put a file-local function in its
            // own COMDAT `.text$mn` (✔MEASURED, selection 1 = NODUPLICATES),
            // so the lift is REACHABLE: honoring it would make this symbol
            // Global and re-create the collision above, and an ANY-selection
            // section would make it Weak and re-create the silent body drop.
            //
            // ★ NO SECTION-KIND GATE, deliberately, and for the same reason the
            // EXTERNAL arm has none: a symbol that declares itself a FUNCTION is
            // a body by its producer's own statement, so if its section resolves
            // to no modeled kind that is a SCHEMA GAP and the slicing loop below
            // must recover it loud. Gating on a resolved kind here would silently
            // drop exactly the bytes an external symbol's absence would refuse --
            // reinstating, for internal linkage only, the asymmetry this arm
            // exists to remove.
            defsBySection[s.sectNum].push_back(
                DefSym{i, s.value, s.name, SymbolBinding::Local,
                       SymbolVisibility::Default});
        } else if (role == CoffSymbolRole::Static
                   && sections[s.sectNum - 1u].kind.has_value()
                   && *sections[s.sectNum - 1u].kind != SectionKind::Text
                   && !isSectionDefinitionSymbol(i)) {
            // A FILE-LOCAL DATA OBJECT -- an atom BOUNDARY, exactly like an
            // external one, and the OTHER half of
            // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM
            // (D-LK-COFF-ARCHIVE-MEMBER-READER-LOSES-STATIC-RODATA-SYMBOLS is
            // the same loss scoped to `.rdata`). The derived-type field cannot
            // decide this one: COFF stamps type 0 on EVERY data symbol
            // regardless of linkage -- `pe.cpp` says so at its defined-DATA loop
            // ("COFF data symbols are `notype`; DTYPE_FUNCTION is
            // functions-only"), and cl.exe and gcc agree -- so a `static const`
            // array is byte-for-byte the shape of a block label on the wire.
            //
            // ★ THE SECOND DISCRIMINATOR IS THE SECTION'S OWN KIND, and it is
            // sound because a block label is a CODE address. It labels an
            // instruction boundary inside a function; there is no such thing as
            // an interior label in `.rdata`. ✔MEASURED, and the risk this had to
            // rule out was specific -- a foreign jump-table label landing in a
            // non-code section and being read as a datum. Over 6 cl.exe
            // 14.51.36231 arms (/Od, /O2, /Gy, /Zi combinations) and 7 mingw
            // gcc 13.2.0 arms (-O0, -O2, -g, -ffunction-sections,
            // -fdata-sections, -fno-pic) compiling a dense 12-case switch,
            // NEITHER producer does that: cl.exe emits the jump table as a
            // class-STATIC type-0 symbol (`$LN18`) INSIDE `.text$mn` at the end
            // of its own function, and the case targets as class LABEL, also in
            // `.text`; gcc puts the table in `.rdata` but attaches NO SYMBOL to
            // it at all, reaching it through the `.rdata` section symbol plus an
            // addend. Every class-STATIC type-0 symbol either producer put in a
            // non-code section was a whole data object (`table`, `msg`,
            // `counter`, `zeroed`) or unwind metadata in a section whose kind
            // does not resolve (`$unwind$`/`$pdata$`), which is excluded above.
            // DSS's own writer is narrower still: its block-symbol loop
            // hardcodes `kTextSectionNumber`.
            //
            // ★ THE RULE IS NOT NEW -- IT REMOVES AN EXCEPTION. This reader
            // ALREADY treats an external data symbol as a boundary that runs to
            // the next one, with exactly the same theoretical exposure (an
            // interior data alias at a non-zero offset would split the object).
            // What made internal linkage different was never evidence, only
            // EXTERNAL-ness; C's object model gives named objects disjoint
            // storage whichever linkage they have. Making the two paths agree is
            // the whole content of this anchor.
            //
            // ★ CLASS STATIC SPECIFICALLY, not "non-external". IMAGE_SYM_CLASS_
            // LABEL (6) means a label WITHIN A MODULE -- a code address by
            // definition, and the class cl.exe stamps on its `$LN` case targets.
            // Promoting one of those on the strength of its section would be
            // reading the section instead of the symbol.
            //
            // ★ BINDING IS `Local` and takes NO COMDAT lift, for the same two
            // reasons spelled out in the function arm above: `resolveCrossCuDefs`
            // skips Local (C internal linkage), and a Selection policy dedups BY
            // NAME across objects, which internal linkage has no part in.
            defsBySection[s.sectNum].push_back(
                DefSym{i, s.value, s.name, SymbolBinding::Local,
                       SymbolVisibility::Default});
        } else if (!s.name.empty()) {
            // A NON-EXTERNAL defined symbol that neither declares a function
            // type nor sits in a non-code section -- an interior `&&label` /
            // jump-table block label (DSS's own writer emits those class STATIC
            // with derived type 0, `pe.cpp`'s "Synthetic per-block symbols"
            // loop; cl.exe emits its `$LN` case targets as class LABEL and its
            // jump TABLE as class STATIC, all three inside `.text`), a section
            // identity, or a foreign `$unwind$`/`$pdata$` thunk symbol.
            // Recorded as a LOCAL ModuleSymbol and NOT an atom boundary for
            // now, so it never splits the function/data item that contains its
            // interior offset.
            mod.symbols.push_back(ModuleSymbol{SymbolId{i}, s.name,
                SymbolBinding::Local, SymbolVisibility::Default});
            // ...and STAGE it for the shared header
            // (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM).
            // The reasoning above is right for a label and STILL UNPROVEN for
            // anything else that lands here: COFF declares no size, so a
            // `.text` symbol with no function type is the same three numbers
            // whether it is a block label or a body written by an assembler
            // that stamped no type. GEOMETRY decides in (6.4) -- a label lies
            // INSIDE its enclosing function, so that atom covers its offset and
            // nothing happens; anything the atoms do NOT cover is promoted to a
            // body rather than left to vanish. Only a KIND-RESOLVED section is
            // staged (an unmodeled `.pdata`/`.debug$S` symbol reconstructs no
            // body BY DESIGN, and is not a dropped body), and a section-
            // DEFINITION symbol is a section IDENTITY, not a body, so it is
            // excluded.
            if (sections[s.sectNum - 1u].kind.has_value()
                && !isSectionDefinitionSymbol(i)) {
                // ★ ONE EXTENT IS DERIVABLE EVEN THOUGH COFF DECLARES NONE: a
                // symbol sitting exactly at SizeOfRawData has no room for a
                // byte, so it is a MARKER (an end-of-section label), not a body.
                // Staging it as size 0 says that in the shared vocabulary, which
                // stops the fallback minting a zero-length atom for it and stops
                // the post-condition then reporting that atom as not covering
                // its own start.
                std::optional<std::uint64_t> const derivedSize =
                    (s.value == sections[s.sectNum - 1u].rawSize)
                        ? std::optional<std::uint64_t>{0u}
                        : std::nullopt;
                bodilessDefined.push_back(link::format::BodilessDefinedSymbol{
                    /*sectionKey=*/s.sectNum, /*sectionOffset=*/s.value,
                    /*declaredSize=*/derivedSize, /*name=*/s.name,
                    /*sectionName=*/sections[s.sectNum - 1u].name});
                bodilessSymIdx.push_back(i);
            }
        }
    }

    // Per-section interval lists for relocation-site routing.
    std::unordered_map<std::uint16_t, std::vector<Interval>> funcIntervalsBySec;
    std::unordered_map<std::uint16_t, std::vector<Interval>> dataIntervalsBySec;

    auto pushModuleSym = [&](DefSym const& d) {
        // A geometry-promoted symbol already has its ModuleSymbol from the
        // classification loop; pushing a second one would duplicate the name in
        // `mod.symbols` (see `DefSym::moduleSymbolAlreadyPushed`).
        if (!d.name.empty() && !d.moduleSymbolAlreadyPushed) {
            mod.symbols.push_back(ModuleSymbol{SymbolId{d.symIdx}, d.name,
                                               d.binding, d.visibility,
                                               d.duplicateMatch});
        }
    };

    // Order each section's boundary set. Hoisted out of the slicing loop
    // because (6.4) below reads the SAME order to compute the extents it
    // reasons about, and two orderings written in two places is one rule that
    // can disagree with itself.
    //
    // ★ THE TIE-BREAK IS PART OF THE RULE, not tidiness. Equal-offset ALIASES
    // share a span, so their relative order does not change any atom's bytes --
    // but it does decide which of them `mod.functions` lists first, and this
    // runs TWICE (again after (6.4) appends). An offset-only comparator lets an
    // unstable sort permute an alias pair between the two passes, making the
    // reconstruction depend on the sort implementation. Breaking the tie on the
    // symbol-table index makes the order total, so both passes agree and
    // aliases come back in the order the object listed them.
    auto sortBoundaries = [&] {
        for (auto& [ordinal, defs] : defsBySection) {
            std::sort(defs.begin(), defs.end(),
                      [](DefSym const& a, DefSym const& b) {
                          if (a.secRelOff != b.secRelOff)
                              return a.secRelOff < b.secRelOff;
                          return a.symIdx < b.symIdx;
                      });
        }
    };
    sortBoundaries();

    // THE ATOM EXTENT RULE, stated once (THE key inversion -- IMAGE_SYMBOL has
    // no size field, like Mach-O's nlist_64, so an atom's END comes from the
    // NEXT boundary rather than from the symbol itself). The k-th boundary of a
    // SORTED `defs` ends at the next STRICTLY-GREATER offset -- skipping
    // equal-offset ALIASES so they share the span and both get identical bytes,
    // the ELF equal-start rule -- else at the section's SizeOfRawData.
    //
    // (6.4) and the slicing loop must agree on this exactly: (6.4) decides which
    // symbols to promote by asking which offsets these extents cover, and if it
    // computed a DIFFERENT extent than the slicer then a symbol could be
    // promoted on the strength of an atom that never materialises (or left alone
    // on the strength of one that does not reach it).
    auto atomEndFor = [](std::vector<DefSym> const& defs, std::size_t k,
                         std::uint64_t sectionSize) -> std::uint64_t {
        std::uint64_t const off = defs[k].secRelOff;
        for (std::size_t j = k + 1; j < defs.size(); ++j) {
            if (defs[j].secRelOff > off) return defs[j].secRelOff;
        }
        return sectionSize;
    };

    // -- (6.4) GEOMETRY FALLBACK: recover a body the wire could not name ---
    //
    // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM. What is
    // still staged as bodiless at this point is a `.text` symbol that declares
    // no function type -- a block label if the section's functions were
    // reconstructed around it, and otherwise a body written by a producer that
    // stamped no type on it. COFF offers nothing further to read, so the last
    // evidence is geometric, and the shared header owns both the inference and
    // the argument for it (`uncoveredDefinedSymbolsThatStartAnAtom`): a symbol
    // no reconstructed atom covers cannot be interior to one, and promoting it
    // preserves its bytes where demoting it drops them.
    //
    // ⚠ THE EXTENTS FED IN ARE PROSPECTIVE, computed from the boundary set
    // decided above with `atomEndFor`. They must be the ones the slicer will
    // actually produce, which is why the extent rule was hoisted rather than
    // re-derived here. Promoting BEFORE slicing (rather than slicing, promoting
    // and re-slicing) means every body is cut exactly once and the bounds checks
    // below run over the final boundary set.
    {
        std::vector<link::format::ReconstructedAtomExtent> prospective;
        for (auto const& [ordinal, defs] : defsBySection) {
            std::uint64_t const secSize = sections[ordinal - 1u].rawSize;
            for (std::size_t k = 0; k < defs.size(); ++k) {
                std::uint64_t const off = defs[k].secRelOff;
                std::uint64_t const end = atomEndFor(defs, k, secSize);
                // A corrupt offset past the section end is the SLICER's refusal
                // to make (it names the symbol and the size); skip it here
                // rather than fabricate a reversed extent.
                if (off > secSize || end < off) continue;
                prospective.push_back(link::format::ReconstructedAtomExtent{
                    ordinal, off, end - off});
            }
        }
        for (std::size_t idx :
             link::format::uncoveredDefinedSymbolsThatStartAnAtom(bodilessDefined,
                                                                  prospective)) {
            auto const& c = bodilessDefined[idx];
            // `Local` and no COMDAT lift, for the reasons the two classified
            // arms above spell out. A promoted symbol is a body this reader
            // could not NAME a reason for, never a reason to change linkage.
            defsBySection[static_cast<std::uint16_t>(c.sectionKey)].push_back(
                DefSym{bodilessSymIdx[idx], c.sectionOffset, c.name,
                       SymbolBinding::Local, SymbolVisibility::Default,
                       /*moduleSymbolAlreadyPushed=*/true});
        }
        sortBoundaries();
    }

    // -- (6.44) EQUAL-OFFSET ALIAS IDENTITY: one atom, several names ------
    //
    // D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS. The boundary set is
    // FINAL here -- every COFF discriminator plus whatever (6.4) recovered --
    // and this is the last moment before bytes are cut. Two boundaries at one
    // offset are two NAMES for one body; minting an atom for each produced
    // byte-identical twins, and `findInterval` hands a relocation in the span
    // they share to exactly ONE of them, leaving the other's copy un-patched.
    // The shared header owns the rule, the ranking, and the argument for both
    // (`resolveEqualOffsetAtomAliases`).
    //
    // COFF passes `declaredExtent = nullopt` because `IMAGE_SYMBOL` has no size
    // field -- this reader derives an atom's end from the next boundary
    // (`atomEndFor`), so equal-offset candidates get equal extents by
    // construction and the conflicting-extent refusal cannot fire here.
    std::unordered_map<std::uint32_t, std::uint32_t> atomOwnerBySym;
    {
        std::vector<link::format::AtomStartCandidate> candidates;
        for (auto const& [ordinal, defs] : defsBySection) {
            Section const& sec = sections[ordinal - 1u];
            for (auto const& d : defs) {
                candidates.push_back(link::format::AtomStartCandidate{
                    /*sectionKey=*/ordinal, /*offset=*/d.secRelOff,
                    /*declaredExtent=*/std::nullopt, /*symbolId=*/d.symIdx,
                    /*binding=*/d.binding, /*visibility=*/d.visibility,
                    /*name=*/d.name, /*sectionName=*/sec.name});
            }
        }
        std::vector<std::uint32_t> owner;
        if (!link::format::resolveEqualOffsetAtomAliases(
                candidates, owner, "pe::readRelocatableObject", reporter)) {
            return std::nullopt;   // the resolver reported, naming both symbols
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (owner[i] != candidates[i].symbolId) {
                atomOwnerBySym.emplace(candidates[i].symbolId, owner[i]);
            }
        }
    }
    // The atom identity a symbol resolves to: itself unless it aliases another.
    // Consulted at BOTH sites that need it -- the slicing loop (does this
    // boundary mint a body?) and the relocation pass (which atom does this
    // target name?) -- because a collapse without the target remap turns a
    // silent miscompile into a spurious `K_SymbolUndefined`.
    auto ownerOf = [&](std::uint32_t symIdx) -> std::uint32_t {
        auto const it = atomOwnerBySym.find(symIdx);
        return it == atomOwnerBySym.end() ? symIdx : it->second;
    };
    // Alias rows are appended AFTER the slicing loop, never during it: every
    // id -> row lookup over `AssembledModule::symbols` keeps the FIRST row for
    // an id, so the canonical name must be recorded before any alias of it.
    std::vector<ModuleSymbol> aliasRows;

    // Slice each section's atoms by SORTED Value. What reached `defsBySection`
    // is every defined symbol that STARTS A BODY, of either linkage: EXTERNAL
    // symbols, class-STATIC symbols declaring DTYPE_FUNCTION, class-STATIC
    // symbols in a non-code section, and whatever (6.4) recovered. A STATIC
    // symbol that survived all four became an interior-label ModuleSymbol and is
    // never an atom. Slicing is BINDING-BLIND from here down: a local body's
    // bytes, bounds checks and relocation routing are the external path's,
    // unchanged.
    for (auto& [ordinal, defs] : defsBySection) {
        Section const& sec = sections[ordinal - 1u];
        std::optional<SectionKind> const rk = sec.kind;
        std::optional<DataSectionKind> const dk =
            rk.has_value() ? dataSectionKindOf(*rk) : std::nullopt;
        bool const isText = rk.has_value() && *rk == SectionKind::Text;

        for (std::size_t k = 0; k < defs.size(); ++k) {
            std::uint64_t const off = defs[k].secRelOff;
            std::uint64_t const end = atomEndFor(defs, k, sec.rawSize);
            if (off > sec.rawSize || end < off) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: defined symbol '" + defs[k].name
                    + "' offset " + std::to_string(off) + " exceeds its section '"
                    + sec.name + "' size " + std::to_string(sec.rawSize) + ".");
            }
            std::uint64_t const len = end - off;

            // (6.44) THE ALIAS ARM: this boundary names a body another boundary
            // at the same offset already owns, so it mints NO atom -- it keeps
            // its NAME and takes the owner's identity. Placed after the offset
            // refusal so a corrupt offset is still named by the symbol that
            // carries it.
            if (std::uint32_t const owner = ownerOf(defs[k].symIdx);
                owner != defs[k].symIdx) {
                if (defs[k].moduleSymbolAlreadyPushed) {
                    // A (6.4) geometry promotion that turned out to alias. It
                    // cannot happen today -- the fallback promotes only symbols
                    // NO reconstructed atom covers, and an atom starting at this
                    // very offset covers it -- but if it ever does, the row it
                    // already pushed carries the WRONG id and retargeting it is
                    // the correct repair, not skipping it.
                    for (auto& ms : mod.symbols) {
                        if (ms.symbol == SymbolId{defs[k].symIdx}) {
                            ms.symbol = SymbolId{owner};
                            break;
                        }
                    }
                } else if (!defs[k].name.empty()) {
                    aliasRows.push_back(ModuleSymbol{SymbolId{owner}, defs[k].name,
                                                     defs[k].binding,
                                                     defs[k].visibility});
                }
                continue;
            }

            if (isText) {
                // A function body -- slice [off, end) out of the file-backed
                // `.text`. A Text section must never be zero-fill.
                if (sec.zeroFill
                    || rangeExceedsBuffer(off, len, sec.rawSize)
                    || rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "pe::readRelocatableObject: function symbol '"
                        + defs[k].name + "' range [+" + std::to_string(off)
                        + ", +" + std::to_string(len) + ") is not a file-backed "
                        "slice of section '" + sec.name + "'.");
                }
                std::size_t const bodyOff = static_cast<std::size_t>(sec.rawPtr + off);
                AssembledFunction fn;
                fn.symbol = SymbolId{defs[k].symIdx};
                fn.bytes.assign(bytes.begin() + bodyOff,
                                bytes.begin() + bodyOff + static_cast<std::size_t>(len));
                funcIntervalsBySec[ordinal].push_back(
                    Interval{off, len, mod.functions.size()});
                mod.functions.push_back(std::move(fn));
                pushModuleSym(defs[k]);
                continue;
            }
            if (!dk.has_value()) {
                // A defined body in a section that resolves to no known code/
                // data kind must NEVER be silently dropped to a bodiless
                // ModuleSymbol (the "never a silent partial reconstruction"
                // contract) -- fail loud so the shape is recovered (a new
                // schema row) rather than mis-linked to an empty def.
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: defined symbol '" + defs[k].name
                    + "' lives in section '" + sec.name + "' which resolves to "
                    "no known code/data section kind -- refusing to silently "
                    "drop a body (add the section's kind to the format schema).");
            }
            // A data object -> an AssembledData item. File-backed sections
            // slice their bytes; a zero-fill (bss) section reserves the size
            // with empty bytes (the reservedSize invariant).
            AssembledData di;
            di.symbol    = SymbolId{defs[k].symIdx};
            di.section   = *dk;
            di.alignment = alignFromCharacteristics(sec.chars);  // section-granular
            if (isZeroFill(*dk)) {
                di.reservedSize = len;
            } else {
                if (sec.zeroFill
                    || rangeExceedsBuffer(off, len, sec.rawSize)
                    || rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "pe::readRelocatableObject: data symbol '" + defs[k].name
                        + "' range [+" + std::to_string(off) + ", +"
                        + std::to_string(len) + ") is not a file-backed slice of "
                        "section '" + sec.name + "'.");
                }
                std::size_t const bodyOff = static_cast<std::size_t>(sec.rawPtr + off);
                di.bytes.assign(bytes.begin() + bodyOff,
                                bytes.begin() + bodyOff + static_cast<std::size_t>(len));
            }
            dataIntervalsBySec[ordinal].push_back(
                Interval{off, len, mod.dataItems.size()});
            mod.dataItems.push_back(std::move(di));
            pushModuleSym(defs[k]);
        }
    }

    // Every canonical row is now recorded, so the aliases can follow: several
    // names, one SymbolId, the owning name first (see (6.44)).
    for (auto& ms : aliasRows) mod.symbols.push_back(std::move(ms));

    // -- (6.45) SYNTHETIC GAP ATOMS: reconstruct ANONYMOUS data-section bytes
    //
    // D-LK-COFF-READER-ANONYMOUS-GAP-ATOMS. Bytes in a DATA section that no
    // symbol names become one synthetic anonymous `AssembledData` per maximal
    // uncovered range, so they reach the image instead of vanishing. This is the
    // COFF arm of the pass ELF has had since the c167 fold, and it runs off the
    // SAME shared rule (`unownedByteRangesOfSection`) -- the only thing that
    // differs is the section vocabulary each reader already speaks.
    //
    // ★★★ WHY THIS HAD TO LAND IN THE SAME CYCLE AS THE CLASSIFICATION ABOVE.
    // ✔MEASURED, mingw gcc 13.2.0 `-O2`: a dense switch puts its jump table in
    // `.rdata` with NO SYMBOL ON IT, reached from `.text` through a REL32
    // against the `.rdata` SECTION symbol plus an addend. Before this cycle such
    // an object was refused -- but for the WRONG REASON: the atom-coverage guard
    // fired on the file-local `msg`/`table` symbols, which are not the bytes at
    // risk. Promoting those symbols (clause (7)(c)) made them covered, the guard
    // fell correctly silent, and a reloc-free `.rdata` would then have read
    // GREEN with the anonymous third of the section dropped. TRADING A LOUD
    // REFUSAL FOR A SILENT BYTE LOSS IS EXACTLY WHAT THIS PROJECT'S BAR
    // FORBIDS, even when the underlying row is filed elsewhere -- so the row
    // gets closed here rather than the regression getting shipped behind it.
    //
    // ★ THE THREE RESTRICTIONS ARE THE SHARED HEADER'S, and COFF satisfies one
    // of them for free. Only DATA (a `.text` gap is inter-function alignment
    // padding), only FILE-BACKED (a zero-fill `.bss` gap has no bytes), and only
    // a DECLARED kind -- and COFF resolves kinds from the section NAME alone
    // (the base name before `$`), with no SHF_ALLOC-style flags fallback, so
    // `.xdata` / `.pdata` / `.debug$S` / `.drectve` / `.chks64` stay `nullopt`
    // and are skipped without a special case. The `.eh_frame` hazard the ELF
    // arm has to exclude by hand cannot arise here.
    //
    // ⓘ INERT ON DSS'S OWN OUTPUT: `pe.cpp` emits a symbol for every data item,
    // so its sections are fully covered and no gap exists to mint.
    {
        std::uint32_t nextSyntheticId = numSymbols;
        for (std::size_t si = 0; si < sections.size(); ++si) {
            Section const& sec = sections[si];
            if (sec.zeroFill || sec.rawSize == 0u) continue;
            std::optional<DataSectionKind> const dk =
                sec.kind.has_value() ? dataSectionKindOf(*sec.kind) : std::nullopt;
            if (!dk.has_value() || isZeroFill(*dk)) continue;
            if (rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) continue;

            std::uint16_t const ordinal = static_cast<std::uint16_t>(si + 1u);
            // A fresh COPY of what is already reconstructed here: `emitGap`
            // appends to the live map vector, and the shared rule must never see
            // the gaps it is in the middle of producing.
            std::vector<link::format::ReconstructedAtomExtent> covered;
            if (auto it = dataIntervalsBySec.find(ordinal);
                it != dataIntervalsBySec.end()) {
                for (auto const& iv : it->second) {
                    covered.push_back(link::format::ReconstructedAtomExtent{
                        ordinal, iv.start, iv.len});
                }
            }
            for (auto const& g : link::format::unownedByteRangesOfSection(
                     ordinal, sec.rawSize, covered)) {
                AssembledData di;
                // A SymbolId past the symbol table cannot collide with any real
                // symbol's, and NO ModuleSymbol is recorded -- the atom stays
                // module-private and is never folded cross-CU by name, which is
                // right for bytes that have no name.
                di.symbol    = SymbolId{nextSyntheticId++};
                di.section   = *dk;
                // The gap's bytes belong to the SAME section, so they carry the
                // same declared alignment as the named atoms around them --
                // exactly as the ELF gap arm does.
                di.alignment = alignFromCharacteristics(sec.chars);
                std::size_t const b0 = static_cast<std::size_t>(sec.rawPtr + g.start);
                di.bytes.assign(bytes.begin() + b0,
                                bytes.begin() + b0 + static_cast<std::size_t>(g.len));
                dataIntervalsBySec[ordinal].push_back(
                    Interval{g.start, g.len, mod.dataItems.size()});
                mod.dataItems.push_back(std::move(di));
            }
        }
    }

    // -- (6.5) POST-CONDITION: no defined symbol's body was dropped -------
    //
    // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM. Asked
    // over the ACTUAL sliced intervals -- not the prospective ones (6.4)
    // reasoned about -- AFTER the gap pass (a symbol covered only by a synthetic
    // gap atom is still covered; asking before it would judge the reconstruction
    // half-finished) and BEFORE the relocation pass, so a failure names the
    // SYMBOL rather than surfacing later as a reloc that routes to nothing.
    //
    // ★ THIS IS NO LONGER A DETECTOR ON THIS READER, and saying so is the point.
    // (6.4) promotes every staged symbol the atoms do not cover, so no OBJECT
    // can reach this refusal any more. What the check still asserts is a
    // property of the READER -- that (6.4)'s promotions actually MATERIALISED as
    // atoms at the coordinates they were filed under. A promotion filed under
    // the wrong section ordinal, or staged at a pre-conversion offset, produces
    // atoms that exist and do not cover the symbol they were minted for, and
    // nothing else downstream would notice: the bytes would land in the wrong
    // atom, not in none.
    //
    // ⚠ WHICH IS WHY EVERY CANDIDATE IS PASSED, PROMOTED ONES INCLUDED. Passing
    // only the ones (6.4) declined would be a tautology -- it declines exactly
    // the covered ones -- and would leave the refusal unreachable by ANY defect
    // rather than merely by any object. The shared header's "WHAT THE
    // POST-CONDITION STILL ASSERTS" paragraph carries the argument, and the
    // mutant that reds it is in `tests/link/test_object_atom_coverage.cpp`.
    {
        std::vector<link::format::ReconstructedAtomExtent> extents;
        for (auto const* bySec : {&funcIntervalsBySec, &dataIntervalsBySec}) {
            for (auto const& [ordinal, ivs] : *bySec) {
                for (auto const& iv : ivs) {
                    extents.push_back(link::format::ReconstructedAtomExtent{
                        ordinal, iv.start, iv.len});
                }
            }
        }
        if (!link::format::everyDefinedSymbolIsCoveredByAnAtom(
                bodilessDefined, extents, "pe::readRelocatableObject", reporter,
                // COFF's own sentence. Every wire discriminator and the
                // geometric one have already been applied, so reaching here
                // means the reader contradicted itself -- point the triager at
                // the reader, not at the object's producer.
                "every COFF discriminator has already been applied to it "
                "(IMAGE_SYM_DTYPE_FUNCTION, the section's kind, and finally "
                "atom-coverage geometry), so this is the READER disagreeing "
                "with itself about where it put the atom, not an object it "
                "cannot classify")) {
            return std::nullopt;
        }
    }

    // -- (7) Reconstruct relocations from every section's reloc table ----
    //
    // Each IMAGE_SECTION_HEADER names its OWN IMAGE_RELOCATION table
    // (PointerToRelocations / NumberOfRelocations). VirtualAddress is
    // section-relative (the writer emits `fnStart+rel.offset` for `.text`,
    // `itemOff+rel.offset` for data). We route it to the reconstructed atom
    // whose byte range contains it (offset made item-relative). A section with
    // relocs but NO reconstructed atom fails loud (mirror the c168 fold --
    // never silently drop a section's relocations).
    auto findInterval = [](std::vector<Interval> const& ivs, std::uint64_t off)
        -> Interval const* {
        for (auto const& iv : ivs) {
            if (off >= iv.start && off < iv.start + iv.len) return &iv;
        }
        return nullptr;
    };

    for (std::size_t si = 0; si < sections.size(); ++si) {
        Section const& sec = sections[si];
        if (sec.relocCount == 0u) continue;
        std::uint16_t const ordinal = static_cast<std::uint16_t>(si + 1u);
        auto const fIt = funcIntervalsBySec.find(ordinal);
        auto const dIt = dataIntervalsBySec.find(ordinal);
        bool const patchesText = (fIt != funcIntervalsBySec.end() && !fIt->second.empty());
        bool const patchesData = (dIt != dataIntervalsBySec.end() && !dIt->second.empty());
        if (!patchesText && !patchesData) {
            // GATE 2 (D-LK-COFF-READER-FOREIGN-OBJECT): a reloc-bearing section
            // that reconstructed NO atom. The skip is gated on KIND-UNRESOLVED,
            // never atom-absence.
            if (!sec.kind.has_value()) {
                // An UNMODELED metadata section whose BASE name is absent from
                // the schema (`.pdata` / `.xdata` / `.debug$S` / `.debug$T` /
                // `.drectve` / `.chks64`) -- kind stayed nullopt. These carry
                // their OWN relocations (`.pdata`/`.xdata` are 0x40000040, NOT
                // discardable -- so the DISCARDABLE bit alone cannot gate this)
                // but hold no reconstructable code/data body the AssembledModule
                // models. SKIP the section AND its reloc table: DSS has no
                // representation for CodeView / SEH-unwind / linker-directive
                // metadata. Dropping `.pdata`/`.xdata` drops unwind for a
                // foreign function (fine for a leaf/exit-42 link; a general
                // limitation -- D-LK-COFF-FOREIGN-UNWIND-DROP).
                continue;
            }
            // A KIND-RESOLVED section that reconstructed NO atom. Since (6.45)
            // this is a TEXT section specifically: an anonymous DATA section is
            // gap-filled there and always has an atom, so it cannot arrive here.
            // Text is excluded from gap-filling DELIBERATELY -- a `.text` gap is
            // inter-function ALIGNMENT PADDING, and fabricating a code atom out
            // of padding would give a corrupt code reference somewhere to land
            // instead of failing loud. FAIL LOUD rather than skip: skipping a
            // resolved-kind section's relocs would be a SILENT DROP (the
            // never-silently-drop contract).
            return fail(DiagnosticCode::F_CorruptedBinary,
                "pe::readRelocatableObject: section '" + sec.name + "' carries "
                + std::to_string(sec.relocCount) + " relocation(s) but "
                "reconstructed no atom to attach them to (its kind resolved, so "
                "it is real code/data, not skippable metadata) -- refusing to "
                "silently drop a section's relocations. An anonymous DATA "
                "section is recovered by the gap-atom pass; a code section is "
                "not, because a gap in executable bytes is alignment padding "
                "rather than a body.");
        }
        std::vector<Interval> const& ivs = patchesText ? fIt->second : dIt->second;

        for (std::uint32_t e = 0; e < sec.relocCount; ++e) {
            std::size_t const ro = static_cast<std::size_t>(sec.relocPtr)
                                 + static_cast<std::size_t>(e) * kRelocSz;
            std::uint64_t const va       = rdU32(bytes, ro + kRelVirtAddrOff);
            std::uint32_t const symIdx   = rdU32(bytes, ro + kRelSymIdxOff);
            std::uint32_t const nativeId = rdU16(bytes, ro + kRelTypeOff);

            if (symIdx >= numSymbols) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: relocation in section '"
                    + sec.name + "' names symbol #" + std::to_string(symIdx)
                    + " past the symbol table (" + std::to_string(numSymbols)
                    + ").");
            }
            if (auxSlot[symIdx]) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: relocation in section '"
                    + sec.name + "' names symbol #" + std::to_string(symIdx)
                    + " which is an AUXILIARY record slot, not a symbol.");
            }
            auto const kindIt = nativeToKind.find(nativeId);
            if (kindIt == nativeToKind.end()) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: relocation Type "
                    + std::to_string(nativeId) + " in section '" + sec.name
                    + "' is not declared by PE format '"
                    + std::string{objectFormatSchema.name()}
                    + "' -- cannot map it back to a universal RelocationKind.");
            }
            RelocationKind const kind = kindIt->second;
            auto const* tri = targetSchema.relocationInfo(kind);
            if (tri == nullptr) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: RelocationKind "
                    + std::to_string(kind.v) + " has no TargetRelocationInfo on '"
                    + std::string{targetSchema.name()}
                    + "' -- cannot resolve its addend width / bias.");
            }

            Interval const* iv = findInterval(ivs, va);
            if (iv == nullptr) {
                return fail(DiagnosticCode::F_CorruptedBinary,
                    "pe::readRelocatableObject: relocation at section offset "
                    + std::to_string(va) + " in '" + sec.name
                    + "' lies in no reconstructed "
                    + std::string{patchesText ? "function" : "data item"}
                    + " -- refusing to silently drop it.");
            }

            // Addend. COFF has no addend column:
            //   * a DATA-section reloc's addend lives IN the patched slot
            //     bytes (widthBytes LE at VirtualAddress -- the writer's
            //     in-place convention); the target-schema addendBias is
            //     un-baked so a re-emission re-adds it once (0 for the
            //     non-pcrel absolute kinds a data slot uses).
            //   * a `.text` reloc carries addend 0 (the writer rejects a
            //     non-zero `.text` addend; link.exe applies the rel32 RIP bias
            //     intrinsically).
            std::int64_t addend = 0;
            if (patchesData) {
                std::uint8_t const w = tri->widthBytes;
                if (w == 0u
                    || rangeExceedsBuffer(va, w, sec.rawSize)
                    || rangeExceedsBuffer(sec.rawPtr, sec.rawSize, bytes.size())) {
                    return fail(DiagnosticCode::F_CorruptedBinary,
                        "pe::readRelocatableObject: data relocation at section "
                        "offset " + std::to_string(va) + " in '" + sec.name
                        + "' has a " + std::to_string(w) + "-byte slot that "
                        "runs past the section -- cannot read the in-place "
                        "addend.");
                }
                std::uint64_t raw = 0;
                std::size_t const slot = static_cast<std::size_t>(sec.rawPtr + va);
                for (std::uint8_t b = 0; b < w; ++b) {
                    raw |= static_cast<std::uint64_t>(bytes[slot + b]) << (8u * b);
                }
                addend = signExtendLE(raw, w)
                       - static_cast<std::int64_t>(tri->addendBias);
            }

            Relocation rel;
            rel.offset = static_cast<std::uint32_t>(va - iv->start);
            // (6.44): a target naming an ALIAS binds to the atom that owns the
            // body, because only the owner is a declared definition -- an id
            // that owns no body is `K_SymbolUndefined` at the linker's compound
            // index. The addend needs no adjustment: an alias shares its owner's
            // offset exactly, so the same S makes the same address.
            rel.target = SymbolId{ownerOf(symIdx)};
            rel.kind   = kind;
            rel.addend = addend;
            if (patchesText) mod.functions[iv->outIdx].relocations.push_back(rel);
            else             mod.dataItems[iv->outIdx].relocations.push_back(rel);

            // isData inference: an extern reached through a relocation the
            // FORMAT declares `"isCall": true` on is a FUNCTION -- force
            // isData=false. On every shipped PE document callSignalNativeIds
            // is EMPTY (COFF x86_64 has no branch-only relocation), so this is
            // a no-op and the type-hint seed (step 6) stands -- NO fail-loud,
            // which is the COFF-vs-Mach-O difference: COFF carries the hint,
            // Mach-O does not. Reads the declared role so a PE document that
            // does have a branch-only wire type (IMAGE_REL_ARM64_BRANCH26)
            // is honoured here untouched.
            if (auto ex = externBySym.find(symIdx); ex != externBySym.end()) {
                if (callSignalNativeIds.contains(nativeId)) {
                    mod.externImports[ex->second].isData = false;
                }
            }
        }
    }

    mod.expectedFuncCount = mod.functions.size();
    return mod;
}

} // namespace dss::pe
