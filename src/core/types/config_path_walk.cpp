#include "core/types/config_path_walk.hpp"

#include "core/substrate/path_identity.hpp"       // absoluteKeepingRoot -- UNC-safe absolute
#include "core/substrate/phase_timers.hpp"        // the `locate-config` pipeline phase
#include "core/types/predefined_macro_json.hpp"   // kBuildVersionText — the binary's own version

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

// The INSTALLED layout, as a path RELATIVE to the directory holding the
// installed executable — computed by `cmake/DssInstall.cmake` from the same
// `GNUInstallDirs` variables the `install()` rules use, and forwarded here by
// `src/core/CMakeLists.txt`.
//
// ★ IT IS COMPUTED, NOT WRITTEN, AND THAT IS THE WHOLE POINT. The install
// layout has exactly ONE owner — the CMake code that performs the install — and
// this file consumes what that owner decided. A literal spelled here would be a
// second owner of the layout, free to drift from the `install(DIRECTORY ...)`
// destination without anything noticing until a packaged compiler could not
// find its own config. It is RELATIVE (never a baked absolute prefix) because
// every packaging path this project targets RELOCATES the prefix — Homebrew's
// cellar, Nix's store, Scoop's app dir, a user's `tar -xf` into `~/opt`.
//
// Undefined => the build is BROKEN, loudly, here and now — exactly as
// `DSS_PROJECT_VERSION` does one header over. A default would ship a compiler
// that silently has no installed-layout arm, which is the defect
// [[D-PKG-NO-PACKAGING-PATH-SHIPS-THE-CONFIG-TREE]] closed.
#ifndef DSS_INSTALL_CONFIG_RELDIR
#    error "DSS_INSTALL_CONFIG_RELDIR is not defined — cmake/DssInstall.cmake must compute it and src/core/CMakeLists.txt must forward it (see the top-level CMakeLists.txt)."
#endif

namespace dss {

namespace {

namespace fs = std::filesystem;

// `<root>/src/dss-config` — the REPO-SHAPED config root. Both the
// `DSS_CONFIG_ROOT` override and the cwd walk name a directory that CONTAINS
// `src/dss-config/`, so both compose through here.
fs::path repoShapedConfigRoot(fs::path const& treeRoot) {
    return treeRoot / "src" / "dss-config";
}

// ── The running executable's own path ────────────────────────────────────────
//
// HOST-OS specific, and that is not the agnosticism the bar vetoes: "where is my
// own image on THIS machine" is a property of the host operating system running
// the compiler, with no bearing on the source language, the target CPU or the
// object format being compiled FOR. The same `#if defined(_WIN32)` shape already
// carries `core/substrate/process_spawn.cpp` and `large_stack_call.cpp`.
//
// ⚠ `argv[0]` is deliberately NOT used. It is whatever the parent chose to pass,
// it is routinely a bare command name resolved through `PATH`, and a caller can
// set it to anything at all — so an `argv[0]`-derived layout probe would be an
// attacker-influenced (and, far more often, merely wrong) input to the question
// "which config tree is mine". Every host below answers from the KERNEL's record
// of the loaded image instead.
std::optional<fs::path> runningExecutablePath() {
#if defined(_WIN32)
    // `GetModuleFileNameW(nullptr, ...)` yields the loaded image's path. It does
    // NOT report the required size, so grow until the result fits: success is
    // "wrote fewer characters than the buffer holds", because a return equal to
    // the buffer size means TRUNCATED (and on older Windows the buffer is then
    // not even null-terminated, which is why the length is what is tested rather
    // than the last character).
    std::wstring buf(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 6; ++attempt) {
        DWORD const n =
            ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::nullopt;                    // the call itself failed
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path{buf};
        }
        buf.resize(buf.size() * 2);
    }
    return std::nullopt;
#elif defined(__APPLE__)
    // `_NSGetExecutablePath` REPORTS the required size through the same
    // out-parameter when it fails, so one retry always suffices; the loop is
    // kept symmetric with the Windows arm rather than special-cased.
    std::uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);                 // size := required, rc != 0
    if (size == 0) return std::nullopt;
    std::string buf(size, '\0');
    if (::_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    return fs::path{buf};
#else
    // Linux (and the BSDs that provide it). `/proc/self/exe` is already fully
    // symlink-resolved by the kernel.
    std::error_code ec;
    fs::path const  exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) return std::nullopt;
    return exe;
#endif
}

