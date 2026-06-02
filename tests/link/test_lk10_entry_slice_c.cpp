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
#include "diagnostic_count.hpp"
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
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
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

// Build a PE-Exec ObjectFormatSchema by loading the shipped
// pe64-x86_64-windows-exec format JSON and patching its
// `entryPoint` field. Used by the D-LK10-ENTRY-EXTERN-ENTRY-DIAG
// test path to exercise the case where the format names a
// synthesized symbol that lives in `externImports[]` rather than
// `functions[]`. Riding on the shipped JSON (rather than inlining
// a 60-line literal) keeps the test aligned with whatever fields
// the format schema validator requires today — no test-side
// staleness as the format substrate evolves.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
loadPeExecWithEntryPoint(std::string const& entryName) {
    // Walk up from cwd looking for the shipped JSON file — mirrors
    // the ancestor-walk in `findShippedConfig` so this test works
    // whether ctest invokes from build/ or the repo root.
    namespace fs = std::filesystem;
    fs::path shipped;
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        fs::path const candidate = here / "src" / "dss-config"
            / "object-formats" / "pe64-x86_64-windows-exec.format.json";
        if (fs::exists(candidate)) {
            shipped = candidate;
            break;
        }
        fs::path const parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
    if (shipped.empty()) return nullptr;
    std::ifstream f{shipped};
    std::stringstream buf;
    buf << f.rdbuf();
    std::string jsonStr = buf.str();
    // The shipped file declares `"entryPoint": ""`. Replace the
    // empty value with the parameterized name. A single literal
    // unique to the field anchors the substitution.
    std::string const needle    = R"("entryPoint": "")";
    std::string const replaced  = R"("entryPoint": ")" + entryName + R"(")";
    auto const pos = jsonStr.find(needle);
    if (pos == std::string::npos) return nullptr;
    // Silent-failure F1 pin (3-stream audit fold): if the shipped
    // JSON ever grows a second `"entryPoint": ""` substring (e.g.
    // a comment reproduces the literal verbatim), `find` would
    // silently match the wrong site. Require uniqueness so a future
    // config drift fails loud at the test helper rather than
    // silently producing a malformed format.
    if (jsonStr.find(needle, pos + needle.size())
            != std::string::npos) {
        return nullptr;
    }
    jsonStr.replace(pos, needle.size(), replaced);
    auto result = ObjectFormatSchema::loadFromText(jsonStr);
    if (!result.has_value()) return nullptr;
    return std::move(result).value();
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
    // silent-failure HIGH (7f8b843 audit fold): pin SPECIFIC code,
    // not just errorCount>0 — a regression firing the wrong
    // diagnostic would silently satisfy a loose check.
    EXPECT_GT(::dss::test_support::countCode(
                  rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "out-of-range must fire K_SymbolUndefined specifically";
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
    EXPECT_GT(::dss::test_support::countCode(
                  rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "UINT32_MAX wrap must fire K_SymbolUndefined specifically";
}

// code-architect FOLD-NOW (7f8b843 audit fold): all prior Slice C
// tests exercise the PE/ByNameImport arm of the trampoline. The
// ELF/Syscall arm (Linux exit_group=231 via SYSCALL) is structurally
// covered by the same emitter but has no dedicated pin. Bytes-only
// pin via injectEntryTrampoline on `elf64-x86_64-linux-exec` format
// — avoids the POSIX-RUN-HARNESS wait (anchored D-LK10-ENTRY-POSIX-
// RUN-HARNESS) while still catching Syscall-arm regressions.
TEST(LK10EntrySliceC, SyscallArmInjectsCleanlyOnElfExec) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, **format, rep))
        << "Syscall-arm trampoline injection must succeed on "
           "elf64-x86_64-linux-exec";
    EXPECT_EQ(rep.errorCount(), 0u);
    // Module post-injection: 2 functions (trampoline at [0], user
    // at [1]) + ZERO synthetic externImports (Syscall arm doesn't
    // need an IAT slot — distinguishes Syscall from ByNameImport).
    ASSERT_EQ(mod.functions.size(), 2u);
    EXPECT_TRUE(mod.externImports.empty())
        << "Syscall arm must NOT inject a synthetic ExternImport "
           "— that's only the ByNameImport arm's behavior.";
    // Trampoline at functions[0] has the relocation to user_entry
    // (REL32 for the direct `call`). No ExitProcess reloc.
    ASSERT_GE(mod.functions[0].relocations.size(), 1u);
    bool userCallFound = false;
    for (auto const& r : mod.functions[0].relocations) {
        if (r.target == SymbolId{1}) {
            userCallFound = true;
            break;
        }
    }
    EXPECT_TRUE(userCallFound)
        << "trampoline must call the user fn (SymbolId{1}) via REL32";
    EXPECT_TRUE(mod.imageEntryOverride.has_value());
    EXPECT_EQ(*mod.imageEntryOverride, 0u);
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

// ── D-LK10-ENTRY-TRAMP-PROLOGUE byte pins ──────────────────────────
//
// The acceptance test below (RunnableBinaryExitFortyTwo) is gated
// `#if defined(_WIN32)` and asserts the OS-reported exit code.
// That's the end-to-end pin, but it's invisible to non-Windows CI.
//
// These two tests pin the EMITTED-BYTES surface of the trampoline
// prologue so a bias regression (e.g. someone "tidying"
// `ms_x64.entryStackPointerBias` back to 0) is caught on EVERY host,
// not only the one with `CreateProcess`. Strong convergence at the
// standing 7-agent audit (test-analyzer Gap 3 + Gap 4 + silent-
// failure HIGH-3 all flagged the host-dependence of the regression
// floor).

TEST(LK10EntrySliceC, MsX64TrampolinePrologueIsSubRsp0x28) {
    // Win64 entry cc has entryStackPointerBias=8 + shadowSpaceBytes=32
    // → alignedSizeWithBias = 40 = 0x28 → trampoline's first opcode
    // MUST be `sub rsp, 0x28` (REX.W + opcode 81 /5 + ModR/M C4 +
    // imm32 LE). Encoded bytes: `48 81 EC 28 00 00 00`.
    //
    // A regression to this number re-opens STATUS_ACCESS_VIOLATION
    // (0xC0000005) on ExitProcess's aligned-SSE stores — the exact
    // bug closed by D-LK10-ENTRY-TRAMP-PROLOGUE.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, **format, rep));
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(mod.functions.empty());
    auto const& trampBytes = mod.functions[0].bytes;
    ASSERT_GE(trampBytes.size(), 7u)
        << "Win64 trampoline must lead with a 7-byte `sub rsp, imm32`";
    EXPECT_EQ(trampBytes[0], 0x48u) << "REX.W prefix";
    EXPECT_EQ(trampBytes[1], 0x81u) << "SUB r/m64, imm32 opcode";
    EXPECT_EQ(trampBytes[2], 0xECu) << "ModR/M: mod=11 reg=/5 rm=100=rsp";
    EXPECT_EQ(trampBytes[3], 0x28u)
        << "imm32 LE byte 0 — value 40 = 32 shadow + 8 align. "
           "Regression to 0x00 indicates ms_x64.entryStackPointerBias "
           "was zeroed; regression to other values indicates "
           "alignedSizeWithBias formula drift.";
    EXPECT_EQ(trampBytes[4], 0x00u) << "imm32 LE byte 1";
    EXPECT_EQ(trampBytes[5], 0x00u) << "imm32 LE byte 2";
    EXPECT_EQ(trampBytes[6], 0x00u) << "imm32 LE byte 3";
}

TEST(LK10EntrySliceC, SysvElfTrampolineEmitsNoPrologue) {
    // Negative pin: sysv_amd64 has entryStackPointerBias=0 +
    // shadowSpaceBytes=0 → alignedSizeWithBias = 0 → NO `sub rsp,
    // ...` op emitted. The trampoline's first instruction must be
    // the `call user_entry` (`E8 disp32`), NOT the REX.W SUB
    // prefix (`0x48`). A regression that always emitted the
    // prologue (e.g. dropping the `if (adjustBytes > 0)` guard)
    // would drift every downstream RVA by 7 bytes silently.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, **format, rep));
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(mod.functions.empty());
    auto const& trampBytes = mod.functions[0].bytes;
    ASSERT_FALSE(trampBytes.empty());
    // First byte cannot be the REX.W prefix that begins `sub rsp, ...`
    // For SysV the trampoline starts with `E8` (CALL rel32) directly.
    EXPECT_NE(trampBytes[0], 0x48u)
        << "SysV trampoline must NOT emit a prologue (cc.shadow=0, "
           "cc.entryBias=0 → adjustBytes=0). A `0x48` first byte "
           "indicates entryStackPointerBias was set non-zero on "
           "sysv_amd64 OR the `if (adjustBytes > 0)` guard was lost.";
    EXPECT_EQ(trampBytes[0], 0xE8u)
        << "SysV trampoline's first instruction is `call user_entry` "
           "directly (no prologue).";
}

// ── D-LK10-ENTRY-EXTERN-ENTRY-DIAG distinct diagnostic ─────────────
//
// `resolveUserEntrySymbol` returns one of three statuses:
//   * Found            — entry name matches an AssembledFunction
//   * NotFound         — entry name matches NOTHING in the module
//   * ResolvedToExtern — entry name matches an ExternImport (invalid)
//
// The two failure modes need DISTINCT diagnostic codes because the
// user-visible remediation differs. The previous shape returned
// `optional<SymbolId>` with nullopt for both cases — the caller
// emitted a single combined message. Closure pins both arms with
// their own diagnostic codes:
//   K_SymbolUndefined            for NotFound
//   K_EntryPointResolvesToExtern for ResolvedToExtern

TEST(LK10EntrySliceC, ExternAsEntryFiresDistinctDiagnostic) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // Custom PE-exec format whose entryPoint names the synthesized
    // symbol "sym_42" — same prefix the trampoline emitter uses for
    // ELF/PE.
    auto format = loadPeExecWithEntryPoint("sym_42");
    ASSERT_NE(format, nullptr)
        << "loadPeExecWithEntryPoint returned null — either the "
           "shipped pe64-x86_64-windows-exec.format.json's `\"entryPoint\": "
           "\"\"` literal was changed (string-replace pattern broke), "
           "or the patched JSON failed schema validation. Check the "
           "shipped file's entryPoint field.";
    // Module with ONE function (SymbolId{1}; entryName 'sym_42'
    // won't match it) + ONE extern (SymbolId{42}; entryName WILL
    // match its synthesized name 'sym_42').
    auto mod = makeReturn42Module();
    ExternImport extern_;
    extern_.symbol      = SymbolId{42};
    extern_.mangledName = "SomeImportedFn";
    extern_.libraryPath = "some.dll";
    mod.externImports.push_back(std::move(extern_));

    DiagnosticReporter rep;
    bool const ok = linker::injectEntryTrampoline(
        mod, **target, *format, rep);
    EXPECT_FALSE(ok)
        << "extern-as-entry must reject the trampoline injection";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_EntryPointResolvesToExtern), 1u)
        << "must fire the EXTERN-specific code, NOT generic "
           "K_SymbolUndefined — closes D-LK10-ENTRY-EXTERN-ENTRY-DIAG";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "K_SymbolUndefined must NOT fire here — that's the "
           "NotFound arm, distinct from this case";
}

TEST(LK10EntrySliceC, NoMatchingEntryFiresGenericUndefined) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // Custom format with entryPoint = "sym_999" which matches
    // NEITHER any function NOR any extern in the module → the
    // NotFound arm fires K_SymbolUndefined (NOT the new extern code).
    auto format = loadPeExecWithEntryPoint("sym_999");
    ASSERT_NE(format, nullptr);
    auto mod = makeReturn42Module();  // only SymbolId{1} declared

    DiagnosticReporter rep;
    bool const ok = linker::injectEntryTrampoline(
        mod, **target, *format, rep);
    EXPECT_FALSE(ok);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u)
        << "NotFound arm must fire K_SymbolUndefined";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_EntryPointResolvesToExtern), 0u)
        << "K_EntryPointResolvesToExtern is reserved for the extern "
           "case — must NOT fire when the name matches nothing";
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
