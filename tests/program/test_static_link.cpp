// c165 (D-LK-STATIC-LINK) -- the STATIC-LINK end-to-end witness. The FINALE of
// the static-lib arc: DSS WRITES a `.a` (c163), READS the armap (c161) + a
// relocatable member's body (c164), and here WIRES them into an actual static
// link -- on an unresolved extern, pull the defining member out of the archive
// and MERGE its code INTO the output image (a self-contained executable, no
// runtime DT_NEEDED for the archive's symbols).
//
// THE DESIGN (proven here): static-linking IS the c154 cross-CU merge, fed from
// archive members instead of sibling CUs. `main`'s `extern int dss_lib_answer`
// binds to the pulled `lib.o`'s definition EXACTLY as it binds a sibling
// translation unit's definition. The driver surface is the c162
// `--resolve-library` flag EXTENDED: an `ar`-magic file routes to the static
// pull+merge; a `.so`/`.dll`/`.dylib` stays on the dynamic export-reader path
// (dispatch by MAGIC BYTES, agnostic -- never a `.a` extension).
//
// This is an INTEGRATION TEST (not an examples/ corpus entry) for the same
// reason c162's round-trip is: the examples_runner is single-artifact-per-target
// and cannot express a two-artifact DEPENDENT build (build the `.a` as artifact
// 1, then static-link `main` against it) -- the D-EXAMPLES-RUNNER-MULTI-ARTIFACT
// limitation, reusing c162's decision.
//
// Cross-platform pins (run everywhere) exercise the pull + merge STRUCTURALLY:
// the reference is bound to the pulled member's definition (stripped from the
// import table); the lazy-pull leaves an unreferenced member unpulled. The ELF
// RUN witness (exit 42 + red-on-disable) is __linux__-gated (ubuntu CI + local
// WSL) -- the run needs a host that executes an ELF exec.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "asm/asm.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "diagnostic_count.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "ffi/binary_readers/ar_reader.hpp"
#include "link/format/ar.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "core/substrate/phase_timers.hpp"
#include "program/program.hpp"
#include "program/runtime_object_cache.hpp"   // resolveArchiveSiblingFormat
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include "../link/gcc_section_relative_c167.inc"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

// D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY: the pull's
// `dynamicLibraries` argument. Almost every case in this file names no
// `--resolve-library` binary at all, so it passes an EMPTY span and the pull
// behaves exactly as it did before that parameter existed -- the argument is
// spelled rather than defaulted so a route that needs one cannot forget it (see
// the header's contract). `ArchiveMemberResolveLibraryImport` is the suite that
// passes a real one.
constexpr std::span<ResolveLibrarySpec const> kNoDynamicLibraries{};

namespace {

// lib.c defines the answer; main.c declares it `extern` (a bare prototype, no
// import library -> a cross-TU reference the linker resolves) and returns it.
constexpr std::string_view kLibSrc =
    "int dss_lib_answer(void){ return 42; }\n";
constexpr std::string_view kMainSrc =
    "extern int dss_lib_answer(void);\n"
    "int main(void){ return dss_lib_answer(); }\n";

fs::path writeSrc(fs::path const& dir, std::string_view name,
                  std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p);
    f << text;
    return p;
}

// The RELOCATABLE ELF format (ET_REL) -- what an `ar` member is. The EXEC format
// -- what `main` links to. Both x86_64 ELF; the member is written ET_REL and
// read back during the exec link.
struct Schemas {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> reloc;   // ET_REL member format
    std::shared_ptr<ObjectFormatSchema> exec;    // ET_EXEC link target
    // The `container: "archive"` format -- what the DRIVER writes a static
    // library with, and therefore the vocabulary a member's bytes are IN.
    // Loaded lazily by `loadStaticlibSchema` below: only the
    // D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT pins
    // need it, and every other case here predates the axis.
    std::shared_ptr<ObjectFormatSchema> staticlib;
    std::shared_ptr<GrammarSchema const> grammar;
};

// The Mach-O **x86_64** sibling.
//
// ⚠ ITS ORIGINAL REASON FOR EXISTING IS GONE, AND THAT IS RECORDED RATHER THAN
// QUIETLY EDITED AWAY. It was introduced as the family whose object and image
// relocation vocabularies "genuinely CONFLICT" -- kind 1 declared as 620756992
// in `macho64-x86_64-darwin{,-staticlib}` and 369098752 in `-exec`/`-dylib` --
// making it the one family that could tell a member read through the OBJECT
// vocabulary apart from one read through the IMAGE's. That conflict was not a
// design difference: the image value was a TYPO that contradicted its own row's
// name and its own stated packing, and correcting it made all four documents
// agree (D-CONFIG-MACHO-X86_64-EXEC-DYLIB-RELOC-NATIVEID-CONTRADICTS-ITS-OWN-ROW).
// The family is kept because it is still a real leg with a real writer
// round trip; what it can no longer do is discriminate two vocabularies, and no
// case here asks it to.
[[nodiscard]] Schemas loadMachoX86Schemas() {
    Schemas s;
    auto t = TargetSchema::loadShipped("x86_64");
    auto r = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    auto e = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    auto l = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-staticlib");
    auto g = GrammarSchema::loadShipped("c");
    if (!t || !r || !e || !l || !g) {
        ADD_FAILURE() << "macho x86_64 schema load failed";
        return s;
    }
    s.target    = std::move(t).value();
    s.reloc     = std::move(r).value();
    s.exec      = std::move(e).value();
    s.staticlib = std::move(l).value();
    s.grammar   = std::move(g).value();
    return s;
}

// Attach the `-staticlib` variant to an already-loaded family.
void loadStaticlibSchema(Schemas& s, std::string_view name) {
    auto l = ObjectFormatSchema::loadShipped(name);
    if (!l) { ADD_FAILURE() << "staticlib schema load failed: " << name; return; }
    s.staticlib = std::move(l).value();
}

[[nodiscard]] Schemas loadSchemas() {
    Schemas s;
    auto t = TargetSchema::loadShipped("x86_64");
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    auto e = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    auto g = GrammarSchema::loadShipped("c");
    if (!t || !r || !e || !g) { ADD_FAILURE() << "schema load failed"; return s; }
    s.target = std::move(t).value();
    s.reloc  = std::move(r).value();
    s.exec   = std::move(e).value();
    s.grammar = std::move(g).value();
    return s;
}

// The Mach-O sibling of loadSchemas (arm64): the MH_OBJECT reloc format for
// the `.a` members + the MH_EXECUTE exec format for the client. Used by the
// c168 Mach-O static-link witness (the pull dispatches to the c168 Mach-O
// object reader, the merge binds the reference exactly as the ELF path does).
[[nodiscard]] Schemas loadMachoSchemas() {
    Schemas s;
    auto t = TargetSchema::loadShipped("arm64");
    auto r = ObjectFormatSchema::loadShipped("macho64-arm64-darwin");
    auto e = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    auto g = GrammarSchema::loadShipped("c");
    if (!t || !r || !e || !g) { ADD_FAILURE() << "macho schema load failed"; return s; }
    s.target = std::move(t).value();
    s.reloc  = std::move(r).value();
    s.exec   = std::move(e).value();
    s.grammar = std::move(g).value();
    return s;
}

// The Windows COFF sibling (x86_64): the `.obj` reloc format for the `.lib`
// members + the PE `.exe` exec format for the client. The c170 pull dispatches
// to the COFF object reader; the RUN executes NATIVELY on a Windows host.
[[nodiscard]] Schemas loadCoffSchemas() {
    Schemas s;
    auto t = TargetSchema::loadShipped("x86_64");
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    auto e = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    auto g = GrammarSchema::loadShipped("c");
    if (!t || !r || !e || !g) { ADD_FAILURE() << "coff schema load failed"; return s; }
    s.target = std::move(t).value();
    s.reloc  = std::move(r).value();
    s.exec   = std::move(e).value();
    s.grammar = std::move(g).value();
    return s;
}

[[nodiscard]] std::uint16_t ccIndexFor(TargetSchema const& target,
                                       ObjectFormatSchema const& format,
                                       DiagnosticReporter& rep) {
    auto const abi = dss::ffi::resolveAbi(target, format, rep);
    if (!abi || abi->cc == nullptr) { ADD_FAILURE() << "resolveAbi failed"; return 0; }
    auto const span = target.callingConventions();
    return static_cast<std::uint16_t>(std::distance(span.data(), abi->cc));
}

// Compile one c source string to an AssembledModule for `format`.
[[nodiscard]] std::optional<AssembledModule>
assembleFromSource(std::string src, std::string label, Schemas const& s,
                   ObjectFormatSchema const& format, DiagnosticReporter& rep) {
    UnitBuilder builder{s.grammar, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), std::move(label));
    CompilationUnit cu = std::move(builder).finish();
    std::uint16_t const cc = ccIndexFor(*s.target, format, rep);
    return assembleUnit(cu, *s.grammar, *s.target, format, cc, rep, CompileOptions{DiagnosticBudget::libraryDefault()});
}

// DSS writes a `.a` from N (source, memberName) pairs: assemble each source to a
// RELOCATABLE (ET_REL) member, then bundle via the c163 writer. Returns the
// archive path (asserts on any failure).
[[nodiscard]] fs::path
buildArchive(fs::path const& dir, std::string_view archiveName,
             std::vector<std::pair<std::string, std::string>> const& members,
             Schemas const& s) {
    std::vector<AssembledModule> mods;
    std::vector<std::string>     names;
    for (auto const& [src, memberName] : members) {
        DiagnosticReporter rep;
        auto mod = assembleFromSource(src, memberName + ".c", s, *s.reloc, rep);
        if (!mod) { ADD_FAILURE() << "assemble member '" << memberName
                                  << "' failed; errs=" << rep.errorCount(); return {}; }
        mods.push_back(std::move(*mod));
        names.push_back(memberName);
    }
    auto const archivePath = dir / std::string{archiveName};
    DiagnosticReporter rep;
    bool const ok = linkAndWriteStaticArchive(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        std::span<std::string const>{names.data(), names.size()},
        *s.target, *s.reloc, archivePath, rep);
    if (!ok) { ADD_FAILURE() << "linkAndWriteStaticArchive failed; errs="
                             << rep.errorCount(); return {}; }
    return archivePath;
}

// DSS writes a `.a`/`.lib` THE WAY THE DRIVER DOES: every member assembled for,
// and the archive written through, the `container: "archive"` format. The older
// `buildArchive` above writes through the bare relocatable schema instead;
// both are DSS's own writer, but only this one reproduces the byte stream a
// `--target <t>:<base>-staticlib` build actually emits, which is the stream the
// D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT pins are
// about.
[[nodiscard]] fs::path
buildArchiveThroughStaticlibFormat(
    fs::path const& dir, std::string_view archiveName,
    std::vector<std::pair<std::string, std::string>> const& members,
    Schemas const& s) {
    if (!s.staticlib) { ADD_FAILURE() << "no staticlib schema loaded"; return {}; }
    std::vector<AssembledModule> mods;
    std::vector<std::string>     names;
    for (auto const& [src, memberName] : members) {
        DiagnosticReporter rep;
        auto mod = assembleFromSource(src, memberName + ".c", s, *s.staticlib, rep);
        if (!mod) { ADD_FAILURE() << "assemble member '" << memberName
                                  << "' failed; errs=" << rep.errorCount(); return {}; }
        mods.push_back(std::move(*mod));
        names.push_back(memberName);
    }
    auto const archivePath = dir / std::string{archiveName};
    DiagnosticReporter rep;
    if (!linkAndWriteStaticArchive(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            std::span<std::string const>{names.data(), names.size()},
            *s.target, *s.staticlib, archivePath, rep)) {
        ADD_FAILURE() << "linkAndWriteStaticArchive failed; errs="
                      << rep.errorCount();
        return {};
    }
    return archivePath;
}

[[nodiscard]] bool importsContain(std::vector<std::string> const& names,
                                  std::string_view symbol) {
    return std::any_of(names.begin(), names.end(),
                       [&](std::string const& n) { return n == symbol; });
}

[[nodiscard]] bool moduleDefinesExternallyVisible(AssembledModule const& mod,
                                                  std::string_view symbol) {
    return std::any_of(mod.symbols.begin(), mod.symbols.end(),
        [&](ModuleSymbol const& ms) {
            return ms.name == symbol
                && isExternallyVisible(ms.binding, ms.visibility);
        });
}

// Read a file's whole contents into a byte vector (for reading a driver-emitted
// `.a`/`.lib` back off disk). Seek-to-end sizing keeps the read binary-exact.
[[nodiscard]] std::vector<std::uint8_t> readFileBytes(fs::path const& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { ADD_FAILURE() << "cannot open " << path.string(); return {}; }
    auto const end = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        f.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    }
    return bytes;
}

// The raw bytes of the FIRST real object member of an `ar` archive on disk.
[[nodiscard]] std::vector<std::uint8_t>
firstMemberBytes(fs::path const& archivePath, std::vector<std::uint8_t>& owner) {
    owner = readFileBytes(archivePath);
    DiagnosticReporter rep;
    auto arch = dss::ffi::readArArchive(
        std::span<std::uint8_t const>{owner.data(), owner.size()},
        archivePath.filename().string(), rep);
    if (!arch || arch->members.empty()) {
        ADD_FAILURE() << "archive did not parse or has no members: "
                      << archivePath.string();
        return {};
    }
    auto const& m = arch->members.front();
    return std::vector<std::uint8_t>(
        owner.begin() + static_cast<std::ptrdiff_t>(m.dataOffset),
        owner.begin() + static_cast<std::ptrdiff_t>(m.dataOffset)
                      + static_cast<std::ptrdiff_t>(m.size));
}

// Structurally walk `ar` member headers and count the "/" LINKER-INDEX members
// (name field exactly "/"): a SysV `.a` carries 1 (the armap); a COFF `.lib`
// carries 2 (the SysV BE armap + the Microsoft LE 2nd linker member -- the c169
// flavor threading). Returns -1 on a bad global magic. Header layout: name(16)
// ... size@+48(10 ASCII-decimal) ... "`\n"(2) = 60; an odd payload is followed
// by ONE '\n' pad byte external to `size` (the universal 2-byte ar alignment).
[[nodiscard]] int countArSlashLinkerMembers(std::span<std::uint8_t const> bytes) {
    if (bytes.size() < 8
        || std::string_view{reinterpret_cast<char const*>(bytes.data()), 8}
               != "!<arch>\n") {
        return -1;
    }
    int count = 0;
    std::size_t off = 8;
    while (off + 60 <= bytes.size()) {
        std::string name{reinterpret_cast<char const*>(bytes.data() + off), 16};
        auto const last = name.find_last_not_of(' ');
        name = (last == std::string::npos) ? std::string{} : name.substr(0, last + 1);
        std::string const sizeField{
            reinterpret_cast<char const*>(bytes.data() + off + 48), 10};
        std::uint64_t size = 0;
        for (char c : sizeField) {
            if (c >= '0' && c <= '9') size = size * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (name == "/") ++count;
        off += 60 + size + (size & 1);
    }
    return count;
}

}  // namespace

// -- Dispatch: ar magic vs dynamic (isArArchiveFile) ----------------------------
//
// The `--resolve-library` dispatch keys on the 8-byte `ar` global magic, NEVER a
// `.a`/`.lib` extension. A DSS-written archive is detected; a non-ar file (even
// named `.a`) and a nonexistent path are not (the latter stays on the dynamic
// path, whose eager open-probe fails it loud -- never a silent drop).
TEST(StaticLink, ArMagicDispatchByBytesNotExtension) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive =
        buildArchive(dir, "libdsslib.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(archive.empty());
    EXPECT_TRUE(isArArchiveFile(archive))
        << "a DSS-written .a must be detected by its !<arch> magic";

    // A file with a `.a` NAME but NOT ar content is NOT an archive (magic, not
    // extension).
    auto const fakeArchive = writeSrc(dir, "not_really.a", "this is not an archive");
    EXPECT_FALSE(isArArchiveFile(fakeArchive))
        << "extension must not fool the dispatch -- content decides";

    // An ELF object (the reloc member on disk would have ELF magic, not ar) and a
    // nonexistent path are both not-ar.
    EXPECT_FALSE(isArArchiveFile(dir / "does_not_exist.a"))
        << "a nonexistent path is not-ar (stays dynamic; eager probe fails it loud)";
}

