// P69 (lane `cs`, fold f26c — D-TARGET-SYSV-AMD64-MEMORY-CLASS-ARGUMENTS-GO-BY-POINTER-AND-X87-RESULTS-BY-SRET) —
// A BY-VALUE AGGREGATE CROSSES THE DSS/REFERENCE BOUNDARY AS ITS CONVENTION SAYS, measured where it runs. Under
// SysV AMD64 (psABI 3.2.3) a MEMORY-class aggregate ARGUMENT travels as its bytes on the stack and an X87-class
// aggregate RESULT comes back in st0; under AAPCS64 a large aggregate travels as a pointer to a caller copy. DSS
// talking to DSS is self-consistent whatever it does, so no DSS-only test sees a convention it shares with
// nobody: before fold f26c a DSS caller handed a gcc or clang callee WRONG VALUES, and a gcc or clang caller
// crashed a DSS callee (SIGSEGV), on every call of these shapes (the row's two-unit witnesses, linux-x86_64 and
// Mach-O x86_64).
//
// THE WITNESS: two units — the CALLEE (the shapes) and the CALLER (every call, each checked) — each compiled by
// DSS (a relocatable object of the arm's target, through the production driver) and by the host's own `cc`, and
// all FOUR caller/callee pairings linked by the host's linker with a `cc`-built `main` and RUN: 42 when every
// check held, else 100 + the first failed check. The shapes are the row's: a 24-byte struct by value with a
// `long long` after it; the struct after six (no register left); two structs; a callee that writes its copy; a
// struct of one `long double` as an argument and as a RESULT (st0); a 16-aligned x87 struct after seven. And the
// stacked-alignment universe (main's ledger 452): a struct of alignment 8, 16, 32, 64 and 256 after one and after
// two stacked eightbytes — gcc and clang begin each at its own alignment, uncapped — and an over-aligned SCALAR
// typedef at the same two positions, which they pass in the next eightbyte; and a union of `long double` and
// `long long` as a result and as an argument (MEMORY under psABI, %rax + %xmm0 on Apple's x86_64: its X87UP
// eightbyte is SSE there, main's ledger 454). CONTROLS the row measured right
// before the fold: a MEMORY-class result through the hidden pointer, and a MEMORY-class struct through `...`.
//
// ARMS: Linux x86_64 with gcc and with clang, GNU ld (SysV, ELF); Apple's ld `-arch x86_64` (SysV, Mach-O, under
// Rosetta through the repo's one cross-arch gate); the AAPCS64 controls, Linux aarch64 and Apple `-arch arm64`,
// and the Win64 controls, cl and link.exe and mingw-w64 gcc and GNU ld -- the controls without the x87 shapes
// (they are SysV's), each a pointer to a caller copy. An arm whose reference toolchain is absent is not run and
// says so. DSS's two objects are built at debug AND at release, and each build is paired.
//
// THE CALLER'S FRAME (main's ledger 495): a DSS caller that passes a stacked argument aligned past the stack
// realigns its stack pointer on the frame a VLA function uses, so every check is made by three callers -- a plain
// one, one with a VLA of its own (GNU arms; cl has none), one that is itself variadic -- each reported on its own
// line; and on the SysV arms a callee below the caller walks the stack (`_Unwind_Backtrace`) and must reach the
// caller and `main` through the realigned frame, which only its call-frame information describes (check 28). The
// walk is ASSERTED where every object of a pairing carries an unwind table -- the ELF arms -- and on the Apple
// x86_64 arm PRINTED in each pairing holding a DSS object, naming the row it waits on: DSS's Mach-O relocatable
// object carries no unwind table yet (D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS, its Mach-O half; main's
// ledger 510). The control's walk is asserted there too, so the instrument itself is read on every SysV arm; and
// DSS's own LINKED image describes the realigned frame on both formats (MEASURED, llvm-dwarfdump 19 --eh-frame;
// pinned by tests/lir/test_lir_stacked_argument_realignment.cpp).
//
// THE PARAMETER IS AN OBJECT OF ITS TYPE (C 6.7.5): every callee of the stacked-alignment universe, and of the
// over-aligned scalars, adds 1000 to its answer when its `&s` (`&v`) is not aligned as the type says, and the
// caller tells a MISPLACED object whose values arrived (exit 200 + its check) from a wrong value (100 + its
// check). The address is read back through a VOLATILE object first: gcc answers `(uintptr_t)&s & (N - 1)` from
// the parameter's declared alignment without looking at the address -- at -O0 it emits no test at all (probe w4,
// read) -- so the plain test proved nothing wherever a reference compiled the callee. Read that way (probe w4,
// -O0 and -O1), gcc 13.3.0 and clang 18.1.3 on Linux x86_64 and Apple clang 21 on x86_64 and arm64 place every
// shape on its alignment, and cl 19.51 its 32- and 64-aligned caller copies; gcc 13.3.0 on aarch64 and mingw-w64
// gcc 13.2.0 do NOT -- their caller copy sits where the stack pointer is (`mov x7, sp` style), only 16-aligned.
// So placement is asserted where DSS PLACES the object: DSS's callee under SysV (it copies the stacked bytes to
// its own slot) and its scalar homes, DSS's caller (its copy, or the stacked bytes a reference callee reads in
// place), and both DSS sides; the copy a REFERENCE caller makes under a pointer convention (`callerCopy`), the
// scalar home a reference callee makes (gcc 13.3.0 aarch64 misplaces its own, run f26c-s22) and everything in the
// control are the reference's, printed and never asserted -- the values are asserted in every pairing. (DSS's callee takes the caller's copy as its parameter, as gcc's does and
// as the convention says; the caller's copy is the caller's to place.) DSS's Win64 caller copy sat on a 16-byte
// boundary for 64, 128 and 256 before fold f26c (probe w2, DSS's own answer, which reads the address).
//
// A SHAPE A REFERENCE REFUSES IS EXPECTED REFUSED, NEVER LEFT OUT: cl 19.51 refuses a by-value parameter aligned
// past 64 (`error C2719`, probe w2: 64 taken, 128 and 256 refused), so of this universe the Win64 arm's reference
// refuses the 256-aligned shape. The arm hands cl the WHOLE units and expects exactly that refusal, by its
// measured line, once per parameter -- a cl that later takes it reddens this, and the cell is re-judged -- then
// builds its reference objects without that shape. DSS caller with DSS callee keeps it: C 6.7.5 makes the
// parameter an object of its 256-aligned type, and mingw-w64 gcc 13.2.0 takes the shape and delivers its values
// (probe w4: its copy misplaced).

#include "core/types/diagnostic_reporter.hpp"
#include "program/program.hpp"
#include "host_translations.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

// The shapes every arm calls. `long long` throughout, so a shape is the same bytes under every data model.
constexpr char const* kShapes =
    "#include <stdarg.h>\n"
    "struct Big { long long a, b, c; };\n";

