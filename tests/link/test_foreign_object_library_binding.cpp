// P69 fold 3c, lane p69/xa — a FOREIGN object that calls the C library, linked by DSS beside a DSS `main`.
//
//   D-LK-FOREIGN-OBJECT-NAMED-TO-THE-LINK-BINDS-NO-LIBRARY-IMPORT — an object NAMED to the link (`dsscp --compile
//     main.c lib.obj`) had none of its library references bound: the three binding arms ran over the archive-pulled
//     members alone. ✔MEASURED 2026-10-20: every object of cl /MD, clang-cl /MD, cl /MT, clang-cl /MT and mingw-w64
//     gcc whose `say()` calls `puts` (directly, through `__imp_puts`, or as its own <stdio.h> spells it) was refused
//     "undefined symbol", where link.exe or GNU ld links it and the program exits 42, and where DSS linked the SAME
//     object archived. Both kinds are bound by one function now (`bindForeignImports`, compile_pipeline.cpp).
//   D-LK-COFF-READER-TAKES-AN-UNTYPED-UNDEFINED-SYMBOL-AS-DATA — clang-cl 19.1.5 writes type 0 on every undefined
//     symbol, and the COFF reader read type 0 as a stated DATUM: clang-cl's default /MT `puts()` was refused "'puts'
//     binds as a DATA import" even archived. Type 0 states nothing now (`Pending`); the definition decides.
//   D-FF1-AR-READER-REFUSES-THE-COFF-LONGNAMES-MEMBER — lib.exe's "//" longnames member ends each name at a NUL,
//     and the ar reader read GNU's `/\n` form alone: every lib.exe archive with a member name past 15 characters
//     was refused as corrupted.
//
// The fixtures (`tests/link/data/foreign_object_library_binding.source.c` states how each was built; each built
// twice, byte-identical, recording no host path; each linked by its reference linker beside a reference `main` runs
// to 42):
//   foreign_object_library_binding_x86_64_pe_cl_MD_O2.obj       md5 dbbb69378fd68a9dd7c6226561100cf0  `__imp_puts`
//   foreign_object_library_binding_x86_64_pe_clangcl_MT_O2.obj  md5 fe08e6ec21638176b00d93913c005dee  `puts`, type 0
//   foreign_object_library_binding_x86_64_pe_gcc_O2.obj         md5 b8d04ea86230f0369557a39227f25408  `puts`, 0x20
//   libforeign_object_library_binding_x86_64_pe_cl_MD_O2.a      md5 0677cb93405c1b457a962bace54e5341  lib.exe, the
//                                                                 cl /MD object under its own 52-character name
// No guard enforces an md5: it records WHICH BYTES were measured.
//
// P69 fold 3d, the same row's second half: in a build of OBJECTS ALONE the object defining the entry was made the
// link's client, and the client reached no binder. ✔MEASURED (lane `xa`'s `client_entry_dss.py`, run 24's dsscp):
// `tests/link/data/object_entry_puts_*` (a `main` calling `puts`) linked alone or beside `object_entry_getenv_*` (a
// unit calling `getenv`), in either order, was refused "undefined symbol" for `__imp_puts` (cl /MD, PE) and for
// `puts` (gcc 13.3, ELF x86_64), the other object's `getenv` bound, where link.exe and GNU ld link both and the
// program exits 42. Every named object is an object input of the link now, the entry's included.
//
// Every TEST but the `…Native` ones runs in-process on every leg: the images are linked, never run, there.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/binary_readers/ar_reader.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "host_native_target.hpp"
#include "image_dependency_table.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kObjects[] = {
    "foreign_object_library_binding_x86_64_pe_cl_MD_O2.obj",
    "foreign_object_library_binding_x86_64_pe_clangcl_MT_O2.obj",
    "foreign_object_library_binding_x86_64_pe_gcc_O2.obj",
};
constexpr char const* kArchive    = "libforeign_object_library_binding_x86_64_pe_cl_MD_O2.a";
constexpr char const* kLongMember = "foreign_object_library_binding_x86_64_pe_cl_MD_O2.obj";
constexpr char const* kMain       = "int say(void);\nint main(void) { return say(); }\n";
constexpr char const* kExec       = "x86_64:pe64-x86_64-windows-exec";

[[nodiscard]] fs::path fixturePath(std::string_view name) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
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

