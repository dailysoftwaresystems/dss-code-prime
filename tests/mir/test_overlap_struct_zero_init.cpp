// D-MIR-OVERLAP-STRUCT-ZERO-INIT — brace-initialization of a struct whose members
// SHARE BYTES (a c107 / D-FFI-DESCRIPTOR-UNION-OVERLAY explicit-offset overlay).
//
// The rule under test, in full:
//   * members OVERLAP + EVERY supplied element is zero  → ACCEPTED, lowered as ONE
//     whole-object zero-fill of the struct's FULL size (never member-wise: a
//     positional write order is meaningless when the writes alias each other).
//   * members OVERLAP + ANY supplied element is non-zero → REFUSED, LOUD, with the
//     pre-existing `H_UnsupportedLoweringForKind` text (that case genuinely IS
//     ambiguous — a later field would silently clobber an earlier one).
//   * members DISJOINT (explicit offsets that simply are not natural) → member-wise,
//     one Store per field at its declared offset.
//
// Every case is built as HAND-CONSTRUCTED HIR, because an explicit-offset struct
// cannot be SPELLED in C source — the offsets arrive only from a shipped-library
// descriptor (`windows.json`'s `ULARGE_INTEGER`, `sys/stat.json`'s macho `struct
// stat`). The FRONT-END half (that `{0}` and `{}` both reach MIR as an all-zero
// aggregate, and that the result is right at RUNTIME) is pinned by the corpus
// example `examples/c/overlap_struct_zero_init/`, which compiles + RUNS the
// real `ULARGE_INTEGER` on a pe64 target.
//
// ASSERTION STRENGTH: the zero-fill cases decode every emitted Store back to
// `Store(Const 0, Gep(alloca, Const off))`, reconstruct the exact SET OF BYTES
// written from each chunk's access width, and require it to equal EXACTLY
// `[0, layout.size)` — so a fill that is short, long, misaligned, or non-zero is a
// failure, not merely "something was emitted".

#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_node.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

// The shipped-target layout params (natural alignment, 16-byte ISA cap), LP64 —
// byte-identical to `tests/core/test_type_layout.cpp`'s `kNatural16`.
constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

[[nodiscard]] TypeInterner makeInterner() { return TypeInterner{CompilationUnitId{1}}; }

[[nodiscard]] MirLoweringConfig makeConfig() {
    MirLoweringConfig cfg;
    cfg.aggregateLayout       = kNatural16;
    cfg.aggregateLayoutLoaded = true;
    cfg.dataModel             = DataModel::Lp64;
    return cfg;
}

// ── the two structs under test ──────────────────────────────────────────────
//
// `overlapStruct` deliberately spans ALL THREE chunk widths the whole-object
// zero-fill walks (I64 → I32 → Char): `{u8@0, u8@1, arr<u8,13>@0}` lays out to
// size 13 / align 1, and its third member SWALLOWS the first two — the same
// "one member is another member's alias" shape `sys/stat.json`'s macho
// `st_mtimespec@48` has over `st_mtim_sec@48` + `st_mtim_nsec@56`. 13 bytes =
// one I64 + one I32 + one Char, so a fill that only handles the wide chunk, or
// that stops at the first field's width, cannot pass.
[[nodiscard]] TypeId overlapStruct(TypeInterner& ti) {
    TypeId const u8  = ti.primitive(TypeKind::U8);
    TypeId const a13 = ti.array(u8, 13);
    std::array<TypeId, 3>        const fields{u8, u8, a13};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 3> const offsets{0, 1, 0};
    return ti.structType("Overlay13", fields, noWidths, offsets);
}

