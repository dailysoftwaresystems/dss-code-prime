// P69 (lane `cs`) — `alias("target")` COMPILED FROM C, THROUGH EVERY TIER, INTO A RELOCATABLE OBJECT.
//
// The semantic tier resolves each alias to its definition, the HIR carries it (`HirSymbolAlias`), the
// MIR module states it (`MirAlias`), DCE roots its target, and the object producer writes it as ONE
// MORE SYMBOL ROW on the target's definition with the alias's own binding (what gcc, clang and
// mingw-w64 gcc write, and what every object reader makes of a foreign alias). These cases compile
// C source with the driver, at the baseline and at `--config=release`, for an ELF object on two
// architectures and a PE object, and read the object back with DSS's own readers:
//   * each alias's name is defined by the object, on the SAME symbol as its target's name;
//   * a weak alias is a weak definition of its name, a strong one a global one;
//   * an alias of a module-private (`static`) definition nothing else calls keeps that definition at
//     release (DCE roots every alias target);
//   * the OBJECT shapes the review of the alias fold named that an object can hold — `static` and
//     `extern` object aliases of `static` and external objects, a hidden alias, a `static` alias of
//     another type — name their target's storage, each value stored once, and a WEAK alias of a
//     `static` body is a weak definition of that body.
// And the formats' side of the decision, which these documents state and the front end reads:
//   * a Mach-O target refuses an alias BY NAME (every Apple clang refuses one: "aliases are not
//     supported on darwin") — the Mach-O documents' `symbolAliasUnsupportedReason`, and no other
//     document's;
//   * a document states commons exactly where it states how a common is resolved
//     (`ObjectFormatSchema::statesCommons`), and where it states none `common` is refused.
// Nothing here runs a binary: examples/c/symbol_alias_second_name runs one on three targets.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::vector<std::uint8_t> readFileBytes(fs::path const& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    auto const end = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

fs::path writeText(fs::path const& dir, std::string_view name, std::string_view text) {
    fs::path const path = dir / name;
    std::ofstream  out(path, std::ios::binary);
    out << text;
    return path;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

// One relocatable-object pair, the reader of its format and its artifact's extension.
struct ObjectPair {
    char const* target;
    char const* format;
    char const* extension;
    bool        pe;
};
constexpr ObjectPair kPairs[] = {
    {"x86_64", "elf64-x86_64-linux", ".o", false},
    {"arm64", "elf64-aarch64-linux", ".o", false},
    {"x86_64", "pe64-x86_64-windows", ".obj", true},
};

constexpr CompileConfig kConfigs[] = {CompileConfig::Debug, CompileConfig::Release};

// Every shape of alias the object must carry, and a caller of each name so none is only declared.
constexpr char kAliasUnit[] =
    "int dss_add(int a, int b) { return a + b; }\n"
    "int dss_sum(int, int) __attribute__((alias(\"dss_add\")));\n"
    "int dss_weak_sum(int, int) __attribute__((weak, alias(\"dss_add\")));\n"
    "int dss_counter = 5;\n"
    "extern int dss_counter_too __attribute__((alias(\"dss_counter\")));\n"
    "static int dss_private_body(void) { return 7; }\n"
    "int dss_exported_body(void) __attribute__((alias(\"dss_private_body\")));\n"
    "int dss_tentative;\n"
    "extern int dss_tentative_too __attribute__((alias(\"dss_tentative\")));\n"
    "int dss_use(void) { return dss_sum(1, 2) + dss_weak_sum(3, 4) + dss_counter_too + dss_tentative_too; }\n";

// The OBJECT shapes of an alias the review of the alias fold asked for, each with a value no other object of the
// unit holds, so its storage is found by its BYTES:
//   * object aliases written `static` — of a `static` object, of an external one — and an external alias of a
//     `static` object (gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0 run each: ✔MEASURED, lane `cs` probe rv
//     cells 1-3);
//   * a hidden alias, a protected one (rv cell 8; a linkage specifier of the language since fold f26b — before
//     it, warned and dropped, ✔MEASURED on this case's first run) and a `static` alias of an object of another
//     type (probe rv cells 7 and 9: they run wherever they compile; mingw-w64 gcc ignores the visibility,
//     warned). NOT HERE: a thread-local alias (rv cell 6) — no thread-local definition can be written to a
//     relocatable object yet, which is a row of the link tier's own (examples/c/symbol_alias_second_name runs
//     one in an image);
//   * a WEAK alias of a `static` body (mingw-w64 gcc 13.2.0 writes the pe64 object and runs it, probe al3 cell 22;
//     its form, ✔MEASURED by probe pw: the body under an EXTERNAL default record, the weak name a weak external
//     of it).
constexpr char kObjectShapesUnit[] =
    "static int dss_s = 1101;\n"
    "static int dss_s_too __attribute__((alias(\"dss_s\")));\n"
    "extern int dss_s_ext __attribute__((alias(\"dss_s\")));\n"
    "int dss_e = 1202;\n"
    "static int dss_e_private __attribute__((alias(\"dss_e\")));\n"
    "int dss_h = 1404;\n"
    "extern int dss_h_hidden __attribute__((alias(\"dss_h\"), visibility(\"hidden\")));\n"
    "int dss_p = 1606;\n"
    "extern int dss_p_protected __attribute__((alias(\"dss_p\"), visibility(\"protected\")));\n"
    "static int dss_word = 1505;\n"
    "static unsigned char dss_word_low __attribute__((alias(\"dss_word\")));\n"
    "static int dss_private_fn(void) { return 42; }\n"
    "int dss_weak_fn(void) __attribute__((weak, alias(\"dss_private_fn\")));\n"
    "int dss_use_shapes(void) {\n"
    "    return dss_s_too + dss_s_ext + dss_e_private + dss_h_hidden + dss_p_protected + dss_word_low\n"
    "           + dss_weak_fn();\n"
    "}\n";

// The 32-bit little-endian value `v` as the bytes of an object holding it.
[[nodiscard]] std::vector<std::uint8_t> wordBytes(std::uint32_t v) {
    return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16),
            static_cast<std::uint8_t>(v >> 24)};
}

}  // namespace

