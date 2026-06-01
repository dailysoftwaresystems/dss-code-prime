#include "ffi/abi/abi_catalog.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <array>
#include <format>
#include <string>
#include <utility>

namespace dss::ffi {

namespace {

// Closed-table: each row pins ONE (target.name, format.kind) →
// (CallConv enum, expected calling-convention name in the
// target.json). Adding a new (target, format) combination = add
// a row here AND ship the cc with the same name in the target's
// JSON. The static_asserts below catch duplicate rows.
//
// Source/target/linker agnostic invariant: the catalog encodes
// the platform ABI **convention** (which cc applies on which
// OS+ISA combination), not the register allocation itself —
// that comes from the target schema's per-cc struct that this
// row points at.
//
// Currently SHIPPED combinations (both target.json + format.json
// present + cc row exists):
//   x86_64 + Elf   → sysv_amd64
//   x86_64 + Pe    → ms_x64
//   x86_64 + MachO → sysv_amd64  (Apple's x86_64 ABI is SysV-with-quirks)
//   arm64  + Elf   → aapcs64
//
// Anchored but NOT yet shipped (FF3 will fail loud
// `NoMatchingCcInTarget` if used; remediation = ship the matching
// format.json + add the cc row in target.json + ALSO add a
// matching CallConv enum variant when the convention is
// arch-distinct):
//   arm64 + Pe    → ms_arm64    — Microsoft ARM64 ABI. PLACEHOLDER
//                                  CallConv: re-uses `CcMS64`
//                                  pending a proper `CcMSARM64`
//                                  variant; the two share `MS_*`
//                                  history but differ in arg
//                                  registers (x0..x7 vs rcx/rdx/r8/r9)
//                                  + return-reg + shadow space.
//                                  D-FF3-4 anchor: split CallConv.
//   arm64 + MachO → apple_arm64 — Apple ARM64 ABI. Divergent from
//                                  AAPCS64 (developer.apple.com:
//                                  /documentation/xcode/writing-arm64-code-for-apple-platforms)
//                                  on varargs (always-stack) and
//                                  indirect-result. CcApple
//                                  variant exists.
constexpr std::array<AbiCatalogRow, 6> kAbiCatalog{{
    { "x86_64", ObjectFormatKind::Elf,   CallConv::CcSysV,    "sysv_amd64"  },
    { "x86_64", ObjectFormatKind::Pe,    CallConv::CcMS64,    "ms_x64"      },
    { "x86_64", ObjectFormatKind::MachO, CallConv::CcSysV,    "sysv_amd64"  },
    { "arm64",  ObjectFormatKind::Elf,   CallConv::CcAAPCS64, "aapcs64"     },
    { "arm64",  ObjectFormatKind::Pe,    CallConv::CcMS64,    "ms_arm64"    },  // placeholder CallConv — see D-FF3-4
    { "arm64",  ObjectFormatKind::MachO, CallConv::CcApple,   "apple_arm64" },
}};

// Compile-time uniqueness: a duplicate (targetName, formatKind)
// row would silently be dead code (the linear scan returns the
// FIRST match) while a maintainer thinks both rows are honored.
// Mirrors `kTargetArchMachineCodes`'s consteval discipline from
// cross_validate_target_format.cpp.
consteval bool kAbiCatalogTuplesUnique() {
    for (std::size_t i = 0; i < kAbiCatalog.size(); ++i) {
        for (std::size_t j = i + 1; j < kAbiCatalog.size(); ++j) {
            if (kAbiCatalog[i].targetName == kAbiCatalog[j].targetName
                && kAbiCatalog[i].formatKind == kAbiCatalog[j].formatKind) {
                return false;
            }
        }
    }
    return true;
}
static_assert(kAbiCatalogTuplesUnique(),
              "kAbiCatalog rows must be unique on (targetName, formatKind) "
              "— a duplicate is dead code that masks a paste-error.");

[[nodiscard]] AbiCatalogRow const*
lookupCatalog(std::string_view targetName, ObjectFormatKind kind) noexcept {
    for (auto const& row : kAbiCatalog) {
        if (row.targetName == targetName && row.formatKind == kind) {
            return &row;
        }
    }
    return nullptr;
}

// (`findCcByName` helper retired post-FF3-#2 — replaced by
// `TargetSchema::callingConventionByName` which is the existing
// O(1) hashmap lookup, not a linear scan. code-reviewer HIGH
// fold; matches the codebase rule "use existing index when one
// exists".)

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
    { AbiResolveErrorKind::UnknownTuple,           "UnknownTuple",           DiagnosticCode::F_AbiUnknownTuple           },
    { AbiResolveErrorKind::NoMatchingCcInTarget,   "NoMatchingCcInTarget",   DiagnosticCode::F_AbiNoMatchingCcInTarget   },
    { AbiResolveErrorKind::FormatAbiModelMismatch, "FormatAbiModelMismatch", DiagnosticCode::F_AbiFormatAbiModelMismatch },
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

std::span<AbiCatalogRow const>
abiCatalogTable() noexcept {
    return {kAbiCatalog.data(), kAbiCatalog.size()};
}

std::expected<AbiTuple, AbiResolveError>
resolveAbi(TargetSchema const&       target,
           ObjectFormatSchema const& format,
           DiagnosticReporter&       reporter) {
    // Operand-stack + result-id targets: no cc table to bind.
    // crossValidateTargetFormat enforces abiModel↔format-kind
    // compatibility upstream; FF3 trusts that gate but treats a
    // mismatch defensively rather than dispatching to a stale
    // catalog row.
    if (target.abiModel() == TargetAbiModel::OperandStack) {
        if (format.kind() != ObjectFormatKind::Wasm) {
            return std::unexpected(emitAndReturn(
                AbiResolveErrorKind::FormatAbiModelMismatch,
                std::format("target '{}' is operand-stack abi-model but "
                            "format kind is '{}' (expected wasm). "
                            "crossValidateTargetFormat should have caught "
                            "this upstream — file a bug.",
                            target.name(),
                            objectFormatKindName(format.kind())),
                reporter));
        }
        return AbiTuple{CallConv::CcWasm, nullptr};
    }
    if (target.abiModel() == TargetAbiModel::ResultId) {
        if (format.kind() != ObjectFormatKind::Spirv) {
            return std::unexpected(emitAndReturn(
                AbiResolveErrorKind::FormatAbiModelMismatch,
                std::format("target '{}' is result-id abi-model but format "
                            "kind is '{}' (expected spirv). "
                            "crossValidateTargetFormat should have caught "
                            "this upstream — file a bug.",
                            target.name(),
                            objectFormatKindName(format.kind())),
                reporter));
        }
        return AbiTuple{CallConv::CcSpirv, nullptr};
    }

    // Register-machine path: look up the catalog row keyed on
    // (target.name, format.kind), then resolve the cc by name
    // in the target's `callingConventions` array.
    AbiCatalogRow const* row = lookupCatalog(target.name(), format.kind());
    if (row == nullptr) {
        return std::unexpected(emitAndReturn(
            AbiResolveErrorKind::UnknownTuple,
            std::format("FF3 ABI catalog has no row for "
                        "(target='{}', format-kind='{}'). "
                        "Either the (target, format) pair is unsupported, "
                        "or a new row must be added to kAbiCatalog "
                        "(src/ffi/abi/abi_catalog.cpp).",
                        target.name(), objectFormatKindName(format.kind())),
            reporter));
    }

    TargetCallingConvention const* cc =
        target.callingConventionByName(row->expectedCcName);
    if (cc == nullptr) {
        return std::unexpected(emitAndReturn(
            AbiResolveErrorKind::NoMatchingCcInTarget,
            std::format("target '{}' paired with format-kind '{}' requires "
                        "callingConventions row named '{}', but target.json "
                        "ships no such row. Extend the target.json's "
                        "callingConventions array.",
                        target.name(),
                        objectFormatKindName(format.kind()),
                        row->expectedCcName),
            reporter));
    }

    // (D-FF3-Coherence redundant — schema loader's `validate()`
    // already rejects cc rows with unresolvable register names
    // at JSON-load time. See abi_catalog.hpp comment block.)
    return AbiTuple{row->callingConvention, cc};
}

} // namespace dss::ffi
