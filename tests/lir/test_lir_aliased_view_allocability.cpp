// ── D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE ───────────
// ── D-TARGET-ALLOCATABLE-POOL-LIST-SET-HAS-NO-OWNER ────────────────────────
// ────────────────────────────────────────────────────────────────────────────
//
// ★★★ THIS FILE IS THE TRIPWIRE FOR
// D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS. The allocator does not
// model two register CLASSES declared over ONE physical register file:
// `lir_regalloc::buildFreeLists` partitions the allocatable set by class, so
// two rows of different classes over one physical register become two
// independently handed-out registers. A configuration that would make both
// views allocatable is refused at LOAD, and these arms are what keeps that
// refusal honest.
//
// ★★★ WHAT CHANGED, AND IT CHANGED THE SUBJECT OF HALF THIS FILE (R1 of the
// operator's design A′). arm64 used to BE the shape above: `fpr` (d0..d31,
// 8 bytes) and `vr` (v0..v31, 16 bytes), ✔MEASURED to share DWARF numbers
// 64..95, thirty-two CROSS-CLASS pairs. arm64 now declares its SIMD&FP file
// ONCE — `v0..v31` are class `fpr` at sixteen bytes and `q`/`d`/`s`/`h`/`b`
// are `subOf` VIEW rows of them, also `fpr` — and NO shipped target declares a
// `vr`-class register at all. ✔RE-MEASURED at this tree: arm64 declares 32
// SAME-class shared-`dwarfNumber` pairs (each `v_k` with its `d_k` view) and
// ZERO cross-class ones; x86_64 declares zero shared numbers of either kind.
//
// ⇒ THE SHAPE IS GONE BECAUSE R1 REMOVED IT, NOT BECAUSE THE RULE WAS RELAXED.
// The rule still judges every target that loads, and a config describing one
// physical register as two classes is still a silent wrong-register answer. So
// its SILENCE on the shipped corpus is not evidence that it works, and arm (D)
// below no longer takes that silence for evidence: it SYNTHESIZES THE NEGATIVE.
//
// ⚠⚠ AND THE CONFORMANCE GAP THIS FILE USED TO RECORD IS CLOSED, WHICH IS WHY
// THE PARAGRAPH RECORDING IT IS GONE RATHER THAN EDITED. It read: making both
// views allocatable is the RIGHT eventual answer — gcc does it
// (`aarch64-linux-gnu-gcc 13.3.0 -O2` on `double y; __asm__("nop":"=w"(y));
// return a + y;` emits `nop / fadd d0, d1, d0`, allocating the `"w"` value to
// one V register while a live `double` holds another) — and DSS refuses, a
// CONFORMANCE GAP rather than a resting state, liftable only by the
// sub-register-aware allocator arc. ✔The measurement was true and the
// PREDICTION was wrong: what lifted it was declaring the file once, so `"w"`
// binds `fpr` (the class a `double` already lives in), both spellings name one
// allocatable file, and no allocator change was needed. The aliasing model the
// arc was going to build already existed and is called `subOf`.
//
// ★★ THE DOUBLE-COUNT IS MEASURED, NOT ARGUED. ✔2026-08-23 (cycle P28) the
// naive fix was built as a mutant — `absorb(cc.argVrs)` / `absorb(cc.returnVrs)`
// added to `lir_regalloc::buildFreeLists` and `lir_rewrite::collectAllocatable`
// — and the arm64 `--config=release` disassembly of a function with thirty live
// `double`s and one `"w"` (VR-class) inline-asm output read:
//     nop                         <- the asm block; its "=w" output is in v7
//     fadd d14, d7,  d14          <- that VR value                  ... d7
//     fadd d14, d14, d7           <- the ordinary `double` a24      ... d7
// One physical register, two live values, rc=0, no diagnostic.
//
// ★ WHAT EACH ARM ASSERTS, and why no single one carries the claim:
//   (AB) THE SHAPE IS ABSENT FROM THE SHIPPED CORPUS, AND SAYING SO IS NOT
//       VACUOUS. ⚠ THIS ARM REPLACES TWO, AND THE ONE IT KILLED WAS THE
//       HONEST ONE. (A) asserted "no shipped target makes a VR-class register
//       allocatable" — still true, and now true because there is no VR-class
//       register anywhere, i.e. it iterated an empty set. (B) was this file's
//       OWN non-vacuity guard against exactly that, requiring a shipped target
//       to really declare a VR class with registers and a populated `argVrs`;
//       R1 made (B) FALSE, which is the guard doing its job. Neither is
//       rewritable in place, because the config they described is gone. The
//       replacement asserts what IS true and is checkable: every pair of
//       registers sharing a `dwarfNumber` on a shipped target is the SAME
//       class, over a walk proved non-empty by arm64's 32 same-class pairs.
//   (C) THE ALIASING FACT, STATED DIRECTLY — arm64's FP arg registers ARE the
//       full V registers, and `d_k` is a declared `subOf` VIEW of `v_k`
//       carrying its `dwarfNumber` and `hwEncoding` at half the width.
//       Written out as an ABI claim rather than derived from the tables the
//       code reads, so an edit cannot move both halves of the comparison at
//       once (the P23 lesson on the return side).
//   (D) THE REFUSAL FIRES, AND IT IS PINNED BY A SYNTHESIZED NEGATIVE — a
//       MUTANT target that re-declares two classes over one `dwarfNumber`,
//       both reachable through an allocatable pool list, must FAIL TO LOAD,
//       and its message must name the anchor that lifts the rule. ⚠ Never by
//       observing that the shipped targets are quiet: they are quiet because
//       they no longer describe the shape, which is not the same fact.
//   (E) THE REFUSAL IS NOT OVER-BROAD — both shipped targets load clean, and
//       arm64 does so WHILE declaring 32 aliased view pairs, so (D) is not
//       passing because the rule refuses everything.
//   (F) ONE OWNER — the set the allocator absorbs is the set `validate()`
//       judges. `kAllocatablePoolLists` is that object; this arm pins the
//       membership so widening it is a deliberate, visible act.
//   (G) THE RULE THAT NOW KEEPS `d0` AND `v0` APART — a cc naming `d0` is
//       refused by D-TARGET-CC-NAMES-SUB-REGISTER. With the file declared
//       once the two spellings are same-class, so (D)'s rule deliberately
//       skips them; this is the guarantee that took its place, and this file
//       is where a reader looks for it.

