#include "asm/asm_template_to_lir.hpp"

#include "analysis/syntactic/parser.hpp"
#include "asm/asm_variant_elect.hpp"
#include "core/types/assembly_config.hpp"
#include "core/types/source_buffer.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"
#include "tokenizer/tokenizer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

using asm_walk::collectDescendantsOfRule;
using asm_walk::findDescendantOfRule;
using asm_walk::firstVisibleToken;
using asm_walk::lastVisibleToken;
using asm_walk::visibleChildren;

// ── the sink ──────────────────────────────────────────────────────────────

void AsmDiagnosticSink::fail(NodeId at, std::string message) {
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
void AsmDiagnosticSink::warn(NodeId at, std::string message) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::A_AsmTextUnsupported;
    d.severity = DiagnosticSeverity::Warning;
    d.actual   = std::move(message);
    if (at.valid()) d.span = tree_.span(at);
    d.buffer = tree_.source().id();
    reporter_.report(std::move(d));
}

std::string AsmDiagnosticSink::pairSuffix() const {
    return std::format(" (assembly dialect '{}', target '{}')",
                       grammar_.name(), target_.name());
}

AsmLoweringHost::~AsmLoweringHost() = default;

namespace {

// A parsed statement: the mnemonic and its operands, in SOURCE order.
struct AsmDecodedInstruction {
    std::string_view            mnemonic;
    NodeId                      node{};
    std::vector<AsmDecodedOperand> operands;
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

} // namespace

// ── the engine ────────────────────────────────────────────────────────────
//
// ★ THE BODY BELOW IS THE OLD `AsmTextLowering`'s INSTRUCTION HALF, MOVED
// WHOLE. It is deliberately still a class body rather than a set of
// out-of-line `AsmInstructionLowering::` definitions: the extraction had to be
// behaviour-preserving, and re-qualifying ~1,200 lines of member functions is
// exactly the kind of mechanical edit that silently changes one of them. What
// DID change is enumerated and nothing else: state the walker owned
// (`labels_`, `openFunctionLabel_`, `openTerminated_`, `blockInstCount_`,
// `emitSection_`, the register table lookup) is now ASKED of `host_`, and
// `fail`/`pairSuffix`/`ok_` are asked of `sink_`.
struct AsmInstructionLowering::Impl {
    Impl(Tree const& tree, GrammarSchema const& grammar,
         TargetSchema const& target, LirBuilder& builder,
         AsmDiagnosticSink& sink, AsmLoweringHost& host)
        : tree_(tree), grammar_(grammar), target_(target),
          cfg_(grammar.assembly()), builder_(builder), sink_(sink),
          host_(host) {}

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
                    sink_.fail(NodeId{},
                         std::format("mnemonic '{}' declares condition '{}', "
                                     "which is not one of the substrate's "
                                     "condition codes{}",
                                     row.spelling, row.condName, sink_.pairSuffix()));
                    continue;
                }
                if (!target_.condCodeEncoding(*cc).has_value()) {
                    sink_.fail(NodeId{},
                         std::format("mnemonic '{}' declares condition '{}', "
                                     "which this target declares no encoding "
                                     "for — a conditional branch on it would "
                                     "reach the encoder and fail there, after "
                                     "the dialect had already claimed to "
                                     "support the spelling{}",
                                     row.spelling, row.condName, sink_.pairSuffix()));
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
                    sink_.fail(NodeId{},
                         std::format("mnemonic '{}' lists target opcodes '{}' "
                                     "and '{}', only one of which reads a "
                                     "condition code from the instruction "
                                     "payload — whichever way the row declared "
                                     "'cond', one of the two elections would be "
                                     "wrong, and the wrong one is silent (a "
                                     "dropped condition, or a condition "
                                     "defaulted to the target's zero code){}",
                                     row.spelling, consumesFrom, name,
                                     sink_.pairSuffix()));
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
                    sink_.fail(NodeId{},
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
                                     sink_.pairSuffix()));
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
                sink_.fail(NodeId{},
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
                                 sink_.pairSuffix()));
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
                sink_.fail(NodeId{},
                     std::format("mnemonic '{}' declares condition '{}', but its "
                                 "target opcode '{}' declares no encoding "
                                 "variant that reads a condition code from the "
                                 "instruction payload — the condition has "
                                 "nowhere to go and would be silently "
                                 "dropped{}",
                                 row.spelling, row.condName, consumesFrom,
                                 sink_.pairSuffix()));
                continue;
            }
            // Set LAST, so the flag is true only on a row that passed BOTH
            // checks — which is what makes `consumesCond ⟹ cond.has_value()`
            // an invariant the emit walk can rely on rather than a coincidence
            // of the run aborting first.
            out.consumesCond = consumes.value_or(false);
        }
        return sink_.ok();
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
    condCodeOfOperand(AsmDecodedOperand const& op) const {
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
                 AsmDecodedInstruction const& ins) {
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
                sink_.fail(ins.operands[i].node,
                     std::format("'{}' already fixes its condition in the "
                                 "mnemonic (the dialect row declares '{}'), but "
                                 "operand {} also names a condition — one "
                                 "instruction cannot carry two, and the row's "
                                 "would silently win{}",
                                 ins.mnemonic, row.condName, i + 1,
                                 sink_.pairSuffix()));
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
                sink_.fail(ins.operands[i].node,
                     std::format("'{}' names more than one condition in its "
                                 "operands ('{}' and '{}'), and its target "
                                 "opcode reads exactly one from the instruction "
                                 "payload{}",
                                 ins.mnemonic,
                                 targetCondCodeName(*out.cond),
                                 targetCondCodeName(*cc), sink_.pairSuffix()));
                out.ok = false;
                return out;
            }
            out.cond        = cc;
            out.fromOperand = i;
        }
        if (!out.cond.has_value()) {
            sink_.fail(ins.node,
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
                             "does){}", ins.mnemonic, sink_.pairSuffix()));
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
                                           AsmDecodedInstruction const& ins) {
        if (!resolved.consumesCond) return 0;
        if (!cond.cond.has_value()) {
            sink_.fail(ins.node,
                 std::format("internal: '{}' resolves to an opcode that reads a "
                             "condition from the instruction payload, but no "
                             "condition was resolved — the pairing that "
                             "guarantees this did not hold{}",
                             ins.mnemonic, sink_.pairSuffix()));
            return 0;
        }
        return static_cast<std::uint32_t>(*cond.cond);
    }

    // ★★★ THE TEXT AN OPERAND WRITES, BUT ONLY WHEN IT IS A BARE SINGLE TOKEN.
    //
    // A selector is compared against what the programmer wrote at a POSITION,
    // before any role is decoded — so this cannot ask the dialect what the
    // operand MEANS. What it can ask is how many tokens the operand spans, and
    // that single question is what makes the comparison both dialect-neutral
    // and tight: `cntvct_el0` and `eq` are one Identifier each, while `#eq`,
    // `%function`, `-8` and `[x1, #16]` are two or more. Returning empty for
    // the multi-token forms is what keeps `cset x0, #eq` from selecting the
    // `cset`/`eq` row — a line the reference assembler REJECTS (✔MEASURED,
    // `aarch64-linux-gnu-as` 2.42 takes `cset x0, eq` and not `cset x0, #eq`),
    // and one a "last visible token" reading would have silently accepted.
    // ⚠ IT IS NOT `trailingNameOf`, WHICH DELIBERATELY DROPS A LEADING SIGIL.
    // Dropping the sigil is right when the caller has already decided the node
    // is a register or a section name; here nothing has decided anything yet,
    // so a sigil is EVIDENCE that this operand is not a bare selector.
    [[nodiscard]] std::string_view soleTokenTextOf(NodeId node) const {
        std::string_view text;
        std::size_t      count = 0;
        auto walk = [&](auto&& self, NodeId n) -> void {
            if (!n.valid() || count > 1) return;
            if (tree_.kind(n) == NodeKind::Token) {
                text = tree_.text(n);
                ++count;
                return;
            }
            for (NodeId const c : tree_.children(n)) {
                if (isEmptySpace(tree_.flags(c))) continue;
                self(self, c);
            }
        };
        walk(walk, node);
        return count == 1 ? text : std::string_view{};
    }

    // ★★★ WHICH ROW THIS LINE IS, SELECTORS INCLUDED — the ONE match site.
    //
    // A selector is consumed BY THE MATCH: it never becomes an operand, never
    // reaches the target, and is excluded from the `destinationFirst` reading.
    // So `mrs x0, cntvct_el0` presents the lowering with exactly one operand
    // (`x0`, the destination) and ZERO remaining, which is what `cntvct`'s
    // `maxOperands: 0` needs, and `cset x0, eq` likewise for `setcc`.
    //
    // ★★ NOTHING HERE KNOWS A SPELLING. The walk is over the row's DECLARED
    // selector list and the operand nodes as written — there is no branch for
    // `mrs`, none for `cset`, and none for a register name. A dialect declaring
    // a selector at index 0 (gas's `msr tpidr_el0, x0`) reaches the identical
    // code, which is the test §4.7.2(4) sets for whether the KEY is right.
    //
    // ⚠ AT MOST ONE ROW CAN MATCH, AND THAT IS A LOAD-TIME GUARANTEE RATHER
    // THAN A CONVENTION HERE — `asmRowsAreSelectorDisjoint` refuses a document
    // whose rows could both take a line. The `matched` re-check below is the
    // cheap assertion that the guarantee held; it is not a tie-break, because a
    // tie-break is exactly what §4.7.1 forbids.
    struct RowMatch {
        std::optional<std::size_t> index;
        // Set when the SPELLING is declared but no row's selectors accepted the
        // line — the state that proves a selector SELECTS rather than decorates.
        bool        spellingDeclared = false;
        std::string offered;   // the accepted selector texts, for the refusal
        std::string wrote;     // what the line actually put at those positions
    };

    [[nodiscard]] RowMatch
    matchRow(std::string_view spelling,
             std::span<NodeId const> operandNodes) const {
        RowMatch                   m;
        std::vector<std::uint32_t> selectorIndices;
        bool                       twoTook = false;
        for (std::size_t i = 0; i < cfg_.instructions.size(); ++i) {
            auto const& row = cfg_.instructions[i];
            // ⚠ BOTH COMPARISONS IN THIS LOOP ARE `spellingMatches`, AND THEY
            // HAD TO MOVE TOGETHER (D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE).
            // Folding the MNEMONIC while the SELECTOR stayed exact
            // would make `MRS X0, CNTVCT_EL0` fail one token later instead of
            // at the mnemonic — a different diagnostic for the same refusal,
            // which reads as progress and is none. The load-time disjointness
            // refusal asks the same predicate, so a dialect that could reach a
            // tie here never loaded.
            if (!cfg_.spellingMatches(row.spelling, spelling)) continue;
            m.spellingDeclared = true;
            bool fits = true;
            for (auto const& sel : row.operandSelectors) {
                if (std::ranges::find(selectorIndices, sel.index)
                    == selectorIndices.end()) {
                    selectorIndices.push_back(sel.index);
                }
                if (!m.offered.empty()) m.offered += ", ";
                m.offered += std::format("'{}'", sel.name);
                if (sel.index >= operandNodes.size()
                    || !cfg_.spellingMatches(
                           sel.name, soleTokenTextOf(operandNodes[sel.index]))) {
                    fits = false;
                }
            }
            if (!fits) continue;
            // Unreachable while the loader holds. Reported rather than silently
            // resolved, because "two rows took this line" is the exact
            // condition the load-time refusal exists to make impossible, and
            // picking one here would be the tie-break §4.7.1 forbids.
            if (m.index.has_value()) { twoTook = true; break; }
            m.index = i;
        }
        if (twoTook) { m.index.reset(); m.offered = "<ambiguous>"; return m; }
        if (!m.index.has_value()) {
            for (auto const idx : selectorIndices) {
                if (!m.wrote.empty()) m.wrote += ", ";
                m.wrote += std::format(
                    "operand {} is {}", idx,
                    idx < operandNodes.size()
                        ? std::format("'{}'",
                                      soleTokenTextOf(operandNodes[idx]))
                        : std::string{"absent"});
            }
        }
        return m;
    }

    // ── instructions ──────────────────────────────────────────────────────
    void lowerStatement(NodeId statement, NodeId mnemonicNode,
                        NodeId tail) {
        std::string_view const spelling = tree_.text(mnemonicNode);
        // ⚠ THE THREE PLACEMENT GUARDS ASK THE HOST AND KEEP THEIR ORIGINAL
        // POSITIONS IN THIS SEQUENCE. They are about the program AROUND the
        // instruction, so the answers moved; the ORDER did not, and it is
        // observable — an unknown mnemonic inside an open data section reports
        // the section, not the mnemonic, and reversing that would change which
        // diagnostic a `.s` gets for an input with two defects.
        if (auto const section = host_.openDataSectionName();
            section.has_value()) {
            sink_.fail(mnemonicNode,
                 std::format("instruction '{}' appears while the '{}' DATA "
                             "section is open — LIR places code in the text "
                             "section only, so this instruction would be "
                             "emitted as if the data section were code{}",
                             spelling, *section,
                             sink_.pairSuffix()));
            return;
        }
        // ★★★ THE OPERAND NODES ARE GATHERED BEFORE THE ROW IS CHOSEN, because
        // a SELECTOR is part of the match key. `mrs x0, cntvct_el0` and
        // `mrs x0, tpidr_el0` differ only at operand 1, and only one of them is
        // a spelling this dialect declares.
        std::vector<NodeId> operandNodes;
        if (tail.valid()) {   // already resolved to the operand-seq node
            for (NodeId const operandNode : visibleChildren(tree_, tail)) {
                if (tree_.kind(operandNode) != NodeKind::Internal) continue;
                operandNodes.push_back(operandNode);
            }
        }
        auto const match  = matchRow(spelling, operandNodes);
        auto const rowIdx = match.index;
        if (!rowIdx.has_value()) {
            // ★★ THE SELECTOR ARM IS A DIFFERENT REFUSAL FROM THE UNKNOWN-
            // MNEMONIC ONE, AND THE DIFFERENCE IS THE POINT. "`mrs` exists but
            // nothing selects `tpidr_el0`" tells a reader that the spelling is
            // modelled and THIS system register is not — which is true, and is
            // what proves the selector SELECTS rather than being decoration.
            // Collapsing it into "unknown mnemonic 'mrs'" would be false about
            // the dialect and would send the reader to add a row that is there.
            if (match.spellingDeclared) {
                sink_.fail(mnemonicNode,
                     std::format("'{}' is declared with a positional operand "
                                 "SELECTOR, and this line selects none of its "
                                 "rows: the dialect accepts {} and here {}. A "
                                 "selector is part of the mnemonic — it is "
                                 "consumed by the match and never reaches the "
                                 "target — so an unselected spelling is refused "
                                 "rather than lowered as if the operand had not "
                                 "been written{}",
                                 spelling,
                                 match.offered.empty() ? std::string{"no "
                                                                     "selector"}
                                                       : match.offered,
                                 match.wrote.empty() ? std::string{"it is "
                                                                   "absent"}
                                                     : match.wrote,
                                 sink_.pairSuffix()));
                return;
            }
            sink_.fail(mnemonicNode,
                 std::format("unknown mnemonic '{}' — it is not in this "
                             "dialect's instruction table. An undeclared "
                             "spelling is refused rather than guessed at: "
                             "adding it is a config edit, and reusing a "
                             "similarly-named target opcode would silently "
                             "encode a different instruction{}",
                             spelling, sink_.pairSuffix()));
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
            sink_.fail(mnemonicNode,
                 std::format("this dialect maps '{}' to target opcode(s) {}, "
                             "none of which this target declares — the dialect "
                             "and the target disagree about the instruction "
                             "set{}", spelling, names, sink_.pairSuffix()));
            return;
        }
        if (!host_.hasOpenFunction()) {
            sink_.fail(mnemonicNode,
                 std::format("instruction '{}' appears before any function "
                             "entry — there is no function for it to belong "
                             "to{}", spelling, sink_.pairSuffix()));
            return;
        }
        if (host_.blockIsTerminated()) {
            sink_.fail(mnemonicNode,
                 std::format("instruction '{}' follows a terminator with no "
                             "intervening label, so it is unreachable — this "
                             "build refuses to emit code it cannot place in a "
                             "basic block{}", spelling, sink_.pairSuffix()));
            return;
        }

        AsmDecodedInstruction ins;
        ins.mnemonic = spelling;
        ins.node     = statement;
        // ★★★ THE SELECTOR IS CONSUMED HERE AND NOWHERE ELSE. It is skipped
        // before decoding, so it never becomes an `AsmDecodedOperand`, never
        // reaches the width derivation, never reaches the election, and is
        // excluded from the `destinationFirst`/`destinationLast` reading — the
        // destination is read off what REMAINS. That last clause is what makes
        // a selector at index 0 (gas's `msr tpidr_el0, x0`) correct by
        // construction instead of by a second rule: position 0 is simply no
        // longer a candidate for the destination.
        for (std::size_t i = 0; i < operandNodes.size(); ++i) {
            if (row.selectorAt(static_cast<std::uint32_t>(i)) != nullptr) {
                continue;
            }
            auto decoded = decodeOperand(operandNodes[i]);
            if (!decoded) return;
            ins.operands.push_back(std::move(*decoded));
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
    // ★★★ THE DIALECT FOLDS THE SPELLING, THEN THE TARGET IS ASKED — the whole
    // of the register half of D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE,
    // in one function that both register seams call.
    //
    // ★★ WHY THE FOLD IS HERE AND NOT IN `TargetSchema::registerByName`. Case
    // policy is a property of the DIALECT, not of the CPU: one processor can be
    // written in two dialects with different rules, so a case-insensitive
    // register table would put a dialect fact inside the target description and
    // every dialect reading that target would inherit it. This engine is the
    // lowest place that holds the dialect (`cfg_`), so it is the right place —
    // and `registerByName` stays an exact match on target vocabulary.
    //
    // ⚠ THE RESULT IS A LOOKUP KEY, NEVER THE TEXT A DIAGNOSTIC QUOTES. Every
    // refusal below still names what the programmer WROTE; echoing `x0` back at
    // someone who typed `X0` is a small lie that makes a real message unusable.
    [[nodiscard]] std::string registerLookupKey(std::string_view written) const {
        return cfg_.spellingKey(written);
    }

    [[nodiscard]] AsmOperandRole resolveRole(NodeId node,
                                             std::uint8_t mask) const {
        if (AssemblyConfig::maskHas(mask, AsmOperandRole::Register)) {
            auto const name = trailingNameOf(node);
            // ⚠ THE HOST ANSWERS, NOT THE TARGET TABLE DIRECTLY, and that is
            // the ONE line that makes a template operand reach this role at
            // all: an embedded `%0` names no target register, so a direct
            // `registerByName` would send it down the OTHER role's arm and it
            // would be read as a symbol. `namesRegister` is contracted to agree
            // with `resolveRegister` below, so the role decision and the decode
            // cannot disagree.
            // ⚠⚠ BOTH SIDES OF THAT CONTRACT MUST BE ASKED WITH THE **SAME**
            // KEY. Folding at one of the two seams and not the other is how a
            // sigil-less dialect starts reading `X0` as a SYMBOL here and as a
            // register in the decode — a disagreement that produces no
            // diagnostic at all, just the wrong operand.
            if (!name.empty()
                && host_.namesRegister(registerLookupKey(name))) {
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

    // Is this placeholder the dialect's `asm goto` LABEL alternative? Asked of
    // the CST by RuleId and of the config by name — never of the text. An
    // absent `templateLabelRule` matches nothing, which is the honoured-absence
    // arm every rule landmark in this engine already has (`RuleId{}` is the
    // invalid sentinel, so a bare `.v` comparison would match slot 0).
    [[nodiscard]] bool placeholderIsALabelRef(NodeId placeholder) const {
        if (!cfg_.templateLabelRule.valid()) return false;
        for (NodeId const k : visibleChildren(tree_, placeholder)) {
            if (tree_.kind(k) != NodeKind::Internal) continue;
            if (tree_.rule(k).v == cfg_.templateLabelRule.v) return true;
        }
        return false;
    }

    // ★★★ WHICH DECLARED WIDTH VIEW THIS PLACEHOLDER STATES, OR NULLPTR — the
    // third alternative of the placeholder family (`%w0`, `%k[out]`), asked of
    // the CST by RuleId exactly as the label arm is.
    //
    // ★★ THE ROUTING IS BY RULE AND THE **WIDTH** IS BY DECLARED LEXEME, AND THE
    // SPLIT IS DELIBERATE. Which alternative matched is a grammar fact only the
    // dialect can name (`assembly.templateModifierRule`); WHICH view a matched
    // letter selects is a vocabulary fact the dialect declares in
    // `assembly.templateModifiers`, and matching the arm's own first token
    // against that declared table is the same shape `instructions[]` already
    // has — a written spelling compared against DECLARED bytes, never against a
    // byte spelled in C++. So nothing here knows that `w` means 32; it knows
    // only that this dialect said so.
    //
    // ⚠ THE FIRST TOKEN IS THE SIGIL BY THE SHAPE'S OWN DEFINITION
    // (`sequence: [templateModifierPlaceholder, asmTemplateSelector]`), and an
    // arm whose first token is NOT a declared modifier returns nullptr, which
    // the caller turns into a loud refusal rather than into a silent full-width
    // decode.
    [[nodiscard]] AssemblyConfig::AsmTemplateModifier const*
    placeholderWidthView(NodeId placeholder) const {
        if (!cfg_.templateModifierRule.valid()) return nullptr;
        for (NodeId const k : visibleChildren(tree_, placeholder)) {
            if (tree_.kind(k) != NodeKind::Internal) continue;
            if (tree_.rule(k).v != cfg_.templateModifierRule.v) continue;
            for (NodeId const t : visibleChildren(tree_, k)) {
                if (tree_.kind(t) != NodeKind::Token) continue;
                return cfg_.templateModifierByLexeme(tree_.text(t));
            }
            return nullptr;
        }
        return nullptr;
    }

    // Is this placeholder the width-view alternative at all? Separated from the
    // lookup above so "the arm matched" and "the arm's letter is declared" stay
    // two questions: the second failing is a CONFIG defect that must be loud,
    // and folding it into "no view" would decode `%w0` at the operand's own
    // type width with a clean build log.
    [[nodiscard]] bool placeholderIsAWidthView(NodeId placeholder) const {
        if (!cfg_.templateModifierRule.valid()) return false;
        for (NodeId const k : visibleChildren(tree_, placeholder)) {
            if (tree_.kind(k) != NodeKind::Internal) continue;
            if (tree_.rule(k).v == cfg_.templateModifierRule.v) return true;
        }
        return false;
    }

    // ★★★ A TEMPLATE PLACEHOLDER — EVERY FORM THE DIALECT'S
    // `templateOperandRule` COVERS — RESOLVED THROUGH THE HOST BY ITS WRITTEN
    // SPELLING, EXACTLY AS A REGISTER SPELLING IS.
    //
    // ★★ IT PRODUCES A `Register`-ROLE OPERAND AND NOTHING DOWNSTREAM KNOWS THE
    // DIFFERENCE, which is the whole design. An operand placeholder (`%0`,
    // `%[name]` in the shipped dialects) denotes the vreg the embedding language
    // bound to it, so variant election, the width-honesty gate, the two-address
    // legalizer and every encoder see an ordinary register operand and need no
    // placeholder concept at all.
    //
    // ⛔ AND THAT IS **NOT** TRUE OF AN `asm goto` LABEL PLACEHOLDER (`%l[done]`
    // or `%l2`, GNU 6.47.2.7), WHICH TAKES A SECOND ROUTE OUT OF THIS FUNCTION.
    // A label names a BRANCH DESTINATION, not a register, so it is decoded into
    // the shape the branch arms already consume and asked of a DIFFERENT host
    // virtual — `resolveBranchTarget`, never `resolveRegister`. Two questions,
    // two virtuals; see `AsmLabelBinding`'s docblock for why the two binding
    // kinds are two spans rather than one struct with a discriminator.
    //
    // ★★★ WHICH OF THE TWO A PLACEHOLDER IS, IS A **RULE IDENTITY** QUESTION AND
    // NOTHING ELSE. The dialect names the label alternative in
    // `assembly.templateLabelRule`, exactly as it names `templateOperandRule`
    // and `labelTailRule`, and this engine compares RuleIds. ⛔ TESTING THE `%l`
    // BYTES HERE WOULD PUT A SECOND OWNER BESIDE
    // `semantics.inlineAsmTemplateLexemes` — the defect
    // `D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER` closed,
    // and one this file must not reopen. The engine holds no opinion about how
    // a label is spelled; it only compares the reconstructed text against the
    // spellings its caller bound.
    // ★ THE TEST IS ON THE MATCHED **CHILD**, NOT ON A SUBTREE SEARCH, AND THE
    // FAILURE MODES DECIDE IT. The two placeholder families are SIBLING
    // alternatives of the placeholder rule, so the label form is always a direct
    // child when it is present at all; a subtree search would additionally match
    // a rule that both families CONTAIN — the shared bracketed-name rule is the
    // obvious mis-declaration — and every operand placeholder would then be
    // routed as a branch target, a silent miscompile. A child test
    // mis-declared the same way matches nothing, which is loud. (The loader
    // refuses both mis-declarations outright; this is the shape that fails
    // safely if one ever slipped past.)
    // ★ WHAT SURVIVES FROM THE OLD REFUSAL, WHICH THIS PARAGRAPH REPLACES: the
    // blocks around a template belong to the CALLER, and the successor edge has
    // to exist in the caller's CFG before a branch to it can be emitted. That is
    // still true — which is why the block is BOUND by the caller and never
    // invented here, and why a label the caller did not bind is still refused by
    // name.
    //
    // ★★★ WHY THE SPELLING IS REBUILT FROM THE TOKENS RATHER THAN TAKEN AS THE
    // NODE'S SOURCE TEXT OR AS ITS LAST TOKEN. Both obvious readings are wrong,
    // in opposite directions. The LAST TOKEN — which is what `decodeRegister`
    // uses, correctly, because a register's sigil carries no identity — would
    // hand the host `0` for `%0`; the host's binding table is keyed on the
    // spelling the EMBEDDING language mints (`%0`, GNU 6.47.2.3), so `0` matches
    // nothing and every placeholder would be refused as unbound. The NODE'S
    // SOURCE TEXT would carry whatever whitespace the author wrote between the
    // sigil and the selector, so `% 0` and `%0` would be two different keys for
    // one operand. Concatenating the visible token texts is exact for both and
    // keeps this function free of any opinion about what the pieces MEAN — the
    // engine still only COMPARES, and a dialect that numbered its placeholders
    // differently needs no change here.
    //
    // ★★★ THE SPELLING IS **NOT** FOLDED BY THE DIALECT'S `spellingCase`, AND
    // THAT IS THE OPPOSITE CALL FROM EVERY OTHER SPELLING IN THIS ENGINE —
    // deliberately, because it is a different KIND of spelling. `spellingCase`
    // exists because gas is case-insensitive about ITS OWN vocabulary
    // (mnemonics, registers, directives, operand selectors); a placeholder
    // names an operand of the EMBEDDING language and is not gas vocabulary at
    // all. ✔MEASURED: GNU 6.47.2.3 symbolic operand names are ordinary C
    // identifiers and gcc is case-SENSITIVE about them — `%[Out]` does not name
    // an operand declared `[out]`. Folding here would silently merge two
    // distinct operands into one under both shipped dialects, which are
    // `asciiFolded`, and the caller would get whichever of the two the binding
    // table happened to list first.
    // ⚠ THE TWO-SEAM CONTRACT DOES NOT APPLY: that contract binds
    // `namesRegister` and `resolveRegister`, and the role disambiguation is
    // bypassed for a placeholder (the rule already settled what this operand
    // is), so there is no second seam to disagree with.
    // ⚠⚠ BUT THE **NORMALIZATION** CLAUSE OF `resolveRegister`'S CONTRACT IS A
    // DIFFERENT SENTENCE AND IT USED TO SAY THE OPPOSITE OF WHAT THIS DOES.
    // `AsmLoweringHost::namesRegister`'s block promised host authors that "the
    // engine folds before it asks" and that "a binding must be registered in
    // folded form" — written when `decodeRegister` was the only caller, and
    // false for this one from the moment it landed. A host that followed the
    // written instruction and registered `%[out]` for a source-spelled `%[Out]`
    // would MISS in `TemplateHost::bindingFor`, which compares exactly. The
    // contract now states the split (register spellings arrive FOLDED,
    // placeholder spellings arrive VERBATIM) and names this function as the
    // second caller; `SpellingCaseSplitsAtThePlaceholderSeam` in
    // tests/asm/test_asm_template_to_lir.cpp exercises both halves against ONE
    // `asciiFolded` dialect so the two callers cannot drift silently.
    std::optional<AsmDecodedOperand> decodePlaceholder(NodeId node) {
        std::string written;
        auto walk = [&](auto&& self, NodeId n) -> void {
            if (!n.valid()) return;
            if (tree_.kind(n) == NodeKind::Token) {
                written += tree_.text(n);
                return;
            }
            for (NodeId const c : tree_.children(n)) {
                if (isEmptySpace(tree_.flags(c))) continue;
                self(self, c);
            }
        };
        walk(walk, node);
        if (written.empty()) {
            sink_.fail(node,
                 std::format("a template operand placeholder carries no text — "
                             "the dialect's 'templateOperandRule' matched a "
                             "production with no tokens under it{}",
                             sink_.pairSuffix()));
            return std::nullopt;
        }

        AsmDecodedOperand out;
        out.node = node;

        if (placeholderIsALabelRef(node)) {
            // ★★★ THE ONLY SHAPE `branchTarget()`'s GATE ADMITS, SPELLED OUT
            // FIELD BY FIELD RATHER THAN LEFT TO THE MEMBER DEFAULTS — because
            // the gate is what makes the rest of the path work unchanged, and a
            // reader has to be able to check the two against each other without
            // opening a second function. ✔VERIFIED against `branchTarget`: it
            // refuses an operand that is `indirect`, is `isMemory`, or carries
            // an EMPTY `symbol`, and passes everything else straight to
            // `host_.resolveBranchTarget`. So a label placeholder reaches the
            // EXISTING `branchTarget` → `resolveBranchTarget` → `makeBlockRef`
            // → `addBr`/`addCondBr` path — the very path the standalone `.s`
            // tests already prove for `jmp .L1` — with no new branch arm.
            // ★ `Displaced` IS THE ROLE A SYMBOL-VALUED SCALAR ALREADY HAS in
            // this decoder (`jmp foo` in a `.s` decodes to exactly this), which
            // is why it is reused rather than a role being minted: a new role
            // would be a language-private verb for a shape the pipeline already
            // has a verb for.
            // ⚠ AND IT IS ALSO WHAT MAKES A LABEL IN A NON-BRANCH POSITION FAIL
            // LOUD RATHER THAN QUIETLY: `movq %l[done], %0` takes the ordinary
            // instruction path, whose symbol-valued source arm asks the host for
            // the label's ADDRESS, and the template host refuses that by name.
            out.role     = AsmOperandRole::Displaced;
            out.symbol   = std::move(written);
            out.isMemory = false;
            out.indirect = false;
            out.hasValue = false;
            return out;
        }

        out.role = AsmOperandRole::Register;

        // ★★★ A WIDTH VIEW NAMES THE **SAME OPERAND** AS THE PLAIN FORM, SO IT
        // IS ASKED FOR UNDER THE PLAIN FORM AND STATES A WIDTH ON TOP.
        // `%w0` and `%0` are one operand written two ways — a modifier selects a
        // VIEW of the register, never a different register — so minting a second
        // binding row per letter would put spellings in the host's table that
        // the front end never minted, and the tier that VALIDATES a reference
        // would stop agreeing with the tier that BINDS it about which forms
        // exist. Rebuilding the plain spelling instead keeps ONE key per
        // operand: the host's table, the semantic scan's minted inventory and
        // every refusal that quotes it all continue to describe the same set.
        //
        // ⚠ THE REBUILD USES THE **DECLARED** SIGIL, NEVER A BYTE SPELLED HERE.
        // `cfg_.templatePlaceholderLexeme` is what the loader joined from the
        // language's `semantics.inlineAsmTemplateLexemes.templatePlaceholder`,
        // which is the same string the letters were composed with — so the
        // prefix stripped and the prefix restored cannot disagree.
        AssemblyConfig::AsmTemplateModifier const* view = nullptr;
        std::string asked = written;
        if (placeholderIsAWidthView(node)) {
            view = placeholderWidthView(node);
            if (view == nullptr || cfg_.templatePlaceholderLexeme.empty()
                || written.size() < view->lexeme.size()) {
                sink_.fail(node,
                     std::format("'{}' matched this dialect's width-view "
                                 "placeholder rule "
                                 "('assembly.templateModifierRule'), but its "
                                 "sigil is not one of the letters declared in "
                                 "'assembly.templateModifiers' — so there is no "
                                 "width for it to state, and decoding it at the "
                                 "operand's own type width would run the "
                                 "operation at a width the template did not "
                                 "ask for{}", written, sink_.pairSuffix()));
                return std::nullopt;
            }
            asked = cfg_.templatePlaceholderLexeme
                  + written.substr(view->lexeme.size());
        }

        AsmResolvedRegister resolved;
        switch (host_.resolveRegister(asked, node, resolved)) {
        case AsmRegisterLookup::Reported:
            return std::nullopt;
        case AsmRegisterLookup::NotARegister:
            // ⚠ THE EMBEDDED HOST NEVER LANDS HERE — it enumerates its bound
            // set and reports (`Reported`). Reaching this arm means a caller
            // with NO operand table saw a placeholder, i.e. a standalone `.s`
            // parsed in the template lexer mode. That is a caller defect and it
            // must say so, not report "unknown register '%0'", which would send
            // the reader to the target's register list for a token the target
            // could never have declared.
            sink_.fail(node,
                 std::format("'{}' is a TEMPLATE placeholder, and this assembly "
                             "text was lowered by a caller that binds no "
                             "template operands — a placeholder names an operand "
                             "(or an `asm goto` label) of the construct that "
                             "embedded the assembly, so there is nothing here "
                             "for it to denote{}", written, sink_.pairSuffix()));
            return std::nullopt;
        case AsmRegisterLookup::Resolved:
            break;
        }

        // ★★★ A MEMORY-BOUND PLACEHOLDER DENOTES THE **MEMORY AT** THE
        // REGISTER, NOT THE REGISTER — AND IT REACHES THE EXISTING MEMORY PATH
        // RATHER THAN A NEW ONE (D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-BEING-DECLARED).
        //
        // The host bound `%1` to a register holding the operand's ADDRESS and
        // said so with `operandKind == membase`. Filling the memory fields here
        // — base + scale + displacement, the same triple `8(%rdi)` and
        // `[x29, #-8]` decode to — hands the operand to `appendMemory` and the
        // `[Reg, MemBase, MemOffset]` encoding-variant guards both shipped
        // targets already declare. ⇒ the DIALECT decides how it is PRINTED and
        // the TARGET decides how it is ENCODED; this function decides neither,
        // which is why `(%rdi)` and `[x0]` need no arm anywhere.
        //
        // ★ SCALE 1 AND DISPLACEMENT 0 ARE THE FORM'S DEFINITION, NOT A DEFAULT
        // TO BE REVISITED: the constraint bound ONE address and nothing else. A
        // dialect that lets a template write an offset around a placeholder
        // (`[%1, #8]`) is the nested-placeholder shape `decodeOperand`'s own
        // descent comment refuses today, and it would arrive through the roled
        // memory decode, not through here.
        //
        // ⚠ EVERY FORM THIS ENGINE CANNOT SHAPE FAILS LOUD RATHER THAN DECAYING
        // TO A REGISTER, and the register arm is the plausible wrong answer
        // precisely because it always assembles. ✔MEASURED while the immediate
        // form was unrealized: a `"i"(7)` operand that fell through to the
        // register arm would emit the register HOLDING 7 where the template
        // asked for the literal — same mnemonic, different instruction, no
        // diagnostic. The immediate arm below now shapes that case; the arms
        // after it still refuse.
        // ⚠⚠ A WIDTH VIEW ON A NON-REGISTER BINDING IS **CARRIED NOWHERE**, AND
        // THAT IS THE MEASURED ANSWER RATHER THAN A SHRUG. ✔MEASURED 2026-08-24,
        // gcc 13.3.0 both ports, `-O0` and `-O2`, each shape compiled with and
        // without the letter and the two outputs compared: `"m"`-bound
        // `__asm__("str %w1, %w0" : "=m"(*p) : "r"(v))` emits `str w1, [x0]` —
        // byte-identical to the unmodified `%0` — and x86 `"i"`-bound `%k1`
        // emits `movl $7, %eax`, likewise identical. The reference ACCEPTS the
        // form and the letter changes nothing, because a memory operand carries
        // an ADDRESS and an immediate IS its value: neither states an operation
        // width. Refusing here would refuse what both references accept, and
        // "applying" the width would invent a fact neither has. ⇒ the two arms
        // below are reached unchanged, and the width dies with the view — which
        // is also SAFE by construction, since `dataRegisterWidth` excludes
        // memory and immediate operands from the width reconciliation anyway.
        //
        // ★★★ A **CLASS-SCOPED** LETTER MUST ALSO MATCH THE OPERAND'S REGISTER
        // CLASS, AND A MISMATCH REFUSES BY NAME — P50, the R8 arm of
        // D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE, and the
        // check runs BEFORE the form switch because the class is a property of
        // the BINDING, not of the form the template writes around it.
        //
        // ✔MEASURED per reference SEPARATELY (gcc 13.3.0, clang 18.1.3 with
        // `-fno-integrated-as`, `-O0` AND `-O2`), and the three arms below are
        // the three measured shapes:
        //   * Reg: an FP view letter on an `"r"`-bound integer is a hard error
        //     under BOTH references (gcc: *invalid 'asm': incompatible floating
        //     point / vector register operand for '%d'*; clang: *invalid
        //     operand in inline asm*) — a unanimous refusal this arm adopts.
        //     The REVERSE (`%x0` on a `"w"`-bound double) is accepted by both
        //     but MEANS different registers — gcc renders `v0`, clang `d0`,
        //     both rc=0 — and the operator ruled that construct DISSOLVED
        //     rather than sided with: under class-scoped letters it is illegal,
        //     and the divergence disappears with the program that exhibited it.
        //   * MemBase: the letter still dies (the arm below), but the CLASS
        //     check applies to the ADDRESS register the binding carries —
        //     ✔MEASURED, `%d0` on an `"m"`-bound double is the SAME hard error
        //     under both references as the `"r"` case, while a GPR letter on
        //     the same binding is accepted-and-ignored. One rule, both
        //     measured behaviours: the binding's class decides.
        //   * ImmInt: SKIPPED. ✔MEASURED, the references SPLIT — gcc refuses
        //     `%d0` on `"i"(7)`, clang renders `7` — and acceptance is decided
        //     by the disjunction, so the letter dies exactly as an unscoped
        //     one does (both references accept-and-ignore a GPR letter there).
        //
        // ⚠ AN EMPTY `registerClass` IS THE WIDTH-ONLY POSTURE (the x86-64
        // document's, where gcc ACCEPTS a GPR letter on an xmm operand and
        // renders the bare form) and skips this check entirely — the loader
        // guarantees a dialect is all-scoped or all-unscoped, so the skip is
        // per DOCUMENT in effect, never per row.
        if (view != nullptr && !view->registerClass.empty()
            && resolved.operandKind != OperandKindFilter::ImmInt) {
            auto const scopedTo = targetRegClassFromName(view->registerClass);
            if (!scopedTo.has_value()) {
                // The loader validates the name at load; reaching this arm
                // means a hand-built `AssemblyConfig` bypassed it. Refuse —
                // treating it as width-only would apply the letter to a class
                // its document never granted.
                sink_.fail(node,
                     std::format("width-view letter '{}' is scoped to register "
                                 "class '{}', which is not a class of the "
                                 "envelope — this dialect's configuration was "
                                 "not produced by the loader that validates "
                                 "it{}",
                                 view->letter, view->registerClass,
                                 sink_.pairSuffix()));
                return std::nullopt;
            }
            auto const operandClass =
                static_cast<TargetRegClass>(resolved.regClass);
            if (operandClass != *scopedTo) {
                std::string declared;
                for (auto const& m : cfg_.templateModifiers) {
                    auto const mc = targetRegClassFromName(m.registerClass);
                    if (!mc.has_value() || *mc != operandClass) continue;
                    if (!declared.empty()) declared += ", ";
                    declared += std::format("'{}' ({} bits)", m.letter,
                                            m.widthBits);
                }
                sink_.fail(node,
                     std::format("'{}' selects a width view through letter "
                                 "'{}', which this dialect declares for class "
                                 "'{}' — but the operand it names lives in "
                                 "class '{}'. {}. A view letter names a view "
                                 "of a register FILE, and no reference gives "
                                 "the mismatch one meaning: an FP letter on an "
                                 "integer operand is a hard error under both "
                                 "gcc and clang, and a GPR letter on an FP "
                                 "operand is rendered as a DIFFERENT register "
                                 "by each — so the construct is refused rather "
                                 "than given whichever meaning one of them "
                                 "picked{}",
                                 written, view->letter, view->registerClass,
                                 targetRegClassName(operandClass),
                                 declared.empty()
                                     ? std::format(
                                           "This dialect declares no view "
                                           "letters for '{}' at all",
                                           targetRegClassName(operandClass))
                                     : std::format(
                                           "The letters declared for '{}' are "
                                           "{}",
                                           targetRegClassName(operandClass),
                                           declared),
                                 sink_.pairSuffix()));
                return std::nullopt;
            }
        }
        switch (resolved.operandKind) {
            case OperandKindFilter::Reg:
                break;
            case OperandKindFilter::MemBase:
                out.role     = AsmOperandRole::Memory;
                out.isMemory = true;
                out.baseReg  = resolved.reg;
                out.hasIndex = false;
                out.scale    = 1;
                out.disp     = 0;
                return out;
            // ★★★ AN IMMEDIATE-BOUND PLACEHOLDER DENOTES A **NUMBER**, AND IT
            // REACHES THE ROLE THE DECODER ALREADY HAS
            // (D-ASM-IMMEDIATE-CONSTRAINT-FORM-NOT-REALIZED). `AsmOperandRole::
            // Immediate` plus `hasValue`/`value` is exactly the shape a
            // `.s`-written `add x0, x0, #5` decodes to, so a `"i"`-bound
            // placeholder joins the EXISTING scalar-source path — the one that
            // range-checks against the 32-bit LIR immediate slot and emits
            // `LirOperand::makeImmInt32` — with no new role and no new operand
            // kind. ⇒ the DIALECT decides how it would have been PRINTED and the
            // TARGET decides how it is ENCODED (the `[Reg, ImmInt]` variants both
            // shipped targets declare); this function decides neither, which is
            // why `$7` and `7` need no arm anywhere.
            // ⚠ THE HOST OWES THE VALUE, AND ITS ABSENCE IS A REFUSAL RATHER
            // THAN A ZERO. `hasImmediate == false` on an immediate binding means
            // the caller named the form and handed over nothing; encoding the
            // default `0` would be a silent wrong answer in the one place where
            // the operand IS its value.
            case OperandKindFilter::ImmInt:
                if (!resolved.hasImmediate) {
                    sink_.fail(node,
                         std::format("'{}' is bound to the operand form '{}', "
                                     "but the binding carries no value — an "
                                     "immediate operand IS its value, so there "
                                     "is nothing here to encode{}",
                                     written,
                                     operandKindFilterName(
                                         OperandKindFilter::ImmInt),
                                     sink_.pairSuffix()));
                    return std::nullopt;
                }
                out.role     = AsmOperandRole::Immediate;
                out.hasValue = true;
                out.value    = resolved.value;
                out.isMemory = false;
                out.indirect = false;
                return out;
            case OperandKindFilter::SymbolRef:
            case OperandKindFilter::MemOffset:
            case OperandKindFilter::BlockRef:
            case OperandKindFilter::LiteralIndex:
                sink_.fail(node,
                     std::format("'{}' is bound to the operand form '{}', which "
                                 "this engine does not shape — only '{}' (the "
                                 "register itself), '{}' (the memory at the "
                                 "register) and '{}' (a compile-time constant) "
                                 "have a template shape{}",
                                 written,
                                 operandKindFilterName(resolved.operandKind),
                                 operandKindFilterName(OperandKindFilter::Reg),
                                 operandKindFilterName(OperandKindFilter::MemBase),
                                 operandKindFilterName(OperandKindFilter::ImmInt),
                                 sink_.pairSuffix()));
                return std::nullopt;
        }
        out.regSpelling  = std::move(written);
        // ★★★ THE VIEW'S WIDTH REPLACES THE OPERAND'S OWN, AND THAT ONE LINE IS
        // THE WHOLE FEATURE. `regWidthBits` is already what `dataRegisterWidth`
        // reconciles into the instruction's operation width and what the
        // encoders read, so a 32-bit view reaches exactly the path a
        // `.s`-written `mov w0, w1` proves — no new concept, no `%w` anywhere
        // below this line. ⚠ AND THE WIDTH-HONESTY GATE STILL APPLIES: a
        // template mixing views (`add %w0, %w1, %2`) is refused by name, which
        // is STRICTER than gcc — ✔MEASURED 2026-08-24, gcc 13.3.0 substitutes
        // textually and happily emits `movl %ax, %ax`, which its own assembler
        // then rejects. Refusing at the compiler is the same answer one stage
        // earlier.
        out.regWidthBits = view != nullptr ? view->widthBits
                                           : resolved.widthBits;
        out.reg          = resolved.reg;
        out.regClass     = resolved.regClass;
        return out;
    }

    // Decode one operand node into a role + payload.
    std::optional<AsmDecodedOperand> decodeOperand(NodeId node) {
        // The bound `attOperand`-style alt node wraps the chosen form; descend
        // to the first node whose rule the dialect bound to a role.
        NodeId cur = node;
        std::uint8_t mask = cfg_.rolesForRule(tree_.rule(cur));
        while (mask == 0) {
            // ★ THE PLACEHOLDER IS TESTED BY RULE, NEVER BY TOKEN, AND ONLY AT
            // THE OPERAND'S OWN POSITION. The dialect names the rule
            // (`assembly.templateOperandRule`); this engine never spells
            // `asmTemplateOperand` or `%`. And there is no `if (embedded)`
            // hiding here: the rule can only match when the dialect's TEMPLATE
            // LEXER MODE minted its sigil, so a standalone `.s` never reaches
            // this branch — the surfaces are separated in the lexer, which is
            // where they actually differ.
            //
            // ⚠ IT SITS INSIDE THE UNROLED DESCENT RATHER THAN AS A SUBTREE
            // SEARCH ABOVE IT, AND THE DIFFERENCE IS NOT COSMETIC. A
            // `findDescendantOfRule` over the whole operand would also match a
            // placeholder nested INSIDE a roled form — the day a dialect
            // declares one in its memory production (`[%0, #8]` is the obvious
            // next ask), the whole memory operand would decode as a bare
            // register and the base/offset would vanish silently. Walking down
            // only through wrappers the dialect bound to NO role means this
            // branch fires exactly where an operand begins, and a nested
            // placeholder stays the roled decode's business — which today
            // refuses it, loudly, which is the correct answer until a dialect
            // declares that form.
            if (cfg_.templateOperandRule.valid()
                && tree_.rule(cur).v == cfg_.templateOperandRule.v) {
                return decodePlaceholder(cur);
            }
            auto const kids = visibleChildren(tree_, cur);
            NodeId next{};
            for (NodeId const k : kids) {
                if (tree_.kind(k) == NodeKind::Internal) { next = k; break; }
            }
            if (!next.valid()) {
                sink_.fail(node, std::format("operand shape is not one this dialect "
                                       "binds to an operand role{}",
                                       sink_.pairSuffix()));
                return std::nullopt;
            }
            cur  = next;
            mask = cfg_.rolesForRule(tree_.rule(cur));
        }
        AsmOperandRole const role = resolveRole(cur, mask);

        AsmDecodedOperand out;
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
                    sink_.fail(cur,
                         std::format("displacement '{}' is a symbol, and a "
                                     "symbol-relative memory operand needs a "
                                     "relocation this build does not reach from "
                                     "assembly yet{}", out.symbol,
                                     sink_.pairSuffix()));
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
                    sink_.fail(cur,
                         std::format("this operand carries TWO displacements "
                                     "({} outside the memory form and {} inside "
                                     "it) and LIR addresses model exactly one{}",
                                     disp, out.disp, sink_.pairSuffix()));
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
                sink_.fail(cur, std::format("an indirect operand needs a target{}",
                                      sink_.pairSuffix()));
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
        sink_.fail(cur, "unhandled operand role");
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
    std::optional<std::uint32_t> dataRegisterWidth(AsmDecodedInstruction const& ins) {
        std::optional<std::uint32_t> width;
        AsmDecodedOperand const*        first = nullptr;
        for (auto const& op : ins.operands) {
            if (op.role != AsmOperandRole::Register) continue;
            if (op.isMemory || op.indirect) continue;
            if (!width.has_value()) { width = op.regWidthBits; first = &op; continue; }
            if (*width == op.regWidthBits) continue;
            sink_.fail(op.node,
                 std::format("register '{}' is {} bits wide but register '{}' is "
                             "{} bits — one instruction cannot operate on both, "
                             "and encoding either width would silently be the "
                             "other instruction{}",
                             first->regSpelling, *width, op.regSpelling,
                             op.regWidthBits, sink_.pairSuffix()));
            return std::nullopt;
        }
        return width;
    }

    // ★ THE OPERAND POSITION THIS DIALECT CALLS THE DESTINATION. The SAME rule
    // `buildLirInst` uses, named once so the width check and the operand
    // partition cannot drift — a role-keyed width check reading a different
    // position from the one that becomes the destination would police the
    // wrong register, quietly.
    [[nodiscard]] std::size_t destinationIndex(std::size_t n) const noexcept {
        return cfg_.operandOrder == AsmOperandOrder::DestinationLast ? n - 1 : 0;
    }

    // ★★★ THE TWO-WIDTH CHECK — D-ASM-X86-WIDTH-EXTENDING-MOVES-UNSPELLABLE.
    //
    // A width-EXTENDING move (`movzbl`, `movswq`, `movslq`, AArch64 `sxtb`)
    // is a SOURCE width and a DESTINATION width in one instruction, which the
    // single-width model above cannot express: `dataRegisterWidth` requires
    // every data register to agree and refuses `movzbl %cl, %ecx` outright.
    //
    // ⚠ THE FIX IS NOT TO RELAX THAT CHECK, and this is the whole design.
    // That check is what refuses `movl %rax, %ecx` — which GNU as 2.42 also
    // refuses (✔MEASURED: `operand type mismatch`) — so deleting or widening
    // it globally would trade one conformance gap for its mirror image, and
    // the mirror image is the dangerous direction (accepting a spelling no
    // reference accepts means silently encoding SOMETHING for it). Instead a
    // row that HAS two widths SAYS SO, and the check splits by ROLE while
    // staying exactly as strict on each side:
    //
    //   * the destination-position register must be exactly `destWidth`;
    //   * every source register must be exactly `width`.
    //
    // ⇒ `movzbl %cl, %ecx` is accepted and `movzbl %cl, %rcx` is refused,
    // which is gas's own answer to both. A memory operand never participates
    // (it carries an ADDRESS, not the operation width) — the same exclusion
    // `dataRegisterWidth` documents, which is what lets `movzbl 8(%r15), %ecx`
    // work.
    //
    // TARGET-NEUTRAL BY CONSTRUCTION: nothing here reads a mnemonic, an
    // architecture or a format. A dialect whose extending move spells its two
    // widths in the REGISTERS rather than the mnemonic declares the same two
    // numbers on its own row and this function is unchanged.
    bool checkRoleKeyedWidths(AsmDecodedInstruction const& ins,
                              AsmInstructionSpelling const& row) {
        std::size_t const n = ins.operands.size();
        if (n == 0) {
            sink_.fail(ins.node,
                 std::format("'{}' declares a destination width of {} bits but "
                             "was written with no operands — there is no "
                             "destination to check it against{}",
                             ins.mnemonic, *row.destWidth, sink_.pairSuffix()));
            return false;
        }
        std::size_t const di = destinationIndex(n);
        for (std::size_t i = 0; i < n; ++i) {
            auto const& op = ins.operands[i];
            if (op.role != AsmOperandRole::Register) continue;
            if (op.isMemory || op.indirect) continue;
            std::uint32_t const want =
                (i == di) ? *row.destWidth : *row.width;
            if (op.regWidthBits == want) continue;
            sink_.fail(op.node,
                 std::format("'{}' widens a {}-bit value into a {}-bit "
                             "destination, but its {} register '{}' is {} bits "
                             "— the spelling and the register disagree, and "
                             "encoding either width would silently be a "
                             "different instruction{}",
                             ins.mnemonic, *row.width, *row.destWidth,
                             i == di ? "destination" : "source",
                             op.regSpelling, op.regWidthBits,
                             sink_.pairSuffix()));
            return false;
        }
        return true;
    }

    // The width this instruction actually operates on, reconciling what the
    // dialect DECLARED (a mnemonic suffix) with what the operands SAY (register
    // widths). Either source may be absent; when both are present they must
    // agree.
    std::optional<std::uint32_t> effectiveWidth(AsmDecodedInstruction const& ins,
                                                AsmInstructionSpelling const& row,
                                                bool consultOperands) {
        // ★ THE TWO-WIDTH ROW SHORT-CIRCUITS, because the single-width
        // reconciliation below is FALSE about it by construction: its
        // registers are SUPPOSED to disagree. The operation width it returns
        // is the SOURCE width — the number the LIR instruction carries and the
        // target's encoding guard is keyed on; the destination width is spent
        // entirely on the check above, because an instruction that writes a
        // different width is a different opcode on the target side.
        if (row.destWidth.has_value()) {
            if (consultOperands && !checkRoleKeyedWidths(ins, row)) {
                return std::nullopt;
            }
            return row.width;
        }
        std::optional<std::uint32_t> derived;
        if (consultOperands) {
            derived = dataRegisterWidth(ins);
            if (!sink_.ok()) return std::nullopt;
        }
        if (row.width.has_value() && derived.has_value()) {
            if (*row.width != *derived) {
                sink_.fail(ins.node,
                     std::format("'{}' declares operand width {}, but its "
                                 "register operands are {} bits — the spelling "
                                 "and the registers disagree, and encoding "
                                 "either one would silently be the other "
                                 "instruction{}", ins.mnemonic, *row.width,
                                 *derived, sink_.pairSuffix()));
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

    std::optional<AsmDecodedOperand> decodeRegister(NodeId node,
                                                 AsmDecodedOperand out) {
        // The last visible TOKEN child is the register name; the sigil is the
        // dialect's and carries no identity.
        std::string_view name;
        for (NodeId const k : visibleChildren(tree_, node)) {
            if (tree_.kind(k) == NodeKind::Token) name = tree_.text(k);
        }
        // ★★★ THE ONE SEAM. Everything above and below this call is identical
        // for both callers; what a register-role SPELLING denotes is the only
        // thing that differs, and it is asked of the host rather than looked
        // up here. The standalone host answers with the target's own ordinal
        // (`resolvePhysicalRegister`, which is this function's former body
        // moved whole, `subOf` walk included); the template host answers with
        // the VREG its caller bound to that spelling, and falls through to the
        // very same physical lookup for a spelling it did not bind — which is
        // what keeps `__asm__("xorl %eax, %eax")` working alongside `%0`.
        // ⚠ THE LOOKUP USES THE DIALECT-FOLDED KEY; `regSpelling` below keeps
        // the text AS WRITTEN, because the width diagnostic quotes it back.
        AsmResolvedRegister resolved;
        switch (host_.resolveRegister(registerLookupKey(name), node,
                                      resolved)) {
        case AsmRegisterLookup::Reported:
            return std::nullopt;
        case AsmRegisterLookup::NotARegister:
            sink_.fail(node,
                 std::format("unknown register '{}' — this target declares no "
                             "register by that name{}", name, sink_.pairSuffix()));
            return std::nullopt;
        case AsmRegisterLookup::Resolved:
            break;
        }
        out.regSpelling  = std::string{name};
        out.regWidthBits = resolved.widthBits;
        out.reg          = resolved.reg;
        out.regClass     = resolved.regClass;
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
    bool decodeMemory(NodeId memory, AsmDecodedOperand& out) {
        std::vector<NodeId> regs;
        collectDescendantsOfRule(
            tree_, memory, cfg_.ruleForRole(AsmOperandRole::Register), regs);
        if (regs.empty()) {
            sink_.fail(memory,
                 std::format("this memory operand names no base register, and "
                             "an absolute address needs a relocation this build "
                             "does not reach from assembly yet{}",
                             sink_.pairSuffix()));
            return false;
        }
        if (regs.size() > 2) {
            sink_.fail(memory,
                 std::format("this memory operand names {} registers; LIR "
                             "addresses model a base and at most one index{}",
                             regs.size(), sink_.pairSuffix()));
            return false;
        }
        AsmDecodedOperand base;
        auto const baseDecoded = decodeRegister(regs[0], base);
        if (!baseDecoded) return false;
        out.isMemory = true;
        out.baseReg  = baseDecoded->reg;
        if (regs.size() == 2) {
            AsmDecodedOperand index;
            auto const indexDecoded = decodeRegister(regs[1], index);
            if (!indexDecoded) return false;
            out.hasIndex = true;
            out.indexReg = indexDecoded->reg;
        }
        // The displacement, when this dialect nests one inside the memory form.
        NodeId const innerImm =
            findDescendantOfRule(tree_, memory,
                                 cfg_.ruleForRole(AsmOperandRole::Immediate));
        if (innerImm.valid()) {
            AsmDecodedOperand disp;
            NodeId const   scalar =
                findDescendantOfRule(tree_, innerImm,
                                     cfg_.ruleForRole(AsmOperandRole::Scalar));
            if (!decodeScalar(scalar.valid() ? scalar : innerImm, disp)) {
                return false;
            }
            if (!disp.hasValue) {
                sink_.fail(innerImm,
                     std::format("memory displacement '{}' is a symbol, and a "
                                 "symbol-relative memory operand needs a "
                                 "relocation this build does not reach from "
                                 "assembly yet{}", disp.symbol, sink_.pairSuffix()));
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
            sink_.fail(memory,
                 std::format("this memory operand carries {} numeric fields; "
                             "LIR addresses model exactly one scale factor{}",
                             numerics.size(), sink_.pairSuffix()));
            return false;
        }
        if (numerics.size() == 1) {
            std::int64_t v = 0;
            if (!parseInteger(numerics[0], v) || v < 1 || v > 8
                || (v & (v - 1)) != 0) {
                sink_.fail(memory,
                     std::format("'{}' is not an index scale this build can "
                                 "encode — LIR's memory scale is 1, 2, 4 or 8{}",
                                 numerics[0], sink_.pairSuffix()));
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
            sink_.fail(at,
                 std::format("displacement {} does not fit the 32-bit "
                             "displacement slot LIR carries{}", v,
                             sink_.pairSuffix()));
            return false;
        }
        return true;
    }

    bool decodeScalar(NodeId node, AsmDecodedOperand& out) {
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
            sink_.fail(node, "could not read the operand's value");
            return false;
        }
        // A leading digit means a number; anything else is a symbol.
        if (text.front() >= '0' && text.front() <= '9') {
            std::int64_t v = 0;
            if (!parseInteger(text, v)) {
                sink_.fail(node, std::format("'{}' is not a value this build can read",
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
    void buildLirInst(AsmDecodedInstruction const& insIn,
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
        AsmDecodedInstruction stripped;
        if (cond.fromOperand != static_cast<std::size_t>(-1)) {
            stripped.mnemonic = insIn.mnemonic;
            stripped.node     = insIn.node;
            for (std::size_t i = 0; i < insIn.operands.size(); ++i) {
                if (i == cond.fromOperand) continue;
                stripped.operands.push_back(insIn.operands[i]);
            }
        }
        AsmDecodedInstruction const& ins =
            cond.fromOperand != static_cast<std::size_t>(-1) ? stripped : insIn;

        // ⚠ A CONTROL-FLOW INSTRUCTION'S REGISTER OPERAND IS AN ADDRESS, NOT
        // DATA (`call *%rax`, `br x16`), so its width says nothing about the
        // operation and must not be derived from.
        bool const dataOperands = *cfClass == CfClass::Plain;
        auto const width = effectiveWidth(ins, row, dataOperands);
        if (!width.has_value()) return;

        // ★★★ THIS REFUSAL USED TO SAY *"which LIR does not model (8, 16, 32 or
        // 64)"* AND THAT SENTENCE WAS FALSE — corrected 2026-08-29 (cycle P45).
        // LIR models 128: `kLirInstFlagWidth128` is declared in `lir_node.hpp`
        // and `lirInstWidthBits` already decodes it. The limit is HERE, in this
        // translator's flag mapping, which has no 128 arm.
        //
        // ★★ THE COST OF THE LIE WAS MEASURED RATHER THAN IMAGINED: reading it
        // back, two readers in one cycle mis-scoped a one-case gap in this
        // switch as a missing width model one tier down, and nearly opened an
        // arc against the wrong file. A fail-loud message that misplaces its own
        // limit sends every future reader to the wrong tier, which is a worse
        // failure than the gap it reports.
        //
        // ⚠ AND THE ARM IS DELIBERATELY STILL ABSENT, WHICH IS WHY THIS IS A
        // MESSAGE FIX AND NOT A `case 128:`. `kLirInstFlagWidth128`'s own
        // comment says it is DECLARED, NOT STAMPED — no shipped lowering sets
        // it and no shipped target declares an encoding variant guarded on
        // width 128, so stamping the flag here would elect nothing and hand the
        // encoder a width no variant matches. The arm belongs in the same
        // change as the first opcode that consumes it; adding it now would be a
        // mechanism with no consumer, which is the shape this project refuses.
        std::uint8_t flags = 0;
        switch (*width) {
        case 8:  flags = kLirInstFlagWidth8;  break;
        case 16: flags = kLirInstFlagWidth16; break;
        case 32: flags = kLirInstFlagWidth32; break;
        case 64: flags = 0;                   break;
        default:
            sink_.fail(ins.node,
                 std::format("'{}' operates on {} bits, and this assembly "
                             "TEMPLATE lowering elects no variant at that "
                             "width — it maps 8, 16, 32 and 64. The limit is "
                             "this translator's, NOT the instruction model's: "
                             "LIR carries 128 (`kLirInstFlagWidth128`, decoded "
                             "by `lirInstWidthBits`), but the flag is declared "
                             "and never stamped, because no shipped target "
                             "declares an encoding variant guarded on width "
                             "128 for a template to elect. This refuses rather "
                             "than narrowing to 64, which would run the "
                             "operation on half the register with nothing in "
                             "the build log{}",
                             ins.mnemonic, *width, sink_.pairSuffix()));
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
        if (!sink_.ok()) return;

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
        // ★★★ THIS ARM IS A REFUSAL, AND IT REPLACES A SILENT FALLTHROUGH — NOT
        // A WARNING. Ported here during the rebase onto AP6, which added it to
        // the dispatch's PREVIOUS home (`asm_text_to_lir.cpp`) while this cycle
        // MOVED the dispatch here. Taking either side of that conflict whole
        // would have dropped it: "ours" deletes the file region it lived in,
        // "theirs" resurrects an engine this refactor removed. ✔MEASURED at the
        // moment of porting — this dispatch had arms for Return/Br/CondBr/Call/
        // IndirectBr/Unreachable/Plain and NO Switch, so the refactor carried
        // the very hole AP6 closed.
        // WITHOUT IT, a switch-classed instruction falls straight out of this
        // dispatch into the PLAIN data path below — destination/source
        // partition, then election over `candidatesForClass(row, Switch)` — with
        // two outcomes, both wrong and neither loud about the real cause:
        //   * a target whose switch opcode declares a variant matching the
        //     written shape ELECTS, and the builder appends a NON-TERMINATOR —
        //     the multi-way dispatch is lowered to an ordinary instruction and
        //     the block is left unterminated. A silent miscompile: control flow
        //     the programmer wrote, gone, rc=0.
        //   * a target whose variants do not match is refused as "no candidate
        //     target opcode encodes that shape", blaming the OPERANDS for what
        //     is really an unimplemented control-flow class.
        // ⚠ REACHABLE FROM CONFIG, not merely in theory: `terminatorKind:
        // "switch"` is a validated `.target.json` value (pinned by
        // `tests/core/test_target_schema.cpp`) and `cfClassOf` maps it here.
        // Nothing between the loader and this point filters it. No test caught
        // it because NEITHER SHIPPED TARGET declares such an opcode today — a
        // right-by-coincidence-of-the-current-target-table shape.
        // ★ WHY REFUSE RATHER THAN LOWER: there is nothing to lower TO. LIR's
        // terminators are br / cond-br / indirect-br / return / unreachable;
        // `addSwitch` exists on the MIR builder ONLY. A build that cannot
        // represent the construct must say so, in one message that names it.
        case CfClass::Switch:
            sink_.fail(ins.node,
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
                             sink_.pairSuffix()));
            return;
        case CfClass::Unreachable:
            sink_.fail(ins.node,
                 std::format("'{}' is {}, which this build does not lower from "
                             "assembly text: an unreachable trap written by hand "
                             "asserts a claim about control flow that the rest "
                             "of the file cannot be checked against{}",
                             ins.mnemonic, cfClassName(*cfClass),
                             sink_.pairSuffix()));
            return;
        case CfClass::Plain:       break;
        }

        // ★★ THE `result` PARTITION IS WHAT SPLITS `load` FROM `store`, AND IT
        // IS THE TARGET-SIDE READING OF "WHICH SIDE THE MEMORY OPERAND WAS
        // WRITTEN ON". The dialect fact is `dest.isMemory` (its `operandOrder`
        // times the memory operand's position); the target fact is that an
        // instruction writing to memory produces no value. Neither side needed
        // a new knob.
        // ⚠ COMPUTED BEFORE THE ZERO-OPERAND ARM BECAUSE THAT ARM NEEDS IT TOO
        // — see SHAPE 0.
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

        std::size_t const n = ins.operands.size();
        // ── SHAPE 0: a ZERO-OPERAND instruction — `nop`, `rdtsc`, and every
        // system-register read a target spells as a fixed word.
        //
        // ★★★ D-ASM-ZERO-OPERAND-PLAIN-INSTRUCTION-UNLOWERABLE, CLOSED. This
        // guard used to be an unconditional REFUSAL, and it was not about
        // `nop`: `ret` reaches the bytes only because a TERMINATOR is
        // dispatched by `CfClass` above, so EVERY zero-operand non-terminator
        // on EVERY target was unreachable. Both shipped targets declare a `nop`
        // whose encoder was pinned by its measured bytes and which no source
        // language could emit — a declared-but-unreachable opcode, which is the
        // shape a capability gap takes when nothing asks for it.
        //
        // ★★ IT ELECTS AMONG THE **CONSUMERS** ONLY, AND THAT IS THE WHOLE
        // CORRECTNESS ARGUMENT. An empty operand list is exactly the
        // `guard.operandKinds: []` these rows declare, so the election is the
        // ordinary one. But a zero-operand PRODUCER (arm64's `cntvct`, whose
        // variant carries `resultSlot: rd`) has nowhere to put its result when
        // the source wrote no destination — electing it would emit
        // `addInst(op, InvalidLirReg, …)` and the encoder would place the
        // result register field from an INVALID register. Withholding the
        // producers means such a row falls through to `reportNoShape`, naming
        // the candidate and saying the shape does not fit, instead of encoding
        // a wrong register with no diagnostic.
        if (n == 0) {
            // A zero-operand instruction names no memory reference, so the
            // memory-direction axis is `false` by construction.
            auto const chosen =
                electAmong(consumers, {}, widthBits, false, ins);
            if (!chosen.has_value()) return;
            if (!checkElectedWidth(*chosen, *width, ins)) return;
            builder_.addInst(chosen->opcode, InvalidLirReg, {}, payload, flags);
            host_.onInstructionEmitted();
            return;
        }
        std::size_t const destIndex = destinationIndex(n);
        AsmDecodedOperand const& dest = ins.operands[destIndex];
        if (dest.indirect) {
            sink_.fail(dest.node,
                 std::format("'{}' writes to an indirect destination, which "
                             "this build does not lower{}", ins.mnemonic,
                             sink_.pairSuffix()));
            return;
        }
        if (dest.role != AsmOperandRole::Register && !dest.isMemory) {
            sink_.fail(dest.node,
                 std::format("'{}' writes to a destination that is neither a "
                             "register nor a memory reference, which this build "
                             "does not lower{}", ins.mnemonic, sink_.pairSuffix()));
            return;
        }

        // The sources, in SOURCE order, with any memory reference expanded into
        // LIR's address form.
        std::vector<LirOperand> sources;
        for (std::size_t i = 0; i < n; ++i) {
            if (i == destIndex) continue;
            AsmDecodedOperand const& src = ins.operands[i];
            if (src.indirect) {
                sink_.fail(src.node,
                     std::format("'{}' reads an indirect source, which this "
                                 "build does not lower{}", ins.mnemonic,
                                 sink_.pairSuffix()));
                return;
            }
            if (src.isMemory) { appendMemory(src, sources); continue; }
            switch (src.role) {
            case AsmOperandRole::Register:
                sources.push_back(LirOperand::makeReg(
                    src.reg));
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
                    sink_.fail(src.node,
                         std::format("immediate {} does not fit the 32-bit "
                                     "immediate slot LIR carries — a wider "
                                     "constant needs the literal pool, which "
                                     "this build does not yet reach from "
                                     "assembly{}", src.value, sink_.pairSuffix()));
                    return;
                }
                sources.push_back(LirOperand::makeImmInt32(
                    static_cast<std::int32_t>(src.value)));
                break;
            }
            case AsmOperandRole::Memory:
            case AsmOperandRole::Indirect:
                sink_.fail(src.node, std::format("this operand form is not yet "
                                           "lowered by this build{}",
                                           sink_.pairSuffix()));
                return;
            }
        }

        LirReg const destReg =
            dest.isMemory ? InvalidLirReg
                          : dest.reg;

        // ── SHAPE 3: memory destination. Sources, then the address tail. Only
        // a non-producer can take it — that is the `store` shape.
        if (dest.isMemory) {
            std::vector<LirOperand> operands = sources;
            appendMemory(dest, operands);
            // ★ THE ONE SITE THAT SETS THE MEMORY-DIRECTION AXIS
            // (D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE). This
            // arm ran because the DESTINATION-position operand is the memory
            // reference; the flag records exactly that, and it is what lets a
            // target declare `cmp mem, reg` (39 /r) apart from the
            // byte-identical operand list of `cmp reg, mem` (3B /r).
            auto const chosen =
                electAmong(consumers, operands, widthBits, true, ins);
            if (!chosen.has_value()) return;
            if (!checkElectedWidth(*chosen, *width, ins)) return;
            // ⚠ THE FLAG IS STAMPED ON THE INSTRUCTION, NOT MERELY USED FOR
            // THE ELECTION, and that is the whole soundness argument: the
            // ENCODER re-runs the same selection post-regalloc, reading the
            // axis off these same `flags`. An election axis the encoder could
            // not reproduce would let the two tiers pick different variants
            // with no diagnostic anywhere.
            builder_.addInst(chosen->opcode, InvalidLirReg, operands, payload,
                             static_cast<std::uint8_t>(
                                 flags | kLirInstFlagMemoryIsDestination));
            host_.onInstructionEmitted();
            return;
        }

        // ── SHAPE 1: producer, register destination. The two-address prefix is
        // not decided in advance — the plain shape is offered to the target's
        // guards first, and the destination-prefixed one only if nothing took
        // it.
        std::optional<asm_elect::ElectedOpcode> chosen;
        std::vector<LirOperand>                 operands;
        // The destination here is a REGISTER; any memory reference is a
        // SOURCE, so the memory-direction axis is `false`.
        Election pe = electQuiet(producers, sources, widthBits, false);
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
            Election t = electQuiet(producers, twoAddr, widthBits, false);
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
                : electQuiet(consumers, destFirst, widthBits, false);
        if (!ce.ambiguousWith.empty()) {
            reportAmbiguous(ins, ce, widthBits);
            return;
        }
        if (ce.opcode.has_value() && chosen.has_value()) {
            sink_.fail(ins.node,
                 std::format("'{}' could be target opcode '{}' (which produces "
                             "a value, so the destination is its result) or "
                             "'{}' (which produces none, so the destination is "
                             "an input) — one spelling cannot be both{}",
                             ins.mnemonic, chosen->info->mnemonic,
                             ce.opcode->info->mnemonic, sink_.pairSuffix()));
            return;
        }
        if (ce.opcode.has_value()) {
            if (!checkElectedWidth(*ce.opcode, *width, ins)) return;
            builder_.addInst(ce.opcode->opcode, InvalidLirReg, destFirst,
                             payload, flags);
            host_.onInstructionEmitted();
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
        host_.onInstructionEmitted();
    }

    // ★★★ M2 — LOWER A SYMBOL-NAMED SOURCE OPERAND TO ITS ADDRESS.
    // D-ASM-SYMBOL-OPERAND-NOT-LOWERED + D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET,
    // which are ONE mechanism seen from two sides: `adr x0, msg`
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
    //
    // ⚠ WHICH shapes exist is stated here; WHICH one a name gets is the HOST's,
    // because only the host has a label model. The engine keeps the "is this
    // even a name?" refusal, which is a property of the OPERAND.
    [[nodiscard]] bool sourceOperandForSymbol(AsmDecodedOperand const&    src,
                                              std::string_view         mnemonic,
                                              std::vector<LirOperand>& sources) {
        if (src.symbol.empty()) {
            sink_.fail(src.node,
                 std::format("'{}' reads an operand that is neither a value nor "
                             "a name{}", mnemonic, sink_.pairSuffix()));
            return false;
        }
        return host_.appendSymbolAddress(src.symbol, src.node, mnemonic,
                                         sources);
    }

    void appendMemory(AsmDecodedOperand const& m,
                      std::vector<LirOperand>& operands) const {
        operands.push_back(
            LirOperand::makeReg(m.baseReg));
        if (m.hasIndex) {
            operands.push_back(LirOperand::makeReg(
                m.indexReg));
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
               std::uint8_t widthBits,
               bool memoryIsDestination) const {
        Election e;
        if (names.empty()) return e;
        e.opcode = asm_elect::electOpcode(target_, names, operands,
                                          widthBits, memoryIsDestination,
                                          &e.rejections,
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
                if (asm_elect::selectEncodingVariant(
                        *info, operands, widthBits,
                        memoryIsDestination) != nullptr) {
                    e.firstWinner = name;
                    break;
                }
            }
        }
        return e;
    }

    void reportAmbiguous(AsmDecodedInstruction const& ins, Election const& e,
                         std::uint8_t widthBits) {
        sink_.fail(ins.node,
             std::format("'{}' could be target opcode '{}' or '{}' — both "
                         "declare an encoding variant taking this operand "
                         "shape at width {}, so the dialect's candidate list "
                         "does not say which instruction was written. Split "
                         "the spelling into rows whose candidate sets do not "
                         "overlap{}", ins.mnemonic,
                         e.firstWinner.empty() ? std::string{"<earlier "
                                                             "candidate>"}
                                               : e.firstWinner,
                         e.ambiguousWith, widthBits, sink_.pairSuffix()));
    }

    void reportNoShape(
        AsmDecodedInstruction const& ins,
        std::size_t operandCount, std::uint8_t widthBits,
        std::span<asm_elect::ElectionRejectionRow const> rejections) {
        std::string detail;
        for (auto const& r : rejections) {
            if (!detail.empty()) detail += "; ";
            detail += std::format("'{}': {}", r.opcodeName,
                                  asm_elect::electionRejectionText(r.why));
        }
        sink_.fail(ins.node,
             std::format("'{}' produced {} LIR operand(s) at width {}, and no "
                         "candidate target opcode encodes that shape — {}. The "
                         "dialect's operand order and the target's opcode "
                         "shapes disagree{}",
                         ins.mnemonic, operandCount, widthBits,
                         detail.empty() ? std::string{"the row names no opcode "
                                                      "this target declares"}
                                        : detail,
                         sink_.pairSuffix()));
    }

    // The single-shot election every control-flow arm uses: the operand shape
    // is fixed by the class, so there is no two-address retry and any failure
    // is final.
    std::optional<asm_elect::ElectedOpcode>
    electAmong(std::vector<std::string> const& names,
               std::span<LirOperand const> operands, std::uint8_t widthBits,
               bool memoryIsDestination,
               AsmDecodedInstruction const& ins) {
        Election e = electQuiet(names, operands, widthBits,
                                memoryIsDestination);
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
                           AsmDecodedInstruction const& ins) {
        if (asm_elect::variantHonorsDeclaredWidth(
                *elected.variant, static_cast<std::uint8_t>(width))) {
            return true;
        }
        sink_.fail(ins.node,
             std::format("'{}' operates on {} bits, but the target's opcode "
                         "'{}' declares no width-keyed encoding variant for the "
                         "operand shape it matched — the variant that matched is "
                         "width-agnostic and would encode at the target's "
                         "natural width, silently dropping the width this "
                         "instruction asked for{}",
                         ins.mnemonic, width, elected.info->mnemonic,
                         sink_.pairSuffix()));
        return false;
    }

    // ── control-flow arms ─────────────────────────────────────────────────
    void buildReturn(AsmDecodedInstruction const& ins,
                     std::vector<std::string> const& names,
                     std::uint32_t payload, std::uint8_t flags) {
        if (!ins.operands.empty()) {
            sink_.fail(ins.node,
                 std::format("'{}' is a return and this build lowers only its "
                             "operand-less form{}", ins.mnemonic,
                             sink_.pairSuffix()));
            return;
        }
        auto const elected = electAmong(
            names, std::span<LirOperand const>{}, lirInstWidthBits(flags),
            false, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, lirInstWidthBits(flags), ins)) {
            return;
        }
        builder_.addReturn(elected->opcode, {}, payload, flags);
        host_.onTerminatorEmitted();
        host_.onInstructionEmitted();
    }

    // The block a branch operand names, or nullopt with a diagnostic. ⚠ A
    // BRANCH TARGET IS FUNCTION-LOCAL: `LirOperand::makeBlockRef` names a block
    // slot, and a slot from another function would silently resolve to whatever
    // block sits at that index here.
    std::optional<LirBlockId> branchTarget(AsmDecodedOperand const& op,
                                           std::string_view mnemonic) {
        if (op.indirect || op.isMemory || op.symbol.empty()) {
            sink_.fail(op.node,
                 std::format("'{}' needs a label to branch to; this operand is "
                             "not one{}", mnemonic, sink_.pairSuffix()));
            return std::nullopt;
        }
        return host_.resolveBranchTarget(op.symbol, op.node, mnemonic);
    }

    void buildBr(AsmDecodedInstruction const& ins,
                 std::vector<std::string> const& names, std::uint32_t payload,
                 std::uint8_t flags) {
        if (ins.operands.size() != 1) {
            sink_.fail(ins.node,
                 std::format("'{}' is an unconditional branch and takes exactly "
                             "one target; {} were written{}", ins.mnemonic,
                             ins.operands.size(), sink_.pairSuffix()));
            return;
        }
        auto const target = branchTarget(ins.operands[0], ins.mnemonic);
        if (!target.has_value()) return;
        std::array<LirOperand, 1> const ops{
            LirOperand::makeBlockRef(target->v)};
        auto const elected =
            electAmong(names, ops, lirInstWidthBits(flags), false, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, lirInstWidthBits(flags), ins)) {
            return;
        }
        builder_.addBr(elected->opcode, *target, payload, flags);
        host_.onTerminatorEmitted();
        host_.onInstructionEmitted();
    }

    void buildCondBr(AsmDecodedInstruction const& ins,
                     std::vector<std::string> const& names,
                     std::uint32_t payload, std::uint8_t flags) {
        if (ins.operands.size() != 1) {
            sink_.fail(ins.node,
                 std::format("'{}' is a conditional branch and takes exactly "
                             "one taken-target; {} were written{}",
                             ins.mnemonic, ins.operands.size(), sink_.pairSuffix()));
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
            electAmong(names, ops, lirInstWidthBits(flags), false, ins);
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
        host_.onInstructionEmitted();
        builder_.beginBlock(fallthrough);
        host_.onBlockOpened(fallthrough);
    }

    void buildCall(AsmDecodedInstruction const& ins,
                   std::vector<std::string> const& names, std::uint32_t payload,
                   std::uint8_t flags, std::uint8_t widthBits) {
        if (ins.operands.size() != 1) {
            sink_.fail(ins.node,
                 std::format("'{}' is a call and takes exactly one callee; {} "
                             "were written{}", ins.mnemonic,
                             ins.operands.size(), sink_.pairSuffix()));
            return;
        }
        AsmDecodedOperand const& callee = ins.operands[0];
        std::vector<LirOperand> ops;
        if (callee.indirect && callee.role == AsmOperandRole::Register) {
            ops.push_back(LirOperand::makeReg(
                callee.reg));
        } else if (!callee.indirect && !callee.symbol.empty()
                   && !callee.isMemory) {
            // ★ WHAT A NAMED CALLEE IS — a function this file defines, a block
            // it defines (refused), or an import — is a LABEL-MODEL question,
            // so the host answers it. The engine keeps only the shape test
            // above, which is a property of the OPERAND.
            auto const target =
                host_.resolveCallee(callee.symbol, callee.node, ins.mnemonic);
            if (!target.has_value()) return;
            ops.push_back(*target);
        } else {
            sink_.fail(callee.node,
                 std::format("'{}' needs a symbol or a register as its callee{}",
                             ins.mnemonic, sink_.pairSuffix()));
            return;
        }
        auto const elected = electAmong(names, ops, widthBits, false, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, widthBits, ins)) return;
        // ★ A CALL IS NOT A TERMINATOR — plain `addInst`. And this path runs
        // POST-callconv (no pass follows), so the operand list is exactly the
        // callee reference: the argument registers were set by the instructions
        // the programmer wrote above it.
        builder_.addInst(elected->opcode, InvalidLirReg, ops, payload,
                         flags);
        host_.onInstructionEmitted();
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
    void buildIndirectBr(AsmDecodedInstruction const& ins,
                         std::vector<std::string> const& names,
                         std::uint32_t payload, std::uint8_t flags,
                         std::uint8_t widthBits) {
        if (ins.operands.size() != 1) {
            sink_.fail(ins.node,
                 std::format("'{}' is an indirect branch and takes exactly one "
                             "target-address operand; {} were written{}",
                             ins.mnemonic, ins.operands.size(), sink_.pairSuffix()));
            return;
        }
        AsmDecodedOperand const& op = ins.operands[0];
        if (op.role != AsmOperandRole::Register || op.isMemory) {
            sink_.fail(op.node,
                 std::format("'{}' is an indirect branch and this build lowers "
                             "only its REGISTER form — the target address must "
                             "already be in a register{}",
                             ins.mnemonic, sink_.pairSuffix()));
            return;
        }
        std::array<LirOperand, 1> const ops{
            LirOperand::makeReg(op.reg)};
        // The election still runs, and runs FIRST: a row whose indirect arm
        // names an opcode that cannot take a register is a config defect, and
        // reporting it before the successor-set refusal keeps the two failures
        // distinguishable.
        auto const elected = electAmong(names, ops, widthBits, false, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, widthBits, ins)) return;
        std::vector<LirBlockId> const succs = host_.addressTakenSuccessors();
        if (succs.empty()) {
            sink_.fail(ins.node,
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
                             host_.enclosingFunctionName(),
                             sink_.pairSuffix()));
            return;
        }
        builder_.addIndirectBr(elected->opcode, ops, succs, payload, flags);
        host_.onTerminatorEmitted();
        host_.onInstructionEmitted();
    }

    Tree const&           tree_;
    GrammarSchema const&  grammar_;
    TargetSchema const&   target_;
    AssemblyConfig const& cfg_;
    LirBuilder&           builder_;
    AsmDiagnosticSink&    sink_;
    AsmLoweringHost&      host_;
    std::vector<ResolvedRow> rows_;
};

AsmInstructionLowering::AsmInstructionLowering(Tree const&          tree,
                                               GrammarSchema const& grammar,
                                               TargetSchema const&  target,
                                               LirBuilder&          builder,
                                               AsmDiagnosticSink&   sink,
                                               AsmLoweringHost&     host)
    : impl_(std::make_unique<Impl>(tree, grammar, target, builder, sink,
                                   host)) {}

AsmInstructionLowering::~AsmInstructionLowering() = default;

bool AsmInstructionLowering::resolveRows() { return impl_->resolveRows(); }

void AsmInstructionLowering::lowerStatement(NodeId statement,
                                            NodeId mnemonicNode,
                                            NodeId operandSeq) {
    impl_->lowerStatement(statement, mnemonicNode, operandSeq);
}

bool AsmInstructionLowering::decodeOperandInto(NodeId node,
                                               AsmDecodedOperand& out) {
    auto decoded = impl_->decodeOperand(node);
    if (!decoded) return false;
    out = std::move(*decoded);
    return true;
}

// ── the shared physical-register resolution ───────────────────────────────

AsmRegisterLookup resolvePhysicalRegister(TargetSchema const&  target,
                                          std::string_view     spelling,
                                          NodeId               at,
                                          AsmDiagnosticSink&   sink,
                                          AsmResolvedRegister& out) {
    auto const ordinal = target.registerByName(spelling);
    if (!ordinal) return AsmRegisterLookup::NotARegister;
    auto const* info = target.registerInfo(*ordinal);
    if (info == nullptr) {
        sink.fail(at, std::format("register '{}' has no info row", spelling));
        return AsmRegisterLookup::Reported;
    }
    out.widthBits = static_cast<std::uint32_t>(info->widthBytes) * 8u;
    std::uint16_t resolved = *ordinal;
    for (int hop = 0; !info->subOf.empty(); ++hop) {
        auto const parent = target.registerByName(info->subOf);
        if (!parent) {
            sink.fail(at,
                      std::format("register '{}' declares subOf='{}', which "
                                  "this target does not declare{}",
                                  info->name, info->subOf, sink.pairSuffix()));
            return AsmRegisterLookup::Reported;
        }
        resolved = *parent;
        info     = target.registerInfo(resolved);
        if (info == nullptr) {
            sink.fail(at, std::format("register ordinal {} has no info row",
                                      resolved));
            return AsmRegisterLookup::Reported;
        }
        // `TargetSchema::validate` already rejects a subOf cycle at load; the
        // bound keeps a hand-built schema from spinning here.
        if (hop > 8) {
            sink.fail(at,
                      std::format("register '{}' has a subOf chain deeper than "
                                  "this build follows{}",
                                  spelling, sink.pairSuffix()));
            return AsmRegisterLookup::Reported;
        }
    }
    out.regClass = static_cast<LirRegClass>(info->regClass);
    out.reg      = makePhysicalReg(resolved, out.regClass);
    return AsmRegisterLookup::Resolved;
}

// ── the EMBEDDED caller ───────────────────────────────────────────────────

namespace {

// ★★★ THE TEMPLATE HOST — the embedded half of the two-caller split, and the
// whole of what "embedded" means to this engine.
//
// ⚠ EVERY REFUSAL BELOW IS A CAPABILITY STATEMENT, NOT A STUB. A template has
// no label model of its own: the statement it lives in owns the enclosing
// function's blocks, and `LirOperand::makeBlockRef` names a function-local
// SLOT — so a permissive `resolveBranchTarget` would not fail, it would bind to
// whichever block sits at that index in the CALLER, which is a miscompile with
// no diagnostic. Refusing names the template and the target.
//
// ★★★ AND THAT SENTENCE IS EXACTLY WHY `asm goto` LOWERS THROUGH A **BINDING**
// RATHER THAN THROUGH A LOOKUP. The host still mints no block and still reads
// no label table of its own; the caller — which owns the CFG and has already
// created the successor edge — hands it a block per spelling, and this class
// only ever ANSWERS with one it was given. A spelling nobody bound is refused,
// which keeps the miscompile above unreachable by construction rather than by
// care.
class TemplateHost final : public AsmLoweringHost {
public:
    TemplateHost(TargetSchema const&                target,
                 std::span<AsmOperandBinding const> bindings,
                 std::span<AsmLabelBinding const>   labelBindings,
                 AsmDiagnosticSink&                 sink)
        : target_(target), bindings_(bindings),
          labelBindings_(labelBindings), sink_(sink) {}

    [[nodiscard]] bool namesRegister(std::string_view spelling) const override {
        return bindingFor(spelling) != nullptr
               || target_.registerByName(spelling).has_value();
    }

    [[nodiscard]] AsmRegisterLookup
    resolveRegister(std::string_view spelling, NodeId at,
                    AsmResolvedRegister& out) override {
        // ★★ THE BINDING IS CONSULTED FIRST, AND THE ORDER IS THE DECISION. A
        // template's operand spellings are the EMBEDDING language's, minted and
        // checked by it; a target register name that collided with one would
        // mean the caller had bound a name the dialect also reads as a
        // register, and letting the TARGET win there would silently ignore the
        // binding the caller asked for.
        if (auto const* b = bindingFor(spelling); b != nullptr) {
            out.reg         = b->reg;
            out.regClass    = b->regClass;
            // The form travels with the register; `decodePlaceholder` is what
            // turns it into the dialect's memory shape. A PHYSICAL register
            // spelling (the fallthrough below) leaves the default `Reg` — a
            // register written in the assembly text denotes itself.
            out.operandKind = b->operandKind;
            // ★ AND THE IMMEDIATE PAYLOAD TRAVELS WITH IT, for the same reason:
            // the FORM says what `%N` denotes and, for `imm32`, the value IS the
            // operand. A physical register spelling (the fallthrough below)
            // leaves both at their defaults — text-written registers denote
            // themselves and no constraint letter is in play.
            out.hasImmediate = b->hasImmediate;
            out.value        = b->value;
            // ★★★ AND THE WIDTH IS **DERIVED** RATHER THAN COPIED, WHICH IS THE
            // ONE FACT ABOUT AN OPERAND REFERENCE THE CALLER CANNOT SUPPLY. The
            // binding says how wide the OPERAND is; the target says which VIEW
            // of its register a bare reference names, and the two are different
            // questions on one of the two shipped processors. Last, because it
            // reads the form and the class copied above.
            if (!bareOperandWidth(*b, at, out.widthBits)) {
                return AsmRegisterLookup::Reported;
            }
            return AsmRegisterLookup::Resolved;
        }
        auto const physical =
            resolvePhysicalRegister(target_, spelling, at, sink_, out);
        if (physical != AsmRegisterLookup::NotARegister) return physical;
        // ★★★ THE UNBOUND-OPERAND REFUSAL, AND IT NAMES THE BOUND SET RATHER
        // THAN JUST THE SPELLING. Reaching here means the dialect put this
        // token in a REGISTER position (a sigil-less dialect would have sent an
        // unknown name to its symbol role instead), so the author meant either
        // a machine register or one of this template's operands, and neither
        // matched. `%3` on a two-operand template is the shape this catches,
        // and the count is the whole diagnosis — the engine's generic "unknown
        // register" would be true and useless.
        // ⚠ THE ENGINE COULD NOT WRITE THIS MESSAGE, and that is the point of
        // it living here: the OPERAND LIST is the embedding language's, so only
        // its host can enumerate it. The engine holds no `%N` convention.
        //
        // ⚠ THE `asm goto` CLAUSE THIS MESSAGE USED TO CARRY IS GONE, AND ITS
        // REMOVAL IS THE POINT RATHER THAN A TRIM. It said a label *"lands here
        // for a reason no spelling can fix … this build parses the label form
        // and lowers none"*, which was true while `AsmOperandBinding` was the
        // only binding kind. A label placeholder no longer reaches this
        // function at all: `decodePlaceholder` routes it by RULE IDENTITY into
        // `resolveBranchTarget`, which has its own refusal naming the labels
        // that WERE bound. Keeping the clause would have sent every author of a
        // mistyped operand name to a paragraph about `asm goto`, and every
        // author of an unbound label to a message that no longer described the
        // build.
        std::string bound;
        for (auto const& b : bindings_) {
            if (!bound.empty()) bound += ", ";
            bound += '\'';
            bound += b.spelling;
            bound += '\'';
        }
        sink_.fail(at, std::format(
            "'{}' names neither a register this target declares nor one of the "
            "{} operand(s) bound to this assembly template ({}) — and binding it "
            "to some register anyway would read a value the enclosing function "
            "never wrote{}",
            spelling, bindings_.size(),
            bound.empty() ? std::string{"this template binds none"} : bound,
            sink_.pairSuffix()));
        return AsmRegisterLookup::Reported;
    }

    [[nodiscard]] std::optional<std::string_view>
    openDataSectionName() const override {
        return std::nullopt;   // a template is code; it opens no section
    }
    [[nodiscard]] bool hasOpenFunction() const override { return true; }
    [[nodiscard]] bool blockIsTerminated() const override {
        return terminated_;
    }
    [[nodiscard]] std::string_view enclosingFunctionName() const override {
        return "<assembly template>";
    }

    // ★★ THIS IS WHERE AN `asm goto` LABEL IN A **NON-BRANCH** POSITION LANDS,
    // AND THE MESSAGE HAS TO SAY SO. `movq %l[done], %0` decodes the label as a
    // symbol-valued source, and the ordinary instruction path asks the host for
    // that symbol's ADDRESS — a different question from "which block does this
    // branch to", and one this build answers for nothing inside a template.
    // ⚠ ✔DOCUMENTED (GNU 6.47.2.7), and it is the reason the refusal STAYS
    // rather than becoming a lowering: `%l[name]` is defined as an `asm goto`
    // BRANCH TARGET. Taking a label's ADDRESS is `&&label`, the computed-goto
    // extension — a different construct with a different lowering — so
    // accepting it here would be inventing a semantic no reference gave us,
    // which is the same defect in the opposite direction from refusing one they
    // all accept.
    [[nodiscard]] bool appendSymbolAddress(std::string const& symbol, NodeId at,
                                           std::string_view mnemonic,
                                           std::vector<LirOperand>&) override {
        sink_.fail(at, std::format(
            "'{}' takes the address of '{}', and an assembly TEMPLATE has no "
            "labels of its own: the blocks around it belong to the language "
            "that embedded it, and a LIR block reference is function-local, so "
            "binding one here would name whichever block sits at that index in "
            "the caller. ⓘ If '{}' is an `asm goto` LABEL placeholder, it is "
            "bound as a BRANCH TARGET and can only be used as one — this "
            "instruction reads it as an ADDRESS, which is the computed-goto "
            "construct and not this one. Name the operand through this "
            "template's operand list instead{}",
            mnemonic, symbol, symbol, sink_.pairSuffix()));
        return false;
    }

    // ★★★ THE `asm goto` TARGET, ANSWERED FROM THE CALLER'S OWN BINDINGS.
    //
    // ⚠ `symbol` IS THE SPELLING THE TEMPLATE WROTE — `%l[done]`, `%l2` — not a
    // label name, because a template has no labels of its own to name. That is
    // the same key `bindingFor` matches an operand on, and it is matched the
    // same way: EXACTLY, never folded. The measurement is in
    // `AsmLoweringHost::namesRegister`'s two-caller paragraph — GNU symbolic
    // names are case-SENSITIVE C identifiers, so folding would merge two
    // distinct labels under both shipped dialects, which are `asciiFolded`.
    //
    // ⚠ A `.s` LABEL STILL LANDS HERE AND IS STILL REFUSED. `jmp Lloop` inside a
    // template arrives with `symbol == "Lloop"`, matches no binding, and gets
    // the same refusal — which is correct and is the original capability
    // statement intact: the block would have to be one the caller's CFG carries,
    // and nothing bound it.
    //
    // ★★★ THE SCAN BELOW TAKES THE **FIRST** MATCH, AND THAT IS SAFE ONLY
    // BECAUSE THE CALLER GUARANTEES THE LIST HAS NO REPEAT — a guarantee that
    // did not exist until cycle P20 and is now made in two places, neither of
    // which is here (D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND):
    //   * the C front end refuses a symbolic name used twice in one statement
    //     (`S_InlineAsmDuplicateSymbolicName`) — operands and `asm goto` labels
    //     share ONE name space, ✔MEASURED 2026-08-19 on gcc 13.3.0 and clang
    //     19.1.1, which reject all three collisions;
    //   * `mir_to_lir`'s binding builder refuses a repeated spelling in either
    //     row set, for the direct-API producers that never run that pass.
    // ⚠ DO NOT "HARDEN" THIS LOOKUP INSTEAD. A host that silently picked, or
    // merged, or reported here would be answering a question about the CALLER's
    // list from inside the engine — which holds no `%N` convention and cannot
    // say which of two rows the author meant. The invariant belongs to whoever
    // builds the list, and the sibling `bindingFor` is first-match for exactly
    // the same reason.
    [[nodiscard]] std::optional<LirBlockId>
    resolveBranchTarget(std::string const& symbol, NodeId at,
                        std::string_view mnemonic) override {
        for (auto const& b : labelBindings_) {
            if (b.spelling == symbol) return b.block;
        }
        // ★ THE REFUSAL NAMES THE BOUND SET, THE SAME SHAPE THE UNBOUND-OPERAND
        // ONE HAS — and for the same reason: the LABEL LIST is the embedding
        // language's, so only its host can enumerate it, and the count is the
        // whole diagnosis. `%l3` on a two-label `asm goto` is the shape this
        // catches, and a generic "no such block" would be true and useless.
        std::string bound;
        for (auto const& b : labelBindings_) {
            if (!bound.empty()) bound += ", ";
            bound += '\'';
            bound += b.spelling;
            bound += '\'';
        }
        sink_.fail(at, std::format(
            "'{}' branches to '{}', which is none of the {} `asm goto` label(s) "
            "bound to this assembly template ({}) — a template declares no "
            "labels of its own: the blocks around it belong to the language "
            "that embedded it, and a LIR block reference is function-local, so "
            "binding one here would name whichever block sits at that index in "
            "the caller{}",
            mnemonic, symbol, labelBindings_.size(),
            bound.empty() ? std::string{"this template binds none"} : bound,
            sink_.pairSuffix()));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<LirOperand>
    resolveCallee(std::string const& symbol, NodeId at,
                  std::string_view mnemonic) override {
        sink_.fail(at, std::format(
            "'{}' calls '{}', and an assembly TEMPLATE cannot mint the import "
            "that would need — an import states whether its target is CODE or "
            "DATA, which selects the linker's indirection slot, and that is the "
            "embedding language's declaration to make rather than this "
            "template's{}",
            mnemonic, symbol, sink_.pairSuffix()));
        return std::nullopt;
    }

    // ⚠ EMPTY, AND THE REASON MOVED WITH P20 RATHER THAN SURVIVING IT. This
    // used to read *"no label model ⇒ nothing here is address-taken"*, which
    // stopped being the reason the moment `asm goto` labels became bindable.
    // The reason now is narrower and still decisive: a bound label is a BRANCH
    // TARGET the template named explicitly, not a block whose ADDRESS was
    // taken, and an indirect branch's successor set is the address-taken set.
    // Handing it the bound labels would claim `jmp *%rax` can only reach the
    // `asm goto` targets — a claim nothing in the template supports and one the
    // optimizer would believe. Empty is a REFUSAL here (`addressTakenSuccessors`
    // states so in its own contract), which is the honest answer.
    [[nodiscard]] std::vector<LirBlockId>
    addressTakenSuccessors() const override {
        return {};
    }

    void onInstructionEmitted() override { ++emitted_; }
    void onTerminatorEmitted() override { terminated_ = true; }
    void onBlockOpened(LirBlockId) override { terminated_ = false; }

private:
    [[nodiscard]] AsmOperandBinding const*
    bindingFor(std::string_view spelling) const {
        for (auto const& b : bindings_) {
            if (b.spelling == spelling) return &b;
        }
        return nullptr;
    }

    // ★★★ WHICH VIEW A **BARE** OPERAND REFERENCE NAMES — THE TARGET'S ANSWER,
    // ASKED HERE BECAUSE THIS IS THE ONE ARM THAT KNOWS THE SPELLING DENOTED AN
    // OPERAND (D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE).
    //
    // A register written in the assembly TEXT denotes itself and keeps the width
    // its own spelling states — `%eax` is 32 bits because `eax` is, and that
    // resolution runs in `resolvePhysicalRegister`, which this function is
    // deliberately not on the path of. Only a spelling the CALLER bound is an
    // operand reference, and only an operand reference has a width to derive.
    //
    // ★★ THE WIDTH-VIEW MODIFIER COMPOSES BY CONSTRUCTION AND NEEDS NO ARM
    // HERE. `decodePlaceholder` rebuilds `%w0` into the plain `%0` before
    // asking, then OVERRIDES the answer with the letter's declared width — so a
    // modified reference passes through this derivation and discards it, which
    // is exactly the reference behaviour: ✔MEASURED 2026-08-27 on gcc 13.3.0
    // and clang 19.1.1, `%w0`/`%x0` render `w0`/`x0` on aarch64 and
    // `%b0`/`%w0`/`%k0`/`%q0` render `%al`/`%ax`/`%eax`/`%rax` on x86_64, each
    // letter giving the SAME view for a `char`, an `int` and a `long long`.
    // The letter wins on both ports; only the letter's ABSENCE differs.
    //
    // ⚠ ONLY THE REGISTER FORM. A `"m"` binding carries an ADDRESS and an `"i"`
    // binding IS its value, so neither states an operation width — ✔MEASURED
    // 2026-08-27, gcc renders a memory-bound `%0` as `[sp, 20]` / `-12(%rbp)`
    // on the two ports, an address with no width in it at all. Those forms keep
    // the binding's own number, which the decoder then discards; asking the
    // target about them would refuse a shape both references accept.
    [[nodiscard]] bool bareOperandWidth(AsmOperandBinding const& b, NodeId at,
                                        std::uint32_t& out) const {
        if (b.operandKind != OperandKindFilter::Reg) {
            out = b.widthBits;
            return true;
        }
        auto const cls = static_cast<TargetRegClass>(b.regClass);
        auto const policy = target_.asmBareOperandWidth(cls);
        // ⚠ UNDECLARED REFUSES, AND THE PLAUSIBLE WRONG ANSWER IS WHY. Both
        // derivations always assemble, so falling back to either one ships a
        // template that means something else with a clean build log — the
        // 4-byte-store-against-an-8-byte-one this row was opened for.
        if (!policy.has_value()) {
            sink_.fail(at, std::format(
                "'{}' is a bare operand reference — no width-view modifier — "
                "and target '{}' does not declare which view of a '{}' register "
                "a bare reference names ('asmBareOperandWidths'). The two "
                "answers a processor can give BOTH assemble: substituting the "
                "operand's own type width where the reference meant the full "
                "register (or the reverse) runs the instruction at a width the "
                "template did not ask for, with nothing to see in the build "
                "log. Declare the derivation for this class{}",
                b.spelling, target_.name(), targetRegClassName(cls),
                sink_.pairSuffix()));
            return false;
        }
        switch (*policy) {
            case AsmBareOperandWidth::OperandType:
                out = b.widthBits;
                return true;
            case AsmBareOperandWidth::RegisterNatural: {
                auto const natural =
                    target_.registerClassNaturalWidthBits(cls);
                // The class declared the policy but declares no full register
                // to take a width from — or declares several that disagree.
                // Either way there is no natural width to substitute, and the
                // operand's own is the answer this policy exists to reject.
                if (!natural.has_value()) {
                    sink_.fail(at, std::format(
                        "'{}' is a bare operand reference, and target '{}' "
                        "declares that such a reference names the FULL '{}' "
                        "register — but that class declares no full register "
                        "with a single width to take one from (a full register "
                        "is a 'registers' row with no 'subOf'){}",
                        b.spelling, target_.name(),
                        targetRegClassName(cls), sink_.pairSuffix()));
                    return false;
                }
                out = *natural;
                return true;
            }
        }
        return false;
    }

    TargetSchema const&                target_;
    std::span<AsmOperandBinding const> bindings_;
    std::span<AsmLabelBinding const>   labelBindings_;
    AsmDiagnosticSink&                 sink_;
    std::size_t                        emitted_    = 0;
    bool                               terminated_ = false;
};

// One template element: an instruction, or one of the two shapes a template
// cannot carry. ⚠ BOTH REFUSALS NAME WHAT THE TEMPLATE WROTE. A directive
// inside a template would change the SECTION the embedding function is being
// emitted into, and a label would define a block the caller's CFG does not
// know about — accepting either silently is the miscompile.
void lowerTemplateElement(Tree const& tree, AssemblyConfig const& cfg,
                          AsmInstructionLowering& engine,
                          AsmDiagnosticSink& sink, NodeId element) {
    if (tree.rule(element).v == cfg.directiveRule.v) {
        sink.fail(element,
                  std::format("an assembly TEMPLATE carries directives, and "
                              "this build lowers only its INSTRUCTIONS — a "
                              "directive here would change the section, the "
                              "symbol table or the data layout of the function "
                              "the template was embedded in, which is the "
                              "embedding language's to decide{}",
                              sink.pairSuffix()));
        return;
    }
    auto const kids = asm_walk::visibleChildren(tree, element);
    if (kids.empty()) return;
    NodeId const name    = kids.front();
    NodeId const rawTail = kids.size() > 1 ? kids[1] : NodeId{};
    if (asm_walk::findDescendantOfRule(tree, rawTail, cfg.labelTailRule)
            .valid()) {
        sink.fail(name,
                  std::format("an assembly TEMPLATE defines the label '{}', "
                              "and this build lowers only its INSTRUCTIONS — a "
                              "template's labels would have to become blocks of "
                              "the function that embedded it, which is the "
                              "embedding language's control flow to state{}",
                              tree.text(name), sink.pairSuffix()));
        return;
    }
    engine.lowerStatement(
        element, name,
        asm_walk::findDescendantOfRule(tree, rawTail, cfg.operandSeqRule));
}

} // namespace

std::optional<Tree>
parseAsmTemplateText(std::string                          templateText,
                     std::string                          bufferName,
                     std::shared_ptr<GrammarSchema const> dialect,
                     AsmTemplateSurface                   surface,
                     DiagnosticBudget                     budget,
                     DiagnosticReporter&                  reporter) {
    auto const emit = [&reporter](std::string message) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::A_AsmTextUnsupported;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(message);
        reporter.report(std::move(d));
    };
    if (!dialect) {
        emit("an assembly template was parsed with no dialect grammar — the "
             "active target names the dialect, and reaching here without one "
             "is a caller defect rather than a property of the template");
        return std::nullopt;
    }
    AssemblyConfig const& cfg = dialect->assembly();
    // ⚠ THE TWO REFUSALS BELOW ARE DIFFERENT QUESTIONS AND ARE KEPT APART.
    // "no `assembly` block" means this grammar is not an assembly dialect at
    // all; "no template surface" means it is one and deliberately hosts no
    // EMBEDDED templates. Collapsing them would send an implementer to the
    // wrong half of the document.
    if (!cfg.declared) {
        emit(std::format("language '{}' has no 'assembly' block, so it cannot "
                         "read an assembly template — the embedded-assembly "
                         "path was reached for a language that declares no "
                         "instruction vocabulary", dialect->name()));
        return std::nullopt;
    }
    // ★★★ THE SURFACE SELECTS THE MODE, AND A BASIC TEMPLATE NEEDS NO TEMPLATE
    // SURFACE AT ALL. A basic template reaches the assembler verbatim in the
    // reference compilers, so the `.s` reading (`main`) IS its reading — a
    // dialect that declares no `templateLexerMode` can still host one, and
    // demanding the key here would refuse a construct gcc and clang compile.
    LexerModeId mode{};   // invalid ⇒ `main`
    if (surface == AsmTemplateSurface::Extended) {
        if (!cfg.templateLexerMode.valid()) {
            // ⚠ THE ILLUSTRATION USED TO SPELL `%0` AND `%eax`, WHICH ARE THIS
            // AT&T DIALECT'S BYTES AND NOT EVERY DIALECT'S — an aarch64-gas
            // reader got a refusal demonstrating a syntax their document does
            // not have (registers there are `x0`, not `%x0`). The sentence says
            // the same thing in ROLE terms instead, which is what the rest of
            // this engine is careful to do and is one of the two places the
            // prose slipped out of it.
            emit(std::format(
                "dialect '{}' declares no 'assembly.templateLexerMode', so it "
                "hosts no EXTENDED assembly templates — an extended template's "
                "operand placeholders need this dialect's placeholder sigil to "
                "carry a token kind distinct from the one it carries in a `.s`, "
                "and the mode is where a dialect says so. Refused rather than "
                "parsed in the MAIN mode, which would silently read the "
                "template as a `.s` line: every operand placeholder would be "
                "rejected and every bare register spelling ACCEPTED, and the "
                "reference compilers do exactly the opposite in an extended "
                "template", dialect->name()));
            return std::nullopt;
        }
        mode = cfg.templateLexerMode;
    }

    // ★ THE TRAILING NEWLINE. The dialect is line-oriented (the newline IS the
    // statement terminator), so a template whose last line has none would lose
    // that line. Appending one unconditionally is harmless: a blank line
    // lowers to nothing.
    templateText += '\n';
    auto src = SourceBuffer::fromString(std::move(templateText),
                                        std::move(bufferName));

    // ★★★ THE FRAGMENT BUFFER IS REGISTERED WITH THE REPORTER **HERE**, BEFORE
    // A SINGLE TOKEN IS READ — which is what makes a template diagnostic
    // RENDERABLE, and what makes it renderable on the path that had lost it
    // entirely.
    //
    // ⚠ ✔MEASURED 2026-08-17 through the CLI, before this line existed:
    //     __asm__("movl $42, %0 @@@" : "=r"(r));
    //     error[P0001]: expected 'LineEnd' — got '@'
    //       --> <unknown-buffer:6>:offset 13
    // Every diagnostic that names this buffer — the lex/parse diagnostics
    // forwarded below AND every `A_AsmTextUnsupported` the LOWERING raises
    // afterwards (`AsmDiagnosticSink::fail` stamps `tree_.source().id()`, the
    // same buffer) — carried an id that resolved to nothing, because the
    // driver builds its `BufferRegistry` from `cu.trees()` +
    // `cu.auxiliaryBuffers()` and a buffer minted at the LIR tier is reachable
    // from neither. A diagnostic that cannot be rendered is barely better than
    // one that was never emitted.
    //
    // ★ WHY AT THE MINT AND NOT AT THE RETURN. On the FAILURE path there is no
    // `Tree` to hand back and the buffer would die inside this function with
    // the diagnostics still pointing at it — the exact path that most needs
    // the source. Registering before the parse makes survival independent of
    // the verdict. It is also why this is not an out-param: an optional the
    // one production caller never reads would be a knob nobody turns, and the
    // failure path could not use it even if the caller did.
    //
    // ★ AND IT IS NOT A KNOB THIS TIER INVENTED. `reporter` is the object that
    // already spans the gap — the LIR tier's reporter is the driver's
    // per-target scratch, which `mergeWithTargetContext` folds into the
    // run-wide reporter that renders. The buffer now travels the same route
    // its diagnostics do.
    reporter.sourceBuffers().add(src);

    // ★★ THE PARSER CONFIG IS BUILT FROM THE DIALECT, NOT DEFAULT-CONSTRUCTED.
    // `ParserConfig{}`'s `maxExpressionDepth` is a C++ FALLBACK; a language
    // states its own in `parser.maxExpressionDepth`, and the `.s` path honours
    // it (`parserConfigFor`, compilation_unit.cpp — "THE single chokepoint that
    // makes the cap config-driven"). Passing `{}` here meant one document's
    // declared cap applied on one of its two surfaces and was silently ignored
    // on the other, which is the definition of a knob that lies.
    // ⚠ IT READS THE SCHEMA ACCESSOR RATHER THAN CALLING THAT CHOKEPOINT
    // BECAUSE THE CHOKEPOINT IS NOT CALLABLE: `parserConfigFor` sits in
    // compilation_unit.cpp's ANONYMOUS namespace, so it has no linkage outside
    // that TU. The single source of truth both sites read is
    // `GrammarSchema::maxExpressionDepth()` — the helper is a two-line adapter
    // over it, not the authority. Promoting it to a shared header is the right
    // long-term shape and is the change to make the day a second knob joins the
    // config; it is recorded here rather than done half-way.
    ParserConfig parserCfg;
    if (auto const cap = dialect->maxExpressionDepth()) {
        parserCfg.maxExpressionDepth = *cap;
    }

    Tokenizer tk{src, dialect, budget, mode};
    auto [stream, lexDiagnostics] = std::move(tk).tokenize();
    Parser parser{src, dialect, std::move(stream), budget, std::move(parserCfg),
                  std::move(lexDiagnostics)};
    // ★ THE LEXER'S REPORTER IS HANDED TO THE PARSER, which folds it into the
    // Tree — the same one-stream-per-fragment discipline `UnitBuilder` applies
    // to a file. Without it a tokenizer diagnostic (an undeclared byte in the
    // template) would be dropped on the floor and the parse would fail for a
    // reason nobody could see.
    ParseResult result = std::move(parser).parse();

    // ★★★ EVERY SEVERITY IS FORWARDED, NOT ONLY `Error`, AND THE COMMENT ABOVE
    // IS WHY. This loop used to `continue` on anything that was not an Error,
    // so a tokenizer or parser WARNING about a template was dropped on the
    // failure path and — because the only production caller holds the returned
    // Tree as a scope-local and never drains its diagnostics — unreachable on
    // the success path too. "Nothing is dropped on the floor" was half true, and
    // the half that was false was the half a reader would rely on.
    // ★ `ffi/c_header_parser.cpp` is the precedent that already does this
    // (`for (auto const& d : model.diagnostics().all()) reporter.report(d);`),
    // and matching it means the two fragment readers behave the same way.
    // ⚠ THE `parsed` VERDICT STILL KEYS ON `Error` ALONE — forwarding a warning
    // must never fail a parse that succeeded.
    bool parsed = true;
    for (auto const& d : result.tree.diagnostics().all()) {
        if (d.severity == DiagnosticSeverity::Error) parsed = false;
        ParseDiagnostic copy = d;
        reporter.report(std::move(copy));
    }
    if (!parsed) return std::nullopt;
    return std::move(result.tree);
}

bool lowerAsmTemplateToLirRun(Tree const&                        templateTree,
                              GrammarSchema const&               dialect,
                              TargetSchema const&                target,
                              std::span<AsmOperandBinding const> bindings,
                              LirBuilder&                        builder,
                              DiagnosticReporter&                reporter,
                              std::span<AsmLabelBinding const>   labelBindings) {
    AsmDiagnosticSink sink{templateTree, dialect, target, reporter};
    auto const&       cfg = dialect.assembly();
    // ⚠ NOT AN ASSERT, for the reason the standalone entry states: a caller
    // that routes a non-dialect grammar here is a bug, and the bug must
    // announce itself rather than emit nothing and link into a program that
    // silently skips the instructions the programmer wrote.
    if (!cfg.declared) {
        sink.fail(templateTree.root(),
                  std::format("language '{}' has no 'assembly' block, so it "
                              "cannot lower an assembly template — the "
                              "embedded-assembly path was reached for a "
                              "language that declares no instruction "
                              "vocabulary",
                              dialect.name()));
        return false;
    }

    TemplateHost           host{target, bindings, labelBindings, sink};
    AsmInstructionLowering engine{templateTree, dialect, target,
                                  builder,      sink,   host};
    if (!engine.resolveRows()) return false;

    // ★ THE SAME LINE STRUCTURE THE STANDALONE PATH WALKS — `asm.lang.json`'s
    // `asmLine` / `asmElement`, named by the dialect and never indexed by
    // position. A template is a `.s` fragment, so its lines parse identically.
    for (NodeId const line :
         asm_walk::visibleChildren(templateTree, templateTree.root())) {
        if (templateTree.kind(line) != NodeKind::Internal) continue;
        if (templateTree.rule(line).v != cfg.lineRule.v) continue;
        for (NodeId const child : asm_walk::visibleChildren(templateTree, line)) {
            if (templateTree.kind(child) != NodeKind::Internal) continue;
            if (templateTree.rule(child).v != cfg.elementRule.v) continue;
            for (NodeId const element :
                 asm_walk::visibleChildren(templateTree, child)) {
                if (templateTree.kind(element) != NodeKind::Internal) continue;
                lowerTemplateElement(templateTree, cfg, engine, sink, element);
                if (!sink.ok()) return false;
            }
        }
    }
    return sink.ok();
}

} // namespace dss
