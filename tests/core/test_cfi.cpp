// Call Frame Information — the shared PC-keyed unwind representation.
//
// Plan 15 CFI slice / D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO.
// These pin the SEMANTICS every format writer depends on: what the state is
// at a given PC, how the state stack behaves, and which malformed streams
// are refused rather than silently mis-encoded.

#include "core/types/cfi.hpp"

#include <gtest/gtest.h>

using namespace dss;

namespace {

// The x86_64 SysV entry state, in DSS ordinals: CFA = rsp(4) + 8 (the CALL
// pushed the return address), return address at CFA-8.
CfiInitialState sysvEntry() {
    CfiInitialState s;
    s.cfaRegister = 4;
    s.cfaOffset   = 8;
    s.returnAddressAtCfaOffset = -8;
    return s;
}

// A DSS x86_64 frame: `sub rsp,0x20` ending at 7, two callee-save stores
// ending at 15 and 23, then the mirrored teardown ending at 40 and 44.
CfiFunction twoSaveFrame() {
    CfiFunction f;
    f.codeLength    = 48;
    f.initial       = sysvEntry();
    f.prologueEndPc = 23;
    f.ops = {
        CfiOp{7,  CfiOpKind::DefCfaOffset,     CfiRegRef{},             CfiRegRef{},  40},
        CfiOp{15, CfiOpKind::RegAtCfaOffset,   CfiRegRef::physical(14), CfiRegRef{}, -40},
        CfiOp{23, CfiOpKind::RegAtCfaOffset,   CfiRegRef::physical(15), CfiRegRef{}, -24},
        CfiOp{32, CfiOpKind::RememberState,    CfiRegRef{},             CfiRegRef{},   0},
        CfiOp{32, CfiOpKind::RegRestoreInitial,CfiRegRef::physical(14), CfiRegRef{},   0},
        CfiOp{40, CfiOpKind::RegRestoreInitial,CfiRegRef::physical(15), CfiRegRef{},   0},
        CfiOp{44, CfiOpKind::DefCfaOffset,     CfiRegRef{},             CfiRegRef{},   8},
        CfiOp{45, CfiOpKind::RestoreState,     CfiRegRef{},             CfiRegRef{},   0},
    };
    return f;
}

} // namespace

TEST(Cfi, EntryStateComesFromTheCallingConventionAlone) {
    // The entry row is DERIVED, not declared: `cfaOffset` is the calling
    // convention's `callPushBytes` and the return address sits that far below
    // the CFA. Nothing here needs new target configuration.
    auto const s = cfiEntryState(sysvEntry());
    EXPECT_EQ(s.cfaRegister, 4u);
    EXPECT_EQ(s.cfaOffset, 8);
    auto const* ra = s.rule(CfiRegRef::returnAddress());
    ASSERT_NE(ra, nullptr) << "the return address must have an entry rule";
    EXPECT_EQ(ra->kind, CfiOpKind::RegAtCfaOffset);
    EXPECT_EQ(ra->offset, -8);
}

TEST(Cfi, LinkRegisterAbiEntryStateHasNoStackSlotForTheReturnAddress) {
    // AAPCS64: the CALL pushes nothing (callPushBytes == 0) and the return
    // address is in x30. The entry rule is `same_value`, not a stack slot —
    // a producer that invented `CFA-0` here would tell an unwinder to load
    // the return address from the caller's frame.
    CfiInitialState init;
    init.cfaRegister = 31;                 // sp
    init.cfaOffset   = 0;
    init.returnAddressRegister = 30;       // x30 / LR
    auto const s = cfiEntryState(init);
    EXPECT_EQ(s.cfaOffset, 0);
    auto const* ra = s.rule(CfiRegRef::returnAddress());
    ASSERT_NE(ra, nullptr);
    EXPECT_EQ(ra->kind, CfiOpKind::RegSameValue);
    EXPECT_EQ(ra->srcReg.ordinal, 30u);
}

