// The `.dsshir` text tier's VOCABULARY and its two capability gaps —
// D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET,
// D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM,
// D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED, and part (d) of
// D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS.
//
// ★★ WHAT EACH ARM ASSERTS, STATED DIRECTLY, BECAUSE THE BYTE COMPARE ALONE
// CANNOT SEE IT. `expectRoundTrip`'s equality is the right pin for a spelling
// that already works; it is the WRONG pin for a value the writer never rendered,
// because "nothing" re-emits as "nothing" and matches itself. Every capability
// arm here therefore also reads the REBUILT POOL or the REBUILT TYPE.
//
// ⚠ AND THE REFUSAL ARMS ASSERT THE ACCEPTED SET IS PRESENT AND CORRECT — not
// merely that the parse failed. A refusal that names no accepted set was the
// defect; a test that only checks `ok == false` would have passed before the fix
// and after it, which is the definition of asserting nothing.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "hir/attributes/diagnostic_info.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_text.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_literal_pool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// Wrap a body line in a minimal well-formed module.
[[nodiscard]] std::string moduleWith(std::string_view bodyLine) {
    return std::string("dsshir 8\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
                       "  function %1 : fn() -> void {\n    block {\n      ")
         + std::string(bodyLine) + "\n      return void\n    }\n  }\n}\n";
}

// Every diagnostic's `actual` text concatenated — what an author would read.
[[nodiscard]] std::string allDiagText(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) { out += d.actual; out += '\n'; }
    return out;
}

struct ParseOutcome {
    bool        ok = false;
    std::string diagnostics;
};

[[nodiscard]] ParseOutcome parseText(std::string const& text) {
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{1}, r);
    return {res->ok, allDiagText(r)};
}

} // namespace

// ── D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET ────────────────────────────────
//
// Five keyword sets, each of which refused an unknown spelling and named no
// accepted set at all. One arm per set, and each arm asserts a SPECIFIC accepted
// spelling appears in the message — a substring the pre-fix message could not
// have contained, because the pre-fix message contained no set.

TEST(HirTextVocabulary, UnknownNodeFlagNamesTheAcceptedSet) {
    ParseOutcome const o = parseText(moduleWith("expr [nosuchflag] lit int 1 : i32"));
    EXPECT_FALSE(o.ok);
    EXPECT_NE(o.diagnostics.find("nosuchflag"), std::string::npos) << o.diagnostics;
    for (std::string_view want : {"'err'", "'syn'", "'shader'", "'host'"}) {
        EXPECT_NE(o.diagnostics.find(want), std::string::npos)
            << "the refusal must name " << want << ":\n" << o.diagnostics;
    }
}

TEST(HirTextVocabulary, UnknownAttributeKindNamesTheAcceptedSet) {
    ParseOutcome const o = parseText(moduleWith("@nosuchattr(1) expr lit int 1 : i32"));
    EXPECT_FALSE(o.ok);
    EXPECT_NE(o.diagnostics.find("nosuchattr"), std::string::npos) << o.diagnostics;
    for (std::string_view want : {"'loc'", "'ffi'", "'shader'", "'transpile'", "'diag'"}) {
        EXPECT_NE(o.diagnostics.find(want), std::string::npos)
            << "the refusal must name " << want << ":\n" << o.diagnostics;
    }
}

TEST(HirTextVocabulary, UnknownForClauseNamesTheAcceptedSet) {
    ParseOutcome const o = parseText(
        moduleWith("for { nosuchclause: expr lit int 1 : i32 body: block { } }"));
    EXPECT_FALSE(o.ok);
    EXPECT_NE(o.diagnostics.find("nosuchclause"), std::string::npos) << o.diagnostics;
    for (std::string_view want : {"'init'", "'cond'", "'update'", "'body'"}) {
        EXPECT_NE(o.diagnostics.find(want), std::string::npos)
            << "the refusal must name " << want << ":\n" << o.diagnostics;
    }
}

TEST(HirTextVocabulary, UnknownOperatorNamesTheAcceptedSet) {
    ParseOutcome const o = parseText(
        moduleWith("expr binop NoSuchOp : i32 (lit int 1 : i32, lit int 2 : i32)"));
    EXPECT_FALSE(o.ok);
    EXPECT_NE(o.diagnostics.find("NoSuchOp"), std::string::npos) << o.diagnostics;
    // Projected off the same `opName` walk the lookup uses. `Add` is a core
    // operator in every build, so its presence is the evidence the set is real
    // rather than a fixed phrase.
    EXPECT_NE(o.diagnostics.find("'Add'"), std::string::npos) << o.diagnostics;
}

