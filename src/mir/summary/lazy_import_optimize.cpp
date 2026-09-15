#include "mir/summary/lazy_import_optimize.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/merge/mir_merge.hpp"
#include "mir/mir_literal_pool.hpp"
#include "mir/mir_opcode.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <unordered_set>
#include <utility>
#include <variant>

namespace dss::mirsum {

namespace {

// A symbol's declared name, or "" — the ONE spelling of the lookup, so a table
// that is short (a symbol minted after the table was built) reads as anonymous
// rather than as an out-of-range crash.
[[nodiscard]] std::string const&
nameOfSymbol(std::span<std::string const> table, SymbolId s) {
    static std::string const kEmpty;
    if (s.v >= table.size()) return kEmpty;
    return table[s.v];
}

// Walk a literal, collecting every symbol-address target's NAME.
// D-MIR-NESTED-AGGREGATE-LITERAL-WALKS-RECURSE-PER-INITIALIZER-LEVEL: the field
// descent was host recursion with no cap; it is now the shared heap walker.
void collectLiteralSymbolNames(MirLiteralValue const& v,
                               std::span<std::string const> names,
                               std::vector<std::string>& out, bool& sawUnnamed) {
    forEachLiteralNode(v, [&](MirLiteralValue const& node) {
        auto const* sa = std::get_if<MirSymbolAddrValue>(&node.value);
        if (sa == nullptr) return;
        std::string const& n = nameOfSymbol(names, SymbolId{sa->symbol});
        if (n.empty()) {
            sawUnnamed = true;
            return;
        }
        out.push_back(n);
    });
}

void sortUnique(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

} // namespace

bool summariesDescribeModules(std::span<LazyImportCu const> cus,
                              std::span<ModuleSummary const> summaries,
                              DiagnosticReporter& reporter) {
    if (cus.size() != summaries.size()) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "summariesDescribeModules: {} module(s) but {} summary(ies) — the "
            "index's module indices would name the wrong modules "
            "(D-OPT11-LAZY-IMPORT-EDGE).",
            cus.size(), summaries.size());
        reporter.report(std::move(d));
        return false;
    }
    for (std::uint32_t m = 0; m < cus.size(); ++m) {
        LazyImportCu const& cu = cus[m];
        if (cu.mir == nullptr || cu.interner == nullptr
            || cu.symbolNames == nullptr) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "summariesDescribeModules: module #{} is missing its mir / "
                "interner / symbolNames (decomposed-input contract violation).",
                m);
            reporter.report(std::move(d));
            return false;
        }
        ModuleSummary const& s = summaries[m];
        auto mismatch = [&](std::string what) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "summariesDescribeModules: summary #{} does not describe module "
                "#{} ({}). An on-demand body fetch resolves a name through this "
                "summary's ORDINALS, so a summary paired with the wrong module "
                "returns the WRONG BODY — which is a silent miscompile, not a "
                "cache miss (D-OPT11-LAZY-IMPORT-EDGE).",
                m, m, what);
            reporter.report(std::move(d));
        };
        if (s.functions.size() != cu.mir->moduleFuncCount()) {
            mismatch(std::format("{} summary functions vs {} module functions",
                                 s.functions.size(),
                                 cu.mir->moduleFuncCount()));
            return false;
        }
        for (std::uint32_t i = 0;
             i < static_cast<std::uint32_t>(s.functions.size()); ++i) {
            MirFuncId const f = cu.mir->funcAt(i);
            if (cu.mir->funcSymbol(f).v == s.functions[i].symbol
                && nameOfSymbol(*cu.symbolNames, cu.mir->funcSymbol(f))
                       == s.functions[i].name)
                continue;
            mismatch(std::format(
                "function #{} is summarized as '{}' (symbol v={}) but the "
                "module has '{}' (symbol v={})",
                i, s.functions[i].name, s.functions[i].symbol,
                nameOfSymbol(*cu.symbolNames, cu.mir->funcSymbol(f)),
                cu.mir->funcSymbol(f).v));
            return false;
        }
        if (s.globals.size() != cu.mir->moduleGlobalCount()) {
            mismatch(std::format("{} summary globals vs {} module globals",
                                 s.globals.size(),
                                 cu.mir->moduleGlobalCount()));
            return false;
        }
        for (std::uint32_t i = 0;
             i < static_cast<std::uint32_t>(s.globals.size()); ++i) {
            MirGlobalId const g = cu.mir->globalAt(i);
            if (cu.mir->globalSymbol(g).v == s.globals[i].symbol
                && nameOfSymbol(*cu.symbolNames, cu.mir->globalSymbol(g))
                       == s.globals[i].name)
                continue;
            mismatch(std::format(
                "global #{} is summarized as '{}' (symbol v={}) but the module "
                "has '{}' (symbol v={})",
                i, s.globals[i].name, s.globals[i].symbol,
                nameOfSymbol(*cu.symbolNames, cu.mir->globalSymbol(g)),
                cu.mir->globalSymbol(g).v));
            return false;
        }
    }
    return true;
}

