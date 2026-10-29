// P69 (lane `cs`, fold f26b; D-LK-OBJECT-STV-PROTECTED-DROPPED) — A VISIBILITY THE TARGET'S FORMAT CANNOT WRITE IS
// WRITTEN AS ITS DOCUMENT SAYS, AND THE PROGRAM IS TOLD.
//
// C takes all four GNU visibilities; ELF writes all four (STV_*), and Mach-O's and COFF's symbol tables have no field
// for `protected` and none for `internal`. What each format's compilers write in their place is the format
// document's `visibilityWrittenAs` (✔MEASURED, lane cs probe pv, 2026-10-16: Apple clang 21.0.0 writes protected as
// a plain external, warning "target does not support 'protected' visibility; using 'default'", and internal,
// silently, as a private external; mingw-w64 gcc 13.2.0 ignores every visibility, warning "visibility attribute not
// supported in this configuration; ignored"). The pipeline settles each such visibility once per unit, right after
// the unit is lowered to HIR (`settleVisibilitiesWrittenAs`, hir/attributes/visibility_written_as.hpp), and warns
// `H_VisibilityWrittenAsAnother` — so what the writers see, and write, is the document's answer:
//   * the shipped documents: the four PE and the eight Mach-O state protected -> default and internal -> hidden;
//     the ten ELF documents state nothing;
//   * a document whose statement cannot be read is refused at load: `default` as a key, a stand-in the same document
//     says the format cannot write, a word that is no visibility, an empty statement;
//   * each unit is settled to its target document's statement, a synthetic document's included — the document
//     decides — and each settled declaration is warned at its position; an ELF unit keeps all four, in silence;
//   * an ALIAS is settled by the same statement — its declaration and the alias record the writers read — and
//     the program is told ONCE, at the alias's declaration;
//   * the objects the writers make of the shipped documents hold the stand-ins (Mach-O and COFF, read back).
// Built and decoded on every host; nothing is run.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "mir/mir.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

constexpr char const* kPeAndMachODocuments[] = {
    "pe64-x86_64-windows",          "pe64-x86_64-windows-exec",       "pe64-x86_64-windows-dll",
    "pe64-x86_64-windows-staticlib", "macho64-arm64-darwin",          "macho64-arm64-darwin-exec",
    "macho64-arm64-darwin-dylib",   "macho64-arm64-darwin-staticlib", "macho64-x86_64-darwin",
    "macho64-x86_64-darwin-exec",   "macho64-x86_64-darwin-dylib",    "macho64-x86_64-darwin-staticlib",
};
constexpr char const* kElfDocuments[] = {
    "elf64-x86_64-linux",     "elf64-x86_64-linux-exec",     "elf64-x86_64-linux-pie",
    "elf64-x86_64-linux-dyn", "elf64-x86_64-linux-staticlib", "elf64-aarch64-linux",
    "elf64-aarch64-linux-exec", "elf64-aarch64-linux-pie",   "elf64-aarch64-linux-dyn",
    "elf64-aarch64-linux-staticlib",
};

// The shipped document `name` with its `visibilityWrittenAs` replaced by `statement` (absent: removed).
[[nodiscard]] std::string documentStating(char const* name, std::optional<nlohmann::json> const& statement) {
    std::ifstream in{dss::test::configRoot() / "object-formats" / (std::string{name} + ".format.json"),
                     std::ios::binary};
    auto doc = nlohmann::json::parse(in);
    doc.erase("visibilityWrittenAs");
    if (statement.has_value()) doc["visibilityWrittenAs"] = *statement;
    return doc.dump();
}

constexpr char kUnit[] =
    "__attribute__((visibility(\"protected\"))) int dss_vp_fn(void) { return 1; }\n"
    "__attribute__((visibility(\"internal\"))) int dss_vi_fn(void) { return 2; }\n"
    "int dss_vp_obj __attribute__((visibility(\"protected\"))) = 3;\n"
    "int dss_vi_obj __attribute__((visibility(\"internal\"))) = 4;\n"
    "__attribute__((visibility(\"hidden\"))) int dss_vh_fn(void) { return 5; }\n"
    "int dss_plain(void) { return dss_vp_fn() + dss_vi_fn() + dss_vp_obj + dss_vi_obj + dss_vh_fn(); }\n";

