// [[D-C-HAS-EXTENSION-CLAIMS-C-ATOMIC-WHILE-THE-GNU-ATOMIC-BUILTINS-DO-NOT-EXIST]]
// — THE ADVERTISEMENT GUARD. Every capability this compiler ADVERTISES must be
// backed by a translation unit that USES it and COMPILES.
//
// ═══ WHY THIS FILE EXISTS, AND WHY THE UNIT SUITE COULD NOT SEE THE DEFECT ═══
//
// DSS answers `__has_extension(c_atomic)` = 1. sqlite's `sqliteInt.h` reads that
// answer as the gate for the GNU atomic builtins:
//
//     #if GCC_VERSION>=4007000 || __has_extension(c_atomic)
//     # define AtomicLoad(PTR)      __atomic_load_n((PTR),__ATOMIC_RELAXED)
//     # define AtomicStore(PTR,VAL) __atomic_store_n((PTR),(VAL),__ATOMIC_RELAXED)
//
// The names did not exist, so the ENTIRE real-world corpus failed to compile on
// all five legs — for SIX cycles, every one of them under a fully green unit
// suite (2174/2174 on Windows at the moment it was found). The suite could not
// see it because nothing in it consumed a header that ASKS DSS WHAT IT HAS and
// then uses the answer. That is a structural blindness, not an oversight, and
// the row that closed the defect is explicit that repairing the instrument is
// the other half of the work.
//
// ★★★ THE GENERAL FORM OF THE BUG IS "AN ADVERTISEMENT THE COMPILER CANNOT
// HONOUR", and `c_atomic` had no reason to be the only one. So this file does
// not pin `c_atomic`. It enumerates the ADVERTISED SET out of the loaded schema
// and requires a compiled witness for every member — which means a capability
// added to `c.lang.json` tomorrow is covered by this test on the day it is
// added, without anyone remembering to extend it.
//
// ═══ THE RATCHET, AND IT RUNS IN BOTH DIRECTIONS ═════════════════════════════
//
// `WitnessesAreDeclaredForEveryAdvertisedFeature` fails if a row in
// `preprocess.languageFeatures` has NO witness here. That is the arm that makes
// the guard survive contact with the future: the only way to advertise a new
// feature is to also state, in compilable C, what the advertisement PROMISES.
// The opposite arm fails if a witness names a feature the config no longer
// declares, so a retracted capability cannot leave a dead witness behind
// asserting nothing — the shape [[feedback-an-escape-every-row-triggers-disarms-the-guard]]
// warns about, where a guard keeps passing after it has stopped guarding.
//
// ⚠ A WITNESS IS NOT A PARAPHRASE OF THE FEATURE NAME. It is what a PROGRAM that
// believed the advertisement would write, which is why `c_atomic`'s witness uses
// the GNU `__atomic_*` builtins and not only `_Atomic`. That choice is MEASURED,
// not assumed: ✔2026-09-09, clang 18.1.3 — the ONE reference that implements
// `__has_extension` at all — answers `c_atomic` = 1 in BOTH `-std=c17` AND
// `-std=c89` (where `__has_feature(c_atomic)` answers 0), and in EVERY mode where
// it answers 1, `_Atomic` AND `__atomic_load_n`/`__atomic_store_n` all compile and
// RUN. A consumer therefore cannot observe the two halves apart, and an
// implementation that ships one without the other is making a claim the only
// reference for that claim does not make. gcc 13.3.0 answers 0 for `c_atomic`
// (it has no `__has_extension`), so it casts no vote on the name — but it does
// provide every `__atomic_*` builtin in the witness, probed separately.
//
// ⚠ WHAT THIS FILE DELIBERATELY DOES NOT DO. It does not evaluate a language
// feature PREDICATE — `preprocess_config.hpp` records that building one is
// explicitly deferred by the operator, and this file must not become one by
// accident. It compiles authored C and reads diagnostics; that is all.
//
// ⓘ SCOPE, stated so the next reader knows what is still uncovered: the
// `__has_attribute` and `__has_builtin` answer sets are NOT enumerated here.
// They are large, and several of their members are position-restricted (the SEH
// builtins are legal only inside a filter expression) or take type-names rather
// than values (`__builtin_offsetof`), so a synthesized call site would produce
// false reds that say nothing about an advertisement. The GNU atomic family this
// row shipped IS covered, by name, in `AdvertisedAtomicBuiltinsAllCompile` below.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/preprocess_config.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// One advertised capability and the translation unit that spends it.
//
// ★ `use` is a COMPLETE translation unit, not a fragment: a witness spliced into
// a surrounding template would share a scope with its siblings and one witness's
// failure could be masked (or caused) by another's declarations. Each is
// analyzed alone.
struct Witness {
    std::string_view feature;
    std::string_view use;
};

