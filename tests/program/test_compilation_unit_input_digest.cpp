// D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION — THE
// PREREQUISITE HALF: `CompilationUnit::inputDigest()`, the unit's TEXTUAL INPUT
// CLOSURE.
//
// ═══ WHY THIS SUBJECT EXISTS AT ALL ══════════════════════════════════════════
//
// A cross-run artifact cache keyed on anything the DRIVER can see is UNSOUND,
// and that is measured rather than argued: lane `dc` (cycle P46) built a
// `staticlib` dependency whose `fold.c` carries `#include "fold_impl.h"`, edited
// ONLY the header, and watched the archive move `227b82a7…` → `f09aa20c…` while
// every term a manifest-level key names — both manifests, both sources, the
// target, the derived format, the config — stayed byte-identical. A C
// dependency's `#include` closure is not in `sources[]`, not in any manifest and
// not under the config root, so nothing `dependency_resolver.cpp` can reach
// names it. A cache built on that key serves an artifact compiled against the
// OLD header with a clean exit code.
//
// ⇒ the compiler has to REPORT what it read. This file pins that report.
//
// ═══ WHAT EVERY CASE HERE IS SHAPED TO PROVE ════════════════════════════════
//
// A digest is only useful if BOTH halves hold, and they need separate cases:
//
//   * REPRODUCIBLE — two builds of identical inputs give the identical digest.
//     Without this, "the digest moved" means nothing, because it moves anyway.
//     `ADigestIsReproducibleAcrossTwoIdenticalBuilds` is that control, and every
//     move case below re-establishes it in its own run by MOVING and then
//     RESTORING, so the two claims are measured on ONE fixture rather than on
//     neighbouring ones.
//
//   * SENSITIVE — every class of input that can change the artifact moves it.
//     One case per class, and the classes are chosen because each is invisible
//     to a DIFFERENT plausible key:
//       - the main source            (visible to a manifest key; the sanity arm)
//       - a quote-`#include`d header (INVISIBLE to a manifest key — `dc`'s case)
//       - `#pragma pack`             (invisible to a TOKEN-ONLY key: the
//                                     directive is REMOVED from the token stream
//                                     and its effect survives only in an
//                                     out-of-band side table)
//
// ⚠⚠ AND ONE HONEST LIMIT, MEASURED RATHER THAN ASSUMED: the `#pragma pack`
// case below does NOT witness the digest's pragma-map term. ✔MEASURED
// 2026-08-31 by REMOVE-direction mutation — delete the whole pragma-pack
// rendering from `UnitBuilder::finish()` and this file stays GREEN (object md5
// moved and returned, build rc 0, control green, so the mutant was compiled in
// and read). The reason is that this project's synthesized buffer RETAINS
// directives, so `#pragma pack(1)` vs `#pragma pack(4)` already differ in the
// parse-source text and that term moves the digest first. The case still earns
// its place — it proves the digest is sensitive to a packing change, which is
// the property a cache key needs — but it does not prove WHICH term delivered
// it, and this comment says so rather than letting the file imply otherwise.
//
// ═══ AND ONE CASE THAT IS A MEASUREMENT, NOT A GUARD ════════════════════════
//
// `AuxiliaryBuffersIsNotTheInputClosure` exists to ANSWER a question lane `dc`
// raised and correctly refused to answer by reading: is
// `PreprocessResult::originBuffers` (surfaced as `CompilationUnit::
// auxiliaryBuffers()`) the input closure? It is collected from
// `lineMap.segments()` — buffers that contributed RENDERED TEXT for diagnostic
// positioning — so a header that contributes no segment is absent from it. As a
// closure it would fail toward HIT, which is the direction that ships wrong
// bytes. The case asserts the DIFFERENCE rather than trusting the reasoning:
// a header that `auxiliaryBuffers()` does not list is nevertheless one whose
// edit moves `inputDigest()`. [[feedback-an-instrument-that-answers-an-adjacent-question]].

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/grammar_schema.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using dss::CompilationUnit;
using dss::DiagnosticBudget;
using dss::GrammarSchema;
using dss::UnitBuilder;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// ⚠ THE LANGUAGE IS SPELLED, AND IT HAS TO BE. Every fixture below is a `.c`
// translation unit this file WRITES, so the language is a property of the source
// authored here rather than of the corpus being read. Nothing else in this file
// names a target, a format or a document.
constexpr std::string_view kLanguage = "c";

