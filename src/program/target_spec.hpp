#pragma once

// ★★ THE SPEC TYPE ITSELF NOW LIVES IN `core`
// (D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS). This header is the
// DRIVER-TIER half of the same story, and it is included from here so every
// existing `#include "program/target_spec.hpp"` keeps compiling unchanged —
// a driver TU that wants both halves asks for the driver header and gets them.
//
// Read `core/types/target_spec.hpp` for WHY the split exists. The one-line
// version: the extension rule below takes an `ObjectFormatSchema`, so keeping it
// on the type dragged `link/` into every consumer of the splitter, and `core`
// must not depend on `link`. The LSP wanted only the splitter and was reaching
// UP a tier for it.
#include "core/types/target_spec.hpp"
#include "core/export.hpp"
#include "link/object_format_schema.hpp"

#include <string_view>

namespace dss {

// Derive the on-disk artifact extension from a LOADED OBJECT FORMAT.
//
// ★★ A FREE FUNCTION TAKING ONLY THE FORMAT, BECAUSE THAT IS ALL IT EVER READ.
// This was `TargetSpec::outputExtension(ObjectFormatSchema const&) const`, and
// ✔MEASURED against its own body: it referenced neither `targetName` nor
// `formatName`. A `const` member that uses nothing of `*this` is a free function
// wearing a member's clothes, and here the disguise had a cost — it made the
// extension look like a fact about a target SPEC, which pinned the spec type to
// the `link` tier and kept the LSP reaching across a layer for the splitter.
// The extension is a fact about the FORMAT, and now only the format is asked.
//
// ⚠ IT STAYS IN THE DRIVER TIER, DELIBERATELY, rather than moving onto
// `ObjectFormatSchema` where the data lives. This is the v1 DRIVER output-naming
// convention — plan 6 (artifact profiles) owns the authoritative
// extension/output-dir policy and will replace it. Putting a driver convention
// on the schema would give `link` an opinion about how a DRIVER names files,
// which is the same one-fact-two-owners shape the split above just undid.
//
// Closed switch over `ObjectFormatKind` + the per-format objectType
// sub-discriminator:
//   * archive        → ".a"      (ELF / Mach-O ecosystems)
//                      ".lib"    (COFF / PE ecosystem)
//   * Elf+Rel        → ".o"
//   * Elf+Exec       → ""        (Linux exec convention)
//   * Elf+Dyn        → ".so", or "" for the PIE sub-shape — the entry
//                      cluster discriminates (D-LK1-4 / c151)
//   * Pe +Obj        → ".obj"
//   * Pe +Exec       → ".exe"
//   * Pe +Dll        → ".dll"
//   * MachO+Object   → ".o"
//   * MachO+Execute  → ""        (macOS exec convention)
//   * MachO+Dylib    → ".dylib"
//   * Wasm           → ".wasm"
//   * Spirv          → ".spv"
//   * Unknown        → ""        (never reached — the linker validates
//                                 schema.kind() != Unknown before this is
//                                 called; the defensive "" is closed-switch
//                                 exhaustiveness, not a real arm)
[[nodiscard]] DSS_EXPORT std::string_view
    outputExtensionFor(ObjectFormatSchema const& fmt) noexcept;

} // namespace dss