// The stacked-alignment universe (main's ledger 452): a by-value aggregate of alignment 8 / 16 / 32 / 64 / 256,
// each after ONE stacked eightbyte (seven `long long`s before it) and after TWO (eight). gcc and clang begin each at
// its own alignment, uncapped (probe sv cell 0, probe sa); under AAPCS64 and Win64 each over 16 bytes is a pointer
// to a caller copy, so there they are controls.
struct Aligned {
    int         bytes;   // the aggregate's alignment
    int         words;   // its size in `long long`s
};
constexpr Aligned kAligned[] = {{8, 4}, {16, 4}, {32, 4}, {64, 8}, {256, 32}};

// The kAligned alignments a unit leaves out: the shapes an arm's reference compiler refuses (NativeArm::refused).
using Alignments = std::vector<int>;

[[nodiscard]] bool taken(Aligned const& a, Alignments const& refused) {
    return std::find(refused.begin(), refused.end(), a.bytes) == refused.end();
}

[[nodiscard]] std::string alignedShapes(Alignments const& refused) {
    std::string s;
    for (Aligned const& a : kAligned) {
        if (!taken(a, refused)) continue;
        std::string const n = std::to_string(a.bytes);
        s += "struct AL" + n + " { " + (a.bytes > 8 ? "_Alignas(" + n + ") " : std::string{}) + "long long v; long long w["
           + std::to_string(a.words - 1) + "]; };\n";
    }
    return s;
}

// The callee's PLACEMENT term: 1000 when the parameter `p` is not `n`-aligned, read through a volatile object so no
// compiler answers it from the type (probe w4: gcc does, even at -O0), else 0.
[[nodiscard]] std::string placementTerm(std::string const& p, int n) {
    return "void *volatile at = (void *)&" + p + "; long long const misplaced = ((unsigned long long)at & "
         + std::to_string(n - 1) + ") ? 1000 : 0; ";
}

// take_alN_odd: seven `long long`s, then the aggregate; take_alN_even: eight. Each returns 42 when every argument
// arrived: 1..7 (28) or 1..8 (36) less 8, plus v (10) and the last word (4) -- plus 1000 when its `&s` is not
// N-aligned (C 6.7.5: the parameter is an object of its type).
[[nodiscard]] std::string alignedCallees(Alignments const& refused) {
    std::string s;
    for (Aligned const& a : kAligned) {
        if (!taken(a, refused)) continue;
        std::string const n = std::to_string(a.bytes);
        std::string const last = std::to_string(a.words - 2);
        std::string const placed = placementTerm("s", a.bytes);
        s += "long long take_al" + n + "_odd(long long a, long long b, long long c, long long d, long long e, "
             "long long f, long long g, struct AL" + n + " s) { " + placed + "return misplaced + a + b + c + d + e "
             "+ f + g + s.v + s.w[" + last + "]; }\n";
        s += "long long take_al" + n + "_even(long long a, long long b, long long c, long long d, long long e, "
             "long long f, long long g, long long h, struct AL" + n + " s) { " + placed + "return misplaced + a + b "
             "+ c + d + e + f + g + h - 8 + s.v + s.w[" + last + "]; }\n";
    }
    return s;
}

[[nodiscard]] std::string alignedDecls(Alignments const& refused) {
    std::string s;
    for (Aligned const& a : kAligned) {
        if (!taken(a, refused)) continue;
        std::string const n = std::to_string(a.bytes);
        s += "long long take_al" + n + "_odd(long long a, long long b, long long c, long long d, long long e, "
             "long long f, long long g, struct AL" + n + " s);\n";
        s += "long long take_al" + n + "_even(long long a, long long b, long long c, long long d, long long e, "
             "long long f, long long g, long long h, struct AL" + n + " s);\n";
    }
    return s;
}

// Checks 6.. of the caller, two per alignment (odd, even); the first check of the SysV group follows them. A shape
// the unit leaves out keeps its two bits, unset.
[[nodiscard]] std::string alignedChecks(Alignments const& refused) {
    std::string s;
    int bit = 6;
    for (Aligned const& a : kAligned) {
        if (!taken(a, refused)) {
            bit += 2;
            continue;
        }
        std::string const n = std::to_string(a.bytes);
        std::string const last = std::to_string(a.words - 2);
        // Each bit in its own statement: two `bit++` in one expression are unsequenced.
        int const oddBit = bit++;
        int const evenBit = bit++;
        s += "    { struct AL" + n + " x = {0}; x.v = 10; x.w[" + last + "] = 4;\n"
             "      PLACED(take_al" + n + "_odd(1, 2, 3, 4, 5, 6, 7, x), " + std::to_string(oddBit) + ");\n"
             "      PLACED(take_al" + n + "_even(1, 2, 3, 4, 5, 6, 7, 8, x), " + std::to_string(evenBit) + "); }\n";
    }
    return s;
}

constexpr int kCommonChecks = 6 + 2 * static_cast<int>(sizeof kAligned / sizeof kAligned[0]);   // 16

// An over-aligned SCALAR (main's ledger 452): `typedef long long SN __attribute__((aligned(N)))` for N = 16, 32, 64,
// after one and after two stacked eightbytes. gcc and clang pass it as the scalar it is, in the next eightbyte, and
// their callee copies it to an N-aligned home when it takes its address (probe sa: every callee answer of 32 and 64
// is a local copy); a callee that reads it where its OWN alignment would put it reads the wrong eightbyte. The
// callee also says whether `&v` honours N (C 6.7.5: the object has the alignment its type states). The syntax is
// GNU's, so the Win64 control has no such cell.
constexpr int kScalarAligns[] = {16, 32, 64};

[[nodiscard]] std::string scalarShapes() {
    std::string s;
    for (int const n : kScalarAligns)
        s += "typedef long long S" + std::to_string(n) + " __attribute__((aligned(" + std::to_string(n) + ")));\n";
    return s;
}

// take_sN_odd / take_sN_even: 42 when every argument arrived (as alignedCallees), plus 1000 when `&v` is not
// N-aligned.
[[nodiscard]] std::string scalarCallees() {
    std::string s;
    for (int const n : kScalarAligns) {
        std::string const t = "S" + std::to_string(n);
        std::string const placed = placementTerm("v", n);
        s += "long long take_s" + std::to_string(n) + "_odd(long long a, long long b, long long c, long long d, "
             "long long e, long long f, long long g, " + t + " v) { " + placed + "return misplaced + a + b + c + d + "
             "e + f + g + v; }\n";
        s += "long long take_s" + std::to_string(n) + "_even(long long a, long long b, long long c, long long d, "
             "long long e, long long f, long long g, long long h, " + t + " v) { " + placed + "return misplaced + a "
             "+ b + c + d + e + f + g + h - 8 + v; }\n";
    }
    return s;
}

