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
// ★★★ WHAT R1 OF DESIGN A′ CHANGED HERE, AND IT CHANGED THE WITNESS RATHER
// THAN THE DEFECT. arm64 now declares its SIMD&FP file ONCE — `v0..v31` are
// class `fpr` and `q`/`d`/`s`/`h`/`b` are `subOf` views of them — so there is
// no `vr`-class register on any shipped target, `argVrs` is deleted, and
// `kArgPoolRows` is two rows (GPR, FPR) rather than three.
//
// ⇒ A VR-CLASS ARGUMENT IS NOW A CLASS WITH NO ROW, WHICH MAKES THIS FILE'S
// SUBJECT SHARPER, NOT WEAKER. The original defect was a VR argument silently
// taking the `else` branch into the INTEGER pool. Today the correct answer for
// that class is a REFUSAL BY NAME, and `next(LirRegClass::VR)` returning
// nullopt (arm (D)) is a direct pin of exactly that: the wrong file is still
// the wrong answer, and it is still never reached by falling through.
//
// ★★ WHAT THE PINS ASSERT, and why no single one carries the claim:
//   (A) THE ROWS ARE THE MAP — for EVERY `LirRegClass` enumerator, the class has
//       an arg pool exactly when a row names it, and the pool is the cc member
//       that row names. Read off `kArgPoolRows`, never typed here.
//   (B) THE ABI CORPUS ARM — a SEPARATE claim, written as an ABI fact and not
//       derived from the rows: a shipped target passes floating/SIMD arguments
//       in a pool that is NOT its integer pool, and on arm64 that pool names
//       the V registers. ★ This exists because of the P23 lesson on the return
//       side: a pin whose expectation comes off the same table as the code
//       moves BOTH HALVES OF THE COMPARISON TOGETHER, so deleting the row
//       reddened nothing. ⚠ It used to say "VR arguments"; that spelling died
//       with the second class, the ABI fact under it did not.
//   (C) THE COUNTER IDENTITY IS DERIVED — `argPoolsShareACursor` answers from
//       the target's own `dwarfNumber`s, never from `hwEncoding`. ⚠ ITS
//       POSITIVE DIRECTION HAS NO WITNESS ANY MORE and the arm says so out
//       loud rather than quietly asserting less; what stays pinned is the
//       wrong-field trap, which arm64 still exhibits (x0 and v0 both encode 0
//       while carrying different DWARF numbers).
//   (D) THE WALK — a sequence of arguments gets the indices AAPCS64 requires,
//       the integer cursor stays independent of the FP one, and a class with
//       no pool is refused rather than filed into a neighbour's.
//   (E) THE THREE REFUSALS ARE THREE FACTS — no-row, undeclared-pool and
//       pool-exhausted never share a diagnostic code.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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
// fact — AAPCS64 passes floating-point and short-vector arguments in the V
// registers, which are not the X registers — so that deleting the FPR row
// cannot move both halves of the comparison at once. Without it, the mutant
// that reintroduces the defect is invisible to this file.
//
// ⚠⚠ WHAT THIS ARM USED TO READ, AND WHY ITS SPELLING HAD TO MOVE. It was
// `SomeShippedTargetPassesVrArgumentsOutsideItsIntegerPool`, and it walked
// `cc->argVrs` — the SECOND arg pool arm64 declared over the same physical
// SIMD&FP file `argFprs` named. R1 of design A′ deleted that pool along with
// the second declaration, so a `cc->argVrs.empty()` guard would now skip every
// target and the arm's own non-vacuity `ASSERT` would fire. The ABI fact it
// stated is untouched: those registers are still the V registers and still are
// not the X registers. It is now stated over the ONE pool that names them.
TEST(LirArgCursorProjection, AShippedTargetPassesFpArgumentsOutsideItsIntegerPool) {
    bool sawArm64VRegisterPool = false;
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);
        ASSERT_FALSE(cc->argFprs.empty())
            << "every shipped target passes floating arguments in registers; "
               "an empty pool here would make the comparison below vacuous";

        auto const* fpPool = argRegisterPool(*cc, LirRegClass::FPR);
        ASSERT_NE(fpPool, nullptr);
        EXPECT_EQ(*fpPool, cc->argFprs)
            << "the FP class must draw from the cc's DECLARED floating arg "
               "pool";
        EXPECT_NE(*fpPool, cc->argGprs)
            << "a floating argument in the INTEGER pool is the miscompile: on "
               "arm64 the reproduction emitted `ldur q0` over argument 0";
    }

    // The per-target half, written as an ABI claim about AArch64 rather than
    // read back off whatever the loader parsed: AAPCS64 §5.4 names v0..v7.
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm.has_value());
    auto const* aapcs64 = (*arm)->callingConvention(0);
    ASSERT_NE(aapcs64, nullptr);
    for (std::size_t k = 0; k < aapcs64->argFprs.size(); ++k) {
        EXPECT_EQ(aapcs64->argFprs[k], "v" + std::to_string(k));
        sawArm64VRegisterPool = true;
    }
    ASSERT_TRUE(sawArm64VRegisterPool)
        << "arm64 declares no floating arg registers at all, so this arm "
           "asserted nothing — a vacuous pass is exactly what it prevents";
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

