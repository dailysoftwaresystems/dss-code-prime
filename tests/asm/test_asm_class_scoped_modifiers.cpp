// ── CLASS-SCOPED WIDTH-VIEW MODIFIER LETTERS ──────────────────────────────
//
//     D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE (R5 + R8)
//
// ★★★ WHAT THIS FILE PINS. `assembly.templateModifiers` rows may scope a
// letter to ONE register class (`registerClass`), and the aarch64 gas document
// scopes all seven of its letters (`w`/`x` ⇒ gpr, `b`/`h`/`s`/`d`/`q` ⇒ fpr)
// while the x86-64 AT&T document scopes none. Both postures are MEASUREMENTS,
// not styles, and each direction of the class check is pinned here against the
// reference behaviour that requires it:
//
//   * ✔MEASURED 2026-09-01, gcc 13.3.0 AND clang 18.1.3 separately (clang
//     under `-fno-integrated-as`), `-O0` and `-O2`: an FP view letter on an
//     `"r"`-bound integer is a HARD ERROR under both (gcc: *invalid 'asm':
//     incompatible floating point / vector register operand for '%d'*; clang:
//     *invalid operand in inline asm*) — and the SAME error under both on an
//     `"m"`-bound operand. A unanimous refusal DSS adopts.
//   * ✔MEASURED in the same run: `%w0`/`%x0` on a `"w"`-bound double — gcc
//     renders `v0`, clang renders `d0`, both rc=0. Accepted by both, MEANING
//     different registers: the operator ruled the construct DISSOLVED (R8) —
//     class-scoped letters make it illegal, refused by name, listing the
//     letters the operand's class does declare.
//   * ✔MEASURED: the five FP letters on a `"w"`-bound double render
//     `b0`/`h0`/`s0`/`d0`/`q0` under both references — the positive half.
//   * ✔MEASURED on the OTHER port: x86-64 gcc ACCEPTS `%b0`/`%w0`/`%k0`/`%q0`
//     on an `"x"`-bound double and renders the bare `%xmm0`, while clang
//     refuses. Acceptance is decided by the disjunction, so the x86 letters
//     stay WIDTH-ONLY and `movsd %q1, %q0` over xmm operands must keep
//     encoding.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

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

