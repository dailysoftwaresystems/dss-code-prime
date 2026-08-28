// D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED — THE THIRD
// PICKER: `ffi::realizeShippedExternSymbols`, the HAND-DECLARED path (C23
// 7.1.4p2 — a name declared in the TU with no `#include` at all).
//
// WHY A SECOND FILE, WHEN `test_shipped_realization_consistency.cpp` ALREADY PINS
// THE RULE. That file pins the RULE (`ShippedTypeConsistency`); this one pins that
// the OTHER PICKER ASKS IT. The two are genuinely different failures: the rule can
// be perfect and this entry point still resolve silently by path sort, which is
// exactly the state the tree was in when this file was written.
//
// ✔MEASURED BEFORE (P42 lane K, pe64 exec, `objdump -p`, a `DSS_CONFIG_ROOT` copy
// whose `memory.json` said `msvcrt.dll` where `string.json` said `ucrtbase.dll`,
// three one-line programs, shipped CLI):
//
//   #include <string.h> then <memory.h>  ->  rc=1  F_ShippedCorpusInvariantBroken
//   #include <memory.h> then <string.h>  ->  rc=1  F_ShippedCorpusInvariantBroken
//   hand-declared prototype, no include  ->  rc=0, ZERO diagnostics,
//                                            `memcpy` imported from MSVCRT.DLL
//
// The third arm is the defect in one line: the SAME program, spelled two ways,
// binding two different C runtimes for one name — and the accepted spelling is the
// one nobody diagnosed. The `#include` path resolves by the TU's include closure,
// this path by the corpus index's relPath sort, and before the `reporter`
// parameter this function had nowhere to say the two orders disagreed.
//
// THE FIVE THINGS PINNED HERE, in the order they can rot:
//
//   1. A co-live divergence among the rows THIS CALL CHOOSES AMONG is REFUSED,
//      in BOTH corpus orders (the relPath sort must not be the thing that decides
//      whether we notice).
//   2. ⛔ THE REFUSED NAME IS STILL ANSWERED. Omitting it from the map routes the
//      reference unbound, and the LINK tier then reports an undefined symbol
//      against the USER'S PROGRAM — a true failure filed against an innocent
//      subject. This is the single most important assertion in the file, because
//      it is the "fix" a later cycle is most likely to reach for.
//   3. THE RULE IS AGREEMENT, NOT UNIQUENESS. Agreeing duplicates are accepted
//      (the corpus ships 118 such (name, format) pairs on purpose), and rows
//      GATED APART so they never co-exist are not compared at all (io.json is
//      pe-gated, unistd.json elf/macho-gated, and they share nine names with
//      DIFFERENT images and DIFFERENT `linkName`s — entirely correctly).
//   4. ONLY THE NAMES THE CALLER ASKED ABOUT ARE HELD. This path reads a
//      descriptor only because it happens to declare a requested name and injects
//      nothing else out of it; holding the program to that descriptor's OTHER
//      rows would refuse builds the `#include` path accepts — a NEW
//      spelling-dependent asymmetry, i.e. this very defect, re-introduced by the
//      fix for it.
//   5. THE REAL CORPUS IS SILENT ON THIS PATH, and a REMOVE-direction mutation of
//      it is caught — per format, so a pe-only divergence never reds elf.
//
// ⚠ EVERY CORPUS VARIANT GETS ITS OWN SCRATCH PATH, AND FOR THIS FILE THAT IS
// STILL LOAD-BEARING. `corpusIndex()` memoizes the whole (name -> rows) index per
// RESOLVED ROOT, process-wide, with no staleness check — and every `ask()` below
// goes through it. Mutating one tree in place and re-asking is answered from the
// PRE-mutation index; lane G measured exactly that and lost a debugging round to
// it.
// ⓘ The OTHER half of what lane G hit is fixed rather than remembered: the
// per-file parse cache `readShippedLibDescriptor` reads through
// (`cachedDescriptorJson`) is now CONTENT-VALIDATED and re-parses a file whose
// bytes changed — D-FFI-SHIPPED-DESCRIPTOR-PARSE-CACHE-SERVES-A-STALE-DOCUMENT,
// pinned by `tests/ffi/test_shipped_descriptor_cache_staleness.cpp`. The root memo
// is the one that still requires a distinct path per variant.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "diagnostic_count.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// A repo-SHAPED config root: `DSS_CONFIG_ROOT` names a directory that CONTAINS
// `src/dss-config/`, never the config directory itself. ✔MEASURED the wrong way
// first — pointing it at the config dir makes the override MISS silently and the
// cwd walk answers with the REAL tree, so every arm of a three-arm experiment
// reported the unmutated corpus and agreed with itself.
[[nodiscard]] fs::path shippedLibsDirOf(ScratchDir const& dir) {
    fs::path const d = dir.path() / "src" / "dss-config" / "shippedLibs";
    fs::create_directories(d);
    return d;
}

