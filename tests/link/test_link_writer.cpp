// linker::writeImage substrate — plan 14 LK10 cycle 1 tests.
//
// Pins:
//   * A well-formed LinkedImage round-trips bytes-for-bytes to disk.
//   * Format-blindness: a PE-tagged image writes the same way as
//     an ELF-tagged one (writer never reads image.format).
//   * 5 precondition / failure guards fire distinct K_* codes:
//     - K_ImageNotOk: `ok() == false` OR empty bytes.
//     - K_ImageWriteParentMissing: parent dir doesn't exist.
//     - K_ImageWriteOpenFailed: empty path, path is a directory,
//       permission denied, etc.
//   * End-to-end gate: AssembledModule → link to ELF → writeImage
//     → file on disk has the same bytes. Proves the LK1 + LK4 +
//     LK10 substrate chain works end-to-end without a system
//     linker.
//   * File-extension policy lives in the caller; the substrate
//     accepts any path the caller gives it.

#include "asm/asm.hpp"
#include "core/substrate/path_identity.hpp"   // the cell below reads back through `\\?\` (H6c)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link/writer.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// Hand-rolled LinkedImage that bypasses the full pipeline — used
// to pin writeImage's preconditions and round-trip behavior
// independently of the linker engine.
[[nodiscard]] LinkedImage makeImage(std::vector<std::uint8_t> bytes,
                                     std::size_t funcCount = 1) {
    LinkedImage img;
    img.format = ObjectFormatKind::Elf;
    img.bytes = std::move(bytes);
    img.expectedFuncCount = funcCount;
    img.resolvedFuncCount = funcCount;
    // Simulate a CLEAN link: `linker::link()` sets this at its successful
    // conclusion, and `ok()` (hence writeImage's precondition) now requires it.
    // A hand-rolled "well-formed image" must set it too (D-CSUBSET-TESTTU-SILENT-EXIT1).
    img.linkedCleanly = true;
    return img;
}

} // namespace

// ── Round-trip: bytes on disk == bytes in image ─────────────────────