// ★★★ THE CROSS-TIER TRIPWIRE `AssemblyConfig::kTemplateModifierWidthBits`
// PROMISES: every width the loader lets a letter declare must be a width
// `lirInstWidthBits` can actually state, or a declared letter would be carried
// to the instruction and read back as 64 — the silent wrong-width operation
// the whole surface exists to prevent. The set lives on `AssemblyConfig`
// (core), the flags live in `lir_node.hpp` (LIR), and core cannot include LIR
// — THIS file includes both, which is why the assert lives here.
namespace {
[[nodiscard]] constexpr bool everyDeclarableModifierWidthHasALirFlag() {
    for (auto const w : AssemblyConfig::kTemplateModifierWidthBits) {
        if (!lirInstWidthFlagForBits(w).has_value()) return false;
    }
    return true;
}
static_assert(everyDeclarableModifierWidthHasALirFlag(),
              "AssemblyConfig::kTemplateModifierWidthBits admits a width "
              "lirInstWidthFlagForBits cannot state — the loader would accept "
              "a letter whose width the instruction cannot carry");

struct ShippedPair {
    std::string_view dialect;   // the document STEM, for locating it
    std::string_view target;
};

constexpr ShippedPair kX86{"asm-x86_64-att", "x86_64"};
constexpr ShippedPair kArm{"asm-arm64-gas", "arm64"};

[[nodiscard]] std::string dialectText(std::string_view name) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{name, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        throw std::runtime_error{std::string{"cannot locate dialect "}
                                 + std::string{name}};
    }
    std::ifstream in{*pathR};
    if (!in) throw std::runtime_error{"cannot open dialect document"};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

[[nodiscard]] std::shared_ptr<GrammarSchema> loadDialect(std::string_view name) {
    auto g = GrammarSchema::loadFromText(dialectText(name), std::string{name});
    if (!g.has_value()) {
        std::string why;
        for (auto const& e : g.error()) why += e.path + ": " + e.message + "\n";
        throw std::runtime_error{"dialect did not load: " + why};
    }
    return *g;
}

// The red-on-disable door: the SHIPPED document, one key edited in memory —
// the `mutate_target_schema.hpp` posture applied to a dialect, exactly as
// `test_asm_template_to_lir.cpp` does.
struct MutatedDialect {
    std::shared_ptr<GrammarSchema> grammar;   // null ⇒ the document was refused
    std::vector<std::string>       loadErrors;
};

[[nodiscard]] MutatedDialect
loadDialectMutated(std::string_view name,
                   std::function<void(nlohmann::json&)> const& mutate) {
    auto doc = nlohmann::json::parse(dialectText(name));
    std::string const before = doc.dump();
    mutate(doc);
    if (doc.dump() == before) {
        throw std::runtime_error{
            "dialect mutation was a NO-OP — the \"mutant\" IS the shipped "
            "document and the pin consuming it asserts nothing"};
    }
    MutatedDialect out;
    auto g = GrammarSchema::loadFromText(doc.dump(), std::string{name});
    if (g.has_value()) {
        out.grammar = *g;
    } else {
        for (auto const& e : g.error()) {
            out.loadErrors.push_back(e.path + ": " + e.message);
        }
    }
    return out;
}

[[nodiscard]] std::string joined(std::vector<std::string> const& v) {
    std::string out;
    for (auto const& s : v) { out += s; out += '\n'; }
    return out;
}

// One binding, with every knob the class check reads: the register class, the
// operand FORM, and the immediate payload. `physical` empty is only legal for
// the `ImmInt` form, where the binding carries no register at all.
struct Binding {
    std::string       spelling;
    std::string       physical;
    LirRegClass       cls   = LirRegClass::GPR;
    std::uint32_t     width = 64;
    OperandKindFilter kind  = OperandKindFilter::Reg;
    bool              hasImmediate = false;
    std::int64_t      immediate    = 0;
};

struct Run {
    std::shared_ptr<GrammarSchema> dialect;
    std::shared_ptr<TargetSchema>  target;
    DiagnosticReporter             reporter;
    bool                           parsed = false;
    bool                           ok     = false;
    std::vector<std::uint8_t>      bytes;
};

[[nodiscard]] std::string messages(Run const& r) {
    std::string out;
    for (auto const& d : r.reporter.all()) { out += d.actual; out += '\n'; }
    return out;
}

// The emitted stream, for a failure message that says what was produced rather
// than only what was expected.
[[nodiscard]] std::string hex(std::vector<std::uint8_t> const& b) {
    std::string out;
    for (auto const v : b) out += std::format("{:02X} ", v);
    return out;
}

[[nodiscard]] std::unique_ptr<Run>
runOn(std::shared_ptr<GrammarSchema> dialect,
      std::shared_ptr<TargetSchema>  target,
      std::string_view templateText, std::vector<Binding> const& binds) {
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
        b.spelling    = in.spelling;
        b.regClass    = in.cls;
        b.widthBits   = in.width;
        b.operandKind = in.kind;
        b.hasImmediate = in.hasImmediate;
        b.value        = in.immediate;
        if (!in.physical.empty()) {
            auto const ord = run->target->registerByName(in.physical);
            if (!ord.has_value()) {
                throw std::runtime_error{"target declares no register "
                                         + in.physical};
            }
            b.reg = makePhysicalReg(*ord, in.cls);
        }
        bindings.push_back(std::move(b));
    }

    run->ok = lowerAsmTemplateToLirRun(*tree, *run->dialect, *run->target,
                                       bindings, builder, run->reporter);

    auto const retOp = run->target->opcodeByMnemonic("ret");
    if (!retOp.has_value()) throw std::runtime_error{"target has no `ret`"};
    builder.addReturn(*retOp, {});
    Lir lir = std::move(builder).finish();

    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto const mod = assemble(lir, *run->target, lirToMir, asmRep);
    if (mod.functions.size() == 1) run->bytes = mod.functions[0].bytes;
    return run;
}

[[nodiscard]] std::unique_ptr<Run>
run(ShippedPair const& p, std::string_view templateText,
    std::vector<Binding> const& binds) {
    auto targetR = TargetSchema::loadShipped(p.target);
    if (!targetR.has_value()) {
        throw std::runtime_error{"cannot load shipped target"};
    }
    return runOn(loadDialect(p.dialect), *targetR, templateText, binds);
}

