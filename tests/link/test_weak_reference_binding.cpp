// ── D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE ─────────────────────
//    The WRITER + READER pins for a WEAK extern IMPORT — a reference that may
//    legally resolve to NOTHING — on all three shipped object formats.
//
// ★★★ THIS FILE ASSERTS THE SYMBOL TABLE, NOT THAT THE PROGRAM LINKS, and the
// row demanded exactly that. Linking SUCCEEDED throughout the defect's life and
// is precisely what hid it: an object whose weak import is marked STRONG links
// perfectly well against a present definition, and diverges only when the
// definition is ABSENT — which is the one scenario weak imports exist for. A
// test that linked and ran a program with the symbol present could never have
// seen it. The RUN witness for the absent case is the corpus example
// `examples/c/weak_extern_import_null`; what lives here is the wire encoding.
//
// ★★ THE DEFECT WAS THAT AN UNDERSTOOD ATTRIBUTE STOPPED ONE LAYER ABOVE THE
// WIRE. `weak` on an extern import reached the HIR linkage map and stopped;
// HIR→MIR consumed that map for function DEFINITIONS and GLOBALS only, so the
// bit was parsed, recorded, and dropped where nothing upstream could report it.
// ✔MEASURED at HEAD before the fix, one source (`extern int ea
// __attribute__((weak)); int main(void){ if (&ea) return ea; return 42; }`),
// three formats: ELF `NOTYPE GLOBAL DEFAULT UND ea`, Mach-O `(undefined)
// external _ea` with `Flags [ (0x0) ]`, COFF `StorageClass: External (0x2)`.
// The references, on identical source: `NOTYPE WEAK DEFAULT UND ea` (gcc 13.3.0
// AND clang 18.1.3), `(undefined) weak external _ea` (clang), and
// `StorageClass: WeakExternal (0x69)` + an Auxiliary Format 3 record naming an
// ABSOLUTE value-0 default (clang, for BOTH `x86_64-w64-windows-gnu` and
// `x86_64-pc-windows-msvc`).
//
// ★ EVERY WEAK ASSERTION HERE SITS BESIDE ITS GLOBAL CONTROL, in the same
// object, built from the same fixture. A writer that regressed to "mark every
// undefined symbol weak" would be a worse defect than the one being fixed — it
// makes a REQUIRED symbol optional, so the program links with it missing and
// then reads through null — and a file that only asserted the weak encoding
// would go green over it.
//
// ⚠ MUST run through ctest, never a bare `.exe`: the shipped-config resolver
// walks the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets
// it. A bare binary reads whichever config tree the shell happens to stand in.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/object_format_schema.hpp"

#include "link_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;
using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;

