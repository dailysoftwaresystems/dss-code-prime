#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS / -TEXT, GNU 6.47.2): the
// per-CU descriptor pool that carries an `__asm__` statement's payload from the
// front end to codegen, plus the parser for the GNU constraint STRING.
//
// ★★★ WHY THE CONSTRAINT SPLITS IN TWO, AND WHY ONLY HALF OF IT LIVES HERE.
// `target_schema.hpp`'s `asmConstraints` facet states the rule this file is the
// other side of: the MODIFIERS (`=` output, `+` in-out, `&` earlyclobber, `%`
// commutative) are GNU-asm GRAMMAR — they mean the same thing on every
// processor, so the front end owns them and `TargetAsmConstraint` refuses to
// store them — while the LETTER is a MACHINE FACT that only `.target.json` can
// answer (`"=a"` is `%rax` on x86_64 and "impossible constraint" on AArch64).
// ⇒ `parseAsmConstraint` splits the string and stops. It NEVER decides what a
// letter means, and there is deliberately no letter table in this file: a
// letter→register table in C++ is the agnosticism break the bar vetoes, and it
// is one no grep catches until a second architecture's inline asm arrives.
//
// ★★ WHY A SIDE POOL RATHER THAN NODE FIELDS. `detail::HirNode` is budgeted at
// `sizeof <= 32` with exactly ONE `std::uint32_t payload`, so a descriptor of
// this size cannot live on the node. The shipped precedent is `HirLiteralPool`:
// a per-CU vector beside the module, indexed by the node's `payload`.
// ✔MEASURED 2026-08-15 (the claim was checked, not inherited): HIR has NO
// rebuild/rewrite/clone passes at all — exactly TWO sites in `src/` construct a
// `HirBuilder` (`cst_to_hir.cpp` and `hir_text.cpp`'s parser), `src/opt/`
// operates on MIR, and `--transpile` is fail-loud at dispatch. So the
// silent-drop hazard that forced LIR's per-instruction pool to be copied
// through four rebuild passes does not exist here. The residual exposure is at
// the HAND-OFF seams, where the pool is a separate by-pointer argument a new
// call site can forget — which is why `HirTextContext`/`HirParseResult` both
// carry it and `hir_verifier` asserts every index resolves.
//
// ⛔ NO `HirKind` IS MINTED FOR THE PAYLOAD-CARRYING FORM. Two kinds would
// encode ONE fact ("this is an inline-asm statement") and every consumer would
// carry both arms forever — the one-fact test plan 29 §4.4 applies to
// `ReturnPiece`, one tier up. `HirKind::InlineAsm` grows children and a payload
// instead; `payload == kNoInlineAsmDescriptor` is the bare-barrier form.

