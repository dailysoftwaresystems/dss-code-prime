// P69 round 4 (lane `lm`; the re-review of lane `cs`, finding 1): AN ALIAS ROW CARRIES ITS GATES.
//
// A symbol row's `aliases` map lists, per object format, the other names that format's C library
// exports the row's object under (`ShippedSymbol::aliases`). The full read
// (`readShippedLibDescriptor`) appends a row for each alias of the ACTIVE format alone, so an alias
// is declared on the format that lists it and nowhere else — and only inside a document that
// exists there.
//
// The corpus INDEX (`buildCorpusIndex`) indexed an alias with its listing format in the row's
// `formats` and NOTHING in its two gates, and an empty gate reads as "every format". So the one
// question asked off the gates, `shippedLibraryFunctionProvidedOnFormat` — the preprocessor's
// `__has_builtin` answer for a library builtin — answered "provided" for an alias on formats that
// never list it, where the binder (the realization oracle, which reads the descriptor in full)
// finds no row and refuses the use. Exposure in the shipped corpus: none (its one `aliases` map is
// on an OBJECT row, unistd.json's `environ`, and the question is about functions).
//
// PINNED HERE, over a scratch corpus:
//   (1) an alias asked on EVERY selectable format — the index's answer equals the full read's,
//       which is "provided" on the listing format and absent on every other;
//   (2) the document's gate holds for an alias as it does for its row;
//   (3) an alias listed for a format on which the row itself is not available is REFUSED at load
//       (an alias is the row under another name, so it exists only where the row does);
//   (4) the availability view (`collectShippedExternSymbolFormats`) still names the listing format.
//
// RED-ON-DISABLE (each read on the built mutant, never assumed): drop the alias row's symbol gate
// in `buildCorpusIndex` → (1) answers "provided" on every format that does not list the alias;
// drop its document gate → (2) answers "provided" on the listing format of a document that does
// not exist there; drop the reader's refusal → (3) reads the descriptor clean.
//
// It drives the index through `DSS_CONFIG_ROOT`, so it MUST run under ctest (a bare .exe takes
// the cwd walk and reads whichever tree the shell stands in).

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// A repo-SHAPED config root: `DSS_CONFIG_ROOT` names a directory that CONTAINS
// `src/dss-config/` (the realization-oracle fixture's own shape).
[[nodiscard]] fs::path shippedLibsDirOf(ScratchDir const& dir) {
    fs::path const d = dir.path() / "src" / "dss-config" / "shippedLibs";
    fs::create_directories(d);
    return d;
}

void writeDesc(fs::path const& shippedLibs, char const* name, std::string const& body) {
    std::ofstream out{shippedLibs / name, std::ios::binary};
    out << body;
    ASSERT_TRUE(out.good()) << "descriptor did not reach disk: " << name;
}

// Every format a descriptor can name.
[[nodiscard]] std::vector<ObjectFormatKind> selectableFormats() {
    std::vector<ObjectFormatKind> out;
    for (auto const& n : kSelectableObjectFormatKindNames) {
        auto const k = objectFormatKindFromName(n);
        if (k.has_value()) out.push_back(*k);
    }
    return out;
}

// THE FULL READ's answer for `name` on `fmt`: does the realization oracle — the binder's own
// question — find a FUNCTION row it would bind? (A row with no image for the format binds too:
// the reference routes unbound to the link tier.)
[[nodiscard]] bool fullReadProvides(fs::path const& treeRoot, std::string const& name,
                                    ObjectFormatKind fmt) {
    ScopedEnv const env{"DSS_CONFIG_ROOT", treeRoot.string()};
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    std::array<std::string, 1> const names{name};
    auto const realized = realizeShippedExternSymbols(names, interner, typeReg, rep, DataModel::Lp64,
                                                      std::optional<std::string_view>{"x86_64"}, fmt);
    EXPECT_TRUE(realized.has_value()) << "the oracle did not locate the scratch corpus";
    if (!realized.has_value()) return false;
    auto const it = realized->find(name);
    if (it == realized->end()) return false;
    ShippedRealizationStatus const s = it->second.status;
    bool const bindable = s == ShippedRealizationStatus::Realized
                       || s == ShippedRealizationStatus::NoLibraryForFormat
                       || s == ShippedRealizationStatus::ProvidedByShippedSource;
    return bindable && it->second.isFunction;
}

