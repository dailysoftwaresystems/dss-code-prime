// P69 fold 3d (lane `xa`) — A SHARED LIBRARY THAT HOLDS ONLY DATA IS WRITTEN WITH ITS DATA ALONE, AND A PROGRAM
// WITH NO INSTRUCTION IS REFUSED AS A PROGRAM WITH NO ENTRY.
//
// [[D-LK-A-DATA-ONLY-SHARED-LIBRARY-IS-REFUSED-AS-AN-EXEC-WITH-NO-INSTRUCTIONS]]: `int dss_library_value = 42;` alone,
// built as a library, was refused on all three object formats as if it were an executable with no instructions —
// K_RelocationKindMismatch "an exec with no instructions would crash at entry" on PE, K_SymbolUndefined on ELF and
// Mach-O. Whether an image may hold no instruction is its document's entry declaration now (the entry verbs; a
// library's document declares none), asked by every image writer through one function,
// `link::format::anImageThatStartsAProgramHoldsAnInstruction`.
//
// ✔MEASURED, the references, each library of one exported datum and no function, its datum read through an import by
// a program that exits 42:
//   * link.exe 14.44 `/DLL /NOENTRY`: `.rdata` and `.data`, no code section, SizeOfCode 0, AddressOfEntryPoint 0,
//     BaseOfCode 0x1000 (cl /LD without /NOENTRY and MinGW-w64 gcc -shared add their C runtime's code);
//   * gcc 13.3 + GNU ld and clang 18.1.3 + ld.lld, `-shared -nostdlib`: e_entry 0, no executable PT_LOAD (GNU ld: R,
//     RW; ld.lld: R, RW, RW, with an empty `.text`);
//   * Apple clang 21 + ld-1267 `-dynamiclib`: a 0-byte `__text` in an r-x `__TEXT`, the `__data`, LC_ID_DYLIB, no
//     LC_MAIN (`-dynamiclib -nostdlib` is refused: "dynamic executables or dylibs must link with libSystem.dylib").
// DSS writes each: no PE code section, no executable ELF segment (and an empty `.text`), and a 0-byte Mach-O
// `__text`.
//
// The in-process cells run on every leg. The `…Native` cells build for the host's own library and executable
// documents and run the program there, and read the library back with the host's reference dump tool (dumpbin,
// readelf, otool); on Linux and macOS the reference compiler's program reads the datum too. The PE references'
// programs read a data-only DSS DLL in `tests/link/test_pe_dllimport_declaration_link.cpp`.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "host_native_target.hpp"
#include "link_facts.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kLibraryUnit = "int dss_library_value = 42;\n";
constexpr char const* kReaderUnit  = "extern int dss_library_value;\nint main(void) { return dss_library_value; }\n";
constexpr std::string_view kDatum  = "dss_library_value";

struct LibraryDocument {
    char const* target;  // the CLI's `<arch>:<format>`
    char const* file;    // the artifact DSS writes for the stem `dsslib`
};
constexpr LibraryDocument kLibraryDocuments[] = {
    {"x86_64:pe64-x86_64-windows-dll", "dsslib.dll"},
    {"x86_64:elf64-x86_64-linux-dyn", "dsslib.so"},
    {"arm64:elf64-aarch64-linux-dyn", "dsslib.so"},
    {"arm64:macho64-arm64-darwin-dylib", "dsslib.dylib"},
    {"x86_64:macho64-x86_64-darwin-dylib", "dsslib.dylib"},
};

struct ProgramDocument {
    char const* target;
    char const* format;
};
constexpr ProgramDocument kProgramDocuments[] = {
    {"x86_64", "pe64-x86_64-windows-exec"},     {"x86_64", "elf64-x86_64-linux-exec"},
    {"x86_64", "elf64-x86_64-linux-pie"},       {"arm64", "elf64-aarch64-linux-exec"},
    {"arm64", "elf64-aarch64-linux-pie"},       {"arm64", "macho64-arm64-darwin-exec"},
    {"x86_64", "macho64-x86_64-darwin-exec"},
};

void writeText(fs::path const& p, std::string_view text) { std::ofstream(p, std::ios::binary) << text; }

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string textOf(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return out;
}

[[nodiscard]] std::string_view formatOf(std::string_view target) { return target.substr(target.find(':') + 1); }

