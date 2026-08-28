// D-CODEGEN-APPLE-ARM64-X29-USED-AS-GENERAL-SCRATCH-AGAINST-ITS-RESERVED-ROLE
//
// Apple's ARM64 platform ABI reserves x29 as the frame pointer UNCONDITIONALLY,
// more strictly than base AAPCS64. DSS allocated it as general scratch — saved
// and restored as callee-saved, so nothing crashed and no ordinary run could
// notice, which is exactly why the pins below assert on the EMITTED CODE rather
// than on a program's answer. A tool that walks frames without unwind tables —
// a sampling profiler, a crash reporter, `lldb` on a stripped image — reads x29
// directly and sees garbage.
//
// ⚠ THIS IS NOT A RE-OPENING OF
// [[D-CODEGEN-MACHO-ARM64-X29-ALLOCATED-WITH-NO-FRAME-RECORD]], which is CLOSED
// and stays closed: that row asked whether unwinding HAPPENS to work and
// measured that it does, table-driven, frame-for-frame identical to the clang
// control. This one asks whether the register's RESERVED ROLE is respected.
//
// ★★ THE CONTROL ARM IS THE LOAD-BEARING HALF OF THIS FILE. "x29 does not
// appear in the Apple arm's emitted code" is a claim that passes trivially if
// the allocator never had cause to reach x29 in the first place. So every Apple
// assertion here is paired with the SAME source compiled under `aapcs64` — the
// convention that declares no reservation — and that arm asserts x29 IS used.
// If the pressure source ever stops reaching x29 under AAPCS64, the control goes
// red and says so, rather than the Apple arm going quietly vacuous.
//
// ★ WHY BOTH ARMS ARE THE SHIPPED CONFIG AND NEITHER IS SYNTHESIZED. The two
// conventions differ HERE by exactly one declared key — `framePointerReservation`
// on `apple_arm64` — so the comparison isolates that key with no fixture in the
// middle. The REMOVE-direction mutant that proves the key is what does the work
// (rather than something else that happens to differ between the rows) is
// `TheReservationIsReadFromTheShippedSchemaNotTheEngine` at the bottom.

#include "synthetic_fn.hpp"
#include "mutate_target_schema.hpp"

#include "lir/lir_callconv.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

struct Arm64Ccs {
    std::shared_ptr<TargetSchema>  schema;
    TargetCallingConvention const* aapcs64 = nullptr;
    TargetCallingConvention const* apple   = nullptr;
    std::uint16_t                  appleIndex   = 0;
    std::uint16_t                  aapcs64Index = 0;
    std::uint16_t                  x29Ordinal   = 0;
};

// Fetching the conventions BY NAME rather than by index is not tidiness: the two
// rows differ only in the declaration under test, so an index typo would
// silently compare a row against itself and pass.
[[nodiscard]] Arm64Ccs loadArm64(std::shared_ptr<TargetSchema> schema) {
    Arm64Ccs out;
    out.schema  = std::move(schema);
    if (out.schema == nullptr) {
        ADD_FAILURE() << "null arm64 schema";
        return out;
    }
    out.aapcs64 = out.schema->callingConventionByName("aapcs64");
    out.apple   = out.schema->callingConventionByName("apple_arm64");
    for (std::uint16_t i = 0;; ++i) {
        auto const* cc = out.schema->callingConvention(i);
        if (cc == nullptr) break;
        if (cc->name == "apple_arm64") out.appleIndex = i;
        if (cc->name == "aapcs64")     out.aapcs64Index = i;
    }
    auto const ord = out.schema->registerByName("x29");
    if (!ord.has_value()) {
        ADD_FAILURE() << "arm64 declares no register named x29";
        return out;
    }
    out.x29Ordinal = *ord;
    return out;
}

[[nodiscard]] Arm64Ccs loadShippedArm64() {
    auto loaded = TargetSchema::loadShipped("arm64");
    if (!loaded) {
        ADD_FAILURE() << "TargetSchema::loadShipped(arm64) failed";
        return {};
    }
    return loadArm64(*loaded);
}

