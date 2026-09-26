// D-FFI-DESCRIPTOR-KNOWN-NAME-HAS-NO-LIBRARY-FOR-FORMAT — the outcome "the
// corpus KNOWS this name, the name IS available on the active object format,
// and the row names NO IMAGE for that format".
//
// ★ WHAT THIS FILE IS, AND WHY IT SYNTHESIZES ITS OWN WITNESS. ✔MEASURED over
// all 567 symbol rows of `src/dss-config/shippedLibs/**`: NOT ONE row reaches
// this outcome on a BACKED object format (elf/pe/macho) — every available row
// names an image, or a `realization` shipped source, or a `synthesize` recipe.
// So there is no in-tree descriptor to point at, and a test that only read the
// corpus would measure nothing at all. The witness below is CONSTRUCTED: a
// descriptor that declares a symbol AVAILABLE on a format while providing no
// `library` entry for it. That is a LATENT arm being closed, not a witnessed
// in-tree defect, and the distinction is the point — an unenumerated arm in a
// binding path is where a silent wrong-bind appears the FIRST TIME a descriptor
// is edited, and this one is reachable by a ONE-TOKEN edit.
//
// ── THE VERDICT, AND WHY IT IS (b) AND NOT (a) ───────────────────────────────
//
// The anchor offered two closings: (a) an ERROR at descriptor LOAD — a
// descriptor declaring an available symbol it cannot bind is malformed — or
// (b) UNBOUND TO THE LINK TIER, like any unrealized name. (b) is what these
// tests pin, and (a) is REFUSED for three MEASURED reasons plus a conformance
// one:
//
//   1. ✔MEASURED: 125 rows across ctype/math/memory/stdio/stdlib/string.json
//      declare NO `availableObjectFormats` at all, which this codebase's own
//      contract reads as AVAILABLE ON EVERY FORMAT — while naming images only
//      for elf/pe/macho. `wasm` and `spirv` are selectable spellings of the
//      SAME closed vocabulary. A load-time "available ⇒ must name an image"
//      rule therefore fails loud on 125 rows of the six most central C
//      descriptors the day it ships, and the only repairs are to invent images
//      that do not exist or to make the corpus lie about where C is available.
//   2. Scoping (a) to EXPLICIT availability lists to dodge (1) is incoherent:
//      it would be LOUD on the NARROWER claim (`["elf","macho","pe"]`) and
//      SILENT on the BROADER one (the empty list), so deleting a token to make
//      a descriptor MORE available would switch the check off.
//   3. The tier refuses it. `realizeShippedExternSymbols` reads descriptors the
//      user never `#include`d — its own contract states that a descriptor which
//      fails to read is SKIPPED, because "an unrelated descriptor's
//      malformedness must not become this program's build failure". A load
//      error on this arm is either swallowed by exactly that skip, or turns one
//      unrelated descriptor into every program's build failure.
//   4. C23 5.1.1.2 phase 8. "The platform knows the name but not which image
//      owns it" is a statement about the PLATFORM's image inventory, not about
//      the user's program: a sibling TU or an operator `-l` may legitimately
//      provide the symbol. Phase 8 puts that verdict at the LINK tier.
//
// ── WHAT (a)'S POSTURE *IS* RIGHT ABOUT, AND WHERE IT LANDS ──────────────────
//
// A `library` key that is PRESENT and names the EMPTY STRING is a different
// animal: it is not a statement that the format has no image, it is a SECOND
// SPELLING of one — and ✔MEASURED, the tiers read the two spellings
// differently. `realizeRow` asked `library.contains(format)`, so `{"pe":""}`
// answered `Realized` carrying no image at all, while the binder folds
// (`buildCuMir`, the asm binder, the lazy archive pull) all test the VALUE and
// route unbound. One row, two verdicts, decided by which tier asked. The
// descriptor-level `realization` map — the DELIBERATE SIBLING of `library`,
// written to the same chokepoint discipline — already refuses its empty value
// ("must be a NON-EMPTY path"); `library` did not. That asymmetry is closed
// here, at the chokepoint the anchor names, and it is what makes arm (b)
// STATABLE: with one spelling of absence, every tier states the same arm.
//
// ⚠ EVERY CORPUS VARIANT GETS ITS OWN SCRATCH PATH. `corpusIndex()` memoizes
// the whole (name → rows) index per RESOLVED ROOT, process-wide, with no
// staleness check, and every oracle call below goes through it. Reusing one
// tree across two variants answers the second from the FIRST's index — the
// `test_shipped_realization_oracle.cpp` lesson, kept rather than relearned.
//
// ⚠ THE ORACLE ARMS MUST RUN UNDER ctest, never as a bare `.exe`: they drive
// the corpus through `DSS_CONFIG_ROOT`, and `DSS_CONFIG_ROOT` names a directory
// that CONTAINS `src/dss-config/`, not the config directory itself. Pointed at
// the config dir the override MISSES SILENTLY and the cwd walk answers with the
// REAL shipped tree — every arm then agrees with itself about the wrong corpus.
//
// ── SECTIONS 4 AND 5: THE SAME ROW REACHED THROUGH `#include` ────────────────
//
// Verdict (b) above is the HAND-WRITTEN declaration's road. A row a header
// injects is a different road, and on it the build always stopped — in HIR, with
// a node id. It now stops where the realization is decided, naming the
// descriptor, the symbol and the format
// ([[D-DIAG-NOLIBRARYFORFORMAT-REPORTS-AN-HIR-NODE-FOR-A-CONFIG-CONDITION]]).

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "diagnostic_count.hpp"
#include "core/types/unsuppressable_codes.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "program/program.hpp"   // section 5: the driver builds the staged tree
#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"
#include "shipped_read_pairs.hpp"   // the real pairs a REAL descriptor is read on

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// The name the synthetic descriptors declare. Deliberately NOT a real C library
// symbol: an arm that accidentally read the shipped corpus instead of the
// fixture would answer `Unknown` for this name rather than quietly succeeding
// against the real `string.json`.
constexpr char const* kProbe = "dss_probe_no_library";

