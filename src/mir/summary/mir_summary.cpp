#include "mir/summary/mir_summary.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dss::mirsum {

namespace {

// ── the frame/ABI-bound opcode set ─────────────────────────────────────────
//
// EXACTLY the opcodes `opt/passes/inlining.cpp::inlineLegalityGate` rule 5
// refuses on, minus the two that get their own summary flags (computed goto,
// SEH). Every one of these binds to the CALLEE's own frame and would silently
// bind to the CALLER's if spliced.
//
// ⚠ THIS SET AND THE GATE MUST NOT DRIFT. They are two spellings of one rule,
// and the failure mode of drift is ASYMMETRIC, which is why the summary is the
// PERMISSIVE half: if the gate grows a refusal this set does not have, the
// index merely makes a body available that the gate then declines — a wasted
// import, nothing worse. The reverse (this set refusing something the gate
// would allow) is the direction that silently costs inlining quality, so a
// maintainer adding a REFUSAL to the gate need not touch this file, while one
// REMOVING a refusal from the gate should.
[[nodiscard]] bool isFrameBoundOpcode(Mir const& mir, MirInstId id,
                                      MirOpcode op) noexcept {
    switch (op) {
        // A by-value struct returned via x8-sret: spliced, it would read the
        // CALLER's indirect-result register.
        case MirOpcode::ReadIndirectResult:
        // A fixed by-value aggregate received WHOLLY from the incoming stack:
        // spliced, it binds to the CALLER's incoming frame.
        case MirOpcode::RecvByValueStackParam:
        // The three va_start area leaves lower to `lea reg, [sp + offset]`
        // against the CALLEE's OWN variadic prologue.
        case MirOpcode::VaRegSaveAreaAddr:
        case MirOpcode::VaOverflowArgAreaAddr:
        case MirOpcode::VaHomeArgAreaAddr:
        // VLA teardown — a moving SP plus per-function scope ids that would
        // collide on a twice-inlined callee.
        case MirOpcode::StackSave:
        case MirOpcode::StackRestore:
            return true;
        case MirOpcode::Return:
            // A MULTI-PIECE return (a struct returned in registers) — the
            // splice paths take operand 0 only, truncating it to its first
            // field.
            return mir.instOperands(id).size() > 1;
        case MirOpcode::Call:
            // A returns-twice call (setjmp) captures the frame of the function
            // that CALLED it; spliced, a later longjmp restores the wrong SP.
            return has(mir.instFlags(id), MirInstFlags::ReturnsTwice);
        default:
            return false;
    }
}

[[nodiscard]] bool isSehOpcode(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::SehTryBegin:
        case MirOpcode::SehFilterReturn:
        case MirOpcode::SehTryEnd:
        case MirOpcode::SehExceptionCode:
        case MirOpcode::SehExceptionInfo:
            return true;
        default:
            return false;
    }
}

void sortUnique(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// Per-block loop-nest depth for one function: how many natural loops contain
// each block. The hotness proxy `SummaryCallSite::loopDepth` reports.
//
// Uses the CANDIDATE-SCOPED `mirNaturalLoops` overload with the completeness
// recipe its docblock specifies (the function's own range ∪ its RPO ∪ the
// module's self-looping blocks). The whole-module overload would be
// byte-identical but O(functions × module blocks) — the exact shape
// [[D-OPT-LICM-NATURAL-LOOPS-MODULE-WIDE-SCAN]] was opened for, and a summary
// walk that reintroduced it would put an O(N²) back into every compile.
void computeLoopDepths(Mir const& mir, MirFuncId f,
                       std::vector<MirBlockId> const& rpo,
                       std::vector<std::vector<MirBlockId>> const& preds,
                       std::span<std::uint32_t const> moduleSelfLoops,
                       std::vector<std::uint32_t>& candidateScratch,
                       std::unordered_map<std::uint32_t, std::uint32_t>& out) {
    out.clear();
    if (rpo.empty()) return;
    MirDomTree const dom = computeMirDomTree(mir, mir.funcEntry(f), rpo, preds);
    mirBackEdgeCandidates(mir, f, rpo, moduleSelfLoops, candidateScratch);
    std::vector<MirNaturalLoop> const loops =
        mirNaturalLoops(mir, dom, preds, candidateScratch);
    for (MirNaturalLoop const& loop : loops) {
        for (MirBlockId const b : loop.body) ++out[b.v];
    }
}

} // namespace

