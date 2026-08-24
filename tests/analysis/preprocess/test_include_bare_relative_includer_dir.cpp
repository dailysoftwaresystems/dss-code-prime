// D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH
//
// `dss --compile main.c` — the argument form a user types most — reported
// `quote include not found` for a header sitting right beside `main.c`, while
// `./main.c`, `sub/main.c` and an absolute path all resolved it. ✔MEASURED
// through the CLI, one variable changed per arm; gcc 13.2 (Windows host) and
// gcc 13.3 (WSL) both resolve the bare form, so by the reference-compilers rule
// the behaviour is REQUIRED.
//
// Root cause: FOUR sites derived the includer's directory as
// `fs::path{buffer-name}.parent_path()`, which is the EMPTY path for a name
// with no directory component, and BOTH quote resolvers skip their self-dir arm
// behind `if (!includingDir.empty())`. An empty includer directory is the
// PROCESS WORKING DIRECTORY, not "no directory". The fix is ONE shared
// derivation (`dss::includingDirectoryOf`) called from all four.
//
// ★ WHY THIS FILE EXISTS AND WHY EVERY TEST IN IT CHANGES THE CWD.
// The defect is only observable when the header the includer names is in the
// PROCESS WORKING DIRECTORY, because that is the directory a bare source name
// designates. Every other fixture in this directory writes its header into a
// scratch dir and passes that dir explicitly, which routes around the self-dir
// arm entirely and can never see this. So each test here takes an
// `InsideRepo` `ScratchDir` and `useAsCwd()`s it — the existing verbs, not a
// new one — and the dtor restores the cwd.
//
// ★ THE PIN MUST FAIL ON THE BARE FORM SPECIFICALLY. A test that preprocesses
// a buffer named `./main.c` is green against the broken compiler and asserts
// nothing about this row. `kBareName` below is the whole point; the `./` and
// `sub/` arms are CONTROLS that keep a "fix" from breaking what already worked.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/include_path_resolve.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"

#include "test_support/repo_root.hpp"     // the ONE repo/config-root resolver
#include "test_support/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// The source-buffer NAMES under test, as named constants rather than literals
// scattered through the bodies. `kBareName` carries NO directory component and
// is the one this row is about; the other two are the forms that already
// worked and must keep working.
constexpr std::string_view kBareName    = "m28.c";
constexpr std::string_view kDotName     = "./m28.c";
constexpr std::string_view kSubdirName  = "sub/m28.c";
constexpr std::string_view kHeaderName  = "h28.h";
constexpr std::string_view kNestedName  = "h29.h";

// Shared schema fixture. Returns a REFERENCE to a function-local static for the
// reason `test_preprocessor.cpp` spells out at length under
// D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE: `GrammarSchema`'s accessors hand
// back references INTO the schema, so a by-value return makes
// `helper()->accessor()` a heap-use-after-free.
//
// ⚠ CALL THIS BEFORE `useAsCwd()`, always. Schema discovery reads
// `$DSS_CONFIG_ROOT` first — which `dss_add_test` sets, so under `ctest` the
// order does not matter — but its fallback is a walk UP FROM THE CWD, and a
// bare `.exe` run has no such variable. Initialising the static while the cwd
// is still the one the process was launched in keeps this file's tests honest
// in both harnesses instead of only under ctest.
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSubsetSchema() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            // THROW, never `std::abort()`: abort kills the whole test
            // BINARY, so every sibling test in this executable loses its
            // verdict and the harness cannot say which unit failed.
            // GoogleTest reports a throw as a failure of this ONE test.
            // The rule and its measurement live in
            // tests/test_support/repo_root.hpp; it is machine-checked by
            // check-no-abort-in-tests, whose ratchet this had breached.
            throw std::runtime_error{"loadShipped(c) failed"};
        }
        return *loaded;
    }();
    return schema;
}

// Preprocess `text` as a buffer NAMED `name`, with NO include dirs at all — so
// the only way a quote include can resolve is the includer-directory arm, which
// is exactly the arm under test. An `-I` dir here would make every assertion
// below pass for the wrong reason.
[[nodiscard]] PreprocessResult ppNamed(std::string_view name, std::string text) {
    auto schema = cSubsetSchema();
    auto buf    = SourceBuffer::fromString(std::move(text), std::string{name});
    std::vector<fs::path> const noDirs;
    return preprocess(buf, schema, noDirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

[[nodiscard]] bool hasCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all())
        if (d.code == code) return true;
    return false;
}

[[nodiscard]] bool hasCode(PreprocessResult const& r, DiagnosticCode code) {
    return hasCode(*r.diagnostics, code);
}

