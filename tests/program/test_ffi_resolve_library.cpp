// c162 (D-FF1-READER-CONSUMER) -- the reader-consumer round-trip witness.
//
// This is the trigger that makes the FF5 `ingest()` binary-reader consumer
// LIVE. The three FF1 readers (ELF/PE/Mach-O export + ar) were built +
// hardened + green, producing a format-blind ImportSurface, but `ingest()`
// -- the consumer that calls the readers -- had NO production caller
// (externs resolved only via inline `extern` + shipped JSON descriptors).
//
// THE RESOLVING INSIGHT: DSS builds its OWN library binary, and for a
// user's OWN library there is NO JSON descriptor -- so reading its real
// export table is the ONLY way to link against it. That is a genuine,
// NON-DUPLICATIVE capability AND a self-contained trigger. The flow, proven
// end-to-end PER-PLATFORM NATIVE here:
//   1. DSS compiles lib.c (`int dss_lib_answer(void){ return 42; }`) into a
//      shared library -- `.so` (ELF, c150 writer) / `.dll` (PE, c152).
//   2. DSS compiles main.c (`extern int dss_lib_answer(void); int main(void)
//      { return dss_lib_answer(); }`) and RESOLVES its extern against that
//      library BY READING its export surface: the FF1 reader ->
//      ImportSurface -> the now-live `ingest()` -> FfiMetadata -> the
//      linker extern-bind path (DT_NEEDED / PE import descriptor) -- driven
//      by the `--resolve-library <path>` surface (Program::setResolveLibraries).
//   3. RUN main -> it calls into the library -> exit 42.
//
// HONEST SCOPE (the binary is NOT signature-authority): the read provides
// exactly (a) VALIDATION -- the declared extern EXISTS as an export (an
// absent one fails loud with F_FfiResolveLibrarySymbolAbsent naming the
// symbol + library, a compile-time error instead of a link/load failure);
// and (b) LIBRARY BINDING -- the true library to import from. The TYPE of
// the extern still comes from the inline `extern` declaration. This is
// NON-DUPLICATIVE of the shipped-JSON path precisely because a DSS-built
// library has no shipped descriptor.
//
// This is an INTEGRATION TEST (not an examples/ corpus entry) because the
// examples_runner is single-artifact-per-target and cannot express a
// two-artifact DEPENDENT build (build the library as artifact 1, then build
// main resolving against artifact 1). The runner multi-artifact extension is
// the named follow-up D-EXAMPLES-RUNNER-MULTI-ARTIFACT.
//
// Per-platform NATIVE round-trips (coordinator steer): Linux writes+reads a
// `.so` and RUNS it (ubuntu CI + local WSL); Windows writes+reads a `.dll`
// and RUNS it natively. macOS is deferred to c163 as a `.a` STATIC-archive
// round-trip (needs an ar WRITER + a static-link model DSS does not yet have)
// -- anchored D-FF1-AR-WRITER-STATIC-LINK.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "ffi/binary_reader.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "host_native_target.hpp"
#include "image_dependency_table.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {

// The library defines the answer; main declares it `extern` (TYPE from the
// declaration) and returns its value. The stem `dsslib` names the artifact
// (`dsslib.so` / `dsslib.dll`) -- the loader-resolvable identity the
// `--resolve-library` read records in main's DT_NEEDED / import descriptor.
constexpr std::string_view kLibSrc =
    "int dss_lib_answer(void){ return 42; }\n";
constexpr std::string_view kMainSrc =
    "extern int dss_lib_answer(void);\n"
    "int main(void){ return dss_lib_answer(); }\n";
// A GENUINELY-UNKNOWN symbol -- absent from the library AND from every shipped
// descriptor (not a real libc/system name). This is the typo case that MUST
// fail loud (distinct from a bare system extern, which falls through).
constexpr std::string_view kMainTypoSrc =
    "extern int dss_absent_symbol(void);\n"
    "int main(void){ return dss_absent_symbol(); }\n";

// The mixed program the silent-failure HIGH fold turns green: a BARE
// `extern int puts;` (a real libc/msvcrt symbol the user did NOT #include ->
// no libraryOverride -> binary-governed) ALONGSIDE the own-library extern.
// `puts` is absent from dsslib but IS a known system symbol -> falls through
// to the format-default library; `dss_lib_answer` binds to dsslib. Both
// resolve; prints "hi" + exits 42.
constexpr std::string_view kMixedSrc =
    "extern int puts(const char*);\n"
    "extern int dss_lib_answer(void);\n"
    "int main(void){ puts(\"hi\"); return dss_lib_answer(); }\n";
// D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED: the
// two-library chain. `dssalpha` REFERENCES a symbol `dssbeta` DEFINES, which
// is what puts an SHN_UNDEF row for `dss_beta_answer` into dssalpha's ELF
// `.dynsym` -- the libtcl8.6.so-imports-zlib shape, built out of DSS's own
// artifacts so the pin needs no third-party library on any host.
constexpr std::string_view kTwoLibBetaSrc =
    "int dss_beta_answer(void){ return 20; }\n";
constexpr std::string_view kTwoLibAlphaSrc =
    "extern int dss_beta_answer(void);\n"
    "int dss_alpha_answer(void){ return dss_beta_answer() + 22; }\n";
constexpr std::string_view kTwoLibMainSrc =
    "extern int dss_alpha_answer(void);\n"
    "extern int dss_beta_answer(void);\n"
    "int main(void){ return dss_alpha_answer() + dss_beta_answer() - 20; }\n";
// A TU with NO source-declared externs -- nothing routes to ingest(). Used to
// pin the eager `--resolve-library` path validation (a bad path must still
// fail loud here, not be silently ignored).
constexpr std::string_view kNoExternSrc =
    "int main(void){ return 7; }\n";

fs::path writeSrc(fs::path const& dir, std::string_view name,
                  std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p);
    f << text;
    return p;
}

// Compile one source file to one target via the real production driver.
// `resolveLibs` non-empty threads the `--resolve-library` surface. Returns
// the compileFiles rc (0 == success).
int buildOne(fs::path const& outDir,
             std::vector<fs::path> const& resolveLibs,
             std::string const& srcPath,
             std::string const& target,
             DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    if (!resolveLibs.empty()) p.setResolveLibraries(resolveLibs);
    return p.compileFiles(std::vector<std::string>{srcPath},
                          "c-subset", std::vector<std::string>{target}, rep);
}

// Same as `buildOne`, but each `--resolve-library` entry also STATES the
// runtime identity to record (D-FFI-DECLARED-IMPORT-NAME, the
// `<path>=<import-name>` form). Needed wherever two stand-in libraries must
// stay distinguishable in the emitted dependency table.
int buildOneWithSpecs(fs::path const& outDir,
                      std::vector<ResolveLibrarySpec> const& resolveLibs,
                      std::string const& srcPath,
                      std::string const& target,
                      DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    if (!resolveLibs.empty()) p.setResolveLibraries(resolveLibs);
    return p.compileFiles(std::vector<std::string>{srcPath},
                          "c-subset", std::vector<std::string>{target}, rep);
}

