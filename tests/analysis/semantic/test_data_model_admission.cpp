// P69 fold 3f (lane `xa`, review-xa3e F3): THE POINTER WIDTH'S HOME AND THE ONE ADMISSIBILITY PREDICATE.
//
// Two quantities decide whether a program's widths can be lowered at all, and each has ONE home:
//   * the pointer width, `scalarByteSize(TypeKind::Ptr, dm)` (core/types/type_lattice/type_layout.hpp), which
//     answers NONE for a value of `DataModel` that names no model -- `DataModel` is an `enum class : std::uint8_t`,
//     so the zero sentinel and every unlisted value of its underlying type are values of it -- where it used to
//     answer a literal 8 ("unreachable for a valid model");
//   * whether the back end lowers that width, `dataModelTheBackEndCannotLower(dm)`: a model is admitted when it
//     states a pointer size and that size is the back end's address width (`kBackEndAddressBytes`).
// The semantic gate asks the predicate, where it used to ask `dataModel == DataModel::Ilp32`; so a value naming no
// model is refused by the same question as ILP32, and S_UnsupportedDataModel says why.
//
// THE UNIVERSE: every row of the data-model table (the closed vocabulary every shipped document's `dataModel` is
// spelled from) plus two values that name no model (the zero sentinel and one past every row), through the home,
// the predicate and the semantic gate. The verdict of each row is computed from the home (a stated pointer size
// equal to the back end's address width), never typed per row.
//
// RED-ON-DISABLE (each an arm of tests/mutations/arms.registry). Each red set is the ONE TEST that holds the mutated
// question to a verdict of its own; the other two compare with the mutated answer itself and agree with it:
//   * the home's `return std::nullopt` back to `return 8` -> ThePointerWidthsHomeStatesNoneOutsideItsTable's
//     unlisted cells;
//   * the predicate's width comparison disabled -> EveryModelIsJudgedByOnePredicate's ILP32 cell (the rule refuses
//     it) and its "a listed model the back end cannot lower" check;
//   * the gate back to its identity test of ILP32 -> TheSemanticGateRefusesWhatThePredicateRefusesAndSaysWhy's
//     unlisted cells (admitted in silence) and its ILP32 text (the model's name, not the reason).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

struct ModelCell {
    std::string label;
    DataModel   model;
    bool        listed;   // a row of the data-model table
};

// The universe: each table row, then two values of the enum's underlying type that name no model.
[[nodiscard]] std::vector<ModelCell> modelCells() {
    std::vector<ModelCell> cells;
    std::uint8_t           past = 0;
    for (auto const& [value, name] : kDataModelTable.rows) {
        cells.push_back({std::string{name}, value, true});
        past = std::max<std::uint8_t>(past, static_cast<std::uint8_t>(value));
    }
    cells.push_back({"the zero sentinel", static_cast<DataModel>(0), false});
    cells.push_back({"one past every row", static_cast<DataModel>(past + 1), false});
    return cells;
}

// The verdict the rule gives, computed from the home: admitted iff the model states a pointer size and it is the
// back end's address width.
[[nodiscard]] bool admittedByTheRule(DataModel dm) {
    std::optional<std::uint64_t> const bytes = scalarByteSize(TypeKind::Ptr, dm);
    return bytes.has_value() && *bytes == kBackEndAddressBytes;
}

}  // namespace

// The home answers each listed model's pointer size and NONE for a value that names no model.
TEST(DataModelAdmission, ThePointerWidthsHomeStatesNoneOutsideItsTable) {
    std::size_t listed = 0;
    for (ModelCell const& c : modelCells()) {
        SCOPED_TRACE(c.label);
        std::optional<std::uint64_t> const bytes = scalarByteSize(TypeKind::Ptr, c.model);
        if (c.listed) {
            ++listed;
            ASSERT_TRUE(bytes.has_value()) << "a model of the table states its pointer size";
            EXPECT_TRUE(*bytes == 4 || *bytes == 8) << *bytes;
        } else {
            EXPECT_FALSE(bytes.has_value()) << "a value that names no model states no pointer size, never a literal";
        }
    }
    EXPECT_EQ(listed, kDataModelTable.rows.size());
    EXPECT_GE(listed, 3u) << "the universe reads the table: LP64, LLP64 and ILP32 at least";
}

// The predicate admits exactly what the rule admits, and says why it refuses the rest.
TEST(DataModelAdmission, EveryModelIsJudgedByOnePredicate) {
    bool sawAdmitted = false;
    bool sawNarrow   = false;
    for (ModelCell const& c : modelCells()) {
        SCOPED_TRACE(c.label);
        std::optional<std::string> const refusal = dataModelTheBackEndCannotLower(c.model);
        EXPECT_EQ(!refusal.has_value(), admittedByTheRule(c.model));
        if (!refusal.has_value()) {
            sawAdmitted = true;
            continue;
        }
        if (!c.listed) {
            EXPECT_NE(refusal->find("states no pointer size"), std::string::npos) << *refusal;
            EXPECT_NE(refusal->find("no table row names"), std::string::npos) << *refusal;
        } else {
            sawNarrow = true;
            EXPECT_NE(refusal->find("data model " + c.label), std::string::npos) << *refusal;
            EXPECT_NE(refusal->find("-byte pointers, and the back end lowers 8-byte addresses only"),
                      std::string::npos)
                << *refusal;
        }
    }
    EXPECT_TRUE(sawAdmitted) << "the universe holds an admitted model";
    EXPECT_TRUE(sawNarrow) << "the universe holds a listed model the back end cannot lower (ILP32)";
}

// The semantic gate refuses what the predicate refuses -- one S_UnsupportedDataModel carrying the predicate's
// reason -- and says nothing of an admitted model.
TEST(DataModelAdmission, TheSemanticGateRefusesWhatThePredicateRefusesAndSaysWhy) {
    for (ModelCell const& c : modelCells()) {
        SCOPED_TRACE(c.label);
        auto cu = buildShippedUnit("c", {std::string{"int main(void) { return 0; }\n"}});
        assertNoBuilderErrors(*cu);
        SemanticModel const m = analyze(cu, DiagnosticBudget::libraryDefault(), c.model);
        std::optional<std::string> const refusal = dataModelTheBackEndCannotLower(c.model);
        std::vector<std::string>         said;
        for (ParseDiagnostic const& d : m.diagnostics().all()) {
            if (d.code == DiagnosticCode::S_UnsupportedDataModel) said.push_back(d.actual);
        }
        if (!refusal.has_value()) {
            EXPECT_TRUE(said.empty()) << "an admitted model is not refused";
            continue;
        }
        ASSERT_EQ(said.size(), 1u) << "a model the back end cannot lower is refused once";
        EXPECT_EQ(said[0], *refusal) << "the gate says the predicate's reason";
        EXPECT_TRUE(m.hasErrors());
    }
}
