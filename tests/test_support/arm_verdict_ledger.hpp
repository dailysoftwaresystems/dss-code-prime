#pragma once

// D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT — A VERDICT FOR EVERY DECLARED ARM.
//
// THE DEFECT THIS CLOSES (measured 2026-08-03, both corpus harnesses): when a
// target arm's arch differed from the host's and EITHER the manifest declared
// no `emulator` OR the declared emulator was not on PATH, the arm was marked
// `SkippedCrossHost` and the runner returned with NO assertion of any kind.
// The ctest entry was GREEN. There was no skip ledger, no per-leg runnability
// table, and nothing asserting that a DECLARED arm ever produced a verdict —
// so on the two `ubuntu-latest` x86_64 CI legs (which install no qemu) 449 of
// 557 manifests were silently skipped and counted as passes.
//
// The fix is NOT "install qemu": that changes WHICH units run, not whether an
// unrun unit can pass. The fix is that every declared arm lands in a LEDGER
// with a reason, the summary prints the skip accounting beside the pass/fail
// counts, and the ENVIRONMENTAL skip class can be promoted to a hard failure.
//
// WHY THE SKIP REASONS ARE NOT COLLAPSED INTO ONE. They mean different things
// and carry different enforcement, and a single `Skipped` erases exactly the
// distinction that makes the ledger a gate rather than a log:
//
//   * STRUCTURAL  (`SkippedByRunOn`, `SkippedNoEmulatorDeclared`) — predictable
//     from the manifest + the host alone. Nothing about the machine can change
//     them. They are counted and shown, never failed. `SkippedNoEmulatorDeclared`
//     is additionally a MANIFEST defect, and it is caught host-independently by
//     `lintDeclaredEmulators` below rather than by reddening the host that
//     happens to notice it.
//   * ENVIRONMENTAL (`SkippedEmulatorMissing`,
//     `SkippedLauncherPrerequisiteMissing`, `SkippedBuildInputMissing`) — the
//     manifest asked for something and the machine could not supply it. A
//     developer without qemu must still get a usable green suite, so the
//     DEFAULT is a visible warning; the gate opts in to
//     `DSS_STRICT_ARM_VERDICTS=1` and the same skip becomes a RED. The three
//     are one CLASS and three NAMES because they share an enforcement and a
//     remedy while naming three different things the machine did not supply:
//     no launcher at all, a launcher whose own prerequisites are absent, and a
//     build input.
//   * HARNESS     (`NotSelectedByRunner`) — the CLI-subprocess runner binds ONE
//     target per manifest (`selectBoundTargetIndex` below: the host's own arch
//     where the manifest offers it, else the first `runOn` match), so the other
//     matching targets are never considered
//     (D-TEST-CLI-HARNESS-BINDS-FIRST-MATCHING-TARGET, whose remaining half is
//     that ONE is bound at all). Ledgered honestly rather than left absent or
//     miscounted as a skip. NEVER a failure, not even in strict mode: it is a
//     known harness limitation with its own anchor, and failing on it would make
//     strict mode unusable until that anchor closes.
//
// AGNOSTIC: nothing here hardcodes an arch, an OS or an emulator name. Every
// decision keys on values the MANIFEST declared (`spec`, `runOn`, `emulator`)
// compared against the host chokepoints below.

#include "host_native_target.hpp"

