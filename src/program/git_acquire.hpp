#pragma once

#include "core/export.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// git_acquire — THE ONE SEAM through which the driver may invoke `git`, and
// the argv it invokes it with.
//
// WHY A SEAM AT ALL, WHICH IS THE ONLY REASON THIS FILE EXISTS. B.4 of
// `.plans/06-artifact-profile-plan` specifies a FOUR-OUTCOME cache machine
// (hit / miss / network-failure-with-checkout / network-failure-without), and
// two of those four arms are network FAILURES. A test driving real `git` can
// reach the first two on a good day and neither of the last two
// deterministically — you cannot ask a working network to fail on cue, and a
// CI leg that tried would be flaky in the direction that reads as green. So
// the acquisition surface is an ABSTRACT INTERFACE with a scripted fake in the
// tests (B.7 layer 1, "the primary coverage and it is not optional"), and the
// state machine over it (`dependency_cache.hpp`) is exercised with ZERO
// network access and ZERO dependency on `git` being installed.
//
// MODELLED ON `dss::substrate::IExecutor` (`core/substrate/thread_pool.hpp`),
// deliberately and to the letter: pure-virtual, non-copyable/non-movable, a
// protected default ctor, and NON-OWNING at the consumer (the cache holds a
// reference, never a `unique_ptr`, exactly as the driver holds its executor).
// That shape is already the repo's answer to "one interface, a production
// implementation and a deterministic test implementation", and inventing a
// second shape for the same job would make two things look different that are
// not.
//
// FIVE OPERATIONS, NO MORE. `clone`, `fetch`, `checkout`, `revParse`,
// `isAvailable`. Every one has a call site in `dependency_cache.cpp` in this
// same change; there is no `ls-remote`, no `status`, no `pull`, no `submodule`
// arm, because B.4's machine calls none of them. `process_spawn.hpp`'s opening
// note records what happened the last time this repo shipped a spawn surface
// with arms nobody called ("an audit ruled that a SPECULATIVE BUILD"), and the
// rule it came away with — add an arm the day a caller needs it — is the rule
// this interface is sized by.
//
// ★ ONLY `revParse` CAPTURES OUTPUT. The other three INHERIT stdio, so a
// clone's progress meter and a fetch's error text land on the operator's own
// terminal, live. Routing them through `spawnAndWaitRedirectStdout` would
// silently swallow git's own explanation of a failure into a file that nothing
// in this codebase ever reads, and the operator would be left with our
// one-line diagnostic instead of git's. `revParse` is the single operation
// whose ANSWER we need in-process (a commit id), which is precisely the caller
// `spawnAndWaitRedirectStdout` was added for — see its "★ WHY IT EXISTS" note.
//
// AGNOSTIC: nothing here names a source language, a target processor or an
// object format. `git` is a host tool, in the same category as the process
// spawner underneath it.

namespace dss {

// The outcome of ONE git invocation.
//
// ★ THREE FIELDS, AND EVERY ONE OF THEM IS READ IN THIS CYCLE. A field with no
// reader is a speculative build (the standing ruling recorded at
// `process_spawn.hpp`), so the struct is sized by the state machine's actual
// questions rather than by what a git result COULD carry:
//   * `ok`     — the only thing `dependency_cache.cpp` branches on;
//   * `output` — `revParse`'s commit id: compared against the lockfile on the
//                hit path and RECORDED into it on the miss path;
//   * `detail` — interpolated into the 0xD01E / 0xD01F prose, so the operator
//                sees what git or the OS actually said.
//
// ⚠ THERE IS DELIBERATELY NO `exitCode`, AND NO `spawned`/`ran` SPLIT. The
// substrate below DOES distinguish them (`SpawnResult::spawned` vs
// `exitCode`), and this layer collapses them ON PURPOSE, because B.4 states
// the discriminator between its two failure arms as "is there a usable
// checkout ... never the git exit status or the operation name (clone vs
// fetch), or an offline build's outcome would depend on whether git happened
// to report the failure the same way". A structured exit code sitting in this
// struct is an invitation to key on exactly that, and the field would have no
// other reader: the human-readable half of both facts is already in `detail`,
// which is where a message wants it. What the substrate discriminates is not
// lost, it is RENDERED — `detail` opens with "could not be spawned" or "exited
// with status N" and carries the OS's own text either way.
struct DSS_EXPORT GitCommandResult {
    // The OS created the process AND it exited zero. False for every other
    // shape: argv[0] unresolvable, working directory absent, child killed by a
    // signal, non-zero exit.
    bool        ok = false;
    // The child's stdout, with trailing newlines/whitespace stripped. Non-empty
    // only for `revParse` — the other three inherit stdout and capture nothing,
    // so reading this on them would be reading a field nobody wrote.
    std::string output;
    // Empty iff `ok`. Human-readable and complete on its own: it names what was
    // attempted and quotes the OS's or git's own words.
    std::string detail;
};

// The acquisition surface. See the header note for why it is an interface and
// why it has exactly these five members.
class DSS_EXPORT IGitRunner {
public:
    virtual ~IGitRunner() noexcept = default;

