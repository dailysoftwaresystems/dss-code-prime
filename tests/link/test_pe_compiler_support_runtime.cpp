// P69 fold H5, lane p69/hm — the routines a reference toolchain's OWN static runtime defines for the objects it
// compiles, which DSS's PE `compilerSupport` runtime defines (src/dss-config/runtime/platform/src/pe_compiler_support.c,
// pe_compiler_support_x86_64.s, pe_gcc_unwind.c; the four pe64 documents' `compilerSupport` row and block).
//
//   D-LK-PE-FOREIGN-OBJECT-STACK-PROBE-ROUTINE-UNDEFINED — a foreign object whose frame passes a page names the stack
//     probe: cl `__chkstk`, mingw-w64 gcc `___chkstk_ms`; DSS refused every such object undefined.
//   D-LK-PE-FOREIGN-OBJECT-GS-COOKIE-ROUTINES-UNDEFINED — cl's /GS family (`__security_cookie`,
//     `__security_check_cookie`, `__GSHandlerCheck`, `__GSHandlerCheck_SEH`, `__report_rangecheckfailure`) and cl's
//     `_fltused`; DSS refused every /GS object undefined.
//   D-LK-PE-FOREIGN-OBJECT-GCC-MAIN-HOOK-UNDEFINED — every mingw-w64 gcc `main` calls `__main`; DSS refused it.
//   And gcc's unwinder entry points a `-fexceptions` object with a `cleanup` names, `__gcc_personality_seh0` and
//   `_Unwind_Resume`; and gcc's stack protector (`-fstack-protector*`, gcc's /GS), `__stack_chk_guard` and
//   `__stack_chk_fail`.
//
// THE PINS ARE TABLES OVER THE MEASURED CELLS OF THE UNIVERSE -- {cl /Od, /O2} x {/GS, /GS-}, mingw-w64 gcc {-O0, -O2}
// with -fexceptions and with -fstack-protector-strong, and the census's cl, clang-cl and gcc objects -- linked into an
// EXE and into a DLL (the two pe64 documents that link; the staticlib and object documents link nothing, and every
// one of the four realizes the role: test_runtime_library_roles.cpp). No leg has a clang-cl (MEASURED again
// 2026-11-03, probe-reference-cc run 20261103-190643-3d44b528): its cells beyond the census's /O2 /GS object are
// unmeasured, and a row added here is the day one is.
//
// The fixtures (each .source.c states how it was built — through probe-reference-cc's keep mode, twice, the two
// reports' md5s equal — and each reference's own run of the same source is the measurement the pins below hold DSS
// to; every one rebuilt from its final recipe, the same md5s: runs 20261103-192148-62c96676 cl,
// 20261103-192258-21a07b3f gcc):
//   pe_compiler_support_x86_64_pe_cl_MD_O2_GS.obj       6ba4e02049be8c5da17cb66191664bb1 (5832 bytes)
//   pe_compiler_support_x86_64_pe_cl_MD_Od_GS.obj       da4aeaa6e83d47b4923426e4b5bd6676 (5247 bytes)
//   pe_compiler_support_x86_64_pe_cl_MD_O2_GSoff.obj    3ae18313ab73620236a6246a9570b249 (5151 bytes)
//   pe_compiler_support_x86_64_pe_cl_MD_Od_GSoff.obj    6d671baa77ea878179641b4652af6644 (4476 bytes)
//   pe_gcc_runtime_x86_64_pe_gcc_O0_fexceptions.o       d252b2c8f95ebb13324140a12c167c30 (2739 bytes)
//   pe_gcc_runtime_x86_64_pe_gcc_O2_fexceptions.o       8bd3467706bf94a9b2ccbb6a125d2609 (3263 bytes)
//   pe_gcc_stack_protector_x86_64_pe_gcc_O0_ssp.o       6d41c56e44324e8026a8348376a443c4 (1208 bytes)
//   pe_gcc_stack_protector_x86_64_pe_gcc_O2_ssp.o       f17637cb83c8b6cf8ecf7b2d9ef4234a (1210 bytes)
// cl's object has a CONSTANT `__except` filter and a `sum` that reads through a volatile view, so cl writes neither
// a filter funclet (an expression filter puts one in an ASSOCIATIVE code COMDAT) nor chained unwind (a vectorized
// loop gets UNW_FLAG_CHAININFO records inside its function): readings this file does not claim. The first cut,
// which has both, is pe_chained_unwind_x86_64_pe_cl_MD_O2_GS.obj (pe_chained_unwind.source.c), the fixture of
// D-LK-COFF-FOREIGN-UNWIND-DROP and D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION.
// No guard enforces an md5: it records WHICH BYTES were measured. No leg has a clang-cl (the batch that built the
// cl object found none on windows-x86_64-release), so cl is the MSVC family's measured producer here.
//
// And the rows' own census objects, lane xa's `win64_unwind_forms_x86_64_pe_{cl,clangcl,gcc}_O2.obj` (READ here;
// win64_unwind_forms.source.c states how each was built): clang-cl 19.1.5's names `__chkstk`, `__security_cookie`,
// `__security_check_cookie` and `_fltused`, cl's the GS handlers besides, gcc's `___chkstk_ms`.
//
// Every TEST but the `…Native` ones runs in-process on every leg: the images are linked, never run, there.