// The emitted image's bytes, for the dependency-table extractors.
[[nodiscard]] std::vector<std::uint8_t> readWholeBinary(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

// The DSS-built library exports the symbol we resolve against -- proven by
// reading its real export surface with the SAME FF1 reader `ingest()` uses.
// This is the "validation source" the round-trip rests on.
[[nodiscard]] bool libraryExportsSymbol(fs::path const& libPath,
                                        std::string_view symbol) {
    DiagnosticReporter rep;
    auto surface = ffi::readImports(libPath, rep);
    if (!surface.has_value()) return false;
    return std::any_of(surface->begin(), surface->end(),
                       [&](ffi::ImportSurface const& row) {
                           return row.mangledName == symbol
                               && row.kind == ffi::SymbolKind::Function;
                       });
}

// Does the library's read surface carry this NAME at all, regardless of the
// kind the reader assigned it? `libraryExportsSymbol` above additionally
// requires SymbolKind::Function, and a DSS-emitted ELF import row is
// STT_NOTYPE -- so the kind check alone would answer "no" for a reason that
// has nothing to do with definedness. This is the predicate that actually
// distinguishes "the reader treats this library's REFERENCE as an EXPORT".
[[nodiscard]] bool libraryExposesName(fs::path const& libPath,
                                      std::string_view symbol) {
    DiagnosticReporter rep;
    auto surface = ffi::readImports(libPath, rep);
    if (!surface.has_value()) return false;
    return std::any_of(surface->begin(), surface->end(),
                       [&](ffi::ImportSurface const& row) {
                           return row.mangledName == symbol;
                       });
}

// The `actual` text of the FIRST diagnostic carrying `code` (empty if none).
// Used to assert the fail-loud message really NAMES the symbol + the offending
// object format, not merely that "some error occurred".
[[nodiscard]] std::string messageForCode(DiagnosticReporter const& r,
                                         DiagnosticCode code) {
    for (auto const& d : r.all()) {
        if (d.code == code) return d.actual;
    }
    return {};
}

// The severity of the FIRST diagnostic carrying `code` (Hint if none — a value
// no assertion below accepts, so an absent diagnostic cannot pass as Error).
[[nodiscard]] DiagnosticSeverity severityForCode(DiagnosticReporter const& r,
                                                 DiagnosticCode code) {
    for (auto const& d : r.all()) {
        if (d.code == code) return d.severity;
    }
    return DiagnosticSeverity::Hint;
}

// One object format's (dynamic-library, exec) target pair. DSS is a
// cross-compiler with no external toolchain, so EVERY leg builds on EVERY host
// — none of the artifacts below is RUN (the availability verdict is a
// COMPILE-time judgment), so no host gating is needed and the coverage stays
// live on all three formats everywhere.
struct FormatLeg {
    std::string_view formatName;   // the `objectFormatKindName` spelling
    std::string_view libTarget;
    std::string_view execTarget;
    std::string_view libArtifact;
};
constexpr FormatLeg kFormatLegs[] = {
    {"elf",   "x86_64:elf64-x86_64-linux-dyn",
     "x86_64:elf64-x86_64-linux-exec",      "dsslib.so"},
    {"pe",    "x86_64:pe64-x86_64-windows-dll",
     "x86_64:pe64-x86_64-windows-exec",     "dsslib.dll"},
    {"macho", "x86_64:macho64-x86_64-darwin-dylib",
     "x86_64:macho64-x86_64-darwin-exec",   "dsslib.dylib"},
};

}  // namespace

// ══ D-FFI-SHIPPED-SYMBOL-ORACLE-IGNORES-OBJECT-FORMATS ════════════════════
//
// The `--resolve-library` oracle used to answer "is X a known system symbol?"
// FORMAT-BLIND: it unioned every `symbols[].name` across every shipped
// descriptor and IGNORED each row's `availableObjectFormats`. So a symbol
// declared for ONE object format was silently vouched for on EVERY format, and
// the pipeline bound it to the TARGET'S FORMAT-DEFAULT library. MEASURED
// end-to-end: `fdatasync` is declared elf-only in unistd.json; a macho build
// referencing it was judged known, bound to libSystem (which has no such
// export), LINKED CLEAN, and the binary died at load with exit 255 and NO
// diagnostic at all. A loud link error had been converted into a silent loader
// death. The three tests below pin the fix from three angles.