// The class-refusal's fingerprint — asserted PRESENT on a mismatch and ABSENT
// on every legal shape, so a check that fired too widely or not at all is
// caught from both sides.
constexpr std::string_view kClassRefusal = "selects a width view through";

}  // namespace

// ══ THE POSITIVE HALF: EVERY FP LETTER IS DECLARED AND DECODES ════════════
//
// `%b0`/`%h0`/`%s0`/`%d0`/`%q0` on an FP-class operand LEX, PARSE and pass the
// class check — the reference renders `b0`..`q0` for exactly this shape. What
// happens NEXT differs by width (no arm64 FP mnemonic exists, so nothing here
// can encode — `c_inline_asm_fp_class_constraint` records that gap), but the
// class refusal must fire for NONE of them, and a parse failure would mean the
// letter was never composed into the lexer mode at all.
TEST(AsmClassScopedModifiers, EveryFpLetterParsesAndPassesTheClassCheckOnAnFpOperand) {
    for (auto const* letter : {"b", "h", "s", "d", "q"}) {
        auto const text = std::format("mov %{}0, %{}1\n", letter, letter);
        auto const r    = run(kArm, text,
                              {{"%0", "v0", LirRegClass::FPR, 64},
                               {"%1", "v1", LirRegClass::FPR, 64}});
        ASSERT_TRUE(r->parsed)
            << "%" << letter << "0 did not PARSE — the letter is not composed "
            << "into the template lexer mode:\n" << messages(*r);
        EXPECT_EQ(messages(*r).find(kClassRefusal), std::string::npos)
            << "%" << letter << "0 on an fpr operand tripped the CLASS check — "
            << "the letter is scoped to the wrong class:\n" << messages(*r);
    }
}

// The `q` letter states 128 bits, and the message naming 128 is what proves
// the letter's declared width arrived — a letter that silently decayed to 64
// would sail through on the `mov` this template writes.
//
// ⚠⚠ THE TIER THAT REFUSES IT MOVED ON 2026-09-02, AND SO DID THE SPELLING
// THIS ARM LOOKS FOR. It used to read *the one width whose consumption the
// pipeline refuses today, BY NAME, at the template translator's width-flag
// map*, and looked for `"128 bits"`. That map gained a `case 128:` in cycle
// P54 (D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME),
// because `arm64.target.json` grew width-128 SIMD arms and a lane arrangement
// can now reach them — so 128 is a width the translator carries.
//
// ⚠⚠ AND THEN THE SECOND HALF OF THAT REPLACEMENT WENT FALSE THE SAME DAY,
// WHICH IS THE FINDING RATHER THAN AN INCONVENIENCE. It read: *no arm64
// mnemonic takes a 128-bit operand through a `q` VIEW LETTER. The two 128-bit
// forms that exist are SIMD and are written with a lane ARRANGEMENT (`%0.16b`),
// so `%q0` still elects nothing.* That is a distinction THE ENGINE CANNOT MAKE:
// an arrangement is applied as a WIDTH and a CLASS and the suffix is then gone,
// so `%0.16b` and `%q0` reach `electOpcode` as the SAME query (fpr, 128). The
// sentence was true only while no width-128 variant existed on a spelling a
// view letter could reach, and it stopped being true the hour lane `av`
// declared the SIMD register move
// (D-ASM-ARM64-GAS-SPELLS-NO-ROUNDING-MODE-OR-VECTOR-MOVE). The whole class is
// anchored at [[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]] — DSS
// now accepts `mov q0, q1`, which GNU as 2.42 and clang 18.1.3 both REJECT.
//
// ⚠⚠ AND THE PARAGRAPH ABOVE PREDICTED ITS OWN THIRD REPLACEMENT, WHICH
// LANDED ON 2026-09-02 (cycle P54, lane `ae`). It says the `%0.16b` / `%q0`
// collapse *is a distinction THE ENGINE CANNOT MAKE*. The engine makes it now:
// an arrangement carries its LANE WIDTH into election and the target says, per
// field, whether that field reads its register as a vector of lanes
// ([[D-ASM-ARRANGEMENT-ERASED-TO-A-WIDTH-BEFORE-ELECTION]]). `move_bytes` reads
// lanes on both ends, a `q` VIEW LETTER states none, and `mov q0, q1` is
// therefore refused here exactly as GNU as 2.42 and clang 18.1.3 refuse it.
//
// ★ WHAT THIS ARM ACTUALLY OWNS IS THE LETTER'S WIDTH, and it is still measured
// in BYTES rather than in a message — over a width-128 instruction that is
// genuinely SCALAR, which is what a view letter names. ✔MEASURED, gas 2.42 and
// clang 18.1.3 agreeing: `ldr q0, [x1, #16]` = 0x3DC00420 against the 64-bit
// `ldr d0, [x1, #16]` = 0xFD400820. The binding width is 64 throughout, so ONLY
// the letter can produce the 128, and a letter that silently decayed to 64
// would elect `fldr_u`'s D arm and emit the second word.
TEST(AsmClassScopedModifiers, TheQLetterStatesTheFullVectorWidth) {
    auto const r = run(kArm, "ldr %q0, [x1, #16]\n",
                       {{"%0", "v0", LirRegClass::FPR, 64}});
    ASSERT_TRUE(r->parsed) << messages(*r);
    ASSERT_TRUE(r->ok) << messages(*r);
    ASSERT_GE(r->bytes.size(), 4u) << hex(r->bytes);
    auto const word = static_cast<std::uint32_t>(r->bytes[0])
                    | (static_cast<std::uint32_t>(r->bytes[1]) << 8)
                    | (static_cast<std::uint32_t>(r->bytes[2]) << 16)
                    | (static_cast<std::uint32_t>(r->bytes[3]) << 24);
    EXPECT_EQ(word, 0x3DC00420u)
        << "the `q` letter must state 128 and elect LDR Qt; 0xFD400820 would "
           "mean it decayed to the 64-bit view: " << hex(r->bytes);

    // ★ AND THE SPELLING THIS ARM USED TO DRIVE IS NOW REFUSED — the letter
    // still answers 128, and 128 on `move_bytes` is a LANE form no view letter
    // names. Both references refuse `mov q0, q1` (✔MEASURED).
    auto const laneForm = run(kArm, "mov %q0, %q1\n",
                              {{"%0", "v0", LirRegClass::FPR, 64},
                               {"%1", "v1", LirRegClass::FPR, 64}});
    ASSERT_TRUE(laneForm->parsed) << messages(*laneForm);
    EXPECT_FALSE(laneForm->ok)
        << "`mov q0, q1` assembled, and gas 2.42 and clang 18.1.3 both REJECT "
           "it";
}

