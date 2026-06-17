#include "opt/passes/inlining.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "opt/analysis/call_graph_scc.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss::opt::passes {

namespace {

// ── module-level call-graph + address-escape analysis ──────────────
//
// Built ONCE per `runInlining` over the immutable source module. The
// inliner consults both during the rebuild's `tryRewrite` hook to
// decide §2.9 legality for each call site.
struct ModuleAnalysis {
    // SymbolId.v → MirFuncId for every DEFINED function in the module.
    // A function whose symbol the call graph cannot resolve here is
    // external (extern decl, library import) — NOT inlinable (rule 1).
    std::unordered_map<std::uint32_t, MirFuncId> symToFunc;
    // SymbolId.v values whose function address ESCAPES — i.e. at least
    // one live `GlobalAddr(sym)` in the module is used as something
    // other than operand[0] of a Call. A callee in this set is refused
    // (rule 4): an indirect call could reach it, so its out-of-line
    // body must be preserved and inlining is unsafe/incomplete.
    std::unordered_set<std::uint32_t> addressEscaped;
    // MirFuncId.v → strongly-connected-component id of the direct call
    // graph (OPT7 cycle 3). The gate refuses inlining any call whose
    // caller + callee share an SCC — generalizing the cycle-1/2 SELF-
    // recursion refusal to MUTUAL recursion (`f→g→f`). Built once by
    // `computeCallGraphSccs` over the immutable source module. Keyed by
    // `funcId.v` (NOT funcSymbol) so the equality test is over the
    // module's own function identities.
    std::unordered_map<std::uint32_t, std::uint32_t> funcToScc;
};

// True iff `inst` is a `GlobalAddr` whose symbol is the callee slot
// (operand[0]) of `user`. Used to decide whether a given GlobalAddr
// use is a "pure call target" (no escape) vs. an escaping reference.
[[nodiscard]] bool
isCalleeOperandOf(Mir const& mir, MirInstId globalAddr, MirInstId user) {
    if (mir.instOpcode(user) != MirOpcode::Call) return false;
    auto const ops = mir.instOperands(user);
    // operand[0] is the callee; operands[1..] are arguments. A
    // GlobalAddr appearing as an ARGUMENT (a passed function pointer)
    // is an escape, not a call target.
    return !ops.empty() && ops[0] == globalAddr;
}

[[nodiscard]] ModuleAnalysis analyzeModule(Mir const& mir) {
    ModuleAnalysis a;

    // (1) symbol → defined-function map. Duplicate symbols (weak
    // aliases / COMDAT / hand-built fixtures) are NOT supported by the
    // direct-resolution model: keep only the FIRST and mark the symbol
    // ambiguous by removing it from the map so no call resolves to a
    // guessed body. (Refusing on ambiguity is the conservative, never-
    // miscompile choice — distinct from DCE which std::aborts; the
    // optimizer must not crash a user's build over a legal-but-unusual
    // module, it just declines to inline.)
    std::size_t const nf = mir.moduleFuncCount();
    std::unordered_set<std::uint32_t> ambiguousSyms;
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const sv = mir.funcSymbol(f).v;
        if (sv == 0) continue;  // anonymous — never an inline target
        auto const [it, inserted] = a.symToFunc.emplace(sv, f);
        if (!inserted) ambiguousSyms.insert(sv);
    }
    for (std::uint32_t sv : ambiguousSyms) a.symToFunc.erase(sv);

    // (2) address-escape scan over EVERY instruction of EVERY function.
    // For each GlobalAddr, every USE must be a callee operand or the
    // symbol escapes. We scan users by walking all instructions and
    // testing their operands against each GlobalAddr in the same block-
    // walk. To keep it O(operands) we instead invert: for each Call we
    // record its callee-operand GlobalAddr as "seen as callee"; for
    // each NON-callee operand reference to a GlobalAddr we mark escape.
    //
    // Concretely: walk all instructions; for any operand that is a
    // GlobalAddr, mark the GlobalAddr's symbol as escaped UNLESS this
    // user is a Call and the operand is its callee slot. A GlobalAddr
    // with no uses at all does not escape (a later DCE drops it).
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const user = mir.blockInstAt(b, ii);
                // Phi operands address the phi pool — a function-address
                // flowing through a Phi is an escape (it could be called
                // indirectly through the merged value). Treat every phi
                // incoming value that is a GlobalAddr as escaped.
                if (mir.instOpcode(user) == MirOpcode::Phi) {
                    for (auto const& inc : mir.phiIncomings(user)) {
                        if (mir.instOpcode(inc.value) == MirOpcode::GlobalAddr) {
                            a.addressEscaped.insert(
                                mir.globalAddrSymbol(inc.value).v);
                        }
                    }
                    continue;
                }
                auto const ops = mir.instOperands(user);
                for (MirInstId const op : ops) {
                    if (mir.instOpcode(op) != MirOpcode::GlobalAddr) continue;
                    if (isCalleeOperandOf(mir, op, user)) continue;
                    a.addressEscaped.insert(mir.globalAddrSymbol(op).v);
                }
            }
        }
    }

    // (3) call-graph SCCs — the recursion-safety substrate (OPT7 cycle
    // 3). Self-contained over `mir` (no target/format/language input);
    // the gate uses it to refuse inlining any call inside a recursive
    // cycle (self OR mutual).
    a.funcToScc = analysis::computeCallGraphSccs(mir);
    return a;
}

// Resolve a Call's callee operand to a DEFINED callee function id, or
// nullopt if the callee is not a direct GlobalAddr-to-defined-function.
[[nodiscard]] std::optional<MirFuncId>
resolveDirectCallee(Mir const& mir, ModuleAnalysis const& a,
                    MirInstId callId) {
    auto const ops = mir.instOperands(callId);
    if (ops.empty()) return std::nullopt;
    MirInstId const callee = ops[0];
    if (mir.instOpcode(callee) != MirOpcode::GlobalAddr) {
        return std::nullopt;  // indirect call — not inlinable
    }
    SymbolId const sym = mir.globalAddrSymbol(callee);
    auto const it = a.symToFunc.find(sym.v);
    if (it == a.symToFunc.end()) return std::nullopt;  // external / ambiguous
    return it->second;
}

