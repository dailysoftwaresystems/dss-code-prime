// D-OPT-DCE-DELETES-A-RELOCATABLE-MEMBERS-HIDDEN-DEFINITIONS — the five faces, each built end to end through the
// driver at the baseline AND at `--config=release`, on every host (ELF artifacts read with DSS's own readers;
// nothing here runs a binary — the examples hidden_definition_across_units_and_entry,
// hidden_definition_in_a_static_library_member and hidden_definition_called_by_an_object_input run them).
//
// ✔MEASURED before the fix (P69, lane lm, the lane's dsscp): (a) a release static-library member lost every hidden
// definition its unit never called; (e) so did a release RELOCATABLE OBJECT (`x86_64:elf64-x86_64-linux`: the
// baseline `.o` defines both hidden definitions and the plain control, the release `.o` the control alone — the
// same `-c` shape the gcc/clang measurement below is); (b) a release image whose CU defines a hidden function only an OBJECT input calls
// failed K_SymbolUndefined; (c) a release image whose CU calls a hidden function a SIBLING CU defines failed
// K_SymbolUndefined (pe64 and ELF alike — `release-unit`, which holds Dce, ran on each CU before the merge); (d) a
// hidden `main` failed its own release link. The baseline built and ran each one. gcc 13.3.0 and clang 18.1.3 keep
// a hidden definition at `-O2 -c` (probe-reference-cc run 20261001-152924-ba09ffc4).
//
// The CONTROL is the lever the fix must keep: a WHOLE image with no foreign input still deletes a hidden definition
// nothing reaches at release (and keeps it at the baseline, whose pipeline is Identity) — so a fix that simply
// stopped DCE deleting hidden definitions is red here.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/binary_readers/ar_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
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

fs::path writeText(fs::path const& dir, std::string_view name, std::string_view text) {
    fs::path const path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << text;
    return path;
}

[[nodiscard]] int compile(fs::path const& outDir, std::vector<std::string> const& sources, std::string const& spec,
                          CompileConfig config, DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    p.setCompileConfig(config);
    return p.compileFiles(sources, "c", {spec}, rep);
}

// Each source its OWN translation unit, merged at link — the shape the CLI routes every multi-source build to
// (`routesToMultiUnit`); `compileFiles` with N sources builds ONE multi-file CU5 unit instead (an API-only surface,
// D-PIPELINE-CU5-MULTIFILE-EXTERN-DATA).
[[nodiscard]] int compileUnits(fs::path const& outDir, std::vector<std::string> const& sources,
                               std::string const& spec, CompileConfig config, DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    p.setCompileConfig(config);
    return p.compileUnits(sources, "c", {spec}, rep);
}

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

// Does an ELF64 image's `.symtab` DEFINE `name`?
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

constexpr CompileConfig kConfigs[] = {CompileConfig::Debug, CompileConfig::Release};

[[nodiscard]] char const* configName(CompileConfig c) { return c == CompileConfig::Release ? "release" : "baseline"; }

}  // namespace

// (a) A static-library member is ONE input of whatever link pulls it: its hidden definitions survive release.
TEST(ReleaseHiddenDefinitions, AStaticLibraryMemberKeepsItsHiddenDefinitions) {
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        ScratchDir scratch{Location::InsideRepo, "release-hidden-member"};
        auto const src = writeText(scratch.path(), "member.c",
            "__attribute__((visibility(\"hidden\"))) int hidden_helper(int x) { return x * 2; }\n"
            "__attribute__((visibility(\"hidden\"))) int hidden_table[4] = {1, 2, 3, 4};\n"
            "int plain_api(int x) { return x + 1; }\n");
        DiagnosticReporter rep;
        ASSERT_EQ(compile(scratch.path(), {src.string()}, "x86_64:elf64-x86_64-linux-staticlib", config, rep), 0)
            << "errors=" << rep.errorCount();
        auto const bytes = readFileBytes(scratch.path() / "member.a");
        DiagnosticReporter arRep;
        auto const archive = ffi::readArArchive(bytes, "member.a", arRep);
        ASSERT_TRUE(archive && archive->members.size() == 1u);
        auto const target = TargetSchema::loadShipped("x86_64");
        auto const format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-staticlib");
        ASSERT_TRUE(target && format);
        auto const memberBytes = std::span<std::uint8_t const>{bytes}.subspan(archive->members[0].dataOffset,
                                                                              archive->members[0].size);
        DiagnosticReporter memberRep;
        auto const member = elf::readRelocatableObject(memberBytes, **target, **format, memberRep);
        ASSERT_TRUE(member.has_value());
        for (std::string_view const name : {"hidden_helper", "hidden_table", "plain_api"}) {
            bool defined = false;
            for (auto const& ms : member->symbols) {
                if (ms.name == name) {
                    defined = true;
                    EXPECT_EQ(ms.binding, SymbolBinding::Global) << name;
                }
            }
            EXPECT_TRUE(defined) << name << " must still be defined by the member";
        }
    }
}