// ══ R8: A GPR LETTER ON AN FP OPERAND IS REFUSED NAMING THE FP LETTERS ════
//
// gcc renders `v0`, clang renders `d0` — same program, two registers, no
// diagnostic from either. The operator ruled the construct dissolved: the
// refusal must name the letter, both classes, and the letters the operand's
// class DOES declare, so the author is handed the legal alphabet instead of a
// coin-flip meaning.
TEST(AsmClassScopedModifiers, AGprLetterOnAnFpOperandIsRefusedNamingTheFpLetters) {
    for (auto const* letter : {"w", "x"}) {
        auto const text = std::format("mov %{}0, %{}1\n", letter, letter);
        auto const r    = run(kArm, text,
                              {{"%0", "v0", LirRegClass::FPR, 64},
                               {"%1", "v1", LirRegClass::FPR, 64}});
        ASSERT_TRUE(r->parsed) << messages(*r);
        EXPECT_FALSE(r->ok)
            << "%" << letter << "0 on an fpr operand was accepted — the ruled "
            << "dissolution did not fire:\n" << messages(*r);
        auto const msg = messages(*r);
        EXPECT_NE(msg.find(kClassRefusal), std::string::npos) << msg;
        EXPECT_NE(msg.find("declares for class 'gpr'"), std::string::npos)
            << msg;
        EXPECT_NE(msg.find("lives in class 'fpr'"), std::string::npos) << msg;
        // The legal alphabet, with widths — all five FP letters, so the
        // author sees the whole vocabulary and not a sample.
        for (auto const* fp : {"'b' (8 bits)", "'h' (16 bits)",
                               "'s' (32 bits)", "'d' (64 bits)",
                               "'q' (128 bits)"}) {
            EXPECT_NE(msg.find(fp), std::string::npos)
                << "the refusal must list " << fp << ": " << msg;
        }
    }
}

