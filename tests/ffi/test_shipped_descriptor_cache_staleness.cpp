// D-FFI-SHIPPED-DESCRIPTOR-PARSE-CACHE-SERVES-A-STALE-DOCUMENT (P42) — the
// shipped-descriptor PARSE CACHE must answer about the file that is on disk NOW.
//
// WHAT WAS MEASURED, AND WHY THIS IS A PRODUCTION DEFECT RATHER THAN A TEST TRAP.
//
// `cachedDescriptorJson` shipped keyed on the canonical PATH with no staleness
// check at all, on the reasoning that "a descriptor cannot change under a build".
// The cache is `thread_local`, so its real lifetime is the THREAD's — and
// `dsscp --lsp` is a long-running server that re-runs a FULL preprocess +
// `analyze` per `textDocument/didChange` (i.e. per keystroke) on a persistent
// `ThreadPool` worker (`LspServer::enqueueParse_` → `IExecutor::submit`, a real
// `ThreadPool` in production and only a `SynchronousExecutor` in tests). A worker
// that read `stdio.json` at 09:00 therefore answered every later question in that
// session from the 09:00 document, whatever `git pull`, a branch switch or an
// edit did to the file at 09:05 — silently, with no diagnostic. The server even
// handles `workspace/didChangeWatchedFiles`, i.e. it is built on the premise that
// files DO change under it.
//
// ✔MEASURED END-TO-END against a real `dsscp --lsp` process: one session, one
// buffer, `didOpen` then `didChange`, with a shipped descriptor made MALFORMED on
// disk in between. The session that had already read the descriptor kept
// reporting the pre-mutation answer; a session started AFTER the mutation
// reported the malformed descriptor. See the run recorded in the row.
//
// ✔MEASURED, the two facts that chose the mechanism (they are in the long note at
// `cachedDescriptorJson`, and the second one refuted the obvious design):
//   * `std::filesystem::last_write_time` — the call a portable guard would make —
//     has ONE-SECOND granularity on this host/toolchain: 4000 in-place rewrites
//     of one file produced 3 distinct stamps, minimum non-zero delta exactly
//     1.000000000 s. A `(path, mtime, size)` key is blind to every same-second
//     rewrite.
//   * That stamp is not even cheap: `last_write_time` + `file_size` measured
//     61-76 µs/file against a whole bulk read at 73 µs/file and `json::parse` at
//     98-126 µs/file over the 49-file shipped corpus. Two `fs::` queries cost
//     about what reading the file costs, so the "cheap but approximate stamp"
//     trade does not exist here — the read is the same price and it is EXACT.
//
// ⇒ the cache is CONTENT-VALIDATED, and this file pins that in both directions:
// a changed file is re-parsed (the defect), and an UNCHANGED file is still served
// without re-parsing (the c112 win the cache exists for). The second half matters
// as much as the first: "delete the cache" would pass every staleness assertion
// here while silently undoing the compile-perf work.
//
// ★ THE PIN IS DELIBERATELY DESIGN-LOCKING. `SameSizeSameStampRewriteIsSeen`
// rewrites the descriptor to a byte-count-IDENTICAL document and then RESTORES
// the original `last_write_time`, so the mutation is provably invisible to a
// `(mtime, size)` key on every host — no reliance on the 1-second measurement
// above, and no flake at a second boundary. A future "optimization" back to a
// stamp key goes red here rather than in a user's editor.
//
// ⚠ NOTHING HERE TOUCHES `src/dss-config`. Every descriptor is written into a
// scratch directory and read by explicit path.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::ffi::readShippedLibMacros;
using dss::ffi::shippedDescriptorCacheStats;
using dss::ffi::ShippedDescriptorCacheStats;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// A minimal, well-formed descriptor carrying exactly the named object-like
// macros. `readShippedLibMacros` is the interner-free reader the preprocessor
// itself uses, so this drives the cache through a REAL production entry point
// rather than a test-only shim.
[[nodiscard]] std::string descriptorWith(std::vector<std::string> const& macroNames) {
    std::string s = "{\n  \"header\": \"dss-cache-probe.h\",\n  \"macros\": [";
    for (std::size_t i = 0; i < macroNames.size(); ++i) {
        if (i != 0) s += ",";
        s += "\n    { \"name\": \"" + macroNames[i] + "\", \"replacement\": \"1\" }";
    }
    s += "\n  ]\n}\n";
    return s;
}

