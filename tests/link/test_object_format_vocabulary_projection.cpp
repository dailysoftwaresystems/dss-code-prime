// ── D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET ──────────
//    The object-format tier's half: the loader, its JSON-free validate() twin,
//    and the five format backends.
// ─────────────────────────────────────────────────────────────────────────
//
// THE CLASS. A config site whose value must name a member of a closed
// vocabulary resolves it through a `…FromName` lookup and reports the miss —
// and the report used to state the accepted set as a STRING LITERAL typed
// beside the check. That is a second owner of a fact the vocabulary table
// already owns, and the two drift in SILENCE: the rows keep working while the
// sentence becomes a lie, and the sentence is the half a config author reads.
// A test asserting the sentence CONTAINS some name stays green the whole way.
//
// ★★★ IT WAS NOT A HAZARD HERE — IT HAD ALREADY FIRED, TWICE, IN THE SHIPPED
// TREE. Both measured 2026-08-20, before either was touched:
//
//   (1) `/sections/{}/kind`. `kSectionKindTable` has SIXTEEN rows and
//       `sectionKindFromName` accepts every one; the refusal advertised
//       ELEVEN. The five it denied by name — `shstrtab`, `relro`, `tdata`,
//       `tbss`, `tvars` — are not theoretical: a walk of the 24 shipped
//       `.format.json` documents counts `shstrtab` on 10 section rows,
//       `relro` on 10, `tdata` on 5, `tbss` on 4 and `tvars` on 2. The loader
//       took them, the corpus used them, and the diagnostic told the author
//       they were not allowed.
//
//   (2) `/elf/osabi`. The accepting if-chain took SEVEN spellings (`sysv`,
//       `none`, `hpux`, `netbsd`, `gnu`, `linux`, `freebsd`) and the refusal
//       advertised SIX — `linux` was accepted and denied at once. That one is
//       an ALIAS of `gnu` in the psABI, which is exactly why nobody noticed:
//       the format that used it kept loading.
//
// ★★ WHAT THESE PINS ASSERT, AND WHY IT TAKES BOTH DIRECTIONS. The claim is
// "the message and the check name the SAME set", and neither half carries it
// alone:
//   (A) CORPUS EXERCISE — the shipped documents really USE these vocabularies.
//       Without it a table mutant could delete a row nothing reads and the
//       whole red-on-disable would be theatre.
//   (B) COMPLETENESS — every spelling the table accepts appears in the
//       refusal. This is the direction both live defects were on, and the only
//       one that catches them.
//   (C) HONESTY — the refusal quotes no vocabulary token the table does not
//       own. Catches the mirror defect: a message WIDER than its check, which
//       sends an author to write config that is then refused.
//
// ★★ AND THE MUTANT IS THE TABLE, NOT THE MESSAGE — that is the entire point
// of the change these pins guard. Before it, deleting a name from a message
// reddened nothing anywhere in this tree. Now the message is rendered from the
// table, so (B) is satisfied by construction and what these pins actually
// protect is that the rendering STAYS: retype any one of them back into a
// literal and (B) goes red the next time a row is added, while (A) reds
// immediately if the row a shipped document needs disappears.
//
// ⚠ MUST run through ctest, never a bare `.exe`: the shipped-config resolver
// walks the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets
// it. A bare binary reads whichever config tree the shell happens to stand in.

#include "core/types/data_model.hpp"
#include "core/types/entry_shape.hpp"
#include "core/types/enum_name_table.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
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
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using json = nlohmann::json;
using dss::link_format::test::rejectSummary;

