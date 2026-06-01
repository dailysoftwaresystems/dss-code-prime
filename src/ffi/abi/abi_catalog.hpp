#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"  // CallConv
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

// Plan 11 FF3 — ABI catalog. Resolves a (target × format) pair to
// a calling-convention selection plus a pointer into the target
// schema's `callingConventions` array.
//
// Substrate-only at this cycle: provides the resolver, does NOT
// yet rewire ML7. The silent-failure surface where
// `src/lir/lir_regalloc.cpp::317` hardcodes
// `callingConventionIndex = 0` regardless of format remains open
// until D-FF3-3 (FF5 integration cycle) threads `AbiTuple` into
// `compileOneTarget` → `compileSingleUnit` → `allocateRegisters`.
// Until then a `target=x86_64 format=pe64-x86_64-windows` pair
// still silently dispatches to `sysv_amd64` (index 0) when it
// should be `ms_x64`; FF3 ships the resolver that closes it.
//
// Closed-table dispatch keyed on (target.name, format.kind) →
// (CallConv enum, expected cc name string). FF3 looks up the row,
// then resolves the cc by name against the target's
// `callingConventions` array. Either lookup miss fails loud with
// a distinct F_* code.
//
// Layout-side (pointer size, integer-type sizes, struct padding,
// va_arg handling) is anchored as D-FF3-1 — not in v1 because no
// shipped target.json yet declares layout, and adding the fields
// is a cross-tier extension to TargetSchema beyond FF3's scope.

namespace dss::ffi {

// Resolved ABI for a (target, format) pair.
struct DSS_EXPORT AbiTuple {
    // The CallConv enum value the type lattice's FnSig should
    // carry for symbols compiled into this (target, format) pair.
    CallConv callingConvention = CallConv::CcSysV;

    // Pointer into the resolving target's `callingConventions()`
    // span. NON-null for register-machine targets (where ML7
    // callconv lowering needs the structured register data).
    // NULL for operand-stack (WASM) / result-id (SPIR-V) targets
    // — those abi-models bypass register allocation entirely, so
    // there's no cc table to attach. Lifetime: tied to the
    // TargetSchema passed to `resolveAbi`. The caller MUST keep
    // the target schema alive for the AbiTuple's lifetime.
    TargetCallingConvention const* cc = nullptr;
};

// Closed-set FF3 failure modes. 1:1 with `F_Abi*` codes via the
// `kAbiResolveErrorTable` (abi_catalog.cpp). `Count_` is a sentinel
// pinning the table-size invariant (silent-failure H3 post-fold #3:
// `LastVariant + 1u` would silently accept a new variant appended
// without a row; `Count_` increments alongside any addition. Matches
// `HirOpKind::Count_` codebase precedent).
enum class AbiResolveErrorKind : std::uint8_t {
    UnknownTuple              = 0,  // (target.name, format.kind) not in catalog
    NoMatchingCcInTarget      = 1,  // target.json lacks the cc the catalog says it needs
    FormatAbiModelMismatch    = 2,  // defensive — abiModel/format-kind disagreement
    Count_                          // table-size sentinel — keep LAST (codebase convention)
};

// (D-FF3-Coherence — REDUNDANT WITH SCHEMA LOADER, anchor RETIRED.
// The silent-failure surface flagged by audit "cc with wrong-arch
// register names slips past FF3" does NOT exist in practice:
// `TargetSchemaData::validate()` already rejects any cc whose
// `argGprs`/`argFprs`/`returnGprs`/`returnFprs`/`callerSaved`/
// `calleeSaved` references don't resolve in `registerIndex` —
// see `src/core/types/target_schema.cpp::validate()` lines 901-908,
// and the loader at `target_schema_json.cpp:903-908` forwards
// validate() errors as ConfigDiagnostics that cause loadFromText
// to return `std::unexpected`. A target.json with a paste-error
// cc cannot load at all; FF3 never sees it.
//   The only way to bypass this gate is to construct a
// `TargetSchemaData` programmatically (not via JSON) — but the
// type lattice + cycle-2b discipline forbids that path. FF3 can
// rely on the schema loader as a single source of truth for cc
// structural validity.
// Verdict: D-FF3-Coherence trigger formally EVALUATED 2026-06-01
// against existing schema-loader behavior; surface does NOT
// exist; anchor retired.)

struct DSS_EXPORT AbiResolveError {
    AbiResolveErrorKind kind = AbiResolveErrorKind::UnknownTuple;
    std::string         detail;
};

[[nodiscard]] DSS_EXPORT std::string_view
    abiResolveErrorKindName(AbiResolveErrorKind k) noexcept;

// Resolve the (target × format) ABI tuple. Both arguments must
// outlive the returned `AbiTuple::cc` pointer.
//
// Diagnostics from FF3 pipe through `reporter` (F_Abi* codes).
// Returns `std::unexpected` with the structured `AbiResolveError`
// kind for programmatic dispatch.
[[nodiscard]] DSS_EXPORT
std::expected<AbiTuple, AbiResolveError>
resolveAbi(TargetSchema const&       target,
           ObjectFormatSchema const& format,
           DiagnosticReporter&       reporter);

// Test-exposed: the catalog table. Row 0..N-1 each map
// (target.name, format.kind) → (CallConv, expected cc name).
// Lets unit tests pin coverage + uniqueness without
// re-implementing the lookup.
struct DSS_EXPORT AbiCatalogRow {
    std::string_view targetName;
    ObjectFormatKind formatKind;
    CallConv         callingConvention;
    std::string_view expectedCcName;
};

[[nodiscard]] DSS_EXPORT std::span<AbiCatalogRow const>
    abiCatalogTable() noexcept;

} // namespace dss::ffi