void writeDesc(fs::path const& shippedLibs, char const* name,
               std::string const& body) {
    std::ofstream out{shippedLibs / name, std::ios::binary};
    out << body;
    ASSERT_TRUE(out.good()) << "descriptor did not reach disk: " << name;
}

// What ONE call to the oracle produced, for a corpus rooted at `treeRoot`.
struct OracleRun {
    std::size_t conflicts = 0;   // F_ShippedCorpusInvariantBroken
    std::size_t answered   = 0;  // entries in the returned map
    std::string firstMessage;    // for the "does it name the CONFIG" assertions
    std::optional<ShippedSymbolRealization> row;   // the FIRST requested name
    bool located = false;        // the oracle found a corpus at all
};

// Ask the oracle about `names`, with `treeRoot` installed as `DSS_CONFIG_ROOT`.
// The interner/registry are per-call: two calls must never share an interner, or
// the SECOND corpus's types would be answered out of the FIRST's identity map.
[[nodiscard]] OracleRun ask(fs::path const& treeRoot,
                            std::vector<std::string> const& names,
                            ObjectFormatKind fmt, DataModel dm,
                            DiagnosticReporter& rep) {
    ScopedEnv const env{"DSS_CONFIG_ROOT", treeRoot.string()};
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    // stdio.json's `vfprintf` spells the ABI alias `va_list`; the real corpus
    // arms below fail to DECODE that descriptor without a binding, and a skipped
    // descriptor is silently absent — the shape that once made `abort` link and
    // `putchar` not. Any consistent stand-in works; nothing here reads the id.
    std::array<NamedTypeBinding, 1> const named{
        NamedTypeBinding{"va_list",
                         interner.pointer(interner.primitive(TypeKind::Void))}};
    auto const realized = realizeShippedExternSymbols(
        names, interner, typeReg, rep, dm,
        std::optional<std::string_view>{"x86_64"}, fmt, named);
    OracleRun out;
    out.conflicts =
        countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken);
    for (auto const& d : rep.all()) {
        if (d.severity == DiagnosticSeverity::Error && out.firstMessage.empty())
            out.firstMessage = d.actual;
    }
    out.located = realized.has_value();
    if (!realized.has_value()) return out;
    out.answered = realized->size();
    if (!names.empty()) {
        auto const it = realized->find(names.front());
        if (it != realized->end()) out.row = it->second;
    }
    return out;
}

// A one-symbol descriptor declaring `dup` and naming ONE pe image. The FILE NAME
// is the caller's, because the corpus index sorts candidates by relPath and that
// sort is what silently decided the answer — so both orders get exercised rather
// than assumed symmetric.
[[nodiscard]] std::string descBody(char const* header, char const* peImage) {
    return std::string{R"({ "header": ")"} + header
         + R"(", "library": { "pe": ")" + peImage
         + R"(" }, "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ] })";
}

} // namespace

// ── 1. THE DEFECT ITSELF: a co-live divergence, in BOTH corpus orders ────────

