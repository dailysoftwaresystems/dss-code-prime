// THE CONVERSION, CROSS-FILE-MOVE, FP-MEMORY AND LANE-ARRANGEMENT ROWS OF THE
// aarch64 gas DIALECT — the LAST three gaps of
// D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME.
//
// ★★★ WHAT THESE PINS ARE FOR. Cycle P54's lane `ad` declared the scalar-FP
// mnemonics and reported THREE structural blockers it could not close from a
// config document. This file is the evidence that each is closed at its root:
//
//   (1) ELECTION WAS CLASS-BLIND. `fmov`, `movq_xmm_to_gpr` and
//       `movq_gpr_to_xmm` all take `[reg]` at width 64 and differ ONLY in which
//       bank each end lives in, so a row naming all three was refused as
//       ambiguous and a row naming one left `fmov x0, d1` electing the
//       diagonal. `load_u`/`fldr_u` are the same statement about memory.
//       `electOpcode` now reads the banks the TARGET already declared.
//   (2) A CONVERSION MNEMONIC IS ONE SPELLING OVER N WIDTH PAIRS. ✔MEASURED by
//       lane `ad`: a width-less `fcvtzs` row compiled `fcvtzs %w0, %s1` rc=0
//       and emitted `fcvtzs x16, s29` (0x9E3803B0) — the X form, where both
//       references give the W form. `FcvtzsNarrowDestinationIsTheWForm` is that
//       regression's ratchet, and `RemovingTheNarrowConversionOpcodeRefuses`
//       recreates the exact pre-P54 shape and proves it now REFUSES.
//   (3) THE LANE ARRANGEMENT `%0.16b` DID NOT PARSE.
//
// ★★ EVERY EXPECTED WORD IS ✔MEASURED AGAINST GNU as 2.42 **AND** clang 18.1.3
// SEPARATELY, both agreeing on every probe (60 probes, zero disagreements), and
// none is derived from a neighbouring form or read out of a manual.
//   ⇒ cycle P55 (lane fp) added the eight half-precision `fcvt` words below on
//     the same terms — 37 further probes, zero disagreements, each base
//     re-derived from THREE independent (Rd,Rn) placements:
//     [[D-TARGET-ARM64-FP16-CONVERSION-FORMS-UNDECLARED]].
//
// ⚠ THE OPERANDS ARE PINNED TO PHYSICAL REGISTERS: `assemble()` runs after
// register allocation and a virtual register has no encoding, so an unpinned
// run would assert against an empty byte vector — i.e. would measure the
// harness giving up rather than the instruction.

#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::string_view kDialect = "asm-arm64-gas";
constexpr std::string_view kTarget  = "arm64";

[[nodiscard]] std::string dialectText() {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{kDialect, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        throw std::runtime_error{"cannot locate the shipped aarch64 dialect"};
    }
    std::ifstream in{*pathR};
    if (!in) throw std::runtime_error{"cannot open the dialect document"};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

[[nodiscard]] std::shared_ptr<GrammarSchema> loadDialect() {
    auto g = GrammarSchema::loadFromText(dialectText(), std::string{kDialect});
    if (!g.has_value()) {
        std::string why;
        for (auto const& e : g.error()) why += e.path + ": " + e.message + "\n";
        throw std::runtime_error{"dialect did not load: " + why};
    }
    return *g;
}

// ── THE CONFIG-TIER MUTATOR, OVER THE **SHIPPED** DOCUMENT ──────────────────
// ⚠ REMOVE DIRECTION ONLY: an ADD-direction mutant stays green when the real
// config LOSES the feature, which is the direction that actually regresses.
[[nodiscard]] std::optional<std::shared_ptr<GrammarSchema>>
mutateShippedDialectDoc(std::function<void(nlohmann::json&)> const& edit) {
    auto doc = nlohmann::json::parse(dialectText());
    auto const before = doc.dump();
    edit(doc);
    if (doc.dump() == before) {
        throw std::runtime_error{
            "the mutation changed NOTHING — the pin below would be vacuous"};
    }
    auto g = GrammarSchema::loadFromText(doc.dump(), std::string{kDialect});
    if (!g.has_value()) return std::nullopt;
    return *g;
}

struct Run {
    std::shared_ptr<GrammarSchema> dialect;
    std::shared_ptr<TargetSchema>  target;
    DiagnosticReporter             reporter;
    DiagnosticReporter             asmReporter;
    bool                           parsed = false;
    bool                           ok     = false;
    std::vector<std::uint8_t>      bytes;
};

[[nodiscard]] std::string messages(Run const& r) {
    std::string out;
    for (auto const& d : r.reporter.all()) { out += d.actual; out += '\n'; }
    for (auto const& d : r.asmReporter.all()) { out += d.actual; out += '\n'; }
    return out;
}

[[nodiscard]] std::string hex(std::vector<std::uint8_t> const& b) {
    std::string out;
    for (auto const v : b) out += std::format("{:02X} ", v);
    return out;
}

// The FIRST 32-bit word, little-endian. This ISA's instruction is exactly one
// word, so the WHOLE instruction is one comparison and no field can be checked
// while a neighbour drifts — which matters most here, where `sf` (bit 31) and
// `ftype` (bits 23:22) are the entire difference between four instructions.
[[nodiscard]] std::uint32_t firstWord(std::vector<std::uint8_t> const& b) {
    if (b.size() < 4) return 0;
    return static_cast<std::uint32_t>(b[0])
         | (static_cast<std::uint32_t>(b[1]) << 8)
         | (static_cast<std::uint32_t>(b[2]) << 16)
         | (static_cast<std::uint32_t>(b[3]) << 24);
}

struct Bind {
    std::string   spelling;
    std::string   physical;
    LirRegClass   cls   = LirRegClass::FPR;
    std::uint32_t width = 64;
};

[[nodiscard]] std::unique_ptr<Run>
runOn(std::shared_ptr<GrammarSchema> dialect,
      std::shared_ptr<TargetSchema>  target,
      std::string_view               templateText,
      std::vector<Bind> const&       binds) {
    auto run     = std::make_unique<Run>();
    run->dialect = std::move(dialect);
    run->target  = std::move(target);

    auto tree = parseAsmTemplateText(std::string{templateText}, "<template>",
                                     run->dialect,
                                     AsmTemplateSurface::Extended,
                                     DiagnosticBudget::libraryDefault(),
                                     run->reporter);
    run->parsed = tree.has_value();
    if (!run->parsed) return run;

    LirBuilder builder{*run->target};
    builder.addFunction(SymbolId{1});
    LirBlockId const entry = builder.createBlock();
    builder.beginBlock(entry);

    std::vector<AsmOperandBinding> bindings;
    for (auto const& in : binds) {
        AsmOperandBinding b;
        b.spelling  = in.spelling;
        b.regClass  = in.cls;
        b.widthBits = in.width;
        auto const ord = run->target->registerByName(in.physical);
        if (!ord.has_value()) {
            throw std::runtime_error{"target declares no register "
                                     + in.physical};
        }
        b.reg = makePhysicalReg(*ord, in.cls);
        bindings.push_back(std::move(b));
    }

    run->ok = lowerAsmTemplateToLirRun(*tree, *run->dialect, *run->target,
                                       bindings, builder, run->reporter);

    auto const retOp = run->target->opcodeByMnemonic("ret");
    if (!retOp.has_value()) throw std::runtime_error{"target has no `ret`"};
    builder.addReturn(*retOp, {});
    Lir lir = std::move(builder).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    auto const mod = assemble(lir, *run->target, lirToMir, run->asmReporter);
    if (mod.functions.size() == 1) run->bytes = mod.functions[0].bytes;
    return run;
}

[[nodiscard]] std::shared_ptr<TargetSchema> shippedTarget() {
    auto t = TargetSchema::loadShipped(kTarget);
    if (!t.has_value()) throw std::runtime_error{"cannot load shipped arm64"};
    return *t;
}

// Two operands, each with its own CLASS and WIDTH — the shape every probe in
// this file needs and the one `test_asm_arm64_fp_dialect_rows.cpp`'s helpers
// deliberately do not have (theirs are class-uniform by construction).
[[nodiscard]] std::unique_ptr<Run>
run2(std::string_view templateText,
     LirRegClass dstCls, std::uint32_t dstWidth, std::string dstReg,
     LirRegClass srcCls, std::uint32_t srcWidth, std::string srcReg,
     std::shared_ptr<GrammarSchema> dialect = nullptr,
     std::shared_ptr<TargetSchema>  target  = nullptr) {
    return runOn(dialect ? std::move(dialect) : loadDialect(),
                 target ? std::move(target) : shippedTarget(), templateText,
                 {Bind{"%0", std::move(dstReg), dstCls, dstWidth},
                  Bind{"%1", std::move(srcReg), srcCls, srcWidth}});
}

// ── THE MEASURED WORDS ─────────────────────────────────────────────────────
//
// ✔GNU as 2.42 AND clang 18.1.3, agreeing on every one. `%0`->v0/x0, `%1`->
// v1/x1, destination-first operand order, so these are the words both
// assemblers produce for the spelling named in the comment.
constexpr std::uint32_t kFmovXfromD = 0x9E660020u;  // fmov x0, d1
constexpr std::uint32_t kFmovDfromX = 0x9E670020u;  // fmov d0, x1
constexpr std::uint32_t kFmovWfromS = 0x1E260020u;  // fmov w0, s1
constexpr std::uint32_t kFmovSfromW = 0x1E270020u;  // fmov s0, w1
constexpr std::uint32_t kFmovDD     = 0x1E604020u;  // fmov d0, d1 (control)
constexpr std::uint32_t kFmovSS     = 0x1E204020u;  // fmov s0, s1 (control)

constexpr std::uint32_t kFcvtzsWS = 0x1E380020u;  // fcvtzs w0, s1
constexpr std::uint32_t kFcvtzsWD = 0x1E780020u;  // fcvtzs w0, d1
constexpr std::uint32_t kFcvtzsXS = 0x9E380020u;  // fcvtzs x0, s1
constexpr std::uint32_t kFcvtzsXD = 0x9E780020u;  // fcvtzs x0, d1
constexpr std::uint32_t kFcvtzuWS = 0x1E390020u;  // fcvtzu w0, s1
constexpr std::uint32_t kFcvtzuXD = 0x9E790020u;  // fcvtzu x0, d1
constexpr std::uint32_t kScvtfSW  = 0x1E220020u;  // scvtf s0, w1
constexpr std::uint32_t kScvtfSX  = 0x9E220020u;  // scvtf s0, x1
constexpr std::uint32_t kScvtfDW  = 0x1E620020u;  // scvtf d0, w1
constexpr std::uint32_t kScvtfDX  = 0x9E620020u;  // scvtf d0, x1
constexpr std::uint32_t kUcvtfSX  = 0x9E230020u;  // ucvtf s0, x1
constexpr std::uint32_t kUcvtfDW  = 0x1E630020u;  // ucvtf d0, w1
constexpr std::uint32_t kFcvtSD   = 0x1E624020u;  // fcvt s0, d1
constexpr std::uint32_t kFcvtDS   = 0x1E22C020u;  // fcvt d0, s1

// ★★★ THE FOUR HALF-PRECISION `fcvt` WORDS, closed 2026-09-02 (cycle P55, lane
// fp) — [[D-TARGET-ARM64-FP16-CONVERSION-FORMS-UNDECLARED]]. ✔MEASURED, GNU as
// 2.42 AND clang 18.1.3 probed SEPARATELY and agreeing on all 37 probes of that
// run, every word read back out of an assembled object with
// `aarch64-linux-gnu-objdump`.
//
// ⚠ EACH BASE IS RE-DERIVED FROM **THREE** INDEPENDENT (Rd,Rn) PLACEMENTS, not
// from one low-register probe: `h3,s7` = 0x1E23C0E3 and `h29,s16` = 0x1E23C21D;
// `h3,d7` = 0x1E63C0E3 and `h16,d29` = 0x1E63C3B0; `s3,h7` = 0x1EE240E3 and
// `s29,h16` = 0x1EE2421D; `d3,h7` = 0x1EE2C0E3 and `d16,h29` = 0x1EE2C3B0. A
// field-placement error cannot hide behind a single Rd=0/Rn=1 probe, which is
// the shape the `fcvtzs x16, s29` silent miscompile wore.
constexpr std::uint32_t kFcvtHS   = 0x1E23C020u;  // fcvt h0, s1
constexpr std::uint32_t kFcvtHD   = 0x1E63C020u;  // fcvt h0, d1
constexpr std::uint32_t kFcvtSH   = 0x1EE24020u;  // fcvt s0, h1
constexpr std::uint32_t kFcvtDH   = 0x1EE2C020u;  // fcvt d0, h1
// The Rd=3 / Rn=7 placements of the same four.
constexpr std::uint32_t kFcvtHS37 = 0x1E23C0E3u;  // fcvt h3, s7
constexpr std::uint32_t kFcvtHD37 = 0x1E63C0E3u;  // fcvt h3, d7
constexpr std::uint32_t kFcvtSH37 = 0x1EE240E3u;  // fcvt s3, h7
constexpr std::uint32_t kFcvtDH37 = 0x1EE2C0E3u;  // fcvt d3, h7

constexpr std::uint32_t kCnt8B    = 0x0E205820u;  // cnt  v0.8b,  v1.8b
constexpr std::uint32_t kCnt16B   = 0x4E205820u;  // cnt  v0.16b, v1.16b
constexpr std::uint32_t kAddv8B   = 0x0E31B820u;  // addv b0,     v1.8b
constexpr std::uint32_t kAddv16B  = 0x4E31B820u;  // addv b0,     v1.16b
// The ORR alias the assemblers spell `mov`. ✔gas 2.42 + clang 18.1.3 agree on
// the two canonical arrangements; clang ALONE accepts `.4h`/`.2s`/`.1d` (all
// 0x0EA11C20) and `.8h`/`.4s`/`.2d` (all 0x4EA11C20), which is why DSS takes
// them — the ORR is bitwise, so the lane size reaches no bit.
constexpr std::uint32_t kMov8B    = 0x0EA11C20u;  // mov  v0.8b,  v1.8b
constexpr std::uint32_t kMov16B   = 0x4EA11C20u;  // mov  v0.16b, v1.16b

// The FP memory forms, written with PHYSICAL registers because this dialect's
// memory production takes a register NAME and no dialect declares a
// placeholder inside its brackets — ✔MEASURED, `ldr %d0, [%1]` and the
// INTEGER control `ldr %x0, [%1]` fail identically at the parser, so the limit
// is the memory form's and has nothing to do with the register class.
constexpr std::uint32_t kLdrD    = 0xFD400420u;  // ldr  d0, [x1, #8]
constexpr std::uint32_t kLdrS    = 0xBD400420u;  // ldr  s0, [x1, #4]
constexpr std::uint32_t kStrD    = 0xFD000420u;  // str  d0, [x1, #8]
constexpr std::uint32_t kLdurD   = 0xFC5F8020u;  // ldur d0, [x1, #-8]
constexpr std::uint32_t kLdrX    = 0xF9400420u;  // ldr  x0, [x1, #8]  (control)

// The `ret` the harness appends after every template. Seeing it as the FIRST
// word is exactly "the template emitted nothing".
constexpr std::uint32_t kRet     = 0xD65F03C0u;

[[nodiscard]] std::unique_ptr<Run> runBare(std::string_view templateText) {
    return runOn(loadDialect(), shippedTarget(), templateText, {});
}

} // namespace

