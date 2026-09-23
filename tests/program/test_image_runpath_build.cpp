// D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH — the runpath request through the REAL
// driver: the CLI flag, the project-manifest key, their merge, what each writer
// records (decoded from the bytes, never grepped), the warning's arithmetic on
// the archive route, the refusal's two facts, and the dependency-artifact cache
// key term. The vocabulary, the declarations and the loader's rules are pinned
// in tests/link/test_image_runpath.cpp; the RUN is witnessed by the corpus pair
// `examples/c/dynlib_runpath_origin` (exits 42 with no loader variable) and
// `examples/c/dynlib_runpath_absent_exits_127` (its control).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/project_config.hpp"
#include "diagnostic_count.hpp"
#include "program/cli_args.hpp"
#include "program/dependency_resolver.hpp"
#include "program/program.hpp"
#include "program/runtime_object_cache.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using namespace dss::test_support;

namespace {

// ── argv for the parser ──────────────────────────────────────────────────────
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*>       ptrs;
    explicit Argv(std::initializer_list<std::string> args) {
        storage.assign(args.begin(), args.end());
        ptrs.reserve(storage.size() + 1);
        for (auto& s : storage) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
    }
    [[nodiscard]] int argc() const noexcept { return static_cast<int>(storage.size()); }
    [[nodiscard]] char** argv() noexcept { return ptrs.data(); }
};

void writeText(fs::path const& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

[[nodiscard]] std::vector<std::uint8_t> readBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::uint16_t u16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return o + 2 > b.size() ? 0 : static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
[[nodiscard]] std::uint32_t u32(std::vector<std::uint8_t> const& b, std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16)
         | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
[[nodiscard]] std::uint64_t u64(std::vector<std::uint8_t> const& b, std::size_t o) {
    return o + 8 > b.size() ? 0
                            : (static_cast<std::uint64_t>(u32(b, o + 4)) << 32) | u32(b, o);
}
[[nodiscard]] std::string cstr(std::vector<std::uint8_t> const& b, std::size_t o) {
    std::string s;
    while (o < b.size() && b[o] != 0) s.push_back(static_cast<char>(b[o++]));
    return s;
}

// ── ELF: the `.dynamic` array, decoded through the section headers ──────────
struct DynEntry {
    std::uint64_t tag = 0;
    std::uint64_t val = 0;
    std::string   str;   // the `.dynstr` string for NEEDED / RUNPATH / RPATH
};
[[nodiscard]] std::vector<DynEntry> elfDynamic(std::vector<std::uint8_t> const& b) {
    std::vector<DynEntry> out;
    if (b.size() < 64) return out;
    std::uint64_t const shoff = u64(b, 0x28);
    std::uint16_t const shentsize = u16(b, 0x3A);
    std::uint16_t const shnum = u16(b, 0x3C);
    std::uint16_t const shstrndx = u16(b, 0x3E);
    if (shoff == 0 || shentsize == 0) return out;
    auto const hdr = [&](std::size_t i) { return static_cast<std::size_t>(shoff) + i * shentsize; };
    std::uint64_t const shstr = u64(b, hdr(shstrndx) + 0x18);
    std::uint64_t dynOff = 0, dynSize = 0, strOff = 0;
    for (std::size_t i = 0; i < shnum; ++i) {
        auto const name = cstr(b, static_cast<std::size_t>(shstr) + u32(b, hdr(i)));
        if (name == ".dynamic") { dynOff = u64(b, hdr(i) + 0x18); dynSize = u64(b, hdr(i) + 0x20); }
        if (name == ".dynstr") strOff = u64(b, hdr(i) + 0x18);
    }
    for (std::uint64_t e = 0; dynOff != 0 && e + 16 <= dynSize; e += 16) {
        DynEntry d;
        d.tag = u64(b, static_cast<std::size_t>(dynOff + e));
        d.val = u64(b, static_cast<std::size_t>(dynOff + e + 8));
        if (d.tag == 0) break;
        if (d.tag == 1 || d.tag == 15 || d.tag == 29)
            d.str = cstr(b, static_cast<std::size_t>(strOff + d.val));
        out.push_back(std::move(d));
    }
    return out;
}
constexpr std::uint64_t kDtNeeded = 1, kDtRpath = 15, kDtRunpath = 29;

[[nodiscard]] std::vector<std::string> tagged(std::vector<DynEntry> const& dyn,
                                              std::uint64_t tag) {
    std::vector<std::string> out;
    for (auto const& d : dyn) if (d.tag == tag) out.push_back(d.str);
    return out;
}

// ── Mach-O: every LC_RPATH, with the three wire facts that make it one ──────
struct Rpath {
    std::uint32_t cmdsize = 0;
    std::uint32_t pathOffset = 0;
    std::string   path;
};
constexpr std::uint32_t kLcRpath = 0x8000001Cu, kLcLoadDylib = 0x0Cu;
[[nodiscard]] std::vector<Rpath> machoRpaths(std::vector<std::uint8_t> const& b) {
    std::vector<Rpath> out;
    if (b.size() < 32) return out;
    std::uint32_t const ncmds = u32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds && off + 8 <= b.size(); ++i) {
        std::uint32_t const cmd = u32(b, off), size = u32(b, off + 4);
        if (size == 0) break;
        if (cmd == kLcRpath) {
            Rpath r;
            r.cmdsize = size;
            r.pathOffset = u32(b, off + 8);
            r.path = cstr(b, off + r.pathOffset);
            out.push_back(std::move(r));
        }
        off += size;
    }
    return out;
}
[[nodiscard]] std::vector<std::string> machoDylibs(std::vector<std::uint8_t> const& b) {
    std::vector<std::string> out;
    std::uint32_t const ncmds = b.size() < 32 ? 0 : u32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds && off + 8 <= b.size(); ++i) {
        std::uint32_t const cmd = u32(b, off), size = u32(b, off + 4);
        if (size == 0) break;
        if (cmd == kLcLoadDylib) out.push_back(cstr(b, off + u32(b, off + 8)));
        off += size;
    }
    return out;
}

