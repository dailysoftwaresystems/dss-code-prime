// HARDWARE ENCODING 31 ON AArch64 — `sp`/`wsp` AND `xzr`/`wzr` SHARE IT, AND
// WHICH ONE A REGISTER FIELD MEANS IS A PROPERTY OF THE FIELD.
// [[D-ASM-ARM64-SP-AND-XZR-SHARE-ENCODING-31-SO-MOV-SP-SILENTLY-BECOMES-ZERO]]
//
// ★★★ WHAT WAS WRONG, AND IT WAS A SILENT MISCOMPILE ON TWO OF THE COMMONEST
// LINES IN HAND-WRITTEN aarch64. ✔MEASURED at the P55 base through the REAL CLI
// (`--language asm-arm64-gas --target arm64:elf64-aarch64-linux`), every word
// read back out of the emitted object with `aarch64-linux-gnu-objdump`:
//
//     mov x0, sp     ->  0xAA1F03E0  = `mov x0, xzr`   — x0 receives ZERO
//     mov sp, x0     ->  0xAA0003FF  = `mov xzr, x0`   — a NO-OP
//     add x0, sp, x1 ->  0x8B0103E0  = `add x0, xzr, x1`
//     add sp, sp, x1 ->  0x8B0103FF  — THE STACK POINTER IS NEVER ADJUSTED
//     cmp sp, x0     ->  0xEB0003FF  = `cmp xzr, x0`
//     add x0, xzr,#16->  0x910043E0  = `add x0, sp, #16` — an ADDRESS, not 16
//     add xzr, x0,#16->  0x9100401F  = `add sp, x0, #16` — SP CLOBBERED
//     ldr x0, [xzr]  ->  0xF94003E0  = `ldr x0, [sp]`
//
// rc=0, no diagnostic, wrong code, every time. gas 2.42 and clang 18.1.3 —
// probed SEPARATELY and agreeing on ALL 128 probes of the census that produced
// this file's constants — emit 0x910003E0 / 0x9100001F / 0x8B2163E0 /
// 0x8B2163FF / 0xEB2063FF for the first five and REFUSE the last three.
//
// ★★★ THE CAUSE IS ONE NUMBER MEANING TWO REGISTERS. `sp` and `xzr` are two
// rows of `registers[]` at `hwEncoding` 31, and AArch64 reads that field as the
// STACK POINTER in add/subtract-immediate, in the extended-register forms and
// as a load/store BASE, and as the ZERO REGISTER everywhere else. Nothing in the
// target document said so, so whichever row the operand resolved to reached the
// encoder and the encoder wrote 31 either way. The fix declares the reading:
// `encodingRole` + `encodingRoleIsDefault` on the register rows, `regRole` /
// `resultRegRole` / `requiresRegRole` on the fields.
//
// ★★ WHY THE PINS BELOW ARE BYTE PINS **AND** REFUSAL PINS IN EQUAL NUMBER.
// Half of this defect was DSS emitting the wrong word; the other half was DSS
// accepting spellings both references reject and emitting a plausible
// instruction naming the OTHER register. A file that only pinned the positive
// direction would pass on a fix that routed everything through the ADD form and
// broke the zero register instead — so `mov x0, xzr`, `mov x0, x1`,
// `add x0, x1, xzr` and `str xzr, [sp]` are pinned by name as CONTROLS.

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
#include <memory>
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

// One AArch64 instruction is exactly one 32-bit word, so the first word is the
// WHOLE instruction and no field can be checked while a neighbour drifts.
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

struct Case { char const* text; std::uint32_t want; char const* why; };

constexpr std::uint32_t kRet = 0xD65F03C0u;

// "The template contributed no instruction" has TWO byte shapes on this path — a
// LOWERING refusal leaves the harness's appended `ret` alone (four bytes), an
// ENCODING refusal aborts the function and the stream is EMPTY. The invariant
// that covers both, and excludes an emitted wrong instruction, is: at most the
// return, and if four bytes then exactly the return.
//
// ⚠ AND THE ARM MUST **FAIL**, NEVER RAISE. Every assertion below is on a value
// that exists whatever happened, so a refusal under a mutant reddens this arm
// instead of aborting the run and silently cancelling the controls that follow.
void expectContributedNothing(Run const& r, char const* text) {
    EXPECT_GT(r.reporter.errorCount() + r.asmReporter.errorCount(), 0u)
        << text << " produced no bytes and no diagnostic — a silent drop is "
                   "worse than either a refusal or a wrong answer";
    EXPECT_LE(r.bytes.size(), 4u)
        << text << " emitted an INSTRUCTION on top of the harness return: "
        << hex(r.bytes);
    if (r.bytes.size() == 4) {
        EXPECT_EQ(firstWord(r.bytes), kRet)
            << text << " emitted a four-byte word that is not the harness "
            << "`ret` — the template encoded something: " << hex(r.bytes);
    }
}

void expectWordsOn(std::shared_ptr<TargetSchema> const& target,
                   std::span<Case const>               cases) {
    for (auto const& c : cases) {
        auto const r = runOn(loadDialect(), target, c.text);
        EXPECT_TRUE(r->parsed) << c.text << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.text << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.text << " emitted " << hex(r->bytes) << " — " << c.why;
    }
}

void expectWords(std::span<Case const> cases) {
    expectWordsOn(shippedTarget(), cases);
}

// ── THE MEASURED WORDS ─────────────────────────────────────────────────────
//
// ✔gas 2.42 AND clang 18.1.3, probed SEPARATELY, agreeing on every one, each
// read back out of an assembled object with `aarch64-linux-gnu-objdump`.
//
// ⚠ EVERY FORM IS PINNED AT MORE THAN ONE REGISTER PLACEMENT wherever the
// encoding has more than one field, because a field-placement error (Rd where
// Rn belongs) is invisible in a single Rd=0/Rn=sp probe.

