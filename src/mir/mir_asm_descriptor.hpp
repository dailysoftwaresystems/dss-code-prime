#pragma once

#include "core/export.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Per-module inline-asm descriptor pool (inline-asm P5, plan 29 §4.4). A MIR
// `InlineAsm` / `InlineAsmGoto` instruction carries only a pool INDEX in its
// `payload`; the template text, the constraint lists and the clobber list live
// here. Exactly the `MirLiteralPool` shape and for exactly its reason: a parallel
// IR owns parallel data, so the `mir` library still depends only on `core`.
//
// ★ WHY A POOL AND NOT MORE OPERANDS. The asm block's INPUTS are already MIR
// operands (they are SSA values the allocator must see). Everything here is
// STATIC text/vocabulary that no pass may substitute — putting it in the operand
// list would make it look substitutable to every rebuild.
//
// ⚠⚠ A POOL INDEX IS NOT SELF-CARRYING, AND THAT IS THE WHOLE HAZARD. MIR is
// rebuilt by three live verbatim-copy sites (`opt/passes/mir_rebuild_helper.cpp`,
// `opt/passes/inlining.cpp` ×2) which forward `instPayload` into a NEW module —
// whose pool is empty. A forwarded index would then name a different entry, or
// none. The structural defence is that `MirBuilder::addInst` REFUSES the two asm
// opcodes (they have dedicated builders, the `Arg`/`Const`/`GlobalAddr`
// precedent), so a copy site that forgot to re-add the descriptor ABORTS instead
// of silently dropping the clobber list. The `Const`/literal-pool pair already
// works exactly this way.

namespace dss {

// Register-class envelope, defined in `core/types/target_schema.hpp`. Declared
// opaquely here (a fixed-underlying-type enum declaration yields a COMPLETE
// type) so this header — reached by every `mir.hpp` consumer — does not pull in
// a 3.4k-line header for one 5-value enum. TUs that name an enumerator include
// `target_schema.hpp` themselves, as they already must to spell `TargetRegClass`.
enum class TargetRegClass : std::uint8_t;

// ONE inline-asm operand's binding contract, as the SOURCE wrote it.
//
// ★ THE CLASS IS THE CONSTRAINT'S, NEVER THE TYPE'S (plan 29 §4.4.3). `"=x"` on
// an integer-typed lvalue is legal and means SSE; deriving the class from the
// MIR type would file it in the GPR pool. This field is the producer-side half
// of the same fact `return_piece_payload` carries on the piece.
struct MirAsmOperand {
    // The constraint string exactly as written, minus nothing: `"=r"`, `"r"`,
    // `"+r"`, `"=&r"`, `"=a"`, `"m"`, `"i"`. Kept verbatim because diagnostics
    // quote it and because the modifier letters are per-target vocabulary this
    // tier must not enumerate.
    std::string    constraint;
    // The register class the CONSTRAINT selects.
    TargetRegClass regClass{};
    // Non-empty ⇒ the constraint PINS one physical register (`"=a"` → the
    // target's register NAME, resolved by the front end against the target's
    // register table — this tier never enumerates register names). Empty ⇒ the
    // allocator picks within `regClass`.
    std::string    fixedRegister;

    // ★★★ THE THIRD BINDING ARM: THE OPERAND **FORM**, for a letter that binds
    // one (`"m"` → `membase`, `"i"` → `imm32`). `regClass` above is meaningless
    // on such an operand and asking it for one is the defect this pair closes —
    // a memory operand requires NO register class, because the ADDRESS is what
    // gets a register (D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-BEING-DECLARED).
    // The two arms are mutually exclusive: `TargetAsmConstraint::binds` names
    // exactly one, so `operandKindResolved` and a meaningful `regClass` are
    // never both live on one entry.
    //
    // ⚠ THE BOOL IS LOAD-BEARING FOR THE REASON THE `optional` PAYLOADS ARE ONE
    // TIER UP: `OperandKindFilter::Reg` is 0, so a default-constructed operand
    // would otherwise read back as "binds a register form" — a plausible wrong
    // answer rather than a loud one.
    //
    // Raw `OperandKindFilter` value, for the reason `TargetRegClass` is declared
    // opaquely above: this header is reached by every `mir.hpp` consumer and
    // must not pull in the 3.4k-line target schema. A TU that names an
    // enumerator includes `target_schema.hpp` itself, as it already must.
    bool           operandKindResolved = false;
    std::uint8_t   operandKind         = 0;

