// Two `.target.json` loader gates, both closing the same failure SHAPE:
// **a config that loads clean and then does nothing.**
//
//  (1) D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO — the psABI DWARF
//      register numbering (`registers[].dwarfNumber` +
//      `target.dwarfReturnAddressColumn`). Half a numbering loads without
//      complaint and makes `.eh_frame` silently never appear.
//  (2) D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY — `vaListLayout`'s key
//      set used to be a UNION over all three strategies, so a key belonging
//      to a DIFFERENT strategy (`gpOffsetLimit` on an `aapcs64_dual_cursor`
//      layout) spelled correctly, loaded clean, and was read by nothing.
//
// ★ Every fixture MUTATES THE SHIPPED FILE IN MEMORY rather than authoring a
//   parallel broken JSON. A hand-written stand-in would drift from the real
//   config and — worse — would let a gate pass against a shape the loader
//   never actually receives. `mutateShippedTargetSchemaDoc` is the existing
//   helper for exactly this (the cycle-10k rationale in its header).

#include "core/types/target_schema.hpp"

#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace {

using ::dss::DiagnosticCode;
using ::dss::TargetSchema;
using ::dss::test_support::mutateShippedTargetSchemaDoc;

// True iff SOME diagnostic's message contains `needle`. Fixed-substring, the
// same matcher the diagnostics themselves are read with elsewhere.
[[nodiscard]] bool saysAny(auto const& diags, std::string const& needle) {
    return std::ranges::any_of(diags, [&needle](auto const& d) {
        return d.message.find(needle) != std::string::npos;
    });
}

[[nodiscard]] std::string summarize(auto const& diags) {
    std::string s;
    for (auto const& d : diags) { s += "\n  " + d.path + ": " + d.message; }
    return s;
}

} // namespace

// ── (1) The DWARF numbering ─────────────────────────────────────

TEST(TargetDwarfNumbering, ShippedTargetsDeclareTheCompletePsAbiTable) {
    // The positive control. If this ever fails, every negative below is
    // testing a schema that was already broken.
    // ✔MEASURED 2026-08-13 with binutils 2.42 (`as`/`aarch64-linux-gnu-as`
    // + `readelf --debug-dump=frames` on a `.cfi_offset` probe): GAS itself
    // translates these names to these numbers.
    for (auto const& [target, raColumn, reg, dwarf, hw] :
         std::initializer_list<std::tuple<char const*, std::uint16_t,
                                          char const*, std::uint16_t,
                                          std::uint16_t>>{
             // ★ The four x86_64 rows where DWARF and hardware DISAGREE.
             {"x86_64", 16, "rsp", 7, 4},
             {"x86_64", 16, "rbp", 6, 5},
             {"x86_64", 16, "rsi", 4, 6},
             {"x86_64", 16, "rdi", 5, 7},
             // AArch64: x30 is a real register AND the RA column; sp is 31.
             {"arm64", 30, "x30", 30, 30},
             {"arm64", 30, "sp", 31, 31},
         }) {
        auto t = TargetSchema::loadShipped(target);
        ASSERT_TRUE(t.has_value()) << target;
        EXPECT_EQ((*t)->dwarfReturnAddressColumn(), raColumn) << target;
        auto const ord = (*t)->registerByName(reg);
        ASSERT_TRUE(ord.has_value()) << target << "/" << reg;
        auto const* info = (*t)->registerInfo(*ord);
        ASSERT_NE(info, nullptr);
        ASSERT_TRUE(info->dwarfNumber.has_value())
            << target << ": '" << reg << "' must declare a DWARF number";
        EXPECT_EQ(*info->dwarfNumber, dwarf)
            << target << "/" << reg << ": wrong DWARF number";
        EXPECT_EQ(info->hwEncoding, hw)
            << target << "/" << reg
            << ": the hardware encoding is the field an encoder must NOT "
               "reach for; this row exists so the two stay visibly distinct";
    }
}

