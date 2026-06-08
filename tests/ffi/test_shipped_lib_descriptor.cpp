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
        "header": "stdio.h", "library": "msvcrt.dll",
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

// `library` is optional — absent ⇒ empty (the lowering falls back to the
// language's externLibraryByFormat default).
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

// An "object" kind decodes to ShippedSymbolKind::Object (→ ExternGlobal).
TEST(ShippedLibDescriptor, ObjectKindDecodes) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    auto const path = writeTemp(dir, "obj.json", R"({
        "header": "stdio.h", "library": "msvcrt.dll",
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
    auto const path = writeTemp(dir, "nosyms.json", R"({ "header": "x.h", "library": "msvcrt.dll" })");

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
        "header": "stdio.h", "library": "msvcrt.dll",
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
        "header": "stdio.h", "library": "msvcrt.dll",
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
        "header": "stdio.h", "library": "msvcrt.dll",
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
        "header": "stdio.h", "library": "msvcrt.dll",
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
        "header": "stdio.h", "library": "msvcrt.dll",
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
        "library": "msvcrt.dll",
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

// Every descriptor SHIPPED under src/dss-config/shippedLibs/<platform>/*.json
// must read + decode cleanly: valid JSON, a non-empty `header` that AGREES with
// the filename stem (the resolver routes `<stdlib.h>`→stdlib.json by stem, so a
// descriptor whose `header` provenance lies about its origin is a real bug), and
// EVERY symbol's `signature` decodes via the one type-text codec. A malformed
// JSON or an unencodable signature in a shipped descriptor breaks the standard-
// library surface for that platform — fail loud HERE, not at a user's `#include`.
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
            // Provenance integrity: `header` MUST match the filename stem the
            // resolver routes by (RED if a clone left stdlib.json saying stdio.h).
            EXPECT_EQ(desc->header, entry.path().stem().string() + ".h")
                << "header provenance must match the filename stem in "
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

// The LP64-vs-LLP64 ABI delta is the ENTIRE reason the linux/macos stdlib
// descriptors differ from windows: C `long` is 64-bit on LP64 (Linux/macOS) but
// 32-bit on LLP64 (Windows), so `atol`/`labs`/`strtoul` return different widths.
// `AllShippedDescriptorsDecode` only proves the signatures DECODE — both `i32`
// and `i64` are valid types, so a copy-paste leaving macos with the Windows i32
// widths (or a Win/Linux file swap) would stay GREEN there. This pins the ACTUAL
// per-platform result widths STRUCTURALLY (interner accessors, not string
// compare) so a width regression goes RED.
TEST(ShippedLibDescriptor, ShippedStdlibAbiDeltaIsLp64VsLlp64) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";

    // Find a named function symbol in <platform>/<lib>.json and return its
    // FnSig (interner kept alive by the caller via the returned descriptor).
    auto fnSigOf = [&](TypeInterner& interner, char const* platform,
                       char const* lib, char const* symName) -> TypeId {
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(
            root / platform / (std::string(lib) + ".json"), interner, typeReg, rep);
        EXPECT_TRUE(desc.has_value())
            << platform << "/" << lib << ".json failed to load";
        if (!desc.has_value()) return {};
        for (auto const& s : desc->symbols) {
            if (s.name == symName) {
                EXPECT_EQ(interner.kind(s.signature), TypeKind::FnSig)
                    << symName << " is not a function in " << platform;
                return s.signature;
            }
        }
        ADD_FAILURE() << symName << " not found in " << platform << "/" << lib;
        return {};
    };
    // The FnSig RESULT kind of <platform>/<lib>::<sym>.
    auto resultKindOf = [&](char const* platform, char const* lib,
                            char const* symName) -> TypeKind {
        TypeInterner interner{CompilationUnitId{1}};
        TypeId const sig = fnSigOf(interner, platform, lib, symName);
        return sig.valid() ? interner.kind(interner.fnResult(sig)) : TypeKind::Void;
    };
    // The FnSig PARAM[i] kind of <platform>/<lib>::<sym> (for fseek's offset).
    auto paramKindOf = [&](char const* platform, char const* lib,
                           char const* symName, std::size_t i) -> TypeKind {
        TypeInterner interner{CompilationUnitId{1}};
        TypeId const sig = fnSigOf(interner, platform, lib, symName);
        if (!sig.valid()) return TypeKind::Void;
        auto const params = interner.fnParams(sig);
        EXPECT_GT(params.size(), i) << symName << " has too few params";
        return i < params.size() ? interner.kind(params[i]) : TypeKind::Void;
    };

    // Pin EVERY `long`-bearing symbol (not a subset — a per-symbol copy-paste is
    // the exact failure mode). stdlib: atol/strtol return long, strtoul returns
    // unsigned long, labs takes+returns long. stdio: ftell returns long, fseek's
    // offset (param[1]) is long.
    for (char const* lp64 : {"linux-x86_64", "macos-arm64"}) {
        // LP64: `long` = 64-bit → i64; `unsigned long` → u64.
        EXPECT_EQ(resultKindOf(lp64, "stdlib", "atol"),    TypeKind::I64) << lp64;
        EXPECT_EQ(resultKindOf(lp64, "stdlib", "strtol"),  TypeKind::I64) << lp64;
        EXPECT_EQ(resultKindOf(lp64, "stdlib", "strtoul"), TypeKind::U64) << lp64;
        EXPECT_EQ(resultKindOf(lp64, "stdlib", "labs"),    TypeKind::I64) << lp64;
        EXPECT_EQ(resultKindOf(lp64, "stdio",  "ftell"),   TypeKind::I64) << lp64;
        EXPECT_EQ(paramKindOf(lp64,  "stdio",  "fseek", 1), TypeKind::I64) << lp64;
    }
    // LLP64 (Windows): `long` = 32-bit → i32; `unsigned long` → u32.
    EXPECT_EQ(resultKindOf("windows-x86_64", "stdlib", "atol"),    TypeKind::I32);
    EXPECT_EQ(resultKindOf("windows-x86_64", "stdlib", "strtol"),  TypeKind::I32);
    EXPECT_EQ(resultKindOf("windows-x86_64", "stdlib", "strtoul"), TypeKind::U32);
    EXPECT_EQ(resultKindOf("windows-x86_64", "stdlib", "labs"),    TypeKind::I32);
    EXPECT_EQ(resultKindOf("windows-x86_64", "stdio",  "ftell"),   TypeKind::I32);
    EXPECT_EQ(paramKindOf("windows-x86_64",  "stdio",  "fseek", 1), TypeKind::I32);
}

// `standard` is optional provenance — it round-trips when present, and a
// non-string `standard` fails loud (it is type-checked on read). Brand-new field,
// so pin both the populate path and the rejection path directly.
TEST(ShippedLibDescriptor, StandardProvenanceRoundTripsAndTypeChecks) {
    ScratchDir dir{Location::Temp, "shipped-lib"};
    {
        auto const path = writeTemp(dir, "std.json", R"({
            "header": "stdio.h", "standard": "c99", "library": "msvcrt.dll",
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
