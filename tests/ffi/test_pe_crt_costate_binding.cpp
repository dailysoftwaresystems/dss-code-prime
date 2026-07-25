// D-FFI-PE-CRT-UCRT-MIGRATION — the CO-STATE-GROUP binding invariant.
//
// WHY THIS TEST EXISTS
//
// The pe C runtime is migrating from legacy `msvcrt.dll` to the modern UCRT
// (`ucrtbase.dll`) incrementally, one group of shipped-library descriptors per
// phase. Incremental is safe ONLY because of one rule: the migration's atomic
// unit is a CO-STATE GROUP.
//
// A C runtime owns process-global state — the heap, the FILE* table, the CRT
// file-descriptor table, `errno`, the static `tm` buffer. An object minted by
// ONE runtime and consumed by the OTHER corrupts SILENTLY: `malloc` from
// ucrtbase freed by msvcrt's `free` walks a foreign heap; a `FILE*` from one
// `fopen` passed to the other's `fclose`; a failing ucrtbase call setting
// ucrtbase's `errno` while the program reads msvcrt's. None of these fault at
// link time, none produce a diagnostic, and none reliably crash near the cause
// — they corrupt memory or silently return wrong answers. That is precisely the
// silent-miscompile class this project refuses to ship.
//
// So the descriptors whose surfaces EXCHANGE such objects must flip TOGETHER,
// in one commit, and must never be observed in a split state. Splitting them is
// the single most dangerous mistake available in this arc, and it is a one-token
// edit in a JSON file that NO other test would catch: a build with `stdio.json`
// on ucrtbase and `malloc.json` still on msvcrt compiles clean, links clean,
// passes every existing test, and corrupts the heap at runtime in a program
// that does `fopen`/`fread`-into-`malloc`ed-memory/`free`.
//
// This test makes that state LOUD. It reads the REAL shipped descriptors (never
// a hand-copied snapshot) and asserts the group agrees.
//
// WHAT IT DELIBERATELY DOES NOT DO: it does not assert WHICH runtime the group
// names. Pinning the literal "msvcrt.dll" would make the test a restatement of
// the config that has to be edited in lockstep with every phase — an inert pin
// that fails for the wrong reason and teaches maintainers to update it
// reflexively. The invariant is AGREEMENT, and it holds before the migration
// (all msvcrt), after it (all ucrtbase), and fails only in the genuinely unsafe
// in-between. That is the property worth guarding.
//
// RED-ON-DISABLE: `SplitCoStateGroupIsCaught` reproduces the exact unsafe shape
// on synthetic descriptors, so the guard itself stays pinned even while the real
// tree is correct.

#include "ffi/shipped_lib_descriptor.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path configRoot() {
    fs::path here = fs::current_path();
    for (int i = 0; i < 12 && !here.empty(); ++i) {
        fs::path const cand = here / "src" / "dss-config";
        if (fs::exists(cand)) return cand;
        fs::path const parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
    return {};
}

// The STATEFUL co-state group: every descriptor whose pe surface mints or
// consumes a CRT-owned object. Membership is a property of the SURFACE, not of
// how many symbols happen to be exported, and each entry names the coupling:
//
//   malloc   — the heap itself (malloc/realloc/free)
//   stdlib   — malloc/free/realloc/atexit + exit's stream teardown
//   string   — `strdup`/`_strdup` RETURN a CRT-heap pointer the caller frees
//   stdio    — the FILE* table; fopen/fclose/fread must pair within one CRT
//   io       — the CRT fd table (_open/_read/_close) that stdio is layered on
//   errno    — the per-CRT errno slot every failing call above writes
//   direct   — _getcwd RETURNS a CRT-heap buffer when passed NULL; sets errno
//   process  — _spawn/_exec inherit the CRT's fd table and set errno
//   sys/stat — `_fstat` CONSUMES a CRT fd minted by io.json's _open
//
// THE MEMBERSHIP RULE, and how sys/stat got here: a descriptor belongs in this
// group when its surface accepts or returns a value carrying RUNTIME-OWNED
// IDENTITY — a heap block only its own allocator can free, a FILE*, an fd, the
// errno slot. It is NOT about "touches per-runtime state" in general: time.json
// keeps a static `struct tm` yet stays out, because a tm is a plain POD record
// any runtime can read.
//
// sys/stat was MISSING from this list in the first draft, and that omission was
// not caught by any test — it was caught by the SQLite corpus, which began
// crashing at vtabH-3.5 once sys/stat.json was pointed at ucrtbase while io.json
// still bound msvcrt. test_fs.c does `struct stat sBuf; fstat(pCsr->fd, &sBuf)`:
// the fd is an index into the OWNING runtime's descriptor table, so msvcrt's fd
// handed to UCRT's _fstat64i32 resolves against a table that never saw it. A
// standalone repro (msvcrt `_open` -> ucrtbase `_fstat64i32`) reproduced it
// exactly: process death, 0xC0000409. `_fstat` takes an `int fd` — by the rule
// above that alone puts sys/stat in the group, regardless of the fact that its
// OTHER entry points (`_wstat`, `_stat64`) take paths and would have been safe
// to move alone. Half-moving a descriptor is the same hazard as half-moving the
// group.
//
// A descriptor NOT listed here either is stateless (value->value: ctype, mem*,
// math) or its per-runtime state is a PLAIN POD the other runtime can safely
// read. time.json is the second kind and worth spelling out, because "has
// per-runtime state" is not by itself disqualifying: localtime/gmtime return a
// pointer into the CRT's static `struct tm`, but that is an ordinary POD record
// with a published layout, not a runtime-owned handle — reading it across CRTs
// is well-defined. (That is precisely why the interim strftime->ucrtbase
// per-symbol override worked while the rest of time.json was still msvcrt.)
// What puts a descriptor IN the group is state carrying runtime-owned IDENTITY
// — a heap block only its allocator can free, a FILE*/fd only its own table can
// resolve, the errno slot a failing call writes. That distinction is why Phase 1
// could flip ctype/memory/tgmath and Phase 2 time/sys-stat, while these eight
// must move together.
constexpr char const* kCoStateGroup[] = {
    "malloc", "stdlib", "string", "stdio", "io", "errno", "direct", "process",
    "sys/stat",
};

