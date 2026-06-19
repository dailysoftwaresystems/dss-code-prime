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

private:
    std::filesystem::path dir_;
};

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