struct Pipeline {
    test_support::SyntheticFn synth;
    MirToLirResult            lir;
    LirLiveness               liveness;
    LirAllocation             alloc;
    LirRewriteResult          rewritten;
    LirCallconvResult         cc;
    DiagnosticReporter        reporter;

    explicit Pipeline(test_support::SyntheticFn s) : synth(std::move(s)) {}
};

// Lower a SYNTHETIC MIR function all the way through the register allocator and
// the callconv materializer under one named calling convention.
//
// ⚠ THE INPUT IS SYNTHESIZED MIR AND NOT C SOURCE, AND THAT IS A MEASUREMENT
// DECISION, NOT A CONVENIENCE. ✔MEASURED: the same pressure written as C locals
// (`long t0 = …;` × 48, all summed at the end) reached only
// `x0..x7, x14, x15` — every local became a STACK SLOT at this tier, so there
// was no register pressure at all and the control arm correctly refused to
// certify the pin. Long-lived MIR values have no stack slots to hide in, so the
// pressure is real and the allocator must exhaust the pool.
[[nodiscard]] Pipeline runPipeline(int liveValues,
                                   std::shared_ptr<TargetSchema> schema,
                                   std::uint16_t ccIndex) {
    std::array<TypeKind, 1> const paramKinds{TypeKind::I64};
    Pipeline p{test_support::buildSyntheticFn(
        paramKinds, TypeKind::I64,
        [liveValues](MirBuilder& mb, TypeInterner&,
                     std::vector<TypeId> const& params, TypeId retT) {
            MirInstId const a = mb.addArg(0, params[0]);
            // `liveValues` results, every one live until the final sum — so
            // they are all simultaneously live and the allocator must reach as
            // deep into the register file as the count demands.
            std::vector<MirInstId> vals;
            vals.reserve(static_cast<std::size_t>(liveValues));
            for (int i = 0; i < liveValues; ++i) {
                std::array<MirInstId, 2> ops{a, a};
                vals.push_back(mb.addInst(MirOpcode::Add, ops, retT));
            }
            MirInstId acc = vals[0];
            for (std::size_t i = 1; i < vals.size(); ++i) {
                std::array<MirInstId, 2> ops{acc, vals[i]};
                acc = mb.addInst(MirOpcode::Add, ops, retT);
            }
            mb.addReturn(acc);
        })};
    if (schema == nullptr) { ADD_FAILURE() << "null schema"; return p; }
    p.lir = lowerToLir(p.synth.mir, *schema, p.synth.interner, p.reporter);
    if (!p.lir.ok) {
        ADD_FAILURE() << "MIR->LIR lowering failed";
        return p;
    }
    p.liveness = analyzeLiveness(p.lir.lir);
    p.alloc = allocateRegisters(p.lir.lir, *schema, p.liveness, ccIndex,
                                p.reporter);
    if (!p.alloc.ok()) {
        ADD_FAILURE() << "allocateRegisters failed";
        return p;
    }
    p.rewritten = rewriteWithAllocation(p.lir.lir, *schema, p.alloc,
                                        p.reporter);
    if (!p.rewritten.ok) {
        ADD_FAILURE() << "rewriteWithAllocation failed";
        return p;
    }
    p.cc = materializeCallingConvention(p.rewritten.lir, *schema, p.alloc,
                                        p.reporter);
    return p;
}

// Count every mention of one PHYSICAL register ordinal in the materialized
// instruction stream — as an instruction RESULT (a write) and as a `Reg` OPERAND
// (a read). This is the emitted code: `materializeCallingConvention` has already
// expanded the prologue, the epilogue and every callee-saved save/restore, so a
// register that appears zero times here appears nowhere in the image.
struct RegMentions {
    std::size_t asResult   = 0;
    std::size_t asOperand  = 0;
    [[nodiscard]] std::size_t total() const { return asResult + asOperand; }
};

