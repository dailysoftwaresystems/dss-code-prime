// THE HALF-, BYTE- AND QUAD-WIDTH SIMD&FP MEMORY FORMS OF THE aarch64 gas
// DIALECT — [[D-TARGET-ARM64-HALF-BYTE-QUAD-MEMORY-FORMS-UNDECLARED]].
//
// ★★★ WHAT THIS FILE IS FOR. `arm64.target.json` declared `fldur`/`fstur`/
// `fldr_u`/`fstr_u` at widths 32, 64 and 128 and nothing else, and the SIMD&FP
// register table spelled `v`/`d`/`s`/`h`/`b` and not `q`. So of the fifteen
// scalar SIMD&FP memory spellings both references assemble, DSS refused nine.
// ✔MEASURED at the P55 base through the REAL CLI, one inline-`asm` template per
// compile: `ldr s0,[x9,#4]` and `ldr d0,[x9,#8]` compiled rc=0 while
// `ldr h0,[x9,#2]` returned *'ldr' produced 3 LIR operand(s) at width 16, and
// no candidate target opcode encodes that shape* and `ldr q0,[x9,#16]` returned
// *'ldr' writes to a destination that is neither a register nor a memory
// reference* — two DIFFERENT refusals, which is the shape of the fix: the
// `h`/`b` half was a missing WIDTH ARM and the `q` half was a missing NAME.
//
// ★★★ THE SILENT-WRONG-ANSWER RISK IS THE SCALE, AND IT IS WHY THE FENCE HERE
// IS LARGER THAN THE PIN. The scaled unsigned-offset LDR/STR encodes
// `imm12 = byteOffset / accessSize`, so ONE byte offset is FIVE different
// fields across the five widths — ✔MEASURED, gas 2.42 and clang 18.1.3
// agreeing: at `[x1,#16]` the field is 16 (B), 8 (H), 4 (S), 2 (D), 1 (Q).
// An arm that borrowed a neighbour's scale would not refuse; it would address
// the WRONG MEMORY and run. `NearMissFence*` below is that fence.
//
// ★★ EVERY WORD IS ✔MEASURED AGAINST GNU as 2.42 **AND** clang 18.1.3, probed
// SEPARATELY and agreeing on all 89 probes of the run that produced them, each
// read back out of an assembled object with `aarch64-linux-gnu-objdump`. Each
// base is derived from more than one (Rt,Rn) placement so a field-placement
// error cannot hide behind a single Rt=0/Rn=x1 probe.
//
// ⚠ THE OPERANDS ARE PHYSICAL REGISTERS WRITTEN IN THE TEMPLATE TEXT, not
// bindings: this dialect's memory production takes a register NAME inside its
// brackets, so a `q0` that does not resolve in the TARGET's table is refused at
// the parse/lowering boundary rather than encoding something. That is exactly
// the surface this row is about.

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
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::mutateShippedTargetSchemaDoc;

