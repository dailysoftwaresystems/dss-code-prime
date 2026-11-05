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
#include "core/types/parse_diagnostic.hpp"   // DiagnosticCode: the merged-image refusal is pinned by its code
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
#include "program/compile_pipeline.hpp"   // extractStaticArchiveMembers, readObjectInputModules: the readers a link uses
#include "repo_root.hpp"
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

// ══ ONE PACKAGING PATH FOR EVERY UNIT (P69 fold H5) ═══════════════════════════════════════════════════════════════
//
// A `container: archive` build packages EVERY unit it compiles as a member -- a unit lowered from MIR (C) and a unit
// assembled from text (the target's assembly dialect, the `encode` tier) alike -- and the archive-wide visibility is
// stated on each. Before H5 the `encode` tier returned through the image/object writer whatever the container:
// ✔MEASURED 2026-11-03, the shipped pe runtime's assembly unit was cached as a bare COFF object (`d\x86`, 476 bytes)
// under an archive's name, every pe link took it as an eagerly-merged object input, a relocatable object of a unit
// that names nothing defined `__chkstk`, and a DSS DLL exported it. THE TABLE IS THE UNIVERSE: every shipped static-
// archive document (x86_64 and arm64; ELF, PE, Mach-O) x every tier a unit can be built by. A document added later
// is a row of `kArchiveDocuments`.
namespace {

struct ArchiveDocument {
    char const* arch;       // the target
    char const* format;     // the `container: archive` document
    char const* assembly;   // the target's assembly dialect (`defaultAssemblyLanguage`)
    char const* asmBody;    // one leaf in that dialect, returning 1
    char const* marker;     // the dialect's function-type marker (`.type <name>, <marker>`)
};
constexpr ArchiveDocument kArchiveDocuments[] = {
    {"x86_64", "elf64-x86_64-linux-staticlib", "asm-x86_64-att", "\tmovl\t$1, %eax\n\tret\n", "@function"},
    {"x86_64", "pe64-x86_64-windows-staticlib", "asm-x86_64-att", "\tmovl\t$1, %eax\n\tret\n", "@function"},
    {"x86_64", "macho64-x86_64-darwin-staticlib", "asm-x86_64-att", "\tmovl\t$1, %eax\n\tret\n", "@function"},
    {"arm64", "elf64-aarch64-linux-staticlib", "asm-arm64-gas", "\tmov\tw0, #1\n\tret\n", "%function"},
    {"arm64", "macho64-arm64-darwin-staticlib", "asm-arm64-gas", "\tmov\tw0, #1\n\tret\n", "%function"},
};

// Every shipped RELOCATABLE-object document (neither an image nor an archive), with its target: the universe of the
// two relocatable cases below. Censused, like `kArchiveDocuments`, by
// `ArchivePackaging.TheTablesHoldEveryShippedArchiveAndRelocatableDocument`.
struct RelocatableDocument {
    char const* arch;     // the target
    char const* format;   // the relocatable document
};
constexpr RelocatableDocument kRelocatableDocuments[] = {
    {"x86_64", "elf64-x86_64-linux"},  {"x86_64", "pe64-x86_64-windows"}, {"x86_64", "macho64-x86_64-darwin"},
    {"arm64", "elf64-aarch64-linux"}, {"arm64", "macho64-arm64-darwin"},
};

// The unit, in C or in the document's assembly dialect: two external functions, `unit_api` and `unit_helper`.
[[nodiscard]] std::pair<std::string, std::string> unitOf(ArchiveDocument const& d, bool assembly) {
    if (!assembly) return {"unit.c", "int unit_api(void) { return 1; }\nint unit_helper(void) { return 1; }\n"};
    std::string s = "\t.text\n";
    for (char const* name : {"unit_api", "unit_helper"}) {
        s += std::string{"\t.globl\t"} + name + "\n\t.type\t" + name + ", " + d.marker + "\n" + name + ":\n"
             + d.asmBody;
    }
    return {"unit.s", s};
}

}  // namespace

