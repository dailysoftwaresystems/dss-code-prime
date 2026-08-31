// ── D-TARGET-NO-CROSS-CLASS-MOVE-VERB — the `registerClassOps` CLASS PAIR ───
//
// ★★★ WHAT CHANGED AND WHY THERE IS ONE TABLE RATHER THAN TWO. Copying a bit
// pattern BETWEEN two register files (aarch64 `fmov x0, d0`, x86_64
// `movq %xmm0, %rax`) is a distinct machine operation from a copy WITHIN one,
// and `registerClassOps` was indexed by ONE class — so the cross-product had
// nowhere to live. The vocabulary was never missing: `movq_gpr_to_xmm` and
// `movq_xmm_to_gpr` ship byte-pinned on BOTH targets. The SLOT was.
//
// A row's subject is now an ORDERED PAIR: `class` is the SOURCE, the optional
// `to` is the DESTINATION, and `to` omitted is the DIAGONAL — which is what
// every row written before the key existed meant. The same-class move sits at
// the diagonal of the same table, so the question *"how does this machine copy
// a bit pattern from class A to class B?"* has exactly ONE home for every
// (A,B). A separate cross-class list would have given it two, and a reader
// would have found whichever their grep reached first.
//
// ★★ EVERY NEW SCHEMA SURFACE OWES A LOAD-TIME REFUSAL, and each one below is
// a config error that would otherwise load clean and then do something wrong
// or nothing at all:
//   * a `to` naming no class, or naming the no-class sentinel;
//   * a `{class, to}` pair declared TWICE — the second row would be dropped or
//     would overwrite, and which one won would be an artefact of file order;
//   * `load`/`store` on a CROSS-class row — memory is not a register class, so
//     "load from gpr into fpr" asks nothing and could never resolve;
//   * a `move` whose own ENCODING names different banks than the row it is
//     filed under. ★★★ THIS IS THE LOAD-BEARING ONE: filing the integer `mov`
//     as the gpr->fpr move would load clean, resolve, and emit an integer move
//     for a cross-FILE copy — re-creating D-LIR-ASM-OPERAND-MOVE-IS-CLASS-BLIND
//     (a register NUMBER written into the wrong file, silently) through the
//     very table that exists to prevent it.
//
// ★ EVERY FIXTURE MUTATES THE SHIPPED FILE IN MEMORY rather than authoring a
//   parallel broken JSON, so each gate is exercised against the shape the
//   loader really receives. `mutateShippedTargetSchemaDoc` is the existing
//   helper for exactly this, and it THROWS on a mutation that matched nothing
//   — the fail-closed contract in its own header.

#include "core/types/target_schema.hpp"

#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace {

using ::dss::RegClassOp;
using ::dss::TargetRegClass;
using ::dss::TargetSchema;
using ::dss::test_support::mutateShippedTargetSchemaDoc;

// The two shipped targets, named once. Both declare the same two cross-class
// rows out of their own byte-pinned opcodes, so every claim here is made twice
// — a fact measured on ONE leg is a portability claim.
constexpr char const* kTargets[] = {"x86_64", "arm64"};

// Append a row to `registerClassOps`, returning the document. Used for the
// negative fixtures; the shipped rows are left exactly as they are so the
// pre-existing pins in `tests/asm/test_asm_substrate.cpp` that address row 0
// keep their subject.
void appendRow(nlohmann::json& doc, nlohmann::json row) {
    doc["registerClassOps"].push_back(std::move(row));
}

} // namespace

