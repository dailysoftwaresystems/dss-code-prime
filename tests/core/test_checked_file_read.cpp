// THE ONE CHECKED WHOLE-FILE READ — D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT
//
// Three things are pinned here, and they are three different claims:
//
//   1. THE HELPER ITSELF answers each failure BY NAME — open, not-a-regular-file,
//      a genuine mid-read input error, a torn read in EITHER direction, and an
//      output-side allocation failure — and does not mistake an EMPTY file for
//      any of them.
//   2. THE LOADERS ARE ROUTED THROUGH IT. Each arm asserts the HELPER's wording
//      out of a real loader entry point, so a site that goes back to a
//      hand-written drain reverts to its own old text and reddens here. This is
//      the red-on-disable for the routing, per site.
//   3. NO HAND-WRITTEN DRAIN SURVIVES IN `src/`. A source scan, so a NEW loader
//      cannot reintroduce the shape by omission — which is the half that
//      routing six call sites cannot achieve on its own.
//
// ★★ WHY THE INPUT-ERROR ARM IS DRIVEN THROUGH A STREAMBUF AND NOT A FILE, and
// the row that demanded this said so first: an `ifstream` badbit CANNOT be forced
// by truncating a file. A short file reads cleanly to EOF and sets no bit at all
// — which is exactly why this hole was invisible to a truncate hammer. The
// failure has to be CONSTRUCTED, so `ThrowingStreamBuf` below is a streambuf that
// really does fail mid-read, and `readStreamChecked` is the seam that admits it.

#include "core/substrate/checked_file_read.hpp"

#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <istream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using dss::core::FileReadFailure;
using dss::core::readFileChecked;
using dss::core::readStreamChecked;

namespace {

// A streambuf that hands over `prefix` and then FAILS. `basic_istream::read`
// catches what escapes a streambuf and sets `badbit` — the one stream bit a
// genuine mid-read failure can still be seen through. (The drain form this
// helper replaces sets NOTHING observable here: ✔MEASURED, see the header.)
struct ThrowingStreamBuf : std::streambuf {
    std::string prefix;
    int         served = 0;
    explicit ThrowingStreamBuf(std::string p) : prefix(std::move(p)) {}
    int_type underflow() override {
        if (served++ == 0 && !prefix.empty()) {
            setg(prefix.data(), prefix.data(), prefix.data() + prefix.size());
            return traits_type::to_int_type(prefix[0]);
        }
        throw std::runtime_error("simulated mid-read I/O failure");
    }
};

[[nodiscard]] bool contains(std::string const& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] fs::path writeBytes(dss::test_support::ScratchDir const& dir,
                                  std::string const& name,
                                  std::string const& bytes) {
    fs::path const p = dir.path() / name;
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return p;
}

// ── the source scan's comment/string stripper ────────────────────────────────
//
// ⚠ A BARE TOKEN GREP WOULD RED ON THE FILE THAT DOCUMENTS THE FIX. The helper's
// own header explains the drain shape in prose and prints `rdbuf()` several
// times; so does every comment this cycle added at the routed call sites. A
// marker that is merely PRESENT can always be tripped by writing about it, so
// comments and string literals are blanked first and only what the compiler
// would actually compile is matched. (Same reasoning, same shape, as
// `scripts/check-no-abort-in-tests/check-no-abort-in-tests.py`.)
[[nodiscard]] std::string stripCommentsAndStrings(std::string const& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        char const c = text[i];
        char const nxt = (i + 1 < n) ? text[i + 1] : '\0';
        if (c == '/' && nxt == '/') {
            while (i < n && text[i] != '\n') ++i;
        } else if (c == '/' && nxt == '*') {
            i += 2;
            while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) {
                out.push_back(text[i] == '\n' ? '\n' : ' ');
                ++i;
            }
            i = (i + 1 < n) ? i + 2 : n;
        } else if (c == '"' || c == '\'') {
            char const quote = c;
            ++i;
            while (i < n && text[i] != quote) {
                if (text[i] == '\\') ++i;
                ++i;
            }
            ++i;
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

// The shape being refused: inserting a streambuf into a stream, i.e.
// `something << x.rdbuf()` / `something << x->rdbuf()`.
[[nodiscard]] bool lineDrainsAStreamBuf(std::string_view line) {
    auto const at = line.find("rdbuf()");
    if (at == std::string_view::npos) return false;
    auto const shift = line.rfind("<<", at);
    return shift != std::string_view::npos;
}

} // namespace

// ═══ 1. THE HELPER ═══════════════════════════════════════════════════════════

TEST(CheckedFileRead, ReadsAFileByteExact) {
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};
    // NUL, CR, LF and 0x1A: the last is the DOS end-of-file byte, which a
    // text-mode read truncates at. This is the pin that keeps the helper binary.
    std::string const bytes{"a\0b\r\nc\x1a" "d", 8};
    auto const p = writeBytes(dir, "bytes.bin", bytes);

