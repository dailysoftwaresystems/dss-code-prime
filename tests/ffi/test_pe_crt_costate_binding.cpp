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
// ★ THE BLIND SPOT THIS FILE USED TO HAVE (closed TF-C111). Agreement was
// measured over the descriptor ROOT `library.pe` and nothing else. But a
// descriptor's binding is not one value: ANY SYMBOL may carry its own `library`
// map, and the semantic injector MERGES it OVER the descriptor's
// (src/analysis/semantic/semantic_analyzer.cpp:13318-13321 — symbol keys WIN, a
// format the symbol omits inherits the root's entry). So a single JSON line
// inside a symbol object rebinds THAT SYMBOL to a different DLL while the root
// still reads as the group's runtime — and the guard reported agreement. That
// is the same silent cross-CRT hazard the whole file exists to prevent, reached
// through the one door it was not watching. `peBindingsOf` now reads the root
// AND every per-symbol override; `PerSymbolOverrideSplitIsCaught` demonstrates
// the closure by RUNNING the old helper against the shape it used to wave
// through.
//
// RED-ON-DISABLE: `SplitCoStateGroupIsCaught` reproduces the exact unsafe shape
// on synthetic descriptors, so the guard itself stays pinned even while the real
// tree is correct; `PerSymbolOverrideSplitIsCaught` and
// `DeclaredCrossRuntimeSymbolIsAccepted` do the same for the per-symbol axis.

#include "ffi/shipped_lib_descriptor.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// The real descriptor tree this invariant is measured over, resolved through
// the ONE test-side resolver (`repo_root.hpp`: $DSS_CONFIG_ROOT → the
// CMake-baked repo root → the cwd ancestor walk). The private cwd-walk that
// stood here found nothing in an OUT-OF-TREE build, whose cwd has no
// `src/dss-config/` above it — and a co-state group with no tree to read is
// exactly the "no test saw it" hole sys/stat fell through above. Contract
// unchanged: empty on a miss, both callers ASSERT on it; the ADD_FAILURE names
// which of the three lookups came up short.
[[nodiscard]] fs::path configRoot() {
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return {};
    }
    return *root / "src" / "dss-config";
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

// ── AVAILABILITY, mirrored from the engine ───────────────────────────────────
//
// `availableObjectFormats` gates in TWO INDEPENDENT PLACES, and knowing which
// is why this guard may skip some overrides without opening a hole. Both read
// through the SAME predicate, `ffi::objectFormatInAvailabilitySet`
// (src/ffi/shipped_lib_descriptor.cpp:2339-2345), whose rule is "an EMPTY set
// means available on EVERY format":
//
//   * the DESCRIPTOR-level set — decoded at src/ffi/shipped_lib_descriptor.cpp
//     :977-982, enforced at src/analysis/semantic/semantic_analyzer.cpp
//     :13280-13294. A header that excludes the active format does not exist on
//     that target: the `#include` is a hard error and NOTHING is injected.
//   * the per-SYMBOL set — decoded at src/ffi/shipped_lib_descriptor.cpp
//     :1505-1506 (the SAME `decodeShippedAvailability` chokepoint), enforced at
//     src/analysis/semantic/semantic_analyzer.cpp:13359-13362. A symbol that
//     excludes the active format is simply not injected, hence never imported.
//
// The two sets are INDEPENDENT — a per-symbol set does NOT inherit, narrow or
// default to the descriptor's; each is evaluated on its own — but they are
// AND-ed by position, because the header gate runs FIRST and short-circuits. So
// a symbol is reachable on pe only when BOTH admit pe. (Contrast the `library`
// maps, where an omitted per-symbol format DOES inherit the root's. Two
// per-format axes, two different composition rules; do not assume one from the
// other.)
//
// A non-array node is treated as "every format" here because that is what the
// engine ends up doing: `decodeShippedAvailability` reports the malformed shape
// and leaves the vector EMPTY, and an empty set admits every format.
[[nodiscard]] bool admitsPe(nlohmann::json const& node) {
    auto const it = node.find("availableObjectFormats");
    if (it == node.end() || !it->is_array() || it->empty()) return true;
    for (auto const& v : *it)
        if (v.is_string() && v.get<std::string>() == "pe") return true;
    return false;
}

// ONE declared pe runtime image.
struct PeBinding {
    std::string image;
    std::string symbol;       // EMPTY ⇒ the descriptor ROOT
    bool        reachable{};  // survives BOTH pe availability gates
};

// Read a `library.pe` string out of any node that may carry a `library` map —
// the descriptor root or one symbol object. nullopt ⇒ this node declares no pe
// binding of its own (for a symbol that means "inherits the root's").
[[nodiscard]] std::optional<std::string> peImageOf(nlohmann::json const& node) {
    auto const lib = node.find("library");
    if (lib == node.end() || !lib->is_object()) return std::nullopt;
    auto const pe = lib->find("pe");
    if (pe == lib->end() || !pe->is_string()) return std::nullopt;
    return pe->get<std::string>();
}