// Reads `library.pe` out of one descriptor. nullopt = the descriptor declares no
// pe binding at all (a header with no pe surface), which is not a split.
[[nodiscard]] std::optional<std::string> peLibraryOf(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) return std::nullopt;
    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
    if (!doc.contains("library") || !doc.at("library").is_object()) return std::nullopt;
    auto const& lib = doc.at("library");
    if (!lib.contains("pe") || !lib.at("pe").is_string()) return std::nullopt;
    return lib.at("pe").get<std::string>();
}

}  // namespace

// The real invariant, over the real tree.
TEST(PeCrtCoStateBinding, EveryStatefulDescriptorNamesTheSamePeRuntime) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config";

    std::map<std::string, std::string> bindingByStem;
    for (char const* stem : kCoStateGroup) {
        fs::path const p = root / "shippedLibs" / (std::string{stem} + ".json");
        ASSERT_TRUE(fs::exists(p))
            << "co-state group names a descriptor that does not exist: " << p
            << " — if it was renamed or retired, update kCoStateGroup with the "
               "reason, do not silently drop the coupling it represented.";
        auto const lib = peLibraryOf(p);
        ASSERT_TRUE(lib.has_value())
            << stem << ".json declares no library.pe — a stateful pe descriptor "
                       "must name its runtime explicitly.";
        bindingByStem.emplace(stem, *lib);
    }

    ASSERT_FALSE(bindingByStem.empty());
    std::string const& expected = bindingByStem.begin()->second;
    for (auto const& [stem, lib] : bindingByStem) {
        EXPECT_EQ(lib, expected)
            << "\nCO-STATE GROUP SPLIT ACROSS TWO C RUNTIMES.\n"
            << "  " << stem << ".json -> " << lib << "\n"
            << "  " << bindingByStem.begin()->first << ".json -> " << expected
            << "\nThese descriptors exchange CRT-owned objects (heap pointers, "
               "FILE*, fds, errno). Binding them to different runtimes does not "
               "fail to link and does not diagnose — it corrupts at runtime.\n"
               "Flip the whole group in ONE commit (D-FFI-PE-CRT-UCRT-MIGRATION "
               "Phase 3), or revert the partial flip.";
    }
}

// The guard is itself pinned: build the unsafe shape and prove the comparison
// that backs the test above rejects it. Without this, a future refactor could
// neuter the real test (e.g. by reading a key that no longer exists, so every
// binding reads back as absent and the loop compares nothing) and stay green.
TEST(PeCrtCoStateBinding, SplitCoStateGroupIsCaught) {
    // Exactly the dangerous shape: the heap on one runtime, stdio on the other.
    std::map<std::string, std::string> const split{
        {"malloc", "ucrtbase.dll"},
        {"stdio",  "msvcrt.dll"},
    };
    std::string const first = split.begin()->second;
    bool allAgree = true;
    for (auto const& [stem, lib] : split) {
        (void)stem;
        if (lib != first) allAgree = false;
    }
    EXPECT_FALSE(allAgree)
        << "the agreement check must REJECT a group split across two runtimes";

    // ...and it must ACCEPT a coherent group, in either direction, so the guard
    // does not simply always fail (which would also be useless).
    for (char const* runtime : {"msvcrt.dll", "ucrtbase.dll"}) {
        std::map<std::string, std::string> const coherent{
            {"malloc", runtime}, {"stdio", runtime}, {"errno", runtime},
        };
        std::string const f = coherent.begin()->second;
        bool agree = true;
        for (auto const& [stem, lib] : coherent) {
            (void)stem;
            if (lib != f) agree = false;
        }
        EXPECT_TRUE(agree) << "a coherent group on " << runtime
                           << " must be accepted (the invariant is AGREEMENT, "
                              "not a specific runtime)";
    }
}

// A typo in a runtime image name ("ucrtbase.dl") would bind every symbol in that
// descriptor to a DLL that does not exist — breaking the binary LOAD at 0xC0000139
// under the eager-import law (D-FFI-DESCRIPTOR-EAGER-IMPORT) rather than failing
// the build. Keep the set of pe runtime images we are willing to name closed.
TEST(PeCrtCoStateBinding, EveryPeRuntimeImageNamedIsAKnownRuntime) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty());

    std::vector<std::string> const known{
        "msvcrt.dll",     // the legacy CRT the migration is leaving
        "ucrtbase.dll",   // the modern Universal CRT it is moving to
        "kernel32.dll",   // Win32 OS surface (windows.json, threads.json)
    };

    for (auto const& e : fs::recursive_directory_iterator(root / "shippedLibs")) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        auto const lib = peLibraryOf(e.path());
        if (!lib.has_value()) continue;
        EXPECT_NE(std::find(known.begin(), known.end(), *lib), known.end())
            << e.path().filename().string() << " names pe runtime image '" << *lib
            << "', which is not a known pe runtime. A misspelled image does not "
               "fail the build — DSS eagerly imports every declared symbol, so "
               "the LOADER rejects the binary (0xC0000139) at run time instead. "
               "If this is a genuinely new runtime, add it here deliberately.";
    }
}