// Read the repo-root `VERSION` file that sits BESIDE `src/` — the same single
// source of truth the top-level `CMakeLists.txt` reads to set the project
// version and to define `DSS_PROJECT_VERSION`. Nothing new is written down; a
// repo-shaped config tree's version is simply the version of the tree that
// contains it.
//
// nullopt => the tree declares no version (unreadable/absent `VERSION`). That is
// NOT a mismatch and is not treated as one: the guard makes a positive claim
// only when it has BOTH facts.
std::optional<std::string> declaredTreeVersion(fs::path const& treeRoot) {
    std::ifstream in(treeRoot / "VERSION", std::ios::binary);
    if (!in) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>()};
    auto const isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    std::size_t b = 0, e = text.size();
    while (b < e && isSpace(text[b])) ++b;
    while (e > b && isSpace(text[e - 1])) --e;
    if (b == e) return std::nullopt;                        // present but empty
    return text.substr(b, e - b);
}

// ★★ BINARY / CONFIG VERSION SKEW — the failure this whole precedence exists to
// make impossible, stated once here because it is the reason the arms are
// ordered the way they are.
//
// A compiler binary paired with a config tree from a DIFFERENT version does not
// fail; it compiles something subtly different. The predefined-macro set, a
// target's register file, a shipped header's `struct` layout and an object
// format's relocation table all live in that tree, so a mismatch is the same
// silent-wrong-answer class as a stale cached object — and it is invisible from
// inside one image, exactly like the copy-reloc `environ` split.
//
// TWO ARMS, TWO MECHANISMS, ONE PROPERTY:
//   * the INSTALLED arm composes the binary's OWN version into the path it
//     probes, so agreement is BY CONSTRUCTION — a 0.0.3 binary cannot reach a
//     0.0.2 tree, and the failure mode is a loud not-found that names the exact
//     path, never a silent wrong answer. (This is gcc's answer: its private data
//     lives under `/usr/lib/gcc/<triple>/<version>/`.)
//   * the REPO-SHAPED arms (`DSS_CONFIG_ROOT`, the cwd walk) read the tree's own
//     `VERSION` and REFUSE on a mismatch.
//
// Engaged => the human-readable explanation; nullopt => no disagreement.
std::optional<std::string> repoTreeVersionSkew(fs::path const& treeRoot) {
    auto const declared = declaredTreeVersion(treeRoot);
    if (!declared.has_value()) return std::nullopt;
    std::string const mine{detail::kBuildVersionText};
    if (*declared == mine) return std::nullopt;
    return "shipped-config version skew: this compiler is version " + mine
         + " but the config tree at '" + repoShapedConfigRoot(treeRoot).generic_string()
         + "' belongs to version " + *declared
         + " (declared by '" + (treeRoot / "VERSION").generic_string()
         + "'). Refusing to compile against a config tree from a different "
           "version — it would not fail, it would silently compile something "
           "else. Rebuild, or point DSS_CONFIG_ROOT at the matching tree.";
}

// The outcome of walking the precedence for ONE requested item. Shared by both
// public forms so they CANNOT disagree about which tree is in play — a file
// resolving out of one tree while a directory resolves out of another is the
// mixed-tree variant of the skew above, and it would be invisible.
struct Resolution {
    std::optional<fs::path>  hit;        // engaged => resolved
    std::optional<std::string> refusal;  // engaged => stop, and say this
    std::vector<std::string> tried;      // every candidate, in order, for the diagnostic
};