// ★ THE TWO TABLES ARE THE SHIPPED VOCABULARY'S, CENSUSED (P69 H6, the review of H5's N4): every shipped object-format
// document a shipped target builds for (its `targetArch` names a `<target>.target.json` of src/dss-config/targets)
// that is a static-archive container is a row of `kArchiveDocuments`, its target and its dialect the target's own
// `defaultAssemblyLanguage`, and every one that is neither an image nor an archive -- a relocatable object -- a row
// of `kRelocatableDocuments` with its target. A document added later fails HERE until its row is written, so no pin's
// universe can fall behind the documents it claims. (spirv-1.6 and wasm32-v1 name targets no shipped target
// document declares, so no C build reaches them: outside both universes, as the census itself reads.)
TEST(ArchivePackaging, TheTablesHoldEveryShippedArchiveAndRelocatableDocument) {
    fs::path const config = dss::test::repoRoot() / "src" / "dss-config";
    std::vector<std::string> targets;
    for (auto const& entry : fs::directory_iterator{config / "targets"}) {
        std::string const file = entry.path().filename().string();
        if (file.ends_with(".target.json")) targets.push_back(file.substr(0, file.size() - std::strlen(".target.json")));
    }
    ASSERT_FALSE(targets.empty()) << "the census read no target document";
    std::vector<std::string> archives;      // "<arch>:<format>"
    std::vector<std::string> relocatables;  // "<arch>:<format>"
    std::size_t              read = 0;
    for (auto const& entry : fs::directory_iterator{config / "object-formats"}) {
        std::string const file = entry.path().filename().string();
        if (!file.ends_with(".format.json")) continue;
        std::string const name   = file.substr(0, file.size() - std::strlen(".format.json"));
        auto const        format = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(format.has_value()) << name << " did not load";
        ++read;
        std::string const arch{(*format)->targetArch()};
        if (std::find(targets.begin(), targets.end(), arch) == targets.end()) continue;   // no C build reaches it
        if ((*format)->isStaticArchive()) archives.push_back(arch + ":" + name);
        else if (!(*format)->isImageFlavor()) relocatables.push_back(arch + ":" + name);
    }
    ASSERT_FALSE(archives.empty() || relocatables.empty()) << "the census found no document of a kind; it read " << read;
    std::vector<std::string> archiveRows;
    for (ArchiveDocument const& d : kArchiveDocuments) {
        archiveRows.push_back(std::string{d.arch} + ":" + d.format);
        auto const target = TargetSchema::loadShipped(d.arch);
        ASSERT_TRUE(target.has_value()) << d.arch;
        EXPECT_EQ((*target)->defaultAssemblyLanguage(), std::string_view{d.assembly})
            << d.format << ": the row's dialect is not its target's own";
    }
    std::vector<std::string> relocatableRows;
    for (RelocatableDocument const& r : kRelocatableDocuments) relocatableRows.push_back(std::string{r.arch} + ":" + r.format);
    for (auto* v : {&archives, &relocatables, &archiveRows, &relocatableRows}) std::sort(v->begin(), v->end());
    EXPECT_EQ(archiveRows, archives) << "a shipped static-archive document has no row of kArchiveDocuments, or a row "
                                        "names no such document or the wrong target";
    EXPECT_EQ(relocatableRows, relocatables) << "a shipped relocatable document has no row of kRelocatableDocuments, "
                                                "or a row names no such document or the wrong target";
}