TEST(ShippedRealizationOracle, HandDeclaredDivergenceIsRefusedInBothCorpusOrders) {
    // `a.json` sorts before `b.json`, so the two arms differ in WHICH image the
    // silent first-wins would have picked. Both must refuse.
    for (bool alphaFirst : {true, false}) {
        SCOPED_TRACE(alphaFirst ? "alpha sorts first" : "beta sorts first");
        ScratchDir dir{Location::Temp, alphaFirst ? "oracle-order-alpha"
                                                  : "oracle-order-beta"};
        fs::path const libs = shippedLibsDirOf(dir);
        writeDesc(libs, "a.json",
                  descBody("a.h", alphaFirst ? "alpha.dll" : "beta.dll"));
        if (::testing::Test::HasFatalFailure()) return;
        writeDesc(libs, "b.json",
                  descBody("b.h", alphaFirst ? "beta.dll" : "alpha.dll"));
        if (::testing::Test::HasFatalFailure()) return;

        DiagnosticReporter rep;
        OracleRun const r = ask(dir.path(), {"dup"}, ObjectFormatKind::Pe,
                                DataModel::Llp64, rep);
        ASSERT_TRUE(r.located) << "the synthetic corpus was not located at all";
        EXPECT_GT(r.conflicts, 0u)
            << "two descriptors named two different pe images for 'dup' and the "
               "HAND-DECLARED path bound one of them silently — the third picker "
               "is not asking the agreement rule";
        // The message must send the author to the CONFIG, naming BOTH files.
        EXPECT_NE(r.firstMessage.find("a.json"), std::string::npos)
            << r.firstMessage;
        EXPECT_NE(r.firstMessage.find("b.json"), std::string::npos)
            << r.firstMessage;
    }
}

// ⛔ THE ASSERTION THAT PROTECTS THE USER'S PROGRAM. A later cycle "fixing" this
// by dropping the ambiguous name would leave this test the only thing standing
// between it and a `K_SymbolUndefined` reported against a source file that is
// perfectly correct.
TEST(ShippedRealizationOracle, ARefusedNameIsStillAnsweredAndNeverRoutedUnbound) {
    ScratchDir dir{Location::Temp, "oracle-still-answered"};
    fs::path const libs = shippedLibsDirOf(dir);
    writeDesc(libs, "a.json", descBody("a.h", "alpha.dll"));
    if (::testing::Test::HasFatalFailure()) return;
    writeDesc(libs, "b.json", descBody("b.h", "beta.dll"));
    if (::testing::Test::HasFatalFailure()) return;

    DiagnosticReporter rep;
    OracleRun const r =
        ask(dir.path(), {"dup"}, ObjectFormatKind::Pe, DataModel::Llp64, rep);
    ASSERT_TRUE(r.located);
    ASSERT_GT(r.conflicts, 0u) << "the divergence was not even detected";
    ASSERT_TRUE(r.row.has_value())
        << "'dup' vanished from the oracle's answer. That routes the reference "
           "UNBOUND, and the link tier then blames the USER'S PROGRAM for an "
           "undefined symbol the CONFIG broke — the diagnostic points at the "
           "wrong file. The name must still be answered; the refusal is the "
           "caller's, off reporter.errorCount()";
    EXPECT_EQ(r.row->status, ShippedRealizationStatus::Realized)
        << "the answer degraded to a non-binding status, which is the same "
           "unbound routing by another spelling";
}

// ── 2. THE RULE IS AGREEMENT, NOT UNIQUENESS ────────────────────────────────

TEST(ShippedRealizationOracle, ByteIdenticalDuplicatesAreAccepted) {
    ScratchDir dir{Location::Temp, "oracle-agreeing"};
    fs::path const libs = shippedLibsDirOf(dir);
    writeDesc(libs, "a.json", descBody("a.h", "alpha.dll"));
    if (::testing::Test::HasFatalFailure()) return;
    writeDesc(libs, "b.json", descBody("b.h", "alpha.dll"));
    if (::testing::Test::HasFatalFailure()) return;

    DiagnosticReporter rep;
    OracleRun const r =
        ask(dir.path(), {"dup"}, ObjectFormatKind::Pe, DataModel::Llp64, rep);
    ASSERT_TRUE(r.located);
    EXPECT_EQ(r.conflicts, 0u)
        << "a legitimate duplicate was refused — the corpus ships 118 co-live "
           "(name, format) pairs on purpose (<memory.h> mirrors <string.h>, "
           "<tgmath.h> mirrors <math.h>) and this would refuse it on day one: "
        << r.firstMessage;
    // NOT VACUOUS: the accept is only worth something if the name really resolved.
    ASSERT_TRUE(r.row.has_value());
    EXPECT_EQ(r.row->status, ShippedRealizationStatus::Realized);
    auto const image = r.row->library.find("pe");
    ASSERT_NE(image, r.row->library.end());
    EXPECT_EQ(image->second, "alpha.dll");
}

