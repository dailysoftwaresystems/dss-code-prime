#pragma once

#include <string>
#include <string_view>

// The HOST-PLATFORM token vocabulary — the ONE spelling of `windows` /
// `linux` / `darwin` shared by everything that has to name the machine the
// compiler is RUNNING ON.
//
// WHAT ASKS THIS QUESTION. Driver-tier features that run something on the
// developer's machine before/around a compile. The first is the
// `.dss-project.json` build-script gate:
//
//     { "run": ["bash", "gen.sh"], "runOn": ["linux", "darwin"] }
//
// A pre-build script is a HOST question by construction — it is a process this
// machine must fork and execute, so `bash gen.sh` is answerable only in terms
// of the OS underfoot. The corpus manifests (`examples/*/*/expected.json`) ask
// the same question in the same words when they declare which hosts may SPAWN a
// produced binary, and `tests/test_support/arm_verdict_ledger.hpp` compares a
// declared `runOn` against the host to decide `SkippedByRunOn`.
//
// THE VOCABULARY IS CLOSED AND CASE-SENSITIVE: exactly `windows`, `linux`,
// `darwin`. MEASURED across the shipped corpus at the time of writing — 1868
// `runOn` entries over the example manifests, 903 `linux` / 520 `darwin` / 445
// `windows`, and NOTHING else. It is `darwin` (the kernel name `uname -s`
// prints), never `macos` / `macOS` / `osx`; `windows`, never `win32` / `Win32`.
// Accepting a near-miss spelling would be the worst outcome available here: a
// mis-cased `"Linux"` in a project file must be a LOUD rejection naming the
// three legal tokens, not a silently-never-matching gate that quietly drops a
// build step and produces a subtly different artifact.
//
// ⚠ THIS IS THE HOST, NEVER THE TARGET — and it is NOT a violation of the
// source/target/linker-agnostic rule, for the same reason
// `tests/test_support/host_native_target.hpp` is not. That rule governs
// COMPILATION: what the engine emits must be keyed on the configured TARGET
// (target schema + object-format schema) and never on the machine doing the
// emitting, so that a Windows host and a Linux host handed the same target
// produce byte-identical output. The question THIS header answers is the
// deliberate opposite one — "what machine am I running on, so that I may decide
// which build scripts to execute" — and it is legitimate only in the driver
// tier, where running a host process is the actual subject.
//
// So, bluntly: NO codegen, semantic-analysis, preprocessor, optimisation or
// link decision may ever read `currentHostOs()` or a `runOn` token. Not to pick
// a default target, not to pick an object format, not to choose a calling
// convention, not to gate a diagnostic, not to select a path separator in
// emitted output. If a value derived from this header can reach a byte of the
// produced artifact, that is the bug — the artifact must depend on the target
// configuration alone.
//
// WHY IT LIVES IN `src/program/` AND NOT `src/core/`. `src/core/types/` is
// included by the preprocessor, the semantic analyser, every backend and the
// linker. A host-OS name string sitting in that include surface is one grep
// away from seeding exactly the host-keyed branch the paragraph above forbids;
// the placement is the enforcement. `src/program/` is the driver — the only
// tier where "which host am I" is a question worth asking at all.
//
// SELF-CONTAINED ON PURPOSE. This header includes nothing but `<string>` and
// `<string_view>` — no `core/export.hpp`, no project header — because
// `tests/test_support/arm_verdict_ledger.hpp` includes it by relative path for
// the benefit of the `integrated_tests` target, which does not carry `src/` on
// its include path (see the include note there). Everything below is `inline` /
// `constexpr`, so consuming it costs no link dependency either. Keep it that
// way: an added include here would break a consumer that never links the lib.

namespace dss {

// The closed vocabulary. Order is the canonical listing order used by
// `runOnTokenList()` in diagnostics; membership, not order, is the contract.
//
// ADDING A TOKEN IS A DELIBERATE ACT. `tests/program/test_platform_token.cpp`
// static_asserts the cardinality, so growing this array is a BUILD error until
// the test's expectations are updated in the same change — no fourth spelling
// slips in as a side effect of some other edit, and no near-miss alias
// (`macos`, `osx`, `win32`) gets added "for compatibility" without a human
// deciding that the corpus's 1868 existing entries should be re-spelled too.
inline constexpr std::string_view kRunOnPlatformTokens[] = {
    "windows",
    "linux",
    "darwin",
};

// Exact, CASE-SENSITIVE membership. `"Linux"`, `"LINUX"`, `"macos"`, `"osx"`
// and `"darwin "` (trailing space) are all false — see the header note: a
// near-miss must be rejected loudly by the caller, never silently tolerated
// into a gate that can then never match.
//
// `constexpr` so a caller can validate a literal at COMPILE time.
[[nodiscard]] constexpr bool isValidRunOnToken(std::string_view token) noexcept {
    for (std::string_view const candidate : kRunOnPlatformTokens) {
        if (candidate == token) return true;
    }
    return false;
}

// The vocabulary as a `", "`-joined list for diagnostics ("expected one of:
// windows, linux, darwin"), matching the join style used throughout the
// driver's config diagnostics.
//
// DERIVED from `kRunOnPlatformTokens`, never a hand-written duplicate string:
// a rejection message that lists a vocabulary the validator does not actually
// implement is worse than no message, because it sends the reader to fix a
// spelling that was already legal.
[[nodiscard]] inline std::string runOnTokenList() {
    std::string list;
    for (std::string_view const token : kRunOnPlatformTokens) {
        if (!list.empty()) list += ", ";
        list += token;
    }
    return list;
}

// The host this binary is RUNNING on, spelled in the vocabulary above.
//
// THE `#else` IS AN `#error`, NOT AN `"unknown"`. A fallback string would be a
// token no `runOn` list can ever contain, so every gate keyed on it would
// silently evaluate to "never run this" — a build that quietly omits its
// pre-build steps and then fails somewhere unrecognisable downstream. There is
// no host the compiler builds on that is not one of these three, so an
// unrecognised host is a porting task, and it should stop the BUILD, loudly, at
// the one line that needs the new arm.
//
// The ordering of the arms matters: `__APPLE__` is checked before `__linux__`
// because Apple platforms define neither `__linux__` nor `_WIN32`, but the
// converse habit of testing `__unix__` first is how a macOS host ends up
// mislabelled. `_WIN32` is defined on 64-bit Windows too (it means "the Win32
// API", not "32-bit"), so it needs no `_WIN64` companion.
[[nodiscard]] inline std::string_view currentHostOs() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    // Add the arm here AND its token to kRunOnPlatformTokens above (which will
    // redden the cardinality static_assert in tests/program/test_platform_token.cpp
    // until that test is updated too). Never an 'unknown' fallback: it would
    // silently disable every runOn-gated build step on the new host.
#  error "platform_token.hpp: unrecognised host OS - no runOn token for it"
#endif
}

} // namespace dss
