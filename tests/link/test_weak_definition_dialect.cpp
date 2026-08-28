// ── D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED ────────────────────────
//    The CONFIG half: the `weakDefinition` block's vocabulary, its loader, and
//    the JSON-free `validate()` twin. The COFF WRITER half — the byte-level
//    pins that prove `pe::encode` emits COMDAT select-any — lives in
//    `test_pe_writer.cpp`, because that is where the encoder is.
//
//    ★ SECTION 5 (cycle P28) ADDS THE ELF AND MACH-O CONSULTATION PINS HERE
//    rather than in their sibling writer files, because since
//    [[D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS]] all
//    three walkers ask ONE shared gate
//    (`src/link/format/weak_definition_gate.hpp`). A copy of the question per
//    writer file would be three pins of one property, which is the drift the
//    shared gate exists to remove.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ WHAT THIS KEY IS, AND WHAT IT DELIBERATELY IS NOT. It is a DIALECT row:
// *how* a format spells a weak definition, a thing it CAN express. It is NOT a
// capability flag asserting that a format CANNOT. The operator's 2026-08-20
// ruling that authorized the COFF weak machinery said exactly that — config
// gets ONE row, the dialect, and explicitly not a `canExpressWeakAlias` flag —
// and [[D-LK-PE-ALTERNATENAME-DECLARE-AND-REFUSE]] is the record of why the
// second shape is dangerous: an implementation gap recorded as a format
// INCAPABILITY is a false fact in the place most likely to be trusted later,
// and it does not reverse cleanly. That row closed the day its "incapability"
// turned out to be an unbuilt writer.
//
// ★★ THE HAZARD THIS FILE IS AIMED AT IS THE INVERSE ONE: **A KEY THE WRITER
// DOES NOT READ IS WORSE THAN NO KEY.** It drifts silently and reads as
// authoritative. So two properties are pinned together and neither carries the
// claim alone:
//   (A) the SHIPPED corpus declares the key only where a writer consults it
//       (`ShippedDeclarationsAreOnlyWhereAWriterConsultsThem`); and
//   (B) a declaration that is malformed, empty, or names a spelling the
//       vocabulary does not own is REFUSED AT LOAD, with a message rendered
//       from the table rather than retyped beside the check.
//
// ⚠ MUST run through ctest, never a bare `.exe`: the shipped-config resolver
// walks the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets
// it. A bare binary reads whichever config tree the shell happens to stand in,
// so the corpus arm would be asserting something about a directory nobody
// chose.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/enum_name_table.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/format/macho.hpp"
#include "link/format/weak_definition_gate.hpp"
#include "link/object_format_backend.hpp"
#include "link/object_format_schema.hpp"

#include "format_reject_support.hpp"
#include "repo_root.hpp"
#include "vocabulary_message_probe.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using json = nlohmann::json;
using dss::link_format::test::countAtPath;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

