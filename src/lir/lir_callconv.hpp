#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"          // SymbolId (c116 SEH funclet-parent map)
#include "core/types/target_schema.hpp"
#include "core/types/cfi.hpp"
#include "lir/lir.hpp"
#include "lir/lir_regalloc.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// LIR calling-convention lowering pass (ML7). Consumes a post-
// regalloc `Lir` module (every vreg replaced by a physical register,
// spills materialized as `frame_load`/`frame_store` pseudo-ops) plus
// the matching `LirAllocation` side-table, and produces a fresh
// `Lir` module where:
//
//   * Each function carries a PROLOGUE prepended to its entry block:
//     callee-saved registers (the subset actually used by the
//     function) are stored to the saved-reg area; the stack pointer
//     is adjusted by the total frame size.
//   * Each return site carries an EPILOGUE inserted immediately
//     before the return: the stack pointer is restored; callee-saved
//     registers are reloaded.
//   * `frame_load` pseudo-ops become `load result, [SP + slotOffset]`.
//   * `frame_store` pseudo-ops become `store value, [SP + slotOffset]`.
//
// **Frame layout (target-blind, D-ML7-2.2 closure 2026-06-02)**:
//   [SP+0 .. SP+outgoingArgAreaSize)              outgoing-args area
//                                                  — THIS fn's reserved
//                                                  space for ITS calls.
//                                                  Encompasses BOTH the
//                                                  callee's shadow space
//                                                  (Win64=32, SysV=0) at
//                                                  [SP+0..shadowSpaceBytes)
//                                                  AND any explicit
//                                                  stack-arg overflow at
//                                                  [SP+shadowSpaceBytes..).
//                                                  Zero on leaf fns (no
//                                                  calls means no callee
//                                                  to home args for).
//   [SP+outgoingArgAreaSize
//      .. SP+outgoingArgAreaSize+savedRegAreaSize) saved callee-saved regs
//   [SP+outgoingArgAreaSize+savedRegAreaSize ..)   spill slots
//   [SP+totalFrameSize)                            the original pre-prologue SP
//
// `outgoingArgAreaSize = hasCalls ? (cc.shadowSpaceBytes +
//    max_overflow_slots * slotSize) : 0`. Under slot-aligned cc
// (Win64 ms_x64) max_overflow_slots = max(0, max_args_across_calls -
// max(argGprs, argFprs)). Under independent-counters (SysV/AAPCS64)
// it's the per-class overflow sum across calls.
//
// `totalFrameSize = hasCalls
//    ? alignedSizeWithBias(rawPreShadow, cc.stackAlignment,
//                          cc.callPushBytes)
//    : alignUp(rawPreShadow, cc.stackAlignment)`
// where `rawPreShadow = outgoingArgAreaSize + savedRegAreaSize +
// spillAreaSize`. The Win64 shadow-space requirement collapses INTO
// outgoingArgAreaSize (no separate max() with shadowSpaceBytes —
// it's already there).
//
// Spill slot N is at offset `outgoingArgAreaSize + savedRegAreaSize +
// N * regWidth`. `regWidth` is the cc's primary integer register
// width (8 bytes on x86_64/ARM64).
//
// **Target-blind output**: prologue/epilogue use the schema's
// `load`/`store`/`add`/`sub` opcodes (resolved by mnemonic) plus the
// existing 3-operand memory addressing. Each saved register's class
// is read from `schema.registerInfo(ordinal)->regClass` so an FPR
// callee-save (e.g. MS-x64 xmm6..xmm15) materializes with FPR class,
// not silently as GPR. Push/pop optimization (target-specific) is
// deferred to the assembler tier — see plan 13 AS1.
//
// **Reads**:
//   * `TargetSchema.callingConventions[alloc.callingConventionIndex]`
//     for the cc's `calleeSaved` set, `stackPointer.ordinal`,
//     `stackAlignment`.
//   * `TargetSchema.frameLoadMnemonic()` / `frameStoreMnemonic()` to
//     locate the pseudo-ops being materialized.
//   * The schema's `opcodeByMnemonic` for `"mov"`, `"add"`, `"sub"`,
//     `"ret"`.
//
// **Failure modes**: the result's `ok()` returns false iff the output
// module is non-empty AND every function got a layout (matches the
// cycle-3a `LirAllocation::ok()` discipline — derived, not stored).
// Possible diagnostic emission paths:
//   * Required opcode missing from the schema → `L_RequiredLirOpcodeMissing`.
//   * `widthForClass` returns 0 (no GPR/FPR width declared) →
//     `L_RequiredLirOpcodeMissing` from `computeFrameLayout`.
//   * Stack-pointer register not declared on the cc → caught at
//     schema-load time by `validate()` for register-machine ABIs
//     (also defensively re-checked here as
//     `L_RequiredLirOpcodeMissing`).
//   * `alloc.perFunc.size() != src.moduleFuncCount()` →
//     `L_VirtualRegInPostRegalloc`.
//   * `funcAlloc.originalSymbol` doesn't match the rewritten
//     function's symbol → `L_VirtualRegInPostRegalloc` (the
//     parallel-index structural guard).
//   * Per-function allocation failed (`funcAlloc.ok == false`) →
//     `L_VirtualRegInPostRegalloc`.
//   * Calling-convention index out of range →
//     `R_CallingConventionLookupFailed`.
//   * Malformed `frame_store` operands or > 2-successor terminator →
//     `L_UnsupportedLoweringForOpcode`.