TEST(ArchivePackaging, EveryTierPackagesItsUnitAsAMemberWithTheArchivesVisibility) {
    for (ArchiveDocument const& d : kArchiveDocuments) {
        for (bool const assembly : {false, true}) {
            for (std::optional<SymbolVisibility> const declared :
                 {std::optional<SymbolVisibility>{SymbolVisibility::Hidden}, std::optional<SymbolVisibility>{}}) {
                SCOPED_TRACE(std::string{d.format} + (assembly ? " / assembly" : " / C")
                             + (declared ? " / declared hidden" : " / no declaration"));
                ScratchDir scratch{Location::InsideRepo, "archive-packaging"};
                auto const [file, text] = unitOf(d, assembly);
                writeText(scratch.path() / file, text);
                Program p;
                p.setOutputDir(scratch.path());
                p.setArchiveDefinitionVisibility(declared);
                DiagnosticReporter rep;
                ASSERT_EQ(p.compileFiles({(scratch.path() / file).string()}, assembly ? d.assembly : "c",
                                         {std::string{d.arch} + ":" + d.format}, rep),
                          0)
                    << "errors=" << rep.errorCount();
                ASSERT_EQ(p.artifactPaths().size(), 1u);
                ASSERT_TRUE(p.artifactPaths().front().has_value());
                fs::path const artifact = *p.artifactPaths().front();
                auto const bytes = readFileBytes(artifact);
                ASSERT_GE(bytes.size(), 8u);
                EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 8), std::string{"!<arch>\n"})
                    << "a static-archive document writes an `ar` archive, never a bare object under its name";
                auto const target = TargetSchema::loadShipped(d.arch);
                auto const format = ObjectFormatSchema::loadShipped(d.format);
                ASSERT_TRUE(target.has_value() && format.has_value());
                DiagnosticReporter readRep;
                auto const members = extractStaticArchiveMembers(std::span<fs::path const>{&artifact, 1}, **target,
                                                                 **format, readRep);
                ASSERT_TRUE(members.has_value()) << "the archive did not read back; errors=" << readRep.errorCount();
                ASSERT_EQ(members->modules.size(), 1u) << "one unit, one member";
                EXPECT_EQ(members->names.front(), "unit" + std::string{(*format)->archiveMemberExtension()})
                    << "the member is named after the unit's own source";
                std::size_t defined = 0;
                for (auto const& ms : members->modules.front().symbols) {
                    if (ms.binding == SymbolBinding::Local) continue;
                    bool const ours = ms.name.ends_with("unit_api") || ms.name.ends_with("unit_helper");
                    if (!ours) continue;
                    ++defined;
                    EXPECT_EQ(ms.visibility, declared.value_or(SymbolVisibility::Default))
                        << ms.name << ": the archive-wide visibility is stated on every unit's definitions";
                }
                EXPECT_EQ(defined, 2u) << "the member defines the unit's two functions (a read that found none read "
                                          "nothing)";
            }
        }
    }
}

// A RELOCATABLE object of a unit that names no runtime routine carries no runtime body: its external definitions
// are the unit's own, on every shipped relocatable document. ✔MEASURED before H5 (compile-and-keep run
// 20261104-012600-85de7399): the pe object of `int plain_api(int x)` defined `__chkstk`, `__security_cookie`,
// `__main` and every other routine of the pe `compilerSupport` role, and two such objects linked together redefined
// them all.
TEST(ArchivePackaging, ARelocatableObjectOfAUnitThatNamesNoRoutineDefinesItsOwnNamesAlone) {
    for (RelocatableDocument const& r : kRelocatableDocuments) {
        SCOPED_TRACE(r.format);
        ScratchDir scratch{Location::InsideRepo, "archive-packaging-relocatable"};
        writeText(scratch.path() / "plain.c", "int plain_api(int x) { return x + 42; }\n");
        Program p;
        p.setOutputDir(scratch.path());
        DiagnosticReporter rep;
        ASSERT_EQ(p.compileFiles({(scratch.path() / "plain.c").string()}, "c", {std::string{r.arch} + ":" + r.format},
                                 rep),
                  0)
            << "errors=" << rep.errorCount();
        ASSERT_EQ(p.artifactPaths().size(), 1u);
        ASSERT_TRUE(p.artifactPaths().front().has_value());
        fs::path const object = *p.artifactPaths().front();
        auto const target = TargetSchema::loadShipped(r.arch);
        auto const format = ObjectFormatSchema::loadShipped(r.format);
        ASSERT_TRUE(target.has_value() && format.has_value());
        DiagnosticReporter readRep;
        auto const modules = readObjectInputModules(std::span<fs::path const>{&object, 1}, **target, **format, readRep);
        ASSERT_TRUE(modules.has_value() && modules->size() == 1u) << "errors=" << readRep.errorCount();
        std::vector<std::string> defined;
        for (auto const& ms : modules->front().symbols) {
            if (ms.binding != SymbolBinding::Local) defined.push_back(ms.name);
        }
        std::string named;
        for (std::string const& d : defined) named += " " + d;
        ASSERT_EQ(defined.size(), 1u) << "the object defines its unit's one function and nothing a runtime ships; it "
                                         "defines" << named;
        EXPECT_TRUE(defined.front().ends_with("plain_api")) << defined.front();
    }
}