// A repo-SHAPED config root: `DSS_CONFIG_ROOT` names the directory that CONTAINS
// `src/dss-config/`, never the config directory itself.
[[nodiscard]] fs::path shippedLibsDirOf(ScratchDir const& dir) {
    fs::path const d = dir.path() / "src" / "dss-config" / "shippedLibs";
    fs::create_directories(d);
    return d;
}

void writeDesc(fs::path const& where, std::string const& body) {
    std::ofstream out{where, std::ios::binary};
    out << body;
    ASSERT_TRUE(out.good()) << "descriptor did not reach disk: "
                            << where.generic_string();
}

// One descriptor declaring `name`, available on `avail`, with `libraryJson`
// spliced in verbatim as the descriptor-level `library` map (or omitted when
// empty). `extra` appends raw JSON to the symbol row (a `synthesize` recipe, a
// per-symbol `library` override) so every variant differs in exactly one token.
//
// ⚠ `name` is a parameter, not a constant, because the `synthesize` axis is
// NAME-KEYED: the reader refuses a recipe id that does not EQUAL the symbol's own
// name (the synth pass identifies the recipe by name). ✔MEASURED the hard way —
// a made-up recipe id failed the READ, the oracle then SKIPPED the descriptor by
// its stated contract, and the arm read as "not answered" rather than as the
// realization it was written to check.
[[nodiscard]] std::string descBody(char const* name, char const* avail,
                                   char const* libraryJson,
                                   char const* extra = "") {
    std::string body = R"({ "header": "probe.h")";
    body += std::string{R"(, "availableObjectFormats": )"} + avail;
    if (libraryJson != nullptr && *libraryJson != '\0')
        body += std::string{R"(, "library": )"} + libraryJson;
    body += std::string{R"(, "symbols": [ { "name": ")"} + name
          + R"(", "signature": "fn(i32) -> i32")" + extra + " } ] }";
    return body;
}

// Read ONE descriptor file through the production reader, interner-fresh.
struct ReadRun {
    bool                     read = false;   // the reader returned a descriptor
    std::size_t              malformed = 0;  // F_ShippedLibDescriptorMalformed
    std::string              firstMessage;
    std::optional<ShippedLibDescriptor> desc;
};

[[nodiscard]] ReadRun readOne(fs::path const& path) {
    TypeInterner       interner{CompilationUnitId{1}};
    TypeRegistry       typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Lp64, std::nullopt,
                                         std::nullopt, {});
    ReadRun out;
    out.read      = desc.has_value();
    out.malformed = countCode(rep, DiagnosticCode::F_ShippedLibDescriptorMalformed);
    for (auto const& d : rep.all())
        if (d.severity == DiagnosticSeverity::Error && out.firstMessage.empty())
            out.firstMessage = d.actual;
    out.desc = std::move(desc);
    return out;
}

// What ONE oracle call produced for a corpus rooted at `treeRoot`.
struct OracleRun {
    bool                                    located = false;
    bool                                    answered = false;
    std::size_t                             errors = 0;
    std::optional<ShippedSymbolRealization> row;
};

[[nodiscard]] OracleRun ask(fs::path const& treeRoot, ObjectFormatKind fmt,
                            char const* name = kProbe) {
    ScopedEnv const    env{"DSS_CONFIG_ROOT", treeRoot.string()};
    TypeInterner       interner{CompilationUnitId{1}};
    TypeRegistry       typeReg;
    DiagnosticReporter rep;
    std::vector<std::string> const names{name};
    auto const realized = realizeShippedExternSymbols(
        names, interner, typeReg, rep, DataModel::Lp64,
        std::optional<std::string_view>{"x86_64"}, fmt, {});
    OracleRun out;
    out.errors  = rep.errorCount();
    out.located = realized.has_value();
    if (!realized.has_value()) return out;
    if (auto const it = realized->find(name); it != realized->end()) {
        out.answered = true;
        out.row      = it->second;
    }
    return out;
}

} // namespace

// ── 1. THE ARM ITSELF: known + available + NO IMAGE ⇒ STATED, and UNBOUND ────

