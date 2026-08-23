// ── D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR ───────
// ────────────────────────────────────────────────────────────────────────────
//
// THE DEFECT THIS FILE PINS, and it was a LIVE SILENT MISCOMPILE rather than a
// latent one. Argument placement selected its register pool with a TWO-WAY
// rule — `(cls == FPR) ? argFprs : argGprs` — over a register-class vocabulary
// with more than two members, so a **VR-class argument took the ELSE branch
// into the INTEGER pool** and was passed in the wrong register file with no
// diagnostic at all.
//
// ✔MEASURED END TO END at cycle P25, on the shipped arm64 release pipeline, by
// building the two-way rule back in as a mutant and reading the disassembly of
// `s3(a, b, y)` where `a`/`b` are `double` parameters and `y` is a `"w"`
// (VR-class) inline-asm output:
//     mutant:   fmov d0, d15 / fmov d1, d14 / ldur q0, [sp,#24]   <- CLOBBERS a
//     fixed:    fmov d0, d15 / fmov d1, d14 / ldur q2, [sp,#24]   <- NSRN 2
//     gcc -O2:  the third `double` argument in d2
// rc=0 in BOTH DSS arms: the wrong one is the quiet one, which is why the pin
// is here and not left to the corpus.
//
// ★★★ WHY THE PIN IS OVER `ArgCursors` AND NOT ONLY OVER THE ROW TABLE. ✔The
// row table ALONE was measured insufficient: with `argPassingRegister` already
// converted to the rows, rebuilding the reproduction came out BYTE-IDENTICAL,
// because the register an outgoing argument lands in is decided by a CURSOR
// WALK that existed in FIVE hand-kept copies. The mutant above only changes the
// output once the walk is the thing under test.
//
// ★★ WHAT THE PINS ASSERT, and why no single one carries the claim:
//   (A) THE ROWS ARE THE MAP — for EVERY `LirRegClass` enumerator, the class has
//       an arg pool exactly when a row names it, and the pool is the cc member
//       that row names. Read off `kArgPoolRows`, never typed here.
//   (B) THE ABI CORPUS ARM — a SEPARATE claim, written as an ABI fact and not
//       derived from the rows: some shipped target passes VR arguments in a
//       pool that is NOT its integer pool. ★ This exists because of the P23
//       lesson on the return side: a pin whose expectation comes off the same
//       table as the code moves BOTH HALVES OF THE COMPARISON TOGETHER, so
//       deleting the VR row reddened nothing.
//   (C) THE COUNTER IDENTITY IS DERIVED — `argPoolsShareACursor` answers from
//       the target's own `dwarfNumber`s. ✔arm64 fpr×vr share one cursor
//       (AAPCS64's single NSRN across the d-views and the v-views); x86_64
//       shares none. A `hwEncoding`-based derivation would answer TRUE for
//       gpr×vr on arm64 and fpr×gpr on x86_64 — the pin names that too.
//   (D) THE WALK — an interleaved sequence of classes gets the indices AAPCS64
//       requires, which is the property the disassembly above measured.
//   (E) THE THREE REFUSALS ARE THREE FACTS — no-row, undeclared-pool and
//       pool-exhausted never share a diagnostic code.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

using namespace dss;

namespace {

constexpr auto kAllRegClassNames = allNames(kTargetRegClassTable);
constexpr char const* kTargets[] = {"x86_64", "arm64"};

[[nodiscard]] bool rowsName(LirRegClass cls) {
    for (auto const& row : kArgPoolRows) {
        if (row.cls == cls) return true;
    }
    return false;
}

[[nodiscard]] std::string summarize(DiagnosticReporter const& r) {
    std::string s;
    for (auto const& d : r.all()) s += "\n  " + d.actual;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

// The DWARF numbers a class's registers carry on this target. The counter
// identity is derived from these; a pin that re-derived it from `hwEncoding`
// would be re-typing the very mistake (C) exists to forbid.
[[nodiscard]] std::vector<std::uint16_t>
dwarfNumbersOf(TargetSchema const& s, TargetRegClass cls) {
    std::vector<std::uint16_t> out;
    for (auto const& r : s.registers()) {
        if (r.regClass == cls && r.dwarfNumber.has_value())
            out.push_back(*r.dwarfNumber);
    }
    return out;
}

} // namespace

// ── (A) THE ROWS ARE THE MAP ────────────────────────────────────────────────
TEST(LirArgCursorProjection, EveryRegisterClassHasAPoolExactlyWhenARowNamesIt) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        for (std::size_t i = 0; i < kAllRegClassNames.size(); ++i) {
            auto const cls = static_cast<LirRegClass>(i);
            SCOPED_TRACE(std::string{lirRegClassName(cls)});
            EXPECT_EQ(argRegisterPool(*cc, cls) != nullptr, rowsName(cls))
                << "a class with no row must own NO pool — the defect this file "
                   "pins is a class SILENTLY acquiring another class's";
        }
    }
}