TEST(TargetDwarfNumbering, NarrowViewsAndTheZeroRegisterCarryNoDwarfNumber) {
    // ABSENT is a real state, not an omission: DWARF numbers ARCHITECTURAL
    // registers, so `eax`/`w0` inherit their parent's identity rather than
    // getting one, and AArch64's `xzr` has none at all.
    // ✔MEASURED 2026-08-13: `aarch64-linux-gnu-as` REJECTS
    // `.cfi_offset xzr, -8` with "bad register expression".
    for (auto const& [target, reg] :
         std::initializer_list<std::pair<char const*, char const*>>{
             {"x86_64", "eax"}, {"x86_64", "r15d"},
             {"arm64", "w0"}, {"arm64", "wzr"}, {"arm64", "xzr"}}) {
        auto t = TargetSchema::loadShipped(target);
        ASSERT_TRUE(t.has_value());
        auto const ord = (*t)->registerByName(reg);
        ASSERT_TRUE(ord.has_value()) << target << "/" << reg;
        auto const* info = (*t)->registerInfo(*ord);
        ASSERT_NE(info, nullptr);
        EXPECT_FALSE(info->dwarfNumber.has_value())
            << target << "/" << reg
            << " must NOT carry a DWARF number — giving it the parent's "
               "would describe a save of the FULL-width register that never "
               "happened";
    }
}

TEST(TargetDwarfNumbering, RegisterNumbersWithoutTheReturnAddressColumnAreRejected) {
    // Half a numbering: every `dwarfNumber` in the file becomes inert,
    // because the `.eh_frame` writer treats a missing RA column as "this
    // target declares no numbering" and refuses the whole section.
    auto r = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        doc["target"].erase("dwarfReturnAddressColumn");
    });
    ASSERT_FALSE(r.has_value())
        << "a register table with DWARF numbers and no return-address column "
           "must be REJECTED at load, not accepted-and-ignored";
    EXPECT_TRUE(saysAny(r.error(), "both halves of the psABI numbering"))
        << summarize(r.error());
}

TEST(TargetDwarfNumbering, AReturnAddressColumnWithoutAnyRegisterNumberIsRejected) {
    // The mirror image. A column alone names no register, so the writer
    // would refuse on the first frame rule it tried to encode.
    auto r = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        for (auto& reg : doc["registers"]) { reg.erase("dwarfNumber"); }
    });
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(saysAny(r.error(), "no register row carries a 'dwarfNumber'"))
        << summarize(r.error());
}

TEST(TargetDwarfNumbering, AFrameRelevantRegisterMissingItsNumberIsRejected) {
    // ★ The COMPLETENESS gate, and note what it is derived from: "every
    //   register" would be the wrong requirement (narrow views and `xzr`
    //   legitimately have none). The set that MUST be numbered is the one
    //   the target ALREADY declares as frame-relevant — each calling
    //   convention's stackPointer / framePointer / linkRegister /
    //   calleeSaved. No new config key, and no way for the two to disagree.
    auto r = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        for (auto& reg : doc["registers"]) {
            // rbx is SysV callee-saved — a register frame rules WILL name.
            if (reg.value("name", std::string{}) == "rbx") {
                reg.erase("dwarfNumber");
            }
        }
    });
    ASSERT_FALSE(r.has_value())
        << "a callee-saved register with no DWARF number would make the "
           ".eh_frame writer refuse the entire section at link time";
    EXPECT_TRUE(saysAny(r.error(), "declares no 'dwarfNumber'"))
        << summarize(r.error());
    // The diagnostic must point at the DECLARATION that created the
    // requirement, not merely at the register row.
    EXPECT_TRUE(saysAny(r.error(), "calleeSaved")) << summarize(r.error());
    EXPECT_TRUE(saysAny(r.error(), "rbx")) << summarize(r.error());
}

TEST(TargetDwarfNumbering, ATargetDeclaringNeitherHalfStillLoads) {
    // Trigger discipline: a target that declares NO numbering at all is a
    // legitimate state (it simply cannot emit `.eh_frame`, and the writer
    // says so by name). Rejecting it here would force every synthetic test
    // schema in the tree to carry a psABI table it has no use for.
    auto r = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        doc["target"].erase("dwarfReturnAddressColumn");
        for (auto& reg : doc["registers"]) { reg.erase("dwarfNumber"); }
    });
    ASSERT_TRUE(r.has_value()) << summarize(r.error());
    EXPECT_FALSE((*r)->dwarfReturnAddressColumn().has_value());
}