// io.json (pe) and unistd.json (elf/macho) share NINE names with different
// images AND different `linkName`s, entirely correctly, because no compile ever
// selects both. This clause is what keeps the check from degenerating into
// "duplicates are forbidden" — ✔MEASURED over the shipped corpus: 12 of the 53
// multi-descriptor names never co-exist on any format.
TEST(ShippedRealizationOracle, RowsGatedApartAreNeverCompared) {
    ScratchDir dir{Location::Temp, "oracle-gated-apart"};
    fs::path const libs = shippedLibsDirOf(dir);
    writeDesc(libs, "peonly.json", R"({
        "header": "peonly.h",
        "availableObjectFormats": ["pe"],
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32",
                       "linkName": "_dup" } ]
    })");
    if (::testing::Test::HasFatalFailure()) return;
    writeDesc(libs, "elfonly.json", R"({
        "header": "elfonly.h",
        "availableObjectFormats": ["elf"],
        "library": { "elf": "libbeta.so" },
        "symbols": [ { "name": "dup", "signature": "fn(i64) -> i64" } ]
    })");
    if (::testing::Test::HasFatalFailure()) return;

    for (auto const [fmt, dm, want] :
         {std::tuple{ObjectFormatKind::Pe, DataModel::Llp64, "alpha.dll"},
          std::tuple{ObjectFormatKind::Elf, DataModel::Lp64, "libbeta.so"}}) {
        SCOPED_TRACE(objectFormatKindName(fmt));
        DiagnosticReporter rep;
        OracleRun const r = ask(dir.path(), {"dup"}, fmt, dm, rep);
        ASSERT_TRUE(r.located);
        EXPECT_EQ(r.conflicts, 0u)
            << "two rows that CANNOT co-exist were compared — the availability "
               "gate is not being applied before the agreement rule: "
            << r.firstMessage;
        // And the surviving row is the one this format actually selects, which is
        // what makes the silence meaningful rather than merely quiet.
        ASSERT_TRUE(r.row.has_value());
        EXPECT_EQ(r.row->status, ShippedRealizationStatus::Realized);
        auto const image = r.row->library.find(std::string{objectFormatKindName(fmt)});
        ASSERT_NE(image, r.row->library.end());
        EXPECT_EQ(image->second, want);
    }
}