namespace {

// A spelling no vocabulary in this tree claims. Deliberately ugly: a probe
// that collided with a real name would make every negative arm pass for the
// wrong reason.
constexpr char const* kBadSpelling = "zzNotAnyFormatVocabularySpelling";

[[nodiscard]] fs::path objectFormatsDir() {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *root / "object-formats";
}

struct ShippedFormatDoc {
    std::string name;
    json        doc;
};

// Enumerated FROM DISK, never from a hard-coded list — a format added tomorrow
// is probed the day it lands.
[[nodiscard]] std::vector<ShippedFormatDoc> shippedFormatDocs() {
    std::vector<ShippedFormatDoc> out;
    auto const dir = objectFormatsDir();
    if (dir.empty()) return out;
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

// ★ `quotedTokens` used to be a file-local copy here, byte-identical to the one
// three `tests/core` files had already merged — and therefore invisible to the
// mutant that closed
// D-TEST-VOCABULARY-PROJECTION-PROBE-HELPERS-ARE-COPIED-PER-FILE. It has ONE
// owner now, `tests/test_support/vocabulary_message_probe.hpp`, which is
// json-free and reachable from every suite; see
// D-TEST-VOCABULARY-PROBE-MESSAGE-HALF-IS-UNREACHABLE-AND-JSON-COUPLED.
using ::dss::test_support::quotedTokens;

[[nodiscard]] bool contains(std::span<std::string_view const> names,
                            std::string_view                  needle) {
    for (auto const n : names) {
        if (n == needle) return true;
    }
    return false;
}

// (B) COMPLETENESS — SOME error diagnostic of the failed load names every
// spelling the table owns, and that diagnostic is the one (C) then reads.
//
// ⚠ THE SEARCH IS THE ASSERTION, and it has to be. A refused load emits more
// than the vocabulary refusal — a rejected `kind` value is not stored, so
// downstream "required"/"congruent" rules then fire on the zero default, each
// with its own quoted tokens. Running the honesty check over the UNION of
// every message reports those as "advertised tokens", which is the pin lying
// rather than the loader. The vocabulary sentence is the one that names the
// whole set.
[[nodiscard]] std::string const*
findVocabularyMessage(dss::link_format::test::FormatLoadResult const& r,
                      std::span<std::string_view const>               names) {
    if (r.has_value()) return nullptr;
    for (auto const& d : r.error()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        auto const quoted = quotedTokens(d.message);
        bool       all    = true;
        for (auto const n : names) {
            if (std::find(quoted.begin(), quoted.end(), std::string{n})
                == quoted.end()) {
                all = false;
                break;
            }
        }
        if (all) return &d.message;
    }
    return nullptr;
}

// One probe: overwrite `pointer` in a shipped document with an unresolvable
// spelling, and demand the refusal name the whole accepted set.
struct VocabularySite {
    char const*                       format;   // shipped document to base on
    char const*                       pointer;  // JSON pointer to overwrite
    std::span<std::string_view const> names;    // the PROJECTED accepted set
    char const*                       label;
    // Non-vocabulary quotes the sentence legitimately carries besides the
    // offending spelling (a JSON field name it points at, a prose aside).
    std::vector<char const*>          ignoreQuoted;
};

void runVocabularySite(VocabularySite const& site) {
    SCOPED_TRACE(std::string{site.label} + " @ " + site.format + site.pointer);

    // The document is the SHIPPED file, mutated in memory — never a
    // hand-written "format-like" fixture. A hand-written one proves the rule
    // fires on the fixture; only the real file proves it fires on what we ship.
    json doc;
    for (auto& f : shippedFormatDocs()) {
        if (f.name == site.format) { doc = f.doc; break; }
    }
    ASSERT_FALSE(doc.is_null())
        << "no shipped format document named '" << site.format << "'";

    json::json_pointer const ptr{std::string{site.pointer}};
    // ⚠ THE PARENT, not the key itself. Several probed keys are OPTIONAL in
    // the shipped document (`elf.type` and `pe.type` both default), and a
    // config author supplying one is exactly the case under test — so the
    // probe SUPPLIES it. What must not go stale is the block it lives in: a
    // pointer whose parent stopped resolving would have the probe writing a
    // brand-new subtree that nothing reads, and the pin would be green over
    // anything.
    auto const parent = ptr.parent_pointer();
    ASSERT_TRUE(parent.empty() || doc.contains(parent))
        << site.label << ": the JSON pointer '" << site.pointer
        << "' has no parent block in the shipped document '" << site.format
        << "'. Re-aim it at the node it names; a stale pointer asserts NOTHING.";

    doc[ptr] = kBadSpelling;
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(),
                                                    "vocab-probe");
    ASSERT_FALSE(r.has_value())
        << site.label
        << ": an unresolvable spelling must be REFUSED at load — a typo that "
           "loads clean leaves the capability it names switched off with no "
           "diagnostic";

    // (B) COMPLETENESS.
    auto const* msg = findVocabularyMessage(r, site.names);
    if (msg == nullptr) {
        ADD_FAILURE()
            << site.label
            << ": NO diagnostic of the refused load names every spelling the "
               "vocabulary accepts. A message NARROWER than its check tells a "
               "config author, by name, that a spelling the loader takes is "
               "not allowed — measured live TWICE in this tier before these "
               "pins existed (SectionKind 11-of-16, elf osabi 6-of-7). Render "
               "the list through `dss::detail::renderAllowedList` over the "
               "table's projection, never as a literal.\n"
            << rejectSummary(r);
        return;
    }

    // (C) HONESTY.
    for (auto const& q : quotedTokens(*msg)) {
        if (contains(site.names, q)) continue;
        if (q == kBadSpelling) continue;
        bool ignored = false;
        for (char const* g : site.ignoreQuoted) {
            if (q == g) { ignored = true; break; }
        }
        EXPECT_TRUE(ignored)
            << site.label << ": the refusal quotes '" << q
            << "', which is neither a spelling the vocabulary accepts nor a "
               "declared non-vocabulary quote for this site. A message WIDER "
               "than its check sends an author to write config that will then "
               "be refused.\nmessage was:\n"
            << *msg;
    }
}

// The vocabularies whose accepted set is "the table MINUS a sentinel the
// loader refuses explicitly". Computed, never hand-listed.
//
// ★★ THESE ARE RE-DERIVED HERE ON PURPOSE, AND THAT IS THE PIN. `src/` now owns
// ONE definition of each (`kSelectableExitMechanismNames` and
// `kSelectableArgsMechanismNames` beside their enums in
// `core/types/target_schema.hpp`, `kSelectableRuntimeLibraryRoleNames` in
// `core/types/object_format_kind.hpp`) and the loader RENDERS those arrays into
// its refusals. Expecting the loader's own array back out of its own message
// would assert only that `allowedList` round-trips. Re-deriving from the TABLE
// with this file's own predicate keeps the expectation independent of the
// array under test, so a canonical projection that is itself wrongly filtered
// still reds here. The table stays the single owner of the SPELLINGS; what is
// duplicated is the derivation, which is the whole point of a pin.
//
// ⚠⚠ EACH `M` IS A LITERAL AND EACH CARRIES A SENTINEL-COUNT `static_assert`.
// All three were spelled `rows.size() - 1` until this cycle, and that spelling
// is `x == x`: `namesWhere<M>` compares `M` against the rows the predicate
// ACCEPTS, so deriving `M` from the table the predicate walks makes both sides
// move together (D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY).
// ✔MEASURED with `g++ -std=c++23 -fsyntax-only` over a nine-arm probe: a new
// SELECTABLE enumerator reds only under the literal, a second UNSELECTABLE row
// reds only under the derived form — so neither spelling alone is the guard the
// comment that stood here claimed, and both parts are written.
[[nodiscard]] constexpr bool selectableRole(RuntimeLibraryRole r) noexcept {
    return r != RuntimeLibraryRole::None;
}
constexpr auto kSelectableRoleNames =
    namesWhere<3>(kRuntimeLibraryRoleTable, selectableRole);
static_assert(kRuntimeLibraryRoleTable.rows.size()
                  == kSelectableRoleNames.size() + 1,
              "exactly one runtimeLibraries role is unselectable (the 'none' "
              "sentinel) — a second one leaves this expectation silently "
              "narrower than the set the loader accepts");
[[nodiscard]] constexpr bool selectableExit(ExitMechanism m) noexcept {
    return m != ExitMechanism::None;
}
constexpr auto kSelectableExitNames =
    namesWhere<2>(kExitMechanismTable, selectableExit);
static_assert(kExitMechanismTable.rows.size() == kSelectableExitNames.size() + 1,
              "exactly one processExit mechanism is unselectable (the 'none' "
              "sentinel) — a second one leaves this expectation silently "
              "narrower than the set the loader accepts");
[[nodiscard]] constexpr bool selectableArgs(ArgsMechanism m) noexcept {
    return m != ArgsMechanism::None;
}
constexpr auto kSelectableArgsNames =
    namesWhere<2>(kArgsMechanismTable, selectableArgs);
static_assert(kArgsMechanismTable.rows.size() == kSelectableArgsNames.size() + 1,
              "exactly one processArgs mechanism is unselectable (the 'none' "
              "sentinel) — a second one leaves this expectation silently "
              "narrower than the set the loader accepts");

constexpr auto kDataModelNames        = allNames(kDataModelTable);
constexpr auto kHeaderMatchNames      = allNames(kHeaderNameMatchingTable);
constexpr auto kDecorationNames       = allNames(kCSymbolDecorationSchemeTable);
constexpr auto kSectionKindNames      = allNames(kSectionKindTable);
constexpr auto kEntryVerbNames        = allNames(kEntryMaterializationTable);
constexpr auto kTlsModelNames         = allNames(kTlsAccessModelTable);
constexpr auto kDispatchNames         = allNames(kExternCallDispatchTable);
constexpr auto kElfTypeNames          = allNames(kElfObjectTypeTable);
constexpr auto kPeTypeNames           = allNames(kPeObjectTypeTable);
constexpr auto kMachoTypeNames        = allNames(kMachOObjectTypeTable);

}  // namespace

