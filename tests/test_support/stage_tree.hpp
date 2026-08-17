#pragma once

// Recursive corpus staging — the ONE copy, shared by both corpus runners.
//
// ★★ WHY THIS FILE EXISTS: IT IS A HOIST, AND THE HOIST DELETED A PIN.
// `stageExampleTree` + `stageExampleTreeSelfTest` shipped DUPLICATED VERBATIM in
// `tests/examples/examples_runner.cpp` and `integrated_tests/runner.cpp` —
// ✔MEASURED 14,765 bytes between the twin markers (14,920 with the marker
// lines themselves), `cmp`-identical across the two files — and the
// duplication was held together by a run-time lint
// (`ExamplesCorpusLint.StagingTwinsAreCharacterIdentical`) that read both source
// files off disk and reddened on the first differing byte. That lint was a good
// guard over a bad situation, and it is DELETED by this hoist rather than kept:
// once there is one definition it can never fail again, and a pin that cannot
// fail is worse than no pin because it reads as coverage. What replaces it is
// `ExamplesCorpusLint.StagingPrimitiveLivesOnlyInTheSharedHeader`, which pins
// the property that actually can regress — a copy coming BACK into a runner.
//
// ★ THE MOVE COST NOTHING IN THE BUILD GRAPH, and that was verified rather than
// assumed. `tests/examples/CMakeLists.txt` and `integrated_tests/CMakeLists.txt`
// BOTH already put `${CMAKE_SOURCE_DIR}/tests/test_support` on their include
// paths — this directory is already the shared home of `arm_verdict_ledger.hpp`
// for exactly this pair of runners — so ZERO CMake change was needed. The block
// deliberately references nothing but `std::filesystem` / `std::string` /
// `std::error_code` / `std::optional` / `<fstream>`, so it lifted verbatim; the
// only edit was moving the `namespace fs` alias inside this header's namespace.
// That matters beyond convenience: the two runners are different binaries with
// different link sets (`dss_examples_runner` links dss-code-prime-lib + GTest;
// `integrated_tests` links nlohmann_json alone and drives the compiler as a
// SUBPROCESS), so a shared home that needed either of those would not be shared.
//
// ⚠ INCLUDED BY RELATIVE PATH by both consumers, for the reason
// `arm_verdict_ledger.hpp` states in its own header: `integrated_tests` carries
// ONLY `tests/test_support` on its include path — no `src/` — because it links
// no DSS library at all.

#include <filesystem>
#include <fstream>
#include <iterator>      // std::istreambuf_iterator (do not rely on a
                         // transitive include from <fstream>)
#include <optional>
#include <string>
#include <system_error>  // std::error_code (do not rely on a transitive
                         // include from <filesystem>)