namespace {

// A spelling no vocabulary in this tree claims. Deliberately ugly: a probe that
// collided with a real name would make every negative arm pass for the wrong
// reason.
constexpr char const* kBadSpelling = "zzNotAWeakDefinitionDialectSpelling";

// The shipped formats whose WALKER consults `weakDefinition`, and the dialect
// each one must declare. Written out ONE ROW AT A TIME rather than derived from
// anything, and that is the teeth — see
// `ShippedDeclarationsAreOnlyWhereAWriterConsultsThem`.
//
// ★★ WHY A LITERAL AND NOT A DERIVATION. The measured failure mode in this
// family (P23, the return-type side) is a pin whose EXPECTATION comes off the
// same table as the code: deleting a row moved both halves of the comparison
// together and reddened nothing. Every alternative spelling of this list is
// that defect — projecting `objectFormatBackendTable()` would make the pin
// green for any corpus consistent with the backends, and globbing the
// documents that declare the key would make it `x == x`. The two owners here
// are the SHIPPED JSON ON DISK and this hand-written table; nothing keeps them
// in step but a human editing both, which is exactly the coupling being pinned.
//
// ⚠ 16 ROWS, NOT 24, AND THE EIGHT ABSENCES ARE THE DESIGN.
//   * the four Mach-O IMAGE documents (exec / dylib × 2 arches) — their walker
//     REFUSES a weak definition (`refuseWeakImageAlias`: N_WEAK_DEF on an image
//     needs MH_WEAK_DEFINES in the mach header, D-LK3-DYLIB-WEAK-EXPORT), so it
//     never asks;
//   * the two pe IMAGE documents (exec / dll) — the COMDAT encoder is the `.obj`
//     arm only;
//   * spirv and wasm — no walker there writes a weak definition in any spelling.
// A key nobody reads drifts silently while reading as authoritative, which is
// worse than no key at all; that is the whole reason
// [[D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED]] landed with two rows and not
// twenty-four.
[[nodiscard]] std::map<std::string, WeakDefinitionDialect> const&
consultingFormats() {
    static std::map<std::string, WeakDefinitionDialect> const kRows{
        // COFF `.obj` — the arm of `pe::encode` that emits a per-body
        // IMAGE_SCN_LNK_COMDAT section with Selection = SELECT_ANY.
        { "pe64-x86_64-windows",             WeakDefinitionDialect::Comdat        },
        { "pe64-x86_64-windows-staticlib",   WeakDefinitionDialect::Comdat        },
        // `elf::encode` — STB_WEAK through `stbForBinding`. EVERY flavor,
        // because the alias pass is deliberately not `isExec`-gated, so an
        // ET_EXEC image emits a weak alias exactly as an ET_REL `.o` does.
        { "elf64-aarch64-linux",             WeakDefinitionDialect::SymbolBinding },
        { "elf64-aarch64-linux-dyn",         WeakDefinitionDialect::SymbolBinding },
        { "elf64-aarch64-linux-exec",        WeakDefinitionDialect::SymbolBinding },
        { "elf64-aarch64-linux-pie",         WeakDefinitionDialect::SymbolBinding },
        { "elf64-aarch64-linux-staticlib",   WeakDefinitionDialect::SymbolBinding },
        { "elf64-x86_64-linux",              WeakDefinitionDialect::SymbolBinding },
        { "elf64-x86_64-linux-dyn",          WeakDefinitionDialect::SymbolBinding },
        { "elf64-x86_64-linux-exec",         WeakDefinitionDialect::SymbolBinding },
        { "elf64-x86_64-linux-pie",          WeakDefinitionDialect::SymbolBinding },
        { "elf64-x86_64-linux-staticlib",    WeakDefinitionDialect::SymbolBinding },
        // `macho::encode`'s MH_OBJECT arm — N_WEAK_DEF in `n_desc`. OBJECT and
        // staticlib only; see the absence note above.
        { "macho64-arm64-darwin",            WeakDefinitionDialect::SymbolFlag    },
        { "macho64-arm64-darwin-staticlib",  WeakDefinitionDialect::SymbolFlag    },
        { "macho64-x86_64-darwin",           WeakDefinitionDialect::SymbolFlag    },
        { "macho64-x86_64-darwin-staticlib", WeakDefinitionDialect::SymbolFlag    },
    };
    return kRows;
}

// The backends whose WALKER spells each dialect, hand-written for the same
// reason `consultingFormats` is: the subject under test is
// `ObjectFormatBackend::weakDefinitionDialects()`, so an expectation projected
// from it asserts nothing. A backend absent from this map must declare NONE.
[[nodiscard]] std::map<std::string, std::set<WeakDefinitionDialect>> const&
spellingBackends() {
    static std::map<std::string, std::set<WeakDefinitionDialect>> const kRows{
        { "elf",   { WeakDefinitionDialect::SymbolBinding } },
        { "macho", { WeakDefinitionDialect::SymbolFlag    } },
        { "pe",    { WeakDefinitionDialect::Comdat        } },
        { "spirv", {} },
        { "wasm",  {} },
    };
    return kRows;
}

struct ShippedFormatDoc {
    std::string name;
    json        doc;
};

// Enumerated FROM DISK, never from a hard-coded list — a format added tomorrow
// is probed the day it lands. (The `shippedFormatDocs` shape used by
// `test_object_format_vocabulary_projection.cpp`, for the same reason.)
[[nodiscard]] std::vector<ShippedFormatDoc> shippedFormatDocs() {
    std::vector<ShippedFormatDoc> out;
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return out;
    }
    auto const dir = *root / "object-formats";
    std::vector<fs::path> paths;
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator{dir, ec}) {
        auto const p = entry.path();
        if (p.filename().string().find(".format.json") == std::string::npos) {
            continue;
        }
        paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end());
    for (auto const& p : paths) {
        std::ifstream in{p, std::ios::binary};
        if (!in) { ADD_FAILURE() << "cannot open " << p.string(); continue; }
        std::string text{std::istreambuf_iterator<char>{in},
                         std::istreambuf_iterator<char>{}};
        ShippedFormatDoc f;
        f.doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (f.doc.is_discarded() || !f.doc.is_object()) {
            ADD_FAILURE() << p.filename().string() << " is not a JSON object";
            continue;
        }
        auto const filename = p.filename().string();
        f.name = filename.substr(
            0, filename.size() - std::string_view{".format.json"}.size());
        out.push_back(std::move(f));
    }
    return out;
}

// A shipped document, as JSON — the fixtures every negative arm mutates, so no
// arm can rot against a hand-written format-like blob that the real file has
// since outgrown.
[[nodiscard]] json shippedDoc(std::string_view name) {
    for (auto& f : shippedFormatDocs()) {
        if (f.name == name) return std::move(f.doc);
    }
    ADD_FAILURE() << name << ".format.json not found";
    return json::object();
}

[[nodiscard]] json shippedPeObjectDoc() {
    return shippedDoc("pe64-x86_64-windows");
}

// ★ `quotedTokens` used to be a file-local copy here, and it was the one that
// had DRIFTED: it returned a `std::set<std::string>` where the other four
// returned a `std::vector<std::string>`. The vector wins — it keeps ORDER and
// MULTIPLICITY, and a set is derivable from it while the reverse is not — so
// this file now uses the one owner,
// `tests/test_support/vocabulary_message_probe.hpp`. See
// D-TEST-VOCABULARY-PROBE-MESSAGE-HALF-IS-UNREACHABLE-AND-JSON-COUPLED.
//
// ⚠ THE NARROWING HAD ALREADY BLURRED SIX ASSERTIONS —
// D-TEST-WEAK-DEFINITION-QUOTED-TOKEN-COUNT-IS-UNFALSIFIABLE-OVER-A-SET —
// which is why the call sites below now say which of the two things they mean.
// All six were written
// as `quoted.count(x) == 1`, and on a `std::set` `count` is 0-or-1 — so the
// `== 1` an author reads as "named exactly once" was UNFALSIFIABLE and the six
// sites were indistinguishable. Over the vector the count is real, and
// ✔MEASURED it separates them: the completeness loop is genuinely `== 1` (the
// rendered allowed-list names each accepted spelling once, so a duplicate table
// row would show up there and nowhere else), while the cross-kind arms are
// presence — `'elf'` appears THREE times in the mis-declaration sentence, by
// design. Each site now spells the one it means.
using ::dss::test_support::quotedTokens;

// Occurrences of `name` among a message's quoted tokens. Named rather than
// spelled out at each site so the multiplicity the `std::set` erased is stated
// once, in the vocabulary the shared owner hands back.
[[nodiscard]] std::size_t quotedCount(std::vector<std::string> const& quoted,
                                      std::string_view                name) {
    return static_cast<std::size_t>(
        std::count(quoted.begin(), quoted.end(), std::string{name}));
}

