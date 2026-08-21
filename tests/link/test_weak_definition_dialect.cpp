// ── D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED ────────────────────────
//    The CONFIG half: the `weakDefinition` block's vocabulary, its loader, and
//    the JSON-free `validate()` twin. The WRITER half — the pins that prove the
//    COFF `.obj` walker actually READS the key — lives in `test_pe_writer.cpp`,
//    because that is where the encoder is.
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

#include "core/types/enum_name_table.hpp"
#include "core/types/object_format_kind.hpp"
#include "link/object_format_backend.hpp"
#include "link/object_format_schema.hpp"

#include "format_reject_support.hpp"
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

// The shipped formats whose WALKER consults `weakDefinition` today: the two
// pe object-flavored documents, both served by the COFF `.obj` arm of
// `pe::encode`. Written out rather than derived, and that is the teeth — see
// `ShippedDeclarationsAreOnlyWhereAWriterConsultsThem`.
[[nodiscard]] std::set<std::string> const& consultingFormats() {
    static std::set<std::string> const kNames{
        "pe64-x86_64-windows",
        "pe64-x86_64-windows-staticlib",
    };
    return kNames;
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
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return out;
    }
    auto const dir = *root / "src" / "dss-config" / "object-formats";
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

// The shipped pe OBJECT document, as JSON — the one fixture every negative arm
// mutates, so no arm can rot against a hand-written format-like blob that the
// real file has since outgrown.
[[nodiscard]] json shippedPeObjectDoc() {
    for (auto& f : shippedFormatDocs()) {
        if (f.name == "pe64-x86_64-windows") return std::move(f.doc);
    }
    ADD_FAILURE() << "pe64-x86_64-windows.format.json not found";
    return json::object();
}

// Every `'…'`-quoted token in a diagnostic message. The shared renderer quotes
// each vocabulary spelling, so this is how a test reads back WHAT THE MESSAGE
// CLAIMS — as opposed to re-deriving it from the table, which would make the
// comparison circular and green by construction.
[[nodiscard]] std::set<std::string> quotedTokens(std::string const& msg) {
    std::set<std::string> out;
    std::size_t i = 0;
    while (true) {
        auto const open = msg.find('\'', i);
        if (open == std::string::npos) break;
        auto const close = msg.find('\'', open + 1);
        if (close == std::string::npos) break;
        out.insert(msg.substr(open + 1, close - open - 1));
        i = close + 1;
    }
    return out;
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
    // not READ drifts silently while reading as authoritative. Exactly ONE
    // walker consults `weakDefinition` today — the COFF `.obj` arm of
    // `pe::encode` — so exactly the formats it serves may declare it.
    //
    // ⚠ WHEN THE ELF OR MACH-O WRITER STARTS CONSULTING ITS OWN DECLARATION
    // ([[D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS]]),
    // THIS SET GROWS IN THE SAME CHANGE — never before it. The failure message
    // says so, because the next author to trip this will be exactly the author
    // who is halfway through that work.
    auto const formats = shippedFormatDocs();
    ASSERT_FALSE(formats.empty());
    // The corpus is real: if the enumeration ever silently returned two files,
    // every arm below would be vacuous.
    EXPECT_GE(formats.size(), 20u)
        << "the shipped object-format corpus should be ~24 documents";

    std::set<std::string> declaring;
    for (auto const& f : formats) {
        if (!f.doc.contains("weakDefinition")) continue;
        declaring.insert(f.name);
        // Every declaration must name the dialect its consulting walker
        // encodes. A pe object document declaring `symbol-binding` would be a
        // key the COFF writer refuses — which is loud, but the corpus should
        // never contain one.
        auto const loaded = ObjectFormatSchema::loadShipped(f.name);
        ASSERT_TRUE(loaded.has_value())
            << f.name << " no longer loads: " << rejectSummary(loaded);
        auto const wd = (*loaded)->weakDefinition();
        ASSERT_TRUE(wd.has_value())
            << f.name << " declares 'weakDefinition' in JSON but the schema "
                         "carries none — the loader dropped it";
        EXPECT_EQ(wd->dialect, WeakDefinitionDialect::Comdat)
            << f.name << " declares dialect '"
            << weakDefinitionDialectName(wd->dialect)
            << "', which no shipped walker consults";
    }
    EXPECT_EQ(declaring, consultingFormats())
        << "the set of shipped documents declaring 'weakDefinition' must equal "
           "the set whose walker CONSULTS it. A document in the left set only "
           "carries a key nobody reads; a format in the right set only has a "
           "walker that will refuse the first weak definition it meets.";
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
    for (auto const& name : allNames(kWeakDefinitionDialectTable)) {
        EXPECT_TRUE(quoted.count(std::string{name}) == 1)
            << "the refusal does not name accepted spelling '" << name
            << "':\n" << msg;
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
