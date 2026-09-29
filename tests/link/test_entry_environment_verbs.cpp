// D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE — the FORMAT half of the environment entry
// forms: which verbs each shipped exec format declares, and the load-time rules
// that hold a format's verbs and its `processArgs` mechanism together.
//
// ★ THE RULE UNDER TEST. A verb in `entryVerbs` is what makes a language row a
// program-entry CANDIDATE, so a verb the declared mechanism cannot realize would
// pass candidate selection and the entry would read the values nothing produced
// from whatever the argument registers hold — the MEASURED fault (an envp observed
// as 0x4) the entry-verb set exists to refuse. So `ObjectFormatData::validate()`
// asks every listed verb whether the mechanism can produce it, and refuses the
// converse too: an environment declaration no verb reads is dead config.
//
// ★ THE FIXTURES ARE THE SHIPPED DOCUMENTS, EACH CHANGED IN ONE PLACE (the
// `test_c_symbol_decoration.cpp` precedent). The unmodified document is loaded
// first as the CONTROL, so a rejection below can only come from the one edit —
// and `errorCount` pins that nothing ELSE fired, so deleting the rule under test
// turns the load green and reds the count instead of the fixture quietly failing
// for an unrelated reason.

#include "core/types/entry_shape.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "format_reject_support.hpp"   // countAtPath / errorCount / rejectSummary
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::rejectSummary;

namespace {

[[nodiscard]] fs::path objectFormatsDir() {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *root / "object-formats";
}

[[nodiscard]] std::string readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) {
        ADD_FAILURE() << "cannot open " << p.string();
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

// Load the shipped `<name>.format.json` after ONE in-memory edit (the file on disk
// is never rewritten). The unmodified document is asserted to load clean first,
// so every rejection a caller sees is the edit's.
[[nodiscard]] link_format::test::FormatLoadResult
loadEdited(std::string const& name, std::function<void(nlohmann::json&)> const& edit) {
    std::string const leaf = name + ".format.json";
    auto const text = readFile(objectFormatsDir() / leaf);
    EXPECT_FALSE(text.empty()) << leaf;
    auto control = ObjectFormatSchema::loadFromText(text, leaf);
    EXPECT_TRUE(control.has_value())
        << "CONTROL: the unmodified shipped " << leaf << " must load clean: "
        << rejectSummary(control);
    nlohmann::json doc = nlohmann::json::parse(text);
    edit(doc);
    return ObjectFormatSchema::loadFromText(doc.dump(), leaf);
}

void addVerb(nlohmann::json& doc, char const* verb) {
    doc.at("entryVerbs").push_back(verb);
}

void dropVerb(nlohmann::json& doc, std::string_view verb) {
    auto& verbs = doc.at("entryVerbs");
    nlohmann::json kept = nlohmann::json::array();
    for (auto const& v : verbs) {
        if (v.get<std::string>() != verb) kept.push_back(v);
    }
    ASSERT_EQ(kept.size() + 1, verbs.size()) << "the edit must drop exactly " << verb;
    verbs = kept;
}

}  // namespace

// ── The SHIPPED declarations, pinned per format ─────────────────────────────

TEST(EntryEnvironmentVerbs, ShippedElfFormatsRealizeTheStackEnvironment) {
    // The entry-stack layout the elf formats declare: the environment vector one
    // 8-byte slot past argv's NULL terminator (SysV AMD64 psABI §3.4.1; AAPCS64
    // Linux), realized by `argc-argv-envp` — and NOT Darwin's four-value verb, which
    // that layout cannot produce.
    std::vector<EntryMaterialization> const want{
        EntryMaterialization::None, EntryMaterialization::ArgcArgv,
        EntryMaterialization::ArgcArgvEnvp};
    for (auto const* name : {"elf64-x86_64-linux-exec", "elf64-x86_64-linux-pie",
                             "elf64-aarch64-linux-exec", "elf64-aarch64-linux-pie"}) {
        SCOPED_TRACE(name);
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value());
        auto const verbs = (*r)->entryVerbs();
        EXPECT_EQ(std::vector<EntryMaterialization>(verbs.begin(), verbs.end()), want);
        auto const& pa = (*r)->processArgs();
        ASSERT_TRUE(pa.has_value());
        EXPECT_EQ(pa->mechanism, ArgsMechanism::StackVector);
        EXPECT_TRUE(pa->envpFollowsArgvTerminator);
        EXPECT_EQ(pa->vectorSlotBytes, 8u)
            << "one machine word per vector slot on both LP64 Linux ABIs";
    }
}

