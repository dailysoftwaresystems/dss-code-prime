#include "analysis/preprocess/preprocessor.hpp"

#include "tokenizer/tokenizer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
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
    std::vector<Token> replacement;
    std::string        text;
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
        std::vector<Token> out;
        out.reserve(body.size());
        std::unordered_set<std::string> painting;
        for (std::size_t k = 0; k < body.size(); ++k) {
            expandInto(body[k], out, painting, 0);
        }
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
        if (p < end && parenOpen_.valid() && in[p].schemaKind == parenOpen_
            && in[p].span.start() == in[nameIdx].span.end()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorUnsupported,
                   synth_->id(), in[nameIdx].span,
                   std::string{"function-like macro is not supported yet "
                               "(FC13 cycle 2; D-PP-FUNCTION-LIKE-MACRO): "}
                       + name);
            return;
        }
        MacroDef def;
        std::string repText;
        for (std::size_t q = skipTrivia(in, p); q < end;
             q = skipTrivia(in, q + 1)) {
            if (isNewline(in[q])) break;
            def.replacement.push_back(in[q]);
            if (!repText.empty()) repText.push_back(space);
            repText.append(text(in[q]));
        }
        def.text = repText;
        auto it = table_.find(name);
        if (it != table_.end() && it->second.text != def.text) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorMacroRedefinition,
                   synth_->id(), in[nameIdx].span,
                   std::string{"incompatible redefinition of macro: "} + name);
            return;
        }
        table_[name] = std::move(def);
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

    void expandInto(Token const& t, std::vector<Token>& out,
                    std::unordered_set<std::string>& painting, int depth) {
        if (depth > 256) { out.push_back(t); return; }
        if (!isWord(t)) { out.push_back(t); return; }
        const std::string name{text(t)};
        auto it = table_.find(name);
        if (it == table_.end() || painting.count(name) != 0) {
            out.push_back(t);
            return;
        }
        painting.insert(name);
        for (Token const& r : it->second.replacement) {
            expandInto(r, out, painting, depth + 1);
        }
        painting.erase(name);
    }

    std::shared_ptr<SourceBuffer>        synth_;
    std::shared_ptr<GrammarSchema const> schema_;
    DiagnosticReporter&                  rep_;
    SchemaTokenId                        hashKind_{};
    SchemaTokenId                        parenOpen_{};
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
