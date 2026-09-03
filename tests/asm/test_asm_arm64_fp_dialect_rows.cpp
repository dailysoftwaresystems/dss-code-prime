// THE SCALAR FLOATING-POINT INSTRUCTION ROWS OF THE aarch64 gas DIALECT
// (D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME, arm64
// half).
//
// ★★★ WHAT THESE PINS ARE FOR, and they are the arm64 twin of
// `test_asm_x86_sse_dialect_rows.cpp`. `arm64.target.json` binds the constraint
// letter `w` to the `fpr` register class, and until cycle P54 this dialect's
// `instructions[]` table spelled NONE of the target's twenty FP/SIMD opcodes —
// so `"w"` could be BOUND but no template could name an instruction that uses
// its operand. ✔MEASURED at the CLI before the rows landed: 51 rows covering 29
// of the target's 87 opcodes; `__asm__("nop" : "+w"(r))` over a `double`
// compiled rc=0 while `__asm__("fadd %d0, %d1, %d2" : "=w"(r) : "w"(a),
// "w"(b))` returned `A_AsmTextUnsupported … unknown mnemonic 'fadd'`.
//
// ★★★ THE FAILURE MODE THESE PINS EXIST TO CATCH IS A WIDTH, NOT AN ABSENCE —
// the same lesson the x86 half learned, arriving through a different field. On
// aarch64 the single/double pair is ftype [23:22] of ONE word, and NO dialect
// row declares a `width`: the REGISTER VIEW written in the template is the only
// thing that says which arm to elect. A row or a variant carrying the wrong
// ftype still assembles and still runs, computing on a value it read as the
// other format, with nothing to see in a build log. Every pin below reads the
// WHOLE 32-bit word, so ftype cannot slip past.
//
// ⚠⚠ AND THAT IS NOT HYPOTHETICAL HERE: declaring these rows made
// `arm64.target.json`'s `fmov` reachable from text for the first time and
// exposed that its width-32 and width-16 arms both carried the width-64 word
// (FMOV Dd,Dn). ✔MEASURED at the CLI: a `float` class-internal move at
// `--config=release` disassembled to `fmov d28, d0` where gcc 13.3.0 and clang
// 18.1.3 both emit `fmov s28, s0`. `FmovSingleIsNotTheDoubleWord` is that
// regression's ratchet.
//
// ★★ EVERY DECLARED SPELLING IS BYTE-VERIFIED AGAINST GNU as 2.42 **AND** clang
// 18.1.3, both agreeing on every probe, and the expected words below are those
// measurements — never derived from a neighbouring form and never read out of a
// manual.
//
// ⚠ THE OPERANDS ARE PINNED TO PHYSICAL SIMD&FP REGISTERS. `assemble()` runs
// after register allocation and a virtual register has no encoding, so an
// unpinned run would assert against an empty byte vector — i.e. would measure
// the harness giving up rather than the instruction.

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

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::string_view kDialect = "asm-arm64-gas";
constexpr std::string_view kTarget  = "arm64";

// The seven spellings this file's subject declares — named once so a pin and a
// mutant cannot drift apart, and so a row silently renamed makes the mutant
// throw rather than make a pin vacuous.
constexpr std::string_view kFpSpellings[] = {"fadd", "fsub", "fmul",
                                             "fdiv", "fneg", "fcmp", "fmov"};

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
//
// ★★ IT READS THE SHIPPED FILE AND MUTATES THAT, never a hand-authored
// stand-in. ⚠ THE MUTATION MUST BE IN THE **REMOVE** DIRECTION: an
// ADD-direction mutant stays green when the real config LOSES the feature,
// which is the direction that actually regresses.
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

