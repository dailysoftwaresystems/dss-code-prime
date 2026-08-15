#pragma once

#include "core/export.hpp"

#include <cstdint>
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
    // `"+r"` — this operand is read AND written: one input tied to one output.
    bool           isReadWrite = false;
    // `"=&r"` — earlyclobber: the output is written before every input is read,
    // so it may not share a register with any of them.
    bool           isEarlyClobber = false;
};

struct DSS_EXPORT MirAsmDescriptor {
    // The assembly template, verbatim. Parsed by the dialect's own grammar at
    // expansion time (plan 29 §4.6: `%N` binding is STRUCTURAL) — never here.
    std::string templateText;
    // `__asm__ volatile` — the block may not be moved or elided even when its
    // outputs are unused. (Both asm opcodes are `hasSideEffects` +
    // `opcodeClobbersMemory` regardless, so this records the SOURCE's word; it
    // is not the optimizer's only defence.)
    bool isVolatile = false;
    // Outputs in SOURCE order. Output k is result piece k: the `ReturnPiece`
    // anchored to this instruction whose payload ordinal is k.
    std::vector<MirAsmOperand> outputs;
    // Inputs in SOURCE order, ALIGNED 1:1 with the instruction's MIR operands
    // (operand j is input j — an asm block has no callee operand, which is why
    // `InlineAsm` is `{0,N}` and not `Call`'s `{1,N}`).
    std::vector<MirAsmOperand> inputs;
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
