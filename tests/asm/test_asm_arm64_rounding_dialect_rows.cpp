// THE REST OF THE SCALAR FLOATING-POINT VOCABULARY OF THE aarch64 gas DIALECT
// — the explicit ROUNDING MODES, the 1-, 2- and 3-source arithmetic, the
// signalling compare, and the SIMD register move
// (D-ASM-ARM64-GAS-SPELLS-NO-ROUNDING-MODE-OR-VECTOR-MOVE, cycle P54, lane av).
//
// ★★★ WHY THESE PINS EXIST, AND IT IS ONE SENTENCE: a rounding-mode mnemonic
// bound to the wrong opcode produces a WORKING PROGRAM WITH THE WRONG NUMBERS.
// `fcvtas`, `fcvtms`, `fcvtns` and `fcvtps` differ from the already-declared
// `fcvtzs` in NOTHING but the `rmode` field, so a mis-binding compiles, links,
// runs, returns a plausible integer, and is invisible in every build log. Lane
// `el` named that hazard when it measured these bytes and left them undeclared;
// this file is the declaration plus the instrument that keeps it honest.
//
// ★★★ THE CENSUS THAT CHOSE THE SHIP LIST, because "what is missing" has no
// short answer on this ISA. ✔MEASURED at the P54 base, 48 inline-asm probes
// through the real CLI: 46 were spellings GNU as 2.42 AND clang 18.1.3 BOTH
// assemble and DSS REFUSED. AArch64 is large and the remainder is bounded only
// by the ISA — the integer group (`madd`/`smulh`/`bic`/`orn`/`eon`/`rev`/
// `cls`/`adds`/`csel`/`extr`/`ubfx`), the barriers, the LSE atomics, the lane
// moves and the whole SIMD arithmetic table are all still refused. What shipped
// is a family that CLOSES: every AArch64 scalar floating-point instruction
// whose operands are registers only, at the S and D forms. `EveryScalarFpRow`
// below is that boundary asked as a question rather than asserted.
//
// ★★ EVERY EXPECTED WORD IS ✔MEASURED AGAINST GNU as 2.42 **AND** clang 18.1.3,
// PROBED SEPARATELY — 124 probes across the census and the ship list, ZERO
// disagreements — and each was read back out of an assembled object with
// `aarch64-linux-gnu-objdump`. None is derived from a neighbour and none is
// read out of a manual. Each `fixedWord` in the target was then derived by
// CLEARING the slot windows from the measured word and re-inserting the
// register numbers to reproduce it exactly.
//
// ⚠ THE PINS READ THE WHOLE 32-BIT WORD. On this ISA the single/double pair is
// `ftype` [23:22] of ONE word and the rounding mode is `rmode` [16:15] of the
// same word; a variant carrying the wrong one still assembles and still runs.
// A field-by-field check would let a neighbour drift.
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

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
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

// ★ THE PHYSICAL REGISTERS ARE THE `v` ROOTS, NOT THE `d`/`s` VIEWS, AND THAT
// MATCHES THE PIPELINE: the allocator hands the encoder a `v` ordinal and the
// WIDTH decides which view the bytes mean.
// ⚠ THE BINDING'S SPELLING IS THE PLAIN PLACEHOLDER (`%0`), NEVER THE LETTERED
// ONE — the modifier letter is a property of the TEMPLATE TEXT.
[[nodiscard]] std::unique_ptr<Run>
runFpN(std::string_view templateText, std::uint32_t widthBits,
       std::size_t operandCount,
       std::shared_ptr<GrammarSchema> dialect = nullptr,
       std::shared_ptr<TargetSchema>  target  = nullptr) {
    static constexpr char const* kRegs[] = {"v0", "v1", "v2", "v3"};
    std::vector<Bind> binds;
    for (std::size_t i = 0; i < operandCount; ++i) {
        binds.push_back(Bind{std::format("%{}", i), kRegs[i],
                             LirRegClass::FPR, widthBits});
    }
    return runOn(dialect ? std::move(dialect) : loadDialect(),
                 target ? std::move(target) : shippedTarget(),
                 templateText, binds);
}

// A conversion: the destination and the source have their own class AND width.
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
// ✔GNU as 2.42 AND clang 18.1.3, probed separately, agreeing on every one.
// `%0`->v0/x0, `%1`->v1/x1, `%2`->v2, `%3`->v3, destination-first order, so
// these are exactly the words both assemblers produce for the spelling in the
// comment.

// The `ret` the harness appends after every template. Seeing it as the FIRST
// word is exactly "the template emitted nothing" — asserting it is the
// positive form of that claim, since the byte stream is never empty.
constexpr std::uint32_t kRet = 0xD65F03C0u;

