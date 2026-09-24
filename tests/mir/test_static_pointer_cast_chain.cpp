// A null pointer, or an integer address, that reaches a pointer object with STATIC
// storage through a CHAIN of pointer casts is static data, never a load-time store —
// P68 round 9, D-C-STATIC-INITIALIZER-POINTER-CAST-CHAIN-REFUSED.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// `char *p = NULL;` with the platforms' own `NULL`, `((void *)0)`, is TWO pointer
// casts: the explicit `(void *)` and the implicit `void *` → `char *` conversion.
// The static-initializer classifiers in `hir_to_mir.cpp`
// (`tryClassifyNullPointerConst`, `tryClassifyIntToPtrConst`) peeled exactly ONE,
// and const-eval refuses a cast to a pointer, so the object became a RUNTIME
// initializer — which the static-data producer refuses on every format ("has a
// runtime initializer"), and which a pointer ARRAY cannot even reach (the
// aggregate-rvalue refusal). ✔MEASURED at 4d9a24c4 on x86_64 ELF: `char *g =
// (void*)0`, `(char*)(void*)0`, `int (*g)(void) = (void*)0`, a struct member, a
// `char *const`, a block-scope static, a mixed pointer array and the integer
// sibling `(char*)(void*)0x10` (scalar and member) were each refused, while gcc
// 13.3.0 and clang 18.1.3 (x86_64 and aarch64, -O0 and -O2, `-std=c17
// -pedantic-errors`), mingw-w64 13.2.0 and MSVC 14.51 accept every one. C 6.6p9
// lets pointer casts build an address constant; 6.3.2.3p4 makes a null pointer
// converted to another pointer type a null pointer.
//
// ★ IT BECAME A BLOCKER WHEN `NULL` BECAME THE REFERENCES' `((void *)0)` (P68
// round 9): at 4d9a24c4 DSS's `NULL` was the integer 0 — one cast — so `static
// char *p = NULL;` folded, and the corrected definition turned it, a
// NULL-terminated pointer array and a getopt option table into refusals on every
// pair until the classifiers peeled the whole chain.
//
// ═══ WHAT IS PINNED, ON EVERY REAL (target, format) PAIR ════════════════════
//   (1) each scalar null-pointer shape is a zero pointer leaf with no init function;
//   (2) each integer-address shape is its value, and a non-zero integer is never
//       taken for a null pointer;
//   (3) each aggregate carrying such a member is a static aggregate whose leaves
//       are exactly those values beside the string relocations;
//  (3b) an element address off a CHAINED null base (sqlite's `SQLITE_INT_TO_PTR`
//       shape with `NULL` as its base) is its offset;
//   (4) the static-data producer (`assembleUnit`) accepts the whole set;
//   (5) THE CONTROL: the one-cast shapes, and a symbol address through a chain,
//       which must stay a RELOCATION and never become an integer.
// Each of (1)-(3b) is its own translation unit, so a regression names the family
// it broke instead of failing every pin at the first refused global.
// ⚠ WHICH ARM EACH FAMILY PROVES. A null chain reaches the integer-address arm too
// (it runs after the null arm and folds `(char *)(void *)0` to the identical zero
// leaf), so (1) and (3) stay green while EITHER arm peels the chain; they go red
// only with both reverted. (2) needs the integer-address arm, and (3b) the null
// arm — the null-base element classifier asks it about the base. ✔MEASURED by the
// three red-on-disable runs recorded in the row. The runtime half
// — the bytes the image holds, read back through volatiles on pe64, ELF x86_64 and
// ELF aarch64, debug and release — is `examples/c/static_pointer_cast_chain`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/object_format_schema.hpp"
#include "mir/mir.hpp"
#include "mir/mir_literal_pool.hpp"
#include "program/compile_pipeline.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::vector<std::string> shippedNames(std::string_view dir,
                                                    std::string_view suffix) {
    std::vector<std::string> out;
    for (auto const& e :
         std::filesystem::directory_iterator{dss::test::configRoot() / dir}) {
        std::string const fn = e.path().filename().string();
        auto const at = fn.find(suffix);
        if (at == std::string::npos) continue;
        out.push_back(fn.substr(0, at));
    }
    return out;
}

