#pragma once

// MIR-tier constant-folding pass.
//
// Scope: integer + bool + bitwise + integer-comparison fold. Float and
// cast opcodes are deferred (no Float arithmetic in c-subset today;
// Cast-arm dispatch lands when needed). Folds pure-functional opcodes
// whose operands all resolve to `Const` instructions at compile time,
// replacing the result with a new `Const` emitted into the rebuilt
// module's literal pool. Reuses the const-eval arithmetic helpers in
// `src/hir/const_eval_arith.hpp` — those operate purely on
// `HirLiteralValue` / `HirOpKind` / `EvalOptions`, structurally
// identical to `MirLiteralValue`, so the bridge is a field-by-field copy.
//
// **Overflow policy**: MIR has no per-instruction `nsw`/`nuw` flag, so
// the pass uses `refuseOnOverflow=false` (wrap semantics) — matches the
// language-neutral "native arithmetic, modular on overflow" convention
// the runtime backend produces. The post-fold `wrapToIntTarget` re-masks
// the int64 arithmetic result to the destination type's bit width.
//
// **Failure recovery**: when a fold would trap (DivisionByZero,
// ShiftCountOutOfRange, etc.), the instruction is COPIED VERBATIM
// rather than rewritten. Folding to a trap would observably differ
// from the unoptimized path; the conservative path defers the trap.
// `tryFold` returns nullopt for THREE reasons — opcode-not-in-foldable-
// set, operand-not-constant, fold-would-trap — and the caller's
// fallthrough to verbatim copy treats all three identically.
// Downstream passes (DCE etc.) MUST NOT delete the instruction based
// on the nullopt signal.
//
// **Runtime-init globals carve-out** (D-OPT2-CONST-FOLD-RUNTIME-INIT-
// GLOBALS): if any module global has `initFunc.valid()`, the pass
// returns `ok=true, instructionsFolded=0` without rebuilding. The
// caller keeps the unoptimized MIR; const-fold simply doesn't pay
// for this module yet. (Full closure: thread a func-id remap through
// the global-clone path, lifting the carve-out. Anchored.)
//
// **Pass discipline**: rebuild, don't mutate. The pass allocates a
// new `MirBuilder`, walks the old module function-by-function /
// block-by-block / instruction-by-instruction, copies or rewrites
// each instruction, and replaces `mir` via `finish() → std::move`.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>

namespace dss::opt::passes {

struct ConstFoldResult {
    bool        ok                  = false;
    std::size_t instructionsFolded  = 0;
};

// Run constant folding over every function in `mir`. Replaces `mir` with
// the rebuilt module on success. Returns the count of instructions that
// were rewritten as `Const` (for the optimizer engine's `passesMutated`
// signal). On verifier-detected SSA violations the underlying MirBuilder
// will abort loud (substrate invariant) — no recovery path needed.
[[nodiscard]] DSS_EXPORT ConstFoldResult
runConstFold(Mir& mir, TypeInterner const& interner,
             DiagnosticReporter& reporter);

} // namespace dss::opt::passes
