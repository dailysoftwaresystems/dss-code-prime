// D-C-ATOMICS-RUNTIME-PE64-BUS-LOCKS-EVERY-ACCESS-TO-A-CACHE-LINE-STRADDLING-OBJECT
// D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE
//
// ★★★ THE PIN FOR "NO BUS LOCK ON THE STRADDLE PATH", AND WHY IT IS NOT A TIMING
// ASSERTION. A split lock — a locked instruction whose memory operand crosses a
// cache line — takes a bus lock that stalls every core. DSS's pe64 atomics
// runtime (`src/dss-config/runtime/platform/src/atomic.c`) used to serve every
// line-crossing `_Atomic` object with one, and a DSS-built witness stretched a
// lock-free neighbour benchmark 8.6× and blew a CI runner's child limit. The
// obvious regression test — "the witness finishes quickly" — is exactly the kind
// of assertion this project refuses: it measures the host, not the program, and
// on a part with a cheap split lock it would be green over the defect.
//
// What IS deterministic is the instruction stream. This test builds a small pe64
// program and a pe64 DLL with the real driver, runs the program under the Win32
// debugging API, and SINGLE-STEPS the thread through each atomic access. Before
// every instruction executes, it decodes the bytes at RIP and, for any
// LOCK-prefixed instruction or XCHG with a memory operand, computes the operand's
// effective address from the thread's live registers. An operand that crosses a
// 64-byte line is a split lock by definition (docs.kernel.org
// `Documentation/arch/x86/buslock.rst`); the test counts them. No clock, no
// sleep, no host property: the same program yields the same instruction stream
// on any x86-64 Windows machine.
//
// THE PHASES — the traced program calls `DebugBreak()` between accesses, and each
// breakpoint opens the next phase:
//   1  this image's copy of the runtime: store, 4 bytes, object crossing a line
//   2  this image's copy: load, 4 bytes, crossing
//   3  this image's copy: store and load, 8 bytes, crossing
//   4  the DLL's OWN copy (a static archive member linked into that image too):
//      store, load and compare-exchange, 4 bytes, crossing
//   5  store, 4 bytes, object inside one line at a misaligned (addr & 7) == 6
//   6  load, 4 bytes, the same object
//   7  store, 8 bytes, misaligned inside one line
//   8  load, 8 bytes, the same object
//   9  compare-exchange, crossing: 4 bytes succeeding and failing, then 8 bytes
//      succeeding and failing (the generic entry, called by name)
//   10 compare-exchange, inside a line: the same four calls on the in-line objects
//   11 THE POSITIVE CONTROL: a native `*(_Atomic unsigned *)p = v`, which DSS
//      emits as an inline `xchg`, aimed at the crossing object — a split lock the
//      tracer MUST see, or every "zero" above is the tracer being blind.
//
// WHAT EACH TEST REDS ON — every one is a red-on-disable pin of one arm:
//   LineCrossingAccessesExecuteNoSplitLockedInstruction — phases 1–4 and 9. Reds
//     if the runtime hands a locked instruction a line-crossing operand again,
//     for a store, a load or a compare-exchange.
//   EveryImageLocksTheOneProcessHeap — phases 1–4 and 9 enter `RtlLockHeap` (what
//     `HeapLock` calls) with ONE heap handle, from BOTH images. Reds if a
//     line-crossing access stops taking the process lock (a plain copy would
//     tear), or if an image gets an arbiter of its own (the exe and the DLL
//     would tear each other).
//   InLineStoreKeepsItsWidthNativeLockedInstruction — phases 5 and 7. Reds if an
//     in-line store loses its lock-free `xchg`, the path the fix must keep.
//   InLineCompareExchangeKeepsItsWidthNativeLockedInstruction — phase 10. Reds if
//     an in-line compare-exchange stops being one `lock cmpxchg` at the object's
//     own width.
//   InLineLoadExecutesNoLockedInstruction — phases 6 and 8. Reds if an in-line
//     load goes back to a locked read-modify-write, which WRITES and faulted on
//     read-only memory (`examples/c/packed_atomic_member_readonly`).
//   TheTracerSeesANativeSplitLock — phase 11, the control.
//
// WINDOWS x86-64 ONLY, and the gate says what it is: a pe64 image only runs on
// Windows, and this runtime body is only linked into pe64 images. Every other leg
// compiles this file and skips its bodies; the ELF and Mach-O legs import their
// platforms' own atomics runtimes and have no DSS body to trace.

#include "core/types/diagnostic_reporter.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace fs = std::filesystem;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))

