// `wchar_t` — the typedef AND the type of `L'…'`/`L"…"` — from ONE source, the
// TARGET's platform ABI typedef table, per (processor, platform) pair.
// P68 round 9, D-C-WCHAR-T-IS-SIGNED-ON-ARM64-LINUX.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// On ELF aarch64, DSS's `wchar_t` and `L'…'` were `int`; the platform's are
// `unsigned int` — a SILENT wrong result: `(wchar_t)-1 > 0` was false under DSS
// and true under gcc and clang (✔MEASURED 2026-09-23: aarch64-linux-gnu-gcc 13.3.0
// and clang 18.1.3 --target=aarch64-linux-gnu, under qemu). Both carriers keyed the
// fact on the PLATFORM alone — `<stddef.h>`'s typedef (`elf → i32`) and the literal
// rows (a per-format map, `elf → I32`; the mechanism is now deleted and its key
// refused) — while it is per processor × platform: `elf` serves x86_64 (`int`) and
// aarch64 (`unsigned int`). The target's `abiTypedefs.wchar_t` already stated it
// right for `__SIZEOF_WCHAR_T__`; it is now the ONE source both carriers read.
//
// Pins:
//   (1) on every REAL pair, a `wchar_t` object, `L'a'`, an element of `L"a"`, and
//       the typedef `<stdlib.h>` also declares, all have the platform's type — the
//       references' answer, written out per (processor, platform) — and
//       `(wchar_t)-1 > 0` is the platform's answer;
//   (2) every shipped target declares `wchar_t` for every format it serves (so
//       S_AbiTypedefUndeclared is unreachable with shipped config);
//   (3) a STRIPPED custom target that declares no `wchar_t` REACHES it: `L'a'` and
//       `L"a"` are refused where they stand, and `<stddef.h>` declares no
//       `wchar_t` (a use of the name then fails loud) — never a guessed width.
// The run-time half is `examples/c/wchar_t_platform_type`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// ✔MEASURED 2026-09-23 (`_Generic` on a `wchar_t` object and on `L'a'`, and
// `(wchar_t)-1 > 0`): gcc 13.3.0 + clang 18.1.3 x86_64-linux → int; their aarch64
// cross compilers under qemu → unsigned int; mingw-w64 13.2.0 + MSVC 14.51 →
// unsigned short. Darwin → int on both arches (Apple clang, measured by lane `fo`
// in round 8 for `__WCHAR_TYPE__`, pinned in test_sizeof_macro_family).
struct WcharFact {
    std::string_view target;
    ObjectFormatKind format;
    std::string_view cType;
    bool             isUnsigned;
};
constexpr WcharFact kWchar[] = {
    {"x86_64", ObjectFormatKind::Elf,   "int",            false},
    {"arm64",  ObjectFormatKind::Elf,   "unsigned int",   true},
    {"x86_64", ObjectFormatKind::Pe,    "unsigned short", true},
    {"x86_64", ObjectFormatKind::MachO, "int",            false},
    {"arm64",  ObjectFormatKind::MachO, "int",            false},
};

[[nodiscard]] WcharFact const* wcharFor(std::string_view target, ObjectFormatKind f) {
    for (auto const& w : kWchar) {
        if (w.target == target && w.format == f) return &w;
    }
    return nullptr;
}

[[nodiscard]] std::vector<std::string> shippedNames(std::string_view dir,
                                                    std::string_view suffix) {
    std::vector<std::string> out;
    for (auto const& e :
         std::filesystem::directory_iterator{dss::test::configRoot() / dir}) {
        std::string const fn = e.path().filename().string();
        auto const at = fn.find(suffix);
        if (at == std::string::npos) continue;
        out.push_back(fn.substr(0, at));
    }
    return out;
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

struct Compiled {
    std::vector<ParseDiagnostic> diagnostics;   // every tier's, errors and warnings
    [[nodiscard]] std::string errors() const {
        std::string out;
        for (auto const& d : diagnostics) {
            if (d.severity != DiagnosticSeverity::Error) continue;
            out += "  " + d.actual + "\n";
        }
        return out;
    }
    [[nodiscard]] std::size_t count(DiagnosticCode c) const {
        std::size_t n = 0;
        for (auto const& d : diagnostics) n += (d.code == c) ? 1u : 0u;
        return n;
    }
};

[[nodiscard]] Compiled compile(TargetSchema const& target, ObjectFormatSchema const& format,
                               std::string source) {
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    Compiled c;
    for (auto const& d : cu->driverDiagnostics().all()) c.diagnostics.push_back(d);
    for (auto const& tree : cu->trees()) {
        for (auto const& d : tree.diagnostics().all()) c.diagnostics.push_back(d);
    }
    auto const analysis = analyzeForTargetFormat(
        cu, DiagnosticBudget::libraryDefault(), target, format, nullptr);
    for (auto const& d : analysis.model.diagnostics().all()) c.diagnostics.push_back(d);
    return c;
}

}  // namespace

