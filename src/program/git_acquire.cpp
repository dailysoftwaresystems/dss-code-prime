#include "program/git_acquire.hpp"

#include "core/substrate/path_identity.hpp"  // genericSpelling
#include "core/substrate/process_spawn.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace dss {

namespace {

namespace fs = std::filesystem;

// Render a `SpawnResult` into `GitCommandResult`'s two failure-carrying
// fields.
//
// ★ THIS IS WHERE THE SUBSTRATE'S `spawned` / `exitCode` SPLIT IS RENDERED
// RATHER THAN PROPAGATED. `process_spawn.hpp` keeps them apart deliberately
// ("the caller emits two remediation-distinct diagnostics off this boolean"),
// and that distinction is real — but B.4 forbids the CACHE from branching on
// it, because the discriminator between its two failure arms must be "is there
// a usable checkout" and nothing else. So the fact is preserved where it
// belongs, in prose the operator reads, and is unavailable where it would be
// misused. `detail` is complete on its own: it says what was attempted, which
// of the two shapes happened, and quotes the OS's or git's own words.
[[nodiscard]] GitCommandResult
fromSpawn(substrate::SpawnResult const& r, std::string const& what) {
    GitCommandResult out;
    if (!r.spawned) {
        out.detail = what + " could not be spawned: " + r.diagnostic;
        return out;
    }
    if (r.exitCode != 0) {
        out.detail = what + " exited with status " + std::to_string(r.exitCode);
        // Non-empty ONLY where the number is a stand-in rather than a choice —
        // a POSIX child killed by a signal, a wait that failed. Dropping it
        // would tell an operator whose `git` segfaulted that git deliberately
        // returned 139. (Same contract `build_scripts.cpp` reads it under.)
        if (!r.diagnostic.empty()) out.detail += " (" + r.diagnostic + ")";
        return out;
    }
    out.ok = true;
    return out;
}

// A private, exclusively-claimed directory to redirect one `git rev-parse`
// into, and its removal.
//
// WHY A DIRECTORY AND NOT JUST A UNIQUE FILE NAME. `create_directory`
// (singular) returns true only when THIS call created it, and that
// check-and-create is atomic at the OS level — so the claim is race-free
// against a second compiler process, or a second thread, picking the same
// name. A unique-looking FILE name has no such primitive behind it
// (`spawnAndWaitRedirectStdout` opens `CREATE_ALWAYS` / `O_TRUNC`, which
// happily lands on someone else's file). Same reasoning, and the same loop, as
// `tests/test_support/scratch_dir.hpp`'s uniqueness note.
//
// It lives under the system temp directory rather than in `.dss-deps/`
// deliberately: this runner knows nothing about the cache layout, and a
// capture file appearing inside a dependency's checkout would be a byte we
// wrote into a tree we promised only to read.
struct CaptureSlot {
    fs::path dir;   // empty when the slot could not be claimed
    fs::path file;

    ~CaptureSlot() {
        if (dir.empty()) return;
        std::error_code ec;
        fs::remove_all(dir, ec);   // best effort; a leaked temp dir is not a
                                   // build failure and has no user impact
    }
    CaptureSlot()                              = default;
    CaptureSlot(CaptureSlot const&)            = delete;
    CaptureSlot& operator=(CaptureSlot const&) = delete;
    CaptureSlot(CaptureSlot&&)                 = delete;
    CaptureSlot& operator=(CaptureSlot&&)      = delete;
};

void claimCaptureSlot(CaptureSlot& slot) {
    static std::atomic<std::uint64_t> seq{0};

    std::error_code ec;
    fs::path const base = fs::temp_directory_path(ec) / "dss-git-rev-parse";
    if (ec) return;
    fs::create_directories(base, ec);
    if (ec) return;

    auto const stamp = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    for (std::uint32_t attempt = 0; attempt < 64u; ++attempt) {
        fs::path candidate = base / (std::to_string(stamp) + "-"
                                     + std::to_string(seq.fetch_add(1)));
        std::error_code cec;
        if (fs::create_directory(candidate, cec)) {
            slot.dir  = std::move(candidate);
            slot.file = slot.dir / "stdout.txt";
            return;
        }
    }
}

// Trailing (and leading) ASCII whitespace off a captured stdout. `git
// rev-parse` prints the id plus a newline, and on Windows the child's own
// text-mode write makes that "\r\n" — comparing a commit id against the
// lockfile with a stray carriage return attached would miss EVERY hit, on one
// host only.
[[nodiscard]] std::string trimAscii(std::string s) {
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v'
            || c == '\f';
    };
    std::size_t b = 0;
    while (b < s.size() && isSpace(s[b])) ++b;
    std::size_t e = s.size();
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

} // namespace

std::vector<std::string> gitCloneArgv(std::string const&  gitExe,
                                      std::string const&  url,
                                      fs::path const&     dest) {
    return {gitExe, "clone", "--", url, dest.string()};
}