#include "core/substrate/path_identity.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"

#include "host_native_target.hpp"
#include "image_dependency_table.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kExec = "x86_64:pe64-x86_64-windows-exec";
constexpr char const* kDll  = "x86_64:pe64-x86_64-windows-dll";
// cl's cells: {/O2, /Od} x {/GS, /GS-}. A /GS- object names no routine of the /GS family, and an overrun in it is
// undefined behaviour, so its modes are `o` and DSS's own `c` alone.
struct ClCell { char const* object; bool gs; };
constexpr ClCell kClCells[] = {
    {"pe_compiler_support_x86_64_pe_cl_MD_O2_GS.obj", true},
    {"pe_compiler_support_x86_64_pe_cl_MD_Od_GS.obj", true},
    {"pe_compiler_support_x86_64_pe_cl_MD_O2_GSoff.obj", false},
    {"pe_compiler_support_x86_64_pe_cl_MD_Od_GSoff.obj", false},
};
constexpr char const* kGccObjects[] = {"pe_gcc_runtime_x86_64_pe_gcc_O0_fexceptions.o",
                                       "pe_gcc_runtime_x86_64_pe_gcc_O2_fexceptions.o"};
constexpr char const* kSspObjects[] = {"pe_gcc_stack_protector_x86_64_pe_gcc_O0_ssp.o",
                                       "pe_gcc_stack_protector_x86_64_pe_gcc_O2_ssp.o"};
struct FormsObject { char const* name; bool takesAFault; };
// cl's C `__try` takes a hardware fault; clang-cl's drops a `__try` around no call (without /EHa), and gcc has none.
constexpr FormsObject kFormsObjects[] = {
    {"win64_unwind_forms_x86_64_pe_cl_O2.obj", true},
    {"win64_unwind_forms_x86_64_pe_clangcl_O2.obj", false},
    {"win64_unwind_forms_x86_64_pe_gcc_O2.obj", false},
};

// The DSS program beside the cl object: the reference's own main (`DSS_FIXTURE_REFERENCE_MAIN` in
// pe_compiler_support.source.c), compiled by DSS, and one more mode, `c`, the cookie's seeding.
constexpr char const* kClMain = R"(#include <stdint.h>
#include <windows.h>
typedef void (*thrower)(void);
int cs_big(int); int cs_guarded(char const *); int cs_ranged(char const *); int cs_overrun(char const *, int);
int cs_float(int); int cs_guarded_through(char const *, thrower); int cs_corrupt_through(char const *, int, thrower);
int cs_trygs(char const *, thrower); int cs_alloca_through(int, char const *, thrower);
extern uintptr_t __security_cookie;
void __security_init_cookie(void);
static void raise_through(void) { RaiseException(0xE0000002u, 0, 0, 0); }
static void quiet(void) {}
int main(int argc, char **argv) {
    char m = argc > 1 ? argv[1][0] : 'o';
    if (m == 'v') return cs_overrun("0123456789abcdef0123456789abcdef0123456789", 40) == 0 ? 1 : 2;
    if (m == 'r') return cs_ranged("0123456789012345678901234567890123456789012345678901234567890123") == 0 ? 3 : 4;
    if (m == 'x') {
        __try {
            cs_corrupt_through("0123456789abcdef0123456789abcdef0123456789", 40, raise_through);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 8;
        }
        return 9;
    }
    if (m == 'c') {
        uintptr_t const shipped = (uintptr_t)0x00002B992DDFA232ull;
        uintptr_t const before  = __security_cookie;
        __security_init_cookie();
        uintptr_t const after = __security_cookie;
        __security_init_cookie();
        return after != shipped && (after >> 48) == 0 && __security_cookie == after
                       && (before == shipped || before == after) ? 42 : 6;
    }
    int ok = cs_big(3) == 6 && cs_guarded("abc") == 'a' + 'b' + 'c' && cs_ranged("ab") == 'a' + 'b'
          && cs_float(4) == 6 && cs_trygs("ab", raise_through) == 'a' + 'b' + 100 && cs_trygs("ab", quiet) == 'a' + 'b';
    int through = 0;
    __try {
        cs_guarded_through("ab", raise_through);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        through += 1;
    }
    __try {
        cs_alloca_through(100, "ab", raise_through);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        through += 1;
    }
    return ok && through == 2 ? 42 : 5;
}
)";

