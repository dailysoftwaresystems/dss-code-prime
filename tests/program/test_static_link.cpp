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
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
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
    std::shared_ptr<GrammarSchema const> grammar;
};

[[nodiscard]] Schemas loadSchemas() {
    Schemas s;
    auto t = TargetSchema::loadShipped("x86_64");
    auto r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    auto e = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    auto g = GrammarSchema::loadShipped("c-subset");
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
    auto g = GrammarSchema::loadShipped("c-subset");
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
    auto g = GrammarSchema::loadShipped("c-subset");
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

// Compile one c-subset source string to an AssembledModule for `format`.
[[nodiscard]] std::optional<AssembledModule>
assembleFromSource(std::string src, std::string label, Schemas const& s,
                   ObjectFormatSchema const& format, DiagnosticReporter& rep) {
    UnitBuilder builder{s.grammar};
    builder.addInMemory(std::move(src), std::move(label));
    CompilationUnit cu = std::move(builder).finish();
    std::uint16_t const cc = ccIndexFor(*s.target, format, rep);
    return assembleUnit(cu, *s.grammar, *s.target, format, cc, rep);
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
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, *s.target,
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

    // RED-ON-DISABLE: WITHOUT the pulled member, dss_lib_answer stays an
    // unresolved import (the exact state the static pull removes).
    DiagnosticReporter aloneRep;
    auto imageAlone = linker::link(
        std::span<AssembledModule const>{&*mainMod, 1}, *s.target, *s.exec, aloneRep);
    EXPECT_TRUE(importsContain(imageAlone.externImportNames, "dss_lib_answer"))
        << "without the static pull, dss_lib_answer is an unresolved import";
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
    auto pulled = pullStaticArchiveMembers(*clientMod, archives, *s.target,
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
        std::vector<std::string>{mainSrc.string()}, "c-subset",
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
    auto const r = runBinary(mainPath, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << "main must spawn. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_lib_answer() pulled from "
           "libdsslib.a, merged into main, and called.";

    // RED-ON-DISABLE: WITHOUT --resolve-library, dss_lib_answer is left an
    // undefined dynamic symbol -> the build still succeeds (elf-exec defers
    // undefined symbols to ld.so) but the RUN fails (symbol lookup error). The
    // ONLY thing that makes main exit 42 is the static pull+merge.
    ScratchDir scratchNo{Location::InsideRepo, "static-link"};
    auto const dirNo = scratchNo.path();
    auto const mainNo = writeSrc(dirNo, "main.c", kMainSrc);
    Program pNo;
    pNo.setOutputDir(dirNo);
    DiagnosticReporter repNo;
    ASSERT_EQ(pNo.compileFiles(std::vector<std::string>{mainNo.string()}, "c-subset",
                  std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"}, repNo), 0);
    auto const r2 = runBinary(dirNo / "main", std::chrono::milliseconds{5000});
    EXPECT_TRUE(!r2.spawned || r2.exitCode != 42u)
        << "WITHOUT the static pull, main must NOT exit 42 (undefined symbol at "
           "load). spawned=" << r2.spawned << " exit=" << r2.exitCode;
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
        std::vector<std::string>{mainSrc.string()}, "c-subset",
        std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"}, rep);
    ASSERT_EQ(rc, 0)
        << "static-link of the real gcc jump-table lib must succeed (the reader must "
           "resolve its section-relative relocs); errs=" << rep.errorCount();
    auto const mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath)) << "the self-contained exec must exist";

#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    auto const r = runBinary(mainPath, std::chrono::milliseconds{5000});
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
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, *s.target, *s.exec, pullRep);
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

    // RED-ON-DISABLE: main ALONE (no pulled member) keeps dss_lib_answer an
    // unresolved import -- the exact state the static pull removes.
    DiagnosticReporter aloneRep;
    auto imageAlone = linker::link(
        std::span<AssembledModule const>{&*mainMod, 1}, *s.target, *s.exec, aloneRep);
    EXPECT_TRUE(std::any_of(imageAlone.externImportNames.begin(),
                            imageAlone.externImportNames.end(), refsAnswer))
        << "without the static pull, dss_lib_answer stays an unresolved import";
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
        std::vector<std::string>{mainSrc.string()}, "c-subset",
        std::vector<std::string>{"arm64:macho64-arm64-darwin-exec"}, rep);
    ASSERT_EQ(rc, 0) << "Mach-O static-link build must succeed; errs=" << rep.errorCount();
    auto const mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath)) << "the self-contained Mach-O exec must exist";

#if defined(__APPLE__) && defined(__aarch64__)
    // RUN (Apple-Silicon macOS): the pulled dss_lib_answer body is IN the exe ->
    // exit 42. No dylib dependency for the archive's symbols (self-contained).
    auto const r = runBinary(mainPath, std::chrono::milliseconds{5000});
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
    auto pulled = pullStaticArchiveMembers(*mainMod, archives, *s.target, *s.exec, pullRep);
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
        std::vector<std::string>{mainSrc.string()}, "c-subset",
        std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    ASSERT_EQ(rc, 0) << "COFF static-link build must succeed; errs=" << rep.errorCount();
    auto mainPath = dir / "main.exe";
    if (!fs::exists(mainPath)) mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath)) << "the self-contained PE exec must exist";

#if defined(_WIN32)
    auto const r = runBinary(mainPath, std::chrono::milliseconds{5000});
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
    ASSERT_EQ(p.compileFiles(std::vector<std::string>{mainSrc.string()}, "c-subset",
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
// A single 2-function c-subset source -> ONE CU -> ONE archive member exporting
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
        std::vector<std::string>{src.string()}, "c-subset",
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
        std::vector<std::string>{src.string()}, "c-subset",
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

// RED-ON-DISABLE for the D_StaticLibFatArchiveUnsupported guard (0xD013): a
// staticlib-target build handed an INPUT static archive via `--resolve-library`
// fails loud (bundling input archives into a merged "fat" archive is unbuilt)
// rather than silently dropping the input's members. Remove the guard and the
// build would (wrongly) succeed, ignoring the input `.a`.
TEST(StaticLink, StaticLibDriverRejectsInputStaticArchive) {
    ScratchDir scratch{Location::InsideRepo, "static-link"};
    auto const dir = scratch.path();
    Schemas const s = loadSchemas();
    ASSERT_TRUE(s.grammar);

    // A real INPUT `.a` on disk (built by this file's own ar writer helper).
    auto const inputArchive =
        buildArchive(dir, "libinput.a", {{std::string{kLibSrc}, "lib.o"}}, s);
    ASSERT_FALSE(inputArchive.empty());
    ASSERT_TRUE(isArArchiveFile(inputArchive));

    auto const src = writeSrc(dir, "dsslibmath.c",
                              "int dss_add(int a,int b){ return a+b; }\n");

    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries(std::vector<fs::path>{inputArchive});
    DiagnosticReporter rep;
    int const rc = p.compileFiles(
        std::vector<std::string>{src.string()}, "c-subset",
        std::vector<std::string>{"x86_64:elf64-x86_64-linux-staticlib"}, rep);
    EXPECT_NE(rc, 0) << "building a static library WITH an input static archive "
                        "must fail loud (fat-archive bundling is unbuilt)";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_StaticLibFatArchiveUnsupported), 1u)
        << "the guard must emit D_StaticLibFatArchiveUnsupported exactly once";
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
        std::vector<std::string>{src.string()}, "c-subset",
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
