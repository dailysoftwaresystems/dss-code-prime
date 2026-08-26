#include "link/object_format_schema.hpp"

#include "core/crypto/sha256.hpp"                 // crypto::sha256Hex — the memo key
#include "core/substrate/checked_file_read.hpp"   // the ONE checked whole-file read
#include "core/substrate/phase_timers.hpp"        // the load-config / build-config phases
#include "core/substrate/relocation_table.hpp"
#include "core/types/config_document_memo.hpp"    // the ONE content-addressed schema memo
#include "core/types/config_key_vocabulary.hpp"  // detail::renderAllowedList — the ONE closed-set renderer
#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>


// ── PIN (D-CORE-JSON-LEAKS-INTO-TWO-PUBLIC-HEADERS) ─────────────────────────
// This TU uses only `validateRelocationsTable`, which touches no JSON, and the
// `relocations[]` substrate was split so it would stop seeing nlohmann for it.
// The split is invisible to every test: a TU that gains a transitive include is
// not an error, so a stray `#include <nlohmann/json.hpp>` in ANY header reached
// from here would silently undo the split and the suite would stay green. This
// guard is therefore the whole enforcement, and it is red-on-disable by
// construction -- restore the include and the build stops here.
// Loader work belongs in the matching `*_json.cpp`, which includes
// `core/substrate/relocation_table_json.hpp`.
#ifdef NLOHMANN_JSON_VERSION_MAJOR
#error "nlohmann/json re-entered this TU's include set. This file validates \
typed rows and must not see JSON; move the JSON work to its *_json.cpp sibling \
(see core/substrate/relocation_table_json.hpp)."
#endif

// ─────────────────────────────────────────────────────────────────────────
// ★★★ COMPILE-ERROR PIN — D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES
// (TF-C125). DO NOT DELETE TO "FIX A BUILD ERROR".
// ─────────────────────────────────────────────────────────────────────────
//
// If your build just failed with `'ObjectFormatKind': ambiguous symbol`
// (MSVC C2872) or `reference to 'ObjectFormatKind' is ambiguous` (GCC/Clang),
// you have re-introduced a FORMAT-IDENTITY BRANCH into shared substrate. That
// is the hard veto this pin exists to enforce, and the fix is never to remove
// the pin — it is to move the per-format rule into the backend that owns it
// (`src/link/format/<x>_backend.cpp`) and ask the abstract
// `ObjectFormatBackend` a CAPABILITY question instead.
//
// ★ WHY A COMPILE ERROR AND NOT A GREP TEST. A grep test asserting "the string
// `ObjectFormatKind::` does not appear in this file" is VACUOUS until somebody
// re-introduces a branch — it passes on day one whether or not it works, and
// it is the exact species of instrument this project has repeatedly caught
// reporting success over something it could not observe. It is also
// rationalizable: a reviewer under deadline can add an exception list. A
// compile error cannot be rationalized, cannot silently stop working, and is
// re-verified on every single build on every host.
//
// ★ HOW IT WORKS. After every #include, the type's NAME is redefined to an
// identifier that does not exist. Any later spelling of it — in any form —
// fails to compile, naming this macro in the diagnostic. Uses already parsed
// (the schema header's own inline members, the `EnumNameTable` instantiations)
// are untouched, because this sits below all includes. The pinned TU
// legitimately needs nothing from that vocabulary: a format's own spelling is
// `backend->configName()`.
//
// ★★ IT IS A MACRO AND NOT A C++ ALIAS, AND THAT IS A CORRECTION, NOT A TASTE
// CALL. The first version of this pin declared a second `ObjectFormatKind` in
// an anonymous namespace to make the name AMBIGUOUS. An independent audit
// refuted it and I re-measured on both toolchains: that pin caught the
// unqualified spelling and **let `dss::ObjectFormatKind::Elf` straight
// through** (gcc rc=0, MSVC rc=0). Qualified lookup in `dss` consults
// using-directives only when the direct search in `dss` finds nothing — and
// the enum is declared directly in `dss`, so the alias never entered the
// candidate set. Explicit qualification is the FIRST thing a maintainer
// reaches for. ✔MEASURED with this macro: clean TU rc=0 on both; unqualified,
// `dss::`-qualified and `::dss::`-qualified all REFUSED on both.
//
// ⚠⚠ WHAT THIS PIN DOES **NOT** CATCH — stated because the version of this
// comment that claimed "a compile error cannot be rationalized" was, on its
// own terms, over-claiming. The pin closes the DIRECT spelling of the type.
// It does not close identity comparisons that never name it:
//   * `backend->configName() == "elf"` — the likeliest re-introduction of all,
//     because the string is right there on the interface the loader holds;
//   * `objectFormatKindFromName("elf") == schema.kind()`;
//   * `objectFormatKindName(k) == "pe"`;
//   * `static_cast<int>(k) == 1`;
//   * a helper added to `object_format_schema.hpp`, which is included ABOVE
//     this line and is therefore not pinned.
// The first of those is covered by `SchemaTierDoesNotCompareFormatIdentity`
// in tests/link/test_object_format_backend_registry.cpp — a source scan, and
// weaker than a compile error, which is exactly why it is named as a SECOND
// net rather than sold as the same thing. The rest are covered by review and
// by the agnosticism grep in `.claude/skills/dss-audit/SKILL.md`.
#define ObjectFormatKind \
    DSS_FORMAT_IDENTITY_IS_NOT_SPELLABLE_IN_THIS_TRANSLATION_UNIT

namespace dss {

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadFromFile(std::filesystem::path const& path) {
    // `load-config` — see `core/substrate/phase_timers.hpp`. Spans the read,
    // the digest, the memo lookup and (miss only) the nested `build-config`.
    substrate::PhaseTimers::Scope const loadScope{
        substrate::CompilePhase::LoadConfig};

    // THE ONE CHECKED READ (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT).
    // `.format.json` is the MOST-read shipped document class in the tree, so a
    // torn read here is the one most likely to be met in the field — and it must
    // say the READ failed, not that the format description is malformed.
    auto text = core::readFileChecked(path);
    if (!text) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             path.string(), std::move(text).error().message}});
    }

    // ── THE CONTENT-ADDRESSED MEMO, THIRD FAMILY ──────────────────────────
    // D-CONFIG-A-SCHEMA-DOCUMENT-IS-REBUILT-ONCE-PER-LOAD-INSIDE-ONE-PROCESS.
    // `.format.json` is the most-read shipped document class in the tree — ✔3
    // loads for a one-line C compile (MEASURED 2026-08-25, cycle P35, Windows
    // Debug, 7 ms) and one per declared sibling flavour on a link — so it is
    // also the family that repeats itself most across a multi-target run.
    // The key is the SHA-256 of the bytes just read; see
    // `config_document_memo.hpp` for why that makes a stale hit structurally
    // impossible rather than a policy to get right.
    //
    // ★ THE DEPENDENCY LEDGER IS EMPTY HERE FOR THE SAME MEASURED REASON AS THE
    // TARGET FAMILY: ✔`object_format_schema_json.cpp` reads no file — no
    // `readFileChecked`, no `ifstream`, no `<filesystem>` — so a built schema is
    // a pure function of this document's own bytes. Sibling FLAVOURS are
    // resolved by the caller through `loadShipped`, each one its own load with
    // its own digest, so nothing is folded in behind this document's back.
    //
    // ⓘ The miss path digests these bytes twice (once here for the key, once
    // inside `loadFromText` for `contentDigest()`) — bounded to the cold path,
    // and 🧠DERIVED from the ✔MEASURED Debug digest rate (14.5 ns/byte) at well
    // under a millisecond for a 20–46 KB format document; the same note on the
    // target family carries the full reasoning.
    std::string const label  = path.string();
    std::string       digest = crypto::sha256Hex(*text);
    if (auto hit =
            detail::ConfigDocumentMemo<ObjectFormatSchema>::lookup(label, digest)) {
        return hit;
    }

    // `build-config` — DEFINED as the work a memo hit skips.
    auto schema = [&] {
        substrate::PhaseTimers::Scope const buildScope{
            substrate::CompilePhase::BuildConfig};
        return loadFromText(*text, label);
    }();
    // ⚠ Stored only on the SUCCESS path — a failed load produced diagnostics and
    // no schema, and the loader's own refusal already reports it every time.
    if (schema) {
        detail::ConfigDocumentMemo<ObjectFormatSchema>::store(
            label, std::move(digest),
            std::vector<detail::ConfigDocumentDependency>{}, *schema);
    }
    return schema;
}

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadShipped(std::string_view name) {
    auto path = findShippedConfig({name, "object-formats", ".format.json",
                                   "object format",
                                   DiagnosticCode::C_InvalidFormatName});
    if (!path) return std::unexpected(std::move(path).error());
    return loadFromFile(*path);
}