// The message of the first ERROR diagnostic pinned at `path`, or "" if the load
// succeeded or nothing fired there.
[[nodiscard]] std::string messageAtPath(
        dss::link_format::test::FormatLoadResult const& r,
        std::string_view path) {
    if (r.has_value()) return {};
    for (auto const& d : r.error()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        if (d.path == path) return d.message;
    }
    return {};
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════
// 1. THE VOCABULARY ITSELF — the sentinel must have NO spelling, in BOTH
//    directions. This is tested first because every other arm assumes it.
// ═════════════════════════════════════════════════════════════════════════

TEST(WeakDefinitionDialect, SentinelHasNoSpellingInEitherDirection) {
    // NAME side: an unlisted value renders EMPTY, not as the first row's name.
    // `EnumNameTable::name()` falls back to `rows[0].second`, which here would
    // report the sentinel as `comdat` — a legitimate dialect — and
    // `validate()`'s `…Name(x).empty()` guard would then see a populated field
    // where none was declared. `weakDefinitionDialectName` asks `nameOrEmpty`
    // for exactly that reason.
    EXPECT_TRUE(
        weakDefinitionDialectName(WeakDefinitionDialect::Unspecified).empty())
        << "the invalid sentinel must render EMPTY; if it renders as a real "
           "dialect the validate() guard silently stops guarding";

    // FROM-NAME side: no string resolves to the sentinel — including the empty
    // string, which is what a `"dialect": ""` typo produces. Listing the
    // sentinel with an empty name would re-open the hole from this side.
    EXPECT_FALSE(weakDefinitionDialectFromName("").has_value());
    EXPECT_FALSE(weakDefinitionDialectFromName("unspecified").has_value());
    EXPECT_FALSE(weakDefinitionDialectFromName(kBadSpelling).has_value());

    // …and every real spelling DOES round-trip, so the refusals above are not
    // simply a lookup that never returns anything.
    for (auto const& name : allNames(kWeakDefinitionDialectTable)) {
        auto const d = weakDefinitionDialectFromName(name);
        ASSERT_TRUE(d.has_value()) << "table spelling '" << name
                                   << "' does not resolve";
        EXPECT_EQ(weakDefinitionDialectName(*d), name);
    }
}

// ═════════════════════════════════════════════════════════════════════════
// 2. THE SHIPPED CORPUS — a key nobody reads is worse than no key.
// ═════════════════════════════════════════════════════════════════════════

TEST(WeakDefinitionDialect, ShippedDeclarationsAreOnlyWhereAWriterConsultsThem) {
    // ★★ THIS IS THE PIN THE WHOLE ROW TURNS ON. The measured reason this key
    // did not land in the cycle that ordered it was that a key the writer does
    // not READ drifts silently while reading as authoritative. So exactly the
    // formats whose walker CONSULTS the key may declare it — no more, and no
    // fewer.
    //
    // ⚠ WIDENED, NOT DELETED, when the ELF and Mach-O writers gained their
    // consultation in cycle P28 — the row is
    // [[D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS]]:
    // 2 rows became 16, in the SAME change as the walkers and the documents. A
    // pin deleted because it started failing is a pin that asserted nothing.
    //
    // ★ AND IT NOW ASSERTS THE DIALECT PER DOCUMENT, NOT JUST MEMBERSHIP. With
    // one consulting walker the old arm could hardcode `== Comdat` for every
    // declaring document; with three dialects that arm would either have to
    // stop checking the value or re-derive it from the document — the second is
    // `x == x`. The literal table in `consultingFormats()` is what stops both.
    auto const formats = shippedFormatDocs();
    ASSERT_FALSE(formats.empty());
    // The corpus is real: if the enumeration ever silently returned two files,
    // every arm below would be vacuous.
    EXPECT_GE(formats.size(), 20u)
        << "the shipped object-format corpus should be ~24 documents";
    // …and so is the expectation. An emptied table would make the set
    // comparison below pass against a corpus that declares the key NOWHERE.
    ASSERT_EQ(consultingFormats().size(), 16u)
        << "the hand-written consulting-format table has been emptied or "
           "half-edited — every arm here would then assert nothing";

    std::set<std::string> declaring;
    std::set<std::string> expected;
    for (auto const& [name, dialect] : consultingFormats()) {
        (void)dialect;
        expected.insert(name);
    }
    for (auto const& f : formats) {
        if (!f.doc.contains("weakDefinition")) continue;
        declaring.insert(f.name);
        // Every declaration must name the dialect its consulting walker
        // encodes. An ELF document declaring `comdat` would be a key the ELF
        // writer refuses — which is loud, but the corpus should never contain
        // one, and since P28 the LOADER refuses it outright (see
        // `MisDeclaredDialectIsRefusedAtLoadNamingTheWalkerThatSpellsIt`).
        auto const loaded = ObjectFormatSchema::loadShipped(f.name);
        ASSERT_TRUE(loaded.has_value())
            << f.name << " no longer loads: " << rejectSummary(loaded);
        auto const wd = (*loaded)->weakDefinition();
        ASSERT_TRUE(wd.has_value())
            << f.name << " declares 'weakDefinition' in JSON but the schema "
                         "carries none — the loader dropped it";
        auto const row = consultingFormats().find(f.name);
        ASSERT_NE(row, consultingFormats().end())
            << f.name << " declares 'weakDefinition' but no walker is recorded "
                         "as consulting it — a key nobody reads";
        EXPECT_EQ(wd->dialect, row->second)
            << f.name << " declares dialect '"
            << weakDefinitionDialectName(wd->dialect)
            << "', but its consulting walker spells '"
            << weakDefinitionDialectName(row->second) << "'";
    }
    EXPECT_EQ(declaring, expected)
        << "the set of shipped documents declaring 'weakDefinition' must equal "
           "the set whose walker CONSULTS it. A document in the left set only "
           "carries a key nobody reads; a format in the right set only has a "
           "walker that will refuse the first weak definition it meets.";
}

// ═════════════════════════════════════════════════════════════════════════
// 2b. THE BACKEND ACCESSOR — the second owner of the same fact, and the one
//     that lets a MIS-DECLARATION fail at LOAD instead of at emit.
//     D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS.
// ═════════════════════════════════════════════════════════════════════════

TEST(WeakDefinitionDialect, EachBackendDeclaresTheDialectsItsWalkerSpells) {
    auto const& table = link::objectFormatBackendTable();
    ASSERT_FALSE(table.empty());
    // Non-vacuity from BOTH sides: an emptied registry would make the loop
    // below run zero times, and an emptied expectation table would make every
    // comparison inside it trivially satisfiable.
    EXPECT_EQ(table.size(), spellingBackends().size())
        << "every registered backend must have a row in the expectation table "
           "— a new backend silently unlisted is exactly the drift this pins";
    ASSERT_EQ(spellingBackends().size(), 5u);

    std::set<WeakDefinitionDialect> spelledByAnyone;
    for (auto const* b : table) {
        auto const row = spellingBackends().find(std::string{b->configName()});
        ASSERT_NE(row, spellingBackends().end())
            << "backend '" << b->configName()
            << "' has no expectation row — add one rather than deleting this "
               "pin";
        std::set<WeakDefinitionDialect> got;
        for (WeakDefinitionDialect d : b->weakDefinitionDialects()) {
            got.insert(d);
            spelledByAnyone.insert(d);
        }
        EXPECT_EQ(got, row->second)
            << "backend '" << b->configName()
            << "' declares a different dialect set than its walker writes";
    }

    // ★ EVERY DIALECT IN THE VOCABULARY IS WRITTEN BY SOMEBODY. A spelling with
    // no walker behind it is a verb shipped ahead of its arm — the
    // `StackReserveVehicle` discipline — and it is also the one input that
    // would reach the loader's `<no walker spells it>` branch, which is
    // therefore unreachable by construction today and is documented as such at
    // its site rather than tested with a fixture that cannot be built.
    for (auto const& name : allNames(kWeakDefinitionDialectTable)) {
        auto const d = weakDefinitionDialectFromName(name);
        ASSERT_TRUE(d.has_value());
        // `.contains`, not `count(...) == 1`: on a `std::set` `count` is 0-or-1
        // by definition, so the `== 1` an author reads as "exactly once" is
        // unfalsifiable — D-TEST-WEAK-DEFINITION-QUOTED-TOKEN-COUNT-IS-UNFALSIFIABLE-OVER-A-SET,
        // which this same file documents and which the step-10 audit found
        // re-created here. Membership is the whole claim; say only that.
        EXPECT_TRUE(spelledByAnyone.contains(*d))
            << "dialect '" << name
            << "' is in the vocabulary but no backend's walker writes it";
    }
}

TEST(WeakDefinitionDialect, EveryShippedDeclarationIsSpelledByItsOwnBackend) {
    // The two owners of the fact, checked against each other on the SHIPPED
    // corpus: the JSON on disk says which dialect a format uses; the backend
    // says which dialects its walker writes. This is the invariant the loader
    // now enforces, asserted end-to-end on the real documents rather than only
    // on a mutated fixture.
    // ⚠ THE DIALECT IS READ OFF THE LOADED SCHEMA, NOT OFF `consultingFormats()`.
    // Taking it from the literal table would compare the table against the
    // backends and never touch the shipped JSON at all — the document could
    // lose its block entirely and this arm would stay green. Reading it from
    // the schema is what makes deleting a declaration from a shipped file red
    // THIS pin as well as the corpus pin above.
    std::size_t checked = 0;
    for (auto const& [name, expectedDialect] : consultingFormats()) {
        auto const loaded = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(loaded.has_value()) << name << ": " << rejectSummary(loaded);
        auto const* backend = (*loaded)->backend();
        ASSERT_NE(backend, nullptr) << name;
        auto const wd = (*loaded)->weakDefinition();
        ASSERT_TRUE(wd.has_value())
            << name << " consults the key but its schema carries no dialect — "
                       "the walker will refuse the first weak definition it "
                       "meets";
        EXPECT_EQ(wd->dialect, expectedDialect) << name;
        bool spelled = false;
        for (WeakDefinitionDialect d : backend->weakDefinitionDialects()) {
            if (d == wd->dialect) { spelled = true; break; }
        }
        EXPECT_TRUE(spelled)
            << name << " declares '" << weakDefinitionDialectName(wd->dialect)
            << "' but backend '" << backend->configName()
            << "' does not write it — the loader should have refused this "
               "document";
        ++checked;
    }
    EXPECT_EQ(checked, 16u) << "the loop must have run over every consulting "
                               "format, not a truncated list";
}

TEST(WeakDefinitionDialect, TheShippedPeObjectSchemaCarriesTheDeclaredDialect) {
    // The positive control for the loader: the value survives the JSON tier and
    // reaches the schema the walker is handed. Without this, every negative arm
    // below could pass against a loader that simply never populated the field.
    auto const loaded = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(loaded.has_value()) << rejectSummary(loaded);
    auto const wd = (*loaded)->weakDefinition();
    ASSERT_TRUE(wd.has_value());
    EXPECT_EQ(wd->dialect, WeakDefinitionDialect::Comdat);

    // …and a format that declares nothing reports nullopt rather than a
    // defaulted dialect. `std::nullopt` is the ABSENCE OF AN ANSWER, never the
    // claim that this format cannot express a weak definition.
    auto const exec = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(exec.has_value()) << rejectSummary(exec);
    EXPECT_FALSE((*exec)->weakDefinition().has_value())
        << "an undeclared block must read back as nullopt — a defaulted "
           "dialect would be config asserting a fact nobody wrote";
}

// ═════════════════════════════════════════════════════════════════════════
// 3. THE LOADER REFUSES EVERY MALFORMED DECLARATION, AND SAYS THE SAME SET
//    THE CHECK ACCEPTS.
// ═════════════════════════════════════════════════════════════════════════

TEST(WeakDefinitionDialect, UnknownSpellingIsRefusedAtItsOwnPointer) {
    json doc = shippedPeObjectDoc();
    doc["weakDefinition"]["dialect"] = kBadSpelling;
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(),
                                                    "weakdef-unknown");
    ASSERT_FALSE(r.has_value())
        << "an unknown dialect must be REFUSED — a spelling no walker "
           "implements would be a weak definition emitted as something else";
    EXPECT_EQ(countAtPath(r, "/weakDefinition/dialect"), 1u) << rejectSummary(r);
}