// The host-OS token vocabulary. INCLUDED BY RELATIVE PATH on purpose: this
// header's consumers are three targets with three different include-path sets,
// and `integrated_tests` (integrated_tests/CMakeLists.txt) carries ONLY
// `tests/test_support` — no `src/` — because it links no DSS library at all,
// driving the compiler as a subprocess instead. A quoted include resolves
// against the includer's own directory first, so this one form works for every
// consumer with no build-graph change; `#include "program/platform_token.hpp"`
// would compile in the two gtest targets and break the CLI runner. The header
// is header-only and dependency-free precisely so this costs nothing (see its
// "SELF-CONTAINED ON PURPOSE" note).
#include "../../src/program/platform_token.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iterator>   // std::size — the kAllArmVerdicts cardinality pin
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dss::test_support {

// ── Host identity: ONE definition, shared by both corpus harnesses ─────────
//
// Both runners previously carried byte-identical private copies of these. That
// is the duplication class `host_native_target.hpp` was created to end (see its
// header comment: the same host-detection ladder was fixed once and its
// duplicate was not, twice). One copy, here.

// The host OS, spelled in the `runOn` vocabulary. FORWARDS to
// `dss::currentHostOs()` (src/program/platform_token.hpp) — the private `#ifdef`
// ladder that used to sit here is gone.
//
// It moved because the vocabulary stopped being test-only: `.dss-project.json`
// build-script entries are gated on the same three tokens, so the DRIVER now
// has to spell them too, and "one copy" (this block's own demand, above) has to
// mean one copy across the whole repo, not one copy per tier. The driver header
// is where the shared definition belongs — see its placement note for why that
// is `src/program/` and emphatically not `src/core/`.
//
// The name and namespace are unchanged so every existing caller in both corpus
// runners compiles untouched, and it still returns `std::string` by value:
// callers store it in `std::string` and pass it to interfaces taking
// `std::string const&`, and handing them an owning copy keeps every one of
// those uses correct with no lifetime question to re-audit.
//
// ONE BEHAVIOUR CHANGE, DELIBERATE: the old `#else` returned `"unknown"`, a
// token no manifest's `runOn` can contain, so an unsupported host would have
// marked the ENTIRE corpus `SkippedByRunOn` — the structural class, which is
// counted and never failed, not even under DSS_STRICT_ARM_VERDICTS=1. That is
// precisely the "silently skipped and counted as passes" shape this file exists
// to close. The shared ladder makes it a compile error instead.
[[nodiscard]] inline std::string currentHostOs() noexcept {
    return std::string{::dss::currentHostOs()};
}

// The host ARCH, returning the SAME identifier strings the target configs use
// as their `name`, so a manifest spec's target prefix compares directly.
// `test_arm_verdict_ledger.cpp` pins this against
// `specTargetArch(hostNativeTarget().execTarget)` so the two host chokepoints
// cannot drift apart.
[[nodiscard]] inline std::string currentHostArch() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

// The target portion of a manifest spec ("arm64:elf64-aarch64-linux-exec" →
// "arm64"). This IS the arch identifier compared against currentHostArch().
[[nodiscard]] inline std::string specTargetArch(std::string const& spec) {
    auto const colon = spec.find(':');
    return colon == std::string::npos ? spec : spec.substr(0, colon);
}

// The OBJECT-FORMAT portion of the same split ("arm64:elf64-aarch64-linux-exec"
// → "elf64-aarch64-linux-exec"), i.e. `specTargetArch`'s twin. Empty when the
// spec carries no `:` — a project-mode arm turns that into a loud manifest
// error rather than composing a wrong artifact path, because the format name IS
// the per-platform output subdirectory a project build routes through
// (`setPerFormatOutputSubdir`, program.cpp).
//
// ★ IT LIVES HERE, BESIDE ITS TWIN, FOR THE REASON THE WHOLE HEADER EXISTS. It
// arrived as two byte-identical locals — one per runner — when project mode
// landed, because `tests/test_support/**` was outside that change's write
// scope; both copies carried a ⚠ naming this fold as the fix. One
// spec-splitting vocabulary, one definition: the two harnesses must not be able
// to disagree about how a spec splits, which is exactly the divergence class
// `D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT` cost us once already.
[[nodiscard]] inline std::string specFormatName(std::string const& spec) {
    auto const colon = spec.find(':');
    return colon == std::string::npos ? std::string{} : spec.substr(colon + 1);
}

// ── WHICH TARGET A ONE-TARGET-PER-MANIFEST RUNNER BINDS ON THIS HOST ───────
//
// D-TEST-INTEGRATED-TESTS-CANNOT-PASS-ON-A-NATIVE-ARM64-LINUX-HOST. The
// CLI-subprocess runner builds and spawns ONE target per manifest (its
// in-process sibling iterates all of them), and it used to bind the FIRST
// target whose `runOn` admits the host. `runOn` names an OS, not a MACHINE:
// the corpus declares `x86_64:elf64-x86_64-linux-exec` before
// `arm64:elf64-aarch64-linux-exec`, and both say `linux`. So on a NATIVE
// aarch64 box the runner bound the x86_64 arm — cross-arch there, with no
// `emulator` declared — and never considered the one arm that machine can
// execute at full speed with no launcher at all.
//
// ✔MEASURED on the arm64 VPS (real hardware, not qemu) before this rule
// existed: `55 verified (31 ran), 4353 skipped, 1307 not-selected`, and ZERO
// non-empty stdout pins — so [Test 5]'s capture-path witness could not be
// satisfied and that entire host verified NO runtime behaviour. The runner
// picked a target it cannot run and skipped the one it can.
//
// THE RULE: among the targets whose `runOn` admits this host, bind the one
// whose TARGET ARCH EQUALS THE HOST'S; fall back to the first `runOn` match
// only when the manifest offers no native arm. That is one comparison between
// two values the SYSTEM reports — `specTargetArch(spec)` against
// `currentHostArch()` — so it names no arch, no OS and no object format, and it
// will keep binding correctly for a target this repo has not shipped yet.
//
// ⚠ IT DELIBERATELY DOES NOT PREFER AN EMULATABLE ARM, and there is a
// measurement behind that: emulating the x86_64 arm on that aarch64 box cost
// **879 s on ONE test**. Native is both far cheaper AND the stronger verdict,
// so "can this host RUN it" is answered by arch equality, never by a launcher.
//
// ⚠ THE OS GATE STILL OUTRANKS THE ARCH PREFERENCE. `runsOnHost` is evaluated
// by the caller and a `false` is never overridden here: a manifest that
// declares the host's own arch for ANOTHER OS (`arm64:macho64-arm64-darwin-exec`
// on arm64 Linux) must stay unbound, because the produced image is not loadable
// here. Arch equality answers "can this machine execute those instructions",
// which is necessary and not sufficient.
//
// ⓘ WHY IT LIVES IN THIS HEADER rather than in the one runner that calls it:
// this is the file that ends "the two harnesses disagree about the host"
// defects (see the `specFormatName` note above). The binding rule is host
// vocabulary, it is precisely what the two runners must never diverge on again,
// and here it gets a unit test on EVERY host — which matters more than usual,
// because the host that exposed the defect is not the host most cycles run on.
struct HostBindingCandidate {
    std::string spec;        // "<arch>:<format>", exactly as the manifest wrote it
    bool        runsOnHost;  // this target's `runOn` list admits this host OS
};

// "No target of this manifest can be bound on this host." A named constant, not
// a bare `-1` at four call sites: an index type that cannot be negative makes
// the sentinel invisible otherwise.
inline constexpr std::size_t kNoBoundTarget = static_cast<std::size_t>(-1);

[[nodiscard]] inline std::size_t
selectBoundTargetIndex(std::vector<HostBindingCandidate> const& candidates,
                       std::string const& hostArch) {
    std::size_t firstRunOnMatch = kNoBoundTarget;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].runsOnHost) continue;   // wrong OS — never bindable
        if (firstRunOnMatch == kNoBoundTarget) firstRunOnMatch = i;
        if (specTargetArch(candidates[i].spec) == hostArch) {
            return i;   // this host EXECUTES it — no emulator, no launcher
        }
    }
    // No native arm declared for this host: the previous behaviour, kept as the
    // FALLBACK so a single-target manifest (every windows- and darwin-matching
    // manifest in the corpus) binds exactly what it always did.
    return firstRunOnMatch;
}

