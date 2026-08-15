#pragma once

#include "core/export.hpp"
#include "core/types/assembly_config.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/tree.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ★★★ THE ONE TEXT→LIR INSTRUCTION ENGINE, AND ITS TWO CALLERS.
//
// A dialect statement (`movq %rax, %rcx`, `nop`, `bl helper`) becomes LIR in
// exactly one place. Two callers reach it and they differ in ONE axis:
//
//   * the STANDALONE `.s` (`asm_text_to_lir.cpp`) — a hand-written file that
//     already names PHYSICAL registers and writes its own frame, entering the
//     pipeline at the `encode` tier;
//   * an EMBEDDED `__asm__` template inside a still-vreg-based function, whose
//     placeholder operands name the caller's VREGS and whose instructions are
//     emitted into the caller's own `LirBuilder`, mid-function.
//
// ⚠ WHY THAT IS ONE ENGINE AND NOT TWO. The election (`asm_variant_elect.hpp`),
// the width-honesty gate, the operand-shape walk and every refusal in them are
// the answer to "can this target encode what this dialect wrote?" — a question
// that has nothing to do with WHO wrote it. A second copy for the embedded path
// would drift, and the drift's failure mode is a green build emitting a
// different instruction from the one in the source: exactly the shape
// `asm_variant_elect.hpp` was written to make impossible between the two tiers
// that already ask it.
//
// ⇒ the ONLY things the two callers parameterise are what an operand DENOTES
// and what the program AROUND the instruction is. Both are asked of an
// `AsmLoweringHost`; nothing below knows which caller it serves, and there is
// no `if (embedded)` anywhere in the engine.