// MOV (to/from SP) = ADD (immediate) with imm12 = 0 — ARM ARM C6.2.209.
constexpr std::uint32_t kMovX0FromSp  = 0x910003E0u;  // mov x0,  sp
constexpr std::uint32_t kMovX30FromSp = 0x910003FEu;  // mov x30, sp
constexpr std::uint32_t kMovSpFromX0  = 0x9100001Fu;  // mov sp,  x0
constexpr std::uint32_t kMovSpFromX30 = 0x910003DFu;  // mov sp,  x30
constexpr std::uint32_t kMovSpFromSp  = 0x910003FFu;  // mov sp,  sp
constexpr std::uint32_t kMovW0FromWsp = 0x110003E0u;  // mov w0,  wsp
constexpr std::uint32_t kMovWspFromW0 = 0x1100001Fu;  // mov wsp, w0

// ★★★ THE CONTROLS — the ZERO-REGISTER readings the fix must NOT have moved.
constexpr std::uint32_t kMovX0FromXzr = 0xAA1F03E0u;  // mov x0,  xzr (ORR)
constexpr std::uint32_t kMovXzrFromX0 = 0xAA0003FFu;  // mov xzr, x0  (ORR)
constexpr std::uint32_t kMovX0FromX1  = 0xAA0103E0u;  // mov x0,  x1  (ORR)
constexpr std::uint32_t kMovW0FromWzr = 0x2A1F03E0u;  // mov w0,  wzr (ORR)
constexpr std::uint32_t kMovW0FromW1  = 0x2A0103E0u;  // mov w0,  w1  (ORR)

// ADD/SUB (immediate): Rd and Rn both read 31 as SP.
constexpr std::uint32_t kAddSpSp16  = 0x910043FFu;  // add sp, sp, #16
constexpr std::uint32_t kAddX0Sp16  = 0x910043E0u;  // add x0, sp, #16
constexpr std::uint32_t kSubSpSp16  = 0xD10043FFu;  // sub sp, sp, #16
constexpr std::uint32_t kSubX0Sp16  = 0xD10043E0u;  // sub x0, sp, #16
constexpr std::uint32_t kAddX0X1_16 = 0x91004020u;  // add x0, x1, #16 (control)

// ADD/SUB/CMP (extended register) — the only reg-reg forms that can name SP.
constexpr std::uint32_t kAddX0SpX1   = 0x8B2163E0u;  // add x0,  sp,  x1
constexpr std::uint32_t kAddSpSpX1   = 0x8B2163FFu;  // add sp,  sp,  x1
constexpr std::uint32_t kAddX30SpX29 = 0x8B3D63FEu;  // add x30, sp,  x29
constexpr std::uint32_t kAddW0WspW1  = 0x0B2143E0u;  // add w0,  wsp, w1
constexpr std::uint32_t kAddWspWspW1 = 0x0B2143FFu;  // add wsp, wsp, w1
constexpr std::uint32_t kSubX0SpX1   = 0xCB2163E0u;  // sub x0,  sp,  x1
constexpr std::uint32_t kSubSpSpX1   = 0xCB2163FFu;  // sub sp,  sp,  x1
constexpr std::uint32_t kSubW9WspW10 = 0x4B2A43E9u;  // sub w9,  wsp, w10
constexpr std::uint32_t kCmpSpX0     = 0xEB2063FFu;  // cmp sp,  x0
constexpr std::uint32_t kCmpSpX30    = 0xEB3E63FFu;  // cmp sp,  x30
constexpr std::uint32_t kCmpWspW0    = 0x6B2043FFu;  // cmp wsp, w0

// The SHIFTED-register CONTROLS — the forms an SP spelling must never reach.
constexpr std::uint32_t kAddX0X1X2  = 0x8B020020u;  // add x0, x1, x2
constexpr std::uint32_t kAddX0X1Xzr = 0x8B1F0020u;  // add x0, x1, xzr
constexpr std::uint32_t kAddX0XzrX1 = 0x8B0103E0u;  // add x0, xzr, x1
constexpr std::uint32_t kSubX0X1X2  = 0xCB020020u;  // sub x0, x1, x2
constexpr std::uint32_t kCmpX0X1    = 0xEB01001Fu;  // cmp x0, x1
constexpr std::uint32_t kCmpX0Xzr   = 0xEB1F001Fu;  // cmp x0, xzr

// CMP (immediate) = SUBS ZR, <Xn|SP>, #imm12 — Rn reads 31 as SP.
constexpr std::uint32_t kCmpSp16   = 0xF10043FFu;  // cmp sp, #16
constexpr std::uint32_t kCmpX0_16  = 0xF100401Fu;  // cmp x0, #16 (control)

// Memory: the BASE reads 31 as SP, the TRANSFERRED register reads it as ZR.
constexpr std::uint32_t kLdrX0Sp8   = 0xF94007E0u;  // ldr x0,  [sp, #8]
constexpr std::uint32_t kLdrX29Sp16 = 0xF9400BFDu;  // ldr x29, [sp, #16]
constexpr std::uint32_t kStrXzrSp   = 0xF90003FFu;  // str xzr, [sp]
constexpr std::uint32_t kStrX0Sp8   = 0xF90007E0u;  // str x0,  [sp, #8]
constexpr std::uint32_t kLdrXzrX1   = 0xF940003Fu;  // ldr xzr, [x1]
constexpr std::uint32_t kLdrX0X1    = 0xF9400020u;  // ldr x0,  [x1] (control)

} // namespace

// ── THE POSITIVE HALF: every stack-pointer spelling both references assemble ─

TEST(AsmArm64SharedEncoding31, MovToAndFromSpEncodesTheAddImmediateAlias) {
    constexpr Case kCases[] = {
        {"mov x0, sp\n",  kMovX0FromSp,
         "`mov Xd, SP` is `ADD Xd, SP, #0`, not `ORR Xd, XZR, Xm`"},
        {"mov x30, sp\n", kMovX30FromSp,
         "a second Rd placement — Rd must move while Rn stays 31"},
        {"mov sp, x0\n",  kMovSpFromX0,
         "`mov SP, Xn` writes the STACK POINTER, and must not be a no-op"},
        {"mov sp, x30\n", kMovSpFromX30, "a second Rn placement"},
        {"mov sp, sp\n",  kMovSpFromSp,
         "both ends SP — the ADD form's Rd and Rn are both 31"},
        {"mov w0, wsp\n", kMovW0FromWsp,
         "the sf=0 sibling; a W write also zeroes bits 63:32"},
        {"mov wsp, w0\n", kMovWspFromW0, "the W form, the other direction"},
    };
    expectWords(kCases);
}