// ── (B) THE ABI CORPUS ARM ──────────────────────────────────────────────────
//
// ⚠ THIS ARM IS DELIBERATELY NOT DERIVED FROM `kArgPoolRows`. It states an ABI
// fact — AAPCS64 passes short-vector arguments in the V registers, which are
// not the X registers — so that deleting the VR row cannot move both halves of
// the comparison at once. Without it, the mutant that reintroduces the defect
// is invisible to this file.
TEST(LirArgCursorProjection, SomeShippedTargetPassesVrArgumentsOutsideItsIntegerPool) {
    bool sawPopulatedVrPool = false;
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);
        if (cc->argVrs.empty()) continue;
        sawPopulatedVrPool = true;

        auto const* vrPool = argRegisterPool(*cc, LirRegClass::VR);
        ASSERT_NE(vrPool, nullptr);
        EXPECT_EQ(*vrPool, cc->argVrs)
            << "the VR class must draw from the cc's DECLARED vector arg pool";
        EXPECT_NE(*vrPool, cc->argGprs)
            << "a VR argument in the INTEGER pool is the miscompile: on arm64 "
               "the reproduction emitted `ldur q0` over argument 0";
    }
    ASSERT_TRUE(sawPopulatedVrPool)
        << "no shipped target declares a vector arg pool, so this arm asserted "
           "nothing — a vacuous pass is exactly what it exists to prevent";
}

// ── (C) THE COUNTER IDENTITY IS DERIVED, AND FROM THE RIGHT FIELD ───────────
TEST(LirArgCursorProjection, SharedCursorsComeFromDwarfNumbersNotHardwareEncodings) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        // Two pools share a cursor exactly when their registers are one
        // physical file — which DWARF numbers state and hardware encodings do
        // not. The expectation is computed from the register table here, so a
        // derivation that switched fields fails even though both halves still
        // "come from the config".
        for (std::size_t i = 0; i < kArgPoolRows.size(); ++i) {
            for (std::size_t j = i + 1; j < kArgPoolRows.size(); ++j) {
                auto const a = kArgPoolRows[i].cls;
                auto const b = kArgPoolRows[j].cls;
                SCOPED_TRACE(std::string{lirRegClassName(a)} + " x "
                             + std::string{lirRegClassName(b)});
                auto const* pa = argRegisterPool(*cc, a);
                auto const* pb = argRegisterPool(*cc, b);
                ASSERT_NE(pa, nullptr);
                ASSERT_NE(pb, nullptr);
                if (pa->empty() || pb->empty()) {
                    EXPECT_FALSE(argPoolsShareACursor(**s, *cc, a, b))
                        << "an EMPTY pool cannot be shown to alias anything";
                    continue;
                }
                auto const orda = (*s)->registerByName((*pa)[0]);
                auto const ordb = (*s)->registerByName((*pb)[0]);
                ASSERT_TRUE(orda.has_value());
                ASSERT_TRUE(ordb.has_value());
                auto const da = (*s)->registers()[*orda].dwarfNumber;
                auto const db = (*s)->registers()[*ordb].dwarfNumber;
                bool const expected =
                    da.has_value() && db.has_value() && *da == *db;
                EXPECT_EQ(argPoolsShareACursor(**s, *cc, a, b), expected);
            }
        }
    }
}

