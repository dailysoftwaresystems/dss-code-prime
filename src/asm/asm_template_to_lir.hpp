#pragma once

#include "core/export.hpp"
#include "core/types/assembly_config.hpp"
#include "core/types/diagnostic_budget.hpp"
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
    // ★★★ A HOST HAS **TWO** CALLERS AND THEY NORMALIZE DIFFERENTLY. THE SPLIT
    // IS DELIBERATE, IT IS NOT SYMMETRIC, AND IT IS STATED HERE BECAUSE THE ONE
    // THING A HOST AUTHOR CANNOT GUESS IS WHICH TEXT ARRIVES IN WHICH FORM.
    //
    //   • A REGISTER SPELLING (`decodeRegister`, the `.s` and template register
    //     forms alike) ARRIVES FOLDED by the dialect's `assembly.spellingCase`
    //     — the engine folds before it asks, so a host compares EXACTLY and
    //     never folds again. That direction is deliberate: a host would have to
    //     be handed the dialect to fold correctly, and the two hosts folding
    //     independently is precisely how the standalone `.s` path and the
    //     embedded template path would come to disagree about what `X0` is.
    //
    //   • A TEMPLATE OPERAND PLACEHOLDER (`decodePlaceholder`) ARRIVES VERBATIM
    //     — NOT folded, under any `spellingCase`. ✔MEASURED: GNU 6.47.2.3
    //     symbolic operand names are ordinary C identifiers and gcc is
    //     case-SENSITIVE about them, so `%[Out]` does not name an operand
    //     declared `[out]`. `spellingCase` exists because gas is
    //     case-insensitive about ITS OWN vocabulary (mnemonics, registers,
    //     directives, operand selectors); a placeholder names an operand of the
    //     EMBEDDING language and is not gas vocabulary at all. Folding it would
    //     merge two distinct operands into one under both shipped dialects,
    //     which are `asciiFolded`, and hand the caller whichever the binding
    //     table listed first — a silent miscompile, not a conformance fix.
    //
    // ⚠ CONSEQUENCE FOR A HOST WITH A LOOKUP TABLE OF ITS OWN (the template
    // host's operand BINDINGS): a binding whose spelling is a PLACEHOLDER is
    // registered and matched EXACTLY AS THE TEMPLATE WRITES IT — do not fold it
    // — while a binding whose spelling is a REGISTER NAME must be registered in
    // the dialect's folded form, because that is the form this virtual will be
    // handed. `TemplateHost::bindingFor` compares with `==` and therefore
    // inherits both rules unchanged.
    // ⚠ THE PARAGRAPH THIS REPLACED SAID ONLY THE FIRST HALF ("the engine folds
    // before it asks … a binding must be registered in folded form") and was
    // written when `decodeRegister` was the only caller. It became false for one
    // of two callers the moment the placeholder decode landed, and a host author
    // who followed it would have registered `%[out]` for a source-spelled
    // `%[Out]` and got a refusal.
    // ⓘ THIS IS DOCUMENTATION, NOT ENFORCEMENT, AND THE DISTINCTION IS HONEST:
    // nothing in the signature stops the two callers from drifting. Enforcing it
    // would mean carrying the discriminator IN the signature (a `spelling KIND`
    // parameter, or two virtuals), which is a larger change than the
    // documentation defect it would fix. What holds the line today is
    // `SpellingCaseSplitsAtThePlaceholderSeam` in
    // tests/asm/test_asm_template_to_lir.cpp, which asserts BOTH halves against
    // ONE `asciiFolded` dialect.
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

// ★★★ PARSE ONE EMBEDDED ASSEMBLY TEMPLATE INTO A TREE — the step BEFORE
// `lowerAsmTemplateToLirRun`, and the reason it is a function of this file.
//
// ★★ A TEMPLATE IS NOT LEXED LIKE A `.s`, AND ONLY THIS TIER KNOWS THAT.
// ✔MEASURED on gcc 13.3.0 and clang 18.1.3 (sources fed as base64):
// `__asm__("xorl %eax,%eax")` — BASIC — assembles with `%` LITERAL;
// `__asm__("xorl %eax,%eax" ::: "eax")` — EXTENDED, and ANY colon makes it
// extended — is REJECTED BY BOTH ("operand number missing after %-letter" /
// "invalid % escape in inline assembly string"); `%%eax` assembles. So in a
// template `%` introduces an OPERAND and the register sigil must be doubled,
// while in a `.s` `%` IS the register sigil. The dialect states that with a
// per-mode `tokens` override and names the mode in `assembly.templateLexerMode`;
// this function is the "template parse entry" that selects it.
// ⇒ a caller that tokenized the template itself would get the `.s` surface and
// every placeholder would die at the parser, which is exactly the state this
// function was added to end.
//
// ★ IT ALSO OWNS THE TRAILING NEWLINE. The dialect is line-oriented — the
// newline IS its statement terminator — so a template whose last line has no
// `\n` would lose that line entirely. That is a property of the DIALECT
// SURFACE, not of any particular caller, so it is applied here once rather
// than remembered at every call site.
//
// ★★★ WHICH OF THE TWO SURFACES A TEMPLATE IS READ ON, AND IT IS THE CALLER'S
// FACT TO STATE — there is no default, because a wrong default is silent.
//
// ✔MEASURED on gcc 13.3.0 and clang 18.1.3, sources fed as base64 so no shell
// could alter a byte, with matched positive controls:
//
//   `__asm__("xorl %eax,%eax")`               BASIC     → both accept; `%` is
//                                                         LITERAL and the text
//                                                         reaches the assembler
//                                                         verbatim.
//   `__asm__("xorl %eax,%eax" ::: "eax")`     EXTENDED  → both REJECT ("operand
//                                                         number missing after
//                                                         %-letter" / "invalid
//                                                         % escape").
//   `__asm__("xorl %%eax,%%eax" ::: "eax")`   EXTENDED  → both accept.
//
// ⇒ ANY colon makes a template extended, and the two forms genuinely differ IN
// THE LEXER rather than only in what the parser accepts. A BASIC template is
// therefore read on the `.s` surface — whatever the reference ASSEMBLER takes,
// this takes — and needs no placeholder vocabulary at all; an EXTENDED one is
// read on the dialect's template surface, where `%` introduces an operand.
// ⚠ Reading a BASIC template on the extended surface would refuse `%eax`, which
// gcc and clang both COMPILE; reading an EXTENDED one on the `.s` surface would
// accept `%eax`, which they both REJECT. Both directions are conformance
// defects, which is why this is a parameter and not a policy.
enum class AsmTemplateSurface : std::uint8_t {
    Basic,      // no operand sections — the `.s` surface, `%` literal
    Extended,   // any `:` at all — the dialect's `templateLexerMode`
};

// Returns nullopt with at least one diagnostic reported when the template did
// not parse — the dialect's OWN parse diagnostics are forwarded to `reporter`
// first, so the caller can add its context and never has to invent a reason —
// or, for `Extended` only, when the dialect declares no template surface.
[[nodiscard]] DSS_EXPORT std::optional<Tree>
parseAsmTemplateText(std::string                          templateText,
                     std::string                          bufferName,
                     std::shared_ptr<GrammarSchema const> dialect,
                     AsmTemplateSurface                   surface,
                     DiagnosticBudget                     budget,
                     DiagnosticReporter&                  reporter);

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
