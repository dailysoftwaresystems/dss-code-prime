#include "asm/asm_text_to_lir.hpp"

#include "asm/asm_template_to_lir.hpp"
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

using asm_walk::collectDescendantsOfRule;
using asm_walk::findDescendantOfRule;
using asm_walk::firstVisibleToken;
using asm_walk::lastVisibleToken;
using asm_walk::visibleChildren;

namespace {

// ★★★ THE STANDALONE `.s` UNIT WALKER — labels, sections, functions, blocks
// and data — AND THE `AsmLoweringHost` THAT ANSWERS THE INSTRUCTION ENGINE'S
// QUESTIONS ABOUT THEM.
//
// ★★ WHAT MOVED OUT AND WHY. Everything that turns ONE dialect statement into
// ONE LIR instruction — the operand decode, the width reconciliation, the
// operand-shape walk, the opcode election, the control-flow arms — now lives in
// `asm_template_to_lir.cpp` and is reached through `AsmInstructionLowering`,
// because an embedded `__asm__` template needs exactly that and nothing else.
// What stayed is everything that is about the FILE: a template has no labels,
// no sections, no data directives and no functions of its own.
// ⇒ this class is the standalone half of a two-caller split, and its `Asm
// LoweringHost` override set is precisely the list of facts the engine cannot
// derive. Read that list to see the shape of the seam.
class AsmTextLowering final : public AsmLoweringHost {
public:
    AsmTextLowering(Tree const& tree, GrammarSchema const& grammar,
                    TargetSchema const& target,
                    std::span<std::string const> entryNames,
                    DiagnosticReporter& reporter)
        : tree_(tree), grammar_(grammar), target_(target),
          cfg_(grammar.assembly()), entryNames_(entryNames),
          builder_(target), sink_(tree, grammar, target, reporter),
          engine_(tree, grammar, target, builder_, sink_, *this) {}

    std::optional<AsmTextModule> run() {
        // ⚠ NOT AN ASSERT. A driver that routes a non-dialect grammar here is a
        // bug, and the bug must announce itself rather than produce an empty
        // module that links to a program doing nothing.
        if (!cfg_.declared) {
            sink_.fail(tree_.root(),
                 std::format("language '{}' has no 'assembly' block, so it "
                             "cannot lower a standalone assembly unit — the "
                             "'encode' pipeline entry was reached for a "
                             "language that declares no instruction vocabulary",
                             grammar_.name()));
            return std::nullopt;
        }

        // PASS 0 — cross-check the DIALECT against the TARGET, once, for every
        // row. See `resolveRows`.
        if (!engine_.resolveRows()) return std::nullopt;
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
        if (!sink_.ok()) return std::nullopt;

        AsmTextModule out;
        out.lir                 = std::move(builder_).finish();
        out.symbols             = std::move(symbols_);
        out.userEntrySymbol     = userEntry_;
        out.externImports       = std::move(externs_);
        out.dataItems           = std::move(dataItems_);
        out.blockSymbolBindings = std::move(blockSymbolBindings_);
        out.perFuncCfi          = std::move(perFuncCfi_);
        out.cfiInitial          = cfiInitial_;
        return out;
    }

    // ── AsmLoweringHost — the facts the instruction engine cannot derive ───
    //
    // ★ EVERY ONE OF THESE IS A LABEL / SECTION / BLOCK FACT, and that is the
    // whole content of "standalone" as opposed to "embedded". The bodies are
    // the ones the engine used to inline against this class's own state; they
    // moved here unchanged, so the diagnostics a `.s` sees are byte-identical.

    [[nodiscard]] bool namesRegister(std::string_view spelling) const override {
        return target_.registerByName(spelling).has_value();
    }

    [[nodiscard]] AsmRegisterLookup
    resolveRegister(std::string_view spelling, NodeId at,
                    AsmResolvedRegister& out) override {
        // ⚠ THE SAME LOOKUP `namesRegister` ANSWERS FROM, so the role
        // disambiguation and the decode cannot disagree — the contract the
        // host interface states, satisfied by construction rather than by
        // inspection.
        return resolvePhysicalRegister(target_, spelling, at, sink_, out);
    }

    [[nodiscard]] std::optional<std::string_view>
    openDataSectionName() const override {
        if (!emitSection_.has_value()) return std::nullopt;
        return dataSectionKindName(*emitSection_);
    }

    [[nodiscard]] bool hasOpenFunction() const override {
        return openFunctionLabel_ != kNoLabel;
    }

    [[nodiscard]] bool blockIsTerminated() const override {
        return openTerminated_;
    }

    [[nodiscard]] std::string_view enclosingFunctionName() const override {
        if (openFunctionLabel_ == kNoLabel) return {};
        return labels_[openFunctionLabel_].name;
    }

    // ★★★ M2 — THE TWO ADDRESS SHAPES, AND WHICH ONE A NAME GETS.
    //   * a DATA or FUNCTION label → `[SymbolRef]`, what `lowerGlobalAddr`
    //     emits for `&global`;
    //   * an INTERIOR label → `[SymbolRef, BlockRef]`, what `lowerBlockAddress`
    //     emits for `&&label`.
    [[nodiscard]] bool
    appendSymbolAddress(std::string const& symbol, NodeId at,
                        std::string_view mnemonic,
                        std::vector<LirOperand>& out) override {
        auto const it = labelIndex_.find(symbol);
        if (it == labelIndex_.end()) {
            // ⚠ REFUSED RATHER THAN IMPORTED, for the reason
            // `bindPendingDataSymbols` states: `ExternImport::isData` selects
            // the linker's indirection slot and an address-materializing
            // instruction says nothing about code-vs-data. Anchored:
            // D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL.
            sink_.fail(at,
                 std::format("'{}' takes the address of '{}', which this file "
                             "defines no label for. An undefined name would "
                             "have to become an import, and an import states "
                             "whether it is CODE or DATA — which selects the "
                             "linker's indirection slot — while an address "
                             "operand says neither. A CALL is the one reference "
                             "that answers it, which is why only a call mints "
                             "one today{}", mnemonic, symbol,
                             sink_.pairSuffix()));
            return false;
        }
        std::size_t const labelIdx = it->second;
        auto const&       L        = labels_[labelIdx];
        if (L.isEntry || L.isData) {
            // Already carries its symbol — a function entry from
            // `classifyLabels`, a data label from the scan.
            out.push_back(LirOperand::makeSymbolRef(L.symbol.v));
            return true;
        }
        // ⚠ AN INTERIOR LABEL OF ANOTHER FUNCTION IS REFUSED, exactly as
        // `resolveBranchTarget` refuses a cross-function branch and for the
        // identical reason: `makeBlockRef` names a block SLOT, and a slot from
        // another function resolves to whatever block sits at that index here.
        // The binding would be silently wrong rather than absent.
        if (L.functionLabel != openFunctionLabel_) {
            sink_.fail(at,
                 std::format("'{}' takes the address of '{}', a label inside a "
                             "DIFFERENT function — the block reference that "
                             "binds an interior label's symbol to its byte "
                             "offset is function-local, so this address cannot "
                             "be expressed here{}",
                             mnemonic, symbol, sink_.pairSuffix()));
            return false;
        }
        SymbolId const sym = symbolForAddressedLabel(labelIdx);
        out.push_back(LirOperand::makeSymbolRef(sym.v));
        out.push_back(LirOperand::makeBlockRef(labels_[labelIdx].block.v));
        return true;
    }