// ── The positive control ────────────────────────────────────────────────────
//
// If this fails, every negative below is testing a schema that was already
// broken — and the whole file would be asserting nothing.
TEST(TargetRegisterClassPairOps, BothShippedTargetsDeclareBothDirections) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto schema = TargetSchema::loadShipped(t);
        ASSERT_TRUE(schema.has_value());
        auto const& s = **schema;

        EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, TargetRegClass::FPR,
                                     RegClassOp::Move),
                  s.opcodeByMnemonic("movq_gpr_to_xmm"));
        EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, TargetRegClass::GPR,
                                     RegClassOp::Move),
                  s.opcodeByMnemonic("movq_xmm_to_gpr"));

        // ⚠ AND THEY MUST BE DISTINCT FROM BOTH DIAGONALS. If a cross-class
        // cell resolved to a diagonal move, every consumer assertion in the
        // suite would pass over the exact wrong-file write this row closes.
        auto const gprMove = s.regClassOpOpcode(TargetRegClass::GPR,
                                                RegClassOp::Move);
        auto const fprMove = s.regClassOpOpcode(TargetRegClass::FPR,
                                                RegClassOp::Move);
        ASSERT_TRUE(gprMove.has_value());
        ASSERT_TRUE(fprMove.has_value());
        EXPECT_NE(s.regClassOpOpcode(TargetRegClass::GPR, TargetRegClass::FPR,
                                     RegClassOp::Move), gprMove);
        EXPECT_NE(s.regClassOpOpcode(TargetRegClass::GPR, TargetRegClass::FPR,
                                     RegClassOp::Move), fprMove);
        EXPECT_NE(s.regClassOpOpcode(TargetRegClass::FPR, TargetRegClass::GPR,
                                     RegClassOp::Move), gprMove);
        EXPECT_NE(s.regClassOpOpcode(TargetRegClass::FPR, TargetRegClass::GPR,
                                     RegClassOp::Move), fprMove);

        // The two directions are DIFFERENT opcodes, deliberately: the variant
        // guard keys only on (operandKinds, width) and both are `reg` at the
        // same width, so one opcode carrying both would silently pick one.
        EXPECT_NE(s.regClassOpOpcode(TargetRegClass::GPR, TargetRegClass::FPR,
                                     RegClassOp::Move),
                  s.regClassOpOpcode(TargetRegClass::FPR, TargetRegClass::GPR,
                                     RegClassOp::Move));
    }
}

// ── The diagonal shorthand IS the diagonal cell, not a second lookup path ───
TEST(TargetRegisterClassPairOps, TheTwoArgFormIsExactlyTheDiagonal) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto schema = TargetSchema::loadShipped(t);
        ASSERT_TRUE(schema.has_value());
        auto const& s = **schema;
        for (auto const cls : {TargetRegClass::GPR, TargetRegClass::FPR,
                               TargetRegClass::VR, TargetRegClass::Flags}) {
            for (auto const op : {RegClassOp::Move, RegClassOp::Load,
                                  RegClassOp::Store}) {
                EXPECT_EQ(s.regClassOpOpcode(cls, op),
                          s.regClassOpOpcode(cls, cls, op))
                    << "the terse form must resolve to the SAME cell as the "
                       "explicit diagonal — a shorthand, never a second table";
            }
        }
    }
}

// ── An UNDECLARED pair resolves to nothing, so the consumer fails loud ──────
//
// ⚠ NOT A GAP TO BE FILLED BY A DEFAULT. arm64's `vr` and `fpr` are two WIDTH
// VIEWS of ONE physical register file (all 32 pairs share a `dwarfNumber`), so
// a "move" between them would be a fake copy papering over a config defect;
// the fix is to declare that file ONCE. And a `Flags` pair has no data to move
// at all. Both must answer nullopt rather than borrowing a neighbour.
TEST(TargetRegisterClassPairOps, AnUndeclaredPairResolvesToNothing) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto schema = TargetSchema::loadShipped(t);
        ASSERT_TRUE(schema.has_value());
        auto const& s = **schema;
        EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, TargetRegClass::VR,
                                     RegClassOp::Move), std::nullopt);
        EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::VR, TargetRegClass::FPR,
                                     RegClassOp::Move), std::nullopt);
        EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, TargetRegClass::Flags,
                                     RegClassOp::Move), std::nullopt);
        // ★ AND THE GPR→GPR UNIVERSAL DEFAULT IS **DIAGONAL-ONLY**. It exists
        // because every pre-FC2 lowering pass emitted `mov` unconditionally;
        // letting it answer an OFF-diagonal query would hand a cross-file copy
        // the integer move, which is the whole defect.
        EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, TargetRegClass::GPR,
                                     RegClassOp::Move),
                  s.opcodeByMnemonic("mov"));
    }
}

