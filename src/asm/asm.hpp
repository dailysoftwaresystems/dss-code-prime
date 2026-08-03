#pragma once

#include "core/export.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/alignment.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Assembler — plan 13 AS1.
//
// Universal byte-encoding pass: walks a post-callconv `Lir` module and
// emits machine bytes per the target schema's `encoding` facet. The
// dispatch is **shape-keyed** (`TargetEncodingShape`) per plan 13 §2.4:
// one walker per shape, NOT one walker per arch. New target = drop a
// `*.target.json` declaring the shape; new shape = one new walker.
//
// Cycle 1 ships the substrate only — the dispatch shell, the output
// types, the format-walker registry hook, and the `A_*` diagnostic
// family. Cycle 2 plugs in `x86-variable`; cycle 3 plugs in `fixed32`.
// Until those land, every non-`None` opcode arriving at the assembler
// fires `A_NoEncodingShapeWalker` (the walker is declared by the
// schema but the assembler has no implementation registered yet); a
// `None`-shape opcode fires `A_NoEncodingDeclared`. Both behaviors are
// the intentional fail-loud substrate — there is never a silent skip.
//
// LIR contract (consumed input):
//   * Every vreg has been replaced by a physical register
//     (post-regalloc; ML6 cycle 3 invariant).
//   * Prologue/epilogue + frame_load/frame_store materialized
//     (ML7 callconv lowering invariant).
//   * Calls have either a resolved direct target (intra-CU) or carry a
//     symbol reference that produces a relocation at assemble time.
//     Cycle 1 substrate doesn't emit relocations yet.
//
// Output contract (produced):
//   * One `AssembledFunction` per LIR function, parallel-indexed with
//     `lir.funcAt(i)` — the same `ok()`-derived discipline pioneered
//     by `LirAllocation::ok()` (ML6 cycle 3a) and adopted by
//     `LirCallconvResult::ok()` (ML7 cycle 1). `AssembledModule::ok()`
//     checks `functions.size() == expectedFuncCount` (not bare
//     non-emptiness — a partial run that drops functions silently is
//     exactly what the cycle-3a pattern was invented to catch).
//   * Each function's `bytes` is the section's machine code; `relocations`
//     names the unresolved symbol references the linker (plan 14) will
//     resolve; `sourceMap` records `byteOffset → MirInstId` (NOT
//     LirInstId — those don't survive the ML7 rewrite, per the
//     architect-2 review). AS2/AS3 populate these vectors when their
//     format walkers light up; cycle 1 leaves them empty.
//   * `AssembledFunction::symbol` is populated from `lir.funcSymbol(fn)`
//     so the linker (plan 14) can place each function's bytes in its
//     object-file symbol table WITHOUT holding a parallel reference to
//     the upstream `Lir` (mirrors `LirFuncAllocation::originalSymbol`).