// A RELOCATABLE object of a unit that CALLS a routine the shipped runtime defines leaves the routine a REFERENCE, as
// gcc's and clang's `-c` leave `memalignment` and cl's `/c` its `__chkstk`: a relocatable artifact defers "who owns
// this name" to the later link (`allowsUndefinedImports`), and that link -- an image build -- supplies the runtime. A
// body pulled into the object would be defined again by every other object that calls it, and by the image's own pull
// (K_SymbolRedefinedAcrossUnits). THE TABLE IS THE UNIVERSE: every shipped relocatable document.
TEST(ArchivePackaging, ARelocatableObjectOfAUnitThatCallsARuntimeRoutineLeavesItAReference) {
    for (RelocatableDocument const& r : kRelocatableDocuments) {
        SCOPED_TRACE(r.format);
        ScratchDir scratch{Location::InsideRepo, "archive-packaging-runtime-reference"};
        writeText(scratch.path() / "calls.c",
                  "#include <stdlib.h>\nsize_t uses_runtime(void *p) { return memalignment(p); }\n");
        Program p;
        p.setOutputDir(scratch.path());
        DiagnosticReporter rep;
        ASSERT_EQ(p.compileFiles({(scratch.path() / "calls.c").string()}, "c", {std::string{r.arch} + ":" + r.format},
                                 rep),
                  0)
            << "errors=" << rep.errorCount();
        ASSERT_EQ(p.artifactPaths().size(), 1u);
        ASSERT_TRUE(p.artifactPaths().front().has_value());
        fs::path const object = *p.artifactPaths().front();
        auto const target = TargetSchema::loadShipped(r.arch);
        auto const format = ObjectFormatSchema::loadShipped(r.format);
        ASSERT_TRUE(target.has_value() && format.has_value());
        DiagnosticReporter readRep;
        auto const modules = readObjectInputModules(std::span<fs::path const>{&object, 1}, **target, **format, readRep);
        ASSERT_TRUE(modules.has_value() && modules->size() == 1u) << "errors=" << readRep.errorCount();
        std::vector<std::string> defined;
        for (auto const& ms : modules->front().symbols) {
            if (ms.binding != SymbolBinding::Local) defined.push_back(ms.name);
        }
        bool const referenced = std::any_of(modules->front().externImports.begin(), modules->front().externImports.end(),
                                            [](auto const& e) { return e.mangledName.ends_with("memalignment"); });
        EXPECT_TRUE(referenced) << "the object does not name `memalignment` as a reference";
        std::string named;
        for (std::string const& d : defined) named += " " + d;
        ASSERT_EQ(defined.size(), 1u) << "a relocatable object defines its unit's function alone, never a runtime body "
                                         "it calls; it defines" << named;
        EXPECT_TRUE(defined.front().ends_with("uses_runtime")) << defined.front();
    }
}