// ── (1) The end-to-end fail-loud + its POSITIVE CONTROL ───────────────────
//
// A symbol declared ONLY for other formats must FAIL LOUD at compile time,
// naming the symbol, the active object format, and the format(s) it IS declared
// for — and a structurally IDENTICAL program whose symbols ARE declared for the
// active format must still bind and link. The control is what stops a blanket-
// reject regression (fail on every unmatched governed extern) from passing: it
// exercises BOTH availability encodings — `random` carries a NON-EMPTY set that
// CONTAINS macho, and `puts` carries no gate at all (the empty set = "every
// format"). RED-ON-DISABLE: force `availableHere` true in compile_pipeline step
// 2.5(ii) and the negative arm builds clean (rc 0, zero diagnostics) — exactly
// the silent-loader-death shape the anchor names.
TEST(FfiResolveLibraryRoundTrip, FormatUnavailableShippedSymbolFailsLoud) {
    // The macho leg: `fdatasync` is declared elf-only (unistd.json), so a macho
    // target is where the blind oracle mis-vouched for it.
    FormatLeg const& leg = kFormatLegs[2];
    ASSERT_EQ(leg.formatName, "macho");

    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc = writeSrc(dir, "dsslib.c", kLibSrc);

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(), std::string{leg.libTarget},
                       libRep), 0)
        << "DSS must build the dynamic library for the " << leg.formatName
        << " leg";
    auto const libPath = dir / std::string{leg.libArtifact};
    ASSERT_TRUE(fs::exists(libPath));
    // Precondition: neither probe symbol is an export of the resolve-library,
    // so BOTH reach the shipped-descriptor oracle as unmatched governed externs
    // (the exact class under test).
    ASSERT_FALSE(libraryExportsSymbol(libPath, "fdatasync"));
    ASSERT_FALSE(libraryExportsSymbol(libPath, "random"));

    // ── NEGATIVE: elf-only `fdatasync` on a macho target ──────────────────
    auto const badSrc = writeSrc(
        dir, "fmt_bad.c",
        "extern int fdatasync(int);\n"
        "extern int dss_lib_answer(void);\n"
        "int main(void){ fdatasync(1); return dss_lib_answer(); }\n");
    DiagnosticReporter badRep;
    int const badRc = buildOne(dir, {libPath}, badSrc.string(),
                               std::string{leg.execTarget}, badRep);
    EXPECT_NE(badRc, 0)
        << "a shipped symbol declared only for OTHER object formats must FAIL "
           "the build, not bind to this format's default library";
    EXPECT_EQ(::dss::test_support::countCode(
                  badRep,
                  DiagnosticCode::F_ShippedSymbolUnavailableForTarget),
              1u)
        << "exactly one F_ShippedSymbolUnavailableForTarget — the per-SYMBOL "
           "sibling of F_ShippedHeaderUnavailableForTarget";
    EXPECT_EQ(severityForCode(
                  badRep,
                  DiagnosticCode::F_ShippedSymbolUnavailableForTarget),
              DiagnosticSeverity::Error);
    // The message must be ACTIONABLE: it names the symbol, the target format,
    // and the format(s) the descriptors DO declare it for. Without all three the
    // user cannot tell this apart from "never heard of this symbol".
    std::string const msg = messageForCode(
        badRep, DiagnosticCode::F_ShippedSymbolUnavailableForTarget);
    EXPECT_NE(msg.find("fdatasync"), std::string::npos)
        << "the diagnostic must NAME the symbol; got: [" << msg << "]";
    EXPECT_NE(msg.find("macho"), std::string::npos)
        << "the diagnostic must NAME the active object format; got: [" << msg
        << "]";
    EXPECT_NE(msg.find("elf"), std::string::npos)
        << "the diagnostic must NAME the format(s) the symbol IS declared for; "
           "got: [" << msg << "]";
    // DISTINGUISHABLE from the typo class: a name in NO descriptor routes
    // UNBOUND to the link tier (TF-C66) and surfaces as K_SymbolUndefined, so
    // this verdict must NOT be the link-tier one.
    EXPECT_EQ(::dss::test_support::countCode(
                  badRep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "a format-unavailable KNOWN symbol is a distinct verdict from an "
           "unknown one — it must not masquerade as K_SymbolUndefined";
    EXPECT_FALSE(fs::exists(dir / "fmt_bad"))
        << "no artifact on a failed build";

    // ── POSITIVE CONTROL: same shape, symbols available on macho ──────────
    // `random` is declared macho-only (a NON-EMPTY set that CONTAINS the target
    // — the membership test must accept it); `abs` carries no gate at all (the
    // EMPTY set = every format). Both must still fall through to the format
    // default, and `dss_lib_answer` must still bind to the read library. (No
    // string literal anywhere in the probe: the x86_64-darwin-exec flavor does
    // not advertise a `rodata` section yet [K_NoMatchingObjectFormat], an
    // unrelated linker limitation that would confound this control.)
    auto const okSrc = writeSrc(
        dir, "fmt_ok.c",
        "extern long random(void);\n"
        "extern int abs(int);\n"
        "extern int dss_lib_answer(void);\n"
        "int main(void){ random(); abs(-1); return dss_lib_answer(); }\n");
    DiagnosticReporter okRep;
    int const okRc = buildOne(dir, {libPath}, okSrc.string(),
                              std::string{leg.execTarget}, okRep);
    EXPECT_EQ(::dss::test_support::countCode(
                  okRep,
                  DiagnosticCode::F_ShippedSymbolUnavailableForTarget),
              0u)
        << "a symbol DECLARED for the active format must never trip the "
           "availability gate — a blanket reject is the regression this "
           "control exists to catch";
    EXPECT_EQ(okRc, 0)
        << "the control program must still BUILD (random+puts -> the macho "
           "format default, dss_lib_answer -> the read library)";
    EXPECT_TRUE(fs::exists(dir / "fmt_ok"))
        << "the control program's artifact must be emitted";
}

// ── (2) UNION ACROSS ROWS, end-to-end on all three formats ────────────────
//
// ★ The semantics that is easiest to get wrong. The SAME name is routinely
// declared by SEVERAL rows with DIFFERENT gates: `mtx_lock` (like `call_once`,
// `thrd_create` and ~20 more) carries THREE separate rows in threads.json gated
// ["elf"], ["macho"] and ["pe"]. The correct predicate is "does ANY row declare
// this name available on the target format?" — a per-ROW test would judge
// `mtx_lock` unavailable on two of every three formats and turn the entire C11
// threads surface red everywhere. RED-ON-DISABLE for the union specifically:
// make the oracle keep only the LAST (or first) row's gate instead of unioning
// and two of these three legs fail.
TEST(FfiResolveLibraryRoundTrip, PerFormatRowsUnionKeepsSymbolKnownEverywhere) {
    for (FormatLeg const& leg : kFormatLegs) {
        SCOPED_TRACE(std::string{"object format: "} + std::string{leg.formatName});
        ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
        auto const dir = scratch.path();
        auto const libSrc = writeSrc(dir, "dsslib.c", kLibSrc);

        DiagnosticReporter libRep;
        ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                           std::string{leg.libTarget}, libRep), 0);
        auto const libPath = dir / std::string{leg.libArtifact};
        ASSERT_TRUE(fs::exists(libPath));

        auto const src = writeSrc(
            dir, "union_probe.c",
            "extern int mtx_lock(void*);\n"
            "extern int dss_lib_answer(void);\n"
            "int main(void){ mtx_lock(0); return dss_lib_answer(); }\n");
        DiagnosticReporter rep;
        int const rc = buildOne(dir, {libPath}, src.string(),
                                std::string{leg.execTarget}, rep);
        EXPECT_EQ(::dss::test_support::countCode(
                      rep,
                      DiagnosticCode::F_ShippedSymbolUnavailableForTarget),
                  0u)
            << "mtx_lock is declared by THREE per-format rows; the UNION makes "
               "it known on every format. A per-row predicate fails here.";
        EXPECT_EQ(rc, 0)
            << "the union-declared symbol must still fall through to the "
               "format default and link";
    }
}

