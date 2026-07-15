#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"        // ExternImport
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"          // SymbolBinding
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "mir/mir.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Cross-module MIR merge (Cycle 25, Stage B) — fold N per-CU `Mir` modules into
// ONE whole-program `Mir`, unifying their CU-scoped type interners into a single
// host `TypeLattice` and resolving cross-CU symbol references (weak-vs-strong)
// to DIRECT intra-module calls.
//
// This is the MERGE CORE, deliberately DECOUPLED from `SemanticModel` so it can
// be unit-tested at the MIR tier with HAND-BUILT inputs. It composes the two
// Stage-A/Cycle-24 kernels:
//   * `reinternType` (core/types/type_lattice/type_reintern.hpp) — re-interns
//     every TypeId that crosses a CU boundary into the host lattice so the host
//     hash-conses structurally-identical types from different CUs to one TypeId.
//   * `resolveCrossCuDefs` (link/cross_cu_resolve.hpp) — the single source of
//     truth for the cross-CU winner-selection policy (strong shadows weak;
//     two-strong is a conflict).
//
// Stage C (separate) wires `mergeCuMirs` into the driver; this stage delivers it
// tested in isolation. The driver keeps the byte-identical single-CU
// `SemanticModel` path for N==1 and only calls `mergeCuMirs` for N>=2, but
// `mergeCuMirs` is correct for N>=1 regardless.
//
// AGNOSTIC: the clone keys on MIR opcode + `SymbolBinding` + `TypeKind` only —
// no language / target / object-format branch. FAIL-LOUD: an unhandled clone
// opcode, a cross-module operand that doesn't map, or a merged module that fails
// `MirVerifier` aborts / returns nullopt with a diagnostic — never a silent
// mis-merge.

namespace dss {

// One CU's decomposed inputs to the merge. DECOMPOSED (not a `CuMirModule`) so a
// MIR-tier unit test can hand-build each field without a real `SemanticModel`:
//   * `mir`           — the CU's frozen module (non-owning; outlives the call).
//   * `interner`      — the CU's type interner (the source lattice for reintern).
//   * `nameOf`        — symbol id → declared name. Covers BOTH function/global
//                       DEFINITIONS (the semantic-model symbol name) AND extern
//                       IMPORTS (the `ExternImport.mangledName`); the merge keys
//                       cross-CU matching on this name, exactly as the linker
//                       keys on the on-binary symbol name. A symbol with no name
//                       (returns "") is treated as module-private (never matched
//                       across CUs).
//   * `externImports` — the CU's real-FFI import rows. An import whose
//                       `mangledName` resolves to a cross-CU DEFINITION is
//                       rewired to a direct call + STRIPPED; the rest survive.
struct MergeCuInput {
    Mir const*                           mir = nullptr;
    TypeInterner const*                  interner = nullptr;
    std::function<std::string(SymbolId)> nameOf;
    std::span<ExternImport const>        externImports;
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): this CU's pe64 <threads.h> SHIM
    // symbols (SymbolId.v → recipe id). These are REFERENCED-ONLY in `mir` (CST→HIR
    // skipped their import; the def is synthesized POST-merge), so the def/global/extern
    // planning never assigns them a merged id — without this the merge ABORTS
    // (`mergedSymbolOf`) on a cloned caller's `GlobalAddr(shimSym)`. The merge registers
    // each with a merged id (unified by name across CUs) so the clone remaps and the
    // merged id reaches `symbolNames` for the post-merge shim synthesis. nullptr / empty
    // for every non-threads TU (the overwhelming majority). Non-owning — the map lives
    // on the caller's `CuMirModule.threadsRecipes`, which outlives the merge.
    std::unordered_map<std::uint32_t, std::string> const* synthRecipes = nullptr;
};

// The merged whole-program module + the unified state the lower half consumes.
//   * `mir`            — the single merged module (host-stamped TypeIds).
//   * `host`           — the unified interner lattice (moved out; owns every
//                        host TypeId the merged `mir` references).
//   * `symbolNames`    — merged-symbol id (`.v`) → declared name, for the lower
//                        half's symbol-table populate.
//   * `externImports`  — the SURVIVING real-FFI imports (cross-CU-resolved ones
//                        stripped), with each `symbol` unified-remapped.
//   * `userEntrySymbol`— the merged symbol of the function whose name is in the
//                        grammar's entry-name list (e.g. "main"), or nullopt.
struct MergedMirModule {
    Mir                                            mir;
    TypeLattice                                    host;
    std::unordered_map<std::uint32_t, std::string> symbolNames;
    std::vector<ExternImport>                      externImports;
    std::optional<SymbolId>                        userEntrySymbol;
};

// Merge `cus` into one module. `host` is MOVED in — the driver moves CU0's
// `TypeLattice`; a test may pass a fresh `TypeLattice` or CU0's. `entryNames` is
// the grammar's entry-function name list (e.g. {"main"}) so the merge can
// compute `userEntrySymbol`. Returns nullopt (with a diagnostic via `reporter`)
// on a two-strong conflict-only-failure path is NOT taken — a conflict is
// reported but the merge still proceeds with the lowest-key winner; nullopt is
// returned only on a structural failure (the merged module fails `MirVerifier`,
// or an internal invariant is breached). Correct for `cus.size() >= 1`.
[[nodiscard]] DSS_EXPORT std::optional<MergedMirModule>
mergeCuMirs(std::span<MergeCuInput const> cus, TypeLattice&& host,
            std::span<std::string const> entryNames, DiagnosticReporter& reporter);

} // namespace dss