// ── 3. SCOPE: only the names the caller ASKED about ─────────────────────────
//
// The fix's own failure mode. Running the FULL `#include`-path check here would
// hold a TU that hand-declares one name to every OTHER row of the descriptors
// that happen to declare it — refusing programs the `#include` path accepts, which
// is a NEW spelling-dependent asymmetry: this defect, re-introduced by its fix.
TEST(ShippedRealizationOracle, OnlyTheRequestedNamesAreHeldToTheRule) {
    ScratchDir dir{Location::Temp, "oracle-scope"};
    fs::path const libs = shippedLibsDirOf(dir);
    // `dup` AGREES in both descriptors; `other` diverges (b.json overrides it).
    writeDesc(libs, "a.json", R"({
        "header": "a.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup",   "signature": "fn(i32) -> i32" },
                     { "name": "other", "signature": "fn(i32) -> i32" } ]
    })");
    if (::testing::Test::HasFatalFailure()) return;
    writeDesc(libs, "b.json", R"({
        "header": "b.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup",   "signature": "fn(i32) -> i32" },
                     { "name": "other", "signature": "fn(i32) -> i32",
                       "library": { "pe": "beta.dll" } } ]
    })");
    if (::testing::Test::HasFatalFailure()) return;

    {
        DiagnosticReporter rep;
        OracleRun const r =
            ask(dir.path(), {"dup"}, ObjectFormatKind::Pe, DataModel::Llp64, rep);
        ASSERT_TRUE(r.located);
        EXPECT_EQ(r.conflicts, 0u)
            << "asking about 'dup' — on which the two descriptors AGREE — was "
               "refused because 'other' diverges. The hand-declared path injects "
               "nothing but the names it was asked for, so this refuses builds "
               "the #include path accepts: " << r.firstMessage;
        EXPECT_TRUE(r.row.has_value());
    }
    // …and the divergent name IS caught when it is the one being asked about, so
    // the silence above is a SCOPE decision and not a dead check.
    {
        DiagnosticReporter rep;
        OracleRun const r = ask(dir.path(), {"other"}, ObjectFormatKind::Pe,
                                DataModel::Llp64, rep);
        ASSERT_TRUE(r.located);
        EXPECT_GT(r.conflicts, 0u)
            << "the divergent name went unreported even when REQUESTED — the "
               "scope filter has swallowed the whole rule";
    }
}

// A caller with no target (direct API / LSP / unit) states no realization at all,
// so it must state no conflict either — inventing one would be the guess this
// oracle exists to delete. The corpus above WOULD conflict on pe.
TEST(ShippedRealizationOracle, NoActiveFormatStatesNothingAndRefusesNothing) {
    ScratchDir dir{Location::Temp, "oracle-no-format"};
    fs::path const libs = shippedLibsDirOf(dir);
    writeDesc(libs, "a.json", descBody("a.h", "alpha.dll"));
    if (::testing::Test::HasFatalFailure()) return;
    writeDesc(libs, "b.json", descBody("b.h", "beta.dll"));
    if (::testing::Test::HasFatalFailure()) return;

    ScopedEnv const env{"DSS_CONFIG_ROOT", dir.path().string()};
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    std::vector<std::string> const names{"dup"};
    auto const realized = realizeShippedExternSymbols(
        names, interner, typeReg, rep, DataModel::Llp64,
        std::optional<std::string_view>{"x86_64"}, std::nullopt);
    ASSERT_TRUE(realized.has_value());
    EXPECT_TRUE(realized->empty()) << "a realization was stated with no format";
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken), 0u)
        << "a conflict was reported about rows no target selects";
}

// ── 4. THE REAL CORPUS — control, then the REMOVE-direction mutant ──────────

namespace {

// Copy the WHOLE repo-shaped config root into scratch and hand back the tree
// root (the directory that CONTAINS `src/dss-config`). Every variant gets its
// own path — see the parse-cache warning in the file header.
[[nodiscard]] fs::path copyConfigTree(ScratchDir const& dir, fs::path const& cfgRoot) {
    fs::path const dst = dir.path() / "src" / "dss-config";
    fs::create_directories(dst.parent_path());
    fs::copy(cfgRoot, dst,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    return dir.path();
}

// The five names <memory.h> and <string.h> BOTH declare — the corpus's own
// co-live duplicate set, and the exact set the measured defect ran through.
[[nodiscard]] std::vector<std::string> memStar() {
    return {"memcpy", "memmove", "memset", "memcmp", "memchr"};
}

} // namespace

// CONTROL. The shipped corpus must be silent on this path, on every format it
// serves — and must actually ANSWER, so the silence is not the silence of a
// corpus that was never read.
TEST(ShippedRealizationOracle, TheShippedCorpusIsSilentOnTheHandDeclaredPath) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    ScratchDir dir{Location::Temp, "oracle-real-clean"};
    fs::path const tree = copyConfigTree(dir, *cfg);

