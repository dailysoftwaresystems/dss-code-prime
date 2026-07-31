// D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING — the writer must give every
// emitted artifact a NEW FILE IDENTITY, never truncate the old one in place.
//
// WHY BYTES ARE NOT ENOUGH. The defective code produced perfectly correct
// bytes; the fault was that it kept REUSING the destination's inode. macOS 26
// attaches an exec DENY to a soured inode, so a repeatedly-rebuilt artifact
// dies with exit 137 SIGKILL and no output at that inode while a byte-
// identical copy at another path runs fine (MEASURED, TF-C103). A test that
// only checks the bytes is GREEN on the defect. So the pinned property here is
// file IDENTITY.
//
// HOW IT IS PINNED, PORTABLY, WITH NO `#ifdef`. C++ exposes no inode number,
// but `std::filesystem` exposes the two primitives that together express the
// same fact:
//   * `create_hard_link(out, witness)` gives the CURRENT file object a second
//     name. The witness is not a copy — it IS that object.
//   * `equivalent(a, b)` answers "do these two paths name the same file
//     object?" (POSIX: same device+inode; Windows: same volume serial + file
//     index).
// Hard-link a witness aside, write again, and ask whether `out` is still the
// witness's file object:
//   * truncate-in-place  -> `out` and the witness stay the SAME object, and
//                           the witness shows the NEW bytes  -> RED.
//   * temp-then-rename   -> `out` is a DIFFERENT object, and the witness
//                           keeps its own generation's bytes -> GREEN.
// Both `create_hard_link` and `equivalent` are supported on NTFS and need no
// elevation, so the windows-msvc-release leg really executes this rather than
// silently no-opping. The tests deliberately assert the POSITIVE direction too
// (`equivalent` returning TRUE for a just-made link) so a filesystem or
// implementation on which the witness mechanism is degenerate fails LOUDLY
// instead of passing vacuously.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/linker.hpp"
#include "link/writer.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// A well-formed LinkedImage (same shape as the sibling test file's helper):
// clean link, parallel-index consistent, non-empty bytes.
[[nodiscard]] LinkedImage makeImage(std::vector<std::uint8_t> bytes) {
    LinkedImage img;
    img.format            = ObjectFormatKind::Elf;
    img.bytes             = std::move(bytes);
    img.expectedFuncCount = 1;
    img.resolvedFuncCount = 1;
    img.linkedCleanly     = true;
    return img;
}