// The DSS program beside a census object (`DSS_FORMS_TRY` defined for cl's).
constexpr char const* kFormsMain = R"(void sink(char *b);
double dsink(double x);
int    leaf(int x);
int    gsbuf(int n);
int    big(int n);
int    dyn(int n);
double regs(double a, double b, double c, int *p);
int    shrink(int *p);
#ifdef DSS_FORMS_TRY
int trygs(int volatile *p, int n);
#endif

static int filled;
void sink(char *b) {
    for (int i = 0; i < 64; ++i) b[i] = (char)(i + 1);
    ++filled;
}
double dsink(double x) { return x; }

int main(void) {
    int p[16] = {1, 2, 3, 4, 5, 6};
    int seven = 7;
    int ok = leaf(4) == 13 && gsbuf(5) == 6 && big(5) == 6 && dyn(64) == 1 && shrink(&seven) == 8
          && shrink(0) == 0 && regs(1.0, 2.0, 3.0, p) == 41.0 && filled == 5;
#ifdef DSS_FORMS_TRY
    int volatile one = 1;
    ok = ok && trygs(&one, 2) == 1 + 3 && trygs(0, 2) == -1 && filled == 7;
#endif
    return ok ? 42 : 5;
}
)";

// The DLL beside a cl object: one exported function (a DSS DLL exports its external functions); the object is linked
// whole, so every routine it names must be defined in the DLL as in the EXE.
constexpr char const* kClDll = "int cs_big(int); int cs_float(int);\nint dll_api(void) { return cs_big(3) + cs_float(4); }\n";

// The DLL beside a census object: the two helpers the object calls, and one exported function.
constexpr char const* kFormsDll = R"(void sink(char *b);
double dsink(double x);
int    big(int n);
static int filled;
void sink(char *b) {
    for (int i = 0; i < 64; ++i) b[i] = (char)(i + 1);
    ++filled;
}
double dsink(double x) { return x; }
int dll_api(void) { return big(5) + filled; }
)";

// The DSS program beside a gcc stack-protector object: the reference's own main (`DSS_FIXTURE_REFERENCE_MAIN` in
// pe_gcc_stack_protector.source.c), compiled by DSS, its `g` mode DSS's own: the guard's seeding by the ONE routine
// the compilerSupport block names, `__security_init_cookie` -- not its shipped value, not zero, and once.
constexpr char const* kSspMain = R"(#include <stdint.h>
int gs_guarded(char const *s, int n);
extern uintptr_t __stack_chk_guard;
void __security_init_cookie(void);
int main(int argc, char **argv) {
    char m = argc > 1 ? argv[1][0] : 'o';
    if (m == 's') return gs_guarded("0123456789abcdef0123456789abcdef", 32) == 0 ? 1 : 2;
    if (m == 'g') {
        uintptr_t const shipped = (uintptr_t)0x595E9FBD94FDA766ull;
        uintptr_t const before  = __stack_chk_guard;
        __security_init_cookie();
        uintptr_t const after = __stack_chk_guard;
        __security_init_cookie();
        return after != shipped && after != 0 && __stack_chk_guard == after
                       && (before == shipped || before == after) ? 42 : 6;
    }
    return gs_guarded("0123456789abcdef", 16) == 1122 ? 42 : 5;
}
)";
constexpr char const* kSspDll = "int gs_guarded(char const *s, int n);\nint dll_api(void) { return gs_guarded(\"ab\", 2); }\n";

// The gcc program is linked from its object ALONE -- an all-object link, whose client module is the object and whose
// entry is gcc's own `main`, the one that calls `__main`. A DSS source beside it holding no entry is the shape
// D-PROGRAM-ENTRY-IN-AN-OBJECT-INPUT-BESIDE-A-SOURCE-IS-UNDEFINED refuses (MEASURED here: `K_ProgramEntryUndefined`),
// and a DSS `main` would replace the very `main` that names `__main`.
constexpr std::string_view kNoSource{};