[[nodiscard]] std::string allErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "  ";
        out += d.actual;
        out += '\n';
    }
    return out;
}

[[nodiscard]] std::shared_ptr<GrammarSchema> const& cLanguage() {
    static std::shared_ptr<GrammarSchema> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema>{};
    }();
    return schema;
}

// Every shipped (target, format) pair whose format names the target's machine —
// the pairs a user can build. Enumerated from the config, never listed here.
struct RealPair {
    std::string                         label;
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] std::vector<RealPair> const& realPairs() {
    static std::vector<RealPair> const pairs = [] {
        std::vector<RealPair> out;
        for (std::string const& t : shippedNames("targets", ".target.json")) {
            auto target = TargetSchema::loadShipped(t);
            if (!target.has_value()) continue;   // counted below: the floor catches a collapse
            for (std::string const& f : shippedNames("object-formats", ".format.json")) {
                auto format = ObjectFormatSchema::loadShipped(f);
                if (!format.has_value()) continue;
                if ((*format)->targetArch() != (*target)->name()) continue;
                out.push_back(RealPair{t + ":" + f, *target, *format});
            }
        }
        return out;
    }();
    return pairs;
}

// FLOOR: ✔MEASURED at P68 round 9 — 22 real pairs (13 x86_64 + 9 arm64 formats),
// the same enumeration `test_limits_h_lattice` counts.
constexpr std::size_t kRealPairFloor = 22;

// The translation unit as the driver builds it for this pair: the shipped system
// headers, and the pair's own consequences (its `NULL` among them).
[[nodiscard]] CompilationUnit unitFor(RealPair const& p, std::string_view source) {
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, *p.target, *p.format);
    builder.addInMemory(std::string{source}, "cast_chain.c");
    return std::move(builder).finish();
}

[[nodiscard]] std::optional<std::uint16_t>
callingConventionIndex(RealPair const& p, DiagnosticReporter& rep) {
    auto const abi = dss::ffi::resolveAbi(*p.target, *p.format, rep);
    if (!abi.has_value() || abi->cc == nullptr) return std::nullopt;
    auto const ccSpan = p.target->callingConventions();
    return static_cast<std::uint16_t>(std::distance(ccSpan.data(), abi->cc));
}

// The driver's front half (`buildCuMir`) for one pair, or the reason it refused.
struct Built {
    std::optional<CuMirModule> mir;
    std::string                why;
};

[[nodiscard]] Built buildFor(RealPair const& p, std::string_view source) {
    Built out;
    DiagnosticReporter rep;
    auto const cc = callingConventionIndex(p, rep);
    if (!cc.has_value()) {
        out.why = "no calling convention resolved:\n" + allErrors(rep);
        return out;
    }
    CompilationUnit cu = unitFor(p, source);
    auto mir = buildCuMir(cu, *cLanguage(), *p.target, *p.format, *cc, rep,
                          CompileOptions{DiagnosticBudget::libraryDefault()});
    if (!mir.has_value() || rep.errorCount() != 0) {
        out.why = "buildCuMir refused:\n" + allErrors(rep);
        return out;
    }
    out.mir = std::move(mir);
    return out;
}

// The static image of the global `name`: its literal, or nullptr with the reason
// (absent, a runtime initializer, or no initializer at all).
[[nodiscard]] MirLiteralValue const* staticImageOf(CuMirModule const& m,
                                                   std::string_view name,
                                                   std::string& why) {
    for (std::uint32_t i = 0; i < m.mir.moduleGlobalCount(); ++i) {
        MirGlobalId const g = m.mir.globalAt(i);
        auto const* rec = m.model.recordFor(SymbolId{m.mir.globalSymbol(g).v});
        if (rec == nullptr || rec->name != name) continue;
        if (m.mir.globalInitFunc(g).valid()) {
            why = "a RUNTIME initializer (an init-function store), not static data";
            return nullptr;
        }
        std::uint32_t const li = m.mir.globalInitLiteralIndex(g);
        if (li == UINT32_MAX) {
            why = "no static initializer at all";
            return nullptr;
        }
        return &m.mir.literalValue(li);
    }
    why = "no global of that name";
    return nullptr;
}

