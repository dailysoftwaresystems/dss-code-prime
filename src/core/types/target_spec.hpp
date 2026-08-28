#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// The `"<targetName>:<formatName>"` COMPILE-TARGET SPEC, and nothing else.
//
// ★★ WHY THIS TYPE LIVES IN `core` AND ITS EXTENSION RULE DOES NOT
// (D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS, the residual half of
// D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS).
//
// `TargetSpec` used to live entirely in `src/program/`, the DRIVER tier, and
// carried one extra member: `outputExtension(ObjectFormatSchema const&)`. That
// member is what pinned it there — it takes a `link/` type, so the whole header
// dragged `link/` behind it, and `core -> link` is the layering inversion the
// parent row exists to prevent. The LSP needs only the SPLITTER, so it was
// reaching UP a tier to get it, which is precisely the shape the parent row
// closed for the project manifest.
//
// ★ THE SPLIT IS CLEAN BECAUSE THE TWO HALVES NEVER TOUCHED EACH OTHER.
// ✔MEASURED by reading the body: `outputExtension` referenced NEITHER
// `targetName` NOR `formatName` — it was a `const` member that used nothing of
// `*this`. It was never a fact about a target spec at all; it is a fact about a
// loaded OBJECT FORMAT, and it now says so as a free function in the driver
// tier (`program/target_spec.hpp`, `outputExtensionFor`). One type answering two
// tiers' questions was the whole defect.
//
// Encoding rationale, unchanged and still load-bearing: the public API surface
// (`dss_compile_directory`, `compileFiles`, `compileDirectory`) keeps a flat
// `std::vector<std::string>` of specs, so the C ABI is untouched. The colon is
// EXPLICIT — the driver never infers a default for either half, because a
// "default format for target X" lookup would silently route a typo to an
// unintended output. `targetName` is a `TargetSchema::loadShipped` key (e.g.
// `"x86_64"`, `"arm64"`); `formatName` is an `ObjectFormatSchema::loadShipped`
// key (e.g. `"elf64-x86_64-linux-exec"`). Anchored D-LK10-3.

namespace dss {

// Parse-failure modes for `TargetSpec::parse`. The driver branches
// on this kind so each failure surfaces with a remediation-distinct
// `D_InvalidTargetSpec` message rather than the same generic
// diagnostic for four root causes. (silent-failure-hunter F7 fold,
// LK10 cycle 2 post-audit review.)
enum class TargetSpecError : std::uint8_t {
    MissingColon       = 1,
    MultipleColons     = 2,
    EmptyTargetName    = 3,
    EmptyFormatName    = 4,
    // Either half contains whitespace. Whitespace in a logical
    // schema name is almost always a CLI / config typo; rejecting
    // loudly here beats silently failing `loadShipped` downstream
    // with a confusing `D_SchemaLoadFailed` that names the wrong
    // root cause. (pr-test-analyzer FOLD-NOW: whitespace handling.)
    WhitespaceInName   = 5,
};

[[nodiscard]] DSS_EXPORT std::string_view
    targetSpecErrorName(TargetSpecError e) noexcept;

struct DSS_EXPORT TargetSpec {
    std::string targetName;   // e.g. "x86_64"
    std::string formatName;   // e.g. "elf64-x86_64-linux-exec"

    // Parse the `"<targetName>:<formatName>"` shape. On failure
    // returns the specific reason so the caller can dispatch a
    // targeted diagnostic.
    //
    // Failure modes:
    //   * `MissingColon`     — no ':' in `spec`.
    //   * `MultipleColons`   — more than one ':' in `spec` (the
    //                          grammar is unambiguous; reject so
    //                          a future third axis doesn't silently
    //                          claim an existing colon).
    //   * `EmptyTargetName`  — `:formatName`.
    //   * `EmptyFormatName`  — `targetName:`.
    //   * `WhitespaceInName` — leading/trailing/embedded whitespace
    //                          in either half.
    [[nodiscard]] static std::expected<TargetSpec, TargetSpecError>
        parse(std::string_view spec) noexcept;
};

} // namespace dss