// -- Structural pull + merge (cross-platform W1 + red-on-disable) ----------------
//
// The pull resolves the referenced member; the merge binds `main`'s
// `dss_lib_answer` reference to the pulled definition and STRIPS the import
// (self-contained -- the definition is IN the image, not a runtime import).
// RED-ON-DISABLE: linking `main` ALONE (no pulled member) leaves `dss_lib_answer`
// an unresolved import -- the exact state the static pull removes.
TEST(StaticLink, PullResolvesReferenceAndMergeStripsImport) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive =
        buildArchive(dir, "libdsslib.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(archive.empty());

    // Assemble main.c (references extern dss_lib_answer) for the EXEC target.
    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s,
                                      *s.exec, mainRep);
    ASSERT_TRUE(mainMod) << "main assemble failed; errs=" << mainRep.errorCount();
    // main carries dss_lib_answer as an unresolved extern import (bare prototype).
    ASSERT_TRUE(std::any_of(mainMod->externImports.begin(),
                            mainMod->externImports.end(),
                            [](ExternImport const& e){ return e.mangledName == "dss_lib_answer"; }))
        << "main must reference dss_lib_answer as an extern import";

    // Pull the archive members that satisfy main's externs.
    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, kNoDynamicLibraries, *s.target,
                                           *s.exec, pullRep);
    ASSERT_TRUE(pulled) << "pull failed; errs=" << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u) << "exactly the one member defining dss_lib_answer";
    EXPECT_TRUE(moduleDefinesExternallyVisible((*pulled)[0], "dss_lib_answer"))
        << "the pulled member must define dss_lib_answer";
    EXPECT_EQ(pullRep.errorCount(), 0u);

    // MERGE + link the combined span: dss_lib_answer binds to the pulled def and
    // is STRIPPED from the import table (the cross-CU reference resolution).
    std::vector<AssembledModule> combined;
    combined.push_back(*mainMod);
    combined.push_back(std::move((*pulled)[0]));
    DiagnosticReporter linkRep;
    auto image = linker::link(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        *s.target, *s.exec, linkRep);
    EXPECT_EQ(linkRep.errorCount(), 0u) << "merged static link must be clean";
    EXPECT_TRUE(image.ok());
    EXPECT_FALSE(importsContain(image.externImportNames, "dss_lib_answer"))
        << "the merge must STRIP dss_lib_answer (bound to the pulled definition, "
           "not a runtime import) -- the self-containedness pin";
    EXPECT_EQ(image.resolvedCrossCuRefs.size(), 1u)
        << "the reference->definition binding must be recorded";

    // RED-ON-DISABLE: WITHOUT the pulled member, dss_lib_answer is UNRESOLVED --
    // the exact state the static pull removes.
    //
    // ⓘ UCRT-P4 (Decision 1) CHANGED HOW "UNRESOLVED" LOOKS, AND FOR THE BETTER.
    // This used to assert the name appeared in `externImportNames`: the retired
    // per-language `externLibraryByFormat` default gave every unbound extern an
    // invented import library, so a missing definition silently became a DYNAMIC
    // IMPORT of a library that does not export it -- a clean link and a failure at
    // LOAD. With the guess gone the row carries no library, so it is not an import
    // at all; it is an unresolved reference, and an exec-flavour link REJECTS it
    // LOUD. Assert THAT, which is the property the pull actually removes.
    DiagnosticReporter aloneRep;
    auto imageAlone = linker::link(
        std::span<AssembledModule const>{&*mainMod, 1}, *s.target, *s.exec, aloneRep);
    EXPECT_TRUE(aloneRep.hasErrors())
        << "without the static pull, the reference to dss_lib_answer must be "
           "rejected LOUD at link -- never quietly turned into an import of a "
           "library that has no such export";
    EXPECT_EQ(imageAlone.resolvedCrossCuRefs.size(), 0u)
        << "and nothing may claim to have resolved it";
}

// -- Two-pass lazy-pull: only REFERENCED members are pulled (W2) -----------------
//
// A 2-member archive: member a.o defines `used_answer`, member b.o defines
// `unused_symbol`. A client referencing ONLY used_answer pulls a.o and leaves
// b.o unpulled -- lazy, not whole-archive. PIN: b.o's symbol is absent from the
// pulled set.
TEST(StaticLink, LazyPullSkipsUnreferencedMember) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive = buildArchive(dir, "libtwo.a", {
        {"int used_answer(void){ return 42; }\n",   "a.o"},
        {"int unused_symbol(void){ return 7; }\n",  "b.o"},
    }, s);
    ASSERT_FALSE(archive.empty());

    // The client references ONLY used_answer.
    DiagnosticReporter cliRep;
    auto clientMod = assembleFromSource(
        "extern int used_answer(void);\n"
        "int main(void){ return used_answer(); }\n", "client.c", s, *s.exec, cliRep);
    ASSERT_TRUE(clientMod) << "client assemble failed; errs=" << cliRep.errorCount();

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(*clientMod, archives, kNoDynamicLibraries, *s.target,
                                           *s.exec, pullRep);
    ASSERT_TRUE(pulled) << "pull failed; errs=" << pullRep.errorCount();

    // EXACTLY one member pulled -- a.o. b.o (unused_symbol) is NEVER pulled.
    ASSERT_EQ(pulled->size(), 1u) << "only the referenced member a.o is pulled";
    EXPECT_TRUE(moduleDefinesExternallyVisible((*pulled)[0], "used_answer"));
    for (auto const& mod : *pulled) {
        EXPECT_FALSE(moduleDefinesExternallyVisible(mod, "unused_symbol"))
            << "member b.o must NOT be pulled -- its symbol must be absent (lazy)";
    }
}

// -- End-to-end via the production driver (Program::compileFiles) ----------------
//
// The `--resolve-library <archive.a>` surface: DSS static-links `main` against
// the DSS-written `.a` through the real driver. The BUILD runs on every host
// (cross-compile to ELF); the RUN + red-on-disable are __linux__-gated.
TEST(StaticLink, DriverStaticLinkBuildsSelfContainedExec) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive =
        buildArchive(dir, "libdsslib.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(archive.empty());
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    // Static-link main against libdsslib.a via the driver.
    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<fs::path>{archive});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{mainSrc.string()}, "c",
        std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"}, rep);
    ASSERT_EQ(rc, 0) << "static-link build must succeed; errs=" << rep.errorCount();
    auto const mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath)) << "the self-contained main exec must exist";

#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    // RUN (x86_64 Linux host): the artifacts are x86_64:elf64-x86_64-linux-exec,
    // so the run needs an x86_64 Linux host. On the ubuntu-ARM64 leg this is
    // compiled out (an x86_64 ELF is ENOEXEC there); the BUILD above still runs
    // there (cross-compile structural coverage). aarch64-NATIVE static-link
    // runtime coverage is a named follow-up (D-LK-STATIC-LINK-AARCH64-RUNTIME):
    // c164/c165 support aarch64, but it needs local qemu validation + the CLI
    // .a-request surface (D-FF1-AR-STATICLIB-DRIVER-WIRING) to land honestly.
    // The pulled dss_lib_answer body is IN the exe -> exit 42. No
    // LD_LIBRARY_PATH needed (that is the self-containedness).
    auto const r = runBinary(mainPath);
    ASSERT_TRUE(r.spawned) << "main must spawn. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_lib_answer() pulled from "
           "libdsslib.a, merged into main, and called.";

    // RED-ON-DISABLE: WITHOUT --resolve-library, `dss_lib_answer` is an
    // UNRESOLVED REFERENCE, and an exec-flavour link now REJECTS it LOUD — so
    // the BUILD FAILS. Assert that, which is the property the static pull
    // actually removes — the same migration `PullResolvesReferenceAndMergeStripsImport`
    // above already made. (Cited BY TEST NAME, not by line: an earlier draft of
    // this comment said "352-362 and ~620" and the second number had already
    // drifted to 636 before it was ever committed. D-PLANS-LINE-CITATION-ROT.)
    //
    // ★ THE OLD EXPECTATION WAS STRICTLY WEAKER AND IS WHY THIS CHANGED: it
    // asserted the build SUCCEEDED and merely that the RUN did not reach 42 —
    // i.e. it accepted a clean link that died at LOAD with a symbol-lookup
    // error. Retiring the per-language `externLibraryByFormat` default turned
    // that into a compile-time refusal, which is the better outcome.
    //
    // ★★ WHY IT WAS MISSED, RECORDED BECAUSE THE SHAPE RECURS: that retirement
    // changed this property at THREE sites in this file — the two
    // `*PullResolvesReferenceAndMergeStripsImport` tests were migrated with the
    // change; this one was not. The Windows leg reported 816/816 while WSL was
    // red, because this arm lives inside the
    // `#if defined(__linux__) && (__x86_64__ || __amd64__)` guard and Windows
    // CANNOT COMPILE IT AT ALL. A green suite over a SUBSET of a multi-site
    // contract is not proof of the contract.
    // ⚠ AND THE SAME BLIND SPOT IS STILL OPEN ONE TEST BELOW:
    // `MachODriverStaticLinkBuildsSelfContainedExec` is guarded by
    // `#if defined(__APPLE__) && defined(__aarch64__)`, which NEITHER gate leg
    // compiles — so it could be arbitrarily stale and no leg would say so.
    // Tracked as D-TEST-PLATFORM-GUARDED-ARM-COMPILES-ON-NO-GATE-LEG.
    ScratchDir scratchNo{Location::InsideRepo, "static-link"};
    auto const dirNo = scratchNo.path();
    auto const mainNo = writeSrc(dirNo, "main.c", kMainSrc);
    Program pNo;
    pNo.setOutputDir(dirNo);
    DiagnosticReporter repNo;
    EXPECT_NE(pNo.compileFiles(std::vector<std::string>{mainNo.string()}, "c",
                  std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"}, repNo), 0)
        << "without the static pull, the unresolved reference to dss_lib_answer "
           "must FAIL THE BUILD -- never quietly become a dynamic import of a "
           "library that has no such export";
    EXPECT_TRUE(repNo.hasErrors())
        << "and that failure must be a REPORTED diagnostic, not a bare nonzero "
           "status with nothing said";
#endif  // __linux__
}

// -- c167: REAL gcc `.a` with SECTION-RELATIVE relocs (the decisive witness) -----
//
// The prior tests static-link DSS-assembled members, which use NAMED-symbol
// relocations. A REAL gcc `.o` references string literals / jump tables through a
// SECTION symbol + addend, and packs anonymous content (no symbol) into `.rodata`
// -- exactly what c164's reader could not link until c167. Here a genuine `gcc -c`
// object (a `switch` jump table computing 42; embedded byte-for-byte) is bundled
// into a DSS-written `.a` and static-linked through the production driver. The RUN
// proves the WHOLE chain end-to-end: the anonymous jump table is reconstructed as
// a synthetic gap atom, the `.rela.text` lea refs redirect to it, the 6
// `.rela.rodata` entries redirect to lib_answer's INTERIOR, the merge binds it all
// into the exec, and it executes to 42. Red-on-disable is structural: revert the
// section-relative resolution and the reader fails loud on the jump table -> the
// static link fails (rc != 0) -> the exec never builds.

namespace {
// Bundle RAW object bytes (a real gcc `.o`) into a DSS-written `.a` via the c163
// ar writer -- hermetic (no gcc/ar at test time; the golden is embedded).
[[nodiscard]] fs::path
writeGoldenArchive(fs::path const& dir, std::string_view archiveName,
                   std::string_view memberName, std::vector<std::uint8_t> objectBytes,
                   std::vector<std::string> exportedSymbols) {
    dss::link::format::ArMemberInput member;
    member.name            = std::string{memberName};
    member.objectBytes     = std::move(objectBytes);
    member.exportedSymbols = std::move(exportedSymbols);
    std::vector<dss::link::format::ArMemberInput> const members{std::move(member)};
    DiagnosticReporter rep;
    auto const bytes = dss::link::format::writeArArchive(
        std::span<dss::link::format::ArMemberInput const>{members.data(), members.size()},
        rep);
    if (bytes.empty()) {
        ADD_FAILURE() << "writeArArchive failed; errs=" << rep.errorCount();
        return {};
    }
    auto const path = dir / std::string{archiveName};
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<char const*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return path;
}
}  // namespace

TEST(StaticLink, RealGccSectionRelativeJumpTableLibExitsFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();

    auto const archive = writeGoldenArchive(
        dir, "libanswer.a", "answer.o",
        dss::test::gccAnswerJumpTableObject(), {"lib_pad", "lib_answer"});
    ASSERT_FALSE(archive.empty());
    EXPECT_TRUE(isArArchiveFile(archive)) << "the bundled golden must be a valid ar archive";

    auto const mainSrc = writeSrc(dir, "main.c",
        "extern int lib_answer(void);\n"
        "int main(void){ return lib_answer(); }\n");

    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<fs::path>{archive});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{mainSrc.string()}, "c",
        std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"}, rep);
    ASSERT_EQ(rc, 0)
        << "static-link of the real gcc jump-table lib must succeed (the reader must "
           "resolve its section-relative relocs); errs=" << rep.errorCount();
    auto const mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath)) << "the self-contained exec must exist";

#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    auto const r = runBinary(mainPath);
    ASSERT_TRUE(r.spawned) << "main must spawn. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE c167 acceptance criterion: exit 42 = a REAL gcc switch jump table "
           "(anonymous .rodata gap atom + interior .text relocs) reconstructed, "
           "merged, and executed from a DSS-written .a.";
#endif  // __linux__
}

// -- c168: Mach-O static-link (pull + merge via the c168 Mach-O reader) ---------
//
// The Mach-O sibling of PullResolvesReferenceAndMergeStripsImport: DSS writes a
// Mach-O `.a` (arm64 MH_OBJECT members via the format-blind c163 ar writer),
// then the static-link PULL dispatches to the c168 Mach-O object reader (NOT the
// ELF one -- the compile_pipeline switch on format.kind()), reconstructs the
// member into an AssembledModule, and the c154 merge binds main's extern to the
// pulled definition + STRIPS the import EXACTLY as the ELF path does. This is the
// STRUCTURAL witness (pull + merge), running on every host; the macOS RUN witness
// rides the macos-latest CI leg (Mach-O has no off-Mac execution) -- the named
// follow-up D-LK-MACHO-STATIC-LINK-RUNTIME. Red-on-disable: without the pull, the
// merge leaves dss_lib_answer an unresolved import (asserted below).
TEST(StaticLink, MachOPullResolvesReferenceAndMergeStripsImport) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadMachoSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive =
        buildArchive(dir, "libdsslib_macho.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(archive.empty());
    EXPECT_TRUE(isArArchiveFile(archive));

    // Mach-O mangles a C name with a leading `_`, so match by substring.
    auto refsAnswer = [](std::string const& n) {
        return n.find("dss_lib_answer") != std::string::npos;
    };

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s, *s.exec, mainRep);
    ASSERT_TRUE(mainMod) << "main assemble failed; errs=" << mainRep.errorCount();
    ASSERT_TRUE(std::any_of(mainMod->externImports.begin(), mainMod->externImports.end(),
                            [&](ExternImport const& e){ return refsAnswer(e.mangledName); }))
        << "main must reference dss_lib_answer as an extern import";

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, kNoDynamicLibraries, *s.target, *s.exec, pullRep);
    ASSERT_TRUE(pulled) << "macho pull failed; errs=" << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u) << "exactly the one member defining dss_lib_answer";
    EXPECT_TRUE(std::any_of((*pulled)[0].symbols.begin(), (*pulled)[0].symbols.end(),
        [&](ModuleSymbol const& ms){
            return refsAnswer(ms.name) && isExternallyVisible(ms.binding, ms.visibility);
        }))
        << "the pulled Mach-O member must define dss_lib_answer";
    EXPECT_EQ(pullRep.errorCount(), 0u);

    std::vector<AssembledModule> combined;
    combined.push_back(*mainMod);
    combined.push_back(std::move((*pulled)[0]));
    DiagnosticReporter linkRep;
    auto image = linker::link(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        *s.target, *s.exec, linkRep);
    EXPECT_EQ(linkRep.errorCount(), 0u) << "merged Mach-O static link must be clean";
    EXPECT_TRUE(image.ok());
    EXPECT_FALSE(std::any_of(image.externImportNames.begin(),
                             image.externImportNames.end(), refsAnswer))
        << "the merge must STRIP dss_lib_answer (bound to the pulled definition)";
    EXPECT_EQ(image.resolvedCrossCuRefs.size(), 1u)
        << "the reference->definition binding must be recorded";

    // RED-ON-DISABLE: main ALONE (no pulled member) leaves dss_lib_answer
    // UNRESOLVED -- the exact state the static pull removes. See the elf sibling
    // above for why this asserts a LOUD REJECTION rather than an import row: with
    // the retired per-language library default gone, an unbound extern no longer
    // gets an invented import library to hide in.
    DiagnosticReporter aloneRep;
    auto imageAlone = linker::link(
        std::span<AssembledModule const>{&*mainMod, 1}, *s.target, *s.exec, aloneRep);
    EXPECT_TRUE(aloneRep.hasErrors())
        << "without the static pull, the reference to dss_lib_answer must be "
           "rejected LOUD at link";
    EXPECT_EQ(imageAlone.resolvedCrossCuRefs.size(), 0u)
        << "and nothing may claim to have resolved it";
}

