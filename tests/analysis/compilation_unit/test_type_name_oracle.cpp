// FC2 Part A (A3) — the compilation-unit type-name oracle + one-shot
// conditional reparse.
//
// An `#include`d header's typedefs are invisible to the INCLUDER's parse
// (files parse alone; trees merge post-parse), so a lone-identifier cast
// `(MyT)-1` whose MyT lives in a header froze as the value reading with
// an AmbiguousTypeNameCandidate. UnitBuilder::finish() resolves the
// candidates against the union of every tree's exported global type
// names and reparses the affected file ONCE with the resolved names
// seeded — these tests pin the POSITIVE outcome (the cast subtree
// exists in the final tree; reviewer duty-10: a diagnostic-only check
// would stay green if the reparse never ran), the zero-cost no-candidate
// path (reparse count 0), the unresolved-candidate path (value reading
// stands), the transitive-include harvest, and the crossRefs id
// refresh after replacement.

// FC13 UPDATE (D-PP-FC2-ORACLE-RETIRE): the config-selected C preprocessor now
// splices a quote-`#include`d header's TEXT inline BEFORE parsing, so a header
// typedef is visible during the includer's FIRST parse -- the cross-file cast
// commits immediately and the type-name ORACLE no longer fires for C quote
// includes (reparse count 0, one tree). The oracle MACHINERY is retained (it
// is language-agnostic; a future non-preprocessed-include language could
// exercise it), but these tests now pin the INLINE resolution that supersedes
// it for C. The examples/c-subset/include_typedef_cast corpus likewise now
// commits the cast inline.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include "analysis/compilation_unit/toy_cu_fixture.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace dss;
using dss::cu_test::hasCode;
using dss::cu_test::loadShippedSchema;

// RAII temp directory (same pattern as test_import_resolver.cpp): include
// targets must share the includer's directory.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("dss_fc2_oracle_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempDir(TempDir const&)            = delete;
    TempDir& operator=(TempDir const&) = delete;

    std::filesystem::path write(std::string const& name,
                                std::string const& content) const {
        auto path = dir_ / name;
        std::ofstream(path, std::ios::binary) << content;
        return path;
    }

    [[nodiscard]] std::filesystem::path const& path() const noexcept {
        return dir_;
    }

private:
    std::filesystem::path dir_;
};

// Normalized structural signature of the subtree rooted at `n`: schema-relative
// rule/token ids + token spelling, NO spans/NodeIds, EmptySpace leaves skipped.
// Two subtrees built under the SAME schema compare equal iff they have identical
// shape + spelling — used to prove a cast resolved by the FIRST-PARSE SEED is
// structurally IDENTICAL to the SAME cast resolved by the finish() oracle
// reparse (D-PERF-2 Opt-4 one-directional parity).
void subtreeSig(Tree const& t, NodeId n, std::string& out) {
    if (isEmptySpace(t.flags(n))) return;
    if (t.kind(n) == NodeKind::Token) {
        out += "T";
        out += std::to_string(t.tokenKind(n).v);
        out += ":";
        out += t.text(n);
        out += ";";
        return;
    }
    out += "R";
    out += std::to_string(t.rule(n).v);
    out += "{";
    for (NodeId c : t.children(n)) subtreeSig(t, c, out);
    out += "}";
}

// Signature of the FIRST `castExpr` subtree in `t` (empty if none).
[[nodiscard]] std::string firstCastExprSig(Tree const& t) {
    if (!t.hasSchema()) return {};
    const auto castRule = t.schema().rules().find("castExpr");
    if (!castRule.valid()) return {};
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == castRule.v) {
            std::string sig;
            subtreeSig(t, id, sig);
            return sig;
        }
    }
    return {};
}

[[nodiscard]] bool treeHasRule(Tree const& t, std::string_view ruleName) {
    if (!t.hasSchema()) return false;
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return false;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) {
            return true;
        }
    }
    return false;
}

} // namespace

// The headline cross-file case: the typedef lives in the header, the
// ambiguous cast in the includer. The oracle must resolve `MyT` from the
// header's export surface and the reparse must produce the CAST parse.
TEST(TypeNameOracle, CrossFileTypedefCastResolvesInline) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "#include \"types.h\"\n"
        "int main() { return (MyT)-1; }\n");
    dir.write("types.h", "typedef int MyT;\n");

    UnitBuilder b{loadShippedSchema("c-subset")};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    Tree const& mainTree = cu.trees()[0];
    EXPECT_FALSE(mainTree.diagnostics().hasErrors());
    EXPECT_TRUE(treeHasRule(mainTree, "castExpr"))
        << "the inlined typedef must let the cast commit on the first parse";
    EXPECT_FALSE(treeHasRule(mainTree, "parenExpr"));
    EXPECT_EQ(cu.typeNameReparseCount(), 0u);
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(),
                         DiagnosticCode::D_UnresolvedImport));
}

