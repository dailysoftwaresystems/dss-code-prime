#pragma once

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/export.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/tree.hpp"
#include "lsp/protocol.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

// ══ D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES ════════
//
// THE ONE PLACE `src/lsp/` IS ALLOWED TO TURN A POSITION INTO AN OFFSET, OR AN
// OFFSET BACK INTO A POSITION.
//
// ★ THE DEFECT THIS TYPE EXISTS TO MAKE UNSPELLABLE. The LSP layer named a
// BUFFER nowhere. It held ONE coordinate space in its head and there are THREE:
//
//   DOCUMENT       what the editor sends and expects back — the file's own text
//   SYNTH          what the tree was parsed from: the "<built-in>" prologue,
//                  then every spliced quote-`#include`, then the main source
//   HEADER-ORIGIN  a real header file a synth range was copied from
//
// Every wrong answer had the same shape — a byte offset interpreted in a buffer
// that did not produce it — and it was INVISIBLE because the synth buffer is
// constructed with the MAIN SOURCE'S NAME, so a wrong answer names a plausible
// file at a shifted line. It stayed hidden until a config change made the
// prologue non-empty on every format; before that the three spaces coincided
// for exactly the fixture the tests used.
//
// ⚠ FIXING THE ARITHMETIC WAS NOT AN OPTION. The registry records this class
// three times already; patched arithmetic re-enters at the next channel. So the
// TYPE is the fix: a handler cannot obtain an offset except from `toSynth`, and
// cannot render a span except through `locate`, and `tree.source()` is
// unreachable from the handlers — enforced mechanically by
// `scripts/check-lsp-coordinates/`, not by a comment.

namespace dss::lsp {

// A position resolved into the coordinate space the trees actually use.
struct SynthPoint {
    dss::Tree const* tree = nullptr;
    dss::ByteOffset  offset{};
};

// A span rendered where a human can open it: the ORIGIN file's uri and a range
// in THAT file's text.
struct Located {
    std::string uri;
    Range       range;
};

// Built per request from the document the editor is asking about. Cheap: one
// `SourceBuffer` over the document text (its ctor just builds a line table).
class DSS_EXPORT DocumentCoordinates {
public:
    DocumentCoordinates(dss::CompilationUnit const& unit,
                        std::string documentUri,
                        std::string const& documentText);

    // ── INBOUND ─────────────────────────────────────────────────────────────
    //
    // Document position -> the synth offset a tree can be queried with.
    //
    // NOTHING when the position has no synth image, and that is a REAL answer,
    // not a failure: an `#if 0` region, the `#include` line the splice
    // consumed, a header live only under a dead branch, or a cursor past the
    // last byte. The caller returns its protocol default.
    // ⚠ It must NEVER fall back to a nearby offset. Answering a hover about the
    // neighbouring token IS this defect — a wrong answer that looks right.
    //
    // MANY images (a header spliced twice) -> the FIRST in synth order.
    // DECIDED, not incidental: hover / definition / completion are point
    // queries whose answer is a single symbol, and both images are copies of the
    // same source text, so they resolve the same symbol. `toSynthAll` is for the
    // enumerating handlers.
    [[nodiscard]] std::optional<SynthPoint> toSynth(Position pos) const;

    // Every synth image, in ascending synth order. For `references` / `rename`,
    // which enumerate results and take the UNION across images.
    // ⓘ The union does NOT duplicate edits: two synth images of one header
    // token map BACK to the same origin range, so `locate` collapses them and
    // the caller dedupes by (uri, range). That is why the multi-image case
    // changes where a rename's edits POINT and never what a rename MEANS.
    [[nodiscard]] std::vector<SynthPoint> toSynthAll(Position pos) const;

    // ── OUTBOUND ────────────────────────────────────────────────────────────
    //
    // A tree span -> the ORIGIN file's uri and a range in that file's own text,
    // resolved through the FORWARD map. Unlike the inbound direction this is a
    // function, not a relation: a synth offset came from exactly one origin.
    //
    // NOTHING when the origin is SYNTHETIC (`<built-in>`, `<command-line>`):
    // LSP has no "location in no file", so a definition that lands in a
    // compiler-generated prologue has no Location to give and the handler
    // returns its default. The DIAGNOSTIC channel decides this differently —
    // see `locateDiagnostic`.
    [[nodiscard]] std::optional<Located> locate(dss::Tree const& tree,
                                                dss::SourceSpan span) const;

    // The diagnostic variant of `locate`. A diagnostic must NEVER be dropped —
    // silence about a real error is worse than an imperfect position — so a
    // SYNTHETIC origin is published on the DOCUMENT at range 0:0 with the
    // origin NAMED, and `syntheticOrigin` carries that name so the caller can
    // prefix the message. This follows the schema-less-document precedent in
    // `publishDiagnostics_`, which already puts a document-level condition at
    // 0:0 rather than inventing a span.
    struct LocatedDiagnostic {
        std::string uri;
        Range       range;
        // Empty for a real file; the origin buffer's name (e.g. "<built-in>")
        // when the position was synthesized and the range was forced to 0:0.
        std::string syntheticOrigin;
    };
    [[nodiscard]] LocatedDiagnostic locateDiagnostic(
        dss::BufferId buffer, dss::SourceSpan span) const;

    [[nodiscard]] std::string const& documentUri() const noexcept {
        return documentUri_;
    }

private:
    // The origin buffer for `id`, or nullptr. Searches the CU's trees' own
    // sources AND its `auxiliaryBuffers()` — together those ARE the registry of
    // every buffer a position can name.
    [[nodiscard]] dss::SourceBuffer const* bufferFor(dss::BufferId id) const;

    dss::CompilationUnit const&           unit_;
    std::string                           documentUri_;
    std::shared_ptr<dss::SourceBuffer>    document_;
};

// TRUE for a buffer that names no file a user can open — the preprocessor's
// synthetic origins.
//
// ⓘ KEYED ON THE NAME, and that is the only signal a `SourceBuffer` carries.
// The `<...>` bracket spelling is this codebase's settled convention for a
// buffer with no file behind it (`<built-in>`, `<command-line>`, `<inline>`,
// `<mem0>`, and the renderer's own `<unknown-buffer:N>`), so the predicate
// reads the convention rather than enumerating today's four names — a list
// would go stale the first time a fifth synthetic origin is added.
[[nodiscard]] DSS_EXPORT bool bufferIsSynthetic(
    dss::SourceBuffer const& buffer) noexcept;

} // namespace dss::lsp
