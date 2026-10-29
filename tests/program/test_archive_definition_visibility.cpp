// D-LK-PE-DLL-EXPORTS-THE-SHIPPED-RUNTIME-IT-LINKS — the shipped runtime is built HIDDEN, once, for the whole
// archive, and a shared library that links it exports its own API alone.
//
// TWO HALVES, pinned separately because they fail separately:
//
//   * THE MECHANISM — `Program::setArchiveDefinitionVisibility` (the archive's own declaration, libgcc.a's semantics).
//     Every external definition a member COMPILES comes back from the member's own symbol table restated, and NONE is
//     deleted: the restatement happens on the LOWERED member, after the optimizer, because DCE roots a definition only
//     while `isExternallyVisible` holds (D-OPT-DCE-DELETES-A-RELOCATABLE-MEMBERS-HIDDEN-DEFINITIONS) — hiding before it
//     would let `--config=release` delete every definition the member's own unit never calls, which for a runtime unit
//     is all of them. The release arm below is that ordering's pin. The control arm (no declaration) keeps `Default`.
//
//   * THE DECLARATION — `resolveShippedRuntimeArchives` makes it for every shipped runtime archive, so a shared
//     library whose one function calls runtime bodies exports that function alone, as gcc and clang `-shared` and
//     Apple clang `-dynamiclib` do (✔MEASURED, probe-reference-cc runs 20261001-045157-a7ec61fb and
//     20261001-045943-1ce3d762). Read with DSS's own export-surface readers (the `--resolve-library` ones), on every
//     host: nothing here runs a binary. The runtime is proved INSIDE each image (a defined `memalignment` in the ELF
//     `.symtab`, a defined non-exported `_memalignment` in the Mach-O symbol table) — without that half, a library
//     that had not linked the runtime at all would export `lib_entry` alone too.
//
// THE pe CASE rides the COFF MEMBER: a COFF symbol has no visibility field, so DSS's COFF writer states a hidden
// external definition as a `.drectve` `-exclude-symbols:<name>` directive beside the ordinary external symbol (the
// ecosystem's spelling: clang emits it for MinGW targets, ✔MEASURED run 20261001-152320-120ebcb3), and the COFF reader
// lifts it back to hidden — so the DLL's export directory, which already reads the visibility, leaves the runtime out.
// Before that writer/reader pair the DLL exported 31 names (✔MEASURED 2026-10-01). Read with DSS's own PE export
// reader; the runtime is proved inside the DLL by its import table naming none of the runtime's bodies.
//
// Executable witness: examples/c/shared_library_exports_its_own_api_alone (every arm runs on its own host, counting
// a library's exports from its file).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/binary_readers/ar_reader.hpp"
#include "ffi/binary_readers/elf_reader.hpp"
#include "ffi/binary_readers/macho_reader.hpp"
#include "ffi/binary_readers/pe_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::vector<std::uint8_t> readFileBytes(fs::path const& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { ADD_FAILURE() << "cannot open " << path.string(); return {}; }
    auto const end = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return bytes;
}

