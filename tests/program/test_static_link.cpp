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
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

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

#if defined(__linux__)
    // RUN: the pulled dss_lib_answer body is IN the exe -> exit 42. No
    // LD_LIBRARY_PATH needed (nothing dynamic to resolve for dss_lib_answer --
    // that is the self-containedness).
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