[[nodiscard]] std::vector<std::uint8_t> readAll(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// "Do these two paths name the SAME file object?" — the identity primitive.
// Uses the `error_code` overload so a filesystem that cannot answer surfaces
// as an explicit failure rather than an exception escaping the test body.
[[nodiscard]] bool sameFileObject(fs::path const& a, fs::path const& b) {
    std::error_code ec;
    bool const      same = fs::equivalent(a, b, ec);
    EXPECT_FALSE(static_cast<bool>(ec))
        << "fs::equivalent('" << a.string() << "', '" << b.string()
        << "') failed: " << ec.message()
        << " — the identity probe itself is broken on this filesystem; the "
           "result below proves nothing.";
    return same;
}

// Names of any staging temps left behind in `dir`. The writer stages every
// artifact in a sibling `<target>.dsstmp-<pid>-<n>` and must remove it on
// EVERY path — renamed away on success, deleted on each failure.
[[nodiscard]] std::vector<std::string> stagingResidue(fs::path const& dir) {
    std::vector<std::string> found;
    std::error_code          ec;
    for (auto const& entry : fs::directory_iterator(dir, ec)) {
        auto const name = entry.path().filename().string();
        if (name.find(".dsstmp-") != std::string::npos) {
            found.push_back(name);
        }
    }
    EXPECT_FALSE(static_cast<bool>(ec))
        << "directory_iterator('" << dir.string() << "') failed: "
        << ec.message();
    return found;
}

[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

} // namespace

// ── THE PIN: successive writes to one path yield distinct file objects ──────

TEST(LinkWriterNewIdentity, SuccessiveWritesProduceDistinctFileIdentities) {
    ScratchDir scratch{Location::Temp, "link-writer-identity"};
    auto const out = scratch.path() / "artifact.bin";

    // Three generations — the defect is a REBUILD defect, so one overwrite is
    // not a sufficient sample. After each write, hard-link the just-written
    // file object aside under its own name.
    constexpr int                          kGenerations = 3;
    std::vector<fs::path>                  witness;
    std::vector<std::vector<std::uint8_t>> payload;

    for (int gen = 0; gen < kGenerations; ++gen) {
        // Distinct length AND content per generation, so a stale witness is
        // unmistakable.
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(8 + gen),
            static_cast<std::uint8_t>(0xA0 + gen));

        DiagnosticReporter rep;
        ASSERT_TRUE(linker::writeImage(makeImage(bytes), out, rep))
            << "generation " << gen << " failed to write";
        ASSERT_EQ(rep.errorCount(), 0u) << "generation " << gen;

        auto const w =
            scratch.path() / ("witness-" + std::to_string(gen) + ".bin");
        std::error_code ec;
        fs::create_hard_link(out, w, ec);
        ASSERT_FALSE(static_cast<bool>(ec))
            << "create_hard_link failed (" << ec.message()
            << ") — this test needs hard links to observe file identity. A "
               "filesystem without them cannot run this pin; do not weaken "
               "the assertion, fix the scratch location.";
        // Anti-vacuity: right now the witness MUST be the same object as
        // `out`. If this ever failed, every `EXPECT_FALSE` below would be
        // trivially true and the test would prove nothing.
        ASSERT_TRUE(sameFileObject(out, w))
            << "a freshly created hard link is not equivalent to its target — "
               "the identity probe is broken, not the writer";

        witness.push_back(w);
        payload.push_back(std::move(bytes));
    }

    // ── Identity: every generation that was SUPERSEDED must be a different
    // file object than the artifact now at `out`. This is the assertion that
    // goes RED against `std::ios::trunc`.
    for (int gen = 0; gen + 1 < kGenerations; ++gen) {
        EXPECT_FALSE(sameFileObject(out, witness[static_cast<std::size_t>(gen)]))
            << "after " << (kGenerations - gen - 1)
            << " later write(s), '" << out.string()
            << "' is STILL the same file object as generation " << gen
            << ". The writer truncated in place and reused the inode — that is "
               "the D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING defect.";
    }
    // ...and the LAST generation is still current, which proves `equivalent`
    // is capable of returning true here (the negative results above are real).
    EXPECT_TRUE(sameFileObject(out, witness.back()))
        << "the most recent write should still be the object at `out`";

    // Pairwise distinct: three writes produced three distinct file objects,
    // not two, and not one recycled one.
    for (std::size_t i = 0; i < witness.size(); ++i) {
        for (std::size_t j = i + 1; j < witness.size(); ++j) {
            EXPECT_FALSE(sameFileObject(witness[i], witness[j]))
                << "generations " << i << " and " << j
                << " are the same file object — the writer recycled an "
                   "identity across rebuilds";
        }
    }

    // ── Bytes: each superseded identity kept ITS OWN generation's content.
    // Under truncate-in-place every witness would read back the FINAL bytes.
    for (std::size_t gen = 0; gen < witness.size(); ++gen) {
        EXPECT_EQ(readAll(witness[gen]), payload[gen])
            << "witness for generation " << gen
            << " does not hold that generation's bytes — the later write "
               "reached through into the older file object";
    }
    // ...and the artifact itself holds the newest bytes (the property the old
    // code also satisfied; kept so a regression here is still caught).
    EXPECT_EQ(readAll(out), payload.back());

    // No staging temp survives a successful write.
    EXPECT_TRUE(stagingResidue(scratch.path()).empty())
        << "a `.dsstmp-*` staging file was left next to the artifact";
}