// ── (3) The oracle's own contract, measured against the REAL config ───────
//
// The end-to-end tests above prove the pipeline verdict; this pins the
// PREDICATE itself against the shipped descriptors as they actually ship, at
// unit precision and unit speed. Every expectation below is a MEASURED property
// of `src/dss-config/shippedLibs/**` (43 descriptors, 131 per-symbol-gated rows,
// 32 document-level gates), read through the SAME `objectFormatInAvailabilitySet`
// membership predicate the `#include` / `__has_include` / semantic-injection
// gates use — so this test also pins that the oracle cannot drift from them.
TEST(FfiResolveLibraryRoundTrip, ShippedSymbolFormatOracleContract) {
    auto const m = ffi::collectShippedExternSymbolFormats();
    ASSERT_TRUE(m.has_value())
        << "the shippedLibs directory must be discoverable from the test cwd "
           "(DSS_CONFIG_ROOT or an ancestor walk)";

    auto availability = [&](std::string const& name)
        -> std::vector<std::string> const* {
        auto const it = m->find(name);
        return it == m->end() ? nullptr : &it->second;
    };
    auto availableOn = [&](std::string const& name, ObjectFormatKind fmt) {
        auto const* v = availability(name);
        return v != nullptr && ffi::objectFormatInAvailabilitySet(*v, fmt);
    };

    // (a) UNION across three per-format rows (threads.json ["elf"]/["macho"]/
    //     ["pe"]) — known on all three, and the set is NON-EMPTY, proving the
    //     answer came from unioning gated rows rather than from an ungated one.
    ASSERT_NE(availability("mtx_lock"), nullptr);
    EXPECT_FALSE(availability("mtx_lock")->empty())
        << "mtx_lock's rows are ALL gated — an empty (= every format) answer "
           "would mean the gates were dropped, not unioned";
    EXPECT_TRUE(availableOn("mtx_lock", ObjectFormatKind::Elf));
    EXPECT_TRUE(availableOn("mtx_lock", ObjectFormatKind::Pe));
    EXPECT_TRUE(availableOn("mtx_lock", ObjectFormatKind::MachO));

    // (b) UNION across rows in ONE descriptor with UNEQUAL widths: stdio.json
    //     declares sprintf twice — an ["elf","macho"] row and a ["pe"] row.
    EXPECT_TRUE(availableOn("sprintf", ObjectFormatKind::Elf));
    EXPECT_TRUE(availableOn("sprintf", ObjectFormatKind::MachO));
    EXPECT_TRUE(availableOn("sprintf", ObjectFormatKind::Pe));

    // (c) UNION across TWO DESCRIPTORS: fabsf ships in math.json AND
    //     tgmath.json, both gated ["elf","macho"] — so it stays unavailable on
    //     pe. A union that saturated on any second row would wrongly admit pe.
    EXPECT_TRUE(availableOn("fabsf", ObjectFormatKind::Elf));
    EXPECT_TRUE(availableOn("fabsf", ObjectFormatKind::MachO));
    EXPECT_FALSE(availableOn("fabsf", ObjectFormatKind::Pe));

    // (d) THE DEFECT'S OWN SYMBOL: fdatasync is elf-only (unistd.json), so it
    //     must be KNOWN on elf and UNAVAILABLE on macho/pe. This single row is
    //     what the blind oracle vouched for everywhere.
    ASSERT_NE(availability("fdatasync"), nullptr);
    EXPECT_EQ(*availability("fdatasync"), (std::vector<std::string>{"elf"}));
    EXPECT_TRUE(availableOn("fdatasync", ObjectFormatKind::Elf));
    EXPECT_FALSE(availableOn("fdatasync", ObjectFormatKind::MachO));
    EXPECT_FALSE(availableOn("fdatasync", ObjectFormatKind::Pe));

    // (e) An ASYMMETRIC per-symbol gate that excludes a format the descriptor
    //     itself allows: stdlib.json's `atexit` is ["macho"] ALONE — elf routes
    //     through __cxa_atexit and pe through _crt_atexit, each a per-format
    //     macro over a DIFFERENT real export.
    //
    //     ★ D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) NARROWED this from
    //     ["pe","macho"], and the pin is TIGHTENED to match: the three
    //     membership probes are now backed by an EXACT set equality (the (d)/(g)
    //     form), so a future row that re-widens `atexit` to any extra format
    //     reds here even if the three probes below would still pass. That
    //     narrowing is load-bearing, not bookkeeping: ucrtbase.dll exports NO
    //     `atexit` at all (MEASURED, objdump -p), and under
    //     D-FFI-DESCRIPTOR-EAGER-IMPORT a pe-available row for an absent export
    //     breaks EVERY pe binary's LOAD with 0xC0000139. The pe replacement is
    //     the real ucrtbase export `_crt_atexit`, pinned immediately below —
    //     WITHOUT that second block this case would merely record that pe lost a
    //     symbol, and a migration that dropped `atexit` and forgot its
    //     replacement would look identical.
    ASSERT_NE(availability("atexit"), nullptr);
    EXPECT_EQ(*availability("atexit"), (std::vector<std::string>{"macho"}));
    EXPECT_FALSE(availableOn("atexit", ObjectFormatKind::Pe));
    EXPECT_TRUE(availableOn("atexit", ObjectFormatKind::MachO));
    EXPECT_FALSE(availableOn("atexit", ObjectFormatKind::Elf));

    //     The pe arm's replacement export, gated the mirror-image way.
    ASSERT_NE(availability("_crt_atexit"), nullptr);
    EXPECT_EQ(*availability("_crt_atexit"), (std::vector<std::string>{"pe"}));
    EXPECT_TRUE(availableOn("_crt_atexit", ObjectFormatKind::Pe));
    EXPECT_FALSE(availableOn("_crt_atexit", ObjectFormatKind::MachO));
    EXPECT_FALSE(availableOn("_crt_atexit", ObjectFormatKind::Elf));

    // (f) FALLBACK TIER 3 — a row with NO per-symbol gate in a descriptor with
    //     NO document gate is available EVERYWHERE, spelled as the EMPTY set
    //     (the `objectFormatInAvailabilitySet` encoding). stdio.json's `puts`.
    ASSERT_NE(availability("puts"), nullptr);
    EXPECT_TRUE(availability("puts")->empty())
        << "an ungated symbol in an ungated descriptor must carry the EMPTY "
           "(= every format) set, not an enumerated one";
    EXPECT_TRUE(availableOn("puts", ObjectFormatKind::Elf));
    EXPECT_TRUE(availableOn("puts", ObjectFormatKind::Pe));
    EXPECT_TRUE(availableOn("puts", ObjectFormatKind::MachO));

    // (g) FALLBACK TIER 2 — a row with NO per-symbol gate INHERITS its
    //     DOCUMENT's gate. windows.json is document-gated ["pe"] and its
    //     symbols carry no gates of their own.
    ASSERT_NE(availability("CloseHandle"), nullptr);
    EXPECT_EQ(*availability("CloseHandle"), (std::vector<std::string>{"pe"}));
    EXPECT_TRUE(availableOn("CloseHandle", ObjectFormatKind::Pe));
    EXPECT_FALSE(availableOn("CloseHandle", ObjectFormatKind::Elf));
    EXPECT_FALSE(availableOn("CloseHandle", ObjectFormatKind::MachO));

    // (h) A name in NO descriptor is ABSENT from the map entirely — the
    //     "never heard of this symbol" class the consumer routes UNBOUND to the
    //     link tier, structurally distinct from "declared, wrong format".
    EXPECT_EQ(availability("dss_absent_symbol"), nullptr);
}