constexpr bool kCanTrace = true;

// The DLL: a second, independently linked copy of the atomics runtime.
constexpr char const* kLibraryStem = "dssatomicslib";
constexpr char const* kLibrarySource = R"C(
#include <stddef.h>
#include <stdint.h>

extern _Bool __atomic_compare_exchange(size_t size, void *mem, void *expected,
                                       void *desired, int success, int failure);

struct __attribute__((packed)) P4 { char c; _Atomic unsigned a; };

void dss_lib_store4(struct P4 *p, unsigned v) { p->a = v; }
unsigned dss_lib_load4(struct P4 *p) { return p->a; }
int dss_lib_cas4(struct P4 *p, unsigned expected, unsigned desired) {
    unsigned wanted = expected;
    unsigned replacement = desired;
    void *const mem = (void *)((uintptr_t)(void *)p + 1u);
    return __atomic_compare_exchange(4, mem, &wanted, &replacement, 5, 5) ? 1 : 0;
}
)C";

// The traced program. Every packed member is reached through a pointer, so the
// lvalue's provable alignment is 1 and each access lowers to the runtime; the
// compare-exchange is called by name. The objects sit at FIXED offsets inside
// 64-byte lines, which is what lets the tracer recognise them from an effective
// address alone.
constexpr char const* kProgramStem = "dssatomicstracee";
constexpr char const* kProgramSource = R"C(
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

extern _Bool __atomic_compare_exchange(size_t size, void *mem, void *expected,
                                       void *desired, int success, int failure);

struct __attribute__((packed)) P4 { char c; _Atomic unsigned a; };
struct __attribute__((packed)) P8 { char c; _Atomic unsigned long long a; };

extern void     dss_lib_store4(struct P4 *p, unsigned v);
extern unsigned dss_lib_load4(struct P4 *p);
extern int      dss_lib_cas4(struct P4 *p, unsigned expected, unsigned desired);

static unsigned char g_arena[1024];

static uintptr_t lineStart(unsigned line) {
    uintptr_t const first = ((uintptr_t)(void *)g_arena + 63u) & ~(uintptr_t)63u;
    return first + (uintptr_t)(64u * line);
}

static int cas4(void *mem, unsigned expected, unsigned desired, unsigned *observed) {
    unsigned wanted = expected;
    unsigned replacement = desired;
    int const ok = __atomic_compare_exchange(4, mem, &wanted, &replacement, 5, 5) ? 1 : 0;
    *observed = wanted;
    return ok;
}

static int cas8(void *mem, unsigned long long expected, unsigned long long desired,
                unsigned long long *observed) {
    unsigned long long wanted = expected;
    unsigned long long replacement = desired;
    int const ok = __atomic_compare_exchange(8, mem, &wanted, &replacement, 5, 5) ? 1 : 0;
    *observed = wanted;
    return ok;
}

int main(void) {
    struct P4 *const crossing4 = (struct P4 *)(void *)(lineStart(1) + 61u);
    struct P8 *const crossing8 = (struct P8 *)(void *)(lineStart(3) + 57u);
    struct P4 *const inside4   = (struct P4 *)(void *)(lineStart(5) + 53u);
    struct P8 *const inside8   = (struct P8 *)(void *)(lineStart(7) + 49u);
    void *const crossing4Mem = (void *)((uintptr_t)(void *)crossing4 + 1u);
    void *const crossing8Mem = (void *)((uintptr_t)(void *)crossing8 + 1u);
    void *const inside4Mem   = (void *)((uintptr_t)(void *)inside4 + 1u);
    void *const inside8Mem   = (void *)((uintptr_t)(void *)inside8 + 1u);
    unsigned observed4 = 0;
    unsigned long long observed8 = 0;
    int ok = 1;

    DebugBreak();
    crossing4->a = 0x11111111u;
    DebugBreak();
    ok &= crossing4->a == 0x11111111u;
    DebugBreak();
    crossing8->a = 0x2222222222222222ull;
    ok &= crossing8->a == 0x2222222222222222ull;
    DebugBreak();
    dss_lib_store4(crossing4, 0x33333333u);
    ok &= dss_lib_load4(crossing4) == 0x33333333u;
    ok &= dss_lib_cas4(crossing4, 0x33333333u, 0x34343434u) == 1;
    DebugBreak();
    inside4->a = 0x44444444u;
    DebugBreak();
    ok &= inside4->a == 0x44444444u;
    DebugBreak();
    inside8->a = 0x5555555555555555ull;
    DebugBreak();
    ok &= inside8->a == 0x5555555555555555ull;
    DebugBreak();
    ok &= cas4(crossing4Mem, 0x34343434u, 0x35353535u, &observed4) == 1;
    ok &= cas4(crossing4Mem, 0x34343434u, 0x36363636u, &observed4) == 0;
    ok &= observed4 == 0x35353535u;
    ok &= cas8(crossing8Mem, 0x2222222222222222ull, 0x2323232323232323ull, &observed8) == 1;
    ok &= cas8(crossing8Mem, 0x2222222222222222ull, 0x2424242424242424ull, &observed8) == 0;
    ok &= observed8 == 0x2323232323232323ull;
    DebugBreak();
    ok &= cas4(inside4Mem, 0x44444444u, 0x45454545u, &observed4) == 1;
    ok &= cas4(inside4Mem, 0x44444444u, 0x46464646u, &observed4) == 0;
    ok &= observed4 == 0x45454545u;
    ok &= cas8(inside8Mem, 0x5555555555555555ull, 0x5656565656565656ull, &observed8) == 1;
    ok &= cas8(inside8Mem, 0x5555555555555555ull, 0x5757575757575757ull, &observed8) == 0;
    ok &= observed8 == 0x5656565656565656ull;
    DebugBreak();
    *(_Atomic unsigned *)(void *)((uintptr_t)crossing4 + 1u) = 0x66666666u;
    DebugBreak();
    ok &= crossing4->a == 0x66666666u;
    return ok ? 42 : 1;
}
)C";

