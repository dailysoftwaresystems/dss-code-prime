// ── D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE ───────────
// ── D-TARGET-ALLOCATABLE-POOL-LIST-SET-HAS-NO-OWNER ────────────────────────
// ────────────────────────────────────────────────────────────────────────────
//
// ★★★ THIS FILE IS THE TRIPWIRE FOR
// D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS, AND IT IS ALSO THE
// MARKER THAT NAMES WHAT A LATER CYCLE RESTORES. The allocator does not model
// two register classes declared over ONE physical register file. arm64 declares
// exactly that — `fpr` (d0..d31, 8 bytes) and `vr` (v0..v31, 16 bytes),
// ✔MEASURED to share DWARF numbers 64..95, thirty-two pairs, with zero
// same-class collisions and zero collisions on x86_64. Until the
// sub-register-aware allocator lands, a configuration that would make BOTH
// views allocatable is refused at LOAD, and these arms are what keeps that
// refusal honest.
//
// ⚠⚠ WHAT THE REFUSAL IS STANDING IN FOR, STATED SO IT IS NOT MISTAKEN FOR THE
// DESIGN: making both views allocatable is the RIGHT eventual answer — gcc does
// it (`aarch64-linux-gnu-gcc 13.3.0 -O2` on `double y; __asm__("nop":"=w"(y));
// return a + y;` emits `nop / fadd d0, d1, d0`, allocating the `"w"` value to
// one V register while a live `double` holds another). DSS refuses instead, and
// that is a CONFORMANCE GAP with a working reference, not a resting state. The
// load-time rule exists only because the allocator would otherwise answer
// WRONG rather than refuse. It is lifted by
// D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS and by nothing else.
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
//   (A) THE TRIGGER IS STILL FALSE — no shipped target makes a VR-class
//       register allocatable. Reds the moment one does, which is the
//       allocator's witness announcing itself.
//   (B) NON-VACUITY — a shipped target must actually DECLARE a VR class with
//       registers and a populated `argVrs`, or (A) asserts nothing at all.
//   (C) THE ALIASING FACT, STATED DIRECTLY — arm64's vector arg registers and
//       its float arg registers are the SAME physical registers under two
//       class names. Written out as an ABI claim rather than derived from the
//       tables the code reads, so an edit cannot move both halves of the
//       comparison at once (the P23 lesson on the return side).
//   (D) THE REFUSAL FIRES — a target that names both views in an allocatable
//       list must FAIL TO LOAD, and its message must name the anchor that
//       lifts the rule.
//   (E) THE REFUSAL IS NOT OVER-BROAD — both shipped targets load clean, and
//       arm64 does so WHILE declaring the aliased pair, so (D) is not passing
//       because the rule refuses everything.
//   (F) ONE OWNER — the set the allocator absorbs is the set `validate()`
//       judges. `kAllocatablePoolLists` is that object; this arm pins the
//       membership so adding `argVrs` to it is a deliberate, visible act.

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

// ── (A) THE TRIGGER IS STILL FALSE ──────────────────────────────────────────
//
// ⚠ This arm is a STATEMENT ABOUT TODAY that is designed to stop being true.
// When it reds, read it as the sub-register-aware allocator's trigger firing —
// not as a test to relax. The correct response is the allocator arc, and the
// wrong response is deleting this arm.
TEST(LirAliasedViewAllocability, NoShippedTargetMakesAVectorRegisterAllocatable) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        for (auto const& cc : (*s)->callingConventions()) {
            SCOPED_TRACE(cc.name);
            auto const alloc = allocatableNames(cc);
            for (auto const& reg : (*s)->registers()) {
                if (reg.regClass != TargetRegClass::VR) continue;
                EXPECT_FALSE(alloc.contains(reg.name))
                    << "register '" << reg.name << "' is VR-class and "
                    << "allocatable. On a target whose vector and float views "
                       "are one physical file this hands one machine register "
                       "to two live values; and if the views are NOT aliased, "
                       "this is the trigger for "
                       "D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS "
                       "having fired — hand the failing case to that arc "
                       "rather than relaxing this arm";
            }
        }
    }
}

// ── (B) NON-VACUITY ─────────────────────────────────────────────────────────
TEST(LirAliasedViewAllocability, AShippedTargetActuallyDeclaresAVectorRegisterFile) {
    bool sawVrRegisters = false;
    bool sawVrArgPool   = false;
    for (char const* const t : kTargets) {
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        for (auto const& reg : (*s)->registers()) {
            if (reg.regClass == TargetRegClass::VR) sawVrRegisters = true;
        }
        for (auto const& cc : (*s)->callingConventions()) {
            if (!cc.argVrs.empty()) sawVrArgPool = true;
        }
    }
    ASSERT_TRUE(sawVrRegisters)
        << "no shipped target declares a VR-class register, so the arm above "
           "iterated an empty set and asserted nothing — a vacuous pass is "
           "exactly what this arm exists to prevent";
    ASSERT_TRUE(sawVrArgPool)
        << "no shipped target populates a vector arg pool, so 'VR is declared "
           "but not allocatable' is not a state any target is actually in";
}