TEST(EntryEnvironmentVerbs, ShippedMachoFormatsPassEveryFormThrough) {
    // dyld calls LC_MAIN with (argc, argv, envp, apple) in the argument registers, so
    // Mach-O realizes all three argument forms with NO mechanism block.
    std::vector<EntryMaterialization> const want{
        EntryMaterialization::None, EntryMaterialization::ArgcArgv,
        EntryMaterialization::ArgcArgvEnvp, EntryMaterialization::ArgcArgvEnvpApple};
    for (auto const* name : {"macho64-x86_64-darwin-exec", "macho64-arm64-darwin-exec"}) {
        SCOPED_TRACE(name);
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value());
        auto const verbs = (*r)->entryVerbs();
        EXPECT_EQ(std::vector<EntryMaterialization>(verbs.begin(), verbs.end()), want);
        EXPECT_FALSE((*r)->processArgs().has_value());
    }
}

TEST(EntryEnvironmentVerbs, ShippedPeFormatNamesTheUcrtEnvironmentPairs) {
    // ✔MEASURED 2026-09-24 on ucrtbase 10.0.26100.9444: `_initialize_narrow_environment`
    // ord 373, `_get_initial_narrow_environment` 321, `_initialize_wide_environment`
    // 375, `_get_initial_wide_environment` 322. Pinned by NAME because DSS
    // eager-imports every name it binds: one misspelling fails every pe program's
    // LOAD, not just the ones with a 3-parameter entry.
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(r.has_value());
    auto const verbs = (*r)->entryVerbs();
    EXPECT_EQ(std::vector<EntryMaterialization>(verbs.begin(), verbs.end()),
              (std::vector<EntryMaterialization>{
                  EntryMaterialization::None, EntryMaterialization::ArgcArgv,
                  EntryMaterialization::ArgcWargv, EntryMaterialization::ArgcArgvEnvp,
                  EntryMaterialization::ArgcWargvWenvp}));
    auto const& pa = (*r)->processArgs();
    ASSERT_TRUE(pa.has_value());
    EXPECT_EQ(pa->initializeNarrowEnvironmentFn, "_initialize_narrow_environment");
    EXPECT_EQ(pa->narrowEnvironmentAccessorFn, "_get_initial_narrow_environment");
    EXPECT_EQ(pa->initializeWideEnvironmentFn, "_initialize_wide_environment");
    EXPECT_EQ(pa->wideEnvironmentAccessorFn, "_get_initial_wide_environment");
}

// ── validate(): a verb the mechanism cannot realize is refused AT LOAD ──────

TEST(EntryEnvironmentVerbs, AVerbTheMechanismCannotRealizeIsRefused) {
    struct Case {
        char const* name;
        char const* format;
        std::function<void(nlohmann::json&)> edit;
        char const* why;   // a phrase the refusal must carry
    };
    std::vector<Case> const cases{
        {"stack env verb without the declared layout", "elf64-x86_64-linux-exec",
         [](nlohmann::json& d) {
             d.at("processArgs").erase("envpFollowsArgvTerminator");
             d.at("processArgs").erase("vectorSlotBytes");
         },
         "does not declare where the environment vector sits"},
        {"a wide verb on the entry stack", "elf64-x86_64-linux-exec",
         [](nlohmann::json& d) { addVerb(d, "argc-wargv"); },
         "NARROW strings only"},
        {"the four-value verb on the entry stack", "elf64-aarch64-linux-exec",
         [](nlohmann::json& d) { addVerb(d, "argc-argv-envp-apple"); },
         "produces at most 3"},
        {"the narrow env verb without its CRT pair", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) {
             d.at("processArgs").erase("initializeNarrowEnvironmentFn");
             d.at("processArgs").erase("narrowEnvironmentAccessorFn");
         },
         "narrow environment's"},
        {"the wide env verb without its CRT pair", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) {
             d.at("processArgs").erase("initializeWideEnvironmentFn");
             d.at("processArgs").erase("wideEnvironmentAccessorFn");
         },
         "wide environment's"},
        {"the four-value verb through a CRT", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { addVerb(d, "argc-argv-envp-apple"); },
         "produces at most 3"},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(c.name);
        auto const r = loadEdited(c.format, c.edit);
        ASSERT_FALSE(r.has_value()) << "must be refused at load";
        EXPECT_EQ(countAtPath(r, "/entryVerbs"), 1u) << rejectSummary(r);
        EXPECT_EQ(dss::link_format::test::countWithMessage(r, c.why), 1u)
            << rejectSummary(r);
        EXPECT_EQ(errorCount(r), 1u)
            << "refused ONLY for this edit: " << rejectSummary(r);
    }
}

