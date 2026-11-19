// P69 fold 3e, lane p69/xa (the review of fold 3d, X1, and its neighbour N1): ONE election decides a program's entry,
// whatever grammar the build resolved and whichever input defines it.
//
// ✔MEASURED before (lane `xa`, the r36 dsscp, against GNU ld 2.42, ld.lld 18.1.3 and MinGW-w64 GNU ld 2.42; every
// reference links each of these cells): an assembly unit elected its entry by itself, over the dialect's own label
// list, so `helper.s main.o` was refused at the startup on ELF (K_SymbolUndefined) and objects alone were refused the
// same with the dialect and with no `--language`. ★ With no `--language` on PE, objects were linked into an image
// entered at a function no election chose: the startup took a module's only function for the entry, and the
// program exited 298975272 (cl's objects) and 1063223336 (gcc's) where lld-link and GNU ld exit 42.
//
// Now a dialect declares its entry rows in the language's own shape (`assembly.entryLabels`: each label with the
// materialization verbs a program entered there may take), the one vocabulary (`entryLinkNames`) reads a language's
// rows and a dialect's alike, the assembly unit is one candidate source of the link's one election
// (`electAssembledEntry`), and the startup takes no entry the election did not name.
//
// The same vocabulary judges a C entry's SIGNATURE (class C's N3): the semantic tier's entry gate reads the rows the
// build's format REALIZES, so a definition of an entry name whose signature is none of them is entered with the
// richest and warned, and a name the format realizes no row of is an ordinary function there.
//
// The families, their fixtures and where their images run are object_entry_support.hpp's.

#include "object_entry_support.hpp"

#include "asm/asm.hpp"
#include "core/types/entry_shape.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support::object_entry;

namespace {

// The two assembly grammars a build of objects resolves beside C: the target's dialect named, and no `--language`
// (that dialect by the target's declaration).
[[nodiscard]] std::vector<std::string> assemblyGrammarsOf(Family const& f) { return {dialectOf(f), std::string{}}; }

// Each linking cell of an assembly unit beside a reference's object, and what its program exits as `prog 7 x y`.
struct AsmCell {
    char const*        label;
    std::vector<Input> inputs;
    std::uint32_t      exit;
};

[[nodiscard]] std::vector<AsmCell> asmCellsOf(Family const& f) {
    std::string const args = objectName(f, "args"), helper = objectName(f, "helper");
    return {
        {"helper.s, then the object's entry", {written("helper.s", helperAsm(f)), fixture(args)}, 47u},
        {"the object's entry, then helper.s", {fixture(args), written("helper.s", helperAsm(f))}, 47u},
        {"main.s, then the helper object", {written("main.s", mainAsm(f)), fixture(helper)}, 5u},
    };
}

// The families' executable targets, each once: every format whose realized rows the entry gate reads.
[[nodiscard]] std::vector<Family> execFamilies() {
    std::vector<Family> out;
    for (auto const& f : kFamilies) {
        if (std::none_of(out.begin(), out.end(),
                         [&](Family const& g) { return std::string_view{g.execTarget} == f.execTarget; })) {
            out.push_back(f);
        }
    }
    return out;
}

// Whether `code` was reported, and only ever as a WARNING.
[[nodiscard]] bool warnedOnly(DiagnosticReporter const& rep, DiagnosticCode code) {
    bool any = false;
    for (auto const& d : rep.all()) {
        if (d.code != code) continue;
        if (d.severity != DiagnosticSeverity::Warning) return false;
        any = true;
    }
    return any;
}

// Darwin's four-parameter `main`. It never reads its fourth parameter: where the format realizes no four-value row,
// nothing defines it.
constexpr char const* kFourParameterMain =
    "int main(int argc, char **argv, char **envp, char **apple) { (void)argv; (void)envp; (void)apple; "
    "return argc; }\n";

// Whether the family's executable format realizes the four-value entry row (Darwin's `apple` vector), by its document.
[[nodiscard]] bool realizesTheFourthVector(Family const& f) {
    std::string_view const spec{f.execTarget};
    auto const format = ObjectFormatSchema::loadShipped(spec.substr(spec.find(':') + 1));
    if (!format.has_value()) {
        ADD_FAILURE() << "cannot load the shipped format of " << f.execTarget;
        return false;
    }
    return (*format)->realizesEntryVerb(EntryMaterialization::ArgcArgvEnvpApple);
}

[[nodiscard]] std::size_t importsOf(fs::path const& image, char const* symbol) {
    return test_support::dependencyOccurrences(test_support::peImportedSymbols(bytesOf(image)), symbol);
}

// One C source built alone for the family's executable target: it links, and the entry gate's warning is said, as a
// warning, naming `name` as the program's entry on the format -- or, where `warned` is false, is not said at all.
void entrySignatureCell(fs::path const& dir, Family const& family, char const* source, char const* name, bool warned,
                        Built& b) {
    build(dir, {written("main.c", source)}, family.execTarget, b);
    ASSERT_EQ(b.rc, 0) << b.diagnostics;
    ASSERT_FALSE(b.image.empty()) << "the build writes one image";
    if (!warned) {
        EXPECT_FALSE(reported(b.rep, DiagnosticCode::S_EntryShapeNotDeclared)) << b.diagnostics;
        return;
    }
    EXPECT_TRUE(warnedOnly(b.rep, DiagnosticCode::S_EntryShapeNotDeclared)) << b.diagnostics;
    EXPECT_NE(textOf(b.rep, DiagnosticCode::S_EntryShapeNotDeclared)
                  .find("'" + std::string{name} + "' is a PROGRAM ENTRY on this format"),
              std::string::npos)
        << b.diagnostics;
}

}  // namespace

