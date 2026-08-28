#include "lsp/lsp_coordinates.hpp"

#include "lsp/lsp_semantic_query.hpp"
#include "lsp/workspace_project.hpp"

// D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES.
// Contract, and the reasoning behind every decision here, live on the
// declarations in `lsp_coordinates.hpp`.

namespace dss::lsp {

bool bufferIsSynthetic(dss::SourceBuffer const& buffer) noexcept {
    std::string_view const name = buffer.name();
    return name.size() >= 2 && name.front() == '<' && name.back() == '>';
}

namespace {

// A buffer's uri, from the only identity a `SourceBuffer` carries: its NAME.
//
// ⚠ AND THE NAME IS NOT ALWAYS A PATH — this is the one thing the first cut of
// this unit got wrong, and the tests caught it in the most useful possible way
// (`"file:///file:/x.c"`). A buffer's name is "how this buffer was identified",
// and the two tiers identify differently: the CLI/driver names a source buffer
// by FILESYSTEM PATH, while the LSP names the document buffer by the URI the
// client sent. Both reach here, because the same `CompilationUnit` is built by
// both. Encoding a name that is already a uri produces a uri-inside-a-uri that
// no client can open — and, worse, it did not MATCH the document's own uri, so
// every diagnostic was routed into a second publish for a file that does not
// exist.
//
// The test is the round-trip itself: if the name parses back as a `file:` uri,
// it IS one. That is exactly the property callers need, and it costs one parse
// on a path already doing filesystem-shaped work.
[[nodiscard]] std::string uriForBufferName(std::string_view name) {
    if (pathFromFileUri(name).has_value()) return std::string{name};
    return fileUriFromPath(name);
}

} // namespace

DocumentCoordinates::DocumentCoordinates(dss::CompilationUnit const& unit,
                                         std::string documentUri,
                                         std::string const& documentText)
    : unit_(unit), documentUri_(std::move(documentUri)) {
    // Named for the DOCUMENT's uri deliberately: if this buffer ever leaks into
    // a position answer, the name in the rendering says which buffer it was,
    // instead of impersonating the main source the way the synth buffer does.
    document_ = dss::SourceBuffer::fromString(documentText, documentUri_);
}

dss::SourceBuffer const* DocumentCoordinates::bufferFor(dss::BufferId id) const {
    if (!id.valid()) return nullptr;
    for (auto const& t : unit_.trees()) {
        if (t.source().id() == id) return &t.source();
    }
    for (auto const& b : unit_.auxiliaryBuffers()) {
        if (b && b->id() == id) return b.get();
    }
    return nullptr;
}

std::vector<SynthPoint> DocumentCoordinates::toSynthAll(Position pos) const {
    std::vector<SynthPoint> out;
    auto trees = unit_.trees();
    if (trees.empty()) return out;

    // The document offset, computed against the DOCUMENT's own text. This is
    // the one conversion that is unambiguously correct: the position came from
    // an editor looking at exactly these bytes.
    const dss::ByteOffset docOffset =
        positionToByteOffset(*document_, pos);

    std::vector<dss::ByteOffset> images;
    for (auto const& tree : trees) {
        const dss::BufferId synth = tree.source().id();
        const dss::BufferId mainOrigin = unit_.mainOriginForSynth(synth);
        if (!mainOrigin.valid()) {
            // NOT preprocessed: this tree's `source()` IS the file, so document
            // and tree coordinates are the same space and no translation
            // exists to do. That is the structural truth for a language with no
            // `preprocess` block (toy / tsql / assembly), NOT a fallback — and
            // it is why the LSP never has to branch on the language: it asks
            // the CU whether a synth map exists and believes the answer.
            out.push_back(SynthPoint{&tree, docOffset});
            continue;
        }
        unit_.inversePreprocessedPositions(mainOrigin, docOffset, images);
        for (dss::ByteOffset const o : images) {
            out.push_back(SynthPoint{&tree, o});
        }
    }
    return out;
}

std::optional<SynthPoint> DocumentCoordinates::toSynth(Position pos) const {
    auto all = toSynthAll(pos);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::optional<Located> DocumentCoordinates::locate(dss::Tree const& tree,
                                                   dss::SourceSpan span) const {
    dss::BufferId   buffer = tree.source().id();
    dss::SourceSpan mapped = span;
    // The FORWARD direction, through the CU's one owner. For a tree that was
    // never preprocessed this leaves the position exactly where it was.
    unit_.remapPreprocessedPosition(buffer, mapped);

    dss::SourceBuffer const* origin = bufferFor(buffer);
    if (origin == nullptr) return std::nullopt;
    if (bufferIsSynthetic(*origin)) return std::nullopt;

    Located out;
    // The origin's NAME is its path. `fileUriFromPath` is the exact inverse of
    // the `pathFromFileUri` every request arrives through, so a location we
    // emit can be sent back to us unchanged.
    out.uri   = uriForBufferName(origin->name());
    out.range = spanToRange(*origin, mapped);
    return out;
}

DocumentCoordinates::LocatedDiagnostic
DocumentCoordinates::locateDiagnostic(dss::BufferId buffer,
                                      dss::SourceSpan span) const {
    LocatedDiagnostic out;
    dss::SourceBuffer const* origin = bufferFor(buffer);

    if (origin != nullptr && !bufferIsSynthetic(*origin)) {
        out.uri   = uriForBufferName(origin->name());
        out.range = spanToRange(*origin, span);
        return out;
    }

    // SYNTHETIC or unknown origin. A diagnostic is never dropped: publish it on
    // the document at 0:0 and hand the caller the origin's name so the message
    // can say where it really came from. Range 0:0 is the existing precedent
    // for a condition that belongs to the document rather than to a span.
    out.uri             = documentUri_;
    out.range           = Range{};
    out.syntheticOrigin = (origin != nullptr) ? std::string{origin->name()}
                                              : std::string{"<unknown-buffer>"};
    return out;
}

} // namespace dss::lsp