// ══ THE UNANIMOUS DIRECTION: AN FP LETTER ON A GPR OPERAND ════════════════
//
// Both references hard-error this shape (`incompatible floating point /
// vector register operand`), so the refusal is conformance, not policy.
TEST(AsmClassScopedModifiers, AFpLetterOnAGprOperandIsRefusedNamingTheGprLetters) {
    for (auto const* letter : {"b", "h", "s", "d", "q"}) {
        auto const text = std::format("mov %{}0, %{}1\n", letter, letter);
        auto const r    = run(kArm, text,
                              {{"%0", "x0", LirRegClass::GPR, 64},
                               {"%1", "x1", LirRegClass::GPR, 64}});
        ASSERT_TRUE(r->parsed) << messages(*r);
        EXPECT_FALSE(r->ok) << "%" << letter << "0 on a gpr operand was "
                            << "accepted:\n" << messages(*r);
        auto const msg = messages(*r);
        EXPECT_NE(msg.find(kClassRefusal), std::string::npos) << msg;
        EXPECT_NE(msg.find("lives in class 'gpr'"), std::string::npos) << msg;
        EXPECT_NE(msg.find("'w' (32 bits)"), std::string::npos) << msg;
        EXPECT_NE(msg.find("'x' (64 bits)"), std::string::npos) << msg;
    }
}

// ══ THE FORM AXIS: MEMBASE IS CHECKED, IMMINT IS SKIPPED ══════════════════
//
// ✔MEASURED: `%d0` on an `"m"`-bound operand is the same hard error under
// both references as the `"r"` case — the binding's ADDRESS register is a
// GPR, and the class check runs on the BINDING's class whatever the template
// writes around it. The GPR letter on the same binding is accepted-and-
// -ignored by both references, which is the shipped letter-dies path.
TEST(AsmClassScopedModifiers, AFpLetterOnAMemoryBindingIsRefusedAndAGprLetterIsNot) {
    // `str w1, [x0]` — the measured reference shape. %0 is the "m" binding:
    // a GPR holding the operand's address.
    Binding mem{"%0", "x0", LirRegClass::GPR, 64, OperandKindFilter::MemBase};
    Binding val{"%1", "x1", LirRegClass::GPR, 32};

    auto const refused = run(kArm, "str %w1, %d0\n", {mem, val});
    ASSERT_TRUE(refused->parsed) << messages(*refused);
    EXPECT_FALSE(refused->ok) << messages(*refused);
    EXPECT_NE(messages(*refused).find(kClassRefusal), std::string::npos)
        << "an FP letter on a gpr-address memory binding must trip the class "
           "check, as both references do: " << messages(*refused);

    auto const accepted = run(kArm, "str %w1, %w0\n", {mem, val});
    ASSERT_TRUE(accepted->parsed) << messages(*accepted);
    EXPECT_TRUE(accepted->ok)
        << "a GPR letter on the same binding must keep the shipped "
           "accepted-and-ignored path: " << messages(*accepted);
    EXPECT_FALSE(accepted->bytes.empty())
        << "`str w1, [x0]` must still encode";
}

// ✔MEASURED: the references SPLIT on an FP letter over an immediate binding —
// gcc refuses `%d0` on `"i"(7)`, clang renders `7` — so acceptance is decided
// by the disjunction and the class check SKIPS the ImmInt form: the letter
// dies exactly as it does for every width view on a non-register binding.
TEST(AsmClassScopedModifiers, AnyLetterOnAnImmediateBindingSkipsTheClassCheck) {
    for (auto const* letter : {"d", "x"}) {
        Binding out{"%0", "x0", LirRegClass::GPR, 64};
        Binding lhs{"%1", "x1", LirRegClass::GPR, 64};
        Binding imm{"%2", "", LirRegClass::None, 32, OperandKindFilter::ImmInt,
                    true, 5};
        auto const text = std::format("add %0, %1, %{}2\n", letter);
        auto const r    = run(kArm, text, {out, lhs, imm});
        ASSERT_TRUE(r->parsed) << messages(*r);
        EXPECT_TRUE(r->ok)
            << "%" << letter << "2 on an immediate binding must be accepted "
            << "with the letter dying (the disjunction's accepted side):\n"
            << messages(*r);
        EXPECT_EQ(messages(*r).find(kClassRefusal), std::string::npos)
            << messages(*r);
        EXPECT_FALSE(r->bytes.empty()) << "`add x0, x1, #5` must encode";
    }
}