namespace {

constexpr std::string_view kDialect = "asm-arm64-gas";
constexpr std::string_view kTarget  = "arm64";

// The four mnemonics this file is about, named once so a row silently renamed
// makes a mutator throw rather than makes a pin vacuous.
constexpr std::string_view kMemoryOpcodes[] = {"fldur", "fstur",
                                               "fldr_u", "fstr_u"};

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

// The FIRST 32-bit word of the emitted stream, little-endian. One AArch64
// instruction is exactly one word, so this is the WHOLE instruction and no
// field can be checked while a neighbour drifts.
[[nodiscard]] std::uint32_t firstWord(std::vector<std::uint8_t> const& b) {
    if (b.size() < 4) return 0;
    return static_cast<std::uint32_t>(b[0])
         | (static_cast<std::uint32_t>(b[1]) << 8)
         | (static_cast<std::uint32_t>(b[2]) << 16)
         | (static_cast<std::uint32_t>(b[3]) << 24);
}

[[nodiscard]] std::shared_ptr<TargetSchema> shippedTarget() {
    auto t = TargetSchema::loadShipped(kTarget);
    if (!t.has_value()) throw std::runtime_error{"cannot load shipped arm64"};
    return *t;
}

[[nodiscard]] std::unique_ptr<Run>
runOn(std::shared_ptr<GrammarSchema> dialect,
      std::shared_ptr<TargetSchema>  target,
      std::string_view               templateText) {
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

    run->ok = lowerAsmTemplateToLirRun(*tree, *run->dialect, *run->target,
                                       {}, builder, run->reporter);

    auto const retOp = run->target->opcodeByMnemonic("ret");
    if (!retOp.has_value()) throw std::runtime_error{"target has no `ret`"};
    builder.addReturn(*retOp, {});
    Lir lir = std::move(builder).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    auto const mod = assemble(lir, *run->target, lirToMir, run->asmReporter);
    if (mod.functions.size() == 1) run->bytes = mod.functions[0].bytes;
    return run;
}

[[nodiscard]] std::unique_ptr<Run> runBare(std::string_view templateText) {
    return runOn(loadDialect(), shippedTarget(), templateText);
}

// One spelling and the word BOTH references produce for it.
struct Case { char const* text; std::uint32_t want; char const* why; };

// The `ret` the harness appends after every template.
constexpr std::uint32_t kRet = 0xD65F03C0u;

// ★★★ "THE TEMPLATE CONTRIBUTED NO INSTRUCTION" HAS **TWO** BYTE SHAPES ON
// THIS PATH, AND EVERY REFUSAL PIN IN THIS FILE HAD TO LEARN THAT THE HARD WAY.
// ✔MEASURED: a LOWERING refusal (an unspellable register, an elected-nothing
// width) leaves the harness's appended `ret` and NOTHING ELSE — four bytes,
// 0xD65F03C0; an ENCODING refusal (an immediate with no representation) aborts
// the whole function, so the stream is EMPTY. A pin asserting either one alone
// is wrong for half the refusals it will meet, and a pin asserting
// `firstWord != <the right word>` passes on a stream that emitted the WRONG
// instruction. The invariant that covers both, and excludes that, is: at most
// the return, and if four bytes then exactly the return.
//
// ⚠ AND `ok` IS **NOT** THE FLAG TO TEST, which is the second thing this file
// got wrong. `ok` is the LOWERING verdict alone: an immediate with no
// representation lowers FINE (the LIR instruction is built) and dies in the
// ENCODER, so `ok` is true while the stream is empty. ✔MEASURED — an
// `EXPECT_FALSE(parsed && ok)` here reddened every encode-tier refusal in this
// file. The fail-loud claim these pins actually make is that a DIAGNOSTIC was
// reported, so that is what is asserted.
void expectContributedNothing(Run const& r, char const* text) {
    EXPECT_GT(r.reporter.errorCount() + r.asmReporter.errorCount(), 0u)
        << text << " produced no bytes and no diagnostic — a silent drop is "
                   "worse than either a refusal or a wrong answer";
    ASSERT_LE(r.bytes.size(), 4u)
        << text << " emitted an INSTRUCTION on top of the harness return: "
        << hex(r.bytes);
    if (r.bytes.size() == 4) {
        EXPECT_EQ(firstWord(r.bytes), kRet)
            << text << " emitted a four-byte word that is not the harness "
            << "`ret` — the template encoded something: " << hex(r.bytes);
    }
}

void expectWords(std::span<Case const> cases) {
    for (auto const& c : cases) {
        auto const r = runBare(c.text);
        ASSERT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.text << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.text << " emitted " << hex(r->bytes) << " — " << c.why;
    }
}

// ── THE MEASURED WORDS ─────────────────────────────────────────────────────
//
// ✔gas 2.42 AND clang 18.1.3, probed SEPARATELY, agreeing on every one.

// The S and D CONTROLS — already shipping before this row, and named here so a
// failure of the new arms can be told apart from a failure of the family.
constexpr std::uint32_t kLdrS   = 0xBD400420u;  // ldr  s0, [x1, #4]
constexpr std::uint32_t kLdrD   = 0xFD400420u;  // ldr  d0, [x1, #8]
constexpr std::uint32_t kStrD   = 0xFD000420u;  // str  d0, [x1, #8]
constexpr std::uint32_t kLdurD  = 0xFC5F8020u;  // ldur d0, [x1, #-8]

// `fldr_u` / `fstr_u` — the SCALED unsigned-offset forms, two placements each.
constexpr std::uint32_t kLdrH    = 0x7D400420u;  // ldr  h0,  [x1,  #2]
constexpr std::uint32_t kLdrH29  = 0x7D40121Du;  // ldr  h29, [x16, #8]
constexpr std::uint32_t kLdrB    = 0x3D400420u;  // ldr  b0,  [x1,  #1]
constexpr std::uint32_t kLdrB29  = 0x3D400E1Du;  // ldr  b29, [x16, #3]
constexpr std::uint32_t kLdrQ    = 0x3DC00420u;  // ldr  q0,  [x1,  #16]
constexpr std::uint32_t kLdrQ29  = 0x3DC00E1Du;  // ldr  q29, [x16, #48]
constexpr std::uint32_t kStrH    = 0x7D000420u;  // str  h0,  [x1,  #2]
constexpr std::uint32_t kStrH29  = 0x7D00121Du;  // str  h29, [x16, #8]
constexpr std::uint32_t kStrB    = 0x3D000420u;  // str  b0,  [x1,  #1]
constexpr std::uint32_t kStrB29  = 0x3D000E1Du;  // str  b29, [x16, #3]
constexpr std::uint32_t kStrQ    = 0x3D800420u;  // str  q0,  [x1,  #16]
constexpr std::uint32_t kStrQ29  = 0x3D800E1Du;  // str  q29, [x16, #48]

// `fldur` / `fstur` — the UNSCALED signed-imm9 forms, two placements each.
// ⚠ EVERY ONE OF THESE CARRIES A NEGATIVE OR NON-MULTIPLE DISPLACEMENT ON
// PURPOSE: an `imm12.scaled` slot cannot express one at all, so a variant that
// took the scaled twin's wire would fail loud here rather than pass quietly.
constexpr std::uint32_t kLdurH   = 0x7C5FE020u;  // ldur h0,  [x1,  #-2]
constexpr std::uint32_t kLdurH29 = 0x7C5FF21Du;  // ldur h29, [x16, #-1]
constexpr std::uint32_t kLdurB   = 0x3C5FF020u;  // ldur b0,  [x1,  #-1]
constexpr std::uint32_t kLdurB29 = 0x3C5F921Du;  // ldur b29, [x16, #-7]
constexpr std::uint32_t kLdurQ   = 0x3CDF0020u;  // ldur q0,  [x1,  #-16]
constexpr std::uint32_t kLdurQ29 = 0x3CDFF21Du;  // ldur q29, [x16, #-1]
constexpr std::uint32_t kSturH   = 0x7C1FE020u;  // stur h0,  [x1,  #-2]
constexpr std::uint32_t kSturH29 = 0x7C1FF21Du;  // stur h29, [x16, #-1]
constexpr std::uint32_t kSturB   = 0x3C1FF020u;  // stur b0,  [x1,  #-1]
constexpr std::uint32_t kSturB29 = 0x3C1F921Du;  // stur b29, [x16, #-7]
constexpr std::uint32_t kSturQ   = 0x3C9F0020u;  // stur q0,  [x1,  #-16]
constexpr std::uint32_t kSturQ29 = 0x3C9FF21Du;  // stur q29, [x16, #-1]

// ★★★ ONE BYTE OFFSET, FIVE FIELDS — the whole silent-wrong-answer fence in
// five constants. `[x1,#16]` at each width.
constexpr std::uint32_t kLdrB_16 = 0x3D404020u;  // ldr b0,[x1,#16]  imm12 = 16
constexpr std::uint32_t kLdrH_16 = 0x7D402020u;  // ldr h0,[x1,#16]  imm12 =  8
constexpr std::uint32_t kLdrS_16 = 0xBD401020u;  // ldr s0,[x1,#16]  imm12 =  4
constexpr std::uint32_t kLdrD_16 = 0xFD400820u;  // ldr d0,[x1,#16]  imm12 =  2
constexpr std::uint32_t kLdrQ_16 = 0x3DC00420u;  // ldr q0,[x1,#16]  imm12 =  1

// The per-width scaled REACH maxima (imm12 = 4095 at each access size).
constexpr std::uint32_t kLdrB_max = 0x3D7FFC20u;  // ldr b0,[x1,#4095]
constexpr std::uint32_t kLdrH_max = 0x7D7FFC20u;  // ldr h0,[x1,#8190]
constexpr std::uint32_t kLdrQ_max = 0x3DFFFC20u;  // ldr q0,[x1,#65520]

// ★★★ THE WORD AT THE HEART OF [[D-ASM-ARM64-BARE-V-REGISTER-ACCEPTED-IN-A-SCALAR-MEMORY-OPERAND]].
// ✔MEASURED three ways and they agree: gas 2.42 and clang 18.1.3 both emit it
// for `ldr q0,[x9,#16]`, and DSS emitted it at the P55 base for the BARE
// `ldr v0,[x9,#16]` — which is what made that spelling an undeclared alias
// rather than a wrong answer. Base x9 (not x1) because the base-register field
// is where an alias bug would hide if the ordinal were mis-read.
constexpr std::uint32_t kLdrQ_16_at_x9 = 0x3DC00520u;  // ldr q0, [x9, #16]

// The unscaled reach, which is the SAME ±256 at every width because the field
// is not scaled — the discriminator in the other direction.
constexpr std::uint32_t kLdurH_255  = 0x7C4FF020u;  // ldur h0,[x1,#255]
constexpr std::uint32_t kLdurH_m256 = 0x7C500020u;  // ldur h0,[x1,#-256]
constexpr std::uint32_t kLdurQ_255  = 0x3CCFF020u;  // ldur q0,[x1,#255]
constexpr std::uint32_t kLdurQ_m256 = 0x3CD00020u;  // ldur q0,[x1,#-256]

// ── THE TARGET-DOC MUTATORS (REMOVE direction, over the SHIPPED document) ───
//
// ⚠ REMOVE, NEVER ADD. An ADD-direction mutant stays green when the real
// config LOSES the feature, which is the direction that actually regresses;
// a REMOVE-direction one throws when the thing it aimed at is already gone.

// Delete every `width`-guarded variant of the four memory opcodes, refusing
// loudly if the count is not what this file was written against.
[[nodiscard]] std::function<void(nlohmann::json&)>
removeWidthArms(std::vector<int> widths) {
    return [widths](nlohmann::json& doc) {
        std::size_t removed = 0;
        for (auto& op : doc.at("opcodes")) {
            auto const mn = op.value("mnemonic", std::string{});
            bool subject = false;
            for (auto const& want : kMemoryOpcodes) if (mn == want) subject = true;
            if (!subject) continue;
            auto& vars = op.at("encoding").at("variants");
            for (auto it = vars.begin(); it != vars.end();) {
                int const w = it->at("guard").value("width", 0);
                if (std::find(widths.begin(), widths.end(), w) != widths.end()) {
                    it = vars.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        auto const expect = widths.size() * std::size(kMemoryOpcodes);
        if (removed != expect) {
            throw std::runtime_error{
                "expected to strip " + std::to_string(expect)
                + " width arm(s) from the four SIMD&FP memory opcodes, stripped "
                + std::to_string(removed)
                + " — the pin below would be vacuous"};
        }
    };
}

// Delete the `aliases` key from every register row that has one — AND the
// `nameRequiresLaneArrangement` beside it, because the two are ONE statement.
//
// ⚠ THE SECOND ERASE ARRIVED 2026-09-03 AND IS NOT A CONVENIENCE
// ([[D-ASM-ARM64-BARE-V-REGISTER-ACCEPTED-IN-A-SCALAR-MEMORY-OPERAND]]). A `v`
// row now says two things about spelling: `q0` is a second name for it, and its
// OWN name denotes it only with a lane arrangement. Erasing only the first
// leaves a register with NO bare spelling at all, which `validate()` refuses at
// LOAD — so the mutant would not load and this pin would go red for a reason
// that has nothing to do with what it is about. Erasing both restores exactly
// the pre-alias world the pin was written against: `v0` bare, `q0` unknown.
// ⓘ THAT LOAD REFUSAL IS ITSELF PINNED, separately and deliberately, by
// `ARowThatUnspellsItsOwnNameWithNoAliasIsRejectedAtLoad` below.
void removeEveryAlias(nlohmann::json& doc) {
    std::size_t removed = 0;
    std::size_t unspelled = 0;
    for (auto& r : doc.at("registers")) {
        if (!r.is_object()) continue;
        if (r.contains("aliases")) {
            r.erase("aliases");
            ++removed;
        }
        if (r.contains("nameRequiresLaneArrangement")) {
            r.erase("nameRequiresLaneArrangement");
            ++unspelled;
        }
    }
    if (removed != 32) {
        throw std::runtime_error{
            "expected 32 alias-bearing register rows in the shipped arm64 "
            "table (the `v` roots), found " + std::to_string(removed)
            + " — the pin below would be vacuous"};
    }
    if (unspelled != 32) {
        throw std::runtime_error{
            "expected the SAME 32 rows to carry `nameRequiresLaneArrangement`, "
            "found " + std::to_string(unspelled)
            + " — the two keys are one statement about one row's spellings, "
              "and a table where they have drifted apart is not the table "
              "this mutant was written against"};
    }
}

// Delete ONLY the `aliases` key, leaving `nameRequiresLaneArrangement` behind —
// the incoherent half-state `validate()` exists to refuse.
void removeAliasesButKeepTheLaneRequirement(nlohmann::json& doc) {
    std::size_t removed = 0;
    for (auto& r : doc.at("registers")) {
        if (r.is_object() && r.contains("aliases")
            && r.value("nameRequiresLaneArrangement", false)) {
            r.erase("aliases");
            ++removed;
        }
    }
    if (removed != 32) {
        throw std::runtime_error{
            "expected 32 rows carrying BOTH `aliases` and "
            "`nameRequiresLaneArrangement`, found " + std::to_string(removed)
            + " — the pin below would be vacuous"};
    }
}

// Delete `nameRequiresLaneArrangement` from every row that carries it, leaving
// the aliases in place: the REMOVE-direction mutant for the bare-`v` refusal.
void removeTheLaneRequirement(nlohmann::json& doc) {
    std::size_t removed = 0;
    for (auto& r : doc.at("registers")) {
        if (r.is_object() && r.contains("nameRequiresLaneArrangement")) {
            r.erase("nameRequiresLaneArrangement");
            ++removed;
        }
    }
    if (removed != 32) {
        throw std::runtime_error{
            "expected 32 rows carrying `nameRequiresLaneArrangement` in the "
            "shipped arm64 table (the `v` roots), found "
            + std::to_string(removed) + " — the pin below would be vacuous"};
    }
}

// Delete every `immMultipleOf` guard from the SCALED memory opcodes: the
// REMOVE-direction mutant for the `ldr`→`ldur` fallback's correctness half.
void removeTheDivisibilityGuards(nlohmann::json& doc) {
    std::size_t removed = 0;
    for (auto& op : doc.at("opcodes")) {
        if (!op.is_object() || !op.contains("encoding")) continue;
        for (auto& v : op.at("encoding").at("variants")) {
            if (v.contains("guard") && v.at("guard").contains("immMultipleOf")) {
                v.at("guard").erase("immMultipleOf");
                ++removed;
            }
        }
    }
    // 4 (load_u) + 4 (store_u) + 5 (fldr_u) + 5 (fstr_u) MINUS the four
    // access-size-1 arms, which declare no modulus because every integer is a
    // multiple of 1 (`validate()` refuses a modulus of 1 outright).
    if (removed != 14) {
        throw std::runtime_error{
            "expected 14 `immMultipleOf` guards on the shipped arm64 scaled "
            "memory opcodes, found " + std::to_string(removed)
            + " — the pin below would be vacuous"};
    }
}

[[nodiscard]] std::string whyNotLoaded(
        std::vector<ConfigDiagnostic> const& errs) {
    std::string out;
    for (auto const& e : errs) { out += e.path + ": " + e.message + "\n"; }
    return out;
}

} // namespace

// ── (1) THE SCALED FORMS — `ldr` / `str` at H, B and Q ─────────────────────

TEST(AsmArm64MemoryRows, ScaledHalfByteQuadLoadsEncodeTheMeasuredWords) {
    constexpr Case kCases[] = {
        {"ldr h0, [x1, #2]\n",    kLdrH,   "LDR Ht, base 0x7D400000, imm12 = 2/2"},
        {"ldr h29, [x16, #8]\n",  kLdrH29, "the second (Rt,Rn) placement of the H arm"},
        {"ldr b0, [x1, #1]\n",    kLdrB,   "LDR Bt, base 0x3D400000, imm12 = 1/1"},
        {"ldr b29, [x16, #3]\n",  kLdrB29, "the second (Rt,Rn) placement of the B arm"},
        {"ldr q0, [x1, #16]\n",   kLdrQ,   "LDR Qt — the `q0` SPELLING must resolve"},
        {"ldr q29, [x16, #48]\n", kLdrQ29, "the second (Rt,Rn) placement of the Q arm"},
    };
    expectWords(kCases);
}

TEST(AsmArm64MemoryRows, ScaledHalfByteQuadStoresEncodeTheMeasuredWords) {
    constexpr Case kCases[] = {
        {"str h0, [x1, #2]\n",    kStrH,   "STR Ht, base 0x7D000000"},
        {"str h29, [x16, #8]\n",  kStrH29, "the second placement of the H store"},
        {"str b0, [x1, #1]\n",    kStrB,   "STR Bt, base 0x3D000000"},
        {"str b29, [x16, #3]\n",  kStrB29, "the second placement of the B store"},
        {"str q0, [x1, #16]\n",   kStrQ,   "STR Qt, base 0x3D800000"},
        {"str q29, [x16, #48]\n", kStrQ29, "the second placement of the Q store"},
    };
    expectWords(kCases);
}

// ── (2) THE UNSCALED FORMS — `ldur` / `stur` at H, B and Q ────────────────

TEST(AsmArm64MemoryRows, UnscaledHalfByteQuadLoadsEncodeTheMeasuredWords) {
    constexpr Case kCases[] = {
        {"ldur h0, [x1, #-2]\n",    kLdurH,   "LDUR Ht, base 0x7C400000, imm9 = -2"},
        {"ldur h29, [x16, #-1]\n",  kLdurH29, "the second placement of the H arm"},
        {"ldur b0, [x1, #-1]\n",    kLdurB,   "LDUR Bt, base 0x3C400000"},
        {"ldur b29, [x16, #-7]\n",  kLdurB29, "the second placement of the B arm"},
        {"ldur q0, [x1, #-16]\n",   kLdurQ,   "LDUR Qt, base 0x3CC00000"},
        {"ldur q29, [x16, #-1]\n",  kLdurQ29, "the second placement of the Q arm"},
    };
    expectWords(kCases);
}

TEST(AsmArm64MemoryRows, UnscaledHalfByteQuadStoresEncodeTheMeasuredWords) {
    constexpr Case kCases[] = {
        {"stur h0, [x1, #-2]\n",   kSturH,   "STUR Ht, base 0x7C000000"},
        {"stur h29, [x16, #-1]\n", kSturH29, "the second placement of the H store"},
        {"stur b0, [x1, #-1]\n",   kSturB,   "STUR Bt, base 0x3C000000"},
        {"stur b29, [x16, #-7]\n", kSturB29, "the second placement of the B store"},
        {"stur q0, [x1, #-16]\n",  kSturQ,   "STUR Qt, base 0x3C800000"},
        {"stur q29, [x16, #-1]\n", kSturQ29, "the second placement of the Q store"},
    };
    expectWords(kCases);
}

// ★ THE CONTROLS, NAMED. These four shipped BEFORE this row; a red here says
// the family broke, not that the new arms are wrong.
TEST(AsmArm64MemoryRows, TheSingleAndDoubleControlsDidNotMove) {
    constexpr Case kCases[] = {
        {"ldr s0, [x1, #4]\n",    kLdrS,  "CONTROL — the S arm, unchanged"},
        {"ldr d0, [x1, #8]\n",    kLdrD,  "CONTROL — the D arm, unchanged"},
        {"str d0, [x1, #8]\n",    kStrD,  "CONTROL — the D store, unchanged"},
        {"ldur d0, [x1, #-8]\n",  kLdurD, "CONTROL — the unscaled D load, unchanged"},
    };
    expectWords(kCases);
}

// ── (3) THE NEAR-MISS FENCE — the silent-wrong-answer risk in this row ─────

// ★★★ ONE BYTE OFFSET, FIVE WIDTHS, FIVE DIFFERENT FIELDS. If any arm borrowed
// a neighbour's scale, THIS is where it shows: not as a refusal but as a
// perfectly valid instruction addressing the wrong memory. `ldr q0,[x1,#16]`
// and `ldr d0,[x1,#8]` even produce the SAME low half (0x0420) at different
// bases, which is exactly how a scale bug looks right at a glance.
TEST(AsmArm64MemoryRows, NearMissFenceOneOffsetEncodesFiveFields) {
    constexpr Case kCases[] = {
        {"ldr b0, [x1, #16]\n", kLdrB_16, "B: accessSize 1 ⇒ imm12 16"},
        {"ldr h0, [x1, #16]\n", kLdrH_16, "H: accessSize 2 ⇒ imm12 8"},
        {"ldr s0, [x1, #16]\n", kLdrS_16, "S: accessSize 4 ⇒ imm12 4 (control)"},
        {"ldr d0, [x1, #16]\n", kLdrD_16, "D: accessSize 8 ⇒ imm12 2 (control)"},
        {"ldr q0, [x1, #16]\n", kLdrQ_16, "Q: accessSize 16 ⇒ imm12 1"},
    };
    expectWords(kCases);
}

// ★★★ A NON-MULTIPLE OF THE ACCESS SIZE HAS NO SCALED ENCODING, SO `ldr` TAKES
// THE **UNSCALED** ONE — [[D-ASM-ARM64-LDR-TO-LDUR-CONVENIENCE-ALIAS-REFUSED]],
// closed 2026-09-03 (cycle P55, lane vd).
//
// ⚠⚠ THIS TEST USED TO ASSERT THE OPPOSITE, AND THE OLD ASSERTION IS RESTATED
// RATHER THAN QUIETLY REPLACED. It pinned that DSS REFUSED these spellings
// "NAMING THE SIZE", above a note reading *✔MEASURED DIVERGENCE, REPORTED NOT
// FIXED HERE: both references ACCEPT these spellings and silently re-write them
// to the UNSCALED form*. The measurement was right and is re-confirmed below;
// what was wrong was that DSS stayed below the union on it. The refusal it
// pinned is GONE because the input now COMPILES — there is no diagnostic left
// to keep specific, and the fence it was really protecting (a wrong
// displacement) is now pinned by the WORD instead of by the message, which is
// strictly stronger: a message can only say the encoding was declined, while
// the word says which memory is addressed.
//
// ✔RE-MEASURED 2026-09-03, gas 2.42 and clang 18.1.3 probed SEPARATELY,
// agreeing on every cell, and NEITHER emitting a warning.
TEST(AsmArm64MemoryRows, NonMultipleOffsetTakesTheUnscaledEncoding) {
    constexpr Case kCases[] = {
        {"ldr h0, [x1, #1]\n",  0x7C401020u, "H #1: 1 is not a multiple of 2"},
        {"ldr h0, [x1, #3]\n",  0x7C403020u, "H #3: same, imm9 = 3"},
        {"ldr q0, [x1, #8]\n",  0x3CC08020u, "Q #8: 8 is not a multiple of 16"},
        {"ldr q0, [x1, #4]\n",  0x3CC04020u, "Q #4: same, imm9 = 4"},
        {"ldr s0, [x1, #2]\n",  0xBC402020u, "S #2 — the PRE-EXISTING width, "
                                             "which diverged too"},
        {"ldr d0, [x1, #4]\n",  0xFC404020u, "D #4 — likewise"},
    };
    expectWords(kCases);
}

// ★★★ THE SCALED FORM IS PREFERRED WHERE **BOTH** ENCODINGS FIT, which is the
// half of the rank that a fallback-only implementation would get wrong while
// looking correct. ✔MEASURED: every one of these is representable as an imm9
// too, and both references still choose the scaled word.
TEST(AsmArm64MemoryRows, TheScaledEncodingWinsWhereBothWouldFit) {
    constexpr Case kCases[] = {
        {"ldr h0, [x1, #2]\n",   0x7D400420u, "H #2: imm9 could carry 2"},
        {"ldr h0, [x1, #254]\n", 0x7D41FC20u, "H #254: still inside imm9"},
        {"ldr s0, [x1, #8]\n",   0xBD400820u, "S #8: likewise"},
        {"ldr d0, [x1, #8]\n",   0xFD400420u, "D #8: likewise"},
        {"ldr q0, [x1, #16]\n",  0x3DC00420u, "Q #16: likewise"},
        {"ldr b0, [x1, #1]\n",   0x3D400420u, "B: EVERY non-negative offset is "
                                              "a multiple of 1, so B never "
                                              "falls back on the positive side"},
        {"ldr b0, [x1, #255]\n", 0x3D43FC20u, "B #255: scaled, not unscaled"},
    };
    expectWords(kCases);
}

// ★★★ A NEGATIVE OFFSET FALLS BACK **EVEN WHEN IT IS A MULTIPLE**, and this is
// the case the row's own premise MISSED. The row framed the trigger as *an
// offset that is not a multiple of the access size*; ✔MEASURED, that is not the
// predicate. The scaled field is UNSIGNED, so no negative offset has a scaled
// encoding at any width — including `b`, where every offset is a multiple of 1
// and "non-multiple" cannot be the reason at all.
TEST(AsmArm64MemoryRows, ANegativeOffsetFallsBackEvenWhenItIsAMultiple) {
    constexpr Case kCases[] = {
        {"ldr b0, [x1, #-1]\n",  0x3C5FF020u, "B: a multiple of 1, still "
                                              "unscaled — the row's premise "
                                              "cannot explain this one"},
        {"ldr h0, [x1, #-2]\n",  0x7C5FE020u, "H: a multiple of 2"},
        {"ldr s0, [x1, #-4]\n",  0xBC5FC020u, "S: a multiple of 4"},
        {"ldr d0, [x1, #-8]\n",  0xFC5F8020u, "D: a multiple of 8"},
        {"ldr q0, [x1, #-16]\n", 0x3CDF0020u, "Q: a multiple of 16"},
        {"str d0, [x1, #-8]\n",  0xFC1F8020u, "the STORE direction behaves "
                                              "identically"},
    };
    expectWords(kCases);
}

// ★★★ THE INTEGER FILE REWRITES IDENTICALLY — the second half of the row's
// premise that was narrower than the truth. It scoped the divergence to the
// SIMD&FP widths; ✔MEASURED, `load_u`/`store_u` diverged in exactly the same
// way and are fixed by the same two keys.
TEST(AsmArm64MemoryRows, TheIntegerFileFallsBackTheSameWay) {
    constexpr Case kCases[] = {
        {"ldr x0, [x1, #1]\n",  0xF8401020u, "X #1: not a multiple of 8"},
        {"ldr w0, [x1, #1]\n",  0xB8401020u, "W #1: not a multiple of 4"},
        {"ldr x0, [x1, #-8]\n", 0xF85F8020u, "X #-8: negative"},
        {"str x0, [x1, #1]\n",  0xF8001020u, "the store direction"},
        {"ldr x0, [x1, #8]\n",  0xF9400420u, "CONTROL: the scaled word is "
                                             "still chosen where it fits"},
    };
    expectWords(kCases);
}

// ★★★ AN EXPLICITLY-WRITTEN `ldur`/`stur` IS **NEVER PROMOTED** TO THE SCALED
// FORM, which is the asymmetry that keeps the rank honest. `ldr` names the
// OPERATION and the assembler picks the encoding; `ldur` names the ENCODING and
// must always get it. ✔MEASURED that both references are asymmetric in exactly
// this way — an offset the scaled form could carry does NOT move it.
TEST(AsmArm64MemoryRows, AnExplicitUnscaledSpellingIsNeverPromoted) {
    constexpr Case kCases[] = {
        {"ldur h0, [x1, #2]\n",  0x7C402020u, "a multiple of 2, still unscaled"},
        {"ldur x0, [x1, #8]\n",  0xF8408020u, "a multiple of 8, still unscaled"},
        {"ldur s0, [x1, #4]\n",  0xBC404020u, "a multiple of 4, still unscaled"},
        {"stur d0, [x1, #8]\n",  0xFC008020u, "the store direction"},
    };
    expectWords(kCases);
}

// ★★★ THE FENCE THE RANK MUST NOT BREACH: an offset OUTSIDE BOTH reaches is
// still refused, loudly. This is where a wrong displacement would be silently
// encoded if the fallback leaked past imm9, so it is pinned at every width and
// in both directions. ✔MEASURED that both references refuse every one of these.
TEST(AsmArm64MemoryRows, AnOffsetOutsideBothReachesIsStillRefused) {
    for (auto const* past : {"ldr h0, [x1, #257]\n",   // non-multiple, > imm9
                             "ldr s0, [x1, #257]\n",
                             "ldr d0, [x1, #257]\n",
                             "ldr q0, [x1, #257]\n",
                             "ldr q0, [x1, #300]\n",
                             "str q0, [x1, #257]\n",
                             "ldr b0, [x1, #-257]\n",  // negative, < imm9
                             "ldr h0, [x1, #-258]\n",
                             "ldr q0, [x1, #-264]\n"}) {
        auto const r = runBare(past);
        ASSERT_TRUE(r->parsed) << past << "\n" << messages(*r);
        expectContributedNothing(*r, past);
        EXPECT_NE(messages(*r).find("signed 9-bit"), std::string::npos)
            << past << " must be refused by the UNSCALED reach — it is the "
               "last candidate the rank reaches, and a message about anything "
               "else means the fallback was never tried: " << messages(*r);
    }
}

// ★★ THE SCALED REACH IS A FUNCTION OF THE ACCESS SIZE, so a pin at one width's
// maximum says nothing about another's: 4095 bytes at B, 8190 at H, 65520 at Q.
TEST(AsmArm64MemoryRows, NearMissFenceScaledReachIsPerWidth) {
    constexpr Case kMax[] = {
        {"ldr b0, [x1, #4095]\n",  kLdrB_max, "B reach = 4095 x 1"},
        {"ldr h0, [x1, #8190]\n",  kLdrH_max, "H reach = 4095 x 2"},
        {"ldr q0, [x1, #65520]\n", kLdrQ_max, "Q reach = 4095 x 16"},
    };
    expectWords(kMax);

    // One access PAST each maximum — a refusal, never a wrapped field.
    // ⚠ THE REFUSAL NOW COMES FROM THE **UNSCALED** REACH, NOT THE SCALED ONE,
    // and the changed message is the evidence that the rank really runs
    // ([[D-ASM-ARM64-LDR-TO-LDUR-CONVENIENCE-ALIAS-REFUSED]], 2026-09-03). This
    // loop used to assert *"exceeds the unsigned 12-bit"*: past the scaled
    // maximum the scaled candidate was the ONLY one `ldr` had, so its encoder
    // spoke. `ldr` now ranks the unscaled form behind it, so the last candidate
    // to be tried is `fldur`/`load` and the imm9 reach is what these offsets
    // fall outside of. ✔MEASURED that both references refuse all three, so the
    // ACCEPTANCE answer is unchanged and only the message moved.
    for (auto const* past : {"ldr b0, [x1, #4096]\n",
                             "ldr h0, [x1, #8192]\n",
                             "ldr q0, [x1, #65536]\n"}) {
        auto const r = runBare(past);
        ASSERT_TRUE(r->parsed) << past << "\n" << messages(*r);
        expectContributedNothing(*r, past);
        EXPECT_NE(messages(*r).find("signed 9-bit"), std::string::npos)
            << past << " was refused for some other reason: " << messages(*r);
    }
}

// ★★ AND THE UNSCALED REACH IS **NOT** A FUNCTION OF THE WIDTH — ±256 at every
// one, because the imm9 is not scaled. That asymmetry is the second half of the
// fence: an `fldur` arm wired to `imm12.scaled` by copy-paste would refuse the
// negative displacements above AND would accept 8190 here.
TEST(AsmArm64MemoryRows, NearMissFenceUnscaledReachIsWidthIndependent) {
    constexpr Case kEdge[] = {
        {"ldur h0, [x1, #255]\n",  kLdurH_255,  "H: +255, the imm9 maximum"},
        {"ldur h0, [x1, #-256]\n", kLdurH_m256, "H: -256, the imm9 minimum"},
        {"ldur q0, [x1, #255]\n",  kLdurQ_255,  "Q: the SAME +255, unscaled"},
        {"ldur q0, [x1, #-256]\n", kLdurQ_m256, "Q: the SAME -256, unscaled"},
    };
    expectWords(kEdge);

    for (auto const* past : {"ldur h0, [x1, #256]\n",
                             "ldur q0, [x1, #256]\n",
                             "ldur q0, [x1, #-257]\n"}) {
        auto const r = runBare(past);
        ASSERT_TRUE(r->parsed) << past << "\n" << messages(*r);
        expectContributedNothing(*r, past);
        EXPECT_NE(messages(*r).find("signed 9-bit"), std::string::npos)
            << past << " was refused for some other reason: " << messages(*r);
    }
}

// ── (4) THE ALIAS ITSELF — one register, two spellings ────────────────────

// ★★★ `q0` AND `v0` ARE ONE ORDINAL, and that is the whole architectural
// claim. A second ROW would have been a second ordinal — a second free-list
// entry the moment anything made it allocatable, and an `asm` clobber naming
// `q0` protecting a register the allocator never hands out while `v0` stayed
// free. The identity below is what makes that unreachable BY CONSTRUCTION.
TEST(AsmArm64MemoryRows, QIsASecondSpellingOfTheSameOrdinalNotASecondRegister) {
    auto const target = shippedTarget();

    std::size_t aliasRows = 0;
    for (std::size_t i = 0; i < target->registerCount(); ++i) {
        auto const* info = target->registerInfo(static_cast<std::uint16_t>(i));
        ASSERT_NE(info, nullptr);
        if (info->aliases.empty()) continue;
        ++aliasRows;
        for (auto const& alias : info->aliases) {
            auto const byAlias = target->registerByName(alias);
            auto const byName  = target->registerByName(info->name);
            ASSERT_TRUE(byAlias.has_value()) << alias;
            ASSERT_TRUE(byName.has_value()) << info->name;
            EXPECT_EQ(*byAlias, *byName)
                << alias << " and " << info->name << " must resolve to ONE "
                << "ordinal — two would be two registers, and the allocator "
                << "would eventually hand both out";
            EXPECT_NE(alias, info->name);
        }
    }
    EXPECT_EQ(aliasRows, 32u)
        << "arm64 must declare exactly one alias per `v` root — the aliasing "
           "mechanism has lost its consumer if this is 0";

    // No `q` row exists: the spelling resolves, the ROW does not.
    for (auto const* q : {"q0", "q7", "q31"}) {
        auto const ord = target->registerByName(q);
        ASSERT_TRUE(ord.has_value()) << q << " must resolve";
        auto const* info = target->registerInfo(*ord);
        ASSERT_NE(info, nullptr);
        EXPECT_NE(info->name, q)
            << q << " resolved to a row of its OWN — the alias became a second "
                    "register row, which is exactly the shape this design "
                    "rejects";
        EXPECT_EQ(info->widthBytes, 16u);
        EXPECT_EQ(info->regClass, TargetRegClass::FPR);
    }

    // ✔MEASURED: `ldr q3,[x1]` = 0x3DC00023 (gas 2.42 + clang 18.1.3). The
    // encoding comes from the `v3` row the alias resolves to, so a wrong
    // resolution is a wrong Rt here, not a refusal.
    auto const r = runBare("ldr q3, [x1]\n");
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_TRUE(r->ok) << messages(*r);
    EXPECT_EQ(firstWord(r->bytes), 0x3DC00023u)
        << "the alias must carry its ROOT's hwEncoding: " << hex(r->bytes);
}

// ── (5) THE RED-ON-DISABLE MUTANTS — one per half, REMOVE direction ────────

// ★★ HALF ONE: strip the width-16 and width-8 arms from all four opcodes. The
// `h`/`b` spellings must stop lowering; the S CONTROL must stay green, and it
// is printed BY NAME so a mutant that broke everything cannot pass for a mutant
// that broke the right thing.
TEST(AsmArm64MemoryRows, RemovingTheHalfAndByteWidthArmsUnspellsThem) {
    auto const mutant =
        mutateShippedTargetSchemaDoc(kTarget, removeWidthArms({16, 8}));
    ASSERT_TRUE(mutant.has_value())
        << "the mutant target did not load: " << whyNotLoaded(mutant.error());

    for (auto const* text : {"ldr h0, [x1, #2]\n",  "str h0, [x1, #2]\n",
                             "ldur h0, [x1, #-2]\n", "stur h0, [x1, #-2]\n",
                             "ldr b0, [x1, #1]\n",  "str b0, [x1, #1]\n",
                             "ldur b0, [x1, #-1]\n", "stur b0, [x1, #-1]\n"}) {
        auto const r = runOn(loadDialect(), *mutant, text);
        expectContributedNothing(*r, text);
    }

    // THE CONTROL, printed by name: the SAME mutant, the SAME dialect, a width
    // the mutation did not touch.
    auto const control = runOn(loadDialect(), *mutant, "ldr s0, [x1, #4]\n");
    ASSERT_TRUE(control->parsed) << "CONTROL `ldr s0, [x1, #4]`: "
                                << messages(*control);
    EXPECT_TRUE(control->ok) << "CONTROL `ldr s0, [x1, #4]`: "
                             << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kLdrS)
        << "CONTROL `ldr s0, [x1, #4]` must still encode 0xBD400420 under the "
           "mutant — if it does not, the mutation broke the family and the "
           "refusals above prove nothing about the H and B arms: "
        << hex(control->bytes);

    // ★ AND THE Q SPELLING MUST STILL WORK under this mutant: the two halves of
    // this row are independent, and a mutant that took both out would let one
    // pin cover for the other.
    auto const q = runOn(loadDialect(), *mutant, "ldr q0, [x1, #16]\n");
    ASSERT_TRUE(q->parsed) << messages(*q);
    EXPECT_TRUE(q->ok) << messages(*q);
    EXPECT_EQ(firstWord(q->bytes), kLdrQ)
        << "the Q half rides the alias, not the width arms this mutant "
           "deleted: " << hex(q->bytes);
}

// ★★ HALF TWO: strip `aliases` from every `v` row. The `q` spellings must stop
// resolving; the D CONTROL must stay green, by name.
TEST(AsmArm64MemoryRows, RemovingTheAliasesUnspellsEveryQRegister) {
    auto const mutant = mutateShippedTargetSchemaDoc(kTarget, removeEveryAlias);
    ASSERT_TRUE(mutant.has_value())
        << "the mutant target did not load: " << whyNotLoaded(mutant.error());

    EXPECT_FALSE((*mutant)->registerByName("q0").has_value())
        << "`q0` still resolves with every alias deleted — the register table "
           "grew a `q` row somewhere and the pins above are not reading the "
           "alias mechanism";

    for (auto const* text : {"ldr q0, [x1, #16]\n",  "str q0, [x1, #16]\n",
                             "ldur q0, [x1, #-16]\n", "stur q0, [x1, #-16]\n",
                             "ldr q3, [x1]\n"}) {
        auto const r = runOn(loadDialect(), *mutant, text);
        expectContributedNothing(*r, text);
    }

    auto const control = runOn(loadDialect(), *mutant, "ldr d0, [x1, #8]\n");
    ASSERT_TRUE(control->parsed) << "CONTROL `ldr d0, [x1, #8]`: "
                                << messages(*control);
    EXPECT_TRUE(control->ok) << "CONTROL `ldr d0, [x1, #8]`: "
                             << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kLdrD)
        << "CONTROL `ldr d0, [x1, #8]` must still encode 0xFD400420 under the "
           "alias-stripped mutant: " << hex(control->bytes);

    // ★ AND THE H HALF MUST STILL WORK: independent again, in the other
    // direction.
    auto const h = runOn(loadDialect(), *mutant, "ldr h0, [x1, #2]\n");
    ASSERT_TRUE(h->parsed) << messages(*h);
    EXPECT_TRUE(h->ok) << messages(*h);
    EXPECT_EQ(firstWord(h->bytes), kLdrH)
        << "the H half rides the width arms, not the alias this mutant "
           "deleted: " << hex(h->bytes);
}

// ── (6) THE DOUBLE-COUNT NEGATIVE, SYNTHESIZED ────────────────────────────

// ★★★ THE ALLOCATOR MAY NEVER HAND `q0` AND `v0` TO TWO LIVE VALUES
// ([[D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE]]).
//
// ⚠ THE NEGATIVE IS SYNTHESIZED, NOT OBSERVED. The shipped table is quiet here
// and its silence proves nothing: this test WRITES the violating config — an
// `argFprs` naming the alias spelling — and asserts the load REFUSES it by
// name. An ADD-direction fixture (checking the shipped lists are clean) stays
// green the day the rule is deleted.
//
// ★ THE REFUSAL IS ABOUT `buildFreeLists`, WHICH MATCHES ON THE ROW'S CANONICAL
// NAME: an alias in a cc list matches NO row, so the register is silently
// DROPPED from the pool rather than double-counted — the producer declares a
// register, gets no register, and gets no diagnostic. That is the same failure
// D-TARGET-CC-NAMES-SUB-REGISTER refuses for `subOf`, reached through the
// other key.
TEST(AsmArm64MemoryRows, ACallingConventionNamingTheAliasIsRejectedAtLoad) {
    bool sawViolation = false;
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [&sawViolation](nlohmann::json& doc) {
            for (auto& cc : doc.at("callingConventions")) {
                auto& pool = cc.at("argFprs");
                if (pool.empty()) continue;
                // Replace the FIRST arg register with its alias spelling: one
                // physical register, named the way the pool may not name it.
                pool[0] = "q0";
                sawViolation = true;
            }
            if (!sawViolation) {
                throw std::runtime_error{
                    "no calling convention declares a non-empty `argFprs` on "
                    "arm64 — this negative would be vacuous"};
            }
        });