// The witness. `probe.h` says it exists on pe; the `library` map names only an
// elf image. Under verdict (b) the platform's answer is a STATED status that
// carries NO image, the name is still ANSWERED (never dropped from the map),
// and NOTHING is reported — the reference routes unbound and the LINK tier
// judges it, per C23 5.1.1.2 phase 8.
TEST(ShippedNoLibraryForFormat, KnownAndAvailableWithNoImageIsStatedAndUnbound) {
    ScratchDir dir{Location::Temp, "ffi-nolib-arm"};
    writeDesc(shippedLibsDirOf(dir) / "probe.json",
              descBody(kProbe, R"(["pe"])", R"({ "elf": "libc.so.6" })"));

    OracleRun const run = ask(dir.path(), ObjectFormatKind::Pe);
    ASSERT_TRUE(run.located) << "the fixture corpus was not located — "
                                "DSS_CONFIG_ROOT must name the dir CONTAINING "
                                "src/dss-config";
    ASSERT_TRUE(run.answered)
        << "⛔ the name must still be ANSWERED. Dropping it routes the reference "
           "unbound with NO statement, and the arm is a fallthrough again";
    ASSERT_TRUE(run.row.has_value());
    EXPECT_EQ(run.row->status, ShippedRealizationStatus::NoLibraryForFormat)
        << "declared + available + no image for this format is the STATED arm";
    EXPECT_TRUE(shippedLibraryImageForFormat(run.row->library, "pe").empty())
        << "the arm must carry NO image — an image here is the silent wrong bind";
    EXPECT_EQ(run.errors, 0u)
        << "verdict (b): this is a statement about the PLATFORM's image "
           "inventory, not about the user's program — it is NOT a compile error";
}

// THE CONTROL ON THE OTHER SIDE. The same descriptor, the same format, the same
// availability — one added `library` key. If this did not flip to `Realized`,
// the arm above would be measuring the fixture rather than the missing image.
TEST(ShippedNoLibraryForFormat, AnImageForTheActiveFormatRealizes) {
    ScratchDir dir{Location::Temp, "ffi-nolib-control"};
    writeDesc(shippedLibsDirOf(dir) / "probe.json",
              descBody(kProbe, R"(["pe"])",
                       R"({ "elf": "libc.so.6", "pe": "ucrtbase.dll" })"));

    OracleRun const run = ask(dir.path(), ObjectFormatKind::Pe);
    ASSERT_TRUE(run.located);
    ASSERT_TRUE(run.answered);
    ASSERT_TRUE(run.row.has_value());
    EXPECT_EQ(run.row->status, ShippedRealizationStatus::Realized);
    EXPECT_EQ(shippedLibraryImageForFormat(run.row->library, "pe"), "ucrtbase.dll");
    EXPECT_EQ(run.errors, 0u);
}

// THE ADJACENT ARM, kept distinct. A row that is NOT available here is
// `UnavailableForFormat`, not `NoLibraryForFormat` — the two are different
// platform statements and collapsing them would make the refutation recorded in
// the anchor true again (the twelve format-gated POSIX descriptors with no `pe`
// key are UNAVAILABLE on pe, which is why their absent key is CORRECT).
TEST(ShippedNoLibraryForFormat, NotAvailableHereIsTheOtherArmNotThisOne) {
    ScratchDir dir{Location::Temp, "ffi-nolib-unavail"};
    writeDesc(shippedLibsDirOf(dir) / "probe.json",
              descBody(kProbe, R"(["elf", "macho"])", R"({ "elf": "libc.so.6" })"));

    OracleRun const run = ask(dir.path(), ObjectFormatKind::Pe);
    ASSERT_TRUE(run.located);
    ASSERT_TRUE(run.answered);
    ASSERT_TRUE(run.row.has_value());
    EXPECT_EQ(run.row->status, ShippedRealizationStatus::UnavailableForFormat);
    EXPECT_EQ(run.errors, 0u);
}

// A `synthesize` row needs NO image at all — the body is compiler-emitted — so
// the absent key must NOT drag it into the arm. Pinned because the arm's test
// and this exemption live in ONE expression: a change to either can silently
// swallow the other. The row is named `mtx_lock` because the recipe id is
// NAME-KEYED and the reader refuses any other spelling.
TEST(ShippedNoLibraryForFormat, ASynthesizedRowNeedsNoImageAndStillRealizes) {
    ScratchDir dir{Location::Temp, "ffi-nolib-synth"};
    writeDesc(shippedLibsDirOf(dir) / "probe.json",
              descBody("mtx_lock", R"(["pe"])", R"({ "elf": "libc.so.6" })",
                       R"(, "synthesize": "mtx_lock")"));

    OracleRun const run = ask(dir.path(), ObjectFormatKind::Pe, "mtx_lock");
    ASSERT_TRUE(run.located);
    ASSERT_TRUE(run.answered);
    ASSERT_TRUE(run.row.has_value());
    EXPECT_EQ(run.row->status, ShippedRealizationStatus::Realized)
        << "a compiler-emitted body needs no image, so the absent `pe` key must "
           "NOT pull this row into the no-library arm";
    EXPECT_EQ(run.row->recipeId, "mtx_lock");
    EXPECT_TRUE(shippedLibraryImageForFormat(run.row->library, "pe").empty())
        << "and it is REALIZED while still naming no image — the two facts are "
           "independent, which is exactly why one expression decides both";
}