// EVERY pe runtime image a descriptor declares: the ROOT `library.pe` PLUS every
// per-symbol `library.pe` override, each tagged with whether it can actually
// bind on pe.
//
// ★ THIS IS THE FIX. The pre-fix helper (kept verbatim below as
// `rootOnlyPeLibraryOf`) read the ROOT and nothing else, so an override binding
// one symbol of a stateful surface to the other runtime was structurally
// invisible to every test in this file. The override is not hypothetical
// plumbing: `decodeLibraryMap` validates the per-symbol map through the SAME
// chokepoint as the root's (src/ffi/shipped_lib_descriptor.cpp:1519-1522), it is
// stored raw on `ShippedLibSymbol::library`
// (src/ffi/shipped_lib_descriptor.hpp:193-209), and the semantic injector merges
// it over the descriptor map at
// src/analysis/semantic/semantic_analyzer.cpp:13318-13321. One line inside a
// symbol object rebinds that symbol to a different DLL with no other visible
// change anywhere.
[[nodiscard]] std::vector<PeBinding> peBindingsOf(fs::path const& p) {
    std::vector<PeBinding> out;
    std::ifstream in{p, std::ios::binary};
    if (!in) return out;
    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return out;

    bool const headerAdmitsPe = admitsPe(doc);

    if (auto rootImage = peImageOf(doc))
        out.push_back(PeBinding{std::move(*rootImage), std::string{}, headerAdmitsPe});

    auto const syms = doc.find("symbols");
    if (syms == doc.end() || !syms->is_array()) return out;
    for (auto const& sym : *syms) {
        if (!sym.is_object()) continue;
        auto image = peImageOf(sym);
        if (!image.has_value()) continue;  // no override ⇒ inherits the root
        auto const nameNode = sym.find("name");
        std::string name = (nameNode != sym.end() && nameNode->is_string())
                               ? nameNode->get<std::string>()
                               : std::string{"<unnamed>"};
        out.push_back(PeBinding{std::move(*image), std::move(name),
                                headerAdmitsPe && admitsPe(sym)});
    }
    return out;
}

// THE PRE-FIX HELPER, KEPT VERBATIM AND DELIBERATELY UNUSED BY THE REAL TESTS.
// It exists so `PerSymbolOverrideSplitIsCaught` can demonstrate the blindness it
// closed by RUNNING it against the shape it used to wave through, rather than by
// asserting something about the presence of a symbol. Do not "clean it up":
// deleting it downgrades that red-on-disable from a demonstration to a claim.
//
// Reads `library.pe` out of one descriptor. nullopt = the descriptor declares no
// pe binding at all (a header with no pe surface), which is not a split.
[[nodiscard]] std::optional<std::string> rootOnlyPeLibraryOf(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) return std::nullopt;
    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
    if (!doc.contains("library") || !doc.at("library").is_object()) return std::nullopt;
    auto const& lib = doc.at("library");
    if (!lib.contains("pe") || !lib.at("pe").is_string()) return std::nullopt;
    return lib.at("pe").get<std::string>();
}

// ── DECLARED CROSS-RUNTIME SYMBOLS ───────────────────────────────────────────
//
// A blanket "an override inside the group is always an error" rule would be
// wrong, and this project has already decided the opposite twice: setjmp.json sat
// on msvcrt for two phases because setjmp/longjmp carry ZERO CRT state (setjmp is
// not one of the nine, but the reasoning is the same one), and the interim
// strftime->ucrtbase override rode inside time.json for a whole phase while the
// rest of that header was still msvcrt. BOTH exceptions have since been retired —
// UCRT-P5 moved setjmp.json to ucrtbase once the facility was found there under
// the name `__intrinsic_setjmp` — and that is the point, not a counterexample: an
// exception has to be expressible so the split can be JUSTIFIED AND DATED rather
// than hidden, and then retired when the reason expires. So it has to be
// EXPRESSIBLE —
// but only as an explicit, justified, per-symbol declaration. An UNDECLARED
// override inside the group fails loud.
//
// ★ WHAT JUSTIFIES A GRANT IS A PROPERTY OF THE SURFACE, NOT OF THE RUNTIME IT
// NAMES: the symbol may sit on the other runtime because NO RUNTIME-OWNED
// IDENTITY crosses it — no heap block only its allocator can free, no FILE*, no
// fd, no errno slot. That is exactly the `kCoStateGroup` membership rule applied
// one level down, at symbol granularity. Anything that fails that test does not
// get a grant; it moves with the group or it does not move. Convenience,
// urgency, and "it seems to work" are not grounds.
//
// A grant is a PERMISSION, not an assertion that the override exists right now.
// It stays valid once the override is retired — Phase 3 makes this one redundant
// the moment stdio.json's root reaches ucrtbase.dll — and that is deliberate:
// requiring a grant to be exercised would turn this table into an inert pin that
// must be edited in lockstep with every phase, the exact failure mode the
// no-literal-runtime rule at the top of this file avoids.
//
// The two ways a grant can be wrong are not equally dangerous, which is why only
// one of them needs its own test:
//   * a MIS-SPELLED stem or symbol is SELF-REVEALING. It matches nothing, so the
//     override it was meant to bless is checked for agreement like any other and
//     the suite goes red with the split named.
//   * a grant naming a descriptor OUTSIDE the group is SILENT — it blesses
//     nothing, because nothing outside the group is ever examined, and the
//     author walks away believing an exception is recorded.
//     `EveryDeclaredExceptionNamesAGroupMember` closes that one.
//
// Note what a grant does NOT excuse: naming an image that the runtime does not
// actually export. That failure is loud by construction — DSS eagerly imports
// every declared symbol (D-FFI-DESCRIPTOR-EAGER-IMPORT), so the LOADER rejects
// the binary (0xC0000139) and the runtime witnesses in examples/c-subset fail to
// start. This table is about the SILENT hazard only.
struct DeclaredCrossRuntimeSymbol {
    char const* stem;
    char const* symbol;
    char const* why;
};

