// P69 fold 3f (lane `xa`): A SHIPPED DESCRIPTOR NEVER REDEFINES A PREDEFINED MACRO OF ITS PAIR.
//
// The preprocessor splices a descriptor's `macros`, its preprocessor-visible `constants` and its
// `floatConstants` as `#define`s on `#include`; a name the pair already predefines (the language's,
// the target's or the format's predefined macro, effective on the pair through
// `mergePredefinedMacros`) is then defined twice, and every `#include` of the header diagnoses it
// (C23 6.10.4.1p2). ✔MEASURED 2026-11-17 (lane `xa`, d288): windows.json's `WINAPI` ->
// `__stdcall` over c.lang.json's predefined pe `WINAPI` was published by the editor on every pe
// `#include <windows.h>`, on all five legs (lsp/test_lsp_workspace_build_agreement, four cases).
// The one check is `ffi::shippedMacrosRedefiningPredefinedMacros`; these cells hold it:
//   * the sweep -- EVERY shipped descriptor on EVERY real pair (a target and a format document
//     naming it) whose format the descriptor is available on, the pair's own effective predefined
//     macros: none redefined;
//   * the synthetic negative -- a scratch descriptor naming a predefined macro on each of its three
//     spliced surfaces, read on a pe pair and on an ELF pair: each name the pair predefines is
//     named, the pair decides which, and a name no pair predefines is not.
// Both run in-process on every leg.

#include "analysis/compilation_unit/compilation_unit.hpp"   // predefinedTypeFactsFor
#include "analysis/preprocess/preprocessor.hpp"             // mergePredefinedMacros
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"
#include "shipped_read_pairs.hpp"   // the shipped documents' stems

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

// One real pair: a shipped target and a shipped format document whose `targetArch` names it, with
// the pair's effective predefined macros for the C language.
struct MacroPair {
    std::string                     label;
    std::string                     arch;
    ObjectFormatKind                kind = ObjectFormatKind::Unknown;
    PredefinedTypeFacts             facts;
    std::vector<PredefinedMacroDef> predefined;
};

struct Pairs {
    std::shared_ptr<GrammarSchema const> c;
    std::vector<MacroPair>               pairs;
    std::string                          failure;   // empty <=> every document loaded and merged
};

[[nodiscard]] Pairs realPairs() {
    Pairs out;
    auto c = GrammarSchema::loadShipped("c");
    if (!c.has_value()) {
        out.failure = "the C language does not load";
        return out;
    }
    out.c = *c;
    using test_support::shipped_read_pairs_detail::stems;
    std::vector<std::shared_ptr<TargetSchema>> targets;
    for (std::string const& name : stems("targets", ".target.json")) {
        auto r = TargetSchema::loadShipped(name);
        if (!r.has_value()) {
            out.failure += "target '" + name + "' does not load; ";
            continue;
        }
        targets.push_back(*r);
    }
    for (std::string const& name : stems("object-formats", ".format.json")) {
        auto r = ObjectFormatSchema::loadShipped(name);
        if (!r.has_value()) {
            out.failure += "object format '" + name + "' does not load; ";
            continue;
        }
        ObjectFormatSchema const& format = **r;
        for (auto const& target : targets) {
            if (format.targetArch() != target->name()) continue;   // not a real pair
            MacroPair pair;
            pair.label = std::string{target->name()} + ":" + std::string{format.name()};
            pair.arch  = std::string{target->name()};
            pair.kind  = format.kind();
            pair.facts = predefinedTypeFactsFor(*target, format);
            MergedPredefinedMacros merged = mergePredefinedMacros(
                out.c->preprocess().predefinedMacros, target->predefinedMacros(), format.predefinedMacros(),
                format.kind(), out.c->preprocess().mutuallyExclusivePredefinedMacros, &pair.facts, out.c.get());
            if (!merged.conflicts.empty()) {
                out.failure += "the predefined macros of " + pair.label + " conflict; ";
                continue;
            }
            pair.predefined = std::move(merged.effective);
            out.pairs.push_back(std::move(pair));
        }
    }
    return out;
}

[[nodiscard]] std::optional<std::vector<std::string>> redefinedOn(fs::path const& descriptor, MacroPair const& pair,
                                                                  GrammarSchema const* c, DiagnosticReporter& rep) {
    ffi::ShippedPairFacts const facts{c, pair.facts.dataModel, pair.facts.charIsUnsigned, pair.facts.abiTypedefs,
                                      pair.facts.longDoubleFormat};
    return ffi::shippedMacrosRedefiningPredefinedMacros(descriptor, pair.predefined,
                                                        std::optional<std::string_view>{pair.arch}, pair.kind,
                                                        &facts, rep);
}

[[nodiscard]] std::string joined(std::vector<std::string> const& names) {
    std::string s;
    for (std::string const& n : names) s += (s.empty() ? "" : ", ") + n;
    return s;
}