[[nodiscard]] std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// DSS's image of a DSS `main` beside `object` (named to the link) or beside `archive` (searched); the image's path,
// or empty with `rep` holding why.
[[nodiscard]] fs::path dssImageOf(fs::path const& dir, std::optional<fs::path> const& object,
                            std::optional<fs::path> const& archive, DiagnosticReporter& rep) {
    fs::create_directories(dir);
    auto const mainSrc = dir / "main.c";
    std::ofstream(mainSrc, std::ios::binary) << kMain;
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    if (archive.has_value()) p.setResolveLibraries(std::vector<fs::path>{*archive});
    std::vector<std::string> inputs{mainSrc.string()};
    if (object.has_value()) inputs.push_back(object->string());
    if (p.compileFiles(inputs, "c", std::vector<std::string>{kExec}, rep) != 0) return {};
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") return e.path();
    }
    return {};
}

// `puts` is imported, and from the C library's image.
void expectPutsFromTheCLibrary(fs::path const& image) {
    auto const bytes = bytesOf(image);
    auto const symbols = test_support::peImportedSymbols(bytes);
    EXPECT_NE(std::find(symbols.begin(), symbols.end(), "puts"), symbols.end()) << "the image imports `puts`";
    std::vector<std::string> libraries;
    for (auto const& l : test_support::peImportedLibraries(bytes)) libraries.push_back(lower(l));
    EXPECT_NE(std::find(libraries.begin(), libraries.end(), "ucrtbase.dll"), libraries.end())
        << "from the C library's image, ucrtbase.dll";
}

}  // namespace

// ══ THE NAMED OBJECT ════════════════════════════════════════════════════════════════════════════════════════════════

// Each producer's object NAMED to the link is bound as an archived member is: `__imp_puts` folded into the import's
// slot (cl /MD), `puts` of type 0 decided by the library's definition (clang-cl /MT), `puts` of type 0x20 (gcc).
TEST(ForeignObjectLibraryBinding, EachNamedObjectsLibraryCallIsBoundToTheLibrarysImage) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-object-library-binding"};
    for (char const* name : kObjects) {
        SCOPED_TRACE(name);
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / name, fixturePath(name), std::nullopt, rep);
        ASSERT_FALSE(image.empty()) << "DSS links the object named to it, as link.exe and GNU ld do:"
                                    << diagnosticsOf(rep);
        expectPutsFromTheCLibrary(image);
    }
}

// ══ THE ENTRY'S OWN OBJECT, in a build of objects alone ═══════════════════════════════════════════════════════════

namespace {

constexpr char const* kPeEntryPuts    = "object_entry_puts_x86_64_pe_cl_MD_O2.obj";
constexpr char const* kPeEntryGetenv  = "object_entry_getenv_x86_64_pe_cl_MD_O2.obj";
constexpr char const* kElfEntryPuts   = "object_entry_puts_x86_64_elf_gcc_O2.o";
constexpr char const* kElfEntryGetenv = "object_entry_getenv_x86_64_elf_gcc_O2.o";
constexpr char const* kElfExec        = "x86_64:elf64-x86_64-linux-exec";

// DSS's image of `objects` alone, in that order, for `target`: the image's path, or empty with `rep` holding why.
[[nodiscard]] fs::path dssImageOfObjects(fs::path const& dir, std::vector<char const*> const& objects,
                                         char const* target, DiagnosticReporter& rep) {
    fs::create_directories(dir);
    std::vector<std::string> inputs;
    for (char const* name : objects) inputs.push_back(fixturePath(name).string());
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    std::vector<std::string> const targets{target};
    int const rc = inputs.size() == 1u ? p.compileFiles(inputs, "c", targets, rep)
                                       : p.compileUnits(inputs, "c", targets, rep);
    if (rc != 0) return {};
    fs::path image;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && (image.empty() || e.path().extension() == ".exe")) image = e.path();
    }
    return image;
}

// The orders each pair is linked in, the entry's object alone first.
std::vector<std::vector<char const*>> const kPeEntryOrders{
    {kPeEntryPuts}, {kPeEntryPuts, kPeEntryGetenv}, {kPeEntryGetenv, kPeEntryPuts}};
std::vector<std::vector<char const*>> const kElfEntryOrders{
    {kElfEntryPuts}, {kElfEntryPuts, kElfEntryGetenv}, {kElfEntryGetenv, kElfEntryPuts}};