// ★ THE ACCEPTED SET AT A NODE POSITION IS THE UNION OF BOTH KEYWORD TABLES, and
// naming only half of it is the same defect one size smaller. `parseNode` routes
// on the EXPRESSION table first and falls through to the statement parser, so a
// keyword that reaches the refusal missed BOTH — and an author who typo'd
// `addressof` must not be handed the statement list alone.
TEST(HirTextVocabulary, UnknownNodeKeywordNamesBothHalvesOfTheAcceptedSet) {
    ParseOutcome const o = parseText(moduleWith("nosuchkeyword"));
    EXPECT_FALSE(o.ok);
    EXPECT_NE(o.diagnostics.find("nosuchkeyword"), std::string::npos) << o.diagnostics;
    for (std::string_view want : {"'block'", "'switch'", "'lit'", "'va_arg'"}) {
        EXPECT_NE(o.diagnostics.find(want), std::string::npos)
            << "the refusal must name " << want << ":\n" << o.diagnostics;
    }
}

// ★★★ THE ROUTER WAS SHORT BY THREE, AND THE THREE IT OMITTED ARE ALL EMITTED.
//
// `isExprKeyword` was a hand-retyped third copy of the expression keyword set,
// carrying twenty of the twenty-three. The missing three — `va_start`, `va_arg`,
// `va_end` — are all written by `emitNodeLine`'s `typedCall`, so a `.dsshir`
// containing one routed to the STATEMENT parser and came back `unknown
// statement`: a write-only spelling produced by a retyped set.
//
// ⚠ THIS ARM DRIVES THE TEXT, NOT THE TABLE. Asserting
// `kHirTextExprKwTable.fromName("va_arg")` would move both halves of the
// comparison together — the table would be both the fix and the expectation, and
// deleting the row would redden nothing that matters. Parsing real text states
// the fact the router is supposed to deliver.
TEST(HirTextVocabulary, VariadicAccessKeywordsRouteToTheExpressionParser) {
    // Child counts are the verifier's, not this reader's — `va_start`/`va_end`
    // take one, `va_arg` takes two — so each line is spelled at its real arity.
    // A wrong arity would fail for a reason that has nothing to do with routing,
    // which would make this pin unable to tell the two apart.
    struct Case { std::string_view kw; std::string_view line; };
    Case const cases[] = {
        {"va_start", "expr va_start : void (lit int 0 : i32)"},
        {"va_arg",   "expr va_arg : i32 (lit int 0 : i32, lit int 1 : i32)"},
        {"va_end",   "expr va_end : void (lit int 0 : i32)"},
        // P69 (D-C-STDARG-VA-COPY-MISSING): the fourth, [dest, src].
        {"va_copy",  "expr va_copy : void (lit int 0 : i32, lit int 1 : i32)"},
    };
    for (Case const& c : cases) {
        ParseOutcome const o = parseText(moduleWith(c.line));
        EXPECT_TRUE(o.ok) << c.kw << " did not parse as an expression:\n" << o.diagnostics;
        EXPECT_EQ(o.diagnostics.find("unknown node keyword"), std::string::npos)
            << c.kw << " was routed to the statement parser:\n" << o.diagnostics;
    }
}

// ── D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED ─────────────────────────────────
//
// ⓘ The row is filed against `src/mir/mir_text.cpp` and the code is in
// `src/hir/hir_text.cpp` — `parseDiag` is a `.dsshir` attribute production and
// has never existed in the MIR tier. The defect is exactly as described; only
// the file in the row's `where` cell was wrong.

TEST(HirTextDiagAttribute, UnallocatedDiagnosticCodeIsRefused) {
    // 0x3FFF sits in the ONE unclaimed family nibble (0x3xxx), so no build
    // defines it and none is planned to without claiming the nibble first.
    ParseOutcome const o = parseText(
        moduleWith("@diag(code 16383) expr lit int 1 : i32"));
    EXPECT_FALSE(o.ok) << "an unallocated ordinal parsed successfully:\n" << o.diagnostics;
    EXPECT_NE(o.diagnostics.find("16383"), std::string::npos) << o.diagnostics;
    EXPECT_NE(o.diagnostics.find("never been allocated"), std::string::npos) << o.diagnostics;
}

TEST(HirTextDiagAttribute, OutOfRangeDiagnosticCodeIsRefused) {
    // Past `DiagnosticCode`'s 16-bit ordinal space entirely. Before the fix the
    // value was TRUNCATED to 16 bits and cast — so `70000` silently became
    // `0x1170`, a different code that may well be allocated.
    ParseOutcome const o = parseText(
        moduleWith("@diag(code 70000) expr lit int 1 : i32"));
    EXPECT_FALSE(o.ok) << o.diagnostics;
    EXPECT_NE(o.diagnostics.find("16-bit"), std::string::npos) << o.diagnostics;
}