[[nodiscard]] std::string scalarDecls() {
    std::string s;
    for (int const n : kScalarAligns) {
        std::string const t = "S" + std::to_string(n);
        s += "long long take_s" + std::to_string(n) + "_odd(long long a, long long b, long long c, long long d, "
             "long long e, long long f, long long g, " + t + " v);\n";
        s += "long long take_s" + std::to_string(n) + "_even(long long a, long long b, long long c, long long d, "
             "long long e, long long f, long long g, long long h, " + t + " v);\n";
    }
    return s;
}

// Checks 19.. of the caller, two per alignment (odd, even): v = 14, so every one answers 42.
constexpr int kScalarChecksBegin = 19;

[[nodiscard]] std::string scalarChecks() {
    std::string s;
    int bit = kScalarChecksBegin;
    for (int const n : kScalarAligns) {
        std::string const t = "S" + std::to_string(n);
        int const oddBit = bit++;
        int const evenBit = bit++;
        s += "    { " + t + " x = 14;\n"
             "      PLACED(take_s" + std::to_string(n) + "_odd(1, 2, 3, 4, 5, 6, 7, x), " + std::to_string(oddBit)
           + ");\n"
             "      PLACED(take_s" + std::to_string(n) + "_even(1, 2, 3, 4, 5, 6, 7, 8, x), "
           + std::to_string(evenBit) + "); }\n";
    }
    return s;
}

// Checks 25 and 26 are the SysV group's union of `long double` and `long long` (after the scalars' bits).
constexpr int kAllChecks = kScalarChecksBegin + 2 * static_cast<int>(sizeof kScalarAligns / sizeof kScalarAligns[0]) + 2;

// THE CALLER'S FRAME (main's ledger 495): every check above is made from three callers -- a plain one; one whose
// stack pointer moves at run time first (a VLA, GNU arms: cl has no VLA); and one that is itself variadic -- since a
// caller that passes a stacked argument aligned past the stack realigns its stack pointer on the frame a VLA
// function uses, and a VLA or `va_start` in it must keep the boundary and its own bytes. Check 27 is the frame's own
// (the VLA's bytes, or the first variadic argument); check 28 an UNWIND WALK on the SysV arms, where the realignment
// happens: a callee below the caller walks the stack (`_Unwind_Backtrace`, from the driver) and must reach the
// caller and then `main` -- through a frame whose stack pointer was realigned, which only its call-frame
// information (the CFA rule moved to the frame pointer) describes.
constexpr int kFrameCheck     = kAllChecks;
constexpr int kWalkCheck      = kAllChecks + 1;
// Check 29: a 64-aligned aggregate with a stacked eightbyte AFTER it (the six integer registers taken first), so
// the caller's outgoing area -- 72 bytes -- is not a multiple of 64 until the caller rounds it, and a realignment
// computed off an unrounded area leaves the aggregate 8 bytes off its boundary. Every other stacked shape ends on
// its own alignment, so this is the one that makes the rounding, and the row that carries the alignment from
// MIR->LIR to the frame layout, load-bearing at run time.
constexpr int kTailCheck      = kAllChecks + 2;
constexpr int kReportedChecks = kAllChecks + 3;
constexpr Aligned kTailShape{64, 8};

struct CallerVariant {
    char const* tag;     // the driver's line for it
    char const* head;    // the function's head, through the declarations of `bad` and `misplaced`
    char const* frame;   // its own frame's check, made after every other
};
constexpr CallerVariant kPlainCaller{
    "plain", "int run_caller(long *bad_out, long *misplaced_out) {\n    long bad = 0;\n    long misplaced = 0;\n",
    ""};
constexpr CallerVariant kVlaCaller{
    "vla",
    "int run_caller_vla(int n, long *bad_out, long *misplaced_out) {\n    long bad = 0;\n    long misplaced = 0;\n"
    "    volatile char frame[n];\n    frame[0] = 27;\n    frame[n - 1] = 28;\n",
    "    if (frame[0] != 27 || frame[n - 1] != 28) bad |= 1L << 27;\n"};
constexpr CallerVariant kVaCaller{
    "va",
    "int run_caller_va(long *bad_out, long *misplaced_out, ...) {\n    long bad = 0;\n    long misplaced = 0;\n"
    "    va_list ap;\n    va_start(ap, misplaced_out);\n    int const k = va_arg(ap, int);\n    va_end(ap);\n",
    "    if (k != 27) bad |= 1L << 27;\n"};

// The callers a unit defines: the VLA one only where the reference has VLAs (GNU).
[[nodiscard]] std::vector<CallerVariant> callerVariants(bool gnu) {
    if (gnu) return {kPlainCaller, kVlaCaller, kVaCaller};
    return {kPlainCaller, kVaCaller};
}

// take_al64_tail: six `long long`s, the aggregate, then `k`: 21 + 10 + 4 + 7 = 42, plus 1000 when `&s` is not
// 64-aligned. Defined, declared and called only where the arm's reference takes the 64-aligned shape.
[[nodiscard]] std::string tailCallee(Alignments const& refused) {
    if (!taken(kTailShape, refused)) return {};
    return "long long take_al64_tail(long long a, long long b, long long c, long long d, long long e, long long f, "
           "struct AL64 s, long long k) { " + placementTerm("s", 64) + "return misplaced + a + b + c + d + e + f + "
           "s.v + s.w[6] + k; }\n";
}
[[nodiscard]] std::string tailDecl(Alignments const& refused) {
    if (!taken(kTailShape, refused)) return {};
    return "long long take_al64_tail(long long a, long long b, long long c, long long d, long long e, long long f, "
           "struct AL64 s, long long k);\n";
}
[[nodiscard]] std::string tailCheck(Alignments const& refused) {
    if (!taken(kTailShape, refused)) return {};
    return "    { struct AL64 x = {0}; x.v = 10; x.w[6] = 4;\n"
           "      PLACED(take_al64_tail(1, 2, 3, 4, 5, 6, x, 7), 29); }\n";
}

// The walk's callee (in the callee unit) and the driver's walker it calls.
constexpr char const* kWalkCallee =
    "long long unwound(void);\n"
    "long long walk_below(long long k) { return k + unwound(); }\n";
constexpr char const* kWalkDecl = "long long walk_below(long long k);\n";
constexpr char const* kWalkCheckText = "    if (walk_below(42) != 42) bad |= 1L << 28;\n";

constexpr char const* kX87Shapes =
    "struct LD { long double x; };\n"
    "struct A16 { long double x; long long y; };\n"
    "union ULI { long double a; long long i; };\n";