enum Phase : int {
    kCrossingStore4             = 1,
    kCrossingLoad4              = 2,
    kCrossingStoreLoad8         = 3,
    kOtherImageCrossing4        = 4,
    kInLineStore4               = 5,
    kInLineLoad4                = 6,
    kInLineStore8               = 7,
    kInLineLoad8                = 8,
    kCrossingCompareExchange    = 9,
    kInLineCompareExchange      = 10,
    kNativeSplitLockControl     = 11,
    kFinalBreakpoint            = 12,
};

constexpr int kLineCrossingPhases[] = {kCrossingStore4, kCrossingLoad4,
                                       kCrossingStoreLoad8, kOtherImageCrossing4,
                                       kCrossingCompareExchange};

// Where the traced program put each object inside its 64-byte line.
constexpr std::uint64_t kCrossing4LineOffset = 62;
constexpr std::uint64_t kInside4LineOffset   = 54;
constexpr std::uint64_t kInside8LineOffset   = 50;

// A LOCK-prefixed instruction, or an XCHG, with a memory operand.
struct LockedAccess {
    std::uint64_t rip = 0;
    bool          addressKnown = false;
    std::uint64_t address = 0;
    unsigned      width = 0;
    std::string   why;

    [[nodiscard]] std::uint64_t lineOffset() const noexcept { return address & 63u; }
    [[nodiscard]] bool crossesALine() const noexcept {
        return addressKnown && lineOffset() + width > 64u;
    }
    [[nodiscard]] std::string describe() const {
        char buffer[160];
        std::snprintf(buffer, sizeof buffer,
                      "rip=0x%llx address=0x%llx width=%u lineOffset=%llu%s%s",
                      static_cast<unsigned long long>(rip),
                      static_cast<unsigned long long>(address), width,
                      static_cast<unsigned long long>(lineOffset()),
                      why.empty() ? "" : " why=", why.c_str());
        return buffer;
    }
};

struct PhaseTrace {
    std::size_t                steps = 0;
    std::size_t                stepsInProgramImage = 0;
    std::size_t                stepsInLibraryImage = 0;
    std::vector<LockedAccess>  lockedAccesses;
    std::vector<std::uint64_t> heapLockHandles;   // RCX on entry to RtlLockHeap
};

struct TraceOutcome {
    std::string             failure;   // empty: built, traced, and exited
    DWORD                   exitCode = 0;
    int                     phasesOpened = 0;
    std::vector<PhaseTrace> phases = std::vector<PhaseTrace>(kFinalBreakpoint + 1);
};

[[nodiscard]] std::uint64_t registerValue(CONTEXT const& context, unsigned index) {
    switch (index) {
        case 0:  return context.Rax;
        case 1:  return context.Rcx;
        case 2:  return context.Rdx;
        case 3:  return context.Rbx;
        case 4:  return context.Rsp;
        case 5:  return context.Rbp;
        case 6:  return context.Rsi;
        case 7:  return context.Rdi;
        case 8:  return context.R8;
        case 9:  return context.R9;
        case 10: return context.R10;
        case 11: return context.R11;
        case 12: return context.R12;
        case 13: return context.R13;
        case 14: return context.R14;
        default: return context.R15;
    }
}