TEST(HirTextDiagAttribute, AllocatedDiagnosticCodeStillRoundTrips) {
    // The complement, and it is the arm that keeps the refusal honest: a
    // validation that refuses everything is not a validation. `P_UnexpectedToken`
    // (0x0001) is allocated in every build.
    std::string const text = moduleWith(
        std::string("@diag(code ")
        + std::to_string(static_cast<std::uint32_t>(DiagnosticCode::P_UnexpectedToken))
        + ", recovery none) expr lit int 1 : i32");
    DiagnosticReporter r;
    auto res = parseHir(text, CompilationUnitId{1}, r);
    ASSERT_TRUE(res->ok) << allDiagText(r);
}

// ── D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM ────────────────────────

TEST(HirTextAggregateLiteral, FoldedAggregateRoundTripsWithPerFieldCores) {
    // A D5.3 folded struct constant `{ 7, 2.5 }`. The writer rendered NOTHING for
    // this arm before cycle P23 and a named refusal marker after it; both are
    // states in which the value cannot be read back.
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const f64   = in.primitive(TypeKind::F64);
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const sig   = in.fnSig({}, voidT, CallConv::CcSysV);
    std::vector<TypeId> const fields{i32, f64};
    TypeId const st = in.structType("S", fields);

    HirAggregateValue agg;
    agg.fields.push_back(HirLiteralValue{std::int64_t{7}, TypeKind::I32});
    agg.fields.push_back(HirLiteralValue{double{2.5}, TypeKind::F64});

    HirLiteralPool pool;
    HirBuilder b{"toy"};
    std::uint32_t const idx = pool.add(HirLiteralValue{std::move(agg), TypeKind::Struct});
    HirNodeId const stmts[] = {
        b.makeExprStmt(b.makeLiteral(st, idx)),
        b.makeReturn(),
    };
    HirNodeId const body = b.makeBlock(stmts);
    HirNodeId const fn   = b.makeFunction(sig, 1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;

    DiagnosticReporter r1;
    std::string const first = emitHir(hir, ctx, r1);
    EXPECT_NE(first.find("agg {int 7 : i32, float 2.5 : f64}"), std::string::npos)
        << "the aggregate has no spelling in the emitted text:\n" << first;

    DiagnosticReporter r2;
    auto res = parseHir(first, CompilationUnitId{2}, r2);
    ASSERT_TRUE(res->ok) << first << "\n" << allDiagText(r2);

    // ★ THE POOL, NOT THE BYTES. Byte-identity below would hold even if the
    // reader threw the fields away and the writer re-rendered nothing, so the
    // VALUE is asserted first and directly.
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const* rebuilt = std::get_if<HirAggregateValue>(&res->literalPool.at(0).value);
    ASSERT_NE(rebuilt, nullptr) << "the aggregate arm did not survive the read";
    ASSERT_EQ(rebuilt->fields.size(), 2u);
    EXPECT_EQ(std::get<std::int64_t>(rebuilt->fields[0].value), 7);
    EXPECT_EQ(std::get<double>(rebuilt->fields[1].value), 2.5);
    // The per-field cores: the top-level `literalCoreFor` recomputation cannot
    // reach these, so losing them is invisible to a byte compare.
    EXPECT_EQ(rebuilt->fields[0].core, TypeKind::I32);
    EXPECT_EQ(rebuilt->fields[1].core, TypeKind::F64);

    HirTextContext ctx2;
    ctx2.interner = &res->interner; ctx2.symbolNames = &res->symbolNames;
    ctx2.literalPool = &res->literalPool;
    DiagnosticReporter r3;
    EXPECT_EQ(first, emitHir(res->hir, ctx2, r3));
}

TEST(HirTextAggregateLiteral, NestedAggregateRoundTrips) {
    // The recursion, because one level of nesting is where a hand-rolled
    // field loop and a recursive one stop agreeing.
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const sig   = in.fnSig({}, voidT, CallConv::CcSysV);
    std::vector<TypeId> const innerFields{i32};
    TypeId const inner = in.structType("Inner", innerFields);
    std::vector<TypeId> const outerFields{inner, i32};
    TypeId const outer = in.structType("Outer", outerFields);

    HirAggregateValue innerAgg;
    innerAgg.fields.push_back(HirLiteralValue{std::int64_t{3}, TypeKind::I32});
    HirAggregateValue outerAgg;
    outerAgg.fields.push_back(HirLiteralValue{std::move(innerAgg), TypeKind::Struct});
    outerAgg.fields.push_back(HirLiteralValue{std::int64_t{9}, TypeKind::I32});

    HirLiteralPool pool;
    HirBuilder b{"toy"};
    std::uint32_t const idx = pool.add(HirLiteralValue{std::move(outerAgg), TypeKind::Struct});
    HirNodeId const stmts[] = { b.makeExprStmt(b.makeLiteral(outer, idx)), b.makeReturn() };
    HirNodeId const body = b.makeBlock(stmts);
    HirNodeId const fn   = b.makeFunction(sig, 1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fn});
    Hir hir = std::move(b).finish(root);

    std::vector<std::string> names{"", "main"};
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;
    DiagnosticReporter r1;
    std::string const first = emitHir(hir, ctx, r1);
    DiagnosticReporter r2;
    auto res = parseHir(first, CompilationUnitId{2}, r2);
    ASSERT_TRUE(res->ok) << first << "\n" << allDiagText(r2);
    ASSERT_EQ(res->literalPool.size(), 1u);
    auto const* out = std::get_if<HirAggregateValue>(&res->literalPool.at(0).value);
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(out->fields.size(), 2u);
    auto const* nested = std::get_if<HirAggregateValue>(&out->fields[0].value);
    ASSERT_NE(nested, nullptr) << "the nested aggregate did not survive";
    ASSERT_EQ(nested->fields.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(nested->fields[0].value), 3);
    EXPECT_EQ(std::get<std::int64_t>(out->fields[1].value), 9);
}