    IGitRunner(IGitRunner const&)            = delete;
    IGitRunner& operator=(IGitRunner const&) = delete;
    IGitRunner(IGitRunner&&)                 = delete;
    IGitRunner& operator=(IGitRunner&&)      = delete;

    // Is `git` reachable at all? U-3 (§B.8) makes this a HARD gate rather than
    // a capability probe: "if a project declares ANY git dependency and `git`
    // is absent or inaccessible, FAIL LOUD (D_DependencyGitNotFound, once per
    // build). No degraded git-less mode for a project that needs git." A cache
    // HIT also needs it — the hit is validated by `revParse`, because a
    // stale-but-unnoticed checkout is the silent-miscompile direction.
    [[nodiscard]] virtual bool isAvailable() = 0;

    // Create a NEW checkout of `url` at `dest`. `dest` must not exist; the
    // caller stages it (see `dependency_cache.hpp` on why a clone never writes
    // straight to `.dss-deps/<name>`).
    [[nodiscard]] virtual GitCommandResult
    clone(std::string const& url, std::filesystem::path const& dest) = 0;

    // Refresh an EXISTING checkout at `checkoutDir`, bringing `ref` (or the
    // remote's default branch when `ref` is empty) up to date.
    //
    // ★ THE REF IS A PARAMETER, AND THAT IS WHAT MAKES `--force-git-cache`
    // WORK. A bare `git fetch` updates the remote-tracking refs and writes
    // FETCH_HEAD from whatever the current branch's upstream is; on a DETACHED
    // checkout — which is what this cache always produces — that is not
    // necessarily the ref the manifest asked for. Fetching the ref EXPLICITLY
    // makes FETCH_HEAD mean exactly "the tip the manifest named", which is the
    // revision `checkout` is then pointed at.
    [[nodiscard]] virtual GitCommandResult
    fetch(std::filesystem::path const& checkoutDir, std::string const& ref) = 0;

    // Move the checkout's HEAD to `rev` (a ref name, a tag, a commit id, or
    // FETCH_HEAD). DETACHED, always — see `gitCheckoutArgv`.
    [[nodiscard]] virtual GitCommandResult
    checkout(std::filesystem::path const& checkoutDir,
             std::string const&           rev) = 0;

