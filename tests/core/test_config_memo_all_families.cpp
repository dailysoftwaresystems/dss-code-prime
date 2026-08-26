// ★★★ THE MEMO ACROSS ALL THREE SCHEMA FAMILIES, AND THE `--time` ROWS THAT
// MAKE ITS COST VISIBLE
// (D-CONFIG-A-SCHEMA-DOCUMENT-IS-REBUILT-ONCE-PER-LOAD-INSIDE-ONE-PROCESS).
//
// `tests/core/test_config_document_memo.cpp` is the standing guard for the memo
// MECHANISM, and it drives it through the GRAMMAR family only. This file is the
// guard for the two families wired in afterwards — `TargetSchema` and
// `ObjectFormatSchema` — and for the three `--time` phases that report what a
// config load costs. They are one file because they are one claim: the phases
// are how a reader learns the memo is working, and a phase that agreed with a
// broken memo would be worse than no phase at all.
//
// ★★ THE TWO FAILURE DIRECTIONS, TESTED SEPARATELY, AS THEY MUST BE.
//   * UNDER-EAGER — the memo never hits, so it is a no-op wearing a green suit.
//     Pinned by the same-instance arms AND by `build-config`'s run count, which
//     must STOP GROWING once a document has been built once.
//   * OVER-EAGER — the memo serves a schema built from SUPERSEDED bytes. For a
//     `.target.json` that is the register file, the calling convention and the
//     instruction encodings; for a `.format.json` it is the on-disk layout of
//     every artifact. Neither would turn anything red downstream: the compiler
//     would emit a plausible binary against a description that is not the one on
//     disk. Pinned by the rewrite arms, which are the reason this file exists.
//
// ⚠ NOTHING HERE TOUCHES THE SHIPPED TREE. Every document under test is a COPY
// in a per-test scratch directory, loaded through `loadFromFile` by explicit
// path, and the rewrite is an ATOMIC REPLACE (write a sibling temp, then
// `std::filesystem::rename` over it) rather than a truncate-then-write — a
// truncate hammer was measured tearing a shipped document 156/156 times, and
// `src/link/writer.cpp` documents why plain `fs::rename` is the correct
// replace-in-one-step on every host this project builds for.
//
// ⓘ The one arm that DOES read the shipped tree is the phase arm, because a
// phase count is only interesting for the route production actually takes
// (`loadShipped` -> `findShippedConfig` -> `loadFromFile`). It only READS.

#include "core/substrate/phase_timers.hpp"
#include "core/types/config_document_memo.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

using dss::ObjectFormatSchema;
using dss::TargetSchema;
using dss::detail::ConfigDocumentMemo;
using dss::detail::ConfigDocumentMemoStore;
using dss::substrate::CompilePhase;
using dss::substrate::PhaseTimers;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