constexpr DeclaredCrossRuntimeSymbol kDeclaredCrossRuntimeSymbols[] = {
    {"stdio", "__stdio_common_vsprintf",
     "UCRT's sprintf CORE — the target of stdio.json's compiler-synthesized "
     "`sprintf` shim. ucrtbase.dll exports no concrete `sprintf` at all, so the "
     "recipe must name the core directly, and the core had to be reachable "
     "BEFORE the rest of stdio.json moved. Its entire surface is CALLER-owned: "
     "(_Options, buf, count, fmt, locale=NULL, ap) is a caller buffer, a caller "
     "format string and a caller va_list. It mints no heap block, returns no "
     "FILE*, takes no fd, and touches no errno slot, so no object minted by one "
     "CRT can reach the other across it — the same reasoning that made the "
     "interim strftime->ucrtbase override safe inside an msvcrt <time.h>."},
};

[[nodiscard]] bool isDeclaredCrossRuntime(std::string_view stem,
                                          std::string_view symbol) {
    for (auto const& e : kDeclaredCrossRuntimeSymbols)
        if (stem == e.stem && symbol == e.symbol) return true;
    return false;
}

// ── THE AGREEMENT DECISION ───────────────────────────────────────────────────

// One pe binding, tagged with the descriptor that declared it.
struct GroupBinding {
    std::string stem;
    std::string symbol;  // EMPTY ⇒ the descriptor root
    std::string image;
};

[[nodiscard]] std::string describe(GroupBinding const& b) {
    return b.symbol.empty() ? b.stem + ".json (root)"
                            : b.stem + ".json symbol '" + b.symbol + "'";
}

// THE ONE PREDICATE the real-tree test and every synthetic red-on-disable run,
// so a demonstration can never drift from the thing it demonstrates. nullopt ⇒
// every binding names the same runtime.
[[nodiscard]] std::optional<std::string> findCoStateSplit(
    std::vector<GroupBinding> const& bindings) {
    if (bindings.empty()) return std::nullopt;
    GroupBinding const& first = bindings.front();
    for (auto const& b : bindings) {
        if (b.image == first.image) continue;
        return "CO-STATE GROUP SPLIT ACROSS TWO C RUNTIMES.\n  " + describe(b)
             + " -> " + b.image + "\n  " + describe(first) + " -> " + first.image;
    }
    return std::nullopt;
}

// Everything one descriptor contributes to the agreement check: its root, plus
// every per-symbol override that is REACHABLE on pe and NOT declared.
//
// An override the availability gates make unreachable on pe is dropped, and that
// is not a hole: it can never bind on pe, so it can never mix runtimes there. If
// the gate is later widened the override becomes reachable and this guard picks
// it up at that moment — the check follows the config rather than a snapshot of
// it.
[[nodiscard]] std::vector<GroupBinding> groupBindingsOf(
    std::string const& stem, std::vector<PeBinding> const& declared) {
    std::vector<GroupBinding> out;
    for (auto const& b : declared) {
        if (!b.reachable) continue;
        if (!b.symbol.empty() && isDeclaredCrossRuntime(stem, b.symbol)) continue;
        out.push_back(GroupBinding{stem, b.symbol, b.image});
    }
    return out;
}

