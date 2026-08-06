// Neutral shipped-lib descriptor reader tests (closes the reader half of
// D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC).
//
// `readShippedLibDescriptor` reads a LANGUAGE-NEUTRAL JSON descriptor + decodes
// each symbol's hir-text `signature` via the ONE `parseTypeFromText` decoder
// into the caller's interner. Pins (strict, red-on-disable):
//   * a well-formed stdio.json → `puts` with signature decoded to EXACTLY
//     FnSig(result I32, one param Ptr<Char>) — inspected STRUCTURALLY via the
//     interner accessors, never a string compare.
//   * malformed JSON / missing required key → F_ShippedLibDescriptorMalformed
//     + nullopt.
//   * a truncated / unknown signature → F_ShippedLibUnsupportedType + nullopt,
//     and NO symbol returned with InvalidType (the CRITICAL fail-loud).

#include "core/types/aggregate_layout.hpp"           // AggregateLayoutParams (variant-layout pins)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"          // ObjectFormatKind (variant selector)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"               // TargetSchema (crux re-verify, gate 2)
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"    // computeLayout (variant offset pins)
#include "core/types/type_lattice/type_registry.hpp"
#include "diagnostic_count.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// Write `content` to a fresh `<scratch>/<name>` and return the path.
[[nodiscard]] fs::path writeTemp(ScratchDir const& dir, std::string const& name,
                                 std::string const& content) {
    fs::path const p = dir.path() / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

// True iff SOME reported diagnostic's text contains `needle`. The descriptor
// reader packs its whole message into `ParseDiagnostic::actual` (see
// `emitMalformed` → `dss::report`). Used by the sentinel pins below: they all
// share ONE code (`F_ShippedLibDescriptorMalformed`), so `hasErrors()` alone
// cannot tell the intended rejection from an unrelated one in the same fixture.
[[nodiscard]] bool anyDiagMentions(DiagnosticReporter const& rep,
                                   std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ── Happy path: structural FnSig inspection ──────────────────────────────────

TEST(ShippedLibDescriptor, ReadsPutsWithDecodedFnSig) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "stdio.json", R"({
        "header": "stdio.h",
        "library": { "pe": "msvcrt.dll", "elf": "libc.so.6", "macho": "/usr/lib/libSystem.B.dylib" },
        "symbols": [
            { "name": "puts", "signature": "fn(ptr<char>) -> i32",
              "kind": "function", "linkage": "external" }
        ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    // Model 3: `library` is a per-object-format map; assert each entry.
    EXPECT_EQ(desc->library.size(), 3u);
    EXPECT_EQ(desc->library.at("pe"), "msvcrt.dll");
    EXPECT_EQ(desc->library.at("elf"), "libc.so.6");
    EXPECT_EQ(desc->library.at("macho"), "/usr/lib/libSystem.B.dylib");
    EXPECT_EQ(desc->header, "stdio.h");   // provenance, first-class
    ASSERT_EQ(desc->symbols.size(), 1u);

    auto const& sym = desc->symbols[0];
    EXPECT_EQ(sym.name, "puts");
    EXPECT_EQ(sym.kind, ShippedSymbolKind::Function);
    EXPECT_EQ(sym.linkage, ShippedSymbolLinkage::External);

    // Structural inspection — NOT a string compare. The signature must be a
    // FnSig(result=I32, params=[Ptr<Char>]).
    ASSERT_TRUE(sym.signature.valid());
    ASSERT_EQ(interner.kind(sym.signature), TypeKind::FnSig);
    EXPECT_EQ(interner.kind(interner.fnResult(sym.signature)), TypeKind::I32);
    auto const params = interner.fnParams(sym.signature);
    ASSERT_EQ(params.size(), 1u);
    ASSERT_EQ(interner.kind(params[0]), TypeKind::Ptr);
    auto const ptrElem = interner.operands(params[0]);
    ASSERT_EQ(ptrElem.size(), 1u);
    EXPECT_EQ(interner.kind(ptrElem[0]), TypeKind::Char);
}

// `library` is optional — absent ⇒ empty map (resolution then falls back to the
// language's externLibraryByFormat default for every format).
TEST(ShippedLibDescriptor, LibraryIsOptional) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "nolib.json", R"({
        "header": "x.h",
        "symbols": [ { "name": "f", "signature": "fn() -> void" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->library.empty());
    ASSERT_EQ(desc->symbols.size(), 1u);
    EXPECT_EQ(desc->symbols[0].kind, ShippedSymbolKind::Function);     // default
    EXPECT_EQ(desc->symbols[0].linkage, ShippedSymbolLinkage::External); // default
}

// c14: a VARIADIC external symbol — `vopen(path, flags, ...)` (the POSIX open/fcntl
// shape SQLite needs) decodes; the trailing `...` in the signature text produces a
// variadic FnSig whose declared params are the FIXED prefix. RED-ON-DISABLE: revert
// the hir_text `...`/Ellipsis parser support and the signature fails to decode
// (F_ShippedLibUnsupportedType → symbol dropped → read nullopt).
TEST(ShippedLibDescriptor, SymbolVariadicSignatureDecodes) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "var_sym.json", R"JSON({
        "header": "vs.h",
        "library": { "elf": "libc.so.6" },
        "symbols": [
            { "name": "vopen", "signature": "fn(ptr<char>, i32, ...) -> i32", "kind": "function", "linkage": "external" },
            { "name": "fixed", "signature": "fn(i32, i32) -> i32",            "kind": "function", "linkage": "external" }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->symbols.size(), 2u);
    // vopen: variadic FnSig with 2 FIXED params (the `...` is a marker, not a param).
    EXPECT_EQ(interner.kind(desc->symbols[0].signature), TypeKind::FnSig);
    EXPECT_TRUE(interner.fnIsVariadic(desc->symbols[0].signature));
    EXPECT_EQ(interner.fnParams(desc->symbols[0].signature).size(), 2u);
    // fixed: NON-variadic (the ordinary fnSig path stays non-variadic).
    EXPECT_FALSE(interner.fnIsVariadic(desc->symbols[1].signature));
}

// c15d (D-SHIPPED-SYMBOL-PER-TARGET-AVAILABILITY): a symbol may carry a per-symbol
// `availableObjectFormats` — errno's `__error` is Darwin-only, `__errno_location`
// glibc-only. The decode populates the per-symbol set; empty/absent = every format.
// The membership predicate (the SAME one the semantic injection gate uses) selects
// per active format. RED-ON-DISABLE: drop the field from the struct/decode and the
// asserted sets go empty → the predicate admits the wrong-format accessor → an
// undefined import at load (the run-28240524858 CI break this repairs).
TEST(ShippedLibDescriptor, SymbolPerTargetAvailabilityDecodesAndSelects) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "errno_like.json", R"JSON({
        "header": "errno.h",
        "availableObjectFormats": ["elf", "macho"],
        "library": { "elf": "libc.so.6", "macho": "/usr/lib/libSystem.B.dylib" },
        "symbols": [
            { "name": "__errno_location", "signature": "fn() -> ptr<i32>", "availableObjectFormats": ["elf"] },
            { "name": "__error",          "signature": "fn() -> ptr<i32>", "availableObjectFormats": ["macho"] },
            { "name": "both",             "signature": "fn() -> i32" }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->symbols.size(), 3u);
    // The per-symbol sets decode exactly.
    ASSERT_EQ(desc->symbols[0].availableObjectFormats.size(), 1u);
    EXPECT_EQ(desc->symbols[0].availableObjectFormats[0], "elf");
    ASSERT_EQ(desc->symbols[1].availableObjectFormats.size(), 1u);
    EXPECT_EQ(desc->symbols[1].availableObjectFormats[0], "macho");
    EXPECT_TRUE(desc->symbols[2].availableObjectFormats.empty());   // = every format
    // The injection-gate predicate selects the format-correct accessor ONLY.
    // __errno_location: elf yes, macho NO.
    EXPECT_TRUE(objectFormatInAvailabilitySet(desc->symbols[0].availableObjectFormats,
                                              ObjectFormatKind::Elf));
    EXPECT_FALSE(objectFormatInAvailabilitySet(desc->symbols[0].availableObjectFormats,
                                               ObjectFormatKind::MachO));
    // __error: macho yes, elf NO (the bug: declaring it on elf → undefined import).
    EXPECT_FALSE(objectFormatInAvailabilitySet(desc->symbols[1].availableObjectFormats,
                                               ObjectFormatKind::Elf));
    EXPECT_TRUE(objectFormatInAvailabilitySet(desc->symbols[1].availableObjectFormats,
                                              ObjectFormatKind::MachO));
    // empty set = injected on EVERY format (back-compat — almost every symbol).
    EXPECT_TRUE(objectFormatInAvailabilitySet(desc->symbols[2].availableObjectFormats,
                                              ObjectFormatKind::Elf));
    EXPECT_TRUE(objectFormatInAvailabilitySet(desc->symbols[2].availableObjectFormats,
                                              ObjectFormatKind::MachO));
}

// D-CSUBSET-C11-THREADS-MACHO: the C11 `tss_t` (pthread_key_t) width DIVERGES per format —
// 8 bytes (u64) on macho, 4 (u32) on elf/pe. The 3-way typedef variant must select the
// format-correct width; a wrong width is a SILENT tss miscompile (the macho witness's
// tss_create/set/get round-trip would corrupt the key). Structural, host-independent pin
// for the width the named-import synth test cannot see. RED-on-disable: collapse the macho
// variant to u32 and the MachO assertion fails.
TEST(ShippedLibDescriptor, ThreadsTssKeyTypedefWidthDivergesPerFormat) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "threads_tss.json", R"JSON({
        "header": "threads.h",
        "availableObjectFormats": ["elf", "pe", "macho"],
        "typedefs": [
            { "name": "tss_t", "variants": [
                { "when": { "format": "elf" },   "type": "u32" },
                { "when": { "format": "pe" },    "type": "u32" },
                { "when": { "format": "macho" }, "type": "u64" }
            ] }
        ]
    })JSON");
    auto tssIsKind = [&](ObjectFormatKind fmt, TypeKind expected) -> bool {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, "arm64", fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        for (auto const& td : desc->typedefs)
            if (td.name == "tss_t") return td.type == interner.primitive(expected);
        ADD_FAILURE() << "tss_t typedef missing";
        return false;
    };
    EXPECT_TRUE(tssIsKind(ObjectFormatKind::MachO, TypeKind::U64))
        << "macho tss_t must be u64 (pthread_key_t = 8 bytes)";
    EXPECT_TRUE(tssIsKind(ObjectFormatKind::Elf, TypeKind::U32))
        << "elf tss_t must be u32 (glibc pthread_key_t = 4 bytes)";
    EXPECT_FALSE(tssIsKind(ObjectFormatKind::MachO, TypeKind::U32))
        << "the macho variant must NOT collapse to the elf u32 width";
}

// An unknown per-symbol availability format name fails loud (closed vocabulary,
// the SAME `objectFormatKindFromName` the header-level set + `library` keys use).
TEST(ShippedLibDescriptor, SymbolPerTargetAvailabilityUnknownFormatFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad_sym_avail.json", R"JSON({
        "header": "x.h",
        "symbols": [
            { "name": "f", "signature": "fn() -> i32", "availableObjectFormats": ["elf", "bogus"] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());   // malformed → whole read fails
    EXPECT_TRUE(rep.hasErrors());
}

// ── c156: per-symbol `version` (D-LK-ELF-SYMBOL-VERSIONING) ───────────────────

// ── per-symbol `library` OVERRIDE (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC) ──────

// A symbol carrying its own `library` map decodes the RAW per-symbol override
// (pe->ucrtbase) onto `ShippedSymbol.library`; a sibling with no `library` field
// gets an EMPTY map (it inherits the descriptor's at injection). The descriptor-
// level `library` is unchanged — this layer stores the RAW override, and the
// merge is the semantic injector's job. RED-ON-DISABLE: drop the per-symbol
// `library` decode (or the field) and the override symbol's map goes empty.
TEST(ShippedLibDescriptor, SymbolLibraryOverrideDecodesRawMap) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "time_like.json", R"JSON({
        "header": "time.h",
        "library": { "pe": "msvcrt.dll", "elf": "libc.so.6" },
        "symbols": [
            { "name": "strftime", "signature": "fn() -> i32", "library": { "pe": "ucrtbase.dll" } },
            { "name": "time",     "signature": "fn() -> i64" }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->symbols.size(), 2u);
    // The override symbol carries its RAW per-symbol map (pe->ucrtbase). The
    // descriptor-level map is NOT merged in here (that is the injector's job).
    EXPECT_EQ(desc->symbols[0].name, "strftime");
    ASSERT_EQ(desc->symbols[0].library.size(), 1u);
    EXPECT_EQ(desc->symbols[0].library.at("pe"), "ucrtbase.dll");
    // The sibling declares no override → empty map (inherits at injection).
    EXPECT_EQ(desc->symbols[1].name, "time");
    EXPECT_TRUE(desc->symbols[1].library.empty());
    // The descriptor-level map is untouched by the per-symbol override.
    EXPECT_EQ(desc->library.at("pe"), "msvcrt.dll");
    EXPECT_EQ(desc->library.at("elf"), "libc.so.6");
}

// An unknown per-symbol `library` object-format KEY fails loud (the SAME closed
// `objectFormatKindFromName` vocabulary the descriptor-level `library` map uses —
// the shared `decodeLibraryMap` chokepoint).
TEST(ShippedLibDescriptor, SymbolLibraryOverrideUnknownFormatFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad_sym_lib.json", R"JSON({
        "header": "x.h",
        "symbols": [
            { "name": "f", "signature": "fn() -> i32", "library": { "bogus": "x.dll" } }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());   // unknown format key → whole read fails
    EXPECT_TRUE(rep.hasErrors());
}

// ★ THE SENTINEL VARIANT of the test above. "bogus" fails the name lookup;
// "unknown" PASSES it — it is a row in `kObjectFormatKindTable` — and the
// library then resolves for no real format, so the symbol reaches the link with
// no import library. Same outcome as the typo, reached by a correctly-spelled
// word, which is why only an explicit selectability check can catch it.
//
// RED-ON-DISABLE: remove the `isSelectableObjectFormatKind` branch in
// `decodeLibraryMap` and the read succeeds with a dead `unknown` entry.
TEST(ShippedLibDescriptor, SymbolLibraryOverrideSentinelFormatFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "sentinel_sym_lib.json", R"JSON({
        "header": "x.h",
        "symbols": [
            { "name": "f", "signature": "fn() -> i32", "library": { "unknown": "x.dll" } }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_TRUE(anyDiagMentions(rep, "sentinel"))
        << "the diagnostic must say WHY 'unknown' is refused — 'unknown "
           "object-format key' would be a confusing lie for a name that IS in "
           "the table";
}

// A per-symbol `library` VALUE that is not a string fails loud (mirrors the
// descriptor-level map's non-string-value rejection via `decodeLibraryMap`).
TEST(ShippedLibDescriptor, SymbolLibraryOverrideNonStringValueFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad_sym_lib2.json", R"JSON({
        "header": "x.h",
        "symbols": [
            { "name": "f", "signature": "fn() -> i32", "library": { "pe": 123 } }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());   // non-string value → whole read fails
    EXPECT_TRUE(rep.hasErrors());
}

// A per-symbol `library` that is not an OBJECT fails loud (a shape error — the
// `decodeLibraryMap` non-object branch, which skips the symbol; the read then
// fails via the "declares nothing"/errorCount delta).
TEST(ShippedLibDescriptor, SymbolLibraryOverrideNonObjectFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad_sym_lib3.json", R"JSON({
        "header": "x.h",
        "symbols": [
            { "name": "f", "signature": "fn() -> i32", "library": "msvcrt.dll" }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());   // non-object library → whole read fails
    EXPECT_TRUE(rep.hasErrors());
}

// ── c156: per-symbol `version` (D-LK-ELF-SYMBOL-VERSIONING) ───────────────────

// A flat `"version": "GLIBC_2.3"` (arch-invariant) decodes onto the symbol.
TEST(ShippedLibDescriptor, SymbolVersionFlatStringDecodes) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "ver_flat.json", R"JSON({
        "header": "x.h",
        "library": { "elf": "libc.so.6" },
        "symbols": [
            { "name": "f", "signature": "fn() -> i32", "version": "GLIBC_2.3" }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->symbols.size(), 1u);
    EXPECT_EQ(desc->symbols[0].version, "GLIBC_2.3");
}

// The realpath shape: a version required only on x86_64/elf (GLIBC_2.3);
// aarch64's single-versioned baseline needs none → 0 variants match → empty
// (unversioned). RED-ON-DISABLE: a flat "GLIBC_2.3" would wrongly require it
// on arm64, whose libc has no GLIBC_2.3 version node → load failure.
TEST(ShippedLibDescriptor, SymbolVersionVariantSelectsPerTarget) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "ver_variant.json", R"JSON({
        "header": "stdlib.h",
        "library": { "elf": "libc.so.6" },
        "symbols": [
            { "name": "realpath", "signature": "fn(ptr<char>, ptr<char>) -> ptr<char>",
              "version": { "variants": [
                  { "when": { "arch": "x86_64", "format": "elf" }, "value": "GLIBC_2.3" }
              ] } }
        ]
    })JSON");
    auto versionFor = [&](std::string_view arch, ObjectFormatKind fmt) -> std::string {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, arch, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        for (auto const& s : desc->symbols)
            if (s.name == "realpath") return s.version;
        ADD_FAILURE() << "realpath symbol missing";
        return "<none>";
    };
    EXPECT_EQ(versionFor("x86_64", ObjectFormatKind::Elf), "GLIBC_2.3")
        << "x86_64/elf must require GLIBC_2.3 (the misbind-fix target)";
    EXPECT_EQ(versionFor("arm64", ObjectFormatKind::Elf), "")
        << "arm64 has one realpath version → unversioned (no requirement)";
}

// Malformed `version` shapes all fail loud (closed schema, anti-silent-drop).
TEST(ShippedLibDescriptor, SymbolVersionMalformedShapesFailLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    int caseNo = 0;
    auto readsClean = [&](std::string const& body) -> bool {
        auto const path =
            writeTemp(dir, "verbad" + std::to_string(caseNo++) + ".json", body);
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, "x86_64",
                                             ObjectFormatKind::Elf);
        return desc.has_value() && !rep.hasErrors();
    };
    // version is a number (neither string nor object).
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"elf":"libc.so.6"},
        "symbols":[{ "name":"f","signature":"fn() -> i32","version": 3 }] })JSON"));
    // empty flat version string.
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"elf":"libc.so.6"},
        "symbols":[{ "name":"f","signature":"fn() -> i32","version": "" }] })JSON"));
    // version OBJECT with no `variants` array.
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"elf":"libc.so.6"},
        "symbols":[{ "name":"f","signature":"fn() -> i32","version": {} }] })JSON"));
    // a variant missing its `value`.
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"elf":"libc.so.6"},
        "symbols":[{ "name":"f","signature":"fn() -> i32",
            "version": { "variants": [ { "when": { "format": "elf" } } ] } }] })JSON"));
    // two variants BOTH matching the active target (x86_64/elf) → ambiguous.
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"elf":"libc.so.6"},
        "symbols":[{ "name":"f","signature":"fn() -> i32",
            "version": { "variants": [
                { "when": { "format": "elf" }, "value": "GLIBC_2.3" },
                { "when": { "arch": "x86_64" }, "value": "GLIBC_2.2.5" }
            ] } }] })JSON"));
    // sanity: a well-formed variant DOES read clean (the negatives above are
    // not failing for an unrelated reason).
    EXPECT_TRUE(readsClean(R"JSON({ "header":"x.h", "library":{"elf":"libc.so.6"},
        "symbols":[{ "name":"f","signature":"fn() -> i32",
            "version": { "variants": [
                { "when": { "arch": "x86_64", "format": "elf" }, "value": "GLIBC_2.3" }
            ] } }] })JSON"));
}

// ── TF-C121: per-symbol `linkName` (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME) ─
//
// The per-target LINK BASE NAME — the undecorated name the shipped library
// exports for a C identifier ON THIS TARGET. Reader-side pins; the COMPOSITION
// with the format decoration is pinned in tests/ffi/test_c_mangle.cpp
// (`FfiCMangleLinkName.*`) and end-to-end on emitted object bytes in
// tests/program/test_compile_pipeline.cpp.

// A flat `"linkName": "..."` (target-invariant) decodes onto the symbol.
TEST(ShippedLibDescriptor, SymbolLinkNameFlatStringDecodes) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "ln_flat.json", R"JSON({
        "header": "x.h",
        "library": { "macho": "/usr/lib/libSystem.B.dylib" },
        "symbols": [
            { "name": "f", "signature": "fn() -> i32", "linkName": "f$EVERYWHERE" }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->symbols.size(), 1u);
    EXPECT_EQ(desc->symbols[0].linkName, "f$EVERYWHERE");
}

// ★ THE PER-ARCH SPLIT IS THE WHOLE POINT — so BOTH arms are pinned, not just
// the one that motivated the field. The `$INODE64` shape: Darwin's modern
// 64-bit-inode ABI is an asm-label ALIAS on x86_64 and the ONLY ABI on arm64,
// so the x86_64 arm resolves to the alias while arm64 matches NO variant and
// keeps the canonical identifier (an empty `linkName`, which downstream means
// "use `name`"). A flat string here would be exact on x86_64 and WRONG on
// arm64 — merely moving the defect — which is why a one-arm pin would be
// worthless. RED-ON-DISABLE (MEASURED): drop the `linkName` decode from
// shipped_lib_descriptor.cpp and the x86_64 arm reads "" instead of
// "fstat$INODE64".
TEST(ShippedLibDescriptor, SymbolLinkNameVariantSelectsPerArchBothArms) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "ln_variant.json", R"JSON({
        "header": "sys/stat.h",
        "library": { "macho": "/usr/lib/libSystem.B.dylib" },
        "symbols": [
            { "name": "fstat", "signature": "fn(i32, ptr<void>) -> i32",
              "linkName": { "variants": [
                  { "when": { "format": "macho", "arch": "x86_64" }, "value": "fstat$INODE64" }
              ] } }
        ]
    })JSON");
    auto linkNameFor = [&](std::string_view arch, ObjectFormatKind fmt) -> std::string {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, arch, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        for (auto const& s : desc->symbols)
            if (s.name == "fstat") return s.linkName;
        ADD_FAILURE() << "fstat symbol missing";
        return "<none>";
    };
    std::string const x86Macho = linkNameFor("x86_64", ObjectFormatKind::MachO);
    EXPECT_EQ(x86Macho, "fstat$INODE64")
        << "x86_64-Darwin reaches the modern 64-bit-inode ABI through the "
           "$INODE64 alias; binding the plain name reaches the LEGACY "
           "32-bit-inode callee, which writes 120 of 144 bytes and reports "
           "st_size 0";
    EXPECT_EQ(linkNameFor("arm64", ObjectFormatKind::MachO), "")
        << "arm64-Darwin has ONE inode ABI, so the plain name is already "
           "correct — 0 variants match, and empty means 'use `name`'";
    EXPECT_EQ(linkNameFor("x86_64", ObjectFormatKind::Elf), "")
        << "the variant is format-gated too: no Linux symbol carries this alias";
    // ★ The UNDECORATED spelling is load-bearing: the leading `_` Mach-O puts on
    // every C symbol is composed by the ENGINE (applyCMangling), never written
    // in config, so a descriptor value that already carried one would be
    // double-decorated into `__fstat$INODE64`.
    //
    // ★★ `starts_with`, NOT `.front()`, AND THE REASON IS THIS TEST'S OWN
    // RED-ON-DISABLE. Dropping the `linkName` decode makes the x86_64 arm read
    // "" — exactly what the EXPECT_EQ above exists to catch — but `EXPECT_*` is
    // NON-FATAL, so execution then reached `.front()` on that EMPTY string.
    // `std::string::front()` REQUIRES `!empty()` ([string.access]), so that was
    // UB on the one path this test documents as its own failure mode.
    //   ⚠ AND THE MEASURED SYMPTOM WAS THE QUIET ONE, WHICH IS WHY THIS IS
    //   WRITTEN DOWN RATHER THAN JUST FIXED. Run 2026-08-05 on the Windows gate
    //   leg (MinGW g++ / libstdc++, `-g`, no `_GLIBCXX_ASSERTIONS`) with the
    //   decode disabled: the old shape did NOT abort — `front()` returned the
    //   NUL terminator and `EXPECT_NE(..., '_')` PASSED VACUOUSLY. So the defect
    //   was not "the red-on-disable crashes" but "this assertion silently stops
    //   asserting on the exact input it was written for", with a hardened
    //   standard library (or the clang ASan/UBSan leg) free to turn it into a
    //   crash instead. `starts_with` is TOTAL on the empty string: no UB, and
    //   the check keeps its meaning for every input.
    EXPECT_FALSE(x86Macho.starts_with('_'))
        << "config declares the UNDECORATED base name — the leading Mach-O `_` "
           "is the FORMAT's fact, composed downstream by applyCMangling; a "
           "pre-decorated value would emit `__fstat$INODE64`";
}

// Malformed `linkName` shapes all fail loud — the SAME closed-schema battery
// `version` gets, run against the SAME shared decoder
// (`decodePerTargetSymbolString`), which is why the two can never drift.
TEST(ShippedLibDescriptor, SymbolLinkNameMalformedShapesFailLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    int caseNo = 0;
    auto readsClean = [&](std::string const& body) -> bool {
        auto const path =
            writeTemp(dir, "lnbad" + std::to_string(caseNo++) + ".json", body);
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, "x86_64",
                                             ObjectFormatKind::MachO);
        return desc.has_value() && !rep.hasErrors();
    };
    // linkName is a number (neither string nor object).
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"macho":"libSystem"},
        "symbols":[{ "name":"f","signature":"fn() -> i32","linkName": 3 }] })JSON"));
    // empty flat string (omit the key instead — an empty value is ambiguous
    // with "no override" and would hide a truncated edit).
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"macho":"libSystem"},
        "symbols":[{ "name":"f","signature":"fn() -> i32","linkName": "" }] })JSON"));
    // OBJECT with no `variants` array.
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"macho":"libSystem"},
        "symbols":[{ "name":"f","signature":"fn() -> i32","linkName": {} }] })JSON"));
    // a variant missing its `value`.
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"macho":"libSystem"},
        "symbols":[{ "name":"f","signature":"fn() -> i32",
            "linkName": { "variants": [ { "when": { "format": "macho" } } ] } }] })JSON"));
    // an unknown key inside `when` (closed vocabulary — a typo'd "ach" would
    // otherwise widen the match silently).
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"macho":"libSystem"},
        "symbols":[{ "name":"f","signature":"fn() -> i32",
            "linkName": { "variants": [
                { "when": { "ach": "x86_64" }, "value": "f$X" } ] } }] })JSON"));
    // TWO variants both matching the active target → ambiguous, not last-wins.
    EXPECT_FALSE(readsClean(R"JSON({ "header":"x.h", "library":{"macho":"libSystem"},
        "symbols":[{ "name":"f","signature":"fn() -> i32",
            "linkName": { "variants": [
                { "when": { "format": "macho" }, "value": "f$A" },
                { "when": { "arch": "x86_64" }, "value": "f$B" }
            ] } }] })JSON"));
    // sanity: a well-formed variant DOES read clean (the negatives above are
    // not failing for an unrelated reason).
    EXPECT_TRUE(readsClean(R"JSON({ "header":"x.h", "library":{"macho":"libSystem"},
        "symbols":[{ "name":"f","signature":"fn() -> i32",
            "linkName": { "variants": [
                { "when": { "arch": "x86_64", "format": "macho" }, "value": "f$INODE64" }
            ] } }] })JSON"));
}

// ★ THE REAL DESCRIPTORS, every renamed symbol at once — the pin that would have
// caught the shipped bug. MEASURED ground truth (macOS 26.5.2, Apple clang
// 21.0.0), and the control is EXHAUSTIVE rather than a sample: ONE generated TU
// per arch taking `&sym` of ALL 232 macho-visible descriptor FUNCTIONS, only
// `-arch` differing, `cc -c` then `nm -u`, 232 undefined symbols on each side.
//   x86_64 → _stat$INODE64 _fstat$INODE64 _lstat$INODE64 _statfs$INODE64
//            _fstatfs$INODE64 _opendir$INODE64 _readdir$INODE64
//            _realpath$DARWIN_EXTSN
//   arm64  → _stat _fstat _lstat _statfs _fstatfs _opendir _readdir
//            _realpath$DARWIN_EXTSN
// No OTHER name among the 232 is renamed on either arch, so this table is
// COMPLETE and a row added here later needs new measurement, not a guess.
//
// ★★ THE TABLE CARRIES THE EXPECTED NAME PER ARCH, NOT A `diverges` BOOL — AND
// THAT SHAPE IS THE WHOLE LESSON OF THIS TEST. The bool asked "does the emitted
// name differ BETWEEN THE TWO ARCHES". `realpath` answers NO — it is
// `_realpath$DARWIN_EXTSN` on both — so the bool filed it under "needs no
// linkName", and a comment here asserted that as MEASURED and told future
// readers not to change it. THE QUESTION THAT DECIDES A LINK NAME IS A DIFFERENT
// ONE: "does the emitted name differ from THE ONE WE DECLARE." By that question
// realpath diverged on BOTH arches while DSS shipped plain `_realpath`. Its
// variant is `format`-keyed where the seven $INODE64 rows are (format, arch)-
// keyed, so its two cells below spell the SAME string on purpose — a shape a
// cross-arch DIFF is structurally blind to.
//   ⚠ AND IT WAS NOT BENIGN. MEASURED on that host: `realpath` and
//   `realpath$DARWIN_EXTSN` are different functions at different addresses
//   (dlsym, arm64 0x185e82d4c vs 0x185e2a8a0; x86_64 likewise, `same=0`), plain
//   `_realpath` LINKS CLEAN so nothing ever failed loud, and the shipped DSS
//   `sqlite3` Mach-O carries 2 call sites to the plain name. `$DARWIN_EXTSN` is
//   the conforming POSIX-2008 form — the NULL-`resolved` malloc'ing behaviour
//   shell.c actually calls — so the plain name reached the legacy callee.
// `closedir` and `mkdir` stay as the per-SYMBOL controls: plain on BOTH arches,
// so they prove the rename is a property of individual symbols and not of a
// header — a blanket "suffix everything in dirent.json" goes red on `closedir`.
TEST(ShippedLibDescriptor, RealDescriptorsCarryTheirDarwinLinkNames) {
    struct Row { char const* descriptor; char const* symbol;
                 char const* expectX86;  char const* expectArm; };
    constexpr std::array<Row, 10> kRows{{
        {"sys/stat.json", "stat",     "stat$INODE64",          ""},
        {"sys/stat.json", "fstat",    "fstat$INODE64",         ""},
        {"sys/stat.json", "lstat",    "lstat$INODE64",         ""},
        {"unistd.json",   "statfs",   "statfs$INODE64",        ""},
        {"unistd.json",   "fstatfs",  "fstatfs$INODE64",       ""},
        {"dirent.json",   "opendir",  "opendir$INODE64",       ""},
        {"dirent.json",   "readdir",  "readdir$INODE64",       ""},
        {"dirent.json",   "closedir", "",                      ""},  // control
        {"sys/stat.json", "mkdir",    "",                      ""},  // control
        // ★ the same string BOTH sides — format-keyed, not arch-keyed.
        {"stdlib.json",   "realpath", "realpath$DARWIN_EXTSN",
                                      "realpath$DARWIN_EXTSN"},
    }};
    auto const shippedRoot = dss::test::findRepoRoot();
    ASSERT_TRUE(shippedRoot.has_value()) << dss::test::repoRootDiagnostic();
    // Reads the REAL descriptor for one (arch, macho) and returns `symbol`'s
    // resolved `linkName` — "<missing>" if the row is gone, so deleting a row
    // REDS here instead of silently satisfying an empty expectation.
    auto linkNameOf = [&](char const* descriptor, char const* symbol,
                          std::string_view arch) -> std::string {
        fs::path const p =
            *shippedRoot / "src" / "dss-config" / "shippedLibs" / descriptor;
        TypeInterner       interner{CompilationUnitId{1}};
        TypeRegistry       typeReg;
        DiagnosticReporter rep;
        auto const desc = readShippedLibDescriptor(p, interner, typeReg, rep,
                                                   DataModel::Lp64, arch,
                                                   ObjectFormatKind::MachO);
        if (!desc.has_value()) {
            ADD_FAILURE() << descriptor << " failed to read for arch " << arch;
            return "<unreadable>";
        }
        for (auto const& s : desc->symbols)
            if (s.name == symbol) return s.linkName;
        ADD_FAILURE() << descriptor << " declares no symbol " << symbol;
        return "<missing>";
    };
    for (auto const& r : kRows) {
        EXPECT_EQ(linkNameOf(r.descriptor, r.symbol, "x86_64"), r.expectX86)
            << r.symbol << " on x86_64-Darwin: the descriptor must declare the "
               "EXACT base name `cc -arch x86_64` emits for this identifier. An "
               "EMPTY expectation means the plain name is already what the "
               "platform binds; a NON-EMPTY one means binding the plain name "
               "reaches a DIFFERENT callee and misbinds SILENTLY (it links "
               "clean either way, which is why only this table can catch it)";
        EXPECT_EQ(linkNameOf(r.descriptor, r.symbol, "arm64"), r.expectArm)
            << r.symbol << " on arm64-Darwin: the same rule, measured "
               "separately. The seven $INODE64 names are PLAIN here while "
               "`realpath` is NOT — which is precisely why the expectation is "
               "per-arch DATA and never a `diverges` bool";
    }
}

// ── macros surface (preprocessor-macro; D-PP-DESCRIPTOR-MACRO-INJECT) ─────────

// Function-like (assert), object-like (no params), and variadic forms all parse;
// `params` ABSENT distinguishes object-like from a zero-param function-like.
TEST(ShippedLibDescriptor, MacrosSurfaceParsedAllForms) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "m.json", R"JSON({
        "header": "m.h",
        "macros": [
            { "name": "assert", "params": ["e"], "replacement": "((void)0)" },
            { "name": "TRUE", "replacement": "1" },
            { "name": "LOG", "params": ["fmt"], "variadic": true, "replacement": "do{}while(0)" }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->macros.size(), 3u);
    EXPECT_EQ(desc->macros[0].name, "assert");
    ASSERT_TRUE(desc->macros[0].params.has_value());
    ASSERT_EQ(desc->macros[0].params->size(), 1u);
    EXPECT_EQ(desc->macros[0].params->at(0), "e");
    EXPECT_EQ(desc->macros[0].replacement, "((void)0)");
    EXPECT_FALSE(desc->macros[0].variadic);
    EXPECT_EQ(desc->macros[1].name, "TRUE");
    EXPECT_FALSE(desc->macros[1].params.has_value());   // object-like
    EXPECT_EQ(desc->macros[1].replacement, "1");
    EXPECT_EQ(desc->macros[2].name, "LOG");
    ASSERT_TRUE(desc->macros[2].params.has_value());
    EXPECT_TRUE(desc->macros[2].variadic);
}

// A macros-ONLY descriptor is VALID — the ≥1-surface check counts macros (the
// assert.h shape). RED-ON-DISABLE: without `&& out.macros.empty()` in the check,
// this would fail-loud as "declares nothing".
TEST(ShippedLibDescriptor, MacrosOnlyDescriptorIsValid) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "mo.json", R"({
        "header": "mo.h", "macros": [ { "name": "X", "replacement": "" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->macros.size(), 1u);
    EXPECT_TRUE(desc->macros[0].replacement.empty());   // null macro `#define X`
}