TEST(LinkWriter, WritesBytesVerbatim) {
    ScratchDir scratch{Location::Temp, "link-writer"};
    auto const out = scratch.path() / "round-trip.bin";

    auto const image = makeImage({0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE});

    DiagnosticReporter rep;
    ASSERT_TRUE(linker::writeImage(image, out, rep));
    EXPECT_EQ(rep.errorCount(), 0u);

    std::ifstream in(out, std::ios::binary);
    std::vector<std::uint8_t> read{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
    EXPECT_EQ(read, image.bytes);
}

// ── D-OUTPUT-EXEC-BIT: executable-flavor output carries the +x bit ──────
//
// The `executable` flag (the caller derives it from
// `ObjectFormatSchema::isImageFlavor()`) controls whether the written file
// gets the POSIX execute bits. POSIX-gated because the bit is a Unix concept
// (Windows/PE ignores Unix modes); on the POSIX CI legs this is the
// red-on-disable witness — remove the `permissions(… add)` call in
// writer.cpp and the `executable=true` arm goes RED (file stays 0644).
#ifndef _WIN32
TEST(LinkWriter, ExecutableOutputCarriesExecuteBitPosix) {
    ScratchDir scratch{Location::Temp, "link-writer-exec"};
    auto const image = makeImage({0x7f, 'E', 'L', 'F', 0x02, 0x01, 0x01, 0x00});

    // executable=true → owner/group/others execute bits all present.
    {
        auto const out = scratch.path() / "runnable";
        DiagnosticReporter rep;
        ASSERT_TRUE(linker::writeImage(image, out, rep, /*executable=*/true));
        EXPECT_EQ(rep.errorCount(), 0u);
        auto const p = fs::status(out).permissions();
        EXPECT_NE(p & fs::perms::owner_exec, fs::perms::none)
            << "an executable output must carry owner-exec so `./out` runs "
               "without a manual chmod (D-OUTPUT-EXEC-BIT)";
        EXPECT_NE(p & fs::perms::group_exec, fs::perms::none);
        EXPECT_NE(p & fs::perms::others_exec, fs::perms::none);
    }

    // executable=false (the default) → NO execute bit: a plain artifact is
    // left at the host umask. Pins the CONDITIONALITY — unconditionally
    // setting +x would make this arm RED.
    {
        auto const out = scratch.path() / "plain";
        DiagnosticReporter rep;
        ASSERT_TRUE(linker::writeImage(image, out, rep, /*executable=*/false));
        auto const p = fs::status(out).permissions();
        EXPECT_EQ(p & fs::perms::owner_exec, fs::perms::none)
            << "a non-executable output must NOT be marked executable";
    }
}
#endif

// Host-independent guard: the `executable=true` path is reachable and clean
// on EVERY platform (on Windows it is a no-op, but must not error/fail the
// write). Runs on the local MSVC gate, where the POSIX bit-assertion above
// is skipped.
TEST(LinkWriter, ExecutableWriteSucceedsEveryPlatform) {
    ScratchDir scratch{Location::Temp, "link-writer-exec-smoke"};
    auto const out = scratch.path() / "smoke";
    auto const image = makeImage({0x01, 0x02, 0x03, 0x04});
    DiagnosticReporter rep;
    EXPECT_TRUE(linker::writeImage(image, out, rep, /*executable=*/true));
    EXPECT_EQ(rep.errorCount(), 0u)
        << "setting the exec bit must not fail a write on any host";
}

TEST(LinkWriter, WritesAnyFormatBytesVerbatim) {
    // pr-test-analyzer Gap 5 fold: writeImage is format-blind
    // by construction (just dumps `image.bytes`). Pin this with
    // a PE-tagged LinkedImage to forestall any future drift that
    // sneaks a per-format branch into writer.cpp.
    ScratchDir scratch{Location::Temp, "link-writer"};
    auto const out = scratch.path() / "pe.bin";
    LinkedImage image;
    image.format = ObjectFormatKind::Pe;
    image.bytes  = {'M', 'Z', 0x00, 0x00};  // PE DOS header start
    image.expectedFuncCount = 1;
    image.resolvedFuncCount = 1;
    image.linkedCleanly = true;   // simulate a clean link (see makeImage)

    DiagnosticReporter rep;
    ASSERT_TRUE(linker::writeImage(image, out, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(fs::file_size(out), 4u);

    std::ifstream in(out, std::ios::binary);
    char hdr[4] = {0};
    in.read(hdr, 4);
    EXPECT_EQ(hdr[0], 'M');
    EXPECT_EQ(hdr[1], 'Z');
}

TEST(LinkWriter, OverwritesExistingFile) {
    // truncate mode contract: writeImage replaces the file's
    // bytes, never appends. Prior content of arbitrary length
    // must not bleed into the new write.
    ScratchDir scratch{Location::Temp, "link-writer"};
    auto const out = scratch.path() / "overwritten.bin";
    {
        std::ofstream stale(out, std::ios::binary);
        std::vector<std::uint8_t> garbage(4096, 0xAA);
        stale.write(reinterpret_cast<char const*>(garbage.data()),
                    static_cast<std::streamsize>(garbage.size()));
    }
    auto const image = makeImage({0x01, 0x02, 0x03});
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::writeImage(image, out, rep));
    EXPECT_EQ(fs::file_size(out), 3u);
}

// ── Precondition guards (fail-loud K_ImageNotOk) ─────────────

TEST(LinkWriter, RejectsImageWithOkFalse) {
    ScratchDir scratch{Location::Temp, "link-writer"};
    LinkedImage image;
    image.format = ObjectFormatKind::Elf;
    image.bytes  = {0x7F, 'E', 'L', 'F'};  // valid bytes...
    image.expectedFuncCount = 3;
    image.resolvedFuncCount = 1;  // ...but parallel-index mismatch
    image.linkedCleanly = true;   // clean link, so the MISMATCH is the sole reason ok() is false
    ASSERT_FALSE(image.ok());

    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(image, scratch.path() / "noop.bin", rep));
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_ImageNotOk) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
    EXPECT_FALSE(fs::exists(scratch.path() / "noop.bin"));
}

