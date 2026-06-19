#include "analysis/preprocess/preprocessor.hpp"

#include "tokenizer/tokenizer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
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
        // AND function-like macros share one cursor-walking engine).
        std::unordered_set<std::string> painting;
        return expand(body, painting, 0);
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
            if (!parseParamList(in, p, end, name, def.params)) {
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
            && a.params == b.params
            && a.text == b.text;
    }

    // Parse a function-like macro's parameter list, starting at `open` (the
    // index of the adjacent `(`). On success fills `out` with the parameter
    // NAMES in order, advances `open` PAST the closing `)`, and returns true.
    // On any malformed input emits a fail-loud diagnostic and returns false.
    // Grammar (C 6.10.3, no variadic in cycle 2):
    //   ( )                        -> zero parameters
    //   ( id ( , id )* )           -> named parameters, comma-separated
    // FAIL-LOUD on: a `...` (variadic -- D-PP-VARIADIC-MACRO, out of scope), a
    // duplicate parameter name, a non-identifier where a parameter is expected,
    // a missing comma between parameters, or no closing `)` before line end.
    bool parseParamList(std::vector<Token> const& in, std::size_t& open,
                        std::size_t end, std::string const& macroName,
                        std::vector<std::string>& out) {
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
            // A variadic marker (C's `...`) in parameter position is a VARIADIC
            // macro -- recognized but deferred (D-PP-VARIADIC-MACRO, FC15-area).
            // Detect it by the CONFIGURED token KIND (`variadicMarkerToken`),
            // NOT by the hard-coded `...` lexeme: a second preprocess-opting
            // language whose variadic marker is spelled differently would
            // otherwise have a word-like marker silently accepted as a named
            // parameter. The `.valid()` guard means a language that declares no
            // variadic form simply never matches here (the marker falls through
            // to the not-a-Word fail-loud below).
            if (isVariadicMarker(in[q])) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorUnsupported,
                       synth_->id(), in[q].span,
                       std::string{"variadic macro is not supported "
                                   "(D-PP-VARIADIC-MACRO): "}
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
    static std::size_t nextSignificant(std::span<Token const> in,
                                       std::size_t from) {
        std::size_t j = from;
        while (j < in.size() && (isTrivia(in[j]) || isNewline(in[j]))) ++j;
        return j;
    }

    // Trim leading + trailing whitespace-trivia (incl. newlines/comments) from
    // an argument token run, in place. Interior trivia is PRESERVED (an
    // argument may legitimately contain spaces, e.g. `a + b`).
    static void trimArgTrivia(std::vector<Token>& arg) {
        std::size_t a = 0, b = arg.size();
        while (a < b && (isTrivia(arg[a]) || isNewline(arg[a]))) ++a;
        while (b > a && (isTrivia(arg[b - 1]) || isNewline(arg[b - 1]))) --b;
        if (a != 0 || b != arg.size()) {
            arg = std::vector<Token>(arg.begin() + static_cast<std::ptrdiff_t>(a),
                                     arg.begin() + static_cast<std::ptrdiff_t>(b));
        }
    }

    // Collect a function-like macro call argument list. `in[open]` is the
    // invocation `(`. Scans tracking PAREN DEPTH (only the configured open/close
    // paren affect depth): a depth-1 comma SEPARATES arguments; the matching
    // depth-0 close ENDS the list. Each argument preserves its interior tokens
    // (nested parens/commas survive); leading/trailing trivia is trimmed. By the
    // comma-separated-groups rule, `(x)` is ONE argument and `()` is ONE EMPTY
    // argument; the special zero-PARAMETER case (`M()` for a 0-arg macro = zero
    // arguments, C 6.10.3p4) is normalized by the CALLER (which knows
    // params.size()). On success returns the arguments and sets `past` to the
    // index JUST PAST the matching close. On EOF before the matching close,
    // emits a fail-loud diagnostic and returns std::nullopt.
    std::optional<std::vector<std::vector<Token>>>
    collectArgs(std::span<Token const> in, std::size_t open,
                std::string const& macroName, std::size_t& past) {
        std::vector<std::vector<Token>> args;
        std::vector<Token>              cur;
        int depth = 1;                 // start just inside the opening `(`
        std::size_t j = open + 1;
        for (; j < in.size(); ++j) {
            Token const& t = in[j];
            if (isParenOpen(t)) {
                ++depth;
                cur.push_back(t);
                continue;
            }
            if (isParenClose(t)) {
                --depth;
                if (depth == 0) {
                    // End of list: flush the final (possibly empty) group.
                    trimArgTrivia(cur);
                    args.push_back(std::move(cur));
                    past = j + 1;
                    return args;
                }
                cur.push_back(t);
                continue;
            }
            if (depth == 1 && isArgSeparator(t)) {
                // Top-level separator: close current argument, start the next.
                trimArgTrivia(cur);
                args.push_back(std::move(cur));
                cur.clear();
                continue;
            }
            cur.push_back(t);
        }
        // Ran off the end without the matching close -> unterminated.
        emitPP(rep_, DiagnosticCode::P_PreprocessorMacroArgument, synth_->id(),
               (open < in.size() ? in[open].span : SourceSpan::empty(0)),
               std::string{"unterminated argument list for function-like "
                           "macro: "}
                   + macroName);
        return std::nullopt;
    }

    // Build the substituted replacement list for a function-like call: each
    // replacement token that IS a parameter name is replaced by that
    // parameter PRE-EXPANDED argument tokens (C 6.10.3.1); every other
    // replacement token passes through unchanged. No `#`/`##` handling (FC15).
    // `expandedArgs[k]` is the fully-expanded argument for `params[k]`.
    std::vector<Token> substitute(
        MacroDef const& def,
        std::vector<std::vector<Token>> const& expandedArgs) {
        std::vector<Token> outTokens;
        for (Token const& r : def.replacement) {
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
                std::vector<Token> const& a =
                    expandedArgs[static_cast<std::size_t>(paramIdx)];
                outTokens.insert(outTokens.end(), a.begin(), a.end());
            } else {
                outTokens.push_back(r);
            }
        }
        return outTokens;
    }

    // Stream macro-expander (C 6.10.3): walk a cursor over `in`, expanding
    // object- AND function-like invocations and rescanning each result under
    // the shared blue-paint set. Returns the fully-expanded token run.
    std::vector<Token> expand(std::span<Token const> in,
                              std::unordered_set<std::string>& painting,
                              int depth) {
        std::vector<Token> out;
        if (depth > 256) {                 // pathological-recursion backstop
            // FAIL LOUD (FC13 cycle 2 review fold): emit a positioned
            // diagnostic at the backstop instead of silently returning the
            // input verbatim. A silently-truncated deep chain otherwise fails
            // DOWNSTREAM at the parser with an inscrutable error; surfacing it
            // HERE attributes the real cause (macro expansion nested too deep)
            // to the PP. Position on the first token of this run (the deepest
            // re-entry's lead token) when available.
            emitPP(rep_, DiagnosticCode::P_PreprocessorUnsupported, synth_->id(),
                   (in.empty() ? SourceSpan::empty(0) : in.front().span),
                   "macro expansion nesting too deep (>256)");
            out.assign(in.begin(), in.end());
            return out;
        }
        std::size_t i = 0;
        while (i < in.size()) {
            Token const& t = in[i];
            if (!isWord(t)) { out.push_back(t); ++i; continue; }
            const std::string name{text(t)};
            auto it = table_.find(name);
            if (it == table_.end() || painting.count(name) != 0) {
                out.push_back(t);
                ++i;
                continue;
            }
            MacroDef const& def = it->second;
            if (!def.isFunctionLike) {
                // OBJECT-like: paint, expand+rescan the replacement, unpaint.
                painting.insert(name);
                std::vector<Token> rescanned =
                    expand(def.replacement, painting, depth + 1);
                painting.erase(name);
                out.insert(out.end(), rescanned.begin(), rescanned.end());
                ++i;
                continue;
            }
            // FUNCTION-like: an invocation ONLY if the next significant token is
            // the configured `(`. Otherwise emit the name VERBATIM (C 6.10.3p10:
            // a function-like name not followed by `(` is not an invocation).
            std::size_t openIdx = nextSignificant(in, i + 1);
            if (openIdx >= in.size() || !isParenOpen(in[openIdx])) {
                out.push_back(t);
                ++i;
                continue;
            }
            std::size_t past = 0;
            auto argsOpt = collectArgs(in, openIdx, name, past);
            if (!argsOpt) {
                // Unterminated invocation already reported: emit the name as-is
                // and resume after it (do NOT swallow the rest of the stream).
                out.push_back(t);
                ++i;
                continue;
            }
            std::vector<std::vector<Token>> args = std::move(*argsOpt);
            // Zero-PARAMETER normalization (C 6.10.3p4): `M()` for a macro that
            // takes NO parameters is ZERO arguments, but collectArgs reports it
            // as one EMPTY argument (the general groups rule). Collapse that one
            // empty group to zero args ONLY when the macro declares no
            // parameters, so `M()` matches arity 0 while a one-parameter `G()`
            // keeps its single empty argument.
            if (def.params.empty() && args.size() == 1 && args[0].empty()) {
                args.clear();
            }
            // ARITY check (C 6.10.3p4). A zero-parameter macro invoked as `M()`
            // collects zero arguments (normalized just above); a one-parameter
            // macro invoked as `G()` collects one EMPTY argument. Mismatch ->
            // fail loud, emit the name verbatim, skip the whole call.
            if (args.size() != def.params.size()) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorMacroArgument,
                       synth_->id(), t.span,
                       std::string{"function-like macro "} + name
                           + " expects "
                           + std::to_string(def.params.size())
                           + " argument(s) but got "
                           + std::to_string(args.size()));
                out.push_back(t);
                i = past;     // skip past the malformed call close paren
                continue;
            }
            // PRE-EXPAND each argument FULLY before substitution (C 6.10.3.1):
            // arguments expand in the CURRENT paint context, with the invoked
            // macro NOT yet painted (painted only for the rescan below).
            std::vector<std::vector<Token>> expandedArgs;
            expandedArgs.reserve(args.size());
            for (std::vector<Token> const& a : args) {
                expandedArgs.push_back(expand(a, painting, depth + 1));
            }
            std::vector<Token> substituted = substitute(def, expandedArgs);
            // RESCAN the substituted result with the invoked macro painted blue
            // (C 6.10.3.4: a self-reference in the rescan is frozen).
            painting.insert(name);
            std::vector<Token> rescanned =
                expand(substituted, painting, depth + 1);
            painting.erase(name);
            out.insert(out.end(), rescanned.begin(), rescanned.end());
            i = past;         // resume AFTER the matching close paren
        }
        return out;
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