TEST(ShippedLibDescriptor, MacroMissingNameFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad.json",
        R"({ "header": "b.h", "macros": [ { "replacement": "1" } ] })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

TEST(ShippedLibDescriptor, MacroVariadicWithoutParamsFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad2.json",
        R"({ "header": "b.h", "macros": [ { "name": "X", "variadic": true } ] })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// A newline in a macro field would break the spliced `#define ... \n` directive
// (terminating it early + leaking the remainder as source) — reject fail-loud.
// RED-ON-DISABLE: without the field-shape gate the embedded `\n` slips through to
// the preprocessor and silently corrupts the synth buffer.
TEST(ShippedLibDescriptor, MacroFieldWithNewlineFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad3.json",
        "{ \"header\": \"b.h\", \"macros\": [ "
        "{ \"name\": \"X\", \"replacement\": \"1\\nint leaked=99;\" } ] }");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// readShippedLibMacros: interner-FREE (the preprocessor's path — it has no
// TypeInterner). Decodes the macros without symbols/constants/typedefs.
TEST(ShippedLibDescriptor, ReadShippedLibMacrosInternerFree) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "assert.json", R"JSON({
        "header": "assert.h",
        "macros": [ { "name": "assert", "params": ["e"], "replacement": "((void)0)" } ]
    })JSON");
    DiagnosticReporter rep;
    auto macros = readShippedLibMacros(path, rep);   // NO interner / typeReg
    ASSERT_TRUE(macros.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(macros->size(), 1u);
    EXPECT_EQ(macros->at(0).name, "assert");
    ASSERT_TRUE(macros->at(0).params.has_value());
    EXPECT_EQ(macros->at(0).replacement, "((void)0)");
}

// readShippedLibMacros on a TYPED-only descriptor returns an EMPTY vector (NOT
// nullopt) — the preprocessor injects nothing for stdint/stddef-style headers.
TEST(ShippedLibDescriptor, ReadShippedLibMacrosEmptyForTypedOnly) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "size.json", R"({
        "header": "size.h", "typedefs": [ { "name": "size_t", "type": "u64" } ]
    })");
    DiagnosticReporter rep;
    auto macros = readShippedLibMacros(path, rep);
    ASSERT_TRUE(macros.has_value());
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(macros->empty());
}

// readShippedLibMacros is NO STRICTER than the semantic read: a HEADER-LESS
// descriptor (the `header` provenance gate is the SEMANTIC read's job, NOT the
// macros-only read's) reads its macros WITHOUT a new error. RED-ON-DISABLE: a
// `header` gate here re-breaks the angle-include preprocess path for any
// symbols-only descriptor — exactly the ImportResolver regression this guards
// (CSubsetAngleIncludeResolvesToDescriptorOnSystemDir uses a header-less api.json
// that the preprocessor now reads for macros while splicing).
TEST(ShippedLibDescriptor, ReadShippedLibMacrosHeaderlessIsLenient) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "api.json", R"JSON({
        "library": { "pe": "lib.dll" },
        "symbols": [ { "name": "use", "signature": "fn() -> i32" } ]
    })JSON");
    DiagnosticReporter rep;
    auto macros = readShippedLibMacros(path, rep);
    ASSERT_TRUE(macros.has_value());   // NOT nullopt — header absence is not an error
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(macros->empty());      // no `macros` key -> nothing injected
}

// ── D-FFI-DESCRIPTOR-INCLUDES: the transitive shipped-header `#include` graph ──
//
// A descriptor may declare `"includes": ["stdio.h"]` — the sibling headers it
// transitively `#include`s. `readShippedLibDescriptor` populates `.includes`, the
// interner-free `readShippedLibIncludes` returns the same (lock-step), and
// `forEachDescriptorInClosure` walks the closure cycle-safe.

// (a) `includes` decodes on BOTH the interned full read AND the interner-free read.
// RED-ON-DISABLE: drop the `decodeShippedIncludes` call in readShippedLibDescriptor
// (or the readShippedLibIncludes impl) → `.includes` empty / nullopt.
TEST(ShippedLibDescriptor, IncludesSurfaceDecodes) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "parent.json", R"JSON({
        "header": "parent.h",
        "includes": ["stdio.h", "sys/uio.h"],
        "symbols": [ { "name": "pfn", "signature": "fn() -> i32" } ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->includes.size(), 2u);
    EXPECT_EQ(desc->includes[0], "stdio.h");
    EXPECT_EQ(desc->includes[1], "sys/uio.h");
    // Interner-FREE read returns the identical list (the walker's source).
    DiagnosticReporter rep2;
    auto inc = readShippedLibIncludes(path, rep2);
    ASSERT_TRUE(inc.has_value());
    EXPECT_FALSE(rep2.hasErrors());
    ASSERT_EQ(inc->size(), 2u);
    EXPECT_EQ(inc->at(0), "stdio.h");
    EXPECT_EQ(inc->at(1), "sys/uio.h");
}

// Absent `includes` ⇒ empty (back-compat — every existing descriptor untouched).
TEST(ShippedLibDescriptor, IncludesAbsentIsEmpty) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "noinc.json", R"JSON({
        "header": "noinc.h", "symbols": [ { "name": "f", "signature": "fn() -> i32" } ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_TRUE(desc->includes.empty());
    DiagnosticReporter rep2;
    auto inc = readShippedLibIncludes(path, rep2);
    ASSERT_TRUE(inc.has_value());
    EXPECT_TRUE(inc->empty());
}

// (a') A malformed `includes` field FAILS LOUD (F_ShippedLibDescriptorMalformed) on
// BOTH reads — a non-array shape AND a non-string / empty entry. RED-ON-DISABLE:
// drop the shape validation in decodeShippedIncludes → the malformed field is
// silently accepted.
TEST(ShippedLibDescriptor, IncludesMalformedFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    // Non-array.
    {
        auto const path = writeTemp(dir, "badinc1.json", R"JSON({
            "header": "b.h", "includes": "stdio.h",
            "symbols": [ { "name": "f", "signature": "fn() -> i32" } ]
        })JSON");
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
        EXPECT_FALSE(desc.has_value());
        EXPECT_GT(dss::test_support::countCode(
                      rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
        // The interner-free read stays in lock-step (same code, nullopt).
        DiagnosticReporter rep2;
        auto inc = readShippedLibIncludes(path, rep2);
        EXPECT_FALSE(inc.has_value());
        EXPECT_GT(dss::test_support::countCode(
                      rep2, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
    }
    // Non-string entry.
    {
        auto const path = writeTemp(dir, "badinc2.json", R"JSON({
            "header": "b.h", "includes": [123],
            "symbols": [ { "name": "f", "signature": "fn() -> i32" } ]
        })JSON");
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
        EXPECT_FALSE(desc.has_value());
        EXPECT_GT(dss::test_support::countCode(
                      rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
    }
}

// (b) ★ CYCLE SAFETY (the correctness must): two temp descriptors a.json(includes:[b])
// + b.json(includes:[a]) — a CYCLE. forEachDescriptorInClosure must visit each
// EXACTLY ONCE and TERMINATE (a broken visited-set would infinite-loop / OOM).
TEST(ShippedLibDescriptor, ClosureCycleTerminates) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const aPath = writeTemp(dir, "a.json", R"JSON({
        "header": "a.h", "includes": ["b.h"],
        "symbols": [ { "name": "af", "signature": "fn() -> i32" } ]
    })JSON");
    (void)writeTemp(dir, "b.json", R"JSON({
        "header": "b.h", "includes": ["a.h"],
        "symbols": [ { "name": "bf", "signature": "fn() -> i32" } ]
    })JSON");
    std::vector<fs::path> const systemDirs{dir.path()};
    std::unordered_set<std::string> visited;
    std::vector<std::string> order;
    std::vector<std::string> unresolved;
    forEachDescriptorInClosure(
        aPath, systemDirs, kDefaultHeaderNameMatching, visited,
        [&](fs::path const& p) { order.push_back(p.stem().string()); },
        [&](std::string const& h, HeaderSearchResult const&) {
            unresolved.push_back(h);
        });
    // Terminated (we got here) + each descriptor visited exactly once.
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "a");   // parent FIRST (the start)
    EXPECT_EQ(order[1], "b");   // then its include
    EXPECT_TRUE(unresolved.empty());
}

// (b') DIAMOND (a→b, a→c, b→d, c→d): the shared leaf `d` is visited ONCE, and every
// descriptor is visited parent-before-child.
TEST(ShippedLibDescriptor, ClosureDiamondVisitsSharedLeafOnce) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const aPath = writeTemp(dir, "da.json", R"JSON({
        "header": "da.h", "includes": ["db.h", "dc.h"],
        "symbols": [ { "name": "af", "signature": "fn() -> i32" } ]
    })JSON");
    (void)writeTemp(dir, "db.json", R"JSON({
        "header": "db.h", "includes": ["dd.h"],
        "symbols": [ { "name": "bf", "signature": "fn() -> i32" } ]
    })JSON");
    (void)writeTemp(dir, "dc.json", R"JSON({
        "header": "dc.h", "includes": ["dd.h"],
        "symbols": [ { "name": "cf", "signature": "fn() -> i32" } ]
    })JSON");
    (void)writeTemp(dir, "dd.json", R"JSON({
        "header": "dd.h", "symbols": [ { "name": "df", "signature": "fn() -> i32" } ]
    })JSON");
    std::vector<fs::path> const systemDirs{dir.path()};
    std::unordered_set<std::string> visited;
    std::vector<std::string> order;
    forEachDescriptorInClosure(
        aPath, systemDirs, kDefaultHeaderNameMatching, visited,
        [&](fs::path const& p) { order.push_back(p.stem().string()); },
        [&](std::string const&, HeaderSearchResult const&) {});
    ASSERT_EQ(order.size(), 4u);        // each of a/b/c/d visited exactly once
    EXPECT_EQ(order[0], "da");          // parent first (the DFS root)
    // The shared leaf `dd` appears exactly once (the diamond dedup — this is the
    // load-bearing property; a broken visited-set would visit it twice).
    EXPECT_EQ(std::count(order.begin(), order.end(), std::string{"dd"}), 1);
    // Parent-before-child holds for every TRAVERSED edge (a DFS: `dd` is reached
    // through `db`'s descent, so `dd` is visited BEFORE `dc` even starts — the
    // `dc→dd` edge is pruned by the visited-set, NOT re-traversed. So the honest
    // invariants are: da precedes every other node, and db precedes dd).
    auto idx = [&](std::string const& s) {
        return std::find(order.begin(), order.end(), s) - order.begin();
    };
    EXPECT_LT(idx("da"), idx("db"));
    EXPECT_LT(idx("da"), idx("dc"));
    EXPECT_LT(idx("da"), idx("dd"));
    EXPECT_LT(idx("db"), idx("dd"));   // dd is db's child, reached first via db
}

// (c) FAIL-LOUD: an `includes` entry that resolves to NO descriptor on systemDirs
// invokes `onUnresolvedInclude` with the offending header name (the import resolver
// turns this into a positioned F_ShippedHeaderNotFound). RED-ON-DISABLE: drop the
// `if (!childPath) onUnresolvedInclude(...)` arm → a typo'd include is silently
// swallowed.
TEST(ShippedLibDescriptor, ClosureUnresolvedIncludeIsReported) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "typo.json", R"JSON({
        "header": "typo.h", "includes": ["stdioo.h"],
        "symbols": [ { "name": "f", "signature": "fn() -> i32" } ]
    })JSON");
    std::vector<fs::path> const systemDirs{dir.path()};
    std::unordered_set<std::string> visited;
    std::vector<std::string> order;
    std::vector<std::string> unresolved;
    forEachDescriptorInClosure(
        path, systemDirs, kDefaultHeaderNameMatching, visited,
        [&](fs::path const& p) { order.push_back(p.stem().string()); },
        [&](std::string const& h, HeaderSearchResult const&) {
            unresolved.push_back(h);
        });
    ASSERT_EQ(order.size(), 1u);        // only the parent (the typo has no descriptor)
    EXPECT_EQ(order[0], "typo");
    ASSERT_EQ(unresolved.size(), 1u);   // the unresolvable entry was surfaced
    EXPECT_EQ(unresolved[0], "stdioo.h");
}

// ── structs surface (named-field aggregate; the struct-body mechanism) ────────

// A `structs` entry decodes into a ShippedStruct with named fields + an interned
// struct TypeId (name + positional field types). Field types decode via the one
// type-text codec (i64 here).
TEST(ShippedLibDescriptor, StructsSurfaceParsed) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "s.json", R"JSON({
        "header": "s.h",
        "structs": [
            { "name": "timeval", "fields": [
                { "name": "tv_sec",  "type": "i64" },
                { "name": "tv_usec", "type": "i64" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->structs.size(), 1u);
    EXPECT_EQ(desc->structs[0].name, "timeval");
    ASSERT_EQ(desc->structs[0].fields.size(), 2u);
    EXPECT_EQ(desc->structs[0].fields[0].name, "tv_sec");
    EXPECT_EQ(desc->structs[0].fields[1].name, "tv_usec");
    EXPECT_EQ(desc->structs[0].fields[0].type, interner.primitive(TypeKind::I64));
    EXPECT_TRUE(desc->structs[0].typeId.valid());
}

// A structs-ONLY descriptor is VALID — the ≥1-surface check counts structs.
// RED-ON-DISABLE: without `&& out.structs.empty()` in the check, this fails-loud
// as "declares nothing".
TEST(ShippedLibDescriptor, StructsOnlyDescriptorIsValid) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "so.json", R"JSON({
        "header": "so.h",
        "structs": [ { "name": "pt", "fields": [ { "name": "x", "type": "i32" } ] } ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->structs.size(), 1u);
}

TEST(ShippedLibDescriptor, StructMissingFieldsFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad.json",
        R"({ "header": "b.h", "structs": [ { "name": "empty", "fields": [] } ] })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

TEST(ShippedLibDescriptor, StructFieldBadTypeFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad2.json",
        R"({ "header": "b.h", "structs": [ { "name": "s",
             "fields": [ { "name": "f", "type": "not_a_type" } ] } ] })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// Duplicate field names fail loud — a last-writer-wins scope binding would
// silently lose a field slot (a wrong-but-runs aggregate). RED-ON-DISABLE: the
// two `f` fields decode fine individually; only the dup guard rejects them.
TEST(ShippedLibDescriptor, StructDuplicateFieldNameFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "dupf.json",
        R"({ "header": "b.h", "structs": [ { "name": "s", "fields": [
             { "name": "f", "type": "i32" }, { "name": "f", "type": "i64" } ] } ] })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// ── unions surface (named-member union; the union-body mechanism, C34b) ───────
//
// A `unions` entry decodes into a ShippedUnion with named members + an interned
// UNION TypeId (TypeKind::Union — every member overlaid at offset 0). The SIBLING
// of `structs`: member names live HERE because the hir-text `union "N" { T,… }`
// spelling carries member TYPES positionally but no names.
// (D-FFI-DESCRIPTOR-UNION-MEMBER-INJECTION)
TEST(ShippedLibDescriptor, UnionsSurfaceParsed) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "u.json", R"JSON({
        "header": "u.h",
        "unions": [
            { "name": "keyU", "fields": [
                { "name": "oneWordValue", "type": "ptr<void>" },
                { "name": "string",       "type": "ptr<char>" },
                { "name": "words",        "type": "arr<i32, 1>" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->unions.size(), 1u);
    EXPECT_EQ(desc->unions[0].name, "keyU");
    ASSERT_EQ(desc->unions[0].fields.size(), 3u);
    EXPECT_EQ(desc->unions[0].fields[0].name, "oneWordValue");
    EXPECT_EQ(desc->unions[0].fields[1].name, "string");
    EXPECT_EQ(desc->unions[0].fields[2].name, "words");
    ASSERT_TRUE(desc->unions[0].typeId.valid());
    // The interned type is a UNION (member-overlay semantics), not a struct.
    EXPECT_EQ(interner.kind(desc->unions[0].typeId), TypeKind::Union);
    auto const ops = interner.operands(desc->unions[0].typeId);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(interner.kind(ops[0]), TypeKind::Ptr);   // ptr<void>
}

// A unions-ONLY descriptor is VALID — the ≥1-surface check counts unions.
// RED-ON-DISABLE: without `out.unions.empty()` (+ !declaredUnions) in the gate,
// this fails-loud as "declares nothing".
TEST(ShippedLibDescriptor, UnionsOnlyDescriptorIsValid) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "uo.json", R"JSON({
        "header": "uo.h",
        "unions": [ { "name": "u", "fields": [ { "name": "a", "type": "i32" } ] } ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->unions.size(), 1u);
}

// A union member must NOT carry an explicit `offset` — every member overlays at 0
// by union semantics (an explicit-offset overlapping layout is the c107 `structs`
// channel). RED-ON-DISABLE: drop the offset-reject loop and the `@4` member is
// silently accepted (then laid out at 0 anyway — a wrong-but-runs surface).
TEST(ShippedLibDescriptor, UnionMemberOffsetFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "ubad.json", R"JSON({
        "header": "ub.h",
        "unions": [ { "name": "u", "fields": [
            { "name": "a", "type": "i32" },
            { "name": "b", "type": "i32", "offset": 4 } ] } ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// A union member with an undecodable type fails loud (the F_ShippedLibUnsupportedType
// path, shared with structs via decodeStructFieldList).
TEST(ShippedLibDescriptor, UnionMemberBadTypeFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "ubt.json",
        R"({ "header": "ub.h", "unions": [ { "name": "u",
             "fields": [ { "name": "a", "type": "not_a_type" } ] } ] })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// Option C: a `unions` entry PUBLISHES its name so a later `structs` FIELD spells
// the union BY NAME (`Entry.key : "keyU"`) — the field resolves to the SAME
// interned union TypeId (so a member scope on that union resolves `entry.key.member`).
// RED-ON-DISABLE: drop the mergedNamedTypes publish in the unions decode and the
// struct field's `keyU` fails to decode (F_ShippedLibUnsupportedType).
TEST(ShippedLibDescriptor, UnionReferencedByNameInStructField) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "uref.json", R"JSON({
        "header": "ur.h",
        "unions": [
            { "name": "keyU", "fields": [
                { "name": "oneWordValue", "type": "ptr<void>" },
                { "name": "string",       "type": "ptr<char>" } ] }
        ],
        "structs": [
            { "name": "Entry", "fields": [
                { "name": "clientData", "type": "ptr<void>" },
                { "name": "key",        "type": "keyU" } ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->unions.size(), 1u);
    ASSERT_EQ(desc->structs.size(), 1u);
    ASSERT_EQ(desc->structs[0].fields.size(), 2u);
    EXPECT_EQ(desc->structs[0].fields[1].name, "key");
    EXPECT_EQ(desc->structs[0].fields[1].type, desc->unions[0].typeId)
        << "the by-name union field must intern to the same union TypeId";
    EXPECT_EQ(interner.kind(desc->structs[0].fields[1].type), TypeKind::Union);
}

// ── BY-NAME composite struct FIELDS (D-CSUBSET-DARWIN-BSD-STRUCT-BY-NAME) ────
//
// The `structs` loop PUBLISHES each injected struct's tag name (the typedef /
// union Option-C mirror), so a LATER entry may spell an EARLIER one as a field
// type BY NAME instead of restating its body — the whole point being that an
// inner struct's per-format widths then live in exactly ONE place.

// IDENTITY, not merely same-shape: the by-name field must resolve to the very
// TypeId the referenced entry interned, because injection is first-wins BY NAME
// and only the winner carries a member field scope — a second, equal-looking
// type would strand `outer.inner.member`.
// RED-ON-DISABLE: delete the `mergedNamedTypes` publish in the structs decode →
// `Inner` no longer resolves as a field type → F_ShippedLibUnsupportedType.
TEST(ShippedLibDescriptor, StructReferencedByNameInLaterStructField) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "sref.json", R"JSON({
        "header": "sr.h",
        "structs": [
            { "name": "Inner", "fields": [
                { "name": "a", "type": "i64" },
                { "name": "b", "type": "i32" } ] },
            { "name": "Outer", "fields": [
                { "name": "first",  "type": "Inner" },
                { "name": "second", "type": "Inner" } ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->structs.size(), 2u);
    EXPECT_EQ(desc->structs[0].name, "Inner");
    EXPECT_EQ(desc->structs[1].name, "Outer");
    ASSERT_EQ(desc->structs[1].fields.size(), 2u);
    EXPECT_EQ(desc->structs[1].fields[0].type, desc->structs[0].typeId)
        << "the by-name struct field must be the IDENTICAL interned TypeId";
    EXPECT_EQ(desc->structs[1].fields[1].type, desc->structs[0].typeId);
    EXPECT_EQ(interner.kind(desc->structs[1].fields[0].type), TypeKind::Struct);
    // And the embedded composite lays out by value (16 = i64 + i32 + tail pad).
    auto inner = computeLayout(desc->structs[0].typeId, interner,
                               AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                               DataModel::Lp64);
    auto outer = computeLayout(desc->structs[1].typeId, interner,
                               AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                               DataModel::Lp64);
    ASSERT_TRUE(inner.has_value());
    ASSERT_TRUE(outer.has_value());
    EXPECT_EQ(inner->size, 16u);
    EXPECT_EQ(outer->size, 32u);
    ASSERT_EQ(outer->fieldOffsets.size(), 2u);
    EXPECT_EQ(outer->fieldOffsets[0], 0u);
    EXPECT_EQ(outer->fieldOffsets[1], 16u);
}

// SOURCE ORDER IS THE DEPENDENCY ORDER, and a violation is LOUD. `Outer` names
// `Inner` BEFORE `Inner` is declared: publication happens as the loop walks, so
// the name is unresolved there and the read FAILS — never a silently empty or
// half-built composite. The same path catches a plain typo (a name declared
// nowhere), which is why the message must quote the offending name.
TEST(ShippedLibDescriptor, ForwardStructNameInStructFieldFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "fwd.json", R"JSON({
        "header": "fw.h",
        "structs": [
            { "name": "Outer", "fields": [ { "name": "first", "type": "Inner" } ] },
            { "name": "Inner", "fields": [ { "name": "a", "type": "i64" } ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedLibUnsupportedType), 1u);
    EXPECT_TRUE(anyDiagMentions(rep, "unknown type 'Inner'"))
        << "the type decoder must NAME the unresolved type";
    EXPECT_TRUE(anyDiagMentions(rep, "field type 'Inner' failed to decode"))
        << "and the descriptor reader must say WHICH field carried it";
}

// The DEPENDENCY GATE. `Inner` carries elf-only `variants`, so on any other
// target ZERO match and it is not injected — and `Outer`, which embeds one BY
// NAME, cannot exist there either. The reader SKIPS the dependent exactly as it
// skips the dependency; it must NOT fail the read, because every shipped
// descriptor is read on EVERY format (the all-descriptor sweeps + the nullopt
// direct-API/LSP path), and one unavailable inner struct would otherwise take a
// whole header down. Fail-loud is untouched: only a name declared EARLIER in
// THIS descriptor can suppress an entry (the forward/typo pin above still red).
TEST(ShippedLibDescriptor, StructByNameUnselectedDependencySkipsDependent) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "dep.json", R"JSON({
        "header": "dp.h",
        "structs": [
            { "name": "Inner", "variants": [
                { "when": { "format": "elf" },
                  "fields": [ { "name": "a", "type": "i64" } ] } ] },
            { "name": "Outer", "fields": [ { "name": "first", "type": "Inner" } ] }
        ]
    })JSON");

    auto readFor = [&](std::optional<ObjectFormatKind> fmt) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64,
                                             std::string_view{"x86_64"}, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        std::vector<std::string> names;
        if (desc)
            for (auto const& s : desc->structs) names.push_back(s.name);
        return names;
    };

    EXPECT_EQ(readFor(ObjectFormatKind::Elf),
              (std::vector<std::string>{"Inner", "Outer"}))
        << "elf selects Inner's variant, so the by-name dependent lands too";
    EXPECT_TRUE(readFor(ObjectFormatKind::Pe).empty())
        << "no Inner variant matches pe → Outer is unavailable there, and the "
           "read must still be CLEAN (not a hard error)";
    EXPECT_TRUE(readFor(std::nullopt).empty())
        << "the unknown-format read selects no variant at all — same verdict";
}

// ── per-target struct VARIANTS (per-target byte layout; plan 25) ─────────────
//
// A `structs` entry may declare per-target `variants` (each `when:{arch?,format?}`
// + its own field list) INSTEAD of a flat `fields`, so a struct can carry the
// correct per-target byte layout. The decoder selects the variant matching the
// active (arch, format); the injection + layout engine are UNCHANGED. The CRUX
// (gate 2 below pins it): x86_64/arm64 AggregateLayoutParams are byte-identical and
// `computeLayout` is param-driven, so the per-target offset delta comes ENTIRELY
// from the selected FIELD LIST.

namespace {
// The shipped-target aggregate-layout params (natural alignment, 16-byte ISA cap),
// LP64 — the layout context the selection pins assert offsets under. Both shipped
// ELF arches feed these identical params (gate 2 proves it from the real schemas).
constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

// A 2-variant descriptor whose SAME named field `x` (i64) sits at a different byte
// offset per arch: variant "archA" has a leading i32 pad → x@8; variant "archB"
// has x alone → x@0. The format is the same (elf) for both, so only the arch
// selects. `objectFormat` "elf".
[[nodiscard]] std::string twoVariantDescriptor() {
    return R"JSON({
        "header": "v.h",
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "archA", "format": "elf" },
                  "fields": [ { "name": "pad", "type": "i32" }, { "name": "x", "type": "i64" } ] },
                { "when": { "arch": "archB", "format": "elf" },
                  "fields": [ { "name": "x", "type": "i64" } ] }
            ] }
        ]
    })JSON";
}
} // namespace

// SELECTION PIN (gate 5; closure gates 1/5). Decoding with arch=archA selects the
// padded variant → `x`@8; with arch=archB → `x`@0. The offset is read from
// `computeLayout` on the interned struct type — the SAME engine MIR uses. This is
// the per-target layout proof: the ONLY difference between the two decodes is the
// selected field list, and that flips `x`'s offset 8 → 0.
// RED-ON-DISABLE: neuter the selector to always take variant 0 (the `matchCount`
// machinery → "use variants[0]") and the archB assertion (x@0) fails — archB would
// see the padded layout (x@8).
TEST(ShippedLibDescriptor, StructVariantSelectsPerArchLayout) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "var.json", twoVariantDescriptor());

    auto offsetOfX = [&](std::string_view arch) -> std::uint64_t {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, arch, ObjectFormatKind::Elf);
        EXPECT_TRUE(desc.has_value()) << "arch=" << arch;
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->structs.size(), 1u) << "arch=" << arch;
        auto layout = computeLayout(desc->structs[0].typeId, interner, kNatural16,
                                    DataModel::Lp64);
        EXPECT_TRUE(layout.has_value());
        // `x` is the LAST field in both variants (index 1 for archA, index 0 for archB).
        return layout->fieldOffsets.back();
    };

    EXPECT_EQ(offsetOfX("archA"), 8u);   // i32 pad@0, pad[4..7], x@8
    EXPECT_EQ(offsetOfX("archB"), 0u);   // x@0 (no pad)
}

// REAL `struct stat` per-arch LAYOUT pin (plan 25, gate 6 LOCAL proof). The
// shipped sys/stat.json `variants` (the glibc x86-64-linux 144-byte layout vs
// the arm64-linux 128-byte layout) are runtime-witnessed by the
// shipped_struct_stat_{x86,arm64} corpus on the linux CI leg; THIS pins the
// exact per-arch sizeof AND a DIVERGENT field offset (st_mode @24 on x86-64,
// @16 on arm64 — the data-corruption case the mechanism exists to prevent)
// LOCALLY, so a layout-authoring slip is caught here, not only on CI. The field
// lists are the shipped sys/stat.json variants verbatim (timespec flattened to
// _sec/_nsec i64 pairs — bit-identical layout). RED-ON-DISABLE: neuter the
// selector → both arches see the 144-byte x86-64 layout → the arm64 asserts fail.
TEST(ShippedLibDescriptor, RealSysStatPerArchLayoutSizesAndOffsets) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "stat.json", R"JSON({
        "header": "sys/stat.h",
        "structs": [
            { "name": "stat", "variants": [
              { "when": {"arch":"x86_64","format":"elf"}, "fields": [
                {"name":"st_dev","type":"u64"},{"name":"st_ino","type":"u64"},
                {"name":"st_nlink","type":"u64"},{"name":"st_mode","type":"u32"},
                {"name":"st_uid","type":"u32"},{"name":"st_gid","type":"u32"},
                {"name":"__pad0","type":"i32"},{"name":"st_rdev","type":"u64"},
                {"name":"st_size","type":"i64"},{"name":"st_blksize","type":"i64"},
                {"name":"st_blocks","type":"i64"},
                {"name":"st_atim_sec","type":"i64"},{"name":"st_atim_nsec","type":"i64"},
                {"name":"st_mtim_sec","type":"i64"},{"name":"st_mtim_nsec","type":"i64"},
                {"name":"st_ctim_sec","type":"i64"},{"name":"st_ctim_nsec","type":"i64"},
                {"name":"r0","type":"i64"},{"name":"r1","type":"i64"},{"name":"r2","type":"i64"}
              ] },
              { "when": {"arch":"arm64","format":"elf"}, "fields": [
                {"name":"st_dev","type":"u64"},{"name":"st_ino","type":"u64"},
                {"name":"st_mode","type":"u32"},{"name":"st_nlink","type":"u32"},
                {"name":"st_uid","type":"u32"},{"name":"st_gid","type":"u32"},
                {"name":"st_rdev","type":"u64"},{"name":"__pad1","type":"u64"},
                {"name":"st_size","type":"i64"},{"name":"st_blksize","type":"i32"},
                {"name":"__pad2","type":"i32"},{"name":"st_blocks","type":"i64"},
                {"name":"st_atim_sec","type":"i64"},{"name":"st_atim_nsec","type":"i64"},
                {"name":"st_mtim_sec","type":"i64"},{"name":"st_mtim_nsec","type":"i64"},
                {"name":"st_ctim_sec","type":"i64"},{"name":"st_ctim_nsec","type":"i64"},
                {"name":"r0","type":"i32"},{"name":"r1","type":"i32"}
              ] }
            ] }
        ]
    })JSON");

    auto layoutFor = [&](std::string_view arch) -> std::optional<StructLayout> {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, arch, ObjectFormatKind::Elf);
        EXPECT_TRUE(desc.has_value()) << "arch=" << arch;
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->structs.size(), 1u) << "arch=" << arch;
        return computeLayout(desc->structs[0].typeId, interner, kNatural16, DataModel::Lp64);
    };

    auto x86 = layoutFor("x86_64");
    auto arm = layoutFor("arm64");
    ASSERT_TRUE(x86.has_value());
    ASSERT_TRUE(arm.has_value());
    EXPECT_EQ(x86->size, 144u);            // glibc x86-64-linux struct stat
    EXPECT_EQ(arm->size, 128u);            // glibc arm64-linux struct stat (DIVERGES)
    EXPECT_EQ(x86->fieldOffsets[3], 24u);  // st_mode (index 3 on x86-64) @ 24
    EXPECT_EQ(arm->fieldOffsets[2], 16u);  // st_mode (index 2 on arm64) @ 16
}

// AMBIGUOUS-MATCH PIN (gate 3; closure gate 3, F1). Two variants BOTH matching the
// active (arch,format) → the read FAILS LOUD with F_ShippedStructVariantAmbiguous,
// never silently "first wins". Here both `when`s are {arch:dup, format:elf}.
TEST(ShippedLibDescriptor, StructVariantAmbiguousMatchFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "amb.json", R"JSON({
        "header": "a.h",
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "dup", "format": "elf" },
                  "fields": [ { "name": "x", "type": "i32" } ] },
                { "when": { "arch": "dup", "format": "elf" },
                  "fields": [ { "name": "y", "type": "i64" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"dup"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedStructVariantAmbiguous),
              1u);
}

// EAGER-DECODE PIN (gate 4; closure gate 4, F2). A NON-active variant carries an
// undecodable field type. Even though we compile for the OTHER (active) target —
// whose variant is well-formed — the read FAILS LOUD: every variant's field list
// is decoded at read time, so a malformed inactive variant never lurks until its
// target's first compile (mirrors signatureByDataModel).
TEST(ShippedLibDescriptor, StructVariantEagerDecodeMalformedInactiveFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "eager.json", R"JSON({
        "header": "e.h",
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "active", "format": "elf" },
                  "fields": [ { "name": "x", "type": "i64" } ] },
                { "when": { "arch": "other", "format": "elf" },
                  "fields": [ { "name": "y", "type": "not_a_type" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    // Compile for arch="active" (its variant decodes fine); the INACTIVE "other"
    // variant's bad type still fails the whole read.
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"active"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedLibUnsupportedType),
              1u);
}

// NO-MATCH → NOT INJECTED (gate 7; closure gate 7). Variants present but NONE match
// the active target → the struct is simply not injected (no `S` in `desc->structs`).
// A c-subset program referencing `struct S` would then emit S_UnknownType (the same
// behavior as any undeclared struct) — never a silent wrong layout. The read itself
// SUCCEEDS (no error): a header that doesn't define a struct for this target is not
// an error here; the absence becomes loud at the USE site.
TEST(ShippedLibDescriptor, StructVariantNoMatchNotInjected) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    // Variant requires arch="only"; we compile for arch="elsewhere" → no match. The
    // descriptor also carries a typedef so the "declares something" gate passes even
    // with the struct dropped.
    auto const path = writeTemp(dir, "nomatch.json", R"JSON({
        "header": "n.h",
        "typedefs": [ { "name": "t", "type": "i32" } ],
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "only", "format": "elf" },
                  "fields": [ { "name": "x", "type": "i64" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"elsewhere"},
                                         ObjectFormatKind::Elf);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->structs.empty()) << "no variant matched → struct not injected";
}

// NO SELECTOR (activeTarget nullopt) + variants present → not injected. The
// direct-API/LSP/test path (no target in scope) cannot select a variant, so a
// variants-only struct is absent — never an arbitrary pick. (Closure gate 8
// back-compat for the nullopt caller, variant arm.)
TEST(ShippedLibDescriptor, StructVariantNoActiveTargetNotInjected) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "nosel.json", R"JSON({
        "header": "ns.h",
        "typedefs": [ { "name": "t", "type": "i32" } ],
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "archA", "format": "elf" },
                  "fields": [ { "name": "x", "type": "i64" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    // Default activeTarget=nullopt, activeFormat=nullopt (the direct-API caller).
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->structs.empty())
        << "no active target → variants-only struct not injected (never arbitrary)";
}

// Plan 25 declares-something fix: a descriptor whose ONLY surface is per-target
// `variants` structs (the REAL <sys/stat.h> shape — no symbols/typedefs/etc.)
// must DECODE CLEANLY under the nullopt direct-API / LSP / AllShippedDescriptors-
// Decode-provenance path. It DECLARES a struct surface (target-conditional), so
// it is NOT a false "declares nothing" even though no struct injects without a
// target. RED-ON-DISABLE: drop the `declaredStructs` term from the
// declares-something check (shipped_lib_descriptor.cpp) → this read fails loud.
TEST(ShippedLibDescriptor, StructVariantsOnlyDescriptorValidUnderNullopt) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "vonly.json", R"JSON({
        "header": "vonly.h",
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "x86_64", "format": "elf" },
                  "fields": [ { "name": "x", "type": "i64" } ] },
                { "when": { "arch": "arm64", "format": "elf" },
                  "fields": [ { "name": "x", "type": "i64" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);  // nullopt target
    ASSERT_TRUE(desc.has_value());        // declares a struct surface → NOT a no-op
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->structs.empty());   // no target → nothing injected
}