namespace dss {

// Source-position back-link from assembled bytes to MIR (the universal
// debug-info anchor — LIR ids don't survive ML7's pass-rewriting, so
// the byte offset is keyed by the original MIR instruction).
struct DSS_EXPORT SourceMapEntry {
    std::uint32_t byteOffset;
    MirInstId     mirInst;
};

// One relocation — an unresolved symbol reference the linker will
// patch. `kind` is the opaque tag declared by `TargetSchema::
// relocations[]` (the bucket-1 reloc taxonomy facet). The assembler
// writes the tag; the linker reads `schema.relocationInfo(kind)` to
// dispatch the formula. `offset` width matches `SourceMapEntry::
// byteOffset` so both indices into `AssembledFunction::bytes` agree
// on the max section size (consistent with ELF/COFF section-relative
// reloc offsets being u32 in the wire format).
struct DSS_EXPORT Relocation {
    std::uint32_t  offset = 0;    // byte offset within the function's bytes
    SymbolId       target{};      // the unresolved symbol the linker resolves
    RelocationKind kind{};        // opaque tag — value obtained from
                                  // `schema.relocationByName(...)->kind` or
                                  // `schema.relocations()[i].kind`; never
                                  // assembler-fabricated. Default-constructed
                                  // (invalid sentinel) means "uninitialized".
    std::int64_t   addend = 0;    // ABI-specific (e.g. PC-relative bias)
};

// One synthetic SYMBOL ↔ interior-block byte-offset binding (D-CSUBSET-
// COMPUTED-GOTO `&&label`). The block-address `lea` materializes a
// SYNTHETIC per-block local symbol's runtime address; that symbol is
// not a function or a data item, so it gets no VA from the usual
// function/data symbolVa population. Instead the encoder records this
// binding — `symbol` is the synthetic local, `blockByteOffset` is the
// target block's offset WITHIN THIS function's `bytes` — and the
// linker computes the symbol's interior VA as `sectionVa +
// funcTextStart[thisFn] + blockByteOffset` (the same geometry a
// function symbol gets, but pointing at an interior block rather than
// the function entry). `addend` would otherwise have to carry the
// block offset off a function symbol; a synthetic own-VA symbol keeps
// `addend == 0` so the EXISTING rel32 / adrp / add_lo12 reloc formulas
// resolve it with no kernel change.
struct DSS_EXPORT SyntheticBlockSymbol {
    SymbolId      symbol{};            // the synthetic per-block local symbol
    std::uint32_t blockByteOffset = 0; // offset of the target block within THIS fn's bytes
};

// c114 (D-WIN64-PDATA-XDATA-UNWIND): a FORMAT-NEUTRAL projection of a
// function's frame prologue — just enough for an unwind-table emitter to
// describe the frame WITHOUT depending on `lir/` (AssembledFunction has
// zero lir/ coupling by design). Populated by the compile pipeline from
// the callconv pass's `FrameLayout`; consumed today ONLY by the pe64
// writer's `.pdata`/`.xdata` builder, but the data is generic (ELF
// `.eh_frame` / Mach-O compact-unwind are legitimate future consumers).
//
// The canonical DSS x86_64 prologue is `sub rsp, totalFrameSize` (or, for
// `totalFrameSize > stackProbePageBytes`, the inline page-probe loop) then
// one `mov [rsp + saveOffset], reg` per USED callee-saved register — no
// push, no frame pointer. `savedRegs` is in emission (ascending-offset)
// order; each entry carries the platform register number + class so the
// emitter can pick UWOP_SAVE_NONVOL (GPR) vs UWOP_SAVE_XMM128 (FPR).
struct DSS_EXPORT FrameSavedReg {
    std::uint16_t regEncoding = 0;   // the register's platform ordinal (x64: rax=0..r15=15; xmm N = N)
    bool          isFpr       = false;  // FPR/vector class ⇒ 16-byte XMM save, else 8-byte GPR
    std::uint32_t saveOffset  = 0;   // byte offset from post-prologue RSP where the reg is stored
};
// c116 (D-WIN64-SEH-FUNCLETS): one MSVC-x64 SEH scope,
// as byte offsets WITHIN the owning (parent) function's `bytes` + the SymbolIds
// the pe writer resolves to image-RVAs at link time. Produced by the compile
// pipeline from the LIR `SehScopeDescriptor` (which carries LIR block ids) by
// translating each block id to its byte offset via the parent AssembledFunction's
// `blockByteOffsets` map (the c70 jump-table binding pattern). Consumed ONLY by
// the pe64 writer's `.xdata` scope-table emitter (the __C_specific_handler scope
// table + UNW_FLAG_EHANDLER — format-specific, so it lives entirely in pe.cpp;
// this struct is format-neutral position data). The guarded PC range is
// `[beginByteOffset, endByteOffset)`; a fault in it dispatches to the filter
// funclet (`filterFuncletSymbol`), and on EXECUTE_HANDLER the OS resumes at
// `jumpTargetByteOffset` (the `__except` body, in the parent frame).
struct DSS_EXPORT SehScopeEntry {
    std::uint32_t beginByteOffset      = 0;  // start of the guarded body (within parent bytes)
    std::uint32_t endByteOffset        = 0;  // one-past the guarded body (half-open range)
    std::uint32_t jumpTargetByteOffset = 0;  // the __except handler block (within parent bytes)
    SymbolId      filterFuncletSymbol{};     // the synthesized filter-funclet function symbol
    SymbolId      personalitySymbol{};       // __C_specific_handler extern (its thunk RVA = the UNWIND handler field)
};
struct DSS_EXPORT FrameUnwindInfo {
    std::uint32_t              totalFrameSize = 0;  // bytes the prologue subtracts from RSP
    bool                       usesStackProbe = false;  // frame > cc.stackProbePageBytes ⇒ the inline probe loop
    std::uint32_t              stackProbePageBytes = 0; // the cc's guard-page size (probe stride); 0 = no probing
    std::vector<FrameSavedReg> savedRegs;           // callee-saved regs stored in the prologue, ascending saveOffset
    // c116 (D-WIN64-SEH-FUNCLETS): the SEH scopes this function guards. Empty for
    // every function without a `__try` (the overwhelming majority). Non-empty ⇒
    // the pe writer sets UNW_FLAG_EHANDLER + appends the __C_specific_handler
    // scope table to this function's UNWIND_INFO.
    std::vector<SehScopeEntry> sehScopes;
};

// One assembled function — bytes + symbol-relative metadata. `symbol`
// is sourced from the originating `lir.funcSymbol(fn)` so the linker
// can place this function's bytes in its object-file symbol table
// without re-consulting the upstream `Lir`.
struct DSS_EXPORT AssembledFunction {
    SymbolId                    symbol{};
    std::vector<std::uint8_t>   bytes;
    std::vector<Relocation>     relocations;
    std::vector<SourceMapEntry> sourceMap;
    // c114 (D-WIN64-PDATA-XDATA-UNWIND): the frame prologue projection an
    // unwind-table emitter needs (see FrameUnwindInfo). nullopt when the
    // pipeline did not attach it (object-file `.obj` path, or a producer
    // that skips frame layout) — the pe64 exec writer then emits no
    // unwind entry for this function (a leaf/frameless entry).
    std::optional<FrameUnwindInfo> unwind;
    // D-CSUBSET-COMPUTED-GOTO: synthetic per-block symbols this
    // function's block-address `lea`s reference, each paired with the
    // target block's byte offset within `bytes`. The encoder records a
    // pending `walker_util::BlockSymPatch` per block-address `lea`;
    // `asm.cpp` resolves each against the function's completed
    // `blockOffsets` table and appends a `SyntheticBlockSymbol` here.
    // The linker turns each into an interior-block VA in `symbolVa`
    // before applying relocations (see
    // `link/format/interior_block_symbol_va.hpp`). Empty for every
    // function that takes no block address.
    std::vector<SyntheticBlockSymbol> blockSymbols;