    // `"+r"` — this operand is read AND written: one input tied to one output.
    // Recorded on BOTH halves of a lowered `+` operand, because both entries
    // describe the SAME source operand and both are quoted by diagnostics.
    bool           isReadWrite = false;
    // `"=&r"` — earlyclobber: the output is written before every input is read,
    // so it may not share a register with any of them.
    bool           isEarlyClobber = false;

    // ★★★ SET ⇔ THIS IS AN **INPUT** ENTRY THAT SHARES ITS LOCATION WITH THE
    // OUTPUT AT THE INDEX IT HOLDS — GNU's matching constraint (6.47.2.4 `"0"`),
    // which is how a `"+r"` operand's READ half reaches the template
    // (D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE).
    //
    // A `+` operand is ONE thing the source wrote and TWO things the machine
    // needs: a value to read and a location to write. `hir_to_mir.cpp` files the
    // write half in `outputs` (so it gets a `ReturnPiece` and a store-back) and
    // APPENDS the read half to `inputs` — after every source-written input, so
    // no existing `%N` index moves — with this field naming its partner. Without
    // it the read half is unrecoverable: an input tied to output 0 and an
    // ordinary input are byte-identical otherwise.
    //
    // ⚠ THE INDEX IS CARRIED, NEVER RECONSTRUCTED, and that is the whole point.
    // "the last N inputs are the tied ones, in output order" is derivable ONLY
    // while every producer happens to append in that order — the same
    // reconstruct-the-fact-from-the-shape trade that made `isExtended` below a
    // conformance defect one tier down. `std::optional<index>` is the shape
    // `TargetOpcodeInfo::requires2Address` already uses for a (result, operand)
    // tie, and it carries the same warning: NEVER compare it to `true`/`false`
    // — `opt == true` is a VALUE comparison and silently means "tied to output
    // 1". Ask `.has_value()`, read the index with `*`.
    //
    // ⛔ AN OUTPUT NEVER CARRIES IT. The tie is written on the input because
    // that is where GNU writes it and because the consumer binds outputs FIRST:
    // when input j is bound, `outs[*tiedOutput]` already holds the register the
    // template will use for both halves.
    std::optional<std::uint32_t> tiedOutput;

    // ★★★ EVERY SPELLING THIS OPERAND ANSWERS TO, AS THE TEMPLATE WRITES IT —
    // MINTED BY THE FRONT END, ONLY EVER *COMPARED* HERE AND BELOW. The
    // positional form always, plus the symbolic form when the source named the
    // operand (`[in]`). Order is [positional, symbolic].
    //
    // ⚠ EMPTY ON EXACTLY ONE KIND OF ENTRY, AND THAT IS THE CORRECT ANSWER: the
    // synthesized read half of a `"+r"` operand (the entry carrying `tiedOutput`
    // above). The source wrote that operand ONCE, so exactly one entry — the
    // OUTPUT — answers to its spellings; giving the read half a copy would
    // publish the same `%N` twice, at two different registers, and a consumer
    // binding one row per spelling would silently keep whichever it saw last.
    // This is the same "the tied entries sit past the last index a `%N` can
    // reach" rule `inputs` states below.
    //
    // ★★ IT IS CARRIED BECAUSE THE INDEX CANNOT BE RECOMPUTED AT THIS TIER, AND
    // THAT IS MEASURED, NOT STYLISTIC. An index derived here as
    // `outputs.size() + inputs.size()` OVERCOUNTS by the number of `"+"`
    // operands, because `inputs` carries the SYNTHESIZED tied read halves (see
    // `tiedOutput` above) while the source counts a `"+"` operand ONCE.
    // ✔MEASURED: `asm goto ("…%l2…" : [o]"+r"(x) : "r"(y) : : L)` — a program
    // gcc compiles — has source base 2 and MIR-computed base 3. Minting at the
    // front end deletes the second count: there is ONE list, the source's, and
    // two tiers cannot disagree when only one of them counts.
    //
    // ⛔ THE BYTES ARE THE LANGUAGE'S, NOT THIS FILE'S. They come from the
    // dialect's declared template lexemes (`semantics.inlineAsmTemplateLexemes`,
    // the owner won by `D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER`).
    // No consumer may rebuild a spelling from a sigil literal — doing so would
    // give the convention a second owner, which is the defect this field closes.
    std::vector<std::string> spellings;
};

struct DSS_EXPORT MirAsmDescriptor {
    // The assembly template, verbatim. Parsed by the dialect's own grammar at
    // expansion time (plan 29 §4.6: `%N` binding is STRUCTURAL) — never here.
    std::string templateText;