namespace dss {

// D-LK10-ENTRY-TRAMP-PROLOGUE: smallest non-negative integer `N`
// satisfying BOTH `N >= rawBytes` AND `N ≡ entryBias (mod
// stackAlignment)`. This is the single formula that decides how
// many bytes a CALL-MAKING function must subtract from its stack
// pointer at entry to (a) reserve at least `rawBytes` of frame
// space and (b) land the call site at the cc's stack-alignment.
//
// Consumers:
//   * Trampoline emitter (`src/link/entry_trampoline.cpp`): passes
//     `rawBytes = cc.shadowSpaceBytes`, `entryBias =
//     cc.entryStackPointerBias`. Result: 40 on Win64 (32 shadow +
//     8 realign), 0 on SysV ELF / Mach-O / ARM64.
//   * ML7 callconv lowering (`lir_callconv.cpp::computeFrameLayout`):
//     anchored D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY for when normal-
//     function call-site shadow-space tightening lands (today ML7
//     calls `alignUp(savedRegs + spill, alignment)` directly, which
//     is `alignedSizeWithBias(... , bias=0)` for non-call-makers
//     after callconv-pass-injected call-site shadow allocates
//     separately).
//
// Algorithm: clamp `rawBytes` up to `entryBias`, then add whole
// `stackAlignment` quanta until the congruence holds. Closed-form
// version computes the modular delta in one step.
//
// Pre-conditions (caller's responsibility — validator at schema-
// load time enforces these for cc fields):
//   * `stackAlignment` is a non-zero power of two.
//   * `entryBias < stackAlignment` (bias is an offset INTO the
//     quantum, not a multiple of it).
//
// `stackAlignment == 0` returns `rawBytes` verbatim (degenerate
// case for non-register-machine targets).
[[nodiscard]] constexpr std::uint32_t
alignedSizeWithBias(std::uint32_t rawBytes,
                    std::uint32_t stackAlignment,
                    std::uint32_t entryBias) noexcept {
    if (stackAlignment == 0) return rawBytes;
    std::uint32_t const remainder = rawBytes % stackAlignment;
    if (remainder == entryBias) return rawBytes;
    // delta = (entryBias - remainder) mod stackAlignment, computed
    // in unsigned arithmetic so wrap is well-defined.
    std::uint32_t const delta =
        (entryBias + stackAlignment - remainder) % stackAlignment;
    return rawBytes + delta;
}

// ── WHICH CLASSES HAVE A RESULT-REGISTER POOL, AS ROWS ───────────────────────
//
// D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET, and
// D-LIR-RETURN-REG-REFUSAL-IS-UNREACHABLE-FROM-THE-TEST-TIER.
//
// One row per register class that can hold a returned value, naming the cc pool
// it reads. `returnRegisterForClass` walks these to DECIDE, and its refusal
// renders the same rows to ADVERTISE — so the two cannot disagree.
//
// ★★ WHY THE ROWS AND THE LOOKUP LIVE IN THE HEADER RATHER THAN IN AN ANONYMOUS
// NAMESPACE, WHICH IS WHERE THEY WERE BORN. The conversion that made ACCEPTANCE
// and ADVERTISEMENT one walk is only worth what a test can hold it to, and in an
// anonymous namespace it was worth NOTHING: ✔MEASURED — retyping the accepted
// set back into a hand-written literal (`"only gpr/fpr results can be returned
// in registers"`, the pre-VR sentence) left `ctest` at rc=0 with 16 of 16 LIR
// tests GREEN, because no caller in this tree reaches a `None`/`Flags` result
// and no tier could observe the refusal at all. A guard that cannot fire asserts
// nothing. The lookup is a pure query over `(schema, cc, class, ordinal)` —
// exactly the shape `lir_pass_util::incomingArgRegister` already publishes for
// the ARG side — so publishing the RETURN side is the symmetric position, not a
// hole opened for a test.
//
// ⚠ THE SPELLINGS ARE NOT MINTED HERE: `lirRegClassName` delegates to
// `kTargetRegClassTable`, the one owner of a register class's name across the
// `.target.json` and `.dsslir` surfaces.
struct ReturnPoolRow {
    LirRegClass                                        cls;
    std::vector<std::string> TargetCallingConvention::* pool;
};
inline constexpr std::array<ReturnPoolRow, 3> kReturnPoolRows{{
    {LirRegClass::GPR, &TargetCallingConvention::returnGprs},
    {LirRegClass::FPR, &TargetCallingConvention::returnFprs},
    {LirRegClass::VR,  &TargetCallingConvention::returnVrs},
}};

// ⚠ AND THE COUNT CANNOT SILENTLY OUTRUN THE ROWS. `std::array<Row, N>` with
// fewer than N initializers is legal C++ and VALUE-INITIALIZES the remainder —
// a row whose `pool` is a NULL pointer-to-member and whose class is `None`.
// `returnRegisterPool` would then match `None` against that padding row and
// dereference the null member, which is undefined behaviour reached through a
// hand-edit that compiles clean. ✔MEASURED while planting the row-deletion
// mutant for this file's pins: deleting one row without adjusting the count
// built without a single warning. Same guarantee `DSS_CHECK_ENUM_NAME_TABLE`
// gives the spelling tables, by the same means — the BUILD stops.
static_assert([] {
    for (auto const& row : kReturnPoolRows) {
        if (row.pool == nullptr) return false;
        if (row.cls == LirRegClass::None) return false;
    }
    return true;
}(), "kReturnPoolRows has a padding row — the array's declared size outruns its "
     "initializers, and a row with a null pointer-to-member would be matched "
     "against LirRegClass::None and dereferenced");

// The rendered accepted set, built ONCE off those rows. Not a literal, and not a
// second array of names either — it is the rows' own spellings.
inline constexpr auto kReturnPoolClassNames = [] {
    std::array<std::string_view, kReturnPoolRows.size()> out{};
    for (std::size_t i = 0; i < kReturnPoolRows.size(); ++i) {
        out[i] = lirRegClassName(kReturnPoolRows[i].cls);
    }
    return out;
}();

// The cc pool a result of class `cls` is returned in, or `nullptr` when this
// class has no row — which is the SAME question the refusal below answers, asked
// once. A class with no row owns no pool and is not silently filed into
// somebody else's.
[[nodiscard]] constexpr std::vector<std::string> const*
returnRegisterPool(TargetCallingConvention const& cc, LirRegClass cls) noexcept {
    for (auto const& row : kReturnPoolRows) {
        if (row.cls == cls) return &(cc.*row.pool);
    }
    return nullptr;
}

// Lookup the `ordinal`-th return register of the given class. Slot 0 is the
// primary return register (the scalar / first-eightbyte result); higher slots
// are the additional eightbyte pieces of an in-register struct return (SysV's
// rax+rdx / xmm0+xmm1 — FC7 C1c, D-FC7-SYSV-STRUCT-RETURN-IN-REGS). `ordinal`
// is the PER-CLASS index (GPR and FPR pieces counted separately).
//
// Returns `nullopt` + a loud diagnostic when the class has no row
// (`returnRegisterPool` is nullptr), when `ordinal` is past that pool, or when
// the cc names a register the target schema does not declare.
[[nodiscard]] DSS_EXPORT std::optional<LirReg>
returnRegisterForClass(TargetSchema const&            schema,
                       TargetCallingConvention const& cc,
                       LirRegClass                    cls,
                       std::uint32_t                  ordinal,
                       std::string_view               contextLabel,
                       DiagnosticReporter&            reporter);

// ── WHICH CLASSES HAVE AN ARG-PASSING POOL, AS ROWS ──────────────────────────
//
// D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR.
//
// ★★★ THIS IS `kReturnPoolRows`' TWIN, ONE CYCLE LATER, FOR THE IDENTICAL
// DEFECT ON THE SIBLING FUNCTION — and it is deliberately the SAME pattern
// rather than a second one. `returnRegisterForClass`'s banner above records
// `returnVrs` as "declared ... populated by the loader ... and this function
// never read it", with a VR result taking the GPR ELSE branch into the integer
// pool and producing a wrong-file capture with no diagnostic. `argPassingReg`
// had the same `(cls == FPR) ? argFprs : argGprs` selection over the same
// >2-member vocabulary, and `argVrs` had the same never-read status.
//
// ⚠ THE ARG SIDE'S DEFECT WAS LIVE, NOT LATENT — the measurement lives in the
// row: a `"w"`-constrained value passed to a `double` parameter compiled rc=0
// with BOTH arguments wrong.
//
// ⚠⚠ AND THE ARG SIDE IS NOT A COPY OF THE RETURN SIDE, BECAUSE THE COUNTER IS
// NOT THE SAME SHAPE. `returnRegisterForClass` takes an `ordinal` the caller
// supplies and every class's pool is independent. Arg passing walks CURSORS, and
// AAPCS64 §6.4.2 stage C.1 allocates a Half/Single/Double/Quad-precision
// floating-point OR SHORT VECTOR argument to `v[NSRN]` — ONE counter across
// what this codebase calls FPR (the d-views) and VR (the v-views). Two rows,
// one cursor. `argPoolsShareACursor` below is why that is DERIVED, not declared.
// ── D-LIR-FRAME-SLOT-STRIDE-ENUMERATES-CLASSES-INSTEAD-OF-DERIVING ─────────
//
// The uniform byte stride of the frame's saved-register area and spill area.
// `occupants` is the set of register classes that actually put a value in one
// of those areas in the function being laid out; the answer is the widest
// register any of them declares, never below the historic `max(GPR, FPR)`
// floor (which the LOCAL-alloca area shares and which must stay ≥ every C
// scalar's alignment).
//
// ★ PUBLISHED RATHER THAN PRIVATE, and for the reason
// [[D-LIR-RETURN-REG-REFUSAL-IS-UNREACHABLE-FROM-THE-TEST-TIER]] recorded: a
// derivation no tier can observe is a derivation whose mutant reddens nothing.
// The defect this replaced — `max(widthForClass(GPR), widthForClass(FPR))`
// written inline — was a two-member enumeration of a longer vocabulary, and it
// produced an 8-byte stride for arm64's 16-byte `vr` class: two spill slots
// that OVERLAPPED by 8 bytes, the second reading past the top of the frame.
[[nodiscard]] DSS_EXPORT std::uint32_t
frameSlotStride(TargetSchema const& schema,
                std::span<LirRegClass const> occupants) noexcept;

// Convenience for call sites (and pins) that name the occupant classes
// literally. Same function, same floor — a second overload, never a second
// derivation.
[[nodiscard]] inline std::uint32_t
frameSlotStrideForClasses(TargetSchema const& schema,
                          std::initializer_list<LirRegClass> occupants) noexcept {
    return frameSlotStride(schema, std::span<LirRegClass const>{
                                       occupants.begin(), occupants.size()});
}

struct ArgPoolRow {
    LirRegClass                                        cls;
    std::vector<std::string> TargetCallingConvention::* pool;
};
inline constexpr std::array<ArgPoolRow, 3> kArgPoolRows{{
    {LirRegClass::GPR, &TargetCallingConvention::argGprs},
    {LirRegClass::FPR, &TargetCallingConvention::argFprs},
    {LirRegClass::VR,  &TargetCallingConvention::argVrs},
}};

// Same anti-padding guarantee the return table carries, for the same measured
// reason: `std::array<Row, N>` with fewer than N initializers is legal C++ and
// value-initializes the remainder into a row whose `pool` is a NULL
// pointer-to-member and whose class is `None` — which `argRegisterPool` would
// then match against `LirRegClass::None` and dereference. The BUILD stops.
static_assert([] {
    for (auto const& row : kArgPoolRows) {
        if (row.pool == nullptr) return false;
        if (row.cls == LirRegClass::None) return false;
    }
    return true;
}(), "kArgPoolRows has a padding row — the array's declared size outruns its "
     "initializers, and a row with a null pointer-to-member would be matched "
     "against LirRegClass::None and dereferenced");

// The rendered accepted set, built ONCE off those rows — so ACCEPTANCE and
// ADVERTISEMENT are one walk over one row set and cannot drift apart.
inline constexpr auto kArgPoolClassNames = [] {
    std::array<std::string_view, kArgPoolRows.size()> out{};
    for (std::size_t i = 0; i < kArgPoolRows.size(); ++i) {
        out[i] = lirRegClassName(kArgPoolRows[i].cls);
    }
    return out;
}();

// The cc pool an argument of class `cls` is passed in, or `nullptr` when this
// class has no row. A class with no row owns no pool and is NOT silently filed
// into somebody else's — which is the whole defect this table closes.
//
// ⚠ `nullptr` (no row at all) and an EMPTY pool (a row exists; the cc declares
// no registers for it) are DIFFERENT FACTS and get different refusals — see
// `argPassingRegister`.
[[nodiscard]] constexpr std::vector<std::string> const*
argRegisterPool(TargetCallingConvention const& cc, LirRegClass cls) noexcept {
    for (auto const& row : kArgPoolRows) {
        if (row.cls == cls) return &(cc.*row.pool);
    }
    return nullptr;
}

// ★★★ THE COUNTER IDENTITY, DERIVED FROM THE TARGET RATHER THAN DECLARED
// BESIDE IT.
//
// Two arg pools share ONE cursor exactly when their k-th entries are the SAME
// PHYSICAL REGISTER. That is not a new fact needing a new schema key — it is
// already in every register row: `dwarfNumber` is DWARF's own identifier for a
// physical register, so two register declarations carrying the same
// `dwarfNumber` ARE one register wearing two widths.
//
// ✔MEASURED over the shipped targets: arm64 has exactly 32 shared
// `dwarfNumber`s, each pairing a `d_k` (class fpr) with a `v_k` (class vr) —
// the whole V-register file and nothing else; x86_64 has ZERO, so the
// derivation is correctly inert there.
//
// ⚠ WHY NOT A `sharedCounter` KEY ON THE CALLING CONVENTION: it would be a
// SECOND OWNER of a fact the register table already carries, and the loader
// could not detect the two disagreeing — the shape §A.1b records as "a fact
// with an owner does not get a second owner" (the reverted `asmSyntax` facet,
// and the `views`-vs-`subOf` duplicate). A cc declaring `sharedCounter: false`
// over two aliasing pools would hand slot k out twice, silently.
//
// ⓘ `cc.slotAligned` is a DIFFERENT, independent fact — Win64's ONE positional
// cursor across ALL classes, where arg k takes slot k whatever its class. It
// already has an owner and still wins; this derivation answers only the
// non-slot-aligned case.
[[nodiscard]] DSS_EXPORT bool
argPoolsShareACursor(TargetSchema const&            schema,
                     TargetCallingConvention const& cc,
                     LirRegClass                    a,
                     LirRegClass                    b) noexcept;

// Lookup the `index`-th arg-passing register of the given class.
//
// ★ PUBLISHED, for the reason `
// D-LIR-RETURN-REG-REFUSAL-IS-UNREACHABLE-FROM-THE-TEST-TIER` measured on the return side: a refusal in an anonymous
// namespace is worth nothing, because retyping its accepted set back into a
// hand-written literal left `ctest` green. These refusals are this change's
// deliverable, so they are reachable from the test tier.
//
// Returns `nullopt` + a loud diagnostic in three DISTINCT cases, which are three
// distinct facts and never share a message:
//   * the class has NO ROW — `argRegisterPool` is nullptr;
//   * the class has a row and the cc declares NO REGISTERS for it — an empty
//     pool states what this target made allocatable, and is NOT a capacity
//     overflow the stack could absorb;
//   * the pool is EXHAUSTED — `index >= pool.size()`, which IS stack passing
//     (D-ML7-2.2).
[[nodiscard]] DSS_EXPORT std::optional<LirReg>
argPassingRegister(TargetSchema const&            schema,
                   TargetCallingConvention const& cc,
                   std::uint32_t                  index,
                   LirRegClass                    cls,
                   std::string_view               contextLabel,
                   DiagnosticReporter&            reporter);

// ── THE ARG CURSORS, AS ONE OBJECT INSTEAD OF THREE HAND-KEPT COPIES ─────────
//
// D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR.
//
// ★★★ THE ROW TABLE ALONE DOES NOT CLOSE THIS DEFECT, AND THAT IS THE MEASURED
// FINDING RATHER THAN A DESIGN PREFERENCE. ✔With `argPassingRegister` converted
// to the rows, the reproduction was recompiled and came out **byte-identical** —
// because the register an outgoing argument lands in is decided by a cursor walk
// that existed in THREE separate copies, none of them `argPassingRegister`:
// `lir_rewrite::classifyCallRegArgs`, `lir_wide_call_args::lowerOneFunc`, and
// the outgoing-area counting in `lir_callconv`. Each carried its own
// `(cls == FPR) ? fprIdx++ : gprIdx++`, and two of them carried a comment
// promising to "advance the shared cursors exactly as callconv does" — a
// promise kept by hand, which is the shape that lets three passes disagree.
//
// ⇒ ONE object owns the walk; the copies call it. The hazard the row describes
// as "four more copies must move together or the passes will disagree" stops
// being a discipline and becomes a structural impossibility.
//
// THE GROUPING IS DERIVED, NOT DECLARED — see `argPoolsShareACursor`. Classes
// whose pools are two views of one physical register file share one cursor
// (AAPCS64's single NSRN across the d-views and the v-views); disjoint files get
// their own. `cc.slotAligned` overrides both into ONE positional cursor across
// every class (Win64: arg k takes slot k whatever its class).
class DSS_EXPORT ArgCursors {
public:
    ArgCursors(TargetSchema const& schema, TargetCallingConvention const& cc);

