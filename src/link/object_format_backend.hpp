#pragma once

#include "core/export.hpp"
#include "core/types/config_key_vocabulary.hpp"  // detail::renderAllowedList — the ONE closed-set renderer
#include "core/types/object_format_kind.hpp"  // ObjectFormatKind (the BRIDGE type — see the tier note)

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
// `ObjectFormatBackend` — the format-identity SEAM.
// D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125).
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ WHAT THIS TYPE EXISTS TO MAKE IMPOSSIBLE.
//
// Before TF-C125 the object-format schema triple
// (`object_format_schema.{hpp,cpp,_json.cpp}`) carried **26 enumerator
// comparisons and 2 kind-keyed tables** — `if (kind == ObjectFormatKind::Elf)`
// gating the ELF identity rules, a 3-arm `switch` behind `isImageFlavor()`, a
// `kCrossKindRules[]` table pairing each identity BLOCK with the kind that may
// declare it, and so on. Every one of them is the shape the bar names as a HARD
// VETO: shared substrate branching on WHICH format it is serving.
//
// The operator's 2026-08-06 ruling was unqualified — *"we must never have
// identity branches, so this must be addressed"* — and explicitly REJECTED the
// standing defence that a schema LOADER may branch because it must know which
// sub-schema applies. This interface is the answer: the loader no longer knows.
// It holds a `ObjectFormatBackend const*`, asks it questions, and never learns
// what it is talking to.
//
// ★★ AND IT REJECTS THE NEAR-MISS THAT LOOKS LIKE A FIX.
//
// Replacing N `if (kind == X)` with ONE table keyed on `ObjectFormatKind` is
// **not** a fix — it is the same dependency in a data-structure costume, and
// this tree argued for exactly that twice, in prose, at
// `object_format_schema_json.cpp` (*"expressed as a TABLE … not an if-chain"*)
// and at `object_format_kind.hpp` (*"a config-coherence rule, not an engine
// branch"*). Both comments were STRUCK in TF-C125, because a rationalization
// left in the tree re-authorizes the pattern it excuses. The precedent that
// governs is `kCManglingRules`, which TF-C122 **deleted** rather than moved.
//
// So: nothing in this header enumerates formats. The registry is a `span` the
// substrate is HANDED (below); the substrate iterates it and never indexes it
// by identity.
//
// ★ CAPABILITY QUERIES, NEVER KIND QUERIES — the load-bearing interface rule.
//
// Every predicate below asks *what can this format do*, not *which format is
// this*. That is not stylistic. `stackReserveControl` is the standing proof
// that capability is not uniform even WITHIN one kind: `pe64-x86_64-windows-
// exec` declares it and `pe64-x86_64-windows-dll` does not, because the Windows
// loader reads the field on an .exe and ignores it on a .dll. Any interface
// that offered `isPe()` would hand a caller the wrong answer for one of those
// two files while looking correct. There is deliberately no such method.
//
// ★ TIER. This header is SHARED SUBSTRATE (`src/link/`). The implementations
// live in `src/link/format/`, the sanctioned realization tier — that is the
// whole point of the split: a Mach-O backend may know it is Mach-O; the loader
// that drives it may not. `ObjectFormatKind` survives here as ONE bridge
// accessor (`kind()`) whose only purpose is to answer the pre-existing
// out-of-tier callers in `src/program/` and `src/ffi/` that still speak the
// enum. The substrate never COMPARES it — and the two schema TUs cannot even
// SPELL it (see the compile-error pin at the top of `object_format_schema.cpp`
// and `object_format_schema_json.cpp`).

namespace dss {

// Forward declarations — the interface passes everything by reference, so no
// consumer of this header pays for the schema / assembler / diagnostic tiers.
namespace detail { struct ObjectFormatData; }
namespace substrate { class DiagnosticCollector; }

class ObjectFormatSchema;
class TargetSchema;
class DiagnosticReporter;
struct AssembledModule;
struct ImageRequest;
// `StackReserveVehicle` comes complete from `core/types/object_format_kind.hpp`
// above — `std::span<StackReserveVehicle const>` needs the complete type.

namespace link {

// ★ THE PARSED DOCUMENT, HELD OPAQUE — and this is a real constraint, not
// tidiness. `link/object_format_schema.hpp` (which includes this header) is
// pulled in by targets that do NOT link nlohmann and do not have its include
// directory: MEASURED at the first TF-C125 build, where `src/mir/merge/
// synth_threads_shim.cpp` and `src/ffi/ingest.cpp` failed with "nlohmann/
// json_fwd.hpp: No such file or directory". Even the fwd header is too much to
// ask of them. Forward-declaring `nlohmann::json` by hand is worse: it is
// `basic_json<>` inside an ABI-VERSIONED inline namespace, so the declaration
// would silently rot on an nlohmann upgrade.
//
// So the substrate names a type it never defines. `object_format_identity_doc
// .hpp` — included only by the loader and the five backends — completes it.
class ObjectFormatIdentityDoc;

// The callback `ObjectFormatData::validate()` hands a backend so the backend
// can append problems to the caller's list without knowing how it is stored.
// (path, message) — `path` is the JSON pointer the diagnostic is pinned at.
using SchemaProblemSink = std::function<void(std::string, std::string)>;

class DSS_EXPORT ObjectFormatBackend {
public:
    ObjectFormatBackend()                                      = default;
    ObjectFormatBackend(ObjectFormatBackend const&)            = delete;
    ObjectFormatBackend& operator=(ObjectFormatBackend const&) = delete;
    virtual ~ObjectFormatBackend()                             = default;

