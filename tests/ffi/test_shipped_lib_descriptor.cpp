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

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "diagnostic_count.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

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
        "library": "msvcrt.dll",
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
    EXPECT_EQ(desc->library, "msvcrt.dll");
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

// `library` is optional — absent ⇒ empty (the lowering falls back to the
// language's externLibraryByFormat default).
TEST(ShippedLibDescriptor, LibraryIsOptional) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "nolib.json", R"({
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

// An "object" kind decodes to ShippedSymbolKind::Object (→ ExternGlobal).
TEST(ShippedLibDescriptor, ObjectKindDecodes) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "obj.json", R"({
        "library": "msvcrt.dll",
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
    auto const path = writeTemp(dir, "nosyms.json", R"({ "library": "msvcrt.dll" })");

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
        "library": "msvcrt.dll",
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
        "library": "msvcrt.dll",
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
        "library": "msvcrt.dll",
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
        "library": "msvcrt.dll",
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
        "library": "msvcrt.dll",
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

} // namespace