TEST(HirTextAggregateLiteral, UnspelledMarkerIsStillRefusedByName) {
    // The writer's marker no longer means "aggregate" — it means the writer met a
    // `HirLiteralValue` arm it does not render. The reader must still refuse it by
    // name and say the value was not serialized, so an author is never sent
    // hunting for a typo.
    ParseOutcome const o = parseText(
        moduleWith("expr lit unspelled_aggregate : i32"));
    EXPECT_FALSE(o.ok);
    EXPECT_NE(o.diagnostics.find("was NOT serialized"), std::string::npos) << o.diagnostics;
}

// ── P69 (lane `cs`): `.dsshir` v8 — A UNION VALUE NAMES ITS MEMBER ───────────
//
// `lit agg member N {…} : <union>` — the member a union value's one field
// initializes. An initializer may designate any member (C 6.7.9p17) and the
// field's type cannot say which: both members below are `i32`, so ONLY the index
// tells the second from the first. v7 wrote `agg {…}` for every aggregate, so a
// module built in memory lost the member on write without a word and no spelling
// could put it back — while `toMirLiteral` copies the member into the MIR literal
// and the static-data encoder refuses a union value that names none.
//
// ★ THREE THINGS ARE ASSERTED, AND THE BYTE COMPARE IS THE WEAKEST OF THEM.
// "Nothing" re-emits as "nothing": a writer that drops the member and a reader
// that never asks for it round-trip byte for byte. So the SPELLING is asserted,
// then the REBUILT POOL, then the value the NEXT TIER receives.
//
// RED-ON-DISABLE: stop writing `member N` → the spelling assertion fails and the
// reader refuses the member-less union; stop storing it on read → the reader
// refuses its own writer's output; drop either refusal → the matching row of
// `AUnionMemberSpellingIsReadOnlyWhereItMeansSomething` reads clean.
namespace {

struct UnionModule {
    TypeInterner             in{CompilationUnitId{1}};
    HirLiteralPool           pool;
    std::vector<std::string> names{"", "u", "s"};
    Hir                      hir{};
};

// `%1`: `union U { i32; i32; }` through its SECOND member. `%2`: the same union
// NESTED in a structure, through its FIRST member — named too, because absence
// is not "member 0", and the nesting is what the writer's and the reader's frame
// stacks walk.
[[nodiscard]] std::unique_ptr<UnionModule> unionModule() {
    auto m = std::make_unique<UnionModule>();
    TypeId const i32 = m->in.primitive(TypeKind::I32);
    std::vector<TypeId> const twoInts{i32, i32};
    TypeId const u = m->in.unionType("U", twoInts);
    std::vector<TypeId> const sFields{i32, u};
    TypeId const s = m->in.structType("S", sFields);

    auto const unionOf = [](std::int64_t v, std::uint32_t member) {
        HirAggregateValue a;
        a.fields.push_back(HirLiteralValue{v, TypeKind::I32});
        a.unionMember = member;
        return HirLiteralValue{std::move(a), TypeKind::Union};
    };
    HirBuilder b{"toy"};
    HirNodeId const gu = b.makeGlobal(u, 1, b.makeLiteral(u, m->pool.add(unionOf(7, 1))));
    HirAggregateValue sa;
    sa.fields.push_back(HirLiteralValue{std::int64_t{3}, TypeKind::I32});
    sa.fields.push_back(unionOf(9, 0));
    HirNodeId const gs = b.makeGlobal(
        s, 2, b.makeLiteral(s, m->pool.add(HirLiteralValue{std::move(sa), TypeKind::Struct})));
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{gu, gs});
    m->hir = std::move(b).finish(root);
    return m;
}

