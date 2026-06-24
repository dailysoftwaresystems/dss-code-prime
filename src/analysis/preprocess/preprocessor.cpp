#include "analysis/preprocess/preprocessor.hpp"

#include "analysis/preprocess/pp_if_eval.hpp"
#include "tokenizer/tokenizer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace dss {

namespace {

[[noreturn]] void ppFatal(char const* what) {
    std::fputs("dss::preprocess fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

constexpr int kMaxIncludeDepth = 64;

void emitPP(DiagnosticReporter& rep, DiagnosticCode code, BufferId buffer,
            SourceSpan span, std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.buffer   = buffer;
    d.span     = span;
    d.actual   = std::move(actual);
    rep.report(std::move(d));
}

bool isTrivia(Token const& t) {
    return t.coreKind == CoreTokenKind::Whitespace
        || t.coreKind == CoreTokenKind::LineComment
        || t.coreKind == CoreTokenKind::BlockComment
        || isEmptySpace(t.flags);
}
bool isNewline(Token const& t) { return t.coreKind == CoreTokenKind::Newline; }

// Phase-2 line-continuation splice: delete every backslash-newline pair,
// recording 1:1 line-map segments per verbatim run so synth offsets remap to
// the ORIGINAL file. Appends to out/map based at out.size().
void appendWithContinuationSplice(std::string_view text,
                                  std::shared_ptr<SourceBuffer> const& origin,
                                  ByteOffset originBase,
                                  std::string& out,
                                  LineMap& map) {
    std::size_t runStart = 0;
    std::size_t i        = 0;
    const char backslash = '\\';
    const char newline   = '\n';
    const char carriage  = '\r';
    auto flushRun = [&](std::size_t end) {
        if (end <= runStart) return;
        LineMapSegment seg;
        seg.synthStart  = static_cast<ByteOffset>(out.size());
        out.append(text.substr(runStart, end - runStart));
        seg.synthEnd    = static_cast<ByteOffset>(out.size());
        seg.origin      = origin;
        seg.originStart = static_cast<ByteOffset>(originBase + runStart);
        map.addSegment(std::move(seg));
    };
    while (i < text.size()) {
        if (text[i] == backslash && i + 1 < text.size()
            && text[i + 1] == newline) {
            flushRun(i);
            i += 2;
            runStart = i;
        } else if (text[i] == backslash && i + 2 < text.size()
                   && text[i + 1] == carriage && text[i + 2] == newline) {
            flushRun(i);
            i += 3;
            runStart = i;
        } else {
            ++i;
        }
    }
    flushRun(text.size());
}

struct PPToken {
    Token            tok;
    std::string_view text;
};

std::vector<PPToken> tokenizeToPP(
    std::shared_ptr<SourceBuffer> const& buffer,
    std::shared_ptr<GrammarSchema const> const& schema,
    DiagnosticReporter& rep) {
    Tokenizer tk{buffer, schema};
    auto result = std::move(tk).tokenize();
    if (result.diagnostics) {
        for (auto const& d : result.diagnostics->all()) rep.report(d);
    }
    std::vector<PPToken> out;
    while (!result.stream.isAtEnd()) {
        Token t = result.stream.advance();
        if (t.coreKind == CoreTokenKind::Eof) break;
        out.push_back(PPToken{t, buffer->slice(t.span)});
    }
    return out;
}

} // namespace

LineMap::Resolved LineMap::resolve(ByteOffset synthOffset) const noexcept {
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

std::function<void(BufferId&, SourceSpan&)> PreprocessResult::makeRemap() const {
    BufferId const synthId = synthBuffer ? synthBuffer->id() : BufferId{};
    // Redirect EVERY synth-buffer diagnostic onto its real origin buffer --
    // both an included HEADER and the ORIGINAL main file. A header-origin span
    // attributes to the header:line; a main-origin span attributes to the
    // ORIGINAL main.c offset (the line-map already shifted it back across any
    // splice), so a main-file error after a leading `#include` reports the
    // real main.c line, not a synth-shifted one. The caller registers all
    // origin buffers (`PreprocessResult::originBuffers`, surfaced as the CU's
    // `auxiliaryBuffers()`) with the diagnostic registry so the redirected
    // buffer id resolves for rendering. (Previously main-origin diagnostics
    // were kept on the synth buffer -- byte-identical only for a no-include
    // TU -- which mis-positioned a post-include main error: the
    // D-PP-INCLUDE-SPLICE-POSITION-ATTRIBUTION deferral, now closed.)
    // Capture the line-map BY VALUE: the returned closure is stored in the
    // CU sidecar (`ppRemap`) and re-invoked by the FC2 oracle reparse LONG
    // after this PreprocessResult is destroyed, so a pointer into
    // `this->lineMap` would dangle. The copy is a vector of segments
    // (shared_ptr origins), cheap relative to a reparse.
    LineMap mapCopy = lineMap;
    return [synthId, mapCopy = std::move(mapCopy)]
           (BufferId& buffer, SourceSpan& span) {
        if (!(buffer == synthId)) return;
        auto s = mapCopy.resolve(span.start());
        auto e = mapCopy.resolve(span.end());
        if (s.origin == nullptr) return;   // empty map -> leave on synth.
        buffer = s.origin->id();
        if (e.origin == s.origin && e.offset >= s.offset) {
            span = SourceSpan::of(s.offset, e.offset);
        } else {
            span = SourceSpan::empty(s.offset);
        }
    };
}

namespace {

// Recursive synth-text builder. Tokenizes a file to FIND quote includes,
// splices the recursively-preprocessed header text in place of each quote
// include directive, and copies everything else (including angle includes)
// VERBATIM with a 1:1 line-map segment.
struct SynthBuilder {
    std::shared_ptr<GrammarSchema const> schema;
    std::span<fs::path const>            includeDirs;
    DiagnosticReporter&                  rep;
    int                                  depth;
    std::vector<fs::path>&               includeStack;
    // Set TRUE when the include-nesting backstop fires (truncating the
    // splice). Shared by reference across the recursive child builders so
    // a deep-nest truncation at any level reaches `preprocess()`.
    bool&                                fatal;

    PreprocessConfig const& cfg() const { return schema->preprocess(); }

    std::optional<fs::path> resolveQuote(std::string_view filename,
                                         fs::path const& includingDir) const {
        // An empty include target names nothing -- fail loud (the caller
        // emits P_PreprocessorIncludeError). Only a REGULAR file is a valid
        // include target; `is_regular_file` excludes a directory (so
        // `#include ""` cannot resolve to the including dir itself, which
        // would later throw in SourceBuffer::fromFile).
        if (filename.empty()) return std::nullopt;
        fs::path const rel{filename};
        std::error_code ec;
        if (rel.is_absolute()) {
            return fs::is_regular_file(rel, ec) ? std::optional<fs::path>{rel}
                                                : std::nullopt;
        }
        if (!includingDir.empty()) {
            if (auto c = includingDir / rel; fs::is_regular_file(c, ec)) return c;
        }
        for (fs::path const& dir : includeDirs) {
            if (auto c = dir / rel; fs::is_regular_file(c, ec)) return c;
        }
        return std::nullopt;
    }

    void copyVerbatim(std::string const& spliced, LineMap const& localMap,
                      std::size_t from, std::size_t to,
                      std::string& out, LineMap& map) {
        if (to <= from) return;
        for (auto const& lseg : localMap.segments()) {
            if (lseg.synthEnd <= from) continue;
            if (lseg.synthStart >= to) break;
            const std::size_t a = std::max<std::size_t>(lseg.synthStart, from);
            const std::size_t b = std::min<std::size_t>(lseg.synthEnd, to);
            if (b <= a) continue;
            LineMapSegment seg;
            seg.synthStart  = static_cast<ByteOffset>(out.size());
            out.append(spliced, a, b - a);
            seg.synthEnd    = static_cast<ByteOffset>(out.size());
            seg.origin      = lseg.origin;
            seg.originStart = static_cast<ByteOffset>(
                lseg.originStart + (a - lseg.synthStart));
            map.addSegment(std::move(seg));
        }
    }

    void build(std::shared_ptr<SourceBuffer> const& source,
               std::string& out, LineMap& map) {
        const char dquote  = '"';
        const char newline = '\n';
        if (depth > kMaxIncludeDepth) {
            emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError, BufferId{},
                   SourceSpan::empty(0),
                   std::string{"include nesting too deep (possible cycle): "}
                       + std::string{source->name()});
            fatal = true;   // splice truncated — PP fatal
            return;
        }
        std::string spliced;
        LineMap     localMap;
        appendWithContinuationSplice(source->text(), source, 0, spliced,
                                     localMap);

        auto scanBuf =
            SourceBuffer::fromString(spliced, std::string{source->name()});
        DiagnosticReporter scratch;
        auto toks = tokenizeToPP(scanBuf, schema, scratch);

        const auto hashKind =
            schema->schemaTokens().find(cfg().directiveIntroToken);
        const auto quoteKind =
            schema->schemaTokens().find(cfg().quoteIncludeToken);

        std::size_t copiedUpTo = 0;
        fs::path const includingDir = fs::path{source->name()}.parent_path();

        auto isHash = [&](Token const& t) {
            return hashKind.valid() && t.schemaKind == hashKind;
        };

        for (std::size_t i = 0; i < toks.size(); ++i) {
            if (!isHash(toks[i].tok)) continue;
            std::size_t j = i + 1;
            while (j < toks.size() && isTrivia(toks[j].tok)) ++j;
            if (j >= toks.size()) break;
            if (toks[j].text != cfg().includeDirective) continue;
            std::size_t k = j + 1;
            while (k < toks.size() && isTrivia(toks[k].tok)) ++k;
            if (k >= toks.size()) continue;
            const bool isQuote =
                quoteKind.valid() && toks[k].tok.schemaKind == quoteKind;
            if (!isQuote) continue;

            // The quote opener (StringStart) consumed only the opening quote;
            // the coalesced string BODY is the very next token, whose text is
            // the raw path bytes between the quotes. The tokenizer emits a
            // coalesced body with core kind Operator (its schema kind is the
            // string-body kind), so we key on POSITION (next token) rather
            // than core kind. An empty body (`#include ""`) leaves filename
            // empty -> resolveQuote fails loud below.
            const std::size_t bodyIdx = k + 1;
            std::string filename;
            const ByteOffset dirStart = toks[i].tok.span.start();
            ByteOffset dirEnd = toks[k].tok.span.end();
            if (bodyIdx < toks.size() && !isTrivia(toks[bodyIdx].tok)
                && !isNewline(toks[bodyIdx].tok)
                && toks[bodyIdx].tok.span.start() == toks[k].tok.span.end()) {
                filename = std::string{toks[bodyIdx].text};
                dirEnd   = toks[bodyIdx].tok.span.end();
            }
            // Consume the closing quote char if it physically follows.
            if (dirEnd < spliced.size() && spliced[dirEnd] == dquote) ++dirEnd;

            auto resolved = resolveQuote(filename, includingDir);
            if (!resolved) {
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"quote include not found: "} + filename);
                continue;
            }
            std::error_code ec;
            fs::path canon = fs::weakly_canonical(*resolved, ec);
            if (ec) canon = *resolved;
            if (std::find(includeStack.begin(), includeStack.end(), canon)
                != includeStack.end()) {
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"circular include of "} + filename);
                continue;
            }
            auto headerBuf = SourceBuffer::fromFile(*resolved);
            if (!headerBuf) {
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"quote include unreadable: "} + filename);
                continue;
            }

            copyVerbatim(spliced, localMap, copiedUpTo, dirStart, out, map);

            includeStack.push_back(canon);
            SynthBuilder child{schema, includeDirs, rep, depth + 1,
                               includeStack, fatal};
            child.build(headerBuf, out, map);
            includeStack.pop_back();

            out.push_back(newline);
            copiedUpTo = dirEnd;
        }
        copyVerbatim(spliced, localMap, copiedUpTo, spliced.size(), out, map);
    }
};

