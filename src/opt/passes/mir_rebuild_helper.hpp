#pragma once

// Shared MIR-tier function rebuilder substrate. Every MIR-tier pass
// that rewrites a function via MirBuilder drives this rebuilder —
// centralizing the rewrite-map / terminator-clone / phi-deferral
// invariants prevents per-pass drift (D-OPT-MIR-REBUILDER-EXTRACT).
//
// **REWRITE-MAP COMPLETENESS CONTRACT** (D-OPT2-REWRITE-MAP-COMPLETENESS):
// every EMITTED OLD-module inst must be in the rewrite map by the time
// a downstream consumer reads it. Phi back-edges are the one delayed
// case — handled in phase 3 only. A missing entry at emit time is a
// substrate-contract violation; `rewriteOperand` fails loud rather
// than propagating an invalid `MirInstId{}`.
//   ⚠ "EMITTED", NOT "VALUE-PRODUCING" — the two are not the same set and
//   reading this line as the narrower one cost a `std::abort` on a legal
//   program (D-OPT-ASM-GOTO-WITH-OUTPUTS-ABORTS-THE-MIR-REBUILDER). An
//   operand can reference an instruction as an ANCHOR rather than as a
//   value: `ReturnPiece`'s single operand names its PRODUCER, and for an
//   `asm goto` with outputs that producer is the block's TERMINATOR. So
//   terminators are recorded too, unconditionally — see the note where
//   the `recordTerminatorInRewrite` hook used to be.
//
// **The 3-phase rebuild**:
//   Phase 1 — pre-create every selected block so terminators can
//             target forward references (loop back-edges).
//   Phase 2 — fill blocks with instructions; Phi incomings deferred.
//   Phase 3 — flush Phi incomings via the now-complete rewrite map.

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dss::opt::passes {

// Shared "clone globals OR carve out on runtime-init" prelude used
// by every MIR-tier rebuild pass (ConstFold, Dce, Mem2Reg, CopyProp).
// A module with any `globalInitFunc.valid()` global requires a two-
// pass func-id remap not yet implemented; carve-out emits Info
// `X_OptPassSkipped` so callers / diff-verify can observe the
// deliberate skip. The carve-out anchor is shared:
// D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS.
enum class GlobalClonePrelude : std::uint8_t { Cloned, CarvedOut };

[[nodiscard]] DSS_EXPORT GlobalClonePrelude
cloneGlobalsOrCarveOut(Mir const& mir, MirBuilder& builder,
                       DiagnosticReporter& reporter,
                       std::string_view passName);

// Verbatim globals clone for the MANDATORY unreachable-block prune
// (D-MIR-UNREACHABLE-PRUNE-NORMALIZE). Does what `cloneGlobalsOrCarveOut`'s
// Cloned path does, with TWO differences that make it correct for a pass
// that MUST process every module:
//   (1) It NEVER carves out — no runtime-init `globalInitFunc.valid()`
//       check, no early return. The prune is a verifier-correctness
//       normalize that must run on EVERY compilation unit (and the merged
//       module); skipping it on a runtime-init module would leave the
//       verifier-rejected orphan continuation blocks in place.
//   (2) It PRESERVES each runtime-init global's `initFunc` (the carve-out
//       helper passes `MirFuncId{}`, dropping it — safe there only because
//       its Cloned path is unreachable whenever any initFunc is valid).
//       The prune drops only BLOCKS — never functions, never reordering
//       them — so the func ORDINAL is stable across the rebuild and needs
//       NO remap. The arena TAG, however, IS re-stamped: the optimizer
//       mints a FRESH MirModuleId per rebuild, so initFunc is carried as
//       `MirFuncId{old.v, builder.id().v}` (same ordinal, new tag). Passing
//       the raw old id would trip MirBuilder's cross-module guard.
//
// ORDERING CONTRACT: call this AFTER every function has been re-added to
// `builder` — the tag re-stamp at the old ordinal requires the function
// slots to already exist (else finish()'s dangling-initFunc check fires).
//
// The setAliasingMode / setCharTypesAliasAll propagation is identical to
// the shared helper (D-OPT-LOAD-ALIAS-ANALYSIS-PIPELINE-PROPAGATE).
DSS_EXPORT void cloneGlobalsVerbatim(Mir const& mir, MirBuilder& builder);

// The `cloneGlobalsVerbatim` body, generalized to a rebuild that DROPS
// functions — which shifts every later function's ORDINAL and therefore
// invalidates the "same ordinal, new tag" shortcut above. Used by the
// optimizer's inline-definition strip epilogue
// (D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED), the one rebuild
// that removes whole functions and must still run on EVERY module.
//
// `oldOrdinalToNew[i]` is the rebuilt module's ordinal for old function ordinal
// `i`, or `UINT32_MAX` if that function was dropped. Its size MUST equal the old
// module's `moduleFuncCount()`.
//
// `passName` names the REBUILD DRIVER for the two fatals below, on the same
// terms (and with the same spelling) as `cloneGlobalsOrCarveOut`'s — it is not
// optional, because a fatal is the one message a reader cannot follow up
// interactively (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS).
//
// ★ WHY THIS EXISTS RATHER THAN A CARVE-OUT. `cloneGlobalsOrCarveOut` answers a
// runtime-init module by SKIPPING the pass — correct for an optimization, and
// catastrophic here: skipping the strip emits an inline definition's body as a
// real external definition, which is exactly the miscompile the strip prevents.
// A pass that must not be skippable needs a real remap, so this is one.
//
// ★ FAIL-LOUD: a global whose `initFunc` maps to `UINT32_MAX` aborts. Nothing
// should ever drop a module-init function, and silently re-pointing a global's
// initializer at whichever function inherited the ordinal would be a wrong-body
// call with no diagnostic.
//
// ORDERING CONTRACT is unchanged: call AFTER every surviving function has been
// re-added to `builder`.
DSS_EXPORT void
cloneGlobalsRemappingInitFunc(Mir const& mir, MirBuilder& builder,
                              std::span<std::uint32_t const> oldOrdinalToNew,
                              std::string_view passName);

// Per-pass policy driving the rebuild. Pass-specific state lives on
// the concrete subclass; the rebuilder owns only the rewrite map +
// the per-function block map. Default-method implementations express
// the "no special handling" path; a concrete policy overrides only
// the hooks it needs.
class DSS_EXPORT MirRebuildPolicy {
public:
    virtual ~MirRebuildPolicy() = default;

    // ★★★ THE PASS THIS POLICY BELONGS TO, PRINTED BY EVERY FATAL THE REBUILD
    // SUBSTRATE CAN RAISE (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS).
    // ~9 policies drive one rebuilder, and its aborts used to name only the
    // HELPER: `rewriteOperand: old MirInstId v=N has no rewrite entry` reads
    // identically whichever policy produced it. ✔MEASURED as a cost, not a
    // theory — isolating D-OPT-ASM-GOTO-WITH-OUTPUTS-ABORTS-THE-MIR-REBUILDER
    // took a `DSS_OPT_TRACE` run whose ONLY product was the word `SimplifyCfg`.
    // A fatal is the one message a reader cannot follow up interactively (the
    // process is gone), so it is the least able to afford a missing attribution.
    //
    // ★★ PURE VIRTUAL, AND THAT IS THE MECHANISM RATHER THAN AN OVERSIGHT. A
    // defaulted `passName = "unknown"` parameter would re-create this exact row
    // for the next pass added — silently, at whichever construction site forgot
    // it. An abstract method cannot be forgotten: a policy that does not name
    // itself does not INSTANTIATE, so the omission is a compile error at the
    // construction site and never a diagnostic hole at runtime.
    //
    // ★ WHY THE NAME LIVES ON THE POLICY AND NOT ON `MirFunctionRebuilder`'s
    // constructor. ✔MEASURED 2026-08-17: the rebuilder is constructed at 17
    // PRODUCTION sites — 11 in `src/opt/passes` (twice each in `cse.cpp` and
    // `licm.cpp`) and SIX outside it (five `src/mir/merge` synth clones plus the
    // optimizer's own inline-definition strip) — against 15 policy CLASSES,
    // because a pass has exactly one policy TYPE. Per-construction-site strings
    // would let two sites of the SAME pass disagree (or copy-paste a
    // neighbour's), and this is the "a fact with an owner does not get a second
    // owner" rule the deleted `recordTerminatorInRewrite` hook below was removed
    // for violating. The policy is the pass's identity; it owns the pass's name.
    //
    // SPELLING: the pipeline's own name for the pass — `kPassNameTable` in
    // `opt/optimizer.hpp`, i.e. the string `DSS_OPT_TRACE` and a
    // `*.pipeline.json` already use — so a fatal's `[pass=Cse]` is greppable
    // against both. A driver that is NOT a pipeline pass (the `src/mir/merge`
    // synth clones, the optimizer's inline-definition strip) names ITSELF, and
    // that is informative rather than a mismatch: it tells the reader the abort
    // did not come from the optimizer pipeline at all.
    [[nodiscard]] virtual std::string_view passName() const noexcept = 0;

    // Phase 1: returns the block list to walk. Called once per function.
    // ConstFold: all blocks (full module preserve). DCE: RPO-reachable
    // subset. Mem2Reg: all blocks (it inserts Phis, doesn't elide blocks).
    [[nodiscard]] virtual std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) = 0;

    // Phase 2: per-block prologue hook called after `dst.beginBlock(newB)`
    // and before the block's instruction loop runs. Default = no-op.
    // Mem2Reg overrides this to insert Phi placeholders at the head of
    // each block in the iterated dominance frontier of a promotable
    // alloca's def-blocks. The rewrite map is mutable so the hook can
    // record any (oldId → newId) entries the same block's subsequent
    // instructions will consume. `blockMap` is provided so the policy
    // can resolve OLD-block-id refs (e.g. inserted-phi marker keys
    // that the rename walk already mapped to OLD block ids).
    virtual void onBlockBegin(
        MirBlockId /*oldB*/, MirBlockId /*newB*/,
        MirBuilder& /*dst*/,
        std::unordered_map<std::uint32_t, MirInstId>& /*rewrite*/,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) {}

    // Phase 2: pre-emit filter for non-Phi, non-terminator value-producing
    // insts. Returns false → skip this inst entirely (caller's counter
    // increments). ConstFold default = always true; DCE = liveInsts.count.
    [[nodiscard]] virtual bool shouldEmit(MirInstId /*oldId*/) {
        return true;
    }

    // Phase 2: pre-emit rewrite hook. Returns a new MirInstId if the
    // pass handled the inst entirely (rebuilder skips the verbatim copy).
    // Returns nullopt → fall through to verbatim copy.
    // ConstFold: emits Const on fold, returns new id. DCE: nullopt always.
    // Mem2Reg: emits the alloca-slot's current SSA value for Load ops,
    //          omits Store ops (returns "skip" via {InvalidMirInstId});
    //          this distinction is encoded via `shouldEmit` for skips
    //          and `tryRewrite` for substitutions.
    [[nodiscard]] virtual std::optional<MirInstId>
    tryRewrite(MirOpcode /*op*/, MirInstId /*oldId*/,
               MirBuilder& /*dst*/,
               std::unordered_map<std::uint32_t, MirInstId> const& /*rewrite*/) {
        return std::nullopt;
    }

    // Pre-rewrite operand substitution hook (applied BEFORE
    // `rewriteOperand` maps old→new). Default = identity. CopyProp
    // overrides this for SSA Phi-collapse: a Phi whose non-self
    // incomings all resolve to the same value `V` redirects every
    // use of the Phi to `V` directly. The hook works in OLD-id space
    // so the resolution chain (P1 → P2 → V) is transitively
    // path-compressed during analysis on the immutable source MIR;
    // `rewriteOperand` then maps the resolved old target to its NEW
    // id (which has already been emitted in phase 2 by SSA's
    // def-dominates-use invariant + RPO block order). This shape is
    // what allows the Phi-collapse pass to leave the dead Phi
    // instruction in the rebuilt module — a subsequent DCE pass
    // sweeps it because it has zero uses.
    [[nodiscard]] virtual MirInstId
    substituteOldOperand(MirInstId oldOp) {
        return oldOp;
    }

    // Post-rewrite operand substitution hook (applied AFTER
    // `rewriteOperand` resolves old→new). Default = identity.
    // Reserved for per-use dominating-definition substitution
    // (D-OPT-COPYPROP-BLOCK-CURSOR — general operand-substitution
    // variant beyond Phi-collapse).
    [[nodiscard]] virtual MirInstId
    substituteOperand(MirInstId operand) {
        return operand;
    }

    // Per-block "just before the terminator" hook called once per
    // block in phase 2, immediately before the terminator's emit.
    // Default = no-op. LICM overrides to inject hoisted-into-this-
    // block instructions at the end of the block (where the
    // terminator would have appeared in the source). The `rewrite`
    // map is mutable so the hook can record (oldHoistedInstId →
    // newHoistedInstId) entries the subsequent terminator emit (and
    // any downstream consumer in later blocks) will resolve through.
    virtual void onBlockBeforeTerminator(
        MirBlockId /*oldB*/, MirBlockId /*newB*/,
        MirBuilder& /*dst*/,
        std::unordered_map<std::uint32_t, MirInstId>& /*rewrite*/,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) {}

    // Block-merge / absorb hook. If a policy returns `next` for
    // block `oldB`, the rebuilder treats `next`'s instructions as a
    // continuation of `oldB`: after emitting `oldB`'s non-terminator
    // insts, the rebuilder continues with `next`'s non-terminator
    // insts (and chases further `absorbSuccessor(next)`...), then
    // finally emits the LAST absorbed block's terminator as the
    // merged block's terminator. `oldB`'s own terminator is
    // dropped. Default = nullopt (no absorb). SimplifyCFG overrides
    // this to merge `(P, B)` pairs where `P.terminator == Br(B)`,
    // `preds[B] == {P}`, and B has no Phi nodes.
    //
    // The absorbed block (`next`) MUST be excluded from
    // `selectBlocks` so the rebuilder doesn't pre-create it; the
    // policy is responsible for that exclusion. Predecessors of
    // `next` in phi-incomings flow through `redirectBlockTarget`
    // (phase 3 calls it on `inc.pred`) so the surviving block id
    // (the absorb chain head) is used in the rebuilt phi.
    [[nodiscard]] virtual std::optional<MirBlockId>
    absorbSuccessor(MirBlockId /*oldB*/) {
        return std::nullopt;
    }

    // Successor-block redirect hook applied during terminator
    // emission. Default = identity. SimplifyCFG overrides this for
    // empty-block jump-threading: a block B whose only inst is a
    // `Br(S)` (no phi at S uses B as a pred) becomes a "trampoline" —
    // any predecessor branching to B is redirected to S directly,
    // B is elided from `selectBlocks`, and `blockMap_.at(redirected)`
    // succeeds because S is in the surviving-blocks set.
    [[nodiscard]] virtual MirBlockId
    redirectBlockTarget(MirBlockId oldTarget) {
        return oldTarget;
    }

    // Per-terminator-opcode full-replacement hook. Default = nullopt
    // → standard `emitTerminator` arm. SimplifyCFG overrides for
    // branch-folding: `CondBr(Const(true|false), T, F)` → `Br(T|F)`.
    // The hook receives the SAME ids the standard path would consume;
    // returning a new MirInstId records it in the rewrite map (old
    // terminator id → the id it became) and skips the standard emit arm.
    [[nodiscard]] virtual std::optional<MirInstId>
    tryRewriteTerminator(MirOpcode /*op*/, MirInstId /*oldId*/,
                         MirBuilder& /*dst*/,
                         std::unordered_map<std::uint32_t, MirInstId> const& /*rewrite*/,
                         std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) {
        return std::nullopt;
    }

    // Phase 3: per-phi-incoming filter. Default = accept everything.
    // DCE: skip incomings whose pred isn't in the (RPO-reachable)
    // blockMap. SimplifyCFG: skip incomings whose CFG EDGE the
    // branch-fold abandoned — `oldPhiBlock` (the OLD id of the block
    // owning the phi) makes the hook an EDGE-level filter
    // (pred → phi-owner), which a block-level pred check cannot
    // express: a folded CondBr's block stays live, only its edge into
    // the not-taken target dies. ConstFold: identity (all preds
    // reachable, no edges removed).
    [[nodiscard]] virtual bool acceptPhiIncoming(
        MirPhiIncoming const& /*inc*/, MirBlockId /*oldPhiBlock*/,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) {
        return true;
    }

    // Phase 3: called when a phi ended up with zero accepted incomings.
    // Default: structural violation, std::abort with diagnostic
    // naming the OLD function/block/phi (so a maintainer can locate
    // the bad MIR), not just the new arena id.
    //
    // ★ `drivingPassName` IS NOT `this->passName()`, AND THE DIFFERENCE IS THE
    // WHOLE REASON IT IS A PARAMETER. Every hook — this one included — is
    // dispatched through the ACTIVE policy, and for a `MirFunc.noOptimize`
    // function the active policy is `MirIdentityRebuildPolicy` (TF-C85's
    // neuter). `this->passName()` would then report `Identity` for an abort the
    // reader has to locate in a release pipeline, naming the substitute instead
    // of the culprit. The rebuilder passes the name of the policy it was
    // CONSTRUCTED with, which is the pipeline pass in either case. An override
    // must print it (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS).
    virtual void onZeroPhiIncomings(MirInstId oldPhi, MirBlockId oldBlock,
                                    MirFuncId oldFn, MirInstId newPhi,
                                    std::string_view drivingPassName);

    // ★★★ THERE IS NO `recordTerminatorInRewrite` HOOK, AND ITS ABSENCE IS THE
    // FIX FOR A PROCESS ABORT ON A LEGAL PROGRAM
    // (D-OPT-ASM-GOTO-WITH-OUTPUTS-ABORTS-THE-MIR-REBUILDER).
    //
    // The hook used to let a policy declare "nothing downstream reads a
    // terminator as an operand", and two policies (Dce, SimplifyCfg) declared
    // it. ✔MEASURED 2026-08-17 at the CLI: the declaration is FALSE. An
    // `asm goto` WITH OUTPUTS is a `InlineAsmGoto` terminator whose result
    // pieces are `ReturnPiece`s at the heads of its successor blocks, and a
    // `ReturnPiece`'s sole operand ANCHORS IT TO ITS PRODUCER — so a terminator
    // is referenced by another instruction's operand list. With the hook
    // answering false the producer had no rewrite entry and `rewriteOperand`
    // `std::abort`ed: `--config=release` exited **127** on a program that
    // `--config=debug` REFUSES with a clean `L_UnsupportedLoweringForOpcode`
    // (D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER). The trace named the pass:
    // every earlier pass (Identity, ConstFold, Mem2Reg, CopyProp, Cse, Licm)
    // took the `true` default and rebuilt it cleanly; `SimplifyCfg` was the
    // first to answer false, and it died.
    //
    // ★ WHY THE HOOK IS DELETED RATHER THAN RE-ANSWERED. Whether an old id is
    // referenced is a property of THE MIR BEING REBUILT — it is spelled out in
    // the operand lists the rebuilder is already walking — not a property of the
    // pass doing the rebuilding. A policy cannot know it, cannot re-derive it,
    // and gets no signal when a new opcode makes its answer false; the substrate
    // then converts that stale answer into an abort in shared code. This is the
    // "a fact with an owner does not get a second owner" rule: the operand lists
    // own the fact, so the map is simply always complete.
    //
    // ★ AND NEITHER STATED REASON SURVIVED THE MEASUREMENT. Dce's was cost
    // ("saves a few hash inserts" — one per block). SimplifyCfg's was a
    // hypothetical: a consumer looking up a FOLDED `CondBr` would retrieve the
    // replacement `Br` "with wrong shape". But that entry is ACCURATE (the new
    // Br is exactly what that old terminator became), it is unreachable today (a
    // CondBr yields no value and nothing anchors to one), and omitting it does
    // not turn a wrong-shape read into a diagnostic — it turns it into a
    // guaranteed `std::abort`. Recording is the strictly safer of the two.
    //
    // ⚠ THE ONE REMAINING WAY A TERMINATOR LEAVES NO ENTRY is `absorbSuccessor`,
    // which DROPS the absorbed-from block's terminator instead of emitting it.
    // That is sound because the hook's precondition is structural there rather
    // than declared: `absorbSuccessor` fires only for a `P` whose terminator is
    // a plain `Br`, and `opcodeInfo(Br)` has `R::None` with no anchor form, so
    // nothing can reference it. A future policy that absorbs across a
    // value-producing or anchorable terminator must re-map, not drop.

    // ★★ TF-C85: "this function is `noOptimize`; your hooks will NOT be called
    // for it." Fired by `MirFunctionRebuilder::rebuildFunction` immediately
    // before it swaps in the identity policy, i.e. BEFORE the block walk and
    // AFTER the pass's own per-function analysis has already run.
    //
    // Default no-op, and that is CORRECT for every policy whose per-function
    // state is consumed ONLY through the hooks above: not calling the hooks IS
    // the neuter, and the unused analysis is simply discarded.
    //
    // It exists for the one shape where that is not true — a policy that also
    // has a POST-rebuild step reading state the hooks were supposed to produce.
    // `Mem2Reg` is exactly that: its `analyze` plans IDF phis, `onBlockBegin`
    // emits them, and `finalizePhiIncomings` (called by the pass driver after
    // the rebuild) wires their incomings. MEASURED on the real pe64 sqlite
    // corpus, neutering without this hook aborts loudly in that finalize step
    // ("phi marker has incomings but no emitted phi") — so the hook is not
    // defensive tidiness, it is the fix for a reproduced crash. Such a policy
    // overrides this to DROP the per-function plan.
    virtual void onFunctionNeutered(MirFuncId /*oldFn*/) {}

    // ★★ TF-C85: "I am a MANDATORY NORMALIZATION, not an optimization — do not
    // neuter me for a `noOptimize` function." Default FALSE, because the
    // overwhelmingly common case is a policy that IS an optimization.
    //
    // ★ WHY THE EXEMPTION EXISTS, MEASURED RATHER THAN ANTICIPATED. The first
    // implementation of the neuter had no exemption, and the real pe64 sqlite
    // corpus went from 2135 pragma errors to FOUR `I_UnreachableBlock` verifier
    // errors in `ext/misc/totype.c` — reproduced on that TU alone. The cause:
    // `PruneUnreachable` is not an optimization at all (its own header says
    // MANDATORY, its anchor is D-MIR-UNREACHABLE-PRUNE-NORMALIZE, and it is not
    // even listed in `release.pipeline.json`). It is the post-lowering pass that
    // funnels every eager-continuation construct — dead code after `return`,
    // `break`, `&&`/`||` — into a CFG the MIR verifier will accept. Neutering it
    // does not "leave the function unoptimized", it leaves the function
    // ILL-FORMED.
    //
    // ★ AND THAT IS THE HONEST LINE, NOT A CONVENIENT ONE: `#pragma optimize`
    // switches OPTIMIZATION off, and it has never meant "emit invalid code".
    // MSVC at `/Od` still produces a well-formed function. A pass that
    // establishes an IR invariant every downstream tier depends on is outside
    // what the pragma controls, and it says so here rather than by omission.
    [[nodiscard]] virtual bool mandatoryNormalization() const noexcept {
        return false;
    }
};