TEST(WeakDefinitionDialect, TheRefusalNamesEverySpellingAndInventsNone) {
    // D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET, on this
    // vocabulary. The message must be RENDERED from the table, so it cannot
    // drift into naming a set the check does not accept — in either direction.
    json doc = shippedPeObjectDoc();
    doc["weakDefinition"]["dialect"] = kBadSpelling;
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(),
                                                    "weakdef-projection");
    ASSERT_FALSE(r.has_value());
    auto const msg = messageAtPath(r, "/weakDefinition/dialect");
    ASSERT_FALSE(msg.empty()) << rejectSummary(r);
    auto const quoted = quotedTokens(msg);

    // (B) COMPLETENESS — every spelling the table accepts appears in the
    //     refusal. This is the direction the live defects in the sibling
    //     vocabularies were on, and the only one that catches them.
    //
    // ★ EXACTLY ONCE, and here that is a REAL count rather than the 0-or-1 a
    //   `std::set` could report. This sentence is `renderAllowedList` over the
    //   table plus the echoed bad spelling, so an accepted spelling appearing
    //   TWICE means the table carries a duplicate row — the rendered list is
    //   the only place that shows it, since `…FromName` would answer for either
    //   copy. ✔MEASURED green at 1 for all three shipped spellings.
    //   ⚠ If a reword ever makes this sentence mention an accepted spelling in
    //   prose as well as in the list, the fix is to move that mention out of
    //   quotes, not to relax the count — a quoted token IS the advertisement.
    for (auto const& name : allNames(kWeakDefinitionDialectTable)) {
        EXPECT_EQ(quotedCount(quoted, name), 1u)
            << "the refusal does not name accepted spelling '" << name
            << "' exactly once:\n" << msg;
    }
    // (C) HONESTY — the refusal quotes no vocabulary token the table does not
    //     own. The only other quoted token may be the rejected input itself,
    //     which the message echoes back. A message WIDER than its check sends
    //     an author to write config that is then refused.
    for (auto const& tok : quoted) {
        if (tok == kBadSpelling) continue;
        EXPECT_TRUE(weakDefinitionDialectFromName(tok).has_value())
            << "the refusal advertises '" << tok
            << "', which `weakDefinitionDialectFromName` does not accept:\n"
            << msg;
    }
}

