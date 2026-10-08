// FOREIGN objects in a DSS PE image, and the address of a library function they take.
//
// D-LK-PE-DLLIMPORT-OBJECT-REFERENCE-UNRESOLVED — COFF names an import's IAT entry `__imp_X`, and every object
// compiled against a `__declspec(dllimport)` declaration (every MSVC `/MD` object: the UCRT headers declare the
// C library dllimport) reaches the function ONLY through that name. ✔MEASURED 2026-09-30: cl 19.51 `/c /O2 /MD`
// writes `__imp_puts` (UNDEF, type 0) for both the call and the address, and DSS refused the link
// (`undefined symbol '__imp_puts'`) — no such object could be linked. The format document states the spelling
// (`importAddressSymbolPrefix`); `foldImportAddressReferences` makes each such reference the import's ADDRESS SLOT,
// which the PE walker binds to the IAT entry.
//
// D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB, the PE half, with foreign code in the image: the MSVC object's
// `&puts` (an IAT load through `__imp_puts`) and its static initializer (`ADDR64 puts`), the MinGW object's `&puts`
// (a load from its `.refptr.puts` slot) and its static initializer, and DSS's own `&puts` must all be ONE value,
// the one GetProcAddress answers. Under link.exe the MSVC static is the thunk; under GNU ld every MinGW form is.
//
// Tier 1 (every host): the fold, on hand-built modules. Tier 2 (Windows with cl/lib and MinGW gcc/ar): the run.

#include "core/types/diagnostic_reporter.hpp"
#include "link/import_address_references.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] ExternImport unboundImport(std::uint32_t id, std::string name) {
    ExternImport e;
    e.symbol      = SymbolId{id};
    e.mangledName = std::move(name);
    e.isData      = true;   // what a type-0 COFF undefined symbol reads back as
    return e;
}

}  // namespace

TEST(ImportAddressReferences, AnUnboundPrefixedReferenceBecomesTheImportsAddressSlot) {
    auto fmt = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(fmt.has_value());
    auto tgt = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(tgt.has_value());
    ASSERT_EQ((*fmt)->importAddressSymbolPrefix(), "__imp_");
    AssembledModule m;
    AssembledFunction fn;
    fn.symbol = SymbolId{3};
    m.functions.push_back(fn);
    m.externImports.push_back(unboundImport(7, "__imp_puts"));
    ExternImport bound = unboundImport(8, "__imp_bound");
    bound.libraryPath = "x.dll";
    m.externImports.push_back(bound);                    // bound already: not ours to touch
    m.externImports.push_back(unboundImport(10, "__imp_"));      // the bare prefix names nothing
    m.externImports.push_back(unboundImport(11, "printf"));
    std::vector<AssembledModule> mods{m};
    auto const counts = linker::foldImportAddressReferences(mods, **fmt, **tgt, {});
    EXPECT_EQ(counts.importedSlots, 1u);
    EXPECT_EQ(counts.localPointers, 0u);
    auto const& r = mods[0].externImports[0];
    EXPECT_EQ(r.mangledName, "puts") << "the row imports the function itself";
    EXPECT_EQ(r.addressSlotSymbol, SymbolId{7})
        << "every relocation the object wrote against `__imp_puts` must now name the import's SLOT";
    EXPECT_GT(r.symbol.v, 11u) << "the import's own symbol is fresh: nothing in the object referenced it";
    EXPECT_EQ(r.kindOrigin, ExternKindOrigin::Pending)
        << "`__imp_X` states no kind; the definition (or the row it folds into) decides";
    EXPECT_TRUE(r.libraryPath.empty()) << "binding the library is the binder's job, after this";
    EXPECT_EQ(mods[0].externImports[1].mangledName, "__imp_bound");
    EXPECT_FALSE(mods[0].externImports[1].addressSlotSymbol.valid());
    EXPECT_EQ(mods[0].externImports[2].mangledName, "__imp_");
    EXPECT_EQ(mods[0].externImports[3].mangledName, "printf");
}