// The arm that names the measurement in the header: AAPCS64's NSRN is ONE
// cursor over the SIMD&FP file and the NGRN is a separate one over the general
// registers, and the field that decides which is `dwarfNumber`, not
// `hwEncoding`. ⚠ Written per-target on purpose: this is an ABI claim about
// arm64, so deriving it from whatever the loader happened to parse would make
// it vacuous the moment the register table lost its DWARF numbers.
//
// ⚠⚠ THE POSITIVE DIRECTION OF `argPoolsShareACursor` HAS NO WITNESS ON ANY
// LOADABLE TARGET, AND SAYING SO IS THE HONEST FORM OF THIS ARM. It used to
// have one: this test was `Aapcs64SharesOneCounterAcrossTheDviewsAndTheVviews`
// and asserted `argPoolsShareACursor(FPR, VR)` TRUE, because arm64 declared
// `argFprs` = d0..d7 (class fpr) and `argVrs` = v0..v7 (class vr) — two pools
// over ONE physical file, sharing DWARF 64..71, related by a cursor derived
// after the fact. R1 of design A′ declared that file once, so there is ONE FP
// arg pool and nothing to relate; ✔RE-MEASURED, the derivation is inert on
// both shipped targets.
//
// ★ AND IT CANNOT BE PINNED BY A MUTANT EITHER, WHICH IS WHY THERE IS NO
// SYNTHESIZED POSITIVE HERE. The only two arg pools left are `argGprs` and
// `argFprs`, and both are members of `kAllocatablePoolLists`; giving their
// registers a shared `dwarfNumber` is exactly the cross-class shape
// D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE refuses at
// LOAD, so no such target can be constructed to hand to this function. The
// derivation is kept because the shape is describable and assuming independent
// cursors hands slot k out twice; what is pinned below is its NEGATIVE
// direction and the wrong-field trap, which arm64 still exhibits.
TEST(LirArgCursorProjection, Aapcs64KeepsItsIntegerAndFpCursorsApartByDwarfNumber) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConvention(0);
    ASSERT_NE(cc, nullptr);

    EXPECT_FALSE(argPoolsShareACursor(**s, *cc, LirRegClass::GPR, LirRegClass::FPR))
        << "NGRN and NSRN are two counters; collapsing them would make a "
           "`double` argument consume an integer slot";
    // A class with no row cannot be shown to alias anything — it has no pool
    // to compare. This is the answer for VR now that the second declaration of
    // the SIMD&FP file is gone.
    EXPECT_FALSE(argPoolsShareACursor(**s, *cc, LirRegClass::FPR, LirRegClass::VR));
    EXPECT_FALSE(argPoolsShareACursor(**s, *cc, LirRegClass::GPR, LirRegClass::VR));

    // ★ THE WRONG-FIELD TRAP, STILL LIVE. ✔MEASURED: arm64's x0 and v0 BOTH
    // carry hwEncoding 0 while carrying different DWARF numbers, so an
    // hwEncoding-based derivation would make the integer and floating pools
    // share a cursor — the same wrong answer the two-way rule gave, arrived at
    // from the other direction. The assertion above is what that mutant reds.
    auto const gprOrd = (*s)->registerByName(cc->argGprs.front());
    auto const fprOrd = (*s)->registerByName(cc->argFprs.front());
    ASSERT_TRUE(gprOrd.has_value());
    ASSERT_TRUE(fprOrd.has_value());
    auto const& g = (*s)->registers()[*gprOrd];
    auto const& f = (*s)->registers()[*fprOrd];
    EXPECT_EQ(g.hwEncoding, f.hwEncoding)
        << "the trap is only a trap while the two files really do reuse "
           "hardware numbers; if they stop, this arm proves nothing";
    ASSERT_TRUE(g.dwarfNumber.has_value());
    ASSERT_TRUE(f.dwarfNumber.has_value());
    EXPECT_NE(*g.dwarfNumber, *f.dwarfNumber)
        << "a general register and a floating register share a DWARF number, "
           "which would make every aliasing derivation in this file "
           "meaningless";

    // …and no general register anywhere collides with a floating one.
    auto const gprD = dwarfNumbersOf(**s, TargetRegClass::GPR);
    auto const fprD = dwarfNumbersOf(**s, TargetRegClass::FPR);
    ASSERT_FALSE(gprD.empty());
    ASSERT_FALSE(fprD.empty());
    // ⓘ The VR class is EMPTY on both shipped targets now, which is why this
    // arm no longer compares a third list. That absence is the subject of
    // `tests/lir/test_lir_aliased_view_allocability`, not of this file.
    EXPECT_TRUE(dwarfNumbersOf(**s, TargetRegClass::VR).empty())
        << "arm64 declares a VR-class register again — the second declaration "
           "of one physical file is back, and this file's cursor derivation "
           "stops being inert";
    for (auto const gd : gprD) {
        for (auto const fd : fprD) {
            EXPECT_NE(gd, fd);
        }
    }
}