// ── 2. THE SECOND SPELLING OF ABSENCE IS REFUSED AT LOAD ─────────────────────

// `{"pe": ""}` is not "no image on pe" — that is what the ABSENT key says. It
// is a key that names nothing, and it split the tiers: `realizeRow` asked
// `contains()` and answered `Realized` with no image, while every binder fold
// tests the VALUE and routes unbound. The descriptor is malformed and says so at
// LOAD, which is the one place a wrong answer cannot yet have been given.
TEST(ShippedNoLibraryForFormat, AnEmptyImageIsRefusedAtDescriptorLoad) {
    ScratchDir dir{Location::Temp, "ffi-nolib-empty"};
    fs::path const   p = shippedLibsDirOf(dir) / "probe.json";
    writeDesc(p, descBody(kProbe, R"(["pe"])", R"({ "pe": "" })"));

    ReadRun const run = readOne(p);
    EXPECT_GE(run.malformed, 1u)
        << "a `library` key naming the EMPTY string must be refused: it is a "
           "SECOND spelling of \"no image on this format\" that the tiers read "
           "differently";
    EXPECT_NE(run.firstMessage.find("library.pe"), std::string::npos)
        << "the diagnostic must name the offending KEY, got: " << run.firstMessage;
}

// THE SAME CHOKEPOINT, THE OTHER CALLER. The per-SYMBOL `library` override runs
// through `decodeLibraryMap` too, and the anchor's whole claim is that this
// question must have ONE owner. A refusal that covered only the descriptor-level
// map would leave the identical hole one nesting level down.
TEST(ShippedNoLibraryForFormat, AnEmptyImageIsRefusedInThePerSymbolOverrideToo) {
    ScratchDir dir{Location::Temp, "ffi-nolib-empty-sym"};
    fs::path const   p = shippedLibsDirOf(dir) / "probe.json";
    writeDesc(p, descBody(kProbe, R"(["pe"])", R"({ "pe": "ucrtbase.dll" })",
                          R"(, "library": { "pe": "" })"));

    ReadRun const run = readOne(p);
    EXPECT_GE(run.malformed, 1u)
        << "the per-symbol `library` override shares `decodeLibraryMap` — the "
           "refusal must reach it, or the hole simply moves one level down";
    EXPECT_NE(run.firstMessage.find("library.pe"), std::string::npos)
        << "got: " << run.firstMessage;
}

// THE CONTROL FOR THE REFUSAL. An OMITTED key is the legitimate way to state
// "this format has no image", and it must still read CLEANLY. Without this, a
// refusal that fired on absence too would pass every arm above while breaking
// the twelve format-gated descriptors the corpus actually ships.
TEST(ShippedNoLibraryForFormat, AnOmittedFormatKeyIsLegalAndReadsCleanly) {
    ScratchDir dir{Location::Temp, "ffi-nolib-omitted"};
    fs::path const   p = shippedLibsDirOf(dir) / "probe.json";
    writeDesc(p, descBody(kProbe, R"(["pe"])", R"({ "elf": "libc.so.6" })"));

    ReadRun const run = readOne(p);
    EXPECT_TRUE(run.read) << "an omitted format key is LEGAL — it is how a "
                             "descriptor states that a format has no image";
    EXPECT_EQ(run.malformed, 0u) << run.firstMessage;
    ASSERT_TRUE(run.desc.has_value());
    EXPECT_TRUE(shippedLibraryImageForFormat(run.desc->library, "pe").empty());
    EXPECT_EQ(shippedLibraryImageForFormat(run.desc->library, "elf"), "libc.so.6");
}

// A row whose ONLY image entry is empty must never reach the oracle as
// `Realized`. This is the end-to-end shape of the split: before the load
// refusal, `{"pe":""}` answered `Realized` carrying no image, and the tier that
// asked `contains()` and the tier that asked for the VALUE disagreed about the
// same row. The descriptor is now refused at read, so the oracle — which SKIPS
// a descriptor that fails to read, by its own stated contract — leaves the name
// `Unknown` and the reference routes unbound. Either way the answer is unbound;
// what changed is that it is no longer reached through a status that CLAIMS
// realization.
TEST(ShippedNoLibraryForFormat, AnEmptyImageNeverAnswersRealized) {
    ScratchDir dir{Location::Temp, "ffi-nolib-empty-oracle"};
    writeDesc(shippedLibsDirOf(dir) / "probe.json",
              descBody(kProbe, R"(["pe"])", R"({ "pe": "" })"));

    OracleRun const run = ask(dir.path(), ObjectFormatKind::Pe);
    ASSERT_TRUE(run.located);
    if (run.answered) {
        ASSERT_TRUE(run.row.has_value());
        EXPECT_NE(run.row->status, ShippedRealizationStatus::Realized)
            << "a row that names NO image must never report itself REALIZED — "
               "that is the status the binder reads as \"bind this\"";
        EXPECT_TRUE(shippedLibraryImageForFormat(run.row->library, "pe").empty());
    }
}

