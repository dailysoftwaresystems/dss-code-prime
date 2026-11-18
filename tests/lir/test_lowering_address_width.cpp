// P69 fold 3f (lane `xa`, review-xa3e F3 -- the pointer width's Lowerer half): EVERY ADDRESS THE MIR->LIR LOWERING
// WRITES IS AN ADDRESS OF THE MODULE'S DATA MODEL, READ ONCE, AND THE WALL REFUSES A MODEL THE BACK END CANNOT LOWER.
//
// The module states the data model it is lowered for (`MirModuleFacts::dataModel`). `lowerToLir` is the WALL: a
// module stating none, a model whose pointer is not the back end's address width (the admissibility predicate,
// `dataModelTheBackEndCannotLower`) and a value no table row names are each refused by name, with the semantic
// gate's code (S_UnsupportedDataModel) and the predicate's reason, before anything is lowered. Past it
// (`lowerToLirPastTheWall`, the seam: under the one width the wall admits a literal 8 and a read of the model agree)
// every Q1 form reads the model: a dense switch's table slot (`JumpTableDescriptor::slotBytes`), the index scale
// that addresses it and the slot's load, and a pointer-class value's load. An ILP32 module lowers each at 4 bytes,
// an LP64 or LLP64 one at 8 -- on both shipped CPUs.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "first_convention.hpp"   // dss::test::kFirstConventionOfTheTarget
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "program/compile_pipeline.hpp"   // materializeJumpTable, the jump table's one writer

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

constexpr std::size_t kCases = 10;   // >= kJumpTableMinCases, dense 0..9: the switch takes the table

// `void f(int d) { switch (d) { case 0..9: return; default: return; } }`, stating `model`.
[[nodiscard]] Mir denseSwitch(TypeInterner& interner, DataModel model) {
    TypeId const intT  = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    std::array<TypeId, 1> params{intT};
    TypeId const sig = interner.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();   // a hand-built module is its own table
    mb.stateArgCounting(MirArgCounting::PerClass);
    mb.stateDataModel(model);
    mb.addFunction(sig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::SwitchHead);
    std::array<MirBlockId, kCases> caseBlocks{};
    for (auto& cb : caseBlocks) cb = mb.createBlock(StructCfMarker::SwitchCase);
    MirBlockId const dflt = mb.createBlock(StructCfMarker::SwitchCase);
    mb.beginBlock(entry);
    MirInstId const disc = mb.addArg(0, intT);
    std::array<std::pair<MirInstId, MirBlockId>, kCases> cases{};
    for (std::size_t i = 0; i < kCases; ++i) {
        MirLiteralValue lv;
        lv.value = static_cast<std::int64_t>(i);
        lv.core  = TypeKind::I32;
        cases[i] = {mb.addConst(lv, intT), caseBlocks[i]};
    }
    mb.addSwitch(disc, cases, dflt);
    for (auto const cb : caseBlocks) { mb.beginBlock(cb); mb.addReturn(); }
    mb.beginBlock(dflt);
    mb.addReturn();
    return std::move(mb).finish();
}

// `void *f(void **p) { return *p; }`, stating `model`: a pointer-class value loaded and returned.
[[nodiscard]] Mir pointerLoad(TypeInterner& interner, DataModel model) {
    TypeId const voidT  = interner.primitive(TypeKind::Void);
    TypeId const ptrT   = interner.pointer(voidT);
    TypeId const ptrPtr = interner.pointer(ptrT);
    std::array<TypeId, 1> params{ptrPtr};
    TypeId const sig = interner.fnSig(params, ptrT, CallConv::CcSysV);
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();
    mb.stateArgCounting(MirArgCounting::PerClass);
    mb.stateDataModel(model);
    mb.addFunction(sig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const p = mb.addArg(0, ptrPtr);
    std::array<MirInstId, 1> loadOps{p};
    MirInstId const v = mb.addInst(MirOpcode::Load, loadOps, ptrT);
    mb.addReturn(v);
    return std::move(mb).finish();
}

// Every instruction of the module's first function with `opcode`, by its flags.
[[nodiscard]] std::multiset<std::uint8_t> flagsOf(Lir const& lir, std::uint16_t opcode) {
    std::multiset<std::uint8_t> out;
    LirFuncId const f = lir.funcAt(0);
    for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
        LirBlockId const b = lir.funcBlockAt(f, bi);
        for (std::uint32_t i = 0; i < lir.blockInstCount(b); ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            if (lir.instOpcode(inst) == opcode) out.insert(lir.instFlags(inst));
        }
    }
    return out;
}