TEST(TargetDwarfNumbering, AnOutOfRangeDwarfNumberIsRejected) {
    auto r = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        doc["registers"][0]["dwarfNumber"] = -1;
    });
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(saysAny(r.error(), "must fit in")) << summarize(r.error());
}

// ── (2) The per-strategy vaListLayout key set ───────────────────

namespace {

// Point the named calling convention's `vaListLayout` at `strategy` and set
// `key` on it. Returns the load result.
[[nodiscard]] auto loadWithVaListKey(char const* target, char const* ccName,
                                     char const* strategy, char const* key) {
    return mutateShippedTargetSchemaDoc(
        target, [&](nlohmann::json& doc) {
            for (auto& cc : doc["callingConventions"]) {
                if (cc.value("name", std::string{}) != ccName) continue;
                cc["vaListLayout"]["strategy"] = strategy;
                cc["vaListLayout"][key] = 48;
            }
        });
}

} // namespace

TEST(VaListStrategyKeys, ShippedTargetsStillLoad) {
    // The positive control for the whole gate: making the key set stricter
    // must not reject the configs that ship.
    for (char const* t : {"x86_64", "arm64"}) {
        auto r = TargetSchema::loadShipped(t);
        EXPECT_TRUE(r.has_value()) << t << summarize(r.error());
    }
}

TEST(VaListStrategyKeys, AKeyFromAnotherStrategyIsRejectedAndNamesBothSides) {
    // ★ THE DEFECT: `gpOffsetLimit` is a real key — of the
    //   `sysv_register_save` strategy. On an `aapcs64_dual_cursor` layout it
    //   is spelled correctly, loads clean under a UNION key set, and is read
    //   by nothing. The producer declared a limit, got no limit, and got no
    //   diagnostic.
    auto r = loadWithVaListKey("arm64", "aapcs64", "aapcs64_dual_cursor",
                               "gpOffsetLimit");
    ASSERT_FALSE(r.has_value())
        << "a key belonging to a DIFFERENT va_list strategy must be REJECTED, "
           "not silently ignored";
    // The diagnostic must name the key AND the strategy that would have
    // accepted it — that is what separates "a typo" from "a copy-paste from
    // the wrong ABI", and it is the difference between an actionable message
    // and an unknown-key echo.
    EXPECT_TRUE(saysAny(r.error(), "gpOffsetLimit")) << summarize(r.error());
    EXPECT_TRUE(saysAny(r.error(), "sysv_register_save"))
        << "the message must name the strategy that WOULD have accepted the "
           "key" << summarize(r.error());
    EXPECT_TRUE(saysAny(r.error(), "aapcs64_dual_cursor"))
        << "the message must name the strategy that was DECLARED"
        << summarize(r.error());
    EXPECT_TRUE(saysAny(r.error(), "never reads it")) << summarize(r.error());
}

TEST(VaListStrategyKeys, TheReverseDirectionIsRejectedToo) {
    // Not a one-way rule: an aapcs64-only key on a SysV layout is the same
    // defect mirrored. Pinning only one direction would leave the gate
    // half-built and passing.
    auto r = loadWithVaListKey("x86_64", "sysv_amd64", "sysv_register_save",
                               "grTopField");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(saysAny(r.error(), "grTopField")) << summarize(r.error());
    EXPECT_TRUE(saysAny(r.error(), "aapcs64_dual_cursor"))
        << summarize(r.error());
}

TEST(VaListStrategyKeys, AGenuineTypoSaysNoStrategyDeclaresIt) {
    // The two cases must read differently. "belongs to strategy X" sends the
    // author to the wrong ABI; "no strategy declares it" sends them to the
    // spelling. Collapsing them into one message loses the distinction the
    // gate exists to draw.
    auto r = loadWithVaListKey("x86_64", "sysv_amd64", "sysv_register_save",
                               "gpOffsetLimitt");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(saysAny(r.error(), "no va_list strategy declares it"))
        << summarize(r.error());
}

TEST(VaListStrategyKeys, AKeyValidForTheDeclaredStrategyIsAccepted) {
    // The complementary arm — proving the gate is scoped to the CROSS-strategy
    // case and has not simply become "reject everything optional".
    auto r = loadWithVaListKey("x86_64", "sysv_amd64", "sysv_register_save",
                               "gpOffsetLimit");
    ASSERT_TRUE(r.has_value()) << summarize(r.error());
}