    // D-OPT-SWITCH-JUMP-TABLE (c70): this function's block-byte-offset table —
    // each LirBlockId.v mapped to its byte offset within `bytes`. Populated by
    // `assemble()` from the SAME `blockOffsets` map it builds for intra-function
    // branch patching. A dense `switch` lowers to an O(1) jump table whose
    // rodata-style `.data` slots hold the runtime addresses of the case-target
    // blocks (via abs64 relocations to synthetic per-block symbols). Those block
    // symbols have NO live block-address `lea` instruction (the table is one
    // indexed load, not a `lea` per target), so the usual `BlockSymPatch` →
    // `blockSymbols` binding never fires for them. `compile_pipeline.cpp` instead
    // binds each jump-table target block's symbol directly from THIS map (the
    // byte offset the linker needs to compute the interior-block VA) and appends
    // the resulting `SyntheticBlockSymbol` to `blockSymbols` above. Empty for
    // every function that contains no jump table (this map is populated
    // unconditionally but only consumed when a jump-table descriptor names this
    // function — a per-function map copy done once at assemble time, never per
    // instruction).
    std::unordered_map<std::uint32_t, std::uint32_t> blockByteOffsets;
};

// One assembled data item — bytes + symbol identity, destined for a
// non-executable section in the output object file (typically PE
// `.rdata`, ELF `.rodata`, Mach-O `__cstring`/`__const`). Mirrors
// `AssembledFunction`'s symbol-keyed shape so the linker's symbol→
// VA resolution treats functions and data uniformly.
//
// **Bucket discipline**: this struct is target-blind and format-blind
// — the bytes are whatever the upstream producer (MIR global
// initializer, HIR string-literal promotion) decided; the format
// walker decides which on-disk section receives them based on
// `section`. Adding a new data category that is NOT already in the
// `SectionKind` enum (e.g. `TlsData` for thread-local storage,
// `InitArray` for static-init function pointer tables) is a new
// enum value + walker arm, NOT a new struct.
//
// `alignment` is the byte alignment the producer requires (e.g. 1
// for C-string concatenation; 8 for pointer-aligned tables; 16 for
// SSE vectors). The walker pads between items to satisfy each
// item's alignment, then aligns the section itself to the format's
// section-alignment requirement. Alignment=1 is the safe default
// (byte-string layout).
//
// `relocations` carry references from THIS data into other symbols
// (e.g. a vtable pointing at functions, a const struct containing a
// pointer field). Same shape as `AssembledFunction.relocations` —
// the reloc's `offset` is relative to the START OF THIS ITEM, the
// walker translates to section-relative when emitting:
//
//     section_offset(item_N) = Σ aligned_size(items[0..N-1])
//
// where `aligned_size(item)` rounds `item.bytes.size()` up to the
// NEXT item's `alignment`. A walker that uses the per-item `offset`
// verbatim against the section base would silently mis-resolve
// every relocation in `items[1..]`.
// Anchored: `D-LK4-RODATA-WALKER-RELOC-BASE-OFFSET` (sub-anchor
// of `D-LK4-RODATA-SUBSTRATE`) — test at the first walker-arm
// landing pins the Σ translation correctness.
//
// D-LK4-RODATA-SUBSTRATE: substrate-only landing (no MIR→LIR
// producer yet). Hand-built `AssembledData` items exercise the
// walker emission. The MIR-global → AssembledData thread-through
// + the assembler's RIP-rel `lea` encoding variant + the HIR
// string-literal-to-global promotion are anchored as follow-up
// cycles toward FF6 (plan 11) hello-world.
struct DSS_EXPORT AssembledData {
    SymbolId                  symbol{};
    DataSectionKind           section = DataSectionKind::Rodata;
    std::vector<std::uint8_t> bytes;
    Alignment                 alignment;  // default = 1-byte
    std::vector<Relocation>   relocations;
    // `reservedSize` — the in-memory byte size for a ZERO-FILL item
    // (`Bss` / `Tbss` — `isZeroFill(section)`), where `bytes` is EMPTY by
    // invariant (the wire format reserves the size in the section header
    // without storing file bytes). For file-backed items (`Rodata`/`Data`/
    // `Tdata`) the on-disk + in-memory size is `bytes.size()` and this field
    // is unused (0). A tentative global `int g;` produces a `Bss` item with
    // `bytes.empty()` and `reservedSize == sizeof(int)`; a `thread_local
    // int g;` produces the same shape with section=Tbss. D-LK4-DATA-PRODUCER
    // (BSS arm) + D-CSUBSET-THREAD-LOCAL (Tbss arm).
    std::uint64_t             reservedSize = 0;

