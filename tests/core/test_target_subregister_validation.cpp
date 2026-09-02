#include "core/types/target_schema.hpp"

#include <algorithm>
#include <format>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

// D-TARGET-CC-NAMES-SUB-REGISTER — the load-time rule that a calling
// convention may only name FULL registers, never a narrower view of one
// (`subOf` non-empty).
//
// Why this is a rule and not a comment: `.target.json` is USER-AUTHORABLE.
// The allocator's pools (`buildFreeLists` in lir_regalloc.cpp,
// `pickScratchRegs` in lir_rewrite.cpp) are the target's register table
// INTERSECTED with the active cc's name lists. So a sub-register named in,
// say, `callerSaved` used to be absorbed in total silence: the producer
// declared a register, the allocator handed it out zero times, and nothing
// anywhere said why. The singleton roles were worse than dropped — a
// `framePointer` spelled as a sub-register reserved the SUB ordinal while
// the parent stayed allocatable, putting a VLA function's fixed-frame base
// back in the pool.
//
// Rejecting at LOAD is what makes the pools' name filter load-bearing on
// its own. Both pool loops previously ALSO carried a private
// `if (!info.subOf.empty()) continue;`; with this rule in force those were
// unreachable — and an unreachable guard is an untestable one, free to rot
// into false comfort. They are deleted, and this file is the replacement.
//
// The negative arms below are the red-on-disable subjects: delete the
// `if (!reg.subOf.empty())` block in `TargetSchemaData::validate()`
// (target_schema.cpp, inside `checkRefs`) and every `Rejected` test here
// goes red, because the config then LOADS.

namespace {

using ::dss::ConfigDiagnostic;
using ::dss::DiagnosticCode;
using ::dss::TargetSchema;

// Two register pairs per class, each a full register plus a declared
// narrower view of it. Deliberately mirrors the shipped shape
// (`eax`.subOf == `rax`, `w0`.subOf == `x0`, and arm64's `d0`.subOf ==
// `v0`) without depending on a shipped file.
//
// ⚠ THE v0/d0 PAIR USED TO BE DECLARED `class:"vr"`, AND THE COMMENT ABOVE
// USED TO CALL IT "the d/q views arm64 anticipates for VR". The anticipation
// resolved the other way: R1 of design A′ made arm64 declare its SIMD&FP file
// ONCE, so `v0` is a 16-byte `fpr` ROOT and `d0` is its 8-byte `fpr` VIEW,
// and no shipped target declares a `vr` register at all. The fixture follows
// the shipped shape because that is the shape the rule now has to catch: a
// WIDE root and a NARROW view inside ONE class, which is exactly the pair
// D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE deliberately
// does not judge (it skips same-class pairs) and this rule therefore must.
constexpr std::string_view kRegisters = R"(
    {"name":"rax", "class":"gpr","widthBytes":8,"hwEncoding":0},
    {"name":"eax", "class":"gpr","widthBytes":4,"hwEncoding":0,"subOf":"rax"},
    {"name":"rbx", "class":"gpr","widthBytes":8,"hwEncoding":3},
    {"name":"xmm0","class":"fpr","widthBytes":8,"hwEncoding":0},
    {"name":"xmm0s","class":"fpr","widthBytes":4,"hwEncoding":0,"subOf":"xmm0"},
    {"name":"v0",  "class":"fpr","widthBytes":16,"hwEncoding":1},
    {"name":"d0",  "class":"fpr","widthBytes":8,"hwEncoding":1,"subOf":"v0"}
)";

// A target whose single calling convention carries `ccBody`. Everything
// outside `ccBody` is held FIXED across every case so the only variable
// under test is the register name in the cc.
[[nodiscard]] std::string targetWithCc(std::string_view ccBody) {
    return std::format(
        R"({{"dssTargetVersion":1,"target":{{"name":"fixture"}},
            "opcodes":[{{"mnemonic":"invalid","result":"none"}}],
            "registers":[{}],
            "callingConventions":[
              {{"name":"testcc","stackAlignment":16,{}}}
            ]}})",
        kRegisters, ccBody);
}

[[nodiscard]] bool anyHasCode(std::vector<ConfigDiagnostic> const& diags,
                              DiagnosticCode                       code) {
    return std::ranges::any_of(
        diags, [code](auto const& d) { return d.code == code; });
}