TEST(CompiledSymbolAliasObject, EachAliasIsOneMoreNameOnItsTargetsDefinitionInTheObject) {
    struct Expect {
        char const*   alias;
        char const*   target;
        SymbolBinding binding;
    };
    constexpr Expect kExpected[] = {
        {"dss_sum", "dss_add", SymbolBinding::Global},
        {"dss_weak_sum", "dss_add", SymbolBinding::Weak},
        {"dss_counter_too", "dss_counter", SymbolBinding::Global},
        {"dss_tentative_too", "dss_tentative", SymbolBinding::Global},
    };
    for (ObjectPair const& pair : kPairs) {
        for (CompileConfig const config : kConfigs) {
            SCOPED_TRACE(std::string{pair.target} + ":" + pair.format + ", "
                         + (config == CompileConfig::Release ? "release" : "baseline"));
            ScratchDir scratch{Location::InsideRepo, "compiled-alias-object"};
            auto const src = writeText(scratch.path(), "unit.c", kAliasUnit);
            Program    p;
            p.setOutputDir(scratch.path());
            p.setCompileConfig(config);
            DiagnosticReporter rep;
            ASSERT_EQ(p.compileFiles({src.string()}, "c", {std::string{pair.target} + ":" + pair.format}, rep), 0)
                << diagnosticsOf(rep);
            auto const bytes  = readFileBytes(scratch.path() / (std::string{"unit"} + pair.extension));
            auto const target = TargetSchema::loadShipped(pair.target);
            auto const format = ObjectFormatSchema::loadShipped(pair.format);
            ASSERT_TRUE(target && format);
            DiagnosticReporter readRep;
            auto const object = pair.pe ? pe::readRelocatableObject(bytes, **target, **format, readRep)
                                        : elf::readRelocatableObject(bytes, **target, **format, readRep);
            ASSERT_TRUE(object.has_value()) << diagnosticsOf(readRep);
            auto const rowNamed = [&](std::string_view name) -> ModuleSymbol const* {
                for (auto const& s : object->symbols) {
                    if (s.name == name) return &s;
                }
                return nullptr;
            };
            for (Expect const& e : kExpected) {
                SCOPED_TRACE(e.alias);
                ModuleSymbol const* aliasRow  = rowNamed(e.alias);
                ModuleSymbol const* targetRow = rowNamed(e.target);
                ASSERT_NE(targetRow, nullptr) << "the target's own name is defined by the object";
                ASSERT_NE(aliasRow, nullptr) << "the alias's name is not defined by the object";
                EXPECT_EQ(aliasRow->symbol, targetRow->symbol) << "the alias is not one more name on its target";
                EXPECT_EQ(aliasRow->binding, e.binding);
            }
            // The module-private body nothing calls but through its exported alias.
            ModuleSymbol const* exported = rowNamed("dss_exported_body");
            ASSERT_NE(exported, nullptr) << "the exported alias of a `static` body is not defined by the object";
            EXPECT_EQ(exported->binding, SymbolBinding::Global);
            bool bodyKept = false;
            for (auto const& fn : object->functions) bodyKept = bodyKept || fn.symbol == exported->symbol;
            EXPECT_TRUE(bodyKept) << "the `static` body an alias names was removed: its alias names nothing";
        }
    }
}

