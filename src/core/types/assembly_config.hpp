#pragma once

#include "core/export.hpp"
#include "core/types/rule_id.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The text→LIR contract of an ASSEMBLY DIALECT document (schema v4 `assembly`
// block; plan 29 P3/P4).
//
// ★★★ WHY A DIALECT IS A LANGUAGE, WHICH IS WHY THIS IS A `.lang.json` BLOCK
// AND NOT A `.target.json` ONE. An `asmSyntax` facet putting assembly grammar
// into `.target.json` was written, built, gated green, reviewed and REVERTED on
// 2026-08-12. ✔MEASURED with gcc on ONE target: AT&T `movq %rsi, (%rdi)` versus
// Intel (`-masm=intel`) `mov QWORD PTR [rdi], rsi` — one CPU, one compiler, two
// dialects differing in register sigil, immediate sigil, comment character,
// operand ORDER, memory-operand form and mnemonic spelling. So every one of
// those is a function of (target, DIALECT); storing a per-(X,Y) fact per-X is
// the duplication shape that killed the verbatim `_fstat$INODE64` binding. And
// the lexical half is not merely duplicative but impossible: `#` is a COMMENT on
// x86-AT&T and the IMMEDIATE marker on arm64, so two dialects cannot share one
// token table at all.
//
// ★★ WHAT THIS BLOCK IS NOT ALLOWED TO CONTAIN: registers, encodings, or any
// opcode SEMANTICS. `instructions[]` maps a SPELLING to a target opcode NAME;
// the name is resolved through `TargetSchema::opcodeByMnemonic` and an
// unresolvable one fails loud naming the target. A register spelling resolves
// through `TargetSchema::registerByName`. That keeps a dialect document from
// becoming a second, drifting copy of the target description — the failure the
// reverted facet would have institutionalized.

