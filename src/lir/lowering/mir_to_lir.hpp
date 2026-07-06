#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"  // ExternCallDispatch (extern-call shape)
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "mir/merge/synth_seh_funclets.hpp"   // MirSehScope (c116 D-WIN64-SEH-FUNCLETS)
#include "mir/mir.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// MIR → LIR instruction selection (plan 12 ML5 cycle 3). Takes a frozen
// MIR module plus the chosen `TargetSchema` (the cycle-2b-shaped JSON
// descriptor) and produces a frozen `Lir` module: the SAME function/block
// CFG topology, but per-block MIR instructions rewritten as per-target LIR
// instructions. Block-for-block 1:1 (no block splitting at this layer);
// MIR values map to fresh LIR virtual registers per-function. Physical
// register assignment + spilling happen later in ML6 regalloc.
//
// First consumer of the cycle-2b `TargetSchema` opcode vocabulary. All
// opcode dispatch goes through `schema.opcodeByMnemonic("add")` etc.;
// nothing in the lowerer hardcodes processor names. Adding ARM64 = drop
// `arm64.target.json` declaring `arg`/`mov`/`add`/`sub`/`mul`/`ret`
// mnemonics; the lowerer is target-blind. (The register-file + calling-
// convention sections of `TargetSchema` are the next-tier consumers —
// ML6 regalloc + ML7 callconv lowering. Cycle 3a does not yet read them.)
//
// Cycle 3a scope (this revision): straight-line vertical slice — Function +
// Block + Arg + Const + Add + Sub + Mul + Return. Control flow (Br/CondBr/
// Switch), comparison (ICmp*/FCmp*), memory (Alloca/Load/Store/Gep),
// calls (Call/IntrinsicCall/GlobalAddr), phi, casts, and aggregate ops
// are deliberately fail-loud-deferred via `L_UnsupportedLoweringForOpcode`
// — same discipline as ML2 cycle 1's HIR→MIR vertical slice.