[[nodiscard]] fs::path fixturePath(std::string_view name) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{dss::core::extendedLengthSpelling(path), std::ios::binary};   // at any length
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

// The DSS program that calls `__GSHandlerCheck_SEH` AS THE EXCEPTION DISPATCHER CALLS IT -- a DISPATCHER_CONTEXT, a
// scope table of one scope, the GS data after it -- in each of the 8 cells {dispatching, unwinding} x {the GS data's
// flag for that phase, the other one} x {the cookie intact, overwritten} (the mode's three letters). It is the pin
// the review of H5 asked for (F1), on the routine itself: the cl objects whose frames would reach the same cells
// (`__try/__finally`, an expression filter) carry their funclets in ASSOCIATIVE code COMDATs, which the object reader
// refuses today (D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION, its trigger fired by this measurement); their end-to-end
// cells join the GS row's universe when that row lands. The SAME source is h6's MSVC measurement (`DSS_REF_CELL`
// picks the cell there, and argv here).
constexpr char const* kGsSehMain = R"(#include <stdint.h>
#include <windows.h>

EXCEPTION_DISPOSITION __GSHandlerCheck_SEH(EXCEPTION_RECORD *record, void *establisherFrame, PCONTEXT context,
                                           DISPATCHER_CONTEXT *dispatch);
extern uintptr_t __security_cookie;

static int volatile g_ran;
static LONG scope_filter(EXCEPTION_POINTERS *pointers, void *frame) {
    (void)pointers;
    (void)frame;
    g_ran = 1;
    return EXCEPTION_CONTINUE_SEARCH;
}
static void scope_finally(BYTE abnormal, void *frame) {   /* winnt.h: typedef BYTE BOOLEAN */
    (void)abnormal;
    (void)frame;
    g_ran = 1;
}
static void covered(void) {}
static unsigned char const unwind_info[4] = {1, 0, 0, 0};   /* version 1, no frame register */
static char const *const kCells[] = {"deg", "dug", "deb", "dub", "uug", "ueg", "uub", "ueb"};

int main(int argc, char **argv) {
#ifdef DSS_REF_CELL
    char const *m = kCells[DSS_REF_CELL];
#else
    char const *m = argc > 1 ? argv[1] : kCells[0];
#endif
    int const       unwinding   = m[0] == 'u';
    ULONG const     flag        = m[1] == 'e' ? 1u : 2u;
    int const       overwritten = m[2] == 'b';
    /* The base every RVA below is taken from: below the lowest of this image's addresses the tables name, at a
     * 64 KiB boundary -- the image's own base, for an image this small -- so no RVA is 0 or 1 (a scope's filter
     * RVA of 1 means EXCEPTION_EXECUTE_HANDLER), and the dispatcher context hands the handler the same base. */
    ULONG_PTR lowest = (ULONG_PTR)covered;
    if ((ULONG_PTR)unwind_info < lowest) lowest = (ULONG_PTR)unwind_info;
    if ((ULONG_PTR)scope_filter < lowest) lowest = (ULONG_PTR)scope_filter;
    if ((ULONG_PTR)scope_finally < lowest) lowest = (ULONG_PTR)scope_finally;
    ULONG_PTR const base = (lowest - 16u) & ~(ULONG_PTR)0xFFFFu;
    RUNTIME_FUNCTION entry;
    entry.BeginAddress = 0;
    entry.EndAddress   = 0;
    entry.UnwindData   = (ULONG)((ULONG_PTR)unwind_info - base);
    ULONG data[6];
    data[0] = 1;
    data[1] = (ULONG)((ULONG_PTR)covered - base);
    data[2] = data[1] + 1;
    data[3] = (ULONG)((unwinding ? (ULONG_PTR)scope_finally : (ULONG_PTR)scope_filter) - base);
    data[4] = unwinding ? 0u : data[1];   /* a termination handler has no jump target */
    data[5] = 8u | flag;
    uintptr_t       frame[4]    = {0, 0, 0, 0};
    ULONG_PTR const establisher = (ULONG_PTR)frame;
    frame[1] = __security_cookie ^ establisher;
    if (overwritten) frame[1] ^= 1u;
    EXCEPTION_RECORD record = {0};
    record.ExceptionCode  = 0xE0000002u;
    record.ExceptionFlags = unwinding ? EXCEPTION_UNWINDING : 0u;
    /* A CONTEXT -- x64: 1232 bytes, 16-aligned, Rsp at 0x98 (winnt.h); DSS's windows.h declares its pointer type
     * alone -- its Rsp a stack address: the context a dispatcher hands is the thread's. */
    DWORD64        context_words[156] = {0};
    PCONTEXT const context            = (PCONTEXT)(((ULONG_PTR)context_words + 15u) & ~(ULONG_PTR)15u);
    ((DWORD64 *)context)[0x98 / 8]    = (DWORD64)establisher;
    DISPATCHER_CONTEXT dispatch = {0};
    dispatch.ControlPc        = (ULONG_PTR)covered;
    dispatch.ImageBase        = base;
    dispatch.FunctionEntry    = &entry;
    dispatch.EstablisherFrame = establisher;
    dispatch.ContextRecord    = context;
    dispatch.HandlerData      = data;
    EXCEPTION_DISPOSITION const d = __GSHandlerCheck_SEH(&record, (void *)establisher, context, &dispatch);
    return d == ExceptionContinueSearch ? (g_ran ? 42 : 43) : 44;
}
)";