TEST(Cfi, FoldGivesTheStateAtEachPcAcrossThePrologue) {
    // ★ The property a frame SHAPE cannot express: the answer CHANGES with PC.
    auto const f = twoSaveFrame();

    // Before the sub retires, the frame does not exist yet.
    auto s0 = foldCfiOps(f, 0);
    ASSERT_TRUE(s0.has_value());
    EXPECT_EQ(s0->cfaOffset, 8);
    EXPECT_EQ(s0->rule(CfiRegRef::physical(14)), nullptr)
        << "r14 has no save rule before its store executes";

    // After the sub, the CFA has moved but no register is saved yet.
    auto s7 = foldCfiOps(f, 7);
    ASSERT_TRUE(s7.has_value());
    EXPECT_EQ(s7->cfaOffset, 40);
    EXPECT_EQ(s7->rule(CfiRegRef::physical(14)), nullptr);

    // After the first store, exactly one register is described.
    auto s15 = foldCfiOps(f, 15);
    ASSERT_TRUE(s15.has_value());
    ASSERT_NE(s15->rule(CfiRegRef::physical(14)), nullptr);
    EXPECT_EQ(s15->rule(CfiRegRef::physical(14))->offset, -40);
    EXPECT_EQ(s15->rule(CfiRegRef::physical(15)), nullptr);

    // Post-prologue: both.
    auto s23 = foldCfiOps(f, 23);
    ASSERT_TRUE(s23.has_value());
    ASSERT_NE(s23->rule(CfiRegRef::physical(15)), nullptr);
    EXPECT_EQ(s23->rule(CfiRegRef::physical(15))->offset, -24);
}

TEST(Cfi, EpilogueTearsTheFrameDownAndRestoreStateArmsItAgain) {
    // ★ The whole reason the epilogue is described. Sampling a PC after
    // `add rsp,N` must report the ENTRY canonical frame address, or an
    // unwinder reads the caller's return address `totalFrameSize` bytes away
    // from where it is. And the code AFTER a `ret` (a second epilogue's
    // block) still has a live frame — which is what `restore_state` re-arms.
    auto const f = twoSaveFrame();

    auto const mid = foldCfiOps(f, 44);       // after `add rsp,0x20`
    ASSERT_TRUE(mid.has_value());
    EXPECT_EQ(mid->cfaOffset, 8) << "the frame is gone at this PC";
    ASSERT_NE(mid->rule(CfiRegRef::physical(14)), nullptr);
    EXPECT_EQ(mid->rule(CfiRegRef::physical(14))->kind, CfiOpKind::RegUndefined)
        << "restore reverts to the ENTRY rule. A callee-save has no entry rule "
           "(the CIE says nothing about it), so the entry rule IS 'undefined' "
           "-- and saying so is the point: leaving the stale save rule in place "
           "would tell an unwinder to reload r14 from a stack slot the epilogue "
           "has already released";

    auto const after = foldCfiOps(f, 46);     // past the `ret`
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->cfaOffset, 40)
        << "restore_state must re-arm the framed rules for the code that "
           "follows the return — otherwise every unwind from the second half "
           "of a two-return function is wrong";
}

TEST(Cfi, RestoreInitialRevertsToTheEntryRowNotToUndefinedGuesswork) {
    CfiFunction f;
    f.codeLength = 16;
    f.initial    = sysvEntry();
    // A function that clobbers and then restores the RETURN ADDRESS column.
    f.ops = {
        CfiOp{4, CfiOpKind::RegAtCfaOffset,    CfiRegRef::returnAddress(),
              CfiRegRef{}, -64},
        CfiOp{8, CfiOpKind::RegRestoreInitial, CfiRegRef::returnAddress(),
              CfiRegRef{}, 0},
    };
    auto const s4 = foldCfiOps(f, 4);
    ASSERT_TRUE(s4.has_value());
    EXPECT_EQ(s4->rule(CfiRegRef::returnAddress())->offset, -64);
    auto const s8 = foldCfiOps(f, 8);
    ASSERT_TRUE(s8.has_value());
    EXPECT_EQ(s8->rule(CfiRegRef::returnAddress())->offset, -8)
        << "restore returns to the CIE's entry rule (-8), not to the last "
           "explicit rule and not to 'undefined'";
}