void writeText(fs::path const& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// A member's unit: two definitions its own unit never references (a function and a datum — DCE's two root tables),
// one it does, and a `static` CONTROL that has no external linkage to restate.
constexpr std::string_view kMemberSource =
    "static int local_scale(int x) { return x * 3; }\n"
    "int unit_helper(int x) { return local_scale(x) + 1; }\n"
    "int unit_table[4] = {1, 2, 3, 4};\n"
    "int unit_api(int x) { return unit_helper(x) + unit_table[x & 3]; }\n";

struct MemberSymbol {
    std::string      name;
    SymbolBinding    binding;
    SymbolVisibility visibility;
};

// Build `kMemberSource` into an ELF x86_64 static library and read its one member back through the object reader the
// static pull uses. nullopt (with a failure recorded) when anything along the way does not hold.
[[nodiscard]] std::optional<std::vector<MemberSymbol>>
buildMember(fs::path const& dir, CompileConfig config, std::optional<SymbolVisibility> archiveVisibility) {
    auto const src = dir / "unit.c";
    writeText(src, kMemberSource);
    Program p;
    p.setOutputDir(dir);
    p.setCompileConfig(config);
    p.setArchiveDefinitionVisibility(archiveVisibility);
    DiagnosticReporter rep;
    if (p.compileFiles({src.string()}, "c", {"x86_64:elf64-x86_64-linux-staticlib"}, rep) != 0) {
        ADD_FAILURE() << "the static library did not build; errors=" << rep.errorCount();
        return std::nullopt;
    }
    auto const bytes = readFileBytes(dir / "unit.a");
    DiagnosticReporter arRep;
    auto const archive = ffi::readArArchive(bytes, "unit.a", arRep);
    if (!archive || archive->members.size() != 1u) {
        ADD_FAILURE() << "unit.a must hold exactly one member";
        return std::nullopt;
    }
    auto const target = TargetSchema::loadShipped("x86_64");
    auto const format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-staticlib");
    if (!target || !format) {
        ADD_FAILURE() << "schema load failed";
        return std::nullopt;
    }
    auto const memberBytes = std::span<std::uint8_t const>{bytes}.subspan(
        archive->members[0].dataOffset, archive->members[0].size);
    DiagnosticReporter memberRep;
    auto const member = elf::readRelocatableObject(memberBytes, **target, **format, memberRep);
    if (!member) {
        ADD_FAILURE() << "the member did not read back; errors=" << memberRep.errorCount();
        return std::nullopt;
    }
    std::vector<MemberSymbol> out;
    for (auto const& ms : member->symbols) out.push_back({ms.name, ms.binding, ms.visibility});
    return out;
}

[[nodiscard]] MemberSymbol const* find(std::vector<MemberSymbol> const& syms, std::string_view name) {
    auto const it = std::find_if(syms.begin(), syms.end(),
                                 [&](MemberSymbol const& s) { return s.name == name; });
    return it == syms.end() ? nullptr : &*it;
}

}  // namespace

TEST(ArchiveDefinitionVisibility, EveryCompiledExternalDefinitionIsRestatedAndNoneIsDeleted) {
    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        SCOPED_TRACE(config == CompileConfig::Release ? "release" : "debug");
        ScratchDir scratch{Location::InsideRepo, "archive-definition-visibility"};
        auto const syms = buildMember(scratch.path(), config, SymbolVisibility::Hidden);
        ASSERT_TRUE(syms.has_value());
        for (std::string_view const name : {"unit_helper", "unit_table", "unit_api"}) {
            auto const* s = find(*syms, name);
            // ★ PRESENT AT RELEASE TOO: the restatement follows the optimizer, so DCE saw Default and kept them.
            ASSERT_NE(s, nullptr) << name << " must still be defined by the member";
            EXPECT_EQ(s->binding, SymbolBinding::Global) << name;
            EXPECT_EQ(s->visibility, SymbolVisibility::Hidden) << name;
        }
        // The `static` control has no external linkage, so there is nothing to restate (a release build may also
        // have inlined it away — absent is as good as Local here).
        if (auto const* s = find(*syms, "local_scale")) {
            EXPECT_EQ(s->binding, SymbolBinding::Local);
        }
    }
}

TEST(ArchiveDefinitionVisibility, WithoutTheDeclarationEveryDefinitionKeepsItsOwn) {
    ScratchDir scratch{Location::InsideRepo, "archive-definition-visibility-control"};
    auto const syms = buildMember(scratch.path(), CompileConfig::Debug, std::nullopt);
    ASSERT_TRUE(syms.has_value());
    for (std::string_view const name : {"unit_helper", "unit_table", "unit_api"}) {
        auto const* s = find(*syms, name);
        ASSERT_NE(s, nullptr) << name;
        EXPECT_EQ(s->visibility, SymbolVisibility::Default) << name;
    }
}

namespace {

// The library half of examples/c/shared_library_exports_its_own_api_alone, verbatim in substance: one function that
// calls runtime bodies (memalignment on every format; strfromd and snprintf DSS's own on pe and Mach-O).
constexpr std::string_view kLibrarySource =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "int lib_entry(void *p) {\n"
    "    char number[32];\n"
    "    strfromd(number, sizeof number, \"%g\", 1.5);\n"
    "    char line[48];\n"
    "    snprintf(line, sizeof line, \"[%s]\", number);\n"
    "    return (int)memalignment(p) + line[1];\n"
    "}\n";

[[nodiscard]] std::uint64_t le(std::vector<std::uint8_t> const& b, std::size_t off, unsigned width) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < width && off + i < b.size(); ++i) v |= std::uint64_t{b[off + i]} << (8u * i);
    return v;
}

[[nodiscard]] std::string cstr(std::vector<std::uint8_t> const& b, std::size_t off) {
    std::string s;
    for (std::size_t i = off; i < b.size() && b[i] != 0; ++i) s.push_back(static_cast<char>(b[i]));
    return s;
}