TEST(CompiledSymbolAliasObject, EachObjectAliasShapeIsOneMoreNameOnItsStorage) {
    for (ObjectPair const& pair : kPairs) {
        for (CompileConfig const config : kConfigs) {
            SCOPED_TRACE(std::string{pair.target} + ":" + pair.format + ", "
                         + (config == CompileConfig::Release ? "release" : "baseline"));
            ScratchDir scratch{Location::InsideRepo, "compiled-alias-shapes"};
            auto const src = writeText(scratch.path(), "unit.c", kObjectShapesUnit);
            Program    p;
            p.setOutputDir(scratch.path());
            p.setCompileConfig(config);
            DiagnosticReporter rep;
            ASSERT_EQ(p.compileFiles({src.string()}, "c", {std::string{pair.target} + ":" + pair.format}, rep), 0)
                << diagnosticsOf(rep);
            auto const bytes  = readFileBytes(scratch.path() / (std::string{"unit"} + pair.extension));
            auto const target = TargetSchema::loadShipped(pair.target);
            auto const format = ObjectFormatSchema::loadShipped(pair.format);
            ASSERT_TRUE(target && format);
            DiagnosticReporter readRep;
            auto const object = pair.pe ? pe::readRelocatableObject(bytes, **target, **format, readRep)
                                        : elf::readRelocatableObject(bytes, **target, **format, readRep);
            ASSERT_TRUE(object.has_value()) << diagnosticsOf(readRep);
            auto const rowNamed = [&](std::string_view name) -> ModuleSymbol const* {
                for (auto const& s : object->symbols) {
                    if (s.name == name) return &s;
                }
                return nullptr;
            };
            auto const storageHolding = [&](std::uint32_t value) {
                std::vector<SymbolId> at;
                auto const            want = wordBytes(value);
                for (auto const& d : object->dataItems) {
                    if (d.bytes == want) at.push_back(d.symbol);
                }
                return at;
            };
            // Every value is stored ONCE: no alias is storage of its own.
            for (std::uint32_t const v : {1101u, 1202u, 1404u, 1505u, 1606u}) {
                SCOPED_TRACE(v);
                EXPECT_EQ(storageHolding(v).size(), 1u)
                    << "the value is stored more than once, or not at all: an alias was given storage of its own";
            }
            // Each EXTERNAL alias is one more name on its target's storage, with its own binding (and, on ELF, its
            // own visibility: COFF has none to state, and mingw-w64 gcc ignores the attribute there, warned).
            struct Named {
                char const*      alias;
                std::uint32_t    value;
                SymbolVisibility visibility;
            };
            for (Named const n : {Named{"dss_s_ext", 1101u, SymbolVisibility::Default},
                                  Named{"dss_h_hidden", 1404u, SymbolVisibility::Hidden},
                                  Named{"dss_p_protected", 1606u, SymbolVisibility::Protected}}) {
                SCOPED_TRACE(n.alias);
                ModuleSymbol const* row = rowNamed(n.alias);
                ASSERT_NE(row, nullptr) << "the alias's name is not a record of the object";
                EXPECT_EQ(row->binding, SymbolBinding::Global);
                auto const at = storageHolding(n.value);
                ASSERT_EQ(at.size(), 1u);
                EXPECT_EQ(row->symbol, at.front()) << "the alias is not one more name on its target's storage";
                if (!pair.pe) EXPECT_EQ(row->visibility, n.visibility) << "the alias's own visibility";
            }
            // A module-private alias is never an exported name.
            for (char const* name : {"dss_s_too", "dss_e_private", "dss_word_low"}) {
                for (auto const& s : object->symbols) {
                    if (s.name == name) EXPECT_EQ(s.binding, SymbolBinding::Local) << name << " is exported";
                }
            }
            // The weak alias of a `static` body: a weak definition of the body's bytes.
            ModuleSymbol const* weak = rowNamed("dss_weak_fn");
            ASSERT_NE(weak, nullptr) << "the weak alias of a `static` body is not a record of the object";
            EXPECT_EQ(weak->binding, SymbolBinding::Weak);
            bool onTheBody = false;
            for (auto const& fn : object->functions) onTheBody = onTheBody || fn.symbol == weak->symbol;
            EXPECT_TRUE(onTheBody) << "the weak alias does not name the `static` body";
        }
    }
}

