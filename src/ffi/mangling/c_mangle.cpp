#include "ffi/mangling/c_mangle.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <array>
#include <string>
#include <utility>

namespace dss::ffi {

namespace {

// D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN (step C4): the closed C++ table
// `kCManglingRules` STOOD HERE and is GONE. It mapped each `ObjectFormatKind`
// to a bool, keyed on the format IDENTITY, and it was one of TWO owners of a
// single per-format fact — the other being a literal encoding in the format
// descriptors themselves (`macho64-*-exec` spelling `processExit.
// importMangledName` as `_exit` while elf/pe spell it `exit`). Two owners, two
// languages, nothing forcing them to agree.
//
// The rule now arrives as a DECLARED VERB read out of `.format.json`
// (`cSymbolDecoration.scheme`), and this file no longer speaks
// `ObjectFormatKind` at all. That last part is the load-bearing half: with the
// identity absent from every signature below, an identity branch is not
// something a reviewer has to look for — it is unrepresentable here.
//
// WHAT REPLACED THE static_asserts. The old table carried two of them, whose
// job was to notice a NEW `ObjectFormatKind` variant that forgot to declare its
// rule (it would otherwise inherit the linear-scan `false`). That job did not
// disappear, it MOVED and got stronger: a new format now cannot be added
// without a `.format.json`, and `ObjectFormatData::validate()` REQUIRES
// `cSymbolDecoration` on every format unconditionally — so the omission is
// caught at config LOAD for every format that will ever exist, rather than by a
// compile-time assert over the enum's current membership.
[[nodiscard]] constexpr bool
addsLeadingUnderscoreFor(CSymbolDecorationScheme scheme) noexcept {
    return scheme == CSymbolDecorationScheme::LeadingUnderscore;
}

} // namespace

bool cFormatAddsLeadingUnderscore(CSymbolDecorationScheme scheme) noexcept {
    return addsLeadingUnderscoreFor(scheme);
}

std::string
applyCMangling(std::string_view       canonicalName,
               CSymbolDecorationScheme scheme) {
    if (canonicalName.empty()) return {};
    if (addsLeadingUnderscoreFor(scheme)) {
        std::string out;
        out.reserve(canonicalName.size() + 1u);
        out.push_back('_');
        out.append(canonicalName);
        return out;
    }
    return std::string{canonicalName};
}

std::string
linkNameFor(std::string_view canonicalName, std::string_view asmLabel,
            CSymbolDecorationScheme scheme, std::string_view linkBaseName) {
    // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): an explicit assembler name
    // REPLACES the format's C mangling. It is
    // returned byte-for-byte — no prefix added, none stripped, no validation of
    // its shape (an assembler name is whatever the target assembler accepts, and
    // `$`/`.`/`@` all appear in real Darwin and ELF-versioned symbols). The label
    // is already gated non-empty at its source (S_AsmLabelInvalid), so an empty
    // one here can only mean "no label".
    if (!asmLabel.empty()) return std::string{asmLabel};
    // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): a descriptor-declared
    // per-target link BASE name swaps WHAT gets decorated and nothing else — so
    // the override and the default fall into the SAME `applyCMangling` call one
    // line below. That single call is the property the tests pin: with
    // `linkName:"fstat$INODE64"` a macho build emits `_fstat$INODE64` and an elf
    // build emits the bare `fstat$INODE64`, because the `_` is the FORMAT's fact
    // (the declared `cSymbolDecoration.scheme`), never the symbol's. Empty ⇒ the canonical identifier,
    // byte-identical to every pre-TF-C121 image.
    return applyCMangling(linkBaseName.empty() ? canonicalName : linkBaseName,
                          scheme);
}

std::string
unapplyCMangling(std::string_view       decoratedName,
                 CSymbolDecorationScheme scheme) {
    if (decoratedName.empty()) return {};
    if (addsLeadingUnderscoreFor(scheme)
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
unapplyCManglingStrict(std::string_view        decoratedName,
                       CSymbolDecorationScheme scheme,
                       DiagnosticReporter&     reporter) {
    if (decoratedName.empty()) return std::string{};
    if (cFormatAddsLeadingUnderscore(scheme)) {
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