// ELF: does `.symtab` DEFINE `name` (the runtime body is inside the image)?
[[nodiscard]] bool elfSymtabDefines(std::vector<std::uint8_t> const& b, std::string_view name) {
    auto const shoff = le(b, 0x28, 8);
    auto const shentsize = le(b, 0x3A, 2), shnum = le(b, 0x3C, 2);
    for (std::uint64_t i = 0; i < shnum; ++i) {
        auto const sh = shoff + i * shentsize;
        if (le(b, sh + 4, 4) != 2u) continue;  // SHT_SYMTAB
        auto const symOff = le(b, sh + 0x18, 8), symSize = le(b, sh + 0x20, 8);
        auto const strOff = le(b, shoff + le(b, sh + 0x28, 4) * shentsize + 0x18, 8);
        for (std::uint64_t k = 0; k < symSize / 24u; ++k) {
            auto const s = symOff + 24u * k;
            if (le(b, s + 6, 2) != 0u && cstr(b, strOff + le(b, s, 4)) == name) return true;
        }
    }
    return false;
}

// Mach-O: does the symbol table DEFINE `name` as an N_SECT symbol the image does not export?
[[nodiscard]] bool machoDefinesUnexported(std::vector<std::uint8_t> const& b, std::string_view name) {
    auto const ncmds = le(b, 16, 4);
    std::uint64_t p = 32;
    for (std::uint64_t c = 0; c < ncmds; ++c) {
        auto const cmd = le(b, p, 4), size = le(b, p + 4, 4);
        if (cmd == 0x2u) {  // LC_SYMTAB
            auto const symOff = le(b, p + 8, 4), nsyms = le(b, p + 12, 4), strOff = le(b, p + 16, 4);
            for (std::uint64_t i = 0; i < nsyms; ++i) {
                auto const s = symOff + 16u * i;
                auto const ntype = le(b, s + 4, 1);
                bool const definedHere = (ntype & 0xE0u) == 0u && (ntype & 0x0Eu) == 0x0Eu;
                bool const exported = (ntype & 0x01u) != 0u && (ntype & 0x10u) == 0u;
                if (definedHere && !exported && cstr(b, strOff + le(b, s, 4)) == name) return true;
            }
        }
        if (size == 0u) break;
        p += size;
    }
    return false;
}

// PE32+: every name the image IMPORTS (each import descriptor's by-name thunks). A runtime body the DLL linked is its
// own, so the DLL imports none of them. Records a failure, and answers what it read so far, when an RVA maps to no
// section.
[[nodiscard]] std::vector<std::string> peImportedNames(std::vector<std::uint8_t> const& b) {
    std::vector<std::string> out;
    auto const pe = le(b, 0x3C, 4);
    auto const sectionCount = le(b, pe + 6, 2);
    auto const opt = pe + 24u;
    auto const sections = opt + le(b, pe + 20, 2);
    auto const rvaToOffset = [&](std::uint64_t rva) -> std::optional<std::uint64_t> {
        for (std::uint64_t i = 0; i < sectionCount; ++i) {
            auto const s = sections + 40u * i;
            auto const vsz = le(b, s + 8, 4), va = le(b, s + 12, 4), rsz = le(b, s + 16, 4), raw = le(b, s + 20, 4);
            if (rva >= va && rva < va + std::max(vsz, rsz)) return raw + (rva - va);
        }
        return std::nullopt;
    };
    auto const importRva = le(b, opt + 120, 4);
    if (importRva == 0u) return out;
    auto const directory = rvaToOffset(importRva);
    if (!directory.has_value()) {
        ADD_FAILURE() << "the import directory's RVA maps to no section";
        return out;
    }
    for (std::uint64_t d = *directory; d + 20u <= b.size() && le(b, d + 12, 4) != 0u; d += 20u) {
        auto const thunkRva = le(b, d, 4) != 0u ? le(b, d, 4) : le(b, d + 16, 4);
        auto const thunks = rvaToOffset(thunkRva);
        if (!thunks.has_value()) {
            ADD_FAILURE() << "an import descriptor's thunk RVA maps to no section";
            return out;
        }
        for (std::uint64_t t = *thunks; t + 8u <= b.size() && le(b, t, 8) != 0u; t += 8u) {
            auto const entry = le(b, t, 8);
            if ((entry >> 63) != 0u) continue;  // by ordinal
            auto const hintName = rvaToOffset(entry & 0x7FFFFFFFu);
            if (!hintName.has_value()) {
                ADD_FAILURE() << "an import's hint/name RVA maps to no section";
                return out;
            }
            out.push_back(cstr(b, *hintName + 2u));
        }
    }
    return out;
}

}  // namespace