namespace dss {

// Output of MIR→LIR lowering. `ok` mirrors ML2's delta-on-errorCount —
// `true` iff this lowering pass added no new error-severity diagnostics.
// `lir` is the frozen module the assembler (AS1 onward) will consume.
//
// `lirToMir` is a substrate-tier reverse-mapping (LirInstId → MirInstId)
// the lowerer populates as it emits LIR instructions. The vector is
// indexed by `LirInstId.v` (slot 0 is the arena's invalid sentinel,
// already-default-`InvalidMirInst`-initialized). Multiple LIR insts
// may map to the same source MIR inst (cycle-3b Switch lowering emits
// 2N+1 LIR blocks per MIR Switch; cycle-3c memory ops emit Load
// followed by additional address-mode insts); some LIR insts have no
// MIR counterpart (Switch's "next-compare" blocks, phi-edge parallel-
// copy moves), in which case the entry is the default `InvalidMirInst`.
//
// `LirVerifier` consumes this mapping to cross-reference LIR vreg
// classes against MIR types WITHOUT the cycle-3e positional-alignment
// hazard that silently skipped switch-bearing functions.
// D-OPT-SWITCH-JUMP-TABLE (c70): one descriptor per DENSE `switch` the lowerer
// turned into an O(1) jump table. The switch-site LIR (bounds check + indexed
// load of a code address + indirect branch) is already emitted into the LIR; a
// descriptor carries only what the assembler-adjacent pipeline needs to
// MATERIALIZE the `.data` address table after `assemble()` has resolved each
// block's byte offset. The table is `slotCount` 8-byte slots — one per value in
// `[minCase, maxCase]` — each an abs64 relocation to the synthetic per-block
// symbol of that value's target block (the default block for a gap). The block
// symbols are minted by the SAME `mintBlockSymbol` sequence the computed-goto
// `&&label` path uses (so they are unique module-wide and get an interior-block
// VA via `link/format/interior_block_symbol_va.hpp`), but — unlike computed-goto
// — no live block-address `lea` binds them, so the pipeline binds each directly
// from the assembled function's `blockByteOffsets` map.
struct DSS_EXPORT JumpTableDescriptor {
    SymbolId    tableSymbol{};    // unique `.data` symbol for this table
    std::size_t slotCount = 0;    // (maxCase - minCase + 1) 8-byte slots
    std::size_t funcIndex = 0;    // index into lir.funcAt(i) of the owning function
    // slot j (0-based, value == minCase + j) → the LIR block its code address
    // occupies. Gaps carry the default block. Parallel to the emitted table's
    // slots; the pipeline writes an abs64 reloc at byte (j * 8) targeting
    // `blockSymbols[lirBlock.v]`.
    std::vector<std::pair<std::uint32_t /*lirBlock.v*/, std::size_t /*slotIndex*/>>
        slotBindings;
    // Each distinct target LIR block (`lirBlock.v`) → its synthetic per-block
    // SymbolId (minted via mintBlockSymbol; deduped by the MIR block id, so
    // duplicate case targets and every gap-to-default share one symbol).
    std::unordered_map<std::uint32_t, SymbolId> blockSymbols;
};

// c78 (D-CSUBSET-FLOAT-NEG-ENCODING): one descriptor per x86-style float-negate
// site the LIR lowerer realized as `xorpd/xorps xmm, [rip+mask]` (a target
// WITHOUT a native `fneg` opcode). Like a JumpTableDescriptor it carries only
// what the assembler-adjacent pipeline needs to MATERIALIZE the constant: a
// 16-byte, 16-byte-aligned `.rodata` item under `symbol`, whose low bytes hold
// the sign bit (bit 63 for F64 / bit 31 for F32) and whose high bytes are zero.
// The XORPS/XORPD memory operand must be 16-byte aligned at runtime; the item's
// 16-byte `Alignment` + the section-alignment layout (ELF sh_addralign; PE 4 KiB
// sectionAlignment, a multiple of 16) guarantee it on all three legs. Minted
// per-occurrence (no dedup — mirrors the jump-table + string/float-literal
// producers); a native-fneg target (arm64) emits NONE of these.
struct DSS_EXPORT SignMaskConstant {
    SymbolId symbol{};        // unique `.rodata` symbol for this mask
    bool     isF64 = true;    // true → F64 mask (bit 63); false → F32 mask (bit 31)
};

// c116 (D-WIN64-SEH-FUNCLETS): one descriptor per MSVC-x64 `__try` region the
// lowerer saw (via a `SehTryBegin` marker in the parent function). Like a
// JumpTableDescriptor it carries only what the assembler-adjacent pipeline needs
// AFTER `assemble()` resolves each block's byte offset: the LIR block ids of the
// guarded body's [entry, exit) and the `__except` handler, plus the symbols the
// pe writer resolves to image-RVAs (the filter funclet + the __C_specific_handler
// personality). `compile_pipeline.cpp` translates the LIR block ids to byte
// offsets against the owning function's `blockByteOffsets` and attaches a
// `SehScopeEntry` to that function's `FrameUnwindInfo.sehScopes`; the pe writer
// then emits the scope table + UNW_FLAG_EHANDLER. The SEH markers themselves
// (`SehTryBegin`/`SehTryEnd`/`SehFilterReturn`) emit NO runtime branch — the OS
// dispatches into the handler via the scope table, so these ids are pure position
// data (the c114 .pdata + c70 jump-table link-time-RVA pattern).
struct DSS_EXPORT SehScopeDescriptor {
    std::size_t   funcIndex        = 0;  // index into lir.funcAt(i) of the owning (parent) function
    std::uint32_t beginLirBlockV   = 0;  // LIR block .v of the guarded body's entry (try block)
    std::uint32_t endLirBlockV     = 0;  // LIR block .v marking one-past the guarded body (the range end)
    std::uint32_t handlerLirBlockV = 0;  // LIR block .v of the __except handler body
    SymbolId      filterFuncletSymbol{}; // the synthesized filter-funclet function symbol
    SymbolId      personalitySymbol{};   // __C_specific_handler extern symbol
};

struct DSS_EXPORT MirToLirResult {
    Lir                    lir;
    std::vector<MirInstId> lirToMir;
    // D-OPT-SWITCH-JUMP-TABLE (c70): one descriptor per dense `switch` lowered to
    // a jump table. Consumed by `compile_pipeline.cpp` AFTER `assemble()` to emit
    // each table's `.data` `AssembledData` (abs64 relocs to block symbols) and to
    // bind those block symbols from the assembled function's `blockByteOffsets`.
    std::vector<JumpTableDescriptor> jumpTableDescriptors;
    // c78 (D-CSUBSET-FLOAT-NEG-ENCODING): one entry per x86-style float-negate
    // site the lowerer realized as `xorpd/xorps xmm, [rip+mask]`. Consumed by
    // `compile_pipeline.cpp` to emit each mask's 16-byte, 16-byte-aligned
    // `.rodata` `AssembledData`. Empty on a native-fneg target (arm64).
    std::vector<SignMaskConstant> signMaskConstants;
    // c116 (D-WIN64-SEH-FUNCLETS): one descriptor per `__try` region. Consumed by
    // `compile_pipeline.cpp` AFTER `assemble()` to attach a `SehScopeEntry` to the
    // owning function's `FrameUnwindInfo.sehScopes` (byte offsets from that
    // function's `blockByteOffsets`). Empty for every module without a `__try`.
    std::vector<SehScopeDescriptor> sehScopeDescriptors;
    // Extern symbol descriptors propagated from `HirToMirResult.
    // externImports` (LK6 cycle 2d — D-LK6-6 closure). LIR does
    // not consume these structurally (call sites carry SymbolRef
    // operands keyed by SymbolId, the same shape as intra-module
    // calls), but the assembler needs them to populate
    // `AssembledModule.externImports`, so we propagate verbatim.
    std::vector<ExternImport> externImports;
    bool                   ok = true;
};

// Lower the frozen `mir` module to LIR, dispatched against `target`.
// Diagnostics are emitted into `reporter`; unsupported opcodes produce
// `L_UnsupportedLoweringForOpcode` and the lowerer seals the affected
// block with a `ret` terminator so `LirBuilder::finish()` does not abort
// in error paths.
//
// `interner` is the CU's type interner — cycle 3d uses it to classify
// MIR values into `LirRegClass` (Float/Double → FPR, everything else
// → GPR), driving correct vreg allocation for float arithmetic, float
// casts, and cross-class Bitcast. Read-only here (the lowerer never
// mints new types); matches the pattern in `lowerToMir` from ML2.
//
// Threading: single-pass, single-threaded, no global state. The caller
// owns `mir` + `target` + `interner` + `reporter`; the returned `Lir`
// is move-owned.
[[nodiscard]] DSS_EXPORT MirToLirResult
lowerToLir(Mir const&          mir,
           TargetSchema const& target,
           TypeInterner const& interner,
           DiagnosticReporter& reporter,
           // Extern symbols extracted by HIR→MIR lowering
           // (`HirToMirResult.externImports`). Propagated verbatim
           // through the returned `MirToLirResult.externImports`.
           // **Also CONSUMED structurally** by `lowerCall` (post-fold
           // 2026-06-02, D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY closure):
           // a call whose GlobalAddr target's SymbolId is in this
           // list lowers to `call_indirect_via_extern` (FF 15 disp32
           // — dereferences the IAT/GOT slot); a call whose target
           // is module-internal lowers to `call` (E8 disp32 — direct
           // rel32). Defaults to empty for static modules.
           std::vector<ExternImport> externImports = {},
           // D-FFI-EXTERN-CALL-DISPATCH: the ACTIVE OBJECT FORMAT's
           // extern-call shape, read from `ObjectFormatSchema::
           // externCallDispatch()`. `indirect-slot` (PE/Mach-O) →
           // extern calls lower to `call_indirect_via_extern` (deref
           // the IAT/__got pointer slot); `direct-plt` (ELF) → extern
           // calls lower to the plain `call` opcode (direct branch to
           // the linker-synthesized PLT stub). `std::nullopt` = the
           // format declared none: lowering an extern call then fails
           // loud (NO silent default — picking the wrong shape
           // miscompiles, e.g. FF 15 through an ELF PLT stub SIGSEGVs).
           // Defaults to nullopt; the production driver passes the
           // active format's value, and only extern-bearing modules
           // consume it (a static module never reaches the guard).
           std::optional<ExternCallDispatch> externCallDispatch =
               std::nullopt,
           // c116 (D-WIN64-SEH-FUNCLETS): the SEH scope records produced by
           // `synthesizeSehFunclets` (keyed by the REBUILT module's parent MIR
           // block ids). Each is translated to LIR block ids + emitted as a
           // `SehScopeDescriptor` on the result. Empty for every module without a
           // `__try`; a non-SEH module never reaches the translation.
           std::span<MirSehScope const> sehScopes = {});

} // namespace dss
