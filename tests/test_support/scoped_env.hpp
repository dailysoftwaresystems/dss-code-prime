#pragma once

#include <cstdlib>
#include <string>

// Portable RAII environment-variable override — hoisted at
// `D-TEST-SCOPED-ENV-DUPLICATED-THREE-WAYS` closure (2026-08-13), mirroring the
// `scratch_dir.hpp` hoist that preceded it.
//
// ★★ THE ROW'S COUNT WAS WRONG, AND IT ERRED LOW. It said THREE copies
// (`tests/core/test_config_path_walk.cpp`, `tests/test_support/test_repo_root.cpp`,
// `tests/lsp/test_workspace_project.cpp`). ✔MEASURED with
// `grep -rn 'class ScopedEnv' tests/`: there were FIVE. The two the row never
// counted are `tests/program/test_asm_dialect_per_target.cpp` and
// `tests/program/test_system_dirs_cwd_independent.cpp`. A duplication row that
// undercounts its own subject is the same instrument failure this tree has been
// burned by before — enumerate with a matcher, never from memory.
// ✅ ALL FIVE are now repointed here (`D-TEST-SCOPED-ENV-STRAGGLERS-IN-TESTS-
// PROGRAM`, 2026-08-13) — the two above were the last, and the census in
// `test_scoped_env.cpp` now asserts this file is the ONLY definition under
// `tests/`, with the walk carrying its own positive control so an all-clear
// cannot come from a walk that read nothing.
//
// ★★ AND THE COPIES CARRIED A FALSE REASON TO STAY COPIES. Two of them said
// they were "kept LOCAL rather than hoisted ... so a test that mutates the
// environment cannot leak that mutation into a sibling binary's expectations".
// That is not a reason, it is a non-sequitur: every test binary is its own
// PROCESS with its own environment block, so sharing a HEADER cannot leak a
// runtime mutation anywhere. The restore is what prevents the leak, and the
// restore is byte-identical in all five. A wrong rationale is worse than none —
// each new author read it, believed it, and wrote a sixth copy would have been
// next. It is recorded here so the refutation outlives the comments it refutes.
//
// ── THE UNION API ────────────────────────────────────────────────────────────
// The five copies differed only in which constructors they carried; both are
// kept, because both have live callers:
//   * `ScopedEnv(name, value)` — override, restore on scope exit.
//   * `ScopedEnv(name)`        — construct-to-CLEAR. The tests that must prove a
//     candidate is ABSENT rather than merely wrong need this: setting the
//     variable to "" is not the same experiment as unsetting it.
// Restore semantics (identical in every prior copy, preserved here): if the
// variable existed on entry its EXACT prior value is put back; if it did not
// exist, it is removed again — never left behind as an empty string, which is a
// third state neither caller means.
//
// Windows: `_putenv_s(name, "")` REMOVES the variable (documented), which is
// why `clear()` needs no separate spelling there.

namespace dss::test_support {

class ScopedEnv {
public:
    ScopedEnv(char const* name, std::string const& value) : name_(name) {
        capture();
        set(value);
    }

    explicit ScopedEnv(char const* name) : name_(name) {
        capture();
        clear();
    }

    ~ScopedEnv() { had_ ? set(prev_) : clear(); }

    ScopedEnv(ScopedEnv const&)            = delete;
    ScopedEnv& operator=(ScopedEnv const&) = delete;
    ScopedEnv(ScopedEnv&&)                 = delete;
    ScopedEnv& operator=(ScopedEnv&&)      = delete;

private:
    void capture() {
        if (char const* prev = std::getenv(name_.c_str())) {
            had_  = true;
            prev_ = prev;
        }
    }

    void set(std::string const& v) {
#ifdef _WIN32
        ::_putenv_s(name_.c_str(), v.c_str());
#else
        ::setenv(name_.c_str(), v.c_str(), /*overwrite=*/1);
#endif
    }

    void clear() {
#ifdef _WIN32
        ::_putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    bool        had_ = false;
    std::string prev_;
};

} // namespace dss::test_support