// Walk the precedence, asking `probe` whether a composed candidate is the thing
// the caller wants (`exists` for the file form, `is_directory` for the dir form).
//
// PRECEDENCE — and each position is a decision, not an accident:
//
//   1. `$DSS_CONFIG_ROOT` (repo-shaped). An EXPLICIT operator override outranks
//      every form of discovery, by definition of "override". `dss_add_test` and
//      `integrated_tests` set it to the repo root so an out-of-tree ctest — whose
//      cwd is a build subdirectory with no `src/dss-config/` anywhere in its
//      ancestry — resolves config at all.
//
//   2. THE INSTALLED LAYOUT, relative to this executable. ★ IT OUTRANKS THE CWD
//      WALK, and that ordering is the safety property: a packaged `dsscp` at
//      `/usr/bin` invoked inside a user's project must compile with the config it
//      was INSTALLED WITH, never with whatever `src/dss-config/` happens to sit in
//      that project's ancestry. Ordering the walk first would make an unrelated
//      directory silently redefine the compiler — the version-skew class above,
//      reached by accident instead of by mistake. The arm is INERT for a
//      development build (a binary in `build/bin/dss/` has no installed layout
//      around it), so this reorders nothing for the repo's own workflow.
//      ★★ AND WHEN THE INSTALLED ROOT EXISTS IT IS AUTHORITATIVE: a miss INSIDE
//      it stops the search rather than falling through to the walk. A packaged
//      compiler that does not ship some target must say so, not quietly borrow
//      that target from a stranger's tree and mix two config trees in one
//      compilation.
//
//   3. The cwd ancestor walk (<= 8 hops). What makes development work — the repo
//      binary finds the repo config — and, after (2), reachable only by a binary
//      that has no installed tree of its own.
Resolution resolveByPrecedence(std::string_view                             subdir,
                               std::string_view                             leaf,
                               std::optional<fs::path> const&               startPath,
                               bool (*probe)(fs::path const&, std::error_code&)) {
    Resolution      out;
    std::error_code ec;

    auto compose = [&](fs::path const& configRoot) {
        fs::path p = configRoot;
        if (!subdir.empty()) p /= std::string{subdir};
        if (!leaf.empty())   p /= std::string{leaf};
        return p;
    };

    // An engaged `startPath` means "discover from exactly here" — the caller is
    // naming the tree, not hinting at one — so it SKIPS both the environment and
    // the installed layout and runs only the walk, from there. Only the LSP uses
    // it and it is load-bearing: the discovery fixtures point at a scratch dir,
    // and honouring the ambient environment over an explicit caller argument
    // would make them untestable.
    if (!startPath.has_value()) {
        // ── 1. the explicit override ─────────────────────────────────────────
        if (const char* envRoot = std::getenv("DSS_CONFIG_ROOT");
            envRoot != nullptr && envRoot[0] != '\0') {
            fs::path const treeRoot{envRoot};
            fs::path const candidate = compose(repoShapedConfigRoot(treeRoot));
            out.tried.push_back("$DSS_CONFIG_ROOT -> " + candidate.generic_string());
            if (probe(candidate, ec)) {
                // Judge the tree only when it is the one about to be USED. A
                // set-but-miss override falls THROUGH to the arms below, exactly
                // as it always has — a stale override never worsens discovery.
                if (auto skew = repoTreeVersionSkew(treeRoot)) {
                    out.refusal = std::move(skew);
                    return out;
                }
                out.hit = candidate;
                return out;
            }
        }

        // ── 2. the installed layout, and it is AUTHORITATIVE once found ──────
        if (auto const exeDir = runningExecutableDir()) {
            if (auto const installedRoot = installedConfigRootFrom(*exeDir)) {
                fs::path const candidate = compose(*installedRoot);
                out.tried.push_back("installed layout -> " + candidate.generic_string());
                if (probe(candidate, ec)) {
                    // No version check: the probed path was composed FROM this
                    // binary's own version, so reaching it IS the agreement.
                    out.hit = candidate;
                    return out;
                }
                // The tree is this binary's own and it does not have the item.
                // Stop — see the AUTHORITATIVE note above.
                return out;
            }
            out.tried.push_back(
                "installed layout -> "
                + (*exeDir / fs::path{std::string{installedConfigRelDir()}})
                      .lexically_normal().generic_string()
                + " (no such directory — this is not an installed tree)");
        } else {
            out.tried.push_back(
                "installed layout -> (skipped: this host did not report the "
                "running executable's own path)");
        }
    }

    // ── 3. the cwd ancestor walk ─────────────────────────────────────────────
    fs::path here = startPath.value_or(fs::current_path(ec));
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        fs::path const candidate = compose(repoShapedConfigRoot(here));
        out.tried.push_back("cwd walk -> " + candidate.generic_string());
        if (probe(candidate, ec)) {
            if (auto skew = repoTreeVersionSkew(here)) {
                out.refusal = std::move(skew);
                return out;
            }
            out.hit = candidate;
            return out;
        }
        fs::path const parent = here.parent_path();
        if (parent == here) break;   // hit the filesystem root
        here = parent;
    }

    return out;
}

bool probeExists(fs::path const& p, std::error_code& ec) {
    return fs::exists(p, ec);
}