[[nodiscard]] std::string emitUnionModule(UnionModule const& m) {
    HirTextContext ctx;
    ctx.interner = &m.in; ctx.symbolNames = &m.names; ctx.literalPool = &m.pool;
    DiagnosticReporter w;
    std::string text = emitHir(m.hir, ctx, w);
    EXPECT_TRUE(allDiagText(w).empty()) << allDiagText(w) << "\n" << text;
    return text;
}

// Every UNION level of a pool's values, in walk order: the member it names, or
// `nullopt` where it names none.
[[nodiscard]] std::vector<std::optional<std::uint32_t>>
unionMembersOf(HirLiteralPool const& pool) {
    std::vector<std::optional<std::uint32_t>> out;
    for (std::uint32_t i = 0; i < pool.size(); ++i) {
        std::vector<HirLiteralValue const*> work{&pool.at(i)};
        while (!work.empty()) {
            HirLiteralValue const* const v = work.back();
            work.pop_back();
            auto const* a = std::get_if<HirAggregateValue>(&v->value);
            if (a == nullptr) continue;
            if (v->core == TypeKind::Union) out.push_back(a->unionMember);
            for (std::size_t f = a->fields.size(); f-- > 0;) work.push_back(&a->fields[f]);
        }
    }
    return out;
}

// `text` with its ONE occurrence of `from` replaced by `to` — a text the writer
// produced, edited at exactly one place, so every refusal row below is about one
// spelling and nothing else. A `from` that is absent or repeated fails the row.
[[nodiscard]] std::string withOneEdit(std::string text, std::string_view from,
                                      std::string_view to) {
    std::size_t const at = text.find(from);
    EXPECT_NE(at, std::string::npos) << "'" << from << "' is not in the text:\n" << text;
    if (at == std::string::npos) return text;
    EXPECT_EQ(text.find(from, at + 1), std::string::npos)
        << "'" << from << "' occurs more than once:\n" << text;
    text.replace(at, from.size(), to);
    return text;
}

} // namespace

TEST(HirTextAggregateLiteral, AUnionValueNamesItsMemberAndReadsBackWithIt) {
    auto const m = unionModule();
    std::string const first = emitUnionModule(*m);
    EXPECT_NE(first.find("lit agg member 1 {int 7 : i32} : "), std::string::npos)
        << "the SECOND member is not named:\n" << first;
    EXPECT_NE(first.find("agg member 0 {int 9 : i32} : union"), std::string::npos)
        << "the first member is NAMED too — absence is not member 0:\n" << first;

    DiagnosticReporter r;
    auto const back = parseHir(first, CompilationUnitId{2}, r);
    ASSERT_TRUE(back->ok) << allDiagText(r) << "\n" << first;
    // ★ THE POOL, NOT THE BYTES.
    EXPECT_EQ(unionMembersOf(back->literalPool),
              (std::vector<std::optional<std::uint32_t>>{1u, 0u}))
        << "the value read back without the member its text names\n" << first;

    HirTextContext ctx2;
    ctx2.interner = &back->interner; ctx2.symbolNames = &back->symbolNames;
    ctx2.literalPool = &back->literalPool;
    DiagnosticReporter w2;
    EXPECT_EQ(emitHir(back->hir, ctx2, w2), first)
        << "write → read → write is not the identity";

    // ★ AND THE NEXT TIER RECEIVES IT: the MIR literal `toMirLiteral` builds from
    // the value that was READ is the one the static-data encoder walks against the
    // union's type. A member lost anywhere on the way is a value with no encoding.
    DiagnosticReporter mr;
    HirToMirResult const mir = lowerToMir(back->hir, back->literalPool, back->interner, mr);
    ASSERT_TRUE(mir.ok) << allDiagText(mr);
    std::vector<std::optional<std::uint32_t>> lowered;
    for (std::uint32_t i = 0; i < mir.mir.moduleGlobalCount(); ++i) {
        std::uint32_t const lit = mir.mir.globalInitLiteralIndex(mir.mir.globalAt(i));
        ASSERT_NE(lit, UINT32_MAX) << "global " << i << " did not fold to static data";
        forEachLiteralNode(mir.mir.literalValue(lit), [&](MirLiteralValue const& n) {
            if (auto const* a = std::get_if<MirAggregateValue>(&n.value);
                a != nullptr && n.core == TypeKind::Union) {
                lowered.push_back(a->unionMember);
            }
        });
    }
    EXPECT_EQ(lowered, (std::vector<std::optional<std::uint32_t>>{1u, 0u}))
        << "the MIR literal the static-data encoder reads does not name the member";
}

