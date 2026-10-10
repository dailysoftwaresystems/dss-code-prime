// A STATED END AGAINST A COUNTED ONE — WHAT A MODULE CARRIES ABOUT ITS SYMBOL-ID SPACE
// (D-MIR-SYNTHESIZED-SYMBOL-MINTED-INSIDE-THE-NAME-TABLE)
//
// `symbolIdEnd()` of a module nobody told anything reads exactly like a real end: one
// past the symbols its arenas define. The door once aborted only on a BUILDER never
// told its end; the frozen module dropped the fact, and the first rebuild's
// `continueSymbolIdsOf` turned the counted end into a stated one — which is how the
// `.dssir` reader, which stated nothing, got through in silence (✔MEASURED 2026-10-08:
// a text declaring `%1 "f"` and `%2 "g"` and defining `f` alone read back ending at 2,
// and MIR→LIR named a block `%2`). The fact now travels WITH the module:
//
//   * a builder's end is STATED by a name table's end (`stateSymbolIdEnd`), by "this
//     module is its own table" (`stateSelfContainedSymbolIds`), or by the statement of
//     the module it replaces, COPIED (`continueSymbolIdsOf`) — and by nothing else:
//     a symbol it is handed and an id it is told to keep clear of raise the end and
//     state nothing;
//   * `finish` hands the fact to the module, a move carries it and empties its
//     source, a rebuild copies it and never makes it, the frozen module's
//     continuation takes it from the module, the `.dssir` reader states what it read;
//   * both leaves — `MirBuilder::mintSymbol`, `MirSymbolIdContinuation::mint` — REFUSE
//     to mint past a counted end, by name, by aborting: the entrance that forgot its
//     statement is at fault, never an input.
//
// ═══ WHY THESE CASES HAVE A BINARY OF THEIR OWN ════════════════════════════════
//
// The refusal is an ABORT. Under the very mutations these cases exist to catch — a
// fact dropped by `finish`, by a move or by the reader; a statement that states
// nothing — every test anywhere that then mints from such a module dies WITH ITS
// PROCESS, and a dead process names no case: the mutation would be seen and nothing
// could be read off it. So this file keeps one rule the rest of the suite has no
// need of: **NO CASE HERE MINTS FROM A MODULE BEFORE IT HAS ASSERTED THAT MODULE'S
// FACT** (an `ASSERT_`, which ends the case), and the only mints over a counted end
// are inside death statements. Each mutation then reddens the cases that name it,
// and kills nothing.
//
// What the door does with the end itself — every pass minting past it, the name
// table kept clear of — is mir/test_synth_symbol_floor; the reader's table and the
// lowering's own minter are mir/test_mir_text_reader_never_aborts.
//
// RED-ON-DISABLE (each alone): `stateSelfContainedSymbolIds` states nothing; `finish`
// drops the fact; a move drops it; `continueSymbolIdsOf` SETS the fact instead of
// copying it; a continuation believes every end stated; the reader's statement
// removed; either leaf mints past a counted end.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"   // LibrarySynthesis
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/mangling/c_mangle.hpp"           // CSymbolDecorationScheme
#include "mir/merge/synth_threads_shim.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_text.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"   // cloneGlobalsVerbatim (a rebuild's helper)

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dss;

namespace {

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// `i32 f(void) { return 0; }` under the symbol `sym`.
void addFunction(MirBuilder& mb, TypeInterner& in, std::uint32_t sym) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    mb.addFunction(in.fnSig({}, i32, CallConv::CcSysV), SymbolId{sym});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
}

// A module whose builder was told NOTHING: its end (4) is only counted.
Mir countedModule(TypeInterner& in) {
    MirBuilder mb;
    addFunction(mb, in, 3);
    return std::move(mb).finish();
}

// A module made from a name table that ends at 20.
Mir statedModule(TypeInterner& in) {
    MirBuilder mb;
    mb.stateSymbolIdEnd(20);
    addFunction(mb, in, 3);
    return std::move(mb).finish();
}

}  // namespace