    struct Slot {
        std::uint32_t index;     // the cursor value this argument consumed
        std::uint32_t poolSize;  // the pool it must fit inside
    };

    // Consume one cursor slot for an argument of class `cls`.
    //
    // `nullopt` ⇔ `cls` names no arg pool at all (`kArgPoolRows` has no row for
    // it — `None`, `Flags`). That is a DIFFERENT fact from an exhausted cursor,
    // which returns a `Slot` whose `index >= poolSize`, and callers must keep
    // them apart: exhaustion is stack passing, no-row is a refusal.
    [[nodiscard]] std::optional<Slot> next(LirRegClass cls);

    // The by-value-stacked-aggregate clamp: an aggregate that exhausted a
    // class's registers pushes that class's cursor to its pool size, so every
    // later argument of that class overflows too. Takes the CLASS rather than a
    // hand-passed cursor, so a caller cannot clamp the wrong one.
    void exhaust(LirRegClass cls);

    [[nodiscard]] std::uint32_t poolSizeFor(LirRegClass cls) const;

private:
    // One entry per row in `kArgPoolRows`, plus the slot-aligned collapse.
    std::array<std::uint32_t, kArgPoolRows.size()> group_{};   // class row -> cursor group
    std::array<std::uint32_t, kArgPoolRows.size()> cursor_{};  // group -> next index
    std::array<std::uint32_t, kArgPoolRows.size()> poolSize_{};// group -> bound
    bool slotAligned_ = false;

