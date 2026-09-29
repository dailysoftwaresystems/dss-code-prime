#pragma once

// ── WHERE A SPAWNED CORPUS ARM'S LOADER LOOKS FOR THE LIBRARIES IT WAS BUILT
//    AGAINST — shared by BOTH corpus runners ─────────────────────────────────
//
// ★★★ THE DEFECT THIS EXISTS FOR. An executable that imports from a DYNAMIC
// library needs the platform's loader to FIND that library when it runs. The
// corpus builds such a library as a `dependsOn` prerequisite, into the SAME
// directory as the executable. On Windows that is enough: the loader searches
// the application's own directory first, so every pe64 arm of a dynamic-library
// example has always RUN. On Linux it is not: DSS records the library in
// DT_NEEDED by name and — exactly like gcc and clang when no `-rpath` is asked
// for — emits no DT_RUNPATH, so the loader searches only its default paths and
// the child dies at exit 127 before `main`. Mach-O is the same story through
// `@rpath` with no LC_RPATH. Neither runner told the loader anything, so every
// ELF and Mach-O arm of those examples was declared `runOn: []` — BUILD-ONLY —
// and the optimized arm those manifests declare was built on NO host at all.
//
// ⇒ A TARGET MAY DECLARE `loaderSearchPathVariable`: the NAME of the environment
// variable through which its host's loader accepts extra library directories.
// The runner sets it, for the one spawn and nothing else, to the directory of
// every library the build resolved against — the SAME set the build handed to
// `--resolve-library` — ahead of any value the variable already had.
//
// ★ WHY A DECLARATION AND NOT A HOST TABLE. The variable is a property of the
// loader that runs the child, and the manifest is already where the corpus says
// HOW a target is spawned: `runOn` names the host, `emulator` names the
// launcher. A table in the runners ({linux: LD_LIBRARY_PATH, darwin: ...}) would
// be the one place in either harness that spelled a platform's loader, and it
// would change the spawn environment of every example rather than of the
// targets that asked. So the runners stay agnostic — nothing here or at either
// call site names a loader, a variable, an OS or an object format — and a
// target that needs the loader told declares so, in its own words.
//
// ★ WHY THE RESOLVED LIBRARIES' DIRECTORIES AND NOTHING WIDER. That is what
// `-L <dir> -l<name>` followed by `LD_LIBRARY_PATH=<dir>` means with gcc, and it
// is the whole of what the build was told: a directory this build never
// resolved against has no business in the child's search path. For a
// `dependsOn` library it is the executable's own directory — the one the
// Windows loader already searches unasked — and for a `prebuiltLibraries` input
// it is that file's directory in the tree.
//
// ★ WHY PREPENDED, NEVER REPLACED. An operator's existing value may be what lets
// the child find something else it needs; dropping it would make the harness
// the reason a correct binary failed to load. Putting the resolved directories
// FIRST is what keeps the example's own libraries from being shadowed by a
// same-named file somewhere the operator's value points.
//
// ⚠ THIS HEADER BELONGS IN `tests/test_support/` beside `run_binary.hpp`, the
// suite's one spawn chokepoint, and the long-term shape is a `runBinary`
// environment parameter rather than a scoped mutation at two call sites. It
// sits beside the in-process runner for one round only, because that directory
// is another lane's this round; it is standard-library-only and self-contained
// so moving it is a rename and an include-path line.
//
// SELF-CONTAINED ON PURPOSE, for the reason every shared corpus-runner header
// is: its two consumers have disjoint link sets (`dss_examples_runner` links
// the compiler, `integrated_tests` links nlohmann_json alone).

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

// The host's PATH-LIST separator — what a search-path variable puts BETWEEN two
// directories. A property of the machine the runner (and so the child) runs
// on, exactly like the separator `findOnPath` splits PATH with.
inline constexpr char kLoaderSearchPathListSeparator =
#if defined(_WIN32)
    ';';
#else
    ':';
#endif

// Is `name` a portable environment-variable NAME — `[A-Za-z_][A-Za-z0-9_]*`?
// Both parsers refuse anything else at LOAD, because a name with a space, an
// `=` or a leading digit is either rejected by the host's setenv or silently
// set under a different name, and either way the child never sees it.
[[nodiscard]] inline bool
isPortableEnvironmentVariableName(std::string_view name) noexcept {
    if (name.empty()) return false;
    auto const alpha = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    if (!alpha(name.front())) return false;
    for (char const c : name) {
        if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

// The value a declared `loaderSearchPathVariable` carries for one spawn: the
// directory of each resolved library in RESOLVE ORDER, each directory ONCE,
// joined by the host's list separator, then `priorValue` when non-empty.
//
// Directories are made ABSOLUTE: the child is spawned with its own working
// directory, so a relative entry would be resolved against a directory the
// build never meant. The FILE path is normalised before its directory is taken
// — normalising the directory instead leaves `a/sub/..` as `a/` with a trailing
// separator, which would make one directory two entries.
//
// An EMPTY library list yields `priorValue` unchanged — but both parsers refuse
// the declaration on a target with no library input, so a runner never reaches
// here with one; the answer is defined anyway rather than left to the caller's
// discipline.
[[nodiscard]] inline std::string
loaderSearchPathValue(std::vector<std::filesystem::path> const& resolvedLibraries,
                      std::string_view priorValue) {
    std::string           value;
    std::set<std::string> seen;
    for (auto const& lib : resolvedLibraries) {
        // The non-throwing overload: the throwing one consults the process
        // working directory and ends a run whose cwd has been deleted.
        std::error_code ec;
        auto file = std::filesystem::absolute(lib, ec);
        if (ec) file = lib;
        auto dir = file.lexically_normal().parent_path();
        std::string const spelled = dir.make_preferred().string();
        if (spelled.empty() || !seen.insert(spelled).second) continue;
        if (!value.empty()) value += kLoaderSearchPathListSeparator;
        value += spelled;
    }
    if (!priorValue.empty()) {
        if (!value.empty()) value += kLoaderSearchPathListSeparator;
        value += priorValue;
    }
    return value;
}

}  // namespace dss::test_support
