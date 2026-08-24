#pragma once

#include "core/export.hpp"
#include "core/types/ascii_case.hpp"   // asciiToLower — the ONE folding helper
// `CfiOpKind` — the closed unwind-rule vocabulary a `frameRule` directive row
// NAMES. Included rather than re-spelled: the alternative (a private rule enum
// here, or the rule stored as a bare string) is the "one fact, two owners"
// shape the bar vetoes, and `cfi.hpp` is dependency-free by construction (it
// includes `core/export.hpp` and the standard library only), so there is no
// cycle to route around the way `sectionName` has to.
#include "core/types/cfi.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/strong_ids.hpp"   // LexerModeId — the template surface's mode

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

// ★★★ DOES A WRITTEN SPELLING HAVE TO MATCH THE DECLARED ONE BYTE FOR BYTE?
// (D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE.)
//
// ★★ IT IS A DIALECT FACT, NOT A TARGET FACT, FOR THE SAME REASON THE OPERAND
// ORDER IS. One CPU can be written in two dialects with different case rules —
// masm folds, and a hypothetical dialect need not — so storing it per-CPU would
// be the per-(X,Y)-fact-stored-per-X shape the header comment above already
// rejects. ⇒ THE DIALECT FOLDS THE SPELLING AND THEN ASKS THE TARGET;
// `TargetSchema::registerByName` stays an EXACT match on target vocabulary, so
// nothing about the CPU description changes when a dialect's case rule does.
//
// ★★★ WHAT IT GOVERNS, STATED AS A RULE RATHER THAN AS A LIST OF FOUR SEAMS:
// matching a written spelling against **DECLARED VOCABULARY** — this dialect's
// own instruction/directive/selector tables, and the target's register table.
// It NEVER governs a name the PROGRAM introduces. That boundary is not a taste
// call, it is the measurement:
//
//   ✔MEASURED 2026-08-15, `as` 2.42 (x86_64) and `aarch64-linux-gnu-as` 2.42,
//   rc + the encoded words read back with `objdump -d`:
//     * mnemonic  — FOLDS. `movq`/`MOVQ`/`MoVq %rax,%rcx` → `48 89 c1`;
//                   `mov`/`MOV`/`MoV x0,x1` → `aa0103e0`.
//     * register  — FOLDS. `%RAX,%RCX` → `48 89 c1`; `%EAX,%ECX` → `89 c1`;
//                   `MOV W0,W1` → `2a0103e0`.
//     * selector  — FOLDS. `MRS X0, CNTVCT_EL0` → `d53be040` (and
//                   `CntVct_El0` likewise); `CSET X0, EQ` → `9a9f17e0`.
//     * directive — FOLDS. `.TEXT`, `.GLOBL`, `.TYPE`, `.SECTION`, `.QUAD`,
//                   `.DATA`, `.BSS`, `.ZERO`, arm64 `.XWORD` — all rc=0.
//   ⇒ fold all four TOGETHER or none: folding only the mnemonic still fails
//   `MOV X0, X1` at the register and `.GLOBL main` at the directive, which
//   hides the gap behind a half-fix.
//
// ⛔ AND THE THREE SURFACES THAT DO **NOT** FOLD, EACH REFUSED BY THE SAME
// MEASUREMENT. Folding any of them would not be a conformance fix, it would be
// a silent miscompile, because in every case the two spellings denote two
// DIFFERENT THINGS to the reference assembler:
//     * SYMBOL / LABEL names — `foo:` and `FOO:` are two defined symbols in one
//       object (`nm`: both present), and `b FOO` against `foo:` leaves `FOO`
//       `*UND*`. Folding merges two real definitions.
//     * THE `.type` MARKER — `.type main, @FUNCTION` and `@Function` are BOTH
//       rc=1, *"unrecognized symbol type"*, on BOTH assemblers, while `.TYPE`
//       itself is accepted. So the directive SPELLING folds and its marker
//       operand does not, in the same line.
//     * THE `.section` NAME OPERAND — `.section .RODATA` is rc=0 and produces a
//       section literally named `.RODATA`: a source writing both `.rodata` and
//       `.RODATA` yields TWO sections of 8 bytes each (`objdump -h`). Folding
//       it would route `.RODATA` data into `.rodata`, which is not what the
//       reference does with the same input.
//   ⚠ THE FIRST TWO OF THOSE ARE COMPARED IN `asm_text_to_lir.cpp` AND THE
//   THIRD IN `sectionRowByName` BELOW, all three with a plain `==`, and every
//   one of them must STAY that way. There is a note at each site.
enum class AsmSpellingCase : std::uint8_t {
    // An exact byte match — what every dialect got before this key existed.
    Sensitive,
    // ASCII case-insensitive, via `dss::asciiToLower`. ⚠ ASCII ONLY, and that
    // is the reference behaviour rather than a shortcut: gas folds `A`-`Z` and
    // has no opinion about any other code point.
    AsciiFolded,
};

inline constexpr std::array<std::pair<std::string_view, AsmSpellingCase>, 2>
    kAsmSpellingCaseNames{{
        {"sensitive", AsmSpellingCase::Sensitive},
        {"asciiFolded", AsmSpellingCase::AsciiFolded},
    }};