// Little-endian reads that answer 0 past the end, so a reader states what it read rather than crashing.
[[nodiscard]] std::uint64_t le(std::vector<std::uint8_t> const& b, std::size_t at, int width) {
    std::uint64_t v = 0;
    for (int i = width - 1; i >= 0; --i) {
        v = (v << 8) | (at + static_cast<std::size_t>(i) < b.size() ? b[at + static_cast<std::size_t>(i)] : 0u);
    }
    return v;
}
[[nodiscard]] std::string cStringAt(std::vector<std::uint8_t> const& b, std::size_t at) {
    std::string s;
    while (at < b.size() && b[at] != 0) s += static_cast<char>(b[at++]);
    return s;
}

// ── each format's shape, read from the image's bytes ─────────────────────────────────────────────────────────────

// PE32+: no section is code (IMAGE_SCN_CNT_CODE) or executable (IMAGE_SCN_MEM_EXECUTE), SizeOfCode and
// AddressOfEntryPoint are 0, and the export directory names the datum.
void expectAPeImageOfTheDatumAlone(std::vector<std::uint8_t> const& b, std::string const& who) {
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') {
        ADD_FAILURE() << who << ": not a PE image";
        return;
    }
    std::size_t const pe = le(b, 0x3c, 4);
    std::size_t const oh = pe + 24;
    if (le(b, oh, 2) != 0x20b) {
        ADD_FAILURE() << who << ": not PE32+";
        return;
    }
    EXPECT_EQ(le(b, oh + 4, 4), 0u) << who << ": SizeOfCode";
    EXPECT_EQ(le(b, oh + 16, 4), 0u) << who << ": AddressOfEntryPoint";
    std::size_t const sections = oh + le(b, pe + 20, 2);
    std::size_t const count    = le(b, pe + 6, 2);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t const s = sections + 40u * i;
        std::string       name;  // eight bytes, NUL-padded
        for (std::size_t j = 0; j < 8 && s + j < b.size() && b[s + j] != 0; ++j) name += static_cast<char>(b[s + j]);
        std::uint64_t const chars = le(b, s + 36, 4);
        EXPECT_EQ(chars & 0x20000020u, 0u) << who << ": section '" << name << "' is code or executable";
    }
    auto const offsetOf = [&](std::uint64_t rva) -> std::size_t {
        for (std::size_t i = 0; i < count; ++i) {
            std::size_t const   s    = sections + 40u * i;
            std::uint64_t const va   = le(b, s + 12, 4);
            std::uint64_t const span = std::max(le(b, s + 8, 4), le(b, s + 16, 4));
            if (rva >= va && rva < va + span) return static_cast<std::size_t>(le(b, s + 20, 4) + (rva - va));
        }
        return b.size();
    };
    std::uint64_t const exportRva = le(b, oh + 112, 4);
    if (exportRva == 0) {
        ADD_FAILURE() << who << ": no export directory";
        return;
    }
    std::size_t const dir   = offsetOf(exportRva);
    std::uint64_t const n   = le(b, dir + 24, 4);
    std::size_t const names = offsetOf(le(b, dir + 32, 4));
    bool exported = false;
    for (std::uint64_t i = 0; i < n; ++i) {
        exported = exported || cStringAt(b, offsetOf(le(b, names + 4 * i, 4))) == kDatum;
    }
    EXPECT_TRUE(exported) << who << ": the export directory does not name " << kDatum;
}