// D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY, closing-work item (2), first half: the
// remedy line to append to a FAILING run's message when that run went through
// qemu-user with `QEMU_LD_PREFIX` unset. Empty in every other case, so a native
// arm's failure text is never padded with an irrelevant environment lecture.
//
// ★ IT LIVES HERE, BESIDE `specTargetArch` AND `findOnPath`, FOR THE REASON THE
// WHOLE HEADER EXISTS — and it is here from the FIRST commit that has it rather
// than after the usual two-copies-then-fold detour. It was written as a local
// helper in `examples_runner.cpp`, and the pairing rule (SKILL.md §2: a
// capability added to one harness is added to the other) was caught by review,
// not by the author. A hint that fires in the in-process runner and stays
// silent in the subprocess runner would mean the SAME failing arm explains
// itself on one path and not the other — the divergence class this header was
// created to end.
//
// ⚠ This does NOT classify the failure or quote the loader line, so it is only
// HALF of item (2) and the registry row stays open for the rest. It is the
// cheap half on purpose: the arm still runs and still reds, because masking a
// cross-arch failure is the workaround the bar forbids. It only makes the red
// say what is most likely wrong.
[[nodiscard]] inline std::string qemuSysrootHint(std::string const& spec,
                                                 std::string const& emulator) {
    std::string const targetArch = specTargetArch(spec);
    if (targetArch.empty() || targetArch == currentHostArch()) {
        return {};   // native — no launcher was involved at all
    }
    // Only qemu-user reads QEMU_LD_PREFIX. Another launcher's failure has
    // nothing to do with it and must not be handed a misleading remedy.
    if (!emulator.starts_with("qemu-")) {
        return {};
    }
    char const* const prefix = std::getenv("QEMU_LD_PREFIX");
    if (prefix != nullptr && *prefix != '\0') {
        return {};   // the operator already owns the setting
    }
    return "\nNOTE: this arm ran under '" + emulator
         + "' with QEMU_LD_PREFIX unset. When qemu-user cannot find the guest "
           "ELF interpreter it exits 255 for EVERY binary, however correct — "
           "look for \"Could not open '/lib/ld-linux-*.so'\" in the output "
           "above, and if it is there export QEMU_LD_PREFIX=<guest sysroot> "
           "(Debian/Ubuntu: /usr/<arch>-linux-gnu, from the "
           "libc6-<arch>-cross or gcc-<arch>-linux-gnu package) and re-run. "
           "MEASURED 2026-08-12 on the WSL leg: 468 of 826 tests failed "
           "without it, 826 of 826 passed with it and nothing else changed.";
}