static_assert(kAsmSpellingCaseNames.size()
                  == static_cast<std::size_t>(AsmSpellingCase::AsciiFolded) + 1,
              "every AsmSpellingCase enumerator needs a config spelling");

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

    // ★★★ THE DATA TRIO (D-ASM-NO-DATA-DEFINING-DIRECTIVE, 2026-08-13). Until
    // these existed the verb set could open a text section and name a symbol
    // but could not put a single BYTE in the binary, so no `.s` could define a
    // string, an array or a word — which is why the extern-call corpus example
    // has to call `putchar` rather than print a message it owns.
    //
    // ★★ THEY MINT NO SECTION VOCABULARY, AND THAT WAS THE ONE DESIGN RULE.
    // `core/types/section_kind.hpp` already declares `DataSectionKind`
    // (`rodata`/`data`/`bss`/`tdata`/`tbss`/`relro`) and was deliberately
    // placed under `core/types/` so `src/asm/` and `src/link/` speak ONE
    // section taxonomy; `AssembledData` already carries a `DataSectionKind`.
    // So `SectionData` names one of THOSE and nothing else — the row stores the
    // NAME (see `AsmDirectiveSpelling::sectionName`) for the same layering
    // reason `instructions[].opcodes` stores opcode names.
    //
    // `.data` / `.bss` / `.section .rodata` — subsequent data items land in the
    // `DataSectionKind` this row names. ⚠ IT IS NOT A SECOND SPELLING OF
    // `SectionText`: text is where CODE goes and LIR has no other, so the two
    // verbs answer different questions and a dialect that mapped `.data` to
    // `sectionText` would silently emit its data as instructions.
    SectionData,
    // `.byte 1,2` / `.word` / `.long` / `.quad 42` — each operand is a VALUE
    // occupying `unitBytes` bytes. The element width is the row's
    // (`unitBytes`), never guessed from the value: `.byte 1` and `.quad 1`
    // differ only in the row.
    EmitData,
    // `.zero 16` / `.space 16, 7` / `.skip 16` — the FIRST operand is a byte
    // COUNT, not a value, and the OPTIONAL SECOND is the byte to fill with
    // (absent ⇒ zero). A distinct verb rather than an `EmitData` flag because
    // the first operand MEANS something different: `.byte 16` writes one byte
    // 0x10 and `.zero 16` writes sixteen bytes 0x00, and a shared verb would
    // put that difference in a key the reader has to notice.
    //
    // ⚠ THIS VERB WAS SPELLED `reserveZeroBytes` UNTIL 2026-08-13 AND THE NAME
    // HAD BECOME A LIE — D-ASM-SPACE-DIRECTIVE-FILL-BYTE-UNMODELLED. The old
    // name was TRUE while the fill byte was unmodelled and the second operand
    // was refused; it stops being true the moment `.space 4, 7` writes
    // `07 07 07 07`. ✔MEASURED 2026-08-13, `aarch64-linux-gnu-as` + `objdump
    // -s`: `.space 4, 7` AND `.zero 4, 7` BOTH assemble rc=0 and BOTH produce
    // `07070707` — so this is ONE verb with an optional fill and not two verbs
    // split by spelling, which is what a `.zero`-takes-no-fill reading would
    // have shipped. A config verb whose name states the wrong default is the
    // "comment records the full fact while the code uses half of it" failure
    // with the halves swapped, so the name moved with the behaviour.
    ReserveFillBytes,

    // ★★★ `.section .rodata` — THE SECTION IS THE DIRECTIVE'S OPERAND, NOT THE
    // ROW'S (D-ASM-SECTION-DIRECTIVE-WITH-OPERAND-UNMODELLED, 2026-08-13).
    // `SectionData` fixes its section PER ROW, so a directive that takes the
    // section as an operand had no shape at all and `.rodata` was unreachable
    // in every dialect.
    //
    // ★★ IT MINTS NO SECTION VOCABULARY AND NO SECOND LOOKUP TABLE — it
    // DELEGATES. `.section X` means "do what the section-opening directive
    // spelled X would do", resolved against THIS dialect's own `directives[]`
    // rows (`SectionText` / `SectionData`). So `.section .data` and `.data`
    // reach the identical row by construction and can never drift, and the
    // only thing `.rodata` adds is one more row in the table that was already
    // there.
    // ⚠ WHY THAT ROW CANNOT SIMPLY BE A BARE `.rodata` DIRECTIVE: ✔MEASURED
    // 2026-08-13 against BOTH reference assemblers — `aarch64-linux-gnu-as`
    // and x86_64 `as` accept `.section .rodata` (rc=0, section `.rodata`
    // PROGBITS+A) and REJECT a bare `.rodata` ("unknown pseudo-op"). Accepting
    // what the reference assembler rejects is the other half of bidirectional
    // conformance, so the row declares `operandOnly` and the bare spelling
    // stays refused BY NAME.
    // ⚠ WHAT IT MODELS IS THE SECTION NAME AND NOTHING ELSE. Real `.section`
    // also carries flags and a type (`.section .note.GNU-stack,"",@progbits`,
    // `.section .rodata,"a",@progbits` — both ✔MEASURED accepted by gas).
    // Those change the section's WIRE SEMANTICS (`"aw"` writable, `"ax"`
    // executable, `@nobits` zero-fill), and DSS derives every one of them from
    // the `DataSectionKind` instead — so honouring the name while dropping the
    // flags would place `.section .rodata,"aw"` read-only in a program that
    // writes it. A `.section` carrying more than the name is therefore
    // REFUSED, naming the operand it will not interpret; it is never silently
    // ignored and never half-applied.
    SectionByName,

    // ★★★ THE CALL-FRAME-INFORMATION FAMILY.
    // D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED.
    // Until these existed the 18 `.cfi_*` spellings the AT&T dialect
    // declares were `IgnoredAnnotation` — parsed, accepted, and DROPPED — so a
    // `.s` assembled, ran correctly, and could not be unwound: no debugger
    // backtrace, no profiler stack, and an exception thrown through the frame
    // terminating instead of propagating. That is an accept-and-ignore with a
    // silent RUNTIME consequence, which is the one shape this project's bar
    // refuses outright.
    //
    // ★★★ THEY MINT NO UNWIND VOCABULARY, AND THAT WAS THE DESIGN RULE.
    // `core/types/cfi.hpp` already declares `CfiOpKind` — the closed set of
    // rule changes a producer can STATE — and its own docblock records that
    // every enumerator was written from a `.cfi_*` spelling a real assembler
    // emits. So a `FrameRule` row names one of THOSE (see
    // `AsmDirectiveSpelling::frameRule`) and nothing else, exactly as
    // `SectionData` names a `DataSectionKind` rather than inventing a section
    // taxonomy. The dialect owns the SPELLING; the shared core owns the MEANING.
    //
    // `.cfi_startproc` — everything until the matching `FrameEnd` describes the
    // frame of the function that is open. ⚠ NOT derivable from the function
    // boundary: gas lets a `.s` describe SOME functions and not others, and a
    // producer that assumed every function had CFI would emit a table covering
    // frames nobody described.
    FrameStart,
    // `.cfi_endproc` — closes the description opened by `FrameStart`. An
    // unmatched one either way is a refusal: a stream that never closes has no
    // extent, and one that closes twice has no owner.
    FrameEnd,
    // `.cfi_def_cfa` / `.cfi_offset` / `.cfi_restore` / … — ONE rule change, at
    // the PC reached by the instruction this directive follows. The row's
    // `frameRule` names the `CfiOpKind`.
    FrameRule,
    // `.cfi_return_column N` — the CIE's return-address column. DSS DERIVES it
    // from the target's `dwarfReturnAddressColumn`, so this row exists to
    // CHECK the file's claim against it: a `.s` naming a different column is
    // refused (one shared CIE cannot carry two), and one naming the same column
    // is the no-op gcc emits. ⚠ Accepting it as an annotation would let a file
    // silently redefine where the return address lives.
    FrameReturnColumn,
    // ★★★ DECLARED BY THE REFERENCE ASSEMBLER, AND DELIBERATELY REFUSED — the
    // EXACT COMPLEMENT of `IgnoredAnnotation`, and the reason it is a verb
    // rather than an absent row. `IgnoredAnnotation` says "this carries
    // information DSS derives elsewhere"; this says "this carries information
    // DSS cannot carry AT ALL, so it is refused by name". Leaving the spelling
    // undeclared would produce the generic *unknown directive* refusal — loud,
    // but it tells the reader the dialect does not know the spelling, which is
    // false, and the obvious repair for a future reader is to add it as an
    // `ignoredAnnotation`. That repair is precisely
    // D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED being re-created inside its own
    // fix, and the row's closing note names it as the thing that must not
    // happen. A declared refusal makes the decision visible in the config and
    // carries its `$comment` to whoever reads it next.
    // ⚠ WHAT IT IS *NOT*: a fallback bucket. Every row carrying it must state,
    // in its own `$comment`, the fact this build cannot represent.
    Unrepresentable,
};