    // The number of bytes this item occupies in its section / VA span — the
    // file-backed `bytes.size()` for Rodata/Data/Tdata, the zero-fill
    // `reservedSize` for Bss/Tbss. The single chokepoint every walker uses to
    // advance section layout offsets; the zero-fill/file-backed split lives
    // in the ONE shared `isZeroFill` predicate (section_kind.hpp — TLS C1
    // audit fold M-3: an exact `== Bss` test here would have silently sized
    // every Tbss item at bytes.size()==0).
    [[nodiscard]] std::uint64_t sizeInSection() const noexcept {
        return isZeroFill(section)
                   ? reservedSize
                   : static_cast<std::uint64_t>(bytes.size());
    }
};

// Validate a span of `AssembledData` items against the substrate
// invariants. Emits one diagnostic per violation; returns true iff
// every item is well-formed. Called by the linker before walker
// dispatch — see `linker.cpp`.
//
// Invariants enforced (closes D-LK4-RODATA-BSS-INVARIANT +
// duplicate-SymbolId + zero-alignment guards from the 3rd-order
// audit):
//
//   1. `bytes.empty() iff section == Bss` — `Bss` is zero-fill
//      and the wire format reserves `sh_size` without storing
//      bytes; a non-empty `Bss` item would either silently
//      embed those bytes (defeating BSS's no-file-footprint
//      property) or silently drop them.
//   2. No two items share the same `SymbolId` — the linker
//      resolves relocations against `SymbolId`, so duplicates
//      would silently let "whichever was processed last" win.
//      `SymbolId{}` (the invalid sentinel) is exempt from
//      duplicate-checking — sentinel items signal "no symbol
//      identity needed", typically for read-only padding /
//      anonymous constants. Multiple sentinel items are
//      legitimate.
//   3. `Alignment` is power-of-two ≥ 1 — already enforced
//      structurally by the `Alignment` newtype (the type CAN'T
//      hold non-power-of-two). This invariant is documented
//      here for the audit trail.
//
// Diagnostic codes emitted (all unsuppressable):
//   * `K_SymbolUndefined` — duplicate `SymbolId` (an item's
//      identity clashes with another).
//   * `K_NoMatchingObjectFormat` — Bss + non-empty bytes
//      (substrate-shape violation; the matching walker arm
//      can't route this to any meaningful section).
[[nodiscard]] DSS_EXPORT bool
validateAssembledData(std::span<AssembledData const> items,
                      DiagnosticReporter& reporter);

// One assembled module — parallel-indexed with the LIR input. Mirrors
// the `ok()`-derivation discipline pioneered by `LirAllocation::ok()`
// (ML6 cycle 3a) and adopted by `LirCallconvResult::ok()` (ML7 cycle
// 1): the parallel-index invariant (`functions.size() ==
// expectedFuncCount`) is the structural success criterion, separate
// from per-instruction encoding errors (which the reporter owns).
//
// `expectedFuncCount` is populated by `assemble()` from
// `lir.moduleFuncCount()`. It IS stored — that mirrors how `LirCallconv
// Result::ok()` reads `lir.moduleFuncCount()` from a `Lir` reference
// the result holds — but `AssembledModule` is deliberately Lir-free
// (the linker consumes it without the Lir), so the count rides along.
// One extern symbol the assembler emitted Relocations against
// (declared in the source via `extern T fn(...)`, lowered through
// HIR `ExternFunction` / `ExternGlobal` → MIR / LIR → assembler).
// `symbol` matches `Relocation::target` for every reloc that
// references this extern; the linker (plan 14 LK6 cycle 2)
// consults `externImports` AFTER `functions` when resolving a
// reloc target — defined symbols win; if not defined, externs
// are looked up; if still unresolved, `K_SymbolUndefined` fires.
//
// `mangledName` is the on-binary symbol name the import-table
// entry MUST carry verbatim (e.g. "printf" on Linux/ELF; "printf"
// on x86_64 PE; "_printf" on legacy Mach-O i386). Per-platform
// underscoring belongs upstream (plan 11 §2.5); the assembler
// stamps whatever the HIR/MIR/LIR thread-through provided.
//
// `libraryPath` names the dynamic library that owns this symbol
// (e.g. "kernel32.dll" / "msvcrt.dll" on Windows; "libc.so.6" on
// Linux; "/usr/lib/libSystem.B.dylib" on macOS). Multiple
// externs with the SAME `libraryPath` share one PE
// IMAGE_IMPORT_DESCRIPTOR / ELF DT_NEEDED entry / Mach-O
// LC_LOAD_DYLIB load command. The linker groups by this field.
//
// The HIR→AssembledFunction thread-through that populates
// these fields is anchored at plan 14 §3.1 D-LK6-6 paired with
// plan 11 FF5 (`ingest()` populating HirAttribute<FfiMetadata>);
// LK6 cycle 2a accepted hand-constructed `externImports` for
// substrate tests; LK6 cycle 2d (D-LK6-6 closed) hoists the row to
// `core/types/extern_import.hpp` and threads it from HIR via
// `HirAttribute<FfiMetadata>` through MIR/LIR/assembler.

// LK11a (cross-CU symbol resolution): the per-module symbol table the linker
// matches by NAME across CUs. `SymbolId` is per-CU (per-arena), so two CUs can
// mint the same integer for different symbols; cross-CU resolution must match by
// the DECLARED NAME, which the numeric IR does not carry (MirFunc / MirGlobal hold
// only SymbolId + binding/visibility). The compile pipeline emits one entry per
// DEFINED function / global — NOT extern imports (those are references, carried in
// `AssembledModule::externImports`). The linker builds a global name table from the
// Global/Weak entries (Local stays module-private) and applies weak-vs-strong.
struct DSS_EXPORT ModuleSymbol {
    SymbolId         symbol{};                           // the per-CU SymbolId (compound-key half)
    std::string      name;                               // declared identifier — the cross-CU match key
    SymbolBinding    binding    = SymbolBinding::Global; // Local: module-private; Global/Weak: enter the global table
    SymbolVisibility visibility = SymbolVisibility::Default;
};

struct DSS_EXPORT AssembledModule {
    std::vector<AssembledFunction> functions;  // parallel-index with lir.funcAt(i)
    std::size_t                    expectedFuncCount = 0;