    auto const r = readFileChecked(p);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, bytes);
    EXPECT_EQ(r->size(), 8u);
}

TEST(CheckedFileRead, AnEmptyFileIsContentNotAFailure) {
    // ★ THE TRAP THIS ARM EXISTS FOR: `ostringstream << in.rdbuf()` sets FAILBIT
    // on an empty file ("no characters inserted"), so any implementation that
    // reached for `fail()` as its error signal would call an empty config an I/O
    // error. An empty document is a CONTENT question and belongs to the parser.
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};
    auto const p = writeBytes(dir, "empty.bin", "");

    auto const r = readFileChecked(p);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->empty());
}

TEST(CheckedFileRead, AMissingFileIsAnOpenFailure) {
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};
    auto const p = dir.path() / "no-such-file.json";

    auto const r = readFileChecked(p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, FileReadFailure::OpenFailed);
    EXPECT_TRUE(contains(r.error().message, "failed to open")) << r.error().message;
    EXPECT_TRUE(contains(r.error().message, "no-such-file.json")) << r.error().message;
}

TEST(CheckedFileRead, ADirectoryIsRefusedAndNeverReadsAsEmpty) {
    // Host-split by construction, and both arms are loud: Windows refuses the
    // OPEN of a directory, POSIX admits it and fails the first read. What must
    // NOT happen on either is an empty string returned as success.
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};

    auto const r = readFileChecked(dir.path());
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(r.error().kind == FileReadFailure::OpenFailed
                || r.error().kind == FileReadFailure::NotARegularFile)
        << "kind=" << static_cast<int>(r.error().kind)
        << " message=" << r.error().message;
}

TEST(CheckedFileRead, AStreamThatFailsMidReadIsNamedAsAnInputError) {
    ThrowingStreamBuf sb{"hello"};
    std::istream in{&sb};

    auto const r = readStreamChecked(in, "<constructed>", 32);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, FileReadFailure::InputError);
    EXPECT_TRUE(contains(r.error().message, "input side")) << r.error().message;
    EXPECT_TRUE(contains(r.error().message, "<constructed>")) << r.error().message;
    // The whole point of the row: the reader must be told the READ failed, not
    // that the document's contents are wrong.
    EXPECT_TRUE(contains(r.error().message, "not a content error"))
        << r.error().message;
}

TEST(CheckedFileRead, AShortStreamIsNamedAsATornRead) {
    std::istringstream in{"0123456789"};

    auto const r = readStreamChecked(in, "<short>", 17);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, FileReadFailure::TornRead);
    EXPECT_TRUE(contains(r.error().message, "torn read")) << r.error().message;
    EXPECT_TRUE(contains(r.error().message, "17")) << r.error().message;
    EXPECT_TRUE(contains(r.error().message, "10")) << r.error().message;
    EXPECT_TRUE(contains(r.error().message, "not a content error"))
        << r.error().message;
}

TEST(CheckedFileRead, AStreamThatGrewIsAlsoNamedAsATornRead) {
    // The direction that LOOKS perfect: the measured prefix reads back cleanly
    // and parses, and the tail is simply gone. Nothing complains unless someone
    // asks whether there was more.
    std::istringstream in{"0123456789"};

    auto const r = readStreamChecked(in, "<grew>", 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, FileReadFailure::TornRead);
    EXPECT_TRUE(contains(r.error().message, "still had more to give"))
        << r.error().message;
}

TEST(CheckedFileRead, AnUnsatisfiableSizeIsNamedAsAnOutputError) {
    // C3, the output side: an allocation the result cannot hold. Named
    // separately from every input-side fault because "this machine ran out of
    // memory" and "this file is bad" have nothing to do with each other, and
    // only one of them is actionable by editing a config.
    std::istringstream in{"0123456789"};

    auto const r = readStreamChecked(in, "<huge>",
                                     std::numeric_limits<std::uintmax_t>::max());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, FileReadFailure::OutputError);
    EXPECT_TRUE(contains(r.error().message, "output side")) << r.error().message;
}

// ═══ 2. THE LOADERS ARE ROUTED THROUGH IT ════════════════════════════════════
//
// Each arm asserts the SHARED helper's wording out of a real loader entry point.
// Revert any one site to its own hand-written drain and its own old message
// ("cannot open file", "SourceBuffer::fromFile: cannot open …") comes back and
// this arm goes red. That is the per-site red-on-disable for the routing.