struct Toolchain {
    std::shared_ptr<GrammarSchema const>      grammar;
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
    std::uint16_t                             ccIndex = 0;
};

[[nodiscard]] std::optional<Toolchain> toolchain(char const* arch, std::shared_ptr<ObjectFormatSchema const> format) {
    auto grammarR = GrammarSchema::loadShipped("c");
    auto targetR  = TargetSchema::loadShipped(arch);
    EXPECT_TRUE(grammarR.has_value() && targetR.has_value() && format != nullptr) << arch;
    if (!grammarR || !targetR || format == nullptr) return std::nullopt;
    DiagnosticReporter rep;
    auto const         abi = ffi::resolveAbi(**targetR, *format, rep);
    EXPECT_TRUE(abi.has_value() && abi->cc != nullptr) << diagnosticsOf(rep);
    if (!abi.has_value() || abi->cc == nullptr) return std::nullopt;
    auto const ccSpan = (*targetR)->callingConventions();
    Toolchain  tc{*grammarR, *targetR, std::move(format), 0};
    tc.ccIndex = static_cast<std::uint16_t>(std::distance(ccSpan.data(), abi->cc));
    return tc;
}

struct Settled {
    bool                                            built = false;
    std::vector<std::pair<std::string, SymbolVisibility>> visibilities;   // each definition of the unit, by name
    std::size_t                                     warned = 0;            // H_VisibilityWrittenAsAnother
    std::size_t                                     warnedWithoutPosition = 0;
    std::string                                     diagnostics;
};

// The unit compiled to MIR for `tc`'s format: each definition's visibility as MIR states it, and the warnings.
[[nodiscard]] Settled settle(Toolchain const& tc) {
    UnitBuilder builder{tc.grammar, DiagnosticBudget::libraryDefault()};
    dss::applySystemDirs(builder, *tc.grammar);
    builder.addInMemory(kUnit, "visibility.c");
    CompilationUnit    cu = std::move(builder).finish();
    DiagnosticReporter rep;
    CompileOptions     opts{DiagnosticBudget::libraryDefault()};
    auto cuMir = buildCuMir(cu, *tc.grammar, *tc.target, *tc.format, tc.ccIndex, rep, opts);
    Settled out;
    out.diagnostics = diagnosticsOf(rep);
    if (!cuMir.has_value() || rep.errorCount() != 0) return out;
    out.built     = true;
    Mir const& mir = cuMir->mir;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const     f   = mir.funcAt(i);
        SymbolRecord const* rec = cuMir->model.recordFor(mir.funcSymbol(f));
        if (rec != nullptr) out.visibilities.emplace_back(rec->name, mir.funcLinkage(f).visibility);
    }
    for (std::uint32_t i = 0; i < mir.moduleGlobalCount(); ++i) {
        MirGlobalId const   g   = mir.globalAt(i);
        SymbolRecord const* rec = cuMir->model.recordFor(mir.globalSymbol(g));
        if (rec != nullptr) out.visibilities.emplace_back(rec->name, mir.globalLinkage(g).visibility);
    }
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::H_VisibilityWrittenAsAnother) continue;
        ++out.warned;
        if (!d.buffer.valid()) ++out.warnedWithoutPosition;
    }
    return out;
}

[[nodiscard]] std::optional<SymbolVisibility> visibilityOf(Settled const& s, std::string_view name) {
    for (auto const& [n, v] : s.visibilities) {
        if (n == name) return v;
    }
    return std::nullopt;
}

}  // namespace

