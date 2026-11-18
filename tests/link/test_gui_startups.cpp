// P69 fold 3f, lane `xa` (D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE item (6)): THE GUI STARTUPS. A C program
// whose entry is `WinMain` or `wWinMain` is a program on pe64, started with the four values the C runtime's GUI
// startups (WinMainCRTStartup, wWinMainCRTStartup) hand it.
//
// ✔MEASURED 2026-11-17 (lane xa, probe-reference-cc; cl 19.52 + link.exe 14.52 /O2 /MD, MinGW-w64 gcc 13.2 + GNU ld
// 2.42 -- wWinMain with -municode; every image run with no argument):
//   * inside WinMain and wWinMain the instance is the image's own module handle (== GetModuleHandleW(NULL)), the
//     previous instance is null, the command line is the one past the program's name, and the show value is the one
//     STARTUPINFOW states when its flags carry STARTF_USESHOWWINDOW, else SW_SHOWDEFAULT (10); `__argc` is 1 -- the
//     startup configured the argument vector too. STARTUPINFOW: 104 bytes, `dwFlags` at 60, `wShowWindow` at 64.
//   * MSVC's GUI startup imports `_configure_{narrow,wide}_argv`, `__p___argc`, `__p___{w,}argv` and
//     `_get_{narrow,wide}_winmain_command_line`; the UCRT answers the command line in an image with NO C runtime
//     startup (an image entered at a plain function, DSS's situation): both empty past the name.
//   * a program defining several entries starts at `main` before `wmain` before `WinMain` before `wWinMain`
//     (link.exe in every pairing; gcc where it runs the wide entry) -- the pe64 exec document's `entryPrecedence`.
//   * on ELF (gcc 13.3, clang 18.1) and Mach-O (Apple clang 21) a WinMain alone is no entry: "undefined reference to
//     `main'" / "Undefined symbols ... _main".
// DSS's startup computes the same four: the instance and the show value through its compilerSupport runtime's
// routines (runtime/platform/src/pe_gui_startup.c), the command line through the UCRT's own routine, the previous
// instance a null pointer. Every cell but the native TESTs links in-process on every leg; the native ones run each
// image where the host is Windows x86_64.

#include "core/types/entry_param_types.hpp"
#include "core/types/entry_shape.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/unit_linker_requests.hpp"
#include "link/format/coff_linker_directives.hpp"
#include "link/object_format_schema.hpp"
#include "object_entry_support.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace dss;
using namespace dss::test_support::object_entry;