BodyReferences collectBodyReferences(Mir const& mir, MirFuncId fn,
                                     std::span<std::string const> symbolNames) {
    BodyReferences out;
    std::uint32_t const nb = mir.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        std::uint32_t const ni = mir.blockInstCount(b);
        for (std::uint32_t ii = 0; ii < ni; ++ii) {
            MirInstId const inst = mir.blockInstAt(b, ii);
            switch (mir.instOpcode(inst)) {
            case MirOpcode::GlobalAddr: {
                std::string const& n =
                    nameOfSymbol(symbolNames, mir.globalAddrSymbol(inst));
                if (n.empty()) {
                    out.sawUnnamed = true;
                    break;
                }
                out.names.push_back(n);
                break;
            }
            case MirOpcode::BlockAddressExport:
                // Anonymous BY CONSTRUCTION — it names an interior point of THIS
                // function's code and the clone mints it a fresh module-private
                // symbol. It is not a cross-TU reference and must not be counted
                // as an unnamed one.
                break;
            case MirOpcode::Const:
                collectLiteralSymbolNames(
                    mir.literalValue(mir.constLiteralIndex(inst)), symbolNames,
                    out.names, out.sawUnnamed);
                break;
            default:
                break;
            }
        }
    }
    sortUnique(out.names);
    return out;
}

