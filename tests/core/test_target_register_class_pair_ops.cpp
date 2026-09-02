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
#include <optional>
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
// ⚠ NOT A GAP TO BE FILLED BY A DEFAULT. A `Flags` pair has no data to move at
// all, and `vr` has no members on either shipped target, so neither can borrow
// a neighbour's opcode — both must answer nullopt.
//
// ★ THE fpr↔vr PAIR IS STILL UNDECLARED, BUT FOR A DIFFERENT REASON THAN THIS
// COMMENT USED TO GIVE, AND THE REASON IS THE FINDING. It read: "arm64's `vr`
// and `fpr` are two WIDTH VIEWS of ONE physical register file (all 32 pairs
// share a `dwarfNumber`), so a 'move' between them would be a fake copy
// papering over a config defect; the fix is to declare that file ONCE." ✔That
// fix LANDED — R1 of design A′ — so arm64 declares no `vr` register at all and
// the pair is undeclared because one side of it is EMPTY, not because a real
// pair was deliberately withheld. The nullopt is the same; what it means is
// not.
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

// ── D-TARGET-REGISTER-CLASS-OPS-HAVE-NO-LONG-REACH-MEMORY-FORM ──────────────
//
// ★★★ THE SECOND SLOT PAIR ON THIS TABLE, AND IT EXISTS FOR THE REASON THE
// FIRST ONE DID: the machine had the instruction and the table had no home for
// it. AArch64 spells a memory access two ways — the UNSCALED `LDUR/STUR
// [Xn,#simm9]` (±256) and the SCALED unsigned-offset `LDR/STR [Xn,#pimm]`
// (0..4095×accessSize) — and WHICH one an access takes is a property of the
// OFFSET, decided at the frame chokepoint, not of the instruction the lowering
// asked for. So the long-reach form has to be reachable FROM the short one,
// per class.
//
// ⚠⚠ WHAT WENT WRONG WITHOUT IT, AND WHY IT WAS INVISIBLE. The scaled twin was
// declared for the INTEGER file alone, as a pair of `load_u`/`store_u` handles
// resolved by MNEMONIC, and the chokepoint swapped only when the op it was
// handed WAS the universal GPR one. That reads as a safety gate and is really a
// vocabulary gap: `fldur`/`fstur` had no twin because nobody had declared one,
// not because AArch64 lacks the form. The moment the SIMD&FP file was declared
// once and the frame stride became 16, FP frame slots crossed ±256 and the
// compiler REFUSED to build a shipped corpus example.
//
// ★★ EVERY NEW SCHEMA SURFACE OWES A LOAD-TIME REFUSAL, and this one owes
// three, because a scaled twin is never named by a lowering pass — it is
// SUBSTITUTED — so all three failure shapes load clean and do nothing visible:
//   * a twin with no short form to be substituted FOR (dead vocabulary);
//   * HALF the pair on a class that owns both short forms (one direction of a
//     slot encodes, the other fails loud, on the same frame);
//   * a twin whose ENCODING moves a different register bank than the row it is
//     filed under — the silent wrong answer, and the reason the memory verbs
//     are now bank-checked exactly as `move` already was.

// ── The positive control for the long-reach slots ───────────────────────────
//
// ⚠ ANTI-VACUITY MATTERS MORE HERE THAN ANYWHERE ELSE IN THIS FILE: `nullopt`
// is the CORRECT answer for a class with no twin, so a suite that only checked
// "nullopt where undeclared" would pass over a target that had lost every
// binding. This arm refuses to pass unless SOME shipped class really declares
// the pair.
TEST(TargetRegisterClassPairOps, SomeShippedClassDeclaresTheLongReachPair) {
    bool anyDeclared = false;
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto schema = TargetSchema::loadShipped(t);
        ASSERT_TRUE(schema.has_value());
        auto const& s = **schema;
        for (auto const cls : {TargetRegClass::GPR, TargetRegClass::FPR,
                               TargetRegClass::VR, TargetRegClass::Flags}) {
            auto const ls = s.regClassOpOpcode(cls, RegClassOp::LoadScaled);
            auto const ss = s.regClassOpOpcode(cls, RegClassOp::StoreScaled);
            if (!ls.has_value() && !ss.has_value()) continue;
            anyDeclared = true;
            // Both halves, always — the pairing rule, observed through the
            // accessor rather than argued from the loader.
            EXPECT_TRUE(ls.has_value());
            EXPECT_TRUE(ss.has_value());
            // And a twin is a DIFFERENT opcode from its short form. A config
            // that pointed both at one mnemonic would read as declared and
            // leave the out-of-reach offset exactly where it was.
            EXPECT_NE(ls, s.regClassOpOpcode(cls, RegClassOp::Load));
            EXPECT_NE(ss, s.regClassOpOpcode(cls, RegClassOp::Store));
        }
    }
    EXPECT_TRUE(anyDeclared)
        << "no shipped target declares a scaled long-reach memory form at all "
           "— every 'undeclared resolves to nullopt' assertion in this file "
           "would then be passing for the wrong reason";
}