// Write a synthetic descriptor. It carries the same keys a shipped one does, so
// the helpers under test reach it by exactly the code path they use on the real
// tree — the point of a red-on-disable is to run the real thing against a bad
// input, not to re-implement the check over a hand-built map.
void writeDescriptor(fs::path const& p, nlohmann::json const& doc) {
    std::ofstream out{p, std::ios::binary};
    ASSERT_TRUE(out.good()) << "could not open synthetic descriptor " << p;
    out << doc.dump(2);
    ASSERT_TRUE(out.good()) << "could not write synthetic descriptor " << p;
}

}  // namespace

// The real invariant, over the real tree.
TEST(PeCrtCoStateBinding, EveryStatefulDescriptorNamesTheSamePeRuntime) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config";

    std::vector<GroupBinding> bindings;
    for (char const* stem : kCoStateGroup) {
        fs::path const p = root / "shippedLibs" / (std::string{stem} + ".json");
        ASSERT_TRUE(fs::exists(p))
            << "co-state group names a descriptor that does not exist: " << p
            << " — if it was renamed or retired, update kCoStateGroup with the "
               "reason, do not silently drop the coupling it represented.";

        std::vector<PeBinding> const declared = peBindingsOf(p);
        bool const hasRoot = std::any_of(
            declared.begin(), declared.end(),
            [](PeBinding const& b) { return b.symbol.empty(); });
        ASSERT_TRUE(hasRoot)
            << stem << ".json declares no library.pe — a stateful pe descriptor "
                       "must name its runtime explicitly.";

        std::vector<GroupBinding> contributed = groupBindingsOf(stem, declared);
        ASSERT_FALSE(contributed.empty())
            << stem << ".json contributes no REACHABLE pe binding — its root "
                       "names a pe image but the availability gate excludes pe, "
                       "so the two disagree about whether this header has a pe "
                       "surface at all.";
        bindings.insert(bindings.end(),
                        std::make_move_iterator(contributed.begin()),
                        std::make_move_iterator(contributed.end()));
    }

    ASSERT_FALSE(bindings.empty());
    auto const split = findCoStateSplit(bindings);
    EXPECT_FALSE(split.has_value())
        << "\n" << split.value_or(std::string{})
        << "\nThese descriptors exchange CRT-owned objects (heap pointers, "
           "FILE*, fds, errno). Binding them to different runtimes does not "
           "fail to link and does not diagnose — it corrupts at runtime.\n"
           "Flip the whole group in ONE commit (D-FFI-PE-CRT-UCRT-MIGRATION "
           "Phase 3), or revert the partial flip.\n"
           "If the offending binding is a PER-SYMBOL `library` override that is "
           "genuinely safe, it must be added to kDeclaredCrossRuntimeSymbols "
           "with the reason NO runtime-owned identity crosses that symbol — not "
           "removed from this check.";
}

// The guard is itself pinned: build the unsafe shape and prove the comparison
// that backs the test above rejects it. Without this, a future refactor could
// neuter the real test (e.g. by reading a key that no longer exists, so every
// binding reads back as absent and the loop compares nothing) and stay green.
TEST(PeCrtCoStateBinding, SplitCoStateGroupIsCaught) {
    // Exactly the dangerous shape: the heap on one runtime, stdio on the other.
    std::vector<GroupBinding> const split{
        {"malloc", "", "ucrtbase.dll"},
        {"stdio", "", "msvcrt.dll"},
    };
    EXPECT_TRUE(findCoStateSplit(split).has_value())
        << "the agreement check must REJECT a group split across two runtimes";

    // ...and it must ACCEPT a coherent group, in either direction, so the guard
    // does not simply always fail (which would also be useless).
    for (char const* runtime : {"msvcrt.dll", "ucrtbase.dll"}) {
        std::vector<GroupBinding> const coherent{
            {"malloc", "", runtime}, {"stdio", "", runtime}, {"errno", "", runtime},
        };
        EXPECT_FALSE(findCoStateSplit(coherent).has_value())
            << "a coherent group on " << runtime
            << " must be accepted (the invariant is AGREEMENT, not a specific "
               "runtime)";
    }
}