// What does NOT state an end: holding a symbol, and keeping clear of an id beside the
// module. Both raise the end; neither says where the id space ends.
TEST(SymbolIdEndIsStated, ABuilderToldNothingIsCountedWhateverRaisesItsEnd) {
    TypeInterner in{CompilationUnitId{1}};
    MirBuilder nothing;
    EXPECT_FALSE(nothing.symbolIdEndIsStated()) << "a builder told nothing";
    addFunction(nothing, in, 3);
    EXPECT_FALSE(nothing.symbolIdEndIsStated()) << "a symbol it is handed states nothing";
    nothing.keepSymbolIdsClearOf(SymbolId{40});
    EXPECT_FALSE(nothing.symbolIdEndIsStated())
        << "raising the end past an id beside the module states nothing";
    EXPECT_EQ(nothing.symbolIdEnd(), 41u) << "CONTROL: the end itself was raised";
    // A module no builder finished is counted too: nobody said anything of it.
    EXPECT_FALSE(Mir{}.symbolIdEndIsStated());
}

TEST(SymbolIdEndIsStated, ANameTablesEndStatesIt) {
    TypeInterner in{CompilationUnitId{1}};
    MirBuilder table;
    table.stateSymbolIdEnd(20);
    ASSERT_TRUE(table.symbolIdEndIsStated());
    addFunction(table, in, 3);
    EXPECT_EQ(table.symbolIdEnd(), 20u) << "a symbol below the table's end moves nothing";
    EXPECT_EQ(table.mintSymbol().v, 20u) << "the first fresh id is the table's end";
}

// "This module is its own table" is a STATEMENT: the end is the counted one, said —
// not moved.
TEST(SymbolIdEndIsStated, SayingAModuleIsItsOwnTableStatesItsEnd) {
    TypeInterner in{CompilationUnitId{1}};
    MirBuilder self;
    addFunction(self, in, 3);
    self.stateSelfContainedSymbolIds();
    ASSERT_TRUE(self.symbolIdEndIsStated())
        << "a hand-built module said it is its own table, and its end is still only counted";
    EXPECT_EQ(self.symbolIdEnd(), 4u) << "its end is the counted one — stated, not moved";
    EXPECT_EQ(self.mintSymbol().v, 4u);
}

// `finish` hands the builder's fact to the module it makes — both ways.
TEST(SymbolIdEndIsStated, FinishHandsTheFactToTheModule) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const counted = countedModule(in);
    EXPECT_FALSE(counted.symbolIdEndIsStated()) << "a builder told nothing finished a stated module";
    EXPECT_EQ(counted.symbolIdEnd(), 4u) << "CONTROL: one past the symbol it defines";
    Mir const stated = statedModule(in);
    EXPECT_TRUE(stated.symbolIdEndIsStated()) << "the module lost the statement its builder made";
    EXPECT_EQ(stated.symbolIdEnd(), 20u);
}

// A REBUILD COPIES THE FACT; IT NEVER MAKES IT — by each of the three ways a rebuild
// carries a module's ids (the door, the module-facts carrier, the clone-globals
// helper). The first rebuild used to SET it, and so turned a counted end into a
// stated one.
TEST(SymbolIdEndIsStated, ARebuildCopiesTheFactAndNeverMakesIt) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const counted = countedModule(in);
    Mir const stated  = statedModule(in);
    ASSERT_FALSE(counted.symbolIdEndIsStated());
    ASSERT_TRUE(stated.symbolIdEndIsStated());
    {
        MirBuilder direct;
        direct.continueSymbolIdsOf(counted);
        EXPECT_FALSE(direct.symbolIdEndIsStated())
            << "continuing a module whose end was only counted made the end a stated one";
        EXPECT_EQ(direct.symbolIdEnd(), counted.symbolIdEnd()) << "CONTROL: the end itself is continued";
        EXPECT_FALSE(std::move(direct).finish().symbolIdEndIsStated());
        MirBuilder facts;
        facts.carryModuleFactsOf(counted);
        EXPECT_FALSE(std::move(facts).finish().symbolIdEndIsStated());
        MirBuilder viaHelper;
        opt::passes::cloneGlobalsVerbatim(counted, viaHelper);
        EXPECT_FALSE(std::move(viaHelper).finish().symbolIdEndIsStated())
            << "a clone-globals helper stated an end its source never had";
    }
    {
        MirBuilder direct;
        direct.continueSymbolIdsOf(stated);
        EXPECT_TRUE(direct.symbolIdEndIsStated());
        EXPECT_TRUE(std::move(direct).finish().symbolIdEndIsStated());
        MirBuilder facts;
        facts.carryModuleFactsOf(stated);
        EXPECT_TRUE(std::move(facts).finish().symbolIdEndIsStated());
        MirBuilder viaHelper;
        opt::passes::cloneGlobalsVerbatim(stated, viaHelper);
        EXPECT_TRUE(std::move(viaHelper).finish().symbolIdEndIsStated())
            << "a rebuild lost the statement its source carried";
    }
    {
        // A statement the builder already made stands, whatever it then continues.
        MirBuilder already;
        already.stateSymbolIdEnd(9);
        already.continueSymbolIdsOf(counted);
        EXPECT_TRUE(already.symbolIdEndIsStated());
    }
    {
        // And a rebuilt builder that continues a stated module mints its end.
        MirBuilder rebuilt;
        rebuilt.continueSymbolIdsOf(stated);
        ASSERT_TRUE(rebuilt.symbolIdEndIsStated());
        EXPECT_EQ(rebuilt.mintSymbol().v, 20u);
    }
}

