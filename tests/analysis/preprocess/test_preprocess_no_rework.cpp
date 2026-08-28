// The two REWORK defects the preprocessor shipped, pinned by the property each
// one violated. Both were pure waste — the same answer, computed again — so
// neither could ever be caught by an output test. What catches them is the
// SHAPE of the cost curve, which is what every case below measures.
//
//   [[D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION]]
//       `evaluateIfExpression` assembled a fresh `prefix + productText` buffer
//       — a copy of the WHOLE translation unit — on every `#if`/`#elif` line
//       evaluated after the TU's first `#`/`##`/predefined product. ✔MEASURED
//       (cycle P36, 103-TU sqlite full-source corpus, Release, Windows/MinGW
//       GCC 13.2): 13,242 evaluations copied 12,424 MB and cost 10.05 s of the
//       13.3 s `preprocess-expand` phase.
//
//   [[D-PERF-PP-EVERY-INCLUDE-RE-READS-AND-RE-TOKENIZES-THE-SAME-HEADER]]
//       `SynthBuilder::build` opened, continuation-spliced, buffered and FULLY
//       TOKENIZED its file on every OCCURRENCE of an `#include` naming it —
//       per TU and again for every TU. ✔MEASURED on the same corpus: 1,364
//       opens read 109.2 MB and the pre-scan tokenized 118.7 MB into 13.2 M
//       pp-tokens, 10.2 s of the 16.1 s `preprocess-splice` phase.
//
// ★★★ NO PIN HERE IS A WALL-CLOCK THRESHOLD, AND ONLY ONE IS STILL A CLOCK AT
// ALL. A test that asserts "this preprocesses in under N ms" is a machine
// benchmark: it reds on a loaded CI box and greens on a fast one, and it says
// nothing about the property. Both defects had the same signature — a cost that
// GREW WITH SOMETHING IT MUST NOT GROW WITH — so each case builds two inputs
// that differ ONLY in that one dimension.
//
// ⚠ THIS BLOCK USED TO SAY "EVERY PIN HERE IS A RATIO", AND CALLED A RATIO
// "insensitive to host speed and to load". ✔MEASURED FALSE on CI run
// 33156833090: the include case read x1.048 against its 0.85 bound on
// `linux-gcc-release` and PASSED on `linux-arm64-gcc-release` in the SAME run at
// the SAME commit. A ratio's MEANING is host-insensitive; its MEASUREMENT is
// two half-second samples on a shared two-vCPU runner, where scheduling noise
// is the same order as the effect. [[D-TEST-PP-NO-REWORK-PINS-A-COUNT-WITH-A-WALL-CLOCK-RATIO]]
//
// ⇒ THE RULE THE TWO CASES NOW FOLLOW, AND THE SPLIT IS THE POINT:
//   • ASK THE PROPERTY WHAT KIND OF QUANTITY IT IS. The include defect is
//     "the same file is read N times instead of once" — a COUNT. It is pinned on
//     `PreScanMemoCounters`: exact integers, identical on every host at every
//     load, and false by a factor of `kUnits` the moment the memo goes unread.
//   • A CLOCK ONLY WHERE COST IS GENUINELY THE OBSERVABLE. The `#if` defect is a
//     memcpy that leaves no trace but time; a counter for it would have no
//     writer in the fixed code and so could never fire. That case keeps a ratio
//     and fixes the ESTIMATOR instead — `min` over interleaved rounds, because
//     scheduling noise is additive and one-sided.
// The surviving bound stays deliberately loose (a small multiple, not a tight
// constant) and is still FALSE by one to two orders of magnitude when the
// rework returns.
//
// ★ AND EVERY CASE ALSO ASSERTS THE ANSWER. A performance pin that does not
// check the OUTPUT is how an optimization that changes a token gets a green
// test — so each arm compares the significant lexeme stream against the shape
// it must produce, and the `#if` arms additionally pin the BRANCH taken, which
// is the thing a broken product-tail slice would silently get wrong.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/source_buffer.hpp"
#include "test_support/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// Shared schema fixture — a REFERENCE to a function-local static, for the
// reason `test_preprocessor.cpp` spells out under
// D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE: `GrammarSchema`'s accessors hand
// back references INTO the schema, so a by-value return would make
// `helper()->accessor()` a heap-use-after-free.
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSchema() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            // THROW, never `std::abort()`: abort kills the whole test BINARY,
            // so every sibling test loses its verdict.
            throw std::runtime_error{"loadShipped(c) failed"};
        }
        return *loaded;
    }();
    return schema;
}