// ══ EVERY GRAMMAR ELECTS AN OBJECT'S ENTRY ═════════════════════════════════════════════════════════════════════════

// Every leg: objects alone, in both orders, elect the entry one of them defines under the target's assembly dialect
// named and under no `--language`, as under C (test_object_entry.cpp's routes): one vocabulary reads the dialect's
// entry rows and the language's alike. RED-ON-DISABLE: leave the dialect's `entryLabels` rows out of `entryLinkNames`
// -> every cell is refused K_ProgramEntryUndefined ("declares no entry name").
TEST(ObjectEntry, EveryGrammarElectsAnObjectsEntry) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-grammar"};
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        std::string const entry = objectName(family, "args"), helper = objectName(family, "helper");
        for (auto const& grammar : assemblyGrammarsOf(family)) {
            for (auto const& inputs : {std::vector<Input>{fixture(entry), fixture(helper)},
                                       std::vector<Input>{fixture(helper), fixture(entry)}}) {
                SCOPED_TRACE(labelOf(family) + ", " + grammarLabel(grammar) + ", " + inputs.front().name + " first");
                linkCell(scratch.path() / std::to_string(k++), family, inputs, grammar);
            }
        }
    }
}

// ══ AN ASSEMBLY UNIT'S ENTRY STANDS IN THE SAME ELECTION ═══════════════════════════════════════════════════════════

// Every leg, every family, under the dialect named and no `--language`: an assembly unit links beside the object
// that defines the entry (either order) and beside the object its own entry calls; its entry beside an object's is
// the ambiguity two entries are; and a unit whose only function is no entry name (`MAIN`: the match is the
// references', case-sensitive) is refused, naming the entry name it lacks -- a module's only function is never taken
// for the entry. RED-ON-DISABLE: leave the objects out of the assembled election -> both helper.s cells are refused
// K_ProgramEntryUndefined; leave the unit out -> main.s beside the helper object is refused the same, and main.s
// beside the object's entry links; hand the startup an empty election -> MAIN.s is refused by the startup, whose
// refusal names no entry name.
TEST(ObjectEntry, AnAssemblyUnitsEntryIsElectedWithTheObjects) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-asm"};
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        for (auto const& grammar : assemblyGrammarsOf(family)) {
            std::string const at = labelOf(family) + ", " + grammarLabel(grammar) + ": ";
            for (auto const& c : asmCellsOf(family)) {
                SCOPED_TRACE(at + c.label);
                linkCell(scratch.path() / std::to_string(k++), family, c.inputs, grammar);
            }
            {
                SCOPED_TRACE(at + "main.s beside the object's entry");
                refusedCell(scratch.path() / std::to_string(k++), family,
                            {written("main.s", mainAsm(family)), fixture(objectName(family, "args"))},
                            DiagnosticCode::K_ProgramEntryAmbiguous, grammar);
            }
            {
                SCOPED_TRACE(at + "MAIN.s alone");
                refusedCell(scratch.path() / std::to_string(k++), family,
                            {written("MAIN.s", soleAsm(family, "MAIN", 5u))}, DiagnosticCode::K_ProgramEntryUndefined,
                            grammar, "'" + std::string{family.symbolPrefix} + "main'");
            }
        }
    }
}

// ══ AN ENTRY'S SIGNATURE IS JUDGED BY THE ROWS ITS FORMAT REALIZES ═════════════════════════════════════════════════

