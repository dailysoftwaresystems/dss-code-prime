// The pin of D-LINK-WRITER-RENAME-OVER-FAILS-WHILE-ANOTHER-PROCESS-HOLDS-THE-FILE,
// the row this file's fix closed.
//
// The contract it pins has two halves, and both are permanent. A correct compile
// never fails because another process briefly holds its output. A holder that
// never lets go ends the commit with a loud error that names it: that is the
// designed answer, not a deferral waiting on a later fix.
//
// ── WHAT THE GATE SAW, AND WHAT WAS MEASURED UNDER IT ──────────────────────────
// Round 7 of cycle P68 failed `Program_CuParallelism.FrontHalfMultiTuPoolIsRepeatStable`
// once, on run 14 of its loop, with eight legs on one workstation:
//     link::writeBytes: staged the bytes but could not rename the temp over
//     '…/repeat_out/repeat0.o': Permission denied.
// `repeat0.o` is the ONE output that test re-reads after every run. Driving the
// test's own pattern out of tree (stage, close, commit over the target, read it
// back) on this workstation with the CPU at 100%, ✔MEASURED:
//   * WHICH HANDLE — the TARGET, never the staged temp. The classic replace
//     (`MoveFileExW(REPLACE_EXISTING)`, what both standard libraries' `rename`
//     call) was refused with `ERROR_ACCESS_DENIED` (5) on 9 / 10000, 44 / 8000
//     and 68 / 20000 commits; a held TEMP refuses with 32 instead, and the gate's
//     text `Permission denied` is exactly libstdc++'s spelling of 5 (32 reads
//     `Input/output error` there — both measured).
//   * WHICH SHARE MODE — it shares DELETE. Every time the classic replace was
//     refused, a POSIX-semantics replace tried IMMEDIATELY on the same staged
//     file succeeded (9 / 9); used for EVERY commit, the POSIX-semantics
//     replace was refused 0 times in 18000.
//   * HOW LONG — measured with nothing else touching the file, the hold lasted
//     2.9–21.5 ms (median 10.8 ms). 🧠 A real-time scan of the freshly written
//     file: the holder pends other opens of it (a filter or an oplock break
//     does; a bare handle cannot), and Defender real-time protection is on.
//
// ── THE PINS, AND WHAT EACH ONE CAN SEE ────────────────────────────────────────
// Every holder here is ONE MORE HANDLE, opened by a route that is not the code
// under test and released only by the TEST — by an explicit call or through the
// commit's own patience callback, never by a timer. Nothing below waits on a
// wall-clock number of its own: the only duration involved is the writer's
// `linker::detail::kCommitHolderWait`, and it is READ, never copied.
//
//   (A) a holder that SHARES DELETE — the measured one — never refuses the
//       commit: the name serves the new bytes while the holder keeps reading the
//       old file, exactly what POSIX `rename(2)` does.
//   (B) a holder that WITHHOLDS delete is WAITED OUT: refused, the patience
//       callback releases it, and the retry lands.
//   (C) THE CONTROL: a holder that NEVER lets go is refused, LOUDLY — the target,
//       the Windows error number, the holder's pid — and the previous artifact
//       survives byte-intact with no staging residue.
//   (D) the PRODUCTION commit keeps asking until its cap: more than one refusal
//       before it gives up — a COUNT, not a clock.
//   (E) a holder of the STAGED file is waited out the same way.
//   (F)/(G) a DIRECTORY target and a READ-ONLY target are refused at once:
//       waiting cannot change either, so the patience is never consulted.
//
// ★ RED ON DISABLE, per mechanism — the fix has two, and one mutant cannot cover
// both (✔ the transcript is in the lane's findings):
//   * drop `kRenameFlagPosixSemantics` from the replace in `writer.cpp`
//     (`renameByHandle`)       -> (A) goes red; (B)–(G) stay green.
//   * make the commit loop stop consulting its patience after a holder's
//     refusal                  -> (B), (D), (E) go red; (A), (C), (F), (G) stay green.
//   (C) is the named GREEN CONTROL under both: a holder that never lets go must
//   be refused loudly by name whichever mechanism is broken.
//
// ⓘ POSIX HAS NO SUCH REFUSAL, and every pin says so rather than skipping:
// `rename(2)` is a directory operation that never consults another open
// descriptor (the measurement `writer.cpp` records for the staging temp). There,
// (A)–(E) assert that the commit lands with ZERO holder refusals and the holder
// still reads the old bytes; (G) asserts the replace lands, because a file's
// mode does not bind a rename. No `#ifdef` gates a TEST — only an expectation.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/writer.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// HOST includes, and they gate no test: the holder is spelled in each host's own
// vocabulary — a share mode on Windows, a plain descriptor on POSIX, which has
// no share modes at all.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

