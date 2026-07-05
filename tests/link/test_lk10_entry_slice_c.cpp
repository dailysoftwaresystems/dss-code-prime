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
// is `call user_entry → mov ecx, eax → call ExitProcess →
// unreachable`. Under direct-plt (D-FFI-PE-IMPORT-THUNK) the
// ExitProcess call is a plain `call rel32` (E8) to the synthesized
// import thunk — was `call_indirect_via_extern` (FF 15) under the
// retired indirect-slot model. The user fn's `ret` IS reached
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

// AArch64 analogue of makeReturn42Module — a user fn that returns 42.
// Hand-assembled (NOT through MIR/LIR — the fixture stands alone):
//   D2 80 05 40   MOVZ X0, #42   (0xD2800540 = base | (42<<5) | Rd=0)
//   D6 5F 03 C0   RET            (0xD65F03C0, X30 implicit)
// Little-endian byte stream: 40 05 80 D2 | C0 03 5F D6.
// The trampoline emitter wraps this with the ELF/Syscall `_start`:
//   call user_entry → mov x0,x0 → mov x8,#94 → SVC #0 → BRK.
[[nodiscard]] AssembledModule makeReturn42ModuleArm64() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6};
    mod.functions.push_back(std::move(fn));
    return mod;
}

// STDIO-FLUSH-AT-EXIT (2026-06-09): the SHIPPED elf64-*-linux-exec
// formats now terminate via libc `exit(3)` (a by-name-import — see
// test_object_format_schema.cpp ShippedElfExecExitsViaLibcExitImport).
// The Syscall ARM of the trampoline emitter is still a fully supported
// capability (the right shape for a future hermetic/no-libc target), so
// the two byte/structure pins below keep exercising it via SYNTHETIC
// syscall formats rather than the shipped JSON — decoupling the
// Syscall-arm regression coverage from the shipped formats' policy.
//
// The synthetic format is a COMPLETE inline ELF-exec schema (all the
// fields the loader requires for a successful load — `elf` identity with
// pageAlign/type, entryPoint, externCallDispatch, sections), differing
// from the shipped format ONLY in the processExit block (syscall, not
// the libc-exit by-name-import). Built as a raw string (no nlohmann dep
// on this test target). injectEntryTrampoline reads only processExit /
// entryCallingConvention / externCallDispatch / entryPoint.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
makeSyscallElfExecFormat() {
    // x86_64 Linux exit_group=231 via SYSCALL (0x0F 0x05) in rax.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth-elf-syscall-x64", "version": "0.1", "kind": "elf" },
      "entryPoint": "",
      "externCallDispatch": "direct-plt",
      "elf": {
        "class": "elf64", "data": "lsb", "osabi": "sysv", "machine": 62,
        "type": "exec", "pageAlign": 4096,
        "interpreter": "/lib64/ld-linux-x86-64.so.2", "bindNow": true
      },
      "entryCallingConvention": "sysv_amd64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      },
      "sections": [
        { "kind": "text", "name": ".text", "type": 1, "flags": 6,
          "addrAlign": 16, "entrySize": 0, "virtualAddress": 4198400 }
      ]
    })");
    if (!r.has_value()) return nullptr;
    return *r;
}

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
makeSyscallElfExecFormatArm64() {
    // AArch64 Linux exit_group=94 via SVC #0 (0xD4000001) in x8.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": { "name": "synth-elf-syscall-arm64", "version": "0.1", "kind": "elf" },
      "entryPoint": "",
      "externCallDispatch": "direct-plt",
      "elf": {
        "class": "elf64", "data": "lsb", "osabi": "sysv", "machine": 183,
        "type": "exec", "pageAlign": 4096,
        "interpreter": "/lib/ld-linux-aarch64.so.1", "bindNow": true
      },
      "entryCallingConvention": "aapcs64",
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 94,
        "syscallNumGpr": "x8",
        "syscallOpcodeBytes": [1, 0, 0, 212]
      },
      "sections": [
        { "kind": "text", "name": ".text", "type": 1, "flags": 6,
          "addrAlign": 16, "entrySize": 0, "virtualAddress": 4194304 }
      ]
    })");
    if (!r.has_value()) return nullptr;
    return *r;
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
    // synthetic ExternImport's SymbolId — that reloc patches the call
    // disp32 (E8 disp32 under direct-plt; was FF 15 disp32 under the
    // retired indirect-slot model) to the ExitProcess import thunk's
    // RVA at link time.
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

