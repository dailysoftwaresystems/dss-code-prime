#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "mir/mir_node.hpp"

#include <cstdint>
#include <span>
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

// One assembled function — bytes + symbol-relative metadata. `symbol`
// is sourced from the originating `lir.funcSymbol(fn)` so the linker
// can place this function's bytes in its object-file symbol table
// without re-consulting the upstream `Lir`.
struct DSS_EXPORT AssembledFunction {
    SymbolId                    symbol{};
    std::vector<std::uint8_t>   bytes;
    std::vector<Relocation>     relocations;
    std::vector<SourceMapEntry> sourceMap;
};

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

struct DSS_EXPORT AssembledModule {
    std::vector<AssembledFunction> functions;  // parallel-index with lir.funcAt(i)
    std::size_t                    expectedFuncCount = 0;

    // FFI extern imports (LK6 cycle 2 — closes D-LK6-2 partial).
    // Populated by the assembler from upstream HIR `ExternFunction`
    // / `ExternGlobal` declarations once the HIR→AS thread-through
    // lands (D-LK6-6). Linker consults this AFTER `functions` for
    // any unresolved `Relocation::target`.
    std::vector<ExternImport>      externImports;

    // Derived: true iff `assemble()` ran on a non-empty LIR module AND
    // every function received its parallel-index slot. The reporter
    // separately tracks whether per-instruction encoding errors were
    // emitted; `ok()` is the SHAPE check, NOT the encoding-success
    // check. Callers that need "every byte successfully encoded" must
    // also consult `reporter.errorCount() == 0` (per the cycle-3a
    // separation of concerns).
    [[nodiscard]] bool ok() const noexcept {
        return expectedFuncCount > 0 && functions.size() == expectedFuncCount;
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

} // namespace dss