// The §2.9 legality gate. Returns the callee function id IFF the call
// at `callId` in `caller` is legal to inline under this cycle's LOCKED
// scope; nullopt = conservatively REFUSE (leave the call as-is).
[[nodiscard]] std::optional<MirFuncId>
inlineLegalityGate(Mir const& mir, ModuleAnalysis const& a,
                   MirFuncId caller, MirInstId callId,
                   std::uint32_t inlineThreshold) {
    // Rule 1: direct call to a defined callee in this module.
    auto const calleeOpt = resolveDirectCallee(mir, a, callId);
    if (!calleeOpt.has_value()) return std::nullopt;
    MirFuncId const callee = *calleeOpt;

    // Rule 2: THE correctness rule — never inline a Weak callee. A
    // strong definition of the same name may replace it at link.
    if (mir.funcBinding(callee) == SymbolBinding::Weak) return std::nullopt;

    // Rule 3: never inline a call WITHIN A RECURSIVE CYCLE (OPT7 cycle 3).
    // The call graph's SCCs collapse every recursive cycle to one id; a
    // call whose caller + callee share an SCC is part of a cycle, so
    // inlining it would unroll an unbounded recursion at inline time.
    // This SUBSUMES the old self-recursion check: a self-call (caller ==
    // callee) is the singleton-SCC case — caller.v and callee.v map to
    // the SAME scc id, so the equality below catches it — AND it also
    // refuses MUTUAL recursion (`f→g→f`: f and g share a multi-member
    // SCC), which the old funcSymbol-equality check missed. Keyed by
    // funcId.v (the SCC map's key), not funcSymbol. (Admitting BOUNDED
    // recursion behind a depth policy is deferred — each bounded form
    // needs its own miscompile pin; D-OPT7-INLINE-LEGALITY-GATE.)
    if (a.funcToScc.at(caller.v) == a.funcToScc.at(callee.v)) {
        return std::nullopt;
    }

    // Rule 4: refuse if the callee's address escapes anywhere in the
    // module (a function pointer could call it indirectly).
    if (a.addressEscaped.count(mir.funcSymbol(callee).v)) return std::nullopt;

    // Rule 5: the callee-body scope gate. OPT7 cycle 2 LIFTED the single-
    // block restriction (general multi-block callees inline via the CFG-
    // clone + return-merge-Phi machinery below); OPT7 cycle 3 LIFTS the
    // NON-LEAF restriction — a callee whose body contains a regular `Call`
    // is now ADMITTED. The splice's generic arm (`emitCalleeInst`) already
    // clones an inner `Call` correctly: its operands remap through the
    // `local` map and its callee `GlobalAddr` re-emits with the SAME
    // symbol, so the inlined-in copy still targets the same function. The
    // recursion danger a non-leaf body would introduce is handled by rule
    // 3 (the SCC refusal), NOT by a leaf restriction — a non-recursive
    // call chain inlines safely one level per pass, `maxIterations`-
    // bounded. OPT7 cycle 6 LIFTS the `IntrinsicCall` restriction too — a
    // callee whose body contains an `IntrinsicCall` is now ADMITTED (it
    // clones SSA-correctly via the same generic arm; the per-op check below
    // carries the frame-sensitivity caveat + its trigger-gated anchor
    // D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC). OPT7 (D-OPT7-MULTIBLOCK-
    // SPLICE-PHI) LIFTS the callee-`Phi` refusal too — a multi-block callee
    // that carries a `Phi` at a real CFG merge (a value-producing `?:` /
    // `&&` / `||` lowers to a MIR Phi BEFORE Mem2Reg; or a post-Mem2Reg
    // join / loop header) is now ADMITTED. `spliceMultiBlock` clones each
    // callee Phi via a DEFERRED flush (placeholder in the clone loop, then
    // its incomings remapped through the value (`local`) + block
    // (`calleeBlockMap`) maps AFTER the loop) — the SAME deferral discipline
    // the caller's own phis already use (phase 3) and the SIBLING of the
    // `returnEdges` flush, with DISJOINT maps so the two compose. The
    // deferral is UNIFORM across phi shapes: a loop/back-edge phi resolves
    // for free (its back-edge incoming value is defined later in RPO, so
    // `local` is complete only after the loop — exactly when the flush
    // runs). What STAYS refused:
    //   * A callee with NO RETURNING PATH (no `Return` in ANY block; every
    //     path ends in `Unreachable` — e.g. `int f(){ while(1){} }`). The
    //     multi-block splice routes each callee `Return` to a continuation
    //     block; a callee that never returns leaves that continuation with
    //     ZERO predecessors, which the MirVerifier (run after every pass)
    //     flags → the whole module is REJECTED → an otherwise-valid program
    //     becomes a build error UNDER inlining. Refusing here keeps the
    //     Call out-of-line so the program still compiles (conservative,
    //     never a miscompile — D-OPT7-MULTIBLOCK-SPLICE non-returning note).
    // Plus the arity safety gate (unchanged): every `Arg(i)` the callee
    // references must be in range of the actual args the call passes —
    // an out-of-range index is a structural call/signature mismatch.
    // A callee that references FEWER args than it declares is still
    // inlinable; only an out-of-range index disqualifies.
    auto const callOps = mir.instOperands(callId);
    std::size_t const callArgCount = callOps.empty() ? 0 : callOps.size() - 1;
    bool hasReturn = false;  // at least one returning path (rule 5, above)
    std::uint32_t instCount = 0;  // COST MODEL: total callee instructions
    std::uint32_t const nb = mir.funcBlockCount(callee);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const cb = mir.funcBlockAt(callee, bi);
        std::uint32_t const ni = mir.blockInstCount(cb);
        for (std::uint32_t i = 0; i < ni; ++i) {
            MirInstId const cid = mir.blockInstAt(cb, i);
            MirOpcode const op  = mir.instOpcode(cid);
            ++instCount;  // every callee inst counts toward the size bound
            // OPT7 cycle 3: a regular `Call` is NO LONGER a refusal — a
            // non-leaf callee whose body contains a direct/indirect `Call`
            // is now admitted (its inner Call clones correctly via the
            // splice's generic arm; recursion is caught by rule 3's SCC
            // refusal, above). OPT7 cycle 6: an `IntrinsicCall` is likewise
            // NO LONGER a refusal — it clones SSA-correctly via the SAME
            // generic arm. The intrinsic id lives in the inst PAYLOAD (a
            // module-stable integer the clone copies verbatim); its operands
            // (the args) remap through the `local` map like any other op; no
            // IntrinsicCall-specific field exists that the generic arm drops.
            // FRAME-SENSITIVITY caveat: a hypothetical frame-sensitive
            // intrinsic (va_start / frameaddress / setjmp-class) would, if
            // inlined, bind to the CALLER's frame instead of the callee's —
            // a miscompile. No such intrinsic exists or is emitted by any
            // shipped frontend today: the intrinsic registry has no inline-
            // safety attribute and is EMPTY through every real compile (no
            // sema/lowering path registers or emits an intrinsic; only the
            // HIR text format can, in tests). That precondition is pinned by
            // a fail-loud tripwire (the c-subset lowering test asserting an
            // empty registry), so blanket admission is correct for the
            // current intrinsic model. Gating on a per-intrinsic inline-
            // safety attribute is trigger-gated to the first frame-sensitive
            // intrinsic — D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC.
            //
            // OPT7 (D-OPT7-MULTIBLOCK-SPLICE-PHI): a callee `Phi` is NO
            // LONGER a refusal — `spliceMultiBlock` clones it via a deferred
            // flush (see rule 5, above). A Phi only ever appears in a MULTI-
            // block callee (a single-block leaf is Return-terminated, no
            // merge), so the multi-block CFG-clone path is the only one that
            // can encounter one; it still counts as 1 instruction here (the
            // cost model is unchanged — Phis are not special-cased).
            if (op == MirOpcode::Return) hasReturn = true;
            if (op == MirOpcode::Arg && mir.argIndex(cid) >= callArgCount) {
                return std::nullopt;  // arg index out of range → refuse
            }
        }
    }
    // No returning path → the splice's continuation block would be
    // predecessor-less → MirVerifier rejects the module. Refuse (the Call
    // stays; the program compiles). See rule 5, above.
    if (!hasReturn) return std::nullopt;

    // COST MODEL (rule 6 — OPT7 cycle 28): a size-based profitability
    // gate. Inline only if the callee is no larger than `inlineThreshold`
    // instructions; a bigger callee is conservatively REFUSED (too large
    // to inline profitably). This is what bounds the code-size growth from
    // shipping `Inlining` in `release.pipeline.json`. `>` (not `>=`): a
    // callee of EXACTLY `inlineThreshold` instructions still inlines; one
    // instruction over is refused. FAIL-SAFE: a threshold below the
    // smallest callee — including 0, if constructed programmatically (the
    // loader rejects 0) — refuses everything; nothing miscompiles.
    if (instCount > inlineThreshold) return std::nullopt;
    return callee;
}