// The arm that names the measurement in the header: on arm64 the d-views and
// the v-views ARE one file, and the general registers are NOT — which is the
// AAPCS64 §6.4.2 stage C.1 single-NSRN fact stated as a test rather than as a
// comment. ⚠ Written per-target on purpose: this is an ABI claim about arm64,
// so deriving it from whatever the loader happened to parse would make it
// vacuous the moment the register table lost its DWARF numbers.
TEST(LirArgCursorProjection, Aapcs64SharesOneCounterAcrossTheDviewsAndTheVviews) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConvention(0);
    ASSERT_NE(cc, nullptr);

    EXPECT_TRUE(argPoolsShareACursor(**s, *cc, LirRegClass::FPR, LirRegClass::VR))
        << "AAPCS64 has ONE NSRN across the d-views and the v-views; two "
           "cursors would hand slot k out twice";
    EXPECT_FALSE(argPoolsShareACursor(**s, *cc, LirRegClass::GPR, LirRegClass::VR));
    EXPECT_FALSE(argPoolsShareACursor(**s, *cc, LirRegClass::GPR, LirRegClass::FPR));

    // …and the field that carries it is `dwarfNumber`, not `hwEncoding`.
    // ✔MEASURED: arm64 x0/w0/v0 ALL encode 0, so an hwEncoding-based
    // derivation would make gpr and vr share a cursor — the same wrong answer
    // the two-way rule gave, arrived at from the other direction.
    auto const gprD = dwarfNumbersOf(**s, TargetRegClass::GPR);
    auto const vrD  = dwarfNumbersOf(**s, TargetRegClass::VR);
    auto const fprD = dwarfNumbersOf(**s, TargetRegClass::FPR);
    ASSERT_FALSE(gprD.empty());
    ASSERT_FALSE(vrD.empty());
    ASSERT_EQ(fprD, vrD) << "the d-views and the v-views must carry the SAME "
                            "DWARF numbers — that is what makes them one file";
    for (auto const g : gprD) {
        for (auto const v : vrD) {
            EXPECT_NE(g, v) << "a general register and a vector register share "
                               "a DWARF number, which would make every "
                               "aliasing derivation in this file meaningless";
        }
    }
}

// ── (D) THE WALK ────────────────────────────────────────────────────────────
//
// The property the disassembly measured, as a unit claim: with the d-views and
// the v-views on one cursor, an interleaved argument list gets consecutive
// indices across BOTH classes — so `s3(double, double, w)` puts the third
// argument at NSRN 2, not at integer slot 0.
TEST(LirArgCursorProjection, InterleavedFprAndVrArgumentsAdvanceOneSharedCursor) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ArgCursors cursors{**s, *cc};

    auto const a = cursors.next(LirRegClass::FPR);
    auto const b = cursors.next(LirRegClass::FPR);
    auto const c = cursors.next(LirRegClass::VR);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(a->index, 0u);
    EXPECT_EQ(b->index, 1u);
    EXPECT_EQ(c->index, 2u)
        << "the VR argument restarted a counter of its own — the shape that "
           "emitted `ldur q0` over an already-placed argument";

    // The integer cursor is INDEPENDENT of it (NGRN vs NSRN).
    auto const g = cursors.next(LirRegClass::GPR);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->index, 0u);

    // A class with no arg pool is NOT silently filed into one.
    EXPECT_FALSE(cursors.next(LirRegClass::None).has_value());

    // The exhaust clamp names a CLASS and reaches the shared cursor.
    ArgCursors clamped{**s, *cc};
    clamped.exhaust(LirRegClass::FPR);
    auto const after = clamped.next(LirRegClass::VR);
    ASSERT_TRUE(after.has_value());
    EXPECT_GE(after->index, after->poolSize)
        << "exhausting the d-views must exhaust the v-views — they are one "
           "register file, so a later vector argument stacks too";
}

