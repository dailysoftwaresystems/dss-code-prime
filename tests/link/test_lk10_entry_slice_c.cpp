// D-LK10-ENTRY Slice C (plan 14 §2.13): the runnable-binary spine
// — Stage 1 acceptance test. THE FIRST DSS-PRODUCED BINARY THE OS
// ACTUALLY RUNS.
//
// This test substrate is unique: every prior link test asserts
// bytes-in-memory; this one writes the bytes to disk, spawns the
// resulting binary via CreateProcess on the Windows host, waits
// for the child to exit, and asserts the OS-reported exit code.
// Per the user's explicit invariants for Slice C:
//   (a) imageEntryOverride is optional<size_t>, NOT a 0-sentinel —
//       index 0 is a valid override (the trampoline sits at
//       functions[0]).
//   (b) The run-harness GENUINELY spawns the file and asserts the
//       OS exit code — not mocked, not in-memory.
//   (c) The synthetic ExitProcess ExternImport threads correctly
//       through the existing PE IAT writer (LK6 cycle 2a) — the
//       produced binary has a real IAT slot resolving to
//       kernel32!ExitProcess at load time.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/entry_trampoline.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link/writer.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

// User entry function body: `mov eax, 42; ret`. The trampoline
// emitter wraps this with the Slice C `_start` trampoline that
// calls into it, moves the return value (rax = 42) into the
// status-arg register (rcx on MS x64), and invokes
// kernel32!ExitProcess(42) via the IAT.
//
// Hand-assembled bytes (NOT through MIR/LIR — the test fixture
// stands alone for substrate clarity):
//   B8 2A 00 00 00     mov eax, 42 (sign-extended imm32)
//   C3                 ret
//
// The trampoline does NOT emit a trailing `ret`: its control flow
// is `call user_entry → mov ecx, eax → call_indirect_via_extern
// ExitProcess → unreachable`. The user fn's `ret` IS reached
// (returning into the trampoline body), but no `ret` is reachable
// inside the trampoline itself.
[[nodiscard]] AssembledModule makeReturn42Module() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    mod.functions.push_back(std::move(fn));
    return mod;
}

}  // namespace

// ── Substrate / bytes-only pins (no run required) ──────────────────

TEST(LK10EntrySliceC, TrampolineInjectionPrependsFunctionAndSetsOverride) {
    // The linker hook, when invoked on a PE Exec module, must
    // prepend a synthetic trampoline as functions[0] and set
    // imageEntryOverride. Pins the structural side-effects without
    // running the binary.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // The produced LinkedImage is the trampoline-wrapped binary.
    // The expectedFuncCount must reflect the prepended trampoline.
    EXPECT_EQ(image.expectedFuncCount, 2u)
        << "linker should have prepended trampoline → "
           "expectedFuncCount = 1 (user) + 1 (trampoline) = 2";
    EXPECT_FALSE(image.bytes.empty())
        << "PE walker should have emitted bytes";
    EXPECT_TRUE(image.ok())
        << "LinkedImage.ok() = (resolvedFuncCount == expectedFuncCount)";
}

TEST(LK10EntrySliceC, ImageEntryOverrideOptionalDiscriminantMatters) {
    // (a) invariant pin: imageEntryOverride is std::optional<size_t>
    // — "present, value 0" is DISTINCT from "absent". The
    // trampoline-prepended module's override is value 0 (trampoline
    // genuinely at functions[0]); a caller-side test fixture with
    // empty override is the "absent" case.
    AssembledModule mod;
    EXPECT_FALSE(mod.imageEntryOverride.has_value())
        << "default-constructed AssembledModule has nullopt "
           "override (no trampoline, fall through to entryPoint)";
    mod.imageEntryOverride = std::optional<std::size_t>{0};
    EXPECT_TRUE(mod.imageEntryOverride.has_value());
    EXPECT_EQ(*mod.imageEntryOverride, 0u)
        << "value 0 is valid — distinguished from nullopt by "
           "has_value() (NOT by a 0-sentinel — see field docblock)";
}