// -- c168: Mach-O static-link through the PRODUCTION driver ----------------------
//
// The `--resolve-library <archive.a>` surface for Mach-O: DSS static-links `main`
// against a DSS-written Mach-O `.a` through the real `Program::compileFiles`
// driver, emitting a SELF-CONTAINED Mach-O executable. The BUILD runs on every
// host (cross-compile to Mach-O -- proving the driver's Mach-O pull + merge +
// exec-emit path); the RUN + red-on-disable are `__APPLE__`-gated (Mach-O has no
// off-Mac execution -- the macos-latest CI leg is the runtime witness, the
// cross-target-runtime-closure discipline; D-LK-MACHO-STATIC-LINK-RUNTIME).
TEST(StaticLink, MachODriverStaticLinkBuildsSelfContainedExec) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadMachoSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive =
        buildArchive(dir, "libdsslib_macho.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(archive.empty());
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<fs::path>{archive});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{mainSrc.string()}, "c",
        std::vector<std::string>{"arm64:macho64-arm64-darwin-exec"}, rep);
    ASSERT_EQ(rc, 0) << "Mach-O static-link build must succeed; errs=" << rep.errorCount();
    auto const mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath)) << "the self-contained Mach-O exec must exist";

#if defined(__APPLE__) && defined(__aarch64__)
    // RUN (Apple-Silicon macOS): the pulled dss_lib_answer body is IN the exe ->
    // exit 42. No dylib dependency for the archive's symbols (self-contained).
    auto const r = runBinary(mainPath);
    ASSERT_TRUE(r.spawned) << "main must spawn. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "exit 42 = dss_lib_answer() pulled from the Mach-O libdsslib.a, merged "
           "into main, and executed on Apple Silicon.";
#endif  // __APPLE__ && __aarch64__
}

// -- c170: Windows COFF static-link (pull + merge via the c170 COFF reader) ------
//
// The COFF sibling of the ELF/Mach-O static-link witnesses: DSS writes a `.a` of
// COFF `.obj` members, the pull DISPATCHES to the c170 COFF object reader (the
// compile_pipeline switch on format.kind() == Pe), and the merge binds main's
// extern to the pulled definition + STRIPS the import. STRUCTURAL (pull + merge),
// runs on every host. PE x64 C mangling is IDENTITY (no leading underscore).
TEST(StaticLink, CoffPullResolvesReferenceAndMergeStripsImport) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadCoffSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive =
        buildArchive(dir, "libdsslib_coff.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(archive.empty());
    auto refsAnswer = [](std::string const& n) {
        return n.find("dss_lib_answer") != std::string::npos;
    };

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s, *s.exec, mainRep);
    ASSERT_TRUE(mainMod) << "main assemble failed; errs=" << mainRep.errorCount();
    ASSERT_TRUE(std::any_of(mainMod->externImports.begin(), mainMod->externImports.end(),
                            [&](ExternImport const& e){ return refsAnswer(e.mangledName); }))
        << "main must reference dss_lib_answer as an extern import";

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, kNoDynamicLibraries, *s.target, *s.exec, pullRep);
    ASSERT_TRUE(pulled) << "coff pull failed; errs=" << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u) << "exactly the one member defining dss_lib_answer";
    EXPECT_TRUE(std::any_of((*pulled)[0].symbols.begin(), (*pulled)[0].symbols.end(),
        [&](ModuleSymbol const& ms){
            return refsAnswer(ms.name) && isExternallyVisible(ms.binding, ms.visibility);
        }))
        << "the pulled COFF member must define dss_lib_answer";
    EXPECT_EQ(pullRep.errorCount(), 0u);

    std::vector<AssembledModule> combined;
    combined.push_back(*mainMod);
    combined.push_back(std::move((*pulled)[0]));
    DiagnosticReporter linkRep;
    auto image = linker::link(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        *s.target, *s.exec, linkRep);
    EXPECT_EQ(linkRep.errorCount(), 0u) << "merged COFF static link must be clean";
    EXPECT_TRUE(image.ok());
    EXPECT_FALSE(std::any_of(image.externImportNames.begin(),
                             image.externImportNames.end(), refsAnswer))
        << "the merge must STRIP dss_lib_answer (bound to the pulled definition)";
    EXPECT_EQ(image.resolvedCrossCuRefs.size(), 1u);
}

// -- c170: Windows COFF static-link through the PRODUCTION driver + NATIVE RUN ----
//
// DSS static-links `main` against a DSS-written `.a` of COFF `.obj` members via
// the real driver, emitting a self-contained PE executable. Unlike the ELF (WSL)
// and Mach-O (macOS-only) legs, the PE exec RUNS on THIS host: the `_WIN32` RUN
// arm executes on the Windows MSVC gate + the windows-msvc CI leg -- exit 42 =
// dss_lib_answer() pulled from the COFF `.a`, merged, and executed.
TEST(StaticLink, CoffDriverStaticLinkExitsFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadCoffSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive =
        buildArchive(dir, "libdsslib_coff.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(archive.empty());
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<fs::path>{archive});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{mainSrc.string()}, "c",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    ASSERT_EQ(rc, 0) << "COFF static-link build must succeed; errs=" << rep.errorCount();
    auto mainPath = dir / "main.exe";
    if (!fs::exists(mainPath)) mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath)) << "the self-contained PE exec must exist";

#if defined(_WIN32)
    auto const r = runBinary(mainPath);
    ASSERT_TRUE(r.spawned) << "main must spawn. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE c170 acceptance criterion: exit 42 = dss_lib_answer() pulled from "
           "the COFF libdsslib.a, merged into main, and executed on Windows.";
#endif  // _WIN32
}

// -- W1 real-lib.c artifact drop (DISABLED; run out-of-band) --------------------
//
// Drops a DSS-compiled-from-real-lib.c `libdsslib.a` + the static-linked `main`
// exec to a persistent dir for the WSL `readelf`/`nm`/run cross-check (the suite
// stays hermetic; the __linux__ RUN pin above is the automated witness). Mirrors
// test_ar_writer's DISABLED_WriteRealArchivesForWslWitness. Run explicitly:
//   test_static_link --gtest_also_run_disabled_tests \
//                    --gtest_filter='*RealLibcWitness*'
// Output dir: $DSS_STATIC_WITNESS_DIR (else the system temp dir); paths printed.
// Proof to run under WSL against ./main:  exit 42; `readelf -d main` has NO
// NEEDED for dsslib; `readelf --dyn-syms main` has NO undefined dss_lib_answer.
TEST(StaticLink, DISABLED_RealLibcWitnessArtifactDrop) {
    char const* envDir = std::getenv("DSS_STATIC_WITNESS_DIR");
    fs::path const outDir = envDir ? fs::path{envDir}
                                   : fs::temp_directory_path() / "dss-static-witness";
    std::error_code ec;
    fs::create_directories(outDir, ec);
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);

    auto const archive =
        buildArchive(outDir, "libdsslib.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(archive.empty());
    auto const mainSrc = writeSrc(outDir, "main.c", kMainSrc);

    Program p;
    p.setOutputDir(outDir);
    p.setResolveLibraries(std::vector<fs::path>{archive});
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileFiles(std::vector<std::string>{mainSrc.string()}, "c",
                  std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"}, rep), 0)
        << "errs=" << rep.errorCount();
    std::cout << "[witness] wrote " << archive.string() << "\n";
    std::cout << "[witness] wrote " << (outDir / "main").string() << "\n";
}

// == c171 (D-FF1-AR-STATICLIB-DRIVER-WIRING): the DRIVER emits a static library ==
//
// The INVERSE arc of the static-LINK tests above: handed a `container: archive`
// FORMAT target, the production `Program::compileFiles` driver lowers each CU to
// its OWN relocatable member (NO cross-CU merge -- an archive PACKAGES separate
// objects) and bundles them into ONE `ar` archive (`.a` for ELF/Mach-O, `.lib`
// for PE) via `linkAndWriteStaticArchive`, dispatched on the format's declared
// container (never the artifactProfile). These witness the WHOLE driver path
// end-to-end: an emitted archive read back through the c161 ar reader with an
// EXACT member count + armap symbol set, its member bytes a real ET_REL.

namespace {
// A single 2-function c source -> ONE CU -> ONE archive member exporting
// BOTH `dss_add` + `dss_sub`. (Shared by the ELF / PE / Mach-O driver witnesses.)
constexpr std::string_view kTwoFnLibSrc =
    "int dss_add(int a,int b){ return a+b; }\n"
    "int dss_sub(int a,int b){ return a-b; }\n";
}  // namespace

TEST(StaticLink, ElfStaticLibDriverEmitsArchiveWithArmap) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    auto const src = writeSrc(dir, "dsslibmath.c", kTwoFnLibSrc);

    Program p;
    p.setOutputDir(dir);
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{src.string()}, "c",
        std::vector<std::string>{"x86_64:elf64-x86_64-linux-staticlib"}, rep);
    ASSERT_EQ(rc, 0) << "ELF staticlib build must succeed; errs=" << rep.errorCount();

    auto const archivePath = dir / "dsslibmath.a";
    ASSERT_TRUE(fs::exists(archivePath))
        << "the driver must emit a `.a` static library at <stem>.a";
    EXPECT_EQ(archivePath.extension().string(), ".a");
    EXPECT_TRUE(isArArchiveFile(archivePath))
        << "the emitted `.a` must carry the !<arch> magic";

    // Read it back with the c161 ar reader: EXACTLY one member; the armap lists
    // EXACTLY {dss_add, dss_sub} (ELF C mangling is identity -- no underscore).
    auto const bytes = readFileBytes(archivePath);
    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(bytes, archivePath.string(), rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;
    ASSERT_EQ(arch->members.size(), 1u) << "one source CU -> exactly one member";

    std::vector<std::string> armap;
    for (auto const& sym : arch->symbols) armap.push_back(sym.name);
    std::sort(armap.begin(), armap.end());
    ASSERT_EQ(armap.size(), 2u) << "the armap lists exactly the two exported fns";
    EXPECT_EQ(armap[0], "dss_add");
    EXPECT_EQ(armap[1], "dss_sub");

    // The member bytes parse as a valid ELF ET_REL (the c164 reader), defining
    // BOTH functions -- proof the archived member is a real relocatable object.
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);
    auto const memberBytes = std::span<std::uint8_t const>{bytes}.subspan(
        arch->members[0].dataOffset, arch->members[0].size);
    DiagnosticReporter mrep;
    auto member = elf::readRelocatableObject(memberBytes, *s.target, *s.reloc, mrep);
    ASSERT_TRUE(member) << "the archived member must parse as an ET_REL; errs="
                        << mrep.errorCount();
    EXPECT_TRUE(moduleDefinesExternallyVisible(*member, "dss_add"));
    EXPECT_TRUE(moduleDefinesExternallyVisible(*member, "dss_sub"));
}

// The PE sibling: a `pe64-*-windows-staticlib` target emits a `.lib` whose bytes
// carry TWO "/" linker members (the SysV BE 1st + the Microsoft LE 2nd) -- the
// ArArchiveFlavor::Coff threading. That 2nd linker member is what distinguishes
// a correct PE static lib from a SysV-only `.a`. Structural on every host (byte
// parse only, no tool run).
TEST(StaticLink, PeStaticLibDriverEmitsCoffLibWithSecondLinkerMember) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    auto const src = writeSrc(dir, "dsslibmath.c", kTwoFnLibSrc);

    Program p;
    p.setOutputDir(dir);
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{src.string()}, "c",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-staticlib"}, rep);
    ASSERT_EQ(rc, 0) << "PE staticlib build must succeed; errs=" << rep.errorCount();

    auto const libPath = dir / "dsslibmath.lib";
    ASSERT_TRUE(fs::exists(libPath))
        << "the driver must emit a `.lib` for a PE staticlib";
    auto const bytes = readFileBytes(libPath);

    // THE COFF-vs-SysV discriminator: TWO "/" linker index members.
    EXPECT_EQ(countArSlashLinkerMembers(bytes), 2)
        << "a PE `.lib` must carry the SysV 1st + Microsoft LE 2nd linker members "
           "(the flavor threading); a SysV-only `.a` would carry just 1";

    // The c161 reader consumes the FIRST (SysV BE) armap: one member, both fns
    // (PE x64 C mangling is identity -- no leading underscore).
    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(bytes, libPath.string(), rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;
    ASSERT_EQ(arch->members.size(), 1u) << "one source CU -> exactly one member";
    std::vector<std::string> armap;
    for (auto const& sym : arch->symbols) armap.push_back(sym.name);
    std::sort(armap.begin(), armap.end());
    ASSERT_EQ(armap.size(), 2u);
    EXPECT_EQ(armap[0], "dss_add");
    EXPECT_EQ(armap[1], "dss_sub");
}

// D-FF1-STATICLIB-FAT-ARCHIVE: a staticlib-target build handed INPUT static
// archives via `--resolve-library` MERGES every one of their members INTO the
// output library (a "fat"/merged static library), instead of failing loud. The
// output `.a` carries the CU-derived member AND the input archive's member; its
// armap is the UNION of both members' exported symbols; and each member re-parses
// as a real ET_REL. RED-ON-DISABLE: restore the D_StaticLibFatArchiveUnsupported
// fail-loud in `compileOneTarget` and this build returns rc != 0 (the ASSERT_EQ
// below flips red) -- the ONLY thing that makes it succeed with 2 members is the
// merge.
TEST(StaticLink, StaticLibDriverMergesInputStaticArchive) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);

    // A real INPUT `.a` on disk defining `dss_lib_answer` (member "helper.o").
    auto const inputArchive =
        buildArchive(dir, "libinput.a", {{std::string{kLibSrc}, "helper.o"}}, s);
    ASSERT_FALSE(inputArchive.empty());
    ASSERT_TRUE(isArArchiveFile(inputArchive));

    // The CU-derived member defines a DIFFERENT symbol (`dss_extra`) -- so the
    // two members' armaps are disjoint and the union is unambiguous.
    auto const src = writeSrc(dir, "dssfat.c",
                              "int dss_extra(int a){ return a + 1; }\n");

    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<fs::path>{inputArchive});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{src.string()}, "c",
        std::vector<std::string>{"x86_64:elf64-x86_64-linux-staticlib"}, rep);
    ASSERT_EQ(rc, 0) << "fat-archive staticlib build must succeed; errs="
                     << rep.errorCount();

    auto const libPath = dir / "dssfat.a";
    ASSERT_TRUE(fs::exists(libPath)) << "the driver must emit the fat `.a`";
    auto const bytes = readFileBytes(libPath);
    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(bytes, libPath.string(), rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;

    // TWO members: the CU-derived `dssfat.o` + the MERGED input `helper.o`.
    ASSERT_EQ(arch->members.size(), 2u)
        << "one CU member + one merged input-archive member";

    // The armap is the UNION: BOTH `dss_extra` (CU) and `dss_lib_answer` (input).
    std::vector<std::string> armap;
    for (auto const& sym : arch->symbols) armap.push_back(sym.name);
    std::sort(armap.begin(), armap.end());
    ASSERT_EQ(armap.size(), 2u) << "exactly the two members' exported symbols";
    EXPECT_EQ(armap[0], "dss_extra");
    EXPECT_EQ(armap[1], "dss_lib_answer");

    // Both members re-parse as valid ET_REL, each defining its own symbol -- the
    // merged input member is a REAL relocatable object, not a dropped stub.
    bool sawExtra = false, sawAnswer = false;
    for (auto const& m : arch->members) {
        auto const mb =
            std::span<std::uint8_t const>{bytes}.subspan(m.dataOffset, m.size);
        DiagnosticReporter mrep;
        auto member = elf::readRelocatableObject(mb, *s.target, *s.reloc, mrep);
        ASSERT_TRUE(member) << "member must parse as ET_REL; errs="
                            << mrep.errorCount();
        if (moduleDefinesExternallyVisible(*member, "dss_extra"))      sawExtra  = true;
        if (moduleDefinesExternallyVisible(*member, "dss_lib_answer")) sawAnswer = true;
    }
    EXPECT_TRUE(sawExtra)  << "the CU-derived member must define dss_extra";
    EXPECT_TRUE(sawAnswer) << "the MERGED input member must define dss_lib_answer";
}

