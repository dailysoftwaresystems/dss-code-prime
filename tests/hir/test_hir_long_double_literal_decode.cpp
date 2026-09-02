// D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION — the CST→HIR literal LEAF
// carries a `long double` at the TARGET's mantissa width.
//
// ★★★ WHY THIS FILE EXISTS BESIDE THE CORPUS EXAMPLE, AND WHY THE EXAMPLE ALONE
// IS NOT THE PIN. `examples/c/c_long_double_literal_decode` BUILDS on all four
// axes but only RUNS the arm its host can execute — on Windows that is `pe64`,
// the ONE axis where `long double` IS binary64 and where this defect never
// existed. ✔MEASURED: with the target-precision routing REMOVED from
// `cst_to_hir.cpp`, the example's own binary exits 21 instead of 42 when the
// elf64-x86_64 artifact is run under WSL — and `examples/c/…` + `integrated_
// tests/c/…` both still reported PASSED on the Windows leg, because neither ran
// that artifact. An instrument that can only fail on a leg the gate is not
// standing on is not a pin; this file asks the question the Windows leg CAN
// answer, by inspecting the literal POOL rather than executing anything.
//
// The oracle is the reference compiler's emitted bit pattern, never DSS's own —
// comparing one DSS-decoded literal to another passes with both wrong together,
// which is how the defect survived from 2026-07-18 to 2026-09-02.
// ✔MEASURED 2026-09-02, each reference probed SEPARATELY, as a static
// initializer's bytes:
//   `0.1L` x87-80  → cd cc cc cc cc cc cc cc fb 3f   (gcc 13.3.0 AND clang 18.1.3)
//   `0.1L` binary128 → 9a ×13 fb 3f                  (aarch64-linux-gnu-gcc 13.3.0)
//   `0.1L` binary64  → 9a 99 99 99 99 99 b9 3f       (cl.exe 14.51.36231, where
//                                                     sizeof(long double) == 8)
// `WideFloatValue::pack()` is byte-identical to what `appendWideFloatBits`
// writes, so the `{lo, hi}` equalities below ARE comparisons of emitted bytes.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/wide_float_value.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include "shipped_schema_or_throw.hpp"   // the ONE load-or-fail-this-test helper

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

using namespace dss;

namespace {

// The target is process-owned for the same reason `test_hir_lowering_c.cpp`'s
// `fixtureTarget` is: `analyze` takes it NON-OWNING and the returned model
// republishes it for the lowering to read, so a function-local would dangle.
[[nodiscard]] TargetSchema const* fixtureTarget(char const* arch) {
    static std::shared_ptr<TargetSchema const> const kX86 = [] {
        auto t = TargetSchema::loadShipped("x86_64");
        return t.has_value() ? *t : nullptr;
    }();
    static std::shared_ptr<TargetSchema const> const kArm = [] {
        auto t = TargetSchema::loadShipped("arm64");
        return t.has_value() ? *t : nullptr;
    }();
    return (std::string_view{arch} == "arm64") ? kArm.get() : kX86.get();
}

// Lower `long double g = <literal>;` under a CHOSEN long-double axis and return
// the single literal the pool holds. The axis is threaded exactly as the driver
// threads it (`effectiveLongDoubleFormat(target, format)`), which is the whole
// point: the decode width is a property of the FORMAT's declaration, not of the
// host this test runs on.
struct Lowered {
    std::shared_ptr<CompilationUnit> cu;
    std::optional<SemanticModel>     model;
    std::unique_ptr<CstToHirResult>  res;
};

[[nodiscard]] Lowered lowerLongDoubleInit(std::string const& literal,
                                          LongDoubleFormat  axis,
                                          char const*       arch) {
    Lowered out;
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory("long double g = " + literal + ";\n", "<mem>");
    out.cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    out.model.emplace(analyze(out.cu, DiagnosticBudget::libraryDefault(),
                              DataModel::Lp64, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt, axis,
                              fixtureTarget(arch)));
    return out;
}

struct Bits { std::uint64_t lo, hi; };

// The pool's ONE float literal, as a wide value — fails the test loudly if the
// leaf is on the `double` arm instead (which is exactly the defect's shape).
void expectWideLiteral(std::string const& literal, LongDoubleFormat axis,
                       char const* arch, TypeKind wantKind, Bits want,
                       char const* provenance) {
    Lowered lw = lowerLongDoubleInit(literal, axis, arch);
    ASSERT_FALSE(lw.model->hasErrors()) << literal << " did not analyze cleanly";
    DiagnosticReporter r;
    lw.res = lowerToHir(*lw.model, r);
    ASSERT_TRUE(lw.res->ok) << literal << " did not lower";
    ASSERT_EQ(lw.res->literalPool.size(), 1u) << literal;
    auto const& v = lw.res->literalPool.at(0);
    EXPECT_EQ(v.core, wantKind);
    ASSERT_TRUE(std::holds_alternative<WideFloatValue>(v.value))
        << literal << ": the leaf is still on the host-`double` arm — that IS "
           "the defect (a binary64-rounded value reaching the pool)";
    auto const& wf = std::get<WideFloatValue>(v.value);
    auto const  p  = wf.pack();
    EXPECT_EQ(p.lo, want.lo) << literal << " low half — reference: " << provenance;
    EXPECT_EQ(p.hi, want.hi) << literal << " high half — reference: " << provenance;
}

}  // namespace