    // ★★★ THE EMBEDDING STATEMENT'S LOCUS — CARRIED, NEVER RECONSTRUCTED
    // (D-ASM-TEMPLATE-DIAGNOSTIC-DOES-NOT-NAME-THE-C-STATEMENT). A template
    // diagnostic renders against the mid-compile-minted `<inline asm>` buffer,
    // which names the byte INSIDE the template but never the source statement
    // that embedded it — so a reader with three `__asm__` blocks in one file
    // cannot tell which one is being refused. ✔MEASURED, both references:
    // on a 3-line C file whose `__asm__` names a bogus mnemonic, gcc 13.3
    // reports the C statement's `<file>:<line>: Error: no such instruction: …`
    // and clang 18 reports its `<file>:<line>:<col>: error: …` with a
    // `note: instantiated into assembly
    // here` block — BOTH make the C statement the primary locus, neither has
    // template-primary. HIR→MIR is the last tier that can see the statement's
    // span (MIR→LIR has no source map), so the locus travels on the descriptor
    // exactly as `isExtended` does: a fact only the front end can see, carried
    // rather than re-derived. `statementBuffer` is the strong id's invalid
    // sentinel (`.valid() == false`) for the span-less callers — the LSP, the
    // FFI header parser and direct-API tests bind no source map, and there the
    // consumer omits the locus rather than guessing one.
    bool        hasStatementLocus = false;
    BufferId    statementBuffer{};
    // `SourceSpan` is factory-only (`of`/`empty`), so the span-less default is
    // the empty span at byte 0 — meaningless until `hasStatementLocus` says
    // otherwise, exactly the `regClassResolved` rule one field up.
    SourceSpan  statementSpan = SourceSpan::empty(0);
    // `__asm__ volatile` — the block may not be moved or elided even when its
    // outputs are unused. (Both asm opcodes are `hasSideEffects` +
    // `opcodeClobbersMemory` regardless, so this records the SOURCE's word; it
    // is not the optimizer's only defence.)
    bool isVolatile = false;

    // ★★★ THE STATEMENT CARRIED AT LEAST ONE `:` — THE EXTENDED FORM. It is a
    // LEXICAL fact about the TEMPLATE, and it is carried rather than derived
    // because the two forms are read by DIFFERENT LEXER MODES
    // (`AsmTemplateSurface`): in a BASIC template `%` is LITERAL and the text
    // reaches the assembler verbatim; in an EXTENDED one `%` introduces an
    // operand. ✔MEASURED 2026-08-15 on gcc 13.3.0 and clang 18.1.3 (re-measured
    // for this carrier, not inherited):
    //
    //   `__asm__("xorl %eax,%eax")`             BASIC     → both rc=0.
    //   `__asm__("xorl %eax,%eax" :::)`         EXTENDED  → both rc=1 ("operand
    //                                                      number missing after
    //                                                      %-letter" / "invalid
    //                                                      % escape").
    //   `__asm__("movl %%eax, %%ebx" :::)`      EXTENDED  → both rc=0.
    //
    // ★ **ANY** COLON MAKES IT EXTENDED, INCLUDING `:::` WITH EVERY SECTION
    // EMPTY — which is exactly the shape no consumer can reconstruct. A reader
    // that asks "does this descriptor have any outputs, inputs or clobbers?"
    // gets `no` for that statement and reads it on the BASIC surface, so the
    // third row above — a program both reference compilers COMPILE — was
    // REFUSED by this build with a raw `expected 'Identifier' — got '%'` parse
    // error out of the `.s` lexer (✔MEASURED at the CLI, same date, against the
    // matched `::: "ebx"` control which compiled clean). Under
    // [[feedback_reference_compilers_are_the_spec]] refusing what the reference
    // toolchain builds is as much a defect as accepting what it rejects, so the
    // fact travels instead of being re-derived.
    bool isExtended = false;