// ── Validation fail-loud (all hosts) -- red-on-disable (a) ─────────────────
//
// A GENUINELY-UNKNOWN declared extern -- absent from the resolve-library's
// export surface AND from every shipped descriptor (a typo like
// `dss_absent_symbol`, NOT a bare system call) -- fails the build LOUD and
// emits NO artifact. TF-C66 moved the verdict to the LINK tier: the per-CU
// stage routes the unmatched extern UNBOUND (empty library), and the link
// gate (rejectOrDropUnreferencedExterns, c143/c150) rejects the REFERENCED-
// undefined symbol with K_SymbolUndefined on an exec image -- the tier that
// can also see a sibling-TU definition (which the retired compile-time
// verdict could not; it false-positived on every in-build symbol of a
// multi-TU compile -- see SiblingTuDefinitionResolvesUnderResolveLibrary).
// RED-ON-DISABLE: drop the unbound routing in compile_pipeline step 2.5(ii)
// and the typo'd extern mis-binds to the format-default library (a dangling
// import deferred to the loader). Distinct from a bare system extern, which
// the MixedBareSystemExternAndOwnLibraryBothResolve test proves falls through.
//
// Host-native dynamic format so the library builds cleanly on every leg
// (Windows -> PE .dll; else -> ELF .so; the reader + the validation are
// host-agnostic -- no artifact is RUN here, the build fails first).
TEST(FfiResolveLibraryRoundTrip, GenuinelyUnknownExternFailsLoud) {
#if defined(_WIN32)
    std::string const libTarget  = "x86_64:pe64-x86_64-windows-dll";
    std::string const execTarget = "x86_64:pe64-x86_64-windows-exec";
    std::string const libArtifact = "dsslib.dll";
#else
    std::string const libTarget  = "x86_64:elf64-x86_64-linux-dyn";
    std::string const execTarget = "x86_64:elf64-x86_64-linux-exec";
    std::string const libArtifact = "dsslib.so";
#endif
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "main_typo.c", kMainTypoSrc);

    // Build the library.
    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(), libTarget, libRep), 0)
        << "DSS must build the shared library from source";
    auto const libPath = dir / libArtifact;
    ASSERT_TRUE(fs::exists(libPath));
    // The library really exports dss_lib_answer (the read surface) but NOT
    // dss_absent_symbol -- the exact validation condition.
    EXPECT_TRUE(libraryExportsSymbol(libPath, "dss_lib_answer"));
    EXPECT_FALSE(libraryExportsSymbol(libPath, "dss_absent_symbol"));

    // Compile main_typo resolving against the library -> fail loud.
    DiagnosticReporter mainRep;
    int const rc = buildOne(dir, {libPath}, mainSrc.string(),
                            execTarget, mainRep);
    EXPECT_NE(rc, 0)
        << "a declared extern absent from the resolve-library must FAIL "
           "the build (link-tier undefined-symbol policy)";
    EXPECT_GT(::dss::test_support::countCode(
                  mainRep, DiagnosticCode::K_SymbolUndefined),
              0u)
        << "the fail-loud must be K_SymbolUndefined -- the referenced extern "
           "is unbound (not in the resolve-library, not a descriptor name) "
           "and no linked TU defines it (TF-C66 link-tier verdict)";
    // D-FFI-SHIPPED-SYMBOL-ORACLE-IGNORES-OBJECT-FORMATS: the TWO fail-loud
    // classes must stay DISTINGUISHABLE. A name in NO descriptor is the
    // link-tier verdict above; the per-format availability verdict
    // (F_ShippedSymbolUnavailableForTarget) is for a name that IS declared but
    // not for this format, and must NOT fire here.
    EXPECT_EQ(::dss::test_support::countCode(
                  mainRep,
                  DiagnosticCode::F_ShippedSymbolUnavailableForTarget),
              0u)
        << "an UNKNOWN symbol must not be reported as format-unavailable -- "
           "the two verdicts name different user-fixable problems";
    EXPECT_FALSE(fs::exists(dir / "main_typo"))
        << "no ELF artifact on a failed build";
    EXPECT_FALSE(fs::exists(dir / "main_typo.exe"))
        << "no PE artifact on a failed build";
}

// ── TF-C66: sibling-TU definition + --resolve-library (all hosts) ──────────
//
// The regression the TF-C66 reroute fixes: a MULTI-TU build where TU A
// declares `extern int dss_in_build(void);` and TU B DEFINES it, compiled
// WITH `--resolve-library <lib>` where the symbol is (correctly) NOT in the
// library's exports and NOT a shipped-descriptor name. The retired per-CU
// compile-time verdict false-positived here (F_FfiResolveLibrarySymbolAbsent
// on a symbol the build itself defines -- the sqlite 185-TU testfixture hit
// this 2770 times); the link tier resolves the reference against the sibling
// definition and the program RUNS. RED-ON-DISABLE: restore the per-CU
// fail-loud arm and this build errors spuriously.
// D-TEST-HOST-SPAWNS-FOREIGN-BINARY: this test BUILDS then SPAWNS the artifact,
// so the target must match the HOST. RETARGET rather than skip — a GTEST_SKIP
// would quietly stop testing --resolve-library on whichever host it excluded,
// which is exactly the coverage the first macOS run needed. The per-host ladder
// that used to live here (and whose missing arm64 arm reddened the native arm64
// CI leg with `posix_spawn ... rc=8`) now lives in ONE place.
TEST(FfiResolveLibraryRoundTrip, SiblingTuDefinitionResolvesUnderResolveLibrary) {
    auto const host = hostNativeTarget();
    std::string const libTarget   = std::string{host.libTarget};
    std::string const execTarget  = std::string{host.execTarget};
    std::string const libArtifact = hostLibArtifact("dsslib");
    std::string const exeArtifact = hostExeArtifact("decl");
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    // The --resolve-library binary is present + ACTIVE (dsslib exports
    // dss_lib_answer), but the program never touches it -- so the exec carries
    // NO dll/so dependency to resolve at load (isolating the routing under
    // test from a dll-search confound). TU A declares the in-build symbol; TU
    // B defines it (returns 42). dss_in_build is "binary-governed" (no
    // override, not a bare no-lib ref) yet absent from the library and from
    // every descriptor -- exactly the class TF-C66 reroutes to the link tier.
    auto const libSrc = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const declSrc = writeSrc(
        dir, "decl.c",
        "extern int dss_in_build(void);\n"
        "int main(void){ return dss_in_build(); }\n");
    auto const defSrc = writeSrc(dir, "def.c",
                                 "int dss_in_build(void){ return 42; }\n");

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(), libTarget, libRep), 0);
    auto const libPath = dir / libArtifact;
    ASSERT_TRUE(fs::exists(libPath));
    ASSERT_FALSE(libraryExportsSymbol(libPath, "dss_in_build"))
        << "precondition: the library must NOT export the in-build symbol";

    DiagnosticReporter rep;
    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries({libPath});
    // compileUnits = N SEPARATE CUs linked into one exec -- the exact path the
    // CLI `--compile a.c b.c` and the sqlite testfixture use (compileFiles
    // would fold both into ONE multi-file CU, a different link model).
    int const rc = p.compileUnits(
        std::vector<std::string>{declSrc.string(), defSrc.string()},
        "c-subset", std::vector<std::string>{execTarget}, rep);
    ASSERT_EQ(rc, 0)
        << "an extern DEFINED BY A SIBLING TU must not fail resolve-library "
           "validation (the TF-C66 false-positive)";
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::K_SymbolUndefined),
              0u);

    auto const exePath = dir / exeArtifact;
    ASSERT_TRUE(fs::exists(exePath)) << "the multi-TU exec must be emitted";
    auto const r = runBinary(exePath, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "the sibling-TU definition (42) must resolve + run under an active "
           "--resolve-library";
}

