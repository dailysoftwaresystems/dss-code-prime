// The staging-temp claim must be EXCLUSIVE — `link::writeBytes` may only ever
// stage into a slot IT CREATED. D-LINK-WRITER-NOREPLACE-WIDE-PATH-UNSUPPORTED.
//
// WHY THIS FILE EXISTS. `writeBytes` stages every artifact in a sibling
// `<target>.dsstmp-<pid>-<n>` and renames it over the target, so each write
// yields a NEW file identity (D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING — macOS
// 26 attaches a per-inode exec DENY, so a reused inode is a dead binary). The
// claim itself is made with `fopen` mode "x" (O_CREAT|O_EXCL), which is what
// makes it race-free against a concurrent process AND makes it STEP OVER a
// stale `.dsstmp-*` left by a killed run instead of silently adopting and
// TRUNCATING it — the exact defect `tests/test_support/scratch_dir.hpp` records
// one layer down, where a PID-seeded path was ADOPTED rather than refused
// because `create_directories` reports success on a directory that already
// exists.
//
// NOTHING PINNED THAT. Before this file, disabling the exclusive create left
// EVERY existing link-writer test GREEN. That count is LEG-DEPENDENT, and
// spelling it out rather than quoting one number is the point — a bare figure
// here would repeat the exact mistake the per-arm notes below exist to correct:
//   * POSIX:   19 — 12 in `test_link_writer.cpp` + 7 in
//                   `test_link_writer_new_identity.cpp`.
//   * Windows: 18 — `LinkWriter.ExecutableOutputCarriesExecuteBitPosix` is
//                   COMPILED OUT by `#ifndef _WIN32` (the +x bit it pins is a
//                   Unix-mode concept, D-OUTPUT-EXEC-BIT), so gtest sees 11
//                   there, not 12.
// MEASURED both ways: by counting `TEST(` in the sources and by
// `--gtest_list_tests` on this host. Whichever leg you are on, none of those
// tests touched exclusivity: `ConcurrentWritersInOneDirectoryDoNotCollide`
// gives each thread a DIFFERENT target path so the atomic counter alone keeps
// the candidates unique, and no test ever pre-created a stale temp.
//
// ★ HOW TO DISABLE THE GUARD — STATED PER ARM, because the two arms of
// `createExclusiveBinary` are genuinely DIFFERENT CODE and one measurement
// cannot cover both (the TF-C104 lesson recurring: one library's behaviour is
// not another's):
//   * Windows: `::_wfopen(p.c_str(), L"wbxN")` -> `L"wbN"`.
//   * POSIX:   drop `O_EXCL` from
//              `::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666)`.
// There is no `"wbx"` on the POSIX arm to swap, so a Windows-only
// `"wbx"` -> `"wb"` run says NOTHING about the POSIX leg, and vice versa.
//
// ★ THE MODE CARRIES A SECOND, INDEPENDENT GUARD, with its own disable lever:
// the `N` (Windows `_O_NOINHERIT`) and the `O_CLOEXEC` (POSIX) that keep the
// claimed handle from crossing into a spawned child. Dropping EITHER leaves
// exclusivity perfectly intact and every test above green — which is exactly
// why it gets its own pin at the bottom of this file. See
// `AClaimedStagingTempNeverCrossesIntoASpawnedChild`.
//
// ★ WHAT RED-ON-DISABLE ACTUALLY COVERS, PER HOST. Do not over-read these pins
// on POSIX — and do not under-read them either. Each bullet below was MEASURED
// against the primitive itself (UCRT `_wfopen`; glibc `open`), both modes:
//
//   * THE EXCLUSIVITY ITSELF — a second claim on an existing REGULAR FILE
//     returning null vs non-null — is RED ON DISABLE ON BOTH ARMS (Windows:
//     refused -> NON-NULL; POSIX: refused -> OPENED). This is the core
//     property, and it is genuinely pinned on every leg.
//
//   * "...AND LEAVES IT BYTE-INTACT" discriminates ONLY ON WINDOWS. `L"wb"`
//     truncates at OPEN time (MEASURED: a 14-byte occupant becomes 0 bytes).
//     On POSIX, `open(O_WRONLY | O_CREAT)` carries no `O_TRUNC`, so the
//     occupant survives a non-exclusive open (MEASURED: still 14 bytes) and
//     that half passes either way. It still earns its place — it is what
//     catches the Windows-side regression, and on POSIX it pins that no
//     `O_TRUNC` ever creeps into the flags.
//
//   * `AClaimOnAnExistingDirectoryIsRefused` passes EITHER WAY ON BOTH ARMS —
//     not POSIX only. POSIX refuses a directory with EEXIST under `O_EXCL` and
//     with EISDIR without it; UCRT `_wfopen` refuses it under `L"wbx"` AND
//     under `L"wb"` (both MEASURED). So it pins a DIFFERENT sub-property —
//     that a non-file squatter is stepped over rather than killing the emit —
//     and pins nothing whatever about exclusivity. Kept, and labelled, rather
//     than deleted or miscredited.
//
// HOW IT IS PINNED, AND WHY DIRECTLY. The property is invisible through
// `writeBytes`'s own surface: the candidate name embeds a pid and a
// process-wide counter, so a test that merely pre-creates a plausible name and
// watches it survive proves NOTHING — under any drift of the naming scheme that
// file was never a candidate and survives trivially. So the primary pins call
// the claim primitive `linker::detail::createExclusiveBinary` DIRECTLY (the
// `detail` sub-namespace convention this tree already uses for independently
// testable sub-builders — `dss::macho::detail::buildAdHocCodeSignature`,
// `dss::link::format::detail::writeU32LEAt`). Deterministic, no pid/counter
// coupling, and unambiguous under disable: with "wb" the second claim on an
// existing path returns NON-NULL and the occupant's bytes are destroyed.
//
// The last test then closes the loop at the `writeBytes` level — including the
// retry/`exists()`-continue branch, which no other test reaches — and it earns
// the right to pre-create names by PROVING they are the writer's candidates.
// See its own header comment for how that proof works.
//
// NO `#ifdef` gates any TEST here: every one executes on every leg. The single
// host conditional is the `getpid` include dance, exactly as `writer.cpp` and
// `scratch_dir.hpp` do it.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/substrate/path_identity.hpp"   // entryKind / reachedKind: what a planted link IS
#include "link/writer.hpp"
#include "os_link.hpp"                         // links made by the OS: MinGW's library makes none
#include "scratch_dir.hpp"
#include "system_reason_text.hpp"                // the test side's one oracle for an OS code's own text

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// `getpid` — the SAME seed `writer.cpp`'s `processSeed()` draws, so the last
// test can reconstruct the exact staging-temp names `writeBytes` will try in
// THIS process. A HOST include split (the `scratch_dir.hpp` precedent), never a
// target/format/language one, and it gates no test. The second half of each arm
// is the handle-inheritance probe: `_get_osfhandle` + `GetHandleInformation` on
// Windows, `fcntl(F_GETFD)` on POSIX — the same "ask the OS what it actually
// did" shape, spelled in each host's own vocabulary.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>   // the denied directory's DACL (the reason cells)
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};

// Owns whatever the claim returned, so a failing assertion cannot leak a
// handle. That matters beyond tidiness on Windows: an open handle blocks
// `ScratchDir`'s `remove_all`, which would turn one red assertion into a
// cascade of stale-directory warnings in every later run.
using Claimed = std::unique_ptr<std::FILE, FileCloser>;

// The claim's stream, or null for a refusal (whose reason the cells that need it read from the primitive itself).
[[nodiscard]] Claimed claim(fs::path const& p) {
    auto claimed = linker::detail::createExclusiveBinary(p);
    return Claimed{claimed ? *claimed : nullptr};
}