    // Outputs in SOURCE order. Output k is result piece k: the `ReturnPiece`
    // anchored to this instruction whose payload ordinal is k.
    std::vector<MirAsmOperand> outputs;
    // Inputs in SOURCE order, ALIGNED 1:1 with the instruction's MIR operands
    // (operand j is input j — an asm block has no callee operand, which is why
    // `InlineAsm` is `{0,N}` and not `Call`'s `{1,N}`).
    //
    // ⚠ THE SOURCE-WRITTEN INPUTS COME FIRST, THEN THE TIED READ HALVES. Every
    // `"+r"` OUTPUT appends one entry here carrying `tiedOutput` — GNU's own
    // transformation (a `+` operand becomes an `=` output plus a matching `"0"`
    // input, appended AFTER the explicit inputs so no index the template can
    // name is disturbed). The `%N` space this list feeds is therefore still
    // outputs-then-SOURCE-inputs for every spelling a template may write; the
    // tied entries occupy the indices past the last one the front end lets a
    // `%N` reach, exactly as they do in gcc.
    std::vector<MirAsmOperand> inputs;

    // ★★★ ONE ENTRY PER `asm goto` LABEL, IN LABEL-SUCCESSOR ORDER — i.e. entry
    // `j` describes CFG successor `j`, and the successors after the last of them
    // are the fall-through (see `MirOpcode::InlineAsmGoto` and
    // `MirBuilder::addInlineAsmGoto`, which owns the successor layout). Each
    // entry holds every spelling that label answers to: the bracketed form when
    // it has a name and the positional form on the reference numbering, minted
    // by the front end from the dialect's declared lexemes exactly like
    // `MirAsmOperand::spellings`.
    //
    // ★★ THE SIZE IS A CHECKED INVARIANT, NOT A CONVENTION:
    // `labelSpellings.size() + 1 == blockSuccessors(theAsmBlock).size()`. Both
    // `MirBuilder::addInlineAsmGoto` and `MirBuilder::cloneInlineAsmGoto` refuse
    // a descriptor that disagrees, and
    // `MirVerifier::checkTerminatorSuccessorArity` re-checks it on a frozen
    // module — including the direct-`Mir`-ctor path no builder owns. Without it
    // a clone that dropped the
    // fall-through edge from a ≥2-label goto still satisfies the opcode row's
    // `[min, ∞)` range and the edge disappears with NO diagnostic.
    //
    // ⚠ AN INNER VECTOR MAY LEGITIMATELY BE EMPTY, and that is an honoured
    // absence rather than a drop: a dialect whose `templateLabelPlaceholder` is
    // declared `null` mints no label spelling at all. The OUTER size still
    // counts the labels, so the invariant above holds for such a dialect too.
    std::vector<std::vector<std::string>> labelSpellings;

    // Register names the block DESTROYS. Names only — `"memory"` and `"cc"` are
    // the two clobber spellings that name no register and are hoisted into the
    // two flags below at parse time, so this list is uniformly resolvable
    // against the target's register table.
    std::vector<std::string> clobbers;
    // The `"memory"` clobber. ⚠ NOT the same as the opcode's
    // `opcodeClobbersMemory` membership: the opcode is ALWAYS a memory barrier
    // (a template is opaque text — the compiler cannot prove it touches no
    // memory), while this records that the SOURCE said so. §2a: protection must
    // key off the parsed clobber LIST being non-empty, never off "is this the
    // extended form".
    bool clobbersMemory = false;
    // The `"cc"` clobber — the block destroys the condition flags.
    bool clobbersConditionCodes = false;
};

class DSS_EXPORT MirAsmDescriptorPool {
public:
    // Append a descriptor; returns its index (the instruction payload). No dedup
    // — two `asm` statements that happen to be textually identical are still two
    // statements, and preserving identity is what makes a rebuild's
    // index-by-index re-add provably lossless. `MirLiteralPool::add`'s rule.
    [[nodiscard]] std::uint32_t add(MirAsmDescriptor d);

    [[nodiscard]] MirAsmDescriptor const& at(std::uint32_t index) const;
    [[nodiscard]] std::size_t             size()  const noexcept { return pool_.size(); }
    [[nodiscard]] bool                    empty() const noexcept { return pool_.empty(); }

private:
    std::vector<MirAsmDescriptor> pool_;
};

} // namespace dss