// A MOVE CARRIES THE FACT AND EMPTIES ITS SOURCE (the emptied module is nobody's
// table), by construction and by assignment.
TEST(SymbolIdEndIsStated, AMoveCarriesTheFactAndEmptiesItsSource) {
    TypeInterner in{CompilationUnitId{1}};
    Mir from = statedModule(in);
    ASSERT_TRUE(from.symbolIdEndIsStated());
    Mir constructed{std::move(from)};
    EXPECT_TRUE(constructed.symbolIdEndIsStated()) << "a move construction dropped the statement";
    EXPECT_FALSE(from.symbolIdEndIsStated());   // NOLINT(bugprone-use-after-move): the moved-from state is the subject
    Mir assigned;
    ASSERT_FALSE(assigned.symbolIdEndIsStated());
    assigned = std::move(constructed);
    EXPECT_TRUE(assigned.symbolIdEndIsStated()) << "a move assignment dropped the statement";
    EXPECT_FALSE(constructed.symbolIdEndIsStated());   // NOLINT(bugprone-use-after-move)
}

// The continuation a lowering tier mints from takes the fact FROM THE MODULE — never
// from a count, never by default — and keeping clear of an id beside the module
// states nothing there either.
TEST(SymbolIdEndIsStated, AContinuationTakesTheFactFromItsModule) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const counted = countedModule(in);
    Mir const stated  = statedModule(in);
    ASSERT_FALSE(counted.symbolIdEndIsStated());
    ASSERT_TRUE(stated.symbolIdEndIsStated());
    MirSymbolIdContinuation overCounted{counted};
    EXPECT_FALSE(overCounted.endIsStated())
        << "a continuation over a counted module believes its end stated";
    overCounted.keepClearOf(SymbolId{40});
    EXPECT_FALSE(overCounted.endIsStated()) << "keeping clear of an id states nothing";
    EXPECT_EQ(overCounted.end(), 41u) << "CONTROL: the end itself was raised";
    MirSymbolIdContinuation overStated{stated};
    ASSERT_TRUE(overStated.endIsStated());
    EXPECT_EQ(overStated.mint().v, 20u);
}

namespace {

// `text` with its whole `symbols { … }` section cut out (the section is optional: a
// function is still spelled by its slot).
std::string withoutSymbolsSection(std::string text) {
    std::string const open = "symbols {\n";
    std::size_t const at = text.find(open);
    std::size_t const close = at == std::string::npos ? at : text.find("}\n", at);
    if (close == std::string::npos) {
        throw std::runtime_error("the emitted text holds no `symbols` section:\n" + text);
    }
    text.erase(at, close + 2 - at);
    return text;
}

}  // namespace

