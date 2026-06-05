#include "opt/passes/inlining.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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
                   MirFuncId caller, MirInstId callId) {
    // Rule 1: direct call to a defined callee in this module.
    auto const calleeOpt = resolveDirectCallee(mir, a, callId);
    if (!calleeOpt.has_value()) return std::nullopt;
    MirFuncId const callee = *calleeOpt;

    // Rule 2: THE correctness rule — never inline a Weak callee. A
    // strong definition of the same name may replace it at link.
    if (mir.funcBinding(callee) == SymbolBinding::Weak) return std::nullopt;

    // Rule 3: never inline a self-recursive call (callee == caller).
    if (mir.funcSymbol(callee).v == mir.funcSymbol(caller).v) {
        return std::nullopt;
    }

    // Rule 4: refuse if the callee's address escapes anywhere in the
    // module (a function pointer could call it indirectly).
    if (a.addressEscaped.count(mir.funcSymbol(callee).v)) return std::nullopt;

    // Rule 5: single-block LEAF — exactly one block, no Call and no Phi
    // in that block (the minimal linear-splice scope). Plus an arity
    // safety gate: every `Arg(i)` the callee references must be in range
    // of the actual arguments the call passes — otherwise the
    // Arg(i)→actual substitution would be out-of-bounds (a structural
    // call/signature mismatch). A callee that references FEWER args than
    // it declares (an ignored parameter, or one a prior pass dropped) is
    // still inlinable; only an out-of-range index disqualifies.
    if (mir.funcBlockCount(callee) != 1) return std::nullopt;
    MirBlockId const cb = mir.funcEntry(callee);
    std::uint32_t const ni = mir.blockInstCount(cb);
    // The call passes `callArgCount` actual args (operands[1..]).
    auto const callOps = mir.instOperands(callId);
    std::size_t const callArgCount = callOps.empty() ? 0 : callOps.size() - 1;
    for (std::uint32_t i = 0; i < ni; ++i) {
        MirInstId const cid = mir.blockInstAt(cb, i);
        MirOpcode const op  = mir.instOpcode(cid);
        // "Leaf" = NO call-like op of ANY kind. `Call` (direct/indirect)
        // AND `IntrinsicCall` (a distinct side-effecting call-like opcode)
        // both disqualify. Inlining an IntrinsicCall-bearing body would be
        // SSA-correct (the intrinsic id is module-stable + correctly
        // remapped), but the minimal correctness-first OPT7 cycle keeps
        // "leaf" strict; relaxing it is deferred (D-OPT7-INLINE-LEGALITY-GATE).
        if (op == MirOpcode::Call) return std::nullopt;          // not a leaf
        if (op == MirOpcode::IntrinsicCall) return std::nullopt;  // not a leaf
        if (op == MirOpcode::Phi)  return std::nullopt;  // malformed single-block phi
        if (op == MirOpcode::Arg && mir.argIndex(cid) >= callArgCount) {
            return std::nullopt;  // arg index out of range → refuse
        }
    }
    return callee;
}

// The inlining rebuild policy. Per-function `analyze` decides which
// Call ids in THIS function are inline targets (and their resolved
// callee); the rebuilder's `tryRewrite` hook performs the linear
// single-block splice when it reaches one of those Calls.
class InliningPolicy final : public MirRebuildPolicy {
public:
    InliningPolicy(Mir const& src, ModuleAnalysis const& analysis) noexcept
        : src_(src), analysis_(analysis) {}

    [[nodiscard]] std::size_t callsInlined() const noexcept {
        return callsInlined_;
    }
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

    // Compute the per-function inline plan: callId → callee. Called
    // once before each function's rebuild.
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
                    inlineLegalityGate(src_, analysis_, caller, id);
                if (callee.has_value()) plan_.emplace(id.v, *callee);
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
        // Map the Call's actual arguments (caller-NEW values). The Call
        // operands are [calleeGlobalAddr, arg0, arg1, ...]; the callee
        // GlobalAddr (operand[0]) is intentionally dropped — inlining
        // removes the indirect-through-address call entirely.
        auto const callOps = src_.instOperands(oldCall);
        std::vector<MirInstId> actualArgs;
        actualArgs.reserve(callOps.size() > 0 ? callOps.size() - 1 : 0);
        for (std::size_t i = 1; i < callOps.size(); ++i) {
            actualArgs.push_back(mapCallerOperand(callOps[i], rewrite, oldCall));
        }

        // The legality gate (Rule 5) already validated that every
        // `Arg(i)` the callee references satisfies `i < actualArgs.size()`,
        // so the Arg(i)→actual substitution below is always in range.
        // The per-Arg bounds check inside the loop is a defensive
        // fail-loud guard: if a future gate change regressed and admitted
        // an out-of-range call, the pass emits X_InlineMalformedCallSite
        // and returns ok=false rather than producing a wrong-arity splice
        // (D-OPT7-INLINE-LEGALITY-GATE).
        MirBlockId const cb = src_.funcEntry(callee);
        std::uint32_t const ni = src_.blockInstCount(cb);