// Decodes the instruction about to execute. Returns nullopt unless it is
// LOCK-prefixed or an XCHG, AND has a memory operand. The address is computed
// from the registers the instruction is about to see. Only the lockable opcode
// set needs its operand width and immediate size known; an instruction outside
// it that still carries LOCK is reported with the operand-size width, and one
// whose address cannot be computed is reported with `why` so the test fails
// loud on it instead of skipping it.
[[nodiscard]] std::optional<LockedAccess> decodeLockedAccess(
    std::uint8_t const* code, std::size_t length, CONTEXT const& context) {
    std::size_t at = 0;
    bool lockPrefix = false, operandSize16 = false, addressSize32 = false,
         segmentFsGs = false;
    for (; at < length; ++at) {
        std::uint8_t const b = code[at];
        if (b == 0xF0) lockPrefix = true;
        else if (b == 0x66) operandSize16 = true;
        else if (b == 0x67) addressSize32 = true;
        else if (b == 0x64 || b == 0x65) segmentFsGs = true;
        else if (b == 0xF2 || b == 0xF3 || b == 0x26 || b == 0x2E || b == 0x36
                 || b == 0x3E) {
        } else {
            break;
        }
    }
    std::uint8_t rex = 0;
    if (at < length && (code[at] & 0xF0) == 0x40) rex = code[at++];
    if (at >= length) return std::nullopt;
    bool         twoByte = false;
    std::uint8_t opcode  = code[at++];
    if (opcode == 0x0F) {
        if (at >= length) return std::nullopt;
        twoByte = true;
        opcode  = code[at++];
    }
    bool const xchgOpcode = !twoByte && (opcode == 0x86 || opcode == 0x87);
    if (!lockPrefix && !xchgOpcode) return std::nullopt;

    LockedAccess access;
    access.rip = context.Rip;
    if (at >= length) {
        access.why = "instruction truncated before its ModRM byte";
        return access;
    }
    std::uint8_t const modrm = code[at++];
    unsigned const     mod   = modrm >> 6;
    unsigned const     reg   = (modrm >> 3) & 7u;
    unsigned const     rm    = modrm & 7u;
    if (mod == 3) return std::nullopt;   // a register operand locks no memory

    bool const rexW = (rex & 0x08) != 0;
    bool const byteForm =
        twoByte ? (opcode == 0xB0 || opcode == 0xC0)
                : ((opcode < 0x40 && (opcode & 1u) == 0) || opcode == 0x80
                   || opcode == 0x82 || opcode == 0x86 || opcode == 0xF6
                   || opcode == 0xFE);
    unsigned const operandWidth = rexW ? 8u : (operandSize16 ? 2u : 4u);
    access.width = byteForm ? 1u
                 : (twoByte && opcode == 0xC7) ? (rexW ? 16u : 8u)
                                               : operandWidth;

    std::uint64_t address      = 0;
    std::int64_t  displacement = 0;
    bool          ripRelative  = false;
    auto const readDisplacement = [&](std::size_t bytes) -> bool {
        if (at + bytes > length) return false;
        if (bytes == 1) {
            displacement += static_cast<std::int8_t>(code[at]);
        } else {
            std::int32_t value = 0;
            std::memcpy(&value, code + at, 4);
            displacement += value;
        }
        at += bytes;
        return true;
    };
    if (rm == 4) {
        if (at >= length) {
            access.why = "instruction truncated before its SIB byte";
            return access;
        }
        std::uint8_t const sib   = code[at++];
        unsigned const     scale = 1u << (sib >> 6);
        unsigned const     index = ((sib >> 3) & 7u) | ((rex & 0x02) ? 8u : 0u);
        unsigned const     base  = (sib & 7u) | ((rex & 0x01) ? 8u : 0u);
        if (index != 4u) address += registerValue(context, index) * scale;
        if ((sib & 7u) == 5u && mod == 0) {
            if (!readDisplacement(4)) {
                access.why = "instruction truncated inside its displacement";
                return access;
            }
        } else {
            address += registerValue(context, base);
        }
    } else if (rm == 5 && mod == 0) {
        ripRelative = true;
        if (!readDisplacement(4)) {
            access.why = "instruction truncated inside its displacement";
            return access;
        }
    } else {
        address += registerValue(context, rm | ((rex & 0x01) ? 8u : 0u));
    }
    if ((mod == 1 && !readDisplacement(1)) || (mod == 2 && !readDisplacement(4))) {
        access.why = "instruction truncated inside its displacement";
        return access;
    }
    unsigned immediate = 0;
    if (!twoByte) {
        if (opcode == 0x80 || opcode == 0x82 || opcode == 0x83) immediate = 1;
        else if (opcode == 0x81) immediate = operandSize16 ? 2u : 4u;
        else if (opcode == 0xF6 && reg <= 1) immediate = 1;
        else if (opcode == 0xF7 && reg <= 1) immediate = operandSize16 ? 2u : 4u;
    } else if (opcode == 0xBA) {
        immediate = 1;
    }
    if (ripRelative) address = context.Rip + at + immediate;
    address += static_cast<std::uint64_t>(displacement);
    if (addressSize32) address &= 0xFFFFFFFFull;
    if (segmentFsGs) {
        access.why = "FS/GS segment override: linear address not computed";
        return access;
    }
    access.addressKnown = true;
    access.address      = address;
    return access;
}