// The chokepoint is `writeBytes`, not `writeImage`: the `ar` static-archive
// writer calls it DIRECTLY, so pinning only the `writeImage` wrapper would
// leave the raw entry point unguarded.
TEST(LinkWriterNewIdentity, WriteBytesReplacesIdentityToo) {
    ScratchDir scratch{Location::Temp, "link-writer-identity-raw"};
    auto const out = scratch.path() / "raw.bin";

    std::vector<std::uint8_t> const first{0x01, 0x02, 0x03, 0x04};
    std::vector<std::uint8_t> const second{0xF0, 0xF1};

    DiagnosticReporter rep;
    ASSERT_TRUE(linker::writeBytes(first, out, rep));
    ASSERT_EQ(rep.errorCount(), 0u);

    auto const      w = scratch.path() / "raw-witness.bin";
    std::error_code ec;
    fs::create_hard_link(out, w, ec);
    ASSERT_FALSE(static_cast<bool>(ec)) << ec.message();
    ASSERT_TRUE(sameFileObject(out, w));

    ASSERT_TRUE(linker::writeBytes(second, out, rep));
    ASSERT_EQ(rep.errorCount(), 0u);

    EXPECT_FALSE(sameFileObject(out, w))
        << "writeBytes reused the destination's file identity";
    EXPECT_EQ(readAll(w), first);
    EXPECT_EQ(readAll(out), second);
    EXPECT_TRUE(stagingResidue(scratch.path()).empty());
}

// ── Failure paths: every one still fails LOUD and leaves no temp ────────────

// Reaches the RENAME and fails there — the only branch that gets all the way
// past a successful staged write, so it is the one that proves the commit
// step cannot fail silently and that cleanup runs. Deterministic on both
// hosts: POSIX `rename` onto a directory is an error, and MoveFileEx
// documents "If lpNewFileName names an existing directory, an error is
// reported."
TEST(LinkWriterNewIdentity, RenameOntoExistingDirectoryFailsLoudAndRemovesTemp) {
    ScratchDir scratch{Location::Temp, "link-writer-identity-dir"};
    auto const target = scratch.path() / "occupied-directory";
    ASSERT_TRUE(fs::create_directory(target));
    {
        std::ofstream occupant(target / "occupant.txt");
        occupant << "still here\n";
    }

    DiagnosticReporter rep;
    EXPECT_FALSE(
        linker::writeImage(makeImage({0xDE, 0xAD, 0xBE, 0xEF}), target, rep))
        << "writing onto an existing directory must not report success";
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_ImageWriteOpenFailed))
        << "a failed commit must emit a real diagnostic, never fail silently";

    // The pre-existing thing at the destination is untouched — a failed
    // rename must not damage what was already there.
    EXPECT_TRUE(fs::is_directory(target));
    EXPECT_TRUE(fs::exists(target / "occupant.txt"));

    // ...and the staged temp was cleaned up rather than leaked next to it.
    EXPECT_TRUE(stagingResidue(scratch.path()).empty())
        << "the staging temp survived a failed rename";
}

// Reaches only the TEMP CREATE and fails there: an intermediate path
// component is a regular file, so no `.dsstmp-*` can be created beneath it.
TEST(LinkWriterNewIdentity, TempCreateFailureIsLoudAndLeavesNothing) {
    ScratchDir scratch{Location::Temp, "link-writer-identity-notdir"};
    auto const fileAsParent = scratch.path() / "regular.txt";
    {
        std::ofstream f(fileAsParent);
        f << "i am a file, not a directory\n";
    }
    auto const untouchedSize = fs::file_size(fileAsParent);

    DiagnosticReporter rep;
    EXPECT_FALSE(
        linker::writeImage(makeImage({0x42}), fileAsParent / "child.bin", rep));
    // Either belt is a valid fail-loud outcome (platforms disagree on whether
    // `exists()` on a non-directory parent reports false or an error); pin
    // that ONE of them fires rather than a silent success.
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_ImageWriteOpenFailed)
                || sawCode(rep, DiagnosticCode::K_ImageWriteParentMissing));
    EXPECT_TRUE(stagingResidue(scratch.path()).empty());
    EXPECT_EQ(fs::file_size(fileAsParent), untouchedSize)
        << "the regular file standing in for a directory was modified";
}