constexpr char const* kCalleeBody =
    "long long take_big(struct Big s, long long k) { return s.a + s.b + s.c + k; }\n"
    "long long take_after_six(long long a, long long b, long long c, long long d, long long e, long long f,\n"
    "                         struct Big s, long long k) {\n"
    "    return a + b + c + d + e + f + s.a + s.b + s.c + k;\n"
    "}\n"
    "long long take_two(struct Big x, struct Big y) { return x.a + y.c; }\n"
    "long long write_param(struct Big s) { s.a = 100; return s.a + s.b; }\n"
    "struct Big make_big(long long v) { struct Big r; r.a = v; r.b = v; r.c = v; return r; }\n"
    "long long take_var(int n, ...) {\n"
    "    va_list ap;\n"
    "    va_start(ap, n);\n"
    "    struct Big s = va_arg(ap, struct Big);\n"
    "    long long k = va_arg(ap, long long);\n"
    "    va_end(ap);\n"
    "    return n + s.a + s.b + s.c + k;\n"
    "}\n";

constexpr char const* kX87CalleeBody =
    "long long take_ld(struct LD s, long long k) { return (long long)s.x + k; }\n"
    "struct LD make_ld(long long v) { struct LD r; r.x = (long double)v; return r; }\n"
    "long long take_a16_after_seven(long long a, long long b, long long c, long long d, long long e, long long f,\n"
    "                               long long g, struct A16 s) {\n"
    "    return a + b + c + d + e + f + g + s.y + (long long)s.x - 21;\n"
    "}\n"
    "union ULI make_uli(long long v) { union ULI u; u.a = (long double)v; return u; }\n"
    "long long take_uli(union ULI u, long long k) { return (long long)u.a + k; }\n";

constexpr char const* kCallerDecls =
    "long long take_big(struct Big s, long long k);\n"
    "long long take_after_six(long long a, long long b, long long c, long long d, long long e, long long f,\n"
    "                         struct Big s, long long k);\n"
    "long long take_two(struct Big x, struct Big y);\n"
    "long long write_param(struct Big s);\n"
    "struct Big make_big(long long v);\n"
    "long long take_var(int n, ...);\n";

constexpr char const* kX87CallerDecls =
    "long long take_ld(struct LD s, long long k);\n"
    "struct LD make_ld(long long v);\n"
    "long long take_a16_after_seven(long long a, long long b, long long c, long long d, long long e, long long f,\n"
    "                               long long g, struct A16 s);\n"
    "union ULI make_uli(long long v);\n"
    "long long take_uli(union ULI u, long long k);\n";

// A placement check: 42 = arrived and placed; 1042 = arrived, the callee's object misplaced; anything else = a
// value that did not arrive.
constexpr char const* kPlacedMacro =
    "#define PLACED(call, bit) do { long long const r_ = (call); \\\n"
    "    if (r_ == 1042) misplaced |= 1L << (bit); else if (r_ != 42) bad |= 1L << (bit); } while (0)\n";

// The checks every caller makes first (0-5).
constexpr char const* kCommonCallerChecks =
    "    struct Big b = { 1, 2, 3 };\n"
    "    struct Big c = { 10, 20, 32 };\n"
    "    if (take_big(b, 36) != 42) bad |= 1L << 0;\n"
    "    if (take_after_six(1, 2, 3, 4, 5, 6, b, 15) != 42) bad |= 1L << 1;\n"
    "    if (take_two(c, b) != 13) bad |= 1L << 2;\n"
    "    if (write_param(b) != 102 || b.a != 1) bad |= 1L << 3;\n"
    "    struct Big r = make_big(14);\n"
    "    if (r.a + r.b + r.c != 42) bad |= 1L << 4;\n"
    "    if (take_var(30, b, 6LL) != 42) bad |= 1L << 5;\n";

constexpr char const* kX87CallerChecks =
    "    struct LD l;\n"
    "    l.x = 40.0L;\n"
    "    if (take_ld(l, 2) != 42) bad |= 1L << 16;\n"
    "    struct LD m = make_ld(42);\n"
    "    if ((long long)m.x != 42) bad |= 1L << 17;\n"
    "    struct A16 a;\n"
    "    a.x = 2.0L;\n"
    "    a.y = 33;\n"
    "    if (take_a16_after_seven(1, 2, 3, 4, 5, 6, 7, a) != 42) bad |= 1L << 18;\n"
    "    union ULI um = make_uli(42);\n"
    "    if ((long long)um.a != 42) bad |= 1L << 25;\n"
    "    union ULI ua;\n"
    "    ua.a = 40.0L;\n"
    "    if (take_uli(ua, 2) != 42) bad |= 1L << 26;\n";

[[nodiscard]] std::string callerTail() {
    return "    *bad_out = bad;\n"
           "    *misplaced_out = misplaced;\n"
           "    for (int i = 0; i < " + std::to_string(kReportedChecks) + "; ++i)\n"
           "        if (bad & (1L << i)) return 100 + i;\n"
           "    for (int i = 0; i < " + std::to_string(kReportedChecks) + "; ++i)\n"
           "        if (misplaced & (1L << i)) return 200 + i;\n"
           "    return 42;\n"
           "}\n";
}

// The driver (always the reference's) calls every caller and prints each one's masks on its own line
// (`<tag> bad=<hex> misplaced=<hex>`), so a misplaced object behind a first failure is seen too; its exit is the
// first caller's that is not 42. `walk`: it defines the unwind walk check 28 calls (`unwound`), which reaches the
// caller under test and then `main`, or answers 1.
[[nodiscard]] std::string driverSource(bool gnu, bool walk) {
    std::string s = walk ? "#define _GNU_SOURCE 1\n" : "";
    s += "#include <stdio.h>\n";
    if (walk) s += "#include <dlfcn.h>\n#include <unwind.h>\n";
    s += "int run_caller(long *bad_out, long *misplaced_out);\n"
         "int run_caller_va(long *bad_out, long *misplaced_out, ...);\n";
    if (gnu) s += "int run_caller_vla(int n, long *bad_out, long *misplaced_out);\n";
    s += "static void *caller_under_test;\n";
    if (walk)
        s += "int main(void);\n"
             "struct Walk { int caller, top; };\n"
             // A frame is a function's when EITHER answer names it. Probe uw, MEASURED: Apple clang 21's
             // _Unwind_FindEnclosingFunction named neither `caller` nor `main` of a three-frame walk at -O0 (on
             // x86_64 and on arm64; it named `caller` at -O1), where dladdr named both; gcc 13.3.0's named both,
             // where dladdr names no symbol of the executable itself (none is in its dynamic table).
             "static int is(void *pc, void *fn) {\n"
             "    Dl_info di;\n"
             "    if (_Unwind_FindEnclosingFunction(pc) == fn) return 1;\n"
             "    return dladdr(pc, &di) && di.dli_saddr == fn;\n"
             "}\n"
             "static _Unwind_Reason_Code step(struct _Unwind_Context *c, void *arg) {\n"
             "    struct Walk *w = (struct Walk *)arg;\n"
             "    void *const pc = (void *)(_Unwind_GetIP(c) - 1);\n"
             "    if (is(pc, caller_under_test)) w->caller = 1;\n"
             "    if (is(pc, (void *)&main)) { w->top = 1; return _URC_END_OF_STACK; }\n"
             "    return _URC_NO_REASON;\n"
             "}\n"
             "long long unwound(void) {\n"
             "    struct Walk w = { 0, 0 };\n"
             "    _Unwind_Backtrace(step, &w);\n"
             "    return w.caller && w.top ? 0 : 1;\n"
             "}\n";
    s += "static int first = 42;\n"
         "static void report(char const *tag, int rc, long bad, long misplaced) {\n"
         "    printf(\"%s bad=%lx misplaced=%lx\\n\", tag, (unsigned long)bad, (unsigned long)misplaced);\n"
         "    if (first == 42) first = rc;\n"
         "}\n"
         "int main(void) {\n"
         "    long bad = -1, misplaced = -1;\n"
         "    caller_under_test = (void *)&run_caller;\n"
         "    int rc = run_caller(&bad, &misplaced);\n"
         "    report(\"plain\", rc, bad, misplaced);\n";
    if (gnu)
        s += "    volatile int n = 3;\n"
             "    bad = -1; misplaced = -1;\n"
             "    caller_under_test = (void *)&run_caller_vla;\n"
             "    rc = run_caller_vla(n, &bad, &misplaced);\n"
             "    report(\"vla\", rc, bad, misplaced);\n";
    s += "    bad = -1; misplaced = -1;\n"
         "    caller_under_test = (void *)&run_caller_va;\n"
         "    rc = run_caller_va(&bad, &misplaced, 27);\n"
         "    report(\"va\", rc, bad, misplaced);\n"
         "    return first;\n"
         "}\n";
    return s;
}