TEST(LK10EntrySliceC, MaxExistingSymbolIdScansDataItems) {
    // D-LK4-RODATA-PRODUCER-STRING audit-fold pin (2026-06-02):
    // `entry_trampoline.cpp::maxExistingSymbolIdV` was extended to
    // scan `module.dataItems` so the trampoline's synthetic _start
    // and ExitProcess SymbolId mints can't collide with a string-
    // literal-promoted rodata global. Without this scan, the
    // collision would surface as `K_DuplicateDataSymbol` in the PE
    // walker's symbolVa loop (loud but root-cause-distant). This
    // test pins the defense: a module with a high-SymbolId rodata
    // item must produce trampoline mints STRICTLY greater than the
    // rodata symbol — regardless of how many user functions exist.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(format.has_value());
    AssembledModule mod = makeReturn42Module();  // 1 user fn at SymbolId{1}
    AssembledData rodata;
    rodata.symbol  = SymbolId{100};  // high; would collide if scan misses
    rodata.section = DataSectionKind::Rodata;
    rodata.bytes   = {'X', 0};
    rodata.alignment = Alignment::of<1>();
    mod.dataItems.push_back(std::move(rodata));
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, **format, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    // Trampoline injection adds a synthetic _start at functions[0]
    // and a synthetic ExitProcess extern. Both SymbolIds must be
    // strictly greater than 100 (the rodata item's SymbolId).
    ASSERT_GE(mod.functions.size(), 2u);
    EXPECT_GT(mod.functions[0].symbol.v, 100u)
        << "trampoline SymbolId must exceed rodata item's "
           "SymbolId — the audit-fold dataItems scan is the load-"
           "bearing defense against K_DuplicateDataSymbol";
    ASSERT_GE(mod.externImports.size(), 1u);
    EXPECT_GT(mod.externImports[0].symbol.v, 100u)
        << "synthetic ExitProcess SymbolId must also exceed "
           "rodata item's SymbolId";
    // Cross-check: the rodata symbol itself must remain in dataItems
    // with its original ID (the audit-fold extension is a SCAN, not
    // a rewrite).
    ASSERT_EQ(mod.dataItems.size(), 1u);
    EXPECT_EQ(mod.dataItems[0].symbol.v, 100u);
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
// pin via injectEntryTrampoline — avoids the POSIX-RUN-HARNESS wait
// (anchored D-LK10-ENTRY-POSIX-RUN-HARNESS) while still catching
// Syscall-arm regressions.
//
// STDIO-FLUSH-AT-EXIT (2026-06-09): the SHIPPED elf64-x86_64-linux-exec
// now terminates via libc `exit(3)` (by-name-import), so this Syscall-arm
// pin uses a SYNTHETIC syscall ELF-exec format (makeSyscallElfExecFormat)
// — keeping the Syscall-arm emitter coverage real + independent of the
// shipped format's policy.
TEST(LK10EntrySliceC, SyscallArmInjectsCleanlyOnElfExec) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = makeSyscallElfExecFormat();
    ASSERT_TRUE(format != nullptr)
        << "synthetic syscall ELF-exec format must load";
    auto mod = makeReturn42Module();
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, *format, rep))
        << "Syscall-arm trampoline injection must succeed on a "
           "syscall-mechanism ELF-exec format";
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
    // ...` op emitted. Since c88 (D-RUNTIME-MAIN-ARGC-ARGV) the
    // SHIPPED format's trampoline leads with the argc/argv
    // materialization pair (see ShippedElfX64TrampolineMaterializes
    // ArgcArgv for the byte pin); this test keeps the PROLOGUE
    // negative-pin: no `sub rsp, imm32` (48 81 EC) may appear
    // anywhere. A regression that always emitted the prologue
    // (e.g. dropping the `if (adjustBytes > 0)` guard) would drift
    // every downstream RVA by 7 bytes silently — and, worse post-
    // c88, would break the process-entry-SP-relative argc/argv
    // offsets if it slipped BEFORE the arg loads.
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
    // `sub rsp, imm32` = 48 81 EC — must appear NOWHERE in the SysV
    // trampoline (adjustBytes=0).
    for (std::size_t i = 0; i + 2 < trampBytes.size(); ++i) {
        bool const subRsp = trampBytes[i] == 0x48u
                         && trampBytes[i + 1] == 0x81u
                         && trampBytes[i + 2] == 0xECu;
        EXPECT_FALSE(subRsp)
            << "SysV trampoline must NOT emit a prologue (cc.shadow=0, "
               "cc.entryBias=0 → adjustBytes=0) — found `sub rsp, "
               "imm32` at byte offset " << i << ". Either "
               "entryStackPointerBias was set non-zero on sysv_amd64 "
               "OR the `if (adjustBytes > 0)` guard was lost.";
    }
}