// ── The one reverse map (D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS) ──
//
// Deliberately adjacent to `ObjectFormatData::validate()` below, because the
// two halves are one rule: `validate()` PROMISES that at most one non-alias
// row claims a wire id and that every alias aliases a real row, and this is
// the code that SPENDS that promise. Splitting them across a reader is how
// two of the three readers came to spend a promise they never applied.
//
// Order-independent: the `emitOnly` skip means the row owning a wire id wins
// no matter where either row sits in the document.
std::expected<RelocationDecodeTable, std::string>
ObjectFormatSchema::relocationDecodeTable() const {
    RelocationDecodeTable table;

    // Duplicate-nativeId guard: a wire id mapping to two DIFFERENT kinds is an
    // ambiguous schema — say so rather than let "last row wins" silently
    // mis-decode. Returns the message, or nullopt to continue.
    auto mapNative = [&](std::uint32_t nid,
                         RelocationKind kind) -> std::optional<std::string> {
        auto const ins = table.nativeToKind.emplace(nid, kind);
        if (!ins.second && ins.first->second.v != kind.v) {
            return "object format schema '" + std::string{name()}
                 + "' maps native reloc id " + std::to_string(nid)
                 + " to two different RelocationKinds ("
                 + std::to_string(ins.first->second.v) + " and "
                 + std::to_string(kind.v) + ") -- ambiguous reverse map.";
        }
        return std::nullopt;
    };

    for (auto const& r : d_.relocations) {
        // AN EMISSION ALIAS IS NOT DECODABLE. It shares its wire type with a
        // real row and exists only so the EMITTER can reach that type through
        // a second DSS kind (x86_64's DWARF FDE pointer vs the call-site
        // rel32 — same R_X86_64_PC32, different implicit addend bias). The
        // wire carries no bias, so there is nothing to decode differently;
        // including it here would make the map ambiguous and reject every
        // object using that entirely ordinary relocation. `validate()`
        // guarantees the aliased row is present, so skipping never leaves the
        // wire id unmapped.
        if (r.emitOnly) continue;
        if (auto e = mapNative(r.nativeId, r.kind)) {
            return std::unexpected(std::move(*e));
        }
        // `pltNativeId` is read UNCONDITIONALLY, from the schema, for every
        // format. It is 0 on every shipped Mach-O and PE document — which is
        // a fact about those formats (an extern call is the same wire id
        // whether or not the linker synthesizes a stub), not a reason for
        // their readers to omit the leg. Omitting it is what made this loop
        // format-keyed in two places at once.
        if (r.pltNativeId != 0u) {
            if (auto e = mapNative(r.pltNativeId, r.kind)) {
                return std::unexpected(std::move(*e));
            }
            table.callSignalNativeIds.insert(r.pltNativeId);
        }
        if (r.isCall) table.callSignalNativeIds.insert(r.nativeId);
    }
    return table;
}