// ── `load`/`store` are DIAGONAL-ONLY, at the accessor as well as the loader ─
TEST(TargetRegisterClassPairOps, MemoryOpsNeverResolveAcrossAPair) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto schema = TargetSchema::loadShipped(t);
        ASSERT_TRUE(schema.has_value());
        auto const& s = **schema;
        for (auto const op : {RegClassOp::Load, RegClassOp::Store}) {
            EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR,
                                         TargetRegClass::FPR, op),
                      std::nullopt)
                << "memory is not a register class — there is no 'load from "
                   "gpr into fpr', and a hand-built schema must not be able to "
                   "reach a memory mnemonic through a cross-class query";
            EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR,
                                         TargetRegClass::GPR, op),
                      std::nullopt);
        }
    }
}

// ── Load-time refusal: a `to` that names nothing ────────────────────────────
TEST(TargetRegisterClassPairOps, AToNamingNoClassIsLoadTimeFatal) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto mutated = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            appendRow(doc, {{"class", "gpr"},
                            {"to", "made-up-class"},
                            {"move", "mov"}});
        });
        EXPECT_FALSE(mutated.has_value())
            << "a destination class that resolves to nothing must be refused "
               "where the config is judged";
    }
}

// ── Load-time refusal: a `to` naming the no-class sentinel ──────────────────
//
// The sentinel SPELLS correctly, so a name lookup accepts it — the same hazard
// `isOperableTargetRegClass` was written for on the `class` side. Both ends of
// the pair go through ONE resolver so neither can drift from the other.
TEST(TargetRegisterClassPairOps, AToNamingTheNoClassSentinelIsLoadTimeFatal) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto mutated = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            appendRow(doc, {{"class", "gpr"}, {"to", "none"}, {"move", "mov"}});
        });
        EXPECT_FALSE(mutated.has_value())
            << "'none' is the no-class sentinel — a row landing in it could "
               "never fire, and it would occupy a real cell";
    }
}

// ── Load-time refusal: the same PAIR declared twice ─────────────────────────
TEST(TargetRegisterClassPairOps, ADuplicatePairIsLoadTimeFatal) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto mutated = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            // The gpr->fpr row already ships; declare it a second time.
            appendRow(doc, {{"class", "gpr"},
                            {"to", "fpr"},
                            {"move", "movq_gpr_to_xmm"}});
        });
        EXPECT_FALSE(mutated.has_value())
            << "two rows for one pair means which mnemonic wins is an "
               "artefact of file order";
    }
}

// ⚠ AND THE DIAGONAL MUST NOT HAVE BECOME A DIFFERENT CELL FROM THE SHORTHAND.
// A row spelled `{class: fpr, to: fpr}` addresses the SAME cell as the shipped
// `{class: fpr}` row, so it is a duplicate. If `to` were stored anywhere but
// the diagonal, this would load clean.
TEST(TargetRegisterClassPairOps, AnExplicitDiagonalDuplicatesTheTerseRow) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        // No `move` key at all: the DUPLICATE check runs before any mnemonic is
        // read, so a refusal here is attributable to the cell address alone and
        // cannot be an unresolvable-mnemonic refusal wearing its clothes (both
        // targets spell their fpr move differently — `movaps` vs `fmov`).
        auto mutated = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            appendRow(doc, {{"class", "fpr"}, {"to", "fpr"}});
        });
        EXPECT_FALSE(mutated.has_value())
            << "`to` equal to `class` IS the diagonal — a second row for it is "
               "a duplicate, and anything else means `to` is not being stored "
               "where the diagonal lives";
    }
}

// ── Load-time refusal: `load`/`store` on a cross-class row ──────────────────
TEST(TargetRegisterClassPairOps, MemoryOpsOnACrossClassRowAreLoadTimeFatal) {
    for (char const* const field : {"load", "store"}) {
        SCOPED_TRACE(field);
        for (char const* const t : kTargets) {
            SCOPED_TRACE(t);
            auto mutated = mutateShippedTargetSchemaDoc(
                t, [field](nlohmann::json& doc) {
                    // ⚠ A PAIR NEITHER TARGET DECLARES, so the DUPLICATE rule
                    // cannot fire first and lend this arm a refusal that is
                    // not the one under test.
                    nlohmann::json row{{"class", "gpr"}, {"to", "vr"}};
                    // Any DECLARED mnemonic — the point is the KEY's placement,
                    // not whether it resolves, so a resolvable name keeps the
                    // refusal attributable to the rule under test.
                    row[field] = "mov";
                    appendRow(doc, std::move(row));
                });
            EXPECT_FALSE(mutated.has_value())
                << "a memory op on a cross-class row loads clean and can never "
                   "resolve — dead vocabulary that reads like a declaration";
        }
    }
}