// ELF64: e_entry is 0, no PT_LOAD is executable (PF_X), and `.dynsym` names the datum as an object.
void expectAnElfImageOfTheDatumAlone(std::vector<std::uint8_t> const& b, std::string const& who) {
    if (b.size() < 64 || b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' || b[4] != 2) {
        ADD_FAILURE() << who << ": not an ELF64 image";
        return;
    }
    EXPECT_EQ(le(b, 0x10, 2), 3u) << who << ": e_type is not ET_DYN";
    EXPECT_EQ(le(b, 0x18, 8), 0u) << who << ": e_entry";
    std::size_t const phoff = le(b, 0x20, 8);
    std::size_t const phent = le(b, 0x36, 2);
    std::size_t const phnum = le(b, 0x38, 2);
    std::size_t loads = 0;
    for (std::size_t i = 0; i < phnum; ++i) {
        std::size_t const p = phoff + phent * i;
        if (le(b, p, 4) != 1) continue;  // PT_LOAD
        ++loads;
        EXPECT_EQ(le(b, p + 4, 4) & 1u, 0u) << who << ": PT_LOAD #" << loads << " is executable";
    }
    EXPECT_GT(loads, 0u) << who << ": no PT_LOAD";
    std::size_t const shoff = le(b, 0x28, 8);
    std::size_t const shent = le(b, 0x3a, 2);
    std::size_t const shnum = le(b, 0x3c, 2);
    bool named = false;
    for (std::size_t i = 0; i < shnum; ++i) {
        std::size_t const s = shoff + shent * i;
        if (le(b, s + 4, 4) != 11) continue;  // SHT_DYNSYM
        std::size_t const strtab = shoff + shent * le(b, s + 40, 4);
        std::size_t const strOff = le(b, strtab + 24, 8);
        std::size_t const off    = le(b, s + 24, 8);
        std::size_t const size   = le(b, s + 32, 8);
        for (std::size_t e = off; e + 24 <= off + size; e += 24) {
            named = named
                 || (cStringAt(b, strOff + le(b, e, 4)) == kDatum && (b[e + 4] & 0xfu) == 1u /* STT_OBJECT */);
        }
    }
    EXPECT_TRUE(named) << who << ": `.dynsym` does not name " << kDatum << " as an object";
}

// Mach-O 64: an MH_DYLIB with no LC_MAIN, a `__text` of 0 bytes, and an export trie that holds the datum.
void expectAMachoImageOfTheDatumAlone(std::vector<std::uint8_t> const& b, std::string const& who) {
    if (b.size() < 32 || le(b, 0, 4) != 0xfeedfacfu) {
        ADD_FAILURE() << who << ": not a 64-bit Mach-O image";
        return;
    }
    EXPECT_EQ(le(b, 12, 4), 6u) << who << ": filetype is not MH_DYLIB";
    std::size_t const ncmds = le(b, 16, 4);
    std::size_t at          = 32;
    std::optional<std::uint64_t> textSize;
    std::optional<std::pair<std::size_t, std::size_t>> trie;
    for (std::size_t c = 0; c < ncmds && at + 8 <= b.size(); ++c) {
        std::uint64_t const cmd  = le(b, at, 4);
        std::size_t const   size = le(b, at + 4, 4);
        EXPECT_NE(cmd, 0x80000028u) << who << ": an LC_MAIN";
        if (cmd == 0x19) {  // LC_SEGMENT_64
            std::size_t const nsects = le(b, at + 64, 4);
            for (std::size_t s = 0; s < nsects; ++s) {
                std::size_t const sect = at + 72 + 80 * s;
                if (cStringAt(b, sect) == "__text") textSize = le(b, sect + 40, 8);
            }
        }
        if (cmd == 0x80000022u) trie = {le(b, at + 40, 4), le(b, at + 44, 4)};  // LC_DYLD_INFO_ONLY's export_off/size
        if (cmd == 0x80000033u) trie = {le(b, at + 8, 4), le(b, at + 12, 4)};   // LC_DYLD_EXPORTS_TRIE
        if (size == 0) break;
        at += size;
    }
    ASSERT_TRUE(textSize.has_value()) << who << ": no `__text`";
    EXPECT_EQ(*textSize, 0u) << who << ": `__text` holds bytes";
    ASSERT_TRUE(trie.has_value() && trie->second != 0) << who << ": no export trie";
    std::string const bytes(b.begin() + static_cast<std::ptrdiff_t>(trie->first),
                            b.begin() + static_cast<std::ptrdiff_t>(std::min(b.size(), trie->first + trie->second)));
    EXPECT_NE(bytes.find("_" + std::string{kDatum}), std::string::npos) << who << ": the export trie lacks the datum";
}

void expectTheDatumAlone(ObjectFormatKind kind, std::vector<std::uint8_t> const& b, std::string const& who) {
    switch (kind) {
        case ObjectFormatKind::Pe:    expectAPeImageOfTheDatumAlone(b, who); return;
        case ObjectFormatKind::Elf:   expectAnElfImageOfTheDatumAlone(b, who); return;
        case ObjectFormatKind::MachO: expectAMachoImageOfTheDatumAlone(b, who); return;
        default: ADD_FAILURE() << who << ": no reader for this format kind";
    }
}

