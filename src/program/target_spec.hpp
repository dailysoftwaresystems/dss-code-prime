#pragma once

#include "core/export.hpp"
#include "link/object_format_schema.hpp"

#include <expected>
#include <string>
#include <string_view>

// Driver-tier compile target descriptor (plan 14 LK10 cycle 2).
//
// `compileFiles` / `compileDirectory` accept `std::vector<std::string>
// targets` (the historic API surface), where each entry encodes a
// (target-arch, object-format) pair as `"<targetName>:<formatName>"`.
// `targetName` is a `TargetSchema::loadShipped` key (e.g. `"x86_64"`,
// `"arm64"`); `formatName` is an `ObjectFormatSchema::loadShipped`
// key (e.g. `"elf64-x86_64-linux"`, `"pe64-x86_64-windows-exec"`).
//
// Encoding rationale: keeping the public API as a flat vector of
// strings preserves the C-ABI surface (`dss_compile_directory`)
// unchanged. The colon separator is explicit — the driver never
// infers a default for either half, because a "default format for
// target X" lookup would silently route to an unintended output on
// typo. CLI argument routing (LK10 cycle 3) parses the same strings.
//
// `outputExtension(ObjectFormatSchema const&)` derives the on-disk
// file extension from the format's `kind()` + per-format
// `objectType` sub-discriminator. This is a v1 driver-layer
// convention; plan 6 (artifact profiles) will eventually own the
// authoritative extension/output-dir policy. Anchored D-LK10-3.

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

    // Derive on-disk file extension from the loaded format schema.
    // Closed switch over `ObjectFormatKind` + per-format
    // objectType sub-discriminator:
    //   * Elf+Rel        → ".o"
    //   * Elf+Exec       → ""        (Linux exec convention)
    //   * Elf+Dyn        → ".so"     (D-LK1-4 — substrate pending)
    //   * Pe +Obj        → ".obj"
    //   * Pe +Exec       → ".exe"
    //   * Pe +Dll        → ".dll"    (D-LK2-4 — substrate pending)
    //   * MachO+Object   → ".o"
    //   * MachO+Execute  → ""        (macOS exec convention)
    //   * MachO+Dylib    → ".dylib"  (D-LK3-3 — substrate pending)
    //   * Wasm           → ".wasm"
    //   * Spirv          → ".spv"
    //   * Unknown        → ""        (never reached — linker validates
    //                                 schema.kind() != Unknown before
    //                                 this is called; defensive "" is
    //                                 belt-and-suspenders for closed-
    //                                 switch exhaustiveness)
    //
    // The Dyn / Dll / Dylib arms return their canonical extensions
    // even though their walker substrates haven't shipped. Schemas
    // with those `objectType` values are rejected by the
    // `ObjectFormatSchema::loadShipped` validate() step today, so
    // the arms are unreachable until D-LK1-4 / D-LK2-4 / D-LK3-3
    // close — at which point the extensions are already correct.
    [[nodiscard]] std::string_view
        outputExtension(ObjectFormatSchema const& fmt) const noexcept;
};

} // namespace dss