// No candidates anywhere → the oracle pass costs nothing and reparses
// nothing (the brief's "reparse does NOT run when no candidate exists"
// pin, via the counter — cheap observability, no behavior knob).
TEST(TypeNameOracle, NoCandidatesMeansNoReparse) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "#include \"types.h\"\n"
        "int main() { return (MyT)1 + (int)2; }\n");
    dir.write("types.h", "typedef int MyT;\n");

    UnitBuilder b{loadShippedSchema("c-subset")};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "castExpr"));
    EXPECT_EQ(cu.typeNameReparseCount(), 0u);
}

// A candidate the oracle CANNOT resolve (no tree exports the name) keeps
// the rolled-back value reading and triggers no reparse — semantic
// analysis remains the authority on the undeclared name.
TEST(TypeNameOracle, UnresolvedCandidateKeepsValueReading) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "int main() { int x; return (zzz)-x; }\n");

    UnitBuilder b{loadShippedSchema("c-subset")};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_FALSE(treeHasRule(cu.trees()[0], "castExpr"));
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "parenExpr"));
    EXPECT_EQ(cu.typeNameReparseCount(), 0u);
}

// Nested includes (header includes header): the oracle harvests ALL
// trees in the CU, so a typedef two include-hops away still resolves.
// (This is deliberately MORE generous than the semantic injection's
// per-edge propagation — see the probe note in the implementation
// report; the harvest union covers the transitive case by design.)
TEST(TypeNameOracle, TransitiveIncludeTypedefResolvesInline) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "#include \"outer.h\"\n"
        "int main() { return (DeepT)-1; }\n");
    dir.write("outer.h", "#include \"inner.h\"\nint helper() { return 0; }\n");
    dir.write("inner.h", "typedef int DeepT;\n");

    UnitBuilder b{loadShippedSchema("c-subset")};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "castExpr"));
    EXPECT_EQ(cu.typeNameReparseCount(), 0u);
}

// Both casts ambiguous: helper.h (defines HelpT, casts to MainT) is inlined
// into main.c (defines MainT). After the splice, helper.h's `(MainT)-v` cast
// appears in the buffer BEFORE `typedef int MainT;` (the includer's own line),
// so on the FIRST parse `MainT` is a forward reference -> the oracle DOES fire
// here (one reparse) to resolve it from the union of in-buffer type names.
// This is the residual case where the FC2 oracle still earns its keep AFTER
// FC13: an in-buffer FORWARD reference across an inline-include boundary. One
// tree, exactly one reparse, both casts resolved.
TEST(TypeNameOracle, BothDirectionsResolveInlineInOneBuffer) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "#include \"helper.h\"\n"
        "typedef int MainT;\n"
        "int main() { return (HelpT)-1; }\n");
    dir.write("helper.h",
              "typedef int HelpT;\n"
              "int helper(int v) { return (MainT)-v; }\n");

    UnitBuilder b{loadShippedSchema("c-subset")};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "castExpr"));
    EXPECT_EQ(cu.typeNameReparseCount(), 1u)
        << "an in-buffer forward reference across an inline-include boundary"
           " still uses the oracle";
}

// ── D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION ─────────────────────────────────────
//
// A shipped SYSTEM descriptor's typedefs are injected SEMANTICALLY (post-parse),
// so an angle `#include <h>` does NOT splice them into the buffer — pre-fix, the
// includer parsed `(size_t)(expr)` as a CALL, recorded an
// AmbiguousTypeNameCandidate, and UnitBuilder::finish() re-tokenized + re-parsed
// the WHOLE TU (~0.75s/TU on SQLite) to learn the name is a type. The fix seeds
// the LIVE resolved-descriptor typedef NAMES into the binder sketch BEFORE the
// first parse (parseAndAdd_ harvests pp.resolvedShippedDescriptors), so the cast
// commits on parse 1 with NO candidate → NO reparse.

