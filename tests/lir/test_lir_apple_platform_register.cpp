// D-CODEGEN-APPLE-ARM64-X18-ALLOCATED-AGAINST-THE-PLATFORM-RESERVATION
//
// Apple's ARM64 platforms RESERVE x18 — ✔DOCUMENTED, read 2026-09-19: "Writing
// ARM64 code for Apple platforms", section "Respect the purpose of specific CPU
// registers" ("The platforms reserve register x18. Don't use this register."),
// and clang's own rule, `AArch64::isX18ReservedByDefault` (true for Darwin,
// Windows, Android, Fuchsia, OHOS) feeding `ReserveXRegister.set(18)`.
// ✔MEASURED, clang 18.1.3: `arm64-apple-macos11` never allocates x18 and REFUSES
// an asm statement that leaves it no other register; `aarch64-linux-gnu` (and
// gcc 13.3.0) allocate it. DSS at `7df54cc1` listed x18 in `apple_arm64`'s
// `callerSaved`, so it allocated it on Darwin — 3 of the 1451 Mach-O arm64
// corpus artifacts of part 1's build write it (`c/large_spill_frame_arm64`,
// debug: `mov w18, #0x0; str x18, [sp, #0x108]`).
//
// ★★ THE FIX IS THE ABSENCE OF x18 FROM EVERY ALLOCATABLE LIST OF THE ROW — the
// rule that already keeps `sp` and x30 out of the allocator — so these pins read
// the EMITTED CODE, with the same two guards the x29 reservation's pins carry
// (`test_lir_frame_pointer_reservation.cpp`):
//   * THE CONTROL: the same source under `aapcs64` (Linux does NOT reserve x18)
//     must reach x18, or the Apple arm's zero proves nothing;
//   * THE REMOVE-DIRECTION MUTANT: x18 put back into `apple_arm64`'s
//     `callerSaved` in the shipped document must bring x18 back, which proves
//     the list — not some other difference between the rows — does the work.
//
// ★ THE PRESSURE SOURCE IS THE CORPUS SHAPE THAT WAS MEASURED TO REACH x18: a
// call with thirty constant arguments (`c/large_spill_frame_arm64`'s `main`),
// whose argument values are all live at the call. ✔MEASURED while writing this
// file: a synthetic 48-value sum does NOT reach x18 under either convention —
// its reloads are satisfied by lower scratch registers — so it could not have
// served as the control.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`.

#include "lowered_lir_fixture.hpp"
#include "mutate_target_schema.hpp"

#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_wide_call_args.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

struct Arm64 {
    std::shared_ptr<TargetSchema>  schema;
    TargetCallingConvention const* aapcs64 = nullptr;
    TargetCallingConvention const* apple   = nullptr;
    std::uint16_t                  appleIndex   = 0;
    std::uint16_t                  aapcs64Index = 0;
    std::uint16_t                  x18 = 0;
};

// By NAME, never by index: the rows differ in the declaration under test, so an
// index typo would compare a row against itself and pass.
[[nodiscard]] Arm64 loadArm64(std::shared_ptr<TargetSchema> schema) {
    Arm64 out;
    out.schema = std::move(schema);
    if (out.schema == nullptr) { ADD_FAILURE() << "null arm64 schema"; return out; }
    out.aapcs64 = out.schema->callingConventionByName("aapcs64");
    out.apple   = out.schema->callingConventionByName("apple_arm64");
    for (std::uint16_t i = 0;; ++i) {
        auto const* cc = out.schema->callingConvention(i);
        if (cc == nullptr) break;
        if (cc->name == "apple_arm64") out.appleIndex = i;
        if (cc->name == "aapcs64")     out.aapcs64Index = i;
    }
    auto const ord = out.schema->registerByName("x18");
    if (!ord.has_value()) { ADD_FAILURE() << "arm64 declares no x18"; return out; }
    out.x18 = *ord;
    return out;
}