// P69 fold f26b (lane `cs`, D-LK-OBJECT-STV-PROTECTED-DROPPED): a PROTECTED and an INTERNAL definition, a function
// and an object of each, keep their visibility in an ELF object at the baseline and at release — STV_PROTECTED and
// STV_INTERNAL, what gcc 13.3.0 writes (lane `cs` probe pv, 2026-10-16; clang 18.1.3 writes HIDDEN for internal, a
// split decided for gcc's: the word the source wrote). Before the fold both words were warned and dropped, and the
// symbols were written DEFAULT.
TEST(CompiledSymbolAliasObject, AProtectedAndAnInternalDefinitionKeepTheirVisibilityInAnElfObject) {
    constexpr char kUnit[] =
        "int dss_vp __attribute__((visibility(\"protected\"))) = 1;\n"
        "int dss_vi __attribute__((visibility(\"internal\"))) = 2;\n"
        "__attribute__((visibility(\"protected\"))) int dss_fp(void) { return 3; }\n"
        "__attribute__((visibility(\"internal\"))) int dss_fi(void) { return 4; }\n"
        "int dss_use_vis(void) { return dss_vp + dss_vi + dss_fp() + dss_fi(); }\n";
    for (ObjectPair const& pair : kPairs) {
        if (pair.pe) continue;   // COFF states no visibility (the PE documents' statement is their own)
        for (CompileConfig const config : kConfigs) {
            SCOPED_TRACE(std::string{pair.target} + ":" + pair.format + ", "
                         + (config == CompileConfig::Release ? "release" : "baseline"));
            ScratchDir scratch{Location::InsideRepo, "compiled-visibility-object"};
            auto const src = writeText(scratch.path(), "unit.c", kUnit);
            Program    p;
            p.setOutputDir(scratch.path());
            p.setCompileConfig(config);
            DiagnosticReporter rep;
            ASSERT_EQ(p.compileFiles({src.string()}, "c", {std::string{pair.target} + ":" + pair.format}, rep), 0)
                << diagnosticsOf(rep);
            for (auto const& d : rep.all()) {
                EXPECT_NE(d.code, DiagnosticCode::H_UnknownLinkageSpecifier) << d.actual;
            }
            auto const bytes  = readFileBytes(scratch.path() / (std::string{"unit"} + pair.extension));
            auto const target = TargetSchema::loadShipped(pair.target);
            auto const format = ObjectFormatSchema::loadShipped(pair.format);
            ASSERT_TRUE(target && format);
            DiagnosticReporter readRep;
            auto const object = elf::readRelocatableObject(bytes, **target, **format, readRep);
            ASSERT_TRUE(object.has_value()) << diagnosticsOf(readRep);
            for (auto const& [name, want] : {std::pair{"dss_vp", SymbolVisibility::Protected},
                                             std::pair{"dss_vi", SymbolVisibility::Internal},
                                             std::pair{"dss_fp", SymbolVisibility::Protected},
                                             std::pair{"dss_fi", SymbolVisibility::Internal}}) {
                SCOPED_TRACE(name);
                ModuleSymbol const* row = nullptr;
                for (auto const& s : object->symbols) {
                    if (s.name == name) row = &s;
                }
                ASSERT_NE(row, nullptr) << "the definition is not a record of the object";
                EXPECT_EQ(row->binding, SymbolBinding::Global);
                EXPECT_EQ(row->visibility, want);
            }
        }
    }
}

