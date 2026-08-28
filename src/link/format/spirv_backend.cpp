// ─────────────────────────────────────────────────────────────────────────
// SPIR-V object-format backend.
// D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125).
// ─────────────────────────────────────────────────────────────────────────
//
// This TU is the SANCTIONED REALIZATION TIER. It is allowed to know that it
// is SPIR-V — that is the whole reason the seam exists. What it must never
// do is let the shared substrate know: `object_format_schema.cpp` and
// `object_format_schema_json.cpp` reach this code only through the abstract
// `ObjectFormatBackend`, and they cannot even SPELL `ObjectFormatKind::Spirv`
// (both TUs carry a compile-error pin that makes the name ambiguous).
//
// ★ EVERY RULE BODY BELOW WAS MOVED VERBATIM. The `validateIdentity` and
// `readIdentity` bodies are byte-for-byte the blocks that used to sit behind
// `if (kind == ObjectFormatKind::Spirv)` in the schema triple — same rules,
// same wording, same JSON pointers. The pointers are load-bearing: 163
// `countAtPath` assertions across `tests/link/` pin diagnostics at exact
// pointers, and this refactor is only correct if every one of them still
// resolves. The rules moved; their pointers did not.
//
// The binding preamble at the top of `validateIdentity` re-establishes the
// unqualified member names the moved code used when it was a member of
// `ObjectFormatData::validate()`. Renaming those references instead would
// have made a 1,000-line move impossible to verify by inspection — and a rule
// that silently starts reading a different field is exactly the class of
// defect this refactor must not introduce.

#include "link/format/object_format_backends.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/format/spirv.hpp"
#include "link/object_format_schema.hpp"