[[nodiscard]] Arm64 loadShippedArm64() {
    auto loaded = TargetSchema::loadShipped("arm64");
    if (!loaded) { ADD_FAILURE() << "loadShipped(arm64) failed"; return {}; }
    return loadArm64(*loaded);
}

// `c/large_spill_frame_arm64`'s shape: thirty constant arguments, all live at
// the call — the corpus program DSS compiled with x18 at `7df54cc1`.
constexpr char const* kPressure =
    "int g(int p00, int p01, int p02, int p03, int p04, int p05, int p06, int p07,\n"
    "      int p08, int p09, int p10, int p11, int p12, int p13, int p14, int p15,\n"
    "      int p16, int p17, int p18, int p19, int p20, int p21, int p22, int p23,\n"
    "      int p24, int p25, int p26, int p27, int p28, int p29) {\n"
    "    return p28 + p29 + p08;\n"
    "}\n"
    "int main(void) {\n"
    "    return g(0, 0, 0, 0, 0, 0, 0, 0,\n"
    "             1, 0, 0, 0, 0, 0, 0, 0,\n"
    "             0, 0, 0, 0, 0, 0, 0, 0,\n"
    "             0, 0, 0, 0, 1, 40);\n"
    "}\n";

// The compile pipeline's own order from MIR→LIR to the emitted frame: the
// wide-call lowering, liveness, allocation, rewrite, two-address legalize and
// the callconv materializer, all under ONE convention. nullopt (with a test
// failure) if any tier refuses.
[[nodiscard]] std::optional<Lir> emitted(std::shared_ptr<TargetSchema> schema,
                                         std::uint16_t ccIndex) {
    auto lowered = lowerCToLir(kPressure, schema, ccIndex);
    if (!lowered.lir.ok) { ADD_FAILURE() << "MIR->LIR refused"; return std::nullopt; }
    DiagnosticReporter rep;
    auto wide = lowerWideCallArgs(lowered.lir.lir, *schema, ccIndex, rep);
    if (!wide.ok) { ADD_FAILURE() << "wide-call lowering refused"; return std::nullopt; }
    auto const liveness = analyzeLiveness(wide.lir);
    auto const alloc = allocateRegisters(wide.lir, *schema, liveness, ccIndex, rep);
    if (!alloc.ok()) { ADD_FAILURE() << "allocation refused"; return std::nullopt; }
    auto rewritten = rewriteWithAllocation(wide.lir, *schema, alloc, rep);
    if (!rewritten.ok) { ADD_FAILURE() << "rewrite refused"; return std::nullopt; }
    auto legal = legalizeTwoAddress(rewritten.lir, *schema, rep);
    if (!legal.ok()) { ADD_FAILURE() << "legalize refused"; return std::nullopt; }
    auto cc = materializeCallingConvention(legal.lir, *schema, alloc, rep);
    if (!cc.ok()) { ADD_FAILURE() << "callconv refused"; return std::nullopt; }
    EXPECT_EQ(rep.errorCount(), 0u);
    return std::move(cc.lir);
}

// Every mention of one PHYSICAL register in the materialized stream — this is
// the emitted code (prologue, body, epilogue, every save and reload).
[[nodiscard]] std::size_t countPhysicalReg(Lir const& lir, std::uint16_t ordinal) {
    std::size_t n = 0;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                LirReg const res = lir.instResult(inst);
                if (res.isPhysical != 0u && res.id == ordinal) ++n;
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind == LirOperandKind::Reg && o.reg.isPhysical != 0u
                        && o.reg.id == ordinal) {
                        ++n;
                    }
                }
            }
        }
    }
    return n;
}

[[nodiscard]] std::string usedPhysicalRegNames(Lir const& lir, TargetSchema const& schema) {
    std::set<std::uint16_t> ords;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                LirReg const res = lir.instResult(inst);
                if (res.isPhysical != 0u) ords.insert(static_cast<std::uint16_t>(res.id));
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind == LirOperandKind::Reg && o.reg.isPhysical != 0u) {
                        ords.insert(static_cast<std::uint16_t>(o.reg.id));
                    }
                }
            }
        }
    }
    auto const regs = schema.registers();
    std::string out;
    for (auto const ord : ords) {
        if (!out.empty()) out += ' ';
        out += (ord < regs.size()) ? std::string{regs[ord].name} : ("#" + std::to_string(ord));
    }
    return out;
}

