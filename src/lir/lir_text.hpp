#pragma once

#include "core/export.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <span>
#include <string>
#include <string_view>

// LIR text format `.dsslir` (ML8) — a round-trippable, human-readable
// serialization of a frozen `Lir`. Same shape MIR's `.dssir` (ML4)
// and HIR's `.dsshir` (HR7) took: emit renders, parse rebuilds and
// runs the verifier on load.
//
// **Cycle 1 (this commit) ships the EMITTER only**. The parser +
// verifier-on-load are anchored as ML8 cycle 2 (D-ML8-1.1 in plan
// 12 §3.1). Cycle 1 establishes the format contract via golden test
// fixtures; cycle 2 closes the round-trip with the contract:
//
//   emitLir(parseLir(emitLir(m))) == emitLir(m)
//
// — i.e. the round-trip contract is a CYCLE-2 obligation; cycle 1
// fixes only the emit side so cycle 2 has a deterministic fixture
// corpus to target.
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
//   target x86_64
//   symbols {
//     %1 "main"
//     %2 "factorial"
//   }
//   literal_pool {
//     lit#0 = i64 42 core I64
//     lit#1 = agg [i64 1, i64 2] core Struct
//   }
//   module {
//     function %1 "main" {
//       block ^b0 [entry] -> [^b1] {
//         rax = mov #42 ; payload=0
//         jmp ^b1 ; payload=0
//       }
//       block ^b1 -> [] {
//         rax = add rax, rax ; payload=0
//         ret rax ; payload=0
//       }
//     }
//   }

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
// severity; the call never aborts.
[[nodiscard]] DSS_EXPORT std::string
emitLir(Lir const& lir, TargetSchema const& schema,
        LirTextContext const& ctx, DiagnosticReporter& reporter);

} // namespace dss