#include "link/object_format_identity_doc.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dss::link::format {
namespace {

// ★ THE ROOT KEYS THIS FORMAT REJECTS — moved verbatim, per-key rationale and
// all, from the `if (data.kind == Wasm || data.kind == Spirv)` block in the
// loader. Every entry names a substrate-tier axis whose information this
// format emits through its OWN format-native vocabulary instead; a top-level
// declaration would be read by nobody and silently ignored, which is the
// dead-config class the list exists to make loud.
//
// The list is DUPLICATED between the WASM and SPIR-V backends rather than
// shared, and that is deliberate: the two agree today by coincidence of what
// neither has implemented yet, not by a shared rule. When plan 18 gives WASM a
// relocation vocabulary, WASM's list shrinks and SPIR-V's does not — and a
// shared constant would have made that a change to both.
char const* const kRejectedRootFields[] = {
            "sections", "relocations", "entryPoint",
            // dim-2 HIGH #3 (7425905 audit fold): D-LK10-ENTRY
            // Slice B fields are meaningless on Wasm/SPIR-V
            // (no trampoline emitter; their exit semantics are
            // format-native). Defense-in-depth: reject loudly
            // rather than silently accept dead data.
            "processExit", "entryCallingConvention",
            // D-RUNTIME-MAIN-ARGC-ARGV: the program-entry argument
            // mechanism rides the same trampoline emitter — dead
            // data on Wasm/SPIR-V for the same reason.
            "processArgs",
            // D-LK2-RODATA closure: the producer-data-section
            // capability is meaningless on Wasm/SPIR-V (their
            // walkers emit rodata via format-native section
            // vocabulary, not via the cross-format dataItems
            // pipeline).
            "supportedDataSections",
            // D-FFI-EXTERN-CALL-DISPATCH: the PLT-stub vs IAT-slot
            // extern-call shape is an ELF/PE/Mach-O dynamic-import
            // notion; WASM/SPIR-V reach imports through their own
            // format-native mechanisms (WASM import section / SPIR-V
            // linkage decorations), so a top-level declaration here
            // would be dead data.
            "externCallDispatch",
            // D-LK-EXTERN-DATA-IMPORT: the extern-DATA import binding
            // model (a loader-bound pointer slot) is likewise an ELF/PE/Mach-O
            // dynamic-import notion — dead data on WASM/SPIR-V.
            "dataImportBinding",
            // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): the extern-
            // ADDRESS materialization binding (GOT-slot page-pair) is
            // likewise an ELF/PE/Mach-O native-image notion — dead data
            // on WASM/SPIR-V (they reach imports format-natively).
            "externAddrBinding",
            // D-CSUBSET-THREAD-LOCAL (TLS C1): the thread-local access
            // model (segment-register / TEB / TLV descriptor) is an
            // ELF/PE/Mach-O native-image notion — dead data on
            // WASM/SPIR-V (their thread-local story is format-native
            // vocabulary when it lands).
            "tlsAccess",
            // D-CSUBSET-C11-THREADS-MACHO: the shipped-library synth
            // vehicle (kernel32 / pthread primitive family for a
            // compiler-synthesized shim) is an ELF/PE/Mach-O native-
            // runtime notion — dead data on WASM/SPIR-V.
            "librarySynthesis",
            // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the per-program stack-
            // reserve capability names an ELF/PE/Mach-O image header field.
            // WASM/SPIR-V have no such field (a WASM module's stack is the
            // embedder's; SPIR-V has no call stack of this shape), so a
            // top-level declaration would be dead data whose request the
            // walker would silently drop.
            "stackReserveControl",
            // ... and its remedy axis: WASM/SPIR-V do not participate in the
            // stack-reserve conversation at all, so neither key belongs.
            "stackReserveUnsupportedReason",
            // D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED: the weak-
            // DEFINITION spelling names a COFF COMDAT section, an ELF symbol
            // binding or a Mach-O n_desc flag. This walker encodes none of
            // them and never asks the schema which one it should use, so a
            // `weakDefinition` block here would be read by NOBODY — the dead-
            // config class this whole list exists to make loud.
            // ⚠ REJECTING THE KEY IS NOT A CLAIM THAT THE FORMAT CANNOT
            // EXPRESS A WEAK DEFINITION. SPIR-V's own linkage decorations have
            // their own answer to duplicate definitions; this row says only
            // that no reader of this key exists on this backend. If a SPIR-V
            // weak-definition encoder lands, it arrives WITH its dialect row
            // and this entry goes.
            "weakDefinition",
};

class SpirvBackend final : public ObjectFormatBackend {
public:
    [[nodiscard]] std::string_view configName() const noexcept override {
        return "spirv";
    }
    [[nodiscard]] ObjectFormatKind kind() const noexcept override {
        return ObjectFormatKind::Spirv;
    }
    // No identity block: this format declares nothing under a root key of its
    // own. The engine + JSON vocabulary arrive with plan 17.
    [[nodiscard]] std::span<char const* const>
    identityBlockNames() const noexcept override { return {}; }
    [[nodiscard]] std::span<char const* const>
    rejectedRootFields() const noexcept override { return kRejectedRootFields; }
    // The sentence the loader splices into the refusal. Byte-identical to the
    // wording that used to be hardcoded in the substrate emitter, so every
    // test matching on the message still matches — but it now travels WITH
    // the list it explains.
    [[nodiscard]] std::string_view
    rejectedRootFieldsReason() const noexcept override {
        return "WASM / SPIR-V emit this information through their own format-native section vocabulary (plan 18 / plan 17).";
    }
    [[nodiscard]] std::span<StackReserveVehicle const>
    stackReserveVehicles() const noexcept override { return {}; }
    [[nodiscard]] std::span<WeakDefinitionDialect const>
    weakDefinitionDialects() const noexcept override {
        // SPIR-V has no coalescing symbol model and this walker writes no weak
        // definition in any spelling. Claiming none is what makes a SPIR-V
        // document declaring `weakDefinition` fail loud at LOAD, instead of
        // carrying a key nobody reads. The row is
        // D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS.
        return {};
    }

    // Not a native image, not a program the OS starts, and not an `ar`
    // member — SPIR-V has no such shapes. `allowsUndefinedImports` is
    // unreachable here (the caller answers `true` from the non-image
    // short-circuit before asking), and answers the strict direction anyway.
    [[nodiscard]] bool isImageFlavor(
            detail::ObjectFormatData const&) const noexcept override {
        return false;
    }
    [[nodiscard]] bool isExecFlavor(
            detail::ObjectFormatData const&) const noexcept override {
        return false;
    }
    [[nodiscard]] bool allowsUndefinedImports(
            detail::ObjectFormatData const&) const noexcept override {
        return false;
    }
    [[nodiscard]] bool isRelocatableMember(
            detail::ObjectFormatData const&) const noexcept override {
        return false;
    }