// ── (D) THE WALK ────────────────────────────────────────────────────────────
//
// The property the disassembly measured, as a unit claim: `s3(double, double,
// <fp>)` puts the third floating argument at NSRN 2, not at integer slot 0.
//
// ⚠⚠ THIS ARM USED TO SPELL THE THIRD ARGUMENT `LirRegClass::VR` AND ASSERT IT
// LANDED AT INDEX 2 — the two-pools-one-cursor property, where an FPR argument
// and a VR argument advanced ONE shared counter because `argFprs` (d0..d7) and
// `argVrs` (v0..v7) were two views of one physical file. R1 of design A′
// declared that file once, so VR names no pool at all and the correct answer
// for it is now NOTHING: `next(VR)` must return nullopt, which is asserted
// below. ★ THAT IS A SHARPER PIN OF THIS FILE'S DEFECT, NOT A WEAKER ONE — the
// defect was a VR argument falling through into the INTEGER pool, and nullopt
// is the one answer that cannot be mistaken for integer slot 0.
TEST(LirArgCursorProjection, FpArgumentsAdvanceOneCursorAndAClasslessArgGetsNone) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ArgCursors cursors{**s, *cc};

    auto const a = cursors.next(LirRegClass::FPR);
    auto const b = cursors.next(LirRegClass::FPR);
    auto const c = cursors.next(LirRegClass::FPR);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(a->index, 0u);
    EXPECT_EQ(b->index, 1u);
    EXPECT_EQ(c->index, 2u)
        << "a floating argument restarted a counter of its own — the shape "
           "that emitted `ldur q0` over an already-placed argument";

    // The integer cursor is INDEPENDENT of it (NGRN vs NSRN).
    auto const g = cursors.next(LirRegClass::GPR);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->index, 0u);

    // A class with no arg pool is NOT silently filed into one — and VR is
    // exactly such a class now, which is this file's defect stated in its
    // strongest form.
    EXPECT_FALSE(cursors.next(LirRegClass::None).has_value());
    EXPECT_FALSE(cursors.next(LirRegClass::Flags).has_value());
    EXPECT_FALSE(cursors.next(LirRegClass::VR).has_value())
        << "a VR-class argument took a cursor slot. With no row naming it, the "
           "ONLY correct answer is nullopt — a Slot would mean it had fallen "
           "into some other class's pool, which is the miscompile this file "
           "records";
    // …and taking those answers must not have moved the pools it did not name.
    auto const afterRefusals = cursors.next(LirRegClass::GPR);
    ASSERT_TRUE(afterRefusals.has_value());
    EXPECT_EQ(afterRefusals->index, 1u)
        << "a refused class advanced a cursor it has no pool in";

    // The exhaust clamp names a CLASS and reaches that class's cursor.
    ArgCursors clamped{**s, *cc};
    clamped.exhaust(LirRegClass::FPR);
    auto const after = clamped.next(LirRegClass::FPR);
    ASSERT_TRUE(after.has_value());
    EXPECT_GE(after->index, after->poolSize)
        << "exhausting the FP pool must stack every later floating argument";
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
// ⚠⚠ THE EMPTY-POOL WITNESS IS NOW SYNTHESIZED, AND IT HAS TO BE. This arm
// used to read the undeclared-pool refusal off x86_64's `argVrs`: a row existed
// for VR and x86_64 populated it with nothing, because an x87 `long double`
// uses the implicit st0 stack. R1 of design A′ deleted that cc key along with
// the second declaration of arm64's SIMD&FP file, and ✔MEASURED at this tree
// NO shipped calling convention leaves an arg pool empty — every one of the
// four populates both `argGprs` and `argFprs`. Reaching for `LirRegClass::VR`
// again would silently test the WRONG refusal (VR has no ROW now, so it
// answers `L_ArgClassHasNoRegisterPool` — the very code this arm exists to
// keep distinct). So the empty pool is SYNTHESIZED as a fixture target below:
// a cc that declares integer arg registers and no floating ones.
TEST(LirArgCursorProjection, TheThreeArgPoolRefusalsNeverShareACode) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConvention(0);
    ASSERT_NE(cc, nullptr);

    DiagnosticReporter noRow;
    EXPECT_FALSE(argPassingRegister(**s, *cc, 0, LirRegClass::Flags,
                                    "argCursorProjection", noRow).has_value());
    ASSERT_EQ(noRow.all().size(), 1u) << summarize(noRow);
    EXPECT_EQ(noRow.all()[0].code, DiagnosticCode::L_ArgClassHasNoRegisterPool);

    // A class whose ROW exists and whose POOL the target left empty. Written
    // as a fixture document rather than read off a shipped one, because no
    // shipped cc has an empty arg pool any more.
    auto emptyFpr = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"noFpArgs"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[
              {"name":"x0","class":"gpr","widthBytes":8,"hwEncoding":0},
              {"name":"sp","class":"gpr","widthBytes":8,"hwEncoding":1},
              {"name":"f0","class":"fpr","widthBytes":8,"hwEncoding":0}],
            "callingConventions":[
              {"name":"noFpArgs","argGprs":["x0"],"stackPointer":"sp",
               "stackAlignment":16}
            ]})",
        "<inline>");
    ASSERT_TRUE(emptyFpr.has_value())
        << (emptyFpr.has_value()
                ? std::string{}
                : [&] {
                      std::string o;
                      for (auto const& d : emptyFpr.error())
                          o += "\n  " + d.path + ": " + d.message;
                      return o;
                  }())
        << "a cc that declares NO floating arg registers is a legal target — "
           "if it stops loading, this arm needs a different empty pool, not a "
           "different assertion";
    auto const* emptyCc = (*emptyFpr)->callingConvention(0);
    ASSERT_NE(emptyCc, nullptr);
    ASSERT_TRUE(emptyCc->argFprs.empty());
    ASSERT_NE(argRegisterPool(*emptyCc, LirRegClass::FPR), nullptr)
        << "the FPR ROW must still exist — 'no row' and 'empty pool' are the "
           "two facts this arm keeps apart, and a missing row would collapse "
           "them";

    DiagnosticReporter undeclared;
    EXPECT_FALSE(argPassingRegister(**emptyFpr, *emptyCc, 0, LirRegClass::FPR,
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