// DSS builds `<dir>/<file>` from the library unit; the path, or empty with the failure recorded.
[[nodiscard]] fs::path dssLibrary(fs::path const& dir, std::string const& target, std::string const& file) {
    fs::create_directories(dir);
    writeText(dir / "dsslib.c", kLibraryUnit);
    Program p;
    p.setOutputDir(dir);
    DiagnosticReporter rep;
    int const rc = p.compileFiles(std::vector<std::string>{(dir / "dsslib.c").string()}, "c",
                                  std::vector<std::string>{target}, rep);
    if (rc != 0 || !fs::exists(dir / file)) {
        ADD_FAILURE() << target << ": DSS built no " << file << " (rc " << rc << ")" << diagnosticsOf(rep);
        return {};
    }
    return dir / file;
}

// One cell: the library of `doc`, built and read.
void libraryCell(fs::path const& dir, LibraryDocument const& doc) {
    fs::path const lib = dssLibrary(dir, doc.target, doc.file);
    if (lib.empty()) return;
    auto const& format = test_support::shippedFormatOrThrow(formatOf(doc.target));
    expectTheDatumAlone(format.kind(), bytesOf(lib), doc.target);
}

// The datum alone, handed straight to the link: one data item, its name, no function.
[[nodiscard]] AssembledModule aModuleOfTheDatumAlone() {
    AssembledModule mod;
    mod.cuId = CompilationUnitId{1};
    AssembledData d;
    d.symbol    = SymbolId{1};
    d.section   = DataSectionKind::Data;
    d.bytes     = {42, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, std::string{kDatum}, SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// One cell: the program document `doc` is handed the datum alone and refuses it as a program with no entry, under
// that code only.
void programCell(ProgramDocument const& doc) {
    auto const target = TargetSchema::loadShipped(doc.target);
    auto const format = ObjectFormatSchema::loadShipped(doc.format);
    if (!target.has_value() || !format.has_value()) {
        ADD_FAILURE() << doc.format << ": the shipped documents do not load";
        return;
    }
    DiagnosticReporter rep;
    LinkedImage const img = linker::link(aModuleOfTheDatumAlone(), **target, **format, rep,
                                         ImageRequest{.artifactFileName = "data_only_program"});
    EXPECT_FALSE(img.ok()) << doc.format << ": a program with no instruction was written";
    bool entryUndefined = false;
    for (auto const& d : rep.all()) {
        entryUndefined = entryUndefined
                      || (d.code == DiagnosticCode::K_ProgramEntryUndefined
                          && d.actual.find(doc.format) != std::string::npos);
        EXPECT_NE(d.code, DiagnosticCode::K_RelocationKindMismatch) << doc.format << ": " << d.actual;
        EXPECT_NE(d.code, DiagnosticCode::K_SymbolUndefined) << doc.format << ": " << d.actual;
    }
    EXPECT_TRUE(entryUndefined) << doc.format
                                << ": the refusal must be a program with no entry, naming the document"
                                << diagnosticsOf(rep);
}

// The host's documents.
struct HostDocuments {
    std::string libTarget, execTarget, libFile, exeFile;
};
[[nodiscard]] HostDocuments hostDocuments() {
    auto const h = test_support::hostNativeTarget();
    return {std::string{h.libTarget}, std::string{h.execTarget}, test_support::hostLibArtifact("dsslib"),
            test_support::hostExeArtifact("reader")};
}

[[nodiscard]] std::optional<std::string> originSpelling(std::string_view execTarget) {
    auto const& rp = test_support::shippedFormatOrThrow(formatOf(execTarget)).runpath();
    if (!rp.has_value()) return std::nullopt;
    return rp->origin;
}

// The tokens of `line`, split at blanks.
[[nodiscard]] std::vector<std::string> tokensOf(std::string const& line) {
    std::vector<std::string> out;
    std::istringstream in{line};
    for (std::string t; in >> t;) out.push_back(t);
    return out;
}

// dumpbin /headers, read as link.exe's `/DLL /NOENTRY` image reads (✔MEASURED, dumpbin 14.44 on it): "File Type:
// DLL", "0 size of code", "0 entry point", and no section flagged "Code" or "Execute …".
void expectDumpbinReadsTheDatumAlone(std::string const& text) {
    EXPECT_NE(text.find("File Type: DLL"), std::string::npos) << text;
    bool noCode = false, noEntry = false;
    std::istringstream in{text};
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto const t = tokensOf(line);
        if (t == std::vector<std::string>{"0", "size", "of", "code"}) noCode = true;
        if (t == std::vector<std::string>{"0", "entry", "point"}) noEntry = true;
        EXPECT_FALSE(t.size() == 1 && t[0] == "Code") << "dumpbin reads a code section:\n" << text;
        EXPECT_FALSE(!t.empty() && t[0] == "Execute") << "dumpbin reads an executable section:\n" << text;
    }
    EXPECT_TRUE(noCode) << "dumpbin does not read 0 size of code:\n" << text;
    EXPECT_TRUE(noEntry) << "dumpbin does not read 0 entry point:\n" << text;
}

// readelf -lW: every LOAD line's flags (the tokens between MemSiz and Align) hold no E.
void expectReadelfReadsNoExecutableSegment(std::string const& text) {
    std::size_t loads = 0;
    std::istringstream in{text};
    for (std::string line; std::getline(in, line);) {
        auto const t = tokensOf(line);
        if (t.size() < 8 || t[0] != "LOAD") continue;
        ++loads;
        for (std::size_t i = 6; i + 1 < t.size(); ++i) {
            EXPECT_EQ(t[i].find('E'), std::string::npos) << "readelf reads an executable LOAD:\n" << text;
        }
    }
    EXPECT_GT(loads, 0u) << "readelf reads no LOAD:\n" << text;
}

// otool -l: `__text`'s size is 0, and no LC_MAIN; otool -hv: the file type is DYLIB.
void expectOtoolReadsTheDatumAlone(std::string const& loadCommands, std::string const& header) {
    std::istringstream in{loadCommands};
    bool inText = false, textRead = false;
    for (std::string line; std::getline(in, line);) {
        auto const t = tokensOf(line);
        EXPECT_FALSE(t.size() == 2 && t[0] == "cmd" && t[1] == "LC_MAIN") << "otool reads an LC_MAIN";
        if (t.size() == 2 && t[0] == "sectname") inText = t[1] == "__text";
        if (inText && t.size() == 2 && t[0] == "size") {
            EXPECT_EQ(std::stoull(t[1], nullptr, 16), 0u) << "otool reads bytes in `__text`";
            textRead = true;
            inText   = false;
        }
    }
    EXPECT_TRUE(textRead) << "otool reads no `__text`:\n" << loadCommands;
    EXPECT_NE(header.find("DYLIB"), std::string::npos) << header;
}

}  // namespace