namespace dss {

// ── shared CST walking ────────────────────────────────────────────────────
//
// ★ SHARED BECAUSE BOTH HALVES OF THE OLD FILE USE THEM — the unit walker
// (labels, sections, data directives) and the instruction engine. They moved
// here rather than being duplicated: two copies of "which token spells this
// operand" is how a dialect-blind reading becomes two readings.
namespace asm_walk {

// Visible (non-EmptySpace) children — the tree-wide indexing convention.
inline std::vector<NodeId> visibleChildren(Tree const& tree, NodeId parent) {
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
inline NodeId findDescendantOfRule(Tree const& tree, NodeId n, RuleId rule) {
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
inline NodeId firstVisibleToken(Tree const& tree, NodeId n) {
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

inline NodeId lastVisibleToken(Tree const& tree, NodeId n) {
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
inline void collectDescendantsOfRule(Tree const& tree, NodeId n, RuleId rule,
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
} // namespace asm_walk

// ── diagnostics ───────────────────────────────────────────────────────────
//
// ★★ ONE SINK, SO THE TWO HALVES CANNOT REPORT DIFFERENTLY. Every refusal in
// the assembly path is an `A_AsmTextUnsupported` naming the span, and every one
// of them ends by naming the two config documents the reader must open
// (`pairSuffix`). Splitting the engine out of the unit walker would otherwise
// have produced two `fail()`s and two `ok_` flags — and a lowering that failed
// in one half while the other reported success is not hypothetical: the
// standalone `run()` returns nullopt on `!ok_`, so a second flag is a silently
// accepted refusal.
class DSS_EXPORT AsmDiagnosticSink {
public:
    AsmDiagnosticSink(Tree const& tree, GrammarSchema const& grammar,
                      TargetSchema const& target, DiagnosticReporter& reporter)
        : tree_(tree), grammar_(grammar), target_(target), reporter_(reporter) {}

    void fail(NodeId at, std::string message);

    // ★★ A DIAGNOSTIC THAT DOES NOT REFUSE THE FILE — see the definition for
    // the measured gas behaviour it matches. ⚠ IT DOES NOT TOUCH `ok_`.
    void warn(NodeId at, std::string message);

    // Every "this pair does not realize that" message ends the same way, and
    // the tail is what makes the diagnostic actionable: it names the two config
    // documents the reader has to open.
    [[nodiscard]] std::string pairSuffix() const;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    Tree const&          tree_;
    GrammarSchema const& grammar_;
    TargetSchema const&  target_;
    DiagnosticReporter&  reporter_;
    bool                 ok_ = true;
};

// ── the walker's own vocabulary ───────────────────────────────────────────
//
// One decoded assembly operand. The CST rule that produced it is already
// resolved to a ROLE by the dialect's `operandForms`, so nothing below ever
// asks "which rule was that?" — it asks "which role?", which is the property
// a second dialect binds to different rules.
struct DSS_EXPORT AsmDecodedOperand {
    AsmOperandRole role{};
    NodeId         node{};        // for the diagnostic span
    // Register role: the target register ordinal + its class. ★ THE ORDINAL IS
    // ALWAYS THE FULL-WIDTH PARENT'S. A narrow spelling (`%eax`, `w0`) is a
    // `subOf` row in the target's register table; LIR names ONE register and
    // carries the width on the INSTRUCTION, so the sub-register resolves to its
    // parent here and `regWidthBits` remembers what the programmer wrote.
    // ★ RESOLVED, NOT AN ORDINAL. The spelling is handed to the caller's
    // `AsmLoweringHost`, which answers with a `LirReg` — PHYSICAL for a
    // standalone `.s` (today's behaviour, `makePhysicalReg` of the target
    // ordinal) and VIRTUAL for an embedded template operand. Storing the
    // resolved register rather than a target ordinal is what lets ONE decoder
    // serve both callers; storing an ordinal would have forced the vreg case
    // to invent a parallel field the shape walk below would then have to
    // choose between.
    LirReg         reg          = InvalidLirReg;
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
    LirReg         baseReg      = InvalidLirReg;
    bool           hasIndex     = false;
    LirReg         indexReg     = InvalidLirReg;
    std::uint32_t  scale        = 1;
    std::int32_t   disp         = 0;
};

// What a register-role operand SPELLING denotes, as the host resolved it.
struct DSS_EXPORT AsmResolvedRegister {
    LirReg        reg       = InvalidLirReg;
    LirRegClass   regClass  = LirRegClass::None;
    // The width the SPELLING states (`%eax` → 32, `x0` → 64). The instruction
    // width is derived from it exactly as it always was — a narrow spelling is
    // the only thing that says a 32-bit operation was meant on a dialect that
    // writes no mnemonic suffix.
    std::uint32_t widthBits = 0;
};

// The three answers a register lookup can give, and they are three rather than
// two on purpose: "the host already reported why" is NOT the same as "this
// spelling names no register", and collapsing them would either double-report
// or replace a precise refusal (a broken `subOf` chain) with a vague one.
// ★ THE SHAPE IS THE SUBSTRATE'S OWN, NOT A NEW IDEA:
// `AngleIncludeResolution` (`core/types/include_path_resolve.hpp:221-224`)
// already separates found / not-found / already-reported for exactly this
// reason, and states it in the same words — *"Reported, never resolved; kept
// DISTINCT because different tiers own the report"*. Grepped before minting
// this; that enum is about include paths, so the TYPE is not reusable, but the
// three-way verdict is a precedent rather than an invention.
enum class AsmRegisterLookup : std::uint8_t {
    Resolved,
    NotARegister,   // caller decides: a sigil-less dialect's symbol, or an error
    Reported,       // the host failed loud already; do not add a second message
};

// ★★ THE TARGET'S OWN ANSWER FOR A REGISTER SPELLING — the resolution BOTH
// hosts share, so the two callers cannot disagree about what `%eax` is.
//
// ★ A NARROW SPELLING RESOLVES TO ITS PARENT'S ORDINAL. `%eax` and `%rax` are
// ONE machine register; LIR names it once and carries the access width on the
// INSTRUCTION (`kLirInstFlagWidth32`), which is also why the allocator holds
// every `subOf` row out of its pools. The target's `subOf` chain is the single
// declaration of that aliasing — followed here rather than re-stated in a table
// this file owns.
//
// ⚠ IT REPORTS ONLY THE BROKEN-SCHEMA ARMS (`Reported`) — a `subOf` naming a
// register the target does not declare, or an ordinal with no info row. "This
// spelling names no register" is `NotARegister` and is SILENT, because it is
// not always an error: a sigil-less dialect (aarch64) reads exactly that answer
// as "then it is a symbol".
[[nodiscard]] DSS_EXPORT AsmRegisterLookup
resolvePhysicalRegister(TargetSchema const& target, std::string_view spelling,
                        NodeId at, AsmDiagnosticSink& sink,
                        AsmResolvedRegister& out);

// ★★★ ONE TEMPLATE OPERAND, AS THE EMBEDDING LANGUAGE BOUND IT.
//
// `spelling` is the text the DIALECT writes for this operand — the engine never
// interprets it, it only compares. That is what keeps the `%N` convention OUT
// of the engine: numbering outputs then inputs is GNU inline-asm vocabulary
// owned by the language that embeds assembly (GCC 6.47.2.3), and an engine that
// only asks "which of these spellings is it?" holds no opinion about how they
// were spelled. A dialect numbering its placeholders differently needs no
// engine change.
//
// ⚠ THE ORDER OF THE SPAN IS THE OPERAND INDEX, and it is load-bearing for the
// DIAGNOSTIC rather than for the lookup: a template naming an operand that was
// never bound must be able to say how many there were and which ones.
struct DSS_EXPORT AsmOperandBinding {
    std::string   spelling;
    LirReg        reg       = InvalidLirReg;
    LirRegClass   regClass  = LirRegClass::None;
    std::uint32_t widthBits = 0;
};

// ★★★ EVERYTHING THE PER-INSTRUCTION ENGINE CANNOT DERIVE FROM (tree, dialect,
// target). Every one of these is a fact about the PROGRAM the instruction sits
// in — which labels exist, which register an operand denotes, whether a
// function is open — and that is precisely the axis on which a standalone `.s`
// and an embedded template differ.
//
// ⚠ NOT A "MODE" FLAG. The alternative shape — one engine with an `isEmbedded`
// bool — was rejected on the bar: it puts the caller's identity INSIDE the
// engine, where every later question gets answered with a second branch, and
// the first thing such a branch does is let the two paths' behaviour diverge
// invisibly. Asking a question of a host has no such gradient.
class DSS_EXPORT AsmLoweringHost {
public:
    virtual ~AsmLoweringHost();

    // ── what an operand denotes ───────────────────────────────────────────
    //
    // Does this spelling name a register? ⚠ MUST AGREE WITH `resolveRegister`
    // (`namesRegister(s)` ⟺ `resolveRegister(s, …) != NotARegister`): the role
    // disambiguation asks the first and the decode asks the second, and a
    // dialect that binds `register` and `displaced` to ONE rule (aarch64's
    // sigil-less names) decides between them on this answer alone.
    //
    // ★★★ THE `spelling` A HOST RECEIVES IS ALREADY NORMALIZED BY THE DIALECT'S
    // `assembly.spellingCase` — the engine folds before it asks, so a host
    // compares EXACTLY and never folds again. That direction is deliberate: a
    // host would have to be handed the dialect to fold correctly, and the two
    // hosts folding independently is precisely how the standalone `.s` path and
    // the embedded template path would come to disagree about what `X0` is.
    // ⚠ CONSEQUENCE FOR A HOST WITH A LOOKUP TABLE OF ITS OWN (the template
    // host's operand BINDINGS): its keys are matched against normalized text,
    // so under a folding dialect a binding must be registered in folded form.
    // No shipped binding spelling has a letter in it today (`%0`, `%1`), so
    // nothing depends on it yet — it is written down because the day one does,
    // the failure would be a silently unbound operand.
    [[nodiscard]] virtual bool
    namesRegister(std::string_view spelling) const = 0;

    [[nodiscard]] virtual AsmRegisterLookup
    resolveRegister(std::string_view spelling, NodeId at,
                    AsmResolvedRegister& out) = 0;

    // ── the program around the instruction ────────────────────────────────
    //
    // The DATA section open at this point, or nullopt in text. An instruction
    // inside an open data section is refused: LIR places code in text only.
    [[nodiscard]] virtual std::optional<std::string_view>
    openDataSectionName() const = 0;
    [[nodiscard]] virtual bool hasOpenFunction() const = 0;
    // Did the last emitted instruction terminate its block? An instruction
    // after a terminator with no intervening label is unreachable.
    [[nodiscard]] virtual bool blockIsTerminated() const = 0;
    [[nodiscard]] virtual std::string_view enclosingFunctionName() const = 0;

    // `leaq foo,%rax` / `adr x0, Lcase1` — append the LIR operands that name
    // `symbol`'s ADDRESS. false ⇒ the host reported why.
    [[nodiscard]] virtual bool
    appendSymbolAddress(std::string const& symbol, NodeId at,
                        std::string_view mnemonic,
                        std::vector<LirOperand>& out) = 0;

    // The block `symbol` names, or nullopt with a diagnostic.
    [[nodiscard]] virtual std::optional<LirBlockId>
    resolveBranchTarget(std::string const& symbol, NodeId at,
                        std::string_view mnemonic) = 0;

    // The callee operand for `call symbol` — a defined function's SymbolRef or
    // a freshly interned import. nullopt ⇒ the host reported why.
    [[nodiscard]] virtual std::optional<LirOperand>
    resolveCallee(std::string const& symbol, NodeId at,
                  std::string_view mnemonic) = 0;

    // Every ADDRESS-TAKEN block of the open function — an indirect branch's
    // successor set. Empty is a refusal, never an assumption.
    [[nodiscard]] virtual std::vector<LirBlockId>
    addressTakenSuccessors() const = 0;

    // ── emit bookkeeping ──────────────────────────────────────────────────
    virtual void onInstructionEmitted() = 0;
    virtual void onTerminatorEmitted() = 0;
    // The conditional branch's minted false edge: the host opens it and resets
    // its own per-block counters.
    virtual void onBlockOpened(LirBlockId block) = 0;
};

// ── the engine ────────────────────────────────────────────────────────────
//
// Construct once per lowering, `resolveRows()` once, then one
// `lowerStatement()` per instruction. `decodeOperandInto` is exposed because
// the unit walker's DATA directives read their operands through the same
// decoder — `.quad Lcase0` and `movq $8,%rax` must agree about what a scalar is.
class DSS_EXPORT AsmInstructionLowering {
public:
    AsmInstructionLowering(Tree const& tree, GrammarSchema const& grammar,
                           TargetSchema const& target, LirBuilder& builder,
                           AsmDiagnosticSink& sink, AsmLoweringHost& host);
    ~AsmInstructionLowering();
    AsmInstructionLowering(AsmInstructionLowering const&)            = delete;
    AsmInstructionLowering& operator=(AsmInstructionLowering const&) = delete;

    // PASS 0 — cross-check every dialect row against the ACTIVE target.
    [[nodiscard]] bool resolveRows();

    // One statement: `mnemonicNode` is the spelling token, `operandSeq` the
    // dialect's operand-sequence node (invalid when the instruction has none).
    void lowerStatement(NodeId statement, NodeId mnemonicNode,
                        NodeId operandSeq);

    [[nodiscard]] bool decodeOperandInto(NodeId node, AsmDecodedOperand& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ★★★ LOWER ONE EMBEDDED ASSEMBLY TEMPLATE INTO THE CALLER'S OWN `LirBuilder`.
//
// `templateTree` is the template TEXT parsed with `dialect` — the same dialect
// grammar the standalone `.s` path uses, which is the whole point: no second
// parser, no second instruction table, no second election.
//
// `bindings` are the template's operands in GNU order (outputs then inputs),
// each carrying the DIALECT SPELLING that names it and the register it denotes.
//
// ⚠ THE BUILDER MUST ALREADY HAVE AN OPEN BLOCK. A template is emitted MID
// FUNCTION, into the block the embedding language is filling; every instruction
// lands there in source order.
//
// Returns false with at least one diagnostic reported on any refusal.
[[nodiscard]] DSS_EXPORT bool
lowerAsmTemplateToLirRun(Tree const&                        templateTree,
                         GrammarSchema const&               dialect,
                         TargetSchema const&                target,
                         std::span<AsmOperandBinding const> bindings,
                         LirBuilder&                        builder,
                         DiagnosticReporter&                reporter);

} // namespace dss