TEST(ImportAddressReferences, ALinkedDefinitionGetsAPointerOfTheImagesOwn) {
    // P69 review M3: `__imp_X` for an X a linked unit DEFINES links, as link.exe (LNK4217) and lld-link
    // do — the old symbol becomes a READ-ONLY POINTER of the image's own, one absolute relocation to X,
    // which the image writer base-relocates like any other pointer; X itself is a fresh UNBOUND row the
    // cross-unit resolver binds to its definition.
    auto fmt = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(fmt.has_value());
    auto tgt = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(tgt.has_value());
    AssembledModule m;
    AssembledFunction fn;
    fn.symbol = SymbolId{3};
    m.functions.push_back(fn);
    m.externImports.push_back(unboundImport(9, "__imp_mine"));
    std::vector<AssembledModule> mods{m};
    std::unordered_set<std::string> const defined{"mine"};
    auto const counts = linker::foldImportAddressReferences(mods, **fmt, **tgt, defined);
    EXPECT_EQ(counts.localPointers, 1u);
    EXPECT_EQ(counts.importedSlots, 0u);
    for (auto const& e : mods[0].externImports) {
        EXPECT_NE(e.mangledName, "__imp_mine") << "the prefixed name is no import any more";
    }
    AssembledData const* ptr = nullptr;
    for (auto const& d : mods[0].dataItems) {
        if (d.symbol == SymbolId{9}) ptr = &d;
    }
    ASSERT_NE(ptr, nullptr) << "every reference the object wrote against `__imp_mine` must name the pointer";
    EXPECT_EQ(ptr->section, DataSectionKind::RelRoConst) << "read-only after its relocation, as link.exe's is";
    EXPECT_EQ(ptr->bytes.size(), 8u);
    ASSERT_EQ(ptr->relocations.size(), 1u);
    auto const abs64 = linker::absolutePointerRelocKind(**tgt, 8);
    ASSERT_TRUE(abs64.has_value());
    EXPECT_EQ(ptr->relocations[0].kind, *abs64) << "the target's absolute pointer relocation";
    EXPECT_EQ(ptr->relocations[0].addend, 0);
    ExternImport const* def = nullptr;
    for (auto const& e : mods[0].externImports) {
        if (e.symbol == ptr->relocations[0].target) def = &e;
    }
    ASSERT_NE(def, nullptr) << "the pointer's relocation must name an unbound row the resolver binds";
    EXPECT_EQ(def->mangledName, "mine");
    EXPECT_TRUE(def->libraryPath.empty());
}

TEST(ImportAddressReferences, AFormatThatStatesNoPrefixRewritesNothing) {
    // `__imp_` is COFF's convention, read from the PE documents; an ELF document states none, so a symbol
    // that merely begins with those six characters is an ordinary name there.
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(fmt.has_value());
    auto tgt = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(tgt.has_value());
    EXPECT_TRUE((*fmt)->importAddressSymbolPrefix().empty());
    AssembledModule m;
    m.externImports.push_back(unboundImport(7, "__imp_puts"));
    std::vector<AssembledModule> mods{m};
    auto const counts = linker::foldImportAddressReferences(mods, **fmt, **tgt, {});
    EXPECT_EQ(counts.importedSlots + counts.localPointers, 0u);
    EXPECT_EQ(mods[0].externImports[0].mangledName, "__imp_puts");
}

// ── Tier 2: an MSVC `/MD` object, a MinGW object and DSS code in ONE DSS image ──