namespace dss::test_support {

namespace fs = std::filesystem;

// The corpus neighbour-staging primitive. Mirrors `srcDir` into `dstDir`
// RECURSIVELY, preserving every relative subpath; returns an empty string on
// success, otherwise ONE sentence naming the path that failed.
//
// ★ WHY RECURSIVE. An example whose dependency is a nested project
// (`<example>/dep_module/.dss-project.json` plus its sources) got NOTHING under
// `dep_module/` into the scratch tree under the previous walk: it used
// `fs::directory_iterator` and `continue`d on every entry that was not a
// regular file, so a subdirectory was skipped whole and in silence. The example
// then died on a missing-file error naming the MANIFEST, sending the reader to
// the one file that was correct. ✔MEASURED before that change:
// `find examples -mindepth 3 -type d` returns 0 over the 581-manifest corpus,
// so every example that exists today stages exactly the same bytes as it did
// before — the recursion is reachable only by the nested shape it was added
// for, and is not a silent widening of 581 green tests.
//
// ★ WHY error_code ON EVERY CALL, INCLUDING THE WALK'S OWN INCREMENT
// (D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT): one of the two callers is a
// plain `main()` with no enclosing `try`, where a throwing `copy_file` ends the
// process with `libc++abi: terminating` as the WHOLE output — no example name,
// no path, no Results line. `recursive_directory_iterator`'s range-for form
// hides a THROWING `operator++`, which is a second and far less obvious way
// into that same state, so the loop below is written long-hand.
//
// ★ WHY NOTHING IS EVER SKIPPED QUIETLY. A silently dropped input is the exact
// defect this function exists to end, so an entry that is neither a directory
// nor a regular file is a LOUD error naming it, not a `continue`. Git tracks
// only regular files, symlinks and directories, so that branch is unreachable
// for a committed example and fires only on a broken symlink or a stray device
// node — both of which the reader needs told about rather than staged around.
// The SYMLINKED DIRECTORY is the subtle member of that family and is refused by
// name below: `is_directory` follows a link while the walk (correctly) does not
// descend through one, so the obvious code stages a hollow directory and loses
// everything under it. ✔MEASURED on this tree: `git ls-files -s examples/` lists
// no mode-120000 entry and `find examples -type l` is empty, with
// `core.symlinks=true` — so the corpus cannot hit this today, but nothing stops
// the next example from shipping one, and it would fail in exactly the silent
// way this whole change exists to abolish.
//
// ★ WHY ONLY THE TOP-LEVEL `expected.json` IS EXCLUDED. That file is the
// HARNESS's input, not the example's, and both runners locate it at exactly
// `exampleDir/"expected.json"`. Matching on the bare FILENAME instead of the
// relative path would drop an example's own data file that happens to carry
// that name one directory down — re-creating this very defect at depth.
// examples/README.md documents that contract on the corpus side.
[[nodiscard]] inline std::string stageExampleTree(fs::path const& srcDir,
                                                  fs::path const& dstDir) {
    std::error_code ec;
    fs::recursive_directory_iterator it{srcDir, fs::directory_options::none,
                                        ec};
    fs::recursive_directory_iterator const walkEnd{};
    // The `ec` test comes BEFORE the end test deliberately: a failed
    // `increment` is permitted to leave the iterator EQUAL to end(), so a loop
    // that asked "am I finished?" first would read a walk error as a completed
    // walk and report success over a tree it never got to the bottom of.
    while (true) {
        if (ec) {
            return "cannot walk the example directory '"
                 + srcDir.generic_string() + "': " + ec.message();
        }
        if (it == walkEnd) break;

        fs::path const from = it->path();
        // LEXICALLY relative, not `fs::relative`: the latter runs both paths
        // through `weakly_canonical`, which touches the filesystem and RESOLVES
        // SYMLINKS — so a repo reached through a symlinked path would have its
        // subpaths rewritten against the resolved tree. The walk guarantees
        // `from` is literally `srcDir` plus components, so the lexical answer is
        // the exact one, needs no syscall, and cannot fail for an I/O reason.
        fs::path const rel = from.lexically_relative(srcDir);
        if (rel.empty()) {
            return "cannot express '" + from.generic_string()
                 + "' relative to '" + srcDir.generic_string()
                 + "': the walk yielded an entry outside the tree it is walking";
        }
        fs::path const to = dstDir / rel;

        bool const isDir = it->is_directory(ec);
        if (ec) {
            return "cannot stat '" + from.generic_string() + "': "
                 + ec.message();
        }
        if (isDir) {
            bool const isLink = it->is_symlink(ec);
            if (ec) {
                return "cannot stat '" + from.generic_string() + "': "
                     + ec.message();
            }
            if (isLink) {
                return "refusing to stage the SYMLINKED directory '"
                     + from.generic_string()
                     + "': the walk deliberately does not descend through a"
                       " symlink (a cycle would never terminate), so staging"
                       " it would create an EMPTY directory here and drop"
                       " everything under it without a word";
            }
            // Created EAGERLY rather than on demand from the first file found
            // inside it, so that an example may ship an intentionally EMPTY
            // directory (an output dir its program writes into) and still find
            // it waiting in the scratch tree.
            fs::create_directories(to, ec);
            if (ec) {
                return "cannot create the staged directory '"
                     + to.generic_string() + "': " + ec.message();
            }
            it.increment(ec);
            continue;
        }

        bool const isFile = it->is_regular_file(ec);
        if (ec) {
            return "cannot stat '" + from.generic_string() + "': "
                 + ec.message();
        }
        if (!isFile) {
            return "refusing to silently skip '" + from.generic_string()
                 + "': it is neither a directory nor a regular file, and a"
                   " dropped input is the confusing missing-file error this"
                   " staging exists to prevent";
        }
        if (rel.generic_string() == "expected.json") {
            it.increment(ec);
            continue;
        }

        fs::create_directories(to.parent_path(), ec);
        if (ec) {
            return "cannot create the staged directory '"
                 + to.parent_path().generic_string() + "': " + ec.message();
        }
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return "cannot stage '" + rel.generic_string() + "' into '"
                 + dstDir.generic_string() + "': " + ec.message();
        }
        it.increment(ec);
    }
    return {};
}