// `is_directory` rather than `exists` for the directory form, because that IS
// the analogue: `exists` would accept a plain FILE named `shippedLibs` and hand
// a caller a "directory" it cannot iterate.
bool probeIsDirectory(fs::path const& p, std::error_code& ec) {
    return fs::is_directory(p, ec);
}

// Render "here is every path I tried, in order" for a not-found diagnostic. An
// installed binary that cannot find its config MUST say where it looked: the
// alternative is a confusing downstream error about a missing language or a
// missing `<stdio.h>`, which is the defect this arm exists to remove rather than
// to re-create one layer down.
std::string renderTried(std::vector<std::string> const& tried) {
    std::string s;
    for (auto const& t : tried) {
        s += "\n  tried: ";
        s += t;
    }
    return s;
}

} // namespace

std::string_view installedConfigRelDir() {
    return DSS_INSTALL_CONFIG_RELDIR;
}

std::optional<std::filesystem::path> runningExecutableDir() {
    // Cached: every shipped-config lookup consults it, and the answer cannot
    // change within a process. Function-local static initialisation is
    // thread-safe since C++11, and this is a READ of immutable process state —
    // the same no-writes discipline the `std::getenv` arm documents.
    static std::optional<fs::path> const cached = [] () -> std::optional<fs::path> {
        auto const exe = runningExecutablePath();
        if (!exe.has_value()) return std::nullopt;
        std::error_code ec;
        // Resolve symlinks: a distro that ships `/usr/bin/dsscp -> ../lib/dss/dsscp`
        // must land on the REAL image's directory, or the relative hop to the data
        // dir starts from the wrong place. (`/proc/self/exe` is already resolved;
        // this normalises the other two hosts to the same answer.) On failure keep
        // the unresolved path rather than losing the arm entirely.
        fs::path const real = fs::weakly_canonical(*exe, ec);
        fs::path const use  = ec ? *exe : real;
        fs::path        dir = use.parent_path();
        if (dir.empty()) return std::nullopt;
        return dir;
    }();
    return cached;
}

std::optional<std::filesystem::path>
installedConfigRootFrom(std::filesystem::path const& executableDir) {
    std::error_code ec;
    // Through the accessor, NOT the macro. ⚠ This read used to name
    // `DSS_INSTALL_CONFIG_RELDIR` directly, which made TWO readers of one fact —
    // and the round-trip test composes its expected path through the accessor,
    // so the pin was checking a path the implementation did not use. ✔MEASURED
    // by the red-on-disable: a mutant that changed the accessor left every test
    // GREEN. One reader, or a test can only ever prove something about the
    // reader it happens to share.
    fs::path const root =
        (executableDir / fs::path{std::string{installedConfigRelDir()}})
            .lexically_normal();
    if (!fs::is_directory(root, ec)) return std::nullopt;
    return root;
}

LoadResult<std::filesystem::path>
findShippedConfig(ShippedConfigLocator const& loc) {
    // ── `locate-config`, the precedence walk's own row in `--time` ──────────
    // This is the ONE thing a shipped load pays that the content-addressed
    // memo can never remove: the walk's answer depends on cwd and on
    // `DSS_CONFIG_ROOT`, both of which a single process may change (the test
    // harness does), so a resolved path is not a memoizable fact about a NAME.
    // ✔MEASURED 2026-08-25: ~2 ms across the 13 grammar lookups of a one-line
    // Debug C compile — small, and small is exactly the claim the row now
    // carries evidence for instead of leaving it to `[other]`.
    substrate::PhaseTimers::Scope const locateScope{
        substrate::CompilePhase::LocateConfig};

    // Reject path-like names up front. `loadShipped` is the LOGICAL-
    // name resolver — only `csharp` / `x86_64` / `toy` / ... — never
    // arbitrary paths. Defending against `../` traversal here also
    // covers callers that forward an untrusted name (LSP requests,
    // future driver flags).
    if (loc.name.empty()
        || loc.name.find('/')  != std::string_view::npos
        || loc.name.find('\\') != std::string_view::npos
        || loc.name.front() == '.') {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {loc.invalidNameCode, DiagnosticSeverity::Error,
             std::string{loc.name},
             std::string{"invalid shipped-"} + std::string{loc.kindLabel} + " name"}});
    }

    std::string const leaf = std::string{loc.name} + std::string{loc.suffix};
    auto const        r = resolveByPrecedence(loc.subdir, leaf, std::nullopt, &probeExists);

    // A version-skewed tree is a REFUSAL, never a fall-through to some other
    // tree: silently answering out of a different tree than the one the operator
    // pointed at is precisely the confusion this reports instead.
    if (r.refusal.has_value()) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {loc.invalidNameCode, DiagnosticSeverity::Error,
             std::string{loc.name}, *r.refusal}});
    }
    if (r.hit.has_value()) return *r.hit;

    return std::unexpected(std::vector<ConfigDiagnostic>{
        {loc.invalidNameCode, DiagnosticSeverity::Error,
         std::string{loc.name},
         std::string{"no shipped "} + std::string{loc.kindLabel}
             + " config found for '" + std::string{loc.name} + "'."
             + renderTried(r.tried)}});
}