#include "core/types/config_path_walk.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_reg.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace dss;

namespace {

constexpr char const* kTargets[] = {"x86_64", "arm64"};

// Every register name this convention makes ALLOCATABLE, read off the one
// published table rather than re-listed here — a second list would be the very
// duplication D-TARGET-ALLOCATABLE-POOL-LIST-SET-HAS-NO-OWNER names.
[[nodiscard]] std::unordered_set<std::string>
allocatableNames(TargetCallingConvention const& cc) {
    std::unordered_set<std::string> out;
    for (auto const list : kAllocatablePoolLists) {
        for (auto const& n : cc.*list) out.insert(n);
    }
    return out;
}

[[nodiscard]] std::string joinDiagnostics(
    std::vector<ConfigDiagnostic> const& ds) {
    std::string s;
    for (auto const& d : ds) s += "\n  " + d.message;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

} // namespace

// ── (AB) THE SHAPE IS ABSENT FROM THE SHIPPED CORPUS ────────────────────────
//
// ⚠⚠ WHAT THIS ARM REPLACES, BECAUSE A DELETED ASSERTION MUST LEAVE A RECORD.
// Two arms stood here. (A) `NoShippedTargetMakesAVectorRegisterAllocatable`
// walked every register of class `TargetRegClass::VR` and asserted none was
// reachable through an allocatable cc list; it was described as "a STATEMENT
// ABOUT TODAY that is designed to stop being true", whose red would be the
// sub-register-aware allocator's trigger announcing itself. (B)
// `AShippedTargetActuallyDeclaresAVectorRegisterFile` was its non-vacuity
// guard: a shipped target had to really declare VR-class registers AND a
// populated `argVrs`, or (A) iterated an empty set.
//
// ★ (B) IS THE ONE THAT WENT FALSE, AND THAT IS THE GUARD WORKING RATHER THAN
// A TEST TO RELAX. R1 removed arm64's `vr` class entirely and deleted the
// `argVrs` key, so BOTH of (B)'s preconditions are now unmeetable and (A)
// passes over nothing. Rewriting (A) alone would have left a green arm
// asserting a property of a config that no longer exists — the exact vacuous
// pass (B) existed to prevent. So they are replaced together, by the claim
// that is now true AND checkable: on a shipped target, two registers sharing a
// `dwarfNumber` are two VIEWS of one file within ONE class, never two classes.
//
// ⓘ The refusal itself is pinned by arm (D), on a synthesized mutant. This arm
// is a statement about the CORPUS; it is not, and must not be read as,
// evidence that the rule fires.
TEST(LirAliasedViewAllocability, NoShippedTargetDeclaresOnePhysicalRegisterAsTwoClasses) {
    std::size_t sharedDwarfPairs = 0;
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const regs = (*s)->registers();
        for (std::size_t i = 0; i < regs.size(); ++i) {
            if (!regs[i].dwarfNumber.has_value()) continue;
            for (std::size_t j = i + 1; j < regs.size(); ++j) {
                if (!regs[j].dwarfNumber.has_value()) continue;
                if (*regs[i].dwarfNumber != *regs[j].dwarfNumber) continue;
                ++sharedDwarfPairs;
                EXPECT_EQ(regs[i].regClass, regs[j].regClass)
                    << "registers '" << regs[i].name << "' and '"
                    << regs[j].name << "' are ONE physical register (they "
                       "share a dwarfNumber) declared under TWO classes. The "
                       "allocator partitions by class and would hand that one "
                       "machine register to two live values; R1 removed the "
                       "only config that had this shape, and re-introducing it "
                       "needs "
                       "D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS";
            }
        }
    }
    // Non-vacuity, inherited from the (B) this replaces: the walk must have
    // actually compared something. ✔MEASURED at this tree: arm64 declares 32
    // same-class shared-dwarf pairs (each `v_k` with its `d_k` view) and
    // x86_64 declares none, so the total is 32.
    EXPECT_EQ(sharedDwarfPairs, 32u)
        << "no shipped target declares two registers over one dwarfNumber, so "
           "the loop above compared nothing at all — and arm64's 32 v/d view "
           "pairs are the measurement this file rests on";
}