    ASSERT_FALSE(mutant.has_value())
        << "a calling convention naming the ALIAS spelling `q0` loaded "
           "cleanly. The allocatable pools are matched against each row's "
           "canonical name, so that config declares a register and gets none — "
           "silently.";

    std::string const why = whyNotLoaded(mutant.error());
    EXPECT_NE(why.find("q0"), std::string::npos)
        << "the refusal must name the spelling that was written: " << why;
    EXPECT_NE(why.find("v0"), std::string::npos)
        << "the refusal must name the canonical spelling to use instead: "
        << why;
    EXPECT_NE(why.find("ALIAS"), std::string::npos)
        << "the refusal must say WHY, not merely that something is wrong: "
        << why;
}

// ★★ AND THE OTHER SPELLING OF THE SAME MISTAKE: a target that declares the
// second name as a same-width `subOf` ROW instead of an alias. That is what
// mints the second ordinal, so it is refused at LOAD rather than left to a
// test over two shipped tables.
//
// ⚠ ✔MEASURED AT THE P55 BASE THAT THIS LOADED CLEAN: the strictly-wider rule
// lived only in this one test over in `test_asm_text_to_lir.cpp` —
// `TargetSubRegisters.NoSubRegisterAppearsInAnyCallingConventionList` — which
// judges the two SHIPPED tables and nothing else.
// ⓘ THE ROW IS SPELLED `qq0`, NOT `q0`, AND THE NAME IS LOAD-BEARING. Written
// as `q0` this fixture measured the WRONG RULE: `q0` is already a spelling on
// this target (it is `v0`'s alias), so the loader's duplicate-name check fired
// FIRST and the refusal said *duplicate register name 'q0'* — a true refusal of
// a different mistake, and a green test that never reached the width rule.
// ✔MEASURED before the rename; kept as a comment because it is the exact shape
// of a fixture that passes for the wrong reason.
TEST(AsmArm64MemoryRows, ASameWidthSubOfRowIsRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            nlohmann::json row;
            row["name"]       = "qq0";       // a name this target does not use
            row["class"]      = "fpr";
            row["widthBytes"] = 16;          // the SAME width as its parent
            row["subOf"]      = "v0";
            row["hwEncoding"] = 0;
            doc.at("registers").push_back(std::move(row));
        });

    ASSERT_FALSE(mutant.has_value())
        << "a same-width `subOf` row loaded cleanly — `subOf` asserts "
           "CONTAINMENT, and a second ordinal for one machine register is what "
           "the `aliases` key exists to avoid";

    std::string const why = whyNotLoaded(mutant.error());
    EXPECT_NE(why.find("STRICTLY WIDER"), std::string::npos)
        << "the refusal must say what `subOf` asserts: " << why;
    EXPECT_NE(why.find("aliases"), std::string::npos)
        << "the refusal must point at the key that DOES express this: " << why;
}