// The non-trivia lexemes of a preprocess result, sliced from the synth buffer:
// what the parser would see.
[[nodiscard]] std::vector<std::string> lexemesOf(PreprocessResult const& r) {
    std::vector<std::string> lexs;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
    }
    return lexs;
}

// The name of the FIRST origin buffer whose filename is `wanted`, or empty when
// no such origin was spliced. `originBuffers` holds the main file plus every
// quote-`#include`'d header, so this is the RESOLVED PATH of a header — the
// value the fix's choice of `.` over `fs::current_path()` is about.
[[nodiscard]] std::string resolvedOriginNamed(PreprocessResult const& r,
                                              std::string_view wanted) {
    for (auto const& buf : r.originBuffers) {
        if (buf == nullptr) continue;
        std::string const name{buf->name()};
        if (fs::path{name}.filename() == fs::path{wanted}) return name;
    }
    return {};
}

// One fixture: an InsideRepo scratch dir that the ctor makes the process cwd.
// It writes NOTHING by itself — each test calls `write()` for the files its own
// arm needs, and the miss arm deliberately writes none. Once constructed,
// anything a bare source name calls "beside the includer" is in this directory.
struct CwdFixture {
    test_support::ScratchDir dir{test_support::Location::InsideRepo,
                                 "preprocess-includer-dir"};

    CwdFixture() {
        // Force the schema static BEFORE the cwd moves — see cSubsetSchema().
        (void)cSubsetSchema();
        dir.useAsCwd();
    }