// Write `text` to `p`, truncating. Fails loud: a mutation that did not reach
// disk would make every assertion below vacuous.
void writeFile(fs::path const& p, std::string const& text) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(out.good()) << "could not open " << p.generic_string();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    ASSERT_TRUE(out.good()) << "the write did not reach disk: " << p.generic_string();
}

[[nodiscard]] std::vector<std::string> macroNamesAt(fs::path const& p,
                                                    DiagnosticReporter& rep) {
    auto const macros = readShippedLibMacros(p, rep);
    std::vector<std::string> names;
    if (!macros) return names;
    for (auto const& m : *macros) names.push_back(m.name);
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] bool contains(std::vector<std::string> const& v, std::string_view n) {
    return std::find(v.begin(), v.end(), n) != v.end();
}

} // namespace

// ── 1. THE DEFECT ───────────────────────────────────────────────────────────
//
// An in-place rewrite that a `(mtime, size)` key CANNOT see — same byte count,
// and the original timestamp put back — must still be seen. Before the fix the
// second read returned the PRE-mutation document.
TEST(ShippedDescriptorParseCache, SameSizeSameStampRewriteIsSeen) {
    ScratchDir dir{Location::Temp, "shipped-descriptor-cache"};
    fs::path const p = dir.path() / "probe.json";

    std::string const before = descriptorWith({"AAA"});
    std::string const after  = descriptorWith({"BBB"});
    // FAIL-CLOSED: the mutation must be a real change that a size key is blind to.
    ASSERT_NE(before, after);
    ASSERT_EQ(before.size(), after.size());

    ASSERT_NO_FATAL_FAILURE(writeFile(p, before));
    std::error_code ec;
    auto const stamp = fs::last_write_time(p, ec);
    ASSERT_FALSE(ec) << "last_write_time failed: " << ec.message();
    auto const sizeBefore = fs::file_size(p, ec);
    ASSERT_FALSE(ec) << "file_size failed: " << ec.message();

    DiagnosticReporter rep1;
    auto const first = macroNamesAt(p, rep1);
    ASSERT_EQ(rep1.errorCount(), 0u);
    EXPECT_EQ(first, (std::vector<std::string>{"AAA"}));

    // Rewrite IN PLACE, then put the original stamp back: from here on the file
    // is byte-count-identical and timestamp-identical to what the cache holds,
    // and ONLY its CONTENT differs.
    ASSERT_NO_FATAL_FAILURE(writeFile(p, after));
    fs::last_write_time(p, stamp, ec);
    ASSERT_FALSE(ec) << "could not restore last_write_time: " << ec.message();
    ASSERT_EQ(fs::last_write_time(p, ec), stamp)
        << "the stamp restore did not take — this test's whole premise is that "
           "the mutation is invisible to a (mtime,size) key";
    ASSERT_EQ(fs::file_size(p, ec), sizeBefore);

    ShippedDescriptorCacheStats const s0 = shippedDescriptorCacheStats();
    DiagnosticReporter rep2;
    auto const second = macroNamesAt(p, rep2);
    ShippedDescriptorCacheStats const s1 = shippedDescriptorCacheStats();
    ASSERT_EQ(rep2.errorCount(), 0u);

    EXPECT_EQ(second, (std::vector<std::string>{"BBB"}));
    EXPECT_FALSE(contains(second, "AAA"))
        << "the pre-mutation macro is still being served: the parse cache "
           "answered about a file that changed under it";
    // The document was DISCARDED and RE-PARSED, not merely re-derived.
    EXPECT_EQ(s1.staleEvictions - s0.staleEvictions, 1u);
    EXPECT_EQ(s1.parses - s0.parses, 1u);
    EXPECT_EQ(s1.revalidatedHits - s0.revalidatedHits, 0u);
}