[[nodiscard]] std::shared_ptr<GrammarSchema const> cSchema() {
    auto loaded = GrammarSchema::loadShipped(kLanguage);
    if (!loaded.has_value()) {
        ADD_FAILURE() << "could not load the shipped '" << kLanguage
                      << "' language document";
        return nullptr;
    }
    return *loaded;
}

void write(fs::path const& p, std::string_view text) {
    std::ofstream out{p, std::ios::binary};
    out << text;
    out.close();
    ASSERT_TRUE(fs::exists(p)) << "failed to write " << p.string();
}

// Build a CU over `dir/main.c` and hand back its input digest.
//
// ★ Driven through `UnitBuilder::addFile` — the REAL input path — never by
// handing the builder a pre-made buffer. The quote-`#include` search starts at
// the INCLUDING FILE'S OWN DIRECTORY, which only a real on-disk file has; an
// in-memory source would resolve nothing and every header case here would be
// green over a closure that never contained a header at all.
[[nodiscard]] std::string digestOf(fs::path const& dir) {
    auto schema = cSchema();
    if (!schema) return {};
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addIncludeDir(dir);
    builder.addFile(dir / "main.c");
    CompilationUnit cu = std::move(builder).finish();
    return std::string{cu.inputDigest()};
}

// The tree's PARSE SOURCE text, i.e. what the preprocessor actually produced.
//
// ⚠ THIS IS A DIAGNOSTIC PROBE, NOT A SECOND DIGEST, and it exists because the
// first cut of this file could not tell two failures apart: a digest that MISSES
// the header, and a fixture whose `#include` never resolved so there was no
// header to miss. Both present as "the digest did not move", and only one of
// them is a defect in the subject. Reading the product text answers it directly.
[[nodiscard]] std::string parseSourceOf(fs::path const& dir) {
    auto schema = cSchema();
    if (!schema) return {};
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addIncludeDir(dir);
    builder.addFile(dir / "main.c");
    CompilationUnit cu = std::move(builder).finish();
    if (cu.trees().empty()) return {};
    return std::string{cu.trees().front().source().text()};
}

} // namespace

// ── THE CONTROL, AND EVERY MOVE CASE DEPENDS ON IT ──────────────────────────
//
// Two builds, identical inputs, identical digest. A digest that drifted on its
// own would make every `EXPECT_NE` below pass while measuring nothing — the
// exact shape of a test that is green for the wrong reason.
TEST(CompilationUnitInputDigest, ADigestIsReproducibleAcrossTwoIdenticalBuilds) {
    ScratchDir scratch{Location::Temp, "input-digest"};
    fs::path const dir = scratch.path();
    write(dir / "impl.h", "#define BIAS 2\n");
    write(dir / "main.c",
          "#include \"impl.h\"\n"
          "int fold(int v) { return v + v + BIAS; }\n");

    std::string const first  = digestOf(dir);
    std::string const second = digestOf(dir);

    ASSERT_FALSE(first.empty())
        << "an EMPTY digest means NOT COMPUTED, which a key builder must treat "
           "as a refusal — see CompilationUnit::inputDigest()";
    EXPECT_EQ(first.size(), 64u) << "the digest is 64 lowercase hex";
    EXPECT_EQ(first, second)
        << "identical inputs must produce an identical digest, or 'the digest "
           "moved' carries no information";
}

// ── THE SANITY ARM: the main source is in the closure ───────────────────────
TEST(CompilationUnitInputDigest, EditingTheMainSourceMovesTheDigest) {
    ScratchDir scratch{Location::Temp, "input-digest"};
    fs::path const dir = scratch.path();
    write(dir / "main.c", "int fold(int v) { return v + v + 2; }\n");

    std::string const before = digestOf(dir);
    write(dir / "main.c", "int fold(int v) { return v + v + 3; }\n");
    std::string const mutated = digestOf(dir);
    write(dir / "main.c", "int fold(int v) { return v + v + 2; }\n");
    std::string const restored = digestOf(dir);

    EXPECT_NE(before, mutated) << "the main source is an input";
    EXPECT_EQ(before, restored)
        << "MOVED and RETURNED, measured in one run — the restore half is what "
           "proves the move was attributable to the edit";
}