// A slot-aligned cc collapses every class onto ONE positional cursor. Read off
// `cc.slotAligned` rather than off the target's name, so this arm follows
// whichever shipped cc declares it.
TEST(LirArgCursorProjection, SlotAlignedCcGivesEveryClassOnePositionalCursor) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    std::size_t sawSlotAligned = 0;
    for (std::size_t i = 0; i < (*s)->callingConventionCount(); ++i) {
        auto const* cc = (*s)->callingConvention(static_cast<std::uint32_t>(i));
        ASSERT_NE(cc, nullptr);
        if (!cc->slotAligned) continue;
        ++sawSlotAligned;
        ArgCursors cursors{**s, *cc};
        auto const a = cursors.next(LirRegClass::GPR);
        auto const b = cursors.next(LirRegClass::FPR);
        auto const c = cursors.next(LirRegClass::GPR);
        ASSERT_TRUE(a.has_value());
        ASSERT_TRUE(b.has_value());
        ASSERT_TRUE(c.has_value());
        EXPECT_EQ(a->index, 0u);
        EXPECT_EQ(b->index, 1u) << "arg k takes slot k whatever its class";
        EXPECT_EQ(c->index, 2u);
        EXPECT_EQ(a->poolSize, b->poolSize);
    }
    ASSERT_GT(sawSlotAligned, 0u)
        << "no shipped x86_64 cc declares slotAligned, so this arm asserted "
           "nothing";
}

// ── (E) THE THREE REFUSALS ARE THREE FACTS ─────────────────────────────────
//
// ⚠ A refusal that misattributes sends the reader to the one place the defect
// is not. "This class has no pool", "this cc declares none of them" and "the
// pool ran out" are three different repairs, so they are three different codes.
TEST(LirArgCursorProjection, TheThreeArgPoolRefusalsNeverShareACode) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->argVrs.empty())
        << "this arm needs a class whose row exists and whose pool is empty; "
           "x86_64 declaring vector arg registers would make it vacuous";

    DiagnosticReporter noRow;
    EXPECT_FALSE(argPassingRegister(**s, *cc, 0, LirRegClass::Flags,
                                    "argCursorProjection", noRow).has_value());
    ASSERT_EQ(noRow.all().size(), 1u) << summarize(noRow);
    EXPECT_EQ(noRow.all()[0].code, DiagnosticCode::L_ArgClassHasNoRegisterPool);

    DiagnosticReporter undeclared;
    EXPECT_FALSE(argPassingRegister(**s, *cc, 0, LirRegClass::VR,
                                    "argCursorProjection", undeclared).has_value());
    ASSERT_EQ(undeclared.all().size(), 1u) << summarize(undeclared);
    EXPECT_EQ(undeclared.all()[0].code, DiagnosticCode::L_ArgClassPoolUndeclared)
        << "an EMPTY pool is what the target DECLARED; reporting it as stack "
           "passing names a full pool that could be absorbed, which is the "
           "wrong repair";

    DiagnosticReporter exhausted;
    EXPECT_FALSE(argPassingRegister(**s, *cc,
                                    static_cast<std::uint32_t>(cc->argGprs.size()),
                                    LirRegClass::GPR,
                                    "argCursorProjection", exhausted).has_value());
    ASSERT_EQ(exhausted.all().size(), 1u) << summarize(exhausted);
    EXPECT_EQ(exhausted.all()[0].code, DiagnosticCode::L_StackPassedArgUnsupported);

    EXPECT_NE(DiagnosticCode::L_ArgClassHasNoRegisterPool,
              DiagnosticCode::L_ArgClassPoolUndeclared);
}

// And the accepted path still answers: index k of a populated pool is the
// register the cc NAMES at k, so acceptance and the pool cannot part company.
TEST(LirArgCursorProjection, EachRowResolvesTheRegisterTheCcNames) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        std::size_t populatedRows = 0;
        for (auto const& row : kArgPoolRows) {
            auto const* pool = argRegisterPool(*cc, row.cls);
            ASSERT_NE(pool, nullptr);
            if (pool->empty()) continue;
            ++populatedRows;
            DiagnosticReporter r;
            auto const got = argPassingRegister(**s, *cc, 0, row.cls,
                                                "argCursorProjection", r);
            ASSERT_TRUE(got.has_value()) << summarize(r);
            auto const want = (*s)->registerByName((*pool)[0]);
            ASSERT_TRUE(want.has_value());
            EXPECT_EQ(got->id, *want);
            EXPECT_EQ(got->regClass(), row.cls);
        }
        EXPECT_GT(populatedRows, 0u)
            << "this target populated no arg pool at all, so the loop asserted "
               "nothing";
    }
}