// ★ THE PER-SYMBOL BLIND SPOT, DEMONSTRATED CLOSED (TF-C111).
//
// The shape: a descriptor whose ROOT agrees with the group perfectly, and whose
// per-symbol override does not. This is the most dangerous form of the split
// because the obvious place to look is right. `malloc`/`free` is the sharpest
// instance — a block minted by ucrtbase's allocator handed to msvcrt's `free`
// walks a foreign heap, which is not a diagnostic, not a link error, and not
// reliably a crash near the cause.
//
// Three things are proved, in order, against the SAME file on disk:
//   1. the PRE-FIX helper reports AGREEMENT — the split was structurally
//      invisible, not merely unnoticed;
//   2. the fixed reader SEES all three declared bindings and classifies each;
//   3. the shared agreement predicate REJECTS the pair that can actually bind.
TEST(PeCrtCoStateBinding, PerSymbolOverrideSplitIsCaught) {
    ScratchDir scratch{Location::Temp, "pe-crt-costate"};
    fs::path const p = scratch.path() / "malloc.json";

    writeDescriptor(p, nlohmann::json{
        {"header", "malloc.h"},
        {"standard", "msvcrt"},
        {"availableObjectFormats", nlohmann::json::array({"pe"})},
        // The ROOT agrees with a group that has moved to the modern CRT.
        {"library", {{"pe", "ucrtbase.dll"}}},
        {"symbols", nlohmann::json::array({
            // Inherits the root — the ordinary case.
            {{"name", "malloc"},
             {"signature", "fn(u64) -> ptr<void>"},
             {"kind", "function"},
             {"linkage", "external"}},
            // ★ THE HAZARD: one line rebinds the DEALLOCATOR to the legacy CRT.
            {{"name", "free"},
             {"signature", "fn(ptr<void>) -> void"},
             {"kind", "function"},
             {"linkage", "external"},
             {"library", {{"pe", "msvcrt.dll"}}}},
            // A DEAD override: same disagreement, but the symbol does not exist
            // on pe, so it can never bind there. It must be seen and then
            // dismissed for a stated reason, never merely missed.
            {{"name", "__posix_only_helper"},
             {"signature", "fn() -> void"},
             {"kind", "function"},
             {"linkage", "external"},
             {"availableObjectFormats", nlohmann::json::array({"elf"})},
             {"library", {{"pe", "msvcrt.dll"}}}},
        })},
    });

    // 1. THE PRE-FIX BEHAVIOUR, EXERCISED — not asserted about. The old helper
    //    reads the root and stops, so it hands back the group's own runtime and
    //    the caller concludes "agrees". Every test in this file used to be built
    //    on exactly this value.
    std::optional<std::string> const preFix = rootOnlyPeLibraryOf(p);
    ASSERT_TRUE(preFix.has_value());
    EXPECT_EQ(*preFix, "ucrtbase.dll")
        << "the pre-fix helper must still be the pre-fix helper — it is kept "
           "verbatim so this demonstration stays real";
    EXPECT_FALSE(findCoStateSplit({{"malloc", "", *preFix},
                                   {"stdio", "", "ucrtbase.dll"}}).has_value())
        << "THE BLIND SPOT: fed only the root binding, the agreement predicate "
           "sees a coherent group and passes a descriptor that binds its "
           "allocator and its deallocator to two different C runtimes.";

    // 2. THE FIXED READER sees every declared binding and classifies each.
    std::vector<PeBinding> const declared = peBindingsOf(p);
    ASSERT_EQ(declared.size(), std::size_t{3})
        << "expected the root plus both per-symbol overrides";
    EXPECT_EQ(declared[0].symbol, "");
    EXPECT_EQ(declared[0].image, "ucrtbase.dll");
    EXPECT_TRUE(declared[0].reachable);
    EXPECT_EQ(declared[1].symbol, "free");
    EXPECT_EQ(declared[1].image, "msvcrt.dll");
    EXPECT_TRUE(declared[1].reachable);
    EXPECT_EQ(declared[2].symbol, "__posix_only_helper");
    EXPECT_FALSE(declared[2].reachable)
        << "a symbol whose availableObjectFormats excludes pe can never bind on "
           "pe (semantic_analyzer.cpp:13359-13362), so its pe override is dead "
           "config rather than a live hazard";

    // 3. THE SHARED PREDICATE REJECTS IT. Note the group has exactly TWO live
    //    bindings: the dead override is dropped, so the failure names the real
    //    one rather than burying it.
    std::vector<GroupBinding> const live = groupBindingsOf("malloc", declared);
    ASSERT_EQ(live.size(), std::size_t{2});
    std::optional<std::string> const split = findCoStateSplit(live);
    ASSERT_TRUE(split.has_value())
        << "the fixed guard must REJECT a per-symbol override that disagrees "
           "with its own descriptor root";
    EXPECT_NE(split->find("symbol 'free'"), std::string::npos)
        << "the diagnostic must name the offending SYMBOL, not just the file — "
           "'malloc.json disagrees with itself' is not actionable. Got:\n"
        << *split;
}