// ── ★★★ THE CASE THE WHOLE MECHANISM EXISTS FOR ─────────────────────────────
//
// `dc`'s measured silent-hit: edit ONLY a quote-`#include`d header. Every term
// a manifest-level key names is unchanged; the artifact is not.
TEST(CompilationUnitInputDigest, EditingOnlyAQuoteIncludedHeaderMovesTheDigest) {
    ScratchDir scratch{Location::Temp, "input-digest"};
    fs::path const dir = scratch.path();
    constexpr std::string_view kMain =
        "#include \"fold_impl.h\"\n"
        "int dss_fold_twice(int v) { return v + v + DSS_FOLD_BIAS; }\n";
    write(dir / "fold_impl.h", "#define DSS_FOLD_BIAS 2\n");
    write(dir / "main.c", kMain);

    std::string const before     = digestOf(dir);
    std::string const beforeText = parseSourceOf(dir);
    // THE HEADER ALONE. `main.c` is not rewritten at any point in this case, so
    // nothing a manifest can see changes.
    //
    // ★★ THE REPLACEMENT IS THE SAME *LENGTH* AGAIN, AND THE HISTORY IS THE
    // POINT. `2` → `3` is a 25-byte edit both ways, and it is the edit this
    // case was ORIGINALLY written with. It was widened to `30` — a different
    // NUMBER OF BYTES — in cycle P46, not for anything about `inputDigest()`
    // but to route around a defect in a DIFFERENT component: the per-file
    // pre-scan memo in `src/analysis/preprocess/preprocessor.cpp` was keyed on
    // `(PathIdentity, size, mtime)`, so a same-size edit inside one filesystem
    // timestamp tick addressed the OLD entry and the COMPILER read the old
    // header. ✔The digest reported the compile that actually happened, which is
    // how that defect was found: the digest was never the defect, it was the
    // instrument.
    // ⇒ RESTORED to the same-length edit now that
    // D-PP-PRE-SCAN-MEMO-SERVES-A-SAME-SIZE-EDIT-INSIDE-ONE-TIMESTAMP-TICK-STALE
    // is CLOSED (P47 lane `mm`, memo re-keyed on the file's content digest).
    // This is the STRONGER fixture of the two: it moves the header's VALUE
    // while holding every `stat`-visible property of the file constant, so it
    // exercises the digest against the narrowest possible input change.
    write(dir / "fold_impl.h", "#define DSS_FOLD_BIAS 3\n");
    std::string const mutated     = digestOf(dir);
    std::string const mutatedText = parseSourceOf(dir);

    // ⚠ THE FIXTURE'S OWN PRECONDITION, CHECKED FIRST. If the product text did
    // not move, the `#include` never resolved and this case is measuring an
    // unresolved directive rather than a header. ✔MEASURED through the CLI on
    // the same shape: the two headers produce DIFFERENT archives
    // (`9d7d2d6f…` vs `1d9697d6…`, returning to `9d7d2d6f…` on restore), so a
    // fixture in which nothing moves is a broken fixture, never a correct hit.
    ASSERT_NE(beforeText, mutatedText)
        << "the preprocessor's product text did not change, so the quote "
           "`#include` did not resolve and this case cannot see a header at "
           "all — fix the fixture before reading the digest assertion below";
    write(dir / "fold_impl.h", "#define DSS_FOLD_BIAS 2\n");
    std::string const restored = digestOf(dir);

    EXPECT_NE(before, mutated)
        << "a header that no manifest lists is still an input, and this is the "
           "exact edit that produced a different archive with an identical "
           "manifest-level key";
    EXPECT_EQ(before, restored);
}

