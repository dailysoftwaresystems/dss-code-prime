// D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS — the permanent pin.
//
// THE DEFECT (measured 2026-08-04, TF-C114). `runBinary(captureStdout=true)`
// created an anonymous pipe, spawned the child, waited for the child to EXIT,
// and only then drained the pipe. A child that writes more than the pipe's
// kernel buffer blocks in write() long before it can exit, so:
//
//     child:  blocked in write(), buffer full, nobody reading
//     parent: blocked in WaitForSingleObject / waitpid, waiting for the exit
//             that the child cannot reach
//
// …until the timeout fires and the parent KILLS a child that was working
// perfectly. The reported symptom is `timedOut=1`, i.e. "the child hung" —
// exactly backwards, and the reason this survived: every existing caller
// printed a few bytes, and the one comment that noticed the buffer was bounded
// concluded the risk was TRUNCATION (which cannot happen, because the child is
// blocked) rather than DEADLOCK (which is what actually happens).
//
// MEASURED, before → after:
//   * Windows (this pin's host): the sqlite-harness leg gate's two plan tests
//     each burned their full 120 s budget; the suite ran 240,700 ms and
//     reported 4 failures. After: 1,851 ms, and those two tests pass.
//   * WSL / Ubuntu 24.04, standalone probe, `cat` of a 200,000-byte file:
//     before `spawned=1 timedOut=1 exit=0 bytes=0 elapsed_ms=20000`;
//     after   `spawned=1 timedOut=0 exit=0 bytes=200000 elapsed_ms=10`.
//
// WHY THIS FILE EXISTS RATHER THAN THE MEASUREMENT ALONE. A one-off probe
// proves the fix today; it does not stop the next author from "simplifying" the
// drain thread away, and the resulting red would be a TIMEOUT in some unrelated
// suite three cycles later. This asserts the property directly, on the shared
// spawn chokepoint both corpus harnesses use (`tests/examples/examples_runner`
// in-process and `integrated_tests/runner` CLI-subprocess both call
// `runBinary`, so both inherit the fix and both would inherit its loss).
//
// THE PIN IS DELIBERATELY TWO-AXIS. `timedOut == false` AND the exact byte
// count. Reverting the ordering trips the first; a "fix" that drains
// concurrently but drops the tail trips the second. Either alone could be
// satisfied by a wrong implementation.
//
// SELF-SPAWN, so the pin has no external dependency. The test executable is its
// own payload generator: `main` below intercepts `--dss-emit-bytes=<N>` before
// gtest sees it and writes N bytes to stdout. That keeps the pin portable to
// every host in the matrix (Windows has no `cat`; a Mac has no `/proc`) and
// makes the payload size a property of this file rather than of the machine.

#include "byte_emit.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Above BOTH kernel buffers by a wide margin: Windows' anonymous-pipe default
// is ~4 KiB and Linux's is 64 KiB, so a payload that only cleared the smaller
// one would leave the POSIX arm of this pin vacuous — and the POSIX arm is the
// one that took a 20-second timeout to discover.
constexpr std::size_t kPayloadBytes = 200000;

constexpr std::string_view kEmitFlag = "--dss-emit-bytes=";

// The last byte is a distinct sentinel, so "the count is right" and "the TAIL
// arrived" are not the same assertion. A drain that races the child and stops
// early would otherwise be able to produce a plausible-looking short read that
// only the count catches.
constexpr char kFill     = 'A';
constexpr char kSentinel = 'Z';

std::filesystem::path selfExecutable() {
    return std::filesystem::path{::testing::internal::GetArgvs().empty()
                                     ? std::string{}
                                     : ::testing::internal::GetArgvs()[0]};
}

}  // namespace

// ── The pin ────────────────────────────────────────────────────────────────