    [[nodiscard]] static std::optional<std::size_t> rowOf(LirRegClass cls);
};

// ── THE OVERFLOW-AREA CURSOR, AS ONE OBJECT FOR THE SAME REASON ─────────────
//
// D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED.
//
// `ArgCursors` answers "does this argument get a REGISTER"; this answers "and if
// it does not, WHERE in the overflow area does it sit and HOW WIDE is the access".
// The two questions were separated in the code but not in the hazard: the byte
// placement of stacked arguments is walked in FOUR places that must agree
// byte-for-byte or the caller writes where the callee does not read —
//   * `lir_callconv::computeMaxOutgoingStackArgs` (sizes the outgoing area),
//   * `lir_wide_call_args::lowerOneFunc` (assigns each overflow arg its offset),
//   * `lir_callconv::materializeOneFunc`'s call arm (the residual placement),
//   * `lir_callconv::materializeOneFunc`'s `arg` arm (the CALLEE's read),
// and each of them used to spell the rule as its own `idx * outgoingSlotSize`.
// That was survivable while the rule was one slot per argument. It is not
// survivable now that a CC may declare NATURAL packing, because then the rule
// depends on each datum's own size and alignment and a fifth spelling is a
// silent miscompile. So the rule is spelled ONCE, here.
//
// ★ THE ACCESS WIDTH IS PART OF THE PLACEMENT, NOT A SEPARATE DECISION. Under
// `Slot` the datum owns the whole pointer-width slot — the caller stored a whole
// register into it — so the access stays the 8-byte one every existing target
// emits (`widthFlags == 0`), byte-for-byte. Under `Natural` the datum owns
// EXACTLY its own bytes, and an 8-byte store would clobber the next argument
// (Apple puts a `short` at +2 with an `int` at +4), so the access must be
// width-exact. Returning them together is what makes "packed narrow but accessed
// wide" unwritable.
class DSS_EXPORT StackArgCursor {
public:
    // `slotBytes` is the outgoing slot quantum (the GPR/pointer width) — the
    // same value `FrameLayout::outgoingSlotSize` carries.
    StackArgCursor(TargetCallingConvention const& cc,
                   std::uint32_t slotBytes) noexcept
        : rules_(cc.stackArgPacking), slot_(slotBytes == 0 ? 1u : slotBytes) {}