// ── D-RUNTIME-MAIN-ARGC-ARGV (c88): shipped-ELF argc/argv byte pins ──
//
// The SHIPPED elf64-*-linux-exec formats declare `processArgs` =
// stack-vector {argc@[sp+0], argv@sp+8}, so the trampoline's FIRST two
// instructions materialize main's arguments from the process-entry
// stack (SysV AMD64 psABI §3.4.1 / AAPCS64 Linux) into the entry cc's
// argGprs[0..1] — BEFORE any SP adjustment and before `call/BL main`.
// Without them, `int main(int argc, char** argv)` reads process-entry
// register garbage (the c87-witnessed argc=846361312 that crashed the
// sqlite3 shell inside main while gcc's build answered 42).
//
// RED-on-disable: delete the `processArgs` block from either shipped
// format JSON (runtime-read config) and its pin fails on the first
// byte — the trampoline reverts to calling main with garbage argc.
// The synthetic-format pins (Arm64TrampolineEmitsExactExitSequence's
// EXACT 20-byte sequence) prove the converse: NO processArgs block ⇒
// byte-identical pre-c88 trampoline, zero arg-setup instructions.
TEST(LK10EntrySliceC, ShippedElfX64TrampolineMaterializesArgcArgv) {
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
    auto const& t = mod.functions[0].bytes;
    ASSERT_GE(t.size(), 17u)
        << "trampoline must lead with mov(8) + lea(8) + call(5)";
    // mov rdi, [rsp+0]  — REX.W 8B /r, ModR/M mod=10 reg=rdi(111)
    // rm=100(SIB), SIB=0x24 (base=rsp, no index), disp32=0.
    std::vector<std::uint8_t> const movArgc = {
        0x48, 0x8B, 0xBC, 0x24, 0x00, 0x00, 0x00, 0x00};
    // lea rsi, [rsp+8]  — REX.W 8D /r, ModR/M mod=10 reg=rsi(110)
    // rm=100(SIB), SIB=0x24, disp32=8.
    std::vector<std::uint8_t> const leaArgv = {
        0x48, 0x8D, 0xB4, 0x24, 0x08, 0x00, 0x00, 0x00};
    EXPECT_EQ(std::vector<std::uint8_t>(t.begin(), t.begin() + 8),
              movArgc)
        << "byte 0..7 must be `mov rdi, [rsp+0]` (argc → argGprs[0]). "
           "Drift here = the stack-vector argc load is wrong/missing "
           "— main(argc,argv) reads garbage argc again "
           "(D-RUNTIME-MAIN-ARGC-ARGV).";
    EXPECT_EQ(std::vector<std::uint8_t>(t.begin() + 8, t.begin() + 16),
              leaArgv)
        << "byte 8..15 must be `lea rsi, [rsp+8]` (the in-place argv "
           "vector's ADDRESS → argGprs[1]). A load (8B) here instead "
           "of lea (8D) would DEREFERENCE the vector — argv would be "
           "argv[0].";
    EXPECT_EQ(t[16], 0xE8u)
        << "the `call user_entry` must follow the two arg-setup "
           "instructions directly (SysV: no prologue between).";
}