// FC13 cycle 4 (D-PP-MACRO-HIDESET-PRECISE): the precise per-token HIDE SET
// (Prosser's realization of C 6.10.3.4). `Token` is a 16B trivially-copyable POD
// that cannot itself carry a set, so the macro-expansion WORKING set is a wrapper
// pairing each token with the set of macro names that must NOT be expanded for
// THAT token. The hide set PERSISTS through the produced stream (per-token),
// rather than being scoped to the recursion that produced it -- which is exactly
// what lets a function-like name and its `(` re-pair when they become adjacent
// only ACROSS the boundary between a just-expanded replacement and the
// surrounding parent stream (`A(F)(3)`, `NAME(4)`).
//
// Representation: a `shared_ptr<const set<string>>`. The set is IMMUTABLE once
// built, so copies are a refcount bump (every token in one replacement shares
// the SAME set object); union/intersect allocate a fresh set only when they
// actually change the contents. A null pointer is the canonical EMPTY hide set
// (the common case -- most tokens are hidden by nothing), so the parser-bound
// body lifts in with `nullptr` and pays no allocation. `std::set` (ordered)
// makes the hot intersection/union a linear merge.
using HideSet = std::shared_ptr<std::set<std::string> const>;

struct ExpToken {
    Token   tok;
    HideSet hide;  // null == empty
};

// M is hidden for this token iff M is a member of its hide set.
inline bool hideContains(HideSet const& hs, std::string const& name) {
    return hs && hs->count(name) != 0;
}

// hs ∪ {name}. Returns a set that CONTAINS every element of `hs` plus `name`.
// Reuses `hs` unchanged when `name` is already present (no allocation).
inline HideSet hideAdd(HideSet const& hs, std::string const& name) {
    if (hideContains(hs, name)) return hs;
    auto next = std::make_shared<std::set<std::string>>();
    if (hs) *next = *hs;
    next->insert(name);
    return next;
}

// a ∩ b. The Prosser function-like rule intersects the macro NAME's hide set
// with the CLOSING paren's before adding the invoked macro. An empty operand
// yields the empty set (null). Reuses an operand verbatim when it is a subset of
// the other (a very common case: the close paren came from the same stream as
// the name, so one hide set contains the other) to avoid an allocation.
inline HideSet hideIntersect(HideSet const& a, HideSet const& b) {
    if (!a || a->empty() || !b || b->empty()) return nullptr;
    auto out = std::make_shared<std::set<std::string>>();
    std::set_intersection(a->begin(), a->end(), b->begin(), b->end(),
                          std::inserter(*out, out->end()));
    if (out->empty()) return nullptr;
    return out;
}

struct MacroDef {
    std::vector<Token>       replacement;
    std::string              text;
    // FC13 cycle 2 (D-PP-FUNCTION-LIKE-MACRO): function-like macros. An
    // object-like macro keeps isFunctionLike=false + an empty params; a
    // function-like one records its parameter NAMES in declared order (used to
    // map a call's argument list onto the replacement). C11/C23 6.10.3p4: a
    // redefinition must agree on BOTH the kind (object vs function-like) AND
    // the parameter spelling, not just the replacement text.
    bool                     isFunctionLike = false;
    std::vector<std::string> params;
    // FC13 cycle 3 (D-PP-VARIADIC-MACRO): a VARIADIC function-like macro
    // (`#define V(a, ...) ...` or `#define V(...) ...`). `params` still holds the
    // NAMED parameters declared BEFORE the `...`; `isVariadic` marks that a
    // trailing `...` catch-all is present, so an invocation binds the first
    // `params.size()` arguments to the named params and gathers the REST (which
    // may be empty, C23) into the configured `variadicArgsName` (`__VA_ARGS__`)
    // catch-all. A non-variadic macro keeps isVariadic=false; `__VA_ARGS__` in
    // its replacement is then a constraint violation (fail loud).
    bool                     isVariadic = false;
};

class MacroExpander {
public:
    // `synth` is the PREFIX buffer (the synthesized text BEFORE any `#`/`##`
    // product is appended); `prefixLen` is its byte length. FC15a (A2): a `#`/`##`
    // product's spelling is accumulated into `productText_` and a product token's
    // span points at `[prefixLen + offsetInProductText, ...)`. After `run()`,
    // `preprocess()` appends `productText()` to the synth text and freezes the
    // FINAL buffer (prefix unchanged + products in the appended tail) -- so every
    // token (original prefix span OR product tail span) slices to its real text in
    // that ONE final buffer, exactly what the parser parses.
    MacroExpander(std::shared_ptr<SourceBuffer> synth,
                  std::shared_ptr<GrammarSchema const> schema,
                  DiagnosticReporter& rep, ByteOffset prefixLen)
        : synth_(std::move(synth)), schema_(std::move(schema)), rep_(rep),
          prefixLen_(prefixLen) {
        hashKind_  = schema_->schemaTokens().find(cfg().directiveIntroToken);
        // FC15a: the STRINGIZE (`#`) and TOKEN-PASTE (`##`) operator kinds are
        // CONFIG lexemes (agnosticism), resolved from `stringizeToken` /
        // `pasteToken`. OPTIONAL: an empty config field leaves the kind
        // InvalidSchemaToken, so `isStringize`/`isPaste` (`.valid()`-guarded)
        // never fire and the engine is a strict FC14 (no `#`/`##` handling).
        stringizeKind_ = schema_->schemaTokens().find(cfg().stringizeToken);
        pasteKind_     = schema_->schemaTokens().find(cfg().pasteToken);
        // The function-like-macro `(` is a CONFIG lexeme, not a hard-coded
        // name (agnosticism: a language whose paren token is named differently
        // would otherwise mis-classify `#define F(x)` as object-like). The
        // loader REQUIRES + validates `functionLikeOpenToken`, so this resolves
        // for any opt-in language; the `.valid()` guard at the use site is
        // defense-in-depth.
        parenOpen_ = schema_->schemaTokens().find(cfg().functionLikeOpenToken);
        // The function-like-macro `)` is ALSO a CONFIG lexeme (FC13 cycle 2):
        // it terminates the parameter-list parse AND balance-tracks a call's
        // argument list. `)` lexes as core `Punctuation` (not a distinct core
        // kind), so it must be resolved from config like the opener -- never
        // hard-coded. The loader REQUIRES + validates `functionLikeCloseToken`,
        // so this resolves for any opt-in language; the `.valid()` guard at the
        // use site is defense-in-depth.
        parenClose_ = schema_->schemaTokens().find(cfg().functionLikeCloseToken);
        // The argument/parameter SEPARATOR (C's `,`) is likewise a CONFIG lexeme
        // (FC13 cycle 2): a `,` lexes as core `Punctuation`, so it is resolved
        // from `functionLikeArgSeparatorToken` rather than a hard-coded name.
        argSep_ = schema_->schemaTokens().find(cfg().functionLikeArgSeparatorToken);
        // The VARIADIC marker (C's `...`) is ALSO a CONFIG lexeme (FC13 cycle 2
        // review fold): `parseParamList` detects `#define V(...)` by this token
        // KIND, never by the hard-coded `...` lexeme -- a second
        // preprocess-opting language whose variadic marker is spelled differently
        // is then parsed correctly. OPTIONAL: when the language declares none,
        // `variadicMarkerToken` is empty and this stays InvalidSchemaToken, so
        // the `.valid()` guard never treats any token as the marker.
        variadicMarker_ =
            schema_->schemaTokens().find(cfg().variadicMarkerToken);
    }

    // TRUE iff a fatal nesting-backstop truncated the expansion.
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

