#include "core/types/predefined_macro_json.hpp"

#include "core/substrate/path_identity.hpp"     // genericSpelling
#include "core/types/config_key_vocabulary.hpp" // isDocumentationKey / DSS_CHECK_KEY_VOCABULARY — the SHARED closed-key substrate
#include "core/types/object_format_kind.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace dss {

// Declared in `core/types/preprocess_config.hpp` (nlohmann-free, so tests can
// reach it); defined here beside its only production caller.
std::expected<long long, std::string>
packVersionComponents(std::string_view           versionText,
                      std::span<const long long> weights) {
    // Split on '.' into non-negative decimal components. Anything else — an
    // empty field ("1..2"), a non-digit ("1.0.2a"), a leading/trailing dot —
    // is a malformed version, not something to salvage.
    std::vector<long long> comps;
    std::size_t            pos = 0;
    while (true) {
        const std::size_t      dot  = versionText.find('.', pos);
        const std::string_view part = versionText.substr(
            pos, dot == std::string_view::npos ? std::string_view::npos
                                               : dot - pos);
        if (part.empty()) {
            return std::unexpected(std::format(
                "version '{}' is not a dot-separated list of non-negative "
                "integers", versionText));
        }
        long long v = 0;
        for (char const c : part) {
            if (c < '0' || c > '9') {
                return std::unexpected(std::format(
                    "version '{}' is not a dot-separated list of non-negative "
                    "integers", versionText));
            }
            v = v * 10 + (c - '0');
            if (v > 1000000000LL) {
                return std::unexpected(std::format(
                    "version '{}' has a component too large to encode",
                    versionText));
            }
        }
        comps.push_back(v);
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
    }
    if (comps.size() != weights.size()) {
        return std::unexpected(std::format(
            "'componentWeights' declares {} component(s) but the version '{}' "
            "has {}", weights.size(), versionText, comps.size()));
    }
    // ★ THE ENCODING HAS A BOUND, AND IT IS DERIVED, NOT HARD-CODED.
    // Component i must not reach the NEXT-more-significant weight, or it
    // carries into that field and the packing silently collapses: with
    // [1000000,1000,1], `0.0.1000` would encode identically to `0.1.0` and
    // a `#if`-time `>=` against the macro would start lying. (No identity
    // macro is NAMED here on purpose — the engine knows the `version` KIND and
    // the `componentWeights` key; which macro uses them is config's business,
    // and a name in this comment is exactly what the agnosticism guard in
    // tests/analysis/preprocess catches.) The bound for component i is
    // weights[i-1]/weights[i] — read off the very weights the config declared,
    // so a different encoding gets its own correct bound for free. The MOST
    // significant component (i == 0) has no bound above it and is unbounded by
    // construction. Loud, naming the component.
    for (std::size_t i = 1; i < comps.size(); ++i) {
        const long long bound = weights[i - 1] / weights[i];
        if (comps[i] >= bound) {
            return std::unexpected(std::format(
                "version '{}' component #{} ({}) reaches its weight bound {} — "
                "the packed encoding would collapse and compare wrongly",
                versionText, i, comps[i], bound));
        }
    }
    long long packed = 0;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        packed += comps[i] * weights[i];
    }
    return packed;
}

// Ruling B' SS6 -- see the contract in `preprocess_config.hpp`.
//
// Deliberately LENIENT about everything that is not its own subject: a file that
// is not JSON, is not an object, or carries no `predefinedMacros` array is
// SKIPPED, because every one of those is somebody else's loud failure (the
// schema loaders read these same files and refuse them by name). Reporting them
// a second time here would make this sweep's output a duplicate of theirs, and a
// sweep whose findings are mostly other checks' findings is a sweep nobody
// reads.
std::vector<std::string>
predefinedMacroDocumentDisagreements(std::string_view configRootDir) {
    namespace fs = std::filesystem;
    using json   = nlohmann::json;

    struct Seen {
        std::string where;   // the document that declared it FIRST
        json        surface; // its `impliedSurface` node (absent -> null)
    };
    std::map<std::string, Seen> firstByName;
    std::vector<std::string>    out;

    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator
             it{fs::path{configRootDir},
                fs::directory_options::skip_permission_denied, ec},
         end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".json") continue;
        files.push_back(it->path());
    }
    // Deterministic on every host: a directory iteration order is the
    // filesystem's, and "which document is FIRST" decides which one the message
    // names as the reference. A host-dependent reference makes the same defect
    // read differently on two machines.
    std::sort(files.begin(), files.end());

    for (fs::path const& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        json doc;
        try {
            in >> doc;
        } catch (...) {
            continue;   // the schema loaders own malformed JSON
        }
        if (!doc.is_object()) continue;
        json const* arr = nullptr;
        if (auto const pp = doc.find("preprocess");
            pp != doc.end() && pp->is_object()) {
            if (auto const a = pp->find("predefinedMacros");
                a != pp->end() && a->is_array()) {
                arr = &*a;
            }
        }
        if (arr == nullptr) {
            if (auto const a = doc.find("predefinedMacros");
                a != doc.end() && a->is_array()) {
                arr = &*a;
            }
        }
        if (arr == nullptr) continue;

        // `f` is a config document found under the shipped-config root, which
        // `DSS_CONFIG_ROOT` can point anywhere — a share included. `where` is
        // the only locator on every diagnostic this loop emits.
        std::string const where = core::genericSpelling(f);
        for (json const& e : *arr) {
            if (!e.is_object()) continue;
            auto const n = e.find("name");
            if (n == e.end() || !n->is_string()) continue;
            std::string name = n->get<std::string>();
            if (name.empty()) continue;
            auto const isIt = e.find("impliedSurface");
            json const surface = (isIt == e.end()) ? json{} : *isIt;

            auto const [pos, inserted] =
                firstByName.try_emplace(name, Seen{where, surface});
            if (inserted) continue;
            if (pos->second.surface == surface) continue;
            out.push_back(std::format(
                "predefined macro '{}' is declared by more than one config "
                "document with DIFFERENT 'impliedSurface' declarations: '{}' "
                "says {}, '{}' says {}. A macro's implied surface is one fact; "
                "declared in N documents it must resolve identically in all N, "
                "or a build picks whichever document its target happens to load "
                "and the claim silently changes meaning per leg",
                name, pos->second.where, pos->second.surface.dump(), where,
                surface.dump()));
        }
    }
    return out;
}