TEST(WeakDefinitionDialect, EmptyDialectStringDoesNotResolveToTheSentinel) {
    // The typo `"dialect": ""` must be refused, not silently resolved to the
    // invalid sentinel. This is the arm that would break if the sentinel were
    // ever listed in the table under an empty name.
    json doc = shippedPeObjectDoc();
    doc["weakDefinition"]["dialect"] = "";
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(), "weakdef-empty");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countAtPath(r, "/weakDefinition/dialect"), 1u) << rejectSummary(r);
}

TEST(WeakDefinitionDialect, BlockWithoutADialectKeyIsRefused) {
    json doc = shippedPeObjectDoc();
    doc["weakDefinition"] = json::object();
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(),
                                                    "weakdef-no-dialect");
    ASSERT_FALSE(r.has_value())
        << "a DECLARED block that names no dialect is neither an answer nor an "
           "omission — it must not load as either";
    EXPECT_EQ(countAtPath(r, "/weakDefinition/dialect"), 1u) << rejectSummary(r);
}

TEST(WeakDefinitionDialect, BlockKeySetIsClosed) {
    // A sibling key inside the block is where a future parameterized dialect
    // will live — which is exactly why an UNRECOGNIZED one must be refused
    // today rather than silently ignored: a dropped key is a capability that
    // quietly does not happen.
    json doc = shippedPeObjectDoc();
    doc["weakDefinition"]["dialect"]  = "comdat";
    doc["weakDefinition"]["dialekt"]  = "comdat";   // the typo
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(),
                                                    "weakdef-typo-key");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countWithMessage(r, "unknown key 'dialekt'"), 1u)
        << rejectSummary(r);

    // …and the `$`-documentation carve-out still applies, or the shipped file's
    // own `$weakDefinitionComment` sibling convention could not be used INSIDE
    // the block by a later author.
    json ok = shippedPeObjectDoc();
    ok["weakDefinition"]["$dialectComment"] = "prose, never config";
    auto const good = ObjectFormatSchema::loadFromText(ok.dump(),
                                                       "weakdef-prose-key");
    EXPECT_TRUE(good.has_value())
        << "a '$'-prefixed key is PROSE and must be accepted:\n"
        << rejectSummary(good);
}

// ═════════════════════════════════════════════════════════════════════════
// 3b. THE LOAD-TIME COHERENCE RULE — a dialect this backend's walker does not
//     write is refused HERE, not at the first weak definition.
//     D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS.
// ═════════════════════════════════════════════════════════════════════════