// DSS's image of `source` (a DSS unit; none when empty) beside `object` (none when empty), for `target` (an EXE
// unless a DLL is asked); the image's path, or empty with `rep` holding why.
[[nodiscard]] fs::path dssImageOf(fs::path const& dir, std::string_view source, fs::path const& object,
                                  DiagnosticReporter& rep, char const* target = kExec) {
    // At any length, as DSS itself writes (P69 H6): the substrate's operations and the extended-length spelling.
    std::error_code made;
    static_cast<void>(dss::core::createDirectories(dir, made));
    auto const out = dir / "dss.out";
    static_cast<void>(dss::core::createDirectories(out, made));
    Program p;
    p.setOutputDir(out);
    std::vector<std::string> inputs;
    if (!source.empty()) {
        auto const unit = dir / "main.c";
        std::ofstream(dss::core::extendedLengthSpelling(unit), std::ios::binary) << source;
        inputs.push_back(unit.string());
    }
    if (!object.empty()) inputs.push_back(object.string());
    if (p.compileFiles(inputs, "c", std::vector<std::string>{target}, rep) != 0) return {};
    std::string const want = std::string_view{target} == kDll ? ".dll" : ".exe";
    fs::path          image;
    static_cast<void>(dss::core::walkTree(out, [&](dss::core::TreeEntry const& e) {
        if (e.kind == dss::core::EntryKind::File && e.path.extension() == want) image = e.path;
        return image.empty() ? dss::core::WalkStep::Prune : dss::core::WalkStep::Stop;
    }));
    return image;
}

// Every routine of the platform runtime a reference object may name, in the order the rows list them: none may be
// IMPORTED by an image DSS links -- each is defined in it, or not named at all.
std::vector<std::string> const kEveryRoutine = {
    "__chkstk", "___chkstk_ms", "__security_cookie", "__security_check_cookie", "__security_init_cookie",
    "__GSHandlerCheck", "__GSHandlerCheck_SEH", "__report_gsfailure", "__report_rangecheckfailure", "_fltused",
    "__main", "__gcc_personality_seh0", "_Unwind_Resume", "__stack_chk_guard", "__stack_chk_fail"};

// None of `names` is IMPORTED: the shipped runtime archive defined each in the image.
void expectNoneImported(fs::path const& image, std::vector<std::string> const& names) {
    auto const imported = test_support::peImportedSymbols(bytesOf(image));
    for (auto const& n : names) {
        EXPECT_EQ(std::find(imported.begin(), imported.end(), n), imported.end())
            << n << " is imported; it must be defined in the image by the compilerSupport runtime";
    }
}

[[nodiscard]] bool onWindowsX64() { return test_support::hostNativeTarget().execTarget == kExec; }

constexpr std::uint32_t kStackBufferOverrun = 0xC0000409u;  // cl's own runtime's status for both reports (MEASURED)

}  // namespace

// ══ THE LINK, ON EVERY LEG ═══════════════════════════════════════════════════════════════════════════════════════

// A cl object with a frame past a page, a range check, floating point and frames with and without __try -- each cell
// of {/O2, /Od} x {/GS, /GS-} -- linked by DSS into an EXE and into a DLL: every routine it names is DEFINED in the
// image, by the runtime archive -- none is imported, none undefined.
TEST(PeCompilerSupportRuntime, AClObjectLinksWithEveryRoutineDefined) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-link"};
    for (auto const& cell : kClCells) {
        for (char const* target : {kExec, kDll}) {
            SCOPED_TRACE(std::string{cell.object} + " as " + target);
            DiagnosticReporter rep;
            auto const image = dssImageOf(scratch.path() / cell.object / (target == kDll ? "dll" : "exe"),
                                          target == kDll ? kClDll : kClMain, fixturePath(cell.object), rep, target);
            ASSERT_FALSE(image.empty()) << "DSS refused the link:" << diagnosticsOf(rep);
            expectNoneImported(image, kEveryRoutine);
        }
    }
}