// The real `windows.json` shape: `ULARGE_INTEGER {QuadPart u64@0, LowPart u32@0,
// HighPart u32@4}` → size 8, align 8. Both `LowPart` and `HighPart` alias
// `QuadPart`'s bytes.
[[nodiscard]] TypeId ulargeStruct(TypeInterner& ti) {
    TypeId const u64 = ti.primitive(TypeKind::U64);
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 3>        const fields{u64, u32, u32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 3> const offsets{0, 0, 4};
    return ti.structType("ULARGE_INTEGER", fields, noWidths, offsets);
}

// Explicit offsets that are DISJOINT — a foreign layout that simply is not the
// natural one (`{u32@0, u32@8}` → size 12, align 4). Nothing about it is
// ambiguous, so a brace initializer must lower MEMBER-WISE.
[[nodiscard]] TypeId disjointOffsetStruct(TypeInterner& ti) {
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 2>        const fields{u32, u32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 2> const offsets{0, 8};
    return ti.structType("Disjoint", fields, noWidths, offsets);
}

// ── HIR construction ────────────────────────────────────────────────────────

struct Built {
    Hir            hir;
    HirLiteralPool literals;
};

// `int f(void) { <struct> s = { <elements> }; return 0; }` with the initializer's
// element VALUES supplied by the caller. `elemValues[i]` is field i's integer
// value; a field whose type is an AGGREGATE gets a nested all-that-value
// ConstructAggregate (matching what `lowerBraceInit`'s zero-fill produces).
// `synthetic` marks EVERY child `HirFlags::Synthetic` — the shape CST→HIR emits
// for `{}` (C23 6.7.10p11), where no element came from source text; the default
// leaves child 0 plain, the shape `{0}` emits.
[[nodiscard]] Built buildVarDeclInit(TypeInterner& ti, TypeId structTy,
                                     std::span<std::int64_t const> elemValues,
                                     bool synthetic = false) {
    Built        out;
    HirBuilder   b{"c"};
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirFlags const cf = synthetic ? HirFlags::Synthetic : HirFlags::None;
    // A leaf zero/value literal of `ty`; an aggregate `ty` recurses so a nested
    // struct/array member carries its own ConstructAggregate (as the zero-fill does).
    auto makeValueOfType = [&](auto&& self, TypeId ty, std::int64_t v) -> HirNodeId {
        TypeKind const k = ti.kind(ty);
        if (k == TypeKind::Array) {
            auto const ops   = ti.operands(ty);
            auto const scals = ti.scalars(ty);
            std::vector<HirNodeId> kids;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(scals[0]); ++i)
                kids.push_back(self(self, ops[0], v));
            return b.makeConstructAggregate(kids, ty, cf);
        }
        if (k == TypeKind::Struct || k == TypeKind::Union) {
            std::vector<HirNodeId> kids;
            for (TypeId ft : ti.operands(ty)) kids.push_back(self(self, ft, v));
            return b.makeConstructAggregate(kids, ty, cf);
        }
        HirLiteralValue lit;
        lit.core  = k;
        lit.value = v;
        return b.makeLiteral(ty, out.literals.add(lit), cf);
    };

    auto const fieldTypes = ti.operands(structTy);
    std::vector<HirNodeId> children;
    for (std::size_t i = 0; i < fieldTypes.size(); ++i)
        children.push_back(makeValueOfType(
            makeValueOfType, fieldTypes[i],
            i < elemValues.size() ? elemValues[i] : std::int64_t{0}));

    HirNodeId const agg  = b.makeConstructAggregate(children, structTy,
                                                    HirFlags::Synthetic);
    HirNodeId const decl = b.makeVarDecl(structTy, /*symbol=*/1, agg);
    HirLiteralValue zero;
    zero.core  = TypeKind::I32;
    zero.value = std::int64_t{0};
    HirNodeId const ret  = b.makeReturn(b.makeLiteral(i32, out.literals.add(zero)));
    HirNodeId const body = b.makeBlock(std::array{decl, ret});
    HirNodeId const fn   = b.makeFunction(ti.fnSig({}, i32, CallConv::CcSysV),
                                          /*symbol=*/2, {}, body);
    out.hir = std::move(b).finish(b.makeModule(std::array{fn}));
    return out;
}

// A float-typed overlay whose FIRST element is `-0.0` — numerically zero but with
// its sign bit SET, so its object representation is NOT all-zero bytes. Kept as a
// dedicated builder because the integer builder cannot express it.
[[nodiscard]] Built buildMinusZeroInit(TypeInterner& ti, TypeId structTy,
                                       double firstValue) {
    Built        out;
    HirBuilder   b{"c"};
    TypeId const i32 = ti.primitive(TypeKind::I32);

    auto const fieldTypes = ti.operands(structTy);
    std::vector<HirNodeId> children;
    for (std::size_t i = 0; i < fieldTypes.size(); ++i) {
        HirLiteralValue lit;
        lit.core  = ti.kind(fieldTypes[i]);
        lit.value = (i == 0) ? firstValue : 0.0;
        children.push_back(b.makeLiteral(fieldTypes[i], out.literals.add(lit)));
    }
    HirNodeId const agg  = b.makeConstructAggregate(children, structTy,
                                                    HirFlags::Synthetic);
    HirNodeId const decl = b.makeVarDecl(structTy, /*symbol=*/1, agg);
    HirLiteralValue zero;
    zero.core  = TypeKind::I32;
    zero.value = std::int64_t{0};
    HirNodeId const ret  = b.makeReturn(b.makeLiteral(i32, out.literals.add(zero)));
    HirNodeId const body = b.makeBlock(std::array{decl, ret});
    HirNodeId const fn   = b.makeFunction(ti.fnSig({}, i32, CallConv::CcSysV),
                                          /*symbol=*/2, {}, body);
    out.hir = std::move(b).finish(b.makeModule(std::array{fn}));
    return out;
}

// `struct { f64@0, f64@0 }` — two doubles fully aliased, so the overlap gate fires
// and the initializer's VALUE decides accept-vs-refuse.
[[nodiscard]] TypeId doubleOverlayStruct(TypeInterner& ti) {
    TypeId const f64 = ti.primitive(TypeKind::F64);
    std::array<TypeId, 2>        const fields{f64, f64};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 2> const offsets{0, 0};
    return ti.structType("DblOverlay", fields, noWidths, offsets);
}

// ── MIR inspection ──────────────────────────────────────────────────────────

// One decoded `Store(value, Gep(base, Const off))` from the lowered function.
struct DecodedStore {
    std::uint64_t offset    = 0;
    std::uint64_t width     = 0;    // bytes written, from the Gep's pointee type
    bool          valueIsZeroConst = false;
};

// The byte width a Store of `chunkTy` actually touches — the SAME width-exact rule
// `D-LIR-INT-MEMORY-WIDTH-EXACT` gives the emitted access. Derived from the type,
// so this helper cannot drift from the emitter by hard-coding 8.
[[nodiscard]] std::uint64_t storeWidthOf(TypeInterner const& ti, TypeId ptrTy) {
    auto const ops = ti.operands(ptrTy);
    if (ops.empty()) return 0;
    auto const l = computeLayout(ops[0], ti, kNatural16, DataModel::Lp64);
    return l ? l->size : 0;
}

// The `std::int64_t` value behind a MIR `Const`, or nullopt when the instruction
// is not a Const / its literal arm is not an integer.
[[nodiscard]] std::optional<std::int64_t> constIntOf(Mir const& m, MirInstId id) {
    if (m.instOpcode(id) != MirOpcode::Const) return std::nullopt;
    auto const& lit = m.literalValue(m.constLiteralIndex(id));
    if (auto const* i = std::get_if<std::int64_t>(&lit.value)) return *i;
    if (auto const* u = std::get_if<std::uint64_t>(&lit.value))
        return static_cast<std::int64_t>(*u);
    return std::nullopt;
}

// Decode every Store in the module's single function whose destination is a
// `Gep(alloca, Const)`. Returns them in emission order.
[[nodiscard]] std::vector<DecodedStore>
decodeStores(Mir const& m, TypeInterner const& ti) {
    std::vector<DecodedStore> out;
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        MirFuncId const f = m.funcAt(fi);
        for (std::uint32_t bi = 0; bi < m.funcBlockCount(f); ++bi) {
            MirBlockId const bb = m.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < m.blockInstCount(bb); ++ii) {
                MirInstId const inst = m.blockInstAt(bb, ii);
                if (m.instOpcode(inst) != MirOpcode::Store) continue;
                auto const ops = m.instOperands(inst);
                if (ops.size() != 2) continue;
                MirInstId const value = ops[0];
                MirInstId const dest  = ops[1];
                if (m.instOpcode(dest) != MirOpcode::Gep) continue;
                auto const gops = m.instOperands(dest);
                if (gops.size() != 2) continue;
                if (m.instOpcode(gops[0]) != MirOpcode::Alloca) continue;
                auto const off = constIntOf(m, gops[1]);
                if (!off.has_value()) continue;
                DecodedStore d;
                d.offset = static_cast<std::uint64_t>(*off);
                d.width  = storeWidthOf(ti, m.instType(dest));
                auto const v = constIntOf(m, value);
                d.valueIsZeroConst = v.has_value() && *v == 0;
                out.push_back(d);
            }
        }
    }
    return out;
}