    // FC15a (A2): the accumulated `#`/`##` PRODUCT spellings, to be appended to
    // the synth text (AFTER `synth_`'s prefix) before the FINAL buffer is frozen.
    // Empty when no product was generated (then the final buffer == the prefix,
    // byte-identical to the FC14 behavior).
    [[nodiscard]] std::string const& productText() const noexcept {
        return productText_;
    }

    std::vector<Token> run(std::vector<Token> const& in) {
        std::vector<Token> body;
        std::size_t i = 0;
        while (i < in.size()) {
            if (isHash(in[i]) && firstOnLine(in, i)) {
                i = handleDirective(in, i, body);
                continue;
            }
            // FC14: a non-directive token is emitted to the body ONLY when every
            // enclosing conditional branch is active (C 6.10.1 conditional
            // elision). A dead-branch token is dropped here -- so elision
            // precedes `expand` naturally (the dead tokens never reach it).
            if (stackActive()) body.push_back(in[i]);
            ++i;
        }
        // FC14: an unterminated conditional (a `#if`/`#ifdef`/`#ifndef` with no
        // matching `#endif`) is a constraint violation (C 6.10p1) -- fail loud
        // rather than silently eliding the rest of the file.
        if (!condStack_.empty()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   SourceSpan::empty(
                       static_cast<ByteOffset>(synth_->size())),
                   "unterminated conditional directive (missing #endif)");
        }
        // Stream-expand the whole directive-stripped body uniformly (object-
        // AND function-like macros share one cursor-walking engine). Lift the
        // plain tokens into the ExpToken working set (every token starts with an
        // EMPTY hide set), run the precise per-token hide-set expander
        // (Prosser's realization of C 6.10.3.4 -- see `expand`), then DROP the
        // hide sets back to the plain Token stream the parser consumes.
        std::vector<ExpToken> work;
        work.reserve(body.size());
        for (Token const& t : body) work.push_back(ExpToken{t, nullptr});
        std::vector<ExpToken> expanded = expand(std::move(work), 0);
        std::vector<Token> out;
        out.reserve(expanded.size());
        for (ExpToken const& et : expanded) out.push_back(et.tok);
        return out;
    }