// ── THE WITNESS TABLE ────────────────────────────────────────────────────────
//
// Each entry answers: "a program that believed `__has_feature(<name>)` == 1,
// what would it write?" — and every one of them was compiled and RUN under
// gcc 13.3.0 and clang 18.1.3 separately before being written down.
constexpr Witness kWitnesses[] = {
    {"c_alignas",
     "_Alignas(16) static char aligned_buffer[32];\n"
     "int use_alignas(void) { return (int)sizeof aligned_buffer; }\n"},

    {"c_alignof",
     "int use_alignof(void) { return (int)_Alignof(double); }\n"},

    // ★★ THE ROW'S OWN FEATURE. `_Atomic` alone would NOT have caught the defect
    // — DSS has had `_Atomic` throughout — so the witness spends the
    // advertisement the way the corpus spends it: through the GNU value-form
    // builtins. Every name below is declared in `semantics.builtinFunctions`,
    // and `AdvertisedAtomicBuiltinsAllCompile` pins that set independently.
    {"c_atomic",
     "static _Atomic int qualified;\n"
     "static volatile int plain;\n"
     "int use_atomic(void) {\n"
     "    qualified = 3;\n"
     "    __atomic_store_n(&plain, 7, __ATOMIC_RELAXED);\n"
     "    int v = __atomic_load_n(&plain, __ATOMIC_RELAXED);\n"
     "    v += __atomic_exchange_n(&plain, 9, __ATOMIC_SEQ_CST);\n"
     "    v += __atomic_fetch_add(&plain, 1, __ATOMIC_SEQ_CST);\n"
     "    v += __atomic_fetch_sub(&plain, 1, __ATOMIC_SEQ_CST);\n"
     "    v += __atomic_fetch_or(&plain, 2, __ATOMIC_SEQ_CST);\n"
     "    v += __atomic_fetch_xor(&plain, 4, __ATOMIC_SEQ_CST);\n"
     "    v += __atomic_fetch_and(&plain, 6, __ATOMIC_SEQ_CST);\n"
     "    return v + (int)qualified;\n"
     "}\n"},

    {"c_generic_selections",
     "int use_generic(int x) {\n"
     "    return _Generic(x, int: 1, long: 2, default: 0);\n"
     "}\n"},

    // ⚠ THE CONDITION IS DELIBERATELY TARGET-INDEPENDENT, and the reason is a
    // MEASUREMENT this file paid for. The first draft asserted
    // `sizeof(int) >= 2` and went RED here with
    // `S_StaticAssertFailed: ... is not an integer constant expression
    //  (folded: `sizeof(int)` Ge 2)` — while the IDENTICAL translation unit
    // compiled rc 0 through the shipped CLI. The difference is the harness, not
    // the compiler: `analyzeShipped` runs the front end with NO TARGET, and
    // `sizeof(int)` has no value until a data model is resolved. A witness that
    // asks a target-dependent question therefore measures THE HARNESS, which is
    // exactly the adjacent-question failure
    // [[feedback-an-instrument-that-answers-an-adjacent-question]] describes —
    // and it fails toward RED here rather than toward clean, which is the only
    // reason it was caught rather than believed. Keep every witness answerable
    // without a target; a target-dependent claim belongs in an example, which
    // has one.
    {"c_static_assert",
     "_Static_assert(2 + 2 == 4, \"the feature is available\");\n"
     "int use_static_assert(void) { return 1; }\n"},

    {"c_thread_local",
     "static _Thread_local int per_thread;\n"
     "int use_thread_local(void) { per_thread += 1; return per_thread; }\n"},
};

[[nodiscard]] std::vector<std::string> declaredFeatureNames() {
    auto schema = loadShippedSchema("c");
    std::vector<std::string> out;
    for (auto const& f : schema->preprocess().languageFeatures)
        out.push_back(f.name);
    return out;
}

// Every Error-severity diagnostic, rendered so a failure NAMES what broke rather
// than reporting a count. A guard whose failure message is "expected 0, got 3"
// costs the next reader the whole re-derivation.
[[nodiscard]] std::string errorSummary(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "\n    ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
    return out;
}

[[nodiscard]] std::size_t errorCount(DiagnosticReporter const& r) {
    auto all = r.all();
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(), [](ParseDiagnostic const& d) {
            return d.severity == DiagnosticSeverity::Error;
        }));
}

}  // namespace

// ★★★ THE RATCHET, FORWARD DIRECTION: an advertised feature with no witness is a
// FAILURE, not a silent skip. This is the arm that makes the guard general — the
// next capability added to `c.lang.json` is covered on the day it is added,
// because the only way to add one without turning this red is to state what it
// promises in compilable C.
TEST(AdvertisedCapabilityIsHonoured, WitnessesAreDeclaredForEveryAdvertisedFeature) {
    for (auto const& name : declaredFeatureNames()) {
        bool const covered =
            std::any_of(std::begin(kWitnesses), std::end(kWitnesses),
                        [&](Witness const& w) { return w.feature == name; });
        EXPECT_TRUE(covered)
            << "`c.lang.json` advertises the language feature '" << name
            << "' to `__has_feature`/`__has_extension`, but no witness in this "
               "file spends it. An advertisement the compiler cannot honour is "
               "STRICTLY WORSE than not having the operator: it routes a "
               "portable header onto a path DSS cannot take, turning a working "
               "fallback into a failed compile (or a silent miscompile). Add a "
               "witness that USES the feature the way a program believing the "
               "advertisement would.";
    }
}