    struct Placement {
        std::uint32_t byteOffset;   // offset within the overflow area
        std::uint8_t  widthFlags;   // kLirInstFlagWidth* for the access; 0 ⇒ 64-bit
    };

    // Place ONE stacked NAMED scalar whose natural size is `naturalBytes`
    // (1/2/4/8; 0 or >slot ⇒ treated as a whole slot, which is what every
    // pre-`Natural` producer effectively said).
    [[nodiscard]] Placement placeNamedScalar(std::uint32_t naturalBytes) noexcept {
        return place(rules_.namedScalars, naturalBytes);
    }

    // Place ONE stacked VARIADIC argument. Separate axis: Apple packs named
    // scalars naturally but keeps 8-byte slots for varargs (✔MEASURED).
    [[nodiscard]] Placement placeVariadic(std::uint32_t naturalBytes) noexcept {
        return place(rules_.variadic, naturalBytes);
    }

    // Place ONE stacked by-value AGGREGATE of `aggBytes`: ceil(aggBytes/slot)
    // whole slots at slot alignment.
    //
    // ⚠ THIS AXIS HAS ONE BUILDABLE VALUE AND THE OTHER IS REFUSED AT LOAD, not
    // approximated here. ✔MEASURED: BOTH shipped ABIs slot-round aggregates
    // (Apple puts an `int` after a 3-byte struct at +8, and after a 12-byte
    // struct at +16 — not +3 / +12), so `slot` is what the vocabulary needs
    // today. A CC declaring `namedAggregates: "natural"` would need the
    // aggregate's own ALIGNMENT, which the `ByValueStackAgg` carrier does not
    // state (it carries a byte SIZE only) — so the loader REFUSES that spelling
    // rather than letting this method guess slot alignment and call it natural.
    // (`target_schema_json.cpp`, the `stackArgPacking` reader.)
    [[nodiscard]] std::uint32_t placeNamedAggregate(std::uint32_t aggBytes) noexcept {
        std::uint32_t const span = ((aggBytes + slot_ - 1u) / slot_) * slot_;
        std::uint32_t const off  = alignUp(cursor_, slot_);
        cursor_ = off + span;
        return off;
    }

    // The raw cursor (bytes consumed so far, NOT slot-rounded).
    [[nodiscard]] std::uint32_t bytes() const noexcept { return cursor_; }

    // The cursor rounded up to a whole slot — the size the outgoing area must
    // reserve, and the byte displacement a callee's `va_start` must skip.
    [[nodiscard]] std::uint32_t slotAlignedBytes() const noexcept {
        return alignUp(cursor_, slot_);
    }

    [[nodiscard]] std::uint32_t slotBytes() const noexcept { return slot_; }

private:
    [[nodiscard]] static constexpr std::uint32_t
    alignUp(std::uint32_t v, std::uint32_t a) noexcept {
        return a == 0 ? v : ((v + a - 1u) / a) * a;
    }