// A mingw-w64 gcc object whose `main` calls `__main`, whose frame passes a page (`___chkstk_ms`) and whose
// `-fexceptions` cleanup frame names gcc's personality and `_Unwind_Resume` -- at -O0 and at -O2 -- linked by DSS,
// alone, into an EXE and into a DLL: each is defined.
TEST(PeCompilerSupportRuntime, AGccObjectsMainHookProbeAndUnwinderEntriesAreDefined) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-gcc-link"};
    for (char const* object : kGccObjects) {
        for (char const* target : {kExec, kDll}) {
            SCOPED_TRACE(std::string{object} + " as " + target);
            DiagnosticReporter rep;
            auto const image = dssImageOf(scratch.path() / object / (target == kDll ? "dll" : "exe"), kNoSource,
                                          fixturePath(object), rep, target);
            ASSERT_FALSE(image.empty()) << "DSS refused the link:" << diagnosticsOf(rep);
            expectNoneImported(image, kEveryRoutine);
        }
    }
}

// A mingw-w64 gcc -fstack-protector-strong object -- at -O0 and at -O2 -- linked by DSS into an EXE and into a DLL:
// the guard and its reporter are defined in the image (gcc's own link imports the reporter from libssp-0.dll, an
// image a plain Windows does not have).
TEST(PeCompilerSupportRuntime, AGccStackProtectorObjectLinksWithItsGuardAndReporterDefined) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-ssp-link"};
    for (char const* object : kSspObjects) {
        for (char const* target : {kExec, kDll}) {
            SCOPED_TRACE(std::string{object} + " as " + target);
            DiagnosticReporter rep;
            auto const image = dssImageOf(scratch.path() / object / (target == kDll ? "dll" : "exe"),
                                          target == kDll ? kSspDll : kSspMain, fixturePath(object), rep, target);
            ASSERT_FALSE(image.empty()) << "DSS refused the link:" << diagnosticsOf(rep);
            expectNoneImported(image, kEveryRoutine);
        }
    }
}

// The program that calls `__GSHandlerCheck_SEH` as the dispatcher does links into an EXE with the routine and the
// cookie DEFINED in the image, by the runtime archive.
TEST(PeCompilerSupportRuntime, TheSehCookieHandlerProgramLinksWithItsRoutineDefined) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-gs-seh-link"};
    DiagnosticReporter       rep;
    auto const image = dssImageOf(scratch.path() / "exe", kGsSehMain, fs::path{}, rep);
    ASSERT_FALSE(image.empty()) << "DSS refused the link:" << diagnosticsOf(rep);
    expectNoneImported(image, kEveryRoutine);
}

// ══ NATIVE: each image runs as the reference's does ══════════════════════════════════════════════════════════════

// Windows x86_64: `__GSHandlerCheck_SEH` checks the frame's cookie WHENEVER an exception passes through the frame,
// in either phase, and calls the scope table's handler only in the phase the GS data's flag names -- the frame lets
// the exception go on in the other. Each cell ends as MSVC's own routine ends it on the same program (✔MEASURED
// 2026-11-04, probe-reference-cc runs 20261104-211814-aecd751e and 20261104-211838-134b9ebf, windows-x86_64-release,
// cl 19.51.36260 /O2 /GS /MD, `DSS_REF_CELL` 0..7): deg 42, dug 43, deb and dub 0xC0000409, uug 42, ueg 43, uub and
// ueb 0xC0000409 -- and the same eight from the program as it stands, which asks only what DSS's windows.h declares
// (runs 20261105-000246-081e1c80 and 20261105-000254-abe24ffe; the first text asked GetModuleHandleW, BOOLEAN and
// CONTEXT, which no non-Windows host's DSS compiles, ✔MEASURED on macos-arm64-debug). The first cut checked the cookie only in the flagged phase and called the scope handler in every
// phase: dug 42, dub 42, ueg 42, ueb 42 -- the four cells the arm turns red.
TEST(PeCompilerSupportRuntimeNative, TheSehCookieHandlerChecksEveryPhaseAndCallsTheScopeOnlyInItsOwn) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-gs-seh-run"};
    DiagnosticReporter       rep;
    auto const image = dssImageOf(scratch.path() / "exe", kGsSehMain, fs::path{}, rep);
    ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
    struct Cell { char const* mode; std::uint32_t exit; };
    Cell const cells[] = {{"deg", 42u}, {"dug", 43u}, {"deb", kStackBufferOverrun}, {"dub", kStackBufferOverrun},
                          {"uug", 42u}, {"ueg", 43u}, {"uub", kStackBufferOverrun}, {"ueb", kStackBufferOverrun}};
    for (Cell const& c : cells) {
        SCOPED_TRACE(c.mode);
        auto const r = test_support::runBinary(image, test_support::kRunBudget, false, {}, {c.mode});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, c.exit) << "42 = the scope's handler ran, 43 = it did not, 0xC0000409 = the cookie check";
    }
}