// ✔MEASURED before (lane `xa`: cl 19.44, clang-cl 19.1.5, MinGW-w64 gcc 13.2, gcc 13.3, clang 18.1.3): the entry gate
// REFUSED, on every format, a signature the language declares no row for, where gcc (-Wmain, a warning) and cl link
// `int main(int, double)` and `void main(void)`. Every leg, every executable format: each links, entered with the
// richest row its format realizes (a PE image's init reads the argument count, the vector and the environment), and
// S_EntryShapeNotDeclared is said as a warning naming `main`.
// THE SHAPE DIMENSION (P69 fold 3f): Darwin's four-parameter `main` is a row the language declares and only Mach-O
// realizes. ✔MEASURED (lane `xa`, 2026-11-16, probe-reference-cc; the program never reads its fourth parameter):
// gcc 13.3.0 on Linux and MinGW-w64 gcc 13.2.0 compile it silent (-Wall and -Wpedantic warn -Wmain) and run it, argv
// and the environment intact; cl 19.52 compiles it silent at /W4 and /Wall and runs it the same; clang 18.1.3 refuses
// it ("too many parameters (4) for 'main'"). So on ELF and pe64 it is entered with the richest row the format realizes,
// `argc-argv-envp`, and warned (it was refused K_ProgramEntryUndefined until fold 3e); on Mach-O it is its own row,
// silent. program/test_entry_envp_run RUNS it on the host's own format.
// RED-ON-DISABLE: stamp no row on such a definition -> no entry is elected and every build is refused
// K_ProgramEntryUndefined (the four-parameter cell is stamped with its declared, unrealized row and refused the same);
// make the diagnostic an error -> every build is refused at the declarator; judge the signature by every verb the
// language declares -> the four-parameter cell on ELF and pe64 is stamped with the four-value row, silent, and refused
// K_ProgramEntryUndefined.
TEST(EntrySignature, AnEntryNameOfNoRealizedRowIsEnteredAndWarned) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "entry-signature-main"};
    std::size_t k = 0;
    for (auto const& family : execFamilies()) {
        // THE SHAPE DIMENSION: each signature, and whether this format realizes a row of it -- read from the format's
        // own document, never from its name. A realized row is that row, silent; any other signature is entered with
        // the richest row the format realizes, and warned.
        bool const fourthVector = realizesTheFourthVector(family);
        struct SignatureCell {
            char const* source;
            bool        warned;
            char const* richest;   // the row the warning names it entered with; nullptr: not asserted
        };
        for (SignatureCell const shape :
             {SignatureCell{"int main(int argc, double d) { (void)argc; (void)d; return 11; }\n", true, nullptr},
              SignatureCell{"void main(void) { }\n", true, nullptr},
              SignatureCell{kFourParameterMain, !fourthVector, "argc-argv-envp"}}) {
            SCOPED_TRACE(std::string{family.execTarget} + ": " + shape.source);
            Built b;
            entrySignatureCell(scratch.path() / std::to_string(k++), family, shape.source, "main", shape.warned, b);
            if (shape.warned && shape.richest != nullptr) {
                EXPECT_NE(textOf(b.rep, DiagnosticCode::S_EntryShapeNotDeclared)
                              .find("called with the richest of them, '" + std::string{shape.richest} + "'"),
                          std::string::npos)
                    << b.diagnostics;
            }
            if (isPe(family) && !b.image.empty()) expectTheArgumentAccessorsImported(b.image);
        }
    }
}

// A wide name is an entry only where its format realizes a wide row (✔MEASURED before, lane `xa`: cl, clang-cl and
// MinGW-w64 gcc -municode run `int wmain(void) { return 22; }` to 22; gcc and clang on Linux, and Apple clang, link
// `wmain` beside `main` as an ordinary function, silent). On pe64 a `wmain(void)` is entered with the richest wide row
// (its init reads the wide vector, never the narrow one) and warned, and beside `main` the exec document's precedence
// takes `main`, said; on every other format it is an ordinary function and nothing is said. RED-ON-DISABLE: judge the
// signature by every verb the language declares -> the ELF and Mach-O builds warn; stamp no row -> the PE `wmain`
// alone is refused K_ProgramEntryUndefined and beside `main` is not warned.
TEST(EntrySignature, AWideNameIsAnEntryOnlyWhereItsFormatRealizesAWideRow) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "entry-signature-wide"};
    std::size_t k = 0;
    for (auto const& family : execFamilies()) {
        bool const pe = isPe(family);
        {
            SCOPED_TRACE(std::string{family.execTarget} + ": wmain(void) beside main");
            Built b;
            entrySignatureCell(scratch.path() / std::to_string(k++), family,
                               "int wmain(void) { return 22; }\nint main(void) { return 11; }\n", "wmain", pe, b);
            if (pe) EXPECT_TRUE(reported(b.rep, DiagnosticCode::K_ProgramEntryChosenByPrecedence)) << b.diagnostics;
        }
        if (pe) {
            SCOPED_TRACE(std::string{family.execTarget} + ": wmain(void) alone");
            Built b;
            entrySignatureCell(scratch.path() / std::to_string(k++), family, "int wmain(void) { return 22; }\n",
                               "wmain", true, b);
            if (!b.image.empty()) {
                EXPECT_EQ(importsOf(b.image, "__p___wargv"), 1u);
                EXPECT_EQ(importsOf(b.image, "__p___argv"), 0u) << "the narrow vector is never read";
            }
        }
    }
}