// The matcher the sub-register rule's pins use. Kept as ONE function so
// the red-on-disable witness search and the assertions cannot drift apart:
// a diagnostic qualifies only if it carries the config-load code AND names
// all three facts the message contract promises — the offending register,
// the list it appeared in, and the register it is a sub-register of.
[[nodiscard]] bool hasSubRegisterRejection(
    std::vector<ConfigDiagnostic> const& diags,
    std::string_view                     reg,
    std::string_view                     field,
    std::string_view                     parent) {
    return std::ranges::any_of(diags, [&](auto const& d) {
        return d.code == DiagnosticCode::C_MalformedJson
               && d.message.find("sub-register") != std::string::npos
               && d.message.find(std::format("register '{}'", reg))
                      != std::string::npos
               && d.message.find(std::format(".{}:", field)) != std::string::npos
               && d.message.find(std::format("of '{}'", parent))
                      != std::string::npos;
    });
}

}  // namespace

// ── the rule fires, on every list ────────────────────────────────────────

// The plural-list half. Each of the six lists `buildFreeLists` /
// `pickScratchRegs` absorb into their `allocatable` set is covered — one
// rule at one site, not a per-list special case, so a future seventh list
// inherits it.
TEST(TargetSubRegisterValidation, SubRegisterInAnyCcListRejectedAtLoad) {
    struct Case {
        char const* field;
        char const* reg;
        char const* parent;
    };
    // Each sub-register is CORRECTLY CLASSED for its list, so the only
    // rule that can fire is the sub-register rule — a class-mismatch
    // diagnostic could otherwise mask a missing rejection.
    //
    // ⚠ TWO ROWS WERE DELETED HERE AND TWO REPLACE THEM. The deleted pair was
    // {"argVrs","d0","v0"} / {"returnVrs","d0","v0"} — "the two VR lists that
    // share the same resolution path". Those cc keys no longer exist: R1 of
    // design A′ removed the second declaration of arm64's SIMD&FP file, and
    // `argVrs`/`returnVrs` went with it (a cc still declaring one is now
    // refused as an UNKNOWN KEY — pinned in `test_target_schema`). The v0/d0
    // pair is kept and re-aimed at the FP lists, because that pair is now the
    // SHIPPED arm64 shape and this rule is what keeps `d0` out of the free
    // list beside `v0`.
    constexpr Case kCases[] = {
        {"argGprs",     "eax",   "rax"},
        {"returnGprs",  "eax",   "rax"},
        {"callerSaved", "eax",   "rax"},
        {"calleeSaved", "eax",   "rax"},
        {"argFprs",     "xmm0s", "xmm0"},
        {"returnFprs",  "xmm0s", "xmm0"},
        {"argFprs",     "d0",    "v0"},
        {"callerSaved", "d0",    "v0"},
    };

    for (auto const& c : kCases) {
        auto const json = targetWithCc(
            std::format(R"("{}":["{}"])", c.field, c.reg));
        auto r = TargetSchema::loadFromText(json, "<inline>");

        ASSERT_FALSE(r.has_value())
            << "callingConvention." << c.field << " naming sub-register '"
            << c.reg << "' must be REJECTED at load — it would otherwise be "
                        "silently dropped from the allocatable pool";
        EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson))
            << c.field;
        EXPECT_TRUE(hasSubRegisterRejection(r.error(), c.reg, c.field, c.parent))
            << "the diagnostic must name the register, the list, and the "
               "parent it is a sub-register of; got: "
            << (r.error().empty() ? std::string{"<none>"}
                                  : r.error().front().message);
    }
}

// The singleton-role half. These resolve to a table ordinal exactly as the
// list entries do, and a sub-register spelling here is WORSE than dropped:
// `framePointer` would reserve the sub ordinal while the parent stayed
// allocatable, handing a VLA function's frame base to a vreg.
TEST(TargetSubRegisterValidation, SubRegisterInAnyCcSingletonRoleRejected) {
    for (auto const* field : {"stackPointer", "framePointer", "linkRegister",
                              "indirectResultRegister"}) {
        auto const json = targetWithCc(
            std::format(R"("argGprs":["rax"],"{}":"eax")", field));
        auto r = TargetSchema::loadFromText(json, "<inline>");

        ASSERT_FALSE(r.has_value())
            << "callingConvention." << field
            << " naming sub-register 'eax' must be REJECTED at load";
        EXPECT_TRUE(hasSubRegisterRejection(r.error(), "eax", field, "rax"))
            << field;
    }
}