// The RUNTIME witness for the fat archive (the WHOLE chain, not just structure):
// build a fat static library that MERGES an input archive's `dss_lib_answer` (=42)
// member, then static-link `main` (which calls dss_lib_answer) against the FAT lib
// -> a self-contained exec -> RUN -> exit 42. Proves the merged member survived
// the double round-trip (input `.a` -> read -> module -> written into the fat `.a`
// -> read again -> linked into main) and is real, linkable, and runnable. The lib
// build's rc==0 is itself red-on-disable (the restored fail-loud rejects it).
TEST(StaticLink, StaticLibFatArchiveExecRunsFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);

    // Input archive: the ONLY definition of dss_lib_answer() = 42.
    auto const inputArchive =
        buildArchive(dir, "libanswer.a", {{std::string{kLibSrc}, "answer.o"}}, s);
    ASSERT_FALSE(inputArchive.empty());

    // Build a FAT static lib: a CU defining an unrelated fn + the merged input.
    auto const libSrc = writeSrc(dir, "libfat.c",
                                 "int dss_unrelated(void){ return 7; }\n");
    Program pLib;
    pLib.setOutputDir(dir);
    pLib.setResolveLibraries(std::vector<fs::path>{inputArchive});
    DiagnosticReporter repLib;
    ASSERT_EQ(pLib.compileFiles(
                  std::vector<std::string>{libSrc.string()}, "c",
                  std::vector<std::string>{"x86_64:elf64-x86_64-linux-staticlib"},
                  repLib),
              0) << "fat lib build must succeed; errs=" << repLib.errorCount();
    auto const fatLib = dir / "libfat.a";
    ASSERT_TRUE(fs::exists(fatLib));

    // Link main (calls dss_lib_answer) against the FAT lib -- the merged member
    // is what satisfies main's reference.
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);
    Program pMain;
    pMain.setOutputDir(dir);
    pMain.setResolveLibraries(std::vector<fs::path>{fatLib});
    DiagnosticReporter repMain;
    int const rc = pMain.compileFiles(
        std::vector<std::string>{mainSrc.string()}, "c",
        std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"}, repMain);
    ASSERT_EQ(rc, 0) << "link against the fat lib must succeed; errs="
                     << repMain.errorCount();
    auto const mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath));

#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    // RUN (x86_64 Linux host): dss_lib_answer's body -- merged in from
    // libanswer.a via libfat.a -- is IN the exe -> exit 42.
    auto const r = runBinary(mainPath);
    ASSERT_TRUE(r.spawned) << "main must spawn. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_lib_answer(), MERGED from "
           "libanswer.a into libfat.a, pulled by main's link against the fat lib, "
           "and called.";
#endif  // __linux__
}

// D-FF1-STATICLIB-FAT-ARCHIVE (PE) + the SEQUENTIAL-IN-PROCESS shape: the PE/COFF
// sibling of StaticLibFatArchiveExecRunsFortyTwo, built the way the examples
// harness's `buildDependencyArtifact` builds it -- THREE sequential in-process
// `Program` builds into ONE shared output dir:
//   1. input.lib  (pe64 staticlib) defining dss_input_answer() = 42
//   2. fatlib.lib (pe64 staticlib) defining dss_fat_extra(), RESOLVING input.lib
//      -> THE FAT MERGE: fatlib.lib must carry BOTH members.
//   3. main.exe   (pe64 exec) calling dss_input_answer, RESOLVING ONLY fatlib.lib
//      -> the sole path to dss_input_answer is the copy the merge carried across.
//
// This pins the pe64 fat-archive merge that the `fat_archive_merge` corpus example
// exercises end-to-end -- but WHITE-BOX (it re-reads the intermediate fatlib.lib
// and asserts its armap is the UNION of both members' symbols) and in a SINGLE
// process across three sequential `Program` builds (the exact shape a
// process-global-state defect would corrupt -- proving there is none). The prior
// PE tests cover single-CU staticlib EMIT + the pull-from-a-plain-`.a` link; NONE
// covered the PE fat MERGE. RED-ON-DISABLE: restore the D_StaticLibFatArchive
// Unsupported fail-loud in compileOneTarget and step 2's build returns rc != 0
// (ASSERT_EQ(rcFat, 0) flips red); a silent-no-op merge drops dss_input_answer
// from fatlib.lib's armap (the union ASSERT flips red) AND main.exe faults at load
// (the exit-42 EXPECT flips red on Windows).
TEST(StaticLink, PeFatArchiveSequentialInProcessMergeExecRunsFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    constexpr char const* kStaticlibSpec = "x86_64:pe64-x86_64-windows-staticlib";

    // ── Build 1: input.lib (pe64 staticlib) -- the ONLY def of dss_input_answer ──
    auto const inputSrc = writeSrc(dir, "input.c",
                                   "int dss_input_answer(void){ return 42; }\n");
    Program pInput;
    pInput.setOutputDir(dir);
    DiagnosticReporter repInput;
    ASSERT_EQ(pInput.compileFiles(
                  std::vector<std::string>{inputSrc.string()}, "c",
                  std::vector<std::string>{kStaticlibSpec}, repInput),
              0) << "input.lib build must succeed; errs=" << repInput.errorCount();
    auto const inputLib = dir / "input.lib";
    ASSERT_TRUE(fs::exists(inputLib)) << "the driver must emit input.lib";

    // ── Build 2: fatlib.lib (pe64 staticlib) RESOLVING input.lib -- THE MERGE ──
    // Same process, a FRESH Program -- exactly buildDependencyArtifact's shape.
    auto const fatSrc = writeSrc(dir, "fatlib.c",
                                 "int dss_fat_extra(void){ return 7; }\n");
    Program pFat;
    pFat.setOutputDir(dir);
    pFat.setResolveLibraries(std::vector<fs::path>{inputLib});
    DiagnosticReporter repFat;
    int const rcFat = pFat.compileFiles(
        std::vector<std::string>{fatSrc.string()}, "c",
        std::vector<std::string>{kStaticlibSpec}, repFat);
    ASSERT_EQ(rcFat, 0) << "fat-archive staticlib build (the MERGE) must succeed; "
                           "errs=" << repFat.errorCount();
    auto const fatLib = dir / "fatlib.lib";
    ASSERT_TRUE(fs::exists(fatLib)) << "the driver must emit the fat fatlib.lib";

    // WHITE-BOX: fatlib.lib carries BOTH members and its armap is their UNION --
    // dss_input_answer (merged from input.lib) MUST be present, or main can never
    // resolve it. This is the decisive intermediate check the corpus example's
    // exit code can only assert transitively.
    auto const fatBytes = readFileBytes(fatLib);
    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(fatBytes, fatLib.string(), rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;
    ASSERT_EQ(arch->members.size(), 2u)
        << "one CU member (fatlib.o) + one MERGED input-archive member (input.o)";
    std::vector<std::string> armap;
    for (auto const& sym : arch->symbols) armap.push_back(sym.name);
    std::sort(armap.begin(), armap.end());
    ASSERT_EQ(armap.size(), 2u) << "exactly the two members' exported symbols";
    EXPECT_EQ(armap[0], "dss_fat_extra")   << "the CU-derived member's symbol";
    EXPECT_EQ(armap[1], "dss_input_answer")
        << "THE merge witness: input.lib's symbol MUST be carried into fatlib.lib";

    // ── Build 3: main.exe (pe64 exec) RESOLVING ONLY fatlib.lib ──
    auto const mainSrc = writeSrc(dir, "main.c",
                                  "extern int dss_input_answer(void);\n"
                                  "int main(void){ return dss_input_answer(); }\n");
    Program pMain;
    pMain.setOutputDir(dir);
    pMain.setResolveLibraries(std::vector<fs::path>{fatLib});
    DiagnosticReporter repMain;
    int const rcMain = pMain.compileFiles(
        std::vector<std::string>{mainSrc.string()}, "c",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, repMain);
    ASSERT_EQ(rcMain, 0) << "link against the fat lib must succeed; errs="
                         << repMain.errorCount();
    auto const mainExe = dir / "main.exe";
    ASSERT_TRUE(fs::exists(mainExe)) << "the self-contained PE exec must exist";

#if defined(_WIN32)
    // RUN (Windows host): dss_input_answer's body -- merged input.lib -> fatlib.lib
    // -> pulled into main -- is IN the exe -> exit 42. A dropped merge would fault-
    // load here (STATUS_ENTRYPOINT_NOT_FOUND) instead.
    auto const r = runBinary(mainExe);
    ASSERT_TRUE(r.spawned) << "main.exe must spawn. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_input_answer(), MERGED from "
           "input.lib into fatlib.lib across three sequential in-process Program "
           "builds, pulled by main's link against the fat lib, and called.";
#endif  // _WIN32
}

// The Mach-O sibling of the ELF driver witness: an arm64 Mach-O staticlib target
// emits a `.a` whose armap lists both members' symbols (Mach-O C mangling
// prepends `_`). STRUCTURAL on every host (no run -- the Mach-O runtime witness
// rides the macOS CI leg).
TEST(StaticLink, MachoStaticLibDriverEmitsArchive) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    auto const src = writeSrc(dir, "dsslibmath.c", kTwoFnLibSrc);

    Program p;
    p.setOutputDir(dir);
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{src.string()}, "c",
        std::vector<std::string>{"arm64:macho64-arm64-darwin-staticlib"}, rep);
    ASSERT_EQ(rc, 0) << "Mach-O staticlib build must succeed; errs=" << rep.errorCount();

    auto const archivePath = dir / "dsslibmath.a";
    ASSERT_TRUE(fs::exists(archivePath))
        << "the driver must emit a `.a` for a Mach-O staticlib";
    EXPECT_TRUE(isArArchiveFile(archivePath));

    auto const bytes = readFileBytes(archivePath);
    DiagnosticReporter rrep;
    auto arch = ffi::readArArchive(bytes, archivePath.string(), rrep);
    ASSERT_TRUE(arch.has_value()) << arch.error().detail;
    ASSERT_EQ(arch->members.size(), 1u) << "one source CU -> exactly one member";

    // Mach-O mangles a C name with a leading `_`; pin the EXACT armap size (both
    // functions, nothing else) and match each symbol by substring.
    ASSERT_EQ(arch->symbols.size(), 2u)
        << "the armap lists exactly the two exported fns";
    auto refsSym = [&](std::string_view want) {
        return std::any_of(arch->symbols.begin(), arch->symbols.end(),
            [&](ffi::ArSymbol const& sym) {
                return sym.name.find(want) != std::string::npos;
            });
    };
    EXPECT_TRUE(refsSym("dss_add"));
    EXPECT_TRUE(refsSym("dss_sub"));
}