// ── Load-time refusal: a move whose ENCODING disagrees with its row ─────────
//
// ★★★ THE ONE THAT GUARDS A SILENT WRONG ANSWER RATHER THAN A DEAD KEY. `mov`
// is the INTEGER move: it reads a gpr and writes a gpr. Filed as the gpr->fpr
// move it would resolve happily and emit an integer move for a cross-FILE
// copy — the encoder then writes the register NUMBER into the integer file and
// the copy reads an unrelated register. That is
// D-LIR-ASM-OPERAND-MOVE-IS-CLASS-BLIND rebuilt inside its own fix, so the
// banks are checked at load.
TEST(TargetRegisterClassPairOps, AMoveWhoseEncodingBanksDisagreeIsLoadTimeFatal) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        // WRONG DESTINATION: `mov` lands in gpr, the row claims fpr.
        auto wrongDest = mutateShippedTargetSchemaDoc(
            t, [](nlohmann::json& doc) {
                for (auto& row : doc["registerClassOps"]) {
                    if (row.value("class", "") != "gpr") continue;
                    if (row.value("to", "") != "fpr") continue;
                    row["move"] = "mov";
                }
            });
        EXPECT_FALSE(wrongDest.has_value())
            << "the integer move filed as the gpr->fpr move would write the "
               "register number into the wrong file, silently";

        // WRONG SOURCE: `mov` reads gpr, the row claims fpr.
        auto wrongSrc = mutateShippedTargetSchemaDoc(
            t, [](nlohmann::json& doc) {
                for (auto& row : doc["registerClassOps"]) {
                    if (row.value("class", "") != "fpr") continue;
                    if (row.value("to", "") != "gpr") continue;
                    row["move"] = "mov";
                }
            });
        EXPECT_FALSE(wrongSrc.has_value())
            << "the integer move filed as the fpr->gpr move would read the "
               "register number out of the wrong file";
    }
}

// ── Load-time refusal: a cross-class move naming no opcode row ──────────────
TEST(TargetRegisterClassPairOps, ACrossClassMoveNamingNoOpcodeIsLoadTimeFatal) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto mutated = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            for (auto& row : doc["registerClassOps"]) {
                if (row.value("to", "") != "fpr") continue;
                row["move"] = "no_such_opcode";
            }
        });
        EXPECT_FALSE(mutated.has_value())
            << "at the consumer an unresolvable mnemonic is indistinguishable "
               "from a trigger-disciplined omission; load time is the one place "
               "the typo is visible as a typo";
    }
}

// ── The REMOVE-direction config mutant: drop the rows, lose the capability ──
//
// ★★ THIS IS THE ARM THAT CANNOT BE FAKED BY A C++ MUTANT. Every C++ mutant is
// blind to the difference between a live key and dead config — remove the rows
// and the accessor must answer nullopt, which is what makes every consumer
// downstream fail loud again. An ADD-direction mutant would stay green here.
TEST(TargetRegisterClassPairOps, RemovingTheCrossClassRowsLosesTheCapability) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto mutated = mutateShippedTargetSchemaDoc(t, [](nlohmann::json& doc) {
            auto& rows = doc["registerClassOps"];
            nlohmann::json kept = nlohmann::json::array();
            for (auto& row : rows) {
                if (row.contains("to")) continue;
                kept.push_back(row);
            }
            rows = std::move(kept);
        });
        ASSERT_TRUE(mutated.has_value())
            << "dropping the cross-class rows must still be a LOADABLE "
               "document — the capability is what disappears, not the target";
        auto const& s = **mutated;
        EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, TargetRegClass::FPR,
                                     RegClassOp::Move), std::nullopt);
        EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, TargetRegClass::GPR,
                                     RegClassOp::Move), std::nullopt);
        // The DIAGONAL survives — the mutant is surgical, which is what makes
        // a red downstream attributable to the cross-class rows alone.
        EXPECT_TRUE(s.regClassOpOpcode(TargetRegClass::FPR,
                                       RegClassOp::Move).has_value());
        EXPECT_TRUE(s.regClassOpOpcode(TargetRegClass::GPR,
                                       RegClassOp::Move).has_value());
    }
}