TEST(AsmArm64SharedEncoding31, TheZeroRegisterMovesDidNotMove) {
    // ★★★ THE PIN THAT CATCHES A FIX THAT ROUTED EVERYTHING THROUGH THE ADD
    // FORM. Each of these is byte-identical to what DSS emitted BEFORE the
    // change; a fix that widened the SP form to swallow them would be caught
    // here and nowhere else.
    constexpr Case kCases[] = {
        {"mov x0, xzr\n",  kMovX0FromXzr, "the zero register keeps its ORR"},
        {"mov xzr, x0\n",  kMovXzrFromX0, "a discard keeps its ORR"},
        {"mov x0, x1\n",   kMovX0FromX1,  "an ordinary copy keeps its ORR"},
        {"mov w0, wzr\n",  kMovW0FromWzr, "the W zero register keeps its ORR"},
        {"mov w0, w1\n",   kMovW0FromW1,  "the W copy keeps its ORR"},
    };
    expectWords(kCases);
}

TEST(AsmArm64SharedEncoding31, AddAndSubImmediateReadThirtyOneAsTheStackPointer) {
    constexpr Case kCases[] = {
        {"add sp, sp, #16\n", kAddSpSp16, "Rd and Rn are both SP"},
        {"add x0, sp, #16\n", kAddX0Sp16, "Rn is SP, Rd is an ordinary reg"},
        {"sub sp, sp, #16\n", kSubSpSp16, "the SUB twin"},
        {"sub x0, sp, #16\n", kSubX0Sp16, "the SUB twin, one end ordinary"},
        {"add x0, x1, #16\n", kAddX0X1_16,
         "CONTROL: no 31 anywhere, the same word as before the change"},
        {"cmp sp, #16\n",     kCmpSp16,
         "SUBS ZR, SP, #imm — Rn is SP while the baked Rd is the zero reg"},
        {"cmp x0, #16\n",     kCmpX0_16, "CONTROL: an ordinary compare"},
    };
    expectWords(kCases);
}

TEST(AsmArm64SharedEncoding31, TheExtendedRegisterFormsCarryTheStackPointer) {
    constexpr Case kCases[] = {
        {"add x0, sp, x1\n",    kAddX0SpX1,   "ADD (extended register), UXTX"},
        {"add sp, sp, x1\n",    kAddSpSpX1,   "Rd = SP as well"},
        {"add x30, sp, x29\n",  kAddX30SpX29, "a second Rd/Rm placement"},
        {"add w0, wsp, w1\n",   kAddW0WspW1,  "the sf=0 sibling, option UXTW"},
        {"add wsp, wsp, w1\n",  kAddWspWspW1, "W, both ends"},
        {"sub x0, sp, x1\n",    kSubX0SpX1,   "the SUB twin"},
        {"sub sp, sp, x1\n",    kSubSpSpX1,   "the SUB twin, Rd = SP"},
        {"sub w9, wsp, w10\n",  kSubW9WspW10, "a second W placement"},
        {"cmp sp, x0\n",        kCmpSpX0,     "SUBS ZR, SP, Xm extended"},
        {"cmp sp, x30\n",       kCmpSpX30,    "a second Rm placement"},
        {"cmp wsp, w0\n",       kCmpWspW0,    "the W compare"},
    };
    expectWords(kCases);
}

TEST(AsmArm64SharedEncoding31, TheShiftedRegisterFormsDidNotMove) {
    // The other direction of the same fence: an ordinary reg-reg line must
    // still take the SHIFTED form, whose bytes differ from the extended form
    // in the `option` field — a difference no width or bank axis can see.
    constexpr Case kCases[] = {
        {"add x0, x1, x2\n",   kAddX0X1X2,  "CONTROL: shifted-register ADD"},
        {"add x0, x1, xzr\n",  kAddX0X1Xzr, "CONTROL: Rm = 31 is the zero reg"},
        {"add x0, xzr, x1\n",  kAddX0XzrX1, "CONTROL: Rn = 31 is the zero reg"},
        {"sub x0, x1, x2\n",   kSubX0X1X2,  "CONTROL: shifted-register SUB"},
        {"cmp x0, x1\n",       kCmpX0X1,    "CONTROL: shifted-register CMP"},
        {"cmp x0, xzr\n",      kCmpX0Xzr,   "CONTROL: compare against zero"},
    };
    expectWords(kCases);
}

TEST(AsmArm64SharedEncoding31, MemoryReadsItsBaseAsSpAndItsDataRegisterAsZero) {
    // ★★ THE TWO ENDS OF ONE INSTRUCTION DISAGREE ABOUT WHAT 31 MEANS, which is
    // the sharpest statement this anchor makes. ✔MEASURED on both references:
    // `str xzr,[sp]` assembles, `str sp,[x0]` and `ldr x0,[xzr]` do not.
    constexpr Case kCases[] = {
        {"ldr x0, [sp, #8]\n",   kLdrX0Sp8,   "the base is the stack pointer"},
        {"ldr x29, [sp, #16]\n", kLdrX29Sp16, "a second Rt placement"},
        {"str x0, [sp, #8]\n",   kStrX0Sp8,   "the store twin"},
        {"str xzr, [sp]\n",      kStrXzrSp,
         "Rt = 31 is the ZERO register in the very instruction whose base is "
         "the stack pointer"},
        {"ldr xzr, [x1]\n",      kLdrXzrX1,   "a discarding load"},
        {"ldr x0, [x1]\n",       kLdrX0X1,    "CONTROL: no 31 anywhere"},
    };
    expectWords(kCases);
}