// ── (1) BOTH CARRIERS, ON EVERY REAL PAIR, ARE THE PLATFORM'S TYPE ───────────
TEST(WcharTAbiTypedef, BothCarriersHaveThePlatformsTypeOnEveryRealPair) {
    ASSERT_NE(cLanguage(), nullptr);
    std::size_t pairs = 0;
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;
            SCOPED_TRACE(targetName + ":" + formatName);
            WcharFact const* w = wcharFor((*targetR)->name(), (*formatR)->kind());
            if (w == nullptr) {
                ADD_FAILURE() << "no reference measurement of wchar_t for this pair";
                continue;
            }
            ++pairs;
            std::string const src = std::format(
                "#include <stddef.h>\n#include <stdlib.h>\n"
                "#define IS(T, M) _Static_assert(_Generic((M), T: 1, default: 0), "
                "#M \" is not \" #T);\n"
                "wchar_t w;\n"
                "IS({0}, w)\nIS({0}, L'a')\nIS({0}, L\"a\"[0])\n"
                "_Static_assert(((wchar_t)-1 > 0) == {1}, \"wchar_t signedness\");\n"
                "_Static_assert(sizeof(wchar_t) == __SIZEOF_WCHAR_T__, \"one source\");\n"
                "_Static_assert(sizeof(L'a') == __SIZEOF_WCHAR_T__, \"one source\");\n",
                w->cType, w->isUnsigned ? 1 : 0);
            Compiled const c = compile(**targetR, **formatR, src);
            EXPECT_EQ(c.errors(), "");
        }
    }
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

// ── (2) EVERY SHIPPED TARGET DECLARES wchar_t ON EVERY FORMAT IT SERVES ──────
// So `S_AbiTypedefUndeclared` stays unreachable with shipped config (the
// coordinator's ruling: it is the fail-loud arm for a CUSTOM target).
TEST(WcharTAbiTypedef, EveryShippedPairDeclaresWcharT) {
    std::size_t pairs = 0;
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;
            ++pairs;
            EXPECT_TRUE((*targetR)->abiTypedefCore("wchar_t", (*formatR)->kind()).has_value())
                << targetName << " declares no wchar_t for " << formatName;
        }
    }
    EXPECT_GE(pairs, 22u);
}

// ── (3) A STRIPPED CUSTOM TARGET REACHES THE REFUSAL ─────────────────────────
TEST(WcharTAbiTypedef, ATargetWithoutWcharTRefusesTheWideLiteralsWhereTheyStand) {
    ASSERT_NE(cLanguage(), nullptr);
    std::ifstream in(dss::test::configRoot() / "targets" / "arm64.target.json",
                     std::ios::binary);
    ASSERT_TRUE(in.is_open());
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // Strip the ONE row: rename `wchar_t` inside the `abiTypedefs` object, so the
    // target is otherwise the shipped one (every other fact unchanged).
    auto const table = text.find("\"abiTypedefs\":");
    ASSERT_NE(table, std::string::npos) << "arm64.target.json has no abiTypedefs table";
    auto const row = text.find("\"wchar_t\":", table);
    ASSERT_NE(row, std::string::npos) << "arm64's abiTypedefs has no wchar_t row";
    text.replace(row, std::string_view{"\"wchar_t\":"}.size(), "\"wchar_x_t\":");
    auto stripped = TargetSchema::loadFromText(text, "<stripped-arm64>");
    ASSERT_TRUE(stripped.has_value())
        << (stripped.error().empty() ? "" : stripped.error()[0].message);
    ASSERT_FALSE((*stripped)->abiTypedefCore("wchar_t", ObjectFormatKind::Elf).has_value())
        << "the strip did not take";
    auto formatR = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(formatR.has_value());

    // `L'a'` and `L"a"`: refused, each where it stands.
    {
        Compiled const c = compile(**stripped, **formatR,
                                   "int f(void) { return L'a'; }\n"
                                   "int g(void) { return sizeof(L\"ab\"); }\n");
        EXPECT_EQ(c.count(DiagnosticCode::S_AbiTypedefUndeclared), 2u)
            << "one refusal per wide literal:\n" << c.errors();
        bool namesAll = false;
        for (auto const& d : c.diagnostics) {
            if (d.code != DiagnosticCode::S_AbiTypedefUndeclared) continue;
            namesAll = d.actual.find("wchar_t") != std::string::npos
                    && d.actual.find("elf") != std::string::npos
                    && d.actual.find("L'a'") != std::string::npos;
            if (namesAll) break;
        }
        EXPECT_TRUE(namesAll) << "the refusal names the construct, the typedef, the "
                                 "target and the format:\n" << c.errors();
    }
    // The control: a narrow literal on the same stripped target is untouched.
    {
        Compiled const c = compile(**stripped, **formatR,
                                   "int f(void) { return 'a' + (int)sizeof(\"ab\"); }\n");
        EXPECT_EQ(c.errors(), "");
    }
    // `<stddef.h>` declares no `wchar_t` on that pair — a use fails loud.
    {
        Compiled const c = compile(**stripped, **formatR,
                                   "#include <stddef.h>\nwchar_t w;\n");
        EXPECT_NE(c.errors(), "") << "wchar_t must not be injected with a guessed type";
        EXPECT_EQ(c.count(DiagnosticCode::S_AbiTypedefUndeclared), 0u);
    }
}