TEST(HirLongDoubleLiteralDecode, X87AxisLeafCarriesTargetPrecision) {
    expectWideLiteral("0.1L", LongDoubleFormat::X87_80, "x86_64", TypeKind::F80,
                      {0xCCCCCCCCCCCCCCCDull, 0x3FFBull},
                      "gcc 13.3.0 + clang 18.1.3: cd cc cc cc cc cc cc cc fb 3f");
}

TEST(HirLongDoubleLiteralDecode, Ieee128AxisLeafCarriesTargetPrecision) {
    expectWideLiteral("0.1L", LongDoubleFormat::Ieee128, "arm64", TypeKind::F128,
                      {0x999999999999999Aull, 0x3FFB999999999999ull},
                      "aarch64-linux-gnu-gcc 13.3.0: 9a x13 fb 3f");
}

// ★ THE DEFECT STATED AS AN INEQUALITY. The value the leaf used to carry is
// exactly `fromDouble(strtod("0.1"))` — an EXACT widen of an already-rounded
// binary64. A pattern-only pin would still pass if the decode were re-routed
// through binary64 and the pattern regenerated from DSS; this one cannot.
TEST(HirLongDoubleLiteralDecode, LeafIsNotTheWidenedHostDouble) {
    for (auto const& [axis, arch, kind] :
         {std::tuple{LongDoubleFormat::X87_80, "x86_64", TypeKind::F80},
          std::tuple{LongDoubleFormat::Ieee128, "arm64", TypeKind::F128}}) {
        Lowered lw = lowerLongDoubleInit("0.1L", axis, arch);
        ASSERT_FALSE(lw.model->hasErrors());
        DiagnosticReporter r;
        lw.res = lowerToHir(*lw.model, r);
        ASSERT_TRUE(lw.res->ok);
        ASSERT_EQ(lw.res->literalPool.size(), 1u);
        auto const* wf = std::get_if<WideFloatValue>(&lw.res->literalPool.at(0).value);
        ASSERT_NE(wf, nullptr);
        EXPECT_FALSE(*wf == WideFloatValue::fromDouble(0.1, kind))
            << "the leaf equals the binary64 0.1 widened — it is still "
               "host-rounded";
    }
}

// The half-way constants: EXACT midpoints between adjacent normals, settled only
// by round-to-nearest-EVEN. The even-neighbour tie rounds DOWN to 1.0; the
// odd-neighbour tie rounds UP.
TEST(HirLongDoubleLiteralDecode, X87HalfwayCasesRoundToNearestEvenThroughLowering) {
    expectWideLiteral(
        "1.0000000000000000000542101086242752217003726400434970855712890625L",
        LongDoubleFormat::X87_80, "x86_64", TypeKind::F80,
        {0x8000000000000000ull, 0x3FFFull},
        "gcc 13.3.0 + clang 18.1.3: 00 00 00 00 00 00 00 80 ff 3f");
    expectWideLiteral(
        "1.0000000000000000001626303258728256651011179201304912567138671875L",
        LongDoubleFormat::X87_80, "x86_64", TypeKind::F80,
        {0x8000000000000002ull, 0x3FFFull},
        "gcc 13.3.0 + clang 18.1.3: 02 00 00 00 00 00 00 80 ff 3f");
}

// ★ THE f64 AXIS MUST NOT MOVE. Where the format declares `long double` IS
// binary64 (pe64 MSVC, Apple arm64), the host `double` path was always the right
// answer and the leaf must STAY on the `double` arm — the fix is keyed on the
// declared axis, not applied to every `long double`.
TEST(HirLongDoubleLiteralDecode, F64AxisLeafStaysOnTheHostDoubleArm) {
    Lowered lw = lowerLongDoubleInit("0.1L", LongDoubleFormat::F64, "x86_64");
    ASSERT_FALSE(lw.model->hasErrors());
    DiagnosticReporter r;
    lw.res = lowerToHir(*lw.model, r);
    ASSERT_TRUE(lw.res->ok);
    ASSERT_EQ(lw.res->literalPool.size(), 1u);
    auto const& v = lw.res->literalPool.at(0);
    EXPECT_EQ(v.core, TypeKind::F64);
    ASSERT_TRUE(std::holds_alternative<double>(v.value))
        << "an f64-axis long double must stay on the host `double` arm";
    EXPECT_EQ(std::get<double>(v.value), 0.1)
        << "and carry the same binary64 value cl.exe 14.51.36231 bakes "
           "(9a 99 99 99 99 99 b9 3f)";
}