// ★ A THIRD WAY TO WRITE IT WRONG: an alias that collides with a name the
// table already declares. Resolving it to whichever row came first is a
// wrong-register answer with no diagnostic, so it is a load error.
//
// ⚠ BOTH DECLARATION ORDERS ARE EXERCISED, because they reach DIFFERENT
// MESSAGES and only one of them is the new rule's. `v31` claiming `v0` (already
// in the index) is refused by the alias-collision check; `v1` claiming `d0`
// (declared LATER in the file) is refused when `d0`'s own ROW cannot enter the
// index. A fixture taking only the second order would leave the alias check
// untested while looking like it tested it.
TEST(AsmArm64MemoryRows, AnAliasCollidingWithADeclaredNameIsRejectedAtLoad) {
    auto const earlier = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            for (auto& r : doc.at("registers")) {
                if (r.value("name", std::string{}) != "v31") continue;
                r["aliases"] = nlohmann::json::array({"v0"});
                return;
            }
            throw std::runtime_error{"arm64 declares no `v31` row"};
        });
    ASSERT_FALSE(earlier.has_value())
        << "`v31` claiming the alias `v0` — a register declared EARLIER — "
           "loaded cleanly; `v0` would then resolve to whichever entry won the "
           "index";
    EXPECT_NE(whyNotLoaded(earlier.error()).find("already a declared register"),
              std::string::npos)
        << whyNotLoaded(earlier.error());

    auto const later = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            for (auto& r : doc.at("registers")) {
                if (r.value("name", std::string{}) != "v1") continue;
                r["aliases"] = nlohmann::json::array({"d0"});
                return;
            }
            throw std::runtime_error{"arm64 declares no `v1` row"};
        });
    ASSERT_FALSE(later.has_value())
        << "`v1` claiming the alias `d0` — a register declared LATER — loaded "
           "cleanly, which would leave `d0` resolving to `v1`";
    EXPECT_NE(whyNotLoaded(later.error()).find("duplicate register name 'd0'"),
              std::string::npos)
        << "the later-order collision must be caught where the ROW fails to "
           "enter the index, naming `d0`: " << whyNotLoaded(later.error());
}