// (1) FP → integer, EXPLICIT rounding mode.  W and X destinations × S and D
// sources, for all eight spellings.
struct ConvWords {
    char const*   spelling;
    std::uint32_t ws;   // W dest, S source
    std::uint32_t wd;   // W dest, D source
    std::uint32_t xs;   // X dest, S source
    std::uint32_t xd;   // X dest, D source
};
constexpr ConvWords kConv[] = {
    {"fcvtns", 0x1E200020u, 0x1E600020u, 0x9E200020u, 0x9E600020u},
    {"fcvtnu", 0x1E210020u, 0x1E610020u, 0x9E210020u, 0x9E610020u},
    {"fcvtas", 0x1E240020u, 0x1E640020u, 0x9E240020u, 0x9E640020u},
    {"fcvtau", 0x1E250020u, 0x1E650020u, 0x9E250020u, 0x9E650020u},
    {"fcvtms", 0x1E300020u, 0x1E700020u, 0x9E300020u, 0x9E700020u},
    {"fcvtmu", 0x1E310020u, 0x1E710020u, 0x9E310020u, 0x9E710020u},
    {"fcvtps", 0x1E280020u, 0x1E680020u, 0x9E280020u, 0x9E680020u},
    {"fcvtpu", 0x1E290020u, 0x1E690020u, 0x9E290020u, 0x9E690020u},
};
// The already-declared toward-zero pair, used as the CONTROL that the new
// spellings are not silently electing it.
constexpr std::uint32_t kFcvtzsWS = 0x1E380020u;
constexpr std::uint32_t kFcvtzsXD = 0x9E780020u;

// (2) Round-to-integral, float → float.
struct UnaryWords {
    char const*   spelling;
    std::uint32_t d;
    std::uint32_t s;
};
constexpr UnaryWords kRint[] = {
    {"frintn", 0x1E644020u, 0x1E244020u},
    {"frinta", 0x1E664020u, 0x1E264020u},
    {"frintm", 0x1E654020u, 0x1E254020u},
    {"frintp", 0x1E64C020u, 0x1E24C020u},
    {"frintz", 0x1E65C020u, 0x1E25C020u},
    {"frinti", 0x1E67C020u, 0x1E27C020u},
    {"frintx", 0x1E674020u, 0x1E274020u},
};

// (3) 1-source arithmetic.
constexpr UnaryWords kUnary[] = {
    {"fabs",  0x1E60C020u, 0x1E20C020u},
    {"fsqrt", 0x1E61C020u, 0x1E21C020u},
};

// (4) 2-source arithmetic.
constexpr UnaryWords kBinary[] = {
    {"fmax",   0x1E624820u, 0x1E224820u},
    {"fmin",   0x1E625820u, 0x1E225820u},
    {"fmaxnm", 0x1E626820u, 0x1E226820u},
    {"fminnm", 0x1E627820u, 0x1E227820u},
    {"fnmul",  0x1E628820u, 0x1E228820u},
};

// (5) 3-source fused multiply-add.
constexpr UnaryWords kFma[] = {
    {"fmadd",  0x1F420C20u, 0x1F020C20u},
    {"fmsub",  0x1F428C20u, 0x1F028C20u},
    {"fnmadd", 0x1F620C20u, 0x1F220C20u},
    {"fnmsub", 0x1F628C20u, 0x1F228C20u},
};

// (6) The signalling compare, and the quiet CONTROL one bit away from it.
constexpr std::uint32_t kFcmpeD = 0x1E612010u;
constexpr std::uint32_t kFcmpeS = 0x1E212010u;
constexpr std::uint32_t kFcmpD  = 0x1E612000u;
constexpr std::uint32_t kFcmpS  = 0x1E212000u;

// (7) The SIMD register move — the ORR alias.
constexpr std::uint32_t kMov16B = 0x4EA11C20u;
constexpr std::uint32_t kMov8B  = 0x0EA11C20u;

}  // namespace

// ══ (1) THE ROUNDING MODES — THE SUBJECT ═══════════════════════════════════