    [[nodiscard]] Placement place(StackArgPacking rule,
                                  std::uint32_t naturalBytes) noexcept {
        // ⚠ A natural size the producer did not state (0), or one larger than a
        // slot, falls back to the SLOT rule. That is the pre-existing behaviour
        // and is correct for every `Slot` CC; a `Natural` CC that reaches here
        // with 0 is a DROPPED CARRIER, and the caller-side walk refuses it loudly
        // rather than letting the two sides disagree — see
        // `lir_wide_call_args::lowerOneFunc`.
        bool const natural = rule == StackArgPacking::Natural
                             && naturalBytes != 0 && naturalBytes <= slot_;
        std::uint32_t const size  = natural ? naturalBytes : slot_;
        std::uint32_t const align = natural ? naturalBytes : slot_;
        std::uint32_t const off   = alignUp(cursor_, align);
        cursor_ = off + size;
        return Placement{off, natural ? widthFlagsForBytes(naturalBytes)
                                      : std::uint8_t{0}};
    }

    [[nodiscard]] static constexpr std::uint8_t
    widthFlagsForBytes(std::uint32_t bytes) noexcept {
        switch (bytes) {
            case 1:  return kLirInstFlagWidth8;
            case 2:  return kLirInstFlagWidth16;
            case 4:  return kLirInstFlagWidth32;
            default: return 0;   // 8 (and anything else) ⇒ the 64-bit access
        }
    }

    StackArgPackingRules rules_{};
    std::uint32_t        slot_   = 8;
    std::uint32_t        cursor_ = 0;
};

// Per-function frame layout computed before emission. Stored so
// downstream passes (AS1 unwind info, debug-info DWARF .debug_frame
// generation) can read it without re-computing.
//
// `savedRegs` are typed `LirReg` (isPhysical=1, class carried per
// entry) so prologue/epilogue emission picks the correct store/load
// opcode per class. A bare `uint16_t` ordinal would silently
// misclassify FPR/VR callee-saves (MS-x64's xmm6..xmm15) as GPR.
struct DSS_EXPORT FrameLayout {
    std::uint32_t       totalFrameSize    = 0;  // bytes the prologue subtracts from SP
    std::uint32_t       outgoingArgAreaSize = 0; // bytes reserved at [SP+0..) for THIS function's outgoing stack args
    std::uint32_t       savedRegAreaSize  = 0;  // bytes occupied by callee-saved regs
    std::uint32_t       spillAreaSize     = 0;  // bytes occupied by spill slots
    // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02): byte
    // count for body-declared local allocas (one `alloca` LIR op =
    // one `slotSize`-byte slot). Allocas live ABOVE spill slots in
    // the frame layout (positive RSP offset post-prologue). The
    // materialize pass rewrites each `alloca` to a `lea result,
    // [sp + localAreaOffset() + i*slotSize]` using the 3-op LEA
    // form. Slot assignment is by scan order (first alloca in
    // function source order = local index 0). The `numLocalAllocas`
    // field carries the count so the layout can be reproduced by
    // any consumer (debug-info unwind, audit tests) without
    // re-scanning the LIR.
    std::uint32_t       localAreaSize     = 0;  // bytes occupied by local-alloca slots
    std::uint32_t       numLocalAllocas   = 0;  // count of `alloca` LIR ops in this function
    // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: padding bytes inserted BETWEEN the
    // spill area and the local-alloca area so the local base lands on the
    // max-local-alignment boundary. Nonzero ONLY when a function has an
    // over-aligned local (`alignas`, or a naturally >8-aligned type like
    // `long double`) AND the raw local base (outgoing+saved+spill) does not
    // already satisfy that alignment — i.e. an ODD outgoing-arg count leaves the
    // base ≡ 8 (mod 16). Every other frame keeps this 0 → its layout is
    // byte-identical to before this cycle (the zero-blast-radius invariant).
    // Folded into `localAreaOffset()` (so alloca offsets shift with it) AND into
    // `totalFrameSize` (so the prologue grows + RSP stays call-aligned).
    std::uint32_t       localAreaAlignPad = 0;
    // FC12a-core (D-FC12A-VARIADIC-CALLEE): bytes reserved for the variadic
    // register-save-area — the zone a variadic callee's prologue spills its integer
    // + (al-gated) SSE arg registers into. Nonzero ONLY when this function calls
    // va_start (detected by a `va_reg_save_area` LIR op) AND the CC declares a
    // `vaListLayout` (size = vaListLayout.regSaveAreaBytes()). Zero everywhere else
    // — backward-compatible with every non-variadic frame.
    std::uint32_t       vaRegSaveAreaSize = 0;
    // Uniform per-class spill-slot width in bytes. DERIVED by `frameSlotStride`
    // from the classes that actually occupy a slot in this function, floored at
    // the historic GPR/FPR width — never the two-member `max` it used to be.
    // ⚠ That old spelling is the defect
    // D-LIR-FRAME-SLOT-STRIDE-ENUMERATES-CLASSES-INSTEAD-OF-DERIVING records:
    // arm64's `vr` is 16 bytes, so it sized two 16-byte slots 8 bytes apart and
    // the second read past the top of the frame. This comment still described
    // it after the fix; the P28 step-10 audit caught that.
    std::uint32_t       slotSize          = 0;
    std::uint32_t       outgoingSlotSize  = 0;  // outgoing-arg slot width (bytes; = pointer width = GPR width)
    // c114 (D-WIN64-PDATA-XDATA-UNWIND): the cc's guard-page stack-probe
    // stride (bytes; 0 = no probing — Linux/macOS/arm64). A downstream
    // unwind-info emitter reproduces the prologue's probe-vs-plain-sub
    // decision from this + totalFrameSize (the same test emitPrologue uses).
    std::uint32_t       stackProbePageBytes = 0;
    std::vector<LirReg> savedRegs;              // callee-saved phys regs actually used
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY: did this function contain at
    // least one call-shaped opcode (per `TargetOpcodeInfo::isCall`)
    // at frame-layout time? True ⇒ totalFrameSize incorporates
    // `cc.shadowSpaceBytes` AND satisfies `N ≡ cc.callPushBytes mod
    // cc.stackAlignment`. False ⇒ totalFrameSize is the existing
    // `alignUp(raw, cc.stackAlignment)` (leaf-fn rule). Exposed so
    // downstream consumers (debug-info unwind, audit tests, future
    // CFI emitters) can verify the invariant without re-scanning
    // the source LIR.
    bool                hasCalls          = false;

