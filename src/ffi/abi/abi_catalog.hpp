#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// Plan 11 FF3 — ABI resolution. Resolves a (target × format) pair to a pointer
// into the target schema's `callingConventions` array.
//
// D-FFI-ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY CLOSED
// (P44): the selection is READ, not derived. The `.format.json` declares
// `cCallingConvention.convention` — the NAME of one of the paired target's
// `callingConventions[]` rows, or the reserved `"none"` — and `resolveAbi`
// resolves that name against the target's own O(1) index. The closed C++ table
// `kAbiCatalog`, keyed on (target name, `ObjectFormatKind`), is DELETED, along
// with `AbiCatalogRow`, `abiCatalogTable()` and the `CallConv` column each row
// carried; see the note at the top of `abi_catalog.cpp` for the full argument
// and for what replaced its `consteval` uniqueness assert.
//
// ★ NEITHER THIS HEADER NOR ITS `.cpp` SPEAKS `ObjectFormatKind` ANY MORE — the
// name survives in both files only inside this note about its removal, and in no
// declaration, signature or expression. That is the same load-bearing property
// `c_mangle.hpp` gained when `kCManglingRules` went: with the identity absent
// from every signature, an identity branch is not something a reviewer must look
// for, it is unrepresentable. `#include "link/object_format_schema.hpp"` stays
// because `resolveAbi` takes an `ObjectFormatSchema const&` — it asks that
// schema what it DECLARES, never what it IS.
//
// D-FF3-3 CLOSED (2026-06-01 post-fold #5): `resolveAbi` is threaded through
// `compileOneTarget` → `compileSingleUnit` → `allocateRegisters(ccIndex)`. A
// `target=x86_64 format=pe64-x86_64-windows` pair correctly dispatches to
// `ms_x64`, not the pre-fix hardcoded `sysv_amd64` (cc[0]). The behavioral pin
// lives at `tests/lir/test_lir_callconv.cpp::CcIndex1DrivesDifferentArgGprThanCc0`.
//
// Layout-side (pointer size, integer-type sizes, struct padding, va_arg
// handling) is anchored as D-FF3-1 — not in v1 because no shipped target.json
// yet declares layout, and adding the fields is a cross-tier extension to
// TargetSchema beyond FF3's scope. When it lands it belongs INSIDE the
// `cCallingConvention` block as a sibling key, which is why that block is an
// object rather than a bare string.

namespace dss::ffi {

// Resolved ABI for a (target, format) pair.
struct DSS_EXPORT AbiTuple {
    // Pointer into the resolving target's `callingConventions()`
    // span. NON-null for register-machine targets (where ML7
    // callconv lowering needs the structured register data).
    // NULL when the format declares `cCallingConvention` `"none"`
    // — an operand-stack (WASM) or result-id (SPIR-V) format has no
    // register-passing convention at all, and a null here is the
    // signal the driver and FFI ingest already refuse on. Lifetime:
    // tied to the TargetSchema passed to `resolveAbi`. The caller
    // MUST keep the target schema alive for the AbiTuple's lifetime.
    //
    // ⚠ THIS STRUCT USED TO CARRY A SECOND FIELD, `CallConv
    // callingConvention`, AND IT IS DELETED RATHER THAN MOVED.
    // ✔MEASURED at closure: no site in `src/` ever read it — all four
    // `resolveAbi` callers read `cc` — and its only consumer was a
    // test asserting the deleted table against itself. The struct is
    // kept (rather than collapsing the return type to a bare pointer)
    // because D-FF3-1's layout facts belong here when they land.
    TargetCallingConvention const* cc = nullptr;
};

// Closed-set FF3 failure modes. 1:1 with `F_Abi*` codes via the
// `kAbiResolveErrorTable` (abi_catalog.cpp). `Count_` is a sentinel
// pinning the table-size invariant (silent-failure H3 post-fold #3:
// `LastVariant + 1u` would silently accept a new variant appended
// without a row; `Count_` increments alongside any addition. Matches
// `HirOpKind::Count_` codebase precedent).
enum class AbiResolveErrorKind : std::uint8_t {
    // The (target, format) pair declares NO ABI. Two ways to reach it, and
    // both are the same statement: the format declares `cCallingConvention`
    // `"none"` while the target passes arguments in registers, or the format
    // declares nothing at all (reachable only from a hand-built
    // `ObjectFormatData`, since the loader requires the key).
    // ⚠ THE NAME PREDATES THE FIX AND ITS OLD MEANING WAS NARROWER — it named
    // a (target.name, format.kind) tuple absent from the deleted `kAbiCatalog`.
    // It is kept because the CONDITION it reports is the same one under the new
    // shape ("this pairing has no declared ABI") and because renaming it would
    // churn a shared `DiagnosticCode` enum for no gain in truth.
    UnknownTuple              = 0,
    // The format NAMES a convention the target ships no row for.
    NoMatchingCcInTarget      = 1,
    // Defensive — the format names a register convention while the target
    // declares a non-register abi-model (or the inverse is caught above).
    FormatAbiModelMismatch    = 2,
    // The resolved cc row carries register names absent from target.registers[].
    CcRegistersInconsistent   = 3,
    Count_                          // table-size sentinel — keep LAST (codebase convention)
};

// (the FF3 Coherence item: previously retired 2026-06-01 on the premise
// that `TargetSchemaData::validate()` already closes this surface
// at JSON load. That premise was FALSE: `TargetSchema`'s ctor is
// public (`TargetSchema`'s converting ctor in target_schema.hpp) and
// performs zero validation —
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

} // namespace dss::ffi