// ── (C) THE ALIASING FACT, STATED DIRECTLY ──────────────────────────────────
//
// ⚠ DELIBERATELY NOT DERIVED FROM THE TABLES THE CODE READS. AAPCS64 §5.4
// passes a 128-bit value in v{k} and a `double` in d{k}, and those are the SAME
// machine register — that is an ABI fact about the processor, and it is written
// here as one. Deriving it would make this arm move together with whatever it
// is meant to catch.
//
// ★ WHAT THIS ARM USED TO CLAIM AND WHY THE CLAIM MOVED. It read the pair off
// TWO cc lists — `argVrs[k]` (class `vr`, 16 bytes) against `argFprs[k]`
// (class `fpr`, 8 bytes) — and asserted that NEITHER was `subOf` the other,
// "exactly the shape D-TARGET-CC-NAMES-SUB-REGISTER does NOT catch, which is
// why a second rule exists". The ABI fact is unchanged; what changed is the
// CONFIG that states it. There is one FP arg pool now, it names the FULL
// registers, and `d_k` is a declared `subOf` view of `v_k` — so the very
// property this arm used to assert the absence of is the property it now
// asserts the presence of, and the second rule's job passed to
// D-TARGET-CC-NAMES-SUB-REGISTER (arm (G)).
TEST(LirAliasedViewAllocability, FpArgRegistersAreTheFullVRegistersAndDIsTheirView) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConventionByName("aapcs64");
    ASSERT_NE(cc, nullptr);
    ASSERT_EQ(cc->argFprs.size(), 8u)
        << "AAPCS64 §5.4 passes the first eight FP/SIMD arguments in v0..v7";

    for (std::size_t k = 0; k < cc->argFprs.size(); ++k) {
        SCOPED_TRACE("arg slot " + std::to_string(k));
        // The ABI claim, spelled out rather than read back off the cc: slot k
        // is the FULL register `v{k}`, and `d{k}` is its 64-bit view.
        std::string const vName = "v" + std::to_string(k);
        std::string const dName = "d" + std::to_string(k);
        EXPECT_EQ(cc->argFprs[k], vName)
            << "the FP arg pool must name the FULL SIMD&FP register — naming "
               "the 8-byte `d` view is the operand-claims-half-the-width shape "
               "D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD records";

        auto const vOrd = (*s)->registerByName(vName);
        auto const dOrd = (*s)->registerByName(dName);
        ASSERT_TRUE(vOrd.has_value());
        ASSERT_TRUE(dOrd.has_value());
        auto const& v = (*s)->registers()[*vOrd];
        auto const& d = (*s)->registers()[*dOrd];

        // ONE class over the file, and the wide row is the ROOT.
        EXPECT_EQ(v.regClass, TargetRegClass::FPR);
        EXPECT_EQ(d.regClass, TargetRegClass::FPR);
        EXPECT_TRUE(v.subOf.empty())
            << "v" << k << " must be a root row — a view of a view has no "
                           "full register for a cc to name";
        EXPECT_EQ(d.subOf, vName)
            << "d" << k << " must be a declared `subOf` VIEW of v" << k
            << "; that declaration is what stops both from being allocatable";

        // They are ONE machine register, and the config says so twice over.
        ASSERT_TRUE(v.dwarfNumber.has_value());
        ASSERT_TRUE(d.dwarfNumber.has_value());
        EXPECT_EQ(*v.dwarfNumber, *d.dwarfNumber)
            << "AAPCS64 passes a binary128 in v" << k << " and a double in d"
            << k << ", and those are ONE machine register — if their DWARF "
               "numbers ever disagree, the derivation every aliasing rule in "
               "the pipeline rests on has lost its ground truth";
        EXPECT_EQ(v.hwEncoding, d.hwEncoding)
            << "the shared hardware encoding is what made the original "
               "wrong-width store produce correct-looking bytes; it is a fact "
               "about the processor and must stay stated";
        // ★ And it is genuinely a WIDTH pair, not two names for one width.
        EXPECT_EQ(v.widthBytes, 16u);
        EXPECT_EQ(d.widthBytes, 8u);
    }
}

