#include "program/dependency_cache.hpp"

#include "core/substrate/path_identity.hpp"  // genericSpelling
#include "core/types/parse_diagnostic.hpp"
// `linker::detail::createExclusiveBinary` — the completeness marker is CLAIMED
// the way every staged file in this tree is (`lsp`, `program` and `link` are
// one `dsscp-lib`, so no new link edge).
#include "link/writer.hpp"

#include <cstdio>
#include <expected>
#include <functional>
#include <system_error>
#include <utility>

// HOST facilities for the per-dependency acquisition LOCK, and nothing else:
// `LockFileEx` on Windows, `flock` on POSIX. The split is confined to
// `AcquisitionLock` below — a host-portability split, never a target, format
// or language one.
#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <cerrno>
#    include <cstring>
#    include <fcntl.h>
#    include <sys/file.h>
#    include <unistd.h>
#endif

namespace dss {

namespace {

namespace fs = std::filesystem;

// ── ONE ACQUISITION OF A DEPENDENCY AT A TIME, ACROSS PROCESSES ─────────────
// [[D-DEPS-CONCURRENT-ACQUISITIONS-OF-ONE-DEPENDENCY-SHARE-A-PATH-AND-CAN-HANG]]
//
// ✔MEASURED 2026-09-18 (lane `rw`): two real builds of one project acquiring
// one git dependency at once shared the FIXED staging path and `remove_all`-ed
// it under each other — git's own clone then failed with `could not lock config
// file …/+staging/…/.git/config: No such file or directory`, and on the MinGW
// build one of the two HUNG: 688 s of CPU inside libstdc++'s
// `std::filesystem::remove_all`, which spins forever when another remover
// deletes the same tree (reproduced in isolation; MSVC's STL fails instead).
// The references hold a cross-process lock for exactly this (Go's
// `lockedfile.MutexAt(<module>.lock)`, cargo's package-cache lock), so no two
// acquisitions of one name ever touch its tree at once — held here for the
// whole `acquire`, released by the OS if the build dies (no stale lock is
// possible).
//
// The handle / descriptor is NOT inheritable (null `SECURITY_ATTRIBUTES` /
// `O_CLOEXEC`): the acquisition spawns `git`, and a child that inherited the
// lock handle would keep the file object — and on POSIX the `flock` — alive
// beyond this build (the `createExclusiveBinary` lesson, one tier over).
class AcquisitionLock {
public:
    AcquisitionLock() = default;
    AcquisitionLock(AcquisitionLock const&)            = delete;
    AcquisitionLock& operator=(AcquisitionLock const&) = delete;
    AcquisitionLock(AcquisitionLock&& other) noexcept { swap(other); }
    AcquisitionLock& operator=(AcquisitionLock&& other) noexcept {
        AcquisitionLock moved{std::move(other)};
        swap(moved);
        return *this;
    }
    ~AcquisitionLock() { release(); }