TEST(AsmArm64SharedEncoding31, AMultiWordMacroWhoseWordsDisagreeTakesNeither) {
    // ★★★ THE SHARPEST CASE IN THIS FILE, AND IT WAS FOUND BY RE-READING EACH
    // ANNOTATION AGAINST ITS OWN FIXED WORD RATHER THAN BY A TEST. arm64's
    // `lea` — which the dialect spells `adr` — is the two-word macro
    // `ADRP Xd, sym; ADD Xd, Xd, #:lo12:sym`, and it places ONE destination
    // register in two fields that read encoding 31 DIFFERENTLY: ADRP's Rd is
    // the ZERO register, ADD-immediate's Rd is the STACK POINTER.
    //
    // ✔MEASURED mid-cycle, AFTER the rest of this anchor had landed:
    // `adr xzr, main` compiled rc=0 and emitted 0x9000001F (`adrp xzr, main`)
    // followed by **0x910003FF — `add sp, sp, #:lo12:main`**. The stack pointer
    // was overwritten with a relocated address, silently, by a line whose
    // destination the programmer wrote as the DISCARD register.
    //
    // ⓘ THE ANSWER IS THAT NEITHER SHARED SPELLING FITS: a destination must
    // satisfy EVERY placement, and no register can be the zero register in one
    // word and the stack pointer in the next. gas 2.42 assembles `adr xzr,
    // main` as a single-word ADR (0x1000001F) — an encoding DSS does not
    // declare — so a loud refusal is a capability gap and no longer a wrong
    // answer. `adr sp, main` is refused by both references too.
    //
    // ⚠⚠ THIS ARM ASSERTS THE **CONFIG AND THE PREDICATE**, NOT A LOWERED
    // TEMPLATE, AND THAT IS A CORRECTION THIS FILE MADE TO ITSELF. The first
    // draft ran `adr xzr, main` through the template harness above and passed —
    // VACUOUSLY: an assembly TEMPLATE has no labels of its own, so this harness
    // refuses EVERY `adr` line for a reason that has nothing to do with
    // register roles, and the CONTROL (`adr x0, main`, which must still work)
    // is what caught it. ✔The behaviour was therefore measured where it can be
    // spelled — the real CLI over a standalone `.s`, after this fix: `adr xzr,
    // main` and `adr sp, main` are REFUSED, and `adr x0, main` still emits
    // 0x90000000 + 0x91000000 with both relocations.
    auto const target = shippedTarget();
    auto const lea = target->opcodeByMnemonic("lea");
    ASSERT_TRUE(lea.has_value()) << "the arm64 target declares no `lea`";
    auto const* info = target->opcodeInfo(*lea);
    ASSERT_NE(info, nullptr);

    auto const sp  = target->registerByName("sp");
    auto const xzr = target->registerByName("xzr");
    auto const x0  = target->registerByName("x0");
    ASSERT_TRUE(sp.has_value() && xzr.has_value() && x0.has_value());

    std::size_t disagreeing = 0;
    for (auto const& v : info->encoding.variants) {
        if (!v.resultSlot.has_value() || v.extraResultSlots.empty()) continue;
        bool const spFits =
            target->registerFitsFieldRole(*sp, v.resultRegRole)
            && std::all_of(v.extraResultSlots.begin(),
                           v.extraResultSlots.end(), [&](auto const& x) {
                               return target->registerFitsFieldRole(*sp,
                                                                    x.regRole);
                           });
        bool const zrFits =
            target->registerFitsFieldRole(*xzr, v.resultRegRole)
            && std::all_of(v.extraResultSlots.begin(),
                           v.extraResultSlots.end(), [&](auto const& x) {
                               return target->registerFitsFieldRole(*xzr,
                                                                    x.regRole);
                           });
        bool const x0Fits =
            target->registerFitsFieldRole(*x0, v.resultRegRole)
            && std::all_of(v.extraResultSlots.begin(),
                           v.extraResultSlots.end(), [&](auto const& x) {
                               return target->registerFitsFieldRole(*x0,
                                                                    x.regRole);
                           });
        EXPECT_TRUE(x0Fits)
            << "an ORDINARY register stopped fitting a `lea` variant's result "
               "placements — the annotation went too far and `adr x0, sym` "
               "would be refused";
        if (!spFits && !zrFits) ++disagreeing;
    }
    EXPECT_GT(disagreeing, 0u)
        << "no multi-placement `lea` variant refuses BOTH `sp` and `xzr` — the "
           "ADRP word reads encoding 31 as the zero register and the ADD word "
           "reads it as the stack pointer, so a variant that accepts either "
           "one will write the OTHER register in one of its two words. That is "
           "the `adr xzr, main` -> `add sp, sp, #:lo12:main` defect, back.";
}

// ── THE REFUSAL HALF: every spelling BOTH references reject ─────────────────

TEST(AsmArm64SharedEncoding31, EveryMixedOrWrongReadingOfThirtyOneIsRefused) {
    // ✔MEASURED: gas 2.42 and clang 18.1.3 REFUSE every one of these, and DSS
    // ACCEPTED every one of them at the P55 base, emitting a plausible
    // instruction that named the OTHER register.
    for (auto const* text : {
             "mov sp, xzr\n",       // the two readings cannot be mixed
             "mov xzr, sp\n",
             "add x0, xzr, #16\n",  // add-immediate's Rn is SP-only
             "add xzr, x0, #16\n",  // ... and so is its Rd
             "cmp xzr, #16\n",      // SUBS-immediate's Rn is SP-only
             "add x0, x1, sp\n",    // Rm is ZR-only, in every form
             "add x0, sp, sp\n",
             "cmp x0, sp\n",
             "cmp sp, sp\n",
             "and x0, sp, x1\n",    // the logical forms cannot name SP
             "orr x0, sp, x1\n",
             "mvn x0, sp\n",
             "ldr x0, [xzr]\n",     // a base of 31 is always SP
             "ldr sp, [x0]\n",      // a transferred register of 31 is ZR
             "str sp, [x0]\n",
             "mov x0, wsp\n",       // the widths may not be mixed
             "mov w0, sp\n",
         }) {
        auto const r = runBare(text);
        expectContributedNothing(*r, text);
    }
}