// ── BASELINE ────────────────────────────────────────────────────────────
//
// The positive control for everything below. Every probe mutates ONE value of
// a document that must otherwise be clean; if the baseline were already
// refused, every "it was refused" assertion below would pass for free.
TEST(ObjectFormatVocabularyProjection, EveryShippedFormatLoadsCleanly) {
    auto const docs = shippedFormatDocs();
    ASSERT_GE(docs.size(), 20u)
        << "the shipped object-format corpus went missing or shrank "
           "unexpectedly — every probe below is based on one of these files";
    for (auto const& f : docs) {
        auto const r = ObjectFormatSchema::loadFromText(f.doc.dump(), f.name);
        EXPECT_TRUE(r.has_value())
            << f.name << " must load clean before any mutant means anything: "
            << rejectSummary(r);
    }
}

// ── (A) CORPUS EXERCISE ─────────────────────────────────────────────────
//
// The shipped corpus really declares these section kinds, and every spelling
// it declares is one `kSectionKindTable` owns. This is what makes the table
// mutant meaningful: delete `{SectionKind::RelRoConst, "relro"}` and TEN
// shipped section rows stop resolving, so the baseline above reds immediately.
//
// ★ It is also the measurement that turned the `/sections/{}/kind` drift from
// "a stale sentence" into "a live defect": five of the spellings the message
// omitted are spellings the SHIPPED FILES USE.
TEST(ObjectFormatVocabularyProjection, ShippedCorpusExercisesTheSectionKindSet) {
    std::map<std::string, std::size_t> declared;
    for (auto const& f : shippedFormatDocs()) {
        if (!f.doc.contains("sections")) continue;
        for (auto const& s : f.doc.at("sections")) {
            if (!s.is_object() || !s.contains("kind")) continue;
            if (!s.at("kind").is_string()) continue;
            ++declared[s.at("kind").get<std::string>()];
        }
    }
    EXPECT_GE(declared.size(), 10u)
        << "the shipped corpus declares too few distinct section kinds for a "
           "table mutant to be meaningful";
    for (auto const& [spelling, count] : declared) {
        EXPECT_TRUE(sectionKindFromName(spelling).has_value())
            << "the SHIPPED corpus declares section kind '" << spelling
            << "' (" << count
            << " rows) which `kSectionKindTable` does not own — the corpus and "
               "the vocabulary disagree";
    }
    // The five spellings the pre-fix message denied by name. Named here, in a
    // test, because their ABSENCE from the sentence was the defect and their
    // presence in the corpus is what made it live rather than latent.
    for (char const* used : {"shstrtab", "relro", "tdata", "tbss", "tvars"}) {
        EXPECT_GT(declared[used], 0u)
            << "no shipped format declares a '" << used
            << "' section any more — re-derive this list from the corpus "
               "rather than deleting the assertion, or the drift measurement "
               "this pin records loses its witness";
    }
}