[[nodiscard]] bool sawUnsupported(DiagnosticReporter const& r,
                                  std::string_view mustContain) {
    for (auto const& d : r.all())
        if (d.code == DiagnosticCode::H_UnsupportedLoweringForKind
            && d.actual.find(mustContain) != std::string::npos)
            return true;
    return false;
}

// Lower a built module and hand back everything the assertions need.
struct Lowered {
    DiagnosticReporter        reporter;
    HirToMirResult            result;
    std::vector<DecodedStore> stores;
};

void lowerInto(Lowered& out, Built& built, TypeInterner& ti,
               HirSourceMap const* sourceMap = nullptr) {
    out.result = lowerToMir(built.hir, built.literals, ti, out.reporter,
                            sourceMap, makeConfig());
    out.stores = decodeStores(out.result.mir, ti);
}

} // namespace

// ── 1 · `{0}` into an overlapping struct is ACCEPTED as a whole-object zero-fill ─

TEST(OverlapStructZeroInit, AllZeroInitZeroesTheFullStructSize) {
    TypeInterner ti = makeInterner();
    TypeId const s  = overlapStruct(ti);
    ASSERT_TRUE(compositeFieldsOverlap(s, ti, kNatural16, DataModel::Lp64))
        << "fixture precondition: this struct's members must actually share bytes";
    auto const lay = computeLayout(s, ti, kNatural16, DataModel::Lp64);
    ASSERT_TRUE(lay.has_value());
    ASSERT_EQ(lay->size, 13u) << "fixture precondition: 13 bytes = I64 + I32 + Char";

    std::array<std::int64_t, 3> const zeros{0, 0, 0};
    Built built = buildVarDeclInit(ti, s, zeros);
    Lowered L;
    lowerInto(L, built, ti);

    ASSERT_TRUE(L.result.ok)
        << "an ALL-ZERO brace initializer of an overlapping struct must lower: "
           "zeroed bytes read the same through every aliasing member "
        << (L.reporter.all().empty() ? std::string{} : L.reporter.all()[0].actual);
    EXPECT_FALSE(sawUnsupported(L.reporter, "overlapping"))
        << "the overlap refusal must NOT fire on an all-zero initializer";

    // Every write must be a CONSTANT ZERO...
    ASSERT_FALSE(L.stores.empty()) << "no Store was emitted at all";
    for (DecodedStore const& d : L.stores)
        EXPECT_TRUE(d.valueIsZeroConst)
            << "store at offset " << d.offset << " is not a constant 0";

    // ...and together they must cover EXACTLY [0, size) — no short fill (which
    // would leave an aliased member holding stack garbage), no overrun (which
    // would smash the neighbouring frame slot), no gap.
    std::set<std::uint64_t> covered;
    for (DecodedStore const& d : L.stores)
        for (std::uint64_t b = 0; b < d.width; ++b) covered.insert(d.offset + b);
    std::set<std::uint64_t> expected;
    for (std::uint64_t b = 0; b < lay->size; ++b) expected.insert(b);
    EXPECT_EQ(covered, expected)
        << "the zero-fill must cover exactly the struct's " << lay->size
        << " bytes; covered " << covered.size();

    // The chunk policy itself: 13 bytes = one I64 + one I32 + one Char, so the
    // widest-chunk walk must produce exactly three stores at 0, 8, 12.
    ASSERT_EQ(L.stores.size(), 3u) << "expected the I64→I32→Char chunk walk";
    EXPECT_EQ(L.stores[0].offset, 0u);
    EXPECT_EQ(L.stores[0].width, 8u);
    EXPECT_EQ(L.stores[1].offset, 8u);
    EXPECT_EQ(L.stores[1].width, 4u);
    EXPECT_EQ(L.stores[2].offset, 12u);
    EXPECT_EQ(L.stores[2].width, 1u);
}

