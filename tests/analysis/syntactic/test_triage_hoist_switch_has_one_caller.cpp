// ===========================================================================
// P69 L2b2 (lane `lm`) — THE TRIAGE HOIST'S OFF SWITCH HAS ONE CALLER.
//
// The unit builder's identity-witness setter (spelled in the test below) turns off the parser's
// hoisted base ask for a unit. It exists for ONE reader — the identity witness
// (tests/analysis/syntactic/test_parser_triage_hoist_identity.cpp), which
// compares every shipped unit with the hoist on and off — and is read from no
// language document and no CLI flag, so a compile always runs with the hoist on.
// A second caller would make the switch a configuration surface nothing pins.
//
// THE SCAN: every C and C++ source under src/, integrated_tests/ and tests/ (a
// link is never descended); the spelling may stand only in the builder's own
// header and source, the witness, and this file (which spells it in two pieces).
// Its verdict reads the repository's state, so the entry carries `repo-guard`.
//
// RED-ON-DISABLE: a call of the setter added to any other file under the
// scanned roots (tests/mutations/arms.registry).
// ⚠ MUST run through ctest: the tree is found through the repo root.
// ===========================================================================

#include "core/substrate/path_identity.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST(TriageHoistSwitch, TheSetterIsCalledByTheIdentityWitnessAlone) {
    std::string const spelling = std::string{"setTriageBaseHoist"} + "ForTheIdentityWitness";
    std::set<std::string> const allowed = {
        "src/analysis/compilation_unit/compilation_unit.hpp",
        "src/analysis/compilation_unit/compilation_unit.cpp",
        "tests/analysis/syntactic/test_parser_triage_hoist_identity.cpp",
    };
    std::vector<std::string> const exts = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"};
    fs::path const root = dss::test::repoRoot();
    std::set<std::string> holders;
    std::size_t scanned = 0;
    for (char const* top : {"src", "integrated_tests", "tests"}) {
        auto const walk = dss::core::walkTree(root / top, [&](dss::core::TreeEntry const& e) {
            if (e.kind != dss::core::EntryKind::File) return dss::core::WalkStep::Continue;
            fs::path const p{e.at.spelling().generic()};
            if (std::find(exts.begin(), exts.end(), p.extension().string()) == exts.end())
                return dss::core::WalkStep::Continue;
            ++scanned;
            std::ifstream in{p, std::ios::binary};
            std::string const text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            EXPECT_FALSE(in.bad()) << "unreadable: " << p.generic_string();
            if (text.find(spelling) != std::string::npos) {
                holders.insert(p.lexically_relative(root).generic_string());
            }
            return dss::core::WalkStep::Continue;
        });
        EXPECT_FALSE(walk.error) << "the walk of " << top << " stopped at " << walk.where.spelling().generic()
                                 << ": " << walk.error.message() << " -- the scan did not read the whole tree";
    }
    EXPECT_GT(scanned, 1000u) << "the scan read " << scanned << " sources: it did not reach the tree";
    EXPECT_EQ(holders, allowed) << "the setter's spelling stands in a file that is not its home or its one caller";
}