[[nodiscard]] std::size_t readUpTo(HANDLE process, std::uint64_t address,
                                   std::uint8_t* out, std::size_t want) {
    for (std::size_t n = want; n > 0; --n) {
        SIZE_T got = 0;
        if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, n,
                              &got)
            && got == n) {
            return n;
        }
    }
    return 0;
}

[[nodiscard]] std::uint64_t imageEnd(HANDLE process, std::uint64_t base) {
    std::uint32_t peOffset    = 0;
    std::uint32_t sizeOfImage = 0;
    SIZE_T        got         = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + 0x3C),
                           &peOffset, 4, &got)
        || got != 4) {
        return base;
    }
    // PE32+: signature (4) + COFF file header (20) + SizeOfImage at optional
    // header offset 56.
    if (!ReadProcessMemory(process,
                           reinterpret_cast<LPCVOID>(base + peOffset + 4 + 20 + 56),
                           &sizeOfImage, 4, &got)
        || got != 4) {
        return base;
    }
    return base + sizeOfImage;
}

[[nodiscard]] bool fileNameEndsWith(HANDLE file, std::wstring_view suffix) {
    if (file == nullptr || file == INVALID_HANDLE_VALUE) return false;
    wchar_t     buffer[1024];
    DWORD const n = GetFinalPathNameByHandleW(file, buffer, 1024, FILE_NAME_NORMALIZED);
    if (n == 0 || n >= 1024) return false;
    std::wstring name(buffer, n);
    if (name.size() < suffix.size()) return false;
    std::wstring tail = name.substr(name.size() - suffix.size());
    for (auto& ch : tail) ch = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch)));
    std::wstring lowered{suffix};
    for (auto& ch : lowered) ch = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch)));
    return tail == lowered;
}