std::string summaryImportKey(SummaryImport const& e) {
    return std::format("{}:{}|{}:{}|{}:{}",
                       e.mangledName.size(), e.mangledName,
                       e.libraryPath.size(), e.libraryPath,
                       e.version.size(),     e.version);
}

std::string summaryImportKey(ExternImport const& e) {
    return std::format("{}:{}|{}:{}|{}:{}",
                       e.mangledName.size(), e.mangledName,
                       e.libraryPath.size(), e.libraryPath,
                       e.version.size(),     e.version);
}

ModuleSummary buildModuleSummary(SummaryCuInput const& cu) {
    if (cu.mir == nullptr || !cu.nameOf) {
        std::fputs("dss::mirsum::buildModuleSummary fatal: SummaryCuInput is "
                   "missing its mir / nameOf (decomposed-input contract "
                   "violation).\n", stderr);
        std::abort();
    }
    Mir const& mir = *cu.mir;

    ModuleSummary out;
    out.moduleDigest   = cu.moduleDigest;
    out.targetIdentity = cu.targetIdentity;

    // Names an `ExternImport` row declares in THIS TU. A defined function
    // whose name is also imported here is a C99 6.7.4p7 inline definition.
    std::unordered_set<std::string> importedNames;
    out.imports.reserve(cu.externImports.size());
    for (ExternImport const& e : cu.externImports) {
        SummaryImport row;
        row.mangledName   = e.mangledName;
        row.libraryPath   = e.libraryPath;
        row.version       = e.version;
        row.symbol        = e.symbol.v;
        row.isData        = e.isData;
        row.isThreadLocal = e.isThreadLocal;
        if (!e.mangledName.empty()) importedNames.insert(e.mangledName);
        out.imports.push_back(std::move(row));
    }

    // ── the whole-program escape scan ─────────────────────────────────────
    //
    // Mirrors `inlining.cpp::analyzeModule` step 2 EXACTLY: a `GlobalAddr`
    // whose use is anything other than operand[0] of a `Call` escapes. A
    // `GlobalAddr` flowing through a `Phi` escapes unconditionally (the merged
    // value could be called indirectly). A `GlobalAddr` with no uses does not
    // escape.
    //
    // ★ Only NAMED symbols are recorded. An unnamed (module-private) symbol
    // cannot be referenced from another TU, so the importing TU's own
    // `analyzeModule` already sees its escapes — putting it in the wire format
    // would carry a value no other TU can interpret.
    std::vector<std::string> escaped;
    auto noteEscape = [&](SymbolId sym) {
        std::string name = cu.nameOf(sym);
        if (!name.empty()) escaped.push_back(std::move(name));
    };

    std::vector<std::vector<MirBlockId>> const preds = mirBuildPredecessors(mir);
    std::vector<std::uint32_t> moduleSelfLoops;
    mirModuleSelfLoopBlocks(mir, moduleSelfLoops);
    std::vector<std::uint32_t> candidateScratch;
    std::unordered_map<std::uint32_t, std::uint32_t> loopDepth;

    std::size_t const nf = mir.moduleFuncCount();
    out.functions.reserve(nf);
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);

        SummaryFunction row;
        row.name         = cu.nameOf(mir.funcSymbol(f));
        row.symbol       = mir.funcSymbol(f).v;
        row.binding      = mir.funcBinding(f);
        row.visibility   = mir.funcVisibility(f);
        row.noInline     = mir.funcNoInline(f);
        row.alwaysInline = mir.funcAlwaysInline(f);
        row.noOptimize   = mir.funcNoOptimize(f);
        row.isInlineDefinition =
            !row.name.empty() && importedNames.count(row.name) != 0;
        row.blockCount = mir.funcBlockCount(f);

        // Loop depths for this function's call sites. Computed over the RPO
        // from the entry — the same reachable set every pass works on.
        std::vector<MirBlockId> const rpo =
            mirReversePostOrder(mir, mir.funcEntry(f));
        computeLoopDepths(mir, f, rpo, preds, moduleSelfLoops, candidateScratch,
                          loopDepth);

        // ⚠ Walk blocks in NATURAL MODULE ORDER (`funcBlockAt`), not RPO. The
        // walk order is what fixes `calls` order in the wire format, and
        // natural order is a property of the arena — stable across stdlib
        // versions and independent of any CFG analysis. RPO would also be
        // deterministic, but it would make the summary's byte image depend on
        // the dominator computation, which is a needless coupling.
        std::vector<std::string> refs;
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const depth = [&] {
                auto const it = loopDepth.find(b.v);
                return it == loopDepth.end() ? 0u : it->second;
            }();
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                MirOpcode const op = mir.instOpcode(id);
                ++row.instCount;

                if (op == MirOpcode::BlockAddress || op == MirOpcode::IndirectBr)
                    row.hasComputedGoto = true;
                if (isSehOpcode(op)) row.hasSeh = true;
                if (isFrameBoundOpcode(mir, id, op)) row.frameBound = true;
                if (op == MirOpcode::Return) row.hasReturn = true;
                if (op == MirOpcode::Arg) {
                    row.argExtent =
                        std::max(row.argExtent, mir.argPosition(id) + 1);
                }
                if (op == MirOpcode::GlobalAddr) {
                    std::string name = cu.nameOf(mir.globalAddrSymbol(id));
                    if (!name.empty()) refs.push_back(std::move(name));
                }

                // Escape analysis + call-site recording share this operand
                // walk. A Phi addresses the phi pool, so it is handled apart.
                if (op == MirOpcode::Phi) {
                    for (auto const& inc : mir.phiIncomings(id)) {
                        if (mir.instOpcode(inc.value) == MirOpcode::GlobalAddr)
                            noteEscape(mir.globalAddrSymbol(inc.value));
                    }
                    continue;
                }
                auto const ops = mir.instOperands(id);
                bool const isCall = op == MirOpcode::Call;
                for (std::size_t oi = 0; oi < ops.size(); ++oi) {
                    MirInstId const operand = ops[oi];
                    if (mir.instOpcode(operand) != MirOpcode::GlobalAddr)
                        continue;
                    // operand[0] of a Call is the callee slot — a pure call
                    // target, not an escape. Anywhere else (including as an
                    // ARGUMENT of a Call) is a passed function pointer.
                    if (isCall && oi == 0) continue;
                    noteEscape(mir.globalAddrSymbol(operand));
                }

                if (!isCall) continue;
                SummaryCallSite site;
                site.loopDepth = depth;
                if (!ops.empty()
                    && mir.instOpcode(ops[0]) == MirOpcode::GlobalAddr) {
                    site.calleeName = cu.nameOf(mir.globalAddrSymbol(ops[0]));
                    site.direct     = !site.calleeName.empty();
                }
                if (!site.direct) row.hasIndirectCall = true;
                row.calls.push_back(std::move(site));
            }
        }
        sortUnique(refs);
        row.symbolRefs = std::move(refs);
        out.functions.push_back(std::move(row));
    }

    std::size_t const ng = mir.moduleGlobalCount();
    out.globals.reserve(ng);
    for (std::uint32_t gi = 0; gi < ng; ++gi) {
        MirGlobalId const g = mir.globalAt(gi);
        SummaryGlobal row;
        row.name           = cu.nameOf(mir.globalSymbol(g));
        row.symbol         = mir.globalSymbol(g).v;
        row.binding        = mir.globalBinding(g);
        row.visibility     = mir.globalVisibility(g);
        row.isConst        = mir.globalIsConst(g);
        row.isThreadLocal  = mir.globalIsThreadLocal(g);
        row.alignmentBytes = mir.globalAlignmentBytes(g);
        row.hasInitFunc    = mir.globalInitFunc(g).valid();

        // `scanLiveSymbols` phase 3's edge set — descending into AGGREGATE
        // literals so a symbol-address LEAF nested inside a struct /
        // array-of-struct (a function-pointer table member) is found, not only
        // a top-level scalar. A function reachable ONLY through such a data
        // relocation is otherwise wrongly deleted.
        std::uint32_t const initIdx = mir.globalInitLiteralIndex(g);
        if (initIdx != UINT32_MAX) {
            std::vector<std::uint32_t> targets;
            // D-MIR-NESTED-AGGREGATE-LITERAL-WALKS-RECURSE-PER-INITIALIZER-LEVEL:
            // was a `self(self, …)` recursive lambda, one host frame per brace
            // level, no cap. ⚠ `targets` is a VECTOR whose order reaches the
            // summary, so the shared walker's field-order guarantee is required.
            forEachLiteralNode(mir.literalValue(initIdx),
                               [&](MirLiteralValue const& n) {
                if (auto const* sa = std::get_if<MirSymbolAddrValue>(&n.value)) {
                    targets.push_back(sa->symbol);
                }
            });
            std::vector<std::string> names;
            names.reserve(targets.size());
            for (std::uint32_t const sv : targets) {
                std::string name = cu.nameOf(SymbolId{sv});
                if (!name.empty()) names.push_back(std::move(name));
            }
            sortUnique(names);
            row.initSymbolRefs = std::move(names);
        }
        out.globals.push_back(std::move(row));
    }

    sortUnique(escaped);
    out.escapedSymbolNames = std::move(escaped);
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// THE WIRE FORMAT
// ═══════════════════════════════════════════════════════════════════════════
//
// Layout, all integers LITTLE-ENDIAN and written BYTE BY BYTE:
//
//   magic     "DSSSUM\0"            7 bytes
//   version   u32                   `kSummaryFormatVersion`
//   digest    str                   moduleDigest
//   target    str                   targetIdentity
//   nFunc     u32   then nFunc function rows
//   nGlobal   u32   then nGlobal global rows
//   nImport   u32   then nImport import rows
//   nEscape   u32   then nEscape strings
//
// where `str` is u32 length followed by that many raw bytes (NOT
// NUL-terminated — a symbol name is arbitrary bytes from a descriptor and may
// legally contain anything), and every bool is one byte 0/1.
//
// ★ NO STRING TABLE, deliberately. A dedup table would shrink the payload but
// would make the byte image depend on FIRST-SEEN ORDER across the whole
// summary — a coupling that turns any future reordering of the walk into a
// silent cache-key change. The `.dss.summary` section is the SMALL one by
// design (the bodies live in `.dss.mir`), so the trade is free.

