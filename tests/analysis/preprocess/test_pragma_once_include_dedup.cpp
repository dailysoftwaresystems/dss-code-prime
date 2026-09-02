// D-PP-PRAGMA-RECOGNIZED-SEMANTICS — `#pragma once` HAS AN EFFECT.
//
// Until 2026-08-29 DSS did not merely ignore `#pragma once`: `applyPragma`'s
// `IncludeOnce` arm REFUSED THE TRANSLATION UNIT (`P_PreprocessorPragma`, exit
// 1). ✔MEASURED through the CLI at `94971261`, one `#include "h.h"` twice:
//     error[P_PreprocessorPragma]: pragma 'once' declares this file
//     include-once ... This implementation has not built include-once dedup
// So every real-world header using the commonest guard idiom failed to compile,
// against an idiom gcc, clang and MSVC all accept.
//
// ★★★ THE KEY IS IDENTITY, NOT CONTENT — operator ruling, 2026-08-28. The cases
// below are the SEVEN the ruling is about, and they are split into the ones that
// MUST dedup and the ones that MUST NOT. ✔MEASURED 2026-08-29 against all four
// references SEPARATELY (WSL gcc 13.3.0, WSL clang 18.1.3, mingw-w64 gcc 13.2.0,
// MSVC 19.51.36252), one self-contained program per question — the header
// defines a struct and a `static`, so a second TEXTUAL inclusion is a hard
// redefinition and the exit code answers "did it dedup?":
//
//   case                       gcc   clang  mingw   MSVC    DSS (this file)
//   `h.h` twice                DEDUP DEDUP  DEDUP   DEDUP   DEDUP
//   `./h.h`                    DEDUP DEDUP  DEDUP   DEDUP   DEDUP
//   `sub/../h.h`               DEDUP DEDUP  DEDUP   DEDUP   DEDUP
//   symlink                    DEDUP DEDUP  (n/a)   (n/a)   DEDUP
//   hard link                  DEDUP DEDUP  DEDUP   NO      DEDUP
//   two byte-identical files   DEDUP NO     NO      NO      NO   <- ruled
//   `#pragma once` in `#if 0`  NO    NO     NO      NO      NO
//
// ⛔ THE ONE DELIBERATE DIVERGENCE IS THE BYTE-IDENTICAL-COPY ROW, AND IT IS A
// COST THIS PROJECT CHOSE RATHER THAN AN OVERSIGHT — DSS REFUSES A PROGRAM WSL
// gcc COMPILES. Content-keying does not merely accept MORE, it silently OMITS
// TEXT in a constructible case: two byte-identical headers whose meaning differs
// because a macro was redefined between the two `#include`s, where gcc skips the
// second. That is a silent wrong-program. ⚠ Note the measurement ALSO refuted
// the "gcc dedups" premise as a statement about gcc in general: mingw-w64 gcc
// 13.2.0 does NOT dedup a byte-identical copy, so the content-keying vote is one
// of four, not one of three. Do not "fix" this test to make the copy dedup.
//
// ★★ THE DEAD-BRANCH ROW IS THE ONE NOBODY ASKED FOR AND IT IS WHY THE RECORD
// IS GATED. A `#pragma once` inside `#if 0` must NOT fire — unanimous across all
// four references. The pre-existing `detectIncludeOnceMechanism` is documented as
// "deliberately NOT gated on the conditional stack", which was correct for ITS
// reader (it gates whether re-entry is PERMITTED, where over-recognition merely
// splices again). Here over-recognition DROPS CONTENT, so the safe direction is
// inverted and the recording arm uses the pre-scan's confidently-live oracle.
//
// ★ RED-ON-DISABLE. Delete either `includeOnce.alreadySpliced(...)` skip in
// `SynthBuilder` (angle or quote arm) and `DedupsAPlainRepeatedInclude`,
// `DedupsAcrossPathSpellings` and `DedupsAHardLink` go red on the SPLICE COUNT;
// revert `applyPragma`'s `IncludeOnce` arm to its refusal and
// `NoLongerRefusesTheTranslationUnit` goes red on `P_PreprocessorPragma`. The
// two MUST-NOT arms plus `ControlNoPragmaIsSplicedTwice` are the negative
// controls that keep a "fix" from deduping everything in sight.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"

