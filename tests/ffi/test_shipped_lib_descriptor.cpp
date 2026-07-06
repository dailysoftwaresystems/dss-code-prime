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
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
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

// Ancestor-walk for the shipped dir (mirrors `findShippedConfig`) so tests work
// whether ctest runs from build/ or the repo root. Returns empty if not found.

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

[[nodiscard]] fs::path shippedLibsRoot() {
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        fs::path const cand = here / "src" / "dss-config" / "shippedLibs";
        if (fs::exists(cand)) return cand;
        fs::path const parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
    return {};
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
    EXPECT_EQ(desc->library.at("pe"),    "msvcrt.dll");
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
        ASSERT_EQ(desc->structs.size(), 1u)
            << "timeval variant not injected for arch=" << arch;
        auto const& tv = desc->structs[0];
        EXPECT_EQ(tv.name, "timeval");
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

} // namespace