namespace {

constexpr char const* kPe = "x86_64:pe64-x86_64-windows-exec";

// WinMain's four values read back as the exit code: 100 + the show value when the instance is this image's base,
// the previous instance is null and the command line past the name is "alpha beta" (the arguments every native cell
// passes); else the facts that held, as bits (1 instance, 2 previous, 4 command line).
constexpr char const* kFactsPrelude =
    "#include <windows.h>\n"
    "static int anchor;\n"
    "/* The instance is this image's base: its DOS and PE headers, and this image's own datum inside it. */\n"
    "static int isThisImage(HINSTANCE h) {\n"
    "    unsigned char const *b = (unsigned char const *)h;\n"
    "    if (b == 0 || b[0] != 'M' || b[1] != 'Z') return 0;\n"
    "    unsigned char const *pe = b + *(unsigned int const *)(b + 60);\n"
    "    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) return 0;\n"
    "    unsigned int const sizeOfImage = *(unsigned int const *)(pe + 24 + 56);\n"
    "    unsigned char const *self = (unsigned char const *)&anchor;\n"
    "    return self > b && self < b + sizeOfImage;\n"
    "}\n"
    "static int facts(HINSTANCE h, HINSTANCE p, int commandLine, int s) {\n"
    "    int const f = isThisImage(h) + 2 * (p == 0) + 4 * commandLine;\n"
    "    return f == 7 ? 100 + s : f;\n"
    "}\n";

constexpr char const* kNarrowEntry =
    "static int sameText(char const *got, char const *want) {\n"
    "    while (*want != 0) { if (*got++ != *want++) return 0; }\n"
    "    return *got == 0;\n"
    "}\n"
    "int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) {\n"
    "    return facts(h, p, sameText(c, \"alpha beta\"), s);\n"
    "}\n";

constexpr char const* kWideEntry =
    "static int sameWide(unsigned short const *got, char const *want) {\n"
    "    while (*want != 0) { if (*got++ != (unsigned char)*want++) return 0; }\n"
    "    return *got == 0;\n"
    "}\n"
    "int APIENTRY wWinMain(HINSTANCE h, HINSTANCE p, LPWSTR c, int s) {\n"
    "    return facts(h, p, sameWide(c, \"alpha beta\"), s);\n"
    "}\n";

[[nodiscard]] std::string narrowProgram() { return std::string{kFactsPrelude} + kNarrowEntry; }
[[nodiscard]] std::string wideProgram() { return std::string{kFactsPrelude} + kWideEntry; }

// The PE image's imports of `symbol`: which values the image's init fetches, and from where.
[[nodiscard]] std::size_t importsOf(fs::path const& image, char const* symbol) {
    return test_support::dependencyOccurrences(test_support::peImportedSymbols(bytesOf(image)), symbol);
}

// One unit holding several entries, each returning its own number, for the precedence cells.
struct Pairing {
    char const*   label;
    std::string   source;
    std::uint32_t exit;   // the entry link.exe runs (✔MEASURED, the header)
};
[[nodiscard]] std::vector<Pairing> pairings() {
    std::string const winMain =
        "int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) { (void)h; (void)p; (void)c; (void)s; "
        "return 44; }\n";
    std::string const wWinMain =
        "int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, LPWSTR c, int s) { (void)h; (void)p; (void)c; (void)s; "
        "return 55; }\n";
    std::string const narrowConsole = "int main(void) { return 11; }\n";
    std::string const wideConsole =
        "int wmain(int argc, unsigned short **argv) { (void)argc; (void)argv; return 22; }\n";
    std::string const head = "#include <windows.h>\n";
    return {{"main beside WinMain", head + narrowConsole + winMain, 11u},
            {"wmain beside WinMain", head + wideConsole + winMain, 22u},
            {"wmain beside wWinMain", head + wideConsole + wWinMain, 22u},
            {"WinMain beside wWinMain", head + winMain + wWinMain, 44u}};
}

}  // namespace

// ── THE SHIPPED DECLARATION ──────────────────────────────────────────────────

// The pe64 exec document realizes both GUI verbs, ranks them below the console ones, and names the routines that
// produce their values; the narrow and wide verbs hand the four slots the C runtime's startups hand.
TEST(GuiStartups, TheShippedPeExecRealizesBothGuiVerbs) {
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(r.has_value());
    auto const& f = **r;
    EXPECT_TRUE(f.realizesEntryVerb(EntryMaterialization::InstancePrevCmdlineShow));
    EXPECT_TRUE(f.realizesEntryVerb(EntryMaterialization::InstancePrevWcmdlineShow));
    using M = EntryMaterialization;
    auto const precedence = f.entryPrecedence();
    EXPECT_TRUE((std::vector<std::vector<M>>(precedence.begin(), precedence.end()))
                == (std::vector<std::vector<M>>{{M::None, M::ArgcArgv, M::ArgcArgvEnvp},
                                                {M::ArgcWargv, M::ArgcWargvWenvp},
                                                {M::InstancePrevCmdlineShow},
                                                {M::InstancePrevWcmdlineShow}}))
        << "the console startups rank first, then the narrow GUI one, then the wide one";
    auto const& pa = f.processArgs();
    ASSERT_TRUE(pa.has_value());
    EXPECT_EQ(pa->narrowWinMainCommandLineFn, "_get_narrow_winmain_command_line");
    EXPECT_EQ(pa->wideWinMainCommandLineFn, "_get_wide_winmain_command_line");
    ASSERT_TRUE(pa->winMainStartupRoutines.has_value());
    EXPECT_EQ(pa->winMainStartupRoutines->role, RuntimeLibraryRole::CompilerSupport);
    EXPECT_EQ(pa->winMainStartupRoutines->libraryPath, "") << "a realized role: the routines come from its archive";
    EXPECT_EQ(pa->winMainStartupRoutines->moduleInstanceFn, "__dss_pe_module_instance");
    EXPECT_EQ(pa->winMainStartupRoutines->showWindowFn, "__dss_pe_show_window_mode");
    for (auto const* name : {"elf64-x86_64-linux-exec", "elf64-aarch64-linux-exec", "macho64-x86_64-darwin-exec",
                             "macho64-arm64-darwin-exec"}) {
        SCOPED_TRACE(name);
        auto o = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(o.has_value());
        EXPECT_FALSE((*o)->realizesEntryVerb(EntryMaterialization::InstancePrevCmdlineShow));
        EXPECT_FALSE((*o)->realizesEntryVerb(EntryMaterialization::InstancePrevWcmdlineShow));
    }
}