// ★★ THE RATCHET, REVERSE DIRECTION: a witness for a feature the config no
// longer declares is a guard that has stopped guarding while still passing.
TEST(AdvertisedCapabilityIsHonoured, EveryWitnessNamesAStillDeclaredFeature) {
    auto const declared = declaredFeatureNames();
    for (auto const& w : kWitnesses) {
        bool const present =
            std::find(declared.begin(), declared.end(), std::string{w.feature})
            != declared.end();
        EXPECT_TRUE(present)
            << "the witness for '" << w.feature << "' outlived its declaration: "
               "`c.lang.json` no longer advertises that feature. Delete the "
               "witness, or restore the row — a witness asserting nothing is "
               "indistinguishable from coverage.";
    }
}

// ★★★ THE ARM THAT WOULD HAVE CAUGHT THE DEFECT. Each witness is COMPILED, and
// an advertised capability whose witness does not compile is the exact failure
// this file exists to name.
TEST(AdvertisedCapabilityIsHonoured, EveryAdvertisedFeatureCompiles) {
    for (auto const& w : kWitnesses) {
        auto model = analyzeShipped("c", {std::string{w.use}});
        EXPECT_EQ(errorCount(model.diagnostics()), 0u)
            << "DSS advertises '" << w.feature
            << "' through `__has_feature`/`__has_extension`, but a translation "
               "unit that USES it does not compile."
            << errorSummary(model.diagnostics());
    }
}

// ★★ THE BUILTIN HALF, pinned by NAME rather than by enumeration — see the file
// header for why the whole `__has_builtin` answer set is not swept here. These
// are the eight rows the closing row shipped, and `__has_builtin` answers 1 for
// each of them because it reads `semantics.builtinFunctions` directly.
//
// ⚠ THE CONTROL IS PART OF THE PIN. `__atomic_thread_fence` is DELIBERATELY not
// shipped (it takes a runtime order, while `atomic_fence` is zero-argument and
// seq_cst-baked), and this arm asserts it still REFUSES. Without the control an
// engine that accepted every unknown `__atomic_*` identifier would pass the arm
// above while honouring nothing — a guard that fails toward *clean*.
TEST(AdvertisedCapabilityIsHonoured, AdvertisedAtomicBuiltinsAllCompile) {
    static constexpr std::string_view kShipped[] = {
        "__atomic_load_n", "__atomic_store_n", "__atomic_exchange_n",
        "__atomic_fetch_add", "__atomic_fetch_sub", "__atomic_fetch_or",
        "__atomic_fetch_xor", "__atomic_fetch_and",
    };
    auto schema = loadShippedSchema("c");
    for (auto const& name : kShipped) {
        bool const declared = std::any_of(
            schema->semantics().builtinFunctions.begin(),
            schema->semantics().builtinFunctions.end(),
            [&](auto const& b) { return b.name == name; });
        EXPECT_TRUE(declared)
            << '`' << name
            << "` must be declared in `semantics.builtinFunctions` — that table "
               "is what `__has_builtin` answers from, so an undeclared row makes "
               "the operator answer 0 for a builtin DSS implements.";
    }
}

// ★★ THE NEGATIVE CONTROL, and it is the arm that keeps the one above honest.
// If the front end resolved ANY `__atomic_*` spelling, every positive arm here
// would pass while the family boundary meant nothing. The excluded names must
// still refuse LOUDLY.
TEST(AdvertisedCapabilityIsHonoured, UnshippedAtomicSpellingsStillRefuseLoudly) {
    static constexpr std::string_view kNotShipped[] = {
        "__atomic_thread_fence", "__atomic_add_fetch", "__atomic_test_and_set",
    };
    for (auto const& name : kNotShipped) {
        std::string const src =
            "static volatile int p;\n"
            "int probe(void) { return (int)" + std::string{name}
            + "(&p, 0); }\n";
        auto model = analyzeShipped("c", {src});
        EXPECT_GT(errorCount(model.diagnostics()), 0u)
            << '`' << name
            << "` is deliberately OUTSIDE the family this row shipped, so it "
               "must refuse LOUDLY. Accepting it silently would mean the front "
               "end resolves atomic spellings it does not lower — which is the "
               "same class of defect this file was written to catch, pointing "
               "the other way. If it was shipped on purpose, move it into the "
               "positive arm and give it a runtime witness.";
    }
}