TEST(LK10EntrySliceC, ShippedElfArm64TrampolineMaterializesArgcArgv) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42ModuleArm64();
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, **format, rep));
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(mod.functions.empty());
    auto const& t = mod.functions[0].bytes;
    ASSERT_EQ(t.size() % 4u, 0u);
    ASSERT_GE(t.size(), 12u)
        << "trampoline must lead with LDUR + ADD + BL";
    auto const word = [&](std::size_t i) {
        return static_cast<std::uint32_t>(t[i])
             | (static_cast<std::uint32_t>(t[i + 1]) << 8)
             | (static_cast<std::uint32_t>(t[i + 2]) << 16)
             | (static_cast<std::uint32_t>(t[i + 3]) << 24);
    };
    // LDUR X0, [SP, #0] = 0xF8400000 | Rn(sp=31)<<5 | Rt(x0=0).
    EXPECT_EQ(word(0), 0xF84003E0u)
        << "word 0 must be `LDUR X0, [SP, #0]` (argc → argGprs[0]) "
           "(D-RUNTIME-MAIN-ARGC-ARGV).";
    // ADD X1, SP, #8 = 0x91000000 | imm12(8)<<10 | Rn(sp=31)<<5 |
    // Rd(x1=1) — the target's frame-relative `lea` single-word form.
    EXPECT_EQ(word(4), 0x910023E1u)
        << "word 1 must be `ADD X1, SP, #8` (the in-place argv "
           "vector's ADDRESS → argGprs[1]). An LDUR here would "
           "dereference the vector.";
    // BL user_entry (imm26 patched by call26 reloc).
    EXPECT_EQ(word(8) & 0xFC000000u, 0x94000000u)
        << "the `BL user_entry` must follow the two arg-setup words "
           "directly (AAPCS64: no prologue between).";
}

// ── D-LK10-ENTRY-ARM64 trampoline byte pin (v0.0.2 V2-1) ───────────
//
// The deterministic, host-independent proof that the ARM64 SYSCALL exit
// mechanism encodes correctly. Asserts the EXACT 20-byte `_start`
// sequence for the aapcs64/syscall trampoline on arm64. RED-on-disable
// across the whole V2-1 substrate: break the MOVZ Imm16 encoding →
// bytes[8..11] wrong; break the bl→call rename → injectEntryTrampoline
// returns false; break the syscall opcode → bytes[12..15] wrong.
//
// STDIO-FLUSH-AT-EXIT (2026-06-09): the SHIPPED elf64-aarch64-linux-exec
// now terminates via libc `exit(3)` (by-name-import → a BL to the `exit`
// PLT stub, NOT MOVZ x8/SVC). This Syscall-arm byte pin therefore drives
// a SYNTHETIC arm64 syscall ELF-exec format (makeSyscallElfExecFormatArm64)
// — preserving the exact syscall-trampoline encoding coverage independent
// of the shipped format's libc-exit policy. The shipped libc-exit shape is
// covered separately by ShippedElfArm64TrampolineCallsLibcExit below.
TEST(LK10EntrySliceC, Arm64TrampolineEmitsExactExitSequence) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = makeSyscallElfExecFormatArm64();
    ASSERT_TRUE(format != nullptr)
        << "synthetic arm64 syscall ELF-exec format must load";
    auto mod = makeReturn42ModuleArm64();
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, *format, rep))
        << "ARM64 syscall-arm trampoline injection must succeed — "
           "requires the `call` (BL), `mov` reg+MOVZ, `syscall` (SVC), "
           "and `unreachable` (BRK) opcodes all present on arm64.";
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(mod.functions.size(), 2u);

    // The 5-instruction `_start` (aapcs64 → no stack prologue since
    // entryStackPointerBias=0 + shadowSpaceBytes=0):
    //   BL  user_entry  → 0x94000000 (imm26=0, call26 reloc patches it)
    //   ORR X0, XZR, X0 → 0xAA0003E0 (mov x0,x0: argGpr0←returnGpr0)
    //   MOVZ X8, #94     → 0xD2800BC8 (exit_group syscall number)
    //   SVC #0           → 0xD4000001 (the syscall)
    //   BRK #0           → 0xD4200000 (unreachable — never returns)
    std::vector<std::uint8_t> const expected = {
        0x00, 0x00, 0x00, 0x94,   // BL imm26
        0xE0, 0x03, 0x00, 0xAA,   // ORR X0, XZR, X0
        0xC8, 0x0B, 0x80, 0xD2,   // MOVZ X8, #94
        0x01, 0x00, 0x00, 0xD4,   // SVC #0
        0x00, 0x00, 0x20, 0xD4,   // BRK #0
    };
    EXPECT_EQ(mod.functions[0].bytes, expected)
        << "ARM64 _start trampoline byte sequence drifted — check the "
           "MOVZ Imm16 encoding (bytes 8..11), the BL/SVC/BRK opcodes, "
           "and that no spurious prologue was emitted.";

    // Syscall arm: NO synthetic ExternImport (distinguishes from the
    // PE ByNameImport arm).
    EXPECT_TRUE(mod.externImports.empty())
        << "syscall arm must not inject an ExternImport";
    // The BL is a call26 relocation to the user entry (SymbolId{1}).
    bool userCallReloc = false;
    for (auto const& r : mod.functions[0].relocations) {
        if (r.target == SymbolId{1}) { userCallReloc = true; break; }
    }
    EXPECT_TRUE(userCallReloc)
        << "trampoline's BL must carry a call26 reloc to user_entry";
    EXPECT_TRUE(mod.imageEntryOverride.has_value());
    EXPECT_EQ(*mod.imageEntryOverride, 0u);
}