    // ── Self-description ────────────────────────────────────────────────

    // The spelling this backend answers to in a `.format.json`'s `"kind"`
    // field ("elf" / "pe" / "macho" / "wasm" / "spirv"). THE resolution key:
    // `objectFormatBackendByConfigName()` matches on exactly this, so a
    // backend that ships without a name is unreachable rather than
    // accidentally-default.
    [[nodiscard]] virtual std::string_view configName() const noexcept = 0;

    // ★ THE BRIDGE, AND THE ONLY REASON `ObjectFormatKind` APPEARS IN THIS
    // HEADER. `src/program/` and `src/ffi/` still hold identity branches of
    // this same species (MEASURED TF-C125: `target_spec.cpp`'s output-EXTENSION
    // switch, `cross_validate_target_format.cpp`'s machine-code switch,
    // `compile_pipeline.cpp`'s archive-member reader switch, `program.cpp`'s
    // `.obj`/`.o` pick, and `abi_catalog.cpp`'s calling-convention table).
    // Those are anchored, NOT fixed here — they sit outside this cycle's
    // authorized file set. Until they close, `ObjectFormatSchema::kind()` must
    // keep answering, and it answers from here.
    //
    // ⚠ ONE `src/link/` CALLER REMAINS, and an earlier draft of this comment
    // wrongly said there were none (caught by an independent audit).
    // `linker.cpp` sets `LinkedImage::format = schema.kind()` — a datum CARRIED
    // OUT for consumers, asserted by three tests, and explicitly never branched
    // on (`writer.cpp`: "Nothing here may ever branch on `image.format`";
    // MEASURED — no `==`, switch or table keyed on it anywhere in `src/`).
    // It is a bridge value, not a bridge branch. It goes when the field does.
    [[nodiscard]] virtual ObjectFormatKind kind() const noexcept = 0;

    // ── Config vocabulary this backend OWNS ─────────────────────────────

    // Root JSON blocks whose contents only this backend reads ("elf";
    // "pe" + "optionalHeader"; "macho" + "image"). Replaces the
    // `kCrossKindRules[]` table: the loader unions every backend's list,
    // and rejects a block present in the document but owned by somebody
    // else. Generic set membership — the loader never learns which names
    // belong to whom.
    [[nodiscard]] virtual std::span<char const* const>
    identityBlockNames() const noexcept = 0;

    // Root JSON keys that are MEANINGLESS for this backend and must be
    // rejected loudly rather than silently ignored (the WASM/SPIR-V
    // `sections`/`relocations`/`entryPoint`/… list). Replaces
    // `if (data.kind == Wasm || data.kind == Spirv)`. Empty for a backend
    // that rejects nothing.
    [[nodiscard]] virtual std::span<char const* const>
    rejectedRootFields() const noexcept = 0;

    // WHY this backend rejects them, as a sentence the loader splices into the
    // refusal. It belongs to the backend for the same reason the LIST does: the
    // substrate emitter used to hardcode "WASM / SPIR-V emit this information
    // through their own format-native section vocabulary (plan 18 / plan 17)",
    // which is correct today and wrong the day a sixth backend declares one
    // rejected key for an entirely different reason. Empty for a backend that
    // rejects nothing (the loop never runs).
    [[nodiscard]] virtual std::string_view
    rejectedRootFieldsReason() const noexcept = 0;

    // The `stackReserveControl.vehicle` values this backend's WALKER
    // actually implements. Replaces `kVehicleKinds[]`. A schema declaring a
    // vehicle no backend claims, or one claimed by a DIFFERENT backend, is
    // rejected at load — the config-coherence rule survives, keyed on
    // declared capability instead of on identity. Empty for every backend
    // whose walker implements none.
    [[nodiscard]] virtual std::span<StackReserveVehicle const>
    stackReserveVehicles() const noexcept = 0;