// THE INDEX's answer: the question `__has_builtin` asks.
[[nodiscard]] std::optional<bool> indexProvides(fs::path const& treeRoot, std::string const& name,
                                                ObjectFormatKind fmt) {
    ScopedEnv const env{"DSS_CONFIG_ROOT", treeRoot.string()};
    return shippedLibraryFunctionProvidedOnFormat(name, fmt);
}

// One function row with an alias on `aliasFormat`, in a document gated to `documentFormats`
// (empty text: no document gate), the row itself gated to `rowFormats` (empty: no row gate).
[[nodiscard]] std::string aliasDescriptor(std::string_view documentFormats, std::string_view rowFormats,
                                          std::string_view aliasFormat) {
    std::string out = R"({ "header": "aliasgate.h", "library": { "elf": "libc.so.6", "pe": "ucrtbase.dll", "macho": "libSystem.B.dylib" },)";
    if (!documentFormats.empty()) {
        out += R"( "availableObjectFormats": [)" + std::string{documentFormats} + "],";
    }
    out += R"( "symbols": [ { "name": "aliasgate_compare", "signature": "fn(ptr<void>, ptr<void>, u64) -> i32",)";
    if (!rowFormats.empty()) {
        out += R"( "availableObjectFormats": [)" + std::string{rowFormats} + "],";
    }
    out += R"( "aliases": { ")" + std::string{aliasFormat} + R"(": ["aliasgate_other_name"] } } ] })";
    return out;
}

[[nodiscard]] std::string allErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "\n  ";
        out += d.actual;
    }
    return out;
}

}  // namespace

// ── (1) THE SYMBOL GATE: an alias exists on the format that lists it, and on no other ──────────
TEST(ShippedAliasRowGates, AnAliasIsProvidedOnItsListingFormatAlone) {
    ScratchDir dir{Location::Temp, "alias-gate-symbol"};
    fs::path const libs = shippedLibsDirOf(dir);
    writeDesc(libs, "aliasgate.json", aliasDescriptor("", "", "elf"));

    std::size_t asked = 0;
    std::size_t providedSomewhere = 0;
    for (ObjectFormatKind const fmt : selectableFormats()) {
        SCOPED_TRACE(std::string{objectFormatKindName(fmt)});
        ++asked;
        // The canonical name is declared everywhere (no gate at all): the control that the
        // corpus was read and that an ungated row still answers "every format".
        auto const canonical = indexProvides(dir.path(), "aliasgate_compare", fmt);
        ASSERT_TRUE(canonical.has_value()) << "the index did not locate the scratch corpus";
        EXPECT_TRUE(*canonical) << "an ungated row is declared on every format";

        bool const listing = fmt == ObjectFormatKind::Elf;
        bool const full = fullReadProvides(dir.path(), "aliasgate_other_name", fmt);
        EXPECT_EQ(full, listing) << "the full read appends an alias row for its listing format alone";
        auto const index = indexProvides(dir.path(), "aliasgate_other_name", fmt);
        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(*index, full)
            << "the index answers \"" << (*index ? "provided" : "absent") << "\" for the alias on "
            << objectFormatKindName(fmt) << " where the full read answers \""
            << (full ? "provided" : "absent")
            << "\": `#if __has_builtin` and the use of the builtin would disagree there";
        if (*index) ++providedSomewhere;
    }
    EXPECT_GE(asked, 3u) << "the selectable-format enumeration collapsed";
    EXPECT_EQ(providedSomewhere, 1u) << "the alias is provided on exactly its one listing format";
}

// ── (2) THE DOCUMENT GATE: an alias is declared only where its document exists ─────────────────
// The document is gated to elf and macho; the row lists its alias on macho. On pe the document
// declares nothing — not the row, not its alias — and on elf the alias is not listed.
TEST(ShippedAliasRowGates, AnAliasTakesItsDocumentsGate) {
    ScratchDir dir{Location::Temp, "alias-gate-document"};
    fs::path const libs = shippedLibsDirOf(dir);
    writeDesc(libs, "aliasgate.json", aliasDescriptor(R"("elf", "macho")", "", "macho"));

    for (ObjectFormatKind const fmt : selectableFormats()) {
        SCOPED_TRACE(std::string{objectFormatKindName(fmt)});
        bool const full = fullReadProvides(dir.path(), "aliasgate_other_name", fmt);
        EXPECT_EQ(full, fmt == ObjectFormatKind::MachO);
        auto const index = indexProvides(dir.path(), "aliasgate_other_name", fmt);
        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(*index, full);
    }

    // And the gate is the DOCUMENT's own, read off the alias row: a document that does not exist
    // on the alias's listing format declares no alias there. (The full reader refuses such a
    // descriptor — arm (3) — so the oracle skips it; the lenient index still walks its rows, and
    // must not answer "provided" for one of them.)
    ScratchDir excluded{Location::Temp, "alias-gate-document-excluded"};
    fs::path const libs2 = shippedLibsDirOf(excluded);
    writeDesc(libs2, "aliasgate.json", aliasDescriptor(R"("pe")", "", "elf"));
    for (ObjectFormatKind const fmt : selectableFormats()) {
        SCOPED_TRACE(std::string{"document gated to pe, alias listed on elf, asked on "}
                     + std::string{objectFormatKindName(fmt)});
        EXPECT_FALSE(fullReadProvides(excluded.path(), "aliasgate_other_name", fmt));
        auto const index = indexProvides(excluded.path(), "aliasgate_other_name", fmt);
        ASSERT_TRUE(index.has_value());
        EXPECT_FALSE(*index) << "the document does not exist on the alias's listing format";
    }
}