// ══ IN-PROCESS, EVERY LEG ════════════════════════════════════════════════════════════════════════════════════════

// Every library document DSS ships writes the datum alone: the C unit `int dss_library_value = 42;` builds, and its
// image is the references' shape (PE: no code section, SizeOfCode and AddressOfEntryPoint 0; ELF: ET_DYN, e_entry 0,
// no executable PT_LOAD; Mach-O: MH_DYLIB, no LC_MAIN, a 0-byte `__text`) and exports the datum.
// RED-ON-DISABLE: refuse every image with no instruction (the writers before fold 3d) and no library builds; write
// a PE code section, or map the ELF segment executable, and that document's reading reddens.
TEST(DataOnlySharedLibrary, EachLibraryDocumentWritesTheDatumAlone) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "data-only-library"};
    std::size_t k = 0;
    for (LibraryDocument const& doc : kLibraryDocuments) {
        SCOPED_TRACE(doc.target);
        libraryCell(scratch.path() / std::to_string(k++), doc);
    }
}

// Every program document refuses the datum alone as a program with no entry (K_ProgramEntryUndefined, naming the
// document), never under the codes the writers used to give it.
// RED-ON-DISABLE: let an image of a program-starting document hold no instruction, and none of these refuses so.
TEST(DataOnlySharedLibrary, AProgramWithNoInstructionIsRefusedAsAProgramWithNoEntry) {
    for (ProgramDocument const& doc : kProgramDocuments) {
        SCOPED_TRACE(doc.format);
        programCell(doc);
    }
}

// ══ NATIVE ═══════════════════════════════════════════════════════════════════════════════════════════════════════