namespace dss {

// The `payload` of an `InlineAsm` node that carries NO descriptor — the
// empty-template optimizer barrier `__asm__ [volatile] ("")`, which has nothing
// to bind, clobber-track or branch to.
//
// ★ IT IS A SENTINEL AT INDEX 0 RATHER THAN "index 0 is the first descriptor",
// and that is the LIR `kLirNoRegConstraints` discipline, not a preference: a
// default-constructed `payload` is 0, so a 0-based pool would make a node that
// was never given a descriptor read back as a PLAUSIBLE pointer to somebody
// else's operands. Handles are therefore 1-BASED (`add` returns `size()`), and
// the only value that cannot be mistaken for a measurement is the one that
// resolves to nothing.
inline constexpr std::uint32_t kNoInlineAsmDescriptor = 0;

// ── the GNU constraint string, split ────────────────────────────────────────

// One parsed constraint. `letter` is what `TargetSchema::asmConstraint()`
// takes; everything else is the grammar half this tier owns.
struct DSS_EXPORT HirAsmConstraint {
    // The constraint exactly as written, modifiers included — quoted verbatim
    // by every diagnostic, because "unsupported constraint" leaves the author
    // unable to tell a typo from a deferral.
    std::string raw;
    // The machine letter with the modifiers stripped, e.g. `a` from `"=&a"`.
    // A STRING, not a `char`, for the reason `TargetAsmConstraint::letter` is
    // one: gcc machine constraints are not all one character (aarch64 `Ush`,
    // x86 `Yz`) and the growth direction is longer spellings.
    std::string letter;
    // `=` or `+` — this operand is WRITTEN by the statement.
    bool isOutput = false;
    // `+` — read AND written; one input tied to one output.
    bool isReadWrite = false;
    // `&` — earlyclobber: the output is written before the inputs are consumed,
    // so it must not share a register with any of them.
    bool earlyClobber = false;
    // `%` — this operand and the next one commute.
    bool commutative = false;
};

// Why a constraint string was refused. Every member except `None` maps to
// `S_InlineAsmConstraintUnsupportedForm` (0xE066); the enum exists so the
// message can NAME which shape fired rather than saying "unsupported".
//
// ⚠ `MultiLetter` is NOT decided here. Whether `Ush` is one letter or three is
// a question only the target can answer, so `parseAsmConstraint` reports the
// remainder as one opaque `letter` and the TARGET-AWARE caller discriminates
// (see `asmConstraintLooksMultiLetter`).
enum class HirAsmConstraintDefect : std::uint8_t {
    None = 0,
    // `""` — or a string that is nothing but modifiers (`"="`). There is no
    // letter to resolve and no operand to bind.
    Empty,
    // `"=r,m"` (GNU 6.47.2.4) — comma-separated ALTERNATIVES ask the allocator
    // to CHOOSE a binding rather than to honour one. Refused as GRAMMAR, with
    // no target consulted, because the comma is grammar on every processor.
    MultiAlternative,
    // A leading character outside the four modifiers the front end owns.
    // GNU has more of them (`#`, `*`, `?`, `!` — register preference and cost
    // markers); accepting one silently would drop the preference it encodes.
    UnknownModifier,
    // `=` or `+` written twice, or written after `&`/`%`. The output modifier
    // is positional in GNU asm and a second one is a contradiction, not a
    // duplicate: `"=+r"` says both "write only" and "read-write".
    MisplacedOutputModifier,
};

[[nodiscard]] DSS_EXPORT std::string_view
asmConstraintDefectDescription(HirAsmConstraintDefect d) noexcept;

// The result of splitting one constraint string. On a defect `value.letter` is
// EMPTY and `value.raw` still carries the text, so the diagnostic can quote it.
struct DSS_EXPORT HirAsmConstraintParse {
    HirAsmConstraint       value;
    HirAsmConstraintDefect defect = HirAsmConstraintDefect::None;

