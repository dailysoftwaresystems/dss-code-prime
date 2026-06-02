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
// D-FF3-3 CLOSED (2026-06-01 post-fold #5): `resolveAbi` is now
// threaded through `compileOneTarget` → `compileSingleUnit` →
// `allocateRegisters(ccIndex)`. A `target=x86_64
// format=pe64-x86_64-windows` pair correctly dispatches to
// `ms_x64` (the cc resolved here), not the pre-fix hardcoded
// `sysv_amd64` (cc[0]). The behavioral pin lives at
// `tests/lir/test_lir_callconv.cpp::CcIndex1DrivesDifferentArgGprThanCc0`.
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
    CcRegistersInconsistent   = 3,  // cc row carries register names absent from target.registers[]
    Count_                          // table-size sentinel — keep LAST (codebase convention)
};

// (D-FF3-Coherence: previously retired 2026-06-01 on the premise
// that `TargetSchemaData::validate()` already closes this surface
// at JSON load. That premise was FALSE: `TargetSchema`'s ctor is
// public (target_schema.hpp:1042) and performs zero validation —
// any caller bypassing the JSON loader (test fixture, .dsslir
// preamble round-trip, fuzz harness, future binary-cache reload)
// can construct a schema carrying a paste-error cc that FF3 must
// defensively reject. The defensive pass below restores the
// closure. UN-RETIRED 2026-06-01 (post-fold #4 silent-failure C1).)

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