    // Take the lock on `file` (created if absent). If another process holds
    // it, `onContention` runs once and then this call BLOCKS until the holder
    // lets go — no timeout: the holder is another build's acquisition, which
    // ends when its clone does (the process-spawn facility's own no-deadline
    // rule, for the same reason).
    [[nodiscard]] static std::expected<AcquisitionLock, std::string>
    acquire(fs::path const& file, std::function<void()> const& onContention) {
        AcquisitionLock lock;
#ifdef _WIN32
        lock.handle_ = ::CreateFileW(
            file.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,  // not inheritable
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock.handle_ == INVALID_HANDLE_VALUE) {
            return std::unexpected("the lock file '" + core::genericSpelling(file)
                                   + "' could not be opened (Windows error "
                                   + std::to_string(::GetLastError()) + ")");
        }
        OVERLAPPED tryRange{};
        if (!::LockFileEx(lock.handle_,
                          LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                          MAXDWORD, MAXDWORD, &tryRange)) {
            DWORD const busy = ::GetLastError();
            if (busy != ERROR_LOCK_VIOLATION) {
                return std::unexpected("the lock file '" + core::genericSpelling(file)
                                       + "' could not be locked (Windows error "
                                       + std::to_string(busy) + ")");
            }
            if (onContention) onContention();
            OVERLAPPED waitRange{};
            if (!::LockFileEx(lock.handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD,
                              MAXDWORD, &waitRange)) {
                return std::unexpected("waiting for the lock file '"
                                       + core::genericSpelling(file)
                                       + "' failed (Windows error "
                                       + std::to_string(::GetLastError()) + ")");
            }
        }
#else
        lock.fd_ = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (lock.fd_ < 0) {
            return std::unexpected("the lock file '" + core::genericSpelling(file)
                                   + "' could not be opened: " + std::strerror(errno));
        }
        if (::flock(lock.fd_, LOCK_EX | LOCK_NB) != 0) {
            if (errno != EWOULDBLOCK) {
                return std::unexpected("the lock file '" + core::genericSpelling(file)
                                       + "' could not be locked: "
                                       + std::strerror(errno));
            }
            if (onContention) onContention();
            while (::flock(lock.fd_, LOCK_EX) != 0) {
                if (errno != EINTR) {
                    return std::unexpected("waiting for the lock file '"
                                           + core::genericSpelling(file)
                                           + "' failed: " + std::strerror(errno));
                }
            }
        }
#endif
        return lock;
    }

private:
    void release() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED range{};
            ::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &range);
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }
    void swap(AcquisitionLock& other) noexcept {
#ifdef _WIN32
        std::swap(handle_, other.handle_);
#else
        std::swap(fd_, other.fd_);
#endif
    }

#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// The three names a checkout directory may NOT take. `.` and `..` are the
// dangerous pair — both are spelled entirely from the legal character set, both
// are derivable from a real URL (`https://host/..`), and `depsDir / ".."`
// resolves OUTSIDE the cache root, so accepting them would let a manifest URL
// aim a clone (and a `remove_all` of a stale staging attempt) at the consumer's
// own project directory. The third is the lockfile's own path, which a checkout
// directory would sit on top of.
constexpr std::array<std::string_view, 3> kReservedCacheNames{
    ".", "..", kDependencyLockfileName};

[[nodiscard]] constexpr bool isLegalCacheNameChar(char c) {
    // ASCII ranges written out rather than `std::isalnum`: that function is
    // locale-sensitive and is UB on a negative `char`, and this predicate must
    // give the SAME answer on every host — a derivation whose legality varied
    // by locale would make one manifest resolve on one machine and fail on
    // another, which is the exact host-dependence 0xD024's allocation refuses.
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

void emitDerivedNameInvalid(DiagnosticReporter& rep, std::string_view url,
                            std::string detail) {
    report(rep, DiagnosticCode::D_DependencyDerivedNameInvalid,
           DiagnosticSeverity::Error,
           "git dependency '" + std::string{url} + "': " + std::move(detail)
               + ". The cache directory name is DERIVED from the URL — the last "
                 "path segment with one trailing `.git` removed — so fix the "
                 "URL: repoint it, re-spell it, or mirror the repository under "
                 "a last segment made only of letters, digits, `.`, `_` and "
                 "`-`.");
}

// `{url, ref}` rendered for a diagnostic, with the ABSENT ref spelled as such.
// A message that printed an empty string for "no ref declared" would make the
// two shapes 0xD020 discriminates on look identical in the one place the reader
// is trying to tell them apart.
[[nodiscard]] std::string
describeEntry(std::string const& url, std::optional<std::string> const& ref) {
    return "'" + url + "'" + (ref ? " at ref '" + *ref + "'"
                                  : " with no ref declared");
}

} // namespace

DerivedCacheName deriveDependencyCacheName(std::string_view url) {
    DerivedCacheName out;

    // LAST NON-EMPTY segment: trailing separators are skipped first, so
    // `…/bar/` and `…/bar//` derive the same name as `…/bar`.
    std::size_t end = url.size();
    while (end > 0 && url[end - 1] == '/') --end;
    if (end == 0) {
        out.status = DerivedNameStatus::NoSegment;
        return out;
    }
    std::size_t const slash = url.rfind('/', end - 1);
    std::size_t const begin = (slash == std::string_view::npos) ? 0 : slash + 1;

    std::string segment{url.substr(begin, end - begin)};

    // ONE trailing `.git`, never repeatedly: `…/x.git.git` derives `x.git`.
    // `>=` rather than `>` so a segment that is EXACTLY `.git` strips to
    // nothing and lands on `NoSegment` — the alternative would create a
    // directory called `.git` inside `.dss-deps`, which makes the cache root
    // look like a git repository to every tool that walks upward.
    constexpr std::string_view kGitSuffix = ".git";
    if (segment.size() >= kGitSuffix.size()
        && segment.compare(segment.size() - kGitSuffix.size(),
                           kGitSuffix.size(), kGitSuffix)
               == 0) {
        segment.resize(segment.size() - kGitSuffix.size());
    }
    if (segment.empty()) {
        out.status = DerivedNameStatus::NoSegment;
        return out;
    }

    // Filled in BEFORE the rejects, because 0xD024's message has to show it.
    out.value = segment;

    for (char const c : segment) {
        if (!isLegalCacheNameChar(c)) {
            out.status        = DerivedNameStatus::IllegalCharacter;
            out.offendingChar = c;
            return out;
        }
    }
    for (auto const& reserved : kReservedCacheNames) {
        if (segment == reserved) {
            out.status = DerivedNameStatus::ReservedName;
            return out;
        }
    }
    out.status = DerivedNameStatus::Ok;
    return out;
}

std::optional<DependencyCache>
DependencyCache::open(fs::path const& projectDir, IGitRunner& git,
                      bool forceRefresh, DiagnosticReporter& rep) {
    fs::path depsDir = projectDir / std::string{kDependencyCacheDirName};
    auto     lock    = DependencyLockfile::load(
        depsDir / std::string{kDependencyLockfileName}, rep);
    // A PRESENT-BUT-UNREADABLE lockfile has already emitted `C_MalformedJson`;
    // returning nullopt means no cache object exists, so nothing downstream can
    // reach the network. The fail-loud is structural, not a check to remember.
    if (!lock) return std::nullopt;
    return DependencyCache{std::move(depsDir), git, forceRefresh,
                           std::move(*lock)};
}

bool DependencyCache::requireGit(DiagnosticReporter& rep) {
    // LATCHED: the answer cannot change under a running build, and 0xD01D's
    // allocation requires it be "emitted once per build, not once per git
    // dependency — N copies of 'git is not installed' is noise, and the
    // reporter's per-code cap must not be the thing that hides it".
    if (gitChecked_) return gitAvailable_;
    gitChecked_   = true;
    gitAvailable_ = git_->isAvailable();
    if (!gitAvailable_) {
        report(rep, DiagnosticCode::D_DependencyGitNotFound,
               DiagnosticSeverity::Error,
               "this project declares a git dependency, but `git` could not be "
               "found on PATH, so no dependency can be acquired. Install git or "
               "put it on PATH. There is deliberately no git-less mode: a cache "
               "HIT is validated by asking git what the checkout's HEAD is, so "
               "even a fully-populated `.dss-deps` cannot be used without it, "
               "and reusing a checkout nobody verified is how a build silently "
               "compiles the wrong revision.");
    }
    return gitAvailable_;
}

std::optional<std::string>
DependencyCache::registerGitDependency(std::string const&                url,
                                       std::optional<std::string> const& ref,
                                       DiagnosticReporter&               rep) {
    DerivedCacheName const derived = deriveDependencyCacheName(url);
    switch (derived.status) {
        case DerivedNameStatus::NoSegment:
            emitDerivedNameInvalid(
                rep, url,
                "the URL has no usable last path segment, so no cache "
                "directory name can be derived from it");
            return std::nullopt;
        case DerivedNameStatus::IllegalCharacter:
            emitDerivedNameInvalid(
                rep, url,
                "the derived cache directory name '" + derived.value
                    + "' contains the character '"
                    + std::string(1, derived.offendingChar)
                    + "', which is outside the allowed set [A-Za-z0-9._-]. The "
                      "set is the same on every host on purpose, so a manifest "
                      "cannot resolve on one machine and fail on another");
            return std::nullopt;
        case DerivedNameStatus::ReservedName:
            emitDerivedNameInvalid(
                rep, url,
                "the derived cache directory name '" + derived.value
                    + "' is reserved: `.` and `..` would resolve outside or on "
                      "top of the cache root, and '"
                    + std::string{kDependencyLockfileName}
                    + "' is the cache's own lockfile");
            return std::nullopt;
        case DerivedNameStatus::Ok:
            break;
    }

    auto const existing = claims_.find(derived.value);
    if (existing != claims_.end()) {
        // THE DIAMOND: the same url with the same ref, named twice. Dedups
        // SILENTLY, exactly as a repeated `path` dependency does — it is one
        // node reached by two edges, not a conflict.
        if (existing->second.url == url && existing->second.ref == ref) {
            return derived.value;
        }
        // Two things want one directory. Detected HERE, on the derived names,
        // BEFORE any acquisition — post-acquisition detection cannot see the
        // same-url-different-ref shape at all, because the second entry's
        // checkout target already exists and looks like a cache hit.
        report(rep, DiagnosticCode::D_DependencyGitNameCollision,
               DiagnosticSeverity::Error,
               "two git dependencies both derive the cache directory '"
                   + derived.value + "': " + describeEntry(existing->second.url,
                                                           existing->second.ref)
                   + " and " + describeEntry(url, ref)
                   + ". One checkout cannot hold both, and whichever was "
                     "acquired second would clobber the first or be silently "
                     "skipped in favour of it — the build would then compile "
                     "against a dependency it did not ask for. Make the two "
                     "entries agree, or repoint one at a URL whose last path "
                     "segment differs.");
        return std::nullopt;
    }

    claims_.emplace(derived.value, Claim{url, ref});
    return derived.value;
}

GitCommandResult
DependencyCache::cloneInPlace_(std::string const& name, std::string const& url,
                               std::optional<std::string> const& ref) {
    GitCommandResult out;
    fs::path const   checkoutDir = depsDir_ / name;
    fs::path const   marker      = partialMarker_(name);
    std::error_code  ec;

    // ── A LEFTOVER IS REMOVED ONLY IF THIS CACHE MARKED IT AS ITS OWN ────────
    // We hold `<name>`'s lock, so no acquisition is writing here: a directory
    // WITH the marker is an interrupted acquisition's residue and is ours to
    // remove. One WITHOUT the marker was not left by this cache — a user's
    // directory, an external partial delete — and deleting it would destroy
    // something we did not create; it is refused, as it always was (the
    // staging design's landing rename refused it too).
    bool const markedOurs = fs::exists(marker, ec) && !ec;
    ec.clear();
    if (fs::exists(fs::symlink_status(checkoutDir, ec)) && !ec) {
        if (!markedOurs) {
            out.detail = "'" + core::genericSpelling(checkoutDir)
                       + "' exists but is not a usable checkout, and no '"
                       + core::genericSpelling(marker)
                       + "' says an interrupted acquisition of this cache left "
                         "it, so it is not removed. Delete it to let the cache "
                         "acquire the dependency again";
            return out;
        }
        fs::remove_all(checkoutDir, ec);
        if (ec) {
            out.detail = "the incomplete checkout an interrupted acquisition left "
                         "at '"
                       + core::genericSpelling(checkoutDir)
                       + "' could not be removed: " + ec.message();
            return out;
        }
    }
    ec.clear();

    // ── MARK INCOMPLETE BEFORE ONE BYTE OF THE TREE IS WRITTEN ──────────────
    if (!markedOurs) {
        fs::create_directories(marker.parent_path(), ec);
        std::FILE* const claimed =
            ec ? nullptr : linker::detail::createExclusiveBinary(marker);
        if (claimed == nullptr || std::fclose(claimed) != 0) {
            out.detail = "the completeness marker '" + core::genericSpelling(marker)
                       + "' could not be created"
                       + (ec ? ": " + ec.message() : std::string{});
            return out;
        }
    }

    // A failed clone or ref checkout leaves nothing a later build could trust:
    // the tree goes, and so does the marker — unless the tree could NOT be
    // removed, in which case the marker stays and says so to the next build.
    auto abandon = [&checkoutDir, &marker](GitCommandResult result) {
        std::error_code rmec;
        fs::remove_all(checkoutDir, rmec);
        if (!rmec) fs::remove(marker, rmec);
        return result;
    };

    out = git_->clone(url, checkoutDir);
    if (!out.ok) return abandon(std::move(out));

    // The ref is applied while the marker still stands: a checkout that fails
    // leaves a tree at the WRONG revision, which must never read as usable —
    // the next build would otherwise fall back onto it under 0xD01F.
    if (ref) {
        out = git_->checkout(checkoutDir, *ref);
        if (!out.ok) return abandon(std::move(out));
    }

    // ── PUBLISH: the tree is complete ───────────────────────────────────────
    // ✔MEASURED: nothing holds this empty file when it is removed (0 refusals in
    // 3300 create-then-remove cycles under load), so one attempt; a failure is
    // LOUD, and the marker left behind makes the next build re-acquire rather
    // than trust.
    fs::remove(marker, ec);
    if (ec) {
        out.ok     = false;
        out.detail = "the checkout at '" + core::genericSpelling(checkoutDir)
                   + "' is complete but could not be marked so — '"
                   + core::genericSpelling(marker)
                   + "' could not be removed: " + ec.message();
    }
    return out;
}

ResolvedGitDependency DependencyCache::acquire(std::string const&  name,
                                               DiagnosticReporter& rep) {
    ResolvedGitDependency out;

    auto const claim = claims_.find(name);
    if (claim == claims_.end()) {
        // DEFENCE IN DEPTH, not a live path: `registerGitDependency` is the
        // only producer of these names and the caller is documented to run it
        // first. Reported rather than asserted because a silent empty result
        // here would be indistinguishable from a dependency that resolved to
        // nothing, which is the one failure shape with no message attached.
        report(rep, DiagnosticCode::D_DependencyGitAcquireFailed,
               DiagnosticSeverity::Error,
               "internal: cache directory '" + name
                   + "' was acquired without being registered first. Name "
                     "registration is the pre-pass that detects colliding "
                     "derived names before anything is fetched — fix the "
                     "caller, not the manifest.");
        return out;
    }
    std::string const&                url = claim->second.url;
    std::optional<std::string> const& ref = claim->second.ref;
    fs::path const checkoutDir            = depsDir_ / name;
    fs::path const marker                 = partialMarker_(name);

    // ── ONE ACQUISITION OF `<name>` AT A TIME — see `AcquisitionLock` ───────
    // Taken before the checkout is even LOOKED at, and held to the end of this
    // function: a build that finds another's acquisition in progress waits for
    // it, then re-reads the state the other one left — never a half-written
    // tree, and never a `remove_all` racing the other build's.
    std::error_code ec;
    fs::path const  lockDir = depsDir_ / std::string{kDependencyLockDirName};
    fs::create_directories(lockDir, ec);
    auto held = ec ? std::expected<AcquisitionLock, std::string>{std::unexpected(
                         "the lock directory '" + core::genericSpelling(lockDir)
                         + "' could not be created: " + ec.message())}
                   : AcquisitionLock::acquire(lockDir / name, [this, &name] {
                         if (lockContentionObserver_) lockContentionObserver_(name);
                     });
    if (!held) {
        report(rep, DiagnosticCode::D_DependencyGitAcquireFailed,
               DiagnosticSeverity::Error,
               "git dependency " + describeEntry(url, ref)
                   + " could not be acquired: " + held.error()
                   + ". Without the lock another build could be writing the "
                     "same checkout, so nothing was touched.");
        return out;
    }
    ec.clear();

    // ── "IS THERE A USABLE CHECKOUT?" — B.4's ONE DISCRIMINATOR ──────────────
    // Deliberately NOT `is_directory`. A directory can exist without being a
    // repository (an interrupted clone, a half-deleted tree, a user's own
    // mkdir), and calling that "a checkout" routes a later failure to 0xD01F —
    // "the build PROCEEDS on possibly-stale sources" — over a tree that has no
    // sources in it at all. Asking git is the only honest probe, and U-3
    // already requires this exact call on the hit path, so it costs nothing
    // there. ★ AND THE COMPLETENESS MARKER VETOES IT: a tree whose writing was
    // interrupted can answer `rev-parse` perfectly well (git writes HEAD before
    // the working tree is complete) — the marker is the only thing that knows.
    bool        hasCheckout = false;
    std::string headCommit;
    bool const  markedIncomplete = fs::exists(marker, ec) && !ec;
    ec.clear();
    if (!markedIncomplete && fs::is_directory(checkoutDir, ec) && !ec) {
        GitCommandResult const head = git_->revParse(checkoutDir, "HEAD");
        hasCheckout                 = head.ok;
        headCommit                  = head.output;
    }

    // ── THE HIT SHORT-CIRCUIT ────────────────────────────────────────────────
    // All four clauses are load-bearing. `url` and `ref` because a lock entry
    // recorded for a DIFFERENT pair says nothing about this one; the commit
    // comparison because a checkout somebody moved by hand is exactly the
    // stale-but-unnoticed state U-3 calls the silent-miscompile direction. Past
    // this point NO network access happens: not a conditional request, not an
    // `ls-remote`, nothing.
    std::optional<LockedDependency> const locked = lock_.find(name);
    if (!force_ && hasCheckout && locked && locked->url == url
        && locked->ref == ref && locked->resolvedCommit == headCommit) {
        out.outcome        = CacheOutcome::Hit;
        out.checkout       = checkoutDir;
        out.resolvedCommit = headCommit;
        return out;
    }

    // ── THE NETWORK PATH ─────────────────────────────────────────────────────
    GitCommandResult acq =
        hasCheckout ? git_->fetch(checkoutDir, ref.value_or(std::string{}))
                    : cloneInPlace_(name, url, ref);

    // ★ A FETCH MUST BE FOLLOWED BY A CHECKOUT, AND IT CHECKS OUT `FETCH_HEAD`
    // RATHER THAN THE REF NAME. Fetching alone updates refs and leaves HEAD
    // exactly where it was, so `rev-parse HEAD` would return the OLD commit and
    // `--force-git-cache` would be a network round trip that changes nothing —
    // a flag whose test could pass ("fetch was called") over an entirely absent
    // mechanism. Checking out the ref by NAME does not fix it either: a branch
    // already checked out moves nowhere. `FETCH_HEAD` is the tip that was just
    // fetched — and `gitFetchArgv` fetches the manifest's ref EXPLICITLY, so
    // FETCH_HEAD means exactly the revision the manifest asked for.
    // The clone arm needs no counterpart here: `cloneInPlace_` has already
    // applied the ref under its marker, and a clone with no ref lands on the
    // remote's default branch, which is what "no ref declared" means.
    //
    // ★ THE CHECKOUT REWRITES A TRUSTED TREE, SO IT RUNS UNDER THE MARKER TOO
    // (cargo's `reset`: remove `.cargo-ok`, reset, recreate it). A fetch
    // touches no working-tree file, so a FAILED FETCH leaves a tree that is
    // still exactly the old commit — the 0xD01F fallback below stays honest. A
    // checkout that fails MIDWAY leaves a tree that is part old, part new, while
    // HEAD may still name the old commit; falling back onto it would compile a
    // mix and report a commit it is not. So the marker goes up first and comes
    // down only when the checkout succeeded.
    bool refreshLeftTreeIncomplete = false;
    if (acq.ok && hasCheckout) {
        fs::create_directories(marker.parent_path(), ec);
        std::FILE* const claimed =
            ec ? nullptr : linker::detail::createExclusiveBinary(marker);
        if (claimed == nullptr || std::fclose(claimed) != 0) {
            acq.ok     = false;
            acq.detail = "the completeness marker '" + core::genericSpelling(marker)
                       + "' could not be created, so the working tree was not "
                         "rewritten";
        } else {
            acq = git_->checkout(checkoutDir, "FETCH_HEAD");
            if (acq.ok) {
                fs::remove(marker, ec);
                if (ec) {
                    acq.ok     = false;
                    acq.detail = "the refreshed checkout is complete but could "
                                 "not be marked so — '"
                               + core::genericSpelling(marker)
                               + "' could not be removed: " + ec.message();
                    refreshLeftTreeIncomplete = true;
                }
            } else {
                refreshLeftTreeIncomplete = true;
            }
        }
    }

    if (!acq.ok) {
        if (refreshLeftTreeIncomplete) {
            // Not a network fallback: the tree this build would fall back ON has
            // been rewritten (or partly rewritten), and its marker says so.
            acq.detail += "; the checkout at '" + core::genericSpelling(checkoutDir)
                        + "' was being rewritten and is now marked incomplete, so "
                          "it is not used — the next build re-acquires it";
        } else if (hasCheckout) {
            // Re-probe rather than reporting the commit seen before the
            // attempt: the fetch may have got as far as moving something, and a
            // build that says it compiled commit X when the tree is at Y is a
            // confident lie in the one field the lockfile exists to make true.
            // It also re-applies the ONE discriminator to the CURRENT state.
            GitCommandResult const now = git_->revParse(checkoutDir, "HEAD");
            if (now.ok) {
                // ★ Info, and `Guaranteed` — BOTH, ASKED FOR SEPARATELY.
                // Info because `--warnings-as-errors` promotes every Warning
                // code-agnostically, so at Warning severity this notice would
                // fail every offline build, which is the exact outcome it
                // exists to prevent. `Guaranteed` because it is the ONLY
                // statement that this build used sources it could not refresh,
                // nothing downstream re-reports it, and the reporter's cap
                // would otherwise be free to drop it on a diagnostic-heavy
                // compile. Membership in `kUnsuppressableCodes` would ALSO
                // bypass the cap, and that is precisely why the property is
                // set here anyway: delivery is not something a code may obtain
                // as a side effect of a suppression verdict.
                ParseDiagnostic d;
                d.code     = DiagnosticCode::D_DependencyGitFetchFallback;
                d.severity = DiagnosticSeverity::Info;
                d.delivery = DiagnosticDelivery::Guaranteed;
                d.actual =
                    "git dependency " + describeEntry(url, ref)
                    + " could not be refreshed (" + acq.detail
                    + "), so the build is CONTINUING on the existing checkout "
                      "at '"
                    + core::genericSpelling(checkoutDir) + "', commit " + now.output
                    + ". Those sources may be out of date. This is deliberate — "
                      "an unreachable network must not stop a build that has "
                      "everything it needs on disk — and it is reported so that "
                      "'possibly stale' is never silent.";
                rep.report(std::move(d));

                out.outcome        = CacheOutcome::FetchFallback;
                out.checkout       = checkoutDir;
                out.resolvedCommit = now.output;
                // ★ DELIBERATELY NOT RECORDED IN THE LOCKFILE. Recording this
                // commit would make the NEXT build a cache hit, which would
                // stop attempting the refresh and stop emitting this notice —
                // the staleness would quietly become the new recorded truth.
                // Leaving the lockfile alone means every subsequent build
                // re-tries and re-says it until the network comes back.
                return out;
            }
            acq.detail += "; the existing checkout at '"
                        + core::genericSpelling(checkoutDir)
                        + "' is no longer usable either (" + now.detail + ")";
        }
        report(rep, DiagnosticCode::D_DependencyGitAcquireFailed,
               DiagnosticSeverity::Error,
               "git dependency " + describeEntry(url, ref)
                   + " could not be acquired into '"
                   + core::genericSpelling(checkoutDir) + "': " + acq.detail
                   + ". There is no usable checkout to fall back on, so the "
                     "dependency's sources do not exist on this machine and "
                     "continuing would compile against a hole. Check the URL, "
                     "the ref, network reachability and credentials.");
        return out;   // AcquireFailed, with `checkout` and `resolvedCommit`
                      // deliberately left empty
    }

    // ── WHAT DID WE ACTUALLY GET? ────────────────────────────────────────────
    // The same probe, applied after the acquisition. A failure here is not a
    // network fallback: we have already moved this tree, so the untouched prior
    // checkout 0xD01F promises to fall back ON no longer exists, and the
    // discriminator ("is there a usable checkout") now answers no.
    GitCommandResult const head = git_->revParse(checkoutDir, "HEAD");
    if (!head.ok) {
        report(rep, DiagnosticCode::D_DependencyGitAcquireFailed,
               DiagnosticSeverity::Error,
               "git dependency " + describeEntry(url, ref)
                   + " was acquired into '" + core::genericSpelling(checkoutDir)
                   + "' but its commit could not be read: " + head.detail
                   + ". Without it the build cannot record what it compiled, so "
                     "the checkout is treated as unusable rather than used "
                     "unidentified.");
        return out;
    }

    out.outcome        = CacheOutcome::Miss;
    out.checkout       = checkoutDir;
    out.resolvedCommit = head.output;
    lock_.record(name, LockedDependency{url, ref, head.output});
    return out;
}

bool DependencyCache::save(DiagnosticReporter& rep) const {
    return lock_.save(lockfilePath(), rep);
}

} // namespace dss