// ══ RED ON DISABLE — THE LETTER ROWS, THE SCOPE KEY, AND THE MIX GATE ═════
//
// ★★★ REMOVE-DIRECTION, ON THE SHIPPED DOCUMENT. Dropping the five FP rows
// must un-mint the composed lexemes: `%d0` stops parsing at all, which is the
// pre-P50 refusal restored. The shipped-document control runs in the same
// test so the pin cannot pass by the harness failing in both directions.
TEST(AsmClassScopedModifiers, DroppingTheFpLetterRowsRestoresTheParseRefusal) {
    auto const dropped = loadDialectMutated(
        kArm.dialect, [](nlohmann::json& doc) {
            auto& mods = doc["assembly"]["templateModifiers"];
            ASSERT_TRUE(mods.is_array());
            nlohmann::json kept = nlohmann::json::array();
            for (auto const& row : mods) {
                if (row.value("registerClass", std::string{}) == "fpr") {
                    continue;
                }
                kept.push_back(row);
            }
            ASSERT_LT(kept.size(), mods.size())
                << "the shipped document declares no fpr-scoped letters — "
                   "this mutation removes nothing";
            mods = kept;
        });
    ASSERT_NE(dropped.grammar, nullptr)
        << "an all-gpr letter table is a coherent document and must load: "
        << joined(dropped.loadErrors);

    auto targetR = TargetSchema::loadShipped(kArm.target);
    ASSERT_TRUE(targetR.has_value());
    auto const mutant = runOn(dropped.grammar, *targetR, "mov %d0, %d1\n",
                              {{"%0", "v0", LirRegClass::FPR, 64},
                               {"%1", "v1", LirRegClass::FPR, 64}});
    EXPECT_FALSE(mutant->parsed)
        << "with the fpr rows dropped, `%d0` still parsed — the letter's "
           "lexeme is minted from somewhere other than the declaration";

    auto const shipped = run(kArm, "mov %d0, %d1\n",
                             {{"%0", "v0", LirRegClass::FPR, 64},
                              {"%1", "v1", LirRegClass::FPR, 64}});
    EXPECT_TRUE(shipped->parsed)
        << "the shipped document must parse the same text — this control is "
           "what makes the mutant arm a measurement: " << messages(*shipped);
}

// ★★★ THE SCOPE KEY ITSELF: delete `registerClass` from EVERY row and the
// document must still load (the width-only posture is legal — it is x86-64's)
// — but the R8 refusal must be GONE: `%x0` on an FP operand then decodes at
// the letter's width and dies later, downstream. The class semantics live in
// the KEY, not in the letter table's shape.
TEST(AsmClassScopedModifiers, UnscopingEveryLetterDeletesTheClassRefusal) {
    auto const unscoped = loadDialectMutated(
        kArm.dialect, [](nlohmann::json& doc) {
            auto& mods = doc["assembly"]["templateModifiers"];
            ASSERT_TRUE(mods.is_array());
            bool removed = false;
            for (auto& row : mods) {
                if (row.contains("registerClass")) {
                    row.erase("registerClass");
                    removed = true;
                }
            }
            ASSERT_TRUE(removed);
        });
    ASSERT_NE(unscoped.grammar, nullptr)
        << "an all-unscoped table is the x86-64 posture and must load: "
        << joined(unscoped.loadErrors);

    auto targetR = TargetSchema::loadShipped(kArm.target);
    ASSERT_TRUE(targetR.has_value());
    auto const r = runOn(unscoped.grammar, *targetR, "mov %x0, %x1\n",
                         {{"%0", "v0", LirRegClass::FPR, 64},
                          {"%1", "v1", LirRegClass::FPR, 64}});
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_EQ(messages(*r).find(kClassRefusal), std::string::npos)
        << "with no scopes declared the class check still fired — the "
           "refusal is not reading the declaration: " << messages(*r);
}