namespace detail {

namespace {

ConfigDiagnostic makeProblem(std::string path, std::string message) {
    return ConfigDiagnostic{
        DiagnosticCode::C_MalformedJson,
        DiagnosticSeverity::Error,
        std::move(path),
        std::move(message),
    };
}

// The kinds a document MAY declare twice, rendered for a diagnostic.
// D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE.
//
// ⚠ DERIVED FROM THE PREDICATE, NEVER SPELLED. A literal "unwind" here would
// be a second owner of the membership rule, and the day a second role becomes
// encoding-discriminated the messages would keep naming the old set while the
// loader accepted the new one — the exact drift shape `namesWhere` exists to
// prevent one vocabulary over.
[[nodiscard]] std::string encodingDiscriminatedKindList() {
    return renderAllowedList(kEncodingDiscriminatedKindNames);
}

} // namespace

// D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE. Both
// indexes are maintained HERE and nowhere else, so `sections` and its two
// projections cannot disagree — the derived-at-load relationship the field's
// own docblock claims.
bool ObjectFormatData::addSectionRow(ObjectFormatSectionInfo info,
                                     std::uint16_t& duplicateOfOut) {
    SectionKindEncoding const id{info.kind, info.encoding};
    auto const idx = static_cast<std::uint16_t>(sections.size());
    auto [it, fresh] = sectionKindIndex.emplace(id, idx);
    if (!fresh) {
        duplicateOfOut = it->second;
        return false;
    }
    // A SECOND row of a kind retires that kind's unique-row answer rather than
    // overwriting it. Overwriting would make `sectionByKind` return whichever
    // row happened to load LAST — a silent choice between two correct-looking
    // answers, which is worse than no answer.
    auto [uit, uFresh] = sectionKindUniqueRow.emplace(info.kind, idx);
    if (!uFresh) uit->second = kAmbiguousSectionRow;
    sections.push_back(std::move(info));
    return true;
}

std::vector<ConfigDiagnostic> ObjectFormatData::validate() const {
    std::vector<ConfigDiagnostic> problems;
    auto fail = [&](std::string path, std::string msg) {
        problems.push_back(makeProblem(std::move(path), std::move(msg)));
    };

    // Format kind must be a real shipped format. `Unknown` is the
    // invalid sentinel — a JSON file that names "unknown" (or omits
    // the field, which the loader rejects upstream) is not a real
    // format declaration.
    // The document must resolve to a real backend. `nullptr` is the invalid
    // sentinel — it is what `objectFormatBackendByConfigName` returns for an
    // unrecognized spelling AND for the reserved `unknown`, and it is what a
    // HAND-BUILT `ObjectFormatData` carries when it never set the field.
    //
    // ★ THIS ARM REPLACED `if (kind == ObjectFormatKind::Unknown)`, and it is
    // strictly stronger than the check it replaced. The old field defaulted to
    // `ObjectFormatKind::Elf`, so a default-constructed `ObjectFormatData`
    // sailed past the sentinel test claiming an ELF identity with
    // `elf.machine == 0` (EM_NONE) — caught only downstream, if at all. A null
    // pointer has no such favoured value to fall into.
    //
    // ⚠ DECLARED BEHAVIOUR CHANGE on the hand-built path (flagged by an
    // independent audit; recorded rather than left to be rediscovered).
    // `ObjectFormatSchema{ObjectFormatData}` is a public, validation-free
    // constructor, so this path is reachable. For a struct that declares
    // NOTHING: the old code ran the ELF identity rules (because the field
    // defaulted to Elf) and produced `/elf/class` + `/elf/data` +
    // `/elf/machine` and NO `/format/kind`; the new code produces
    // `/format/kind` alone and skips `validateIdentity`. Second order: a
    // default struct plus `container: archive` used to PASS the container rule
    // (the Elf arm saw the default `objectType == Rel`) and now fails it. Both
    // moves are toward the strict direction — an unresolved format is refused
    // for being unresolved, instead of being silently adjudicated as ELF.
    if (backend == nullptr) {
        fail("/format/kind",
             std::format("format kind '{}' is reserved as the invalid "
                         "sentinel; declare one of {}",
                         kObjectFormatKindSentinelName,
                         link::objectFormatBackendNameList()));
    }

    // FC3 c1: the data model is REQUIRED (the loader rejects a missing
    // or unknown `dataModel` upstream; this arm catches a HAND-BUILT
    // ObjectFormatData that never set it — the zero default is the
    // invalid sentinel, never a silent width choice).
    if (dataModelName(dataModel).empty()) {
        fail("/dataModel",
             std::format("missing required 'dataModel' — every object format "
                         "must declare its C-family width triple ({}); a "
                         "silent default would bake wrong primitive widths",
                         detail::renderAllowedList(allNames(kDataModelTable),
                                                   ", ")));
    }

    // D-PP-HEADER-CASE-INSENSITIVE-PE: the header-name case rule is REQUIRED
    // (the loader rejects a missing or unknown `headerNameMatching` upstream;
    // this arm catches a HAND-BUILT ObjectFormatData that never set it — the
    // zero default is the invalid sentinel, never a silent case rule).
    if (headerNameMatchingName(headerNameMatching).empty()) {
        fail("/headerNameMatching",
             std::format("missing required 'headerNameMatching' — every "
                         "object format must declare how an `#include` header "
                         "NAME is matched ({}); a silent default would let the "
                         "BUILD HOST's filesystem decide, which both wrongly "
                         "rejects `<Windows.h>` for a pe target on a "
                         "case-sensitive host and silently ACCEPTS `<Stdio.h>` "
                         "for an elf target on a case-insensitive one",
                         detail::renderAllowedList(
                             allNames(kHeaderNameMatchingTable), " or ")));
    }

    // ── D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN: the C-symbol decoration
    //    rule is REQUIRED on EVERY format ──────────────────────────────
    //
    // The loader rejects a missing or unknown `cSymbolDecoration` upstream;
    // THIS arm is the only thing standing between a HAND-BUILT
    // `ObjectFormatData` and a schema whose decoration rule was never
    // declared. That path is real and is exactly the one the linker and the
    // walkers are handed: `ObjectFormatSchema{ObjectFormatData}` is a public
    // constructor that runs NO validation, so every in-memory producer
    // reaches the engine without passing the JSON tier at all.
    //
    // ★★ THE TEETH — READ THIS BEFORE ADDING ANY CONDITION TO THE LINE
    // BELOW. The predicate guarding this rule is the constant `true`: there
    // is no `if (isExecFlavor())`, no `if (kind == …)`, nothing. That is
    // deliberate and it is what makes the rule enforce anything at all, for
    // three independent reasons:
    //   * a relocatable Mach-O `.o` carries `_main` exactly as its MH_EXECUTE
    //     sibling does — the decoration is not an executable-only property;
    //   * `unapplyCMangling` runs on LIBRARY INGEST, which happens under
    //     whatever flavor is being produced, including a staticlib;
    //   * decisively, A UNIVERSAL PREDICATE CANNOT BE TAUTOLOGICAL. Contrast
    //     the `processExit ⇒ isExecFlavor()` rule in this same function: its
    //     ET_DYN arm enforces NOTHING, because `elfDynPieShape` counts
    //     `processExit.has_value()` as one of its own cluster members, so the
    //     antecedent already contains the consequent. The tree says so in its
    //     own words at `ObjectFormatSchema::isExecFlavor()` — "a TAUTOLOGY
    //     [that] enforces nothing". A rule whose guard is `true` has no
    //     antecedent to be contaminated.
    //
    // ★ VERIFIED at the time of writing (grep of this header + this file):
    // NO sibling predicate reads `cSymbolDecoration` — not `isExecFlavor()`,
    // not `isImageFlavor()`, not `allowsUndefinedImports()`, not
    // `isStaticArchive()`. IF A FUTURE PREDICATE STARTS READING THIS FIELD,
    // AND THIS RULE IS EVER GATED ON THAT PREDICATE, THE RULE LOSES ITS TEETH
    // ON THAT ARM — which is the processExit story repeating. Keep the guard
    // constant, or the reason it is safe disappears with it.
    if (cSymbolDecorationSchemeName(cSymbolDecoration.scheme).empty()) {
        fail("/cSymbolDecoration",
             std::format("missing required 'cSymbolDecoration' — every object "
                         "format must declare how a canonical C identifier is "
                         "decorated to obtain its linker-visible name ({}); a "
                         "silent default would re-hide the rule in the engine's "
                         "C++ table, which is the two-owner defect this key "
                         "exists to remove",
                         detail::renderAllowedList(
                             allNames(kCSymbolDecorationSchemeTable), " or ")));
    }

    // ── D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED: a PRESENT
    //    `weakDefinition` block must name a real dialect ─────────────────
    //
    // Presence is the declaration, so ABSENCE is not an error here — the
    // walker that needs the answer refuses when it needs it, and a format
    // that never encodes a weak definition never has to answer. What is an
    // error is a block that is PRESENT and says nothing: the sentinel has no
    // spelling (it is deliberately absent from `kWeakDefinitionDialectTable`,
    // so no JSON string can produce it), which means the only way to reach
    // this state is a HAND-BUILT `ObjectFormatData` — the
    // `ObjectFormatSchema{ObjectFormatData}` public constructor runs no
    // validation, and every in-memory producer reaches the walkers through it.
    // Without this arm such a schema would carry an engaged optional whose
    // dialect matches no encoder, and the walker's refusal would name the
    // empty string.
    if (weakDefinition.has_value()
     && weakDefinitionDialectName(weakDefinition->dialect).empty()) {
        fail("/weakDefinition/dialect",
             std::format("'weakDefinition' is present but names no dialect — a "
                         "DECLARED block must state HOW this format spells a "
                         "weak definition ({}). Omit the block entirely to "
                         "leave the question unanswered; an engaged block with "
                         "the invalid sentinel is neither an answer nor an "
                         "omission. D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-"
                         "DECLARED.",
                         detail::renderAllowedList(
                             allNames(kWeakDefinitionDialectTable), " or ")));
    }

    // ── D-FF1-AR-STATICLIB-DRIVER-WIRING (c171): container rules ──
    //
    // `container: archive` is a STATIC-LIBRARY format: its driver output is
    // an `ar` bundle of RELOCATABLE members a foreign linker later pulls +
    // merges. Reject it on any IMAGE flavor (ET_EXEC/ET_DYN, PE Exec/Dll,
    // MH_EXECUTE/MH_DYLIB) — a linker cannot pull a member out of a
    // pre-linked image — and on WASM/SPIR-V, which have no `ar` archive
    // concept. This mirrors the `linkAndWriteStaticArchive` runtime guard
    // (`isImageFlavor()` reject) at LOAD time, so a mis-declared static-lib
    // format fails loud at its source rather than deep in the driver.
    if (container == ObjectFormatContainer::Archive) {
        // WAS: a 4-arm `switch (kind)` mapping each format to its own
        // relocatable member type. The question being asked is a CAPABILITY —
        // "can this format's artifact BE an `ar` member?" — so the backend
        // answers it. Null backend ⇒ false, the fail-closed direction: an
        // unresolvable format is not a static library.
        bool const relocatableMember =
            backend != nullptr && backend->isRelocatableMember(*this);
        if (!relocatableMember) {
            fail("/container",
                 "'container: archive' (a static library) requires a "
                 "RELOCATABLE member type — ELF 'rel' / PE 'obj' / Mach-O "
                 "'object'; an image flavor (exec/dyn/dll/dylib) or a "
                 "non-`ar` format (wasm/spirv) cannot be bundled into an "
                 "archive a foreign linker pulls from "
                 "(D-FF1-AR-STATICLIB-DRIVER-WIRING)");
        }
    }

    // Cross-row reloc uniqueness + non-empty-name + non-zero-kind:
    // shared substrate with TargetSchema so the two sides of plan
    // 13 §2.6's reloc-taxonomy unifier are validated identically.
    substrate::validateRelocationsTable<ObjectFormatRelocationInfo>(
        relocations, fail);

    // nativeId is the format's actual wire value (e.g. ELF R_X86_64_PC32
    // = 2 in `r_info` low 32 bits). Zero would silently write a
    // R_X86_64_NONE relocation that the linker treats as a no-op — a
    // miscompile that round-trips as syntactically valid. Reject at
    // load time when relocations[] is non-empty.
    // D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS: `nativeId → kind` is a
    // reverse map every object READER builds, so it must be a FUNCTION. A
    // wire type may therefore be claimed by AT MOST ONE non-alias row; any
    // further row sharing it must declare `emitOnly` (an emission alias, e.g.
    // R_X86_64_PC32 serving both the call-site rel32 and the DWARF FDE
    // pointer, which differ only in an implicit addend bias the wire format
    // does not carry).
    // ✔MEASURED 2026-08-13: without this, an ambiguous pair was discovered at
    // READ time and rejected every x86_64 ELF object — DSS-produced and
    // gcc-produced alike. Checking it here fails the SCHEMA that is wrong
    // rather than every object that is right.
    {
        std::unordered_map<std::uint32_t, std::size_t> primaryOf;
        for (std::size_t i = 0; i < relocations.size(); ++i) {
            if (relocations[i].emitOnly) continue;
            auto const ins = primaryOf.emplace(relocations[i].nativeId, i);
            if (!ins.second) {
                fail(std::format("/relocations/{}/nativeId", i),
                     std::format(
                         "relocation '{}': nativeId {} is already claimed by "
                         "'{}' at /relocations/{}. A native wire id maps back "
                         "to exactly ONE RelocationKind (object readers build "
                         "that reverse map); if this row exists only so the "
                         "emitter can reach the same wire type through a "
                         "different kind, declare \"emitOnly\": true",
                         relocations[i].name, relocations[i].nativeId,
                         relocations[ins.first->second].name,
                         ins.first->second));
            }
        }
        // An alias that aliases nothing is a typo, not an alias: it would sit
        // in no reverse map at all and silently decode as an unknown type.
        for (std::size_t i = 0; i < relocations.size(); ++i) {
            if (relocations[i].emitOnly
                && !primaryOf.contains(relocations[i].nativeId)) {
                fail(std::format("/relocations/{}/emitOnly", i),
                     std::format(
                         "relocation '{}' declares emitOnly but no other row "
                         "claims nativeId {}; an emission alias must alias a "
                         "real row, otherwise nothing can DECODE this wire id",
                         relocations[i].name, relocations[i].nativeId));
            }
        }
        // D-LK-MACHO-ISDATA-NO-CALL-SIGNAL: `isCall` is consumed through the
        // very reverse map an `emitOnly` row is EXCLUDED from, so the pair is a
        // contradiction -- the declaration could never be read. Silent, and it
        // reads to a maintainer as a guarantee that is in force. Refuse it.
        for (std::size_t i = 0; i < relocations.size(); ++i) {
            if (relocations[i].isCall && relocations[i].emitOnly) {
                fail(std::format("/relocations/{}/isCall", i),
                     std::format(
                         "relocation '{}' declares both \"isCall\" and "
                         "\"emitOnly\". An emission alias is excluded from the "
                         "nativeId -> kind reverse map object readers build, so "
                         "its call/branch role can never be consulted -- declare "
                         "\"isCall\" on the row that OWNS nativeId {} instead",
                         relocations[i].name, relocations[i].nativeId));
            }
        }
    }

    for (std::size_t i = 0; i < relocations.size(); ++i) {
        if (relocations[i].nativeId == 0) {
            fail(std::format("/relocations/{}/nativeId", i),
                 std::format("relocation '{}': 'nativeId' must be != 0 "
                             "(the format-specific wire tag, e.g. ELF "
                             "R_X86_64_PC32 = 2)",
                             relocations[i].name));
        }
    }

    // Sections: (kind, encoding) unique cross-row + name non-empty. The format
    // walker resolves `sectionByKind(SectionKind::Text)` to find
    // the format-native section name + structural fields.
    //
    // ★ THE UNIQUENESS KEY IS THE PAIR SINCE
    //   D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE, and
    //   the three rules below are what keep `sectionByKind` -- the kind-only
    //   lookup every WRITER uses -- honest afterwards. Without them a second
    //   row of ANY kind would make that accessor answer "no such section" for
    //   a section the document plainly declares.
    {
        std::unordered_map<SectionKindEncoding, std::size_t> seenSection;
        std::unordered_map<SectionKind, std::size_t> rowsPerKind;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            auto const& s = sections[i];
            if (s.name.empty()) {
                fail(std::format("/sections/{}/name", i),
                     "section row: 'name' must be a non-empty string");
            }
            ++rowsPerKind[s.kind];
            auto [it, fresh] =
                seenSection.emplace(SectionKindEncoding{s.kind, s.encoding}, i);
            if (!fresh) {
                fail(std::format("/sections/{}/kind", i),
                     std::format("section '{}': duplicate 'kind' value "
                                 "(already declared by section '{}' at "
                                 "/sections/{})",
                                 s.name, sections[it->second].name,
                                 it->second));
            }
            // (b) AN ENCODING ON A ROLE THAT DOES NOT HAVE ONE IS INERT
            // CONFIG, and inert config is rejected BY NAME here exactly as
            // `charSignedness` rejects a second-owner key rather than letting
            // it sit and read as meaningful. Declaring `"encoding"` on a
            // `text` row states a discriminator nothing will ever dispatch on.
            if (s.encoding != SectionEncoding::Unspecified
                && !sectionKindIsEncodingDiscriminated(s.kind)) {
                fail(std::format("/sections/{}/encoding", i),
                     std::format("section '{}': 'kind' \"{}\" has exactly one "
                                 "wire encoding, so declaring 'encoding' "
                                 "\"{}\" states a discriminator no reader "
                                 "dispatches on. Only these kinds are "
                                 "encoding-discriminated: {}",
                                 s.name, sectionKindName(s.kind),
                                 sectionEncodingName(s.encoding),
                                 encodingDiscriminatedKindList()));
            }
        }
        for (std::size_t i = 0; i < sections.size(); ++i) {
            auto const& s = sections[i];
            if (rowsPerKind[s.kind] < 2u) continue;
            // (c) A KIND MAY REPEAT ONLY WHERE THE ROLE HAS SEVERAL ENCODINGS.
            if (!sectionKindIsEncodingDiscriminated(s.kind)) {
                fail(std::format("/sections/{}/kind", i),
                     std::format("section '{}': 'kind' \"{}\" is declared by "
                                 "{} rows. A kind may appear more than once "
                                 "only when its ROLE has several wire "
                                 "encodings, because every other kind is "
                                 "resolved by kind alone and a second row "
                                 "makes that lookup answer nothing. "
                                 "Encoding-discriminated kinds: {}",
                                 s.name, sectionKindName(s.kind),
                                 rowsPerKind[s.kind],
                                 encodingDiscriminatedKindList()));
                continue;
            }
            // (d) …AND THEN EVERY ONE OF THEM MUST SAY WHICH ENCODING IT IS.
            // The pair rule alone would admit one `Unspecified` row beside one
            // declared row: pair-unique, yet the `Unspecified` row is exactly
            // the one no reader can identify.
            if (s.encoding == SectionEncoding::Unspecified) {
                fail(std::format("/sections/{}/encoding", i),
                     std::format("section '{}': this document declares {} "
                                 "rows of kind \"{}\", so each must say which "
                                 "wire encoding it carries -- this row says "
                                 "nothing, and a reader would have to guess "
                                 "between them. One of: {}",
                                 s.name, rowsPerKind[s.kind],
                                 sectionKindName(s.kind),
                                 renderAllowedList(kDeclarableSectionEncodingNames)));
            }
        }
    }