// The concrete rebuilder. Owns the rewrite map + block map. Drives
// the 3-phase walk + calls into `policy` at the documented hook
// points. Pass-specific counters live on the policy (the rebuilder
// is policy-agnostic about WHY a hook returned false; counting is the
// policy's concern).
// ★★ TF-C85: THE NEUTERED POLICY. Every hook above already has an
// identity/verbatim DEFAULT — this class is nothing but those defaults plus the
// one hook that has no default (`selectBlocks`, answered with ALL of the
// function's blocks in natural order). Driving `rebuildFunction` with THIS
// policy therefore produces an exact copy of the source function: no fold, no
// promotion, no skip, no absorb, no redirect, no operand substitution.
//
// It is what a `MirFunc.noOptimize` function gets, and it is why the opt-out can
// be honored WITHOUT the rebuilder ever skipping a function: skipping the
// `rebuildFunction` call would not preserve the function, it would DELETE it
// from the rebuilt module (`dst_` is a fresh builder — a function that is never
// added simply does not exist downstream, and its callers would then reference a
// symbol with no definition).
class DSS_EXPORT MirIdentityRebuildPolicy final : public MirRebuildPolicy {
public:
    // The pipeline's spelling of the pass this policy IS when a caller uses it
    // directly (`kPassNameTable`: `PassId::Identity` → "Identity"; the debug
    // pipeline is literally `["Identity"]`).
    //
    // ⚠ NOT the name reported when this policy is the TF-C85 NEUTER substitute
    // inside another pass's rebuilder — there the fatals print the pass the
    // rebuilder was constructed with, so a `noOptimize` function aborting under
    // `SimplifyCfg` still says `SimplifyCfg`. See `onZeroPhiIncomings`'
    // `drivingPassName`. Do not "fix" this string to cover that case; it would
    // mis-name the direct use instead.
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "Identity";
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override;
};