// ── (D) THE REFUSAL FIRES, PINNED BY A SYNTHESIZED NEGATIVE ─────────────────
//
// ★★★ THE MUTANT IS THE WHOLE POINT OF THIS ARM NOW. Before R1 the mutation
// was one line — arm64 already declared `d0` (fpr) and `v0` (vr) over
// dwarfNumber 64, so pushing `"v0"` into `callerSaved` beside the `d0` already
// there made both reachable and the rule fired. That config is gone: there is
// no `vr`-class register on any shipped target, so the same push now adds a
// SAME-class row the rule deliberately skips, and the arm would go green over
// a rule that had stopped working. So the two-class shape is REBUILT here.
//
// ⚠ NEVER ASSERT THIS RULE WORKS BY OBSERVING THAT THE SHIPPED TARGETS ARE
// SILENT. They are silent because they no longer describe the shape, which is
// a fact about the corpus and not about the rule.
TEST(LirAliasedViewAllocability, NamingBothViewsInAnAllocatableListIsRefusedAtLoad) {
    // Re-declare arm64's SIMD&FP file as TWO classes over one physical
    // register, the shape R1 removed: a second row carrying `v0`'s own
    // dwarfNumber 64 under class `vr`, reachable through `callerSaved` while
    // `v0` itself is reachable through `argFprs` and `callerSaved`.
    //
    // ⓘ A NEW ROW rather than a re-classed `d0`, so the mutation moves exactly
    // one fact — the number of classes over dwarf 64 — and cannot be satisfied
    // by some other rule firing on a disturbed `subOf` chain.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            doc.at("registers").push_back(nlohmann::json{
                {"name", "vmut0"}, {"class", "vr"},
                {"widthBytes", 16}, {"hwEncoding", 0}, {"dwarfNumber", 64}});
            for (auto& cc : doc.at("callingConventions")) {
                cc.at("callerSaved").push_back("vmut0");
            }
        });
    ASSERT_FALSE(mutated.has_value())
        << "a convention making BOTH classes over DWARF register 64 allocatable "
           "must be REFUSED at load — the allocator partitions by class and "
           "would hand that one machine register to two live values";

    auto const& ds = mutated.error();
    bool named = false;
    for (auto const& d : ds) {
        if (d.message.find("dwarfNumber 64") == std::string::npos) continue;
        if (d.message.find("D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS")
            == std::string::npos) {
            continue;
        }
        named = true;
    }
    EXPECT_TRUE(named)
        << "the refusal must name the shared DWARF number AND the anchor that "
           "lifts it — a guard that refuses without saying what it is standing "
           "in for tells a later cycle nothing about what to restore. Got:"
        << joinDiagnostics(ds);
}