constexpr std::string_view kLib =
    "int dss_rp_answer(void) { return 42; }\n";
constexpr std::string_view kMain =
    "extern int dss_rp_answer(void);\nint main(void) { return dss_rp_answer(); }\n";
constexpr std::string_view kPlain = "int main(void) { return 42; }\n";

// Build ONE source for ONE target, with optional runpaths and resolve-libs.
int build(fs::path const& outDir, fs::path const& src, std::string const& target,
          std::vector<std::string> const& runpaths,
          std::vector<fs::path> const& libs, DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    if (!runpaths.empty()) p.setRunpaths(runpaths);
    if (!libs.empty()) p.setResolveLibraries(libs);
    return p.compileFiles(std::vector<std::string>{src.string()}, "c",
                          std::vector<std::string>{target}, rep);
}

[[nodiscard]] std::string dump(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all())
        s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// THE CLI FLAG
// ════════════════════════════════════════════════════════════════════════════

TEST(RunpathCli, RepeatableBothSpellingsKeptVerbatimAndInOrder) {
    Argv a{"dsscp", "--compile", "a.c", "--language", "c",
           "--target", "x86_64:elf64-x86_64-linux-exec",
           "--rpath", "$ORIGIN", "--rpath=${ORIGIN}/../lib",
           "--rpath", "@loader_path", "--rpath=/opt/a:/opt/b"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    EXPECT_EQ(r->runpaths, (std::vector<std::string>{"$ORIGIN", "${ORIGIN}/../lib",
                                                     "@loader_path", "/opt/a:/opt/b"}))
        << "gcc's literal semantics: every string kept as written, in order";
}

TEST(RunpathCli, AnEmptyValueAndAModeThatEmitsNoImageAreRefused) {
    Argv empty{"dsscp", "--compile", "a.c", "--language", "c",
               "--target", "x86_64:elf64-x86_64-linux-exec", "--rpath="};
    auto e = parseCliArgs(empty.argc(), empty.argv());
    ASSERT_FALSE(e.has_value());
    EXPECT_EQ(e.error().kind, CliArgsError::MissingFlagValue);

    Argv transpile{"dsscp", "--transpile", "a.c", "--language", "c",
                   "--target", "x86_64:elf64-x86_64-linux", "--rpath", "/opt/a"};
    auto t = parseCliArgs(transpile.argc(), transpile.argv());
    ASSERT_FALSE(t.has_value());
    EXPECT_EQ(t.error().kind, CliArgsError::NoModeSelected);
    EXPECT_NE(t.error().detail.find("--rpath"), std::string::npos) << t.error().detail;
    EXPECT_NE(t.error().detail.find("silently discarded"), std::string::npos);

    EXPECT_NE(cliHelpText().find("--rpath <dir>"), std::string::npos)
        << "a flag the help does not name is one nobody finds";
}

// `--rpath` with NO mode flag at all. The mode gate leaves Mode::None to the
// generic no-mode guard, so the flag must be in THAT guard's list — without it
// `dsscp --rpath /opt/a` parsed clean and the driver printed its "ready" line,
// discarding the request in silence (found by review, pinned here).
TEST(RunpathCli, ABareRpathWithNoModeIsRefused) {
    Argv a{"dsscp", "--rpath", "/opt/a"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value()) << "a runpath request with nothing to record it must not parse";
    EXPECT_EQ(r.error().kind, CliArgsError::NoModeSelected) << r.error().detail;
}

// The parse pin above proves argv reaches `CliArgs`; it cannot prove that
// `Program::run` then HANDS the entries to the build. This drives the shipped
// entry point and decodes the image — with a CONTROL run without the flag, so
// an implementation that always records `$ORIGIN` cannot pass.
TEST(RunpathCli, TheFlagReachesTheImageThroughProgramRun) {
    ScratchDir scratch{Location::Temp, "runpath-cli-run"};
    auto const dir = scratch.path();
    writeText(dir / "dsslib.c", kLib);
    writeText(dir / "main.c", kMain);
    DiagnosticReporter libRep;
    {
        Program p;
        p.setOutputDir(dir);
        ASSERT_EQ(p.compileFiles(std::vector<std::string>{(dir / "dsslib.c").string()}, "c",
                                 std::vector<std::string>{"x86_64:elf64-x86_64-linux-dyn"},
                                 libRep), 0) << dump(libRep);
    }
    auto const runOnce = [&](fs::path const& out, bool withFlag) {
        std::vector<std::string> args{
            "dsscp", "--compile", (dir / "main.c").string(), "--language", "c",
            "--target", "x86_64:elf64-x86_64-linux-exec",
            "--resolve-library", (dir / "dsslib.so").string(),
            "--output", out.string()};
        if (withFlag) {
            args.push_back("--rpath");
            args.push_back("${ORIGIN}");
        }
        std::vector<char*> argv;
        for (auto& s : args) argv.push_back(s.data());
        argv.push_back(nullptr);
        Program p;
        return p.run(static_cast<int>(args.size()), argv.data());
    };
    ASSERT_EQ(runOnce(dir / "with", true), 0);
    ASSERT_EQ(runOnce(dir / "without", false), 0);
    EXPECT_EQ(tagged(elfDynamic(readBytes(dir / "with" / "main")), kDtRunpath),
              std::vector<std::string>{"$ORIGIN"})
        << "`--rpath '${ORIGIN}'` on the command line must reach the image";
    EXPECT_TRUE(tagged(elfDynamic(readBytes(dir / "without" / "main")), kDtRunpath).empty())
        << "CONTROL: without the flag nothing is recorded";
}

// ════════════════════════════════════════════════════════════════════════════
// THE PROJECT-MANIFEST KEY
// ════════════════════════════════════════════════════════════════════════════

namespace {
[[nodiscard]] std::string manifestWith(std::string_view runpathsJson) {
    return std::string{R"({"language": "c", "artifactProfile": "cli",
        "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
        "runpaths": )"} + std::string{runpathsJson} + "}";
}
} // namespace