    for (auto const [fmt, dm] :
         {std::tuple{ObjectFormatKind::Pe, DataModel::Llp64},
          std::tuple{ObjectFormatKind::Elf, DataModel::Lp64},
          std::tuple{ObjectFormatKind::MachO, DataModel::Lp64}}) {
        SCOPED_TRACE(objectFormatKindName(fmt));
        DiagnosticReporter rep;
        // mem* (memory.json + string.json) AND `pow` (math.json + tgmath.json):
        // two different co-live duplicate sets, so a check that silently stopped
        // seeing one of them still has the other to be wrong about.
        auto names = memStar();
        names.push_back("pow");
        OracleRun const r = ask(tree, names, fmt, dm, rep);
        ASSERT_TRUE(r.located);
        EXPECT_EQ(r.conflicts, 0u)
            << "the SHIPPED corpus is refused on the hand-declared path: "
            << r.firstMessage;
        EXPECT_EQ(r.answered, names.size())
            << "the corpus answered only " << r.answered << " of "
            << names.size() << " names — this control proves nothing about a "
               "corpus it never read";
    }
}

// THE REMOVE-DIRECTION MUTANT, on the REAL corpus. The way this defect ARRIVES is
// a corpus that LOSES agreement, so the mutation REMOVES a key rather than adding
// an exotic one: `memory.json` stops naming a pe image while `string.json` still
// names one. `realizeRow` treats "available here, no image here" as a USABLE
// answer and stops on it, so the relPath sort alone would decide between importing
// `memcpy` from a real DLL and routing it unbound to the link tier.
//
// ⚠ It mutates a COPY. `src/dss-config` is never touched.
TEST(ShippedRealizationOracle, RemovingOneLibraryKeyIsCaughtOnTheHandDeclaredPath) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    ScratchDir dir{Location::Temp, "oracle-real-mutant"};
    fs::path const tree = copyConfigTree(dir, *cfg);
    fs::path const memory = tree / "src" / "dss-config" / "shippedLibs" / "memory.json";

    nlohmann::json doc;
    {
        std::ifstream in{memory, std::ios::binary};
        ASSERT_TRUE(in.good()) << memory.generic_string();
        doc = nlohmann::json::parse(in, nullptr, false);
    }
    ASSERT_FALSE(doc.is_discarded());
    ASSERT_TRUE(doc.contains("library") && doc.at("library").contains("pe"))
        << "memory.json no longer carries the 'pe' key this mutant removes — "
           "re-point the mutation at a descriptor that still does, do not delete "
           "the test";
    doc.at("library").erase("pe");
    {
        std::ofstream out{memory, std::ios::binary | std::ios::trunc};
        out << doc.dump(2);
        ASSERT_TRUE(out.good()) << "the mutation did not reach disk";
    }

    DiagnosticReporter peRep;
    OracleRun const pe =
        ask(tree, memStar(), ObjectFormatKind::Pe, DataModel::Llp64, peRep);
    ASSERT_TRUE(pe.located);
    EXPECT_GT(pe.conflicts, 0u)
        << "memory.json names no pe image for the five mem* names while "
           "string.json still does, and the HAND-DECLARED path said nothing — "
           "the agreement rule is not wired into realizeShippedExternSymbols";
    EXPECT_NE(pe.firstMessage.find("memory.json"), std::string::npos)
        << pe.firstMessage;
    EXPECT_NE(pe.firstMessage.find("string.json"), std::string::npos)
        << pe.firstMessage;
    // ⛔ and STILL answered, on the real corpus too.
    EXPECT_EQ(pe.answered, memStar().size())
        << "a refused name was dropped from the answer on the real corpus";

    // PER-FORMAT: memory.json's elf key is untouched, so elf must stay silent.
    // Without this the check would be indistinguishable from "duplicates are
    // forbidden", which refuses the shipped configuration.
    DiagnosticReporter elfRep;
    OracleRun const elf =
        ask(tree, memStar(), ObjectFormatKind::Elf, DataModel::Lp64, elfRep);
    ASSERT_TRUE(elf.located);
    EXPECT_EQ(elf.conflicts, 0u)
        << "a pe-only divergence was reported on elf — the per-format "
           "discriminator is broken: " << elf.firstMessage;
}