// (a) EFFECTIVENESS pin: a shipped-typedef cast `(MyT)(0)` (the `(size_t)(x)`
// offsetof shape) commits on the FIRST parse → typeNameReparseCount() == 0.
// RED-ON-DISABLE: revert the parseAndAdd_ seed harvest → the shipped typedef is
// Unknown at first parse → a candidate is recorded → finish() reparses → the
// count flips to 1 (the cast still commits via the reparse, so `castExpr` stays;
// the COUNT is the pin).
TEST(TypeNameOracle, ShippedTypedefCastSeededOnFirstParseNoReparse) {
    TempDir srcDir;
    TempDir sysDir;
    auto main = srcDir.write(
        "main.c",
        "#include <mydefs.h>\n"
        "int main() { return (MyT)(0); }\n");
    // A NEUTRAL descriptor: the typedef `MyT` is a typed surface injected
    // semantically, NOT spliced into the buffer like a quote include's text.
    sysDir.write(
        "mydefs.json",
        R"({ "header": "mydefs.h",
             "typedefs": [ { "name": "MyT", "type": "i32" } ] })");

    UnitBuilder b{loadShippedSchema("c-subset")};
    b.addSystemDir(sysDir.path());
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "castExpr"))
        << "the seeded shipped typedef must commit the cast on the first parse";
    EXPECT_EQ(cu.typeNameReparseCount(), 0u)
        << "seeding the shipped typedef NAME eliminates the full-file reparse";
    // The descriptor path is still recorded for semantic injection (the seed is
    // additive, not a replacement for the import-resolver record).
    EXPECT_EQ(cu.shippedLibDescriptors().size(), 1u);
}

// (b) Opt-4 PARITY: the SAME cast `(MyT)(0)`, resolved two ways — once by the
// FIRST-PARSE SEED (shipped descriptor, reparse 0), once by the finish() oracle
// REPARSE (in-buffer forward reference, reparse 1) — must produce a
// STRUCTURALLY IDENTICAL cast subtree. This empirically pins the one-directional
// property: seeding resolves the type EARLIER (fewer reparses) but yields the
// EXACT tree the reparse would have — it never diverges, suppresses, or
// reshapes. RED-ON-DISABLE: a seed that mis-committed (e.g. a different operand
// grouping) would make the two signatures differ.
TEST(TypeNameOracle, SeededFirstParseCastMatchesReparseCast) {
    // Path A — resolved by the FIRST-PARSE SEED (shipped typedef), no reparse.
    TempDir seedSrc;
    TempDir seedSys;
    auto seedMain = seedSrc.write(
        "main.c",
        "#include <mydefs.h>\n"
        "int f() { return (MyT)(0); }\n");
    seedSys.write(
        "mydefs.json",
        R"({ "header": "mydefs.h",
             "typedefs": [ { "name": "MyT", "type": "i32" } ] })");
    UnitBuilder seedB{loadShippedSchema("c-subset")};
    seedB.addSystemDir(seedSys.path());
    seedB.addFile(seedMain);
    auto seedCu = std::move(seedB).finish();

    // Path B — the SAME cast, but `MyT` is an in-buffer FORWARD reference (typedef
    // AFTER the use), so the seed cannot cover it and the finish() oracle reparse
    // resolves it from the union of in-buffer global type names.
    TempDir reparseSrc;
    auto reparseMain = reparseSrc.write(
        "main.c",
        "int f() { return (MyT)(0); }\n"
        "typedef int MyT;\n");
    UnitBuilder reparseB{loadShippedSchema("c-subset")};
    reparseB.addFile(reparseMain);
    auto reparseCu = std::move(reparseB).finish();

    ASSERT_EQ(seedCu.trees().size(), 1u);
    ASSERT_EQ(reparseCu.trees().size(), 1u);
    EXPECT_FALSE(seedCu.trees()[0].diagnostics().hasErrors());
    EXPECT_FALSE(reparseCu.trees()[0].diagnostics().hasErrors());

    // The distinguishing precondition: the two paths reached the cast by
    // DIFFERENT mechanisms (seed vs reparse).
    EXPECT_EQ(seedCu.typeNameReparseCount(), 0u)
        << "path A resolves the cast via the first-parse seed";
    EXPECT_EQ(reparseCu.typeNameReparseCount(), 1u)
        << "path B resolves the cast via the retained oracle reparse";

    // The parity: identical cast subtree structure + spelling.
    const std::string seedSig    = firstCastExprSig(seedCu.trees()[0]);
    const std::string reparseSig = firstCastExprSig(reparseCu.trees()[0]);
    EXPECT_FALSE(seedSig.empty()) << "path A must contain a castExpr";
    EXPECT_FALSE(reparseSig.empty()) << "path B must contain a castExpr";
    EXPECT_EQ(seedSig, reparseSig)
        << "a cast resolved by the first-parse seed must be structurally"
           " identical to the same cast resolved by the oracle reparse";
}