// Windows x86_64: the cl program DSS links runs every mode to the exit cl's own link of the same source gives
// (MEASURED, probe-reference-cc run 20261029-031937-7c816e30, windows-x86_64-release, cl 19.51.36260; `c` is DSS's own, the shipped runtime's seeding):
//   o  42           every routine runs: the probe, the cookie pair, the range check in range, floating point, a /GS
//                   frame an exception passes through (its handler checks the cookie), one with __try, one framed
//   v  0xC0000409   a cookie overwritten: the epilogue's check fast-fails
//   r  0xC0000409   an index past the array: the range check fast-fails
//   x  0xC0000409   a cookie overwritten in a frame an exception passes through: its handler's check fast-fails
//   c  42           `__security_init_cookie` seeds a 48-bit cookie that is not the shipped one, once
// Every cell of {/O2, /Od} x {/GS, /GS-} likewise (MEASURED, probe-reference-cc run 20261103-192148-62c96676, from the
// final recipe): /Od /GS ends o 42 and v r x 0xC0000409 as /O2 /GS does; a /GS- cell ends o 42, and its v r x are
// undefined behaviour with no reference result, so they are not asked.
TEST(PeCompilerSupportRuntimeNative, EachClModeEndsAsTheReferencesDoes) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-run"};
    struct Mode { char const* arg; std::uint32_t exit; };
    Mode const gsModes[] = {{"o", 42u}, {"v", kStackBufferOverrun}, {"r", kStackBufferOverrun},
                            {"x", kStackBufferOverrun}, {"c", 42u}};
    Mode const noGsModes[] = {{"o", 42u}, {"c", 42u}};
    for (auto const& cell : kClCells) {
        SCOPED_TRACE(cell.object);
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / cell.object, kClMain, fixturePath(cell.object), rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        for (auto const& m : cell.gs ? std::vector<Mode>(std::begin(gsModes), std::end(gsModes))
                                     : std::vector<Mode>(std::begin(noGsModes), std::end(noGsModes))) {
            SCOPED_TRACE(m.arg);
            auto const r = test_support::runBinary(image, test_support::kRunBudget, false, {}, {m.arg});
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, m.exit);
        }
    }
}

// Windows x86_64: the gcc program DSS links runs to 42, as gcc's own link does (MEASURED, probe-reference-cc run 20261029-021706-85876fe9, windows-x86_64-debug, MinGW-W64 gcc 13.2.0 x86_64-ucrt-posix-seh, at -O0 and at -O2): the normal
// path runs the cleanup once and then the code after the scope (C H); a FOREIGN exception reaches the filter and the
// scope's landing point and the cleanup does NOT run (F H) -- libgcc passes over an exception it did not raise
// (MEASURED, probe run 20261029-012510-b6ca0483), and DSS's personality does the same; the big frame computes its
// value. An exception in libgcc's OWN protocol is refused loud: DSS ships no unwinder for it, so the process ends,
// saying why, by abort(), rather than skip the cleanups a libgcc personality would run. ucrtbase's abort() ends a
// process with a fast fail, 0xC0000409, after its message (MEASURED, probe-reference-cc run
// 20261029-031937-7c816e30, windows-x86_64-release: cl 19.51 /O2 /MD's abort() after a line on stderr).
TEST(PeCompilerSupportRuntimeNative, AGccProgramPassesOverAForeignExceptionAndRefusesGccsOwn) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-gcc-run"};
    for (char const* object : kGccObjects) {
        SCOPED_TRACE(object);
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / object, kNoSource, fixturePath(object), rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "11 = the normal path did not run C H; 21 = the foreign exception did not run F H";
        auto const g = test_support::runBinary(image, test_support::kRunBudget, true, {}, {"g"});
        ASSERT_TRUE(g.spawned) << g.diagnostic;
        EXPECT_EQ(g.exitCode, kStackBufferOverrun)
            << "a gcc-protocol exception must end the process by abort(), ucrtbase's fast fail: " << g.capturedStdout;
        EXPECT_NE(g.capturedStdout.find("DSS runtime: a gcc exception reached __gcc_personality_seh0"),
                  std::string::npos)
            << g.capturedStdout;
    }
}

