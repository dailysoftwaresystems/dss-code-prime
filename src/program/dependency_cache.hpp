#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "program/dependency_lockfile.hpp"
#include "program/git_acquire.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// dependency_cache — the `.dss-deps` state machine: derive a cache name, catch
// the collisions BEFORE touching the network, and answer B.4's four outcomes.
//
// WHERE `.dss-deps/` LIVES, AND IT IS A CORRECTNESS RULE. It is created in the
// CONSUMING project's own directory, beside that project's
// `.dss-project.json` — never inside the compiler's tree, never in a shared or
// user-home location. Two consequences the caller must honour and this class
// enforces by taking the directory as a CONSTRUCTOR ARGUMENT rather than
// deriving it: (1) compiling somebody else's project must not write a byte into
// the compiler's own tree, and (2) two consuming projects naming the same git
// URL each get their own checkout, so the cache is never a cross-project shared
// mutable store whose contents depend on what somebody else built last. There
// is ONE `.dss-deps` per BUILD — at the ROOT consumer's manifest directory, not
// one per node — because a `path` dependency's tree may be read-only.
// The process working directory is never consulted here.
//
// AGNOSTIC: no source language, target processor or object format is named.

namespace dss {

// The lockfile's fixed filename, and the checkout-staging directory, both
// spelled ONCE.
inline constexpr std::string_view kDependencyCacheDirName = ".dss-deps";
inline constexpr std::string_view kDependencyLockfileName = "dss-lock.json";
// Contains a `+`, which is OUTSIDE the derived-name character set below, so no
// URL can ever derive a name that collides with it. That is the whole reason
// for the odd spelling — a plain "staging" would be a legal derived name and a
// repo called `staging` would land on top of the compiler's scratch area.
inline constexpr std::string_view kDependencyStagingDirName = "+staging";

// ── U-5: THE `.dss-deps/<name>` DERIVATION ───────────────────────────────────
//
// It is part of the SPEC, not an implementation detail, because 0xD020 makes
// COLLISIONS BETWEEN DERIVED NAMES a diagnostic and a diagnostic about a
// derived value is meaningless unless the derivation is pinned. The rule:
// LAST NON-EMPTY `/`-separated segment, ONE trailing `.git` stripped,
// CASE-SENSITIVE. `…/org/bar.git` and `…/org/bar/` both give `bar`;
// `…/org/Bar.git` gives `Bar`, a DIFFERENT name and not a collision;
// `…/x.git.git` gives `x.git`, because exactly one suffix is stripped and not
// repeatedly.
//
// ⓘ AN HONEST EDGE, STATED RATHER THAN QUIETLY WIDENED. The rule splits on `/`
// only, so an scp-style URL with no path separator after the host
// (`git@host:bar.git`) derives `git@host:bar` and is REJECTED for its `@` and
// `:`. The common scp spelling (`git@github.com:org/bar.git`) has a slash and
// derives `bar` normally. Teaching the splitter about `:` would be inventing a
// rule the spec does not state, in the one function whose exact behaviour a
// user-visible diagnostic quotes; the remediation the reject already carries
// (re-spell the URL, e.g. `ssh://git@host/bar.git`) resolves it in one edit.
enum class DerivedNameStatus : std::uint8_t {
    Ok,
    // The URL has no non-empty last segment at all (`""`, `"///"`, `".git"`).
    NoSegment,
    // A character outside `[A-Za-z0-9._-]`. The set is a CONSERVATIVE
    // INTERSECTION, not a host capability test: deriving legality from the HOST
    // would make one manifest resolve on one machine and fail on another.
    IllegalCharacter,
    // Every character is legal but the name cannot be used as a directory
    // under `.dss-deps/`: `.` and `..` would resolve OUTSIDE or ON TOP OF the
    // cache root, and `dss-lock.json` is the lockfile's own path. All three are
    // reachable from a real URL (`https://host/..`, `https://host/dss-lock.json`)
    // and all three are the same fact — a single name that is not usable —
    // which is what 0xD024 is for.
    ReservedName,
};

struct DSS_EXPORT DerivedCacheName {
    DerivedNameStatus status = DerivedNameStatus::NoSegment;
    // The derived segment. Filled in on EVERY status except `NoSegment`,
    // including the two rejects — 0xD024's allocation requires the message to
    // show the derived name, because "the name appears NOWHERE in the manifest,
    // so a message quoting only the URL leaves the reader to run the derivation
    // in their head".
    std::string       value;
    // The first character outside the legal set. Meaningful only when
    // `status == IllegalCharacter`; 0xD024 requires it in the message.
    char              offendingChar = '\0';
};

// PURE. No filesystem access, no diagnostics, no allocation beyond the name.
[[nodiscard]] DSS_EXPORT DerivedCacheName
deriveDependencyCacheName(std::string_view url);

// ── B.4: THE FOUR OUTCOMES ───────────────────────────────────────────────────
enum class CacheOutcome : std::uint8_t {
    // A checkout exists, the lockfile records this exact `(url, ref)`, and
    // `rev-parse HEAD` equals the recorded commit. NO NETWORK ACCESS AT ALL —
    // not a conditional request, not an `ls-remote`, nothing. Exactly one git
    // invocation happens: the `rev-parse` that validated the claim.
    Hit,
    // No checkout, or no matching lock entry: clone → checkout → rev-parse, and
    // record. ★ A `--force-git-cache` REFRESH ALSO REPORTS `Miss`. The flag
    // bypasses the hit short-circuit and nothing else, so everything after it
    // IS the miss path — fetch → checkout → rev-parse → record — and giving it
    // a fifth name would imply a fifth behaviour that does not exist.
    Miss,
    // Clone/fetch failed and a USABLE checkout is present: 0xD01F at Info, and
    // the build PROCEEDS on what it has. The offline-build guarantee.
    FetchFallback,
    // Clone/fetch failed and there is NO usable checkout: 0xD01E, Error. The
    // dependency's sources do not exist on this machine, so continuing would
    // compile against a hole.
    AcquireFailed,
};

struct DSS_EXPORT ResolvedGitDependency {
    CacheOutcome          outcome = CacheOutcome::AcquireFailed;
    // The `.dss-deps/<name>` directory the consumer compiles from. EMPTY on
    // `AcquireFailed` — there is nothing usable there, and handing back a path
    // that looks valid is how a caller ends up globbing an empty tree and
    // reporting a successful build of nothing.
    std::filesystem::path checkout;
    // The commit the checkout is actually at. Empty on `AcquireFailed`. On
    // `FetchFallback` it is the STALE commit that was already there, which is
    // the honest answer to "what did this build compile".
    std::string           resolvedCommit;
};

// ── THE CACHE ────────────────────────────────────────────────────────────────
//
// USAGE, in the order the resolver must call it:
//   1. `open(projectDir, git, force, rep)` — loads the lockfile. nullopt means
//      the lockfile was present and unreadable, `C_MalformedJson` is already
//      emitted, and the build must ABANDON. Note what that makes structural:
//      with no cache object there is no `acquire`, so a corrupt lockfile
//      CANNOT reach the network. That is a property of the API shape, not of a
//      check somebody remembered to write.
//   2. `requireGit(rep)` — once, before any acquisition, if the manifest
//      declares any git dependency at all (U-3: no degraded git-less mode).
//   3. `registerGitDependency(url, ref, rep)` for EVERY git entry of a node
//      BEFORE acquiring any of them (M7) — this is where 0xD020 and 0xD024
//      fire, on the DERIVED NAMES and before a single byte is fetched. Returns
//      the derived name.
//   4. `acquire(name, rep)` per registered name.
//   5. `save(rep)` once, after the walk.
class DSS_EXPORT DependencyCache {
public:
    // `projectDir` is the ROOT consumer's manifest directory; the cache lives
    // at `projectDir/.dss-deps`. `git` is borrowed and must outlive the cache
    // (the non-owning-interface shape `substrate::IExecutor` established).
    // `forceRefresh` is `--force-git-cache`.
    [[nodiscard]] static std::optional<DependencyCache>
    open(std::filesystem::path const& projectDir, IGitRunner& git,
         bool forceRefresh, DiagnosticReporter& rep);