// Match-ALL-SPECIFIED (F1): a variant whose `when` specifies ONLY `arch` (no
// `format`) matches on that arch under ANY format. Here the lone variant is
// `when:{arch:"a"}`; compiling arch="a" format=macho selects it (format unspecified
// ⇒ unconstrained). This proves "every SPECIFIED key must match" — an unspecified
// key is a wildcard. (The danger case — an under-specified `when` matching two
// targets — is the ambiguity pin above; this is the legitimate single-match form.)
TEST(ShippedLibDescriptor, StructVariantWhenArchOnlyMatchesAnyFormat) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "archonly.json", R"JSON({
        "header": "ao.h",
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "a" },
                  "fields": [ { "name": "x", "type": "i32" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"a"},
                                         ObjectFormatKind::MachO);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->structs.size(), 1u);
    EXPECT_EQ(desc->structs[0].name, "S");
}

// A struct entry declaring BOTH `fields` and `variants` is malformed (ambiguous
// intent) → fail loud. RED-ON-DISABLE: each surface decodes fine alone; only the
// XOR gate rejects the pair.
TEST(ShippedLibDescriptor, StructBothFieldsAndVariantsFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "both.json", R"JSON({
        "header": "b.h",
        "structs": [
            { "name": "S",
              "fields": [ { "name": "x", "type": "i32" } ],
              "variants": [ { "when": { "arch": "a" },
                              "fields": [ { "name": "x", "type": "i32" } ] } ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"a"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// An unknown `when` key fails loud against the closed vocabulary {arch,format}
// (rejectUnknownKeys). A typo'd key (e.g. "ach") would otherwise be silently
// ignored → the variant matches more broadly than intended.
TEST(ShippedLibDescriptor, StructVariantUnknownWhenKeyFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "badwhen.json", R"JSON({
        "header": "bw.h",
        "structs": [
            { "name": "S", "variants": [
                { "when": { "ach": "x86_64" },
                  "fields": [ { "name": "x", "type": "i32" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// An unknown `when.format` value fails loud against the closed object-format
// vocabulary (objectFormatKindFromName) — a typo'd "elff" would otherwise NEVER
// match → the struct silently vanishes on every target.
TEST(ShippedLibDescriptor, StructVariantUnknownFormatValueFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "badfmt.json", R"JSON({
        "header": "bf.h",
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "x86_64", "format": "elff" },
                  "fields": [ { "name": "x", "type": "i32" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// ★ THE SENTINEL VARIANT of the test above, and by that test's OWN stated
// rationale: `"unknown"` would never match either, so the struct variant would
// silently vanish on every target — identical consequence, but it survives the
// name lookup because it IS a table row.
//
// RED-ON-DISABLE: remove the `isSelectableObjectFormatKind` branch in the
// `when.format` decode and this read succeeds with a variant that never fires.
TEST(ShippedLibDescriptor, StructVariantSentinelFormatValueFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "sentinelfmt.json", R"JSON({
        "header": "bf.h",
        "structs": [
            { "name": "S", "variants": [
                { "when": { "arch": "x86_64", "format": "unknown" },
                  "fields": [ { "name": "x", "type": "i32" } ] }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_TRUE(anyDiagMentions(rep, "sentinel"));
}

// BACK-COMPAT (gate 8; closure gate 8). An existing flat-`fields` struct decodes
// BYTE-IDENTICALLY whether activeTarget is nullopt (direct-API/LSP/test) or set (a
// real per-target compile) — the flat path never consults the selector, so the
// interned type + its layout are the same. This proves the new axis does not
// perturb the single-layout structs that ship (tm/timespec/utimbuf — timeval
// itself moved to per-format variants at c83; the flat field list here is the
// historical shape, kept as the back-compat fixture).
TEST(ShippedLibDescriptor, StructFlatFieldsBackCompatRegardlessOfTarget) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "flat.json", R"JSON({
        "header": "f.h",
        "structs": [
            { "name": "timeval", "fields": [
                { "name": "tv_sec",  "type": "i64" },
                { "name": "tv_usec", "type": "i64" }
            ] }
        ]
    })JSON");

    auto decodeLayout = [&](std::optional<std::string_view> arch,
                            std::optional<ObjectFormatKind> fmt) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, arch, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->structs.size(), 1u);
        auto layout = computeLayout(desc->structs[0].typeId, interner, kNatural16,
                                    DataModel::Lp64);
        EXPECT_TRUE(layout.has_value());
        return *layout;
    };

    auto const nul = decodeLayout(std::nullopt, std::nullopt);              // direct-API
    auto const set = decodeLayout(std::string_view{"x86_64"}, ObjectFormatKind::Elf); // per-target
    EXPECT_EQ(nul.size, set.size);
    ASSERT_EQ(nul.fieldOffsets.size(), set.fieldOffsets.size());
    EXPECT_EQ(nul.fieldOffsets, set.fieldOffsets);
    EXPECT_EQ(nul.size, 16u);                          // two i64s, no padding
    ASSERT_EQ(nul.fieldOffsets.size(), 2u);
    EXPECT_EQ(nul.fieldOffsets[0], 0u);
    EXPECT_EQ(nul.fieldOffsets[1], 8u);
}

// CRUX RE-VERIFY (gate 2; closure gate 2). The plan-lock's load-bearing claim:
// x86_64 and arm64 `.target.json` feed BYTE-IDENTICAL AggregateLayoutParams, and
// `computeLayout` is purely param-driven (no arch branch). Therefore the ONLY
// source of a per-target offset difference is the selected FIELD LIST — which is
// exactly what the variant mechanism switches. This pin catches a FUTURE
// target.json divergence that would invalidate "field-list-only" (e.g. someone
// gives arm64 a different maxAlignment): if these params ever diverge, a struct
// with the SAME field list could lay out differently per arch and the mechanism's
// premise breaks. Asserted against the REAL shipped schemas, not a fixture.
TEST(ShippedLibDescriptor, CruxX86AndArm64AggregateLayoutParamsIdentical) {
    auto x86R = TargetSchema::loadShipped("x86_64");
    auto arm64R = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(x86R.has_value());
    ASSERT_TRUE(arm64R.has_value());
    ASSERT_TRUE((*x86R)->aggregateLayoutLoaded());
    ASSERT_TRUE((*arm64R)->aggregateLayoutLoaded());
    auto const a = (*x86R)->aggregateLayout();
    auto const b = (*arm64R)->aggregateLayout();
    EXPECT_EQ(a.scalarAlignment, b.scalarAlignment)
        << "x86_64 and arm64 must share the scalar-alignment rule (field-list-only "
           "premise of per-target struct variants)";
    EXPECT_EQ(a.maxAlignment, b.maxAlignment)
        << "x86_64 and arm64 must share maxAlignment (field-list-only premise)";
    // bitFieldStrategy on the target is the back-compat fallback; the layout-driving
    // params above are the two the per-target-struct premise rests on.
}

// ── availableObjectFormats (per-target AVAILABILITY axis; c8) ─────────────────

// `availableObjectFormats` decodes into the descriptor's per-format set (which
// object-formats the header EXISTS on). The full read + the front-end reader
// share ONE decode chokepoint (decodeShippedAvailability), so they cannot drift.
TEST(ShippedLibDescriptor, AvailabilityDecodes) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "av.json", R"JSON({
        "header": "sys/time.h",
        "availableObjectFormats": ["elf", "macho"],
        "typedefs": [ { "name": "time_t", "type": "i64" } ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->availableObjectFormats.size(), 2u);
    EXPECT_EQ(desc->availableObjectFormats[0], "elf");
    EXPECT_EQ(desc->availableObjectFormats[1], "macho");
}

// ABSENT `availableObjectFormats` ⇒ empty set ⇒ available on EVERY format (the
// back-compat default — every pre-c8 descriptor keeps resolving on all targets).
TEST(ShippedLibDescriptor, AvailabilityAbsentIsEmptyAllFormats) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "noav.json", R"({
        "header": "h.h", "typedefs": [ { "name": "t", "type": "i32" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->availableObjectFormats.empty());
}

// An UNKNOWN object-format name in `availableObjectFormats` fails loud — a typo'd
// platform would silently make the header available NOWHERE (the entry never
// matches the active format) or, worse, mask a real availability. RED-ON-DISABLE:
// drop the objectFormatKindFromName check and "bogus" decodes as a live format.
TEST(ShippedLibDescriptor, AvailabilityUnknownFormatFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "badav.json", R"JSON({
        "header": "h.h", "availableObjectFormats": ["elf", "bogus"],
        "typedefs": [ { "name": "t", "type": "i32" } ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// ★ THE SENTINEL VARIANT of the test above. "bogus" fails the name lookup;
// "unknown" passes it and then narrows availability to a format no image can
// have — the header goes silently unavailable EVERYWHERE, which is the first
// failure mode that test names.
//
// RED-ON-DISABLE: remove the `isSelectableObjectFormatKind` branch in
// `decodeShippedAvailability` and "unknown" decodes as a live format.
TEST(ShippedLibDescriptor, AvailabilitySentinelFormatFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "sentinelav.json", R"JSON({
        "header": "h.h", "availableObjectFormats": ["elf", "unknown"],
        "typedefs": [ { "name": "t", "type": "i32" } ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_TRUE(anyDiagMentions(rep, "sentinel"));
}

// readShippedLibAvailability: interner-FREE (the front-end gate's path — neither
// the preprocessor `__has_include` nor the import resolver has a TypeInterner).
// Decodes the SAME set the full read does, through the shared chokepoint.
TEST(ShippedLibDescriptor, ReadShippedLibAvailabilityInternerFree) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "av2.json", R"JSON({
        "header": "sys/time.h", "availableObjectFormats": ["elf", "macho"],
        "structs": [ { "name": "timeval", "fields": [ { "name": "s", "type": "i64" } ] } ]
    })JSON");
    DiagnosticReporter rep;
    auto avail = readShippedLibAvailability(path, rep);   // NO interner / typeReg
    ASSERT_TRUE(avail.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(avail->size(), 2u);
    EXPECT_EQ(avail->at(0), "elf");
    EXPECT_EQ(avail->at(1), "macho");
}

// readShippedLibAvailability on a descriptor with NO availableObjectFormats
// returns an EMPTY vector (NOT nullopt) — empty ⇒ available on every format. The
// reader is no STRICTER than the full read (no header/typed-surface gate).
TEST(ShippedLibDescriptor, ReadShippedLibAvailabilityAbsentIsEmpty) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "av3.json", R"({
        "typedefs": [ { "name": "size_t", "type": "u64" } ]
    })");
    DiagnosticReporter rep;
    auto avail = readShippedLibAvailability(path, rep);
    ASSERT_TRUE(avail.has_value());   // NOT nullopt — absence is not an error
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(avail->empty());      // empty ⇒ every format
}

// readShippedLibAvailability fails loud (nullopt) on a malformed availability —
// the front-end gate must never silently treat a broken descriptor as available.
TEST(ShippedLibDescriptor, ReadShippedLibAvailabilityUnknownFormatFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "av4.json",
        R"({ "header": "h.h", "availableObjectFormats": ["nonsense"] })");
    DiagnosticReporter rep;
    auto avail = readShippedLibAvailability(path, rep);
    EXPECT_FALSE(avail.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// objectFormatInAvailabilitySet: the SHARED membership predicate (c9). The
// semantic `#include` gate + the preprocessor `__has_include` + the macro-splice
// ALL call this, so they can never disagree. Empty set ⇒ available everywhere.
// RED-ON-DISABLE: the gate/__has_include behavior flips if this predicate is wrong.
TEST(ShippedLibDescriptor, ObjectFormatInAvailabilitySetMembership) {
    auto const elf = objectFormatKindFromName("elf").value();
    auto const macho = objectFormatKindFromName("macho").value();
    auto const pe = objectFormatKindFromName("pe").value();
    std::vector<std::string> const elfMacho{"elf", "macho"};
    EXPECT_TRUE(ffi::objectFormatInAvailabilitySet(elfMacho, elf));
    EXPECT_TRUE(ffi::objectFormatInAvailabilitySet(elfMacho, macho));
    EXPECT_FALSE(ffi::objectFormatInAvailabilitySet(elfMacho, pe))
        << "pe ∉ [elf,macho] → unavailable";
    std::vector<std::string> const empty{};
    EXPECT_TRUE(ffi::objectFormatInAvailabilitySet(empty, pe))
        << "empty availableObjectFormats ⇒ available on EVERY format (back-compat)";
}

// shippedHeaderAvailableForFormat: reads the descriptor's availableObjectFormats
// (interner-free) then applies the predicate — the EXACT decision the preprocessor
// `__has_include` + macro-splice make. RED-ON-DISABLE on the per-target gate.
TEST(ShippedLibDescriptor, ShippedHeaderAvailableForFormatReadsDescriptor) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "av5.json", R"JSON({
        "header": "h.h", "availableObjectFormats": ["elf", "macho"],
        "typedefs": [ { "name": "t", "type": "i32" } ]
    })JSON");
    auto const elf = objectFormatKindFromName("elf").value();
    auto const pe = objectFormatKindFromName("pe").value();
    EXPECT_TRUE(ffi::shippedHeaderAvailableForFormat(path, elf));
    EXPECT_FALSE(ffi::shippedHeaderAvailableForFormat(path, pe))
        << "the descriptor excludes pe → __has_include is FALSE / the splice is skipped";
}

// A descriptor with NO availableObjectFormats is available on every format — the
// back-compat default that keeps every pre-c8 header resolving on all targets.
TEST(ShippedLibDescriptor, ShippedHeaderAvailableForFormatAbsentSetIsAllFormats) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "av6.json", R"({
        "header": "h.h", "typedefs": [ { "name": "t", "type": "i32" } ]
    })");
    auto const pe = objectFormatKindFromName("pe").value();
    EXPECT_TRUE(ffi::shippedHeaderAvailableForFormat(path, pe))
        << "no availableObjectFormats ⇒ available on every format";
}

// An "object" kind decodes to ShippedSymbolKind::Object (→ ExternGlobal).
TEST(ShippedLibDescriptor, ObjectKindDecodes) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "obj.json", R"({
        "header": "stdio.h", "library": { "pe": "msvcrt.dll" },
        "symbols": [ { "name": "errno", "signature": "i32", "kind": "object" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    ASSERT_EQ(desc->symbols.size(), 1u);
    EXPECT_EQ(desc->symbols[0].kind, ShippedSymbolKind::Object);
    EXPECT_EQ(interner.kind(desc->symbols[0].signature), TypeKind::I32);
}

// ── Malformed JSON → F_ShippedLibDescriptorMalformed + nullopt ───────────────

TEST(ShippedLibDescriptor, MalformedJsonFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad.json", R"({ "library": )");  // truncated

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

TEST(ShippedLibDescriptor, MissingSymbolsKeyFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "nosyms.json", R"({ "header": "x.h", "library": { "pe": "msvcrt.dll" } })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_GT(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
}

TEST(ShippedLibDescriptor, MissingSignatureKeyFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "nosig.json", R"({
        "header": "stdio.h", "library": { "pe": "msvcrt.dll" },
        "symbols": [ { "name": "puts" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_GT(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
}

TEST(ShippedLibDescriptor, UnknownKeyFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "unknownkey.json", R"({
        "header": "stdio.h", "library": { "pe": "msvcrt.dll" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32",
                       "calling_convention": "stdcall" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_GT(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
}

TEST(ShippedLibDescriptor, UnknownEnumFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "badenum.json", R"({
        "header": "stdio.h", "library": { "pe": "msvcrt.dll" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32",
                       "kind": "macro" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_GT(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
}

// ── Bad signature → F_ShippedLibUnsupportedType, NO InvalidType extern ───────

TEST(ShippedLibDescriptor, TruncatedSignatureFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "truncsig.json", R"({
        "header": "stdio.h", "library": { "pe": "msvcrt.dll" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    // The CRITICAL fail-loud: a signature that fails to decode → nullopt, the
    // dedicated code fires, and NO symbol was ever returned carrying InvalidType.
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibUnsupportedType), 1u);
}

TEST(ShippedLibDescriptor, UnknownTypeSignatureFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "unknowntype.json", R"({
        "header": "stdio.h", "library": { "pe": "msvcrt.dll" },
        "symbols": [ { "name": "puts", "signature": "fn(wat) -> i32" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_GT(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibUnsupportedType), 0u);
}

// `header` is REQUIRED provenance — a descriptor without it fails loud (the
// user must always be able to know where a shipped symbol comes from).
TEST(ShippedLibDescriptor, MissingHeaderFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "noheader.json", R"({
        "library": { "pe": "msvcrt.dll" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_GT(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
}

// c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): the SysV `va_list` named-type binding
// production threads into every shipped-descriptor read (stdio.json's
// vfprintf spells `va_list`). Tests reading SHIPPED files bind it the same
// way — the exact `__va_list_tag[1]` mint the analyzer's SysVRegisterSave
// arm produces. Returns the storage by value; the caller keeps it alive
// across the read.
[[nodiscard]] std::array<NamedTypeBinding, 1>
sysvVaListBinding(TypeInterner& interner) {
    TypeId const voidPtr =
        interner.pointer(interner.primitive(TypeKind::Void));
    std::array<TypeId, 4> tagFields{
        interner.primitive(TypeKind::U32), interner.primitive(TypeKind::U32),
        voidPtr, voidPtr};
    TypeId const vaListTy =
        interner.array(interner.structType("__va_list_tag", tagFields), 1);
    return {NamedTypeBinding{"va_list", vaListTy}};
}

// The shipped descriptor dir, resolved through the ONE test-side resolver
// (`repo_root.hpp`: $DSS_CONFIG_ROOT → the CMake-baked repo root → the cwd
// ancestor walk). This used to be a private ancestor-walk of its own, which
// found nothing in an OUT-OF-TREE build — that cwd has no `src/dss-config/`
// anywhere above it, and the walk never read the `DSS_CONFIG_ROOT` ctest
// already exports. Contract unchanged: empty on a miss, and every caller
// ASSERTs on that. The ADD_FAILURE carries the resolver's three-source
// diagnostic so the log names WHICH of the three lookups came up short, not
// just the call site that noticed.
[[nodiscard]] fs::path shippedLibsRoot() {
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return {};
    }
    return *root / "src" / "dss-config" / "shippedLibs";
}

// ── Item 1: constants + typedefs decode (neutral shipped-header content) ─────

// Happy path: a constants-only descriptor (the <limits.h> shape — no symbols)
// decodes its named integer constants + typedefs structurally. RED-ON-DISABLE:
// the relaxed "symbols OPTIONAL" rule — a pre-change reader rejected a no-symbols
// descriptor.
TEST(ShippedLibDescriptor, ConstantsAndTypedefsDecode) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "limits.json", R"({
        "header": "limits.h",
        "constants": [
            { "name": "CHAR_BIT", "value": 8,           "type": "i32" },
            { "name": "INT_MIN",  "value": -2147483648, "type": "i32" },
            { "name": "UINT_MAX", "value": 4294967295,  "type": "u32" }
        ],
        "typedefs": [ { "name": "my_size_t", "type": "u64" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->symbols.empty());   // a constants-only descriptor: no link surface
    ASSERT_EQ(desc->constants.size(), 3u);
    EXPECT_EQ(desc->constants[0].name, "CHAR_BIT");
    EXPECT_EQ(desc->constants[0].value, 8);
    EXPECT_EQ(interner.kind(desc->constants[0].type), TypeKind::I32);
    EXPECT_EQ(desc->constants[1].name, "INT_MIN");
    EXPECT_EQ(desc->constants[1].value, std::int64_t{-2147483648});
    EXPECT_EQ(desc->constants[2].name, "UINT_MAX");
    EXPECT_EQ(desc->constants[2].value, std::int64_t{4294967295});
    EXPECT_EQ(interner.kind(desc->constants[2].type), TypeKind::U32);
    ASSERT_EQ(desc->typedefs.size(), 1u);
    EXPECT_EQ(desc->typedefs[0].name, "my_size_t");
    EXPECT_EQ(interner.kind(desc->typedefs[0].type), TypeKind::U64);
}

// ── Option C (D-FFI-DESCRIPTOR-TYPEDEF-NAME-RESOLUTION): a descriptor spells its
// OWN typedef BY NAME ────────────────────────────────────────────────────────────
//
// The reader resolves a descriptor's typedefs FIRST and threads each resolved
// `name -> TypeId` into the `namedTypes` span used for the REST of that
// descriptor's signature / struct-field / constant / later-typedef parses. So a
// signature can spell `ptr<Widget>` where `Widget` is a typedef the SAME descriptor
// declares, instead of re-inlining `Widget`'s full struct body at every use (the
// Tcl_Obj ~45-site ripple this closes). GENERIC + content-blind: no name is
// special-cased. Pinned on a SYNTHETIC descriptor (independent of tcl.json): a
// typedef `Widget` = a 2-field struct, a SECOND typedef `WidgetPair` spelling
// `ptr<Widget>` BY NAME, a `structs` field `ptr<Widget>`, and a symbol
// `fn(ptr<Widget>) -> i32` — all four must land the ONE interned `Widget` struct.
// RED-ON-DISABLE: drop the typedef-name threading (pass the bare caller
// `namedTypes` again) → every by-name `Widget` becomes "unknown type" → read fails.
TEST(ShippedLibDescriptor, OptionCDescriptorTypedefReferencedByName) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "widget.json", R"({
        "header": "widget.h",
        "typedefs": [
            { "name": "Widget",     "type": "struct \"Widget\" { i32, ptr<char> }" },
            { "name": "WidgetPair", "type": "struct \"WidgetPair\" { ptr<Widget>, ptr<Widget> }" }
        ],
        "structs": [
            { "name": "Holder", "fields": [ { "name": "w", "type": "ptr<Widget>" } ] }
        ],
        "symbols": [
            { "name": "widget_id", "signature": "fn(ptr<Widget>) -> i32" }
        ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value())
        << "a descriptor referencing its OWN typedef by name must decode";
    EXPECT_FALSE(rep.hasErrors());

    ASSERT_EQ(desc->typedefs.size(), 2u);
    ASSERT_EQ(interner.kind(desc->typedefs[0].type), TypeKind::Struct);
    TypeId const widget = desc->typedefs[0].type;         // Widget
    EXPECT_EQ(interner.name(widget), "Widget");

    // WidgetPair's fields are `ptr<Widget>` — the pointee is the SAME interned Widget.
    ASSERT_EQ(interner.kind(desc->typedefs[1].type), TypeKind::Struct);
    auto const pairFields = interner.operands(desc->typedefs[1].type);
    ASSERT_EQ(pairFields.size(), 2u);
    ASSERT_EQ(interner.kind(pairFields[0]), TypeKind::Ptr);
    auto const pairPointee = interner.operands(pairFields[0]);
    ASSERT_EQ(pairPointee.size(), 1u);
    EXPECT_EQ(pairPointee[0].v, widget.v)
        << "a later typedef spelling ptr<Widget> by name must land the same Widget";

    // The symbol signature `fn(ptr<Widget>) -> i32` — the param pointee is Widget.
    ASSERT_EQ(desc->symbols.size(), 1u);
    ASSERT_EQ(interner.kind(desc->symbols[0].signature), TypeKind::FnSig);
    auto const params = interner.fnParams(desc->symbols[0].signature);
    ASSERT_EQ(params.size(), 1u);
    ASSERT_EQ(interner.kind(params[0]), TypeKind::Ptr);
    auto const symPointee = interner.operands(params[0]);
    ASSERT_EQ(symPointee.size(), 1u);
    EXPECT_EQ(symPointee[0].v, widget.v)
        << "a signature spelling ptr<Widget> by name must land the same Widget";

    // The `structs`-surface field `ptr<Widget>` too — ONE Widget across every surface.
    ASSERT_EQ(desc->structs.size(), 1u);
    ASSERT_EQ(desc->structs[0].fields.size(), 1u);
    ASSERT_EQ(interner.kind(desc->structs[0].fields[0].type), TypeKind::Ptr);
    auto const holderPointee = interner.operands(desc->structs[0].fields[0].type);
    ASSERT_EQ(holderPointee.size(), 1u);
    EXPECT_EQ(holderPointee[0].v, widget.v);
}

// Fail-loud is PRESERVED under Option C: threading the descriptor's OWN typedefs
// adds ONLY those declared names — it does NOT turn every bare identifier into a
// valid type. A signature spelling `ptr<Nonesuch>`, where no such typedef exists,
// still FAILS the read (the identifier fallback is the LAST resort before the
// unknown-type reject). RED-ON-DISABLE of fail-loud: were the fallback to swallow
// an unknown name, this malformed descriptor would wrongly decode.
TEST(ShippedLibDescriptor, OptionCUnknownTypeNameStillFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad.json", R"({
        "header": "bad.h",
        "typedefs": [ { "name": "Widget", "type": "struct \"Widget\" { i32 }" } ],
        "symbols": [ { "name": "f", "signature": "fn(ptr<Nonesuch>) -> i32" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value())
        << "an unknown type name must still fail loud under Option C";
    EXPECT_TRUE(rep.hasErrors());
}

// TF-C65 (D-FFI-SHIPPED-LIBS-OS-ONLY, user-directed 2026-07-24): shipped-lib
// descriptors are for OS/platform surfaces ONLY (libc, syscalls, Win32, the C
// runtime — ABIs that ARE the platform contract). A THIRD-PARTY library the
// user builds and links (Tcl, zlib, and anything like them) must be consumed
// by PARSING ITS REAL HEADERS (`-I`) and resolving its real binary
// (`--resolve-library`) — a hand-transcribed descriptor can silently drift
// from the installed library's ABI, whereas parsing the actual header is
// self-correcting by construction. tcl.json + zlib.json were DELETED once the
// front end parsed real tcl.h/zlib.h clean (TF-C62/63/64) and the end-to-end
// witness ran: real tcl.h parsed + libtcl8.6.so resolved + Tcl_Eval("expr
// 40 + 2") returned 42 with NO descriptor. This tombstone keeps the policy
// enforced: re-adding a third-party descriptor turns it red and forces the
// discussion back to "make the front end parse the real header".
TEST(ShippedLibDescriptor, ThirdPartyDescriptorsStayDeleted) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    EXPECT_FALSE(fs::exists(root / "tcl.json"))
        << "tcl.json must stay deleted — Tcl is a third-party library: parse "
           "its real headers (D-FFI-SHIPPED-LIBS-OS-ONLY)";
    EXPECT_FALSE(fs::exists(root / "zlib.json"))
        << "zlib.json must stay deleted — zlib is a third-party library: parse "
           "its real headers (D-FFI-SHIPPED-LIBS-OS-ONLY)";
}

// MF-2: an unsigned constant at the TOP of its range (ULLONG_MAX) round-trips
// losslessly — stored as the int64 BIT-PATTERN (UINT64_MAX reinterpreted == -1),
// which the HIR fold re-reads as uint64. RED-ON-DISABLE: a naive get<int64_t>
// decode cannot represent ULLONG_MAX.
TEST(ShippedLibDescriptor, UnsignedConstantMaxRoundTrips) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({
        "header": "x.h",
        "constants": [ { "name": "ULLONG_MAX", "value": 18446744073709551615, "type": "u64" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    ASSERT_EQ(desc->constants.size(), 1u);
    EXPECT_EQ(static_cast<std::uint64_t>(desc->constants[0].value),
              0xFFFFFFFFFFFFFFFFull);
}

// Fail-loud: a constant whose `type` is not an integer scalar (a float here) is
// out of scope — F_ShippedLibUnsupportedType, descriptor unusable.
TEST(ShippedLibDescriptor, NonIntegerConstantTypeFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({
        "header": "x.h",
        "constants": [ { "name": "PI", "value": 3, "type": "f64" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibUnsupportedType), 1u);
}

// c52 (D-FFI-MATH-INFINITY): the float-constant surface decodes "inf" -> +inf
// and a finite literal -> its value, both as f64. The INFINITY case is the
// sqlite frontier; the finite case pins the general float-literal path.
TEST(ShippedLibDescriptor, FloatConstantsDecodeInfAndFinite) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "math.json", R"({
        "header": "math.h",
        "floatConstants": [
            { "name": "INFINITY", "value": "inf",  "type": "f64" },
            { "name": "NEG_INF",  "value": "-inf", "type": "f64" },
            { "name": "HALF",     "value": "0.5",  "type": "f64" },
            { "name": "FLT_HALF", "value": "0.5",  "type": "f32" }
        ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->constants.empty());   // floats are NOT in the integer surface
    ASSERT_EQ(desc->floatConstants.size(), 4u);
    EXPECT_EQ(desc->floatConstants[0].name, "INFINITY");
    EXPECT_TRUE(std::isinf(desc->floatConstants[0].value));
    EXPECT_GT(desc->floatConstants[0].value, 0.0);
    EXPECT_EQ(interner.kind(desc->floatConstants[0].type), TypeKind::F64);
    EXPECT_TRUE(std::isinf(desc->floatConstants[1].value));
    EXPECT_LT(desc->floatConstants[1].value, 0.0);
    EXPECT_DOUBLE_EQ(desc->floatConstants[2].value, 0.5);
    EXPECT_EQ(interner.kind(desc->floatConstants[3].type), TypeKind::F32);
}

// c52 NEGATIVE PIN (a): an INTEGER type in `floatConstants` is out of scope —
// F_ShippedLibUnsupportedType (the float-surface sibling of the integer gate;
// an integer constant belongs in `constants`).
TEST(ShippedLibDescriptor, IntegerInFloatConstantsFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({
        "header": "x.h",
        "floatConstants": [ { "name": "N", "value": "1.0", "type": "i32" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibUnsupportedType), 1u);
}

// c52 NEGATIVE PIN (b): a FINITE literal that OVERFLOWS to infinity is rejected
// (only the explicit "inf" token may produce an infinity — never a silent
// overflow). F_ShippedLibDescriptorMalformed (an invalid value).
TEST(ShippedLibDescriptor, FloatConstantOverflowToInfFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({
        "header": "x.h",
        "floatConstants": [ { "name": "OK",  "value": "1.0",  "type": "f64" },
                            { "name": "BAD", "value": "1e400", "type": "f64" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

// c52 NEGATIVE PIN (c): a NUMERIC (non-string) value in `floatConstants` fails
// loud — JSON has no Infinity literal, so the value MUST be a string. This also
// guards the encoding choice (the "inf" token shape) from silent drift. The
// valid `OK` sibling keeps the descriptor from ALSO tripping "declares nothing",
// isolating the single value diagnostic.
TEST(ShippedLibDescriptor, FloatConstantNumericValueFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({
        "header": "x.h",
        "floatConstants": [ { "name": "OK", "value": "1.0", "type": "f64" },
                            { "name": "PI", "value": 3.14,  "type": "f64" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

// Fail-loud: a value that does not fit its declared width (300 in an i8). The
// valid sibling (`OK`) keeps the descriptor from ALSO tripping the "declares
// nothing" rule, isolating the single out-of-range diagnostic.
TEST(ShippedLibDescriptor, OutOfRangeConstantValueFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({
        "header": "x.h",
        "constants": [ { "name": "OK",  "value": 1,   "type": "i32" },
                       { "name": "BAD", "value": 300, "type": "i8"  } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

// Fail-loud: a negative value for an unsigned type (the `OK` sibling isolates
// the single diagnostic, as above).
TEST(ShippedLibDescriptor, NegativeUnsignedConstantFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({
        "header": "x.h",
        "constants": [ { "name": "OK",  "value": 1,  "type": "i32" },
                       { "name": "BAD", "value": -1, "type": "u32" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

// Fail-loud: an unknown per-constant key (closed key set {name,value,type}).
TEST(ShippedLibDescriptor, UnknownConstantKeyFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({
        "header": "x.h",
        "constants": [ { "name": "K", "value": 1, "type": "i32", "extra": 2 } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

// Fail-loud: a descriptor that declares NOTHING (no symbols/constants/typedefs)
// is a no-op artifact — the relaxed "at least one non-empty" rule (replaces the
// old symbols-required rule). RED-ON-DISABLE: the combined non-empty check.
TEST(ShippedLibDescriptor, EmptyDescriptorFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "x.json", R"({ "header": "x.h" })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

// c100 (D-FFI-WINDOWS-KERNEL32-FUNCTIONS, the time.h slice): the REAL shipped
// time.json ships a per-format `struct tm` — MSVCRT (pe) is the ISO-C 9 ints
// (tm_sec..tm_isdst) = 36 bytes with NO tm_gmtoff/tm_zone; glibc/Darwin
// (elf/macho) is 11 fields = 56 bytes. SQLite's os_win stack-allocates a
// `struct tm` and localtime/localtime_s write it IN FULL, so a pe build seeing the
// 56-byte layout would over-read the caller's frame (and an elf build seeing 36
// would short-write). This pins the real file's per-format tm sizeof AND the
// pe 9-int layout. RED-ON-DISABLE: drop the pe struct tm variant → the pe build
// sees the elf 56-byte tm → the pe sizeof assert fails.
TEST(ShippedLibDescriptor, RealTimeStructTmPerFormatLayout) {
    fs::path const shippedRoot = shippedLibsRoot();
    ASSERT_FALSE(shippedRoot.empty())
        << "could not locate src/dss-config/shippedLibs from cwd";
    fs::path const timePath = shippedRoot / "time.json";
    ASSERT_TRUE(fs::exists(timePath)) << timePath.generic_string();

    // sizeof(struct tm) from the REAL time.json, per format, via the SAME layout
    // engine MIR uses (kNatural16 = the shipped-target LP64 params).
    auto tmSizeFor = [&](ObjectFormatKind fmt) -> std::uint64_t {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(timePath, interner, typeReg, rep,
                                             DataModel::Lp64, std::string_view{"x86_64"},
                                             fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        if (!desc.has_value()) return 0;
        for (auto const& s : desc->structs) {
            if (s.name == "tm") {
                auto layout = computeLayout(s.typeId, interner, kNatural16,
                                            DataModel::Lp64);
                EXPECT_TRUE(layout.has_value());
                return layout ? layout->size : 0;
            }
        }
        ADD_FAILURE() << "struct tm absent from time.json for the requested format";
        return 0;
    };
    EXPECT_EQ(tmSizeFor(ObjectFormatKind::Pe), 36u)
        << "pe struct tm must be the 9-int MSVCRT layout (36 bytes, no gmtoff/zone)";
    EXPECT_EQ(tmSizeFor(ObjectFormatKind::Elf), 56u)
        << "elf struct tm is the glibc 11-field layout (56 bytes)";
    EXPECT_EQ(tmSizeFor(ObjectFormatKind::MachO), 56u)
        << "macho struct tm is the Darwin 11-field layout (56 bytes)";
}

// c101 (D-FFI-WINDOWS-KERNEL32-FUNCTIONS, the sync-types slice): the real
// windows.json ships the Win32 synchronization structs — SRWLOCK (a single PVOID
// Ptr, 8 bytes) and CRITICAL_SECTION (the RTL_CRITICAL_SECTION 6-field layout:
// ptr DebugInfo + i32 LockCount + i32 RecursionCount + ptr OwningThread + ptr
// LockSemaphore + u64 SpinCount = 40 bytes on x64). SQLite's sqlite3_mutex embeds
// `union { CRITICAL_SECTION cs; SRWLOCK srwl; }` and passes &cs/&srwl to
// Initialize/Enter/Leave, which write the FULL struct — a too-small CRITICAL_SECTION
// would let kernel32 overflow the caller's mutex slot. Pins the real file's pe
// layout. RED-ON-DISABLE: drop a CRITICAL_SECTION field (e.g. SpinCount) → sizeof
// != 40. windows.json is pe-only, so this loads with ObjectFormatKind::Pe.
TEST(ShippedLibDescriptor, RealWindowsSyncStructLayout) {
    fs::path const shippedRoot = shippedLibsRoot();
    ASSERT_FALSE(shippedRoot.empty())
        << "could not locate src/dss-config/shippedLibs from cwd";
    fs::path const winPath = shippedRoot / "windows.json";
    ASSERT_TRUE(fs::exists(winPath)) << winPath.generic_string();

    auto sizeOf = [&](std::string_view structName) -> std::uint64_t {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(winPath, interner, typeReg, rep,
                                             DataModel::Lp64, std::string_view{"x86_64"},
                                             ObjectFormatKind::Pe);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        if (!desc.has_value()) return 0;
        for (auto const& s : desc->structs) {
            if (s.name == structName) {
                auto layout = computeLayout(s.typeId, interner, kNatural16,
                                            DataModel::Lp64);
                EXPECT_TRUE(layout.has_value());
                return layout ? layout->size : 0;
            }
        }
        ADD_FAILURE() << "struct " << structName << " absent from windows.json";
        return 0;
    };
    EXPECT_EQ(sizeOf("SRWLOCK"), 8u) << "SRWLOCK is a single PVOID Ptr";
    EXPECT_EQ(sizeOf("CRITICAL_SECTION"), 40u)
        << "RTL_CRITICAL_SECTION x64: ptr+i32+i32+ptr+ptr+u64 = 40 bytes";
}

// c115 SEH (D-WIN64-SEH-FUNCLETS): the x64 EXCEPTION_RECORD layout the sqlite
// sehExceptionFilter reads (.NumberParameters + .ExceptionInformation[2]) — the
// SDK's um/winnt.h shape, natural C alignment: ExceptionCode@0, ExceptionFlags@4,
// ExceptionRecord@8, ExceptionAddress@16, NumberParameters@24, [pad@28],
// ExceptionInformation[15]@32, sizeof 152. A wrong offset here would read garbage
// exception state at c116 runtime (the class the linux legs can't catch —
// runtime-probed like the c106 _wfinddata64i32 fix). Also pins the pe-only gate
// (EXCEPTION_RECORD is meaningless on elf/macho).
TEST(ShippedLibDescriptor, RealWindowsExceptionRecordLayout) {
    fs::path const shippedRoot = shippedLibsRoot();
    ASSERT_FALSE(shippedRoot.empty());
    fs::path const winPath = shippedRoot / "windows.json";
    ASSERT_TRUE(fs::exists(winPath));

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(winPath, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Pe);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    // windows.json is pe-only.
    EXPECT_TRUE(objectFormatInAvailabilitySet(desc->availableObjectFormats,
                                              ObjectFormatKind::Pe));
    EXPECT_FALSE(objectFormatInAvailabilitySet(desc->availableObjectFormats,
                                               ObjectFormatKind::Elf));

    auto layoutOf = [&](std::string_view name) -> std::optional<StructLayout> {
        for (auto const& s : desc->structs) {
            if (s.name == name) {
                return computeLayout(s.typeId, interner, kNatural16, DataModel::Lp64);
            }
        }
        ADD_FAILURE() << "struct " << name << " absent from windows.json";
        return std::nullopt;
    };

    auto er = layoutOf("EXCEPTION_RECORD");
    ASSERT_TRUE(er.has_value());
    EXPECT_EQ(er->size, 152u) << "x64 EXCEPTION_RECORD is 152 bytes";
    ASSERT_EQ(er->fieldOffsets.size(), 6u);
    EXPECT_EQ(er->fieldOffsets[0], 0u)  << "ExceptionCode@0";
    EXPECT_EQ(er->fieldOffsets[1], 4u)  << "ExceptionFlags@4";
    EXPECT_EQ(er->fieldOffsets[2], 8u)  << "ExceptionRecord@8";
    EXPECT_EQ(er->fieldOffsets[3], 16u) << "ExceptionAddress@16";
    EXPECT_EQ(er->fieldOffsets[4], 24u) << "NumberParameters@24 (sqlite reads this)";
    EXPECT_EQ(er->fieldOffsets[5], 32u)
        << "ExceptionInformation[15]@32 after the u32→u64 alignment pad "
           "(sqlite reads [2])";

    auto ep = layoutOf("EXCEPTION_POINTERS");
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->size, 16u) << "two pointers";
    ASSERT_EQ(ep->fieldOffsets.size(), 2u);
    EXPECT_EQ(ep->fieldOffsets[0], 0u) << "ExceptionRecord*@0";
    EXPECT_EQ(ep->fieldOffsets[1], 8u) << "ContextRecord*@8";

    // THE load-bearing identity: EXCEPTION_POINTERS.ExceptionRecord is a pointer
    // to an INLINE struct-text that MUST intern to the SAME TypeId as the
    // field-bearing standalone EXCEPTION_RECORD — else `p->ExceptionRecord->
    // NumberParameters` cannot resolve (struct identity is by name + field
    // TYPES, ignoring field names). Pin the two TypeIds equal.
    TypeId erStandalone{}, epFieldPointee{};
    for (auto const& s : desc->structs) {
        if (s.name == "EXCEPTION_RECORD")   erStandalone   = s.typeId;
        if (s.name == "EXCEPTION_POINTERS") {
            auto const fields = interner.operands(s.typeId);   // field types
            ASSERT_GE(fields.size(), 1u);
            // field 0 = ExceptionRecord* — its pointee is the inline struct.
            auto const pointee = interner.operands(fields[0]);
            ASSERT_GE(pointee.size(), 1u);
            epFieldPointee = pointee[0];
        }
    }
    ASSERT_TRUE(erStandalone.valid());
    ASSERT_TRUE(epFieldPointee.valid());
    EXPECT_EQ(erStandalone, epFieldPointee)
        << "the inline EXCEPTION_RECORD in EXCEPTION_POINTERS.ExceptionRecord "
           "must intern to the same TypeId as the standalone struct — the "
           "p->ExceptionRecord->member resolution depends on it";
}


// c102 (D-FFI-WINDOWS-KERNEL32-FUNCTIONS, the file/heap/time slice): the real
// windows.json ships the 47 kernel32 file/heap/mmap/library/error/sysinfo/time
// functions the sqlite os_win VFS calls through its aSyscall[] table — every one an
// SDK-verified real kernel32 export (HeapAlloc/HeapReAlloc/HeapSize forward to
// NTDLL.Rtl*, loader-valid exactly like c101's AcquireSRWLockExclusive). This pins
// the DECODED SIGNATURES so a width/arity/return regression fails loud HERE, not as
// a silent os_win miscompile: SIZE_T must decode u64 (a u32 truncates a >4 GiB mmap
// length); SetFilePointerEx's by-value LARGE_INTEGER (an 8-byte union) must be the
// single i64 the Win x64 ABI passes in one register; LPCWSTR must be ptr<u16> (wide)
// and LPCSTR ptr<char> (ANSI) — a swap silently corrupts every path string. Every
// signature is the sqlite os_win aSyscall[] WINAPI cast (ground truth). RED-ON-DISABLE:
// drop a symbol → the presence loop fails; change a scalar width / swap wide-vs-ANSI
// → the shape / pointee assert fails. windows.json is pe-only (ObjectFormatKind::Pe).
TEST(ShippedLibDescriptor, RealWindowsKernel32FileHeapTimeSignatures) {
    fs::path const shippedRoot = shippedLibsRoot();
    ASSERT_FALSE(shippedRoot.empty())
        << "could not locate src/dss-config/shippedLibs from cwd";
    fs::path const winPath = shippedRoot / "windows.json";
    ASSERT_TRUE(fs::exists(winPath)) << winPath.generic_string();

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(winPath, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Pe);
    ASSERT_TRUE(desc.has_value());
    ASSERT_FALSE(rep.hasErrors());

    auto sigOf = [&](std::string_view name) -> std::optional<TypeId> {
        for (auto const& s : desc->symbols)
            if (s.name == name) return s.signature;
        return std::nullopt;
    };

    // (1) Every c102 kernel32 function is present + decodes to an FnSig.
    static constexpr std::string_view kC102Fns[] = {
        "CreateFileW", "DeleteFileW", "ReadFile", "WriteFile", "SetFilePointerEx",
        "SetEndOfFile", "FlushFileBuffers", "GetFileSizeEx", "GetFileAttributesW",
        "GetFileAttributesExW", "GetFullPathNameW", "GetTempPathW", "AreFileApisANSI",
        "LockFileEx", "UnlockFileEx", "CloseHandle", "HeapCreate", "HeapDestroy",
        "HeapAlloc", "HeapReAlloc", "HeapFree", "HeapSize", "HeapCompact",
        "HeapValidate", "GetProcessHeap", "CreateFileMappingW", "MapViewOfFile",
        "UnmapViewOfFile", "FlushViewOfFile", "LoadLibraryW", "FreeLibrary",
        "GetProcAddress", "GetLastError", "FormatMessageW", "LocalFree",
        "OutputDebugStringA", "GetSystemInfo", "GetSystemTimeAsFileTime",
        "GetTickCount64", "QueryPerformanceCounter", "Sleep", "GetCurrentProcessId",
        "GetCurrentThreadId", "WaitForSingleObject", "WaitForSingleObjectEx",
        "MultiByteToWideChar", "WideCharToMultiByte",
    };
    for (auto name : kC102Fns) {
        auto s = sigOf(name);
        ASSERT_TRUE(s.has_value()) << name << " absent from windows.json symbols";
        EXPECT_EQ(interner.kind(*s), TypeKind::FnSig) << name;
    }

    // (2) Representative signatures pinned to exact (result, params...) shape —
    // the full scalar/pointer/void span and arities 0/1/3/4/7/8.
    using K = TypeKind;
    auto shape = [&](std::string_view name, K ret, std::vector<K> const& params) {
        auto s = sigOf(name);
        ASSERT_TRUE(s.has_value()) << name;
        ASSERT_EQ(interner.kind(*s), K::FnSig) << name;
        EXPECT_EQ(interner.kind(interner.fnResult(*s)), ret) << name << " return";
        auto ps = interner.fnParams(*s);
        ASSERT_EQ(ps.size(), params.size()) << name << " arity";
        for (std::size_t i = 0; i < params.size(); ++i)
            EXPECT_EQ(interner.kind(ps[i]), params[i]) << name << " param " << i;
    };
    shape("CreateFileW", K::Ptr,
          {K::Ptr, K::U32, K::U32, K::Ptr, K::U32, K::U32, K::Ptr});
    shape("SetFilePointerEx", K::I32, {K::Ptr, K::I64, K::Ptr, K::U32}); // LARGE_INTEGER by-value = i64
    shape("HeapAlloc", K::Ptr, {K::Ptr, K::U32, K::U64});                // SIZE_T = u64
    shape("GetLastError", K::U32, {});
    shape("GetTickCount64", K::U64, {});
    shape("GetSystemInfo", K::Void, {K::Ptr});
    shape("WideCharToMultiByte", K::I32,
          {K::U32, K::U32, K::Ptr, K::I32, K::Ptr, K::I32, K::Ptr, K::Ptr});

    // (3) wide (LPCWSTR → ptr<u16>) vs ANSI (LPCSTR → ptr<char>) must not swap.
    auto pointeeKind = [&](std::string_view name, std::size_t paramIdx) -> K {
        auto s = sigOf(name);
        EXPECT_TRUE(s.has_value()) << name;
        if (!s) return K::Void;
        auto ps = interner.fnParams(*s);
        EXPECT_GT(ps.size(), paramIdx) << name;
        if (ps.size() <= paramIdx) return K::Void;
        auto elem = interner.operands(ps[paramIdx]);
        EXPECT_EQ(elem.size(), 1u) << name << " param " << paramIdx << " is not a ptr";
        return elem.empty() ? K::Void : interner.kind(elem[0]);
    };
    EXPECT_EQ(pointeeKind("CreateFileW", 0), K::U16) << "LPCWSTR path is wide (u16)";
    EXPECT_EQ(pointeeKind("GetProcAddress", 1), K::Char) << "LPCSTR name is ANSI (char)";
    EXPECT_EQ(pointeeKind("MultiByteToWideChar", 2), K::Char) << "LPCSTR src is ANSI";
    EXPECT_EQ(pointeeKind("MultiByteToWideChar", 4), K::U16) << "LPWSTR dst is wide";
}

// Every descriptor SHIPPED under src/dss-config/shippedLibs/*.json (Model 3: a
// FLAT, platform-neutral layout — one descriptor per header) must read + decode
// cleanly: valid JSON, a non-empty `header` that AGREES with the filename stem
// (the resolver routes `<stdlib.h>`→stdlib.json by stem, so a descriptor whose
// `header` provenance lies about its origin is a real bug), and EVERY symbol's
// `signature` decodes via the one type-text codec. A malformed JSON or an
// unencodable signature in a shipped descriptor breaks the standard-library
// surface — fail loud HERE, not at a user's `#include`.
TEST(ShippedLibDescriptor, AllShippedDescriptorsDecode) {
    fs::path const shippedRoot = shippedLibsRoot();
    ASSERT_FALSE(shippedRoot.empty())
        << "could not locate src/dss-config/shippedLibs from cwd";

    std::size_t count = 0;
    for (auto const& entry : fs::recursive_directory_iterator(shippedRoot)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        ++count;
        // Fresh interner per descriptor — each shipped lib is read into its
        // consuming CU's interner in production.
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        // c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): thread the SysV va_list
        // binding exactly as production does (stdio.json's vfprintf).
        auto const namedTypes = sysvVaListBinding(interner);
        auto desc = readShippedLibDescriptor(entry.path(), interner, typeReg, rep,
                                             DataModel::Lp64, std::nullopt,
                                             std::nullopt, namedTypes);
        EXPECT_TRUE(desc.has_value())
            << "shipped descriptor failed to load: "
            << entry.path().generic_string();
        EXPECT_FALSE(rep.hasErrors())
            << "shipped descriptor emitted diagnostics: "
            << entry.path().generic_string();
        if (desc.has_value()) {
            EXPECT_FALSE(desc->header.empty())
                << "shipped descriptor has empty header: "
                << entry.path().generic_string();
            // Provenance integrity: `header` MUST match the descriptor's path
            // RELATIVE to shippedRoot, subdir-PRESERVING (mirrors the resolver:
            // `<stdio.h>`->stdio.json->"stdio.h"; `<sys/types.h>`->sys/types.json
            // ->"sys/types.h"). RED if a clone left stdlib.json saying stdio.h, OR
            // a `sys/*` descriptor flattens its provenance to the bare stem.
            fs::path const rel = fs::relative(entry.path(), shippedRoot);
            std::string const expectedHeader =
                (rel.parent_path() / rel.stem()).generic_string() + ".h";
            EXPECT_EQ(desc->header, expectedHeader)
                << "header provenance must match the subdir-preserving filename in "
                << entry.path().generic_string();
            for (auto const& s : desc->symbols) {
                EXPECT_TRUE(s.signature.valid())
                    << "symbol '" << s.name << "' has invalid signature in "
                    << entry.path().generic_string();
            }
        }
    }
    EXPECT_GT(count, 0u)
        << "no shipped descriptors found under " << shippedRoot.generic_string();
}

// Model 3 (2026-06-09): the descriptors are PLATFORM-NEUTRAL — ONE stdlib.json /
// stdio.json with a per-format `library` map. The 6 `long`-bearing symbols carry
// the C `long`/`unsigned long` type in the **LP64** (i64/u64) form, which is
// correct for the runnable linux/macos targets. `AllShippedDescriptorsDecode`
// only proves the signatures DECODE — both `i32` and `i64` are valid types, so a
// regression that reverted these to the Windows LLP64 (i32/u32) widths would
// stay GREEN there. This pins the ACTUAL neutral result widths STRUCTURALLY
// (interner accessors, not a string compare) so a width revert goes RED.
//
// The Windows LLP64 (i32/u32) form for these 6 is latently DEFERRED — UNEXERCISED
// by any corpus/test — and tracked by `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH`
// (a `long` whose width depends on the data model). When that anchor lands a
// per-target primitive-width model, the neutral descriptor's `long` will resolve
// to i32 on Windows and i64 on Unix; until then the neutral i64/u64 is the single
// authored form and this test guards it.
TEST(ShippedLibDescriptor, ShippedStdlibSignaturesAreLp64) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";

    // Find a named function symbol in the FLAT <lib>.json and return its FnSig
    // (interner kept alive by the caller via the returned descriptor).
    auto fnSigOf = [&](TypeInterner& interner, char const* lib,
                       char const* symName) -> TypeId {
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto const namedTypes = sysvVaListBinding(interner);   // c82
        auto desc = readShippedLibDescriptor(
            root / (std::string(lib) + ".json"), interner, typeReg, rep,
            DataModel::Lp64, std::nullopt, std::nullopt, namedTypes);
        EXPECT_TRUE(desc.has_value()) << lib << ".json failed to load";
        EXPECT_FALSE(rep.hasErrors()) << lib << ".json emitted diagnostics";
        if (!desc.has_value()) return {};
        for (auto const& s : desc->symbols) {
            if (s.name == symName) {
                EXPECT_EQ(interner.kind(s.signature), TypeKind::FnSig)
                    << symName << " is not a function in " << lib;
                return s.signature;
            }
        }
        ADD_FAILURE() << symName << " not found in " << lib;
        return {};
    };
    // The FnSig RESULT kind of <lib>::<sym>.
    auto resultKindOf = [&](char const* lib, char const* symName) -> TypeKind {
        TypeInterner interner{CompilationUnitId{1}};
        TypeId const sig = fnSigOf(interner, lib, symName);
        return sig.valid() ? interner.kind(interner.fnResult(sig)) : TypeKind::Void;
    };
    // The FnSig PARAM[i] kind of <lib>::<sym> (for fseek's offset).
    auto paramKindOf = [&](char const* lib, char const* symName,
                           std::size_t i) -> TypeKind {
        TypeInterner interner{CompilationUnitId{1}};
        TypeId const sig = fnSigOf(interner, lib, symName);
        if (!sig.valid()) return TypeKind::Void;
        auto const params = interner.fnParams(sig);
        EXPECT_GT(params.size(), i) << symName << " has too few params";
        return i < params.size() ? interner.kind(params[i]) : TypeKind::Void;
    };

    // Pin EVERY `long`-bearing symbol (not a subset — a per-symbol copy-paste is
    // the exact failure mode). stdlib: atol/strtol return long, strtoul returns
    // unsigned long, labs takes+returns long. stdio: ftell returns long, fseek's
    // offset (param[1]) is long. LP64: `long` = 64-bit → i64; `unsigned long` → u64.
    EXPECT_EQ(resultKindOf("stdlib", "atol"),    TypeKind::I64);
    EXPECT_EQ(resultKindOf("stdlib", "strtol"),  TypeKind::I64);
    EXPECT_EQ(resultKindOf("stdlib", "strtoul"), TypeKind::U64);
    EXPECT_EQ(resultKindOf("stdlib", "labs"),    TypeKind::I64);
    EXPECT_EQ(resultKindOf("stdio",  "ftell"),   TypeKind::I64);
    EXPECT_EQ(paramKindOf("stdio",   "fseek", 1), TypeKind::I64);
    // labs takes long too (param[0]) — pin it so a revert of the ARG width
    // (not just the result) also goes RED.
    EXPECT_EQ(paramKindOf("stdlib",  "labs", 0),  TypeKind::I64);
}

// Model 3 per-format `library` MAP: a shipped descriptor routes a DIFFERENT
// runtime image per object format, keyed by the canonical objectFormatKindName
// vocabulary ("pe"/"elf"/"macho"). Pin that the SHIPPED stdio.json carries all
// three (RED if a clone drops one or hardcodes a single string), AND that an
// UNKNOWN format key fails loud (the typo-catch the map's vocabulary check buys).
TEST(ShippedLibDescriptor, ShippedStdioLibraryMapRoutesPerObjectFormat) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto const namedTypes = sysvVaListBinding(interner);   // c82: vfprintf's va_list
    auto desc = readShippedLibDescriptor(root / "stdio.json", interner, typeReg, rep,
                                         DataModel::Lp64, std::nullopt,
                                         std::nullopt, namedTypes);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    // The neutral descriptor names the correct runtime per format — the whole
    // point of Model 3. RED if a future edit reverts to one string or swaps them.
    //
    // ★ D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) FLIPPED the pe entry msvcrt.dll →
    // ucrtbase.dll, so this is a RE-AIM of an exact-string pin, not a relaxation:
    // it is still one `EXPECT_EQ` per format and a revert to msvcrt still reds
    // here. stdio is one of NINE descriptors that flipped ATOMICALLY (stdio, io,
    // errno, stdlib, malloc, string, direct, process, sys/stat) because they share
    // CRT co-state — FILE buffers, `errno`, the heap, the fd table — and a split
    // bind would silently read one runtime's state through the other's API.
    // setjmp.json deliberately stays on msvcrt (ucrtbase exports no `_setjmp`, and
    // `jmp_buf` is caller-owned, so it carries no cross-runtime state). The
    // GROUP-WIDE atomicity is pinned in tests/ffi/test_pe_crt_costate_binding.cpp;
    // this test keeps its original single-descriptor scope.
    EXPECT_EQ(desc->library.at("pe"),    "ucrtbase.dll");
    EXPECT_EQ(desc->library.at("elf"),   "libc.so.6");
    EXPECT_EQ(desc->library.at("macho"), "/usr/lib/libSystem.B.dylib");
}

// An UNKNOWN object-format key in the `library` map is a typo/garbage and fails
// loud on read (F_ShippedLibDescriptorMalformed) — NOT a silently-ignored key
// that would route nothing. RED-on-disable: drop the objectFormatKindFromName
// check and "pee" decodes silently.
TEST(ShippedLibDescriptor, UnknownLibraryFormatKeyFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "badfmt.json", R"({
        "header": "stdio.h", "library": { "pee": "msvcrt.dll" },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

// A non-OBJECT `library` (the pre-Model-3 single-string shape) is now malformed —
// the schema requires the per-format map. Pin the rejection so a stale string
// descriptor fails loud rather than silently losing its routing.
TEST(ShippedLibDescriptor, NonObjectLibraryFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "strlib.json", R"({
        "header": "stdio.h", "library": "msvcrt.dll",
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;

    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);
}

// `standard` is optional provenance — it round-trips when present, and a
// non-string `standard` fails loud (it is type-checked on read). Brand-new field,
// so pin both the populate path and the rejection path directly.
TEST(ShippedLibDescriptor, StandardProvenanceRoundTripsAndTypeChecks) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    {
        auto const path = writeTemp(dir, "std.json", R"({
            "header": "stdio.h", "standard": "c99", "library": { "pe": "msvcrt.dll" },
            "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
        })");
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
        ASSERT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->standard, "c99");
    }
    {
        auto const path = writeTemp(dir, "badstd.json", R"({
            "header": "stdio.h", "standard": 89,
            "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
        })");
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
        EXPECT_FALSE(desc.has_value());
        EXPECT_GT(dss::test_support::countCode(
                      rep, DiagnosticCode::F_ShippedLibDescriptorMalformed), 0u);
    }
}

// ── per-target CONSTANT VARIANTS (per-target VALUE/TYPE; plan 25 extension) ────
//
// A `constants` entry may declare per-target `variants` (each `when:{arch?,format?}`
// + its own {value,type}) INSTEAD of a flat {value,type}, so a constant's VALUE can
// diverge per target (the per-platform `O_NONBLOCK` case). The selection mirrors the
// struct surface: MATCH-ALL-SPECIFIED, exactly-one, eager-decode-all, ambiguous
// fail-loud. The result is the SAME flat ShippedConstant — no inject-path change.

// SELECTION PIN: format=elf picks value A (4), format=macho picks value B (2048),
// from ONE descriptor. RED-ON-DISABLE: neuter the selector to always take
// variants[0] and the macho assertion (2048) fails — macho would see the elf value.
TEST(ShippedLibDescriptor, ConstantVariantSelectsPerFormatValue) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "cvar.json", R"JSON({
        "header": "cv.h",
        "constants": [
            { "name": "O_NONBLOCK", "variants": [
                { "when": { "format": "elf" },   "value": 4,    "type": "i32" },
                { "when": { "format": "macho" }, "value": 2048, "type": "i32" }
            ] }
        ]
    })JSON");
    auto valueFor = [&](ObjectFormatKind fmt) -> std::int64_t {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, std::string_view{"x86_64"}, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->constants.size(), 1u);
        EXPECT_EQ(desc->constants[0].name, "O_NONBLOCK");
        return desc->constants.empty() ? -1 : desc->constants[0].value;
    };
    EXPECT_EQ(valueFor(ObjectFormatKind::Elf), 4);
    EXPECT_EQ(valueFor(ObjectFormatKind::MachO), 2048);
}

// AMBIGUOUS-MATCH PIN: two variants BOTH matching the active (arch,format) →
// F_ShippedConstantVariantAmbiguous, never silently first-wins.
TEST(ShippedLibDescriptor, ConstantVariantAmbiguousMatchFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "camb.json", R"JSON({
        "header": "ca.h",
        "constants": [
            { "name": "K", "variants": [
                { "when": { "format": "elf" }, "value": 1, "type": "i32" },
                { "when": { "format": "elf" }, "value": 2, "type": "i32" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedConstantVariantAmbiguous),
              1u);
}

// EAGER-DECODE PIN: a NON-active variant carries an out-of-range value (300 in an
// i8). Even compiling for the OTHER (active) target — whose variant is fine — the
// read FAILS LOUD: every variant's {value,type} is decoded at read time, so a
// malformed inactive variant never lurks until its target's first compile.
TEST(ShippedLibDescriptor, ConstantVariantEagerDecodeMalformedInactiveFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "ceager.json", R"JSON({
        "header": "ce.h",
        "constants": [
            { "name": "K", "variants": [
                { "when": { "format": "elf" },   "value": 1,   "type": "i32" },
                { "when": { "format": "macho" }, "value": 300, "type": "i8"  }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    // Compile for elf (its variant decodes fine); the INACTIVE macho variant's
    // out-of-range value still fails the whole read.
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedLibDescriptorMalformed),
              1u);   // out-of-range value → malformed
}

// DECLARES-SOMETHING PIN: a descriptor whose ONLY surface is constant `variants`
// injects ZERO constants under the nullopt direct-API path, yet it DECLARES a
// constant surface → NOT a false "declares nothing". RED-ON-DISABLE: drop the
// `declaredConstants` term from the declares-something check → this read fails loud.
TEST(ShippedLibDescriptor, ConstantVariantsOnlyDescriptorValidUnderNullopt) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "convonly.json", R"JSON({
        "header": "convonly.h",
        "constants": [
            { "name": "K", "variants": [
                { "when": { "format": "elf" },   "value": 1, "type": "i32" },
                { "when": { "format": "macho" }, "value": 2, "type": "i32" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);  // nullopt target
    ASSERT_TRUE(desc.has_value());        // declares a constant surface → NOT a no-op
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->constants.empty()); // no target → nothing injected
}

// A constant entry declaring BOTH a flat value/type AND `variants` is malformed
// (ambiguous intent) → fail loud.
TEST(ShippedLibDescriptor, ConstantBothFlatAndVariantsFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "cboth.json", R"JSON({
        "header": "cb.h",
        "constants": [
            { "name": "K", "value": 1, "type": "i32",
              "variants": [ { "when": { "format": "elf" }, "value": 2, "type": "i32" } ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// ── per-target TYPEDEF VARIANTS (per-target WIDTH; plan 25 extension) ──────────
//
// A `typedefs` entry may declare per-target `variants` (each `when` + its own
// `type`) INSTEAD of a flat `type`; the name is invariant, only the width varies
// (a `wchar_t` that is i32 on elf but i16 on pe). Same selection contract.

// SELECTION PIN: format=elf picks i32, format=macho picks i16, from ONE descriptor.
// RED-ON-DISABLE: neuter the selector → macho sees i32.
TEST(ShippedLibDescriptor, TypedefVariantSelectsPerFormatType) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "tvar.json", R"JSON({
        "header": "tv.h",
        "typedefs": [
            { "name": "wchar_t", "variants": [
                { "when": { "format": "elf" },   "type": "i32" },
                { "when": { "format": "macho" }, "type": "i16" }
            ] }
        ]
    })JSON");
    auto kindFor = [&](ObjectFormatKind fmt) -> TypeKind {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, std::string_view{"x86_64"}, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->typedefs.size(), 1u);
        if (desc->typedefs.empty()) return TypeKind::Void;
        EXPECT_EQ(desc->typedefs[0].name, "wchar_t");
        return interner.kind(desc->typedefs[0].type);
    };
    EXPECT_EQ(kindFor(ObjectFormatKind::Elf), TypeKind::I32);
    EXPECT_EQ(kindFor(ObjectFormatKind::MachO), TypeKind::I16);
}

// AMBIGUOUS-MATCH PIN: two typedef variants BOTH matching → F_ShippedTypedefVariantAmbiguous.
TEST(ShippedLibDescriptor, TypedefVariantAmbiguousMatchFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "tamb.json", R"JSON({
        "header": "ta.h",
        "typedefs": [
            { "name": "t", "variants": [
                { "when": { "format": "elf" }, "type": "i32" },
                { "when": { "format": "elf" }, "type": "i64" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedTypedefVariantAmbiguous),
              1u);
}

// EAGER-DECODE PIN: a NON-active typedef variant carries an undecodable type. Even
// compiling for the OTHER (active) target the read FAILS LOUD (every variant's type
// is decoded at read time).
TEST(ShippedLibDescriptor, TypedefVariantEagerDecodeMalformedInactiveFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "teager.json", R"JSON({
        "header": "te.h",
        "typedefs": [
            { "name": "t", "variants": [
                { "when": { "format": "elf" },   "type": "i32" },
                { "when": { "format": "macho" }, "type": "not_a_type" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedLibUnsupportedType),
              1u);
}

// DECLARES-SOMETHING PIN: a typedef-variants-only descriptor under nullopt declares
// a typedef surface → NOT "declares nothing". RED-ON-DISABLE: drop `declaredTypedefs`.
TEST(ShippedLibDescriptor, TypedefVariantsOnlyDescriptorValidUnderNullopt) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "tdvonly.json", R"JSON({
        "header": "tdvonly.h",
        "typedefs": [
            { "name": "t", "variants": [
                { "when": { "format": "elf" },   "type": "i32" },
                { "when": { "format": "macho" }, "type": "i16" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);  // nullopt target
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->typedefs.empty());
}

// A typedef entry declaring BOTH a flat type AND `variants` is malformed → fail loud.
TEST(ShippedLibDescriptor, TypedefBothFlatAndVariantsFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "tboth.json", R"JSON({
        "header": "tb.h",
        "typedefs": [
            { "name": "t", "type": "i32",
              "variants": [ { "when": { "format": "elf" }, "type": "i16" } ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// ── per-format MACRO VARIANTS (per-format REPLACEMENT; plan 25 extension) ──────
//
// A `macros` entry may declare per-FORMAT `variants` (each `when:{format}` + its
// own replacement) INSTEAD of a flat body, so a macro can carry a different
// replacement per object-format (the errno `__errno_location`/elf vs `__error`/macho
// case). FORMAT-ONLY — arch is not threaded into the preprocessor. The full read
// passes `activeFormat`; selection produces the SAME flat ShippedMacro.

// SELECTION PIN: format=elf picks replacement A, format=macho picks replacement B,
// from ONE descriptor. RED-ON-DISABLE: neuter the selector → macho sees the elf
// replacement. Read via the SEMANTIC path (readShippedLibDescriptor) which threads
// activeFormat into decodeShippedMacros.
TEST(ShippedLibDescriptor, MacroVariantSelectsPerFormatReplacement) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "mvar.json", R"JSON({
        "header": "mv.h",
        "macros": [
            { "name": "__errno_location_macro", "variants": [
                { "when": { "format": "elf" },   "replacement": "(*__errno_location())" },
                { "when": { "format": "macho" }, "replacement": "(*__error())" }
            ] }
        ]
    })JSON");
    auto replFor = [&](ObjectFormatKind fmt) -> std::string {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, std::string_view{"x86_64"}, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->macros.size(), 1u);
        if (desc->macros.empty()) return {};
        EXPECT_EQ(desc->macros[0].name, "__errno_location_macro");
        return desc->macros[0].replacement;
    };
    EXPECT_EQ(replFor(ObjectFormatKind::Elf), "(*__errno_location())");
    EXPECT_EQ(replFor(ObjectFormatKind::MachO), "(*__error())");
}

// SELECTION PIN via the INTERNER-FREE preprocessor reader: readShippedLibMacros with
// activeFormat selects the per-format replacement WITHOUT a TypeInterner (the
// preprocessor's actual path). Confirms the threaded activeFormat reaches the
// interner-free reader too.
TEST(ShippedLibDescriptor, MacroVariantSelectsViaInternerFreeReader) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "mvar2.json", R"JSON({
        "header": "mv2.h",
        "macros": [
            { "name": "ERRNO", "variants": [
                { "when": { "format": "elf" },   "replacement": "elf_errno" },
                { "when": { "format": "macho" }, "replacement": "macho_errno" }
            ] }
        ]
    })JSON");
    {
        DiagnosticReporter rep;
        auto macros = readShippedLibMacros(path, rep, ObjectFormatKind::Elf);
        ASSERT_TRUE(macros.has_value());
        EXPECT_FALSE(rep.hasErrors());
        ASSERT_EQ(macros->size(), 1u);
        EXPECT_EQ(macros->at(0).replacement, "elf_errno");
    }
    {
        DiagnosticReporter rep;
        auto macros = readShippedLibMacros(path, rep, ObjectFormatKind::MachO);
        ASSERT_TRUE(macros.has_value());
        EXPECT_FALSE(rep.hasErrors());
        ASSERT_EQ(macros->size(), 1u);
        EXPECT_EQ(macros->at(0).replacement, "macho_errno");
    }
    // nullopt format (a test caller / no target) → a variants-only macro is NOT
    // injected (no selection possible) — never an arbitrary pick.
    {
        DiagnosticReporter rep;
        auto macros = readShippedLibMacros(path, rep);   // nullopt activeFormat
        ASSERT_TRUE(macros.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_TRUE(macros->empty()) << "no active format → variants-only macro not injected";
    }
}

// AMBIGUOUS-MATCH PIN: two macro variants BOTH matching the active format →
// F_ShippedMacroVariantAmbiguous, never silently first-wins.
TEST(ShippedLibDescriptor, MacroVariantAmbiguousMatchFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "mamb.json", R"JSON({
        "header": "ma.h",
        "macros": [
            { "name": "X", "variants": [
                { "when": { "format": "elf" }, "replacement": "1" },
                { "when": { "format": "elf" }, "replacement": "2" }
            ] }
        ]
    })JSON");
    DiagnosticReporter rep;
    auto macros = readShippedLibMacros(path, rep, ObjectFormatKind::Elf);
    EXPECT_FALSE(macros.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedMacroVariantAmbiguous),
              1u);
}

// EAGER-DECODE PIN: a NON-active macro variant carries a directive-breaking newline
// in its replacement. Even compiling for the OTHER (active) format the read FAILS
// LOUD (every variant's body is decoded at read time).
TEST(ShippedLibDescriptor, MacroVariantEagerDecodeMalformedInactiveFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    // The inactive (macho) variant's replacement carries an embedded newline.
    auto const path = writeTemp(dir, "meager.json",
        "{ \"header\": \"me.h\", \"macros\": [ "
        "{ \"name\": \"X\", \"variants\": [ "
        "{ \"when\": { \"format\": \"elf\" },   \"replacement\": \"1\" }, "
        "{ \"when\": { \"format\": \"macho\" }, \"replacement\": \"1\\nint leaked=99;\" } "
        "] } ] }");
    DiagnosticReporter rep;
    // Compile for elf (its variant is fine); the INACTIVE macho variant's newline
    // still fails the whole read.
    auto macros = readShippedLibMacros(path, rep, ObjectFormatKind::Elf);
    EXPECT_FALSE(macros.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedLibDescriptorMalformed),
              1u);
}

// DECLARES-SOMETHING PIN: a macro-variants-only descriptor read under nullopt format
// (the AllShippedDescriptors / direct-API path) injects ZERO macros yet DECLARES a
// macro surface → NOT a false "declares nothing". RED-ON-DISABLE: drop the
// `declaredMacroVariants` term from the declares-something check → this read fails
// loud. Read via the SEMANTIC path (which is what enforces declares-something).
TEST(ShippedLibDescriptor, MacroVariantsOnlyDescriptorValidUnderNullopt) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "mvonly.json", R"JSON({
        "header": "mvonly.h",
        "macros": [
            { "name": "X", "variants": [
                { "when": { "format": "elf" },   "replacement": "1" },
                { "when": { "format": "macho" }, "replacement": "2" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);  // nullopt format
    ASSERT_TRUE(desc.has_value());        // declares a macro surface → NOT a no-op
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_TRUE(desc->macros.empty());    // no format → nothing injected
}

// FORMAT-ONLY PIN: a macro variant `when` may NOT carry `arch` (arch is not threaded
// into the preprocessor — c9 build-key avoidance). An `arch` key fails loud against
// the closed {format} vocabulary, never silently ignored.
TEST(ShippedLibDescriptor, MacroVariantArchKeyFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "march.json", R"JSON({
        "header": "mar.h",
        "macros": [
            { "name": "X", "variants": [
                { "when": { "arch": "x86_64", "format": "elf" }, "replacement": "1" }
            ] }
        ]
    })JSON");
    DiagnosticReporter rep;
    auto macros = readShippedLibMacros(path, rep, ObjectFormatKind::Elf);
    EXPECT_FALSE(macros.has_value());
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_GT(test_support::countCode(rep, DiagnosticCode::F_ShippedLibDescriptorMalformed),
              0u);
}

// A macro entry declaring BOTH a flat body AND `variants` is malformed (ambiguous
// intent) → fail loud.
TEST(ShippedLibDescriptor, MacroBothFlatBodyAndVariantsFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "mboth.json", R"JSON({
        "header": "mb.h",
        "macros": [
            { "name": "X", "replacement": "1",
              "variants": [ { "when": { "format": "elf" }, "replacement": "2" } ] }
        ]
    })JSON");
    DiagnosticReporter rep;
    auto macros = readShippedLibMacros(path, rep, ObjectFormatKind::Elf);
    EXPECT_FALSE(macros.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// NO-MATCH → NOT INJECTED: a macro whose only variant requires macho, compiled for
// elf → the macro is simply not injected (the read SUCCEEDS; the absence becomes
// loud at the use site if referenced). Sibling to the struct no-match pin.
TEST(ShippedLibDescriptor, MacroVariantNoMatchNotInjected) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "mnomatch.json", R"JSON({
        "header": "mn.h",
        "macros": [
            { "name": "MAC_ONLY", "variants": [
                { "when": { "format": "macho" }, "replacement": "1" }
            ] },
            { "name": "ALWAYS", "replacement": "7" }
        ]
    })JSON");
    DiagnosticReporter rep;
    auto macros = readShippedLibMacros(path, rep, ObjectFormatKind::Elf);
    ASSERT_TRUE(macros.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(macros->size(), 1u) << "macho-only macro not injected for elf; flat one stays";
    EXPECT_EQ(macros->at(0).name, "ALWAYS");
}

// ── c83: REAL <sys/time.h> `struct timeval` per-FORMAT layout pin ────────────
//
// D-FFI-MACHO-TIMEVAL-TV-USEC-WIDTH. Reads the SHIPPED sys/time.json (the real
// file, not an inline copy) so the pin goes red the moment the shipped macho
// variant drifts or is dropped. Darwin repeats the c15c stat SAME-SIZE trap:
// sizeof(struct timeval) == 16 on BOTH formats, so size alone cannot
// discriminate — the load-bearing divergence is tv_usec's WIDTH. glibc LP64
// suseconds_t is `long` (i64, field bytes 8..15 — one elf variant covers both
// shipped arches); Darwin's is 32-bit — xnu bsd/sys/_types.h `typedef __int32_t
// __darwin_suseconds_t`, declared in bsd/sys/_types/_timeval.h
// {__darwin_time_t tv_sec; __darwin_suseconds_t tv_usec} with tv_sec staying
// `long` (bsd/arm/_types.h + bsd/i386/_types.h) — so macho is {i64@0, i32@8}
// + 4 TRAILING pad bytes (payload 12 aligned up to the struct's 8-alignment).
// An i64 read of the macho field folds those undefined padding bytes into the
// high half (little-endian misread); an i64 write clobbers them. Consumers:
// gettimeofday (sqlite os_unix reads tv_usec) + utimes.
//
// Pins, for BOTH shipped arches (the format variants are arch-agnostic —
// glibc agrees across x86_64/arm64; Darwin's fields are fixed-width):
//   * elf:   {tv_sec i64@0, tv_usec I64@8}, sizeof 16.
//   * macho: {tv_sec i64@0, tv_usec I32@8}, sizeof 16 — the 4 trailing pad
//     bytes are PROVEN by size 16 with the 4-byte field ending at 12 (the
//     layout engine's final alignUp), the exact bytes an i64 field would claim.
// RED-ON-DISABLE: regress the shipped macho variant's tv_usec to i64 → the I32
// width assert fails; DELETE the macho variant → no variant matches for macho →
// the struct is not injected → the structs.size() assert fails; flatten the
// struct back to a single field list → the macho width assert fails. The
// runtime witness is the shipped_timeval_macho corpus on the macos-latest CI leg.
// ── TF-C90 (D-CSUBSET-SYS-TYPES-BSD-SPELLING-GROUP-ABSENT): the <sys/types.h>
//    BSD-COMPAT SPELLING GROUP, per OBJECT FORMAT ─────────────────────────────
//
// `u_char` / `u_short` / `u_int` / `u_long` / `quad_t` / `u_quad_t` / `caddr_t` /
// `fixpt_t` / `segsz_t` — the family every unix <sys/types.h> ships together and
// the WINDOWS one ships not at all. Each is declared with `variants` and NO flat
// `type`, so PER-ENTRY AVAILABILITY IS WHICH VARIANTS EXIST (0 matching variants
// ⇒ `selected == false` ⇒ the typedef is not injected — the `off64_t` mechanism).
//
// ✔MEASURED availability, per name, against three reference header sets rather
// than asserted: musl (emsdk sysroot sys/types.h:63-69) and bionic (Android NDK
// sysroot sys/types.h:54,136-139) declare the first seven; mingw-w64 x86_64
// sys/types.h declares NONE of the nine; the macOS SDK sys/types.h:84-128
// declares all nine. So the split pinned here is elf+macho for the seven,
// macho-ONLY for `fixpt_t`/`segsz_t` (absent from BOTH linux libcs), and NONE on
// pe.
//
// The assertions are TWO-SIDED on every axis, because a one-sided "is it there?"
// cannot see the defect that matters here — a name leaking onto a format that
// does not have it:
//   * PRESENT-side: the exact TypeKind, AND the vocabulary tag (`u_long` must be
//     the C `unsigned long`, and must NOT carry some other spelling — a bare
//     kind check cannot tell u64 from u64-"unsigned long long").
//   * ABSENT-side: `fixpt_t`/`segsz_t` must be MISSING on elf, and all nine
//     MISSING on pe.
//   * ANTI-VACUITY POSITIVE CONTROL: on the very same pe/elf reads, `mode_t`
//     (a flat, format-independent entry) must still be PRESENT. Without it, a
//     descriptor that failed to load — or a `typedefs` array that silently
//     decoded to empty — would satisfy every ABSENT assertion and the test would
//     pass while measuring nothing.
// RED-ON-DISABLE: add a `pe` variant to any of the nine → its pe ABSENT assert
// fails; add an `elf` variant to `fixpt_t`/`segsz_t` → their elf ABSENT assert
// fails; delete any elf/macho variant → that name's PRESENT assert fails;
// retag `u_long` (or drop its tag) → the vocabulary assert fails; swap a kind
// (u32→i32) → the kind assert fails. Runtime witnesses: the
// shipped_sys_types_bsd (elf+macho RUN) and shipped_sys_types_bsd_macho
// (macho RUN) corpora, plus the shipped_sys_types_bsd_absent_elf error manifest.
//
// ★ THIS TEST IS THE *ONLY* WITNESS FOR THE pe ABSENCE, ON PURPOSE, and the reason
// is a measured pair of blockers in the harness rather than anything about this
// group — recorded here because a reader will otherwise ask why the elf absence
// has a corpus twin and the pe absence does not:
//   (1) D-DIAG-PE-SPAN-LINE-MAPPING-SYNTHETIC-LINES — on the pe64 target a
//       source-spanned diagnostic's reported LINE is SHIFTED.
//       ✔MEASURED with a 4-line file (`int a; int b; nosuchtype_t c; int main…`):
//       pe reports source line 3 as 5, and as 8 once one `#include <stdio.h>` is
//       added, while elf and macho report 3 in both. The compiler renders the
//       CORRECT source text at the WRONG line, so only the line MAPPING is off.
//       Worse, the in-process examples runner and the CLI harness then disagree on
//       the COLUMN for the same diagnostic (31:11 vs 31:1) — there is no honest
//       line:col to put in a manifest.
//   (2) D-TEST-POSITIONED-FALSE-REQUIRES-SPANLESS-RENDERING — `positioned:false`
//       is not an escape hatch. The integrated CLI arm matches a
//       code-only expectation by grepping for the SYMBOLIC rendering
//       `error[S_UnknownType]`, and the CLI emits that spelling ONLY for
//       SPAN-LESS diagnostics — a spanned one renders `error[S0006]`
//       (D-DIAG-TWO-CODE-RENDERINGS). So `positioned:false` is today usable only
//       for genuinely span-less codes, an undocumented coupling.
// A pe corpus arm lands when those are fixed; until then the pe axis is pinned
// HERE, strictly, and it does red alone (add a pe variant to any of the nine).
TEST(ShippedLibDescriptor, RealSysTypesBsdSpellingGroupPerFormat) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "sys" / "types.json";

    // Read the REAL descriptor for one (arch, format) and hand the caller both
    // the interner and the decoded typedef list, so kind AND vocabulary tag can
    // be inspected structurally (never a string compare of the JSON).
    struct Read {
        TypeInterner                interner{CompilationUnitId{1}};
        TypeRegistry                typeReg;
        DiagnosticReporter          rep;
        std::optional<ShippedLibDescriptor> desc;
    };
    auto readFor = [&](std::string_view arch, ObjectFormatKind fmt,
                       Read& out) {
        out.desc = readShippedLibDescriptor(path, out.interner, out.typeReg,
                                            out.rep, DataModel::Lp64, arch, fmt);
        ASSERT_TRUE(out.desc.has_value())
            << "sys/types.json failed to load for arch=" << arch;
        ASSERT_FALSE(out.rep.hasErrors())
            << "sys/types.json emitted diagnostics for arch=" << arch;
    };
    auto findTypedef = [](Read const& r, std::string_view name) -> TypeId {
        for (auto const& td : r.desc->typedefs)
            if (td.name == name) return td.type;
        return {};
    };

    // The seven shared names, with the kind each MUST decode to, and the
    // vocabulary tag each MUST carry ("" = deliberately untagged).
    struct Shared { char const* name; TypeKind kind; char const* vocab; };
    static constexpr std::array<Shared, 7> kShared{{
        {"u_char",   TypeKind::U8,  ""},
        {"u_short",  TypeKind::U16, ""},
        {"u_int",    TypeKind::U32, ""},
        // The ONE tagged member: every reference libc spells u_long exactly
        // `unsigned long`, and the typedef name IS that spelling.
        {"u_long",   TypeKind::U64, "unsigned long"},
        // UNTAGGED on purpose: glibc spells quad_t `long int` while musl and
        // Darwin spell it `long long`, so a tag would be a one-sided claim.
        {"quad_t",   TypeKind::I64, ""},
        {"u_quad_t", TypeKind::U64, ""},
        {"caddr_t",  TypeKind::Ptr, ""},
    }};
    // BSD-only: present on macho, ABSENT on elf.
    struct BsdOnly { char const* name; TypeKind kind; };
    static constexpr std::array<BsdOnly, 2> kBsdOnly{{
        {"fixpt_t", TypeKind::U32},
        {"segsz_t", TypeKind::I32},
    }};

    for (std::string_view arch : {"x86_64", "arm64"}) {
        // ── macho: all nine PRESENT ──
        {
            Read m;
            ASSERT_NO_FATAL_FAILURE(readFor(arch, ObjectFormatKind::MachO, m));
            EXPECT_TRUE(findTypedef(m, "mode_t").valid())
                << "positive control: mode_t must be present (arch=" << arch << ")";
            for (auto const& s : kShared) {
                TypeId const t = findTypedef(m, s.name);
                ASSERT_TRUE(t.valid())
                    << s.name << " must be injected on macho (arch=" << arch << ")";
                EXPECT_EQ(m.interner.kind(t), s.kind) << s.name << " kind on macho";
                EXPECT_EQ(m.interner.vocabularyName(t), std::string_view{s.vocab})
                    << s.name << " vocabulary tag on macho";
            }
            // caddr_t is `char *`, not a bare pointer to anything.
            TypeId const ca = findTypedef(m, "caddr_t");
            ASSERT_TRUE(ca.valid());
            auto const caOps = m.interner.operands(ca);
            ASSERT_EQ(caOps.size(), 1u);
            EXPECT_EQ(m.interner.kind(caOps[0]), TypeKind::Char)
                << "caddr_t must be ptr<char>";
            for (auto const& b : kBsdOnly) {
                TypeId const t = findTypedef(m, b.name);
                ASSERT_TRUE(t.valid())
                    << b.name << " must be injected on macho (arch=" << arch << ")";
                EXPECT_EQ(m.interner.kind(t), b.kind) << b.name << " kind on macho";
            }
        }
        // ── elf: the seven PRESENT, the two BSD-only ABSENT ──
        {
            Read e;
            ASSERT_NO_FATAL_FAILURE(readFor(arch, ObjectFormatKind::Elf, e));
            EXPECT_TRUE(findTypedef(e, "mode_t").valid())
                << "positive control: mode_t must be present (arch=" << arch << ")";
            for (auto const& s : kShared) {
                TypeId const t = findTypedef(e, s.name);
                ASSERT_TRUE(t.valid())
                    << s.name << " must be injected on elf (arch=" << arch << ")";
                EXPECT_EQ(e.interner.kind(t), s.kind) << s.name << " kind on elf";
                EXPECT_EQ(e.interner.vocabularyName(t), std::string_view{s.vocab})
                    << s.name << " vocabulary tag on elf";
            }
            for (auto const& b : kBsdOnly)
                EXPECT_FALSE(findTypedef(e, b.name).valid())
                    << b.name << " is BSD-only (absent from glibc/musl/bionic) and "
                       "must NOT be injected on elf (arch=" << arch << ")";
        }
        // ── pe: ALL NINE ABSENT, header itself still usable ──
        {
            Read p;
            ASSERT_NO_FATAL_FAILURE(readFor(arch, ObjectFormatKind::Pe, p));
            EXPECT_TRUE(findTypedef(p, "mode_t").valid())
                << "positive control: mode_t must still be present on pe — without "
                   "it every ABSENT assert below would pass vacuously (arch="
                << arch << ")";
            for (auto const& s : kShared)
                EXPECT_FALSE(findTypedef(p, s.name).valid())
                    << s.name << " must NOT be injected on pe (mingw-w64 "
                       "<sys/types.h> declares none of this group) (arch=" << arch << ")";
            for (auto const& b : kBsdOnly)
                EXPECT_FALSE(findTypedef(p, b.name).valid())
                    << b.name << " must NOT be injected on pe (arch=" << arch << ")";
        }
    }
}

TEST(ShippedLibDescriptor, RealSysTimeTimevalPerFormatLayout) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "sys" / "time.json";

    auto checkFor = [&](std::string_view arch, ObjectFormatKind fmt,
                        TypeKind expectedUsecKind) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, arch, fmt);
        ASSERT_TRUE(desc.has_value()) << "arch=" << arch;
        EXPECT_FALSE(rep.hasErrors()) << "arch=" << arch;
        // TWO structs since itimerval landed (D-CSUBSET-DARWIN-BSD-STRUCT-BY-NAME):
        // timeval FIRST (source order is the dependency order — itimerval names it),
        // itimerval second. Still an exact count, not a floor: an entry that
        // silently stopped being injected must stay red here.
        ASSERT_EQ(desc->structs.size(), 2u)
            << "timeval/itimerval not both injected for arch=" << arch;
        auto const& tv = desc->structs[0];
        EXPECT_EQ(tv.name, "timeval");
        EXPECT_EQ(desc->structs[1].name, "itimerval");
        ASSERT_EQ(tv.fields.size(), 2u) << "arch=" << arch;
        EXPECT_EQ(tv.fields[0].name, "tv_sec");
        EXPECT_EQ(tv.fields[1].name, "tv_usec");
        EXPECT_EQ(interner.kind(tv.fields[0].type), TypeKind::I64)
            << "tv_sec must be i64 on every format (Darwin __darwin_time_t is long)";
        EXPECT_EQ(interner.kind(tv.fields[1].type), expectedUsecKind)
            << "tv_usec width wrong for arch=" << arch;
        auto layout = computeLayout(tv.typeId, interner, kNatural16, DataModel::Lp64);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->size, 16u);            // SAME size both formats (the trap)
        ASSERT_EQ(layout->fieldOffsets.size(), 2u);
        EXPECT_EQ(layout->fieldOffsets[0], 0u);  // tv_sec  @ 0
        EXPECT_EQ(layout->fieldOffsets[1], 8u);  // tv_usec @ 8
    };

    for (std::string_view arch : {"x86_64", "arm64"}) {
        checkFor(arch, ObjectFormatKind::Elf,   TypeKind::I64);
        checkFor(arch, ObjectFormatKind::MachO, TypeKind::I32);
    }
}

// REAL <sys/time.h> `struct itimerval` — the FIRST by-name composite consumer
// (D-CSUBSET-DARWIN-BSD-STRUCT-BY-NAME). xnu sys/proc.h's `struct extern_proc`
// embeds one BY VALUE, so while itimerval was missing DSS left extern_proc
// incomplete, which left kinfo_proc's kp_proc incomplete: 4 S0026 across sqlite
// test1.c + mem1.c from this ONE hole.
//
// The load-bearing assertion is IDENTITY: each member's type must be the very
// TypeId the `timeval` entry interned FOR THIS FORMAT, which is what makes the
// tv_usec width single-sourced — itimerval declares no `variants` and still
// comes out right on both. Layout is MEASURED natively (arm64 Darwin, offsetof):
// sizeof 32, it_interval@0, it_value@16; glibc LP64 agrees at 32/0/16 with a
// WIDER inner tv_usec, so a restated body would have had to get both right.
// RED-ON-DISABLE: drop the structs-loop name publish → `"type": "timeval"` stops
// resolving → sys/time.json fails to read at all.
TEST(ShippedLibDescriptor, RealSysTimeItimervalByNameComposite) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "sys" / "time.json";

    auto checkFor = [&](std::string_view arch, ObjectFormatKind fmt) {
        SCOPED_TRACE(std::string{arch} + "/" + std::string{objectFormatKindName(fmt)});
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, arch, fmt);
        ASSERT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        ASSERT_EQ(desc->structs.size(), 2u);
        auto const& tv = desc->structs[0];
        auto const& it = desc->structs[1];
        ASSERT_EQ(tv.name, "timeval");
        ASSERT_EQ(it.name, "itimerval");
        ASSERT_EQ(it.fields.size(), 2u);
        EXPECT_EQ(it.fields[0].name, "it_interval");
        EXPECT_EQ(it.fields[1].name, "it_value");
        // IDENTITY with the timeval entry of the SAME read — not just same shape.
        EXPECT_EQ(it.fields[0].type, tv.typeId)
            << "it_interval must BE the shipped timeval, not a look-alike";
        EXPECT_EQ(it.fields[1].type, tv.typeId);
        EXPECT_EQ(interner.kind(it.fields[0].type), TypeKind::Struct);
        auto layout = computeLayout(it.typeId, interner, kNatural16, DataModel::Lp64);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->size, 32u);            // MEASURED: sizeof(struct itimerval)
        ASSERT_EQ(layout->fieldOffsets.size(), 2u);
        EXPECT_EQ(layout->fieldOffsets[0], 0u);  // it_interval @ 0
        EXPECT_EQ(layout->fieldOffsets[1], 16u); // it_value    @ 16
    };

    for (std::string_view arch : {"x86_64", "arm64"}) {
        checkFor(arch, ObjectFormatKind::Elf);
        checkFor(arch, ObjectFormatKind::MachO);
    }
}

// REAL <sys/stat.h> macho `st_mtimespec` — the second by-name consumer
// (D-CSUBSET-DARWIN-BSD-STRUCT-BY-NAME). sqlite os_unix.c:7731 assigns the WHOLE
// member (`conchModTime = buf.st_mtimespec;`), so it must be a by-value composite,
// and it OVERLAYS the flat st_mtim_sec/st_mtim_nsec pair the st_mtime macro still
// maps onto — hence the c107 explicit-offset channel for this variant.
//
// The invariant that matters most: NOTHING MOVED. Every offset asserted here is
// the value natural derivation produced before the overlay landed, MEASURED
// natively (arm64 Darwin, offsetof) — and the total is still 144, the number the
// shipped_stat_macho corpus and libSystem's own fstat() agree on.
// The invariant that is NOT free: with all 25 offsets stated explicitly the
// variant stops DERIVING its layout, so a wrong field WIDTH moves no offset and
// changes no total — offsets and size alone are vacuous against it (MEASURED,
// see the width block). Hence the per-field width table below.
// RED-ON-DISABLE: drop st_mtimespec → the member lookup fails; mistype ONE
// offset → that assert (or the size) goes red; narrow ONE field's type →
// its kind/width assert goes red (MEASURED with st_uid u32 → u16, which is
// green against offsets+size alone).
TEST(ShippedLibDescriptor, RealSysStatMachoMtimespecOverlay) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const statPath = root / "sys" / "stat.json";

    // Index of a named field in a named struct, or npos-ish failure.
    auto indexOf = [](ShippedStruct const& s, std::string_view fname) -> std::size_t {
        for (std::size_t i = 0; i < s.fields.size(); ++i)
            if (s.fields[i].name == fname) return i;
        return static_cast<std::size_t>(-1);
    };
    auto structNamed = [](ShippedLibDescriptor const& d,
                          std::string_view sname) -> ShippedStruct const* {
        for (auto const& s : d.structs)
            if (s.name == sname) return &s;
        return nullptr;
    };
    auto readFor = [&](TypeInterner& interner, TypeRegistry& typeReg,
                       ObjectFormatKind fmt) {
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(statPath, interner, typeReg, rep,
                                             DataModel::Lp64,
                                             std::string_view{"x86_64"}, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        return desc;
    };

    {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = readFor(interner, typeReg, ObjectFormatKind::MachO);
        ASSERT_TRUE(desc.has_value());
        auto const* ts = structNamed(*desc, "timespec");
        auto const* st = structNamed(*desc, "stat");
        ASSERT_NE(ts, nullptr) << "sys/stat.json must declare timespec BEFORE stat";
        ASSERT_NE(st, nullptr);
        std::size_t const iSpec = indexOf(*st, "st_mtimespec");
        ASSERT_NE(iSpec, static_cast<std::size_t>(-1))
            << "the macho variant must carry st_mtimespec";
        // BY-NAME IDENTITY: the member IS the shipped timespec, so a whole-struct
        // assignment from `struct timespec` type-checks.
        EXPECT_EQ(st->fields[iSpec].type, ts->typeId);
        EXPECT_EQ(interner.kind(st->fields[iSpec].type), TypeKind::Struct);
        auto specLayout = computeLayout(ts->typeId, interner, kNatural16,
                                        DataModel::Lp64);
        ASSERT_TRUE(specLayout.has_value());
        EXPECT_EQ(specLayout->size, 16u);   // {tv_sec i64, tv_nsec i64}

        auto layout = computeLayout(st->typeId, interner, kNatural16, DataModel::Lp64);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->size, 144u) << "Darwin struct stat is 144 bytes — UNCHANGED";
        auto offOf = [&](std::string_view fname) -> std::uint64_t {
            std::size_t const i = indexOf(*st, fname);
            EXPECT_NE(i, static_cast<std::size_t>(-1)) << fname;
            if (i == static_cast<std::size_t>(-1) || i >= layout->fieldOffsets.size())
                return static_cast<std::uint64_t>(-1);
            return layout->fieldOffsets[i];
        };
        // THE OVERLAY: three members, the same 16 bytes (Darwin's own
        // `#define st_mtime st_mtimespec.tv_sec`, modeled honestly).
        EXPECT_EQ(offOf("st_mtimespec"), 48u);
        EXPECT_EQ(offOf("st_mtim_sec"),  48u);
        EXPECT_EQ(offOf("st_mtim_nsec"), 56u);
        // NOTHING MOVED — the pre-existing offsets, both sides of the overlay.
        EXPECT_EQ(offOf("st_dev"), 0u);
        EXPECT_EQ(offOf("st_mode"), 4u);
        EXPECT_EQ(offOf("st_nlink"), 6u);
        EXPECT_EQ(offOf("st_ino"), 8u);
        EXPECT_EQ(offOf("st_uid"), 16u);
        EXPECT_EQ(offOf("st_gid"), 20u);
        EXPECT_EQ(offOf("st_rdev"), 24u);
        EXPECT_EQ(offOf("st_atim_sec"), 32u);
        EXPECT_EQ(offOf("st_ctim_sec"), 64u);
        EXPECT_EQ(offOf("st_birthtim_sec"), 80u);
        EXPECT_EQ(offOf("st_size"), 96u);        // the corpus's own witness
        EXPECT_EQ(offOf("st_blocks"), 104u);
        EXPECT_EQ(offOf("st_blksize"), 112u);
        EXPECT_EQ(offOf("st_qspare1"), 136u);

        // ★★ WIDTHS — EVERY FIELD, because the EXPLICIT-OFFSET channel makes the
        // offset pins above BLIND to a width slip. MEASURED, not inferred: with
        // `st_uid` narrowed u32 → u16 in a copy of this descriptor, every
        // assertion above stayed GREEN. It has to: the offsets are read verbatim
        // from the config so nothing moves, and the explicit arm's size is
        // `align.alignUp(max field extent)` = alignUp(136 + 8) = 144 no matter
        // what the other 24 fields are — narrowing even the LAST field keeps 144
        // (alignUp(136 + 2) is still 144). A derived-offset variant self-corrects
        // and the offset pins catch it; this one does not, so the width is only
        // pinned where it is written down. Pinned by TypeKind, which fixes
        // SIGNEDNESS too — a u32 → i32 swap is a different miscompile at the same
        // 4 bytes — AND by the laid-out byte size, which is what the composite
        // overlay member needs (`st_mtimespec` is a struct, not a scalar kind).
        // Every width MEASURED natively (arm64 Darwin, `sizeof(((struct stat*)0)
        // ->f)`, EVERY row): 4/2/2/8/4/4/4 then the four 16-byte timespecs, then
        // 8/8/4/4/4/4 and the 16-byte `st_qspare` — which is Darwin's
        // `int64_t[2]`, modeled here as the two i64 rows qspare0/qspare1.
        struct FieldWidth { char const* name; TypeKind kind; std::uint64_t bytes; };
        static constexpr std::array<FieldWidth, 25> kMachoStatWidths{{
            {"st_dev",           TypeKind::I32,    4},
            {"st_mode",          TypeKind::U16,    2},
            {"st_nlink",         TypeKind::U16,    2},
            {"st_ino",           TypeKind::U64,    8},
            {"st_uid",           TypeKind::U32,    4},
            {"st_gid",           TypeKind::U32,    4},
            {"st_rdev",          TypeKind::I32,    4},
            {"__pad0",           TypeKind::I32,    4},
            {"st_atim_sec",      TypeKind::I64,    8},
            {"st_atim_nsec",     TypeKind::I64,    8},
            // THE OVERLAY TRIO: the whole `struct timespec` and the two flat i64
            // halves it overlays must keep the SAME total 16 bytes at 48 — a
            // narrowed half would leave the composite reading bytes no flat
            // member names, which is exactly the by-name/flat divergence
            // D-CSUBSET-DARWIN-BSD-STRUCT-BY-NAME exists to keep honest.
            {"st_mtim_sec",      TypeKind::I64,    8},
            {"st_mtim_nsec",     TypeKind::I64,    8},
            {"st_mtimespec",     TypeKind::Struct, 16},
            {"st_ctim_sec",      TypeKind::I64,    8},
            {"st_ctim_nsec",     TypeKind::I64,    8},
            {"st_birthtim_sec",  TypeKind::I64,    8},
            {"st_birthtim_nsec", TypeKind::I64,    8},
            {"st_size",          TypeKind::I64,    8},
            {"st_blocks",        TypeKind::I64,    8},
            {"st_blksize",       TypeKind::I32,    4},
            {"st_flags",         TypeKind::U32,    4},
            {"st_gen",           TypeKind::U32,    4},
            {"st_lspare",        TypeKind::I32,    4},
            {"st_qspare0",       TypeKind::I64,    8},
            {"st_qspare1",       TypeKind::I64,    8},
        }};
        // EXHAUSTIVE by construction: the table covers the variant field-for-field,
        // so a field ADDED to this explicit-offset variant without a width pin (and
        // without the offset every field here must state) fails HERE rather than
        // slipping in unpinned.
        EXPECT_EQ(st->fields.size(), kMachoStatWidths.size())
            << "the macho struct stat variant has 25 explicitly-offset fields; a "
               "new one needs its offset AND its width pinned in this table";
        for (auto const& w : kMachoStatWidths) {
            std::size_t const i = indexOf(*st, w.name);
            ASSERT_NE(i, static_cast<std::size_t>(-1)) << w.name << " is missing";
            TypeId const ft = st->fields[i].type;
            ASSERT_TRUE(ft.valid()) << w.name;
            EXPECT_EQ(interner.kind(ft), w.kind) << w.name << " kind/signedness";
            auto const fl = computeLayout(ft, interner, kNatural16, DataModel::Lp64);
            ASSERT_TRUE(fl.has_value()) << w.name;
            EXPECT_EQ(fl->size, w.bytes) << w.name << " WIDTH — an explicit-offset "
                                            "variant cannot self-correct this";
        }
    }
    // ABSENT elsewhere: st_mtimespec is a Darwin member. elf keeps the glibc
    // flattening, pe the MSVC record — neither may grow it.
    for (ObjectFormatKind const fmt : {ObjectFormatKind::Elf, ObjectFormatKind::Pe}) {
        SCOPED_TRACE(std::string{objectFormatKindName(fmt)});
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = readFor(interner, typeReg, fmt);
        ASSERT_TRUE(desc.has_value());
        auto const* st = structNamed(*desc, "stat");
        ASSERT_NE(st, nullptr) << "struct stat must still be injected here";
        EXPECT_EQ(indexOf(*st, "st_mtimespec"), static_cast<std::size_t>(-1))
            << "st_mtimespec must stay macho-only";
        // The timespec row is FLAT on purpose: a format-gated dependency would
        // make the whole `stat` entry unavailable here (the gate skips a
        // dependent whose by-name referent is not selected).
        EXPECT_NE(structNamed(*desc, "timespec"), nullptr);
    }
}

// ── c117 (the macho shell.c POSIX-header batch: pwd/dirent/resource) ─────────
// Each reads the REAL shipped descriptor per format + computes the layout the
// MIR engine uses; red if the macho variant regresses to the elf layout or is
// dropped. The Darwin layouts are verified against the macOS SDK (arm64).

// struct passwd DIVERGES: Darwin has 10 fields (pw_change + pw_class after
// pw_gid, pw_expire at the tail) vs glibc's 7, pushing the shell.c-read pw_dir
// from @32 (elf) to @48 (macho) — a single layout silently misreads the home dir.
TEST(ShippedLibDescriptor, RealPwdPerFormatPasswdLayout) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "pwd.json";
    auto layoutFor = [&](ObjectFormatKind fmt) -> std::optional<StructLayout> {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64,
                                             std::string_view{"arm64"}, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->structs.size(), 1u);
        return computeLayout(desc->structs[0].typeId, interner, kNatural16,
                             DataModel::Lp64);
    };
    auto elf = layoutFor(ObjectFormatKind::Elf);
    auto macho = layoutFor(ObjectFormatKind::MachO);
    ASSERT_TRUE(elf.has_value());
    ASSERT_TRUE(macho.has_value());
    EXPECT_EQ(elf->size, 48u);              // glibc struct passwd (7 fields)
    EXPECT_EQ(macho->size, 72u);            // Darwin struct passwd (10 fields)
    EXPECT_EQ(elf->fieldOffsets[5], 32u);   // pw_dir (index 5 on elf)  @ 32
    EXPECT_EQ(macho->fieldOffsets[7], 48u); // pw_dir (index 7 on macho) @ 48
}

// struct dirent DIVERGES: Darwin's 64-bit-inode form (d_seekoff + d_namlen,
// d_name[1024]) is 1048 bytes with d_name @ 21 vs glibc's 280 / d_name @ 19 —
// shell.c reads d_name, so a single layout reads the wrong bytes.
TEST(ShippedLibDescriptor, RealDirentPerFormatDirentLayout) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "dirent.json";
    auto layoutFor = [&](ObjectFormatKind fmt) -> std::optional<StructLayout> {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64,
                                             std::string_view{"arm64"}, fmt);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_EQ(desc->structs.size(), 1u);
        return computeLayout(desc->structs[0].typeId, interner, kNatural16,
                             DataModel::Lp64);
    };
    auto elf = layoutFor(ObjectFormatKind::Elf);
    auto macho = layoutFor(ObjectFormatKind::MachO);
    ASSERT_TRUE(elf.has_value());
    ASSERT_TRUE(macho.has_value());
    EXPECT_EQ(elf->size, 280u);             // glibc struct dirent
    EXPECT_EQ(macho->size, 1048u);          // Darwin 64-bit-inode struct dirent
    EXPECT_EQ(elf->fieldOffsets[4], 19u);   // d_name (index 4 on elf)  @ 19
    EXPECT_EQ(macho->fieldOffsets[5], 21u); // d_name (index 5 on macho) @ 21
}

// struct rusage keeps the SAME 144-byte size + BSD field order on Darwin, but
// its embedded struct timeval's tv_usec is i32 (Darwin __darwin_suseconds_t)
// vs glibc's i64 — the same-size-swap trap (shell.c .timer reads tv_usec).
TEST(ShippedLibDescriptor, RealResourcePerFormatRusageTimeval) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "sys" / "resource.json";
    auto checkFor = [&](ObjectFormatKind fmt, TypeKind expectedUsecKind) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64,
                                             std::string_view{"arm64"}, fmt);
        ASSERT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        ASSERT_EQ(desc->structs.size(), 2u);   // timeval + rusage
        auto const& tv = desc->structs[0];
        EXPECT_EQ(tv.name, "timeval");
        ASSERT_EQ(tv.fields.size(), 2u);
        EXPECT_EQ(tv.fields[1].name, "tv_usec");
        EXPECT_EQ(interner.kind(tv.fields[1].type), expectedUsecKind)
            << "tv_usec width wrong";
        auto const& ru = desc->structs[1];
        EXPECT_EQ(ru.name, "rusage");
        auto ruLayout = computeLayout(ru.typeId, interner, kNatural16,
                                      DataModel::Lp64);
        ASSERT_TRUE(ruLayout.has_value());
        EXPECT_EQ(ruLayout->size, 144u);   // SAME size both formats (the trap)
    };
    checkFor(ObjectFormatKind::Elf,   TypeKind::I64);
    checkFor(ObjectFormatKind::MachO, TypeKind::I32);
}

// ── c106 (the shell.c pe header/descriptor batch) ──────────────────────────

// Decode a REAL shipped descriptor for one format (the RealTimeStructTm idiom).
static std::optional<ShippedLibDescriptor> decodeShippedFor(
    fs::path const& p, TypeInterner& interner, TypeRegistry& typeReg,
    ObjectFormatKind fmt) {
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(p, interner, typeReg, rep,
                                         DataModel::Lp64,
                                         std::string_view{"x86_64"}, fmt);
    EXPECT_TRUE(desc.has_value()) << p.generic_string();
    EXPECT_FALSE(rep.hasErrors()) << p.generic_string();
    return desc;
}

// c106 (D-FFI-STDDEF-WCHAR-PE-WIDTH, closing): wchar_t is 2 bytes on pe (the
// Windows UTF-16 code unit) and 4 bytes on elf/macho (the POSIX width). A
// wrong width mis-sizes EVERY `wchar_t buf[N]` and every wide-string object
// the Windows shell path touches — a silent-overlay class, so the widths are
// pinned from the REAL stddef.json through the REAL layout engine.
// RED-ON-DISABLE: drop the pe variant → wchar_t decodes at the elf i32 → the
// pe width assert fails.
TEST(ShippedLibDescriptor, RealStddefWcharPerFormatWidth) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    auto widthFor = [&](ObjectFormatKind fmt) -> std::uint64_t {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = decodeShippedFor(root / "stddef.json", interner, typeReg, fmt);
        if (!desc) return 0;
        for (auto const& td : desc->typedefs) {
            if (td.name == "wchar_t") {
                auto layout = computeLayout(td.type, interner, kNatural16,
                                            DataModel::Lp64);
                EXPECT_TRUE(layout.has_value());
                return layout ? layout->size : 0;
            }
        }
        ADD_FAILURE() << "wchar_t typedef absent from stddef.json";
        return 0;
    };
    EXPECT_EQ(widthFor(ObjectFormatKind::Pe), 2u)
        << "pe wchar_t is the 16-bit Windows code unit";
    EXPECT_EQ(widthFor(ObjectFormatKind::Elf), 4u);
    EXPECT_EQ(widthFor(ObjectFormatKind::MachO), 4u);
}

// c113 (D-CSUBSET-INTRINSIC-BARRIER): the shipped <intrin.h> descriptor.
// Three load-bearing properties of the REAL file:
//   (1) pe-ONLY — an MSVC compiler-intrinsic header is meaningless on
//       elf/macho (the header-level availability gate rejects the include
//       there with F_ShippedHeaderUnavailableForTarget).
//   (2) NO `symbols` — EMPIRICALLY load-bearing: every descriptor symbol is
//       EAGER-imported, and msvcrt.dll exports NO compiler intrinsic (a c113
//       draft declaring _byteswap_* as symbols crashed the loader with
//       STATUS_ENTRYPOINT_NOT_FOUND 0xC0000139 — the windows.json
//       InterlockedCompareExchange trap, twice-proven). The intrinsics are
//       always-on BUILTINS (c-subset.lang.json), never descriptor symbols.
//   (3) the honest non-empty payload = the size_t→u64 typedef (MSVC's real
//       intrin.h makes size_t visible; the string/stdio.json convention).
// RED-on-disable: widen the gate / re-add a symbol / drop the typedef.
TEST(ShippedLibDescriptor, RealIntrinHeaderIsPeOnlyAndCarriesNoEagerSymbols) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    auto desc = decodeShippedFor(root / "intrin.json", interner, typeReg,
                                 ObjectFormatKind::Pe);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->header, "intrin.h");
    // (1) the header-level gate is exactly ["pe"].
    ASSERT_EQ(desc->availableObjectFormats.size(), 1u);
    EXPECT_EQ(desc->availableObjectFormats[0], "pe");
    EXPECT_TRUE(objectFormatInAvailabilitySet(desc->availableObjectFormats,
                                              ObjectFormatKind::Pe));
    EXPECT_FALSE(objectFormatInAvailabilitySet(desc->availableObjectFormats,
                                               ObjectFormatKind::Elf));
    EXPECT_FALSE(objectFormatInAvailabilitySet(desc->availableObjectFormats,
                                               ObjectFormatKind::MachO));
    // (2) no eager-import surface — a compiler-intrinsic header must never
    //     declare linkable symbols (the 0xC0000139 loader trap).
    EXPECT_TRUE(desc->symbols.empty())
        << "intrin.h intrinsics are builtins, NOT descriptor symbols — a "
           "symbols entry here eager-imports a non-export and crashes the "
           "pe loader (STATUS_ENTRYPOINT_NOT_FOUND)";
    // (3) the size_t typedef is the non-empty payload, u64 on pe64/LLP64.
    ASSERT_EQ(desc->typedefs.size(), 1u);
    EXPECT_EQ(desc->typedefs[0].name, "size_t");
    auto layout = computeLayout(desc->typedefs[0].type, interner, kNatural16,
                                DataModel::Llp64);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->size, 8u);
}

// c106: the MSVC stat records. `struct _stat64`/`__stat64` are the ucrt
// 56-byte time64 shape — st_size at 24, st_mtime at 40 (natural alignment
// inserts 2B after gid and 4B before the i64 size). The time32 `struct _stat`
// (the shape behind msvcrt.dll's DIRECT `_wstat` export) is 36 bytes with
// st_size at 20. A wrong offset silently reads garbage file sizes/mtimes on
// the Windows shell path (the sqlite .stats/.import machinery), so both
// layouts pin through the real layout engine. The elf arm asserts ABSENCE:
// these tags are pe-variant-only (a POSIX build must not grow MSVC records).
// RED-ON-DISABLE: drop the pe variant (or reorder fields) → size/offset red.
TEST(ShippedLibDescriptor, RealSysStatMsvcRecordLayouts) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    fs::path const statPath = root / "sys" / "stat.json";
    {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = decodeShippedFor(statPath, interner, typeReg,
                                     ObjectFormatKind::Pe);
        ASSERT_TRUE(desc.has_value());
        bool saw64 = false, saw32 = false;
        for (auto const& s : desc->structs) {
            if (s.name == "_stat64" || s.name == "__stat64") {
                auto layout = computeLayout(s.typeId, interner, kNatural16,
                                            DataModel::Lp64);
                ASSERT_TRUE(layout.has_value()) << s.name;
                EXPECT_EQ(layout->size, 56u) << s.name;
                ASSERT_EQ(layout->fieldOffsets.size(), 11u) << s.name;
                EXPECT_EQ(layout->fieldOffsets[7], 24u) << s.name << " st_size";
                EXPECT_EQ(layout->fieldOffsets[9], 40u) << s.name << " st_mtime";
                saw64 = true;
            }
            if (s.name == "_stat") {
                auto layout = computeLayout(s.typeId, interner, kNatural16,
                                            DataModel::Lp64);
                ASSERT_TRUE(layout.has_value());
                // The x64 _wstat export writes the _stat64i32 shape — TIME64,
                // size32 — 48 bytes (c106-audit runtime-probed msvcrt.dll). A
                // 36B time32 _stat overran the caller by 12B and mis-read the
                // times. st_size stays a 32-bit field @20; the i64 times land
                // at 24/32/40.
                EXPECT_EQ(layout->size, 48u) << "_stat is the x64 _stat64i32 shape";
                ASSERT_EQ(layout->fieldOffsets.size(), 11u);
                EXPECT_EQ(layout->fieldOffsets[7], 20u) << "_stat st_size";
                EXPECT_EQ(layout->fieldOffsets[8], 24u) << "_stat st_atime (i64)";
                EXPECT_EQ(layout->fieldOffsets[9], 32u) << "_stat st_mtime (i64)";
                saw32 = true;
            }
        }
        EXPECT_TRUE(saw64) << "pe must ship _stat64/__stat64";
        EXPECT_TRUE(saw32) << "pe must ship the time32 _stat";
    }
    {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = decodeShippedFor(statPath, interner, typeReg,
                                     ObjectFormatKind::Elf);
        ASSERT_TRUE(desc.has_value());
        for (auto const& s : desc->structs) {
            EXPECT_NE(s.name, "_stat64") << "MSVC records must not leak onto elf";
            EXPECT_NE(s.name, "_stat")   << "MSVC records must not leak onto elf";
        }
    }
}

// c106: struct _wfinddata_t is the x64 msvcrt _wfinddata64i32_t record (the ABI
// of the DIRECT _wfindfirst/_wfindnext exports — c106-audit runtime-probed
// msvcrt.dll: TIME64, not time32; the "legacy names = time32" lore is x86-32
// only). 560 bytes: {attrib u32@0, [pad4], time i64@8/16/24, size u32@32,
// name wchar[260]@36}. The windirent shim copies data.name at @36; a time32
// (540B, name@20) descriptor read attribute bytes as UTF-16 and overran the
// shim's stack object by 16B. RED-ON-DISABLE: retype a time field i64→i32 →
// name shifts off 36 → offset red.
TEST(ShippedLibDescriptor, RealIoWfinddata64i32Layout) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    auto desc = decodeShippedFor(root / "io.json", interner, typeReg,
                                 ObjectFormatKind::Pe);
    ASSERT_TRUE(desc.has_value());
    bool saw = false;
    for (auto const& s : desc->structs) {
        if (s.name != "_wfinddata_t") continue;
        auto layout = computeLayout(s.typeId, interner, kNatural16,
                                    DataModel::Lp64);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->size, 560u);
        ASSERT_EQ(layout->fieldOffsets.size(), 6u);
        EXPECT_EQ(layout->fieldOffsets[1], 8u)   << "time_create (i64) @ 8";
        EXPECT_EQ(layout->fieldOffsets[4], 32u)  << "size @ 32";
        EXPECT_EQ(layout->fieldOffsets[5], 36u)  << "name (wchar[260]) @ 36";
        saw = true;
    }
    EXPECT_TRUE(saw) << "_wfinddata_t absent from io.json on pe";
}

// c106 (audit MEDIUM): the windows.json records that kernel32 WRITES and the
// program READS — WIN32_FIND_DATAW (592B, cFileName@44), SYSTEMTIME (16B),
// CONSOLE_SCREEN_BUFFER_INFO (22B), COORD (4B), SMALL_RECT (8B). All SDK
// 10.0.26100.0-verified; pinned so a field-order/type drift can't silently
// mis-place a member kernel32 fills in (the same silent class as the stat/find
// records). RED-ON-DISABLE: drop a WIN32_FIND_DATAW reserved field → cFileName
// shifts off 44 → red.
TEST(ShippedLibDescriptor, RealWindowsFindDataAndConsoleLayouts) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    auto desc = decodeShippedFor(root / "windows.json", interner, typeReg,
                                 ObjectFormatKind::Pe);
    ASSERT_TRUE(desc.has_value());
    auto sizeOffOf = [&](std::string_view name)
        -> std::optional<StructLayout> {
        for (auto const& s : desc->structs)
            if (s.name == name)
                return computeLayout(s.typeId, interner, kNatural16,
                                     DataModel::Lp64);
        return std::nullopt;
    };
    auto fd = sizeOffOf("WIN32_FIND_DATAW");
    ASSERT_TRUE(fd.has_value());
    EXPECT_EQ(fd->size, 592u);
    ASSERT_EQ(fd->fieldOffsets.size(), 10u);
    EXPECT_EQ(fd->fieldOffsets[8], 44u) << "cFileName @ 44";
    auto st = sizeOffOf("SYSTEMTIME");
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->size, 16u);
    auto csbi = sizeOffOf("CONSOLE_SCREEN_BUFFER_INFO");
    ASSERT_TRUE(csbi.has_value());
    EXPECT_EQ(csbi->size, 22u);
    ASSERT_EQ(csbi->fieldOffsets.size(), 5u);
    EXPECT_EQ(csbi->fieldOffsets[2], 8u)  << "wAttributes @ 8";
    EXPECT_EQ(csbi->fieldOffsets[3], 10u) << "srWindow @ 10";
    auto co = sizeOffOf("COORD");
    ASSERT_TRUE(co.has_value());
    EXPECT_EQ(co->size, 4u);
    auto sr = sizeOffOf("SMALL_RECT");
    ASSERT_TRUE(sr.has_value());
    EXPECT_EQ(sr->size, 8u);
}

// c106: the strtoll SPLIT — msvcrt.dll does not export strtoll (pre-C99 CRT);
// on pe `strtoll` is a MACRO onto the real _strtoi64 export while the
// [elf,macho]-gated strtoll SYMBOL stays un-injected; on elf the inverse.
// A drift in either direction is a loader break (importing a phantom strtoll
// on pe → 0xC0000139) or a broken elf build (losing the real symbol), so BOTH
// sides of BOTH formats pin.
TEST(ShippedLibDescriptor, RealStdlibStrtollPeMacroSplit) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    // Macro VARIANTS select at decode (flat result per format); symbol
    // availability filters at semantic INJECTION — so the symbol side pins
    // the per-symbol gate through the SAME predicate the injector applies
    // (objectFormatInAvailabilitySet), never mere presence in the vector.
    auto scan = [&](ObjectFormatKind fmt, bool& macroStrtoll,
                    bool& symStrtoll, bool& symStrtoi64) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = decodeShippedFor(root / "stdlib.json", interner, typeReg, fmt);
        ASSERT_TRUE(desc.has_value());
        macroStrtoll = symStrtoll = symStrtoi64 = false;
        for (auto const& m : desc->macros)
            if (m.name == "strtoll") {
                macroStrtoll = true;
                EXPECT_EQ(m.replacement, "_strtoi64");
            }
        for (auto const& s : desc->symbols) {
            if (s.name == "strtoll")
                symStrtoll = objectFormatInAvailabilitySet(
                    s.availableObjectFormats, fmt);
            if (s.name == "_strtoi64")
                symStrtoi64 = objectFormatInAvailabilitySet(
                    s.availableObjectFormats, fmt);
        }
    };
    bool m = false, s = false, s64 = false;
    scan(ObjectFormatKind::Pe, m, s, s64);
    EXPECT_TRUE(m)   << "pe strtoll must be the _strtoi64 macro";
    EXPECT_FALSE(s)  << "a pe strtoll IMPORT is a phantom (msvcrt has none)";
    EXPECT_TRUE(s64) << "pe must import the real _strtoi64";
    scan(ObjectFormatKind::Elf, m, s, s64);
    EXPECT_FALSE(m)  << "elf strtoll is the real symbol, not a macro";
    EXPECT_TRUE(s);
    EXPECT_FALSE(s64) << "_strtoi64 is pe-gated";
}

// c106: the glibc timespec-flattening macros (st_atime -> st_atim_sec …) must
// stay OFF pe — flat, they rewrote every pe st_atime member access into a
// nonexistent st_atim_sec field (the c106 probe's phantom). elf keeps them.
// Also pins the pe errno accessor split (_errno on pe; __errno_location
// stays elf-only — importing the wrong accessor is a loader break).
TEST(ShippedLibDescriptor, RealStatTimeMacrosAndErrnoAccessorPerFormat) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    auto statMacroNames = [&](ObjectFormatKind fmt) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = decodeShippedFor(root / "sys" / "stat.json", interner,
                                     typeReg, fmt);
        std::vector<std::string> names;
        if (desc)
            for (auto const& m : desc->macros) names.push_back(m.name);
        return names;
    };
    auto const peNames = statMacroNames(ObjectFormatKind::Pe);
    for (auto const& n : peNames)
        EXPECT_TRUE(n != "st_atime" && n != "st_mtime" && n != "st_ctime")
            << n << " must not rewrite pe member accesses";
    auto const elfNames = statMacroNames(ObjectFormatKind::Elf);
    bool elfHasStAtime = false;
    for (auto const& n : elfNames)
        if (n == "st_atime") elfHasStAtime = true;
    EXPECT_TRUE(elfHasStAtime)
        << "elf keeps the glibc st_atime flattening macro";

    auto errnoAccessors = [&](ObjectFormatKind fmt, bool& peAcc, bool& elfAcc) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = decodeShippedFor(root / "errno.json", interner, typeReg, fmt);
        ASSERT_TRUE(desc.has_value());
        peAcc = elfAcc = false;
        // Injection-availability, not vector presence (the per-symbol gate
        // filters at semantic injection, decode keeps every row).
        for (auto const& s : desc->symbols) {
            if (s.name == "_errno")
                peAcc = objectFormatInAvailabilitySet(
                    s.availableObjectFormats, fmt);
            if (s.name == "__errno_location")
                elfAcc = objectFormatInAvailabilitySet(
                    s.availableObjectFormats, fmt);
        }
    };
    bool pe = false, el = false;
    errnoAccessors(ObjectFormatKind::Pe, pe, el);
    EXPECT_TRUE(pe)  << "pe errno accessor is msvcrt _errno";
    EXPECT_FALSE(el) << "__errno_location on pe is a phantom import";
    errnoAccessors(ObjectFormatKind::Elf, pe, el);
    EXPECT_FALSE(pe);
    EXPECT_TRUE(el);
}

// SQLite testfixture test_syscall.c: errno.json ships ENOMEM + EDEADLK as REAL
// constants the test_syscall.c error-name map consumes. ENOMEM AGREES across all
// three formats (12 — flat, in the low block); EDEADLK DIVERGES (elf 35 / macho
// 11 / pe 36 — Linux asm-generic/errno.h vs Darwin sys/errno.h vs ucrt errno.h),
// so it carries per-format `variants` that must decode to the ACTIVE format's
// number. RED-ON-DISABLE: remove ENOMEM (or either EDEADLK variant), or perturb a
// value, and the matching per-format EXPECT below fails — a wrong errno number is
// a silent interop miscompile in the test corpus.
TEST(ShippedLibDescriptor, RealErrnoEnomemEdeadlkPerFormat) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    auto constFor = [&](ObjectFormatKind fmt, char const* name,
                        std::int64_t& valueOut, TypeKind& kindOut) -> bool {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        auto desc = decodeShippedFor(root / "errno.json", interner, typeReg, fmt);
        if (!desc) return false;
        for (auto const& c : desc->constants) {
            if (c.name == name) {
                valueOut = c.value;
                kindOut  = interner.kind(c.type);
                return true;
            }
        }
        return false;
    };
    struct Case { ObjectFormatKind fmt; char const* name; std::int64_t edeadlk; };
    Case const cases[] = {
        { ObjectFormatKind::Elf,   "elf",   35 },
        { ObjectFormatKind::MachO, "macho", 11 },
        { ObjectFormatKind::Pe,    "pe",    36 },
    };
    for (auto const& tc : cases) {
        std::int64_t v = -1;
        TypeKind     k = TypeKind::Void;
        // ENOMEM: flat, 12 on every format.
        ASSERT_TRUE(constFor(tc.fmt, "ENOMEM", v, k))
            << "ENOMEM missing for format " << tc.name;
        EXPECT_EQ(v, 12) << "ENOMEM for " << tc.name;
        EXPECT_EQ(k, TypeKind::I32) << "ENOMEM type for " << tc.name;
        // EDEADLK: per-format variant.
        v = -1;
        k = TypeKind::Void;
        ASSERT_TRUE(constFor(tc.fmt, "EDEADLK", v, k))
            << "EDEADLK missing for format " << tc.name;
        EXPECT_EQ(v, tc.edeadlk) << "EDEADLK for " << tc.name;
        EXPECT_EQ(k, TypeKind::I32) << "EDEADLK type for " << tc.name;
    }
}

// c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): windows.json models ULARGE_INTEGER as an
// explicit-offset OVERLAP struct {QuadPart u64@0, LowPart u32@0, HighPart u32@4} —
// the FILETIME→time idiom (shell.c writes the two u32 halves, reads the u64 whole).
// The layout engine must place the members at their DECLARED offsets (overlapping),
// giving size 8, not the 16 a naturally-derived {u64,u32,u32} would produce.
// RED-ON-DISABLE: drop HighPart's `@4` (or the whole offsets set) → the derive path
// lays QuadPart@0/LowPart@8/HighPart@12 → size 16, fieldOffsets != {0,0,4}.
TEST(ShippedLibDescriptor, RealWindowsUlargeOverlayLayout) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    auto desc = decodeShippedFor(root / "windows.json", interner, typeReg,
                                 ObjectFormatKind::Pe);
    ASSERT_TRUE(desc.has_value());
    bool saw = false;
    for (auto const& s : desc->structs) {
        if (s.name != "ULARGE_INTEGER") continue;
        EXPECT_TRUE(interner.hasExplicitOffsets(s.typeId))
            << "ULARGE_INTEGER must carry explicit offsets";
        auto layout = computeLayout(s.typeId, interner, kNatural16,
                                    DataModel::Lp64);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->size, 8u) << "overlap → 8 bytes, not 16";
        ASSERT_EQ(layout->fieldOffsets.size(), 3u);
        EXPECT_EQ(layout->fieldOffsets[0], 0u) << "QuadPart @ 0";
        EXPECT_EQ(layout->fieldOffsets[1], 0u) << "LowPart @ 0 (overlays QuadPart low)";
        EXPECT_EQ(layout->fieldOffsets[2], 4u) << "HighPart @ 4 (overlays QuadPart high)";
        saw = true;
    }
    EXPECT_TRUE(saw) << "ULARGE_INTEGER absent from windows.json structs on pe";
}

// ── FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): the pe64 <threads.h> shim `synthesize`
//    recipe tag ────────────────────────────────────────────────────────────────────

// A known recipe id that EQUALS the symbol name decodes onto `ShippedSymbol.synthesize`.
// The vocabulary predicate is the SINGLE source of truth shared with the driver's merged
// reconstruction; Cycle 2 ADDED thrd_create/call_once/thrd_join (the last threads recipes).
TEST(ShippedLibDescriptor, SynthesizeTagDecodesForKnownRecipe) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "threads.json", R"({
        "header": "threads.h",
        "symbols": [
            { "name": "mtx_lock", "signature": "fn(ptr<void>) -> i32",
              "availableObjectFormats": ["pe"], "synthesize": "mtx_lock" },
            { "name": "mtx_lock", "signature": "fn(ptr<void>) -> i32",
              "availableObjectFormats": ["elf"] }
        ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->symbols.size(), 2u);
    // The pe entry carries the tag; the elf entry is a PLAIN untagged extern (locks the
    // tag pe-only — M4(b) at the descriptor tier).
    EXPECT_EQ(desc->symbols[0].synthesize, "mtx_lock");
    EXPECT_TRUE(desc->symbols[1].synthesize.empty());
    EXPECT_TRUE(isKnownSynthesizeRecipe("mtx_lock"));
    EXPECT_TRUE(isKnownSynthesizeRecipe("tss_create"));
    EXPECT_TRUE(isKnownSynthesizeRecipe("thrd_create"));   // Cycle 2 (DIRECT-PASS)
    EXPECT_TRUE(isKnownSynthesizeRecipe("thrd_join"));     // Cycle 2 (multi-block)
    EXPECT_TRUE(isKnownSynthesizeRecipe("call_once"));     // Cycle 2 (once trampoline)
    EXPECT_FALSE(isKnownSynthesizeRecipe("bogus"));
}

// M4(d): an UNKNOWN `synthesize` recipe id fails the read (closed vocabulary).
// RED-ON-DISABLE: without the isKnownSynthesizeRecipe guard a typo'd recipe reaches the
// synth pass with no arm → a silently-undefined shim.
TEST(ShippedLibDescriptor, SynthesizeUnknownRecipeFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "threads_bad.json", R"({
        "header": "threads.h",
        "symbols": [
            { "name": "mtx_lock", "signature": "fn(ptr<void>) -> i32",
              "availableObjectFormats": ["pe"], "synthesize": "mtx_lokc" }
        ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// The `synthesize` value MUST EQUAL the symbol name — the synth pass keys each body on
// the symbol name, so a mismatch would synthesize the WRONG recipe. Fail loud.
// RED-ON-DISABLE: without the name-invariant a descriptor could map mtx_lock's symbol to
// mtx_unlock's body (a lock that silently unlocks).
TEST(ShippedLibDescriptor, SynthesizeNameMismatchFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "threads_mismatch.json", R"({
        "header": "threads.h",
        "symbols": [
            { "name": "mtx_lock", "signature": "fn(ptr<void>) -> i32",
              "availableObjectFormats": ["pe"], "synthesize": "mtx_unlock" }
        ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// ── D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): `shimFamilyOf`, the recipe→pass split ──
//
// There is ONE recipe map but MORE THAN ONE synthesis pass, and each pass fails loud on
// a recipe it has no arm for — deliberately, as its own anti-vocab-drift backstop. So
// both driver seams (compile_pipeline.cpp single-CU, program.cpp multi-CU) PARTITION the
// map by `shimFamilyOf` before calling either pass. That makes the partition load-bearing
// in a way the loader's own vocabulary check is not: a recipe the split sends to the
// WRONG pass turns a build that should succeed into a hard failure, and a recipe the
// split cannot classify at all is treated by both seams as an internal invariant breach.
//
// The documented contract is a BICONDITIONAL — `shimFamilyOf(id) == nullopt` ⇔
// `!isKnownSynthesizeRecipe(id)` — held "by construction" because both functions scan the
// same `kRecipes` table. "By construction" is exactly the kind of claim that stops being
// true the day someone gives one of them a fast path, an early-out or a second table, so
// it is asserted here over EVERY id rather than sampled.

namespace {

// THE PINNED RECIPE VOCABULARY, id → family. This is the test's independent model of
// `kRecipes` (which is file-local to shipped_lib_descriptor.cpp and cannot be enumerated
// from outside), and it is cross-checked in BOTH directions below:
//   * forwards — every id here must be known AND map to this family;
//   * backwards — `EveryShippedSynthesizeTagIsPinnedToItsFamily` walks the REAL shipped
//     descriptors and requires the set of `synthesize` tags they declare to be EXACTLY
//     this set, so adding a recipe to `kRecipes` + a descriptor without updating this
//     table reds, and so does deleting one.
// The residual blind spot is honest and inert: a row added to `kRecipes` that NO shipped
// descriptor declares is invisible here — but it is also unreachable, since the only way
// a recipe id reaches a synthesis pass is a descriptor's `synthesize` tag.
struct RecipeExpectation {
    char const* id;
    ShimFamily  family;
};
constexpr RecipeExpectation kPinnedRecipes[] = {
    // <threads.h> over kernel32 (win32) / libSystem (pthread) — 18 non-trampoline …
    {"mtx_init", ShimFamily::Threads},      {"mtx_lock", ShimFamily::Threads},
    {"mtx_unlock", ShimFamily::Threads},    {"mtx_trylock", ShimFamily::Threads},
    {"mtx_destroy", ShimFamily::Threads},   {"cnd_init", ShimFamily::Threads},
    {"cnd_signal", ShimFamily::Threads},    {"cnd_broadcast", ShimFamily::Threads},
    {"cnd_wait", ShimFamily::Threads},      {"cnd_destroy", ShimFamily::Threads},
    {"tss_create", ShimFamily::Threads},    {"tss_get", ShimFamily::Threads},
    {"tss_set", ShimFamily::Threads},       {"tss_delete", ShimFamily::Threads},
    {"thrd_current", ShimFamily::Threads},  {"thrd_yield", ShimFamily::Threads},
    {"thrd_exit", ShimFamily::Threads},     {"thrd_detach", ShimFamily::Threads},
    // … + the 3 trampolines.
    {"thrd_create", ShimFamily::Threads},   {"thrd_join", ShimFamily::Threads},
    {"call_once", ShimFamily::Threads},
    // <stdio.h> printf/scanf family over the UCRT __stdio_common_v* cores — SIX recipes
    // as of TF-C119, where `sprintf` was once the only one and P3 grew it to five.
    // ucrtbase.dll exports NOT ONE of these six names (in a real MSVC build each is a
    // header inline over a `__stdio_common_v*` core), so once the pe CRT flipped off
    // msvcrt a compiler that binds by export table has nothing to import and must
    // synthesize the body. Each arrived WITH its descriptor row and its core's symbol row,
    // which is what the backward pin below re-checks.
    //
    // ★ `snprintf` HAS NOW GRADUATED out of the negative list, and its case is stronger
    // than its five neighbours' rather than weaker: they were importable until the pe CRT
    // flipped (msvcrt DID export all five), whereas bare `snprintf` was NEVER an export of
    // EITHER CRT — MEASURED, `objdump -p` finds it in neither ucrtbase.dll nor msvcrt.dll.
    // It is also the only member that needed NO new core: there is no
    // `__stdio_common_vsnprintf` to import (ucrtbase has `__stdio_common_vsprintf` at
    // ordinal 117 and `__stdio_common_vsnprintf_s` at 115, nothing between), so it reuses
    // `sprintf`'s core with a different `_Options` bit and a real `_BufferCount`.
    {"printf", ShimFamily::Stdio},          {"fprintf", ShimFamily::Stdio},
    {"sprintf", ShimFamily::Stdio},         {"snprintf", ShimFamily::Stdio},
    {"vfprintf", ShimFamily::Stdio},        {"sscanf", ShimFamily::Stdio},
};

// Ids that must NOT be recipes. Three groups, each catching a different regression:
//   * the UNSHIPPED stdio id — a speculative body sneaking into `kRecipes` reds here;
//   * DEFERRED threads ids (thrd_sleep + the timed waits stay elf-FFI-only) — promoting
//     one to a synth recipe without a body reds here;
//   * ordinary near-misses: plain libc names, a typo, case variants, the empty string.
//
// ★ D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) MOVED `printf`/`fprintf`/`vfprintf`/`sscanf`
// from this negative list into `kPinnedRecipes` above. That is a STRENGTHENING, not a
// removal: each went from "must not be known" to "must be known AND classify as Stdio
// AND be declared by exactly one shipped descriptor row" (the backward pin below), which
// is a strictly narrower constraint. TF-C119 moved `snprintf` the same way, for the same
// reason: stdio.json now declares BOTH halves of it — an [elf,macho] import row and a [pe]
// `synthesize` row — so "must not be known" would now be asserting the opposite of the
// shipped truth. ⚠ CLAIM ROT CORRECTED 2026-08-05: this note used to read "[elf], NOT
// [elf,macho] like its four siblings ... macho is staged behind a real Mac run, exactly as
// popen/pclose/fileno are", and BOTH halves of that are now false — `snprintf` reads
// [elf,macho], and so do `fileno` and (as of this cycle) `popen`/`pclose`. The staging it
// described was real and its REASON still governs any FUTURE row (the libSystem export was
// INFERRED, never measured, and under the eager-import law a wrong guess breaks the LOAD of
// every macho binary that includes <stdio.h>) — it simply ENDED, on a measurement taken on
// the operator's real Mac. Nothing here ASSERTS an availability set, so the rot was
// invisible to the suite; that is exactly why it is corrected rather than left.
// `puts`/`fputs`/`__stdio_common_vsprintf` remain here and are NOT
// candidates for the same graduation: they are REAL ucrtbase exports imported directly,
// and a recipe id for any of them would mean the loader had started synthesizing over a
// symbol it can simply import. `vsnprintf` is added as the fresh negative in `snprintf`'s
// place — it is the next plausible speculative body (the UCRT header reaches the core
// through it), and it has no descriptor row, so a recipe landing ahead of one still reds.
constexpr char const* kNonRecipes[] = {
    "vsnprintf", "snprintf_s",                               // unshipped stdio arms
    "thrd_sleep", "mtx_timedlock", "cnd_timedwait",          // deferred threads ids
    "puts", "fputs", "__stdio_common_vsprintf",
    "mtx_lokc", "SPRINTF", "Sprintf", "sprintf ", " sprintf", "",
};

} // namespace

// EXHAUSTIVE forward pin: every id in the vocabulary is known, maps to its declared
// family, and the two predicates AGREE. Sampling two ids (as the pre-existing
// `SynthesizeTagDecodesForKnownRecipe` does for `isKnownSynthesizeRecipe`) cannot catch a
// family typo on the 19th row; walking the whole table costs nothing and does.
//
// RED-ON-DISABLE: flip any one `kRecipes` row's family tag (e.g. make `sprintf` Threads)
// and this reds on that id alone — and the corresponding real build breaks, because the
// threads pass would then be handed a recipe it has no arm for.
TEST(ShippedLibDescriptor, ShimFamilyOfPartitionsEveryRecipeInTheVocabulary) {
    std::size_t threads = 0, stdio = 0;
    for (auto const& r : kPinnedRecipes) {
        EXPECT_TRUE(isKnownSynthesizeRecipe(r.id))
            << "pinned recipe '" << r.id << "' vanished from the closed vocabulary";
        auto const fam = shimFamilyOf(r.id);
        ASSERT_TRUE(fam.has_value())
            << "pinned recipe '" << r.id << "' belongs to NO family — the driver seams "
               "treat that as an internal invariant breach and abort the build";
        EXPECT_EQ(*fam, r.family)
            << "recipe '" << r.id << "' is routed to the WRONG synthesis pass";
        (r.family == ShimFamily::Threads ? threads : stdio) += 1;
    }
    // The shape of the vocabulary itself, so a silent addition/removal is visible.
    EXPECT_EQ(threads, 21u) << "the <threads.h> family is the 18 non-trampoline + 3 trampolines";
    EXPECT_EQ(stdio, 6u)
        << "the <stdio.h> family ships EXACTLY "
           "printf/fprintf/sprintf/snprintf/vfprintf/sscanf (P3 grew it from 1 to 5; "
           "TF-C119 added snprintf) — a SEVENTH would mean a body landed ahead of its "
           "stdio.json row, and a FIFTH that one was retired without retiring its "
           "descriptor row";
}

// THE LOCKSTEP INVARIANT, asserted as the biconditional the header documents rather than
// as two independent spot-checks: over every pinned id, every deliberate non-recipe, AND a
// systematically generated mutation neighbourhood (each id truncated by one character and
// each id with a character appended), `shimFamilyOf(id).has_value()` must EQUAL
// `isKnownSynthesizeRecipe(id)`.
//
// RED-ON-DISABLE: give either function an early-out the other lacks — e.g. make
// `shimFamilyOf` return `ShimFamily::Stdio` for anything starting with "s", or have
// `isKnownSynthesizeRecipe` short-circuit true on a prefix — and the mutation sweep reds
// even though every hand-written sample would still pass.
TEST(ShippedLibDescriptor, ShimFamilyOfAndIsKnownRecipeStayInLockstep) {
    std::unordered_set<std::string> const known = [] {
        std::unordered_set<std::string> s;
        for (auto const& r : kPinnedRecipes) s.insert(r.id);
        return s;
    }();

    auto biconditional = [](std::string_view id) {
        EXPECT_EQ(shimFamilyOf(id).has_value(), isKnownSynthesizeRecipe(id))
            << "lockstep broken for '" << id
            << "': shimFamilyOf and isKnownSynthesizeRecipe disagree";
    };

    for (auto const& r : kPinnedRecipes) biconditional(r.id);

    for (auto const* n : kNonRecipes) {
        biconditional(n);
        EXPECT_FALSE(isKnownSynthesizeRecipe(n))
            << "'" << n << "' must NOT be a synth recipe";
        EXPECT_FALSE(shimFamilyOf(n).has_value())
            << "'" << n << "' must belong to NO shim family";
    }

    // The generated neighbourhood: one character off in each direction. Skip a mutation
    // that happens to collide with a real recipe (none do today; the guard keeps the
    // sweep correct if the vocabulary later grows a pair like `tss_get`/`tss_gets`).
    for (auto const& r : kPinnedRecipes) {
        std::string const id{r.id};
        std::string const shorter = id.substr(0, id.size() - 1);
        std::string const longer  = id + "x";
        for (auto const& m : {shorter, longer}) {
            biconditional(m);
            if (known.find(m) == known.end()) {
                EXPECT_FALSE(isKnownSynthesizeRecipe(m))
                    << "a one-character mutation of '" << id << "' ('" << m
                    << "') must not be admitted by the CLOSED vocabulary";
            }
        }
    }
}

// BACKWARD pin, against the REAL shipped descriptors: the set of `synthesize` tags any
// descriptor under src/dss-config/shippedLibs declares must be EXACTLY `kPinnedRecipes`,
// and each must resolve to the family pinned there. This is what makes the forward table
// self-maintaining — ship a new recipe and this reds until it is pinned; retire one and
// this reds until the pin is removed.
//
// Reads through the REAL `readShippedLibDescriptor` (never a text scrape), mirroring
// `AllShippedDescriptorsDecode`'s sweep + va_list binding.
TEST(ShippedLibDescriptor, EveryShippedSynthesizeTagIsPinnedToItsFamily) {
    fs::path const shippedRoot = shippedLibsRoot();
    ASSERT_FALSE(shippedRoot.empty())
        << "could not locate src/dss-config/shippedLibs from cwd";

    std::unordered_set<std::string> declared;
    for (auto const& entry : fs::recursive_directory_iterator(shippedRoot)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto const namedTypes = sysvVaListBinding(interner);
        auto desc = readShippedLibDescriptor(entry.path(), interner, typeReg, rep,
                                             DataModel::Lp64, std::nullopt,
                                             std::nullopt, namedTypes);
        ASSERT_TRUE(desc.has_value())
            << "shipped descriptor failed to load: " << entry.path().generic_string();
        for (auto const& s : desc->symbols) {
            if (s.synthesize.empty()) continue;
            declared.insert(s.synthesize);
            // The loader already rejects an unknown id at READ time; this asserts the
            // SECOND half — that the id is also CLASSIFIABLE, which is what the driver
            // seams need and what the loader does not check.
            auto const fam = shimFamilyOf(s.synthesize);
            ASSERT_TRUE(fam.has_value())
                << "shipped recipe '" << s.synthesize << "' in "
                << entry.path().generic_string() << " belongs to no shim family";
            bool pinned = false;
            for (auto const& r : kPinnedRecipes) {
                if (s.synthesize == r.id) {
                    pinned = true;
                    EXPECT_EQ(*fam, r.family)
                        << "shipped recipe '" << s.synthesize << "' changed family";
                }
            }
            EXPECT_TRUE(pinned)
                << "shipped recipe '" << s.synthesize
                << "' is not pinned in kPinnedRecipes — add it (with its family) so the "
                   "vocabulary stays covered";
        }
    }

    EXPECT_EQ(declared.size(), std::size(kPinnedRecipes))
        << "the shipped descriptors and kPinnedRecipes must declare the SAME recipe set";
    for (auto const& r : kPinnedRecipes) {
        EXPECT_TRUE(declared.find(r.id) != declared.end())
            << "pinned recipe '" << r.id
            << "' is declared by NO shipped descriptor — it is unreachable, so either "
               "ship it or retire it from the vocabulary";
    }
}

// ── FC17.9(c) (D-CSUBSET-SETJMP): setjmp.json descriptor decode ───────────────

// The `returnsTwice` symbol bit decodes (default false) exactly like `noreturn`, and
// the two are INDEPENDENT: setjmp/_setjmp are returnsTwice (NOT noreturn); longjmp is
// noreturn (NOT returnsTwice). This is the descriptor half of the carrier chain
// (returnsTwice -> SymbolRecord -> MirInstFlags::ReturnsTwice). RED-ON-DISABLE: drop
// the `returnsTwice` decode arm and the setjmp assertion flips to false.
TEST(ShippedLibDescriptor, SetjmpReturnsTwiceAndLongjmpNoreturnDecode) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "setjmp.json", R"JSON({
        "header": "setjmp.h",
        "availableObjectFormats": ["elf", "pe", "macho"],
        "symbols": [
            { "name": "setjmp",  "signature": "fn(ptr<void>) -> i32",            "returnsTwice": true, "availableObjectFormats": ["elf", "macho"] },
            { "name": "_setjmp", "signature": "fn(ptr<void>, ptr<void>) -> i32", "returnsTwice": true, "availableObjectFormats": ["pe"] },
            { "name": "longjmp", "signature": "fn(ptr<void>, i32) -> void",      "noreturn": true }
        ]
    })JSON");

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(desc->symbols.size(), 3u);

    auto sym = [&](std::string_view n) -> ShippedSymbol const* {
        for (auto const& s : desc->symbols) if (s.name == n) return &s;
        return nullptr;
    };
    ASSERT_NE(sym("setjmp"), nullptr);
    ASSERT_NE(sym("_setjmp"), nullptr);
    ASSERT_NE(sym("longjmp"), nullptr);
    // setjmp / _setjmp: returnsTwice, NOT noreturn.
    EXPECT_TRUE(sym("setjmp")->returnsTwice);
    EXPECT_FALSE(sym("setjmp")->noreturn);
    EXPECT_TRUE(sym("_setjmp")->returnsTwice);
    EXPECT_FALSE(sym("_setjmp")->noreturn);
    // longjmp: noreturn, NOT returnsTwice — the two bits are independent.
    EXPECT_TRUE(sym("longjmp")->noreturn);
    EXPECT_FALSE(sym("longjmp")->returnsTwice);
}

// `returnsTwice` must be a boolean — a non-bool fails loud (the `noreturn`
// type-validation mirror). RED-ON-DISABLE: drop the is_boolean guard and this reads a
// garbage value silently.
TEST(ShippedLibDescriptor, SetjmpReturnsTwiceMustBeBoolean) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "bad_rt.json", R"JSON({
        "header": "setjmp.h",
        "symbols": [
            { "name": "setjmp", "signature": "fn(ptr<void>) -> i32", "returnsTwice": "yes" }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(rep.hasErrors());
}

// The `jmp_buf` opaque buffer is sized per-(arch,format) — the sys/stat.json per-arch
// variant mechanism, keyed on BOTH {arch,format} (setjmp diverges by ARCH within elf).
// Pins each built pair's SIZE + ALIGNMENT via computeLayout. ★ THE LOAD-BEARING pe64
// pin: the pe x86_64 buffer is 256B AND 16-ALIGNED (MSVC's `_setjmp` saves Xmm6-Xmm15
// with `movaps`, which #GP-crashes on an 8-aligned buffer). The 16-alignment is pure
// CONFIG DATA — a `u128` element self-aligns to the target's maxAlignment (16), so
// `arr<u128,16>` is 256B/16-align with NO alignas field. RED-ON-DISABLE: change the pe
// element to `i64` (arr<i64,32>, also 256B) → align drops to 8 → the pe assertion fails
// (and the pe64 example would #GP-crash at runtime).
TEST(ShippedLibDescriptor, SetjmpJmpBufSizeAndAlignPerArchFormat) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "setjmp.json", R"JSON({
        "header": "setjmp.h",
        "availableObjectFormats": ["elf", "pe", "macho"],
        "typedefs": [
            { "name": "jmp_buf", "variants": [
                { "when": { "arch": "x86_64", "format": "elf" },   "type": "arr<i64, 25>" },
                { "when": { "arch": "arm64",  "format": "elf" },   "type": "arr<i64, 39>" },
                { "when": { "arch": "x86_64", "format": "pe" },    "type": "arr<u128, 16>" },
                { "when": { "arch": "arm64",  "format": "macho" }, "type": "arr<i64, 28>" }
            ] }
        ]
    })JSON");

    auto layoutFor = [&](std::string_view arch, ObjectFormatKind fmt)
        -> std::optional<StructLayout> {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64, arch, fmt);
        EXPECT_TRUE(desc.has_value()) << "arch=" << arch;
        EXPECT_FALSE(rep.hasErrors());
        for (auto const& td : desc->typedefs)
            if (td.name == "jmp_buf")
                return computeLayout(td.type, interner, kNatural16, DataModel::Lp64);
        ADD_FAILURE() << "jmp_buf typedef missing for arch=" << arch;
        return std::nullopt;
    };

    auto elfX86 = layoutFor("x86_64", ObjectFormatKind::Elf);
    auto elfArm = layoutFor("arm64",  ObjectFormatKind::Elf);
    auto peX86  = layoutFor("x86_64", ObjectFormatKind::Pe);
    auto machoArm = layoutFor("arm64", ObjectFormatKind::MachO);
    ASSERT_TRUE(elfX86.has_value());
    ASSERT_TRUE(elfArm.has_value());
    ASSERT_TRUE(peX86.has_value());
    ASSERT_TRUE(machoArm.has_value());

    EXPECT_EQ(elfX86->size, 200u);   // glibc x86_64 jmp_buf
    EXPECT_EQ(elfArm->size, 312u);   // glibc arm64 jmp_buf (DIVERGES by arch)
    EXPECT_EQ(peX86->size, 256u);    // MSVC _JUMP_BUFFER
    EXPECT_EQ(machoArm->size, 224u); // macOS arm64 jmp_buf, over-sized from 192B
    // ★ THE pe64 16-alignment (the movaps crash-guard).
    EXPECT_EQ(peX86->align.bytes(), 16u)
        << "pe x86_64 jmp_buf MUST be 16-aligned or _setjmp's movaps #GP-crashes";
    // The elf/macho buffers are 8-aligned (arr<i64,N>) — correct there.
    EXPECT_EQ(elfX86->align.bytes(), 8u);
    EXPECT_EQ(machoArm->align.bytes(), 8u);

    // A (arch,format) pair NOT enumerated (e.g. macho x86_64) yields NO jmp_buf —
    // fail-loud-safe: not injected, so a later use is a loud undefined type, never a
    // silent wrong size.
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, "x86_64",
                                         ObjectFormatKind::MachO);
    ASSERT_TRUE(desc.has_value());
    bool hasJmpBuf = false;
    for (auto const& td : desc->typedefs) if (td.name == "jmp_buf") hasJmpBuf = true;
    EXPECT_FALSE(hasJmpBuf)
        << "an un-enumerated (arch,format) must NOT inject a jmp_buf (fail-loud-safe)";
}