    // ── Capability predicates (NEVER identity predicates) ───────────────

    // Does this schema describe a LOAD-TIME-BOUND image (ELF ET_EXEC/ET_DYN,
    // PE Exec/Dll, Mach-O MH_EXECUTE/MH_DYLIB) rather than relocatable
    // section bytes a later linker binds? Replaces the 3-arm switch that was
    // `ObjectFormatSchema::isImageFlavor()`.
    [[nodiscard]] virtual bool
    isImageFlavor(detail::ObjectFormatData const& d) const noexcept = 0;

    // Does this schema describe a PROGRAM THE OS STARTS?
    //
    // ★★ THE DERIVATION MOVES; IT DOES NOT COLLAPSE. The old
    // `ObjectFormatData::isExecFlavor()` re-derived the ELF ET_DYN PIE shape
    // from ALL FOUR cluster members (objectType + interpreter + processExit +
    // entryCallingConvention + processArgs) specifically so a HAND-BUILT,
    // validate-bypassing `ObjectFormatData` could not fake a PIE by setting
    // one field. `ObjectFormatSchema{ObjectFormatData}` is a public
    // constructor that runs no validation, so that path is real and reachable.
    // Replacing the derivation with a declared `execFlavor: true` boolean
    // would hand the single-field fake straight back. The ELF backend
    // therefore keeps the full four-member re-derivation verbatim — what
    // changed is WHO owns it, not WHAT it computes.
    [[nodiscard]] virtual bool
    isExecFlavor(detail::ObjectFormatData const& d) const noexcept = 0;

    // May the emitted artifact carry a REFERENCED extern that no library
    // binds (an undefined symbol a LATER binder resolves)? Replaces
    // `kind == Elf && elf.objectType == Dyn && !processExit.has_value()`.
    // The relocatable answer (always true) is decided by the CALLER from
    // `isImageFlavor()`, so a backend only answers for its image flavors.
    [[nodiscard]] virtual bool
    allowsUndefinedImports(detail::ObjectFormatData const& d) const noexcept = 0;

    // Is this schema's artifact a RELOCATABLE member — the shape that may be
    // bundled into an `ar` static archive? Replaces the `container: archive`
    // switch in `validate()`. False for every image flavor and for any format
    // with no `ar` concept.
    [[nodiscard]] virtual bool
    isRelocatableMember(detail::ObjectFormatData const& d) const noexcept = 0;

    // Does this backend use the two-level (segment, section) naming, i.e. is
    // a non-empty `sections[].segment` MEANINGFUL here? Replaces
    // `if (kind == Elf || kind == Pe)` guarding the "segment must be empty"
    // rule. Stated as the capability rather than as its complement so a new
    // backend answers for itself instead of being enumerated by a rule
    // written before it existed.
    [[nodiscard]] virtual bool sectionsCarrySegmentNames() const noexcept = 0;

    // ── Load + validate ─────────────────────────────────────────────────

    // Read this backend's own identity blocks out of the parsed document into
    // `data`. Called ONLY after the document resolved to this backend, so the
    // old `if (data.kind == K && doc.contains("<block>"))` pair collapses to
    // the `doc.contains` half. (The `data.kind ==` half was already DEAD in
    // five places — `kCrossKindRules` had emitted an Error for exactly those
    // (kind, block) pairs and `loadFromText` had already failed. MEASURED
    // TF-C122, INFERRED-only at the time; removing the branch removes the
    // question.)
    virtual void readIdentity(ObjectFormatIdentityDoc const&  doc,
                              detail::ObjectFormatData&       data,
                              substrate::DiagnosticCollector& coll) const = 0;

    // Validate this backend's identity block on an ALREADY-POPULATED
    // `ObjectFormatData` — including one built in memory that never saw the
    // JSON tier. Replaces the four `if (kind == K) { … }` rule blocks in
    // `ObjectFormatData::validate()`. Diagnostics MUST be pinned at the same
    // JSON pointers they used before (`/elf/machine`, `/pe/machine`,
    // `/macho/cputype`, …): the rules move, their pointers do not.
    virtual void validateIdentity(detail::ObjectFormatData const& d,
                                  SchemaProblemSink const&        fail) const = 0;

    // ── Emit ────────────────────────────────────────────────────────────