namespace {

constexpr char const* kMsvcMember =
    "#include <stdio.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "put_fn const msvc_table[2] = {puts, 0};\n"                        // ADDR64 puts
    "void const *msvc_puts_address(void) { return (void const *)puts; }\n"   // mov rax, [__imp_puts]
    "int msvc_call(void) { return puts(\"msvc member\"); }\n"          // call [__imp_puts]
    // P69 review M3: a dllimport declaration of a function the DSS image itself DEFINES — cl writes
    // `__imp_dss_defined`, which only a pointer of the image's own can satisfy (link.exe: LNK4217).
    "__declspec(dllimport) int dss_defined(void);\n"
    "int msvc_calls_dss_defined(void) { return dss_defined(); }\n"      // call [__imp_dss_defined]
    "void const *msvc_dss_defined_address(void) { return (void const *)dss_defined; }\n";

constexpr char const* kMingwMember =
    "#include <stdio.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "put_fn mingw_table[2] = {puts, 0};\n"                              // ADDR64 puts in .data
    "void const *mingw_puts_address(void) { return (void const *)puts; }\n";  // mov rax, [.refptr.puts]

constexpr char const* kMain =
    "#include <stdio.h>\n"
    "#include <windows.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "extern put_fn const msvc_table[2];\n"
    "extern put_fn mingw_table[2];\n"
    "void const *msvc_puts_address(void);\n"
    "void const *mingw_puts_address(void);\n"
    "int msvc_call(void);\n"
    "int msvc_calls_dss_defined(void);\n"
    "void const *msvc_dss_defined_address(void);\n"
    "int dss_defined(void) { return 7; }\n"
    "int main(void) {\n"
    "    HMODULE crt = LoadLibraryA(\"ucrtbase.dll\");\n"
    "    void const *want = crt ? (void const *)GetProcAddress(crt, \"puts\") : 0;\n"
    "    int bad = 0;\n"
    "    if (want == 0) bad |= 1;\n"
    "    if (msvc_puts_address() != want) bad |= 2;\n"
    "    if ((void const *)msvc_table[0] != want) bad |= 4;\n"
    "    if (mingw_puts_address() != want) bad |= 8;\n"
    "    if ((void const *)mingw_table[0] != want) bad |= 16;\n"
    "    if ((void const *)puts != want) bad |= 32;\n"
    "    if (msvc_call() < 0) bad |= 64;\n"
    "    if (msvc_calls_dss_defined() != 7) bad |= 128;\n"
    "    if (msvc_dss_defined_address() != (void const *)dss_defined) bad |= 256;\n"
    "    return bad == 0 ? 42 : 1000 + bad;\n"
    "}\n";

void writeText(fs::path const& p, char const* text) { std::ofstream(p, std::ios::binary) << text; }

[[nodiscard]] bool shell(fs::path const& dir, std::string const& cmd) {
    std::string const line = "cd /d \"" + dir.string() + "\" && " + cmd + " >nul 2>&1";
    return std::system(("\"" + line + "\"").c_str()) == 0;
}

}  // namespace

TEST(PeForeignImportAddressNative, MsvcAndMingwObjectsAndDssCodeAgreeOnOneAddressOfPuts) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-foreign-import-address"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
#if defined(_WIN32)
    if (std::system("where gcc >nul 2>&1") != 0 || std::system("where ar >nul 2>&1") != 0) {
        GTEST_SKIP() << "no MinGW `gcc`/`ar` on PATH -- the mixed-object arm is inert on this host";
    }
#else
    // Unreachable in practice (MSVC was located above, which only happens on Windows), and
    // `where ... >nul` would create a file named `nul` on a POSIX shell.
    GTEST_SKIP() << "not a Windows host -- the mixed-object arm is inert here";