// The grant mechanism must actually GRANT — and must grant NARROWLY. Same file
// shape twice: once with the symbol this file declares an exception for, once
// with an identical override on an ordinary symbol. Only the declared one is
// allowed through, which is what makes the table a decision rather than a
// blanket amnesty.
TEST(PeCrtCoStateBinding, DeclaredCrossRuntimeSymbolIsAccepted) {
    ASSERT_FALSE(std::empty(kDeclaredCrossRuntimeSymbols))
        << "this test demonstrates the grant mechanism; with no grants declared "
           "it would silently demonstrate nothing";
    DeclaredCrossRuntimeSymbol const& grant = kDeclaredCrossRuntimeSymbols[0];
    ASSERT_NE(grant.why, nullptr);
    EXPECT_GT(std::string_view{grant.why}.size(), std::size_t{80})
        << "a grant carries the REASON no runtime-owned identity crosses that "
           "symbol; a bare entry is an unexplained exemption";

    ScratchDir scratch{Location::Temp, "pe-crt-costate"};

    auto build = [](char const* symbolName) {
        return nlohmann::json{
            {"header", "stdio.h"},
            {"standard", "c89"},
            {"library", {{"pe", "ucrtbase.dll"}}},
            {"symbols", nlohmann::json::array({
                {{"name", symbolName},
                 {"signature", "fn(ptr<char>) -> i32"},
                 {"kind", "function"},
                 {"linkage", "external"},
                 {"availableObjectFormats", nlohmann::json::array({"pe"})},
                 // Disagrees with the root — allowed ONLY when declared.
                 {"library", {{"pe", "msvcrt.dll"}}}},
            })},
        };
    };

    fs::path const declaredPath = scratch.path() / "stdio-declared.json";
    writeDescriptor(declaredPath, build(grant.symbol));
    EXPECT_FALSE(
        findCoStateSplit(groupBindingsOf(grant.stem, peBindingsOf(declaredPath)))
            .has_value())
        << "a DECLARED cross-runtime symbol must be accepted — " << grant.stem
        << " symbol '" << grant.symbol << "' is exempt because: " << grant.why;

    fs::path const undeclaredPath = scratch.path() / "stdio-undeclared.json";
    writeDescriptor(undeclaredPath, build("fclose"));
    EXPECT_TRUE(
        findCoStateSplit(groupBindingsOf(grant.stem, peBindingsOf(undeclaredPath)))
            .has_value())
        << "the grant must be NARROW: the identical override on an UNDECLARED "
           "symbol of the same descriptor must still fail. If this passes, the "
           "exception mechanism is a blanket amnesty and the guard is inert.";
}

// A grant for a descriptor OUTSIDE the co-state group blesses nothing, because
// nothing outside the group is ever examined — the only way a grant can be wrong
// SILENTLY (a mis-spelled stem or symbol fails loud instead, by leaving the
// override it meant to bless subject to the agreement check).
TEST(PeCrtCoStateBinding, EveryDeclaredExceptionNamesAGroupMember) {
    for (auto const& e : kDeclaredCrossRuntimeSymbols) {
        EXPECT_NE(std::find_if(std::begin(kCoStateGroup), std::end(kCoStateGroup),
                               [&](char const* s) {
                                   return std::string_view{s} == e.stem;
                               }),
                  std::end(kCoStateGroup))
            << "kDeclaredCrossRuntimeSymbols grants an exception for '" << e.stem
            << "' symbol '" << e.symbol << "', but '" << e.stem
            << "' is not a member of kCoStateGroup — the grant is INERT. Either "
               "the stem is misspelled, or the exception belongs to a descriptor "
               "this guard does not examine (in which case delete it rather than "
               "leave a record of a decision that is not enforced anywhere).";
    }
}

// A typo in a runtime image name ("ucrtbase.dl") would bind every symbol in that
// descriptor to a DLL that does not exist — breaking the binary LOAD at 0xC0000139
// under the eager-import law (D-FFI-DESCRIPTOR-EAGER-IMPORT) rather than failing
// the build. Keep the set of pe runtime images we are willing to name closed.
//
// This sweeps the ROOT and every PER-SYMBOL override, and — unlike the co-state
// check — it does NOT skip bindings the availability gate currently makes
// unreachable. Spelling is spelling: a dead override with a typo in it becomes a
// broken binary the day someone widens the gate, and the whole point of a closed
// vocabulary is to catch that while it is still a text edit.
TEST(PeCrtCoStateBinding, EveryPeRuntimeImageNamedIsAKnownRuntime) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty());

    std::vector<std::string> const known{
        "msvcrt.dll",     // the legacy CRT the migration is leaving
        "ucrtbase.dll",   // the modern Universal CRT it is moving to
        "kernel32.dll",   // Win32 OS surface (windows.json, threads.json)
    };

    std::size_t checked = 0;
    for (auto const& e : fs::recursive_directory_iterator(root / "shippedLibs")) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        for (auto const& b : peBindingsOf(e.path())) {
            ++checked;
            EXPECT_NE(std::find(known.begin(), known.end(), b.image), known.end())
                << e.path().filename().string()
                << (b.symbol.empty() ? std::string{" (root)"}
                                     : " symbol '" + b.symbol + "'")
                << " names pe runtime image '" << b.image
                << "', which is not a known pe runtime. A misspelled image does "
                   "not fail the build — DSS eagerly imports every declared "
                   "symbol, so the LOADER rejects the binary (0xC0000139) at run "
                   "time instead. If this is a genuinely new runtime, add it "
                   "here deliberately.";
        }
    }
    // Guard the guard: if the reader ever stops finding bindings (a renamed key,
    // a moved directory) this test would sweep nothing and stay green.
    EXPECT_GT(checked, std::size_t{10})
        << "found only " << checked
        << " pe bindings across the shipped tree — the reader is almost "
           "certainly looking at the wrong key or the wrong directory.";
}

