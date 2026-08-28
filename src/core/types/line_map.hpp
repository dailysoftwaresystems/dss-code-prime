#pragma once

#include "core/export.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <memory>
#include <span>
#include <vector>

// ── D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES ────────
//
// The synth-buffer <-> origin-buffer coordinate map, EXTRACTED here from
// `analysis/preprocess/preprocessor.hpp` so that both tiers that must speak
// about preprocessor coordinates can include it without pulling the whole
// preprocessor surface.
//
// ★ WHY THE MOVE, AND WHY IT IS THE FIX RATHER THAN A TIDY-UP. The map used to
// be reachable only as a `std::function` sealed inside `PreprocessedPositionMap`,
// which exposes exactly ONE direction. Every consumer that needed the other
// direction — and the LSP needs it on every keystroke — had no option but to
// treat a synth offset as if it were a document offset. Putting the segments
// where both `CompilationUnit` and `src/lsp/` can reach them is what makes the
// inverse derivable from the SAME data instead of from a second table.
//
// Both operations are INLINE and this header has no `.cpp`: a translation unit
// that can see the segments can answer both directions with no link edge, which
// is what keeps `src/lsp/` from having to depend on `analysis/preprocess/`.

namespace dss {

// One contiguous run of the synthesized buffer that came VERBATIM from a
// single origin buffer. The synth buffer is a concatenation of such runs
// (header text spliced in where a quote-include was), so a binary search on
// `synthStart` resolves any synth offset to its origin.
struct DSS_EXPORT LineMapSegment {
    ByteOffset                    synthStart = 0;   // inclusive, synth coords
    ByteOffset                    synthEnd   = 0;   // exclusive, synth coords
    std::shared_ptr<SourceBuffer> origin;           // the real file this run came from
    ByteOffset                    originStart = 0;   // origin offset of synthStart
};

// synth-offset -> (origin buffer, origin offset), and back. Built by the
// synth-buffer builder; consumed to remap diagnostics off the synth buffer back
// onto the real header/main file, and to carry an editor position the other
// way. A run is a VERBATIM copy (offsets advance 1:1 within a segment), so the
// origin offset of a synth offset `o` in segment `s` is
// `s.originStart + (o - s.synthStart)`. Offsets that land in SYNTHESIZED glue
// (e.g. an injected newline between concatenated files) map to the nearest
// preceding segment's origin -- good enough for attribution and never out of
// bounds.
//
// ⚠ SEGMENTS ARE IN ASCENDING `synthStart` ORDER, by construction of the
// builder. `resolve`'s early `break` already depends on it; `inverse` relies on
// it for its output ordering. Neither re-sorts, deliberately: a sort would
// silently paper over a builder that broke the invariant `resolve` needs.
class DSS_EXPORT LineMap {
public:
    void addSegment(LineMapSegment seg) { segments_.push_back(std::move(seg)); }

    // Resolve a synth offset. Returns {origin buffer (may be null if the map
    // is empty), origin offset}. Never aborts.
    struct Resolved {
        SourceBuffer const* origin = nullptr;
        ByteOffset          offset = 0;
    };
    [[nodiscard]] Resolved resolve(ByteOffset synthOffset) const noexcept {
        if (segments_.empty()) return {};
        LineMapSegment const* best = &segments_.front();
        for (auto const& seg : segments_) {
            if (seg.synthStart <= synthOffset) best = &seg;
            else break;
        }
        Resolved r;
        r.origin = best->origin.get();
        const ByteOffset delta = (synthOffset >= best->synthStart)
                                     ? (synthOffset - best->synthStart) : 0;
        r.offset = best->originStart + delta;
        return r;
    }

    // The INVERSE of `resolve`, over the SAME `segments_` vector — never a
    // second table and never an index-parallel side list, because two
    // containers describing one splice structure is exactly the drift the
    // preprocessor's own four-seed-site note exists about.
    //
    // ★ IT IS A RELATION, NOT A FUNCTION, and all three cases are decided here
    // rather than left to callers:
    //
    //  * ZERO images — an origin byte that never reached the synth buffer: a
    //    header live only under a dead branch (never spliced at all), or an
    //    offset one past a segment's last byte. `out` is left EMPTY and the
    //    caller answers with its protocol default. ⚠ NEVER the nearest
    //    neighbour: an offset with no image is precisely the case where a
    //    plausible-looking answer IS the defect, so there is deliberately no
    //    softening fallback here.
    //    ⓘ ✔MEASURED, and it corrects a claim this comment first carried: an
    //    `#if 0` REGION IS NOT ONE OF THESE. The synth buffer is built by TEXT
    //    CONCATENATION of the whole file and a dead branch's TOKENS are what
    //    get elided, so dead text has a perfectly good image and simply owns no
    //    nodes. Callers that reach for a NODE already answer their default
    //    there; nothing about this relation needs to.
    //
    //  * MANY images — an origin spliced more than once (a header included
    //    twice without a guard, or under two live branches). EVERY image is
    //    returned, in ASCENDING SYNTH ORDER; what a consumer does with them is
    //    that consumer's documented decision (see `lsp_coordinates.hpp`).
    //
    //  * SYNTHESIZED GLUE — `resolve` attributes glue to the nearest PRECEDING
    //    segment so attribution is never out of bounds. The inverse does NOT
    //    invent the matching origin byte: glue lies outside every segment's
    //    synth range, so nothing here can return it, and no origin offset maps
    //    ONTO glue.
    //
    // Membership is HALF-OPEN in both systems, matching `synthEnd` and
    // `resolve`'s arithmetic: origin offset `o` is inside segment `s` iff
    // `s.originStart <= o < s.originStart + (s.synthEnd - s.synthStart)`.
    //
    // `out` is CLEARED first, and is the caller's so a query loop (references /
    // rename walk many nodes) reuses one allocation.
    void inverse(BufferId originBuffer, ByteOffset originOffset,
                 std::vector<ByteOffset>& out) const {
        out.clear();
        for (LineMapSegment const& seg : segments_) {
            if (!seg.origin) continue;
            if (!(seg.origin->id() == originBuffer)) continue;
            if (originOffset < seg.originStart) continue;
            // A run is a verbatim copy, so its origin length IS its synth
            // length — the same identity `resolve` uses going the other way.
            const ByteOffset span  = seg.synthEnd - seg.synthStart;
            const ByteOffset delta = originOffset - seg.originStart;
            if (delta >= span) continue;
            out.push_back(seg.synthStart + delta);
        }
    }

    [[nodiscard]] std::span<LineMapSegment const> segments() const noexcept {
        return segments_;
    }
    [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }

private:
    std::vector<LineMapSegment> segments_;
};

} // namespace dss