// ── TF-C67: a sibling-TU-defined DATA extern under --resolve-library ───────
//
// The DATA (ExternGlobal) twin of SiblingTuDefinitionResolvesUnderResolveLibrary.
// TF-C66's reroute marks a governed-unmatched extern UNBOUND (noLibraryBinding),
// for DATA externs as well as functions -- e.g. sqlite's `extern FuncDefHash
// sqlite3BuiltinFunctions;` defined in a sibling TU. The HIR->MIR ExternGlobal
// lowering (hir_to_mir.cpp) used to REQUIRE a library for every data extern
// (H0009), unlike the ExternFunction arm which exempts noLibraryBinding. That
// asymmetry SKIPPED the unbound data extern's import row, which then broke a
// referencing function body's lowering into an unsealed MIR block -- a
// MirBuilder abort on the full sqlite testfixture (D-MIR-LOWER-BLOCK-CREATED-
// NEVER-FILLED-TESTFIXTURE). TF-C67 gives ExternGlobal the SAME noLibraryBinding
// exemption: an unbound data extern is sibling-resolved at link (row stripped),
// or rejected LOUD if unresolved. RED-ON-DISABLE: revert the ExternGlobal
// exemption and this build fails H0009 on `dss_data_answer`.
// D-TEST-HOST-SPAWNS-FOREIGN-BINARY: builds then SPAWNS, so the target must
// match the HOST — see `tests/test_support/host_native_target.hpp`.
TEST(FfiResolveLibraryRoundTrip, SiblingTuDataDefinitionResolvesUnderResolveLibrary) {
    auto const host = hostNativeTarget();
    std::string const libTarget   = std::string{host.libTarget};
    std::string const execTarget  = std::string{host.execTarget};
    std::string const libArtifact = hostLibArtifact("dsslib");
    std::string const exeArtifact = hostExeArtifact("decl");
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc = writeSrc(dir, "dsslib.c", kLibSrc);
    // TU A references the in-build DATA symbol; TU B defines it (= 42). It is
    // governed (no override), absent from the library and every descriptor ->
    // TF-C66 marks it UNBOUND -> exactly the ExternGlobal path TF-C67 fixes.
    auto const declSrc = writeSrc(
        dir, "decl.c",
        "extern int dss_data_answer;\n"
        "int main(void){ return dss_data_answer; }\n");
    auto const defSrc = writeSrc(dir, "def.c",
                                 "int dss_data_answer = 42;\n");

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(), libTarget, libRep), 0);
    auto const libPath = dir / libArtifact;
    ASSERT_TRUE(fs::exists(libPath));

    DiagnosticReporter rep;
    Program p;
    p.setOutputDir(dir);
    p.setResolveLibraries({libPath});
    int const rc = p.compileUnits(
        std::vector<std::string>{declSrc.string(), defSrc.string()},
        "c-subset", std::vector<std::string>{execTarget}, rep);
    ASSERT_EQ(rc, 0)
        << "an unbound DATA extern DEFINED BY A SIBLING TU must lower + link "
           "(no H0009, no MirBuilder abort) -- the TF-C67 ExternGlobal exemption";
    EXPECT_EQ(::dss::test_support::countCode(
                  rep, DiagnosticCode::K_SymbolUndefined),
              0u);

    auto const exePath = dir / exeArtifact;
    ASSERT_TRUE(fs::exists(exePath)) << "the multi-TU exec must be emitted";
    auto const r = runBinary(exePath, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "the sibling-TU data definition (42) must resolve + run under an "
           "active --resolve-library";
}

// ── Eager --resolve-library path validation (all hosts) -- MEDIUM fold ─────
//
// A nonexistent / unreadable `--resolve-library` path must fail loud
// F_FileOpenFailed EVEN WHEN the TU has NO source-declared externs (so nothing
// routes to the ingest() consumer). Before the fold this was SILENTLY IGNORED
// (the binary was reached only through ingest(), which the partition skipped)
// -- violating the documented contract "opened + read at compile time, fails
// loud on a missing/unreadable file". RED-ON-DISABLE: drop the eager open-probe
// in compile_pipeline step 2.5-pre and this compile succeeds (exit 0), silently
// ignoring the bad path.
TEST(FfiResolveLibraryRoundTrip, MissingResolveLibraryPathFailsLoudEvenWithNoExterns) {
#if defined(_WIN32)
    std::string const execTarget = "x86_64:pe64-x86_64-windows-exec";
#else
    std::string const execTarget = "x86_64:elf64-x86_64-linux-exec";
#endif
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const mainSrc = writeSrc(dir, "noextern.c", kNoExternSrc);
    auto const missing = dir / "does_not_exist_lib.bin";  // never created

    DiagnosticReporter rep;
    int const rc = buildOne(dir, {missing}, mainSrc.string(), execTarget, rep);
    EXPECT_NE(rc, 0)
        << "a missing --resolve-library path must fail the compile even when "
           "the TU has no externs (nothing routes to ingest)";
    EXPECT_GT(::dss::test_support::countCode(
                  rep, DiagnosticCode::F_FileOpenFailed), 0u)
        << "must fire F_FileOpenFailed -- the eager path probe honors the "
           "documented open-at-compile-time contract";
}