// The pe-only `setjmp(env) -> _setjmp(env, 0)` function-like macro (the stdbit.json
// per-format macro precedent): on pe the macro is present; on elf it is NOT (elf ships
// the real 1-arg `setjmp` symbol, not a macro). Read via the interner-FREE
// `readShippedLibMacros` (the preprocessor's path). RED-ON-DISABLE: drop the pe macro
// variant → the pe `setjmp(env)` never expands to `_setjmp` and resolves to nothing.
TEST(ShippedLibDescriptor, SetjmpPeMacroExpandsToUnderscoreSetjmp) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "setjmp.json", R"JSON({
        "header": "setjmp.h",
        "availableObjectFormats": ["elf", "pe", "macho"],
        "macros": [
            { "name": "setjmp", "variants": [
                { "when": { "format": "pe" }, "params": ["env"], "replacement": "_setjmp(env, 0)" }
            ] }
        ]
    })JSON");
    DiagnosticReporter rep;

    // pe: the macro is present, function-like (params=[env]), expands to _setjmp(env,0).
    auto pe = readShippedLibMacros(path, rep, ObjectFormatKind::Pe);
    ASSERT_TRUE(pe.has_value());
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(pe->size(), 1u);
    EXPECT_EQ((*pe)[0].name, "setjmp");
    ASSERT_TRUE((*pe)[0].params.has_value());
    ASSERT_EQ((*pe)[0].params->size(), 1u);
    EXPECT_EQ((*pe)[0].params->at(0), "env");
    EXPECT_EQ((*pe)[0].replacement, "_setjmp(env, 0)");

    // elf: the variants-only macro has NO elf variant → not injected (elf uses the
    // real `setjmp` symbol).
    auto elf = readShippedLibMacros(path, rep, ObjectFormatKind::Elf);
    ASSERT_TRUE(elf.has_value());
    EXPECT_TRUE(elf->empty())
        << "the pe-only setjmp macro must NOT be injected on elf";
}