TEST(WeakDefinitionDialect, MisDeclaredDialectIsRefusedAtLoadNamingTheWalkerThatSpellsIt) {
    // ★ THE WHOLE POINT OF THE ACCESSOR. Before it existed, an ELF document
    // declaring `comdat` loaded CLEAN and the mistake surfaced only when some
    // module finally carried a weak symbol — possibly months later, on another
    // machine, as a refusal that named a file nobody had touched. The
    // `stackReserveControl.vehicle` rule has asked "does anyone implement this,
    // and is it you?" since TF-C125; this is the same question on the other
    // config vocabulary whose rows name an encoder.
    json doc = shippedDoc("elf64-x86_64-linux");
    doc["weakDefinition"]["dialect"] = "comdat";
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(), "weakdef-xkind");
    ASSERT_FALSE(r.has_value())
        << "an ELF document declaring the COFF dialect must be refused at LOAD";
    EXPECT_EQ(countAtPath(r, "/weakDefinition/dialect"), 1u) << rejectSummary(r);

    auto const msg = messageAtPath(r, "/weakDefinition/dialect");
    ASSERT_FALSE(msg.empty()) << rejectSummary(r);
    auto const quoted = quotedTokens(msg);
    // ★ PRESENCE, NOT MULTIPLICITY, and the difference was invisible until the
    //   `std::set` narrowing was undone. These three read `quoted.count(x) == 1`
    //   before, which on a set is 0-or-1 and therefore said only "present". Over
    //   the vector the real count is available — and ✔MEASURED it is THREE for
    //   `'elf'`, because this sentence names the declared kind, then the walker
    //   that would refuse, then the dialect list that walker writes. That
    //   repetition is the sentence doing its job, so the assertion here is
    //   presence and says so, instead of an `== 1` that was only ever true
    //   because the container could not count.
    EXPECT_GE(quotedCount(quoted, "pe"), 1u)
        << "the refusal must NAME the walker that does spell it, or the author "
           "cannot tell whether the dialect or the kind is wrong:\n" << msg;
    EXPECT_GE(quotedCount(quoted, "elf"), 1u)
        << "…and the kind this document actually declares:\n" << msg;
    EXPECT_GE(quotedCount(quoted, "symbol-binding"), 1u)
        << "…and what this backend's walker DOES write, which is the remedy:\n"
        << msg;
}

TEST(WeakDefinitionDialect, TheMirrorDirectionIsRefusedToo) {
    // The rule is symmetric — it is not an "ELF may not say comdat" special
    // case. A pe document declaring the ELF dialect fails identically.
    json doc = shippedPeObjectDoc();
    doc["weakDefinition"]["dialect"] = "symbol-binding";
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(), "weakdef-xkind2");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countAtPath(r, "/weakDefinition/dialect"), 1u) << rejectSummary(r);
    auto const quoted = quotedTokens(messageAtPath(r, "/weakDefinition/dialect"));
    // Presence, for the same reason as the mirror arm above: this sentence
    // names each side more than once by design.
    EXPECT_GE(quotedCount(quoted, "elf"), 1u);
    EXPECT_GE(quotedCount(quoted, "comdat"), 1u);
}

TEST(WeakDefinitionDialect, ABackendThatWritesNoneRefusesTheKeyOutright) {
    // ★ THE TWO BYTECODE BACKENDS ARE REFUSED ONE TIER EARLIER, AND THAT IS
    // STRONGER, NOT WEAKER. ✔MEASURED: `weakDefinition` is already on wasm's
    // and spirv's `rejectedRootFields()` list, so the key is a DEAD ROOT KEY
    // there and the loader refuses it at `/weakDefinition` before the dialect
    // is ever parsed. The coherence rule below it therefore never sees a
    // spirv/wasm document — which is why `backendWeakDialectList`'s "writes no
    // weak definition" rendering is unreachable for both shipped backends, and
    // is documented at its site rather than pinned with a fixture that cannot
    // be built.
    for (char const* name : {"wasm32-v1", "spirv-1.6"}) {
        SCOPED_TRACE(name);
        json doc = shippedDoc(name);
        doc["weakDefinition"]["dialect"] = "symbol-binding";
        auto const r =
            ObjectFormatSchema::loadFromText(doc.dump(),
                                             std::string{name} + "-weakdef");
        ASSERT_FALSE(r.has_value())
            << "a format whose walker spells no dialect must not be able to "
               "declare one";
        EXPECT_EQ(countWithMessage(
                      r, "must not declare a top-level 'weakDefinition'"),
                  1u)
            << rejectSummary(r);
    }
}

TEST(WeakDefinitionDialect, TheCorrectlyDeclaredShippedDocumentStillLoads) {
    // ANTI-SUBSUMPTION for the three arms above: the SAME mutation pipeline
    // with the CORRECT dialect must load clean. Without this, a loader that had
    // simply started rejecting every `weakDefinition` block would satisfy all
    // of them.
    json doc = shippedDoc("elf64-x86_64-linux");
    doc["weakDefinition"]["dialect"] = "symbol-binding";
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(), "weakdef-ok");
    ASSERT_TRUE(r.has_value()) << rejectSummary(r);
    auto const wd = (*r)->weakDefinition();
    ASSERT_TRUE(wd.has_value());
    EXPECT_EQ(wd->dialect, WeakDefinitionDialect::SymbolBinding);
}

TEST(WeakDefinitionDialect, NonObjectBlockIsRefused) {
    // The bare-scalar shape (`"weakDefinition": "comdat"`) is the design that
    // was NOT chosen — see `WeakDefinition` for why a block is the house shape
    // — so it must fail loud rather than being quietly accepted as a synonym.
    json doc = shippedPeObjectDoc();
    doc["weakDefinition"] = "comdat";
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(),
                                                    "weakdef-scalar");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countAtPath(r, "/weakDefinition"), 1u) << rejectSummary(r);
}

// ═════════════════════════════════════════════════════════════════════════
// 4. THE LOADER-BYPASSING PATH — the one the linker and the walkers are
//    actually handed.
// ═════════════════════════════════════════════════════════════════════════