namespace dss {

// Which asm operand is the DESTINATION. ★ ONE DIALECT-WIDE FACT, DECLARED ONCE.
// The reverted facet declared it PER INSTRUCTION, justified by the claim "that
// flag is already false for x86 store forms". ✔MEASURED: false — AT&T is
// uniformly destination-LAST *including stores* (`movq %rsi, (%rdi)`), Intel
// uniformly destination-FIRST. Both dialects are internally uniform, so a
// per-instruction field meant N edits to express the ONE thing that
// distinguishes them, and the justification for it had never been probed.
enum class AsmOperandOrder : std::uint8_t {
    DestinationLast,    // AT&T / gas:  op  src, dst
    DestinationFirst,   // Intel / masm: op dst, src
};

inline constexpr std::array<std::pair<std::string_view, AsmOperandOrder>, 2>
    kAsmOperandOrderNames{{
        {"destinationLast", AsmOperandOrder::DestinationLast},
        {"destinationFirst", AsmOperandOrder::DestinationFirst},
    }};
static_assert(kAsmOperandOrderNames.size()
                  == static_cast<std::size_t>(AsmOperandOrder::DestinationFirst)
                         + 1,
              "every AsmOperandOrder enumerator needs a config spelling");

// What an assembler DIRECTIVE means. ★ A CLOSED VERB SET, NOT A STRING MATCH:
// gas's `.globl` and `.global` are two spellings of one verb, and a dialect
// whose spelling is `.export` binds the same verb. The engine switches on the
// verb and never on the spelling — the one place a directive's meaning lives.
// ⚠ `IgnoredAnnotation` is an EXPLICIT decision, not a fallback: it says "this
// directive carries information DSS derives elsewhere". An UNDECLARED directive
// is a different thing and fails loud, because a silently-dropped `.globl`
// produces a binary whose entry symbol is local and a link failure that points
// nowhere near the cause.
enum class AsmDirectiveVerb : std::uint8_t {
    SectionText,         // `.text` — subsequent content is executable code
    GlobalSymbol,        // `.globl NAME` — NAME gets external linkage
    IgnoredAnnotation,   // parsed, validated, and deliberately carries nothing
    // ★★★ `.type NAME, @function` — NAME's label OPENS A LIR FUNCTION; every
    // other label in the file is a BLOCK inside the function that is open when
    // it appears. Without this verb the lowering cannot tell `main:` from
    // `.L3:`, and the only alternatives are both wrong: one-function-per-label
    // (a `jmp .L3` then crosses a function boundary) or a guess from
    // branch/call targets (which reads `.L3` as a function the moment anything
    // calls through a register, and reads a never-branched-to entry as a
    // block).
    // ⚠ WHAT THE ENGINE MUST NOT LEARN FROM THIS IS THE OBJECT FORMAT.
    // `@function` is ELF's spelling; COFF writes `.def NAME; .scl 2; .type 32;
    // .endef` and Mach-O writes nothing at all. So the SPELLING and the MARKER
    // TEXT both live in the dialect row (`spelling` + the optional `marker`);
    // the engine only knows "this directive names a symbol, and — if a marker
    // is declared — the marker must be among its operands".
    FunctionEntry,
};

inline constexpr std::array<std::pair<std::string_view, AsmDirectiveVerb>, 4>
    kAsmDirectiveVerbNames{{
        {"sectionText", AsmDirectiveVerb::SectionText},
        {"globalSymbol", AsmDirectiveVerb::GlobalSymbol},
        {"ignoredAnnotation", AsmDirectiveVerb::IgnoredAnnotation},
        {"functionEntry", AsmDirectiveVerb::FunctionEntry},
    }};
static_assert(kAsmDirectiveVerbNames.size()
                  == static_cast<std::size_t>(
                         AsmDirectiveVerb::FunctionEntry)
                         + 1,
              "every AsmDirectiveVerb enumerator needs a config spelling");

// The operand ROLES the lowering understands, each bound by the dialect to one
// of its own rules. ★ THE ENGINE WALKS ROLES, NEVER RULE NAMES — a dialect whose
// memory form is `[base, #off]` binds `Memory` to a different rule and needs no
// engine change. That indirection is the whole reason a second dialect is a
// config file rather than a code change.
enum class AsmOperandRole : std::uint8_t {
    Register,    // `%rax`            → a physical register
    Immediate,   // `$42`             → a literal value
    Memory,      // `(%rax,%rbx,8)`   → a base/index/scale reference, no displacement
    Displaced,   // `-8(%rbp)` / `foo`→ a scalar, optionally followed by a base
    Indirect,    // `*%rax`           → an indirect branch/call target
    Scalar,      // `42` / `foo`      → the value inside an immediate or displacement
    NegNumber,   // `-8`              → a negated integer
};

inline constexpr std::array<std::pair<std::string_view, AsmOperandRole>, 7>
    kAsmOperandRoleNames{{
        {"register", AsmOperandRole::Register},
        {"immediate", AsmOperandRole::Immediate},
        {"memory", AsmOperandRole::Memory},
        {"displaced", AsmOperandRole::Displaced},
        {"indirect", AsmOperandRole::Indirect},
        {"scalar", AsmOperandRole::Scalar},
        {"negNumber", AsmOperandRole::NegNumber},
    }};
inline constexpr std::size_t kAsmOperandRoleCount =
    kAsmOperandRoleNames.size();
static_assert(kAsmOperandRoleCount
                  == static_cast<std::size_t>(AsmOperandRole::NegNumber) + 1,
              "every AsmOperandRole enumerator needs a config spelling — a role "
              "with no name is unbindable, and the loader's REQUIRE-ALL check "
              "would silently stop covering it");

// One `assembly.instructions[]` row: an assembly SPELLING and the target
// opcodes it may name. `width` is the operand width the spelling's suffix
// encodes (`movq` → 64, `movl` → 32); the target's encoding variants are
// guarded on it, so a wrong width is a wrong instruction rather than a
// wrong-looking one.
//
// ★★★ WHY `opcodes` IS A LIST AND NOT A NAME. One dialect mnemonic denotes a
// SET of target opcodes, chosen by the OPERAND SHAPE: AT&T `movq` is the
// target's `mov` in `movq %rax,%rcx`, its `load` in `movq (%rdi),%rax` and its
// `store` in `movq %rax,(%rdi)`. gas spells all three `movq` because x86 spells
// all three `MOV`; DSS's LIR splits them because the three have different
// operand shapes and different encodings.
// ⚠ THE DIALECT DOES NOT SAY WHICH — it says WHICH ARE POSSIBLE, and the
// TARGET's own `encoding.variants[].guard` picks the member (see
// `asm/asm_variant_elect.hpp`). A dialect that named the winner per shape would
// be a second, drifting copy of the target's guard table, and the drift would
// show up as a green build emitting the wrong instruction.
struct AsmInstructionSpelling {
    std::string              spelling;      // as written in the `.s`
    // Candidate target opcodes, in the dialect's declaration order. Each is
    // resolved against `TargetSchema::opcodeByMnemonic` at lowering time (the
    // opcode table lives in `.target.json`, which `core` cannot see here).
    std::vector<std::string> opcodeNames;
    // ★★★ OPTIONAL, BECAUSE WHERE THE WIDTH IS WRITTEN IS A DIALECT FACT.
    // ✔MEASURED 2026-08-13 against a real arm64 dialect: AT&T puts the width in
    // the MNEMONIC SUFFIX (`movq` / `movl` — same registers, different width),
    // while aarch64 gas puts it in the REGISTER (`add x0,x1,x2` is 64-bit,
    // `add w0,w1,w2` is 32-bit — SAME mnemonic). A mandatory per-spelling width
    // forces the arm64 dialect to declare ONE width for a spelling that has
    // two, and `add w0,w1,w2` then encodes a 64-bit ADD with no diagnostic.
    // ⚠ ABSENT MEANS "DERIVE IT FROM THE OPERANDS", NOT "DEFAULT TO 64". The
    // lowering reads the width off the DATA register operands (never off a
    // memory base/index — `movl (%rdi),%eax` is legal precisely because the
    // address width and the operation width are different questions). PRESENT
    // means the dialect asserts it, and the assertion is CHECKED against the
    // operands: a disagreement is refused, in both directions, exactly as gas
    // rejects `movl %rax,%ecx` and `movl %eax,%rcx`.
    std::optional<std::uint32_t> width;
    // OPTIONAL: the `TargetCondCode` spelling this mnemonic carries (`je` →
    // `"eq"`, arm64 `b.eq` → `"eq"`). EMPTY means the row declares none.
    //
    // ★ IT IS NOT A CONTROL-FLOW KNOB. Whether the instruction is a branch, a
    // conditional branch, a return or a call is the TARGET's `terminatorKind` /
    // `isCall`, never re-declared here — a dialect that restated it could
    // disagree with the target and there would be no way to tell which was
    // right. `cond` supplies the one datum the target's ENCODING leaves open:
    // WHICH condition a variant that declares `condCodeFromPayload` should
    // read out of the instruction payload.
    //
    // ★★★ REQUIRED ⟺ THE OPCODE'S ENCODING READS ONE, IN BOTH DIRECTIONS. The
    // cross-check runs in `asm_text_to_lir.cpp`'s `resolveRows` — once per
    // dialect row, before any statement is walked, so a broken row is reported
    // for the CONFIG rather than for the first `.s` line that happens to use
    // it. A row whose opcode reads a payload condition and declares none, and a
    // row that declares one on an opcode that reads none, are both refused; so
    // is a row whose candidate opcodes DISAGREE about it.
    //
    // ⚠ THAT KEY WAS `terminatorKind == cond-br` UNTIL 2026-08-13, AND IT WAS
    // WRONG IN BOTH DIRECTIONS — D-ASM-COND-ALLOWED-ONLY-ON-JCC. Both shipped
    // targets declare exactly two opcodes that read a condition: `jcc` (a
    // cond-br) and `setcc` (`terminatorKind: None`, `result: value`). Keyed on
    // the terminator shape the rule coincided with the right answer on `jcc`
    // and refused a condition on `setcc` — the opcode whose ONLY job is to
    // materialize one. Terminator-ness and condition-consumption are two
    // independent facts about an opcode and only one of them is about the
    // condition; a `cbz`-shaped conditional branch is the other direction of
    // the same point (a cond-br that correctly carries no condition code).
    std::string              condName;
};

struct AsmDirectiveSpelling {
    std::string      spelling;   // as written after the introducer, WITHOUT it
    AsmDirectiveVerb verb{};
    // `FunctionEntry` ONLY (refused on every other verb). The operand text that
    // must be present for the directive to mark a function — gas/ELF's
    // `@function`. EMPTY means "naming the symbol is enough", which is the
    // shape a format with no type annotation needs.
    // ⚠ THE MARKER IS WHAT KEEPS `.type foo, @object` FROM MINTING A FUNCTION.
    // Both spellings are `.type`; only the operand distinguishes them, and it
    // is object-format vocabulary, so it belongs in the dialect document and
    // not in the engine.
    std::string      marker;
};

struct DSS_EXPORT AssemblyConfig {
    // False for every language that declares no `assembly` block — which is
    // every language that is not an assembly dialect. ⚠ THE FLAG IS NOT
    // REDUNDANT WITH `instructions.empty()`: a dialect may legitimately declare
    // an empty spelling table while it is being grown, and "declared nothing"
    // must stay distinguishable from "is not an assembly dialect at all", or a
    // C file routed here by a driver bug would get "no instructions" instead of
    // "this language has no assembly surface".
    bool declared = false;