// Runs `program` under the debugging API and traces phases 1..11. The loop
// ALWAYS drains to EXIT_PROCESS_DEBUG_EVENT: a failure terminates the debuggee
// and keeps draining, so nothing is left suspended behind a returned test.
void traceProgram(fs::path const& program, TraceOutcome& out) {
    HMODULE const ntdll = GetModuleHandleW(L"ntdll.dll");
    auto const rtlLockHeap = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
        ntdll != nullptr ? GetProcAddress(ntdll, "RtlLockHeap") : nullptr));
    if (rtlLockHeap == 0) {
        out.failure = "RtlLockHeap is not exported by this host's ntdll.dll";
        return;
    }

    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof startup;
    std::wstring commandLine = L"\"" + program.wstring() + L"\"";
    std::wstring const directory = program.parent_path().wstring();
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                        DEBUG_ONLY_THIS_PROCESS, nullptr, directory.c_str(),
                        &startup, &process)) {
        out.failure = "CreateProcessW(DEBUG_ONLY_THIS_PROCESS) failed, GetLastError="
                    + std::to_string(GetLastError());
        return;
    }

    constexpr std::size_t kStepBudget = 5'000'000;   // a COUNT, not a clock
    std::uint64_t programBase = 0, programEnd = 0, libraryBase = 0, libraryEnd = 0;
    bool          sawLoaderBreakpoint = false;
    bool          terminated          = false;
    std::size_t   totalSteps          = 0;
    auto const fail = [&](std::string message) {
        if (out.failure.empty()) out.failure = std::move(message);
        if (!terminated) {
            TerminateProcess(process.hProcess, 1);
            terminated = true;
        }
    };

    for (;;) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, INFINITE)) {
            fail("WaitForDebugEvent failed, GetLastError=" + std::to_string(GetLastError()));
            break;
        }
        DWORD continueStatus = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (event.u.CreateProcessInfo.hFile != nullptr) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
            }
            programBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                event.u.CreateProcessInfo.lpBaseOfImage));
            programEnd = imageEnd(process.hProcess, programBase);
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            std::wstring const libraryName =
                std::wstring{L"\\"} + std::wstring(kLibraryStem, kLibraryStem + std::strlen(kLibraryStem)) + L".dll";
            if (fileNameEndsWith(event.u.LoadDll.hFile, libraryName)) {
                libraryBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                    event.u.LoadDll.lpBaseOfDll));
                libraryEnd = imageEnd(process.hProcess, libraryBase);
            }
            if (event.u.LoadDll.hFile != nullptr) CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            auto const& record = event.u.Exception.ExceptionRecord;
            bool const  isBreakpoint = record.ExceptionCode == EXCEPTION_BREAKPOINT;
            bool const  isStep       = record.ExceptionCode == EXCEPTION_SINGLE_STEP;
            if (!isBreakpoint && !isStep) {
                continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            } else if (!terminated) {
                HANDLE const thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                                 FALSE, event.dwThreadId);
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (thread == nullptr || !GetThreadContext(thread, &context)) {
                    fail("could not read the debuggee thread's context");
                } else {
                    if (isBreakpoint) {
                        if (!sawLoaderBreakpoint) {
                            sawLoaderBreakpoint = true;
                        } else {
                            ++out.phasesOpened;
                        }
                        // Step past an `int3` the context still points at.
                        std::uint8_t byte = 0;
                        auto const exceptionAddress = static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(record.ExceptionAddress));
                        if (context.Rip == exceptionAddress
                            && readUpTo(process.hProcess, context.Rip, &byte, 1) == 1
                            && byte == 0xCC) {
                            context.Rip += 1;
                        }
                    }
                    int const phase = out.phasesOpened;
                    bool const tracing = sawLoaderBreakpoint && phase >= kCrossingStore4
                                      && phase < kFinalBreakpoint;
                    if (tracing) {
                        auto& trace = out.phases[static_cast<std::size_t>(phase)];
                        ++trace.steps;
                        if (++totalSteps > kStepBudget) {
                            fail("step budget exhausted in phase " + std::to_string(phase));
                        }
                        if (context.Rip >= programBase && context.Rip < programEnd) {
                            ++trace.stepsInProgramImage;
                        }
                        if (context.Rip >= libraryBase && context.Rip < libraryEnd) {
                            ++trace.stepsInLibraryImage;
                        }
                        if (context.Rip == rtlLockHeap) {
                            trace.heapLockHandles.push_back(context.Rcx);
                        }
                        std::uint8_t code[16];
                        std::size_t const n = readUpTo(process.hProcess, context.Rip, code, sizeof code);
                        if (n == 0) {
                            fail("could not read the instruction at the debuggee's RIP");
                        } else if (auto access = decodeLockedAccess(code, n, context)) {
                            trace.lockedAccesses.push_back(std::move(*access));
                        }
                        context.EFlags |= 0x100u;    // TF: trap after the next instruction
                    } else {
                        context.EFlags &= ~0x100u;
                    }
                    if (!terminated && !SetThreadContext(thread, &context)) {
                        fail("could not write the debuggee thread's context");
                    }
                }
                if (thread != nullptr) CloseHandle(thread);
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            out.exitCode = event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            break;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (out.failure.empty() && libraryBase == 0) {
        out.failure = "the traced program never loaded its DLL";
    }
}

[[nodiscard]] fs::path writeSource(fs::path const& dir, std::string const& name,
                                   char const* text) {
    auto const path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << text;
    return path;
}