// The shipped documents: PE and Mach-O write protected as default and internal as hidden; ELF writes all four.
// RED-ON-DISABLE: drop the key from a document, or the loader's reading of it, and that document states nothing.
TEST(VisibilityWrittenAs, EachShippedDocumentStatesWhatItsFormatWrites) {
    for (char const* name : kPeAndMachODocuments) {
        SCOPED_TRACE(name);
        auto const f = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(f.has_value()) << name;
        auto const standIns = (*f)->visibilityStandIns();
        ASSERT_EQ(standIns.size(), 2u) << "protected and internal, each with its stand-in";
        EXPECT_EQ((*f)->visibilityWrittenFor(SymbolVisibility::Protected), SymbolVisibility::Default);
        EXPECT_EQ((*f)->visibilityWrittenFor(SymbolVisibility::Internal), SymbolVisibility::Hidden);
        EXPECT_EQ((*f)->visibilityWrittenFor(SymbolVisibility::Hidden), SymbolVisibility::Hidden);
        EXPECT_EQ((*f)->visibilityWrittenFor(SymbolVisibility::Default), SymbolVisibility::Default);
    }
    for (char const* name : kElfDocuments) {
        SCOPED_TRACE(name);
        auto const f = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(f.has_value()) << name;
        EXPECT_TRUE((*f)->visibilityStandIns().empty()) << "ELF writes every visibility as itself";
        EXPECT_EQ((*f)->visibilityWrittenFor(SymbolVisibility::Protected), SymbolVisibility::Protected);
        EXPECT_EQ((*f)->visibilityWrittenFor(SymbolVisibility::Internal), SymbolVisibility::Internal);
    }
}

// A statement the loader cannot read is refused at load, at the key's own path, rather than settled into a
// visibility the writer cannot write either. RED-ON-DISABLE: drop a check of the reader, and its document loads.
TEST(VisibilityWrittenAs, AStatementThatCannotBeReadIsRefusedAtLoad) {
    struct Bad {
        char const*    why;
        nlohmann::json statement;
    };
    Bad const bad[] = {
        {"`default` is written by every format", {{"default", "hidden"}}},
        {"a stand-in the same document says the format cannot write",
         {{"protected", "internal"}, {"internal", "hidden"}}},
        {"a word that is no visibility", {{"protected", "sideways"}}},
        {"no visibility at all", nlohmann::json::object()},
        {"not an object", "default"},
    };
    ASSERT_TRUE(ObjectFormatSchema::loadFromText(documentStating("macho64-arm64-darwin-exec", std::nullopt),
                                                 "no statement")
                    .has_value())
        << "the control: the document without the key loads";
    for (Bad const& b : bad) {
        SCOPED_TRACE(b.why);
        auto const loaded = ObjectFormatSchema::loadFromText(documentStating("macho64-arm64-darwin-exec", b.statement),
                                                             b.why);
        EXPECT_FALSE(loaded.has_value()) << "a statement the loader cannot read loaded";
    }
}

// Each unit is settled to its target document's statement before any tier reads it, and every settled declaration
// is warned at its position; a document that states another stand-in writes another visibility — the DOCUMENT
// decides, not a writer's own mapping. RED-ON-DISABLE: drop the pipeline's settling step, and the Mach-O and PE units
// keep `protected` and `internal` in MIR, unwarned.
TEST(VisibilityWrittenAs, EachUnitIsSettledToItsDocumentAndTold) {
    auto const shipped = [](char const* name) -> std::shared_ptr<ObjectFormatSchema const> {
        auto f = ObjectFormatSchema::loadShipped(name);
        EXPECT_TRUE(f.has_value()) << name;
        return f.has_value() ? *f : nullptr;
    };
    auto const synthetic = []() -> std::shared_ptr<ObjectFormatSchema const> {
        auto f = ObjectFormatSchema::loadFromText(
            documentStating("macho64-arm64-darwin-exec", nlohmann::json{{"protected", "hidden"}, {"internal", "default"}}),
            "macho64-arm64-darwin-exec, protected -> hidden, internal -> default");
        EXPECT_TRUE(f.has_value());
        return f.has_value() ? *f : nullptr;
    };
    struct Case {
        char const*                               label;
        char const*                               arch;
        std::shared_ptr<ObjectFormatSchema const> format;
        SymbolVisibility                          protectedAs;
        SymbolVisibility                          internalAs;
        std::size_t                               warned;
    };
    Case const cases[] = {
        {"Mach-O arm64", "arm64", shipped("macho64-arm64-darwin-exec"), SymbolVisibility::Default,
         SymbolVisibility::Hidden, 4},
        {"PE x86_64", "x86_64", shipped("pe64-x86_64-windows-exec"), SymbolVisibility::Default,
         SymbolVisibility::Hidden, 4},
        {"ELF x86_64", "x86_64", shipped("elf64-x86_64-linux-exec"), SymbolVisibility::Protected,
         SymbolVisibility::Internal, 0},
        {"a Mach-O document stating other stand-ins", "arm64", synthetic(), SymbolVisibility::Hidden,
         SymbolVisibility::Default, 4},
    };
    for (Case const& c : cases) {
        SCOPED_TRACE(c.label);
        ASSERT_NE(c.format, nullptr);
        auto const tc = toolchain(c.arch, c.format);
        ASSERT_TRUE(tc.has_value());
        Settled const s = settle(*tc);
        ASSERT_TRUE(s.built) << s.diagnostics;
        EXPECT_EQ(visibilityOf(s, "dss_vp_fn"), std::optional{c.protectedAs}) << s.diagnostics;
        EXPECT_EQ(visibilityOf(s, "dss_vp_obj"), std::optional{c.protectedAs});
        EXPECT_EQ(visibilityOf(s, "dss_vi_fn"), std::optional{c.internalAs});
        EXPECT_EQ(visibilityOf(s, "dss_vi_obj"), std::optional{c.internalAs});
        EXPECT_EQ(visibilityOf(s, "dss_vh_fn"), std::optional{SymbolVisibility::Hidden}) << "hidden is written";
        EXPECT_EQ(visibilityOf(s, "dss_plain"), std::optional{SymbolVisibility::Default});
        EXPECT_EQ(s.warned, c.warned) << "one warning per settled declaration" << s.diagnostics;
        EXPECT_EQ(s.warnedWithoutPosition, 0u) << "a declaration's warning carries its position" << s.diagnostics;
    }
}