TEST(RunpathManifest, PortableEntriesAreKeptInOrder) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(manifestWith(R"(["${ORIGIN}", "/opt/x", "${ORIGIN}/../lib"])"),
                                 "p.json", rep);
    ASSERT_TRUE(pc.has_value()) << dump(rep);
    EXPECT_EQ(pc->runpaths,
              (std::vector<std::string>{"${ORIGIN}", "/opt/x", "${ORIGIN}/../lib"}));

    DiagnosticReporter rep2;
    auto none = parseProjectConfig(R"({"language": "c", "artifactProfile": "cli",
        "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]})",
                                   "p.json", rep2);
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->runpaths.empty()) << "absent means nothing is recorded";
}

TEST(RunpathManifest, ANonPortableEntryIsRefusedAndTheRefusalPointsAtTheCli) {
    for (std::string_view bad : {R"(["$ORIGIN"])", R"(["/opt/ok", "lib"])",
                                 R"(["@loader_path"])", R"(["/a:/b"])"}) {
        DiagnosticReporter rep;
        auto pc = parseProjectConfig(manifestWith(bad), "p.json", rep);
        EXPECT_FALSE(pc.has_value()) << bad;
        ASSERT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u) << bad << dump(rep);
        auto const& msg = rep.all().front().actual;
        EXPECT_NE(msg.find("field 'runpaths' entry ["), std::string::npos) << msg;
        EXPECT_NE(msg.find("--rpath"), std::string::npos)
            << "the refusal must name where a loader-specific spelling IS accepted: "
            << msg;
    }
    for (std::string_view shape : {R"("${ORIGIN}")", R"([7])", R"([""])"}) {
        DiagnosticReporter rep;
        EXPECT_FALSE(parseProjectConfig(manifestWith(shape), "p.json", rep).has_value())
            << shape;
        EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u) << shape << dump(rep);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// WHAT THE WRITERS RECORD — through the real driver, decoded from the bytes