// ── 3. THE SHARED ACCESSOR IS THE ONE OWNER ──────────────────────────────────

// Both spellings of absence collapse to the SAME answer, and a named image is
// returned verbatim. This is the predicate `realizeRow` decides the arm with and
// the one the cross-descriptor consistency checker renders with, so a tier
// cannot read one spelling as an image while another reads it as absence.
TEST(ShippedNoLibraryForFormat, TheAccessorCollapsesBothSpellingsOfAbsence) {
    std::unordered_map<std::string, std::string> const named{{"pe", "ucrtbase.dll"}};
    std::unordered_map<std::string, std::string> const emptyValue{{"pe", ""}};
    std::unordered_map<std::string, std::string> const absent{{"elf", "libc.so.6"}};

    EXPECT_EQ(shippedLibraryImageForFormat(named, "pe"), "ucrtbase.dll");
    EXPECT_TRUE(shippedLibraryImageForFormat(emptyValue, "pe").empty());
    EXPECT_TRUE(shippedLibraryImageForFormat(absent, "pe").empty());
    EXPECT_TRUE(shippedLibraryImageForFormat({}, "pe").empty());
}

// ── 4. THE `#include` PATH REFUSES A BODY-LESS ROW WHERE THE FACT IS DECIDED ──
//
// [[D-DIAG-NOLIBRARYFORFORMAT-REPORTS-AN-HIR-NODE-FOR-A-CONFIG-CONDITION]]. The
// arm above is the HAND-WRITTEN declaration's: the oracle states it and routes
// the reference unbound. A row reached through `#include` took a different road:
// the semantic tier injected it, HIR marked it library-bound with no image, and
// the build stopped three tiers lower on `H_UnsupportedLoweringForKind` "HIR
// ExternFunction (id N) — `importLibrary` is missing from the
// HirAttribute<FfiMetadata> side-table" — ✔MEASURED, a node id and an internal
// attribute, naming neither the descriptor, nor the symbol, nor the format.
//
// The injection now asks `refuseShippedSymbolWithoutABody` per row it injects —
// the same `realizeRow` kernel the oracle uses — and reports the answer on the
// `#include`. The VERDICT did not move (the build stopped before and stops now,
// for every such row the header declares, called or not); the tier and the words
// did. The hand-written road is unchanged, and section 5 pins that it still is.
namespace {

// A descriptor exactly as the injection holds one, with `sym` its only row.
[[nodiscard]] ShippedLibDescriptor oneRowDescriptor(ShippedSymbol sym) {
    ShippedLibDescriptor desc;
    desc.header = "probe.h";
    desc.symbols.push_back(std::move(sym));
    return desc;
}

[[nodiscard]] ShippedSymbol probeRow(std::vector<std::string> avail) {
    ShippedSymbol sym;
    sym.name                   = kProbe;
    sym.availableObjectFormats = std::move(avail);
    return sym;
}

// The refusal for the only row of `desc`, on `fmt`.
[[nodiscard]] std::optional<ParseDiagnostic>
refusalOf(ShippedLibDescriptor const& desc, ObjectFormatKind fmt) {
    return refuseShippedSymbolWithoutABody(desc, desc.symbols.front(), fmt,
                                           fs::path{"shippedLibs"} / "probe.json");
}

} // namespace

// The witness: the row says it exists on pe and names a body only for elf. The
// refusal is an Error under the new code, and its RENDERED text — what the reader
// sees — names the descriptor, the symbol and the format.
TEST(ShippedNoLibraryForFormat, TheIncludePathRefusalNamesDescriptorSymbolAndFormat) {
    ShippedLibDescriptor desc = oneRowDescriptor(probeRow({"pe"}));
    desc.library = {{"elf", "libc.so.6"}};

    auto const d = refusalOf(desc, ObjectFormatKind::Pe);
    ASSERT_TRUE(d.has_value())
        << "an AVAILABLE row with no image, no source and no recipe for the "
           "active format has nothing to bind: the injection must say so";
    EXPECT_EQ(d->code, DiagnosticCode::F_ShippedSymbolDeclaresNoBodyForFormat);
    EXPECT_EQ(d->severity, DiagnosticSeverity::Error);

    DiagnosticReporter rep;
    rep.report(*d);
    std::string const rendered = rep.formatAll(BufferRegistry{});
    EXPECT_NE(rendered.find("shippedLibs/probe.json"), std::string::npos)
        << "the DESCRIPTOR must be named; got:\n" << rendered;
    EXPECT_NE(rendered.find(std::string{"`"} + kProbe + "`"), std::string::npos)
        << "the SYMBOL must be named; got:\n" << rendered;
    EXPECT_NE(rendered.find("object format `pe`"), std::string::npos)
        << "the FORMAT must be named; got:\n" << rendered;
    EXPECT_EQ(rendered.find("HirAttribute"), std::string::npos)
        << "no internal attribute name belongs in a statement about config";
}