// What each GUI verb hands, slot by slot -- the one table every reader derives from.
TEST(GuiStartups, EachGuiVerbHandsTheFourValuesOfItsWidth) {
    using V = EntryArgumentValue;
    using S = EntryParamShape;
    struct Want {
        EntryMaterialization  verb;
        std::array<S, 4>      shapes;
        std::array<V, 4>      values;
    };
    for (Want const& w :
         {Want{EntryMaterialization::InstancePrevCmdlineShow,
               {S::PtrOpaque, S::PtrOpaque, S::PtrChar, S::I32},
               {V::ModuleInstance, V::PreviousInstance, V::NarrowCommandLine, V::ShowWindow}},
          Want{EntryMaterialization::InstancePrevWcmdlineShow,
               {S::PtrOpaque, S::PtrOpaque, S::PtrU16, S::I32},
               {V::ModuleInstance, V::PreviousInstance, V::WideCommandLine, V::ShowWindow}}}) {
        SCOPED_TRACE(entryMaterializationName(w.verb));
        auto const slots = entryVerbSlots(w.verb);
        ASSERT_EQ(slots.size(), 4u);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            EXPECT_EQ(slots[i].shape, w.shapes[i]) << i;
            EXPECT_EQ(slots[i].value, w.values[i]) << i;
        }
        EXPECT_FALSE(entryVerbPassesEnvironment(w.verb)) << "four slots, and none an environment";
        EXPECT_FALSE(entryVerbPassesArgumentVector(w.verb));
        EXPECT_TRUE(entryVerbPassesWinMainArguments(w.verb));
    }
    EXPECT_FALSE(entryVerbIsWide(EntryMaterialization::InstancePrevCmdlineShow));
    EXPECT_TRUE(entryVerbIsWide(EntryMaterialization::InstancePrevWcmdlineShow));
}

// The semantic tier's classifier and the startup's builder are inverse functions of one table: every declarable
// shape's type classifies back to the shape, and the pointers no shape names classify to none.
TEST(GuiStartups, EveryDeclarableShapeRoundTripsThroughItsType) {
    TypeInterner in{CompilationUnitId{1}};
    for (auto const& [shape, name] : kEntryParamShapeTable.rows) {
        if (!isDeclarableEntryParamShape(shape)) continue;
        SCOPED_TRACE(std::string{name});
        TypeId const ty = typeOfEntryParamShape(in, shape);
        ASSERT_TRUE(ty.valid());
        EXPECT_EQ(classifyEntryParam(in, ty), shape);
    }
    TypeId const i32 = in.primitive(TypeKind::I32);
    std::array<TypeId, 1> const oneInt{i32};
    // A handle as <windows.h> spells it under STRICT (a pointer to a struct) and without (a pointer to void), a
    // pointer to a union, and `char *` with the language's own char kind: the shapes a WinMain's types take.
    EXPECT_EQ(classifyEntryParam(in, in.pointer(in.structType("HINSTANCE__", oneInt))), EntryParamShape::PtrOpaque);
    EXPECT_EQ(classifyEntryParam(in, in.pointer(in.unionType("U", oneInt))), EntryParamShape::PtrOpaque);
    EXPECT_EQ(classifyEntryParam(in, in.pointer(in.primitive(TypeKind::Char))), EntryParamShape::PtrChar);
    EXPECT_EQ(classifyEntryParam(in, in.pointer(in.pointer(in.primitive(TypeKind::Char)))),
              EntryParamShape::PtrPtrChar);
    // No shape: a pointer to an int, to a pointer to an int, to a function, and a 32-bit unit's pointer (a
    // 32-bit-wchar_t platform's `wchar_t *`).
    EXPECT_FALSE(classifyEntryParam(in, in.pointer(i32)).has_value());
    EXPECT_FALSE(classifyEntryParam(in, in.pointer(in.pointer(i32))).has_value());
    EXPECT_FALSE(classifyEntryParam(in, in.pointer(in.fnSig(oneInt, i32, CallConv::CcSysV))).has_value());
    EXPECT_FALSE(classifyEntryParam(in, in.pointer(in.primitive(TypeKind::U32))).has_value());
    EXPECT_FALSE(classifyEntryParam(in, in.primitive(TypeKind::I64)).has_value());
}