// What the reader said, so a red names its reason.
[[nodiscard]] std::string said(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

}  // namespace

// The sweep: every shipped descriptor x every real pair whose format it is available on.
TEST(ShippedMacrosOverPredefinedMacros, NoShippedDescriptorRedefinesAPredefinedMacroOfAPairItNames) {
    Pairs const all = realPairs();
    ASSERT_TRUE(all.failure.empty()) << all.failure;
    ASSERT_GE(all.pairs.size(), 22u) << "the real-pair enumeration collapsed";
    std::vector<fs::path> descriptors;
    for (auto const& e : fs::recursive_directory_iterator{dss::test::configRoot() / "shippedLibs"}) {
        if (e.is_regular_file() && e.path().extension() == ".json") descriptors.push_back(e.path());
    }
    std::sort(descriptors.begin(), descriptors.end());
    ASSERT_GE(descriptors.size(), 40u) << "the descriptor walk collapsed";
    std::size_t asked = 0;
    for (fs::path const& descriptor : descriptors) {
        std::string const name = fs::relative(descriptor, dss::test::configRoot() / "shippedLibs").generic_string();
        std::size_t askedHere = 0;
        for (MacroPair const& pair : all.pairs) {
            if (!ffi::shippedHeaderAvailableForFormat(descriptor, pair.kind)) continue;
            ++askedHere;
            DiagnosticReporter rep;
            auto const redefined = redefinedOn(descriptor, pair, all.c.get(), rep);
            ASSERT_TRUE(redefined.has_value()) << name << " cannot be read on " << pair.label << ":" << said(rep);
            EXPECT_TRUE(redefined->empty())
                << name << " redefines what " << pair.label << " predefines: " << joined(*redefined);
        }
        EXPECT_GE(askedHere, 1u) << name << " is available on no real pair";
        asked += askedHere;
    }
    EXPECT_GE(asked, descriptors.size()) << "every descriptor is asked on a pair";
}

// The synthetic negative: one predefined name on each spliced surface, beside one no pair predefines.
TEST(ShippedMacrosOverPredefinedMacros, ADescriptorRedefiningOneIsNamedWithTheNamesItRedefines) {
    Pairs const all = realPairs();
    ASSERT_TRUE(all.failure.empty()) << all.failure;
    auto const pairNamed = [&](std::string_view label) -> MacroPair const* {
        for (MacroPair const& p : all.pairs)
            if (p.label == label) return &p;
        return nullptr;
    };
    MacroPair const* const pe  = pairNamed("x86_64:pe64-x86_64-windows-exec");
    MacroPair const* const elf = pairNamed("x86_64:elf64-x86_64-linux-exec");
    ASSERT_NE(pe, nullptr);
    ASSERT_NE(elf, nullptr);

    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "shipped-macros-over-predefined"};
    fs::path const descriptor = scratch.path() / "probe.json";
    std::ofstream(descriptor, std::ios::binary)
        << "{\n"
           "  \"header\": \"probe.h\",\n"
           "  \"availableObjectFormats\": [\"pe\", \"elf\"],\n"
           "  \"symbols\": [],\n"
           "  \"macros\": [\n"
           "    { \"name\": \"WINAPI\", \"replacement\": \"__stdcall\" },\n"
           "    { \"name\": \"PROBE_OWN_MACRO\", \"replacement\": \"1\" }\n"
           "  ],\n"
           "  \"constants\": [\n"
           "    { \"name\": \"_WIN64\", \"value\": 1, \"type\": \"i32\" },\n"
           "    { \"name\": \"PROBE_OWN_CONSTANT\", \"value\": 2, \"type\": \"i32\" }\n"
           "  ],\n"
           "  \"floatConstants\": [\n"
           "    { \"name\": \"__STDC__\", \"value\": \"1.0\", \"type\": \"f64\" }\n"
           "  ]\n"
           "}\n";

    DiagnosticReporter onPe;
    auto const peRedefined = redefinedOn(descriptor, *pe, all.c.get(), onPe);
    ASSERT_TRUE(peRedefined.has_value()) << "the scratch descriptor reads on pe:" << said(onPe);
    EXPECT_EQ(*peRedefined, (std::vector<std::string>{"WINAPI", "_WIN64", "__STDC__"}))
        << "each spliced surface's predefined name, sorted, once: " << joined(*peRedefined);

    DiagnosticReporter onElf;
    auto const elfRedefined = redefinedOn(descriptor, *elf, all.c.get(), onElf);
    ASSERT_TRUE(elfRedefined.has_value()) << "the scratch descriptor reads on ELF:" << said(onElf);
    EXPECT_EQ(*elfRedefined, (std::vector<std::string>{"__STDC__"}))
        << "the pair decides: ELF predefines neither `WINAPI` nor `_WIN64`: " << joined(*elfRedefined);
}
