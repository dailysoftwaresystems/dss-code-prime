#pragma once

#include "core/substrate/arena_tag.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"           // SymbolBinding / SymbolVisibility
#include "core/types/type_lattice/type_id.hpp"   // TypeId
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <type_traits>

// MIR storage PODs (ML1). The MIR module is four dense arenas — instructions,
// basic blocks, functions, and module-level globals — all tagged by one
// `MirModuleId` (the cross-module guard), dogfooding the SP1 substrate exactly
// as HIR does. The PODs live in `detail/` so consumers go through `Mir`'s
// accessors; several fields are pool offsets that are meaningless in isolation.
//
// Topology (per plan 12 §2.2):
//   function  → a contiguous range of blocks  [blockStart, blockStart+blockCount)
//   block     → a contiguous range of insts   [instStart,  instStart +instCount)
//             + a contiguous range of CFG successor block-ids in the succ pool
//   instruction → operands as a range into the operand pool (or, for Phi, the
//                 phi pool of incoming value/block pairs)
// Contiguous arena ranges (func→blocks, block→insts) need no indirection pool —
// the arena slot ordering IS the membership order, so the builder appends a
// block's instructions back-to-back. The only genuinely non-contiguous lists
// (operands, phi incomings, successors) live in module-owned pools.

namespace dss {

// In the FUSED value model a value IS the instruction that defines it, so a
// "value reference" is just the defining instruction's id. The alias documents
// intent at call sites (operands, results) without a second arena or any
// conversion boilerplate.
using MirValueId = MirInstId;

// ── per-instruction markers ──────────────────────────────────────────────────
//
// `Synthetic` marks instructions lowering inserted with no source origin (e.g.
// structured-CF scaffolding). `Volatile` marks a memory access the optimizer
// must not reorder or elide. Also: two `Volatile` ops must never be reordered
// relative to each other, and any future instruction-scheduling / sinking pass
// MUST treat a `Volatile` op as a scheduling barrier w.r.t. every other
// `Volatile` or side-effecting op (today guaranteed structurally by the shared
// rebuild-walk's original-scan-order discipline; pin:
// TwoVolatileStoresToDifferentGlobalsKeepRelativeOrder). Multiple flags may apply.
enum class MirInstFlags : std::uint8_t {
    None      = 0,
    Synthetic = 1u << 0,
    Volatile  = 1u << 1,
    // FC17.9(c) (D-CSUBSET-SETJMP): marks a `Call` whose callee "returns more than
    // once" (C11 7.13.1.1 — `setjmp`/`_setjmp`). Set at HIR→MIR Call lowering from the
    // callee's `SymbolRecord.returnsTwice` (via a CST→HIR side-table, the `Volatile`-
    // from-`isVolatile` funnel mirror). Unlike `noreturn` (HIR-discharged into an
    // `Unreachable`, never reaching MIR) this flag is the CARRIER the MIR optimizer
    // passes read: a returns-twice `Call` is a frame-capture barrier — mem2reg must not
    // promote locals live across it and the inliner must not inline its callee. The
    // walking-skeleton lands the carrier (the flag reaches MIR + a red-on-disable pin);
    // the passes that CONSUME it are a follow-on.
    ReturnsTwice = 1u << 2,
    // FC17.9(d) cycle 1b (D-CSUBSET-ATOMIC): marks a plain `Store` that is an
    // object INITIALIZATION (C11 7.17.2.1 — the initialization of an atomic object
    // is NOT itself atomic), so the MIR verifier's atomic-lowering belt
    // (I_AtomicAccessNotLowered) SPARES it even when its target's pointee is
    // `_Atomic`-qualified. Set at exactly the two runtime-init store paths that are
    // deliberately left plain (the address-taken atomic-param reception store + the
    // atomic-global `__module_init__` store); every ordinary scalar `_Atomic`
    // access instead funnels to AtomicLoad/AtomicStore, so a plain atomic-pointee
    // Store WITHOUT this flag is a missed funnel site → fails LOUD. Fail-SAFE: a
    // future init path that forgets the flag trips the belt (a false-positive
    // caught at test time), never a silent non-atomic access. NOT read by any
    // optimizer pass (it is a verifier-only exemption marker) — distinct from any
    // atomic-OP marker (the atomic ops carry their own opcode identity, not a flag).
    AtomicInitExempt = 1u << 3,
    // bits 4-7 reserved
};

[[nodiscard]] inline constexpr MirInstFlags operator|(MirInstFlags a, MirInstFlags b) noexcept {
    using U = std::underlying_type_t<MirInstFlags>;
    return static_cast<MirInstFlags>(static_cast<U>(a) | static_cast<U>(b));
}
[[nodiscard]] inline constexpr MirInstFlags operator&(MirInstFlags a, MirInstFlags b) noexcept {
    using U = std::underlying_type_t<MirInstFlags>;
    return static_cast<MirInstFlags>(static_cast<U>(a) & static_cast<U>(b));
}
[[nodiscard]] inline constexpr bool any(MirInstFlags v) noexcept {
    return static_cast<std::underlying_type_t<MirInstFlags>>(v) != 0;
}
[[nodiscard]] inline constexpr bool has(MirInstFlags v, MirInstFlags bit) noexcept {
    return any(v & bit);
}

// ── structured-CF markers (block metadata) ───────────────────────────────────
//
// Each block carries the structural control-flow role it plays in the program
// (plan 12 §2.3). The marker is CANONICALLY DERIVED FROM THE CFG —
// `deriveStructCfMarkers` (mir/mir_struct_markers.hpp) computes it from
// dominators / post-dominators / natural loops, and every producer (HIR→MIR
// lowering, CFG-mutating optimizer passes, the cross-CU merge) re-stamps its
// output from that derivation after `finish()`; creation-time
// `createBlock(marker)` stamps are intent-documenting DEFAULTS the final
// stamping overwrites. The verifier checks stored == derived per reachable
// block (I_StructCfMismatch). A future WASM lowering (plan 18) still consumes
// the stored field directly — the derivation keeps it TRUSTWORTHY through
// arbitrary CFG transforms without a Relooper recovery pass at consume time.
// A marker is a per-block byte, never an instruction-stream entity (keeping
// the stream clean for the optimizer and instruction selection).
//
// ONE marker per block; the derivation's priority order (the spec in
// mir_struct_markers.hpp) IS the multi-role collision policy — e.g. a block
// that is both a back-edge target and an if-join derives LoopHeader (the
// higher-priority claim wins). If a real consumer ever needs multi-role
// visibility, this becomes a small bitset without changing the POD layout.
//
// DORMANT VALUES — NOT derived (and no producer emits them); they remain
// `mir_text` round-trip vocabulary only:
//   - ExitBlock:  no producer ever emitted it.
//   - LoopLatch:  not CFG-derivable (a while body-tail and a for-update block
//     can present identical CFG shapes); back-edge SOURCES are recoverable
//     from `mirNaturalLoops::backEdgeSources` when a consumer needs them.
//   - SwitchHead: never emitted; the discriminant block derives by the
//     lower-priority rules (usually Linear/EntryBlock).
enum class StructCfMarker : std::uint8_t {
    Linear,       // no structural role (straight-line code) — the default
    EntryBlock,   // function entry
    ExitBlock,    // DORMANT (see above) — function exit
    LoopHeader,   // loop entry; target of the back-edge; dominates the body
    LoopLatch,    // DORMANT (see above) — back-edge source
    LoopExit,     // target of a loop-exiting edge
    IfThen,       // then-arm of a conditional
    IfElse,       // else-arm of a conditional
    IfJoin,       // post-dominating merge of an if
    SwitchHead,   // DORMANT (see above) — the switch discriminant block
    SwitchCase,   // a case arm
    SwitchJoin,   // post-dominating merge of a switch
};

// One incoming edge of a Phi: the value flowing in along a given predecessor
// block. Stored in the module's phi pool; a Phi instruction's operand range
// addresses this pool instead of the general operand pool.
struct MirPhiIncoming {
    MirValueId value{};  // the value flowing in (a defining instruction id)
    MirBlockId pred{};   // the predecessor block this value arrives from