// ★★ THE MIX GATE: one row unscoped beside scoped ones is refused AT LOAD,
// naming the posture rule — an unscoped row is indistinguishable from a
// forgotten class, and a forgotten class would load as legal-on-every-class.
TEST(AsmClassScopedModifiers, AMixedScopeDocumentIsRefusedAtLoad) {
    auto const mixed = loadDialectMutated(
        kArm.dialect, [](nlohmann::json& doc) {
            auto& mods = doc["assembly"]["templateModifiers"];
            ASSERT_TRUE(mods.is_array() && !mods.empty());
            ASSERT_TRUE(mods.front().contains("registerClass"));
            mods.front().erase("registerClass");
        });
    EXPECT_EQ(mixed.grammar, nullptr)
        << "a document mixing scoped and unscoped letters loaded clean";
    EXPECT_NE(joined(mixed.loadErrors).find("mixes"), std::string::npos)
        << joined(mixed.loadErrors);
}

// The scope's own vocabulary: an unknown class name, and the inoperable
// "none", are load errors naming the closed set.
TEST(AsmClassScopedModifiers, AnUnknownOrInoperableScopeIsRefusedAtLoad) {
    for (auto const* bad : {"zzz", "none"}) {
        auto const broken = loadDialectMutated(
            kArm.dialect, [bad](nlohmann::json& doc) {
                auto& mods = doc["assembly"]["templateModifiers"];
                ASSERT_TRUE(mods.is_array() && !mods.empty());
                mods.front()["registerClass"] = bad;
            });
        EXPECT_EQ(broken.grammar, nullptr)
            << "registerClass '" << bad << "' loaded clean";
        auto const why = joined(broken.loadErrors);
        EXPECT_NE(why.find(bad), std::string::npos) << why;
        EXPECT_NE(why.find("gpr"), std::string::npos)
            << "the refusal must render the legal set: " << why;
    }
}

// ══ THE OTHER PORT: WIDTH-ONLY IS A MEASUREMENT, NOT AN OMISSION ══════════
//
// ✔MEASURED: x86-64 gcc accepts a GPR letter on an `"x"`-bound double and
// renders the bare `%xmm0`; DSS at HEAD compiles `movsd %q1, %q0` over xmm
// operands (the letter's 64 coinciding with `movsd`'s width). Scoping those
// letters would refuse a program a reference accepts — this pin is what keeps
// a future scoping honest about that cost.
TEST(AsmClassScopedModifiers, TheX86GprLettersStillReachAnXmmOperand) {
    auto const r = run(kX86, "movsd %q1, %q0\n",
                       {{"%0", "xmm0", LirRegClass::FPR, 64},
                        {"%1", "xmm1", LirRegClass::FPR, 64}});
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_TRUE(r->ok) << messages(*r);
    EXPECT_EQ(messages(*r).find(kClassRefusal), std::string::npos)
        << messages(*r);
    EXPECT_FALSE(r->bytes.empty())
        << "`movsd %q1, %q0` over xmm operands must still encode — gcc "
           "accepts this program and so did this pipeline before P50";
}

// ⚠ THE KNOWN, RECORDED GAP — PINNED AS A RATCHET, NOT AS AN ENDORSEMENT. A
// NARROWER x86 letter on the same xmm operand (`%k0`, 32 bits, against
// `movsd`'s 64) is refused by the width-honesty gate where gcc
// accepts-and-ignores it. That divergence PREDATES the class-scoped surface
// (the letters have stated widths since P30) and closing it needs gcc's full
// mismatch-degrades-to-bare semantics measured across the x86 modifier set —
// its own row's work, recorded in `asm-x86_64-att.lang.json`'s
// `$templateModifiersComment`. This pin exists so the day that behaviour
// changes, the record and the code are forced to move together.
TEST(AsmClassScopedModifiers, TheX86NarrowLetterOnAnXmmOperandStillWidthRefuses) {
    auto const r = run(kX86, "movsd %k1, %k0\n",
                       {{"%0", "xmm0", LirRegClass::FPR, 64},
                        {"%1", "xmm1", LirRegClass::FPR, 64}});
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_FALSE(r->ok)
        << "the recorded width-honesty refusal is gone — if this is a "
           "deliberate conformance fix, update the x86 dialect's "
           "$templateModifiersComment record with the new measurement; if "
           "not, a 32-bit view is silently widening";
    EXPECT_EQ(messages(*r).find(kClassRefusal), std::string::npos)
        << "the width-only port must never class-refuse: " << messages(*r);
}