// ── THE PROGRAMS, BUILT ON EVERY LEG ─────────────────────────────────────────

// What the show-window routine reads is <windows.h>'s STARTUPINFOW, with the layout the references' own header gives
// it (✔MEASURED 2026-11-17, cl 19.52 and MinGW-w64 gcc 13.2: 104 bytes, `dwFlags` at 60, `wShowWindow` at 64) and
// the two constants it compares with -- each stated by DSS's <windows.h> (the windows descriptor) and asserted here
// by the language itself, so a row that drifts refuses the build.
TEST(GuiStartups, TheStartupInformationHasTheReferencesLayout) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "gui_startups_layout"};
    Built b;
    build(scratch.path(),
          {written("layout.c",
                   "#include <stddef.h>\n"
                   "#include <windows.h>\n"
                   "_Static_assert(sizeof(STARTUPINFOW) == 104, \"STARTUPINFOW is 104 bytes\");\n"
                   "_Static_assert(offsetof(STARTUPINFOW, dwFlags) == 60, \"dwFlags at 60\");\n"
                   "_Static_assert(offsetof(STARTUPINFOW, wShowWindow) == 64, \"wShowWindow at 64\");\n"
                   "_Static_assert(STARTF_USESHOWWINDOW == 1, \"STARTF_USESHOWWINDOW\");\n"
                   "_Static_assert(SW_SHOWDEFAULT == 10, \"SW_SHOWDEFAULT\");\n"
                   "int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) {\n"
                   "    (void)h; (void)p; (void)c; (void)s; return 0;\n}\n")},
          kPe, b);
    EXPECT_EQ(b.rc, 0) << b.diagnostics;
    EXPECT_FALSE(b.image.empty());
}

// A WinMain or wWinMain alone is the program's entry, declared as <windows.h> spells it (`WINAPI`, `APIENTRY`): the
// image's init configures the width's argument vector and reads the width's command line from the UCRT, and the
// compilerSupport runtime's routines join it, with the startup information their show routine reads.
TEST(GuiStartups, AGuiEntryAloneIsTheProgramsEntry) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "gui_startups_alone"};
    struct Case {
        char const*              label;
        std::string              source;
        std::vector<char const*> imported;
        std::vector<char const*> absent;
    };
    std::vector<Case> const cases{
        {"WinMain", narrowProgram(),
         {"_configure_narrow_argv", "__p___argv", "_get_narrow_winmain_command_line", "GetStartupInfoW"},
         {"__p___argc", "_configure_wide_argv", "__p___wargv", "_get_wide_winmain_command_line",
          "_initialize_narrow_environment", "_get_initial_narrow_environment"}},
        {"wWinMain", wideProgram(),
         {"_configure_wide_argv", "__p___wargv", "_get_wide_winmain_command_line", "GetStartupInfoW"},
         {"__p___argc", "_configure_narrow_argv", "__p___argv", "_get_narrow_winmain_command_line",
          "_initialize_wide_environment", "_get_initial_wide_environment"}},
        // STRICT's handle type: a pointer to a struct.
        {"WinMain of struct handles",
         "struct HINSTANCE__ { int unused; };\n"
         "int WinMain(struct HINSTANCE__ *h, struct HINSTANCE__ *p, char *c, int s) {\n"
         "    (void)h; (void)c; (void)s; return p == 0 ? 44 : 1;\n}\n",
         {"_get_narrow_winmain_command_line"},
         {"_get_wide_winmain_command_line"}},
    };
    int k = 0;
    for (auto const& c : cases) {
        SCOPED_TRACE(c.label);
        Built b;
        build(scratch.path() / std::to_string(k++), {written("gui.c", c.source)}, kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        ASSERT_FALSE(b.image.empty());
        EXPECT_FALSE(reported(b.rep, DiagnosticCode::S_EntryShapeNotDeclared)) << b.diagnostics;
        for (char const* name : c.imported) EXPECT_EQ(importsOf(b.image, name), 1u) << name;
        for (char const* name : c.absent) EXPECT_EQ(importsOf(b.image, name), 0u) << name;
    }
    // A console program pulls none of it: the GUI routines' unit is its own archive member.
    Built console;
    build(scratch.path() / "console", {written("main.c", "int main(void) { return 11; }\n")}, kPe, console);
    ASSERT_EQ(console.rc, 0) << console.diagnostics;
    for (char const* name : {"GetStartupInfoW", "_get_narrow_winmain_command_line", "_get_wide_winmain_command_line"}) {
        EXPECT_EQ(importsOf(console.image, name), 0u) << name;
    }
}