// The exact message contract, asserted once on a concrete case rather than
// only through the matcher — so a future edit that keeps the substrings but
// loses their meaning is still visible in a diff.
TEST(TargetSubRegisterValidation, RejectionNamesRegisterListAndParent) {
    auto r = TargetSchema::loadFromText(
        targetWithCc(R"("callerSaved":["eax"])"), "<inline>");
    ASSERT_FALSE(r.has_value());

    auto const it = std::ranges::find_if(r.error(), [](auto const& d) {
        return d.message.find("sub-register") != std::string::npos;
    });
    ASSERT_NE(it, r.error().end()) << "no sub-register diagnostic emitted";

    EXPECT_EQ(it->code, DiagnosticCode::C_MalformedJson);
    EXPECT_EQ(it->severity, dss::DiagnosticSeverity::Error);
    // The JSON path points at the offending ELEMENT, not the cc — the
    // producer's fix is on that line.
    EXPECT_EQ(it->path, "/callingConventions/0/callerSaved/0");
    EXPECT_NE(it->message.find("callingConvention 'testcc'.callerSaved:"),
              std::string::npos)
        << it->message;
    EXPECT_NE(it->message.find("register 'eax'"), std::string::npos)
        << it->message;
    EXPECT_NE(it->message.find("sub-register of 'rax'"), std::string::npos)
        << it->message;
}

// ── the rule does NOT over-fire ──────────────────────────────────────────

// Positive control for every negative above: the SAME fixture, naming the
// FULL registers, must load. Without this, deleting the whole `checkRefs`
// body would still leave the negative arms... failing, but a rule that
// rejected everything would also pass them.
TEST(TargetSubRegisterValidation, FullRegistersInEveryCcRoleStillLoad) {
    auto r = TargetSchema::loadFromText(
        targetWithCc(
            // ⚠ `"argVrs":["v0"],"returnVrs":["v0"]` USED TO SIT HERE. Those
            // keys are retired and are now REFUSED as unknown, so leaving
            // them would make this positive control fail for a reason that
            // has nothing to do with `subOf`. `v0` keeps its place in the
            // fixture through `callerSaved`, so the FULL 16-byte root really
            // is exercised in an allocatable list.
            R"("argGprs":["rax","rbx"],"returnGprs":["rax"],
                "callerSaved":["rax","v0"],"calleeSaved":["rbx"],
                "argFprs":["xmm0"],"returnFprs":["xmm0"],
                "stackPointer":"rbx","framePointer":"rax",
                "linkRegister":"rax","indirectResultRegister":"rbx")"),
        "<inline>");
    ASSERT_TRUE(r.has_value())
        << "a cc naming only full registers must load — the rule keys on "
           "`subOf`, not on cc membership";

    // And the sub-register rows themselves are still LOADED (the rule
    // rejects naming them in a cc, it does not ban declaring them).
    auto const eax = (*r)->registerByName("eax");
    ASSERT_TRUE(eax.has_value());
    auto const* info = (*r)->registerInfo(*eax);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->subOf, "rax");
}

// Every SHIPPED target must still load clean. `eax`..`r15d` and `w0`..`wzr`
// are real `subOf` rows in these files; the rule must reject only their
// appearance in a cc, which is precisely what the shipped configs avoid.
TEST(TargetSubRegisterValidation, EveryShippedTargetStillLoadsClean) {
    for (auto const* name : {"x86_64", "arm64"}) {
        auto const r = TargetSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value())
            << name << " no longer loads — the sub-register cc rule rejected "
                       "a SHIPPED config";
        // Non-vacuous: the target really does declare sub-register rows, so
        // the clean load above is evidence about the rule and not about an
        // empty input.
        auto const  regs = (*r)->registers();
        std::size_t subOfRows = 0;
        for (auto const& info : regs) {
            if (!info.subOf.empty()) ++subOfRows;
        }
        EXPECT_GT(subOfRows, 0u)
            << name << " declares no subOf rows — this test proves nothing "
                       "about the rule until it does";
    }
}

// ── ★ the reserved-register exclusions, after the deletion ───────────────