std::vector<std::string> gitFetchArgv(std::string const& gitExe,
                                      std::string const& ref) {
    return {gitExe, "fetch", "--force", "--tags", "origin",
            ref.empty() ? std::string{"HEAD"} : ref};
}

std::vector<std::string> gitCheckoutArgv(std::string const& gitExe,
                                         std::string const& rev) {
    return {gitExe, "checkout", "--detach", "--force", rev};
}

std::vector<std::string> gitRevParseArgv(std::string const& gitExe,
                                         std::string const& rev) {
    return {gitExe, "rev-parse", rev};
}

std::optional<fs::path> SystemGitRunner::resolve_() {
    // Answered once. PATH cannot change under a running build, and the lookup
    // walks every PATH entry with extension probing on Windows — cheap, but not
    // free, and re-answering it per operation would make an N-dependency build
    // do N×4 directory scans for one unchanging fact.
    if (!resolved_) {
        gitExe_   = substrate::resolveExecutableOnPath("git");
        resolved_ = true;
    }
    return gitExe_;
}

bool SystemGitRunner::isAvailable() { return resolve_().has_value(); }

GitCommandResult SystemGitRunner::clone(std::string const& url,
                                        fs::path const&    dest) {
    auto const exe = resolve_();
    if (!exe) {
        GitCommandResult out;
        out.detail = "`git` is not on PATH";
        return out;
    }
    // cwd is INHERITED (the empty sentinel) rather than `dest`'s parent: `dest`
    // does not exist yet, and it is passed to git as an absolute path anyway.
    return fromSpawn(
        substrate::spawnAndWaitInherit(gitCloneArgv(exe->string(), url, dest)),
        "git clone of '" + url + "'");
}

GitCommandResult SystemGitRunner::fetch(fs::path const&    checkoutDir,
                                        std::string const& ref) {
    auto const exe = resolve_();
    if (!exe) {
        GitCommandResult out;
        out.detail = "`git` is not on PATH";
        return out;
    }
    return fromSpawn(
        substrate::spawnAndWaitInherit(gitFetchArgv(exe->string(), ref),
                                       checkoutDir),
        "git fetch in '" + core::genericSpelling(checkoutDir) + "'");
}

GitCommandResult SystemGitRunner::checkout(fs::path const&    checkoutDir,
                                           std::string const& rev) {
    auto const exe = resolve_();
    if (!exe) {
        GitCommandResult out;
        out.detail = "`git` is not on PATH";
        return out;
    }
    return fromSpawn(
        substrate::spawnAndWaitInherit(gitCheckoutArgv(exe->string(), rev),
                                       checkoutDir),
        "git checkout of '" + rev + "'");
}

GitCommandResult SystemGitRunner::revParse(fs::path const&    checkoutDir,
                                           std::string const& rev) {
    GitCommandResult out;
    auto const       exe = resolve_();
    if (!exe) {
        out.detail = "`git` is not on PATH";
        return out;
    }

    CaptureSlot slot;
    claimCaptureSlot(slot);
    if (slot.file.empty()) {
        out.detail = "git rev-parse could not claim a capture file under the "
                     "system temporary directory";
        return out;
    }

    // ★ THE ONE REDIRECTED SPAWN IN THE DRIVER. stderr and stdin still inherit
    // — only stdout moves — so a `fatal: not a git repository` still reaches
    // the operator's terminal while the commit id reaches us. See
    // `spawnAndWaitRedirectStdout`'s "★★ A FILE, NEVER AN ANONYMOUS PIPE" note
    // for the measured deadlock a pipe would have re-imported into a facility
    // that has no timeout to escape it with.
    out = fromSpawn(
        substrate::spawnAndWaitRedirectStdout(gitRevParseArgv(exe->string(), rev),
                                              checkoutDir, slot.file),
        "git rev-parse " + rev + " in '" + core::genericSpelling(checkoutDir) + "'");
    if (!out.ok) return out;

    std::ifstream in{slot.file, std::ios::binary};
    if (!in) {
        out.ok     = false;
        out.detail = "git rev-parse succeeded but its captured output at '"
                   + core::genericSpelling(slot.file) + "' could not be read";
        return out;
    }
    out.output = trimAscii(std::string{std::istreambuf_iterator<char>{in},
                                       std::istreambuf_iterator<char>{}});

    // A zero exit with NOTHING printed is not an answer, and accepting it would
    // hand the cache an empty "commit id" that compares unequal to every
    // recorded one — a permanent, silent miss. Fail loud instead.
    if (out.output.empty()) {
        out.ok     = false;
        out.detail = "git rev-parse " + rev + " in '"
                   + core::genericSpelling(checkoutDir)
                   + "' exited 0 but printed nothing, so there is no commit id "
                     "to record";
    }
    return out;
}

} // namespace dss