// ════════════════════════════════════════════════════════════════════════════

TEST(RunpathBuild, ElfExecPieAndSharedObjectRecordOneRunpathEntry) {
    ScratchDir scratch{Location::Temp, "runpath-elf"};
    auto const dir = scratch.path();
    writeText(dir / "dsslib.c", kLib);
    writeText(dir / "main.c", kMain);

    for (std::string_view arch : {"x86_64", "aarch64"}) {
        SCOPED_TRACE(std::string{arch});
        std::string const tgt = arch == "x86_64" ? "x86_64:elf64-x86_64-linux"
                                                 : "arm64:elf64-aarch64-linux";
        auto const out = dir / std::string{arch};
        // The shared object records its OWN runpath (its `$ORIGIN` is ITS dir).
        DiagnosticReporter libRep;
        ASSERT_EQ(build(out, dir / "dsslib.c", tgt + "-dyn", {"${ORIGIN}/deps"}, {}, libRep), 0)
            << dump(libRep);
        auto const lib = readBytes(out / "dsslib.so");
        auto const libDyn = elfDynamic(lib);
        EXPECT_EQ(tagged(libDyn, kDtRunpath), std::vector<std::string>{"$ORIGIN/deps"});
        EXPECT_TRUE(tagged(libDyn, kDtRpath).empty()) << "DT_RUNPATH, never both";

        for (std::string_view flavor : {"-exec", "-pie"}) {
            SCOPED_TRACE(std::string{flavor});
            auto const exeDir = out / std::string{flavor.substr(1)};
            DiagnosticReporter rep;
            ASSERT_EQ(build(exeDir, dir / "main.c", tgt + std::string{flavor},
                            {"${ORIGIN}", "/opt/a", "${ORIGIN}"}, {out / "dsslib.so"}, rep),
                      0) << dump(rep);
            EXPECT_EQ(rep.all().size(), 0u) << dump(rep);
            auto const dyn = elfDynamic(readBytes(exeDir / "main"));
            // ONE entry, joined by the declared ':', duplicates dropped — the
            // GNU ld shape (measured: `/opt/a:$ORIGIN:$ORIGIN/../lib`).
            EXPECT_EQ(tagged(dyn, kDtRunpath), std::vector<std::string>{"$ORIGIN:/opt/a"});
            // Placed right after the DT_NEEDED run, as GNU ld places it.
            std::size_t lastNeeded = 0, runpathAt = 0;
            for (std::size_t i = 0; i < dyn.size(); ++i) {
                if (dyn[i].tag == kDtNeeded) lastNeeded = i;
                if (dyn[i].tag == kDtRunpath) runpathAt = i;
            }
            EXPECT_EQ(runpathAt, lastNeeded + 1);
            auto const needed = tagged(dyn, kDtNeeded);
            EXPECT_EQ(std::count(needed.begin(), needed.end(), std::string{"dsslib.so"}), 1)
                << "the library is still recorded by bare name";

            // The CONTROL: the same build without the request records nothing.
            auto const plainDir = exeDir / "plain";
            DiagnosticReporter plainRep;
            ASSERT_EQ(build(plainDir, dir / "main.c", tgt + std::string{flavor}, {},
                            {out / "dsslib.so"}, plainRep), 0) << dump(plainRep);
            auto const plain = elfDynamic(readBytes(plainDir / "main"));
            EXPECT_TRUE(tagged(plain, kDtRunpath).empty());
            EXPECT_TRUE(tagged(plain, kDtRpath).empty());
        }
    }
}

