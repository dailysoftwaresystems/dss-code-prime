#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_reg.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

// LIR node PODs (plan 12 §2.1 file tree → `lir_inst.hpp` + per-block /
// per-function shape). Each POD ≤ 32 bytes for cache density; the
// arena container assumes trivial-copyability.

namespace dss {

// `TargetCondCode` lives next to `TargetResultRule` / `TargetRegClass`
// in `core/types/target_schema.hpp` — same target-blind universal-
// vocabulary tier (compile-time enum, not JSON-driven). LIR pulls it
// in via target_schema.hpp's include.

// FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): `LirInst.flags` bit 0 is the
// operation-WIDTH discriminator the encoding-variant guards match on.
//   0 (default) = 64-bit — every pre-FC3 instruction and all
//                 hand-built test LIR (back-compat by construction).
//   1           = 32-bit — set by MIR→LIR lowering when the MIR
//                 instruction's type is I32/U32 (ALU/div/compare/
//                 trunc tiers; plumbing movs/loads/stores stay 64).
// The width rides `flags` (not a new POD field) deliberately: every
// existing flags-copying rewrite pass (lir_2addr_legalize, the
// regalloc rewriter, lir_callconv materialize) and the `.dsslir`
// text round-trip ("payload=N flags=M" is mandatory-emitted) carry
// it forward with ZERO changes — a width dropped at any rebuild
// would silently re-select the 64-bit forms (a miscompile), so the
// carrier that survives by construction is the safe one.
// kLirInstFlagWidth8 (D-CSUBSET-CHAR-STRING-VALUE-CODEGEN) adds the
// sub-native byte form (`char` ≡ I8): movsx/movzx r/m8, mov r8, sxtb,
// ldrb/strb. kLirInstFlagWidth16 (D-LIR-INT-MEMORY-WIDTH-EXACT) adds the
// half-word memory form (I16/U16): x86 0x66-prefixed mov / movzx r16,
// arm64 STURH/LDURH. The width bits are mutually exclusive (narrowest
// wins); the JSON loader accepts a guard width of 8, 16, 32, 64, or 128.
inline constexpr std::uint8_t kLirInstFlagWidth32 = 0x01;
inline constexpr std::uint8_t kLirInstFlagWidth8  = 0x02;
inline constexpr std::uint8_t kLirInstFlagWidth16 = 0x04;

// ── EARLY-CLOBBER RESULT (GNU inline asm's `&`, e.g. `"=&r"`) ────────
//
// "This instruction WRITES its result before it has finished READING its
// inputs, so the result must not share a register with any of them."
//
// ⚠⚠ WITHOUT IT THE DEFAULT IS SHARING, AND THE SHARING IS DELIBERATE.
// ✔MEASURED: for a SINGLE-instruction block at position P the inputs are used
// at the early slot (range ends P+1) and the result is defined at the late slot
// (range starts P+1); `expireActive` frees a range when `end <= currentStart`,
// so the input's register is returned to the free list and — the list being
// LIFO — handed straight back to the result. gcc and clang do exactly the same
// thing: a plain `"=r"` with one input assigns `%0` and `%1` the SAME register
// on x86_64 (`%eax`/`%eax`) and on aarch64 (`x0`/`x0`). That is correct for an
// ordinary instruction, which consumes its operands before it defines anything.
// A template that writes `%0` before reading `%1` breaks the assumption, and
// `&` is how the author says so. Accepting `&` and ignoring it is therefore not
// a missing optimisation — it is a value silently destroyed before it is read.
//
// ★★ THE CARRIER IS `flags` BECAUSE `flags` SURVIVES A REBUILD BY CONSTRUCTION.
// ✔MEASURED 2026-08-14: every rebuilding pass already threads `flags` through
// `addInst(op, res, ops, payload, flags)` — all EIGHT sites across
// `lir_2addr_legalize`, `lir_callconv`, `lir_rewrite`, `lir_wide_call_args` and
// the `.dsslir` reader — and the text round-trip emits `flags=M` unconditionally
// as a raw value, so an unknown bit is neither dropped nor rejected. The
// per-instruction constraint POOL explicitly does NOT have that property (see
// `detail::LirInst::regConstraints` below), which is why an earlyclobber bit
// does not ride there: a dropped `&` reads as the perfectly legal "no
// earlyclobber" and the allocator re-shares the register.
//
// ★ WHERE IT IS CONSUMED, AND WHY THAT IS THE ONLY PLACE: `lir_liveness.cpp`
// records the def at the instruction's EARLY slot instead of its late slot.
// The result's live range then OVERLAPS the slot at which every input is read,
// so no input expires before the result is allocated and the linear scan cannot
// hand the result an input's register — through the ordinary machinery, with no
// second exclusion list to keep in sync. ⚠ The discriminating variable is EARLY
// SLOT vs LATE SLOT, *not* which instruction of a multi-instruction expansion
// carries the def: for the single-instruction case the def is already on the
// first instruction, so "move the def to the first instruction" is a NO-OP (a
// previous handoff recorded that as the fix and it was measured wrong). The
// multi-instruction case is already safe, because a later instruction's late
// slot is past every input's use.
//
// ⚠ THE PRODUCER IS NOT WIRED YET AND THAT IS THE REMAINING HALF OF THE BUG.
// ✔MEASURED 2026-08-15: the front end already parses `&` into
// `AsmOperandConstraint::earlyClobber` (`hir/hir_inline_asm.hpp`) and MIR
// already carries it as `AsmOperandDescriptor::isEarlyClobber`
// (`mir/mir_asm_descriptor.hpp`), and NEITHER has a consumer anywhere — grep
// for both names finds only their own declarations. The MIR→LIR asm expansion
// must set this bit on the emitted instruction from that descriptor; until it
// does, `&` is still parsed, carried, and dropped.
inline constexpr std::uint8_t kLirInstFlagEarlyClobberResult = 0x08;

// ── THE MEMORY REFERENCE IS THE DESTINATION-POSITION OPERAND ─────────
// D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE.
//
// "The memory reference this instruction names occupies its DESTINATION
// position." For an instruction that writes, that is the operand it
// writes; for a flag-setter it is the left-hand side of the operation.
// Both are the same statement — which operand the destination position
// names — and it is the one fact an operand list cannot carry.
//
// ★★★ WHY A FLAG AND NOT AN OPERAND SHAPE. ✔MEASURED by reading
// `AsmInstructionLowering::buildLirInst`: `cmpq %r14, 4096(%r15)`
// (memory destination, shape 3) and `cmpq 4096(%r15), %r14` (register
// destination with a memory source, shape 2) build the BYTE-IDENTICAL
// operand list `[reg, reg, MemBase, MemOffset]` while meaning
// `mem - reg` (0x39 /r) and `reg - mem` (0x3B /r). Every other
// arithmetic mnemonic escapes the collision because it PRODUCES a
// value, so its two directions live on a producer and a consumer
// elected from disjoint candidate sets; `cmp` produces nothing in
// either direction and has no such split. Reordering the list is not
// available either: shape 3's `sources + address tail` is exactly the
// order the C front end's `store` already emits, on both shipped
// targets.
//
// ★★ WHY IT RIDES `flags` AND NOT THE DIALECT. Two tiers ask "can this
// opcode encode this shape?" through ONE function (`asm_variant_elect.hpp`,
// operator ruling) — the text lowering to pick the opcode, the encoder to
// pick the variant it emits — and they must agree byte-for-byte. An
// election axis the ENCODER cannot see would let the two disagree with no
// diagnostic. The dialect is likewise the wrong home: `assembly_config.hpp`
// records that a dialect must say which opcodes are POSSIBLE and never
// which one wins for a shape, on pain of becoming a drifting second copy
// of the target's guard table. `flags` is the carrier that survives every
// rebuilding pass by construction (see the width bits above).
//
// ⚠ ABSENCE OF A `memoryDestination` KEY ON A GUARD MEANS "DOES NOT
// DISCRIMINATE", so every pre-existing variant is unaffected — including
// `store`, which is reached through shape 3 on a destination-LAST dialect
// (AT&T `movq %rax,(%rdi)`) and through shape 2 on a destination-FIRST one
// (arm64 `str x1,[sp,#24]`) and must stay electable from both.
inline constexpr std::uint8_t kLirInstFlagMemoryIsDestination = 0x10;

// ── THIS CALL'S OUTGOING STACK ARGUMENTS HAVE ALREADY BEEN PLACED ────────────
//
// D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES.
//
// "Every argument of this Call that goes in the outgoing-argument area has
// already been assigned its byte offset, and no later pass may assign one."
//
// ★★★ WHY THE BIT EXISTS AT ALL, AND WHY ITS ABSENCE WAS A SILENT MISCOMPILE.
// Two passes walk a Call's argument list and both know how to lay out the
// overflow area: `lowerWideCallArgs` (pre-regalloc, which REMOVES each stacked
// scalar into a `store_outgoing_arg` carrier) and `lir_callconv` (post-regalloc,
// which places whatever is still on the Call). After the first pass runs, the
// second one walks a SHORTER list — so its cursor restarts from bytes the first
// pass already handed out, and a stacked scalar followed by a stacked aggregate
// received the SAME bytes twice with no diagnostic. The repair is that ONE pass
// owns the placement; this bit is how the second pass KNOWS the first one ran,
// so "callconv places nothing here" is CHECKED rather than argued from the
// operand list's shape.
//
// ⚠ IT IS CONSUMED, NOT FORWARDED. `materializeCallingConvention` re-emits the
// call with its own flags (0), so the bit's whole lifetime is
// `lowerWideCallArgs` → `materializeCallingConvention` — exactly the lifetime of
// the placement it describes. It never reaches the assembler and no encoding
// variant can match on it.
inline constexpr std::uint8_t kLirInstFlagOutgoingArgsPlaced = 0x20;

// ── THE 128-BIT OPERATION WIDTH ─────────────────────────────────────────────
//
// *** THIS BIT EXISTS SO THE WIDTH FIELD CAN DESCRIBE A REGISTER WIDER THAN A
// GENERAL-PURPOSE ONE, and its absence was an LIR EXPRESSIVENESS DEFECT rather
// than a missing optimization. Before it, `lirInstWidthBits` was a three-flag
// field over {8, 16, 32, 64} and 128 was UNSAYABLE -- so no instruction could
// state that it operates on a full 128-bit vector register, no target could
// declare an encoding variant guarded on that width, and every consumer that
// compares an operation width against a REGISTER width (x86-64 `xmm` is 16
// bytes) was structurally unable to see a match.
//
// It was FOUND as a 121-instruction residue in the OPT8 peephole (a full-width
// `movaps %xmm12,%xmm12` self-copy that the peephole could not prove was a
// no-op), and it is fixed HERE rather than worked around there, because the
// same limit blocks every future vector operation and not just those 121
// copies. The flags byte had two spare bits; this takes one.
//
// *** IT IS DECLARED, NOT STAMPED. No shipped lowering sets it yet: the FPR
// class moves on both shipped targets (`movaps`, `fmov`) declare encoding
// variants with NO width guard, so they elect correctly at any width and have
// no reason to state one. The day a real 128-bit operation needs the width to
// elect its variant, it says so and every width-comparing consumer -- the
// peephole included -- starts agreeing with no edit, because they all compare
// DECLARED vocabulary rather than special-casing a class.
inline constexpr std::uint8_t kLirInstFlagWidth128 = 0x40;

// The instruction's operation width in bits, derived from its flags.
[[nodiscard]] constexpr std::uint8_t
lirInstWidthBits(std::uint8_t flags) noexcept {
    if ((flags & kLirInstFlagWidth8) != 0)   return std::uint8_t{8};
    if ((flags & kLirInstFlagWidth16) != 0)  return std::uint8_t{16};
    if ((flags & kLirInstFlagWidth128) != 0) return std::uint8_t{128};
    return (flags & kLirInstFlagWidth32) != 0 ? std::uint8_t{32}
                                              : std::uint8_t{64};
}

// True iff this instruction's result must not share a register with any of its
// inputs. Named rather than open-coded so the one consumer and every future
// producer agree on the bit.
[[nodiscard]] constexpr bool
lirInstResultIsEarlyClobber(std::uint8_t flags) noexcept {
    return (flags & kLirInstFlagEarlyClobberResult) != 0;
}

// True iff this instruction's memory reference occupies its
// destination-position operand. Named rather than open-coded so the
// producer (the assembly-text lowering's memory-destination shape) and the
// consumer (the encoding-variant matcher) agree on the bit.
[[nodiscard]] constexpr bool
lirInstMemoryIsDestination(std::uint8_t flags) noexcept {
    return (flags & kLirInstFlagMemoryIsDestination) != 0;
}

// True iff a Call's outgoing stack arguments were already placed by
// `lowerWideCallArgs`. Named rather than open-coded so the one producer and the
// one consumer agree on the bit
// (D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES).
[[nodiscard]] constexpr bool
lirCallOutgoingArgsArePlaced(std::uint8_t flags) noexcept {
    return (flags & kLirInstFlagOutgoingArgsPlaced) != 0;
}

// Operand variant tag carried on each entry of the LIR operand pool.
// LIR operands are heterogeneous (registers / immediates / memory
// addressing modes); the kind discriminator picks the right field
// to read from the pool slot.
enum class LirOperandKind : std::uint8_t {
    None       = 0,
    Reg        = 1,  // virtual or physical register (pre/post regalloc)
    ImmInt     = 2,  // sign-extended int64 immediate
    // Slot 3 reserved — was `ImmFloat` (never minted, no union arm, no
    // payload field). Wide floats flow through the literal pool via
    // `LiteralIndex` (cycle 3c). Leaving the slot reserved keeps the
    // numeric values stable for callers that already serialized the
    // enum (none currently exist, but the enum value space is part of
    // the substrate contract).
    BlockRef   = 4,  // basic-block reference (br targets, etc.)
    SymbolRef  = 5,  // symbol reference (call targets, GlobalAddr)
    // Memory addressing modes (base + index + scale + displacement)
    // are spread across THREE pool entries to keep each pool slot a
    // uniform 8 bytes: a leading `Reg` operand for the base, then a
    // `MemBase` (the scale field), then a `MemOffset` (the displacement).
    // Cycle 3d's 4-operand `lea` arm adds an optional `Reg` index
    // operand between base and MemBase. The builder emits these as a
    // paired construct (cycle 3c lowerLoad/lowerStore/lowerGep);
    // `LirVerifier` will check the pairing once ML6 starts consuming
    // memory addresses.
    MemBase    = 6,
    MemOffset  = 7,
    // Pool index for wide literals (int64/double/string/aggregate). The
    // 8-byte operand POD cannot inline a 64-bit immediate without
    // breaking the cache-density invariant, so wide literals flow
    // through `LirLiteralPool` (cycle 3c). `litIndex` indexes into the
    // owning module's pool; consumers fetch via `lir.literalValue(idx)`.
    LiteralIndex = 8,
    // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): a by-value-
    // aggregate stack-arg MARKER on a Call's operand list. It carries the
    // aggregate's BYTE SIZE (`byValueAggBytes`) and ALWAYS immediately
    // FOLLOWS the `Reg` operand holding the aggregate's temp address —
    // mirroring the leading-Reg + trailing-descriptor convention the
    // memory addressing modes use (Reg + MemBase + MemOffset). The
    // preceding Reg is a normal register use (liveness/regalloc track it,
    // keeping the temp address live to the callconv pass); this marker is
    // invisible to liveness/regalloc (not a Reg) and tells lir_callconv to
    // pass that aggregate ENTIRELY in the outgoing overflow area by a
    // byte-wise copy (ceil(size / outgoingSlot) slots), never in a register
    // and never split (SysV §3.2.3/§3.5.7). CC-neutral: the size is the
    // only datum.
    //
    // ★★ THE CARRIER IS A TRIPLE ONCE IT HAS BEEN PLACED
    // (D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES):
    //     Reg (temp address) , ByValueStackAgg (size + exhaust) , MemOffset
    // where the trailing `MemOffset` states the byte offset WITHIN the
    // outgoing-argument area that this aggregate occupies. `MirToLir` emits the
    // PAIR, because that tier has no calling convention in scope and so cannot
    // place anything; `lowerWideCallArgs` — the one pass that walks the call's
    // COMPLETE argument list — appends the third operand. A carrier that reaches
    // `lir_callconv` on a Call whose `kLirInstFlagOutgoingArgsPlaced` is set and
    // that states no offset is REFUSED, never re-placed: re-placing it from a
    // second cursor, over a list the first pass had already shortened, is the
    // silent miscompile this triple exists to make unrepresentable.
    //
    // ⚠ The trailing `MemOffset` is NOT an addressing mode. It is the same
    // "spread a wide descriptor across uniform 8-byte pool slots" convention the
    // memory addressing modes use (Reg + MemBase + MemOffset), and the verifier's
    // load/store/lea address-form rule does not apply to a Call.
    ByValueStackAgg = 9,
    // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): a POST-REGALLOC operand naming a
    // SPILLED value by its stack slot + register class, NOT a virtual register.
    // Emitted by `rewriteWithAllocation` in place of a scratch-reload for a
    // spilled register-passed CALL ARG so `materializeCallingConvention` loads
    // that arg DIRECTLY into its ABI argument register (`frame_load argReg,
    // [slot]`) — instead of reload-into-scratch (rewriter) then move-scratch-
    // into-argReg (callconv). There is exactly one arg register per register-
    // passed arg ⇒ demand == supply, so call-arg reloads need ZERO rewriter
    // scratch, closing the func-2088 exhaustion (≥4 spilled GPR args outran the
    // 3 non-arg caller-saved GPR scratch). It is NOT a `Reg`, so
    // `verifyLirPostRegalloc` (which checks only Reg-kind operands for
    // physical-ness) accepts it; callconv CONSUMES it before final golden-text
    // emission, so it never reaches the assembler. Carries `spillSlotV` (the
    // LirSpillSlot.v) + `spillSlotClass` (the value's LirRegClass, so callconv
    // picks the class-correct load).
    SpillSlotRef = 10,
};

// One slot in the operand pool. The tag picks the active field.
struct LirOperand {
    LirOperandKind kind = LirOperandKind::None;  // 1
    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: for a ByValueStackAgg
    // marker, which arg-register CLASS the placement EXHAUSTS once this aggregate is
    // stacked (0 = none/BACKFILL — SysV; 1 = GPR; 2 = FPR — AAPCS64 §B). lir_callconv
    // clamps the matching class cursor so a SUBSEQUENT arg/vararg of that class also
    // goes to memory (matching the callee's va_start clamp). Unused (0) for any other
    // operand kind. Repurposes one padding byte — no struct-size change.
    std::uint8_t   byValueAggExhaust = 0;        // 1
    // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): for a SpillSlotRef operand, the
    // value's LirRegClass (as a uint8 — GPR/FPR/...) so callconv resolves the
    // class-correct load op. Repurposes one of the two padding bytes — no
    // struct-size change (still 8). Unused (0) for any other operand kind.
    std::uint8_t   spillSlotClass = 0;           // 1
    // D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: for a `Reg` (or
    // `SpillSlotRef`) operand sitting in a CALL's argument region, the argument's
    // NATURAL BYTE SIZE (1/2/4/8). 0 = "not stated", which every producer other
    // than `MirToLir::lowerCall` leaves it at and which every `Slot`-packing CC
    // ignores. Consumed by `lir_wide_call_args`, whose overflow cursor needs the
    // datum's own size + alignment once a CC declares NATURAL packing.
    //
    // ★★ WHY THE OPERAND AND NOT THE INSTRUCTION. The width bits ride
    // `LirInst::flags`, which is exactly right for a one-datum instruction (the
    // callee's `arg` uses them); a CALL carries N arguments of N different sizes
    // in ONE instruction, so its width axis is necessarily per-OPERAND. This
    // repurposes the last padding byte — `LirOperand` stays 8 bytes (asserted
    // below), the same trick `byValueAggExhaust` and `spillSlotClass` already use.
    //
    // ⚠ IT IS NOT ROUND-TRIPPED THROUGH `.dsslir` TEXT — deliberately, and it
    // shares that property with the two side bytes above (see `lir_text.cpp`).
    // Its whole lifetime is isel → `lowerWideCallArgs`, which run back-to-back in
    // `compile_pipeline` with only verbatim operand copies in between. A CC that
    // declares NATURAL packing and reaches the overflow cursor with 0 here is a
    // DROPPED CARRIER, and `lowerWideCallArgs` REFUSES it loudly rather than
    // silently reverting that one argument to slot packing while the callee reads
    // it packed — which is the shape of a silent miscompile.
    std::uint8_t   argNaturalBytes = 0;          // 1
    union {
        LirReg        reg;        // 4 — kind == Reg
        std::int32_t  immInt32;   // 4 — kind == ImmInt (truncated; full int64 lives in scalar pool)
        std::uint32_t blockSlot;  // 4 — kind == BlockRef → LirBlockId.v
        std::uint32_t symbolV;    // 4 — kind == SymbolRef → SymbolId.v
        std::uint32_t scale;      // 4 — kind == MemBase (1/2/4/8)
        std::int32_t  offset;     // 4 — kind == MemOffset
        std::uint32_t litIndex;   // 4 — kind == LiteralIndex (into LirLiteralPool)
        std::uint32_t byValueAggBytes; // 4 — kind == ByValueStackAgg (aggregate byte size)
        std::uint32_t spillSlotV; // 4 — kind == SpillSlotRef (LirSpillSlot.v)
    };