inline constexpr std::array<std::pair<std::string_view, AsmDirectiveVerb>, 13>
    kAsmDirectiveVerbNames{{
        {"sectionText", AsmDirectiveVerb::SectionText},
        {"globalSymbol", AsmDirectiveVerb::GlobalSymbol},
        {"ignoredAnnotation", AsmDirectiveVerb::IgnoredAnnotation},
        {"functionEntry", AsmDirectiveVerb::FunctionEntry},
        {"sectionData", AsmDirectiveVerb::SectionData},
        {"emitData", AsmDirectiveVerb::EmitData},
        {"reserveFillBytes", AsmDirectiveVerb::ReserveFillBytes},
        {"sectionByName", AsmDirectiveVerb::SectionByName},
        {"frameStart", AsmDirectiveVerb::FrameStart},
        {"frameEnd", AsmDirectiveVerb::FrameEnd},
        {"frameRule", AsmDirectiveVerb::FrameRule},
        {"frameReturnColumn", AsmDirectiveVerb::FrameReturnColumn},
        {"unrepresentable", AsmDirectiveVerb::Unrepresentable},
    }};
static_assert(kAsmDirectiveVerbNames.size()
                  == static_cast<std::size_t>(
                         AsmDirectiveVerb::Unrepresentable)
                         + 1,
              "every AsmDirectiveVerb enumerator needs a config spelling");

// ★★★ THE `frameRule` NAME → `CfiOpKind`, RESOLVED AGAINST THE ONE OWNER OF
// THOSE NAMES. Deliberately a LOOP over `cfiOpKindName` rather than a second
// `{name, kind}` table beside it: a parallel table is a second home for a fact
// `core/types/cfi.hpp` already states, and the two drift the first time a rule
// is added (the `hasDwarfNumber` bit-mask that `DwarfRegisterMapping` deleted
// for exactly this reason). Define the lookup as the complement of the
// existing names, never as an enumeration of them.
[[nodiscard]] constexpr std::optional<CfiOpKind>
asmFrameRuleFromName(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kCfiOpKindCount; ++i) {
        auto const k = static_cast<CfiOpKind>(i);
        if (cfiOpKindName(k) == name) return k;
    }
    return std::nullopt;
}

// Does this verb belong to the call-frame family — i.e. is it meaningful only
// between a `FrameStart` and its `FrameEnd`? ONE predicate, so the walker's
// "are we inside a frame description?" gate and any future loader check cannot
// disagree about the membership of the set.
[[nodiscard]] constexpr bool
asmVerbIsFrameBody(AsmDirectiveVerb v) noexcept {
    return v == AsmDirectiveVerb::FrameRule
        || v == AsmDirectiveVerb::FrameReturnColumn;
}