// Every row that DOES have a body on the active format — each of the three kinds
// of body, and a role the caller did not resolve — and a row or a descriptor not
// available here at all, is answered with nothing. Without these the case above
// could pass on a function that refuses everything.
TEST(ShippedNoLibraryForFormat, TheIncludePathRefusalIsSilentForEveryRowWithABody) {
    {
        ShippedLibDescriptor desc = oneRowDescriptor(probeRow({"pe"}));
        desc.library = {{"elf", "libc.so.6"}, {"pe", "ucrtbase.dll"}};
        EXPECT_FALSE(refusalOf(desc, ObjectFormatKind::Pe).has_value())
            << "an IMAGE for the active format is a body";
    }
    {
        ShippedSymbol sym = probeRow({"pe"});
        sym.library = {{"pe", "ucrtbase.dll"}};   // the per-SYMBOL override
        ShippedLibDescriptor desc = oneRowDescriptor(std::move(sym));
        desc.library = {{"elf", "libc.so.6"}};
        EXPECT_FALSE(refusalOf(desc, ObjectFormatKind::Pe).has_value())
            << "the symbol's own image merges over the descriptor's, as the "
               "injection merges it";
    }
    {
        ShippedLibDescriptor desc = oneRowDescriptor(probeRow({"pe"}));
        desc.realization = {{"pe", "runtime/platform/src/probe.c"}};
        EXPECT_FALSE(refusalOf(desc, ObjectFormatKind::Pe).has_value())
            << "a shipped SOURCE for the active format is a body";
    }
    {
        ShippedSymbol sym = probeRow({"pe"});
        sym.synthesize = kProbe;
        EXPECT_FALSE(refusalOf(oneRowDescriptor(std::move(sym)),
                               ObjectFormatKind::Pe).has_value())
            << "a `synthesize` recipe is a compiler-emitted body and needs no image";
    }
    {
        ShippedLibDescriptor desc = oneRowDescriptor(probeRow({"pe"}));
        desc.libraryRoles = {{"pe", RuntimeLibraryRole::CLibrary}};
        EXPECT_FALSE(refusalOf(desc, ObjectFormatKind::Pe).has_value())
            << "a ROLE is a body declared through the format's runtime-library "
               "table; a caller that resolved no roles binds nothing, and that is "
               "not a descriptor without a body";
    }
    {
        ShippedLibDescriptor desc = oneRowDescriptor(probeRow({"elf", "macho"}));
        desc.library = {{"elf", "libc.so.6"}};
        EXPECT_FALSE(refusalOf(desc, ObjectFormatKind::Pe).has_value())
            << "a row NOT available here is the symbol gate's to answer";
    }
    {
        ShippedLibDescriptor desc = oneRowDescriptor(probeRow({}));
        desc.availableObjectFormats = {"elf"};
        desc.library = {{"elf", "libc.so.6"}};
        EXPECT_FALSE(refusalOf(desc, ObjectFormatKind::Pe).has_value())
            << "a DESCRIPTOR not available here declares nothing here";
    }
}

// The shipped corpus, read the way the editor and the header parser read it (no
// role resolver), on every object format a shipped target builds for: no row the
// `#include` path injects may be refused. A corpus edit that leaves an available
// row without a body reds here, at the gate, before any user's `#include` does.
//
// ⚠ elf / pe / macho ONLY, on purpose: `wasm32-v1` and `spirv-1.6` are declared
// formats no shipped target pairs with, and the 125 rows that declare no
// `availableObjectFormats` (available everywhere) name images only for the three
// (✔MEASURED, see the file header). On those two the refusal is the true answer.
TEST(ShippedNoLibraryForFormat, NoShippedRowIsBodylessOnAFormatAShippedTargetBuilds) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    fs::path const libs = *cfg / "shippedLibs";
    ASSERT_TRUE(fs::is_directory(libs)) << libs.generic_string();

    // ★ ON EVERY DISTINCT REAL PAIR, WITH THE PAIR'S OWN FACTS (P68 round 12,
    // S2a-1). The legs were three hand-written (format, data model) rows read as
    // "x86_64" — arm64 was never read — with no pair facts, and a descriptor
    // that FAILED to read was skipped in silence (`continue`), so a corpus edit
    // that broke a read on one format shrank this sweep instead of reddening it;
    // the floors below could not see a handful of missing files. The pairs now
    // come from the shipped documents (`shipped_read_pairs.hpp`) and a failed
    // read is a failure, named.
    auto const& pairs = dss::test_support::shippedReadPairs();
    ASSERT_FALSE(pairs.empty());
    std::size_t descriptorsRead = 0;
    std::size_t rowsAsked       = 0;
    std::vector<std::string> refused;
    for (auto const& pair : pairs) {
        SCOPED_TRACE(pair.label());
        ShippedPairFacts const facts = pair.pairFacts();
        for (auto const& entry : fs::recursive_directory_iterator{libs}) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            TypeInterner       interner{CompilationUnitId{1}};
            TypeRegistry       typeReg;
            DiagnosticReporter rep;
            // stdio.json's `vfprintf` spells the ABI alias `va_list`, and a read
            // without a binding fails loud — which the silent `continue` this
            // sweep used to take HID: ✔MEASURED at S2a-1, stdio.json was never
            // read here on any leg, so none of its rows was ever asked. Any
            // consistent stand-in serves, as in the consistency sweep: nothing
            // here reads a TypeId.
            std::array<NamedTypeBinding, 1> const named{NamedTypeBinding{
                "va_list", interner.pointer(interner.primitive(TypeKind::Void))}};
            auto const desc = readShippedLibDescriptor(
                entry.path(), interner, typeReg, rep, pair.dataModel(),
                pair.activeTarget(), pair.activeFormat(), named, nullptr, &facts);
            EXPECT_TRUE(desc.has_value())
                << entry.path().generic_string() << " failed to read on this pair: "
                << (rep.all().empty() ? std::string{"<no diagnostic>"}
                                      : rep.all().front().actual);
            if (!desc.has_value()) continue;
            ++descriptorsRead;
            for (auto const& sym : desc->symbols) {
                ++rowsAsked;
                if (auto const d = refuseShippedSymbolWithoutABody(
                        *desc, sym, pair.kind, entry.path()))
                    refused.push_back(d->actual);
            }
        }
    }
    // Floors: a walk that read nothing would pass the assertion below vacuously.
    EXPECT_GE(descriptorsRead, 100u) << "the descriptor walk collapsed";
    EXPECT_GE(rowsAsked, 1000u) << "the row walk collapsed";
    EXPECT_TRUE(refused.empty())
        << refused.size() << " shipped row(s) would be refused on `#include`; "
        << "first: " << (refused.empty() ? std::string{} : refused.front());
}

