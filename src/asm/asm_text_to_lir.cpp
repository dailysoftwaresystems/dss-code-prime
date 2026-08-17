#include "asm/asm_text_to_lir.hpp"

#include "asm/asm_variant_elect.hpp"
#include "core/types/assembly_config.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss {

namespace {

// ── the walker's own vocabulary ───────────────────────────────────────────
//
// One decoded assembly operand. The CST rule that produced it is already
// resolved to a ROLE by the dialect's `operandForms`, so nothing below ever
// asks "which rule was that?" — it asks "which role?", which is the property
// a second dialect binds to different rules.
struct DecodedOperand {
    AsmOperandRole role{};
    NodeId         node{};        // for the diagnostic span
    // Register role: the target register ordinal + its class. ★ THE ORDINAL IS
    // ALWAYS THE FULL-WIDTH PARENT'S. A narrow spelling (`%eax`, `w0`) is a
    // `subOf` row in the target's register table; LIR names ONE register and
    // carries the width on the INSTRUCTION, so the sub-register resolves to its
    // parent here and `regWidthBits` remembers what the programmer wrote.
    std::uint16_t  regOrdinal   = 0;
    LirRegClass    regClass     = LirRegClass::None;
    std::uint32_t  regWidthBits = 0;
    std::string    regSpelling;   // as written, for the width diagnostic
    // Immediate / displaced-scalar role: the literal value, when the scalar
    // was a NUMBER. `symbol` is set instead when it was a name.
    std::int64_t   value      = 0;
    bool           hasValue   = false;
    std::string    symbol;        // empty unless the scalar was a name
    // `*%rax` — the dialect's indirect marker. Carried, never dropped: `jmp foo`
    // and `jmp *%rax` are different instructions and losing the star is a
    // miscompile with no diagnostic.
    bool           indirect   = false;
    // Memory role (or a displaced scalar WITH a base): the LIR addressing
    // triple/quad — base [+ index * scale] + displacement.
    bool           isMemory     = false;
    std::uint16_t  baseOrdinal  = 0;
    LirRegClass    baseClass    = LirRegClass::None;
    bool           hasIndex     = false;
    std::uint16_t  indexOrdinal = 0;
    LirRegClass    indexClass   = LirRegClass::None;
    std::uint32_t  scale        = 1;
    std::int32_t   disp         = 0;
};

// A parsed statement: the mnemonic and its operands, in SOURCE order.
struct DecodedInstruction {
    std::string_view            mnemonic;
    NodeId                      node{};
    std::vector<DecodedOperand> operands;
};

// ★★ THE CONTROL-FLOW CLASS IS READ OFF THE TARGET, NEVER OFF THE DIALECT.
// `TargetOpcodeInfo` already states `terminatorKind` (a closed set) and
// `isCall`; a dialect knob restating them could DISAGREE with the target and
// nothing could say which half was right. So the dialect names candidate
// opcodes and the class comes from whichever of them the target declares.
enum class CfClass : std::uint8_t {
    Plain,        // an ordinary value/side-effect instruction
    Return,
    Br,
    CondBr,
    Call,
    IndirectBr,
    Switch,
    Unreachable,
};

[[nodiscard]] CfClass cfClassOf(TargetOpcodeInfo const& info) noexcept {
    switch (info.terminatorKind) {
    case TargetTerminatorKind::Br:          return CfClass::Br;
    case TargetTerminatorKind::CondBr:      return CfClass::CondBr;
    case TargetTerminatorKind::Return:      return CfClass::Return;
    case TargetTerminatorKind::IndirectBr:  return CfClass::IndirectBr;
    case TargetTerminatorKind::Switch:      return CfClass::Switch;
    case TargetTerminatorKind::Unreachable: return CfClass::Unreachable;
    case TargetTerminatorKind::None:        break;
    }
    return info.isCall ? CfClass::Call : CfClass::Plain;
}

// ★★★ DOES THIS OPCODE CONSUME A CONDITION CODE? THE ENCODER ANSWERS, AND IT
// IS THE ONLY THING THAT CAN. `condCodeFromPayload` is the declaration that an
// encoding variant reads `LirInst.payload` as a `TargetCondCode` — x86 ORs the
// nibble into the last opcode byte (`0F 90+cc`), arm64 places it at
// `condBitPos` and optionally inverts it. An opcode with no such variant has
// nowhere to put a condition, and one with such a variant NEEDS one.
//
// ⚠ THE PREVIOUS KEY WAS `terminatorKind == cond-br`, AND IT WAS ONE OPCODE TOO
// NARROW IN BOTH DIRECTIONS. Both shipped targets declare exactly two
// cond-consuming opcodes — `jcc` (a cond-br) and `setcc` (`terminatorKind:
// None`, `result: value`) — so keying on the terminator shape REQUIRED a
// condition where it happened to coincide and REJECTED it on `setcc`, whose
// whole purpose is to materialize a condition. Terminator-ness and
// condition-consumption are two independent facts about an opcode, and only one
// of them is about the condition.
//
// AGNOSTIC: read off the ACTIVE target's own encoding rows — no opcode name, no
// mnemonic list, no arch test. A target that grows `cmovcc` or `csel` is
// covered the moment its variant declares the flag.
[[nodiscard]] bool consumesCondCode(TargetOpcodeInfo const& info) noexcept {
    for (auto const& v : info.encoding.variants) {
        if (v.tmpl.condCodeFromPayload) return true;
    }
    return false;
}

[[nodiscard]] constexpr std::string_view cfClassName(CfClass c) noexcept {
    switch (c) {
    case CfClass::Plain:       return "a plain instruction";
    case CfClass::Return:      return "a return";
    case CfClass::Br:          return "an unconditional branch";
    case CfClass::CondBr:      return "a conditional branch";
    case CfClass::Call:        return "a call";
    case CfClass::IndirectBr:  return "an indirect branch";
    case CfClass::Switch:      return "a multi-way switch";
    case CfClass::Unreachable: return "an unreachable trap";
    }
    return "an unclassified instruction";
}

// ★★★ IS THIS OPCODE REACHED THROUGH A REGISTER RATHER THAN A NAMED BLOCK?
// The TARGET says so, with `terminatorKind: indirect-br` — a closed substrate
// vocabulary entry, not an arch test and not a mnemonic match. It is the target
// half of the two-sided key that lets ONE dialect spelling denote both a direct
// and an indirect branch (D-ASM-ATT-INDIRECT-BRANCH-INEXPRESSIBLE); the dialect
// half is whether the operand carried this dialect's indirect marker.
[[nodiscard]] constexpr bool isIndirectClass(CfClass c) noexcept {
    return c == CfClass::IndirectBr;
}

// One dialect instruction row, resolved against the ACTIVE target. Built once
// per lowering, before any statement is walked, so a dialect/target
// disagreement is reported for the CONFIG rather than for the first `.s` line
// that happened to use it.
struct ResolvedRow {
    // ★★★ TWO CLASSES, NOT ONE, BECAUSE gas SPELLS TWO INSTRUCTIONS THE SAME
    // WAY. `jmp .L1` and `jmp *%rax` are one mnemonic and two DSS opcodes with
    // DIFFERENT terminator kinds (`br` / `indirect-br`), so a single
    // `cfClass` could not hold the row and the loader refused it — "one
    // spelling cannot denote both". That refusal was right about the CAUSE
    // (the two reach different `LirBuilder` terminators) and wrong about the
    // CONCLUSION: the choice is decidable, by the same two-sided shape that
    // split `load` from `store`. The DIALECT says whether the operand carried
    // its indirect marker; the TARGET says which of its opcodes is reached
    // through a register. Neither side needed a new knob.
    //
    // ⚠ A ROW IS ONLY "KIND-SPLIT" WHEN BOTH ARE PRESENT. `call` on x86_64
    // resolves to ONE opcode that encodes both `call foo` and `call *%rax` as
    // variants, so its indirect arm is empty and the indirectness is settled
    // inside `buildCall` by the operand — exactly as before. Candidates INSIDE
    // one arm must still agree, which is what keeps `["mov", "jmp"]` refused.
    std::optional<CfClass>        directClass;
    std::optional<CfClass>        indirectClass;
    std::optional<TargetCondCode> cond;
    // Do this row's resolvable candidates read the instruction payload as a
    // condition code (`consumesCondCode`)? Unanimous by construction — a row
    // whose candidates disagree is refused at resolve time — so the emit walk
    // can put a condition into the payload without re-deciding per election.
    bool                          consumesCond = false;

    [[nodiscard]] bool anyClass() const noexcept {
        return directClass.has_value() || indirectClass.has_value();
    }
};

// Visible (non-EmptySpace) children — the tree-wide indexing convention.
std::vector<NodeId> visibleChildren(Tree const& tree, NodeId parent) {
    std::vector<NodeId> out;
    for (NodeId const c : tree.children(parent)) {
        if (!isEmptySpace(tree.flags(c))) out.push_back(c);
    }
    return out;
}

// The first descendant (self included) whose rule is `rule`, or invalid.
// ⚠ DEPTH-FIRST AND RULE-KEYED, NEVER POSITION-KEYED: the dialect decides how
// deeply its operand production nests, so an index into `children` would be a
// dialect fact living in the engine.
NodeId findDescendantOfRule(Tree const& tree, NodeId n, RuleId rule) {
    // ⚠ AN INVALID RULE IS A ROLE THE DIALECT DECLARED ABSENT (JSON `null`), and
    // it must match NOTHING. Slot 0 of the rule interner is the invalid
    // sentinel, so a bare `.v` comparison would match every node whose rule
    // resolved to slot 0 — an absent role would then silently claim a real
    // production.
    if (!rule.valid()) return NodeId{};
    if (!n.valid()) return NodeId{};
    if (tree.kind(n) == NodeKind::Internal && tree.rule(n).v == rule.v) return n;
    for (NodeId const c : tree.children(n)) {
        if (isEmptySpace(tree.flags(c))) continue;
        if (NodeId const hit = findDescendantOfRule(tree, c, rule); hit.valid()) {
            return hit;
        }
    }
    return NodeId{};
}

// The FIRST / LAST visible TOKEN anywhere under `n`, in document order, or
// invalid. ⚠ DEPTH-FIRST AND NOT DIRECT-CHILDREN-ONLY, AND THAT DISTINCTION IS
// A MEASURED BUG RATHER THAN A PRECAUTION: an operand arrives wrapped in the
// dialect's `{alt}` node, so `visibleChildren(operand)` yields ONE INTERNAL
// child and no tokens at all. A direct-children scan therefore found no leading
// token for `.section .rodata` and refused a line that was perfectly written
// (✔MEASURED through the CLI before this helper existed). How deeply a dialect
// nests its operand production is the dialect's business, which is the same
// reason `findDescendantOfRule` is rule-keyed rather than position-keyed.
NodeId firstVisibleToken(Tree const& tree, NodeId n) {
    if (!n.valid()) return NodeId{};
    if (tree.kind(n) == NodeKind::Token) return n;
    for (NodeId const c : tree.children(n)) {
        if (isEmptySpace(tree.flags(c))) continue;
        if (NodeId const hit = firstVisibleToken(tree, c); hit.valid()) {
            return hit;
        }
    }
    return NodeId{};
}

NodeId lastVisibleToken(Tree const& tree, NodeId n) {
    if (!n.valid()) return NodeId{};
    if (tree.kind(n) == NodeKind::Token) return n;
    NodeId found{};
    for (NodeId const c : tree.children(n)) {
        if (isEmptySpace(tree.flags(c))) continue;
        if (NodeId const hit = lastVisibleToken(tree, c); hit.valid()) {
            found = hit;
        }
    }
    return found;
}

// Every descendant (self included) whose rule is `rule`, in DOCUMENT ORDER.
// Used for the memory operand's register list: a base/index pair is ordered by
// position in every addressing syntax there is, so document order is the one
// dialect-neutral reading of "which register is the base".
void collectDescendantsOfRule(Tree const& tree, NodeId n, RuleId rule,
                              std::vector<NodeId>& out) {
    if (!rule.valid()) return;
    if (!n.valid()) return;
    if (tree.kind(n) == NodeKind::Internal && tree.rule(n).v == rule.v) {
        out.push_back(n);
        return;   // a register never nests another register
    }
    for (NodeId const c : tree.children(n)) {
        if (isEmptySpace(tree.flags(c))) continue;
        collectDescendantsOfRule(tree, c, rule, out);
    }
}

class AsmTextLowering {
public:
    AsmTextLowering(Tree const& tree, GrammarSchema const& grammar,
                    TargetSchema const& target,
                    std::span<std::string const> entryNames,
                    DiagnosticReporter& reporter)
        : tree_(tree), grammar_(grammar), target_(target),
          cfg_(grammar.assembly()), entryNames_(entryNames),
          reporter_(reporter), builder_(target) {}

    std::optional<AsmTextModule> run() {
        // ⚠ NOT AN ASSERT. A driver that routes a non-dialect grammar here is a
        // bug, and the bug must announce itself rather than produce an empty
        // module that links to a program doing nothing.
        if (!cfg_.declared) {
            fail(tree_.root(),
                 std::format("language '{}' has no 'assembly' block, so it "
                             "cannot lower a standalone assembly unit — the "
                             "'encode' pipeline entry was reached for a "
                             "language that declares no instruction vocabulary",
                             grammar_.name()));
            return std::nullopt;
        }

        // PASS 0 — cross-check the DIALECT against the TARGET, once, for every
        // row. See `resolveRows`.
        if (!resolveRows()) return std::nullopt;
        // PASS 1 — directives and labels. Assembly is not a declare-before-use
        // language; a `jmp .Lend` above `.Lend:` is ordinary, and `.globl main`
        // / `.type main, @function` may sit on either side of `main:`.
        if (!scanUnit()) return std::nullopt;
        // PASS 1b — decide which labels open functions and which are blocks.
        if (!classifyLabels()) return std::nullopt;
        // PASS 1c — resolve every symbol-valued DATA slot to the label it
        // names and MINT that label's symbol.
        //
        // ★★★ THE POSITION OF THIS PASS IS LOAD-BEARING AND IT IS NOT A
        // CONVENIENCE. It runs BEFORE the emit walk because
        // `derivableIndirectSuccessors()` reads `LabelInfo::symbol.valid()` as
        // the address-taken predicate, and the emit walk is where an indirect
        // branch consults it. A jump table in `.data` is the ONLY way a `.s`
        // makes a block address-taken without writing an instruction, so
        // resolving these AFTER the walk would leave `br x0` refusing a
        // successor set the file had already stated — the refusal would be
        // true of the pass ORDER rather than of the source.
        if (!bindPendingDataSymbols()) return std::nullopt;
        // PASS 2 — emit.
        if (!emitAll()) return std::nullopt;
        // PASS 3 — the data labels' module symbols. Deferred to here for the
        // one reason `.globl` exists: it may appear AFTER the label it exports,
        // so binding is only decidable once every directive has been seen.
        addDataSymbols();
        // PASS 3b — write each pending data relocation into its item. Deferred
        // to here because a slot naming an INTERIOR label also has to state
        // WHICH LIR BLOCK the symbol is, and blocks are created by the emit
        // walk (`openFunction` reserves a function's blocks when it opens).
        emitPendingDataRelocations();
        if (!ok_) return std::nullopt;

        AsmTextModule out;
        out.lir                 = std::move(builder_).finish();
        out.symbols             = std::move(symbols_);
        out.userEntrySymbol     = userEntry_;
        out.externImports       = std::move(externs_);
        out.dataItems           = std::move(dataItems_);
        out.blockSymbolBindings = std::move(blockSymbolBindings_);
        return out;
    }

private:
    // ── diagnostics ───────────────────────────────────────────────────────
    void fail(NodeId at, std::string message) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::A_AsmTextUnsupported;
        d.severity = DiagnosticSeverity::Error;
        // `actual` is this reporter's free-text channel (see the shared
        // `report()` helper) — the same field every A_* site fills.
        d.actual   = std::move(message);
        if (at.valid()) d.span = tree_.span(at);
        d.buffer = tree_.source().id();
        reporter_.report(std::move(d));
        ok_ = false;
    }

