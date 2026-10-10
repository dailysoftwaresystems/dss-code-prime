#pragma once

#include "asm/asm.hpp"                         // AssembledModule
#include "core/export.hpp"                     // DSS_EXPORT
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"       // pcRelativeImportAddress

#include <span>
#include <vector>

// ── A UNIT THAT TAKES AN IMPORT'S ADDRESS BY A DISPLACEMENT ───────────────
// P69 review M1 case (c) and MINOR 8 (D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE).
//
// A displacement reaches nothing outside the image, so a unit's
// `leaq puts(%rip)` — or arm64 `adrp`/`add` — can only compute the import's
// CALL ENTRY in this image: the PE import thunk, the ELF PLT stub, the Mach-O
// stub. On an image whose call entry is not the import's address (the images
// that declare `externAddrBinding`), the format says what that reference means
// (`pcRelativeImportAddress`):
//   * `refused` (ELF shared objects, Mach-O images) — refused by name, unit by
//     unit and import by import, as the format's own linkers refuse it;
//   * `callEntry` (PE images) — the unit's address of that FUNCTION import IS
//     its call entry, and so is every other address reference the unit makes
//     to it: its data relocations (`.quad puts`, `&tbl[k]`), its GOT loads, and
//     the slots it carries for them. They are retargeted to the import's
//     `callEntrySymbol`, which the image walker binds to the call entry, so the
//     unit agrees with itself — as link.exe, lld-link and GNU ld make such a
//     MinGW object agree with itself — while every other unit keeps the
//     loader-bound address. A coalescable (WEAK) item of the unit, like a
//     `.refptr.puts` COMDAT, is not changed: other units may be bound to the
//     copy that wins, so the unit gets a private clone of it instead.
// A reference is a branch by its format row (`isCall`) or by the target's own
// declared branch encodings (`link/branch_sites.hpp`); a GOT load and a
// thread-local reference are never this case. A name some linked unit DEFINES
// is not an import here. Runs on the link's INPUT units, before the merge,
// because a unit is the scope of the rule.
//
// Returns TRUE when nothing needs a change (`out` untouched); FALSE when `out`
// holds the adjusted units OR an error was reported — the caller tells the two
// apart by the reporter's error count (the `lowerGotSlotReferences`
// convention).
namespace dss::linker {

[[nodiscard]] DSS_EXPORT bool
bindPcRelativeImportAddressUnits(std::span<AssembledModule const> units,
                                 std::vector<AssembledModule>&    out,
                                 TargetSchema const&              target,
                                 ObjectFormatSchema const&        format,
                                 DiagnosticReporter&              reporter);

}  // namespace dss::linker