TEST(RunpathBuild, MachOExecAndDylibRecordOneLcRpathPerPath) {
    ScratchDir scratch{Location::Temp, "runpath-macho"};
    auto const dir = scratch.path();
    writeText(dir / "dsslib.c", kLib);
    writeText(dir / "main.c", kMain);

    for (std::string_view tgt : {"arm64:macho64-arm64-darwin", "x86_64:macho64-x86_64-darwin"}) {
        SCOPED_TRACE(std::string{tgt});
        auto const out = dir / (tgt.starts_with("arm64") ? "arm64" : "x86_64");
        DiagnosticReporter libRep;
        ASSERT_EQ(build(out, dir / "dsslib.c", std::string{tgt} + "-dylib",
                        {"${ORIGIN}/deps"}, {}, libRep), 0) << dump(libRep);
        auto const lib = machoRpaths(readBytes(out / "dsslib.dylib"));
        ASSERT_EQ(lib.size(), 1u);
        EXPECT_EQ(lib[0].path, "@loader_path/deps");

        DiagnosticReporter rep;
        ASSERT_EQ(build(out / "exe", dir / "main.c", std::string{tgt} + "-exec",
                        {"${ORIGIN}", "/opt/a", "${ORIGIN}"}, {out / "dsslib.dylib"}, rep),
                  0) << dump(rep);
        EXPECT_EQ(rep.all().size(), 0u) << dump(rep);
        auto const bytes = readBytes(out / "exe" / "main");
        auto const rp = machoRpaths(bytes);
        // One command PER path, request order, the duplicate dropped (a current
        // dyld refuses a duplicate LC_RPATH — read at source).
        ASSERT_EQ(rp.size(), 2u);
        EXPECT_EQ(rp[0].path, "@loader_path");
        EXPECT_EQ(rp[1].path, "/opt/a");
        for (auto const& r : rp) {
            EXPECT_EQ(r.pathOffset, 12u) << "lc_str follows cmd/cmdsize/offset";
            EXPECT_EQ(r.cmdsize % 8, 0u) << "load commands are 8-aligned";
            EXPECT_EQ(r.cmdsize, ((12 + r.path.size() + 1 + 7) / 8) * 8);
        }
        EXPECT_EQ(rp[0].cmdsize, 32u) << "measured against ld64.lld: @loader_path -> 32";
        // …and the library it resolves is the @rpath/ one dyld will search for.
        auto const dylibs = machoDylibs(bytes);
        EXPECT_NE(std::find(dylibs.begin(), dylibs.end(), "@rpath/dsslib.dylib"),
                  dylibs.end());

        DiagnosticReporter plainRep;
        ASSERT_EQ(build(out / "plain", dir / "main.c", std::string{tgt} + "-exec", {},
                        {out / "dsslib.dylib"}, plainRep), 0) << dump(plainRep);
        EXPECT_TRUE(machoRpaths(readBytes(out / "plain" / "main")).empty())
            << "without the request there is no LC_RPATH — the row's premise";
    }
}

TEST(RunpathBuild, APeImageIsBuiltByteIdenticalAndTheRequestIsReportedOnce) {
    ScratchDir scratch{Location::Temp, "runpath-pe"};
    auto const dir = scratch.path();
    writeText(dir / "main.c", kPlain);
    for (std::string_view tgt : {"x86_64:pe64-x86_64-windows-exec",
                                 "x86_64:pe64-x86_64-windows-dll"}) {
        SCOPED_TRACE(std::string{tgt});
        std::string const file = tgt.ends_with("exec") ? "main.exe" : "main.dll";
        DiagnosticReporter withRep, plainRep;
        ASSERT_EQ(build(dir / "with", dir / "main.c", std::string{tgt},
                        {"${ORIGIN}", "/opt/a"}, {}, withRep), 0) << dump(withRep);
        ASSERT_EQ(build(dir / "plain", dir / "main.c", std::string{tgt}, {}, {}, plainRep), 0);
        EXPECT_EQ(countCode(withRep, DiagnosticCode::K_FormatLacksRunpath), 1u) << dump(withRep);
        EXPECT_EQ(withRep.errorCount(), 0u) << "ACCEPTED, as both PE references accept it";
        EXPECT_EQ(countCode(plainRep, DiagnosticCode::K_FormatLacksRunpath), 0u);
        EXPECT_EQ(readBytes(dir / "with" / file), readBytes(dir / "plain" / file))
            << "nothing is recorded — the image is the one built without the request, "
               "exactly as mingw GNU ld's and MSVC link's were (measured)";
    }
}