TEST(ShippedRuntimeVisibility, ASharedLibraryExportsItsOwnFunctionAloneOnElfAndMachO) {
    struct Case {
        char const* spec;
        char const* artifact;
        char const* expectedExport;
        bool        elf;
    };
    for (Case const& c : {Case{"x86_64:elf64-x86_64-linux-dyn", "lib.so", "lib_entry", true},
                          Case{"arm64:elf64-aarch64-linux-dyn", "lib.so", "lib_entry", true},
                          Case{"arm64:macho64-arm64-darwin-dylib", "lib.dylib", "_lib_entry", false},
                          Case{"x86_64:macho64-x86_64-darwin-dylib", "lib.dylib", "_lib_entry", false}}) {
        SCOPED_TRACE(c.spec);
        ScratchDir scratch{Location::InsideRepo, "shipped-runtime-visibility"};
        auto const src = scratch.path() / "lib.c";
        writeText(src, kLibrarySource);
        Program p;
        p.setOutputDir(scratch.path());
        DiagnosticReporter rep;
        ASSERT_EQ(p.compileFiles({src.string()}, "c", {c.spec}, rep), 0) << "errors=" << rep.errorCount();
        auto const bytes = readFileBytes(scratch.path() / c.artifact);
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter readRep;
        auto const surface = c.elf ? ffi::readElf64(bytes, c.artifact, readRep)
                                   : ffi::readMacho(bytes, c.artifact, readRep);
        ASSERT_TRUE(surface.has_value()) << surface.error().detail;
        std::vector<std::string> names;
        for (auto const& row : *surface) names.push_back(row.mangledName);
        std::sort(names.begin(), names.end());
        EXPECT_EQ(names, std::vector<std::string>{c.expectedExport})
            << "a shared library exports its own API; the runtime bodies it links are its implementation";
        // NON-VACUITY: the runtime body is inside this image, not merely absent from it.
        if (c.elf) {
            EXPECT_TRUE(elfSymtabDefines(bytes, "memalignment"))
                << "the image must define the runtime's memalignment (in .symtab, not .dynsym)";
        } else {
            EXPECT_TRUE(machoDefinesUnexported(bytes, "_memalignment"))
                << "the image must define the runtime's _memalignment as a non-exported symbol";
        }
    }
}

// The pe case: the same one declaration reaches the DLL through the COFF member's `.drectve` `-exclude-symbols:`
// directives (the header says how). The reference answer is the ELF and Mach-O one: the library's own function alone.
TEST(ShippedRuntimeVisibility, ADllExportsItsOwnFunctionAloneOnPe) {
    ScratchDir scratch{Location::InsideRepo, "shipped-runtime-visibility-pe"};
    auto const src = scratch.path() / "lib.c";
    writeText(src, kLibrarySource);
    Program p;
    p.setOutputDir(scratch.path());
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileFiles({src.string()}, "c", {"x86_64:pe64-x86_64-windows-dll"}, rep), 0)
        << "errors=" << rep.errorCount();
    auto const bytes = readFileBytes(scratch.path() / "lib.dll");
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter readRep;
    auto const surface = ffi::readPe(bytes, "lib.dll", readRep);
    ASSERT_TRUE(surface.has_value()) << surface.error().detail;
    std::vector<std::string> names;
    for (auto const& row : *surface) names.push_back(row.mangledName);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, std::vector<std::string>{"lib_entry"})
        << "a DLL exports its own API; the runtime bodies it links are its implementation (31 names before the COFF "
           "member carried the declaration)";
    // NON-VACUITY: the runtime bodies are inside this image — the DLL imports its C runtime's cores, and none of the
    // bodies DSS's runtime realizes on pe.
    auto const imported = peImportedNames(bytes);
    EXPECT_FALSE(imported.empty()) << "the DLL imports its C runtime's cores; a parse that finds no import read nothing";
    for (char const* body : {"memalignment", "strfromd"}) {
        EXPECT_EQ(std::count(imported.begin(), imported.end(), std::string{body}), 0)
            << body << " must be the DLL's own body, never an import";
    }
}