// THE READER STATES WHAT IT READ — with a `symbols` section, and without one. It is
// the one entrance fed by input, so it states its end on every path a module leaves
// it by. Read only: this case lowers nothing (the lowering of a module read from text
// is mir/test_mir_text_reader_never_aborts).
TEST(SymbolIdEndIsStated, AModuleReadFromTextStatesItsEndWithOrWithoutASymbolsSection) {
    TypeInterner in{CompilationUnitId{1}};
    MirBuilder mb;
    mb.stateSymbolIdEnd(2);
    addFunction(mb, in, 1);
    Mir source = std::move(mb).finish();
    rederiveStructCfMarkers(source);   // the reader's verifier checks stored == derived
    std::unordered_map<std::uint32_t, std::string> const names{{1u, "f"}};
    MirTextContext ctx{&in, nullptr, &names};
    DiagnosticReporter w;
    std::string const text = emitMir(source, ctx, w);
    ASSERT_NE(text.find("symbols {"), std::string::npos) << text;
    std::string const sectionless = withoutSymbolsSection(text);
    ASSERT_EQ(sectionless.find("symbols"), std::string::npos) << sectionless;
    for (std::string const* const t : {&text, &sectionless}) {
        SCOPED_TRACE(t == &text ? "with a `symbols` section" : "with no `symbols` section");
        DiagnosticReporter r;
        auto const parsed = parseMir(*t, CompilationUnitId{7}, r);
        ASSERT_TRUE(parsed->ok) << *t;
        EXPECT_TRUE(parsed->mir.symbolIdEndIsStated())
            << "the reader handed out a module whose id space nobody stated";
        EXPECT_EQ(parsed->mir.symbolIdEnd(), 2u) << "one past `f`, the one symbol the text defines";
    }
}

// BOTH LEAVES DIE OVER A COUNTED END, by name — a builder that CONTINUES a module
// nobody stated (the hole the first rebuild used to paper over), and the frozen
// module's continuation. And the death reaches through a real pass: a hand-built
// module that says nothing, handed to a pass that mints, dies at the pass's first id
// — which is what makes an entrance nobody has written yet loud the day it is added.
TEST(SymbolIdEndIsStatedDeath, AMintPastAnEndThatWasOnlyCountedDies) {
    EXPECT_DEATH({
        TypeInterner in{CompilationUnitId{1}};
        Mir const counted = countedModule(in);
        MirBuilder rebuilt;
        rebuilt.continueSymbolIdsOf(counted);
        (void)rebuilt.mintSymbol();
    }, "MirBuilder::mintSymbol fatal: this module's symbol-id space was never stated");
    EXPECT_DEATH({
        TypeInterner in{CompilationUnitId{1}};
        Mir const counted = countedModule(in);
        MirSymbolIdContinuation past{counted};
        (void)past.mint();
    }, "MirSymbolIdContinuation::mint fatal: this module's symbol-id space was never stated");
    EXPECT_DEATH({
        TypeInterner in{CompilationUnitId{1}};
        Mir const counted = countedModule(in);
        MirSymbolIdContinuation past{counted};
        past.keepClearOf(SymbolId{40});   // raises the end; states nothing
        (void)past.mintOrAbort("theLoweringThatMints");
    }, "MirSymbolIdContinuation::mint fatal: this module's symbol-id space was never stated");
    // Through a real pass: the <threads.h> shim synthesis over a module whose builder
    // said nothing. A function of its own: a death statement is a macro argument, and
    // a brace list's commas would split it.
    auto const shimOverAModuleThatSaysNothing = [] {
        TypeInterner in{CompilationUnitId{1}};
        TypeId const i32 = in.primitive(TypeKind::I32);
        TypeId const pV  = in.pointer(in.primitive(TypeKind::Void));
        std::array<TypeId, 1> const lockParams{pV};
        TypeId const lockSig = in.fnSig(lockParams, i32, CallConv::CcMS64);
        MirBuilder mb;
        mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{100});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const slot     = mb.addInst(MirOpcode::Alloca, {}, pV, 40);
        MirInstId const lockAddr = mb.addGlobalAddr(SymbolId{10}, in.pointer(lockSig));
        MirInstId const callOps[] = {lockAddr, slot};
        mb.addReturn(mb.addInst(MirOpcode::Call, callOps, i32));
        Mir mir = std::move(mb).finish();
        LibrarySynthesis const win32{LibrarySynthVehicle::Win32,
                                     RuntimeLibraryRole::SystemPrimitives, "kernel32.dll"};
        std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "mtx_lock"}};
        std::vector<ExternImport> ext;
        DiagnosticReporter rep;
        (void)synthesizeThreadsShim(mir, in, recipes, win32, CSymbolDecorationScheme::None, ext, rep);
    };
    EXPECT_DEATH(shimOverAModuleThatSaysNothing(),
                 "MirBuilder::mintSymbol fatal: this module's symbol-id space was never stated");
}