bool isImportable(std::uint32_t importer, std::string const& calleeName,
                  std::span<LazyImportCu const> cus,
                  std::span<ModuleSummary const> summaries,
                  SummaryIndex const& index,
                  SummaryIndexPolicy const& policy) {
    auto const site = index.definitionOf(calleeName);
    if (!site.has_value() || !site->isFunction) return false;
    // A callee defined in the importer's OWN module needs no import — the
    // in-module inliner already reaches it.
    if (site->moduleIndex == importer) return false;
    if (site->moduleIndex >= summaries.size()) return false;
    ModuleSummary const& srcSummary = summaries[site->moduleIndex];
    if (site->functionIndex >= srcSummary.functions.size()) return false;
    SummaryFunction const& callee = srcSummary.functions[site->functionIndex];
    // The PERMISSIVE summary filter — unchanged, and deliberately the same call
    // the eager `importPlan` makes, so lazy and eager admit exactly the same
    // bodies and only the ORDER of discovery differs.
    if (!isInlineCandidate(callee, policy, index.escapedSymbols)) return false;

    // ── SATISFIABILITY, on the real body ─────────────────────────────────────
    if (site->moduleIndex >= cus.size()) return false;
    LazyImportCu const& src = cus[site->moduleIndex];
    if (src.mir == nullptr || src.symbolNames == nullptr) return false;
    if (site->functionIndex >= src.mir->moduleFuncCount()) return false;
    MirFuncId const f = src.mir->funcAt(site->functionIndex);
    // Defense in depth AT THE POINT OF USE. `summariesDescribeModules` proves
    // this correspondence globally and once; a caller that skipped it must still
    // never receive a wrong body — it must receive a crash.
    if (nameOfSymbol(*src.symbolNames, src.mir->funcSymbol(f)) != calleeName) {
        std::fprintf(stderr,
            "dss::isImportable fatal: the index places '%s' at module #%u "
            "function #%u, but that function is named '%s' — this fetch would "
            "have imported the WRONG BODY. Call `summariesDescribeModules` "
            "before using an index (D-OPT11-LAZY-IMPORT-EDGE).\n",
            calleeName.c_str(), site->moduleIndex, site->functionIndex,
            nameOfSymbol(*src.symbolNames, src.mir->funcSymbol(f)).c_str());
        std::abort();
    }
    BodyReferences const refs = collectBodyReferences(*src.mir, f, *src.symbolNames);
    if (refs.sawUnnamed) return false;
    for (std::string const& r : refs.names) {
        if (index.winners.count(r) != 0) continue;   // a cross-TU definition
        bool viaImport = false;
        for (SummaryImport const& e : srcSummary.imports) {
            if (e.mangledName == r) { viaImport = true; break; }
        }
        if (viaImport) continue;                     // a library symbol we copy
        // ⚠ A REFERENCED-ONLY SHIPPED-LIBRARY SHIM LANDS HERE AND IS REFUSED,
        // deliberately. On pe64 a name like `printf` is neither a cross-CU
        // definition nor an import row — its definition is SYNTHESIZED after the
        // whole-program merge, from a recipe map the importer has no way to
        // learn from another TU. Importing such a body would clone a
        // `GlobalAddr` to a symbol nothing defines. The cost is a missed import
        // on that platform, which is the conservative direction.
        // Anything else is module-private to the SOURCE TU — a `static` object,
        // whose identity and state cannot be duplicated into the importer. The
        // body is UNAVAILABLE. (Promoting such a referent, the way LLVM ThinLTO
        // does, is a separate transformation with its own correctness argument;
        // refusing is the conservative direction and costs only a missed
        // import.)
        return false;
    }
    return true;
}