constexpr char const* kChecks =
    "exit 100 + the first check whose VALUES did not arrive, else 200 + the first whose callee's parameter object "
    "was MISPLACED (its address, read through a volatile object, not aligned as its type says), else 42: "
    "0 take_big, 1 take_after_six (no register left), 2 take_two, 3 write_param "
    "(a callee writes its own copy), 4 make_big (the MEMORY result, a control), 5 take_var (`...`, a control), "
    "6-15 take_alN_odd / take_alN_even for N = 8, 16, 32, 64, 256 in that order (a stacked aggregate begins on its "
    "own alignment after one and after two stacked eightbytes; placed by the caller's stacked bytes, or its copy; a "
    "shape the arm's reference refuses is in the DSS caller + DSS callee pairing only), 16 take_ld (an x87-only "
    "struct argument), 17 make_ld "
    "(the X87 result in st0), 18 take_a16_after_seven (a 16-aligned x87 struct after one stacked eightbyte), "
    "19-24 take_sN_odd / take_sN_even for an over-aligned scalar typedef, N = 16, 32, 64 in that order (passed in "
    "the next eightbyte; `&v` the callee's own home), 25 make_uli and 26 take_uli (a union of "
    "`long double` and `long long` as a result and as an argument: MEMORY under psABI, %rax + %xmm0 on Apple's x86_64), "
    "27 the caller's own frame (its VLA's bytes, or its first variadic argument), 28 an unwind walk from a callee "
    "below the caller, through it, to `main` (SysV arms), 29 take_al64_tail (a 64-aligned aggregate with a stacked "
    "eightbyte after it: the caller's outgoing area is a multiple of 64 only once rounded); every check is made by "
    "each caller the driver names";

static_assert(kCommonChecks == 16, "the SysV group's checks begin at 16");
static_assert(kAllChecks == 27, "the shapes' checks are 0-26");
static_assert(kFrameCheck == 27 && kWalkCheck == 28 && kTailCheck == 29,
              "the frame's, the walk's and the tail's bits are spelled 27, 28 and 29 above");
static_assert(kReportedChecks <= 31, "every check is one bit of a `long`");

// `x87`: the SysV group; `gnu`: the over-aligned scalar typedefs, whose attribute is GNU's; `refused`: the
// stacked-alignment shapes the unit leaves out.
// `walk`: the unwind walk (check 28), on the SysV arms.
[[nodiscard]] std::string calleeSource(bool x87, bool gnu, bool walk, Alignments const& refused) {
    return std::string{kShapes} + alignedShapes(refused) + (x87 ? kX87Shapes : "") + (gnu ? scalarShapes() : "")
         + kCalleeBody + alignedCallees(refused) + tailCallee(refused) + (x87 ? kX87CalleeBody : "")
         + (gnu ? scalarCallees() : "") + (walk ? kWalkCallee : "");
}

[[nodiscard]] std::string callerSource(bool x87, bool gnu, bool walk, Alignments const& refused) {
    std::string const checks = std::string{kCommonCallerChecks} + alignedChecks(refused) + tailCheck(refused)
                             + (x87 ? kX87CallerChecks : "") + (gnu ? scalarChecks() : "")
                             + (walk ? kWalkCheckText : "");
    std::string s = std::string{kShapes} + alignedShapes(refused) + (x87 ? kX87Shapes : "")
                  + (gnu ? scalarShapes() : "") + kCallerDecls + alignedDecls(refused) + tailDecl(refused)
                  + (x87 ? kX87CallerDecls : "") + (gnu ? scalarDecls() : "") + (walk ? kWalkDecl : "")
                  + kPlacedMacro;
    for (CallerVariant const& v : callerVariants(gnu)) s += std::string{v.head} + checks + v.frame + callerTail();
    return s;
}

void writeText(fs::path const& p, std::string_view text) {
    std::ofstream(p, std::ios::binary) << text;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string all;
    for (auto const& d : rep.all()) all += "\n  " + d.actual;
    return all;
}

[[nodiscard]] int runCapturing(std::string const& cmd, fs::path const& log) {
    return std::system(test_support::native_probe::captureCmd(cmd, log).c_str());
}

// One unit to a relocatable object of `spec` through the production driver: the object the driver wrote for
// `<stem>.c` (whatever its format's suffix), or empty with the diagnostics in `rep`.
[[nodiscard]] fs::path dssObject(fs::path const& dir, std::string const& stem, std::string const& text,
                                 std::string const& spec, CompileConfig config, DiagnosticReporter& rep) {
    fs::path const src = dir / (stem + ".c");
    writeText(src, text);
    fs::path const out = dir / ("dss-" + stem);
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    p.setCompileConfig(config);
    if (p.compileFiles(std::vector<std::string>{src.string()}, "c", std::vector<std::string>{spec}, rep) != 0)
        return {};
    for (auto const& e : fs::directory_iterator(out))
        if (e.is_regular_file() && e.path().stem() == stem) return e.path();
    return {};
}