// Several entries in one program: the document's precedence elects one, and says so.
TEST(GuiStartups, AProgramWithSeveralEntriesStartsAtTheHighestRanked) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "gui_startups_precedence"};
    int k = 0;
    for (auto const& p : pairings()) {
        SCOPED_TRACE(p.label);
        Built b;
        build(scratch.path() / std::to_string(k++), {written("several.c", p.source)}, kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        EXPECT_TRUE(reported(b.rep, DiagnosticCode::K_ProgramEntryChosenByPrecedence)) << b.diagnostics;
        bool const gui = p.exit == 44u;
        EXPECT_EQ(importsOf(b.image, "_get_narrow_winmain_command_line"), gui ? 1u : 0u);
        EXPECT_EQ(importsOf(b.image, "_get_wide_winmain_command_line"), 0u) << "wWinMain never outranks";
    }
}

// On ELF and Mach-O no startup calls a WinMain: alone it is no entry, as both references' links refuse it.
TEST(GuiStartups, AWinMainIsNoEntryOffPe) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "gui_startups_off_pe"};
    int k = 0;
    for (auto const* target : {"x86_64:elf64-x86_64-linux-exec", "arm64:elf64-aarch64-linux-exec",
                               "x86_64:macho64-x86_64-darwin-exec", "arm64:macho64-arm64-darwin-exec"}) {
        SCOPED_TRACE(target);
        Built b;
        build(scratch.path() / std::to_string(k++),
              {written("gui.c", "int WinMain(void *h, void *p, char *c, int s) { (void)h; (void)p; (void)c; "
                                "(void)s; return 44; }\n")},
              target, b);
        EXPECT_NE(b.rc, 0);
        EXPECT_TRUE(b.image.empty());
        EXPECT_TRUE(reported(b.rep, DiagnosticCode::K_ProgramEntryUndefined)) << b.diagnostics;
    }
}