// The real shipped shape, same rule: `ULARGE_INTEGER u = {0};` → 8 zero bytes.
TEST(OverlapStructZeroInit, RealUlargeIntegerShapeZeroFills) {
    TypeInterner ti = makeInterner();
    TypeId const s  = ulargeStruct(ti);
    ASSERT_TRUE(compositeFieldsOverlap(s, ti, kNatural16, DataModel::Lp64));

    std::array<std::int64_t, 3> const zeros{0, 0, 0};
    Built built = buildVarDeclInit(ti, s, zeros);
    Lowered L;
    lowerInto(L, built, ti);

    ASSERT_TRUE(L.result.ok)
        << (L.reporter.all().empty() ? std::string{} : L.reporter.all()[0].actual);
    ASSERT_EQ(L.stores.size(), 1u) << "8 bytes is exactly one I64 chunk";
    EXPECT_EQ(L.stores[0].offset, 0u);
    EXPECT_EQ(L.stores[0].width, 8u);
    EXPECT_TRUE(L.stores[0].valueIsZeroConst);
}

// ── 2 · `{}` (C23 empty initializer) into the same struct ────────────────────
//
// CST→HIR normalizes `{}` to the SAME positional all-zero child list `{0}` gets —
// the one observable difference at this tier is that `{}` marks EVERY child
// `Synthetic` (no element came from source text) while `{0}` leaves the first
// child plain. This pins that the accept decision is driven by the VALUES, never
// by the synthetic bit. The end-to-end `{}` proof (real front end, real run) is
// the corpus example `examples/c/overlap_struct_zero_init/`.

