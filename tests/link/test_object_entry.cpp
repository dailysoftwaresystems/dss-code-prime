// P69 fold 3d, lane p69/xa: a program's ENTRY defined by an OBJECT that a reference compiler built.
//
//   D-PROGRAM-AN-OBJECTS-ENTRY-IS-CALLED-WITHOUT-ITS-ARGUMENTS: an object's `main` linked for
//     `pe64-x86_64-windows-exec` was called on whatever the argument registers held, because the pre-main init that
//     reads the C runtime's argument accessors was made only for an entry a DSS unit defines. ✔MEASURED (lane `xa`'s
//     `argc_pe.py`, run 24's dsscp), as `prog 7 x y`: cl 19.44's `main(argc, argv)` object exited 1091911680 and its
//     `main(argc, argv, envp)` object exited 12, where link.exe's images exit 47 and 11. The init is now made for the
//     entry wherever it is defined, and it calls the object's entry by name (`realizeForeignEntryShape`).
//   D-PROGRAM-ENTRY-IN-AN-OBJECT-INPUT-BESIDE-A-SOURCE-IS-UNDEFINED: a build naming a source and an object that
//     defines `main` (`dsscp --compile helper.c main.o`, `cc helper.c main.o`) was refused K_ProgramEntryUndefined on
//     every format (MEASURED, PE and ELF x86_64): the election read the units' functions alone. The named objects'
//     entries now stand in the same election (`collectObjectEntryCandidates`), so an object's `main` beside a
//     source's `main` is the ambiguity it is between two units.
//
// The families, their fixtures and where their images run are object_entry_support.hpp's. P69 fold 3e adds the
// x86_64 Mach-O family (Apple clang's `-arch x86_64` objects; their images run under Rosetta 2 on the arm64 Mac) and
// the two ELF PIE documents, linked from the ELF families' objects.

#include "object_entry_support.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support::object_entry;

namespace {

// The routes an object's entry is linked through: objects alone (both orders, so the entry's object is not the
// first), beside one source (the single-unit route), beside two (the merged one).
struct Route {
    char const*        label;
    std::vector<Input> inputs;
};

[[nodiscard]] std::vector<Route> routesOf(Family const& f, std::string_view shape) {
    std::string const entry  = objectName(f, shape);
    std::string const helper = objectName(f, "helper");
    return {
        {"objects alone, the entry's first", {fixture(entry), fixture(helper)}},
        {"objects alone, the entry's last", {fixture(helper), fixture(entry)}},
        {"beside one source", {written("helper.c", kHelperSource), fixture(entry)}},
        {"beside two sources",
         {written("helper.c", kHelperSource), fixture(entry), written("other.c", kOtherSource)}},
    };
}

}  // namespace

// ══ EVERY ROUTE LINKS AN OBJECT'S ENTRY ════════════════════════════════════════════════════════════════════════════

// Every leg: each reference's entry object links on every route, for its own executable target; a PE image imports
// the C runtime's argument accessors its init calls the entry with. RED-ON-DISABLE: make no init for an entry another
// input defines -> each PE image imports none of the accessors; leave the named objects' entries out of a unit's
// election -> every route beside a source is refused K_ProgramEntryUndefined.
TEST(ObjectEntry, EveryRouteLinksAnObjectsEntryForItsFormat) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry"};
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        for (auto const& shape : kShapes) {
            for (auto const& route : routesOf(family, shape.name)) {
                SCOPED_TRACE(labelOf(family) + " " + shape.name + ": " + route.label);
                linkCell(scratch.path() / std::to_string(k++), family, route.inputs);
            }
        }
    }
}

// Every leg: an entry an object defines beside one a source defines is refused as two entries are, and so are two
// objects' entries; neither build writes an image. RED-ON-DISABLE: leave the objects' entries out of the units'
// election -> the source's `main` is elected and the build links with two definitions of the entry's name.
TEST(ObjectEntry, AnObjectsEntryBesideAnotherEntryIsAmbiguous) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-ambiguous"};
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        std::string const args   = objectName(family, "args");
        std::string const none   = objectName(family, "void");
        std::string const helper = objectName(family, "helper");
        struct Case {
            char const*        label;
            std::vector<Input> inputs;
        };
        std::vector<Case> const cases{
            {"a source's main and an object's", {written("main.c", kMainSource), fixture(args), fixture(helper)}},
            {"two sources and an object's main",
             {written("main.c", kMainSource), written("other.c", kOtherSource), fixture(args), fixture(helper)}},
            {"two objects' main", {fixture(args), fixture(none), fixture(helper)}},
        };
        for (auto const& c : cases) {
            SCOPED_TRACE(labelOf(family) + ": " + c.label);
            refusedCell(scratch.path() / std::to_string(k++), family, c.inputs,
                        DiagnosticCode::K_ProgramEntryAmbiguous, "c");
        }
    }
}

// ══ NATIVE: each program receives its arguments ═══════════════════════════════════════════════════════════════════

// Where the host runs the family's images: each image, run as `prog 7 x y`, exits as the reference's program of the
// same objects does (47, 5, 11) on every route. RED-ON-DISABLE: make no init for an entry another input defines -> a
// PE program's `main(argc, argv)` reads garbage (MEASURED 1091911680) and `main(argc, argv, envp)` exits 12.
TEST(ObjectEntryNative, EachProgramReceivesItsArgumentsAsTheReferencesDo) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-run"};
    NativeFamilies natives;
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        auto const gate = natives.admit(family);
        if (!gate) continue;
        for (auto const& shape : kShapes) {
            for (auto const& route : routesOf(family, shape.name)) {
                SCOPED_TRACE(labelOf(family) + " " + shape.name + ": " + route.label);
                runCell(scratch.path() / std::to_string(k++), family, *gate, route.inputs, shape.exit);
            }
        }
    }
    if (natives.ranNone()) GTEST_SKIP() << natives.why();
}