    [[nodiscard]] bool ok() const noexcept {
        return defect == HirAsmConstraintDefect::None;
    }
};

// Split `raw` into modifiers + letter. Shape (GNU 6.47.2.4, and the front end
// owns exactly this much of it):
//
//     [ '=' | '+' ]?  [ '&' | '%' ]*  <letter-spelling>
//
// ★ THE MODIFIER SET IS CLOSED AND POSITIONAL, and both halves are load-
// bearing. Closed: a character outside it that appears BEFORE the spelling is
// `UnknownModifier`, never silently absorbed into the letter — an absorbed `#`
// would turn a register-preference marker into a letter this target does not
// declare and report the wrong defect. Positional: once a non-modifier
// character has been seen the rest is the SPELLING, so a `&` inside it is
// `MisplacedOutputModifier` rather than an earlyclobber flag arriving late.
[[nodiscard]] DSS_EXPORT HirAsmConstraintParse
parseAsmConstraint(std::string_view raw);

class TargetSchema;

// Is `spelling` PROVABLY two-or-more letters written together (`"rm"`), as
// opposed to one long letter this target simply does not declare?
//
// ★★ THE DISCRIMINATION IS A MEASUREMENT, NOT A LENGTH RULE, and it has to be
// because both shapes are >1 character. gcc machine constraints include real
// multi-character spellings (aarch64 `Ush`, x86 `Yz`), so "longer than one
// character ⇒ multi-letter" would report `"Ush"` — a legal constraint on a
// target that declares it — as a multi-letter defect. ⇒ the test is: the whole
// spelling is NOT declared, and EVERY individual character IS. Under that test
// `"rm"` on x86_64 is multi-letter, `"Ush"` on x86_64 is an undeclared letter,
// and `"Ush"` on a target that declares it never reaches here.
//
// ⚠ REQUIRES A TARGET, and that is the point: the question is unanswerable
// without one. A caller with no target in scope must NOT guess — it reports
// neither defect and leaves the resolution to the tier that binds the operand,
// which cannot function without resolving the letter and therefore fails loud
// on its own.
[[nodiscard]] DSS_EXPORT bool
asmConstraintLooksMultiLetter(TargetSchema const& target,
                              std::string_view    spelling);

// ── the descriptor ───────────────────────────────────────────────────────────

// One operand of an inline-asm statement. The VALUE EXPRESSION is not here: it
// is a CHILD of the `InlineAsm` HIR node, at this operand's index. Keeping the
// expression in the child list rather than as a node id in the pool is what
// makes the operand a first-class part of the tree — walked, typed, verified
// and (for an output) treated exactly like an assignment target.
struct DSS_EXPORT HirInlineAsmOperand {
    // The `[name]` symbolic form (GNU 6.47.2.3), empty when the operand was
    // written positionally. Carried because a template may reference an operand
    // by name (`%[name]`) as well as by index.
    std::string      symbolicName;

    // ★★★ EVERY SPELLING THIS OPERAND ANSWERS TO IN A TEMPLATE, CARRIED
    // VERBATIM FROM THE FRONT END — the positional form (`%0`) always, and the
    // symbolic form (`%[name]`) when the source named it. Order is
    // [positional, symbolic].
    //
    // ★★ CARRIED, NEVER RE-DERIVED, AND THAT IS AN ARCHITECTURAL DECISION AND
    // NOT A CACHE. The sigil BYTES are LANGUAGE vocabulary with exactly one
    // declared owner (`semantics.inlineAsmTemplateLexemes`) and the NUMBERING is
    // the SOURCE's operand list — in which a `"+"` read-write operand appears
    // ONCE, while every tier below realizes it as an output PLUS a synthesized
    // tied input. A consumer that rebuilt `%N` from its own list would therefore
    // overcount by the number of `"+"` operands and shift every `asm goto` label
    // reference, with nothing to notice it. Minting once at the front end makes
    // the disagreement unwritable: only one tier counts.
    // ⇒ every tier below this one COMPARES these strings and never composes one.
    std::vector<std::string> spellings;

    HirAsmConstraint constraint;
    // Redundant with `constraint.isOutput` BY CONSTRUCTION and deliberately so:
    // this records which SECTION the operand was written in, which is what the
    // `%N` index space is built from. A `"=r"` written in the INPUT section is
    // a real (if strange) shape, and collapsing the two facts would make the
    // index space depend on the constraint text.
    bool             isOutput = false;

    // ── the TARGET half, resolved by the front end when a target is in scope ──
    //
    // ★★ THESE ARE THE FIELDS `MirAsmDescriptor`'s `MirAsmOperand` consumes
    // (`mir_asm_descriptor.hpp`: *"resolved by the front end against the
    // target's register table — this tier never enumerates register names"*).
    // Resolving them HERE, once, is what keeps every downstream tier from
    // re-reading `.target.json` and drifting.
    //
    // ⚠ BOTH ARE UNSET WHEN NO TARGET WAS IN SCOPE, which is a real state, not
    // a defect: the LSP, the FFI header parser and every direct-API unit test
    // analyze without one. An unset binding is NOT a licence to guess a class —
    // the consumer that needs it cannot function without it and must fail loud
    // naming the operand, exactly as `TargetSchema::asmConstraint`'s own
    // docblock requires of a missed letter.
    //
    // Set ⇔ the letter resolved and it selects a register CLASS (`"r"`, `"x"`).
    // Stored as the raw enum value rather than `TargetRegClass` so this header
    // — reached by every HIR consumer — does not pull in the 3.4k-line target
    // schema; the same trade `mir_asm_descriptor.hpp` makes one tier down.
    bool          regClassResolved = false;
    std::uint8_t  regClass         = 0;