// Does this verb OPEN A SECTION — i.e. can a `SectionByName` operand resolve
// to a row carrying it? ★ ONE PREDICATE, SO THE LOADER'S "which rows may be
// `operandOnly`" CHECK AND THE WALKER'S OPERAND RESOLUTION CANNOT DISAGREE.
// Two independent lists is how a row becomes declarable-but-unreachable.
[[nodiscard]] constexpr bool
asmVerbOpensSection(AsmDirectiveVerb v) noexcept {
    return v == AsmDirectiveVerb::SectionText
        || v == AsmDirectiveVerb::SectionData;
}

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

// ★★★ ONE POSITIONAL OPERAND SELECTOR — an operand that SELECTS THE OPCODE
// instead of being one (operator ruling, plan 29 §4.7, 2026-08-14).
//
// gas writes `mrs <Xd>, cntvct_el0` and `cset <Xd>, eq`; the target opcodes are
// `cntvct` (zero-operand, the counter baked into the fixed word) and `setcc`
// (the condition carried as a `TargetCondCode`). A plain spelling row hands the
// lowering a leftover operand against `maxOperands: 0`, so EVERY line using it
// fails loud — which is why both spellings were absent from this dialect and
// both were anchored (D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED,
// D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED). A selector says "this written
// operand is part of the mnemonic": it is consumed BY THE MATCH, never becomes
// an operand, and never reaches the target.
//
// ⛔ THE REJECTED ALTERNATIVE, RECORDED BECAUSE IT IS THE OBVIOUS ONE: model
// system registers as a real operand KIND with a sysreg table. ✔MEASURED both
// shipped targets — x86_64 `rdtsc` is `min/maxOperands 0` with
// `implicitRegisters.outputs [rax,rdx]`, arm64 `cntvct` is `min/maxOperands 0`
// with `result value` and `fixedWord 0xD53BE040`. They differ in RESULT MODEL,
// which is real and fine, but BOTH are zero-operand with WHICH COUNTER baked
// into the opcode. "Read the hardware counter" is therefore ONE VERB across
// ISAs, and `cntvct_el0` is an AArch64 spelling detail exactly as `edx:eax` is
// an x86 one. A generic `mrs` + sysreg operand would give the arm64 read a
// DIFFERENT SHAPE from the x86 read, so every shared consumer would acquire a
// per-arch shape distinction — the `if (arch == …)` the bar hard-vetoes,
// arriving through CONFIG rather than C++, which is the slower and worse way
// for it to arrive.
//
// ★★★ WHY THE KEY IS POSITIONAL AND NOT "THE LITERAL OPERAND". That simpler
// key is already FALSE about this instruction's mirror image: gas writes
// `msr tpidr_el0, x0` — selector FIRST (✔MEASURED 2026-08-14,
// `aarch64-linux-gnu-as` 2.42, `d51bd040`) — and this dialect declares
// `operandOrder: destinationFirst`, so a position-blind rule would read the
// SELECTOR as the destination. Declaring the INDEX is what keeps the key from
// being wrong about a form nothing ships yet.
// ⚠ A SELECTOR AT INDEX 0 SUPPRESSES `destinationFirst` FOR THAT POSITION, and
// it does so BY CONSTRUCTION rather than by a second rule: the selector is
// excluded from the operand list before the destination is read, so position 0
// is simply no longer a candidate. No `msr` row ships (nothing needs one, and
// §A.2 cuts both ways) — the point is only that the KEY is not wrong about it.
struct AsmOperandSelector {
    // Position in the operand list AS WRITTEN, counting selectors.
    std::uint32_t index = 0;
    // The exact operand text that selects this row. ⚠ COMPARED, NEVER
    // INTERPRETED: the engine holds no opinion about what a system register or
    // a condition spelling MEANS, which is what keeps `sysreg` out of the
    // shared substrate's vocabulary.
    std::string   name;
};

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
    // ★★★ OPTIONAL — THE DESTINATION'S WIDTH, WHEN THIS SPELLING WRITES A
    // RESULT WIDER (OR NARROWER) THAN THE VALUE IT OPERATES ON.
    // D-ASM-X86-WIDTH-EXTENDING-MOVES-UNSPELLABLE.
    //
    // ⚠ ABSENT IS NOT "64" AND NOT "UNKNOWN" — it is "this spelling has ONE
    // width", which is true of every ordinary instruction and is why the
    // single-width model held for so long. `width` then governs BOTH sides
    // and the lowering requires every data register to agree, which is the
    // guard that refuses `movl %rax, %ecx` (✔MEASURED, GNU as 2.42 refuses
    // it too — `operand type mismatch`).
    //
    // ★★ A WIDTH-EXTENDING MOVE IS TWO WIDTHS IN ONE INSTRUCTION BY
    // DEFINITION and no amount of relaxing the agreement check can express
    // it — relaxing would ACCEPT `movl %rax, %ecx`, trading one conformance
    // gap for its mirror image. PRESENT therefore SPLITS the check by ROLE
    // rather than weakening it: the destination-position register must be
    // exactly `destWidth`, every source register exactly `width`, and any
    // other pairing is refused as loudly as before. `movzbl` declares
    // width 8 / destWidth 32, so `movzbl %cl, %ecx` is accepted and
    // `movzbl %cl, %rcx` is refused — which is precisely gas's own answer
    // to both (✔MEASURED, 2.42).
    //
    // ★ IT IS A DIALECT FACT FOR THE SAME REASON `width` IS: WHERE the two
    // widths are written is the dialect's business. AT&T bakes both into the
    // mnemonic (`movzbl` = byte source, long destination); an ISA that spells
    // the pair in its REGISTER names (`sxtb x0, w0`) declares the same two
    // numbers on its own row and the engine reads them identically. The
    // TARGET side needs nothing new: an instruction that writes a different
    // width is a different machine instruction, so it is a different opcode
    // in `opcodeNames`, exactly as `load` and `load_subreg` already are.
    //
    // ⚠ A row declaring `destWidth` MUST declare `width` too — a destination
    // width with no source width states half a fact, and the half it omits is
    // the one the LIR instruction carries. The loader refuses it.
    std::optional<std::uint32_t> destWidth;
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
    // ⚠⚠ ON A SELECTOR ROW, `cond` BESIDE `operandSelectors` IS **NOT ONE FACT
    // WRITTEN TWICE**, and "simplifying" it is a silent miscompile.
    // `{"spelling":"cset","operandSelectors":[{"index":1,"name":"lo"}],
    //   "cond":"ult"}` maps the gas SPELLING to the `TargetCondCode` — the same
    // non-identity the shipped `b.lo`/`ult` row already carries. DERIVING
    // `cond` from the selector string is precisely the letter-pattern-matching
    // the `b.<cc>` comment warns about: gas's `lt/le/gt/ge` are SIGNED and its
    // unsigned peers are `lo/ls/hi/hs`, so a derivation would map `ls` to `sle`
    // and miscompile every unsigned comparison with a clean build log.
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

    // OPTIONAL; EMPTY on every ordinary row. See `AsmOperandSelector`.
    std::vector<AsmOperandSelector> operandSelectors;

    // The selector declared at `index`, or nullptr.
    [[nodiscard]] AsmOperandSelector const*
    selectorAt(std::uint32_t index) const noexcept {
        for (auto const& s : operandSelectors) {
            if (s.index == index) return &s;
        }
        return nullptr;
    }
};