// ── THE setjmp FACILITY'S pe (IMAGE, EXTERNAL NAME) PAIR ─────────────────────
//
// UCRT-P5 moved setjmp.json from msvcrt.dll to ucrtbase.dll, and that move is
// NOT the one-token edit every other descriptor's flip was: the two runtimes
// export this facility under DIFFERENT NAMES. MEASURED 2026-08-11
// (`objdump -p C:/Windows/System32/{msvcrt,ucrtbase}.dll`, GNU binutils 2.42):
//
//   msvcrt.dll    `_setjmp` (ordinal 722, RVA 0x7ab10) and `setjmp` (ord 1215)
//   ucrtbase.dll  `__intrinsic_setjmp` (ord 75, RVA 0xed430) and `setjmp`
//                 (ord 2371) — and NO `_setjmp` AT ALL
//
// So the descriptor carries TWO facts that have to agree: the image, and the
// external name (`linkName`, defaulting to the C identifier `_setjmp` exactly as
// ffi::linkNameFor resolves it). The dangerous states are the MIXTURES:
//
//   * ucrtbase.dll + no linkName ⇒ imports `_setjmp` from the one runtime that
//     does not export it. This is the naive repoint — and believing it was the
//     only available move is why this descriptor sat on the legacy CRT for two
//     phases after every one of its siblings had left.
//   * msvcrt.dll + `__intrinsic_setjmp` ⇒ a revert of the IMAGE that forgets the
//     NAME, importing a UCRT-only name from the legacy CRT.
//
// NEITHER mixture fails the build. DSS eagerly imports every declared shipped
// symbol (D-FFI-DESCRIPTOR-EAGER-IMPORT), so an absent name is rejected by the
// LOADER at 0xC0000139: every pe binary that includes <setjmp.h> refuses to
// start, with no diagnostic and no link error pointing at the JSON line.
//
// Following this file's standing rule (see the header: agreement, never a
// literal runtime), the pin is NOT "the image is ucrtbase". It is "the (image,
// name) pair is one that a real Windows actually exports" — true before the
// migration, true after it, false only in the mixtures. Adding a row to this
// table is a deliberate act and must cite an objdump.
struct ExportedSetjmpPair {
    char const* image;
    char const* externalName;
};

constexpr ExportedSetjmpPair kExportedSetjmpPairs[] = {
    {"msvcrt.dll",   "_setjmp"},              // ord 722  — the pre-UCRT-P5 bind
    {"msvcrt.dll",   "setjmp"},               // ord 1215 — tail-thunk to _setjmp
    {"ucrtbase.dll", "__intrinsic_setjmp"},   // ord 75   — what MSVC itself emits
    {"ucrtbase.dll", "setjmp"},               // ord 2371 — tail-thunk to the above
};

[[nodiscard]] bool isExportedSetjmpPair(std::string_view image,
                                        std::string_view externalName) {
    for (auto const& p : kExportedSetjmpPairs)
        if (image == p.image && externalName == p.externalName) return true;
    return false;
}

// The pe (image, external name) a descriptor actually binds for one symbol, with
// the same two defaulting rules the engine uses: a per-symbol `library` entry
// wins over the root (semantic_analyzer.cpp), and an absent `linkName` means the
// identifier itself (ffi::linkNameFor). nullopt = the symbol is not declared.
struct PeSymbolBinding {
    std::string image;
    std::string externalName;
};

[[nodiscard]] std::optional<PeSymbolBinding>
peBindingOfSymbol(nlohmann::json const& doc, std::string_view symbolName) {
    if (!doc.is_object() || !doc.contains("symbols")) return std::nullopt;
    for (auto const& sym : doc.at("symbols")) {
        if (!sym.is_object() || !sym.contains("name")) continue;
        if (sym.at("name").get<std::string>() != symbolName) continue;

        PeSymbolBinding out;
        if (doc.contains("library") && doc.at("library").is_object()
            && doc.at("library").contains("pe"))
            out.image = doc.at("library").at("pe").get<std::string>();
        if (sym.contains("library") && sym.at("library").is_object()
            && sym.at("library").contains("pe"))
            out.image = sym.at("library").at("pe").get<std::string>();

        out.externalName = std::string{symbolName};
        if (sym.contains("linkName")) {
            // Fail closed rather than wave through a shape this reader does not
            // model: `linkName` may also be an object with per-target variants,
            // and silently ignoring one would make this pin assert nothing.
            EXPECT_TRUE(sym.at("linkName").is_string())
                << "symbol '" << symbolName << "' carries a non-flat 'linkName'. "
                   "This guard only models the flat-string form — teach it the "
                   "variant form rather than letting the pair go unchecked.";
            if (sym.at("linkName").is_string())
                out.externalName = sym.at("linkName").get<std::string>();
        }
        return out;
    }
    return std::nullopt;
}

