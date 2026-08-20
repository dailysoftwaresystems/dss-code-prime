#include "link/object_format_schema.hpp"

#include "link/object_format_identity_doc.hpp"

#include "core/crypto/sha256.hpp"  // crypto::sha256Hex — the retained content digest
#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/substrate/relocation_table_json.hpp"
#include "core/types/artifact_profile.hpp"  // isRegisteredArtifactProfile / registeredArtifactProfileList (AP3, shared w/ grammar loader)
#include "core/types/config_key_vocabulary.hpp"  // isDocumentationKey / DSS_CHECK_KEY_VOCABULARY (TF-C74 extraction)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/predefined_macro_json.hpp"  // detail::parsePredefinedMacroArray (TF-C97 — the SHARED predefine grammar, 3rd family)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>     // the per-ARM key vocabularies are selected as spans
#include <string>
#include <string_view>
#include <utility>
#include <vector>   // the root key vocabulary is a UNION built at load time


// ─────────────────────────────────────────────────────────────────────────
// ★★★ COMPILE-ERROR PIN — D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-
// BRANCHES (TF-C125). DO NOT DELETE TO "FIX A BUILD ERROR".
// ─────────────────────────────────────────────────────────────────────────
//
// Twin of the pin in `object_format_schema.cpp` — READ THE FULL NOTE THERE,
// including the list of what this pin does NOT catch. Short version: after all
// #includes the type's NAME is redefined to an identifier that does not exist,
// so any later spelling of it fails to compile, in EVERY form — unqualified,
// `dss::`-qualified and `::dss::`-qualified (✔MEASURED on gcc and MSVC; the
// anonymous-namespace ALIAS this replaced let the qualified spellings through,
// which an independent audit caught).
//
// ★ THIS TU IS WHERE THE RULING BITES HARDEST, which is why it is pinned
// rather than merely cleaned. It held 7 enumerator comparisons AND BOTH
// kind-keyed tables — `kCrossKindRules` and `kVehicleKinds` — and the tree had
// written a DEFENCE of them into the source: *"Expressed as a TABLE … not an
// if-chain: config vocabulary checked against config vocabulary, evaluated
// generically."* That sentence is gone, deliberately, along with its twin in
// `core/types/object_format_kind.hpp`. A rationalization left in the tree
// re-authorizes the pattern it excuses, and the next reader would have cited
// it. The rule those tables enforced still runs — it now asks each backend
// what IT owns, instead of consulting a table that named owners by identity.
//
// A loader is not an exception to the identity-branch veto. It was argued to
// be one, in this file, and the operator rejected the argument.
#define ObjectFormatKind \
    DSS_FORMAT_IDENTITY_IS_NOT_SPELLABLE_IN_THIS_TRANSLATION_UNIT

namespace dss {

namespace {

using json = nlohmann::json;
using Collector = substrate::DiagnosticCollector;

// ── The typo-discriminator adapter for THIS loader ────────────────────────
//
// Binds the shared check (`dss::detail::rejectUnknownKeys`, beside
// `isDocumentationKey`) to this file's sink, diagnostic code and `path/key`
// convention. The CHECK and the sentence are shared; the allowed-key TABLE
// stays with the block it describes, because that is the half a maintainer
// edits when a field is added.
//
// ★★ WHY EVERY CLOSED-KEY BLOCK IN THIS FILE NOW CALLS IT, AND WHY DOING
// TWO OF THEM WOULD HAVE BEEN WORSE THAN DOING NONE. This loader had exactly
// two rejections — the document ROOT and each `relocations[]` row — and TEN
// other closed-key objects with none, so a misspelled key loaded clean and
// left the capability it names at its default. That is the failure this whole
// mechanism exists to prevent, and on a config-driven compiler it means a
// capability quietly not happening: `"segmentPrefixByt"` and TLS access
// silently reverts to the default byte; `"minimumByte"` and a declared stack
// reserve validates requests against 0.
//
// The partial state was ALSO what made the gap invisible. A reader who sees
// the root and the relocation rows refusing typos reasonably concludes the
// nested blocks do too — the container/leaf asymmetry the target loader's own
// helper header names as the archetype. Coverage here is therefore all-or-
// nothing by design, not by completionism.
template <typename KnownKeys>
void rejectUnknownKeys(json const& obj,
                       KnownKeys const& known,
                       std::string_view path,
                       std::string_view objectLabel,
                       Collector& coll) {
    detail::rejectUnknownKeys(obj, known, objectLabel,
        [&](std::string_view key, std::string message) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("{}/{}", path, key), std::move(message));
        });
}