    // D-LK4-3 (cross-CU symbol namespacing): the CompilationUnit this module was
    // assembled from. `SymbolId` is per-arena (per-CU), so when the linker holds
    // multiple modules (LK11) two CUs can mint the same integer for DIFFERENT
    // symbols — the linker keys its symbol index by `(cuId, SymbolId)` to keep them
    // distinct. Default-invalid; the compile pipeline stamps it from the owning
    // CompilationUnit. Single-CU builds (every shipped corpus today) carry one cuId.
    CompilationUnitId              cuId{};

    // LK11a: the per-module symbol table (one ModuleSymbol per DEFINED function /
    // global; extern imports are NOT here — see `externImports`). The linker uses it
    // for cross-CU NAME matching + weak-vs-strong resolution. Populated by the
    // compile pipeline from the semantic symbol table (name) + MIR (binding /
    // visibility). Empty for hand-built substrate modules that don't exercise it.
    std::vector<ModuleSymbol>      symbols;

    // FFI extern imports (LK6 cycle 2 — closes D-LK6-2 partial).
    // Populated by the assembler from upstream HIR `ExternFunction`
    // / `ExternGlobal` declarations once the HIR→AS thread-through
    // lands (D-LK6-6). Linker consults this AFTER `functions` for
    // any unresolved `Relocation::target`.
    std::vector<ExternImport>      externImports;

