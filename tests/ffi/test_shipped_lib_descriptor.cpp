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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

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
// perturb the single-layout structs that ship today (timeval / the opaque structs).
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
        auto desc = readShippedLibDescriptor(entry.path(), interner, typeReg, rep);
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
        auto desc = readShippedLibDescriptor(
            root / (std::string(lib) + ".json"), interner, typeReg, rep);
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
    auto desc = readShippedLibDescriptor(root / "stdio.json", interner, typeReg, rep);
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

} // namespace