[[nodiscard]] std::vector<std::uint8_t> bytesOf(std::string_view s) {
    return {s.begin(), s.end()};
}

[[nodiscard]] std::vector<std::uint8_t> readAll(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Create `p` holding `content` by a route that is NOT the code under test.
void putFile(fs::path const& p, std::string_view content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    ASSERT_TRUE(fs::exists(p)) << "test setup failed to create '" << p.string() << "'";
}

[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

[[nodiscard]] std::string allMessages(DiagnosticReporter const& rep) {
    std::string joined;
    for (auto const& d : rep.all()) {
        joined += d.actual;
        joined += '\n';
    }
    return joined;
}

// Names of any staging temps left in `dir` — `writeBytes` must remove its
// `<target>.dsstmp-<pid>-<n>` on EVERY path, the refused commit included.
[[nodiscard]] std::vector<std::string> stagingResidue(fs::path const& dir) {
    std::vector<std::string> found;
    std::error_code          ec;
    for (auto const& entry : fs::directory_iterator(dir, ec)) {
        auto const name = entry.path().filename().string();
        if (name.find(".dsstmp-") != std::string::npos) found.push_back(name);
    }
    EXPECT_FALSE(static_cast<bool>(ec)) << ec.message();
    return found;
}

[[nodiscard]] unsigned long thisProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// How the writer names a holder that is this very process.
[[nodiscard]] std::string thisProcessAsAHolder() {
    return "(pid " + std::to_string(thisProcessId()) + ")";
}

// ONE MORE OPEN HANDLE on a file — the holder. Released by the destructor, or
// earlier by `release()`; releasing twice is a no-op, so a patience callback may
// release on every call it receives.
class Holder {
public:
    enum class Shares : std::uint8_t {
        // The measured real holder's mode: FILE_SHARE_READ|WRITE|DELETE.
        EverythingIncludingDelete,
        // What `std::ifstream`, an editor or a hex viewer open with:
        // FILE_SHARE_READ|WRITE and NOT delete — the one kind a POSIX-semantics
        // replace cannot step past.
        ReadAndWriteButNotDelete,
    };

    Holder(fs::path const& p, Shares shares) {
#ifdef _WIN32
        DWORD const share = shares == Shares::EverythingIncludingDelete
                                ? (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
                                : (FILE_SHARE_READ | FILE_SHARE_WRITE);
        handle_ = ::CreateFileW(p.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        (void)shares;   // POSIX has no share modes: an open descriptor is all a holder is
        fd_ = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    }
    ~Holder() { release(); }
    Holder(Holder const&)            = delete;
    Holder& operator=(Holder const&) = delete;

    [[nodiscard]] bool holding() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }

    void release() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

    // The bytes as THIS HANDLE sees them, from offset 0 — the file the holder
    // opened, whatever its old name now points at.
    [[nodiscard]] std::vector<std::uint8_t> bytesThroughTheHandle() const {
        std::vector<std::uint8_t> out;
        unsigned char             chunk[4096];
#ifdef _WIN32
        LARGE_INTEGER zero{};
        if (!::SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN)) return out;
        DWORD got = 0;
        while (::ReadFile(handle_, chunk, sizeof chunk, &got, nullptr) && got > 0) {
            out.insert(out.end(), chunk, chunk + got);
        }
#else
        off_t offset = 0;
        for (;;) {
            ssize_t const got = ::pread(fd_, chunk, sizeof chunk, offset);
            if (got <= 0) break;
            out.insert(out.end(), chunk, chunk + got);
            offset += got;
        }
#endif
        return out;
    }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// A patience for the pins that must see a holder WAITED OUT: on the first
// refusal it releases the holder — the handshake — and it answers "try again"
// for as long as the writer's OWN cap allows, so a regression that never lands
// fails as an assertion rather than as a hung test. It never sleeps: the holder
// it releases is the only one there is, so the next attempt is the one that
// must land.
class ReleaseOnFirstRefusal {
public:
    explicit ReleaseOnFirstRefusal(Holder& holder) : holder_(holder) {}

    bool operator()(std::uint32_t refusalsSoFar) {
        ++asked_;
        lastCount_ = refusalsSoFar;
        if (asked_ == 1) started_ = std::chrono::steady_clock::now();
        holder_.release();
        return std::chrono::steady_clock::now() - started_
             < linker::detail::kCommitHolderWait;
    }

    [[nodiscard]] std::uint32_t asked() const { return asked_; }
    [[nodiscard]] std::uint32_t lastCount() const { return lastCount_; }

private:
    Holder&                               holder_;
    std::uint32_t                         asked_     = 0;
    std::uint32_t                         lastCount_ = 0;
    std::chrono::steady_clock::time_point started_{};
};

// The refusal count the writer's diagnostic reports ("… refused it N time(s)
// across …"), or 0 when absent.
[[nodiscard]] unsigned long reportedRefusals(std::string const& text) {
    std::smatch     m;
    std::regex const rx{R"(refused it (\d+) time\(s\))"};
    if (!std::regex_search(text, m, rx)) return 0;
    return std::stoul(m[1].str());
}

}  // namespace

// ── (A) the measured holder: shares DELETE, never refuses the commit ──────────
TEST(LinkWriterCommitHolder, AHolderThatSharesDeleteNeverRefusesTheCommit) {
    ScratchDir scratch{Location::Temp, "link-writer-commit-sharesdelete"};
    auto const out  = scratch.path() / "repeat0.o";
    auto const gen1 = bytesOf("generation 1: the previous build's object");
    auto const gen2 = bytesOf("generation 2: this build's object, which must land");

    DiagnosticReporter rep;
    ASSERT_TRUE(linker::writeBytes(gen1, out, rep)) << allMessages(rep);

    Holder holder{out, Holder::Shares::EverythingIncludingDelete};
    ASSERT_TRUE(holder.holding()) << "the holder could not open the previous artifact, "
                                     "so this test would pin nothing";
    ASSERT_EQ(holder.bytesThroughTheHandle(), gen1) << "test premise: the holder reads "
                                                       "the previous generation";

    EXPECT_TRUE(linker::writeBytes(gen2, out, rep))
        << "a holder that SHARES DELETE refused the commit. That is the holder "
           "the gate met — a real-time scan of the previous artifact, measured "
           "sharing delete — and a compile that fails because of it is a wrong "
           "answer. Diagnostics:\n"
        << allMessages(rep);
    EXPECT_EQ(rep.errorCount(), 0u) << allMessages(rep);
    EXPECT_EQ(readAll(out), gen2) << "the name must serve THIS build's bytes";
    EXPECT_EQ(holder.bytesThroughTheHandle(), gen1)
        << "the holder must keep reading the file it opened: the commit gives the "
           "name a NEW file, it never rewrites one under an open handle";
    EXPECT_TRUE(stagingResidue(scratch.path()).empty());

    holder.release();
    EXPECT_EQ(readAll(out), gen2) << "the new artifact must outlive the holder";
}

// ── (B) a holder that withholds delete is waited out ──────────────────────────
TEST(LinkWriterCommitHolder, AHolderThatLetsGoIsWaitedOutAndTheCommitLands) {
    ScratchDir scratch{Location::Temp, "link-writer-commit-waitedout"};
    auto const target = scratch.path() / "artifact.bin";
    auto const staged = scratch.path() / "artifact.bin.staged";
    putFile(target, "generation 1");
    putFile(staged, "generation 2");

    Holder holder{target, Holder::Shares::ReadAndWriteButNotDelete};
    ASSERT_TRUE(holder.holding());

    ReleaseOnFirstRefusal patience{holder};
    auto const outcome = linker::detail::commitReplacing(
        staged, target, [&patience](std::uint32_t n) { return patience(n); });

    EXPECT_TRUE(outcome.committed) << "the holder let go, yet the commit did not land: "
                                   << outcome.refusal;
    EXPECT_TRUE(outcome.refusal.empty()) << outcome.refusal;
#ifdef _WIN32
    EXPECT_GE(outcome.holderRefusals, 1u)
        << "a holder withholding FILE_SHARE_DELETE must refuse the first attempt — "
           "otherwise this test never exercised the wait";
    EXPECT_EQ(patience.asked(), outcome.holderRefusals)
        << "every holder refusal must be offered to the patience, and only those";
    EXPECT_EQ(patience.lastCount(), outcome.holderRefusals)
        << "the running count handed to the patience must be the count";
#else
    EXPECT_EQ(outcome.holderRefusals, 0u)
        << "rename(2) never consults another open descriptor";
    EXPECT_EQ(patience.asked(), 0u);
#endif
    EXPECT_EQ(readAll(target), bytesOf("generation 2"));
    EXPECT_FALSE(fs::exists(staged)) << "a landed commit leaves no staged file behind";
}

// ── (C) THE CONTROL: a holder that never lets go is refused, loudly, by name ──
TEST(LinkWriterCommitHolder, AHolderThatNeverLetsGoIsRefusedLoudlyAndByName) {
    ScratchDir scratch{Location::Temp, "link-writer-commit-neverletsgo"};
    auto const out  = scratch.path() / "repeat0.o";
    auto const gen1 = bytesOf("generation 1: must survive a refused commit byte-intact");
    auto const gen2 = bytesOf("generation 2: cannot land while the holder holds");

    DiagnosticReporter setup;
    ASSERT_TRUE(linker::writeBytes(gen1, out, setup)) << allMessages(setup);

    Holder holder{out, Holder::Shares::ReadAndWriteButNotDelete};
    ASSERT_TRUE(holder.holding());

    DiagnosticReporter rep;
    bool const         wrote = linker::writeBytes(gen2, out, rep);
    std::string const  text  = allMessages(rep);
#ifdef _WIN32
    EXPECT_FALSE(wrote) << "a holder that never lets go was reported as a success";
    EXPECT_EQ(rep.errorCount(), 1u) << "one failure, one diagnostic:\n" << text;
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_ImageWriteOpenFailed)) << text;
    EXPECT_NE(text.find("repeat0.o"), std::string::npos) << "the refusal must name the target:\n"
                                                        << text;
    EXPECT_NE(text.find("Windows error "), std::string::npos)
        << "the refusal must carry the holder's error BY NUMBER — the text is "
           "localized, the number is what can be searched on:\n"
        << text;
    EXPECT_NE(text.find(thisProcessAsAHolder()), std::string::npos)
        << "the refusal must NAME the holder — here this very process:\n"
        << text;
    EXPECT_NE(text.find("NOT updated"), std::string::npos) << text;
    EXPECT_GE(reportedRefusals(text), 1ul) << "the refusal must say how often it was refused:\n"
                                           << text;
    EXPECT_EQ(readAll(out), gen1) << "a refused commit must leave the previous artifact "
                                     "byte-intact";
#else
    EXPECT_TRUE(wrote) << "rename(2) is never refused because a descriptor is open: " << text;
    EXPECT_EQ(readAll(out), gen2);
#endif
    EXPECT_EQ(holder.bytesThroughTheHandle(), gen1);
    EXPECT_TRUE(stagingResidue(scratch.path()).empty())
        << "the staged temp must be removed on the refused path too";
}