    // Read-only / initialized / zero-fill data items (D-LK-RODATA-
    // SUBSTRATE). Each entry carries a SymbolId, the section it
    // belongs to (typically `SectionKind::Rodata`), the raw bytes,
    // and an alignment hint. The linker walker concatenates items
    // sharing a `section` (with per-item alignment padding), emits
    // the resulting bytes as the format's matching on-disk section,
    // and maps each item's `symbol` to its section-relative address
    // for relocation resolution.
    //
    // Substrate-only at this slice: hand-built `AssembledData`
    // items exercise the walker emission. The MIR-global →
    // AssembledData thread-through (`hir_to_mir.cpp` string-literal
    // promotion → `mir_to_lir.cpp` global-data routing → assembler
    // `dataItems` populate) is anchored as a follow-up cycle.
    std::vector<AssembledData>     dataItems;

    // D-LK10-ENTRY Slice C (plan 14 §2.13): override of the image
    // entry-point function index. When set, the format walker
    // (PE/ELF/Mach-O) uses `functions[*imageEntryOverride]` as the
    // image entry instead of falling back to the schema's
    // `entryPoint` string resolution.
    //
    // Set by the linker's `injectEntryTrampoline()` after prepending
    // a synthetic `_start` trampoline as `functions[0]` — value is
    // `0u`. The format-schema-declared `entryPoint` continues to
    // name the USER fn (the trampoline's call target), NOT the
    // image entry.
    //
    // The field is `std::optional<std::size_t>`, NOT a `size_t`
    // with 0-as-sentinel: index 0 IS a valid override target (the
    // trampoline genuinely sits at `functions[0]`). Using 0 alone
    // as "unset" would collide with the valid-0 case — same
    // `Unknown=0-vs-valid-0` trap LK4 cycle-1 review already
    // caught.
    std::optional<std::size_t>     imageEntryOverride;  // INDEX into functions[]