// Destination directory does not exist — the pre-rename precondition, kept
// pinned across the rewrite.
TEST(LinkWriterNewIdentity, MissingDestinationDirectoryIsLoudAndLeavesNothing) {
    ScratchDir scratch{Location::Temp, "link-writer-identity-nodir"};
    auto const ghost = scratch.path() / "does-not-exist" / "ghost.bin";

    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(makeImage({0x42}), ghost, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_ImageWriteParentMissing));
    EXPECT_FALSE(fs::exists(ghost));
    EXPECT_TRUE(stagingResidue(scratch.path()).empty());
}

// A path with no filename component can never be renamed onto. Guarded BEFORE
// anything is created, so it must not drop a stray `.dsstmp-*` into the
// process's current working directory.
TEST(LinkWriterNewIdentity, NoFilenameComponentIsLoudAndStagesNothing) {
    auto const cwd = fs::current_path();

    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(makeImage({0x01}), fs::path{""}, rep));
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_ImageWriteOpenFailed));
    EXPECT_TRUE(stagingResidue(cwd).empty())
        << "a filename-less path leaked a staging temp into the cwd";
}

// ── Uniqueness: concurrent writers share one directory ─────────────────────

// The staging temps of every concurrent writer land in the SAME directory, so
// the naming scheme is load-bearing. `tests/test_support/scratch_dir.hpp`
// records why a pid+counter pair alone is not a guarantee (pids recycle;
// killed runs leave stale slots); the writer therefore claims its temp with an
// exclusive create and steps over any slot it did not make. Pin the observable
// consequence: N writers, one directory, every artifact correct, no residue.
TEST(LinkWriterNewIdentity, ConcurrentWritersInOneDirectoryDoNotCollide) {
    ScratchDir scratch{Location::Temp, "link-writer-identity-concurrent"};
    constexpr int kWriters = 8;

    std::vector<char>        ok(kWriters, 0);   // not vector<bool> — bit-packed
    std::vector<std::thread> threads;
    threads.reserve(kWriters);

    for (int i = 0; i < kWriters; ++i) {
        threads.emplace_back([&, i] {
            // Each thread gets its OWN reporter — DiagnosticReporter carries
            // instance state and is not a shared sink.
            DiagnosticReporter        rep;
            std::vector<std::uint8_t> bytes(
                64u + static_cast<std::size_t>(i),
                static_cast<std::uint8_t>(i));
            auto const out =
                scratch.path() / ("artifact-" + std::to_string(i) + ".bin");
            bool const wrote = linker::writeImage(makeImage(bytes), out, rep);
            ok[static_cast<std::size_t>(i)] =
                static_cast<char>(wrote && rep.errorCount() == 0u);
        });
    }
    for (auto& t : threads) t.join();

    for (int i = 0; i < kWriters; ++i) {
        EXPECT_TRUE(ok[static_cast<std::size_t>(i)] != 0)
            << "concurrent writer " << i << " failed";
        auto const out =
            scratch.path() / ("artifact-" + std::to_string(i) + ".bin");
        std::vector<std::uint8_t> const expected(
            64u + static_cast<std::size_t>(i),
            static_cast<std::uint8_t>(i));
        EXPECT_EQ(readAll(out), expected)
            << "writer " << i
            << " landed the wrong bytes — two writers shared a staging temp";
    }
    EXPECT_TRUE(stagingResidue(scratch.path()).empty())
        << "concurrent writes left staging temps behind";
}