namespace {

void putU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void putU8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }

void putBool(std::vector<std::uint8_t>& b, bool v) {
    b.push_back(v ? std::uint8_t{1} : std::uint8_t{0});
}

void putStr(std::vector<std::uint8_t>& b, std::string const& s) {
    putU32(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

void putStrs(std::vector<std::uint8_t>& b, std::vector<std::string> const& v) {
    putU32(b, static_cast<std::uint32_t>(v.size()));
    for (std::string const& s : v) putStr(b, s);
}

// A bounds-checked cursor. EVERY read goes through it, so a truncated or
// hostile buffer can never read past the end — it sets `bad` and every
// subsequent read is a no-op, which the top-level decode turns into nullopt.
struct Cursor {
    std::span<std::uint8_t const> b;
    std::size_t                   at  = 0;
    bool                          bad = false;

    [[nodiscard]] bool need(std::size_t n) {
        if (bad || at + n > b.size()) { bad = true; return false; }
        return true;
    }
    [[nodiscard]] std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t const v =
            static_cast<std::uint32_t>(b[at])
            | (static_cast<std::uint32_t>(b[at + 1]) << 8)
            | (static_cast<std::uint32_t>(b[at + 2]) << 16)
            | (static_cast<std::uint32_t>(b[at + 3]) << 24);
        at += 4;
        return v;
    }
    [[nodiscard]] std::uint8_t u8() {
        if (!need(1)) return 0;
        return b[at++];
    }
    [[nodiscard]] bool boolean() { return u8() != 0; }
    [[nodiscard]] std::string str() {
        std::uint32_t const n = u32();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<char const*>(b.data() + at), n);
        at += n;
        return s;
    }
    [[nodiscard]] std::vector<std::string> strs() {
        std::uint32_t const n = u32();
        if (bad) return {};
        // Cheapest possible overrun screen BEFORE reserving: each string costs
        // at least its 4-byte length prefix, so a count that cannot fit its own
        // prefixes is malformed. Without it a hostile 4-byte count would
        // reserve gigabytes before the per-element check ever ran.
        if (!need(static_cast<std::size_t>(n) * 4)) return {};
        std::vector<std::string> v;
        v.reserve(n);
        for (std::uint32_t i = 0; i < n && !bad; ++i) v.push_back(str());
        return v;
    }
    // A count screen for FIXED-MINIMUM-SIZE rows, same argument as `strs`.
    [[nodiscard]] bool countFits(std::uint32_t n, std::size_t minRowBytes) {
        return need(static_cast<std::size_t>(n) * minRowBytes);
    }
};