// No global anywhere in the module — the block-scope static included, whose name
// is the function's business — is initialized by a load-time store.
void expectNoRuntimeInitializer(CuMirModule const& m) {
    for (std::uint32_t i = 0; i < m.mir.moduleGlobalCount(); ++i) {
        MirGlobalId const g = m.mir.globalAt(i);
        auto const* rec = m.model.recordFor(SymbolId{m.mir.globalSymbol(g).v});
        EXPECT_FALSE(m.mir.globalInitFunc(g).valid())
            << "global '" << (rec != nullptr ? rec->name : std::string{"<unnamed>"})
            << "' has a runtime initializer";
    }
}

[[nodiscard]] bool isPointerLeaf(MirLiteralValue const& v, std::uint64_t value) {
    auto const* u = std::get_if<std::uint64_t>(&v.value);
    return u != nullptr && *u == value && v.core == TypeKind::Ptr;
}

[[nodiscard]] bool isIntLeaf(MirLiteralValue const& v, std::int64_t value) {
    auto const* i = std::get_if<std::int64_t>(&v.value);
    return i != nullptr && *i == value;
}

[[nodiscard]] bool isRelocation(MirLiteralValue const& v) {
    return std::holds_alternative<MirSymbolAddrValue>(v.value);
}

[[nodiscard]] MirAggregateValue const* fieldsOf(MirLiteralValue const& v) {
    return std::get_if<MirAggregateValue>(&v.value);
}

// Run `check` over the static image of each named global on every real pair.
void onEveryRealPair(std::string_view source,
                     std::function<void(CuMirModule const&)> const& check) {
    ASSERT_NE(cLanguage(), nullptr);
    std::size_t pairs = 0;
    for (RealPair const& p : realPairs()) {
        SCOPED_TRACE(p.label);
        ++pairs;
        Built const b = buildFor(p, source);
        if (!b.mir.has_value()) {
            ADD_FAILURE() << b.why;
            continue;
        }
        expectNoRuntimeInitializer(*b.mir);
        check(*b.mir);
    }
    EXPECT_GE(pairs, kRealPairFloor) << "the real-pair enumeration collapsed";
}

// ── the four translation units ──────────────────────────────────────────────
constexpr std::string_view kScalarNulls =
    "#include <stddef.h>\n"
    "typedef int (*fn_t)(void);\n"
    "char *g02 = (void *)0;\n"
    "char *g03 = (char *)(void *)0;\n"
    "fn_t g04 = (void *)0;\n"
    "char *const g07 = (void *)0;\n"
    "char *n10 = NULL;\n"
    "static char *s10 = NULL;\n"
    "char *blockStatic(void) { static char *p = NULL; return p; }\n"
    "char *useS10(void) { return s10; }\n";

constexpr std::string_view kIntegerAddresses =
    "struct S { char *p; int n; };\n"
    "char *g08 = (char *)(void *)0x10;\n"
    "struct S g12 = { (char *)(void *)0x10, 3 };\n"
    "char *one = (char *)(void *)1;\n";

constexpr std::string_view kAggregates =
    "#include <stddef.h>\n"
    "struct S { char *p; int n; };\n"
    "struct opt { const char *name; int has_arg; int *flag; int val; };\n"
    "struct S g05 = { (void *)0, 3 };\n"
    "char *g06[3] = { (void *)0, \"x\", (void *)0 };\n"
    "char *n14[3] = { NULL, \"x\", NULL };\n"
    "static struct opt g13[] = { { \"help\", 0, NULL, 'h' }, { NULL, 0, NULL, 0 } };\n"
    "int useG13(void) { return g13[0].val; }\n";