// ══ D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED ════════
//
// The END-TO-END pin, and the only tier that can catch this defect: it is a
// COMPOSITION bug. The reader, `ingest()`, and the writer were each doing
// what they were told; the damage appeared only when a library that IMPORTS
// another library's symbol was named on `--resolve-library` before it.
//
// The shape, exactly as the sqlite testfixture hits it (libtcl8.6.so imports
// 14 zlib names, and both libraries are named on the command line):
//
//   dssbeta   defines  dss_beta_answer
//   dssalpha  defines  dss_alpha_answer  AND REFERENCES dss_beta_answer,
//             so dssalpha's ELF `.dynsym` carries an SHN_UNDEF row for it
//   twolib    references BOTH, built with `--resolve-library dssalpha`
//             FIRST and `--resolve-library dssbeta` second
//
// MEASURED before the fix (real driver, real binaries): the ELF reader
// surfaced dssalpha's SHN_UNDEF row as an EXPORT, `ingest()`'s
// first-source-wins bound `dss_beta_answer` to dssalpha, and dssbeta reached
// the linker in no `ExternImport.libraryPath` — so the emitted DT_NEEDED set
// held dssalpha and NOT dssbeta. Reversing the two `--resolve-library`
// arguments produced both: the artifact's declared dependencies were a
// function of COMMAND-LINE ORDER. The PE and Mach-O legs were already
// correct (their readers read `.edata` / the export trie, which cannot
// contain a reference), and they are here because a pin that only asks the
// broken format is a pin that cannot notice the next format to break.
//
// NOT host-gated and nothing is RUN: "did every resolved library get
// recorded?" is a COMPILE-time judgment about the emitted bytes, so all
// three formats stay live on every host — the same reasoning as
// `kFormatLegs`.
//
// The library identity is STATED via `--resolve-library <path>=<name>`
// (D-FFI-DECLARED-IMPORT-NAME) rather than left to the binary's embedded
// identity, because the shipped Mach-O dylib schema declares a single
// `installName` ("@rpath/libdss.dylib") that BOTH stand-ins would inherit —
// two libraries collapsing to one recorded name would make the Mach-O leg
// pass for a reason unrelated to the defect.
TEST(FfiResolveLibraryRoundTrip, EveryResolvedLibraryReachesTheEmittedDependencyTable) {
    struct TwoLibLeg {
        char const* label;
        char const* libTarget;
        char const* execTarget;
        char const* alphaArtifact;
        char const* betaArtifact;
        char const* execArtifact;
        // The ON-BINARY spellings. Mach-O's C decoration prepends an
        // underscore, so the name a reader recovers is format-dependent and
        // must be stated per leg rather than assumed.
        char const* alphaExport;
        char const* betaExport;
        std::vector<std::string> (*deps)(std::vector<std::uint8_t> const&);
    };
    TwoLibLeg const legs[] = {
        {"elf",   "x86_64:elf64-x86_64-linux-dyn",
                  "x86_64:elf64-x86_64-linux-exec",
         "dssalpha.so", "dssbeta.so", "twolib",
         "dss_alpha_answer", "dss_beta_answer",
         &::dss::test_support::elfNeededLibraries},
        {"pe",    "x86_64:pe64-x86_64-windows-dll",
                  "x86_64:pe64-x86_64-windows-exec",
         "dssalpha.dll", "dssbeta.dll", "twolib.exe",
         "dss_alpha_answer", "dss_beta_answer",
         &::dss::test_support::peImportedLibraries},
        {"macho", "x86_64:macho64-x86_64-darwin-dylib",
                  "x86_64:macho64-x86_64-darwin-exec",
         "dssalpha.dylib", "dssbeta.dylib", "twolib",
         "_dss_alpha_answer", "_dss_beta_answer",
         &::dss::test_support::machoLoadedDylibs},
    };

    // Each leg runs through a lambda so a leg that ABORTS on an ASSERT_ costs
    // only its own verdict: an ASSERT_ in a bare loop body returns from the
    // whole TEST and silently stops testing every format after it.
    auto checkLeg = [](TwoLibLeg const& leg) {
        ScratchDir scratch{Location::InsideRepo, "ffi-two-library-deps"};
        auto const dir = scratch.path();
        auto const betaSrc  = writeSrc(dir, "dssbeta.c", kTwoLibBetaSrc);
        auto const alphaSrc = writeSrc(dir, "dssalpha.c", kTwoLibAlphaSrc);
        auto const mainSrc  = writeSrc(dir, "twolib.c", kTwoLibMainSrc);

        // 1. beta: defines the symbol alpha will reference.
        DiagnosticReporter betaRep;
        ASSERT_EQ(buildOne(dir, {}, betaSrc.string(),
                           std::string{leg.libTarget}, betaRep), 0)
            << (betaRep.all().empty() ? "" : betaRep.all().front().actual);
        auto const betaPath = dir / leg.betaArtifact;
        ASSERT_TRUE(fs::exists(betaPath));
        ASSERT_TRUE(libraryExportsSymbol(betaPath, leg.betaExport));

        // 2. alpha: RESOLVES its reference against beta, so alpha's own
        //    symbol table records `dss_beta_answer` as something it NEEDS.
        DiagnosticReporter alphaRep;
        ASSERT_EQ(buildOneWithSpecs(
                      dir, {{betaPath, leg.betaArtifact}}, alphaSrc.string(),
                      std::string{leg.libTarget}, alphaRep), 0)
            << (alphaRep.all().empty() ? "" : alphaRep.all().front().actual);
        auto const alphaPath = dir / leg.alphaArtifact;
        ASSERT_TRUE(fs::exists(alphaPath));
        ASSERT_TRUE(libraryExportsSymbol(alphaPath, leg.alphaExport));
        // The precondition the whole pin rests on, asserted rather than
        // assumed: alpha's read surface must NOT carry beta's symbol at all.
        // MEASURED: on ELF this was TRUE before the fix (the reader surfaced
        // alpha's SHN_UNDEF row for `dss_beta_answer`) and is FALSE after.
        // Name-only on purpose -- see `libraryExposesName`.
        EXPECT_FALSE(libraryExposesName(alphaPath, leg.betaExport))
            << "alpha REFERENCES " << leg.betaExport << "; a reference is not "
               "an export, and reading it as one is the defect this pins";

        // 3. main: references both, with ALPHA NAMED FIRST — the order that
        //    made the second library disappear.
        DiagnosticReporter mainRep;
        ASSERT_EQ(buildOneWithSpecs(dir,
                                    {{alphaPath, leg.alphaArtifact},
                                     {betaPath,  leg.betaArtifact}},
                                    mainSrc.string(),
                                    std::string{leg.execTarget}, mainRep), 0)
            << (mainRep.all().empty() ? "" : mainRep.all().front().actual);
        auto const execPath = dir / leg.execArtifact;
        ASSERT_TRUE(fs::exists(execPath));

        auto const bytes = readWholeBinary(execPath);
        ASSERT_FALSE(bytes.empty());
        auto const recorded = leg.deps(bytes);
        EXPECT_EQ(::dss::test_support::dependencyOccurrences(
                      recorded, leg.alphaArtifact), 1u)
            << "got [" << ::dss::test_support::joinDependencies(recorded) << "]";
        EXPECT_EQ(::dss::test_support::dependencyOccurrences(
                      recorded, leg.betaArtifact), 1u)
            << "the SECOND library is the one that went missing: alpha was "
               "credited with beta's symbol, so beta reached the writer in no "
               "import row at all. got ["
            << ::dss::test_support::joinDependencies(recorded) << "]";
    };
    for (auto const& leg : legs) {
        SCOPED_TRACE(leg.label);
        checkLeg(leg);
    }
}

// ── PE dynamic round-trip (Windows host) -- the native witness + (b) ───────

#if defined(_WIN32)
TEST(FfiResolveLibraryRoundTrip, PeDynamicRoundTripExitsFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    // 1. DSS builds dsslib.dll (PE .dll export directory, c152).
    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       "x86_64:pe64-x86_64-windows-dll", libRep), 0);
    auto const dllPath = dir / "dsslib.dll";
    ASSERT_TRUE(fs::exists(dllPath));
    ASSERT_TRUE(libraryExportsSymbol(dllPath, "dss_lib_answer"))
        << "dsslib.dll must export dss_lib_answer (the read validation "
           "source the whole round-trip rests on)";

    // 2. DSS builds main.exe RESOLVING its extern against dsslib.dll.
    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {dllPath}, mainSrc.string(),
                       "x86_64:pe64-x86_64-windows-exec", mainRep), 0)
        << "main.exe must build, resolving dss_lib_answer against dsslib.dll";
    auto const exePath = dir / "main.exe";
    ASSERT_TRUE(fs::exists(exePath));

    // 3. RUN main.exe natively -> it imports dss_lib_answer from dsslib.dll
    //    (same dir) and returns 42.
    auto const r = runBinary(exePath, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned)
        << "main.exe must spawn -- if not, the PE import descriptor to "
           "dsslib.dll is structurally invalid. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_lib_answer() called "
           "through the dsslib.dll import bound by the reader-consumer.";
}