// What the v8 reader refuses, each AT THE TEXT and by name — `H_TextMalformed`,
// in words about the input. The alternative for the two member-less rows is the
// static-data encoder's refusal of an upstream DEFECT in DSS several tiers later,
// which a malformed `.dsshir` is not. Every row is the writer's own output with
// ONE spelling edited; the unedited text is the control, and an EMPTY union value
// (which initializes no member) is the second one.
TEST(HirTextAggregateLiteral, AUnionMemberSpellingIsReadOnlyWhereItMeansSomething) {
    auto const m = unionModule();
    std::string const good = emitUnionModule(*m);
    {
        DiagnosticReporter r;
        ASSERT_TRUE(parseHir(good, CompilationUnitId{3}, r)->ok)
            << "CONTROL: the unedited text must read\n" << allDiagText(r) << "\n" << good;
    }
    struct Case {
        char const*      name;
        std::string_view from;
        std::string_view to;
        std::string_view needle;
    };
    for (Case const& c : {
             Case{"a top-level union value with a field and no member",
                  "lit agg member 1 {int 7 : i32}", "lit agg {int 7 : i32}",
                  "must name the member it initializes"},
             Case{"a NESTED union value with a field and no member",
                  "agg member 0 {int 9 : i32} : union", "agg {int 9 : i32} : union",
                  "must name the member it initializes"},
             Case{"`member` on a top-level value that is a structure's",
                  "lit agg {int 3 : i32", "lit agg member 0 {int 3 : i32",
                  "only a union value names its member"},
             Case{"`member` on a NESTED value whose core is not `union`",
                  "agg member 0 {int 9 : i32} : union", "agg member 0 {int 9 : i32} : struct",
                  "only a union value names its member"},
             Case{"`member` with a word where the index belongs",
                  "lit agg member 1 {int 7 : i32}", "lit agg member x {int 7 : i32}",
                  "expected a member index after `agg member`, got 'x'"},
             Case{"`member` with no index at all",
                  "lit agg member 1 {int 7 : i32}", "lit agg member {int 7 : i32}",
                  "expected a member index after `agg member`"},
             Case{"a member index past its 32-bit field",
                  "lit agg member 1 {int 7 : i32}", "lit agg member 4294967297 {int 7 : i32}",
                  "union member index 4294967297 does not fit its 32-bit field"},
         }) {
        std::string const text = withOneEdit(good, c.from, c.to);
        DiagnosticReporter r;
        auto const parsed = parseHir(text, CompilationUnitId{4}, r);
        EXPECT_FALSE(parsed->ok) << c.name << "\n" << text;
        bool named = false;
        for (auto const& d : r.all()) {
            named = named || (d.code == DiagnosticCode::H_TextMalformed
                              && d.actual.find(c.needle) != std::string::npos);
        }
        EXPECT_TRUE(named) << c.name << ": the refusal must say why, at the text\n"
                           << allDiagText(r);
    }
    // The second control: a union value with NO field initializes no member and
    // names none — it is the value's zero image, and it reads.
    {
        std::string const empty =
            withOneEdit(good, "lit agg member 1 {int 7 : i32}", "lit agg {}");
        DiagnosticReporter r;
        auto const parsed = parseHir(empty, CompilationUnitId{5}, r);
        ASSERT_TRUE(parsed->ok) << allDiagText(r) << "\n" << empty;
        EXPECT_EQ(unionMembersOf(parsed->literalPool),
                  (std::vector<std::optional<std::uint32_t>>{std::nullopt, 0u}));
    }
}

// ── part (d): `_BitInt(N)` was WRITE-ONLY in the type grammar ────────────────

