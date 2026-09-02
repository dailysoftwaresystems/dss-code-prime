#include "program/target_spec.hpp"

namespace dss {

// ── D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES ────────────────────────
//
// A `switch (fmt.kind())` STOOD HERE — seven `ObjectFormatKind` mentions across
// five arms, each with a NESTED per-kind sub-schema switch inside it
// (`ElfObjectType`, `PeObjectType`, `MachOObjectType`), plus a leading
// `fmt.kind() == ObjectFormatKind::Pe ? ".lib" : ".a"` for the archive arm. It
// is GONE, and so are the three inner switches with it.
//
// ★ IT WAS THE CLEAREST CASE IN THE ANCHOR, AND THE ANCHOR SAID SO: an output
// file's EXTENSION is a NAMING fact, not an engine behaviour, so its natural
// home is the format's own declaration rather than a method on a backend. Each
// `.format.json` now declares `outputExtension` directly.
//
// ★★ AND THE NEST COLLAPSED TO A READ RATHER THAN MOVING, WHICH IS THE
// MEASUREMENT THAT JUSTIFIES THE SHAPE. The old code needed the inner switches
// because ONE `ObjectFormatKind` spans several artifact flavors — an `elf` may
// be a `.o`, a `.so`, or an extensionless executable. But a `.format.json`
// DOCUMENT is exactly one flavor: `elf64-x86_64-linux-dyn` is the `.so`,
// `-pie` is the PIE executable, `-staticlib` is the `.a`. So the whole
// derivation — including the subtlest arm, ET_DYN discriminated into
// PIE-vs-shared-object by whether the schema declares `processExit` — is
// answered by one string in the one document that knows it.
//
// ⓘ The empty answer is DECLARED, never inferred: four shipped formats really
// do produce artifacts with no extension, and `""` is what they say. The
// distinction between "declares nothing" and "declares that there is nothing"
// is carried by `std::optional` in `ObjectFormatData` and enforced at load.
std::string_view outputExtensionFor(ObjectFormatSchema const& fmt) noexcept {
    return fmt.outputExtension();
}

} // namespace dss