    // ⚠ A BRANCH TARGET IS FUNCTION-LOCAL: `LirOperand::makeBlockRef` names a
    // block slot, and a slot from another function would silently resolve to
    // whatever block sits at that index here.
    [[nodiscard]] std::optional<LirBlockId>
    resolveBranchTarget(std::string const& symbol, NodeId at,
                        std::string_view mnemonic) override {
        auto const it = labelIndex_.find(symbol);
        if (it == labelIndex_.end()) {
            sink_.fail(at,
                 std::format("'{}' branches to '{}', which this file defines no "
                             "label for — a branch out of the translation unit "
                             "is a relocation this build does not reach from "
                             "assembly yet{}", mnemonic, symbol,
                             sink_.pairSuffix()));
            return std::nullopt;
        }
        auto const& L = labels_[it->second];
        if (L.isEntry || L.functionLabel != openFunctionLabel_) {
            sink_.fail(at,
                 std::format("'{}' branches to '{}', which belongs to a "
                             "different function — LIR block references are "
                             "function-local, so this edge cannot be "
                             "expressed{}", mnemonic, symbol,
                             sink_.pairSuffix()));
            return std::nullopt;
        }
        return L.block;
    }

    [[nodiscard]] std::optional<LirOperand>
    resolveCallee(std::string const& symbol, NodeId at,
                  std::string_view mnemonic) override {
        auto const it = labelIndex_.find(symbol);
        if (it != labelIndex_.end()) {
            // ⚠ A LABEL THIS FILE DEFINES BUT DID NOT MARK AS A FUNCTION
            // ENTRY IS A **BLOCK**, AND A BLOCK IS NOT A CALL TARGET. It
            // has no module symbol (only function-entry labels are minted
            // one), so there is nothing for a call's SymbolRef to name, and
            // the alternatives are both silent: treating it as an extern
            // would import a name this very file defines, and treating it
            // as a function would call into the middle of another frame.
            if (!labels_[it->second].isEntry) {
                sink_.fail(at,
                     std::format("'{}' calls '{}', which this file defines "
                                 "as a BLOCK inside another function rather "
                                 "than as a function entry — a call needs a "
                                 "function symbol, and calling into an "
                                 "interior label would enter a frame whose "
                                 "prologue never ran. Mark it with this "
                                 "dialect's function-entry directive ({}) "
                                 "if it really is one{}",
                                 mnemonic, symbol,
                                 cfg_.spellingsForVerb(
                                     AsmDirectiveVerb::FunctionEntry),
                                 sink_.pairSuffix()));
                return std::nullopt;
            }
            return LirOperand::makeSymbolRef(labels_[it->second].symbol.v);
        }
        // ★★★ AN UNDEFINED CALLEE IS AN EXTERN, WITH NO DIRECTIVE AND NO
        // GUESS. See `AsmTextModule::externImports` for why gas has no extern
        // directive and why `libraryPath` stays empty; the reference gate at
        // the link tier is what judges the reference.
        return LirOperand::makeSymbolRef(internExtern(symbol).v);
    }

    [[nodiscard]] std::vector<LirBlockId>
    addressTakenSuccessors() const override {
        return derivableIndirectSuccessors();
    }