    constexpr LirOperand() noexcept : kind(LirOperandKind::None), reg{} {}

    // ── constexpr factories ────────────────────────────────────────
    //
    // Every operand-building call site (MIR→LIR isel cycles 3a-d, ARM64
    // isel, AS1 assembler, golden-text emitter) ends up filling these
    // exact union arms. Centralised so the discriminator + arm always
    // stay in sync — accidentally setting `kind=Reg` while writing
    // `immInt32` would otherwise be a quiet aliasing bug.
    [[nodiscard]] static constexpr LirOperand makeReg(LirReg r) noexcept {
        LirOperand o{};
        o.kind = LirOperandKind::Reg;
        o.reg  = r;
        return o;
    }
    // D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: a `Reg` operand in a
    // CALL's argument region, carrying the argument's natural byte size so the
    // overflow cursor can pack it. Every OTHER Reg operand keeps `makeReg`, whose
    // `argNaturalBytes` is 0 = "not stated" — so this is additive by construction.
    [[nodiscard]] static constexpr LirOperand
    makeArgReg(LirReg r, std::uint8_t naturalBytes) noexcept {
        LirOperand o{};
        o.kind            = LirOperandKind::Reg;
        o.argNaturalBytes = naturalBytes;
        o.reg             = r;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeImmInt32(std::int32_t v) noexcept {
        LirOperand o{};
        o.kind     = LirOperandKind::ImmInt;
        o.immInt32 = v;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeBlockRef(std::uint32_t v) noexcept {
        LirOperand o{};
        o.kind      = LirOperandKind::BlockRef;
        o.blockSlot = v;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeSymbolRef(std::uint32_t v) noexcept {
        LirOperand o{};
        o.kind     = LirOperandKind::SymbolRef;
        o.symbolV  = v;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeMemBase(std::uint32_t scale) noexcept {
        LirOperand o{};
        o.kind  = LirOperandKind::MemBase;
        o.scale = scale;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeMemOffset(std::int32_t offset) noexcept {
        LirOperand o{};
        o.kind   = LirOperandKind::MemOffset;
        o.offset = offset;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeLiteralIndex(std::uint32_t idx) noexcept {
        LirOperand o{};
        o.kind     = LirOperandKind::LiteralIndex;
        o.litIndex = idx;
        return o;
    }
    // FC12a-struct: the by-value-aggregate stack-arg size marker. ALWAYS emitted
    // immediately AFTER the Reg operand holding the aggregate's temp address.
    // `exhaust` (D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS): the arg-register
    // class lir_callconv exhausts after placing this stacked aggregate (0 none/backfill,
    // 1 GPR, 2 FPR) — SysV passes 0, AAPCS64 passes the straddling aggregate's class.
    [[nodiscard]] static constexpr LirOperand
    makeByValueStackAgg(std::uint32_t bytes, std::uint8_t exhaust = 0) noexcept {
        LirOperand o{};
        o.kind             = LirOperandKind::ByValueStackAgg;
        o.byValueAggExhaust = exhaust;
        o.byValueAggBytes  = bytes;
        return o;
    }
    // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): a spilled register-passed call-arg
    // reference — its stack slot (`slotV` = LirSpillSlot.v) + its register class
    // (`cls`, stored as a uint8). Emitted by the rewriter, consumed by callconv's
    // arg-setup (turned into a `frame_load argReg, [slot]` sequenced within the
    // parallel-move machinery). See LirOperandKind::SpillSlotRef.
    [[nodiscard]] static constexpr LirOperand
    makeSpillSlotRef(std::uint32_t slotV, std::uint8_t cls) noexcept {
        LirOperand o{};
        o.kind           = LirOperandKind::SpillSlotRef;
        o.spillSlotClass = cls;
        o.spillSlotV     = slotV;
        return o;
    }
};
static_assert(sizeof(LirOperand) == 8, "LirOperand POD must stay 8 bytes");
static_assert(std::is_trivially_copyable_v<LirOperand>);

// ── THE BY-VALUE STACKED-AGGREGATE CARRIER, AS ONE SHAPE INSTEAD OF FOUR ─────
//
// D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES.
//
// FOUR walks over a Call's argument list have to agree on where a carrier
// begins, which of its operands are DESCRIPTIVE (consumed with the carrier and
// never an argument position of their own), and where it was placed:
// `lir_wide_call_args::lowerOneFunc`, `lir_callconv::computeMaxOutgoingStackArgs`,
// `lir_callconv::materializeOneFunc`'s call arm, and
// `lir_rewrite::classifyCallRegArgs`. Each of them used to spell the lookahead
// out by hand — the same "a promise four copies keep" shape that
// `ArgCursors` and `StackArgCursor` were minted to end on the register and byte
// axes. So the shape is stated ONCE, here, beside the operand kind it is about.

// True iff `ops[k]` is the Reg that CARRIES a by-value stacked aggregate — i.e.
// it is immediately followed by the `ByValueStackAgg` marker describing it.
[[nodiscard]] constexpr bool
lirIsByValueStackAggCarrier(std::span<LirOperand const> ops,
                            std::size_t k) noexcept {
    return k < ops.size() && ops[k].kind == LirOperandKind::Reg
        && (k + 1) < ops.size()
        && ops[k + 1].kind == LirOperandKind::ByValueStackAgg;
}

// True iff `ops[k]` DESCRIBES the carrier before it rather than being an
// argument of its own: the `ByValueStackAgg` marker, or the `MemOffset` that
// states where that marker's aggregate was placed. A walk skips these without
// advancing its argument-position counter.
[[nodiscard]] constexpr bool
lirIsByValueStackAggDescriptor(std::span<LirOperand const> ops,
                               std::size_t k) noexcept {
    if (k >= ops.size()) return false;
    if (ops[k].kind == LirOperandKind::ByValueStackAgg) return true;
    return ops[k].kind == LirOperandKind::MemOffset && k > 0
        && ops[k - 1].kind == LirOperandKind::ByValueStackAgg;
}

// The byte offset, within the outgoing-argument area, that the carrier at `k`
// was placed at — or `nullopt` when it has not been placed. Precondition:
// `lirIsByValueStackAggCarrier(ops, k)`.
[[nodiscard]] constexpr std::optional<std::int32_t>
lirByValueStackAggPlacedOffset(std::span<LirOperand const> ops,
                               std::size_t k) noexcept {
    if ((k + 2) < ops.size() && ops[k + 2].kind == LirOperandKind::MemOffset)
        return ops[k + 2].offset;
    return std::nullopt;
}

// How many operands the carrier at `k` occupies: 2 unplaced, 3 placed.
// Precondition: `lirIsByValueStackAggCarrier(ops, k)`.
[[nodiscard]] constexpr std::size_t
lirByValueStackAggCarrierOperands(std::span<LirOperand const> ops,
                                  std::size_t k) noexcept {
    return lirByValueStackAggPlacedOffset(ops, k).has_value() ? 3u : 2u;
}

// ── per-INSTRUCTION register-constraint pool ─────────────────────
//
// D-LIR-PER-INST-REG-CONSTRAINTS. Fixed-register semantics ("this
// instruction destroys rcx and needs its shift count
// in cl") existed ONLY per-OPCODE, on `TargetOpcodeInfo::
// implicitRegisters` — a field the target JSON owns and the loader
// populates. An inline-asm statement declares the same contract
// per-INSTRUCTION: two `asm` statements sharing one opcode declare
// different clobbers, so the opcode row cannot express it.
//
// ★★ WHY THIS IS A SECOND CARRIER FOR THE **SAME STRUCT** RATHER THAN
// A SECOND WRITER OF THE OPCODE FIELD. `implicitRegisters` is
// config-owned: one field with two writers (the JSON loader AND a
// lowering) is the shape where a target's declared contract and a
// statement's declared contract silently overwrite each other, and
// which one wins depends on load order. So the TYPE is reused
// verbatim (`ImplicitRegisterConstraint` — same three semantically
// distinct sets, same role projection, same validator vocabulary) and
// only the CARRIER is new. A consumer that wants the full contract
// for an instruction reads the opcode's set AND this one; neither
// mutates the other.
//
// ★★ WHY A MODULE-LEVEL POOL RATHER THAN AN INLINE FIELD. The
// constraint is SEVEN std::vectors — already several times the 32-byte
// cache-density budget `detail::LirInst` is built around
// (`static_assert(sizeof(LirInst) <= 32)`) before a single element is
// stored, and UNBOUNDED after (each vector owns heap, and the strings
// inside own more). So the instruction stores
// a HANDLE and the value lives here, exactly the discipline
// `LirLiteralPool` already uses for wide literals: by-index, no
// dedup, identity-preserving. Two side structures with one shape is
// one thing to remember; `lir_pass_util::copyModuleSideStructures`
// carries both across a rebuild in ONE call for exactly that reason.
//
// ⚠ AN OPERAND KIND WAS CONSIDERED AND REJECTED. Appending a
// clobber-carrying operand breaks `operandsMatchGuard`'s positional
// kind equality (`src/asm/format/walker_util.hpp`) and the verifier's
// "last two operands are MemBase then MemOffset" invariant, and a
// clobber-only operand structurally cannot express a fixed-register
// INPUT (an input has to be a *use* the allocator sees, in a
// *position* the encoder does not).
class DSS_EXPORT LirRegConstraintPool {
public:
    // Append a constraint set; returns its INDEX (0-origin), which the
    // instruction stores as a 1-based HANDLE — see
    // `lirRegConstraintHandleForIndex` below. No dedup: two `asm`
    // statements that happen to clobber the same registers are still
    // two statements, and preserving identity is what makes a rebuild's
    // index-by-index copy provably lossless. Same discipline as
    // `LirLiteralPool::add`.
    [[nodiscard]] std::uint32_t add(ImplicitRegisterConstraint c);

    // Out-of-range is a producer bug, not a recoverable state: it means
    // a handle outlived the pool it indexed (the exact silent-miscompile
    // this pool's verifier rule exists to catch). Aborts, mirroring
    // `LirLiteralPool::at`. Callers that must not abort ask
    // `Lir::instRegConstraints`, which returns `nullptr` for "none".
    // ⚠ DO NOT ASSUME THE VERIFIER SAW THE MODULE FIRST. ✔MEASURED
    // 2026-08-14: `lir_verifier.hpp` is included by exactly ONE
    // production TU (`lir_text.cpp`, the `.dsslir` reader) and by five
    // test TUs — `verifyLir` and `verifyLirPostRegalloc` have ZERO
    // production call sites, and `compile_pipeline.cpp` runs the MIR
    // verifier only. So on the real compile path this abort IS the
    // first line of defence, not the second.
    [[nodiscard]] ImplicitRegisterConstraint const& at(std::uint32_t index) const;

    [[nodiscard]] std::size_t size()  const noexcept { return pool_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return pool_.empty(); }

private:
    std::vector<ImplicitRegisterConstraint> pool_;
};

// The "this instruction declares no per-instruction constraints"
// handle. ★ It is 0 SO THAT EVERY INSTRUCTION EVER BUILT — including
// the hand-built LIR in ~40 test files and every `detail::LirInst`
// that predates this field — means "none" by construction, exactly
// the back-compat argument `kLirInstFlagWidth32` records above. The
// handle is therefore 1-based and `at()` is 0-based; the two
// converters below are the ONLY places that arithmetic appears.
inline constexpr std::uint32_t kLirNoRegConstraints = 0;

[[nodiscard]] constexpr std::uint32_t
lirRegConstraintHandleForIndex(std::uint32_t poolIndex) noexcept {
    return poolIndex + 1;
}
// Precondition: `handle != kLirNoRegConstraints` (callers test first —
// the "none" state is not an index and has no pool slot).
[[nodiscard]] constexpr std::uint32_t
lirRegConstraintIndexForHandle(std::uint32_t handle) noexcept {
    return handle - 1;
}

namespace detail {

// ── instruction POD ──────────────────────────────────────────────
//
// Cache-density 32 bytes. The `opcode` field is a `std::uint16_t`
// whose meaning is defined entirely by the `TargetSchema` the module
// was built with (cycle 2 pivot: opcodes live in JSON, not C++ enums).
// Consumers look up opcode semantics via `schema.opcodeInfo(opcode)`,
// never via a C++ enum. Operands live in the module's operand pool,
// addressed by `[operandStart, operandStart + operandCount)`. The
// optional result register is in `result`; whether it's meaningful
// is per-opcode (TargetResultRule::Value / Optional).
struct LirInst {
    std::uint16_t opcode       = 0;          // 2 — Invalid sentinel = 0
    std::uint8_t  flags        = 0;          // 1
    std::uint8_t  _pad         = 0;          // 1
    LirReg        result       = InvalidLirReg;  // 4 — result reg, if any
    std::uint32_t operandStart = 0;          // 4 — into operand pool
    std::uint32_t operandCount = 0;          // 4
    std::uint32_t payload      = 0;          // 4 — per-opcode scalar (label id, etc.)
    // 4 — 1-based handle into the module's `LirRegConstraintPool`;
    // `kLirNoRegConstraints` (0) = this instruction declares none. Claims
    // the slot previously reserved as `_pad2` ("explicit pad to 24B;
    // future field") — the field it was reserved FOR.
    //
    // ⚠ UNLIKE `flags`, THIS DOES **NOT** SURVIVE A REBUILD BY
    // CONSTRUCTION, and pretending otherwise is how it would become a
    // silent miscompile. `flags` survives because every rebuilding pass
    // already threads it through `addInst(op, res, ops, payload, flags)`;
    // this handle rides the POD and `addInst` never sees it, so a pass
    // that rebuilds an instruction drops it to 0 — which reads as the
    // perfectly legal "no constraints" and lets the allocator reuse a
    // register the instruction destroys. A per-instruction field is
    // inherently uncarriable without the pass's participation: the
    // rebuild re-creates the instruction and only the pass knows the
    // correspondence. So the mechanism is (a) `lir_pass_util::
    // carryInstSideData` — ONE call per rebuilt instruction, the whole
    // participation surface — and (b) the verifier's `L_SideStructure
    // ReferenceLost` rule, which turns a forgotten (a) LOUD: the shared
    // copy helper carries the POOL unconditionally, so a dropped handle
    // leaves a pool entry that NO instruction references, and that is
    // detectable from the rebuilt module alone.
    std::uint32_t regConstraints = kLirNoRegConstraints;
};
static_assert(sizeof(LirInst) <= 32, "detail::LirInst grew unexpectedly");
static_assert(std::is_trivially_copyable_v<LirInst>);

// ── basic-block POD ──────────────────────────────────────────────
//
// Mirrors MirBlock: owns a contiguous instruction range + successor
// range, carries an owning function id.
struct LirBlock {
    std::uint32_t instStart = 0;             // 4 — into instruction arena
    std::uint32_t instCount = 0;             // 4 — includes terminator
    std::uint32_t succStart = 0;             // 4 — into succ pool
    std::uint32_t succCount = 0;             // 4
    std::uint32_t func      = 0;             // 4 — owning LirFuncId.v
    std::uint32_t _pad[3]   = {};            // 12 — explicit padding to 32B
};
static_assert(sizeof(LirBlock) <= 32, "detail::LirBlock grew unexpectedly");
static_assert(std::is_trivially_copyable_v<LirBlock>);

// ── function POD ─────────────────────────────────────────────────
struct LirFunc {
    std::uint32_t blockStart = 0;            // 4 — into block arena
    std::uint32_t blockCount = 0;            // 4
    std::uint32_t symbol     = 0;            // 4 — declared SymbolId.v
    std::uint32_t numVRegs   = 0;            // 4 — virtual-register count
    std::uint32_t _pad[4]    = {};           // 16 — explicit padding to 32B
};
static_assert(sizeof(LirFunc) <= 32, "detail::LirFunc grew unexpectedly");
static_assert(std::is_trivially_copyable_v<LirFunc>);

} // namespace detail

} // namespace dss

// ArenaNames specializations — one per arena, four entity names
// (attribute/element/tag/access). Pattern mirrors Mir's per-tier
// declarations in mir_node.hpp.
namespace dss::substrate {

template <>
struct ArenaNames<LirInstId, LirModuleId> {
    static constexpr char const* attribute = "LirAttribute";
    static constexpr char const* element   = "LirInstId";
    static constexpr char const* tag       = "LirModuleId";
    static constexpr char const* access    = "Lir::inst";
};

template <>
struct ArenaNames<LirBlockId, LirModuleId> {
    static constexpr char const* attribute = "LirBlockAttribute";
    static constexpr char const* element   = "LirBlockId";
    static constexpr char const* tag       = "LirModuleId";
    static constexpr char const* access    = "Lir::block";
};

template <>
struct ArenaNames<LirFuncId, LirModuleId> {
    static constexpr char const* attribute = "LirFuncAttribute";
    static constexpr char const* element   = "LirFuncId";
    static constexpr char const* tag       = "LirModuleId";
    static constexpr char const* access    = "Lir::func";
};

} // namespace dss::substrate