// Windows x86_64: each gcc stack-protector program DSS links (-O0 and -O2) runs as decided (MEASURED for the
// reference, probe-reference-cc run 20261103-192258-21a07b3f, MinGW-W64 gcc 13.2.0 from the final recipe):
//   o  42           in bounds: gcc's own link gives 42 too
//   s  0xC0000409   32 bytes into the 16-byte array, the canary moved: DSS's `__stack_chk_fail` says "*** stack
//                   smashing detected ***: terminated" on the standard error and is the fast fail /GS's report is --
//                   what libmingwex's own reporter does (MEASURED, run 20261104-235014-6f1093e3) and what GCC's manual
//                   says a failed guard check does. gcc's default link (libssp's) ends 0xC000001D, a trap, saying
//                   nothing, at -O0 and -O2 -- an exception a caller's `__try` could catch; the fast fail runs no
//                   handler. In o and g nothing is said.
//   g  42           `__security_init_cookie`, the ONE seeding routine, replaces the shipped guard, once (gcc's library
//                   seeds its own per run: three runs, three values)
TEST(PeCompilerSupportRuntimeNative, EachGccStackProtectorModeEndsAsDecided) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-ssp-run"};
    struct Mode { char const* arg; std::uint32_t exit; };
    Mode const modes[] = {{"o", 42u}, {"s", kStackBufferOverrun}, {"g", 42u}};
    for (char const* object : kSspObjects) {
        SCOPED_TRACE(object);
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / object, kSspMain, fixturePath(object), rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        for (auto const& m : modes) {
            SCOPED_TRACE(m.arg);
            auto const r = test_support::runBinary(image, test_support::kRunBudget, true, {}, {m.arg});
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, m.exit) << "6 = the guard was not seeded once; 5 = a value was wrong";
            bool const said = r.capturedStdout.find("*** stack smashing detected ***: terminated") != std::string::npos;
            EXPECT_EQ(said, m.exit == kStackBufferOverrun)
                << "the moved canary must be reported on the standard error, and nothing else said; the run printed: "
                << r.capturedStdout;
        }
    }
}

// ══ THE ROWS' CENSUS OBJECTS ═════════════════════════════════════════════════════════════════════════════════════

// Each census object -- a frame past a page, an `_alloca` frame, /GS buffers (cl and clang-cl), floating point --
// links under DSS, into an EXE and into a DLL, with every routine it names defined in the image.
TEST(PeCompilerSupportRuntime, EachCensusObjectLinksWithItsRoutinesDefined) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-forms-link"};
    for (auto const& o : kFormsObjects) {
        for (char const* target : {kExec, kDll}) {
            SCOPED_TRACE(std::string{o.name} + " as " + target);
            DiagnosticReporter rep;
            std::string const  source = target == kDll ? std::string{kFormsDll}
                                      : std::string{o.takesAFault ? "#define DSS_FORMS_TRY 1\n" : ""} + kFormsMain;
            auto const image = dssImageOf(scratch.path() / o.name / (target == kDll ? "dll" : "exe"), source,
                                          fixturePath(o.name), rep, target);
            ASSERT_FALSE(image.empty()) << "DSS refused the link:" << diagnosticsOf(rep);
            expectNoneImported(image, kEveryRoutine);
        }
    }
}

// Windows x86_64: each census program DSS links runs to 42, as cl's and gcc's own links of the same source and main
// do (MEASURED, probe-reference-cc run 20261029-022956-2f1e5e64, windows-x86_64-release, cl 19.51.36260 /O2; run 20261029-023017-72734ec6, windows-x86_64-debug, MinGW-W64 gcc 13.2.0 -O2); clang-cl's own link is measured nowhere -- no leg has one --
// and its 42 is the source's C value, the same program.
TEST(PeCompilerSupportRuntimeNative, EachCensusProgramRunsAsItsReferenceDoes) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-compiler-support-forms-run"};
    for (auto const& o : kFormsObjects) {
        SCOPED_TRACE(o.name);
        DiagnosticReporter rep;
        std::string const  source = std::string{o.takesAFault ? "#define DSS_FORMS_TRY 1\n" : ""} + kFormsMain;
        auto const image = dssImageOf(scratch.path() / o.name, source, fixturePath(o.name), rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "5 = a value the program computes was wrong";
    }
}