// ── THE SITE TABLE ──────────────────────────────────────────────────────
TEST(ObjectFormatVocabularyProjection, EveryRefusalNamesEverySpellingItsCheckTakes) {
    std::vector<VocabularySite> const sites{
        {"elf64-x86_64-linux", "/dataModel", kDataModelNames,
         "dataModel", {"dataModel"}},
        {"elf64-x86_64-linux", "/headerNameMatching", kHeaderMatchNames,
         "headerNameMatching", {"headerNameMatching"}},
        {"elf64-x86_64-linux", "/cSymbolDecoration/scheme", kDecorationNames,
         "cSymbolDecoration.scheme", {"cSymbolDecoration", "scheme"}},
        // THE DRIFTED ONE. `/sections/0/kind` is a `text` row on every shipped
        // format, so this probe is the same shape everywhere.
        {"elf64-x86_64-linux", "/sections/0/kind", kSectionKindNames,
         "sections[].kind", {"kind"}},
        {"elf64-x86_64-linux-exec", "/processExit/mechanism", kSelectableExitNames,
         "processExit.mechanism", {"processExit.mechanism"}},
        {"elf64-x86_64-linux-exec", "/processArgs/mechanism", kSelectableArgsNames,
         "processArgs.mechanism", {"processArgs.mechanism"}},
        {"elf64-x86_64-linux-exec", "/entryVerbs/0", kEntryVerbNames,
         "entryVerbs[]", {"entryVerbs", "returns", "params"}},
        {"elf64-x86_64-linux", "/elf/type", kElfTypeNames, "elf.type",
         {"type"}},
        {"pe64-x86_64-windows", "/pe/type", kPeTypeNames, "pe.type", {"type"}},
        {"macho64-arm64-darwin", "/macho/filetype", kMachoTypeNames,
         "macho.filetype", {"filetype"}},
    };
    for (auto const& s : sites) runVocabularySite(s);
}

