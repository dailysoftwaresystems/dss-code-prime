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
//   * `decideUnitLinkerRequests` — which request STANDS where two units state
//     one option, what a link that makes a shared library does with a request,
//     what is refused by name, and what is warned and dropped. The first two are
//     the FORMAT's answers, stated per option by its directive vocabulary and
//     carried on each request (`UnitRequestPrecedence`,
//     `SharedLibraryDisposition`): this file holds no option's precedence. The
//     program's own `--stack-reserve` wins over any unit's stack request — that
//     one is DSS's own rule, the user's explicit choice over an object's;
//   * `applyUnitLinkerDecisions` — the two decisions that change the MODULE
//     rather than the header: a hide of another unit's definition, and an entry
//     that is a function of the program rather than the target's startup.
// Every diagnostic either makes names the object or archive member that stated
// the directive (`UnitImageRequest::unit`). A relocatable artifact decides
// nothing: it hands every request on to its final linker
// (`UnitLinkerRequests::handOn`), which decides them all.
namespace dss::linker {

struct DSS_EXPORT UnitLinkerDecisions {
    // The image writer's (`ImageRequest::directives`); empty for a relocatable
    // artifact.
    DirectiveImageSettings     image;
    // A FUNCTION of the link the image starts at, with no runtime startup (an
    // `/ENTRY:` naming no runtime startup); nullopt keeps the target's startup.
    std::optional<std::string> entrySymbol;
    // Whose directive that entry is — "'main.obj': the directive '/ENTRY:f'" —
    // for the refusals `applyUnitLinkerDecisions` makes of it.
    std::string                entryStatedBy;
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
// `image` requests — one naming a function, or a runtime startup no DSS image
// provides, becomes a warning the image link reports, and one naming the C
// runtime's own startup goes in silence, since it names the startup the image
// has anyway. The handed-on token stays, so a relocatable artifact's final
// linker decides for itself.
DSS_EXPORT void dropPulledMemberEntryRequests(UnitLinkerRequests& requests);

// `/ENTRY:name` is a REFERENCE to `name` (P69 send-back 5, review-xa4 MINOR 3;
// ✔MEASURED 2026-10-08, link.exe 14.44.35228 and lld-link 19.1.5 on cl 19.44
// objects: an object stating `/ENTRY:my_start`, `my_start` defined ONLY by an
// archive member, links and exits 42 under both, and without the archive both
// refuse the name as unresolved; the command line's `/ENTRY:` the same). So for
// a unit NAMED TO THE LINK, an entry it states and does not define — neither the
// C runtime's startup nor one no DSS image provides — becomes a REQUIRED
// reference of the unit (`ExternImport::requiredByDirective`, as an `/INCLUDE:`
// or an `/EXPORT:` makes one): the archive search pulls the member that defines
// it, a sibling unit's definition binds it, and the entry then finds it. One
// nothing defines is refused by name as any required reference is. A member the
// archive search itself pulls names no entry (`dropPulledMemberEntryRequests`)
// and is never handed here.
DSS_EXPORT void requireEntryReference(AssembledModule& unit);

}  // namespace dss::linker