    void write(std::string_view relName, std::string_view bytes) const {
        fs::path const p = dir.path() / fs::path{relName};
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

// ── THE ROW'S PIN ──────────────────────────────────────────────────────────
// RED-ON-DISABLE (MEASURED): revert any of the four `includingDirectoryOf`
// call sites to `fs::path{...}.parent_path()` and this fails with
// `P_PreprocessorIncludeError` — the CLI's `error[P0016] quote include not
// found: h28.h` seen from the inside.
TEST(IncluderDirectory, BareRelativeSourceNameResolvesAHeaderBesideIt) {
    CwdFixture fx;
    fx.write(kHeaderName, "#define H28_VALUE 7\n");

    PreprocessResult const r =
        ppNamed(kBareName, "#include \"h28.h\"\nint v = H28_VALUE;\n");

    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "a header in the working directory IS beside a source named with no "
           "directory component; refusing it is the defect this row records";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // The strongest available property: the header was not merely FOUND, its
    // text was spliced and its macro expanded.
    auto const lexs = lexemesOf(r);
    ASSERT_EQ(lexs.size(), 5u) << "expected `int v = 7 ;`";
    EXPECT_EQ(lexs[3], "7")
        << "the included header's macro must have expanded, so the splice "
           "really happened rather than the directive being dropped";
}

// The REFUSAL must survive the fix. A change that made the search succeed by
// weakening the miss report would pass the test above and break this one.
TEST(IncluderDirectory, BareRelativeSourceNameStillFailsLoudOnAGenuineMiss) {
    CwdFixture fx;   // deliberately writes NO header

    PreprocessResult const r =
        ppNamed(kBareName, "#include \"h28.h\"\nint v = 1;\n");

    EXPECT_TRUE(hasCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "a header that is genuinely nowhere must still fail LOUD; the row is "
           "about making the search correct, never about softening the miss";
    EXPECT_TRUE(r.diagnostics->hasErrors());
}

// WHY `.` AND NOT `fs::current_path()`. The bare form must resolve the header
// to the SAME path the `./` form does; an absolute substitution would make the
// two disagree and would push an absolute path into `__FILE__`, diagnostics and
// the line-map for every self-dir hit. RED-ON-DISABLE (MEASURED): return
// `fs::current_path()` from `includingDirectoryOf` and this fails on the
// equality below while every other test in this file stays green.
TEST(IncluderDirectory, BareAndDotSlashFormsResolveTheHeaderToTheSamePath) {
    CwdFixture fx;
    fx.write(kHeaderName, "#define H28_VALUE 7\n");

    std::string const text = "#include \"h28.h\"\nint v = H28_VALUE;\n";
    PreprocessResult const bare = ppNamed(kBareName, text);
    PreprocessResult const dot  = ppNamed(kDotName, text);

    std::string const bareHeader = resolvedOriginNamed(bare, kHeaderName);
    std::string const dotHeader  = resolvedOriginNamed(dot, kHeaderName);
    ASSERT_FALSE(bareHeader.empty()) << "the bare form must splice the header";
    ASSERT_FALSE(dotHeader.empty())  << "the ./ form must splice the header";
    EXPECT_EQ(bareHeader, dotHeader)
        << "`main.c` and `./main.c` name the same file, so they must resolve "
           "their quote includes to the same path — byte for byte";
    EXPECT_FALSE(fs::path{bareHeader}.is_absolute())
        << "the self-dir arm must not turn a relative include into an absolute "
           "path; that value reaches __FILE__ and every diagnostic";
}

// The already-working arms are CONTROLS: a fix that reached them would be a
// regression, and one that broke them would be caught here rather than in the
// corpus.
TEST(IncluderDirectory, SubdirectoryRelativeSourceNameResolvesAgainstItsOwnDir) {
    CwdFixture fx;
    fx.write("sub/h28.h", "#define H28_VALUE 7\n");
    // A DECOY of the same name in the cwd, holding a DIFFERENT value. If the
    // subdir form ever fell back to the working directory this would expand to
    // 9 instead of 7 — so this arm pins the search ORDER too, not just success.
    fx.write(kHeaderName, "#define H28_VALUE 9\n");

    PreprocessResult const r =
        ppNamed(kSubdirName, "#include \"h28.h\"\nint v = H28_VALUE;\n");

    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto const lexs = lexemesOf(r);
    ASSERT_EQ(lexs.size(), 5u) << "expected `int v = 7 ;`";
    EXPECT_EQ(lexs[3], "7")
        << "a source named `sub/m28.c` must resolve `h28.h` from `sub/`, never "
           "from the working directory";
}

// Nested includes at depth > 0: the recursion derives the CHILD's includer dir
// from the header's own resolved name, so the chain must keep resolving once
// the root is `.` rather than empty.
TEST(IncluderDirectory, NestedQuoteIncludeResolvesUnderABareRelativeSourceName) {
    CwdFixture fx;
    fx.write(kHeaderName, "#include \"h29.h\"\n");
    fx.write(kNestedName, "#define H29_VALUE 7\n");

    PreprocessResult const r =
        ppNamed(kBareName, "#include \"h28.h\"\nint v = H29_VALUE;\n");

    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorIncludeError));
    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto const lexs = lexemesOf(r);
    ASSERT_EQ(lexs.size(), 5u) << "expected `int v = 7 ;`";
    EXPECT_EQ(lexs[3], "7") << "the transitive include must resolve too";
}

// `__has_include("h")` reaches the includer directory through a DIFFERENT
// derivation site (the `MacroExpander`'s retained includer dir) than the
// directive does (the `SynthBuilder` scan). The header of
// `include_path_resolve.hpp` states the invariant those two exist to keep:
// `__has_include` must give the SAME answer `#include` would. Fixing only the
// directive's site would have CREATED that divergence rather than closed it.
TEST(IncluderDirectory, HasIncludeAgreesWithTheDirectiveOnABareRelativeSource) {
    CwdFixture fx;
    fx.write(kHeaderName, "#define H28_VALUE 7\n");

    PreprocessResult const r = ppNamed(
        kBareName,
        "#if __has_include(\"h28.h\")\nint yes;\n#else\nint no;\n#endif\n");

    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto const lexs = lexemesOf(r);
    ASSERT_EQ(lexs.size(), 3u) << "expected `int yes ;`";
    EXPECT_EQ(lexs[1], "yes")
        << "__has_include must answer 1 for exactly the header #include "
           "resolves; a 0 here is the two tiers disagreeing";
}

// `#embed "resource"` resolves through the THIRD derivation site (the per-origin
// `embedResolutionDir` line-map lookup), so it needs its own arm: C23 6.10.4
// makes the quote search "as for #include", which means the working directory
// for a source named without one.
TEST(IncluderDirectory, EmbedResolvesAResourceBesideABareRelativeSource) {
    CwdFixture fx;
    fx.write("r28.bin", std::string_view{"\x01\x02\x03", 3});

    PreprocessResult const r =
        ppNamed(kBareName, "int d[] = {\n#embed \"r28.bin\"\n};\n");

    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorEmbed))
        << "the resource is in the working directory, which IS the directory of "
           "a source named `m28.c`";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto const lexs = lexemesOf(r);
    // `int d [ ] = { 1 , 2 , 3 } ;`
    ASSERT_GE(lexs.size(), 12u);
    EXPECT_EQ(lexs[6], "1");
    EXPECT_EQ(lexs[8], "2");
    EXPECT_EQ(lexs[10], "3");
}