TEST(HirTextBitIntType, BitIntTypeRoundTripsInBothSignednesses) {
    // ⚠ THIS PRODUCTION IS SHIPPED, not a debug surface. `emitHir`/`parseHir`
    // have no callers in `src/` outside their own TU, but `parseTypeFromText`
    // drives this same `parseType` — ✔MEASURED 2026-08-23: 9 call sites, 7 in
    // `ffi/shipped_lib_descriptor.cpp` and 2 in `analysis/semantic/
    // semantic_analyzer.cpp` — so before the reader arm existed, no shipped FFI
    // descriptor could name a bit-precise type at all.
    TypeInterner in{CompilationUnitId{1}};
    TypeRegistry reg;
    DiagnosticReporter r;

    TypeId const signed37 = parseTypeFromText("_BitInt(37)", in, reg, r, {});
    ASSERT_TRUE(signed37.valid()) << allDiagText(r);
    EXPECT_EQ(in.kind(signed37), TypeKind::BitInt);
    EXPECT_EQ(in.bitIntWidth(signed37), 37);
    EXPECT_TRUE(in.bitIntIsSigned(signed37));

    TypeId const unsigned128 = parseTypeFromText("unsigned _BitInt(128)", in, reg, r, {});
    ASSERT_TRUE(unsigned128.valid()) << allDiagText(r);
    EXPECT_EQ(in.bitIntWidth(unsigned128), 128);
    EXPECT_FALSE(in.bitIntIsSigned(unsigned128));

    // And the identity that makes it a ROUND TRIP rather than two independent
    // decoders: the writer's spelling is what the reader just accepted.
    EXPECT_EQ(parseTypeFromText("_BitInt(37)", in, reg, r, {}), signed37);
}

TEST(HirTextBitIntType, BareUnsignedIsRefusedWithAReason) {
    // `unsigned` is not a type on its own in this format — an unsigned primitive
    // is spelled by its own name. A reader that accepted a bare `unsigned` would
    // be inventing a width.
    TypeInterner in{CompilationUnitId{1}};
    TypeRegistry reg;
    DiagnosticReporter r;
    EXPECT_FALSE(parseTypeFromText("unsigned", in, reg, r, {}).valid());
    EXPECT_NE(allDiagText(r).find("_BitInt"), std::string::npos) << allDiagText(r);
}

// ── D-HIR-TEXT-WRITER-SPELLS-KEYWORDS-THE-READER-HAS-NO-ROW-FOR ─────────────
//
// ★★★ THE SAME CLASS AS THE `va_*` ARM ABOVE, TWO MORE SPELLINGS, AND A THIRD
// COPY OF THE SET THAT NOBODY HAD LOOKED AT.
// ✔MEASURED 2026-08-23: `emitExpr` wrote `builtincall` and `labeladdr`, and
// NEITHER string appeared anywhere else in `hir_text.cpp` — no table row, no
// dispatch arm, so `emitHir` produced text `parseHir` refused BY NAME. The
// `va_*` repair the same day added three rows and left the mechanism intact,
// which is why the class re-opened one arm over.
//
// ⚠ AND IT HAD ALREADY RE-OPENED IN A THIRD PLACE the repair did not reach: the
// WRITER's router (`isExprKind`) is the mirror of the reader's `isExprKeyword`,
// and it listed neither `LabelAddressOf` nor the three `va_*` kinds the reader
// had just been taught. So a `.dsshir` could not carry these nodes in EITHER
// direction, for two independent reasons, and each half hid the other.
//
// ⓘ These arms drive real TEXT rather than the tables, for the reason the `va_*`
// arm states: a table-vs-table assertion moves both halves together.

TEST(HirTextVocabulary, BuiltinCallKeywordRoundTripsThroughTheReader) {
    // `#1` is BuiltinLowering::UMulHigh — the first ALLOCATED row (0 is the
    // `None` sentinel, deliberately unlisted in the table).
    ParseOutcome const o = parseText(moduleWith(
        "expr builtincall #1 : u64 (lit uint 3 : u64, lit uint 5 : u64)"));
    EXPECT_TRUE(o.ok) << "builtincall did not parse:\n" << o.diagnostics;
    EXPECT_EQ(o.diagnostics.find("unknown node keyword"), std::string::npos)
        << o.diagnostics;
    EXPECT_EQ(o.diagnostics.find("unknown expression"), std::string::npos)
        << o.diagnostics;
}

// ★ THE PAYLOAD IS VALIDATED, NOT CAST — the same argument this file's
// `UnallocatedDiagnosticCodeIsRefused` makes about `@diag(code N)`, applied to
// the other unvalidated ordinal in the format. `hir_to_mir` maps this payload
// straight onto a `MirOpcode`, so an ordinal outside the closed set is not a
// lowering at all.
TEST(HirTextVocabulary, UnallocatedBuiltinCallLoweringIsRefused) {
    ParseOutcome const o = parseText(moduleWith(
        "expr builtincall #60000 : u64 (lit uint 3 : u64)"));
    EXPECT_FALSE(o.ok) << o.diagnostics;
    EXPECT_NE(o.diagnostics.find("names no BuiltinLowering"), std::string::npos)
        << o.diagnostics;
    // The accepted set is present, per this file's standing rule that a refusal
    // naming no set is the defect rather than the fix.
    EXPECT_NE(o.diagnostics.find("'umulh'"), std::string::npos) << o.diagnostics;
}