TEST(LinkWriter, RejectsEmptyBytes) {
    // Walker contract violation (ok() == true but no bytes) maps
    // to K_ImageEmpty (0x800B) — type-design post-fold #2 split
    // from K_ImageNotOk to give walker-bug-vs-caller-bug
    // remediations distinct dispatchable codes.
    ScratchDir scratch{Location::Temp, "link-writer"};
    LinkedImage image;
    image.format = ObjectFormatKind::Elf;
    image.expectedFuncCount = 1;
    image.resolvedFuncCount = 1;
    image.linkedCleanly = true;   // clean link — isolates the empty-bytes guard (K_ImageEmpty)
    ASSERT_TRUE(image.ok());
    ASSERT_TRUE(image.bytes.empty());

    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(image, scratch.path() / "noop.bin", rep));
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_ImageEmpty) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(LinkWriter, RejectsParentComponentIsAFile) {
    // pr-test-analyzer FOLD-NOW: the `exists()` ec-error branch
    // at writer.cpp's parent-dir check is distinct from `!exists`.
    // Reached on most platforms when an intermediate path
    // component is a regular file (e.g. `/tmp/file.txt/child.bin`).
    // `exists()` returns false (not an error) on POSIX, but
    // `parent_path()` of `file.txt/child.bin` IS `file.txt`,
    // which exists as a non-directory — the subsequent open()
    // will fail with K_ImageWriteOpenFailed. The ec arm is
    // platform-specific; on Windows certain reserved names in
    // the parent trigger it. This test pins the K_* code dispatch
    // for the typical "parent is a file" case.
    ScratchDir scratch{Location::Temp, "link-writer"};
    auto const fileAsParent = scratch.path() / "regular.txt";
    {
        std::ofstream f(fileAsParent);
        f << "i am a file, not a directory\n";
    }
    auto const bad = fileAsParent / "child.bin";
    auto const image = makeImage({0x42});
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(image, bad, rep));
    // Either ParentMissing (exists returns false for non-dir
    // parent on some platforms) or OpenFailed (parent exists but
    // open of child fails). Both are valid fail-loud outcomes —
    // pin that one of them fires (NOT silent success).
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_ImageWriteParentMissing
         || d.code == DiagnosticCode::K_ImageWriteOpenFailed) {
            sawCode = true;
        }
    }
    EXPECT_TRUE(sawCode);
}

TEST(LinkWriter, RejectsMissingParentDirectory) {
    ScratchDir scratch{Location::Temp, "link-writer"};
    auto const nonexistent =
        scratch.path() / "does-not-exist" / "ghost.bin";
    auto const image = makeImage({0x42});

    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(image, nonexistent, rep));
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_ImageWriteParentMissing)
            sawCode = true;
    }
    EXPECT_TRUE(sawCode);
    EXPECT_FALSE(fs::exists(nonexistent));
}

TEST(LinkWriter, RejectsEmptyPath) {
    // pr-test-analyzer Gap 3 fold: an unset config field passing
    // "" should fail loud at the open() stage, not silently
    // succeed or crash. Pins libc++/MSVC ofstream behavior.
    auto const image = makeImage({0x01});
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(image, fs::path{""}, rep));
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_ImageWriteOpenFailed)
            sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(LinkWriter, RejectsPathIsADirectory) {
    // pr-test-analyzer Gap 4 fold: writing TO a directory (not a
    // file inside it) is the most-common positive case for the
    // post-open failbit branch. Without this, the open() failure
    // arm has no real coverage.
    ScratchDir scratch{Location::Temp, "link-writer"};
    auto const image = makeImage({0x01});
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(image, scratch.path(), rep));
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_ImageWriteOpenFailed)
            sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// A TARGET THAT NAMES NO LAST NAME IS REFUSED BY NAME, NEVER THROWN (P69 H6c2, H1): its staging sibling is cut from