// c155 (D-FFI-ELF-ATEXIT-CXA-SPLIT, surfaced by the D-LK10-CRT-INIT-INVOKE closure
// diagnosis): the real stdlib.json MUST keep
// `atexit` gated OFF elf and `__cxa_atexit` gated elf-ONLY. Glibc's libc.so.6
// exports only `__cxa_atexit` in its dynamic symbol table (`atexit` is a
// libc_nonshared.a STATIC shim gcc links into every exec) — c155 re-witnessed the
// failure mode on WSL glibc 2.39: an elf binary importing `atexit` by name dies at
// spawn with `ld.so: symbol lookup error: undefined symbol: atexit`. That break is
// invisible to compile-time CI (the compile succeeds; only the spawn fails), so the
// availability sets are load-bearing runtime-correctness config, not documentation.
// libSystem DOES export `atexit`, so macho keeps the direct symbol.
//
// ★★ D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) TURNED A TWO-WAY SPLIT INTO A THREE-WAY ONE,
// and this test grew a third arm rather than dropping the arm that changed. `atexit`
// narrowed from ["pe","macho"] to ["macho"] ALONE because ucrtbase.dll exports NO
// `atexit` at all (MEASURED, objdump -p) — the UCRT's `atexit` is a static-lib shim over
// the exported `_crt_atexit`, EXACTLY the shape glibc's libc_nonshared.a shim has over
// `__cxa_atexit`. So the pe arm did not disappear; it moved to its own real export, and
// pinning all THREE sets exactly is what makes that visible. Note the failure modes are
// now symmetric and both spawn/load-time, i.e. both invisible to a compile-only CI: an
// elf `atexit` import dies at spawn with ld.so's undefined-symbol error, and under
// D-FFI-DESCRIPTOR-EAGER-IMPORT a pe `atexit` import breaks EVERY pe binary's LOAD with
// 0xC0000139. The availability sets are load-bearing runtime-correctness config.
//
// The pe arm's DELIVERY is verified elsewhere and is deliberately not re-derived here:
// `_crt_atexit` registers handlers that only the ucrtbase `exit` path drains, so
// pe64-x86_64-windows-exec.format.json's `processExit` had to be repointed off kernel32
// `ExitProcess` in the SAME commit (tests/link/test_object_format_schema.cpp pins that
// row; examples/c-subset/shipped_atexit is the byte-exact runtime witness).
//
// RED-ON-DISABLE: adding "elf" to atexit's set (the naive "fix" for an elf atexit user),
// re-widening it back to include "pe" (the naive "fix" for a pe atexit user, which would
// break every pe binary's load), widening __cxa_atexit beyond elf, or widening
// _crt_atexit beyond pe — each flips an exact-set assert here before the regression can
// reach a spawn/load-time failure.
TEST(ShippedLibDescriptor, RealStdlibAtexitPerFormatAvailabilitySplit) {
    fs::path const shippedRoot = shippedLibsRoot();
    ASSERT_FALSE(shippedRoot.empty())
        << "could not locate src/dss-config/shippedLibs from cwd";
    fs::path const stdlibPath = shippedRoot / "stdlib.json";
    ASSERT_TRUE(fs::exists(stdlibPath)) << stdlibPath.generic_string();

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    // Decode keeps EVERY symbol row regardless of the requested format (the
    // per-symbol gate filters at INJECTION — the c106 pin-shape lesson), so one
    // Elf-kind read exposes both symbols' availability sets.
    auto desc = readShippedLibDescriptor(stdlibPath, interner, typeReg, rep,
                                         DataModel::Lp64,
                                         std::string_view{"x86_64"},
                                         ObjectFormatKind::Elf);
    ASSERT_TRUE(desc.has_value());
    ASSERT_FALSE(rep.hasErrors());

    std::vector<std::string> atexitSet;
    std::vector<std::string> cxaSet;
    std::vector<std::string> crtSet;
    bool sawAtexit = false;
    bool sawCxa    = false;
    bool sawCrt    = false;
    for (auto const& s : desc->symbols) {
        if (s.name == "atexit")        { sawAtexit = true; atexitSet = s.availableObjectFormats; }
        if (s.name == "__cxa_atexit")  { sawCxa    = true; cxaSet    = s.availableObjectFormats; }
        if (s.name == "_crt_atexit")   { sawCrt    = true; crtSet    = s.availableObjectFormats; }
    }
    ASSERT_TRUE(sawAtexit) << "atexit absent from stdlib.json symbols";
    ASSERT_TRUE(sawCxa)    << "__cxa_atexit absent from stdlib.json symbols";
    ASSERT_TRUE(sawCrt)
        << "_crt_atexit absent from stdlib.json symbols — the pe arm of the split has no "
           "registration primitive at all, so pe `atexit(f)` cannot resolve";

    EXPECT_EQ(atexitSet, (std::vector<std::string>{"macho"}))
        << "atexit must stay OFF elf (glibc's libc.so.6 has no `atexit` dynsym export -- "
           "an elf by-name import dies loud at spawn with ld.so's symbol lookup error, "
           "witnessed c155 on glibc 2.39) AND OFF pe (ucrtbase.dll exports no `atexit` "
           "either, and under D-FFI-DESCRIPTOR-EAGER-IMPORT that breaks every pe binary's "
           "LOAD with 0xC0000139) -- macho alone ships the direct libSystem symbol";
    EXPECT_EQ(cxaSet, (std::vector<std::string>{"elf"}))
        << "__cxa_atexit is the elf-only registration primitive (GLIBC_2.2.5 "
           "dynsym export); pe uses _crt_atexit and macho the standard `atexit`";
    EXPECT_EQ(crtSet, (std::vector<std::string>{"pe"}))
        << "_crt_atexit is the pe-only registration primitive (a real ucrtbase.dll "
           "export); it must never widen to elf/macho, where it does not exist";

    // The gate the injector consults, asserted directly for every arm x every format —
    // the full 3x3, so no arm can quietly widen into another's territory.
    EXPECT_FALSE(objectFormatInAvailabilitySet(atexitSet, ObjectFormatKind::Elf));
    EXPECT_FALSE(objectFormatInAvailabilitySet(atexitSet, ObjectFormatKind::Pe));
    EXPECT_TRUE(objectFormatInAvailabilitySet(atexitSet, ObjectFormatKind::MachO));
    EXPECT_TRUE(objectFormatInAvailabilitySet(cxaSet, ObjectFormatKind::Elf));
    EXPECT_FALSE(objectFormatInAvailabilitySet(cxaSet, ObjectFormatKind::Pe));
    EXPECT_FALSE(objectFormatInAvailabilitySet(cxaSet, ObjectFormatKind::MachO));
    EXPECT_FALSE(objectFormatInAvailabilitySet(crtSet, ObjectFormatKind::Elf));
    EXPECT_TRUE(objectFormatInAvailabilitySet(crtSet, ObjectFormatKind::Pe));
    EXPECT_FALSE(objectFormatInAvailabilitySet(crtSet, ObjectFormatKind::MachO));
}