    // Derived: saved-reg area starts immediately after the outgoing-
    // args area. Updated by D-ML7-2.2 closure (2026-06-02) — the
    // outgoing area is the new SP+0 zone for stack-arg overflow on
    // ANY cc that overflows its argGprs/argFprs pool. Zero when this
    // function makes no calls or every call fits in the register
    // pool — backward-compatible with leaf-fn / register-only-call
    // shapes.
    [[nodiscard]] constexpr std::uint32_t
    savedRegAreaOffset() const noexcept { return outgoingArgAreaSize; }

    // Derived: spill area starts immediately after the saved-reg area
    // (which itself starts after the outgoing-args area).
    [[nodiscard]] constexpr std::uint32_t
    spillAreaOffset() const noexcept {
        return outgoingArgAreaSize + savedRegAreaSize;
    }

    // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b): local-alloca area
    // starts immediately after the spill area (above spills in the
    // stack-grows-down convention; positive offset from post-prologue
    // RSP). Each alloca i (0-indexed by scan order) sits at
    // `localAreaOffset() + i * slotSize`. Zero when this function
    // has no body-local declarations — backward-compatible with all
    // pre-13.3b shapes (corpus tests + globals-only examples).
    //
    // **Frame-zone ordering [outgoing | saved | spill | LOCALS]
    // rationale** (7-agent fold A4 + 2nd-order silent-failure HIGH-2
    // correction): saved-reg area MUST sit contiguously above the
    // prologue's push sequence for unwind-info correctness on
    // Windows x64 — UWOP_PUSH_NONVOL codes in UNWIND_INFO record
    // each saved-reg push relative to the prologue's SP-adjusting
    // subq, so no other zone may interleave between the pushes and
    // the saved-reg block. Locals-above-spill (vs spill-above-
    // locals) is LLVM's x86 FrameLowering convention; the disp8
    // encoding-density argument (smaller 1-byte vs 4-byte
    // displacement for spill traffic) is anchored as a future win
    // — the current x86_variable encoder emits only MemDisp32 mode
    // (`x86_variable.cpp::ModMode::{RegDirect,MemDisp32,RipRel}`),
    // so the displacement-size benefit doesn't materialize until
    // a `MemDisp8` mode + selection lands (anchor: future
    // `D-AS4-DISP8-ENCODING` cycle).
    [[nodiscard]] constexpr std::uint32_t
    localAreaOffset() const noexcept {
        // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: `localAreaAlignPad` (0 for every
        // non-over-aligned frame) shifts the local base up to its required
        // boundary. Both the alloca-offset progression and `vaRegSaveAreaOffset`
        // derive from this, so the whole topmost frame region moves in lockstep.
        return outgoingArgAreaSize + savedRegAreaSize + spillAreaSize
             + localAreaAlignPad;
    }

    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the variadic register-save-area sits
    // immediately ABOVE the local-alloca area (the topmost frame zone, positive
    // offset from post-prologue SP). The variadic prologue spills the integer arg
    // regs at this offset (gpSlotBytes stride) then the al-gated SSE arg regs after
    // them (fpSlotBytes stride); `va_reg_save_area` materializes to `lea [sp + this]`.
    // Zero-width when this function doesn't call va_start.
    [[nodiscard]] constexpr std::uint32_t
    vaRegSaveAreaOffset() const noexcept {
        // Derives from `localAreaOffset()` so the alignas local-area pad
        // (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN) is included — the va-save area
        // sits immediately above the (possibly padded) local area. A variadic
        // function with an over-aligned local would otherwise place its
        // register-save area `localAreaAlignPad` bytes too low (overlapping the
        // top local slot). Pad is 0 for every non-over-aligned frame → identical.
        return localAreaOffset() + localAreaSize;
    }
};

// One CFI rule change, keyed by the LIR instruction that establishes it.
//
// This is the producer-side intermediate between the callconv materializer
// (which knows WHICH instruction changes the frame, but not where it will land
// in the byte stream) and the assembler (which knows the byte offsets, but not
// what any instruction means). Neither half can produce a PC-keyed `CfiOp`
// alone; the pipeline joins them.
//
// * The alternative -- recovering the prologue's byte offsets downstream by
//   counting instructions or by assuming their encodings -- is what the
//   pre-existing pe64 `.pdata`/`.xdata` builder did, and it is exactly the
//   fragility this replaces. An instruction id is a fact; an assumed byte
//   length is a hypothesis that no longer holds the moment an encoder picks a
//   shorter form.
struct DSS_EXPORT LirCfiOp {
    LirInstId    inst{};   // the instruction AFTER which the rule is in effect
    CfiOpKind    kind  = CfiOpKind::DefCfaOffset;
    CfiRegRef    reg{};
    CfiRegRef    srcReg{};
    std::int64_t offset = 0;
    // When set, the rule takes effect at the END of `block` rather than after
    // `inst`. Exists for exactly one op: the `restore_state` that re-arms the
    // framed rules for the code FOLLOWING a `ret`.
    //
    // * A function with two returns has two epilogues, and the code BETWEEN
    //   them still has a live frame. Without this, the stream would say the
    //   frame was torn down from the first epilogue onward and every unwind
    //   from the second half of the function would read the caller's return
    //   address `totalFrameSize` bytes from where it is. gcc solves it the
    //   same way (`.cfi_remember_state` / `.cfi_restore_state` bracketing each
    //   epilogue); the restore has to land PAST the `ret`, and the block end
    //   is that point -- the epilogue's own last instruction is not, because
    //   the one-byte `ret` sits between them with the frame genuinely gone.
    LirBlockId   block{};
    bool         atBlockEnd = false;
};

// One function's CFI production, plus the one boundary only the materializer
// can state: how many of these ops belong to the PROLOGUE.
//
// * `prologueOpCount` is recorded, not inferred. Win64 `UNWIND_INFO` has a
//   mandatory `SizeOfProlog` field, and a consumer that guessed it -- "the ops
//   before the first body instruction", say -- would be re-deriving, at a
//   distance, a fact the emitter had in hand and threw away. An invented
//   `SizeOfProlog` makes the OS unwinder mis-classify PCs as mid-prologue and
//   unwind a frame that is not there yet.
struct DSS_EXPORT LirFuncCfi {
    std::vector<LirCfiOp> ops;
    std::uint32_t         prologueOpCount = 0;
};

struct DSS_EXPORT LirCallconvResult {
    Lir                       lir{};
    std::vector<FrameLayout>  perFunc;  // 1:1 with src.funcAt(i)
    // 1:1 with `perFunc`: the frame-affecting instructions this function's
    // prologue / frame-pointer capture / epilogues emitted, in emission order.
    // Empty for a function whose frame is entirely absent (a zero-size frame
    // with no callee-saves changes nothing, so it has nothing to say).
    std::vector<LirFuncCfi> perFuncCfi;
    // True iff `materializeCallingConvention` ran to its successful conclusion.
    // Set ONLY at the final return, so EVERY failure early-return (a config /
    // per-function / SEH / VLA-verifier reject — each returns an empty or
    // partial result) leaves it false. Mirrors `LirTwoAddrLegalizeResult::
    // allFunctionsLegalized`. It is load-bearing: a FAILURE that returns an
    // empty module is shape-indistinguishable from a genuinely EMPTY module by
    // the count check alone (both 0 == 0), so the completion flag is what keeps
    // a failed pass fail-loud.
    bool                      allFunctionsLaidOut = false;