// Vocabularies whose site is an OPTIONAL block the shipped corpus declares on
// only some formats — probed on the format that has it.
TEST(ObjectFormatVocabularyProjection, OptionalBlockRefusalsNameTheirWholeSet) {
    runVocabularySite({"elf64-x86_64-linux-exec", "/externCallDispatch",
                       kDispatchNames, "externCallDispatch",
                       {"externCallDispatch"}});
    runVocabularySite({"elf64-x86_64-linux-exec", "/tlsAccess/model",
                       kTlsModelNames, "tlsAccess.model",
                       {"tlsAccess.model", "model", "segmentPrefixByte",
                        "baseDisplacement", "tlsIndexSlotName"}});
    runVocabularySite({"pe64-x86_64-windows-exec", "/runtimeLibraries/0/role",
                       kSelectableRoleNames, "runtimeLibraries[].role",
                       {"role", "image", "runtimeLibraries"}});
}

// ── THE FORMAT-KIND SET HAS A SECOND OWNER, AND NOTHING PINNED IT ───────
//
// `objectFormatBackendByConfigName` resolves `format.kind` by walking the
// BACKEND REGISTRY and asking each entry for its own `configName()`. The enum
// projection `kSelectableObjectFormatKindNames` is a DIFFERENT owner of the
// same fact, and ✔MEASURED 2026-08-20 by reading
// `test_object_format_backend_registry.cpp`: it pins that every registered name
// is non-empty, unique, and resolves back to its own entry — and never that the
// registered names ARE the selectable enum spellings.
//
// This pin closes that gap from the side that matters to a config author: the
// refusal must name every spelling a backend actually answers to. It is driven
// from `objectFormatBackendTable()`, so a sixth backend whose name never
// reaches the sentence reds here rather than shipping a message that omits it.
TEST(ObjectFormatVocabularyProjection, FormatKindRefusalNamesEveryRegisteredBackend) {
    std::vector<std::string> registered;
    for (auto const* b : link::objectFormatBackendTable()) {
        registered.emplace_back(b->configName());
    }
    ASSERT_GE(registered.size(), 2u)
        << "a one-entry registry cannot witness a set";

    json doc;
    for (auto& f : shippedFormatDocs()) {
        if (f.name == "elf64-x86_64-linux") { doc = f.doc; break; }
    }
    ASSERT_FALSE(doc.is_null());
    doc["format"]["kind"] = kBadSpelling;
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(), "kind-probe");
    ASSERT_FALSE(r.has_value()) << "an unresolvable format kind must be REFUSED";

    std::vector<std::string_view> names;
    for (auto const& s : registered) names.emplace_back(s);
    auto const* msg = findVocabularyMessage(r, names);
    ASSERT_NE(msg, nullptr)
        << "the `format.kind` refusal does not name every REGISTERED backend. "
           "The registry is the half that decides — rendering the ENUM "
           "projection instead would be correct only by coincidence, and "
           "nothing in this tree pins the two together.\n"
        << rejectSummary(r);
    for (auto const& q : quotedTokens(*msg)) {
        if (q == kBadSpelling) continue;
        if (q == "kind") continue;
        EXPECT_NE(std::find(registered.begin(), registered.end(), q),
                  registered.end())
            << "the `format.kind` refusal advertises '" << q
            << "', which no registered backend answers to — a spelling the "
               "author would be sent to write and the loader would then "
               "refuse.\nmessage was:\n"
            << *msg;
    }

    // The sentinel arm earns its own message and must name the same set: a
    // `"kind": "unknown"` is a DIFFERENT author mistake from `"kind": "elff"`,
    // and both remedies point at the same list.
    doc["format"]["kind"] = std::string{kObjectFormatKindSentinelName};
    auto const rs = ObjectFormatSchema::loadFromText(doc.dump(),
                                                     "sentinel-probe");
    ASSERT_FALSE(rs.has_value())
        << "the reserved `unknown` sentinel must be REFUSED as a format kind";
    EXPECT_NE(findVocabularyMessage(rs, names), nullptr)
        << "the sentinel refusal does not name every registered backend\n"
        << rejectSummary(rs);
}

