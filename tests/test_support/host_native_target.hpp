#pragma once

#include <string>
#include <string_view>

// The HOST's own native target/format spellings — the single place a test that
// BUILDS an artifact and then SPAWNS it decides what to build for.
//
// D-TEST-HOST-SPAWNS-FOREIGN-BINARY. A test that spawns its own output must
// build for the machine it is running on; anything else emits a foreign binary
// and `posix_spawn` fails with rc=8 (ENOEXEC). That defect has now been found
// TWICE, in the same shape, because each site carried its OWN `#if` ladder:
//
//   * TF-C69 — the ladder read "Windows or else Linux", so macOS silently got an
//     elf64-x86_64 binary and the suite could never be green on a Mac. Fixed by
//     giving the __APPLE__ branch an __aarch64__ arm.
//   * TF-C109 — the very same file's Linux `#else` branch was never given the
//     matching arm, so the NATIVE arm64 CI leg (ubuntu-24.04-arm) built an
//     x86_64 ELF and spawned it: `posix_spawn(...) failed: rc=8`, 771/772.
//
// One ladder was fixed and its duplicate was not. That is the multi-site class
// the bar names explicitly: funnel every site through ONE chokepoint so the
// coverage is by-construction, because duplication is what breeds the missed
// site. Add a host here ONCE and every build-and-spawn site gets it.
//
// ⚠ NOT a violation of the source/target/linker-agnostic rule. That rule governs
// the ENGINE (`src/`), where logic must be keyed on the configured TARGET and
// never on the host. This header is the deliberate opposite and belongs only to
// tests: the question it answers is literally "what machine am I running on, so
// that I may execute what I just built". The compiler under test still selects
// its target purely from the target/format config it is handed.
//
// Tests that only need STRUCTURAL coverage should NOT use this — they should
// build for a fixed target on every host and gate just the RUN on a host match
// (see `tests/program/test_static_link.cpp`). Use this when the assertion is the
// round-trip itself and must stay LIVE on every host rather than be skipped.
namespace dss::test_support {

struct HostNativeTarget {
    std::string_view libTarget;   // shared-library flavor (ET_DYN / dylib / dll)
    std::string_view execTarget;  // executable flavor
    std::string_view libSuffix;   // ".so" / ".dylib" / ".dll"
    std::string_view exeSuffix;   // "" / ".exe"
};

// The host's native quadruple. Every arm is a SHIPPED target/format pair.
[[nodiscard]] inline HostNativeTarget hostNativeTarget() noexcept {
#if defined(_WIN32)
    return {"x86_64:pe64-x86_64-windows-dll",
            "x86_64:pe64-x86_64-windows-exec", ".dll", ".exe"};
#elif defined(__APPLE__)
#  if defined(__aarch64__)
    return {"arm64:macho64-arm64-darwin-dylib",
            "arm64:macho64-arm64-darwin-exec", ".dylib", ""};
#  else
    return {"x86_64:macho64-x86_64-darwin-dylib",
            "x86_64:macho64-x86_64-darwin-exec", ".dylib", ""};
#  endif
#elif defined(__aarch64__)
    // The arm that was missing. `elf64-aarch64-linux-dyn` shipped in c171
    // (D-LK-ELF-DYN-AARCH64-FLAVOR) and the round-trip is MEASURED working:
    // DSS builds the ET_DYN with a real-named .dynsym export, an aarch64 exec
    // resolves its extern against it, and the program runs -> exit 42.
    return {"arm64:elf64-aarch64-linux-dyn",
            "arm64:elf64-aarch64-linux-exec", ".so", ""};
#else
    return {"x86_64:elf64-x86_64-linux-dyn",
            "x86_64:elf64-x86_64-linux-exec", ".so", ""};
#endif
}

// `stem` + the host's library/executable suffix, e.g. hostLibArtifact("dsslib").
[[nodiscard]] inline std::string hostLibArtifact(std::string_view stem) {
    return std::string{stem} + std::string{hostNativeTarget().libSuffix};
}
[[nodiscard]] inline std::string hostExeArtifact(std::string_view stem) {
    return std::string{stem} + std::string{hostNativeTarget().exeSuffix};
}

}  // namespace dss::test_support