#endif
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();

    writeText(dir / "msvc_member.c", kMsvcMember);
    writeText(dir / "mingw_member.c", kMingwMember);
    writeText(dir / "main.c", kMain);
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- /MD msvc_member.c"));
    ASSERT_TRUE(tools.run("lib /nologo /OUT:msvc_member.lib msvc_member.obj"));
    // ★ `-mcmodel=large`, ON PURPOSE: it is the MinGW shape the loader CAN bind — `&puts` loaded from the object's
    // own `.refptr.puts` slot (an `ADDR64 puts` COMDAT), which design c2 makes loader-bound like any other slot.
    // ✔MEASURED 2026-09-30 on this host's gcc 13.2.0 (Brecht Sanders, ucrt): `-O0` and `-O2 -mcmodel=large` load
    // from `.refptr.puts`; plain `-O2` (medium, the default) and `-O2 -fPIC` compute it with `leaq puts(%rip)`, an
    // instruction that can only ever yield the image's thunk. DSS gives THAT unit the thunk for every address it
    // takes of `puts` (`pcRelativeImportAddress: callEntry`, residue (c) of D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE,
    // closed), so it agrees with itself and not with GetProcAddress — the arm below,
    // `MingwDefaultO2MemberAgreesWithItselfOnTheThunk`. This arm needs every form to be GetProcAddress's, hence the
    // slot-loading shape.
    ASSERT_TRUE(shell(dir, "gcc -c -O2 -mcmodel=large -o mingw_member.o mingw_member.c"));
    ASSERT_TRUE(shell(dir, "ar rcs libmingw_member.a mingw_member.o"));

    auto const out = dir / "out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    p.setResolveLibraries(std::vector<fs::path>{dir / "msvc_member.lib", dir / "libmingw_member.a"});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(std::vector<std::string>{(dir / "main.c").string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    std::string diags;
    for (auto const& d : rep.all()) diags += "\n  " + d.actual;
    ASSERT_EQ(rc, 0) << "DSS must link an MSVC /MD member (it names `__imp_puts`) beside a MinGW one:" << diags;
    auto const r = test_support::runBinary(out / "main.exe");
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "1000 + a bitset: 2 the MSVC object's &puts, 4 its static, 8 the MinGW object's &puts, 16 its "
           "static, 32 DSS's &puts, 64 a call through `__imp_puts` — each against GetProcAddress; 128 a call "
           "through `__imp_dss_defined` (a DSS definition, the LNK4217 pointer), 256 that pointer's value "
           "against DSS's own `&dss_defined`";
}

// ── P69 review M1 (c): MinGW gcc's DEFAULT `-O2` shape, which no loader can bind ──
//
// ✔MEASURED 2026-09-30 / 2026-10-01 on this host's gcc 13.2.0 (ucrt): plain `-O2` computes `&puts` as
// `leaq puts(%rip)` — a displacement, which reaches nothing outside the image, so only the image's import THUNK —
// and writes the static `{puts}` as an ADDR64 (it also carries an unused `.rdata$.refptr.puts`). GNU ld, link.exe
// 14.51 and lld-link each bind BOTH to the one thunk: code == static, != GetProcAddress (exit 42 under all three,
// work/xa/m1probe). DSS gives that object the same meaning (`pcRelativeImportAddress: callEntry`): every address
// the UNIT takes of `puts` is the thunk, so the unit agrees with itself, while DSS's own code — and every other
// unit — keeps the loader-bound address GetProcAddress answers.
namespace {

constexpr char const* kMingwDefaultMember =
    "#include <stdio.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "put_fn mingw_o2_table[2] = {puts, 0};\n"                                  // ADDR64 puts
    "void const *mingw_o2_puts_address(void) { return (void const *)puts; }\n";  // leaq puts(%rip)

constexpr char const* kMainForTheDefaultShape =
    "#include <stdio.h>\n"
    "#include <windows.h>\n"
    "typedef int (*put_fn)(const char *);\n"
    "extern put_fn mingw_o2_table[2];\n"
    "void const *mingw_o2_puts_address(void);\n"
    "int main(void) {\n"
    "    HMODULE crt = LoadLibraryA(\"ucrtbase.dll\");\n"
    "    void const *want = crt ? (void const *)GetProcAddress(crt, \"puts\") : 0;\n"
    "    void const *code = mingw_o2_puts_address();\n"
    "    int bad = 0;\n"
    "    if (want == 0) bad |= 1;\n"
    "    if (code != (void const *)mingw_o2_table[0]) bad |= 2;\n"
    "    if (code == want) bad |= 4;\n"
    "    if ((void const *)puts != want) bad |= 8;\n"
    "    if (((put_fn)code)(\"through the thunk\") < 0) bad |= 16;\n"
    "    return bad == 0 ? 42 : 1000 + bad;\n"
    "}\n";

[[nodiscard]] std::vector<std::uint8_t> readAll(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Does the COFF object's `.text` reach `puts` through a REL32 field whose instruction is `lea r64, [rip+d]`
// (`48 8D /r` with ModR/M mod=00 rm=101)? The measured default shape; a gcc that stopped writing it would leave
// this arm testing something else, so the arm asserts it first.
[[nodiscard]] bool textTakesPutsByLea(std::vector<std::uint8_t> const& b) {
    auto const u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8)); };
    auto const u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (b.size() < 20) return false;
    std::uint32_t const nsec = u16(2), symPtr = u32(8), nsym = u32(12);
    std::size_t const strTab = symPtr + nsym * 18u;
    auto const symName = [&](std::uint32_t idx) {
        std::size_t const s = symPtr + idx * 18u;
        if (u32(s) == 0) return std::string{reinterpret_cast<char const*>(&b[strTab + u32(s + 4)])};
        std::string n;
        for (std::size_t k = 0; k < 8 && b[s + k] != 0; ++k) n.push_back(static_cast<char>(b[s + k]));
        return n;
    };
    for (std::uint32_t i = 0; i < nsec; ++i) {
        std::size_t const h = 20 + i * 40u;
        std::string name;
        for (std::size_t k = 0; k < 8 && b[h + k] != 0; ++k) name.push_back(static_cast<char>(b[h + k]));
        if (name.rfind(".text", 0) != 0) continue;
        std::uint32_t const raw = u32(h + 20), relPtr = u32(h + 24), nrel = u16(h + 32);
        for (std::uint32_t r = 0; r < nrel; ++r) {
            std::size_t const rel = relPtr + r * 10u;
            std::uint32_t const at = u32(rel), sym = u32(rel + 4), type = u16(rel + 8);
            if (type != 4 /* IMAGE_REL_AMD64_REL32 */ || at < 3 || symName(sym) != "puts") continue;
            std::size_t const op = raw + at - 3;
            if (b[op] == 0x48 && b[op + 1] == 0x8D && (b[op + 2] & 0xC7) == 0x05) return true;
        }
    }
    return false;
}

}  // namespace