// THE PIN THE DELETION COULD PLAUSIBLY BREAK.
//
// `buildFreeLists` excludes three kinds of register, and NONE of them was
// ever excluded by the deleted `subOf` line:
//
//   * `rsp` / `rflags` (and arm64's `sp`) — excluded because they appear in
//     NO cc list, so the `allocatable` name filter drops them. They carry
//     no `subOf` at all, so the deleted line could never have applied.
//   * the VLA frame pointer (`rbp` / `x29`) — the opposite case: it IS in a
//     cc list (an ordinary allocatable callee-saved GPR for a non-VLA
//     function) and is held out ONLY by the `reservedFramePointer` ordinal
//     check, which compares against `cc.framePointer->ordinal`.
//
// This test pins the MECHANISM — each register's exclusion reason, and the
// absence of `subOf` on all of them — which is the claim the deletion rests
// on. The behavioural half for `rsp` is pinned by
// `LirRegAlloc.ReservedStackPointerNeverAllocated`.
TEST(TargetSubRegisterValidation, ReservedRegistersExcludedIndependentlyOfSubOf) {
    struct Target {
        char const* name;
        char const* stackPointer;
        char const* framePointer;
    };
    constexpr Target kTargets[] = {
        {"x86_64", "rsp", "rbp"},
        {"arm64",  "sp",  "x29"},
    };

    for (auto const& t : kTargets) {
        auto const r = TargetSchema::loadShipped(t.name);
        ASSERT_TRUE(r.has_value()) << t.name;
        auto const& sch = **r;

        // Does `name` appear in any of the six lists `buildFreeLists`
        // absorbs into `allocatable`? Mirrors that construction exactly.
        auto inAllocatableSet = [&](std::string_view name) {
            for (auto const& cc : sch.callingConventions()) {
                for (auto const* list : {&cc.argGprs, &cc.argFprs,
                                         &cc.returnGprs, &cc.returnFprs,
                                         &cc.callerSaved, &cc.calleeSaved}) {
                    for (auto const& n : *list) {
                        if (n == name) return true;
                    }
                }
            }
            return false;
        };
        auto infoFor = [&](std::string_view name) {
            auto const ord = sch.registerByName(name);
            EXPECT_TRUE(ord.has_value()) << t.name << ": " << name;
            return ord.has_value() ? sch.registerInfo(*ord) : nullptr;
        };

        // (1) The stack pointer: excluded by the NAME FILTER alone.
        auto const* sp = infoFor(t.stackPointer);
        ASSERT_NE(sp, nullptr);
        EXPECT_TRUE(sp->subOf.empty())
            << t.name << ": '" << t.stackPointer << "' carries subOf='"
            << sp->subOf
            << "' — the deleted subOf skip would have been its exclusion "
               "reason, which is exactly the confusion this pin exists to "
               "prevent";
        EXPECT_FALSE(inAllocatableSet(t.stackPointer))
            << t.name << ": '" << t.stackPointer
            << "' entered a calling-convention list — it is now ALLOCATABLE "
               "and the stack frame disappears mid-function";

        // (2) The VLA frame pointer: NOT excluded by the name filter (it is
        //     an ordinary callee-saved GPR), held out only by the
        //     `reservedFramePointer` ordinal check — and it carries no
        //     `subOf`, so the deleted line was never involved.
        auto const* fp = infoFor(t.framePointer);
        ASSERT_NE(fp, nullptr);
        EXPECT_TRUE(fp->subOf.empty())
            << t.name << ": frame pointer '" << t.framePointer
            << "' carries subOf — the reservation would hold out the SUB "
               "ordinal while the parent stayed allocatable";
        EXPECT_TRUE(inAllocatableSet(t.framePointer))
            << t.name << ": frame pointer '" << t.framePointer
            << "' must stay in the cc lists — a non-VLA function allocates "
               "it normally (the byte-identical-frames invariant)";

        // The reservation the allocator actually applies is
        // `cc.framePointer->ordinal`; pin that it resolves to the SAME
        // (full-width) register the checks above just examined.
        auto const fpOrdinal = sch.registerByName(t.framePointer);
        ASSERT_TRUE(fpOrdinal.has_value());
        bool sawFramePointerRole = false;
        for (auto const& cc : sch.callingConventions()) {
            if (!cc.framePointer.has_value()) continue;
            sawFramePointerRole = true;
            EXPECT_EQ(cc.framePointer->ordinal, *fpOrdinal)
                << t.name << " cc '" << cc.name
                << "': framePointer resolves to a different ordinal than '"
                << t.framePointer << "'";
        }
        EXPECT_TRUE(sawFramePointerRole)
            << t.name << " declares no framePointer on any cc — the VLA "
                         "reservation has no input and this pin is vacuous";
    }
}

// `rflags` is x86_64-only (arm64 models condition state differently), so
// it gets its own arm rather than a nullable column in the table above.
TEST(TargetSubRegisterValidation, X86FlagsRegisterExcludedByNameFilterOnly) {
    auto const r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    auto const& sch = **r;

    auto const ord = sch.registerByName("rflags");
    ASSERT_TRUE(ord.has_value()) << "x86_64 no longer declares rflags";
    auto const* info = sch.registerInfo(*ord);
    ASSERT_NE(info, nullptr);

    EXPECT_TRUE(info->subOf.empty())
        << "rflags carries subOf='" << info->subOf
        << "' — its exclusion must remain the name filter's doing";
    for (auto const& cc : sch.callingConventions()) {
        for (auto const* list : {&cc.argGprs, &cc.argFprs, &cc.returnGprs,
                                 &cc.returnFprs, &cc.callerSaved,
                                 &cc.calleeSaved}) {
            for (auto const& n : *list) {
                EXPECT_NE(n, "rflags")
                    << "rflags entered cc '" << cc.name
                    << "' — the allocator would hand out the flags register";
            }
        }
    }
}