TEST(OverlapStructZeroInit, EmptyInitializerShapeZeroFillsIdentically) {
    TypeInterner ti = makeInterner();
    TypeId const s  = overlapStruct(ti);
    std::array<std::int64_t, 3> const zeros{0, 0, 0};

    Built braceZero = buildVarDeclInit(ti, s, zeros, /*synthetic=*/false);
    Lowered A;
    lowerInto(A, braceZero, ti);

    Built emptyInit = buildVarDeclInit(ti, s, zeros, /*synthetic=*/true);
    Lowered B;
    lowerInto(B, emptyInit, ti);

    ASSERT_TRUE(B.result.ok)
        << "`{}` into an overlapping struct must lower exactly as `{0}` does: "
        << (B.reporter.all().empty() ? std::string{} : B.reporter.all()[0].actual);
    ASSERT_EQ(A.stores.size(), B.stores.size());
    for (std::size_t i = 0; i < A.stores.size(); ++i) {
        EXPECT_EQ(A.stores[i].offset, B.stores[i].offset) << "chunk " << i;
        EXPECT_EQ(A.stores[i].width, B.stores[i].width) << "chunk " << i;
        EXPECT_TRUE(B.stores[i].valueIsZeroConst) << "chunk " << i;
    }
}

// ── 3 · a NON-zero initializer into an overlapping struct is STILL REFUSED ────

TEST(OverlapStructZeroInit, NonZeroInitIntoOverlappingStructStillFailsLoud) {
    TypeInterner ti = makeInterner();
    TypeId const s  = ulargeStruct(ti);

    // `{0, 1, 0}` — `LowPart = 1` aliases `QuadPart`'s low 4 bytes, so member-wise
    // stores would depend on declaration order. Exactly the ambiguity the refusal
    // exists for; it must survive the zero-init relaxation.
    std::array<std::int64_t, 3> const someNonZero{0, 1, 0};
    Built built = buildVarDeclInit(ti, s, someNonZero);
    Lowered L;
    lowerInto(L, built, ti);

    EXPECT_FALSE(L.result.ok) << "a non-zero overlapping brace-init must NOT lower";
    EXPECT_TRUE(sawUnsupported(
        L.reporter, "brace-initialization of an overlapping "
                    "explicit-offset struct is unsupported"))
        << "the pre-existing refusal text must be preserved verbatim, not softened";
    EXPECT_TRUE(L.stores.empty())
        << "a refused initializer must emit NO partial member writes";
}

// `-0.0` is numerically zero but its sign bit is SET, so its bytes are NOT all
// zero — a zero-fill would silently change the stored value. It must be refused.
TEST(OverlapStructZeroInit, NegativeZeroIsNotTreatedAsAZeroFill) {
    TypeInterner ti = makeInterner();
    TypeId const s  = doubleOverlayStruct(ti);
    ASSERT_TRUE(compositeFieldsOverlap(s, ti, kNatural16, DataModel::Lp64));

    Built plusZero = buildMinusZeroInit(ti, s, 0.0);
    Lowered P;
    lowerInto(P, plusZero, ti);
    ASSERT_TRUE(P.result.ok) << "+0.0 IS all-zero bytes and must be accepted";

    Built minusZero = buildMinusZeroInit(ti, s, -0.0);
    Lowered M;
    lowerInto(M, minusZero, ti);
    EXPECT_FALSE(M.result.ok)
        << "-0.0 has its sign bit set — a zero-fill would drop it; refuse instead";
    EXPECT_TRUE(sawUnsupported(M.reporter, "overlapping"));
}