// sqlite's `SQLITE_INT_TO_PTR(X)`, `(void *)&((char *)0)[X]`, with a CHAINED null
// base — `NULL` or `(void *)0` under the `(char *)`. Only the null-pointer arm can
// answer it: the null-base element classifier asks that arm whether the base is a
// null pointer, and the integer-address arm does not look through `&…[X]` at all.
// So this is the family that goes red when the NULL arm alone loses the chain —
// every other family is ALSO answered by the integer-address arm (a null chain
// folds to the identical zero leaf there), which masks it.
constexpr std::string_view kNullBases =
    "#include <stddef.h>\n"
    "struct F { const char *name; void *user; };\n"
    "void *b1 = (void *)&((char *)(void *)0)[5];\n"
    "void *b2 = (void *)&((char *)NULL)[7];\n"
    "struct F b3[2] = { { \"a\", (void *)&((char *)NULL)[3] },\n"
    "                   { \"b\", (void *)&((char *)(void *)0)[9] } };\n";

// The control: shapes that folded before the chain was peeled, and a symbol
// address through a chain — which the symbol-address arm claims FIRST and which
// must never be mistaken for an integer.
constexpr std::string_view kControl =
    "struct S { char *p; int n; };\n"
    "int x;\n"
    "void *c01 = (void *)0;\n"
    "char *c09 = (char *)0x10;\n"
    "struct S c05 = { 0, 3 };\n"
    "char *a1 = (char *)(void *)&x;\n"
    "char *a3 = (char *)(void *)\"s\";\n";

}  // namespace

// ── (1) A SCALAR NULL POINTER THROUGH A CAST CHAIN IS A ZERO POINTER LEAF ────
TEST(StaticPointerCastChain, AScalarNullPointerThroughACastChainIsAZeroLeaf) {
    onEveryRealPair(kScalarNulls, [](CuMirModule const& m) {
        for (std::string_view const name : {"g02", "g03", "g04", "g07", "n10", "s10"}) {
            std::string why;
            MirLiteralValue const* v = staticImageOf(m, name, why);
            if (v == nullptr) {
                ADD_FAILURE() << name << ": " << why;
                continue;
            }
            EXPECT_TRUE(isPointerLeaf(*v, 0))
                << name << " must be a zero pointer leaf (8 zero bytes, no relocation)";
        }
    });
}

// ── (2) AN INTEGER ADDRESS THROUGH A CAST CHAIN IS ITS VALUE ──────────────────
TEST(StaticPointerCastChain, AnIntegerAddressThroughACastChainIsItsValue) {
    onEveryRealPair(kIntegerAddresses, [](CuMirModule const& m) {
        std::string why;
        if (MirLiteralValue const* v = staticImageOf(m, "g08", why); v != nullptr) {
            EXPECT_TRUE(isPointerLeaf(*v, 0x10)) << "g08 must hold the address 0x10";
        } else {
            ADD_FAILURE() << "g08: " << why;
        }
        // Never a null pointer: the null arm runs first and must decline a non-zero.
        if (MirLiteralValue const* v = staticImageOf(m, "one", why); v != nullptr) {
            EXPECT_TRUE(isPointerLeaf(*v, 1)) << "`(char *)(void *)1` is the address 1";
        } else {
            ADD_FAILURE() << "one: " << why;
        }
        if (MirLiteralValue const* v = staticImageOf(m, "g12", why); v != nullptr) {
            auto const* agg = fieldsOf(*v);
            ASSERT_NE(agg, nullptr) << "g12 must be a static aggregate";
            ASSERT_EQ(agg->fields.size(), 2u);
            EXPECT_TRUE(isPointerLeaf(agg->fields[0], 0x10));
            EXPECT_TRUE(isIntLeaf(agg->fields[1], 3));
        } else {
            ADD_FAILURE() << "g12: " << why;
        }
    });
}