// Red-on-disable (b): WITHOUT --resolve-library, dss_lib_answer has no
// binding source, falls through to the format-default library (msvcrt.dll),
// and the loader cannot find it there -> the process fails to start / exits
// non-42. The ONLY thing that makes main run is the binding discovered by
// READING dsslib.dll -- so this proves the library binding comes from the
// read, not from anywhere else.
TEST(FfiResolveLibraryRoundTrip, PeWithoutResolveLibraryMisbindsAtRuntime) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       "x86_64:pe64-x86_64-windows-dll", libRep), 0);

    // Build main WITHOUT resolveLibraries -> binds to the msvcrt default.
    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {}, mainSrc.string(),
                       "x86_64:pe64-x86_64-windows-exec", mainRep), 0)
        << "main.exe still BUILDS (dss_lib_answer binds to the format "
           "default) -- the misbinding surfaces only at load/run.";
    auto const exePath = dir / "main.exe";
    ASSERT_TRUE(fs::exists(exePath));

    auto const r = runBinary(exePath, std::chrono::milliseconds{5000});
    // Mis-bound to msvcrt.dll (which does not export dss_lib_answer) -> the
    // loader fails to resolve the import; the process never returns 42.
    EXPECT_TRUE(!r.spawned || r.exitCode != 42u)
        << "WITHOUT the binary read, main must NOT exit 42 -- the binding "
           "that makes it run comes ONLY from reading dsslib.dll. "
           "spawned=" << r.spawned << " exit=" << r.exitCode;
}

// silent-failure HIGH fold (b): the MIXED program -- a bare `extern puts;`
// (a real msvcrt symbol NOT #included -> binary-governed, absent from dsslib,
// but a KNOWN system symbol -> falls through to msvcrt) ALONGSIDE the own-lib
// `dss_lib_answer` (-> dsslib.dll). BOTH resolve; the program prints "hi" and
// exits 42. Before the fold, --resolve-library wrongly failed loud on `puts`,
// rejecting this legitimate program. RED-ON-DISABLE: revert the
// descriptor-fallthrough (treat every unmatched governed extern as a typo) and
// this compile fails on `puts`.
TEST(FfiResolveLibraryRoundTrip, PeMixedBareSystemExternAndOwnLibraryExitFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "mixed.c", kMixedSrc);

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       "x86_64:pe64-x86_64-windows-dll", libRep), 0);
    auto const dllPath = dir / "dsslib.dll";

    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {dllPath}, mainSrc.string(),
                       "x86_64:pe64-x86_64-windows-exec", mainRep), 0)
        << "the mixed bare-puts + own-lib program must BUILD -- puts falls "
           "through to msvcrt (known system symbol), dss_lib_answer binds to "
           "dsslib.dll. A fail-loud here is the HIGH bug.";
    auto const exePath = dir / "mixed.exe";
    ASSERT_TRUE(fs::exists(exePath));

    auto const r = runBinary(exePath, std::chrono::milliseconds{5000},
                             /*captureStdout=*/true);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "exit 42 = puts->msvcrt + dss_lib_answer->dsslib.dll both bound.";
    EXPECT_NE(r.capturedStdout.find("hi"), std::string::npos)
        << "puts must print 'hi' (it resolved to msvcrt); got: ["
        << r.capturedStdout << "]";
}
#endif  // _WIN32

// ── ELF dynamic round-trip (Linux host) -- the native witness ──────────────
//
// Gated on __linux__ only, and BUILT FOR THE HOST's own arch: the artifacts are
// spawned, so a foreign build would fail ENOEXEC.
//
// ⚠ The previous gate here also required an x86_64 host, on the stated grounds
// that "there is no aarch64 `.so` (ET_DYN) format flavor yet". That premise is
// STALE and was MEASURED false at TF-C109: `elf64-aarch64-linux-dyn` shipped in
// c171 (D-LK-ELF-DYN-AARCH64-FLAVOR) and the round-trip works end to end — DSS
// emits the aarch64 ET_DYN with a real-named `.dynsym` export, an aarch64 exec
// resolves its extern against it, and the program runs to exit 42. So the
// arm64 leg was silently forfeiting real coverage it could have had. The gate
// now widens: the native ubuntu-24.04-arm leg runs this for real.
#if defined(__linux__)
TEST(FfiResolveLibraryRoundTrip, ElfDynamicRoundTripExitsFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    // Built for the HOST's own arch — this test spawns its output.
    auto const host = hostNativeTarget();

    // 1. DSS builds dsslib.so (ELF ET_DYN, c150 -- real-named .dynsym export).
    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       std::string{host.libTarget}, libRep), 0);
    auto const soPath = dir / "dsslib.so";
    ASSERT_TRUE(fs::exists(soPath));
    ASSERT_TRUE(libraryExportsSymbol(soPath, "dss_lib_answer"));

    // 2. DSS builds main RESOLVING its extern against dsslib.so.
    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {soPath}, mainSrc.string(),
                       std::string{host.execTarget}, mainRep), 0);
    auto const mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath));

    // 3. RUN main -> ld.so resolves DT_NEEDED dsslib.so (LD_LIBRARY_PATH) +
    //    libc, binds dss_lib_answer, returns 42.
    ::setenv("LD_LIBRARY_PATH", dir.string().c_str(), /*overwrite=*/1);
    auto const r = runBinary(mainPath, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned)
        << "main must spawn under ld.so. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_lib_answer() called "
           "through the dsslib.so binding the reader-consumer discovered.";
}

// silent-failure HIGH fold (b), ELF leg: the mixed bare-puts + own-lib program
// runs under ld.so (puts -> libc.so.6 via descriptor-fallthrough,
// dss_lib_answer -> dsslib.so via the read). Prints "hi" + exits 42.
// Witnessed locally under WSL; runs for real on the ubuntu CI legs.
TEST(FfiResolveLibraryRoundTrip, ElfMixedBareSystemExternAndOwnLibraryExitFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "mixed.c", kMixedSrc);

    // Built for the HOST's own arch — this test spawns its output.
    auto const host = hostNativeTarget();

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       std::string{host.libTarget}, libRep), 0);
    auto const soPath = dir / "dsslib.so";

    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {soPath}, mainSrc.string(),
                       std::string{host.execTarget}, mainRep), 0)
        << "the mixed bare-puts + own-lib program must BUILD -- puts falls "
           "through to libc.so.6, dss_lib_answer binds to dsslib.so.";
    auto const mainPath = dir / "mixed";
    ASSERT_TRUE(fs::exists(mainPath));

    ::setenv("LD_LIBRARY_PATH", dir.string().c_str(), /*overwrite=*/1);
    auto const r = runBinary(mainPath, std::chrono::milliseconds{5000},
                             /*captureStdout=*/true);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "exit 42 = puts->libc + dss_lib_answer->dsslib.so both bound.";
    EXPECT_NE(r.capturedStdout.find("hi"), std::string::npos)
        << "puts must print 'hi'; got: [" << r.capturedStdout << "]";
}
#endif  // __linux__