// ── A PACKING CHANGE MOVES THE DIGEST ───────────────────────────────────────
//
// `#pragma pack` is REMOVED from the preprocessed token stream; its effect
// reaches layout through an out-of-band side table keyed by synth offset. A
// cache key blind to it would serve an archive whose structures are laid out
// under a DIFFERENT packing — a silent miscompile with a clean exit code.
//
// ⚠ WHAT THIS CASE PROVES AND WHAT IT DOES NOT: it proves the digest MOVES.
// ✔MEASURED by mutation that it does NOT prove the digest's pragma-map term is
// what moved it — see the ⚠⚠ note in this file's header. The parse-source text
// already carries the directive.
TEST(CompilationUnitInputDigest, ChangingPragmaPackMovesTheDigest) {
    ScratchDir scratch{Location::Temp, "input-digest"};
    fs::path const dir = scratch.path();
    auto const source = [](int packTo) {
        return "#pragma pack(" + std::to_string(packTo) +
               ")\n"
               "struct S { char c; int i; };\n"
               "#pragma pack()\n"
               "int width(void) { return (int)sizeof(struct S); }\n";
    };
    write(dir / "main.c", source(1));

    std::string const packed1 = digestOf(dir);
    write(dir / "main.c", source(4));
    std::string const packed4 = digestOf(dir);
    write(dir / "main.c", source(1));
    std::string const restored = digestOf(dir);

    EXPECT_NE(packed1, packed4)
        << "the pragma's EFFECT is outside the token stream, so a digest that "
           "covered only text and tokens would report these two units as the "
           "same input — and they lay out `struct S` differently";
    EXPECT_EQ(packed1, restored);
}

// ── THE MEASUREMENT `dc` ASKED FOR, ANSWERED BY EXECUTION ───────────────────
//
// Is `auxiliaryBuffers()` the input closure? This case does not assert a
// particular answer to "does this header appear"; it asserts the property that
// decides the design either way: `inputDigest()` is SENSITIVE to a header edit
// REGARDLESS of whether `auxiliaryBuffers()` lists that header. If the header is
// absent from `auxiliaryBuffers()`, that set fails toward HIT and must never be
// used as a closure. If it is present, it is still only a set of BUFFERS with no
// content digests, so it could not key anything on its own.
//
// The observed membership is REPORTED (via `RecordProperty`) rather than pinned,
// because it is a property of the line-map's construction — which belongs to
// `src/analysis/preprocess/`, not here — and a pin on it from this file would be
// a second owner of a decision that lane's tests get to make.
TEST(CompilationUnitInputDigest, AuxiliaryBuffersIsNotTheInputClosure) {
    ScratchDir scratch{Location::Temp, "input-digest"};
    fs::path const dir = scratch.path();
    // A `#define`-only header: it contributes NO product text of its own, which
    // is exactly the shape `dc` predicted would contribute no line-map segment.
    write(dir / "defs_only.h", "#define DSS_BIAS 2\n");
    write(dir / "main.c",
          "#include \"defs_only.h\"\n"
          "int fold(int v) { return v + DSS_BIAS; }\n");

    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    bool headerIsAnAuxiliaryBuffer = false;
    std::string before;
    {
        UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
        builder.addFile(dir / "main.c");
        CompilationUnit cu = std::move(builder).finish();
        before = std::string{cu.inputDigest()};
        for (auto const& buf : cu.auxiliaryBuffers()) {
            if (buf && std::string_view{buf->name()}.find("defs_only.h")
                           != std::string_view::npos) {
                headerIsAnAuxiliaryBuffer = true;
            }
        }
    }
    RecordProperty("defineOnlyHeaderIsAnAuxiliaryBuffer",
                   headerIsAnAuxiliaryBuffer ? "yes" : "no");

    // The SAME NUMBER OF BYTES, restored for the reason the header case above
    // records now that
    // D-PP-PRE-SCAN-MEMO-SERVES-A-SAME-SIZE-EDIT-INSIDE-ONE-TIMESTAMP-TICK-STALE
    // is closed: `2` → `3` holds size and mtime constant and moves only the
    // bytes, which is the narrowest input change this case can make.
    write(dir / "defs_only.h", "#define DSS_BIAS 3\n");
    std::string const mutated = digestOf(dir);

    EXPECT_NE(before, mutated)
        << "whatever auxiliaryBuffers() happens to contain, the input digest "
           "must move when a `#define`-only header changes — that set is "
           "collected for DIAGNOSTIC POSITIONING and is not a closure";
}