// `None` (0) renders EMPTY in `kBuiltinLoweringTable` because it is the "no
// lowering" sentinel. A `BuiltinCall` node exists precisely because the builtin
// HAS a lowering, so 0 must be refused rather than read back as a plausible one.
TEST(HirTextVocabulary, BuiltinCallLoweringSentinelZeroIsRefused) {
    ParseOutcome const o = parseText(moduleWith(
        "expr builtincall #0 : u64 (lit uint 3 : u64)"));
    EXPECT_FALSE(o.ok) << o.diagnostics;
    EXPECT_NE(o.diagnostics.find("names no BuiltinLowering"), std::string::npos)
        << o.diagnostics;
}

// `&&label` — a LEAF expression whose payload is the label ordinal, in the same
// per-function namespace `goto`/`label` use. The label it names really exists
// here, because the verifier resolves it and a dangling ordinal would fail for a
// reason that has nothing to do with the keyword being readable.
TEST(HirTextVocabulary, LabelAddressKeywordRoundTripsThroughTheReader) {
    std::string const text =
        "dsshir 8\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      label L1:\n        return void\n"
        "      expr labeladdr L1 : ptr<void>\n"
        "      return void\n    }\n  }\n}\n";
    ParseOutcome const o = parseText(text);
    EXPECT_TRUE(o.ok) << "labeladdr did not parse:\n" << o.diagnostics;
    EXPECT_EQ(o.diagnostics.find("unknown node keyword"), std::string::npos)
        << o.diagnostics;
}

// ★★ THE ROUTER HALF, WHICH IS A DIFFERENT FACT FROM THE READER HALF. The two
// arms above prove `parseHir` accepts the spellings; this one proves the node
// reaches the EXPRESSION writer at all. `emitNodeLine` asks `isExprKind` first
// and hands anything it disowns to the statement writer, which degrades the node
// to `error` — a byte-identical round trip of a SMALLER program, invisible to
// every equality check in the suite.
//
// ⓘ Driven through parse→emit→parse rather than by calling the router, which is
// file-private: the observable consequence is that the re-emitted text still
// contains the keyword.
//
// ⚠⚠ THE STATEMENT BEFORE THE SUBJECT LINE IS `unreachable`, AND THAT IS THE
// WHOLE PIN. ✔MEASURED 2026-08-23: the first draft used `return` there, and this
// format's lexer is NEWLINE-BLIND — so `return` swallowed the next line as its
// VALUE and the subject node became a `ReturnStmt` CHILD, which renders through
// `emitExpr` directly and never asks the router anything. The arm then PASSED
// against a mutant carrying the exact pre-fix router, which is the definition of
// asserting nothing. `unreachable` takes no operand, so it cannot absorb the
// line, and the subject stays a direct child of the block — the only position
// where `emitNodeLine` consults `isExprKind` at all.
TEST(HirTextVocabulary, ExpressionNodesInStatementPositionAreNotDegradedToError) {
    struct Case { std::string_view kw; std::string_view line; };
    Case const cases[] = {
        {"va_arg",    "va_arg : i32 (lit int 0 : i32, lit int 1 : i32)"},
        {"va_start",  "va_start : void (lit int 0 : i32)"},
        {"va_end",    "va_end : void (lit int 0 : i32)"},
        {"va_copy",   "va_copy : void (lit int 0 : i32, lit int 1 : i32)"},
        {"labeladdr", "labeladdr L1 : ptr<void>"},
    };
    for (Case const& c : cases) {
        std::string const text =
            "dsshir 8\nproducer \"\"\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
            "  function %1 : fn() -> void {\n    block {\n"
            "      label L1:\n        unreachable\n      "
            + std::string(c.line) + "\n      return void\n    }\n  }\n}\n";
        DiagnosticReporter r1;
        auto res = parseHir(text, CompilationUnitId{1}, r1);
        ASSERT_TRUE(res->ok) << c.kw << ":\n" << allDiagText(r1);

        HirTextContext ctx;
        ctx.interner    = &res->interner;
        ctx.symbolNames = &res->symbolNames;
        ctx.literalPool = &res->literalPool;
        DiagnosticReporter r2;
        std::string const out = emitHir(res->hir, ctx, r2);
        EXPECT_NE(out.find(c.kw), std::string::npos)
            << c.kw << " was written in statement position and came back as "
               "something else:\n" << out;
        EXPECT_EQ(out.find("unexpected node kind"), std::string::npos) << out;
        EXPECT_EQ(allDiagText(r2).find("unexpected node kind"), std::string::npos)
            << allDiagText(r2);
    }
}