TEST(WeakDefinitionDialect, HandBuiltSchemaWithAnEngagedButEmptyBlockIsRejected) {
    // `ObjectFormatSchema{ObjectFormatData}` is a public constructor that runs
    // NO validation, so every in-memory producer reaches the engine without
    // passing the JSON tier at all. No JSON string can produce the sentinel
    // (it is absent from the table), which makes THIS the only way to reach an
    // engaged block that names no dialect — and the only thing standing
    // between it and a walker is `validate()`.
    dss::detail::ObjectFormatData data;
    data.name               = "synth-weakdef";
    data.backend            = dss::link::objectFormatBackendByConfigName("elf");
    data.dataModel          = DataModel::Lp64;
    data.headerNameMatching = HeaderNameMatching::CaseSensitive;
    data.cSymbolDecoration.scheme = CSymbolDecorationScheme::None;
    data.elf.fileClass      = 2;   // ELFCLASS64
    data.elf.dataEncoding   = 1;   // ELFDATA2LSB
    data.elf.machine        = 62;  // EM_X86_64
    // ENGAGED, and left at the invalid sentinel — the state under test.
    data.weakDefinition     = WeakDefinition{};

    std::size_t atKey = 0;
    for (auto const& d : data.validate()) {
        if (d.path == "/weakDefinition/dialect") ++atKey;
    }
    EXPECT_EQ(atKey, 1u)
        << "an engaged 'weakDefinition' whose dialect names nothing must be "
           "rejected — otherwise a walker asks the schema, gets an engaged "
           "optional, and refuses while naming the empty string";

    // ANTI-SUBSUMPTION: the SAME schema with the block DISENGAGED must draw no
    // diagnostic at that pointer at all. Without this, the arm above could be
    // firing on an unrelated defect in the fixture and would stay green after
    // the rule it pins is deleted.
    data.weakDefinition.reset();
    std::size_t atKeyWhenAbsent = 0;
    for (auto const& d : data.validate()) {
        if (d.path == "/weakDefinition/dialect") ++atKeyWhenAbsent;
    }
    EXPECT_EQ(atKeyWhenAbsent, 0u)
        << "ABSENCE is the absence of an answer, never an error — making the "
           "block required would be the capability shape this row refuses";
}

// ═════════════════════════════════════════════════════════════════════════
// 5. THE WALKERS ACTUALLY READ THE KEY — the half that makes it a
//    declaration rather than documentation.
//    D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS.
// ═════════════════════════════════════════════════════════════════════════
//
// ★★ THE PE HALF OF THIS LIVES IN `test_pe_writer.cpp`, WHERE ITS ENCODER IS.
// The ELF and Mach-O halves live HERE rather than in their sibling writer
// files because the property is the SHARED gate's, not any one walker's: all
// three ask through `link/format/weak_definition_gate.hpp`, and a pin per
// writer file would be three copies of one question — the drift the shared gate
// exists to remove.

namespace {

// A module defining one STRONG and one WEAK function. The weak half is what
// makes the gate ask; the strong half is the control that proves the refusal is
// about weakness and not about "any module at all".
[[nodiscard]] AssembledModule strongPlusWeakModule(std::string_view weakName,
                                                   std::string_view strongName) {
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction st;
    st.symbol = SymbolId{10};
    st.bytes  = {0xC3};
    mod.functions.push_back(std::move(st));
    AssembledFunction wk;
    wk.symbol = SymbolId{11};
    wk.bytes  = {0x90, 0xC3};
    mod.functions.push_back(std::move(wk));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, std::string{strongName},
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{11}, std::string{weakName},
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});
    return mod;
}

// The same module with NO weak row anywhere — the "absence stays free" control.
[[nodiscard]] AssembledModule strongOnlyModule(std::string_view strongName) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction st;
    st.symbol = SymbolId{10};
    st.bytes  = {0xC3};
    mod.functions.push_back(std::move(st));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, std::string{strongName},
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// A shipped document with its `weakDefinition` block DELETED, loaded. This is
// the state every ELF and Mach-O document was in before cycle P28, so it is the
// exact input the gate was built for.
[[nodiscard]] dss::link_format::test::FormatLoadResult
shippedDocMinusWeakDefinition(std::string_view name) {
    json doc = shippedDoc(name);
    doc.erase("weakDefinition");
    return ObjectFormatSchema::loadFromText(doc.dump(),
                                            std::string{name} + "-nodialect");
}

[[nodiscard]] bool sawLacksDialect(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksWeakDefinitionDialect) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string diagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += d.actual + "\n";
    return out;
}

} // namespace

TEST(WeakDefinitionDialect, ElfEncodeRefusesAWeakDefinitionUnderAnUnansweredSchema) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = shippedDocMinusWeakDefinition("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value()) << rejectSummary(fmt);
    ASSERT_FALSE((*fmt)->weakDefinition().has_value())
        << "the fixture must actually be missing the block, or this arm tests "
           "nothing";

    AssembledModule mod = strongPlusWeakModule("weakfn", "strongfn");
    DiagnosticReporter rep;
    auto const bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "the encode must STOP — emitting STB_WEAK under a schema that never "
           "said ELF spells a weak definition that way is the unread-key defect";
    EXPECT_TRUE(sawLacksDialect(rep)) << diagText(rep);
    EXPECT_NE(diagText(rep).find("D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED"),
              std::string::npos)
        << diagText(rep);
    EXPECT_NE(diagText(rep).find("symbol-binding"), std::string::npos)
        << "the refusal must name the dialect this walker WANTS declared, which "
           "is the remedy:\n" << diagText(rep);
}

TEST(WeakDefinitionDialect, MachOEncodeRefusesAWeakDefinitionUnderAnUnansweredSchema) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = shippedDocMinusWeakDefinition("macho64-x86_64-darwin");
    ASSERT_TRUE(fmt.has_value()) << rejectSummary(fmt);
    ASSERT_FALSE((*fmt)->weakDefinition().has_value());

    AssembledModule mod = strongPlusWeakModule("_weakfn", "_strongfn");
    DiagnosticReporter rep;
    auto const bytes = macho::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "the encode must STOP — emitting N_WEAK_DEF under a schema that "
           "never said Mach-O spells a weak definition that way is the "
           "unread-key defect";
    EXPECT_TRUE(sawLacksDialect(rep)) << diagText(rep);
    EXPECT_NE(diagText(rep).find("symbol-flag"), std::string::npos)
        << diagText(rep);
}