// ── (D) the production commit keeps asking until its cap ──────────────────────
TEST(LinkWriterCommitHolder, TheProductionCommitKeepsAskingTheHolderUntilItsCap) {
    ScratchDir scratch{Location::Temp, "link-writer-commit-cap"};
    auto const target = scratch.path() / "artifact.bin";
    auto const staged = scratch.path() / "artifact.bin.staged";
    putFile(target, "generation 1");
    putFile(staged, "generation 2");

    Holder holder{target, Holder::Shares::ReadAndWriteButNotDelete};
    ASSERT_TRUE(holder.holding());

    auto const outcome = linker::detail::commitReplacing(staged, target);
#ifdef _WIN32
    EXPECT_FALSE(outcome.committed);
    EXPECT_GT(outcome.holderRefusals, 1u)
        << "the production commit gave the holder a single chance. It must keep "
           "asking until linker::detail::kCommitHolderWait has passed — the first "
           "retry is unconditional, so anything below 2 means it did not wait at "
           "all. Refusal: "
        << outcome.refusal;
    EXPECT_NE(outcome.refusal.find(thisProcessAsAHolder()), std::string::npos)
        << outcome.refusal;
    EXPECT_EQ(readAll(target), bytesOf("generation 1"));
    EXPECT_TRUE(fs::exists(staged))
        << "commitReplacing must leave the staged file to its caller, who owns its cleanup";
#else
    EXPECT_TRUE(outcome.committed) << outcome.refusal;
    EXPECT_EQ(outcome.holderRefusals, 0u);
#endif
}