// ── (C) THE ALIASING FACT, STATED DIRECTLY ──────────────────────────────────
//
// ⚠ DELIBERATELY NOT DERIVED FROM THE TABLES THE CODE READS. AAPCS64 §5.4
// passes a 128-bit value in v{k} and a `double` in d{k}, and those are the SAME
// machine register — that is an ABI fact about the processor, and it is written
// here as one. Deriving it would make this arm move together with whatever it
// is meant to catch.
TEST(LirAliasedViewAllocability, VectorAndFloatArgRegistersAreOnePhysicalFile) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConventionByName("aapcs64");
    ASSERT_NE(cc, nullptr);
    ASSERT_FALSE(cc->argVrs.empty());
    ASSERT_LE(cc->argVrs.size(), cc->argFprs.size());

    for (std::size_t k = 0; k < cc->argVrs.size(); ++k) {
        SCOPED_TRACE("arg slot " + std::to_string(k));
        auto const vOrd = (*s)->registerByName(cc->argVrs[k]);
        auto const dOrd = (*s)->registerByName(cc->argFprs[k]);
        ASSERT_TRUE(vOrd.has_value());
        ASSERT_TRUE(dOrd.has_value());
        auto const& v = (*s)->registers()[*vOrd];
        auto const& d = (*s)->registers()[*dOrd];

        EXPECT_EQ(v.regClass, TargetRegClass::VR);
        EXPECT_EQ(d.regClass, TargetRegClass::FPR);
        ASSERT_TRUE(v.dwarfNumber.has_value());
        ASSERT_TRUE(d.dwarfNumber.has_value());
        EXPECT_EQ(*v.dwarfNumber, *d.dwarfNumber)
            << "AAPCS64 passes a binary128 in v" << k << " and a double in d"
            << k << ", and those are ONE machine register — if their DWARF "
               "numbers ever disagree, the derivation every aliasing rule in "
               "the pipeline rests on has lost its ground truth";
        // Neither is declared a `subOf` the other: this aliasing is exactly the
        // shape D-TARGET-CC-NAMES-SUB-REGISTER does NOT catch, which is why a
        // second rule exists.
        EXPECT_TRUE(v.subOf.empty());
        EXPECT_TRUE(d.subOf.empty());
        // ★ And it is genuinely a WIDTH pair, not two names for one width —
        // that is what makes an aliasing-aware allocator necessary rather than
        // merely tidy.
        EXPECT_GT(v.widthBytes, d.widthBytes);
    }
}

// ── (D) THE REFUSAL FIRES ───────────────────────────────────────────────────
TEST(LirAliasedViewAllocability, NamingBothViewsInAnAllocatableListIsRefusedAtLoad) {
    // The naive fix the ⛔ row names so nobody reaches for it: put the V views
    // in `callerSaved` beside the D views already there.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            for (auto& cc : doc.at("callingConventions")) {
                cc.at("callerSaved").push_back("v0");
            }
        });
    ASSERT_FALSE(mutated.has_value())
        << "a convention naming BOTH d0 and v0 as allocatable must be REFUSED "
           "at load — the allocator partitions by class and would hand DWARF "
           "register 64 to two live values";

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
    // ★ And arm64 must still be the aliased case, or (D) is passing against a
    // target that no longer has the property under test.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    std::size_t crossClassPairs = 0;
    for (auto const& a : (*s)->registers()) {
        if (!a.dwarfNumber.has_value()) continue;
        for (auto const& b : (*s)->registers()) {
            if (&a >= &b) continue;
            if (!b.dwarfNumber.has_value()) continue;
            if (*a.dwarfNumber != *b.dwarfNumber) continue;
            if (a.regClass == b.regClass) continue;
            ++crossClassPairs;
        }
    }
    EXPECT_EQ(crossClassPairs, 32u)
        << "arm64 declares 32 aliased d/v pairs; if that count changes the "
           "measurement this whole file rests on has moved";
}

// ── (F) ONE OWNER ───────────────────────────────────────────────────────────
//
// ⚠ THIS ARM PINS A MEMBERSHIP, WHICH IS UNUSUAL AND DELIBERATE. The table is
// the ONE place that says which cc lists make a register allocatable, and it is
// read by three consumers in two tiers. Adding `argVrs` to it is precisely the
// engine-side naive fix; this arm makes that edit an explicit, reviewed act
// rather than a one-line convenience.
TEST(LirAliasedViewAllocability, TheAllocatablePoolListSetIsTheSixAbiLists) {
    ASSERT_EQ(kAllocatablePoolLists.size(), 6u);
    std::vector<std::string_view> const names{
        kAllocatablePoolListNames.begin(), kAllocatablePoolListNames.end()};
    EXPECT_EQ(names, (std::vector<std::string_view>{
                         "callerSaved", "calleeSaved", "argGprs", "argFprs",
                         "returnGprs", "returnFprs"}))
        << "the vector pools (`argVrs`/`returnVrs`) are absent BY DESIGN: on "
           "arm64 they name the V views of registers whose D views are already "
           "allocatable, so absorbing them double-counts one physical file. "
           "Adding them needs "
           "D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS";

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