// P69 fold f26b (lane `cs`, item 10): a FUNCTION alias of an OBJECT and an OBJECT alias of a FUNCTION — clang's
// reading, decided (clang 18.1.3 runs both, gcc 13.3.0 refuses the first; lane `cs` probes al2 / al3): each alias
// is one more name on its target's storage in the object, at the baseline and at release, on ELF and PE.
TEST(CompiledSymbolAliasObject, AFunctionAliasOfAnObjectAndAnObjectAliasOfAFunctionNameTheirTargets) {
    constexpr char kUnit[] =
        "int dss_obj = 1707;\n"
        "void dss_fn_on_obj(void) __attribute__((alias(\"dss_obj\")));\n"
        "int dss_code(void) { return 9; }\n"
        "extern int dss_obj_on_code __attribute__((alias(\"dss_code\")));\n"
        "int dss_use_pair(void) { return dss_obj + dss_code() + (&dss_obj_on_code != 0); }\n";
    for (ObjectPair const& pair : kPairs) {
        for (CompileConfig const config : kConfigs) {
            SCOPED_TRACE(std::string{pair.target} + ":" + pair.format + ", "
                         + (config == CompileConfig::Release ? "release" : "baseline"));
            ScratchDir scratch{Location::InsideRepo, "compiled-alias-pair"};
            auto const src = writeText(scratch.path(), "unit.c", kUnit);
            Program    p;
            p.setOutputDir(scratch.path());
            p.setCompileConfig(config);
            DiagnosticReporter rep;
            ASSERT_EQ(p.compileFiles({src.string()}, "c", {std::string{pair.target} + ":" + pair.format}, rep), 0)
                << diagnosticsOf(rep);
            auto const bytes  = readFileBytes(scratch.path() / (std::string{"unit"} + pair.extension));
            auto const target = TargetSchema::loadShipped(pair.target);
            auto const format = ObjectFormatSchema::loadShipped(pair.format);
            ASSERT_TRUE(target && format);
            DiagnosticReporter readRep;
            auto const object = pair.pe ? pe::readRelocatableObject(bytes, **target, **format, readRep)
                                        : elf::readRelocatableObject(bytes, **target, **format, readRep);
            ASSERT_TRUE(object.has_value()) << diagnosticsOf(readRep);
            auto const rowNamed = [&](std::string_view name) -> ModuleSymbol const* {
                for (auto const& s : object->symbols) {
                    if (s.name == name) return &s;
                }
                return nullptr;
            };
            for (auto const& [alias, targetName] : {std::pair{"dss_fn_on_obj", "dss_obj"},
                                                    std::pair{"dss_obj_on_code", "dss_code"}}) {
                SCOPED_TRACE(alias);
                ModuleSymbol const* a = rowNamed(alias);
                ModuleSymbol const* t = rowNamed(targetName);
                ASSERT_NE(t, nullptr) << "the target's own name is defined by the object";
                ASSERT_NE(a, nullptr) << "the alias's name is not defined by the object";
                EXPECT_EQ(a->symbol, t->symbol) << "the alias is not one more name on its target";
                EXPECT_EQ(a->binding, SymbolBinding::Global);
            }
            // The object's value is stored once: the function alias gave it no storage, and no body of its own.
            std::size_t stored = 0;
            for (auto const& d : object->dataItems) stored += d.bytes == wordBytes(1707u) ? 1u : 0u;
            EXPECT_EQ(stored, 1u);
        }
    }
}