namespace detail {

using json = nlohmann::json;

// ── `impliedSurface`: a key vocabulary PER ARM, and never their union ──────
//
// ★★★ THE UNION WOULD BE THE DEFECT, NOT THE SIMPLIFICATION. Each `kind` is a
// different parse arm reading a different set of fields, so a key belonging to
// a SIBLING arm is not "extra" — it is a field the declared arm's code never
// looks at, which is precisely the shape of
// D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY: a correctly-spelled key that
// loads clean and does nothing, and in config that is indistinguishable from a
// key that does something. Flattening the three tables into one would ACCEPT
// exactly that.
//
// ★ And a sibling-arm key gets its OWN sentence rather than a bare "unknown
// key", because the author who wrote it did not make a typo — they pasted from
// the wrong mechanism, and the useful thing to tell them is WHICH mechanism
// owns the key they wrote.
// ★ ONE OWNER PER KEY SPELLING, and the arm tables are COMPOSED from them.
// The spellings used to be typed into the arm tables, again into every
// `contains(...)`/`at(...)` in the parse arms below, and a third time into the
// prose of the missing-field sentence — three owners of one fact, and the
// dangerous disagreement is silent in both directions: a table naming a key the
// parse code never reads accepts a field that does nothing
// (D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY), and a parse arm reading a
// key the table omits is refused before it is ever read.
// ★ THE ONE OWNER OF THE CONFIG-ONLY `version` KIND SPELLING. It is NOT a row
// of `kPredefinedMacroKindTable` on purpose — a `version` entry LOWERS to
// `Constant` at load, so an enum row would name a value no expansion path ever
// produces. Being outside the table is exactly why it needs a name here: it was
// a bare literal at three sites, which is the same second-owner shape one axis
// down (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
constexpr std::string_view kVersionKindName             = "version";

// D-PP-PREDEFINE-REDEFINITION-PARTITION: the ONE owner of the entry key that
// answers "may a PROGRAM `#define`/`#undef` this name?". Named here rather than
// spelled at its three read sites for the reason the block above records — a
// retyped key spelling is a second owner, and the disagreement is silent.
constexpr std::string_view kProgramRedefinitionKey      = "programRedefinition";

constexpr std::string_view kImpliedSurfaceKindKey       = "kind";
constexpr std::string_view kSurfaceHeadersKey           = "headers";
constexpr std::string_view kClaimsNothingReasonKey      = "reason";
constexpr std::string_view kImpliedSurfaceNoteKey       = "note";
constexpr std::string_view kSurfaceClaimHeaderKey       = "header";
constexpr std::string_view kSurfaceClaimNamesKey        = "names";

// One `headers` claim's key vocabulary. Declared HERE rather than at its
// `rejectUnknownKeys` call site because the missing-`impliedSurface` sentence
// renders a well-formed claim as its example, and that example is the one text
// an author copies — a claim shape that drifted from this table would hand them
// a document the loader then refuses.
constexpr std::array<std::string_view, 2> kSurfaceClaimKeys{
    kSurfaceClaimHeaderKey, kSurfaceClaimNamesKey};
DSS_CHECK_KEY_VOCABULARY(kSurfaceClaimKeys);

constexpr std::array<std::string_view, 2> kSurfaceArmKeys{
    kImpliedSurfaceKindKey, kSurfaceHeadersKey};
DSS_CHECK_KEY_VOCABULARY(kSurfaceArmKeys);
constexpr std::array<std::string_view, 3> kClaimsNothingArmKeys{
    kImpliedSurfaceKindKey, kClaimsNothingReasonKey, kImpliedSurfaceNoteKey};
DSS_CHECK_KEY_VOCABULARY(kClaimsNothingArmKeys);
constexpr std::array<std::string_view, 2> kNotExpressibleArmKeys{
    kImpliedSurfaceKindKey, kImpliedSurfaceNoteKey};
DSS_CHECK_KEY_VOCABULARY(kNotExpressibleArmKeys);

struct ImpliedSurfaceArm {
    ImpliedSurfaceKind                kind;
    std::span<std::string_view const> keys;
};
constexpr std::array<ImpliedSurfaceArm, 3> kImpliedSurfaceArms{{
    {ImpliedSurfaceKind::Surface,        kSurfaceArmKeys},
    {ImpliedSurfaceKind::ClaimsNothing,  kClaimsNothingArmKeys},
    {ImpliedSurfaceKind::NotExpressible, kNotExpressibleArmKeys},
}};
// ⚠ EVERY ENUMERATOR MUST HAVE EXACTLY ONE ARM. Without this, adding a fourth
// `ImpliedSurfaceKind` would silently give it an EMPTY key set — which refuses
// even its own `kind` key, i.e. a new state that can never be written. The
// enum's own name table is the authority for what the enumerators ARE, so the
// check is against that rather than against a retyped count.
static_assert(
    [] {
        for (auto const& row : kImpliedSurfaceKindTable.rows) {
            int seen = 0;
            for (auto const& arm : kImpliedSurfaceArms) {
                if (arm.kind == row.first) ++seen;
            }
            if (seen != 1) return false;
        }
        return true;
    }(),
    "kImpliedSurfaceArms must declare exactly one key table per "
    "ImpliedSurfaceKind — a missing arm yields an EMPTY vocabulary that "
    "refuses every key including 'kind'");

void parsePredefinedMacroArray(nlohmann::json const&           pms,
                               std::string_view                arrayPath,
                               DiagnosticCode                  entryCode,
                               substrate::DiagnosticCollector& coll,
                               std::vector<PredefinedMacroDef>& out) {
    for (std::size_t mi = 0; mi < pms.size(); ++mi) {
        const auto  mpath = std::format("{}/{}", arrayPath, mi);
        json const& e     = pms[mi];
        if (!e.is_object()) {
            coll.emit(entryCode, mpath,
                      "a 'predefinedMacros' entry must be an object");
            continue;
        }
        PredefinedMacroDef pm;
        // PROVENANCE, set before any field is read so it is carried even by a
        // row that fails later validation (a diagnostic about a broken row must
        // still be able to say which row).
        pm.declaredAt = mpath;
        // ── D-CONFIG-PREDEFINED-MACRO-ROW-KEYS-UNGATED ────────────────────
        //
        // The CLOSED key vocabulary of a `predefinedMacros` ENTRY. Every
        // field below is read with a bare `e.contains(...)` probe, so before
        // this gate existed a misspelled optional key was silently DROPPED
        // and the entry loaded clean carrying the default the author was
        // trying to override: `"paramss"` shipped an OBJECT-like macro where
        // a function-like one was declared, `"availabelObjectFormats"` made a
        // pe-only spelling predefine on EVERY format, and
        // `"componentWeigths"` turned the `version` kind's missing-field
        // diagnostic into the only thing standing between a typo and a wrong
        // encoding. Same archetype as the encoding-variant row whose
        // `"tempalte"` yielded an all-default template
        // (`D-CONFIG-TARGET-VARIANT-GUARD-UNKNOWN-KEY-SILENTLY-IGNORED`): a
        // CONTAINER with no key set sitting beside leaves that have one.
        //
        // ★ IT LIVES IN THE SHARED PARSER, WHICH IS THE WHOLE POINT AND NOT A
        // COMPLICATION. `predefinedMacros` is declared by THREE config
        // families — the LANGUAGE (`/preprocess/predefinedMacros` in
        // `<lang>.lang.json`), the TARGET (`/predefinedMacros` in
        // `<arch>.target.json`) and the OBJECT FORMAT (`/predefinedMacros` in
        // `<fmt>.format.json`) — and all three reach the entry grammar
        // through this one function. A gate written in any one loader would
        // leave the other two accepting typos, i.e. it would re-create by
        // omission exactly the drift the TF-C74 extraction removed. Written
        // here it is inherited, and a family cannot opt out of it.
        //
        // ★ THE VOCABULARY WAS ENUMERATED FROM WHAT THE SHIPPED DOCUMENTS
        // CONTAIN, NOT ONLY FROM WHAT THIS FUNCTION READS. Those are
        // different sets in general — the sibling target-loader gate's first
        // cut omitted `/target/description`, a key BOTH shipped targets
        // declare and nothing reads, and 21 tests went red. ✔MEASURED here
        // over every `predefinedMacros` array in `src/dss-config/` (21 files,
        // 82 entries: 33 language, 13 target, 36 format): the declared set is
        // `name`/`kind`/`value`/`availableObjectFormats`/`params`/
        // `componentWeights` plus 7 `$comment`s — and it coincides exactly
        // with the read set, so this family has no declared-but-unread key.
        //
        // `$`-prefixed keys are PROSE (the codebase-wide convention) and are
        // exempt via the shared predicate, never a literal `"$comment"`
        // compare — the shipped `c.lang.json` uses `$comment` INSIDE
        // macro entries, and a literal compare would still reject a
        // `$valueComment`.
        //
        // ★ AND IT RUNS BEFORE THE REQUIRED-`name`/`kind` CHECKS, for the
        // reason the `keywords`-entry gate in the grammar loader records: a
        // misspelled `"nmae"` would otherwise be reported ONLY as a MISSING
        // 'name' — a field the author demonstrably did write — sending them
        // hunting in the wrong place. Both diagnostics fire, typo named
        // first. The entry is NOT skipped here for the same reason: the
        // emitted diagnostic already fails the load, and continuing lets the
        // author see every problem with the row in one pass.
        {
            static constexpr std::array<std::string_view, 8> kMacroEntryKeys{
                "name", "kind", "value", "params", "componentWeights",
                "availableObjectFormats", "impliedSurface",
                kProgramRedefinitionKey};
            DSS_CHECK_KEY_VOCABULARY(kMacroEntryKeys);
            // The allowed list is RENDERED FROM THE TABLE by the shared check,
            // never retyped into the message — a hand-written list is one that
            // silently stops matching the array the next time a key is added.
            rejectUnknownKeys(
                e, kMacroEntryKeys, "a 'predefinedMacros' entry",
                [&](std::string_view key, std::string message) {
                    message +=
                        ". Here that means the macro would ship with the very "
                        "default the key was written to override";
                    coll.emit(entryCode, std::format("{}/{}", mpath, key),
                              std::move(message));
                });
        }
        // `name` -- REQUIRED, non-empty string.
        if (!e.contains("name")) {
            coll.emit(DiagnosticCode::C_MissingField, mpath + "/name",
                      "a 'predefinedMacros' entry requires 'name'");
            continue;
        }
        if (!e.at("name").is_string()) {
            coll.emit(entryCode, mpath + "/name",
                      "'predefinedMacros.name' must be a string");
            continue;
        }
        pm.name = e.at("name").get<std::string>();
        if (pm.name.empty()) {
            coll.emit(DiagnosticCode::C_MissingField, mpath + "/name",
                      "'predefinedMacros.name' must be non-empty");
            continue;
        }
        // `kind` -- REQUIRED, one of the CLOSED verb set.
        if (!e.contains("kind")) {
            coll.emit(DiagnosticCode::C_MissingField, mpath + "/kind",
                      "a 'predefinedMacros' entry requires 'kind'");
            continue;
        }
        if (!e.at("kind").is_string()) {
            coll.emit(entryCode, mpath + "/kind",
                      "'predefinedMacros.kind' must be a string");
            continue;
        }
        const std::string kind       = e.at("kind").get<std::string>();
        bool              isConstant = false;
        // ⚠ THIS USED TO BE SIX HAND-WRITTEN `kind == "…"` COMPARISONS, five of
        // which duplicated `kPredefinedMacroKindTable` exactly — a second owner
        // of the enum's spellings sitting three lines from a `…FromName` that
        // already does the lookup — and the refusal below then spelled the same
        // six a THIRD time, UNQUOTED, where no census could see them
        // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
        // ★ `version` STAYS AN ARM OF ITS OWN and is deliberately NOT a table
        // row: it is a CONFIG-level spelling that LOWERS to `Constant` (see the
        // block below), so giving it a row would put a value in the enum that
        // no expansion path ever sees. It gets a named constant instead, so the
        // spelling still has exactly one owner.
        auto const kindOpt = predefinedMacroKindFromName(kind);
        if (kindOpt.has_value()) {
            pm.kind    = *kindOpt;
            isConstant = (*kindOpt == PredefinedMacroKind::Constant);
        } else if (kind == kVersionKindName) {
            // TF-C83 (D-CSUBSET-TOOLCHAIN-IDENTITY-PREDEFINES). The BUILD's own version, packed
            // into ONE integer by config-declared `componentWeights`.
            //
            // WHY A KIND AND NOT A `{{version}}` PLACEHOLDER. A templating
            // syntax would be a NEW vocabulary the engine has to parse, with
            // its own unknown-placeholder failure mode to invent. This table
            // already has the exact shape needed: `date`/`time` are macros
            // whose NAME is config and whose VALUE the engine derives. A
            // version macro is the same shape with a different source, so it
            // is the same mechanism — and an unknown KIND already fails loud
            // (the `else` below), which is precisely the guarantee a
            // placeholder scheme would have had to re-create.
            //
            // It LOWERS TO Constant: the input is build-invariant, so the
            // derivation is a LOAD-time concern and the result is exactly the
            // constant it claims to be. The expansion path is untouched —
            // `date`/`time` stay the only genuinely per-run derived kinds.
            pm.kind = PredefinedMacroKind::Constant;
            if (!e.contains("componentWeights")) {
                coll.emit(DiagnosticCode::C_MissingField,
                          mpath + "/componentWeights",
                          std::format("a '{}' predefinedMacros entry requires "
                                      "'componentWeights' (e.g. "
                                      "[1000000, 1000, 1])",
                                      kVersionKindName));
                continue;
            }
            json const& cw = e.at("componentWeights");
            if (!cw.is_array() || cw.empty()) {
                coll.emit(entryCode, mpath + "/componentWeights",
                          "'componentWeights' must be a non-empty array of "
                          "positive integers, most-significant FIRST");
                continue;
            }
            std::vector<long long> weights;
            bool                   wOk = true;
            for (std::size_t wi = 0; wi < cw.size(); ++wi) {
                if (!cw[wi].is_number_unsigned() || cw[wi].get<long long>() <= 0) {
                    coll.emit(entryCode, mpath + "/componentWeights",
                              "each 'componentWeights' entry must be a "
                              "positive integer");
                    wOk = false;
                    break;
                }
                const long long w = cw[wi].get<long long>();
                // STRICTLY DESCENDING. Equal or ascending weights make the
                // packing non-injective (two versions encode alike) and break
                // the ordering the encoding exists to provide.
                if (!weights.empty() && w >= weights.back()) {
                    coll.emit(entryCode, mpath + "/componentWeights",
                              std::format("'componentWeights' must be strictly "
                                          "descending; {} does not precede {}",
                                          weights.back(), w));
                    wOk = false;
                    break;
                }
                weights.push_back(w);
            }
            if (!wOk) continue;
            if (weights.back() != 1) {
                coll.emit(entryCode, mpath + "/componentWeights",
                          "the LAST 'componentWeights' entry must be 1 (the "
                          "least-significant component is unscaled)");
                continue;
            }
            // The version STRING is injected by the build from the repo-root
            // `VERSION` file, which CMake already reads as the single source
            // of truth. Nothing is duplicated: VERSION -> CMake -> here.
            //
            // The packing itself lives in `packVersionComponents` so that every
            // failure mode below is reachable from a UNIT TEST with an
            // arbitrary version string. Baking `kBuildVersionText` into this
            // function would make the bound check testable only by editing
            // VERSION and reconfiguring the build — i.e. effectively untested.
            auto const packed =
                packVersionComponents(kBuildVersionText, weights);
            if (!packed.has_value()) {
                coll.emit(entryCode, mpath + "/componentWeights",
                          packed.error());
                continue;
            }
            pm.value = std::format("{}", *packed);
        } else {
            // The accepted set is the enum's own projection PLUS the
            // config-only `version` spelling — rendered, never retyped, and
            // QUOTED so a reader (human or test) can tell the spellings apart
            // from the prose around them.
            coll.emit(entryCode, mpath + "/kind",
                      std::format("unknown predefined-macro kind '{}' — "
                                  "accepted: {} / '{}'",
                                  kind,
                                  detail::renderAllowedList(
                                      allNames(kPredefinedMacroKindTable),
                                      " / "),
                                  kVersionKindName));
            continue;
        }
        // `componentWeights` belongs to the `version` kind alone. Silently
        // ignoring it elsewhere would let a typo'd `kind` ship a macro whose
        // declared encoding never ran.
        if (kind != kVersionKindName && e.contains("componentWeights")) {
            coll.emit(entryCode, mpath + "/componentWeights",
                      std::format("'componentWeights' is valid only on a '{}' "
                                  "predefinedMacros entry", kVersionKindName));
            continue;
        }
        // `value` -- REQUIRED iff kind==constant; the static replacement
        // spelling. Ignored for the derived kinds.
        if (isConstant) {
            if (!e.contains("value")) {
                coll.emit(DiagnosticCode::C_MissingField, mpath + "/value",
                          "a 'constant' predefinedMacros entry requires 'value'");
                continue;
            }
            if (!e.at("value").is_string()) {
                coll.emit(entryCode, mpath + "/value",
                          "'predefinedMacros.value' must be a string");
                continue;
            }
            pm.value = e.at("value").get<std::string>();
        }
        // c105 (D-PP-FUNCTION-LIKE-PREDEFINE): OPTIONAL `params` — a
        // FUNCTION-LIKE predefine (e.g. the MSVC-profile `__declspec(x)` →
        // empty erase). Constant-kind only (the derived kinds are inherently
        // object-like). Each param must be a non-empty unique string
        // (C 6.10.3p6 duplicate-param parity with the directive handler,
        // enforced HERE so a config typo fails at load).
        if (e.contains("params")) {
            if (!isConstant) {
                coll.emit(entryCode, mpath + "/params",
                          "'params' is valid only on a 'constant' "
                          "predefinedMacros entry");
                continue;
            }
            json const& prs = e.at("params");
            if (!prs.is_array()) {
                coll.emit(entryCode, mpath + "/params",
                          "'predefinedMacros.params' must be an array of "
                          "parameter-name strings");
                continue;
            }
            bool prOk = true;
            for (std::size_t pi = 0; pi < prs.size(); ++pi) {
                if (!prs[pi].is_string() || prs[pi].get<std::string>().empty()) {
                    coll.emit(entryCode, mpath + "/params",
                              "each 'params' entry must be a non-empty string");
                    prOk = false;
                    break;
                }
                std::string p = prs[pi].get<std::string>();
                // c105 audit L2: each param must BE an identifier
                // ([A-Za-z_][A-Za-z0-9_]*) — a config `"a b"` would otherwise
                // emit a malformed prologue #define that fails only at first
                // preprocess, not at load.
                bool idOk = !(p[0] >= '0' && p[0] <= '9');
                for (char const c : p) {
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                          || (c >= '0' && c <= '9') || c == '_')) {
                        idOk = false;
                        break;
                    }
                }
                if (!idOk) {
                    coll.emit(entryCode, mpath + "/params",
                              std::format("macro parameter '{}' is not an "
                                          "identifier",
                                          p));
                    prOk = false;
                    break;
                }
                if (std::find(pm.params.begin(), pm.params.end(), p)
                    != pm.params.end()) {
                    coll.emit(entryCode, mpath + "/params",
                              std::format("duplicate macro parameter '{}'", p));
                    prOk = false;
                    break;
                }
                pm.params.push_back(std::move(p));
            }
            if (!prOk) continue;
            pm.isFunctionLike = true;
        }
        // OPTIONAL `availableObjectFormats` — a per-format availability filter
        // (mirrors the shipped-lib descriptor field). Absent ⇒ available on
        // every format. Present: an array of object-format NAMES; each must be
        // a known name ("pe"/"elf"/"macho") or fail LOUD (never a silent typo).
        // Lets `_WIN32` be predefined pe-only, and (TF-C74) lets the Apple-only
        // `__arm64__` spelling be predefined macho-only.
        if (e.contains("availableObjectFormats")) {
            json const& afs = e.at("availableObjectFormats");
            if (!afs.is_array()) {
                // The `e.g.` is an ARRAY-SHAPE illustration, so it stays a
                // single name rather than the whole list — but the name itself
                // is projected, because a renamed format spelling would leave a
                // lie in the one sentence an author copies from.
                coll.emit(entryCode, mpath + "/availableObjectFormats",
                          std::format("'predefinedMacros.availableObjectFormats' "
                                      "must be an array of object-format names, "
                                      "e.g. [\"{}\"]",
                                      objectFormatKindName(ObjectFormatKind::Pe)));
                continue;
            }
            bool afOk = true;
            for (std::size_t ai = 0; ai < afs.size(); ++ai) {
                json const& av = afs[ai];
                if (!av.is_string()) {
                    coll.emit(entryCode, mpath + "/availableObjectFormats",
                              "'availableObjectFormats' entries must be strings");
                    afOk = false;
                    break;
                }
                std::string fmt = av.get<std::string>();
                auto const fmtKind = objectFormatKindFromName(fmt);
                if (!fmtKind.has_value()) {
                    // ⚠ ✔MEASURED 2026-08-20: this named `"pe"/"elf"/"macho"` —
                    // THREE of the FIVE spellings the very next line accepts.
                    // `wasm` and `spirv` are declared enumerators with shipped
                    // skeleton formats, so an author gating a macro to one of
                    // them was told BY NAME that a spelling the loader takes is
                    // not allowed. The SELECTABLE projection, not `allNames`:
                    // the `unknown` sentinel spells correctly and is refused two
                    // lines below, so advertising it would send an author to
                    // write the one value that passes this check and fails that
                    // one (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
                    coll.emit(entryCode, mpath + "/availableObjectFormats",
                              std::format("unknown object-format name '{}' — "
                                          "accepted: {}", fmt,
                                          detail::renderAllowedList(
                                              kSelectableObjectFormatKindNames,
                                              " / ")));
                    afOk = false;
                    break;
                }
                // The `unknown` sentinel spells correctly, so the name lookup
                // accepts it — and the filter then matches no real format, so
                // the macro is silently predefined NOWHERE. A macro listed as
                // available that never gets predefined is the same silent
                // no-op a typo'd name would produce; both fail loud here.
                if (!isSelectableObjectFormatKind(*fmtKind)) {
                    coll.emit(entryCode, mpath + "/availableObjectFormats",
                              std::string{kObjectFormatKindSentinelRejection});
                    afOk = false;
                    break;
                }
                pm.availableObjectFormats.push_back(std::move(fmt));
            }
            if (!afOk) continue;
        }
        // ── D-PP-PREDEFINE-REDEFINITION-PARTITION ─────────────────────────
        //
        // `programRedefinition` -- MANDATORY on EVERY entry, a string from the
        // closed verb set. It answers ONE question, and it is narrower than it
        // first looks: when a PROGRAM `#define`s or `#undef`s this name, is that
        // DIAGNOSED? The change ALWAYS takes effect either way. The standard
        // text (6.10.10 carries no `Constraints` heading, so 4p2 makes it UB and
        // no diagnostic is required) and the two-reference measurement behind
        // the verbs live on `PredefinedMacroRedefinition` in
        // `preprocess_config.hpp`.
        //
        // ** MANDATORY, and for the same reason as `impliedSurface`: NEITHER
        // DEFAULT IS SAFE. Defaulting to a warn verb makes DSS noisy about the
        // next `__ARM_NEON` an author adds -- a warning gcc and clang do not
        // emit. Defaulting to ordinary drops the diagnostic on the next
        // `__STDC_ISO_10646__`, which both references DO emit. Both failures are
        // silent and point in opposite directions, so the question is asked out
        // loud on every row rather than answered by omission.
        if (!e.contains(kProgramRedefinitionKey)) {
            coll.emit(DiagnosticCode::C_MissingField,
                      std::format("{}/{}", mpath, kProgramRedefinitionKey),
                      std::format(
                          "a 'predefinedMacros' entry requires '{}' - accepted: "
                          "{}. It selects whether a program's '#define' or "
                          "'#undef' of this name is DIAGNOSED (the change "
                          "applies either way): '{}' for a name ISO C 6.10.10 "
                          "itself lists, '{}' for one whose value the engine "
                          "derives per use, '{}' for an implementation extension "
                          "that is neither. There is no default, because "
                          "guessing a warn verb emits a diagnostic gcc and clang "
                          "do not, and guessing ordinary drops one they do",
                          kProgramRedefinitionKey,
                          detail::renderAllowedList(
                              allNames(kPredefinedMacroRedefinitionTable), " / "),
                          predefinedMacroRedefinitionName(
                              PredefinedMacroRedefinition::WarnIsoMacro),
                          predefinedMacroRedefinitionName(
                              PredefinedMacroRedefinition::WarnDerivedMacro),
                          predefinedMacroRedefinitionName(
                              PredefinedMacroRedefinition::Ordinary)));
            continue;
        }
        if (!e.at(kProgramRedefinitionKey).is_string()) {
            coll.emit(entryCode,
                      std::format("{}/{}", mpath, kProgramRedefinitionKey),
                      std::format("'predefinedMacros.{}' must be a string",
                                  kProgramRedefinitionKey));
            continue;
        }
        {
            const std::string rdText =
                e.at(kProgramRedefinitionKey).get<std::string>();
            auto const rdOpt = predefinedMacroRedefinitionFromName(rdText);
            if (!rdOpt.has_value()) {
                coll.emit(entryCode,
                          std::format("{}/{}", mpath, kProgramRedefinitionKey),
                          std::format("unknown '{}' '{}' - accepted: {}",
                                      kProgramRedefinitionKey, rdText,
                                      detail::renderAllowedList(
                                          allNames(
                                              kPredefinedMacroRedefinitionTable),
                                          " / ")));
                continue;
            }
            pm.programRedefinition = *rdOpt;
            // ★ THE STRUCTURAL RULE, and it is about the KIND, never about any
            // NAME. `ordinary` is implemented by LOWERING the row to a
            // `#define name value` line in the synthetic "<built-in>" prologue
            // -- the c105 mechanism the function-like rows already use, and the
            // very model the references' own built-in buffer implements (gcc
            // reports a `-D` clash as `<command-line>` over `<built-in>`; clang's
            // reads `In file included from <built-in>:388:`). That lowering needs
            // the row's replacement to BE `value`, which is true only of
            // `constant` (the `version` kind lowers to `constant` at load, so it
            // qualifies). A `line`/`file`/`date`/`time`/`counter` row declared
            // ordinary would lower to `#define __LINE__` -- an EMPTY object-like
            // macro -- so the name would still be "defined" and would expand to
            // NOTHING. Fail loud instead of shipping a predefine that silently
            // evaporates.
            if (pm.programRedefinition == PredefinedMacroRedefinition::Ordinary
                && pm.kind != PredefinedMacroKind::Constant) {
                coll.emit(entryCode,
                          std::format("{}/{}", mpath, kProgramRedefinitionKey),
                          std::format(
                              "a '{}' predefinedMacros entry cannot be '{}': its "
                              "replacement is DERIVED per use, not the static "
                              "'value' text, so there is nothing to lower into "
                              "the built-in prologue and the macro would expand "
                              "to nothing. Declare it '{}'",
                              predefinedMacroKindName(pm.kind),
                              predefinedMacroRedefinitionName(
                                  PredefinedMacroRedefinition::Ordinary),
                              predefinedMacroRedefinitionName(
                                  PredefinedMacroRedefinition::WarnDerivedMacro)));
                continue;
            }
            // ★ AND THE MIRROR RULE, which records a fact that was true and
            // UNSAID before this field existed: a FUNCTION-LIKE predefine has
            // ALWAYS been an ordinary macro. c105 lowers it to a "<built-in>"
            // `#define name(params) value`, so the directive handler owns it and
            // it has always been `#undef`-able -- the c105 note says so in as
            // many words. A warn verb on one would promise a diagnostic the
            // engine never had a place to emit, i.e. config asserting behaviour
            // that is already false in the shipped binary.
            if (pm.isFunctionLike
                && predefinedNameIsDiagnosedOnChange(pm.programRedefinition)) {
                coll.emit(entryCode,
                          std::format("{}/{}", mpath, kProgramRedefinitionKey),
                          std::format(
                              "a FUNCTION-LIKE predefinedMacros entry ('params' "
                              "present) must be '{}': it lowers to an ordinary "
                              "'#define' in the built-in prologue, so it has "
                              "always been '#undef'-able with no diagnostic and "
                              "a '{}' claim here would be unenforceable",
                              predefinedMacroRedefinitionName(
                                  PredefinedMacroRedefinition::Ordinary),
                              rdText));
                continue;
            }
        }
        // -- D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE (ruling B') ----
        //
        // `impliedSurface` -- MANDATORY on EVERY entry, always an OBJECT tagged
        // by `kind`. There is no `null` form and no default.
        //
        //   {"kind":"surface",         "headers":[{"header":h,"names":[...]}]}
        //   {"kind":"claims-nothing",  "reason":<closed>, "note":<optional>}
        //   {"kind":"not-expressible", "note":<REQUIRED, non-empty>}
        //
        // ** THE MISSING FIELD IS THE ERROR, AND SO IS AN UNTAGGED ONE. Every
        // other optional key here defaults to "off" when omitted, which is right
        // for a key that ADDS a behavior. This key REMOVES an assumption, so a
        // default would re-create the exact state it exists to end. MEASURED on
        // this field's own first cut: with a `null` form available, 80 of 84 rows
        // took it and FIVE of those were surface-implying platform identities --
        // written while populating the mechanism built to prevent that. The tag
        // is what makes the low-content answer visible in a diff.
        //
        // ** AND A `surface` CLAIM IS SYMBOL-GRANULAR, NEVER HEADER-GRANULAR. A
        // claim naming only a header is nearly VACUOUS once transitive re-export
        // exists: the header resolves the moment an `includes` edge fires, even
        // if the only name that arrives is one of the ones claimed. So `names` is
        // REQUIRED and NON-EMPTY on every claim, and an empty `headers` array is
        // refused -- a requirement that cannot fail is the nominal claim this key
        // exists to make impossible.
        //
        // ** THE KEYS ARE GATED PER STATE. A `reason` under `surface`, or
        // `headers` under `claims-nothing`, is REFUSED rather than ignored: an
        // unreachable field sitting in config looks exactly like a field that
        // does something, which is how the `paramss` class of defect works.
        //
        // ENFORCED IN THE SHARED PARSER, so all THREE families inherit it --
        // language, target and format. The motivating macros are language-owned
        // but nothing about the defect is: `_WIN32` is language-owned, `__LP64__`
        // is FORMAT-owned, and both imply real surfaces.
        //
        // The SHAPE is validated here (core owns the predicate's grammar); the
        // SATISFACTION of a `surface` claim is checked against the shipped
        // descriptor corpus by `ffi::validateShippedSurfaceRequirements`, the
        // only tier that can see the corpus. `not-expressible` is RECORDED and
        // deliberately NOT evaluated -- a language-feature predicate is an
        // operator-deferred build, and this enumerator is not a licence for one.
        //
        // WARNING: THE KEY WAS RENAMED FROM `requires` ON 2026-08-18 BY OPERATOR
        // RULING -- see the long note in `preprocess_config.hpp`. Do not
        // reintroduce that spelling.
        if (!e.contains("impliedSurface")) {
            // The three example objects are SHAPES, not a set — an author needs
            // to see what a well-formed value looks like, and a bare list of
            // three kind spellings would not show that. Every vocabulary token
            // inside them is nonetheless PROJECTED: the `kind` spelling from
            // `kImpliedSurfaceKindTable`, the `reason` from
            // `kClaimsNothingReasonTable`, and the arm's own keys from the
            // per-arm key tables that the parse code below reads. Only the
            // illustrative header/name (`unistd.h` / `getpid`) is literal, and
            // no vocabulary owns those.
            // ⚠ "all three states" is derived too — a fourth `ImpliedSurfaceKind`
            // must not leave this sentence counting to three.
            // Manual argument indices throughout: `{0}` (the tag key) appears
            // four times, and `std::format` forbids mixing automatic and manual
            // indexing, so ONE repeated argument makes the whole sentence
            // manual. Every index is named in the argument list below in order.
            coll.emit(DiagnosticCode::C_MissingField, mpath + "/impliedSurface",
                      std::format(
                          "a 'predefinedMacros' entry requires 'impliedSurface' "
                          "- an object tagged by '{0}': "
                          "{{\"{0}\":\"{1}\",\"{2}\":[{{\"{9}\":\"unistd.h\","
                          "\"{10}\":[\"getpid\"]}}]}}, or "
                          "{{\"{0}\":\"{3}\",\"{4}\":\"{5}\"}}, or "
                          "{{\"{0}\":\"{6}\",\"{7}\":\"...\"}}. It is MANDATORY "
                          "in all {8} states: an omitted field cannot be told "
                          "apart from a question nobody asked, and an unasked "
                          "question is how an identity macro ships promising a "
                          "platform surface that was never built",
                          kImpliedSurfaceKindKey,
                          impliedSurfaceKindName(ImpliedSurfaceKind::Surface),
                          kSurfaceHeadersKey,
                          impliedSurfaceKindName(ImpliedSurfaceKind::ClaimsNothing),
                          kClaimsNothingReasonKey,
                          claimsNothingReasonName(ClaimsNothingReason::ArchProperty),
                          impliedSurfaceKindName(ImpliedSurfaceKind::NotExpressible),
                          kImpliedSurfaceNoteKey,
                          kImpliedSurfaceKindTable.rows.size(),
                          kSurfaceClaimHeaderKey,
                          kSurfaceClaimNamesKey));
            continue;
        }
        {
            json const& is = e.at("impliedSurface");
            if (!is.is_object()) {
                coll.emit(entryCode, mpath + "/impliedSurface",
                          std::format("'impliedSurface' must be an OBJECT "
                                      "tagged by '{}' - there is no null form, "
                                      "because a bare null is exactly the "
                                      "low-content answer that cannot be "
                                      "reviewed",
                                      kImpliedSurfaceKindKey));
                continue;
            }
            if (!is.contains(kImpliedSurfaceKindKey)
                || !is.at(kImpliedSurfaceKindKey).is_string()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          mpath + "/impliedSurface/kind",
                          std::format("'impliedSurface' requires a string '{}' "
                                      "— accepted: {}",
                                      kImpliedSurfaceKindKey,
                                      detail::renderAllowedList(
                                          allNames(kImpliedSurfaceKindTable),
                                          " / ")));
                continue;
            }
            const std::string kindText =
                is.at(kImpliedSurfaceKindKey).get<std::string>();
            auto const        kindOpt  = impliedSurfaceKindFromName(kindText);
            if (!kindOpt.has_value()) {
                coll.emit(entryCode, mpath + "/impliedSurface/kind",
                          std::format("unknown 'impliedSurface.{}' '{}' — "
                                      "accepted: {}",
                                      kImpliedSurfaceKindKey, kindText,
                                      detail::renderAllowedList(
                                          allNames(kImpliedSurfaceKindTable),
                                          " / ")));
                continue;
            }
            ImpliedSurface impl;
            impl.kind = *kindOpt;