    constexpr bool operator==(MirPhiIncoming const&) const noexcept = default;
};
static_assert(std::is_trivially_copyable_v<MirPhiIncoming>);

// TLS C1 (D-CSUBSET-THREAD-LOCAL, code-audit LOW-1): the `addGlobal`
// PARAMETER type for a global's storage duration — a strong enum so
// thread-storage is un-transposable with the adjacent `isConst` bool and
// un-fillable by an integer (the historical pre-cycle 8-arg call shape
// `addGlobal(…, isConst, alignmentBytes)` would otherwise compile silently
// with alignmentBytes converting to isThreadLocal=true). The DivSlotPair
// type-level-invariant precedent. The `MirGlobal` FIELD stays `bool
// isThreadLocal` (POD layout untouched); `addGlobal` translates.
enum class MirThreadStorage : std::uint8_t {
    Shared,     // ordinary process-shared storage (the default duration)
    PerThread,  // C11/C23 6.2.4 thread storage duration → .tdata/.tbss
};

// The bool→enum bridge for PROPAGATION sites (merge / optimizer rebuilds /
// DCE), which read `Mir::globalIsThreadLocal(g)` and must re-state the
// value: `mirThreadStorageOf(m.globalIsThreadLocal(g))`.
[[nodiscard]] constexpr MirThreadStorage
mirThreadStorageOf(bool isThreadLocal) noexcept {
    return isThreadLocal ? MirThreadStorage::PerThread
                         : MirThreadStorage::Shared;
}

namespace detail {

// ── instruction POD ───────────────────────────────────────────────────────────
//
// Fused: simultaneously the instruction and the SSA value it defines (the value
// id == this instruction's MirInstId). A value-less instruction (Store, the
// terminators, a void Call) carries `typeId == InvalidType`. `operandStart` /
// `operandCount` address the module operand pool — EXCEPT for `Phi`, where they
// address the phi pool (gated by `opcode == MirOpcode::Phi`). Parent/owner links
// live on the block, not here, keeping the node small for scan-hot passes.
//
// `payload` is per-opcode: Const → literal-pool index; GlobalAddr → SymbolId.v;
// Arg → parameter index; IntrinsicCall → intrinsic id; otherwise unused.
struct MirInst {
    MirOpcode     opcode       = MirOpcode::Invalid;  // 2  — Invalid: visibly-bogus default
    MirInstFlags  flags        = MirInstFlags::None;  // 1
    std::uint8_t  _pad         = 0;                   // 1  — explicit padding
    TypeId        typeId       = InvalidType;         // 8  — result type (Invalid if value-less)
    std::uint32_t operandStart = 0;                   // 4  — into operand pool (or phi pool if Phi)
    std::uint32_t operandCount = 0;                   // 4
    std::uint32_t payload      = 0;                   // 4  — per-opcode scalar
    // Secondary per-opcode scalar. Currently used ONLY by `Alloca`
    // (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): `payload` carries the aggregate
    // byte size (frame-slot sizing), so the local's EFFECTIVE alignment
    // (max of natural + `alignas`) rides here — MIR→LIR reads it to compute
    // each function's max local alignment (fed to the frame layout). 0 for
    // every other opcode + a scalar alloca that recorded no over-alignment
    // (its natural alignment is derivable, so 0 is a safe "no info" sentinel).
    // Grows MirInst 24→28 bytes — the static_assert below still holds.
    std::uint32_t payload2     = 0;                   // 4  — secondary per-opcode scalar
};
static_assert(sizeof(MirInst) <= 32, "detail::MirInst grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirInst>);

// ── basic-block POD ────────────────────────────────────────────────────────────
//
// Owns a contiguous instruction range and a contiguous CFG-successor range. The
// terminator is the last instruction in the range (derived, not stored — single
// source of truth). Successor block-ids live in the module succ pool so the CFG
// is recoverable in O(1) per block WITHOUT parsing the terminator's operands —
// keeping dataflow (operands) and control-flow (successors) cleanly separated.
struct MirBlock {
    std::uint32_t  instStart = 0;                       // 4  — into the instruction arena
    std::uint32_t  instCount = 0;                       // 4  — includes the terminator
    std::uint32_t  succStart = 0;                       // 4  — into the succ pool
    std::uint32_t  succCount = 0;                       // 4
    std::uint32_t  func      = 0;                       // 4  — owning MirFuncId.v (module-implied tag)
    StructCfMarker marker    = StructCfMarker::Linear;  // 1
    std::uint8_t   _pad[3]   = {};                      // 3  — explicit padding
};
static_assert(sizeof(MirBlock) <= 32, "detail::MirBlock grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirBlock>);

// ── function POD ────────────────────────────────────────────────────────────────
//
// Owns a contiguous block range. `signature` is the FnSig in the CU's type
// lattice (the same TypeId discipline HIR's Function uses — interned + shared,
// not a child). The entry block is the function's first block. `symbol` is the
// declared SymbolId.v.
struct MirFunc {
    TypeId        signature  = InvalidType;  // 8  — FnSig TypeId
    std::uint32_t blockStart = 0;            // 4  — into the block arena
    std::uint32_t blockCount = 0;            // 4
    std::uint32_t symbol     = 0;            // 4  — declared SymbolId.v
    // D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD (step 13.6 OPT1 gate,
    // 2026-06-03): linkage attributes for the optimizer's DCE pass.
    // `isExternallyVisible(binding, visibility)` is the DCE-protect
    // predicate; a function for which it returns true MUST NOT be
    // deleted by DCE even when no intra-module use exists. C-style
    // languages without `static` default both fields to (Global,
    // Default) — every function externally visible by language
    // convention. Front-ends with `static` / `inline` / `hidden`
    // emit the matching binding/visibility at HIR→MIR lowering.
    // Fits the existing 4-byte _pad slot — no struct-size growth.
    SymbolBinding    binding    = SymbolBinding::Global;     // 1
    SymbolVisibility visibility = SymbolVisibility::Default; // 1
    // TF-C78 (D-CSUBSET-NOINLINE): the source declared this function
    // `__attribute__((noinline))` — the optimizer's inliner MUST NOT splice its
    // body into any caller. Reaches here as source → SymbolRecord.isNoInline →
    // HirNoInlineMap → this bit (the `binding`/`visibility` route above, whose
    // LinkageAttr shape it copies); read by `inlining.cpp`'s §2.9 legality gate
    // beside the Weak refusal.
    //
    // ★ WHY A REAL SINK RATHER THAN AN IGNORED NAME. The cheap alternative was
    // listing `noinline` as KNOWN-but-inert vocabulary (an `effects` `none` row).
    // That row's contract is "consumed elsewhere or deliberately inert", and with
    // `Inlining` in the SHIPPED release pipeline (release.pipeline.json) nothing
    // consumed it and it was not inert — DSS had a live pass free to contradict
    // the directive. sqlite writes `SQLITE_NOINLINE` to BOUND STACK DEPTH on
    // recursive paths, so ignoring it has a runtime consequence, not a cosmetic
    // one.
    //
    // ★ DIRECTION OF A DROPPED FLAG: a lost `true` means the function gets
    // INLINED anyway — silent, and exactly the outcome the attribute exists to
    // prevent. So it must survive every MirFunc creation/copy/rebuild/serialize
    // path, not merely the lowering that mints it. The `mir_text` printer/parser
    // pair dropped `binding`/`visibility` for exactly this reason before this
    // cycle; both are now carried (see `emitFunction`/`parseFunction`).
    //
    // Steals one byte from the former 2-byte `_pad` — no struct-size growth
    // (the static_assert below still holds at 24).
    bool             noInline   = false;                     // 1
    // TF-C81 (D-CSUBSET-ALWAYSINLINE): the source declared this function
    // `__attribute__((always_inline))` — the optimizer's inliner must NOT let its
    // SIZE-BASED cost model (rule 6, `instCount > inlineThreshold`) refuse this
    // callee. Reaches here by the identical route its `noInline` neighbour takes:
    // source → SymbolRecord.isAlwaysInline → HirAlwaysInlineMap → this bit.
    //
    // ★ IT SUPPRESSES ONE RULE, NOT THE GATE. Every CORRECTNESS refusal still
    // wins — Weak binding, a same-SCC (recursive) call, an address-escaped
    // callee, a callee with no returning path, an arity/type mismatch. And it is
    // read only where an inliner runs at all: under the shipped `debug` pipeline
    // (`Identity` only) there is no cost model to bypass and the bit is inert.
    //
    // ★ DIRECTION OF A DROPPED FLAG — THE MIRROR OF ITS NEIGHBOUR'S. A lost
    // `noInline` true silently DOES the forbidden thing; a lost `alwaysInline`
    // true merely re-applies the size threshold, so the program stays correct and
    // only the optimization is missed. Both are still carried through every
    // MirFunc creation/copy/rebuild/serialize path — and for THIS flag the
    // argument is sharper than for its neighbour: MEASURED, dropping it at the
    // `mir_rebuild_helper` hop leaves the end-to-end shipped-release pin GREEN
    // (Inlining runs first in iteration 1, so the splice is already done), which
    // means only the dedicated propagation pin can catch that hop at all.
    //
    // ★ IF BOTH BITS ARE SET (only reachable through hand-built MIR or parsed
    // `.dssir` — the source tier rejects the combination as
    // S_ConflictingInlineAttributes), the REFUSAL wins: `inlining.cpp` checks
    // rule 2b before the rule-6 bypass. Conservative, and MEASURED to be what
    // clang does with the same contradiction.
    //
    // Takes the LAST byte of the original 2-byte `_pad` — MEASURED, sizeof stays
    // 24 and the static_assert below still holds.
    bool             alwaysInline = false;                   // 1
    // ★★ TF-C85: the source put this function inside an MSVC
    // `#pragma optimize("", off)` region — the optimizer must rebuild it
    // VERBATIM. Reaches here by a route whose LAST FOUR HOPS are its two
    // neighbours' verbatim (SymbolRecord → HirNoOptimizeMap → this bit) but
    // whose FIRST hop is different in kind: not an attribute on the declaration
    // but a LEXICALLY SCOPED PREPROCESSOR REGION, stamped per emitted token so a
    // macro-borne definition answers with the state at its INVOCATION.
    //
    // ★ IT NEUTERS POLICY, IT NEVER SKIPS A REBUILD. `MirFunctionRebuilder`
    // swaps the pass's policy for an identity policy when this bit is set; it
    // still calls `rebuildFunction`, because SKIPPING the call would not
    // preserve the function — it would DELETE it from the rebuilt module.
    //
    // ★ DIRECTION OF A DROPPED FLAG: `alwaysInline`'s, not `noInline`'s. Every
    // DSS optimizer pass is semantics-preserving, so a lost `true` means the
    // function got optimized after all — a directive not honored, never a wrong
    // program. It is carried through every MirFunc creation/copy/rebuild/
    // serialize path regardless, because (TF-C81, MEASURED) a half-landed flag
    // and no flag are indistinguishable in the emitted binary.
    //
    // ★ AND IT IS NOT AN FP FIX. sqlite's motivating use (`ext/misc/totype.c`)
    // targets x87 excess precision, which MEASURED cannot occur in this tree —
    // integer-only const-fold maps, no reassociation, no FMA/fast-math, `double`
    // is SSE2 at exactly 64 bits. This bit is faithfulness to a source
    // directive; describing it as repairing a live miscompile would be false.
    //
    // Takes one more byte from the tail; MEASURED (mir_node's own padding note)
    // sizeof goes 24 → 28 and the `<= 32` static_assert below still holds.
    bool             noOptimize = false;                     // 1
    // TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): the source declared this function
    // `__attribute__((no_sanitize_thread))` — it is EXCLUDED from thread-sanitizer
    // instrumentation. Reaches here by the route its `noInline`/`alwaysInline`
    // neighbours take, hop for hop: source → SymbolRecord.isNoSanitizeThread →
    // HirNoSanitizeThreadMap → this bit.
    //
    // ★★ ITS CONSUMER IS THE SERIALIZER, NOT A PASS — AND THAT IS THE WHOLE
    // HONEST CLAIM. MEASURED: `grep -rni sanitiz src/` has ZERO hits. DSS ships no
    // sanitizer, no instrumentation pass and no `-fsanitize` surface, so there is
    // nothing here for this bit to switch off. What it DOES is be observable:
    // `mir_text`'s `appendFuncAttrs` prints it as `nosanitizethread` and
    // `parseFunction` reads it back, so the per-function fact is queryable in the
    // `.dssir` a user can dump and survives a round trip. Do not rewrite this
    // comment to imply a pass reads it until one does.
    //
    // ★ WHY CARRY IT AT ALL: an ignore-list entry would have discarded the fact
    // three tiers earlier, and the day an instrumentation pass lands it would have
    // to re-derive from source what the front-end already parsed
    // ([[D-TEST-IGNORE-LIST-IS-A-LICENSE-TO-DROP]]). sqlite's `SQLITE_NO_TSAN`
    // (wal.c:932) marks the two wal-index header functions whose races are
    // DELIBERATE and benign — a fact about the program's concurrency contract, not
    // a performance hint, which is exactly the kind a compiler should not silently
    // forget.
    //
    // ★ DIRECTION OF A DROPPED FLAG: `alwaysInline`'s, not `noInline`'s — nothing
    // reads it, so a lost `true` cannot make a wrong program, only an unrecorded
    // directive. Carried through every MirFunc creation/copy/rebuild/serialize path
    // regardless, on TF-C81's MEASURED ground that a half-landed flag and no flag
    // are indistinguishable downstream. ★ AND FOR THIS AXIS THERE IS NO END-TO-END
    // BEHAVIOURAL WITNESS AT ALL (no codegen difference exists to observe), so each
    // propagation hop needs its own direct assertion — the MIR-text and rebuild
    // pins are not the best evidence, they are the ONLY evidence.
    //
    // Takes one more byte from the tail; MEASURED (see the padding-budget note
    // below, re-measured at TF-C92) sizeof stays 28 and the `<= 32` static_assert
    // holds with room to spare.
    bool             noSanitizeThread = false;                // 1
    // NOTE (TF-C81, amended TF-C85 and TF-C92): there is deliberately NO `_pad`
    // member. The six 1-byte fields above (binding, visibility, noInline,
    // alwaysInline, noOptimize, noSanitizeThread) sit in the tail slot, so explicit
    // padding would be a lie about the layout rather than documentation of it.
    //
    // ★ PADDING BUDGET — MEASURED (TF-C82), replacing a WRONG prediction this
    // comment used to carry ("the next flag grows the struct 24 → 32; it should
    // re-introduce `_pad[3]`"). Both halves of that were false, and the root of
    // the error was assuming an 8-byte alignment: `TypeId` is sizeof 8 / ALIGNOF
    // 4, so `MirFunc` is sizeof 24 / **alignof 4**, and the struct grows in
    // 4-byte steps, not 8. Re-measured with `/usr/bin/clang++ -std=c++23 -I src`
    // on arm64-apple-darwin against the real header plus field-for-field replica
    // structs: +1 one-byte flag → **28** (not 32); +8 flags → 32, and the
    // `<= 32` static_assert below STILL HOLDS; +9 flags → 36, the FIRST size
    // that breaches it. So there is room for eight more 1-byte flags, and a
    // re-introduced `_pad` would be padding to nothing until then.
    // ★ Re-MEASURED AT TF-C92 (this cycle added the sixth flag,
    // `noSanitizeThread`) with `/usr/bin/clang++ -std=c++23 -I src` on
    // arm64-apple-darwin against the REAL header — not a replica:
    // **sizeof 28, alignof 4**, unchanged by the addition. The TF-C82 numbers
    // above are stated relative to the then-current FOUR-flag / 24-byte record, so
    // they still read correctly; the closed form they describe is
    // `size = roundUp4(20 + flagCount)`, which puts the ceiling at **12 flags
    // (= 32)** and the first breach at 13 (= 36). At six flags that leaves room
    // for SIX more before the static_assert below fails.
    // ★ Re-MEASURE before quoting these numbers again — a plausible-sounding
    // size claim went unchecked into this comment once already.
};
// ★★ TF-C92: the EXACT size + alignment, not only the ceiling. The `<= 32`
// budget assert below has FOUR BYTES OF SLACK at the current 28, so it pinned
// neither the measured size nor the 4-byte alignment the whole padding-budget
// comment above rests on — and it would NOT have caught this cycle's added flag
// (24 → 28 fits the ceiling silently). It is kept as the BUDGET statement (that
// is what its message says) and joined by the two exact pins the comment's
// arithmetic actually claims:
//   • 28 = roundUp4(20 + 6 flags) — 20 bytes of {TypeId 0-7, blockStart 8-11,
//     blockCount 12-15, symbol 16-19} plus six 1-byte flags at 20-25, rounded to
//     the 4-byte alignment. MEASURED on arm64-apple-darwin with
//     `/usr/bin/clang++ -std=c++23 -I src` against this real header via
//     `offsetof`: signature 0, blockStart 8, blockCount 12, symbol 16,
//     binding 20, visibility 21, noInline 22, alwaysInline 23, noOptimize 24,
//     noSanitizeThread 25.
//   • alignof 4, NOT 8 — `TypeId` is a two-`uint32_t` arena id, so the struct
//     grows in 4-byte steps. Assuming 8 is the exact error that put a wrong size
//     prediction into the comment above once already; pinning it stops the next
//     reader from having to re-derive it.
// A flag added WITHOUT updating these two lines is now a compile error naming the
// new size, which is the moment to re-do the budget arithmetic rather than to
// discover later that a stale comment was quoted.
static_assert(sizeof(MirFunc) == 28,
              "detail::MirFunc is 28 bytes (roundUp4(20 + 6 one-byte flags)) — "
              "if you added a flag, re-measure and update the padding-budget "
              "comment above along with this number");
static_assert(alignof(MirFunc) == 4,
              "detail::MirFunc is 4-byte aligned because TypeId is two uint32_t "
              "— the whole padding budget above assumes 4-byte growth steps");
static_assert(sizeof(MirFunc) <= 32, "detail::MirFunc grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirFunc>);

// ── global POD ──────────────────────────────────────────────────────────────────
//
// A module-level storage cell. `type` is the declared variable type (the same
// lattice TypeId everything else uses); `symbol` is the declared `SymbolId.v`
// (lookup key for `GlobalAddr`). Initialization shape (mutually exclusive):
//   - `initLiteralIndex != UINT32_MAX`: constant initializer — index into the
//     module's `MirLiteralPool`. The literal's value is the global's initial
//     state at module load.
//   - `initFunc.valid()`: non-constant initializer — `initFunc` is a synthesized
//     `__module_init__` MirFunc whose body stores into this global as part of
//     module load. (Plan 12's ML2-globals cycle uses fold-first/fall-back-to-
//     init-function policy: the constant case is the common path.)
//   - Both UINT32_MAX / Invalid: declared with no initializer (zero-init by
//     C language convention; the runtime decides the zero pattern).
// Deliberate divergence from `MirBlock`'s `uint32_t func` field: `initFunc`
// here is a full `MirFuncId` (with arena tag) — globals are written + read
// across multiple lowering passes, so preserving provenance through the
// strong-id form is worth the 4 extra bytes vs. a raw u32. POD is 28B (no
// trailing pad), well under the 32B static_assert ceiling.
struct MirGlobal {
    TypeId        type             = InvalidType;       // 8
    std::uint32_t symbol           = 0;                 // 4  — declared SymbolId.v
    std::uint32_t initLiteralIndex = UINT32_MAX;        // 4  — into MirLiteralPool
    MirFuncId     initFunc{};                           // 8  — module-init function id (strong)
    // D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD (step 13.6 OPT1 gate,
    // 2026-06-03): same linkage discipline as MirFunc — DCE-protected
    // when `isExternallyVisible(binding, visibility)` returns true.
    // Externally-observable globals (a C-style file-scope `int g;`
    // with no `static`) MUST survive DCE / unused-symbol elimination.
    SymbolBinding    binding    = SymbolBinding::Global;     // 1
    SymbolVisibility visibility = SymbolVisibility::Default; // 1
    // D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL (writable data sections cycle): true iff
    // the source declared this global `const`. Read by the assembler's
    // section selection (`lowerMirGlobalsToDataItems`): an INITIALIZED global
    // routes to read-only `.rodata` when const, writable `.data` when mutable.
    // Default `false` (mutable) is the conservative writable default — a global
    // wrongly stamped mutable still lands in writable memory (never the read-
    // only-store crash). Consumes one byte of the former 2-byte pad → zero size
    // growth (the static_assert below still holds).
    bool             isConst    = false;                    // 1
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): true iff the source declared this
    // global `_Thread_local`/`thread_local` (C11/C23 6.2.4 thread storage
    // duration — one object PER THREAD). Read by the assembler's section
    // selection BEFORE isConst (a `const thread_local` goes to the
    // thread-template `.tdata`, never `.rodata` — its address varies per
    // thread), routing initialized → `.tdata` / zero-init → `.tbss`; the
    // format walkers lay the template out as the per-thread image (PT_TLS /
    // the PE TLS directory — slices B/C). Threaded from the source via the
    // declaration-keyed `HirThreadLocalMap` at HIR→MIR lowering (the isConst
    // precedent). Consumes the former 1-byte explicit pad → zero size growth
    // (the static_assert below still holds). Default `false` — ordinary
    // process-shared storage.
    bool             isThreadLocal = false;                  // 1
    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): the EXPLICIT
    // `alignas(N)` alignment in bytes (a power of two ≤ 256), or 0 for no
    // override. Read by the assembler's data-item emission
    // (`lowerMirGlobalsToDataItems`), which raises the emitted symbol's section
    // alignment to `max(natural, this)` when nonzero. Threaded from the source's
    // `alignas` via the declaration-keyed `HirAlignmentMap` at HIR→MIR lowering.
    // Consumes the former 4-byte tail padding → zero size growth (28→32 bytes,
    // the static_assert below still holds).
    std::uint32_t    alignment  = 0;                         // 4
};
static_assert(sizeof(MirGlobal) <= 32, "detail::MirGlobal grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirGlobal>);

} // namespace detail

} // namespace dss

// Cross-arena guard wording (the SH3 / SP1 discipline) for the four MIR arenas.
// The primary `ArenaNames` template is a must-specialize tripwire (arena_tag.hpp),
// so these are mandatory before instantiating any ArenaContainer / ArenaAttribute
// over a MIR id. All four share `MirModuleId` as the arena tag — one module, one
// tag, four element-id spaces — and each names its own element so a fatal
// message identifies which tier's guard fired.
namespace dss::substrate {

template <>
struct ArenaNames<MirInstId, MirModuleId> {
    static constexpr char const* attribute = "MirAttribute";
    static constexpr char const* element   = "MirInstId";
    static constexpr char const* tag       = "MirModuleId";
    static constexpr char const* access    = "Mir::inst";
};

template <>
struct ArenaNames<MirBlockId, MirModuleId> {
    static constexpr char const* attribute = "MirBlockAttribute";
    static constexpr char const* element   = "MirBlockId";
    static constexpr char const* tag       = "MirModuleId";
    static constexpr char const* access    = "Mir::block";
};

template <>
struct ArenaNames<MirFuncId, MirModuleId> {
    static constexpr char const* attribute = "MirFuncAttribute";
    static constexpr char const* element   = "MirFuncId";
    static constexpr char const* tag       = "MirModuleId";
    static constexpr char const* access    = "Mir::func";
};

template <>
struct ArenaNames<MirGlobalId, MirModuleId> {
    static constexpr char const* attribute = "MirGlobalAttribute";
    static constexpr char const* element   = "MirGlobalId";
    static constexpr char const* tag       = "MirModuleId";
    static constexpr char const* access    = "Mir::global";
};

} // namespace dss::substrate