[[nodiscard]] bool listsName(TargetCallingConvention const& cc, std::string_view reg) {
    for (auto const list : kAllocatablePoolLists) {
        for (auto const& n : cc.*list) {
            if (n == reg) return true;
        }
    }
    return false;
}

} // namespace

// ── (A) THE DECLARATION ─────────────────────────────────────────────────────
TEST(ApplePlatformRegister, OnlyTheAppleConventionLeavesX18OutOfEveryAllocatableList) {
    auto const t = loadShippedArm64();
    ASSERT_NE(t.apple, nullptr);
    ASSERT_NE(t.aapcs64, nullptr);
    EXPECT_FALSE(listsName(*t.apple, "x18"))
        << "apple_arm64 must name x18 in NO allocatable list — Apple reserves it";
    EXPECT_FALSE(listsName(*t.apple, "w18"));
    EXPECT_TRUE(listsName(*t.aapcs64, "x18"))
        << "aapcs64 (the Linux ELF convention) keeps x18: Linux does not reserve "
           "it, and gcc 13.3.0 and clang 18.1.3 both allocate it there";
}

// ── (B) THE EMITTED CODE ─────────────────────────────────────────────────────
TEST(ApplePlatformRegister, NoEmittedInstructionTouchesX18UnderAppleArm64) {
    auto const t = loadShippedArm64();
    ASSERT_NE(t.apple, nullptr);
    auto const lir = emitted(t.schema, t.appleIndex);
    ASSERT_TRUE(lir.has_value());
    EXPECT_EQ(countPhysicalReg(*lir, t.x18), 0u)
        << "the emitted Apple ARM64 code names x18 — a register the platform "
           "reserves. Registers reached: " << usedPhysicalRegNames(*lir, *t.schema);
}

TEST(ApplePlatformRegister, TheAapcs64ControlDoesReachX18OnTheSameSource) {
    // ★★★ WITHOUT THIS THE TEST ABOVE IS UNFALSIFIABLE.
    auto const t = loadShippedArm64();
    ASSERT_NE(t.aapcs64, nullptr);
    auto const lir = emitted(t.schema, t.aapcs64Index);
    ASSERT_TRUE(lir.has_value());
    EXPECT_GT(countPhysicalReg(*lir, t.x18), 0u)
        << "the AAPCS64 control never reached x18, so the Apple arm's zero "
           "proves nothing — find a source that does, do not delete this test. "
           "Registers reached: " << usedPhysicalRegNames(*lir, *t.schema);
}

// ── (C) THE REMOVE-DIRECTION MUTANT: the list is what does the work ─────────
TEST(ApplePlatformRegister, PuttingX18BackIntoTheAppleListBringsItBack) {
    auto mutated = mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            for (auto& cc : doc.at("callingConventions")) {
                if (cc.value("name", std::string{}) == "apple_arm64") {
                    cc.at("callerSaved").push_back("x18");
                }
            }
        });
    ASSERT_TRUE(mutated.has_value()) << "the mutant schema failed to load";
    auto const t = loadArm64(*mutated);
    ASSERT_NE(t.apple, nullptr);
    ASSERT_TRUE(listsName(*t.apple, "x18"));
    auto const lir = emitted(t.schema, t.appleIndex);
    ASSERT_TRUE(lir.has_value());
    EXPECT_GT(countPhysicalReg(*lir, t.x18), 0u)
        << "with x18 listed again the Apple arm still never used it — then the "
           "shipped arm's zero is not attributable to the list. Registers "
           "reached: " << usedPhysicalRegNames(*lir, *t.schema);
}