            // The key vocabulary is PER STATE, so a field belonging to another
            // state is refused rather than silently ignored — and the arm is
            // looked up from `kImpliedSurfaceArms` ONCE, here, rather than
            // named again inside each `if` branch below. A per-branch call is a
            // per-branch chance to pass the wrong table, and that mistake reads
            // as correct code.
            std::span<std::string_view const> armKeys;
            for (auto const& arm : kImpliedSurfaceArms) {
                if (arm.kind == impl.kind) armKeys = arm.keys;
            }
            bool keysOk = true;
            rejectUnknownKeys(
                is, armKeys,
                std::format("an 'impliedSurface' declared with kind '{}'",
                            kindText),
                [&](std::string_view key, std::string message) {
                    // ★ NAME THE ARM THE KEY BELONGS TO. A sibling-arm key is
                    // not a typo — it is a paste from the wrong mechanism, and
                    // "unknown key" would send the author looking for a
                    // misspelling that is not there.
                    for (auto const& arm : kImpliedSurfaceArms) {
                        if (arm.kind == impl.kind) continue;
                        bool const owns =
                            std::ranges::find(arm.keys, key) != arm.keys.end();
                        if (!owns) continue;
                        message += std::format(
                            ". '{}' IS a declared key — of the '{}' arm, whose "
                            "parse code is the only thing that reads it. Under "
                            "'{}' it is UNREACHABLE, and an unreachable field "
                            "is refused rather than ignored: in config it looks "
                            "exactly like a field that does something",
                            key, impliedSurfaceKindName(arm.kind), kindText);
                        break;
                    }
                    coll.emit(entryCode,
                              std::format("{}/impliedSurface/{}", mpath, key),
                              std::move(message));
                    keysOk = false;
                });
            if (!keysOk) continue;