// ── D-LANG-TYPE-IDENTITY-VOCABULARY: the per-DATA-MODEL `when` selector ────
//
// C defines `size_t` / `uint64_t` / `intmax_t` as ALIASES of a standard NAMED
// type, and WHICH name is DATA-MODEL-dependent (`size_t` IS `unsigned long` on
// LP64 and `unsigned long long` on LLP64). The descriptors spelled them as a
// bare `u64` — the ANONYMOUS representative, a THIRD type matching NEITHER named
// entry, so a `_Generic` over the two standard names silently missed. `when:
// {dataModel}` is the third target axis alongside {arch, format}; these pin the
// SHIPPED spelling, structurally (TypeId equality against a freshly-built named
// primitive), never a string compare.
TEST(ShippedLibDescriptor, ShippedFixedWidthAliasesCarryTheirVocabularyTag) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";

    struct Row { char const* lib; char const* alias; TypeKind core;
                 char const* lp64Name; char const* llp64Name; };
    constexpr Row kRows[] = {
        {"stddef", "size_t",     TypeKind::U64, "unsigned long", "unsigned long long"},
        {"stddef", "ptrdiff_t",  TypeKind::I64, "long",          "long long"},
        {"stdint", "uint64_t",   TypeKind::U64, "unsigned long", "unsigned long long"},
        {"stdint", "int64_t",    TypeKind::I64, "long",          "long long"},
        {"stdint", "uintptr_t",  TypeKind::U64, "unsigned long", "unsigned long long"},
        {"stdint", "intmax_t",   TypeKind::I64, "long",          "long long"},
        {"stdio",  "size_t",     TypeKind::U64, "unsigned long", "unsigned long long"},
        {"string", "size_t",     TypeKind::U64, "unsigned long", "unsigned long long"},
        {"time",   "time_t",     TypeKind::I64, "long",          "long long"},
    };
    for (Row const& row : kRows) {
        for (DataModel const dm : {DataModel::Lp64, DataModel::Llp64}) {
            SCOPED_TRACE(std::string{row.lib} + "." + row.alias
                         + (dm == DataModel::Lp64 ? " LP64" : " LLP64"));
            TypeInterner interner{CompilationUnitId{1}};
            TypeRegistry typeReg;
            DiagnosticReporter rep;
            // stdio's `vfprintf` needs the SysV va_list binding, exactly as the
            // production reader threads it (c82).
            auto const namedTypes = sysvVaListBinding(interner);
            auto desc = readShippedLibDescriptor(
                root / (std::string{row.lib} + ".json"), interner, typeReg, rep,
                dm, "x86_64",
                dm == DataModel::Lp64 ? ObjectFormatKind::Elf : ObjectFormatKind::Pe,
                namedTypes);
            ASSERT_TRUE(desc.has_value());
            EXPECT_FALSE(rep.hasErrors());
            char const* want = dm == DataModel::Lp64 ? row.lp64Name : row.llp64Name;
            bool found = false;
            for (auto const& td : desc->typedefs) {
                if (td.name != row.alias) continue;
                found = true;
                EXPECT_EQ(td.type, interner.primitive(row.core, want))
                    << "the alias must BE the data model's named standard type";
                EXPECT_NE(td.type, interner.primitive(row.core))
                    << "... and must NOT be the ANONYMOUS representative of its "
                       "core, which matches no named `_Generic` association";
            }
            EXPECT_TRUE(found) << "alias not injected on this data model";
        }
    }
}