// ★★★ THE PIN THIS WHOLE FILE IS FOR. Thirty-two words, eight spellings, four
// width pairs each. A `fcvtms` that emitted `fcvtzs`'s word would round toward
// zero where the source says toward −∞ and every other test would still pass.
TEST(AsmArm64RoundingRows, EveryRoundingModeEncodesItsOwnWordAtEveryWidthPair) {
    for (auto const& c : kConv) {
        struct Case {
            char const*   text;
            LirRegClass   dstCls;
            std::uint32_t dstW;
            char const*   dstReg;
            std::uint32_t srcW;
            std::uint32_t want;
        };
        std::string const ws = std::format("{} %w0, %s1\n", c.spelling);
        std::string const wd = std::format("{} %w0, %d1\n", c.spelling);
        std::string const xs = std::format("{} %x0, %s1\n", c.spelling);
        std::string const xd = std::format("{} %x0, %d1\n", c.spelling);
        Case const cases[] = {
            {ws.c_str(), LirRegClass::GPR, 32, "x0", 32, c.ws},
            {wd.c_str(), LirRegClass::GPR, 32, "x0", 64, c.wd},
            {xs.c_str(), LirRegClass::GPR, 64, "x0", 32, c.xs},
            {xd.c_str(), LirRegClass::GPR, 64, "x0", 64, c.xd},
        };
        for (auto const& k : cases) {
            auto const r = run2(k.text, k.dstCls, k.dstW, k.dstReg,
                                LirRegClass::FPR, k.srcW, "v1");
            ASSERT_TRUE(r->parsed) << k.text << ": " << messages(*r);
            EXPECT_TRUE(r->ok) << k.text << ": " << messages(*r);
            EXPECT_EQ(firstWord(r->bytes), k.want)
                << k.text << " emitted " << hex(r->bytes);
        }
    }
}

// ★★★ AND THE SAME CLAIM STATED NEGATIVELY, because "each emits its own word"
// is only half of "none of them emits the TOWARD-ZERO word". This is the
// hazard `el` named, asked directly.
TEST(AsmArm64RoundingRows, NoRoundingModeEverEmitsTheTowardZeroWord) {
    for (auto const& c : kConv) {
        auto const ws = std::format("{} %w0, %s1\n", c.spelling);
        auto const rw = run2(ws, LirRegClass::GPR, 32, "x0",
                             LirRegClass::FPR, 32, "v1");
        ASSERT_TRUE(rw->parsed) << ws << messages(*rw);
        EXPECT_NE(firstWord(rw->bytes), kFcvtzsWS)
            << c.spelling << " emitted FCVTZS's word — the rounding mode was "
                             "silently changed: " << hex(rw->bytes);

        auto const xd = std::format("{} %x0, %d1\n", c.spelling);
        auto const rx = run2(xd, LirRegClass::GPR, 64, "x0",
                             LirRegClass::FPR, 64, "v1");
        ASSERT_TRUE(rx->parsed) << xd << messages(*rx);
        EXPECT_NE(firstWord(rx->bytes), kFcvtzsXD)
            << c.spelling << " emitted FCVTZS's word: " << hex(rx->bytes);
    }
}