TEST(PeCrtCoStateBinding, SetjmpPeBindingNamesAPairWindowsExports) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty());
    fs::path const path = root / "shippedLibs" / "setjmp.json";
    ASSERT_TRUE(fs::exists(path)) << path.string();

    std::ifstream in{path, std::ios::binary};
    ASSERT_TRUE(in.is_open());
    nlohmann::json const doc = nlohmann::json::parse(in, nullptr, false);
    ASSERT_FALSE(doc.is_discarded()) << "setjmp.json does not parse";

    auto const bind = peBindingOfSymbol(doc, "_setjmp");
    // Guard the guard: if the pe row is ever renamed this reader would find
    // nothing and the test would pass by sweeping an empty set.
    ASSERT_TRUE(bind.has_value())
        << "setjmp.json declares no `_setjmp` symbol — this guard just stopped "
           "checking anything. If the pe row was renamed, re-aim the guard.";

    EXPECT_TRUE(isExportedSetjmpPair(bind->image, bind->externalName))
        << "setjmp.json binds pe `_setjmp` to image '" << bind->image
        << "' under the external name '" << bind->externalName
        << "', which that runtime does not export. This does NOT fail the build: "
           "DSS eagerly imports every declared shipped symbol, so the LOADER "
           "rejects every pe binary that includes <setjmp.h> at 0xC0000139. The "
           "image and the name have to move together — ucrtbase.dll needs "
           "`__intrinsic_setjmp`, msvcrt.dll needs `_setjmp`.";
}

// RED-ON-DISABLE for the decision above, run against the two shapes that would
// otherwise reach a user as a binary that will not start. Without this the pin
// would be a single assertion over a tree that is currently correct, and a
// future edit that broke `peBindingOfSymbol` (say, dropping the per-symbol
// library merge) would leave it green.
TEST(PeCrtCoStateBinding, SetjmpPeBindingMixtureIsCaught) {
    // The two known-good pairs are accepted...
    EXPECT_TRUE(isExportedSetjmpPair("msvcrt.dll", "_setjmp"));
    EXPECT_TRUE(isExportedSetjmpPair("ucrtbase.dll", "__intrinsic_setjmp"));

    // ...and both mixtures are rejected.
    EXPECT_FALSE(isExportedSetjmpPair("ucrtbase.dll", "_setjmp"))
        << "the naive repoint (image moved, name left behind) must be caught";
    EXPECT_FALSE(isExportedSetjmpPair("msvcrt.dll", "__intrinsic_setjmp"))
        << "the half-revert (name moved, image left behind) must be caught";

    // The reader's defaulting rules are what turn a JSON edit into a pair, so
    // exercise them directly on synthetic documents rather than trusting them.
    auto const parse = [](char const* text) {
        return nlohmann::json::parse(text, nullptr, false);
    };

    // No `linkName` ⇒ the external name IS the identifier.
    auto const bare = peBindingOfSymbol(parse(R"JSON({
        "library": { "pe": "ucrtbase.dll" },
        "symbols": [ { "name": "_setjmp" } ]
    })JSON"), "_setjmp");
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(bare->externalName, "_setjmp");
    EXPECT_FALSE(isExportedSetjmpPair(bare->image, bare->externalName));

    // A per-symbol `library` entry WINS over the root — the axis that was this
    // file's original blind spot.
    auto const overridden = peBindingOfSymbol(parse(R"JSON({
        "library": { "pe": "ucrtbase.dll" },
        "symbols": [ { "name": "_setjmp", "linkName": "__intrinsic_setjmp",
                       "library": { "pe": "msvcrt.dll" } } ]
    })JSON"), "_setjmp");
    ASSERT_TRUE(overridden.has_value());
    EXPECT_EQ(overridden->image, "msvcrt.dll");
    EXPECT_FALSE(isExportedSetjmpPair(overridden->image,
                                      overridden->externalName));

    // An undeclared symbol reports nullopt rather than a default-constructed
    // pair that would silently satisfy the table.
    EXPECT_FALSE(peBindingOfSymbol(parse(R"JSON({"symbols":[]})JSON"), "_setjmp")
                     .has_value());
}