TEST(RunBinaryCapture, ALargeChildIsDrainedConcurrentlyAndNotDeadlocked) {
    auto const self = selfExecutable();
    ASSERT_FALSE(self.empty()) << "could not recover argv[0]";
    ASSERT_TRUE(std::filesystem::exists(self))
        << "argv[0] does not resolve to a file: " << self;

    // argv becomes [self, --dss-emit-bytes=N, self]; the child's `main` sees the
    // flag and never reaches gtest. The trailing `self` is `runBinary`'s
    // `binaryPath` (its POSIX arm chmods that path, so it must be a real file —
    // the test executable already is one, and 0755 is what it already carries).
    std::vector<std::string> const prefix{
        self.string(), std::string{kEmitFlag} + std::to_string(kPayloadBytes)};

    // A bounded budget: if the ordering regresses, this pin must RED in a
    // minute rather than inherit whatever the default budget is.
    auto const t0 = std::chrono::steady_clock::now();
    auto const r  = dss::test_support::runBinary(
        self, std::chrono::seconds{60}, /*captureStdout=*/true, prefix);
    auto const elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();

    ASSERT_TRUE(r.spawned) << r.diagnostic;

    // AXIS 1 — the deadlock itself.
    EXPECT_FALSE(r.timedOut)
        << "runBinary TIMED OUT capturing " << kPayloadBytes
        << " bytes after " << elapsedMs << " ms. That is the drain-after-exit"
           " deadlock (D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS): the child"
           " is blocked writing into a pipe nobody is reading, and the harness"
           " then kills it and reports the stall as the CHILD's fault."
           "  diagnostic: "
        << r.diagnostic;

    // AXIS 2 — the whole payload, tail included.
    EXPECT_EQ(r.capturedStdout.size(), kPayloadBytes)
        << "captured " << r.capturedStdout.size() << " of " << kPayloadBytes
        << " bytes — a concurrent drain that stops early is the other half of"
           " the same defect";
    if (r.capturedStdout.size() == kPayloadBytes) {
        EXPECT_EQ(r.capturedStdout.back(), kSentinel)
            << "the LAST byte did not arrive";
        EXPECT_EQ(r.capturedStdout.find_first_not_of(kFill),
                  kPayloadBytes - 1u)
            << "the captured bytes are not the payload we sent — the capture is"
               " interleaved or corrupted, not merely short";
    }
    EXPECT_EQ(r.exitCode, 0u) << r.diagnostic;
}

// A small child must still work — the concurrent drain must not have broken the
// case every other caller in the tree relies on. Red-on-disable for a drain
// thread that is never joined, or joined before EOF: this loses the bytes.
TEST(RunBinaryCapture, ASmallChildStillRoundTripsItsBytes) {
    auto const self = selfExecutable();
    ASSERT_TRUE(std::filesystem::exists(self)) << self;
    std::vector<std::string> const prefix{self.string(),
                                          std::string{kEmitFlag} + "7"};
    auto const r = dss::test_support::runBinary(
        self, std::chrono::seconds{60}, /*captureStdout=*/true, prefix);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.capturedStdout, std::string(6, kFill) + kSentinel);
    EXPECT_EQ(r.exitCode, 0u);
}

// ── D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY — the sysroot-derivation pins ───────
//
// THE DEFECT, and it has now fired twice with an identical signature.
// `qemu-aarch64` being on PATH does not mean it can RUN anything: a
// dynamically linked guest ELF names its loader in PT_INTERP, qemu-user
// resolves that path against the HOST filesystem, and without
// QEMU_LD_PREFIX pointing at a guest sysroot every emulated arm dies at
// exit 255 before the guest's first instruction. ✔MEASURED 2026-08-12 on the
// WSL leg: raw `ctest` with the variable unset → 468 of 826 failing, all one
// cause; with `QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` exported and nothing
// else changed → 826 of 826 passing. The failures read as `baseline
// exit-code mismatch (expected=42; OS=255)`, i.e. as a compiler regression.
//
// WHY THESE PINS AND NOT THE END-TO-END RUN. The end-to-end proof needs an
// x86_64 Linux host with qemu-user AND a cross sysroot installed, which is
// one leg of the matrix; the Windows gate would never execute a line of it.
// Every branch below is therefore driven through injected roots against a
// synthesized guest ELF, so the derivation is gated on EVERY host — including
// the one where the mechanism itself is dormant.
//
// STRICTNESS. The derivation pins assert exact strings, not "non-empty":
// picking the WRONG sysroot is the failure mode that would otherwise pass,
// and it is indistinguishable from picking the right one under a
// nonempty-check. The diagnostic pin asserts on tokens rather than the whole
// sentence only because the sentence embeds a per-run scratch path; it
// requires the VARIABLE NAME and the interpreter path, which are the two
// facts whose absence made 468 reds unattributable.