// ── validate(): an environment declaration no verb reads is dead config ────

TEST(EntryEnvironmentVerbs, AnEnvironmentDeclarationNoVerbReadsIsRefused) {
    struct Case {
        char const* name;
        char const* format;
        char const* verb;   // the one verb dropped
        char const* path;   // where the dead declaration is named
    };
    std::vector<Case> const cases{
        {"the entry-stack position", "elf64-x86_64-linux-pie", "argc-argv-envp",
         "/processArgs/envpFollowsArgvTerminator"},
        {"the narrow CRT pair", "pe64-x86_64-windows-exec", "argc-argv-envp",
         "/processArgs/initializeNarrowEnvironmentFn"},
        {"the wide CRT pair", "pe64-x86_64-windows-exec", "argc-wargv-wenvp",
         "/processArgs/initializeWideEnvironmentFn"},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(c.name);
        auto const r = loadEdited(c.format,
                                  [&](nlohmann::json& d) { dropVerb(d, c.verb); });
        ASSERT_FALSE(r.has_value()) << "dead config must be refused at load";
        EXPECT_EQ(countAtPath(r, c.path), 1u) << rejectSummary(r);
        EXPECT_EQ(errorCount(r), 1u)
            << "refused ONLY for this edit: " << rejectSummary(r);
    }
}

// ── the loader: each environment declaration is WHOLE or absent ─────────────

TEST(EntryEnvironmentVerbs, EachEnvironmentDeclarationIsWholeOrAbsent) {
    struct Case {
        char const* name;
        char const* format;
        std::function<void(nlohmann::json&)> edit;
        char const* path;
    };
    std::vector<Case> const cases{
        {"a position with no slot width", "elf64-x86_64-linux-exec",
         [](nlohmann::json& d) { d.at("processArgs").erase("vectorSlotBytes"); },
         "/processArgs"},
        {"a zero slot width", "elf64-x86_64-linux-exec",
         [](nlohmann::json& d) { d.at("processArgs")["vectorSlotBytes"] = 0; },
         "/processArgs/vectorSlotBytes"},
        // An EXEC document, not a PIE one: on an ET_DYN document `processArgs` is a
        // member of the entry cluster, so a REFUSED block also trips the partial-
        // cluster rule and everything that keys on the exec flavor — ✔MEASURED six
        // diagnostics for this one edit on elf64-aarch64-linux-pie — which would say
        // nothing more about THIS rule.
        {"a position that is not a boolean", "elf64-aarch64-linux-exec",
         [](nlohmann::json& d) {
             d.at("processArgs")["envpFollowsArgvTerminator"] = "yes";
         },
         "/processArgs/envpFollowsArgvTerminator"},
        {"an initialize call with no accessor", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) {
             d.at("processArgs").erase("narrowEnvironmentAccessorFn");
         },
         "/processArgs/narrowEnvironmentAccessorFn"},
        {"an empty export name", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) {
             d.at("processArgs")["initializeWideEnvironmentFn"] = "";
         },
         "/processArgs/initializeWideEnvironmentFn"},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(c.name);
        auto const r = loadEdited(c.format, c.edit);
        ASSERT_FALSE(r.has_value()) << "a partial declaration must be refused";
        // The loader drops a `processArgs` block it refused, so `validate()` sees a
        // format with no mechanism — which is legal (pass-through) — and adds
        // nothing: ONE diagnostic, at the declaration's own pointer.
        std::size_t exactHits = 0;
        for (auto const& d : r.error()) {
            if (d.path == c.path) ++exactHits;
        }
        EXPECT_EQ(exactHits, 1u) << rejectSummary(r);
        EXPECT_EQ(errorCount(r), 1u)
            << "refused ONLY for this edit: " << rejectSummary(r);
    }
}