// The selector composes with {arch, format} and rejects a typo'd model name —
// a silently-never-matching key would make the entry VANISH on every target.
TEST(ShippedLibDescriptor, TypedefDataModelVariantSelectsAndFailsLoud) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "dm.json", R"JSON({
        "header": "dm.h",
        "typedefs": [
            { "name": "alias_t", "variants": [
                { "when": { "dataModel": "LP64" },  "type": "u64 \"unsigned long\"" },
                { "when": { "dataModel": "LLP64" }, "type": "u64 \"unsigned long long\"" }
            ] }
        ]
    })JSON");
    auto aliasFor = [&](DataModel dm, TypeInterner& interner) -> TypeId {
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep, dm);
        EXPECT_TRUE(desc.has_value());
        EXPECT_FALSE(rep.hasErrors());
        if (!desc.has_value()) return {};
        for (auto const& td : desc->typedefs)
            if (td.name == "alias_t") return td.type;
        ADD_FAILURE() << "alias_t missing";
        return {};
    };
    {
        TypeInterner interner{CompilationUnitId{1}};
        EXPECT_EQ(aliasFor(DataModel::Lp64, interner),
                  interner.primitive(TypeKind::U64, "unsigned long"));
    }
    {
        TypeInterner interner{CompilationUnitId{1}};
        EXPECT_EQ(aliasFor(DataModel::Llp64, interner),
                  interner.primitive(TypeKind::U64, "unsigned long long"));
    }
    // A typo'd data-model name is a CLOSED-vocabulary violation, not a silent
    // never-match.
    auto const bad = writeTemp(dir, "dmbad.json", R"JSON({
        "header": "dmbad.h",
        "typedefs": [
            { "name": "alias_t", "variants": [
                { "when": { "dataModel": "LP62" }, "type": "u64" }
            ] }
        ]
    })JSON");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(bad, interner, typeReg, rep,
                                         DataModel::Lp64);
    EXPECT_TRUE(rep.hasErrors())
        << "an unknown data-model name must fail LOUD (it could never match)";
    EXPECT_FALSE(desc.has_value());
}

// ── TF-C92: the real <sys/ioctl.h> request-ENCODING macros, per format ────────
//
// sqlite/src/os_unix.c uses this header's macros at TWO measured sites — macho
// os_unix.c:2986 `_IOWR('z', 23, struct ByteRangeLockPB2)` (consumed at :3013 by
// `fsctl`), and elf os_unix.c:392-396 `_IO` x4 + `_IOR` x1 under `#ifdef
// __linux__`. Because an angle include resolves to DESCRIPTORS ONLY (the real
// <sys/ioccom.h> text is never read), a missing row let `_IOWR` reach the parser
// as a call whose 3rd argument is a TYPE-NAME: `error[P0009] … got 'struct'` at
// os_unix.c:2986:56 — MEASURED as the sole diagnostic in that TU, and MEASURED to
// come back the instant the `_IOWR` row (or just its macho variant) is removed.
//
// The two OSes disagree on the DIRECTION FIELD ENTIRELY — Darwin gives each
// direction its own bit high in the word (VOID 0x20000000, OUT 0x40000000,
// IN 0x80000000, INOUT 0xc0000000) and masks the size to 13 bits; Linux
// asm-generic packs a 2-bit direction CODE at bit 30 (NONE 0, WRITE 1, READ 2,
// so _IOWR is 3) and does not mask. So this pins the exact per-format
// replacement TEXT, not just that some replacement exists: a copy-paste between
// the two arms would compute a plausible-looking but WRONG ioctl number, and a
// wrong request number is silent at every layer below the syscall.
//
// RED-ON-DISABLE: drop a row → its EXPECT_TRUE(has …) fails; swap either arm's
// direction constant or shift → the exact-text EXPECT fails; add `_IOC` or the
// IOC_*/_IOC_* direction vocabulary → the deliberate-omission EXPECTs fail (that
// line is a decision recorded in the descriptor's `$comment`, not an oversight,
// so re-crossing it must be a conscious edit). The VALUE side is pinned
// end-to-end by `examples/c-subset/shipped_ioctl_iowr_macho/`, which
// `_Static_assert`s the encoded numbers (0xc0207a17 &c.) that were measured
// against the real SDK sys/ioccom.h.
TEST(ShippedLibDescriptor, RealIoctlRequestEncodingMacrosPerFormat) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    fs::path const path = root / "sys" / "ioctl.json";
    ASSERT_TRUE(fs::exists(path)) << path.generic_string();

    // Read through the interner-FREE preprocessor path — the tier that actually
    // splices these as synthetic `#define`s at include resolution.
    auto macrosFor = [&](ObjectFormatKind fmt) {
        DiagnosticReporter rep;
        auto m = readShippedLibMacros(path, rep, fmt);
        EXPECT_FALSE(rep.hasErrors()) << "sys/ioctl.json must read clean";
        EXPECT_TRUE(m.has_value());
        return m.value_or(std::vector<ShippedMacro>{});
    };
    auto find = [](std::vector<ShippedMacro> const& ms, std::string_view name)
        -> ShippedMacro const* {
        for (auto const& m : ms)
            if (m.name == name) return &m;
        return nullptr;
    };

    auto const macho = macrosFor(ObjectFormatKind::MachO);
    auto const elf   = macrosFor(ObjectFormatKind::Elf);

    // Exactly the four PORTABLE encoding macros, on BOTH formats — one row per
    // name with a variant per format, so neither format is silently short.
    std::vector<std::string> const expected{"_IO", "_IOR", "_IOW", "_IOWR"};
    ASSERT_EQ(macho.size(), expected.size());
    ASSERT_EQ(elf.size(), expected.size());
    for (auto const& name : expected) {
        EXPECT_NE(find(macho, name), nullptr) << name << " missing on macho";
        EXPECT_NE(find(elf, name), nullptr) << name << " missing on elf";
    }

    // ARITY: `_IO` takes (group, num); the three sized ones take (group, num,
    // type). `params` PRESENT (not nullopt) is what makes them FUNCTION-LIKE —
    // an object-like row would not consume `('z', 23, struct T)` at all.
    auto expectArity = [&](std::vector<ShippedMacro> const& ms,
                           std::string_view name, std::size_t arity,
                           char const* fmtName) {
        auto const* m = find(ms, name);
        ASSERT_NE(m, nullptr) << name << " on " << fmtName;
        ASSERT_TRUE(m->params.has_value())
            << name << " must be FUNCTION-like on " << fmtName;
        EXPECT_EQ(m->params->size(), arity) << name << " on " << fmtName;
        EXPECT_FALSE(m->variadic) << name << " is not variadic";
        EXPECT_FALSE(m->replacement.empty()) << name << " must have a body";
    };
    expectArity(macho, "_IO",   2u, "macho");
    expectArity(macho, "_IOR",  3u, "macho");
    expectArity(macho, "_IOW",  3u, "macho");
    expectArity(macho, "_IOWR", 3u, "macho");
    expectArity(elf,   "_IO",   2u, "elf");
    expectArity(elf,   "_IOR",  3u, "elf");
    expectArity(elf,   "_IOW",  3u, "elf");
    expectArity(elf,   "_IOWR", 3u, "elf");

    // EXACT per-format replacement text. macho ← the SDK sys/ioccom.h layout;
    // elf ← the musl/asm-generic layout. Every direction literal carries `u`:
    // on elf that is LOAD-BEARING (`2 << 30` overflows a signed int, and DSS and
    // clang were measured to widen that UB differently — 0x80000000 vs
    // 0xffffffff80000000 — so a signed spelling computes a request number no
    // native consumer of the same header agrees with).
    auto expectBody = [&](std::vector<ShippedMacro> const& ms,
                          std::string_view name, char const* body,
                          char const* fmtName) {
        auto const* m = find(ms, name);
        ASSERT_NE(m, nullptr) << name << " on " << fmtName;
        EXPECT_EQ(m->replacement, body) << name << " on " << fmtName;
    };
    expectBody(macho, "_IO",
               "(0x20000000u | (((0u) & 0x1fffu) << 16) | ((g) << 8) | (n))", "macho");
    expectBody(macho, "_IOR",
               "(0x40000000u | ((sizeof(t) & 0x1fffu) << 16) | ((g) << 8) | (n))", "macho");
    expectBody(macho, "_IOW",
               "(0x80000000u | ((sizeof(t) & 0x1fffu) << 16) | ((g) << 8) | (n))", "macho");
    expectBody(macho, "_IOWR",
               "(0xc0000000u | ((sizeof(t) & 0x1fffu) << 16) | ((g) << 8) | (n))", "macho");
    expectBody(elf, "_IO",
               "(((0u) << 30) | ((g) << 8) | (n) | ((0u) << 16))", "elf");
    expectBody(elf, "_IOR",
               "(((2u) << 30) | ((g) << 8) | (n) | (sizeof(t) << 16))", "elf");
    expectBody(elf, "_IOW",
               "(((1u) << 30) | ((g) << 8) | (n) | (sizeof(t) << 16))", "elf");
    expectBody(elf, "_IOWR",
               "(((3u) << 30) | ((g) << 8) | (n) | (sizeof(t) << 16))", "elf");

    // The per-format VARIANT SELECTION really diverged — if the selector ever
    // handed one format the other's arm, these would compare equal. This is the
    // property the `$comment`'s agnosticism claim rests on.
    for (auto const& name : expected) {
        auto const* m = find(macho, name);
        auto const* e = find(elf, name);
        ASSERT_NE(m, nullptr);
        ASSERT_NE(e, nullptr);
        EXPECT_NE(m->replacement, e->replacement)
            << name << ": the macho and elf encodings are NOT the same layout";
    }

    // No `sizeof` in `_IO` (len is a literal 0 on both formats), and `sizeof(t)`
    // present in each sized one — the type-name-through-a-macro-parameter
    // mechanism the c92 defect proved untested.
    for (auto const* ms : {&macho, &elf}) {
        auto const* io = find(*ms, "_IO");
        ASSERT_NE(io, nullptr);
        EXPECT_EQ(io->replacement.find("sizeof"), std::string::npos)
            << "_IO encodes no parameter length";
        for (auto const& name : {"_IOR", "_IOW", "_IOWR"}) {
            auto const* sized = find(*ms, name);
            ASSERT_NE(sized, nullptr) << name;
            EXPECT_NE(sized->replacement.find("sizeof(t)"), std::string::npos)
                << name << " must take its length from sizeof(t)";
        }
    }

    // DELIBERATE OMISSIONS (no-over-ship line, justified in the `$comment`):
    // `_IOC` and the per-OS direction-bit vocabulary are NOT the portable
    // interface and no corpus site names them; their absence is LOUD (an
    // undeclared identifier), never a silent wrong number.
    for (auto const* ms : {&macho, &elf})
        for (auto const& name : {"_IOC", "IOC_VOID", "IOC_OUT", "IOC_IN",
                                 "IOC_INOUT", "IOCPARM_MASK", "_IOC_NONE",
                                 "_IOC_WRITE", "_IOC_READ"})
            EXPECT_EQ(find(*ms, name), nullptr)
                << name << " is deliberately NOT shipped — see the $comment";

    // The macros ride ALONGSIDE the pre-existing link surface; `ioctl`'s request
    // parameter is the u64 these encodings widen into, so the two halves of this
    // descriptor have to keep agreeing.
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    auto desc = decodeShippedFor(path, interner, typeReg, ObjectFormatKind::MachO);
    ASSERT_TRUE(desc.has_value());
    ASSERT_EQ(desc->symbols.size(), 1u);
    EXPECT_EQ(desc->symbols[0].name, "ioctl");
    EXPECT_EQ(desc->macros.size(), expected.size())
        << "the full read must select the same four macros as the pp read";
    // No `constants` surface: the numerics are inlined into each macro body so
    // every row is self-contained (adding IOC_* constants would be a new claim).
    EXPECT_TRUE(desc->constants.empty())
        << "sys/ioctl.json ships no constant surface by design";
}

// ── D-CSUBSET-DARWIN-BSD-SYMBOL-CLUSTER: the Darwin/BSD vocabulary sqlite
//    os_unix.c's proxy/AFP-locking region consumes ────────────────────────────
//
// strlcpy/strlcat (<string.h>), random/srandomdev (<stdlib.h>), futimes
// (<sys/time.h>), fsctl + the uuid_t typedef (<unistd.h>). Every consumer lives
// inside sqlite os_unix.c's `#if defined(__APPLE__) &&
// SQLITE_ENABLE_LOCKING_STYLE` region (strlcpy first at :7434, strlcat :7443,
// random :3197, srandomdev :6169, futimes :7883, fsctl :3013, uuid_t
// :7600/:7607/:7791), and every name lives in a REAL SDK header the shipped
// descriptor SHADOWS totally (an angle-include never reads the SDK text) — so
// the descriptor row is the ONLY possible source, and each name is S0001
// (uuid_t: S0006) without it. Hence the macho-ONLY gates: no elf/pe consumer
// exists (the no-over-ship rule), and srandomdev/fsctl/uuid_t do not even
// exist off Darwin.
//
// THE PRESENCE+ABSENCE PAIRS BELOW *ARE* THE RED-ON-DISABLE: delete any of the
// six symbol rows and its PRESENT assert fails; widen a row's
// availableObjectFormats beyond ["macho"] (or drop the set) and its exact-set +
// gate asserts fail; delete the uuid_t macho variant (or add an elf one) and
// the typedef PRESENT/ABSENT asserts fail. Runtime witnesses: the
// shipped_strlcpy_strlcat_macho and shipped_bsd_random_futimes_uuid_macho
// corpora on the darwin CI leg.
//
// TWO DIFFERENT ABSENCE MECHANISMS, each asserted the way its architecture
// actually works:
//   * SYMBOLS: decode keeps EVERY row regardless of the requested format (the
//     c106 pin-shape lesson — see RealStdlibAtexitPerFormatAvailabilitySplit);
//     the per-symbol gate filters at semantic INJECTION. So the elf ABSENCE is
//     pinned via the exact availability set + the injector's own
//     `objectFormatInAvailabilitySet` predicate — the atexit-test idiom — not
//     via row disappearance.
//   * TYPEDEFS: there is no per-typedef availableObjectFormats key (closed key
//     set {name,type,variants}); availability IS which `variants` exist, and
//     selection happens AT DECODE — zero matches ⇒ the typedef is not in
//     `desc->typedefs` at all (the sys/types fixpt_t/segsz_t mechanism), so
//     the elf ABSENCE is a straight lookup miss.