[[nodiscard]] PreprocessResult pp(std::string text,
                                  std::vector<fs::path> const& dirs = {},
                                  std::string name = "rework.c") {
    auto buf = SourceBuffer::fromString(std::move(text), std::move(name));
    return preprocess(buf, cSchema(), dirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

// Move `path`'s write time to a stamp the pre-scan memo CANNOT match, and
// PROVE it moved. Both include cases below need this, and neither may express
// it as a wall-clock margin.
//
// ★★ WHY THIS MEASURES INSTEAD OF GUESSING. The margin a stamp bump needs is a
// property of the FILESYSTEM (its timestamp granularity), not of the code — so
// any constant written here is sized on whichever machine happened to write it
// and is wrong everywhere else, which is exactly the defect
// [[D-TEST-A-NEW-WALL-CLOCK-LITERAL-IN-A-TEST-IS-UNGUARDED]] refuses. This
// steps by the file clock's OWN smallest representable tick and doubles until
// the filesystem actually RECORDS a different stamp: a coarse filesystem takes
// a few more doublings and a fine one stops at the first, so the granularity is
// discovered rather than assumed.
//
// ⚠ AND THE RANGE IS DISCOVERED THE SAME WAY. ✔MEASURED 2026-08-26, Windows /
// MinGW: displacing the stamp by its own distance from the clock epoch — the
// first spelling tried here — is REFUSED by the OS with `cannot set file time:
// Invalid argument`, because the result leaves the range `SetFileTime` accepts.
// So this uses the `error_code` overload and treats a refusal as "too far, keep
// looking" rather than letting it throw.
//
// ⓘ DIRECTION IS IRRELEVANT, which is what makes a forward step safe to choose
// freely: `PreScanKey::operator==` compares `mtime` with `==`, never with `<`,
// so ANY distinct stamp is a miss.
void forceADistinctWriteTime(fs::path const& path,
                             fs::file_time_type const& from) {
    auto step = fs::file_time_type::duration{1};
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::last_write_time(path, from + step, ec);
        if (!ec && fs::last_write_time(path) != from) return;
        step *= 2;
    }
    FAIL() << "could not move " << path.string()
           << "'s write time to any stamp distinguishable from the one the "
              "memo recorded, after 40 doublings of the file clock tick - the "
              "case downstream cannot say anything about the mtime component "
              "of the memo key";
}

// The SIGNIFICANT lexemes of a result — what the parser actually consumes.
// Trivia is dropped deliberately: these cases answer "did the program change",
// and the whole point of removing rework is that it must not.
[[nodiscard]] std::vector<std::string> significantLexemes(
    PreprocessResult const& r) {
    std::vector<std::string> out;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        if (isEmptySpace(t.flags)) continue;
        out.push_back(std::string{r.synthBuffer->slice(t.span)});
    }
    return out;
}