TEST(AsmArm64SharedEncoding31, ARefusalNamesTheOpcodesItTried) {
    // A fail-loud that says nothing is a refusal the author cannot act on.
    auto const r = runBare("mov sp, xzr\n");
    EXPECT_FALSE(r->ok);
    auto const why = messages(*r);
    EXPECT_NE(why.find("mov"), std::string::npos)
        << "the refusal does not name the mnemonic that failed: " << why;
    EXPECT_GT(r->reporter.errorCount() + r->asmReporter.errorCount(), 0u)
        << "no diagnostic at all for `mov sp, xzr`";
}

// ── THE TARGET TABLE'S OWN INVARIANTS ──────────────────────────────────────

TEST(AsmArm64SharedEncoding31, TheSharedEncodingRowsDeclareDistinctRoles) {
    auto const target = shippedTarget();
    struct Row { char const* name; char const* role; bool isDefault; };
    constexpr Row kRows[] = {
        {"xzr", "zeroRegister",  true},
        {"sp",  "stackPointer", false},
        {"wzr", "zeroRegister",  true},
        {"wsp", "stackPointer", false},
    };
    for (auto const& row : kRows) {
        auto const ord = target->registerByName(row.name);
        ASSERT_TRUE(ord.has_value())
            << row.name << " is not a register this target declares";
        auto const* info = target->registerInfo(*ord);
        ASSERT_NE(info, nullptr) << row.name;
        EXPECT_EQ(info->encodingRole, row.role) << row.name;
        EXPECT_EQ(info->encodingRoleIsDefault, row.isDefault) << row.name;
        EXPECT_EQ(info->hwEncoding, 31u)
            << row.name << " no longer shares encoding 31 — this whole file is "
                           "about the collision, and it has moved";
    }
    // The ordinals must stay DISTINCT: one ordinal for two registers is the
    // other way this defect could come back, and `canonicalAsmRegister` walks
    // `subOf` only, so a merged row would be a silent wrong-register answer.
    EXPECT_NE(target->registerByName("sp"), target->registerByName("xzr"));
    EXPECT_NE(target->registerByName("wsp"), target->registerByName("wzr"));
}

TEST(AsmArm64SharedEncoding31, ARoleLessRegisterFitsEveryReading) {
    // The axis discriminates ONLY among the rows that share a number. `x0` is
    // register 0 in both readings, and narrowing it would refuse the ordinary
    // case in every form at once.
    auto const target = shippedTarget();
    auto const x0 = target->registerByName("x0");
    ASSERT_TRUE(x0.has_value());
    EXPECT_TRUE(target->registerFitsFieldRole(*x0, ""));
    EXPECT_TRUE(target->registerFitsFieldRole(*x0, "stackPointer"));
    EXPECT_TRUE(target->registerFitsFieldRole(*x0, "zeroRegister"));

    auto const sp  = target->registerByName("sp");
    auto const xzr = target->registerByName("xzr");
    ASSERT_TRUE(sp.has_value());
    ASSERT_TRUE(xzr.has_value());
    EXPECT_FALSE(target->registerFitsFieldRole(*sp, ""))
        << "`sp` is not the default reading, so a silent field must refuse it";
    EXPECT_TRUE(target->registerFitsFieldRole(*sp, "stackPointer"));
    EXPECT_FALSE(target->registerFitsFieldRole(*sp, "zeroRegister"));
    EXPECT_TRUE(target->registerFitsFieldRole(*xzr, ""))
        << "`xzr` IS the default reading, which is what keeps ~250 variants "
           "silent and correct";
    EXPECT_FALSE(target->registerFitsFieldRole(*xzr, "stackPointer"));
}

TEST(AsmArm64SharedEncoding31, BothShippedTargetsSatisfyTheRoleInvariant) {
    // The rule `validate()` enforces, re-derived here over the shipped tables
    // so a future register row that collides without declaring a role is
    // caught by NAME rather than by a load failure somewhere downstream.
    for (auto const* name : {"arm64", "x86_64"}) {
        auto t = TargetSchema::loadShipped(name);
        ASSERT_TRUE(t.has_value()) << name << " did not load";
        auto const& target = *t;
        for (std::size_t i = 0; i < target->registerCount(); ++i) {
            auto const* a = target->registerInfo(static_cast<std::uint16_t>(i));
            ASSERT_NE(a, nullptr);
            std::size_t group = 0;
            std::size_t defaults = 0;
            for (std::size_t j = 0; j < target->registerCount(); ++j) {
                auto const* b =
                    target->registerInfo(static_cast<std::uint16_t>(j));
                if (b->regClass != a->regClass || b->widthBytes != a->widthBytes
                    || b->hwEncoding != a->hwEncoding) {
                    continue;
                }
                ++group;
                if (b->encodingRoleIsDefault) ++defaults;
                if (j != i && !b->encodingRole.empty()
                    && b->encodingRole == a->encodingRole) {
                    ADD_FAILURE()
                        << name << ": '" << a->name << "' and '" << b->name
                        << "' share one encoding and one role — the role is "
                           "what tells them apart";
                }
            }
            if (group > 1) {
                EXPECT_FALSE(a->encodingRole.empty())
                    << name << ": '" << a->name
                    << "' shares its (class, width, hwEncoding) with "
                    << (group - 1) << " other row(s) and declares no role";
                EXPECT_EQ(defaults, 1u)
                    << name << ": the group containing '" << a->name
                    << "' declares " << defaults << " default reading(s)";
            } else {
                EXPECT_TRUE(a->encodingRole.empty())
                    << name << ": '" << a->name
                    << "' declares a role but shares its encoding with nothing";
            }
        }
    }
}

// ── LOAD-TIME REFUSALS: the rules that make the defect UNSAYABLE ────────────