// ── shared splice helpers (free functions; consumed by BOTH the
// single-block linear path AND the multi-block CFG-clone path) ─────
//
// The local map is calleeOld-inst-.v → callerNew-inst id, populated in
// the callee's def-before-use order. `malformed` is a shared sticky
// flag the arity-mismatch guard sets; the top-level `runInlining`
// emits `X_InlineMalformedCallSite` + discards the module when set, so
// neither path produces a wrong-arity splice.

// Map a callee-block operand to its caller-NEW value via the LOCAL map.
[[nodiscard]] MirInstId
mapCalleeOperand(Mir const& src, MirInstId calleeOp,
                 std::unordered_map<std::uint32_t, MirInstId> const& local,
                 MirFuncId callee) {
    (void)src;
    auto const it = local.find(calleeOp.v);
    if (it == local.end()) {
        std::fprintf(stderr,
            "dss::opt::passes::Inlining fatal: callee operand v=%u "
            "(callee funcId v=%u) has no local mapping during splice "
            "— def-before-use violation in the callee body.\n",
            calleeOp.v, callee.v);
        std::abort();
    }
    return it->second;
}

// Map one of the CALLER Call's own operands (callee address or an
// actual argument) to its caller-NEW value via the rewrite map.
[[nodiscard]] MirInstId
mapCallerOperand(MirInstId callerOp,
                 std::unordered_map<std::uint32_t, MirInstId> const& rewrite,
                 MirInstId oldCall) {
    auto const it = rewrite.find(callerOp.v);
    if (it == rewrite.end()) {
        std::fprintf(stderr,
            "dss::opt::passes::Inlining fatal: caller Call v=%u "
            "operand v=%u has no rewrite entry during splice — "
            "operands must be emitted before the Call in block/RPO "
            "order (D-OPT2-REWRITE-MAP-COMPLETENESS).\n",
            oldCall.v, callerOp.v);
        std::abort();
    }
    return it->second;
}

// Compute the Call's actual arguments as caller-NEW values. Call
// operands are [calleeGlobalAddr, arg0, arg1, ...]; operand[0] (the
// callee GlobalAddr) is intentionally dropped — inlining removes the
// indirect-through-address call entirely.
[[nodiscard]] std::vector<MirInstId>
mapActualArgs(Mir const& src, MirInstId oldCall,
              std::unordered_map<std::uint32_t, MirInstId> const& rewrite) {
    auto const callOps = src.instOperands(oldCall);
    std::vector<MirInstId> actualArgs;
    actualArgs.reserve(callOps.size() > 0 ? callOps.size() - 1 : 0);
    for (std::size_t i = 1; i < callOps.size(); ++i) {
        actualArgs.push_back(mapCallerOperand(callOps[i], rewrite, oldCall));
    }
    return actualArgs;
}

// Re-emit one CALLEE instruction into the caller's currently-open
// block. `Arg(i)` → the actual argument; Const / GlobalAddr re-emit
// through their dedicated builders; every other op re-emits via
// `addInst` with operands mapped through `local`. Records the
// calleeOld→callerNew mapping into `local`. A callee `Phi` must NEVER
// reach here: `spliceMultiBlock` handles callee Phis BEFORE dispatching
// to this helper (a placeholder-then-deferred-flush, like the caller's
// own phis), and a single-block leaf is Return-terminated so it provably
// has no Phi — so a Phi reaching this helper is a structural violation
// (defensive fail-loud guard, below). An out-of-range `Arg` index sets
// `malformed` (the gate guarantees it can't happen; this is the
// defensive fail-loud guard).
void emitCalleeInst(Mir const& src, MirInstId cid, MirOpcode cop,
                    MirBuilder& dst,
                    std::vector<MirInstId> const& actualArgs,
                    std::unordered_map<std::uint32_t, MirInstId>& local,
                    MirFuncId callee, bool& malformed) {
    if (cop == MirOpcode::Arg) {
        std::uint32_t const idx = src.argIndex(cid);
        if (idx >= actualArgs.size()) {
            malformed = true;
            if (!actualArgs.empty()) local.emplace(cid.v, actualArgs[0]);
            return;
        }
        local.emplace(cid.v, actualArgs[idx]);
        return;
    }
    if (cop == MirOpcode::Const) {
        local.emplace(cid.v, dst.addConst(
            src.literalValue(src.constLiteralIndex(cid)), src.instType(cid)));
        return;
    }
    if (cop == MirOpcode::GlobalAddr) {
        local.emplace(cid.v, dst.addGlobalAddr(
            src.globalAddrSymbol(cid), src.instType(cid)));
        return;
    }
    if (cop == MirOpcode::Phi) {
        std::fprintf(stderr,
            "dss::opt::passes::Inlining fatal: callee funcId v=%u Phi "
            "reached emitCalleeInst — spliceMultiBlock handles callee Phis "
            "BEFORE dispatching here (deferred placeholder + flush), and a "
            "single-block leaf is Return-terminated so it has no Phi; a Phi "
            "here is a structural violation (D-OPT7-MULTIBLOCK-SPLICE-PHI).\n",
            callee.v);
        std::abort();
    }
    auto const cops = src.instOperands(cid);
    std::vector<MirInstId> newOps;
    newOps.reserve(cops.size());
    for (MirInstId const o : cops) {
        newOps.push_back(mapCalleeOperand(src, o, local, callee));
    }
    local.emplace(cid.v, dst.addInst(cop, newOps, src.instType(cid),
                                     src.instPayload(cid), src.instFlags(cid)));
}

// The single-block-leaf inlining rebuild policy (OPT7 cycle 1). Per-
// function `analyze` decides which Call ids in THIS function are
// single-block-leaf inline targets; the rebuilder's `tryRewrite` hook
// performs the LINEAR splice into the call's own block. A function
// containing ANY multi-block inline target is routed through
// `MultiBlockInliner` instead (see `runInlining`), so this policy only
// ever sees single-block targets — preserving the cycle-1 behavior
// (and its tests + the Weak-inline pin) byte-for-byte.
class InliningPolicy final : public MirRebuildPolicy {
public:
    InliningPolicy(Mir const& src, ModuleAnalysis const& analysis,
                   std::uint32_t inlineThreshold) noexcept
        : src_(src), analysis_(analysis), inlineThreshold_(inlineThreshold) {}

    [[nodiscard]] std::size_t callsInlined() const noexcept {
        return callsInlined_;
    }
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