// Resolve an executable name against $PATH (e.g. "qemu-aarch64"), returning
// its full path or "" if not found. This is the ONE place the two harnesses
// decide whether a declared emulator is present, so they cannot disagree about
// what "missing" means. AGNOSTIC: the name comes from the manifest.
[[nodiscard]] inline std::string findOnPath(std::string const& exe) {
    namespace fs = std::filesystem;
    char const* const pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) return {};
#if defined(_WIN32)
    char const sep = ';';
    std::vector<std::string> const exts = {"", ".exe"};
#else
    char const sep = ':';
    std::vector<std::string> const exts = {""};
#endif
    std::string const path = pathEnv;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto const end = path.find(sep, start);
        std::string const dir = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            for (auto const& ext : exts) {
                fs::path const cand = fs::path(dir) / (exe + ext);
                std::error_code ec;
                if (fs::exists(cand, ec) && fs::is_regular_file(cand, ec)) {
                    return cand.generic_string();
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

// ── The verdict ────────────────────────────────────────────────────────────

enum class ArmVerdict {
    // VERIFIED — the arm produced an assertion.
    Ran,                        // compiled, spawned, exit code + stdout asserted
    ExpectErrorAsserted,        // an `expectDiagnostics` arm: the compile was
                                // asserted to FAIL with the exact declared
                                // diagnostic set. Nothing is spawned by design,
                                // so it is verified without being `Ran`.
    // STRUCTURAL skips — manifest + host decide these; the machine cannot.
    SkippedByRunOn,             // the manifest's `runOn` excludes this host OS
    SkippedNoEmulatorDeclared,  // cross-arch arm with no `emulator` key
    // ENVIRONMENTAL skips — depend on the machine, not the manifest.
    SkippedEmulatorMissing,     // `emulator` declared but not found on PATH
    SkippedLauncherPrerequisiteMissing,
                                // the launcher itself is PRESENT and looks
                                // perfectly usable, and something it DECLARED it
                                // needs BEYOND its own argv[0] is not.
                                // ✔MEASURED (arm64 VPS, sqlite leg elf64-x86_64
                                // under qemu-x86_64): three corpus segments
                                // aborted with glibc's `libgcc_s.so.1 must be
                                // installed for pthread_exit to work`, one AFTER
                                // its summary had printed. And the sharper case:
                                // the sqlite catalogue's Windows launcher for the
                                // arm64 leg is `["wsl.exe","-e","qemu-aarch64"]`,
                                // so resolving argv[0] confirms **wsl.exe** and
                                // never asks whether qemu exists inside the
                                // distro — every unit then exits 255 and 14 of
                                // them were charged to the compiler.
                                // ★ NOT `SkippedEmulatorMissing`, deliberately:
                                // that name says "missing" of a launcher that is
                                // right there and runs, which is the very
                                // conflation this verdict exists to end. Same
                                // CLASS (environmental) because it has the same
                                // enforcement and the same remedy — install the
                                // thing — and a separate NAME because a reader
                                // must be able to tell "no emulator here" from
                                // "the emulator is here and its sysroot is not".
    SkippedBuildInputMissing,   // a DECLARED BUILD input is absent from this
                                // machine — the sqlite harness's resolve-library
                                // binaries (tcl86.dll for the pe64 leg, libtcl
                                // .so for an elf leg, …) or a leg's target C
                                // compiler. D-HARNESS-CROSS-HOST-ANY-TARGET: the
                                // BUILD of a declared leg is attempted on every
                                // host, so "this box has no copy of the target's
                                // tcl runtime" needed a name of its own. It is
                                // NOT `Poisoned` (nothing was miscompiled) and
                                // NOT structural (another machine, same
                                // manifest, builds it fine) — it is the build
                                // twin of SkippedEmulatorMissing, and strict
                                // mode promotes both for the same reason.
    // HARNESS limitation — see D-TEST-CLI-HARNESS-BINDS-FIRST-MATCHING-TARGET.
    NotSelectedByRunner,        // runOn matched, but another target was bound first
    // FAILURE — every assertion has already fired at the recording site.
    Poisoned,                   // compile failed / artifact missing / spawn failed
    kCount_                     // sentinel — cardinality pin, never a verdict
};

// The `phase_timers.hpp::kCount_` precedent. This is the LOAD-BEARING half of
// the closed-enum enforcement, so it is worth saying why a sentinel earns its
// keep in an enum whose values are otherwise all meaningful:
//
// `kAllArmVerdicts` is hand-maintained, and the ONLY compile-time fact that can
// contradict it is the enum's own cardinality. Without `kCount_` nothing ties
// the two together — an author who appends an enumerator and does not touch the
// array gets a clean build, a green suite, and a verdict that classifies into
// NO bucket (see `armVerdictClass` below), i.e. it VANISHES from
// `renderCountsLine()`'s accounting. That is precisely the defect this ledger
// exists to prevent, so the ledger failing that way would be self-refuting.
inline constexpr std::size_t kArmVerdictCount =
    static_cast<std::size_t>(ArmVerdict::kCount_);

inline constexpr ArmVerdict kAllArmVerdicts[] = {
    ArmVerdict::Ran,
    ArmVerdict::ExpectErrorAsserted,
    ArmVerdict::SkippedByRunOn,
    ArmVerdict::SkippedNoEmulatorDeclared,
    ArmVerdict::SkippedEmulatorMissing,
    ArmVerdict::SkippedLauncherPrerequisiteMissing,
    ArmVerdict::SkippedBuildInputMissing,
    ArmVerdict::NotSelectedByRunner,
    ArmVerdict::Poisoned,
};

// LAYER 1 of 3 — COMPILE-TIME, and the only one that is toolchain-independent.
// ⚠ It cannot be replaced by `-Werror=switch`: that flag is applied ONLY under
// `src/{asm,lir,core,opt,mir/lowering}` (see their CMakeLists), the project sets
// no `-Wall`/`-Wextra` for `tests/`, and a `default:`-less switch is silent
// without it. So on the Windows gate leg a missing case would not even warn.
// A `static_assert` fires on every compiler, in every directory, with no flags.
static_assert(std::size(kAllArmVerdicts) == kArmVerdictCount,
              "ArmVerdict gained or lost an enumerator without updating "
              "kAllArmVerdicts — every verdict must be enumerable, or it "
              "silently escapes the ledger's accounting");

// Stable short names. These appear in gate logs and are grepped by humans —
// treat them as a pinned vocabulary, not free-form prose.
[[nodiscard]] constexpr std::string_view armVerdictName(ArmVerdict v) noexcept {
    switch (v) {
        case ArmVerdict::Ran:                       return "ran";
        case ArmVerdict::ExpectErrorAsserted:       return "expect-error-asserted";
        case ArmVerdict::SkippedByRunOn:            return "skipped-by-runOn";
        case ArmVerdict::SkippedNoEmulatorDeclared: return "skipped-no-emulator-declared";
        case ArmVerdict::SkippedEmulatorMissing:    return "skipped-emulator-missing";
        case ArmVerdict::SkippedLauncherPrerequisiteMissing:
            return "skipped-launcher-prerequisite-missing";
        case ArmVerdict::SkippedBuildInputMissing:  return "skipped-build-input-missing";
        case ArmVerdict::NotSelectedByRunner:       return "not-selected-by-runner";
        case ArmVerdict::Poisoned:                  return "poisoned";
        case ArmVerdict::kCount_:                   return "invalid-sentinel";
    }
    return "unknown";  // LAYER 3 detector — see armVerdictClass below
}

// ── The class of a verdict: ONE exhaustive chokepoint ──────────────────────
//
// Every accounting question — verified? skipped? failed? — is answered HERE and
// nowhere else, because the alternative is what this replaced: three
// independent `if`-chains plus a fourth inside `skippedCount()`, none of which
// mentioned all seven verdicts. A new enumerator satisfied none of the four
// predicates, so it landed in NO class, `skippedCount()` did not count it, and
// `renderCountsLine()` printed a breakdown that no longer summed to its own
// `(of N declared arms)` denominator — silently.
//
// `Unclassified` is NOT a class anything may legitimately be in. It is the
// DETECTOR: the only way to reach it is to add an enumerator and not give it an
// arm below, and both LAYER 3 tests refuse it.
enum class ArmVerdictClass {
    Verified,           // the arm produced an assertion
    StructuralSkip,     // manifest + host decide it; the machine cannot
    EnvironmentalSkip,  // the machine decides it — strict mode's input
    HarnessSkip,        // a known harness limitation, never a failure
    Failure,            // already reported loudly at the recording site
    Unclassified,       // ⚠ NOT reachable for a correct enum — the detector
};

// LAYER 2 of 3 — a `default:`-less exhaustive switch. Free where the toolchain
// supplies `-Wswitch` (the CI clang/gcc legs), inert on the Windows gate leg,
// which is exactly why layers 1 and 3 exist rather than this alone.
[[nodiscard]] constexpr ArmVerdictClass armVerdictClass(ArmVerdict v) noexcept {
    switch (v) {
        case ArmVerdict::Ran:
        case ArmVerdict::ExpectErrorAsserted:
            return ArmVerdictClass::Verified;
        case ArmVerdict::SkippedByRunOn:
        case ArmVerdict::SkippedNoEmulatorDeclared:
            return ArmVerdictClass::StructuralSkip;
        case ArmVerdict::SkippedEmulatorMissing:
        case ArmVerdict::SkippedLauncherPrerequisiteMissing:
        case ArmVerdict::SkippedBuildInputMissing:
            return ArmVerdictClass::EnvironmentalSkip;
        case ArmVerdict::NotSelectedByRunner:
            return ArmVerdictClass::HarnessSkip;
        case ArmVerdict::Poisoned:
            return ArmVerdictClass::Failure;
        case ArmVerdict::kCount_:
            break;  // the sentinel is not a verdict and has no class
    }
    return ArmVerdictClass::Unclassified;
}

// A verdict that produced an assertion. The whole point of the ledger is that
// `N passed` can be read against THIS count rather than against the total.
[[nodiscard]] constexpr bool armVerdictIsVerified(ArmVerdict v) noexcept {
    return armVerdictClass(v) == ArmVerdictClass::Verified;
}

// Predictable from manifest + host alone; the machine cannot change it.
[[nodiscard]] constexpr bool armVerdictIsStructuralSkip(ArmVerdict v) noexcept {
    return armVerdictClass(v) == ArmVerdictClass::StructuralSkip;
}

// Depends on the machine. THIS is the class strict mode promotes to a failure.
[[nodiscard]] constexpr bool armVerdictIsEnvironmentalSkip(ArmVerdict v) noexcept {
    return armVerdictClass(v) == ArmVerdictClass::EnvironmentalSkip;
}

// Every flavour of "declared but not verified". Keyed on the CLASS, so a new
// skip enumerator is counted the moment it is classified — the old if-chain
// named `NotSelectedByRunner` literally and would have missed a sibling.
[[nodiscard]] constexpr bool armVerdictIsSkip(ArmVerdict v) noexcept {
    auto const c = armVerdictClass(v);
    return c == ArmVerdictClass::StructuralSkip
        || c == ArmVerdictClass::EnvironmentalSkip
        || c == ArmVerdictClass::HarnessSkip;
}

// Reported loudly at the recording site — NOT a skip, and not verified either.
[[nodiscard]] constexpr bool armVerdictIsFailure(ArmVerdict v) noexcept {
    return armVerdictClass(v) == ArmVerdictClass::Failure;
}

// ── Strict mode ────────────────────────────────────────────────────────────

inline constexpr char const* kStrictArmVerdictsEnv = "DSS_STRICT_ARM_VERDICTS";

// The parse result. Malformed is reported SEPARATELY rather than folded into
// `on=false`, because the two callers report loudly in different idioms (gtest
// ADD_FAILURE vs the subprocess runner's `check`) and neither may silently
// interpret an unrecognised value. Same discipline as
// `golden_file.hpp::goldenRefreshRequested()`: a stale `=0` must never quietly
// disable a gate, and a typo'd `=ture` must never quietly disable it either.
struct StrictArmVerdictsSetting {
    bool        on        = false;
    bool        malformed = false;
    std::string raw;  // populated iff the variable was set
};

[[nodiscard]] inline StrictArmVerdictsSetting readStrictArmVerdicts() {
    StrictArmVerdictsSetting s;
    char const* const rawEnv = std::getenv(kStrictArmVerdictsEnv);
    if (rawEnv == nullptr) return s;
    s.raw = rawEnv;
    std::string_view const v{s.raw};
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes") {
        s.on = true;
        return s;
    }
    if (v.empty() || v == "0" || v == "false" || v == "FALSE" || v == "no") {
        return s;
    }
    s.malformed = true;
    return s;
}

// ── The ledger ─────────────────────────────────────────────────────────────

struct ArmVerdictRecord {
    std::string example;  // "c/<name>" — stable across both harnesses
    std::string spec;     // the manifest's declared target spec
    std::string arm;      // "baseline" or an optimizedPipelines label
    ArmVerdict  verdict = ArmVerdict::Poisoned;
    std::string detail;   // WHY, in the manifest's own vocabulary
};

class ArmVerdictLedger {
public:
    void record(ArmVerdictRecord rec) { records_.push_back(std::move(rec)); }

    void record(std::string example, std::string spec, std::string arm,
                ArmVerdict verdict, std::string detail) {
        records_.push_back({std::move(example), std::move(spec), std::move(arm),
                            verdict, std::move(detail)});
    }

    [[nodiscard]] std::vector<ArmVerdictRecord> const& records() const noexcept {
        return records_;
    }
    [[nodiscard]] std::size_t total() const noexcept { return records_.size(); }

    [[nodiscard]] std::size_t count(ArmVerdict v) const noexcept {
        std::size_t n = 0;
        for (auto const& r : records_) if (r.verdict == v) ++n;
        return n;
    }
    [[nodiscard]] std::size_t verifiedCount() const noexcept {
        std::size_t n = 0;
        for (auto const& r : records_) if (armVerdictIsVerified(r.verdict)) ++n;
        return n;
    }
    [[nodiscard]] std::size_t skippedCount() const noexcept {
        std::size_t n = 0;
        for (auto const& r : records_) if (armVerdictIsSkip(r.verdict)) ++n;
        return n;
    }
    [[nodiscard]] std::size_t failedCount() const noexcept {
        std::size_t n = 0;
        for (auto const& r : records_) if (armVerdictIsFailure(r.verdict)) ++n;
        return n;
    }
    // The three classes `renderCountsLine()` reports. If this is not `total()`,
    // some record's verdict belongs to NO reported class and the summary's
    // numbers do not add up — see the fail-loud branch in renderCountsLine().
    [[nodiscard]] std::size_t accountedCount() const noexcept {
        return verifiedCount() + skippedCount() + failedCount();
    }

    // The records strict mode turns into failures. Returned rather than acted
    // on here so each harness reports in its own idiom.
    [[nodiscard]] std::vector<ArmVerdictRecord> environmentalSkips() const {
        std::vector<ArmVerdictRecord> out;
        for (auto const& r : records_) {
            if (armVerdictIsEnvironmentalSkip(r.verdict)) out.push_back(r);
        }
        return out;
    }

    // ONE line, every class named and counted. Deliberately spells out each
    // skip reason: a bare "N skipped" would re-create the very conflation this
    // ledger exists to end.
    [[nodiscard]] std::string renderCountsLine() const {
        std::ostringstream o;
        o << verifiedCount() << " verified ("
          << count(ArmVerdict::Ran) << " ran, "
          << count(ArmVerdict::ExpectErrorAsserted) << " expect-error), "
          << skippedCount() << " skipped [structural: "
          << count(ArmVerdict::SkippedByRunOn) << " by-runOn, "
          << count(ArmVerdict::SkippedNoEmulatorDeclared) << " no-emulator-declared"
          << "; environmental: "
          << count(ArmVerdict::SkippedEmulatorMissing) << " emulator-missing, "
          << count(ArmVerdict::SkippedLauncherPrerequisiteMissing)
          << " launcher-prerequisite-missing, "
          << count(ArmVerdict::SkippedBuildInputMissing) << " build-input-missing"
          << "; harness: "
          << count(ArmVerdict::NotSelectedByRunner) << " not-selected], "
          << count(ArmVerdict::Poisoned) << " poisoned"
          << "  (of " << total() << " declared arms)";
        // FAIL LOUD IN THE SUMMARY ITSELF. The line above is a breakdown, and a
        // breakdown that does not sum to its own denominator is worse than no
        // breakdown: it reads like full accounting. A verdict reaches here
        // unaccounted only by being unclassified (`armVerdictClass`) or by
        // being a NEW enumerator in a reported class but not in its per-verdict
        // `count(...)` term — so this catches the runtime half of what the
        // static_assert catches at compile time, and it announces itself on
        // whatever leg is running rather than waiting to be noticed.
        if (accountedCount() != total()) {
            o << "  ★ LEDGER ACCOUNTING HOLE: " << accountedCount()
              << " of " << total()
              << " records fall in a reported class — the rest belong to NO"
                 " class and have VANISHED from this line"
                 " (D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT)";
        }
        return o.str();
    }

    // One line per NON-verified record. `verified` arms are omitted: they are
    // already accounted for by the harness's own pass count, and printing 557
    // of them would bury the handful the reader is here for.
    //
    // `includeStructural=false` additionally drops the by-runOn / no-emulator
    // lines. A corpus-wide caller wants that: on a Windows host ~1100 of the
    // corpus's arms are `runOn`-excluded, they are fully predictable from the
    // manifest, and listing every one of them buries the environmental and
    // harness rows that a reader actually has to act on. Their COUNT is never
    // dropped — it is in `renderCountsLine()`, which is the accounting this
    // whole ledger exists to produce.
    [[nodiscard]] std::string renderSkipDetail(std::string_view indent = "  ",
                                               bool includeStructural = true) const {
        std::ostringstream o;
        for (auto const& r : records_) {
            if (armVerdictIsVerified(r.verdict)) continue;
            if (!includeStructural && armVerdictIsStructuralSkip(r.verdict)) {
                continue;
            }
            o << indent << '[' << armVerdictName(r.verdict) << "] "
              << r.example << " spec=" << r.spec << " arm=" << r.arm;
            if (!r.detail.empty()) o << " — " << r.detail;
            o << '\n';
        }
        return o.str();
    }

private:
    std::vector<ArmVerdictRecord> records_;
};

// ── Manifest lint: a cross-arch-capable arm must declare an emulator ───────
//
// D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR. Exactly ONE manifest in a
// 557-manifest corpus declared an `arm64:elf64-aarch64-linux-exec` arm with no
// `emulator`, against 449 siblings that declare `qemu-aarch64` on the same
// (arch, runOn-OS) pair — so it was skipped on every x86_64 Linux host forever
// and could only ever run on the native arm64 CI leg. Nothing asserted the
// omission; it was found by grepping 557 files.
//
// WHY THE RULE IS CORPUS-CONSISTENCY AND NOT "cross-arch ⇒ needs an emulator".
// Two alternatives were considered and are wrong:
//
//   * "every runnable arm needs an emulator" would demand one from all 443
//     native `x86_64 + windows` arms, which need none.
//   * "an arm whose arch differs from THIS host needs an emulator" is
//     host-relative, so it depends on which leg notices — and on an x86_64 Mac
//     it would demand an emulator from all 514 `arm64 + darwin` arms, for which
//     none exists. Demanding a declaration that cannot honestly be made is
//     worse than the silence it replaces.
//
// The rule implemented here is host-INDEPENDENT and derived entirely from the
// corpus's own declarations: if the corpus declares emulator E for (arch, os)
// ANYWHERE, then every arm with that (arch, os) must declare E. It fires on
// Windows, Linux and macOS alike — i.e. at authoring time on whichever leg the
// author runs — needs no hardcoded arch/OS/emulator vocabulary, and stays
// silent for a pair no emulator has ever been declared for (where the harness
// genuinely cannot know one exists).

// One declared (target arm × runOn OS) pair, flattened out of a manifest.
struct DeclaredArm {
    std::string manifest;  // stable id for the message, e.g. "c/<name>"
    std::string spec;
    std::string runOnOs;   // ONE OS name from the target's `runOn`
    std::string emulator;  // "" ⇒ none declared
};

struct EmulatorLintFinding {
    std::string manifest;  // "" for a corpus-wide (vocabulary) finding
    std::string spec;
    std::string arch;
    std::string runOnOs;
    std::string message;
};

// PURE over the flattened declarations — no filesystem, no host, fully
// unit-testable (see tests/test_support/test_arm_verdict_ledger.cpp).
[[nodiscard]] inline std::vector<EmulatorLintFinding>
lintDeclaredEmulators(std::vector<DeclaredArm> const& arms) {
    using Key = std::pair<std::string, std::string>;  // (arch, runOnOs)
    std::map<Key, std::set<std::string>> declared;    // non-empty emulators
    std::map<Key, std::size_t>           declaringCount;
    for (auto const& a : arms) {
        Key const k{specTargetArch(a.spec), a.runOnOs};
        declared.try_emplace(k);
        if (!a.emulator.empty()) {
            declared[k].insert(a.emulator);
            ++declaringCount[k];
        }
    }

    std::vector<EmulatorLintFinding> findings;

    // (1) Ambiguous vocabulary: one (arch, os) pair, two emulator spellings.
    // Reported BEFORE the omissions, because with two candidates the harness
    // cannot say which one an omitting arm should have declared.
    for (auto const& [k, names] : declared) {
        if (names.size() <= 1u) continue;
        std::ostringstream o;
        o << "the corpus declares " << names.size()
          << " different emulators for (arch=" << k.first
          << ", runOn=" << k.second << "):";
        for (auto const& n : names) o << " '" << n << "'";
        o << " — one (arch, runOn) pair must have ONE emulator vocabulary, or"
             " an omitting arm cannot be told what it is missing";
        findings.push_back({/*manifest*/ "", /*spec*/ "", k.first, k.second,
                            o.str()});
    }

    // (2) The omission this lint exists for.
    for (auto const& a : arms) {
        if (!a.emulator.empty()) continue;
        Key const k{specTargetArch(a.spec), a.runOnOs};
        auto const it = declared.find(k);
        if (it == declared.end() || it->second.size() != 1u) continue;
        std::ostringstream o;
        o << "declares no 'emulator' while " << declaringCount[k]
          << " sibling arm(s) with the same (arch=" << k.first
          << ", runOn=" << k.second << ") declare '" << *it->second.begin()
          << "' — this arm is silently skipped on every host of a different"
             " arch (D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR)";
        findings.push_back({a.manifest, a.spec, k.first, k.second, o.str()});
    }
    return findings;
}

}  // namespace dss::test_support