namespace {

// Find a register row by name inside a raw target document.
[[nodiscard]] nlohmann::json* findRegisterRow(nlohmann::json& doc,
                                              std::string_view name) {
    if (!doc.contains("registers")) return nullptr;
    for (auto& r : doc.at("registers")) {
        auto it = r.find("name");
        if (it != r.end() && it->is_string()
            && it->get<std::string>() == name) {
            return &r;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string whyNotLoaded(
        std::vector<ConfigDiagnostic> const& errs) {
    std::string out;
    for (auto const& e : errs) { out += e.path + ": " + e.message + "\n"; }
    return out;
}

} // namespace

TEST(AsmArm64SharedEncoding31, ACollidingRowWithNoRoleIsRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto* sp = findRegisterRow(doc, "sp");
            ASSERT_NE(sp, nullptr) << "the shipped arm64 has no `sp` row";
            sp->erase("encodingRole");
        });
    EXPECT_FALSE(mutant.has_value())
        << "a target whose `sp` shares hwEncoding 31 with `xzr` and declares "
           "no role LOADED CLEAN — that is exactly the document shape that "
           "made `mov x0, sp` emit `mov x0, xzr`";
    if (!mutant.has_value()) {
        auto const why = whyNotLoaded(mutant.error());
        EXPECT_NE(why.find("encodingRole"), std::string::npos)
            << "the refusal does not name the missing key: " << why;
    }
}

TEST(AsmArm64SharedEncoding31, TwoRowsClaimingOneRoleIsRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto* sp = findRegisterRow(doc, "sp");
            ASSERT_NE(sp, nullptr);
            (*sp)["encodingRole"] = "zeroRegister";
        });
    EXPECT_FALSE(mutant.has_value())
        << "`sp` and `xzr` both claiming role 'zeroRegister' loaded clean — "
           "the collision is then unresolved and the reading is a coin flip";
}

TEST(AsmArm64SharedEncoding31, AGroupWithNoDefaultReadingIsRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto* xzr = findRegisterRow(doc, "xzr");
            ASSERT_NE(xzr, nullptr);
            xzr->erase("encodingRoleIsDefault");
        });
    EXPECT_FALSE(mutant.has_value())
        << "a shared encoding with NO default reading loaded clean — every "
           "field that names no `regRole` would then refuse every register in "
           "the group, silently unspelling both";
}

TEST(AsmArm64SharedEncoding31, TwoDefaultReadingsAreRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto* sp = findRegisterRow(doc, "sp");
            ASSERT_NE(sp, nullptr);
            (*sp)["encodingRoleIsDefault"] = true;
        });
    EXPECT_FALSE(mutant.has_value())
        << "two default readings of one encoding loaded clean — the fall-back "
           "a silent field resolves to would be whichever row came first";
}

TEST(AsmArm64SharedEncoding31, ARoleOnANonCollidingRowIsRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto* x5 = findRegisterRow(doc, "x5");
            ASSERT_NE(x5, nullptr);
            (*x5)["encodingRole"] = "stackPointer";
        });
    EXPECT_FALSE(mutant.has_value())
        << "a role on a register whose encoding is its own loaded clean — "
           "every field that does not name that role would then refuse `x5`, "
           "a documentation key silently unspelling a register";
}

TEST(AsmArm64SharedEncoding31, TheGroupRuleIsScopedToTargetsWithARegisterField) {
    // ★★ THE FENCE AROUND THE SCOPE, AND IT IS A FENCE RATHER THAN A NOTE.
    // The group rule states *two registers that can reach ONE register field
    // must say which reading each is*, so it is scoped to a target that HAS a
    // register field. A scope with no test is indistinguishable from a hole, so
    // both sides are asserted here: the SAME colliding pair loads clean with no
    // encoding anywhere, and is REFUSED the moment one opcode grows a field
    // that could receive a register.
    //
    // ⓘ ✔MEASURED that the permissive half is a real document shape and not a
    // hypothetical: three fixtures in `tests/core/test_target_schema.cpp`
    // declare two or three GPRs with no `hwEncoding` at all (every one
    // defaulting to 0) while testing `implicitRegisters` and `linkRegister`.
    constexpr char const* kNoField =
        R"({"dssTargetVersion":1,"target":{"name":"Collide"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[
              {"name":"a","class":"gpr","widthBytes":8,"hwEncoding":31},
              {"name":"b","class":"gpr","widthBytes":8,"hwEncoding":31}
            ]})";
    auto const permissive = TargetSchema::loadFromText(kNoField, "<inline>");
    EXPECT_TRUE(permissive.has_value())
        << "a target with NO register-bearing encoding field anywhere was "
           "refused for a collision no field could ever observe: "
        << (permissive.has_value() ? std::string{}
                                   : whyNotLoaded(permissive.error()));

    constexpr char const* kWithField =
        R"({"dssTargetVersion":1,"target":{"name":"Collide"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"m","result":"value","minOperands":1,"maxOperands":1,
               "encoding":{"format":"fixed32","registerClass":"gpr",
                 "variants":[{"guard":{"operandKinds":["reg"]},
                              "template":{"fixedWord":0},
                              "resultSlot":"rd",
                              "wires":[{"index":0,"slotKind":"rn"}]}]}}
            ],
            "registers":[
              {"name":"a","class":"gpr","widthBytes":8,"hwEncoding":31},
              {"name":"b","class":"gpr","widthBytes":8,"hwEncoding":31}
            ]})";
    auto const strict = TargetSchema::loadFromText(kWithField, "<inline>");
    EXPECT_FALSE(strict.has_value())
        << "the SAME colliding pair loaded clean once a register field "
           "existed — the scope is then a hole, and a field could encode "
           "either register with nothing saying which";
    if (!strict.has_value()) {
        auto const why = whyNotLoaded(strict.error());
        EXPECT_NE(why.find("encodingRole"), std::string::npos) << why;
    }
}