// The objects the writers make of the shipped documents hold the stand-ins: on Mach-O a protected definition is a
// plain external and an internal one a private external; on COFF a protected definition is an external symbol and an
// internal one is hidden by the object's directive — each read back by DSS's own readers as default and hidden.
TEST(VisibilityWrittenAs, TheObjectsHoldTheStandIns) {
    struct Pair {
        char const* target;
        char const* format;
        char const* extension;
        bool        macho;
    };
    for (Pair const& pair : {Pair{"arm64", "macho64-arm64-darwin", ".o", true},
                             Pair{"x86_64", "pe64-x86_64-windows", ".obj", false}}) {
        SCOPED_TRACE(std::string{pair.target} + ":" + pair.format);
        ScratchDir scratch{Location::InsideRepo, "visibility-written-as-object"};
        fs::path const src = scratch.path() / "unit.c";
        std::ofstream(src, std::ios::binary) << kUnit;
        Program p;
        p.setOutputDir(scratch.path());
        DiagnosticReporter rep;
        ASSERT_EQ(p.compileFiles({src.string()}, "c", {std::string{pair.target} + ":" + pair.format}, rep), 0)
            << diagnosticsOf(rep);
        std::ifstream in(scratch.path() / (std::string{"unit"} + pair.extension), std::ios::binary);
        std::vector<std::uint8_t> const bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        auto const target = TargetSchema::loadShipped(pair.target);
        auto const format = ObjectFormatSchema::loadShipped(pair.format);
        ASSERT_TRUE(target && format);
        DiagnosticReporter readRep;
        auto const object = pair.macho ? macho::readRelocatableObject(bytes, **target, **format, readRep)
                                       : pe::readRelocatableObject(bytes, **target, **format, readRep);
        ASSERT_TRUE(object.has_value()) << diagnosticsOf(readRep);
        for (auto const& [name, want] : {std::pair{"dss_vp_fn", SymbolVisibility::Default},
                                         std::pair{"dss_vp_obj", SymbolVisibility::Default},
                                         std::pair{"dss_vi_fn", SymbolVisibility::Hidden},
                                         std::pair{"dss_vi_obj", SymbolVisibility::Hidden}}) {
            SCOPED_TRACE(name);
            std::string const symbol = std::string{pair.macho ? "_" : ""} + name;
            ModuleSymbol const* row  = nullptr;
            for (auto const& s : object->symbols) {
                if (s.name == symbol) row = &s;
            }
            ASSERT_NE(row, nullptr) << "the definition is not a record of the object";
            EXPECT_EQ(row->binding, SymbolBinding::Global);
            EXPECT_EQ(row->visibility, want);
        }
    }
}