// (e) A relocatable OBJECT is completed by a later link: its hidden definitions survive release too.
TEST(ReleaseHiddenDefinitions, ARelocatableObjectKeepsItsHiddenDefinitions) {
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        ScratchDir scratch{Location::InsideRepo, "release-hidden-object"};
        auto const src = writeText(scratch.path(), "unit.c",
            "__attribute__((visibility(\"hidden\"))) int hidden_helper(int x) { return x * 2; }\n"
            "__attribute__((visibility(\"hidden\"))) int hidden_table[4] = {1, 2, 3, 4};\n"
            "int plain_api(int x) { return x + 1; }\n");
        DiagnosticReporter rep;
        ASSERT_EQ(compile(scratch.path(), {src.string()}, "x86_64:elf64-x86_64-linux", config, rep), 0)
            << "errors=" << rep.errorCount();
        auto const bytes = readFileBytes(scratch.path() / "unit.o");
        auto const target = TargetSchema::loadShipped("x86_64");
        auto const format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
        ASSERT_TRUE(target && format);
        DiagnosticReporter objRep;
        auto const object = elf::readRelocatableObject(bytes, **target, **format, objRep);
        ASSERT_TRUE(object.has_value());
        for (std::string_view const name : {"hidden_helper", "hidden_table", "plain_api"}) {
            bool defined = false;
            for (auto const& ms : object->symbols) {
                if (ms.name == name) defined = true;
            }
            EXPECT_TRUE(defined) << name << " must still be defined by the relocatable object";
        }
    }
}

// (b) An image linked with an OBJECT input: the object calls a hidden function the image's own CU never calls.
TEST(ReleaseHiddenDefinitions, AnImageLinkedWithAnObjectInputKeepsTheHiddenDefinitionTheObjectCalls) {
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        ScratchDir scratch{Location::InsideRepo, "release-hidden-object-input"};
        fs::path const objDir = scratch.path() / "obj";
        fs::create_directories(objDir);
        auto const objSrc = writeText(scratch.path(), "obj.c",
            "extern int hidden_helper(int x);\n"
            "int obj_entry(int x) { return hidden_helper(x) + 1; }\n");
        DiagnosticReporter objRep;
        ASSERT_EQ(compile(objDir, {objSrc.string()}, "x86_64:elf64-x86_64-linux", config, objRep), 0);
        auto const mainSrc = writeText(scratch.path(), "main.c",
            "__attribute__((visibility(\"hidden\"))) int hidden_helper(int x) { return x * 2; }\n"
            "extern int obj_entry(int x);\n"
            "int main(void) { return obj_entry(20) == 41 ? 42 : 1; }\n");
        DiagnosticReporter rep;
        ASSERT_EQ(compile(scratch.path(), {mainSrc.string(), (objDir / "obj.o").string()},
                          "x86_64:elf64-x86_64-linux-exec", config, rep), 0)
            << "the image must link: errors=" << rep.errorCount();
        EXPECT_TRUE(elfSymtabDefines(readFileBytes(scratch.path() / "main"), "hidden_helper"));
    }
}

// (c) Two CUs: a hidden helper and a hidden datum one CU defines and never uses, the other CU calls/reads.
TEST(ReleaseHiddenDefinitions, ASiblingCusHiddenDefinitionsSurviveTheUnitStage) {
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        ScratchDir scratch{Location::InsideRepo, "release-hidden-sibling-cu"};
        auto const a = writeText(scratch.path(), "helper.c",
            "__attribute__((visibility(\"hidden\"))) int hidden_helper(int x) { return x * 2; }\n"
            "__attribute__((visibility(\"hidden\"))) int hidden_table[2] = {1, 1};\n");
        auto const b = writeText(scratch.path(), "main.c",
            "__attribute__((visibility(\"hidden\"))) int hidden_helper(int x);\n"
            "extern int hidden_table[2];\n"
            "int main(void) { return hidden_helper(20) + hidden_table[0] + hidden_table[1] == 42 ? 42 : 1; }\n");
        DiagnosticReporter rep;
        ASSERT_EQ(compileUnits(scratch.path(), {b.string(), a.string()}, "x86_64:elf64-x86_64-linux-exec", config,
                               rep), 0)
            << "the image must link: errors=" << rep.errorCount();
    }
}

// (d) A hidden `main` is still the program's entry: the entry trampoline names it.
TEST(ReleaseHiddenDefinitions, AHiddenMainIsStillTheEntry) {
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        ScratchDir scratch{Location::InsideRepo, "release-hidden-main"};
        auto const src = writeText(scratch.path(), "main.c",
            "__attribute__((visibility(\"hidden\"))) int main(void) { return 42; }\n");
        DiagnosticReporter rep;
        ASSERT_EQ(compile(scratch.path(), {src.string()}, "x86_64:elf64-x86_64-linux-exec", config, rep), 0)
            << "the image must link: errors=" << rep.errorCount();
        EXPECT_TRUE(elfSymtabDefines(readFileBytes(scratch.path() / "main"), "main"));
    }
}

// CONTROL — the lever the fix keeps: a WHOLE image (no object input, no operator archive) deletes a hidden
// definition nothing reaches at release; the baseline (Identity) keeps it.
TEST(ReleaseHiddenDefinitions, AWholeImageStillDeletesAnUnreachedHiddenDefinitionAtRelease) {
    for (CompileConfig const config : kConfigs) {
        SCOPED_TRACE(configName(config));
        ScratchDir scratch{Location::InsideRepo, "release-hidden-whole-image"};
        auto const src = writeText(scratch.path(), "main.c",
            "__attribute__((visibility(\"hidden\"))) int hidden_unused(int x) { return x * 2; }\n"
            "int main(void) { return 42; }\n");
        DiagnosticReporter rep;
        ASSERT_EQ(compile(scratch.path(), {src.string()}, "x86_64:elf64-x86_64-linux-exec", config, rep), 0);
        EXPECT_EQ(elfSymtabDefines(readFileBytes(scratch.path() / "main"), "hidden_unused"),
                  config != CompileConfig::Release)
            << "release: a whole image's unreached hidden definition is dead; baseline: Identity keeps it";
    }
}