// Remove every `assembly.instructions[]` row whose spelling is in `spellings`.
// Refuses loudly when a name matches nothing, so a renamed row cannot silently
// make a pin vacuous.
[[nodiscard]] std::function<void(nlohmann::json&)>
removeInstructionRows(std::vector<std::string> spellings) {
    return [spellings](nlohmann::json& doc) {
        auto& rows = doc.at("assembly").at("instructions");
        for (auto const& want : spellings) {
            std::size_t removed = 0;
            for (auto it = rows.begin(); it != rows.end();) {
                if (it->value("spelling", std::string{}) == want) {
                    it = rows.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            if (removed != 1) {
                throw std::runtime_error{
                    "expected exactly one `" + want
                    + "` row in the shipped dialect, found "
                    + std::to_string(removed)};
            }
        }
    };
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

// The FIRST 32-bit word of the emitted stream, little-endian — this ISA's
// instruction is exactly one word, so the whole instruction is one comparison
// and no field can be checked while a neighbour drifts.
[[nodiscard]] std::uint32_t firstWord(std::vector<std::uint8_t> const& b) {
    if (b.size() < 4) return 0;
    return static_cast<std::uint32_t>(b[0])
         | (static_cast<std::uint32_t>(b[1]) << 8)
         | (static_cast<std::uint32_t>(b[2]) << 16)
         | (static_cast<std::uint32_t>(b[3]) << 24);
}

struct Bind {
    std::string  spelling;      // the template placeholder, e.g. "%d0"
    std::string  physical;      // the target register this operand names
    LirRegClass  cls = LirRegClass::FPR;
    std::uint32_t width = 64;
};

// One template lowering taken to BYTES.
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

// ★ THE PHYSICAL REGISTERS ARE THE `v` ROOTS, NOT THE `d`/`s` VIEWS, AND THAT
// MATCHES THE PIPELINE. R1 declared the SIMD&FP file once — `v0..v31` are the
// class's members and `d`/`s`/`h`/`b` are `subOf` views a calling convention
// may never name — so the allocator hands the encoder a `v` ordinal and the
// WIDTH decides which view the bytes mean. Naming `d0` here would still work
// (same `hwEncoding`) and would test a register the real pipeline never
// produces.
// ⚠ THE BINDING'S SPELLING IS THE PLAIN PLACEHOLDER (`%0`), NEVER THE
// LETTERED ONE. The modifier letter is a property of the TEMPLATE TEXT, not of
// the operand the embedding language bound — `AsmOperandBinding::spelling` is
// the key `%0`, `%d0` and `%q0` all resolve through, and binding `"%d0"` would
// simply put a key in the host's table that nothing ever looks up.
[[nodiscard]] std::unique_ptr<Run>
runFp3(std::string_view templateText, std::uint32_t widthBits) {
    return runOn(loadDialect(), shippedTarget(), templateText,
                 {Bind{"%0", "v0", LirRegClass::FPR, widthBits},
                  Bind{"%1", "v1", LirRegClass::FPR, widthBits},
                  Bind{"%2", "v2", LirRegClass::FPR, widthBits}});
}

[[nodiscard]] std::unique_ptr<Run>
runFp2(std::string_view templateText, std::uint32_t widthBits) {
    return runOn(loadDialect(), shippedTarget(), templateText,
                 {Bind{"%0", "v0", LirRegClass::FPR, widthBits},
                  Bind{"%1", "v1", LirRegClass::FPR, widthBits}});
}

// ── THE MEASURED WORDS ─────────────────────────────────────────────────────
//
// ✔GNU as 2.42 AND clang 18.1.3, agreeing on every row. `%0`->v0, `%1`->v1,
// `%2`->v2 with destination-first operand order, so these are exactly the words
// both assemblers produce for `<op> ?0, ?1, ?2` / `<op> ?0, ?1`.
constexpr std::uint32_t kFaddD = 0x1E622820u;  // fadd d0, d1, d2
constexpr std::uint32_t kFaddS = 0x1E222820u;  // fadd s0, s1, s2
constexpr std::uint32_t kFsubD = 0x1E623820u;
constexpr std::uint32_t kFsubS = 0x1E223820u;
constexpr std::uint32_t kFmulD = 0x1E620820u;
constexpr std::uint32_t kFmulS = 0x1E220820u;
constexpr std::uint32_t kFdivD = 0x1E621820u;
constexpr std::uint32_t kFdivS = 0x1E221820u;
constexpr std::uint32_t kFnegD = 0x1E614020u;  // fneg d0, d1
constexpr std::uint32_t kFnegS = 0x1E214020u;
constexpr std::uint32_t kFcmpD = 0x1E612000u;  // fcmp d0, d1 — Rm, not Rn
constexpr std::uint32_t kFcmpS = 0x1E212000u;
constexpr std::uint32_t kFmovD = 0x1E604020u;  // fmov d0, d1
constexpr std::uint32_t kFmovS = 0x1E204020u;  // fmov s0, s1
// ⚠ THE WORD BOTH REFERENCES PRODUCE **ONLY** UNDER `.arch armv8.2-a+fp16`,
// and REFUSE at the default -march (✔MEASURED 2026-09-02, gas 2.42 and clang
// 18.1.3 separately). It is kept as the NEGATIVE constant — the word DSS must
// never emit — after `arm64.target.json`'s width-16 `fmov` arm was deleted for
// being above the union (cycle P54, lane `ae`).
constexpr std::uint32_t kFmovH = 0x1EE04020u;  // fmov h0, h1 (armv8.2+fp16)

}  // namespace

// ══ THE SUBJECT: SEVEN SPELLINGS, TWO WIDTHS, WHOLE WORDS ══════════════════

TEST(AsmArm64FpDialectRows, ThreeOperandArithmeticEncodesBothFtypes) {
    struct Case {
        std::string_view spelling;
        std::uint32_t    dWord;
        std::uint32_t    sWord;
    };
    constexpr Case kCases[] = {
        {"fadd", kFaddD, kFaddS},
        {"fsub", kFsubD, kFsubS},
        {"fmul", kFmulD, kFmulS},
        {"fdiv", kFdivD, kFdivS},
    };
    for (auto const& c : kCases) {
        {
            auto const r = runFp3(std::format("{} %d0, %d1, %d2\n", c.spelling), 64);
            ASSERT_TRUE(r->parsed) << c.spelling << ": " << messages(*r);
            ASSERT_TRUE(r->ok) << c.spelling << ": " << messages(*r);
            EXPECT_EQ(firstWord(r->bytes), c.dWord)
                << c.spelling << " at the D views must encode the ftype=01 "
                   "word gas and clang both emit; got " << hex(r->bytes);
        }
        {
            auto const r = runFp3(std::format("{} %s0, %s1, %s2\n", c.spelling), 32);
            ASSERT_TRUE(r->parsed) << c.spelling << ": " << messages(*r);
            ASSERT_TRUE(r->ok) << c.spelling << ": " << messages(*r);
            EXPECT_EQ(firstWord(r->bytes), c.sWord)
                << c.spelling << " at the S views must encode the ftype=00 "
                   "word; got " << hex(r->bytes);
        }
        // ★ THE DISCRIMINATION, STATED RATHER THAN LEFT TO THE READER: the two
        // words must DIFFER, or the width axis is decorative and a row declared
        // at the wrong view would still pass every assertion above.
        EXPECT_NE(c.dWord, c.sWord) << c.spelling;
    }
}

TEST(AsmArm64FpDialectRows, FnegEncodesBothFtypes) {
    auto const d = runFp2("fneg %d0, %d1\n", 64);
    ASSERT_TRUE(d->parsed && d->ok) << messages(*d);
    EXPECT_EQ(firstWord(d->bytes), kFnegD) << hex(d->bytes);

    auto const s = runFp2("fneg %s0, %s1\n", 32);
    ASSERT_TRUE(s->parsed && s->ok) << messages(*s);
    EXPECT_EQ(firstWord(s->bytes), kFnegS) << hex(s->bytes);
}

// ★★ `fcmp`'s target `result` is `none`, so BOTH written operands are INPUTS
// and neither is a destination. The observable is which FIELD the second
// register lands in: an input pair rides Rn[9:5] and Rm[20:16], so `fcmp ?0,
// ?1` puts register 1 at bit 16 — where a destination-first reading would have
// put it at bit 5 and produced 0x1E602020 instead. The pin reads the whole
// word, so that transposition cannot hide.
TEST(AsmArm64FpDialectRows, FcmpTreatsBothOperandsAsInputs) {
    auto const d = runFp2("fcmp %d0, %d1\n", 64);
    ASSERT_TRUE(d->parsed && d->ok) << messages(*d);
    EXPECT_EQ(firstWord(d->bytes), kFcmpD) << hex(d->bytes);
    EXPECT_NE(firstWord(d->bytes), 0x1E602020u)
        << "the second operand landed in Rn — the row was read as though it "
           "had a destination";

    auto const s = runFp2("fcmp %s0, %s1\n", 32);
    ASSERT_TRUE(s->parsed && s->ok) << messages(*s);
    EXPECT_EQ(firstWord(s->bytes), kFcmpS) << hex(s->bytes);
}

// ══ THE REGRESSION THIS BLOCK FOUND ════════════════════════════════════════

// ★★★ THE RATCHET ON THE BYTE DIVERGENCE. Before P54 all three `fmov` variants
// in `arm64.target.json` carried the width-64 word, so a `float` move emitted
// FMOV Dd,Dn — ✔MEASURED at the CLI, `fmov d28, d0` where gcc 13.3.0 and clang
// 18.1.3 both emit `fmov s28, s0`. Each width now carries its own ftype.
TEST(AsmArm64FpDialectRows, FmovSingleIsNotTheDoubleWord) {
    auto const d = runFp2("fmov %d0, %d1\n", 64);
    ASSERT_TRUE(d->parsed && d->ok) << messages(*d);
    EXPECT_EQ(firstWord(d->bytes), kFmovD) << hex(d->bytes);

    auto const s = runFp2("fmov %s0, %s1\n", 32);
    ASSERT_TRUE(s->parsed && s->ok) << messages(*s);
    EXPECT_EQ(firstWord(s->bytes), kFmovS) << hex(s->bytes);
    EXPECT_NE(firstWord(s->bytes), kFmovD)
        << "the single-precision FMOV emitted the DOUBLE word — the exact "
           "divergence from gas 2.42 and clang 18.1.3 this row corrected";

    // ⚠⚠ THE HALF-PRECISION ARM ASSERTED A WORD UNTIL 2026-09-02 AND NOW
    // ASSERTS A REFUSAL, because the variant it read was ABOVE THE UNION
    // (cycle P54, lane `ae`). ✔MEASURED, gas 2.42 and clang 18.1.3 probed
    // SEPARATELY and agreeing: `fmov h0, h1` is REJECTED by BOTH at the default
    // -march — FP16 arithmetic is `FEAT_FP16` — while DSS assembled it rc=0 to
    // 0x1EE04020, a program neither reference accepts. `kFmovH`'s bytes were
    // correctly measured (both references DO produce them under
    // `.arch armv8.2-a+fp16`) and the reasoning attached to them inverted the
    // disjunction: it makes a construct required when SOME reference accepts
    // it, and forbidden when NONE does.
    // ⓘ THE fp16 STORY IS PER-INSTRUCTION, NOT PER-WIDTH. The fp16 FCVT forms
    // are base ARMv8-A and both references assemble them (`fcvt h0, s1` =
    // 0x1E23C020, `fcvt s0, h1` = 0x1EE24020), so DSS WAS BELOW the union there
    // — closed 2026-09-02 (cycle P55, lane fp) by the target opcode `fpcvt_h`,
    // and all four words are pinned in
    // `tests/asm/test_asm_arm64_conversion_dialect_rows.cpp`:
    // [[D-TARGET-ARM64-FP16-CONVERSION-FORMS-UNDECLARED]].
    // ⚠ THAT SENTENCE SAID *the fp16 CONVERSIONS* AND THE SET IS NARROWER THAN
    // IT SOUNDS, ✔RE-MEASURED in P55: only FCVT (float↔float) has half arms at
    // the default -march. `fcvtzs w0, h1`, `fcvtzs x0, h1`, `scvtf h0, w1`,
    // `scvtf h0, x1`, `fcvtns w0, h1` and `fcvtas w0, h1` are ALL refused by
    // BOTH references, so the fp16 INTEGER conversions sit beside `fmov h0, h1`
    // on the `FEAT_FP16` side of this same fence.
    auto const h = runFp2("fmov %h0, %h1\n", 16);
    ASSERT_TRUE(h->parsed) << messages(*h);
    EXPECT_FALSE(h->ok)
        << "`fmov h0, h1` assembled, and gas 2.42 and clang 18.1.3 both refuse "
           "it at the default -march";
    EXPECT_NE(firstWord(h->bytes), kFmovH)
        << "the FEAT_FP16 word was emitted: " << hex(h->bytes);
}

// ══ THE NEAR-MISS RULE: WHAT STAYS UNDECLARED AND MUST REFUSE ══════════════

// ★★★ THE FENCE. Every spelling here is accepted by GNU as 2.42 and clang
// 18.1.3 and is DELIBERATELY absent from this dialect, because the target
// declares no template emitting its bytes — so there is nothing to bind them to
// and binding one to a neighbour would silently encode a different instruction.
//
// ⚠⚠ THE LIST WAS LONGER, AND THE SEVEN SPELLINGS THAT LEFT IT ARE THE POINT.
// It also carried `fcvt`, `fcvtzs`, `fcvtzu`, `scvtf`, `ucvtf`, `addv` and
// `cnt`, under a SECOND reason: *the target DOES declare one but the row shape
// cannot select it safely — one mnemonic over N width PAIRS, and two rows
// sharing a spelling are refused at load*. That reason was TRUE when it was
// written and was CLOSED in the same cycle (P54, lane el): a variant-level
// `destWidth` declaration plus a `destWidthFromOperands` row key make one row
// cover every pair, and the lane-arrangement grammar reached `cnt`/`addv`. All
// seven now SHIP, with their own byte pins in
// `test_asm_arm64_conversion_dialect_rows.cpp`.
// ⇒ this arm keeps only the group whose reason has not changed. ★ AND THE
// HAZARD IT WAS WATCHING FOR IS NOT UNGUARDED: `fcvtzs %w0, %s1` silently
// emitting FCVTZS **Xd**,Sn is now `FcvtzsNarrowDestinationIsTheWForm`'s
// subject, which asserts the W word rather than the absence of any word — a
// strictly stronger question than "is this spelling unknown".
//
// ⚠⚠ AND IT SHRANK AGAIN LATER THE SAME DAY, WHICH IS WORTH RECORDING BECAUSE
// IT IS THE SECOND TIME THIS ONE LIST WENT STALE INSIDE A SINGLE CYCLE. Nine
// MORE spellings left it — `fabs`, `fsqrt`, `fmax`, `fmin`, `fmadd`, `fmsub`,
// `fnmul`, `frinta`, `frintz` — when the operator ruled the remaining
// vocabulary in and lane `av` declared it
// (D-ASM-ARM64-GAS-SPELLS-NO-ROUNDING-MODE-OR-VECTOR-MOVE). Their reason was
// never "these are dangerous"; it was "the target declares no template emitting
// their bytes", and that sentence stopped being true the moment the opcodes
// landed. Their byte pins live in
// `tests/asm/test_asm_arm64_rounding_dialect_rows.cpp`.
// ⇒ WHAT IS LEFT HERE IS THE ONE GROUP WITH A STRUCTURAL REASON RATHER THAN AN
// ABSENT-OPCODE ONE: `fcsel` and `fccmp` carry a CONDITION operand (and `fccmp`
// an `#nzcv` immediate), which this dialect does not model as an instruction
// operand. The fence is smaller and it is still a fence.
TEST(AsmArm64FpDialectRows, UndeclaredFpSpellingsStayRefused) {
    constexpr std::string_view kNearMisses[] = {
        "fcsel", "fccmp",
    };
    for (auto const sp : kNearMisses) {
        auto const r = runFp2(std::format("{} %d0, %d1\n", sp), 64);
        ASSERT_TRUE(r->parsed)
            << sp << " failed to PARSE — this arm is about the instruction "
                     "table, and a lexer refusal would make it vacuous: "
            << messages(*r);
        EXPECT_FALSE(r->ok)
            << sp << " lowered — it is undeclared on purpose, and something "
                     "bound it to a neighbouring target opcode";
        EXPECT_NE(messages(*r).find("unknown mnemonic"), std::string::npos)
            << sp << ": the refusal must name the spelling as unknown to the "
                     "table, not fail some later way: " << messages(*r);
    }
}

// ★★★ THE CROSS-FILE `fmov` NOW ENCODES, AND THIS ARM IS THE ONE THAT SAID SO
// IN ADVANCE. It used to assert a REFUSAL, and its own failure message named
// the exact event that would invalidate it — *"election grew a class axis and
// this pin was not told"*. It did, in cycle P54 (lane el): `electOpcode` gained
// a REGISTER-PROFILE axis reading the banks `arm64.target.json` already
// declared, so `fmov`, `movq_xmm_to_gpr` and `movq_gpr_to_xmm` are separable
// and one dialect row names all three.
//
// ⚠ WHAT SURVIVES UNCHANGED IS THE UNCONDITIONAL HALF: whatever this line does,
// it must not do it SILENTLY and it must not take the fpr diagonal. Emitting
// `fmov d0, d1`'s word for a GPR destination would be a GPR ordinal reaching an
// FP field — the miscompile the encoder's register-class gate exists to refuse
// — and that arm is kept verbatim.
// ⓘ The whole family (all four directions, both widths, plus the mutants that
// prove the axis is reading the target) lives in
// `test_asm_arm64_conversion_dialect_rows.cpp`; this arm stays here because it
// is this file's claim that changed.
TEST(AsmArm64FpDialectRows, CrossFileFmovNowEncodesAndNeverTakesTheDiagonal) {
    auto const r = runOn(loadDialect(), shippedTarget(), "fmov %x0, %d1\n",
                         {Bind{"%0", "x0", LirRegClass::GPR, 64},
                          Bind{"%1", "v1", LirRegClass::FPR, 64}});
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_TRUE(r->ok) << messages(*r);
    // ✔gas 2.42 + clang 18.1.3, agreeing: `fmov x0, d1` = 0x9E660020.
    EXPECT_EQ(firstWord(r->bytes), 0x9E660020u) << hex(r->bytes);
    EXPECT_NE(firstWord(r->bytes), kFmovD)
        << "a cross-file `fmov` took the fpr diagonal — a GPR ordinal reached "
           "an FP field, which is exactly what the encoder's register-class "
           "gate exists to refuse: " << hex(r->bytes);
}

// ══ RED-ON-DISABLE, REMOVE DIRECTION, OVER THE SHIPPED DOCUMENTS ═══════════

// M1 — delete all seven rows from the SHIPPED dialect. Every arithmetic pin
// above must lose its subject, and the shipped CONTROL in the same test proves
// the discrimination is the mutation and not the harness.
TEST(AsmArm64FpDialectRows, RemovingTheRowsUnspellsEveryFpMnemonic) {
    std::vector<std::string> all;
    for (auto const sp : kFpSpellings) all.emplace_back(sp);
    auto const dropped = mutateShippedDialectDoc(removeInstructionRows(all));
    ASSERT_TRUE(dropped.has_value())
        << "the mutant dialect did not load — the pin would measure a load "
           "failure rather than the missing rows";

    for (auto const sp : kFpSpellings) {
        auto const mutant =
            runOn(*dropped, shippedTarget(), std::format("{} %d0, %d1\n", sp),
                  {Bind{"%0", "v0", LirRegClass::FPR, 64},
                   Bind{"%1", "v1", LirRegClass::FPR, 64}});
        ASSERT_TRUE(mutant->parsed) << sp;
        EXPECT_FALSE(mutant->ok)
            << sp << " still lowered with its row deleted — the pins above are "
                     "not reading the dialect table";
        EXPECT_NE(messages(*mutant).find("unknown mnemonic"),
                  std::string::npos)
            << sp << ": " << messages(*mutant);
    }

    // ★ THE CONTROL, in the same test so it cannot be skipped: the SHIPPED
    // document lowers the same line.
    auto const control = runFp2("fneg %d0, %d1\n", 64);
    ASSERT_TRUE(control->parsed && control->ok) << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kFnegD) << hex(control->bytes);
}

// M2 — delete the `fmov` opcode's WIDTH-32 encoding variant from the SHIPPED
// target. `fmov %s0, %s1` must then elect nothing and fail loud, while the D
// arm keeps encoding — which is what proves the byte pin above reads the
// target's variant table rather than a constant that happens to match.
TEST(AsmArm64FpDialectRows, RemovingTheFmovSingleVariantUnelectsTheSForm) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            std::size_t removed = 0;
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", std::string{}) != "fmov") continue;
                auto& variants = op.at("encoding").at("variants");
                for (auto it = variants.begin(); it != variants.end();) {
                    if (it->at("guard").value("width", 0u) == 32u) {
                        it = variants.erase(it);
                        ++removed;
                    } else {
                        ++it;
                    }
                }
            }
            if (removed != 1) {
                throw std::runtime_error{
                    "expected exactly one width-32 `fmov` variant in the "
                    "shipped arm64 target, found "
                    + std::to_string(removed)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing variant";

    auto const sMutant =
        runOn(loadDialect(), *mutant, "fmov %s0, %s1\n",
              {Bind{"%0", "v0", LirRegClass::FPR, 32},
               Bind{"%1", "v1", LirRegClass::FPR, 32}});
    ASSERT_TRUE(sMutant->parsed) << messages(*sMutant);
    EXPECT_TRUE(!sMutant->ok || sMutant->bytes.size() <= 4u)
        << "the S-form still encoded with its variant deleted: "
        << hex(sMutant->bytes);
    EXPECT_NE(firstWord(sMutant->bytes), kFmovS)
        << "the S word survived the deletion of the variant that declares it — "
           "the byte pin is not reading the target";

    // ★ THE D ARM IS THE MATCHED CONTROL: the SAME mutant target still encodes
    // the width-64 form, so the mutation was SURGICAL rather than merely
    // destructive.
    auto const dMutant =
        runOn(loadDialect(), *mutant, "fmov %d0, %d1\n",
              {Bind{"%0", "v0", LirRegClass::FPR, 64},
               Bind{"%1", "v1", LirRegClass::FPR, 64}});
    ASSERT_TRUE(dMutant->parsed && dMutant->ok) << messages(*dMutant);
    EXPECT_EQ(firstWord(dMutant->bytes), kFmovD) << hex(dMutant->bytes);
}

// ══ THE CENSUS THAT MAKES THE GAP ITSELF MEASURABLE ════════════════════════

// ★★ THE ROW'S OWN DEFECT CLASS, AS A STANDING PREDICATE RATHER THAN A
// NARRATIVE: for EVERY register class the shipped target declares operable,
// some declared instruction row must be able to name it. That is the sentence
// D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME was opened
// against, and nothing in the tree asked it before this arm. It flips the day a
// target grows a class (or a constraint letter binding one) with no dialect
// spelling to use it — which is exactly how this defect arrived twice, once per
// ISA.
// ⚠⚠ AND IT IS PROVEN NON-VACUOUS BY SYNTHESIZING THE NEGATIVE RATHER THAN BY
// ITS OWN SILENCE. A guard whose subject cannot be absent asserts nothing; the
// second half of this test runs the SAME predicate over the M1 mutant dialect —
// the seven rows deleted — and requires it to name `fpr`. If a future edit ever
// makes the predicate structurally unable to fail, that half goes red first.
namespace {

// Every `asmConstraints`-bindable register class that NO declared instruction
// row can name. Empty is the healthy answer.
[[nodiscard]] std::vector<TargetRegClass>
classesNoInstructionCanName(GrammarSchema const& dialect,
                            TargetSchema const&  target) {
    std::vector<TargetRegClass> bound;
    for (auto const& c : target.asmConstraints()) {
        if (c.binds != AsmConstraintBinding::RegisterClass) continue;
        if (!c.registerClass.has_value()) continue;
        if (std::ranges::find(bound, *c.registerClass) == bound.end()) {
            bound.push_back(*c.registerClass);
        }
    }
    std::vector<TargetRegClass> orphaned;
    for (auto const cls : bound) {
        bool named = false;
        for (auto const& row : dialect.assembly().instructions) {
            for (auto const& name : row.opcodeNames) {
                auto const ord = target.opcodeByMnemonic(name);
                if (!ord.has_value()) continue;
                auto const* info = target.opcodeInfo(*ord);
                if (info == nullptr) continue;
                if (info->encoding.registerClass.has_value()
                    && *info->encoding.registerClass == cls) {
                    named = true;
                    break;
                }
            }
            if (named) break;
        }
        if (!named) orphaned.push_back(cls);
    }
    return orphaned;
}

[[nodiscard]] std::string classList(std::vector<TargetRegClass> const& v) {
    std::string out;
    for (auto const c : v) {
        if (!out.empty()) out += ", ";
        out += targetRegClassName(c);
    }
    return out.empty() ? std::string{"<none>"} : out;
}

}  // namespace

TEST(AsmArm64FpDialectRows, EveryConstraintBoundClassHasAnInstructionRow) {
    auto const target  = shippedTarget();
    auto const dialect = loadDialect();

    // Guard the guard: a target that bound NO class would make the predicate
    // trivially true.
    std::size_t bindable = 0;
    for (auto const& c : target->asmConstraints()) {
        if (c.binds == AsmConstraintBinding::RegisterClass
            && c.registerClass.has_value()) {
            ++bindable;
        }
    }
    ASSERT_GT(bindable, 0u)
        << "the shipped arm64 target binds no register class at all — this "
           "arm has lost its subject";

    auto const orphaned = classesNoInstructionCanName(*dialect, *target);
    EXPECT_TRUE(orphaned.empty())
        << "register class(es) " << classList(orphaned)
        << " are bindable by an `asmConstraints` letter but NO declared "
           "instruction row can name them — a constraint that binds a class no "
           "instruction can use is the defect this file exists for";

    // ★ THE SYNTHESIZED NEGATIVE. With EVERY row that can name an `fpr`-class
    // opcode removed, `fpr` becomes exactly the orphan this row was opened
    // against — which is what proves the predicate above can answer anything
    // other than "empty".
    //
    // ⚠⚠ THE HARDCODED LIST WENT VACUOUS TWICE IN ONE DAY, SO IT IS GONE.
    // ✔MEASURED both times, and both times the arm caught itself rather than
    // passing: deleting only `kFpSpellings` stopped orphaning `fpr` when lane
    // `el` added the conversions, `cnt`/`addv` and the FP siblings on
    // `ldr`/`str`/`ldur`/`stur`; the eleven-name patch that fixed it stopped
    // working hours later when lane `av` added twenty more `fpr`-class rows
    // (D-ASM-ARM64-GAS-SPELLS-NO-ROUNDING-MODE-OR-VECTOR-MOVE). A list that has
    // to be maintained in lockstep with a growing vocabulary is a pin that goes
    // vacuous between cycles, which is the failure this whole file is about.
    // ⇒ THE MUTANT NOW DERIVES ITS OWN LIST from the two shipped documents:
    // every dialect row naming an opcode whose `encoding.registerClass` is the
    // class under test. It cannot drift, because it asks the same question the
    // predicate asks. ⓘ `kFpSpellings` stays — it is the M1 mutant's subject
    // above and this file's list of what it declared.
    std::vector<std::string> all;
    for (auto const& row : dialect->assembly().instructions) {
        bool namesTheClass = false;
        for (auto const& name : row.opcodeNames) {
            auto const ord = target->opcodeByMnemonic(name);
            if (!ord.has_value()) continue;
            auto const* info = target->opcodeInfo(*ord);
            if (info == nullptr) continue;
            if (info->encoding.registerClass.has_value()
                && *info->encoding.registerClass == *targetRegClassFromName("fpr")) {
                namesTheClass = true;
                break;
            }
        }
        if (!namesTheClass) continue;
        if (std::ranges::find(all, row.spelling) == all.end()) {
            all.push_back(row.spelling);
        }
    }
    ASSERT_FALSE(all.empty())
        << "no shipped dialect row names an `fpr`-class opcode — the "
           "synthesized negative has lost its subject before it ran";
    auto const dropped = mutateShippedDialectDoc(removeInstructionRows(all));
    ASSERT_TRUE(dropped.has_value());
    auto const orphanedMutant = classesNoInstructionCanName(**dropped, *target);
    auto const fpr = targetRegClassFromName("fpr");
    ASSERT_TRUE(fpr.has_value());
    EXPECT_NE(std::ranges::find(orphanedMutant, *fpr), orphanedMutant.end())
        << "with every FP row deleted the predicate STILL reported no orphaned "
           "class — it cannot fail, so its silence on the shipped tree proves "
           "nothing. Found: " << classList(orphanedMutant);
}