[[nodiscard]] RegMentions
countPhysicalReg(Lir const& lir, std::uint16_t ordinal) {
    RegMentions out;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                LirReg const res = lir.instResult(inst);
                if (res.isPhysical != 0u && res.id == ordinal) ++out.asResult;
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind != LirOperandKind::Reg) continue;
                    if (o.reg.isPhysical != 0u && o.reg.id == ordinal) {
                        ++out.asOperand;
                    }
                }
            }
        }
    }
    return out;
}

// Every DISTINCT physical GPR ordinal the materialized stream mentions, as a
// readable list. Carried in the control arm's failure message so a future
// "never reached x29" reports WHICH registers it did reach — the difference
// between "the pool is too small" and "the allocator stops earlier than
// expected" is otherwise invisible from a bare zero.
[[nodiscard]] std::string
usedPhysicalRegNames(Lir const& lir, TargetSchema const& schema) {
    std::set<std::uint16_t> ords;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        LirFuncId const fn = lir.funcAt(f);
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const blk = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(blk); ++i) {
                LirInstId const inst = lir.blockInstAt(blk, i);
                LirReg const res = lir.instResult(inst);
                if (res.isPhysical != 0u) {
                    ords.insert(static_cast<std::uint16_t>(res.id));
                }
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
        out += (ord < regs.size()) ? std::string{regs[ord].name}
                                   : ("#" + std::to_string(ord));
    }
    return out;
}

// How many simultaneously-live values the pressure function holds. arm64
// declares 31 GPRs and x29 sits at the END of the callee-saved list, so the
// count must comfortably exceed the whole allocatable pool for the allocator to
// reach it. The CONTROL arm is what keeps this number honest: if a future
// allocator change stops reaching x29 at this count, the control fails LOUDLY
// and names the remedy rather than letting the Apple arm pass vacuously.
constexpr int kLiveValues = 48;

} // namespace

// ── (A) THE DECLARATION IS PRESENT AND SAYS WHAT IT MUST ────────────────────

TEST(FramePointerReservation, TheTwoArm64ConventionsDeclareDifferentReservations) {
    auto const t = loadShippedArm64();
    ASSERT_NE(t.apple, nullptr);
    ASSERT_NE(t.aapcs64, nullptr);
    // ✔MEASURED 2026-08-28 against the references, which is WHY they differ:
    // `clang --target=arm64-apple-macos11` emits ZERO non-prologue x29 writes at
    // -O2/-O3/-Os even with `-fomit-frame-pointer` explicitly requested, while
    // `clang --target=aarch64-linux-gnu` emits four on the identical source. The
    // disjunction `(gcc ∪ clang ∪ MSVC)` therefore PERMITS the allocation on ELF
    // and FORBIDS it on Apple.
    EXPECT_EQ(t.apple->framePointerReservation, FramePointerReservation::Always)
        << "apple_arm64 must declare that its platform ABI reserves the frame "
           "pointer unconditionally";
    EXPECT_EQ(t.aapcs64->framePointerReservation,
              FramePointerReservation::DynamicFrameOnly)
        << "aapcs64 must keep the default — clang really does allocate x29 on "
           "ELF, so reserving it here would be an invented restriction, which "
           "the bar forbids in the same breath as an unimplemented one";
    // Both name the register; a reservation naming nothing reserves nothing.
    ASSERT_TRUE(t.apple->framePointer.has_value());
    EXPECT_EQ(t.apple->framePointer->name, "x29");
}

// ── (B) THE ALLOCATOR WITHHOLDS IT — AND THE CONTROL SHOWS IT WOULD NOT ─────

TEST(FramePointerReservation, AppleReservesTheFramePointerInAPlainLeafFunction) {
    auto const t = loadShippedArm64();
    ASSERT_NE(t.apple, nullptr);
    auto p = runPipeline(kLiveValues, t.schema, t.appleIndex);
    ASSERT_TRUE(p.alloc.ok());
    ASSERT_FALSE(p.alloc.perFunc.empty());
    for (auto const& fa : p.alloc.perFunc) {
        ASSERT_TRUE(fa.reservedFramePointer.has_value())
            << "apple_arm64 declares framePointerReservation `always`, so EVERY "
               "function must withhold the frame pointer — not only the ones "
               "that move SP at runtime";
        EXPECT_EQ(*fa.reservedFramePointer, t.x29Ordinal);
    }
}