    // Compute the per-function single-block-leaf inline plan: callId →
    // callee. Only callees with exactly one block are admitted here;
    // multi-block targets are handled by `MultiBlockInliner`.
    void analyze(MirFuncId caller) {
        plan_.clear();
        std::uint32_t const nb = src_.funcBlockCount(caller);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = src_.funcBlockAt(caller, bi);
            std::uint32_t const ni = src_.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const id = src_.blockInstAt(b, ii);
                if (src_.instOpcode(id) != MirOpcode::Call) continue;
                auto const callee =
                    inlineLegalityGate(src_, analysis_, caller, id,
                                       inlineThreshold_);
                if (!callee.has_value()) continue;
                if (src_.funcBlockCount(*callee) != 1) continue;  // multi-block path
                plan_.emplace(id.v, *callee);
            }
        }
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        // Inlining preserves the caller's CFG entirely — it only
        // rewrites Call instructions in place. Walk every block in
        // source order; no block is elided or inserted (single-block
        // leaf splices LINEARLY into the call's own block).
        std::vector<MirBlockId> blocks;
        std::uint32_t const nb = src.funcBlockCount(fn);
        blocks.reserve(nb);
        for (std::uint32_t i = 0; i < nb; ++i) {
            blocks.push_back(src.funcBlockAt(fn, i));
        }
        return blocks;
    }

    [[nodiscard]] std::optional<MirInstId>
    tryRewrite(MirOpcode op, MirInstId oldId,
               MirBuilder& dst,
               std::unordered_map<std::uint32_t, MirInstId> const& rewrite) override {
        if (op != MirOpcode::Call) return std::nullopt;
        auto const it = plan_.find(oldId.v);
        if (it == plan_.end()) return std::nullopt;  // not selected → verbatim Call
        return spliceCallee(it->second, oldId, dst, rewrite);
    }

private:
    // Splice the single-block leaf `callee` body in place of the Call
    // `oldCall`. Returns the value the Call produced (the threaded
    // callee return value), or InvalidMirInstId for a void callee with
    // no return value — in which case the rebuilder records the mapping
    // but nothing downstream reads it (a void Call's result is never
    // used as an operand). Reads the caller's `rewrite` map (const) to
    // resolve the Call's actual arguments; emits the spliced body into
    // the caller's CURRENTLY-OPEN block via `dst`.
    [[nodiscard]] MirInstId
    spliceCallee(MirFuncId callee, MirInstId oldCall, MirBuilder& dst,
                 std::unordered_map<std::uint32_t, MirInstId> const& rewrite) {
        std::vector<MirInstId> const actualArgs =
            mapActualArgs(src_, oldCall, rewrite);

        MirBlockId const cb = src_.funcEntry(callee);
        std::uint32_t const ni = src_.blockInstCount(cb);

        // Walk the callee block: copy each NON-terminator instruction
        // into the caller via a LOCAL calleeOld→callerNew map. The
        // terminator (a Return) is handled after the loop.
        std::unordered_map<std::uint32_t, MirInstId> local;
        local.reserve(ni);
        MirInstId result{};  // invalid until a Return value is mapped
        for (std::uint32_t i = 0; i < ni; ++i) {
            MirInstId const cid = src_.blockInstAt(cb, i);
            MirOpcode const cop = src_.instOpcode(cid);

            if (opcodeInfo(cop).isTerminator) {
                if (cop == MirOpcode::Return) {
                    auto const rops = src_.instOperands(cid);
                    if (!rops.empty()) {
                        result = mapCalleeOperand(src_, rops[0], local, callee);
                    }
                    // void Return → result stays invalid (no value).
                } else {
                    // A single-block leaf's only legal terminator is a
                    // Return (no Br/CondBr/Switch with one block; an
                    // Unreachable would mean the callee never returns,
                    // which the gate doesn't special-case — refuse via
                    // fail-loud rather than splice a non-returning body
                    // whose "result" is undefined).
                    std::fprintf(stderr,
                        "dss::opt::passes::Inlining fatal: single-block "
                        "callee funcId v=%u terminates with non-Return "
                        "opcode %d — the legality gate admitted a body "
                        "the splice can't thread a return value through "
                        "(D-OPT7-INLINE-LEGALITY-GATE).\n",
                        callee.v, static_cast<int>(cop));
                    std::abort();
                }
                break;  // terminator is the last inst
            }

            // Ordinary callee instruction (incl. Arg): re-emit into the
            // caller's open block via the shared helper.
            emitCalleeInst(src_, cid, cop, dst, actualArgs, local, callee,
                           malformed_);
        }

        ++callsInlined_;
        return result;
    }

    Mir const&            src_;
    ModuleAnalysis const& analysis_;
    std::uint32_t         inlineThreshold_;  // COST MODEL size bound
    // Per-function inline plan: caller Call old-id .v → callee MirFuncId.
    std::unordered_map<std::uint32_t, MirFuncId> plan_;
    std::size_t callsInlined_ = 0;
    bool        malformed_    = false;
};

// ── multi-block (general LEAF) inlining rebuild (OPT7 cycle 2) ──────
//
// A `tryRewrite`-style hook (cycle 1) maps ONE Call to ONE value and
// runs mid-block-fill — it structurally CANNOT create blocks or split
// the call-site block, which a multi-block splice requires. So a
// function containing ANY multi-block inline target is rebuilt by this
// dedicated routine instead of `MirFunctionRebuilder`.
//
// The inliner preserves the caller's CFG (it only rewrites Calls), so
// each ORIGINAL caller block keeps its identity as a branch target
// (`blockMap[old] → new entry`). A multi-block splice SPLITS the host
// block at the Call: instructions before the Call stay; instructions
// after move to a fresh CONTINUATION block. Between them the callee's
// CFG is CLONED (fresh block + inst ids), every callee `Return` becomes
// a `Br` to the continuation, and a RETURN-MERGE PHI in the
// continuation joins the per-return values — that Phi IS the Call's
// result. Because a caller block can be split more than once (multiple
// inline Calls), the block that finally carries the original block's
// terminator is tracked separately (`blockExitMap[old] → last cont`),
// and caller Phi incomings redirect their `pred` through it.
//
// StructCfMarker: markers are NOT maintained through the rebuild — after
// it, `runInlining` re-stamps EVERY block from the canonical CFG
// derivation (`rederiveStructCfMarkers`, mir_struct_markers.hpp), so an
// inlined callee's loop headers RE-EMERGE as `LoopHeader` in the caller
// (the splice preserves the back-edges, and the derivation reads them)
// and an inlined if-diamond re-derives IfThen/IfElse/IfJoin around the
// caller's post-dominator structure. Every block is therefore CREATED
// with a creation-time default marker only (caller blocks keep their
// source marker, cloned/continuation blocks are `Linear`); the post-
// finish re-derivation is the single source of marker truth. The
// verifier checks stored == derived per reachable block, which the
// final stamping satisfies by construction.
//
// LAYOUT (the C1 contract — D-OPT2 layout class, MirVerifier I0010):
// every block is pre-created in FINAL-LAYOUT order BEFORE any fill, so
// the function's block layout (= `funcBlockAt` / creation order) is
// TOPOLOGICAL for every flow a splice can produce. Phase 1 walks each
// caller block in layout order, creating that caller block, then — for
// every plan'd MULTI-BLOCK call in it (scanned in inst order) — the
// splice's cloned-callee blocks (callee-RPO) FOLLOWED BY its
// continuation block. The resulting layout is
// `[A', clones1, cont1, clones2, cont2, B', ...]`: a clone-defined value
// consumed in a continuation (the 1-return degenerate-Phi-elision case),
// a continuation-Phi consumed in a later caller block (a post-Mem2Reg
// iteration-2 caller), and an earlier call's result consumed as a later
// call's argument all have their def laid out before their use. Without
// this, a ≥2-return merge Phi laundered the cross-layout flow (Phi
// operands are layout-exempt) but the EXACTLY-1-return elision fed a
// clone-defined value to the continuation as a NON-Phi operand —
// dominance-valid (the clone dominates the continuation) but
// layout-inverted, which no linear consumer (rebuilder rewrite map,
// mir_to_lir regForValue) can resolve. The MirVerifier's layout rule
// now enforces this contract at every producing pass.
//
// SSA: callee blocks are emitted in RPO from the callee entry (a valid
// def-before-use order for a Phi-free SSA body — every def dominates
// its uses, and RPO visits a block's dominators first), so the shared
// calleeOld→callerNew `local` map is always populated before a use.
// The engine's verify-after-every-pass hook re-runs MirVerifier, so any
// splice that broke SSA dominance / Phi completeness / a CFG edge / the
// layout contract is a build break, never a runtime miscompile.
class MultiBlockInliner {
public:
    // `plan` (callId .v → callee) is precomputed by `planHasMultiBlock`
    // (it already ran the §2.9 gate per call); this rebuilder consumes
    // it by reference, so its lifetime must outlive the rebuild.
    MultiBlockInliner(Mir const& src, MirBuilder& dst,
                      std::unordered_map<std::uint32_t, MirFuncId> const& plan)
        : src_(src), dst_(dst), plan_(plan) {}