// ── The same check for a block whose vocabulary is PER-ARM ────────────────
//
// `processExit` and `processArgs` each key their field set on a `mechanism`
// verb: the declared arm's parse code reads its own fields and NOTHING of the
// sibling arm's. A union of both arms would therefore accept a key that can
// never be read — inert config that loads clean and does nothing, which is
// `D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY` rebuilt in a new place. So
// the allowed set is `common + the declared arm's own`, and a key belonging to
// a DIFFERENT arm gets a message naming that arm: the difference between "you
// typed this wrong" and "you pasted this from the other mechanism" is the
// whole diagnostic, and only the loader can tell them apart.
//
// `armOwning(key)` answers "which other arm reads this key", or empty.
template <typename KnownKeys, typename ArmOwningFn>
void rejectUnknownArmKeys(json const& obj,
                          KnownKeys const& allowedHere,
                          std::string_view path,
                          std::string_view objectLabel,
                          std::string_view declaredArm,
                          ArmOwningFn&& armOwning,
                          Collector& coll) {
    detail::rejectUnknownKeys(obj, allowedHere, objectLabel,
        [&](std::string_view key, std::string message) {
            std::string_view const other = armOwning(key);
            if (!other.empty()) {
                message += std::format(
                    ". '{}' belongs to mechanism '{}', but this block declares "
                    "'{}' — the '{}' parse arm never reads it, so it would load "
                    "clean and do NOTHING "
                    "(D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY)",
                    key, other, declaredArm, declaredArm);
            }
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("{}/{}", path, key), std::move(message));
        });
}

} // namespace

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadFromText(std::string_view jsonText,
                                  std::string_view sourceLabel) {
    // ── Content digest ────────────────────────────────────────────────
    // Digest the bytes AS RECEIVED, before the parser is allowed an opinion
    // about them. This is the one chokepoint where the document bytes are
    // already in memory (`loadFromFile` reads them, hands them here, and drops
    // them), so the digest costs zero extra I/O — versus ~165 ms per
    // invocation to re-walk and re-read `src/dss-config/` (MEASURED
    // 2026-08-17, I/O-dominated). Computed BEFORE the parse so it is the
    // digest of what was actually LOADED, independent of what the parse made
    // of it. See `contentDigest()` for the full rationale and for why a
    // non-`loadFromText` construction leaves it EMPTY.
    std::string digest = crypto::sha256Hex(jsonText);

    Collector coll;
    json doc;
    try {
        doc = json::parse(jsonText);
    } catch (json::parse_error const& e) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  std::format("JSON parse error: {}", e.what()));
        return std::unexpected(std::move(coll).release());
    }
    if (!doc.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  "top-level value must be a JSON object");
        return std::unexpected(std::move(coll).release());
    }

    // ── closed root-key vocabulary ────────────────────────────────────────
    //
    // ★ THE LAST UNGUARDED LOADER FAMILY, and the highest-stakes of the three.
    // The language loader closed its root keys in TF-C72 and the TARGET loader
    // in TF-C74; the FORMAT loader still read every root key through a bare
    // `doc.contains(…)` and IGNORED unknowns. That matters more here than
    // anywhere else because format keys carry SILENT-MISCOMPILE semantics:
    // `dataModel`, `bitFieldStrategy`, `longDoubleFormat`, `externCallDispatch`,
    // `dataImportBinding`, `tlsAccess`, `stackReserveControl`. A typo'd
    // `"bitFieldStrateg"` in any of the shipped `.format.json` files loads
    // perfectly clean, is never read, and the engine silently falls back to the
    // TARGET's strategy: a wrong-LAYOUT miscompile with no diagnostic. Same
    // helper, same `C_MalformedJson` code and same message wording as the
    // target loader — three loaders behaving identically is the point.
    //
    // ★ `charSignedness` is DELIBERATELY ABSENT from this vocabulary, and its
    // absence is load-bearing rather than an oversight. Bare-`char` signedness
    // is declared ENTIRELY on the TARGET (`charIsUnsigned`, which carries its
    // own per-object-format overrides); a format file that re-declares it here
    // would be a SECOND source of truth that nothing reads, so this guard
    // rejects it by name rather than letting it sit inert.
    //
    // The `$`-prefix carve-out is MANDATORY, not decorative: the shipped format
    // files use `$comment` / `$…Comment` heavily (MEASURED: 20 distinct
    // `$`-prefixed root keys across the 24 shipped files, `$comment` in all 24),
    // so without it this guard would reject every shipped format on first load.
    //
    // ★ Every name here is a key the loader GENUINELY READS — derived by
    // walking this function's own `doc.contains(…)` / `doc.at(…)` sites, NOT
    // copied from a shipped file's key set (that would bake in whatever one
    // file happens to contain and start failing legitimate keys only other
    // files use). `relocations` is read indirectly, through the shared
    // `loadRelocationTable(doc, …)` substrate below. RE-DERIVED (not
    // incremented) after TF-C97 added `predefinedMacros`: 26 keys appear in a
    // direct `doc.contains(…)`/`doc.at(…)`, plus `relocations` through the
    // shared substrate, = 27. VERIFIED against the union of non-`$` root keys
    // across all 24 shipped files: that union is 27 once the 18 LP64 files
    // declare `predefinedMacros`, and the two sets are identical — no key is
    // read-but-never-declared, and none is declared-but-never-read.
    // RE-DERIVED again after D-PP-HEADER-CASE-INSENSITIVE-PE added the REQUIRED
    // `headerNameMatching`: 27 keys appear in a direct `doc.contains(…)`/
    // `doc.at(…)`, plus `relocations` through the shared substrate, = 28; all
    // 24 shipped files declare the new key, so the two sets stay identical.
    // RE-DERIVED a third time after D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN
    // added the REQUIRED `cSymbolDecoration`: 28 keys appear in a direct
    // `doc.contains(…)`/`doc.at(…)`, plus `relocations` through the shared
    // substrate, = 29; all 24 shipped files declare the new key, so the two
    // sets stay identical. ★ REGISTERING THE KEY HERE IS NOT BOOKKEEPING — it
    // is what makes declaring it possible at all: the loop below rejects any
    // unregistered non-`$` root key, so a descriptor that declared
    // `cSymbolDecoration` before this row existed would be refused at LOAD.
    // That ordering is why the vocabulary lands before the descriptors, never
    // after.
    // RE-DERIVED a fourth time in TF-C125: the five per-kind identity blocks
    // (`elf` / `pe` / `optionalHeader` / `macho` / `image`) LEFT this array —
    // they are now contributed by the backends that own them, so 29 - 5 = 24.
    // See the note under the array.
    // RE-DERIVED a fifth time in UCRT-P4: three new root keys —
    // `runtimeLibraries` (the ROLE → IMAGE table that becomes the single owner
    // of "which image plays which runtime role"), `sehPersonality` (the
    // unwinder-personality declaration that deletes two platform literals from
    // `src/mir/merge/synth_seh_funclets.cpp`), and `entryVerbs` (the set of
    // program-entry materialization verbs this format realizes) — so 24 + 3 = 27.
    // ★ THE ORDERING IS LOAD-BEARING AND IT IS ASYMMETRIC (MEASURED, both
    // directions): a key present in a `.format.json` but absent HERE makes that
    // format REFUSED AT LOAD, reddening every test and example on it; a key
    // present here that no file declares leaves LOAD untouched. So the
    // vocabulary row always lands BEFORE the descriptors that declare it, and
    // never after. `DSS_CHECK_KEY_VOCABULARY` below is a static_assert on
    // size-equals-initializer-count plus uniqueness, so the `27` is ENFORCED at
    // compile time rather than maintained by discipline.
    static constexpr std::array<std::string_view, 27> kFormatDocumentKeys{
        // identity + loader gates
        "dssObjectFormatVersion", "format",
        // C-family ABI axes (every one a silent-miscompile risk if it typos)
        "dataModel", "bitFieldStrategy", "longDoubleFormat",
        // the per-OS `#include` header-NAME case rule — a silent WRONG-ACCEPT
        // in one direction and a wrong REJECT in the other if it typos
        "headerNameMatching",
        // the per-format C-symbol decoration rule — a wrong value BINDS TO A
        // DIFFERENT FUNCTION (`_exit` is a real, distinct export on both
        // undecorated formats), so a typo here is a silent-miscompile risk
        "cSymbolDecoration",
        // the C-visible face of those axes (TF-C97 — `__LP64__`/`_LP64`)
        "predefinedMacros",
        // output packaging + artifact vocabulary
        "container", "artifactProfiles",
        // program-entry cluster
        "entryPoint", "entryCallingConvention", "processExit", "processArgs",
        // the set of program-entry materialization VERBS this format realizes
        // (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE — a typo here means a verb the format
        // really does realize drops out of the intersection, so every entry that
        // needs it becomes unresolvable, and the reverse). ★ THE RETIRED
        // `entryShapes` SPELLING IS DELIBERATELY ABSENT: this vocabulary is
        // strict, so a stale file carrying the old key is REFUSED AT LOAD rather
        // than loading with an empty verb set.
        "entryVerbs",
        // import / link contract
        "externCallDispatch", "dataImportBinding", "externAddrBinding",
        "tlsAccess", "librarySynthesis",
        // ROLE → IMAGE table + the roles that name it. A typo in either is a
        // wrong-IMAGE bind, i.e. the eager-import 0xC0000139 class on pe and an
        // undefined symbol elsewhere — never a silent fallback.
        "runtimeLibraries", "sehPersonality",
        // stack-reserve capability + its remedy axis
        "stackReserveControl", "stackReserveUnsupportedReason",
        // section / relocation description
        "sections", "relocations", "supportedDataSections"};
    DSS_CHECK_KEY_VOCABULARY(kFormatDocumentKeys);

    // ★ THE PER-KIND IDENTITY BLOCKS ARE **NOT** LISTED ABOVE ANY MORE, and
    // that is the TF-C125 fix for a real gap an independent audit found. The
    // array used to end `"elf", "pe", "optionalHeader", "macho", "image"` —
    // the SAME five names the backends now own via `identityBlockNames()` —
    // which made this a second, hand-maintained owner of the block vocabulary
    // sitting in shared substrate. The cross-block ownership loop below claims
    // "add a sixth format and this loop covers it untouched"; that was true of
    // the LOOP and false of the LOAD, because a sixth backend's block would
    // have been rejected here as an unknown root key until somebody hand-edited
    // this array AND its size. Two owners that nothing forced to agree is the
    // exact defect this cycle exists to remove — the vocabulary now comes from
    // the backends themselves.
    //
    // The allowed ROOT vocabulary is the static table UNIONED with whatever
    // identity blocks the registered backends declare — built as one range so
    // the shared check needs no special case, and so the ALLOWED list in the
    // diagnostic names the backend blocks too. A sixth backend's block becomes
    // both accepted AND advertised without an edit here.
    std::vector<std::string_view> rootKeys{kFormatDocumentKeys.begin(),
                                           kFormatDocumentKeys.end()};
    for (auto const* b : link::objectFormatBackendTable()) {
        for (char const* blk : b->identityBlockNames()) {
            rootKeys.emplace_back(blk);
        }
    }
    rejectUnknownKeys(doc, rootKeys, "", "the object format document", coll);

    // dssObjectFormatVersion — same per-schema-file version contract
    // as TargetSchema's. v1 is the only accepted version today;
    // future LK* cycles bump as schema shape grows.
    if (!doc.contains("dssObjectFormatVersion")
     || !doc.at("dssObjectFormatVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::C_VersionMismatch, std::string{sourceLabel},
                  "missing or non-integer 'dssObjectFormatVersion'");
        return std::unexpected(std::move(coll).release());
    }
    int const ver = doc.at("dssObjectFormatVersion").get<int>();
    if (ver != 1) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssObjectFormatVersion",
                  std::format("only version 1 supported (got {})", ver));
        return std::unexpected(std::move(coll).release());
    }

    detail::ObjectFormatData data;
    data.id = substrate::mintMonotonicId<ObjectFormatSchemaId>();

    if (!doc.contains("format") || !doc.at("format").is_object()) {
        coll.emit(DiagnosticCode::C_MissingField, std::string{sourceLabel},
                  "missing 'format' object");
        return std::unexpected(std::move(coll).release());
    }
    auto const& format = doc.at("format");
    // The identity block's key set. `version` is OPTIONAL and informational;
    // `name` and `kind` are required below. A typo in any of the three used to
    // load clean — and a misspelled `kind` then fell through to the REQUIRED-key
    // diagnostic, which named the absent key rather than the present typo.
    static constexpr std::array<std::string_view, 3> kFormatBlockKeys{
        "name", "kind", "version"};
    DSS_CHECK_KEY_VOCABULARY(kFormatBlockKeys);
    rejectUnknownKeys(format, kFormatBlockKeys, "/format",
                      "the 'format' identity block", coll);
    if (!format.contains("name") || !format.at("name").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/name",
                  "missing or non-string 'name'");
        return std::unexpected(std::move(coll).release());
    }
    data.name = format.at("name").get<std::string>();
    // Cross-tier symmetry with `target.name` (D-LK6-8.2 post-fold #2
    // architect Q3): `format.name` is the label every walker
    // diagnostic message uses. An empty or whitespace-only name
    // would produce unintelligible diagnostics silently. The same
    // non-empty-non-whitespace discipline applies on both sides.
    auto const isAsciiSpace = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r'
            || c == '\v' || c == '\f';
    };
    bool const isBadName = [&]() noexcept {
        if (data.name.empty()) return true;
        if (isAsciiSpace(data.name.front())) return true;
        if (isAsciiSpace(data.name.back()))  return true;
        for (char c : data.name) if (!isAsciiSpace(c)) return false;
        return true;  // all whitespace
    }();
    if (isBadName) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/name",
                  "'name' must be a non-empty string with no leading "
                  "or trailing whitespace — appears verbatim in every "
                  "walker diagnostic.");
        return std::unexpected(std::move(coll).release());
    }
    if (format.contains("version") && format.at("version").is_string()) {
        data.version = format.at("version").get<std::string>();
    }
    if (!format.contains("kind") || !format.at("kind").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/kind",
                  "missing or non-string 'kind' (one of 'elf' / 'pe' / "
                  "'macho' / 'wasm' / 'spirv')");
        return std::unexpected(std::move(coll).release());
    }
    // ── Resolve the declared format to its BACKEND ──────────────────────
    //
    // WAS: `objectFormatKindFromName` → `isSelectableObjectFormatKind` →
    // `data.kind = *kindOpt`, after which seven sites in this file compared
    // that enum. The declared spelling now resolves straight to the
    // implementation that owns it, and the enum never enters this TU.
    auto const kindText = format.at("kind").get<std::string>();

    // ★ The `unknown` SENTINEL is rejected on its own, BEFORE the registry
    // lookup, purely to keep its diagnostic. Both paths end in "no backend",
    // but the sentinel earns a message that names it — a config author who
    // wrote `"kind": "unknown"` made a different mistake from one who wrote
    // `"kind": "elff"`, and the old code distinguished them. Comparing a
    // declared spelling against the RESERVED spelling (derived from the shared
    // name table, not written out here) is config-vs-config, not an engine
    // identity test.
    //
    // It EARLY-RETURNS on purpose. Continuing under an unresolved format would
    // run the cross-block guard below and emit one spurious "identity block
    // 'elf' is only meaningful when …" per declared block — diagnostics that
    // point at the BLOCKS and advise renaming them, when the single actual
    // defect is the kind. Stopping here leaves exactly one diagnostic.
    if (isObjectFormatKindSentinelName(kindText)) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/format/kind",
                  std::string{kObjectFormatKindSentinelRejection}
                      + " — declare one of 'elf' / 'pe' / 'macho' / 'wasm' / "
                        "'spirv'");
        return std::unexpected(std::move(coll).release());
    }

    // ★★★ THE FAIL-CLOSED GATE. `objectFormatBackendByConfigName` returns
    // nullptr for every spelling no backend claims, and this REFUSES rather
    // than continuing with a null backend. A design in which null meant "skip
    // the identity rules" would let all 24 shipped formats validate clean
    // while validating nothing — see the note on the resolver itself.
    auto const* const backend = link::objectFormatBackendByConfigName(kindText);
    if (backend == nullptr) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/format/kind",
                  "expected 'elf' / 'pe' / 'macho' / 'wasm' / 'spirv'");
        return std::unexpected(std::move(coll).release());
    }
    data.backend = backend;

    // Cross-format identity-block validation (test-analyzer Gap 6 fold, LK8
    // review): every per-format identity block (`elf`, `pe`, `optionalHeader`,
    // `macho`, `image`) is read ONLY by the backend that owns it. A schema
    // declaring `kind: wasm` with a stray `elf` block would silently drop the
    // block. Reject loudly so a copy-paste-then-rename mistake surfaces here.
    //
    // ★★ WAS `kCrossKindRules[]` — five rows pairing a block name with the
    // `ObjectFormatKind` allowed to declare it. THAT TABLE IS THE EXACT SHAPE
    // THIS CYCLE EXISTS TO DELETE, and the comment above it argued it was fine
    // because it was "a TABLE … not an if-chain". It is not fine: a table keyed
    // on format identity is the same dependency as an `if` on format identity,
    // which is why TF-C122 DELETED `kCManglingRules` rather than relocating it.
    //
    // The rule survives with identical semantics and identical wording. What
    // changed is where the pairing comes from: each backend DECLARES the blocks
    // it owns, this loop unions them, and a block owned by somebody other than
    // the resolved backend is rejected. Nothing here knows which name belongs
    // to whom — add a sixth format and this loop covers it untouched.
    for (auto const* other : link::objectFormatBackendTable()) {
        if (other == backend) continue;
        for (char const* blockName : other->identityBlockNames()) {
            if (!doc.contains(blockName)) continue;
            // A block name owned by MORE than one backend is not a conflict;
            // only a block the RESOLVED backend does not own is.
            bool ownedHere = false;
            for (char const* mine : backend->identityBlockNames()) {
                if (std::string_view{mine} == std::string_view{blockName}) {
                    ownedHere = true;
                    break;
                }
            }
            if (ownedHere) continue;
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::string{"/"} + blockName,
                      std::string{"identity block '"} + blockName
                          + "' is only meaningful when format.kind == '"
                          + std::string{other->configName()}
                          + "' (got kind '"
                          + std::string{backend->configName()}
                          + "'). A stray block of the wrong kind would "
                            "be silently dropped — fix the block name or "
                            "the format.kind.");
        }
    }
    // Root keys this format cannot express. WASM has no native relocations, no
    // per-section file-layout knobs, and its entry point lives inside the Start
    // section's function index; SPIR-V's `OpEntryPoint` is emitted inline as a
    // typed module instruction. A top-level declaration of either would be
    // SILENTLY IGNORED by the walker, so reject it loudly and re-anchor the key
    // against plan 18 / plan 17 vocabulary.
    //
    // WAS `if (data.kind == ObjectFormatKind::Wasm || … ::Spirv)` wrapping a
    // hardcoded list. The list moved to the two backends that own it (verbatim,
    // per-key rationale comments included) and is now DECLARED capability, not
    // an enumeration this loader maintains. A format that rejects nothing
    // returns an empty span and this loop does nothing.
    for (char const* field : backend->rejectedRootFields()) {
        if (doc.contains(field)) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::string{"/"} + field,
                      std::string{"format kind '"}
                          + std::string{backend->configName()}
                          + "' must not declare a top-level '"
                          + field
                          + "' field — "
                          + std::string{backend->rejectedRootFieldsReason()}
                          + " A top-level declaration would be silently "
                            "ignored by the walker.");
        }
    }

    // ── FC3 c1: `dataModel` — REQUIRED on EVERY format ──────────────
    //
    // The per-OS C-family width triple ("LP64" / "LLP64" / "ILP32").
    // Closed enum + required: a missing field or unknown spelling is a
    // LOAD reject — a silent default would bake wrong `long` widths
    // into every compile for the format (the knob-that-lies failure the
    // unknown-key discipline exists to prevent). Required on wasm /
    // spirv skeletons too (ILP32, declared-only — the semantic consumer
    // fails loud when an ILP32 format is actually selected).
    if (!doc.contains("dataModel")) {
        coll.emit(DiagnosticCode::C_MissingField, "/dataModel",
                  "missing required 'dataModel' — every object format "
                  "must declare its C-family width triple ('LP64', "
                  "'LLP64', or 'ILP32'); a silent default would bake "
                  "wrong primitive widths");
    } else if (!doc.at("dataModel").is_string()) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/dataModel",
                  "'dataModel' must be a string ('LP64', 'LLP64', or "
                  "'ILP32')");
    } else {
        auto const s = doc.at("dataModel").get<std::string>();
        auto const dm = dataModelFromName(s);
        if (!dm) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/dataModel",
                      std::format("unknown dataModel '{}' — expected one "
                                  "of 'LP64', 'LLP64', 'ILP32'", s));
        } else {
            data.dataModel = *dm;
        }
    }

    // ── D-PP-HEADER-CASE-INSENSITIVE-PE: `headerNameMatching` — REQUIRED
    //    on EVERY format ──────────────────────────────────────────────
    //
    // How an `#include` header NAME is matched against the filesystem
    // ("case-sensitive" / "case-insensitive"). Closed enum + REQUIRED,
    // for the SAME reason `dataModel` is: the alternative to declaring
    // it is letting the BUILD HOST's filesystem answer a TARGET
    // question. MEASURED before this axis existed, on one Linux host
    // with one binary and only the filesystem varied: `<Windows.h>` was
    // rejected for a pe64 target on ext4 and accepted through /mnt/c,
    // and `<Stdio.h>` compiled CLEAN for an elf target on Windows.
    //
    // ★ Required rather than optional-with-default ON PURPOSE. A
    // default would be silent, and the silence would land on exactly
    // the files most likely to be added later: a new pe or macho
    // format that forgot the key would resolve case-SENSITIVELY and
    // reject `<Windows.h>` again, with nothing in the tree to say why.
    // Making a new format file fail at LOAD is what removes that.
    if (!doc.contains("headerNameMatching")) {
        coll.emit(DiagnosticCode::C_MissingField, "/headerNameMatching",
                  "missing required 'headerNameMatching' — every object "
                  "format must declare how an `#include` header NAME is "
                  "matched ('case-sensitive' or 'case-insensitive'); a "
                  "silent default would let the build HOST's filesystem "
                  "decide a TARGET question");
    } else if (!doc.at("headerNameMatching").is_string()) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/headerNameMatching",
                  "'headerNameMatching' must be a string ('case-sensitive' "
                  "or 'case-insensitive')");
    } else {
        auto const s = doc.at("headerNameMatching").get<std::string>();
        auto const hm = headerNameMatchingFromName(s);
        if (!hm) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/headerNameMatching",
                      std::format("unknown headerNameMatching '{}' — expected "
                                  "one of 'case-sensitive', 'case-insensitive'",
                                  s));
        } else {
            data.headerNameMatching = *hm;
        }
    }

    // ── D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN: `cSymbolDecoration` ──────
    //
    // HOW this format decorates a canonical C identifier to obtain the
    // LINKER-visible name (`none` / `leading-underscore`). A BLOCK rather
    // than a bare scalar — `{"scheme": "..."}` — mirroring `processExit`'s
    // `mechanism` dispatch below, so a future scheme that needs per-arm
    // parameters (32-bit PE stdcall's `@N` byte count) gains a sibling key
    // INSIDE the block instead of a second root key. See `CSymbolDecoration`
    // (core/types/object_format_kind.hpp) for the full rationale.
    //
    // The scheme is a CLOSED verb, never a literal prefix string: a free
    // string makes `"__"` / `" "` representable, i.e. it lets config request
    // decorations no engine arm implements, and nothing could refuse them at
    // load. An unknown spelling is a HARD error — the `dataModel` discipline
    // — because a typo that fell back to `none` on a Mach-O format would bind
    // every C call to an undecorated name that libSystem does not export.
    //
    // ★ REQUIRED ON EVERY FORMAT, UNCONDITIONALLY — not gated on exec flavor,
    // not gated on kind. A relocatable Mach-O `.o` carries `_main` just as its
    // executable sibling does; `unapplyCMangling` runs on LIBRARY INGEST
    // regardless of the artifact flavor being produced; and decisively, a
    // universal predicate CANNOT BE TAUTOLOGICAL. Gate the rule on any
    // property and it stops enforcing on whatever that property excludes,
    // silently — which is precisely how the `processExit ⇒ isExecFlavor`
    // rule lost its teeth on the ET_DYN arm (see the footgun note on
    // `ObjectFormatSchema::isExecFlavor`). REQUIRED rather than
    // optional-with-default for the reason the anchor exists: the defect
    // being closed is a per-format fact with TWO OWNERS, and a default would
    // just make the C++ table the silent winner again for any file that
    // forgot the key.
    if (!doc.contains("cSymbolDecoration")) {
        coll.emit(DiagnosticCode::C_MissingField, "/cSymbolDecoration",
                  "missing required 'cSymbolDecoration' — every object format "
                  "must declare how a canonical C identifier is decorated to "
                  "obtain its linker-visible name (a block "
                  "{\"scheme\": \"none\"} or "
                  "{\"scheme\": \"leading-underscore\"}); a silent default "
                  "would re-hide the rule in the engine's C++ table, which is "
                  "the two-owner defect this key exists to remove");
    } else if (!doc.at("cSymbolDecoration").is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/cSymbolDecoration",
                  "'cSymbolDecoration' must be an object with a 'scheme' "
                  "string ('none' or 'leading-underscore')");
    } else {
        auto const& csd = doc.at("cSymbolDecoration");
        // One key, and the strictest one in the file: this block decides how a
        // canonical C identifier becomes a linker-visible name, and a wrong
        // answer BINDS TO A DIFFERENT FUNCTION (`_exit` is a real, distinct
        // export on both undecorated formats). A misspelled `"schema"` used to
        // load clean and surface as the missing-key diagnostic, which names the
        // absent key rather than the present typo.
        static constexpr std::array<std::string_view, 1>
            kCSymbolDecorationKeys{"scheme"};
        DSS_CHECK_KEY_VOCABULARY(kCSymbolDecorationKeys);
        rejectUnknownKeys(csd, kCSymbolDecorationKeys, "/cSymbolDecoration",
                          "the 'cSymbolDecoration' block", coll);
        if (!csd.contains("scheme") || !csd.at("scheme").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/cSymbolDecoration/scheme",
                      "'cSymbolDecoration' requires a 'scheme' string "
                      "('none' or 'leading-underscore')");
        } else {
            auto const s = csd.at("scheme").get<std::string>();
            auto const sc = cSymbolDecorationSchemeFromName(s);
            if (!sc) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/cSymbolDecoration/scheme",
                          std::format("unknown cSymbolDecoration.scheme "
                                      "'{}' — expected one of 'none', "
                                      "'leading-underscore'", s));
            } else {
                data.cSymbolDecoration.scheme = *sc;
            }
        }
    }

    // ── TF-C97 (D-PP-FORMAT-DATA-MODEL-PREDEFINES): OPTIONAL
    //    `predefinedMacros` ────────────────────────────────────────────
    //
    // The macros this FORMAT predefines — the C-visible face of the axes
    // declared immediately above, `dataModel` first among them
    // (`__LP64__`/`_LP64`). Declared HERE and not on the target because the
    // axis is per-FORMAT: one CPU (x86_64) is LP64 under elf64/macho64 and
    // LLP64 under pe64, so a target-side row would be wrong on one of its own
    // formats. Both `.target.json` files say exactly this in prose and defer
    // to this key.
    //
    // The per-entry grammar is the SHARED parser the language and target
    // loaders use (`parsePredefinedMacroArray`), so the closed `kind` verb
    // set, the Constant⇒`value` rule, the function-like `params` checks, the
    // within-array duplicate-name reject and the `availableObjectFormats`
    // validation are INHERITED rather than re-implemented — the same argument
    // that extracted the parser at TF-C74, now paying off a third time.
    // Malformed entries emit `C_MalformedJson` (this family's code for a
    // structurally-wrong value, as `dataModel`/`bitFieldStrategy` above do);
    // MISSING required fields emit the universal `C_MissingField`.
    //
    // ★ THE LOADER NEVER SYNTHESIZES A MACRO NAME. It reads whatever the file
    // declares; which names an LP64 format ought to declare is derived by the
    // config author from that file's own `dataModel`, in the file. Deriving it
    // here would bake a C spelling into the object-format tier — a
    // source-language-agnosticism break, not a shortcut. OPTIONAL; absent ⇒
    // empty ⇒ the effective predefine list is byte-identical to pre-TF-C97,
    // which is the DECLARED answer for every LLP64/ILP32 format.
    if (doc.contains("predefinedMacros")) {
        json const& pms = doc.at("predefinedMacros");
        if (!pms.is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/predefinedMacros",
                      "'predefinedMacros' must be an array");
        } else {
            detail::parsePredefinedMacroArray(
                pms, "/predefinedMacros", DiagnosticCode::C_MalformedJson,
                coll, data.predefinedMacros);
        }
    }

    // ── D-CSUBSET-BITFIELD-ABI-EXACT: OPTIONAL `bitFieldStrategy` ────
    //
    // The per-ABI C bit-field packing rule ("gnu_packed" / "msvc_straddle").
    // Determined by the OBJECT FORMAT / OS, not the CPU (x86_64 serves BOTH
    // ELF-SysV gnu_packed and PE-MS msvc_straddle), so it lives here next to
    // `dataModel`. OPTIONAL: absent ⇒ BitFieldStrategy::None and the driver
    // falls back to the TARGET's declared `aggregateLayout.bitFieldStrategy`
    // (back-compat). A wrong spelling is a HARD error — a typo can never
    // silently fall back to a wrong rule (the dataModel discipline).
    if (doc.contains("bitFieldStrategy")) {
        if (!doc.at("bitFieldStrategy").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/bitFieldStrategy",
                      "'bitFieldStrategy' must be a string ('gnu_packed' or "
                      "'msvc_straddle')");
        } else {
            auto const s = doc.at("bitFieldStrategy").get<std::string>();
            auto const bs = bitFieldStrategyFromName(s);
            if (!bs) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/bitFieldStrategy",
                          std::format("unknown bitFieldStrategy '{}' — expected "
                                      "'gnu_packed' or 'msvc_straddle'", s));
            } else if (*bs == BitFieldStrategy::None) {
                // "none" is the sentinel, not a selectable strategy on a format.
                coll.emit(DiagnosticCode::C_MalformedJson, "/bitFieldStrategy",
                          "bitFieldStrategy 'none' is not selectable — omit the "
                          "field to leave it unset (the target's value is used)");
            } else {
                data.bitFieldStrategy = *bs;
            }
        }
    }

    // ── D-FF1-AR-STATICLIB-DRIVER-WIRING (c171): OPTIONAL `container` ──
    //
    // The output PACKAGING axis ("single" / "archive"). `archive` marks a
    // static-library format whose driver output is an `ar` bundle of
    // relocatable members (`.a`/`.lib`) — the driver routes it to
    // `linkAndWriteStaticArchive`, dispatching on this field (never the
    // artifactProfile name — the standing agnosticism veto). OPTIONAL:
    // absent ⇒ ObjectFormatContainer::Single (one standalone file), so
    // every pre-c171 format is byte-identical. A wrong spelling is a HARD
    // error — a typo can never silently pick a packaging (the dataModel
    // discipline). Unlike `bitFieldStrategy`, `single` (the default) IS a
    // selectable spelling: there is no sentinel value to reject.
    if (doc.contains("container")) {
        if (!doc.at("container").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/container",
                      "'container' must be a string ('single' or 'archive')");
        } else {
            auto const s = doc.at("container").get<std::string>();
            auto const c = objectFormatContainerFromName(s);
            if (!c) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/container",
                          std::format("unknown container '{}' — expected "
                                      "'single' or 'archive'", s));
            } else {
                data.container = *c;
            }
        }
    }

    // ── FC17.9(e) (D-CSUBSET-LONG-DOUBLE): OPTIONAL `longDoubleFormat` ──
    //
    // The per-format `long double` representation axis ("f64" / "x87-80" /
    // "ieee128"). OS/format-determined like `dataModel`/`bitFieldStrategy`
    // (x86_64 serves BOTH pe64's 64-bit-IEEE and ELF-SysV's x87 80-bit), so
    // it lives here. OPTIONAL: absent ⇒ LongDoubleFormat::None — the semantic
    // bind then leaves `long double` rows UNREALIZED (loud
    // S_LongDoubleFormatUndeclared on use, never a silent base-core width). A
    // wrong spelling is a HARD error — a typo can never silently un-declare
    // the axis (the dataModel discipline). `None` has no spelling: omission
    // is the only undeclared state.
    if (doc.contains("longDoubleFormat")) {
        if (!doc.at("longDoubleFormat").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/longDoubleFormat",
                      "'longDoubleFormat' must be a string ('f64', 'x87-80', "
                      "or 'ieee128')");
        } else {
            auto const s = doc.at("longDoubleFormat").get<std::string>();
            auto const lf = longDoubleFormatFromName(s);
            if (!lf) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/longDoubleFormat",
                          std::format("unknown longDoubleFormat '{}' — expected "
                                      "'f64', 'x87-80', or 'ieee128'", s));
            } else {
                data.longDoubleFormat = *lf;
            }
        }
    }

    // Top-level `entryPoint` — universal entry-symbol name for
    // executable artifacts (e.g. "_start" / "main" / Mach-O's
    // LC_MAIN target). Empty for relocatable artifacts. The walker
    // resolves this against AssembledModule's symbols at emit time
    // to compute the entry virtual address.
    if (doc.contains("entryPoint")) {
        if (!doc.at("entryPoint").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/entryPoint",
                      "'entryPoint' must be a string");
        } else {
            data.entryPoint = doc.at("entryPoint").get<std::string>();
        }
    }

    // D-LK10-ENTRY Slice B (plan 14 §2.13): `entryCallingConvention`
    // — names the cc the trampoline emitter resolves via
    // `target.callingConventionByName(...)`. Required (cross-field
    // rule in validate() below) whenever `processExit` is declared;
    // shipped values: "ms_x64" for PE-Exec, "sysv_amd64" for ELF/
    // Mach-O-Exec, "aapcs64" for ARM64.
    if (doc.contains("entryCallingConvention")) {
        if (!doc.at("entryCallingConvention").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/entryCallingConvention",
                      "'entryCallingConvention' must be a string");
        } else {
            auto const cc =
                doc.at("entryCallingConvention").get<std::string>();
            // dim-2 HIGH #2 (7425905 audit fold): non-whitespace
            // check. Without this, leading/trailing whitespace in a
            // hand-edited JSON would silently pass schema-load and
            // fail only at Slice C trampoline-build time via
            // `callingConventionByName()` returning nullptr.
            // Symmetric to the format.name discipline.
            bool const hasWs = std::any_of(cc.begin(), cc.end(),
                [](unsigned char c) { return std::isspace(c); });
            if (cc.empty() || hasWs) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/entryCallingConvention",
                          "'entryCallingConvention' must be a "
                          "non-empty string with no whitespace "
                          "(must resolve via `target."
                          "callingConventionByName(...)`).");
            } else {
                data.entryCallingConvention = cc;
            }
        }
    }

    // D-FFI-EXTERN-CALL-DISPATCH: `externCallDispatch` — the format's
    // extern-call shape ("indirect-slot" / "direct-plt"). Optional in
    // the JSON (a relocatable / WASM / SPIR-V format, or an exec format
    // built for a non-FFI purpose, omits it). validate() does NOT require
    // it — the real requirement ("a format that LOWERS an extern call
    // needs a dispatch shape") is enforced at MIR→LIR (the `Lowerer` ctor
    // guard fails loud on extern-imports-under-nullopt). Present-but-
    // unknown IS a fail-loud HERE at load (a typo must NOT silently fall
    // through to a default extern-call shape).
    if (doc.contains("externCallDispatch")) {
        if (!doc.at("externCallDispatch").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/externCallDispatch",
                      "'externCallDispatch' must be a string "
                      "(\"indirect-slot\" or \"direct-plt\")");
        } else {
            auto const s =
                doc.at("externCallDispatch").get<std::string>();
            auto const d = externCallDispatchFromName(s);
            if (!d.has_value()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/externCallDispatch",
                          std::format("unknown externCallDispatch '{}' "
                                      "— accepted: \"indirect-slot\" "
                                      "(PE IAT / Mach-O __got), "
                                      "\"direct-plt\" (ELF PLT stub)",
                                      s));
            } else {
                data.externCallDispatch = *d;
            }
        }
    }

    // D-LK-EXTERN-DATA-IMPORT: `dataImportBinding` — the format's
    // extern-DATA import binding model ("got-indirect"). Optional in
    // the JSON (a format whose data-import model has not landed — every
    // relocatable flavor — omits it; the linker's pre-walker gate then
    // fails loud on any surviving data import instead of binding a data
    // symbol through the function-import machinery).
    // Present-but-unknown IS a fail-loud HERE at load (a typo must NOT
    // silently degrade to "no data imports supported" — the
    // externCallDispatch discipline). ★ That closed-enum reject is now
    // ALSO what keeps `"copy-relocation"` from coming back: the value
    // was DELETED from `DataImportBinding`
    // (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET — it split
    // an aliased libc object silently), so a format file spelling it
    // is REFUSED AT LOAD naming the file, exactly as any other unknown
    // value is. Leaving the value accepted-but-unused would have left a
    // future format one JSON edit away from the whole defect class.
    if (doc.contains("dataImportBinding")) {
        if (!doc.at("dataImportBinding").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/dataImportBinding",
                      "'dataImportBinding' must be a string "
                      "(\"got-indirect\")");
        } else {
            auto const s =
                doc.at("dataImportBinding").get<std::string>();
            auto const b = dataImportBindingFromName(s);
            if (!b.has_value()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/dataImportBinding",
                          std::format("unknown dataImportBinding '{}' "
                                      "— accepted: \"got-indirect\" "
                                      "(a loader-bound pointer slot "
                                      "holding the library object's "
                                      "ADDRESS: ELF .got + R_*_GLOB_DAT, "
                                      "Mach-O __got, PE IAT). "
                                      "\"copy-relocation\" was REMOVED: "
                                      "it claimed one name of an alias "
                                      "set and split the object. See "
                                      "D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET.",
                                      s));
            } else {
                data.dataImportBinding = *b;
            }
        }
    }

    // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): `externAddrBinding`
    // — the format's extern-ADDRESS materialization binding ("got").
    // Optional in the JSON (only the arm64 relocatable + static-archive
    // formats declare it; the DSS-linked exec/pie/dyn formats omit it and
    // materialize an `&extern` value via the ordinary lea). Present-but-
    // unknown IS a fail-loud HERE at load (a typo must NOT silently
    // degrade to "no GOT-address support" — the externCallDispatch /
    // dataImportBinding discipline).
    if (doc.contains("externAddrBinding")) {
        if (!doc.at("externAddrBinding").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/externAddrBinding",
                      "'externAddrBinding' must be a string (\"got\")");
        } else {
            auto const s =
                doc.at("externAddrBinding").get<std::string>();
            auto const b = externAddrBindingFromName(s);
            if (!b.has_value()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/externAddrBinding",
                          std::format("unknown externAddrBinding '{}' "
                                      "— accepted: \"got\" (ELF "
                                      "relocatable GOT-slot address via "
                                      "ADR_GOT_PAGE + LD64_GOT_LO12_NC)",
                                      s));
            } else {
                data.externAddrBinding = *b;
            }
        }
    }

    // D-CSUBSET-THREAD-LOCAL (TLS C1): `tlsAccess` block — the format's
    // thread-local access model + the x86 access-sequence values.
    // Optional in the JSON (a format whose TLS machinery has not landed
    // — PE / Mach-O / every relocatable flavor — omits it; MIR→LIR then
    // fails loud K_FormatLacksThreadLocalSupport on the first
    // thread-local access instead of silently lowering a process-shared
    // alias). A PRESENT block is strict — closed verb set + range
    // checks; a typo must NOT silently degrade to "no TLS support" (the
    // externCallDispatch discipline).
    if (doc.contains("tlsAccess")) {
        auto const& ta = doc.at("tlsAccess");
        if (!ta.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/tlsAccess",
                      // Lists all FOUR keys the parse arm reads. It listed
                      // three: `tlsIndexSlotName` was read and never
                      // advertised, which mattered the moment the key set
                      // became closed below.
                      "'tlsAccess' must be an object { \"model\": "
                      "\"local-exec\"|\"pe-indexed\"|\"macho-tlv\", "
                      "\"segmentPrefixByte\": N, \"baseDisplacement\": N, "
                      "\"tlsIndexSlotName\": \"…\" }");
        } else {
            TlsAccessInfo info{};
            bool ok = true;
            // ⓘ The wrong-shape message just above lists only three of these
            // four keys; `tlsIndexSlotName` is read too. The TABLE is derived
            // from the parse code, which is the half that decides.
            static constexpr std::array<std::string_view, 4> kTlsAccessKeys{
                "model", "segmentPrefixByte", "baseDisplacement",
                "tlsIndexSlotName"};
            DSS_CHECK_KEY_VOCABULARY(kTlsAccessKeys);
            rejectUnknownKeys(ta, kTlsAccessKeys, "/tlsAccess",
                              "the 'tlsAccess' block", coll);
            if (!ta.contains("model") || !ta.at("model").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField, "/tlsAccess/model",
                          "'tlsAccess.model' is required and must be a "
                          "string — accepted: \"local-exec\" (ELF static "
                          "TLS), \"pe-indexed\" (PE TEB slot array), "
                          "\"macho-tlv\" (Mach-O TLV descriptor)");
                ok = false;
            } else {
                auto const s = ta.at("model").get<std::string>();
                auto const m = tlsAccessModelFromName(s);
                if (!m.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tlsAccess/model",
                              std::format("unknown tlsAccess model '{}' — "
                                          "accepted: \"local-exec\", "
                                          "\"pe-indexed\", \"macho-tlv\"",
                                          s));
                    ok = false;
                } else {
                    info.model = *m;
                }
            }
            if (ta.contains("segmentPrefixByte")) {
                if (!ta.at("segmentPrefixByte").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tlsAccess/segmentPrefixByte",
                              "'segmentPrefixByte' must be an integer in "
                              "[0, 255]");
                    ok = false;
                } else {
                    std::int64_t const b =
                        ta.at("segmentPrefixByte").get<std::int64_t>();
                    if (b < 0 || b > 255) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/tlsAccess/segmentPrefixByte",
                                  std::format("'segmentPrefixByte' ({}) out "
                                              "of range [0, 255]", b));
                        ok = false;
                    } else {
                        info.segmentPrefixByte = static_cast<std::uint8_t>(b);
                    }
                }
            }
            if (ta.contains("baseDisplacement")) {
                if (!ta.at("baseDisplacement").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tlsAccess/baseDisplacement",
                              "'baseDisplacement' must be a non-negative "
                              "integer (the tp slot's disp32)");
                    ok = false;
                } else {
                    std::int64_t const d =
                        ta.at("baseDisplacement").get<std::int64_t>();
                    // The value rides an x86 SIGNED disp32 at encode time;
                    // cap at INT32_MAX so the u32→i32 handoff can never
                    // flip sign silently.
                    if (d < 0 || d > 0x7FFFFFFFLL) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/tlsAccess/baseDisplacement",
                                  std::format("'baseDisplacement' ({}) out "
                                              "of range [0, 2^31-1] (it is "
                                              "emitted as a signed disp32)",
                                              d));
                        ok = false;
                    } else {
                        info.baseDisplacement = static_cast<std::uint32_t>(d);
                    }
                }
            }
            // TLS C3 (D-CSUBSET-THREAD-LOCAL): the `_tls_index` slot NAME —
            // the writer-minted module-TLS-index singleton the `pe-indexed`
            // access sequence's riprel read targets. A string when present;
            // REQUIRED (non-empty) for the pe-indexed model below so a
            // pe-indexed format can never silently ship WITHOUT the slot its
            // access sequence indexes (the same closed-verb strictness the
            // model/segment/displacement fields hold). Ignored for
            // local-exec / macho-tlv (their tp reads index no module array).
            if (ta.contains("tlsIndexSlotName")) {
                if (!ta.at("tlsIndexSlotName").is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tlsAccess/tlsIndexSlotName",
                              "'tlsIndexSlotName' must be a string (the name "
                              "of the module TLS-index slot the pe-indexed "
                              "access sequence reads)");
                    ok = false;
                } else {
                    info.tlsIndexSlotName =
                        ta.at("tlsIndexSlotName").get<std::string>();
                }
            }
            if (ok && info.model == TlsAccessModel::PeIndexed
                && info.tlsIndexSlotName.empty()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/tlsAccess/tlsIndexSlotName",
                          "'tlsIndexSlotName' is REQUIRED for the "
                          "'pe-indexed' TLS model — its access sequence "
                          "reads a named module TLS-index singleton "
                          "(`mov ecx, [_tls_index]`); a pe-indexed block "
                          "without it cannot lower a thread-local access");
                ok = false;
            }
            if (ok) data.tlsAccess = info;
        }
    }

    // ── UCRT-P4: `runtimeLibraries` — the ROLE → IMAGE table ─────────────
    //
    // Parsed FIRST among the role-consuming blocks (`processExit`,
    // `processArgs`, `sehPersonality`, `librarySynthesis` all resolve against
    // it), so `resolveRuntimeRole` below is available to every one of them.
    // Each row is `{"role": <closed enum>, "image": <non-empty string>}`; roles
    // are unique cross-row. An unknown role spelling, the `none` sentinel, a
    // missing/empty image, or a duplicate role all REFUSE the document — the
    // whole point of the table is that a wrong image binds to a symbol that
    // either does not exist (a pe LOAD failure at 0xC0000139) or is a DIFFERENT
    // function, and neither is detectable downstream.
    if (doc.contains("runtimeLibraries")) {
        if (!doc.at("runtimeLibraries").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/runtimeLibraries",
                      "'runtimeLibraries' must be an ARRAY of "
                      "{ \"role\": ..., \"image\": ... } rows — the role "
                      "vocabulary is closed ('cLibrary', "
                      "'unwindPersonality', 'systemPrimitives')");
        } else {
            auto const& arr = doc.at("runtimeLibraries");
            std::size_t i = 0;
            for (auto const& row : arr) {
                auto const path = std::format("/runtimeLibraries/{}", i);
                ++i;
                if (!row.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "each runtimeLibraries entry must be an object "
                              "{ \"role\": ..., \"image\": ... }");
                    continue;
                }
                // The ROLE -> IMAGE table is the one owner of "which image
                // does this format import from", so a typo'd key here is a
                // wrong-IMAGE bind — the pe eager-import 0xC0000139 class.
                static constexpr std::array<std::string_view, 2>
                    kRuntimeLibraryRowKeys{"role", "image"};
                DSS_CHECK_KEY_VOCABULARY(kRuntimeLibraryRowKeys);
                rejectUnknownKeys(row, kRuntimeLibraryRowKeys, path,
                                  "a 'runtimeLibraries' row", coll);
                if (!row.contains("role") || !row.at("role").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField, path + "/role",
                              "missing or non-string 'role'");
                    continue;
                }
                auto const roleText = row.at("role").get<std::string>();
                auto const role = runtimeLibraryRoleFromName(roleText);
                if (!role.has_value() || *role == RuntimeLibraryRole::None) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path + "/role",
                              std::format(
                                  "unknown runtimeLibraries role '{}' — "
                                  "accepted: \"cLibrary\", "
                                  "\"unwindPersonality\", "
                                  "\"systemPrimitives\"", roleText));
                    continue;
                }
                if (!row.contains("image") || !row.at("image").is_string()
                 || row.at("image").get<std::string>().empty()) {
                    coll.emit(DiagnosticCode::C_MissingField, path + "/image",
                              "requires a non-empty 'image' (the DLL / "
                              "shared-object / dylib identity that plays this "
                              "role)");
                    continue;
                }
                if (data.runtimeLibraries.imageForRole(*role).has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path + "/role",
                              std::format(
                                  "duplicate runtimeLibraries role '{}' — one "
                                  "role names exactly one image; two rows "
                                  "would make the resolution order the answer",
                                  roleText));
                    continue;
                }
                data.runtimeLibraries.bindings.push_back(
                    RuntimeLibraryBinding{*role,
                                          row.at("image").get<std::string>()});
            }
        }
    }

    // Resolve a block's declared `role` against the table above. Fails LOUD
    // (and returns false) on a missing/non-string/unknown role AND on a role
    // the table does not declare. The `at` path is the BLOCK's own JSON path so
    // the diagnostic points at the naming site, not at the table.
    //
    // ★ THE SECOND FAILURE MODE IS THE INTERESTING ONE and it is why this is a
    // shared helper rather than four copies: a block that names a role the
    // format never declared must NOT resolve to "some image". Before this
    // table, the equivalent fact was a literal spelled per consumer, and
    // `src/mir/merge/synth_seh_funclets.cpp` proved what that costs — an
    // `msvcrt.dll` string in shared MIR substrate that no format could
    // override and no test could see.
    auto resolveRuntimeRole =
        [&](nlohmann::json const& block, std::string const& blockPath,
            std::string& outImage) -> bool {
            std::string const rolePath = blockPath + "/role";
            if (!block.contains("role") || !block.at("role").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField, rolePath,
                          "requires a non-empty string 'role' naming a row of "
                          "this format's top-level 'runtimeLibraries' table "
                          "(\"cLibrary\" / \"unwindPersonality\" / "
                          "\"systemPrimitives\") — a literal image path here "
                          "would be a second owner of a fact the role table "
                          "owns");
                return false;
            }
            auto const roleText = block.at("role").get<std::string>();
            auto const role = runtimeLibraryRoleFromName(roleText);
            if (!role.has_value() || *role == RuntimeLibraryRole::None) {
                coll.emit(DiagnosticCode::C_MalformedJson, rolePath,
                          std::format("unknown runtime-library role '{}' — "
                                      "accepted: \"cLibrary\", "
                                      "\"unwindPersonality\", "
                                      "\"systemPrimitives\"", roleText));
                return false;
            }
            auto const image = data.runtimeLibraries.imageForRole(*role);
            if (!image.has_value()) {
                std::string declared;
                for (auto const& b : data.runtimeLibraries.bindings) {
                    if (!declared.empty()) declared += ", ";
                    declared += runtimeLibraryRoleName(b.role);
                    declared += " -> ";
                    declared += b.image;
                }
                if (declared.empty()) declared = "<none>";
                coll.emit(DiagnosticCode::C_MissingField, rolePath,
                          std::format(
                              "names runtime-library role '{}', but this "
                              "format's 'runtimeLibraries' table does not "
                              "declare it (declared: {}). Add the row — a "
                              "role that resolves to nothing would leave this "
                              "block's import bound to no image.",
                              roleText, declared));
                return false;
            }
            outImage = std::string{*image};
            return true;
        };

    // ── UCRT-P4: `sehPersonality` — the unwinder-personality declaration ──
    //
    // `{"role": <runtimeLibraries role>, "mangledName": <non-empty>}`. Both
    // halves REQUIRED when the block is present: a personality with no name has
    // no handler to point unwind info at, and one with no role has no image to
    // import it from. OPTIONAL as a block — a format that declares none FAILS
    // LOUD only when a guarded region actually resolves, which is what an
    // ELF/Mach-O build carrying `__try` should get instead of an msvcrt import.
    if (doc.contains("sehPersonality")) {
        if (!doc.at("sehPersonality").is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/sehPersonality",
                      "'sehPersonality' must be an object { \"role\": ..., "
                      "\"mangledName\": ... }");
        } else {
            auto const& sp = doc.at("sehPersonality");
            static constexpr std::array<std::string_view, 2>
                kSehPersonalityKeys{"role", "mangledName"};
            DSS_CHECK_KEY_VOCABULARY(kSehPersonalityKeys);
            rejectUnknownKeys(sp, kSehPersonalityKeys, "/sehPersonality",
                              "the 'sehPersonality' block", coll);
            SehPersonality out;
            bool ok = resolveRuntimeRole(sp, "/sehPersonality",
                                         out.libraryPath);
            if (ok) {
                out.role = *runtimeLibraryRoleFromName(
                    sp.at("role").get<std::string>());
            }
            if (!sp.contains("mangledName")
             || !sp.at("mangledName").is_string()
             || sp.at("mangledName").get<std::string>().empty()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/sehPersonality/mangledName",
                          "requires a non-empty 'mangledName' — the routine an "
                          "emitted unwind record names as its handler "
                          "(pe: \"__C_specific_handler\")");
                ok = false;
            } else {
                out.mangledName = sp.at("mangledName").get<std::string>();
            }
            if (ok) data.sehPersonality = std::move(out);
        }
    }

    // ── UCRT-P4: `entryVerbs` — the REALIZED materialization-verb set ────
    //
    // An ARRAY of closed-vocabulary verb names: `["none", "argc-argv"]`.
    //
    // ★★★ THIS KEY USED TO BE `entryShapes`, AN ARRAY OF FULL
    // `{returns, params, materialization}` ROWS, and shrinking it to verbs is the
    // point rather than a simplification. The accepted entry SIGNATURES are a
    // SOURCE-LANGUAGE fact — `int wmain(int, wchar_t**)` is C's spelling, not the
    // loader's — so they now live in their one owner, the language's
    // `DeclarationRule::entryFunctions` mapping, which also carries each
    // signature's verb. What a FORMAT alone can answer is what it can actually
    // hand an entry, and that is exactly this set. Candidate selection
    // INTERSECTS the two, which is why `argc-wargv` appearing here only on
    // pe64-x86_64-windows-exec is the whole mechanism by which `wmain` is a
    // program entry on Windows and nowhere else.
    //
    // ⚠ THE OLD KEY IS REJECTED BY NAME (see the strict top-level key
    // vocabulary): a format file still carrying `entryShapes` fails loud instead
    // of loading with an EMPTY verb set — which would silently make every program
    // entry-less on that format, or, on a non-exec format, silently pass the
    // exec-flavor pairing rule. A retired owner must never be merely ignored.
    //
    // Duplicate verbs are refused rather than deduped silently: a set with a
    // repeated member is an authoring mistake, and this loader has no reason to
    // guess which of two identical declarations was meant.
    if (doc.contains("entryVerbs")) {
        if (!doc.at("entryVerbs").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/entryVerbs",
                      "'entryVerbs' must be an ARRAY of materialization verb "
                      "names (accepted: \"none\", \"argc-argv\", \"argc-wargv\")");
        } else {
            auto const& arr = doc.at("entryVerbs");
            std::size_t i = 0;
            for (auto const& row : arr) {
                auto const path = std::format("/entryVerbs/{}", i);
                ++i;
                if (!row.is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "each entryVerbs entry must be a verb NAME string "
                              "(accepted: \"none\", \"argc-argv\", "
                              "\"argc-wargv\"). Full { returns, params, ... } "
                              "rows belong to the source language's "
                              "`entryFunctions` mapping, which is the single "
                              "owner of an entry SIGNATURE.");
                    continue;
                }
                auto const spelling = row.get<std::string>();
                auto const v = entryMaterializationFromName(spelling);
                if (!v.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              std::format(
                                  "unknown entry materialization verb '{}' — "
                                  "accepted: \"none\", \"argc-argv\", "
                                  "\"argc-wargv\"", spelling));
                    continue;
                }
                bool dupe = false;
                for (auto const prior : data.entryVerbs) {
                    if (prior != *v) continue;
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              std::format(
                                  "duplicate entry verb '{}' — this is a SET, and "
                                  "a repeated member is an authoring mistake this "
                                  "loader will not silently absorb.", spelling));
                    dupe = true;
                    break;
                }
                if (!dupe) data.entryVerbs.push_back(*v);
            }
        }
    }

    // D-CSUBSET-C11-THREADS-MACHO: `librarySynthesis` block — the format's
    // compiler-synthesized-shim vehicle (the kernel32 vs pthread primitive
    // family a synthesized shipped-library shim, today C11 <threads.h>,
    // emits over) + the native library its on-demand helpers import from.
    // Optional in the JSON: ELF omits it (glibc exports the C11 thread API
    // directly → the synth recipe map is empty on elf → clean no-op). A
    // PRESENT block is strict (closed verb set + non-empty path); a format
    // that carries synthesize-tagged threads symbols yet declares NO block
    // fails loud in `synthesizeThreadsShim` (never a silently-assumed
    // vehicle) — the same fail-loud-on-absence discipline as tlsAccess.
    if (doc.contains("librarySynthesis")) {
        auto const& ls = doc.at("librarySynthesis");
        if (!ls.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/librarySynthesis",
                      // ⚠ THIS MESSAGE USED TO ADVERTISE `libraryPath`, WHICH
                      // IS NOT A KEY — it is the value RESOLVED from `role`
                      // against the top-level `runtimeLibraries` table (UCRT-P4
                      // moved the image behind the role table so no block spells
                      // a path). Harmless while unknown keys were ignored;
                      // actively wrong now that they are refused, because an
                      // author following it writes a key the loader rejects.
                      "'librarySynthesis' must be an object { \"vehicle\": "
                      "\"win32\"|\"pthread\", \"role\": \"…\" } — 'role' names a "
                      "row of this format's 'runtimeLibraries' table; the image "
                      "path is resolved from it, never spelled here");
        } else {
            LibrarySynthesis info{};
            bool ok = true;
            // `role` is consumed by `resolveRuntimeRole`, not by a `ls.at(...)`
            // in this arm — which is exactly why it has to be listed HERE and
            // could not be inferred by reading the block's own statements.
            static constexpr std::array<std::string_view, 2>
                kLibrarySynthesisKeys{"vehicle", "role"};
            DSS_CHECK_KEY_VOCABULARY(kLibrarySynthesisKeys);
            rejectUnknownKeys(ls, kLibrarySynthesisKeys, "/librarySynthesis",
                              "the 'librarySynthesis' block", coll);
            if (!ls.contains("vehicle") || !ls.at("vehicle").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/librarySynthesis/vehicle",
                          "'librarySynthesis.vehicle' is required and must be "
                          "a string — accepted: \"win32\" (kernel32 "
                          "CRITICAL_SECTION/CONDITION_VARIABLE/Fls*), "
                          "\"pthread\" (POSIX pthread_* / Darwin libSystem)");
                ok = false;
            } else {
                auto const s = ls.at("vehicle").get<std::string>();
                auto const v = librarySynthVehicleFromName(s);
                if (!v.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/librarySynthesis/vehicle",
                              std::format("unknown librarySynthesis vehicle "
                                          "'{}' — accepted: \"win32\", "
                                          "\"pthread\"",
                                          s));
                    ok = false;
                } else {
                    info.vehicle = *v;
                }
            }
            // UCRT-P4: the native library the synthesized shim's helpers import
            // from is named by ROLE, not spelled as a path here. On pe that role
            // is `systemPrimitives` (kernel32.dll — OS primitives, NOT the C
            // library); on Mach-O the same image serves both roles, so the
            // format points `cLibrary` at it and names that. THAT ASYMMETRY IS
            // THE ARGUMENT FOR A ROLE TABLE: a single per-format CRT string
            // could not express "the synth vehicle's image is the C library
            // here and is not the C library there".
            if (!resolveRuntimeRole(ls, "/librarySynthesis",
                                    info.libraryPath)) {
                ok = false;
            } else {
                info.role = *runtimeLibraryRoleFromName(
                    ls.at("role").get<std::string>());
            }
            if (ok) data.librarySynthesis = info;
        }
    }

    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: `stackReserveControl` block —
    // WHETHER this format can carry a per-PROGRAM stack reserve, and (if so)
    // WHERE it lands + the legal request range. PRESENCE is the capability:
    // the linker gate asks `stackReserveControl().has_value()`, never a
    // format identity, which is what lets the PE **exec** format declare it
    // while the PE **dll** (same kind, same header struct, but the loader
    // ignores the field) declares nothing. A PRESENT block is strict — the
    // vehicle is a closed verb and all three bounds are REQUIRED, because a
    // capability that does not state its own legal range cannot validate a
    // request and a defaulted range would silently admit an absurd value.
    if (doc.contains("stackReserveControl")) {
        auto const& sr = doc.at("stackReserveControl");
        if (!sr.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/stackReserveControl",
                      "'stackReserveControl' must be an object { \"vehicle\": "
                      "\"pe-optional-header\", \"minimumBytes\": N, "
                      "\"maximumBytes\": N, \"granularityBytes\": N }");
        } else {
            StackReserveControl info{};
            bool ok = true;
            // All four REQUIRED, so a typo already surfaced as a missing-key
            // diagnostic — naming the absent key rather than the present typo,
            // and leaving `"minimumByte"` reading as "the author forgot the
            // bound" when they did not.
            static constexpr std::array<std::string_view, 4>
                kStackReserveControlKeys{"vehicle", "minimumBytes",
                                         "maximumBytes", "granularityBytes"};
            DSS_CHECK_KEY_VOCABULARY(kStackReserveControlKeys);
            rejectUnknownKeys(sr, kStackReserveControlKeys,
                              "/stackReserveControl",
                              "the 'stackReserveControl' block", coll);
            if (!sr.contains("vehicle") || !sr.at("vehicle").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/stackReserveControl/vehicle",
                          "'stackReserveControl.vehicle' is required and must "
                          "be a string — accepted: \"pe-optional-header\" "
                          "(IMAGE_OPTIONAL_HEADER64.SizeOfStackReserve)");
                ok = false;
            } else {
                auto const s = sr.at("vehicle").get<std::string>();
                auto const v = stackReserveVehicleFromName(s);
                if (!v.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/stackReserveControl/vehicle",
                              std::format("unknown stackReserveControl vehicle "
                                          "'{}' — accepted: "
                                          "\"pe-optional-header\". A vehicle "
                                          "ships only WITH the walker arm that "
                                          "writes it, so a format can never "
                                          "declare a reserve no walker emits.",
                                          s));
                    ok = false;
                } else {
                    info.vehicle = *v;
                }
            }
            // The three REQUIRED bounds. `is_number_unsigned` rejects a
            // negative and a float in one check (nlohmann types `-1` as a
            // SIGNED integer and `1.5` as a float), so a `"minimumBytes": -1`
            // can never wrap into a huge u64.
            auto const readBound = [&](char const* key, std::uint64_t& out) {
                auto const ptr = std::string{"/stackReserveControl/"} + key;
                if (!sr.contains(key)) {
                    coll.emit(DiagnosticCode::C_MissingField, ptr,
                              std::format("'stackReserveControl.{}' is required "
                                          "— a declared capability must state "
                                          "its own legal request range", key));
                    ok = false;
                    return;
                }
                if (!sr.at(key).is_number_unsigned()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, ptr,
                              std::format("'stackReserveControl.{}' must be a "
                                          "non-negative integer byte count",
                                          key));
                    ok = false;
                    return;
                }
                out = sr.at(key).get<std::uint64_t>();
            };
            readBound("minimumBytes",     info.minimumBytes);
            readBound("maximumBytes",     info.maximumBytes);
            readBound("granularityBytes", info.granularityBytes);

            // Internal coherence — a range that cannot admit ANY value, or a
            // zero granularity (a modulo-by-zero at the request gate), is
            // dead config that would fail every request for the wrong reason.
            if (ok && info.granularityBytes == 0) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/stackReserveControl/granularityBytes",
                          "'granularityBytes' must be > 0 (a request is "
                          "alignment-checked against it; 0 admits nothing)");
                ok = false;
            }
            if (ok && info.minimumBytes == 0) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/stackReserveControl/minimumBytes",
                          "'minimumBytes' must be > 0 (a zero-byte stack "
                          "reserve cannot start a program)");
                ok = false;
            }
            if (ok && info.maximumBytes < info.minimumBytes) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/stackReserveControl/maximumBytes",
                          std::format("'maximumBytes' ({}) must be >= "
                                      "'minimumBytes' ({}) — the declared "
                                      "range admits no value",
                                      info.maximumBytes, info.minimumBytes));
                ok = false;
            }
            if (ok && (info.minimumBytes % info.granularityBytes) != 0) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/stackReserveControl/minimumBytes",
                          std::format("'minimumBytes' ({}) must itself be a "
                                      "multiple of 'granularityBytes' ({}) — "
                                      "otherwise the smallest legal request is "
                                      "unreachable",
                                      info.minimumBytes,
                                      info.granularityBytes));
                ok = false;
            }
            // Vehicle ↔ implementing-walker coherence. A vehicle NAMES a
            // structure of one image format, so declaring `pe-optional-header`
            // on an ELF schema is dead config whose request the ELF walker
            // would silently drop — the exact silent-drop this capability
            // exists to prevent.
            //
            // ★★ WAS `kVehicleKinds[]`, a table pairing each vehicle with the
            // `ObjectFormatKind` allowed to declare it, DEFENDED IN THIS FILE
            // as *"Expressed as a TABLE … not an if-chain: config vocabulary
            // checked against config vocabulary, evaluated generically."* That
            // defence was struck in TF-C125 along with its twin in
            // `core/types/object_format_kind.hpp`, because it is wrong: a table
            // keyed on format identity is the same dependency as an `if` on
            // format identity — the consumer still has to know an identity to
            // get an answer — and it is why `kCManglingRules` was DELETED in
            // TF-C122 rather than moved. Left standing, the sentence would have
            // re-authorized the pattern for the next reader; that is what a
            // rationalization in the tree does.
            //
            // The rule is unchanged in what it rejects and in what it says. The
            // pairing now comes from the backend that IMPLEMENTS the vehicle
            // (`stackReserveVehicles()`), so the loader asks "does anyone
            // implement this, and is it you?" instead of consulting a table
            // that named the owner. A second vehicle lands with its walker arm
            // and this loop covers it untouched.
            if (ok) {
                link::ObjectFormatBackend const* implementer = nullptr;
                for (auto const* candidate : link::objectFormatBackendTable()) {
                    for (StackReserveVehicle v :
                             candidate->stackReserveVehicles()) {
                        if (v == info.vehicle) { implementer = candidate; break; }
                    }
                    if (implementer != nullptr) break;
                }
                if (implementer != backend) {
                    // ⚠ `implementer == nullptr` — no walker implements the
                    // vehicle at all — is a NEW refusal, not the old one. An
                    // earlier draft of this comment called it "the same
                    // refusal"; an independent audit measured otherwise and it
                    // was wrong. The old `kVehicleKinds` loop emitted only when
                    // a ROW MATCHED, so a vehicle with no row was silently
                    // ACCEPTED ON EVERY FORMAT. Rejecting it is strictly better
                    // — such a request would be dropped by every walker, which
                    // is the silent-drop this capability exists to prevent —
                    // but it is a behaviour change and is recorded as one.
                    // Latent today: `StackReserveVehicle` has one enumerator
                    // and one implementer, so no reachable input differs.
                    // (`stackReserveVehicleFromName` already rejected an
                    // unknown spelling upstream, so reaching here means a
                    // vehicle whose enum row shipped ahead of its walker arm.)
                    coll.emit(
                        DiagnosticCode::C_MalformedJson,
                        "/stackReserveControl/vehicle",
                        std::format(
                            "stackReserveControl vehicle '{}' names a "
                            "structure of the '{}' image format, but this "
                            "schema declares kind '{}'. The '{}' walker "
                            "would silently DROP a stack-reserve request "
                            "routed to it. Fix the vehicle or the "
                            "format.kind. D-SQLITE-PE64-FULL-TIER-STACK-"
                            "DEPTH.",
                            stackReserveVehicleName(info.vehicle),
                            implementer != nullptr
                                ? std::string{implementer->configName()}
                                : std::string{"<no walker implements it>"},
                            backend->configName(),
                            backend->configName()));
                    ok = false;
                }
            }
            if (ok) data.stackReserveControl = info;
        }
    }

    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: `stackReserveUnsupportedReason` —
    // the REMEDY axis. Not a second capability: it declares WHY this format
    // cannot carry a reserve, so the refusal can tell the user where the
    // property actually lives for THIS artifact (a PE dll's inert header
    // field, a relocatable object's absent one, an ELF `ulimit`, an
    // unimplemented Mach-O walker arm). Optional — a format that declares
    // neither key still REFUSES a request, just with the generic message.
    if (doc.contains("stackReserveUnsupportedReason")) {
        auto const& sru = doc.at("stackReserveUnsupportedReason");
        if (!sru.is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/stackReserveUnsupportedReason",
                      "'stackReserveUnsupportedReason' must be a string — "
                      "accepted: \"runtime-controlled\", "
                      "\"loader-ignores-field\", \"no-image-field\", "
                      "\"walker-not-implemented\"");
        } else if (data.stackReserveControl.has_value()) {
            // Contradictory config: a format cannot both DECLARE the
            // capability and explain why it lacks it. Reject rather than pick
            // one silently — a schema in this state means the author changed
            // their mind and left the losing key behind, and whichever the
            // engine ignored would be a lie sitting in the config.
            coll.emit(DiagnosticCode::C_ConflictingField,
                      "/stackReserveUnsupportedReason",
                      "a format must not declare BOTH 'stackReserveControl' "
                      "(it CAN carry a stack reserve) and "
                      "'stackReserveUnsupportedReason' (why it CANNOT) — the "
                      "two are mutually exclusive. Delete whichever no longer "
                      "applies. D-SQLITE-PE64-FULL-TIER-STACK-DEPTH.");
        } else {
            auto const s = sru.get<std::string>();
            auto const r = stackReserveUnsupportedReasonFromName(s);
            if (!r.has_value()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/stackReserveUnsupportedReason",
                          std::format("unknown stackReserveUnsupportedReason "
                                      "'{}' — accepted: "
                                      "\"runtime-controlled\", "
                                      "\"loader-ignores-field\", "
                                      "\"no-image-field\", "
                                      "\"walker-not-implemented\"",
                                      s));
            } else {
                data.stackReserveUnsupportedReason = *r;
            }
        }
    }

    // D-LK10-ENTRY Slice B: `processExit` block. Two arms keyed on
    // `mechanism`:
    //   "syscall"        requires syscallNumber (u32) +
    //                    syscallNumGpr (string) +
    //                    syscallOpcodeBytes (array of u8, non-empty).
    //   "by-name-import" requires importLibraryPath (string,
    //                    non-empty) + importMangledName (string,
    //                    non-empty).
    if (doc.contains("processExit")) {
        if (!doc.at("processExit").is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/processExit",
                      "'processExit' must be an object");
        } else {
            auto const& pe = doc.at("processExit");
            ProcessExit out;
            bool armOk = true;
            if (!pe.contains("mechanism")
             || !pe.at("mechanism").is_string()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/processExit/mechanism",
                          "'processExit.mechanism' must be a string "
                          "(\"syscall\" or \"by-name-import\")");
                armOk = false;
            } else {
                auto const mechName =
                    pe.at("mechanism").get<std::string>();
                auto const m = exitMechanismFromName(mechName);
                if (!m.has_value() || *m == ExitMechanism::None) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/processExit/mechanism",
                              std::format("unknown processExit.mechanism"
                                          " '{}' — accepted: "
                                          "\"syscall\", \"by-name-"
                                          "import\"", mechName));
                    armOk = false;
                } else {
                    out.mechanism = *m;
                    // ── PER-ARM typo discriminator ──────────────────────
                    //
                    // `mechanism` is the only key BOTH arms read. The syscall
                    // arm reads three fields the by-name-import arm never
                    // looks at, and vice versa, so the allowed set is
                    // arm-dependent and a union would accept inert config.
                    // Neither arm's set is empty, so both directions of the
                    // cross-arm message are reachable.
                    static constexpr std::array<std::string_view, 1>
                        kProcessExitCommonKeys{"mechanism"};
                    static constexpr std::array<std::string_view, 3>
                        kProcessExitSyscallKeys{"syscallNumber",
                                                "syscallNumGpr",
                                                "syscallOpcodeBytes"};
                    static constexpr std::array<std::string_view, 2>
                        kProcessExitImportKeys{"role", "importMangledName"};
                    DSS_CHECK_KEY_VOCABULARY(kProcessExitCommonKeys);
                    DSS_CHECK_KEY_VOCABULARY(kProcessExitSyscallKeys);
                    DSS_CHECK_KEY_VOCABULARY(kProcessExitImportKeys);
                    {
                        bool const isSyscall =
                            out.mechanism == ExitMechanism::Syscall;
                        std::vector<std::string_view> allowedHere{
                            kProcessExitCommonKeys.begin(),
                            kProcessExitCommonKeys.end()};
                        auto const own = isSyscall
                            ? std::span<std::string_view const>{kProcessExitSyscallKeys}
                            : std::span<std::string_view const>{kProcessExitImportKeys};
                        auto const other = isSyscall
                            ? std::span<std::string_view const>{kProcessExitImportKeys}
                            : std::span<std::string_view const>{kProcessExitSyscallKeys};
                        allowedHere.insert(allowedHere.end(),
                                           own.begin(), own.end());
                        std::string_view const otherName =
                            isSyscall ? exitMechanismName(ExitMechanism::ByNameImport)
                                      : exitMechanismName(ExitMechanism::Syscall);
                        rejectUnknownArmKeys(
                            pe, allowedHere, "/processExit",
                            "the 'processExit' block",
                            exitMechanismName(out.mechanism),
                            [&](std::string_view key) -> std::string_view {
                                for (auto const& k : other) {
                                    if (key == k) return otherName;
                                }
                                return {};
                            },
                            coll);
                    }
                    // simplifier FOLD-NOW #1 (7425905 audit fold):
                    // collapse the 3 repeated `!contains || !is_string
                    // || .empty()` patterns into one lambda. Behavior
                    // unchanged; diagnostic text preserved.
                    auto requireNonEmptyString =
                        [&](char const* field, std::string& out) -> bool {
                            std::string const path =
                                std::string{"/processExit/"} + field;
                            if (!pe.contains(field)
                             || !pe.at(field).is_string()
                             || pe.at(field).get<std::string>().empty()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          path,
                                          std::format(
                                              "requires non-empty '{}' "
                                              "(string)", field));
                                return false;
                            }
                            out = pe.at(field).get<std::string>();
                            return true;
                        };
                    if (out.mechanism == ExitMechanism::Syscall) {
                        if (!pe.contains("syscallNumber")
                         || !pe.at("syscallNumber").is_number_unsigned()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/processExit/syscallNumber",
                                      "syscall arm requires "
                                      "'syscallNumber' (u32)");
                            armOk = false;
                        } else {
                            out.syscallNumber =
                                pe.at("syscallNumber").get<std::uint32_t>();
                        }
                        if (!requireNonEmptyString("syscallNumGpr",
                                                    out.syscallNumGpr)) {
                            armOk = false;
                        }
                        if (!pe.contains("syscallOpcodeBytes")
                         || !pe.at("syscallOpcodeBytes").is_array()
                         || pe.at("syscallOpcodeBytes").empty()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/processExit/syscallOpcodeBytes",
                                      "syscall arm requires non-empty "
                                      "'syscallOpcodeBytes' (array of u8)");
                            armOk = false;
                        } else {
                            auto const& arr = pe.at("syscallOpcodeBytes");
                            for (std::size_t bi = 0; bi < arr.size(); ++bi) {
                                if (!arr[bi].is_number_unsigned()
                                 || arr[bi].get<std::uint64_t>() > 0xFFu) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              std::format("/processExit/syscallOpcodeBytes/{}", bi),
                                              "each entry must be u8 (0..255)");
                                    armOk = false;
                                    continue;
                                }
                                out.syscallOpcodeBytes.push_back(
                                    static_cast<std::uint8_t>(
                                        arr[bi].get<std::uint32_t>()));
                            }
                        }
                    } else {  // ByNameImport
                        // UCRT-P4: the image comes from the ROLE table, never a
                        // path spelled here. `role` is recorded alongside the
                        // resolved path so `validate()` can re-check the pair —
                        // the loader is not the only tier that enforces it.
                        if (!resolveRuntimeRole(pe, "/processExit",
                                                out.importLibraryPath)) {
                            armOk = false;
                        } else {
                            out.role = *runtimeLibraryRoleFromName(
                                pe.at("role").get<std::string>());
                        }
                        if (!requireNonEmptyString("importMangledName",
                                                    out.importMangledName)) {
                            armOk = false;
                        }
                    }
                }
            }
            if (armOk) {
                data.processExit = std::move(out);
            }
        }
    }

    // D-RUNTIME-MAIN-ARGC-ARGV (c88): `processArgs` block. One arm
    // keyed on `mechanism`:
    //   "stack-vector" requires argcStackOffset (u32) +
    //                  argvStackOffset (u32) — byte offsets from the
    //                  PROCESS-ENTRY stack pointer of the argc word
    //                  and the first argv slot. BOTH are explicit
    //                  (no silent defaults — a wrong offset reads
    //                  garbage argc, the exact failure this block
    //                  exists to close).
    if (doc.contains("processArgs")) {
        if (!doc.at("processArgs").is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/processArgs",
                      "'processArgs' must be an object");
        } else {
            auto const& pa = doc.at("processArgs");
            ProcessArgs out;
            bool armOk = true;
            if (!pa.contains("mechanism")
             || !pa.at("mechanism").is_string()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/processArgs/mechanism",
                          // Both verbs, matching the unknown-value message
                          // just below. Naming only one made the other read
                          // as unsupported.
                          "'processArgs.mechanism' must be a string "
                          "(\"stack-vector\" or \"crt-argv-accessors\")");
                armOk = false;
            } else {
                auto const mechName =
                    pa.at("mechanism").get<std::string>();
                auto const m = argsMechanismFromName(mechName);
                if (!m.has_value() || *m == ArgsMechanism::None) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/processArgs/mechanism",
                              std::format("unknown processArgs.mechanism"
                                          " '{}' — accepted: "
                                          "\"stack-vector\", "
                                          "\"crt-argv-accessors\"", mechName));
                    armOk = false;
                } else {
                    out.mechanism = *m;
                    // ── PER-ARM typo discriminator ──────────────────────
                    //
                    // Same rule as `processExit`: `mechanism` is the only key
                    // both arms read, and the stack-vector arm's two offsets
                    // are meaningless to the CRT arm (and its eight fields
                    // meaningless to stack-vector). `ArgsMechanism` has
                    // exactly three enumerators and `none` is refused above,
                    // so these two arms are exhaustive.
                    static constexpr std::array<std::string_view, 1>
                        kProcessArgsCommonKeys{"mechanism"};
                    static constexpr std::array<std::string_view, 2>
                        kProcessArgsStackVectorKeys{"argcStackOffset",
                                                    "argvStackOffset"};
                    static constexpr std::array<std::string_view, 8>
                        kProcessArgsCrtKeys{"role", "configureNarrowArgvFn",
                                            "configureWideArgvFn",
                                            "argcAccessorFn",
                                            "narrowArgvAccessorFn",
                                            "wideArgvAccessorFn", "argvMode",
                                            "argvUnavailableExitStatus"};
                    DSS_CHECK_KEY_VOCABULARY(kProcessArgsCommonKeys);
                    DSS_CHECK_KEY_VOCABULARY(kProcessArgsStackVectorKeys);
                    DSS_CHECK_KEY_VOCABULARY(kProcessArgsCrtKeys);
                    {
                        bool const isStackVector =
                            out.mechanism == ArgsMechanism::StackVector;
                        std::vector<std::string_view> allowedHere{
                            kProcessArgsCommonKeys.begin(),
                            kProcessArgsCommonKeys.end()};
                        auto const own = isStackVector
                            ? std::span<std::string_view const>{kProcessArgsStackVectorKeys}
                            : std::span<std::string_view const>{kProcessArgsCrtKeys};
                        auto const other = isStackVector
                            ? std::span<std::string_view const>{kProcessArgsCrtKeys}
                            : std::span<std::string_view const>{kProcessArgsStackVectorKeys};
                        allowedHere.insert(allowedHere.end(),
                                           own.begin(), own.end());
                        std::string_view const otherName = isStackVector
                            ? argsMechanismName(ArgsMechanism::CrtArgvAccessors)
                            : argsMechanismName(ArgsMechanism::StackVector);
                        rejectUnknownArmKeys(
                            pa, allowedHere, "/processArgs",
                            "the 'processArgs' block",
                            argsMechanismName(out.mechanism),
                            [&](std::string_view key) -> std::string_view {
                                for (auto const& k : other) {
                                    if (key == k) return otherName;
                                }
                                return {};
                            },
                            coll);
                    }
                    if (*m == ArgsMechanism::StackVector) {
                        // StackVector arm: both offsets explicit u32,
                        // bounded to int32 (they feed a MemOffset LIR
                        // operand, an int32 displacement).
                        auto requireOffset =
                            [&](char const* field, std::uint32_t& dst) -> bool {
                                std::string const path =
                                    std::string{"/processArgs/"} + field;
                                if (!pa.contains(field)
                                 || !pa.at(field).is_number_unsigned()) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              path,
                                              std::format(
                                                  "stack-vector arm requires "
                                                  "'{}' (u32 byte offset from "
                                                  "the process-entry stack "
                                                  "pointer)", field));
                                    return false;
                                }
                                auto const v =
                                    pa.at(field).get<std::uint64_t>();
                                if (v > 0x7FFFFFFFull) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              path,
                                              std::format(
                                                  "'{}' = {} exceeds the "
                                                  "int32 displacement range "
                                                  "the trampoline's memory "
                                                  "operand carries", field, v));
                                    return false;
                                }
                                dst = static_cast<std::uint32_t>(v);
                                return true;
                            };
                        if (!requireOffset("argcStackOffset",
                                           out.argcStackOffset)) {
                            armOk = false;
                        }
                        if (!requireOffset("argvStackOffset",
                                           out.argvStackOffset)) {
                            armOk = false;
                        }
                    } else {
                        out.mechanism = *m;
                        // Shared by both CRT arms: a per-field non-empty-string
                        // requirement whose diagnostic names the ARM, so a config
                        // author who mixed two arms' field sets is told which one
                        // they are in.
                        auto requireStr =
                            [&](char const* field, std::string& dst) -> bool {
                                std::string const path =
                                    std::string{"/processArgs/"} + field;
                                if (!pa.contains(field)
                                 || !pa.at(field).is_string()
                                 || pa.at(field).get<std::string>().empty()) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              path,
                                              std::format(
                                                  "{} arm requires a non-empty "
                                                  "string '{}'",
                                                  argsMechanismName(*m), field));
                                    return false;
                                }
                                dst = pa.at(field).get<std::string>();
                                return true;
                            };
                        if (*m == ArgsMechanism::CrtArgvAccessors) {
                            // CrtArgvAccessors arm (UCRT-P4): the UCRT populate
                            // call + the three address-returning accessors, plus
                            // the two integers. The library comes from the ROLE
                            // table, never a path here.
                            if (!resolveRuntimeRole(pa, "/processArgs",
                                                    out.crtLibraryPath)) {
                                armOk = false;
                            } else {
                                out.role = *runtimeLibraryRoleFromName(
                                    pa.at("role").get<std::string>());
                            }
                            if (!requireStr("configureNarrowArgvFn",
                                            out.configureNarrowArgvFn)) {
                                armOk = false;
                            }
                            if (!requireStr("configureWideArgvFn",
                                            out.configureWideArgvFn)) {
                                armOk = false;
                            }
                            if (!requireStr("argcAccessorFn",
                                            out.argcAccessorFn)) {
                                armOk = false;
                            }
                            if (!requireStr("narrowArgvAccessorFn",
                                            out.narrowArgvAccessorFn)) {
                                armOk = false;
                            }
                            if (!requireStr("wideArgvAccessorFn",
                                            out.wideArgvAccessorFn)) {
                                armOk = false;
                            }
                            // `argvMode` — CONSTRAINED to a declared member of the
                            // `_crt_argv_mode` enum (0..2). ★ MEASURED 2026-08-10:
                            // mode 7 does NOT return an error — the process dies at
                            // 0xC0000409 printing nothing. So the only tier that can
                            // catch a fat-fingered mode is THIS one, and it must
                            // refuse rather than clamp.
                            if (!pa.contains("argvMode")
                             || !pa.at("argvMode").is_number_unsigned()) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          "/processArgs/argvMode",
                                          "crt-argv-accessors arm requires "
                                          "'argvMode' (the `_crt_argv_mode` value "
                                          "handed to the configure call: 0 = no "
                                          "argv, 1 = argv unexpanded, 2 = argv with "
                                          "wildcard expansion)");
                                armOk = false;
                            } else {
                                auto const v =
                                    pa.at("argvMode").get<std::uint64_t>();
                                if (v > 2u) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              "/processArgs/argvMode",
                                              std::format(
                                                  "'argvMode' = {} is outside the "
                                                  "declared `_crt_argv_mode` enum "
                                                  "(0..2). MEASURED: a value "
                                                  "outside the enum does not return "
                                                  "an error — the process dies at "
                                                  "0xC0000409 printing nothing, so "
                                                  "load time is the only tier that "
                                                  "can refuse it.", v));
                                    armOk = false;
                                } else {
                                    out.argvMode =
                                        static_cast<std::uint32_t>(v);
                                }
                            }
                            // `argvUnavailableExitStatus` — REQUIRED and must be
                            // NON-ZERO. Zero is the C success status, so a zero here
                            // would make the "argv came back NULL" path
                            // indistinguishable from a program that ran and
                            // succeeded — a guard that reports nothing.
                            if (!pa.contains("argvUnavailableExitStatus")
                             || !pa.at("argvUnavailableExitStatus")
                                     .is_number_integer()) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          "/processArgs/argvUnavailableExitStatus",
                                          "crt-argv-accessors arm requires "
                                          "'argvUnavailableExitStatus' (the status "
                                          "the synthesized init RETURNS when the "
                                          "CRT populate call produced no argv — the "
                                          "errno_t the configure call returns "
                                          "CANNOT distinguish that case, MEASURED)");
                                armOk = false;
                            } else {
                                auto const v = pa.at("argvUnavailableExitStatus")
                                                   .get<std::int64_t>();
                                if (v == 0 || v < -2147483648LL
                                 || v > 2147483647LL) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              "/processArgs/"
                                              "argvUnavailableExitStatus",
                                              std::format(
                                                  "'argvUnavailableExitStatus' = {} "
                                                  "must be a NON-ZERO i32 — zero is "
                                                  "C's success status, so a zero "
                                                  "here makes the no-argv failure "
                                                  "indistinguishable from a "
                                                  "successful run", v));
                                    armOk = false;
                                } else {
                                    out.argvUnavailableExitStatus =
                                        static_cast<std::int32_t>(v);
                                }
                            }
                        } else {
                            // Closed-enum discipline: a new ArgsMechanism member
                            // must add its own field-set arm HERE. Falling through
                            // would accept the block and leave every field empty.
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/processArgs/mechanism",
                                      std::format(
                                          "processArgs.mechanism '{}' is a declared "
                                          "vocabulary member but this loader has no "
                                          "field-set arm for it — the block would "
                                          "load with every field EMPTY",
                                          argsMechanismName(*m)));
                            armOk = false;
                        }
                    }
                }
            }
            if (armOk) {
                data.processArgs = out;
            }
        }
    }

    // ── UCRT-P4: NO `runtimeLibraries` ROW MAY SIT UNNAMED ────────────────
    //
    // The reverse half of the role-table biconditional. `ObjectFormatData::
    // validate()` enforces that every block's role RESOLVES and matches; this
    // enforces that every declared row is actually NAMED by some block.
    //
    // ★ IT LIVES HERE, NOT IN validate(), FOR A MEASURED REASON. validate() sees
    // only blocks that LOADED, so a block rejected for an unrelated defect loses
    // its role claim and makes the row it named look inert — a second, misleading
    // diagnostic stacked on the real one (MEASURED: `LK10EntrySliceB.ByName-
    // ImportArmMissingMangledNameRejected` gained exactly that). Reading the
    // `role` keys straight out of the JSON captures the DECLARED intent whether or
    // not the block validated, so this answer cannot cascade.
    //
    // WHY REJECT AT ALL, rather than shrug at a spare row: an unnamed row is
    // exactly how a stale image survives a CRT migration unnoticed — nothing reads
    // it, so nothing contradicts it, and the mechanical "no pe table names
    // msvcrt.dll" exit criterion would be checking a value with no consumer. This
    // file already rejects inert config by name (see the `charSignedness` note at
    // the top); this is the same rule for the same reason.
    if (!data.runtimeLibraries.empty()) {
        // Which roles does ANY block name? Gathered from the document, generically
        // — the list of role-NAMING block keys is the only thing enumerated, and it
        // is the same four the parsers above read.
        std::vector<RuntimeLibraryRole> named;
        for (char const* blockKey : {"processExit", "processArgs",
                                     "sehPersonality", "librarySynthesis"}) {
            if (!doc.contains(blockKey) || !doc.at(blockKey).is_object()) continue;
            auto const& blk = doc.at(blockKey);
            if (!blk.contains("role") || !blk.at("role").is_string()) continue;
            if (auto const r =
                    runtimeLibraryRoleFromName(blk.at("role").get<std::string>())) {
                named.push_back(*r);
            }
        }
        for (auto const& b : data.runtimeLibraries.bindings) {
            if (std::find(named.begin(), named.end(), b.role) != named.end()) {
                continue;
            }
            coll.emit(DiagnosticCode::C_MalformedJson, "/runtimeLibraries",
                      std::format(
                          "declares runtime-library role '{}' -> '{}' that NO "
                          "block in this format names — inert config. A role-table "
                          "row is only meaningful because some spine block "
                          "resolves against it; an unnamed row is how a stale "
                          "image survives a CRT migration unnoticed, with nothing "
                          "reading it and therefore nothing contradicting it. "
                          "Remove the row, or name it from the block that needs "
                          "it. (D-FFI-PE-CRT-UCRT-MIGRATION.)",
                          runtimeLibraryRoleName(b.role), b.image));
        }
    }

    // D-LK2-RODATA closure — `supportedDataSections`. Optional
    // top-level array of `DataSectionKind` names ("rodata" / "data" /
    // "bss" / "tdata" / "tbss" — the last two per D-CSUBSET-THREAD-
    // LOCAL — plus "relro" per D-LK-RELRO-CONST-DATA-RELOCATABLE, c145)
    // the format's walker accepts on `AssembledModule.
    // dataItems`. Absent / empty = walker rejects all producer-data-
    // section items (the format-side validate() rule below also
    // gates this on isImageFlavor — relocatable .obj cannot declare
    // the capability since rodata in .obj rides through the symbol
    // table, not the dataItems pipeline). Cross-format agnosticism:
    // adding a fourth executable format that supports rodata = drop
    // `"supportedDataSections": ["rodata"]` into its JSON; zero C++
    // changes in the linker substrate.
    if (doc.contains("supportedDataSections")) {
        if (!doc.at("supportedDataSections").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/supportedDataSections",
                      "'supportedDataSections' must be an array of "
                      "DataSectionKind names (\"rodata\" / \"data\" / "
                      "\"bss\" / \"tdata\" / \"tbss\" / \"relro\")");
        } else {
            auto const& arr = doc.at("supportedDataSections");
            std::size_t i = 0;
            for (auto const& elem : arr) {
                auto const path =
                    std::format("/supportedDataSections/{}", i);
                if (!elem.is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "must be a string DataSectionKind name");
                } else {
                    auto const name = elem.get<std::string>();
                    auto const k = dataSectionKindFromName(name);
                    if (!k.has_value()) {
                        coll.emit(DiagnosticCode::C_MalformedJson, path,
                                  std::format("unknown DataSectionKind "
                                              "'{}' (expected 'rodata' "
                                              "/ 'data' / 'bss' / "
                                              "'tdata' / 'tbss' / 'relro')",
                                              name));
                    } else {
                        bool dup = false;
                        for (auto existing : data.supportedDataSections) {
                            if (existing == *k) { dup = true; break; }
                        }
                        if (dup) {
                            coll.emit(DiagnosticCode::C_MalformedJson, path,
                                      std::format("duplicate "
                                                  "DataSectionKind '{}' "
                                                  "in supportedDataSections",
                                                  name));
                        } else {
                            data.supportedDataSections.push_back(*k);
                        }
                    }
                }
                ++i;
            }
        }
    }

    // artifactProfiles ── which profiles this format SERVES (plan 06 AP3;
    // the format-side symmetric twin of the language's `artifactProfiles[]`,
    // AP1). Each entry is validated against the SHARED registered vocabulary
    // (`isRegisteredArtifactProfile`) — a typo like "clii" fails loud here
    // (`C_UnknownArtifactProfile`) at its source rather than silently
    // mis-serving downstream. Absent ⇒ empty ⇒ serves no profile
    // (fail-closed). Zero C++ changes per new format — pure config; the
    // driver gate is a generic set-membership, never a profile-name branch.
    if (doc.contains("artifactProfiles")) {
        if (!doc.at("artifactProfiles").is_array()) {
            coll.emit(DiagnosticCode::C_UnknownArtifactProfile,
                      "/artifactProfiles",
                      "'artifactProfiles' must be an array of profile names");
        } else {
            auto const& arr = doc.at("artifactProfiles");
            std::size_t i = 0;
            for (auto const& elem : arr) {
                auto const path = std::format("/artifactProfiles/{}", i);
                if (!elem.is_string()) {
                    coll.emit(DiagnosticCode::C_UnknownArtifactProfile, path,
                              "each 'artifactProfiles' entry must be a string");
                } else {
                    auto const name = elem.get<std::string>();
                    if (!isRegisteredArtifactProfile(name)) {
                        coll.emit(DiagnosticCode::C_UnknownArtifactProfile, path,
                                  std::format("unknown artifact profile '{}' "
                                              "(registered profiles: {})",
                                              name, registeredArtifactProfileList()));
                    } else {
                        bool dup = false;
                        for (auto const& existing : data.artifactProfiles) {
                            if (existing == name) { dup = true; break; }
                        }
                        if (dup) {
                            coll.emit(DiagnosticCode::C_ConflictingField, path,
                                      std::format("artifact profile '{}' declared "
                                                  "more than once in artifactProfiles",
                                                  name));
                        } else {
                            data.artifactProfiles.push_back(name);
                        }
                    }
                }
                ++i;
            }
        }
    }

    // relocations[] — substrate-tier; shares the cross-side
    // `relocation_table.hpp` substrate with TargetSchema so the
    // `{name, kind}` shape of plan 13 §2.6's reloc-taxonomy unifier
    // is identical-by-construction on both sides. The `nativeId`
    // field is the format's wire tag (e.g. ELF R_X86_64_PC32 = 2).
    substrate::loadRelocationsTable<ObjectFormatRelocationInfo>(
        doc, data.relocations, data.relocationNameIndex,
        data.relocationKindIndex, coll,
        [](nlohmann::json const& r, ObjectFormatRelocationInfo& info,
           Collector& c, std::size_t i) -> bool {
            if (!r.contains("nativeId") || !r.at("nativeId").is_number_integer()) {
                c.emit(DiagnosticCode::C_MissingField,
                       std::format("/relocations/{}/nativeId", i),
                       "missing or non-integer 'nativeId' (format-specific "
                       "wire tag, e.g. ELF R_X86_64_PC32 = 2)");
                return false;
            }
            std::int64_t const v = r.at("nativeId").get<std::int64_t>();
            if (v <= 0 || v > 0xFFFFFFFFLL) {
                c.emit(DiagnosticCode::C_MalformedJson,
                       std::format("/relocations/{}/nativeId", i),
                       std::format("'nativeId' ({}) must be in (0, 2^32)", v));
                return false;
            }
            info.nativeId = static_cast<std::uint32_t>(v);
            // D-LK-OBJECT-EXTERN-CALL-RELOCATABLE: optional PLT-variant
            // nativeId (e.g. R_X86_64_PLT32=4) emitted for an undefined-extern
            // call in a relocatable object. Absent → 0 (no PLT variant).
            if (r.contains("pltNativeId")) {
                if (!r.at("pltNativeId").is_number_integer()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/pltNativeId", i),
                           "'pltNativeId' must be an integer");
                    return false;
                }
                std::int64_t const pv =
                    r.at("pltNativeId").get<std::int64_t>();
                if (pv <= 0 || pv > 0xFFFFFFFFLL) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/pltNativeId", i),
                           std::format("'pltNativeId' ({}) must be in (0, 2^32)",
                                       pv));
                    return false;
                }
                info.pltNativeId = static_cast<std::uint32_t>(pv);
            }
            // D-LK-MACHO-ISDATA-NO-CALL-SIGNAL: the DECLARED call/branch role.
            // True iff this format's native wire relocation can only target
            // executable code, so an extern reached through it is PROVEN to be
            // a function. Absent -> false (the relocation proves nothing about
            // the target's class). The readers consume this INSTEAD of guessing
            // from the target's arithmetic formula -- see the field's own
            // comment in object_format_schema.hpp for why the formula is the
            // wrong vocabulary for the question.
            if (r.contains("isCall")) {
                if (!r.at("isCall").is_boolean()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/isCall", i),
                           "'isCall' must be a boolean");
                    return false;
                }
                info.isCall = r.at("isCall").get<bool>();
            }
            // D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS: an EMISSION ALIAS —
            // a row sharing its wire type with another row, present only so the
            // emitter can reach that wire type through a different DSS kind.
            // Excluded from the nativeId → kind REVERSE map a reader builds.
            if (r.contains("emitOnly")) {
                if (!r.at("emitOnly").is_boolean()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/emitOnly", i),
                           "'emitOnly' must be a boolean");
                    return false;
                }
                info.emitOnly = r.at("emitOnly").get<bool>();
            }
            // ── TYPO DISCRIMINATOR FOR THE RELOCATION ROW ────────────────
            //
            // Every field above is read with `r.contains(...)`, so an
            // UNRECOGNISED key was ignored in silence. That is tolerable for
            // a key nothing reads and intolerable for these: each one is a
            // ROLE DECLARATION whose absence changes what an object reader
            // concludes, and absence is spelled exactly the same way as a
            // typo. `"isCal"` / `"iscall"` / `"emitonly"` would have loaded
            // clean and left the format silently back at the behaviour
            // D-LK-MACHO-ISDATA-NO-CALL-SIGNAL exists to end -- a mach-o
            // format refusing every member that calls a function, or worse,
            // an ELF one classifying an extern function as DATA. The document
            // ROOT has had this discriminator since TF-C125; the rows never
            // did.
            //
            // `kind` and `name` are consumed by the shared loader
            // (`substrate::loadRelocationsTable`) before this extension runs,
            // so the closed set must be the UNION of both halves even though
            // nothing here reads them. `$`-prefixed prose keys are skipped, as
            // everywhere — the PREFIX predicate, never a literal `"$comment"`.
            //
            // ✔ THE PROMOTION THIS COMMENT USED TO ASK FOR HAS HAPPENED. The
            // check was a fourth hand-rolled copy of the target loader's
            // file-local helper; it now lives beside `isDocumentationKey` in
            // `core/types/config_key_vocabulary.hpp` and every loader calls
            // the one implementation. The measured count was worse than
            // "fourth": EIGHT named helpers over ~57 call sites, two of them
            // (`opt/optimizer_json.cpp`, `ffi/shipped_lib_descriptor.cpp`)
            // missing the `$`-prose carve-out outright. The TABLE stays here,
            // with the fields it describes — only the loop moved.
            static constexpr std::array<std::string_view, 6> kRelocationRowKeys{
                "name", "kind", "nativeId", "pltNativeId", "isCall", "emitOnly"};
            DSS_CHECK_KEY_VOCABULARY(kRelocationRowKeys);
            bool rowClean = true;
            detail::rejectUnknownKeys(r, kRelocationRowKeys, "a relocation row",
                [&](std::string_view key, std::string message) {
                    // The shared sentence, plus what makes THIS row's typo
                    // worse than a generic one: its keys are ROLE
                    // declarations, and a misspelled role is spelled exactly
                    // like an absent one.
                    message += ". A misspelled ROLE key is indistinguishable "
                               "from an absent one, and absence is what "
                               "D-LK-MACHO-ISDATA-NO-CALL-SIGNAL was";
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/{}", i, key),
                           std::move(message));
                    rowClean = false;
                });
            return rowClean;
        });

    // sections[] — D-LK4-2 schema row. Each entry maps a universal
    // SectionKind to format-native name + structural fields.
    if (doc.contains("sections")) {
        if (!doc.at("sections").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/sections",
                      "'sections' must be an array");
        } else {
            auto const& secs = doc.at("sections");
            data.sections.reserve(secs.size());
            for (std::size_t i = 0; i < secs.size(); ++i) {
                auto const& s = secs[i];
                if (!s.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}", i),
                              "section entry must be an object");
                    continue;
                }
                // Every optional numeric field below is read through
                // `readU64`, which RETURNS SILENTLY when the key is absent —
                // so a misspelled `"addrAlign"` left the section at alignment
                // 0 with no diagnostic anywhere. The largest closed set in the
                // file, and the one where absence and typo were least
                // distinguishable.
                static constexpr std::array<std::string_view, 8> kSectionRowKeys{
                    "kind", "name", "segment", "type", "flags", "addrAlign",
                    "entrySize", "virtualAddress"};
                DSS_CHECK_KEY_VOCABULARY(kSectionRowKeys);
                rejectUnknownKeys(s, kSectionRowKeys,
                                  std::format("/sections/{}", i),
                                  "a 'sections' row", coll);
                ObjectFormatSectionInfo info;
                if (!s.contains("kind") || !s.at("kind").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/sections/{}/kind", i),
                              "missing or non-string 'kind' (one of 'text' "
                              "/ 'rodata' / 'data' / 'bss' / 'symtab' / "
                              "'strtab' / 'reloc' / 'dynamic' / 'note' / "
                              "'debug' / 'custom')");
                    continue;
                }
                auto const kOpt =
                    sectionKindFromName(s.at("kind").get<std::string>());
                if (!kOpt.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/kind", i),
                              "unknown SectionKind name");
                    continue;
                }
                info.kind = *kOpt;
                if (!s.contains("name") || !s.at("name").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/sections/{}/name", i),
                              "missing or non-string 'name'");
                    continue;
                }
                info.name = s.at("name").get<std::string>();
                // `segment` is optional for ELF/PE (empty default);
                // Mach-O validate() rejects empty here.
                if (s.contains("segment")) {
                    if (!s.at("segment").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/segment", i),
                                  "'segment' must be a string");
                    } else {
                        info.segment = s.at("segment").get<std::string>();
                    }
                }
                auto readU64 = [&](char const* field, std::uint64_t& out) {
                    if (!s.contains(field)) return;
                    if (!s.at(field).is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/{}", i, field),
                                  std::format("'{}' must be a non-negative "
                                              "integer",
                                              field));
                        return;
                    }
                    std::int64_t const v = s.at(field).get<std::int64_t>();
                    if (v < 0) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/{}", i, field),
                                  std::format("'{}' ({}) must be >= 0",
                                              field, v));
                        return;
                    }
                    out = static_cast<std::uint64_t>(v);
                };
                std::uint64_t typeRaw = 0;
                readU64("type", typeRaw);
                if (typeRaw > 0xFFFFFFFFu) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/type", i),
                              "'type' must fit in 32 bits");
                    continue;
                }
                info.type = static_cast<std::uint32_t>(typeRaw);
                readU64("flags", info.flags);
                readU64("addrAlign", info.addrAlign);
                readU64("entrySize", info.entrySize);
                readU64("virtualAddress", info.virtualAddress);
                std::uint16_t const idx =
                    static_cast<std::uint16_t>(data.sections.size());
                auto [it, fresh] =
                    data.sectionKindIndex.emplace(info.kind, idx);
                if (!fresh) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/kind", i),
                              std::format("duplicate section kind '{}'",
                                          std::string{sectionKindName(info.kind)}));
                    continue;
                }
                data.sections.push_back(std::move(info));
            }
        }
    }

    // ── Per-format identity sub-block readers ───────────────────────────
    //
    // WAS: five `if (data.kind == ObjectFormatKind::<X> && doc.contains(...))`
    // readers totalling ~800 lines. They moved VERBATIM to the backend that
    // owns each block (`src/link/format/<x>_backend.cpp`) — same parsing, same
    // diagnostics, same JSON pointers.
    //
    // ★ THE `data.kind ==` HALF OF EACH CONDITION WAS ALREADY DEAD, in all
    // five. `kCrossKindRules` (above, now the generic block-ownership loop) had
    // ALREADY emitted an Error for exactly those (kind, block) pairs, so
    // `loadFromText` had already failed and the `data.kind ==` half could only
    // suppress a write into a struct about to be discarded. That was MEASURED
    // in TF-C122 but only INFERRED from the collector's severity default —
    // never executed, and safe only while that guard stayed at Error, a
    // coupling nothing tested. Routing through the resolved backend removes the
    // question entirely rather than leaving it to be re-derived.
    backend->readIdentity(link::ObjectFormatIdentityDoc{doc}, data,
                          coll);

    for (auto&& problem : data.validate()) {
        coll.emitRaw(std::move(problem));
    }

    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll).release());
    }

    auto schema = std::make_shared<ObjectFormatSchema>(std::move(data));
    schema->contentDigest_ = std::move(digest);
    return schema;
}

} // namespace dss