// ── arm64 declares the pair for BOTH its register files ─────────────────────
//
// The integer file gets its twin from the universal `load_u`/`store_u`
// no-row default; the SIMD&FP file names `fldr_u`/`fstr_u` on its own row.
// The asymmetry is `move`'s, not a new one — but the RESULT must be
// symmetric, because a frame does not care which file a slot belongs to.
TEST(TargetRegisterClassPairOps, Arm64GivesBothRegisterFilesALongReachForm) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto const& s = **schema;
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, RegClassOp::LoadScaled),
              s.opcodeByMnemonic("load_u"));
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, RegClassOp::StoreScaled),
              s.opcodeByMnemonic("store_u"));
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, RegClassOp::LoadScaled),
              s.opcodeByMnemonic("fldr_u"));
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, RegClassOp::StoreScaled),
              s.opcodeByMnemonic("fstr_u"));
}

// ── x86_64 declares NONE, and that is the right answer ──────────────────────
//
// Its memory forms already carry a disp32, so no offset a frame can produce
// overruns them. The agnosticism claim is exactly this: the feature is absent
// because the CONFIG does not declare it, with no code anywhere asking which
// target is running.
TEST(TargetRegisterClassPairOps, X8664DeclaresNoLongReachFormAtAll) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const& s = **schema;
    for (auto const cls : {TargetRegClass::GPR, TargetRegClass::FPR,
                           TargetRegClass::VR, TargetRegClass::Flags}) {
        EXPECT_EQ(s.regClassOpOpcode(cls, RegClassOp::LoadScaled), std::nullopt);
        EXPECT_EQ(s.regClassOpOpcode(cls, RegClassOp::StoreScaled), std::nullopt);
    }
    EXPECT_EQ(s.opcodeByMnemonic("load_u"), std::nullopt);
    EXPECT_EQ(s.opcodeByMnemonic("store_u"), std::nullopt);
    EXPECT_EQ(s.opcodeByMnemonic("fldr_u"), std::nullopt);
    EXPECT_EQ(s.opcodeByMnemonic("fstr_u"), std::nullopt);
}

// ── The long-reach slots are DIAGONAL-ONLY, like the forms they replace ─────
TEST(TargetRegisterClassPairOps, LongReachMemoryOpsNeverResolveAcrossAPair) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto schema = TargetSchema::loadShipped(t);
        ASSERT_TRUE(schema.has_value());
        auto const& s = **schema;
        for (auto const op : {RegClassOp::LoadScaled, RegClassOp::StoreScaled}) {
            EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR,
                                         TargetRegClass::FPR, op),
                      std::nullopt);
            EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR,
                                         TargetRegClass::GPR, op),
                      std::nullopt);
        }
    }
}

// ── Load-time refusal: a twin with no short form to substitute for ──────────
//
// ★ THE REMOVE DIRECTION. The mutant DELETES the `load` key from a shipped row
// that already carries `loadScaled`, rather than adding a key — an added key
// is a shape no shipped config has, and a fixture that only ever adds cannot
// tell a LIVE key from dead config.
TEST(TargetRegisterClassPairOps, ATwinWithoutItsShortFormIsLoadTimeFatal) {
    for (char const* const field : {"load", "store"}) {
        SCOPED_TRACE(field);
        auto mutated = mutateShippedTargetSchemaDoc(
            "arm64", [field](nlohmann::json& doc) {
                for (auto& row : doc["registerClassOps"]) {
                    if (row.value("class", std::string{}) != "fpr") continue;
                    if (row.contains("to")) continue;   // the diagonal row only
                    row.erase(field);
                }
            });
        EXPECT_FALSE(mutated.has_value())
            << "a scaled twin is SUBSTITUTED for the short form, never named — "
               "with the short form deleted it can never be selected, and a "
               "key that loads clean and does nothing reads like a declaration";
    }
}

// ── Load-time refusal: half the long-reach pair ─────────────────────────────
//
// ★ ALSO THE REMOVE DIRECTION, and this is the one that guards a frame whose
// two directions disagree: with `storeScaled` gone, a big-frame FP spill STORE
// fails loud while its RELOAD encodes — on the SAME slot.
TEST(TargetRegisterClassPairOps, HalfTheLongReachPairIsLoadTimeFatal) {
    for (char const* const field : {"loadScaled", "storeScaled"}) {
        SCOPED_TRACE(field);
        auto mutated = mutateShippedTargetSchemaDoc(
            "arm64", [field](nlohmann::json& doc) {
                for (auto& row : doc["registerClassOps"]) {
                    if (row.value("class", std::string{}) != "fpr") continue;
                    if (row.contains("to")) continue;
                    row.erase(field);
                }
            });
        EXPECT_FALSE(mutated.has_value())
            << "a class owning both short forms must declare both long ones or "
               "neither — half a pair is a frame that encodes in one direction "
               "and refuses in the other";
    }
}