#include "test_support/repo_root.hpp"
#include "test_support/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// The token the header contributes. Counting it in the lexeme stream is the
// DIRECT observable for "how many times was this file's text spliced?" — a
// stronger and more local property than watching a downstream redefinition
// error, which would also fire for reasons that have nothing to do with this row.
constexpr std::string_view kMarker = "pp_once_marker";

// A header that declares itself include-once and contributes exactly one
// `kMarker` per splice.
constexpr std::string_view kOnceHeader = "#pragma once\nint pp_once_marker;\n";

// The same header with the pragma buried in a NOT-TAKEN branch. C 6.10p1 makes
// an elided pragma entirely silent, so this must behave like a header with no
// include-once mechanism at all.
constexpr std::string_view kDeadPragmaHeader =
    "#if 0\n#pragma once\n#endif\nint pp_once_marker;\n";

// The CONTROL header: no include-once mechanism of any kind.
constexpr std::string_view kPlainHeader = "int pp_once_marker;\n";

// Shared schema fixture — a REFERENCE to a function-local static, because
// `GrammarSchema`'s accessors hand back references INTO the schema and a
// by-value return makes `helper()->accessor()` a heap-use-after-free
// (D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE).
//
// ⚠ FORCED BEFORE THE CWD MOVES — schema discovery falls back to a walk UP FROM
// THE CWD when `$DSS_CONFIG_ROOT` is unset, which is the case for a bare `.exe`
// run outside ctest.
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSchema() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            // THROW, never `std::abort()`: abort kills the whole test BINARY and
            // every sibling test loses its verdict.
            throw std::runtime_error{"loadShipped(c) failed"};
        }
        return *loaded;
    }();
    return schema;
}

// A scratch directory that is also the process cwd, so a quote include resolves
// through the includer-directory arm with NO `-I` path involved.
struct OnceFixture {
    test_support::ScratchDir dir{test_support::Location::InsideRepo,
                                 "preprocess-pragma-once"};

    OnceFixture() {
        (void)cSchema();   // force the static BEFORE the cwd moves
        dir.useAsCwd();
    }

    void write(std::string_view relName, std::string_view bytes) const {
        fs::path const p = dir.path() / fs::path{relName};
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] fs::path pathOf(std::string_view relName) const {
        return dir.path() / fs::path{relName};
    }
};