    // D-CSUBSET-MULTI-FN-WIN64-CC (step 13.5 cycle 2 post-fold,
    // 2026-06-03): explicit USER entry-function symbol — set by the
    // compile pipeline based on the source language's entry-function
    // name config (e.g. c-subset's "main"). Distinct from
    // `imageEntryOverride` which names the IMAGE entry (= the
    // injected trampoline at functions[0]); this field names the
    // FUNCTION the trampoline calls.
    //
    // Pre-fix, the trampoline injector resolved the user entry via
    // `resolveUserEntrySymbol` which fell back to `functions[0]`
    // when the format JSON's `entryPoint` was empty (default for
    // every shipped format pending D-LK1-1's real-symbol-name
    // thread). With multi-function modules where the entry isn't
    // first in source order, this resolved to the WRONG function
    // — e.g. `int helper(int x){return x;}; int main(){return
    // helper(42);}` had main calling helper correctly, but the
    // trampoline called HELPER instead of main, returning rcx
    // (= the trampoline's RCX-on-entry, which is uninitialized).
    // With this field set, `resolveUserEntrySymbol` looks it up
    // directly — bypassing the functions[0] fallback.
    //
    // Empty optional ⇒ pre-13.5-cycle-2 fallback discipline
    // (entryPoint or functions[0]). Set ⇒ load-bearing.
    std::optional<SymbolId>        userEntrySymbol;     // SymbolId of user-written entry fn

    // Derived: true iff every function received its parallel-index slot —
    // `functions.size() == expectedFuncCount`. The reporter separately tracks
    // whether per-instruction encoding errors were emitted; `ok()` is the
    // SHAPE check, NOT the encoding-success check. Callers that need "every
    // byte successfully encoded" must also consult `reporter.errorCount() == 0`
    // (per the cycle-3a separation of concerns). An EMPTY module (0 functions —
    // a declaration-only / all-preprocessed-out TU) is a VALID success (0 == 0):
    // `assemble()` returns empty-in ⇒ empty-out and the module lowers to a valid
    // empty relocatable object, matching gcc/clang. The earlier
    // `expectedFuncCount > 0` clause wrongly forced ok()==false, silently
    // rejecting the whole compile (D-CSUBSET-TESTTU-SILENT-EXIT1). A genuine
    // shape failure (the lirToMir-size-mismatch path leaves expectedFuncCount=N>0
    // with `functions` empty) is still caught by the count mismatch (0 != N) —
    // and that path also reports A_LirToMirSizeMismatch.
    [[nodiscard]] bool ok() const noexcept {
        return functions.size() == expectedFuncCount;
    }