class DSS_EXPORT MirFunctionRebuilder {
public:
    MirFunctionRebuilder(Mir const& src, MirBuilder& dst,
                         MirRebuildPolicy& policy)
        : src_(src), dst_(dst), policy_(policy), active_(&policy) {}

    // Run the 3-phase rebuild for a single old-module function.
    void rebuildFunction(MirFuncId oldFn);

    // Post-rebuild accessors so a pass that inserts NEW SSA values
    // (e.g. Mem2Reg's IDF phis — net-new defs that don't correspond
    // to any old-module instruction) can resolve old→new ids when
    // wiring up their own phi incomings. Read-only; the rebuilder
    // remains the sole writer of these maps during rebuild.
    [[nodiscard]] std::unordered_map<std::uint32_t, MirInstId> const&
    rewriteMap() const noexcept { return rewrite_; }
    [[nodiscard]] std::unordered_map<std::uint32_t, MirBlockId> const&
    blockMap() const noexcept { return blockMap_; }

private:
    [[nodiscard]] MirInstId rewriteOperand(MirInstId oldOp) const;
    // Operand mapping composition: `substituteOldOperand` (OLD-id
    // pre-rewrite redirect — e.g. CopyProp Phi-collapse, CSE value
    // numbering) → `rewriteOperand` (old→new arena translation) →
    // `substituteOperand` (NEW-id post-rewrite redirect — reserved
    // for future general copy-prop). Single helper at every operand-
    // resolution site so the 3-step composition stays uniform.
    [[nodiscard]] MirInstId mapOperand(MirInstId oldOp) const {
        return active_->substituteOperand(
            rewriteOperand(active_->substituteOldOperand(oldOp)));
    }
    void emitValue(MirOpcode op, MirInstId oldId);
    void emitTerminator(MirOpcode op, MirInstId oldId);

    Mir const&        src_;
    MirBuilder&       dst_;
    // The pass's own policy — the one this rebuilder was constructed with.
    MirRebuildPolicy& policy_;
    // ★★ TF-C85: the policy IN EFFECT for the function currently being rebuilt.
    // `&policy_` normally; `&identity_` for the duration of a `noOptimize`
    // function. EVERY hook call in the rebuild goes through this pointer, which
    // is the property that makes the neuter total: a hook still reached through
    // `policy_` would be a pass-specific edit applied to a function the source
    // excluded from optimization.
    MirRebuildPolicy* active_ = nullptr;
    MirIdentityRebuildPolicy identity_;
    std::unordered_map<std::uint32_t, MirInstId>  rewrite_;
    std::unordered_map<std::uint32_t, MirBlockId> blockMap_;
};

} // namespace dss::opt::passes