namespace {

// One import batch: the names to page in, grouped by defining module.
struct ImportBatch {
    // definingModule → the names to take from it, sorted.
    std::vector<std::pair<std::uint32_t, std::vector<std::string>>> byModule;
    std::uint32_t total = 0;
};

// ★★★ THE AVAILABILITY CLOSURE — RUN TO EXHAUSTION, FETCHED IN BATCHES.
//
// Two properties, and the whole fork-dissolution claim rests on keeping them
// apart:
//
//   * WHAT is taken is the transitive closure of importable callees, computed
//     over the SUMMARY call graph. It runs until the frontier is empty, so
//     `levels` cannot change it. What keeps it from degenerating into the whole
//     program is `isInlineCandidate` — the closure of functions already small
//     enough to inline is small.
//   * HOW MANY FETCH BATCHES it costs is `ceil(levels-traversed / levels)`, and
//     THAT is what `maxImportDepth` controls. It is the number of round trips a
//     provider reading `.dss.mir` sections off disk would issue.
//
// ⚠ SO `levels` MUST NEVER BOUND THE LOOP, ONLY CHUNK IT. Bounding it would put
// prefetch depth back in charge of what a TU may ever see, which is precisely
// the eager behaviour this row exists to replace — and it would make the emitted
// module a function of a latency knob, against the operator's ruling that
// optimized output be byte-identical for any prefetch depth.
//
// ⚠ SORTED AT EVERY LEVEL. Under a nonzero budget WHICH imports are charged
// first decides which fit, so the frontier is ordered by name before it is
// spent — otherwise two runs of the same build could produce different modules
// because a worklist popped in a different order.
[[nodiscard]] ImportBatch
takeClosure(std::uint32_t importer, std::vector<std::string> frontier,
            std::uint32_t levels, std::span<LazyImportCu const> cus,
            std::span<ModuleSummary const> summaries, SummaryIndex const& index,
            SummaryIndexPolicy const& policy,
            std::unordered_set<std::string>& taken, std::uint32_t& spent,
            std::uint32_t& batches) {
    std::unordered_map<std::uint32_t, std::vector<std::string>> picked;
    ImportBatch batch;
    while (!frontier.empty()) {
        ++batches;   // one fetch batch — `levels` call-graph levels of it
        for (std::uint32_t level = 0; level < levels && !frontier.empty();
             ++level) {
            sortUnique(frontier);
            std::vector<std::string> next;
            for (std::string const& name : frontier) {
                if (taken.count(name) != 0) continue;
                if (!isImportable(importer, name, cus, summaries, index, policy))
                    continue;
                auto const site = index.definitionOf(name);
                SummaryFunction const& callee =
                    summaries[site->moduleIndex].functions[site->functionIndex];
                if (policy.perModuleImportInstBudget != 0
                    && spent + callee.instCount
                           > policy.perModuleImportInstBudget) {
                    // Over budget — a later, cheaper callee may still fit. This
                    // is option B of the §B fork, and it is the ONE knob that
                    // legitimately changes the outcome.
                    continue;
                }
                spent += callee.instCount;
                taken.insert(name);
                picked[site->moduleIndex].push_back(name);
                ++batch.total;
                for (SummaryCallSite const& c : callee.calls) {
                    if (c.direct && !c.calleeName.empty())
                        next.push_back(c.calleeName);
                }
            }
            frontier = std::move(next);
        }
    }
    batch.byModule.reserve(picked.size());
    for (auto& [m, names] : picked) {
        sortUnique(names);
        batch.byModule.emplace_back(m, std::move(names));
    }
    // Module order, so the merge's CU sequence — and therefore its symbol
    // numbering — is a pure function of the batch rather than of a hash table.
    std::sort(batch.byModule.begin(), batch.byModule.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
    return batch;
}

// Every direct callee `mir` NAMES that it does not itself define.
[[nodiscard]] std::vector<std::string>
outboundCallees(Mir const& mir, std::span<std::string const> symbolNames) {
    std::unordered_set<std::string> defined;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        std::string const& n =
            nameOfSymbol(symbolNames, mir.funcSymbol(mir.funcAt(fi)));
        if (!n.empty()) defined.insert(n);
    }
    std::vector<std::string> out;
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const inst = mir.blockInstAt(b, ii);
                if (mir.instOpcode(inst) != MirOpcode::Call) continue;
                auto const ops = mir.instOperands(inst);
                if (ops.empty()) continue;
                if (mir.instOpcode(ops[0]) != MirOpcode::GlobalAddr) continue;
                std::string const& n =
                    nameOfSymbol(symbolNames, mir.globalAddrSymbol(ops[0]));
                if (n.empty() || defined.count(n) != 0) continue;
                out.push_back(n);
            }
        }
    }
    sortUnique(out);
    return out;
}