TEST(FramePointerReservation, Aapcs64DoesNotReserveItInAPlainLeafFunction) {
    // THE CONTROL for the test above. Without it, "apple reserves x29" could be
    // true because every convention now reserves it — i.e. because the change
    // was unconditional and the ELF corpus silently moved with it.
    auto const t = loadShippedArm64();
    ASSERT_NE(t.aapcs64, nullptr);
    auto p = runPipeline(kLiveValues, t.schema, t.aapcs64Index);
    ASSERT_TRUE(p.alloc.ok());
    ASSERT_FALSE(p.alloc.perFunc.empty());
    for (auto const& fa : p.alloc.perFunc) {
        EXPECT_FALSE(fa.reservedFramePointer.has_value())
            << "aapcs64 declares no reservation and this function moves no SP "
               "at runtime, so x29 must stay an ordinary allocatable "
               "callee-saved GPR — byte-identical frames on the ELF legs is the "
               "whole zero-blast-radius claim of this change";
    }
}

// ── (C) THE EMITTED CODE — THE ARM THE ANCHOR ACTUALLY NAMES ───────────────

TEST(FramePointerReservation, NoEmittedInstructionTouchesX29UnderAppleArm64) {
    // ★★ THE PIN. `materializeCallingConvention` has run, so this stream IS the
    // emitted code: prologue, body, epilogue, every callee-saved save/restore.
    // Under a convention that reserves x29 unconditionally, the register must
    // not appear at all — it is neither allocated (so nothing writes it) nor
    // used (so nothing needs it saved).
    auto const t = loadShippedArm64();
    ASSERT_NE(t.apple, nullptr);
    auto p = runPipeline(kLiveValues, t.schema, t.appleIndex);
    ASSERT_TRUE(p.cc.ok());
    ASSERT_EQ(p.reporter.errorCount(), 0u);
    auto const m = countPhysicalReg(p.cc.lir, t.x29Ordinal);
    EXPECT_EQ(m.asResult, 0u)
        << "the emitted Apple ARM64 code WRITES x29 " << m.asResult
        << " time(s) — Apple's platform ABI reserves it as the frame pointer "
           "unconditionally, and a frame walker without unwind tables reads it "
           "directly";
    EXPECT_EQ(m.total(), 0u)
        << "the emitted Apple ARM64 code mentions x29 " << m.total()
        << " time(s)";
}

TEST(FramePointerReservation, TheAapcs64ControlDoesTouchX29OnTheSameSource) {
    // ★★★ WITHOUT THIS THE TEST ABOVE IS UNFALSIFIABLE. It asserts the pressure
    // source really does drive the allocator as far as x29 under a convention
    // that permits it — so a zero on the Apple arm means "withheld", not "never
    // wanted". If a future allocator change stops reaching x29 here, THIS test
    // goes red and names the reason, rather than its sibling passing vacuously.
    auto const t = loadShippedArm64();
    ASSERT_NE(t.aapcs64, nullptr);
    auto p = runPipeline(kLiveValues, t.schema, t.aapcs64Index);
    ASSERT_TRUE(p.cc.ok());
    ASSERT_EQ(p.reporter.errorCount(), 0u);
    auto const m = countPhysicalReg(p.cc.lir, t.x29Ordinal);
    EXPECT_GT(m.total(), 0u)
        << "the AAPCS64 control never reached x29 on this source, so the Apple "
           "arm's zero proves nothing about the reservation. Raise the register "
           "pressure in `kLiveValues` until this passes again — do NOT "
           "delete this test to make the suite green. Physical registers this "
           "source DID reach: "
        << usedPhysicalRegNames(p.cc.lir, *t.schema);
}