    // Rule landmarks — resolved at load, so a row naming a rule that does not
    // exist is a load error rather than a lowering that silently matches
    // nothing.
    RuleId unitRule{};
    RuleId lineRule{};
    RuleId elementRule{};
    RuleId directiveRule{};
    RuleId statementRule{};
    RuleId labelTailRule{};
    RuleId operandSeqRule{};

    // Indexed by `static_cast<std::size_t>(AsmOperandRole)`. Every role is
    // REQUIRED when the block is present — a partial `operandForms` is a load
    // error, never a silently-unrecognized operand shape.
    std::array<RuleId, kAsmOperandRoleCount> operandFormRules{};

    AsmOperandOrder operandOrder = AsmOperandOrder::DestinationLast;

    std::vector<AsmInstructionSpelling> instructions;
    std::vector<AsmDirectiveSpelling>   directives;

    // Label names that START A PROGRAM. ★ IT IS THE SAME FACT `c-subset.lang.json`
    // states as `semantics.declarations[].entryFunctions`, stated at a tier that
    // has no declarations: the `encode` path runs no semantic analysis, so there
    // is no declaration row for an entry SHAPE to hang off, and a `.s` label
    // carries no signature to match one against anyway. ⚠ IT IS NOT A SECOND
    // POLICY — it supplies only the NAMES; whether a named entry is realizable
    // stays the format's `entryVerbs()` question, answered by the same
    // intersection every other language goes through.
    // Empty is legal and means "this dialect starts no program by itself" — a
    // pure object/relocatable build, where the entry is somebody else's.
    std::vector<std::string> entryLabels;

