#include "ffi/mangling/c_mangle.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <array>
#include <string>
#include <utility>

namespace dss::ffi {

namespace {

// Closed-table: per-ObjectFormatKind decoration rule for C names.
// Each row maps a format kind to whether it prefixes C identifiers
// with a leading underscore. The shape is one row per
// `ObjectFormatKind` variant so the static_assert below catches a
// new variant addition that forgot to declare its rule.
struct CManglingRule {
    ObjectFormatKind format;
    bool             addLeadingUnderscore;
};

constexpr std::array<CManglingRule, kObjectFormatKindTable.rows.size()> kCManglingRules{{
    { ObjectFormatKind::Unknown, false },  // defensive default
    { ObjectFormatKind::Elf,     false },  // System V / Linux convention
    { ObjectFormatKind::Pe,      false },  // PE64 cdecl (32-bit cdecl `_func` lands at D-FF4-1)
    { ObjectFormatKind::MachO,   true  },  // Apple convention: leading `_` on every C symbol
    { ObjectFormatKind::Wasm,    false },  // uses import-namespace, no name mangling
    { ObjectFormatKind::Spirv,   false },  // no C ABI surface
}};

// One row per format-kind variant: a future format added to
// `ObjectFormatKind` without a matching rule here would silently
// inherit the linear-scan default (false / no decoration), masking
// the design decision. The static_assert + the
// `kCManglingRulesAlignedWithEnum` consteval pin force a fold-now.
// Anchored against `kObjectFormatKindTable` (the canonical
// variant-count source in `link/object_format_schema.hpp`) rather
// than `Spirv+1u` — the latter would silently accept any future
// variant appended after Spirv as long as the row count happened
// to match. (silent-failure H1 + code-reviewer post-fold #2.)
static_assert(kCManglingRules.size() == kObjectFormatKindTable.rows.size(),
              "kCManglingRules row count must equal the ObjectFormatKind "
              "variant count (anchored against kObjectFormatKindTable). "
              "Adding a kind requires adding a rule.");

consteval bool kCManglingRulesAlignedWithEnum() {
    for (std::size_t i = 0; i < kCManglingRules.size(); ++i) {
        if (static_cast<std::size_t>(kCManglingRules[i].format) != i) return false;
    }
    return true;
}
static_assert(kCManglingRulesAlignedWithEnum(),
              "kCManglingRules row order must match the ObjectFormatKind "
              "underlying values — a paste-error row in the wrong slot "
              "would silently apply the wrong rule to the wrong format.");

[[nodiscard]] constexpr bool
addsLeadingUnderscoreFor(ObjectFormatKind format) noexcept {
    auto const idx = static_cast<std::size_t>(format);
    if (idx >= kCManglingRules.size()) return false;
    return kCManglingRules[idx].addLeadingUnderscore;
}

} // namespace

bool cFormatAddsLeadingUnderscore(ObjectFormatKind format) noexcept {
    return addsLeadingUnderscoreFor(format);
}

std::string
applyCMangling(std::string_view canonicalName, ObjectFormatKind format) {
    if (canonicalName.empty()) return {};
    if (addsLeadingUnderscoreFor(format)) {
        std::string out;
        out.reserve(canonicalName.size() + 1u);
        out.push_back('_');
        out.append(canonicalName);
        return out;
    }
    return std::string{canonicalName};
}

std::string
unapplyCMangling(std::string_view decoratedName, ObjectFormatKind format) {
    if (decoratedName.empty()) return {};
    if (addsLeadingUnderscoreFor(format)
        && !decoratedName.empty()
        && decoratedName.front() == '_') {
        return std::string{decoratedName.substr(1)};
    }
    // Conservative: missing-prefix is not synthesized into an
    // error here — operators sometimes ship libraries with
    // non-standard naming, and the linker resolves by exact
    // symbol equality. FF1 binary readers feed
    // already-as-on-disk strings; FF4's job is to compute the
    // canonical form, not validate the producer.
    return std::string{decoratedName};
}

namespace {

// Closed-table mapping MangleErrorKind → (name, F_* code). Same
// shape as kAbiResolveErrorTable / kHeaderReadErrorTable. Pinned
// against `MangleErrorKind::Count_` (codebase precedent — `HirOpKind::Count_`,
// `AbiResolveErrorKind::Count_`).
struct MangleErrorRow {
    MangleErrorKind  kind;
    std::string_view name;
    DiagnosticCode   code;
};

constexpr std::array<MangleErrorRow,
                     static_cast<std::size_t>(MangleErrorKind::Count_)>
    kMangleErrorTable{{
    { MangleErrorKind::MissingExpectedPrefix, "MissingExpectedPrefix",
      DiagnosticCode::F_MangleMissingExpectedPrefix },
}};

consteval bool kMangleErrorTableRowsAligned() {
    for (std::size_t i = 0; i < kMangleErrorTable.size(); ++i) {
        if (static_cast<std::size_t>(kMangleErrorTable[i].kind) != i) return false;
    }
    return true;
}
static_assert(kMangleErrorTableRowsAligned(),
              "kMangleErrorTable row order must match MangleErrorKind "
              "underlying values.");

} // namespace

std::string_view
mangleErrorKindName(MangleErrorKind k) noexcept {
    auto const idx = static_cast<std::size_t>(k);
    if (idx >= kMangleErrorTable.size()) return "Unknown";
    return kMangleErrorTable[idx].name;
}

std::expected<std::string, MangleError>
unapplyCManglingStrict(std::string_view    decoratedName,
                       ObjectFormatKind    format,
                       DiagnosticReporter& reporter) {
    if (decoratedName.empty()) return std::string{};
    if (cFormatAddsLeadingUnderscore(format)) {
        // Guarded by the empty-input early-return above; decoratedName.front() is safe.
        if (decoratedName.front() != '_') {
            std::string detail = std::string{"format expects leading '_' "
                                             "decoration but input '"}
                                 + std::string{decoratedName}
                                 + "' does not carry it";
            dss::report(reporter,
                        DiagnosticCode::F_MangleMissingExpectedPrefix,
                        DiagnosticSeverity::Error, detail);
            return std::unexpected(MangleError{
                MangleErrorKind::MissingExpectedPrefix, std::move(detail)});
        }
        return std::string{decoratedName.substr(1)};
    }
    // No-decoration formats: strict mode is structurally a no-op.
    return std::string{decoratedName};
}

} // namespace dss::ffi