// ── A STATED SUBSYSTEM CHOOSES THE STARTUP (P69 fold 3f) ────────────────────
//
// ✔MEASURED 2026-11-17 (probe-reference-cc runs 20261117-092912-9c3db0d9 and 20261117-100011-e9df126e; cl 19.52 + link.exe 14.52 /MD; each image
// run with no argument, its exit naming the entry that ran -- main 11, wmain 22, WinMain 44, wWinMain 55): with no
// `/ENTRY:` stated, link.exe starts a program at the C runtime startups of the subsystem a unit states, ranked among
// themselves as with none stated -- /SUBSYSTEM:WINDOWS beside main + WinMain 44, wmain + WinMain 44, main + wWinMain
// 55, WinMain + wWinMain 44 (LNK4067); /SUBSYSTEM:CONSOLE beside main + WinMain 11; of two units stating two
// subsystems the LAST stands (WINDOWS then CONSOLE 11, CONSOLE then WINDOWS 44); and a stated
// /ENTRY:wmainCRTStartup beside /SUBSYSTEM:WINDOWS runs wmain (22) -- the subsystem stated in the entries' own unit
// (GS), in a unit of its own (GX, the fixtures' text), and as the link's option (GS). It REFUSES /SUBSYSTEM:WINDOWS
// beside `main` alone and /SUBSYSTEM:CONSOLE beside WinMain alone (LNK2019 `WinMain` / `main`), programs gcc runs
// (`main` under -mwindows; WinMain under its console default): DSS links both and starts the program's own entry.
// The fixtures (run 20261117-100011-e9df126e: each built twice, byte-identical, naming itself X:\dss-fixture\probe.obj and no host path):
// object_subsystem_windows_x86_64_pe_cl_MD_O2.obj, 618 bytes, md5 894dfa500621c36864a5874285a142aa, and
// object_subsystem_console_x86_64_pe_cl_MD_O2.obj, 618 bytes, md5 d6b7c08e45fda9db1060118fe47572a1; each `.drectve`
// reads `/SUBSYSTEM:WINDOWS` (or `CONSOLE`) `/DEFAULTLIB:"MSVCRT" /DEFAULTLIB:"OLDNAMES"`.

namespace {

// The cl objects that state the image's subsystem and nothing else of the program (their source and build:
// tests/link/data/object_subsystem.source.c).
[[nodiscard]] std::string subsystemObject(char const* subsystem) {
    return std::string{"object_subsystem_"} + subsystem + "_x86_64_pe_cl_MD_O2.obj";
}

struct SubsystemCell {
    char const*              label;
    std::vector<std::string> objects;    // linked in this order, before the program's own unit
    std::string              source;     // the program's own unit: its entries
    std::uint32_t            exit;       // the entry that runs: link.exe's, or DSS's own where link.exe refuses
    char const*              rankedBy;   // what the precedence warning names, or null when nothing is warned
};

[[nodiscard]] std::vector<SubsystemCell> subsystemCells() {
    std::string const head          = "#include <windows.h>\n";
    std::string const narrowConsole = "int main(void) { return 11; }\n";
    std::string const winMain =
        "int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) { (void)h; (void)p; (void)c; (void)s; "
        "return 44; }\n";
    std::string const wWinMain =
        "int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, LPWSTR c, int s) { (void)h; (void)p; (void)c; (void)s; "
        "return 55; }\n";
    std::string const wideConsole =
        "int wmain(int argc, unsigned short **argv) { (void)argc; (void)argv; return 22; }\n";
    std::string const windows      = subsystemObject("windows");
    std::string const console      = subsystemObject("console");
    std::string const wmainStartup = "object_entry_startup_wmain_x86_64_pe_cl_MD_O2.obj";
    return {
        {"WINDOWS: main beside WinMain", {windows}, head + narrowConsole + winMain, 44u, "/SUBSYSTEM:WINDOWS"},
        {"WINDOWS: wmain beside WinMain", {windows}, head + wideConsole + winMain, 44u, "/SUBSYSTEM:WINDOWS"},
        {"WINDOWS: main beside wWinMain", {windows}, head + narrowConsole + wWinMain, 55u, "/SUBSYSTEM:WINDOWS"},
        {"WINDOWS: WinMain beside wWinMain", {windows}, head + winMain + wWinMain, 44u, "/SUBSYSTEM:WINDOWS"},
        {"WINDOWS: main alone, which link.exe refuses and gcc runs", {windows}, narrowConsole, 11u, nullptr},
        {"CONSOLE: main beside WinMain", {console}, head + narrowConsole + winMain, 11u, "/SUBSYSTEM:CONSOLE"},
        {"CONSOLE: WinMain alone, which link.exe refuses and gcc runs", {console}, head + winMain, 44u, nullptr},
        {"WINDOWS then CONSOLE: the last stands", {windows, console}, head + narrowConsole + winMain, 11u,
         "/SUBSYSTEM:CONSOLE"},
        {"CONSOLE then WINDOWS: the last stands", {console, windows}, head + narrowConsole + winMain, 44u,
         "/SUBSYSTEM:WINDOWS"},
        {"WINDOWS beside a stated wmainCRTStartup: the stated startup restricts", {windows, wmainStartup},
         head + narrowConsole + winMain, 22u, nullptr},
    };
}

[[nodiscard]] std::vector<Input> inputsOf(SubsystemCell const& c) {
    std::vector<Input> in;
    for (auto const& o : c.objects) in.push_back(fixture(o));
    in.push_back(written("program.c", c.source));
    return in;
}

}  // namespace