    [[nodiscard]] std::size_t callsInlined() const noexcept { return callsInlined_; }
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

    void rebuildFunction(MirFuncId caller) {
        dst_.addFunction(src_.funcSignature(caller), src_.funcSymbol(caller),
                         src_.funcBinding(caller), src_.funcVisibility(caller));

        std::uint32_t const nb = src_.funcBlockCount(caller);

        blockMap_.clear();
        blockExitMap_.clear();
        rewrite_.clear();
        deferredPhis_.clear();
        splicePlans_.clear();

        // Phase 1: pre-create EVERY block in FINAL-LAYOUT order, BEFORE
        // any fill (the C1 layout contract — see the class-doc). Walk each
        // caller block in layout order: create that caller block FIRST
        // (it keeps its identity in `blockMap_` as a branch target), then
        // scan its instructions in order and, for each plan'd MULTI-BLOCK
        // call, pre-create that splice's cloned-callee blocks (in
        // callee-RPO) FOLLOWED BY its continuation block — stored per
        // callId in `splicePlans_`. This yields the topological layout
        // `[A', clones1, cont1, clones2, cont2, B', ...]`. Terminators can
        // still target forward references (loop back-edges) because every
        // block exists before phase 2 emits a single instruction. A
        // single-block-leaf call pre-creates NOTHING (it splices linearly
        // into its host block); only multi-block calls add blocks here.
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const oldB = src_.funcBlockAt(caller, bi);
            MirBlockId const newB = dst_.createBlock(src_.blockMarker(oldB));
            blockMap_.emplace(oldB.v, newB);
            blockExitMap_.emplace(oldB.v, newB);  // updated if split
            std::uint32_t const ni = src_.blockInstCount(oldB);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const id = src_.blockInstAt(oldB, ii);
                if (src_.instOpcode(id) != MirOpcode::Call) continue;
                auto const it = plan_.find(id.v);
                if (it == plan_.end()) continue;
                MirFuncId const callee = it->second;
                if (src_.funcBlockCount(callee) == 1) continue;  // linear splice
                precreateMultiBlockSplice(callee, id);
            }
        }

        // Phase 2: fill blocks.
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const oldB = src_.funcBlockAt(caller, bi);
            MirBlockId cur = blockMap_.at(oldB.v);
            dst_.beginBlock(cur);
            std::uint32_t const ni = src_.blockInstCount(oldB);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const id = src_.blockInstAt(oldB, ii);
                MirOpcode const op = src_.instOpcode(id);

                if (op == MirOpcode::Phi) {
                    // Caller's OWN phi — placeholder now, incomings in
                    // phase 3 (the caller's CFG is preserved, so its
                    // phis remain valid; only their `pred` may shift to
                    // a split block's exit, handled via blockExitMap_).
                    MirInstId const newPhi = dst_.addPhi(src_.instType(id));
                    rewrite_.emplace(id.v, newPhi);
                    deferredPhis_.push_back({id, newPhi, oldB});
                    continue;
                }

                if (opcodeInfo(op).isTerminator) {
                    emitTerminator(op, id);
                    blockExitMap_[oldB.v] = cur;  // this block carries the term
                    break;
                }

                // A selected inline Call?
                if (op == MirOpcode::Call) {
                    auto const it = plan_.find(id.v);
                    if (it != plan_.end()) {
                        MirFuncId const callee = it->second;
                        if (src_.funcBlockCount(callee) == 1) {
                            // Single-block leaf: splice linearly into cur.
                            MirInstId const result =
                                spliceSingleBlock(callee, id);
                            rewrite_.emplace(id.v, result);
                        } else {
                            // Multi-block: split here. `cur` advances to
                            // the continuation block; subsequent insts of
                            // this original block emit into it.
                            cur = spliceMultiBlock(callee, id, cur);
                        }
                        ++callsInlined_;
                        continue;
                    }
                }

                // Ordinary caller instruction (incl. a non-inlined Call):
                // verbatim copy with operands mapped through rewrite_.
                emitCallerInst(id, op);
            }
        }

        // Phase 3: flush caller phi incomings. A phi-incoming pred that
        // was a split block redirects to that block's EXIT (the block
        // that actually branches into the phi's block).
        for (auto const& dp : deferredPhis_) {
            for (MirPhiIncoming const& inc : src_.phiIncomings(dp.oldPhi)) {
                MirInstId const newVal = mapCallerValue(inc.value, dp.oldPhi);
                auto const exitIt = blockExitMap_.find(inc.pred.v);
                if (exitIt == blockExitMap_.end()) {
                    std::fprintf(stderr,
                        "dss::opt::passes::Inlining fatal: caller phi v=%u "
                        "incoming pred v=%u has no blockExitMap_ entry — "
                        "every reachable caller block is pre-created in "
                        "phase 1 (substrate-contract violation).\n",
                        dp.oldPhi.v, inc.pred.v);
                    std::abort();
                }
                dst_.addPhiIncoming(dp.newPhi,
                                    MirPhiIncoming{newVal, exitIt->second});
            }
        }
    }