namespace {

using dss::test_support::ScratchDir;
using dss::test_support::detail::decideQemuGuestSysroot;
using dss::test_support::detail::elfInterpreterPath;
using dss::test_support::detail::qemuUserGuestArch;

// An interpreter path that must resolve on NO host in the matrix — used by
// every fixture whose point is that the derivation finds nothing. A realistic
// spelling (`/lib/ld-linux-aarch64.so.1`) would make these pins pass or fail
// depending on whether the machine happens to have a cross sysroot installed,
// which is precisely the ambient dependence this row exists to remove.
constexpr char const* kUnresolvableInterp = "/lib/ld-dss-no-such-loader.so.9";

// A guest loader spelling that is NOT derivable from the arch name — the
// derivation must carry it through from the artifact's own bytes.
constexpr char const* kGuestInterp = "/lib/ld-linux-aarch64.so.1";

// A qemu-user launcher path. It deliberately does NOT exist on disk: the pins
// that reach `runBinary` must be refused BEFORE the spawn, and if that
// refusal ever regressed, a launcher that resolved to a real executable would
// re-exec this very test binary with the fixture as its argument.
constexpr char const* kFakeQemuLauncher = "/opt/definitely-absent/qemu-aarch64";

// Minimal ELF64-LSB executable. PT_LOAD is emitted FIRST and PT_INTERP
// second, so a reader that returns phdr[0] instead of scanning fails the pin
// rather than passing on a one-entry fixture.
void writeElf64(std::filesystem::path const& at, std::string const& interpreter) {
    using namespace dss::test_support;
    std::uint16_t const phnum = interpreter.empty() ? 1u : 2u;
    std::size_t const interpOffset = 64u + 56u * static_cast<std::size_t>(phnum);

    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {0x7Fu, 'E', 'L', 'F', 2u, 1u, 1u, 0u});
    bytes.resize(16u, 0u);          // EI_PAD
    appU16(bytes, 2u);              // e_type    = ET_EXEC
    appU16(bytes, 183u);            // e_machine = EM_AARCH64
    appU32(bytes, 1u);              // e_version
    appU64(bytes, 0x400000u);       // e_entry
    appU64(bytes, 64u);             // e_phoff
    appU64(bytes, 0u);              // e_shoff
    appU32(bytes, 0u);              // e_flags
    appU16(bytes, 64u);             // e_ehsize
    appU16(bytes, 56u);             // e_phentsize
    appU16(bytes, phnum);           // e_phnum
    appU16(bytes, 64u);             // e_shentsize
    appU16(bytes, 0u);              // e_shnum
    appU16(bytes, 0u);              // e_shstrndx

    auto phdr = [&bytes](std::uint32_t type, std::uint64_t offset,
                         std::uint64_t size) {
        appU32(bytes, type);
        appU32(bytes, 4u);          // p_flags = PF_R
        appU64(bytes, offset);      // p_offset
        appU64(bytes, 0x400000u);   // p_vaddr
        appU64(bytes, 0x400000u);   // p_paddr
        appU64(bytes, size);        // p_filesz
        appU64(bytes, size);        // p_memsz
        appU64(bytes, 1u);          // p_align
    };
    phdr(/*PT_LOAD*/ 1u, 0u, 64u);
    if (!interpreter.empty()) {
        phdr(/*PT_INTERP*/ 3u, interpOffset, interpreter.size() + 1u);
        bytes.insert(bytes.end(), interpreter.begin(), interpreter.end());
        bytes.push_back(0u);
    }

