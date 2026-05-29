#pragma once

#include "core/export.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// LIR text format `.dsslir` (ML8) — a round-trippable, human-readable
// serialization of a frozen `Lir`. Same shape MIR's `.dssir` (ML4)
// and HIR's `.dsshir` (HR7) took: emit renders, parse rebuilds and
// runs the verifier on load.
//
// **Cycle 1 shipped the EMITTER**. **Cycle 2 (this commit) closes
// the parser + verify-on-load + the byte-identical round-trip
// contract**:
//
//   emitLir(parseLir(emitLir(m), schema, r)->lir, schema, ctx, r)
//       == emitLir(m, schema, ctx, r)
//
// holds byte-identical for every input the cycle-1/2 emitter produces
// (i.e. for any module the substrate itself produced). Producer-side
// corruption (a `Lir` carrying e.g. malformed MemBase/MemOffset
// pairing) is OUT of contract — emit is a pure serializer, see
// `emitLir`'s docstring. Cycle-1/2 split note is retained as an
// audit trail; the format contract is now WHOLE.
//
// Compared to `.dssir`:
//   - LIR has no Type system, so no type re-interning at parse time.
//     The opcode set is per-target and dispatched via `TargetSchema`
//     (loaded separately by name from the preamble).
//   - Physical registers are rendered by their declared mnemonic
//     (`rax`, `xmm0`, ...) so the text is target-specific. Virtual
//     registers render as `%v.N` keyed by their `LirReg.id`.
//   - LIR carries a literal pool (cycle 3c). The text format renders
//     the pool inline as a preamble section so the parser can
//     reconstruct it. EVERY literal carries its `core` TypeKind tag
//     so the type hint round-trips.
//   - The text is target-bound: the preamble carries `target <name>`
//     and the parser loads the matching shipped `TargetSchema`.
//   - Every instruction emits `; payload=N` (UNCONDITIONAL). Round-
//     trip of `TargetCondCode::Eq == 0` would otherwise silently flip
//     to "no payload" on parse. The 5-char overhead is the cost of
//     lossless round-trip; cycle 2 can introduce a schema-driven
//     opt-out if profiling shows it matters.
//
// Text grammar (representative):
//
//   dsslir 1
//   target x86_64 version "1.0.0"
//   symbols {
//     %1 "main"
//     %2 "factorial"
//     %7                // synthetic — referenced but no name supplied
//   }
//   literal_pool {
//     lit#0 = i64 42 core I64
//     lit#1 = agg [i64 1 core I64, i64 2 core I64] core Struct
//   }
//   module {
//     function %1 "main" {
//       block ^b0 [entry] -> [^b1] {
//         rax = mov #42 ; payload=0 flags=0
//         %v.1:gpr = mov #7 ; payload=0 flags=0
//         jmp ^b1 ; payload=0 flags=0
//       }
//       block ^b1 -> [] {
//         rax = add rax, rax ; payload=0 flags=0
//         ret rax ; payload=0 flags=0
//       }
//     }
//   }
//
// Virtual registers carry a MANDATORY `:<class>` suffix (`%v.N:gpr` /
// `:fpr` / `:vr` / `:flags` / `:none`) so an FPR vreg round-trips
// without silently demoting to GPR. Class names match `lirRegClassName`.