// ── 2. THE REMOVE DIRECTION ─────────────────────────────────────────────────
//
// The way this defect ARRIVES in the field is a descriptor that LOSES something,
// so the fixture synthesizes the NEGATIVE: a macro deleted from the document must
// stop being reported.
TEST(ShippedDescriptorParseCache, RemovingAMacroIsSeen) {
    ScratchDir dir{Location::Temp, "shipped-descriptor-cache"};
    fs::path const p = dir.path() / "probe.json";

    ASSERT_NO_FATAL_FAILURE(writeFile(p, descriptorWith({"KEPT", "DROPPED"})));
    DiagnosticReporter rep1;
    auto const first = macroNamesAt(p, rep1);
    ASSERT_EQ(rep1.errorCount(), 0u);
    ASSERT_TRUE(contains(first, "DROPPED"))
        << "the fixture never declared the macro it is about to remove";

    ASSERT_NO_FATAL_FAILURE(writeFile(p, descriptorWith({"KEPT"})));
    DiagnosticReporter rep2;
    auto const second = macroNamesAt(p, rep2);
    ASSERT_EQ(rep2.errorCount(), 0u);
    EXPECT_EQ(second, (std::vector<std::string>{"KEPT"}));
    EXPECT_FALSE(contains(second, "DROPPED"));
}

// ── 3. THE CACHE IS STILL A CACHE ───────────────────────────────────────────
//
// The other direction, and it is not optional: every assertion above is also
// satisfied by a cache that was simply deleted, which would silently undo the
// c112 compile-perf work (a single TU reads one descriptor up to 4×). An
// UNCHANGED file must be served from the already-parsed document.
TEST(ShippedDescriptorParseCache, UnchangedFileIsNotReparsed) {
    ScratchDir dir{Location::Temp, "shipped-descriptor-cache"};
    fs::path const p = dir.path() / "probe.json";
    ASSERT_NO_FATAL_FAILURE(writeFile(p, descriptorWith({"ONLY"})));

    DiagnosticReporter warm;
    ASSERT_EQ(macroNamesAt(p, warm), (std::vector<std::string>{"ONLY"}));
    ASSERT_EQ(warm.errorCount(), 0u);

    ShippedDescriptorCacheStats const s0 = shippedDescriptorCacheStats();
    for (int i = 0; i < 3; ++i) {
        DiagnosticReporter rep;
        ASSERT_EQ(macroNamesAt(p, rep), (std::vector<std::string>{"ONLY"}));
        ASSERT_EQ(rep.errorCount(), 0u);
    }
    ShippedDescriptorCacheStats const s1 = shippedDescriptorCacheStats();

    EXPECT_EQ(s1.lookups - s0.lookups, 3u);
    EXPECT_EQ(s1.revalidatedHits - s0.revalidatedHits, 3u)
        << "an unchanged descriptor was not served from the cache";
    EXPECT_EQ(s1.parses - s0.parses, 0u)
        << "the parse cache is re-parsing an unchanged document — the c112 "
           "within-TU 4x->1x dedup is gone";
    EXPECT_EQ(s1.staleEvictions - s0.staleEvictions, 0u);
}

// ── 4. A FAILED READ IS NEVER ANSWERED FROM THE CACHE ───────────────────────
//
// The worst form of the same defect: a confident answer about a file that could
// not be opened. The entry is dropped and the failure reported — and the drop is
// proven by what happens NEXT, when the path comes back with different content.
TEST(ShippedDescriptorParseCache, AFailedReadEvictsAndIsReported) {
    ScratchDir dir{Location::Temp, "shipped-descriptor-cache"};
    fs::path const p = dir.path() / "probe.json";
    ASSERT_NO_FATAL_FAILURE(writeFile(p, descriptorWith({"FIRST"})));

    DiagnosticReporter warm;
    ASSERT_EQ(macroNamesAt(p, warm), (std::vector<std::string>{"FIRST"}));
    ASSERT_EQ(warm.errorCount(), 0u);

    std::error_code ec;
    ASSERT_TRUE(fs::remove(p, ec)) << "could not remove the descriptor: " << ec.message();

    DiagnosticReporter gone;
    EXPECT_FALSE(readShippedLibMacros(p, gone).has_value())
        << "a deleted descriptor was answered from the parse cache";
    EXPECT_EQ(countCode(gone, DiagnosticCode::F_ShippedLibDescriptorMalformed), 1u);

    // Same path, different content: if the failure had left the old entry in
    // place this would still say FIRST.
    ASSERT_NO_FATAL_FAILURE(writeFile(p, descriptorWith({"SECOND"})));
    DiagnosticReporter back;
    EXPECT_EQ(macroNamesAt(p, back), (std::vector<std::string>{"SECOND"}));
    EXPECT_EQ(back.errorCount(), 0u);
}