// ── (3) AN AGGREGATE CARRYING SUCH A MEMBER IS A STATIC AGGREGATE ─────────────
TEST(StaticPointerCastChain, AggregateMembersThroughACastChainAreStaticData) {
    onEveryRealPair(kAggregates, [](CuMirModule const& m) {
        std::string why;
        if (MirLiteralValue const* v = staticImageOf(m, "g05", why); v != nullptr) {
            auto const* agg = fieldsOf(*v);
            ASSERT_NE(agg, nullptr);
            ASSERT_EQ(agg->fields.size(), 2u);
            EXPECT_TRUE(isPointerLeaf(agg->fields[0], 0));
            EXPECT_TRUE(isIntLeaf(agg->fields[1], 3));
        } else {
            ADD_FAILURE() << "g05: " << why;
        }
        // `(void *)0` and `NULL` beside a string: a null leaf, the string's
        // relocation, a null leaf.
        for (std::string_view const name : {"g06", "n14"}) {
            MirLiteralValue const* v = staticImageOf(m, name, why);
            if (v == nullptr) {
                ADD_FAILURE() << name << ": " << why;
                continue;
            }
            auto const* agg = fieldsOf(*v);
            ASSERT_NE(agg, nullptr) << name;
            ASSERT_EQ(agg->fields.size(), 3u) << name;
            EXPECT_TRUE(isPointerLeaf(agg->fields[0], 0)) << name;
            EXPECT_TRUE(isRelocation(agg->fields[1])) << name << ": the string's address";
            EXPECT_TRUE(isPointerLeaf(agg->fields[2], 0)) << name;
        }
        // The getopt option table: NULL in the name and flag members of both rows.
        if (MirLiteralValue const* v = staticImageOf(m, "g13", why); v != nullptr) {
            auto const* rows = fieldsOf(*v);
            ASSERT_NE(rows, nullptr);
            ASSERT_EQ(rows->fields.size(), 2u);
            auto const* help = fieldsOf(rows->fields[0]);
            auto const* last = fieldsOf(rows->fields[1]);
            ASSERT_NE(help, nullptr);
            ASSERT_NE(last, nullptr);
            ASSERT_EQ(help->fields.size(), 4u);
            ASSERT_EQ(last->fields.size(), 4u);
            EXPECT_TRUE(isRelocation(help->fields[0])) << "\"help\"";
            EXPECT_TRUE(isIntLeaf(help->fields[1], 0));
            EXPECT_TRUE(isPointerLeaf(help->fields[2], 0)) << "flag = NULL";
            EXPECT_TRUE(isIntLeaf(help->fields[3], 'h'));
            EXPECT_TRUE(isPointerLeaf(last->fields[0], 0)) << "name = NULL";
            EXPECT_TRUE(isIntLeaf(last->fields[1], 0));
            EXPECT_TRUE(isPointerLeaf(last->fields[2], 0)) << "flag = NULL";
            EXPECT_TRUE(isIntLeaf(last->fields[3], 0));
        } else {
            ADD_FAILURE() << "g13: " << why;
        }
    });
}

// ── (3b) AN ELEMENT ADDRESS OFF A CHAINED NULL BASE IS ITS OFFSET ────────────
TEST(StaticPointerCastChain, AnElementAddressOffAChainedNullBaseIsItsOffset) {
    onEveryRealPair(kNullBases, [](CuMirModule const& m) {
        std::string why;
        if (MirLiteralValue const* v = staticImageOf(m, "b1", why); v != nullptr) {
            EXPECT_TRUE(isPointerLeaf(*v, 5)) << "&((char *)(void *)0)[5] is the address 5";
        } else {
            ADD_FAILURE() << "b1: " << why;
        }
        if (MirLiteralValue const* v = staticImageOf(m, "b2", why); v != nullptr) {
            EXPECT_TRUE(isPointerLeaf(*v, 7)) << "&((char *)NULL)[7] is the address 7";
        } else {
            ADD_FAILURE() << "b2: " << why;
        }
        if (MirLiteralValue const* v = staticImageOf(m, "b3", why); v != nullptr) {
            auto const* rows = fieldsOf(*v);
            ASSERT_NE(rows, nullptr);
            ASSERT_EQ(rows->fields.size(), 2u);
            std::int64_t const offsets[2] = {3, 9};
            for (std::size_t r = 0; r < 2; ++r) {
                auto const* row = fieldsOf(rows->fields[r]);
                ASSERT_NE(row, nullptr);
                ASSERT_EQ(row->fields.size(), 2u);
                EXPECT_TRUE(isRelocation(row->fields[0])) << "row " << r << " name";
                EXPECT_TRUE(isPointerLeaf(row->fields[1],
                                          static_cast<std::uint64_t>(offsets[r])))
                    << "row " << r << " user";
            }
        } else {
            ADD_FAILURE() << "b3: " << why;
        }
    });
}