// ── (E) THE REFUSAL IS NOT OVER-BROAD ───────────────────────────────────────
TEST(LirAliasedViewAllocability, EveryShippedTargetStillLoadsCleanly) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value())
            << "the aliased-view rule must not refuse a target that declares "
               "the aliasing WITHOUT making both views allocatable — arm64 is "
               "exactly that shape and is the reason this arm exists";
    }
    // ★ And arm64 must still DECLARE the aliasing, or this arm proves only
    // that the rule tolerates a target with nothing to judge.
    //
    // ⚠ THE COUNT THIS ARM READS CHANGED CLASS, NOT VALUE. It used to count
    // CROSS-class shared-dwarf pairs and require 32 — arm64's `d_k` (fpr)
    // against `v_k` (vr). R1 folded that file into one class, so cross-class
    // is now 0 and the same 32 pairs are SAME-class views. Counting the
    // cross-class shape here would assert the config is still defective.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    std::size_t viewPairs      = 0;
    std::size_t crossClassPairs = 0;
    for (auto const& a : (*s)->registers()) {
        if (!a.dwarfNumber.has_value()) continue;
        for (auto const& b : (*s)->registers()) {
            if (&a >= &b) continue;
            if (!b.dwarfNumber.has_value()) continue;
            if (*a.dwarfNumber != *b.dwarfNumber) continue;
            if (a.regClass == b.regClass) ++viewPairs;
            else                          ++crossClassPairs;
        }
    }
    EXPECT_EQ(viewPairs, 32u)
        << "arm64 declares 32 aliased v/d view pairs; if that count changes "
           "the measurement this whole file rests on has moved";
    EXPECT_EQ(crossClassPairs, 0u)
        << "arm64 declares one physical register under two CLASSES again — "
           "that is the shape R1 removed, and it is refused at load, so this "
           "count cannot be non-zero on a target that loaded";
}