// P69 fold f26b (lane `cs`, item 10): a `__declspec(selectany)` ALIAS — of a `static` object and of an external
// one — is the name its own binding makes it: the select-any is ignored and said (`ignoredOnAlias`), and the pe64
// object holds the alias as a GLOBAL name on its target's storage, as mingw-w64 gcc 13.2.0 writes it (a plain
// EXTERNAL label; ✔MEASURED, lane `cs` probe sw, 2026-10-20 — that object's image runs, exit 42). Before the fold
// the alias kept the select-any kind, and the writer refused the first unit (`K_NoMatchingObjectFormat`) and wrote
// the second's alias as an OVERRIDABLE weak external (✔MEASURED, probe sw on this fold's own build).
TEST(CompiledSymbolAliasObject, ASelectAnyAliasIsAGlobalNameOnItsTargetsStorage) {
    struct Unit {
        char const*   what;
        char const*   text;
        std::uint32_t value;
    };
    for (Unit const u : {Unit{"of a static object",
                              "static int dss_sa_target = 1808;\n"
                              "__declspec(selectany) extern int dss_sa_alias __attribute__((alias(\"dss_sa_target\")));\n"
                              "int dss_use_sa(void) { return dss_sa_alias; }\n",
                              1808u},
                         Unit{"of an external object",
                              "int dss_sa_target = 1909;\n"
                              "__declspec(selectany) extern int dss_sa_alias __attribute__((alias(\"dss_sa_target\")));\n"
                              "int dss_use_sa(void) { return dss_sa_alias; }\n",
                              1909u}}) {
        for (ObjectPair const& pair : kPairs) {
            if (!pair.pe) continue;   // the `__declspec` spelling is refused by name off pe
            for (CompileConfig const config : kConfigs) {
                SCOPED_TRACE(std::string{u.what} + ", " + pair.format + ", "
                             + (config == CompileConfig::Release ? "release" : "baseline"));
                ScratchDir scratch{Location::InsideRepo, "compiled-selectany-alias"};
                auto const src = writeText(scratch.path(), "unit.c", u.text);
                Program    p;
                p.setOutputDir(scratch.path());
                p.setCompileConfig(config);
                DiagnosticReporter rep;
                ASSERT_EQ(p.compileFiles({src.string()}, "c", {std::string{pair.target} + ":" + pair.format}, rep),
                          0)
                    << diagnosticsOf(rep);
                std::size_t said = 0;
                for (auto const& d : rep.all()) {
                    said += d.code == DiagnosticCode::S_AttributeIgnoredForDeclarationKind
                                    && d.actual.find("'selectany'") != std::string::npos
                                ? 1u
                                : 0u;
                }
                EXPECT_EQ(said, 1u) << "the ignored select-any is said once" << diagnosticsOf(rep);
                auto const bytes  = readFileBytes(scratch.path() / (std::string{"unit"} + pair.extension));
                auto const target = TargetSchema::loadShipped(pair.target);
                auto const format = ObjectFormatSchema::loadShipped(pair.format);
                ASSERT_TRUE(target && format);
                DiagnosticReporter readRep;
                auto const object = pe::readRelocatableObject(bytes, **target, **format, readRep);
                ASSERT_TRUE(object.has_value()) << diagnosticsOf(readRep);
                ModuleSymbol const* alias = nullptr;
                for (auto const& s : object->symbols) {
                    if (s.name == "dss_sa_alias") alias = &s;
                }
                ASSERT_NE(alias, nullptr) << "the alias's name is not a record of the object";
                EXPECT_EQ(alias->binding, SymbolBinding::Global) << "the alias is the name its own binding makes it";
                std::vector<SymbolId> at;
                for (auto const& d : object->dataItems) {
                    if (d.bytes == wordBytes(u.value)) at.push_back(d.symbol);
                }
                ASSERT_EQ(at.size(), 1u) << "the target's value is stored once";
                EXPECT_EQ(alias->symbol, at.front()) << "the alias is not one more name on its target's storage";
            }
        }
    }
}

TEST(CompiledSymbolAliasObject, AMachOTargetRefusesAnAliasByName) {
    for (char const* spec : {"x86_64:macho64-x86_64-darwin", "arm64:macho64-arm64-darwin"}) {
        SCOPED_TRACE(spec);
        ScratchDir scratch{Location::InsideRepo, "compiled-alias-macho"};
        auto const src = writeText(scratch.path(), "unit.c",
                                   "int dss_add(int a, int b) { return a + b; }\n"
                                   "int dss_sum(int, int) __attribute__((alias(\"dss_add\")));\n");
        Program p;
        p.setOutputDir(scratch.path());
        DiagnosticReporter rep;
        EXPECT_NE(p.compileFiles({src.string()}, "c", {spec}, rep), 0);
        std::size_t refused = 0;
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::S_AttributeNotHonoured && d.actual.find("'alias'") != std::string::npos)
                ++refused;
        }
        EXPECT_EQ(refused, 1u) << "a Mach-O target refuses an alias once, by name" << diagnosticsOf(rep);
    }
}

TEST(CompiledSymbolAliasObject, TheFormatDocumentsStateWhereAnAliasAndACommonCanBe) {
    std::size_t formats = 0;
    for (auto const& entry : fs::directory_iterator{dss::test::configRoot() / "object-formats"}) {
        std::string const file = entry.path().filename().string();
        auto const        at   = file.find(".format.json");
        if (at == std::string::npos) continue;
        std::string const name = file.substr(0, at);
        SCOPED_TRACE(name);
        auto const format = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(format.has_value());
        ++formats;
        bool const macho = (*format)->kind() == ObjectFormatKind::MachO;
        EXPECT_EQ((*format)->symbolAliasUnsupportedReason(),
                  macho ? std::optional<SymbolAliasUnsupportedReason>{SymbolAliasUnsupportedReason::PlatformCompilersRefuse}
                        : std::nullopt)
            << "only the Mach-O documents say their platform's compilers refuse an alias";
        bool const writesObjects = (*format)->kind() == ObjectFormatKind::Elf || (*format)->kind() == ObjectFormatKind::Pe
                                   || macho;
        EXPECT_EQ((*format)->statesCommons(), writesObjects)
            << "every ELF, PE and Mach-O document states how a common is resolved, and the others state none";
    }
    EXPECT_GE(formats, 24u) << "the shipped documents were not all read";
}