[[nodiscard]] PreprocessResult ppMain(std::string text) {
    auto schema = cSchema();
    auto buf = SourceBuffer::fromString(std::move(text), std::string{"main.c"});
    std::vector<fs::path> const noDirs;
    return preprocess(buf, schema, noDirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

// The same, with an `-I` path — so an ANGLE `#include <h.h>` resolves to a real
// SOURCE file on that path and takes the angle-source include arm.
[[nodiscard]] PreprocessResult ppMainWithDirs(std::string text,
                                              fs::path const& dir) {
    auto schema = cSchema();
    auto buf = SourceBuffer::fromString(std::move(text), std::string{"main.c"});
    std::vector<fs::path> const dirs{dir};
    return preprocess(buf, schema, dirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

// How many times `kMarker` survives into the token stream = how many times the
// header's text was spliced.
[[nodiscard]] std::size_t spliceCount(PreprocessResult const& r) {
    std::size_t n = 0;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        if (r.synthBuffer->slice(t.span) == kMarker) ++n;
    }
    return n;
}

[[nodiscard]] bool hasCode(PreprocessResult const& r, DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all())
        if (d.code == code) return true;
    return false;
}

// ── THE MUST-DEDUP ARMS ─────────────────────────────────────────────────────

TEST(PragmaOnceIncludeDedup, DedupsAPlainRepeatedInclude) {
    OnceFixture fx;
    fx.write("h.h", kOnceHeader);

    PreprocessResult const r =
        ppMain("#include \"h.h\"\n#include \"h.h\"\n");

    EXPECT_EQ(spliceCount(r), 1u)
        << "a header carrying `#pragma once` and included twice must be spliced "
           "ONCE; all four references dedup this";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

TEST(PragmaOnceIncludeDedup, DedupsAcrossPathSpellings) {
    OnceFixture fx;
    fx.write("h.h", kOnceHeader);
    fx.write("sub/keep.txt", "x");   // make `sub/` exist so `sub/..` is walkable

    PreprocessResult const r = ppMain(
        "#include \"h.h\"\n#include \"./h.h\"\n#include \"sub/../h.h\"\n");

    EXPECT_EQ(spliceCount(r), 1u)
        << "`h.h`, `./h.h` and `sub/../h.h` are ONE file; a path-STRING key "
           "would splice three times";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

TEST(PragmaOnceIncludeDedup, DedupsAHardLink) {
    OnceFixture fx;
    fx.write("h.h", kOnceHeader);
    std::error_code ec;
    fs::create_hard_link(fx.pathOf("h.h"), fx.pathOf("hard.h"), ec);
    // A hard link is ONE file with two names on every filesystem this project
    // builds on. If the platform refuses, say so LOUDLY rather than passing
    // vacuously — a silent skip here would hide the very cell this arm exists
    // for (`PathIdentity` alone cannot answer it).
    ASSERT_FALSE(ec) << "could not create a hard link: " << ec.message();

    PreprocessResult const r =
        ppMain("#include \"h.h\"\n#include \"hard.h\"\n");

    EXPECT_EQ(spliceCount(r), 1u)
        << "two hard links name ONE file, so `#pragma once` covers both. "
           "✔MEASURED: WSL gcc, WSL clang and mingw-w64 gcc all dedup this "
           "(MSVC does not); `core::PathIdentity` normalises a PATH and cannot "
           "answer it, which is why the registry consults `fs::equivalent`";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

TEST(PragmaOnceIncludeDedup, DedupsASymlink) {
    OnceFixture fx;
    fx.write("h.h", kOnceHeader);
    std::error_code ec;
    fs::create_symlink(fx.pathOf("h.h"), fx.pathOf("link.h"), ec);
    if (ec) {
        // ⚠ A NON-VACUOUS SKIP, AND THE REASON IS MEASURED RATHER THAN GUESSED.
        // ✔MEASURED 2026-08-29 on the Windows leg: `fs::create_symlink` fails
        // with "Function not implemented" — libstdc++/MinGW does not implement
        // symlink CREATION on this host at all (its `read_symlink` reports the
        // same), so this is a platform gap and NOT a privilege setting the
        // operator could change. The POSIX legs (WSL x86_64, arm64 VPS, macOS)
        // implement it and DO run this arm.
        //
        // ⚠ THE CELL IS STILL COVERED ON WINDOWS, JUST NOT FROM HERE. A symlink
        // made with `cmd /c mklink` IS deduped by the shipped CLI — ✔MEASURED
        // through `dsscp --compile`. And `DedupsAHardLink` above exercises the
        // SAME `fs::equivalent` path unconditionally on every host, so a
        // regression in that mechanism cannot hide behind this skip.
        GTEST_SKIP() << "this host cannot create a symlink (" << ec.message()
                     << "); NOT a passing assertion about `#pragma once`";
    }

    PreprocessResult const r =
        ppMain("#include \"h.h\"\n#include \"link.h\"\n");

    EXPECT_EQ(spliceCount(r), 1u)
        << "a symlink and its target are ONE file. ⚠ MEASURED on the STL that "
           "builds DSS (libstdc++ 13.2, MinGW-w64 UCRT): `read_symlink` reports "
           "'Function not implemented' and `weakly_canonical` returns the LINK "
           "unchanged, so this cell is unreachable through path normalisation "
           "alone and `fs::equivalent` is what answers it";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// ── THE ANGLE ARM, WHICH IS A SECOND CALL SITE AND NOT THE SAME CODE ───────
//
// `SynthBuilder` splices includes from TWO arms — the quote arm and the
// angle-SOURCE arm (an angle header that is a real file on the `-I` path rather
// than a shipped JSON descriptor). Both consult the registry, and both had to be
// edited. Every arm above drives the QUOTE arm only, so without this one half
// the change would ship unpinned and a regression in the angle arm would be
// invisible — the "a partial fix reads as a complete one" failure, inside the
// test file that exists to prevent it.
TEST(PragmaOnceIncludeDedup, DedupsThroughTheAngleSourceIncludeArm) {
    OnceFixture fx;
    fx.write("h.h", kOnceHeader);

    PreprocessResult const r =
        ppMainWithDirs("#include <h.h>\n#include <h.h>\n", fx.dir.path());

    EXPECT_EQ(spliceCount(r), 1u)
        << "the angle-source include arm must consult the same include-once "
           "registry as the quote arm; two arms that disagree about what one "
           "file is are two implementations that agree until they don't";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

TEST(PragmaOnceIncludeDedup, DedupsAcrossTheQuoteAndAngleArmsTogether) {
    OnceFixture fx;
    fx.write("h.h", kOnceHeader);

    // The SAME file reached once through each arm. This is the property that a
    // per-arm registry would fail while both single-arm tests above still passed.
    PreprocessResult const r =
        ppMainWithDirs("#include \"h.h\"\n#include <h.h>\n", fx.dir.path());

    EXPECT_EQ(spliceCount(r), 1u)
        << "one file reached through the quote arm and the angle arm is still "
           "ONE file";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// ── THE MUST-NOT-DEDUP ARMS (the negative controls) ─────────────────────────

TEST(PragmaOnceIncludeDedup, DoesNotDedupTwoByteIdenticalFiles) {
    OnceFixture fx;
    fx.write("h.h", kOnceHeader);
    fx.write("copy.h", kOnceHeader);   // byte-identical, DIFFERENT file

    PreprocessResult const r =
        ppMain("#include \"h.h\"\n#include \"copy.h\"\n");

    EXPECT_EQ(spliceCount(r), 2u)
        << "THE OPERATOR RULED IDENTITY, NOT CONTENT (2026-08-28). Two "
           "byte-identical files are TWO files. WSL gcc 13.3.0 dedups them and "
           "DSS deliberately does not — content-keying buys its extra acceptance "
           "by DROPPING TEXT when a macro was redefined between the two "
           "`#include`s. Do not 'fix' this to 1";
}

TEST(PragmaOnceIncludeDedup, DoesNotFireFromANotTakenBranch) {
    OnceFixture fx;
    fx.write("dead.h", kDeadPragmaHeader);

    PreprocessResult const r =
        ppMain("#include \"dead.h\"\n#include \"dead.h\"\n");

    EXPECT_EQ(spliceCount(r), 2u)
        << "a `#pragma once` inside `#if 0` is ELIDED (C 6.10p1) and must not "
           "fire — unanimous across all four references. Deduping here would "
           "SILENTLY DROP the second splice, which is the dangerous direction";
}

TEST(PragmaOnceIncludeDedup, ControlNoPragmaIsSplicedTwice) {
    OnceFixture fx;
    fx.write("plain.h", kPlainHeader);

    PreprocessResult const r =
        ppMain("#include \"plain.h\"\n#include \"plain.h\"\n");

    // Without this arm every assertion above could pass because the fixture
    // never splices anything twice in the first place.
    EXPECT_EQ(spliceCount(r), 2u)
        << "a header with NO include-once mechanism must still be spliced twice; "
           "if this is 1 the dedup is unconditional and every other arm here is "
           "vacuous";
}

// ── THE REFUSAL THAT WAS THE DEFECT ────────────────────────────────────────

TEST(PragmaOnceIncludeDedup, NoLongerRefusesTheTranslationUnit) {
    OnceFixture fx;
    fx.write("h.h", kOnceHeader);

    PreprocessResult const r =
        ppMain("#include \"h.h\"\n#include \"h.h\"\n");

    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorPragma))
        << "`applyPragma`'s `IncludeOnce` arm used to refuse the whole "
           "translation unit. The pragma's effect is realized by the include "
           "machinery during the SPLICE — the only phase that can honour it — so "
           "this pass has nothing left to report";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// A `#pragma once` in the MAIN file is well-formed and inert: nothing includes
// it, so there is nothing to dedup, and it must not be reported either.
TEST(PragmaOnceIncludeDedup, IsAcceptedInTheMainSourceFile) {
    OnceFixture fx;

    PreprocessResult const r = ppMain("#pragma once\nint pp_once_marker;\n");

    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorPragma));
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(spliceCount(r), 1u);
}

}  // namespace