TEST(Cfi, UnbalancedRestoreStateIsRefusedRatherThanTreatedAsANoOp) {
    // ★ Fail loud. A `restore_state` with nothing remembered means the
    // producer's own model of the frame is wrong; treating it as a no-op
    // emits a table describing a frame that never existed.
    CfiFunction f;
    f.codeLength = 8;
    f.initial    = sysvEntry();
    f.ops = { CfiOp{4, CfiOpKind::RestoreState, CfiRegRef{}, CfiRegRef{}, 0} };
    EXPECT_FALSE(foldCfiOps(f, 8).has_value());
    EXPECT_NE(validateCfiFunction(f).find("no matching remember_state"),
              std::string::npos)
        << "the diagnostic must name the defect, not just report failure";
}

TEST(Cfi, ValidationRejectsAnOutOfOrderOrOutOfRangeStream) {
    // DWARF's advance_loc is a forward-only delta and Win64's unwind codes are
    // a descending scan: an out-of-order stream silently produces a wrong
    // table in BOTH encodings, so it is refused in the representation.
    CfiFunction f;
    f.codeLength = 32;
    f.initial    = sysvEntry();
    f.ops = {
        CfiOp{16, CfiOpKind::DefCfaOffset, CfiRegRef{}, CfiRegRef{}, 40},
        CfiOp{8,  CfiOpKind::DefCfaOffset, CfiRegRef{}, CfiRegRef{}, 48},
    };
    EXPECT_NE(validateCfiFunction(f).find("non-decreasing pc"),
              std::string::npos);

    CfiFunction g;
    g.codeLength = 8;
    g.initial    = sysvEntry();
    g.ops = { CfiOp{99, CfiOpKind::DefCfaOffset, CfiRegRef{}, CfiRegRef{}, 40} };
    EXPECT_NE(validateCfiFunction(g).find("past the function"),
              std::string::npos);

    // A CFA base rule naming the return-address COLUMN is a producer bug that
    // would otherwise encode as a plausible-looking wrong table.
    CfiFunction h;
    h.codeLength = 8;
    h.initial    = sysvEntry();
    h.ops = { CfiOp{4, CfiOpKind::DefCfaRegister, CfiRegRef::returnAddress(),
                    CfiRegRef{}, 0} };
    EXPECT_NE(validateCfiFunction(h).find("return-address column"),
              std::string::npos);

    EXPECT_TRUE(validateCfiFunction(twoSaveFrame()).empty())
        << "a well-formed stream must validate clean";
}

TEST(Cfi, TheOpVocabularyIsNamedSoARefusalCanPrintTheRuleItCannotEncode) {
    // A format walker printing "unsupported unwind opcode 5" has told the
    // reader nothing. Every enumerator has a name, and the two predicates
    // that classify them are defined as complements rather than by
    // re-enumerating the enum at each call site.
    EXPECT_EQ(cfiOpKindName(CfiOpKind::RegAtCfaOffset), "offset");
    EXPECT_EQ(cfiOpKindName(CfiOpKind::RememberState), "remember_state");
    for (std::size_t i = 0; i < kCfiOpKindCount; ++i) {
        auto const k = static_cast<CfiOpKind>(i);
        EXPECT_NE(cfiOpKindName(k), "<unknown-cfi-op>")
            << "op " << i << " has no name — a refusal naming it would print "
               "a placeholder";
        // Exactly one of the three classes: a CFA rule, a register rule, or
        // the state stack.
        bool const stack = (k == CfiOpKind::RememberState
                            || k == CfiOpKind::RestoreState);
        int const classes = static_cast<int>(cfiOpTouchesCfa(k))
                          + static_cast<int>(cfiOpTouchesRegRule(k))
                          + static_cast<int>(stack);
        EXPECT_EQ(classes, 1) << "op " << i << " is in " << classes
                              << " classes; the classification must partition";
    }
}
