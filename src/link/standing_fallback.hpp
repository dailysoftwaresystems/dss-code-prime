#pragma once

// ══ THE ONE HOME OF "WHICH OF A NAME'S FALLBACKS STANDS" (P69 H6c2) ══════════════════════════════════════════════
//
// A COFF link can be told two things a name resolves to when nothing defines it: a WEAK EXTERNAL's own default (PE/COFF
// 5.5.3; the absolute 0 -- "nothing" -- or another undefined symbol B) and a linker DIRECTIVE's fallback
// (`/alternatename:A=B`). When a link is given more than one for one name, ONE stands, and it is decided here, by the
// reference linkers' rule (✔MEASURED 2026-11-16 on windows-x86_64-release, link.exe of cl 19.52 and MinGW gcc 13.2's
// GNU ld, every shape in both orders; lld-link is absent on every leg):
//   1. a weak external's own default stands over every directive fallback -- a defined default and the absolute 0
//      alike, across objects (u9 F2) and inside one object whose own directive section names the same name (u11);
//      GNU ld does not read `/alternatename` at all ("unrecognized");
//   2. of two weak defaults, the FIRST in link order stands (u12);
//   3. of two directive fallbacks, the FIRST in link order stands (u7) -- within one object, the first in its
//      directive section's order (u8).
// link.exe says nothing about a dropped one; DSS WARNS, naming both, as `unit_linker_decisions.cpp` warns about a
// dropped export. "Link order" is the order the link holds its inputs in (`linkPositionPrecedes`, the operator's
// order of the inputs, D-LK-THE-LINK-ORDER-IS-NOT-THE-COMMAND-LINE-ORDER) -- never the order a caller's span holds
// modules in -- and at one place the statement the caller met FIRST stands.
//
// Every site where two statements of one name meet calls `standingFallback`: the COFF reader (one object's directives,
// and a weak external beside its own object's directive), the image link (`linker::bindFallbackReferences`), the
// relocatable merge, and the archive search's offer of fallbacks.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/cross_cu_resolve.hpp"   // linkPositionPrecedes -- the one comparison of two places in the link

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dss::linker {

// Where a statement of a name's fallback came from.
enum class FallbackSource : std::uint8_t {
    WeakDefault,   // a weak external's own default
    Directive,     // a linker directive's `/alternatename:`
};

// One statement of what a name resolves to when nothing defines it.
struct FallbackStatement {
    std::string                  fallback;   // the name it resolves to; EMPTY for a weak default of the absolute 0
    FallbackSource               source = FallbackSource::Directive;
    std::optional<std::uint32_t> place;      // the input's place in the link; none stands after every stated one
    std::string                  statedBy;   // who stated it, for the warning
};

// Does `candidate` stand over `prior`, a statement of the same name the caller met EARLIER?
[[nodiscard]] inline bool fallbackStandsOver(FallbackStatement const& candidate, FallbackStatement const& prior) noexcept {
    if (candidate.source != prior.source) return candidate.source == FallbackSource::WeakDefault;
    return linkPositionPrecedes(candidate.place, prior.place);
}

// THE ONE DECISION: of `standing` (met first) and `candidate`, keeps in `standing` the statement that stands, and warns
// (K_LinkerDirectiveUnhonourable, a Warning) when the dropped one names a different fallback.
inline void standingFallback(std::string_view name, FallbackStatement& standing, FallbackStatement candidate,
                             DiagnosticReporter& reporter) {
    bool const replaces = fallbackStandsOver(candidate, standing);
    FallbackStatement const& kept    = replaces ? candidate : standing;
    FallbackStatement const& dropped = replaces ? standing : candidate;
    if (kept.fallback != dropped.fallback) {
        auto const spell = [](FallbackStatement const& s) {
            return s.fallback.empty() ? std::string{"nothing (the absolute 0)"} : "'" + s.fallback + "'";
        };
        auto const what = [](FallbackStatement const& s) {
            return s.source == FallbackSource::WeakDefault ? std::string{"a weak external's default"}
                                                           : std::string{"a linker directive"};
        };
        report(reporter, DiagnosticCode::K_LinkerDirectiveUnhonourable, DiagnosticSeverity::Warning,
               std::format("linker: '{}' is given the fallback {} by {} of {}, and {} by {} of {}; {} stands -- {} -- "
                           "and {} is dropped, as link.exe and GNU ld drop it.",
                           name, spell(kept), what(kept), kept.statedBy, spell(dropped), what(dropped),
                           dropped.statedBy, spell(kept),
                           kept.source != dropped.source ? "a weak external's own default stands over every directive"
                                                         : "the first in link order stands",
                           spell(dropped)));
    }
    if (replaces) standing = std::move(candidate);
}

}  // namespace dss::linker