std::optional<std::filesystem::path>
findShippedConfigDir(std::string_view                            subdir,
                     std::optional<std::filesystem::path> const& startPath) {
    auto const r = resolveByPrecedence(subdir, /*leaf=*/{}, startPath, &probeIsDirectory);

    // ⚠ A REFUSAL AND A MISS ARE THE SAME ANSWER HERE, AND THAT IS A KNOWN
    // NARROWING RATHER THAN AN OVERSIGHT. This form returns `std::optional`
    // BECAUSE every caller already owns a distinct not-found behaviour (fall
    // through to another tier / report "not located" / skip the dir so the miss
    // fails loud downstream) — none of them would forward a diagnostic
    // manufactured here, and widening the signature would ripple into four files
    // owned by other work. The skew EXPLANATION therefore comes from the file
    // form, which every compilation reaches first (the driver resolves the
    // language schema through `findShippedConfig` before any directory lookup).
    // What matters for correctness holds either way: a skewed tree is never
    // USED, and because both forms run the SAME precedence they cannot end up
    // reading different trees.
    if (r.refusal.has_value()) return std::nullopt;
    return r.hit;

    // NOT merged with `findShippedConfig` into one "find the root, then probe"
    // helper at the PUBLIC level, and that is a semantic decision rather than
    // duplication left standing: the file form must CONTINUE walking past an
    // ancestor that has a `src/dss-config/<subdir>` but not the requested leaf,
    // while this form is done the moment the directory itself resolves. The
    // shared `resolveByPrecedence` preserves exactly that — it probes the
    // COMPOSED path (root+subdir+leaf, or root+subdir) at every candidate, so
    // each form keeps its own reach while the ORDER of the arms lives once.
}

// THE RESOLVED SYSTEM-INCLUDE DIRS — see the header for why this is the one
// owner and what the three drifted copies did.
//
// ★ THE ANSWER IS COMPUTED ONCE PER CALL AND IS MEANT TO BE HOISTED BY THE
// CALLER. Every entry costs a `findShippedConfigDir`, which reads the
// environment and probes up to eight ancestor directories. The driver's
// multi-TU build resolves ONCE for a whole `CuBuildKey` and hands the vector to
// each unit rather than re-walking per source file — the answer is fixed for an
// invocation (a language's `shippedLibDirs` and the config root are both), and
// hoisting it is also one less piece of ambient filesystem state consulted from
// inside a concurrent job.
std::vector<std::filesystem::path> resolveSystemDirs(GrammarSchema const& grammar) {
    std::vector<std::filesystem::path> out;
    auto const&                        dirs = grammar.semantics().shippedLibDirs;
    if (dirs.empty()) return out;
    out.reserve(dirs.size());
    std::error_code ec;
    for (std::string const& sub : dirs) {
        auto const resolved = findShippedConfigDir(sub);
        if (!resolved) continue;   // fail loud downstream, not here
        // Same idiom as the driver's `applyIncludeDirs`: on an `absolute`
        // failure keep the RAW path rather than drop the dir — a dir the
        // filesystem could not canonicalise is still more useful to the
        // resolver than no dir at all.
        // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: and the SAME idiom
        // means the same defect — this inherited a bare `absolute` that re-roots
        // a schema dir on a UNC share onto the local drive, so a shipped config
        // served from `\\host\share` resolved to nothing. Both sites now go
        // through the one helper.
        std::filesystem::path const abs = core::absoluteKeepingRoot(*resolved, ec);
        out.push_back(ec ? *resolved : abs);
        ec.clear();
    }
    return out;
}

} // namespace dss