    std::ofstream out{at, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<char const*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void writeText(std::filesystem::path const& at, std::string_view text) {
    std::ofstream out{at, std::ios::binary | std::ios::trunc};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// A sysroot at `<root>/<name>` that provides `interpreter`, or — when
// `provideLoader` is false — a same-named directory that does not, which is
// how the pins tell a NAME match from a PROBE.
void makeSysroot(std::filesystem::path const& root, std::string const& name,
                 std::string const& interpreter, bool provideLoader) {
    auto const sysroot = root / name;
    std::filesystem::create_directories(sysroot);
    if (!provideLoader) return;
    auto const loader = sysroot / std::string{interpreter}.substr(1);
    std::filesystem::create_directories(loader.parent_path());
    writeText(loader, "not really a loader");
}

// QEMU_LD_PREFIX removed for the duration, then restored exactly.
// `run_binary.hpp` snapshots the operator's value on the first EMULATED
// spawn; every pin that can reach that snapshot holds this guard, so the
// snapshot is empty no matter what the gate exported into this process.
class QemuLdPrefixCleared {
public:
    QemuLdPrefixCleared() {
        if (char const* const raw = std::getenv("QEMU_LD_PREFIX")) {
            had_   = true;
            saved_ = raw;
        }
        assign(nullptr);
    }
    ~QemuLdPrefixCleared() { assign(had_ ? saved_.c_str() : nullptr); }
    QemuLdPrefixCleared(QemuLdPrefixCleared const&)            = delete;
    QemuLdPrefixCleared& operator=(QemuLdPrefixCleared const&) = delete;

    static void assign(char const* value) {
#if defined(_WIN32)
        // An empty value DELETES the variable on Windows; there is no
        // `unsetenv`, and leaving it set-but-empty would read as "the operator
        // set it" to the very code under test.
        ::_putenv_s("QEMU_LD_PREFIX", value != nullptr ? value : "");
#else
        if (value != nullptr) ::setenv("QEMU_LD_PREFIX", value, 1);
        else                  ::unsetenv("QEMU_LD_PREFIX");
#endif
    }

private:
    bool        had_ = false;
    std::string saved_;
};

}  // namespace

TEST(QemuGuestSysroot, GuestArchComesFromTheLauncherName) {
    EXPECT_EQ(qemuUserGuestArch("/usr/bin/qemu-aarch64"), "aarch64");
    EXPECT_EQ(qemuUserGuestArch("qemu-riscv64"), "riscv64");
    EXPECT_EQ(qemuUserGuestArch("/usr/bin/qemu-x86_64"), "x86_64");
    // `qemu-user-static`'s binfmt spelling — the suffix is packaging, not arch.
    EXPECT_EQ(qemuUserGuestArch("/usr/bin/qemu-aarch64-static"), "aarch64");
    // A `.exe` must not become part of the architecture.
    EXPECT_EQ(qemuUserGuestArch("C:/msys64/usr/bin/qemu-arm.exe"), "arm");
}

TEST(QemuGuestSysroot, NonQemuUserLaunchersYieldNoArch) {
    // The FULL-SYSTEM emulator boots a kernel and never reads QEMU_LD_PREFIX;
    // handing it this remedy would attach a fix to an unrelated failure.
    EXPECT_EQ(qemuUserGuestArch("/usr/bin/qemu-system-aarch64"), "");
    EXPECT_EQ(qemuUserGuestArch("qemu-system-x86_64"), "");
    EXPECT_EQ(qemuUserGuestArch("/usr/bin/qemu"), "");
    EXPECT_EQ(qemuUserGuestArch("/usr/bin/wine"), "");
    EXPECT_EQ(qemuUserGuestArch("/usr/bin/box64"), "");
    EXPECT_EQ(qemuUserGuestArch(""), "");
}

TEST(QemuGuestSysroot, InterpreterIsReadOutOfTheGuestBytes) {
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const dynamicElf = scratch.path() / "dynamic.elf";
    auto const staticElf  = scratch.path() / "static.elf";
    auto const notAnElf   = scratch.path() / "notelf.bin";
    writeElf64(dynamicElf, kGuestInterp);
    writeElf64(staticElf, "");
    // Longer than the 64-byte header read, and NUL-free, so the reader reaches
    // the magic check with a full header rather than bailing on a short file —
    // and it opens `MZ`, because the artifact that must not be refused here is
    // a real PE spawned under a launcher prefix, not a hypothetical.
    writeText(notAnElf, "MZ this is a PE-shaped image, not an ELF, and it is "
                        "deliberately longer than the sixty-four bytes the "
                        "header read consumes.");

    EXPECT_EQ(elfInterpreterPath(dynamicElf), kGuestInterp);
    // No PT_INTERP: a static guest needs no sysroot and must not be refused.
    EXPECT_EQ(elfInterpreterPath(staticElf), "");
    EXPECT_EQ(elfInterpreterPath(notAnElf), "");
    EXPECT_EQ(elfInterpreterPath(scratch.path() / "absent.elf"), "");
}

TEST(QemuGuestSysroot, DerivesTheSysrootThatActuallyCarriesTheLoader) {
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const hostRoot   = scratch.path() / "root";
    auto const searchRoot = scratch.path() / "usr";
    std::filesystem::create_directories(hostRoot);
    std::filesystem::create_directories(searchRoot);
    // TWO name matches for this arch, and the one WITHOUT a loader sorts
    // FIRST — `aarch64-elf` is the bare-metal cross toolchain, a real triple
    // that ships headers and libs but no dynamic loader. Ordering is
    // load-bearing in this fixture: with the decoy sorting second, a
    // derivation that accepted the first NAME match would still land on the
    // right answer and this pin would pass on a broken implementation
    // (✔MEASURED — it did, until the decoy was renamed).
    makeSysroot(searchRoot, "aarch64-elf", kGuestInterp,
                /*provideLoader=*/false);
    makeSysroot(searchRoot, "aarch64-linux-gnu", kGuestInterp,
                /*provideLoader=*/true);
    auto const guest = scratch.path() / "guest.elf";
    writeElf64(guest, kGuestInterp);

    auto const decision =
        decideQemuGuestSysroot({"/usr/bin/qemu-aarch64"}, guest,
                               /*operatorSuppliedPrefix=*/"", hostRoot,
                               searchRoot);
    EXPECT_EQ(decision.interpreter, kGuestInterp);
    EXPECT_EQ(decision.sysroot,
              (searchRoot / "aarch64-linux-gnu").generic_string());
    EXPECT_EQ(decision.diagnostic, "");
}

// The arch filter, isolated from sort order: the ONLY tree on this host that
// carries the loader belongs to a different guest. Running an aarch64 binary
// against a riscv64 sysroot is not a near-miss, it is a different machine's
// libc — the right answer is the refusal, not the nearest available directory.
TEST(QemuGuestSysroot, AWrongArchSysrootIsNeverChosen) {
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const hostRoot   = scratch.path() / "root";
    auto const searchRoot = scratch.path() / "usr";
    std::filesystem::create_directories(hostRoot);
    std::filesystem::create_directories(searchRoot);
    makeSysroot(searchRoot, "riscv64-linux-gnu", kGuestInterp,
                /*provideLoader=*/true);
    auto const guest = scratch.path() / "guest.elf";
    writeElf64(guest, kGuestInterp);

    auto const decision =
        decideQemuGuestSysroot({"/usr/bin/qemu-aarch64"}, guest,
                               /*operatorSuppliedPrefix=*/"", hostRoot,
                               searchRoot);
    EXPECT_EQ(decision.sysroot, "");
    EXPECT_NE(decision.diagnostic.find("QEMU_LD_PREFIX"), std::string::npos)
        << decision.diagnostic;
}

TEST(QemuGuestSysroot, NoSysrootAnywhereIsOneLegibleRefusalNamingTheVariable) {
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const hostRoot   = scratch.path() / "root";
    auto const searchRoot = scratch.path() / "usr";
    std::filesystem::create_directories(hostRoot);
    std::filesystem::create_directories(searchRoot);
    auto const guest = scratch.path() / "guest.elf";
    writeElf64(guest, kUnresolvableInterp);

    auto const decision =
        decideQemuGuestSysroot({"/usr/bin/qemu-aarch64"}, guest,
                               /*operatorSuppliedPrefix=*/"", hostRoot,
                               searchRoot);
    EXPECT_EQ(decision.interpreter, kUnresolvableInterp);
    EXPECT_EQ(decision.sysroot, "");
    // The two facts whose absence made 468 reds unattributable.
    EXPECT_NE(decision.diagnostic.find("QEMU_LD_PREFIX"), std::string::npos)
        << decision.diagnostic;
    EXPECT_NE(decision.diagnostic.find(kUnresolvableInterp), std::string::npos)
        << decision.diagnostic;
    EXPECT_NE(decision.diagnostic.find("qemu-aarch64"), std::string::npos)
        << decision.diagnostic;
}

TEST(QemuGuestSysroot, EveryNothingToDoBranchLeavesTheSpawnAlone) {
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const hostRoot   = scratch.path() / "root";
    auto const searchRoot = scratch.path() / "usr";
    std::filesystem::create_directories(searchRoot);
    auto const guest = scratch.path() / "guest.elf";
    writeElf64(guest, kUnresolvableInterp);
    auto const staticGuest = scratch.path() / "static.elf";
    writeElf64(staticGuest, "");

    auto expectSilent = [&](dss::test_support::detail::QemuSysrootDecision const& d,
                            char const* why) {
        EXPECT_EQ(d.sysroot, "") << why;
        EXPECT_EQ(d.diagnostic, "") << why;
    };

    // A native run: no launcher, so no loader to place.
    expectSilent(decideQemuGuestSysroot({}, guest, "", hostRoot, searchRoot),
                 "no launcher prefix");
    // Somebody else's emulator, and the full-system one, are not this
    // variable's business even though the guest cannot resolve its loader.
    expectSilent(decideQemuGuestSysroot({"/usr/bin/wine"}, guest, "", hostRoot,
                                        searchRoot),
                 "non-qemu launcher");
    expectSilent(decideQemuGuestSysroot({"/usr/bin/qemu-system-aarch64"}, guest,
                                        "", hostRoot, searchRoot),
                 "qemu-system is not qemu-user");
    // A static guest asks for no interpreter; refusing it would break a shape
    // that runs correctly today with no sysroot at all.
    auto const staticDecision = decideQemuGuestSysroot(
        {"/usr/bin/qemu-aarch64"}, staticGuest, "", hostRoot, searchRoot);
    expectSilent(staticDecision, "static guest has no PT_INTERP");
    EXPECT_EQ(staticDecision.interpreter, "");
}

TEST(QemuGuestSysroot, AnOperatorSuppliedPrefixIsLeftCompletelyAlone) {
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const hostRoot   = scratch.path() / "root";
    auto const searchRoot = scratch.path() / "usr";
    std::filesystem::create_directories(searchRoot);
    // A derivable sysroot IS present, so this pin fails if the operator's
    // value is merely tie-broken against rather than honoured outright.
    makeSysroot(searchRoot, "aarch64-linux-gnu", kGuestInterp,
                /*provideLoader=*/true);
    auto const guest = scratch.path() / "guest.elf";
    writeElf64(guest, kGuestInterp);

    auto const decision = decideQemuGuestSysroot(
        {"/usr/bin/qemu-aarch64"}, guest,
        /*operatorSuppliedPrefix=*/"/opt/my-own-sysroot", hostRoot, searchRoot);
    EXPECT_EQ(decision.sysroot, "");
    EXPECT_EQ(decision.diagnostic, "");
    EXPECT_EQ(decision.interpreter, "");
}

TEST(QemuGuestSysroot, AHostRootThatAlreadyCarriesTheLoaderNeedsNoPrefix) {
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const hostRoot   = scratch.path() / "root";
    auto const searchRoot = scratch.path() / "usr";
    std::filesystem::create_directories(searchRoot);
    // The loader sits at the host root (the native-arch and multiarch-libc
    // shapes), and a derivable sysroot also exists — narrowing the child's
    // search to the sysroot would be a change, not a fix.
    makeSysroot(scratch.path(), "root", kGuestInterp, /*provideLoader=*/true);
    makeSysroot(searchRoot, "aarch64-linux-gnu", kGuestInterp,
                /*provideLoader=*/true);
    auto const guest = scratch.path() / "guest.elf";
    writeElf64(guest, kGuestInterp);

    auto const decision =
        decideQemuGuestSysroot({"/usr/bin/qemu-aarch64"}, guest, "", hostRoot,
                               searchRoot);
    EXPECT_EQ(decision.interpreter, kGuestInterp);
    EXPECT_EQ(decision.sysroot, "");
    EXPECT_EQ(decision.diagnostic, "");
}

TEST(QemuGuestSysroot, TheDerivedSysrootIsActuallyExportedToTheChild) {
    QemuLdPrefixCleared cleared;
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const hostRoot   = scratch.path() / "root";
    auto const searchRoot = scratch.path() / "usr";
    std::filesystem::create_directories(hostRoot);
    std::filesystem::create_directories(searchRoot);
    makeSysroot(searchRoot, "aarch64-linux-gnu", kGuestInterp,
                /*provideLoader=*/true);
    auto const guest = scratch.path() / "guest.elf";
    writeElf64(guest, kGuestInterp);

    ASSERT_EQ(std::getenv("QEMU_LD_PREFIX"), nullptr);
    EXPECT_EQ(dss::test_support::ensureQemuGuestSysroot(
                  {"/usr/bin/qemu-aarch64"}, guest, hostRoot, searchRoot),
              "");
    char const* const exported = std::getenv("QEMU_LD_PREFIX");
    ASSERT_NE(exported, nullptr)
        << "the derivation decided on a sysroot but never exported it — the "
           "child would inherit nothing and die in the loader exactly as "
           "before";
    EXPECT_EQ(std::string{exported},
              (searchRoot / "aarch64-linux-gnu").generic_string());

    // A native spawn must not touch the variable at all.
    QemuLdPrefixCleared::assign(nullptr);
    EXPECT_EQ(dss::test_support::ensureQemuGuestSysroot({}, guest, hostRoot,
                                                        searchRoot),
              "");
    EXPECT_EQ(std::getenv("QEMU_LD_PREFIX"), nullptr);
}

// The call site, not just the decision. `runBinary` is where the refusal has
// to happen — one legible sentence instead of a child that dies at exec time
// and is reported as an exit-code mismatch. The launcher path does not exist,
// so a regressed call site reaches the spawn and reports a spawn error
// INSTEAD of naming the variable: this pin reds on the message, which is the
// property the row is about.
TEST(QemuGuestSysroot, RunBinaryRefusesTheSpawnAndSaysWhichVariableIsUnset) {
    QemuLdPrefixCleared cleared;
    ScratchDir scratch{dss::test_support::Location::Temp, "qemu-ld-prefix"};
    auto const guest = scratch.path() / "guest.elf";
    writeElf64(guest, kUnresolvableInterp);

    auto const result = dss::test_support::runBinary(
        guest, std::chrono::seconds{10}, /*captureStdout=*/false,
        {kFakeQemuLauncher});

    EXPECT_FALSE(result.spawned) << "nothing may be exec'd once the guest's "
                                    "loader is known to be unreachable";
    EXPECT_FALSE(result.timedOut);
    EXPECT_NE(result.diagnostic.find("QEMU_LD_PREFIX"), std::string::npos)
        << result.diagnostic;
    EXPECT_NE(result.diagnostic.find(kUnresolvableInterp), std::string::npos)
        << result.diagnostic;
}

// ── main: payload generator OR test runner ─────────────────────────────────
//
// Defining `main` here means the linker never pulls gtest_main's object out of
// the static library, so there is no duplicate symbol. The flag is consumed
// BEFORE InitGoogleTest, which would otherwise reject it as unrecognised.
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string_view const arg{argv[i]};
        if (arg.substr(0, kEmitFlag.size()) != kEmitFlag) continue;
        auto const n = static_cast<std::size_t>(
            std::strtoull(std::string{arg.substr(kEmitFlag.size())}.c_str(),
                          nullptr, 10));
        if (n == 0) return 2;
        // Binary mode matters on Windows: the default text mode would translate
        // any '\n' to CRLF and the byte count would not be the count we asked
        // for. The payload deliberately contains no newline at all, so the two
        // modes agree — but say so, because a future payload with a newline
        // would make this test fail for a reason that has nothing to do with
        // the defect it pins.
        std::string const payload = std::string(n - 1, kFill) + kSentinel;
        std::fwrite(payload.data(), 1, payload.size(), stdout);
        std::fflush(stdout);
        return 0;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