TEST(AsmArm64SharedEncoding31, AFieldRoleNoRegisterDeclaresIsRejectedAtLoad) {
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            for (auto& o : doc.at("opcodes")) {
                auto it = o.find("mnemonic");
                if (it == o.end() || it->get<std::string>() != "sp_copy") {
                    continue;
                }
                for (auto& v : o.at("encoding").at("variants")) {
                    v["requiresRegRole"] = "stackpointer";  // wrong case
                }
            }
        });
    EXPECT_FALSE(mutant.has_value())
        << "a field naming a role no register declares loaded clean — the "
           "encoding would be silently unreachable and the author would see an "
           "assembler refusing a line both references accept";
}

// ── RED ON DISABLE: the REMOVE direction, with the controls named ───────────

TEST(AsmArm64SharedEncoding31, RemovingSpCopyFromTheMovRowUnspellsTheSpMoves) {
    // ★ THE MUTATION IS OVER THE **DIALECT**, because that is where the choice
    // of candidate lives. With `sp_copy` gone from `mov`'s opcode list the only
    // candidates are the ORR forms — whose fields read 31 as the zero register
    // — so every SP spelling must be REFUSED. ⚠ It must not silently fall back
    // to the ORR word: that is the original defect, and a fall-back would show
    // up here as bytes rather than as a refusal.
    auto doc = nlohmann::json::parse(dialectText());
    bool touched = false;
    for (auto& row : doc.at("assembly").at("instructions")) {
        auto it = row.find("spelling");
        if (it == row.end() || it->get<std::string>() != "mov") continue;
        row["opcodes"] = nlohmann::json::array({"mov", "move_bytes"});
        touched = true;
    }
    ASSERT_TRUE(touched) << "no `mov` row in the shipped dialect — the mutant "
                            "would be the shipped document and prove nothing";
    auto mutantDialect =
        GrammarSchema::loadFromText(doc.dump(), std::string{kDialect});
    ASSERT_TRUE(mutantDialect.has_value());

    for (auto const* text : {"mov x0, sp\n", "mov sp, x0\n", "mov x30, sp\n",
                             "mov w0, wsp\n", "mov wsp, w0\n"}) {
        auto const r = runOn(*mutantDialect, shippedTarget(), text);
        expectContributedNothing(*r, text);
    }

    // THE CONTROLS, printed by NAME: the same mutant dialect, the same target,
    // the spellings the mutation did not touch. If these went red too, the
    // mutation broke `mov` outright and the refusals above prove nothing.
    constexpr Case kControls[] = {
        {"mov x0, xzr\n", kMovX0FromXzr, "CONTROL `mov x0, xzr` under the "
                                         "mutant dialect"},
        {"mov x0, x1\n",  kMovX0FromX1,  "CONTROL `mov x0, x1` under the "
                                         "mutant dialect"},
        {"mov w0, wzr\n", kMovW0FromWzr, "CONTROL `mov w0, wzr` under the "
                                         "mutant dialect"},
    };
    for (auto const& c : kControls) {
        auto const r = runOn(*mutantDialect, shippedTarget(), c.text);
        EXPECT_TRUE(r->parsed) << c.why << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.why << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.why << " emitted " << hex(r->bytes);
    }
}