// The names an ELF64 little-endian image's `.dynsym` states UNDEFINED (section index 0): the references its loader
// binds. Read by the test's own walk of the section headers, not by the readers under test.
[[nodiscard]] std::vector<std::string> undefinedDynamicSymbolsOf(std::vector<std::uint8_t> const& b) {
    std::vector<std::string> out;
    auto const u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8)); };
    auto const u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    auto const u64 = [&](std::size_t o) { return static_cast<std::uint64_t>(u32(o)) | (std::uint64_t{u32(o + 4)} << 32); };
    if (b.size() < 64u || b[0] != 0x7F || b[1] != 'E' || b[4] != 2u || b[5] != 1u) {
        ADD_FAILURE() << "the image is no ELF64 little-endian file";
        return out;
    }
    std::uint64_t const shoff = u64(0x28);
    std::uint32_t const shentsize = u16(0x3A), shnum = u16(0x3C);
    if (shoff == 0u || shentsize < 64u || shoff + std::uint64_t{shnum} * shentsize > b.size()) {
        ADD_FAILURE() << "the image states no section headers in the file";
        return out;
    }
    auto const sh = [&](std::uint32_t i) { return static_cast<std::size_t>(shoff + std::uint64_t{i} * shentsize); };
    for (std::uint32_t s = 0; s < shnum; ++s) {
        if (u32(sh(s) + 4) != 11u /*SHT_DYNSYM*/) continue;
        std::uint32_t const link = u32(sh(s) + 0x28);
        if (link >= shnum) continue;
        std::size_t const symOff = u64(sh(s) + 0x18), symSize = u64(sh(s) + 0x20);
        std::size_t const strOff = u64(sh(link) + 0x18);
        for (std::size_t e = symOff + 24u; e + 24u <= symOff + symSize && e + 24u <= b.size(); e += 24u) {
            if (u16(e + 6) != 0u /*SHN_UNDEF*/) continue;
            std::string name;
            for (std::size_t p = strOff + u32(e); p < b.size() && b[p] != 0u; ++p) name.push_back(static_cast<char>(b[p]));
            out.push_back(std::move(name));
        }
    }
    return out;
}

[[nodiscard]] std::string labelOf(std::vector<char const*> const& objects) {
    std::string s;
    for (char const* o : objects) s += (s.empty() ? "" : ", ") + std::string{o};
    return s;
}

}  // namespace

// Every leg: the cl /MD object defining `main`, which calls `puts`, linked alone or beside an object calling
// `getenv`, in either order: `puts` is imported from the C library's image, and so is `getenv` where it is named.
// RED-ON-DISABLE: make the entry's object the link's client again -> "undefined symbol '__imp_puts'".
TEST(ForeignObjectLibraryBinding, TheEntrysObjectInABuildOfObjectsAloneIsBoundOnPe) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-object-entry-pe"};
    std::size_t k = 0;
    for (auto const& objects : kPeEntryOrders) {
        SCOPED_TRACE(labelOf(objects));
        DiagnosticReporter rep;
        auto const image = dssImageOfObjects(scratch.path() / std::to_string(k++), objects, kExec, rep);
        ASSERT_FALSE(image.empty()) << "link.exe links these objects:" << diagnosticsOf(rep);
        expectPutsFromTheCLibrary(image);
        if (objects.size() == 2u) {
            auto const symbols = test_support::peImportedSymbols(bytesOf(image));
            EXPECT_NE(std::find(symbols.begin(), symbols.end(), "getenv"), symbols.end()) << "the image imports getenv";
        }
    }
}

// Every leg: gcc's object defining `main`, which calls `puts`, linked alone or beside an object calling `getenv`, in
// either order, for an ELF x86_64 executable: the image needs the C library, and its `.dynsym` names `puts` (and
// `getenv`) undefined, for the loader to bind. RED-ON-DISABLE: make the entry's object the link's client again ->
// "undefined symbol 'puts'".
TEST(ForeignObjectLibraryBinding, TheEntrysObjectInABuildOfObjectsAloneIsBoundOnElf) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-object-entry-elf"};
    std::size_t k = 0;
    for (auto const& objects : kElfEntryOrders) {
        SCOPED_TRACE(labelOf(objects));
        DiagnosticReporter rep;
        auto const image = dssImageOfObjects(scratch.path() / std::to_string(k++), objects, kElfExec, rep);
        ASSERT_FALSE(image.empty()) << "GNU ld links these objects:" << diagnosticsOf(rep);
        auto const bytes  = bytesOf(image);
        auto const needed = test_support::elfNeededLibraries(bytes);
        EXPECT_EQ(test_support::dependencyOccurrences(needed, "libc.so.6"), 1u)
            << "the image needs the C library once; it needs: " << test_support::joinDependencies(needed);
        auto const undefined = undefinedDynamicSymbolsOf(bytes);
        EXPECT_EQ(test_support::dependencyOccurrences(undefined, "puts"), 1u)
            << "its .dynsym names puts undefined; it names: " << test_support::joinDependencies(undefined);
        if (objects.size() == 2u) {
            EXPECT_EQ(test_support::dependencyOccurrences(undefined, "getenv"), 1u)
                << "and getenv; it names: " << test_support::joinDependencies(undefined);
        }
    }
}