// Suppression cannot hide it: the semantic tier's error gate stops the build on
// the model's own error count, so a suppressed line would leave a failed build
// with nothing naming the row (prong 2).
TEST(ShippedNoLibraryForFormat, TheIncludePathRefusalCannotBeSuppressed) {
    constexpr auto code = DiagnosticCode::F_ShippedSymbolDeclaresNoBodyForFormat;
    EXPECT_TRUE(isUnsuppressable(code));
    EXPECT_EQ(membershipProngOf(code), MembershipProng::BuildFailsWithNothingSaid);

    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(code);
    DiagnosticReporter r{cfg};
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = "shipped descriptor `x.json` declares `f` on `pe` with no body";
    r.report(std::move(d));
    ASSERT_EQ(r.all().size(), 1u) << "--suppress silenced it";
    EXPECT_NE(r.all()[0].actual.find("x.json"), std::string::npos)
        << "the delivered diagnostic must still NAME the row";
    EXPECT_EQ(r.errorCount(), 1u);
}

// ── 5. END TO END: THE DRIVER BUILDS A HEADER WHOSE ROW LOST ITS BODY ─────────
//
// The row's own closing test: a fixture that REMOVES a format's body — in a
// staged COPY of the shipped config tree; the shipped descriptor is never edited
// — built by the driver. `dirent.json`'s descriptor-level `realization` map (its
// only entry is `pe`) is deleted from the copy, so on pe `opendir` / `readdir` /
// `closedir` stay declared and available with no body.
namespace {

// A repo-shaped copy of the shipped config tree (DSS_CONFIG_ROOT names the
// directory CONTAINING `src/dss-config`) with `dirent.json`'s `realization` map
// removed. Returns the root, or an empty path after a failure it reported.
[[nodiscard]] fs::path stageTreeWithoutDirentRealization(ScratchDir const& dir) {
    auto const shipped = dss::test::findConfigRoot();
    if (!shipped) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    fs::path const root = dir.path() / "tree";
    fs::path const cfg  = root / "src" / "dss-config";
    std::error_code ec;
    fs::create_directories(cfg.parent_path(), ec);
    fs::copy(*shipped, cfg, fs::copy_options::recursive, ec);
    if (ec) {
        ADD_FAILURE() << "staging the config tree failed: " << ec.message();
        return {};
    }
    fs::path const dirent = cfg / "shippedLibs" / "dirent.json";
    nlohmann::json doc;
    {
        std::ifstream in{dirent, std::ios::binary};
        doc = nlohmann::json::parse(in, nullptr, false);
    }
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("realization")) {
        ADD_FAILURE() << "the staged dirent.json has no `realization` map to "
                         "remove — this fixture no longer removes a body";
        return {};
    }
    doc.erase("realization");
    {
        std::ofstream out{dirent, std::ios::binary | std::ios::trunc};
        out << doc.dump(2);
    }
    return root;
}

struct DriverRun {
    int                rc = -1;
    DiagnosticReporter rep;
};