private:
    PreprocessConfig const& cfg() const { return schema_->preprocess(); }
    // Slice a token's lexeme. FC15a (A2): a token whose span begins at-or-after
    // the prefix length is a `#`/`##` PRODUCT -- its bytes live in `productText_`
    // (offset by `prefixLen_`), not yet in `synth_`. Every ORIGINAL token spans
    // the prefix (`< prefixLen_`) and slices `synth_` as before. (When no product
    // exists `productText_` is empty and this is byte-identical to the prior
    // single-slice form.)
    std::string_view text(Token const& t) const {
        if (t.span.start() >= prefixLen_) {
            const ByteOffset s = t.span.start() - prefixLen_;
            const ByteOffset e = t.span.end() - prefixLen_;
            if (e <= productText_.size()) {
                return std::string_view{productText_}.substr(s, e - s);
            }
            return {};   // defensive: never UB on a malformed product span
        }
        return synth_->slice(t.span);
    }
    bool isHash(Token const& t) const {
        return hashKind_.valid() && t.schemaKind == hashKind_;
    }
    // FC15a: the STRINGIZE (`#`) / TOKEN-PASTE (`##`) operators, by config kind.
    bool isStringize(Token const& t) const {
        return stringizeKind_.valid() && t.schemaKind == stringizeKind_;
    }
    bool isPaste(Token const& t) const {
        return pasteKind_.valid() && t.schemaKind == pasteKind_;
    }
    // A macro NAME / invocation is an identifier-like word. The tokenizer
    // leaves a plain word's schemaKind == InvalidSchemaToken (the PARSER
    // later promotes Word -> Identifier), so the PP keys on the universal
    // `Word` core kind rather than the Identifier schema id. Keywords are
    // also Word-kind but carry a valid schemaKind; a keyword is never in the
    // macro table (a `#define int ...` is the author's error), so matching
    // table membership by text keeps expansion correct.
    static bool isWord(Token const& t) {
        return t.coreKind == CoreTokenKind::Word;
    }

    bool firstOnLine(std::vector<Token> const& in, std::size_t idx) const {
        for (std::size_t p = idx; p-- > 0;) {
            if (isNewline(in[p])) return true;
            if (!isTrivia(in[p])) return false;
        }
        return true;
    }

    static std::size_t skipTrivia(std::vector<Token> const& in, std::size_t p) {
        while (p < in.size() && isTrivia(in[p]) && !isNewline(in[p])) ++p;
        return p;
    }
    static std::size_t lineEnd(std::vector<Token> const& in, std::size_t p) {
        while (p < in.size() && !isNewline(in[p])) ++p;
        if (p < in.size()) ++p;
        return p;
    }
    std::size_t handleDirective(std::vector<Token> const& in, std::size_t start,
                                std::vector<Token>& body) {
        const std::size_t end = lineEnd(in, start);
        std::size_t p = skipTrivia(in, start + 1);
        if (p >= end || isNewline(in[p])) return end;
        const std::string_view word = text(in[p]);

        // FC14 (MF-3): the SIX conditional-compilation directives are dispatched
        // UNCONDITIONALLY -- they must always update `condStack_` so nesting is
        // tracked correctly even inside a dead branch (an `#if` nested in an
        // elided `#if 0` still needs its matching `#endif` to balance). Their
        // operand is evaluated ONLY when the branch should be (handled inside).
        if (word == cfg().ifDirective) {
            handleIf(in, p + 1, end, /*kind=*/IfKind::Expr);
            return end;
        }
        if (word == cfg().ifdefDirective) {
            handleIf(in, p + 1, end, IfKind::Ifdef);
            return end;
        }
        if (word == cfg().ifndefDirective) {
            handleIf(in, p + 1, end, IfKind::Ifndef);
            return end;
        }
        if (word == cfg().elifDirective) {
            handleElif(in, p + 1, end);
            return end;
        }
        if (word == cfg().elseDirective) {
            handleElse(in[p].span);
            return end;
        }
        if (word == cfg().endifDirective) {
            handleEndif(in[p].span);
            return end;
        }

        // FC14 (MF-3): every NON-conditional directive -- AND its diagnostics --
        // is GATED on the conditional stack being active. Inside a dead branch
        // an unknown/malformed directive is NOT an error (C 6.10p1: a skipped
        // group's directives are only parsed enough to track nesting), so the
        // whole arm (including the unsupported-directive diagnostic) is skipped.
        if (!stackActive()) return end;

        if (word == cfg().defineDirective) {
            handleDefine(in, p + 1, end);
        } else if (word == cfg().undefDirective) {
            handleUndef(in, p + 1, end);
        } else if (word == cfg().includeDirective) {
            // NAMED EXCLUSION (D-PP-CONDITIONAL-INCLUDE-ORDERING): a quote
            // `#include` inside a conditional was already spliced by
            // `SynthBuilder` BEFORE this macro pass ran, so `#if 0 #include
            // "x.h" #endif` already resolved x.h (its TEXT is then elided here,
            // but the splice side-effect / missing-file error already happened
            // upstream). The angle-include line is passed through to the
            // post-parse import resolver as before. Not a silent miscompile: a
            // missing dead-branch include errors LOUDLY at splice time.
            for (std::size_t q = start; q < end; ++q) body.push_back(in[q]);
        } else {
            emitPP(rep_, DiagnosticCode::P_PreprocessorUnsupported,
                   synth_->id(), in[p].span,
                   std::string{"unsupported preprocessor directive (out of "
                               "FC13 cycle-1 scope): "}
                       + std::string{word});
        }
        return end;
    }

    // FC14: which `#if`-family directive opened a frame.
    enum class IfKind { Expr, Ifdef, Ifndef };

    // The condition stack (C 6.10.1). Each open `#if`/`#ifdef`/`#ifndef` pushes
    // a frame; `#elif`/`#else` mutate the TOP; `#endif` pops. A token (or a
    // gated directive) is live iff EVERY frame's `thisBranchActive` is true.
    struct CondFrame {
        bool enclosingActive;   // was the stack active when this frame opened?
        bool anyBranchTaken;    // has any branch of this group been taken yet?
        bool thisBranchActive;  // is the CURRENT branch the live one?
        bool seenElse;          // has a `#else` been seen in this group?
    };

    // True iff every open conditional frame's current branch is active (empty
    // stack => active). The gate for token emission + non-conditional
    // directives.
    [[nodiscard]] bool stackActive() const {
        for (CondFrame const& f : condStack_) {
            if (!f.thisBranchActive) return false;
        }
        return true;
    }

    // True iff `name` is currently a defined macro (C's `defined X` / `#ifdef`).
    [[nodiscard]] bool isDefined(std::string_view name) const {
        return table_.find(std::string{name}) != table_.end();
    }

    // `#if EXPR` / `#ifdef NAME` / `#ifndef NAME`: push a new frame. The branch
    // is live iff the enclosing context is active AND the condition holds. The
    // operand/condition is evaluated ONLY when the enclosing context is active
    // (a dead branch's operand is NOT evaluated -- C 6.10.1p6).
    void handleIf(std::vector<Token> const& in, std::size_t p, std::size_t end,
                  IfKind kind) {
        bool const enclosing = stackActive();
        bool cond = false;
        if (enclosing) {
            if (kind == IfKind::Expr) {
                cond = evalIfOperand(in, p, end);
            } else {
                // `#ifdef`/`#ifndef NAME`: the operand is a single macro name.
                std::size_t q = skipTrivia(in, p);
                if (q >= end || isNewline(in[q]) || !isWord(in[q])) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(),
                           (q < end ? in[q].span : SourceSpan::empty(0)),
                           std::string{"#"}
                               + std::string{kind == IfKind::Ifdef ? "ifdef"
                                                                   : "ifndef"}
                               + " requires a macro name");
                    // Treat a malformed #ifdef as a false (inactive) branch, but
                    // STILL push a frame so the matching #endif balances.
                    cond = false;
                } else {
                    bool const def = isDefined(text(in[q]));
                    cond = (kind == IfKind::Ifdef) ? def : !def;
                }
            }
        }
        condStack_.push_back(CondFrame{
            /*enclosingActive=*/enclosing,
            /*anyBranchTaken=*/enclosing && cond,
            /*thisBranchActive=*/enclosing && cond,
            /*seenElse=*/false});
    }

    // `#elif EXPR`: on the TOP frame, take this branch iff the enclosing context
    // is active, NO prior branch of this group was taken, AND the expression
    // holds. The operand is evaluated ONLY when it could be taken (C 6.10.1p6).
    void handleElif(std::vector<Token> const& in, std::size_t p,
                    std::size_t end) {
        if (condStack_.empty()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   (p <= end && p > 0 ? in[p - 1].span : SourceSpan::empty(0)),
                   "#elif without a matching #if");
            return;
        }
        CondFrame& f = condStack_.back();
        if (f.seenElse) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   (p <= end && p > 0 ? in[p - 1].span : SourceSpan::empty(0)),
                   "#elif after #else");
            return;
        }
        // A prior active branch latches `anyBranchTaken`.
        f.anyBranchTaken = f.anyBranchTaken || f.thisBranchActive;
        bool const mayTake = f.enclosingActive && !f.anyBranchTaken;
        bool cond = false;
        if (mayTake) cond = evalIfOperand(in, p, end);
        f.thisBranchActive = mayTake && cond;
        f.anyBranchTaken   = f.anyBranchTaken || f.thisBranchActive;
    }

    // `#else`: take this branch iff the enclosing context is active and no prior
    // branch of this group was taken.
    void handleElse(SourceSpan at) {
        if (condStack_.empty()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   at, "#else without a matching #if");
            return;
        }
        CondFrame& f = condStack_.back();
        if (f.seenElse) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   at, "#else after #else");
            return;
        }
        f.anyBranchTaken   = f.anyBranchTaken || f.thisBranchActive;
        f.seenElse         = true;
        f.thisBranchActive = f.enclosingActive && !f.anyBranchTaken;
        f.anyBranchTaken   = true;
    }

    // `#endif`: pop the top frame.
    void handleEndif(SourceSpan at) {
        if (condStack_.empty()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   at, "#endif without a matching #if");
            return;
        }
        condStack_.pop_back();
    }

    // Evaluate an `#if`/`#elif` controlling expression: slice the operand tokens
    // (from `p` to the line's newline), then delegate to the shared ICE
    // evaluator (`pp_if_eval`), which reuses the const-eval arithmetic core +
    // the existing macro expander (via the callbacks below). Returns the
    // BRANCH-TAKEN boolean (the evaluator already emitted any fail-loud
    // diagnostic; a nullopt -> false, the branch is not taken).
    [[nodiscard]] bool evalIfOperand(std::vector<Token> const& in,
                                     std::size_t p, std::size_t end) {
        // The operand runs from `p` up to (but not including) the trailing
        // newline that `lineEnd` consumed.
        std::size_t last = end;
        while (last > p && isNewline(in[last - 1])) --last;
        std::vector<Token> operand(in.begin() + static_cast<std::ptrdiff_t>(p),
                                   in.begin() + static_cast<std::ptrdiff_t>(last));
        PpMacroExpand expandCb =
            [this](std::vector<Token> const& toks) { return expandTokens(toks); };
        PpIsDefined definedCb =
            [this](std::string_view n) { return isDefined(n); };
        auto v = evaluateIfExpression(operand, *schema_, expandCb, definedCb,
                                      *synth_, rep_);
        return v.has_value() && *v != 0;
    }

    // Macro-expand a token run with the SAME engine `run()` uses (object +
    // function-like, hide-set-precise): lift into the ExpToken working set,
    // expand, drop the hide sets. Used by the `#if` evaluator's callback so the
    // controlling expression's macros expand identically to the body's.
    std::vector<Token> expandTokens(std::vector<Token> const& toks) {
        std::vector<ExpToken> work;
        work.reserve(toks.size());
        for (Token const& t : toks) work.push_back(ExpToken{t, nullptr});
        std::vector<ExpToken> expanded = expand(std::move(work), 0);
        std::vector<Token> out;
        out.reserve(expanded.size());
        for (ExpToken const& et : expanded) out.push_back(et.tok);
        return out;
    }

    bool isParenOpen(Token const& t) const {
        return parenOpen_.valid() && t.schemaKind == parenOpen_;
    }
    bool isParenClose(Token const& t) const {
        return parenClose_.valid() && t.schemaKind == parenClose_;
    }
    bool isArgSeparator(Token const& t) const {
        return argSep_.valid() && t.schemaKind == argSep_;
    }
    bool isVariadicMarker(Token const& t) const {
        return variadicMarker_.valid() && t.schemaKind == variadicMarker_;
    }

    void handleDefine(std::vector<Token> const& in, std::size_t p,
                      std::size_t end) {
        const char space = static_cast<char>(0x20);
        p = skipTrivia(in, p);
        if (p >= end || isNewline(in[p]) || !isWord(in[p])) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   (p < end ? in[p].span : SourceSpan::empty(0)),
                   "#define requires a macro name");
            return;
        }
        const std::string name{text(in[p])};
        const std::size_t nameIdx = p;
        ++p;

        MacroDef def;
        // FUNCTION-like iff the configured open-paren is IMMEDIATELY ADJACENT
        // to the macro name (C 6.10.3p3: no white space between the name and
        // the `(`). `#define F (x)` -- a space before `(` -- is OBJECT-like
        // (the `(x)` is part of the replacement list). We test adjacency on the
        // raw spans (NO skipTrivia) so a single intervening space disqualifies.
        if (p < end && isParenOpen(in[p])
            && in[p].span.start() == in[nameIdx].span.end()) {
            def.isFunctionLike = true;
            if (!parseParamList(in, p, end, name, def.params, def.isVariadic)) {
                return;  // parseParamList already emitted a fail-loud diagnostic
            }
            // After the parameter list, `p` indexes the token just past `)`;
            // the rest of the line is the replacement list (collected below).
        }

        std::string repText;
        for (std::size_t q = skipTrivia(in, p); q < end;
             q = skipTrivia(in, q + 1)) {
            if (isNewline(in[q])) break;
            def.replacement.push_back(in[q]);
            if (!repText.empty()) repText.push_back(space);
            repText.append(text(in[q]));
        }
        def.text = std::move(repText);

        // The variadic catch-all identifier (`__VA_ARGS__`) is valid ONLY inside
        // a VARIADIC macro's replacement (C 6.10.3p5 / 6.10.3.1p2 constraint:
        // the identifier `__VA_ARGS__` shall occur only in the replacement-list
        // of a function-like macro that uses the ellipsis notation). Reject it
        // HERE, at definition time, in an object-like OR a non-variadic
        // function-like macro -- catching the misuse where it is DECLARED rather
        // than waiting for a (possibly absent) invocation. Matched by TEXT (it is
        // an ordinary identifier), and only when the language actually declares a
        // catch-all spelling (`variadicArgsName` non-empty).
        if (!def.isVariadic && !cfg().variadicArgsName.empty()) {
            for (Token const& r : def.replacement) {
                if (isWord(r) && text(r) == cfg().variadicArgsName) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(), r.span,
                           std::string{"'"} + cfg().variadicArgsName
                               + "' may appear only in a variadic macro's "
                                 "replacement: " + name);
                    return;
                }
            }
        }

        auto it = table_.find(name);
        if (it != table_.end() && !sameDefinition(it->second, def)) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorMacroRedefinition,
                   synth_->id(), in[nameIdx].span,
                   std::string{"incompatible redefinition of macro: "} + name);
            return;
        }
        table_[name] = std::move(def);
    }

    // Two `#define`s of the same name are COMPATIBLE (C 6.10.3p1/p2) only when
    // they agree on EVERY axis: object-vs-function-like, the parameter spelling
    // (in order), AND the replacement-token spelling. We compare the
    // whitespace-normalized replacement TEXT (the cycle-1 contract) plus the
    // kind + parameter names. A mismatch on any axis is an incompatible
    // redefinition (fail-loud at the call site).
    static bool sameDefinition(MacroDef const& a, MacroDef const& b) {
        return a.isFunctionLike == b.isFunctionLike
            && a.isVariadic == b.isVariadic
            && a.params == b.params
            && a.text == b.text;
    }

    // Parse a function-like macro's parameter list, starting at `open` (the
    // index of the adjacent `(`). On success fills `out` with the NAMED
    // parameter names in order, sets `isVariadic` iff a trailing `...` catch-all
    // is present, advances `open` PAST the closing `)`, and returns true. On any
    // malformed input emits a fail-loud diagnostic and returns false.
    // Grammar (C 6.10.3, FC13 cycle 3 -- plain variadic, no `#`/`##`):
    //   ( )                        -> zero parameters
    //   ( id ( , id )* )           -> named parameters, comma-separated
    //   ( ... )                    -> zero named + a variadic catch-all
    //   ( id ( , id )* , ... )     -> named parameters + a variadic catch-all
    // The `...` (when the language declares one) must be the LAST element before
    // `)`; it is accepted (sets isVariadic), NOT a fail-loud as in cycle 2.
    // FAIL-LOUD on: a `...` that is NOT last (`(a, ..., b)` / a token after the
    // `...` other than `)`), a duplicate parameter name, a non-identifier where a
    // parameter is expected, a missing comma between parameters, or no closing
    // `)` before line end.
    bool parseParamList(std::vector<Token> const& in, std::size_t& open,
                        std::size_t end, std::string const& macroName,
                        std::vector<std::string>& out, bool& isVariadic) {
        std::size_t q = skipTrivia(in, open + 1);  // first token after `(`
        // Empty list `()` -> zero parameters.
        if (q < end && isParenClose(in[q])) {
            open = q + 1;
            return true;
        }
        while (true) {
            if (q >= end || isNewline(in[q])) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(),
                       (q < end ? in[q].span : SourceSpan::empty(0)),
                       std::string{"unterminated macro parameter list: "}
                           + macroName);
                return false;
            }
            // A variadic marker (C's `...`) in parameter position makes this a
            // VARIADIC macro (C 6.10.3p4). Detected by the CONFIGURED token KIND
            // (`variadicMarkerToken`), NOT the hard-coded `...` lexeme: a second
            // preprocess-opting language whose variadic marker is spelled
            // differently is parsed correctly (the `.valid()` guard means a
            // language declaring no variadic form never matches here -- the
            // marker then falls through to the not-a-Word fail-loud below). The
            // `...` must be LAST: the only thing allowed after it is the closing
            // `)`. We reach this arm at the START of the list (`(...)`) and after
            // each comma (`(a, ...)`), so requiring `)` next rejects the mid-list
            // form `(a, ..., b)` and a stray token after `...`.
            if (isVariadicMarker(in[q])) {
                std::size_t r = skipTrivia(in, q + 1);
                if (r < end && isParenClose(in[r])) {
                    isVariadic = true;
                    open = r + 1;
                    return true;
                }
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(),
                       (r < end ? in[r].span : in[q].span),
                       std::string{"variadic '...' must be the last element of "
                                   "the macro parameter list: "}
                           + macroName);
                return false;
            }
            if (!isWord(in[q])) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[q].span,
                       std::string{"expected a parameter name in macro "
                                   "parameter list: "}
                           + macroName);
                return false;
            }
            std::string param{text(in[q])};
            // C 6.10.3p6: the configured catch-all identifier (`__VA_ARGS__`)
            // shall NOT be used as a parameter NAME. Reject it loudly so the
            // substitute() invariant ("`__VA_ARGS__` is not a valid parameter
            // name") actually holds -- otherwise `#define F(__VA_ARGS__) ...`
            // silently binds a parameter the variadic catch-all later shadows.
            if (!cfg().variadicArgsName.empty()
                && param == cfg().variadicArgsName) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[q].span,
                       std::string{"'"} + cfg().variadicArgsName
                           + "' may not be used as a macro parameter name: "
                           + macroName);
                return false;
            }
            for (std::string const& seen : out) {
                if (seen == param) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(), in[q].span,
                           std::string{"duplicate macro parameter '"} + param
                               + "' in macro: " + macroName);
                    return false;
                }
            }
            out.push_back(std::move(param));
            q = skipTrivia(in, q + 1);  // token after the parameter name
            if (q < end && isParenClose(in[q])) {
                open = q + 1;
                return true;
            }
            if (q >= end || isNewline(in[q]) || !isArgSeparator(in[q])) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(),
                       (q < end ? in[q].span : SourceSpan::empty(0)),
                       std::string{"expected ',' or ')' in macro parameter "
                                   "list: "}
                           + macroName);
                return false;
            }
            q = skipTrivia(in, q + 1);  // token after the comma -> next param
        }
    }

    void handleUndef(std::vector<Token> const& in, std::size_t p,
                     std::size_t end) {
        p = skipTrivia(in, p);
        if (p >= end || isNewline(in[p]) || !isWord(in[p])) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   (p < end ? in[p].span : SourceSpan::empty(0)),
                   "#undef requires a macro name");
            return;
        }
        table_.erase(std::string{text(in[p])});
    }

    // Index of the next non-trivia / non-newline token from the cursor. A
    // function-like invocation lookahead PEEKS past intervening whitespace AND
    // newlines (`FOO\n(1)` is a valid call: once the directive line itself is
    // stripped, C 6.10.3p10/p11 treat the name and the `(` as adjacent across
    // white space, including line breaks). Returns in.size() if none.
    static std::size_t nextSignificant(std::span<ExpToken const> in,
                                       std::size_t from) {
        std::size_t j = from;
        while (j < in.size()
               && (isTrivia(in[j].tok) || isNewline(in[j].tok))) ++j;
        return j;
    }

    // Trim leading + trailing whitespace-trivia (incl. newlines/comments) from
    // an argument token run, in place. Interior trivia is PRESERVED (an
    // argument may legitimately contain spaces, e.g. `a + b`).
    static void trimArgTrivia(std::vector<ExpToken>& arg) {
        std::size_t a = 0, b = arg.size();
        while (a < b && (isTrivia(arg[a].tok) || isNewline(arg[a].tok))) ++a;
        while (b > a
               && (isTrivia(arg[b - 1].tok) || isNewline(arg[b - 1].tok))) --b;
        if (a != 0 || b != arg.size()) {
            arg = std::vector<ExpToken>(
                arg.begin() + static_cast<std::ptrdiff_t>(a),
                arg.begin() + static_cast<std::ptrdiff_t>(b));
        }
    }

    // Collect a function-like macro call argument list. `in[open]` is the
    // invocation `(`. Scans tracking PAREN DEPTH (only the configured open/close
    // paren affect depth): a depth-1 comma SEPARATES arguments; the matching
    // depth-0 close ENDS the list. Each argument preserves its interior tokens
    // (nested parens/commas survive) AND their hide sets; leading/trailing trivia
    // is trimmed. By the comma-separated-groups rule, `(x)` is ONE argument and
    // `()` is ONE EMPTY argument; the special zero-PARAMETER case (`M()` for a
    // 0-arg macro = zero arguments, C 6.10.3p4) is normalized by the CALLER (which
    // knows params.size()). The depth-1 SEPARATOR tokens are also recorded into
    // `separators` (one per top-level comma -> `args.size()-1` entries): a
    // VARIADIC macro re-joins the trailing arguments with the ORIGINAL separator
    // tokens to form `__VA_ARGS__` (preserving the source commas, C 6.10.3p4),
    // rather than synthesizing one. On success returns the arguments, sets `past`
    // to the index JUST PAST the matching close, and copies the CLOSING paren's
    // hide set into `closeHide` (the Prosser function-like rule intersects the
    // macro NAME's hide set with the CLOSE paren's). On EOF before the matching
    // close, emits a fail-loud diagnostic and returns std::nullopt.
    std::optional<std::vector<std::vector<ExpToken>>>
    collectArgs(std::span<ExpToken const> in, std::size_t open,
                std::string const& macroName, std::size_t& past,
                std::vector<ExpToken>& separators, HideSet& closeHide) {
        std::vector<std::vector<ExpToken>> args;
        std::vector<ExpToken>              cur;
        int depth = 1;                 // start just inside the opening `(`
        std::size_t j = open + 1;
        for (; j < in.size(); ++j) {
            ExpToken const& t = in[j];
            if (isParenOpen(t.tok)) {
                ++depth;
                cur.push_back(t);
                continue;
            }
            if (isParenClose(t.tok)) {
                --depth;
                if (depth == 0) {
                    // End of list: flush the final (possibly empty) group and
                    // surface the close paren's hide set for the Prosser rule.
                    trimArgTrivia(cur);
                    args.push_back(std::move(cur));
                    past      = j + 1;
                    closeHide = t.hide;
                    return args;
                }
                cur.push_back(t);
                continue;
            }
            if (depth == 1 && isArgSeparator(t.tok)) {
                // Top-level separator: close current argument, start the next.
                // Record the separator token verbatim (the variadic catch-all
                // re-joins trailing args with these ORIGINAL commas).
                trimArgTrivia(cur);
                args.push_back(std::move(cur));
                cur.clear();
                separators.push_back(t);
                continue;
            }
            cur.push_back(t);
        }
        // Ran off the end without the matching close -> unterminated.
        emitPP(rep_, DiagnosticCode::P_PreprocessorMacroArgument, synth_->id(),
               (open < in.size() ? in[open].tok.span : SourceSpan::empty(0)),
               std::string{"unterminated argument list for function-like "
                           "macro: "}
                   + macroName);
        return std::nullopt;
    }

    // Map a replacement token that names a NAMED parameter to its parameter
    // index, or -1. (`def.replacement` carries only significant tokens -- trivia
    // was dropped at #define time -- so a parameter is always a bare `Word`.)
    [[nodiscard]] int paramIndexOf(Token const& r, MacroDef const& def) const {
        if (!isWord(r)) return -1;
        std::string_view rt = text(r);
        for (std::size_t k = 0; k < def.params.size(); ++k) {
            if (def.params[k] == rt) return static_cast<int>(k);
        }
        return -1;
    }
    [[nodiscard]] bool isVaArgsName(Token const& r, MacroDef const& def) const {
        return def.isVariadic && isWord(r) && !cfg().variadicArgsName.empty()
            && text(r) == cfg().variadicArgsName;
    }

    // Build the substituted replacement list for a function-like call (C 6.10.3).
    // A replacement token that names a parameter (or `__VA_ARGS__`) is replaced by
    // that argument's tokens; every other token passes through stamped with `hs`.
    // FC15a adds the `#` (stringize, C 6.10.3.2) and `##` (token-paste, C 6.10.3.3)
    // operators in TWO phases:
    //   PHASE A -- substitution. A normal parameter substitutes its PRE-EXPANDED
    //   argument (`expandedArgs[k]` / `vaArgs`, C 6.10.3.1). A `#` immediately
    //   followed by a parameter is replaced by ONE string-literal product
    //   (`stringizeArg`, F2) built from that parameter's RAW argument
    //   (`rawArgs[k]` / `rawVaArgs`). A parameter that is an OPERAND of a `##`
    //   (its adjacent significant replacement token is a `##`) substitutes its RAW
    //   argument (C 6.10.3.1: `#`/`##` operands are NOT pre-expanded). `##` tokens
    //   are kept verbatim as MARKERS for phase B.
    //   PHASE B -- paste. Each `##` MARKER is collapsed LEFT-TO-RIGHT: the last
    //   significant token to its left and the first to its right are concatenated
    //   into a single re-tokenized product (`pasteTokens`, F1), then a rescan
    //   continues from that product (so `a##b##c` chains).
    // Fail-loud (each with best-effort recovery): a `#` not followed by a
    // parameter -> P_PreprocessorStringize; a `##` at the start/end of the list,
    // or a paste whose spelling is not a single token -> P_PreprocessorPaste.
    //
    // HIDE-SET stamping (Prosser, C 6.10.3.4): EVERY token of the substituted
    // result carries `hs` = (hideset(name) ∩ hideset(close-paren)) ∪ {M}. A
    // replacement-origin token (plain, or a `#`/`##` product) is stamped with
    // exactly `hs`; an argument token already carries its own (accreted) hide set,
    // UNIONED with `hs`.
    std::vector<ExpToken> substitute(
        MacroDef const& def,
        std::vector<std::vector<ExpToken>> const& expandedArgs,
        std::vector<ExpToken> const& vaArgs,
        std::vector<std::vector<ExpToken>> const& rawArgs,
        std::vector<ExpToken> const& rawVaArgs,
        HideSet const& hs) {
        auto stampArg = [&](std::vector<ExpToken> const& a,
                            std::vector<ExpToken>& outTokens) {
            for (ExpToken const& e : a) {
                outTokens.push_back(ExpToken{e.tok, hideUnionAll(e.hide, hs)});
            }
        };
        // The RAW token run for a `#`/`##` operand at replacement index `i`
        // (a named parameter or `__VA_ARGS__`). Returns nullptr if `i` is not a
        // parameter position.
        auto rawArgAt =
            [&](std::size_t i) -> std::vector<ExpToken> const* {
            Token const& r = def.replacement[i];
            if (isVaArgsName(r, def)) return &rawVaArgs;
            int const pi = paramIndexOf(r, def);
            if (pi >= 0) return &rawArgs[static_cast<std::size_t>(pi)];
            return nullptr;
        };

        // ── PHASE A: substitution (keeping `##` markers verbatim). ──
        std::vector<ExpToken> items;
        const std::size_t n = def.replacement.size();
        for (std::size_t i = 0; i < n; ++i) {
            Token const& r = def.replacement[i];
            if (isStringize(r)) {
                // `#` operand is the NEXT significant token, which MUST be a
                // parameter (or `__VA_ARGS__`).
                if (i + 1 < n) {
                    if (auto const* raw = rawArgAt(i + 1)) {
                        stringizeArg(*raw, hs, items);
                        ++i;   // consume the parameter operand
                        continue;
                    }
                }
                emitPP(rep_, DiagnosticCode::P_PreprocessorStringize, synth_->id(),
                       r.span,
                       "'#' in a macro replacement must be followed by a "
                       "parameter");
                items.push_back(ExpToken{r, hs});   // recovery: emit `#` verbatim
                continue;
            }
            if (isPaste(r)) {
                items.push_back(ExpToken{r, hs});    // marker for phase B
                continue;
            }
            if (isVaArgsName(r, def)) {
                // RAW iff this `__VA_ARGS__` is a `##` operand (adjacent `##`).
                bool const pasteOperand =
                    (i > 0 && isPaste(def.replacement[i - 1]))
                    || (i + 1 < n && isPaste(def.replacement[i + 1]));
                stampArg(pasteOperand ? rawVaArgs : vaArgs, items);
                continue;
            }
            int const pi = paramIndexOf(r, def);
            if (pi >= 0) {
                bool const pasteOperand =
                    (i > 0 && isPaste(def.replacement[i - 1]))
                    || (i + 1 < n && isPaste(def.replacement[i + 1]));
                stampArg(pasteOperand
                             ? rawArgs[static_cast<std::size_t>(pi)]
                             : expandedArgs[static_cast<std::size_t>(pi)],
                         items);
                continue;
            }
            // A plain replacement token gets EXACTLY hs (no prior hide set).
            items.push_back(ExpToken{r, hs});
        }

        // ── PHASE B: collapse every `##` marker LEFT-TO-RIGHT. ──
        return collapsePastes(std::move(items), hs);
    }

    // Phase B of `substitute`: walk `items`, and at each `##` MARKER concatenate
    // the token immediately before it with the one immediately after into a
    // single re-tokenized product (F1), splicing `[i-1, i, i+1)` -> product and
    // RESCANNING from there (so `a##b##c` collapses left-to-right to one token).
    // A `##` at the very start or end of the list (no operand on one side) is a
    // constraint violation (C 6.10.3.3p1) -> P_PreprocessorPaste, recovery: drop
    // the dangling `##` and keep the lone operand. A product that is not exactly
    // one token (F1) -> P_PreprocessorPaste, recovery: emit both operands verbatim.
    std::vector<ExpToken> collapsePastes(std::vector<ExpToken> items,
                                         HideSet const& hs) {
        std::size_t i = 0;
        while (i < items.size()) {
            if (!isPaste(items[i].tok)) { ++i; continue; }
            SourceSpan const opSpan = items[i].tok.span;
            const bool hasLeft  = (i > 0);
            const bool hasRight = (i + 1 < items.size());
            if (!hasLeft || !hasRight) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorPaste, synth_->id(),
                       opSpan,
                       std::string{"'##' must not appear at the "}
                           + (!hasLeft ? "start" : "end")
                           + " of a macro replacement list");
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                // Resume at the operand that now occupies `i` (or end).
                continue;
            }
            // Concatenate the spellings of the two operands.
            std::string spelling{text(items[i - 1].tok)};
            spelling += text(items[i + 1].tok);
            auto product = pasteTokens(spelling, opSpan);
            if (!product) {
                // F1 failure (zero or >1 tokens) already reported; recover by
                // dropping the `##` and leaving BOTH operands verbatim. Advance
                // past the left operand so we don't re-paste it.
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                ++i;   // step past the (now adjacent) left operand
                continue;
            }
            // Replace [i-1, i, i+1) with the single product token (hide set hs --
            // a fresh replacement-origin token), then rescan from i-1 so a
            // chained `##` to its right pastes against this product.
            const std::size_t lo = i - 1;
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(lo),
                        items.begin() + static_cast<std::ptrdiff_t>(i + 2));
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(lo),
                         ExpToken{*product, hs});
            i = lo;   // rescan from the product
        }
        return items;
    }

    // FC15a (F2, C 6.10.3.2): STRINGIZE the RAW argument token run `raw` into a
    // string-literal product, appending the resulting token(s) to `out`. Per
    // C 6.10.3.2p2 the spelling is the argument's SOURCE text with: every run of
    // white space (incl. between tokens) collapsed to a single space and
    // leading/trailing space deleted; and a `\` inserted before each `"` and `\`
    // (the chars of a string/char literal -- in valid C those characters appear
    // ONLY inside such a literal, so escaping every occurrence is exact). The
    // result is wrapped in `"..."`, appended to `productText_` (A2), and
    // RE-TOKENIZED so the product is a real StringStart + StringLiteral pair
    // (a single fabricated token would not satisfy the grammar's
    // `stringLiteralExpr = StringStart StringLiteral`) whose spans point at the
    // appended region. Each product token is stamped with `hs`.
    void stringizeArg(std::vector<ExpToken> const& raw, HideSet const& hs,
                      std::vector<ExpToken>& out) {
        std::string inner = "\"";
        if (!raw.empty()) {
            // The raw operand's tokens are un-pre-expanded args from the CALL
            // site, so they are contiguous in the prefix buffer: one slice
            // recovers the exact source spelling (incl. interior string quotes
            // and whitespace). The CLOSING delimiter of a string/char literal
            // that ENDS the argument was consumed by the tokenizer (no token
            // covers it), so extend the slice by one byte when it is a `"`/`'`.
            const ByteOffset s = raw.front().tok.span.start();
            ByteOffset e = raw.back().tok.span.end();
            if (e < prefixLen_) {
                const std::string_view tail = synth_->slice(e, e + 1);
                if (tail.size() == 1 && (tail[0] == '"' || tail[0] == '\'')) ++e;
            }
            appendStringized(synth_->slice(s, e), inner);
        }
        inner.push_back('"');
        for (Token const& t : materializeSignificant(inner)) {
            out.push_back(ExpToken{t, hs});
        }
    }

    // Append `src` to `out` realizing C 6.10.3.2's stringize transform: collapse
    // each run of source white space to a single space and drop leading/trailing
    // space; insert a `\` before each `"` and `\`.
    static void appendStringized(std::string_view src, std::string& out) {
        std::size_t i = 0;
        const std::size_t nbytes = src.size();
        // Skip leading white space.
        auto isWs = [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r'
                || c == '\f' || c == '\v';
        };
        while (i < nbytes && isWs(src[i])) ++i;
        bool pendingSpace = false;
        for (; i < nbytes; ++i) {
            char const c = src[i];
            if (isWs(c)) { pendingSpace = true; continue; }
            if (pendingSpace) { out.push_back(' '); pendingSpace = false; }
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
    }

    // FC15a (A2 -- the load-bearing buffer mechanism): MATERIALIZE a `#`/`##`
    // product whose spelling is `spelling`. The spelling is APPENDED to
    // `productText_` (which `preprocess()` later concatenates onto the synth text
    // before freezing the FINAL buffer), then re-tokenized; each resulting
    // SIGNIFICANT token (non-trivia, non-Eof) is returned with its span REWRITTEN
    // to point at the appended region of the (eventual) final buffer
    // (`prefixLen_ + productBase + tokenOffset`). So a product token slices to its
    // real spelling -- `add3` / `"hello"` -- from the SAME buffer the parser
    // parses, never to `##`/`#`. The re-tokenization uses a throwaway reporter so
    // a malformed product does not pollute the user diagnostics here (the caller's
    // F1/F2 logic owns the user-facing fail-loud).
    std::vector<Token> materializeSignificant(std::string_view spelling) {
        const ByteOffset productBase =
            static_cast<ByteOffset>(productText_.size());
        productText_.append(spelling);
        auto tiny = SourceBuffer::fromString(std::string{spelling}, "<pp-product>");
        DiagnosticReporter scratch;
        auto ppToks = tokenizeToPP(tiny, schema_, scratch);
        std::vector<Token> out;
        for (PPToken const& pt : ppToks) {
            if (isTrivia(pt.tok) || isNewline(pt.tok)) continue;
            if (pt.tok.coreKind == CoreTokenKind::Eof) continue;
            Token t = pt.tok;
            // Rewrite the tiny-buffer span into the final-buffer product region.
            t.span = SourceSpan::of(
                prefixLen_ + productBase + pt.tok.span.start(),
                prefixLen_ + productBase + pt.tok.span.end());
            out.push_back(t);
        }
        return out;
    }

    // FC15a (F1, C 6.10.3.3p3): build the single TOKEN produced by pasting the
    // spelling `spelling` (the left operand's spelling concatenated with the
    // right's). The pasted text MUST re-tokenize to EXACTLY ONE significant token
    // -- `lookupLexeme` alone is insufficient (it would silently accept `1##"x"`
    // -> `1"x"` or `)##(` -> `)(`). Zero or more-than-one significant tokens is a
    // constraint violation -> fail loud P_PreprocessorPaste (positioned at the
    // `##`), returning nullopt; the caller recovers by emitting both operands.
    std::optional<Token> pasteTokens(std::string_view spelling,
                                     SourceSpan opSpan) {
        // NOTE: materializeSignificant appends to productText_ unconditionally,
        // so a REJECTED paste still occupies a few bytes of the product tail --
        // harmless (no token references them; the buffer only grows).
        std::vector<Token> toks = materializeSignificant(spelling);
        if (toks.size() != 1) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPaste, synth_->id(), opSpan,
                   std::string{"pasting formed '"} + std::string{spelling}
                       + "', which is not a single valid token");
            return std::nullopt;
        }
        return toks.front();
    }

    // Union of an argument token's own (accreted) hide set with the invocation
    // hide set `hs`. Argument tokens were pre-expanded in the caller's context,
    // so they may already hide names; we ADD `hs` (the invoked macro plus the
    // surviving name∩close intersection) on top, never dropping the argument's.
    static HideSet hideUnionAll(HideSet const& argHide, HideSet const& hs) {
        if (!hs || hs->empty()) return argHide;
        if (!argHide || argHide->empty()) return hs;
        auto out = std::make_shared<std::set<std::string>>(*argHide);
        out->insert(hs->begin(), hs->end());
        return out;
    }

    // Stream macro-expander (C 6.10.3 / 6.10.3.4 via Prosser's PRECISE per-token
    // hide set). Walks a cursor over the ExpToken working set `in`; on each
    // expansion it SPLICES the substituted tokens (each carrying its computed
    // hide set) back over the consumed `[i, past)` region of `in` and RESCANS
    // from the splice point -- so a function-like name and a `(` that become
    // adjacent only ACROSS the boundary between a replacement and the surrounding
    // parent stream are re-paired (`A(F)(3)`, `NAME(4)`). A token whose macro name
    // is IN its own hide set is frozen (direct self-reference, mutual recursion).
    // The `depth` backstop is defense-in-depth: a correct hide set already bounds
    // recursion, but a malformed/over-deep chain still fails LOUD here rather than
    // downstream at the parser.
    std::vector<ExpToken> expand(std::vector<ExpToken> in, int depth) {
        std::vector<ExpToken> out;
        if (depth > 256) {                 // pathological-NESTING backstop
            // FAIL LOUD (FC13 cycle 2 review fold): emit a positioned
            // diagnostic at the backstop instead of silently returning the
            // input verbatim. A silently-truncated deep nest otherwise fails
            // DOWNSTREAM at the parser with an inscrutable error; surfacing it
            // HERE attributes the real cause (macro expansion nested too deep)
            // to the PP. Position on the first token of this run (the deepest
            // re-entry's lead token) when available.
            //
            // Under the PRECISE hide set a finite macro CHAIN (`M0`->...->`Mn`)
            // expands ITERATIVELY in one frame (each step splices + rescans,
            // depth stays flat) and terminates correctly -- so this backstop no
            // longer fires on a finite chain (that was a cycle-2 artifact of the
            // recursive engine). What still recurses is NESTING: argument
            // pre-expansion (`expand(arg, depth+1)`), so a pathological
            // 256-deep-nested argument (`F(F(F(...F(0)...)))`) trips this guard.
            // Defense-in-depth: the hide set already bounds macro recursion; this
            // catches an over-deep nest (or an internal bug) loudly, not silently.
            emitPP(rep_, DiagnosticCode::P_PreprocessorUnsupported, synth_->id(),
                   (in.empty() ? SourceSpan::empty(0) : in.front().tok.span),
                   "macro expansion nesting too deep (>256)");
            truncated_ = true;   // stream is now truncated — PP fatal
            return in;
        }
        std::size_t i = 0;
        while (i < in.size()) {
            ExpToken const& t = in[i];
            if (!isWord(t.tok)) { out.push_back(t); ++i; continue; }
            const std::string name{text(t.tok)};
            auto it = table_.find(name);
            // Not a macro, OR M is in THIS token's hide set (Prosser: M ∉
            // hideset(T) required to expand) -> emit verbatim.
            if (it == table_.end() || hideContains(t.hide, name)) {
                out.push_back(t);
                ++i;
                continue;
            }
            MacroDef const& def = it->second;
            if (!def.isFunctionLike) {
                // OBJECT-like (Prosser): replace T with M's replacement, each
                // token carrying hideset(T) ∪ {M}; splice over [i, i+1) and
                // RESCAN from i (so a function-like name newly exposed at the
                // replacement's tail re-pairs with the parent's `(`).
                const HideSet hs = hideAdd(t.hide, name);
                std::vector<ExpToken> repl;
                repl.reserve(def.replacement.size());
                for (Token const& r : def.replacement) {
                    repl.push_back(ExpToken{r, hs});
                }
                spliceOver(in, i, i + 1, repl);
                continue;          // rescan from i (the first replacement token)
            }
            // FUNCTION-like: an invocation ONLY if the next significant token is
            // the configured `(`. Otherwise emit the name VERBATIM (C 6.10.3p10:
            // a function-like name not followed by `(` is not an invocation).
            std::size_t openIdx = nextSignificant(in, i + 1);
            if (openIdx >= in.size() || !isParenOpen(in[openIdx].tok)) {
                out.push_back(t);
                ++i;
                continue;
            }
            std::size_t past = 0;
            std::vector<ExpToken> separators;  // depth-1 commas (for __VA_ARGS__)
            HideSet closeHide;                 // close-paren hide set (Prosser ∩)
            auto argsOpt =
                collectArgs(in, openIdx, name, past, separators, closeHide);
            if (!argsOpt) {
                // Unterminated invocation already reported: emit the name as-is
                // and resume after it (do NOT swallow the rest of the stream).
                out.push_back(t);
                ++i;
                continue;
            }
            std::vector<std::vector<ExpToken>> args = std::move(*argsOpt);
            // Zero-PARAMETER normalization (C 6.10.3p4): a NON-variadic macro that
            // takes NO parameters invoked as `M()` is ZERO arguments, but
            // collectArgs reports it as one EMPTY argument (the general groups
            // rule). Collapse that one empty group to zero args ONLY when the
            // macro declares no parameters, so `M()` matches arity 0 while a
            // one-parameter `G()` keeps its single empty argument. For a VARIADIC
            // macro with zero NAMED params (`V(...)`) invoked as `V()`, the same
            // collapse yields zero arguments -> an EMPTY variadic portion (C23 ok)
            // -- so the named-arity floor (0) is met and __VA_ARGS__ is empty.
            if (def.params.empty() && args.size() == 1 && args[0].empty()) {
                args.clear();
            }
            // ARITY check (C 6.10.3p4). NON-variadic: exact match. VARIADIC: at
            // least `params.size()` arguments (the named params); the rest --
            // possibly NONE (C23 allows an empty variadic part) -- form
            // __VA_ARGS__. Mismatch -> fail loud, emit the name verbatim, skip
            // the whole call.
            const bool arityBad = def.isVariadic
                                      ? (args.size() < def.params.size())
                                      : (args.size() != def.params.size());
            if (arityBad) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorMacroArgument,
                       synth_->id(), t.tok.span,
                       std::string{"function-like macro "} + name
                           + (def.isVariadic ? " expects at least "
                                             : " expects ")
                           + std::to_string(def.params.size())
                           + " argument(s) but got "
                           + std::to_string(args.size()));
                out.push_back(t);
                i = past;     // skip past the malformed call close paren
                continue;
            }
            // The Prosser function-like hide set:
            //   HS' = (hideset(name) ∩ hideset(close-paren)) ∪ {M}.
            // Intersecting with the CLOSE paren's hide set is what keeps a
            // self-reference frozen (`F(x) F(x)`: the rescanned inner `F` and its
            // `)` both carry {F}, so F stays hidden) WHILE letting a cross-stream
            // re-pairing expand (`A(F)(3)`: the `(3)` came from the parent with an
            // EMPTY hide set, so the intersection drops {A} and F is free).
            const HideSet hs = hideAdd(hideIntersect(t.hide, closeHide), name);
            // PRE-EXPAND each NAMED argument FULLY before substitution
            // (C 6.10.3.1): arguments expand in the CURRENT context (their tokens
            // keep whatever hide sets they already carry; the invoked macro is NOT
            // yet added -- that happens at substitution via `hs`). For a variadic
            // macro, only the first `params.size()` args bind to named params; the
            // rest are gathered into __VA_ARGS__.
            const std::size_t namedCount =
                def.isVariadic ? def.params.size() : args.size();
            // FC15a (C 6.10.3.2 / 6.10.3.3p1): a `#`/`##` OPERAND uses the RAW
            // (un-pre-expanded) argument. Capture each named arg's raw tokens AND
            // the raw trailing `__VA_ARGS__` run BEFORE the pre-expansion move
            // below consumes `args[k]`. `substitute` chooses raw-vs-expanded per
            // operand position. (Cheap: only the call's own arg tokens; the common
            // no-`#`/`##` macro never reads these.)
            std::vector<std::vector<ExpToken>> rawArgs;
            rawArgs.reserve(namedCount);
            for (std::size_t k = 0; k < namedCount; ++k) rawArgs.push_back(args[k]);
            std::vector<ExpToken> rawVaArgs;
            if (def.isVariadic) {
                for (std::size_t k = def.params.size(); k < args.size(); ++k) {
                    if (k > def.params.size() && k - 1 < separators.size()) {
                        rawVaArgs.push_back(separators[k - 1]);
                    }
                    rawVaArgs.insert(rawVaArgs.end(), args[k].begin(),
                                     args[k].end());
                }
            }
            std::vector<std::vector<ExpToken>> expandedArgs;
            expandedArgs.reserve(namedCount);
            for (std::size_t k = 0; k < namedCount; ++k) {
                expandedArgs.push_back(expand(std::move(args[k]), depth + 1));
            }
            // Build the (pre-expanded) __VA_ARGS__ token run from the TRAILING
            // arguments (indices >= params.size()), each pre-expanded like a
            // named arg, re-joined with the ORIGINAL source separator commas
            // (`separators[k]` separates arg k from arg k+1). EMPTY when the
            // variadic portion is empty (C23). A non-variadic macro passes an
            // empty run (substitute never consults it).
            std::vector<ExpToken> vaArgs;
            if (def.isVariadic) {
                for (std::size_t k = def.params.size(); k < args.size(); ++k) {
                    if (k > def.params.size()) {
                        // The separator BEFORE this trailing arg is the comma
                        // between arg (k-1) and arg k == separators[k-1].
                        if (k - 1 < separators.size()) {
                            vaArgs.push_back(separators[k - 1]);
                        }
                    }
                    std::vector<ExpToken> ex =
                        expand(std::move(args[k]), depth + 1);
                    vaArgs.insert(vaArgs.end(), ex.begin(), ex.end());
                }
            }
            std::vector<ExpToken> substituted =
                substitute(def, expandedArgs, vaArgs, rawArgs, rawVaArgs, hs);
            // Splice the substituted result over the WHOLE call `[i, past)` and
            // RESCAN from i: the invoked macro M is in every substituted token's
            // hide set, so a self-reference is frozen; a function-like name newly
            // exposed at the substitution's tail re-pairs with the parent's `(`.
            spliceOver(in, i, past, substituted);
            // resume at i (rescan the substitution + the trailing parent stream)
        }
        return out;
    }

    // Replace `in[from, to)` with `repl` (the freshly produced tokens) and leave
    // the cursor implicitly at `from` for a rescan. Both regions can be large;
    // a vector splice (erase + insert) is the textbook realization and the
    // rewritten-token volume is bounded by the hide set, so this is fine.
    static void spliceOver(std::vector<ExpToken>& in, std::size_t from,
                           std::size_t to, std::vector<ExpToken> const& repl) {
        in.erase(in.begin() + static_cast<std::ptrdiff_t>(from),
                 in.begin() + static_cast<std::ptrdiff_t>(to));
        in.insert(in.begin() + static_cast<std::ptrdiff_t>(from),
                  repl.begin(), repl.end());
    }

    std::shared_ptr<SourceBuffer>        synth_;
    std::shared_ptr<GrammarSchema const> schema_;
    DiagnosticReporter&                  rep_;
    // Set TRUE when the >256 macro-expansion-nesting backstop fires and
    // RETURNS the input verbatim (truncating the expansion). Surfaced via
    // `truncated()` so `preprocess()` can flag the result fatal.
    bool                                 truncated_ = false;
    SchemaTokenId                        hashKind_{};
    SchemaTokenId                        parenOpen_{};
    SchemaTokenId                        parenClose_{};
    SchemaTokenId                        argSep_{};
    SchemaTokenId                        variadicMarker_{};
    // FC15a: the `#`/`##` operator kinds (config-resolved; InvalidSchemaToken
    // when the language declares neither -- then the engine never produces a
    // product).
    SchemaTokenId                        stringizeKind_{};
    SchemaTokenId                        pasteKind_{};
    // FC15a (A2): byte length of the PREFIX buffer (`synth_`), and the
    // accumulated `#`/`##` PRODUCT spellings appended AFTER it. A product token's
    // span is `[prefixLen_ + offsetInProductText_, ...)`; `text()` dispatches a
    // span at-or-after `prefixLen_` to `productText_`. The final synth buffer is
    // `synth_->text() + productText_` (built by `preprocess()` after `run()`).
    ByteOffset                           prefixLen_{};
    std::string                          productText_;
    std::unordered_map<std::string, MacroDef> table_;
    // FC14: the conditional-compilation frame stack (one frame per open
    // `#if`/`#ifdef`/`#ifndef`). See CondFrame + handleIf/Elif/Else/Endif.
    std::vector<CondFrame>               condStack_;
};

} // namespace