// Elapsed time of ONE preprocess, in microseconds.
//
// ⚠ ONE case still uses this, and it reads a MINIMUM over interleaved rounds
// rather than a single sample. The comment here used to say that a ratio of two
// back-to-back samples means "a slow or loaded host scales both sides together"
// — ✔MEASURED FALSE on CI run 33156833090, where exactly that shape reddened on
// one runner and passed on another at the same commit. Load does not scale two
// samples; it is ADDED to whichever one it lands on.
// [[D-TEST-PP-NO-REWORK-PINS-A-COUNT-WITH-A-WALL-CLOCK-RATIO]]
template <typename F>
[[nodiscard]] double microseconds(F&& f) {
    auto const t0 = std::chrono::steady_clock::now();
    f();
    auto const t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

// ══════════════════════════════════════════════════════════════════════════
// D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION
// ══════════════════════════════════════════════════════════════════════════
//
// The defect's exact shape, and it had a SWITCH: the per-`#if` cost became
// O(size of the whole TU) only once the unit had materialized its first
// `#`/`##`/predefined PRODUCT, because the copy existed to append that product
// tail to the prefix buffer. With an empty tail the old code sliced the prefix
// directly and copied nothing.
//
// ★★ SO THE PIN IS BUILT ON THAT SWITCH, AND THE FIRST VERSION OF IT WAS NOT.
// The first cut compared a small unit against a unit 80x larger with the same
// `#if` count, and bounded the ratio at 4x the size ratio. That mutant-tested
// GREEN with the whole-buffer copy PUT BACK — measured, the copies added ~16% to
// an arm whose linear work already dominated, and the loose bound swallowed it.
// A performance pin that its own defect cannot turn red is worth nothing, which
// is what the mutant run is for.
//
// The two arms here are the SAME unit — same bulk, same `#if` count, same
// answers — differing by TWO LINES: one arm defines and invokes a `##` paste and
// the other does not. That is precisely the defect's switch, so with the rework
// present the product arm pays `#if count x whole-unit bytes` of memcpy that the
// no-product arm never pays, and without it the two are the same work.

// `n` lines of inert bulk followed by `ifCount` conditional groups. `withProduct`
// adds the two lines that arm the defect: a `##` paste whose result lands in the
// product tail. Every `#if` operand is deliberately built from LITERALS and an
// undefined name only — a predefined macro in the operand would materialize its
// value into the product tail too, and arm the "no product" arm as well.
[[nodiscard]] std::string tuWithBulkAndIfs(std::size_t bulkLines,
                                           std::size_t ifCount,
                                           bool withProduct) {
    std::string s;
    if (withProduct) {
        s += "#define CAT(a, b) a##b\n";
        s += "int product = CAT(1, 2);\n";
    }
    // ★ A FLUSH BOUNDARY, IN BOTH ARMS. Macro expansion is POSITIONAL
    // (c18): the pending body is expanded at each table-mutating directive, so
    // without a `#define` after the paste the `CAT(1, 2)` above would not expand
    // until EOF and the product tail would still be EMPTY at every `#if` below
    // — the defect would never arm, and this pin passed against its own mutant
    // for exactly that reason until the mutant run caught it. The line is in
    // BOTH arms, so the two units still differ only in the paste.
    s += "#define FORCE_A_FLUSH_BOUNDARY 1\n";
    for (std::size_t i = 0; i < bulkLines; ++i) {
        s += "static int bulk_" + std::to_string(i) + " = " + std::to_string(i)
             + ";\n";
    }
    for (std::size_t i = 0; i < ifCount; ++i) {
        s += "#if !defined(NEVER_DEFINED_ANYWHERE) && (1 + 1 == 2)\n";
        s += "int live_" + std::to_string(i) + " = 1;\n";
        s += "#else\n";
        s += "int dead_" + std::to_string(i) + " = 0;\n";
        s += "#endif\n";
    }
    return s;
}

TEST(PpIfNoRework, AProductInTheUnitDoesNotMakeEveryIfCostTheWholeUnit) {
    constexpr std::size_t kIfs  = 800;
    constexpr std::size_t kBulk = 12000;   // ~400 KB of inert text

    std::string const noProduct   = tuWithBulkAndIfs(kBulk, kIfs, /*withProduct=*/false);
    std::string const withProduct = tuWithBulkAndIfs(kBulk, kIfs, /*withProduct=*/true);

    // Warm the schema + allocator before either measurement, on an unrelated
    // unit so neither arm is charged for it.
    (void)pp("int warm = 1;\n");

    PreprocessResult noR   = pp(noProduct);
    PreprocessResult withR = pp(withProduct);

    // ★★★ THE ESTIMATOR IS A MINIMUM OVER INTERLEAVED ROUNDS, AND THAT IS A
    // CORRECTION TO THE ESTIMATOR RATHER THAN A LOOSENING OF THE BOUND — the
    // bound below is untouched. Scheduling noise on a shared CI runner is
    // ADDITIVE and one-sided: a descheduled arm can only be measured as SLOWER
    // than it is, never faster. A single sample of each arm therefore estimates
    // `true cost + whatever that arm happened to be charged`, and the ratio of
    // two such samples is a ratio of two noise draws. `min` over k rounds is the
    // consistent estimator of the quantity this case is actually about, and the
    // arms are INTERLEAVED so a load excursion lands on both rather than on
    // whichever ran second.
    //
    // ✔MEASURED, and this is why it is here rather than left as it was: the
    // SIBLING case in this file — same file, same idiom, a single sample per arm
    // — went red on `linux-gcc-release` at x1.048 against its 0.85 bound while
    // PASSING on `linux-arm64-gcc-release` in the SAME CI run at the SAME commit
    // (run 33156833090). That case has since been re-pinned on an exact COUNT
    // because its property IS a count. This one's property is a MEMCPY whose
    // only trace is time, so the instrument stays a clock and the fix belongs in
    // how the clock is read. [[D-TEST-PP-NO-REWORK-PINS-A-COUNT-WITH-A-WALL-CLOCK-RATIO]]
    //
    // ⓘ `kRounds` is a repetition count, not a duration: it is not sized on any
    // machine and cannot go stale on a slower one.
    constexpr int kRounds = 3;
    double noUs   = std::numeric_limits<double>::infinity();
    double withUs = std::numeric_limits<double>::infinity();
    for (int round = 0; round < kRounds; ++round) {
        noUs   = std::min(noUs,   microseconds([&] { noR   = pp(noProduct); }));
        withUs = std::min(withUs, microseconds([&] { withR = pp(withProduct); }));
    }

    ASSERT_FALSE(noR.diagnostics->hasErrors());
    ASSERT_FALSE(withR.diagnostics->hasErrors());

    // THE ANSWER FIRST — a perf pin that does not check output is how a
    // miscompile gets a green test. Every `#if` must have taken its LIVE arm, in
    // BOTH arms.
    auto countPrefixed = [](std::vector<std::string> const& v, char const* pfx) {
        std::size_t n = 0;
        for (std::string const& lx : v) if (lx.rfind(pfx, 0) == 0) ++n;
        return n;
    };
    auto const noLex   = significantLexemes(noR);
    auto const withLex = significantLexemes(withR);
    EXPECT_EQ(countPrefixed(noLex, "live_"), kIfs)
        << "an `#if` that must be TRUE did not take its live arm";
    EXPECT_EQ(countPrefixed(noLex, "dead_"), 0u)
        << "a dead `#else` arm reached the token stream";
    EXPECT_EQ(countPrefixed(withLex, "live_"), kIfs);
    EXPECT_EQ(countPrefixed(withLex, "dead_"), 0u);

    // THE PIN. The two units differ by two lines. Adding a `##` paste must not
    // change what an `#if` COSTS — it changes only where a handful of bytes
    // live. With the rework present, the product arm copies the whole ~400 KB
    // unit twice for each of 800 `#if`s (~640 MB of memcpy) and the no-product
    // arm copies nothing at all.
    const double ratio = withUs / (noUs > 0.0 ? noUs : 1.0);
    EXPECT_LT(ratio, 2.0)
        << "adding a `##` paste to a " << kBulk << "-line unit with " << kIfs
        << " `#if`s made it x" << ratio << " as expensive (" << noUs << "us -> "
        << withUs
        << "us). The paste added two lines of work; anything more means every "
           "`#if` after it is re-materializing `prefix + productText` — a copy "
           "of the WHOLE unit, once per evaluation.";
}

TEST(PpIfNoRework, AProductTokenInAnIfOperandStillResolvesToItsValue) {
    // The correctness half of the same change: after the switch from a
    // materialized `prefix + product` buffer to slicing the product tail in
    // place, a token whose bytes live in the PRODUCT region must still read as
    // its spelling. `__STDC_VERSION__` is a predefined macro, so its value is
    // minted into the product tail during the operand's expansion — this
    // operand is decided ENTIRELY by bytes that are not in the prefix buffer.
    PreprocessResult const r = pp(
        "#define CAT(a, b) a##b\n"
        "int product = CAT(3, 4);\n"
        "#if __STDC_VERSION__ >= 201112L\n"
        "int modern = 1;\n"
        "#else\n"
        "int ancient = 0;\n"
        "#endif\n");
    ASSERT_FALSE(r.diagnostics->hasErrors());

    auto const lex = significantLexemes(r);
    bool sawModern  = false;
    bool sawAncient = false;
    bool sawProduct = false;
    for (std::string const& lx : lex) {
        if (lx == "modern")  sawModern = true;
        if (lx == "ancient") sawAncient = true;
        if (lx == "34")      sawProduct = true;
    }
    EXPECT_TRUE(sawProduct)
        << "the `##` paste did not produce the token `34` — the product tail is "
           "not reaching the stream";
    EXPECT_TRUE(sawModern)
        << "`#if __STDC_VERSION__ >= 201112L` folded FALSE: the predefined "
           "macro's value lives in the product tail, so this is what a lost "
           "product-region slice looks like — a silently taken wrong branch";
    EXPECT_FALSE(sawAncient);
}

// ══════════════════════════════════════════════════════════════════════════
// D-PERF-PP-EVERY-INCLUDE-RE-READS-AND-RE-TOKENIZES-THE-SAME-HEADER
// ══════════════════════════════════════════════════════════════════════════
//
// The defect's exact shape: the read + continuation-splice + tokenize of a
// header was paid per OCCURRENCE of an `#include` naming it — per translation
// unit, and again for every unit in the project — rather than once per FILE.
//
// ★★ WHY BOTH ARMS PREPROCESS THE SAME NUMBER OF UNITS AND THE SAME NUMBER OF
// BYTES, AND DIFFER ONLY IN HOW MANY DISTINCT FILES EXIST. Two earlier cuts of
// this case were WRONG, and each red was informative:
//   • "40 includes of a guarded header vs 1 include" went red at x35 with the
//     memo working perfectly — re-including a guarded header still copies its
//     full text into the synth buffer (the pre-scan gates only that header's own
//     nested includes; conditional ELISION happens later, in the macro pass), so
//     the 40-include unit genuinely carries 40x the bytes. Real growth, DIFFERENT
//     defect.
//   • "40 includes of one file vs 40 includes of 40 identical files, in ONE unit"
//     came out at x1.00 — correct, and it says the win is not WITHIN a unit: one
//     unit reads each of its headers about once anyway.
// The rework this pin is about is ACROSS units. So both arms below preprocess
// `kUnits` translation units that each include one large guarded header; the
// arms differ only in whether those units name ONE file or `kUnits`
// byte-identical copies of it. Identical bytes spliced, identical token streams,
// identical downstream work — and `kUnits` times as many DISTINCT files to read,
// splice and tokenize on one side. The memo can collapse the first and cannot
// touch the second, so the ratio between them IS the per-file rework, isolated.
// It is ~1.0 with the defect present.

// Write `count` byte-identical guarded headers into `dir`, each `n`
// declarations long.
//
// The body is ORDINARY DECLARATION TEXT and deliberately carries no `#define`s
// beyond its guard. An earlier cut filled it with 3000 object-like macros and
// the timing pin came out at x1.03 with the memo demonstrably HITTING (37 hits,
// 13 misses on the probe) — because a macro-dense header's cost is the macro
// pass, not the tokenize, so the thing this pin measures was a rounding error in
// it. What the memo owns is the READ + SPLICE + TOKENIZE, so the fixture has to
// be dominated by exactly that.
void writeIdenticalHeaders(fs::path const& dir, char const* stem,
                           std::size_t count, std::size_t n) {
    fs::create_directories(dir);
    std::string body = "#ifndef BIG_H_INCLUDED\n#define BIG_H_INCLUDED\n";
    for (std::size_t i = 0; i < n; ++i) {
        body += "extern int big_symbol_" + std::to_string(i) + ";\n";
        body += "extern long big_other_" + std::to_string(i)
              + "(int a, int b, char const* c);\n";
    }
    body += "#define BIG_MACRO_0 0\n#endif\n";
    for (std::size_t k = 0; k < count; ++k) {
        std::ofstream os{dir / (std::string{stem} + std::to_string(k) + ".h"),
                         std::ios::binary};
        os << body;
    }
}

constexpr std::size_t kUnits = 12;

TEST(PpIncludeNoRework, OneHeaderAcrossManyUnitsIsReadAndTokenizedOnce) {
    test_support::ScratchDir scratch{test_support::Location::Temp,
                                     "pp-include-rework"};
    fs::path const dir = scratch.path();
    writeIdenticalHeaders(dir, "same", /*count=*/1, /*n=*/3000);
    writeIdenticalHeaders(dir, "dist", /*count=*/kUnits, /*n=*/3000);
    std::vector<fs::path> const dirs{dir};

    // One unit per arm element. Each includes exactly one header, so every unit
    // splices the same bytes and every unit's downstream work is the same. The
    // arms differ ONLY in how many DISTINCT files those kUnits includes name.
    auto runSame = [&] {
        for (std::size_t i = 0; i < kUnits; ++i) {
            PreprocessResult r = pp("#include \"same0.h\"\nint use = BIG_MACRO_0;\n",
                                    dirs, "u" + std::to_string(i) + ".c");
            if (r.diagnostics->hasErrors()) ADD_FAILURE() << "same arm errored";
        }
    };
    auto runDistinct = [&] {
        for (std::size_t i = 0; i < kUnits; ++i) {
            PreprocessResult r =
                pp("#include \"dist" + std::to_string(i) + ".h\"\nint use = BIG_MACRO_0;\n",
                   dirs, "v" + std::to_string(i) + ".c");
            if (r.diagnostics->hasErrors()) ADD_FAILURE() << "distinct arm errored";
        }
    };

    // Each arm is measured from a ZEROED counter, and the reset is what makes
    // the two arms independent of every sibling case in this binary — the memo
    // and its counters are process-lifetime, so a shared scratch state would
    // make this case's verdict depend on test ORDER.
    PreScanMemoCounters::reset();
    runSame();
    PreScanMemoCounters::Row const same = PreScanMemoCounters::read();

    PreScanMemoCounters::reset();
    runDistinct();
    PreScanMemoCounters::Row const dist = PreScanMemoCounters::read();

    // THE PIN, AND IT IS A COUNT BECAUSE THE PROPERTY IS A COUNT. The read +
    // continuation-splice + tokenize of a header must be paid once per FILE and
    // not once per OCCURRENCE of an `#include` naming it. `kUnits` units sharing
    // ONE header therefore do that work ONCE and take `kUnits - 1` memo hits;
    // `kUnits` units with one byte-identical header EACH must do it `kUnits`
    // times and can take none, because no two of them name the same file.
    //
    // ★★★ THIS USED TO BE A WALL-CLOCK RATIO (`sameUs / distUs < 0.85`) AND THE
    // RATIO IS WHAT BROKE, NOT THE PROPERTY.
    // [[D-TEST-PP-NO-REWORK-PINS-A-COUNT-WITH-A-WALL-CLOCK-RATIO]]. ✔MEASURED on
    // CI run 33156833090: it read x1.0483 against the 0.85 bound on
    // `linux-gcc-release` and PASSED on `linux-arm64-gcc-release` in the SAME
    // run at the SAME commit — two half-second arms on a shared two-vCPU runner,
    // where scheduling noise is the same order as the effect being measured. The
    // file's own header argued a ratio is host- and load-insensitive; that is
    // true of the ratio's MEANING and false of its MEASUREMENT.
    // ⇒ The counters below are exact integers on any host at any load, and they
    // are FALSE BY A FACTOR OF `kUnits` the moment the memo stops being
    // consulted — a far larger margin than the 1.7x the ratio ever had.
    EXPECT_EQ(same.builds, 1u)
        << kUnits << " units sharing ONE header read, spliced and tokenized it "
        << same.builds << " times. It must be read ONCE: those units differ only "
           "in how many distinct files exist, so anything above 1 is the "
           "per-OCCURRENCE rework this case exists to refuse.";
    EXPECT_EQ(same.hits, kUnits - 1)
        << "the shared header was built " << same.builds << " time(s) but served "
        << same.hits << " time(s) from the memo, where " << (kUnits - 1)
        << " was due. A build count of 1 with no hits means the other units "
           "never asked at all, which would make the count above vacuous.";
    EXPECT_EQ(dist.builds, kUnits)
        << kUnits << " units naming " << kUnits << " DISTINCT byte-identical "
           "headers did the per-file work " << dist.builds << " times. This arm "
           "is the control: the memo cannot collapse it, and if it does then the "
           "key is matching on something weaker than file identity.";
    EXPECT_EQ(dist.hits, 0u)
        << "a distinct-file arm took " << dist.hits << " memo hit(s). Two "
           "byte-identical files are not the same file; a hit here means the key "
           "has stopped distinguishing them.";

    // THE ANSWER. A unit that includes `same0.h` and a unit that includes
    // `dist0.h` must be the same program — the files are byte-identical. This is
    // what a memo that ever served the wrong entry would break, loudly. A pin on
    // work avoided that does not check the OUTPUT is how "avoid all the work"
    // gets a green test.
    PreprocessResult const a =
        pp("#include \"same0.h\"\nint use = BIG_MACRO_0;\n", dirs, "a.c");
    PreprocessResult const b =
        pp("#include \"dist0.h\"\nint use = BIG_MACRO_0;\n", dirs, "b.c");
    ASSERT_FALSE(a.diagnostics->hasErrors());
    ASSERT_FALSE(b.diagnostics->hasErrors());
    EXPECT_EQ(significantLexemes(a), significantLexemes(b))
        << "two byte-identical headers produced different programs — the "
           "per-file pre-scan is handing back an entry that does not match the "
           "file it was asked for";
}

TEST(PpIncludeNoRework, TheMemoServesTheFileThatWasAskedFor) {
    // The memo is keyed on file IDENTITY, so two DIFFERENT headers with the
    // same include spelling in different directories must never share an entry
    // — the failure mode a path-blind cache would have, and the one that would
    // silently compile the wrong header's declarations into a unit.
    test_support::ScratchDir scratch{test_support::Location::Temp, "pp-include-identity"};
    fs::path const a = scratch.path() / "a";
    fs::path const b = scratch.path() / "b";
    fs::create_directories(a);
    fs::create_directories(b);
    {
        std::ofstream os{a / "same.h", std::ios::binary};
        os << "#ifndef SAME_H\n#define SAME_H\nint from_a(void);\n#endif\n";
    }
    {
        std::ofstream os{b / "same.h", std::ios::binary};
        os << "#ifndef SAME_H\n#define SAME_H\nint from_b(void);\n#endif\n";
    }

    PreprocessResult const ra =
        pp("#include \"same.h\"\n", std::vector<fs::path>{a}, "ra.c");
    PreprocessResult const rb =
        pp("#include \"same.h\"\n", std::vector<fs::path>{b}, "rb.c");
    ASSERT_FALSE(ra.diagnostics->hasErrors());
    ASSERT_FALSE(rb.diagnostics->hasErrors());

    auto const la = significantLexemes(ra);
    auto const lb = significantLexemes(rb);
    auto contains = [](std::vector<std::string> const& v, char const* w) {
        for (std::string const& s : v) if (s == w) return true;
        return false;
    };
    EXPECT_TRUE(contains(la, "from_a"));
    EXPECT_FALSE(contains(la, "from_b"))
        << "a header from directory b leaked into the unit that included a's — "
           "the pre-scan memo is keyed on the include SPELLING rather than on "
           "the resolved file's identity";
    EXPECT_TRUE(contains(lb, "from_b"));
    EXPECT_FALSE(contains(lb, "from_a"));
}

TEST(PpIncludeNoRework, AnEditedHeaderIsNotServedFromTheMemo) {
    // The memo's key carries size + last-write-time precisely so a file that
    // CHANGES inside one process misses rather than being served stale. A
    // compile does not normally edit its own inputs — but a cache that CANNOT
    // notice would be a silent wrong answer, and this is the case that says it
    // notices.
    test_support::ScratchDir scratch{test_support::Location::Temp, "pp-include-edited"};
    fs::path const dir = scratch.path();
    fs::create_directories(dir);
    fs::path const h = dir / "mutable.h";
    {
        std::ofstream os{h, std::ios::binary};
        os << "int before_edit(void);\n";
    }
    std::vector<fs::path> const dirs{dir};
    PreprocessResult const first = pp("#include \"mutable.h\"\n", dirs, "e1.c");
    ASSERT_FALSE(first.diagnostics->hasErrors());

    // The stamp the memo recorded when it took its entry, captured BEFORE the
    // rewrite so this case can PROVE the stamp moved instead of assuming it.
    auto const stampInMemo = fs::last_write_time(h);

    // Rewrite with DIFFERENT content AND a different length.
    {
        std::ofstream os{h, std::ios::binary};
        os << "int after_the_edit_and_longer(void);\n";
    }

    // Force a stamp the memo cannot match, by measuring the filesystem's
    // granularity rather than guessing a margin. See `forceADistinctWriteTime`.
    forceADistinctWriteTime(h, stampInMemo);

    PreprocessResult const second = pp("#include \"mutable.h\"\n", dirs, "e2.c");
    ASSERT_FALSE(second.diagnostics->hasErrors());

    auto const l2 = significantLexemes(second);
    bool sawNew = false;
    bool sawOld = false;
    for (std::string const& s : l2) {
        if (s == "after_the_edit_and_longer") sawNew = true;
        if (s == "before_edit") sawOld = true;
    }
    EXPECT_TRUE(sawNew) << "the edited header's new content did not reach the "
                           "unit";
    EXPECT_FALSE(sawOld)
        << "the PREVIOUS content of an edited header was served out of the "
           "per-file pre-scan memo — the key is not carrying the file's size "
           "and write time";
}

TEST(PpIncludeNoRework, AHeaderRewrittenToTheSameLengthIsNotServedFromTheMemo) {
    // ISOLATES THE MTIME COMPONENT, and it exists because its sibling above
    // cannot. That case rewrites to a DIFFERENT length, so the `size` half of
    // `PreScanKey` already forces the miss on its own and the case stays green
    // even if `mtime` were dropped from the key entirely. Here the replacement
    // text is byte-for-byte the SAME LENGTH as the original and the file is the
    // same file, so identity and size both still match: the write time is the
    // ONLY thing left that can produce a miss. Drop `mtime` from the key and
    // this case goes red while its sibling stays green, which is what makes the
    // two a decomposition of the key rather than a duplicate of one assertion.
    test_support::ScratchDir scratch{test_support::Location::Temp, "pp-include-same-length"};
    fs::path const dir = scratch.path();
    fs::create_directories(dir);
    fs::path const h = dir / "same_length.h";
    {
        std::ofstream os{h, std::ios::binary};
        os << "int aaa_before(void);\n";
    }
    std::vector<fs::path> const dirs{dir};
    PreprocessResult const first = pp("#include \"same_length.h\"\n", dirs, "s1.c");
    ASSERT_FALSE(first.diagnostics->hasErrors());

    auto const stampInMemo = fs::last_write_time(h);
    auto const sizeInMemo = fs::file_size(h);

    // Same byte count, different identifier.
    {
        std::ofstream os{h, std::ios::binary};
        os << "int aaa_afterx(void);\n";
    }
    // The premise of this case, asserted rather than trusted: if the rewrite
    // changed the length then the size half of the key would do the work and
    // this case would prove nothing about mtime.
    ASSERT_EQ(fs::file_size(h), sizeInMemo)
        << "the two texts are not the same length, so this case is no longer "
           "isolating the mtime component of the memo key";

    // Same construction as the sibling, and here it is the ONLY thing that can
    // produce the miss.
    forceADistinctWriteTime(h, stampInMemo);

    PreprocessResult const second = pp("#include \"same_length.h\"\n", dirs, "s2.c");
    ASSERT_FALSE(second.diagnostics->hasErrors());

    auto const l2 = significantLexemes(second);
    bool sawNew = false;
    bool sawOld = false;
    for (std::string const& s : l2) {
        if (s == "aaa_afterx") sawNew = true;
        if (s == "aaa_before") sawOld = true;
    }
    EXPECT_TRUE(sawNew) << "the edited header's new content did not reach the "
                           "unit";
    EXPECT_FALSE(sawOld)
        << "a header whose SIZE was unchanged was served stale out of the "
           "per-file pre-scan memo - the key's mtime component is not carrying "
           "its weight, so only a length change can invalidate an entry";
}

}  // namespace