// After a batch has been merged in: declare every reference the merged module
// makes but does not define, so the module is LINKABLE, and mark every imported
// body available-externally so `optimize()`'s existing strip removes it.
//
// ⚠ ID-KEYED, NEVER NAME-KEYED. Two merged symbols may legitimately carry the
// same name (the FFI case where two declared images own one name), so a
// name→id reverse map would silently fold them. Every decision below is made on
// the merged SymbolId, which is unique by construction.
void declareUndefinedReferences(
    Mir const& merged,
    std::unordered_map<std::uint32_t, std::string> const& symbolNames,
    std::span<ModuleSummary const> summaries, SummaryIndex const& index,
    std::unordered_map<std::uint32_t, std::string> const& synthRecipes,
    std::vector<ExternImport>& externs) {
    std::unordered_set<std::uint32_t> defined;
    std::size_t const nf = merged.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi)
        defined.insert(merged.funcSymbol(merged.funcAt(fi)).v);
    std::size_t const ng = merged.moduleGlobalCount();
    for (std::uint32_t gi = 0; gi < ng; ++gi)
        defined.insert(merged.globalSymbol(merged.globalAt(gi)).v);
    // ★ TWO MORE KINDS OF "ALREADY ACCOUNTED FOR", AND MISSING EITHER ABORTS A
    // BUILD THAT IS PERFECTLY WELL-FORMED.
    //
    //   1. A BLOCK-EXPORT SYMBOL IS DEFINED BY ITS OWN INSTRUCTION. It names an
    //      interior code point, it is anonymous by construction, and a static
    //      initializer relocates against it — see
    //      D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED — so the literal
    //      walk below WILL reach it. It is not undefined, and it can never be
    //      an extern.
    //   2. A REFERENCED-ONLY SHIPPED-LIBRARY SHIM has no definition and no
    //      extern row ON PURPOSE: its body is synthesized after the whole-program
    //      merge from the recipe map. Declaring it as an import would emit an
    //      import row for a symbol DSS is about to define itself.
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = merged.funcAt(fi);
        for (std::uint32_t bi = 0; bi < merged.funcBlockCount(f); ++bi) {
            MirBlockId const b = merged.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < merged.blockInstCount(b); ++ii) {
                MirInstId const inst = merged.blockInstAt(b, ii);
                if (merged.instOpcode(inst) != MirOpcode::BlockAddressExport)
                    continue;
                defined.insert(merged.blockAddressExportSymbol(inst).v);
            }
        }
    }
    for (auto const& [v, recipe] : synthRecipes) defined.insert(v);
    std::unordered_set<std::uint32_t> declared;
    for (ExternImport const& e : externs) declared.insert(e.symbol.v);

    std::vector<std::uint32_t> undefined;
    auto note = [&](std::uint32_t v) {
        if (defined.count(v) != 0 || declared.count(v) != 0) return;
        declared.insert(v);           // once, however many references there are
        undefined.push_back(v);
    };
    // D-MIR-NESTED-AGGREGATE-LITERAL-WALKS-RECURSE-PER-INITIALIZER-LEVEL: was a
    // `self(self, …)` recursive lambda, one host frame per brace level, no cap.
    auto noteLiteral = [&](MirLiteralValue const& v) -> void {
        forEachLiteralNode(v, [&](MirLiteralValue const& n) {
            if (auto const* sa = std::get_if<MirSymbolAddrValue>(&n.value)) {
                note(sa->symbol);
            }
        });
    };
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = merged.funcAt(fi);
        std::uint32_t const nb = merged.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = merged.funcBlockAt(f, bi);
            std::uint32_t const ni = merged.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const inst = merged.blockInstAt(b, ii);
                switch (merged.instOpcode(inst)) {
                case MirOpcode::GlobalAddr:
                    note(merged.globalAddrSymbol(inst).v);
                    break;
                case MirOpcode::Const:
                    noteLiteral(merged.literalValue(
                        merged.constLiteralIndex(inst)));
                    break;
                default:
                    break;
                }
            }
        }
    }
    for (std::uint32_t gi = 0; gi < ng; ++gi) {
        std::uint32_t const lit =
            merged.globalInitLiteralIndex(merged.globalAt(gi));
        if (lit != UINT32_MAX) noteLiteral(merged.literalValue(lit));
    }
    // Sorted, so the emitted row order — and therefore the module's import
    // table — is a pure function of the module rather than of a walk order.
    std::sort(undefined.begin(), undefined.end());

    for (std::uint32_t v : undefined) {
        auto const it = symbolNames.find(v);
        if (it == symbolNames.end() || it->second.empty()) {
            // Unnameable and undefined. `isImportable` refuses every body that
            // could produce this, so reaching here means the availability rule
            // and the clone disagree — a module that cannot link, and the one
            // outcome worse than refusing the import.
            std::fprintf(stderr,
                "dss::lazyImportOptimize fatal: merged symbol v=%u is "
                "referenced, undefined and unnamed — the satisfiability rule "
                "in `isImportable` admitted a body it should have refused "
                "(D-OPT11-LAZY-IMPORT-EDGE).\n", v);
            std::abort();
        }
        std::string const& name = it->second;
        ExternImport row;
        row.symbol      = SymbolId{v};
        row.mangledName = name;
        // A PROGRAM definition wins over a library one — the same precedence
        // the merge itself applies when it strips an extern whose name has a
        // cross-CU definition.
        if (auto const w = index.definitionOf(name); w.has_value()) {
            row.isData = !w->isFunction;
        } else {
            // A library symbol: copy the DECLARED row so the importer names the
            // same dynamic symbol the source TU does. Spelling it with an empty
            // `libraryPath` instead would make it a DIFFERENT import identity
            // (`ffiImportKey` is the (name, library, version) triple), which is
            // exactly how a split CRT is built.
            bool found = false;
            for (ModuleSummary const& s : summaries) {
                for (SummaryImport const& e : s.imports) {
                    if (e.mangledName != name) continue;
                    row.libraryPath   = e.libraryPath;
                    row.version       = e.version;
                    row.isData        = e.isData;
                    row.isThreadLocal = e.isThreadLocal;
                    found = true;
                    break;
                }
                if (found) break;
            }
            if (!found) {
                std::fprintf(stderr,
                    "dss::lazyImportOptimize fatal: merged symbol '%s' is "
                    "referenced and undefined, and no summary declares it as "
                    "an import — the satisfiability rule admitted a body it "
                    "should have refused (D-OPT11-LAZY-IMPORT-EDGE).\n",
                    name.c_str());
                std::abort();
            }
        }
        // NEVER EAGER, whatever the source row said. An eager row binds even
        // when unreferenced, and this row exists only because a body we may
        // inline away happens to reference the symbol. The source TU still
        // carries its own eager row, so the eager BINDING is unaffected — what
        // this avoids is an import the importer does not actually need.
        row.isEagerImport = false;
        externs.push_back(std::move(row));
    }
}

} // namespace