// ── (7) THE SHIPPED TABLES, AS A CONTROL ──────────────────────────────────

// ★ The invariants the two rules above enforce, asserted over what actually
// ships. This is the POSITIVE control for the two negatives: they prove the
// rule fires, this proves the shipped configs are on the legal side of it.
TEST(AsmArm64MemoryRows, ShippedTargetsSatisfyTheAliasAndSubOfInvariants) {
    for (auto const* name : {"x86_64", "arm64"}) {
        auto const schemaR = TargetSchema::loadShipped(name);
        ASSERT_TRUE(schemaR.has_value()) << name;
        auto const& schema = **schemaR;

        for (auto const& r : schema.registers()) {
            if (!r.subOf.empty()) {
                auto const parent = schema.registerByName(r.subOf);
                ASSERT_TRUE(parent.has_value()) << r.name;
                auto const* p = schema.registerInfo(*parent);
                ASSERT_NE(p, nullptr);
                EXPECT_GT(p->widthBytes, r.widthBytes)
                    << name << ": `" << r.name << "` is a `subOf` view, so its "
                    << "parent must be STRICTLY wider";
            }
            for (auto const& alias : r.aliases) {
                auto const ord = schema.registerByName(alias);
                ASSERT_TRUE(ord.has_value()) << name << ": " << alias;
                auto const* got = schema.registerInfo(*ord);
                ASSERT_NE(got, nullptr);
                EXPECT_EQ(got->name, r.name)
                    << name << ": alias `" << alias << "` must resolve to the "
                    << "row that declares it";
                // An alias may never be spellable in a convention.
                for (auto const& cc : schema.callingConventions()) {
                    for (auto const* list : {&cc.argGprs, &cc.argFprs,
                                             &cc.returnGprs, &cc.returnFprs,
                                             &cc.callerSaved, &cc.calleeSaved}) {
                        for (auto const& n : *list) {
                            EXPECT_NE(n, alias)
                                << name << ": convention '" << cc.name
                                << "' names the alias spelling `" << alias
                                << "` — the pool is matched on canonical names, "
                                   "so that silently drops `" << r.name << "`";
                        }
                    }
                }
            }
        }
    }
}