// P10 (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE): a static-archive MEMBER is a
// FINAL module of its own artifact — the foreign linker pulls members whole,
// nothing downstream merges them — so the member's lowering site carries the
// PROGRAM stage. This pin witnesses that site through the REAL driver
// channel: the same source, compiled at Debug and at Release into a
// staticlib, must produce DIFFERENT member bytes (the release build ran the
// two-stage schedule — release-unit per CU, then the program stage over the
// member — and ConstFold/Inlining observably change `.text`), while the
// debug build (Identity at both stages) leaves the foldable arithmetic live.
// If the member's program-stage call is deleted, the release member is
// optimized ONLY at the unit stage and this comparison narrows toward
// equality — the archive twin of the routing pin in test_compile_pipeline.
TEST(StaticArchive, ReleaseMemberIsProgramStageOptimized) {
    ScratchDir scratch{Location::InsideRepo, "static_link"};
    scratch.useAsCwd();
    char const* const src =
        "int fold(int a) { return (2 + 3) * a; }\n"
        "int entry(int x) { return fold(x) + fold(x + 1); }\n";

    auto build = [&](char const* label, CompileConfig cfg) {
        fs::path const outDir = scratch.path() / label;
        fs::path const srcPath = scratch.path() / (std::string{label} + ".c");
        {
            std::ofstream out{srcPath, std::ios::binary};
            out << src;
        }
        std::vector<std::string> files;
        files.push_back(srcPath.generic_string());
        Program prog;
        prog.setCompileConfig(cfg);
        prog.setOutputDir(outDir);
        DiagnosticReporter rep;
        int const rc = prog.compileUnits(
            files, "c", {"x86_64:elf64-x86_64-linux-staticlib"}, rep);
        EXPECT_EQ(rc, 0) << label << " build must succeed";
        if (rc != 0) return std::vector<std::uint8_t>{};
        fs::path artifact = outDir / (std::string{label} + ".a");
        std::error_code ec;
        auto const size = fs::file_size(artifact, ec);
        EXPECT_FALSE(ec) << "no staticlib artifact at " << artifact;
        if (ec) return std::vector<std::uint8_t>{};
        std::ifstream in{artifact, std::ios::binary};
        return std::vector<std::uint8_t>{(std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>()};
    };

    auto const dbg = build("member_dbg", CompileConfig::Debug);
    auto const rel = build("member_rel", CompileConfig::Release);
    ASSERT_FALSE(dbg.empty()) << "the debug member archive must exist";
    ASSERT_FALSE(rel.empty()) << "the release member archive must exist";
    EXPECT_NE(dbg, rel)
        << "Debug and Release members must differ byte-wise — the release "
           "member carries the PROGRAM-stage schedule (unit stage + member "
           "pre-lower optimize); identical bytes mean the member's "
           "program-stage call is gone";

    // ★ THE STRICT SITE WITNESS — the byte comparison above can pass on the
    // UNIT stage's schedule difference ALONE (debug unit=Identity vs
    // release-unit), so it cannot see the member's program-stage site
    // specifically. This arm can: a single-CU archive build runs the Optimize
    // phase EXACTLY TWICE (once per-CU at the unit site, once over the member
    // at its lowering site). Delete the member's program-stage call and this
    // reads 1 — a strict red that names the site, independent of which passes
    // either schedule contains.
    substrate::PhaseTimers::reset();
    fs::path const outDir = scratch.path() / "member_count";
    fs::path const srcPath = scratch.path() / "member_count.c";
    {
        std::ofstream out{srcPath, std::ios::binary};
        out << src;
    }
    {
        Program prog;
        prog.setCompileConfig(CompileConfig::Release);
        prog.setOutputDir(outDir);
        DiagnosticReporter rep;
        std::vector<std::string> files;
        files.push_back(srcPath.generic_string());
        ASSERT_EQ(prog.compileUnits(
                      files, "c",
                      {"x86_64:elf64-x86_64-linux-staticlib"}, rep), 0);
    }
    EXPECT_EQ(substrate::PhaseTimers::read(substrate::CompilePhase::Optimize)
                  .runs,
              2u)
        << "a 1-CU static-archive build must run Optimize TWICE — the unit "
           "stage in buildCuMir and the program stage at the member's "
           "lowering site (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE)";
}

// ═══════════════════════════════════════════════════════════════════════════
// D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT
//
// A `.o` inside a `.a` is a RELOCATABLE OBJECT no matter what image it is being
// linked into, so it must be decoded through the relocatable vocabulary — never
// through the `-exec`/`-dylib` vocabulary of the artifact being produced. The
// member reader used to be handed the IMAGE's schema, which made DSS unable to
// read back its OWN writer's output for the most ordinary library member there
// is: one that CALLS something.
//
// ★★★ EVERY CASE BELOW PINS THE **RESOLUTION** — WHICH DOCUMENT DESCRIBES THE
// MEMBER — AND THAT IS THE CONTRACT. `archiveMemberFormat`, the pull's single
// member-read chokepoint, resolves the member's own object format through
// `runtime::resolveArchiveSiblingFormat` and never falls back to the link
// format; that fallback IS the defect this anchor names. So each case asserts
// that the resolution lands on the very document the fixture WROTE the archive
// with, which is a statement about the contract and holds no matter what the
// two vocabularies happen to contain.
//
// ⚠⚠ IT DID NOT ALWAYS SAY THAT, AND HOW THE OLD PIN DIED IS THE LESSON WORTH
// KEEPING. Each case used to assert a CONSEQUENCE instead: that reading the
// member through the IMAGE document REFUSES. That is only true while the two
// vocabularies happen to DIVERGE, so it was never the contract — it was a
// property of the corpus. On Mach-O x86_64 it was worse than that: the refusal
// existed only because the image documents held a TYPO. For a row named
// `X86_64_RELOC_BRANCH` they declared a nativeId that, under the packing their
// OWN comment states, decodes to r_type=SIGNED / 8 bytes / NOT pc-relative, and
// the row named `_4` declared a TWO-byte slot. The documents refuted
// themselves; no external authority was needed to call it a bug. When it was
// corrected (D-CONFIG-MACHO-X86_64-EXEC-DYLIB-RELOC-NATIVEID-CONTRADICTS-ITS-OWN-ROW)
// all four macho64-x86_64 documents became IDENTICAL on this axis, the
// image stopped refusing, and the pin went red — having spent its life
// asserting a consequence that rested on a defect.
// ⇒ A DIVERGENCE DISCRIMINATOR IS KEPT ONLY WHERE THE DIVERGENCE IS GENUINE AND
// DEFENSIBLE, as an EXTRA, never as the case's reason to exist.
//
// ⓘ WHERE THE TWO FAMILIES NOW STAND — ✔MEASURED over the shipped documents:
//   * ELF x86_64 STILL DIVERGES, structurally: `elf64-x86_64-linux{,-staticlib}`
//     declare `pltNativeId: 4` (R_X86_64_PLT32) and an `emitOnly` PC32 alias
//     (`R_X86_64_PC32_UNBIASED`); `-exec` declares NEITHER, and its only extra
//     row is a TLS one. A member that CALLS a library function emits wire 4,
//     which no image document has a row for. That is a real difference in what
//     the two artifacts can contain, so the ELF case keeps its discriminator.
//   * MACH-O x86_64 NO LONGER DIVERGES AT ALL: all four documents declare the
//     same three rows with the same wire values. Its discriminator is gone, and
//     its absence is correct rather than a gap — the resolution pin is what that
//     case was always really about.
// ═══════════════════════════════════════════════════════════════════════════

// ── THE SHARED CONTRACT ASSERTION ───────────────────────────────────────────
//
// `s.exec` is what the link PRODUCES; `s.staticlib` is the document the fixture
// WROTE the archive with. The pull must resolve the second from the first. The
// oracle is the fixture's own writer, so no format name is spelled here and the
// case cannot rot when the shipped set is renamed or extended.
//
// ⓘ The requester only shapes the REFUSAL PROSE (`resolveArchiveSiblingFormat`
// takes it so a refusal can name the static link rather than the runtime object
// cache it was first written for). The RESOLUTION is requester-independent, so
// borrowing the public constant here is a fact about the API, not a second owner
// of the static-link one — which is `internal` to the pipeline and deliberately
// not exported.
void expectMemberFormatResolvesToTheWriterDocument(Schemas const&   s,
                                                   std::string_view familyLabel) {
    ASSERT_TRUE(s.target && s.exec && s.staticlib)
        << familyLabel << ": schemas must load";
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    fs::path const formatsDir = *cfg / "object-formats";
    ASSERT_TRUE(fs::is_directory(formatsDir))
        << familyLabel
        << ": the shipped object-format tree is the SUBJECT of this assertion; "
           "without it the scan would be empty and the case would assert "
           "nothing: " << formatsDir.generic_string();

    auto const sibling = dss::runtime::resolveArchiveSiblingFormat(
        *s.exec, *s.target, formatsDir,
        dss::runtime::kRuntimeCacheSiblingRequester);
    ASSERT_TRUE(sibling.has_value())
        << familyLabel << ": the member's object format must RESOLVE from the "
           "image format the link is producing — a refusal here means the pull "
           "has no document to read the member with at all: " << sibling.error();
    EXPECT_EQ(*sibling, s.staticlib->name())
        << familyLabel
        << ": the pull must read the member through the SAME document that "
           "WROTE it. Resolving anything else decodes relocatable bytes with a "
           "vocabulary that was never promised to describe them";
    EXPECT_NE(*sibling, s.exec->name())
        << familyLabel
        << ": resolving the LINK's own image format is the defect itself — the "
           "member read must never fall back to the artifact being produced "
           "(D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-"
           "FORMAT)";
}

// The ELF instance, exactly as the anchor recorded it: a member that CALLS A
// LIBRARY FUNCTION, hence an R_X86_64_PLT32 the exec format does not declare.
TEST(ArchiveMemberObjectFormat,
     ElfMemberCallingALibraryFunctionReadsBackThroughTheObjectVocabulary) {
    ScratchDir scratch{Location::InsideRepo, "member-format-elf"};
    Schemas s = loadSchemas();
    ASSERT_TRUE(s.grammar);
    loadStaticlibSchema(s, "elf64-x86_64-linux-staticlib");
    ASSERT_TRUE(s.staticlib);

    // `puts` is an extern CALL, so the member carries a PLT-flavored branch
    // relocation — the whole point of the fixture. A member with no call at all
    // reads back fine through either vocabulary and would assert nothing.
    auto const archivePath = buildArchiveThroughStaticlibFormat(
        scratch.path(), "libanswer.a",
        {{"extern int puts(char const* s);\n"
          "int dss_lib_answer(void){ puts(\"lib\"); return 42; }\n",
          "libanswer"}}, s);
    ASSERT_FALSE(archivePath.empty());

    std::vector<std::uint8_t> owner;
    auto const memberBytes = firstMemberBytes(archivePath, owner);
    ASSERT_FALSE(memberBytes.empty());

    // ── THE CONTRACT: the member resolves to the document that WROTE it ─────
    expectMemberFormatResolvesToTheWriterDocument(s, "elf64-x86_64");

    // ── THE DIVERGENCE DISCRIMINATOR — KEPT HERE BECAUSE IT IS GENUINE ──────
    // These exact bytes must be REFUSED by the image vocabulary, and on THIS
    // family that refusal is structural rather than incidental: a member that
    // calls a library function emits R_X86_64_PLT32 (wire 4), which
    // `elf64-x86_64-linux{,-staticlib}` declare as `pltNativeId` and no image
    // document declares at all. It is an EXTRA assertion on top of the
    // resolution pin above, never this case's reason to exist.
    // ⚠ IF THIS EVER GOES GREEN-SIDE — i.e. the image format legitimately gains
    // a PLT32 row — DELETE THIS BLOCK, do not re-derive it onto some other
    // coincidence. The contract is already pinned above, and a discriminator
    // that has to be hunted for a fresh divergence every time the corpus
    // converges is asserting the corpus, not the compiler. That mistake is on
    // the record: the Mach-O sibling below carried exactly such a block until
    // the typo it depended on was fixed (see this section's docblock).
    {
        DiagnosticReporter imageRep;
        auto const throughImage = elf::readRelocatableObject(
            std::span<std::uint8_t const>{memberBytes}, *s.target, *s.exec,
            imageRep);
        EXPECT_FALSE(throughImage.has_value())
            << "the image format '" << s.exec->name() << "' ACCEPTED a "
               "relocatable member carrying R_X86_64_PLT32 — the ELF x86_64 "
               "vocabularies no longer divide here, so this EXTRA block should "
               "be DELETED (the resolution pin above carries the contract)";
        EXPECT_GT(imageRep.errorCount(), 0u);
    }
    {
        DiagnosticReporter objRep;
        auto const throughObject = elf::readRelocatableObject(
            std::span<std::uint8_t const>{memberBytes}, *s.target,
            *s.staticlib, objRep);
        ASSERT_TRUE(throughObject.has_value())
            << "the OBJECT format '" << s.staticlib->name()
            << "' could not read DSS's own writer output; errs="
            << objRep.errorCount();
        EXPECT_EQ(objRep.errorCount(), 0u);
    }

    // ── AND THE PULL, HANDED THE IMAGE FORMAT, MUST NOW SUCCEED ─────────────
    // This is the defect itself: the caller legitimately holds the image format
    // (it is producing an image), and the member read must not inherit it.
    DiagnosticReporter cliRep;
    auto clientMod = assembleFromSource(
        "extern int dss_lib_answer(void);\n"
        "int main(void){ return dss_lib_answer(); }\n",
        "client.c", s, *s.exec, cliRep);
    ASSERT_TRUE(clientMod);

    std::vector<fs::path> const archives{archivePath};
    DiagnosticReporter pullRep;
    auto pulled = pullStaticArchiveMembers(*clientMod, archives, kNoDynamicLibraries, *s.target,
                                           *s.exec, pullRep);
    ASSERT_TRUE(pulled.has_value())
        << "the pull was handed the IMAGE format and read the member with it; "
           "errs=" << pullRep.errorCount();
    EXPECT_EQ(pullRep.errorCount(), 0u);
    ASSERT_EQ(pulled->size(), 1u);
    EXPECT_TRUE(moduleDefinesExternallyVisible((*pulled)[0], "dss_lib_answer"));

    // The member's library call survived the decode. Asserting only "no error"
    // would also pass if the relocation had been dropped on the floor.
    std::vector<std::string> memberImports;
    for (auto const& ext : (*pulled)[0].externImports) {
        memberImports.push_back(ext.mangledName);
    }
    EXPECT_TRUE(importsContain(memberImports, "puts"))
        << "the pulled member lost the library call it was built to carry";
}

// The Mach-O x86_64 instance — the leg the anchor's 2026-08-20 amendment
// recorded as worse than the ELF case it was opened for. The member here calls
// a SIBLING FUNCTION rather than a library one: that is enough to emit
// X86_64_RELOC_BRANCH, and it keeps the case clear of
// D-LK-MACHO-ISDATA-NO-CALL-SIGNAL, which an EXTERN call on this leg now
// reaches.
//
// ⚠ This was once described here as "the CONFLICTING one". It is not, any more:
// the conflict was a typo in the image documents and has been corrected. What
// this case asserts is the RESOLUTION contract plus a genuine writer→reader
// round trip; see this section's docblock for why that is the stronger pin.
TEST(ArchiveMemberObjectFormat,
     MachoX86MemberWithABranchRelocReadsBackThroughTheObjectVocabulary) {
    ScratchDir scratch{Location::InsideRepo, "member-format-macho-x86"};
    Schemas const s = loadMachoX86Schemas();
    ASSERT_TRUE(s.grammar);
    ASSERT_TRUE(s.staticlib);

    auto const archivePath = buildArchiveThroughStaticlibFormat(
        scratch.path(), "libbranch.a",
        {{"int dss_lib_step(int x){ return x + 1; }\n"
          "int dss_lib_answer(void){ return dss_lib_step(41); }\n",
          "libbranch"}}, s);
    ASSERT_FALSE(archivePath.empty());

    std::vector<std::uint8_t> owner;
    auto const memberBytes = firstMemberBytes(archivePath, owner);
    ASSERT_FALSE(memberBytes.empty());

    // ── THE CONTRACT: the member resolves to the document that WROTE it ─────
    //
    // ⓘ AND THERE IS DELIBERATELY NO DIVERGENCE DISCRIMINATOR HERE. This case
    // used to assert that the IMAGE document REFUSES these bytes; that refusal
    // rested entirely on a self-refuting nativeId in the image documents, and
    // when it was corrected all four macho64-x86_64 documents became identical
    // on this axis. Re-deriving the block onto some other difference would be
    // hunting the corpus for a coincidence to lean on — see this section's
    // docblock. What the case is ACTUALLY about is which document the read
    // resolves, and that is asserted directly.
    expectMemberFormatResolvesToTheWriterDocument(s, "macho64-x86_64");
    {
        DiagnosticReporter objRep;
        auto const throughObject = macho::readRelocatableObject(
            std::span<std::uint8_t const>{memberBytes}, *s.target,
            *s.staticlib, objRep);
        ASSERT_TRUE(throughObject.has_value())
            << "the OBJECT format '" << s.staticlib->name()
            << "' could not read DSS's own writer output; errs="
            << objRep.errorCount();
        EXPECT_EQ(objRep.errorCount(), 0u);
    }

    DiagnosticReporter cliRep;
    auto clientMod = assembleFromSource(
        "extern int dss_lib_answer(void);\n"
        "int main(void){ return dss_lib_answer(); }\n",
        "client.c", s, *s.exec, cliRep);
    ASSERT_TRUE(clientMod);

    std::vector<fs::path> const archives{archivePath};
    DiagnosticReporter pullRep;
    auto pulled = pullStaticArchiveMembers(*clientMod, archives, kNoDynamicLibraries, *s.target,
                                           *s.exec, pullRep);
    ASSERT_TRUE(pulled.has_value())
        << "macho64-x86_64 pull refused; errs=" << pullRep.errorCount();
    EXPECT_EQ(pullRep.errorCount(), 0u);
    ASSERT_EQ(pulled->size(), 1u);
    EXPECT_TRUE(moduleDefinesExternallyVisible((*pulled)[0], "_dss_lib_answer"))
        << "the pulled member does not define the decorated entry the client "
           "referenced";
}

// ── The UNRESOLVABLE case: fail loud, and never fall back to the image ──────
//
// The fallback to the image format IS the defect, so "cannot resolve" must be a
// refusal rather than a quieter version of the old behaviour. The condition is
// forced the only way it can be without editing a shipped document: point the
// config discovery at a tree that holds the IMAGE format and no
// `container: "archive"` document at all, so the scan finds ZERO candidates.
TEST(ArchiveMemberObjectFormat,
     UnresolvableMemberFormatRefusesAndNamesMemberArchiveAndBothFormats) {
    ScratchDir scratch{Location::InsideRepo, "member-format-unresolvable"};
    Schemas s = loadSchemas();
    ASSERT_TRUE(s.grammar);
    loadStaticlibSchema(s, "elf64-x86_64-linux-staticlib");
    ASSERT_TRUE(s.staticlib);

    auto const archivePath = buildArchiveThroughStaticlibFormat(
        scratch.path(), "libanswer.a",
        {{"extern int puts(char const* s);\n"
          "int dss_lib_answer(void){ puts(\"lib\"); return 42; }\n",
          "libanswer"}}, s);
    ASSERT_FALSE(archivePath.empty());

    DiagnosticReporter cliRep;
    auto clientMod = assembleFromSource(
        "extern int dss_lib_answer(void);\n"
        "int main(void){ return dss_lib_answer(); }\n",
        "client.c", s, *s.exec, cliRep);
    ASSERT_TRUE(clientMod);

    // A config root carrying exactly ONE object-format document — the image
    // one. Every schema this case needs is already loaded and held above, so
    // narrowing discovery affects only the member-format resolution.
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    fs::path const shippedExec = *cfg
                               / "object-formats"
                               / "elf64-x86_64-linux-exec.format.json";
    ASSERT_TRUE(fs::is_regular_file(shippedExec)) << shippedExec.string();
    fs::path const fakeFormats =
        scratch.path() / "cfgroot" / "src" / "dss-config" / "object-formats";
    fs::create_directories(fakeFormats);
    fs::copy_file(shippedExec, fakeFormats / shippedExec.filename(),
                  fs::copy_options::overwrite_existing);

    std::vector<fs::path> const archives{archivePath};
    DiagnosticReporter pullRep;
    std::optional<std::vector<AssembledModule>> pulled;
    {
        ScopedEnv env("DSS_CONFIG_ROOT",
                      (scratch.path() / "cfgroot").string());
        pulled = pullStaticArchiveMembers(*clientMod, archives, kNoDynamicLibraries, *s.target,
                                          *s.exec, pullRep);
    }
    ASSERT_FALSE(pulled.has_value())
        << "an unresolvable member format was absorbed instead of refused — "
           "the only way to continue here is to read the member with the image "
           "format, which is the defect this row exists for";

    // ⚠⚠ AND THE `ASSERT_FALSE` ABOVE IS NOT THE ASSERTION THAT MATTERS —
    // ✔MEASURED, it passes under a mutant that reports the refusal and THEN
    // falls back to the image format anyway, because the reader refuses those
    // bytes one tier lower and the pull returns `nullopt` either way. A nullopt
    // therefore cannot tell "refused" from "fell back and failed downstream",
    // which is exactly the silent-fallback the row is about.
    //
    // THE COUNT IS WHAT DISCRIMINATES. Resolution is latched per link, so a
    // refusal is reported EXACTLY ONCE and nothing after it touches the member.
    // A fallback shows up as a SECOND diagnostic — the reader's
    // `F_CorruptedBinary` on bytes it was never meant to be handed.
    ASSERT_EQ(pullRep.errorCount(), 1u)
        << "expected exactly ONE diagnostic (the member-format refusal). More "
           "than one means the member was handed to a reader ANYWAY after the "
           "refusal was reported — a fallback wearing a diagnostic, which is "
           "the same wrong bytes reaching the same wrong vocabulary";
    ASSERT_FALSE(pullRep.all().empty());
    EXPECT_EQ(pullRep.all().front().code, DiagnosticCode::D_SchemaLoadFailed)
        << "the refusal must be the CONFIGURATION failure it is (the member's "
           "object format could not be resolved), not a claim that the member's "
           "bytes are corrupt — they are not";

    std::string text;
    for (auto const& d : pullRep.all()) {
        text += d.actual;
        text.push_back('\n');
    }
    // The four facts a triager needs, none of which the format resolver itself
    // can know: which member, in which archive, for which link, and the row.
    EXPECT_NE(text.find("libanswer"), std::string::npos) << text;
    EXPECT_NE(text.find(archivePath.filename().string()), std::string::npos)
        << text;
    EXPECT_NE(text.find("elf64-x86_64-linux-exec"), std::string::npos) << text;
    EXPECT_NE(text.find("D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-"
                        "THE-OBJECT-FORMAT"),
              std::string::npos) << text;
    // And it must name the caller, not the runtime object cache the shared
    // resolver was first written for.
    EXPECT_NE(text.find("static-link archive member read"), std::string::npos)
        << text;
}

// ── D-LK-ARCHIVE-MEMBER-EXTERN-LOSES-ITS-LIBRARY ─────────────────────────────
//
// ★★★ AN ARCHIVE MEMBER THAT CALLS A LIBRARY FUNCTION READ BACK CLEANLY AND
// THEN FAILED TO LINK — on EVERY leg, which is what made it the last thing
// standing between DSS and an ordinary static library.
//
// ✔MEASURED at the defect (shipped CLI, this host, baseline config) with a
// discriminating pair over ONE pair of sources — a TU calling `puts` plus a
// `main` calling it: compiled DIRECTLY as a multi-TU exec the build returned
// rc=0 on pe64-x86_64, elf64-x86_64, elf64-aarch64, macho64-arm64 and
// macho64-x86_64; routed through `-staticlib` + `--resolve-library` it returned
// rc=1 on all five, with `error[K_SymbolUndefined] undefined symbol 'puts'`.
//
// The direct path binds `puts` at the SEMANTIC tier, folding the shipped
// descriptor's per-format `library` map into the extern's `libraryPath`. An
// object file has nowhere to put that: an undefined symbol in ELF/COFF/Mach-O is
// a NAME and nothing else (✔MEASURED — the emitted `.lib`/`.a` contain the
// string `puts` and no `ucrtbase.dll` / `libc.so.6` anywhere). So the archive
// path must RE-DERIVE the binding from the same authority, which is what
// `pullStaticArchiveMembers` now does.
//
// ★★ THE ORACLE HERE IS THE DIRECT PATH ITSELF, NOT A HARDCODED LIBRARY NAME.
// Each case asks what the SAME SOURCE binds when compiled straight to the exec
// format, then requires the pulled member to reach BYTE-IDENTICALLY that. A test
// that spelled the image name would pin today's corpus rather than the property,
// and would have to be edited every time the platform's realization legitimately
// moved — while still not asserting that the two paths AGREE, which is the only
// thing that matters (two rows that disagree on the owning image do not fold in
// the linker's dedup and become two import descriptors for one C runtime — the
// split-CRT defect UCRT-P4 exists to prevent).

// A TU that calls a plain library function, plus its client. `puts` is chosen
// because it is a PLAIN descriptor row on every shipped format — no `synthesize`
// recipe, no per-target `linkName` — so this case isolates the binding itself.
constexpr std::string_view kPutsLibSrc =
    "extern int puts(const char *s);\n"
    "static int dss_helper(int v){ return v + 1; }\n"
    "int dss_lib_answer(void){ puts(\"from-lib\"); return dss_helper(41); }\n";

// The `libraryPath` the named extern carries in `mod`, or nullopt when the
// module has no such import at all.
[[nodiscard]] std::optional<std::string>
importLibraryOf(AssembledModule const& mod, std::string_view symbol) {
    for (auto const& e : mod.externImports) {
        if (e.mangledName == symbol) return e.libraryPath;
    }
    return std::nullopt;
}

// The on-binary spelling `cName` takes in `mod` — the C name on an undecorated
// format, the decorated one on Mach-O. Derived from the module the DIRECT path
// produced rather than from an underscore literal, so no format name appears in
// a test.
[[nodiscard]] std::optional<std::string>
onBinaryNameEndingIn(AssembledModule const& mod, std::string_view cName) {
    for (auto const& e : mod.externImports) {
        if (e.mangledName == cName) return e.mangledName;
        if (e.mangledName.size() > cName.size()
            && e.mangledName.compare(e.mangledName.size() - cName.size(),
                                     cName.size(), cName) == 0) {
            return e.mangledName;
        }
    }
    return std::nullopt;
}

// One family's worth of the discriminating pair. `s` must carry `exec` and
// `staticlib`; the member is written through the `container: "archive"` format,
// exactly as a `-staticlib` target build does.
void expectArchiveMemberRebindsLibraryImport(Schemas const& s,
                                             std::string_view familyLabel) {
    ASSERT_TRUE(s.target && s.exec && s.staticlib && s.grammar)
        << familyLabel << ": schemas must load";

    // (1) THE REFERENCE BEHAVIOUR: the same source compiled straight to the exec
    // format. Whatever image it names for `puts` is what the archive path owes.
    // The lib source and its client in ONE translation unit — an exec format
    // needs an entry point, and this is exactly the "compiled DIRECTLY as one
    // multi-TU exec" arm of the CLI measurement in the docblock above.
    DiagnosticReporter directRep;
    auto directMod = assembleFromSource(
        std::string{kPutsLibSrc} + std::string{kMainSrc}, "direct.c", s,
        *s.exec, directRep);
    ASSERT_TRUE(directMod) << familyLabel << ": direct assemble failed; errs="
                           << directRep.errorCount();
    auto const putsName = onBinaryNameEndingIn(*directMod, "puts");
    ASSERT_TRUE(putsName) << familyLabel
                          << ": the direct build must import puts at all";
    auto const referenceLibrary = importLibraryOf(*directMod, *putsName);
    ASSERT_TRUE(referenceLibrary) << familyLabel << ": direct import lookup";
    ASSERT_FALSE(referenceLibrary->empty())
        << familyLabel
        << ": the DIRECT path must bind puts to an owning image — without that "
           "there is no reference behaviour for the archive path to match";

    // (2) THE ARCHIVE ROUND TRIP over the identical source.
    ScratchDir dir{Location::InsideRepo, "static-link"};
    auto const archive = buildArchiveThroughStaticlibFormat(
        dir.path(), "libanswer.a", {{std::string{kPutsLibSrc}, "lib"}}, s);
    ASSERT_FALSE(archive.empty()) << familyLabel << ": archive build";

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s,
                                      *s.exec, mainRep);
    ASSERT_TRUE(mainMod) << familyLabel << ": main assemble failed; errs="
                         << mainRep.errorCount();

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, kNoDynamicLibraries, *s.target,
                                           *s.exec, pullRep);
    ASSERT_TRUE(pulled) << familyLabel << ": pull failed; errs="
                        << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u) << familyLabel << ": one defining member";
    EXPECT_EQ(pullRep.errorCount(), 0u) << familyLabel;

    // ★ THE ASSERTION THE DEFECT FAILED: the member read back out of the archive
    // must carry the SAME owning image the direct build derived. Before the fix
    // this was the empty string on every leg.
    auto const pulledLibrary = importLibraryOf((*pulled)[0], *putsName);
    ASSERT_TRUE(pulledLibrary)
        << familyLabel << ": the pulled member must still import " << *putsName;
    EXPECT_FALSE(pulledLibrary->empty())
        << familyLabel
        << ": the pulled member's " << *putsName << " import lost its owning "
           "library — the object file records only the NAME, so the binding must "
           "be re-derived from the shipped-descriptor corpus at pull time "
           "(D-LK-ARCHIVE-MEMBER-EXTERN-LOSES-ITS-LIBRARY)";
    EXPECT_EQ(*pulledLibrary, *referenceLibrary)
        << familyLabel
        << ": the archive path must reach the SAME image as the direct path, or "
           "the two rows do not fold in the linker's (name, library, version) "
           "dedup and one C runtime becomes two imports";

    // (3) AND THE WHOLE POINT: the combined span must LINK. This is the surface
    // the CLI reported rc=1 on.
    std::vector<AssembledModule> combined;
    combined.push_back(*mainMod);
    combined.push_back((*pulled)[0]);
    DiagnosticReporter linkRep;
    auto image = linker::link(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        *s.target, *s.exec, linkRep);
    EXPECT_EQ(countCode(linkRep, DiagnosticCode::K_SymbolUndefined), 0u)
        << familyLabel
        << ": a library call inside an archive member is not an undefined symbol";
    EXPECT_EQ(linkRep.errorCount(), 0u) << familyLabel << ": link must be clean";
    EXPECT_TRUE(image.ok()) << familyLabel;
}