// ★★ THE CONTROL: `fcvtzs` itself is untouched. Without it a bug that broke
// every conversion would leave the two tests above green in the same way a
// bug that fixed nothing would.
TEST(AsmArm64RoundingRows, TheTowardZeroSpellingIsUnchanged) {
    auto const rw = run2("fcvtzs %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                         LirRegClass::FPR, 32, "v1");
    ASSERT_TRUE(rw->parsed) << messages(*rw);
    EXPECT_TRUE(rw->ok) << messages(*rw);
    EXPECT_EQ(firstWord(rw->bytes), kFcvtzsWS) << hex(rw->bytes);

    auto const rx = run2("fcvtzs %x0, %d1\n", LirRegClass::GPR, 64, "x0",
                         LirRegClass::FPR, 64, "v1");
    ASSERT_TRUE(rx->parsed) << messages(*rx);
    EXPECT_EQ(firstWord(rx->bytes), kFcvtzsXD) << hex(rx->bytes);
}

// ══ (2) ROUND-TO-INTEGRAL ══════════════════════════════════════════════════

TEST(AsmArm64RoundingRows, EveryRintModeEncodesBothFtypes) {
    for (auto const& c : kRint) {
        auto const d = std::format("{} %d0, %d1\n", c.spelling);
        auto const rd = runFpN(d, 64, 2);
        ASSERT_TRUE(rd->parsed) << d << messages(*rd);
        EXPECT_TRUE(rd->ok) << d << messages(*rd);
        EXPECT_EQ(firstWord(rd->bytes), c.d) << d << hex(rd->bytes);

        auto const s = std::format("{} %s0, %s1\n", c.spelling);
        auto const rs = runFpN(s, 32, 2);
        ASSERT_TRUE(rs->parsed) << s << messages(*rs);
        EXPECT_TRUE(rs->ok) << s << messages(*rs);
        EXPECT_EQ(firstWord(rs->bytes), c.s) << s << hex(rs->bytes);
    }
}

// ★★ THE SEVEN MUST BE SEVEN DIFFERENT WORDS AT EACH WIDTH. A table that
// accidentally repeated one base would still satisfy the equality pins above
// for the row it copied FROM.
TEST(AsmArm64RoundingRows, TheSevenRintModesAreSevenDistinctWords) {
    std::set<std::uint32_t> dWords;
    std::set<std::uint32_t> sWords;
    for (auto const& c : kRint) {
        auto const rd = runFpN(std::format("{} %d0, %d1\n", c.spelling), 64, 2);
        ASSERT_TRUE(rd->parsed && rd->ok) << c.spelling << messages(*rd);
        EXPECT_TRUE(dWords.insert(firstWord(rd->bytes)).second)
            << c.spelling << " emitted a D-form word another mode already "
                             "emitted: " << hex(rd->bytes);
        auto const rs = runFpN(std::format("{} %s0, %s1\n", c.spelling), 32, 2);
        ASSERT_TRUE(rs->parsed && rs->ok) << c.spelling << messages(*rs);
        EXPECT_TRUE(sWords.insert(firstWord(rs->bytes)).second)
            << c.spelling << " emitted an S-form word another mode already "
                             "emitted: " << hex(rs->bytes);
    }
    EXPECT_EQ(dWords.size(), std::size(kRint));
    EXPECT_EQ(sWords.size(), std::size(kRint));
}

// ══ (3)(4)(5) THE ARITHMETIC FAMILIES ══════════════════════════════════════

TEST(AsmArm64RoundingRows, OneSourceArithmeticEncodesBothFtypes) {
    for (auto const& c : kUnary) {
        auto const rd = runFpN(std::format("{} %d0, %d1\n", c.spelling), 64, 2);
        ASSERT_TRUE(rd->parsed) << c.spelling << messages(*rd);
        EXPECT_TRUE(rd->ok) << c.spelling << messages(*rd);
        EXPECT_EQ(firstWord(rd->bytes), c.d) << c.spelling << hex(rd->bytes);

        auto const rs = runFpN(std::format("{} %s0, %s1\n", c.spelling), 32, 2);
        ASSERT_TRUE(rs->parsed) << c.spelling << messages(*rs);
        EXPECT_TRUE(rs->ok) << c.spelling << messages(*rs);
        EXPECT_EQ(firstWord(rs->bytes), c.s) << c.spelling << hex(rs->bytes);
    }
}

TEST(AsmArm64RoundingRows, TwoSourceArithmeticEncodesBothFtypes) {
    for (auto const& c : kBinary) {
        auto const rd =
            runFpN(std::format("{} %d0, %d1, %d2\n", c.spelling), 64, 3);
        ASSERT_TRUE(rd->parsed) << c.spelling << messages(*rd);
        EXPECT_TRUE(rd->ok) << c.spelling << messages(*rd);
        EXPECT_EQ(firstWord(rd->bytes), c.d) << c.spelling << hex(rd->bytes);

        auto const rs =
            runFpN(std::format("{} %s0, %s1, %s2\n", c.spelling), 32, 3);
        ASSERT_TRUE(rs->parsed) << c.spelling << messages(*rs);
        EXPECT_TRUE(rs->ok) << c.spelling << messages(*rs);
        EXPECT_EQ(firstWord(rs->bytes), c.s) << c.spelling << hex(rs->bytes);
    }
}

// ★★ `fmax` AND `fmaxnm` MUST NOT BE THE SAME BYTES. They agree on every
// ordinary input and disagree only on NaN, so a fold between them is invisible
// to any test that does not feed one — which is exactly why the check is on the
// WORDS rather than on a computed value.
TEST(AsmArm64RoundingRows, MaxAndMaxnmAreDifferentInstructions) {
    auto const mx = runFpN("fmax %d0, %d1, %d2\n", 64, 3);
    auto const nm = runFpN("fmaxnm %d0, %d1, %d2\n", 64, 3);
    ASSERT_TRUE(mx->parsed && mx->ok) << messages(*mx);
    ASSERT_TRUE(nm->parsed && nm->ok) << messages(*nm);
    EXPECT_NE(firstWord(mx->bytes), firstWord(nm->bytes))
        << "fmax and fmaxnm assembled to the same word — the NaN-propagating "
           "and IEEE maxNum forms have been folded: " << hex(mx->bytes);

    auto const mn  = runFpN("fmin %d0, %d1, %d2\n", 64, 3);
    auto const nnm = runFpN("fminnm %d0, %d1, %d2\n", 64, 3);
    ASSERT_TRUE(mn->parsed && mn->ok) << messages(*mn);
    ASSERT_TRUE(nnm->parsed && nnm->ok) << messages(*nnm);
    EXPECT_NE(firstWord(mn->bytes), firstWord(nnm->bytes)) << hex(mn->bytes);
}

// ★★ THE FIRST FOUR-TOKEN INSTRUCTIONS THIS DIALECT SPELLS — one destination
// and THREE sources, the third riding the `Ra` slot the integer `msub` already
// used. Nothing in the shared grammar changed to accept them.
TEST(AsmArm64RoundingRows, FusedMultiplyAddWiresThreeSources) {
    for (auto const& c : kFma) {
        auto const rd =
            runFpN(std::format("{} %d0, %d1, %d2, %d3\n", c.spelling), 64, 4);
        ASSERT_TRUE(rd->parsed) << c.spelling << messages(*rd);
        EXPECT_TRUE(rd->ok) << c.spelling << messages(*rd);
        EXPECT_EQ(firstWord(rd->bytes), c.d) << c.spelling << hex(rd->bytes);

        auto const rs =
            runFpN(std::format("{} %s0, %s1, %s2, %s3\n", c.spelling), 32, 4);
        ASSERT_TRUE(rs->parsed) << c.spelling << messages(*rs);
        EXPECT_TRUE(rs->ok) << c.spelling << messages(*rs);
        EXPECT_EQ(firstWord(rs->bytes), c.s) << c.spelling << hex(rs->bytes);
    }
}

// ★★★ THE ADDEND IS THE **FOURTH** OPERAND, AND ITS FIELD IS `Ra` [14:10].
// Reading the operand order wrong would put the addend in `Rm` and the
// multiplier in `Ra` — an instruction that still assembles and computes
// something else entirely. Distinct registers make the placement visible.
TEST(AsmArm64RoundingRows, TheFmaAddendLandsInRaAndNotInRm) {
    auto const r = runFpN("fmadd %d0, %d1, %d2, %d3\n", 64, 4);
    ASSERT_TRUE(r->parsed && r->ok) << messages(*r);
    auto const w = firstWord(r->bytes);
    EXPECT_EQ((w >> 16) & 0x1Fu, 2u)
        << "Rm should carry %2 (v2), the second multiplicand: " << hex(r->bytes);
    EXPECT_EQ((w >> 10) & 0x1Fu, 3u)
        << "Ra should carry %3 (v3), the addend: " << hex(r->bytes);
    EXPECT_EQ((w >> 5) & 0x1Fu, 1u)
        << "Rn should carry %1 (v1): " << hex(r->bytes);
    EXPECT_EQ(w & 0x1Fu, 0u)
        << "Rd should carry %0 (v0): " << hex(r->bytes);
}

// ══ (6) THE SIGNALLING COMPARE ═════════════════════════════════════════════

// ★★ BOTH OPERANDS ARE INPUTS AND Rd [4:0] IS **NOT A REGISTER** — it is
// `opcode2`, whose bit 4 is the E bit that makes the form signalling. A
// `resultSlot` on this opcode would overwrite it and emit the quiet compare.
TEST(AsmArm64RoundingRows, SignallingCompareKeepsItsEBit) {
    auto const rd = runFpN("fcmpe %d0, %d1\n", 64, 2);
    ASSERT_TRUE(rd->parsed) << messages(*rd);
    EXPECT_TRUE(rd->ok) << messages(*rd);
    EXPECT_EQ(firstWord(rd->bytes), kFcmpeD) << hex(rd->bytes);
    EXPECT_NE(firstWord(rd->bytes), kFcmpD)
        << "fcmpe emitted the QUIET fcmp word — the E bit was dropped: "
        << hex(rd->bytes);

    auto const rs = runFpN("fcmpe %s0, %s1\n", 32, 2);
    ASSERT_TRUE(rs->parsed) << messages(*rs);
    EXPECT_TRUE(rs->ok) << messages(*rs);
    EXPECT_EQ(firstWord(rs->bytes), kFcmpeS) << hex(rs->bytes);
    EXPECT_NE(firstWord(rs->bytes), kFcmpS) << hex(rs->bytes);
}

// The CONTROL: the quiet `fcmp` still emits the quiet word.
TEST(AsmArm64RoundingRows, TheQuietCompareIsUnchanged) {
    auto const rd = runFpN("fcmp %d0, %d1\n", 64, 2);
    ASSERT_TRUE(rd->parsed && rd->ok) << messages(*rd);
    EXPECT_EQ(firstWord(rd->bytes), kFcmpD) << hex(rd->bytes);
    auto const rs = runFpN("fcmp %s0, %s1\n", 32, 2);
    ASSERT_TRUE(rs->parsed && rs->ok) << messages(*rs);
    EXPECT_EQ(firstWord(rs->bytes), kFcmpS) << hex(rs->bytes);
}

// ══ (7) THE SIMD REGISTER MOVE ═════════════════════════════════════════════

// ★★★ ONE WRITTEN SOURCE, TWO ENCODED FIELDS. `mov Vd.16b, Vn.16b` is an ALIAS
// for `orr Vd.16b, Vn.16b, Vn.16b`, so the single operand must reach BOTH `Rn`
// and `Rm`. A wire set that filled only one would leave the other reading v0
// and silently OR the destination with itself.
TEST(AsmArm64RoundingRows, TheSimdRegisterMoveWiresOneSourceIntoTwoFields) {
    auto const q = runFpN("mov %0.16b, %1.16b\n", 128, 2);
    ASSERT_TRUE(q->parsed) << messages(*q);
    EXPECT_TRUE(q->ok) << messages(*q);
    EXPECT_EQ(firstWord(q->bytes), kMov16B) << hex(q->bytes);
    EXPECT_EQ((firstWord(q->bytes) >> 5) & 0x1Fu, 1u) << "Rn must be v1";
    EXPECT_EQ((firstWord(q->bytes) >> 16) & 0x1Fu, 1u)
        << "Rm must ALSO be v1 — the alias ORs the source with itself";

    auto const d = runFpN("mov %0.8b, %1.8b\n", 64, 2);
    ASSERT_TRUE(d->parsed) << messages(*d);
    EXPECT_TRUE(d->ok) << messages(*d);
    EXPECT_EQ(firstWord(d->bytes), kMov8B) << hex(d->bytes);
}

// ★★ THE INTEGER `mov` STILL ELECTS THE INTEGER OPCODE. The dialect row now
// names TWO opcodes and the REGISTER CLASS separates them; a regression in
// that axis would send `mov x0, x1` to the vector move.
TEST(AsmArm64RoundingRows, TheIntegerMoveIsUnaffectedByTheVectorOpcode) {
    auto const r = run2("mov %x0, %x1\n", LirRegClass::GPR, 64, "x0",
                        LirRegClass::GPR, 64, "x1");
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_TRUE(r->ok) << messages(*r);
    EXPECT_NE(firstWord(r->bytes), kMov16B) << hex(r->bytes);
    EXPECT_NE(firstWord(r->bytes), kMov8B) << hex(r->bytes);
    // ORR Xd, XZR, Xm — the integer register move gas emits for `mov x0, x1`.
    EXPECT_EQ(firstWord(r->bytes), 0xAA0103E0u) << hex(r->bytes);
}

// ══ THE STANDING BOUNDARY ══════════════════════════════════════════════════

// ★★★ THE SHIP LIST STATED AS A PREDICATE RATHER THAN AS PROSE. Every spelling
// below is one both references assemble; every one must be nameable here. A
// row silently dropped, renamed, or never landed makes this fail with the
// spelling in the message — which is the difference between a boundary that is
// checked and a boundary that is merely written down.
TEST(AsmArm64RoundingRows, EveryScalarFpRowThisCycleDeclaredIsStillDeclared) {
    auto const doc = nlohmann::json::parse(dialectText());
    std::set<std::string> spelled;
    for (auto const& row : doc.at("assembly").at("instructions")) {
        spelled.insert(row.value("spelling", std::string{}));
    }
    constexpr char const* kShipped[] = {
        "fcvtns", "fcvtnu", "fcvtas", "fcvtau", "fcvtms", "fcvtmu",
        "fcvtps", "fcvtpu",
        "frintn", "frinta", "frintm", "frintp", "frintz", "frinti", "frintx",
        "fabs", "fsqrt",
        "fmax", "fmin", "fmaxnm", "fminnm", "fnmul",
        "fmadd", "fmsub", "fnmadd", "fnmsub",
        "fcmpe",
    };
    for (auto const* s : kShipped) {
        EXPECT_TRUE(spelled.contains(s))
            << "the shipped aarch64 dialect no longer spells `" << s
            << "`, which GNU as 2.42 and clang 18.1.3 both assemble";
    }
    // The SIMD move is spelled `mov`, sharing a row with the integer one.
    EXPECT_TRUE(spelled.contains("mov"));
}

// ★★★ AND THE OTHER SIDE OF THE BOUNDARY: what is DELIBERATELY still refused.
// Each of these IS accepted by gas 2.42 and clang 18.1.3 and MUST stay refused,
// because every one needs an OPERAND FORM this dialect does not model as an
// instruction operand — a condition, an FP literal, or a fixed-point shift
// count in a field no `EncodingSlotKind` names. If one starts assembling, the
// opcode behind it was bound to a neighbour, which is the near-miss class.
// ✔MEASURED words, for the day someone declares them: fcsel d0,d1,d2,eq
// 0x1E620C20 · fccmp d0,d1,#0,eq 0x1E610400 · fccmpe 0x1E610410 · fcmp d0,#0.0
// 0x1E602008 · fmov s0,#1.0 0x1E2E1000 · fcvtzs w0,s1,#4 0x1E18F020.
//
// ⚠ THE TIER EACH ONE REFUSES AT IS PART OF THE PIN, NOT A DETAIL, AND IT WAS
// MEASURED RATHER THAN ASSUMED. `fcsel`/`fccmp`/`fccmpe` PARSE — the condition
// is a bare name, which `armName` accepts — and are refused at ELECTION, so
// their byte stream is the harness's trailing `ret` and nothing else. `fmov
// %s0, #1.0` does NOT parse: this dialect's immediate rule takes no FP literal,
// so it dies at the parser and the harness never builds a function, leaving the
// stream EMPTY. A single `EXPECT_EQ(firstWord, kRet)` over both groups fails on
// the second — which is how this distinction was found — and softening it to
// "empty OR ret" would have made the first group's pin vacuous.
TEST(AsmArm64RoundingRows, TheOperandFormGapsStayRefused) {
    struct Case {
        char const* text;
        bool        parses;  // ✔MEASURED at this base, not assumed
    };
    constexpr Case kCases[] = {
        {"fcsel %d0, %d1, %d2, eq\n",  true},
        {"fccmp %d0, %d1, #0, eq\n",   true},
        {"fccmpe %d0, %d1, #0, eq\n",  true},
        {"fmov %s0, #1.0\n",           false},
    };
    for (auto const& c : kCases) {
        auto const r = runFpN(c.text, 64, 2);
        EXPECT_FALSE(r->parsed && r->ok)
            << c.text << " assembled, and this target declares no opcode whose "
                         "template emits its bytes";
        EXPECT_EQ(r->parsed, c.parses)
            << c.text << " changed the TIER it refuses at: " << messages(*r);
        if (c.parses) {
            EXPECT_EQ(firstWord(r->bytes), kRet)
                << c.text << " emitted " << hex(r->bytes);
        } else {
            EXPECT_TRUE(r->bytes.empty())
                << c.text << " emitted " << hex(r->bytes);
        }
    }
}

// ★ AND THE FEATURE-GATED SPELLINGS, WHICH ARE REFUSED FOR THE OPPOSITE REASON:
// ✔MEASURED, gas 2.42 AND clang 18.1.3 BOTH REJECT them at the default -march
// (`fjcvtzs` needs +jscvt, `frint32z`/`frint64z` need +frintts, and every fp16
// `h`-view scalar form needs +fp16). Declaring them would put DSS ABOVE the
// union, which the bar refuses in the same direction as being below it.
TEST(AsmArm64RoundingRows, FeatureGatedSpellingsStayRefused) {
    for (auto const* text : {"fjcvtzs %w0, %d1\n", "frint32z %d0, %d1\n",
                             "frint64z %d0, %d1\n"}) {
        auto const r = runFpN(text, 64, 2);
        EXPECT_FALSE(r->parsed && r->ok)
            << text << " assembled, and both references refuse it at the "
                       "default -march";
    }
}

// ══ RED ON DISABLE — CONFIG-LEVEL, REMOVE DIRECTION ════════════════════════

// ★★★ MUTANT 1 (DIALECT): remove the eight rounding-conversion ROWS. Every
// spelling must go back to being an unknown mnemonic, and the CONTROL
// `fcvtzs` — whose row is untouched — must still assemble its own word. Without
// the control this arm would read identically if the mutator had broken the
// whole document.
TEST(AsmArm64RoundingRows, RemovingTheRoundingRowsUnspellsEveryMode) {
    auto mutant = mutateShippedDialectDoc(removeInstructionRows(
        {"fcvtns", "fcvtnu", "fcvtas", "fcvtau",
         "fcvtms", "fcvtmu", "fcvtps", "fcvtpu"}));
    ASSERT_TRUE(mutant.has_value())
        << "the mutated dialect did not load — the pin below would measure a "
           "load failure rather than the missing rows";

    for (auto const& c : kConv) {
        auto const r = run2(std::format("{} %w0, %s1\n", c.spelling),
                            LirRegClass::GPR, 32, "x0",
                            LirRegClass::FPR, 32, "v1", *mutant);
        EXPECT_FALSE(r->parsed && r->ok)
            << c.spelling << " still assembled with its dialect row removed";
        EXPECT_EQ(firstWord(r->bytes), kRet)
            << c.spelling << " emitted " << hex(r->bytes);
    }

    // THE CONTROL, on the SAME mutant: `fcvtzs` keeps its row and its word.
    auto const ctl = run2("fcvtzs %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                          LirRegClass::FPR, 32, "v1", *mutant);
    ASSERT_TRUE(ctl->parsed) << messages(*ctl);
    EXPECT_TRUE(ctl->ok) << messages(*ctl);
    EXPECT_EQ(firstWord(ctl->bytes), kFcvtzsWS)
        << "the control lost its word too — the mutant broke the document "
           "rather than removing eight rows: " << hex(ctl->bytes);
}

// ★★★ MUTANT 2 (TARGET): remove the NARROW opcode of one rounding mode. The W
// destination must then elect NOTHING — and must NOT fall back to the X form,
// which is precisely the silent divergence `el` measured on `fcvtzs`.
TEST(AsmArm64RoundingRows, RemovingANarrowRoundingOpcodeRefusesRatherThanWidens) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto& ops = doc.at("opcodes");
            std::size_t removed = 0;
            for (auto it = ops.begin(); it != ops.end();) {
                if (it->value("mnemonic", std::string{})
                    == "fp_to_si_floor32") {
                    it = ops.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            if (removed != 1) {
                throw std::runtime_error{
                    "expected exactly one `fp_to_si_floor32` opcode in the "
                    "shipped arm64 target, found " + std::to_string(removed)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing opcode";

    auto const r = run2("fcvtms %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                        LirRegClass::FPR, 32, "v1", nullptr, *mutant);
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_NE(firstWord(r->bytes), 0x1E300020u)
        << "the W word survived the deletion of the opcode that declares it — "
           "the byte pin is not reading the target: " << hex(r->bytes);
    EXPECT_NE(firstWord(r->bytes), 0x9E300020u)
        << "the X form was emitted for a W destination once the W opcode was "
           "removed — the destination width is not routing the election: "
        << hex(r->bytes);

    // THE CONTROL, on the SAME mutant: the other seven modes are untouched, so
    // `fcvtns %w0, %s1` must still emit its own word.
    auto const ctl = run2("fcvtns %w0, %s1\n", LirRegClass::GPR, 32, "x0",
                          LirRegClass::FPR, 32, "v1", nullptr, *mutant);
    ASSERT_TRUE(ctl->parsed) << messages(*ctl);
    EXPECT_TRUE(ctl->ok) << messages(*ctl);
    EXPECT_EQ(firstWord(ctl->bytes), 0x1E200020u) << hex(ctl->bytes);
}

// ★★★ MUTANT 3 (TARGET): delete ONE wire of the SIMD register move — the `rm`
// half of the alias. The instruction must not quietly emit an ORR whose second
// source is v0. ⚠ The loader's own coverage rule does not catch this: the
// operand IS covered, by the surviving `rn` wire, so nothing at load time can
// see that a FIELD lost its source. The byte is the only witness.
TEST(AsmArm64RoundingRows, DroppingTheAliasSecondWireChangesTheBytes) {
    auto mutant = test_support::mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            std::size_t removed = 0;
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", std::string{}) != "move_bytes") {
                    continue;
                }
                for (auto& v : op.at("encoding").at("variants")) {
                    auto& wires = v.at("wires");
                    for (auto it = wires.begin(); it != wires.end();) {
                        if (it->value("slotKind", std::string{}) == "rm") {
                            it = wires.erase(it);
                            ++removed;
                        } else {
                            ++it;
                        }
                    }
                }
            }
            if (removed != 2) {
                throw std::runtime_error{
                    "expected two `rm` wires across `move_bytes`, found "
                    + std::to_string(removed)};
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutated target did not load — the pin would measure a load "
           "failure rather than the missing wire";

    auto const r = runFpN("mov %0.16b, %1.16b\n", 128, 2, nullptr, *mutant);
    ASSERT_TRUE(r->parsed) << messages(*r);
    EXPECT_NE(firstWord(r->bytes), kMov16B)
        << "the alias word survived the deletion of the wire that fills its "
           "Rm field — the pin is not reading the target: " << hex(r->bytes);

    // THE CONTROL, on the SAME mutant: `fmov %d0, %d1` is a different opcode
    // and must be untouched.
    auto const ctl = runFpN("fmov %d0, %d1\n", 64, 2, nullptr, *mutant);
    ASSERT_TRUE(ctl->parsed) << messages(*ctl);
    EXPECT_TRUE(ctl->ok) << messages(*ctl);
    EXPECT_EQ(firstWord(ctl->bytes), 0x1E604020u) << hex(ctl->bytes);
}

// ★★ MUTANT 4 (DIALECT): remove the `fcmpe` row. The signalling compare must go
// back to being an unknown mnemonic — and the quiet `fcmp`, one bit away, must
// still assemble. The two are the closest neighbours in this whole file, so
// they are the sharpest pair to separate.
TEST(AsmArm64RoundingRows, RemovingTheSignallingRowLeavesTheQuietOne) {
    auto mutant = mutateShippedDialectDoc(removeInstructionRows({"fcmpe"}));
    ASSERT_TRUE(mutant.has_value()) << "the mutated dialect did not load";

    auto const r = runFpN("fcmpe %d0, %d1\n", 64, 2, *mutant);
    EXPECT_FALSE(r->parsed && r->ok)
        << "fcmpe still assembled with its row removed";
    EXPECT_EQ(firstWord(r->bytes), kRet) << hex(r->bytes);

    auto const ctl = runFpN("fcmp %d0, %d1\n", 64, 2, *mutant);
    ASSERT_TRUE(ctl->parsed) << messages(*ctl);
    EXPECT_TRUE(ctl->ok) << messages(*ctl);
    EXPECT_EQ(firstWord(ctl->bytes), kFcmpD) << hex(ctl->bytes);
}
