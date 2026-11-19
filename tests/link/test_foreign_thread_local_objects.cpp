// P69 fold 3f, lane p69/xa (ledger 716) -- a FOREIGN object that reaches a thread-local through the access model a
// reference compiler writes for position-independent code, linked by DSS beside a DSS `main`.
//
//   The references (MEASURED 2026-11-17, probe-reference-cc; tests/link/data/foreign_thread_local.source.c states
//   how each object was built): the unit, built -fPIC, reaches its own `__thread` counter through
//     * x86_64 ELF (gcc 13.3.0 and clang 18.1.3): the general-dynamic model -- R_X86_64_TLSGD and a call to
//       `__tls_get_addr`; the reference executable (PIE and non-PIE alike) KEEPS the call: it imports
//       `__tls_get_addr@GLIBC_2.3`, which `.gnu.version_r` files under ld-linux-x86-64.so.2 -- the PROGRAM
//       INTERPRETER exports it;
//     * aarch64 ELF (gcc 13.3.0): the TLS descriptor -- the four R_AARCH64_TLSDESC_*; the reference executable
//       names no loader symbol at all (the link relaxed the access to local-exec) and needs libc.so.6 alone;
//     * Mach-O arm64 (Apple clang 21.0.0): the thread-local variable descriptor -- TLVLDP / TLVLDPOFF, and a
//       `__thread_vars` descriptor whose thunk relocation names `__tlv_bootstrap`; the reference executable
//       binds `__tlv_bootstrap` from libSystem.
//   Each reference program -- the unit beside a `main` bumping the counter twice -- runs to 42.
//
//   DSS today: every such object is refused at READ by its reader -- an ELF one as a corrupted binary, since no
//   relocatable or archive-member document declares a thread-local wire type (the disclosed P1 row of the
//   foreign object's thread-local relocations, whose size main ruled to the next cycle), and the Mach-O one
//   because its `__thread_data` section maps to no section row (K_ObjectSectionNotModelled, MEASURED: the
//   definition half, its own row). So no loader name is reached. These
//   cells are that row's tracked witness: each is pinned REFUSED by its reader's own words, and the day DSS
//   reads the object the cell reds and moves to the linked expectation -- the image imports exactly the
//   reference image's loader names (`referenceLoaderNames`, each from the image the reference names) and
//   runs to 42 where the host runs its format.
//
// Every cell runs in-process on every leg: the objects are read and refused, never run.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

constexpr char const* kElfX64   = "x86_64:elf64-x86_64-linux-exec";
constexpr char const* kElfArm64 = "arm64:elf64-aarch64-linux-exec";
constexpr char const* kMachO    = "arm64:macho64-arm64-darwin-exec";
constexpr char const* kMain =
    "int dss_tls_bump(void);\nint main(void) {\n    dss_tls_bump();\n    return dss_tls_bump();\n}\n";

constexpr char const* kOwedTo =
    "the foreign object's thread-local relocations (the disclosed P1 row): no member document declares a "
    "thread-local wire type";

// One foreign object, the image it is linked into, the code and words its reader refuses it with today, and the
// loader names the REFERENCE's image of the same program imports (MEASURED; the linked expectation's, not read
// today).
struct Cell {
    char const*              fixture;
    char const*              exec;
    DiagnosticCode           refusedAs;
    char const*              refusedSaying;
    std::vector<std::string> referenceLoaderNames;
};

[[nodiscard]] std::vector<Cell> const& cells() {
    static std::vector<Cell> const all{
        {"foreign_thread_local_x86_64_elf_gcc_fpic_O2.o", kElfX64, DiagnosticCode::F_CorruptedBinary,
         "relocation type 19 in '.rela.text' is not declared by ELF format", {"__tls_get_addr"}},
        {"foreign_thread_local_x86_64_elf_clang_fpic_O2.o", kElfX64, DiagnosticCode::F_CorruptedBinary,
         "relocation type 19 in '.rela.text' is not declared by ELF format", {"__tls_get_addr"}},
        {"foreign_thread_local_arm64_elf_gcc_fpic_O2.o", kElfArm64, DiagnosticCode::F_CorruptedBinary,
         "relocation type 562 in '.rela.text' is not declared by ELF format", {}},
        {"foreign_thread_local_arm64_macho_clang_O2.o", kMachO, DiagnosticCode::K_ObjectSectionNotModelled,
         "lives in section '__DATA,__thread_data' which resolves to no known code/data section kind",
         {"__tlv_bootstrap"}},
    };
    return all;
}

[[nodiscard]] fs::path fixturePath(std::string_view name) {
    return dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

}  // namespace

// Every leg: each object, linked by DSS beside a DSS `main`, is refused at READ by its reader, by its code and in
// its own words -- and nothing else is refused. Every cell is judged, whatever an earlier one did.
TEST(ForeignThreadLocalObjects, EachIsRefusedAtReadUntilItsRowCloses) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "foreign-thread-local"};
    std::size_t k = 0;
    for (auto const& cell : cells()) {
        SCOPED_TRACE(std::string{cell.fixture} + " -> " + cell.exec);
        fs::path const dir = scratch.path() / std::to_string(k++);
        fs::create_directories(dir / "out");
        std::ofstream(dir / "main.c", std::ios::binary) << kMain;
        DiagnosticReporter rep;
        Program p;
        p.setOutputDir(dir / "out");
        int const rc = p.compileFiles(
            std::vector<std::string>{(dir / "main.c").string(), fixturePath(cell.fixture).string()}, "c",
            std::vector<std::string>{cell.exec}, rep);
        EXPECT_NE(rc, 0) << "owed to " << kOwedTo << ": DSS links this object now, so the cell moves to the "
                         << "linked expectation (the image imports the reference's loader names and runs to 42)";
        std::size_t refusals = 0, saying = 0;
        for (auto const& d : rep.all()) {
            if (d.severity != DiagnosticSeverity::Error) continue;
            ++refusals;
            if (d.code == cell.refusedAs && d.actual.find(cell.refusedSaying) != std::string::npos) {
                ++saying;
            }
        }
        EXPECT_EQ(saying, 1u) << "the reader refuses the object once, " << diagnosticCodeName(cell.refusedAs)
                              << ", saying: " << cell.refusedSaying
                              << diagnosticsOf(rep);
        EXPECT_EQ(refusals, 1u) << "and nothing else is refused:" << diagnosticsOf(rep);
    }
}