// its last name (`withSuffix`), and `C:\absent\..` reaches the volume's ROOT on Windows (GetFullPathNameW folds the
// `..` before any name is read), `<dir>/..` ends in `..` on POSIX -- neither has one. Until H6c2 the cut threw,
// uncaught. Nothing is written: the refusal comes before any staging file is claimed.
TEST(LinkWriter, RejectsATargetThatNamesNoLastName) {
    ScratchDir scratch{Location::Temp, "link-writer-no-last-name"};
#if defined(_WIN32)
    fs::path const target = fs::absolute(scratch.path()).root_path() / "dss-h6c2-absent-level" / "..";
#else
    fs::path const target = scratch.path() / "..";
#endif
    auto const         image = makeImage({0x01});
    DiagnosticReporter rep;
    bool               written = true;
    ASSERT_NO_THROW(written = linker::writeImage(image, target, rep));
    EXPECT_FALSE(written);
    bool named = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::K_ImageWriteOpenFailed && d.actual.find("names no last name") != std::string::npos)
            named = true;
    EXPECT_TRUE(named) << "the write of `" << target.string() << "` was not refused for naming no last name";
}

// A TARGET THE FILE SYSTEM REFUSES IS REFUSED BY NAME (P69 H6c2, the sibling of the universe's NotUtf8 cells): POSIX
// takes a name as bytes, and a file system that stores UTF-8 names alone (APFS) refuses one that is no UTF-8 (EILSEQ).
// The writer reports that refusal NAMING the target -- by its own bytes, the home's lossless rendering of a POSIX
// name (never an escape, which another name could spell) -- and writes nothing. Where the file system takes the
// name, the refusal cannot arise; the case says so.
TEST(LinkWriter, ATargetTheFileSystemRefusesIsRefusedByName) {
#if defined(_WIN32)
    GTEST_SKIP() << "a Windows name is UTF-16, never refused by the file system for its bytes";
#else
    ScratchDir scratch{Location::Temp, "link-writer-refused-name"};
    // Does this file system take the name? Asked of the OS itself, beside the target.
    std::string const probe = (scratch.path() / std::string{"probe\xFF"}).string();
    int const         fd    = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    int const         why   = errno;
    if (fd >= 0) {
        ::close(fd);
        GTEST_SKIP() << "this file system takes a name that is no UTF-8: the refusal cannot arise here";
    }
    ASSERT_EQ(why, EILSEQ) << "the probe was refused for another reason: " << std::strerror(why);
    fs::path const     target = scratch.path() / std::string{"out\xFF.o"};
    auto const         image  = makeImage({0x01});
    DiagnosticReporter rep;
    bool               written = true;
    ASSERT_NO_THROW(written = linker::writeImage(image, target, rep));
    EXPECT_FALSE(written);
    std::string const quoted = "'" + target.native() + "'";   // the whole target, as POSIX hands it
    // ... and the HOST'S OWN REASON, by number first, its text the system's (P69 H6c2): the probe's errno, never a
    // list of guesses.
    std::string const reason   = "errno=" + std::to_string(why) + ": " + std::strerror(why);
    bool              named    = false;
    bool              reasoned = false;
    std::string       said;
    for (auto const& d : rep.all()) {
        if (d.actual.find(quoted) != std::string::npos) named = true;
        if (d.actual.find(reason) != std::string::npos) reasoned = true;
        said += "\n  [" + std::to_string(static_cast<unsigned>(d.code)) + "] " + d.actual;
    }
    EXPECT_TRUE(named) << "no diagnostic of the refused write names its whole target by its own bytes:" << said;
    EXPECT_TRUE(reasoned) << "no diagnostic of the refused write names the host's reason `" << reason << "`:" << said;
    std::error_code ec;
    EXPECT_EQ(dss::core::entryKind(dss::core::HostPath{target}, dss::core::Use::Leaf, ec), dss::core::EntryKind::Absent)
        << "the refused write left something behind";
#endif
}

// ── End-to-end pipeline: c source → ELF .o on disk ───────────

