#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// MIR text format `.dssir` (ML4) — a round-trippable, human-readable
// serialization of a frozen `Mir`. Same shape HR7 took for HIR: emit
// renders, parse rebuilds (re-interning types) and runs `MirVerifier`
// on load. The contract is byte-identical round-trip:
// `emitMir(parseMir(emitMir(m))) == emitMir(m)`.
//
// Compared to `.dsshir`:
//   - No extension opcodes (MIR has a closed `MirOpcode` enum), so no
//     `ext_ops` / `intrinsics` preamble (intrinsic ids are bare integers).
//   - No 5 side-tables. Side-table support deferred until ML2 starts
//     populating a MirSourceMap (parallel to HirSourceMap).
//   - Literal pool is OWNED by `Mir`, so no external pool injection;
//     `Const` instructions render their literal value inline by
//     dereferencing the module's own pool.
//   - Instruction surface is CFG (block-of-block), not tree-of-tree:
//     each function is a sequence of labeled blocks, each block is a
//     sequence of instructions ending in a terminator.
//
// Text grammar (representative):
//
//   dssir 2
//   types {
//     type 1 = struct "Node" {i32, ptr<type 1>}
//   }
//   symbols {
//     %1 "main"
//     %2 "factorial"
//   }
//   module {
//     global %3 : i32 = lit int 0 : i32
//     function %1 : fn() -> i32 {
//       block %b1 [entry] {
//         %v2 = const : i32 (lit int 42 : i32)
//         return %v2
//       }
//     }
//   }
//
// ★★ v2 (P68 round 8, lane `ht`, part 1d): A COMPOSITE IS `type <H>`, DEFINED ONCE
// in the `types` section — `type <H> = struct|union "<name>" (opaque | [packed]
// [aligned N] [pack N] { <type> [@N | ~N] [bits N] [packed], … })`, the HIR v5
// table's spelling, written and completed through the ONE owner of what a
// definition carries (`core/types/type_lattice/composite_definition.hpp`). v1
// spelled every composite inline at every use with its field types only: the text
// was O(mentions × graph) (✔MEASURED, `emitMir` of sqlite3.c ran to 30 GB and
// `std::bad_alloc`), a self-referential composite had no spelling, and every layout
// channel was lost on the way back. An inline `struct "N" {…}` in a v2 module is
// refused by name, and a `dssir 1` text by the version check.
//
// ★★ ONE INSTRUCTION PER LINE, and it is a RULE of this grammar rather than a
// habit of the emitter. `parseInstruction` refuses any instruction whose line
// still holds tokens after its arm has run (a closing `}` excepted, so a
// hand-written body may end on the instruction's line). That is the CLASS guard
// for a writer arm that renders an operand or payload tail its reader arm does
// not consume — the defect three separate opcodes carried, each producing a
// SUCCESSFUL parse of a WRONG instruction followed some tokens later by a
// diagnostic about a line that was never the problem
// (D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS).
//
// Verify-on-load: `parseMir` runs `MirVerifier` against the rebuilt
// module + interner. `result->ok` is the delta on the reporter's
// error count over the full parse + verify (so a pre-existing
// diagnostic doesn't taint the verdict).

namespace dss {

class DiagnosticReporter;

// ── MirTextContext ─────────────────────────────────────────────────
//
// Non-owning enrichment for the emitter. A fully-null context still produces a
// STRUCTURALLY complete file (synthetic symbol handles, `?` type placeholders,
// one Warning).
//
// ⚠ IT IS NOT RE-PARSEABLE, and this sentence used to say it was. `parseType`
// refuses `?` by name — it has to, because `?` names no type and guessing one
// would silently retype the module — so a dump taken with no interner is a
// one-way dump. Stating the opposite here made an emitter warning look like a
// cosmetic note about a file that would still round-trip
// (D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS).
//
// ⚠⚠ AND IT WAS WORSE THAN "NOT RE-PARSEABLE" UNTIL 2026-08-23: the refused type
// travelled on as `InvalidType` into `MirBuilder::addFunction`/`addGlobal`, which
// ABORT on one — so reading such a dump back killed the process rather than
// returning a refusal (D-MIR-TEXT-INVALID-TYPE-REACHES-A-BUILDER-THAT-ABORTS).
struct DSS_EXPORT MirTextContext {
    // Decodes each instruction's `TypeId` into structural text. The
    // real pipeline always supplies the interner the semantic phase
    // produced. Its `owner()` must match the CU the module's `TypeId`s
    // were interned against.
    TypeInterner const* interner = nullptr;