TEST(RunpathBuild, AnArchiveOfManyMembersReportsTheRequestOnce) {
    ScratchDir scratch{Location::Temp, "runpath-archive"};
    auto const dir = scratch.path();
    std::vector<std::string> srcs;
    for (int i = 0; i < 3; ++i) {
        auto const p = dir / ("m" + std::to_string(i) + ".c");
        writeText(p, "int m" + std::to_string(i) + "(void) { return " + std::to_string(i) + "; }\n");
        srcs.push_back(p.string());
    }
    Program p;
    p.setOutputDir(dir / "out");
    p.setRunpaths({"${ORIGIN}"});
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileUnits(srcs, "c",
                             std::vector<std::string>{"x86_64:elf64-x86_64-linux-staticlib"},
                             rep), 0) << dump(rep);
    // ★ THE COUNT IS THE PIN: every member is linked separately, and the gate
    // reports once per link — so without the archive route's single report
    // and its runpath-free member requests this reads 3.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksRunpath), 1u) << dump(rep);
    ASSERT_FALSE(rep.all().empty());
    EXPECT_NE(rep.all().front().actual.find("linkAndWriteStaticArchive"), std::string::npos)
        << rep.all().front().actual;
}

TEST(RunpathBuild, ARelocatableObjectReportsTheRequestOnce) {
    ScratchDir scratch{Location::Temp, "runpath-object"};
    auto const dir = scratch.path();
    writeText(dir / "m.c", "int m(void) { return 1; }\n");
    DiagnosticReporter rep;
    ASSERT_EQ(build(dir / "out", dir / "m.c", "x86_64:elf64-x86_64-linux",
                    {"/opt/a"}, {}, rep), 0) << dump(rep);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksRunpath), 1u) << dump(rep);
}

TEST(RunpathBuild, AnUnrecordableEntryFailsTheLinkAndIsTheOnlyExplanation) {
    // The two facts prong (2) of the unsuppressable table rests on, measured:
    // the link FAILS with no artifact, and this diagnostic is the ONLY one.
    ScratchDir scratch{Location::Temp, "runpath-refused"};
    auto const dir = scratch.path();
    writeText(dir / "main.c", kPlain);
    DiagnosticReporter rep;
    EXPECT_NE(build(dir / "out", dir / "main.c", "x86_64:elf64-x86_64-linux-exec",
                    {"/opt/a", ""}, {}, rep), 0);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_InvalidRunpathRequest), 1u) << dump(rep);
    EXPECT_EQ(rep.errorCount(), 1u) << "nothing else explains the failure" << dump(rep);
    EXPECT_FALSE(fs::exists(dir / "out" / "main")) << "no image on a refused link";
}

TEST(RunpathBuild, TheManifestAndTheCliAccumulateManifestFirst) {
    ScratchDir scratch{Location::Temp, "runpath-merge"};
    auto const dir = scratch.path();
    writeText(dir / "main.c", kPlain);
    auto const proj = dir / "app.dss-project.json";
    writeText(proj, R"({"language": "c", "artifactProfile": "cli",
        "targets": ["x86_64:elf64-x86_64-linux-exec"],
        "sources": [")" + (dir / "main.c").generic_string() + R"("],
        "runpaths": ["/opt/manifest", "${ORIGIN}"]})");
    Program p;
    p.setOutputDir(dir / "out");
    p.setRunpaths({"/opt/cli", "/opt/manifest"});   // what `Program::run` stamps from --rpath
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileProject(proj.string(), rep), 0) << dump(rep);
    auto const dyn = elfDynamic(readBytes(dir / "out" / "elf64-x86_64-linux-exec" / "main"));
    EXPECT_EQ(tagged(dyn, kDtRunpath),
              std::vector<std::string>{"/opt/manifest:$ORIGIN:/opt/cli"})
        << "manifest entries first, then the CLI's; the CLI's repeat of a "
           "manifest entry is the duplicate the writer drops";
}