    [[nodiscard]] std::filesystem::path const& depsDir() const noexcept {
        return depsDir_;
    }
    [[nodiscard]] std::filesystem::path lockfilePath() const {
        return depsDir_ / std::string{kDependencyLockfileName};
    }

    // U-3. `false` after emitting `D_DependencyGitNotFound` — ONCE per build,
    // however many git dependencies asked. N copies of "git is not installed"
    // is noise, and the reporter's per-code cap must not be the thing that
    // hides it.
    [[nodiscard]] bool requireGit(DiagnosticReporter& rep);

    // Claim `.dss-deps/<name>` for `(url, ref)`. Returns the derived name, or
    // nullopt after emitting exactly one of:
    //   * `D_DependencyDerivedNameInvalid` (0xD024) — the name is unusable;
    //   * `D_DependencyGitNameCollision`   (0xD020) — a DIFFERENT `(url, ref)`
    //     already claimed it. The SAME `(url, ref)` is the diamond case and
    //     dedups SILENTLY, exactly as a repeated `path` dependency does.
    // Both are fail-loud and stop the build: whichever entry was acquired
    // second would otherwise clobber the first or be silently skipped in favour
    // of it, and the build would compile against a dependency it did not ask
    // for with nothing indicating a substitution.
    [[nodiscard]] std::optional<std::string>
    registerGitDependency(std::string const&                url,
                          std::optional<std::string> const& ref,
                          DiagnosticReporter&               rep);

