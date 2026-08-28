#pragma once

#include "core/export.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
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

// ── [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]] — THE PROVENANCE OF A BYTE THAT
//    CAME FROM NO FILE ───────────────────────────────────────────────────────
//
// One run of the synth buffer's PRODUCT TAIL: bytes the preprocessor MINTED
// (a `#` stringize result, a `##` paste result, a predefined macro's value, an
// `#embed` byte list, a `_Pragma` operand) rather than copied from a file.
//
// ★★ WHY THE SEGMENT TABLE ALONE CANNOT ANSWER FOR THESE, AND WHY THAT WAS A
// WRONG POSITION RATHER THAN A MISSING ONE. A product token's span points PAST
// the end of every segment by construction, so the segment scan below picks the
// LAST segment and extrapolates `originStart + (offset - synthStart)` — an
// offset past the ORIGIN's end (✔MEASURED at this row's creation: 71 through
// 117 against a 50-byte origin). The renderer's own clamp then quietly showed
// end-of-file. So the byte HAS a well-defined position; it is simply not a
// position in any file's text, and the map has to say so rather than compute a
// plausible number.
//
// ★ THE ANSWER, AND IT IS THE ONE BOTH REFERENCES GIVE. ✔MEASURED, gcc 13.3.0
// and clang 18.1.3 on `#define STR(x) #x` + `int STR(name) = 1;`: clang reports
// the error at the EXPANSION SITE (`4:5`, the `STR` token in the user's file)
// with `note: expanded from macro 'STR'` at the macro's DEFINITION; gcc reports
// it inside line 4 with `note: in definition of macro 'STR'`. Neither invents a
// text offset for the minted bytes and neither points at end-of-file. So the
// record carries exactly those two locations.
//
// `siteOffset` is a PREFIX offset (it is the invoking token's own synth offset,
// propagated down through every nested/chained expansion by the expander), so
// resolving it goes through the ordinary segment scan and lands in a real file.
struct DSS_EXPORT MacroExpansionSite {
    ByteOffset  productStart  = 0;   // inclusive, FINAL synth coords
    ByteOffset  productEnd    = 0;   // exclusive, FINAL synth coords
    ByteOffset  siteOffset    = 0;   // synth offset of the EXPANSION SITE
    ByteOffset  defOffset     = 0;   // synth offset of the `#define`d NAME token
    bool        hasDefinition = false;  // false for a predefined / built-in mint
    std::string name;                // the macro or directive that minted the run
};

// What a synth offset IS. The map is the ONE owner of this question, and the
// enumeration is CLOSED so a consumer cannot invent a fourth interpretation.
enum class SynthOriginKind : std::uint8_t {
    // No answer: the map is empty, or a walk that must terminate did not.
    Unmapped,
    // A real byte of a real file: `origin[offset]` IS the byte.
    Direct,
    // A byte the splice INJECTED into the prefix — the newline glue between two
    // concatenated files, a descriptor's synthesized `#define` block, a rewritten
    // `#include <...>` line. It has no origin byte, so it answers with the
    // INJECTION POINT: the origin offset just past the preceding run.
    SyntheticGlue,
    // A byte MINTED by macro expansion. It answers with the EXPANSION SITE, and
    // `expansion` names the construct that minted it.
    Expansion,
    // The synthetic END of the translation unit (the Eof token's position, which
    // sits one past the last product byte). Answers with the end of the last
    // origin run.
    EndOfUnit,
    // ★ THE ESCAPE, AND IT EXISTS TO BE FAILED ON, NOT HANDLED. The arms above
    // are total and each is in-bounds by construction, so this is reachable only
    // from a segment that does not describe its own origin — a compiler bug. The
    // consumer's contract is to fail LOUD and named; a silent clamp here is what
    // turned a wrong position into an invisible one.
    Escaped,
};

// The full answer, and the ONLY one that carries the discriminator. `origin` is
// null for `Unmapped`; `offset` is guaranteed `<= origin->size()` for every
// other kind (a byte offset OR the one-past-the-end cursor, both of which the
// renderer positions correctly).
struct DSS_EXPORT SynthOrigin {
    SourceBuffer const*       origin    = nullptr;
    ByteOffset                offset    = 0;
    SynthOriginKind           kind      = SynthOriginKind::Unmapped;
    MacroExpansionSite const* expansion = nullptr;   // set iff kind == Expansion
};