constexpr char        kMagic[]    = "DSSSUM";
constexpr std::size_t kMagicBytes = 7;  // includes the NUL

// A binding / visibility arriving from a foreign summary must be a DECLARED
// enumerator. A raw cast of an out-of-range byte would produce a
// `SymbolBinding` no switch handles — and this one feeds cross-TU symbol
// resolution, so a garbage value is a link-time miscompile, not a display bug.
[[nodiscard]] bool decodeBinding(std::uint8_t v, SymbolBinding& out) {
    switch (v) {
        case 0: out = SymbolBinding::Local;  return true;
        case 1: out = SymbolBinding::Global; return true;
        case 2: out = SymbolBinding::Weak;   return true;
        default: return false;
    }
}
[[nodiscard]] bool decodeVisibility(std::uint8_t v, SymbolVisibility& out) {
    switch (v) {
        case 0: out = SymbolVisibility::Default;   return true;
        case 1: out = SymbolVisibility::Hidden;    return true;
        case 2: out = SymbolVisibility::Protected; return true;
        case 3: out = SymbolVisibility::Internal;  return true;
        default: return false;
    }
}

} // namespace

std::vector<std::uint8_t> encodeModuleSummary(ModuleSummary const& s) {
    std::vector<std::uint8_t> b;
    b.insert(b.end(), kMagic, kMagic + kMagicBytes);
    putU32(b, kSummaryFormatVersion);
    putStr(b, s.moduleDigest);
    putStr(b, s.targetIdentity);

    putU32(b, static_cast<std::uint32_t>(s.functions.size()));
    for (SummaryFunction const& f : s.functions) {
        putStr(b, f.name);
        putU32(b, f.symbol);
        putU8(b, static_cast<std::uint8_t>(f.binding));
        putU8(b, static_cast<std::uint8_t>(f.visibility));
        putU32(b, f.instCount);
        putU32(b, f.argExtent);
        putU32(b, f.blockCount);
        putBool(b, f.noInline);
        putBool(b, f.alwaysInline);
        putBool(b, f.noOptimize);
        putBool(b, f.hasComputedGoto);
        putBool(b, f.hasSeh);
        putBool(b, f.frameBound);
        putBool(b, f.hasReturn);
        putBool(b, f.hasIndirectCall);
        putBool(b, f.isInlineDefinition);
        putStrs(b, f.symbolRefs);
        putU32(b, static_cast<std::uint32_t>(f.calls.size()));
        for (SummaryCallSite const& c : f.calls) {
            putStr(b, c.calleeName);
            putBool(b, c.direct);
            putU32(b, c.loopDepth);
        }
    }

    putU32(b, static_cast<std::uint32_t>(s.globals.size()));
    for (SummaryGlobal const& g : s.globals) {
        putStr(b, g.name);
        putU32(b, g.symbol);
        putU8(b, static_cast<std::uint8_t>(g.binding));
        putU8(b, static_cast<std::uint8_t>(g.visibility));
        putBool(b, g.isConst);
        putBool(b, g.isThreadLocal);
        putU32(b, g.alignmentBytes);
        putBool(b, g.hasInitFunc);
        putStrs(b, g.initSymbolRefs);
    }

    putU32(b, static_cast<std::uint32_t>(s.imports.size()));
    for (SummaryImport const& i : s.imports) {
        putStr(b, i.mangledName);
        putStr(b, i.libraryPath);
        putStr(b, i.version);
        putU32(b, i.symbol);
        putBool(b, i.isData);
        putBool(b, i.isThreadLocal);
    }

    putStrs(b, s.escapedSymbolNames);
    return b;
}

