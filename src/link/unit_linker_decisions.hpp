#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/unit_linker_requests.hpp"
#include "link/image_request.hpp"
#include "link/object_format_schema.hpp"

#include <optional>
#include <string>
#include <vector>

// ── WHAT A LINK DECIDES FROM ITS UNITS' LINKER-DIRECTIVE REQUESTS ──────────
// P69 round 4 (D-LK-COFF-READER-SKIPPED-EVERY-LINKER-DIRECTIVE).
//
// A foreign COFF object's directives ask the link for an image property
// (`/STACK:`, `/SUBSYSTEM:`, `/ENTRY:`, `/SECTION:` ...), for an export or an
// exclusion of ANOTHER unit's definition, or for something no DSS image can
// carry yet. The object reader states each on its unit
// (`AssembledModule::linkerRequests`); the cross-unit merge folds them in UNIT
// order; and `linker::link` asks THIS file two questions about the module it is
// about to write:
//   * `decideUnitLinkerRequests` — which request WINS, with the precedence
//     link.exe gives each option (✔MEASURED 2026-10-07, link.exe 14.44.35228
//     and lld-link 19.1.5 on cl 19.44 objects: `/STACK:`, `/HEAP:`, `/BASE:` and
//     `/ENTRY:` the first; `/SUBSYSTEM:`, `/VERSION:` and `/ALIGN:` the last;
//     `/SECTION:` in order; the program's own `--stack-reserve` over any unit's),
//     what is refused by name, and what is warned and dropped as the reference
//     linkers drop it;
//   * `applyUnitLinkerDecisions` — the two decisions that change the MODULE
//     rather than the header: a hide of another unit's definition, and an entry
//     that is a function of the program rather than the target's startup.
// A relocatable artifact decides nothing: it hands every request on to its
// final linker (`UnitLinkerRequests::handOn`), which decides them all.
namespace dss::linker {

struct DSS_EXPORT UnitLinkerDecisions {
    // The image writer's (`ImageRequest::directives`); empty for a relocatable
    // artifact.
    DirectiveImageSettings     image;
    // A FUNCTION of the link the image starts at, with no runtime startup (an
    // `/ENTRY:` naming no runtime startup); nullopt keeps the target's startup.
    std::optional<std::string> entrySymbol;
    // Definitions the image must not export, wherever the link defines them.
    std::vector<std::string>   hides;
};

// Decide `requests` — the merged module's, in unit order — for an artifact of
// `format`; `programRequest` is the program's own image request (its
// `stackReserveBytes` wins over every unit's stack request). nullopt after
// reporting a refusal by name.
[[nodiscard]] DSS_EXPORT std::optional<UnitLinkerDecisions>
decideUnitLinkerRequests(UnitLinkerRequests const& requests,
                         ObjectFormatSchema const& format,
                         ImageRequest const&       programRequest,
                         DiagnosticReporter&       reporter);

// Apply the module-level decisions to `in`: each hidden name's external
// definition becomes `SymbolVisibility::Hidden`, and an entry symbol becomes
// the module's `imageEntryOverride` (so no startup trampoline is injected).
// Returns false when nothing changes (`out` untouched), true after writing the
// changed module to `out`, and nullopt after reporting a refusal by name: an
// entry naming a datum or nothing the link defines.
[[nodiscard]] DSS_EXPORT std::optional<bool>
applyUnitLinkerDecisions(AssembledModule const&     in,
                         UnitLinkerDecisions const& decisions,
                         AssembledModule&           out,
                         DiagnosticReporter&        reporter);

// A member the ARCHIVE SEARCH pulls names no entry. link.exe fixes the image's
// entry before it searches an archive, so a member it pulls cannot move it, and
// GNU ld reads no MS `/ENTRY:` at all (✔MEASURED 2026-10-07: a cl member stating
// `/ENTRY:f`, pulled from an archive, starts `main` under both; the same object
// named on the command line starts `f` under link.exe and lld-link; lld-link
// alone starts `f` from the archive too). The static-link pull hands each
// member it pulls to this function: every entry request leaves the member's
// `image` requests — one naming a function becomes a warning the image link
// reports, and one naming the C runtime's startup goes in silence, since it
// names the startup the image has anyway. The handed-on token stays, so a
// relocatable artifact's final linker decides for itself.
DSS_EXPORT void dropPulledMemberEntryRequests(UnitLinkerRequests& requests);

}  // namespace dss::linker