// ── STDIO-FLUSH-AT-EXIT (2026-06-09): shipped ELF → libc exit ──────
//
// The shipped elf64-*-linux-exec formats terminate via libc `exit(3)`
// (a by-name-import → DIRECT call/BL to the `exit` PLT stub under
// externCallDispatch=direct-plt), so stdio is flushed + atexit runs
// before the process dies. These pins prove the SHIPPED trampoline:
//   (1) injects a synthetic `exit` ExternImport (libc.so.6) — so
//       libc.so.6 becomes a DT_NEEDED + a PLT stub/GOT slot is emitted;
//   (2) emits a CALL/BL to that import (NOT a raw exit_group syscall);
//   (3) does NOT load a syscall number / emit a SYSCALL/SVC.
//
// RED-on-disable: revert either shipped ELF format's processExit to
// `mechanism:"syscall"` (the old exit_group) and these fail — the
// `exit` import vanishes (1) and the trampoline reverts to MOVZ+SVC /
// mov-eax+SYSCALL (2,3). This is the strict guard the libc-exit policy
// rides on (the runtime witnesses are the native linux CI legs + the
// local WSL/qemu run of shipped_include_puts → "hello\n" + exit 42).
TEST(LK10EntrySliceC, ShippedElfX64TrampolineCallsLibcExit) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42Module();
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, **format, rep))
        << "shipped ELF-x64 libc-exit trampoline injection must succeed";
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(mod.functions.size(), 2u);

    // (1) The synthetic `exit` import (libc.so.6) was appended.
    bool sawExitImport = false;
    for (auto const& e : mod.externImports) {
        if (e.mangledName == "exit") {
            sawExitImport = true;
            EXPECT_EQ(e.libraryPath, "libc.so.6")
                << "libc exit must import from libc.so.6 (→ DT_NEEDED)";
        }
    }
    EXPECT_TRUE(sawExitImport)
        << "by-name-import exit arm must inject an `exit` ExternImport "
           "(none → the format reverted to the raw exit_group syscall).";

    // (2)+(3) The trampoline body: `call user_entry` (E8) + `mov ecx/edi`
    // + `call exit` (E8) — under direct-plt the exit call is a PLAIN
    // DIRECT `call` (E8 disp32), NOT FF 15 (indirect) and NOT a SYSCALL
    // (0F 05). Count E8 opcodes: exactly two direct calls.
    auto const& tramp = mod.functions[0].bytes;
    int e8Count = 0;
    bool sawSyscall = false;
    for (std::size_t i = 0; i + 1 < tramp.size(); ++i) {
        if (tramp[i] == 0xE8u) ++e8Count;
        if (tramp[i] == 0x0Fu && tramp[i + 1] == 0x05u) sawSyscall = true;
    }
    EXPECT_EQ(e8Count, 2)
        << "expected two direct calls: E8 → user_entry + E8 → exit stub";
    EXPECT_FALSE(sawSyscall)
        << "libc-exit trampoline must NOT contain a raw SYSCALL (0F 05) "
           "— the whole point is to route through libc exit(3), which "
           "flushes stdio, instead of the raw exit_group syscall.";

    // Two relocations: user_entry (SymbolId{1}) + the exit import. The
    // exit import got a fresh SymbolId; its reloc target is whichever
    // externImports[].symbol carries mangledName=="exit".
    SymbolId exitSym{0};
    for (auto const& e : mod.externImports)
        if (e.mangledName == "exit") exitSym = e.symbol;
    bool sawUserReloc = false, sawExitReloc = false;
    for (auto const& r : mod.functions[0].relocations) {
        if (r.target == SymbolId{1}) sawUserReloc = true;
        if (exitSym.v != 0 && r.target == exitSym) sawExitReloc = true;
    }
    EXPECT_TRUE(sawUserReloc) << "trampoline must call user_entry";
    EXPECT_TRUE(sawExitReloc)
        << "trampoline's exit call must carry a REL32 reloc to the "
           "synthetic `exit` import (direct-plt → patched to the stub VA)";
}