// The behavioural pin for the primitive above, and it lives HERE rather than in
// either runner because both harnesses must execute the IDENTICAL checks — a
// capability tested in one and merely present in the other is the silent
// harness bug this pair keeps producing. Written as a plain function returning
// findings rather than as GTest assertions because the CLI-subprocess runner
// has no GTest link and reports through its own `check()`.
//
// Every assertion compares BYTES read back off disk, never mere existence: a
// zero-byte file at the right relative path is precisely the outcome a weaker
// pin would call success.
[[nodiscard]] inline std::string stageExampleTreeSelfTest(fs::path const& sandbox) {
    struct Planted {
        char const* rel;
        char const* body;
    };
    // `dep_module/…` is the AP6 shape this whole change was made for; the
    // two-levels-down `src/lib.c` proves the walk descends further than one
    // level; and `data/expected.json` proves the manifest exclusion is keyed on
    // the RELATIVE PATH rather than on the filename.
    static constexpr Planted kPlanted[] = {
        {"main.c",
         "int main(void) { return 42; }\n"},
        {"dep_module/.dss-project.json",
         "{\"language\":\"c-subset\",\"sources\":[\"src/*.c\"]}\n"},
        {"dep_module/src/lib.c",
         "int dep_answer(void) { return 7; }\n"},
        {"dep_module/data/expected.json",
         "{\"note\":\"example DATA two levels down, not the manifest\"}\n"},
    };
    static constexpr char const* kManifestBody =
        "{\"language\":\"c-subset\",\"source\":\"main.c\",\"exitCode\":42}\n";

    std::string findings;
    std::size_t findingCount = 0;
    auto const fail = [&findings, &findingCount](std::string const& what) {
        ++findingCount;
        findings += "\n    - " + what;
    };

    std::error_code ec;
    fs::path const src = sandbox / "example";
    fs::path const dst = sandbox / "staged";
    // A rerun must not inherit a stale tree, or a file left behind by the
    // PREVIOUS run would satisfy an assertion this run's staging never met.
    fs::remove_all(sandbox, ec);
    if (ec) {
        return "self-test sandbox '" + sandbox.generic_string()
             + "' could not be cleared: " + ec.message();
    }

    auto const plant = [&](char const* rel, char const* body) -> bool {
        fs::path const p = src / rel;
        std::error_code mkEc;
        fs::create_directories(p.parent_path(), mkEc);
        if (mkEc) {
            fail("cannot create fixture directory '"
                 + p.parent_path().generic_string() + "': " + mkEc.message());
            return false;
        }
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << body;
        out.close();
        if (!out) {
            fail("cannot write fixture file '" + p.generic_string() + "'");
            return false;
        }
        return true;
    };
    auto const slurp = [](fs::path const& p) -> std::optional<std::string> {
        std::ifstream in(p, std::ios::binary);
        if (!in) return std::nullopt;
        return std::string{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>{}};
    };

    for (auto const& p : kPlanted) {
        if (!plant(p.rel, p.body)) {
            return "the staging self-test could not build its own fixture:"
                 + findings;
        }
    }
    if (!plant("expected.json", kManifestBody)) {
        return "the staging self-test could not build its own fixture:"
             + findings;
    }
    fs::create_directories(dst, ec);
    if (ec) {
        return "self-test staging target '" + dst.generic_string()
             + "' could not be created: " + ec.message();
    }

    if (std::string const err = stageExampleTree(src, dst); !err.empty()) {
        fail("staging a nested fixture reported: " + err);
    }
    for (auto const& p : kPlanted) {
        fs::path const landed = dst / p.rel;
        auto const got = slurp(landed);
        if (!got.has_value()) {
            fail(std::string{"'"} + p.rel + "' never reached the staged tree"
                 " (expected it at '" + landed.generic_string() + "')");
        } else if (*got != std::string{p.body}) {
            // Both texts are QUOTED, not just their sizes: a same-length
            // corruption would otherwise report "wanted 35, read 35" and send
            // the reader looking for a bug in this message instead of in the
            // staging. Right-path/wrong-content is precisely the outcome a
            // mere existence check calls success, so its diagnostic has to be
            // the strongest one here.
            fail(std::string{"'"} + p.rel + "' reached '"
                 + landed.generic_string() + "' with the WRONG BYTES ("
                 + std::to_string(got->size()) + " read vs "
                 + std::to_string(std::string{p.body}.size()) + " wanted)\n"
                   "        wanted: " + std::string{p.body}
                 + "        read:   " + *got);
        }
    }
    if (slurp(dst / "expected.json").has_value()) {
        fail("the TOP-LEVEL manifest 'expected.json' was staged; it is the"
             " harness's own input, and a scratch copy of it would shadow the"
             " file the runner actually parsed");
    }

    // The fail-loud half. An unreadable source tree must produce a NAMED error
    // rather than a quiet success over zero entries — handing the caller an
    // empty scratch dir and telling it everything went fine is exactly how the
    // confusing missing-file error class comes back.
    fs::path const absent = sandbox / "no-such-example-directory";
    std::string const absentErr = stageExampleTree(absent, dst);
    if (absentErr.empty()) {
        fail("staging a MISSING source directory reported SUCCESS; an empty"
             " stage that claims to have worked is the silent failure this"
             " primitive must never produce");
    } else if (absentErr.find(absent.generic_string()) == std::string::npos) {
        fail("staging a MISSING source directory failed without NAMING it,"
             " which is the half of the diagnostic the reader needs: "
             + absentErr);
    }

    if (findingCount == 0) return {};
    return "recursive neighbour staging is broken — "
         + std::to_string(findingCount) + " finding(s):" + findings;
}

}  // namespace dss::test_support