// ── 4 · a NON-overlapping explicit-offset struct lowers MEMBER-WISE ───────────
//
// The gate keys on ACTUAL overlap, not on the mere presence of explicit offsets.
// Before this cycle the guard asked `hasExplicitOffsets`, so this shape — whose
// members are provably disjoint — was refused too: a FALSE refusal.

TEST(OverlapStructZeroInit, DisjointExplicitOffsetStructLowersMemberWise) {
    TypeInterner ti = makeInterner();
    TypeId const s  = disjointOffsetStruct(ti);
    ASSERT_TRUE(ti.hasExplicitOffsets(s))
        << "fixture precondition: the offsets must be EXPLICIT";
    ASSERT_FALSE(compositeFieldsOverlap(s, ti, kNatural16, DataModel::Lp64))
        << "fixture precondition: and they must NOT overlap";

    std::array<std::int64_t, 2> const values{7, 9};
    Built built = buildVarDeclInit(ti, s, values);
    Lowered L;
    lowerInto(L, built, ti);

    ASSERT_TRUE(L.result.ok)
        << "disjoint explicit offsets are unambiguous — member-wise is correct: "
        << (L.reporter.all().empty() ? std::string{} : L.reporter.all()[0].actual);
    // MEMBER-wise, not a zero-fill: exactly one Store per field, at that field's
    // DECLARED offset (0 and 8 — NOT the natural 0 and 4), each 4 bytes wide, and
    // carrying the field's own non-zero value.
    ASSERT_EQ(L.stores.size(), 2u) << "one Store per field, no zero-fill chunks";
    EXPECT_EQ(L.stores[0].offset, 0u);
    EXPECT_EQ(L.stores[0].width, 4u);
    EXPECT_FALSE(L.stores[0].valueIsZeroConst) << "field 0's value is 7, not 0";
    EXPECT_EQ(L.stores[1].offset, 8u) << "the DECLARED offset, not the natural 4";
    EXPECT_EQ(L.stores[1].width, 4u);
    EXPECT_FALSE(L.stores[1].valueIsZeroConst) << "field 1's value is 9, not 0";
}

// ── D-DIAG-BRACE-INIT-AGGREGATE-SOURCE-SPAN ──────────────────────────────────
//
// The refusal must be LOCATABLE. `HirToMir::unsupported` reads the span from the
// source map keyed by the reported node; this pins the MIR half (given an entry,
// the diagnostic carries it). The CST→HIR half — that `lowerBraceInit` actually
// RECORDS an entry for the aggregate, which it did not before this cycle — is
// pinned in `tests/hir/test_hir_lowering_c.cpp`.

TEST(OverlapStructZeroInit, RefusalCarriesTheAggregateSourceSpan) {
    TypeInterner ti = makeInterner();
    TypeId const s  = ulargeStruct(ti);
    std::array<std::int64_t, 3> const someNonZero{0, 1, 0};
    Built built = buildVarDeclInit(ti, s, someNonZero);

    // Find the top-level aggregate (the node the refusal reports against) and give
    // it a span, exactly as the real CST→HIR lowering now does.
    HirNodeId aggNode{};
    // Arena slot 0 is the reserved sentinel; real ids run [1, nodeCount()).
    for (std::uint32_t i = 1; i < built.hir.nodeCount(); ++i) {
        HirNodeId const n{i};
        if (built.hir.kind(n) == HirKind::ConstructAggregate) { aggNode = n; break; }
    }
    ASSERT_TRUE(aggNode.valid());
    HirSourceMap spans{built.hir};
    constexpr std::uint32_t kBuf = 77;
    spans.set(aggNode, HirSourceLoc{BufferId{kBuf}, SourceSpan::of(100, 112)});

    Lowered L;
    lowerInto(L, built, ti, &spans);
    ASSERT_FALSE(L.result.ok);

    bool located = false;
    for (auto const& d : L.reporter.all()) {
        if (d.code != DiagnosticCode::H_UnsupportedLoweringForKind) continue;
        if (d.actual.find("overlapping") == std::string::npos) continue;
        EXPECT_EQ(d.buffer.v, kBuf)
            << "the refusal must name the buffer its construct lives in";
        EXPECT_EQ(d.span.start(), 100u)
            << "a fail-loud diagnostic with no `--> file:line` is half a diagnostic";
        EXPECT_EQ(d.span.length(), 12u);
        located = true;
    }
    EXPECT_TRUE(located) << "no locatable overlap refusal was reported";
}