    // Encode a linked module into this format's bytes. Replaces the 6-arm
    // `switch (schema.kind())` in `linker.cpp`.
    //
    // ★ `request` is passed to EVERY backend, and that is safe for exactly
    // the reason the old switch's comment gave for passing it to only one:
    // the linker's pre-walker gate refuses any request whose vehicle this
    // schema does not declare, and `stackReserveVehicles()` above is what the
    // load-time rule checks that declaration against. A backend with no
    // vehicle can only ever be handed an EMPTY request. "Declared but
    // silently dropped" stays unreachable, now by a capability chain rather
    // than by a hand-maintained argument list.
    [[nodiscard]] virtual std::vector<std::uint8_t>
    encode(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& objectFormatSchema,
           DiagnosticReporter&       reporter,
           ImageRequest const&       request) const = 0;
};

// ─────────────────────────────────────────────────────────────────────────
// The registry.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★ HANDED TO THE SUBSTRATE, NOT SELF-REGISTERED. `objectFormatBackendTable()`
// is DECLARED here and DEFINED in exactly one non-substrate TU
// (`src/link/format/object_format_backends.cpp`). That direction is deliberate
// and the alternative was rejected with a reason: a static self-registration
// idiom (each backend TU registering itself from a file-scope initializer) is
// silently broken by MSVC, which discards an object file none of whose symbols
// are referenced — a format would vanish from the build with NO link error and
// the only symptom would be "that format stopped loading". A single table the
// substrate calls is a LINK-TIME dependency: delete the TU and the build fails.
[[nodiscard]] DSS_EXPORT std::span<ObjectFormatBackend const* const>
objectFormatBackendTable() noexcept;

// Resolve a `.format.json`'s declared `"kind"` spelling to its backend.
//
// ★★★ FAILS CLOSED, AND THAT IS THE FIRST THING TESTED. Returns `nullptr` for
// every unrecognized spelling — including `"unknown"`, the invalid sentinel,
// which no backend claims. Every caller in the schema tier treats `nullptr` as
// "this document declares no resolvable format" and REFUSES.
//
// The failure mode this guards is the catastrophic one for this whole design:
// if a null backend meant "skip the identity rules", all 24 shipped formats
// would validate CLEAN while validating NOTHING, and the negative tests would
// keep passing because their fixtures reject for other reasons too. The
// whole-family mutation probe in `tests/link/test_object_format_backend_
// registry.cpp` exists to make that state impossible to reach quietly: it
// mutates each required identity field of each shipped format and demands a
// reject AT THAT EXACT JSON POINTER.
[[nodiscard]] DSS_EXPORT ObjectFormatBackend const*
objectFormatBackendByConfigName(std::string_view configName) noexcept;

// Render a possibly-null backend for a DIAGNOSTIC. Byte-identical to the old
// `objectFormatKindName(schema.kind())` on every input — a resolved backend
// renders its config spelling, and null renders the same reserved `unknown`
// the enum's sentinel row spells — so relocating a guard to pointer identity
// does not change one character of any message a test matches on.
[[nodiscard]] inline std::string_view
objectFormatBackendName(ObjectFormatBackend const* backend) noexcept {
    return backend != nullptr ? backend->configName()
                              : kObjectFormatKindSentinelName;
}

// The set of `"kind"` spellings a `.format.json` may declare, rendered as the
// "expected one of …" half of a diagnostic.
// D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET.
//
// ★★ PROJECTED FROM THE REGISTRY, BECAUSE THE REGISTRY IS THE HALF THAT
// DECIDES. `objectFormatBackendByConfigName` resolves a declared spelling by
// walking `objectFormatBackendTable()` and asking each entry for its OWN name,
// so the accepted set is exactly this list — it is NOT
// `kSelectableObjectFormatKindNames`, which projects the ENUM table.
//
// ⚠ THOSE TWO ARE DIFFERENT OWNERS OF ONE FACT AND NOTHING PINS THEM TOGETHER.
// ✔MEASURED 2026-08-20 by reading `tests/link/test_object_format_backend_
// registry.cpp`: it pins that every registered name is non-empty, unique, and
// resolves back to its own entry — and never that the registered names ARE the
// selectable enum spellings. They coincide today at five. A sixth backend whose
// config name has no enum row (or an enum row no backend claims) would leave
// every "expected one of …" sentence in the schema tier quietly wrong, in the
// direction that tells a config author a spelling the loader ACCEPTS is not
// allowed. Rendering from the registry removes the second owner rather than
// pinning the two halves together.
//
// `sep` — the schema tier's house separator is ` / `; the call sites that read
// `'a', 'b'` pass `", "`. The QUOTING is not a parameter (see
// `renderAllowedList`).
[[nodiscard]] inline std::string
objectFormatBackendNameList(std::string_view sep = " / ") {
    std::vector<std::string_view> names;
    names.reserve(objectFormatBackendTable().size());
    for (auto const* b : objectFormatBackendTable()) {
        names.push_back(b->configName());
    }
    return ::dss::detail::renderAllowedList(names, sep);
}

} // namespace link
} // namespace dss