// ── (3) AN ALIAS WHERE THE ROW IS NOT: refused at load ─────────────────────────────────────────
TEST(ShippedAliasRowGates, AnAliasOnAFormatTheRowExcludesIsRefused) {
    struct Arm {
        char const* what;
        char const* documentFormats;
        char const* rowFormats;
        char const* aliasFormat;
        bool        refused;
    };
    const Arm arms[] = {
        {"no gate anywhere", "", "", "elf", false},
        {"the row gated to its alias's format", "", R"("elf", "macho")", "elf", false},
        {"the document gated to its alias's format", R"("elf", "macho")", "", "macho", false},
        {"the ROW excludes the alias's format", "", R"("pe")", "elf", true},
        {"the DOCUMENT excludes the alias's format", R"("pe")", "", "elf", true},
        {"the document admits it and the row does not", R"("elf", "pe")", R"("pe")", "elf", true},
    };
    for (Arm const& a : arms) {
        SCOPED_TRACE(a.what);
        ScratchDir dir{Location::Temp, "alias-gate-refusal"};
        fs::path const libs = shippedLibsDirOf(dir);
        writeDesc(libs, "aliasgate.json", aliasDescriptor(a.documentFormats, a.rowFormats, a.aliasFormat));
        // Read on EVERY format: the rule is about the descriptor, not about the active format, so
        // an arm no current build selects cannot rot.
        for (ObjectFormatKind const fmt : selectableFormats()) {
            SCOPED_TRACE(std::string{objectFormatKindName(fmt)});
            TypeInterner interner{CompilationUnitId{1}};
            TypeRegistry typeReg;
            DiagnosticReporter rep;
            auto const desc = readShippedLibDescriptor(libs / "aliasgate.json", interner, typeReg, rep,
                                                       DataModel::Lp64,
                                                       std::optional<std::string_view>{"x86_64"}, fmt);
            if (a.refused) {
                EXPECT_FALSE(desc.has_value()) << "an alias listed where its row is not available";
                EXPECT_NE(allErrors(rep).find("aliasgate_compare"), std::string::npos)
                    << "the refusal names the row:" << allErrors(rep);
                EXPECT_NE(allErrors(rep).find(a.aliasFormat), std::string::npos)
                    << "the refusal names the format:" << allErrors(rep);
            } else {
                EXPECT_TRUE(desc.has_value()) << allErrors(rep);
                EXPECT_EQ(allErrors(rep), "");
            }
        }
    }
}

// ── (4) THE AVAILABILITY VIEW is unchanged: the alias is known, on its listing format ──────────
TEST(ShippedAliasRowGates, TheAvailabilityViewNamesTheListingFormat) {
    ScratchDir dir{Location::Temp, "alias-gate-availability"};
    fs::path const libs = shippedLibsDirOf(dir);
    writeDesc(libs, "aliasgate.json", aliasDescriptor("", "", "elf"));
    ScopedEnv const env{"DSS_CONFIG_ROOT", dir.path().string()};
    auto const formats = collectShippedExternSymbolFormats();
    ASSERT_TRUE(formats.has_value());
    auto const alias = formats->find("aliasgate_other_name");
    ASSERT_NE(alias, formats->end()) << "the alias is a known name";
    EXPECT_EQ(alias->second, std::vector<std::string>{"elf"});
    auto const canonical = formats->find("aliasgate_compare");
    ASSERT_NE(canonical, formats->end());
    EXPECT_TRUE(canonical->second.empty()) << "an ungated row is available everywhere";
}