TEST(ArchiveMemberLibraryImport, ElfMemberRebindsToTheImageTheDirectPathBinds) {
    Schemas s = loadSchemas();
    loadStaticlibSchema(s, "elf64-x86_64-linux-staticlib");
    expectArchiveMemberRebindsLibraryImport(s, "elf64-x86_64");
}

TEST(ArchiveMemberLibraryImport, CoffMemberRebindsToTheImageTheDirectPathBinds) {
    Schemas s = loadCoffSchemas();
    loadStaticlibSchema(s, "pe64-x86_64-windows-staticlib");
    expectArchiveMemberRebindsLibraryImport(s, "pe64-x86_64");
}

TEST(ArchiveMemberLibraryImport, MachOMemberRebindsToTheImageTheDirectPathBinds) {
    Schemas s = loadMachoSchemas();
    loadStaticlibSchema(s, "macho64-arm64-darwin-staticlib");
    expectArchiveMemberRebindsLibraryImport(s, "macho64-arm64");
}

// ★★★ THE HALF A CANONICAL-NAME LOOKUP MISSES — AND CHOOSING THE SYMBOL FOR
// THIS TEST TOOK A MUTANT RUN, BECAUSE THE OBVIOUS CHOICE ASSERTED NOTHING.
//
// The corpus is keyed on the C identifier, but a compiled TU emits the PLATFORM
// LINK NAME, so a member's symbol table can hold a spelling that is not a corpus
// key at all. Un-decorating it recovers that same spelling, the forward lookup
// misses, and the whole family becomes un-archivable.
//
// ⚠⚠ BUT MOST `linkName` ROWS ARE **ALSO** DECLARED UNDER THEIR LINK SPELLING,
// AND THOSE PROVE NOTHING HERE. ✔MEASURED over the shipped corpus: `time` carries
// `linkName: "_time64"` AND `time.json` separately declares a first-class
// `_time64` row — so `_time64` IS a corpus key and the FORWARD arm finds it
// unaided. The same is true of `open` / `close` / `read` / `access` / `isatty` /
// `dup` / `dup2` / `getcwd` / `chdir` / `rmdir` / `unlink` and the rest of the pe
// `<io.h>` and `<time.h>` families. A test written on `time` therefore passes
// with the reverse arm DELETED — ✔MEASURED, exactly that mutant left an earlier
// draft of this test GREEN.
//
// EXACTLY NINE rows in the shipped corpus realize to a link name that is NOT
// itself declared: `write`→`_write`, `lseek`→`_lseek`, `getpid`→`_getpid`,
// `_setjmp`→`__intrinsic_setjmp`, and the five Darwin suffixed ones
// (`opendir` / `readdir` / `statfs` / `fstatfs` → `…$INODE64`, `realpath` →
// `realpath$DARWIN_EXTSN`). Those nine are the ONLY witnesses of the reverse
// arm, so this test uses one of them — `write`. ✔MEASURED with the reverse arm
// mutated out (shipped CLI, pe64-x86_64): the archived TU`s exec link failed
// with `undefined symbol '_write'`.
//
// pe64 is used because the arm64 Darwin family (the Mach-O leg this suite can
// build everywhere) carries none of the suffixed rows — they are x86_64-gated.
// Nothing about the mechanism is pe-specific: the index is built through
// `linkNameFor` under the format`s OWN declared decoration rule.
//
// ★ THE PROTOTYPE IS HAND-WRITTEN RATHER THAN INCLUDED, AND THAT CHANGES NOTHING
// ABOUT WHAT IS ASSERTED. Since UCRT-P4 the DECLARATION SYNTAX HAS NO AUTHORITY
// OVER REALIZATION: a hand-written prototype and an included one resolve through
// the SAME descriptor row and produce a BYTE-IDENTICAL import (C23 6.2.2p5 +
// 7.1.4p2). The hand-written form is used because this in-process harness feeds
// `UnitBuilder::addInMemory` with no system include search path, so the include
// form fails to preprocess here for reasons unrelated to the property under test.
constexpr std::string_view kLinkNameLibSrc =
    "extern int write(int fd, const void *buf, unsigned n);\n"
    "int dss_lib_answer(void){ write(1, \"\", 0); return 42; }\n";

TEST(ArchiveMemberLibraryImport, CoffMemberBindsARowCarryingAPlatformLinkName) {
    Schemas s = loadCoffSchemas();
    loadStaticlibSchema(s, "pe64-x86_64-windows-staticlib");
    ASSERT_TRUE(s.target && s.exec && s.staticlib && s.grammar);

    // The reference behaviour again: what does the direct build bind, and under
    // which on-binary spelling? Both are READ from it, never spelled here.
    DiagnosticReporter directRep;
    auto directMod = assembleFromSource(
        std::string{kLinkNameLibSrc} + std::string{kMainSrc}, "direct.c", s,
        *s.exec, directRep);
    ASSERT_TRUE(directMod) << "direct assemble failed; errs="
                           << directRep.errorCount();
    auto const linkedName = onBinaryNameEndingIn(*directMod, "write");
    ASSERT_TRUE(linkedName)
        << "the direct build must import the platform's link name for write() — "
           "if this stops holding, the corpus row changed and this test is "
           "asserting the wrong thing";
    auto const referenceLibrary = importLibraryOf(*directMod, *linkedName);
    ASSERT_TRUE(referenceLibrary && !referenceLibrary->empty())
        << "direct build must bind " << *linkedName;

    ScratchDir dir{Location::InsideRepo, "static-link"};
    auto const archive = buildArchiveThroughStaticlibFormat(
        dir.path(), "libanswer.a", {{std::string{kLinkNameLibSrc}, "lib"}}, s);
    ASSERT_FALSE(archive.empty());

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s,
                                      *s.exec, mainRep);
    ASSERT_TRUE(mainMod) << "main assemble failed; errs=" << mainRep.errorCount();

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, kNoDynamicLibraries, *s.target,
                                           *s.exec, pullRep);
    ASSERT_TRUE(pulled) << "pull failed; errs=" << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u);

    auto const pulledLibrary = importLibraryOf((*pulled)[0], *linkedName);
    ASSERT_TRUE(pulledLibrary)
        << "the pulled member must still import " << *linkedName;
    EXPECT_EQ(*pulledLibrary, *referenceLibrary)
        << "a descriptor row carrying a per-target linkName must bind through "
           "the REALIZED link name: the member's symbol table holds '"
        << *linkedName
        << "', which is not a corpus key, so a canonical-name-only lookup misses "
           "it — and this is one of the NINE rows whose link name the corpus "
           "does not separately declare, so the forward arm cannot cover it";

    std::vector<AssembledModule> combined;
    combined.push_back(*mainMod);
    combined.push_back((*pulled)[0]);
    DiagnosticReporter linkRep;
    auto image = linker::link(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        *s.target, *s.exec, linkRep);
    EXPECT_EQ(countCode(linkRep, DiagnosticCode::K_SymbolUndefined), 0u);
    EXPECT_EQ(linkRep.errorCount(), 0u);
    EXPECT_TRUE(image.ok());
}