    void onInstructionEmitted() override {
        ++blockInstCount_;
        ++funcInstCount_;
    }
    void onTerminatorEmitted() override { openTerminated_ = true; }
    void onBlockOpened(LirBlockId) override {
        openTerminated_ = false;
        blockInstCount_ = 0;
    }

private:
    // ★ THE DATA DIRECTIVES READ THEIR OPERANDS THROUGH THE SAME DECODER THE
    // INSTRUCTION ENGINE USES. `.quad Lcase0` and `movq $8,%rax` must agree
    // about what a scalar is, what a negated literal is and which tokens spell
    // a dotted name — and two decoders is exactly how they stop agreeing (the
    // measured `D-ASM-NEGATIVE-SCALAR-LOSES-ITS-SIGN` / `D-ASM-DOTTED-NAME-NOT-AN-OPERAND`
    // pair are both "one reading, applied in one place only").
    [[nodiscard]] std::optional<AsmDecodedOperand> decodeOperand(NodeId node) {
        AsmDecodedOperand out;
        if (!engine_.decodeOperandInto(node, out)) return std::nullopt;
        return out;
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
            sink_.fail(at, std::format("label '{}' is defined more than once", name));
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
        if (!sink_.ok()) return;
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
        while (cur.valid() && sink_.ok()) {
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
        return sink_.ok();
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
            sink_.fail(directive, "a directive needs a name");
            return;
        }
        std::string const spelling{tree_.text(kids[1])};
        auto const* row = cfg_.directiveBySpelling(spelling);
        if (row == nullptr) {
            sink_.fail(kids[1],
                 std::format("unknown assembler directive '.{}' — this build "
                             "refuses an unrecognized directive rather than "
                             "ignoring it, because a silently-dropped directive "
                             "changes the binary with no diagnostic{}",
                             spelling, sink_.pairSuffix()));
            return;
        }
        // ★★ A SECTION NAME IS NOT A DIRECTIVE
        // (D-ASM-SECTION-DIRECTIVE-WITH-OPERAND-UNMODELLED).
        // The row exists so `.section rodata` can resolve;
        // writing it bare is what gas itself refuses, and the refusal names the
        // form that works rather than saying "unknown directive" about a
        // spelling the dialect visibly declares.
        if (row->operandOnly) {
            sink_.fail(kids[1],
                 std::format("'.{}' is a SECTION NAME in this dialect, not a "
                             "directive — it is reachable only as the operand "
                             "of {}, which is exactly what the reference "
                             "assembler accepts ('.section .{}' assembles; a "
                             "bare '.{}' is an unknown pseudo-op){}",
                             spelling,
                             cfg_.spellingsForVerb(
                                 AsmDirectiveVerb::SectionByName),
                             spelling, spelling, sink_.pairSuffix()));
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
                sink_.fail(directive, std::format(".{} needs a symbol name",
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
                sink_.fail(directive,
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
                sink_.fail(directive,
                     std::format(".{} needs the symbol it marks as a function "
                                 "entry", spelling));
                return;
            }
            if (markerSeen) functionEntryNames_.insert(*named);
            return;
        }
        case AsmDirectiveVerb::IgnoredAnnotation:
            return;
        // ★★★ THE CALL-FRAME FAMILY IS APPLIED IN PASS 2, NOT HERE, AND THE
        // REASON IS THE ONE FACT PASS 1 CANNOT KNOW: **WHICH INSTRUCTION THE
        // RULE FOLLOWS.** A `.cfi_*` rule takes effect at the PC reached by the
        // instruction above it, and no LIR instruction exists until the emit
        // walk. Recording it here against a source position and re-resolving it
        // later would be a second ordering to keep in step with the first.
        // ⚠ SO THEY ARE SILENT HERE RATHER THAN VALIDATED-TWICE — the same
        // split `trackSection` uses one direction and this uses the other: pass
        // 2 does the whole job, diagnostics included, so no directive is ever
        // reported by both passes.
        case AsmDirectiveVerb::FrameStart:
        case AsmDirectiveVerb::FrameEnd:
        case AsmDirectiveVerb::FrameRule:
        case AsmDirectiveVerb::FrameReturnColumn:
            return;
        // ★★★ DECLARED AND DELIBERATELY REFUSED.
        // D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED.
        // The reference assembler takes this directive; this
        // build cannot represent what it states; so it is refused BY NAME
        // instead of accepted and dropped. Reported in PASS 1 because the
        // refusal needs nothing the emit walk provides — and a pass-1 refusal
        // stops the run before pass 2, so it can never be reported twice.
        case AsmDirectiveVerb::Unrepresentable:
            sink_.fail(kids[1],
                 std::format("'.{}' is declared by this dialect as "
                             "UNREPRESENTABLE: the reference assembler accepts "
                             "it, and this build has no way to carry what it "
                             "states, so it is refused by name rather than "
                             "parsed and dropped. Accepting it would produce a "
                             "binary that runs correctly and describes its own "
                             "frames wrongly — the failure is invisible until "
                             "something tries to unwind. See this directive's "
                             "'$comment' in the dialect document for the "
                             "specific fact that cannot be represented{}",
                             spelling, sink_.pairSuffix()));
            return;
        }
        sink_.fail(kids[1], "unhandled directive verb");
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
            sink_.fail(at,
                 std::format("internal: directive '.{}' declares data section "
                             "'{}', which the substrate's DataSectionKind "
                             "vocabulary does not name — the load-time "
                             "validation that guarantees this did not hold{}",
                             spelling, row.sectionName, sink_.pairSuffix()));
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
            sink_.fail(directive,
                 std::format("'.{}' needs the section it opens as its operand "
                             "(one of {}){}",
                             spelling, cfg_.sectionOperandSpellings(),
                             sink_.pairSuffix()));
            return nullptr;
        }
        std::vector<NodeId> operands;
        for (NodeId const o : visibleChildren(tree_, kids[2])) {
            if (tree_.kind(o) == NodeKind::Internal) operands.push_back(o);
        }
        if (operands.empty()) {
            sink_.fail(directive,
                 std::format("'.{}' needs the section it opens as its operand "
                             "(one of {}){}",
                             spelling, cfg_.sectionOperandSpellings(),
                             sink_.pairSuffix()));
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
            sink_.fail(operands[1],
                 std::format("'.{}' carries {} operands; this build models the "
                             "section NAME and nothing else. The flags/type "
                             "operands ('{}' here) change what the section IS — "
                             "writable, executable, zero-fill — and this build "
                             "derives every one of those from the section kind, "
                             "so honouring the name while dropping them would "
                             "place data somewhere the source did not ask for, "
                             "with no diagnostic{}",
                             spelling, operands.size(),
                             tree_.text(operands[1]), sink_.pairSuffix()));
            return nullptr;
        }
        std::string_view const introducer = tree_.text(kids[0]);
        NodeId const lead = firstVisibleToken(tree_, operands[0]);
        if (!lead.valid() || tree_.text(lead) != introducer) {
            sink_.fail(operands[0],
                 std::format("'.{}' names its section WITH this dialect's "
                             "directive introducer ('{}'); '{}' does not carry "
                             "it. The reference assembler treats the two as "
                             "DIFFERENT sections — an undotted name creates a "
                             "section of exactly that name with no allocation "
                             "flag — so they are not synonyms{}",
                             spelling, introducer, tree_.text(operands[0]),
                             sink_.pairSuffix()));
            return nullptr;
        }
        NodeId const nameTok = lastVisibleToken(tree_, operands[0]);
        std::string_view const name =
            nameTok.valid() ? tree_.text(nameTok) : std::string_view{};
        auto const* named = cfg_.sectionRowByName(name);
        if (named == nullptr) {
            sink_.fail(operands[0],
                 std::format("'.{}' names section '{}{}', which this dialect "
                             "does not declare — the sections it can open are "
                             "{}. A section this build cannot place is refused "
                             "by name rather than mapped onto a different one, "
                             "because data in the wrong section is read-only "
                             "where the program writes it, or writable where it "
                             "must not be{}",
                             spelling, introducer, name,
                             cfg_.sectionOperandSpellings(), sink_.pairSuffix()));
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
        sink_.fail(at,
             std::format("'.{}' defines data, but no data section is open — "
                         "bytes written here would land in the TEXT section and "
                         "be executed. To open one, {}{}",
                         spelling, how, sink_.pairSuffix()));
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
            sink_.fail(directive,
                 std::format("'.{}' writes bytes, but the open section is "
                             "zero-fill ({}) — the wire format reserves its "
                             "size WITHOUT storing file bytes, so the bytes "
                             "would be silently dropped{}",
                             spelling, dataSectionKindName(*scanSection_),
                             sink_.pairSuffix()));
            return;
        }
        if (kids.size() < 3) {
            sink_.fail(directive,
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
                // RELOCATION IS ALL IT IS
                // (D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET). `.quad Lcase0` writes
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
                    sink_.fail(decoded->node,
                         std::format("'.{}' takes a value or a symbol name, and "
                                     "this operand is neither{}",
                                     spelling, sink_.pairSuffix()));
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
                                            AsmDecodedOperand const&   operand,
                                            std::uint32_t           unitBytes,
                                            std::string_view        spelling) {
        auto const kind = absoluteRelocKind(unitBytes);
        if (!kind.has_value()) {
            // ⚠ REFUSED, NOT NARROWED TO A DIFFERENT WIDTH. `.long Lw` on a
            // target with only an 8-byte absolute relocation would silently
            // store the low half of an address; a table read through it jumps
            // somewhere that is not the label.
            sink_.fail(operand.node,
                 std::format("'.{}' names '{}', which needs an ABSOLUTE "
                             "{}-byte relocation to write that address at link "
                             "time, and this target declares none (its "
                             "relocations are matched by the width/pc-relative "
                             "FORMULA, never by name). Emitting the slot "
                             "without one would store whatever the address "
                             "happened to be at compile time{}",
                             spelling, operand.symbol, unitBytes,
                             sink_.pairSuffix()));
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
        sink_.fail(at,
             std::format("value {} does not fit the {} byte(s) '.{}' emits — it "
                         "is outside [{}, {}] read as signed and unsigned, so "
                         "encoding it would silently truncate{}",
                         v, unitBytes, spelling, signedMin, unsignedMax,
                         sink_.pairSuffix()));
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
            sink_.fail(directive,
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
                sink_.fail(decoded->node,
                     std::format("'.{}' takes a byte count and an optional fill "
                                 "byte — at most two operands{}",
                                 spelling, sink_.pairSuffix()));
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
                    sink_.fail(decoded->node,
                         std::format("'.{}' needs a numeric fill byte; '{}' is "
                                     "a symbol, and its address is not known "
                                     "when these bytes are laid out{}",
                                     spelling,
                                     decoded->symbol.empty()
                                         ? std::string{"a non-numeric operand"}
                                         : decoded->symbol,
                                     sink_.pairSuffix()));
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
                sink_.fail(decoded->node,
                     std::format("'.{}' needs a non-negative byte count{}",
                                 spelling, sink_.pairSuffix()));
                return;
            }
            count = decoded->value;
        }
        if (!count.has_value()) {
            sink_.fail(directive,
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
                sink_.warn(fillNode.valid() ? fillNode : directive,
                     std::format("'.{}' names fill byte {}, but the open "
                                 "section is zero-fill ({}) — the wire format "
                                 "reserves its size WITHOUT storing file bytes, "
                                 "so there is nowhere for a non-zero pattern to "
                                 "live and the fill is ignored (the reference "
                                 "assembler does the same, and warns){}",
                                 spelling, *fill,
                                 dataSectionKindName(item.section),
                                 sink_.pairSuffix()));
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
                    sink_.fail(L.at,
                         std::format("label '{}' is marked as a function entry "
                                     "but is defined inside a DATA section — "
                                     "one label cannot be both code and data{}",
                                     L.name, sink_.pairSuffix()));
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
        if (unclassified.empty()) return sink_.ok();

        std::string names;
        for (auto const i : unclassified) {
            if (!names.empty()) names += ", ";
            names += '\'';
            names += labels_[i].name;
            names += '\'';
        }
        auto const expected =
            cfg_.spellingsForVerb(AsmDirectiveVerb::FunctionEntry);
        sink_.fail(labels_[unclassified.front()].at,
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
                 sink_.pairSuffix()));
        return false;
    }

    // ── pass 2: emit ──────────────────────────────────────────────────────
    bool emitAll() {
        walkElements(tree_.root(), [&](NodeId element) { emitElement(element); });
        if (!sink_.ok()) return false;
        closeFunction();
        return sink_.ok();
    }

    void emitElement(NodeId element) {
        {
            if (!sink_.ok()) return;
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
                    if (!sink_.ok()) return;
                    if (NodeId const nested = elementInLabelTail(labelTail);
                        nested.valid()) {
                        emitElement(nested);
                    }
                    return;
                }
                trackSection(element);
                // D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED: the call-frame
                // family is applied HERE and only here, because the anchor a
                // rule needs — the instruction it follows — exists only in this
                // pass. See the family docblock beside `applyFrameDirective`.
                applyFrameDirective(element);
                return;
            }
            // A statement: a label definition, an instruction, or a label
            // followed on the same line by one.
            NodeId cur = element;
            while (cur.valid() && sink_.ok()) {
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
                    if (!sink_.ok()) return;
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
                engine_.lowerStatement(cur, name,
                                       findDescendantOfRule(
                                           tree_, rawTail,
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

    // ══════════════════════════════════════════════════════════════════════
    // CALL FRAME INFORMATION — the `.s` PRODUCER
    // (D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED).
    // ══════════════════════════════════════════════════════════════════════
    //
    // ★★★ WHAT THIS PRODUCES AND WHY IT IS NOT A NEW CARRIER. The output is
    // `LirFuncCfi` — the SAME per-function op list the calling-convention
    // materializer emits for a C function (`lir/lir_callconv.hpp`), keyed the
    // same way (by the `LirInstId` after which the rule is in effect) and
    // resolved to byte offsets by the SAME join in `asm.cpp`. Two producers,
    // one representation, one resolver: a `.s` frame and a C frame become
    // `.eh_frame` FDEs through identical code, so neither can drift into
    // describing frames the other cannot.
    //
    // ★★ THE ANCHOR IS THE PRECEDING INSTRUCTION, WHICH IS EXACTLY WHAT gas
    // MEANS. `.cfi_def_cfa_offset 48` written under `subq $40, %rsp` says "from
    // the end of that instruction onward, the CFA is 48 above rsp" — DWARF's
    // `DW_CFA_advance_loc` target and `LirCfiOp::inst`'s documented "the
    // instruction AFTER which the rule is in effect" are the same convention,
    // because they are describing the same physical fact.
    // ⚠ A RULE BEFORE THE FUNCTION'S FIRST INSTRUCTION anchors to NOTHING and
    // takes effect at offset 0. That is `LirCfiOp` with an INVALID `inst` and
    // `atBlockEnd == false` — a state the callconv producer never emits (its
    // ops always follow an instruction it just wrote) and which the resolver
    // therefore has to be told about explicitly rather than inferring.
    //
    // ★ WHAT IS FOLDED HERE RATHER THAN CARRIED. `.cfi_adjust_cfa_offset` and
    // `.cfi_rel_offset` both state a number relative to the RUNNING CFA offset,
    // and DWARF encodes only absolute ones — `dwarf_cfi.hpp` refuses an
    // unresolved `AdjustCfaOffset` outright, naming the producer as the tier
    // that must fold. gas resolves them against its own running state for the
    // same reason. So this walker keeps that state (including the
    // remember/restore stack, which changes it) and emits absolute rules.

    // The frame description that is currently open, i.e. between a
    // `frameStart` directive and its `frameEnd`.
    struct OpenFrame {
        bool         active = false;
        NodeId       at{};             // the `.cfi_startproc`, for diagnostics
        std::size_t  funcIndex = 0;    // which `perFuncCfi_` slot it fills
        // The running CFA offset, in bytes, as gas would track it. Seeded from
        // the entry state (`callPushBytes`) and updated by every CFA rule.
        std::int64_t cfaOffset = 0;
        // `.cfi_remember_state` pushes, `.cfi_restore_state` pops. Only the CFA
        // offset is stacked because it is the only piece of state this walker
        // has to answer a question about; the REGISTER rules are carried
        // through to the encoder untouched, where `foldCfiOps` owns the full
        // state machine.
        std::vector<std::int64_t> cfaStack;
    };

    // ★★★ THE ENTRY STATE, DERIVED FROM THE TARGET AND CROSS-CHECKED ACROSS
    // EVERY CALLING CONVENTION IT DECLARES.
    //
    // A `.s` names no calling convention — the `encode` tier runs no callconv
    // pass by construction — so the C path's "read `callingConvention(index)`"
    // has no index to read. But the three facts an entry state needs are psABI
    // properties of the MACHINE rather than of a convention: whether the call
    // instruction pushes the return address (`callPushBytes`), which register
    // is the stack pointer, and whether there is a link register. ✔MEASURED
    // 2026-08-17 on both shipped targets: x86_64's `sysv_amd64` and `ms_x64`
    // agree (rsp / 8 / no link register) and arm64's `aapcs64` and
    // `apple_arm64` agree (sp / 0 / x30).
    //
    // ⚠ SO IT IS DERIVED **AND VERIFIED**, NOT ASSUMED. Reading convention 0
    // and hoping would be a silent guess the day a target declares a convention
    // with a different call-push; requiring every declared convention to AGREE
    // turns that day into a diagnostic. This is the same discipline the
    // `fdePointerRelocationOf` lookup uses — derive from what the target already
    // states, and REFUSE on ambiguity rather than picking.
    [[nodiscard]] std::optional<CfiInitialState> deriveCfiInitialState(NodeId at) {
        if (cfiInitial_.has_value()) return cfiInitial_;
        auto const ccs = target_.callingConventions();
        if (ccs.empty()) {
            sink_.fail(at,
                 std::format("target '{}' declares no calling convention, so "
                             "the frame state every function of this file "
                             "starts in cannot be stated — call-frame "
                             "information needs the stack-pointer register and "
                             "whether the call instruction pushes a return "
                             "address{}", target_.name(), sink_.pairSuffix()));
            return std::nullopt;
        }
        std::optional<CfiInitialState> agreed;
        std::string_view               agreedName;
        for (auto const& cc : ccs) {
            if (!cc.stackPointer.has_value()) {
                sink_.fail(at,
                     std::format("target '{}' calling convention '{}' declares "
                                 "no stack-pointer register, so no "
                                 "canonical-frame-address rule can be stated "
                                 "for a function in this file{}",
                                 target_.name(), cc.name, sink_.pairSuffix()));
                return std::nullopt;
            }
            CfiInitialState st;
            st.cfaRegister = cc.stackPointer->ordinal;
            st.cfaOffset   = static_cast<std::int64_t>(cc.callPushBytes);
            if (cc.callPushBytes > 0) {
                st.returnAddressAtCfaOffset =
                    -static_cast<std::int64_t>(cc.callPushBytes);
            } else if (cc.linkRegister.has_value()) {
                st.returnAddressRegister = cc.linkRegister->ordinal;
            }
            if (!agreed.has_value()) {
                agreed     = st;
                agreedName = cc.name;
                continue;
            }
            if (!(*agreed == st)) {
                sink_.fail(at,
                     std::format("target '{}' calling conventions '{}' and '{}' "
                                 "describe DIFFERENT frame entry states, and an "
                                 "assembly file names no convention — so this "
                                 "build cannot say which one a '.s' function "
                                 "starts in. One shared CIE cannot describe "
                                 "both, and picking either would misdescribe "
                                 "every function written for the other{}",
                                 target_.name(), agreedName, cc.name,
                                 sink_.pairSuffix()));
                return std::nullopt;
            }
        }
        cfiInitial_ = agreed;
        return cfiInitial_;
    }

    // The operand nodes of a directive, in source order. `kids[2]` is the
    // optional operand sequence; a directive with no operands has none.
    [[nodiscard]] std::vector<NodeId>
    directiveOperands(std::vector<NodeId> const& kids) const {
        std::vector<NodeId> out;
        if (kids.size() < 3) return out;
        for (NodeId const o : visibleChildren(tree_, kids[2])) {
            if (tree_.kind(o) == NodeKind::Internal) out.push_back(o);
        }
        return out;
    }

    // ★★★ THE REGISTER A CFI RULE NAMES — AND IT ARRIVES IN TWO FORMS, BOTH OF
    // WHICH THE REFERENCE ASSEMBLER TAKES.
    //   * a DWARF REGISTER NUMBER (`.cfi_offset 6, -16`) — what gcc emits;
    //   * this dialect's register SPELLING (`.cfi_offset %rbp, -16` /
    //     `.cfi_offset x29, -16`) — what a hand-written file often uses.
    //
    // ★★ THE NUMBER IS RESOLVED THROUGH THE TARGET'S OWN `dwarfNumber` TABLE,
    // NEVER THROUGH THE HARDWARE ENCODING. They are different permutations —
    // `%rbp` is DWARF 6 and hardware 5 — and an encoder handed the wrong one
    // writes a table a debugger follows into the wrong frame, without
    // complaining. The same table `dwarf_cfi.hpp` reads on the way out is the
    // one read here on the way in, so a round trip is the identity by
    // construction.
    //
    // ★ THE RETURN-ADDRESS COLUMN IS CHECKED FIRST because on x86_64 it is 16 —
    // a synthetic column no register carries — and on AArch64 it is 30, which
    // IS x30's ordinary number. Preferring the RA column on a tie is safe
    // precisely because they encode identically (`dwarfOf` maps both to 30) and
    // it is what makes the CIE's own rule and the file's rule name one thing.
    [[nodiscard]] std::optional<CfiRegRef>
    frameRegisterOperand(NodeId operandNode, std::string_view spelling) {
        auto decoded = decodeOperand(operandNode);
        if (!decoded) return std::nullopt;
        if (decoded->role == AsmOperandRole::Register) {
            if (!decoded->reg.isPhysical) {
                sink_.fail(operandNode,
                     std::format("'.{}' names a register this walker did not "
                                 "resolve to a physical one{}",
                                 spelling, sink_.pairSuffix()));
                return std::nullopt;
            }
            return CfiRegRef::physical(
                static_cast<std::uint16_t>(decoded->reg.id));
        }
        if (!decoded->hasValue || decoded->isMemory) {
            sink_.fail(operandNode,
                 std::format("'.{}' needs a register — either a DWARF register "
                             "NUMBER (which is what a compiler emits) or one of "
                             "this dialect's register spellings — and this "
                             "operand is neither{}",
                             spelling, sink_.pairSuffix()));
            return std::nullopt;
        }
        if (decoded->value < 0) {
            sink_.fail(operandNode,
                 std::format("'.{}' names DWARF register {}, and a DWARF "
                             "register number is never negative{}",
                             spelling, decoded->value, sink_.pairSuffix()));
            return std::nullopt;
        }
        auto const wanted = static_cast<std::uint64_t>(decoded->value);
        if (auto const ra = target_.dwarfReturnAddressColumn();
            ra.has_value() && wanted == *ra) {
            return CfiRegRef::returnAddress();
        }
        auto const regs = target_.registers();
        std::optional<std::uint16_t> found;
        for (std::size_t i = 0; i < regs.size(); ++i) {
            if (!regs[i].dwarfNumber.has_value()) continue;
            if (*regs[i].dwarfNumber != wanted) continue;
            if (found.has_value()) {
                // Two rows claiming one DWARF number means the target's table
                // no longer identifies a register. Picking either writes a rule
                // about the wrong one — silently.
                sink_.fail(operandNode,
                     std::format("target '{}' declares DWARF register number {} "
                                 "on BOTH '{}' and '{}', so '.{}' cannot name "
                                 "one of them{}",
                                 target_.name(), wanted, regs[*found].name,
                                 regs[i].name, spelling, sink_.pairSuffix()));
                return std::nullopt;
            }
            found = static_cast<std::uint16_t>(i);
        }
        if (!found.has_value()) {
            sink_.fail(operandNode,
                 std::format("target '{}' declares no register with DWARF "
                             "number {}, so '.{}' names a frame slot this build "
                             "cannot resolve. The numbering is a per-target "
                             "psABI table (`registers[].dwarfNumber` in that "
                             "target's document) and the hardware encoding is a "
                             "DIFFERENT permutation, so guessing one from the "
                             "other would describe another register entirely{}",
                             target_.name(), wanted, spelling,
                             sink_.pairSuffix()));
            return std::nullopt;
        }
        return CfiRegRef::physical(*found);
    }

    // A plain signed integer operand (an offset, a delta, a column).
    [[nodiscard]] std::optional<std::int64_t>
    frameNumberOperand(NodeId operandNode, std::string_view spelling,
                       std::string_view what) {
        auto decoded = decodeOperand(operandNode);
        if (!decoded) return std::nullopt;
        if (!decoded->hasValue || decoded->isMemory
            || decoded->role == AsmOperandRole::Register) {
            sink_.fail(operandNode,
                 std::format("'.{}' needs {} as a plain number, and this "
                             "operand is not one{}",
                             spelling, what, sink_.pairSuffix()));
            return std::nullopt;
        }
        return decoded->value;
    }

    // How many operands each rule takes, derived from the SHARED predicates in
    // `core/types/cfi.hpp` rather than from a table repeated here. A rule that
    // names a register and carries an offset takes two; one that only names a
    // register takes one; the CFA-offset rules take one number; the state-stack
    // ops take none. ★ `DefCfa` is the one that takes both a register and an
    // offset, which is exactly `cfiOpTouchesCfa && !offset-only` — spelled out
    // below rather than encoded as a magic count, so a reader can check it.
    struct FrameRuleArity { bool reg; bool srcReg; bool offset; };
    [[nodiscard]] static constexpr FrameRuleArity arityOf(CfiOpKind k) noexcept {
        switch (k) {
        case CfiOpKind::DefCfa:            return {true,  false, true};
        case CfiOpKind::DefCfaRegister:    return {true,  false, false};
        case CfiOpKind::DefCfaOffset:      return {false, false, true};
        case CfiOpKind::AdjustCfaOffset:   return {false, false, true};
        case CfiOpKind::RegAtCfaOffset:    return {true,  false, true};
        case CfiOpKind::RegValIsCfaOffset: return {true,  false, true};
        case CfiOpKind::RegInRegister:     return {true,  true,  false};
        case CfiOpKind::RegSameValue:      return {true,  false, false};
        case CfiOpKind::RegUndefined:      return {true,  false, false};
        case CfiOpKind::RegRestoreInitial: return {true,  false, false};
        case CfiOpKind::RememberState:     return {false, false, false};
        case CfiOpKind::RestoreState:      return {false, false, false};
        }
        return {false, false, false};
    }

    // Apply one call-frame directive during the emit walk.
    void applyFrameDirective(NodeId directive) {
        auto const kids = visibleChildren(tree_, directive);
        if (kids.size() < 2) return;      // pass 1 refused a nameless directive
        std::string const spelling{tree_.text(kids[1])};
        auto const* row = cfg_.directiveBySpelling(spelling);
        if (row == nullptr || row->operandOnly) return;
        switch (row->verb) {
        case AsmDirectiveVerb::FrameStart:  openFrame(directive, spelling); return;
        case AsmDirectiveVerb::FrameEnd:    closeFrame(directive, spelling); return;
        case AsmDirectiveVerb::FrameRule:
            applyFrameRule(directive, kids, *row, spelling);
            return;
        case AsmDirectiveVerb::FrameReturnColumn:
            applyFrameReturnColumn(directive, kids, spelling);
            return;
        default: return;
        }
    }

    void openFrame(NodeId at, std::string_view spelling) {
        if (frame_.active) {
            sink_.fail(at,
                 std::format("'.{}' opens a frame description while one is "
                             "already open — frame descriptions do not nest, "
                             "and the rules after this point would belong to "
                             "two functions at once{}",
                             spelling, sink_.pairSuffix()));
            return;
        }
        if (openFunctionLabel_ == kNoLabel) {
            sink_.fail(at,
                 std::format("'.{}' opens a frame description with no function "
                             "open — call-frame information describes the frame "
                             "of one function, and there is nothing here for it "
                             "to describe. Mark the entry with this dialect's "
                             "function-entry directive ({}) first{}",
                             spelling,
                             cfg_.spellingsForVerb(
                                 AsmDirectiveVerb::FunctionEntry),
                             sink_.pairSuffix()));
            return;
        }
        // ⚠ `openFunction` pushes a slot and sets `openFunctionLabel_` in the
        // same breath, so the guard above already implies a non-empty vector.
        // It is checked anyway rather than trusted, because the alternative on
        // a substrate break is `size() - 1` underflowing to a huge index.
        if (perFuncCfi_.empty()) {
            sink_.fail(at,
                 std::format("internal: '.{}' opens a frame description with a "
                             "function open but no per-function slot for it — "
                             "the label walk and the frame walk disagree about "
                             "this file's function list{}",
                             spelling, sink_.pairSuffix()));
            return;
        }
        auto const initial = deriveCfiInitialState(at);
        if (!initial.has_value()) return;
        frame_.active    = true;
        frame_.at        = at;
        frame_.funcIndex = perFuncCfi_.size() - 1;
        frame_.cfaOffset = initial->cfaOffset;
        frame_.cfaStack.clear();
        // ★ ENGAGE THE SLOT HERE, not on the first rule. A function bracketed
        // by frame-start/frame-end with NO rules between is DESCRIBED and must
        // get its own (rule-free) unwind entry — ✔MEASURED 2026-08-17, that is
        // exactly what gcc 13.3.0 emits for a leaf function and what gas turns
        // into an FDE. Engaging on the first rule instead would make a leaf
        // silently unwindable-through-nothing.
        perFuncCfi_[frame_.funcIndex].emplace();
    }

    void closeFrame(NodeId at, std::string_view spelling) {
        if (!frame_.active) {
            sink_.fail(at,
                 std::format("'.{}' closes a frame description that was never "
                             "opened{}", spelling, sink_.pairSuffix()));
            return;
        }
        if (!frame_.cfaStack.empty()) {
            sink_.fail(at,
                 std::format("'.{}' closes a frame description with {} "
                             "remembered state(s) never restored — the rules "
                             "after the unmatched remember describe a frame the "
                             "source never returned to{}",
                             spelling, frame_.cfaStack.size(),
                             sink_.pairSuffix()));
            return;
        }
        frame_.active = false;
    }

    void applyFrameRule(NodeId directive, std::vector<NodeId> const& kids,
                        AsmDirectiveSpelling const& row,
                        std::string_view spelling) {
        if (!requireOpenFrame(directive, spelling)) return;
        // A `frameRule` row with no rule cannot load (the loader requires it);
        // the guard keeps a substrate break from becoming a wrong rule.
        if (!row.frameRule.has_value()) {
            sink_.fail(directive,
                 std::format("internal: directive '.{}' declares verb "
                             "'frameRule' with no rule, which the load-time "
                             "validation that guarantees this did not hold{}",
                             spelling, sink_.pairSuffix()));
            return;
        }
        CfiOpKind const kind    = *row.frameRule;
        auto const      arity   = arityOf(kind);
        auto const      operands = directiveOperands(kids);
        std::size_t const wanted = static_cast<std::size_t>(arity.reg)
                                 + static_cast<std::size_t>(arity.srcReg)
                                 + static_cast<std::size_t>(arity.offset);
        if (operands.size() != wanted) {
            sink_.fail(directive,
                 std::format("'.{}' states the frame rule '{}', which takes {} "
                             "operand(s); {} were written. A rule applied with "
                             "the wrong operands describes a frame the source "
                             "did not{}",
                             spelling, cfiOpKindName(kind), wanted,
                             operands.size(), sink_.pairSuffix()));
            return;
        }
        LirCfiOp op;
        op.kind = kind;
        std::size_t next = 0;
        if (arity.reg) {
            auto const r = frameRegisterOperand(operands[next++], spelling);
            if (!r.has_value()) return;
            op.reg = *r;
        }
        if (arity.srcReg) {
            auto const r = frameRegisterOperand(operands[next++], spelling);
            if (!r.has_value()) return;
            op.srcReg = *r;
        }
        if (arity.offset) {
            auto const v = frameNumberOperand(operands[next++], spelling,
                                              "a byte offset");
            if (!v.has_value()) return;
            op.offset = *v;
        }
        // ── the folds ────────────────────────────────────────────────────
        // ★ `.cfi_adjust_cfa_offset N` becomes an ABSOLUTE `def_cfa_offset`,
        //   because DWARF has no adjust opcode and `dwarf_cfi.hpp` refuses one
        //   unresolved, naming the producer as the tier that must fold. gas
        //   does the identical resolution against its own running state.
        if (kind == CfiOpKind::AdjustCfaOffset) {
            op.kind   = CfiOpKind::DefCfaOffset;
            op.offset = frame_.cfaOffset + op.offset;
        }
        // ★ `.cfi_rel_offset reg, off` is `.cfi_offset reg, off − CFA offset`.
        //   Declared per ROW (`offsetFromCfa`) rather than detected here, so a
        //   dialect spelling it differently binds the same fold.
        if (row.frameOffsetFromCfa) {
            op.offset -= frame_.cfaOffset;
        }
        // ── running state, so the next fold sees this rule ───────────────
        switch (op.kind) {
        case CfiOpKind::DefCfa:
        case CfiOpKind::DefCfaOffset: frame_.cfaOffset = op.offset; break;
        case CfiOpKind::RememberState:
            frame_.cfaStack.push_back(frame_.cfaOffset);
            break;
        case CfiOpKind::RestoreState:
            if (frame_.cfaStack.empty()) {
                sink_.fail(directive,
                     std::format("'.{}' restores a frame state that was never "
                                 "remembered — the producer's own model of the "
                                 "frame is inconsistent, and encoding it would "
                                 "describe a frame that never existed{}",
                                 spelling, sink_.pairSuffix()));
                return;
            }
            frame_.cfaOffset = frame_.cfaStack.back();
            frame_.cfaStack.pop_back();
            break;
        default: break;
        }
        recordFrameOp(op);
    }

    void applyFrameReturnColumn(NodeId directive,
                                std::vector<NodeId> const& kids,
                                std::string_view spelling) {
        if (!requireOpenFrame(directive, spelling)) return;
        auto const operands = directiveOperands(kids);
        if (operands.size() != 1) {
            sink_.fail(directive,
                 std::format("'.{}' takes exactly one operand — the DWARF "
                             "column the return address lives in; {} were "
                             "written{}",
                             spelling, operands.size(), sink_.pairSuffix()));
            return;
        }
        auto const stated = frameNumberOperand(operands[0], spelling,
                                               "a DWARF column number");
        if (!stated.has_value()) return;
        auto const declared = target_.dwarfReturnAddressColumn();
        if (!declared.has_value()) {
            sink_.fail(operands[0],
                 std::format("'.{}' states a return-address column, but target "
                             "'{}' declares none — so this build has nothing to "
                             "check the claim against and no column to write "
                             "into the CIE{}",
                             spelling, target_.name(), sink_.pairSuffix()));
            return;
        }
        // ★★ THE CHECK *IS* THE HONOURING. One shared CIE carries one
        // return-address column, so a file naming a DIFFERENT one is asking for
        // something this image cannot express; accepting it silently would let
        // a `.s` redefine where the return address lives and have no effect
        // whatever. Naming the same column is the no-op gcc emits.
        if (static_cast<std::uint64_t>(*stated) != *declared) {
            sink_.fail(operands[0],
                 std::format("'.{}' states DWARF return-address column {}, but "
                             "target '{}' declares column {}. The column is a "
                             "property of the whole image's shared CIE, so a "
                             "per-function override cannot be expressed — and "
                             "accepting it would leave the emitted table saying "
                             "{} while this file says {}{}",
                             spelling, *stated, target_.name(), *declared,
                             *declared, *stated, sink_.pairSuffix()));
        }
    }

    [[nodiscard]] bool requireOpenFrame(NodeId at, std::string_view spelling) {
        if (frame_.active) return true;
        sink_.fail(at,
             std::format("'.{}' states a call-frame rule outside any frame "
                         "description — a rule with no frame to belong to has "
                         "no function and no extent. Open one with this "
                         "dialect's {} directive{}",
                         spelling,
                         cfg_.spellingsForVerb(AsmDirectiveVerb::FrameStart),
                         sink_.pairSuffix()));
        return false;
    }

    // Attach one op to the open frame, anchored to the instruction it follows.
    // ⚠ AN INVALID `inst` IS THE FUNCTION-ENTRY ANCHOR (offset 0) and is a
    // deliberate state, not a miss — see the family docblock above.
    void recordFrameOp(LirCfiOp op) {
        if (funcInstCount_ > 0) op.inst = builder_.lastInst();
        if (frame_.funcIndex < perFuncCfi_.size()
            && perFuncCfi_[frame_.funcIndex].has_value()) {
            perFuncCfi_[frame_.funcIndex]->ops.push_back(op);
        }
    }

    // ── functions and blocks ──────────────────────────────────────────────
    void enterLabel(std::string const& name, NodeId at) {
        auto const it = labelIndex_.find(name);
        if (it == labelIndex_.end()) {
            sink_.fail(at, std::format("label '{}' was not collected", name));
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
        if (!sink_.ok()) return;
        auto& entry = labels_[labelIdx];
        builder_.addFunction(entry.symbol);
        // D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED: the parallel slot, opened
        // with the function so its index and the LIR function index are the
        // same number by construction rather than by agreement.
        perFuncCfi_.emplace_back();
        funcInstCount_     = 0;
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
            sink_.fail(at, std::format("label '{}' has no open function",
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
            sink_.fail(at,
                 std::format("control falls out of '{}' into label '{}', and "
                             "this build cannot write that edge down: {}. LIR "
                             "requires every block to end in a terminator, so "
                             "the fallthrough must become an explicit branch — "
                             "either declare the target's unconditional-branch "
                             "opcode unambiguously, or end '{}' with an "
                             "explicit jump{}",
                             from, labels_[targetLabel].name, branchProbeNote_,
                             from, sink_.pairSuffix()));
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
        // ★ AN UNCLOSED FRAME DESCRIPTION IS A REFUSAL, NOT A TRUNCATION
        // (D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED). A description with no
        // `frameEnd` states no extent: closing it here at the function boundary
        // would silently invent one, and the invented FDE would cover exactly
        // the bytes the source never claimed.
        if (frame_.active) {
            sink_.fail(frame_.at,
                 std::format("assembly function '{}' opens a frame description "
                             "that is never closed — {} states which bytes the "
                             "unwind table covers, and ending it at the "
                             "function boundary instead would emit an FDE over "
                             "an extent the source never stated{}",
                             entry.name,
                             cfg_.spellingsForVerb(AsmDirectiveVerb::FrameEnd),
                             sink_.pairSuffix()));
            frame_.active = false;
        }
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
            sink_.fail(entry.at,
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
                             sink_.pairSuffix()));
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
            sink_.fail(labels_[i].at,
                 std::format("label '{}' reserved a basic block that the "
                             "lowering never reached — the emit walk and the "
                             "label scan disagree about this file's structure{}",
                             labels_[i].name, sink_.pairSuffix()));
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
                sink_.fail(entry.at,
                     std::format("label '{}' is one of this build's "
                                 "program-entry names but is not exported — an "
                                 "entry symbol with local linkage is invisible "
                                 "to the linker's entry resolution. Add this "
                                 "dialect's global-symbol directive ({}) for "
                                 "it{}",
                                 entry.name,
                                 cfg_.spellingsForVerb(
                                     AsmDirectiveVerb::GlobalSymbol),
                                 sink_.pairSuffix()));
            } else if (!userEntry_.has_value()) {
                userEntry_ = entry.symbol;
            }
        }
        symbols_.push_back(std::move(sym));
        openFunctionLabel_ = kNoLabel;
        openBlockLabel_    = kNoLabel;
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
                sink_.fail(p.at,
                     std::format("'.{}' names '{}', which this file defines no "
                                 "label for. A data slot holding an address "
                                 "must name something this translation unit "
                                 "defines: an undefined name would have to be "
                                 "imported, and an import states whether it is "
                                 "CODE or DATA (which selects the linker's "
                                 "indirection slot), while a data directive "
                                 "says neither{}",
                                 p.spelling, p.name, sink_.pairSuffix()));
                return false;
            }
            p.labelIndex = it->second;
            auto const& L = labels_[p.labelIndex];
            // An entry or data label already carries its symbol; only an
            // interior BLOCK label is minted here, and minting it is precisely
            // the act that makes its block address-taken.
            if (L.isEntry || L.isData) continue;
            if (L.functionLabel == kNoLabel) {
                sink_.fail(p.at,
                     std::format("'.{}' names '{}', which is a label inside no "
                                 "function — its address is not part of any "
                                 "function's bytes, so there is nothing to "
                                 "relocate against{}",
                                 p.spelling, p.name, sink_.pairSuffix()));
                return false;
            }
            // ★ CALLED FOR THE MINT, NOT FOR THE VALUE — hence the explicit
            // discard. PASS 1c's job here is only to ENSURE the label has a
            // symbol; the id itself is stored on the label and re-read by
            // PASS 3b when it writes the relocation, so binding it to a local
            // here would be a second copy of a fact one line of state already
            // holds. ⚠ THE DISCARD IS SAFE BECAUSE THIS FUNCTION HAS NO
            // FAILURE PATH — it mints-if-absent and returns; it is NOT the
            // `(void)encodeInst` shape that laundered partial output in
            // D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD. If a failure arm is ever
            // added to it, this line must consume the result, not cast it away.
            // (Was an unannotated discard of a `[[nodiscard]]` value, i.e. a
            // live -Wunused-result on every build; found 2026-08-14.)
            // ★ AND `[[nodiscard]]` STAYS ON THE FUNCTION: the other caller,
            // `sourceOperandForSymbol`, does consume the id, so the attribute is
            // still earning its keep and this discard is the exception it marks.
            (void)symbolForAddressedLabel(p.labelIndex);
        }
        return sink_.ok();
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
                sink_.fail(p.at,
                     std::format("internal: '.{}' takes the address of '{}', "
                                 "which reserved no basic block — the emit walk "
                                 "never opened the function that contains it, "
                                 "so this slot's relocation would name a symbol "
                                 "with no address{}",
                                 p.spelling, p.name, sink_.pairSuffix()));
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
    LirBuilder                   builder_;
    // ⚠ DECLARATION ORDER IS INITIALISATION ORDER AND BOTH MATTER HERE.
    // `engine_` binds references to `builder_` and `sink_`, so both must be
    // constructed first; `engine_` also takes `*this` as its host, which is
    // legal because it only STORES the reference (nothing is called on a
    // half-built object).
    AsmDiagnosticSink            sink_;
    AsmInstructionLowering       engine_;

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
    // D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED. One slot per LIR function, in
    // `openFunction` order — which IS `builder_.addFunction` order and
    // therefore `AssembledModule::functions` order, the parallel-index
    // discipline every other per-function vector here follows.
    // ⚠ A SLOT EXISTS FOR EVERY FUNCTION, DESCRIBED OR NOT, precisely so the
    // index stays the function index; a vector holding only described
    // functions would make "slot k" mean something different from "function k".
    // The slot is ENGAGED by the frame-start directive.
    std::vector<std::optional<LirFuncCfi>>      perFuncCfi_;
    OpenFrame                                   frame_{};
    std::optional<CfiInitialState>              cfiInitial_;
    // Instructions emitted into the CURRENT function. The anchor question a
    // `.cfi_*` rule asks is "is there an instruction above me *in this
    // function*?" — `blockInstCount_` resets at every label and would answer it
    // wrongly for the first rule after a block boundary, and `lastInst()`
    // ABORTS when nothing has been appended at all.
    std::uint32_t                               funcInstCount_     = 0;
    bool                                        openTerminated_    = false;
    bool                                        branchProbed_      = false;
    std::optional<std::uint16_t>                branchOpcode_;
    std::string                                 branchProbeNote_;
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