TEST(AsmArm64SharedEncoding31, RemovingTheStackPointerRoleFromMemoryBases) {
    // Strip `regRole` from every wire in the document. Every SP-reading field
    // then falls back to the group's DEFAULT reading — the zero register — so
    // every `[sp]` base, every add/sub-immediate on SP and every extended-
    // register form must be refused, while the `xzr` spellings stay green.
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            for (auto& o : doc.at("opcodes")) {
                auto enc = o.find("encoding");
                if (enc == o.end() || !enc->contains("variants")) continue;
                for (auto& v : enc->at("variants")) {
                    v.erase("resultRegRole");
                    auto w = v.find("wires");
                    if (w == v.end()) continue;
                    for (auto& wire : *w) wire.erase("regRole");
                }
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutant target did not load: " << whyNotLoaded(mutant.error());

    for (auto const* text : {"ldr x0, [sp, #8]\n", "str x0, [sp, #8]\n",
                             "add sp, sp, #16\n",  "add x0, sp, #16\n",
                             "sub sp, sp, #16\n",  "cmp sp, #16\n",
                             "add x0, sp, x1\n",   "cmp sp, x0\n",
                             "mov x0, sp\n",       "mov sp, x0\n"}) {
        auto const r = runOn(loadDialect(), *mutant, text);
        expectContributedNothing(*r, text);
    }

    // THE CONTROLS, by name — the ZERO-register readings, which this mutation
    // makes MORE permissive rather than less, and one ordinary line.
    constexpr Case kControls[] = {
        {"mov x0, xzr\n",  kMovX0FromXzr, "CONTROL `mov x0, xzr` under the "
                                          "role-stripped target"},
        {"ldr x0, [x1]\n", kLdrX0X1,      "CONTROL `ldr x0, [x1]` under the "
                                          "role-stripped target"},
        {"add x0, x1, x2\n", kAddX0X1X2,  "CONTROL `add x0, x1, x2` under the "
                                          "role-stripped target"},
    };
    for (auto const& c : kControls) {
        auto const r = runOn(loadDialect(), *mutant, c.text);
        EXPECT_TRUE(r->parsed) << c.why << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.why << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.why << " emitted " << hex(r->bytes);
    }
}

TEST(AsmArm64SharedEncoding31, RemovingRequiresRegRoleMakesTheMovRowAmbiguous) {
    // ★★ `requiresRegRole` IS WHAT KEEPS THE SP ENCODING OUT OF THE ORDINARY
    // CASE. Without it, `mov x0, x1` matches BOTH the ORR form and the ADD
    // form — neither operand carries a role, and a role-less register fits
    // every field — so the election must report an AMBIGUITY rather than pick
    // one. A fix that silently picked would be an order-dependent answer on the
    // commonest line in the dialect.
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            for (auto& o : doc.at("opcodes")) {
                auto it = o.find("mnemonic");
                if (it == o.end() || it->get<std::string>() != "sp_copy") {
                    continue;
                }
                for (auto& v : o.at("encoding").at("variants")) {
                    v.erase("requiresRegRole");
                }
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutant target did not load: " << whyNotLoaded(mutant.error());

    auto const r = runOn(loadDialect(), *mutant, "mov x0, x1\n");
    EXPECT_FALSE(r->ok)
        << "`mov x0, x1` still elected ONE opcode with `requiresRegRole` "
           "removed — the ambiguity check is not seeing `sp_copy`, so the "
           "shipped key is not what keeps the two apart";
    EXPECT_NE(messages(*r).find("sp_copy"), std::string::npos)
        << "the ambiguity report does not name `sp_copy`: " << messages(*r);

    // THE CONTROL, by name: the SP spelling still works under this mutant —
    // the key governs the ORDINARY case, not the SP one.
    auto const control = runOn(loadDialect(), *mutant, "mov x0, sp\n");
    EXPECT_TRUE(control->parsed)
        << "CONTROL `mov x0, sp`: " << messages(*control);
    EXPECT_TRUE(control->ok)
        << "CONTROL `mov x0, sp`: " << messages(*control);
    EXPECT_EQ(firstWord(control->bytes), kMovX0FromSp)
        << "CONTROL `mov x0, sp` must still encode 0x910003E0 under the "
           "mutant: " << hex(control->bytes);
}

TEST(AsmArm64SharedEncoding31, RemovingTheWspRowUnspellsTheWWidthStackPointer) {
    // ⚠ THE MUTANT ALSO STRIPS `wzr`'s ROLE KEYS, and that is a MEASUREMENT the
    // first draft of this arm made by accident: deleting `wsp` alone leaves
    // `wzr` the ONLY row at (gpr, 4 bytes, 31), and `validate()` then refuses
    // the document for declaring a role on a register whose encoding is its own
    // — the rule two tests above. So the honest W-width-free document is the
    // one this repository shipped BEFORE the anchor: no `wsp`, and a `wzr` with
    // no role at all. Anything else would be testing the load rule twice
    // instead of testing the register.
    auto const mutant = mutateShippedTargetSchemaDoc(
        kTarget, [](nlohmann::json& doc) {
            auto& regs = doc.at("registers");
            bool removed = false;
            for (std::size_t i = 0; i < regs.size(); ++i) {
                auto it = regs[i].find("name");
                if (it != regs[i].end() && it->is_string()
                    && it->get<std::string>() == "wsp") {
                    regs.erase(i);
                    removed = true;
                    break;
                }
            }
            EXPECT_TRUE(removed)
                << "the shipped arm64 has no `wsp` row to remove";
            auto* wzr = findRegisterRow(doc, "wzr");
            EXPECT_NE(wzr, nullptr) << "the shipped arm64 has no `wzr` row";
            if (wzr != nullptr) {
                wzr->erase("encodingRole");
                wzr->erase("encodingRoleIsDefault");
            }
        });
    ASSERT_TRUE(mutant.has_value())
        << "the mutant target did not load: " << whyNotLoaded(mutant.error());
    EXPECT_FALSE((*mutant)->registerByName("wsp").has_value());

    for (auto const* text : {"mov w0, wsp\n", "mov wsp, w0\n",
                             "add w0, wsp, w1\n", "cmp wsp, w0\n"}) {
        auto const r = runOn(loadDialect(), *mutant, text);
        expectContributedNothing(*r, text);
    }

    // THE CONTROLS by name: the X-width stack pointer and the W-width ZERO
    // register are untouched by removing `wsp`.
    constexpr Case kControls[] = {
        {"mov x0, sp\n",  kMovX0FromSp,  "CONTROL `mov x0, sp` without wsp"},
        {"mov w0, wzr\n", kMovW0FromWzr, "CONTROL `mov w0, wzr` without wsp"},
    };
    for (auto const& c : kControls) {
        auto const r = runOn(loadDialect(), *mutant, c.text);
        EXPECT_TRUE(r->parsed) << c.why << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.why << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.why << " emitted " << hex(r->bytes);
    }
}

TEST(AsmArm64SharedEncoding31, RemovingTheExtendedRegisterOpcodesRefusesNotFalls) {
    // ⚠ THE POINT OF THIS ARM IS THE **DIRECTION** OF THE FAILURE. With the
    // extended-register opcodes gone, `add x0, sp, x1` must be REFUSED — never
    // fall back to the shifted-register word, which is what DSS did at the P55
    // base and which decodes as `add x0, xzr, x1`.
    auto const mutant = test_support::mutateShippedTargetSchemaJson(
        kTarget, {"add_ext_reg", "sub_ext_reg", "cmp_ext_reg"});
    ASSERT_TRUE(mutant.has_value())
        << "the mutant target did not load: " << whyNotLoaded(mutant.error());

    for (auto const* text : {"add x0, sp, x1\n", "add sp, sp, x1\n",
                             "sub x0, sp, x1\n", "cmp sp, x0\n",
                             "add w0, wsp, w1\n"}) {
        auto const r = runOn(loadDialect(), *mutant, text);
        expectContributedNothing(*r, text);
    }

    constexpr Case kControls[] = {
        {"add x0, x1, x2\n",  kAddX0X1X2,
         "CONTROL `add x0, x1, x2` with the extended opcodes removed"},
        {"add x0, xzr, x1\n", kAddX0XzrX1,
         "CONTROL `add x0, xzr, x1` with the extended opcodes removed"},
        {"cmp x0, x1\n",      kCmpX0X1,
         "CONTROL `cmp x0, x1` with the extended opcodes removed"},
        {"add sp, sp, #16\n", kAddSpSp16,
         "CONTROL `add sp, sp, #16` — the IMMEDIATE form is a different "
         "opcode and this mutation must not have touched it"},
    };
    for (auto const& c : kControls) {
        auto const r = runOn(loadDialect(), *mutant, c.text);
        EXPECT_TRUE(r->parsed) << c.why << "\n" << messages(*r);
        EXPECT_TRUE(r->ok) << c.why << "\n" << messages(*r);
        EXPECT_EQ(firstWord(r->bytes), c.want)
            << c.why << " emitted " << hex(r->bytes);
    }
}
