// P69 (lane `cs`) — A SECOND NAME OF A DEFINITION IN THE SUMMARY INDEX (`--lto=thin`).
//
// The index is the third door of the cross-unit election (beside the unit linker and the merge),
// and an alias is a definition of its name at all three: it enters the election with its own
// binding and kind (a weak alias yields to a strong definition elsewhere, a strong one beside a
// strong definition is a conflict), it is no body a prefetch can import (it has none), it roots
// its target in the whole-program liveness walk (as DCE roots it), and a call to it is a call to
// its target in the call graph (a recursion through a second name is a recursion).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "mir/summary/summary_index.hpp"
#include "link_facts.hpp"   // ordinaryLinkFormat

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::mirsum;

namespace {

SummaryFunction fn(std::string name, std::uint32_t symbol, SymbolBinding binding = SymbolBinding::Global,
                   std::optional<WeakDefinitionKind> kind = std::nullopt) {
    SummaryFunction f;
    f.name       = std::move(name);
    f.symbol     = symbol;
    f.binding    = binding;
    f.weakKind   = kind;
    f.visibility = SymbolVisibility::Default;
    f.instCount  = 3;
    f.blockCount = 1;
    f.hasReturn  = true;
    return f;
}

void calls(SummaryFunction& f, std::string callee) {
    SummaryCallSite c;
    c.calleeName = callee;
    c.direct     = true;
    f.calls.push_back(std::move(c));
    f.symbolRefs.push_back(std::move(callee));
    std::sort(f.symbolRefs.begin(), f.symbolRefs.end());
}

SummaryAlias alias(std::string name, std::uint32_t symbol, std::string target,
                   SymbolBinding binding = SymbolBinding::Global,
                   std::optional<WeakDefinitionKind> kind = std::nullopt) {
    SummaryAlias a;
    a.name       = std::move(name);
    a.symbol     = symbol;
    a.targetName = std::move(target);
    a.binding    = binding;
    a.weakKind   = kind;
    return a;
}

ModuleSummary mod(std::string digest, std::vector<SummaryFunction> fns, std::vector<SummaryAlias> aliases = {}) {
    ModuleSummary s;
    s.moduleDigest   = std::move(digest);
    s.targetIdentity = "x86_64:elf64";
    s.functions      = std::move(fns);
    s.aliases        = std::move(aliases);
    return s;
}

SummaryIndexPolicy policy() {
    SummaryIndexPolicy p;
    p.inlineThreshold = 50;
    p.maxImportDepth  = 4;
    return p;
}

[[nodiscard]] std::optional<SummaryIndex> index(std::vector<ModuleSummary> const& mods, DiagnosticReporter& rep) {
    return buildSummaryIndex(mods, policy(), dss::test_support::ordinaryLinkFormat(), rep);
}

}  // namespace

TEST(SummaryIndexAlias, AnAliasEntersTheElectionAsADefinitionOfItsName) {
    {
        DiagnosticReporter rep;
        auto const ix = index({mod("d0", {fn("g", 1)}, {alias("f", 2, "g")}), mod("d1", {fn("f", 1)})}, rep);
        ASSERT_TRUE(ix.has_value());
        EXPECT_EQ(ix->conflictingNames, (std::vector<std::string>{"f"}))
            << "a strong alias beside a strong definition of its name is two definitions";
    }
    {
        DiagnosticReporter rep;
        auto const ix = index({mod("d0", {fn("g", 1)},
                                   {alias("f", 2, "g", SymbolBinding::Weak, WeakDefinitionKind::Overridable)}),
                               mod("d1", {fn("f", 1)})},
                              rep);
        ASSERT_TRUE(ix.has_value());
        EXPECT_TRUE(ix->conflictingNames.empty());
        auto const site = ix->definitionOf("f");
        ASSERT_TRUE(site.has_value());
        EXPECT_EQ(site->moduleIndex, 1u) << "a weak alias yields to the strong definition";
    }
    {
        DiagnosticReporter rep;
        auto const ix = index({mod("d0", {fn("g", 1)}, {alias("f", 2, "g")}), mod("d1", {fn("use", 1)})}, rep);
        ASSERT_TRUE(ix.has_value());
        EXPECT_TRUE(ix->conflictingNames.empty());
        EXPECT_FALSE(ix->definitionOf("f").has_value()) << "an alias that wins its name has no body to import";
        EXPECT_TRUE(ix->definitionOf("g").has_value());
    }
}

TEST(SummaryIndexAlias, AnAliasRootsItsTargetInTheLivenessWalk) {
    {
        DiagnosticReporter rep;
        auto const ix = index({mod("d0", {fn("g", 1, SymbolBinding::Local)}, {alias("f", 2, "g")})}, rep);
        ASSERT_TRUE(ix.has_value());
        EXPECT_EQ(ix->liveSymbols.count("g"), 1u) << "the definition an alias names is live";
        EXPECT_EQ(ix->liveSymbols.count("f"), 1u);
        auto const& dead = ix->deadSymbolsPerModule[0];
        EXPECT_EQ(std::count(dead.begin(), dead.end(), "g"), 0);
    }
    {
        // A MODULE-PRIVATE alias is no root of its own (nothing outside its module can name it): only the
        // alias's own statement keeps the definition it names, as DCE keeps it.
        DiagnosticReporter rep;
        auto const ix = index({mod("d0", {fn("g", 1, SymbolBinding::Local)}, {alias("f", 2, "g", SymbolBinding::Local)})},
                              rep);
        ASSERT_TRUE(ix.has_value());
        EXPECT_EQ(ix->liveSymbols.count("g"), 1u) << "the definition a module-private alias names is live";
    }
    // CONTROL: with no alias, the module-private function nothing references is dead.
    DiagnosticReporter rep;
    auto const ix = index({mod("d0", {fn("g", 1, SymbolBinding::Local)})}, rep);
    ASSERT_TRUE(ix.has_value());
    EXPECT_EQ(ix->liveSymbols.count("g"), 0u);
}

TEST(SummaryIndexAlias, ACallToAnAliasIsACallToItsTargetInTheCallGraph) {
    SummaryFunction g = fn("g", 1);
    calls(g, "f");
    SummaryFunction h = fn("h", 3);
    calls(h, "g");
    DiagnosticReporter rep;
    auto const ix = index({mod("d0", {g, h}, {alias("f", 2, "g")})}, rep);
    ASSERT_TRUE(ix.has_value());
    ASSERT_EQ(ix->sccOf.count("g"), 1u);
    ASSERT_EQ(ix->sccOf.count("f"), 1u);
    EXPECT_EQ(ix->sccOf.at("g"), ix->sccOf.at("f")) << "g calls itself through its second name";
    EXPECT_NE(ix->sccOf.at("h"), ix->sccOf.at("g")) << "CONTROL: a caller of the cycle is not in it";
}
