#include "mir/summary/summary_index.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/cross_cu_resolve.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <utility>

namespace dss::mirsum {

namespace {

// A (module, function) coordinate, packed so it can key a flat map.
struct FuncSite {
    std::uint32_t module = 0;
    std::uint32_t func   = 0;
};

// `resolveCrossCuDefs` keys on `LinkedSymbolKey{CompilationUnitId, SymbolId}`.
// The index has no CompilationUnitId, so it uses the SYNTHETIC `moduleIdx + 1`
// — byte-for-byte the convention `mergeCuMirs` uses, which is what lets the
// two agree on a winner. `+1` because CompilationUnitId 0 is the null id.
[[nodiscard]] std::uint32_t cuIdOf(std::uint32_t moduleIdx) {
    return moduleIdx + 1;
}
[[nodiscard]] std::uint32_t moduleIdxOf(LinkedSymbolKey const& k) {
    return k.cuId.v - 1;
}

} // namespace

bool isInlineCandidate(SummaryFunction const& f,
                       SummaryIndexPolicy const& policy,
                       std::unordered_set<std::string> const& escaped) {
    // An unnamed function cannot be named across a TU boundary at all.
    if (f.name.empty()) return false;

    // Gate rule 2 — THE correctness rule. A Weak definition may be replaced at
    // link time by a strong one of the same name, so its body is not the body
    // that will run.
    if (f.binding == SymbolBinding::Weak) return false;

    // Gate rules 2b / 2c — source-declared directives, obeyed unconditionally.
    // `noOptimize` is refused in BOTH directions by the gate; here we can only
    // see the CALLEE half, and the caller half is the importer's own business.
    if (f.noInline) return false;
    if (f.noOptimize) return false;

    // Gate rule 5 — the shape refusals. Each is a construct that binds to the
    // callee's own frame, or whose ids would collide when duplicated.
    if (f.hasComputedGoto) return false;
    if (f.hasSeh) return false;
    if (f.frameBound) return false;
    // No returning path → the splice's continuation block would have no
    // predecessor → the MirVerifier rejects the module → a valid program
    // becomes a build error.
    if (!f.hasReturn) return false;

    // Gate rule 4 — the WHOLE-PROGRAM address-escape refusal. `escaped` MUST
    // be the union across every TU (`SummaryIndex::escapedSymbols`); the
    // per-TU set would be less conservative than today's merged module.
    if (escaped.count(f.name) != 0) return false;

    // Gate rule 6 — the cost model, with the SAME `>` comparison the gate
    // uses (a callee of EXACTLY the threshold still inlines) and the SAME
    // `always_inline` waiver, which overrides profitability and nothing else.
    if (f.instCount > policy.inlineThreshold && !f.alwaysInline) return false;

    // ⚠ NOT SCREENED HERE, DELIBERATELY:
    //   * gate rule 3 (recursion) is a property of the caller/callee PAIR, not
    //     of the callee — the caller applies it via `SummaryIndex::sccOf`.
    //   * the arity / type check is per-CALL-SITE and stays at the gate.
    //   * `isInlineDefinition` (C99 6.7.4p7) does NOT disqualify: an inline
    //     definition is exactly the thing one wants to inline. It matters for
    //     who OWNS the out-of-line symbol, which is the resolver's business.
    return true;
}

std::optional<SummaryIndex>
buildSummaryIndex(std::span<ModuleSummary const> summaries,
                  SummaryIndexPolicy const&      policy,
                  DiagnosticReporter&            reporter) {
    SummaryIndex out;
    out.deadSymbolsPerModule.resize(summaries.size());
    out.tier2KeyInputs.resize(summaries.size());

    // ── (0) self-identity. A summary carries its target so it can never be
    // mixed across machines; disagreement is a structural failure, not a
    // warning, because the bodies behind these summaries were compiled for
    // different machines and importing one into the other is a miscompile.
    for (std::uint32_t m = 1; m < summaries.size(); ++m) {
        if (summaries[m].targetIdentity == summaries[0].targetIdentity) continue;
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "buildSummaryIndex: module #{} was summarized for target '{}' but "
            "module #0 for '{}' — summaries from different targets cannot be "
            "indexed together.",
            m, summaries[m].targetIdentity, summaries[0].targetIdentity);
        reporter.report(std::move(d));
        return std::nullopt;
    }