// A stacked-alignment shape an arm's REFERENCE compiler refuses as a by-value parameter: its alignment, and the
// line the reference was measured to refuse it by. Each shape is two parameters in each unit (take_alN_odd and
// take_alN_even, defined in the callee and declared in the caller), so each unit says that line twice.
struct RefusedShape {
    int         alignment;
    std::string line;
};
constexpr std::size_t kParametersPerShape = 2;

struct NativeArm {
    std::string label;
    char const* spec;       // the relocatable object DSS writes
    std::string cc;         // the reference compiler driver: `cc`, `clang`, or `cl` (the MSVC arm)
    std::string ccFlags;    // its flags, for compiling and for linking
    bool x87;               // the SysV shapes (an x87 `long double`)
    bool msvc = false;      // compiled and linked under the MSVC developer environment
    // The shapes the reference refuses. The arm hands it the WHOLE units and EXPECTS exactly these refusals (a
    // reference that later takes one reddens the test, and the cell is re-judged), then builds its reference
    // objects without them; the DSS caller + DSS callee pairing keeps every shape.
    std::vector<RefusedShape> refused = {};
    // The convention hands a large aggregate as a pointer to a copy the CALLER makes (AAPCS64, Win64): with a
    // reference caller, that copy -- the DSS callee's parameter object -- is the reference's to place.
    bool callerCopy = false;
    // A DSS relocatable object of this arm's format carries NO unwind table yet (the Mach-O half of
    // D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS; main's ledger 510): the unwind walk (check 28) of every
    // pairing holding a DSS object is PRINTED, naming that row, never asserted; the control's walk is asserted.
    bool walkWaitsOnObjectUnwind = false;
};

// The checks whose object a pointer convention's CALLER places: the stacked-alignment aggregates (6-15, and the
// tail shape, 29).
constexpr unsigned long kCallerPlacedChecks =
    (((1UL << 16) - 1UL) & ~((1UL << 6) - 1UL)) | (1UL << kTailCheck);
// The checks whose object the CALLEE places: an over-aligned scalar's `&v` (19-24) is the callee's own home -- it
// arrives as the scalar it is and the callee copies it where its address can be taken (probe sa). ✔MEASURED, run
// f26c-s22 on linux-arm64-debug: gcc 13.3.0 aarch64's own callee misplaces the 32- and 64-aligned homes in the
// control (mask 0x1e00000) and the 64-aligned one under a DSS caller (0x800000).
constexpr unsigned long kCalleeHomeChecks = ((1UL << 25) - 1UL) & ~((1UL << kScalarChecksBegin) - 1UL);
constexpr unsigned long kEveryCheck       = (1UL << kReportedChecks) - 1UL;