    // Resolve `rev` to a commit id inside `checkoutDir`, returning it in
    // `GitCommandResult::output`. The ONLY operation that captures stdout.
    //
    // It is also the "is this a usable checkout" PROBE. `is_directory` is not
    // that question: a clone interrupted halfway leaves a directory that is not
    // a repository, and treating it as a checkout would route a later failure
    // to 0xD01F (build proceeds on "possibly stale sources") over a tree that
    // has no sources at all. Asking git is the only honest answer, and U-3
    // already requires the call on the hit path, so it costs nothing extra.
    [[nodiscard]] virtual GitCommandResult
    revParse(std::filesystem::path const& checkoutDir,
             std::string const&           rev) = 0;

protected:
    IGitRunner() noexcept = default;
};

// ── THE ARGV, AS PURE FUNCTIONS ──────────────────────────────────────────────
//
// WHY THEY ARE NOT PRIVATE TO THE .cpp. The whole point of the seam above is
// that the state machine is testable without git; the cost of that is that the
// four argv vectors — the part that talks to the real tool — are then reachable
// by NO test at all, and every one of them encodes a decision that is silent
// when wrong (a missing `--`, a `checkout` that does not detach, a `fetch` that
// does not move FETCH_HEAD). Pure functions make them assertable byte-for-byte
// with no process, which is the same trick `buildWindowsCommandLine` uses one
// tier down.
//
// `gitExe` is the RESOLVED path to git (from `resolveExecutableOnPath`), passed
// in rather than looked up here so these stay pure.

// `git clone -- <url> <dest>`.
//
// ★ THE `--` IS LOAD-BEARING. `url` comes from a manifest, i.e. from user data;
// a value beginning with `-` would otherwise be parsed by git as an OPTION.
// This is the same class of hazard the argv-vector script design closes one
// tier down, and it costs one token.
[[nodiscard]] DSS_EXPORT std::vector<std::string>
gitCloneArgv(std::string const& gitExe, std::string const& url,
             std::filesystem::path const& dest);

// `git fetch --force --tags origin <ref-or-HEAD>`.
//
// `--force` because a re-fetch of a rewritten tag or a force-pushed branch must
// update the local ref rather than refuse; this is a CACHE we own, not a user's
// working tree, and refusing here would leave `--force-git-cache` unable to do
// the one thing it exists to do. `--tags` because a manifest `ref` may name a
// tag and the default refspec brings only tags reachable from the fetched
// branches. The literal `HEAD` for an absent ref asks the remote for its own
// default branch, which is exactly what a `{git}`-with-no-`ref` entry means.
[[nodiscard]] DSS_EXPORT std::vector<std::string>
gitFetchArgv(std::string const& gitExe, std::string const& ref);

// `git checkout --detach --force <rev>`.
//
// ★ `--detach`, ALWAYS. Checking out a BRANCH name would create a local branch
// and leave HEAD attached to it — and then the next `--force-git-cache` fetch
// updates the remote-tracking ref while `git checkout <branch>` (already on it)
// moves nothing at all. The flag would cost a network round trip and change
// nothing, which is the exact defect §3 item 2 of the AP6 plan calls out. A
// detached HEAD pointed at FETCH_HEAD moves every time.
// `--force` because the tree is ours: a half-applied earlier checkout must not
// be able to wedge the cache.
[[nodiscard]] DSS_EXPORT std::vector<std::string>
gitCheckoutArgv(std::string const& gitExe, std::string const& rev);

// `git rev-parse <rev>`.
[[nodiscard]] DSS_EXPORT std::vector<std::string>
gitRevParseArgv(std::string const& gitExe, std::string const& rev);

// ── The production implementation ────────────────────────────────────────────
//
// Spawns real `git` through `core/substrate/process_spawn.hpp`. Holds no state
// beyond the resolved executable path, so it is safe to construct once per
// build and hand to the cache by reference.
class DSS_EXPORT SystemGitRunner final : public IGitRunner {
public:
    SystemGitRunner() noexcept = default;

    [[nodiscard]] bool isAvailable() override;
    [[nodiscard]] GitCommandResult
    clone(std::string const& url, std::filesystem::path const& dest) override;
    [[nodiscard]] GitCommandResult
    fetch(std::filesystem::path const& checkoutDir,
          std::string const&           ref) override;
    [[nodiscard]] GitCommandResult
    checkout(std::filesystem::path const& checkoutDir,
             std::string const&           rev) override;
    [[nodiscard]] GitCommandResult
    revParse(std::filesystem::path const& checkoutDir,
             std::string const&           rev) override;

private:
    // The resolved `git` image, or nullopt when the PATH lookup has not run yet
    // or found nothing. Cached because every operation needs it and a PATH scan
    // per invocation would re-answer a question whose answer cannot change
    // mid-build.
    [[nodiscard]] std::optional<std::filesystem::path> resolve_();

    std::optional<std::filesystem::path> gitExe_;
    bool                                 resolved_ = false;
};

} // namespace dss