namespace {

// One x86_64 relocatable module with TWO undefined externs, referenced from the
// same function: `wk` declared WEAK and `st` declared strong. Both are unbound
// (empty `libraryPath`) — the bare-prototype cross-TU shape a source `extern`
// produces — because that is the shape the row's own repro has, and because a
// library-bound import would drag import-table machinery into a symbol-table
// question.
//
// The two rows differ in EXACTLY ONE FIELD. That is what makes every assertion
// below attributable: any difference in the emitted records is caused by
// `binding` and by nothing else.
struct Fixture {
    AssembledModule mod;
    static constexpr std::uint32_t kFnSym   = 1;
    static constexpr std::uint32_t kWeakSym = 2;
    static constexpr std::uint32_t kStrongSym = 3;
};

[[nodiscard]] Fixture makeFixture(RelocationKind callKind) {
    Fixture f;
    f.mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{Fixture::kFnSym};
    // Two `call rel32` slots, one relocation each — the bytes are never
    // executed here, only their relocation targets matter.
    fn.bytes = {0xE8, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(Relocation{1u, SymbolId{Fixture::kWeakSym},
                                        callKind, 0});
    fn.relocations.push_back(Relocation{6u, SymbolId{Fixture::kStrongSym},
                                        callKind, 0});
    f.mod.functions.push_back(fn);
    f.mod.symbols = {ModuleSymbol{SymbolId{Fixture::kFnSym}, "f",
                                  SymbolBinding::Global,
                                  SymbolVisibility::Default}};

    ExternImport weakRow;
    weakRow.symbol      = SymbolId{Fixture::kWeakSym};
    weakRow.mangledName = "wk";
    weakRow.binding     = SymbolBinding::Weak;
    ExternImport strongRow;
    strongRow.symbol      = SymbolId{Fixture::kStrongSym};
    strongRow.mangledName = "st";
    strongRow.binding     = SymbolBinding::Global;
    f.mod.externImports = {weakRow, strongRow};
    return f;
}

// The Mach-O fixture needs its own mangling: ld64 keys on a leading underscore,
// and the pipeline has already applied it by the time a row reaches a writer.
[[nodiscard]] Fixture makeMachOFixture(RelocationKind callKind) {
    Fixture f = makeFixture(callKind);
    f.mod.symbols[0].name              = "_f";
    f.mod.externImports[0].mangledName = "_wk";
    f.mod.externImports[1].mangledName = "_st";
    return f;
}

struct Loaded {
    std::optional<std::shared_ptr<TargetSchema const>>       target;
    std::optional<std::shared_ptr<ObjectFormatSchema const>> format;
};

[[nodiscard]] Loaded load(std::string_view targetName,
                          std::string_view formatName) {
    Loaded l;
    auto t = TargetSchema::loadShipped(std::string{targetName});
    auto f = ObjectFormatSchema::loadShipped(std::string{formatName});
    if (t) l.target = *t;
    if (f) l.format = *f;
    return l;
}

// The x86_64 `call rel32` relocation kind, taken from the SHIPPED target schema
// by FORMULA rather than retyped: a 4-byte PC-relative reloc. A hardcoded
// number here would keep passing on the day the taxonomy changed and the writer
// stopped emitting what this project actually emits.
[[nodiscard]] std::optional<RelocationKind> pcRel32Kind(TargetSchema const& t) {
    for (auto const& r : t.relocations())
        if (r.widthBytes == 4 && r.pcRelative) return r.kind;
    return std::nullopt;
}

}  // namespace

// ── 1. ELF: STB_WEAK in st_info, on the SHN_UNDEF entry ──────────────────
//
// ELF spells a weak reference and a weak definition in the ONE `st_info`
// binding field, so there is nothing extra to encode and the writer reaches it
// through the same `stbForBinding` a defined symbol uses.
TEST(WeakReferenceBinding, ElfUndefinedWeakImportTakesStbWeakBesideAStrongOne) {
    auto l = load("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(l.target.has_value());
    ASSERT_TRUE(l.format.has_value());
    auto const kind = pcRel32Kind(**l.target);
    ASSERT_TRUE(kind.has_value()) << "x86_64 must declare a 4-byte PC-relative reloc";

    Fixture f = makeFixture(*kind);
    DiagnosticReporter rep;
    auto bytes = elf::encode(f.mod, **l.target, **l.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Walk `.symtab` / `.strtab` and pick out the two undefined entries by NAME.
    std::uint64_t const shoff    = readU64LE(bytes, 40);
    std::uint16_t const shnum    = readU16LE(bytes, 60);
    std::uint16_t const shstrndx = readU16LE(bytes, 62);
    std::uint64_t const shstrOff =
        readU64LE(bytes, shoff + shstrndx * 64u + 24);
    std::uint64_t symtabOff = 0, symtabSize = 0, strtabOff = 0;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const hdr  = shoff + i * 64u;
        std::uint32_t const nOff = readU32LE(bytes, hdr);
        std::string const name(
            reinterpret_cast<char const*>(bytes.data() + shstrOff + nOff));
        if (name == ".symtab") {
            symtabOff  = readU64LE(bytes, hdr + 24);
            symtabSize = readU64LE(bytes, hdr + 32);
        } else if (name == ".strtab") {
            strtabOff = readU64LE(bytes, hdr + 24);
        }
    }
    ASSERT_GT(symtabOff, 0u);
    ASSERT_GT(strtabOff, 0u);

    constexpr std::uint8_t kStbGlobal = 1, kStbWeak = 2;
    constexpr std::uint16_t kShnUndef = 0;
    std::optional<std::uint8_t> weakBind, strongBind;
    std::optional<std::uint16_t> weakShndx;
    for (std::uint64_t o = 0; o + 24 <= symtabSize; o += 24) {
        std::uint64_t const e    = symtabOff + o;
        std::uint32_t const nOff = readU32LE(bytes, e);
        std::string const name(
            reinterpret_cast<char const*>(bytes.data() + strtabOff + nOff));
        std::uint8_t  const info  = bytes[e + 4];
        std::uint16_t const shndx = readU16LE(bytes, e + 6);
        if (name == "wk") { weakBind = static_cast<std::uint8_t>(info >> 4); weakShndx = shndx; }
        if (name == "st") { strongBind = static_cast<std::uint8_t>(info >> 4); }
    }
    ASSERT_TRUE(weakBind.has_value()) << "the weak import must appear in .symtab by name";
    ASSERT_TRUE(strongBind.has_value()) << "the strong import must appear too";
    EXPECT_EQ(*weakBind, kStbWeak)
        << "a WEAK extern import must be STB_WEAK. gcc 13.3.0 and clang 18.1.3 both "
           "emit `NOTYPE WEAK DEFAULT UND` for `extern int ea __attribute__((weak));` "
           "and both link+run a program that tests it for null with no definition "
           "present; DSS emitted STB_GLOBAL, which makes the same program a link error.";
    EXPECT_EQ(*strongBind, kStbGlobal)
        << "the CONTROL: an unannotated import must stay STB_GLOBAL. Marking every "
           "undefined symbol weak would be the worse defect -- a REQUIRED symbol "
           "becomes optional, the image links with it missing, and the reference "
           "reads through null with no diagnostic.";
    EXPECT_EQ(*weakShndx, kShnUndef)
        << "a weak REFERENCE is still UNDEFINED -- it defines nothing.";
}

// ── 2. Mach-O: N_WEAK_REF (0x0040) in n_desc, on the N_UNDF entry ────────
//
// A different bit from N_WEAK_DEF (0x0080) in the SAME field, on a different
// kind of symbol. The two are adjacent and conflating them would publish a weak
// reference as a weak definition of nothing.
TEST(WeakReferenceBinding, MachOUndefinedWeakImportTakesNWeakRefBesideAStrongOne) {
    auto l = load("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(l.target.has_value());
    ASSERT_TRUE(l.format.has_value());
    auto const kind = pcRel32Kind(**l.target);
    ASSERT_TRUE(kind.has_value());

    Fixture f = makeMachOFixture(*kind);
    DiagnosticReporter rep;
    auto bytes = macho::encode(f.mod, **l.target, **l.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Find LC_SYMTAB (0x02) among the load commands and read its nlist band.
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::uint64_t off = 32;   // mach_header_64 is 32 bytes
    std::uint32_t symoff = 0, nsyms = 0, stroff = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmd == 0x02u) {
            symoff = readU32LE(bytes, off + 8);
            nsyms  = readU32LE(bytes, off + 12);
            stroff = readU32LE(bytes, off + 16);
        }
        off += cmdsize;
    }
    ASSERT_GT(nsyms, 0u);

    constexpr std::uint16_t kNWeakRef = 0x0040;
    constexpr std::uint8_t  kNTypeMask = 0x0e, kNTypeUndf = 0x00;
    std::optional<std::uint16_t> weakDesc, strongDesc;
    std::optional<std::uint8_t>  weakType;
    for (std::uint32_t i = 0; i < nsyms; ++i) {
        std::uint64_t const e = symoff + i * 16u;
        std::uint32_t const strx = readU32LE(bytes, e);
        std::string const name(
            reinterpret_cast<char const*>(bytes.data() + stroff + strx));
        if (name == "_wk") { weakDesc = readU16LE(bytes, e + 6); weakType = bytes[e + 4]; }
        if (name == "_st") { strongDesc = readU16LE(bytes, e + 6); }
    }
    ASSERT_TRUE(weakDesc.has_value()) << "the weak import must appear in the nlist band";
    ASSERT_TRUE(strongDesc.has_value());
    EXPECT_EQ(static_cast<std::uint16_t>(*weakDesc & kNWeakRef), kNWeakRef)
        << "a WEAK extern import must carry N_WEAK_REF (0x0040) in n_desc. clang "
           "18.1.3 emits `(undefined) weak external _ea` for this source; DSS "
           "emitted `(undefined) external _ea` with n_desc 0.";
    EXPECT_EQ(static_cast<std::uint16_t>(*strongDesc & kNWeakRef), 0u)
        << "the CONTROL: an unannotated import must carry no weak-reference bit.";
    ASSERT_TRUE(weakType.has_value());
    EXPECT_EQ(static_cast<std::uint8_t>(*weakType & kNTypeMask), kNTypeUndf)
        << "a weak REFERENCE is still N_UNDF -- it defines nothing.";
}

// ── 3. COFF: IMAGE_SYM_CLASS_WEAK_EXTERNAL + an ABSOLUTE value-0 default ──
//
// ⚠⚠ THE ROW THIS PIN CLOSES CARRIED A CLAIM THAT PE "HAS NO DIRECT EQUIVALENT"
// AND MUST THEREFORE FAIL LOUD. That was its author's INFERENCE, never a
// measurement, and it is REFUTED: ✔MEASURED that clang emits a real COFF weak
// external for BOTH windows triples, that mingw ld LINKS such an object with no
// definition present, and that the resulting PE executable RUNS taking the null
// branch (exit 42) while the same object linked WITH a definition returns 7.
// One working reference makes the behaviour required, so PE takes the same route
// as ELF and Mach-O.
//
// ⓘ mingw-w64 gcc 13.2.0 ACCEPTS the attribute and then emits a plain strong
// UNDEF whose link FAILS -- it is not a working reference for this construct and
// casts no vote for its own output. "It compiled" is not "the weak import works".
TEST(WeakReferenceBinding, CoffUndefinedWeakImportTakesWeakExternalWithAnAbsoluteZeroDefault) {
    auto l = load("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(l.target.has_value());
    ASSERT_TRUE(l.format.has_value());
    auto const kind = pcRel32Kind(**l.target);
    ASSERT_TRUE(kind.has_value());

    Fixture f = makeFixture(*kind);
    DiagnosticReporter rep;
    auto bytes = pe::encode(f.mod, **l.target, **l.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // IMAGE_FILE_HEADER: PointerToSymbolTable @8, NumberOfSymbols @12.
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    std::uint32_t const nSyms  = readU32LE(bytes, 12);
    std::uint32_t const strTab = symPtr + nSyms * 18u;
    ASSERT_GT(nSyms, 0u);

    auto symName = [&](std::uint32_t idx) -> std::string {
        std::uint64_t const e = symPtr + idx * 18u;
        if (readU32LE(bytes, e) == 0u) {
            std::uint32_t const off = readU32LE(bytes, e + 4);
            return std::string(
                reinterpret_cast<char const*>(bytes.data() + strTab + off));
        }
        std::string s;
        for (int b = 0; b < 8 && bytes[e + b] != 0; ++b)
            s.push_back(static_cast<char>(bytes[e + b]));
        return s;
    };

    constexpr std::uint8_t kClassExternal     = 2;
    constexpr std::uint8_t kClassWeakExternal = 105;
    constexpr std::int16_t kSectUndef         = 0;
    constexpr std::int16_t kSectAbsolute      = -1;
    constexpr std::uint32_t kSearchAlias      = 3;

    std::optional<std::uint32_t> weakIdx, strongIdx;
    for (std::uint32_t i = 0; i < nSyms;) {
        std::string const n = symName(i);
        if (n == "wk") weakIdx = i;
        if (n == "st") strongIdx = i;
        i += 1u + bytes[symPtr + i * 18u + 17];   // skip this record's aux slots
    }
    ASSERT_TRUE(weakIdx.has_value()) << "the weak import must appear in the symbol table";
    ASSERT_TRUE(strongIdx.has_value());

    std::uint64_t const we = symPtr + *weakIdx * 18u;
    EXPECT_EQ(bytes[we + 16], kClassWeakExternal)
        << "a WEAK extern import must be IMAGE_SYM_CLASS_WEAK_EXTERNAL (105). "
           "clang emits exactly this for both windows triples; DSS emitted "
           "IMAGE_SYM_CLASS_EXTERNAL, and a program that tests the symbol for "
           "null then cannot link at all.";
    EXPECT_EQ(static_cast<std::int16_t>(readU16LE(bytes, we + 12)), kSectUndef)
        << "PE/COFF 5.5.3 requires the weak external's own record to be UNDEF.";
    EXPECT_EQ(readU32LE(bytes, we + 8), 0u)
        << "PE/COFF 5.5.3 requires Value zero on the weak external's own record.";
    ASSERT_EQ(bytes[we + 17], 1u)
        << "the weak external must carry exactly one Auxiliary Format 3 record -- "
           "without it the record names no fallback and the linker has nothing to "
           "resolve the symbol to.";

    // Auxiliary Format 3: TagIndex[4], Characteristics[4].
    std::uint64_t const aux = we + 18u;
    std::uint32_t const tagIdx = readU32LE(bytes, aux);
    EXPECT_EQ(readU32LE(bytes, aux + 4), kSearchAlias)
        << "Characteristics must be IMAGE_WEAK_EXTERN_SEARCH_ALIAS (3) -- the only "
           "value under which link.exe resolves a weak external at all (MEASURED "
           "one object per value; 1 and 2 both produce LNK2019).";
    ASSERT_LT(tagIdx, nSyms);
    std::uint64_t const de = symPtr + tagIdx * 18u;
    EXPECT_EQ(static_cast<std::int16_t>(readU16LE(bytes, de + 12)), kSectAbsolute)
        << "the fallback must be an ABSOLUTE symbol -- that is what makes an "
           "unresolved weak reference an ADDRESS rather than a link error.";
    EXPECT_EQ(readU32LE(bytes, de + 8), 0u)
        << "the fallback's Value must be 0 -- the null the reference resolves to.";
    EXPECT_EQ(bytes[de + 16], kClassExternal)
        << "the fallback must be EXTERNAL, and this pin exists because the "
           "cheaper choice was measured WRONG. A MODULE-PRIVATE "
           "(IMAGE_SYM_CLASS_STATIC) fallback cannot collide across objects and "
           "passed both mingw ld and a run to the null branch -- and link.exe "
           "14.51 refuses such an object outright with LNK1235 'corrupt or "
           "invalid COFF symbol table', while a DSS object with no weak import "
           "links and runs under the same command. Two working linkers said "
           "nothing; the third is what turned a defensible choice into a "
           "measured one.";
    EXPECT_EQ(symName(tagIdx), std::string{".weak.wk.default.f"})
        << "the fallback's name carries the FIRST symbol that references the "
           "import -- clang's scheme -- so two objects that weak-import one name "
           "do not both publish a fallback under the same EXTERNAL symbol. It "
           "leads with `.` so it can never collide with a C identifier.";

    std::uint64_t const se = symPtr + *strongIdx * 18u;
    EXPECT_EQ(bytes[se + 16], kClassExternal)
        << "the CONTROL: an unannotated import must stay IMAGE_SYM_CLASS_EXTERNAL.";
    EXPECT_EQ(bytes[se + 17], 0u)
        << "the CONTROL carries no auxiliary record -- only a weak external does.";
}

// ── 4. The WRITE/READ round trip, per format ─────────────────────────────
//
// ★ THIS IS THE HALF THAT KEEPS THE WRITER HONEST. A bit the writer emits and
// no reader lifts is indistinguishable from a bit nobody emits — that is the
// stated reason the Mach-O reader lifts N_WEAK_DEF — and DSS reads its own
// objects back on the static-link path, so a weak import that came back Global
// would be a silent weak→strong downgrade inside one toolchain.
//
// ⓘ THE COFF ARM ALSO RETIRES A REFUSAL. `coff_object_reader` used to FAIL LOUD
// on this exact shape, citing this row by name: "DSS's link-tier symbol model
// has no way to carry it: ExternImport declares no binding on ANY format". That
// was true when written; the binding now exists, so refusing would mean refusing
// a shape DSS itself emits.
TEST(WeakReferenceBinding, EveryFormatReadsBackTheWeakReferenceItWrote) {
    struct Leg {
        char const*   targetName;
        char const*   formatName;
        bool          machoNames;
    };
    Leg const legs[] = {
        {"x86_64", "elf64-x86_64-linux",   false},
        {"x86_64", "macho64-x86_64-darwin", true},
        {"x86_64", "pe64-x86_64-windows",  false},
    };
    for (Leg const& leg : legs) {
        SCOPED_TRACE(leg.formatName);
        auto l = load(leg.targetName, leg.formatName);
        ASSERT_TRUE(l.target.has_value());
        ASSERT_TRUE(l.format.has_value());
        auto const kind = pcRel32Kind(**l.target);
        ASSERT_TRUE(kind.has_value());

        Fixture f = leg.machoNames ? makeMachOFixture(*kind) : makeFixture(*kind);
        DiagnosticReporter wrep;
        std::vector<std::uint8_t> bytes;
        if (std::string_view{leg.formatName}.starts_with("elf"))
            bytes = elf::encode(f.mod, **l.target, **l.format, wrep);
        else if (std::string_view{leg.formatName}.starts_with("macho"))
            bytes = macho::encode(f.mod, **l.target, **l.format, wrep);
        else
            bytes = pe::encode(f.mod, **l.target, **l.format, wrep);
        ASSERT_EQ(wrep.errorCount(), 0u);

        DiagnosticReporter rrep;
        std::optional<AssembledModule> got;
        if (std::string_view{leg.formatName}.starts_with("elf"))
            got = elf::readRelocatableObject(bytes, **l.target, **l.format, rrep);
        else if (std::string_view{leg.formatName}.starts_with("macho"))
            got = macho::readRelocatableObject(bytes, **l.target, **l.format, rrep);
        else
            got = pe::readRelocatableObject(bytes, **l.target, **l.format, rrep);
        ASSERT_TRUE(got.has_value())
            << "the object must read back (errors=" << rrep.errorCount() << ")";

        std::optional<SymbolBinding> weakBack, strongBack;
        for (auto const& e : got->externImports) {
            if (e.mangledName == "wk" || e.mangledName == "_wk") weakBack = e.binding;
            if (e.mangledName == "st" || e.mangledName == "_st") strongBack = e.binding;
        }
        ASSERT_TRUE(weakBack.has_value())
            << "the weak import must survive the round trip as an import at all";
        ASSERT_TRUE(strongBack.has_value());
        EXPECT_EQ(*weakBack, SymbolBinding::Weak)
            << "a weak reference that comes back Global is a SILENT weak->strong "
               "downgrade inside one toolchain: DSS reads its own objects on the "
               "static-link path, so the property would be lost with no diagnostic.";
        EXPECT_EQ(*strongBack, SymbolBinding::Global)
            << "the CONTROL: a strong reference must not come back weak.";
    }
}

// ── 5. The cross-CU fold: STRONGEST WINS ─────────────────────────────────
//
// Two CUs importing one identity, one declaring it weak and one not. A strong
// reference anywhere in the program makes the symbol REQUIRED, so the surviving
// row must bind Global — the opposite rule produces an image that links with the
// symbol absent and then reads through a null address, which is a silent wrong
// answer rather than a link error.
//
// Pinned on the shared owner rather than through a merge, because BOTH merge
// tiers (`mir_merge.cpp`'s ffiImportKey group and `linker.cpp`'s dedupKey group)
// call this one function; a pin per tier would be two pins of one property.
// ⚠ The `Local` arms are not reachable input — `collectExterns` refuses a Local
// binding on an extern at the declaration — but the function is TOTAL, and the
// arms are pinned so a future producer that forgets that refusal cannot get
// `Local` back out and put an unspellable binding on an undefined symbol.
TEST(WeakReferenceBinding, StrongestReferenceBindingWinsAndIsOrderIndependent) {
    EXPECT_EQ(strongerReferenceBinding(SymbolBinding::Weak, SymbolBinding::Global),
              SymbolBinding::Global);
    EXPECT_EQ(strongerReferenceBinding(SymbolBinding::Global, SymbolBinding::Weak),
              SymbolBinding::Global)
        << "order-independent: whichever CU's row lands first, the fold is the same.";
    EXPECT_EQ(strongerReferenceBinding(SymbolBinding::Weak, SymbolBinding::Weak),
              SymbolBinding::Weak)
        << "two weak references stay weak -- nothing required the symbol.";
    EXPECT_EQ(strongerReferenceBinding(SymbolBinding::Local, SymbolBinding::Local),
              SymbolBinding::Weak)
        << "no arm may return Local: an undefined LOCAL symbol is unspellable in "
           "every format, so the fold must never hand one to a writer.";
    EXPECT_EQ(strongerReferenceBinding(SymbolBinding::Local, SymbolBinding::Global),
              SymbolBinding::Global);
}