[[nodiscard]] std::vector<std::uint8_t> readAll(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(std::string_view s) {
    return {s.begin(), s.end()};
}

// Create `p` holding `content`, by a route that is NOT the code under test —
// so a test whose premise is "this path already exists" does not depend on the
// primitive it is about to interrogate.
void putFile(fs::path const& p, std::string_view content) {
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    EXPECT_TRUE(fs::exists(p))
        << "test setup failed to create '" << p.string() << "'";
}

// Every regular file directly under `dir`, name -> exact bytes. Used to assert
// that a refused claim changed NOTHING — not the occupant, not its neighbours,
// and no stray file added or removed.
[[nodiscard]] std::map<std::string, std::vector<std::uint8_t>>
snapshotDir(fs::path const& dir) {
    std::map<std::string, std::vector<std::uint8_t>> shot;
    std::error_code                                  ec;
    for (auto const& entry : fs::directory_iterator(dir, ec)) {
        auto const name = entry.path().filename().string();
        shot[name] = entry.is_directory() ? std::vector<std::uint8_t>{}
                                          : readAll(entry.path());
    }
    EXPECT_FALSE(static_cast<bool>(ec))
        << "directory_iterator('" << dir.string()
        << "') failed: " << ec.message()
        << " — the snapshot probe is broken, so the comparison below proves "
           "nothing.";
    return shot;
}

[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

// Concatenated text of every diagnostic — `dss::report` puts the writer's
// message in `ParseDiagnostic::actual`.
[[nodiscard]] std::string allMessages(DiagnosticReporter const& rep) {
    std::string joined;
    for (auto const& d : rep.all()) {
        joined += d.actual;
        joined += '\n';
    }
    return joined;
}

// The seed `writer.cpp`'s `processSeed()` returns in this process.
[[nodiscard]] std::uint64_t hostProcessSeed() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

// The exact path `writeBytes` builds for the claim attempt whose counter value
// is `n`: the TARGET path plus an ASCII suffix (writer.cpp's "sibling by
// construction" rule — `operator+=`, not `operator/`).
[[nodiscard]] fs::path stagingCandidate(fs::path const& target,
                                        std::uint64_t   n) {
    auto p = target;
    p += ".dsstmp-" + std::to_string(hostProcessSeed()) + "-"
       + std::to_string(n);
    return p;
}

// ── THE STAGING-SLOT LEDGER — read this before adding any `writeBytes` or
// `writeImage` call to this file ───────────────────────────────────────────
//
// `writeBytes` names its candidates `<target>.dsstmp-<pid>-<n>`, where `n`
// comes from a function-local static atomic that starts at 0 and takes EXACTLY
// ONE value per claim ATTEMPT (`tempCounter.fetch_add(1)`), process-wide. The
// integration test at the bottom of this file RECONSTRUCTS those names, so it
// must know the counter's current value — which means every `writeBytes` call
// in this binary has to declare how many values it burned. This variable is
// that running total.
//
// PROCESS-LIFETIME storage, mirroring the writer's own counter, and that is
// not an optimisation: under `--gtest_repeat=N` the writer's counter keeps
// climbing across iterations, so a per-test local resetting to 0 would make
// every derived name miss from iteration 2 onward. MEASURED before this was
// static: `--gtest_repeat=2` failed on iteration 2 ("no artifact may appear
// when the write failed" — the writer sailed past decoys that were no longer
// its candidates).
//
// ★ THE RULE. A `writeBytes` caller here MUST add the number of claim ATTEMPTS
// it made to `nextSlot`. It lives at namespace scope — not inside the
// integration test, where it started — precisely SO THAT a second caller can
// exist: the file previously forbade one outright, which made the empty-span
// coverage below impossible to add. `--gtest_shuffle` means the callers' order
// is not fixed, so no caller may assume it ran first; each simply declares its
// own consumption and the total stays true in any order.
//
// ★ AND YOU ARE NOT TRUSTED TO GET IT RIGHT — step 2 of the integration test
// AUDITS this ledger on every run, in whichever order the tests execute. It
// occupies exactly `kMaxClaimAttempts` consecutive slots starting at
// `nextSlot` and REQUIRES an exhaustion failure. Under-count by one and the
// writer's last permitted attempt lands one slot PAST the decoy window, finds
// it free, claims it, the write SUCCEEDS and step 2 goes RED. Over-count by
// one and the writer's FIRST attempt lands one slot BEFORE the window, is
// free, and succeeds immediately — also RED. Drift in either direction is
// caught, so this ledger is a checked claim rather than an assumption.
std::uint64_t nextSlot = 0;

} // namespace

// ── The claim primitive, pinned directly ───────────────────────────────────

// (a) A fresh path is created and opened for writing, and the bytes land.
TEST(LinkWriterExclusiveClaim, AFreshPathIsClaimedAndTheStagedBytesLand) {
    ScratchDir scratch{Location::Temp, "link-writer-claim-fresh"};
    auto const slot = scratch.path() / "fresh.dsstmp";
    ASSERT_FALSE(fs::exists(slot)) << "test premise: the slot must start free";

    auto const payload = bytesOf("staged-generation-0");
    {
        Claimed f = claim(slot);
        ASSERT_NE(f.get(), nullptr)
            << "createExclusiveBinary refused a path that does not exist — the "
               "claim can never succeed, so no artifact can ever be written.";
        // Created by the OPEN, before anything is written.
        EXPECT_TRUE(fs::exists(slot))
            << "the claim reported success but created no file";
        ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), f.get()),
                  payload.size());
    }
    EXPECT_EQ(readAll(slot), payload);
}

// The stream is BINARY. On Windows a text-mode claim ("wx") would expand every
// 0x0A into 0x0D 0x0A and stamp 0x1A as EOF — i.e. corrupt every artifact byte
// stream that happens to contain those values, which every real ELF/PE/Mach-O
// image does. Cheap to assert, and it goes red on "wbx" -> "wx".
TEST(LinkWriterExclusiveClaim, TheClaimedStreamIsBinaryAndTranslatesNothing) {
    ScratchDir scratch{Location::Temp, "link-writer-claim-binary"};
    auto const slot = scratch.path() / "binary.dsstmp";

    std::vector<std::uint8_t> const payload{
        0x00, 0x0A, 0x0D, 0x0A, 0x1A, 0x0D, 0x00, 0xFF, 0x7F, 0x80, 0x0A};
    {
        Claimed f = claim(slot);
        ASSERT_NE(f.get(), nullptr);
        ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), f.get()),
                  payload.size());
    }
    EXPECT_EQ(fs::file_size(slot), payload.size())
        << "the claimed stream translated line endings — it is not binary";
    EXPECT_EQ(readAll(slot), payload);
}

// ── (b) THE PIN: a second claim on the SAME path is REFUSED, and the occupant
// is left BYTE-UNCHANGED. This is the assertion that goes red on "wb". ───────
TEST(LinkWriterExclusiveClaim, AClaimOnAnExistingFileIsRefusedAndLeavesItIntact) {
    ScratchDir scratch{Location::Temp, "link-writer-claim-refuse"};
    auto const slot = scratch.path() / "occupied.dsstmp";

    // Stand the occupant up THROUGH THE PRIMITIVE ITSELF: the premise of this
    // test is exactly the postcondition test (a) just proved, so there is no
    // second creation mechanism that could disagree about what "exists" means.
    auto const original = bytesOf("stale temp from a killed run — DO NOT ADOPT");
    {
        Claimed first = claim(slot);
        ASSERT_NE(first.get(), nullptr)
            << "could not establish the occupant; the refusal below would "
               "prove nothing";
        ASSERT_EQ(std::fwrite(original.data(), 1, original.size(), first.get()),
                  original.size());
    }
    ASSERT_TRUE(fs::exists(slot));
    ASSERT_EQ(readAll(slot), original) << "test premise broken before the pin";

    // ── The claim under test ──
    Claimed second = claim(slot);
    EXPECT_EQ(second.get(), nullptr)
        << "createExclusiveBinary OPENED a path that already exists. The claim "
           "is no longer exclusive — `fopen` mode \"wb\" (or anything else "
           "without \"x\") instead of \"wbx\". Consequences, both silent: "
           "`writeBytes` ADOPTS and TRUNCATES a stale `.dsstmp-*` left by a "
           "killed run, and two concurrent processes staging into one output "
           "directory can share a slot and interleave their bytes.";
    // Close it if the claim wrongly succeeded, so the read below sees the
    // committed state and no handle is leaked into the ScratchDir teardown.
    second.reset();

    // (b, second half) The refusal must not have TOUCHED the file. "wb" opens
    // with O_TRUNC, so a non-exclusive claim empties the occupant at OPEN time
    // — before a single byte is written.
    EXPECT_EQ(fs::file_size(slot), original.size())
        << "the refused claim truncated the existing file";
    EXPECT_EQ(readAll(slot), original)
        << "the refused claim rewrote the existing file's bytes — a stale "
           "staging temp was adopted rather than stepped over";
}