std::optional<ModuleSummary>
decodeModuleSummary(std::span<std::uint8_t const> bytes) {
    Cursor c{bytes};
    if (!c.need(kMagicBytes)) return std::nullopt;
    for (std::size_t i = 0; i < kMagicBytes; ++i) {
        if (bytes[i] != static_cast<std::uint8_t>(kMagic[i]))
            return std::nullopt;
    }
    c.at = kMagicBytes;
    if (c.u32() != kSummaryFormatVersion) return std::nullopt;

    ModuleSummary s;
    s.moduleDigest   = c.str();
    s.targetIdentity = c.str();

    {
        std::uint32_t const n = c.u32();
        // Smallest possible function row: 4 (name len) + 4 (symbol) + 2
        // (binding/visibility) + 12 (three u32) + 9 (bools) + 4 (refs count)
        // + 4 (calls count) = 39.
        if (!c.countFits(n, 39)) return std::nullopt;
        s.functions.reserve(n);
        for (std::uint32_t i = 0; i < n && !c.bad; ++i) {
            SummaryFunction f;
            f.name   = c.str();
            f.symbol = c.u32();
            if (!decodeBinding(c.u8(), f.binding)) return std::nullopt;
            if (!decodeVisibility(c.u8(), f.visibility)) return std::nullopt;
            f.instCount          = c.u32();
            f.argExtent          = c.u32();
            f.blockCount         = c.u32();
            f.noInline           = c.boolean();
            f.alwaysInline       = c.boolean();
            f.noOptimize         = c.boolean();
            f.hasComputedGoto    = c.boolean();
            f.hasSeh             = c.boolean();
            f.frameBound         = c.boolean();
            f.hasReturn          = c.boolean();
            f.hasIndirectCall    = c.boolean();
            f.isInlineDefinition = c.boolean();
            f.symbolRefs         = c.strs();
            std::uint32_t const nc = c.u32();
            if (!c.countFits(nc, 9)) return std::nullopt;
            f.calls.reserve(nc);
            for (std::uint32_t k = 0; k < nc && !c.bad; ++k) {
                SummaryCallSite site;
                site.calleeName = c.str();
                site.direct     = c.boolean();
                site.loopDepth  = c.u32();
                f.calls.push_back(std::move(site));
            }
            s.functions.push_back(std::move(f));
        }
    }
    {
        std::uint32_t const n = c.u32();
        if (!c.countFits(n, 21)) return std::nullopt;
        s.globals.reserve(n);
        for (std::uint32_t i = 0; i < n && !c.bad; ++i) {
            SummaryGlobal g;
            g.name   = c.str();
            g.symbol = c.u32();
            if (!decodeBinding(c.u8(), g.binding)) return std::nullopt;
            if (!decodeVisibility(c.u8(), g.visibility)) return std::nullopt;
            g.isConst        = c.boolean();
            g.isThreadLocal  = c.boolean();
            g.alignmentBytes = c.u32();
            g.hasInitFunc    = c.boolean();
            g.initSymbolRefs = c.strs();
            s.globals.push_back(std::move(g));
        }
    }
    {
        std::uint32_t const n = c.u32();
        if (!c.countFits(n, 18)) return std::nullopt;
        s.imports.reserve(n);
        for (std::uint32_t i = 0; i < n && !c.bad; ++i) {
            SummaryImport row;
            row.mangledName   = c.str();
            row.libraryPath   = c.str();
            row.version       = c.str();
            row.symbol        = c.u32();
            row.isData        = c.boolean();
            row.isThreadLocal = c.boolean();
            s.imports.push_back(std::move(row));
        }
    }
    s.escapedSymbolNames = c.strs();

    if (c.bad) return std::nullopt;
    // TRAILING BYTES ARE A REFUSAL, not a shrug. A summary is carried in a
    // section whose size the format writer chose; extra bytes mean the reader
    // and the writer disagree about the layout, and a reader that shrugs would
    // consume a summary it only partly understood.
    if (c.at != bytes.size()) return std::nullopt;
    return s;
}

} // namespace dss::mirsum
