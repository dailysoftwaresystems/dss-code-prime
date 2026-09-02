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

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "diagnostic_count.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
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