// A ZERO-LENGTH occupant is the shape a killed run leaves most often: the temp
// was created by the claim and the process died before `fwrite`. It must be
// refused just as firmly — and this is the case a length-based "is it in use?"
// heuristic would wave through.
TEST(LinkWriterExclusiveClaim, AClaimOnAnExistingEMPTYFileIsAlsoRefused) {
    ScratchDir scratch{Location::Temp, "link-writer-claim-empty"};
    auto const slot = scratch.path() / "empty.dsstmp";
    putFile(slot, "");
    ASSERT_TRUE(fs::exists(slot));
    ASSERT_EQ(fs::file_size(slot), 0u);

    Claimed refused = claim(slot);
    EXPECT_EQ(refused.get(), nullptr)
        << "an existing ZERO-BYTE file was claimed — the exclusive create is "
           "gone, and the most common stale-temp shape is adopted silently";
    refused.reset();

    EXPECT_TRUE(fs::exists(slot))
        << "the refused claim removed the existing file";
    EXPECT_EQ(fs::file_size(slot), 0u);
}

// A DIRECTORY occupying a candidate name must be refused too — this is what
// lets `writeBytes` step over a non-file squatter instead of failing the whole
// emit. (POSIX: O_CREAT|O_EXCL on an existing name fails regardless of its
// type; Windows: `_wfopen` cannot open a directory for writing.)
TEST(LinkWriterExclusiveClaim, AClaimOnAnExistingDirectoryIsRefused) {
    ScratchDir scratch{Location::Temp, "link-writer-claim-dir"};
    auto const slot = scratch.path() / "squatter.dsstmp";
    ASSERT_TRUE(fs::create_directory(slot));
    putFile(slot / "inhabitant.txt", "still here");

    Claimed refused = claim(slot);
    EXPECT_EQ(refused.get(), nullptr)
        << "a DIRECTORY was opened as a staging temp";
    refused.reset();

    EXPECT_TRUE(fs::is_directory(slot))
        << "the refused claim replaced a directory with a file";
    EXPECT_TRUE(fs::exists(slot / "inhabitant.txt"));
}

// (c) The refusal leaves NO trace: no stray file, no neighbour touched, no
// entry removed. Asserted as an exact before/after directory snapshot rather
// than a spot check, so anything unexpected shows up.
TEST(LinkWriterExclusiveClaim, ARefusedClaimChangesNothingInTheDirectory) {
    ScratchDir scratch{Location::Temp, "link-writer-claim-notrace"};
    auto const slot = scratch.path() / "target.bin.dsstmp-1-0";
    putFile(slot, "occupant payload");
    putFile(scratch.path() / "target.bin", "the artifact");
    putFile(scratch.path() / "neighbour.txt", "unrelated");

    auto const before = snapshotDir(scratch.path());
    ASSERT_EQ(before.size(), 3u) << "test premise: three files staged";

    Claimed refused = claim(slot);
    EXPECT_EQ(refused.get(), nullptr);
    refused.reset();

    EXPECT_EQ(snapshotDir(scratch.path()), before)
        << "a refused claim modified the directory — it must be a pure "
           "check-and-fail with no side effect at all";
}

// ANTI-VACUITY. Every refusal above would also be produced by a primitive that
// simply never opens anything (a helper that always returns null passes them
// all). Pin the positive direction on the SAME path: remove the occupant and
// the very same path becomes claimable again. So the refusals are caused by
// EXISTENCE, not by the path, the directory, or a broken primitive.
TEST(LinkWriterExclusiveClaim, TheSamePathIsClaimableAgainOnceTheOccupantIsGone) {
    ScratchDir scratch{Location::Temp, "link-writer-claim-recycle"};
    auto const slot = scratch.path() / "recycled.dsstmp";

    {
        Claimed first = claim(slot);
        ASSERT_NE(first.get(), nullptr);
    }
    {
        Claimed refused = claim(slot);
        ASSERT_EQ(refused.get(), nullptr)
            << "the occupied claim must be refused for the release below to "
               "mean anything";
    }

    ASSERT_TRUE(fs::remove(slot));
    Claimed again = claim(slot);
    EXPECT_NE(again.get(), nullptr)
        << "the path became permanently unclaimable after its occupant was "
           "removed — the primitive is not testing EXISTENCE, so every "
           "refusal asserted above proves nothing";
}

// ── The staging temp must not cross an exec ────────────────────────────────
//
// ★ A SECOND GUARD LIVES IN THE SAME MODE STRING, and nothing above touches it.
// `writeBytes` holds this handle open across its `fwrite`, and the compiler
// creates processes (`core/substrate/process_spawn.cpp` — a user build hook
// today, `git` acquisition arriving). On Windows that spawn passes
// `bInheritHandles=TRUE`, which is correct and deliberate there (the child must
// inherit the parent's stdio, with NO `STARTF_USESTDHANDLES`), so an
// INHERITABLE staging-temp handle is duplicated into the child and held for its
// lifetime. The commit rename then hits the target's own "is anyone holding
// this" check and FAILS — MEASURED, `MoveFileExW` -> `GetLastError=32`
// ERROR_SHARING_VIOLATION — and `writeBytes` reports it as "the target is
// currently open/running", pointing the operator at some other process.
//
// The POSIX consequence is different and must not be described as the same one:
// `rename(2)` does not care that a file is open (MEASURED: returns 0 either
// way). What leaks there is the DESCRIPTOR — the child gets a writable fd onto
// the compiler's staged artifact, which the spawn substrate's security note
// forbids handing to a consumer that interpolates user-supplied text.
//
// ★ WHY THE PROPERTY IS ASSERTED ON THE HANDLE RATHER THAN THROUGH A SPAWN.
// A spawn-and-observe test would need a child that OUTLIVES the parent's
// rename attempt, i.e. a timing window, and would make a deterministic
// statement depend on one. The inheritance bit is the cause and it is directly
// readable, so it is read directly: `GetHandleInformation` / `fcntl(F_GETFD)`,
// one call, no race. The end-to-end consequence was measured out of tree (both
// modes, both hosts) and is recorded at the definition site in `writer.cpp`.
//
// RED ON DISABLE, per arm and independent of every other pin here: drop the `N`
// and the Windows half goes red with `HANDLE_FLAG_INHERIT` set; drop the
// `O_CLOEXEC` and the POSIX half goes red with `FD_CLOEXEC` clear. Neither
// disturbs exclusivity, so this is the only test in the file that moves.
TEST(LinkWriterExclusiveClaim, AClaimedStagingTempNeverCrossesIntoASpawnedChild) {
    ScratchDir scratch{Location::Temp, "link-writer-claim-noinherit"};
    auto const slot = scratch.path() / "noinherit.dsstmp";

    Claimed f = claim(slot);
    ASSERT_NE(f.get(), nullptr)
        << "the claim failed, so there is no handle whose inheritance could be "
           "examined";

#ifdef _WIN32
    auto const handle =
        reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(f.get())));
    ASSERT_NE(handle, INVALID_HANDLE_VALUE)
        << "the CRT stream carries no OS handle — the probe below would be "
           "reading nothing";
    DWORD flags = 0;
    ASSERT_NE(::GetHandleInformation(handle, &flags), 0)
        << "GetHandleInformation failed (" << ::GetLastError()
        << "), so this test cannot observe the bit it exists to assert";
    EXPECT_EQ(flags & HANDLE_FLAG_INHERIT, DWORD{0})
        << "the staging-temp handle is INHERITABLE. `_wfopen` marks it so "
           "unless the mode contains `N` (_O_NOINHERIT), and "
           "`spawnAndWaitInherit` creates children with bInheritHandles=TRUE — "
           "so a build hook or `git` receives a duplicate of this handle and "
           "holds it for its lifetime, after which the commit rename fails with "
           "ERROR_SHARING_VIOLATION and blames an unrelated process.";
    // The whole flags word, not just the one bit: HANDLE_FLAG_PROTECT_FROM_CLOSE
    // has no business being set on a stream we `fclose` ourselves, and pinning
    // the word catches anything else a future mode string switches on.
    EXPECT_EQ(flags, DWORD{0}) << "unexpected handle flags 0x" << std::hex
                               << flags;