    // Run the four-outcome machine for a name returned by
    // `registerGitDependency`. The `(url, ref)` is read back from the
    // registration rather than passed again, so the pair that was checked for
    // collisions and the pair that is acquired cannot disagree.
    [[nodiscard]] ResolvedGitDependency acquire(std::string const&  name,
                                                DiagnosticReporter& rep);

    // Persist the lockfile. Called ONCE by the owner after the walk — not per
    // acquisition, because the file is rewritten whole and N writes would buy
    // nothing.
    [[nodiscard]] bool save(DiagnosticReporter& rep) const;

    // Read-only view, for the caller that wants to report what was recorded.
    [[nodiscard]] DependencyLockfile const& lockfile() const noexcept {
        return lock_;
    }

private:
    DependencyCache(std::filesystem::path depsDir, IGitRunner& git,
                    bool forceRefresh, DependencyLockfile lock)
        : depsDir_(std::move(depsDir)), git_(&git), force_(forceRefresh),
          lock_(std::move(lock)) {}

    // Clone into a staging directory and rename into place only once the
    // checkout is at the requested ref.
    //
    // ★ WHY STAGE AT ALL. A clone that dies halfway — a dropped connection, a
    // Ctrl-C — leaves a directory at `.dss-deps/<name>` that is not a
    // repository. Nothing then removes it, and every later build sees "a
    // directory is there". The `rev-parse` probe keeps that from being read as
    // a usable checkout (so it cannot route to the 0xD01F "proceed on stale
    // sources" arm), but without staging the cache would still be WEDGED: git
    // refuses to clone into a non-empty directory, so the project would be
    // unbuildable until the operator deleted a directory by hand. Staging makes
    // the whole acquisition atomic — the final path either does not exist or
    // holds a complete checkout at the right revision — for the same reason and
    // by the same mechanism the lockfile is written temp-then-renamed.
    [[nodiscard]] GitCommandResult
    cloneStaged_(std::string const& name, std::string const& url,
                 std::optional<std::string> const& ref);

    // What was registered under a derived name, so a collision can compare the
    // FULL `(url, ref)` pair — 0xD020's discriminator is the pair, not the url
    // alone.
    struct Claim {
        std::string                url;
        std::optional<std::string> ref;
    };

    std::filesystem::path        depsDir_;
    IGitRunner*                  git_ = nullptr;   // borrowed, never owned
    bool                         force_        = false;
    bool                         gitChecked_   = false;
    bool                         gitAvailable_ = false;
    DependencyLockfile           lock_;
    std::map<std::string, Claim> claims_;
};

} // namespace dss