[[nodiscard]] std::string readWhole(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

// ★ ATOMIC REPLACE, not truncate-then-write. The subject of the rewrite arms is
// "the second load must see the SECOND document", and a torn intermediate state
// would let a reader observe a THIRD thing that was never either document —
// which would make a failure here unattributable. Writing a sibling temp and
// renaming over the target is the same one-step replace `src/link/writer.cpp`
// performs for artifacts, for the same reason.
void atomicReplace(fs::path const& p, std::string_view bytes) {
    fs::path const tmp = fs::path{p}.concat(".rewrite-tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(out.good()) << "atomicReplace: writing " << tmp.string();
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    ASSERT_FALSE(ec) << "atomicReplace: rename " << tmp.string() << " -> "
                     << p.string() << ": " << ec.message();
}

// Copy one shipped document into `dir` under its own filename and hand back the
// copy's path. Reads the shipped tree; never writes to it.
[[nodiscard]] fs::path copyShipped(fs::path const& dir, std::string_view subdir,
                                   std::string_view leaf) {
    fs::path const source = dss::test::configRoot() / subdir / leaf;
    std::string const bytes = readWhole(source);
    EXPECT_FALSE(bytes.empty())
        << "shipped document is empty or unreadable: " << source.string();
    fs::path const dest = dir / leaf;
    {
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    return dest;
}

// Replace the FIRST occurrence of `from` with `to`. Used to change a document's
// declared `version`, which is both a byte change (so the digest moves) and an
// OBSERVABLE one (so a stale hit is caught by what the schema SAYS, not merely
// by which pointer came back). A test that varied only invisible bytes could not
// tell a working memo from one returning the wrong schema.
[[nodiscard]] std::string substituteOnce(std::string text, std::string_view from,
                                         std::string_view to) {
    auto const at = text.find(from);
    EXPECT_NE(at, std::string::npos)
        << "the document no longer contains '" << from
        << "' — this fixture's observable knob has moved and the arm below "
           "would pass vacuously";
    if (at == std::string::npos) return text;
    text.replace(at, from.size(), to);
    return text;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. THE TARGET FAMILY IS MEMOIZED — the second load builds nothing.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigMemoAllFamilies, TargetLoadedTwiceIsBuiltOnce) {
    ScratchDir scratch{Location::Temp, "config_memo_target"};
    fs::path const doc =
        copyShipped(scratch.path(), "targets", "x86_64.target.json");

    ConfigDocumentMemo<TargetSchema>::clear();
    ConfigDocumentMemoStore::resetStats();
    auto const before = ConfigDocumentMemoStore::stats();

    auto first = TargetSchema::loadFromFile(doc);
    ASSERT_TRUE(first.has_value()) << "the shipped x86_64 target must load";
    auto second = TargetSchema::loadFromFile(doc);
    ASSERT_TRUE(second.has_value());

    // ★ THE POINTER IS THE CLAIM. Equal CONTENT could be produced by two builds;
    // only one instance proves the second build did not happen.
    EXPECT_EQ(first->get(), second->get())
        << "two loads of one unchanged document returned two instances — the "
           "memo did not hit, so it is a no-op";

    auto const after = ConfigDocumentMemoStore::stats();
    EXPECT_EQ(after.misses - before.misses, 1u)
        << "exactly one of the two loads may reach the builder";
    EXPECT_EQ(after.hits - before.hits, 1u)
        << "the second load must be served from the memo";

    // The schema is a real one, not an empty shell that happens to be shared.
    EXPECT_EQ((*first)->name(), "x86_64");
    EXPECT_FALSE((*first)->contentDigest().empty())
        << "a file-route load must retain the digest of the bytes it read";
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. ★★★ THE STALENESS PIN, TARGET FAMILY. The path does not change; the BYTES
//    do. A memo keyed on anything but the bytes serves the pre-rewrite schema.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigMemoAllFamilies, ARewrittenTargetDocumentIsNeverServedStale) {
    ScratchDir scratch{Location::Temp, "config_memo_target_rewrite"};
    fs::path const doc =
        copyShipped(scratch.path(), "targets", "x86_64.target.json");

    ConfigDocumentMemo<TargetSchema>::clear();

    auto first = TargetSchema::loadFromFile(doc);
    ASSERT_TRUE(first.has_value());
    std::string const originalVersion{(*first)->version()};
    ASSERT_FALSE(originalVersion.empty())
        << "the fixture needs a declared target version to observe";

    // Same path, different bytes, one observable field moved.
    std::string const rewritten = substituteOnce(
        readWhole(doc), "\"version\": \"" + originalVersion + "\"",
        "\"version\": \"9.9.9-memo-probe\"");
    atomicReplace(doc, rewritten);

    auto second = TargetSchema::loadFromFile(doc);
    ASSERT_TRUE(second.has_value())
        << "the rewritten document must still be a valid target";

    EXPECT_NE(first->get(), second->get())
        << "the memo served the SAME instance for two DIFFERENT documents";
    EXPECT_EQ((*second)->version(), "9.9.9-memo-probe")
        << "the second load reported the PRE-REWRITE version — a stale hit, "
           "which for a target schema is a silently wrong register file, "
           "calling convention and instruction encoding set";
    EXPECT_EQ((*first)->version(), originalVersion)
        << "the first schema was mutated after the fact — a memo entry must be "
           "immutable once published";
    EXPECT_NE((*first)->contentDigest(), (*second)->contentDigest());
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. THE OBJECT-FORMAT FAMILY IS MEMOIZED — the most-read shipped class.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigMemoAllFamilies, ObjectFormatLoadedTwiceIsBuiltOnce) {
    ScratchDir scratch{Location::Temp, "config_memo_format"};
    fs::path const doc =
        copyShipped(scratch.path(), "object-formats", "spirv-1.6.format.json");

    ConfigDocumentMemo<ObjectFormatSchema>::clear();
    ConfigDocumentMemoStore::resetStats();
    auto const before = ConfigDocumentMemoStore::stats();

    auto first = ObjectFormatSchema::loadFromFile(doc);
    ASSERT_TRUE(first.has_value()) << "the shipped spirv-1.6 format must load";
    auto second = ObjectFormatSchema::loadFromFile(doc);
    ASSERT_TRUE(second.has_value());

    EXPECT_EQ(first->get(), second->get())
        << "two loads of one unchanged format document returned two instances "
           "— the memo did not hit, so it is a no-op";

    auto const after = ConfigDocumentMemoStore::stats();
    EXPECT_EQ(after.misses - before.misses, 1u);
    EXPECT_EQ(after.hits - before.hits, 1u);
    EXPECT_EQ((*first)->name(), "spirv-1.6");
    EXPECT_FALSE((*first)->contentDigest().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. ★★★ THE STALENESS PIN, OBJECT-FORMAT FAMILY.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigMemoAllFamilies, ARewrittenFormatDocumentIsNeverServedStale) {
    ScratchDir scratch{Location::Temp, "config_memo_format_rewrite"};
    fs::path const doc =
        copyShipped(scratch.path(), "object-formats", "spirv-1.6.format.json");

    ConfigDocumentMemo<ObjectFormatSchema>::clear();

    auto first = ObjectFormatSchema::loadFromFile(doc);
    ASSERT_TRUE(first.has_value());
    std::string const originalVersion{(*first)->version()};
    ASSERT_FALSE(originalVersion.empty())
        << "the fixture needs a declared format version to observe";

    std::string const rewritten = substituteOnce(
        readWhole(doc), "\"version\": \"" + originalVersion + "\"",
        "\"version\": \"9.9.9-memo-probe\"");
    atomicReplace(doc, rewritten);

    auto second = ObjectFormatSchema::loadFromFile(doc);
    ASSERT_TRUE(second.has_value());

    EXPECT_NE(first->get(), second->get())
        << "the memo served the SAME instance for two DIFFERENT documents";
    EXPECT_EQ((*second)->version(), "9.9.9-memo-probe")
        << "the second load reported the PRE-REWRITE version — a stale hit, "
           "which for a format schema is a silently wrong on-disk artifact "
           "layout";
    EXPECT_NE((*first)->contentDigest(), (*second)->contentDigest());
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. THE FAMILY KEY IS LOAD-BEARING, NOT DECORATION. Three schema types now
//    share ONE store, and the typed facade `static_pointer_cast`s whatever the
//    store returns — so a store that ignored `family` would hand a
//    `TargetSchema` back to a caller that will read it as an
//    `ObjectFormatSchema`. That is undefined behaviour, not a wrong answer, and
//    it is the one failure in this file that no downstream assertion could
//    survive to report.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigMemoAllFamilies, OneLabelAndDigestDoNotCrossFamilies) {
    ScratchDir scratch{Location::Temp, "config_memo_family_key"};
    fs::path const doc =
        copyShipped(scratch.path(), "targets", "x86_64.target.json");

    ConfigDocumentMemo<TargetSchema>::clear();
    auto loaded = TargetSchema::loadFromFile(doc);
    ASSERT_TRUE(loaded.has_value());
    std::string const label{doc.string()};
    std::string const digest{(*loaded)->contentDigest()};

    // The exact key that just hit for the target family.
    EXPECT_NE(ConfigDocumentMemo<TargetSchema>::lookup(label, digest), nullptr)
        << "the entry this arm is about is not in the memo, so the negative "
           "below would pass for the wrong reason";

    // The SAME label and the SAME digest, asked for under a different type.
    EXPECT_EQ(ConfigDocumentMemo<ObjectFormatSchema>::lookup(label, digest),
              nullptr)
        << "the store answered across schema families — the typed facade would "
           "then cast a TargetSchema to an ObjectFormatSchema";
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. THE `--time` PHASES ARE REAL, AND THEY AGREE WITH THE MEMO.
//
// ★ THIS IS THE ARM THAT MAKES THE INSTRUMENT WORTH HAVING. `locate-config`,
//   `load-config` and `build-config` must each record a run and a NONZERO
//   duration on a real shipped load — an enum entry with no Scope behind it
//   would print a row of zeroes forever and read as "config costs nothing".
// ★★ And the joint claim: a SECOND identical load advances `load-config` (it
//   still walks, reads and digests) but must NOT advance `build-config`, whose
//   run count IS the miss count. One assertion catches both a missing Scope and
//   a memo that stopped hitting.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigMemoAllFamilies, ConfigLoadPhasesAccumulateAndTrackTheMemo) {
    ConfigDocumentMemo<TargetSchema>::clear();
    ASSERT_EQ(PhaseTimers::liveScopeCount(), 0u)
        << "a live Scope would make the reset below close an interval it never "
           "opened";
    PhaseTimers::reset();

    auto first = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(first.has_value())
        << "x86_64 must be a shipped target — this arm needs the production "
           "route (loadShipped -> findShippedConfig -> loadFromFile)";

    auto const locate1 = PhaseTimers::read(CompilePhase::LocateConfig);
    auto const load1   = PhaseTimers::read(CompilePhase::LoadConfig);
    auto const build1  = PhaseTimers::read(CompilePhase::BuildConfig);

    EXPECT_GE(locate1.runs, 1u) << "the precedence walk recorded no run";
    EXPECT_GT(locate1.cpuNanoseconds, 0u)
        << "locate-config would print a permanent zero";
    EXPECT_GE(load1.runs, 1u) << "the document load recorded no run";
    EXPECT_GT(load1.cpuNanoseconds, 0u)
        << "load-config would print a permanent zero";
    EXPECT_GE(build1.runs, 1u) << "the cold build recorded no run";
    EXPECT_GT(build1.cpuNanoseconds, 0u)
        << "build-config would print a permanent zero";

    auto second = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->get(), second->get())
        << "the shipped route did not hit the memo";

    auto const locate2 = PhaseTimers::read(CompilePhase::LocateConfig);
    auto const load2   = PhaseTimers::read(CompilePhase::LoadConfig);
    auto const build2  = PhaseTimers::read(CompilePhase::BuildConfig);

    EXPECT_EQ(locate2.runs, locate1.runs + 1u)
        << "the walk is deliberately NOT memoized — its answer depends on cwd "
           "and on the environment, both of which one process may change";
    EXPECT_EQ(load2.runs, load1.runs + 1u)
        << "every load is paid for, hit or miss; that is what makes this row "
           "the always-paid half";
    EXPECT_EQ(build2.runs, build1.runs)
        << "the second load BUILT the document again — build-config's run "
           "count is the miss count, and the memo did not hit";

    // ★★ EXCLUSIVE ATTRIBUTION, on the one shape where it is checkable by
    // arithmetic: `build-config` nests inside `load-config`, so the outer row
    // must NOT contain the inner one's time. A Scope opened in the wrong place
    // (or the same phase nested inside itself) shows up here as a load row that
    // swallowed the build.
    EXPECT_LE(load2.wallNanoseconds, load2.cpuNanoseconds);
    EXPECT_EQ(load2.peakConcurrency, 1u)
        << "these loads are strictly serial — a peak above 1 means a scope "
           "nested inside another of its OWN phase";
    EXPECT_EQ(build2.peakConcurrency, 1u);
}