// ── (F) ONE OWNER ───────────────────────────────────────────────────────────
//
// ⚠ THIS ARM PINS A MEMBERSHIP, WHICH IS UNUSUAL AND DELIBERATE. The table is
// the ONE place that says which cc lists make a register allocatable, and it is
// read by three consumers in two tiers. Widening it is precisely the
// engine-side naive fix; this arm makes that edit an explicit, reviewed act
// rather than a one-line convenience.
//
// ⓘ THE SIX ARE UNCHANGED, BUT THE REASON THE SEVENTH IS ABSENT IS. This
// message used to say the vector pools `argVrs`/`returnVrs` were "absent BY
// DESIGN" because absorbing them would double-count one physical file. Those
// two cc keys no longer exist at all — R1 deleted them along with the second
// declaration of the file they named — so the seventh entry a future author
// would reach for is a NEW pool, not those.
TEST(LirAliasedViewAllocability, TheAllocatablePoolListSetIsTheSixAbiLists) {
    ASSERT_EQ(kAllocatablePoolLists.size(), 6u);
    std::vector<std::string_view> const names{
        kAllocatablePoolListNames.begin(), kAllocatablePoolListNames.end()};
    EXPECT_EQ(names, (std::vector<std::string_view>{
                         "callerSaved", "calleeSaved", "argGprs", "argFprs",
                         "returnGprs", "returnFprs"}))
        << "these six ARE the allocatable set the allocator absorbs and "
           "`validate()` judges. A seventh entry naming a second pool over a "
           "file one of these already names double-counts that file, and "
           "needs D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS";

    // And the table really is what a consumer reads — a name that resolves to
    // no register on a shipped target would mean the table and the JSON keys
    // have drifted.
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        for (auto const& cc : (*s)->callingConventions()) {
            for (auto const& n : allocatableNames(cc)) {
                EXPECT_TRUE((*s)->registerByName(n).has_value())
                    << "allocatable name '" << n << "' resolves to no register";
            }
        }
    }
}

// ── (G) THE RULE THAT NOW KEEPS `d0` AND `v0` FROM BOTH BEING ALLOCATABLE ───
//
// ★★★ THIS ARM LIVES HERE BECAUSE THIS FILE IS WHERE A READER LOOKS FOR THE
// GUARANTEE, even though the rule it pins is D-TARGET-CC-NAMES-SUB-REGISTER
// and has its own file (`tests/core/test_target_subregister_validation`).
// Arm (D)'s rule deliberately skips a same-class pair (`first.regClass ==
// reg.regClass`), and after R1 `d0` and `v0` ARE same-class — so the question
// "what stops arm64 handing out DWARF register 64 twice?" is no longer
// answered anywhere in this file unless this arm answers it. The answer is
// that `d0` is a `subOf` VIEW and a calling convention may only name a FULL
// register, so `d0` can never enter an allocatable list in the first place.
//
// ⚠ THE REFUSAL'S MESSAGE DOES NOT CARRY THE ANCHOR ID, unlike (D)'s — so this
// arm matches on the message CONTRACT (`sub-register of '<parent>'`) that
// `test_target_subregister_validation` also pins. Asserting an id that is not
// emitted would fail for the right reason at the wrong site.
TEST(LirAliasedViewAllocability, ACcNamingTheDViewIsRefusedAsASubRegister) {
    // Swap the FULL register out of the FP arg pool for its own 64-bit view —
    // the edit an author makes who remembers AAPCS64 as "a double goes in d0".
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            for (auto& cc : doc.at("callingConventions")) {
                cc.at("argFprs")[0] = "d0";
            }
        });
    ASSERT_FALSE(mutated.has_value())
        << "a convention naming `d0` must be REFUSED at load: it is a declared "
           "view of `v0`, so naming it would either drop it from the pool in "
           "silence or — if a later cycle absorbed views — make one physical "
           "register allocatable twice";

    auto const& ds = mutated.error();
    bool named = false;
    for (auto const& d : ds) {
        if (d.message.find("register 'd0'") == std::string::npos) continue;
        if (d.message.find("sub-register of 'v0'") == std::string::npos) continue;
        named = true;
    }
    EXPECT_TRUE(named)
        << "the refusal must name the offending register AND the full register "
           "it is a view of — those two names are the producer's whole repair. "
           "Got:"
        << joinDiagnostics(ds);
}