    // The rule realizing `role`, or an INVALID RuleId when the dialect declared
    // the role ABSENT (JSON `null`). ⚠ "Absent" is a DECLARED answer, not an
    // omission: aarch64 gas has no `displaced` form (there is no
    // `disp(base,index,scale)` syntax) and no `indirect` form (indirectness
    // lives in the mnemonic — `b` versus `br`), so forcing it to bind those
    // roles to some unrelated rule would make the lowering recognize operand
    // shapes the dialect cannot write. Every role must still be MENTIONED; a
    // missing key is still a load error.
    [[nodiscard]] RuleId ruleForRole(AsmOperandRole role) const noexcept {
        return operandFormRules[static_cast<std::size_t>(role)];
    }

    // Which role (if any) a CST rule realizes. Linear over a fixed 7-element
    // array — the operand shapes of one dialect, not a growable table.
    //
    // ★★★ A RULE MAY REALIZE MORE THAN ONE ROLE, AND THAT IS THE DIALECT
    // DECLARING THE SHAPE AMBIGUOUS — NOT A CONFIG ERROR.
    // ✔MEASURED 2026-08-13: aarch64 gas has NO register sigil, so in `mov x0,
    // x1` / `bl helper` / `b Lend` the operands `x0`, `helper` and `Lend` are
    // the SAME TOKEN. No grammar can split them; that is what gas IS. So the
    // arm64 dialect binds `register` AND `scalar` to one rule and the LOOKUP
    // decides — `TargetSchema::registerByName` first, symbol on a miss, which
    // is exactly what gas does.
    // ⚠ THE OLD `roleForRule` RETURNED THE FIRST MATCH AND THAT WAS A REAL
    // DEFECT — the higher-numbered role silently never applied. The fix is to
    // return the WHOLE SET and make the caller decide by lookup; the fix is NOT
    // to reject the config, which would make the arm64 dialect inexpressible.
    // The one binding that stays a LOAD error is the genuinely undecidable one:
    // two NON-register roles on one rule, where no lookup can separate them.
    //
    // Returns a bitmask over `AsmOperandRole` (bit i = role i).
    [[nodiscard]] std::uint8_t rolesForRule(RuleId rule) const noexcept {
        if (!rule.valid()) return 0;
        std::uint8_t mask = 0;
        for (std::size_t i = 0; i < kAsmOperandRoleCount; ++i) {
            if (operandFormRules[i].valid()
                && operandFormRules[i].v == rule.v) {
                mask |= static_cast<std::uint8_t>(1u << i);
            }
        }
        return mask;
    }