    // Derived: true iff the pass COMPLETED and every function got a FrameLayout
    // (the parallel-index invariant `perFunc.size() == moduleFuncCount()`).
    // Matches cycle-3a's `LirAllocation::ok()` discipline (computed on access;
    // cannot drift from per-function results). An EMPTY module (0 functions —
    // a declaration-only / all-preprocessed-out TU) is a VALID success: the
    // pass completes (allFunctionsLaidOut = true) with 0 == 0, so it lowers to
    // a valid empty relocatable object rather than being silently rejected
    // (D-CSUBSET-TESTTU-SILENT-EXIT1). The earlier `moduleFuncCount() > 0`
    // clause wrongly forced ok()==false for the empty case; but merely dropping
    // it would make a FAILURE that returns an empty module read as ok (0 == 0),
    // hence the explicit completion flag.
    [[nodiscard]] bool ok() const noexcept {
        return allFunctionsLaidOut
            && perFunc.size() == lir.moduleFuncCount()
            // The CFI sink rides the SAME parallel-index discipline: a
            // function that got a FrameLayout but no CFI slot would silently
            // reach the writers as "this function has no unwind info", which
            // is indistinguishable from a genuinely frameless leaf.
            && perFuncCfi.size() == perFunc.size();
    }

    // Find the FrameLayout for the given function by position in
    // the OUTPUT module. Returns nullptr if the function index is
    // out of range. (LirFuncId arena tags differ across passes, so
    // positional lookup is the substrate-tier contract.)
    [[nodiscard]] FrameLayout const* forFuncByIndex(std::uint32_t i) const noexcept {
        return (i < perFunc.size()) ? &perFunc[i] : nullptr;
    }
};

// c116 H1 (D-WIN64-SEH-FUNCLETS): one SEH filter-funclet ↔ its guarding parent
// binding, by SymbolId. The funclet's `recover_parent_frame_slot` ops resolve their
// slot offsets against the PARENT's FrameLayout, so callconv must know, for each
// funclet function, which function is its parent. Derived from the `MirSehScope`
// records (funclet + parent symbols) by the compile pipeline and passed to
// `materializeCallingConvention`. Empty for every non-SEH module.
struct DSS_EXPORT SehFuncletParent {
    SymbolId funcletSymbol{};   // the synthesized filter funclet
    SymbolId parentSymbol{};    // the function that guards the __try
};

// D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: one entry per function that has a body-
// local whose EFFECTIVE alignment exceeds the target machine word — computed at
// MIR→LIR (types + the Alloca alignment channel are present there) and keyed by
// SymbolId so it survives the intervening LIR rebuilds. `computeFrameLayout`
// looks each function up by symbol and, when present, aligns the local area to
// the required boundary (or fails loud past the slot-width bound). Empty for a
// module with no over-aligned local (mirrors `FuncLocalAlignment`).
struct DSS_EXPORT LirFuncLocalAlignment {
    SymbolId      funcSymbol{};
    std::uint32_t maxLocalAlignBytes = 0;
    // #2 per-alloca fix: the EFFECTIVE alignment (bytes) of every body-local
    // alloca in scan order (the SAME order the callconv places them). The frame
    // layout aligns each alloca's offset up to ITS OWN alignment — required on
    // arm64 (8-byte slot < 16-byte stack alignment). Length == the function's
    // alloca count (checked loud at consume). 0 in a slot = no over-alignment
    // (alignUp is a no-op). Mirrors `FuncLocalAlignment::perAllocaAlignBytes`.
    std::vector<std::uint32_t> perAllocaAlignBytes;
};

[[nodiscard]] DSS_EXPORT LirCallconvResult
materializeCallingConvention(Lir const&           src,
                             TargetSchema const&  schema,
                             LirAllocation const& alloc,
                             DiagnosticReporter&  reporter,
                             // c116 H1 (D-WIN64-SEH-FUNCLETS): the funclet→parent
                             // bindings so a funclet's `recover_parent_frame_slot`
                             // ops resolve against the parent's finalized layout.
                             // Empty for every non-SEH module (the default).
                             std::span<SehFuncletParent const> sehFuncletParents = {},
                             // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: per-function
                             // max local alignment (SymbolId-keyed). Empty for a
                             // module with no over-aligned local.
                             std::span<LirFuncLocalAlignment const> funcLocalAlignments = {});

} // namespace dss