// ── 6. THE SHIM COMPANION SURFACE (P42 lane N — lane K's named residual) ─────
//
// Lane K closed the picker for the REQUESTED names and said so plainly:
// `realizeShippedDescriptorSurfaceFor` "still takes the first descriptor that
// realizes a name and is not held to the cross-descriptor agreement rule". That
// second picker is reached only for a `synthesize` row, and what it hands back is
// not one name — it is the WHOLE surface the emitted shim body may call, which
// the analyzer then IMPORTS. So the companions' library / linkName / version are
// decided by corpus relPath order, on rows the user never wrote.
//
// ✔MEASURED on the shipped corpus: no choice is made there today (only
// `stdio.json` and `threads.json` carry `synthesize` rows and nothing on either
// surface is declared twice), which is exactly why the pin BUILDS the second
// declaring descriptor instead of waiting for the corpus to grow one — a latent
// order-dependence is still an order-dependence.
//
// ⚠ THE SIGNATURE OF `realizeShippedDescriptorSurfaceFor` IS UNCHANGED. The rule
// runs in `realizeShippedExternSymbols` (step 4), which already carries the
// reporter and already decides the shim claim the surface fetch is a consequence
// of. The two walk the same candidate list in the same order.

namespace {

// TWO rows: a pe `synthesize` recipe (`printf` — a real id from the closed recipe
// vocabulary, since the reader refuses an unknown one at read time) and ONE
// ordinary COMPANION row whose pe image is the caller's. The recipe rows are
// IDENTICAL across both descriptors on purpose: step (2b) must stay silent, so
// anything the run reports comes from the companion surface alone.
[[nodiscard]] std::string shimDescBody(char const* header,
                                       char const* companionImage) {
    return std::string{R"({ "header": ")"} + header
         + R"(", "library": { "pe": "shared.dll" }, "symbols": [ )"
         + R"({ "name": "printf", "signature": "fn(ptr<char>, ...) -> i32", )"
         + R"("availableObjectFormats": ["pe"], "synthesize": "printf" }, )"
         + R"({ "name": "dupcore", "signature": "fn(i32) -> i32", )"
         + R"("availableObjectFormats": ["pe"], "library": { "pe": ")"
         + companionImage + R"(" } } ] })";
}

} // namespace

TEST(ShippedRealizationOracle, ShimCompanionSurfaceDivergenceIsRefused) {
    // Both arms: `printf` resolves to the SAME recipe with the SAME image, so the
    // requested-name rule (2b) has nothing to say. Only `dupcore` — a row the
    // caller never asked about and would import anyway — diverges.
    for (bool alphaFirst : {true, false}) {
        SCOPED_TRACE(alphaFirst ? "alpha sorts first" : "beta sorts first");
        ScratchDir dir{Location::Temp, alphaFirst ? "oracle-shim-alpha"
                                                  : "oracle-shim-beta"};
        fs::path const libs = shippedLibsDirOf(dir);
        writeDesc(libs, "a.json",
                  shimDescBody("a.h", alphaFirst ? "core-alpha.dll" : "core-beta.dll"));
        if (::testing::Test::HasFatalFailure()) return;
        writeDesc(libs, "b.json",
                  shimDescBody("b.h", alphaFirst ? "core-beta.dll" : "core-alpha.dll"));
        if (::testing::Test::HasFatalFailure()) return;

        DiagnosticReporter rep;
        OracleRun const r = ask(dir.path(), {"printf"}, ObjectFormatKind::Pe,
                                DataModel::Llp64, rep);
        ASSERT_TRUE(r.located) << "the synthetic corpus was not located at all";
        ASSERT_TRUE(r.row.has_value()) << "'printf' vanished from the answer";
        ASSERT_EQ(r.row->status, ShippedRealizationStatus::Realized);
        ASSERT_FALSE(r.row->recipeId.empty())
            << "'printf' did not resolve to a SHIM row, so the companion-surface "
               "path this test is about was never reached — re-point the fixture, "
               "do not delete the test";
        EXPECT_GT(r.conflicts, 0u)
            << "two descriptors both realize the `printf` recipe and name two "
               "different pe images for the companion `dupcore` the shim body "
               "would call — whichever descriptor the surface fetch picked would "
               "have decided that import silently, by relPath sort";
        EXPECT_NE(r.firstMessage.find("a.json"), std::string::npos) << r.firstMessage;
        EXPECT_NE(r.firstMessage.find("b.json"), std::string::npos) << r.firstMessage;
        EXPECT_NE(r.firstMessage.find("dupcore"), std::string::npos)
            << "the refusal must name the COMPANION row that diverged, not the "
               "recipe: " << r.firstMessage;
    }
}