// ── THE FOURTH SITE: the import resolver's post-parse directive walk ───────
//
// ★ IT IS UNREACHABLE FOR EVERY SHIPPED LANGUAGE AS CONFIGURED, AND THAT IS A
// REASON TO BUILD THE SCHEMA THAT REACHES IT, NOT A REASON TO LEAVE IT
// UNPINNED. `ImportResolver` skips the QUOTE form entirely when the
// config-selected preprocessor is enabled (the PP owns quote includes end to
// end and already diagnosed any failure; re-reporting would double-diagnose).
// ✔MEASURED over `src/dss-config/sources/*.lang.json`: `c` is the only
// shipped language declaring an include directive at all, and it enables the
// preprocessor — so the two conditions are exactly complementary and the arm
// never runs today. A language with includes and NO preprocessor is a
// perfectly legal config, so the arm is live code with no live caller; the
// schema below is that caller, built by flipping ONE key of the shipped
// c document rather than by inventing a language.
[[nodiscard]] std::shared_ptr<GrammarSchema const> cSubsetWithPreprocessorOff() {
    fs::path const path =
        dss::test::configRoot() / "sources" / "c.lang.json";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read the shipped c config: "
                      << path.string();
        return nullptr;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    // The shipped document carries exactly ONE `"enabled"` key (the preprocess
    // block's), so this rebind is unambiguous. If that ever stops being true
    // the count check below reddens rather than silently flipping the wrong one.
    constexpr std::string_view kOn = "\"enabled\":             true";
    std::size_t const first = text.find(kOn);
    if (first == std::string::npos
        || text.find(kOn, first + kOn.size()) != std::string::npos) {
        ADD_FAILURE() << "the shipped c config no longer carries exactly "
                         "one `" << kOn << "` — rebind the preprocess `enabled` "
                         "key explicitly instead of by unique match";
        return nullptr;
    }
    text.replace(first, kOn.size(), "\"enabled\":             false");
    auto loaded = GrammarSchema::loadFromText(text, "<c, preprocess off>");
    if (!loaded.has_value()) {
        ADD_FAILURE() << "loadFromText(c with preprocess off) failed";
        return nullptr;
    }
    return *loaded;
}

TEST(IncluderDirectory, ImportResolverQuoteArmResolvesUnderABareRelativeSource) {
    CwdFixture fx;
    fx.write(kHeaderName, "int helper28(void) { return 7; }\n");

    auto schema = cSubsetWithPreprocessorOff();
    ASSERT_NE(schema, nullptr);
    ASSERT_FALSE(schema->preprocess().enabled)
        << "this test is only meaningful against the arm the PP-enabled config "
           "skips; a still-enabled preprocessor would make it assert nothing";

    UnitBuilder b{schema, DiagnosticBudget::libraryDefault()};
    b.addInMemory("#include \"h28.h\"\nint main(void) { return helper28(); }\n",
                  std::string{kBareName});
    auto cu = std::move(b).finish();

    EXPECT_FALSE(hasCode(cu.driverDiagnostics(), DiagnosticCode::D_UnresolvedImport))
        << "the post-parse quote arm must reach the working directory for a "
           "tree whose source name carries no directory component";
    // The header became a SECOND tree joined by a CrossTreeRef edge — the
    // post-parse include-following shape, as distinct from the preprocessor's
    // textual splice. Asserting the edge (not just the absence of a warning)
    // is what makes this fail if the include silently resolved to nothing.
    ASSERT_EQ(cu.trees().size(), 2u)
        << "the resolved header is loaded as its own tree under this schema";
    EXPECT_EQ(cu.crossRefs().size(), 1u);
}

// The derivation itself, at the level the four sites share. The EMPTY-name arm
// is the discriminating one: it is why this is a derivation and not a `.`
// substitution buried in the two resolvers, which would collapse "there is no
// including file" into "the including file is in the working directory".
TEST(IncluderDirectory, IncludingDirectoryOfSubstitutesDotOnlyForABareName) {
    EXPECT_EQ(includingDirectoryOf("m28.c"), fs::path{"."})
        << "a name with no directory component designates the working directory";
    EXPECT_EQ(includingDirectoryOf("sub/m28.c"), fs::path{"sub"});
    EXPECT_EQ(includingDirectoryOf("./m28.c"), fs::path{"."});
    EXPECT_EQ(includingDirectoryOf(""), fs::path{})
        << "no NAME means no including FILE, which must stay distinguishable "
           "from an includer that lives in the working directory — the callers' "
           "`!includingDir.empty()` guard is what reads that distinction";

    fs::path const abs = fs::temp_directory_path() / "m28.c";
    EXPECT_EQ(includingDirectoryOf(abs.string()), abs.parent_path())
        << "an absolute name keeps its real parent, untouched";
}

} // namespace