// An ALIAS is settled too, by the same statement, and the program is told ONCE: an alias's linkage is held on its
// declaration and on the alias record the writers read (`HirSymbolAlias`), both are settled, and the warning is
// the declaration's, at its position, naming the alias. Mach-O refuses an alias by name
// (`symbolAliasUnsupportedReason`), so the PE document settles one; the ELF document keeps both visibilities, in
// silence.
// RED-ON-DISABLE: skip the alias record's settling and the PE aliases reach MIR still `protected` and `internal`;
// tell the alias record too and each alias is told twice.
TEST(VisibilityWrittenAs, AnAliasIsSettledToItsDocumentAndToldOnce) {
    constexpr char kAliases[] =
        "int dss_va_target = 7;\n"
        "extern int dss_va_protected __attribute__((alias(\"dss_va_target\"), visibility(\"protected\")));\n"
        "extern int dss_va_internal __attribute__((alias(\"dss_va_target\"), visibility(\"internal\")));\n"
        "int dss_va_read(void) { return dss_va_protected + dss_va_internal; }\n";
    struct Case {
        char const*      label;
        char const*      format;
        SymbolVisibility protectedAs;
        SymbolVisibility internalAs;
        std::size_t      warned;
    };
    for (Case const& c : {Case{"PE x86_64", "pe64-x86_64-windows", SymbolVisibility::Default,
                               SymbolVisibility::Hidden, 2u},
                          Case{"ELF x86_64", "elf64-x86_64-linux", SymbolVisibility::Protected,
                               SymbolVisibility::Internal, 0u}}) {
        SCOPED_TRACE(c.label);
        auto const format = ObjectFormatSchema::loadShipped(c.format);
        ASSERT_TRUE(format.has_value());
        auto const tc = toolchain("x86_64", *format);
        ASSERT_TRUE(tc.has_value());
        UnitBuilder builder{tc->grammar, DiagnosticBudget::libraryDefault()};
        dss::applySystemDirs(builder, *tc->grammar);
        builder.addInMemory(kAliases, "aliases.c");
        CompilationUnit    cu = std::move(builder).finish();
        DiagnosticReporter rep;
        CompileOptions     opts{DiagnosticBudget::libraryDefault()};
        auto cuMir = buildCuMir(cu, *tc->grammar, *tc->target, *tc->format, tc->ccIndex, rep, opts);
        ASSERT_TRUE(cuMir.has_value() && rep.errorCount() == 0) << diagnosticsOf(rep);
        std::optional<SymbolVisibility> protectedVis;
        std::optional<SymbolVisibility> internalVis;
        for (MirAlias const& alias : cuMir->mir.aliases()) {
            SymbolRecord const* rec = cuMir->model.recordFor(alias.alias);
            ASSERT_NE(rec, nullptr);
            if (rec->name == "dss_va_protected") protectedVis = alias.linkage.visibility;
            if (rec->name == "dss_va_internal") internalVis = alias.linkage.visibility;
        }
        EXPECT_EQ(protectedVis, std::optional{c.protectedAs})
            << "the alias's visibility is not the one its format's document writes" << diagnosticsOf(rep);
        EXPECT_EQ(internalVis, std::optional{c.internalAs})
            << "the alias's visibility is not the one its format's document writes" << diagnosticsOf(rep);
        std::size_t warned = 0;
        std::size_t named  = 0;
        for (auto const& d : rep.all()) {
            if (d.code != DiagnosticCode::H_VisibilityWrittenAsAnother) continue;
            ++warned;
            EXPECT_TRUE(d.buffer.valid()) << "an alias is told at its declaration: " << d.actual;
            if (d.actual.find("'dss_va_protected'") != std::string::npos
                || d.actual.find("'dss_va_internal'") != std::string::npos) {
                ++named;
            }
        }
        EXPECT_EQ(warned, c.warned) << "each settled alias is told once" << diagnosticsOf(rep);
        EXPECT_EQ(named, c.warned) << "each warning names its alias" << diagnosticsOf(rep);
    }
}