private:
    struct DeferredPhi {
        MirInstId  oldPhi;
        MirInstId  newPhi;
        MirBlockId oldBlock;
    };

    // PHASE-1 pre-created blocks for ONE multi-block splice (the C1
    // layout contract). `calleeRpo` is the callee's block RPO (the fill
    // order, == def-before-use); `calleeBlockMap` maps each callee block
    // .v → its pre-created clone; `contBlock` is the pre-created
    // continuation. All created in final-layout order in phase 1 so the
    // function layout stays topological; phase-2 `spliceMultiBlock`
    // consumes these (it never calls createBlock).
    struct SplicePlan {
        std::vector<MirBlockId>                       calleeRpo;
        std::unordered_map<std::uint32_t, MirBlockId> calleeBlockMap;
        MirBlockId                                    contBlock{};
    };

    // Map an operand of a CALLER instruction (a caller-OLD value) to its
    // caller-NEW id via the function-wide rewrite map.
    [[nodiscard]] MirInstId
    mapCallerValue(MirInstId callerOld, MirInstId user) {
        auto const it = rewrite_.find(callerOld.v);
        if (it == rewrite_.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::Inlining fatal: caller inst v=%u "
                "operand v=%u has no rewrite entry during multi-block "
                "rebuild — RPO/scan-order violation "
                "(D-OPT2-REWRITE-MAP-COMPLETENESS).\n",
                user.v, callerOld.v);
            std::abort();
        }
        return it->second;
    }

    // Verbatim-copy one ordinary CALLER instruction into the open block.
    void emitCallerInst(MirInstId id, MirOpcode op) {
        if (op == MirOpcode::Const) {
            rewrite_.emplace(id.v, dst_.addConst(
                src_.literalValue(src_.constLiteralIndex(id)), src_.instType(id)));
            return;
        }
        if (op == MirOpcode::Arg) {
            rewrite_.emplace(id.v,
                dst_.addArg(src_.argIndex(id), src_.instType(id)));
            return;
        }
        if (op == MirOpcode::GlobalAddr) {
            rewrite_.emplace(id.v, dst_.addGlobalAddr(
                src_.globalAddrSymbol(id), src_.instType(id)));
            return;
        }
        auto const ops = src_.instOperands(id);
        std::vector<MirInstId> newOps;
        newOps.reserve(ops.size());
        for (MirInstId const o : ops) newOps.push_back(mapCallerValue(o, id));
        rewrite_.emplace(id.v, dst_.addInst(op, newOps, src_.instType(id),
                                            src_.instPayload(id),
                                            src_.instFlags(id)));
    }

    // Emit a CALLER terminator into the open block, mapping operands via
    // rewrite_ and successors via blockMap_ (each original block's entry).
    void emitTerminator(MirOpcode op, MirInstId id) {
        auto const oldOps  = src_.instOperands(id);
        auto const oldBlk  = src_.instBlock(id);
        auto const oldSucc = src_.blockSuccessors(oldBlk);
        auto mapSucc = [&](MirBlockId oldS) -> MirBlockId {
            auto const it = blockMap_.find(oldS.v);
            if (it == blockMap_.end()) {
                std::fprintf(stderr,
                    "dss::opt::passes::Inlining fatal: caller terminator "
                    "v=%u successor block v=%u not in blockMap_ — every "
                    "caller block is pre-created in phase 1.\n", id.v, oldS.v);
                std::abort();
            }
            return it->second;
        };
        switch (op) {
            case MirOpcode::Br:
                dst_.addBr(mapSucc(oldSucc[0]));
                return;
            case MirOpcode::CondBr:
                dst_.addCondBr(mapCallerValue(oldOps[0], id),
                               mapSucc(oldSucc[0]), mapSucc(oldSucc[1]));
                return;
            case MirOpcode::Switch: {
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                std::size_t const ncases = oldSucc.size() - 1;
                cases.reserve(ncases);
                for (std::size_t i = 0; i < ncases; ++i) {
                    cases.emplace_back(mapCallerValue(oldOps[1 + i], id),
                                       mapSucc(oldSucc[i]));
                }
                dst_.addSwitch(mapCallerValue(oldOps[0], id), cases,
                               mapSucc(oldSucc[ncases]));
                return;
            }
            case MirOpcode::Return: {
                // FC7 C1c: map EVERY return-piece operand (a multi-piece by-value
                // struct return carries N pieces; taking only oldOps[0] dropped the
                // rest — see mir_rebuild_helper's Return clone). 0/1/N all handled.
                std::vector<MirInstId> rvs;
                rvs.reserve(oldOps.size());
                for (MirInstId const o : oldOps) rvs.push_back(mapCallerValue(o, id));
                dst_.addReturnMulti(rvs);
                return;
            }
            case MirOpcode::Unreachable:
                dst_.addUnreachable();
                return;
            default:
                std::fprintf(stderr,
                    "dss::opt::passes::Inlining fatal: emitTerminator: "
                    "MirOpcode %d marked isTerminator but no clone arm.\n",
                    static_cast<int>(op));
                std::abort();
        }
    }

    // Splice a SINGLE-BLOCK leaf callee linearly into the open block
    // `cur` (same shape as the cycle-1 path, reusing the shared emit
    // helpers). Returns the threaded return value (invalid for void).
    [[nodiscard]] MirInstId
    spliceSingleBlock(MirFuncId callee, MirInstId oldCall) {
        std::vector<MirInstId> const actualArgs =
            mapActualArgs(src_, oldCall, rewrite_);
        MirBlockId const cb = src_.funcEntry(callee);
        std::uint32_t const ni = src_.blockInstCount(cb);
        std::unordered_map<std::uint32_t, MirInstId> local;
        local.reserve(ni);
        MirInstId result{};
        for (std::uint32_t i = 0; i < ni; ++i) {
            MirInstId const cid = src_.blockInstAt(cb, i);
            MirOpcode const cop = src_.instOpcode(cid);
            if (opcodeInfo(cop).isTerminator) {
                if (cop == MirOpcode::Return) {
                    auto const rops = src_.instOperands(cid);
                    if (!rops.empty()) {
                        result = mapCalleeOperand(src_, rops[0], local, callee);
                    }
                } else {
                    std::fprintf(stderr,
                        "dss::opt::passes::Inlining fatal: single-block "
                        "callee funcId v=%u terminates with non-Return "
                        "opcode %d during multi-block rebuild.\n",
                        callee.v, static_cast<int>(cop));
                    std::abort();
                }
                break;
            }
            emitCalleeInst(src_, cid, cop, dst_, actualArgs, local, callee,
                           malformed_);
        }
        return result;
    }

    // Pre-create (PHASE 1) the blocks of a multi-block splice in
    // FINAL-LAYOUT order: the cloned-callee blocks (callee-RPO) FOLLOWED
    // BY the continuation block, keyed by callId in `splicePlans_`. RPO
    // order is the same def-before-use order phase-2 fill uses, so a
    // clone's layout precedes every clone that consumes it; the
    // continuation comes LAST so a clone-defined value it consumes (the
    // 1-return elision) is laid out before it. Creates NO instructions —
    // only blocks. All blocks are `Linear` (markers re-derived post-
    // finish). Fail-loud on a duplicate callId (a Call id is unique).
    void precreateMultiBlockSplice(MirFuncId callee, MirInstId oldCall) {
        if (splicePlans_.count(oldCall.v) != 0) {
            std::fprintf(stderr,
                "dss::opt::passes::Inlining fatal: multi-block call v=%u "
                "pre-created twice in phase 1 — a Call instruction id must "
                "be unique within a function (substrate-contract "
                "violation).\n", oldCall.v);
            std::abort();
        }
        SplicePlan sp;
        MirBlockId const calleeEntry = src_.funcEntry(callee);
        sp.calleeRpo = mirReversePostOrder(src_, calleeEntry);
        sp.calleeBlockMap.reserve(sp.calleeRpo.size());
        for (MirBlockId const fb : sp.calleeRpo) {
            sp.calleeBlockMap.emplace(fb.v,
                                      dst_.createBlock(StructCfMarker::Linear));
        }
        // Continuation LAST → laid out after every clone.
        sp.contBlock = dst_.createBlock(StructCfMarker::Linear);
        splicePlans_.emplace(oldCall.v, std::move(sp));
    }

    // Splice a MULTI-BLOCK leaf callee using the PHASE-1 pre-created
    // blocks (the C1 layout contract — `spliceMultiBlock` never calls
    // createBlock). The open block `cur` is split: it is sealed with a Br
    // to the cloned callee entry; the callee CFG is filled into the pre-
    // created clones; every callee Return becomes a Br to the pre-created
    // continuation; a return-merge Phi in the continuation joins the per-
    // return values. Returns the continuation block (the new open block
    // for the remaining instructions of the host caller block).
    [[nodiscard]] MirBlockId
    spliceMultiBlock(MirFuncId callee, MirInstId oldCall, MirBlockId cur) {
        (void)cur;
        std::vector<MirInstId> const actualArgs =
            mapActualArgs(src_, oldCall, rewrite_);

        // Consume the PHASE-1 pre-created blocks. A plan'd multi-block
        // callId with no pre-created entry would mean phase 1 and phase 2
        // disagree on which calls are multi-block — a silent createBlock
        // fallback here would reintroduce the layout-inversion bug class,
        // so fail loud instead (C1 fail-loud requirement).
        auto const planIt = splicePlans_.find(oldCall.v);
        if (planIt == splicePlans_.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::Inlining fatal: multi-block call v=%u "
                "(callee funcId v=%u) has no phase-1 pre-created {clones, "
                "cont} entry — phase 1/phase 2 plan disagreement; a "
                "createBlock fallback would reintroduce the layout-"
                "inversion bug class (D-OPT2 layout contract).\n",
                oldCall.v, callee.v);
            std::abort();
        }
        SplicePlan const& sp = planIt->second;
        std::vector<MirBlockId> const& calleeRpo = sp.calleeRpo;
        std::unordered_map<std::uint32_t, MirBlockId> const& calleeBlockMap =
            sp.calleeBlockMap;
        MirBlockId const contBlock = sp.contBlock;
        MirBlockId const calleeEntry = src_.funcEntry(callee);

        // Seal `cur` with a Br to the cloned callee entry.
        auto const entryIt = calleeBlockMap.find(calleeEntry.v);
        if (entryIt == calleeBlockMap.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::Inlining fatal: callee funcId v=%u entry "
                "block v=%u missing from RPO clone map.\n",
                callee.v, calleeEntry.v);
            std::abort();
        }
        dst_.addBr(entryIt->second);

        // Clone each callee block. ONE function-wide local map (callee
        // values cross cloned blocks; RPO emission keeps def-before-use).
        std::unordered_map<std::uint32_t, MirInstId> local;
        // (value, cloned-pred-block) pairs collected from each rewritten
        // callee Return — the incomings of the return-merge Phi.
        std::vector<MirPhiIncoming> returnEdges;
        // DEFERRED callee phis: a placeholder Phi is emitted in the clone
        // loop (so later insts referencing it resolve through `local`); its
        // incomings are flushed AFTER the loop (so a back-edge incoming
        // VALUE, defined later in RPO, has its `local` entry). {oldPhi,
        // newPhi}. Sibling of `returnEdges` with DISJOINT maps — the two
        // compose without interference (D-OPT7-MULTIBLOCK-SPLICE-PHI).
        std::vector<std::pair<MirInstId, MirInstId>> deferredCalleePhis;
        for (MirBlockId const fb : calleeRpo) {
            MirBlockId const newFb = calleeBlockMap.at(fb.v);
            dst_.beginBlock(newFb);
            std::uint32_t const ni = src_.blockInstCount(fb);
            for (std::uint32_t i = 0; i < ni; ++i) {
                MirInstId const cid = src_.blockInstAt(fb, i);
                MirOpcode const cop = src_.instOpcode(cid);
                if (opcodeInfo(cop).isTerminator) {
                    emitCalleeTerminator(cop, cid, callee, calleeBlockMap,
                                         local, contBlock, newFb, returnEdges);
                    break;
                }
                if (cop == MirOpcode::Phi) {
                    // Placeholder now (incomings flushed after the loop, so
                    // a back-edge value resolves); record the mapping so
                    // later insts referencing this phi resolve via `local`.
                    MirInstId const newPhi = dst_.addPhi(src_.instType(cid));
                    local.emplace(cid.v, newPhi);
                    deferredCalleePhis.emplace_back(cid, newPhi);
                    continue;
                }
                emitCalleeInst(src_, cid, cop, dst_, actualArgs, local, callee,
                               malformed_);
            }
        }

        // Flush callee-phi incomings. `local` is complete after the clone
        // loop → a back-edge (loop-phi) incoming VALUE resolves;
        // `calleeBlockMap` is complete from phase 1 → every incoming PRED
        // resolves. NO exit-redirect (callee blocks are 1:1, never split —
        // unlike the caller's phase-3, which routes a split pred through
        // blockExitMap_). addPhiIncoming is keyed by phi id → needs no open
        // block (same as the caller phase-3). A `calleeBlockMap` miss is a
        // structural violation (every callee block is pre-created in phase
        // 1) → fail loud.
        for (auto const& [oldPhi, newPhi] : deferredCalleePhis) {
            for (MirPhiIncoming const& inc : src_.phiIncomings(oldPhi)) {
                MirInstId const newVal =
                    mapCalleeOperand(src_, inc.value, local, callee);
                auto const predIt = calleeBlockMap.find(inc.pred.v);
                if (predIt == calleeBlockMap.end()) {
                    std::fprintf(stderr,
                        "dss::opt::passes::Inlining fatal: cloned callee "
                        "funcId v=%u phi v=%u incoming pred block v=%u not "
                        "in clone map — every callee block is pre-created in "
                        "phase 1 (D-OPT7-MULTIBLOCK-SPLICE-PHI).\n",
                        callee.v, oldPhi.v, inc.pred.v);
                    std::abort();
                }
                dst_.addPhiIncoming(newPhi,
                                    MirPhiIncoming{newVal, predIt->second});
            }
        }

        // Open the continuation + build the return-merge value.
        dst_.beginBlock(contBlock);
        MirInstId result{};
        if (!returnEdges.empty()) {
            if (returnEdges.size() == 1) {
                // Single return path → elide the degenerate 1-incoming
                // Phi to the value directly (verifier-legal either way;
                // this keeps the merge minimal).
                result = returnEdges[0].value;
            } else {
                MirInstId const phi = dst_.addPhi(src_.instType(oldCall));
                for (MirPhiIncoming const& e : returnEdges) {
                    dst_.addPhiIncoming(phi, e);
                }
                result = phi;
            }
        }
        // For a value-returning callee, `result` is the Call's value;
        // record it so caller operands referencing the Call resolve. A
        // void callee leaves `result` invalid — a void Call's result is
        // never used as an operand, so no rewrite entry is needed (and
        // none must be emitted: an invalid id in rewrite_ would fail a
        // later mapCallerValue lookup).
        if (result.valid()) rewrite_.emplace(oldCall.v, result);
        return contBlock;
    }

    // Emit a cloned-CALLEE terminator. A `Return` becomes a `Br` to the
    // continuation (recording the (value, this-cloned-block) incoming for
    // the merge Phi); Br/CondBr/Switch re-emit with targets remapped
    // through calleeBlockMap; Unreachable re-emits verbatim.
    void emitCalleeTerminator(
        MirOpcode cop, MirInstId cid, MirFuncId callee,
        std::unordered_map<std::uint32_t, MirBlockId> const& calleeBlockMap,
        std::unordered_map<std::uint32_t, MirInstId> const& local,
        MirBlockId contBlock, MirBlockId clonedPred,
        std::vector<MirPhiIncoming>& returnEdges) {
        auto const cops    = src_.instOperands(cid);
        auto const cBlk    = src_.instBlock(cid);
        auto const cSucc   = src_.blockSuccessors(cBlk);
        auto mapCalleeSucc = [&](MirBlockId fs) -> MirBlockId {
            auto const it = calleeBlockMap.find(fs.v);
            if (it == calleeBlockMap.end()) {
                std::fprintf(stderr,
                    "dss::opt::passes::Inlining fatal: cloned callee "
                    "funcId v=%u terminator v=%u successor block v=%u not "
                    "in clone map — every callee block is pre-created.\n",
                    callee.v, cid.v, fs.v);
                std::abort();
            }
            return it->second;
        };
        switch (cop) {
            case MirOpcode::Return: {
                // FC7 C1c: a MULTI-PIECE by-value struct Return (N>1 piece operands)
                // cannot be merged through a single continuation Phi — inlining a
                // by-value-struct-returning callee is not supported (OPT7-gated;
                // D-FC7-INLINE-MULTI-PIECE-RETURN). Fail loud rather than silently
                // drop pieces 1..N-1.
                if (cops.size() > 1) {
                    std::fprintf(stderr,
                        "dss::inlining fatal: callee %u has a multi-piece struct "
                        "Return (%zu pieces); inlining by-value-struct-returning "
                        "functions is unsupported (D-FC7-INLINE-MULTI-PIECE-RETURN).\n",
                        callee.v, static_cast<std::size_t>(cops.size()));
                    std::abort();
                }
                MirInstId rv{};
                if (!cops.empty()) {
                    rv = mapCalleeOperand(src_, cops[0], local, callee);
                }
                dst_.addBr(contBlock);
                // A void callee's Return contributes no merge value (rv
                // invalid). For a value-returning callee, rv is the value
                // flowing into the merge Phi along this cloned block.
                if (rv.valid()) returnEdges.push_back({rv, clonedPred});
                return;
            }
            case MirOpcode::Br:
                dst_.addBr(mapCalleeSucc(cSucc[0]));
                return;
            case MirOpcode::CondBr:
                dst_.addCondBr(mapCalleeOperand(src_, cops[0], local, callee),
                               mapCalleeSucc(cSucc[0]), mapCalleeSucc(cSucc[1]));
                return;
            case MirOpcode::Switch: {
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                std::size_t const ncases = cSucc.size() - 1;
                cases.reserve(ncases);
                for (std::size_t i = 0; i < ncases; ++i) {
                    cases.emplace_back(
                        mapCalleeOperand(src_, cops[1 + i], local, callee),
                        mapCalleeSucc(cSucc[i]));
                }
                dst_.addSwitch(mapCalleeOperand(src_, cops[0], local, callee),
                               cases, mapCalleeSucc(cSucc[ncases]));
                return;
            }
            case MirOpcode::Unreachable:
                dst_.addUnreachable();
                return;
            default:
                std::fprintf(stderr,
                    "dss::opt::passes::Inlining fatal: cloned callee "
                    "funcId v=%u terminator opcode %d has no clone arm.\n",
                    callee.v, static_cast<int>(cop));
                std::abort();
        }
    }

    Mir const&            src_;
    MirBuilder&           dst_;
    std::unordered_map<std::uint32_t, MirFuncId> const& plan_;
    // OLD caller block .v → NEW caller block (its entry / branch target).
    std::unordered_map<std::uint32_t, MirBlockId> blockMap_;
    // OLD caller block .v → NEW block carrying its terminator (the last
    // continuation in its split chain; == blockMap_ entry if not split).
    std::unordered_map<std::uint32_t, MirBlockId> blockExitMap_;
    // Function-wide caller-OLD inst .v → caller-NEW id.
    std::unordered_map<std::uint32_t, MirInstId>  rewrite_;
    std::vector<DeferredPhi> deferredPhis_;
    // PHASE-1 pre-created splice blocks, keyed by the multi-block Call's
    // OLD inst .v. Populated in phase 1 (final-layout order), consumed in
    // phase 2 by `spliceMultiBlock`. Cleared per function.
    std::unordered_map<std::uint32_t, SplicePlan> splicePlans_;
    std::size_t callsInlined_ = 0;
    bool        malformed_    = false;
};