namespace dss {

class DiagnosticReporter;

// ── LirTextContext ─────────────────────────────────────────────────
//
// Non-owning enrichment for the emitter. A fully-empty context still
// produces a complete, re-parseable file: every reachable symbol id
// gets a `symbols { }` entry, even if no name is supplied (synthetic
// `%N <synthetic>` form).
struct DSS_EXPORT LirTextContext {
    // SymbolId.v → human name. Slot 0 is the invalid-symbol sentinel.
    // An id past the end (or an empty entry) renders as the synthetic
    // form. The emitter additionally walks reachable symbol ids and
    // emits a `symbols { }` entry for every one so cycle-2 parser
    // never sees an unbound `%N` reference.
    //
    // Stored as `std::span` so callers may back the names with any
    // contiguous container (`std::vector`, `std::array`, a flat slice
    // of a larger table) — the emitter only needs read-only random
    // access.
    std::span<std::string const> symbolNames{};
};

// Serialize `lir` to canonical `.dsslir` text. The schema is required
// to resolve opcode mnemonics + physical-register names. Diagnostics
// (e.g. `L_PhysRegOrdinalOutOfRange`) go to `reporter` at Warning
// severity; the call never aborts. **Emit is a pure serializer**:
// it does NOT run the LIR verifier. Callers that want producer-side
// integrity checks should run `verifyLirText` (or the full `verifyLir`)
// before emit. This keeps round-trip (emit → parse → emit) free of
// double-verification cost — the parser already verifies on load.
[[nodiscard]] DSS_EXPORT std::string
emitLir(Lir const& lir, TargetSchema const& schema,
        LirTextContext const& ctx, DiagnosticReporter& reporter);

// ── LirParseResult ────────────────────────────────────────────────
//
// Heap-allocated + move/copy deleted so the contained `Lir`'s arena
// allocators don't relocate under callers holding `LirInstId` /
// `LirBlockId` references into it — same stable-address contract
// HR7's `HirParseResult` and ML4's `MirParseResult` honor. Callers
// always interact via `std::unique_ptr<LirParseResult>`, so the move-
// deletion costs nothing at call sites and rules out a class of
// slicing / dangling-reference bugs.
//
// `symbolNames`: built from the `symbols { }` preamble. Entries past
// the end of the vector (or empty entries) are synthetic references
// with no declared name.
//
// `ok` is true iff zero error-severity diagnostics were emitted by
// the parser OR the verify-on-load pass. On `ok == false`, both `lir`
// and `symbolNames` are guaranteed to be empty (not partially built)
// so consumers don't risk reading names for symbols that don't
// exist in the module.
struct DSS_EXPORT LirParseResult {
    Lir                      lir;
    std::vector<std::string> symbolNames;
    bool                     ok = false;

    explicit LirParseResult(Lir l, std::vector<std::string> names) noexcept
        : lir(std::move(l)), symbolNames(std::move(names)) {}

    LirParseResult(LirParseResult const&)            = delete;
    LirParseResult& operator=(LirParseResult const&) = delete;
    LirParseResult(LirParseResult&&)                 = delete;
    LirParseResult& operator=(LirParseResult&&)      = delete;
};

// Parse `.dsslir` text into a frozen `Lir` + symbol-name table, then
// run the LIR-only verify-on-load rules (currently MemBase/MemOffset
// pairing — the only rule that doesn't require an MIR cross-reference).
// All diagnostics go to `reporter`.
//
// `schema` is passed BY CONST REFERENCE rather than loaded by the
// parser so (a) the parser stays decoupled from the schema registry's
// lifecycle (production callers own long-lived `shared_ptr<TargetSchema>`
// instances), (b) tests can inject synthetic schemas, and (c) the
// `LirBuilder` the parser drives requires the SAME schema instance the
// caller holds — re-loading would create a different `TargetSchemaId`
// and break the strong-id arena tag.
//
// Schema cross-check: `schema.name()` and `schema.version()` MUST
// match the preamble's `target <name> version "<sv>"`. A mismatch is
// rejected with `I_TextVersionMismatch` (Error). The asymmetric case
// of an empty schema version against a non-empty text version is
// accepted with a Warning of the same code.
//
// Round-trip contract:
//   emitLir(parseLir(emitLir(m), schema, r)->lir, schema, ctx, r) == emitLir(m, ...)
// holds byte-identical for every well-formed input.
[[nodiscard]] DSS_EXPORT std::unique_ptr<LirParseResult>
parseLir(std::string_view text, TargetSchema const& schema,
         DiagnosticReporter& reporter);

} // namespace dss