// ── Load-time refusal: a memory verb whose ENCODING banks disagree ──────────
//
// ★★★ THE ONE THAT GUARDS A SILENT WRONG ANSWER RATHER THAN A DEAD KEY, and
// the reason it became load-bearing NOW: the chokepoint swaps PER CLASS, by
// opcode identity. Filing the INTEGER `load_u` as the fpr class's `loadScaled`
// would resolve happily, and a large-frame FP reload would emit `LDR Xt` —
// landing the float in the integer register of the same hwEncoding, rc=0, no
// diagnostic. That is D-LIR-ASM-OPERAND-MOVE-IS-CLASS-BLIND rebuilt one role
// over, so the memory verbs are bank-checked exactly as `move` already was.
TEST(TargetRegisterClassPairOps, AMemoryVerbWhoseEncodingBanksDisagreeIsLoadTimeFatal) {
    struct Case { char const* field; char const* wrongBankMnemonic; };
    for (auto const& c : {Case{"loadScaled",  "load_u"},
                          Case{"storeScaled", "store_u"},
                          Case{"load",        "load"},
                          Case{"store",       "store"}}) {
        SCOPED_TRACE(c.field);
        auto mutated = mutateShippedTargetSchemaDoc(
            "arm64", [&c](nlohmann::json& doc) {
                for (auto& row : doc["registerClassOps"]) {
                    if (row.value("class", std::string{}) != "fpr") continue;
                    if (row.contains("to")) continue;
                    row[c.field] = c.wrongBankMnemonic;
                }
            });
        EXPECT_FALSE(mutated.has_value())
            << "an INTEGER memory verb filed under the fpr class resolves and "
               "then writes the register number into the wrong file — the "
               "banks must be checked where the config is judged";
    }
}

// ── The refusal is about the BANK, not about the key being unusual ──────────
//
// ⚠ THE CONTROL FOR THE ARM ABOVE. Every one of those mutants also swapped a
// mnemonic, so a loader that refused ANY change to those keys would produce
// the same four reds for the wrong reason. Re-declaring the SHIPPED mnemonic
// under a `$comment` edit must still LOAD — the mutation is real (the helper
// throws on a no-op) and the schema is still correct.
TEST(TargetRegisterClassPairOps, ACorrectlyBankedMemoryVerbStillLoads) {
    auto mutated = mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            for (auto& row : doc["registerClassOps"]) {
                if (row.value("class", std::string{}) != "fpr") continue;
                if (row.contains("to")) continue;
                // Restate the SHIPPED bindings verbatim + a prose key: the
                // document differs, the meaning does not.
                row["loadScaled"]  = "fldr_u";
                row["storeScaled"] = "fstr_u";
                row["$bankControlComment"] = "correctly banked; must load";
            }
        });
    ASSERT_TRUE(mutated.has_value())
        << "a correctly-banked long-reach pair must LOAD — otherwise the four "
           "bank refusals above are reds about the key, not about the bank";
    EXPECT_EQ((*mutated)->regClassOpOpcode(TargetRegClass::FPR,
                                           RegClassOp::LoadScaled),
              (*mutated)->opcodeByMnemonic("fldr_u"));
}

// ── Load-time refusal: one memory mnemonic claimed by two register files ────
//
// ★★★ A SORT-ORDER RULE, NOT A TIDINESS ONE. The frame chokepoint finds a
// short form's long-reach twin by looking the OPCODE up in a per-class table.
// If two classes named one mnemonic as their `load`, WHICH twin an
// out-of-reach access got would be decided by which row the walk reached
// first — exactly
// [[feedback-an-opener-closer-duplicate-must-never-be-settled-by-sort-order]],
// and the wrong answer it produces is an access encoded against the other
// register file.
//
// ⚠ THE FIXTURE USES A BANK-AGNOSTIC OPCODE ON PURPOSE. The memory-verb bank
// check already refuses a mnemonic whose encoding DECLARES the wrong file, so
// a fixture naming `fldur` under `vr` would red for that reason and this rule
// would never be exercised. Stripping the encoding's bank declarations first
// leaves the collision as the only thing left to refuse.
TEST(TargetRegisterClassPairOps, TwoClassesClaimingOneMemoryVerbIsLoadTimeFatal) {
    auto mutated = mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            // Make `fldur`/`fstur` bank-agnostic so only the collision is left.
            for (auto& o : doc["opcodes"]) {
                auto const m = o.value("mnemonic", std::string{});
                if (m != "fldur" && m != "fstur") continue;
                if (!o.contains("encoding")) continue;
                for (auto& v : o["encoding"]["variants"]) {
                    v.erase("resultRegClass");
                    if (!v.contains("wires")) continue;
                    for (auto& w : v["wires"]) w.erase("regClass");
                }
            }
            // A SECOND class naming the SAME two memory verbs.
            doc["registerClassOps"].push_back(
                {{"class", "vr"}, {"load", "fldur"}, {"store", "fstur"}});
        });
    EXPECT_FALSE(mutated.has_value())
        << "one memory verb cannot belong to two register files — the "
           "chokepoint's per-class lookup would settle the twin by row order";
}