PreprocessResult preprocess(
    std::shared_ptr<SourceBuffer>        mainSource,
    std::shared_ptr<GrammarSchema const> schema,
    std::span<fs::path const>            includeDirs) {
    if (!mainSource || !schema) ppFatal("preprocess: null source or schema");
    if (!schema->preprocess().enabled) {
        ppFatal("preprocess: called with a schema whose preprocess pass is "
                "disabled - caller must gate on preprocess().enabled");
    }

    PreprocessResult result;
    result.diagnostics = std::make_unique<DiagnosticReporter>();

    std::string synthText;
    std::vector<fs::path> includeStack;
    {
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(fs::path{mainSource->name()}, ec);
        includeStack.push_back(ec ? fs::path{mainSource->name()} : canon);
    }
    SynthBuilder builder{schema, includeDirs, *result.diagnostics, 0,
                         includeStack, result.fatal};
    builder.build(mainSource, synthText, result.lineMap);

    // FC15a (A2 reorder): the `#`/`##` operators produce SYNTHETIC tokens whose
    // text does not exist in the spliced prefix. Their spelling must reach the
    // PARSER via a real span, so it is APPENDED to the synth text and the FINAL
    // buffer is frozen AFTER expansion. Until then, tokenization + the macro
    // expander read the spliced text via a PROVISIONAL PREFIX buffer (the same
    // bytes as the final buffer's leading prefix, so every original-token span
    // resolves identically in both). `prefixLen` is the byte length of that
    // prefix; a product token's span points at `[prefixLen + productOffset, ...)`.
    const ByteOffset prefixLen = static_cast<ByteOffset>(synthText.size());
    auto prefixBuffer = SourceBuffer::fromString(
        synthText, std::string{mainSource->name()});
    result.mainSourceId = mainSource->id();

    auto ppToks = tokenizeToPP(prefixBuffer, schema, *result.diagnostics);
    std::vector<Token> synthTokens;
    synthTokens.reserve(ppToks.size());
    for (auto const& tk : ppToks) synthTokens.push_back(tk.tok);

    MacroExpander expander{prefixBuffer, schema, *result.diagnostics, prefixLen};
    std::vector<Token> finalTokens = expander.run(synthTokens);
    // OR in the macro-expansion truncation; the SynthBuilder already wrote
    // `result.fatal` by reference for an include-nesting truncation.
    result.fatal = result.fatal || expander.truncated();

    // Append the accumulated `#`/`##` product spellings AFTER the prefix, then
    // freeze the FINAL buffer: ONE buffer whose unchanged leading prefix backs
    // every original token and whose appended tail backs every product token
    // (A2 -- no side-vector). When no product was generated this is byte-identical
    // to the prefix (the FC14 single-buffer behavior).
    synthText.append(expander.productText());
    result.synthBuffer = SourceBuffer::fromString(
        std::move(synthText), std::string{mainSource->name()});

    // Collect the DISTINCT origin buffers the line-map references (the original
    // main file + every spliced header), EXCLUDING the synth buffer. The
    // caller registers these so a `makeRemap`-redirected diagnostic resolves
    // for rendering instead of `--> <unknown-buffer:N>`. Dedup by buffer id.
    {
        std::unordered_set<BufferId> seenOrigins;
        BufferId const synthId = result.synthBuffer->id();
        for (LineMapSegment const& seg : result.lineMap.segments()) {
            if (!seg.origin) continue;
            BufferId const oid = seg.origin->id();
            if (oid == synthId) continue;   // never the synth buffer itself
            if (!seenOrigins.insert(oid).second) continue;
            result.originBuffers.push_back(seg.origin);
        }
    }

    // Diagnostics emitted during the build (BufferId{}, the synth id did not
    // exist yet) AND during tokenize/expansion (stamped with the PROVISIONAL
    // PREFIX buffer id) both belong on the FINAL synth buffer: every such span is
    // a prefix span, byte-identical in the final buffer (a strict prefix). Rewrite
    // both to the final synth id so the later `makeRemap` can attribute them.
    BufferId const prefixId = prefixBuffer->id();
    result.diagnostics->remapBuffers([&](BufferId& bid, SourceSpan&) {
        if (bid == BufferId{} || bid == prefixId) {
            bid = result.synthBuffer->id();
        }
    });

    Token eof;
    eof.coreKind = CoreTokenKind::Eof;
    eof.span     = SourceSpan::empty(
        static_cast<ByteOffset>(result.synthBuffer->size()));
    finalTokens.push_back(eof);
    result.tokens = std::move(finalTokens);

    return result;
}

} // namespace dss