TEST(RunpathBuild, ADependencyRecordsItsOwnRunpathsNeverTheConsumers) {
    ScratchDir scratch{Location::Temp, "runpath-dep"};
    auto const dir = scratch.path();
    auto const dep = dir / "dsslib";
    writeText(dep / "dsslib.c", kLib);
    writeText(dep / std::string{kDependencyManifestName},
              R"({"language": "c", "artifactProfile": "lib",
                  "targets": ["x86_64:elf64-x86_64-linux-dyn"],
                  "sources": ["dsslib.c"], "runpaths": ["/opt/dep"]})");
    writeText(dir / "main.c", kMain);
    auto const proj = dir / "app.dss-project.json";
    writeText(proj, R"({"language": "c", "artifactProfile": "cli",
        "targets": ["x86_64:elf64-x86_64-linux-exec"],
        "sources": [")" + (dir / "main.c").generic_string() + R"("],
        "runpaths": ["/opt/root"],
        "dependsOn": [{"path": ")" + dep.generic_string() + R"("}]})");
    Program p;
    p.setOutputDir(dir / "out");
    p.setRunpaths({"/opt/cli"});
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileProject(proj.string(), rep), 0) << dump(rep);

    std::optional<fs::path> so;
    for (auto const& e : fs::recursive_directory_iterator{dir / "out" / std::string{kDependencyOutputDirName}})
        if (e.path().extension() == ".so") so = e.path();
    ASSERT_TRUE(so.has_value()) << "the dependency's shared object was not found";
    EXPECT_EQ(tagged(elfDynamic(readBytes(*so)), kDtRunpath),
              std::vector<std::string>{"/opt/dep"})
        << "a runpath is a property of the image that carries it";
    auto const root = elfDynamic(readBytes(dir / "out" / "elf64-x86_64-linux-exec" / "main"));
    EXPECT_EQ(tagged(root, kDtRunpath), std::vector<std::string>{"/opt/root:/opt/cli"});
}

// ════════════════════════════════════════════════════════════════════════════
// THE DEPENDENCY-ARTIFACT CACHE KEY
// ════════════════════════════════════════════════════════════════════════════

namespace {
[[nodiscard]] runtime::DependencyArtifactRequest keyRequest(std::vector<std::string> runpaths) {
    runtime::DependencyArtifactRequest r;
    r.configRoot = fs::path{"config-root"};
    r.overrideVariable = "DSS_TEST_CACHE";
    r.inputClosureDigest = std::string(64, 'a');
    r.artifactStem = "dsslib";
    r.artifactSuffix = ".so";
    r.targetSpec = "x86_64:elf64-x86_64-linux-dyn";
    r.buildFormatName = "elf64-x86_64-linux-dyn";
    r.siblingFormatName = "elf64-x86_64-linux-staticlib";
    r.configName = "debug";
    r.ltoModeName = "full";
    r.runpaths = std::move(runpaths);
    return r;
}
[[nodiscard]] std::string digestOf(std::vector<std::string> runpaths) {
    auto const k = runtime::computeDependencyArtifactKey(keyRequest(std::move(runpaths)));
    EXPECT_TRUE(k.has_value()) << (k.has_value() ? "" : k.error());
    return k.has_value() ? k->digest : std::string{};
}
} // namespace

TEST(RunpathCacheKey, TwoBuildsDifferingOnlyInRunpathsNeverShareAnEntry) {
    auto const none = digestOf({});
    auto const one = digestOf({"${ORIGIN}"});
    ASSERT_FALSE(none.empty());
    EXPECT_NE(none, one) << "the runpath reaches the bytes, so it must reach the key";
    EXPECT_EQ(one, digestOf({"${ORIGIN}"})) << "and the key is a function of it";
    EXPECT_NE(digestOf({"/a", "/b"}), digestOf({"/b", "/a"}))
        << "order is meaning: the loader searches in it";
    // Length-prefixed, so an entry holding a newline cannot forge a second one.
    EXPECT_NE(digestOf({"a\nrunpath=b", "c"}), digestOf({"a", "b\nrunpath=c"}));
    auto const doc = runtime::computeDependencyArtifactKey(keyRequest({"${ORIGIN}"}));
    ASSERT_TRUE(doc.has_value());
    EXPECT_NE(doc->document.find("runpath-count=1\nrunpath=9:${ORIGIN}\n"),
              std::string::npos) << doc->document;
}