// Build `text` for `spec` through the driver, under `configRoot` when it is not
// empty (the shipped tree otherwise).
[[nodiscard]] DriverRun buildUnder(fs::path const& configRoot, ScratchDir const& dir,
                                   std::string const& tag, std::string_view text,
                                   std::string const& spec) {
    std::optional<ScopedEnv> env;
    if (!configRoot.empty()) env.emplace("DSS_CONFIG_ROOT", configRoot.string());
    DriverRun run;
    fs::path const src = dir.path() / (tag + ".c");
    {
        std::ofstream f{src, std::ios::binary};
        f << text;
    }
    dss::Program p;
    p.setOutputDir(dir.path() / ("out-" + tag));
    run.rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                            std::vector<std::string>{spec}, run.rep);
    return run;
}

constexpr char const* kDirentProgram =
    "#include <dirent.h>\n"
    "int main(void) {\n"
    "    DIR *d = opendir(\".\");\n"
    "    if (d == 0) return 1;\n"
    "    (void)readdir(d);\n"
    "    return closedir(d);\n"
    "}\n";
constexpr char const* kPeSpec  = "x86_64:pe64-x86_64-windows-exec";
constexpr char const* kElfSpec = "x86_64:elf64-x86_64-linux-exec";

} // namespace

TEST(ShippedNoLibraryForFormat, ABuildOfAHeaderWhoseRowLostItsBodyNamesAllThree) {
    ScratchDir dir{Location::Temp, "ffi-nobody-e2e"};
    fs::path const root = stageTreeWithoutDirentRealization(dir);
    ASSERT_FALSE(root.empty());

    DriverRun const run = buildUnder(root, dir, "nobody-pe", kDirentProgram, kPeSpec);
    EXPECT_NE(run.rc, 0) << "rows with no body on pe must stop the build";
    EXPECT_EQ(countCode(run.rep, DiagnosticCode::F_ShippedSymbolDeclaresNoBodyForFormat), 3u)
        << "one refusal per body-less row `<dirent.h>` declares on pe";
    EXPECT_EQ(countCode(run.rep, DiagnosticCode::H_UnsupportedLoweringForKind), 0u)
        << "the refusal is raised where the fact is decided; nothing reaches HIR";

    std::string const rendered = run.rep.formatAll(BufferRegistry{});
    for (char const* needle :
         {"shippedLibs/dirent.json", "`opendir`", "object format `pe`"}) {
        EXPECT_NE(rendered.find(needle), std::string::npos)
            << needle << " is not named; got:\n" << rendered;
    }
    for (auto const& d : run.rep.all()) {
        if (d.code != DiagnosticCode::F_ShippedSymbolDeclaresNoBodyForFormat) continue;
        EXPECT_TRUE(d.buffer.valid()) << "the refusal must stand on the `#include`";
        EXPECT_GT(d.span.length(), 0u);
    }
}

// The controls: the SAME staged tree on elf (dirent's rows name an image there),
// and the SHIPPED tree on pe (the realization source is there) — both build.
// Without them the case above could be measuring a fixture that breaks every
// build rather than a missing body.
TEST(ShippedNoLibraryForFormat, TheSameProgramBuildsWhereTheRowsHaveABody) {
    ScratchDir dir{Location::Temp, "ffi-nobody-controls"};
    fs::path const root = stageTreeWithoutDirentRealization(dir);
    ASSERT_FALSE(root.empty());

    DriverRun const elf = buildUnder(root, dir, "nobody-elf", kDirentProgram, kElfSpec);
    EXPECT_EQ(elf.rc, 0) << elf.rep.formatAll(BufferRegistry{});
    EXPECT_EQ(countCode(elf.rep, DiagnosticCode::F_ShippedSymbolDeclaresNoBodyForFormat), 0u);

    DriverRun const shipped = buildUnder({}, dir, "shipped-pe", kDirentProgram, kPeSpec);
    EXPECT_EQ(shipped.rc, 0) << shipped.rep.formatAll(BufferRegistry{});
    EXPECT_EQ(countCode(shipped.rep, DiagnosticCode::F_ShippedSymbolDeclaresNoBodyForFormat), 0u);
}

// The hand-written road is unchanged: a bare declaration of the same body-less
// name is the oracle's `NoLibraryForFormat`, routed UNBOUND, and judged by the
// LINK tier by name — not by the `#include` path's refusal.
TEST(ShippedNoLibraryForFormat, AHandWrittenDeclarationStillRoutesToTheLinkTier) {
    ScratchDir dir{Location::Temp, "ffi-nobody-handwritten"};
    fs::path const root = stageTreeWithoutDirentRealization(dir);
    ASSERT_FALSE(root.empty());

    DriverRun const run = buildUnder(root, dir, "handwritten-pe",
                                     "typedef struct DIR DIR;\n"
                                     "extern DIR *opendir(const char *name);\n"
                                     "int main(void) { return opendir(\".\") == 0; }\n",
                                     kPeSpec);
    EXPECT_NE(run.rc, 0);
    EXPECT_GE(countCode(run.rep, DiagnosticCode::K_SymbolUndefined), 1u)
        << run.rep.formatAll(BufferRegistry{});
    EXPECT_EQ(countCode(run.rep, DiagnosticCode::F_ShippedSymbolDeclaresNoBodyForFormat), 0u)
        << "no `#include` injected the row, so the injection's refusal cannot apply";
}