#else
    int const fd = ::fileno(f.get());
    ASSERT_GE(fd, 0) << "the stream carries no descriptor";
    int const fdFlags = ::fcntl(fd, F_GETFD);
    ASSERT_GE(fdFlags, 0)
        << "fcntl(F_GETFD) failed, so this test cannot observe the flag it "
           "exists to assert";
    EXPECT_EQ(fdFlags & FD_CLOEXEC, FD_CLOEXEC)
        << "the staging-temp descriptor is NOT close-on-exec. `O_CLOEXEC` is "
           "missing from the `open` flags, so a spawned build hook or `git` "
           "inherits a WRITABLE descriptor onto the compiler's staged artifact.";
#endif
}

// ── The EMPTY-SPAN commit path ─────────────────────────────────────────────
//
// `writeBytes` skips `fwrite` entirely when the span is empty: `fwrite` is UB
// on a null pointer even with a zero count, and `std::span::data()` is
// permitted to be null for an EMPTY span. Nothing else in the tree reaches
// that branch — `writeImage` rejects empty bytes one level up (`K_ImageEmpty`)
// and every other `writeBytes` caller passes a non-empty span — so without
// this test the branch ships unexecuted.
//
// WHAT IS PINNED is the contract writer.hpp states for this surface: it "does
// NOT gate on `bytes.empty()` (that is a LinkedImage-specific contract — an
// empty write is a legitimate raw-bytes request)". So a zero-byte request must
// COMMIT: appear at the target, at exactly zero bytes, with no error and no
// staging temp left behind.
//
// ★ HONEST ABOUT ITS STRENGTH, because the anti-vacuity standard in this file
// cuts both ways. Deleting the guard makes the call UNDEFINED, not reliably
// red — a lenient libc returns 0 for a zero-count `fwrite` and this test would
// still pass. No test can be a UB detector; that arm belongs to a sanitizer
// leg. What this test DOES catch is every DEFINED way to break the branch:
// rejecting empty input, returning false, emitting a diagnostic, skipping the
// rename so nothing commits, or writing any byte at all into the artifact.
//
// ★ IT IS A `writeBytes` CALLER, so it declares its slot consumption to the
// ledger above. See `nextSlot` for why that is mandatory and how step 2 of the
// integration test audits it.
TEST(LinkWriterExclusiveClaim, AnEmptySpanCommitsAZeroByteArtifact) {
    ScratchDir scratch{Location::Temp, "link-writer-empty-span"};
    auto const out = scratch.path() / "empty-artifact.bin";
    ASSERT_FALSE(fs::exists(out)) << "test premise: the target must start free";

    DiagnosticReporter rep;
    bool const         ok =
        linker::writeBytes(std::span<std::uint8_t const>{}, out, rep);

    // ONE claim attempt: the scratch dir is fresh, so the writer's first
    // candidate is free and it claims on the first try. Declared BEFORE the
    // assertions below so that a failure in one of them cannot skip the
    // update and silently desync the ledger for the integration test.
    nextSlot += 1;

    ASSERT_TRUE(ok) << "a zero-byte artifact was REFUSED. `writeBytes` does "
                       "not gate on `bytes.empty()` — an empty write is a "
                       "legitimate raw-bytes request (writer.hpp), and the "
                       "empty-span branch exists precisely to commit it: "
                    << allMessages(rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "the write reported success but recorded errors: "
        << allMessages(rep);
    EXPECT_TRUE(fs::exists(out))
        << "the write reported success but committed no artifact — the empty "
           "span skipped the rename, not just the `fwrite`";
    EXPECT_EQ(fs::file_size(out), 0u)
        << "a zero-byte request produced a non-empty artifact";
    EXPECT_TRUE(readAll(out).empty());

    // The staging temp was renamed away, not abandoned: an empty artifact
    // takes the same commit path as any other, and the skipped `fwrite` must
    // not have short-circuited the cleanup.
    for (auto const& entry : fs::directory_iterator(scratch.path())) {
        EXPECT_EQ(entry.path().filename().string().find(".dsstmp-"),
                  std::string::npos)
            << "a staging temp was left behind: " << entry.path().string();
    }
}

// ── Integration: `writeBytes` steps over stale temps and never adopts one ───
//
// ★ THIS TEST'S THREE STEPS ARE ORDERED ON PURPOSE, and it derives the exact
// staging-temp names `writeBytes` will try from the shared `nextSlot` ledger
// (see its comment above — the writer's counter is process-wide and takes one
// value per claim ATTEMPT, so each step's slot numbers are arithmetic, not a
// guess). This is no longer the only `writeBytes` caller in the binary: the
// empty-span test above is one too, and under `--gtest_shuffle` it may run
// either side of this one. That is safe BECAUSE every caller declares its
// consumption to the ledger, and because step 2 below re-audits the running
// total on every execution — an off-by-one in either direction turns step 2
// RED rather than quietly vacating step 3.
//
// ★ HOW THE ANTI-VACUITY PROOF WORKS — i.e. how we know the names we create
// really are the writer's candidates, and not just files that survive because
// they were never considered. Step 2 occupies EXACTLY `kMaxClaimAttempts`
// consecutive slots starting at the derived counter value and requires
// `writeBytes` to FAIL by exhaustion. That outcome is reachable only if all
// three of these hold: the suffix scheme is `.dsstmp-<pid>-<n>`, the seed is
// this process's pid, and the counter advances by one per attempt from the
// derived value. Any drift in ANY of them frees a slot, the writer claims it,
// the write SUCCEEDS, and step 2 goes RED. So step 2 is a positive statement
// about candidacy — not an absence of evidence — and step 3 inherits it.
TEST(LinkWriterExclusiveClaim, WriteBytesStepsOverStaleTempsAndNeverAdoptsOne) {
    // The claim cap, READ FROM THE IMPLEMENTATION — `writer.hpp` publishes
    // `linker::detail::kMaxClaimAttempts` through its internal seam as the
    // single source of truth for exactly this reason. NEVER hand-copy it.
    //
    // WHY, precisely: step 2 below needs EXACTLY this many consecutive
    // occupied slots to force the exhaustion arm, and a stale copy here would
    // not fail — it would go VACUOUS. Suppose the literal 1000 were copied and
    // the real cap were later LOWERED to 500. Step 2 over-occupies, the writer
    // still exhausts at 500, and step 2 still PASSES. But it then advances
    // `nextSlot` by 1000 while the writer advanced its own counter by only
    // 500, so every decoy step 3 creates thereafter sits on a name the writer
    // will never try. Step 3's assertions would all still pass — for the wrong
    // reason, silently, forever. Reading the constant makes that drift
    // impossible to express. Widened to 64-bit for the slot arithmetic below.
    constexpr auto kMaxClaimAttempts =
        static_cast<std::uint64_t>(linker::detail::kMaxClaimAttempts);
    // How many stale temps step 3 puts in the writer's way. Small — the point
    // there is that the retry LOOP works, not how far it can run.
    constexpr std::uint64_t kStaleAhead = 16;

    auto const payload = bytesOf("the artifact that must still land");

    // `nextSlot` — the counter value `writeBytes` will draw NEXT — is the
    // file-scope ledger declared above, NOT a local. It must survive both
    // across `--gtest_repeat` iterations (the writer's counter does) and
    // across sibling tests that also call `writeBytes`.

    // ── Step 1: control. No decoys anywhere, so a plain write MUST succeed.
    // Without this, step 2's failure could be caused by anything at all.
    {
        ScratchDir scratch{Location::Temp, "link-writer-stale-control"};
        auto const out = scratch.path() / "artifact.bin";

        DiagnosticReporter rep;
        ASSERT_TRUE(linker::writeBytes(payload, out, rep))
            << "a write into an EMPTY directory failed; nothing below can be "
               "attributed to the stale temps: " << allMessages(rep);
        ASSERT_EQ(rep.errorCount(), 0u);
        ASSERT_EQ(readAll(out), payload);
        nextSlot += 1;  // one attempt, claimed on the first try
    }

    // ── Step 2: occupy EXACTLY every slot the writer is allowed to try. It
    // must give up and say so — the positive proof that these names ARE its
    // candidates (see the header comment above).
    {
        ScratchDir scratch{Location::Temp, "link-writer-stale-exhaust"};
        auto const out = scratch.path() / "artifact.bin";

        std::vector<fs::path> decoys;
        decoys.reserve(kMaxClaimAttempts);
        for (std::uint64_t i = 0; i < kMaxClaimAttempts; ++i) {
            auto const p = stagingCandidate(out, nextSlot + i);
            putFile(p, "stale-" + std::to_string(nextSlot + i));
            decoys.push_back(p);
        }

        DiagnosticReporter rep;
        EXPECT_FALSE(linker::writeBytes(payload, out, rep))
            << "every one of the writer's " << kMaxClaimAttempts
            << " permitted staging slots was already occupied, yet the write "
               "reported SUCCESS. Either the claim is no longer exclusive (it "
               "adopted one of them), or the candidate naming scheme in "
               "writer.cpp no longer matches "
               "`<target>.dsstmp-<pid>-<counter>` — in which case the pins in "
               "step 3 below are vacuous and this file must be updated, not "
               "the assertion relaxed.";
        EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_ImageWriteOpenFailed))
            << "the exhausted claim must fail LOUD";
        // K_ImageWriteOpenFailed covers four distinct arms; pin that it was
        // the EXHAUSTION arm, so a parent-directory or permission failure
        // could not be mistaken for the branch under test.
        EXPECT_NE(allMessages(rep).find("could not claim a unique staging temp"),
                  std::string::npos)
            << "expected the claim-exhaustion diagnostic, got: "
            << allMessages(rep);

        EXPECT_FALSE(fs::exists(out))
            << "no artifact may appear when the write failed";
        // Not one decoy was adopted, truncated, or cleaned up on the way out.
        for (std::uint64_t i = 0; i < kMaxClaimAttempts; ++i) {
            auto const& p = decoys[static_cast<std::size_t>(i)];
            ASSERT_TRUE(fs::exists(p))
                << "stale temp #" << (nextSlot + i)
                << " disappeared — the writer removed a temp it did not create";
            ASSERT_EQ(readAll(p), bytesOf("stale-" + std::to_string(nextSlot + i)))
                << "stale temp #" << (nextSlot + i)
                << " was rewritten — the writer adopted a slot it did not "
                   "create";
        }
        nextSlot += kMaxClaimAttempts;  // one counter value burned per attempt
    }

    // ── Step 3: the retry branch as a SUCCESS path. A handful of stale temps
    // sit on the writer's first candidates; it must step over each of them and
    // still commit. This is the `exists()`-continue arm of the claim loop,
    // which nothing else in the suite reaches.
    //
    // RED ON DISABLE: with a non-exclusive claim the writer OPENS the very
    // first decoy (truncating it), writes the artifact into it, and renames it
    // over the target — so `staleFirst` VANISHES and the decoy count drops.
    // Both assertions below fire. (Under a NAMING drift these decoys would not
    // be candidates and would survive vacuously — which is precisely what step
    // 2 exists to rule out.)
    {
        ScratchDir scratch{Location::Temp, "link-writer-stale-stepover"};
        auto const out        = scratch.path() / "artifact.bin";
        auto const staleFirst = stagingCandidate(out, nextSlot);

        std::vector<fs::path> decoys;
        decoys.reserve(kStaleAhead);
        for (std::uint64_t i = 0; i < kStaleAhead; ++i) {
            auto const p = stagingCandidate(out, nextSlot + i);
            putFile(p, "stale-" + std::to_string(nextSlot + i));
            decoys.push_back(p);
        }

        DiagnosticReporter rep;
        ASSERT_TRUE(linker::writeBytes(payload, out, rep))
            << "the writer could not get past " << kStaleAhead
            << " stale staging temps: " << allMessages(rep);
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(readAll(out), payload)
            << "the artifact does not hold the requested bytes";

        // The FIRST candidate — the one a non-exclusive claim would have
        // adopted — is still there, byte for byte.
        EXPECT_TRUE(fs::exists(staleFirst))
            << "the writer's FIRST candidate slot '" << staleFirst.string()
            << "' is gone: it adopted the stale temp, wrote into it and "
               "renamed it over the target instead of stepping over it";
        EXPECT_EQ(readAll(staleFirst), bytesOf("stale-" + std::to_string(nextSlot)));

        for (std::uint64_t i = 0; i < kStaleAhead; ++i) {
            auto const& p = decoys[static_cast<std::size_t>(i)];
            EXPECT_TRUE(fs::exists(p)) << "stale temp #" << (nextSlot + i)
                                       << " disappeared";
            EXPECT_EQ(readAll(p), bytesOf("stale-" + std::to_string(nextSlot + i)))
                << "stale temp #" << (nextSlot + i) << " was rewritten";
        }
        // ...and the writer's own temp was renamed away, so exactly the
        // decoys remain.
        std::uint64_t remaining = 0;
        for (auto const& entry : fs::directory_iterator(scratch.path())) {
            if (entry.path().filename().string().find(".dsstmp-")
                != std::string::npos) {
                ++remaining;
            }
        }
        EXPECT_EQ(remaining, kStaleAhead)
            << "expected exactly the " << kStaleAhead
            << " pre-created stale temps to remain; a different count means "
               "the writer either consumed one of them or leaked its own";

        // `kStaleAhead` slots stepped over plus the one actually claimed. This
        // line is not bookkeeping hygiene — it is what keeps the derivation
        // true on the NEXT `--gtest_repeat` iteration. MEASURED when it was
        // missing: iteration 2's step 2 under-shot by 17 slots, the writer's
        // real counter walked off the end of the decoy window, and it claimed a
        // free slot instead of exhausting.
        nextSlot += kStaleAhead + 1;
    }
}