TEST(LK10EntrySliceC, ShippedElfArm64TrampolineCallsLibcExit) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(format.has_value());
    auto mod = makeReturn42ModuleArm64();
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectEntryTrampoline(
        mod, **target, **format, rep))
        << "shipped ELF-arm64 libc-exit trampoline injection must succeed";
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(mod.functions.size(), 2u);

    // (1) The synthetic `exit` import (libc.so.6) was appended.
    bool sawExitImport = false;
    for (auto const& e : mod.externImports)
        if (e.mangledName == "exit") {
            sawExitImport = true;
            EXPECT_EQ(e.libraryPath, "libc.so.6");
        }
    EXPECT_TRUE(sawExitImport)
        << "by-name-import exit arm must inject an `exit` ExternImport.";

    // (2)+(3) Two BLs (0x94......): BL user_entry + BL exit stub. NO SVC
    // (0xD4000001) — the MOVZ x8/SVC syscall sequence must be gone.
    auto const& tramp = mod.functions[0].bytes;
    ASSERT_EQ(tramp.size() % 4u, 0u);
    int blCount = 0;
    bool sawSvc = false;
    for (std::size_t i = 0; i + 3 < tramp.size(); i += 4) {
        std::uint32_t const w = static_cast<std::uint32_t>(tramp[i])
            | (static_cast<std::uint32_t>(tramp[i + 1]) << 8)
            | (static_cast<std::uint32_t>(tramp[i + 2]) << 16)
            | (static_cast<std::uint32_t>(tramp[i + 3]) << 24);
        if ((w & 0xFC000000u) == 0x94000000u) ++blCount;   // BL imm26
        if (w == 0xD4000001u) sawSvc = true;               // SVC #0
    }
    EXPECT_EQ(blCount, 2)
        << "expected two BLs: BL → user_entry + BL → exit stub "
           "(direct-plt). A single BL means the exit call regressed to "
           "the raw syscall arm.";
    EXPECT_FALSE(sawSvc)
        << "libc-exit trampoline must NOT contain SVC #0 (0xD4000001) — "
           "it routes through libc exit(3) (flushes stdio), not the raw "
           "exit_group syscall.";
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