// ══ NATIVE: the elected entry is the one that runs ════════════════════════════════════════════════════════════════

// Where the host runs the family's images: objects alone under each assembly grammar run each shape as the
// references do (47, 5, 11), and so do the assembly unit's programs (helper.s beside the object's `main(argc, argv)`
// 47, main.s beside the helper object 5). RED-ON-DISABLE: restore the startup's single-function fallback and leave a
// build with no `--language` unelected -> a PE program of objects is entered at a function no election chose
// (MEASURED 298975272 and 1063223336 before); make no init for an assembly unit's entry -> its PE program reads
// garbage arguments.
TEST(ObjectEntryNative, TheElectedEntryRunsUnderEveryGrammar) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "object-entry-grammar-run"};
    NativeFamilies natives;
    std::size_t k = 0;
    for (auto const& family : kFamilies) {
        auto const gate = natives.admit(family);
        if (!gate) continue;
        for (auto const& grammar : assemblyGrammarsOf(family)) {
            for (auto const& shape : kShapes) {
                SCOPED_TRACE(labelOf(family) + " " + shape.name + ": objects alone, " + grammarLabel(grammar));
                runCell(scratch.path() / std::to_string(k++), family, *gate,
                        {fixture(objectName(family, "helper")), fixture(objectName(family, shape.name))}, shape.exit,
                        grammar);
            }
            for (auto const& c : asmCellsOf(family)) {
                SCOPED_TRACE(labelOf(family) + ", " + grammarLabel(grammar) + ": " + c.label);
                runCell(scratch.path() / std::to_string(k++), family, *gate, c.inputs, c.exit, grammar);
            }
        }
    }
    if (natives.ranNone()) GTEST_SKIP() << natives.why();
}

// ══ A MODULE THAT STATES NO ENTRY IS NEVER ENTERED AT ITS ONE FUNCTION (class C's N1) ════════════════════════════════

// Every family's executable document: a module of ONE function that states no entry (`userEntrySymbol`), linked
// alone, is refused K_ProgramEntryUndefined ("no program entry was elected"): the startup calls the function the
// build's election chose and no other, and a module's only function is never taken for it. ✔MEASURED before (lane
// `xa`, cd349db1's bytes): the startup took a module's only function for the entry, so a pe64 image of objects with
// no `--language` exited 298975272 where every reference links the archive member's `main` (42). RED-ON-DISABLE:
// restore the single-function fallback in `resolveUserEntrySymbol` -> each module links, entered at its function.
TEST(ObjectEntry, AModuleThatStatesNoEntryIsNeverEnteredAtItsOneFunction) {
    for (Family const& f : execFamilies()) {
        SCOPED_TRACE(f.execTarget);
        std::string_view const spec{f.execTarget};
        std::size_t const colon = spec.find(':');
        auto const target = TargetSchema::loadShipped(spec.substr(0, colon));
        ASSERT_TRUE(target.has_value());
        auto const format = ObjectFormatSchema::loadShipped(spec.substr(colon + 1));
        ASSERT_TRUE(format.has_value());
        ASSERT_TRUE((*format)->entryPoint().empty()) << "the document names no entry of its own";
        AssembledModule mod;
        mod.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = std::string_view{f.arch} == "arm64"
                        ? std::vector<std::uint8_t>{0x40, 0x05, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6}   // mov w0,#42 ; ret
                        : std::vector<std::uint8_t>{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};              // mov eax,42 ; ret
        mod.functions.push_back(std::move(fn));
        DiagnosticReporter rep;
        LinkedImage const img = linker::link(mod, **target, **format, rep);
        EXPECT_FALSE(img.ok()) << "a module that states no entry was entered at its one function";
        std::size_t elected = 0;
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::K_ProgramEntryUndefined
                && d.actual.find("no program entry was elected") != std::string::npos) {
                ++elected;
            }
        }
        EXPECT_EQ(elected, 1u) << "refused, by name, for the entry no election chose";
    }
}