    // Non-empty ⇔ the letter PINS one physical register (`"=a"` → `rax`). The
    // NAME as the target declares it, so no consumer re-resolves an ordinal.
    std::string   fixedRegister;
};

// Everything an `__asm__` statement carries, decoded once at the front end.
struct DSS_EXPORT HirInlineAsmDescriptor {
    // The template's DECODED bytes (escapes resolved, adjacent literals
    // concatenated) — never the source spelling.
    std::string templateText;

    // ★ OUTPUTS FIRST, THEN INPUTS, EACH IN SOURCE ORDER — and that ordering IS
    // the `%N` index space (GNU 6.47.2.3), not a convenience. `%0` is the first
    // OUTPUT when the statement has one and the first INPUT when it does not,
    // so a bound written against either section alone would accept `%2` on a
    // one-output-one-input statement while rejecting a legal `%1`.
    std::vector<HirInlineAsmOperand> operands;
    // How many leading entries of `operands` came from the outputs section.
    std::uint32_t outputCount = 0;

    // Clobbered REGISTER names only, verbatim as the source wrote them. The two
    // GNU spellings that name no register are hoisted into the flags below at
    // capture time — the split `MirAsmDescriptor` also makes, and for its
    // reason: it leaves this list uniformly resolvable against the target's
    // register table with no special cases at the consumer.
    //
    // ⚠ THE TWO SPELLINGS COME FROM CONFIG, NEVER FROM A C++ LITERAL.
    // `semantics.inlineAsm.memoryClobber` / `.conditionCodeClobber` live in the
    // ASM language document because they are a property of the assembly surface
    // being embedded, not of the host language embedding it. Hard-coded here
    // they would be an `if (spelling == "memory")` in shared substrate.
    std::vector<std::string> clobbers;
    bool clobbersMemory        = false;
    bool clobbersConditionCodes = false;

    // `asm goto` targets, as the per-function label ordinals the HIR
    // `GotoStmt`/`LabelStmt` namespace already uses — resolved at lowering, so
    // no consumer re-resolves an identifier.
    std::vector<std::uint32_t> labelOrdinals;

    // ★★★ WHAT THE TEMPLATE CALLS EACH OF THOSE LABELS. Index-aligned 1:1 with
    // `labelOrdinals` (`HirVerifier::checkInlineAsm` asserts the sizes), one
    // inner vector per label holding EVERY spelling it answers to: the
    // bracketed form (`%l[done]`) and the positional form (`%l2`) on the
    // reference numbering, in that order.
    //
    // ★★ CARRIED, NEVER RE-DERIVED — and here the argument is stronger than it
    // is for an operand, because an ordinal genuinely cannot rebuild a spelling:
    // the label's SOURCE NAME is gone by this tier (it is a number now), and the
    // positional index is `operandCount + position`, a fact of the STATEMENT
    // rather than of the label. The spelling is a lexical fact and is treated as
    // one — the same argument this file's `HirInlineAsmOperand::spellings`
    // makes, arrived at from the side where re-derivation is not merely risky
    // but impossible.
    //
    // ⚠ An inner vector is EMPTY exactly when the language declares
    // `templateLabelPlaceholder` as `null` — the honoured-absence case; such a
    // language cannot write a label reference at all.
    std::vector<std::vector<std::string>> labelSpellings;