TEST(PeForeignImportAddressNative, MingwDefaultO2MemberAgreesWithItselfOnTheThunk) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "pe-foreign-import-address-o2"};
    auto const dir = scratch.path();
#if defined(_WIN32)
    if (std::system("where gcc >nul 2>&1") != 0 || std::system("where ar >nul 2>&1") != 0) {
        GTEST_SKIP() << "no MinGW `gcc`/`ar` on PATH -- the default-shape arm is inert on this host";
    }
#else
    GTEST_SKIP() << "not a Windows host -- the default-shape arm is inert here";
#endif
    writeText(dir / "mingw_o2.c", kMingwDefaultMember);
    writeText(dir / "main.c", kMainForTheDefaultShape);
    ASSERT_TRUE(shell(dir, "gcc -c -O2 -o mingw_o2.o mingw_o2.c"));   // the DEFAULT model: no -mcmodel
    ASSERT_TRUE(textTakesPutsByLea(readAll(dir / "mingw_o2.o")))
        << "the measured default shape (`leaq puts(%rip)`) is what this arm is about; this gcc wrote another";
    ASSERT_TRUE(shell(dir, "ar rcs libmingw_o2.a mingw_o2.o"));

    auto const out = dir / "out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    p.setResolveLibraries(std::vector<fs::path>{dir / "libmingw_o2.a"});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(std::vector<std::string>{(dir / "main.c").string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    std::string diags;
    for (auto const& d : rep.all()) diags += "\n  " + d.actual;
    ASSERT_EQ(rc, 0) << diags;
    auto const r = test_support::runBinary(out / "main.exe");
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "1000 + a bitset: 2 the member's code &puts differs from its own static (the unit disagrees with "
           "itself), 4 the member's &puts equals GetProcAddress (a displacement cannot reach the DLL — the "
           "measurement under GNU ld, link.exe and lld-link says thunk), 8 DSS's own &puts is not "
           "GetProcAddress's, 16 the thunk does not call puts";
}