// True iff `caller`'s inline plan contains at least one MULTI-BLOCK
// callee — the signal to route this function through the multi-block
// rebuild rather than the cycle-1 `tryRewrite` path.
[[nodiscard]] bool
planHasMultiBlock(Mir const& src, ModuleAnalysis const& analysis,
                  MirFuncId caller,
                  std::unordered_map<std::uint32_t, MirFuncId>& planOut,
                  std::uint32_t inlineThreshold) {
    planOut.clear();
    bool anyMulti = false;
    std::uint32_t const nb = src.funcBlockCount(caller);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = src.funcBlockAt(caller, bi);
        std::uint32_t const ni = src.blockInstCount(b);
        for (std::uint32_t ii = 0; ii < ni; ++ii) {
            MirInstId const id = src.blockInstAt(b, ii);
            if (src.instOpcode(id) != MirOpcode::Call) continue;
            auto const callee =
                inlineLegalityGate(src, analysis, caller, id, inlineThreshold);
            if (!callee.has_value()) continue;
            planOut.emplace(id.v, *callee);
            if (src.funcBlockCount(*callee) != 1) anyMulti = true;
        }
    }
    return anyMulti;
}

} // namespace

InliningResult runInlining(Mir& mir, TypeInterner const& /*interner*/,
                           DiagnosticReporter& reporter,
                           std::uint32_t inlineThreshold) {
    InliningResult result{};
    MirBuilder builder;

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, "Inlining")
        == GlobalClonePrelude::CarvedOut) {
        result.ok = true;
        return result;
    }

    ModuleAnalysis const analysis = analyzeModule(mir);
    InliningPolicy policy{mir, analysis, inlineThreshold};

    std::size_t callsInlined = 0;
    bool        malformed    = false;

    // Per-function routing: a function whose inline plan is empty or
    // contains ONLY single-block-leaf targets goes through the cycle-1
    // `MirFunctionRebuilder` + `tryRewrite` path (UNCHANGED — preserving
    // the cycle-1 behavior + the Weak-inline pin byte-for-byte). A
    // function with ANY multi-block target is rebuilt by
    // `MultiBlockInliner` (which subsumes the single-block linear case
    // for any single-block targets in the same function).
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::unordered_map<std::uint32_t, MirFuncId> plan;
        if (planHasMultiBlock(mir, analysis, f, plan, inlineThreshold)) {
            MultiBlockInliner mb{mir, builder, plan};
            mb.rebuildFunction(f);
            callsInlined += mb.callsInlined();
            malformed = malformed || mb.malformed();
        } else {
            policy.analyze(f);
            MirFunctionRebuilder rb{mir, builder, policy};
            rb.rebuildFunction(f);
        }
    }
    callsInlined += policy.callsInlined();
    malformed = malformed || policy.malformed();

    if (malformed) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::X_InlineMalformedCallSite;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = "opt::Inlining: a call site selected for inlining had "
                     "an argument count that did not match the callee's "
                     "parameter count — structural MIR violation; refusing "
                     "to splice a wrong-arity body "
                     "(D-OPT7-INLINE-LEGALITY-GATE).";
        reporter.report(std::move(d));
        // Do NOT install the partially-rebuilt module — return without
        // moving `builder` into `mir`. ok=false signals the engine.
        result.ok = false;
        return result;
    }

    result.callsInlined = callsInlined;
    mir = std::move(builder).finish();
    // Canonical-marker stamping (D-OPT4-1): the splice changed the
    // caller's CFG (split blocks, cloned callee bodies). Markers are
    // re-derived from the NEW shape — inlined loop headers re-emerge
    // as LoopHeader; continuation blocks take their actual role.
    rederiveStructCfMarkers(mir);
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
