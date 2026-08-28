#include "ffi/abi/abi_catalog.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <array>
#include <format>
#include <span>
#include <string>
#include <utility>

namespace dss::ffi {

namespace {

// ── D-FFI-ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY ────────
//
// THE CLOSED C++ TABLE `kAbiCatalog` STOOD HERE AND IS GONE, along with the
// `AbiCatalogRow` type, the `abiCatalogTable()` accessor, its `consteval`
// uniqueness assert, the `lookupCatalog` scan, and the `CallConv` column each
// row carried. Six rows keyed on (target NAME, `ObjectFormatKind`) selected the
// calling convention for every C symbol this compiler emits.
//
// ★ IT IS THE SAME DEFECT AS `kCManglingRules`, WHICH A PRIOR CYCLE DELETED
// (D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN) — a table keyed on format identity is
// an `if` on format identity with the branch spelled as data — AND THE SAME
// EVIDENCE WAS SITTING IN PLAIN SIGHT: the strings the table produced,
// `"sysv_amd64"` / `"ms_x64"` / `"apple_arm64"` / `"aapcs64"`, were ALREADY
// written in the shipped descriptors. The fourth column was not deriving
// anything; it was RE-STATING a declaration, in a second language, with nothing
// forcing the two to agree.
//
// The selection now arrives as a DECLARED VERB read out of `.format.json`
// (`cCallingConvention.convention`), and this file no longer speaks
// `ObjectFormatKind` at all. That last part is the load-bearing half, exactly as
// it was in `c_mangle.cpp`: with the identity absent from every signature below,
// an identity branch is not something a reviewer has to look for — it is
// unrepresentable here.
//
// ★★ WHAT ELSE WENT, AND WHY IT COULD: THE `CallConv` COLUMN. Each row also
// pinned a `CallConv` enum value, surfaced as `AbiTuple::callingConvention`, and
// the anchor's own hazard text dramatised it as "arguments in the wrong
// registers". ✔MEASURED: NOTHING IN `src/` EVER READ IT. Every one of the four
// `resolveAbi` call sites (`program.cpp`, `compile_pipeline.cpp`, and both in
// `ingest.cpp`) reads `abi->cc` — the pointer into the target's own
// `callingConventions[]` — and none reads the enum; its only consumer was a test
// asserting the table against itself. The live hazard was the NAME column, and
// an inflated hazard misdirects a fix as surely as an understated one.
//
// ★ WHAT REPLACED THE `consteval` UNIQUENESS ASSERT. Its job was to notice two
// rows claiming the same (target, format) pair, one of which would then be dead
// code. That job did not disappear, it BECAME STRUCTURAL: a format declares
// exactly one `cCallingConvention`, so two claims on one pair are no longer
// representable, and JSON object-key duplication is refused by the parser one
// tier below. A whole class of assert was replaced by a shape in which the
// mistake cannot be written.
//
// ★ WHY THE ARCH IS NOT PART OF THE KEY ANY MORE. `crossValidateTargetFormat`
// refuses any (target, format) pair whose declared machine code disagrees with
// the target's arch, so a format document that reaches here has exactly one arch
// it can ever be paired with. The old first column was carrying no information
// that the format did not already carry.

// Closed-table error-kind → name + F_* code mapping. Mirrors the
// pattern from `kHeaderReadErrorTable` (c_header_parser.cpp).
struct AbiResolveErrorRow {
    AbiResolveErrorKind kind;
    std::string_view    name;
    DiagnosticCode      code;
};

// Closed-table — one row per AbiResolveErrorKind variant. The
// array size is anchored on the enum's `_Count` sentinel — a
// future variant appended (or mid-enum-inserted) bumps `Count_`
// and forces a corresponding row addition, OR the build breaks.
// (silent-failure H3 post-fold #3 fix.)
constexpr std::array<AbiResolveErrorRow,
                     static_cast<std::size_t>(AbiResolveErrorKind::Count_)>
    kAbiResolveErrorTable{{
    { AbiResolveErrorKind::UnknownTuple,            "UnknownTuple",            DiagnosticCode::F_AbiUnknownTuple            },
    { AbiResolveErrorKind::NoMatchingCcInTarget,    "NoMatchingCcInTarget",    DiagnosticCode::F_AbiNoMatchingCcInTarget    },
    { AbiResolveErrorKind::FormatAbiModelMismatch,  "FormatAbiModelMismatch",  DiagnosticCode::F_AbiFormatAbiModelMismatch  },
    { AbiResolveErrorKind::CcRegistersInconsistent, "CcRegistersInconsistent", DiagnosticCode::F_AbiCcRegistersInconsistent },
}};

consteval bool kAbiResolveErrorTableRowsAligned() {
    for (std::size_t i = 0; i < kAbiResolveErrorTable.size(); ++i) {
        if (static_cast<std::size_t>(kAbiResolveErrorTable[i].kind) != i) return false;
    }
    return true;
}
static_assert(kAbiResolveErrorTableRowsAligned(),
              "kAbiResolveErrorTable row order must match underlying enum.");

[[nodiscard]] AbiResolveError
emitAndReturn(AbiResolveErrorKind kind, std::string detail,
              DiagnosticReporter& reporter) {
    auto const idx = static_cast<std::size_t>(kind);
    ParseDiagnostic p;
    p.code     = kAbiResolveErrorTable[idx].code;
    p.severity = DiagnosticSeverity::Error;
    p.actual   = detail;
    reporter.report(std::move(p));
    return AbiResolveError{kind, std::move(detail)};
}

} // namespace

std::string_view
abiResolveErrorKindName(AbiResolveErrorKind k) noexcept {
    auto const idx = static_cast<std::size_t>(k);
    if (idx >= kAbiResolveErrorTable.size()) return "Unknown";
    return kAbiResolveErrorTable[idx].name;
}

std::expected<AbiTuple, AbiResolveError>
resolveAbi(TargetSchema const&       target,
           ObjectFormatSchema const& format,
           DiagnosticReporter&       reporter) {
    // ── THE FORMAT'S DECLARED SELECTION ─────────────────────────────────────
    //
    // One read. Not a lookup, not a scan, not a dispatch: the descriptor was
    // asked which convention it uses, and it answered.
    std::string_view const declared = format.cCallingConvention().convention;

    // An EMPTY selection can only reach here from a HAND-BUILT
    // `ObjectFormatData` — `ObjectFormatSchema{ObjectFormatData}` is a public
    // constructor that runs no validation, so test fixtures, backend-synthesized
    // descriptors and future binary-cache reloads all arrive without passing the
    // JSON tier. The loader REQUIRES the key and `validate()` refuses the empty
    // sentinel, so this arm defends only that one path — but it must defend it,
    // because the alternative is treating "never declared" as "declares none"
    // and silently returning a null cc to a register-machine pipeline.
    if (!format.cCallingConvention().declared()) {
        return std::unexpected(emitAndReturn(
            AbiResolveErrorKind::UnknownTuple,
            std::format("object format '{}' declares no `cCallingConvention` — "
                        "the field is REQUIRED at config load, so this schema "
                        "was constructed in memory without passing the loader. "
                        "Set `ObjectFormatData::cCallingConvention` (a "
                        "callingConventions[] row name from the paired target, "
                        "or \"{}\") before handing the schema to FF3.",
                        format.name(), kCCallingConventionNone),
            reporter));
    }

    // ── THE `none` ARM: A FORMAT WITH NO REGISTER-LEVEL C ABI ───────────────
    //
    // WASM's operand stack and SPIR-V's result ids are not conventions this
    // engine has yet to learn — they are the absence of a register-passing
    // convention, and the format says so. A null `cc` is the signal the driver
    // (`D_TargetAbiModelUnsupportedByDriver`) and FFI ingest
    // (`F_FfiIngestAbiModelUnsupported`) already refuse on.
    //
    // ⚠ THE COHERENCE CHECK BELOW READS `abiModel`, WHICH IS A DECLARED TARGET
    // PROPERTY, NOT AN IDENTITY. That distinction is the whole point: the old
    // code compared `format.kind()` against `Wasm`/`Spirv` literals — two more
    // identity branches — to reach the same conclusion. Two DECLARATIONS
    // disagreeing is a config error and is judged as one; neither party is named
    // by the engine.
    bool const targetIsRegisterMachine =
        target.abiModel() == TargetAbiModel::RegisterMachine;

    if (format.cCallingConvention().declaresNoConvention()) {
        if (targetIsRegisterMachine) {
            return std::unexpected(emitAndReturn(
                AbiResolveErrorKind::UnknownTuple,
                std::format("target '{}' declares abiModel '{}' — it passes "
                            "arguments in REGISTERS — but object format '{}' "
                            "declares `cCallingConvention` \"{}\", i.e. no "
                            "register-level C calling convention at all. The "
                            "pair has no declared ABI: either the format must "
                            "name one of the target's callingConventions[] "
                            "rows, or this target must not be paired with it.",
                            target.name(),
                            targetAbiModelName(target.abiModel()),
                            format.name(), kCCallingConventionNone),
                reporter));
        }
        return AbiTuple{nullptr};
    }

    // The inverse disagreement: a format that names a convention paired with a
    // target that does not pass arguments in registers. `crossValidateTargetFormat`
    // catches the pairing upstream; FF3 is defensive because its schema arguments
    // can arrive without having passed it.
    if (!targetIsRegisterMachine) {
        return std::unexpected(emitAndReturn(
            AbiResolveErrorKind::FormatAbiModelMismatch,
            std::format("object format '{}' declares `cCallingConvention` '{}' "
                        "— a register-passing convention — but target '{}' "
                        "declares abiModel '{}', which has no register "
                        "convention to resolve it against. Expected \"{}\" for "
                        "this pairing. crossValidateTargetFormat should have "
                        "caught this upstream — file a bug.",
                        format.name(), declared, target.name(),
                        targetAbiModelName(target.abiModel()),
                        kCCallingConventionNone),
            reporter));
    }

    // ── RESOLVE THE DECLARED NAME AGAINST THE TARGET'S OWN VOCABULARY ───────
    //
    // `callingConventionByName` is the schema's existing O(1) index, not a scan.
    // A miss is the format naming a convention this processor does not declare —
    // loud, and the message says which name and which document to fix.
    TargetCallingConvention const* cc =
        target.callingConventionByName(declared);
    if (cc == nullptr) {
        return std::unexpected(emitAndReturn(
            AbiResolveErrorKind::NoMatchingCcInTarget,
            std::format("object format '{}' declares `cCallingConvention` '{}', "
                        "but target '{}' ships no callingConventions[] row with "
                        "that name. Either extend the target's "
                        "callingConventions array, or correct the format's "
                        "`cCallingConvention.convention`.",
                        format.name(), declared, target.name()),
            reporter));
    }

    // D-FF3-Coherence (un-retired post-fold #4): defense-in-depth
    // structural validation of the resolved cc. Catches the
    // paste-error class (e.g. `ms_arm64` cc declared with
    // `rcx,rdx,r8,r9` from `ms_x64`) when the schema reaches FF3
    // through a path that bypasses `TargetSchemaData::validate()`
    // — `TargetSchema`'s ctor is public + skips validate, so test
    // fixtures, future `.dsslir` round-trip loaders, fuzz
    // harnesses, etc. can construct a schema directly. Without
    // this pass FF3 would return a `cc *` into structurally-wrong
    // data and ML7 regalloc would emit wrong-arch register names.
    auto firstUnresolved = [&target](
            std::span<std::string const> names) -> std::string_view {
        for (auto const& n : names) {
            if (!target.registerByName(n).has_value()) return n;
        }
        return {};
    };
    struct CcRoleSpan {
        std::string_view              roleName;
        std::span<std::string const>  names;
    };
    std::array<CcRoleSpan, 6> const roles{{
        { "argGprs",     cc->argGprs     },
        { "argFprs",     cc->argFprs     },
        { "returnGprs",  cc->returnGprs  },
        { "returnFprs",  cc->returnFprs  },
        { "callerSaved", cc->callerSaved },
        { "calleeSaved", cc->calleeSaved },
    }};
    for (auto const& role : roles) {
        std::string_view const bad = firstUnresolved(role.names);
        if (!bad.empty()) {
            return std::unexpected(emitAndReturn(
                AbiResolveErrorKind::CcRegistersInconsistent,
                std::format("target '{}' callingConvention '{}': "
                            "{}[] contains register name '{}' that "
                            "is not declared in target.registers[]. "
                            "Most likely a paste-error from an "
                            "unrelated arch — every cc register name "
                            "must resolve in the target's own "
                            "register file.",
                            target.name(), cc->name,
                            role.roleName, bad),
                reporter));
        }
    }

    return AbiTuple{cc};
}

} // namespace dss::ffi