// ★★ THE TWO BYTE SHAPES OF A REFUSAL, PINNED APART — the premise
// `expectContributedNothing` rests on, and the one this file got wrong first.
// ✔MEASURED: an ENCODING refusal aborts the function and emits NOTHING; a
// LOWERING refusal emits the harness's `ret` and nothing else. A refusal pin
// written for one shape is vacuous or false for the other, which is why the
// helper accepts both and rejects everything else.
TEST(AsmArm64MemoryRows, ARefusalHasTwoByteShapesAndBothAreEmptyOfInstructions) {
    // ENCODE-tier: the width arm EXISTS, the immediate has no representation.
    // ⚠ THE EXAMPLE MOVED FROM `#3` TO `#300` ON 2026-09-03 AND THE REASON IS
    // THE POINT OF THIS FILE'S OTHER HALF: `ldr q0,[x1,#3]` is no longer a
    // refusal at all — it is 0x3CC03020, the unscaled encoding, on DSS and on
    // both references ([[D-ASM-ARM64-LDR-TO-LDUR-CONVENIENCE-ALIAS-REFUSED]]).
    // 300 is neither a multiple of 16 nor inside imm9, so it is outside BOTH
    // reaches and refused by both references too — which is what this pin needs
    // and what `#3` had stopped being.
    auto const encodeSide = runBare("ldr q0, [x1, #300]\n");
    ASSERT_TRUE(encodeSide->parsed) << messages(*encodeSide);
    EXPECT_TRUE(encodeSide->bytes.empty())
        << "an aborted function encode must leave an EMPTY stream: "
        << hex(encodeSide->bytes);
    expectContributedNothing(*encodeSide, "ldr q0, [x1, #300]");

    // LOWER-tier: the spelling names nothing this target declares, so the
    // instruction is never built and the appended `ret` is all there is.
    auto const lowerSide = runBare("ldr z0, [x1, #16]\n");
    EXPECT_EQ(lowerSide->bytes.size(), 4u)
        << "a lowering refusal must leave exactly the harness `ret`: "
        << hex(lowerSide->bytes);
    expectContributedNothing(*lowerSide, "ldr z0, [x1, #16]");
}