// SEVERAL UNITS INTO ONE ARCHIVE, IN EITHER LANGUAGE: one member each, named after its own source, defining its own
// function alone -- the table's units axis (P69 H6, the review of H5's F4). A member is never merged with another, so
// nothing about several units needs a merged symbol space; the assembly route refused them all the same
// (D_PlanNotLanded, "a multi-CU standalone-assembly build is not yet lowered") where `as` + `ar`, `ml64` + `lib` and
// `gcc -c a.s b.s` + `ar rcs` build it. Two units as the CLI makes them (`dsscp a.s b.s`: `compileUnits`).
TEST(ArchivePackaging, SeveralUnitsArePackagedAsOneMemberEachInEitherLanguage) {
    for (ArchiveDocument const& d : kArchiveDocuments) {
        for (bool const assembly : {false, true}) {
            SCOPED_TRACE(std::string{d.format} + (assembly ? " / assembly" : " / C"));
            ScratchDir scratch{Location::InsideRepo, "archive-units"};
            std::vector<std::string> sources;
            for (char const* stem : {"first", "second"}) {
                std::string const fn = std::string{stem} + "_api";
                std::string text;
                if (assembly) {
                    text = std::string{"\t.text\n\t.globl\t"} + fn + "\n\t.type\t" + fn + ", " + d.marker + "\n" + fn
                         + ":\n" + d.asmBody;
                } else {
                    text = "int " + fn + "(void) { return 1; }\n";
                }
                fs::path const file = scratch.path() / (std::string{stem} + (assembly ? ".s" : ".c"));
                writeText(file, text);
                sources.push_back(file.string());
            }
            Program p;
            p.setOutputDir(scratch.path());
            DiagnosticReporter rep;
            std::string said;
            int const rc = p.compileUnits(sources, assembly ? d.assembly : "c", {std::string{d.arch} + ":" + d.format},
                                          rep);
            for (auto const& diag : rep.all()) said += "\n  " + diag.actual;
            ASSERT_EQ(rc, 0) << "two units did not build into the archive; errors=" << rep.errorCount() << said;
            ASSERT_EQ(p.artifactPaths().size(), 1u);
            ASSERT_TRUE(p.artifactPaths().front().has_value());
            fs::path const artifact = *p.artifactPaths().front();
            auto const bytes = readFileBytes(artifact);
            ASSERT_GE(bytes.size(), 8u);
            EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 8), std::string{"!<arch>\n"});
            auto const target = TargetSchema::loadShipped(d.arch);
            auto const format = ObjectFormatSchema::loadShipped(d.format);
            ASSERT_TRUE(target.has_value() && format.has_value());
            DiagnosticReporter readRep;
            auto const members = extractStaticArchiveMembers(std::span<fs::path const>{&artifact, 1}, **target,
                                                             **format, readRep);
            ASSERT_TRUE(members.has_value()) << "the archive did not read back; errors=" << readRep.errorCount();
            ASSERT_EQ(members->modules.size(), 2u) << "two units, two members";
            std::string const ext{(*format)->archiveMemberExtension()};
            for (std::size_t i = 0; i < 2; ++i) {
                std::string const stem = i == 0 ? "first" : "second";
                EXPECT_EQ(members->names[i], stem + ext) << "the member is named after its own unit's source";
                std::vector<std::string> defined;
                for (auto const& ms : members->modules[i].symbols) {
                    if (ms.binding != SymbolBinding::Local && (ms.name.ends_with("first_api")
                                                               || ms.name.ends_with("second_api")))
                        defined.push_back(ms.name);
                }
                ASSERT_EQ(defined.size(), 1u) << stem << "'s member defines one of the two functions";
                EXPECT_TRUE(defined.front().ends_with(stem + "_api")) << defined.front();
            }
        }
    }
}

// ...AND INTO A MERGED IMAGE THEY ARE STILL REFUSED, BY NAME. The archive's several units are members, never merged;
// an image MERGES its units, and the assembly route has no namespacing of one unit's symbols from another's, so two
// `.s` units into an executable stay D_PlanNotLanded (its text now names the merged image). Each archive document's
// executable sibling, so the refusal is pinned on every target and format family the archive cell covers. (The ONE-
// unit assembly archive is `EveryTierPackagesItsUnitAsAMemberWithTheArchivesVisibility`'s assembly cell.)
TEST(ArchivePackaging, SeveralAssemblyUnitsIntoAMergedImageAreStillRefusedByName) {
    for (ArchiveDocument const& d : kArchiveDocuments) {
        std::string exec{d.format};
        ASSERT_TRUE(exec.ends_with("-staticlib")) << exec;
        exec.replace(exec.size() - std::string_view{"-staticlib"}.size(), std::string_view{"-staticlib"}.size(),
                     "-exec");
        SCOPED_TRACE(exec);
        ScratchDir scratch{Location::InsideRepo, "asm-units-image"};
        std::vector<std::string> sources;
        for (char const* stem : {"first", "second"}) {
            std::string const fn = std::string{stem} + "_api";
            fs::path const file = scratch.path() / (std::string{stem} + ".s");
            writeText(file, std::string{"\t.text\n\t.globl\t"} + fn + "\n" + fn + ":\n" + d.asmBody);
            sources.push_back(file.string());
        }
        Program p;
        p.setOutputDir(scratch.path());
        DiagnosticReporter rep;
        EXPECT_NE(p.compileUnits(sources, d.assembly, {std::string{d.arch} + ":" + exec}, rep), 0)
            << "two assembly units were linked into one image with no namespacing of their symbols";
        bool refused = false;
        for (auto const& diag : rep.all()) {
            if (diag.code == DiagnosticCode::D_PlanNotLanded && diag.actual.find("into a merged image") != std::string::npos)
                refused = true;
        }
        EXPECT_TRUE(refused) << "the merged image was not refused by D_PlanNotLanded naming it; errors="
                             << rep.errorCount();
    }
}