// The dense switch's INDEX SCALE: the immediate that defines the multiplier of the first function's one `mul` (the
// lowering scales the table index by a fresh constant, `mul index, scale`). nullopt unless the function holds exactly
// one `mul` whose multiplier one immediate defines. Read at the multiplier itself: the case values 0..9 are
// immediates of their own, both widths among them.
[[nodiscard]] std::optional<std::int32_t> indexScaleOf(Lir const& lir, std::uint16_t mulOpcode) {
    std::vector<std::pair<LirReg, std::int32_t>> immediateDefs;   // a register one immediate defines
    std::vector<LirReg> multipliers;
    LirFuncId const f = lir.funcAt(0);
    for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
        LirBlockId const b = lir.funcBlockAt(f, bi);
        for (std::uint32_t i = 0; i < lir.blockInstCount(b); ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            auto const ops = lir.instOperands(inst);
            if (lir.instOpcode(inst) == mulOpcode) {
                if (ops.size() != 2 || ops[1].kind != LirOperandKind::Reg) return std::nullopt;
                multipliers.push_back(ops[1].reg);
            } else if (ops.size() == 1 && ops[0].kind == LirOperandKind::ImmInt) {
                immediateDefs.emplace_back(lir.instResult(inst), ops[0].immInt32);
            }
        }
    }
    if (multipliers.size() != 1) return std::nullopt;
    for (auto const& [reg, value] : immediateDefs)
        if (reg == multipliers.front()) return value;
    return std::nullopt;
}

[[nodiscard]] std::string everyDiagnostic(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

}  // namespace

TEST(LoweringAddressWidth, TheWallRefusesAModuleStatingNoModelOrOneTheBackEndCannotLower) {
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // Admitted: each model whose pointer is the back end's address width.
    for (DataModel const model : {DataModel::Lp64, DataModel::Llp64}) {
        TypeInterner interner{CompilationUnitId{1}};
        Mir const m = denseSwitch(interner, model);
        DiagnosticReporter rep;
        MirToLirResult const r = lowerToLir(m, **target, interner, rep, test::kFirstConventionOfTheTarget);
        EXPECT_TRUE(r.ok) << dataModelSpelled(model) << everyDiagnostic(rep);
    }
    // Refused, each by name and with the reason the semantic gate gives: a model of 4-byte pointers, a value no
    // table row names (it states no pointer size), and a module stating none.
    struct Refused {
        DataModel   model;
        std::string reason;
    };
    for (Refused const& c : {Refused{DataModel::Ilp32, *dataModelTheBackEndCannotLower(DataModel::Ilp32)},
                             Refused{static_cast<DataModel>(7),
                                     *dataModelTheBackEndCannotLower(static_cast<DataModel>(7))},
                             Refused{DataModel{}, "states no data model"}}) {
        TypeInterner interner{CompilationUnitId{1}};
        Mir const m = denseSwitch(interner, c.model);
        DiagnosticReporter rep;
        MirToLirResult const r = lowerToLir(m, **target, interner, rep, test::kFirstConventionOfTheTarget);
        EXPECT_FALSE(r.ok) << "the wall lowered " << dataModelSpelled(c.model);
        std::size_t named = 0;
        for (auto const& d : rep.all())
            if (d.code == DiagnosticCode::S_UnsupportedDataModel && d.actual.find(c.reason) != std::string::npos) ++named;
        EXPECT_EQ(named, 1u) << "the wall's refusal of " << dataModelSpelled(c.model) << everyDiagnostic(rep);
    }
}

TEST(LoweringAddressWidth, PastTheWallEveryAddressIsTheModelsWidth) {
    for (char const* cpu : {"x86_64", "arm64"}) {
        auto const target = TargetSchema::loadShipped(cpu);
        ASSERT_TRUE(target.has_value()) << cpu;
        auto const loadOp = (*target)->opcodeByMnemonic("load");
        ASSERT_TRUE(loadOp.has_value()) << cpu;
        auto const mulOp = (*target)->opcodeByMnemonic("mul");
        ASSERT_TRUE(mulOp.has_value()) << cpu;
        for (auto const& [model, width] : {std::pair{DataModel::Ilp32, 4u}, std::pair{DataModel::Lp64, 8u},
                                           std::pair{DataModel::Llp64, 8u}}) {
            SCOPED_TRACE(std::string{cpu} + ", " + dataModelSpelled(model));
            std::uint8_t const widthFlags = *lirInstWidthFlagForBits(8 * width);
            // The dense switch: its table's slot, the index scale that addresses it, and the slot's load.
            {
                TypeInterner interner{CompilationUnitId{1}};
                Mir const m = denseSwitch(interner, model);
                DiagnosticReporter rep;
                MirToLirResult const r =
                    lowerToLirPastTheWall(m, **target, interner, rep, test::kFirstConventionOfTheTarget);
                ASSERT_TRUE(r.ok) << everyDiagnostic(rep);
                ASSERT_EQ(r.jumpTableDescriptors.size(), 1u);
                EXPECT_EQ(r.jumpTableDescriptors[0].slotBytes, width) << "the table's slot width";
                EXPECT_EQ(indexScaleOf(r.lir, *mulOp), std::optional<std::int32_t>{static_cast<std::int32_t>(width)})
                    << "the index scale";
                std::multiset<std::uint8_t> const loads = flagsOf(r.lir, *loadOp);
                EXPECT_EQ(loads.count(widthFlags), 1u) << "the slot's load at the model's width";
            }
            // A pointer-class value's load.
            {
                TypeInterner interner{CompilationUnitId{1}};
                Mir const m = pointerLoad(interner, model);
                DiagnosticReporter rep;
                MirToLirResult const r =
                    lowerToLirPastTheWall(m, **target, interner, rep, test::kFirstConventionOfTheTarget);
                ASSERT_TRUE(r.ok) << everyDiagnostic(rep);
                std::multiset<std::uint8_t> const loads = flagsOf(r.lir, *loadOp);
                ASSERT_EQ(loads.size(), 1u);
                EXPECT_EQ(*loads.begin(), widthFlags) << "the pointer's load at the model's width";
            }
        }
    }
}