    // SPIR-V ships no relocatable-object READER, so there is no shape for
    // this backend to recognize and no bytes it could hand a linker — the
    // strict answer, exactly as above. (A SPIR-V module DOES have a magic —
    // `0x07230203` — and the day a reader lands, THAT is the change that
    // earns a real check here; claiming recognition first would route a file
    // to a reader that does not exist.)
    [[nodiscard]] bool looksLikeRelocatableObject(
            detail::ObjectFormatData const&,
            std::span<std::uint8_t const>) const noexcept override {
        return false;
    }

    // ⚠ BEHAVIOUR WIDENED HERE, DELIBERATELY, AND SAID OUT LOUD. The rule this
    // feeds used to be `if (kind == Elf || kind == Pe)` — an ENUMERATION, which
    // by construction said nothing about WASM or SPIR-V, so a hand-built
    // `ObjectFormatData` for either could carry section rows with segment names
    // and `validate()` would shrug. Restating it as a capability makes this
    // backend answer for itself, and the answer is false, so those rows are now
    // rejected too. No shipped config can reach it (both formats reject
    // `sections` outright at the JSON tier, see the list above); the reachable
    // path is the in-memory one, and there the new answer is the correct one.
    [[nodiscard]] bool sectionsCarrySegmentNames() const noexcept override {
        return false;
    }

    void readIdentity(ObjectFormatIdentityDoc const&,
                      detail::ObjectFormatData&,
                      substrate::DiagnosticCollector&) const override {
        // No identity block to read.
    }
    void validateIdentity(detail::ObjectFormatData const&,
                          SchemaProblemSink const&) const override {
        // No identity block to validate. Every SPIR-V coherence rule that
        // exists today is a REJECTION of a substrate key, enforced at load
        // through `rejectedRootFields()` above.
    }

    [[nodiscard]] std::vector<std::uint8_t>
    encode(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& objectFormatSchema,
           DiagnosticReporter&       reporter,
           ImageRequest const&       request) const override {
        (void)request;  // no declared vehicle — see stackReserveVehicles()
        return spirv::encode(module, targetSchema, objectFormatSchema,
                                  reporter);
    }

    // D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES: this format has NO
    // relocatable-object reader, and says so ITSELF.
    //
    // ★ THE REFUSAL MOVED HERE FROM A SHARED `default:` ARM, AND THAT IS WHY
    // THE METHOD IS PURE VIRTUAL. A `default:` silently absorbs every format
    // nobody thought about; a pure virtual makes a SIXTH backend unable to
    // compile without answering. The wording and the F_ code are unchanged, so
    // the diagnostic a caller sees is byte-identical to the switch's.
    [[nodiscard]] std::optional<AssembledModule>
    readRelocatableObject(std::span<std::uint8_t const> bytes,
                          TargetSchema const&           targetSchema,
                          ObjectFormatSchema const&     objectFormatSchema,
                          DiagnosticReporter&           reporter,
                          CompilationUnitId             cuId) const override {
        (void)bytes; (void)targetSchema; (void)cuId;
        ParseDiagnostic d;
        d.code     = DiagnosticCode::F_UnsupportedBinaryFormat;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "archive member reader: object format '{}' (kind {}) has no "
            "relocatable-object reader -- cannot pull archive members for "
            "this format.",
            objectFormatSchema.name(),
            objectFormatKindName(objectFormatSchema.kind()));
        reporter.report(std::move(d));
        return std::nullopt;
    }

};

} // namespace

// ★ FUNCTION-LOCAL STATIC, NOT A NAMESPACE-SCOPE ONE, and the difference is
// load-bearing. `objectFormatBackendTable()` takes this object's ADDRESS from
// another TU. A namespace-scope static would make that a static-INITIALIZATION-
// ORDER dependency across translation units: the address is valid before
// construction, but the VTABLE POINTER is not, so a table built during static
// init would hold an object whose virtual calls are undefined. The magic-static
// below is constructed on first call, in order, always. (Unreachable today —
// nothing calls the table during static init — which is exactly why it would
// have been found the hard way.)
ObjectFormatBackend const& spirvBackend() noexcept {
    static SpirvBackend const instance{};
    return instance;
}

} // namespace dss::link::format