// The host's own library document: DSS builds the library of the datum alone, then a program that reads it through
// an import, and the program exits 42.
TEST(DataOnlySharedLibraryNative, ADssProgramReadsTheDatumOfALibraryOfDataAlone) {
    auto const host = hostDocuments();
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "data-only-library-dss-run"};
    auto const dir = scratch.path();
    fs::path const lib = dssLibrary(dir, host.libTarget, host.libFile);
    ASSERT_FALSE(lib.empty());
    writeText(dir / "reader.c", kReaderUnit);
    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<fs::path>{lib});
    if (auto const origin = originSpelling(host.execTarget); origin.has_value()) {
        p.setRunpaths(std::vector<std::string>{"${ORIGIN}"});
    }
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileFiles(std::vector<std::string>{(dir / "reader.c").string()}, "c",
                             std::vector<std::string>{host.execTarget}, rep),
              0)
        << "DSS must build the program that reads the datum" << diagnosticsOf(rep);
    auto const r = test_support::runBinary(dir / host.exeFile);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u) << "the read did not reach the datum the library exports";
}

// The host's reference tools read DSS's library of the datum alone as they read their own linker's: dumpbin on
// Windows, readelf on Linux, otool on macOS; and on Linux and macOS the reference compiler's program, linked against
// DSS's library, reads the datum and exits 42.
TEST(DataOnlySharedLibraryNative, TheReferenceToolsReadTheLibraryAsTheirOwnLinkersShape) {
    auto const host = hostDocuments();
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "data-only-library-reference"};
    auto const dir = scratch.path();
    fs::path const lib = dssLibrary(dir, host.libTarget, host.libFile);
    ASSERT_FALSE(lib.empty());
    auto const kind = test_support::shippedFormatOrThrow(formatOf(host.libTarget)).kind();
#if defined(_WIN32)
    ASSERT_EQ(kind, ObjectFormatKind::Pe);
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    ASSERT_EQ(test_support::native_probe::systemUnder(
                  tools.env, "\"cd /d \"" + dir.string() + "\" && dumpbin /nologo /headers \"" + lib.string()
                                 + "\" > dumpbin.txt 2>&1\""),
              0)
        << "dumpbin must read the DLL";
    expectDumpbinReadsTheDatumAlone(textOf(dir / "dumpbin.txt"));
#else
    auto const sh = [&](std::string const& cmd, char const* out) {
        return std::system((cmd + " > \"" + (dir / out).string() + "\" 2>&1").c_str());
    };
    if (kind == ObjectFormatKind::Elf) {
        ASSERT_EQ(sh("readelf -lW \"" + lib.string() + "\"", "readelf.txt"), 0) << textOf(dir / "readelf.txt");
        expectReadelfReadsNoExecutableSegment(textOf(dir / "readelf.txt"));
    } else {
        ASSERT_EQ(kind, ObjectFormatKind::MachO);
        ASSERT_EQ(sh("otool -l \"" + lib.string() + "\"", "otool-l.txt"), 0) << textOf(dir / "otool-l.txt");
        ASSERT_EQ(sh("otool -hv \"" + lib.string() + "\"", "otool-hv.txt"), 0) << textOf(dir / "otool-hv.txt");
        expectOtoolReadsTheDatumAlone(textOf(dir / "otool-l.txt"), textOf(dir / "otool-hv.txt"));
    }
    auto const located = test_support::native_probe::findCompiler(dir);
    if (located.status == test_support::native_probe::ProbeStatus::ToolAbsent) GTEST_SKIP() << located.detail;
    ASSERT_EQ(located.status, test_support::native_probe::ProbeStatus::Ok) << located.detail;
    auto const origin = originSpelling(host.execTarget);
    ASSERT_TRUE(origin.has_value()) << host.execTarget << " declares no runpath";
    writeText(dir / "reader.c", kReaderUnit);
    fs::path const exe = dir / "reader_reference";
    ASSERT_EQ(sh(located.compiler->buildCmd(dir / "reader.c", exe) + " \"" + lib.string() + "\" '-Wl,-rpath," + *origin
                     + "'",
                 "reference-build.txt"),
              0)
        << "the reference compiler must link its program against DSS's library:\n"
        << textOf(dir / "reference-build.txt");
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u) << "the reference program's read did not reach the datum DSS's library exports";
#endif
}