// synth-offset -> (origin buffer, origin offset), and back. Built by the
// synth-buffer builder; consumed to remap diagnostics off the synth buffer back
// onto the real header/main file, and to carry an editor position the other
// way. A run is a VERBATIM copy (offsets advance 1:1 within a segment), so the
// origin offset of a synth offset `o` in segment `s` is
// `s.originStart + (o - s.synthStart)`.
//
// ⚠ ✔MEASURED CORRECTION TO WHAT THIS COMMENT USED TO CLAIM. It said offsets in
// SYNTHESIZED glue "map to the nearest preceding segment's origin -- good enough
// for attribution and never out of bounds". The first half was the behaviour;
// the second half was FALSE, and not only for macro products. The splice injects
// MULTI-BYTE glue into the prefix — a shipped descriptor's whole `#define` block
// (`copyVerbatim`'s callers append `defs` with no segment), a rewritten
// `#include <h>` line — and the extrapolation `originStart + (o - synthStart)`
// runs past the origin's end by the full length of that glue. `originOf` below
// answers the injection POINT instead, which is in bounds by construction and
// names the `#include` that pulled the text in.
//
// ⚠ SEGMENTS ARE IN ASCENDING `synthStart` ORDER, by construction of the
// builder. `resolve`'s early `break` already depends on it; `inverse` relies on
// it for its output ordering. Neither re-sorts, deliberately: a sort would
// silently paper over a builder that broke the invariant `resolve` needs.
//
// ⚠ SEGMENTS ARE IN ASCENDING `synthStart` ORDER, by construction of the
// builder. `resolve`'s early `break` already depends on it; `inverse` relies on
// it for its output ordering. Neither re-sorts, deliberately: a sort would
// silently paper over a builder that broke the invariant `resolve` needs.
class DSS_EXPORT LineMap {
public:
    void addSegment(LineMapSegment seg) { segments_.push_back(std::move(seg)); }

    // ── [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: the PRODUCT TAIL ──────────
    //
    // The synth buffer is `prefix + productText`. `productBase` is the length of
    // the prefix, i.e. the first offset that belongs to the MINTED tail. It is
    // stamped by `preprocess()` the moment the splice finishes, BEFORE the macro
    // pass runs, so every expansion record added below is inside a region the
    // map already knows the shape of.
    //
    // ⚠ UNSET is a distinct state from `productBase == size`. A map built by a
    // producer that mints nothing (or by a test) leaves it unset, and then EVERY
    // offset is a prefix offset — which is exactly the pre-product behaviour.
    // Defaulting it to zero instead would make every offset a product offset.
    void setProductBase(ByteOffset base) noexcept {
        productBase_    = base;
        hasProductBase_ = true;
    }
    [[nodiscard]] bool       hasProductBase() const noexcept { return hasProductBase_; }
    [[nodiscard]] ByteOffset productBase()    const noexcept { return productBase_; }

    // ⚠ APPEND IN ASCENDING `productStart` ORDER. The producer
    // (`MacroExpander::materializeSignificant`) appends to one monotonically
    // growing string, so this holds by construction; `expansionFor` breaks early
    // on it. Nothing here re-sorts, for the same reason `addSegment` does not.
    void addExpansion(MacroExpansionSite site) {
        expansions_.push_back(std::move(site));
    }
    [[nodiscard]] std::span<MacroExpansionSite const> expansions() const noexcept {
        return expansions_;
    }

    // The record whose product run CONTAINS `synthOffset`, or null. Half-open,
    // matching `synthEnd` and every other range in this file.
    [[nodiscard]] MacroExpansionSite const*
    expansionFor(ByteOffset synthOffset) const noexcept {
        for (MacroExpansionSite const& e : expansions_) {
            if (e.productStart > synthOffset) break;   // ascending -> no later hit
            if (synthOffset < e.productEnd) return &e;
        }
        return nullptr;
    }