// ★★★ AND THE OTHER DIRECTION, WHICH MATTERS AT LEAST AS MUCH: a binder that
// bound optimistically would turn a typo into a LOAD-time failure, which is
// strictly worse than the defect it fixes. A name the corpus does not answer for
// must stay UNBOUND and must still be rejected LOUD, with the message unchanged.
constexpr std::string_view kBogusLibSrc =
    "extern int dss_no_such_symbol_anywhere(int v);\n"
    "int dss_lib_answer(void){ return dss_no_such_symbol_anywhere(41); }\n";

void expectGenuinelyUndefinedStillFailsLoud(Schemas const& s,
                                            std::string_view familyLabel) {
    ASSERT_TRUE(s.target && s.exec && s.staticlib && s.grammar) << familyLabel;
    ScratchDir dir{Location::InsideRepo, "static-link"};
    auto const archive = buildArchiveThroughStaticlibFormat(
        dir.path(), "libanswer.a", {{std::string{kBogusLibSrc}, "lib"}}, s);
    ASSERT_FALSE(archive.empty()) << familyLabel;

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s,
                                      *s.exec, mainRep);
    ASSERT_TRUE(mainMod) << familyLabel;

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, kNoDynamicLibraries, *s.target,
                                           *s.exec, pullRep);
    ASSERT_TRUE(pulled) << familyLabel;
    ASSERT_EQ(pulled->size(), 1u) << familyLabel;

    auto const bogusName =
        onBinaryNameEndingIn((*pulled)[0], "dss_no_such_symbol_anywhere");
    ASSERT_TRUE(bogusName) << familyLabel << ": the member must import it";
    auto const bogusLibrary = importLibraryOf((*pulled)[0], *bogusName);
    ASSERT_TRUE(bogusLibrary) << familyLabel;
    EXPECT_TRUE(bogusLibrary->empty())
        << familyLabel
        << ": a name no descriptor declares must stay UNBOUND — binding it to "
           "any image would replace a build error with a loader failure. Got '"
        << *bogusLibrary << "'";

    std::vector<AssembledModule> combined;
    combined.push_back(*mainMod);
    combined.push_back((*pulled)[0]);
    DiagnosticReporter linkRep;
    (void)linker::link(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        *s.target, *s.exec, linkRep);
    EXPECT_GE(countCode(linkRep, DiagnosticCode::K_SymbolUndefined), 1u)
        << familyLabel
        << ": a genuinely undefined symbol reached through an archive member "
           "must still be rejected LOUD by the reference gate";
    bool namedTheSymbol = false;
    for (auto const& d : linkRep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
            && d.actual.find(*bogusName) != std::string::npos) {
            namedTheSymbol = true;
        }
    }
    EXPECT_TRUE(namedTheSymbol)
        << familyLabel
        << ": the rejection must NAME the symbol — the message quality is part "
           "of the contract, not incidental";
}

TEST(ArchiveMemberLibraryImport, ElfGenuinelyUndefinedSymbolStillFailsLoud) {
    Schemas s = loadSchemas();
    loadStaticlibSchema(s, "elf64-x86_64-linux-staticlib");
    expectGenuinelyUndefinedStillFailsLoud(s, "elf64-x86_64");
}

TEST(ArchiveMemberLibraryImport, CoffGenuinelyUndefinedSymbolStillFailsLoud) {
    Schemas s = loadCoffSchemas();
    loadStaticlibSchema(s, "pe64-x86_64-windows-staticlib");
    expectGenuinelyUndefinedStillFailsLoud(s, "pe64-x86_64");
}

// ★★★ A `synthesize` RECIPE ROW MUST NEVER BE BOUND, AND THE MEMBER HAS TO BE
// HAND-BUILT TO REACH THE ARM AT ALL.
//
// ✔MEASURED: a DSS-compiled pe64 member never carries a bare `printf` reference
// — with a `<stdio.h>` include OR with a hand-written prototype, the semantic
// tier claims the shim, so the archive DEFINES `printf` and imports the UCRT
// cores instead. (Those cores were the pe64 half of this very defect, and they
// bind through the ordinary arm.) The recipe arm is therefore reachable only
// from a FOREIGN archive whose member references `printf` directly, which is
// what this module imitates: an `ExternImport` with no library plus a relocation
// naming it, exactly what a gcc- or MSVC-produced object contains.
//
// Binding it would request a symbol the UCRT image does not export — the binary
// would link clean and die at LOAD with 0xC0000139 and no diagnostic anywhere.
// Leaving it unbound produces a build error instead.
TEST(ArchiveMemberLibraryImport, CoffSynthesizeRecipeRowIsNeverBound) {
    Schemas s = loadCoffSchemas();
    loadStaticlibSchema(s, "pe64-x86_64-windows-staticlib");
    ASSERT_TRUE(s.target && s.exec && s.staticlib && s.grammar);

    // A member shaped like a foreign object: `dss_lib_answer` calls `printf`,
    // which it imports with NO owning library (all an object file can record).
    AssembledModule member;
    AssembledFunction answer;
    answer.symbol = SymbolId{1};
    answer.bytes.assign(16, 0x90);
    answer.relocations.push_back(
        Relocation{4u, SymbolId{9}, RelocationKind{1}, 0});
    member.functions.push_back(std::move(answer));
    member.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_lib_answer",
                                          SymbolBinding::Global,
                                          SymbolVisibility::Default});
    member.externImports.push_back(
        ExternImport{SymbolId{9}, "printf", "", /*isData=*/false});
    member.expectedFuncCount = member.functions.size();

    ScratchDir dir{Location::InsideRepo, "static-link"};
    auto const archivePath = dir.path() / "libanswer.a";
    {
        std::vector<AssembledModule> mods;
        mods.push_back(std::move(member));
        std::vector<std::string> const names{"lib"};
        DiagnosticReporter writeRep;
        ASSERT_TRUE(linkAndWriteStaticArchive(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            std::span<std::string const>{names.data(), names.size()},
            *s.target, *s.staticlib, archivePath, writeRep))
            << "archive write failed; errs=" << writeRep.errorCount();
    }

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s,
                                      *s.exec, mainRep);
    ASSERT_TRUE(mainMod);

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archivePath};
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, kNoDynamicLibraries, *s.target,
                                           *s.exec, pullRep);
    ASSERT_TRUE(pulled) << "pull failed; errs=" << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u);

    auto const printfLibrary = importLibraryOf((*pulled)[0], "printf");
    ASSERT_TRUE(printfLibrary)
        << "the pulled member must still import printf";
    EXPECT_TRUE(printfLibrary->empty())
        << "this platform realizes printf as a COMPILER-SYNTHESIZED body, not as "
           "a library export; binding the name would produce an image that links "
           "clean and fails to LOAD. Got '"
        << *printfLibrary << "'";
}

// ── D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY ─────────────────
//
// ★★★ THE SUITE ABOVE CLOSED THE **PLATFORM** HALF. THIS IS THE OTHER HALF:
// A LIBRARY THE OPERATOR NAMED ON THE COMMAND LINE.
//
// Same root cause, different authority. An object file records an undefined
// symbol's NAME and nothing else, so a member's library binding is lost at
// archive-write time and has to be re-derived at pull time. `pullStaticArchive-
// Members` learned to ask the shipped-descriptor corpus — which answers for
// `puts` and `ucrtbase.dll`, and has never heard of a library the operator built
// five seconds ago and pointed the build at with `--resolve-library`. That
// binary was invisible to the pull, because the pull was never handed the list.
//
// ✔MEASURED before the fix (shipped CLI, this host, baseline config, pe64): a
// three-step build — a `.dll` defining `dss_dyn_answer`; a `-staticlib` calling
// it, built WITH `--resolve-library <dll>`; then an exec linking the client
// against BOTH — returned rc=0, rc=0, **rc=1**, with
// `error[K_SymbolUndefined] undefined symbol 'dss_dyn_answer'`. The gap PREDATES
// the platform-half fix: it was narrowed by it, never introduced by it.
//
// ★★ THE ORACLE IS THE OPERATOR'S OWN STATED IDENTITY, NOT A LIBRARY NAME THIS
// FILE SPELLS. `--resolve-library <path>=<import-name>` STATES the runtime
// identity to record, so a case that passes one knows exactly what the member's
// row must come back carrying, on every format, with no platform string in the
// test. The same lever makes the PRECEDENCE case below possible: it is the only
// way to name an image the shipped corpus provably would NOT have chosen.

// The dynamic library's source, its archived caller, and that caller's client —
// the three steps of the measured CLI repro, in-process.
constexpr std::string_view kDynLibSrc =
    "int dss_dyn_answer(void){ return 42; }\n";
constexpr std::string_view kUsesDynLibSrc =
    "extern int dss_dyn_answer(void);\n"
    "int dss_lib_answer(void){ return dss_dyn_answer(); }\n";

// DSS writes a real DYNAMIC library (ELF `.so` / PE `.dll` / Mach-O `.dylib`)
// whose EXPORT TABLE the `--resolve-library` reader then reads. Nothing is
// stubbed: this is the artifact the CLI's step 1 emits, through the same
// `linkAndWrite` the driver calls.
[[nodiscard]] fs::path
buildDynamicLibrary(fs::path const& dir, std::string_view fileName,
                    std::string_view src, Schemas const& s,
                    ObjectFormatSchema const& dynFormat) {
    DiagnosticReporter rep;
    auto mod = assembleFromSource(std::string{src}, "dynsrc.c", s, dynFormat, rep);
    if (!mod) {
        ADD_FAILURE() << "dynamic-library assemble failed; errs=" << rep.errorCount();
        return {};
    }
    auto const outPath = dir / std::string{fileName};
    std::vector<AssembledModule> mods;
    mods.push_back(std::move(*mod));
    DiagnosticReporter writeRep;
    if (!linkAndWrite(std::span<AssembledModule const>{mods.data(), mods.size()},
                      *s.target, dynFormat, outPath, writeRep)) {
        ADD_FAILURE() << "dynamic-library link/write failed; errs="
                      << writeRep.errorCount();
        return {};
    }
    return outPath;
}

// The STATED identity — what `--resolve-library <path>=<import-name>` records.
// Naming it here is what lets the assertions below be exact without spelling a
// platform image anywhere.
constexpr std::string_view kStatedIdentity = "dss-operator-named-lib";

// One family's worth of the repro. `s` must carry `exec` + `staticlib`;
// `dynFormat` is the family's shared-library format.
void expectArchiveMemberBindsAnOperatorNamedLibrary(
    Schemas const& s, ObjectFormatSchema const& dynFormat,
    std::string_view familyLabel) {
    ASSERT_TRUE(s.target && s.exec && s.staticlib && s.grammar)
        << familyLabel << ": schemas must load";

    ScratchDir dir{Location::InsideRepo, "static-link"};
    auto const dynPath =
        buildDynamicLibrary(dir.path(), "dss_dyn", kDynLibSrc, s, dynFormat);
    ASSERT_FALSE(dynPath.empty()) << familyLabel << ": dynamic library build";

    auto const archive = buildArchiveThroughStaticlibFormat(
        dir.path(), "libusesdyn.a", {{std::string{kUsesDynLibSrc}, "usesdyn"}}, s);
    ASSERT_FALSE(archive.empty()) << familyLabel << ": archive build";

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s,
                                      *s.exec, mainRep);
    ASSERT_TRUE(mainMod) << familyLabel << ": client assemble failed; errs="
                         << mainRep.errorCount();

    std::vector<ResolveLibrarySpec> const dynamicLibs{
        ResolveLibrarySpec{dynPath, std::string{kStatedIdentity}}};

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(
        *mainMod, archives,
        std::span<ResolveLibrarySpec const>{dynamicLibs}, *s.target, *s.exec,
        pullRep);
    ASSERT_TRUE(pulled) << familyLabel << ": pull failed; errs="
                        << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u) << familyLabel << ": one defining member";
    EXPECT_EQ(pullRep.errorCount(), 0u) << familyLabel;

    auto const dynName = onBinaryNameEndingIn((*pulled)[0], "dss_dyn_answer");
    ASSERT_TRUE(dynName)
        << familyLabel << ": the pulled member must still import dss_dyn_answer";

    // ★ THE ASSERTION THE DEFECT FAILED. Before the fix this was the empty
    // string on every leg — the corpus has no row for this name, and the
    // operator's binary was never shown to the pull at all.
    auto const pulledLibrary = importLibraryOf((*pulled)[0], *dynName);
    ASSERT_TRUE(pulledLibrary) << familyLabel << ": pulled import lookup";
    EXPECT_EQ(*pulledLibrary, kStatedIdentity)
        << familyLabel
        << ": the pulled member's " << *dynName << " import must bind to the "
           "library the OPERATOR named, under the identity the operator STATED. "
           "An object file records only the NAME, so the binding has to be "
           "re-derived at pull time from the `--resolve-library` list — the "
           "shipped-descriptor corpus structurally cannot answer for a library "
           "it has never heard of "
           "(D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY)";

    // AND THE WHOLE POINT: the combined span must LINK. This is the surface the
    // CLI reported rc=1 on.
    std::vector<AssembledModule> combined;
    combined.push_back(*mainMod);
    combined.push_back((*pulled)[0]);
    DiagnosticReporter linkRep;
    auto image = linker::link(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        *s.target, *s.exec, linkRep);
    EXPECT_EQ(linkRep.errorCount(), 0u) << familyLabel << ": link must be clean";
    EXPECT_TRUE(image.ok()) << familyLabel;
}

TEST(ArchiveMemberResolveLibraryImport, ElfMemberBindsAnOperatorNamedLibrary) {
    Schemas s = loadSchemas();
    loadStaticlibSchema(s, "elf64-x86_64-linux-staticlib");
    auto dynF = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(dynF) << "elf dyn schema load";
    expectArchiveMemberBindsAnOperatorNamedLibrary(s, **dynF, "elf64-x86_64");
}

TEST(ArchiveMemberResolveLibraryImport, CoffMemberBindsAnOperatorNamedLibrary) {
    Schemas s = loadCoffSchemas();
    loadStaticlibSchema(s, "pe64-x86_64-windows-staticlib");
    auto dynF = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-dll");
    ASSERT_TRUE(dynF) << "pe dll schema load";
    expectArchiveMemberBindsAnOperatorNamedLibrary(s, **dynF, "pe64-x86_64");
}

TEST(ArchiveMemberResolveLibraryImport, MachOMemberBindsAnOperatorNamedLibrary) {
    Schemas s = loadMachoSchemas();
    loadStaticlibSchema(s, "macho64-arm64-darwin-staticlib");
    auto dynF = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-dylib");
    ASSERT_TRUE(dynF) << "macho dylib schema load";
    expectArchiveMemberBindsAnOperatorNamedLibrary(s, **dynF, "macho64-arm64");
}