// Built and traced ONCE for the whole suite: every test reads the same trace.
[[nodiscard]] TraceOutcome const& tracedOnce() {
    static TraceOutcome const outcome = [] {
        TraceOutcome result;
        ScratchDir   scratch{Location::InsideRepo, "atomics-no-split-lock"};
        auto const   dir = scratch.path();

        Program library;
        library.setOutputDir(dir);
        DiagnosticReporter libraryReport;
        auto const librarySource =
            writeSource(dir, std::string{kLibraryStem} + ".c", kLibrarySource);
        if (library.compileFiles({librarySource.string()}, "c",
                                 {"x86_64:pe64-x86_64-windows-dll"}, libraryReport)
            != 0) {
            result.failure = "the DLL did not build; errors="
                           + std::to_string(libraryReport.errorCount());
            return result;
        }
        auto const libraryPath = dir / (std::string{kLibraryStem} + ".dll");

        Program programBuild;
        programBuild.setOutputDir(dir);
        programBuild.setResolveLibraries(std::vector<fs::path>{libraryPath});
        DiagnosticReporter programReport;
        auto const programSource =
            writeSource(dir, std::string{kProgramStem} + ".c", kProgramSource);
        if (programBuild.compileFiles({programSource.string()}, "c",
                                      {"x86_64:pe64-x86_64-windows-exec"}, programReport)
            != 0) {
            result.failure = "the traced program did not build; errors="
                           + std::to_string(programReport.errorCount());
            return result;
        }
        traceProgram(dir / (std::string{kProgramStem} + ".exe"), result);
        return result;
    }();
    return outcome;
}

#else

constexpr bool kCanTrace = false;

#endif

}  // namespace

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))

TEST(AtomicsRuntimeNoSplitLock, TracedProgramRanToItsEndAndReadBackEveryValue) {
    auto const& trace = tracedOnce();
    ASSERT_TRUE(trace.failure.empty()) << trace.failure;
    // The trace itself, printed so a GREEN run carries its evidence too: a
    // verdict of "no split lock" over a phase that stepped nothing would be a
    // vacuous one, and only the counts say which it was.
    for (int phase = kCrossingStore4; phase < kFinalBreakpoint; ++phase) {
        auto const& p = trace.phases[static_cast<std::size_t>(phase)];
        std::size_t crossing = 0;
        for (auto const& access : p.lockedAccesses) crossing += access.crossesALine() ? 1u : 0u;
        std::printf("[trace] phase %d: steps=%zu inProgram=%zu inLibrary=%zu "
                    "lockedAccesses=%zu crossingALine=%zu heapLocks=%zu\n",
                    phase, p.steps, p.stepsInProgramImage, p.stepsInLibraryImage,
                    p.lockedAccesses.size(), crossing, p.heapLockHandles.size());
    }
    EXPECT_EQ(trace.phasesOpened, kFinalBreakpoint)
        << "every DebugBreak must have been reached, in order";
    EXPECT_EQ(trace.exitCode, 42u)
        << "the traced program checks every value it stored and every "
           "compare-exchange result and write-back; 1 means one did not hold";
    for (int phase = kCrossingStore4; phase < kFinalBreakpoint; ++phase) {
        EXPECT_GT(trace.phases[static_cast<std::size_t>(phase)].steps, 0u)
            << "phase " << phase << " was never single-stepped, so nothing below "
                                    "is a measurement of it";
    }
}

TEST(AtomicsRuntimeNoSplitLock, LineCrossingAccessesExecuteNoSplitLockedInstruction) {
    auto const& trace = tracedOnce();
    ASSERT_TRUE(trace.failure.empty()) << trace.failure;
    for (int const phase : kLineCrossingPhases) {
        for (auto const& access : trace.phases[static_cast<std::size_t>(phase)].lockedAccesses) {
            EXPECT_TRUE(access.addressKnown)
                << "phase " << phase << ": a locked instruction whose operand the "
                   "tracer could not place — refused rather than skipped: "
                << access.describe();
            EXPECT_FALSE(access.crossesALine())
                << "phase " << phase << ": a SPLIT LOCK — a locked instruction whose "
                   "operand crosses a 64-byte cache line, which takes a bus lock "
                   "that stalls every core: " << access.describe();
        }
    }
}

TEST(AtomicsRuntimeNoSplitLock, EveryImageLocksTheOneProcessHeap) {
    auto const& trace = tracedOnce();
    ASSERT_TRUE(trace.failure.empty()) << trace.failure;
    std::optional<std::uint64_t> heap;
    for (int const phase : kLineCrossingPhases) {
        auto const& phaseTrace = trace.phases[static_cast<std::size_t>(phase)];
        EXPECT_FALSE(phaseTrace.heapLockHandles.empty())
            << "phase " << phase << ": a line-crossing access took no process lock, "
               "so its plain copy is not atomic against anything";
        for (std::uint64_t const handle : phaseTrace.heapLockHandles) {
            EXPECT_NE(handle, 0u) << "phase " << phase;
            if (!heap) heap = handle;
            EXPECT_EQ(handle, *heap)
                << "phase " << phase << ": two different heaps locked — the images "
                   "in one process would not exclude each other";
        }
    }
    EXPECT_GT(trace.phases[kOtherImageCrossing4].stepsInLibraryImage, 0u)
        << "phase 4 must have run the DLL's own copy of the runtime, or its lock "
           "handle says nothing about a second image";
    EXPECT_GT(trace.phases[kCrossingStore4].stepsInProgramImage, 0u);
}

