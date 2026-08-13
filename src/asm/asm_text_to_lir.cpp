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

// One dialect instruction row, resolved against the ACTIVE target. Built once
// per lowering, before any statement is walked, so a dialect/target
// disagreement is reported for the CONFIG rather than for the first `.s` line
// that happened to use it.
struct ResolvedRow {
    std::optional<CfClass>        cfClass;   // nullopt: no candidate resolves here
    std::optional<TargetCondCode> cond;
    // Do this row's resolvable candidates read the instruction payload as a
    // condition code (`consumesCondCode`)? Unanimous by construction — a row
    // whose candidates disagree is refused at resolve time — so the emit walk
    // can put `cond` into the payload without re-deciding per election.
    bool                          consumesCond = false;
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
        // PASS 2 — emit.
        if (!emitAll()) return std::nullopt;

        AsmTextModule out;
        out.lir             = std::move(builder_).finish();
        out.symbols         = std::move(symbols_);
        out.userEntrySymbol = userEntry_;
        out.externImports   = std::move(externs_);
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
            // target declares. Candidates that disagree make the row
            // unusable — the operand shape of a branch and of an arithmetic
            // instruction are not the same kind of thing, so there would be no
            // shape to elect over.
            std::optional<CfClass> cls;
            std::string_view       clsFrom;
            bool                   mixed = false;
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
                CfClass const here = cfClassOf(*info);
                if (!cls.has_value()) { cls = here; clsFrom = name; continue; }
                if (*cls != here) {
                    fail(NodeId{},
                         std::format("mnemonic '{}' lists target opcodes '{}' "
                                     "and '{}', which are {} and {} — one "
                                     "spelling cannot denote both, because the "
                                     "two take different operand shapes and "
                                     "there would be nothing to elect over{}",
                                     row.spelling, clsFrom, name,
                                     cfClassName(*cls), cfClassName(here),
                                     pairSuffix()));
                    mixed = true;
                    break;
                }
            }
            if (mixed) continue;
            out.cfClass = cls;

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
            if (consumes.value_or(false) && row.condName.empty()) {
                fail(NodeId{},
                     std::format("mnemonic '{}' resolves to target opcode '{}', "
                                 "whose encoding reads a condition code from the "
                                 "instruction payload, but declares no 'cond' — "
                                 "the condition would default to the target's "
                                 "zero code and encode the wrong instruction "
                                 "with no diagnostic{}",
                                 row.spelling, consumesFrom, pairSuffix()));
                continue;
            }
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

    // The `LirInst.payload` a row's instructions carry: the declared condition
    // when the elected opcode's encoder reads one (`condCodeFromPayload`), and
    // 0 otherwise — which is what every non-conditional opcode has always
    // carried.
    //
    // ⚠ NOT `value_or(0)`. Silently encoding the target's zero condition when
    // the invariant broke is exactly the miscompile the resolve-time pair
    // exists to prevent — `sete` would become `seto` with no diagnostic. The
    // invariant is established above; a violation is a substrate bug and says
    // so.
    [[nodiscard]] std::uint32_t payloadFor(ResolvedRow const& resolved,
                                           DecodedInstruction const& ins) {
        if (!resolved.consumesCond) return 0;
        if (!resolved.cond.has_value()) {
            fail(ins.node,
                 std::format("internal: '{}' resolves to an opcode that reads a "
                             "condition from the instruction payload, but the "
                             "row carries no resolved condition — the "
                             "resolve-time pairing that guarantees this did not "
                             "hold{}", ins.mnemonic, pairSuffix()));
            return 0;
        }
        return static_cast<std::uint32_t>(*resolved.cond);
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
        SymbolId    symbol{};              // valid only when `isEntry`
        std::size_t functionLabel = kNoLabel;  // index of the owning entry
        LirBlockId  block{};
        bool        opened = false;
    };

    bool scanUnit() {
        walkElements(tree_.root(), [&](NodeId element) {
            if (!ok_) return;
            if (tree_.rule(element).v == cfg_.directiveRule.v) {
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
                std::string name{tree_.text(label)};
                if (labelIndex_.contains(name)) {
                    fail(label,
                         std::format("label '{}' is defined more than once",
                                     name));
                    return;
                }
                labelIndex_.emplace(name, labels_.size());
                LabelInfo info;
                info.name = std::move(name);
                info.at   = label;
                labels_.push_back(std::move(info));
                // A label chain may end in a DIRECTIVE (`main: .globl main`);
                // `nextStatementAfterLabel` only follows the statement arm, so
                // the directive arm is picked up when the walk reaches it.
                cur = nextStatementAfterLabel(cur);
            }
        });
        return ok_;
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
        switch (row->verb) {
        case AsmDirectiveVerb::SectionText:
            // Every function this walker emits already lands in the text
            // section — LIR has no other. Accepting the directive states that
            // the file's intent and the walker's behaviour agree; a `.data`
            // spelling has no verb and is refused above, which is the honest
            // answer while data emission is unmodelled.
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
            if (L.isEntry) {
                L.symbol = SymbolId{static_cast<std::uint32_t>(functionCount_)};
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
        walkElements(tree_.root(), [&](NodeId element) {
            if (!ok_) return;
            // Directives were applied in pass 1; re-applying them here would
            // double-report every directive diagnostic.
            if (tree_.rule(element).v == cfg_.directiveRule.v) return;
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
                    // The tail may carry another element on the same line.
                    auto const tailKids = visibleChildren(tree_, tail);
                    cur = NodeId{};
                    for (NodeId const tk : tailKids) {
                        if (tree_.kind(tk) != NodeKind::Internal) continue;
                        if (tree_.rule(tk).v == cfg_.directiveRule.v) break;
                        if (tree_.rule(tk).v == cfg_.statementRule.v) {
                            cur = tk;
                            break;
                        }
                    }
                    continue;
                }
                emitInstruction(cur, name,
                                findDescendantOfRule(tree_, rawTail,
                                                     cfg_.operandSeqRule));
                return;
            }
        });
        if (!ok_) return false;
        closeFunction();
        return ok_;
    }

    // ── functions and blocks ──────────────────────────────────────────────
    void enterLabel(std::string const& name, NodeId at) {
        auto const it = labelIndex_.find(name);
        if (it == labelIndex_.end()) {
            fail(at, std::format("label '{}' was not collected", name));
            return;
        }
        auto& L = labels_[it->second];
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
        if (!resolved.cfClass.has_value()) {
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

    // The register-name text a node spells: the LAST visible token, because a
    // sigil (`%`) leads and carries no identity. Shared by the role
    // disambiguation and by `decodeRegister`, so the two can never disagree
    // about which token is the name.
    [[nodiscard]] std::string_view registerNameOf(NodeId node) const {
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
            auto const name = registerNameOf(node);
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
                out.disp = disp;
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
    // AN OPTIONAL SCALE" — which is LIR's own addressing model, not a dialect
    // shape. Base is the first register the form names, index the second; the
    // scale is the one numeric token that is not inside either of them (a
    // displacement never lives here — the dialect puts it in the sibling
    // scalar). A dialect whose memory form nests differently binds `memory` to
    // its own rule and this reading is unchanged.
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
        // The scale: numeric tokens outside every register subtree.
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
        bool negate = false;
        NodeId cur  = node;
        if (tree_.kind(cur) == NodeKind::Internal
            && tree_.rule(cur).v
                   == cfg_.ruleForRole(AsmOperandRole::NegNumber).v) {
            negate = true;
        }
        std::string_view text;
        for (NodeId const k : visibleChildren(tree_, cur)) {
            if (tree_.kind(k) == NodeKind::Token) {
                text = tree_.text(k);
                if (tree_.kind(cur) == NodeKind::Internal && negate) continue;
            }
        }
        if (text.empty() && tree_.kind(cur) == NodeKind::Token) {
            text = tree_.text(cur);
        }
        if (text.empty()) {
            // Nested one level deeper (an alt wrapper): take the deepest token.
            NodeId probe = cur;
            while (tree_.kind(probe) == NodeKind::Internal) {
                auto const kids = visibleChildren(tree_, probe);
                if (kids.empty()) break;
                probe = kids.back();
            }
            if (tree_.kind(probe) == NodeKind::Token) text = tree_.text(probe);
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
        out.symbol = std::string{text};
        return true;
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
    void buildLirInst(DecodedInstruction const& ins,
                      AsmInstructionSpelling const& row,
                      ResolvedRow const& resolved) {
        // ⚠ A CONTROL-FLOW INSTRUCTION'S REGISTER OPERAND IS AN ADDRESS, NOT
        // DATA (`call *%rax`, `br x16`), so its width says nothing about the
        // operation and must not be derived from.
        bool const dataOperands = *resolved.cfClass == CfClass::Plain;
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
        std::uint32_t const payload = payloadFor(resolved, ins);
        if (!ok_) return;

        switch (*resolved.cfClass) {
        case CfClass::Return:      buildReturn(ins, row, payload, flags); return;
        case CfClass::Br:          buildBr(ins, row, payload, flags); return;
        case CfClass::CondBr:      buildCondBr(ins, row, payload, flags); return;
        case CfClass::Call:        buildCall(ins, row, payload, flags, widthBits); return;
        case CfClass::IndirectBr:
        case CfClass::Unreachable:
            fail(ins.node,
                 std::format("'{}' is {}, which this build does not lower from "
                             "assembly text: its successor set is not something "
                             "a `.s` states, and inventing one would silently "
                             "change the control-flow graph{}",
                             ins.mnemonic, cfClassName(*resolved.cfClass),
                             pairSuffix()));
            return;
        case CfClass::Plain:       break;
        }

        std::size_t const n = ins.operands.size();
        if (n == 0) {
            fail(ins.node,
                 std::format("'{}' has no operands, which this build cannot "
                             "map onto {}{}", ins.mnemonic,
                             candidateList(row), pairSuffix()));
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
                    fail(src.node,
                         std::format("symbolic operand '{}' is not yet lowered "
                                     "by this build{}", src.symbol,
                                     pairSuffix()));
                    return;
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
        for (auto const& name : row.opcodeNames) {
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
                electAmong(consumers, operands, widthBits, row, ins);
            if (!chosen.has_value()) return;
            if (!checkElectedWidth(*chosen, row, *width, ins)) return;
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
        Election ce =
            anyMemory ? Election{}
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
            if (!checkElectedWidth(*ce.opcode, row, *width, ins)) return;
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
        if (!checkElectedWidth(*chosen, row, *width, ins)) return;
        builder_.addInst(chosen->opcode, destReg, operands, payload, flags);
        ++blockInstCount_;
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

    [[nodiscard]] std::string
    candidateList(AsmInstructionSpelling const& row) const {
        std::string names;
        for (auto const& n : row.opcodeNames) {
            if (!names.empty()) names += ", ";
            names += '\'';
            names += n;
            names += '\'';
        }
        return std::format("target opcode(s) {}", names);
    }

    [[nodiscard]] TargetOpcodeInfo const*
    twoAddressCandidate(AsmInstructionSpelling const& row) const {
        for (auto const& name : row.opcodeNames) {
            auto const ordinal = target_.opcodeByMnemonic(name);
            if (!ordinal) continue;
            auto const* info = target_.opcodeInfo(*ordinal);
            if (info != nullptr && info->requires2Address) return info;
        }
        return nullptr;
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
               AsmInstructionSpelling const& row,
               DecodedInstruction const& ins) {
        (void)row;
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
                           AsmInstructionSpelling const& row,
                           std::uint32_t width,
                           DecodedInstruction const& ins) {
        (void)row;
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
                     AsmInstructionSpelling const& row, std::uint32_t payload,
                     std::uint8_t flags) {
        if (!ins.operands.empty()) {
            fail(ins.node,
                 std::format("'{}' is a return and this build lowers only its "
                             "operand-less form{}", ins.mnemonic,
                             pairSuffix()));
            return;
        }
        auto const elected = electAmong(
            row.opcodeNames, std::span<LirOperand const>{},
            lirInstWidthBits(flags), row, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, row, lirInstWidthBits(flags), ins)) {
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
                 AsmInstructionSpelling const& row, std::uint32_t payload,
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
            electAmong(row.opcodeNames, ops, lirInstWidthBits(flags), row, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, row, lirInstWidthBits(flags), ins)) {
            return;
        }
        builder_.addBr(elected->opcode, *target, payload, flags);
        openTerminated_ = true;
        ++blockInstCount_;
    }

    void buildCondBr(DecodedInstruction const& ins,
                     AsmInstructionSpelling const& row,
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
            electAmong(row.opcodeNames, ops, lirInstWidthBits(flags), row, ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, row, lirInstWidthBits(flags), ins)) {
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
    // ★ SYMBOL IDS CONTINUE THE FUNCTION NUMBERING. `classifyLabels` (pass 1b)
    // hands the function-entry labels 0..functionCount_-1 and has already run
    // when any statement is emitted, so `functionCount_ + n` cannot collide
    // with a defined symbol — which the linker's per-CU `declare()` would
    // otherwise reject as a duplicate.
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
        row.symbol = SymbolId{static_cast<std::uint32_t>(functionCount_
                                                         + externs_.size())};
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
                   AsmInstructionSpelling const& row, std::uint32_t payload,
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
        auto const elected = electAmong(row.opcodeNames, ops, widthBits, row,
                                        ins);
        if (!elected.has_value()) return;
        if (!checkElectedWidth(*elected, row, widthBits, ins)) return;
        // ★ A CALL IS NOT A TERMINATOR — plain `addInst`. And this path runs
        // POST-callconv (no pass follows), so the operand list is exactly the
        // callee reference: the argument registers were set by the instructions
        // the programmer wrote above it.
        builder_.addInst(elected->opcode, InvalidLirReg, ops, payload,
                         flags);
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