// ── (1) THE REGISTER-CLASS ELECTION AXIS ───────────────────────────────────
//
// ★★★ ONE DIALECT ROW, THREE TARGET OPCODES, AND THE CLASS DECIDES. Before this
// axis the row could name only `fmov` (naming all three was refused as
// ambiguous, because `[reg]` at width 64 is all three) and `fmov x0, d1` then
// elected the fpr→fpr diagonal, to be caught one tier later by the encoder.
TEST(AsmArm64ConversionRows, CrossFileFmovElectsByRegisterClass) {
    struct Case {
        char const*   text;
        LirRegClass   dstCls;
        std::uint32_t dstW;
        char const*   dstReg;
        LirRegClass   srcCls;
        std::uint32_t srcW;
        char const*   srcReg;
        std::uint32_t want;
    } const cases[] = {
        {"fmov %x0, %d1\n", LirRegClass::GPR, 64, "x0",
                            LirRegClass::FPR, 64, "v1", kFmovXfromD},
        {"fmov %d0, %x1\n", LirRegClass::FPR, 64, "v0",
                            LirRegClass::GPR, 64, "x1", kFmovDfromX},
        {"fmov %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                            LirRegClass::FPR, 32, "v1", kFmovWfromS},
        {"fmov %s0, %w1\n", LirRegClass::FPR, 32, "v0",
                            LirRegClass::GPR, 32, "x1", kFmovSfromW},
        // The class-INTERNAL controls: the same spelling still elects `fmov`.
        {"fmov %d0, %d1\n", LirRegClass::FPR, 64, "v0",
                            LirRegClass::FPR, 64, "v1", kFmovDD},
        {"fmov %s0, %s1\n", LirRegClass::FPR, 32, "v0",
                            LirRegClass::FPR, 32, "v1", kFmovSS},
    };
    for (auto const& c : cases) {
        auto const r = run2(c.text, c.dstCls, c.dstW, c.dstReg,
                            c.srcCls, c.srcW, c.srcReg);
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.text << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.text << " emitted " << hex(r->bytes);
    }
}

// ★★ THE CLASS AXIS READS THE TARGET'S OWN PER-FIELD BANKS, so deleting one
// must change what elects. `fp_to_si`'s source WIRE declares `regClass: "fpr"`
// (its Rn is a float register while the opcode's own bank is `gpr`, because its
// RESULT is an integer); with that override gone the wire inherits `gpr`, an
// `fpr` source no longer matches, and `fcvtzs x0, d1` must stop encoding.
//
// ⚠ THE OBVIOUS MUTANT — deleting `movq_xmm_to_gpr`'s `resultRegClass` — WAS
// TRIED FIRST AND THE DOCUMENT REFUSED TO LOAD, which is `registerClassOps`
// declaring that opcode as the `fpr`→`gpr` cell and `validate()` holding the
// two statements to each other. That refusal is correct and is why the mutant
// moved to a wire the class table does not also constrain: a mutant that dies
// at load measures the loader, not the axis.
TEST(AsmArm64ConversionRows, RemovingASourceWireBankUnelectsTheConversion) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            std::size_t removed = 0;
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", std::string{}) != "fp_to_si") continue;
                for (auto& v : op.at("encoding").at("variants")) {
                    for (auto& w : v.at("wires")) {
                        if (w.contains("regClass")) {
                            w.erase("regClass");
                            ++removed;
                        }
                    }
                }
            }
            if (removed != 2) {
                throw std::runtime_error{
                    "expected two `fp_to_si` wires declaring regClass, found "
                    + std::to_string(removed)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing bank declaration";

    auto const r = run2("fcvtzs %x0, %d1\n", LirRegClass::GPR, 64, "x0",
                        LirRegClass::FPR, 64, "v1", nullptr, *mutant);
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_NE(firstWord(r->bytes), kFcvtzsXD)
        << "the conversion word survived the deletion of the source-bank "
           "declaration that elects it — the election is not reading the "
           "target: " << hex(r->bytes);

    // The CONTROL, on the SAME mutant: `fcvtzs %w0, %s1` goes through
    // `fp_to_si32`, whose wires were not touched, and must still encode. A
    // mutant that broke everything would prove nothing about the wire.
    auto const control = run2("fcvtzs %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                              LirRegClass::FPR, 32, "v1", nullptr, *mutant);
    ASSERT_TRUE(control->parsed) << messages(*control);
    EXPECT_TRUE(control->ok) << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kFcvtzsWS) << hex(control->bytes);
}

// ── (2) THE CONVERSIONS ────────────────────────────────────────────────────
//
// ★★★ THE REGRESSION RATCHET. ✔MEASURED before this cycle: `fcvtzs %w0, %s1`
// compiled rc=0 and emitted 0x9E3803B0 — `fcvtzs x16, s29`, the X form — while
// gas 2.42 and clang 18.1.3 both give the W form. Both registers are 32 bits so
// the width-honesty gate saw no disagreement; the derived 32 elected the
// source-width arm and that arm's word carries sf=1. This pin fails if the
// destination width ever stops routing the election.
TEST(AsmArm64ConversionRows, FcvtzsNarrowDestinationIsTheWForm) {
    auto const r = run2("fcvtzs %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                        LirRegClass::FPR, 32, "v1");
    ASSERT_TRUE(r->parsed) << messages(*r);
    ASSERT_TRUE(r->ok) << messages(*r);
    EXPECT_EQ(firstWord(r->bytes), kFcvtzsWS) << hex(r->bytes);
    EXPECT_NE(firstWord(r->bytes), kFcvtzsXS)
        << "the X form was emitted for a W destination — the exact silent "
           "wrong-width this row exists to close";
}

TEST(AsmArm64ConversionRows, EveryConversionWidthPairEncodesItsOwnWord) {
    struct Case {
        char const*   text;
        LirRegClass   dstCls;
        std::uint32_t dstW;
        char const*   dstReg;
        LirRegClass   srcCls;
        std::uint32_t srcW;
        char const*   srcReg;
        std::uint32_t want;
    } const cases[] = {
        {"fcvtzs %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                              LirRegClass::FPR, 32, "v1", kFcvtzsWS},
        {"fcvtzs %w0, %d1\n", LirRegClass::GPR, 32, "x0",
                              LirRegClass::FPR, 64, "v1", kFcvtzsWD},
        {"fcvtzs %x0, %s1\n", LirRegClass::GPR, 64, "x0",
                              LirRegClass::FPR, 32, "v1", kFcvtzsXS},
        {"fcvtzs %x0, %d1\n", LirRegClass::GPR, 64, "x0",
                              LirRegClass::FPR, 64, "v1", kFcvtzsXD},
        {"fcvtzu %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                              LirRegClass::FPR, 32, "v1", kFcvtzuWS},
        {"fcvtzu %x0, %d1\n", LirRegClass::GPR, 64, "x0",
                              LirRegClass::FPR, 64, "v1", kFcvtzuXD},
        {"scvtf %s0, %w1\n",  LirRegClass::FPR, 32, "v0",
                              LirRegClass::GPR, 32, "x1", kScvtfSW},
        {"scvtf %s0, %x1\n",  LirRegClass::FPR, 32, "v0",
                              LirRegClass::GPR, 64, "x1", kScvtfSX},
        {"scvtf %d0, %w1\n",  LirRegClass::FPR, 64, "v0",
                              LirRegClass::GPR, 32, "x1", kScvtfDW},
        {"scvtf %d0, %x1\n",  LirRegClass::FPR, 64, "v0",
                              LirRegClass::GPR, 64, "x1", kScvtfDX},
        {"ucvtf %s0, %x1\n",  LirRegClass::FPR, 32, "v0",
                              LirRegClass::GPR, 64, "x1", kUcvtfSX},
        {"ucvtf %d0, %w1\n",  LirRegClass::FPR, 64, "v0",
                              LirRegClass::GPR, 32, "x1", kUcvtfDW},
        {"fcvt %s0, %d1\n",   LirRegClass::FPR, 32, "v0",
                              LirRegClass::FPR, 64, "v1", kFcvtSD},
        {"fcvt %d0, %s1\n",   LirRegClass::FPR, 64, "v0",
                              LirRegClass::FPR, 32, "v1", kFcvtDS},
    };
    for (auto const& c : cases) {
        auto const r = run2(c.text, c.dstCls, c.dstW, c.dstReg,
                            c.srcCls, c.srcW, c.srcReg);
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.text << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.text << " emitted " << hex(r->bytes);
    }
}

// ★ THE SAME-WIDTH `fcvt` SPELLINGS ARE REFUSED, WHICH IS THE REFERENCES' OWN
// ANSWER — ✔MEASURED, gas 2.42 AND clang 18.1.3 both reject `fcvt s0, s1`,
// `fcvt d0, d1` and `fcvt h0, h1`. DSS refuses them for a STATED reason (no
// variant of EITHER `fcvt` opcode declares a destination width equal to its
// source width) rather than by accident, which is why the pin asserts the
// refusal rather than the message.
//
// ⚠ THE THIRD ARM AND THE WORD *EITHER* ARE NEW IN P55: this test read two
// spellings and named `fpcvt` alone while `fpcvt` was the only opcode the
// `fcvt` row could elect. `fpcvt_h` joined the row for
// [[D-TARGET-ARM64-FP16-CONVERSION-FORMS-UNDECLARED]], and a same-width
// spelling is now eliminated on BOTH candidates rather than on one — so a
// second opcode that forgot the property would be a hole this test could not
// see if it kept asking about the first. The width derivation is also no
// longer inferred from where `%s0` lands in the string, which was a coincidence
// of two spellings that happened to share a length.
TEST(AsmArm64ConversionRows, SameWidthFcvtIsRefusedAsBothReferencesDo) {
    struct Case {
        char const*   text;
        std::uint32_t width;
    } const cases[] = {
        {"fcvt %s0, %s1\n", 32},
        {"fcvt %d0, %d1\n", 64},
        {"fcvt %h0, %h1\n", 16},
    };
    for (auto const& c : cases) {
        auto const r = run2(c.text, LirRegClass::FPR, c.width, "v0",
                            LirRegClass::FPR, c.width, "v1");
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_FALSE(r->ok)
            << c.text << " was accepted, and both references refuse it";
        EXPECT_EQ(firstWord(r->bytes), kRet)
            << c.text << " emitted " << hex(r->bytes);
    }
}

// ★★★ THE FOUR HALF-PRECISION CONVERSION FORMS, EACH AT TWO REGISTER
// PLACEMENTS — [[D-TARGET-ARM64-FP16-CONVERSION-FORMS-UNDECLARED]].
//
// DSS refused all four and BOTH references assemble all four at the DEFAULT
// -march, so this was a BELOW-THE-UNION conformance defect on a hand-written-
// assembly surface. The words come from `fpcvt` (the `fcvt s, h` widen, on its
// free source-width-16 slot) and `fpcvt_h` (the other three); which opcode
// answers is not this test's subject — the BYTES are, and they are the same
// bytes either way.
//
// ⚠ THE SECOND PLACEMENT OF EACH FORM IS THE POINT OF THE SECOND HALF. A base
// word verified only at Rd=0/Rn=1 leaves the Rd and Rn fields unmeasured, which
// is exactly the shape the `fcvtzs x16, s29` silent miscompile wore: it was the
// REGISTER-BEARING word that was wrong. `%0`→v3 and `%1`→v7 give Rd=3, Rn=7.
TEST(AsmArm64ConversionRows, TheFp16ConversionFormsEncodeTheirOwnWords) {
    struct Case {
        char const*   text;
        std::uint32_t dstW;
        char const*   dstReg;
        std::uint32_t srcW;
        char const*   srcReg;
        std::uint32_t want;
    } const cases[] = {
        {"fcvt %h0, %s1\n", 16, "v0", 32, "v1", kFcvtHS},
        {"fcvt %h0, %d1\n", 16, "v0", 64, "v1", kFcvtHD},
        {"fcvt %s0, %h1\n", 32, "v0", 16, "v1", kFcvtSH},
        {"fcvt %d0, %h1\n", 64, "v0", 16, "v1", kFcvtDH},
        {"fcvt %h0, %s1\n", 16, "v3", 32, "v7", kFcvtHS37},
        {"fcvt %h0, %d1\n", 16, "v3", 64, "v7", kFcvtHD37},
        {"fcvt %s0, %h1\n", 32, "v3", 16, "v7", kFcvtSH37},
        {"fcvt %d0, %h1\n", 64, "v3", 16, "v7", kFcvtDH37},
    };
    for (auto const& c : cases) {
        auto const r = run2(c.text, LirRegClass::FPR, c.dstW, c.dstReg,
                            LirRegClass::FPR, c.srcW, c.srcReg);
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.text << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.text << " (" << c.dstReg << ", " << c.srcReg << ") emitted "
            << hex(r->bytes);
    }

    // ★ AND THE TWO C-REACHABLE ARMS STILL ANSWER WHAT THEY ALWAYS DID, so the
    // third variant added to `fpcvt` cannot be read as having moved them.
    for (auto const& [text, want] :
         {std::pair<char const*, std::uint32_t>{"fcvt %s0, %d1\n", kFcvtSD},
          std::pair<char const*, std::uint32_t>{"fcvt %d0, %s1\n", kFcvtDS}}) {
        auto const w = want == kFcvtSD;
        auto const c = run2(text, LirRegClass::FPR, w ? 32u : 64u, "v0",
                            LirRegClass::FPR, w ? 64u : 32u, "v1");
        ASSERT_TRUE(c->parsed && c->ok) << text << "\n" << messages(*c);
        EXPECT_EQ(firstWord(c->bytes), want) << text << hex(c->bytes);
    }
}

// ★★★ THE RED-ON-DISABLE MUTANT FOR THE HALF FORMS, REMOVE DIRECTION, OVER THE
// SHIPPED TARGET. Deleting `fpcvt_h` returns the target to its pre-P55 state
// for three of the four forms. The dialect row still names it, the lines still
// parse, and the ONLY acceptable outcome is a REFUSAL — emitting `fpcvt`'s
// float↔double word for a half spelling is precisely the silent wrong
// instruction this row exists to close.
//
// ⚠ THE CONTROL IS PRINTED BY NAME AND IT IS THE **HALF** ARM THAT SURVIVES:
// `fcvt s0, h1` lives on `fpcvt`, so it must still encode on the mutant. A
// control that only checked `fcvt s0, d1` would pass even if the mutation had
// deleted every h-view spelling, which is the failure mode a mutant most often
// hides behind.
TEST(AsmArm64ConversionRows, RemovingTheHalfConversionOpcodeRefuses) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto&       ops     = doc.at("opcodes");
            std::size_t removed = 0;
            for (auto it = ops.begin(); it != ops.end();) {
                if (it->value("mnemonic", std::string{}) == "fpcvt_h") {
                    it = ops.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            if (removed != 1) {
                throw std::runtime_error{
                    "expected exactly one `fpcvt_h` opcode in the shipped "
                    "arm64 target, found " + std::to_string(removed)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing opcode";

    struct Case {
        char const*   text;
        std::uint32_t dstW;
        std::uint32_t srcW;
        std::uint32_t neverEmit;
    } const gone[] = {
        // `fcvt h0, s1` shares its source width with `fcvt d0, s1`.
        {"fcvt %h0, %s1\n", 16, 32, kFcvtDS},
        // `fcvt h0, d1` shares its source width with `fcvt s0, d1`.
        {"fcvt %h0, %d1\n", 16, 64, kFcvtSD},
        // `fcvt d0, h1` shares its source width with `fcvt s0, h1`.
        {"fcvt %d0, %h1\n", 64, 16, kFcvtSH},
    };
    for (auto const& c : gone) {
        auto const r = run2(c.text, LirRegClass::FPR, c.dstW, "v0",
                            LirRegClass::FPR, c.srcW, "v1", nullptr, *mutant);
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_FALSE(r->ok)
            << c.text << " assembled after its opcode was deleted";
        EXPECT_NE(firstWord(r->bytes), c.neverEmit)
            << c.text << " fell through to the OTHER destination at this "
                         "source width — the destination width is not routing "
                         "the election: "
            << hex(r->bytes);
        EXPECT_EQ(firstWord(r->bytes), kRet)
            << c.text << " emitted " << hex(r->bytes);
    }

    // THE CONTROLS, named so a passing arm is visible rather than silent.
    struct Control {
        char const*   name;
        char const*   text;
        std::uint32_t dstW;
        std::uint32_t srcW;
        std::uint32_t want;
    } const controls[] = {
        {"fcvt s0, h1 (the half arm that lives on `fpcvt`)",
         "fcvt %s0, %h1\n", 32, 16, kFcvtSH},
        {"fcvt s0, d1 (the C-reachable narrow)",
         "fcvt %s0, %d1\n", 32, 64, kFcvtSD},
        {"fcvt d0, s1 (the C-reachable widen)",
         "fcvt %d0, %s1\n", 64, 32, kFcvtDS},
    };
    for (auto const& c : controls) {
        auto const r = run2(c.text, LirRegClass::FPR, c.dstW, "v0",
                            LirRegClass::FPR, c.srcW, "v1", nullptr, *mutant);
        ASSERT_TRUE(r->parsed) << c.name << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << "CONTROL " << c.name << " stopped assembling: "
                           << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << "CONTROL " << c.name << " emitted " << hex(r->bytes);
    }
}

// ★★★ THE OTHER HALF OF THE SAME GUARANTEE — delete the width-16 variant of
// `fpcvt` and `fcvt s0, h1` must REFUSE rather than fall onto `fpcvt_h`'s
// width-16 arm, which writes a DOUBLE. Both opcodes take `[reg]` at width 16
// and differ only in the destination width they declare, so this is the exact
// pair a destination-blind election would confuse — and the wrong answer would
// be a 64-bit write where 32 bits were asked for, in a register the caller
// reads as a float.
TEST(AsmArm64ConversionRows, RemovingFpcvtsHalfArmDoesNotFallOntoTheDoubleForm) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            std::size_t removed = 0;
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", std::string{}) != "fpcvt") continue;
                auto& vars = op.at("encoding").at("variants");
                for (auto it = vars.begin(); it != vars.end();) {
                    if (it->at("guard").value("width", 0u) == 16u) {
                        it = vars.erase(it);
                        ++removed;
                    } else {
                        ++it;
                    }
                }
            }
            if (removed != 1) {
                throw std::runtime_error{
                    "expected exactly one width-16 `fpcvt` variant in the "
                    "shipped arm64 target, found " + std::to_string(removed)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing variant";

    auto const r = run2("fcvt %s0, %h1\n", LirRegClass::FPR, 32, "v0",
                        LirRegClass::FPR, 16, "v1", nullptr, *mutant);
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_FALSE(r->ok)
        << "`fcvt s0, h1` assembled after the only variant declaring a 32-bit "
           "destination at source width 16 was deleted";
    EXPECT_NE(firstWord(r->bytes), kFcvtDH)
        << "the DOUBLE-destination word was emitted for a SINGLE destination: "
        << hex(r->bytes);
    EXPECT_NE(firstWord(r->bytes), kFcvtSH)
        << "the word survived the deletion of the variant that declares it — "
           "the byte pin is not reading the target: "
        << hex(r->bytes);

    // CONTROL, by name: the sibling at the same source width is untouched.
    auto const ctl = run2("fcvt %d0, %h1\n", LirRegClass::FPR, 64, "v0",
                          LirRegClass::FPR, 16, "v1", nullptr, *mutant);
    ASSERT_TRUE(ctl->parsed) << messages(*ctl);
    EXPECT_TRUE(ctl->ok)
        << "CONTROL `fcvt d0, h1` (fpcvt_h's width-16 arm) stopped "
           "assembling: " << messages(*ctl);
    EXPECT_EQ(firstWord(ctl->bytes), kFcvtDH)
        << "CONTROL `fcvt d0, h1` emitted " << hex(ctl->bytes);
}

// ★★★ THE ANTI-SILENT-MISCOMPILE MUTANT, AND IT RECREATES THE MEASURED SHAPE
// EXACTLY. Deleting the whole `fp_to_si32` opcode returns the target to its
// pre-P54 state — one `fcvtzs` family, X destination only. The dialect row
// still names it, `fcvtzs %w0, %s1` still parses, and the ONLY acceptable
// outcome is a REFUSAL: emitting `fp_to_si`'s X-form word here is precisely the
// silent wrong-width this row exists to close.
TEST(AsmArm64ConversionRows, RemovingTheNarrowConversionOpcodeRefuses) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto& ops = doc.at("opcodes");
            std::size_t removed = 0;
            for (auto it = ops.begin(); it != ops.end();) {
                if (it->value("mnemonic", std::string{}) == "fp_to_si32") {
                    it = ops.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            if (removed != 1) {
                throw std::runtime_error{
                    "expected exactly one `fp_to_si32` opcode in the shipped "
                    "arm64 target, found " + std::to_string(removed)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing opcode";

    auto const r = run2("fcvtzs %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                        LirRegClass::FPR, 32, "v1", nullptr, *mutant);
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_NE(firstWord(r->bytes), kFcvtzsXS)
        << "the X form was emitted for a W destination once the W opcode was "
           "removed — the destination width is not routing the election: "
        << hex(r->bytes);
    EXPECT_NE(firstWord(r->bytes), kFcvtzsWS)
        << "the W word survived the deletion of the opcode that declares it — "
           "the byte pin is not reading the target: "
        << hex(r->bytes);
}

// ★★ AND THE OTHER HALF OF THE SAME GUARANTEE: a target that simply FORGOT the
// `destWidth` key must not fall back to the old silence. Deleting it from
// `fp_to_si`'s variants leaves the opcode electable by shape and width, and
// `variantHonorsDeclaredDestWidth` must then refuse the two-width line.
TEST(AsmArm64ConversionRows, AForgottenDestWidthRefusesRatherThanGuesses) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            std::size_t removed = 0;
            for (auto& op : doc.at("opcodes")) {
                auto const m = op.value("mnemonic", std::string{});
                if (m != "fp_to_si" && m != "fp_to_si32") continue;
                for (auto& v : op.at("encoding").at("variants")) {
                    if (v.contains("destWidth")) {
                        v.erase("destWidth");
                        ++removed;
                    }
                }
            }
            if (removed != 4) {
                throw std::runtime_error{
                    "expected four `destWidth` declarations across "
                    "fp_to_si/fp_to_si32, found " + std::to_string(removed)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing declarations";

    // A 32-bit source into a 64-bit destination: the two widths DISAGREE, so
    // the gate must fire.
    auto const r = run2("fcvtzs %x0, %s1\n", LirRegClass::GPR, 64, "x0",
                        LirRegClass::FPR, 32, "v1", nullptr, *mutant);
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_FALSE(r->ok)
        << "a two-width instruction was encoded by a variant that declares no "
           "destination width — the honesty gate is not running";
    EXPECT_NE(firstWord(r->bytes), kFcvtzsXS) << hex(r->bytes);
}

// ── (3) THE LANE ARRANGEMENTS ──────────────────────────────────────────────
TEST(AsmArm64ConversionRows, LaneArrangementsCarryTheOperandWidth) {
    struct Case {
        char const*   text;
        std::uint32_t dstW;
        std::uint32_t srcW;
        std::uint32_t want;
    } const cases[] = {
        // The arrangement STATES the width; the binding's own width is the
        // operand's C type and is deliberately the WRONG number here, which is
        // what proves the suffix is what routes the election.
        {"cnt %0.8b, %1.8b\n",   64,  64,  kCnt8B},
        {"cnt %0.16b, %1.16b\n", 64,  64,  kCnt16B},
        {"addv %b0, %1.8b\n",    64,  64,  kAddv8B},
        {"addv %b0, %1.16b\n",   64,  64,  kAddv16B},
    };
    for (auto const& c : cases) {
        auto const r = run2(c.text, LirRegClass::FPR, c.dstW, "v0",
                            LirRegClass::FPR, c.srcW, "v1");
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.text << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.text << " emitted " << hex(r->bytes);
    }
}

// ★ AN ARRANGEMENT NAMES LANES OF A VECTOR REGISTER, so a GPR-bound operand
// wearing one is refused BY NAME rather than reaching the encoder's bank gate
// with a message about fields.
TEST(AsmArm64ConversionRows, AnArrangementOnAGprOperandIsRefusedByName) {
    auto const r = run2("cnt %0.8b, %1.8b\n", LirRegClass::GPR, 64, "x0",
                        LirRegClass::GPR, 64, "x1");
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_FALSE(r->ok) << "a GPR operand accepted a lane arrangement";
    EXPECT_NE(messages(*r).find("lane arrangement"), std::string::npos)
        << "the refusal does not name the arrangement: " << messages(*r);
}

// ★ AN ARRANGEMENT AT A LANE WIDTH THE OPCODE DOES NOT READ IS NOT SILENTLY
// DROPPED — `cnt v0.4h, v1.4h` is refused by BOTH references (✔MEASURED: CNT is
// byte-only) and must be refused here.
//
// ⚠⚠ THIS TEST'S NAME AND REASON BOTH CHANGED ON 2026-09-02 (cycle P54, lane
// `ae`, [[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]]) AND THE OLD
// ONES WERE *UNDECLARED ARRANGEMENT … the refusal has to come from the lookup*.
// `.4h` IS declared now — it has to be, because ✔MEASURED clang 18.1.3
// assembles `mov v0.4h, v1.4h` as 0x0EA11C20 and one working reference makes a
// spelling required. What refuses `cnt v0.4h` is no longer the arrangement
// TABLE but the ELECTION: `popcount_bytes` declares `laneBits: 8` on both ends,
// so a 16-bit lane matches no variant. The verdict is identical and its OWNER
// moved one tier, which is exactly the distinction the sibling test below
// exists to hold.
TEST(AsmArm64ConversionRows, AnUnreadableLaneWidthIsRefusedNotDropped) {
    auto const r = run2("cnt %0.4h, %1.4h\n", LirRegClass::FPR, 64, "v0",
                        LirRegClass::FPR, 64, "v1");
    ASSERT_TRUE(r->parsed)
        << "`.4h` no longer parses — it is a declared arrangement: "
        << messages(*r);
    EXPECT_FALSE(r->ok)
        << "a lane width this opcode does not read was accepted: "
        << hex(r->bytes);
    EXPECT_NE(firstWord(r->bytes), kCnt8B)
        << "`.4h` decayed to the `.8b` word — the lane width was dropped";
}

// ── (3b) THE LANE AXIS AT ELECTION ─────────────────────────────────────────
//
// [[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]], closed 2026-09-02
// (cycle P54, lane `ae`).
//
// ★★★ THE DEFECT, ✔MEASURED AT THE P54 BASE THROUGH THE REAL CLI, on BOTH
// surfaces and in BOTH directions, with gas 2.42 and clang 18.1.3 probed
// SEPARATELY and REJECTING every one of these lines:
//
//   scalar spelling → lane form:  `cnt d0, d1` emitted `cnt v0.8b, v1.8b`;
//     `mov d0, d1` emitted `mov v0.8b, v1.8b`; `mov v0, v1` emitted
//     `mov v0.16b, v1.16b`; `addv b0, d1` emitted `addv b0, v1.8b`.
//   lane spelling → SCALAR form:  `fadd v0.8b, v1.8b, v2.8b` emitted
//     **`fadd d0, d1, d2` (0x1E622820)** — a scalar double-precision add for a
//     byte-lane spelling — and `fmul`/`fsqrt`/`fmov`/`fneg`/`fcmp`/`fabs`
//     behaved the same way. THAT SECOND DIRECTION IS A SILENT WRONG ANSWER,
//     not an over-acceptance, and the anchor's own headline said the class was
//     acceptance-only. It was refuted by measuring it.
//
// Cause: `applyArrangement` wrote the arrangement's WIDTH and CLASS onto the
// operand and discarded the suffix, so `%0.16b`, `%q0` and a bare `%0` were one
// query at election. The lane width now survives to
// `asm_elect::variantAcceptsRegisterProfile`, which compares it against the
// target's own `wires[].lanes`/`laneBits` and `destLanes`/`destLaneBits`.
TEST(AsmArm64ConversionRows, AScalarSpellingNeverElectsALaneForm) {
    struct Case {
        char const*   text;
        std::uint32_t dstW;
        std::uint32_t srcW;
        std::uint32_t neverEmit;   // the word it used to emit
    } const cases[] = {
        {"cnt %q0, %q1\n",  128, 128, kCnt16B},
        {"cnt %d0, %d1\n",   64,  64, kCnt8B},
        {"addv %b0, %d1\n",   8,  64, kAddv8B},
        {"mov %q0, %q1\n",  128, 128, kMov16B},
        {"mov %d0, %d1\n",   64,  64, kMov8B},
        // The BARE reference: `registerNatural` derives 128 for an fpr operand
        // with no view letter, so `mov %0, %1` reached `move_bytes` too.
        {"mov %0, %1\n",    128, 128, kMov16B},
    };
    for (auto const& c : cases) {
        auto const r = run2(c.text, LirRegClass::FPR, c.dstW, "v0",
                            LirRegClass::FPR, c.srcW, "v1");
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_FALSE(r->ok)
            << c.text << " assembled, and gas 2.42 and clang 18.1.3 both "
                         "REFUSE it — a scalar spelling reached a lane form";
        EXPECT_NE(firstWord(r->bytes), c.neverEmit)
            << c.text << " emitted the lane form's word anyway: "
            << hex(r->bytes);
        EXPECT_EQ(firstWord(r->bytes), kRet)
            << c.text << " emitted " << hex(r->bytes);
    }
}

// ★★★ AND THE OTHER DIRECTION, WHICH IS THE ONE THAT WAS A WRONG ANSWER. Every
// spelling below is REFUSED by gas 2.42 and by clang 18.1.3 (✔MEASURED), and
// every one of them assembled at the P54 base into the SCALAR instruction whose
// word is named beside it.
TEST(AsmArm64ConversionRows, ALaneArrangementNeverElectsAScalarForm) {
    struct Case {
        char const*   text;
        std::uint32_t neverEmit;   // the SCALAR word it used to emit
    } const cases[] = {
        {"fmov %0.8b, %1.8b\n",  0x1E604020u},  // fmov d0, d1
        {"fneg %0.8b, %1.8b\n",  0x1E614020u},  // fneg d0, d1
        {"fsqrt %0.8b, %1.8b\n", 0x1E61C020u},  // fsqrt d0, d1
        {"fabs %0.8b, %1.8b\n",  0x1E60C020u},  // fabs d0, d1
        {"fcmp %0.8b, %1.8b\n",  0x1E612000u},  // fcmp d0, d1
    };
    for (auto const& c : cases) {
        auto const r = run2(c.text, LirRegClass::FPR, 64, "v0",
                            LirRegClass::FPR, 64, "v1");
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_FALSE(r->ok)
            << c.text << " assembled, and both references REFUSE it — a lane "
                         "arrangement reached a SCALAR instruction";
        EXPECT_NE(firstWord(r->bytes), c.neverEmit)
            << c.text << " emitted the scalar word: " << hex(r->bytes);
    }
    // The three-operand arm, which `run2` cannot shape: `fadd` is the spelling
    // whose measured emission — `fadd d0, d1, d2` for `fadd v0.8b, v1.8b,
    // v2.8b` — is the sharpest statement of what this axis prevents.
    auto const fadd = runOn(loadDialect(), shippedTarget(),
                            "fadd %0.8b, %1.8b, %2.8b\n",
                            {Bind{"%0", "v0", LirRegClass::FPR, 64},
                             Bind{"%1", "v1", LirRegClass::FPR, 64},
                             Bind{"%2", "v2", LirRegClass::FPR, 64}});
    ASSERT_TRUE(fadd->parsed) << messages(*fadd);
    EXPECT_FALSE(fadd->ok)
        << "`fadd %0.8b, %1.8b, %2.8b` assembled — both references refuse it";
    EXPECT_NE(firstWord(fadd->bytes), 0x1E622820u)
        << "the SCALAR DOUBLE ADD was emitted for a byte-lane spelling: "
        << hex(fadd->bytes);
}

// ★★★ THE TWO DIFFERENCES ARE DIFFERENT, AND A DESIGN THAT COULD EXPRESS ONLY
// ONE OF THEM WOULD NOT BE FINISHED. `cnt %q0, %q1` must REFUSE (a scalar
// spelling on a field that reads lanes) while `mov %0.4h, %1.4h` must ACCEPT (a
// lane spelling at a lane width the encoding does not read) — and the two are
// separated by the presence key and its refinement respectively.
//
// ✔MEASURED, and it is the ONE gas-vs-clang disagreement in this family: gas
// 2.42 REFUSES `mov v0.4h, v1.4h` while clang 18.1.3 assembles it as
// 0x0EA11C20, the same word as `.8b`, because the ORR alias is bitwise. One
// working reference makes it required. `cnt v0.4h, v1.4h` is refused by BOTH,
// and stays refused here, because CNT reads eight-bit lanes and says so.
TEST(AsmArm64ConversionRows, TheLaneWidthIsReadOnlyWhereTheEncodingReadsIt) {
    struct Case {
        char const*   text;
        std::uint32_t width;
        bool          accepts;
        std::uint32_t want;   // meaningful when `accepts`
    } const cases[] = {
        // move_bytes declares `lanes` with NO `laneBits`: every arrangement of
        // the right total width elects, and they are one word each.
        {"mov %0.8b, %1.8b\n",     64, true,  kMov8B},
        {"mov %0.4h, %1.4h\n",     64, true,  kMov8B},
        {"mov %0.2s, %1.2s\n",     64, true,  kMov8B},
        {"mov %0.1d, %1.1d\n",     64, true,  kMov8B},
        {"mov %0.16b, %1.16b\n",  128, true,  kMov16B},
        {"mov %0.8h, %1.8h\n",    128, true,  kMov16B},
        {"mov %0.4s, %1.4s\n",    128, true,  kMov16B},
        {"mov %0.2d, %1.2d\n",    128, true,  kMov16B},
        // popcount_bytes declares `laneBits: 8`: only the byte arrangements.
        {"cnt %0.8b, %1.8b\n",     64, true,  kCnt8B},
        {"cnt %0.16b, %1.16b\n",  128, true,  kCnt16B},
        {"cnt %0.4h, %1.4h\n",     64, false, 0},
        {"cnt %0.2s, %1.2s\n",     64, false, 0},
        {"cnt %0.8h, %1.8h\n",    128, false, 0},
        {"cnt %0.2d, %1.2d\n",    128, false, 0},
    };
    for (auto const& c : cases) {
        auto const r = run2(c.text, LirRegClass::FPR, c.width, "v0",
                            LirRegClass::FPR, c.width, "v1");
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        if (c.accepts) {
            EXPECT_TRUE(r->ok) << c.text << "\n" << messages(*r);
            EXPECT_EQ(firstWord(r->bytes), c.want)
                << c.text << " emitted " << hex(r->bytes);
        } else {
            EXPECT_FALSE(r->ok)
                << c.text << " assembled, and both references REFUSE it";
            EXPECT_EQ(firstWord(r->bytes), kRet)
                << c.text << " emitted " << hex(r->bytes);
        }
    }
}

// ★ THE REGISTER FIELDS STILL LAND WHERE THEY LAND. The eight arrangements the
// vector move takes collapse to two words, so a wrongly-wired Rn/Rm would be
// invisible in the table above — these two probes move BOTH register operands.
// ✔MEASURED, clang 18.1.3: `mov v3.4h, v7.4h` = 0x0EA71CE3 and
// `mov v3.2d, v7.2d` = 0x4EA71CE3.
TEST(AsmArm64ConversionRows, TheVectorMoveWiresItsSourceIntoBothFields) {
    auto const lo = run2("mov %0.4h, %1.4h\n", LirRegClass::FPR, 64, "v3",
                         LirRegClass::FPR, 64, "v7");
    ASSERT_TRUE(lo->parsed && lo->ok) << messages(*lo);
    EXPECT_EQ(firstWord(lo->bytes), 0x0EA71CE3u) << hex(lo->bytes);

    auto const hi = run2("mov %0.2d, %1.2d\n", LirRegClass::FPR, 128, "v3",
                         LirRegClass::FPR, 128, "v7");
    ASSERT_TRUE(hi->parsed && hi->ok) << messages(*hi);
    EXPECT_EQ(firstWord(hi->bytes), 0x4EA71CE3u) << hex(hi->bytes);
}

// ★★ RED-ON-DISABLE (1), REMOVE DIRECTION, OVER THE SHIPPED TARGET: take the
// LANE DECLARATION off `popcount_bytes` and the anchor's exact defect returns —
// `cnt %d0, %d1`, a spelling both references refuse, assembles the CNT word.
// ⚠ WITH A NAMED CONTROL ON THE SAME MUTANT, because a mutant that broke
// everything would prove nothing: `addv %b0, %1.8b` goes through
// `addlanes_bytes`, which this mutation does not touch, and must still encode.
TEST(AsmArm64ConversionRows, RemovingTheLaneDeclarationReopensTheScalarLeak) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            std::size_t stripped = 0;
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", std::string{}) != "popcount_bytes") {
                    continue;
                }
                for (auto& v : op.at("encoding").at("variants")) {
                    stripped += v.erase("destLanes");
                    stripped += v.erase("destLaneBits");
                    for (auto& w : v.at("wires")) {
                        stripped += w.erase("lanes");
                        stripped += w.erase("laneBits");
                    }
                }
            }
            if (stripped != 8) {
                throw std::runtime_error{
                    "expected eight lane keys on `popcount_bytes` (two "
                    "variants x {destLanes, destLaneBits, wire lanes, wire "
                    "laneBits}), stripped " + std::to_string(stripped)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing lane declaration";

    auto const leaked = run2("cnt %d0, %d1\n", LirRegClass::FPR, 64, "v0",
                             LirRegClass::FPR, 64, "v1", nullptr, *mutant);
    ASSERT_TRUE(leaked->parsed) << messages(*leaked);
    EXPECT_TRUE(leaked->ok)
        << "the scalar spelling stayed refused with the lane declaration "
           "deleted — the refusal is not reading the target: "
        << messages(*leaked);
    EXPECT_EQ(firstWord(leaked->bytes), kCnt8B)
        << "the mutant did not reproduce the measured leak: "
        << hex(leaked->bytes);

    auto const control = run2("addv %b0, %1.8b\n", LirRegClass::FPR, 8, "v0",
                              LirRegClass::FPR, 64, "v1", nullptr, *mutant);
    ASSERT_TRUE(control->parsed) << messages(*control);
    EXPECT_TRUE(control->ok)
        << "CONTROL `addv %b0, %1.8b` (opcode `addlanes_bytes`, untouched by "
           "this mutation) also broke: " << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kAddv8B) << hex(control->bytes);
}

// ★★ RED-ON-DISABLE (2): take only the REFINEMENT off — `laneBits` goes, the
// `lanes` presence key stays — and CNT starts accepting a lane width it does
// not read. This is the half `mov v0.4h` needs to keep, so the two keys are
// pinned by two different mutants rather than by one.
TEST(AsmArm64ConversionRows, RemovingTheLaneWidthWidensWhatCntAccepts) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            std::size_t stripped = 0;
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", std::string{}) != "popcount_bytes") {
                    continue;
                }
                for (auto& v : op.at("encoding").at("variants")) {
                    stripped += v.erase("destLaneBits");
                    for (auto& w : v.at("wires")) {
                        stripped += w.erase("laneBits");
                    }
                }
            }
            if (stripped != 4) {
                throw std::runtime_error{
                    "expected four `laneBits` keys on `popcount_bytes`, "
                    "stripped " + std::to_string(stripped)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load";

    auto const widened = run2("cnt %0.4h, %1.4h\n", LirRegClass::FPR, 64, "v0",
                              LirRegClass::FPR, 64, "v1", nullptr, *mutant);
    ASSERT_TRUE(widened->parsed) << messages(*widened);
    EXPECT_TRUE(widened->ok)
        << "`.4h` stayed refused with CNT's lane WIDTH deleted — the "
           "refinement is not being read: " << messages(*widened);
    EXPECT_EQ(firstWord(widened->bytes), kCnt8B) << hex(widened->bytes);

    // CONTROL: the byte arrangement this opcode really does read still encodes,
    // so the mutation narrowed nothing.
    auto const control = run2("cnt %0.8b, %1.8b\n", LirRegClass::FPR, 64, "v0",
                              LirRegClass::FPR, 64, "v1", nullptr, *mutant);
    ASSERT_TRUE(control->parsed) << messages(*control);
    EXPECT_TRUE(control->ok)
        << "CONTROL `cnt %0.8b, %1.8b` also broke: " << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kCnt8B) << hex(control->bytes);
}

// ★★ RED-ON-DISABLE (3), OVER THE SHIPPED DIALECT: `laneBits` is a REQUIRED key
// on an arrangement row, so removing one must stop the document LOADING. A
// defaulted lane width would be a number the author never wrote, keying an
// election that eliminates candidates.
TEST(AsmArm64ConversionRows, AnArrangementRowWithoutALaneWidthDoesNotLoad) {
    auto const broken = mutateShippedDialectDoc([](nlohmann::json& doc) {
        auto& rows = doc.at("assembly").at("registerArrangements");
        std::size_t stripped = 0;
        for (auto& row : rows) {
            if (row.value("suffix", std::string{}) != ".8b") continue;
            stripped += row.erase("laneBits");
        }
        if (stripped != 1) {
            throw std::runtime_error{
                "the shipped `.8b` arrangement row declares no `laneBits` — "
                "the pin below would be vacuous"};
        }
    });
    EXPECT_FALSE(broken.has_value())
        << "the dialect loaded with an arrangement row that states no lane "
           "width — the key is not required";
}

// ★★ THE THREE COHERENCE RULES ON THE NEW KEYS, EACH EXERCISED. These mutants
// ADD rather than remove, and that is correct for a LOADER-REFUSAL pin: there
// is no way to remove your way into a malformed document, and the subject here
// is the loader's verdict rather than a capability. Each aims at a shape that
// reads as a guarantee and is none — the coherence family `resultRegClass` and
// `destWidth` already belong to.
TEST(AsmArm64ConversionRows, LaneKeysWithNothingToGovernAreRefusedAtLoad) {
    struct Case {
        char const* what;
        void (*mutate)(nlohmann::json&);
    } const cases[] = {
        // (a) a lane WIDTH on a field that reads a scalar.
        {"`laneBits` without `lanes`",
         [](nlohmann::json& doc) {
             for (auto& op : doc.at("opcodes")) {
                 if (op.value("mnemonic", std::string{}) != "fadd") continue;
                 auto& w = op.at("encoding").at("variants")[0].at("wires")[0];
                 w["laneBits"] = 8;
                 return;
             }
             throw std::runtime_error{"arm64 declares no `fadd` row"};
         }},
        // (b) a lane shape on a variant that writes no destination.
        {"`destLanes` without `resultSlot`",
         [](nlohmann::json& doc) {
             for (auto& op : doc.at("opcodes")) {
                 if (op.value("mnemonic", std::string{}) != "fcmp") continue;
                 auto& v = op.at("encoding").at("variants")[0];
                 if (v.contains("resultSlot")) {
                     throw std::runtime_error{
                         "`fcmp` grew a `resultSlot` — aim this mutant at a "
                         "variant that still writes no destination"};
                 }
                 v["destLanes"] = true;
                 return;
             }
             throw std::runtime_error{"arm64 declares no `fcmp` row"};
         }},
        // (c) a lane width that is not a whole number of lanes of the width it
        //     views — 16-bit lanes cannot tile a 64-bit... they can; 48 cannot.
        {"a `laneBits` that does not divide the guard width",
         [](nlohmann::json& doc) {
             for (auto& op : doc.at("opcodes")) {
                 if (op.value("mnemonic", std::string{}) != "popcount_bytes") {
                     continue;
                 }
                 auto& w = op.at("encoding").at("variants")[0].at("wires")[0];
                 w["laneBits"] = 48;   // 64 % 48 != 0
                 return;
             }
             throw std::runtime_error{"arm64 declares no `popcount_bytes` row"};
         }},
    };
    for (auto const& c : cases) {
        auto const broken = test_support::mutateShippedTargetSchemaDoc(
            kTarget, c.mutate);
        EXPECT_FALSE(broken.has_value())
            << "the target loaded with " << c.what
            << " — a declaration that governs nothing reads as a guarantee "
               "and is none, and election ELIMINATES candidates on this key";
    }
}

// ★★ THE fp16 POSTURE, WHICH IS PER-INSTRUCTION AND NOT PER-WIDTH.
// ✔MEASURED 2026-09-02, gas 2.42 and clang 18.1.3 probed separately and
// agreeing: `fmov h0, h1` is REJECTED by both at the default -march (FP16
// ARITHMETIC is `FEAT_FP16`), while the fp16 CONVERSIONS are base ARMv8-A and
// both assemble them — `fcvt h0, s1` = 0x1E23C020, `fcvt s0, h1` = 0x1EE24020.
// `arm64.target.json` carried a width-16 `fmov` arm that made `fmov h0, h1`
// assemble rc=0 to 0x1EE04020 (✔MEASURED at the P54 base), i.e. DSS accepted a
// program NEITHER reference accepts; the arm is deleted.
// ⚠ THE CONVERSION HALF WAS A SEPARATE GAP AND IT IS CLOSED (2026-09-02, cycle
// P55, lane fp): DSS was BELOW the union on the four `fcvt` h-forms, which
// needed a second opcode because two variants of one opcode may not share an
// operand shape at one width. `fpcvt_h` is that opcode and
// `TheFp16ConversionFormsEncodeTheirOwnWords` pins all four words at two
// register placements each — [[D-TARGET-ARM64-FP16-CONVERSION-FORMS-UNDECLARED]].
// ⚠⚠ THE SENTENCE ABOVE THAT CALLS THE CONVERSIONS *base ARMv8-A* IS TRUE OF
// FCVT AND ONLY OF FCVT, ✔RE-MEASURED in P55: the fp16 INTEGER conversions are
// `FEAT_FP16` and both references refuse them at the default -march
// (`fcvtzs w0,h1`, `fcvtzs x0,h1`, `scvtf h0,w1`, `scvtf h0,x1`, `fcvtns w0,h1`,
// `fcvtas w0,h1`), so the `h` view stays unsayable on every `fcvtzs`/`fcvtzu`/
// `scvtf`/`ucvtf` and rounding row — declaring it there would be ABOVE the
// union exactly as the deleted `fmov` arm was.
TEST(AsmArm64ConversionRows, TheFp16MoveIsRefusedBecauseBothReferencesAre) {
    auto const r = run2("fmov %h0, %h1\n", LirRegClass::FPR, 16, "v0",
                        LirRegClass::FPR, 16, "v1");
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_FALSE(r->ok)
        << "`fmov h0, h1` assembled, and gas 2.42 and clang 18.1.3 both refuse "
           "it at the default -march";
    EXPECT_NE(firstWord(r->bytes), 0x1EE04020u)
        << "the FEAT_FP16 word was emitted: " << hex(r->bytes);

    // And the CONTROLS, so the deletion is not read as "fmov stopped working".
    for (auto const& [text, want] :
         {std::pair<char const*, std::uint32_t>{"fmov %d0, %d1\n", kFmovDD},
          std::pair<char const*, std::uint32_t>{"fmov %s0, %s1\n", kFmovSS}}) {
        auto const c = run2(text, LirRegClass::FPR,
                            want == kFmovDD ? 64u : 32u, "v0",
                            LirRegClass::FPR,
                            want == kFmovDD ? 64u : 32u, "v1");
        ASSERT_TRUE(c->parsed && c->ok) << text << "\n" << messages(*c);
        EXPECT_EQ(firstWord(c->bytes), want) << text << hex(c->bytes);
    }
}

// ★ TWO WIDTH VIEWS ON ONE OPERAND IS REFUSED RATHER THAN ORDERED: `%d0.8b`
// states 64 twice by coincidence and `%s0.8b` states two different widths, and
// which one answered would be decided by the order the two overrides run in.
TEST(AsmArm64ConversionRows, AViewLetterBesideAnArrangementIsRefused) {
    auto const r = run2("cnt %d0.8b, %1.8b\n", LirRegClass::FPR, 64, "v0",
                        LirRegClass::FPR, 64, "v1");
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_FALSE(r->ok) << "a view letter and an arrangement were both applied";
    EXPECT_NE(messages(*r).find("TWICE"), std::string::npos)
        << "the refusal does not say the width was stated twice: "
        << messages(*r);
}

// ★★ THE DIALECT MUTANT: delete `registerArrangements` and every arrangement
// spelling must stop being a register spelling. REMOVE direction, over the
// SHIPPED document.
TEST(AsmArm64ConversionRows, RemovingTheArrangementTableUnspellsTheSimdForms) {
    auto const dropped = mutateShippedDialectDoc([](nlohmann::json& doc) {
        auto& as = doc.at("assembly");
        if (!as.contains("registerArrangements")) {
            throw std::runtime_error{
                "the shipped dialect declares no `registerArrangements` — the "
                "pin below would be vacuous"};
        }
        as.erase("registerArrangements");
    });
    ASSERT_TRUE(dropped.has_value())
        << "the dialect did not load without its arrangement table";

    auto const r = runOn(*dropped, shippedTarget(), "cnt %0.8b, %1.8b\n",
                         {Bind{"%0", "v0", LirRegClass::FPR, 64},
                          Bind{"%1", "v1", LirRegClass::FPR, 64}});
    EXPECT_FALSE(r->parsed && r->ok)
        << "the arrangement form still lowered with its table deleted: "
        << hex(r->bytes);
    EXPECT_NE(firstWord(r->bytes), kCnt8B)
        << "the CNT word survived the deletion of the table that gives `.8b` a "
           "width — the byte pin is not reading the dialect: " << hex(r->bytes);
}

// ── (1b) THE FP MEMORY FORMS — the same class axis, over memory ─────────────
//
// `load_u` and `fldr_u` present BYTE-IDENTICAL operand shapes at identical
// widths and differ only in the data register's class, which is why one gas
// `ldr` row can now name both.
TEST(AsmArm64ConversionRows, FpMemoryFormsElectByDataRegisterClass) {
    struct Case { char const* text; std::uint32_t want; } const cases[] = {
        {"ldr d0, [x1, #8]\n",   kLdrD},
        {"ldr s0, [x1, #4]\n",   kLdrS},
        {"str d0, [x1, #8]\n",   kStrD},
        {"ldur d0, [x1, #-8]\n", kLdurD},
        {"ldr x0, [x1, #8]\n",   kLdrX},   // the INTEGER control
    };
    for (auto const& c : cases) {
        auto const r = runBare(c.text);
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.text << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.text << " emitted " << hex(r->bytes);
    }
}

// ── THE NEAR-MISS FENCE ────────────────────────────────────────────────────
//
// ⚠⚠ THIS FENCE WAS WRITTEN AGAINST TEN SPELLINGS AND NINE OF THEM ARE NOW
// DECLARED — corrected 2026-09-02 (cycle P54, lane av,
// D-ASM-ARM64-GAS-SPELLS-NO-ROUNDING-MODE-OR-VECTOR-MOVE). The sentence it
// stood on — *`arm64.target.json` declares no opcode whose template emits its
// bytes* — was TRUE when this file landed and stopped being true hours later,
// when the operator ruled the measured-but-undeclared vocabulary in. The
// paragraph is kept rather than deleted because its REASONING is what produced
// the fix: it recorded that `fcvtas`/`fcvtms`/`fcvtns`/`fcvtps` differ from
// `fcvtzs` ONLY in `rmode`, so binding them to `fp_to_si` would silently change
// a conversion's rounding mode — and that is exactly why each of the eight
// rounding conversions now has its OWN opcode pair rather than a variant on the
// toward-zero one. It also left the measured words *for the day someone
// declares them*, and every one was independently RE-MEASURED before use
// (fabs 0x1E60C020 · fsqrt 0x1E61C020 · fmax 0x1E624820 · fmadd 0x1F420C20 ·
// frinta 0x1E664020 · fcvtas 0x1E240020 · fcvtms 0x1E300020 · fcvtns
// 0x1E200020 · fcvtps 0x1E280020 · mov v0.16b,v1.16b 0x4EA11C20 — all ten
// confirmed against gas 2.42 and clang 18.1.3 a second time).
//
// ⇒ WHAT THE FENCE GUARDS NOW is the ONE spelling in the original list whose
// reason has not changed, plus its family: `fcsel` and `fccmp` need a CONDITION
// operand this dialect does not model. The positive pins for the nine that left
// live in `tests/asm/test_asm_arm64_rounding_dialect_rows.cpp`, which asserts
// each one's WORD rather than the absence of any word — a strictly stronger
// question than "is this spelling unknown".
TEST(AsmArm64ConversionRows, NearMissSpellingsStayRefused) {
    for (auto const* text : {"fcsel %d0, %d1\n", "fccmp %d0, %d1\n"}) {
        auto const r = run2(text, LirRegClass::FPR, 64, "v0",
                            LirRegClass::FPR, 64, "v1");
        EXPECT_FALSE(r->parsed && r->ok)
            << text << " assembled, and this target declares no opcode whose "
                       "template emits its bytes";
        // ⚠ THE BYTE STREAM IS NEVER EMPTY: the harness appends a `ret` after
        // the template, so `kRet` as the FIRST word is what "the template
        // emitted nothing" actually looks like. An `EXPECT(bytes.empty())`
        // here failed on every arm and would have been read as a product
        // defect; asserting the `ret` is the positive form of the same claim.
        EXPECT_EQ(firstWord(r->bytes), kRet)
            << text << " emitted " << hex(r->bytes);
    }
}

// ★★★ THE 128-BIT SIMD REGISTER MOVE NOW ENCODES, AND THIS ARM IS WHERE IT WAS
// PREDICTED. It asserted a REFUSAL when this file landed — *the target declares
// no ORR-based vector move* — which was true for exactly as long as that
// sentence was in the tree: lane `av` declared `move_bytes` the same day, on
// the operator's ruling, and `mov Vd.16b, Vn.16b` is now the alias for
// `orr Vd.16b, Vn.16b, Vn.16b` that gas and clang have always assembled.
// ⚠ WHAT SURVIVES UNCHANGED IS THE HALF THIS ARM REALLY OWNED: the ARRANGEMENT
// must still PARSE on the `mov` spelling. That is this file's subject — the
// lane-arrangement grammar — and it is asserted below without reference to what
// the target does or does not declare, so a grammar regression fails here even
// as the vocabulary keeps growing. The move's BYTES are pinned in
// `tests/asm/test_asm_arm64_rounding_dialect_rows.cpp`.
TEST(AsmArm64ConversionRows, TheSimdRegisterMoveParsesAndNowEncodes) {
    auto const r = run2("mov %0.16b, %1.16b\n", LirRegClass::FPR, 128, "v0",
                        LirRegClass::FPR, 128, "v1");
    EXPECT_TRUE(r->parsed)
        << "the arrangement no longer parses on the `mov` spelling: "
        << messages(*r);
    EXPECT_TRUE(r->ok) << messages(*r);
    EXPECT_EQ(firstWord(r->bytes), 0x4EA11C20u)
        << "the ORR alias's word: " << hex(r->bytes);
}

// ★★★ THE STANDING PREDICATE, WIDENED. Lane `ad` left this dialect with a test
// asking that every register class an `asmConstraints` letter can bind have
// SOME instruction row able to name it. The sharper question this cycle
// answers is about the CLASS PAIR: an opcode whose two ends live in different
// banks is unreachable unless election can see both, so every cross-class
// opcode the target declares must be named by some dialect row.
TEST(AsmArm64ConversionRows, EveryCrossClassOpcodeIsNamedByADialectRow) {
    auto const target  = shippedTarget();
    auto const dialect = loadDialect();
    auto const doc     = nlohmann::json::parse(dialectText());

    std::vector<std::string> named;
    for (auto const& row : doc.at("assembly").at("instructions")) {
        for (auto const& op : row.value("opcodes", nlohmann::json::array())) {
            named.push_back(op.get<std::string>());
        }
    }

    auto const targetDoc = [] {
        auto p = findShippedConfig(
            ShippedConfigLocator{kTarget, "targets", ".target.json", "target",
                                 DiagnosticCode::C_InvalidTargetName});
        if (!p.has_value()) throw std::runtime_error{"no shipped arm64 target"};
        std::ifstream in{*p};
        std::ostringstream buf;
        buf << in.rdbuf();
        return nlohmann::json::parse(buf.str());
    }();

    std::vector<std::string> unreachable;
    for (auto const& op : targetDoc.at("opcodes")) {
        if (!op.contains("encoding")) continue;
        auto const& enc = op.at("encoding");
        if (!enc.contains("registerClass") || !enc.contains("variants")) {
            continue;
        }
        auto const bank = enc.at("registerClass").get<std::string>();
        bool crossClass = false;
        for (auto const& v : enc.at("variants")) {
            if (v.contains("resultRegClass")
                && v.at("resultRegClass").get<std::string>() != bank) {
                crossClass = true;
            }
            for (auto const& w : v.value("wires", nlohmann::json::array())) {
                if (w.contains("regClass")
                    && w.at("regClass").get<std::string>() != bank) {
                    crossClass = true;
                }
            }
        }
        if (!crossClass) continue;
        auto const m = op.at("mnemonic").get<std::string>();
        if (std::find(named.begin(), named.end(), m) == named.end()) {
            unreachable.push_back(m);
        }
    }
    EXPECT_TRUE(unreachable.empty())
        << "the arm64 target declares cross-class opcodes no dialect row can "
           "name, so the class-election axis cannot reach them: "
        << [&] {
               std::string s;
               for (auto const& m : unreachable) { s += m; s += ' '; }
               return s;
           }();
}
