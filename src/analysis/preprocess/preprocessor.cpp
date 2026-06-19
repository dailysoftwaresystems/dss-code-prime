#include "analysis/preprocess/preprocessor.hpp"

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
                               includeStack};
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
    MacroExpander(std::shared_ptr<SourceBuffer> synth,
                  std::shared_ptr<GrammarSchema const> schema,
                  DiagnosticReporter& rep)
        : synth_(std::move(synth)), schema_(std::move(schema)), rep_(rep) {
        hashKind_  = schema_->schemaTokens().find(cfg().directiveIntroToken);
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

    std::vector<Token> run(std::vector<Token> const& in) {
        std::vector<Token> body;
        std::size_t i = 0;
        while (i < in.size()) {
            if (isHash(in[i]) && firstOnLine(in, i)) {
                i = handleDirective(in, i, body);
                continue;
            }
            body.push_back(in[i]);
            ++i;
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
    std::string_view text(Token const& t) const {
        return synth_->slice(t.span);
    }
    bool isHash(Token const& t) const {
        return hashKind_.valid() && t.schemaKind == hashKind_;
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
        if (word == cfg().defineDirective) {
            handleDefine(in, p + 1, end);
        } else if (word == cfg().undefDirective) {
            handleUndef(in, p + 1, end);
        } else if (word == cfg().includeDirective) {
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

    // Build the substituted replacement list for a function-like call: each
    // replacement token that IS a NAMED parameter is replaced by that
    // parameter's PRE-EXPANDED argument tokens (C 6.10.3.1); in a VARIADIC macro,
    // a replacement `Word` whose text == the configured `variadicArgsName`
    // (`__VA_ARGS__`) is replaced by the PRE-EXPANDED, comma-joined trailing
    // arguments (`vaArgs`, possibly EMPTY -> substitutes to nothing, C23); every
    // other replacement token passes through unchanged, stamped with `hs`. No
    // `#`/`##` handling (FC15). `expandedArgs[k]` is the fully-expanded argument
    // for `params[k]`.
    //
    // HIDE-SET stamping (Prosser, C 6.10.3.4): EVERY token of the substituted
    // result carries `hs` = (hideset(name) ∩ hideset(close-paren)) ∪ {M}. A
    // replacement token NOT from an argument is stamped with exactly `hs`. An
    // argument's tokens already carry their OWN hide sets (accreted while the arg
    // was pre-expanded in the caller's context); each is UNIONED with `hs` so the
    // invoked macro (and the surviving intersection) is added without dropping
    // what the argument expansion already hid. The `__VA_ARGS__` check precedes
    // the named-parameter scan, but they cannot collide -- `__VA_ARGS__` is not a
    // valid parameter NAME, and a non-variadic macro that mentions it was already
    // rejected at definition time.
    std::vector<ExpToken> substitute(
        MacroDef const& def,
        std::vector<std::vector<ExpToken>> const& expandedArgs,
        std::vector<ExpToken> const& vaArgs,
        HideSet const& hs) {
        std::vector<ExpToken> outTokens;
        auto appendArg = [&](std::vector<ExpToken> const& a) {
            for (ExpToken const& e : a) {
                // An argument token already carries its own (accreted) hide set;
                // UNION the invocation hide set `hs` on top.
                outTokens.push_back(ExpToken{e.tok, hideUnionAll(e.hide, hs)});
            }
        };
        for (Token const& r : def.replacement) {
            if (def.isVariadic && isWord(r) && !cfg().variadicArgsName.empty()
                && text(r) == cfg().variadicArgsName) {
                appendArg(vaArgs);
                continue;
            }
            int paramIdx = -1;
            if (isWord(r)) {
                std::string_view rt = text(r);
                for (std::size_t k = 0; k < def.params.size(); ++k) {
                    if (def.params[k] == rt) {
                        paramIdx = static_cast<int>(k);
                        break;
                    }
                }
            }
            if (paramIdx >= 0) {
                appendArg(expandedArgs[static_cast<std::size_t>(paramIdx)]);
            } else {
                // A plain replacement token gets EXACTLY hs (it has no prior
                // hide set of its own -- it comes straight from the #define).
                outTokens.push_back(ExpToken{r, hs});
            }
        }
        return outTokens;
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
                substitute(def, expandedArgs, vaArgs, hs);
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
    SchemaTokenId                        hashKind_{};
    SchemaTokenId                        parenOpen_{};
    SchemaTokenId                        parenClose_{};
    SchemaTokenId                        argSep_{};
    SchemaTokenId                        variadicMarker_{};
    std::unordered_map<std::string, MacroDef> table_;
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
                         includeStack};
    builder.build(mainSource, synthText, result.lineMap);

    result.synthBuffer = SourceBuffer::fromString(
        std::move(synthText), std::string{mainSource->name()});
    result.mainSourceId = mainSource->id();

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

    // Build-phase diagnostics were stamped with BufferId{} (the synth id did
    // not exist yet); rewrite them to the real synth buffer so the later
    // remap can attribute them.
    result.diagnostics->remapBuffers([&](BufferId& bid, SourceSpan&) {
        if (bid == BufferId{}) bid = result.synthBuffer->id();
    });

    auto ppToks = tokenizeToPP(result.synthBuffer, schema, *result.diagnostics);
    std::vector<Token> synthTokens;
    synthTokens.reserve(ppToks.size());
    for (auto const& tk : ppToks) synthTokens.push_back(tk.tok);

    MacroExpander expander{result.synthBuffer, schema, *result.diagnostics};
    std::vector<Token> finalTokens = expander.run(synthTokens);

    Token eof;
    eof.coreKind = CoreTokenKind::Eof;
    eof.span     = SourceSpan::empty(
        static_cast<ByteOffset>(result.synthBuffer->size()));
    finalTokens.push_back(eof);
    result.tokens = std::move(finalTokens);

    return result;
}

} // namespace dss