    // The `goto` qualifier was written. ⚠ NOT the same question as "are there
    // labels": ✔MEASURED 2026-08-14, clang 18.1.3/19.1.1 ACCEPT `asm goto` with
    // an EMPTY label section (gcc 13.3 rejects it), and the operator ruled to
    // follow clang — so a goto-qualified statement with zero labels is legal and
    // must stay distinguishable from an unqualified one.
    bool isGoto = false;

    // The statement carried at least one `:` — the EXTENDED form.
    //
    // ★★★ IT IS A LEXICAL FACT ABOUT THE TEMPLATE, NOT A COSMETIC ONE.
    // ✔MEASURED 2026-08-14 on gcc 13.3.0 AND clang 18.1.3 (sources fed as
    // base64 so no shell quoting could confound it): a bare `%eax` in an
    // EXTENDED template is an ERROR on BOTH ("operand number missing after
    // %-letter" / "invalid % escape"), while the SAME text in a BASIC template
    // is emitted verbatim — in basic asm `%` is LITERAL. **Any colon makes it
    // extended**, including `:::` with every section empty. So the two
    // templates lex DIFFERENTLY and this flag is what selects which reading
    // applies (`S_InlineAsmPlaceholderInBasicTemplate`, 0xE06B).
    bool isExtended = false;

    // Does this statement promise the allocator anything?
    //
    // ★ THE PREDICATE IS "THE CLOBBER LIST IS NON-EMPTY", NEVER "IS THIS THE
    // EXTENDED FORM", and the difference is measured. §2a of the 2026-08-14
    // reference probe: `__asm__("xorl %eax,%eax")` (basic) and the same with an
    // EMPTY clobber list (`:::`) BOTH destroy `%eax` under gcc and clang — the
    // corruption is the programmer's, by design — while a real `::: "eax"`
    // makes the value survive. Emitting a basic template verbatim with zero
    // assumed clobbers IS the reference semantics; refusing it, or
    // conservatively spilling around it, are both divergences.
    // ⚠ The two non-register spellings COUNT: `::: "memory"` protects memory
    // and carries no register name, so a predicate over `clobbers` alone would
    // read that statement as promising nothing.
    [[nodiscard]] bool protectsRegisters() const noexcept {
        return !clobbers.empty() || clobbersMemory || clobbersConditionCodes;
    }

    // The operands an `%N` placeholder may name, i.e. the bound
    // `S_InlineAsmPlaceholderOutOfRange` (0xE06A) checks against.
    [[nodiscard]] std::size_t placeholderCount() const noexcept {
        return operands.size();
    }
};

// The per-CU pool. Mirrors `HirLiteralPool` deliberately — same append-only
// shape, same "no dedup" rule (dedup is an optimizer concern and would break
// the one-descriptor-per-statement identity a diagnostic span relies on) —
// EXCEPT that handles are 1-based so `payload == 0` means "no descriptor".
class DSS_EXPORT HirInlineAsmPool {
public:
    // Append a descriptor; returns its HANDLE (never `kNoInlineAsmDescriptor`).
    [[nodiscard]] std::uint32_t add(HirInlineAsmDescriptor d);

    // Resolve a handle. Aborts loud on `kNoInlineAsmDescriptor` or an
    // out-of-range handle: a caller that reached here without checking
    // `payload != kNoInlineAsmDescriptor` has a structural bug, and returning a
    // default-constructed descriptor would hand it an asm statement with no
    // operands and no clobbers — the silent miscompile this whole arc exists to
    // prevent, manufactured by the accessor.
    [[nodiscard]] HirInlineAsmDescriptor const& at(std::uint32_t handle) const;

    // Is `handle` resolvable? The verifier's question — it must be able to ASK
    // without aborting, because reporting a dangling handle is its job.
    [[nodiscard]] bool contains(std::uint32_t handle) const noexcept {
        return handle != kNoInlineAsmDescriptor && handle <= pool_.size();
    }

    [[nodiscard]] std::size_t size()  const noexcept { return pool_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return pool_.empty(); }

private:
    std::vector<HirInlineAsmDescriptor> pool_;
};

} // namespace dss