// ── A DANGLING SYMLINK AT A CANDIDATE NAME IS AN OCCUPIED SLOT ─────────────
//    D-LINK-WRITER-DANGLING-SYMLINK-CLAIM-MISROUTE
//
// The claim loop disambiguates a null return from `createExclusiveBinary` by
// asking the filesystem whether the candidate is occupied, and it must get the
// SAME answer the exclusive create itself acted on. `O_CREAT|O_EXCL` refuses a
// directory ENTRY; `std::filesystem::exists` resolves the entry and answers
// about its TARGET. A DANGLING symlink is exactly the shape on which those two
// questions have different answers, and before the repair the loop took the
// "this is a real error" arm and refused to emit the artifact at all.
//
// ★ EVERY LEG, AND THE LINK IS THE OS'S. ✔MEASURED 2026-08-26 on the MinGW-W64
// UCRT g++ 13.2.0 build: `std::filesystem::create_symlink` fails with `ENOSYS` —
// "Function not implemented" (40) — in a directory where `mklink` succeeds in the
// same session: libstdc++ on Windows never asks the OS, and no privilege changes
// that. These cases used to SKIP there, saying so; the link is now made by the
// OS's own call (`tests/test_support/os_link.hpp`), so they run on that build too
// — the build whose library also cannot SEE a link (✔MEASURED 2026-10-20: its
// `symlink_status` and `status` both read a dangling file link's OWN attributes,
// `regular`), which is exactly where the claim loop's probe most needs measuring.
// What a planted entry IS is asked of the substrate (`dss::core::entryKind`,
// `reachedKind`), which asks the OS there; never of the library.
// ⚠ A PLANT THAT FAILS FAILS BEFORE CONSUMING A SLOT: the ledger above is
// audited by the integration test, so a case that had already called
// `writeBytes` would desynchronise it.
namespace {

// Plant a dangling symlink at `p`. Returns false with `why` when the OS refused.
[[nodiscard]] bool plantDanglingSymlink(fs::path const& p, std::string& why) {
    std::error_code const ec = dss::test_support::makeSymbolicLink(
        p.parent_path() / "no-such-target-dss", p, dss::test_support::LinkTo::File);
    if (ec) {
        why = "the OS refused to make a symbolic link: " + ec.message() + " ("
            + std::to_string(ec.value())
            + "). This leg cannot construct the input this test is about, so "
              "nothing was asserted, which is no pass.";
        return false;
    }
    return true;
}

[[nodiscard]] bool isDanglingLink(fs::path const& p) {
    std::error_code kec;
    std::error_code rec;
    return dss::core::entryKind(p, dss::core::Use::Leaf, kec) == dss::core::EntryKind::Link && !kec
        && dss::core::reachedKind(p, dss::core::Use::Leaf, rec) == dss::core::EntryKind::Absent && !rec;
}

} // namespace