// One (arch, format) read of a REAL descriptor for the cluster tests: the
// interner rides along so signatures/typedefs can be inspected STRUCTURALLY
// (never a string compare of raw JSON).
struct DarwinBsdClusterRead {
    TypeInterner       interner{CompilationUnitId{1}};
    TypeRegistry       typeReg;
    DiagnosticReporter rep;
    std::optional<ShippedLibDescriptor> desc;
};

static void readDarwinBsdCluster(fs::path const& path, std::string_view arch,
                                 ObjectFormatKind fmt, DarwinBsdClusterRead& out) {
    out.desc = readShippedLibDescriptor(path, out.interner, out.typeReg, out.rep,
                                        DataModel::Lp64, arch, fmt);
    ASSERT_TRUE(out.desc.has_value())
        << path.generic_string() << " failed to load for arch=" << arch;
    ASSERT_FALSE(out.rep.hasErrors())
        << path.generic_string() << " emitted diagnostics for arch=" << arch;
}

static ShippedSymbol const* findDarwinBsdSymbol(DarwinBsdClusterRead const& r,
                                                std::string_view name) {
    for (auto const& s : r.desc->symbols)
        if (s.name == name) return &s;
    return nullptr;
}

// PRESENT side of one macho-only symbol: exact decoded FnSig shape
// (result + params, param pointees where the param is a pointer) + the exact
// ["macho"] availability set + the injector gate excluding elf/pe.
//
// `retPointee` is the RESULT's pointee, asserted only when engaged — the
// trailing-optional keeps every pre-existing caller byte-identical while
// letting a pointer-RETURNING row (the malloc-zone cluster: four of the seven
// hand back `malloc_zone_t *`/`void *`) pin `ptr<void>` rather than settle for
// "some Ptr", which alone would not distinguish it from `ptr<char>`.
static void expectMachoOnlyFn(DarwinBsdClusterRead const& r, std::string_view name,
                              TypeKind ret,
                              std::vector<TypeKind> const& params,
                              std::vector<std::optional<TypeKind>> const& pointees,
                              std::optional<TypeKind> retPointee = std::nullopt) {
    ASSERT_EQ(params.size(), pointees.size()) << "test-table shape";
    auto const* sym = findDarwinBsdSymbol(r, name);
    ASSERT_NE(sym, nullptr) << name << " row absent (RED-ON-DISABLE: this is "
                               "the delete-the-row red)";
    ASSERT_EQ(r.interner.kind(sym->signature), TypeKind::FnSig) << name;
    EXPECT_EQ(r.interner.kind(r.interner.fnResult(sym->signature)), ret)
        << name << " return kind";
    if (retPointee.has_value()) {
        auto relem = r.interner.operands(r.interner.fnResult(sym->signature));
        ASSERT_EQ(relem.size(), 1u) << name << " result pointee";
        EXPECT_EQ(r.interner.kind(relem[0]), *retPointee)
            << name << " result pointee kind";
    }
    auto ps = r.interner.fnParams(sym->signature);
    ASSERT_EQ(ps.size(), params.size()) << name << " arity";
    for (std::size_t i = 0; i < params.size(); ++i) {
        EXPECT_EQ(r.interner.kind(ps[i]), params[i]) << name << " param " << i;
        if (pointees[i].has_value()) {
            auto elem = r.interner.operands(ps[i]);
            ASSERT_EQ(elem.size(), 1u) << name << " param " << i << " pointee";
            EXPECT_EQ(r.interner.kind(elem[0]), *pointees[i])
                << name << " param " << i << " pointee kind";
        }
    }
    EXPECT_EQ(sym->availableObjectFormats, (std::vector<std::string>{"macho"}))
        << name << " must be gated macho-ONLY: its only consumers are inside "
           "os_unix.c's __APPLE__ && SQLITE_ENABLE_LOCKING_STYLE region, and "
           "DSS eager-imports every DECLARED shipped extern";
    EXPECT_TRUE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                              ObjectFormatKind::MachO)) << name;
    EXPECT_FALSE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                               ObjectFormatKind::Elf)) << name;
    EXPECT_FALSE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                               ObjectFormatKind::Pe)) << name;
}

TEST(ShippedLibDescriptor, RealStringJsonStrlcpyStrlcatMachoOnly) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "string.json";

    using K = TypeKind;
    // BSD semantics make the RESULT the load-bearing kind: both return the
    // length of the string they TRIED to create (a size_t/u64), NOT the dest
    // pointer strcpy/strcat return — a Ptr result here would silently break
    // the corpus exit arithmetic (5*8+2).
    for (std::string_view arch : {"x86_64", "arm64"}) {
        DarwinBsdClusterRead m;
        ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, arch,
                                                     ObjectFormatKind::MachO, m));
        for (auto const* name : {"strlcpy", "strlcat"})
            expectMachoOnlyFn(m, name, K::U64,
                              {K::Ptr, K::Ptr, K::U64},
                              {K::Char, K::Char, std::nullopt});
    }

    // (x86_64, Elf) read: the rows are STILL in the decode (symbol gating
    // filters at injection, not at decode — the c106 pin-shape lesson), and the
    // availability set is format-invariant, so the elf ABSENCE asserted above
    // (gate == false) is the whole story. Positive control: strlen (ungated)
    // must carry an EMPTY set — without it a decode that dropped every
    // availability set would pass the exact-set asserts vacuously... it cannot,
    // but the control also proves THIS read decoded a real symbol surface.
    DarwinBsdClusterRead e;
    ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, "x86_64",
                                                 ObjectFormatKind::Elf, e));
    for (auto const* name : {"strlcpy", "strlcat"}) {
        auto const* sym = findDarwinBsdSymbol(e, name);
        ASSERT_NE(sym, nullptr)
            << name << " must still DECODE on an elf read (gating is at "
               "injection); its absence on elf is the gate assert above";
        EXPECT_FALSE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                                   ObjectFormatKind::Elf)) << name;
    }
    auto const* strlen_ = findDarwinBsdSymbol(e, "strlen");
    ASSERT_NE(strlen_, nullptr) << "positive control: strlen must be present";
    EXPECT_TRUE(strlen_->availableObjectFormats.empty())
        << "positive control: strlen ships ungated (every format)";
}

TEST(ShippedLibDescriptor, RealStdlibJsonRandomSrandomdevMachoOnly) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "stdlib.json";

    using K = TypeKind;
    for (std::string_view arch : {"x86_64", "arm64"}) {
        DarwinBsdClusterRead m;
        ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, arch,
                                                     ObjectFormatKind::MachO, m));
        // `long random(void)` — the i64 result is the LP64 C `long`; a FLAT
        // signature is correct because the [macho] gate means only LP64
        // targets can ever select this row (asserted right below).
        expectMachoOnlyFn(m, "random", K::I64, {}, {});
        expectMachoOnlyFn(m, "srandomdev", K::Void, {}, {});
    }

    // Elf read: rows persist in the decode; the gate is the absence (same
    // rationale as the string.json test). Positive control: rand (ungated).
    DarwinBsdClusterRead e;
    ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, "x86_64",
                                                 ObjectFormatKind::Elf, e));
    for (auto const* name : {"random", "srandomdev"}) {
        auto const* sym = findDarwinBsdSymbol(e, name);
        ASSERT_NE(sym, nullptr) << name << " must still decode on elf";
        EXPECT_FALSE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                                   ObjectFormatKind::Elf)) << name;
    }
    auto const* rand_ = findDarwinBsdSymbol(e, "rand");
    ASSERT_NE(rand_, nullptr) << "positive control: rand must be present";
    EXPECT_TRUE(rand_->availableObjectFormats.empty())
        << "positive control: rand ships ungated (every format)";
}

// D-LK-SQLITE-MACHO-UNDEFINED-LIBSYSTEM-SYMBOLS — the seven Darwin ZONE
// ALLOCATOR externs sqlite mem1.c's SQLITE_SYSTEM_MALLOC Darwin arm references
// on the DEFAULT macho path. libsystem_malloc owns them; they reach the program
// re-exported through the libSystem umbrella, so the honest existence check is
// dlopen+dlsym, NOT libSystem.B.dylib's own export trie (that trie lists six
// symbols total and reports all seven absent — a false negative).
//
// DELIBERATELY NO malloc_zone_t MODELLING: mem1.c uses it only as an opaque
// pointer plus calls through its function-pointer members, so every
// `malloc_zone_t *` is ptr<void> here and the REAL type keeps arriving from the
// SDK malloc/malloc.h via D-INCLUDE-ANGLE-SOURCE-FALLBACK. That fallback is
// also why these rows live in stdlib.json instead of a new malloc/malloc.json:
// a descriptor SHADOWS its header totally, so a partial malloc/malloc.json
// would delete mem1.c's access to the real malloc_zone_t and REGRESS a TU that
// compiles today.
//
// The size_t/vm_size_t params are asserted U64 exactly. A flat u64 is correct
// ONLY because the ["macho"] gate narrows these rows to LP64 (all eight
// macho64-* formats declare dataModel LP64) — an ungated row would have needed
// an LLP64 arm, so the availability assert below is load-bearing for the
// SIGNATURE's correctness and not merely for the no-over-ship rule.
TEST(ShippedLibDescriptor, RealStdlibJsonMallocZoneMachoOnly) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "stdlib.json";

    using K = TypeKind;
    for (std::string_view arch : {"x86_64", "arm64"}) {
        DarwinBsdClusterRead m;
        ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, arch,
                                                     ObjectFormatKind::MachO, m));
        // malloc/malloc.h:390 `malloc_zone_t *malloc_default_zone(void)`
        expectMachoOnlyFn(m, "malloc_default_zone", K::Ptr, {}, {}, K::Void);
        // :394 `malloc_zone_t *malloc_create_zone(vm_size_t, unsigned)` —
        // vm_size_t is 8-byte unsigned, `unsigned flags` is u32.
        expectMachoOnlyFn(m, "malloc_create_zone", K::Ptr, {K::U64, K::U32},
                          {std::nullopt, std::nullopt}, K::Void);
        // :509 `void malloc_set_zone_name(malloc_zone_t *, const char *)` —
        // the ptr<char> name param is what distinguishes it from a zone ptr.
        expectMachoOnlyFn(m, "malloc_set_zone_name", K::Void, {K::Ptr, K::Ptr},
                          {K::Void, K::Char});
        // :457 `size_t malloc_size(const void *)` — the U64 RESULT is the
        // load-bearing kind: mem1.c uses it as the allocation-size oracle, so
        // an i32 result would silently truncate a >2GB block's size.
        expectMachoOnlyFn(m, "malloc_size", K::U64, {K::Ptr}, {K::Void});
        // :403 / :447 / :450 — the zone-scoped malloc/free/realloc trio.
        expectMachoOnlyFn(m, "malloc_zone_malloc", K::Ptr, {K::Ptr, K::U64},
                          {K::Void, std::nullopt}, K::Void);
        expectMachoOnlyFn(m, "malloc_zone_free", K::Void, {K::Ptr, K::Ptr},
                          {K::Void, K::Void});
        expectMachoOnlyFn(m, "malloc_zone_realloc", K::Ptr,
                          {K::Ptr, K::Ptr, K::U64},
                          {K::Void, K::Void, std::nullopt}, K::Void);
    }

    // Elf read: rows persist in the decode; the gate predicate IS the elf
    // absence. Positive control: malloc (ungated) — it also proves this read
    // decoded a real symbol surface rather than an empty one.
    DarwinBsdClusterRead e;
    ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, "x86_64",
                                                 ObjectFormatKind::Elf, e));
    for (auto const* name : {"malloc_default_zone", "malloc_create_zone",
                             "malloc_set_zone_name", "malloc_size",
                             "malloc_zone_malloc", "malloc_zone_free",
                             "malloc_zone_realloc"}) {
        auto const* sym = findDarwinBsdSymbol(e, name);
        ASSERT_NE(sym, nullptr) << name << " must still decode on elf";
        EXPECT_FALSE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                                   ObjectFormatKind::Elf))
            << name << " must NOT be injected on elf: glibc has no zone "
                       "allocator at all, so declaring it there would plant an "
                       "undefined import that kills the loader";
    }
    auto const* malloc_ = findDarwinBsdSymbol(e, "malloc");
    ASSERT_NE(malloc_, nullptr) << "positive control: malloc must be present";
    EXPECT_TRUE(malloc_->availableObjectFormats.empty())
        << "positive control: malloc ships ungated (every format)";
}

TEST(ShippedLibDescriptor, RealSysTimeJsonFutimesMachoOnly) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "sys" / "time.json";

    using K = TypeKind;
    for (std::string_view arch : {"x86_64", "arm64"}) {
        DarwinBsdClusterRead m;
        ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, arch,
                                                     ObjectFormatKind::MachO, m));
        // The timeval pointer keeps utimes' own ptr<void> spelling (sqlite's
        // sole call passes NULL, so no layout knowledge rides on the param).
        expectMachoOnlyFn(m, "futimes", K::I32,
                          {K::I32, K::Ptr},
                          {std::nullopt, K::Void});
    }

    DarwinBsdClusterRead e;
    ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, "x86_64",
                                                 ObjectFormatKind::Elf, e));
    auto const* sym = findDarwinBsdSymbol(e, "futimes");
    ASSERT_NE(sym, nullptr) << "futimes must still decode on elf";
    EXPECT_FALSE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                               ObjectFormatKind::Elf));
    auto const* utimes = findDarwinBsdSymbol(e, "utimes");
    ASSERT_NE(utimes, nullptr) << "positive control: utimes must be present";
    EXPECT_TRUE(utimes->availableObjectFormats.empty())
        << "positive control: utimes rides the header-level [elf,macho] gate "
           "with no per-symbol set";
}

TEST(ShippedLibDescriptor, RealUnistdJsonFsctlMachoOnly) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "unistd.json";

    using K = TypeKind;
    for (std::string_view arch : {"x86_64", "arm64"}) {
        DarwinBsdClusterRead m;
        ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, arch,
                                                     ObjectFormatKind::MachO, m));
        // SDK unistd.h:785 `int fsctl(const char *, unsigned long, void *,
        // unsigned int)` — the FULL 4-param shape must decode: the u64 request
        // param is the one the sys/ioctl.json _IOWR encoding widens into at
        // sqlite os_unix.c:3013, so a truncated/reordered decode here would
        // corrupt that call's request value silently.
        expectMachoOnlyFn(m, "fsctl", K::I32,
                          {K::Ptr, K::U64, K::Ptr, K::U32},
                          {K::Char, std::nullopt, K::Void, std::nullopt});
    }

    DarwinBsdClusterRead e;
    ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, "x86_64",
                                                 ObjectFormatKind::Elf, e));
    auto const* sym = findDarwinBsdSymbol(e, "fsctl");
    ASSERT_NE(sym, nullptr) << "fsctl must still decode on elf";
    EXPECT_FALSE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                               ObjectFormatKind::Elf));
    auto const* close_ = findDarwinBsdSymbol(e, "close");
    ASSERT_NE(close_, nullptr) << "positive control: close must be present";
    EXPECT_TRUE(close_->availableObjectFormats.empty())
        << "positive control: close ships with no per-symbol set";
}

// D-LK-SQLITE-MACHO-UNDEFINED-LIBSYSTEM-SYMBOLS — the five Darwin filesystem/
// sysctl externs sqlite's DEFAULT macho path references (os_unix.c: flock,
// statfs, fstatfs; test1.c: sysctl; mem1.c: sysctlbyname). Unlike the AFP
// cluster above these are NOT behind SQLITE_ENABLE_LOCKING_STYLE, so each was a
// hard K_SymbolUndefined at link until its row existed — DSS imports only
// DECLARED shipped externs.
//
// Every signature is asserted EXACTLY (arity, each param kind, each pointee),
// not merely "the name decodes": these are raw syscall wrappers whose args are
// pointers to caller-owned buffers, so a silently wrong width or a dropped
// param is an ABI defect that still links and still runs. The two that would
// bite hardest are sysctl/sysctlbyname, where `size_t *oldlenp` is an IN-OUT
// buffer-length cell — decode it as ptr<u32> and the callee writes 8 bytes into
// a 4-byte view of the caller's stack slot.
TEST(ShippedLibDescriptor, RealUnistdJsonDarwinFsSysctlMachoOnly) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "unistd.json";

    using K = TypeKind;
    for (std::string_view arch : {"x86_64", "arm64"}) {
        DarwinBsdClusterRead m;
        ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, arch,
                                                     ObjectFormatKind::MachO, m));
        // SDK sys/fcntl.h:616 `int flock(int, int)` — NOT sys/file.h, whose
        // __BEGIN_DECLS block is EMPTY (it only re-includes sys/fcntl.h).
        expectMachoOnlyFn(m, "flock", K::I32, {K::I32, K::I32},
                          {std::nullopt, std::nullopt});
        // SDK sys/mount.h:460/:442. `struct statfs *` keeps the fsctl/futimes
        // opaque ptr<void> spelling — no consumer needs its layout, and the
        // real type still reaches mem1.c/os_unix.c through the SDK header.
        expectMachoOnlyFn(m, "statfs", K::I32, {K::Ptr, K::Ptr},
                          {K::Char, K::Void});
        expectMachoOnlyFn(m, "fstatfs", K::I32, {K::I32, K::Ptr},
                          {std::nullopt, K::Void});
        // SDK sys/sysctl.h:800 `int sysctl(int *, u_int, void *, size_t *,
        // void *, size_t)` — u_int is 4-byte unsigned (sys/_types/_u_int.h:30),
        // size_t/size_t* are 8-byte unsigned on every macho64 format (all eight
        // declare dataModel LP64), which is exactly why the row may ship a FLAT
        // signature with no signatureByDataModel.
        expectMachoOnlyFn(m, "sysctl", K::I32,
                          {K::Ptr, K::U32, K::Ptr, K::Ptr, K::Ptr, K::U64},
                          {K::I32, std::nullopt, K::Void, K::U64, K::Void,
                           std::nullopt});
        // SDK sys/sysctl.h:802 — same tail, name-keyed head.
        expectMachoOnlyFn(m, "sysctlbyname", K::I32,
                          {K::Ptr, K::Ptr, K::Ptr, K::Ptr, K::U64},
                          {K::Char, K::Void, K::U64, K::Void, std::nullopt});
    }

    // Elf read: rows persist in the decode (gating is at injection, not decode);
    // the gate predicate IS the elf absence. Positive control: close (ungated).
    DarwinBsdClusterRead e;
    ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, "x86_64",
                                                 ObjectFormatKind::Elf, e));
    for (auto const* name : {"flock", "statfs", "fstatfs", "sysctl",
                             "sysctlbyname"}) {
        auto const* sym = findDarwinBsdSymbol(e, name);
        ASSERT_NE(sym, nullptr) << name << " must still decode on elf";
        EXPECT_FALSE(objectFormatInAvailabilitySet(sym->availableObjectFormats,
                                                   ObjectFormatKind::Elf))
            << name << " must NOT be injected on elf: glibc exports no "
                       "sysctlbyname, and declaring it there would plant an "
                       "undefined import (DSS eager-imports every shipped extern)";
    }
    auto const* closeCtl = findDarwinBsdSymbol(e, "close");
    ASSERT_NE(closeCtl, nullptr) << "positive control: close must be present";
    EXPECT_TRUE(closeCtl->availableObjectFormats.empty())
        << "positive control: close ships with no per-symbol set";
}

TEST(ShippedLibDescriptor, RealUnistdJsonUuidTMachoOnlyTypedef) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "unistd.json";

    auto findTypedef = [](DarwinBsdClusterRead const& r,
                          std::string_view name) -> TypeId {
        for (auto const& td : r.desc->typedefs)
            if (td.name == name) return td.type;
        return {};
    };

    // ── macho (both arches): uuid_t PRESENT as Array of U8, length 16 ──
    // `typedef unsigned char __darwin_uuid_t[16]` (SDK sys/_types.h:89) — the
    // 16 is load-bearing: sqlite os_unix.c:7607 asserts PROXY_HOSTIDLEN ==
    // sizeof(uuid_t), and the shipped_bsd_random_futimes_uuid_macho corpus
    // exit arithmetic IS sizeof(uuid_t).
    for (std::string_view arch : {"x86_64", "arm64"}) {
        DarwinBsdClusterRead m;
        ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, arch,
                                                     ObjectFormatKind::MachO, m));
        TypeId const t = findTypedef(m, "uuid_t");
        ASSERT_TRUE(t.valid())
            << "uuid_t must be injected on macho (arch=" << arch << ") — "
               "RED-ON-DISABLE: deleting the macho variant reds this";
        ASSERT_EQ(m.interner.kind(t), TypeKind::Array) << "uuid_t is an ARRAY";
        auto const ops = m.interner.operands(t);
        ASSERT_EQ(ops.size(), 1u);
        EXPECT_EQ(m.interner.kind(ops[0]), TypeKind::U8)
            << "uuid_t element is unsigned char (u8)";
        auto const lens = m.interner.scalars(t);
        ASSERT_EQ(lens.size(), 1u);
        EXPECT_EQ(lens[0], 16) << "uuid_t is 16 bytes (PROXY_HOSTIDLEN)";
        // The whole shape in one interned identity (kind+element+length).
        EXPECT_EQ(t, m.interner.array(m.interner.primitive(TypeKind::U8), 16))
            << "uuid_t must BE arr<u8, 16>";
    }

    // ── elf: ABSENT — typedef variants select AT DECODE (unlike symbols), so
    // zero matching variants means uuid_t is simply not in desc->typedefs.
    // Positive control on the SAME read: the `close` symbol must be present,
    // so a descriptor that failed to load (or a typedefs array that silently
    // decoded to empty-everything) cannot satisfy this vacuously. ──
    DarwinBsdClusterRead e;
    ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, "x86_64",
                                                 ObjectFormatKind::Elf, e));
    EXPECT_FALSE(findTypedef(e, "uuid_t").valid())
        << "uuid_t is Darwin-only (__darwin_uuid_t) and must NOT be injected "
           "on elf — RED-ON-DISABLE: adding an elf variant reds this";
    EXPECT_NE(findDarwinBsdSymbol(e, "close"), nullptr)
        << "positive control: the elf read must still decode the symbol "
           "surface (guards vacuous ABSENT passes)";
}

// D-FFI-DARWIN-SYSCTL-CONSTANTS-EMPTY — the DECLARED numbers of the Darwin
// sysctl MIB constants, and the COMPLETENESS of the two domains they form.
//
// sqlite src/test1.c:9100-9104 addresses hw.availcpu / hw.ncpu by MIB ARRAY
// (`nm[0] = CTL_HW; nm[1] = HW_AVAILCPU;`), which is why CTL_HW / HW_NCPU /
// HW_AVAILCPU had to ship at all; the other 36 rows are here because a PARTIAL
// constant domain is the trap sys/file.json's LOCK_* comment names — a name
// that exists in the real header, is absent here, and therefore compiles to a
// fail-loud miss on an include this descriptor shadows totally.
//
// EVERY value below was MEASURED on a real Mac (macOS 26.5.2 build 25F84,
// Apple clang 21.0.0) by COMPILING rather than by reading a header, with four
// agreeing instruments: `cc -E -dM`, the `.long` emitted by `cc -S` for
// `const int x = <NAME>;`, `#pragma message` stringification, and a native
// arm64 run plus an x86_64 (Rosetta) run.
//
// WHY BOTH ARCHES ARE READ even though every row is FLAT: the flatness is a
// MEASURED RESULT, not a default. The `-dM` dumps for `-arch arm64` and
// `-arch x86_64` were diffed over the entire CTL_*/HW_* surface (48 lines
// each) and are byte-identical, which is exactly the check `_SC_PAGESIZE` in
// unistd.json FAILS (elf 30 / macho 29, hence its `variants`). Reading both
// arches here is what would catch someone later adding an arch variant that
// contradicts the measurement.
//
// ★ WHAT THIS TEST CANNOT DO, so it is not mistaken for the whole guard: a
// number that is internally consistent but points at the WRONG kernel node is
// invisible here — asserting CTL_HW == 6 beside a row that says 6 proves only
// that both were typed the same way. That check requires the kernel and lives
// in examples/c-subset/shipped_sysctl_mib_hw_macho, which cross-checks each MIB
// against the sysctlbyname NAME form of the same node on a real Mac.
// MEASURED by demonstration: perturbing HW_MEMSIZE 24 -> 3 still BUILDS
// (rc 0) and the example then exits 15 instead of 42.
//
// RED-ON-DISABLE: delete any row, or perturb any value, and the matching
// EXPECT_EQ below fails for both arches; drop a row from either domain and the
// contiguity check also names the gap.
TEST(ShippedLibDescriptor, RealSysSysctlJsonMibConstantDomains) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "sys" / "sysctl.json";

    struct Row { char const* name; std::int64_t value; };
    // The CTL_ top level: contiguous 0..8, plus its two bounds.
    Row const ctl[] = {
        {"CTL_UNSPEC", 0}, {"CTL_KERN", 1}, {"CTL_VM", 2}, {"CTL_VFS", 3},
        {"CTL_NET", 4}, {"CTL_DEBUG", 5}, {"CTL_HW", 6}, {"CTL_MACHDEP", 7},
        {"CTL_USER", 8}, {"CTL_MAXID", 9}, {"CTL_MAXNAME", 12},
    };
    // The HW_ second level: contiguous 1..27, plus HW_MAXID 28.
    Row const hw[] = {
        {"HW_MACHINE", 1}, {"HW_MODEL", 2}, {"HW_NCPU", 3}, {"HW_BYTEORDER", 4},
        {"HW_PHYSMEM", 5}, {"HW_USERMEM", 6}, {"HW_PAGESIZE", 7},
        {"HW_DISKNAMES", 8}, {"HW_DISKSTATS", 9}, {"HW_EPOCH", 10},
        {"HW_FLOATINGPT", 11}, {"HW_MACHINE_ARCH", 12}, {"HW_VECTORUNIT", 13},
        {"HW_BUS_FREQ", 14}, {"HW_CPU_FREQ", 15}, {"HW_CACHELINE", 16},
        {"HW_L1ICACHESIZE", 17}, {"HW_L1DCACHESIZE", 18}, {"HW_L2SETTINGS", 19},
        {"HW_L2CACHESIZE", 20}, {"HW_L3SETTINGS", 21}, {"HW_L3CACHESIZE", 22},
        {"HW_TB_FREQ", 23}, {"HW_MEMSIZE", 24}, {"HW_AVAILCPU", 25},
        {"HW_TARGET", 26}, {"HW_PRODUCT", 27}, {"HW_MAXID", 28},
    };

    for (std::string_view arch : {"x86_64", "arm64"}) {
        DarwinBsdClusterRead m;
        ASSERT_NO_FATAL_FAILURE(readDarwinBsdCluster(path, arch,
                                                     ObjectFormatKind::MachO, m));
        auto find = [&](std::string_view n) -> ShippedConstant const* {
            for (auto const& c : m.desc->constants)
                if (c.name == n) return &c;
            return nullptr;
        };
        // The three sqlite src/test1.c actually reaches, named separately so a
        // failure says WHY the row exists rather than only that a number moved.
        for (auto const* n : {"CTL_HW", "HW_NCPU", "HW_AVAILCPU"})
            ASSERT_NE(find(n), nullptr)
                << n << " is referenced by sqlite src/test1.c:9100-9104 — "
                        "without it the macho testfixture does not compile "
                        "(error[S0001], MEASURED)";

        for (auto const* tbl : {&ctl[0], &hw[0]}) {
            std::size_t const n = (tbl == &ctl[0]) ? std::size(ctl) : std::size(hw);
            for (std::size_t i = 0; i < n; ++i) {
                auto const* c = find(tbl[i].name);
                ASSERT_NE(c, nullptr) << tbl[i].name << " missing (arch="
                                      << arch << ")";
                EXPECT_EQ(c->value, tbl[i].value)
                    << tbl[i].name << " (arch=" << arch << ") — MEASURED on "
                       "macOS 26.5.2; a wrong MIB number does NOT fail to "
                       "compile, it queries the wrong kernel node";
                EXPECT_EQ(m.interner.kind(c->type), TypeKind::I32)
                    << tbl[i].name << " is an int MIB id (sysctl takes int*)";
            }
        }

        // DOMAIN COMPLETENESS, not merely per-row correctness: the ids must be
        // contiguous with no hole, which is what makes "ship the whole closed
        // domain" checkable instead of aspirational. A dropped row is caught
        // here even if every surviving row is right.
        for (std::int64_t want = 0; want <= 8; ++want) {
            bool seen = false;
            for (auto const& r : ctl)
                if (r.value == want && std::string_view{r.name} != "CTL_MAXID")
                    seen = true;
            EXPECT_TRUE(seen) << "CTL_ top level has a hole at id " << want;
        }
        for (std::int64_t want = 1; want <= 27; ++want) {
            bool seen = false;
            for (auto const& r : hw) if (r.value == want) seen = true;
            EXPECT_TRUE(seen) << "HW_ level has a hole at id " << want;
        }
        EXPECT_EQ(m.desc->constants.size(), std::size(ctl) + std::size(hw))
            << "the descriptor must ship EXACTLY the two closed domains "
               "(arch=" << arch << ") — a row added without updating this test "
               "is either an unmeasured value or a partial third domain";

        // The deliberate OMISSIONS. Each of KERN_/VM_/USER_/CTLTYPE_ is its own
        // closed domain with ZERO in-tree consumer, and a domain nobody calls is
        // where a transcription error would sit unchecked. Pinned so that a
        // future PARTIAL add is caught: shipping one KERN_* row means shipping
        // the KERN_* domain whole and extending this test.
        for (auto const& c : m.desc->constants) {
            std::string_view const n{c.name};
            EXPECT_FALSE(n.rfind("KERN_", 0) == 0 || n.rfind("VM_", 0) == 0
                         || n.rfind("USER_", 0) == 0
                         || n.rfind("CTLTYPE_", 0) == 0)
                << n << " belongs to a domain this descriptor deliberately does "
                        "not ship — ship it WHOLE (the sys/file.json LOCK_* "
                        "discipline) and extend this test, or not at all";
        }

        // Positive control on the SAME read: the two functions must still be
        // there, so a descriptor that failed to decode cannot satisfy the
        // ABSENT assertions above vacuously.
        EXPECT_NE(findDarwinBsdSymbol(m, "sysctl"), nullptr)
            << "positive control: sysctl must decode on macho";
        EXPECT_NE(findDarwinBsdSymbol(m, "sysctlbyname"), nullptr)
            << "positive control: sysctlbyname must decode on macho";
    }
}


// ── TF-C97 (D-FFI-DESCRIPTOR-CROSS-FILE-TYPE-IDENTITY): two DESCRIPTORS, one
// ── `timespec` — the PREMISE this cycle had to measure before it could design
//
// The anchor was opened on the inferred claim that `time.json` and
// `sys/stat.json` declare a textually identical `timespec` and "intern DISTINCT
// TypeIds". MEASURED here, into ONE interner exactly as a real compile does:
// they intern the SAME TypeId. The complete-at-once composite path derives its
// `declSiteKey` from the FIELD CONTENT, so two byte-identical rows collapse —
// and had they NOT, `ShippedTypeConsistency` would have reported
// F_ShippedTypeIdentityConflict on the second read rather than letting a second
// type through silently.
//
// So this pin is a GUARD, not the fix's witness: the day someone re-spells one
// of the two rows `i64 "long"` (the exact `struct timeval` divergence this
// project has already shipped three times), the identity splits and this goes
// RED — one file above the place the user would otherwise meet it, as an
// unexplained assignment error. The cross-ORIGIN half of the anchor (a SOURCE
// declaration of the same tag shadowing the injected one) is not visible at this
// tier at all; it is pinned in
// tests/analysis/semantic/test_semantic_analyzer_c_subset.cpp.
//
// RED-ON-DISABLE (MEASURED by demonstration): change either descriptor's
// `tv_sec` to `i64 "long"` and both the tag EQ and the member EQ go red.
TEST(ShippedLibDescriptor, RealTimeAndSysStatShareOneTimespecTypeId) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";

    auto structNamed = [](ShippedLibDescriptor const& d,
                          std::string_view sname) -> ShippedStruct const* {
        for (auto const& s : d.structs)
            if (s.name == sname) return &s;
        return nullptr;
    };
    auto fieldNamed = [](ShippedStruct const& s,
                         std::string_view fname) -> ShippedField const* {
        for (auto const& f : s.fields)
            if (f.name == fname) return &f;
        return nullptr;
    };

    // ONE interner + ONE registry — the arrangement the semantic phase uses, so
    // the identity measured here is the identity a compile sees.
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto timeDesc = readShippedLibDescriptor(
        root / "time.json", interner, typeReg, rep, DataModel::Lp64,
        std::string_view{"arm64"}, ObjectFormatKind::MachO);
    auto statDesc = readShippedLibDescriptor(
        root / "sys" / "stat.json", interner, typeReg, rep, DataModel::Lp64,
        std::string_view{"arm64"}, ObjectFormatKind::MachO);
    ASSERT_TRUE(timeDesc.has_value());
    ASSERT_TRUE(statDesc.has_value());
    EXPECT_FALSE(rep.hasErrors());

    auto const* fromTime = structNamed(*timeDesc, "timespec");
    auto const* fromStat = structNamed(*statDesc, "timespec");
    ASSERT_NE(fromTime, nullptr) << "time.json must declare struct timespec";
    ASSERT_NE(fromStat, nullptr) << "sys/stat.json must declare struct timespec";

    // THE DIRECT IDENTITY ASSERTION — exact TypeIds, not "compatible".
    EXPECT_EQ(fromTime->typeId, fromStat->typeId)
        << "two descriptors declaring a byte-identical `timespec` must intern "
           "ONE TypeId; a split here is the cross-file identity defect at the "
           "descriptor tier";

    // And the MEMBER — the surface that actually breaks when identity splits.
    auto const* st = structNamed(*statDesc, "stat");
    ASSERT_NE(st, nullptr);
    auto const* mtimespec = fieldNamed(*st, "st_mtimespec");
    ASSERT_NE(mtimespec, nullptr) << "the macho variant must carry st_mtimespec";
    EXPECT_EQ(mtimespec->type, fromTime->typeId)
        << "`stat.st_mtimespec` must denote the SAME timespec `struct timespec "
           "x;` resolves to — this is the assignment sqlite os_unix.c:7731 makes";

    // Positive control: the shared type is the real 16-byte {i64,i64}, so the
    // EQ above cannot pass vacuously on two invalid/empty ids.
    EXPECT_TRUE(fromTime->typeId.valid());
    auto const layout = computeLayout(fromTime->typeId, interner, kNatural16,
                                      DataModel::Lp64);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->size, 16u);
    EXPECT_EQ(interner.kind(fromTime->typeId), TypeKind::Struct);
}

} // namespace