// ★★★ ORDER IS PRECEDENCE, AND THIS IS THE ONLY CASE THAT CAN SEE IT.
//
// The two arms both skip a row whose `libraryPath` is already set, so which one
// runs FIRST decides who wins a name they BOTH answer for. Every case above uses
// `dss_dyn_answer`, which the corpus has no row for — reversing the arms leaves
// them all green. This case uses `puts`: a plain descriptor row on every shipped
// format, AND an export of a stand-in library the operator names. If the corpus
// arm ran first, the member would come back bound to the platform's C runtime.
//
// ★ THE OPPOSING ANSWER IS MEASURED, NOT SPELLED. What the corpus would say is
// read off the DIRECT path — the same oracle the sibling suite uses — so the
// case asserts the two answers DIFFER and that the operator's won, with no
// platform image name written down anywhere.
constexpr std::string_view kStandinPutsSrc =
    "int puts(const char *s){ return s == 0 ? 0 : 1; }\n";

void expectOperatorNamedLibraryOutranksThePlatformCorpus(
    Schemas const& s, ObjectFormatSchema const& dynFormat,
    std::string_view familyLabel) {
    ASSERT_TRUE(s.target && s.exec && s.staticlib && s.grammar)
        << familyLabel << ": schemas must load";

    // What the PLATFORM says about `puts` here — the answer the operator must
    // outrank.
    DiagnosticReporter directRep;
    auto directMod = assembleFromSource(
        std::string{kPutsLibSrc} + std::string{kMainSrc}, "direct.c", s,
        *s.exec, directRep);
    ASSERT_TRUE(directMod) << familyLabel << ": direct assemble failed; errs="
                           << directRep.errorCount();
    auto const putsName = onBinaryNameEndingIn(*directMod, "puts");
    ASSERT_TRUE(putsName) << familyLabel << ": the direct build must import puts";
    auto const platformLibrary = importLibraryOf(*directMod, *putsName);
    ASSERT_TRUE(platformLibrary) << familyLabel << ": direct import lookup";
    ASSERT_FALSE(platformLibrary->empty())
        << familyLabel
        << ": the platform must bind puts to SOME image, or there is no "
           "competing answer for the operator's binary to outrank and this case "
           "asserts nothing";
    ASSERT_NE(*platformLibrary, kStatedIdentity)
        << familyLabel
        << ": the stated identity must differ from the platform's, or the "
           "comparison below cannot tell the two arms apart";

    ScratchDir dir{Location::InsideRepo, "static-link"};
    auto const dynPath = buildDynamicLibrary(dir.path(), "dss_standin",
                                             kStandinPutsSrc, s, dynFormat);
    ASSERT_FALSE(dynPath.empty()) << familyLabel << ": stand-in library build";

    auto const archive = buildArchiveThroughStaticlibFormat(
        dir.path(), "libputs.a", {{std::string{kPutsLibSrc}, "lib"}}, s);
    ASSERT_FALSE(archive.empty()) << familyLabel << ": archive build";

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s,
                                      *s.exec, mainRep);
    ASSERT_TRUE(mainMod) << familyLabel << ": client assemble failed";

    std::vector<ResolveLibrarySpec> const dynamicLibs{
        ResolveLibrarySpec{dynPath, std::string{kStatedIdentity}}};

    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(
        *mainMod, archives,
        std::span<ResolveLibrarySpec const>{dynamicLibs}, *s.target, *s.exec,
        pullRep);
    ASSERT_TRUE(pulled) << familyLabel << ": pull failed; errs="
                        << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u) << familyLabel;

    auto const pulledLibrary = importLibraryOf((*pulled)[0], *putsName);
    ASSERT_TRUE(pulledLibrary)
        << familyLabel << ": the pulled member must still import " << *putsName;
    EXPECT_EQ(*pulledLibrary, kStatedIdentity)
        << familyLabel
        << ": the operator NAMED a binary exporting " << *putsName
        << ", so its export table outranks the platform default — the "
           "`--resolve-library` arm must run BEFORE the shipped-descriptor arm. "
           "Got '" << *pulledLibrary << "'; the platform's answer is '"
        << *platformLibrary << "'";
}

TEST(ArchiveMemberResolveLibraryImport, ElfOperatorNamedLibraryOutranksTheCorpus) {
    Schemas s = loadSchemas();
    loadStaticlibSchema(s, "elf64-x86_64-linux-staticlib");
    auto dynF = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(dynF) << "elf dyn schema load";
    expectOperatorNamedLibraryOutranksThePlatformCorpus(s, **dynF, "elf64-x86_64");
}

TEST(ArchiveMemberResolveLibraryImport, CoffOperatorNamedLibraryOutranksTheCorpus) {
    Schemas s = loadCoffSchemas();
    loadStaticlibSchema(s, "pe64-x86_64-windows-staticlib");
    auto dynF = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-dll");
    ASSERT_TRUE(dynF) << "pe dll schema load";
    expectOperatorNamedLibraryOutranksThePlatformCorpus(s, **dynF, "pe64-x86_64");
}

// ★★ THE FAIL-LOUD DIRECTION, PINNED IN THE PRESENCE OF THE FLAG. A binder that
// widens what it accepts every time it misses would eventually bind a typo; the
// guard has to hold with a `--resolve-library` binary in hand. The member's
// reference names a symbol NOTHING defines and the named library does NOT
// export, so it must come back UNBOUND and the link must still refuse it with
// the full `K_SymbolUndefined` message.
TEST(ArchiveMemberResolveLibraryImport, GenuinelyUndefinedSymbolStillFailsLoud) {
    Schemas s = loadSchemas();
    loadStaticlibSchema(s, "elf64-x86_64-linux-staticlib");
    auto dynF = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(dynF) << "elf dyn schema load";
    ASSERT_TRUE(s.target && s.exec && s.staticlib && s.grammar);

    ScratchDir dir{Location::InsideRepo, "static-link"};
    auto const dynPath =
        buildDynamicLibrary(dir.path(), "dss_dyn", kDynLibSrc, s, **dynF);
    ASSERT_FALSE(dynPath.empty());

    // The member calls a name the stand-in library does NOT export and no
    // descriptor row realizes.
    constexpr std::string_view kTypoLibSrc =
        "extern int dss_no_such_symbol_anywhere(void);\n"
        "int dss_lib_answer(void){ return dss_no_such_symbol_anywhere(); }\n";
    auto const archive = buildArchiveThroughStaticlibFormat(
        dir.path(), "libtypo.a", {{std::string{kTypoLibSrc}, "typo"}}, s);
    ASSERT_FALSE(archive.empty());

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", s,
                                      *s.exec, mainRep);
    ASSERT_TRUE(mainMod);

    std::vector<ResolveLibrarySpec> const dynamicLibs{
        ResolveLibrarySpec{dynPath, std::string{kStatedIdentity}}};
    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(
        *mainMod, archives,
        std::span<ResolveLibrarySpec const>{dynamicLibs}, *s.target, *s.exec,
        pullRep);
    ASSERT_TRUE(pulled) << "pull failed; errs=" << pullRep.errorCount();
    ASSERT_EQ(pulled->size(), 1u);
    EXPECT_EQ(pullRep.errorCount(), 0u)
        << "an unbindable member reference is not the PULL's error — the LINK "
           "tier judges the reference (C23 5.1.1.2 phase 8)";

    auto const typoLibrary =
        importLibraryOf((*pulled)[0], "dss_no_such_symbol_anywhere");
    ASSERT_TRUE(typoLibrary) << "the pulled member must still import the name";
    EXPECT_TRUE(typoLibrary->empty())
        << "nothing defines this symbol and the named library does not export "
           "it, so the row must stay UNBOUND. Got '" << *typoLibrary << "'";

    std::vector<AssembledModule> combined;
    combined.push_back(*mainMod);
    combined.push_back((*pulled)[0]);
    DiagnosticReporter linkRep;
    auto image = linker::link(
        std::span<AssembledModule const>{combined.data(), combined.size()},
        *s.target, *s.exec, linkRep);
    EXPECT_FALSE(image.ok())
        << "a genuinely undefined symbol must still fail the link";
    EXPECT_GE(countCode(linkRep, DiagnosticCode::K_SymbolUndefined), 1u)
        << "the undefined reference must still be rejected LOUD by the "
           "reference gate — the presence of a --resolve-library binary must "
           "not weaken the guard";
    bool namedTheSymbol = false;
    for (auto const& d : linkRep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
            && d.actual.find("dss_no_such_symbol_anywhere") != std::string::npos) {
            namedTheSymbol = true;
        }
    }
    EXPECT_TRUE(namedTheSymbol)
        << "the rejection must NAME the symbol — the message quality is part of "
           "the contract, not incidental";
}

// ★★ A NAMED BINARY THAT CANNOT BE READ REFUSES THE PULL — it must never leave
// the flag QUIETLY INEFFECTIVE, which is the shape of the whole anchor. Feeding
// an ELF link a PE `.dll` is caught by the target-aware read chokepoint
// (D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL) — the two formats are
// both undecorated, so nothing else would catch it and the image would record an
// import naming a library of the wrong format: a LOAD-time death with no
// build-time signal.
TEST(ArchiveMemberResolveLibraryImport, WrongFormatNamedBinaryRefusesThePull) {
    Schemas elf = loadSchemas();
    loadStaticlibSchema(elf, "elf64-x86_64-linux-staticlib");
    ASSERT_TRUE(elf.target && elf.exec && elf.staticlib && elf.grammar);

    Schemas pe = loadCoffSchemas();
    auto peDynF = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-dll");
    ASSERT_TRUE(peDynF) << "pe dll schema load";

    ScratchDir dir{Location::InsideRepo, "static-link"};
    // A genuine PE `.dll`, handed to an ELF link.
    auto const peDll =
        buildDynamicLibrary(dir.path(), "dss_dyn.dll", kDynLibSrc, pe, **peDynF);
    ASSERT_FALSE(peDll.empty());

    auto const archive = buildArchiveThroughStaticlibFormat(
        dir.path(), "libusesdyn.a", {{std::string{kUsesDynLibSrc}, "usesdyn"}},
        elf);
    ASSERT_FALSE(archive.empty());

    DiagnosticReporter mainRep;
    auto mainMod = assembleFromSource(std::string{kMainSrc}, "main.c", elf,
                                      *elf.exec, mainRep);
    ASSERT_TRUE(mainMod);

    std::vector<ResolveLibrarySpec> const dynamicLibs{
        ResolveLibrarySpec{peDll, ""}};
    DiagnosticReporter pullRep;
    std::vector<fs::path> const archives{archive};
    auto pulled = pullStaticArchiveMembers(
        *mainMod, archives,
        std::span<ResolveLibrarySpec const>{dynamicLibs}, *elf.target,
        *elf.exec, pullRep);
    EXPECT_FALSE(pulled.has_value())
        << "a --resolve-library binary of the WRONG object format must REFUSE "
           "the pull, not be silently skipped";
    EXPECT_GT(pullRep.errorCount(), 0u)
        << "and the refusal must be reported — the chokepoint names the path "
           "and the structural cause";
}

// ★★★ THE DRIVER-LEVEL PIN, AND IT EXISTS BECAUSE A MUTANT PROVED THE UNIT
// CASES ABOVE DO NOT COVER WHAT THE DEFECT ACTUALLY WAS.
//
// Every case above calls `pullStaticArchiveMembers` DIRECTLY and hands it the
// library list itself. That asserts the pull binds correctly — it asserts
// NOTHING about whether the DRIVER ever gives it the list, which is precisely
// the half that was broken: `pullStaticArchiveMembers` was never handed
// `--resolve-library` at all.
//
// ✔MEASURED (this cycle, mutant D): dropping the threading at ONE of the three
// `linkAndWriteWithStaticArchives` call sites in `Program`'s per-target compile
// reproduced the original CLI failure EXACTLY — rc=1,
// `undefined symbol 'dss_dyn_answer'` — while the whole in-process suite above
// stayed GREEN, 32/32. A gap no test can see is the shape this repo has already
// paid for once (two corpus runners, one enforcing and its sibling shrugging),
// so the threading gets its own witness through the real driver.
//
// THREE SEQUENTIAL IN-PROCESS `Program` BUILDS INTO ONE SHARED OUTPUT DIR — the
// same shape `PeFatArchiveSequentialInProcessMergeExecRunsFortyTwo` uses, and
// the same three steps as the measured CLI repro:
//   1. dynsrc.dll  (pe64 dll)       defining dss_dyn_answer() = 42
//   2. usesdyn.lib (pe64 staticlib) calling it, RESOLVING dynsrc.dll
//   3. client.exe  (pe64 exec)      calling dss_lib_answer, RESOLVING BOTH
// Step 3 is the one that returned rc=1 before the fix: the member pulled out of
// usesdyn.lib carries `dss_dyn_answer` as a bare NAME, and only the operator's
// `--resolve-library dynsrc.dll` can say which image owns it.
TEST(StaticLink, PeArchiveMemberBindsAnOperatorNamedLibraryThroughTheDriver) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();

    // ── Build 1: dynsrc.dll — the operator's own dynamic library ──
    auto const dynSrc = writeSrc(dir, "dynsrc.c", kDynLibSrc);
    Program pDyn;
    pDyn.setOutputDir(dir);
    DiagnosticReporter repDyn;
    ASSERT_EQ(pDyn.compileFiles(
                  std::vector<std::string>{dynSrc.string()}, "c",
                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-dll"},
                  repDyn),
              0) << "dynsrc.dll build must succeed; errs=" << repDyn.errorCount();
    auto const dynDll = dir / "dynsrc.dll";
    ASSERT_TRUE(fs::exists(dynDll)) << "the driver must emit dynsrc.dll";

    // ── Build 2: usesdyn.lib — a STATIC library whose member calls into it ──
    // Built WITH `--resolve-library dynsrc.dll`, exactly as the CLI repro does.
    // The binding is correct HERE and is then LOST: the archive member records
    // `dss_dyn_answer` as a name and no format has anywhere to put the library.
    auto const usesSrc = writeSrc(dir, "usesdyn.c", kUsesDynLibSrc);
    Program pLib;
    pLib.setOutputDir(dir);
    pLib.setResolveLibraries(std::vector<fs::path>{dynDll});
    DiagnosticReporter repLib;
    ASSERT_EQ(pLib.compileFiles(
                  std::vector<std::string>{usesSrc.string()}, "c",
                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-staticlib"},
                  repLib),
              0) << "usesdyn.lib build must succeed; errs=" << repLib.errorCount();
    auto const usesLib = dir / "usesdyn.lib";
    ASSERT_TRUE(fs::exists(usesLib)) << "the driver must emit usesdyn.lib";

    // ── Build 3: client.exe — THE STEP THAT RETURNED rc=1 ──
    auto const clientSrc = writeSrc(dir, "client.c", kMainSrc);
    Program pClient;
    pClient.setOutputDir(dir);
    pClient.setResolveLibraries(std::vector<fs::path>{dynDll, usesLib});
    DiagnosticReporter repClient;
    int const rcClient = pClient.compileFiles(
        std::vector<std::string>{clientSrc.string()}, "c",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, repClient);
    EXPECT_EQ(countCode(repClient, DiagnosticCode::K_SymbolUndefined), 0u)
        << "the pulled member's dss_dyn_answer must bind to the library the "
           "OPERATOR named. Before the fix this reported K_SymbolUndefined — the "
           "driver never handed the `--resolve-library` list to the archive pull "
           "(D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-LIBRARY)";
    ASSERT_EQ(rcClient, 0) << "client.exe link must succeed; errs="
                           << repClient.errorCount();
    auto const clientExe = dir / "client.exe";
    ASSERT_TRUE(fs::exists(clientExe)) << "the driver must emit client.exe";

#if defined(_WIN32)
    // RUN (Windows host): `dss_lib_answer`'s body came out of usesdyn.lib and
    // its call to `dss_dyn_answer` is a real PE import of dynsrc.dll, which sits
    // beside the exe. A binding that named the wrong image — or none — would
    // fault at LOAD (0xC0000139) rather than return 42, which is exactly why
    // this witness is a RUN and not a header inspection.
    auto const r = runBinary(clientExe);
    ASSERT_TRUE(r.spawned) << "client.exe must spawn. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_dyn_answer(), reached "
           "through an archive member whose import was re-bound at pull time to "
           "the operator-named dynsrc.dll";
#endif  // _WIN32
}
