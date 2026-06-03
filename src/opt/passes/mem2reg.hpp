#pragma once

// MIR-tier Mem2Reg — promote address-of-never-taken stack allocas to
// SSA values via Cytron-Ferrante (idom + iterated dominance frontier
// + rename DFS over the dominator tree).
//
// **PROMOTABILITY GATE** (D-OPT-MEM2REG-PROMOTABLE-ALLOCA-SCAN): an
// alloca is promoted iff every use of its result is either:
//   - `Load`  with the alloca at operand index 0 (the address)
//   - `Store` with the alloca at operand index 1 (the address)
// ANY other use — pass to a Call, GEP'd, Bitcast'd, stored AS A VALUE
// via Store-operand[0], reaches a Phi as a value, etc. — disqualifies.
// The gate is the classic Mem2Reg correctness pre-condition; getting
// it wrong is a silent miscompile (a Load through an aliased pointer
// would see a stale value).
//
// **PHI PLACEMENT** (D-OPT-DOMFRONTIER-LOOP-TEST): for each promotable
// alloca, the rename pass inserts a Phi at every block in the IDF of
// its def-blocks (blocks containing a Store to that slot). This is
// the exact algorithm Cytron-Ferrante 1991 prescribes; the IDF
// shape generalizes from `copy_prop_across_join`'s diamond (one Phi
// at the join) up to arbitrary CFG shapes including back-edges.
//
// **DIFFERENTIAL-EXECUTION CORRECTNESS**: Mem2Reg is semantics-
// preserving — observed behavior (exit code, stdout) is byte-identical
// before vs after. The corpus pin `copy_prop_across_join` exercises
// this via its `mem2reg-only` optimizedPipelines arm; the runner
// re-spawns the OS process under both pipelines and diff-asserts.
//
// **CFG SAFETY**: Mem2Reg INSERTS Phi instructions only. It never
// re-orders blocks, never re-points terminators, never re-marks
// blocks. The `MirFunctionRebuilder` clones each block's
// `StructCfMarker` verbatim; Mem2Reg never overrides
// `recordTerminatorInRewrite` / `acceptPhiIncoming` and never adds
// per-block synthesis other than the IDF phis. A test pin
// (StructCfMarkersUnchanged) byte-compares pre/post markers.
//
// **VERIFY-AFTER-PASS**: the optimizer engine runs `MirVerifier::verify`
// after every successful pass (D-OPT1-VERIFY-AFTER-EVERY-PASS). The
// verifier's use-dom-def check independently catches a missing Phi
// (a use whose def doesn't dominate its block — the exact failure
// mode of a buggy Mem2Reg).
//
// **Runtime-init globals carve-out**: mirrors ConstFold + DCE — a
// module with any `globalInitFunc.valid()` global skips Mem2Reg with
// an Info diagnostic. The two-pass func-id remap that would lift this
// is deferred (same anchor: D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>

namespace dss::opt::passes {

struct Mem2RegResult {
    bool        ok                = false;
    std::size_t allocasPromoted   = 0;
    std::size_t phisInserted      = 0;
    std::size_t loadsReplaced     = 0;
    std::size_t storesEliminated  = 0;
};

[[nodiscard]] DSS_EXPORT Mem2RegResult
runMem2Reg(Mir& mir, TypeInterner const& interner,
           DiagnosticReporter& reporter);

} // namespace dss::opt::passes