// ═══════════════════════════════════════════════════════════════════════════
// (8) THE BARE VECTOR NAME
//     [[D-ASM-ARM64-BARE-V-REGISTER-ACCEPTED-IN-A-SCALAR-MEMORY-OPERAND]]
// ═══════════════════════════════════════════════════════════════════════════
//
// ★★★ THE MEASUREMENT THAT DECIDED THE ROW'S BAND, RECORDED BECAUSE IT WAS THE
// FIRST THING THE ROW ASKED FOR AND NOBODY HAD DONE IT. ✔MEASURED 2026-09-03
// through the REAL CLI at `arm64:elf64-aarch64-linux-exec`, read back with
// `aarch64-linux-gnu-objdump`: `ldr v0,[x9,#16]` compiled rc=0 and emitted
// 0x3DC00520, which decodes as `ldr q0,[x9,#16]` — BYTE-IDENTICAL to what gas
// 2.42 and clang 18.1.3 emit for the `q0` spelling, and both of them REFUSE the
// `v0` one. So the bare `v` was an UNDECLARED ALIAS producing correct bytes for
// a program neither reference would assemble, and NOT a silent wrong answer;
// had the word decoded as anything else the row would have re-banded upward
// instead of closing as an acceptance defect.
//
// ★★ AND THE ROW'S SCOPE CLAIM WAS WIDER THAN THE TRUTH. It said the
// over-acceptance covered *every width and every mnemonic*. ✔MEASURED, it did
// not: `mov v0,v1`, `fmov v0,v1` and `fadd v0,v1,v2` were ALREADY refused
// (`A_AsmTextUnsupported` — no 128-bit variant of those opcodes exists), so the
// only spellings that accepted a bare `v` were the four with a 128-bit memory
// form. Bare `v` was accepted exactly where `q` is and produced exactly `q`'s
// bytes: a complete alias over a narrow surface, not a partial one over a wide.

// ✔MEASURED on BOTH references: every one of these is refused — gas with
// `Error: unexpected register type at operand 1`, clang with
// `invalid operand for instruction`.
TEST(AsmArm64MemoryRows, ABareVectorNameIsRefusedAtEveryMnemonicAndOrdinal) {
    for (auto const* text : {"ldr v0, [x9, #16]\n",
                             "str v0, [x9, #16]\n",
                             "ldur v0, [x9, #1]\n",
                             "stur v0, [x9, #1]\n",
                             "ldr v0, [x9]\n",
                             "ldr v31, [x9]\n",
                             "ldr q0, [v0]\n"}) {   // and as a memory BASE
        auto const r = runBare(text);
        ASSERT_TRUE(r->parsed) << text << "\n" << messages(*r);
        expectContributedNothing(*r, text);
    }
}

// ★★ THE REFUSAL MUST **TEACH**, not merely decline — the row asked for a
// diagnostic that names the spelling and says what to write instead, so that is
// asserted rather than left to inspection.
TEST(AsmArm64MemoryRows, TheBareVectorRefusalNamesTheSpellingAndTheAlternative) {
    auto const r = runBare("ldr v0, [x9, #16]\n");
    ASSERT_TRUE(r->parsed) << messages(*r);
    auto const msg = messages(*r);
    EXPECT_NE(msg.find("'v0'"), std::string::npos)
        << "the refusal must quote what was WRITTEN: " << msg;
    EXPECT_NE(msg.find("'q0'"), std::string::npos)
        << "the refusal must name the scalar spelling of the SAME register, "
           "which is the row's whole point — the register is fine, the name "
           "is not: " << msg;
    EXPECT_NE(msg.find("lane arrangement"), std::string::npos)
        << "the refusal must say what is required instead: " << msg;
    EXPECT_NE(msg.find("v0.16b"), std::string::npos)
        << "the refusal must offer the dialect's OWN declared arrangements, "
           "composed onto the spelling that was written: " << msg;
    // The ordinal must travel: a message hard-coded for `v0` would be worse
    // than none, because it would send a `v31` user to the wrong register.
    auto const r31 = runBare("ldr v31, [x9]\n");
    EXPECT_NE(messages(*r31).find("'q31'"), std::string::npos)
        << "the alternative must be THIS row's alias, not a fixed string: "
        << messages(*r31);
}

// ★★★ THE CONTROLS, PINNED BY NAME. Each is a spelling the fix must NOT touch,
// and each fails in a different direction if the refusal were written against
// the REGISTER instead of against the NAME: `q0` is the same ordinal reached
// through an alias, `d0`..`b0` are the same ordinal reached through `subOf`,
// and `v0.16b` is the same NAME with an arrangement written on it.
TEST(AsmArm64MemoryRows, TheAliasSubRegistersAndArrangementViewsAllStayGreen) {
    constexpr Case kCases[] = {
        {"ldr q0, [x9, #16]\n",  kLdrQ_16_at_x9, "the ALIAS — lane `lq` "
                                                 "shipped it hours before "
                                                 "this row and breaking it "
                                                 "would be a regression"},
        {"ldr q31, [x9]\n",      0x3DC0013Fu, "the alias at the far ordinal"},
        {"str q0, [x9, #16]\n",  0x3D800520u, "the alias, store direction"},
        {"ldr d0, [x9, #8]\n",   0xFD400520u, "a `subOf` view"},
        {"ldr s0, [x9, #4]\n",   0xBD400520u, "a `subOf` view"},
        {"ldr h0, [x9, #2]\n",   0x7D400520u, "a `subOf` view"},
        {"ldr b0, [x9, #1]\n",   0x3D400520u, "a `subOf` view"},
        {"mov v0.16b, v1.16b\n", 0x4EA11C20u, "the NAME with an arrangement"},
        {"mov v0.8b, v1.8b\n",   0x0EA11C20u, "a narrower arrangement"},
        {"cnt v0.8b, v1.8b\n",   0x0E205820u, "an arrangement on a second "
                                              "mnemonic"},
    };
    expectWords(kCases);
}