// ── (D) THE ANSWER LIVES IN THE `.target.json` ─────────────────────────────

TEST(FramePointerReservation, TheReservationIsReadFromTheShippedSchemaNotTheEngine) {
    // ★★ THE REMOVE-DIRECTION MUTANT. Take the declaration OUT of the shipped
    // document and the Apple arm must go back to allocating x29 — which is the
    // proof that the key is what does the work, and not some other difference
    // between the two cc rows. An ADD-direction fixture (injecting `always` into
    // a row that already has it) would stay green on the day the real config
    // lost the key, which is the failure this project paid eleven red tests to
    // learn.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            for (auto& cc : doc.at("callingConventions")) {
                if (cc.value("name", std::string{}) == "apple_arm64") {
                    cc.erase("framePointerReservation");
                }
            }
        });
    ASSERT_TRUE(mutated.has_value())
        << "the mutant target schema failed to load";

    auto const t = loadArm64(*mutated);
    ASSERT_NE(t.apple, nullptr);
    ASSERT_EQ(t.apple->framePointerReservation,
              FramePointerReservation::DynamicFrameOnly)
        << "erasing the key must fall back to the default — if it did not, the "
           "mutant is not the pre-fix engine and proves nothing";

    auto p = runPipeline(kLiveValues, t.schema, t.appleIndex);
    ASSERT_TRUE(p.cc.ok());
    ASSERT_FALSE(p.alloc.perFunc.empty());
    for (auto const& fa : p.alloc.perFunc) {
        EXPECT_FALSE(fa.reservedFramePointer.has_value())
            << "with the declaration removed the allocator still withheld x29 — "
               "the reservation is coming from the ENGINE, not from the "
               "`.target.json`, which is the defect "
               "D-CODEGEN-APPLE-ARM64-X29-USED-AS-GENERAL-SCRATCH-AGAINST-ITS-"
               "RESERVED-ROLE was closed by removing";
    }
    auto const m = countPhysicalReg(p.cc.lir, t.x29Ordinal);
    EXPECT_GT(m.total(), 0u)
        << "the un-declared mutant emitted no x29 either — then the shipped "
           "arm's zero is not attributable to the declaration";
}

// ── (E) A RESERVATION THAT RESERVES NOTHING IS REFUSED AT LOAD ─────────────

TEST(FramePointerReservation, ADeclaredReservationWithNoFramePointerIsRefused) {
    // The declaration's entire content is "withhold THIS register". A row that
    // says `always` while naming no register has stated an ABI guarantee the
    // engine cannot keep — and would pass every test, because the pool it fails
    // to shrink is the pool it always had. Refused where the config is judged.
    auto bad = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"arm64","version":"0.0","abiModel":"register-machine"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ],
      "registers": [
        {"name":"x0","class":"gpr","widthBytes":8,"hwEncoding":0},
        {"name":"sp","class":"gpr","widthBytes":8,"hwEncoding":31}
      ],
      "callingConventions": [
        {
          "name":"reserves_nothing",
          "argGprs":["x0"], "argFprs":[], "returnGprs":["x0"],
          "returnFprs":[], "callerSaved":[], "calleeSaved":[],
          "stackAlignment":16, "stackPointer":"sp",
          "framePointerReservation":"always"
        }
      ]
    })");
    EXPECT_FALSE(bad.has_value())
        << "a calling convention declared framePointerReservation `always` with "
           "no framePointer register and loaded clean";
}

TEST(FramePointerReservation, AnUnknownReservationSpellingIsRefusedAtLoad) {
    // A typo falling back to the default would re-open the exact defect the key
    // closes, on the one convention whose author was trying to close it — the
    // `dataModel` discipline, applied to this axis.
    auto bad = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            for (auto& cc : doc.at("callingConventions")) {
                if (cc.value("name", std::string{}) == "apple_arm64") {
                    cc["framePointerReservation"] = "allways";
                }
            }
        });
    EXPECT_FALSE(bad.has_value())
        << "a misspelled framePointerReservation loaded clean and would have "
           "silently taken the default";
}