    // ── Per-format identity rules ───────────────────────────────────────
    //
    // WAS: four blocks totalling ~1,030 lines, each opened by
    // `if (kind == ObjectFormatKind::<X>)` — the ELF class/data/machine +
    // ET_EXEC/ET_DYN cluster rules, the PE machine + section-field rules, the
    // PE32+ optional-header rules, and the Mach-O cputype/segment/16-char
    // rules. Every one of them now lives with the backend that owns it
    // (`src/link/format/<x>_backend.cpp`), MOVED VERBATIM: same rules, same
    // wording, same JSON pointers. This function no longer knows how many
    // formats exist, nor that ELF has a `machine` field at all.
    //
    // ★ GUARDED ON A RESOLVED BACKEND, AND THAT GUARD IS THE RISKIEST LINE IN
    // THE REFACTOR. If a null backend meant "no identity rules apply", all 24
    // shipped formats would validate CLEAN while validating NOTHING — and the
    // negative tests would not catch it, because they assert only that
    // SOMETHING rejected and their fixtures reject for other reasons too. The
    // null case is unreachable here (the `/format/kind` rule above already
    // failed the document), and `tests/link/test_object_format_backend_
    // registry.cpp` mutates every required identity field of every shipped
    // format to prove the rules are still RUNNING, not merely still present.
    if (backend != nullptr) {
        backend->validateIdentity(*this, fail);
    }