TEST(WeakDefinitionDialect, AbsenceStaysFreeWhenNoWeakDefinitionIsPresent) {
    // ★★ THE ARM THAT KEEPS THE KEY FROM BECOMING REQUIRED, and it is the one
    // the 2026-08-20 ruling turns on: the obvious implementation (ask the
    // schema up front) makes `weakDefinition` a back-door mandatory key on
    // every format. THE SAME unanswered schema that refuses above must encode
    // GREEN here, because this module carries no weak definition to spell.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto elfFmt = shippedDocMinusWeakDefinition("elf64-x86_64-linux");
    ASSERT_TRUE(elfFmt.has_value()) << rejectSummary(elfFmt);

    AssembledModule elfMod = strongOnlyModule("strongfn");
    DiagnosticReporter elfRep;
    auto const elfBytes = elf::encode(elfMod, **target, **elfFmt, elfRep);
    EXPECT_EQ(elfRep.errorCount(), 0u) << diagText(elfRep);
    EXPECT_FALSE(elfBytes.empty());

    auto machoFmt = shippedDocMinusWeakDefinition("macho64-x86_64-darwin");
    ASSERT_TRUE(machoFmt.has_value()) << rejectSummary(machoFmt);
    AssembledModule machoMod = strongOnlyModule("_strongfn");
    DiagnosticReporter machoRep;
    auto const machoBytes =
        macho::encode(machoMod, **target, **machoFmt, machoRep);
    EXPECT_EQ(machoRep.errorCount(), 0u) << diagText(machoRep);
    EXPECT_FALSE(machoBytes.empty());
}

TEST(WeakDefinitionDialect, TheShippedSchemasEncodeAWeakDefinitionGreen) {
    // ANTI-SUBSUMPTION for every arm above: with the SHIPPED documents — the
    // ones this change added the key to — the identical weak module encodes
    // clean. A gate that had started refusing everything would satisfy the
    // refusal arms and fail here.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    auto elfFmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(elfFmt.has_value()) << rejectSummary(elfFmt);
    AssembledModule elfMod = strongPlusWeakModule("weakfn", "strongfn");
    DiagnosticReporter elfRep;
    auto const elfBytes = elf::encode(elfMod, **target, **elfFmt, elfRep);
    EXPECT_EQ(elfRep.errorCount(), 0u) << diagText(elfRep);
    EXPECT_FALSE(elfBytes.empty());

    auto machoFmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(machoFmt.has_value()) << rejectSummary(machoFmt);
    AssembledModule machoMod = strongPlusWeakModule("_weakfn", "_strongfn");
    DiagnosticReporter machoRep;
    auto const machoBytes =
        macho::encode(machoMod, **target, **machoFmt, machoRep);
    EXPECT_EQ(machoRep.errorCount(), 0u) << diagText(machoRep);
    EXPECT_FALSE(machoBytes.empty());
}

TEST(WeakDefinitionDialect, TheGateRefusesADialectTheWalkerCannotSpell) {
    // ★ THE SECOND REFUSAL ARM, EXERCISED RATHER THAN READ. It is no longer
    // reachable through the JSON tier — the loader's coherence rule refuses a
    // cross-backend declaration first (`MisDeclaredDialectIsRefusedAtLoad…`) —
    // so it is reached the way a real caller would: a schema that DID load,
    // handed to a walker that spells a different dialect. That is exactly the
    // in-memory `ObjectFormatSchema{ObjectFormatData}` path, which runs no
    // validation at all, and it is why the walker-side arm was not deleted when
    // the load-time rule landed.
    auto fmt = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(fmt.has_value()) << rejectSummary(fmt);
    ASSERT_EQ((*fmt)->weakDefinition()->dialect, WeakDefinitionDialect::Comdat);

    AssembledModule mod = strongPlusWeakModule("weakfn", "strongfn");
    DiagnosticReporter rep;
    // Ask as the ELF walker would.
    EXPECT_FALSE(dss::link::format::requireWeakDefinitionDialect(
        mod, **fmt, WeakDefinitionDialect::SymbolBinding, "probe::encode", rep));
    EXPECT_TRUE(sawLacksDialect(rep)) << diagText(rep);
    EXPECT_NE(diagText(rep).find("comdat"), std::string::npos) << diagText(rep);
    EXPECT_NE(diagText(rep).find("symbol-binding"), std::string::npos)
        << diagText(rep);

    // …and the SAME schema asked by the walker that DOES spell `comdat` passes.
    DiagnosticReporter ok;
    EXPECT_TRUE(dss::link::format::requireWeakDefinitionDialect(
        mod, **fmt, WeakDefinitionDialect::Comdat, "probe::encode", ok));
    EXPECT_EQ(ok.errorCount(), 0u) << diagText(ok);
}

TEST(WeakDefinitionDialect, TheGateSeesAWeakALIASOfAStrongDefinition) {
    // ⚠ THE SHAPE A CANONICAL-ONLY SCAN MISSES, and it is the shape gcc emits
    // for `__attribute__((weak, alias("strong_fn")))`: ONE SymbolId with TWO
    // rows, the first Global and the second Weak. `definedBinding` is
    // first-row-wins, so the canonical reads Global — and the writer still puts
    // STB_WEAK on the wire through the alias pass. A gate that asked only about
    // canonicals would let this module past unasked.
    auto fmt = shippedDocMinusWeakDefinition("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value()) << rejectSummary(fmt);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction f;
    f.symbol = SymbolId{10};
    f.bytes  = {0x90, 0x90, 0xC3};
    mod.functions.push_back(std::move(f));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "strongfn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "weakalias",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    EXPECT_TRUE(dss::link::format::moduleDefinesWeakSymbol(mod))
        << "a WEAK ALIAS of a STRONG definition is still a weak definition on "
           "the wire";
    EXPECT_FALSE(dss::link::format::requireWeakDefinitionDialect(
        mod, **fmt, WeakDefinitionDialect::SymbolBinding, "probe::encode", rep));
    EXPECT_TRUE(sawLacksDialect(rep)) << diagText(rep);
}