// ══ THE ARCHIVED OBJECT, under lib.exe's long name ═══════════════════════════════════════════════════════════════

// lib.exe's archive of the cl /MD object, the member under its own 52-character name: read to that name, searched,
// and its library call bound.
TEST(ForeignObjectLibraryBinding, ALibExeArchiveWithALongMemberNameIsSearchedAndBound) {
    auto const bytes = bytesOf(fixturePath(kArchive));
    ASSERT_FALSE(bytes.empty());
    {
        DiagnosticReporter rep;
        auto const archive = ffi::readArArchive(std::span<std::uint8_t const>{bytes}, kArchive, rep);
        ASSERT_TRUE(archive.has_value()) << archive.error().detail << diagnosticsOf(rep);
        ASSERT_EQ(archive->members.size(), 1u);
        EXPECT_EQ(archive->members.front().name, kLongMember) << "the name its longnames entry holds, to its NUL";
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-object-library-binding-ar"};
    DiagnosticReporter rep;
    auto const image = dssImageOf(scratch.path(), std::nullopt, fixturePath(kArchive), rep);
    ASSERT_FALSE(image.empty()) << "DSS links the lib.exe archive, as link.exe does:" << diagnosticsOf(rep);
    expectPutsFromTheCLibrary(image);
}

namespace {

// A minimal `ar` archive: the "//" table, then one member per name field, each holding `name` bytes.
[[nodiscard]] std::vector<std::uint8_t> arWith(std::string const& longNames, std::vector<std::string> const& fields) {
    std::vector<std::uint8_t> out;
    auto const put = [&](std::string const& s) { out.insert(out.end(), s.begin(), s.end()); };
    auto const header = [&](std::string const& field, std::size_t size) {
        std::string h = field;
        h.resize(16, ' ');
        std::string sz = std::to_string(size);
        sz.resize(10, ' ');
        put(h + std::string(12, ' ') + std::string(6, ' ') + std::string(6, ' ') + std::string(8, ' ') + sz + "`\n");
    };
    put("!<arch>\n");
    header("//", longNames.size());
    put(longNames);
    if (longNames.size() % 2 != 0) put("\n");
    for (auto const& f : fields) {
        header(f, 2);
        put("xx");
    }
    return out;
}

[[nodiscard]] std::vector<std::string> memberNames(std::vector<std::uint8_t> const& archive, std::string& error) {
    DiagnosticReporter rep;
    auto const read = ffi::readArArchive(std::span<std::uint8_t const>{archive}, "synthetic.a", rep);
    if (!read.has_value()) {
        error = read.error().detail;
        return {};
    }
    std::vector<std::string> names;
    for (auto const& m : read->members) names.push_back(m.name);
    return names;
}

}  // namespace

// The table's own bytes say how each entry ends: GNU's `<name>/\n`, the COFF archive format's `<name>\0`, or a table
// mixing the two, entry by entry; a final entry that ends in neither is still refused.
TEST(ArLongNameTable, EachEntryEndsWhereItsOwnBytesSay) {
    std::string error;
    std::string const coff = std::string{"a_coff_member_name.obj"} + '\0' + "another_coff_name.obj" + '\0';
    EXPECT_EQ(memberNames(arWith(coff, {"/0", "/23"}), error),
              (std::vector<std::string>{"a_coff_member_name.obj", "another_coff_name.obj"}))
        << error;
    std::string const gnu = "a_gnu_member_name.o/\nanother_gnu_name.o/\n";
    EXPECT_EQ(memberNames(arWith(gnu, {"/0", "/21"}), error),
              (std::vector<std::string>{"a_gnu_member_name.o", "another_gnu_name.o"}))
        << error;
    std::string const mixed = std::string{"a_gnu_member_name.o/\n"} + "a_coff_member_name.obj" + '\0';
    EXPECT_EQ(memberNames(arWith(mixed, {"/0", "/21"}), error),
              (std::vector<std::string>{"a_gnu_member_name.o", "a_coff_member_name.obj"}))
        << error;
    error.clear();
    EXPECT_TRUE(memberNames(arWith("a_name_that_never_ends", {"/0"}), error).empty());
    EXPECT_NE(error.find("unterminated final entry"), std::string::npos) << error;
}

// ══ THE READING OF COFF'S SYMBOL TYPE ════════════════════════════════════════════════════════════════════════════

// 0x20 states a function; any other type states nothing, and the definition decides.
TEST(ForeignObjectLibraryBinding, AnUndefinedCoffSymbolOfTypeZeroStatesNoKind) {
    auto const target = TargetSchema::loadShipped("x86_64");
    auto const format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(target.has_value() && format.has_value());
    auto const rowOf = [&](char const* fixture, std::string_view name) -> std::optional<ExternImport> {
        auto const bytes = bytesOf(fixturePath(fixture));
        DiagnosticReporter rep;
        auto const mod = pe::readRelocatableObject(bytes, **target, **format, rep, CompilationUnitId{1});
        EXPECT_TRUE(mod.has_value()) << fixture << diagnosticsOf(rep);
        if (!mod.has_value()) return std::nullopt;
        for (auto const& e : mod->externImports) {
            if (e.mangledName == name) return e;
        }
        ADD_FAILURE() << fixture << " reads no row '" << name << "'";
        return std::nullopt;
    };
    auto const clang = rowOf("foreign_object_library_binding_x86_64_pe_clangcl_MT_O2.obj", "puts");
    ASSERT_TRUE(clang.has_value());
    EXPECT_EQ(clang->kindOrigin, ExternKindOrigin::Pending) << "clang-cl's type 0 states nothing";
    auto const gcc = rowOf("foreign_object_library_binding_x86_64_pe_gcc_O2.obj", "puts");
    ASSERT_TRUE(gcc.has_value());
    EXPECT_EQ(gcc->kindOrigin, ExternKindOrigin::Stated) << "type 0x20 states a function";
    EXPECT_FALSE(gcc->isData);
}

// ══ NATIVE: each image runs ═══════════════════════════════════════════════════════════════════════════════════════

// Windows x86_64: each image DSS links — every named object and the lib.exe archive — runs to 42, as the reference
// linker's program of the same object does.
TEST(ForeignObjectLibraryBindingNative, EachImageRunsTo42) {
    if (test_support::hostNativeTarget().execTarget != kExec) {
        GTEST_SKIP() << "the images run only where the host is Windows x86_64";
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-object-library-binding-run"};
    auto const runs = [](fs::path const& image) {
        ASSERT_FALSE(image.empty());
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "say() returns 42 when `puts` succeeded";
    };
    for (char const* name : kObjects) {
        SCOPED_TRACE(name);
        DiagnosticReporter rep;
        auto const image = dssImageOf(scratch.path() / name, fixturePath(name), std::nullopt, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        runs(image);
    }
    SCOPED_TRACE(kArchive);
    DiagnosticReporter rep;
    auto const image = dssImageOf(scratch.path() / "archive", std::nullopt, fixturePath(kArchive), rep);
    ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
    runs(image);
}

// Where the host runs the format's executables (Windows x86_64: the PE images; Linux x86_64: the ELF ones): each image
// of the entry's object, alone or beside the `getenv` object in either order, runs to 42, as the reference linker's
// program of the same objects does.
TEST(ForeignObjectLibraryBindingNative, EachImageOfTheEntrysObjectRunsTo42) {
    auto const host = std::string{test_support::hostNativeTarget().execTarget};
    bool const pe   = host == kExec;
    bool const elf  = host == kElfExec;
    if (!pe && !elf) GTEST_SKIP() << "the images run only where the host is Windows x86_64 or Linux x86_64";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-object-entry-run"};
    std::size_t k = 0;
    for (auto const& objects : pe ? kPeEntryOrders : kElfEntryOrders) {
        SCOPED_TRACE(labelOf(objects));
        DiagnosticReporter rep;
        auto const image = dssImageOfObjects(scratch.path() / std::to_string(k++), objects, pe ? kExec : kElfExec, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        auto const r = test_support::runBinary(image, test_support::kRunBudget, /*captureStdout=*/true);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "main returns 42 when `puts` succeeded";
        EXPECT_NE(r.capturedStdout.find("entry object"), std::string::npos) << "puts wrote its line";
    }
}