    // SymbolId.v → human name. Slot 0 is the invalid-symbol sentinel.
    // An id past the end (or an empty entry) falls back to the synthetic
    // `%<v>` handle without a name. A production caller fills this
    // from the CU's symbol table; a unit test may leave it null.
    std::vector<std::string> const* symbolNames = nullptr;

    // The SPARSE form of the same table: one entry per symbol, keyed by
    // `SymbolId.v` — what `parseMir` hands back (`MirParseResult::symbolNames`).
    // A `.dssir` text's slots are the RAW ids of the CU's symbol table, and a
    // module references a scattering of them: ✔MEASURED P68 (lane `ht`, part
    // 1c) through the product's own lower half — 804 of 804 example modules
    // sparse (up to 434× max slot / entries), sqlite3.c's 6755 symbols spread
    // to %48033. A dense table would cost the NUMBER, not the text. A caller
    // sets at most one of the two; the dense one is consulted first.
    std::unordered_map<std::uint32_t, std::string> const* symbolNameMap = nullptr;
};

// Serialize `mir` to canonical `.dssir` text. Pure function. The call never
// aborts.
//
// ★★ SEVERITY IS PER-DIAGNOSTIC, AND IT USED TO BE DOCUMENTED — AND
// DEFAULTED — AS WARNING FOR ALL OF THEM. That put this tier one severity below
// the sibling `hir_text.cpp`, whose emitter defaults to Error, for the SAME class
// of event on two files that describe themselves as twins. The two classes are
// now distinguished at each site rather than by a default:
//   * ERROR   — a value or type this format cannot spell. The text is emitted
//               with a named marker, the reader REFUSES that marker, and the
//               module cannot be recovered from the text.
//   * WARNING — a lossy dump the CALLER asked for (no `interner`, no
//               `symbolNames`). Reported once, not per value.
// (D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS.)
[[nodiscard]] DSS_EXPORT std::string emitMir(Mir const& mir,
                                             MirTextContext const& ctx,
                                             DiagnosticReporter& reporter);

// ── MirParseResult ────────────────────────────────────────────────
//
// Heap-allocated; access the module as `result->mir`. The `interner`
// owns types re-interned from the text. Non-movable / non-copyable —
// the `Mir`'s arenas hold tag references that mustn't change address.
struct DSS_EXPORT MirParseResult {
    Mir          mir;
    TypeInterner interner;
    // SymbolId.v → name, ONE ENTRY PER SYMBOL THE TEXT DECLARES — sparse,
    // because the slots are raw CU ids (see `MirTextContext::symbolNameMap`,
    // which takes it back for a re-emission). It used to be a dense vector
    // sized by the LARGEST slot written: ✔MEASURED P68 (lane `ht`, part 1c),
    // `symbols { %4000000000 "x" }` threw `std::bad_alloc` out of `parseMir`,
    // `%4294967295` wrapped `v + 1` to 0 and wrote out of bounds (SIGSEGV), and
    // 40 bytes of `%100000000 "x"` built a 3.2 GB table. Memory is now the
    // text's: one entry per declaration.
    std::unordered_map<std::uint32_t, std::string> symbolNames;
    bool         ok = false;

    MirParseResult(Mir m, TypeInterner ti,
                   std::unordered_map<std::uint32_t, std::string> names)
        : mir(std::move(m)), interner(std::move(ti)),
          symbolNames(std::move(names)) {}

    MirParseResult(MirParseResult const&)            = delete;
    MirParseResult& operator=(MirParseResult const&) = delete;
    MirParseResult(MirParseResult&&)                 = delete;
    MirParseResult& operator=(MirParseResult&&)      = delete;
};

// Parse `.dssir` text into a frozen Mir + interner + symbol-name
// table, then run `MirVerifier` on the result. All diagnostics
// (parse + verify) go to `reporter`. `result->ok` is true iff no
// Error-severity diagnostic was emitted during the call. Types are
// re-interned into a fresh `TypeInterner` tagged with `cuId`.
// Collect-all: malformed input recovers to the next construct.
[[nodiscard]] DSS_EXPORT std::unique_ptr<MirParseResult> parseMir(
    std::string_view text, CompilationUnitId cuId,
    DiagnosticReporter& reporter);

} // namespace dss