    // ★★ A DIAGNOSTIC THAT DOES NOT REFUSE THE FILE — the one shape this walker
    // needed and did not have. It exists for exactly one situation, and the
    // situation is defined by the reference assembler rather than by taste:
    // where gas ACCEPTS an input (rc=0), does something the wire format forces,
    // and SAYS SO. ✔MEASURED 2026-08-13 — `.bss` + `.space 4, 7` assembles
    // rc=0 with `Warning: ignoring fill value in section '.bss'`.
    // ⇒ Refusing would reject what the reference accepts; accepting silently
    // would drop the programmer's fill with nothing said. Matching gas means
    // matching BOTH halves: the exit code AND the message.
    // ⚠ IT DOES NOT TOUCH `ok_`. A warning that failed the lowering would be an
    // error wearing a different severity, and the harness would report a refusal
    // where gas reports a build.
    void warn(NodeId at, std::string message) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::A_AsmTextUnsupported;
        d.severity = DiagnosticSeverity::Warning;
        d.actual   = std::move(message);
        if (at.valid()) d.span = tree_.span(at);
        d.buffer = tree_.source().id();
        reporter_.report(std::move(d));
    }

    // Every "this pair does not realize that" message ends the same way, and
    // the tail is what makes the diagnostic actionable: it names the two config
    // documents the reader has to open.
    [[nodiscard]] std::string pairSuffix() const {
        return std::format(" (assembly dialect '{}', target '{}')",
                           grammar_.name(), target_.name());
    }

    // ── pass 0: resolve the dialect's rows against the target ─────────────
    //
    // ★★ THE (DIALECT × TARGET) CROSS-CHECKS RUN BEFORE THE FIRST STATEMENT,
    // not when a spelling is first used. A row whose `cond` names a condition
    // this target cannot encode is broken CONFIG; discovering that only when
    // some `.s` happens to write `je` means the same config is "fine" for one
    // input and broken for the next.
    // ⚠ AN UNRESOLVABLE OPCODE NAME IS **NOT** CHECKED HERE, and the asymmetry
    // is deliberate: one dialect document is read for more than one target, and
    // `languageReferences` lets a shared base declare spellings a given CPU
    // does not have. Naming an absent opcode is refused where it is USED
    // (naming the spelling and the target); naming an impossible CONDITION for
    // an opcode the target DOES declare is refused here.
    bool resolveRows() {
        rows_.assign(cfg_.instructions.size(), ResolvedRow{});
        for (std::size_t i = 0; i < cfg_.instructions.size(); ++i) {
            auto const& row = cfg_.instructions[i];
            auto&       out = rows_[i];

            if (!row.condName.empty()) {
                auto const cc = kTargetCondCodeTable.fromName(row.condName);
                if (!cc.has_value()) {
                    fail(NodeId{},
                         std::format("mnemonic '{}' declares condition '{}', "
                                     "which is not one of the substrate's "
                                     "condition codes{}",
                                     row.spelling, row.condName, pairSuffix()));
                    continue;
                }
                if (!target_.condCodeEncoding(*cc).has_value()) {
                    fail(NodeId{},
                         std::format("mnemonic '{}' declares condition '{}', "
                                     "which this target declares no encoding "
                                     "for — a conditional branch on it would "
                                     "reach the encoder and fail there, after "
                                     "the dialect had already claimed to "
                                     "support the spelling{}",
                                     row.spelling, row.condName, pairSuffix()));
                    continue;
                }
                out.cond = *cc;
            }

            // The control-flow class, taken from whichever candidates this
            // target declares — kept in TWO buckets, split by the target's own
            // `terminatorKind: indirect-br`. Candidates INSIDE one bucket that
            // disagree make the row unusable: the operand shape of a branch and
            // of an arithmetic instruction are not the same kind of thing, so
            // there would be no shape to elect over. Candidates ACROSS the two
            // buckets are the `jmp` / `jmp_indirect` pair, decided per
            // instruction by the operand's indirect marker.
            std::array<std::optional<CfClass>, 2> cls{};
            std::array<std::string_view, 2>       clsFrom{};
            bool                                  mixed = false;
            // Condition-consumption, tracked alongside the class for the same
            // reason: it must be UNANIMOUS across the candidates or the row's
            // `cond` would be honoured for one election and silently dropped
            // for another.
            std::optional<bool>    consumes;
            std::string_view       consumesFrom;
            for (auto const& name : row.opcodeNames) {
                auto const ordinal = target_.opcodeByMnemonic(name);
                if (!ordinal) continue;
                auto const* info = target_.opcodeInfo(*ordinal);
                if (info == nullptr) continue;
                bool const consumesHere = consumesCondCode(*info);
                if (!consumes.has_value()) {
                    consumes     = consumesHere;
                    consumesFrom = name;
                } else if (*consumes != consumesHere) {
                    fail(NodeId{},
                         std::format("mnemonic '{}' lists target opcodes '{}' "
                                     "and '{}', only one of which reads a "
                                     "condition code from the instruction "
                                     "payload — whichever way the row declared "
                                     "'cond', one of the two elections would be "
                                     "wrong, and the wrong one is silent (a "
                                     "dropped condition, or a condition "
                                     "defaulted to the target's zero code){}",
                                     row.spelling, consumesFrom, name,
                                     pairSuffix()));
                    mixed = true;
                    break;
                }
                CfClass const     here = cfClassOf(*info);
                std::size_t const arm  = isIndirectClass(here) ? 1u : 0u;
                if (!cls[arm].has_value()) {
                    cls[arm]     = here;
                    clsFrom[arm] = name;
                    continue;
                }
                if (*cls[arm] != here) {
                    fail(NodeId{},
                         std::format("mnemonic '{}' lists target opcodes '{}' "
                                     "and '{}', which are {} and {} — one "
                                     "spelling cannot denote both, because the "
                                     "two take different operand shapes and "
                                     "there would be nothing to elect over. "
                                     "(The one pair that IS decidable is a "
                                     "DIRECT and an INDIRECT branch, told apart "
                                     "by this dialect's indirect operand "
                                     "marker; these two are on the same side of "
                                     "that split.){}",
                                     row.spelling, clsFrom[arm], name,
                                     cfClassName(*cls[arm]), cfClassName(here),
                                     pairSuffix()));
                    mixed = true;
                    break;
                }
            }
            if (mixed) continue;
            out.directClass   = cls[0];
            out.indirectClass = cls[1];
            // ⚠ A KIND-SPLIT ROW IS ONLY DECIDABLE IF THE DIALECT CAN WRITE THE
            // MARKER. `operandForms.indirect` is `null` on a dialect that puts
            // indirectness in the MNEMONIC instead (aarch64 spells the pair `b`
            // / `br`), so such a dialect naming both opcodes on ONE row would
            // leave the engine choosing — every instruction would take the
            // direct arm and `br`'s opcode would be dead config.
            if (out.directClass.has_value() && out.indirectClass.has_value()
                && !cfg_.ruleForRole(AsmOperandRole::Indirect).valid()) {
                fail(NodeId{},
                     std::format("mnemonic '{}' lists both '{}' ({}) and '{}' "
                                 "({}), which this build separates by the "
                                 "operand's INDIRECT marker — but this dialect "
                                 "declares no 'indirect' operand form (the role "
                                 "is bound to null), so no `.s` it reads could "
                                 "ever select the second. Give the two "
                                 "instructions their own spellings, the way a "
                                 "dialect that puts indirectness in the "
                                 "mnemonic already does{}",
                                 row.spelling, clsFrom[0],
                                 cfClassName(*out.directClass), clsFrom[1],
                                 cfClassName(*out.indirectClass),
                                 pairSuffix()));
                continue;
            }

            // ★ `cond` AND THE ENCODER'S CONDITION SLOT PIN EACH OTHER. A row
            // whose opcode reads the payload as a condition but declares none
            // would silently encode payload 0 (whatever the target's
            // condition-zero happens to be); a condition on a row whose opcode
            // reads no payload condition would be silently dropped.
            // ⚠ THE KEY IS `condCodeFromPayload`, NOT `terminatorKind` — see
            // `consumesCondCode`. Keyed on the terminator shape, this pair
            // demanded a condition on `jcc` (right, by coincidence) and refused
            // one on `setcc` (wrong: it is the opcode whose ONLY job is to
            // materialize a condition).
            // ⚠ THE "MISSING cond" HALF IS AN EMIT-TIME CHECK, NOT A LOAD-TIME
            // ONE, AND THE MOVE IS FORCED BY A REAL DIALECT RATHER THAN CHOSEN.
            // ✔MEASURED 2026-08-13: aarch64 writes the condition as an OPERAND
            // (`cset x0, eq`), where AT&T fuses it into the mnemonic (`sete`).
            // The ROW cannot say which — a row with no `cond` is either an
            // operand-carrying spelling or a mistake, and only the INSTRUCTION
            // distinguishes them. Refusing at load would make `cset`
            // inexpressible; defaulting at emit would encode the target's zero
            // condition. So the requirement becomes "the condition arrives from
            // the row OR from an operand", enforced per instruction in
            // `conditionFor` — where BOTH sources are visible and neither can
            // be silently absent. D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED.
            if (consumes.has_value() && !*consumes && !row.condName.empty()) {
                fail(NodeId{},
                     std::format("mnemonic '{}' declares condition '{}', but its "
                                 "target opcode '{}' declares no encoding "
                                 "variant that reads a condition code from the "
                                 "instruction payload — the condition has "
                                 "nowhere to go and would be silently "
                                 "dropped{}",
                                 row.spelling, row.condName, consumesFrom,
                                 pairSuffix()));
                continue;
            }
            // Set LAST, so the flag is true only on a row that passed BOTH
            // checks — which is what makes `consumesCond ⟹ cond.has_value()`
            // an invariant the emit walk can rely on rather than a coincidence
            // of the run aborting first.
            out.consumesCond = consumes.value_or(false);
        }
        return ok_;
    }

    // ★★★ THE CONDITION AN OPERAND NAMES, OR NULLOPT WHEN IT NAMES NONE.
    //
    // ✔MEASURED 2026-08-13: aarch64 gas writes `cset x0, eq` and `csel x0, x1,
    // x2, ne` — the condition is an OPERAND, where AT&T fuses it into the
    // mnemonic (`sete`). No `operandForms` role fits a bare condition name, and
    // MINTING ONE WOULD BREAK EVERY DIALECT: `operandForms` is REQUIRE-ALL, so
    // an eighth role is a load error in every document that does not mention
    // it, and the reuse rule forbids a language-private vocabulary when a
    // substrate one exists.
    //
    // ★★ SO THE CONDITION IS RESOLVED THE WAY A REGISTER ALREADY IS: BY ASKING.
    // A sigil-less operand is a register if `registerByName` says so; a
    // condition name is one if `kTargetCondCodeTable` says so AND this target
    // declares an encoding for it. The lookup is gated on the TARGET fact that
    // the row's opcode reads a payload condition at all, so a label innocently
    // named `eq` in a file full of ordinary branches is never reinterpreted —
    // only an instruction that NEEDS a condition ever looks for one.
    [[nodiscard]] std::optional<TargetCondCode>
    condCodeOfOperand(DecodedOperand const& op) const {
        if (op.isMemory || op.indirect || op.symbol.empty()) return std::nullopt;
        auto const cc = kTargetCondCodeTable.fromName(op.symbol);
        if (!cc.has_value()) return std::nullopt;
        if (!target_.condCodeEncoding(*cc).has_value()) return std::nullopt;
        return cc;
    }

    // The condition this instruction carries, from the ROW (a mnemonic that
    // fuses it) or from an OPERAND (a dialect that writes it separately) —
    // and the index of the operand it consumed, so the shape walk can drop it.
    struct ResolvedCondition {
        std::optional<TargetCondCode> cond;
        std::size_t                   fromOperand = static_cast<std::size_t>(-1);
        bool                          ok          = true;
    };

    [[nodiscard]] ResolvedCondition
    conditionFor(ResolvedRow const& resolved, AsmInstructionSpelling const& row,
                 DecodedInstruction const& ins) {
        ResolvedCondition out;
        if (!resolved.consumesCond) {
            // ⚠ AN OPCODE WITH NO CONDITION SLOT NEVER LOOKS FOR ONE. The
            // resolve-time pair already refused a `cond` on such a row; an
            // operand spelled like a condition here is an ordinary symbol.
            return out;
        }
        if (resolved.cond.has_value()) {
            out.cond = resolved.cond;
            // ⚠ AND THEN AN OPERAND-SPELLED CONDITION IS A REFUSAL, NOT A
            // SECOND OPINION. `sete eq, %rcx` would otherwise let the row's
            // condition win silently while the writer read the operand's.
            for (std::size_t i = 0; i < ins.operands.size(); ++i) {
                if (!condCodeOfOperand(ins.operands[i]).has_value()) continue;
                fail(ins.operands[i].node,
                     std::format("'{}' already fixes its condition in the "
                                 "mnemonic (the dialect row declares '{}'), but "
                                 "operand {} also names a condition — one "
                                 "instruction cannot carry two, and the row's "
                                 "would silently win{}",
                                 ins.mnemonic, row.condName, i + 1,
                                 pairSuffix()));
                out.ok = false;
                return out;
            }
            return out;
        }
        // No row condition: exactly one operand must name it.
        for (std::size_t i = 0; i < ins.operands.size(); ++i) {
            auto const cc = condCodeOfOperand(ins.operands[i]);
            if (!cc.has_value()) continue;
            if (out.cond.has_value()) {
                fail(ins.operands[i].node,
                     std::format("'{}' names more than one condition in its "
                                 "operands ('{}' and '{}'), and its target "
                                 "opcode reads exactly one from the instruction "
                                 "payload{}",
                                 ins.mnemonic,
                                 targetCondCodeName(*out.cond),
                                 targetCondCodeName(*cc), pairSuffix()));
                out.ok = false;
                return out;
            }
            out.cond        = cc;
            out.fromOperand = i;
        }
        if (!out.cond.has_value()) {
            fail(ins.node,
                 std::format("'{}' resolves to a target opcode whose encoding "
                             "reads a condition code from the instruction "
                             "payload, but neither this dialect's row (no "
                             "'cond' key) nor this instruction's operands names "
                             "one — the condition would default to the target's "
                             "zero code and encode the wrong instruction with no "
                             "diagnostic. Either fuse the condition into the "
                             "spelling (one row per condition, as a dialect "
                             "writing 'sete'/'setne' does) or write it as an "
                             "operand (as a dialect writing 'cset x0, eq' "
                             "does){}", ins.mnemonic, pairSuffix()));
            out.ok = false;
        }
        return out;
    }

    // The `LirInst.payload` a row's instructions carry: the resolved condition
    // when the elected opcode's encoder reads one (`condCodeFromPayload`), and
    // 0 otherwise — which is what every non-conditional opcode has always
    // carried.
    //
    // ⚠ NOT `value_or(0)`. Silently encoding the target's zero condition when
    // the invariant broke is exactly the miscompile `conditionFor` exists to
    // prevent — `sete` would become `seto` with no diagnostic.
    [[nodiscard]] std::uint32_t payloadFor(ResolvedRow const& resolved,
                                           ResolvedCondition const& cond,
                                           DecodedInstruction const& ins) {
        if (!resolved.consumesCond) return 0;
        if (!cond.cond.has_value()) {
            fail(ins.node,
                 std::format("internal: '{}' resolves to an opcode that reads a "
                             "condition from the instruction payload, but no "
                             "condition was resolved — the pairing that "
                             "guarantees this did not hold{}",
                             ins.mnemonic, pairSuffix()));
            return 0;
        }
        return static_cast<std::uint32_t>(*cond.cond);
    }

    [[nodiscard]] std::optional<std::size_t>
    rowIndexBySpelling(std::string_view s) const {
        for (std::size_t i = 0; i < cfg_.instructions.size(); ++i) {
            if (cfg_.instructions[i].spelling == s) return i;
        }
        return std::nullopt;
    }

    // ── pass 1: directives + labels ───────────────────────────────────────
    static constexpr std::size_t kNoLabel = static_cast<std::size_t>(-1);

    struct LabelInfo {
        std::string name;
        NodeId      at{};
        bool        isEntry  = false;
        SymbolId    symbol{};              // valid on an entry OR a data label
        std::size_t functionLabel = kNoLabel;  // index of the owning entry
        LirBlockId  block{};
        bool        opened = false;
        // ★ THE THIRD KIND OF LABEL (D-ASM-NO-DATA-DEFINING-DIRECTIVE). A label
        // seen while a DATA section is open names a data ITEM, not a function
        // and not a basic block: it gets a SymbolId and an `AssembledData` slot,
        // and the block model never sees it. Without this a `.s` that defines a
        // string would have its label refused as "appears before any
        // function-entry marker" — a true diagnostic aimed at the wrong thing.
        bool        isData   = false;
        std::size_t dataItem = kNoLabel;   // index into `dataItems_`
    };

    // One `.quad Lw`-style data slot whose value is a symbol's ADDRESS, held
    // between the pass that read it and the passes that can resolve it.
    // D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET.
    struct PendingDataReloc {
        std::size_t    itemIndex  = 0;  // index into `dataItems_`
        std::uint32_t  byteOffset = 0;  // offset of the slot within that item
        std::string    name;            // the label the directive named
        NodeId         at{};            // the operand's span, for the diagnostic
        RelocationKind kind{};          // the target's absolute reloc of this width
        std::string    spelling;        // the directive that wrote it
        std::size_t    labelIndex = kNoLabel;  // resolved in pass 1c
    };

    // ★★★ A DOT-PREFIXED NAME IS A LABEL WHENEVER IT CARRIES A LABEL TAIL.
    // `.L3:` and `.text` are BYTE-IDENTICAL up to the token after the name, so
    // the shared grammar reads both as `asmDirective` and hangs an
    // `{optional asmLabelTail}` slot off it; the presence of that slot IS the
    // answer, read as a RuleId question rather than by inspecting spelling.
    // Returns the defined name (introducer + name, e.g. `.L3`), or empty when
    // the node is a real directive.
    //
    // ★★ THE INTRODUCER'S OWN TEXT IS READ FROM THE TREE, NEVER HARDCODED AS
    // `"."`. `directiveIntroducer` is a dialect-declared TOKEN — both shipped
    // dialects spell it `.`, and a third that spells it `@` or `!` would get a
    // label whose name silently disagreed with every branch that targets it.
    [[nodiscard]] std::string dotLabelName(NodeId directive,
                                           NodeId& labelTail) const {
        labelTail = NodeId{};
        auto const kids = visibleChildren(tree_, directive);
        if (kids.size() < 3) return {};
        NodeId const tail =
            findDescendantOfRule(tree_, kids[2], cfg_.labelTailRule);
        if (!tail.valid()) return {};
        labelTail = tail;
        return std::string{tree_.text(kids[0])} + std::string{tree_.text(kids[1])};
    }

    // The element nested inside a label tail (`Lfoo: ret`, `.L3: .quad 1`), or
    // invalid. One helper for both passes, so the two can never disagree about
    // what a label line contains.
    [[nodiscard]] NodeId elementInLabelTail(NodeId labelTail) const {
        NodeId const element =
            findDescendantOfRule(tree_, labelTail, cfg_.elementRule);
        if (!element.valid()) return NodeId{};
        for (NodeId const arm : visibleChildren(tree_, element)) {
            if (tree_.kind(arm) == NodeKind::Internal) return arm;
        }
        return NodeId{};
    }

    // Record one label definition. Shared by the ordinary `name:` form and the
    // dot-prefixed `.L3:` one, so a dot label is the same kind of thing to
    // every later pass — including the data-section arm.
    bool collectLabel(std::string name, NodeId at) {
        if (labelIndex_.contains(name)) {
            fail(at, std::format("label '{}' is defined more than once", name));
            return false;
        }
        labelIndex_.emplace(name, labels_.size());
        LabelInfo info;
        info.name = std::move(name);
        info.at   = at;
        if (scanSection_.has_value()) {
            // A data label OPENS A NEW ITEM. Every subsequent data-emitting
            // directive appends to it until the next label or section change.
            info.isData   = true;
            info.symbol   = mintSymbol();
            info.dataItem = openDataItem(info.symbol);
        }
        labels_.push_back(std::move(info));
        return true;
    }

    void scanElement(NodeId element) {
        if (!ok_) return;
        if (tree_.rule(element).v == cfg_.directiveRule.v) {
            NodeId      labelTail{};
            std::string const dotted = dotLabelName(element, labelTail);
            if (!dotted.empty()) {
                // D-ASM-DOT-PREFIXED-LABEL-NOT-DEFINED-BY-CONSUMER: the node is
                // an `asmDirective` and the thing it DEFINES is a label. Routing
                // it to the directive vocabulary is what produced `A0008 unknown
                // assembler directive '.L3'` on every `gcc -S` output.
                if (!collectLabel(dotted, element)) return;
                if (NodeId const nested = elementInLabelTail(labelTail);
                    nested.valid()) {
                    scanElement(nested);
                }
                return;
            }
            applyDirective(element);
            return;
        }
        // ⚠ THE CHAIN, NOT JUST THE FIRST LABEL. `a: b: ret` nests a second
        // statement inside the first one's label tail, and `walkElements`
        // only visits LINE-level elements — so collecting one label per
        // line would silently drop every label after the first, and the
        // emit pass would then fail on a label it had never minted.
        NodeId cur = element;
        while (cur.valid() && ok_) {
            NodeId const label = labelOf(cur);
            if (!label.valid()) return;
            if (!collectLabel(std::string{tree_.text(label)}, label)) return;
            // ★ A LABEL CHAIN MAY END IN A DIRECTIVE (`main: .globl main`,
            // `msg: .asciz "hi"`) AND THE DIRECTIVE MUST BE APPLIED HERE.
            // ⚠ THE COMMENT THAT USED TO SIT HERE SAID IT WAS "picked up
            // when the walk reaches it" — measured FALSE: `walkElements`
            // visits LINE-level elements only, and a directive nested in a
            // label tail is never one, so `main: .globl main` silently
            // dropped the export. The same shape now matters far more,
            // because gas writes data on the label's own line.
            // Anchored: D-ASM-DIRECTIVE-AFTER-LABEL-ON-ONE-LINE-DROPPED.
            NodeId const tailDirective = nextDirectiveAfterLabel(cur);
            if (tailDirective.valid()) scanElement(tailDirective);
            cur = nextStatementAfterLabel(cur);
        }
    }

    bool scanUnit() {
        walkElements(tree_.root(), [&](NodeId element) { scanElement(element); });
        return ok_;
    }

    // ── data items ────────────────────────────────────────────────────────
    //
    // ★★★ ONE `AssembledData` PER DATA LABEL, IN THE SECTION THAT WAS OPEN.
    // The row type, the section vocabulary and the linker walkers are the SAME
    // ONES THE C PATH USES (`lowerMirGlobalsToDataItems`) — this walker mints no
    // parallel taxonomy, which is why the directive verbs bind `DataSectionKind`
    // rather than naming sections themselves.
    std::size_t openDataItem(SymbolId symbol) {
        AssembledData item;
        item.symbol  = symbol;
        item.section = *scanSection_;
        dataItems_.push_back(std::move(item));
        openDataItem_ = dataItems_.size() - 1;
        return openDataItem_;
    }

    // The item data lands in right now, opening an ANONYMOUS one if the section
    // was entered without a label. ⚠ `SymbolId{}` is the substrate's declared
    // "anonymous data" marker (`validateAssembledData` exempts it from the
    // duplicate check), so unlabelled data is representable rather than refused
    // — `.rodata` padding and literal blobs legitimately have no name.
    std::size_t currentDataItem() {
        if (openDataItem_ != kNoLabel) return openDataItem_;
        return openDataItem(SymbolId{});
    }

    // ── directives ────────────────────────────────────────────────────────
    void applyDirective(NodeId directive) {
        auto const kids = visibleChildren(tree_, directive);
        // [0] is the introducer token, [1] the name, [2] the optional operands.
        if (kids.size() < 2) {
            fail(directive, "a directive needs a name");
            return;
        }
        std::string const spelling{tree_.text(kids[1])};
        auto const* row = cfg_.directiveBySpelling(spelling);
        if (row == nullptr) {
            fail(kids[1],
                 std::format("unknown assembler directive '.{}' — this build "
                             "refuses an unrecognized directive rather than "
                             "ignoring it, because a silently-dropped directive "
                             "changes the binary with no diagnostic{}",
                             spelling, pairSuffix()));
            return;
        }
        // ★★ A SECTION NAME IS NOT A DIRECTIVE (D-ASM-SECTION-DIRECTIVE-WITH-
        // OPERAND-UNMODELLED). The row exists so `.section rodata` can resolve;
        // writing it bare is what gas itself refuses, and the refusal names the
        // form that works rather than saying "unknown directive" about a
        // spelling the dialect visibly declares.
        if (row->operandOnly) {
            fail(kids[1],
                 std::format("'.{}' is a SECTION NAME in this dialect, not a "
                             "directive — it is reachable only as the operand "
                             "of {}, which is exactly what the reference "
                             "assembler accepts ('.section .{}' assembles; a "
                             "bare '.{}' is an unknown pseudo-op){}",
                             spelling,
                             cfg_.spellingsForVerb(
                                 AsmDirectiveVerb::SectionByName),
                             spelling, spelling, pairSuffix()));
            return;
        }
        switch (row->verb) {
        case AsmDirectiveVerb::SectionText:
        case AsmDirectiveVerb::SectionData:
            applySectionRow(*row, kids[1], spelling);
            return;
        case AsmDirectiveVerb::SectionByName: {
            auto const* named = sectionRowFromOperand(directive, kids, spelling);
            if (named == nullptr) return;
            applySectionRow(*named, kids[1], named->spelling);
            return;
        }
        case AsmDirectiveVerb::EmitData:
            emitDataValues(directive, kids, *row, spelling);
            return;
        case AsmDirectiveVerb::ReserveFillBytes:
            reserveFillBytes(directive, kids, spelling);
            return;
        case AsmDirectiveVerb::GlobalSymbol: {
            if (kids.size() < 3) {
                fail(directive, std::format(".{} needs a symbol name",
                                            spelling));
                return;
            }
            for (NodeId const operand : visibleChildren(tree_, kids[2])) {
                if (tree_.kind(operand) != NodeKind::Internal) continue;
                globals_.insert(std::string{tree_.text(operand)});
            }
            return;
        }
        case AsmDirectiveVerb::FunctionEntry: {
            if (kids.size() < 3) {
                fail(directive,
                     std::format(".{} needs the symbol it marks as a function "
                                 "entry", spelling));
                return;
            }
            auto const operands = visibleChildren(tree_, kids[2]);
            // The FIRST operand names the symbol; the marker (when the dialect
            // declares one) must appear among the rest. ⚠ THE MARKER IS WHAT
            // SEPARATES `.type main, @function` FROM `.type buf, @object` —
            // without it every `.type`d symbol would become a function.
            std::optional<std::string> named;
            bool markerSeen = row->marker.empty();
            for (NodeId const operand : operands) {
                std::string const text{tree_.text(operand)};
                if (!named.has_value()) { named = text; continue; }
                if (!row->marker.empty() && text == row->marker) {
                    markerSeen = true;
                }
            }
            if (!named.has_value()) {
                fail(directive,
                     std::format(".{} needs the symbol it marks as a function "
                                 "entry", spelling));
                return;
            }
            if (markerSeen) functionEntryNames_.insert(*named);
            return;
        }
        case AsmDirectiveVerb::IgnoredAnnotation:
            return;
        }
        fail(kids[1], "unhandled directive verb");
    }

    // ★★★ APPLY ONE SECTION-OPENING ROW — the single place a section change
    // happens in pass 1, whether the row was reached by writing the directive
    // (`.data`) or by naming it (`.section .data`). Two code paths for one
    // effect is how `.section .data` would start differing from `.data`; there
    // is exactly one, so they cannot.
    void applySectionRow(AsmDirectiveSpelling const& row, NodeId at,
                         std::string_view spelling) {
        if (row.verb == AsmDirectiveVerb::SectionText) {
            // Every function this walker emits lands in the text section — LIR
            // has no other — so the verb's whole job is to CLOSE whatever data
            // section was open, which is what makes a `.data … .text …` file
            // put its instructions back in code.
            scanSection_.reset();
            openDataItem_ = kNoLabel;
            return;
        }
        // ★ THE SECTION IS THE DIALECT ROW'S, RESOLVED THROUGH THE ONE SHARED
        // TAXONOMY. The loader already refused an unknown name, so a miss here
        // is a substrate bug and says so rather than defaulting.
        auto const kind = dataSectionKindFromName(row.sectionName);
        if (!kind.has_value()) {
            fail(at,
                 std::format("internal: directive '.{}' declares data section "
                             "'{}', which the substrate's DataSectionKind "
                             "vocabulary does not name — the load-time "
                             "validation that guarantees this did not hold{}",
                             spelling, row.sectionName, pairSuffix()));
            return;
        }
        scanSection_ = *kind;
        // ⚠ THE OPEN ITEM DOES NOT SURVIVE A SECTION CHANGE. An item carries
        // ONE `DataSectionKind`, so bytes written after `.data` cannot append
        // to an item opened under `.rodata` — they would be emitted read-only
        // and the program would fault writing them.
        openDataItem_ = kNoLabel;
    }

    // ★★★ THE SECTION A `SectionByName` DIRECTIVE'S OPERAND NAMES, or nullptr
    // (diagnostic already emitted). `.section .rodata` — the section is the
    // OPERAND, and it resolves against this dialect's OWN section-opening rows,
    // so `.section .data` and `.data` reach the identical row by construction.
    //
    // ★★ THE INTRODUCER IS READ OFF THE TREE, NEVER HARDCODED AS `"."`. kids[0]
    // is this very directive's introducer token, so the check costs nothing and
    // a dialect spelling it `@` or `!` behaves correctly rather than silently
    // comparing against a dot nobody wrote. And the operand MUST carry it:
    // ✔MEASURED 2026-08-13, gas's `.section rodata` (no dot) creates a section
    // literally named `rodata` with NO alloc flag, which is a different section
    // from `.rodata` — accepting the undotted spelling as a synonym would place
    // data somewhere the reference assembler does not.
    [[nodiscard]] AsmDirectiveSpelling const*
    sectionRowFromOperand(NodeId directive, std::vector<NodeId> const& kids,
                          std::string_view spelling) {
        if (kids.size() < 3) {
            fail(directive,
                 std::format("'.{}' needs the section it opens as its operand "
                             "(one of {}){}",
                             spelling, cfg_.sectionOperandSpellings(),
                             pairSuffix()));
            return nullptr;
        }
        std::vector<NodeId> operands;
        for (NodeId const o : visibleChildren(tree_, kids[2])) {
            if (tree_.kind(o) == NodeKind::Internal) operands.push_back(o);
        }
        if (operands.empty()) {
            fail(directive,
                 std::format("'.{}' needs the section it opens as its operand "
                             "(one of {}){}",
                             spelling, cfg_.sectionOperandSpellings(),
                             pairSuffix()));
            return nullptr;
        }
        // ★★ FLAGS AND TYPE ARE REFUSED, NOT IGNORED, AND THIS IS THE DECISION
        // THE ANCHOR ASKED TO BE STATED. Real `.section` carries them —
        // ✔MEASURED 2026-08-13, gas accepts `.section .rodata,"a",@progbits`
        // and `.section .note.GNU-stack,"",@progbits` rc=0. Every one of those
        // operands changes the section's WIRE SEMANTICS (`"aw"` writable,
        // `"ax"` executable, `@nobits` zero-fill), and DSS derives all of them
        // from the `DataSectionKind` instead. So honouring the name while
        // dropping the flags would put `.section .rodata,"aw"` in a read-only
        // section the program then faults writing — a silent accept-and-ignore
        // with a runtime cost. WHAT IS MODELLED: the section NAME. WHAT FAILS
        // LOUD: everything after it.
        if (operands.size() > 1) {
            fail(operands[1],
                 std::format("'.{}' carries {} operands; this build models the "
                             "section NAME and nothing else. The flags/type "
                             "operands ('{}' here) change what the section IS — "
                             "writable, executable, zero-fill — and this build "
                             "derives every one of those from the section kind, "
                             "so honouring the name while dropping them would "
                             "place data somewhere the source did not ask for, "
                             "with no diagnostic{}",
                             spelling, operands.size(),
                             tree_.text(operands[1]), pairSuffix()));
            return nullptr;
        }
        std::string_view const introducer = tree_.text(kids[0]);
        NodeId const lead = firstVisibleToken(tree_, operands[0]);
        if (!lead.valid() || tree_.text(lead) != introducer) {
            fail(operands[0],
                 std::format("'.{}' names its section WITH this dialect's "
                             "directive introducer ('{}'); '{}' does not carry "
                             "it. The reference assembler treats the two as "
                             "DIFFERENT sections — an undotted name creates a "
                             "section of exactly that name with no allocation "
                             "flag — so they are not synonyms{}",
                             spelling, introducer, tree_.text(operands[0]),
                             pairSuffix()));
            return nullptr;
        }
        NodeId const nameTok = lastVisibleToken(tree_, operands[0]);
        std::string_view const name =
            nameTok.valid() ? tree_.text(nameTok) : std::string_view{};
        auto const* named = cfg_.sectionRowByName(name);
        if (named == nullptr) {
            fail(operands[0],
                 std::format("'.{}' names section '{}{}', which this dialect "
                             "does not declare — the sections it can open are "
                             "{}. A section this build cannot place is refused "
                             "by name rather than mapped onto a different one, "
                             "because data in the wrong section is read-only "
                             "where the program writes it, or writable where it "
                             "must not be{}",
                             spelling, introducer, name,
                             cfg_.sectionOperandSpellings(), pairSuffix()));
            return nullptr;
        }
        return named;
    }

    // ★ THE ONE "ARE WE IN DATA?" GATE, shared by both data verbs. Emitting
    // data into the text section would put bytes where LIR puts instructions
    // and the linker would run them.
    [[nodiscard]] bool requireDataSection(NodeId at, std::string_view spelling) {
        if (scanSection_.has_value()) return true;
        // ⚠ THE SUGGESTION LISTS ONLY WHAT A `.s` MAY ACTUALLY WRITE.
        // `spellingsForVerb` excludes `operandOnly` rows precisely so this
        // message never tells a reader to write `.rodata`, which this dialect
        // and gas both refuse; the `.section` route is named separately, with
        // its own operand list, so both doors are stated and neither is wrong.
        auto const direct =
            cfg_.spellingsForVerb(AsmDirectiveVerb::SectionData);
        auto const byName =
            cfg_.spellingsForVerb(AsmDirectiveVerb::SectionByName);
        std::string how;
        if (!direct.empty()) how = std::format("write one of {}", direct);
        if (!byName.empty()) {
            if (!how.empty()) how += ", or ";
            how += std::format("name one after {} (the sections it can open "
                               "are {})",
                               byName, cfg_.sectionOperandSpellings());
        }
        if (how.empty()) {
            how = "this dialect declares none — it needs a directive row with "
                  "verb 'sectionData'";
        }
        fail(at,
             std::format("'.{}' defines data, but no data section is open — "
                         "bytes written here would land in the TEXT section and "
                         "be executed. To open one, {}{}",
                         spelling, how, pairSuffix()));
        return false;
    }

    // `.byte 1,2` / `.long 42` / `.quad -1` — each operand occupies the row's
    // `unitBytes` bytes, little-endian.
    //
    // ★★ THE ELEMENT WIDTH COMES FROM THE ROW, NEVER FROM THE VALUE. `.byte 1`
    // and `.quad 1` are the same value and different bytes; sizing from the
    // value would make a table's stride depend on its contents.
    void emitDataValues(NodeId directive, std::vector<NodeId> const& kids,
                        AsmDirectiveSpelling const& row,
                        std::string_view spelling) {
        if (!requireDataSection(directive, spelling)) return;
        if (isZeroFill(*scanSection_)) {
            fail(directive,
                 std::format("'.{}' writes bytes, but the open section is "
                             "zero-fill ({}) — the wire format reserves its "
                             "size WITHOUT storing file bytes, so the bytes "
                             "would be silently dropped{}",
                             spelling, dataSectionKindName(*scanSection_),
                             pairSuffix()));
            return;
        }
        if (kids.size() < 3) {
            fail(directive,
                 std::format("'.{}' needs at least one value to emit",
                             spelling));
            return;
        }
        std::size_t const itemIdx = currentDataItem();
        auto&             item    = dataItems_[itemIdx];
        // ★ ALIGNMENT IS DERIVED FROM THE WIDEST ELEMENT THE ITEM CARRIES, not
        // declared. A `.quad` table must be 8-aligned for the loads that read
        // it; a `.byte` string needs 1. `.p2align` is an `ignoredAnnotation` in
        // both shipped dialects, so honouring it would be a second, silently
        // conflicting source for the same fact.
        if (auto const a = Alignment::fromBytes(row.unitBytes); a.has_value()) {
            if (a->bytes() > item.alignment.bytes()) item.alignment = *a;
        }
        for (NodeId const operandNode : visibleChildren(tree_, kids[2])) {
            if (tree_.kind(operandNode) != NodeKind::Internal) continue;
            auto decoded = decodeOperand(operandNode);
            if (!decoded) return;
            if (!decoded->hasValue) {
                // ★★★ A SYMBOL-VALUED DATA SLOT IS A RELOCATION, AND THE
                // RELOCATION IS ALL IT IS (D-ASM-INTERIOR-LABELS-NOT-
                // ADDRESSABLE-AT-AN-OFFSET). `.quad Lcase0` writes
                // `unitBytes` ZERO bytes now and asks the linker to write the
                // address later — byte-for-byte the shape a C
                // symbol-address global (`int* p = &x;`) already emits
                // through `lowerMirGlobalsToDataItems`.
                //
                // ⚠ THE NAME CANNOT BE RESOLVED HERE, AND THE REASON IS THE
                // WHOLE PASS STRUCTURE: this runs in PASS 1, where
                // `labelIndex_` is still filling (assembly is not
                // declare-before-use — a jump table above the blocks it
                // names is the NORMAL layout), no label has been classified
                // as code or data, and no LIR block exists. So the slot is
                // RECORDED against the name and resolved in pass 1c.
                if (decoded->symbol.empty()) {
                    fail(decoded->node,
                         std::format("'.{}' takes a value or a symbol name, and "
                                     "this operand is neither{}",
                                     spelling, pairSuffix()));
                    return;
                }
                if (!recordDataRelocation(itemIdx, *decoded, row.unitBytes,
                                          spelling)) {
                    return;
                }
                continue;
            }
            if (!valueFitsUnit(decoded->value, row.unitBytes, decoded->node,
                               spelling)) {
                return;
            }
            appendLittleEndianBytes(
                item.bytes, static_cast<std::uint64_t>(decoded->value),
                row.unitBytes);
        }
    }

    // ★★★ THE TARGET'S ABSOLUTE-ADDRESS RELOCATION OF `widthBytes` BYTES,
    // FOUND BY FORMULA AND NEVER BY NAME. `widthBytes == n && !pcRelative` is
    // the same scan `compile_pipeline.cpp` runs for a C jump table and a
    // symbol-address global, and the same one the linker runs for a cross-CU
    // thunk slot. Matching on the string "abs64" would bind DSS to one
    // target's spelling of a property every target states structurally.
    [[nodiscard]] std::optional<RelocationKind>
    absoluteRelocKind(std::uint32_t widthBytes) const {
        for (auto const& r : target_.relocations()) {
            if (r.widthBytes == widthBytes && !r.pcRelative) return r.kind;
        }
        return std::nullopt;
    }

    // Reserve a symbol-valued slot in data item `itemIdx` and record the
    // relocation that will fill it. Returns false with a diagnostic when the
    // target cannot express an absolute address of this width.
    [[nodiscard]] bool recordDataRelocation(std::size_t             itemIdx,
                                            DecodedOperand const&   operand,
                                            std::uint32_t           unitBytes,
                                            std::string_view        spelling) {
        auto const kind = absoluteRelocKind(unitBytes);
        if (!kind.has_value()) {
            // ⚠ REFUSED, NOT NARROWED TO A DIFFERENT WIDTH. `.long Lw` on a
            // target with only an 8-byte absolute relocation would silently
            // store the low half of an address; a table read through it jumps
            // somewhere that is not the label.
            fail(operand.node,
                 std::format("'.{}' names '{}', which needs an ABSOLUTE "
                             "{}-byte relocation to write that address at link "
                             "time, and this target declares none (its "
                             "relocations are matched by the width/pc-relative "
                             "FORMULA, never by name). Emitting the slot "
                             "without one would store whatever the address "
                             "happened to be at compile time{}",
                             spelling, operand.symbol, unitBytes,
                             pairSuffix()));
            return false;
        }
        auto& item = dataItems_[itemIdx];
        PendingDataReloc pending;
        pending.itemIndex  = itemIdx;
        pending.byteOffset = static_cast<std::uint32_t>(item.bytes.size());
        pending.name       = operand.symbol;
        pending.at         = operand.node;
        pending.kind       = *kind;
        pending.spelling   = std::string{spelling};
        pendingDataRelocs_.push_back(std::move(pending));
        // The slot itself is ZERO bytes of the declared width — the linker
        // writes the address over them.
        item.bytes.insert(item.bytes.end(), unitBytes, std::uint8_t{0});
        return true;
    }

    // Does `v` fit `unitBytes` bytes read either as signed or as unsigned?
    // ⚠ BOTH READINGS ARE ACCEPTED because assembly writes both: `.byte 255`
    // and `.byte -1` are the same byte and gas takes either. What is refused is
    // a value outside BOTH ranges, which would be silently truncated.
    [[nodiscard]] bool valueFitsUnit(std::int64_t v, std::uint32_t unitBytes,
                                     NodeId at, std::string_view spelling) {
        if (unitBytes >= 8) return true;   // every std::int64_t fits 8 bytes
        std::int64_t const  signedMin = -(std::int64_t{1} << (unitBytes * 8 - 1));
        std::uint64_t const unsignedMax =
            (std::uint64_t{1} << (unitBytes * 8)) - 1u;
        if (v >= signedMin && v <= static_cast<std::int64_t>(unsignedMax)) {
            return true;
        }
        fail(at,
             std::format("value {} does not fit the {} byte(s) '.{}' emits — it "
                         "is outside [{}, {}] read as signed and unsigned, so "
                         "encoding it would silently truncate{}",
                         v, unitBytes, spelling, signedMin, unsignedMax,
                         pairSuffix()));
        return false;
    }

    // `.zero 16` / `.space 16, 7` / `.skip 16` — the first operand is a byte
    // COUNT, the OPTIONAL second is the byte to fill with (absent ⇒ zero).
    //
    // ★ ONE VERB, TWO REALIZATIONS, SPLIT BY THE SUBSTRATE'S OWN `isZeroFill`
    // PREDICATE rather than by a section name: a zero-fill section reserves the
    // extent (`reservedSize`, no file bytes — the invariant
    // `validateAssembledData` enforces), and every other section stores real
    // bytes. That is the same chokepoint `AssembledData::sizeInSection` and the
    // walkers use, so a future zero-fill kind lands at one point.
    //
    // ★★★ WHY THE FILL NEEDED NO NEW REPRESENTATION, WHICH IS THE QUESTION
    // D-ASM-SPACE-DIRECTIVE-FILL-BYTE-UNMODELLED ACTUALLY ASKED. `AssembledData`
    // already answers it BOTH ways and the answer is the `isZeroFill` split:
    //   • FILE-BACKED (`Rodata`/`Data`/`Tdata`/`RelRoConst`) — `bytes` is a raw
    //     `std::vector<std::uint8_t>`, so a fill is materialized straight into
    //     it. The old code already wrote `insert(count, 0)` here; the fill is
    //     that same call with the byte parameterised, and NOTHING was added.
    //   • ZERO-FILL (`Bss`/`Tbss`) — `bytes` MUST stay empty (the invariant
    //     `validateAssembledData` enforces as `K_BssDataHasBytes`) and only
    //     `reservedSize` exists. There is no representation for "reserve N bytes
    //     of 0x07" and there cannot be one: the wire format stores no file bytes
    //     for the section at all.
    // ⇒ a non-zero fill in a zero-fill section is not "unimplemented", it is
    // INEXPRESSIBLE — and gas agrees, which settles what to do about it.
    void reserveFillBytes(NodeId directive, std::vector<NodeId> const& kids,
                          std::string_view spelling) {
        if (!requireDataSection(directive, spelling)) return;
        if (kids.size() < 3) {
            fail(directive,
                 std::format("'.{}' needs the number of bytes to reserve",
                             spelling));
            return;
        }
        std::optional<std::int64_t> count;
        std::optional<std::int64_t> fill;
        NodeId                      fillNode{};
        for (NodeId const operandNode : visibleChildren(tree_, kids[2])) {
            if (tree_.kind(operandNode) != NodeKind::Internal) continue;
            auto decoded = decodeOperand(operandNode);
            if (!decoded) return;
            if (fill.has_value()) {
                fail(decoded->node,
                     std::format("'.{}' takes a byte count and an optional fill "
                                 "byte — at most two operands{}",
                                 spelling, pairSuffix()));
                return;
            }
            if (count.has_value()) {
                // ★ THE FILL IS ONE BYTE, CHECKED THROUGH THE SAME CHOKEPOINT
                // `.byte` USES. Reusing `valueFitsUnit` keeps ONE policy for
                // "does this value fit N bytes": both signed and unsigned
                // readings accepted (`.space 4, -1` and `.space 4, 255` are the
                // same byte and gas takes either — ✔MEASURED, `-1` rc=0), and a
                // value outside BOTH refused. ⚠ THAT REFUSAL IS A KNOWN,
                // DELIBERATE DIVERGENCE: ✔MEASURED, gas TRUNCATES `.space 4,
                // 300` with `Warning: value 0x12c truncated to 0x2c` and exits
                // 0. This build refuses instead — the same call this walker
                // already makes for `.byte 300`, so the divergence is one
                // module-wide policy rather than a second opinion invented
                // here, and it errs toward refusing input rather than silently
                // writing a byte the source did not name.
                if (!decoded->hasValue) {
                    fail(decoded->node,
                         std::format("'.{}' needs a numeric fill byte; '{}' is "
                                     "a symbol, and its address is not known "
                                     "when these bytes are laid out{}",
                                     spelling,
                                     decoded->symbol.empty()
                                         ? std::string{"a non-numeric operand"}
                                         : decoded->symbol,
                                     pairSuffix()));
                    return;
                }
                if (!valueFitsUnit(decoded->value, 1, decoded->node, spelling)) {
                    return;
                }
                fill     = decoded->value;
                fillNode = decoded->node;
                continue;
            }
            if (!decoded->hasValue || decoded->value < 0) {
                fail(decoded->node,
                     std::format("'.{}' needs a non-negative byte count{}",
                                 spelling, pairSuffix()));
                return;
            }
            count = decoded->value;
        }
        if (!count.has_value()) {
            fail(directive,
                 std::format("'.{}' needs the number of bytes to reserve",
                             spelling));
            return;
        }
        auto& item = dataItems_[currentDataItem()];
        if (isZeroFill(item.section)) {
            // ★★ A NON-ZERO FILL HAS NOWHERE TO GO HERE, AND THE REFERENCE
            // ASSEMBLER'S ANSWER IS THE ONE THIS BUILD GIVES. ✔MEASURED
            // 2026-08-13: `aarch64-linux-gnu-as` on `.bss` + `.space 4, 7`
            // exits 0 with `Warning: ignoring fill value in section '.bss'`. So
            // gas ACCEPTS the input, DROPS the fill, and SAYS SO — and matching
            // a reference compiler means matching all three
            // ([[feedback_reference_compilers_are_the_spec]], which is
            // bidirectional: refusing what gas accepts is a defect too).
            // ⇒ warn, do not fail. An explicit `0` is not warned about: nothing
            // is being dropped, so there is nothing to say.
            if (fill.has_value() && *fill != 0) {
                warn(fillNode.valid() ? fillNode : directive,
                     std::format("'.{}' names fill byte {}, but the open "
                                 "section is zero-fill ({}) — the wire format "
                                 "reserves its size WITHOUT storing file bytes, "
                                 "so there is nowhere for a non-zero pattern to "
                                 "live and the fill is ignored (the reference "
                                 "assembler does the same, and warns){}",
                                 spelling, *fill,
                                 dataSectionKindName(item.section),
                                 pairSuffix()));
            }
            item.reservedSize += static_cast<std::uint64_t>(*count);
            return;
        }
        item.bytes.insert(item.bytes.end(),
                          static_cast<std::size_t>(*count),
                          static_cast<std::uint8_t>(
                              static_cast<std::uint64_t>(fill.value_or(0))
                              & 0xFFu));
    }

    // ── pass 1b: which labels are functions ───────────────────────────────
    //
    // ★★★ NO FALLBACK GUESS (operator ruling, 2026-08-12). A `.s` with labels
    // and no function-entry marker is REFUSED, naming the dialect, the
    // directive that was expected and every label that could not be placed.
    // The two things this must never do:
    //   * infer an entry from call/branch targets — that reads `.L3` as a
    //     function the moment anything branches to it through a register, and
    //     reads a never-branched-to entry as a block;
    //   * keep one-label-one-function — a `jmp .L3` would then cross a function
    //     boundary, and LIR block references are function-local, so the branch
    //     would resolve to whatever block index happened to sit there.
    bool classifyLabels() {
        std::vector<std::size_t> unclassified;
        std::size_t              currentFunction = kNoLabel;
        for (std::size_t i = 0; i < labels_.size(); ++i) {
            auto& L = labels_[i];
            L.isEntry = functionEntryNames_.contains(L.name);
            // ★ A DATA LABEL IS NEITHER, AND SAYING SO EXPLICITLY IS WHAT KEEPS
            // A `.s` THAT DEFINES A STRING FROM BEING REFUSED. It carries its
            // own SymbolId and its own `AssembledData` slot (minted during the
            // scan, where the open section was known); the block model never
            // sees it, and it does not open, close or continue a function.
            // ⚠ A LABEL MARKED BOTH IS REFUSED rather than resolved: the file
            // says one thing in the section directive and another in the
            // function-entry marker, and picking either would silently ignore
            // half of what was written.
            if (L.isData) {
                if (L.isEntry) {
                    fail(L.at,
                         std::format("label '{}' is marked as a function entry "
                                     "but is defined inside a DATA section — "
                                     "one label cannot be both code and data{}",
                                     L.name, pairSuffix()));
                    return false;
                }
                continue;
            }
            if (L.isEntry) {
                L.symbol = mintSymbol();
                ++functionCount_;
                currentFunction = i;
                L.functionLabel = i;
                continue;
            }
            if (currentFunction == kNoLabel) {
                unclassified.push_back(i);
                continue;
            }
            L.functionLabel = currentFunction;
        }
        if (unclassified.empty()) return ok_;

        std::string names;
        for (auto const i : unclassified) {
            if (!names.empty()) names += ", ";
            names += '\'';
            names += labels_[i].name;
            names += '\'';
        }
        auto const expected =
            cfg_.spellingsForVerb(AsmDirectiveVerb::FunctionEntry);
        fail(labels_[unclassified.front()].at,
             std::format(
                 "{} label(s) appear before any function-entry marker and this "
                 "build cannot place them: {}. A label either OPENS a function "
                 "or is a BLOCK inside the function that is already open, and "
                 "nothing in the source says which these are. {} This build "
                 "does NOT guess: inferring an entry from branch or call "
                 "targets would read an interior label as a function the "
                 "moment anything branched to it indirectly, and treating "
                 "every label as its own function would make an intra-function "
                 "branch cross a function boundary{}",
                 unclassified.size(), names,
                 expected.empty()
                     ? std::format("This dialect declares NO 'functionEntry' "
                                   "directive at all, so no `.s` it reads can "
                                   "define a function; the dialect document "
                                   "needs a directive row with verb "
                                   "'functionEntry'.")
                     : std::format("Mark the function with this dialect's "
                                   "function-entry directive ({}).", expected),
                 pairSuffix()));
        return false;
    }

    // ── pass 2: emit ──────────────────────────────────────────────────────
    bool emitAll() {
        walkElements(tree_.root(), [&](NodeId element) { emitElement(element); });
        if (!ok_) return false;
        closeFunction();
        return ok_;
    }

    void emitElement(NodeId element) {
        {
            if (!ok_) return;
            // Directives were applied in pass 1; re-applying them here would
            // double-report every directive diagnostic. ⚠ THE SECTION STATE IS
            // STILL RE-READ, because pass 2 must know whether an instruction
            // sits in code or in data — and re-reading the ROW (not re-applying
            // the directive) keeps one source of truth with no second report.
            if (tree_.rule(element).v == cfg_.directiveRule.v) {
                NodeId      labelTail{};
                std::string const dotted = dotLabelName(element, labelTail);
                if (!dotted.empty()) {
                    // A dot-prefixed LABEL, not a directive - see
                    // `dotLabelName`. It enters the block model exactly as
                    // `Lfoo:` does, which is what makes `jmp .L3` reach a real
                    // LirBlockId once the dialect can spell the operand.
                    enterLabel(dotted, element);
                    if (!ok_) return;
                    if (NodeId const nested = elementInLabelTail(labelTail);
                        nested.valid()) {
                        emitElement(nested);
                    }
                    return;
                }
                trackSection(element);
                return;
            }
            // A statement: a label definition, an instruction, or a label
            // followed on the same line by one.
            NodeId cur = element;
            while (cur.valid() && ok_) {
                auto const kids = visibleChildren(tree_, cur);
                if (kids.empty()) return;
                NodeId const name = kids.front();
                // ⚠ THE TAIL IS THE DIALECT'S ALT WRAPPER, NOT THE ARM. The
                // shared grammar names the wrapper `asmStatementTail` and the
                // arms `asmLabelTail` / `asmOperandSeq`; comparing the wrapper's
                // rule against an ARM's landmark never matches, and the first
                // version of this walker did exactly that — `main:` was then
                // read as a zero-operand instruction and refused as "unknown
                // mnemonic 'main'", a true diagnostic aimed at the wrong thing.
                // Descend by RULE so the wrapper's depth stays the dialect's
                // business.
                NodeId const rawTail = kids.size() > 1 ? kids[1] : NodeId{};
                NodeId const tail =
                    findDescendantOfRule(tree_, rawTail, cfg_.labelTailRule);
                if (tail.valid()) {
                    enterLabel(std::string{tree_.text(name)}, name);
                    if (!ok_) return;
                    // The tail may carry another element on the same line: a
                    // STATEMENT continues this loop's label chain, a DIRECTIVE
                    // is handed back to `emitElement` - which is also what
                    // routes a nested dot-LABEL (`Lfoo: .L3: ret`) and what
                    // replays a `main: .text` line's SECTION effect, instead of
                    // dropping either.
                    NodeId const nested = elementInLabelTail(tail);
                    cur = NodeId{};
                    if (nested.valid()) {
                        if (tree_.rule(nested).v == cfg_.statementRule.v) {
                            cur = nested;
                        } else {
                            emitElement(nested);
                            return;
                        }
                    }
                    continue;
                }
                emitInstruction(cur, name,
                                findDescendantOfRule(tree_, rawTail,
                                                     cfg_.operandSeqRule));
                return;
            }
        }
    }

    // Replay a directive's SECTION effect during pass 2. ★ IT READS THE SAME
    // ROW `applyDirective` reads and does nothing else — an unknown spelling
    // was already refused in pass 1, so silence here is the absence of a SECOND
    // report rather than a second policy.
    void trackSection(NodeId directive) {
        auto const kids = visibleChildren(tree_, directive);
        if (kids.size() < 2) return;
        auto const* row = cfg_.directiveBySpelling(std::string{tree_.text(kids[1])});
        if (row == nullptr || row->operandOnly) return;
        // ★ A `SectionByName` DIRECTIVE IS RESOLVED TO ITS NAMED ROW FIRST, so
        // pass 2 tracks `.section .data` exactly as it tracks `.data`. ⚠ THIS
        // WALK IS SILENT ON FAILURE BY DESIGN — pass 1 already reported every
        // way the operand can be wrong, and re-reporting here would double
        // every `.section` diagnostic. `sectionRowFromOperand` cannot be reused
        // for that reason; the lookup is repeated WITHOUT its diagnostics.
        if (row->verb == AsmDirectiveVerb::SectionByName) {
            if (kids.size() < 3) return;
            AsmDirectiveSpelling const* named = nullptr;
            for (NodeId const o : visibleChildren(tree_, kids[2])) {
                if (tree_.kind(o) != NodeKind::Internal) continue;
                NodeId const t = lastVisibleToken(tree_, o);
                if (t.valid()) named = cfg_.sectionRowByName(tree_.text(t));
                break;
            }
            if (named == nullptr) return;
            row = named;
        }
        if (row->verb == AsmDirectiveVerb::SectionText) {
            emitSection_.reset();
        } else if (row->verb == AsmDirectiveVerb::SectionData) {
            emitSection_ = dataSectionKindFromName(row->sectionName);
        }
    }

    // ── functions and blocks ──────────────────────────────────────────────
    void enterLabel(std::string const& name, NodeId at) {
        auto const it = labelIndex_.find(name);
        if (it == labelIndex_.end()) {
            fail(at, std::format("label '{}' was not collected", name));
            return;
        }
        auto& L = labels_[it->second];
        // A data label named its `AssembledData` item during the scan; there is
        // nothing for the block model to do with it.
        if (L.isData) return;
        if (L.isEntry) {
            openFunction(it->second);
            return;
        }
        enterBlock(it->second, at);
    }

    void openFunction(std::size_t labelIdx) {
        closeFunction();
        if (!ok_) return;
        auto& entry = labels_[labelIdx];
        builder_.addFunction(entry.symbol);
        openFunctionLabel_ = labelIdx;
        // ★ EVERY BLOCK OF THIS FUNCTION IS CREATED UP FRONT, IN LABEL ORDER.
        // A forward branch (`jmp .Lend` above `.Lend:`) needs the target's
        // LirBlockId before the label is reached, and `createBlock` call order
        // IS block order — there is no layout pass to reorder them afterwards.
        entry.block = builder_.createBlock();
        for (std::size_t i = labelIdx + 1; i < labels_.size(); ++i) {
            // ⚠ A DATA LABEL DOES NOT END THE FUNCTION'S BLOCK RUN. A `.s` may
            // interleave `.data`/`.text`, and stopping at the first data label
            // would leave every later block of this function with an INVALID
            // LirBlockId — which `beginBlock` turns into a process abort rather
            // than a diagnostic.
            if (labels_[i].isData) continue;
            if (labels_[i].functionLabel != labelIdx) break;
            labels_[i].block = builder_.createBlock();
        }
        builder_.beginBlock(entry.block);
        entry.opened     = true;
        openTerminated_  = false;
        blockInstCount_  = 0;
        openBlockLabel_  = labelIdx;
    }

    void enterBlock(std::size_t labelIdx, NodeId at) {
        if (openFunctionLabel_ == kNoLabel) {
            // classifyLabels already refused this shape; keep the guard so a
            // future caller cannot reach `createBlock`'s process abort.
            fail(at, std::format("label '{}' has no open function",
                                 labels_[labelIdx].name));
            return;
        }
        // ⚠ THE `beginBlock` GUARD, RAISED TO A DIAGNOSTIC. `LirBuilder`
        // ABORTS THE PROCESS when a block is opened while its predecessor has
        // no terminator, and a process abort is not fail-loud: it prints no
        // span, names no file and leaves the caller no verdict.
        if (!openTerminated_) {
            if (!synthesizeFallthrough(labelIdx, at)) return;
        }
        builder_.beginBlock(labels_[labelIdx].block);
        labels_[labelIdx].opened = true;
        openTerminated_          = false;
        blockInstCount_          = 0;
        openBlockLabel_          = labelIdx;
    }

    // ★★ FALLING INTO A LABEL IS DEFINED, NOT AMBIGUOUS — SO IT IS REALIZED,
    // NOT GUESSED AT. Every assembler defines "control reaching the end of a
    // run of instructions continues into the next label"; LIR simply cannot
    // represent an unterminated block, so the edge has to be written down.
    // That is NOT the same kind of act as inventing a `ret` for a function
    // that falls off its END (where there is no next block and the two
    // candidate meanings — return or trap — are both a claim about intent).
    //
    // The branch opcode is the TARGET's, found by asking which of its opcodes
    // is an unconditional branch over a single block reference. ⚠ AMBIGUITY IS
    // REFUSED: two such opcodes mean the engine would be picking, and a target
    // with none cannot express the edge at all. Either way the answer is a
    // diagnostic, never `LirBuilder`'s abort.
    bool synthesizeFallthrough(std::size_t targetLabel, NodeId at) {
        auto const br = unconditionalBranchOpcode();
        std::string const from =
            openBlockLabel_ == kNoLabel ? std::string{"<function entry>"}
                                        : labels_[openBlockLabel_].name;
        if (!br.has_value()) {
            fail(at,
                 std::format("control falls out of '{}' into label '{}', and "
                             "this build cannot write that edge down: {}. LIR "
                             "requires every block to end in a terminator, so "
                             "the fallthrough must become an explicit branch — "
                             "either declare the target's unconditional-branch "
                             "opcode unambiguously, or end '{}' with an "
                             "explicit jump{}",
                             from, labels_[targetLabel].name, branchProbeNote_,
                             from, pairSuffix()));
            return false;
        }
        // ⓘ ADJACENT LABELS (`blockInstCount_ == 0`) NEED NO SPECIAL ARM. The
        // synthesized branch is itself an instruction, so the block is filled
        // AND terminated — `.L1:` immediately followed by `.L2:` becomes a
        // block that jumps to the next, which is exactly what two labels at one
        // address mean. Without the synthesis this was `LirBuilder`'s "block
        // opened but never filled" process abort.
        builder_.addBr(*br, labels_[targetLabel].block);
        openTerminated_ = true;
        ++blockInstCount_;
        return true;
    }

    // Which target opcode is "the" unconditional branch. Cached: the scan is
    // over the whole opcode table and a function with many labels would
    // otherwise repeat it per label.
    std::optional<std::uint16_t> unconditionalBranchOpcode() {
        if (branchProbed_) return branchOpcode_;
        branchProbed_ = true;
        std::string found;
        for (std::uint16_t op = 0; op < target_.opcodeCount(); ++op) {
            auto const* info = target_.opcodeInfo(op);
            if (info == nullptr) continue;
            if (info->terminatorKind != TargetTerminatorKind::Br) continue;
            if (!found.empty()) {
                branchOpcode_.reset();
                branchProbeNote_ = std::format(
                    "target opcodes '{}' and '{}' are BOTH unconditional "
                    "branches, so this build will not pick one", found,
                    info->mnemonic);
                return branchOpcode_;
            }
            found         = info->mnemonic;
            branchOpcode_ = op;
        }
        if (!branchOpcode_.has_value()) {
            branchProbeNote_ =
                "this target declares no opcode whose terminatorKind is 'br'";
        }
        return branchOpcode_;
    }

    void closeFunction() {
        if (openFunctionLabel_ == kNoLabel) return;
        auto const& entry = labels_[openFunctionLabel_];
        if (!openTerminated_) {
            // ★ A FUNCTION THAT FALLS OFF ITS END IS REFUSED, NOT PADDED. LIR
            // requires every block to be terminated, and the two ways to
            // satisfy it silently — appending a `ret` or an `unreachable` —
            // are each a decision about what the programmer meant. `ret` on a
            // function meant to fall through into the next label produces a
            // program that returns instead of continuing; `unreachable` turns
            // it into a trap. Both are miscompiles of intent. (Falling into a
            // LABEL is a different case and IS realized — see
            // `synthesizeFallthrough`; here there is no next block.)
            fail(entry.at,
                 std::format("assembly function '{}' has no terminating "
                             "instruction — {} must end in a terminator (e.g. "
                             "`ret`); falling off the end would need this build "
                             "to invent one{}",
                             entry.name,
                             blockInstCount_ == 0
                                 ? std::format("its final block (label '{}') is "
                                               "empty and every block",
                                               openBlockLabel_ == kNoLabel
                                                   ? entry.name
                                                   : labels_[openBlockLabel_].name)
                                 : std::string{"every block"},
                             pairSuffix()));
        }
        // ⚠ `LirBuilder::closeFunction` ABORTS on a block it created and never
        // opened. That can only happen if a label of this function was never
        // reached by the emit walk, which would be an engine bug rather than a
        // source defect — surface it as a diagnostic naming the label instead
        // of killing the process with no span.
        for (std::size_t i = openFunctionLabel_; i < labels_.size(); ++i) {
            if (labels_[i].isData) continue;   // never reserved a block
            if (i != openFunctionLabel_
                && labels_[i].functionLabel != openFunctionLabel_) break;
            if (labels_[i].opened) continue;
            fail(labels_[i].at,
                 std::format("label '{}' reserved a basic block that the "
                             "lowering never reached — the emit walk and the "
                             "label scan disagree about this file's structure{}",
                             labels_[i].name, pairSuffix()));
        }

        ModuleSymbol sym;
        sym.symbol     = entry.symbol;
        sym.name       = entry.name;
        sym.binding    = globals_.contains(entry.name)
                             ? SymbolBinding::Global
                             : SymbolBinding::Local;
        sym.visibility = SymbolVisibility::Default;
        // ★ THE ENTRY MUST BE EXPORTED, AND THAT IS NOW ENFORCED RATHER THAN
        // ONLY DOCUMENTED. The previous code elected on NAME alone while its
        // header comment promised a `.globl`-exported label; the two disagreed,
        // and the disagreement had a cost — a `main:` without `.globl` would be
        // elected here and then emitted with LOCAL linkage, which is exactly
        // the "entry symbol is local, link failure points nowhere near the
        // cause" failure the dialect's own `.globl` note warns about. Refusing
        // is the only arm with no silent path: leaving `userEntrySymbol` unset
        // instead would send the trampoline to whatever landed at functions[0].
        if (std::ranges::find(entryNames_, entry.name) != entryNames_.end()) {
            if (sym.binding != SymbolBinding::Global) {
                fail(entry.at,
                     std::format("label '{}' is one of this build's "
                                 "program-entry names but is not exported — an "
                                 "entry symbol with local linkage is invisible "
                                 "to the linker's entry resolution. Add this "
                                 "dialect's global-symbol directive ({}) for "
                                 "it{}",
                                 entry.name,
                                 cfg_.spellingsForVerb(
                                     AsmDirectiveVerb::GlobalSymbol),
                                 pairSuffix()));
            } else if (!userEntry_.has_value()) {
                userEntry_ = entry.symbol;
            }
        }
        symbols_.push_back(std::move(sym));
        openFunctionLabel_ = kNoLabel;
        openBlockLabel_    = kNoLabel;
    }

    // ── instructions ──────────────────────────────────────────────────────
    void emitInstruction(NodeId statement, NodeId mnemonicNode, NodeId tail) {
        std::string_view const spelling = tree_.text(mnemonicNode);
        if (emitSection_.has_value()) {
            fail(mnemonicNode,
                 std::format("instruction '{}' appears while the '{}' DATA "
                             "section is open — LIR places code in the text "
                             "section only, so this instruction would be "
                             "emitted as if the data section were code{}",
                             spelling, dataSectionKindName(*emitSection_),
                             pairSuffix()));
            return;
        }
        auto const rowIdx = rowIndexBySpelling(spelling);
        if (!rowIdx.has_value()) {
            fail(mnemonicNode,
                 std::format("unknown mnemonic '{}' — it is not in this "
                             "dialect's instruction table. An undeclared "
                             "spelling is refused rather than guessed at: "
                             "adding it is a config edit, and reusing a "
                             "similarly-named target opcode would silently "
                             "encode a different instruction{}",
                             spelling, pairSuffix()));
            return;
        }
        auto const& row      = cfg_.instructions[*rowIdx];
        auto const& resolved = rows_[*rowIdx];
        if (!resolved.anyClass()) {
            std::string names;
            for (auto const& n : row.opcodeNames) {
                if (!names.empty()) names += ", ";
                names += '\'';
                names += n;
                names += '\'';
            }
            fail(mnemonicNode,
                 std::format("this dialect maps '{}' to target opcode(s) {}, "
                             "none of which this target declares — the dialect "
                             "and the target disagree about the instruction "
                             "set{}", spelling, names, pairSuffix()));
            return;
        }
        if (openFunctionLabel_ == kNoLabel) {
            fail(mnemonicNode,
                 std::format("instruction '{}' appears before any function "
                             "entry — there is no function for it to belong "
                             "to{}", spelling, pairSuffix()));
            return;
        }
        if (openTerminated_) {
            fail(mnemonicNode,
                 std::format("instruction '{}' follows a terminator with no "
                             "intervening label, so it is unreachable — this "
                             "build refuses to emit code it cannot place in a "
                             "basic block{}", spelling, pairSuffix()));
            return;
        }

        DecodedInstruction ins;
        ins.mnemonic = spelling;
        ins.node     = statement;
        if (tail.valid()) {   // already resolved to the operand-seq node
            for (NodeId const operandNode : visibleChildren(tree_, tail)) {
                if (tree_.kind(operandNode) != NodeKind::Internal) continue;
                auto decoded = decodeOperand(operandNode);
                if (!decoded) return;
                ins.operands.push_back(std::move(*decoded));
            }
        }
        buildLirInst(ins, row, resolved);
    }

    // The NAME text a node spells: the LAST visible token, because whatever
    // leads it is a SIGIL and carries no identity — `%` before a register,
    // `@`/`%` before a type marker, the directive introducer before a section
    // name. Shared by the role disambiguation, by `decodeRegister` and by the
    // `SectionByName` operand read, so no two of them can disagree about which
    // token is the name.
    [[nodiscard]] std::string_view trailingNameOf(NodeId node) const {
        std::string_view name;
        for (NodeId const k : visibleChildren(tree_, node)) {
            if (tree_.kind(k) == NodeKind::Token) name = tree_.text(k);
        }
        if (name.empty() && tree_.kind(node) == NodeKind::Token) {
            name = tree_.text(node);
        }
        return name;
    }

    // ★★★ WHICH ROLE A SHARED RULE REALIZES, DECIDED BY ASKING THE TARGET.
    // ✔MEASURED 2026-08-13: aarch64 gas has no register sigil, so `x0`,
    // `helper` and `Lend` are the SAME TOKEN — `mov x0, x1`, `bl helper` and
    // `b Lend` cannot be told apart by any grammar. The dialect says so by
    // binding `register` AND a symbol-ish role to one rule, and the LOOKUP
    // settles each operand: a spelling the target declares as a register IS a
    // register; anything else is the other role. That is exactly what gas does.
    // ⚠ THE PAIRING IS ONLY DECIDABLE BECAUSE ONE SIDE IS `register` — the
    // loader refuses two NON-register roles on one rule, where nothing could
    // decide.
    [[nodiscard]] AsmOperandRole resolveRole(NodeId node,
                                             std::uint8_t mask) const {
        if (AssemblyConfig::maskHas(mask, AsmOperandRole::Register)) {
            auto const name = trailingNameOf(node);
            if (!name.empty() && target_.registerByName(name).has_value()) {
                return AsmOperandRole::Register;
            }
        }
        for (std::size_t i = 0; i < kAsmOperandRoleCount; ++i) {
            auto const role = static_cast<AsmOperandRole>(i);
            if (role == AsmOperandRole::Register) continue;
            if (AssemblyConfig::maskHas(mask, role)) return role;
        }
        return AsmOperandRole::Register;
    }

    // Decode one operand node into a role + payload.
    std::optional<DecodedOperand> decodeOperand(NodeId node) {
        // The bound `attOperand`-style alt node wraps the chosen form; descend
        // to the first node whose rule the dialect bound to a role.
        NodeId cur = node;
        std::uint8_t mask = cfg_.rolesForRule(tree_.rule(cur));
        while (mask == 0) {
            auto const kids = visibleChildren(tree_, cur);
            NodeId next{};
            for (NodeId const k : kids) {
                if (tree_.kind(k) == NodeKind::Internal) { next = k; break; }
            }
            if (!next.valid()) {
                fail(node, std::format("operand shape is not one this dialect "
                                       "binds to an operand role{}",
                                       pairSuffix()));
                return std::nullopt;
            }
            cur  = next;
            mask = cfg_.rolesForRule(tree_.rule(cur));
        }
        AsmOperandRole const role = resolveRole(cur, mask);

        DecodedOperand out;
        out.role = role;
        out.node = cur;
        switch (role) {
        case AsmOperandRole::Register:
            return decodeRegister(cur, std::move(out));
        case AsmOperandRole::Immediate: {
            NodeId const scalar =
                findDescendantOfRule(tree_, cur,
                                     cfg_.ruleForRole(AsmOperandRole::Scalar));
            if (!decodeScalar(scalar.valid() ? scalar : cur, out)) {
                return std::nullopt;
            }
            return out;
        }
        case AsmOperandRole::Displaced: {
            // A displaced operand with a memory base is a MEMORY reference; one
            // without is a bare scalar. The distinction is a RuleId question,
            // exactly as the dialect's own comment says.
            NodeId const base =
                findDescendantOfRule(tree_, cur,
                                     cfg_.ruleForRole(AsmOperandRole::Memory));
            NodeId const scalar =
                findDescendantOfRule(tree_, cur,
                                     cfg_.ruleForRole(AsmOperandRole::Scalar));
            if (base.valid()) {
                if (scalar.valid() && !decodeScalar(scalar, out)) {
                    return std::nullopt;
                }
                if (!out.symbol.empty()) {
                    fail(cur,
                         std::format("displacement '{}' is a symbol, and a "
                                     "symbol-relative memory operand needs a "
                                     "relocation this build does not reach from "
                                     "assembly yet{}", out.symbol,
                                     pairSuffix()));
                    return std::nullopt;
                }
                if (!fitsDisp(out.value, cur)) return std::nullopt;
                std::int32_t const disp =
                    static_cast<std::int32_t>(out.value);
                out.value    = 0;
                out.hasValue = false;
                if (!decodeMemory(base, out)) return std::nullopt;
                // ⚠ TWO DISPLACEMENTS ARE REFUSED, NOT MERGED. A dialect that
                // writes one OUTSIDE the memory form and nests another INSIDE
                // it has said the address twice, and picking either (or adding
                // them) would be this build deciding which one the programmer
                // meant. Neither shipped dialect can express both; the refusal
                // exists so a third one cannot do it silently.
                if (out.disp != 0 && disp != 0) {
                    fail(cur,
                         std::format("this operand carries TWO displacements "
                                     "({} outside the memory form and {} inside "
                                     "it) and LIR addresses model exactly one{}",
                                     disp, out.disp, pairSuffix()));
                    return std::nullopt;
                }
                if (disp != 0) out.disp = disp;
                return out;
            }
            if (!decodeScalar(scalar.valid() ? scalar : cur, out)) {
                return std::nullopt;
            }
            return out;
        }
        case AsmOperandRole::Memory:
            if (!decodeMemory(cur, out)) return std::nullopt;
            return out;
        case AsmOperandRole::Indirect: {
            // `*%rax` wraps an ordinary operand. Decode the inner form and
            // carry the marker; the control-flow arms below consume it.
            NodeId inner{};
            for (NodeId const k : visibleChildren(tree_, cur)) {
                if (tree_.kind(k) == NodeKind::Internal) { inner = k; break; }
            }
            if (!inner.valid()) {
                fail(cur, std::format("an indirect operand needs a target{}",
                                      pairSuffix()));
                return std::nullopt;
            }
            auto decoded = decodeOperand(inner);
            if (!decoded) return std::nullopt;
            decoded->indirect = true;
            decoded->node     = cur;
            return decoded;
        }
        case AsmOperandRole::Scalar:
        case AsmOperandRole::NegNumber:
            if (!decodeScalar(cur, out)) return std::nullopt;
            return out;
        }
        fail(cur, "unhandled operand role");
        return std::nullopt;
    }

    // ★★ THE OPERATION WIDTH THE DATA REGISTERS AGREE ON, or nullopt when the
    // instruction names none. THIS IS THE HALF A SUFFIX-LESS DIALECT NEEDS:
    // aarch64 writes `add x0,x1,x2` and `add w0,w1,w2` with ONE mnemonic, so the
    // registers are the only thing that says which width was meant.
    // ⚠ MEMORY BASE/INDEX REGISTERS ARE EXCLUDED BY CONSTRUCTION — they live
    // inside a `isMemory` operand and never appear as a Register-role operand.
    // `movl (%rdi),%eax` is legal precisely because the ADDRESS width and the
    // OPERATION width are different questions. An INDIRECT operand (`call
    // *%rax`) is excluded for the same reason.
    // ⚠ AND DISAGREEMENT IS REFUSED IN BOTH DIRECTIONS. gas rejects `movl
    // %rax,%ecx` AND `movl %eax,%rcx`; a check that looked only at the
    // destination would accept one of them and silently encode the other
    // instruction.
    std::optional<std::uint32_t> dataRegisterWidth(DecodedInstruction const& ins) {
        std::optional<std::uint32_t> width;
        DecodedOperand const*        first = nullptr;
        for (auto const& op : ins.operands) {
            if (op.role != AsmOperandRole::Register) continue;
            if (op.isMemory || op.indirect) continue;
            if (!width.has_value()) { width = op.regWidthBits; first = &op; continue; }
            if (*width == op.regWidthBits) continue;
            fail(op.node,
                 std::format("register '{}' is {} bits wide but register '{}' is "
                             "{} bits — one instruction cannot operate on both, "
                             "and encoding either width would silently be the "
                             "other instruction{}",
                             first->regSpelling, *width, op.regSpelling,
                             op.regWidthBits, pairSuffix()));
            return std::nullopt;
        }
        return width;
    }

    // The width this instruction actually operates on, reconciling what the
    // dialect DECLARED (a mnemonic suffix) with what the operands SAY (register
    // widths). Either source may be absent; when both are present they must
    // agree.
    std::optional<std::uint32_t> effectiveWidth(DecodedInstruction const& ins,
                                                AsmInstructionSpelling const& row,
                                                bool consultOperands) {
        std::optional<std::uint32_t> derived;
        if (consultOperands) {
            derived = dataRegisterWidth(ins);
            if (!ok_) return std::nullopt;
        }
        if (row.width.has_value() && derived.has_value()) {
            if (*row.width != *derived) {
                fail(ins.node,
                     std::format("'{}' declares operand width {}, but its "
                                 "register operands are {} bits — the spelling "
                                 "and the registers disagree, and encoding "
                                 "either one would silently be the other "
                                 "instruction{}", ins.mnemonic, *row.width,
                                 *derived, pairSuffix()));
                return std::nullopt;
            }
            return derived;
        }
        if (row.width.has_value()) return row.width;
        if (derived.has_value())   return derived;
        // Neither says: an instruction with no register operands and no declared
        // suffix (`ret`, `jmp .L1`) operates at the width a flags-less LIR
        // instruction already means.
        return std::uint32_t{lirInstWidthBits(0)};
    }

    std::optional<DecodedOperand> decodeRegister(NodeId node,
                                                 DecodedOperand out) {
        // The last visible TOKEN child is the register name; the sigil is the
        // dialect's and carries no identity.
        std::string_view name;
        for (NodeId const k : visibleChildren(tree_, node)) {
            if (tree_.kind(k) == NodeKind::Token) name = tree_.text(k);
        }
        auto const ordinal = target_.registerByName(name);
        if (!ordinal) {
            fail(node,
                 std::format("unknown register '{}' — this target declares no "
                             "register by that name{}", name, pairSuffix()));
            return std::nullopt;
        }
        auto const* info = target_.registerInfo(*ordinal);
        if (info == nullptr) {
            fail(node, std::format("register '{}' has no info row", name));
            return std::nullopt;
        }
        out.regSpelling  = std::string{name};
        out.regWidthBits = static_cast<std::uint32_t>(info->widthBytes) * 8u;
        // ★★ A NARROW SPELLING RESOLVES TO ITS PARENT'S ORDINAL. `%eax` and
        // `%rax` are ONE machine register; LIR names it once and carries the
        // access width on the INSTRUCTION (`kLirInstFlagWidth32`), which is
        // also why the allocator holds every `subOf` row out of its pools. The
        // target's `subOf` chain is the single declaration of that aliasing —
        // resolved here rather than re-stated in a table this file owns.
        std::uint16_t resolved = *ordinal;
        for (int hop = 0; !info->subOf.empty(); ++hop) {
            auto const parent = target_.registerByName(info->subOf);
            if (!parent) {
                fail(node,
                     std::format("register '{}' declares subOf='{}', which this "
                                 "target does not declare{}", info->name,
                                 info->subOf, pairSuffix()));
                return std::nullopt;
            }
            resolved = *parent;
            info     = target_.registerInfo(resolved);
            if (info == nullptr) {
                fail(node, std::format("register ordinal {} has no info row",
                                       resolved));
                return std::nullopt;
            }
            // `TargetSchema::validate` already rejects a subOf cycle at load;
            // the bound keeps a hand-built schema from spinning here.
            if (hop > 8) {
                fail(node,
                     std::format("register '{}' has a subOf chain deeper than "
                                 "this build follows{}", name, pairSuffix()));
                return std::nullopt;
            }
        }
        out.regOrdinal = resolved;
        out.regClass   = static_cast<LirRegClass>(info->regClass);
        return out;
    }

    // ★ THE MEMORY OPERAND IS READ AS "THE REGISTERS IT NAMES, IN ORDER, PLUS
    // AN OPTIONAL DISPLACEMENT AND AN OPTIONAL SCALE" — which is LIR's own
    // addressing model, not a dialect shape. Base is the first register the
    // form names, index the second. A dialect whose memory form nests
    // differently binds `memory` to its own rule and this reading is unchanged.
    //
    // ★★★ WHERE THE DISPLACEMENT LIVES IS A DIALECT FACT, AND ASSUMING IT WAS
    // ALWAYS OUTSIDE WAS A SILENT MISCOMPILE. AT&T writes it OUTSIDE the parens
    // (`-8(%rbp)`), so `decodeOperand`'s `Displaced` arm reads it and this
    // function saw only registers and a scale. aarch64 writes it INSIDE the
    // brackets (`[x29, #-8]`) as an `immediate`-role child — and the old
    // reading swept its numeric token up as the SCALE. ✔MEASURED: `[x29, #-8]`
    // produced base=x29, scale=8, disp=0, silently addressing `x29 * 8`; and
    // because 1/2/4/8 are exactly the legal scales, the offsets that a
    // programmer is most likely to write are precisely the ones that pass the
    // scale validation without a word. `#-16` would have failed loud; `#-8`
    // did not.
    // ⇒ the displacement is read from the dialect's OWN `immediate` role when
    // the memory form nests one, and the scale is then the numeric token that
    // is inside NEITHER a register NOR that immediate.
    bool decodeMemory(NodeId memory, DecodedOperand& out) {
        std::vector<NodeId> regs;
        collectDescendantsOfRule(
            tree_, memory, cfg_.ruleForRole(AsmOperandRole::Register), regs);
        if (regs.empty()) {
            fail(memory,
                 std::format("this memory operand names no base register, and "
                             "an absolute address needs a relocation this build "
                             "does not reach from assembly yet{}",
                             pairSuffix()));
            return false;
        }
        if (regs.size() > 2) {
            fail(memory,
                 std::format("this memory operand names {} registers; LIR "
                             "addresses model a base and at most one index{}",
                             regs.size(), pairSuffix()));
            return false;
        }
        DecodedOperand base;
        auto const baseDecoded = decodeRegister(regs[0], base);
        if (!baseDecoded) return false;
        out.isMemory    = true;
        out.baseOrdinal = baseDecoded->regOrdinal;
        out.baseClass   = baseDecoded->regClass;
        if (regs.size() == 2) {
            DecodedOperand index;
            auto const indexDecoded = decodeRegister(regs[1], index);
            if (!indexDecoded) return false;
            out.hasIndex     = true;
            out.indexOrdinal = indexDecoded->regOrdinal;
            out.indexClass   = indexDecoded->regClass;
        }
        // The displacement, when this dialect nests one inside the memory form.
        NodeId const innerImm =
            findDescendantOfRule(tree_, memory,
                                 cfg_.ruleForRole(AsmOperandRole::Immediate));
        if (innerImm.valid()) {
            DecodedOperand disp;
            NodeId const   scalar =
                findDescendantOfRule(tree_, innerImm,
                                     cfg_.ruleForRole(AsmOperandRole::Scalar));
            if (!decodeScalar(scalar.valid() ? scalar : innerImm, disp)) {
                return false;
            }
            if (!disp.hasValue) {
                fail(innerImm,
                     std::format("memory displacement '{}' is a symbol, and a "
                                 "symbol-relative memory operand needs a "
                                 "relocation this build does not reach from "
                                 "assembly yet{}", disp.symbol, pairSuffix()));
                return false;
            }
            if (!fitsDisp(disp.value, innerImm)) return false;
            out.disp = static_cast<std::int32_t>(disp.value);
        }
        // The scale: numeric tokens outside every register subtree AND outside
        // the displacement read above.
        std::vector<std::string_view> numerics;
        collectNumericTokensOutsideRegisters(memory, numerics);
        if (numerics.size() > 1) {
            fail(memory,
                 std::format("this memory operand carries {} numeric fields; "
                             "LIR addresses model exactly one scale factor{}",
                             numerics.size(), pairSuffix()));
            return false;
        }
        if (numerics.size() == 1) {
            std::int64_t v = 0;
            if (!parseInteger(numerics[0], v) || v < 1 || v > 8
                || (v & (v - 1)) != 0) {
                fail(memory,
                     std::format("'{}' is not an index scale this build can "
                                 "encode — LIR's memory scale is 1, 2, 4 or 8{}",
                                 numerics[0], pairSuffix()));
                return false;
            }
            out.scale = static_cast<std::uint32_t>(v);
        }
        return true;
    }

    void collectNumericTokensOutsideRegisters(
        NodeId n, std::vector<std::string_view>& out) const {
        if (!n.valid()) return;
        if (tree_.kind(n) == NodeKind::Internal
            && tree_.rule(n).v
                   == cfg_.ruleForRole(AsmOperandRole::Register).v) {
            return;
        }
        // ⚠ AND OUTSIDE THE DISPLACEMENT. A dialect nesting its offset inside
        // the memory form (`[x29, #-8]`) puts a numeric token there too, and
        // counting it as a scale is the miscompile `decodeMemory` documents.
        if (tree_.kind(n) == NodeKind::Internal
            && cfg_.ruleForRole(AsmOperandRole::Immediate).valid()
            && tree_.rule(n).v
                   == cfg_.ruleForRole(AsmOperandRole::Immediate).v) {
            return;
        }
        if (tree_.kind(n) == NodeKind::Token) {
            auto const text = tree_.text(n);
            if (!text.empty() && text.front() >= '0' && text.front() <= '9') {
                out.push_back(text);
            }
            return;
        }
        for (NodeId const c : tree_.children(n)) {
            if (isEmptySpace(tree_.flags(c))) continue;
            collectNumericTokensOutsideRegisters(c, out);
        }
    }

    [[nodiscard]] static bool parseInteger(std::string_view text,
                                           std::int64_t& out) {
        int              base   = 10;
        std::string_view digits = text;
        if (digits.size() > 2 && digits[0] == '0'
            && (digits[1] == 'x' || digits[1] == 'X')) {
            base   = 16;
            digits = digits.substr(2);
        } else if (digits.size() > 2 && digits[0] == '0'
                   && (digits[1] == 'b' || digits[1] == 'B')) {
            base   = 2;
            digits = digits.substr(2);
        } else if (digits.size() > 1 && digits[0] == '0') {
            base   = 8;
            digits = digits.substr(1);
        }
        auto const res = std::from_chars(digits.data(),
                                         digits.data() + digits.size(), out,
                                         base);
        return res.ec == std::errc{}
               && res.ptr == digits.data() + digits.size();
    }

    bool fitsDisp(std::int64_t v, NodeId at) {
        if (v < std::numeric_limits<std::int32_t>::min()
            || v > std::numeric_limits<std::int32_t>::max()) {
            fail(at,
                 std::format("displacement {} does not fit the 32-bit "
                             "displacement slot LIR carries{}", v,
                             pairSuffix()));
            return false;
        }
        return true;
    }

    bool decodeScalar(NodeId node, DecodedOperand& out) {
        // A scalar is either a number (possibly negated) or a symbol name.
        //
        // ★★★ THE NEGATION IS SEARCHED FOR ON THE WHOLE DESCENT, NOT TESTED ON
        // THE NODE WE WERE HANDED — and that distinction was a LIVE SILENT
        // MISCOMPILE for a full cycle (D-ASM-NEGATIVE-SCALAR-LOSES-ITS-SIGN,
        // shipped in `e5b60f6c`, found 2026-08-13).
        //
        // ✔MEASURED three ways, every one of them exit-code visible and none of
        // them diagnosed: `movq $-8,%rcx` + `addq %rcx,%rax` returned 108 where
        // 92 was written (it ADDED 8); the arm64 twin `mov x1,#-8` did the
        // same; and `leaq -8(%rsp),%rax` computed a WRONG ADDRESS, which is
        // strictly worse than a wrong constant.
        //
        // ⚠ THE MECHANISM, because the shape recurs: `decodeOperand` resolves
        // the SCALAR role and hands over the alt WRAPPER (`attScalar` /
        // `armScalar`), whose own rule is the wrapper's — never `negNumber`. So
        // the old equality test on that one node was false for every negative,
        // control fell through to the "deepest token" probe, and the probe
        // returned the `IntLiteral` from UNDER `attNegNumber` with the
        // `MinusSign` sibling left behind. A node-identity test asked the wrong
        // node; nothing in the grammar was wrong.
        //
        // ⚠ AND THE SIGN COMES FROM THE PARSE STRUCTURE, NEVER FROM RE-READING
        // TEXT. `attNegNumber`/`armNegNumber` exist precisely so `-` is grammar
        // rather than lexing (gas's `-` is also a binary operator, so a signed
        // literal token would make `8-4` lex as two numbers); recovering the
        // sign by scanning for a '-' character would re-introduce exactly the
        // ambiguity the rule was written to remove.
        RuleId const negRule = cfg_.ruleForRole(AsmOperandRole::NegNumber);
        NodeId const negNode = findDescendantOfRule(tree_, node, negRule);
        bool const   negate  = negNode.valid();

        // ★★ WHICH TOKENS SPELL THE VALUE, AND WHY THE TWO ARMS DIFFER.
        //   * NEGATED: the negation rule is (sign, magnitude), so the value is
        //     its LAST token and the sign is carried by `negate`. Joining the
        //     tokens would produce "-8", which `parseInteger` reads as a
        //     non-number and would then be taken for a SYMBOL named `-8`.
        //   * NOT NEGATED: the value is EVERY token, joined in document order.
        //     ✔MEASURED 2026-08-13: `.L3` as an operand is
        //     `DirectiveDot Identifier`, and the old "deepest token" probe
        //     returned `L3` — a different symbol from the one the file wrote,
        //     which would have bound a branch to the wrong label (or minted a
        //     bogus extern) with no diagnostic. Joining is also what makes a
        //     plain `IntLiteral`, a bare `Identifier` and an alt wrapper around
        //     either read identically, which is the one reading a dialect-blind
        //     walker can defend. D-ASM-DOTTED-NAME-NOT-AN-OPERAND.
        std::string text;
        if (negate) {
            std::string_view last;
            forEachTokenInOrder(negNode,
                                [&](std::string_view t) { last = t; });
            text = std::string{last};
        } else {
            forEachTokenInOrder(node, [&](std::string_view t) { text += t; });
        }
        if (text.empty()) {
            fail(node, "could not read the operand's value");
            return false;
        }
        // A leading digit means a number; anything else is a symbol.
        if (text.front() >= '0' && text.front() <= '9') {
            std::int64_t v = 0;
            if (!parseInteger(text, v)) {
                fail(node, std::format("'{}' is not a value this build can read",
                                       text));
                return false;
            }
            out.value    = negate ? -v : v;
            out.hasValue = true;
            return true;
        }
        out.symbol = std::move(text);
        return true;
    }

    // Every TOKEN at or under `n`, in document order. The one traversal both
    // scalar arms use, so "which tokens spell this operand" has a single
    // answer.
    template <class Fn>
    void forEachTokenInOrder(NodeId n, Fn&& fn) const {
        if (!n.valid()) return;
        if (tree_.kind(n) == NodeKind::Token) { fn(tree_.text(n)); return; }
        for (NodeId const c : tree_.children(n)) {
            if (isEmptySpace(tree_.flags(c))) continue;
            forEachTokenInOrder(c, fn);
        }
    }

    // ★★★ THE OPERAND→LIR SHAPE IS DERIVED FROM THE TARGET, NOT RE-DECLARED
    // PER INSTRUCTION. The dialect states operand ORDER once; the target
    // already states, per opcode, its control-flow class, whether it produces a
    // value (`result`) and whether it is two-address (`requires2Address`).
    // Those facts plus the order determine the mapping:
    //   * destination operand → the LIR result, when the destination is a
    //     REGISTER and the opcode yields a value;
    //   * a destination that is MEMORY produces no result and expands into the
    //     address triple at the END of the operand list — which is exactly the
    //     `store` shape, and is why the "destination must be a register"
    //     refusal must not fire on it;
    //   * the remaining source operands, in source order, → the LIR operands;
    //   * a two-address opcode ALSO takes the destination as its first input,
    //     which is exactly the legalized form AT&T already writes
    //     (`addq %rcx, %rax` really is `rax = rax + rcx`).
    // ⚠ AND IT IS CHECKED, NOT ASSUMED: the resulting shape is put to the
    // TARGET's own encoding guards (`asm_variant_elect.hpp`), so a shape no
    // declared variant accepts fails loud naming the mnemonic and every opcode
    // that was tried — instead of being fitted.
    void buildLirInst(DecodedInstruction const& insIn,
                      AsmInstructionSpelling const& row,
                      ResolvedRow const& resolved) {
        // ★★ WHICH ARM OF A KIND-SPLIT ROW: the DIALECT fact (did the operand
        // carry this dialect's indirect marker?) times the TARGET fact (which
        // of the row's opcodes is reached through a register). A row with only
        // one arm has nothing to decide — `call` on x86_64 encodes both forms
        // as variants of ONE opcode and settles it inside `buildCall`.
        bool wroteIndirect = false;
        for (auto const& o : insIn.operands) {
            wroteIndirect = wroteIndirect || o.indirect;
        }
        std::optional<CfClass> const cfClass =
            (resolved.directClass.has_value() && resolved.indirectClass.has_value())
                ? (wroteIndirect ? resolved.indirectClass : resolved.directClass)
                : (resolved.directClass.has_value() ? resolved.directClass
                                                    : resolved.indirectClass);

        // ★ THE CONDITION, RESOLVED BEFORE THE SHAPE — because on a dialect
        // that writes it as an OPERAND it is not part of the shape at all.
        // `cset x0, eq` has ONE value operand; leaving `eq` in the list would
        // present the elector a two-operand shape the target's `setcc` (zero
        // value operands, reads FLAGS) declares no variant for.
        auto const cond = conditionFor(resolved, row, insIn);
        if (!cond.ok) return;
        DecodedInstruction stripped;
        if (cond.fromOperand != static_cast<std::size_t>(-1)) {
            stripped.mnemonic = insIn.mnemonic;
            stripped.node     = insIn.node;
            for (std::size_t i = 0; i < insIn.operands.size(); ++i) {
                if (i == cond.fromOperand) continue;
                stripped.operands.push_back(insIn.operands[i]);
            }
        }
        DecodedInstruction const& ins =
            cond.fromOperand != static_cast<std::size_t>(-1) ? stripped : insIn;

        // ⚠ A CONTROL-FLOW INSTRUCTION'S REGISTER OPERAND IS AN ADDRESS, NOT
        // DATA (`call *%rax`, `br x16`), so its width says nothing about the
        // operation and must not be derived from.
        bool const dataOperands = *cfClass == CfClass::Plain;
        auto const width = effectiveWidth(ins, row, dataOperands);
        if (!width.has_value()) return;

        std::uint8_t flags = 0;
        switch (*width) {
        case 8:  flags = kLirInstFlagWidth8;  break;
        case 16: flags = kLirInstFlagWidth16; break;
        case 32: flags = kLirInstFlagWidth32; break;
        case 64: flags = 0;                   break;
        default:
            fail(ins.node,
                 std::format("'{}' operates on {} bits, which LIR does not "
                             "model (8, 16, 32 or 64){}", ins.mnemonic, *width,
                             pairSuffix()));
            return;
        }
        std::uint8_t const widthBits = lirInstWidthBits(flags);
        // The condition, if this row's opcode encodes one. Computed ONCE and
        // handed to EVERY shape below — which shape elected is about operand
        // placement and says nothing about whether the encoder reads a
        // condition.
        //
        // ⚠ EVERY BUILDER TAKES IT, INCLUDING THE ONES NO SHIPPED TARGET NEEDS
        // IT FOR. Only `jcc` and `setcc` declare `condCodeFromPayload` today —
        // a cond-br and a plain instruction — so passing 0 to the return /
        // branch / call builders would work on both shipped targets and be a
        // SILENTLY DROPPED CONDITION on the first target that declares a
        // conditional return or a conditional branch-with-link (ARM has both).
        // That is the same "right by coincidence on the current opcode table"
        // shape D-ASM-COND-ALLOWED-ONLY-ON-JCC was; the resolve-time pair would
        // have DEMANDED a `cond` on such a row and the emit walk would have
        // thrown it away.
        std::uint32_t const payload = payloadFor(resolved, cond, ins);
        if (!ok_) return;

        // The candidate names the elected arm may draw from. A kind-split row
        // must NOT offer the other arm's opcode to the elector — `jmp *%rax`
        // presenting `[reg]` would otherwise be handed to `jmp`, whose only
        // variant guards on `[blockref]`, and the rejection list would name an
        // opcode the instruction was never eligible for.
        auto const armNames = candidatesForClass(row, *cfClass);

        switch (*cfClass) {
        case CfClass::Return:      buildReturn(ins, armNames, payload, flags); return;
        case CfClass::Br:          buildBr(ins, armNames, payload, flags); return;
        case CfClass::CondBr:      buildCondBr(ins, armNames, payload, flags); return;
        case CfClass::Call:        buildCall(ins, armNames, payload, flags, widthBits); return;
        case CfClass::IndirectBr:  buildIndirectBr(ins, armNames, payload, flags, widthBits); return;
        // ★★★ THIS ARM IS A REFUSAL, AND IT REPLACES A SILENT FALLTHROUGH —
        // NOT A WARNING. ✔MEASURED 2026-08-15
        // (D-BUILD-CLANG-ONLY-WARNINGS-INVISIBLE-TO-THE-MSVC-AND-GCC-LEGS):
        // with no `Switch` case and no `default:`, a switch-classed instruction
        // fell straight out of this dispatch into the PLAIN data path below —
        // destination/source partition, then election over
        // `candidatesForClass(row, Switch)`. Two outcomes, both wrong and
        // neither loud about the real cause:
        //   * a target whose switch opcode declares a variant matching the
        //     written shape ELECTS, and `builder_.addInst` appends a
        //     NON-TERMINATOR — the multi-way dispatch is lowered to an ordinary
        //     instruction and the block is left unterminated. A silent
        //     miscompile: control flow the programmer wrote, gone, rc=0.
        //   * a target whose variants do not match is refused as
        //     "no candidate target opcode encodes that shape", which blames the
        //     OPERANDS for what is really an unimplemented control-flow class.
        // ⚠ AND IT IS REACHABLE FROM CONFIG, not merely in theory.
        // `terminatorKind: "switch"` is a validated `.target.json` value —
        // `tests/core/test_target_schema.cpp` pins that a switch-kinded opcode
        // with minSuccessors 2 LOADS and one with minSuccessors 1 is refused —
        // and `cfClassOf` maps it here. Nothing between the loader and this
        // point filters it: `resolveRows` only cross-checks that a row's
        // candidates agree, and `emitInstruction`'s `anyClass()` gate only asks
        // that SOME class resolved. The reason no test caught it is that
        // NEITHER SHIPPED TARGET declares such an opcode today — a
        // right-by-coincidence-of-the-current-target-table shape, the same one
        // D-ASM-COND-ALLOWED-ONLY-ON-JCC was.
        // ★ WHY REFUSE RATHER THAN LOWER: there is nothing to lower TO. LIR's
        // terminator API is br / cond-br / indirect-br / return / unreachable
        // (`lir.hpp`); `addSwitch` exists on the MIR builder ONLY, and
        // `target_schema.hpp`'s own note calls the LIR one "reserved". A build
        // that cannot represent the construct must say so, in one message that
        // names the construct — which is what this arm does.
        case CfClass::Switch:
            fail(ins.node,
                 std::format("'{}' is {}, which this build does not lower from "
                             "assembly text: LIR carries no multi-way "
                             "terminator (its terminators are an unconditional "
                             "branch, a conditional branch, an indirect branch, "
                             "a return and an unreachable trap), so the "
                             "case-value-to-label mapping a switch instruction "
                             "carries has nowhere to go. Spell the dispatch as "
                             "compare-and-branch, or as an indirect branch "
                             "through a jump table{}",
                             ins.mnemonic, cfClassName(*cfClass),
                             pairSuffix()));
            return;
        case CfClass::Unreachable:
            fail(ins.node,
                 std::format("'{}' is {}, which this build does not lower from "
                             "assembly text: an unreachable trap written by hand "
                             "asserts a claim about control flow that the rest "
                             "of the file cannot be checked against{}",
                             ins.mnemonic, cfClassName(*cfClass),
                             pairSuffix()));
            return;
        case CfClass::Plain:       break;
        }

        std::size_t const n = ins.operands.size();
        if (n == 0) {
            fail(ins.node,
                 std::format("'{}' has no operands, which this build cannot "
                             "map onto {}{}", ins.mnemonic,
                             candidateList(armNames), pairSuffix()));
            return;
        }
        std::size_t const destIndex =
            cfg_.operandOrder == AsmOperandOrder::DestinationLast ? n - 1 : 0;
        DecodedOperand const& dest = ins.operands[destIndex];
        if (dest.indirect) {
            fail(dest.node,
                 std::format("'{}' writes to an indirect destination, which "
                             "this build does not lower{}", ins.mnemonic,
                             pairSuffix()));
            return;
        }
        if (dest.role != AsmOperandRole::Register && !dest.isMemory) {
            fail(dest.node,
                 std::format("'{}' writes to a destination that is neither a "
                             "register nor a memory reference, which this build "
                             "does not lower{}", ins.mnemonic, pairSuffix()));
            return;
        }

        // The sources, in SOURCE order, with any memory reference expanded into
        // LIR's address form.
        std::vector<LirOperand> sources;
        for (std::size_t i = 0; i < n; ++i) {
            if (i == destIndex) continue;
            DecodedOperand const& src = ins.operands[i];
            if (src.indirect) {
                fail(src.node,
                     std::format("'{}' reads an indirect source, which this "
                                 "build does not lower{}", ins.mnemonic,
                                 pairSuffix()));
                return;
            }
            if (src.isMemory) { appendMemory(src, sources); continue; }
            switch (src.role) {
            case AsmOperandRole::Register:
                sources.push_back(LirOperand::makeReg(
                    makePhysicalReg(src.regOrdinal, src.regClass)));
                break;
            case AsmOperandRole::Immediate:
            case AsmOperandRole::Scalar:
            case AsmOperandRole::NegNumber:
            case AsmOperandRole::Displaced: {
                if (!src.hasValue) {
                    // ★★★ M2 — A SYMBOL-VALUED SOURCE IS AN ADDRESS, AND IT
                    // LOWERS TO THE OPERAND SHAPE THE C FRONT END ALREADY
                    // EMITS. Nothing here asks which mnemonic was written: the
                    // operand becomes `[SymbolRef]` (a data/function address,
                    // as `lowerGlobalAddr` emits) or `[SymbolRef, BlockRef]`
                    // (an interior label, as `lowerBlockAddress` emits), and
                    // the ELECTION decides whether this target has an opcode
                    // that takes it. An opcode with no symbol-shaped variant
                    // fails through the ordinary "no candidate target opcode
                    // encodes that shape" path, naming the candidates.
                    if (!sourceOperandForSymbol(src, ins.mnemonic, sources)) {
                        return;
                    }
                    break;
                }
                if (src.value < std::numeric_limits<std::int32_t>::min()
                    || src.value > std::numeric_limits<std::int32_t>::max()) {
                    fail(src.node,
                         std::format("immediate {} does not fit the 32-bit "
                                     "immediate slot LIR carries — a wider "
                                     "constant needs the literal pool, which "
                                     "this build does not yet reach from "
                                     "assembly{}", src.value, pairSuffix()));
                    return;
                }
                sources.push_back(LirOperand::makeImmInt32(
                    static_cast<std::int32_t>(src.value)));
                break;
            }
            case AsmOperandRole::Memory:
            case AsmOperandRole::Indirect:
                fail(src.node, std::format("this operand form is not yet "
                                           "lowered by this build{}",
                                           pairSuffix()));
                return;
            }
        }

        // ★★ THE `result` PARTITION IS WHAT SPLITS `load` FROM `store`, AND IT
        // IS THE TARGET-SIDE READING OF "WHICH SIDE THE MEMORY OPERAND WAS
        // WRITTEN ON". The dialect fact is `dest.isMemory` (its `operandOrder`
        // times the memory operand's position); the target fact is that an
        // instruction writing to memory produces no value. Neither side needed
        // a new knob.
        std::vector<std::string> producers;
        std::vector<std::string> consumers;
        for (auto const& name : armNames) {
            auto const ordinal = target_.opcodeByMnemonic(name);
            if (!ordinal) continue;
            auto const* info = target_.opcodeInfo(*ordinal);
            if (info == nullptr) continue;
            (info->result != TargetResultRule::None ? producers : consumers)
                .push_back(name);
        }

        LirReg const destReg =
            dest.isMemory ? InvalidLirReg
                          : makePhysicalReg(dest.regOrdinal, dest.regClass);

        // ── SHAPE 3: memory destination. Sources, then the address tail. Only
        // a non-producer can take it — that is the `store` shape.
        if (dest.isMemory) {
            std::vector<LirOperand> operands = sources;
            appendMemory(dest, operands);
            auto const chosen =
                electAmong(consumers, operands, widthBits, ins);
            if (!chosen.has_value()) return;
            if (!checkElectedWidth(*chosen, *width, ins)) return;
            builder_.addInst(chosen->opcode, InvalidLirReg, operands, payload,
                             flags);
            ++blockInstCount_;
            return;
        }

        // ── SHAPE 1: producer, register destination. The two-address prefix is
        // not decided in advance — the plain shape is offered to the target's
        // guards first, and the destination-prefixed one only if nothing took
        // it.
        std::optional<asm_elect::ElectedOpcode> chosen;
        std::vector<LirOperand>                 operands;
        Election pe = electQuiet(producers, sources, widthBits);
        if (!pe.ambiguousWith.empty()) {
            reportAmbiguous(ins, pe, widthBits);
            return;
        }
        if (pe.opcode.has_value()) { chosen = pe.opcode; operands = sources; }
        if (!chosen.has_value() && twoAddressAmong(producers) != nullptr) {
            std::vector<LirOperand> twoAddr;
            twoAddr.reserve(sources.size() + 1);
            twoAddr.push_back(LirOperand::makeReg(destReg));
            twoAddr.insert(twoAddr.end(), sources.begin(), sources.end());
            Election t = electQuiet(producers, twoAddr, widthBits);
            if (!t.ambiguousWith.empty()) {
                reportAmbiguous(ins, t, widthBits);
                return;
            }
            if (t.opcode.has_value()) {
                chosen   = t.opcode;
                operands = std::move(twoAddr);
            }
        }

        // ── SHAPE 2: NON-producer, register destination — the destination is an
        // INPUT and goes FIRST.
        // ✔MEASURED 2026-08-13 on BOTH dialects: without this arm `cmpq
        // %rbx,%rax` produced ONE LIR operand for an opcode that takes two. A
        // compare writes only flags (`result: none`), so its AT&T "destination"
        // is a source — and it cannot simply keep its source-order position
        // either, because AT&T `cmpq %rbx,%rax` IS Intel `cmp rax,rbx`.
        //
        // ⚠ OFFERED ONLY WHEN NO MEMORY REFERENCE IS INVOLVED, and that gate is
        // load-bearing. ✔MEASURED: `movq 8(%rdi),%rax` builds the shape-2 list
        // `[rax, rdi, MemBase, MemOffset]`, which is BYTE-FOR-BYTE the guard
        // `store` declares — so `load` (shape 1) and `store` (shape 2) both
        // elected and the run was refused as ambiguous. When a memory reference
        // is present, LIR's own addressing model already decides the form from
        // WHICH SIDE it was written on: memory destination ⇒ shape 3, memory
        // source ⇒ shape 1. Shape 2 is for the register/immediate
        // flag-setters, and offering it alongside a memory operand only
        // manufactures a collision.
        bool anyMemory = dest.isMemory;
        for (auto const& o : ins.operands) anyMemory = anyMemory || o.isMemory;
        std::vector<LirOperand> destFirst;
        destFirst.reserve(sources.size() + 1);
        destFirst.push_back(LirOperand::makeReg(destReg));
        destFirst.insert(destFirst.end(), sources.begin(), sources.end());
        // ★★★ THE GATE IS "THIS ROW HAS A PRODUCER", NOT "A MEMORY OPERAND IS
        // PRESENT" — and the difference is the whole of arm64's `str`.
        // ✔MEASURED 2026-08-13: `str x1, [sp, #24]` was INEXPRESSIBLE. arm64 is
        // `destinationFirst`, so operand 0 is `x1` — a REGISTER — while the
        // thing the instruction actually writes is the MEMORY operand written
        // second. Shape 3 (memory destination) therefore never fired, shape 1
        // needs a producer and `store` produces nothing, and shape 2 was gated
        // off by the mere presence of a memory reference. The result was
        // `A0008: 'str' produced 3 LIR operand(s) … no candidate target opcode
        // encodes that shape` on the most ordinary store in the ISA.
        // ⚠ THE OLD GATE WAS PROTECTING SOMETHING REAL, WHICH IS WHY IT NARROWS
        // RATHER THAN DISAPPEARS. ✔MEASURED earlier in this arc: AT&T
        // `movq 8(%rdi),%rax` builds the shape-2 list `[rax, rdi, MemBase,
        // MemOffset]`, BYTE-FOR-BYTE the guard `store` declares, so `load`
        // (shape 1) and `store` (shape 2) BOTH elected and the run was refused
        // as ambiguous. What separates that case from arm64's `str` is not the
        // memory operand — both have one — it is that AT&T's `movq` row lists a
        // PRODUCER (`mov`/`load`) and arm64's `str` row lists none.
        // ⇒ a row that can produce a value keeps LIR's memory-side rule (memory
        // source ⇒ shape 1, memory destination ⇒ shape 3); a row that can ONLY
        // consume gets its destination as an input. That also keeps the
        // dangerous direction unreachable BY CONSTRUCTION rather than by
        // coincidence: a load whose producer candidate failed can never fall
        // through to its store sibling, because the presence of the producer
        // candidate is exactly what withholds shape 2.
        Election ce =
            (anyMemory && !producers.empty())
                ? Election{}
                : electQuiet(consumers, destFirst, widthBits);
        if (!ce.ambiguousWith.empty()) {
            reportAmbiguous(ins, ce, widthBits);
            return;
        }
        if (ce.opcode.has_value() && chosen.has_value()) {
            fail(ins.node,
                 std::format("'{}' could be target opcode '{}' (which produces "
                             "a value, so the destination is its result) or "
                             "'{}' (which produces none, so the destination is "
                             "an input) — one spelling cannot be both{}",
                             ins.mnemonic, chosen->info->mnemonic,
                             ce.opcode->info->mnemonic, pairSuffix()));
            return;
        }
        if (ce.opcode.has_value()) {
            if (!checkElectedWidth(*ce.opcode, *width, ins)) return;
            builder_.addInst(ce.opcode->opcode, InvalidLirReg, destFirst,
                             payload, flags);
            ++blockInstCount_;
            return;
        }
        if (!chosen.has_value()) {
            std::vector<asm_elect::ElectionRejectionRow> rejections;
            for (auto const& r : pe.rejections) rejections.push_back(r);
            for (auto const& r : ce.rejections) rejections.push_back(r);
            reportNoShape(ins, sources.size(), widthBits, rejections);
            return;
        }
        if (!checkElectedWidth(*chosen, *width, ins)) return;
        builder_.addInst(chosen->opcode, destReg, operands, payload, flags);
        ++blockInstCount_;
    }

    // ★★★ M2 — LOWER A SYMBOL-NAMED SOURCE OPERAND TO ITS ADDRESS.
    // D-ASM-SYMBOL-OPERAND-NOT-LOWERED + D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-
    // AT-AN-OFFSET, which are ONE mechanism seen from two sides: `adr x0, msg`
    // and `adr x0, Lcase1` differ only in WHAT the label is, and the second
    // operand is the whole difference.
    //
    // ★★ THE TWO SHAPES ARE THE C FRONT END'S, VERBATIM (`mir_to_lir.cpp`):
    //   * a DATA or FUNCTION label → `[SymbolRef]`, what `lowerGlobalAddr`
    //     emits for `&global`;
    //   * an INTERIOR label → `[SymbolRef, BlockRef]`, what `lowerBlockAddress`
    //     emits for `&&label`. The trailing BlockRef is metadata that wires to
    //     no encoding field — the encoder reads it and records a
    //     `BlockSymPatch`, which is how the synthetic symbol learns its byte
    //     offset. Both targets already declare a `lea` variant for each shape
    //     (`["symbol"]` / `["symbol","blockref"]`), so this needed no config,
    //     no new `LirOperandKind` and no relocation kind: interiority lives in
    //     the SYMBOL'S VA, so `addend == 0` and the existing rel32 / ADRP+ADD
    //     formulas resolve it unchanged.
    [[nodiscard]] bool sourceOperandForSymbol(DecodedOperand const&    src,
                                              std::string_view         mnemonic,
                                              std::vector<LirOperand>& sources) {
        if (src.symbol.empty()) {
            fail(src.node,
                 std::format("'{}' reads an operand that is neither a value nor "
                             "a name{}", mnemonic, pairSuffix()));
            return false;
        }
        auto const it = labelIndex_.find(src.symbol);
        if (it == labelIndex_.end()) {
            // ⚠ REFUSED RATHER THAN IMPORTED, for the reason
            // `bindPendingDataSymbols` states: `ExternImport::isData` selects
            // the linker's indirection slot and an address-materializing
            // instruction says nothing about code-vs-data. Anchored:
            // D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL.
            fail(src.node,
                 std::format("'{}' takes the address of '{}', which this file "
                             "defines no label for. An undefined name would "
                             "have to become an import, and an import states "
                             "whether it is CODE or DATA — which selects the "
                             "linker's indirection slot — while an address "
                             "operand says neither. A CALL is the one reference "
                             "that answers it, which is why only a call mints "
                             "one today{}", mnemonic, src.symbol, pairSuffix()));
            return false;
        }
        std::size_t const labelIdx = it->second;
        auto const&       L        = labels_[labelIdx];
        if (L.isEntry || L.isData) {
            // Already carries its symbol — a function entry from
            // `classifyLabels`, a data label from the scan.
            sources.push_back(LirOperand::makeSymbolRef(L.symbol.v));
            return true;
        }
        // ⚠ AN INTERIOR LABEL OF ANOTHER FUNCTION IS REFUSED, exactly as
        // `branchTarget` refuses a cross-function branch and for the identical
        // reason: `makeBlockRef` names a block SLOT, and a slot from another
        // function resolves to whatever block sits at that index here. The
        // binding would be silently wrong rather than absent.
        if (L.functionLabel != openFunctionLabel_) {
            fail(src.node,
                 std::format("'{}' takes the address of '{}', a label inside a "
                             "DIFFERENT function — the block reference that "
                             "binds an interior label's symbol to its byte "
                             "offset is function-local, so this address cannot "
                             "be expressed here{}",
                             mnemonic, src.symbol, pairSuffix()));
            return false;
        }
        SymbolId const sym = symbolForAddressedLabel(labelIdx);
        sources.push_back(LirOperand::makeSymbolRef(sym.v));
        sources.push_back(LirOperand::makeBlockRef(labels_[labelIdx].block.v));
        return true;
    }

    void appendMemory(DecodedOperand const& m,
                      std::vector<LirOperand>& operands) const {
        operands.push_back(
            LirOperand::makeReg(makePhysicalReg(m.baseOrdinal, m.baseClass)));
        if (m.hasIndex) {
            operands.push_back(LirOperand::makeReg(
                makePhysicalReg(m.indexOrdinal, m.indexClass)));
        }
        operands.push_back(LirOperand::makeMemBase(m.scale));
        operands.push_back(LirOperand::makeMemOffset(m.disp));
    }

    // The candidate opcodes of `row` whose control-flow class is `cls` — the
    // ARM of a kind-split row. Order is the dialect's declaration order, which
    // is the elector's only tie-break, so filtering preserves it.
    // ⚠ A CANDIDATE THIS TARGET DOES NOT DECLARE IS DROPPED HERE, exactly as
    // `resolveRows` skipped it: a shared dialect base may name a spelling this
    // CPU has no opcode for, and the refusal for that belongs at the point of
    // USE (which `emitInstruction`'s `anyClass()` gate already owns).
    [[nodiscard]] std::vector<std::string>
    candidatesForClass(AsmInstructionSpelling const& row, CfClass cls) const {
        std::vector<std::string> out;
        for (auto const& name : row.opcodeNames) {
            auto const ordinal = target_.opcodeByMnemonic(name);
            if (!ordinal) continue;
            auto const* info = target_.opcodeInfo(*ordinal);
            if (info == nullptr) continue;
            if (cfClassOf(*info) != cls) continue;
            out.push_back(name);
        }
        return out;
    }

    [[nodiscard]] std::string
    candidateList(std::vector<std::string> const& names) const {
        std::string joined;
        for (auto const& n : names) {
            if (!joined.empty()) joined += ", ";
            joined += '\'';
            joined += n;
            joined += '\'';
        }
        return std::format("target opcode(s) {}", joined);
    }

    // One election attempt: the winner (if any), why each loser lost, and the
    // name of a SECOND winner when the row is ambiguous. Reporting is the
    // caller's, because the two-address retry makes one failed attempt normal.
    struct Election {
        std::optional<asm_elect::ElectedOpcode>      opcode;
        std::vector<asm_elect::ElectionRejectionRow> rejections;
        std::string                                  ambiguousWith;
        std::string                                  firstWinner;
    };

    [[nodiscard]] Election
    electQuiet(std::vector<std::string> const& names,
               std::span<LirOperand const> operands,
               std::uint8_t widthBits) const {
        Election e;
        if (names.empty()) return e;
        e.opcode = asm_elect::electOpcode(target_, names, operands,
                                          widthBits, &e.rejections,
                                          &e.ambiguousWith);
        if (!e.ambiguousWith.empty()) {
            // `electOpcode` returns nullopt on ambiguity; recover the FIRST
            // winner's name for the message by replaying against the prefix
            // that produced it.
            for (auto const& name : names) {
                if (name == e.ambiguousWith) break;
                auto const ordinal = target_.opcodeByMnemonic(name);
                if (!ordinal) continue;
                auto const* info = target_.opcodeInfo(*ordinal);
                if (info == nullptr) continue;
                if (asm_elect::selectEncodingVariant(*info, operands,
                                                     widthBits) != nullptr) {
                    e.firstWinner = name;
                    break;
                }
            }
        }
        return e;
    }

    void reportAmbiguous(DecodedInstruction const& ins, Election const& e,
                         std::uint8_t widthBits) {
        fail(ins.node,
             std::format("'{}' could be target opcode '{}' or '{}' — both "
                         "declare an encoding variant taking this operand "
                         "shape at width {}, so the dialect's candidate list "
                         "does not say which instruction was written. Split "
                         "the spelling into rows whose candidate sets do not "
                         "overlap{}", ins.mnemonic,
                         e.firstWinner.empty() ? std::string{"<earlier "
                                                             "candidate>"}
                                               : e.firstWinner,
                         e.ambiguousWith, widthBits, pairSuffix()));
    }

    void reportNoShape(
        DecodedInstruction const& ins,
        std::size_t operandCount, std::uint8_t widthBits,
        std::span<asm_elect::ElectionRejectionRow const> rejections) {
        std::string detail;
        for (auto const& r : rejections) {
            if (!detail.empty()) detail += "; ";
            detail += std::format("'{}': {}", r.opcodeName,
                                  asm_elect::electionRejectionText(r.why));
        }
        fail(ins.node,
             std::format("'{}' produced {} LIR operand(s) at width {}, and no "
                         "candidate target opcode encodes that shape — {}. The "
                         "dialect's operand order and the target's opcode "
                         "shapes disagree{}",
                         ins.mnemonic, operandCount, widthBits,
                         detail.empty() ? std::string{"the row names no opcode "
                                                      "this target declares"}
                                        : detail,
                         pairSuffix()));
    }

    // The single-shot election every control-flow arm uses: the operand shape
    // is fixed by the class, so there is no two-address retry and any failure
    // is final.
    std::optional<asm_elect::ElectedOpcode>
    electAmong(std::vector<std::string> const& names,
               std::span<LirOperand const> operands, std::uint8_t widthBits,
               DecodedInstruction const& ins) {
        Election e = electQuiet(names, operands, widthBits);
        if (e.opcode.has_value()) return e.opcode;
        if (!e.ambiguousWith.empty()) {
            reportAmbiguous(ins, e, widthBits);
            return std::nullopt;
        }
        reportNoShape(ins, operands.size(), widthBits, e.rejections);
        return std::nullopt;
    }

    [[nodiscard]] TargetOpcodeInfo const*
    twoAddressAmong(std::vector<std::string> const& names) const {
        for (auto const& name : names) {
            auto const ordinal = target_.opcodeByMnemonic(name);
            if (!ordinal) continue;
            auto const* info = target_.opcodeInfo(*ordinal);
            if (info != nullptr && info->requires2Address) return info;
        }
        return nullptr;
    }

    // ★★ THE DECLARED WIDTH MUST ACTUALLY REACH THE BYTES. See
    // `asm_variant_elect.hpp`'s `variantHonorsDeclaredWidth` for the measured
    // arm64-`mov` / x86-`lea` case this closes.
    bool checkElectedWidth(asm_elect::ElectedOpcode const& elected,
                           std::uint32_t width,
                           DecodedInstruction const& ins) {
        if (asm_elect::variantHonorsDeclaredWidth(
                *elected.variant, static_cast<std::uint8_t>(width))) {
            return true;
        }
        fail(ins.node,
             std::format("'{}' operates on {} bits, but the target's opcode "
                         "'{}' declares no width-keyed encoding variant for the "
                         "operand shape it matched — the variant that matched is "
                         "width-agnostic and would encode at the target's "
                         "natural width, silently dropping the width this "
                         "instruction asked for{}",
                         ins.mnemonic, width, elected.info->mnemonic,
                         pairSuffix()));
        return false;
    }

    // ── control-flow arms ─────────────────────────────────────────────────
    void buildReturn(DecodedInstruction const& ins,
                     std::vector<std::string> const& names,
                     std::uint32_t payload, std::uint8_t flags) {
        if (!ins.operands.empty()) {
            fail(ins.node,
                 std::format("'{}' is a return and this build lowers only its "
                             "operand-less form{}", ins.mnemonic,
                             pairSuffix()));
            return;
        }
        auto const elected = electAmong(
            names, std::span<LirOperand const>{}, lirInstWidthBits(flags), ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, lirInstWidthBits(flags), ins)) {
            return;
        }
        builder_.addReturn(elected->opcode, {}, payload, flags);
        openTerminated_ = true;
        ++blockInstCount_;
    }

    // The block a branch operand names, or nullopt with a diagnostic. ⚠ A
    // BRANCH TARGET IS FUNCTION-LOCAL: `LirOperand::makeBlockRef` names a block
    // slot, and a slot from another function would silently resolve to whatever
    // block sits at that index here.
    std::optional<LirBlockId> branchTarget(DecodedOperand const& op,
                                           std::string_view mnemonic) {
        if (op.indirect || op.isMemory || op.symbol.empty()) {
            fail(op.node,
                 std::format("'{}' needs a label to branch to; this operand is "
                             "not one{}", mnemonic, pairSuffix()));
            return std::nullopt;
        }
        auto const it = labelIndex_.find(op.symbol);
        if (it == labelIndex_.end()) {
            fail(op.node,
                 std::format("'{}' branches to '{}', which this file defines no "
                             "label for — a branch out of the translation unit "
                             "is a relocation this build does not reach from "
                             "assembly yet{}", mnemonic, op.symbol,
                             pairSuffix()));
            return std::nullopt;
        }
        auto const& L = labels_[it->second];
        if (L.isEntry || L.functionLabel != openFunctionLabel_) {
            fail(op.node,
                 std::format("'{}' branches to '{}', which belongs to a "
                             "different function — LIR block references are "
                             "function-local, so this edge cannot be "
                             "expressed{}", mnemonic, op.symbol, pairSuffix()));
            return std::nullopt;
        }
        return L.block;
    }

    void buildBr(DecodedInstruction const& ins,
                 std::vector<std::string> const& names, std::uint32_t payload,
                 std::uint8_t flags) {
        if (ins.operands.size() != 1) {
            fail(ins.node,
                 std::format("'{}' is an unconditional branch and takes exactly "
                             "one target; {} were written{}", ins.mnemonic,
                             ins.operands.size(), pairSuffix()));
            return;
        }
        auto const target = branchTarget(ins.operands[0], ins.mnemonic);
        if (!target.has_value()) return;
        std::array<LirOperand, 1> const ops{
            LirOperand::makeBlockRef(target->v)};
        auto const elected =
            electAmong(names, ops, lirInstWidthBits(flags), ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, lirInstWidthBits(flags), ins)) {
            return;
        }
        builder_.addBr(elected->opcode, *target, payload, flags);
        openTerminated_ = true;
        ++blockInstCount_;
    }

    void buildCondBr(DecodedInstruction const& ins,
                     std::vector<std::string> const& names,
                     std::uint32_t payload, std::uint8_t flags) {
        if (ins.operands.size() != 1) {
            fail(ins.node,
                 std::format("'{}' is a conditional branch and takes exactly "
                             "one taken-target; {} were written{}",
                             ins.mnemonic, ins.operands.size(), pairSuffix()));
            return;
        }
        auto const taken = branchTarget(ins.operands[0], ins.mnemonic);
        if (!taken.has_value()) return;
        // ★ THE FALSE EDGE IS MINTED. A `.s` writes only the taken target; the
        // fallthrough is the next instruction and usually has no label, but LIR
        // records BOTH successors explicitly (the encoder emits the trailing
        // unconditional jump from operand[1]). So an anonymous block is created
        // here and the instructions that follow are emitted into it.
        LirBlockId const fallthrough = builder_.createBlock();
        std::array<LirOperand, 2> const ops{
            LirOperand::makeBlockRef(taken->v),
            LirOperand::makeBlockRef(fallthrough.v)};
        auto const elected =
            electAmong(names, ops, lirInstWidthBits(flags), ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, lirInstWidthBits(flags), ins)) {
            return;
        }
        // ⚠ THE SHARED `payload`, NOT `*resolved.cond`. Under the old
        // `terminatorKind == cond-br` key a cond-br row ALWAYS carried a
        // condition, so the dereference was safe by construction; under the
        // `condCodeFromPayload` key it is not. A `cbz`/`tbz`-shaped conditional
        // branch is a cond-br whose encoding reads NO condition code, so `cond`
        // is correctly REJECTED on its row — and dereferencing the empty
        // optional here would be undefined behaviour rather than a diagnostic.
        builder_.addCondBr(elected->opcode, ops, *taken, fallthrough, payload,
                           flags);
        ++blockInstCount_;
        builder_.beginBlock(fallthrough);
        openTerminated_ = false;
        blockInstCount_ = 0;
    }

    // The `ExternImport` row for `name`, minted on first reference and reused
    // after. ★ ONE ROW PER NAME, because two references to `puts` are two
    // relocations against ONE dynamic symbol; minting twice would present the
    // linker with two rows the merge would have to collapse (and whose
    // `(mangledName, libraryPath, version)` key would collapse them anyway,
    // leaving one dead SymbolId that `declare()` had already registered).
    //
    // ★ SYMBOL IDS COME FROM THE SAME MONOTONIC MINT AS EVERY OTHER SYMBOL
    // THIS FILE DEFINES (`mintSymbol`), so an import can never collide with a
    // defined function or a data item — which the linker's per-CU `declare()`
    // would otherwise reject as a duplicate.
    //
    // ★ THE NAME IS TAKEN VERBATIM, NOT MANGLED. A `.s` writes the ON-BINARY
    // symbol — a Mach-O source writes `_puts` itself, exactly as gas requires —
    // so applying a format's C mangling here would rename what the programmer
    // wrote. That is also why there is no format branch anywhere in this
    // function: there is nothing per-format left to decide.
    [[nodiscard]] SymbolId internExtern(std::string const& name) {
        if (auto const it = externIndex_.find(name); it != externIndex_.end()) {
            return externs_[it->second].symbol;
        }
        ExternImport row;
        row.symbol      = mintSymbol();
        row.mangledName = name;
        // `libraryPath` / `version` stay EMPTY (unbound), `isEagerImport` false
        // (nothing shipped this row, so the reference gate may drop it when
        // nothing references it), and `isData` false — this row exists because
        // a CALL named it, and a call target is code.
        row.isData = false;
        externIndex_.emplace(name, externs_.size());
        externs_.push_back(std::move(row));
        return externs_.back().symbol;
    }

    void buildCall(DecodedInstruction const& ins,
                   std::vector<std::string> const& names, std::uint32_t payload,
                   std::uint8_t flags, std::uint8_t widthBits) {
        if (ins.operands.size() != 1) {
            fail(ins.node,
                 std::format("'{}' is a call and takes exactly one callee; {} "
                             "were written{}", ins.mnemonic,
                             ins.operands.size(), pairSuffix()));
            return;
        }
        DecodedOperand const& callee = ins.operands[0];
        std::vector<LirOperand> ops;
        if (callee.indirect && callee.role == AsmOperandRole::Register) {
            ops.push_back(LirOperand::makeReg(
                makePhysicalReg(callee.regOrdinal, callee.regClass)));
        } else if (!callee.indirect && !callee.symbol.empty()
                   && !callee.isMemory) {
            auto const it = labelIndex_.find(callee.symbol);
            if (it != labelIndex_.end()) {
                // ⚠ A LABEL THIS FILE DEFINES BUT DID NOT MARK AS A FUNCTION
                // ENTRY IS A **BLOCK**, AND A BLOCK IS NOT A CALL TARGET. It
                // has no module symbol (only function-entry labels are minted
                // one), so there is nothing for a call's SymbolRef to name, and
                // the alternatives are both silent: treating it as an extern
                // would import a name this very file defines, and treating it
                // as a function would call into the middle of another frame.
                if (!labels_[it->second].isEntry) {
                    fail(callee.node,
                         std::format("'{}' calls '{}', which this file defines "
                                     "as a BLOCK inside another function rather "
                                     "than as a function entry — a call needs a "
                                     "function symbol, and calling into an "
                                     "interior label would enter a frame whose "
                                     "prologue never ran. Mark it with this "
                                     "dialect's function-entry directive ({}) "
                                     "if it really is one{}",
                                     ins.mnemonic, callee.symbol,
                                     cfg_.spellingsForVerb(
                                         AsmDirectiveVerb::FunctionEntry),
                                     pairSuffix()));
                    return;
                }
                ops.push_back(
                    LirOperand::makeSymbolRef(labels_[it->second].symbol.v));
            } else {
                // ★★★ AN UNDEFINED CALLEE IS AN EXTERN, WITH NO DIRECTIVE AND
                // NO GUESS. See `AsmTextModule::externImports` for why gas has
                // no extern directive and why `libraryPath` stays empty; the
                // reference gate at the link tier is what judges the reference.
                ops.push_back(LirOperand::makeSymbolRef(
                    internExtern(callee.symbol).v));
            }
        } else {
            fail(callee.node,
                 std::format("'{}' needs a symbol or a register as its callee{}",
                             ins.mnemonic, pairSuffix()));
            return;
        }
        auto const elected = electAmong(names, ops, widthBits, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, widthBits, ins)) return;
        // ★ A CALL IS NOT A TERMINATOR — plain `addInst`. And this path runs
        // POST-callconv (no pass follows), so the operand list is exactly the
        // callee reference: the argument registers were set by the instructions
        // the programmer wrote above it.
        builder_.addInst(elected->opcode, InvalidLirReg, ops, payload,
                         flags);
        ++blockInstCount_;
    }

    // ★★★ M1 — THE ONE PLACE AN INTERIOR LABEL ACQUIRES A SymbolId, AND IT IS
    // REACHED ONLY FROM A RELOCATION SITE. `classifyLabels` mints for an ENTRY
    // label (it becomes a function symbol) and the scan mints for a DATA label
    // (it names an `AssembledData`); a BLOCK label deliberately gets none,
    // because carrying a symbol is exactly what
    // `derivableIndirectSuccessors()` reads as "this block's address was
    // taken". That is the obligation the comment below states, and it is why
    // this function is private to the two callers that bind a relocation:
    // `sourceOperandForSymbol` (an address-materializing instruction) and
    // `bindPendingDataSymbols` (a symbol-valued data slot). A third caller with
    // any other motive would WIDEN the successor set of every indirect branch
    // in the function.
    [[nodiscard]] SymbolId symbolForAddressedLabel(std::size_t labelIdx) {
        auto& L = labels_[labelIdx];
        if (!L.symbol.valid()) L.symbol = mintSymbol();
        return L.symbol;
    }

    // The index into `lir.funcAt(i)` of the function whose entry label is
    // `entryIdx`. ★ DERIVED, NOT STORED: `openFunction` calls
    // `builder_.addFunction` once per entry label in source order, so a
    // function's LIR index IS its ordinal among the entry labels. A stored
    // field would be a second copy of that fact, free to disagree.
    [[nodiscard]] std::size_t functionOrdinalOf(std::size_t entryIdx) const {
        std::size_t ordinal = 0;
        for (std::size_t i = 0; i < entryIdx && i < labels_.size(); ++i) {
            if (labels_[i].isEntry) ++ordinal;
        }
        return ordinal;
    }

    // PASS 1c — resolve each pending data slot's NAME to a label and mint the
    // symbol the relocation will target.
    bool bindPendingDataSymbols() {
        for (auto& p : pendingDataRelocs_) {
            auto const it = labelIndex_.find(p.name);
            if (it == labelIndex_.end()) {
                // ★ A NAME THIS FILE DEFINES NOWHERE IS REFUSED RATHER THAN
                // IMPORTED. `internExtern` mints a row whose `isData` drives
                // the linker's GOT-vs-PLT slot choice (`elf.cpp`), and a data
                // directive states nothing about whether the thing it points
                // at is code or data — so the import would be a guess with a
                // wire-format consequence. Anchored:
                // D-ASM-DATA-SLOT-CANNOT-NAME-AN-UNDEFINED-SYMBOL.
                fail(p.at,
                     std::format("'.{}' names '{}', which this file defines no "
                                 "label for. A data slot holding an address "
                                 "must name something this translation unit "
                                 "defines: an undefined name would have to be "
                                 "imported, and an import states whether it is "
                                 "CODE or DATA (which selects the linker's "
                                 "indirection slot), while a data directive "
                                 "says neither{}",
                                 p.spelling, p.name, pairSuffix()));
                return false;
            }
            p.labelIndex = it->second;
            auto const& L = labels_[p.labelIndex];
            // An entry or data label already carries its symbol; only an
            // interior BLOCK label is minted here, and minting it is precisely
            // the act that makes its block address-taken.
            if (L.isEntry || L.isData) continue;
            if (L.functionLabel == kNoLabel) {
                fail(p.at,
                     std::format("'.{}' names '{}', which is a label inside no "
                                 "function — its address is not part of any "
                                 "function's bytes, so there is nothing to "
                                 "relocate against{}",
                                 p.spelling, p.name, pairSuffix()));
                return false;
            }
            // ★ THE RETURN IS DELIBERATELY UNUSED, AND THE DISCARD IS SPELLED
            // rather than implicit: pass 3b re-reads `labels_[…].symbol` when
            // it writes the relocation, so what this call is FOR is the mint
            // itself — which, per M1 above, is the act that marks the block
            // address-taken. `[[nodiscard]]` stays on the function because the
            // OTHER caller (`sourceOperandForSymbol`) does consume the id.
            (void)symbolForAddressedLabel(p.labelIndex);
        }
        return ok_;
    }

    // PASS 3b — write each pending slot's relocation, and, for a slot naming an
    // INTERIOR label, the block-symbol binding the driver needs.
    void emitPendingDataRelocations() {
        for (auto const& p : pendingDataRelocs_) {
            if (p.labelIndex == kNoLabel) continue;   // pass 1c already failed
            auto const& L = labels_[p.labelIndex];
            dataItems_[p.itemIndex].relocations.push_back(
                Relocation{p.byteOffset, L.symbol, p.kind, /*addend=*/0});
            if (L.isEntry || L.isData) continue;
            // ★★★ M4 — THE ONE CHANNEL THAT DID NOT ALREADY EXIST. An interior
            // label named ONLY from data emits no instruction, so the encoder
            // records no `BlockSymPatch` and `assemble()` never binds the
            // symbol to a byte offset. `AsmTextModule::blockSymbolBindings`
            // carries the (function, block, symbol) triple to the driver, which
            // binds it from the assembled function's `blockByteOffsets` through
            // the SAME helper the C jump-table path uses.
            //
            // ⚠ AN INVALID BLOCK ID FAILS LOUD RATHER THAN BEING SKIPPED, and
            // the difference matters: the relocation was ALREADY pushed above,
            // so skipping the binding would leave a data slot relocating
            // against a symbol with no address — the linker would report an
            // undefined symbol and point at the link, not at this label.
            // `openFunction` reserves every block of a function when it opens
            // and pass 1c already refused a label belonging to no function, so
            // reaching this is a pass-ordering bug in this file.
            if (!L.block.valid()) {
                fail(p.at,
                     std::format("internal: '.{}' takes the address of '{}', "
                                 "which reserved no basic block — the emit walk "
                                 "never opened the function that contains it, "
                                 "so this slot's relocation would name a symbol "
                                 "with no address{}",
                                 p.spelling, p.name, pairSuffix()));
                return;
            }
            blockSymbolBindings_.push_back(AsmBlockSymbolBinding{
                functionOrdinalOf(L.functionLabel), L.block.v, L.symbol});
        }
    }

    // The successor set an indirect branch in the OPEN function can be DERIVED
    // to reach: every INTERIOR label of that function whose address this file
    // has bound a relocation against. Empty ⇒ nothing to derive the set FROM.
    //
    // ★★★ "ADDRESS-TAKEN" IS ASKED OF THE EXISTING LABEL MODEL, AND THE TEST
    // IS `symbol.valid()` FOR A REASON. A relocation names a SYMBOL; an
    // interior label is a BLOCK and carries none until something needs to name
    // it — which is exactly the step the C path performs at the same moment
    // (`lowerBlockAddress` calls `mintBlockSymbol(target)` and only then emits
    // the `lea` that materializes the address). So "this interior label carries
    // a symbol" and "this file bound a relocation against this interior label"
    // are ONE fact, read off `LabelInfo` rather than mirrored into a side-table
    // — the same property `Mir::isBlockAddressTaken` states for the C tier
    // ("DERIVED from the IR … NO parallel side-table to maintain"). No
    // asm-private successor list, no jump-table model, no new operand kind.
    //
    // ⚠ THE ONE OBLIGATION THIS PLACES ON A FUTURE CHANGE: whatever mints a
    // symbol for an INTERIOR label must be the code that binds a relocation
    // against it. A minter with any other motive would WIDEN this set, and a
    // wider indirect-branch successor set is precisely the "every block"
    // over-approximation the refusal below exists to refuse.
    [[nodiscard]] std::vector<LirBlockId> derivableIndirectSuccessors() const {
        std::vector<LirBlockId> out;
        for (auto const& L : labels_) {
            if (L.isEntry || L.isData) continue;          // not a block
            if (L.functionLabel != openFunctionLabel_) continue;
            if (!L.symbol.valid()) continue;              // no relocation bound
            out.push_back(L.block);
        }
        return out;
    }

    // ★★★ THE INDIRECT BRANCH — REACHED ONLY BECAUSE ELECTION NOW SPANS
    // TERMINATOR KINDS (D-ASM-ATT-INDIRECT-BRANCH-INEXPRESSIBLE). gas spells
    // `jmp .L1` and `jmp *%rax` with ONE mnemonic; the row names both target
    // opcodes and the star on the operand picks the arm. Nothing below asks
    // WHICH opcode was elected — the dispatch already happened, keyed on the
    // `TargetTerminatorKind::IndirectBr` the TARGET declares.
    //
    // ⚠⚠ AND THE REFUSAL IS KEYED ON **DERIVABILITY**, NOT ON THE OPCODE —
    // WHICH IS THE WHOLE DESIGN AND NOT A WORDING PREFERENCE. LIR's
    // `addIndirectBr` takes every ADDRESS-TAKEN successor block, and a `.s`
    // binds that set in exactly two ways:
    //   * materializing a label's address (`adr x1, Lcase1` / `leaq Lw, %rax`)
    //     — `sourceOperandForSymbol`;
    //   * a jump table in data (`.quad Lcase1`) — `bindPendingDataSymbols`.
    // ★★★ ✔MEASURED 2026-08-13: THE PREDICTION THIS COMMENT MADE CAME TRUE
    // WITH NO EDIT TO THIS FUNCTION. Both bindings landed, and the ONLY thing
    // that changed here is that the condition stopped holding — the refusal
    // below was not moved, rewritten or deleted, and `addIndirectBr` needed no
    // relaxation. That is what a refusal keyed on the missing INPUT buys over
    // one keyed on the opcode ("indirect branches are unsupported"), which
    // would have had to be deleted by the very change that made it obsolete
    // and would have said nothing true in the meantime.
    //
    // ⚠ `addIndirectBr` IS NOT RELAXED TO ACCEPT AN EMPTY LIST, and that is the
    // opposite of a granularity fork: its explicit successor list is the
    // CORRECT LIR shape (it matches LLVM's `indirectbr` and matches what the C
    // path already supplies through `lowerIndirectBr`). The gap is that this
    // front end cannot yet DERIVE the input, not that LIR asks the wrong
    // question. An EMPTY successor list would say "control leaves this function
    // here" — an UNDER-approximation, and unsound rather than merely imprecise,
    // because liveness and regalloc read the successor pool and would judge
    // every value live across the edge to be dead. "Every block of this
    // function" would be a sound over-approximation and is still this build
    // deciding what the programmer meant, which this walker does nowhere else.
    // Anchored: D-ASM-INDIRECT-BRANCH-SUCCESSOR-SET-UNSTATED, whose blocker is
    // the merged capability row D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET.
    void buildIndirectBr(DecodedInstruction const& ins,
                         std::vector<std::string> const& names,
                         std::uint32_t payload, std::uint8_t flags,
                         std::uint8_t widthBits) {
        if (ins.operands.size() != 1) {
            fail(ins.node,
                 std::format("'{}' is an indirect branch and takes exactly one "
                             "target-address operand; {} were written{}",
                             ins.mnemonic, ins.operands.size(), pairSuffix()));
            return;
        }
        DecodedOperand const& op = ins.operands[0];
        if (op.role != AsmOperandRole::Register || op.isMemory) {
            fail(op.node,
                 std::format("'{}' is an indirect branch and this build lowers "
                             "only its REGISTER form — the target address must "
                             "already be in a register{}",
                             ins.mnemonic, pairSuffix()));
            return;
        }
        std::array<LirOperand, 1> const ops{
            LirOperand::makeReg(makePhysicalReg(op.regOrdinal, op.regClass))};
        // The election still runs, and runs FIRST: a row whose indirect arm
        // names an opcode that cannot take a register is a config defect, and
        // reporting it before the successor-set refusal keeps the two failures
        // distinguishable.
        auto const elected = electAmong(names, ops, widthBits, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, widthBits, ins)) return;
        std::vector<LirBlockId> const succs = derivableIndirectSuccessors();
        if (succs.empty()) {
            fail(ins.node,
                 std::format("'{}' elects target opcode '{}', an indirect "
                             "branch, and its SUCCESSOR SET CANNOT BE DERIVED: "
                             "function '{}' binds no relocation against any of "
                             "its INTERIOR LABELS, so none of its blocks is "
                             "address-taken and there is nothing to derive the "
                             "set from. That binding is the missing input — not "
                             "the instruction, which elected fine. A `.s` writes "
                             "one by materializing a label's address "
                             "(`lea`/`adr` of a label) or by listing labels in a "
                             "jump table (`.quad <label>`); this build realizes "
                             "BOTH, so add one of them for the label(s) this "
                             "branch may reach. Guessing is what stays refused: "
                             "NO successors would claim control "
                             "leaves the function here, which liveness and "
                             "register allocation read as 'every value live "
                             "across this edge is dead'; EVERY block would be "
                             "this build deciding which labels you meant{}",
                             ins.mnemonic, elected->info->mnemonic,
                             labels_[openFunctionLabel_].name, pairSuffix()));
            return;
        }
        builder_.addIndirectBr(elected->opcode, ops, succs, payload, flags);
        openTerminated_ = true;
        ++blockInstCount_;
    }

    // ── tree walking ──────────────────────────────────────────────────────
    //
    // Visit each line's element in source order. The line rule is the dialect's
    // to name, so nothing here indexes children by position.
    template <class Fn>
    void walkElements(NodeId root, Fn&& fn) {
        for (NodeId const line : visibleChildren(tree_, root)) {
            if (tree_.kind(line) != NodeKind::Internal) continue;
            if (tree_.rule(line).v != cfg_.lineRule.v) continue;
            for (NodeId const child : visibleChildren(tree_, line)) {
                if (tree_.kind(child) != NodeKind::Internal) continue;
                if (tree_.rule(child).v != cfg_.elementRule.v) continue;
                for (NodeId const element : visibleChildren(tree_, child)) {
                    if (tree_.kind(element) != NodeKind::Internal) continue;
                    fn(element);
                }
            }
        }
    }

    // The DIRECTIVE that follows a label ON THE SAME LINE, or invalid.
    //
    // ★★ `msg: .asciz "hi"` AND `main: .globl main` ARE ORDINARY gas, AND THE
    // SCAN USED TO DROP BOTH. `walkElements` visits LINE-level elements, and a
    // directive nested inside a label tail is not one — the old comment claimed
    // it would be "picked up when the walk reaches it", which is false. A
    // dropped `.globl` emits the entry symbol with LOCAL linkage; a dropped
    // data directive emits an empty item. Neither says anything.
    NodeId nextDirectiveAfterLabel(NodeId statement) {
        auto const kids = visibleChildren(tree_, statement);
        if (kids.size() < 2) return NodeId{};
        NodeId const labelTail =
            findDescendantOfRule(tree_, kids[1], cfg_.labelTailRule);
        if (!labelTail.valid()) return NodeId{};
        NodeId const element =
            findDescendantOfRule(tree_, labelTail, cfg_.elementRule);
        if (!element.valid()) return NodeId{};
        return findDescendantOfRule(tree_, element, cfg_.directiveRule);
    }

    // The statement that follows a label ON THE SAME LINE, or invalid.
    NodeId nextStatementAfterLabel(NodeId statement) {
        auto const kids = visibleChildren(tree_, statement);
        if (kids.size() < 2) return NodeId{};
        NodeId const labelTail =
            findDescendantOfRule(tree_, kids[1], cfg_.labelTailRule);
        if (!labelTail.valid()) return NodeId{};
        NodeId const element =
            findDescendantOfRule(tree_, labelTail, cfg_.elementRule);
        if (!element.valid()) return NodeId{};
        return findDescendantOfRule(tree_, element, cfg_.statementRule);
    }

    // The label a statement defines, or invalid when it defines none.
    NodeId labelOf(NodeId element) {
        if (tree_.rule(element).v != cfg_.statementRule.v) return NodeId{};
        auto const kids = visibleChildren(tree_, element);
        if (kids.size() < 2) return NodeId{};
        NodeId const tail = kids[1];
        if (tree_.kind(tail) != NodeKind::Internal) return NodeId{};
        // A statement tail wraps the chosen arm; the label arm is the one whose
        // rule the dialect named `labelTailRule`. ⚠ Search the FIRST tail only
        // — a `a: b: ret` chain nests a second statement inside this one, and
        // its label is collected when the walk reaches it, not here.
        NodeId const labelTail =
            findDescendantOfRule(tree_, tail, cfg_.labelTailRule);
        return labelTail.valid() ? kids.front() : NodeId{};
    }

    // ★ ONE MONOTONIC SYMBOL MINT FOR EVERY SYMBOL THIS FILE DEFINES OR
    // IMPORTS — functions, data items and extern references alike.
    // ⚠⚠ IT STARTS AT 1, NOT 0, AND THAT IS THE WHOLE POINT.
    // `SymbolId::valid()` is `v != 0`, so the previous numbering handed the
    // FIRST function the id the substrate reserves as its INVALID SENTINEL.
    // ✔MEASURED LATENT, NOT LIVE at the time: no `.valid()` gate sat on the
    // link/emit path, so every emitted object was byte-correct — which is
    // exactly what makes it a booby trap rather than a bug. The first consumer
    // to write `if (sym.valid())` would silently drop function 0, and nothing
    // in the output would show it. D-ASM-FIRST-FUNCTION-TAKES-SYMBOLID-ZERO.
    [[nodiscard]] SymbolId mintSymbol() noexcept {
        return SymbolId{nextSymbolId_++};
    }

    // One `ModuleSymbol` per DATA label, so the linker can match it across CUs
    // by NAME exactly as it does a function's. ⚠ THE BINDING IS READ FROM THE
    // SAME `globals_` SET the function arm reads — a `.globl msg` and a
    // `.globl main` are one directive with one meaning, and a second policy for
    // data would drift from the first.
    void addDataSymbols() {
        for (auto const& L : labels_) {
            if (!L.isData) continue;
            ModuleSymbol sym;
            sym.symbol     = L.symbol;
            sym.name       = L.name;
            sym.binding    = globals_.contains(L.name) ? SymbolBinding::Global
                                                       : SymbolBinding::Local;
            sym.visibility = SymbolVisibility::Default;
            symbols_.push_back(std::move(sym));
        }
    }

    Tree const&                  tree_;
    GrammarSchema const&         grammar_;
    TargetSchema const&          target_;
    AssemblyConfig const&        cfg_;
    std::span<std::string const> entryNames_;
    DiagnosticReporter&          reporter_;
    LirBuilder                   builder_;

    std::vector<ResolvedRow>                    rows_;
    std::vector<LabelInfo>                      labels_;
    std::unordered_map<std::string, std::size_t> labelIndex_;
    std::unordered_set<std::string>             functionEntryNames_;
    std::unordered_set<std::string>             globals_;
    std::vector<ModuleSymbol>                   symbols_;
    // Referenced-but-undefined symbols, in FIRST-REFERENCE order (source
    // order), with `externIndex_` mapping the name to its slot. Deterministic
    // by construction — the emit walk is source-ordered — so two runs over one
    // file mint identical SymbolIds and emit identical bytes.
    std::vector<ExternImport>                    externs_;
    std::unordered_map<std::string, std::size_t> externIndex_;
    std::optional<SymbolId>                     userEntry_;
    // Data items in SOURCE order, plus the section state each pass tracks. A
    // nullopt section means TEXT — the default a `.s` starts in.
    std::vector<AssembledData>                  dataItems_;
    // D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET: symbol-valued data
    // slots in SOURCE order, and the interior-label block symbols only they
    // name. Both empty for a `.s` whose data holds no addresses.
    std::vector<PendingDataReloc>               pendingDataRelocs_;
    std::vector<AsmBlockSymbolBinding>          blockSymbolBindings_;
    std::optional<DataSectionKind>              scanSection_;
    std::optional<DataSectionKind>              emitSection_;
    std::size_t                                 openDataItem_      = kNoLabel;
    std::uint32_t                               nextSymbolId_      = 1;
    std::size_t                                 functionCount_     = 0;
    std::size_t                                 openFunctionLabel_ = kNoLabel;
    std::size_t                                 openBlockLabel_    = kNoLabel;
    std::uint32_t                               blockInstCount_    = 0;
    bool                                        openTerminated_    = false;
    bool                                        branchProbed_      = false;
    std::optional<std::uint16_t>                branchOpcode_;
    std::string                                 branchProbeNote_;
    bool                                        ok_                = true;
};

} // namespace

std::optional<AsmTextModule>
lowerAsmTextToLir(Tree const& tree, GrammarSchema const& grammar,
                  TargetSchema const& target,
                  std::span<std::string const> entryNames,
                  DiagnosticReporter& reporter) {
    AsmTextLowering lowering{tree, grammar, target, entryNames, reporter};
    return lowering.run();
}

} // namespace dss