// ★★★ CAN TWO ROWS SHARING A SPELLING BOTH MATCH THE SAME LINE?
//
// ★★ THE ANSWER IS A LOAD-TIME REFUSAL, AND IT IS THE PRICE OF ADMISSION FOR
// THE SELECTOR KEY (plan 29 §4.7.1). Without it, selectors buy the `cntvct` row
// by planting the `ldr`/`ldur` bug one level up: that pair was split into
// separate rows precisely because guard election *"would take the first and
// silently encode LDR where the programmer wrote LDUR"*. So NEVER first-match
// and NEVER most-specific-silently-wins — two rows that could both take a line
// are refused when the DOCUMENT loads, naming both.
//
// ★ THE PREDICATE IS DELIBERATELY CONSERVATIVE — provably disjoint, or refused.
// Two rows are distinguishable ⟺ some index carries a selector in BOTH rows
// with DIFFERENT names, because that index alone then decides every line. A
// selector row against a bare row (no common index) is NOT separable that way:
// the bare row matches everything the selector row matches, so it is refused —
// which is exactly the §4.7.1 case. Erring toward refusal costs a config author
// one diagnostic; erring the other way costs a reader a wrong instruction with
// no diagnostic at all.
//
// ★ AND IT IS WHY SELECTORS FORECLOSE NOTHING. If a target ever needs a real
// system-register operand KIND, a selector row and a generic row COEXIST — the
// selector row is a SPECIALIZATION, the same relation `nop` has to a generic
// form — *provided this refusal is in place*. That is the second reason it is
// not optional.
//
// ⚠⚠ THE PREDICATE MOVED ONTO `AssemblyConfig` WHEN `spellingCase` LANDED, AND
// THAT IS NOT COSMETIC. "Different names" is a question about SPELLINGS, so it
// is answered under the document's own case policy: in a folding dialect,
// selectors `eq` and `EQ` at one index name the SAME condition, so two rows
// carrying them are NOT disjoint and one line would match both. A free function
// could not see the policy, and a caller that forgot to pass it would silently
// re-open exactly the §4.7.1 ambiguity this refusal exists to close.

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
    // `SectionData` ONLY (required there, refused on every other verb). The
    // `DataSectionKind` spelling from `core/types/section_kind.hpp` —
    // `"rodata"` / `"data"` / `"bss"` / `"tdata"` / `"tbss"` / `"relro"`.
    //
    // ★ STORED AS A NAME, RESOLVED BY THE LOWERING — the same layering
    // `opcodeNames` and `condName` already use, and for a structural reason
    // rather than a stylistic one: `section_kind.hpp` includes
    // `target_schema.hpp`, which includes `grammar_schema.hpp`, which includes
    // THIS header, so naming the enum here is a genuine include cycle. The
    // LOADER still validates the name against `dataSectionKindFromName`, so an
    // unknown section is a load error naming the closed set — never a silently
    // mis-routed item.
    std::string      sectionName;
    // `EmitData` ONLY (required there, refused on every other verb). How many
    // bytes ONE operand of this directive occupies: 1 (`.byte`), 2 (`.word` /
    // `.short` / `.hword`), 4 (`.long` / `.int` / `.word` on some ports) or 8
    // (`.quad` / `.xword`).
    // ⚠ THE SPELLING→WIDTH MAP IS DIALECT DATA, NOT ENGINE DATA, AND `.word` IS
    // THE PROOF: it is 2 bytes on x86 gas and 4 bytes on several other gas
    // ports. An engine that knew "`.word` means two bytes" would silently halve
    // every table on the ports where it does not.
    // ✔MEASURED 2026-08-13 on BOTH ports, one label per directive with the
    // offsets read back by `objdump -t` (each delta IS the width above it):
    // x86_64 `as` — byte 1, word 2, short 2, long 4, quad 8, and `.xword` is
    // REJECTED outright; `aarch64-linux-gnu-as` — byte 1, hword 2, short 2,
    // word 4, long 4, quad 8, xword 8. `.word` really is 2 on one and 4 on the
    // other, in the same build of binutils.
    std::uint32_t    unitBytes = 0;

    // ★★★ IS THIS SPELLING REACHABLE AS A BARE DIRECTIVE, OR ONLY AS THE
    // OPERAND OF A `SectionByName` ONE?
    // (D-ASM-SECTION-DIRECTIVE-WITH-OPERAND-UNMODELLED.)
    // True on a row that names a section the dialect can SELECT but cannot
    // WRITE as a directive of its own — gas's `.rodata`, which exists only
    // as `.section .rodata`.
    //
    // ★ IT IS A REACHABILITY FACT, NOT A SECOND VOCABULARY, AND THAT IS THE
    // whole reason the row lives in `directives[]` at all rather than in a
    // private section-name table hung off the `.section` row. A separate table
    // would have restated the `.data`→`Data` and `.bss`→`Bss` mappings that are
    // already here — the exact duplication shape this arc keeps closing (a
    // dotted mnemonic declared once as a token and once as an instruction) —
    // and the two copies would drift the first time a section was renamed.
    // ⇒ ONE table, one lookup, and this flag says which door a row opens.
    // ⚠ REFUSED ON ANY VERB THAT DOES NOT OPEN A SECTION (`asmVerbOpensSection`)
    // and refused in a dialect that declares no `SectionByName` row at all —
    // in either case nothing could ever reach the row, and a row nothing can
    // reach is config that silently does nothing.
    bool             operandOnly = false;

    // ★★★ `FrameRule` ONLY (required there, refused on every other verb): WHICH
    // rule change this spelling states. The value is a `CfiOpKind` — the shared
    // core's unwind vocabulary — resolved AT LOAD from the name
    // `cfiOpKindName()` publishes, so an unknown rule is a load error naming
    // the closed set rather than a directive that silently carries nothing.
    //
    // ★ WHY THE NAMES LINE UP 1:1 WITH gas's SPELLINGS AND THAT IS NOT A
    // COINCIDENCE: `core/types/cfi.hpp` states outright that every enumerator
    // was written from a `.cfi_*` spelling a real assembler emits. So the AT&T
    // row for `.cfi_def_cfa_register` names `"def_cfa_register"` and the
    // mapping reads as an identity — but it is DECLARED, because a dialect
    // whose spelling is `.frame_cfa_reg` binds the same rule with no engine
    // change, which is the whole reason the spelling and the meaning are two
    // fields.
    std::optional<CfiOpKind> frameRule;
    // `FrameRule` ONLY. TRUE on `.cfi_rel_offset`, whose stated offset is
    // measured from the CURRENT CFA rather than from the canonical frame
    // address — gas defines it as `.cfi_offset reg, off − <current CFA
    // offset>` and resolves it against its own running state. There is no
    // `CfiOpKind` for it and there must not be: it is the SAME rule
    // (`RegAtCfaOffset`) stated in a different coordinate system, so a second
    // enumerator would put one fact in two places and every consumer would
    // carry both arms forever. The producer folds it; the representation never
    // sees it.
    bool                     frameOffsetFromCfa = false;
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

    // ★★★ THE TEMPLATE SURFACE — how an EMBEDDED `__asm__` template differs
    // from a standalone `.s`, declared as two names rather than built into the
    // engine (D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER, 2026-08-15).
    //
    // ★★ WHY A LEXER MODE AND NOT A GRAMMAR ALTERNATIVE. ✔MEASURED, and
    // EXERCISED rather than read: `%` is ALREADY the register sigil in the AT&T
    // dialect and the type sigil in the arm64 one, and
    // `detectAmbiguousAlternatives` REFUSES two sibling alts sharing a FIRST
    // token — two in-process tests watch the loader refuse the naive arm. So a
    // placeholder needs `%` to carry a SECOND kind in template context, which
    // is precisely what a per-mode `tokens` override is for. The mode is named
    // here so the template parse entry selects it from CONFIG rather than from
    // a hard-coded string.
    // ★ AND THE MODE BUYS A CORRECTNESS PROPERTY, NOT ONLY AN EXPRESSIVENESS
    // ONE: a placeholder token is minted ONLY inside the template mode, so
    // every placeholder-headed shape is unreachable from a `.s` BY
    // CONSTRUCTION. gas rejects `%0` in a `.s`; so does DSS, without a check
    // that could be forgotten.
    //
    // `templateOperandRule` names the SHARED grammar's placeholder rule
    // (`asm.lang.json`'s `asmTemplateOperand`). The lowering engine asks the
    // dialect which rule that is, exactly as it asks which rule is
    // `labelTailRule` — so the engine holds no opinion about how a placeholder
    // is spelled and never names a rule of another document in C++.
    //
    // ⚠ OPTIONAL AS A SET, REQUIRED OF EACH OTHER — the loader refuses any
    // proper subset. A mode with no rule mints tokens no shape accepts; a
    // rule with no mode declares a shape whose FIRST token nothing ever
    // produces; and see `templateLabelRule` below for the third. Every one of
    // them is a silent no-op, which is the one outcome this config surface is
    // shaped to make impossible. A dialect that hosts no embedded templates
    // declares none of them, and all stay invalid.
    LexerModeId templateLexerMode{};
    RuleId      templateOperandRule{};

    // ★★★ WHICH PLACEHOLDER RULE IS THE `asm goto` **LABEL** REFERENCE —
    // `asm.lang.json`'s `asmTemplateLabelRef`, naming `%l[done]` and `%l2`.
    //
    // ★★ IT EXISTS SO THE ENGINE CAN TELL THE TWO PLACEHOLDER FAMILIES APART
    // WITHOUT EVER LOOKING AT A BYTE. An operand placeholder denotes a
    // REGISTER and is asked for through `AsmLoweringHost::resolveRegister`; a
    // label placeholder denotes a BLOCK and is asked for through
    // `resolveBranchTarget`. Two questions, two virtuals — and the only
    // dialect-neutral way to know which one a matched node is asking is the
    // rule it was produced by. Testing the `%l` bytes in C++ would put a
    // second owner beside `semantics.inlineAsmTemplateLexemes`, which is the
    // defect `D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER`
    // closed; this key is what keeps it closed.
    //
    // ⚠⚠ MANDATORY FOR EVERY DIALECT DOCUMENT, PRESENT AND FUTURE, AND THAT IS
    // A LOAD-BREAKING CHANGE FOR ANY OUT-OF-TREE DIALECT — stated plainly
    // rather than discovered. `asm.lang.json`'s `asmLine` closure SPELLS the
    // template holes, so a dialect that imports the shared standalone surface
    // imports the placeholder shapes whether or not it wants them, and the
    // capability was therefore never really optional for such a document. That
    // is not a new policy: it is the consequence recorded in the closed row
    // `D-CONFIG-ASM-TEMPLATE-CAPABILITY-NO-LONGER-OPTIONAL-FOR-A-SHARED-SURFACE-IMPORTER`,
    // now extended to a third key by the same argument. A dialect importing a
    // NARROWER entry — one whose closure does not reach the template shapes —
    // may still declare none of the three.
    RuleId      templateLabelRule{};

    // Indexed by `static_cast<std::size_t>(AsmOperandRole)`. Every role is
    // REQUIRED when the block is present — a partial `operandForms` is a load
    // error, never a silently-unrecognized operand shape.
    std::array<RuleId, kAsmOperandRoleCount> operandFormRules{};

    AsmOperandOrder operandOrder = AsmOperandOrder::DestinationLast;

    // How a WRITTEN spelling is matched against DECLARED VOCABULARY. See
    // `AsmSpellingCase` for the measurement that fixes which surfaces this
    // reaches and which three it must never reach.
    // ⚠ THE MEMBER DEFAULT IS THE STRICT ARM AND THE KEY IS STILL REQUIRED.
    // Defaulting to `AsciiFolded` would make a dialect that forgot the key
    // silently accept spellings it never declared; defaulting to `Sensitive`
    // and leaving the key optional would make it silently refuse what its
    // reference assembler takes — the defect this field closed. So the loader
    // demands the key exactly as it demands `operandOrder`, and this
    // initializer only fixes the value of a default-constructed config.
    AsmSpellingCase spellingCase = AsmSpellingCase::Sensitive;

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

    // ★★★ THE ONE SPELLING COMPARISON. Every match against declared vocabulary
    // — mnemonic, directive, operand selector, register name — goes through
    // this function and nothing else re-implements it. Four independent `==`s
    // honouring a policy is four chances for one of them to be forgotten, and a
    // forgotten one is not a compile error: it is a dialect that folds its
    // mnemonics and refuses its registers.
    //
    // ★ THE FOLD IS `dss::asciiToLower`, THE ONE FOLDING HELPER — never a
    // hand-rolled loop (`ascii_case.hpp` exists because the second loop is how
    // two comparisons drift apart).
    //
    // ⚠ THE TWO EARLY-OUTS ARE CORRECTNESS-NEUTRAL AND NOT AN OPTIMIZATION
    // GAMBLE. An ASCII fold is a per-code-unit map, so it can never change a
    // string's LENGTH; and two byte-identical strings fold identically under
    // any policy. So the allocating path is reached only by a same-length
    // non-identical pair — which, in a dialect whose tables are lowercase and a
    // source that is too, is never.
    [[nodiscard]] bool spellingMatches(std::string_view declared,
                                       std::string_view written) const {
        if (declared.size() != written.size()) return false;
        if (declared == written) return true;
        if (spellingCase == AsmSpellingCase::Sensitive) return false;
        return asciiToLower(declared) == asciiToLower(written);
    }

    // The canonical form of `s` under this document's policy — the key a
    // DUPLICATE-DETECTING container must be keyed on.
    //
    // ★★ IT IS THE SAME FOLD, SO IT CANNOT DISAGREE WITH `spellingMatches`:
    // `spellingMatches(a, b)` ⟺ `spellingKey(a) == spellingKey(b)`, by
    // construction, since both route through `asciiToLower` and the size/equal
    // early-outs above are implied by string equality. That equivalence is what
    // makes the LOAD-TIME refusal and the RUN-TIME match one rule: a loader that
    // deduplicated on raw spellings while the engine matched on folded ones
    // would let `mov` and `MOV` both load clean and both take the same line —
    // the §4.7.1 ambiguity, arriving through the back door.
    [[nodiscard]] std::string spellingKey(std::string_view s) const {
        return spellingCase == AsmSpellingCase::AsciiFolded
                   ? asciiToLower(s)
                   : std::string{s};
    }

    // See the block comment above `AsmDirectiveSpelling` for why this is a
    // member rather than a free predicate.
    [[nodiscard]] bool
    rowsAreSelectorDisjoint(AsmInstructionSpelling const& a,
                            AsmInstructionSpelling const& b) const {
        for (auto const& sa : a.operandSelectors) {
            auto const* sb = b.selectorAt(sa.index);
            if (sb != nullptr && !spellingMatches(sb->name, sa.name)) {
                return true;
            }
        }
        return false;
    }

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

    // Every directive spelling bound to `verb` that a source file may actually
    // WRITE, comma-joined and with the dialect's own introducer omitted (the
    // row stores it that way). Built for the refusal that has to tell a reader
    // WHICH directive it wanted; a hand-written list in the diagnostic would go
    // stale the first time a dialect renamed a spelling.
    // ⚠ `operandOnly` ROWS ARE EXCLUDED, AND THAT IS THE POINT OF THE FLAG
    // REACHING THIS FUNCTION. Every caller is telling a reader "write one of
    // these"; listing `rodata` there would send them to write `.rodata`, which
    // this dialect (and gas) refuse. A diagnostic that suggests a fix the next
    // compile rejects is worse than one that lists less.
    [[nodiscard]] std::string spellingsForVerb(AsmDirectiveVerb verb) const {
        std::string out;
        for (auto const& row : directives) {
            if (row.verb != verb || row.operandOnly) continue;
            if (!out.empty()) out += ", ";
            out += '\'';
            out += row.spelling;
            out += '\'';
        }
        return out;
    }

    // Every spelling a `SectionByName` operand may name — i.e. every row whose
    // verb OPENS a section, `operandOnly` or not. ★ THE TWO LISTS ARE
    // DELIBERATELY DIFFERENT AND BOTH ARE DERIVED FROM THE SAME ROWS: what you
    // may WRITE as a directive and what you may NAME after `.section` differ by
    // exactly the `operandOnly` rows, and stating each from the table means
    // neither can go stale.
    [[nodiscard]] std::string sectionOperandSpellings() const {
        std::string out;
        for (auto const& row : directives) {
            if (!asmVerbOpensSection(row.verb)) continue;
            if (!out.empty()) out += ", ";
            out += '\'';
            out += row.spelling;
            out += '\'';
        }
        return out;
    }

    // The section-opening row a `SectionByName` operand names, or nullptr.
    // ⚠ LOOKUP IS OVER SECTION-OPENING ROWS ONLY, so `.section text` reaches
    // the `sectionText` row and `.section globl` reaches nothing rather than
    // silently reaching a directive that has no section to open.
    //
    // ⛔⛔ THE `==` HERE IS EXACT ON PURPOSE AND `spellingCase` MUST NOT REACH
    // IT — THIS ARGUMENT IS A SECTION *NAME*, NOT A DECLARED SPELLING. It is
    // the one seam in this header where the folding rule looks like it applies
    // and measurably does not: ✔MEASURED 2026-08-15, x86_64 `as` 2.42 —
    // `.section .RODATA` is rc=0 and opens a section literally named
    // `.RODATA`, and a source writing BOTH `.rodata` and `.RODATA` gets TWO
    // sections of 8 bytes each (`objdump -h`). A fold here would put `.RODATA`
    // data in `.rodata`; the reference puts it somewhere else. That is a silent
    // divergence, not a conformance fix — the same reason symbol names and the
    // `.type` marker stay exact. The DIRECTIVE that carries this operand is a
    // different question and does fold: `.SECTION .rodata` reaches this row
    // through `directiveBySpelling` and lands in `.rodata` (✔MEASURED, rc=0,
    // one section).
    [[nodiscard]] AsmDirectiveSpelling const*
    sectionRowByName(std::string_view s) const noexcept {
        for (auto const& row : directives) {
            if (!asmVerbOpensSection(row.verb)) continue;
            if (row.spelling == s) return &row;
        }
        return nullptr;
    }

    // Does this dialect declare a directive that names its section by operand?
    // The loader uses it to refuse an `operandOnly` row nothing could reach.
    [[nodiscard]] bool hasSectionByName() const noexcept {
        for (auto const& row : directives) {
            if (row.verb == AsmDirectiveVerb::SectionByName) return true;
        }
        return false;
    }

    // The FIRST row spelled `s`. ⚠ A SPELLING MAY NOW HAVE MORE THAN ONE ROW —
    // a selector-carrying mnemonic has one per selector value (`cset` × 12), and
    // the loader has already proved they are pairwise disjoint. This accessor
    // therefore answers "is this spelling declared at all"; the row that
    // actually applies to a LINE is chosen by the selectors, which need the
    // operands. Use `instructionRowCount` when the count is the question.
    [[nodiscard]] AsmInstructionSpelling const*
    instructionBySpelling(std::string_view s) const {
        for (auto const& row : instructions) {
            if (spellingMatches(row.spelling, s)) return &row;
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t instructionRowCount(std::string_view s) const {
        std::size_t n = 0;
        for (auto const& row : instructions) {
            if (spellingMatches(row.spelling, s)) ++n;
        }
        return n;
    }

    [[nodiscard]] AsmDirectiveSpelling const*
    directiveBySpelling(std::string_view s) const {
        for (auto const& row : directives) {
            if (spellingMatches(row.spelling, s)) return &row;
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

[[nodiscard]] inline std::optional<AsmSpellingCase>
asmSpellingCaseFromName(std::string_view name) {
    for (auto const& [text, v] : kAsmSpellingCaseNames) {
        if (text == name) return v;
    }
    return std::nullopt;
}

// The config spelling of a policy — read out of the SAME table `fromName`
// validates against, so the two can never name it differently.
[[nodiscard]] inline std::string_view
asmSpellingCaseName(AsmSpellingCase v) noexcept {
    for (auto const& [text, candidate] : kAsmSpellingCaseNames) {
        if (candidate == v) return text;
    }
    return "<unnamed>";
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