            if (impl.kind == ImpliedSurfaceKind::Surface) {
                if (!is.contains(kSurfaceHeadersKey)) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              mpath + "/impliedSurface/headers",
                              std::format("a '{}' impliedSurface requires '{}'",
                                          impliedSurfaceKindName(
                                              ImpliedSurfaceKind::Surface),
                                          kSurfaceHeadersKey));
                    continue;
                }
                json const& hs = is.at(kSurfaceHeadersKey);
                // NON-EMPTY. An empty array is a requirement that cannot fail,
                // and a requirement that cannot fail is the nominal claim this
                // key exists to make impossible. Say "implies nothing" with the
                // `claims-nothing` tag and a reason, where it is reviewable.
                if (!hs.is_array() || hs.empty()) {
                    coll.emit(entryCode, mpath + "/impliedSurface/headers",
                              std::format(
                                  "'{0}' must be a NON-EMPTY array of "
                                  "{{\"{1}\",\"{2}\"}} claims - an empty array "
                                  "is a requirement that can never fail; say "
                                  "\"implies nothing\" with "
                                  "{{\"{3}\":\"{4}\",\"{5}\":...}}",
                                  kSurfaceHeadersKey, kSurfaceClaimHeaderKey,
                                  kSurfaceClaimNamesKey, kImpliedSurfaceKindKey,
                                  impliedSurfaceKindName(
                                      ImpliedSurfaceKind::ClaimsNothing),
                                  kClaimsNothingReasonKey));
                    continue;
                }
                bool reqOk = true;
                for (std::size_t hi = 0; hi < hs.size() && reqOk; ++hi) {
                    const auto  hpath =
                        std::format("{}/impliedSurface/headers/{}", mpath, hi);
                    json const& h = hs[hi];
                    if (!h.is_object()) {
                        coll.emit(entryCode, hpath,
                                  std::format("each '{}' entry must be an "
                                              "object {{\"{}\":\"h\",\"{}\":"
                                              "[\"n\",...]}}",
                                              kSurfaceHeadersKey,
                                              kSurfaceClaimHeaderKey,
                                              kSurfaceClaimNamesKey));
                        reqOk = false;
                        break;
                    }
                    {
                        rejectUnknownKeys(
                            h, kSurfaceClaimKeys, "a 'headers' claim",
                            [&](std::string_view key, std::string message) {
                                coll.emit(entryCode,
                                          std::format("{}/{}", hpath, key),
                                          std::move(message));
                                reqOk = false;
                            });
                        if (!reqOk) break;
                    }
                    if (!h.contains("header") || !h.at("header").is_string()
                        || h.at("header").get<std::string>().empty()) {
                        coll.emit(entryCode, hpath + "/header",
                                  "a 'headers' claim requires a non-empty string "
                                  "'header' (the shipped header NAME, e.g. "
                                  "\"dirent.h\")");
                        reqOk = false;
                        break;
                    }
                    ShippedSurfaceClaim claim;
                    claim.header = h.at("header").get<std::string>();
                    for (auto const& prior : impl.headers) {
                        if (prior.header == claim.header) {
                            coll.emit(entryCode, hpath + "/header",
                                      std::format("duplicate 'header' claim '{}' "
                                                  "- one header is claimed once, "
                                                  "with all its names in a single "
                                                  "'names' array",
                                                  claim.header));
                            reqOk = false;
                            break;
                        }
                    }
                    if (!reqOk) break;
                    // `names` -- REQUIRED and NON-EMPTY. THE GRANULARITY RULE,
                    // and this is the only place it can be enforced before the
                    // claim is believed. A header-only claim is satisfied by the
                    // descriptor merely EXISTING -- and with transitive re-export
                    // that is nearly vacuous, since the header resolves the moment
                    // an `includes` edge fires, whatever arrives through it.
                    if (!h.contains("names") || !h.at("names").is_array()
                        || h.at("names").empty()) {
                        coll.emit(entryCode, hpath + "/names",
                                  std::format("the 'headers' claim for '{}' "
                                              "requires a NON-EMPTY 'names' "
                                              "array. A header-only claim is "
                                              "satisfied by the descriptor merely "
                                              "EXISTING, and with transitive "
                                              "re-export that is nearly vacuous - "
                                              "the header resolves the moment an "
                                              "'includes' edge fires, whatever "
                                              "arrives through it. Name the "
                                              "declarations the macro's consumers "
                                              "actually use",
                                              claim.header));
                        reqOk = false;
                        break;
                    }
                    for (std::size_t ni = 0; ni < h.at("names").size(); ++ni) {
                        json const& nv = h.at("names")[ni];
                        if (!nv.is_string() || nv.get<std::string>().empty()) {
                            coll.emit(entryCode, hpath + "/names",
                                      "each 'names' entry must be a non-empty "
                                      "string");
                            reqOk = false;
                            break;
                        }
                        std::string nm = nv.get<std::string>();
                        if (std::find(claim.names.begin(), claim.names.end(), nm)
                            != claim.names.end()) {
                            coll.emit(entryCode, hpath + "/names",
                                      std::format("duplicate name '{}' in a "
                                                  "'names' array",
                                                  nm));
                            reqOk = false;
                            break;
                        }
                        claim.names.push_back(std::move(nm));
                    }
                    if (!reqOk) break;
                    impl.headers.push_back(std::move(claim));
                }
                if (!reqOk) continue;
            } else if (impl.kind == ImpliedSurfaceKind::ClaimsNothing) {
                if (!is.contains(kClaimsNothingReasonKey)
                    || !is.at(kClaimsNothingReasonKey).is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              mpath + "/impliedSurface/reason",
                              std::format("a '{}' impliedSurface requires a "
                                          "string '{}' from the closed set {}",
                                          impliedSurfaceKindName(
                                              ImpliedSurfaceKind::ClaimsNothing),
                                          kClaimsNothingReasonKey,
                                          detail::renderAllowedList(
                                              allNames(kClaimsNothingReasonTable),
                                              " / ")));
                    continue;
                }
                const std::string reasonText =
                    is.at(kClaimsNothingReasonKey).get<std::string>();
                auto const        reasonOpt =
                    claimsNothingReasonFromName(reasonText);
                // CLOSED, and an unknown value fails loud. A free-text reason
                // could not be audited across dozens of rows, and a reason that
                // cannot be audited is a null with extra keystrokes.
                if (!reasonOpt.has_value()) {
                    coll.emit(entryCode, mpath + "/impliedSurface/reason",
                              std::format("unknown '{}' '{}' — accepted: {}",
                                          kClaimsNothingReasonKey, reasonText,
                                          detail::renderAllowedList(
                                              allNames(kClaimsNothingReasonTable),
                                              " / ")));
                    continue;
                }
                impl.reason = *reasonOpt;
                // `note` is OPTIONAL here ON PURPOSE: the honest low-content rows
                // have to stay cheap, or dozens of them become dozens of essays
                // and the field decays into ceremony. When present it must still
                // be a non-empty string -- an empty note is a key that says
                // nothing while looking like it says something.
                if (is.contains(kImpliedSurfaceNoteKey)) {
                    if (!is.at(kImpliedSurfaceNoteKey).is_string()
                        || is.at(kImpliedSurfaceNoteKey)
                               .get<std::string>().empty()) {
                        coll.emit(entryCode, mpath + "/impliedSurface/note",
                                  std::format("'{}' must be a non-empty string "
                                              "when present - omit it rather "
                                              "than declaring an empty one",
                                              kImpliedSurfaceNoteKey));
                        continue;
                    }
                    impl.note = is.at(kImpliedSurfaceNoteKey).get<std::string>();
                }
            } else {
                // NotExpressible
                // REQUIRED here, and that asymmetry with `claims-nothing` is the
                // point: this state's entire content is the sentence saying WHAT
                // the predicate cannot express. Without it the tag is
                // indistinguishable from "claims nothing", which is the exact
                // conflation the third state exists to prevent.
                if (!is.contains(kImpliedSurfaceNoteKey)
                    || !is.at(kImpliedSurfaceNoteKey).is_string()
                    || is.at(kImpliedSurfaceNoteKey).get<std::string>().empty()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              mpath + "/impliedSurface/note",
                              std::format(
                                  "a '{}' impliedSurface requires a non-empty "
                                  "'{}' saying WHAT it implies that this "
                                  "predicate cannot state (e.g. \"GNU C "
                                  "language extensions: statement expressions, "
                                  "__asm__, __attribute__\"). Without it the "
                                  "tag cannot be told apart from '{}', which is "
                                  "the conflation this state exists to prevent",
                                  impliedSurfaceKindName(
                                      ImpliedSurfaceKind::NotExpressible),
                                  kImpliedSurfaceNoteKey,
                                  impliedSurfaceKindName(
                                      ImpliedSurfaceKind::ClaimsNothing)));
                    continue;
                }
                impl.note = is.at(kImpliedSurfaceNoteKey).get<std::string>();
            }
            pm.impliedSurface = std::move(impl);
        }
        // TF-C74: a WITHIN-LIST duplicate name is a load error. Two entries
        // spelling one macro would make the effective definition depend on
        // which of the four preprocessor seed sites iterated last (the
        // `predefined_` map keeps the FIRST via `emplace`, while the pre-scan
        // value prefix and the "<built-in>" prologue are LAST-writer-wins
        // `#define` streams) — a silent divergence between the pre-scan and
        // the authoritative pass, exactly the P0016 seam the shared format
        // filter exists to prevent. Rejecting at load makes it impossible.
        // NOTE: this is a NEW rule (the pre-extraction language loader had NO
        // duplicate check — MEASURED by reading the loop, which only
        // `push_back`s). The shipped configs declare no duplicates, so the
        // shipped-load behaviour is unchanged.
        if (std::find_if(out.begin(), out.end(),
                         [&](PredefinedMacroDef const& prior) {
                             return prior.name == pm.name;
                         })
            != out.end()) {
            coll.emit(entryCode, mpath + "/name",
                      std::format("duplicate predefined macro '{}' — a name may "
                                  "be declared at most once per "
                                  "'predefinedMacros' list",
                                  pm.name));
            continue;
        }
        out.push_back(std::move(pm));
    }
}

} // namespace detail

} // namespace dss