        // Walk the callee block: copy each NON-Arg / NON-terminator
        // instruction into the caller via a LOCAL calleeOld→callerNew
        // map. The terminator (a Return) is handled after the loop.
        std::unordered_map<std::uint32_t, MirInstId> local;
        local.reserve(ni);
        MirInstId result{};  // invalid until a Return value is mapped
        for (std::uint32_t i = 0; i < ni; ++i) {
            MirInstId const cid = src_.blockInstAt(cb, i);
            MirOpcode const cop = src_.instOpcode(cid);

            if (cop == MirOpcode::Arg) {
                // Map the parameter to the corresponding actual arg. The
                // gate (Rule 5) guarantees `idx < actualArgs.size()`; the
                // bounds check is the defensive fail-loud guard. If it
                // ever trips, mark the splice malformed and substitute
                // arg 0 (or, with zero args, leave it unmapped — the
                // top-level emits X_InlineMalformedCallSite and discards
                // the module, so the placeholder is never observed).
                std::uint32_t const idx = src_.argIndex(cid);
                if (idx >= actualArgs.size()) {
                    malformed_ = true;
                    if (!actualArgs.empty()) {
                        local.emplace(cid.v, actualArgs[0]);
                    }
                    continue;
                }
                local.emplace(cid.v, actualArgs[idx]);
                continue;
            }
            if (opcodeInfo(cop).isTerminator) {
                if (cop == MirOpcode::Return) {
                    auto const rops = src_.instOperands(cid);
                    if (!rops.empty()) {
                        result = mapCalleeOperand(rops[0], local, callee);
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

            // Ordinary callee instruction: re-emit into the caller.
            MirInstId const newId = emitCalleeInst(cid, cop, dst, local, callee);
            local.emplace(cid.v, newId);
        }

        ++callsInlined_;
        return result;
    }

    // Re-emit one ordinary (non-Arg, non-terminator) callee instruction
    // into the caller's open block. Leaves use their dedicated builders;
    // other ops use addInst with operands mapped through the local map.
    [[nodiscard]] MirInstId
    emitCalleeInst(MirInstId cid, MirOpcode cop, MirBuilder& dst,
                   std::unordered_map<std::uint32_t, MirInstId> const& local,
                   MirFuncId callee) {
        if (cop == MirOpcode::Const) {
            return dst.addConst(
                src_.literalValue(src_.constLiteralIndex(cid)),
                src_.instType(cid));
        }
        if (cop == MirOpcode::GlobalAddr) {
            return dst.addGlobalAddr(src_.globalAddrSymbol(cid),
                                     src_.instType(cid));
        }
        if (cop == MirOpcode::Phi) {
            // The gate excludes single-block callees containing a Phi.
            std::fprintf(stderr,
                "dss::opt::passes::Inlining fatal: callee funcId v=%u "
                "contains a Phi during splice — the legality gate must "
                "have excluded it.\n", callee.v);
            std::abort();
        }
        auto const cops = src_.instOperands(cid);
        std::vector<MirInstId> newOps;
        newOps.reserve(cops.size());
        for (MirInstId const o : cops) {
            newOps.push_back(mapCalleeOperand(o, local, callee));
        }
        return dst.addInst(cop, newOps, src_.instType(cid),
                           src_.instPayload(cid), src_.instFlags(cid));
    }

    // Map a callee-block operand to its caller-NEW value: an Arg or any
    // prior callee inst is in the LOCAL map (populated in def order).
    [[nodiscard]] MirInstId
    mapCalleeOperand(MirInstId calleeOp,
                     std::unordered_map<std::uint32_t, MirInstId> const& local,
                     MirFuncId callee) {
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

    Mir const&            src_;
    ModuleAnalysis const& analysis_;
    // Per-function inline plan: caller Call old-id .v → callee MirFuncId.
    std::unordered_map<std::uint32_t, MirFuncId> plan_;
    std::size_t callsInlined_ = 0;
    bool        malformed_    = false;
};

} // namespace

InliningResult runInlining(Mir& mir, TypeInterner const& /*interner*/,
                           DiagnosticReporter& reporter) {
    InliningResult result{};
    MirBuilder builder;

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, "Inlining")
        == GlobalClonePrelude::CarvedOut) {
        result.ok = true;
        return result;
    }

    ModuleAnalysis const analysis = analyzeModule(mir);
    InliningPolicy policy{mir, analysis};

    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        policy.analyze(f);
        MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(f);
    }

    if (policy.malformed()) {
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

    result.callsInlined = policy.callsInlined();
    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