// The two masks the driver prints for the caller `tag` (`<tag> bad=<hex> misplaced=<hex>`, one line), or nullopt
// when it printed no such line.
struct CheckMasks {
    unsigned long bad;
    unsigned long misplaced;
};
[[nodiscard]] std::optional<CheckMasks> masksOf(std::string const& out, std::string const& tag) {
    std::string const lead = tag + " bad=";
    std::size_t at = 0;
    while (at < out.size()) {
        std::size_t const end = out.find('\n', at);
        std::string const line = out.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? out.size() : end + 1;
        if (line.rfind(lead, 0) != 0) continue;
        auto const m = line.find(" misplaced=");
        if (m == std::string::npos) return std::nullopt;
        try {
            return CheckMasks{std::stoul(line.substr(lead.size()), nullptr, 16),
                              std::stoul(line.substr(m + 11), nullptr, 16)};
        } catch (std::exception const&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool hostHas(std::string const& tool) {
#if defined(_WIN32)
    return std::system(("where " + tool + " >nul 2>&1").c_str()) == 0;
#else
    return std::system(("command -v " + tool + " >/dev/null 2>&1").c_str()) == 0;
#endif
}

[[nodiscard]] std::vector<NativeArm> nativeArms() {
#if defined(__linux__) && defined(__x86_64__)
    std::vector<NativeArm> arms{{"SysV, gcc + GNU ld (x86_64)", "x86_64:elf64-x86_64-linux", "cc",
                                 "-fno-pie -no-pie", true}};
    if (hostHas("clang"))
        arms.push_back({"SysV, clang + GNU ld (x86_64)", "x86_64:elf64-x86_64-linux", "clang", "-fno-pie -no-pie",
                        true});
    return arms;
#elif defined(__linux__) && defined(__aarch64__)
    return {{"AAPCS64 control, gcc + GNU ld (aarch64)", "arm64:elf64-aarch64-linux", "cc", "-fno-pie -no-pie",
             false, false, {}, /*callerCopy=*/true}};
#elif defined(__APPLE__)
    return {{"SysV, Apple clang + ld -arch x86_64", "x86_64:macho64-x86_64-darwin", "cc", "-arch x86_64", true, false,
             {}, /*callerCopy=*/false, /*walkWaitsOnObjectUnwind=*/true},
            {"AAPCS64 control, Apple clang + ld -arch arm64", "arm64:macho64-arm64-darwin", "cc", "-arch arm64",
             false, false, {}, /*callerCopy=*/true}};
#elif defined(_WIN32)
    // Win64's control: a large aggregate is a pointer to a caller copy, read against cl -- which refuses the
    // 256-aligned shape as a by-value parameter (C2719, measured on cl 19.51 in run f26c-s20 and probe w2).
    // And mingw-w64 gcc with GNU ld (main's ledger 495: Win64 is gcc's as well as cl's), which takes every shape.
    return {{"Win64 control, cl + link.exe", "x86_64:pe64-x86_64-windows", "cl", "/nologo /O1 /std:c11", false, true,
             {{256, "error C2719: 's': formal parameter with requested alignment of 256 won't be aligned"}},
             /*callerCopy=*/true},
            {"Win64 control, mingw-w64 gcc + GNU ld", "x86_64:pe64-x86_64-windows", "gcc", "", false, false, {},
             /*callerCopy=*/true}};
#else
    return {};
#endif
}

}  // namespace

TEST(SysVMemoryClassReferenceLinkNative, EveryPairingOfDssAndTheHostsCompilerAgreesOnEveryShape) {
    namespace np = test_support::native_probe;
    auto const arms = nativeArms();
    if (arms.empty()) GTEST_SKIP() << "no SysV, AAPCS64 or Win64 host toolchain arm is declared for this host";
    test_support::ScratchDir scratch{test_support::Location::Temp, "sysv-memory-class-ref-link"};
    fs::path const dir = scratch.path();
    auto const strict = test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed) << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
                                   << "' is not a recognised value";
    for (std::size_t k = 0; k < arms.size(); ++k) {
        NativeArm const& arm = arms[k];
        SCOPED_TRACE(arm.label);
        fs::path const armDir = dir / ("arm" + std::to_string(k));
        fs::create_directories(armDir);
        // The reference toolchain of the arm: absent is said and not run, never a pass.
        std::optional<np::MsvcTools> tools;
        if (arm.msvc) {
            auto const msvc = np::locateMsvcToolchain(armDir);
            if (msvc.toolAbsent()) {
                std::cout << "[native-arm] " << arm.label << ": not run — " << msvc.detail << "\n";
                continue;
            }
            ASSERT_TRUE(msvc.ok()) << msvc.describe();
            tools.emplace(np::msvcToolsIn(msvc, armDir));
            ASSERT_TRUE(tools->ready()) << tools->describe();
        } else {
            writeText(armDir / "check.c", "int main(void){return 0;}\n");
            if (runCapturing(arm.cc + " " + arm.ccFlags + " -o \"" + (armDir / "check").string() + "\" \""
                                 + (armDir / "check.c").string() + "\"",
                             armDir / "check.txt")
                != 0) {
                std::cout << "[native-arm] " << arm.label << ": not run — `" << arm.cc
                          << "` cannot build a trivial program" << np::tailOf(armDir / "check.txt", 5) << "\n";
                continue;
            }
        }
        // One reference tool line in the arm's directory, its output kept in `log`.
        auto const tool = [&](std::string const& cmd, fs::path const& log) -> int {
            if (tools.has_value())
                return np::systemUnder(tools->env,
                                       np::captureCmd("cd /d \"" + armDir.string() + "\" && " + cmd, log));
            return runCapturing(cmd, log);
        };
        std::vector<std::string> launcher;
        bool launcherExecsImage = false;
        if (std::string const arch = test_support::specTargetArch(arm.spec);
            arch != test_support::currentHostArch()) {
            auto const gate = test_support::crossArchDecisionForThisHost(arch, /*manifestEmulator=*/"");
            if (!gate.runs) {
                if (strict.on && test_support::armVerdictIsEnvironmentalSkip(gate.skip)) {
                    ADD_FAILURE() << arm.label << ": " << gate.why;
                } else {
                    std::cout << "[native-arm] " << arm.label << ": not run — " << gate.why << "\n";
                }
                continue;
            }
            launcher           = gate.launcherPrefix;
            launcherExecsImage = gate.launcherExecsImage;
        }
        // The two units, each by DSS and by the reference compiler; the driver by the reference's. DSS compiles
        // every shape; the reference units leave out the shapes the reference refuses, and the pairings with a
        // reference object take DSS's objects of those same units.
        DiagnosticReporter rep;
        bool const gnu = !arm.msvc;
        Alignments refusedAlignments;
        for (RefusedShape const& r : arm.refused) refusedAlignments.push_back(r.alignment);
        // The unwind walk where the realignment happens: the SysV arms -- asserted where every object carries an
        // unwind table, printed in a pairing holding a DSS object where the arm's objects do not (`waitsWalk`).
        bool const walk = arm.x87;
        unsigned long const waitsWalk = walk && arm.walkWaitsOnObjectUnwind ? 1UL << kWalkCheck : 0UL;
        std::string const calleeAll = calleeSource(arm.x87, gnu, walk, {});
        std::string const callerAll = callerSource(arm.x87, gnu, walk, {});
        std::string const calleeTaken = calleeSource(arm.x87, gnu, walk, refusedAlignments);
        std::string const callerTaken = callerSource(arm.x87, gnu, walk, refusedAlignments);
        // DSS's objects at BOTH build configurations: the release pipeline reaches the same call boundary after
        // the optimizer, and the frame it realigns is the one MIR->LIR builds after it.
        struct DssObjects {
            char const* config;
            fs::path    calleeAll, callerAll, callee, caller;
        };
        std::vector<DssObjects> dssObjects;
        for (auto const& [configName, config] :
             {std::pair{"debug", CompileConfig::Debug}, std::pair{"release", CompileConfig::Release}}) {
            SCOPED_TRACE(configName);
            std::string const at = std::string{"-"} + configName;
            DssObjects o{configName, {}, {}, {}, {}};
            o.calleeAll = dssObject(armDir, "callee" + at, calleeAll, arm.spec, config, rep);
            ASSERT_FALSE(o.calleeAll.empty()) << "DSS must compile the callee unit:" << diagnosticsOf(rep);
            o.callerAll = dssObject(armDir, "caller" + at, callerAll, arm.spec, config, rep);
            ASSERT_FALSE(o.callerAll.empty()) << "DSS must compile the caller unit:" << diagnosticsOf(rep);
            o.callee = o.calleeAll;
            o.caller = o.callerAll;
            if (!refusedAlignments.empty()) {
                o.callee = dssObject(armDir, "callee-taken" + at, calleeTaken, arm.spec, config, rep);
                ASSERT_FALSE(o.callee.empty()) << "DSS must compile the callee unit the reference takes:"
                                               << diagnosticsOf(rep);
                o.caller = dssObject(armDir, "caller-taken" + at, callerTaken, arm.spec, config, rep);
                ASSERT_FALSE(o.caller.empty()) << "DSS must compile the caller unit the reference takes:"
                                               << diagnosticsOf(rep);
            }
            dssObjects.push_back(std::move(o));
        }
        std::string const objSuffix = arm.msvc ? ".obj" : ".o";
        // One unit through the reference compiler: its exit code; the object at `obj`, the output in `log`.
        auto const refCompile = [&](std::string const& stem, std::string const& text, fs::path const& obj,
                                    fs::path const& log, std::string& cmd) -> int {
            fs::path const src = armDir / ("ref-" + stem + ".c");
            writeText(src, text);
            cmd = arm.msvc ? "cl " + arm.ccFlags + " /c /Fo\"" + obj.string() + "\" \"" + src.string() + "\""
                           : arm.cc + " " + arm.ccFlags + " -O1 -c -o \"" + obj.string() + "\" \"" + src.string()
                                 + "\"";
            return tool(cmd, log);
        };
        auto const refObject = [&](std::string const& stem, std::string const& text) -> fs::path {
            fs::path const obj = armDir / ("ref-" + stem + objSuffix);
            fs::path const log = armDir / ("ref-" + stem + ".txt");
            std::string cmd;
            EXPECT_EQ(refCompile(stem, text, obj, log, cmd), 0) << cmd << "\n" << np::tailOf(log, 20);
            return obj;
        };
        // The shapes the reference refuses, EXPECTED refused: the WHOLE unit is handed to it, and it must refuse
        // exactly those, each by its measured line once per parameter, and nothing else.
        auto const expectRefused = [&](std::string const& stem, std::string const& text) {
            fs::path const obj = armDir / ("ref-all-" + stem + objSuffix);
            fs::path const log = armDir / ("ref-all-" + stem + ".txt");
            std::string cmd;
            EXPECT_NE(refCompile("all-" + stem, text, obj, log, cmd), 0)
                << arm.label << ": the reference now TAKES every shape of the " << stem
                << " unit -- re-judge the cell (a shape it took joins the pairings):\n" << cmd;
            std::vector<std::size_t> said(arm.refused.size(), 0);
            std::string other;
            std::ifstream in{log};
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.find(": error ") == std::string::npos && line.find(": fatal error ") == std::string::npos)
                    continue;
                auto const hit = std::find_if(arm.refused.begin(), arm.refused.end(), [&](RefusedShape const& r) {
                    return line.find(r.line) != std::string::npos;
                });
                if (hit == arm.refused.end()) other += "\n  " + line;
                else ++said[static_cast<std::size_t>(hit - arm.refused.begin())];
            }
            for (std::size_t i = 0; i < arm.refused.size(); ++i)
                EXPECT_EQ(said[i], kParametersPerShape)
                    << arm.label << ", the " << stem << " unit: `" << arm.refused[i].line << "` said " << said[i]
                    << " time(s), once per parameter of the " << arm.refused[i].alignment << "-aligned shape expected"
                    << np::tailOf(log, 20);
            EXPECT_TRUE(other.empty()) << arm.label << ", the " << stem
                                       << " unit: the reference refuses something else too:" << other;
        };
        if (!arm.refused.empty()) {
            expectRefused("callee", calleeAll);
            expectRefused("caller", callerAll);
        }
        fs::path const refCallee = refObject("callee", calleeTaken);
        fs::path const refCaller = refObject("caller", callerTaken);
        fs::path const driver    = refObject("driver", driverSource(gnu, walk));
        // `referencePlaced`: the checks whose parameter object the REFERENCE placed in that pairing -- every one in
        // the control; under a pointer convention, the caller copies a reference caller makes; the scalar homes a
        // reference callee makes. Their placement is printed, never asserted; every other object of every pairing
        // is DSS's to place -- its stacked bytes and its caller copies, which a reference callee reads in place, and
        // every object of a DSS callee -- and every value is asserted everywhere.
        struct Pairing {
            std::string   label;
            std::string   tag;
            fs::path      caller;
            fs::path      callee;
            unsigned long referencePlaced;
            // The walk's bit when this pairing holds a DSS object on an arm whose objects carry no unwind table:
            // its failure is printed, never asserted.
            unsigned long walkPrinted;
        };
        std::vector<Pairing> pairings;
        for (DssObjects const& o : dssObjects) {
            std::string const at = std::string{" ("} + o.config + ")";
            std::string const tagAt = std::string{"-"} + o.config;
            pairings.push_back({"DSS caller, DSS callee, every shape" + at, "dd" + tagAt, o.callerAll, o.calleeAll,
                                0UL, waitsWalk});
            pairings.push_back({"reference caller, DSS callee" + at, "rd" + tagAt, refCaller, o.callee,
                                arm.callerCopy ? kCallerPlacedChecks : 0UL, waitsWalk});
            pairings.push_back({"DSS caller, reference callee" + at, "dr" + tagAt, o.caller, refCallee,
                                kCalleeHomeChecks, waitsWalk});
        }
        pairings.push_back({"reference caller, reference callee (the control)", "rr", refCaller, refCallee,
                            kEveryCheck, 0UL});
#if defined(_WIN32)
        std::string const exeSuffix = ".exe";
#else
        std::string const exeSuffix;
#endif
        for (Pairing const& pair : pairings) {
            SCOPED_TRACE(pair.label);
            fs::path const prog = armDir / ("prog-" + pair.tag + exeSuffix);
            fs::path const linkLog = armDir / ("link-" + pair.tag + ".txt");
            std::string const objs = "\"" + driver.string() + "\" \"" + pair.caller.string() + "\" \""
                                   + pair.callee.string() + "\"";
            std::string const link = arm.msvc ? "cl " + arm.ccFlags + " /Fe\"" + prog.string() + "\" " + objs
                                              : arm.cc + " " + arm.ccFlags + " -o \"" + prog.string() + "\" " + objs;
            ASSERT_EQ(tool(link, linkLog), 0)
                << "the reference linker refused the pairing: " << link << "\n" << np::tailOf(linkLog, 20);
            auto const r = test_support::runBinary(prog, test_support::kRunBudget, /*captureStdout=*/true,
                                                   launcher, /*programArgs=*/{}, launcherExecsImage);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            bool everyCallerClean = true;
            for (CallerVariant const& v : callerVariants(gnu)) {
                SCOPED_TRACE(v.tag);
                auto const masks = masksOf(r.capturedStdout, v.tag);
                // Reported, and the next pairing still runs: every pairing's verdict is read in one run.
                EXPECT_TRUE(masks.has_value()) << arm.label << ", " << pair.label << ": the driver printed no masks "
                                               << "for the " << v.tag << " caller (exit " << r.exitCode
                                               << "): " << r.capturedStdout;
                if (!masks.has_value()) {
                    everyCallerClean = false;
                    continue;
                }
                EXPECT_EQ(masks->bad & ~pair.walkPrinted, 0UL)
                    << arm.label << ", " << pair.label << ", the " << v.tag
                    << " caller: values did not arrive at check(s) of mask 0x" << std::hex
                    << (masks->bad & ~pair.walkPrinted) << std::dec << ", exit " << r.exitCode << " -- " << kChecks;
                EXPECT_EQ(masks->misplaced & ~pair.referencePlaced, 0UL)
                    << arm.label << ", " << pair.label << ", the " << v.tag << " caller: a parameter object DSS "
                    << "placed is misplaced at check(s) of mask 0x" << std::hex
                    << (masks->misplaced & ~pair.referencePlaced) << std::dec << ", exit " << r.exitCode << " -- "
                    << kChecks;
                if (masks->bad != 0UL || masks->misplaced != 0UL) everyCallerClean = false;
                std::cout << "[native-arm] " << arm.label << ", " << pair.label << ", the " << v.tag
                          << " caller: bad 0x" << std::hex << masks->bad << ", misplaced 0x" << masks->misplaced
                          << std::dec
                          << ((masks->misplaced & pair.referencePlaced) != 0UL
                                  ? " (the reference's own placement, not asserted)"
                                  : "")
                          << (pair.walkPrinted == 0UL ? ""
                              : (masks->bad & pair.walkPrinted) != 0UL
                                  ? " (the walk, check 28, FAILED: it waits on "
                                    "D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS -- printed, not asserted)"
                                  : " (the walk, check 28, PASSED: the row it waited on, "
                                    "D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS, no longer stops it -- assert it)")
                          << "\n";
            }
            EXPECT_EQ(r.exitCode == 42u, everyCallerClean)
                << "the exit and the printed masks disagree: exit " << r.exitCode << ", " << r.capturedStdout;
            std::cout << "[native-arm] " << arm.label << ", " << pair.label << ": exit " << r.exitCode
                      << "\n";
        }
    }
}