// The directive vocabulary states the startups each subsystem starts a program at, resolved to the verbs they call
// an entry with: a `/SUBSYSTEM:` request carries those verbs -- WINDOWS the GUI ones, CONSOLE the console ones -- and
// a subsystem no DSS image starts under carries none. Both documents that state the vocabulary.
TEST(GuiStartups, ASubsystemStatesTheStartupsItStartsAt) {
    using M = EntryMaterialization;
    std::vector<M> const gui{M::InstancePrevCmdlineShow, M::InstancePrevWcmdlineShow};
    std::vector<M> const console{M::None, M::ArgcArgv, M::ArgcArgvEnvp, M::ArgcWargv, M::ArgcWargvWenvp};
    struct Want {
        char const*    text;
        std::vector<M> verbs;
    };
    for (auto const* stem : {"pe64-x86_64-windows", "pe64-x86_64-windows-staticlib"}) {
        SCOPED_TRACE(stem);
        auto f = ObjectFormatSchema::loadShipped(stem);
        ASSERT_TRUE(f.has_value());
        ASSERT_TRUE((*f)->pe().linkerDirectives.has_value());
        auto const& vocabulary = *(*f)->pe().linkerDirectives;
        for (Want const& w : {Want{" /SUBSYSTEM:WINDOWS", gui}, Want{" /subsystem:console,6.02", console},
                              Want{" /SUBSYSTEM:NATIVE", {}}}) {
            SCOPED_TRACE(w.text);
            auto const parsed = pe::parseCoffLinkerDirectives(w.text, vocabulary);
            ASSERT_TRUE(parsed.has_value()) << parsed.error();
            ASSERT_EQ(parsed->image.size(), 1u);
            auto const* request = std::get_if<UnitSubsystemRequest>(&parsed->image[0].value);
            ASSERT_NE(request, nullptr);
            EXPECT_TRUE(request->verbs == w.verbs) << "the request carries the verbs its subsystem's startups call";
        }
    }
}

// A unit that states the image's subsystem ranks the entries that subsystem's startups call above every other: the
// program starts where link.exe starts it, and the precedence warning names the request that ranked. A program
// defining none of them starts in its own entry, and a stated startup restricts the election whatever the subsystem.
TEST(GuiStartups, AStatedSubsystemRanksItsStartupsEntries) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "gui_startups_subsystem"};
    int k = 0;
    for (auto const& c : subsystemCells()) {
        SCOPED_TRACE(c.label);
        Built b;
        build(scratch.path() / std::to_string(k++), inputsOf(c), kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        ASSERT_FALSE(b.image.empty());
        // The entry that was elected, read off what the image's init fetches for it.
        EXPECT_EQ(importsOf(b.image, "_get_narrow_winmain_command_line"), c.exit == 44u ? 1u : 0u)
            << "WinMain is elected exactly when link.exe starts it" << b.diagnostics;
        EXPECT_EQ(importsOf(b.image, "_get_wide_winmain_command_line"), c.exit == 55u ? 1u : 0u)
            << "wWinMain is elected exactly when link.exe starts it" << b.diagnostics;
        EXPECT_EQ(importsOf(b.image, "__p___wargv"), c.exit == 22u || c.exit == 55u ? 1u : 0u)
            << "a wide entry is elected exactly when link.exe starts one" << b.diagnostics;
        if (c.rankedBy == nullptr) {
            EXPECT_FALSE(reported(b.rep, DiagnosticCode::K_ProgramEntryChosenByPrecedence)) << b.diagnostics;
        } else {
            EXPECT_NE(textOf(b.rep, DiagnosticCode::K_ProgramEntryChosenByPrecedence).find(c.rankedBy),
                      std::string::npos)
                << "the precedence warning names the subsystem that ranked: " << c.rankedBy << b.diagnostics;
        }
    }
}