    [[nodiscard]] static constexpr bool
    maskHas(std::uint8_t mask, AsmOperandRole role) noexcept {
        return (mask & (1u << static_cast<std::size_t>(role))) != 0;
    }

    // Every directive spelling bound to `verb`, comma-joined and written with
    // the dialect's own introducer omitted (the row stores it that way). Built
    // for the refusal that has to tell a reader WHICH directive it wanted; a
    // hand-written list in the diagnostic would go stale the first time a
    // dialect renamed a spelling.
    [[nodiscard]] std::string spellingsForVerb(AsmDirectiveVerb verb) const {
        std::string out;
        for (auto const& row : directives) {
            if (row.verb != verb) continue;
            if (!out.empty()) out += ", ";
            out += '\'';
            out += row.spelling;
            out += '\'';
        }
        return out;
    }

    [[nodiscard]] AsmInstructionSpelling const*
    instructionBySpelling(std::string_view s) const noexcept {
        for (auto const& row : instructions) {
            if (row.spelling == s) return &row;
        }
        return nullptr;
    }

    [[nodiscard]] AsmDirectiveSpelling const*
    directiveBySpelling(std::string_view s) const noexcept {
        for (auto const& row : directives) {
            if (row.spelling == s) return &row;
        }
        return nullptr;
    }
};

[[nodiscard]] inline std::optional<AsmOperandOrder>
asmOperandOrderFromName(std::string_view name) {
    for (auto const& [text, v] : kAsmOperandOrderNames) {
        if (text == name) return v;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<AsmDirectiveVerb>
asmDirectiveVerbFromName(std::string_view name) {
    for (auto const& [text, v] : kAsmDirectiveVerbNames) {
        if (text == name) return v;
    }
    return std::nullopt;
}

// The legal spellings of a closed set, comma-joined — for the load diagnostic
// that rejects an unknown one. Built from the same table it validates against,
// so it can never list a stale set.
template <class Table>
[[nodiscard]] inline std::string asmNameList(Table const& table) {
    std::string out;
    for (auto const& [text, v] : table) {
        (void)v;
        if (!out.empty()) out += ", ";
        out += '\'';
        out += text;
        out += '\'';
    }
    return out;
}

} // namespace dss