// An in-line locked instruction at the named offset and width — the lock-free
// arm the fix must keep for an object inside one line.
[[nodiscard]] bool hasInLineLockedAccess(TraceOutcome const& trace, int phase,
                                         std::uint64_t lineOffset, unsigned width) {
    for (auto const& access : trace.phases[static_cast<std::size_t>(phase)].lockedAccesses) {
        if (access.addressKnown && access.lineOffset() == lineOffset
            && access.width == width && !access.crossesALine()) {
            return true;
        }
    }
    return false;
}

TEST(AtomicsRuntimeNoSplitLock, InLineStoreKeepsItsWidthNativeLockedInstruction) {
    auto const& trace = tracedOnce();
    ASSERT_TRUE(trace.failure.empty()) << trace.failure;
    EXPECT_TRUE(hasInLineLockedAccess(trace, kInLineStore4, kInside4LineOffset, 4))
        << "a 4-byte store inside one line must stay a single width-native locked "
           "instruction (an `xchg`): lock-free, and touching only its own bytes";
    EXPECT_TRUE(hasInLineLockedAccess(trace, kInLineStore8, kInside8LineOffset, 8))
        << "an 8-byte store inside one line must stay a single width-native locked "
           "instruction";
    for (int const phase : {kInLineStore4, kInLineLoad4, kInLineStore8, kInLineLoad8,
                            kInLineCompareExchange}) {
        EXPECT_TRUE(trace.phases[static_cast<std::size_t>(phase)].heapLockHandles.empty())
            << "phase " << phase << ": an object inside one line took the process "
               "lock, which serializes it against every line-crossing access in the "
               "process for no reason";
    }
}

TEST(AtomicsRuntimeNoSplitLock, InLineCompareExchangeKeepsItsWidthNativeLockedInstruction) {
    auto const& trace = tracedOnce();
    ASSERT_TRUE(trace.failure.empty()) << trace.failure;
    EXPECT_TRUE(hasInLineLockedAccess(trace, kInLineCompareExchange, kInside4LineOffset, 4))
        << "a 4-byte compare-exchange inside one line must stay one `lock cmpxchg` "
           "at the object's own width";
    EXPECT_TRUE(hasInLineLockedAccess(trace, kInLineCompareExchange, kInside8LineOffset, 8))
        << "an 8-byte compare-exchange inside one line must stay one `lock cmpxchg` "
           "at the object's own width";
}

TEST(AtomicsRuntimeNoSplitLock, InLineLoadExecutesNoLockedInstruction) {
    auto const& trace = tracedOnce();
    ASSERT_TRUE(trace.failure.empty()) << trace.failure;
    for (int const phase : {kInLineLoad4, kInLineLoad8}) {
        for (auto const& access : trace.phases[static_cast<std::size_t>(phase)].lockedAccesses) {
            ADD_FAILURE()
                << "phase " << phase << ": a load executed a locked instruction. A "
                   "load inside one line is ONE plain read; a locked read-modify-write "
                   "WRITES, and faults on read-only memory: " << access.describe();
        }
    }
}

TEST(AtomicsRuntimeNoSplitLock, TheTracerSeesANativeSplitLock) {
    auto const& trace = tracedOnce();
    ASSERT_TRUE(trace.failure.empty()) << trace.failure;
    bool seen = false;
    for (auto const& access : trace.phases[kNativeSplitLockControl].lockedAccesses) {
        if (access.crossesALine() && access.lineOffset() == kCrossing4LineOffset
            && access.width == 4) {
            seen = true;
        }
    }
    EXPECT_TRUE(seen)
        << "THE CONTROL: the native `xchg` aimed at the line-crossing object is a "
           "real split lock, and the tracer did not see it — so every 'no split "
           "lock' verdict above could be the tracer's blindness, not the runtime's "
           "behaviour";
}

#else

TEST(AtomicsRuntimeNoSplitLock, RequiresAWindowsX8664Host) {
    (void)kCanTrace;
    GTEST_SKIP() << "a pe64 image only RUNS on Windows, and DSS's own atomics runtime "
                    "is linked only into pe64 images; the ELF and Mach-O legs import "
                    "their platforms' runtimes and have no DSS body to trace";
}

#endif