// ── (E) a holder of the STAGED file is waited out the same way ────────────────
TEST(LinkWriterCommitHolder, AHolderOfTheStagedFileIsWaitedOutToo) {
    ScratchDir scratch{Location::Temp, "link-writer-commit-stagedheld"};
    auto const target = scratch.path() / "artifact.bin";
    auto const staged = scratch.path() / "artifact.bin.staged";
    putFile(target, "generation 1");
    putFile(staged, "generation 2");

    Holder holder{staged, Holder::Shares::ReadAndWriteButNotDelete};
    ASSERT_TRUE(holder.holding());

    ReleaseOnFirstRefusal patience{holder};
    auto const outcome = linker::detail::commitReplacing(
        staged, target, [&patience](std::uint32_t n) { return patience(n); });

    EXPECT_TRUE(outcome.committed) << outcome.refusal;
#ifdef _WIN32
    EXPECT_GE(outcome.holderRefusals, 1u)
        << "opening the staged file for the rename needs DELETE access, which a "
           "holder withholding FILE_SHARE_DELETE must refuse";
    EXPECT_EQ(patience.asked(), outcome.holderRefusals);
#else
    EXPECT_EQ(outcome.holderRefusals, 0u);
#endif
    EXPECT_EQ(readAll(target), bytesOf("generation 2"));
}