TEST(CheckedFileReadRouting, GrammarSchemaLoadFromFileReportsTheSharedReadFailure) {
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};
    auto const p = dir.path() / "absent.lang.json";

    auto const r = dss::GrammarSchema::loadFromFile(p);
    ASSERT_FALSE(r.has_value());
    ASSERT_FALSE(r.error().empty());
    EXPECT_TRUE(contains(r.error().front().message, "failed to open"))
        << r.error().front().message;
    EXPECT_TRUE(contains(r.error().front().message, "absent.lang.json"))
        << r.error().front().message;
}

TEST(CheckedFileReadRouting, TargetSchemaLoadFromFileReportsTheSharedReadFailure) {
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};
    auto const p = dir.path() / "absent.target.json";

    auto const r = dss::TargetSchema::loadFromFile(p);
    ASSERT_FALSE(r.has_value());
    ASSERT_FALSE(r.error().empty());
    EXPECT_TRUE(contains(r.error().front().message, "failed to open"))
        << r.error().front().message;
}

TEST(CheckedFileReadRouting, ObjectFormatSchemaLoadFromFileReportsTheSharedReadFailure) {
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};
    auto const p = dir.path() / "absent.format.json";

    auto const r = dss::ObjectFormatSchema::loadFromFile(p);
    ASSERT_FALSE(r.has_value());
    ASSERT_FALSE(r.error().empty());
    EXPECT_TRUE(contains(r.error().front().message, "failed to open"))
        << r.error().front().message;
}

TEST(CheckedFileReadRouting, SourceBufferFromFileReportsTheSharedReadFailure) {
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};
    auto const p = dir.path() / "absent.c";

    try {
        (void)dss::SourceBuffer::fromFile(p);
        ADD_FAILURE() << "SourceBuffer::fromFile accepted a missing file";
    } catch (std::runtime_error const& e) {
        std::string const what{e.what()};
        EXPECT_TRUE(contains(what, "SourceBuffer::fromFile")) << what;
        EXPECT_TRUE(contains(what, "failed to open")) << what;
    }
}

TEST(CheckedFileReadRouting, ADirectoryReachesTheLoadersAsANamedRefusal) {
    // The arm that separates "routed" from "happens to also fail": the
    // not-a-regular-file wording exists ONLY in the shared helper. No loader
    // ever produced it before, so this cannot pass against a hand-written drain
    // on a host where opening a directory succeeds.
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                     "checked-file-read"};

    auto const r = readFileChecked(dir.path());
    ASSERT_FALSE(r.has_value());
    if (r.error().kind == FileReadFailure::NotARegularFile) {
        EXPECT_TRUE(contains(r.error().message, "is not a regular file"))
            << r.error().message;
    } else {
        EXPECT_EQ(r.error().kind, FileReadFailure::OpenFailed);
    }
}

// ═══ 3. NO HAND-WRITTEN DRAIN SURVIVES IN `src/` ═════════════════════════════

TEST(CheckedFileReadGuard, NoHandWrittenStreamBufDrainRemainsInSrc) {
    fs::path const srcRoot = dss::test::repoRoot() / "src";
    ASSERT_TRUE(fs::is_directory(srcRoot)) << srcRoot.string();

    std::size_t scanned = 0;
    std::vector<std::string> findings;

    for (auto const& entry : fs::recursive_directory_iterator(srcRoot)) {
        if (!entry.is_regular_file()) continue;
        auto const ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") continue;

        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) continue;
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        ++scanned;

        std::string const code = stripCommentsAndStrings(text);
        std::istringstream lines{code};
        std::string line;
        int lineNo = 0;
        while (std::getline(lines, line)) {
            ++lineNo;
            if (lineDrainsAStreamBuf(line)) {
                findings.push_back(
                    fs::relative(entry.path(), dss::test::repoRoot())
                        .generic_string()
                    + ":" + std::to_string(lineNo) + ": " + line);
            }
        }
    }

    // FAIL-CLOSED. A scan that collapsed (a moved subtree, a drifted extension
    // filter) reports zero findings and would otherwise read as a pass — the
    // worst defect a guard can have, and one this repo has already shipped once.
    // The floor sits far below the live figure so ordinary churn never trips it.
    ASSERT_GE(scanned, 300u)
        << "the scan COLLAPSED (" << scanned << " files under " << srcRoot.string()
        << "). This is not a clean tree; it is a broken instrument.";

    std::string report;
    for (auto const& f : findings) report += "\n    " + f;
    EXPECT_TRUE(findings.empty())
        << "a hand-written `<< …rdbuf()` drain is back in src/:" << report
        << "\n  Route it through dss::core::readFileChecked "
           "(core/substrate/checked_file_read.hpp). A drain checks NOTHING: "
           "`in.bad()` cannot fire on it, `out.bad()` misses a throwing "
           "streambuf, and a short read reporting EOF sets no bit at all. "
           "D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT";
}