// The mirror, and it is what separates this from "two descriptors may not both
// realize a recipe". AGREEING companion surfaces are accepted — `<memory.h>`
// mirroring `<string.h>` is the shipped pattern, and a rule that refused it would
// refuse the corpus the day a second descriptor mirrored a shim.
TEST(ShippedRealizationOracle, AgreeingShimCompanionSurfacesAreAccepted) {
    ScratchDir dir{Location::Temp, "oracle-shim-agree"};
    fs::path const libs = shippedLibsDirOf(dir);
    writeDesc(libs, "a.json", shimDescBody("a.h", "core-alpha.dll"));
    if (::testing::Test::HasFatalFailure()) return;
    writeDesc(libs, "b.json", shimDescBody("b.h", "core-alpha.dll"));
    if (::testing::Test::HasFatalFailure()) return;

    DiagnosticReporter rep;
    OracleRun const r =
        ask(dir.path(), {"printf"}, ObjectFormatKind::Pe, DataModel::Llp64, rep);
    ASSERT_TRUE(r.located);
    EXPECT_EQ(r.conflicts, 0u)
        << "two descriptors mirroring one shim surface byte-identically were "
           "refused — the rule became UNIQUENESS instead of AGREEMENT: "
        << r.firstMessage;
    ASSERT_TRUE(r.row.has_value());
    EXPECT_EQ(r.row->status, ShippedRealizationStatus::Realized);
}

// A NON-shim requested name must not drag its descriptor's other rows into the
// comparison — that is lane K's `addRealizationsOf` scoping, and widening it back
// would refuse builds the `#include` path accepts. Same two descriptors, same
// divergent `dupcore`, but the caller asks about an ordinary row instead of the
// recipe: step (4) must not fire.
TEST(ShippedRealizationOracle, ANonShimRequestDoesNotHoldTheWholeSurface) {
    ScratchDir dir{Location::Temp, "oracle-shim-nonshim"};
    fs::path const libs = shippedLibsDirOf(dir);
    // `agreed` is declared by both and agrees; `dupcore` still diverges.
    auto body = [](char const* header, char const* companionImage) {
        return std::string{R"({ "header": ")"} + header
             + R"(", "library": { "pe": "shared.dll" }, "symbols": [ )"
             + R"({ "name": "agreed", "signature": "fn(i32) -> i32", )"
             + R"("availableObjectFormats": ["pe"] }, )"
             + R"({ "name": "dupcore", "signature": "fn(i32) -> i32", )"
             + R"("availableObjectFormats": ["pe"], "library": { "pe": ")"
             + companionImage + R"(" } } ] })";
    };
    writeDesc(libs, "a.json", body("a.h", "core-alpha.dll"));
    if (::testing::Test::HasFatalFailure()) return;
    writeDesc(libs, "b.json", body("b.h", "core-beta.dll"));
    if (::testing::Test::HasFatalFailure()) return;

    DiagnosticReporter rep;
    OracleRun const r =
        ask(dir.path(), {"agreed"}, ObjectFormatKind::Pe, DataModel::Llp64, rep);
    ASSERT_TRUE(r.located);
    EXPECT_EQ(r.conflicts, 0u)
        << "a divergence on a row the caller never asked about, in a descriptor "
           "read only because it declares 'agreed', was refused — the "
           "hand-declared path is now STRICTER than `#include`, which is this "
           "defect re-introduced by its own fix: " << r.firstMessage;
}