// ── THE ELF HEADER VERBS — the second live drift ────────────────────────
//
// `class`, `data` and `osabi` map a spelling straight onto the psABI's numeric
// field, so their vocabulary is a ROW TABLE private to the ELF backend rather
// than an `EnumNameTable` this test can project. Completeness is therefore by
// CONSTRUCTION (one array feeds both the lookup and the message) and what this
// pin adds is the property that construction cannot give: that the ALIASES the
// psABI requires are still BOTH accepted and BOTH advertised.
//
// ✔That is exactly the defect that was live: `linux` — ELFOSABI_GNU's second
// spelling — was accepted and unadvertised. An alias is the shape this class
// hides in, because the format that uses the OTHER spelling keeps loading.
TEST(ObjectFormatVocabularyProjection, ElfOsAbiAliasesAreBothTakenAndBothNamed) {
    json base;
    for (auto& f : shippedFormatDocs()) {
        if (f.name == "elf64-x86_64-linux") { base = f.doc; break; }
    }
    ASSERT_FALSE(base.is_null());
    ASSERT_TRUE(base.contains("elf")) << "the probe needs an `elf` block";

    // Both halves of each psABI alias pair must resolve, and to the SAME wire
    // byte — the property that makes them aliases rather than two values.
    auto osabiOf = [&](char const* spelling) -> std::optional<std::uint8_t> {
        json doc      = base;
        doc["elf"]["osabi"] = spelling;
        auto const r  = ObjectFormatSchema::loadFromText(doc.dump(),
                                                         "osabi-probe");
        if (!r.has_value()) {
            ADD_FAILURE() << "osabi '" << spelling
                          << "' must LOAD — it is a psABI alias the reader "
                             "accepts:\n"
                          << rejectSummary(r);
            return std::nullopt;
        }
        return (*r)->elf().osabi;
    };
    auto const sysv  = osabiOf("sysv");
    auto const none  = osabiOf("none");
    auto const gnu   = osabiOf("gnu");
    auto const linux_ = osabiOf("linux");
    ASSERT_TRUE(sysv.has_value() && none.has_value());
    ASSERT_TRUE(gnu.has_value() && linux_.has_value());
    EXPECT_EQ(*sysv, *none)
        << "'sysv' and 'none' are ELFOSABI_NONE's two spellings and must map "
           "to the same wire byte";
    EXPECT_EQ(*gnu, *linux_)
        << "'gnu' and 'linux' are ELFOSABI_GNU's two spellings and must map to "
           "the same wire byte";

    // …and the refusal must ADVERTISE every one of them. This is the half that
    // was wrong: `linux` was accepted above and missing from the sentence.
    json bad            = base;
    bad["elf"]["osabi"] = kBadSpelling;
    auto const r = ObjectFormatSchema::loadFromText(bad.dump(), "osabi-reject");
    ASSERT_FALSE(r.has_value()) << "an unknown osabi must be REFUSED";
    std::string const* msg = nullptr;
    for (auto const& d : r.error()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        if (d.path.find("/elf/osabi") == std::string::npos) continue;
        msg = &d.message;
        break;
    }
    ASSERT_NE(msg, nullptr) << "no diagnostic at /elf/osabi\n" << rejectSummary(r);
    auto const advertised = quotedTokens(*msg);
    for (char const* spelling : {"sysv", "none", "hpux", "netbsd", "gnu",
                                 "linux", "freebsd"}) {
        EXPECT_NE(std::find(advertised.begin(), advertised.end(),
                            std::string{spelling}),
                  advertised.end())
            << "the /elf/osabi refusal does not advertise '" << spelling
            << "', which the reader ACCEPTS (proven above for the alias pairs). "
               "A spelling accepted and denied at once is this class's whole "
               "failure mode.\nmessage was:\n"
            << *msg;
    }
    // …and nothing beyond what it takes.
    for (auto const& q : advertised) {
        if (q == kBadSpelling || q == "osabi") continue;
        json probe            = base;
        probe["elf"]["osabi"] = q;
        auto const pr = ObjectFormatSchema::loadFromText(probe.dump(),
                                                         "osabi-honesty");
        EXPECT_TRUE(pr.has_value())
            << "the /elf/osabi refusal advertises '" << q
            << "' but the reader REFUSES it — a message wider than its check:\n"
            << rejectSummary(pr);
    }
}