TEST(LinkWriter, EndToEndAssembleLinkWriteToDisk) {
    // The LK10 cycle 1 acceptance pin: prove the
    // AssembledModule → link → writeImage chain hits disk
    // without a system linker. Uses a hand-constructed module
    // (matches the existing ELF test pattern in
    // test_elf_writer.cpp) rather than the full c CST
    // pipeline — the latter requires ML7 callconv lowering of
    // the virtual `arg` pseudo-op to mov-from-arg-register
    // sequences that the x86_64 assembler can encode. `ret` IS
    // declared at `x86_64.target.json` (opcode 0xC3); `arg`
    // is intentionally a virtual op without an encoding block
    // (line 22 of the same JSON documents this). The c →
    // ELF link chain is exercised end-to-end at LK10 cycle 2
    // once ML7 callconv lowering wires through the driver.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());

    AssembledModule module;
    module.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};  // x86_64 `ret` — one byte, well-formed
    module.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto image = linker::link(module, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_TRUE(image.ok());
    ASSERT_FALSE(image.bytes.empty());

    ScratchDir scratch{Location::Temp, "link-writer"};
    auto const elfPath = scratch.path() / "f.o";
    ASSERT_TRUE(linker::writeImage(image, elfPath, rep));
    EXPECT_EQ(rep.errorCount(), 0u);

    ASSERT_TRUE(fs::exists(elfPath));
    EXPECT_EQ(fs::file_size(elfPath), image.bytes.size());

    // ELF magic (\x7FELF) — pins that the bytes on disk are
    // actually the linker's output, not zeros or stale data.
    std::ifstream in(elfPath, std::ios::binary);
    char hdr[4] = {0};
    in.read(hdr, 4);
    EXPECT_EQ(static_cast<unsigned char>(hdr[0]), 0x7Fu);
    EXPECT_EQ(hdr[1], 'E');
    EXPECT_EQ(hdr[2], 'L');
    EXPECT_EQ(hdr[3], 'F');
}

// ── P69 H6c (plan section 6 #23, #24, #4): AN IMAGE BELOW A DIRECTORY THE PLAIN RULE WOULD TRIM AS A LEAF ──────
//
// `S\d \a.bin` names the image in the directory `d ` (a MIDDLE name keeps its trailing space), so the write asks
// that directory as the level of the image's own leaf reference (#23), stages its sibling there (`withSuffix`, #4)
// and commits by the target's leaf reference (#24) -- with the trimmed twin `d` absent, and present: the bytes land
// in `d `, nothing is left beside them, and the twin stays empty. Read back through `\\?\`, the one spelling with no
// second reading.
TEST(LinkWriter, AnImageBelowADirectoryEndingInASpaceIsWrittenThere) {
#if !defined(_WIN32)
    GTEST_SKIP() << "POSIX resolves no name by its position: `d ` is its bytes in every use";
#else
    auto const namesIn = [](std::wstring const& dir) {
        std::vector<std::string> names;
        static_cast<void>(core::walkTree(fs::path{dir}, [&](core::TreeEntry const& e) {
            names.push_back(e.at.filename().string());
            return core::WalkStep::Prune;
        }));
        return names;
    };
    for (bool const twin : {false, true}) {
        SCOPED_TRACE(twin ? "the twin `d` present" : "the twin `d` absent");
        ScratchDir         scratch{Location::Temp, twin ? "link-writer-space-twin" : "link-writer-space"};
        std::wstring const S       = scratch.path().native();
        std::wstring const literal = L"\\\\?\\" + S + L"\\d ";
        std::wstring const twinDir = L"\\\\?\\" + S + L"\\d";
        std::error_code    ec;
        static_cast<void>(core::createDirectories(fs::path{literal}, ec));
        ASSERT_FALSE(ec) << ec.message();
        if (twin) {
            static_cast<void>(core::createDirectories(fs::path{twinDir}, ec));
            ASSERT_FALSE(ec) << ec.message();
        }
        auto const         image = makeImage({0xD5, 0x20, 0x0A});
        DiagnosticReporter rep;
        bool const         wrote = linker::writeImage(image, fs::path{S + L"\\d \\a.bin"}, rep);
        std::string        said;
        for (auto const& d : rep.all()) said += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
        ASSERT_TRUE(wrote) << "the image below `d ` was refused:" << said;
        EXPECT_EQ(rep.errorCount(), 0u) << said;
        std::ifstream             in =
            core::openInputStream(fs::path{literal + L"\\a.bin"}, std::ios::binary).value_or(std::ifstream{});
        std::vector<std::uint8_t> read{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        EXPECT_EQ(read, image.bytes) << "the image is not in `d `";
        EXPECT_EQ(namesIn(literal), std::vector<std::string>{"a.bin"})
            << "`d ` holds something besides the image (a staging sibling left), or not the image";
        if (twin) EXPECT_TRUE(namesIn(twinDir).empty()) << "the twin `d` received a file";
    }
#endif
}