// (a) THE PRIMITIVE-LEVEL MEASUREMENT — the anti-vacuity proof for (b).
//
// (b) below asserts that the writer steps over a dangling symlink. That claim
// is only meaningful if the claim primitive actually REFUSES such a name: on a
// platform where the exclusive create simply succeeded through the link, (b)
// would pass while proving nothing at all. So the divergence itself is pinned
// here, on the same primitive and in the same process.
TEST(LinkWriterExclusiveClaim, ADanglingSymlinkRefusesTheClaimButExistsSaysNo) {
    ScratchDir scratch{Location::Temp, "link-writer-dangling-primitive"};
    auto const p = scratch.path() / "candidate.dsstmp-probe";

    std::string why;
    ASSERT_TRUE(plantDanglingSymlink(p, why)) << why;
    ASSERT_TRUE(isDanglingLink(p)) << "the OS does not record the planted entry as a dangling link";

    // The claim refuses it — the entry exists, so exclusive create cannot win.
    EXPECT_FALSE(static_cast<bool>(claim(p)))
        << "the exclusive create SUCCEEDED on a name occupied by a dangling "
           "symlink. If that is genuinely this platform's behaviour, the "
           "disambiguation below is moot and this file must be re-measured "
           "rather than the assertion relaxed.";

    // The probe the claim loop relies on sees the ENTRY, on every build.
    std::error_code ecEntry;
    EXPECT_TRUE(fs::exists(fs::symlink_status(p, ecEntry)))
        << "`symlink_status()` must see the ENTRY — this is the probe the "
           "claim loop relies on";
    EXPECT_FALSE(static_cast<bool>(ecEntry))
        << "symlink_status reported an error: " << ecEntry.message();

    // ...and where this build's library can tell a link at all, `exists()`
    // FOLLOWS it and reports the missing TARGET: the two probes disagree, the
    // defect in one line. A library that cannot tell a link (libstdc++ on
    // Windows: `is_symlink` is never true there) reads the link's own entry for
    // both, so on that build the misroute cannot happen and (b) pins the plain
    // outcome; asked by what the library ANSWERS, never by its name.
    std::error_code ecLink;
    if (fs::is_symlink(fs::symlink_status(p, ecLink))) {
        std::error_code ecFollow;
        EXPECT_FALSE(fs::exists(p, ecFollow))
            << "`exists()` was expected to FOLLOW the link and report the missing "
               "TARGET; it did not, so this platform cannot exhibit the misroute";
    } else {
        RecordProperty("library_sees_links", "no: exists() and symlink_status() both read the entry");
    }
}

// (b) THE ROUTING ITSELF, through `writeBytes`.
//
// RED ON DISABLE: restore `std::filesystem::exists(candidate, eec)` in
// `writer.cpp`'s claim loop in place of the `symlink_status` probe. The first
// candidate's dangling link then reads back as "not occupied", the loop takes
// the real-error arm, and `writeBytes` returns FALSE with
// `K_ImageWriteOpenFailed` — the ASSERT_TRUE below fires and no artifact
// appears.
TEST(LinkWriterExclusiveClaim, WriteBytesStepsOverDanglingSymlinkCandidates) {
    // Small — the point is that the ENTRY-level probe routes to the retry arm,
    // not how far the retry can run (the integration test above owns that).
    constexpr std::uint64_t kDanglingAhead = 3;

    ScratchDir scratch{Location::Temp, "link-writer-dangling-stepover"};
    auto const out     = scratch.path() / "artifact.bin";
    auto const payload = bytesOf("the artifact that must still land");

    // Plant BEFORE any `writeBytes` call, so a host whose OS refuses a link
    // fails without touching the staging-slot ledger.
    std::vector<fs::path> links;
    links.reserve(kDanglingAhead);
    for (std::uint64_t i = 0; i < kDanglingAhead; ++i) {
        auto const p = stagingCandidate(out, nextSlot + i);
        std::string why;
        if (!plantDanglingSymlink(p, why)) {
            for (auto const& done : links) { fs::remove(done); }
            FAIL() << why;
        }
        links.push_back(p);
    }

    DiagnosticReporter rep;
    ASSERT_TRUE(linker::writeBytes(payload, out, rep))
        << "a dangling symlink sitting on the writer's first "
        << kDanglingAhead
        << " candidate name(s) stopped the artifact from being written. The "
           "claim loop asked `exists()` — which follows the link to a target "
           "that is not there — instead of asking about the directory ENTRY "
           "the exclusive create actually refused "
           "(D-LINK-WRITER-DANGLING-SYMLINK-CLAIM-MISROUTE): "
        << allMessages(rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readAll(out), payload)
        << "the artifact does not hold the requested bytes";

    // Every planted link is untouched: still a symlink, still dangling. A
    // writer that "resolved" the situation by unlinking or by writing through
    // would pass the assertion above and fail here.
    for (std::uint64_t i = 0; i < kDanglingAhead; ++i) {
        auto const& p = links[static_cast<std::size_t>(i)];
        std::error_code kec;
        EXPECT_EQ(dss::core::entryKind(p, dss::core::Use::Leaf, kec), dss::core::EntryKind::Link)
            << "candidate #" << (nextSlot + i)
            << " is no longer a symlink — the writer consumed a slot it did "
               "not create";
        std::error_code rec;
        EXPECT_EQ(dss::core::reachedKind(p, dss::core::Use::Leaf, rec), dss::core::EntryKind::Absent)
            << "candidate #" << (nextSlot + i)
            << " now resolves — the writer wrote THROUGH the link and created "
               "its target, which is the other way this misroute can go wrong";
    }

    // `kDanglingAhead` slots stepped over plus the one actually claimed.
    nextSlot += kDanglingAhead + 1;
}

// ════ THE OS'S REASON (P69 H6c2, main's ruling on the staging claim's guess list) ════════════════════════════════════
// A host primitive that fails ANSWERS the OS's reason, read at the failing call, and every diagnostic built on it NAMES
// that reason. ★ THE ORACLE IS THE HOST'S OWN CALL on the same spelling -- CreateFileW / GetFileAttributesW and
// GetLastError, POSIX `open` and errno -- and its text the system's own (`dss::test_support::systemReasonText`, the
// test side's one oracle: FormatMessageW spelt as UTF-8 by its own conversion; `std::strerror` on POSIX), never the
// home's answer.