TEST(LoweringAddressWidth, TheJumpTableIsWrittenAtItsDescriptorsSlotWidth) {
    // F3-R1: the table's ONE writer (`materializeJumpTable`) reads the slot width the lowering scaled the index by
    // -- the descriptor's -- for the table's size, its alignment, each slot's offset and its relocation's width. A
    // descriptor stating none is refused by name.
    for (char const* cpu : {"x86_64", "arm64"}) {
        auto const target = TargetSchema::loadShipped(cpu);
        ASSERT_TRUE(target.has_value()) << cpu;
        for (std::size_t const width : {std::size_t{4}, std::size_t{8}}) {
            SCOPED_TRACE(std::string{cpu} + ", " + std::to_string(width) + "-byte slots");
            JumpTableDescriptor desc;
            desc.tableSymbol  = SymbolId{9};
            desc.slotCount    = 3;
            desc.slotBytes    = width;
            desc.slotBindings = {{0u, 0u}, {1u, 1u}, {0u, 2u}};   // a gap's slot names the default block again
            desc.blockSymbols = {{0u, SymbolId{10}}, {1u, SymbolId{11}}};
            AssembledFunction owner;
            owner.blockByteOffsets = {{0u, 16u}, {1u, 32u}};
            DiagnosticReporter rep;
            std::optional<AssembledData> const table = materializeJumpTable(desc, owner, **target, rep);
            ASSERT_TRUE(table.has_value()) << everyDiagnostic(rep);
            EXPECT_EQ(table->bytes.size(), 3 * width) << "the table's size is not its slots' width";
            EXPECT_EQ(table->alignment.bytes(), width) << "the table's alignment is not its slots' width";
            auto const kind = addressSlotRelocationKind((*target)->relocations(), static_cast<std::uint8_t>(width));
            ASSERT_TRUE(kind.has_value());
            ASSERT_EQ(table->relocations.size(), 3u);
            for (std::size_t j = 0; j < 3; ++j) {
                EXPECT_EQ(table->relocations[j].offset, j * width) << "slot " << j;
                EXPECT_EQ(table->relocations[j].kind, *kind) << "slot " << j;
            }
            EXPECT_EQ(owner.blockSymbols.size(), 2u) << "each block's symbol bound once";
        }
        JumpTableDescriptor none;
        none.tableSymbol = SymbolId{9};
        none.slotCount   = 1;
        AssembledFunction owner;
        DiagnosticReporter rep;
        EXPECT_FALSE(materializeJumpTable(none, owner, **target, rep).has_value()) << cpu;
        EXPECT_TRUE(rep.hasErrors()) << cpu;
    }
}

TEST(LoweringAddressWidth, PastTheWallAModelStatingNoPointerSizeIsStillRefused) {
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    for (DataModel const model : {DataModel{}, static_cast<DataModel>(7)}) {
        TypeInterner interner{CompilationUnitId{1}};
        Mir const m = denseSwitch(interner, model);
        DiagnosticReporter rep;
        MirToLirResult const r =
            lowerToLirPastTheWall(m, **target, interner, rep, test::kFirstConventionOfTheTarget);
        EXPECT_FALSE(r.ok) << dataModelSpelled(model);
        std::size_t refused = 0;
        for (auto const& d : rep.all()) refused += d.code == DiagnosticCode::S_UnsupportedDataModel ? 1u : 0u;
        EXPECT_EQ(refused, 1u) << everyDiagnostic(rep);
    }
}