TEST(LK10EntrySliceC, SyntheticExitProcessExternThreadsThroughIat) {
    // (c) invariant pin: the trampoline's synthetic
    // ExternImport{kernel32.dll, ExitProcess} must be injected so
    // the PE walker's existing IAT writer (LK6 cycle 2a) emits a
    // real IAT slot. Verify by inspecting the post-link module's
    // externImports after running the trampoline emitter directly
    // (the linker copies the module internally, so we replicate the
    // injection path here for visibility).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    ASSERT_TRUE(mod.externImports.empty())
        << "user module has no externImports pre-injection";
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, **format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    // Post-injection: 1 synthetic ExternImport for ExitProcess.
    ASSERT_EQ(mod.externImports.size(), 1u);
    EXPECT_EQ(mod.externImports[0].libraryPath, "kernel32.dll");
    EXPECT_EQ(mod.externImports[0].mangledName, "ExitProcess");
    // The trampoline at functions[0] has a Relocation targeting the
    // synthetic ExternImport's SymbolId — that reloc patches the
    // disp32 in `FF 15 disp32` to the IAT slot's RVA at link time.
    ASSERT_GE(mod.functions.size(), 2u);
    auto const& tramp = mod.functions[0];
    bool found = false;
    for (auto const& r : tramp.relocations) {
        if (r.target == mod.externImports[0].symbol) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "trampoline must have a Relocation targeting the "
           "synthetic ExitProcess SymbolId — that's what threads "
           "through the PE walker's IAT writer";
    // The trampoline must also have a Relocation targeting the
    // user function (the direct `call user_entry`).
    bool userFound = false;
    for (auto const& r : tramp.relocations) {
        if (r.target == SymbolId{1}) {
            userFound = true;
            break;
        }
    }
    EXPECT_TRUE(userFound)
        << "trampoline must call the user entry fn via REL32";
    EXPECT_TRUE(mod.imageEntryOverride.has_value());
    EXPECT_EQ(*mod.imageEntryOverride, 0u);
}

// ── Fail-loud pins added at Slice C audit fold (3-agent convergence) ──

TEST(LK10EntrySliceC, ImageEntryOverrideOutOfRangeFailsLoud) {
    // test-analyzer H1 (Slice C audit fold): the 5 walker sites
    // each guard `*override >= functions.size()` with K_SymbolUndefined.
    // The shared `resolveEntryFnIdx` helper is the single enforcement
    // point post-fold; this test pins the contract.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    mod.imageEntryOverride = std::optional<std::size_t>{42};  // way out
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **format, rep);
    EXPECT_TRUE(image.bytes.empty())
        << "out-of-range imageEntryOverride must reject (no bytes)";
    EXPECT_GT(rep.errorCount(), 0u)
        << "must emit K_SymbolUndefined";
}

TEST(LK10EntrySliceC, SymbolIdSpaceExhaustionFailsLoud) {
    // 3-agent convergence (silent-failure + test-analyzer + dim-2):
    // module with SymbolId at UINT32_MAX boundary would silently
    // wrap `maxV+1` / `maxV+2` to 0/1 — collide with real SymbolIds.
    // Slice C audit fold added a fail-loud guard at the mint site.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(format.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{std::numeric_limits<std::uint32_t>::max()};
    fn.bytes  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    ASSERT_FALSE(linker::injectEntryTrampoline(
        mod, **target, **format, rep))
        << "trampoline injection must fail loud at UINT32_MAX wrap";
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(LK10EntrySliceC, LinkerSkipsInjectionWhenOverrideAlreadyPresent) {
    // test-analyzer M3 + dim-2 HIGH 2 (Slice C audit fold): the
    // linker's bypass condition `!imageEntryOverride.has_value()`
    // lets caller-injected trampolines pass through unchanged. A
    // regression dropping the check would double-inject. Pre-set
    // override + a sentinel function; the linker must NOT prepend
    // a second trampoline.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    // Caller pre-states "this module already has its trampoline".
    mod.imageEntryOverride = std::optional<std::size_t>{0};
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **format, rep);
    // 1 user fn + no trampoline added = expectedFuncCount stays 1.
    EXPECT_EQ(image.expectedFuncCount, 1u)
        << "linker must NOT double-inject when imageEntryOverride is "
           "already set by the caller — bypass guard at linker.cpp.";
}

// ── Stage-1 runnable smoke (Windows host only) ─────────────────────

#if defined(_WIN32)
TEST(LK10EntrySliceC, RunnableBinaryExitFortyTwo) {
    // (b) THE acceptance test: write a PE binary to disk and run
    // it. The trampoline wraps `mov eax, 42; ret` such that the OS
    // sees exit code 42.
    //
    // Pipeline: user-fn bytes → AssembledModule → linker::link()
    // (trampoline injected) → writeImage(.exe on disk) → runBinary
    // (CreateProcess + WaitForSingleObject + GetExitCodeProcess) →
    // assert exit == 42.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "link must succeed cleanly";
    ASSERT_FALSE(image.bytes.empty());
    ASSERT_TRUE(image.ok());

    ScratchDir scratch{Location::Temp, "lk10-entry"};
    auto const exePath = scratch.path() / "return42.exe";
    ASSERT_TRUE(linker::writeImage(image, exePath, rep))
        << "writeImage must succeed";

    auto const result = runBinary(exePath, std::chrono::milliseconds{5000});
    ASSERT_TRUE(result.spawned)
        << "CreateProcess must succeed for the emitted .exe — if "
           "this fails, the binary is structurally invalid at the "
           "Windows loader level (D-LK10-ENTRY substrate gap). "
           "Diagnostic: " << result.diagnostic;
    EXPECT_FALSE(result.timedOut)
        << "binary should exit promptly via ExitProcess(42)";
    EXPECT_EQ(result.exitCode, 42u)
        << "THE acceptance criterion: OS-reported exit code must "
           "be 42 (the user fn's return value, threaded through "
           "the Slice C trampoline → mov ecx, eax → ExitProcess "
           "via IAT). Got: " << result.exitCode;
}
#endif