namespace {

using dss::test_support::systemReasonText;

#ifdef _WIN32
// What the host answers to CREATE_NEW of `spelling` (the claim's own disposition): 0 when it made the file (removed).
[[nodiscard]] DWORD hostCreateRefusal(std::wstring const& spelling) {
    HANDLE const h = ::CreateFileW(spelling.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ::GetLastError();
    ::CloseHandle(h);
    ::DeleteFileW(spelling.c_str());
    return 0;
}
#else
[[nodiscard]] int hostCreateRefusal(std::string const& spelling) {
    int const fd = ::open(spelling.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return errno;
    ::close(fd);
    ::unlink(spelling.c_str());
    return 0;
}
#endif

// The LITERAL spelling of an absolute scratch path: what the home hands the host for these plain names (its leaf
// reference), so the oracle asks the host the same question (`\\?\` on Windows: a long last name reaches the file
// system there, where a plain spelling past MAX_PATH would be refused by the path parser first).
[[nodiscard]] fs::path::string_type literalOf(fs::path const& p) {
#ifdef _WIN32
    return L"\\\\?\\" + p.native();
#else
    return p.native();
#endif
}

[[nodiscard]] std::error_code hostCode(long code) {
#ifdef _WIN32
    return std::error_code{static_cast<int>(code), std::system_category()};
#else
    return std::error_code{static_cast<int>(code), std::generic_category()};
#endif
}

// The names a claim is refused for on every host, each with the host's own refusal: a name below a FILE, and a last
// name longer than any file system holds (300 units; NAME_MAX is 255 on every POSIX file system of the legs).
struct RefusedName {
    char const* what;
    fs::path    path;
};
[[nodiscard]] std::vector<RefusedName> refusedNames(fs::path const& scratch) {
    {
        std::ofstream file(scratch / "a-file", std::ios::binary);
        file << "x";
    }
    return {{"a name below a file", scratch / "a-file" / "x.o"},
            {"a 300-unit last name", scratch / std::string(300, 'n')}};
}

}  // namespace

// A REFUSED CLAIM ANSWERS THE HOST'S OWN REASON FOR THE NAME -- the code the host's own create answers, its text the
// system's -- on every leg (main's PINS: Windows not-found / invalid-name, Linux ENOTDIR, macOS too).
TEST(LinkWriterExclusiveClaim, ARefusedClaimAnswersTheHostsOwnReasonForTheName) {
    ScratchDir const scratch{Location::Temp, "link-writer-claim-reason"};
    for (RefusedName const& name : refusedNames(scratch.path())) {
        SCOPED_TRACE(name.what);
        auto const oracle = hostCreateRefusal(literalOf(name.path));
        ASSERT_NE(oracle, 0) << "the host made " << name.what << ": the refusal cannot be asked here";
        auto const claimed = linker::detail::createExclusiveBinary(core::HostPath{name.path});
        ASSERT_FALSE(claimed.has_value()) << "the claim made " << name.what;
        EXPECT_EQ(claimed.error(), hostCode(oracle)) << core::reasonText(claimed.error());
        EXPECT_EQ(core::reasonText(claimed.error()), systemReasonText(oracle));
    }
}

// EVERY HANDLE CALL OF THE HOME ANSWERS THE HOST'S OWN REASON, read at the call (the convention primitives, main's (b)):
// an absent name and a name below a file, against the host's own call of the same spelling.
TEST(LinkWriterExclusiveClaim, EveryHandleCallOfTheHomeAnswersTheHostsOwnReason) {
    ScratchDir const scratch{Location::Temp, "link-writer-handle-reason"};
    std::vector<RefusedName> names = refusedNames(scratch.path());
    names.push_back({"an absent name", scratch.path() / "absent.o"});
    for (RefusedName const& name : names) {
        SCOPED_TRACE(name.what);
#ifdef _WIN32
        HANDLE const plain = ::CreateFileW(literalOf(name.path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        DWORD const  oracle = plain == INVALID_HANDLE_VALUE ? ::GetLastError() : 0;
        if (plain != INVALID_HANDLE_VALUE) ::CloseHandle(plain);
        ASSERT_NE(oracle, 0u) << "the host opened " << name.what;
        auto const opened = core::openHandle(core::HostPath{name.path}, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS);
        ASSERT_FALSE(opened.has_value());
        EXPECT_EQ(opened.error(), hostCode(oracle)) << core::reasonText(opened.error());
        EXPECT_EQ(core::reasonText(opened.error()), systemReasonText(oracle));   // the code AND its text
        DWORD const attributesOracle =
            ::GetFileAttributesW(literalOf(name.path).c_str()) == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : 0;
        ASSERT_NE(attributesOracle, 0u);
        auto const attributes = core::fileAttributes(core::HostPath{name.path});
        ASSERT_FALSE(attributes.has_value());
        EXPECT_EQ(attributes.error(), hostCode(attributesOracle)) << core::reasonText(attributes.error());
        EXPECT_EQ(core::reasonText(attributes.error()), systemReasonText(attributesOracle));
#else
        int const plain  = ::open(name.path.c_str(), O_RDONLY);
        int const oracle = plain < 0 ? errno : 0;
        if (plain >= 0) ::close(plain);
        ASSERT_NE(oracle, 0) << "the host opened " << name.what;
        auto const opened = core::openDescriptor(core::HostPath{name.path}, O_RDONLY, 0);
        ASSERT_FALSE(opened.has_value());
        EXPECT_EQ(opened.error(), hostCode(oracle)) << core::reasonText(opened.error());
        EXPECT_EQ(core::reasonText(opened.error()), systemReasonText(oracle));   // the code AND its text
#endif
    }
}

// EVERY STREAM OPEN OF THE HOME ANSWERS THE HOST'S OWN REASON (H6c2 (d), main's ruling 677): `openInputStream`,
// `openOutputStream` and `openFileStream` ("rb" / "wb"), each against the host's own open of the SAME spelling with the
// same disposition (CreateFileW OPEN_EXISTING / CREATE_ALWAYS and GetLastError; POSIX `open` O_RDONLY /
// O_WRONLY|O_CREAT|O_TRUNC and errno) -- where the host refuses, the home answers its code AND its text; where the host
// opens, the home opens. The names: an absent name, a name below a file, a 300-unit last name, and on POSIX a last name
// that is no UTF-8 (APFS refuses it, EILSEQ; ext4 takes it). A refusal the C runtime reports by `_doserrno` / errno
// is the home's to read AT the failing open (`runtimeReason`), never a stream left to be checked.
TEST(LinkWriterExclusiveClaim, EveryStreamOpenOfTheHomeAnswersTheHostsOwnReason) {
    ScratchDir const         scratch{Location::Temp, "link-writer-stream-reason"};
    std::vector<RefusedName> names = refusedNames(scratch.path());
    names.push_back({"an absent name", scratch.path() / "absent.o"});
#ifndef _WIN32
    names.push_back({"a last name that is no UTF-8", scratch.path() / std::string{"out\xFF.o"}});
#endif
    for (RefusedName const& name : names) {
        for (bool const writing : {false, true}) {
            SCOPED_TRACE(std::string{name.what} + (writing ? ", opened to write" : ", opened to read"));
#ifdef _WIN32
            HANDLE const plain = ::CreateFileW(literalOf(name.path).c_str(), writing ? GENERIC_WRITE : GENERIC_READ,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                               writing ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD const  oracle = plain == INVALID_HANDLE_VALUE ? ::GetLastError() : 0;
            if (plain != INVALID_HANDLE_VALUE) ::CloseHandle(plain);
#else
            int const plain  = writing ? ::open(name.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666)
                                       : ::open(name.path.c_str(), O_RDONLY);
            int const oracle = plain < 0 ? errno : 0;
            if (plain >= 0) ::close(plain);
#endif
            auto const judge = [&](char const* family, std::optional<std::error_code> const& refused) {
                SCOPED_TRACE(family);
                if (oracle == 0) {
                    EXPECT_FALSE(refused.has_value())
                        << "the host opened it and the home refused: " << core::reasonText(refused.value_or(std::error_code{}));
                    return;
                }
                ASSERT_TRUE(refused.has_value()) << "the host refused it and the home opened it";
                EXPECT_EQ(*refused, hostCode(oracle)) << core::reasonText(*refused);
                EXPECT_EQ(core::reasonText(*refused), systemReasonText(oracle));   // the code AND its text
            };
            core::HostPath const value{name.path};
            if (writing) {
                auto stream = core::openOutputStream(value, std::ios::binary | std::ios::trunc);
                judge("openOutputStream", stream ? std::nullopt : std::optional<std::error_code>{stream.error()});
                if (stream) stream->close();
                auto const file = core::openFileStream(value, "wb");
                judge("openFileStream wb", file ? std::nullopt : std::optional<std::error_code>{file.error()});
                if (file) std::fclose(*file);
            } else {
                auto const stream = core::openInputStream(value, std::ios::binary);
                judge("openInputStream", stream ? std::nullopt : std::optional<std::error_code>{stream.error()});
                auto const file = core::openFileStream(value, "rb");
                judge("openFileStream rb", file ? std::nullopt : std::optional<std::error_code>{file.error()});
                if (file) std::fclose(*file);
            }
        }
    }
    // A value that names nothing (the empty spelling as a leaf) answers the home's OWN refusal in each family, as every
    // call of the home does -- until H6c2 `openFileStream` set errno itself, a reason no host gave.
    core::HostPath const nothing{fs::path{}};
    auto const           noInput = core::openInputStream(nothing, std::ios::binary);
    auto const           noOutput = core::openOutputStream(nothing, std::ios::binary);
    auto const           noFile = core::openFileStream(nothing, "rb");
    ASSERT_FALSE(noInput.has_value());
    ASSERT_FALSE(noOutput.has_value());
    ASSERT_FALSE(noFile.has_value());
    EXPECT_EQ(noInput.error(), std::make_error_code(std::errc::invalid_argument)) << core::reasonText(noInput.error());
    EXPECT_EQ(noOutput.error(), std::make_error_code(std::errc::invalid_argument)) << core::reasonText(noOutput.error());
    EXPECT_EQ(noFile.error(), std::make_error_code(std::errc::invalid_argument)) << core::reasonText(noFile.error());
}

// A VALUE THAT NAMES NOTHING IS REFUSED BY THE HOME'S OWN REASON, never by a code it made up (until H6c2 the handle
// calls set ERROR_INVALID_PARAMETER / EINVAL for it): an ill-formed GIVEN on Windows, the empty spelling as a leaf
// everywhere.
TEST(LinkWriterExclusiveClaim, AValueThatNamesNothingIsRefusedByTheHomesOwnReason) {
    auto const empty = linker::detail::createExclusiveBinary(core::HostPath{fs::path{}});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error(), std::make_error_code(std::errc::invalid_argument)) << core::reasonText(empty.error());
    // The code AND its text: an errno's is the C runtime's own on every host.
    EXPECT_EQ(core::reasonText(empty.error()), "errno=" + std::to_string(EINVAL) + ": " + std::strerror(EINVAL));
#ifdef _WIN32
    auto const illFormed = linker::detail::createExclusiveBinary(core::HostPath{std::string{"bad\xFF.o"}});
    ASSERT_FALSE(illFormed.has_value());
    EXPECT_EQ(illFormed.error(), core::pathRefusalCode(core::PathRefusal::IllFormedSpelling))
        << core::reasonText(illFormed.error());
    EXPECT_NE(core::reasonText(illFormed.error()).find(core::pathRefusalName(core::PathRefusal::IllFormedSpelling)),
              std::string::npos);
#endif
}

// A WRITE REFUSED BY ITS DIRECTORY NAMES THE CLAIM'S REASON, NOT THE PROBE'S (main's (b): the reason survives the call
// that follows the refusal). The directory denies adding a file, so the claim is refused (ERROR_ACCESS_DENIED / EACCES),
// and the writer's entry probe that follows answers "nothing there" (ERROR_FILE_NOT_FOUND / ENOENT) -- the reason a
// diagnostic reading the thread's last error or errno AFTER the probe would name.
namespace {
struct DeniedDirectory {
    fs::path dir;
    bool     denied = false;
#ifdef _WIN32
    PACL                 oldDacl    = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    explicit DeniedDirectory(fs::path d) : dir(std::move(d)) {
        fs::create_directories(dir);
        if (::GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl,
                                    nullptr, &descriptor)
            != ERROR_SUCCESS)
            return;
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return;
        std::vector<std::uint8_t> user(512);
        DWORD                     got = 0;
        bool const known = ::GetTokenInformation(token, TokenUser, user.data(), static_cast<DWORD>(user.size()), &got);
        ::CloseHandle(token);
        if (!known) return;
        EXPLICIT_ACCESS_W deny{};
        deny.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
        deny.grfAccessMode        = DENY_ACCESS;
        deny.grfInheritance       = NO_INHERITANCE;
        deny.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType  = TRUSTEE_IS_USER;
        deny.Trustee.ptstrName    = static_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
        PACL newDacl = nullptr;
        if (::SetEntriesInAclW(1, &deny, oldDacl, &newDacl) != ERROR_SUCCESS) return;
        denied = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, newDacl, nullptr)
              == ERROR_SUCCESS;
        ::LocalFree(newDacl);
    }
    ~DeniedDirectory() {
        if (denied)
            static_cast<void>(::SetNamedSecurityInfoW(const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
                                                      DACL_SECURITY_INFORMATION, nullptr, nullptr, oldDacl, nullptr));
        if (descriptor != nullptr) ::LocalFree(descriptor);
    }
#else
    explicit DeniedDirectory(fs::path d) : dir(std::move(d)) {
        fs::create_directories(dir);
        denied = ::chmod(dir.c_str(), 0500) == 0;
    }
    ~DeniedDirectory() {
        if (denied) static_cast<void>(::chmod(dir.c_str(), 0700));
    }
#endif
    DeniedDirectory(DeniedDirectory const&)            = delete;
    DeniedDirectory& operator=(DeniedDirectory const&) = delete;
};
}  // namespace

TEST(LinkWriterExclusiveClaim, AWriteRefusedByItsDirectoryNamesTheClaimsReasonNotTheProbes) {
    ScratchDir const      scratch{Location::Temp, "link-writer-denied-dir"};
    DeniedDirectory const denied{scratch.path() / "denied"};
    ASSERT_TRUE(denied.denied) << "the test could not make its own directory refuse a new file";
    auto const oracle = hostCreateRefusal(literalOf(denied.dir / "probe.o"));
    if (oracle == 0) GTEST_SKIP() << "this process may add a file to a directory that denies it (a superuser): the refusal cannot arise here";
    fs::path const      target = denied.dir / "out.o";
    std::uint8_t const  byte   = 0x01;
    DiagnosticReporter  rep;
    bool                wrote  = true;
    ASSERT_NO_THROW(wrote = linker::writeBytes(std::span<std::uint8_t const>{&byte, 1}, target, rep));
    EXPECT_FALSE(wrote);
    std::string said;
    bool        named = false;
    for (auto const& d : rep.all()) {
        said += "\n  " + d.actual;
        if (d.code == DiagnosticCode::K_ImageWriteOpenFailed && d.actual.find(systemReasonText(oracle)) != std::string::npos)
            named = true;
    }
    EXPECT_TRUE(named) << "no diagnostic names the claim's own reason `" << systemReasonText(oracle) << "`:" << said;
#ifdef _WIN32
    std::string const probes = "GetLastError=" + std::to_string(ERROR_FILE_NOT_FOUND) + ":";
#else
    std::string const probes = "errno=" + std::to_string(ENOENT) + ":";
#endif
    EXPECT_EQ(said.find(probes), std::string::npos) << "a diagnostic names the entry probe's reason:" << said;
}

// THE REASON'S TEXT IS THE SYSTEM'S OWN, by number first (`core::reasonText`): a Win32 code's message as UTF-8, an
// errno's strerror text, and the home's own refusal by its name -- never "no reason" for a code the host gave.
TEST(LinkWriterExclusiveClaim, TheReasonsTextIsTheSystemsOwnByNumberFirst) {
#ifdef _WIN32
    for (DWORD const code : {DWORD{ERROR_FILE_NOT_FOUND}, DWORD{ERROR_ACCESS_DENIED}, DWORD{ERROR_INVALID_NAME}})
        EXPECT_EQ(core::reasonText(hostCode(static_cast<long>(code))), systemReasonText(code));
#else
    for (int const code : {ENOENT, EACCES, ENOTDIR, ENAMETOOLONG})
        EXPECT_EQ(core::reasonText(hostCode(code)), systemReasonText(code));
#endif
    // EVERY REFUSAL'S CODE IS AN ERROR: a refusal valued 0 would read as success to every `if (ec)` (the enumeration
    // starts at 1 for it). Each value from the first to the last.
    for (int r = static_cast<int>(core::PathRefusal::NoLastName);
         r <= static_cast<int>(core::PathRefusal::HostAnsweredNoReason); ++r)
        EXPECT_TRUE(static_cast<bool>(core::pathRefusalCode(static_cast<core::PathRefusal>(r))))
            << core::pathRefusalName(static_cast<core::PathRefusal>(r)) << " reads as success";
    EXPECT_EQ(core::reasonText(core::pathRefusalCode(core::PathRefusal::HostAnsweredNoReason)),
              std::string{"dss.path-refusal="} + std::to_string(static_cast<int>(core::PathRefusal::HostAnsweredNoReason))
                  + ": " + core::pathRefusalName(core::PathRefusal::HostAnsweredNoReason));
}
