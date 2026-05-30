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
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link/writer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;

namespace {

// Per-test scratch directory, removed on destruction. Atomic
// monotonic counter ensures unique paths even when stack
// addresses collide across sequential test runs (code-reviewer
// post-fold concern: `reinterpret_cast<uintptr_t>(this)` reuses
// reclaimed addresses).
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<std::uint64_t> counter{0};
        auto const base = fs::temp_directory_path()
                          / "dss-link-writer-test";
        std::error_code ec;
        fs::create_directories(base, ec);
        path_ = base / std::to_string(counter.fetch_add(1));
        fs::create_directories(path_, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] fs::path const& path() const noexcept { return path_; }
private:
    fs::path path_;
};

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
    return img;
}

} // namespace

// ── Round-trip: bytes on disk == bytes in image ─────────────────────

TEST(LinkWriter, WritesBytesVerbatim) {
    ScratchDir scratch;
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

TEST(LinkWriter, WritesAnyFormatBytesVerbatim) {
    // pr-test-analyzer Gap 5 fold: writeImage is format-blind
    // by construction (just dumps `image.bytes`). Pin this with
    // a PE-tagged LinkedImage to forestall any future drift that
    // sneaks a per-format branch into writer.cpp.
    ScratchDir scratch;
    auto const out = scratch.path() / "pe.bin";
    LinkedImage image;
    image.format = ObjectFormatKind::Pe;
    image.bytes  = {'M', 'Z', 0x00, 0x00};  // PE DOS header start
    image.expectedFuncCount = 1;
    image.resolvedFuncCount = 1;

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
    ScratchDir scratch;
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
    ScratchDir scratch;
    LinkedImage image;
    image.format = ObjectFormatKind::Elf;
    image.bytes  = {0x7F, 'E', 'L', 'F'};  // valid bytes...
    image.expectedFuncCount = 3;
    image.resolvedFuncCount = 1;  // ...but parallel-index mismatch
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
    ScratchDir scratch;
    LinkedImage image;
    image.format = ObjectFormatKind::Elf;
    image.expectedFuncCount = 1;
    image.resolvedFuncCount = 1;
    // bytes left empty — substrate invariant violation
    ASSERT_TRUE(image.ok());
    ASSERT_TRUE(image.bytes.empty());

    DiagnosticReporter rep;
    EXPECT_FALSE(linker::writeImage(image, scratch.path() / "noop.bin", rep));
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_ImageNotOk) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(LinkWriter, RejectsMissingParentDirectory) {
    ScratchDir scratch;
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
    ScratchDir scratch;
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

// ── End-to-end pipeline: c-subset source → ELF .o on disk ───────────

TEST(LinkWriter, EndToEndAssembleLinkWriteToDisk) {
    // The LK10 cycle 1 acceptance pin: prove the
    // AssembledModule → link → writeImage chain hits disk
    // without a system linker. Uses a hand-constructed module
    // (matches the existing ELF test pattern in
    // test_elf_writer.cpp) rather than the full c-subset CST
    // pipeline — the latter requires ML7 callconv lowering of
    // the virtual `arg` pseudo-op to mov-from-arg-register
    // sequences that the x86_64 assembler can encode. `ret` IS
    // declared at `x86_64.target.json:206` (opcode 0xC3); `arg`
    // is intentionally a virtual op without an encoding block
    // (line 22 of the same JSON documents this). The c-subset →
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
    auto image = link(module, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_TRUE(image.ok());
    ASSERT_FALSE(image.bytes.empty());

    ScratchDir scratch;
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