// ── (4) THE STATIC-DATA PRODUCER ACCEPTS THE WHOLE SET ────────────────────────
// The tier that refused: `lowerMirGlobalsToDataItems` inside `assembleUnit`. One
// translation unit carrying every shape above, with a `main`, so the executable
// formats resolve their entry as a real build would.
TEST(StaticPointerCastChain, TheStaticDataProducerAcceptsEveryShapeOnEveryRealPair) {
    ASSERT_NE(cLanguage(), nullptr);
    std::string const source = std::string{kScalarNulls}
                             + "struct S { char *p; int n; };\n"
                               "struct opt { const char *name; int has_arg; int *flag; int val; };\n"
                               "char *g08 = (char *)(void *)0x10;\n"
                               "struct S g12 = { (char *)(void *)0x10, 3 };\n"
                               "struct S g05 = { (void *)0, 3 };\n"
                               "char *g06[3] = { (void *)0, \"x\", (void *)0 };\n"
                               "char *n14[3] = { NULL, \"x\", NULL };\n"
                               "static struct opt g13[] = "
                               "{ { \"help\", 0, NULL, 'h' }, { NULL, 0, NULL, 0 } };\n"
                               "struct F { const char *name; void *user; };\n"
                               "void *b1 = (void *)&((char *)(void *)0)[5];\n"
                               "struct F b3[2] = { { \"a\", (void *)&((char *)NULL)[3] },\n"
                               "                   { \"b\", (void *)&((char *)(void *)0)[9] } };\n"
                               "int main(void) { return useS10() == NULL && g13[0].val == 'h'; }\n";
    std::size_t pairs = 0;
    for (RealPair const& p : realPairs()) {
        SCOPED_TRACE(p.label);
        ++pairs;
        DiagnosticReporter rep;
        auto const cc = callingConventionIndex(p, rep);
        ASSERT_TRUE(cc.has_value()) << allErrors(rep);
        CompilationUnit cu = unitFor(p, source);
        auto const mod = assembleUnit(cu, *cLanguage(), *p.target, *p.format, *cc, rep,
                                      CompileOptions{DiagnosticBudget::libraryDefault()});
        EXPECT_TRUE(mod.has_value());
        EXPECT_EQ(allErrors(rep), "");
    }
    EXPECT_GE(pairs, kRealPairFloor) << "the real-pair enumeration collapsed";
}

// ── (5) THE CONTROL: ONE CAST, AND A SYMBOL ADDRESS THROUGH A CHAIN ───────────
TEST(StaticPointerCastChain, OneCastShapesAndSymbolAddressesFoldAsBefore) {
    onEveryRealPair(kControl, [](CuMirModule const& m) {
        std::string why;
        if (MirLiteralValue const* v = staticImageOf(m, "c01", why); v != nullptr) {
            EXPECT_TRUE(isPointerLeaf(*v, 0)) << "c01";
        } else {
            ADD_FAILURE() << "c01: " << why;
        }
        if (MirLiteralValue const* v = staticImageOf(m, "c09", why); v != nullptr) {
            EXPECT_TRUE(isPointerLeaf(*v, 0x10)) << "c09";
        } else {
            ADD_FAILURE() << "c09: " << why;
        }
        if (MirLiteralValue const* v = staticImageOf(m, "c05", why); v != nullptr) {
            auto const* agg = fieldsOf(*v);
            ASSERT_NE(agg, nullptr);
            ASSERT_EQ(agg->fields.size(), 2u);
            EXPECT_TRUE(isPointerLeaf(agg->fields[0], 0));
            EXPECT_TRUE(isIntLeaf(agg->fields[1], 3));
        } else {
            ADD_FAILURE() << "c05: " << why;
        }
        for (std::string_view const name : {"a1", "a3"}) {
            MirLiteralValue const* v = staticImageOf(m, name, why);
            if (v == nullptr) {
                ADD_FAILURE() << name << ": " << why;
                continue;
            }
            EXPECT_TRUE(isRelocation(*v))
                << name << " is a link-time address — a relocation, never an integer";
        }
    });
}