// ── (F) a DIRECTORY target is refused at once ──────────────────────────────────
TEST(LinkWriterCommitHolder, ADirectoryTargetIsRefusedAtOnceNeverWaitedFor) {
    ScratchDir scratch{Location::Temp, "link-writer-commit-directory"};
    auto const target = scratch.path() / "occupied-directory";
    auto const staged = scratch.path() / "occupied-directory.staged";
    ASSERT_TRUE(fs::create_directory(target));
    putFile(staged, "bytes");

    std::uint32_t asked   = 0;
    auto const    outcome = linker::detail::commitReplacing(
        staged, target, [&asked](std::uint32_t) {
            ++asked;
            return false;
        });

    EXPECT_FALSE(outcome.committed);
    EXPECT_EQ(asked, 0u) << "waiting cannot turn a directory into a file; the "
                            "patience must not even be asked. Refusal: "
                         << outcome.refusal;
    EXPECT_EQ(outcome.holderRefusals, 0u);
    EXPECT_FALSE(outcome.refusal.empty());
#ifdef _WIN32
    EXPECT_NE(outcome.refusal.find("DIRECTORY"), std::string::npos) << outcome.refusal;
#endif
    EXPECT_TRUE(fs::is_directory(target));
}

// ── (G) a READ-ONLY target is refused at once on Windows ──────────────────────
TEST(LinkWriterCommitHolder, AReadOnlyTargetIsRefusedAtOnceNeverWaitedFor) {
    ScratchDir scratch{Location::Temp, "link-writer-commit-readonly"};
    auto const target = scratch.path() / "artifact.bin";
    auto const staged = scratch.path() / "artifact.bin.staged";
    putFile(target, "generation 1");
    putFile(staged, "generation 2");

    constexpr auto kWrite =
        fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    std::error_code ec;
    fs::permissions(target, kWrite, fs::perm_options::remove, ec);
    ASSERT_FALSE(static_cast<bool>(ec)) << ec.message();
    // Restore on every path out, so the scratch directory can be removed.
    struct Restore {
        fs::path p;
        ~Restore() {
            std::error_code rec;
            if (fs::exists(p, rec)) {
                fs::permissions(p, fs::perms::owner_write, fs::perm_options::add, rec);
            }
        }
    } const restore{target};

    std::uint32_t asked   = 0;
    auto const    outcome = linker::detail::commitReplacing(
        staged, target, [&asked](std::uint32_t) {
            ++asked;
            return false;
        });

    EXPECT_EQ(asked, 0u) << outcome.refusal;
    EXPECT_EQ(outcome.holderRefusals, 0u);
#ifdef _WIN32
    EXPECT_FALSE(outcome.committed) << "a read-only file is never replaced";
    EXPECT_NE(outcome.refusal.find("READ-ONLY"), std::string::npos) << outcome.refusal;
    EXPECT_EQ(readAll(target), bytesOf("generation 1"));
#else
    EXPECT_TRUE(outcome.committed) << "a file's mode does not bind rename(2): "
                                   << outcome.refusal;
    EXPECT_EQ(readAll(target), bytesOf("generation 2"));
#endif
}