    // Find an `AssembledFunction` by position in the OUTPUT module.
    // Returns nullptr if out of range. Position is the index into
    // the original LIR module's `funcAt(i)` enumeration (parallel-
    // index discipline).
    [[nodiscard]] AssembledFunction const* forFuncByIndex(std::uint32_t i) const noexcept {
        return (i < functions.size()) ? &functions[i] : nullptr;
    }
};

// Universal entrypoint — no per-arch overload. Same target-blind
// shape that ML5 cycle 2a established (`lowerToLir(Mir, TargetSchema,
// ...)` / ML7's `materializeCallingConvention(Lir, TargetSchema,
// ...)` ).
//
// `lirToMir` maps each `LirInstId.v` (slot index) back to the
// originating `MirInstId` — sourced from the ML5 lowerer's
// `MirToLirResult::lirToMir` projection. AS2/AS3 will use it to stamp
// the right `MirInstId` onto each emitted byte range's
// `SourceMapEntry`. The substrate enforces `lirToMir.size() ==
// lir.instCount()` at entry — a shorter span would silently UB once
// the format walkers light up. A size mismatch emits
// `A_LirToMirSizeMismatch` and returns an empty module.
//
// On any per-instruction encoding failure the assembler emits an
// `A_*` diagnostic into `reporter` and continues — partial bytes
// accumulated before the failure are kept (parallel-index discipline
// requires every LIR func to produce some `AssembledFunction` slot,
// even an empty one).
[[nodiscard]] DSS_EXPORT AssembledModule
assemble(Lir const&                 lir,
         TargetSchema const&        schema,
         std::span<MirInstId const> lirToMir,
         DiagnosticReporter&        reporter,
         // Extern symbols propagated from `MirToLirResult.
         // externImports`. Copied verbatim into
         // `AssembledModule.externImports` for the linker to
         // resolve against. Defaults to empty for static modules
         // and for legacy test call sites that construct
         // `externImports` directly on the returned
         // `AssembledModule`. (LK6 cycle 2d — D-LK6-6 closure.)
         std::span<ExternImport const> externs = {});

// D-LK4-RODATA-PRODUCER (2026-06-02) — materialize module-level MIR
// globals into `AssembledData` items the linker emits as `.rodata`
// (PE walker) / `.rodata` (ELF) / `__cstring`+`__const` (Mach-O).
//
// Reads each MirGlobal from `mir`, looks up its `initLiteralIndex`
// in the module's `MirLiteralPool`, and encodes the literal value
// into LE wire bytes sized by the global's `TypeId`. The
// `TypeInterner` is consulted for primitive byte sizes; AGGREGATE
// types (Struct / Union / Array, recursively, incl. nested) are
// encoded via the `type_layout` engine — `aggregateLayout` (the
// target's per-ABI alignment params) + `dataModel` (the format's
// pointer width) drive `computeLayout`, so a const-init
// `struct P { int x; int y; } v = { 20, 22 };` at file scope reaches
// the binary's `.rodata` byte-exact. `aggregateLayout` is nullopt
// only when the target declared no `aggregateLayout` block; an
// aggregate global then fails loud (no guessed layout). F16/F80/F128
// leaves (any float without a lossless host-`double` byte arm) + zero-init
// (`.bss`) globals + runtime-init globals stay
// fail-loud-deferred (their own anchors). Pointer-with-address-
// constant leaves are not byte-encodable (they need a relocation)
// and fail loud — no consumer yet.
//
// Caller (`program/compile_pipeline.cpp` after `assemble()`)
// assigns the returned vector to `AssembledModule::dataItems`
// before invoking the linker. Empty-MIR-globals is empty-out;
// runtime-init globals (those with `initFunc.valid()`) are SKIPPED
// here — their bytes land via the synthesized `__module_init__`
// function at module-load time, not via the rodata pipeline.
[[nodiscard]] DSS_EXPORT std::vector<AssembledData>
lowerMirGlobalsToDataItems(Mir const&                           mir,
                           TypeInterner const&                  interner,
                           std::optional<AggregateLayoutParams> aggregateLayout,
                           DataModel                            dataModel,
                           DiagnosticReporter&                  reporter,
                           // F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): the target's
                           // ABSOLUTE-64 pointer relocation kind, found by FORMULA
                           // (widthBytes==8 && !pcRelative) by the caller — never a
                           // hardcoded name/constant (agnosticism). A symbol-address
                           // global emits this reloc; nullopt → fail loud if one exists.
                           std::optional<RelocationKind>        absPtrRelocKind = std::nullopt);

} // namespace dss