    // Sections must NOT carry a segment name unless this format's own backend
    // uses the two-level (segment, section) naming — the field is Mach-O's.
    // Reject explicitly so a JSON edit can't silently no-op.
    //
    // WAS `if (kind == ObjectFormatKind::Elf || kind == ObjectFormatKind::Pe)`
    // — an ENUMERATION of the formats that existed when the rule was written,
    // which is exactly why it said nothing at all about WASM or SPIR-V. Asking
    // for the CAPABILITY makes every backend answer, including ones written
    // after the rule. Null backend ⇒ skipped; an unresolved format has already
    // failed at `/format/kind` above and nothing downstream should read it.
    if (backend != nullptr && !backend->sectionsCarrySegmentNames()) {
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (!sections[i].segment.empty()) {
                // ⚠ The wording still says "for ELF/PE rows" while the RULE now
                // covers every backend that does not carry segment names (i.e.
                // also WASM and SPIR-V, and any future one). Kept verbatim on
                // purpose: `tests/link/` matches diagnostic text, and this
                // cycle's contract was that the rules move and their messages
                // and pointers do not. Widening the sentence is a separate,
                // message-only change — flagged by an independent audit and
                // recorded here rather than smuggled into a refactor whose
                // whole claim is that nothing observable moved.
                fail(std::format("/sections/{}/segment", i),
                     std::format("section '{}': 'segment' must be empty "
                                 "for ELF/PE rows (only Mach-O uses the "
                                 "two-level (segment, section) naming)",
                                 sections[i].name));
            }
        }
    }

    // Cross-format exec-flavor invariant (type-design Q5 convergence
    // + type-design O1 post-audit fold). Since c150 this predicate
    // is DELIBERATELY NARROWER than `isImageFlavor()`: an ELF ET_DYN
    // `.so` is an image flavor (load-time-bound) but NOT an exec
    // flavor — it has no entry, so the entry-machinery legality
    // gates below (processExit / entryCallingConvention /
    // processArgs) exclude it (the dyn Text-row + VA==pageAlign rule
    // lives in the ELF block above). c151 (D-LK1-4 PIE half): an
    // ET_DYN schema carrying the FULL entry cluster (interpreter +
    // processExit + entryCallingConvention + processArgs — the
    // all-or-none rule above already rejected partial states) is a
    // PIE EXECUTABLE and joins the exec-flavor set, making its entry
    // machinery legal here. The cluster is re-derived from the same
    // four fields (single source of truth with the ELF-block rule).
    // c152 (D-LK2-4): PE Dll likewise LEAVES the exec-flavor set — a
    // DSS `.dll` has no entry (AddressOfEntryPoint = 0, no DllMain;
    // the PE-block Dll rules above reject its entry cluster with the
    // precise message, and these generic gates back them up). The
    // DllMain follow-up (D-LK2-DLL-DLLMAIN-ENTRY) would rejoin via a
    // cluster discriminator, the c151 PIE pattern. c153 (D-LK3-3):
    // Mach-O MH_DYLIB completes the entry-less-library trio — the
    // `macho.filetype == Execute` arm below excludes it by
    // construction, and the Mach-O dylib block above rejects its
    // entry cluster with the precise message.
    //
    // A single source of truth tying the image-side triplet:
    //   format declares "executable mode" ⟺ a Text-section row
    //   declares a non-zero virtualAddress (where to load it).
    // `entryPoint` is independent (empty defaults to functions[0];
    // non-empty resolves by name) — NOT cross-tied here. All exec
    // arms (ELF ET_EXEC + ET_DYN-PIE, PE PE32+ Exec, Mach-O
    // MH_EXECUTE) inherit this gate uniformly; the entry-less image
    // shapes (ELF `.so`, PE Dll) carry their own Text-row/VA rules in
    // their format blocks above (the PE `!= Obj` virtualAddress rules
    // cover Exec AND Dll; the Dll block adds the Text-row-required
    // rule).
    // D-LK10-ENTRY entry-gate fold: the four arms moved to
    // `ObjectFormatData::isExecFlavor()` (object_format_schema.hpp) and are
    // read from there by BOTH this rule set and the public
    // `ObjectFormatSchema::isExecFlavor()` the linker's trampoline gate asks.
    // One implementation on purpose — a second copy is how a config the loader
    // accepts and a gate the linker applies come to disagree.
    bool const isExecFlavor = this->isExecFlavor();
    if (isExecFlavor) {
        // Walker requires Text + virtualAddress != 0 to compute
        // e_entry / p_vaddr / IMAGE_OPTIONAL_HEADER.ImageBase. The
        // per-format rule above already rejects ELF ET_EXEC with
        // text.virtualAddress == 0; this terminal pass restates the
        // contract uniformly so PE/MachO image arms inherit the
        // gate the same way (one rule covers all 3 formats).
        bool sawText = false;
        for (auto const& s : sections) {
            if (s.kind != SectionKind::Text) continue;
            sawText = true;
            if (s.virtualAddress == 0) {
                fail("/sections/<text>/virtualAddress",
                     "image-flavor format (ELF ET_EXEC / PE PE32+ / "
                     "Mach-O MH_EXECUTE) requires the Text section "
                     "row's `virtualAddress != 0`. The walker computes "
                     "the entry-point VA from this field; a value of "
                     "0 would emit an image loaded at virtual address "
                     "0, which the runtime kernel rejects as ENOEXEC.");
            }
        }
        if (!sawText) {
            fail("/sections",
                 "image-flavor format requires a Text section row "
                 "(SectionKind::Text). No such row was declared.");
        }
    }

    // D-LK10-ENTRY Slice B (plan 14 §2.13): cross-field coherence
    // between `processExit` and `entryCallingConvention`. Both go
    // together — the trampoline emitter needs both to construct the
    // LIR sequence (mechanism dispatch + status-arg-register
    // lookup). Declaring one without the other is a silent
    // under-spec that would surface only at Slice C emitter time.
    if (processExit.has_value() && entryCallingConvention.empty()) {
        fail("/entryCallingConvention",
             "format declares `processExit` but `entryCallingConvention`"
             " is empty — Slice C trampoline emitter requires the "
             "active calling convention's name to look up "
             "argGprs[0] (status-arg register). Both fields must "
             "be declared together. (D-LK10-ENTRY §2.13.)");
    }
    if (!processExit.has_value() && !entryCallingConvention.empty()) {
        fail("/processExit",
             "format declares `entryCallingConvention` but no "
             "`processExit` block — both fields are paired "
             "(D-LK10-ENTRY §2.13). Either declare both or "
             "neither.");
    }
    // ═══════════════════════════════════════════════════════════════
    // THE `processExit` ⟺ `isExecFlavor` BICONDITIONAL.
    //
    // Stated here as ONE rule with TWO enforcement halves, deliberately
    // adjacent, because the two halves used to be the same distance apart
    // as this comment is long — and a rule whose halves live 60 lines
    // apart is a rule one of whose halves gets weakened later without
    // anyone noticing the other exists.
    //
    //   ⇐ half (the older one, silent-failure H1 / 7425905 audit fold):
    //     `processExit` (and its paired `entryCallingConvention`) are
    //     meaningful ONLY on exec-flavored formats — the trampoline
    //     emitter never runs on a relocatable (.o / Obj / Object) or on
    //     an entry-less image (ELF `.so`, PE Dll, Mach-O MH_DYLIB).
    //     Declaring them there is dead data that would silently confuse
    //     anyone diffing format schemas.
    //
    //   ⇒ half (D-LK10-ENTRY entry-gate fold): an exec-flavored format
    //     MUST declare `processExit`. DSS ALWAYS synthesises an entry
    //     trampoline for an exec-flavored format — that is DSS POLICY,
    //     not a platform fact — so a format that declares no exit
    //     mechanism gives the trampoline emitter nothing to call.
    //     Rejecting at CONFIG-LOAD time is what makes the linker's
    //     runtime gate (link/linker.cpp, K_FormatLacksProcessExit)
    //     unreachable from the shipped-config path: the config never
    //     survives to reach it.
    //     MEASURED before this rule existed: `int main(void){return 42;}`
    //     at `--target x86_64:macho64-x86_64-darwin-exec` — whose format
    //     JSON then declared no `processExit` — produced rc=0, a
    //     4162-byte artifact and `LC_MAIN entryoff=0x1000` pointing at
    //     `main`'s own `48 81 ec 10 00 00 00` (`sub rsp,0x10`) prologue,
    //     with ZERO diagnostics.
    //
    // ★ THE ⇒ HALF HAS A KNOWN-VACUOUS ARM, AND IT IS ACCEPTABLE.
    // `isExecFlavor()` has four arms. Three of them — ELF ET_EXEC, PE
    // PE32+ Exec, Mach-O MH_EXECUTE — derive purely from `objectType` /
    // `filetype`, and the ⇒ half has full teeth there. The FOURTH, ELF
    // ET_DYN, qualifies only through `elfDynPieShape`, which ITSELF
    // includes `processExit.has_value()` as one of its four cluster
    // members — so on that arm "exec-flavored ⇒ declares processExit" is
    // a TAUTOLOGY and enforces nothing.
    // ★ WHAT ACTUALLY COVERS ET_DYN, STATED PER TIER (corrected by the
    // TF-C120 audit, which caught this paragraph claiming ONE rule was
    // "stronger" without saying WHERE it runs):
    //   * AT CONFIG LOAD — the all-or-none `clusterCount` check above
    //     (the `/elf` PARTIAL-cluster failure), which rejects any 1-, 2-
    //     or 3-of-4 state naming exactly which members are missing. It
    //     IS stronger than the ⇒ half. But it is a `validate()` rule, so
    //     it runs on `loadShipped` / `loadFromFile` / `loadFromText` and
    //     NOWHERE ELSE.
    //   * IN MEMORY (`ObjectFormatSchema{ObjectFormatData}` — a public
    //     constructor that runs no validation, the tier the linker and
    //     walker gates defend) — `clusterCount` is ABSENT. The cover
    //     there is the ELF walker's own half-cluster belt in
    //     `elf::encodeElfExecDynamic` (`isPie != !interpreter.empty()`),
    //     which refuses loud and emits no bytes. MEASURED by
    //     `EntryGateFold.ElfWalkerRefusesHandBuiltEtDynPartialEntryCluster`
    //     (tests/link/test_lk10_entry_slice_c.cpp). That belt reads the
    //     two cluster members the walker itself consumes, so a 3-of-4
    //     ET_DYN missing only `processExit` — which satisfies neither
    //     `isExecFlavor()` nor `processExit()` and is therefore invisible
    //     to both entry-gate predicates — still cannot emit an image.
    // Said out loud here so nobody reads the ⇒ half as covering ET_DYN,
    // and so no test claims to cover the dyn arm of the ⇒ half — such a
    // test would be asserting a truth of the predicate's own definition.
    // ═══════════════════════════════════════════════════════════════
    if (isExecFlavor && !processExit.has_value()) {
        fail("/processExit",
             "exec-flavored format (ELF ET_EXEC / ELF ET_DYN PIE with the "
             "full entry cluster / PE PE32+ Exec / Mach-O MH_EXECUTE) must "
             "declare a `processExit` block. DSS ALWAYS synthesises an entry "
             "trampoline on an exec-flavored format -- the image entry is the "
             "trampoline, and the trampoline's last act is to call the "
             "mechanism this key names -- so a format that declares none "
             "leaves the emitter nothing to call. Without this rule the "
             "linker SKIPPED trampoline synthesis with no diagnostic and the "
             "emitted image's entry pointed straight at the user's first "
             "function. Declare `processExit` (mechanism `syscall` or "
             "`by-name-import`) together with `entryCallingConvention`, or "
             "make this format non-exec. (D-LK10-ENTRY 2.13.) NOTE: this is "
             "a DSS policy about how DSS builds entries, NOT a claim that "
             "the platform cannot terminate otherwise -- a Mach-O LC_MAIN "
             "entry, for one, is CALLED by dyld rather than jumped to, so "
             "the platform would cope; DSS's own entry shape will not.");
    }
    if (processExit.has_value() && !isExecFlavor) {
        fail("/processExit",
             "processExit is only legal on exec-flavored formats "
             "(ELF ET_EXEC / ELF ET_DYN PIE with the full entry "
             "cluster / PE PE32+ Exec / Mach-O MH_EXECUTE). "
             "Relocatable artifacts (.o / Obj / Object) cannot have "
             "an entry trampoline, and the entry-less library shapes "
             "(ELF ET_DYN .so, PE Dll, Mach-O MH_DYLIB) have no entry. "
             "(D-LK10-ENTRY 2.13 / D-LK1-4 / D-LK2-4 / D-LK3-3.)");
    }
    if (!entryCallingConvention.empty() && !isExecFlavor) {
        fail("/entryCallingConvention",
             "entryCallingConvention is only legal on exec-flavored "
             "formats — relocatable artifacts have no entry "
             "trampoline to resolve a cc against. (D-LK10-ENTRY §2.13.)");
    }

    // D-RUNTIME-MAIN-ARGC-ARGV (c88): `processArgs` rides the SAME
    // trampoline emitter as `processExit` — it is meaningless without
    // one (the emitter fails loud when processExit is absent, so a
    // processArgs-only format would be dead config whose argument
    // setup silently never emits). Same exec-flavor gate as
    // processExit: relocatable artifacts have no entry trampoline.
    if (processArgs.has_value() && !processExit.has_value()) {
        fail("/processArgs",
             "format declares `processArgs` but no `processExit` block "
             "— argument materialization is emitted by the entry "
             "trampoline, which requires a declared exit mechanism. "
             "Declare both or neither. (D-RUNTIME-MAIN-ARGC-ARGV.)");
    }
    if (processArgs.has_value() && !isExecFlavor) {
        fail("/processArgs",
             "processArgs is only legal on exec-flavored formats "
             "(ELF ET_EXEC / ELF ET_DYN PIE with the full entry "
             "cluster / PE PE32+ Exec / Mach-O MH_EXECUTE). "
             "Relocatable artifacts (.o / Obj / Object) have no entry "
             "trampoline to materialize arguments in, and the "
             "entry-less library shapes (ELF ET_DYN .so, PE Dll, "
             "Mach-O MH_DYLIB) have no entry. "
             "(D-RUNTIME-MAIN-ARGC-ARGV.)");
    }

    // ── UCRT-P4 (D-FFI-PE-CRT-UCRT-MIGRATION): the ROLE TABLE
    //    BICONDITIONAL, enforced at THIS tier as well as at load ────────────
    //
    // The loader already refuses a block naming a role the table does not
    // declare. This arm is the OTHER tier — the one a hand-built
    // `ObjectFormatData` reaches, since `ObjectFormatSchema{ObjectFormatData}`
    // is a public constructor that runs no validation — and it enforces BOTH
    // directions:
    //   (a) every block that names a role must resolve to the table's image for
    //       that role, byte for byte. A hand-built schema that set a block's
    //       resolved `…LibraryPath` WITHOUT the table (or against a different
    //       image) is exactly the two-owners-that-nothing-forces-to-agree defect
    //       this table exists to delete, so it fails here rather than emitting
    //       an import nobody declared.
    //   (b) every table row must be NAMED by some block. An unnamed row is inert
    //       config, and this file already rejects inert config by name (see the
    //       `charSignedness` note in the header). It is also how a stale row
    //       would survive a migration — the very thing the mechanical
    //       "no pe table names msvcrt.dll" exit criterion is checked against.
    {
        // (role → image) pairs each spine block CLAIMS, gathered generically:
        // nothing here knows which block owns which role.
        struct RoleClaim { RuntimeLibraryRole role; std::string_view image;
                           char const* where; };
        std::vector<RoleClaim> claims;
        if (processExit.has_value()
            && processExit->role != RuntimeLibraryRole::None) {
            claims.push_back({processExit->role, processExit->importLibraryPath,
                              "/processExit"});
        }
        if (processArgs.has_value()
            && processArgs->role != RuntimeLibraryRole::None) {
            claims.push_back({processArgs->role, processArgs->crtLibraryPath,
                              "/processArgs"});
        }
        if (sehPersonality.has_value()
            && sehPersonality->role != RuntimeLibraryRole::None) {
            claims.push_back({sehPersonality->role,
                              sehPersonality->libraryPath, "/sehPersonality"});
        }
        if (librarySynthesis.has_value()
            && librarySynthesis->role != RuntimeLibraryRole::None) {
            claims.push_back({librarySynthesis->role,
                              librarySynthesis->libraryPath,
                              "/librarySynthesis"});
        }
        for (auto const& c : claims) {
            auto const image = runtimeLibraries.imageForRole(c.role);
            if (!image.has_value()) {
                fail(c.where,
                     std::format(
                         "names runtime-library role '{}' but this format's "
                         "`runtimeLibraries` table does not declare it — the "
                         "block's import would be bound to no image. "
                         "(D-FFI-PE-CRT-UCRT-MIGRATION.)",
                         runtimeLibraryRoleName(c.role)));
            } else if (*image != c.image) {
                fail(c.where,
                     std::format(
                         "resolved image '{}' disagrees with the "
                         "`runtimeLibraries` row for role '{}', which declares "
                         "'{}'. The role table is the SINGLE OWNER of this "
                         "fact; the block's path is a derived copy, and two "
                         "copies that disagree is the defect the table "
                         "removes. (D-FFI-PE-CRT-UCRT-MIGRATION.)",
                         std::string{c.image},
                         runtimeLibraryRoleName(c.role),
                         std::string{*image}));
            }
        }
        // ⚠ THE OTHER DIRECTION — "a table row NO block names is inert config" —
        // is enforced in the JSON LOADER, not here, and the reason is a MEASURED
        // cascade rather than convenience. This tier sees only what LOADED: when a
        // block is rejected for an unrelated defect (say `processExit` omitting
        // `importMangledName`), `data.processExit` is never populated, so its role
        // CLAIM disappears and the row it named looks unnamed — producing a second,
        // misleading diagnostic about the table on top of the real one. MEASURED
        // while building this cycle: `LK10EntrySliceB.ByNameImportArmMissing-
        // MangledNameRejected` gained exactly that spurious `/runtimeLibraries`
        // error. The LOADER reads each block's `role` key straight out of the JSON,
        // so it knows the DECLARED intent whether or not the block validated, and
        // its answer cannot cascade. The cost is that a hand-built
        // `ObjectFormatData` carrying an inert row is not caught — accepted,
        // because an inert row is a config-AUTHORING fault and the JSON tier is
        // where config is authored (contrast direction (a) above, which guards a
        // real MISCOMPILE and therefore must hold on both paths).
    }

    // ── UCRT-P4 (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE): `entryVerbs` ⟺
    //    exec-flavored ─────────────────────────────────────────────
    //
    // The SAME paired-cluster discipline `processExit` / `entryCallingConvention`
    // follow, and for the same reason in both directions. A format that STARTS a
    // program must declare what it can hand an entry: with an empty set NOTHING
    // the source language declares can survive the intersection, so every program
    // would be refused as entry-less; with no set at all, "realizes anything"
    // restores the MEASURED defect (a `wmain`-only source selected as the ELF
    // entry, and a 3-param `main` accepted rc=0 on pe64 AND elf64, faulting at
    // run). A format that starts NOTHING must not carry verbs: they would be
    // unreachable config nothing ever consults.
    //
    // ⚠ THE ELF ET_DYN CAVEAT THAT MAKES `processExit`'s OWN RULE A TAUTOLOGY
    // DOES NOT APPLY HERE. `elfDynPieShape` counts `processExit.has_value()` as
    // one of its cluster members, which is why "exec-flavored ⇒ declares
    // processExit" enforces nothing on the dyn arm. `entryVerbs` is NOT a
    // cluster member of any `isExecFlavor()` arm, so this rule has real teeth on
    // all four arms. Do NOT add `entryVerbs` to `elfDynPieShape` — that would
    // silently hollow this rule out exactly as the other one was hollowed.
    //
    // ★ AND THIS RULE IS WHAT LETS THE ENGINE USE EMPTINESS AS ITS PREDICATE.
    // "Does this build need a program entry" is answered by `entryVerbs.empty()`
    // rather than by `isExecFlavor()` or a format name, which is only sound
    // because this rule pins the two equivalent in BOTH directions. Weakening
    // either direction turns that predicate into a silent lie — the exec arm
    // would stop requiring an entry, or a relocatable `.o` would start demanding
    // one.
    if (isExecFlavor && entryVerbs.empty()) {
        fail("/entryVerbs",
             "exec-flavored format declares NO `entryVerbs` — every format that "
             "starts a program must declare which program-entry materialization "
             "verbs it can realize. That set is intersected with the source "
             "language's declared entry signatures to select the program entry, "
             "so an absent set leaves NO candidate and an unchecked one restores "
             "the MEASURED defect: a 3-parameter `main` that compiles rc=0 with "
             "zero diagnostics and faults on its first envp dereference, and a "
             "`wmain`-only source selected as the ELF entry. C23 5.1.2.2.1 "
             "permits an implementation-defined entry set and C23 3.4.1 requires "
             "the implementation to DOCUMENT it; this key and the language's "
             "`entryFunctions` mapping are jointly that documentation. "
             "(D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE.)");
    }
    if (!entryVerbs.empty() && !isExecFlavor) {
        fail("/entryVerbs",
             "entryVerbs is only legal on exec-flavored formats (ELF ET_EXEC / "
             "ELF ET_DYN PIE with the full entry cluster / PE PE32+ Exec / "
             "Mach-O MH_EXECUTE). A relocatable artifact and an entry-less "
             "library image both resolve NO program entry, so the verbs would "
             "never be consulted — and the engine reads this emptiness as "
             "\"needs no entry\", which a stray verb would falsify. "
             "(D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE.)");
    }

    // A materialization verb that needs argument setup is only realizable if the
    // format actually declares HOW it obtains arguments — the two axes are
    // independent (verb = WHICH arguments, `processArgs.mechanism` = HOW), but
    // "which" without any "how" on a format whose loader delivers nothing would
    // call the entry on uninitialized registers. Mach-O legitimately declares NO
    // `processArgs` because dyld puts argc/argv in the argument registers BEFORE
    // any DSS code runs, so absence is a real answer there and this rule cannot
    // simply require the block — it requires the pairing to be COHERENT, which
    // for the shipped mechanisms means: a CRT-accessor mechanism must be paired
    // with at least one verb that consumes it, or the accessor names are dead.
    if (processArgs.has_value()
        && processArgs->mechanism == ArgsMechanism::CrtArgvAccessors) {
        bool anyMaterializing = false;
        for (auto const v : entryVerbs) {
            if (v != EntryMaterialization::None) {
                anyMaterializing = true;
                break;
            }
        }
        if (!anyMaterializing) {
            fail("/processArgs",
                 "declares the `crt-argv-accessors` mechanism but `entryVerbs` "
                 "names no verb that consumes it — the five declared CRT export "
                 "names would never be emitted. Declare the argc/argv verb(s) "
                 "this format realizes, or drop the mechanism. "
                 "(D-FFI-PE-CRT-UCRT-MIGRATION.)");
        }
    }

    // D-LK-OBJECT-DATA-SECTION-RELOCATABLE: `supportedDataSections` is NO
    // longer restricted to exec-flavored formats. A RELOCATABLE object DOES
    // carry data — a global lands in `.data`/`.rodata`/`.bss` with `sh_addr=0`
    // and a section-relative `.symtab` entry the final linker binds. So the
    // capability is legal on a relocatable schema too; legality is enforced
    // DOWNSTREAM (the walker fail-louds if it cannot actually emit a declared
    // section — the same ungated discipline `externCallDispatch` uses just
    // below). The prior D-LK2-RODATA reject (relocatable objects "emit rodata
    // via symbol tables, not the dataItems gate") described an INTENT the ELF
    // ET_REL writer never implemented; this anchor implements it, keeping the
    // dataItems producer path (which is already format-blind) as the single
    // source of the section bytes + symbols for BOTH exec and relocatable.

    // D-FFI-EXTERN-CALL-DISPATCH: `externCallDispatch` is NOT validate-
    // required, even on exec formats. The precise requirement is "a format
    // that LOWERS AN EXTERN CALL needs a dispatch shape", which is enforced
    // exactly at MIR→LIR (a module with extern imports under a no-dispatch
    // format fails loud — `L_RequiredLirOpcodeMissing`, pinned by
    // `MirToLir.ExternImportsWithNoDispatchFailLoud`). Requiring it on EVERY
    // exec format would over-broadly force formats built for non-FFI
    // purposes (e.g. the codesign-placeholder fixtures) to carry an
    // unrelated field. The shipped exec formats DO declare it (and their FFI
    // corpora exercise it); an unknown VALUE still fails loud at load (the
    // loader's enum check). This keeps the "no silent default to a broken
    // call shape" invariant at the point it actually matters.

    return problems;
}

} // namespace detail

} // namespace dss