LazyImportOutcome
lazyImportOptimize(std::uint32_t importer,
                   std::span<LazyImportCu const> cus,
                   std::span<ModuleSummary const> summaries,
                   SummaryIndex const& index, SummaryIndexPolicy const& policy,
                   std::string_view sourceLanguage,
                   LazyOptimizeFn const& optimizeOne,
                   DiagnosticReporter& reporter) {
    LazyImportOutcome out;
    if (importer >= cus.size() || importer >= summaries.size()
        || cus[importer].mir == nullptr || cus[importer].interner == nullptr
        || cus[importer].symbolNames == nullptr || !optimizeOne) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "lazyImportOptimize: importer #{} is out of range for {} CUs / {} "
            "summaries, or no optimize callback was supplied.",
            importer, cus.size(), summaries.size());
        reporter.report(std::move(d));
        return out;
    }

    // The importer's live state. It is REPLACED wholesale by every import batch
    // (a cross-module clone re-interns types and unifies symbol spaces), so the
    // three travel together and nothing below ever reads a stale one.
    TypeInterner const* liveInterner = cus[importer].interner;
    CompilationUnitId const ownerCu  = liveInterner->owner();
    std::string const srcLanguage{sourceLanguage};
    std::vector<std::string>                       liveNames    = *cus[importer].symbolNames;
    std::vector<ExternImport>                      liveExterns(
        cus[importer].externImports.begin(), cus[importer].externImports.end());
    std::unique_ptr<TypeLattice>                   liveHost;
    std::unordered_map<std::uint32_t, std::string> liveNameMap;
    // The importer's shim recipes, RE-KEYED after every merge. A shim symbol is
    // referenced with no definition and no import row, so the planner has to be
    // told it exists or the clone aborts on the first `GlobalAddr` naming one.
    std::unordered_map<std::uint32_t, std::string> liveRecipes;
    std::unordered_map<std::string, std::string>   recipeByName;
    if (cus[importer].synthRecipes != nullptr) {
        liveRecipes = *cus[importer].synthRecipes;
        for (auto const& [v, recipe] : liveRecipes) {
            std::string const& n = nameOfSymbol(liveNames, SymbolId{v});
            if (!n.empty()) recipeByName.emplace(n, recipe);
        }
    }

    // Names already paged in. It is what stops the OUTER loop re-importing a
    // body `stripInlineDefinitions` has just removed — the module names the
    // callee again the moment its body is gone, and without this the two would
    // chase each other forever.
    std::unordered_set<std::string> taken;
    std::uint32_t spent = 0;

    // The importer's module as the loop sees it. Until the first merge it is the
    // CU's own, borrowed const; from then on it is the locally-owned rewrite.
    Mir const* readMir = cus[importer].mir;
    Mir        owned;

    // ⓘ `maxImportDepth` is a FETCH BATCH SIZE here, not a bound: see
    // `takeClosure`. 0 would mean "fetch nothing per batch", which is a stall
    // rather than a policy, so it clamps to one level.
    std::uint32_t const levels =
        policy.maxImportDepth == 0 ? 1u : policy.maxImportDepth;

    for (std::uint32_t round = 0;; ++round) {
        // ── INNER: close availability over the CURRENT module, ONE MERGE ─────
        // The closure is computed FIRST, to exhaustion, and only then cloned —
        // so a round costs exactly one merge whatever `maxImportDepth` is, and
        // the merge's symbol renumbering (a function of the CU set it is handed)
        // cannot become a function of a latency knob.
        std::vector<std::string> frontier = outboundCallees(*readMir, liveNames);
        ImportBatch batch =
            takeClosure(importer, std::move(frontier), levels, cus, summaries,
                        index, policy, taken, spent, out.importBatches);
        if (batch.total != 0) {
            std::vector<MergeCuInput> inputs;
            inputs.reserve(batch.byModule.size() + 1);
            // Slot 0 is the DESTINATION — the importer keeps its symbol values.
            inputs.push_back(MergeCuInput{
                readMir, liveInterner,
                [&liveNames](SymbolId s) { return nameOfSymbol(liveNames, s); },
                liveExterns, &liveRecipes, nullptr});
            for (auto const& [m, names] : batch.byModule) {
                inputs.push_back(MergeCuInput{
                    cus[m].mir, cus[m].interner,
                    [tbl = cus[m].symbolNames](SymbolId s) {
                        return nameOfSymbol(*tbl, s);
                    },
                    {}, nullptr, &names});
            }

            // The importer's OWN cu id, so a module that has been through an
            // import still identifies as the TU it is. Minting a synthetic id
            // would make the lattice's owner disagree with every other artifact
            // this TU produces.
            TypeLattice fresh{ownerCu, srcLanguage};
            auto merged = mergeCuMirs(inputs, std::move(fresh),
                                      /*entryNames=*/{}, reporter);
            if (!merged.has_value()) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "lazyImportOptimize: the on-demand body import for module "
                    "#{} failed to clone {} function(s) "
                    "(D-OPT11-LAZY-IMPORT-EDGE).",
                    importer, batch.total);
                reporter.report(std::move(d));
                return out;
            }

            liveHost = std::make_unique<TypeLattice>(std::move(merged->host));
            liveInterner = &liveHost->interner();
            liveNameMap  = std::move(merged->symbolNames);
            liveExterns  = std::move(merged->externImports);
            // The flat table the next round reads. Rebuilt rather than patched:
            // a merge renumbers, so a patched table would describe the module
            // that went IN.
            std::uint32_t maxV = 0;
            for (auto const& [v, n] : liveNameMap) maxV = std::max(maxV, v);
            liveNames.assign(static_cast<std::size_t>(maxV) + 1, std::string{});
            for (auto const& [v, n] : liveNameMap) liveNames[v] = n;
            // Re-key the shim recipes onto the merged ids. By NAME, because a
            // name is the only thing that survives a renumbering — and the map
            // that went in now describes symbols that no longer exist.
            liveRecipes.clear();
            for (auto const& [v, n] : liveNameMap) {
                auto const it = recipeByName.find(n);
                if (it != recipeByName.end()) liveRecipes.emplace(v, it->second);
            }

            owned   = std::move(merged->mir);
            readMir = &owned;

            // Mark every body just imported available-externally, and declare
            // everything the enlarged module now references but does not define.
            std::unordered_set<std::uint32_t> alreadyDeclared;
            for (ExternImport const& e : liveExterns)
                alreadyDeclared.insert(e.symbol.v);
            std::size_t const nf = owned.moduleFuncCount();
            std::vector<std::pair<std::uint32_t, std::string>> importedRows;
            for (std::uint32_t fi = 0; fi < nf; ++fi) {
                SymbolId const s = owned.funcSymbol(owned.funcAt(fi));
                std::string const& n = nameOfSymbol(liveNames, s);
                if (n.empty() || taken.count(n) == 0) continue;
                if (alreadyDeclared.count(s.v) != 0) continue;
                alreadyDeclared.insert(s.v);
                importedRows.emplace_back(s.v, n);
            }
            std::sort(importedRows.begin(), importedRows.end());
            for (auto const& [v, n] : importedRows) {
                ExternImport row;
                row.symbol        = SymbolId{v};
                row.mangledName   = n;
                row.isData        = false;   // a function body, always
                row.isEagerImport = false;
                liveExterns.push_back(std::move(row));
            }
            declareUndefinedReferences(owned, liveNameMap, summaries, index,
                                       liveRecipes, liveExterns);

            out.importedBodies += batch.total;
            ++out.importMerges;
        }

        // ── the unchanged optimizer, on the post-import module ───────────────
        //
        // ⓘ A TU THAT IMPORTED NOTHING IS LEFT ENTIRELY ALONE. Running the
        // program-stage pipeline over it here would change what every build
        // emits for a TU this stage had nothing to offer — and the whole-program
        // optimize that follows still sees it. Strictly additive means exactly
        // this: no import, no change.
        if (out.importedBodies == 0) break;
        if (!optimizeOne(owned, *liveInterner, liveExterns)) return out;
        ++out.optimizeRuns;

        // ── OUTER: did the optimize EXPOSE demand the closure never saw? ─────
        std::vector<std::string> fresh;
        for (std::string const& n : outboundCallees(owned, liveNames)) {
            if (taken.count(n) != 0) continue;
            if (isImportable(importer, n, cus, summaries, index, policy))
                fresh.push_back(n);
        }
        if (fresh.empty()) break;
        if (round + 1 >= kMaxImportRounds) {
            out.demandLeftAtBound = static_cast<std::uint32_t>(fresh.size());
            ParseDiagnostic d;
            // ★ THE SAME CODE THE PASS-SCHEDULE FIXPOINT USES WHEN IT HITS ITS
            // CAP, and the same severity, because it is the same event one tier
            // out: a bounded loop stopped because it ran out of rounds, not
            // because it converged. The module is correct; it is simply not the
            // module a converged import fixpoint would have produced. Minting a
            // second code for the identical fact would let one of them be
            // suppressed while the other is not.
            d.code     = DiagnosticCode::X_OptFixpointTruncated;
            d.severity = DiagnosticSeverity::Warning;
            d.actual   = std::format(
                "lazyImportOptimize: module #{} still had {} importable "
                "callee(s) after {} optimize round(s) — the import fixpoint hit "
                "its bound. The module is CORRECT; some cross-CU inlining was "
                "left for the whole-program stage (D-OPT11-LAZY-IMPORT-EDGE).",
                importer, fresh.size(), round + 1);
            reporter.report(std::move(d));
            break;
        }
    }

    out.ok = true;
    if (out.importedBodies > 0) {
        out.mir           = std::move(owned);
        out.host          = std::move(liveHost);
        out.symbolNames   = std::move(liveNameMap);
        out.externImports = std::move(liveExterns);
        out.synthRecipes  = std::move(liveRecipes);
    }
    return out;
}

} // namespace dss::mirsum