    // ── (1) cross-TU resolution, delegated to the linker's policy. Only
    // externally-visible definitions participate; a Local definition is
    // module-private (C 6.2.2p3) and is neither a winner nor a loser even when
    // it shares a name with an externally-visible symbol elsewhere.
    std::vector<linker::CrossCuDef> defs;
    for (std::uint32_t m = 0; m < summaries.size(); ++m) {
        for (SummaryFunction const& f : summaries[m].functions) {
            if (f.name.empty() || f.binding == SymbolBinding::Local) continue;
            defs.push_back(linker::CrossCuDef{
                f.name, f.binding,
                LinkedSymbolKey{CompilationUnitId{cuIdOf(m)}, SymbolId{f.symbol}}});
        }
        for (SummaryGlobal const& g : summaries[m].globals) {
            if (g.name.empty() || g.binding == SymbolBinding::Local) continue;
            defs.push_back(linker::CrossCuDef{
                g.name, g.binding,
                LinkedSymbolKey{CompilationUnitId{cuIdOf(m)}, SymbolId{g.symbol}}});
        }
    }
    linker::CrossCuResolution const resolution = linker::resolveCrossCuDefs(defs);

    for (linker::CrossCuConflict const& c : resolution.conflicts) {
        out.conflictingNames.push_back(c.name);
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_SymbolRedefinedAcrossUnits;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "buildSummaryIndex: symbol '{}' has multiple strong (global) "
            "definitions across compilation units (CU #{} and CU #{}).",
            c.name, c.existing.cuId.v, c.incoming.cuId.v);
        reporter.report(std::move(d));
    }
    std::sort(out.conflictingNames.begin(), out.conflictingNames.end());
    out.conflictingNames.erase(
        std::unique(out.conflictingNames.begin(), out.conflictingNames.end()),
        out.conflictingNames.end());

    // Turn each winning key back into a (module, index) site. The scan is over
    // the summaries in module order, so a name whose winner cannot be located
    // simply gets no entry — it is then not importable, which is the
    // conservative outcome.
    for (std::uint32_t m = 0; m < summaries.size(); ++m) {
        for (std::uint32_t i = 0;
             i < static_cast<std::uint32_t>(summaries[m].functions.size()); ++i) {
            SummaryFunction const& f = summaries[m].functions[i];
            if (f.name.empty() || f.binding == SymbolBinding::Local) continue;
            auto const it = resolution.winners.find(f.name);
            if (it == resolution.winners.end()) continue;
            if (moduleIdxOf(it->second) != m || it->second.symbol.v != f.symbol)
                continue;
            out.winners[f.name] = DefiningSite{m, i, /*isFunction=*/true};
        }
        for (std::uint32_t i = 0;
             i < static_cast<std::uint32_t>(summaries[m].globals.size()); ++i) {
            SummaryGlobal const& g = summaries[m].globals[i];
            if (g.name.empty() || g.binding == SymbolBinding::Local) continue;
            auto const it = resolution.winners.find(g.name);
            if (it == resolution.winners.end()) continue;
            if (moduleIdxOf(it->second) != m || it->second.symbol.v != g.symbol)
                continue;
            out.winners[g.name] = DefiningSite{m, i, /*isFunction=*/false};
        }
    }

    // ── (2) THE WHOLE-PROGRAM ESCAPE SET. A plain union: a symbol whose
    // address escapes in ANY TU escapes for the program.
    for (ModuleSummary const& s : summaries) {
        for (std::string const& n : s.escapedSymbolNames) out.escapedSymbols.insert(n);
    }

    // ── (3) THE WHOLE-PROGRAM CALL-GRAPH SCC, over NAMES.
    //
    // Keyed on names rather than on (module, function) because that is the
    // granularity at which cross-TU recursion is visible: `f` in A calling `g`
    // in B calling `f` is one cycle only if the two `f`s are the same NAME. An
    // iterative Tarjan (the same algorithm `opt/analysis/call_graph_scc.cpp`
    // runs over one module) — explicit stack, so a deep call graph cannot
    // overflow the C++ stack.
    //
    // ⚠ DETERMINISM. Tarjan's output depends on the order roots are visited
    // and the order each node's successors are walked, so BOTH are fixed here:
    // roots in module-then-declaration order, successors in the summary's
    // recorded call order. No hash map is iterated to make a choice.
    {
        // Adjacency by name. A name that is defined nowhere (a library import)
        // gets no node — it cannot participate in a cycle we can see.
        std::unordered_map<std::string, std::uint32_t> nodeOf;
        std::vector<std::string>                       nodeName;
        auto nodeFor = [&](std::string const& n) -> std::uint32_t {
            auto const it = nodeOf.find(n);
            if (it != nodeOf.end()) return it->second;
            std::uint32_t const id =
                static_cast<std::uint32_t>(nodeName.size());
            nodeOf.emplace(n, id);
            nodeName.push_back(n);
            return id;
        };
        // Create nodes in module-then-declaration order so node ids — and
        // therefore the SCC ids below — are a pure function of the input
        // sequence.
        for (ModuleSummary const& s : summaries) {
            for (SummaryFunction const& f : s.functions) {
                if (!f.name.empty()) (void)nodeFor(f.name);
            }
        }
        std::vector<std::vector<std::uint32_t>> adj(nodeName.size());
        for (ModuleSummary const& s : summaries) {
            for (SummaryFunction const& f : s.functions) {
                if (f.name.empty()) continue;
                std::uint32_t const from = nodeFor(f.name);
                for (SummaryCallSite const& c : f.calls) {
                    if (!c.direct || c.calleeName.empty()) continue;
                    auto const it = nodeOf.find(c.calleeName);
                    if (it == nodeOf.end()) continue;  // undefined — no node
                    adj[from].push_back(it->second);
                }
            }
        }

        std::size_t const n = nodeName.size();
        std::vector<std::uint32_t> index(n, UINT32_MAX), low(n, 0);
        std::vector<std::uint8_t>  onStack(n, 0);
        std::vector<std::uint32_t> stack;
        std::vector<std::uint32_t> sccId(n, UINT32_MAX);
        std::uint32_t nextIndex = 0, nextScc = 0;
        // Explicit DFS frame: the node plus how many of its successors have
        // been dispatched.
        struct Frame { std::uint32_t v; std::size_t next; };
        std::vector<Frame> work;

        for (std::uint32_t root = 0; root < n; ++root) {
            if (index[root] != UINT32_MAX) continue;
            work.push_back(Frame{root, 0});
            index[root] = low[root] = nextIndex++;
            stack.push_back(root);
            onStack[root] = 1;
            while (!work.empty()) {
                Frame& fr = work.back();
                if (fr.next < adj[fr.v].size()) {
                    std::uint32_t const w = adj[fr.v][fr.next++];
                    if (index[w] == UINT32_MAX) {
                        index[w] = low[w] = nextIndex++;
                        stack.push_back(w);
                        onStack[w] = 1;
                        work.push_back(Frame{w, 0});
                    } else if (onStack[w]) {
                        low[fr.v] = std::min(low[fr.v], index[w]);
                    }
                    continue;
                }
                std::uint32_t const v = fr.v;
                work.pop_back();
                if (!work.empty()) {
                    low[work.back().v] = std::min(low[work.back().v], low[v]);
                }
                if (low[v] == index[v]) {
                    for (;;) {
                        std::uint32_t const w = stack.back();
                        stack.pop_back();
                        onStack[w] = 0;
                        sccId[w]   = nextScc;
                        if (w == v) break;
                    }
                    ++nextScc;
                }
            }
        }
        for (std::uint32_t i = 0; i < n; ++i) out.sccOf[nodeName[i]] = sccId[i];
    }

    // ── (4) THE WHOLE-PROGRAM LIVENESS BFS — the `scanLiveSymbols` lift.
    //
    // ★★★ NO PER-TU PASS CAN COMPUTE THIS. `scanLiveSymbols` is an
    // inter-procedural BFS seeded with externally-visible roots, expanded
    // through live `GlobalAddr` references, with a FIXPOINT over global
    // initializer literals (a function reachable only through a data
    // relocation in another global's initializer). Every one of those three
    // phases crosses TU boundaries in a real program; a TU that ran the BFS
    // alone would see its own imports as roots and prove nothing dead.
    {
        // name → the UNION of every edge list any definition of that name
        // contributes.
        //
        // ⚠ THE UNION IS LOAD-BEARING, not tidiness. A name can be defined in
        // several TUs — a C99 6.7.4p7 inline definition beside its external
        // one, or a weak/strong pair — and picking any ONE of their edge lists
        // would under-approximate liveness, which is the direction that
        // silently DELETES a live symbol. Over-approximating merely keeps a
        // dead one, which the per-TU DCE gets another chance at. The
        // multiply-defined set is tiny, so the union costs nothing.
        std::unordered_map<std::string, std::vector<std::string>> unionRefs;
        for (ModuleSummary const& s : summaries) {
            for (SummaryFunction const& f : s.functions) {
                if (f.name.empty()) continue;
                auto& v = unionRefs[f.name];
                v.insert(v.end(), f.symbolRefs.begin(), f.symbolRefs.end());
            }
            for (SummaryGlobal const& g : s.globals) {
                if (g.name.empty()) continue;
                auto& v = unionRefs[g.name];
                v.insert(v.end(), g.initSymbolRefs.begin(), g.initSymbolRefs.end());
            }
        }

        std::deque<std::string> worklist;
        auto markLive = [&](std::string const& n) {
            if (n.empty()) return;
            if (out.liveSymbols.insert(n).second) worklist.push_back(n);
        };
        // Phase 1 — seed with externally-visible roots, in module order.
        for (ModuleSummary const& s : summaries) {
            for (SummaryFunction const& f : s.functions) {
                if (!f.name.empty()
                    && isExternallyVisible(f.binding, f.visibility))
                    markLive(f.name);
            }
            for (SummaryGlobal const& g : s.globals) {
                if (!g.name.empty()
                    && isExternallyVisible(g.binding, g.visibility))
                    markLive(g.name);
            }
        }
        // Phases 2+3 — one worklist covers both. `unionRefs` already merges a
        // function's `GlobalAddr` references with a global's initializer
        // symbol-address targets, so the "fixpoint over global initializers"
        // is not a separate loop here: reaching a live global enqueues its
        // initializer targets exactly as reaching a live function enqueues its
        // references, and the worklist runs to exhaustion either way.
        while (!worklist.empty()) {
            std::string const cur = std::move(worklist.front());
            worklist.pop_front();
            auto const it = unionRefs.find(cur);
            if (it == unionRefs.end()) continue;
            for (std::string const& ref : it->second) markLive(ref);
        }
    }

    // Per-module dead lists — a NAMED symbol this module defines that the BFS
    // never reached.
    for (std::uint32_t m = 0; m < summaries.size(); ++m) {
        std::vector<std::string>& dead = out.deadSymbolsPerModule[m];
        for (SummaryFunction const& f : summaries[m].functions) {
            if (!f.name.empty() && out.liveSymbols.count(f.name) == 0)
                dead.push_back(f.name);
        }
        for (SummaryGlobal const& g : summaries[m].globals) {
            if (!g.name.empty() && out.liveSymbols.count(g.name) == 0)
                dead.push_back(g.name);
        }
        std::sort(dead.begin(), dead.end());
        dead.erase(std::unique(dead.begin(), dead.end()), dead.end());
    }

    // ── (5) THE PREFETCH PLAN.
    //
    // Per importer module, a breadth-first walk of the call graph from that
    // module's own call sites, bounded by `maxImportDepth` and (optionally) by
    // `perModuleImportInstBudget`. Every edge must first pass
    // `isInlineCandidate`, so the frontier is "N levels of functions ALREADY
    // SMALL ENOUGH TO INLINE" — which is what stops it degenerating into the
    // whole program.
    //
    // ⚠ DETERMINISM UNDER A BUDGET. When a budget is set, WHICH imports get
    // charged first decides which ones fit — so the frontier at each depth is
    // sorted by name before it is charged. Without that the outcome would
    // depend on the order a worklist happened to pop, and two runs could
    // produce different objects.
    for (std::uint32_t m = 0; m < summaries.size(); ++m) {
        std::unordered_set<std::string> taken;
        std::uint32_t                   spent = 0;
        // Seed: every direct callee named by any call site in this module.
        std::vector<std::string> frontier;
        for (SummaryFunction const& f : summaries[m].functions) {
            for (SummaryCallSite const& c : f.calls) {
                if (c.direct && !c.calleeName.empty())
                    frontier.push_back(c.calleeName);
            }
        }
        for (std::uint32_t depth = 1;
             depth <= policy.maxImportDepth && !frontier.empty(); ++depth) {
            std::sort(frontier.begin(), frontier.end());
            frontier.erase(std::unique(frontier.begin(), frontier.end()),
                           frontier.end());
            std::vector<std::string> next;
            for (std::string const& name : frontier) {
                if (taken.count(name) != 0) continue;
                auto const site = out.definitionOf(name);
                if (!site.has_value() || !site->isFunction) continue;
                // A callee defined in the importer's OWN module needs no
                // import — the in-module inliner already reaches it, and
                // charging it against the budget would crowd out real imports.
                if (site->moduleIndex == m) continue;
                SummaryFunction const& callee =
                    summaries[site->moduleIndex].functions[site->functionIndex];
                if (!isInlineCandidate(callee, policy, out.escapedSymbols))
                    continue;
                if (policy.perModuleImportInstBudget != 0
                    && spent + callee.instCount
                           > policy.perModuleImportInstBudget) {
                    continue;  // over budget — a LATER, cheaper callee may fit
                }
                spent += callee.instCount;
                taken.insert(name);
                out.importPlan.push_back(ImportDecision{
                    m, site->moduleIndex, name, callee.instCount, depth});
                for (SummaryCallSite const& c : callee.calls) {
                    if (c.direct && !c.calleeName.empty())
                        next.push_back(c.calleeName);
                }
            }
            frontier = std::move(next);
        }
    }
    std::sort(out.importPlan.begin(), out.importPlan.end(),
              [](ImportDecision const& a, ImportDecision const& b) {
                  if (a.importerModule != b.importerModule)
                      return a.importerModule < b.importerModule;
                  return a.calleeName < b.calleeName;
              });

    // ── (6) TIER-2 KEY INPUTS. See the `Tier2KeyInputs` docblock for why
    // keying the post-import object on the TU alone is a silent miscompile.
    std::string const policyIdentity = std::format(
        "inlineThreshold={};maxImportDepth={};budget={}",
        policy.inlineThreshold, policy.maxImportDepth,
        policy.perModuleImportInstBudget);
    for (std::uint32_t m = 0; m < summaries.size(); ++m) {
        out.tier2KeyInputs[m].moduleIndex    = m;
        out.tier2KeyInputs[m].ownDigest      = summaries[m].moduleDigest;
        out.tier2KeyInputs[m].policyIdentity = policyIdentity;
    }
    for (ImportDecision const& d : out.importPlan) {
        out.tier2KeyInputs[d.importerModule].importedFrom.emplace_back(
            d.calleeName, summaries[d.definingModule].moduleDigest);
    }
    for (Tier2KeyInputs& k : out.tier2KeyInputs) {
        std::sort(k.importedFrom.begin(), k.importedFrom.end());
        k.importedFrom.erase(
            std::unique(k.importedFrom.begin(), k.importedFrom.end()),
            k.importedFrom.end());
    }

    return out;
}

} // namespace dss::mirsum