// ★★★ RED ON DISABLE — REMOVE direction, over the SHIPPED target document.
// Strip `nameRequiresLaneArrangement` from the 32 `v` rows and the bare
// spelling comes straight back, emitting the q word. That is the pre-fix
// behaviour reproduced exactly, which is what makes the pins above non-vacuous.
TEST(AsmArm64MemoryRows, RemovingTheLaneRequirementRestoresTheBareVSpelling) {
    auto const mutant =
        mutateShippedTargetSchemaDoc(kTarget, removeTheLaneRequirement);
    ASSERT_TRUE(mutant.has_value())
        << "the mutant target did not load: " << whyNotLoaded(mutant.error());

    auto const r = runOn(loadDialect(), *mutant, "ldr v0, [x9, #16]\n");
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_TRUE(r->ok) << "with the key removed the bare `v` must be accepted "
                          "again — if it is still refused, the refusal is NOT "
                          "reading this key and the pins above prove nothing: "
                       << messages(*r);
    EXPECT_EQ(firstWord(r->bytes), kLdrQ_16_at_x9)
        << "and it must emit the word the P55 base measurement recorded — the "
           "`q` form, byte-identical to the alias spelling: " << hex(r->bytes);

    // ★★ THE TWO CONTROLS, PRINTED BY NAME AND CHECKED **UNDER THE MUTANT**
    // rather than only beside it. They are the two spellings the fix must never
    // have touched, and they fail in opposite directions: `q0` is the same
    // ordinal reached through an ALIAS (a refusal written against the REGISTER
    // instead of the NAME would take it out), and `v1.8b` is the same NAME
    // reached WITH an arrangement (a refusal gated on the width instead of on
    // the written suffix would take that one out). Checking them here proves
    // the mutation moved ONLY the bare spelling — a mutant that broke the whole
    // family would otherwise pass this pin.
    auto const control = runOn(loadDialect(), *mutant, "ldr q0, [x9, #16]\n");
    ASSERT_TRUE(control->parsed) << messages(*control);
    EXPECT_TRUE(control->ok) << "CONTROL `ldr q0, [x9, #16]`: "
                             << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kLdrQ_16_at_x9)
        << "CONTROL `ldr q0, [x9, #16]` under the mutant: "
        << hex(control->bytes);

    auto const arranged = runOn(loadDialect(), *mutant, "mov v0.8b, v1.8b\n");
    ASSERT_TRUE(arranged->parsed) << messages(*arranged);
    EXPECT_TRUE(arranged->ok) << "CONTROL `mov v0.8b, v1.8b`: "
                              << messages(*arranged);
    EXPECT_EQ(firstWord(arranged->bytes), 0x0EA11C20u)
        << "CONTROL `mov v0.8b, v1.8b` under the mutant — the arrangement path "
           "reaches the SAME rows this mutation edits and must be untouched by "
           "it in both directions: " << hex(arranged->bytes);
}

// ★★ THE LOAD-TIME COHERENCE RULE: a row may unspell its own name only if it
// leaves another spelling behind. Without it the key could silently produce a
// register with NO bare spelling at all — present in the table, refused on
// every line that names it.
TEST(AsmArm64MemoryRows, ARowThatUnspellsItsOwnNameWithNoAliasIsRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, removeAliasesButKeepTheLaneRequirement);
    ASSERT_FALSE(mutant.has_value())
        << "a `v` row keeping `nameRequiresLaneArrangement` with its `aliases` "
           "deleted leaves the register with no bare spelling at all, and the "
           "loader accepted it";
    std::string why;
    for (auto const& e : mutant.error()) why += e.path + ": " + e.message + "\n";
    EXPECT_NE(why.find("nameRequiresLaneArrangement"), std::string::npos)
        << "the refusal must name the key that caused it: " << why;
    EXPECT_NE(why.find("aliases"), std::string::npos)
        << "and the key whose absence made it incoherent: " << why;
}

// ═══════════════════════════════════════════════════════════════════════════
// (9) THE DIVISIBILITY GUARD AND THE RANK — the two keys that together carry
//     the `ldr`→`ldur` fallback, each pinned by its OWN mutant.
//     [[D-ASM-ARM64-LDR-TO-LDUR-CONVENIENCE-ALIAS-REFUSED]]
// ═══════════════════════════════════════════════════════════════════════════

// ★★★ RED ON DISABLE (A) — strip `immMultipleOf` from the scaled variants and
// the scaled candidate starts matching offsets its own encoder cannot carry.
// The rank then hands the operation to that form and the unscaled fallback is
// never reached, so the P55-base refusal comes straight back. This is what
// makes "the two keys are one mechanism" a MEASURED claim rather than a
// comment: the rank alone is not the fix.
TEST(AsmArm64MemoryRows, RemovingTheDivisibilityGuardsRestoresTheRefusal) {
    auto const mutant =
        mutateShippedTargetSchemaDoc(kTarget, removeTheDivisibilityGuards);
    ASSERT_TRUE(mutant.has_value())
        << "the mutant target did not load: " << whyNotLoaded(mutant.error());

    for (auto const* text : {"ldr h0, [x1, #1]\n",
                             "ldr s0, [x1, #1]\n",
                             "ldr d0, [x1, #255]\n",
                             "ldr q0, [x1, #1]\n"}) {
        auto const r = runOn(loadDialect(), *mutant, text);
        ASSERT_TRUE(r->parsed) << text << "\n" << messages(*r);
        expectContributedNothing(*r, text);
    }

    // CONTROL, printed by name: an offset the SCALED form really can carry is
    // unaffected — the mutation removed a restriction, so nothing that used to
    // encode may stop encoding.
    auto const control = runOn(loadDialect(), *mutant, "ldr h0, [x1, #2]\n");
    ASSERT_TRUE(control->parsed) << messages(*control);
    EXPECT_TRUE(control->ok) << "CONTROL `ldr h0, [x1, #2]`: "
                             << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kLdrH)
        << "CONTROL `ldr h0, [x1, #2]` under the mutant: "
        << hex(control->bytes);

    // CONTROL 2: the NEGATIVE half is carried by the SIGN axis, not by the
    // divisibility one, so it must survive this mutation. A mutant that took
    // out both halves would let one pin cover for the other.
    auto const neg = runOn(loadDialect(), *mutant, "ldr d0, [x1, #-8]\n");
    ASSERT_TRUE(neg->parsed) << messages(*neg);
    EXPECT_TRUE(neg->ok) << "CONTROL `ldr d0, [x1, #-8]`: " << messages(*neg);
    EXPECT_EQ(firstWord(neg->bytes), kLdurD)
        << "CONTROL `ldr d0, [x1, #-8]` under the mutant: " << hex(neg->bytes);
}

// ★★★ RED ON DISABLE (B) — strip `opcodesAreRankedEncodings` from the DIALECT
// and the same lines are refused again, but for the OTHER reason: two
// candidates fit and the row can no longer say which. Two mutants, two
// mechanisms, two DIFFERENT refusals — which is what proves neither key is
// carrying the other's weight.
TEST(AsmArm64MemoryRows, RemovingTheRankMakesTheCandidateListAmbiguous) {
    std::string text = dialectText();
    // ⚠ THE COMMA IS ON THE LEFT, because the key is the LAST member of its
    // row object. Written with a trailing comma this mutant matched nothing and
    // the `ASSERT_LT` below caught it as vacuous on the first run — which is
    // the fixture guard doing exactly its job, and the reason the assert is
    // here rather than trusted away.
    std::string const key = ", \"opcodesAreRankedEncodings\": true";
    auto const before = text.size();
    for (auto at = text.find(key); at != std::string::npos;
         at = text.find(key, at)) {
        text.erase(at, key.size());
    }
    ASSERT_LT(text.size(), before)
        << "the dialect document declares no `opcodesAreRankedEncodings` in "
           "the spelling this mutant strips — the pin would be vacuous";

    auto g = GrammarSchema::loadFromText(text, std::string{kDialect});
    ASSERT_TRUE(g.has_value())
        << "the mutant dialect did not load — the mutation was meant to remove "
           "a key, not to break the document";

    // `ldr h0,[x1,#2]` fits BOTH the scaled and the unscaled candidate, so
    // without the rank the row is ambiguous and the line is refused.
    auto const r = runOn(*g, shippedTarget(), "ldr h0, [x1, #2]\n");
    ASSERT_TRUE(r->parsed) << messages(*r);
    expectContributedNothing(*r, "ldr h0, [x1, #2]");
    EXPECT_NE(messages(*r).find("could be target opcode"), std::string::npos)
        << "without the rank the refusal must be the AMBIGUITY one — a "
           "different refusal means the candidates never both matched and this "
           "mutant is not testing the rank: " << messages(*r);

    // CONTROL, printed by name: `ldur` names ONE encoding per register class,
    // so it was never ambiguous and must stay green under this mutant.
    auto const control = runOn(*g, shippedTarget(), "ldur h0, [x1, #-2]\n");
    ASSERT_TRUE(control->parsed) << messages(*control);
    EXPECT_TRUE(control->ok) << "CONTROL `ldur h0, [x1, #-2]`: "
                             << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kLdurH)
        << "CONTROL `ldur h0, [x1, #-2]` under the mutant: "
        << hex(control->bytes);
}

// ★★ THE MODULUS VOCABULARY IS FENCED AT LOAD: 1 states nothing (every integer
// is a multiple of 1) and would read to a maintainer as "this field is
// unscaled", which is its opposite.
TEST(AsmArm64MemoryRows, ADivisibilityGuardOfOneIsRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            for (auto& op : doc.at("opcodes")) {
                if (!op.is_object()) continue;
                if (op.value("mnemonic", std::string{}) != "fldr_u") continue;
                for (auto& v : op.at("encoding").at("variants")) {
                    if (v.at("guard").value("width", 0) == 16) {
                        v.at("guard")["immMultipleOf"] = 1;
                    }
                }
            }
        });
    ASSERT_FALSE(mutant.has_value())
        << "a modulus of 1 restricts nothing and the loader accepted it";
    std::string why;
    for (auto const& e : mutant.error()) why += e.path + ": " + e.message + "\n";
    EXPECT_NE(why.find("immMultipleOf"), std::string::npos)
        << "the refusal must name the key: " << why;
}