    // ★★★ THE ONE OWNER OF "WHAT IS THIS SYNTH OFFSET". Total, closed-domain,
    // and VALIDATED: every kind but `Unmapped` comes back with an offset that is
    // a real position in the buffer it names (`offset <= origin->size()`), and
    // an offset that cannot satisfy that comes back as `Escaped` rather than as
    // a plausible number. `resolve` and `PreprocessResult::makeRemap` are both
    // expressed in terms of this, so there is exactly ONE implementation of the
    // forward direction and no consumer can re-derive a different answer.
    [[nodiscard]] SynthOrigin originOf(ByteOffset synthOffset) const noexcept {
        if (segments_.empty()) return {};

        // ── (1) THE PRODUCT TAIL. A minted byte's position IS its expansion
        // site, so hop to the site and resolve THAT. The site is a prefix offset
        // by construction (the expander propagates the ORIGINAL invoking token's
        // offset down through every nested and chained expansion), so one hop
        // suffices; the bound is defence against a malformed record, and it
        // refuses rather than looping.
        MacroExpansionSite const* minted = nullptr;
        ByteOffset                at     = synthOffset;
        if (hasProductBase_) {
            constexpr int kMaxHops = 64;
            for (int hop = 0; hop <= kMaxHops; ++hop) {
                if (at < productBase_) break;
                MacroExpansionSite const* const e = expansionFor(at);
                if (e == nullptr) {
                    // Past every minted run: the synthetic END OF THE UNIT (the
                    // Eof token sits one past the last product byte). The end of
                    // the last origin run is the position for it.
                    SynthOrigin r = endOfLastRun_();
                    r.expansion = minted;
                    if (r.kind != SynthOriginKind::Unmapped
                        && r.kind != SynthOriginKind::Escaped) {
                        r.kind = SynthOriginKind::EndOfUnit;
                    }
                    return r;
                }
                if (minted == nullptr) minted = e;
                if (e->siteOffset == at) break;   // no progress -> refuse to spin
                at = e->siteOffset;
            }
            // The walk either reached a prefix offset or ran out of hops; the ONE
            // test decides both, so an offset that became resolvable on the last
            // hop is not thrown away for arriving late.
            if (at >= productBase_) return {};    // Unmapped: still not a prefix offset
        }

        // ── (2) THE PREFIX. Pick the segment that starts at or before `at`.
        LineMapSegment const* best = nullptr;
        for (LineMapSegment const& seg : segments_) {
            if (seg.synthStart <= at) best = &seg;
            else break;
        }
        SynthOrigin r;
        r.expansion = minted;
        if (best == nullptr) {
            // BEFORE the first segment: the non-emitted predefine/`--define`
            // prologue. No origin byte exists; answer with the first origin's
            // own start, which is where that prologue was injected.
            best     = &segments_.front();
            r.kind   = SynthOriginKind::SyntheticGlue;
            r.offset = best->originStart;
        } else {
            const ByteOffset runLen = (best->synthEnd > best->synthStart)
                                          ? (best->synthEnd - best->synthStart) : 0;
            const ByteOffset delta  = at - best->synthStart;
            if (delta < runLen) {
                r.kind   = SynthOriginKind::Direct;
                r.offset = best->originStart + delta;
            } else {
                // SYNTHESIZED GLUE in the prefix. NOT extrapolated: the answer is
                // the INJECTION POINT — the origin offset just past this run —
                // which is in bounds by construction and names the `#include`
                // (or the splice) that put the synthetic text there.
                r.kind   = SynthOriginKind::SyntheticGlue;
                r.offset = best->originStart + runLen;
            }
        }
        r.origin = best->origin.get();
        if (minted != nullptr && r.kind != SynthOriginKind::Unmapped) {
            r.kind = SynthOriginKind::Expansion;
        }
        return validated_(r);
    }

    // Resolve a synth offset. Returns {origin buffer (may be null if the map
    // is empty or the offset has no position at all), origin offset}. Never
    // aborts. DERIVED from `originOf` so the two can never disagree: a product
    // offset resolves to its EXPANSION SITE here too, which is what makes
    // `__LINE__`/`__FILE__` and a `#line` record correct for a directive reached
    // through a macro rather than extrapolated past the origin's end.
    struct Resolved {
        SourceBuffer const* origin = nullptr;
        ByteOffset          offset = 0;
    };
    [[nodiscard]] Resolved resolve(ByteOffset synthOffset) const noexcept {
        const SynthOrigin r = originOf(synthOffset);
        if (r.kind == SynthOriginKind::Unmapped
            || r.kind == SynthOriginKind::Escaped) {
            return {};
        }
        return Resolved{r.origin, r.offset};
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
    // The END of the last origin run — the position of "after everything this
    // translation unit copied". Used for the synthetic Eof position, which sits
    // past every product byte and so belongs to no run at all.
    [[nodiscard]] SynthOrigin endOfLastRun_() const noexcept {
        if (segments_.empty()) return {};
        LineMapSegment const& last = segments_.back();
        SynthOrigin r;
        r.origin = last.origin.get();
        r.offset = last.originStart
                   + ((last.synthEnd > last.synthStart)
                          ? (last.synthEnd - last.synthStart) : 0);
        r.kind   = SynthOriginKind::SyntheticGlue;
        return validated_(r);
    }

    // ★ THE VALIDATION THE PRODUCER NEVER DID. Every arm above is in bounds by
    // construction — a segment is a VERBATIM copy of `[originStart, +runLen)` of
    // its origin, so `originStart + runLen <= origin->size()` — which makes this
    // an internal-consistency assertion rather than a fallback. It exists so the
    // failure is a NAMED kind the consumer must handle loudly, and so no future
    // arm can quietly reintroduce an extrapolation: an escaping offset can no
    // longer leave this function wearing a plausible number.
    //
    // `offset == origin->size()` is ACCEPTED, deliberately: one-past-the-end is
    // the legal cursor for an injection point and for end-of-unit, and the
    // renderer positions it on the last line rather than reading it.
    [[nodiscard]] static SynthOrigin validated_(SynthOrigin r) noexcept {
        if (r.origin == nullptr) return {};
        if (r.offset > r.origin->size()) {
            r.kind = SynthOriginKind::Escaped;
        }
        return r;
    }

    std::vector<LineMapSegment>     segments_;
    // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: the minted tail's provenance.
    // Empty for every producer that mints nothing, which is exactly the
    // pre-product behaviour at zero cost.
    std::vector<MacroExpansionSite> expansions_;
    ByteOffset                      productBase_    = 0;
    bool                            hasProductBase_ = false;
};

} // namespace dss