// ── THE PROGRAMS, RUN (Windows x86_64 hosts) ─────────────────────────────────

namespace {

[[nodiscard]] bool onWindowsX64() {
    return test_support::currentHostOs() == "windows" && test_support::currentHostArch() == "x86_64";
}

[[nodiscard]] test_support::RunResult runWithArguments(fs::path const& image) {
    return test_support::runBinary(image, test_support::kRunBudget, /*captureStdout=*/false, /*launcher=*/{},
                                   /*programArgs=*/{"alpha", "beta"}, /*launcherExecsImage=*/false);
}

#if defined(_WIN32)
// The image started as a shortcut or a launcher starts it: its STARTUPINFO states the show value (STARTF_USESHOWWINDOW)
// -- the arm of the show routine `runBinary`, which states none, never reaches. The exit code, or nullopt when the
// process could not be started or did not end in the run budget.
[[nodiscard]] std::optional<std::uint32_t> exitWhenShown(fs::path const& image, unsigned short show) {
    std::wstring const commandLine = L"\"" + image.wstring() + L"\" alpha beta";
    STARTUPINFOW si{};
    si.cb          = sizeof si;
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = show;
    PROCESS_INFORMATION pi{};
    if (::dss::substrate::startImageInheriting(::dss::core::HostPath{image}, commandLine, &si, &pi) != 0) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> out;
    if (::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(test_support::kRunBudget.count())) == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (::GetExitCodeProcess(pi.hProcess, &code)) out = static_cast<std::uint32_t>(code);
    } else {
        ::TerminateProcess(pi.hProcess, 1u);
        ::WaitForSingleObject(pi.hProcess, 1000u);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return out;
}
#endif

}  // namespace

// Each entry is handed what both references hand it: the image's base, null, the command line past the name, and
// SW_SHOWDEFAULT (10) when the startup information states no show value -- and the stated one when it does.
TEST(GuiStartupsNative, EachGuiEntryIsHandedTheReferencesFourValues) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run on Windows x86_64 hosts only";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "gui_startups_native"};
    int k = 0;
    for (auto const& [label, source] : {std::pair{"WinMain", narrowProgram()}, std::pair{"wWinMain", wideProgram()}}) {
        SCOPED_TRACE(label);
        Built b;
        build(scratch.path() / std::to_string(k++), {written("gui.c", source)}, kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        auto const r = runWithArguments(b.image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 110u) << "100 + SW_SHOWDEFAULT, every fact holding";
#if defined(_WIN32)
        // SW_SHOWMINNOACTIVE (7): the value the startup information states is the one handed.
        auto const shown = exitWhenShown(b.image, 7u);
        ASSERT_TRUE(shown.has_value()) << "the image started with a stated show value did not run to its end";
        EXPECT_EQ(*shown, 107u) << "100 + SW_SHOWMINNOACTIVE, every fact holding";
#endif
    }
}

// The precedence cells, run: each program exits from the entry link.exe runs.
TEST(GuiStartupsNative, AProgramWithSeveralEntriesRunsTheReferencesEntry) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run on Windows x86_64 hosts only";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "gui_startups_native_precedence"};
    int k = 0;
    for (auto const& p : pairings()) {
        SCOPED_TRACE(p.label);
        Built b;
        build(scratch.path() / std::to_string(k++), {written("several.c", p.source)}, kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        auto const r = runWithArguments(b.image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, p.exit);
    }
}

// The stated-subsystem cells, run: each program exits from the entry link.exe starts, and from its own entry where
// link.exe refuses the program and gcc runs it.
TEST(GuiStartupsNative, AStatedSubsystemRunsTheReferencesEntry) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run on Windows x86_64 hosts only";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "gui_startups_native_subsystem"};
    int k = 0;
    for (auto const& c : subsystemCells()) {
        SCOPED_TRACE(c.label);
        Built b;
        build(scratch.path() / std::to_string(k++), inputsOf(c), kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        auto const r = runWithArguments(b.image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, c.exit);
    }
}
