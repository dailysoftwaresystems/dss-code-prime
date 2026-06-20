#include "lir/lir_text.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "lir/lir_literal_pool.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_reg.hpp"
#include "lir/lir_verifier.hpp"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace dss {

namespace {

[[nodiscard]] std::optional<TypeKind>
typeKindFromName(std::string_view s) noexcept;

// TypeKind round-trip tag emitted on every literal-pool entry. The
// pool can't infer the tag from the variant arm alone (e.g. `Char` vs
// `U32` both live in `uint64_t`), so the tag is the disambiguator.
[[nodiscard]] std::string_view typeKindName(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool:      return "Bool";
        case TypeKind::I8:        return "I8";
        case TypeKind::I16:       return "I16";
        case TypeKind::I32:       return "I32";
        case TypeKind::I64:       return "I64";
        case TypeKind::I128:      return "I128";
        case TypeKind::U8:        return "U8";
        case TypeKind::U16:       return "U16";
        case TypeKind::U32:       return "U32";
        case TypeKind::U64:       return "U64";
        case TypeKind::U128:      return "U128";
        case TypeKind::F16:       return "F16";
        case TypeKind::F32:       return "F32";
        case TypeKind::F64:       return "F64";
        case TypeKind::F128:      return "F128";
        case TypeKind::Char:      return "Char";
        case TypeKind::Byte:      return "Byte";
        case TypeKind::Void:      return "Void";
        case TypeKind::Struct:    return "Struct";
        case TypeKind::Union:     return "Union";
        case TypeKind::Tuple:     return "Tuple";
        case TypeKind::Array:     return "Array";
        case TypeKind::Slice:     return "Slice";
        case TypeKind::Enum:      return "Enum";
        case TypeKind::Vector:    return "Vector";
        case TypeKind::Matrix:    return "Matrix";
        case TypeKind::Ptr:       return "Ptr";
        case TypeKind::Ref:       return "Ref";
        case TypeKind::FnPtr:     return "FnPtr";
        case TypeKind::Nullable:  return "Nullable";
        case TypeKind::Optional:  return "Optional";
        case TypeKind::FnSig:     return "FnSig";
        case TypeKind::Param:     return "Param";
        case TypeKind::Bind:      return "Bind";
        case TypeKind::Extension: return "Extension";
        case TypeKind::Count_:    break;
    }
    return "?";
}

// Inverse of `typeKindName` — reads the `core <Kind>` suffix tag from
// `.dsslir` text. Walks the enum once; the linear scan is fine for the
// modest core-kind count and avoids maintaining a parallel string map.
[[nodiscard]] std::optional<TypeKind>
typeKindFromName(std::string_view s) noexcept {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(TypeKind::Count_); ++i) {
        auto const k = static_cast<TypeKind>(i);
        if (typeKindName(k) == s) return k;
    }
    return std::nullopt;
}

// Render a symbol id as `%<v>` always, optionally followed by ` "name"`
// when ctx supplies a non-empty entry. The "name" form is consumed by
// cycle-2's parser as a debug annotation; the `%<v>` numeric handle is
// the authoritative identity. Slot 0 is the invalid-symbol sentinel.
[[nodiscard]] std::string
renderSymbol(std::uint32_t v, LirTextContext const& ctx) {
    if (v < ctx.symbolNames.size() && !ctx.symbolNames[v].empty()) {
        return std::format("%{} \"{}\"", v, ctx.symbolNames[v]);
    }
    return std::format("%{}", v);
}

// Render a physical register by its declared mnemonic. Falls back to
// `phys#<id>` if the ordinal is out of range — this is a producer
// bug; emit `L_PhysRegOrdinalOutOfRange` so the verifier path catches
// it. (Distinct from `L_UnsupportedLoweringForOpcode`: register-table
// integrity, not opcode-lowering coverage.)
[[nodiscard]] std::string
renderPhysReg(LirReg r, TargetSchema const& schema,
              DiagnosticReporter& reporter) {
    auto const* info = schema.registerInfo(static_cast<std::uint16_t>(r.id));
    if (info == nullptr) {
        dss::report(
            reporter, DiagnosticCode::L_PhysRegOrdinalOutOfRange,
            DiagnosticSeverity::Warning,
            std::format("emitLir: phys reg ordinal {} not in register table",
                        static_cast<std::uint32_t>(r.id)));
        return std::format("phys#{}", static_cast<std::uint32_t>(r.id));
    }
    return info->name;
}

// Render any register (physical or virtual). Virtual: `%v.<id>:<class>`.
// The `:<class>` suffix is MANDATORY (not optional default-elided) so a
// round-trip preserves the vreg's class even when the parser has no
// MIR cross-reference to re-derive it. Without this, an FPR vreg would
// silently demote to GPR on parse, breaking the byte-identical contract.
[[nodiscard]] std::string
renderReg(LirReg r, TargetSchema const& schema, DiagnosticReporter& reporter) {
    if (!r.valid()) return "<invalid>";
    if (r.isPhysical != 0) return renderPhysReg(r, schema, reporter);
    return std::format("%v.{}:{}", static_cast<std::uint32_t>(r.id),
                       lirRegClassName(r.regClass()));
}

// Render a string literal with the same C-style escapes the parser
// will inherit in cycle 2.
[[nodiscard]] std::string renderEscapedString(std::string_view payload) {
    std::string out = "\"";
    out.reserve(payload.size() + 2);
    for (char c : payload) {
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out.append("\\n");
        else if (c == '\t') out.append("\\t");
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] std::string renderLiteralValue(LirLiteralValue const& v);

[[nodiscard]] std::string
renderAggregateLiteral(LirAggregateValue const& agg) {
    std::string out = "agg [";
    for (std::size_t i = 0; i < agg.fields.size(); ++i) {
        if (i > 0) out.append(", ");
        out.append(renderLiteralValue(agg.fields[i]));
    }
    out.push_back(']');
    return out;
}

// Render a complete literal value: value arm + `core <TypeKind>` tag.
// The tag is part of EVERY entry (including inner aggregate fields)
// so the variant arm + TypeKind together fully reconstruct the entry
// on parse — `Char` vs `U32` both live in `uint64_t` and would alias
// without the tag.
[[nodiscard]] std::string
renderLiteralValue(LirLiteralValue const& v) {
    std::string body = std::visit(
        [](auto const& payload) -> std::string {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return "poison";
            } else if constexpr (std::is_same_v<T, bool>) {
                return payload ? "bool true" : "bool false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::format("i64 {}", payload);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                return std::format("u64 {}", payload);
            } else if constexpr (std::is_same_v<T, double>) {
                return std::format("f64 {}", payload);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return "str " + renderEscapedString(payload);
            } else {
                return renderAggregateLiteral(payload);
            }
        },
        v.value);
    body.append(std::format(" core {}", typeKindName(v.core)));
    return body;
}

// Render one LIR operand. Operand kinds are encoded with sigils:
//   * `Reg`           — `<mnemonic>` (phys) or `%v.<id>` (virtual)
//   * `ImmInt`        — `#<value>`
//   * `BlockRef`      — `^b<slot>` (slot is within-function ordinal,
//                       matching the successor-list rendering)
//   * `SymbolRef`     — `@<id>`
//   * `MemBase`       — `*<scale>`
//   * `MemOffset`     — `+<offset>` or `-<offset>` for signed
//   * `LiteralIndex`  — `lit#<index>`
//   * `None`          — `_`
// The (reserved) enum slot 3 (formerly `ImmFloat`) is unreachable; the
// switch is `[[nodiscard]]`-exhaustive over the live variants.
//
// `fnEntryV` is the function's entry-block .v — subtracted from
// `op.blockSlot` so a BlockRef operand renders in the SAME within-
// function-slot space the successor list uses. Without this offset
// the inst-operand block ref would carry the module-wide id while the
// successor list carries the within-function slot, breaking round-
// trip (parser keys its `blockMap_` by within-function slot).
[[nodiscard]] std::string
renderOperand(LirOperand const& op, TargetSchema const& schema,
              std::uint32_t fnEntryV, DiagnosticReporter& reporter) {
    switch (op.kind) {
        case LirOperandKind::None:
            return "_";
        case LirOperandKind::Reg:
            return renderReg(op.reg, schema, reporter);
        case LirOperandKind::ImmInt:
            return std::format("#{}", op.immInt32);
        case LirOperandKind::BlockRef:
            return std::format("^b{}", op.blockSlot - fnEntryV);
        case LirOperandKind::SymbolRef:
            return std::format("@{}", op.symbolV);
        case LirOperandKind::MemBase:
            return std::format("*{}", op.scale);
        case LirOperandKind::MemOffset:
            return (op.offset >= 0)
                       ? std::format("+{}", op.offset)
                       : std::format("{}", op.offset);
        case LirOperandKind::LiteralIndex:
            return std::format("lit#{}", op.litIndex);
        case LirOperandKind::ByValueStackAgg:
            return std::format("byval#{}", op.byValueAggBytes);
    }
    // Fall-through is a substrate-corruption signal — the discriminator
    // landed on the reserved slot 3 (formerly ImmFloat) or on an out-of-
    // band byte. Loud so the caller's reporter audit catches it.
    dss::report(
        reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
        DiagnosticSeverity::Warning,
        std::format("emitLir: operand kind tag {} is unknown (substrate corruption)",
                    static_cast<std::uint32_t>(op.kind)));
    return "<?>";
}

void emitPreamble(std::string& out, TargetSchema const& schema) {
    // `dsslir 1` pins the FORMAT version (text grammar).
    // `target <name> version "<sv>"` pins the TARGET schema (which
    // defines the opcode/register name space) AND its semantic version,
    // so a cross-schema-bump load is rejected at parse time rather than
    // silently mis-interpreting an opcode-number permutation. The
    // version may be empty (`""`) when the JSON omitted it; parser
    // accepts empty as "any" but a non-empty mismatch is an error.
    // LirModuleId is process-local and deliberately not part of the
    // canonical text contract — two `emitLir()` outputs of the same
    // logical module must compare equal regardless of allocation order.
    out.append("dsslir 1\n");
    out.append(std::format("target {} version \"{}\"\n",
                           schema.name(), schema.version()));
}

// Walk every reachable symbol id in the module: function-declared
// symbols (`function %N {`) and `SymbolRef` operands (call targets,
// GlobalAddr). Returned as a sorted set so the preamble's `symbols`
// section is deterministic across builds.
[[nodiscard]] std::set<std::uint32_t>
collectReachableSymbols(Lir const& lir) {
    std::set<std::uint32_t> seen;
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t i = 0; i < fnCount; ++i) {
        LirFuncId const fn = lir.funcAt(i);
        seen.insert(lir.funcSymbol(fn).v);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const b = lir.funcBlockAt(fn, bi);
            std::uint32_t const instCount = lir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < instCount; ++ii) {
                LirInstId const inst = lir.blockInstAt(b, ii);
                for (auto const& op : lir.instOperands(inst)) {
                    if (op.kind == LirOperandKind::SymbolRef) {
                        seen.insert(op.symbolV);
                    }
                }
            }
        }
    }
    seen.erase(0);  // slot 0 is the invalid-symbol sentinel
    return seen;
}

void emitSymbols(std::string& out, Lir const& lir,
                 LirTextContext const& ctx) {
    out.append("symbols {\n");
    // Every reachable symbol id gets an entry, even if no name was
    // supplied. Cycle-2 parser thus never encounters an unbound `%N`.
    auto const reachable = collectReachableSymbols(lir);
    for (std::uint32_t v : reachable) {
        // Named form: `  %N "name"`. Synthetic form: `  %N` (no string).
        // The "no string" branch is the canonical synthetic marker; an
        // earlier draft used `<synthetic>` but `<` / `>` aren't in the
        // lexer's token grammar and would have forced a special-case
        // parser path purely for a debug annotation. The empty form is
        // both lossless (round-trips to the same set) and parseable
        // with the existing punctuation token set.
        if (v < ctx.symbolNames.size() && !ctx.symbolNames[v].empty()) {
            out.append(std::format("  %{} \"{}\"\n", v, ctx.symbolNames[v]));
        } else {
            out.append(std::format("  %{}\n", v));
        }
    }
    out.append("}\n");
}

void emitLiteralPool(std::string& out, LirLiteralPool const& pool) {
    out.append("literal_pool {\n");
    for (std::uint32_t i = 0; i < pool.size(); ++i) {
        out.append(std::format("  lit#{} = {}\n", i,
                               renderLiteralValue(pool.at(i))));
    }
    out.append("}\n");
}

void emitInst(std::string& out, Lir const& lir, LirInstId inst,
              TargetSchema const& schema, std::uint32_t fnEntryV,
              DiagnosticReporter& reporter) {
    std::uint16_t const op = lir.instOpcode(inst);
    auto const* info = schema.opcodeInfo(op);
    std::string mnemonic = (info != nullptr)
                               ? std::string{info->mnemonic}
                               : std::format("op#{}", op);
    LirReg const result = lir.instResult(inst);
    out.append("    ");
    if (result.valid()) {
        out.append(renderReg(result, schema, reporter));
        out.append(" = ");
    }
    out.append(mnemonic);
    auto const operands = lir.instOperands(inst);
    for (std::size_t i = 0; i < operands.size(); ++i) {
        out.append((i == 0) ? " " : ", ");
        out.append(renderOperand(operands[i], schema, fnEntryV, reporter));
    }
    // Payload + flags are ALWAYS emitted (even when 0). Skipping a
    // zero payload would silently flip `TargetCondCode::Eq` (value 0)
    // on round-trip; skipping a zero flags would lose any future
    // single-bit annotation that uses 0 as a meaningful state. The
    // 5-char-per-inst overhead is the cost of a lossless round-trip.
    out.append(std::format(" ; payload={} flags={}",
                           lir.instPayload(inst),
                           static_cast<std::uint32_t>(lir.instFlags(inst))));
    out.push_back('\n');
}

void emitBlock(std::string& out, Lir const& lir, LirFuncId fn, LirBlockId b,
               TargetSchema const& schema, DiagnosticReporter& reporter) {
    // Slot is the block's offset within the function's block range.
    // `funcEntry == funcBlockAt(0)` by builder invariant, so the
    // subtraction `b.v - fnEntry.v` always equals the within-function
    // ordinal — stable across rebuilds, deterministic across runs.
    LirBlockId const fnEntry = lir.funcEntry(fn);
    std::uint32_t const slot = b.v - fnEntry.v;
    out.append(std::format("  block ^b{}", slot));
    if (b.v == fnEntry.v) out.append(" [entry]");
    auto const succs = lir.blockSuccessors(b);
    out.append(" -> [");
    for (std::size_t i = 0; i < succs.size(); ++i) {
        if (i > 0) out.append(", ");
        out.append(std::format("^b{}", succs[i].v - fnEntry.v));
    }
    out.append("] {\n");
    std::uint32_t const n = lir.blockInstCount(b);
    for (std::uint32_t i = 0; i < n; ++i) {
        emitInst(out, lir, lir.blockInstAt(b, i), schema, fnEntry.v, reporter);
    }
    out.append("  }\n");
}

void emitFunction(std::string& out, Lir const& lir, LirFuncId fn,
                  TargetSchema const& schema, LirTextContext const& ctx,
                  DiagnosticReporter& reporter) {
    SymbolId const sym = lir.funcSymbol(fn);
    out.append(std::format("  function {} {{\n",
                           renderSymbol(sym.v, ctx)));
    std::uint32_t const blockCount = lir.funcBlockCount(fn);
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        emitBlock(out, lir, fn, lir.funcBlockAt(fn, i), schema, reporter);
    }
    out.append("  }\n");
}

} // namespace

std::string emitLir(Lir const& lir, TargetSchema const& schema,
                    LirTextContext const& ctx,
                    DiagnosticReporter& reporter) {
    std::string out;
    out.reserve(1024);
    emitPreamble(out, schema);
    emitSymbols(out, lir, ctx);
    emitLiteralPool(out, lir.literalPool());
    out.append("module {\n");
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t i = 0; i < fnCount; ++i) {
        emitFunction(out, lir, lir.funcAt(i), schema, ctx, reporter);
    }
    out.append("}\n");
    return out;
}

// ═════════════════════════════════════════════════════════════════════
// PARSER (ML8 cycle 2). Closes the round-trip contract:
//   emitLir(parseLir(emitLir(m), schema, r)->lir, schema, ctx, r) == emitLir(m, ...)
// ═════════════════════════════════════════════════════════════════════

namespace {

enum class TokKind : std::uint8_t {
    End, Unknown,
    Ident, Integer, Float, String,
    Percent, Caret, Hash, At, Underscore,
    Plus, Minus, Star, Dot, Colon,
    Eq, Arrow, Comma, Semicolon,
    LBrace, RBrace, LBracket, RBracket,
};

struct Tok {
    TokKind     kind = TokKind::End;
    std::string text;
};

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    Tok take() {
        skipWhitespaceAndComments();
        Tok t;
        if (pos_ >= text_.size()) { t.kind = TokKind::End; return t; }
        char const c = text_[pos_];
        // Single-char punctuation.
        switch (c) {
            case '{': ++pos_; t.kind = TokKind::LBrace;    t.text = "{"; return t;
            case '}': ++pos_; t.kind = TokKind::RBrace;    t.text = "}"; return t;
            case '[': ++pos_; t.kind = TokKind::LBracket;  t.text = "["; return t;
            case ']': ++pos_; t.kind = TokKind::RBracket;  t.text = "]"; return t;
            case ',': ++pos_; t.kind = TokKind::Comma;     t.text = ","; return t;
            case ';': ++pos_; t.kind = TokKind::Semicolon; t.text = ";"; return t;
            case ':': ++pos_; t.kind = TokKind::Colon;     t.text = ":"; return t;
            case '=': ++pos_; t.kind = TokKind::Eq;        t.text = "="; return t;
            case '%': ++pos_; t.kind = TokKind::Percent;   t.text = "%"; return t;
            case '^': ++pos_; t.kind = TokKind::Caret;     t.text = "^"; return t;
            case '#': ++pos_; t.kind = TokKind::Hash;      t.text = "#"; return t;
            case '@': ++pos_; t.kind = TokKind::At;        t.text = "@"; return t;
            case '_': {
                // `_` may be the None-operand sigil OR the leading char
                // of an identifier. Identifier path wins iff the next
                // char extends it.
                if (pos_ + 1 < text_.size()
                    && (std::isalnum(static_cast<unsigned char>(text_[pos_ + 1]))
                     || text_[pos_ + 1] == '_')) {
                    break;  // fall through to identifier-scan path
                }
                ++pos_; t.kind = TokKind::Underscore; t.text = "_"; return t;
            }
            case '*': ++pos_; t.kind = TokKind::Star;      t.text = "*"; return t;
            case '.': ++pos_; t.kind = TokKind::Dot;       t.text = "."; return t;
            case '+': ++pos_; t.kind = TokKind::Plus;      t.text = "+"; return t;
            case '-':
                if (pos_ + 1 < text_.size() && text_[pos_ + 1] == '>') {
                    pos_ += 2; t.kind = TokKind::Arrow; t.text = "->"; return t;
                }
                ++pos_; t.kind = TokKind::Minus; t.text = "-"; return t;
            default: break;
        }
        if (c == '"') {
            ++pos_;
            std::string s;
            while (pos_ < text_.size() && text_[pos_] != '"') {
                if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
                    char const e = text_[pos_ + 1];
                    if      (e == 'n')  s += '\n';
                    else if (e == 't')  s += '\t';
                    else if (e == '"')  s += '"';
                    else if (e == '\\') s += '\\';
                    else                s += e;
                    pos_ += 2;
                } else {
                    s += text_[pos_++];
                }
            }
            if (pos_ < text_.size()) ++pos_;  // consume closing '"'
            t.kind = TokKind::String;
            t.text = std::move(s);
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t const start = pos_;
            while (pos_ < text_.size()
                && (std::isdigit(static_cast<unsigned char>(text_[pos_]))
                 || text_[pos_] == '.'
                 || text_[pos_] == 'e' || text_[pos_] == 'E'
                 || ((text_[pos_] == '+' || text_[pos_] == '-')
                     && pos_ > start
                     && (text_[pos_ - 1] == 'e' || text_[pos_ - 1] == 'E')))) {
                ++pos_;
            }
            t.text = std::string(text_.substr(start, pos_ - start));
            t.kind = (t.text.find('.') != std::string::npos
                   || t.text.find('e') != std::string::npos
                   || t.text.find('E') != std::string::npos)
                ? TokKind::Float : TokKind::Integer;
            return t;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t const start = pos_;
            while (pos_ < text_.size()
                && (std::isalnum(static_cast<unsigned char>(text_[pos_]))
                 || text_[pos_] == '_')) {
                ++pos_;
            }
            t.text = std::string(text_.substr(start, pos_ - start));
            t.kind = TokKind::Ident;
            return t;
        }
        ++pos_;
        t.kind = TokKind::Unknown;
        t.text = std::string(1, c);
        return t;
    }

    [[nodiscard]] Tok peek() {
        std::size_t const save = pos_;
        Tok t = take();
        pos_ = save;
        return t;
    }

    // Save/restore pair for two-pass scanning (function bodies, inst
    // lookahead for the `<reg> = ...` prefix). Skips whitespace
    // BEFORE returning the save point so a restored cursor lands on
    // the next significant token — same shape MIR's parser uses.
    [[nodiscard]] std::size_t peekPos() {
        skipWhitespaceAndComments();
        return pos_;
    }
    void setPos(std::size_t p) noexcept { pos_ = p; }

private:
    void skipWhitespaceAndComments() {
        while (pos_ < text_.size()) {
            char const c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    std::string_view text_;
    std::size_t      pos_ = 0;
};

class Parser {
public:
    Parser(std::string_view text, TargetSchema const& schema,
           DiagnosticReporter& reporter)
        : lex_(text), schema_(schema), reporter_(reporter), builder_(schema) {}

    [[nodiscard]] std::unique_ptr<LirParseResult> run() {
        auto const errBefore = reporter_.errorCount();
        if (!parsePreamble()) return makeEmptyResult();
        if (peekIdent("symbols"))      parseSymbols();
        if (peekIdent("literal_pool")) parseLiteralPool();
        if (!expectIdent("module")) return makeEmptyResult();
        if (!expect(TokKind::LBrace)) return makeEmptyResult();
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            if (pk.kind == TokKind::Ident && pk.text == "function") {
                lex_.take();
                parseFunction();
            } else {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("expected 'function' inside module, got '{}'",
                                 pk.text));
                // Panic-mode skip until next `function` or `}`.
                while (true) {
                    Tok q = lex_.peek();
                    if (q.kind == TokKind::End) break;
                    if (q.kind == TokKind::RBrace) break;
                    if (q.kind == TokKind::Ident && q.text == "function") break;
                    lex_.take();
                }
            }
        }
        (void)expect(TokKind::RBrace);
        return finalize(errBefore);
    }

private:
    Lexer                                       lex_;
    TargetSchema const&                         schema_;
    DiagnosticReporter&                         reporter_;
    LirBuilder                                  builder_;
    std::vector<std::string>                    symbolNames_;
    // Block-slot mapping is per-function (slots are function-local).
    std::unordered_map<std::uint32_t, LirBlockId> blockMap_;
    // Vreg-id mapping. Builder mints vregs from 1; text uses the same
    // 1-origin ids. We keep an explicit map so the `id` from text
    // survives even when the builder's mint order diverges (it doesn't
    // diverge today, but the map is cheap and load-bearing for round-
    // trip when a future builder change reorders mints).
    std::unordered_map<std::uint32_t, LirReg>   vregMap_;
    // Successor list parsed from the current block's header `-> [...]`.
    // The terminator dispatch uses this (not the operand-list BlockRef
    // count) because addCondBr does not embed BlockRef operands on the
    // inst — the block-header successor list is the authoritative
    // count for the dispatch fork (0=Ret/Unreachable, 1=Br, 2=CondBr).
    std::vector<std::uint32_t>                  currentBlockSuccSlots_;
    bool                                        errors_ = false;

    void emit(DiagnosticCode code, std::string what) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(what);
        reporter_.report(std::move(d));
        errors_ = true;
    }

    [[nodiscard]] std::unique_ptr<LirParseResult> makeEmptyResult() {
        // Build a valid empty module so consumers can always read
        // `result->lir` without UB; `result->ok` carries the verdict.
        // Clear `symbolNames_` too — keeping a partially-populated
        // table would let `ok=false` callers read names for symbols
        // that don't exist in the (empty) module. Either the whole
        // result is meaningful or it isn't.
        LirBuilder empty{schema_};
        symbolNames_.clear();
        auto r = std::make_unique<LirParseResult>(
            std::move(empty).finish(), std::move(symbolNames_));
        r->ok = false;
        return r;
    }

    [[nodiscard]] bool expect(TokKind k) {
        Tok t = lex_.take();
        if (t.kind != k) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected token kind {}, got '{}'",
                             static_cast<int>(k), t.text));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool expectIdent(std::string_view name) {
        Tok t = lex_.take();
        if (t.kind != TokKind::Ident || t.text != name) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected '{}', got '{}'", name, t.text));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool peekIdent(std::string_view name) {
        Tok t = lex_.peek();
        return t.kind == TokKind::Ident && t.text == name;
    }

    template <typename T>
    [[nodiscard]] T parseNumber(std::string_view text, std::string_view what) {
        T v{};
        auto [ptr, ec] = std::from_chars(text.data(),
                                         text.data() + text.size(), v);
        if (ec != std::errc{} || ptr != text.data() + text.size()) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("malformed {} value '{}'", what, text));
        }
        return v;
    }

    [[nodiscard]] double parseDouble(std::string_view text) {
        std::string buf{text};
        char* end = nullptr;
        errno = 0;
        double const d = std::strtod(buf.c_str(), &end);
        // Reject ERANGE AND trailing-garbage ("3.5junk" silently
        // parsed as 3.5 before this guard).
        if (errno == ERANGE
            || end == buf.c_str()
            || end != buf.c_str() + buf.size()) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("malformed float value '{}'", text));
        }
        return d;
    }

    // ── preamble ───────────────────────────────────────────────────
    [[nodiscard]] bool parsePreamble() {
        if (!expectIdent("dsslir")) return false;
        Tok const ver = lex_.take();
        if (ver.kind != TokKind::Integer || ver.text != "1") {
            emit(DiagnosticCode::I_TextVersionMismatch,
                 std::format("expected format version 1, got '{}'", ver.text));
            return false;
        }
        if (!expectIdent("target")) return false;
        Tok const name = lex_.take();
        if (name.kind != TokKind::Ident) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected target name identifier, got '{}'", name.text));
            return false;
        }
        if (name.text != schema_.name()) {
            emit(DiagnosticCode::I_TextVersionMismatch,
                 std::format("target name mismatch: text says '{}', loaded "
                             "schema is '{}'", name.text, schema_.name()));
            return false;
        }
        if (!expectIdent("version")) return false;
        Tok const ver2 = lex_.take();
        if (ver2.kind != TokKind::String) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected version string, got '{}'", ver2.text));
            return false;
        }
        // Empty schema version accepts any text version (loader didn't
        // pin it); non-empty must match. Surface the asymmetry as a
        // Warning when the schema is empty BUT the text carries a
        // non-empty version — that text came from a version-pinned
        // schema snapshot, so loading it against an unversioned schema
        // is an ingestion-side hazard the user should know about even
        // if we accept it.
        if (schema_.version().empty() && !ver2.text.empty()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::I_TextVersionMismatch;
            d.severity = DiagnosticSeverity::Warning;
            d.actual   = std::format(
                "target version asymmetry: text declares version \"{}\" "
                "but loaded schema's version is empty — load proceeding, "
                "but this file came from a different schema snapshot",
                ver2.text);
            reporter_.report(std::move(d));
        }
        if (!schema_.version().empty() && ver2.text != schema_.version()) {
            emit(DiagnosticCode::I_TextVersionMismatch,
                 std::format("target version mismatch: text says \"{}\", "
                             "loaded schema is \"{}\"",
                             ver2.text, schema_.version()));
            return false;
        }
        return true;
    }

    // ── symbols ────────────────────────────────────────────────────
    void parseSymbols() {
        (void)expectIdent("symbols");
        if (!expect(TokKind::LBrace)) return;
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            auto vOpt = parsePercentInteger("symbol id");
            if (!vOpt.has_value()) continue;  // diagnostic already emitted
            std::uint32_t const v = *vOpt;
            // Slot 0 IS the invalid-symbol sentinel — reject loudly so
            // a malformed text doesn't poison `symbolNames_[0]`.
            if (v == 0) {
                emit(DiagnosticCode::I_TextMalformed,
                     "symbol id 0 is the invalid-symbol sentinel; cannot "
                     "appear in symbols { }");
                if (lex_.peek().kind == TokKind::String) (void)lex_.take();
                continue;
            }
            // Optional `"name"` — bare `%N` is the synthetic form.
            Tok next = lex_.peek();
            if (next.kind == TokKind::String) {
                lex_.take();
                if (symbolNames_.size() <= v) symbolNames_.resize(v + 1);
                symbolNames_[v] = next.text;
            }
            // No name → leave symbolNames_ unset for slot v.
        }
        (void)expect(TokKind::RBrace);
    }

    // ── literal pool ───────────────────────────────────────────────
    void parseLiteralPool() {
        (void)expectIdent("literal_pool");
        if (!expect(TokKind::LBrace)) return;
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            // `lit#<idx> = <value> core <Kind>`
            if (!(pk.kind == TokKind::Ident && pk.text == "lit")) {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("expected 'lit' in literal_pool, got '{}'",
                                 pk.text));
                // Panic-skip to the next `lit` keyword or `}` so a
                // single malformed entry doesn't cascade per-token.
                while (true) {
                    Tok q = lex_.peek();
                    if (q.kind == TokKind::End) break;
                    if (q.kind == TokKind::RBrace) break;
                    if (q.kind == TokKind::Ident && q.text == "lit") break;
                    lex_.take();
                }
                continue;
            }
            (void)expectIdent("lit");
            (void)expect(TokKind::Hash);
            Tok idxTok = lex_.take();
            std::uint32_t const declaredIdx = (idxTok.kind == TokKind::Integer)
                ? parseNumber<std::uint32_t>(idxTok.text, "lit index") : 0;
            (void)expect(TokKind::Eq);
            LirLiteralValue v = parseLiteralValue();
            std::uint32_t const actualIdx = builder_.literalPoolAdd(std::move(v));
            // Slot drift is silent corruption; pin the contract here.
            if (actualIdx != declaredIdx) {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("literal pool index drift: text declared "
                                 "lit#{} but builder minted lit#{} (pool "
                                 "ordering must match emit order)",
                                 declaredIdx, actualIdx));
            }
        }
        (void)expect(TokKind::RBrace);
    }

    // Parse `<value-arm> core <Kind>` — same shape on entries AND on
    // inner aggregate fields. Mirrors the emitter's `renderLiteralValue`.
    [[nodiscard]] LirLiteralValue parseLiteralValue() {
        LirLiteralValue lv;
        Tok tag = lex_.take();
        if (tag.kind != TokKind::Ident) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected literal tag, got '{}'", tag.text));
            return lv;
        }
        if (tag.text == "poison") {
            // monostate already default
        } else if (tag.text == "bool") {
            Tok v = lex_.take();
            if (v.kind != TokKind::Ident
                || (v.text != "true" && v.text != "false")) {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("expected 'true' or 'false' after 'bool', "
                                 "got '{}'", v.text));
            }
            lv.value = (v.text == "true");
        } else if (tag.text == "i64") {
            bool const neg = (lex_.peek().kind == TokKind::Minus);
            if (neg) lex_.take();
            Tok v = lex_.take();
            std::int64_t value = parseNumber<std::int64_t>(v.text, "i64 literal");
            if (neg) value = -value;
            lv.value = value;
        } else if (tag.text == "u64") {
            Tok v = lex_.take();
            lv.value = parseNumber<std::uint64_t>(v.text, "u64 literal");
        } else if (tag.text == "f64") {
            // Floats may carry a leading `-` (the emitter renders the
            // raw `std::format("f64 {}", payload)` which prints `-0.5`
            // not `-` `0.5`). Stitch the sign back together before
            // handing the token off to strtod.
            bool const neg = (lex_.peek().kind == TokKind::Minus);
            if (neg) lex_.take();
            Tok v = lex_.take();
            std::string buf;
            if (neg) buf.push_back('-');
            buf.append(v.text);
            lv.value = parseDouble(buf);
        } else if (tag.text == "str") {
            Tok v = lex_.take();
            if (v.kind != TokKind::String) {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("expected string after 'str', got '{}'", v.text));
            }
            lv.value = std::move(v.text);
        } else if (tag.text == "agg") {
            if (!expect(TokKind::LBracket)) return lv;
            LirAggregateValue agg;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RBracket) break;
                if (!agg.fields.empty()) (void)expect(TokKind::Comma);
                agg.fields.push_back(parseLiteralValue());
            }
            (void)expect(TokKind::RBracket);
            lv.value = std::move(agg);
        } else {
            // Skip until the entry terminator (`,` for inner-aggregate
            // arms, `]` to close an aggregate, or `\n`-style break via
            // next `lit` keyword for top-level entries). Earlier draft
            // silently fell through to `expectIdent("core")` which
            // either reported a cascade or — worse — successfully
            // parsed `core <Kind>` after the bogus tag, leaving the
            // literal as `monostate` with a valid `core`. Now we
            // diagnose once and return early.
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("unknown literal tag '{}'", tag.text));
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::Comma
                 || pk.kind == TokKind::RBracket
                 || pk.kind == TokKind::RBrace
                 || pk.kind == TokKind::End) break;
                if (pk.kind == TokKind::Ident && pk.text == "lit") break;
                lex_.take();
            }
            return lv;
        }
        if (!expectIdent("core")) return lv;
        Tok coreT = lex_.take();
        if (coreT.kind != TokKind::Ident) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected TypeKind name after 'core', got '{}'",
                             coreT.text));
            return lv;
        }
        if (auto k = typeKindFromName(coreT.text); k.has_value()) {
            lv.core = *k;
        } else {
            emit(DiagnosticCode::I_TextUnknownName,
                 std::format("unknown TypeKind '{}'", coreT.text));
        }
        return lv;
    }

    // ── %N helpers ─────────────────────────────────────────────────
    //
    // Returns `std::nullopt` on parse failure so callers can distinguish
    // "user wrote `%0`" (slot 0 IS the invalid-symbol sentinel — caller
    // should reject it loudly) from "parse failed before we got an
    // integer". Earlier draft returned 0 on failure which silently
    // aliased the sentinel and let `parseSymbols` write to
    // `symbolNames_[0]` AND `parseFunction` call `addFunction(SymbolId{0})`.
    [[nodiscard]] std::optional<std::uint32_t>
    parsePercentInteger(std::string_view what) {
        if (!expect(TokKind::Percent)) return std::nullopt;
        Tok t = lex_.take();
        if (t.kind != TokKind::Integer) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected integer after '%' ({}), got '{}'",
                             what, t.text));
            return std::nullopt;
        }
        return parseNumber<std::uint32_t>(t.text, what);
    }

    // ── functions ──────────────────────────────────────────────────
    void parseFunction() {
        auto symOpt = parsePercentInteger("function symbol");
        if (!symOpt.has_value()) return;
        std::uint32_t const sym = *symOpt;
        if (sym == 0) {
            emit(DiagnosticCode::I_TextMalformed,
                 "function symbol id 0 is the invalid-symbol sentinel");
            return;
        }
        // Optional inline name annotation `"name"`. If `symbols { }`
        // already declared a name for this slot AND the inline annotation
        // disagrees, that is a producer-side bug — diagnose loudly so a
        // future emit-vs-parse drift doesn't silently flip identities.
        if (lex_.peek().kind == TokKind::String) {
            Tok name = lex_.take();
            if (symbolNames_.size() <= sym) symbolNames_.resize(sym + 1);
            if (symbolNames_[sym].empty()) {
                symbolNames_[sym] = std::move(name.text);
            } else if (symbolNames_[sym] != name.text) {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("function %{} inline name \"{}\" contradicts "
                                 "the symbols-block entry \"{}\"",
                                 sym, name.text, symbolNames_[sym]));
            }
        }
        if (!expect(TokKind::LBrace)) return;
        builder_.addFunction(SymbolId{sym});
        // Per-function state reset.
        blockMap_.clear();
        vregMap_.clear();
        // Two-pass: scan headers to pre-create every block (so forward
        // BlockRef operands resolve), then re-walk for bodies.
        std::size_t const bodyStart = lex_.peekPos();
        scanBlockHeaders();
        lex_.setPos(bodyStart);
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            if (!(pk.kind == TokKind::Ident && pk.text == "block")) {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("expected 'block' inside function, got '{}'",
                                 pk.text));
                lex_.take();
                continue;
            }
            parseBlock();
        }
        (void)expect(TokKind::RBrace);
    }

    // Walk the function body counting braces; create every `block ^bN`
    // header in declaration order so forward refs from terminators
    // resolve against pre-created `LirBlockId`s.
    void scanBlockHeaders() {
        int depth = 1;  // inside the function's `{`
        while (depth > 0) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::End) break;
            if (pk.kind == TokKind::RBrace) { lex_.take(); --depth; continue; }
            if (pk.kind == TokKind::LBrace) { lex_.take(); ++depth; continue; }
            if (pk.kind == TokKind::Ident && pk.text == "block" && depth == 1) {
                lex_.take();
                std::uint32_t const slot = parseCaretBlockSlot();
                if (blockMap_.find(slot) == blockMap_.end()) {
                    blockMap_[slot] = builder_.createBlock();
                }
                continue;
            }
            lex_.take();
        }
    }

    [[nodiscard]] std::uint32_t parseCaretBlockSlot() {
        if (!expect(TokKind::Caret)) return 0;
        Tok t = lex_.take();
        // `^b<digits>` arrives as identifier whose first char is `b`.
        if (t.kind != TokKind::Ident || t.text.empty() || t.text[0] != 'b') {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected '^b<int>', got '^{}'", t.text));
            return 0;
        }
        std::uint32_t v = 0;
        for (std::size_t i = 1; i < t.text.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(t.text[i]))) {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("malformed block-slot '^{}'", t.text));
                return 0;
            }
            v = v * 10 + static_cast<std::uint32_t>(t.text[i] - '0');
        }
        return v;
    }

    void parseBlock() {
        (void)expectIdent("block");
        std::uint32_t const slot = parseCaretBlockSlot();
        auto it = blockMap_.find(slot);
        if (it == blockMap_.end()) {
            emit(DiagnosticCode::I_TextUnknownName,
                 std::format("block ^b{} not pre-declared", slot));
            return;
        }
        // Optional `[entry]` marker — informational only; the LIR
        // builder treats `funcBlockAt(0)` as the entry by construction.
        // Validate the marker text loudly so a `[bogus]` doesn't
        // silently round-trip as `[entry]`.
        if (lex_.peek().kind == TokKind::LBracket) {
            lex_.take();
            (void)expectIdent("entry");
            (void)expect(TokKind::RBracket);
        }
        // Successor list `-> [^b..., ...]`. Captured into
        // `currentBlockSuccSlots_` so the terminator dispatch knows
        // whether to call `addReturn` (0 succs), `addBr` (1), or
        // `addCondBr` (2) — addCondBr does not embed BlockRef
        // operands on the inst, so without this list the parser would
        // see zero BlockRefs in the operand list and silently
        // mis-dispatch a CondBr as a Return.
        currentBlockSuccSlots_.clear();
        if (lex_.peek().kind == TokKind::Arrow) {
            lex_.take();
            (void)expect(TokKind::LBracket);
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RBracket) break;
                if (pk.kind == TokKind::Caret) {
                    currentBlockSuccSlots_.push_back(parseCaretBlockSlot());
                    if (lex_.peek().kind == TokKind::Comma) lex_.take();
                } else {
                    lex_.take();  // skip stray
                }
            }
            (void)expect(TokKind::RBracket);
        }
        if (!expect(TokKind::LBrace)) return;
        builder_.beginBlock(it->second);
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            parseInst();
        }
        (void)expect(TokKind::RBrace);
    }

    // ── instructions / operands ────────────────────────────────────
    //
    // Hostile text claiming `%v.50000` while the builder is at vreg 1
    // would, without this cap, allocate ~50000 wasted vregs in a tight
    // loop. The cap is a per-function denial-of-service guard (architect
    // HIGH). 65535 matches the bit-width the LIR substrate's vreg
    // tables size against.
    static constexpr std::uint32_t kMaxVRegIdPerFunction = 65535;

    [[nodiscard]] LirReg lookupOrMintVreg(std::uint32_t id, LirRegClass cls) {
        auto it = vregMap_.find(id);
        if (it != vregMap_.end()) return it->second;
        if (id == 0 || id > kMaxVRegIdPerFunction) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("vreg id {} out of per-function range "
                             "[1, {}]", id, kMaxVRegIdPerFunction));
            return InvalidLirReg;
        }
        // Mint until the builder's nextVReg counter aligns. The builder
        // mints monotonically starting at 1; if text references a vreg
        // id higher than the next-mint, fill the gap with discarded
        // mints of the same class. Filler vregs are unused — their
        // class is therefore inert (no defs / no uses observe it).
        while (true) {
            LirReg const minted = builder_.newVReg(cls);
            vregMap_[static_cast<std::uint32_t>(minted.id)] = minted;
            if (minted.id == id) return minted;
            if (minted.id > id) {
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("vreg id {} already past builder's "
                                 "next-mint {}", id, minted.id));
                return InvalidLirReg;
            }
        }
    }

    // Phys register parsing: an identifier whose text resolves via
    // `schema.registerByName`. Returns nullopt if not a known register.
    [[nodiscard]] std::optional<LirReg> tryParsePhysReg(std::string_view mnem) {
        auto ord = schema_.registerByName(mnem);
        if (!ord.has_value()) return std::nullopt;
        auto const* info = schema_.registerInfo(*ord);
        LirRegClass const cls = (info != nullptr)
            ? static_cast<LirRegClass>(info->regClass)
            : LirRegClass::GPR;
        return makePhysicalReg(*ord, cls);
    }

    // `<reg>` operand: either phys mnemonic OR `%v.<id>:<class>`.
    // Returns the parsed LirReg. Same routine serves both result-
    // position (LHS of `=`) and operand-position; callers disambiguate
    // by reading the result back into the right slot.
    //
    // The `:<class>` suffix is MANDATORY (mirrors the emitter — required
    // for byte-identical round-trip of FPR/VR/Flags vregs that would
    // otherwise demote to GPR silently). Class names: `gpr`/`fpr`/
    // `vr`/`flags`/`none` — see `lirRegClassFromName`.
    [[nodiscard]] LirReg parseRegOperand() {
        Tok pk = lex_.peek();
        if (pk.kind == TokKind::Percent) {
            lex_.take();
            (void)expectIdent("v");
            (void)expect(TokKind::Dot);
            Tok idTok = lex_.take();
            std::uint32_t const id = parseNumber<std::uint32_t>(idTok.text, "vreg id");
            // Mandatory `:<class>` suffix.
            LirRegClass cls = LirRegClass::GPR;
            if (lex_.peek().kind == TokKind::Colon) {
                lex_.take();
                Tok classTok = lex_.take();
                if (auto c = lirRegClassFromName(classTok.text); c.has_value()) {
                    cls = *c;
                } else {
                    emit(DiagnosticCode::I_TextUnknownName,
                         std::format("unknown LirRegClass '{}' after vreg id",
                                     classTok.text));
                }
            } else {
                emit(DiagnosticCode::I_TextMalformed,
                     "expected ':<class>' after vreg id (mandatory for "
                     "lossless round-trip of FPR/VR/Flags vregs)");
            }
            return lookupOrMintVreg(id, cls);
        }
        if (pk.kind == TokKind::Ident) {
            lex_.take();
            if (auto r = tryParsePhysReg(pk.text); r.has_value()) return *r;
            emit(DiagnosticCode::I_TextUnknownName,
                 std::format("unknown register mnemonic '{}'", pk.text));
            return InvalidLirReg;
        }
        emit(DiagnosticCode::I_TextMalformed,
             std::format("expected register, got '{}'", pk.text));
        lex_.take();
        return InvalidLirReg;
    }

    // Parse one operand based on its leading sigil.
    [[nodiscard]] LirOperand parseOperand() {
        Tok pk = lex_.peek();
        switch (pk.kind) {
            case TokKind::Underscore:
                lex_.take();
                return LirOperand{};   // None
            case TokKind::Hash: {
                lex_.take();
                Tok n = lex_.take();
                bool const neg = (n.kind == TokKind::Minus);
                if (neg) n = lex_.take();
                std::int32_t v = parseNumber<std::int32_t>(n.text, "ImmInt");
                if (neg) v = -v;
                return LirOperand::makeImmInt32(v);
            }
            case TokKind::Caret: {
                std::uint32_t const slot = parseCaretBlockSlot();
                auto it = blockMap_.find(slot);
                if (it == blockMap_.end()) {
                    // Forward-or-cross-function ref to a block
                    // `scanBlockHeaders` didn't pre-create. Diagnose
                    // loudly — silent fallback to slot 0 would let
                    // the CFG silently glue a wrong target into the
                    // terminator dispatch downstream.
                    emit(DiagnosticCode::I_TextUnknownName,
                         std::format("block ^b{} referenced but never "
                                     "declared in this function", slot));
                    return LirOperand::makeBlockRef(0);
                }
                // Store the module-wide `LirBlockId.v` (matches the
                // `addBr`/`addCondBr` storage convention so a re-emit's
                // `op.blockSlot - fnEntryV` recovers the same within-
                // function slot we just consumed). The reverse map for
                // the terminator dispatch is the same `blockMap_`.
                return LirOperand::makeBlockRef(it->second.v);
            }
            case TokKind::At: {
                lex_.take();
                Tok n = lex_.take();
                std::uint32_t const v = parseNumber<std::uint32_t>(n.text, "SymbolRef id");
                return LirOperand::makeSymbolRef(v);
            }
            case TokKind::Star: {
                lex_.take();
                Tok n = lex_.take();
                return LirOperand::makeMemBase(parseNumber<std::uint32_t>(n.text, "MemBase scale"));
            }
            case TokKind::Plus:
            case TokKind::Minus: {
                bool const neg = (pk.kind == TokKind::Minus);
                lex_.take();
                Tok n = lex_.take();
                std::int32_t v = parseNumber<std::int32_t>(n.text, "MemOffset");
                if (neg) v = -v;
                return LirOperand::makeMemOffset(v);
            }
            case TokKind::Ident: {
                // `lit#<N>` literal-index, `byval#<N>` by-value-stack-agg size
                // marker (FC12a-struct), OR a phys-reg mnemonic.
                if (pk.text == "lit") {
                    lex_.take();
                    (void)expect(TokKind::Hash);
                    Tok n = lex_.take();
                    return LirOperand::makeLiteralIndex(
                        parseNumber<std::uint32_t>(n.text, "LiteralIndex"));
                }
                if (pk.text == "byval") {
                    lex_.take();
                    (void)expect(TokKind::Hash);
                    Tok n = lex_.take();
                    return LirOperand::makeByValueStackAgg(
                        parseNumber<std::uint32_t>(n.text, "ByValueStackAgg bytes"));
                }
                return LirOperand::makeReg(parseRegOperand());
            }
            case TokKind::Percent:
                return LirOperand::makeReg(parseRegOperand());
            default:
                emit(DiagnosticCode::I_TextMalformed,
                     std::format("unexpected operand token '{}'", pk.text));
                lex_.take();
                return LirOperand{};
        }
    }

    void parseInst() {
        // Single-pass lookahead for the optional `<result> =` prefix.
        // The result form is either `<phys-mnemonic> = ...` (2 tokens
        // before `=`) or `%v.<id>:<class> = ...` (7 tokens before `=`).
        // Probe up to 8 tokens for an `=` that isn't preceded by a
        // statement terminator (`;`/`}`/EOF). Replaces the cycle-2-
        // landing's two nested save/restore lookaheads (simplifier F1).
        std::size_t const save = lex_.peekPos();
        bool hasResult = false;
        for (int i = 0; i < 8; ++i) {
            Tok t = lex_.take();
            if (t.kind == TokKind::Eq)        { hasResult = true; break; }
            if (t.kind == TokKind::Semicolon
             || t.kind == TokKind::RBrace
             || t.kind == TokKind::End)        break;
        }
        lex_.setPos(save);
        LirReg result = InvalidLirReg;
        if (hasResult) {
            result = parseRegOperand();
            (void)expect(TokKind::Eq);
        }
        // Mnemonic.
        Tok mnem = lex_.take();
        if (mnem.kind != TokKind::Ident) {
            emit(DiagnosticCode::I_TextMalformed,
                 std::format("expected opcode mnemonic, got '{}'", mnem.text));
            // Skip the rest of the line by consuming until `;` or
            // newline-equivalent (next inst's leading token).
            skipToNextInstOrBlockEnd();
            return;
        }
        auto opOpt = schema_.opcodeByMnemonic(mnem.text);
        if (!opOpt.has_value()) {
            emit(DiagnosticCode::I_TextUnknownName,
                 std::format("unknown opcode mnemonic '{}' for target '{}'",
                             mnem.text, schema_.name()));
            skipToNextInstOrBlockEnd();
            return;
        }
        std::uint16_t const op = *opOpt;
        // Operand list — terminated by `;`.
        std::vector<LirOperand> operands;
        Tok pk = lex_.peek();
        if (pk.kind != TokKind::Semicolon) {
            operands.push_back(parseOperand());
            while (true) {
                Tok pk2 = lex_.peek();
                if (pk2.kind != TokKind::Comma) break;
                lex_.take();
                operands.push_back(parseOperand());
            }
        }
        // `; payload=N flags=M` (MANDATORY per cycle-1 contract:
        // unconditional emission). Earlier draft accepted the keywords
        // as optional and zero-defaulted them — that silently produced
        // `flags=0` on truncated input. Now both keywords AND the
        // semicolon are required so a malformed-tail input is loud.
        std::uint32_t payload = 0;
        std::uint8_t  flags   = 0;
        if (!expect(TokKind::Semicolon)) {
            skipToNextInstOrBlockEnd();
            return;
        }
        if (!expectIdent("payload")) { skipToNextInstOrBlockEnd(); return; }
        (void)expect(TokKind::Eq);
        {
            Tok n = lex_.take();
            payload = parseNumber<std::uint32_t>(n.text, "payload");
        }
        if (!expectIdent("flags"))   { skipToNextInstOrBlockEnd(); return; }
        (void)expect(TokKind::Eq);
        {
            Tok n = lex_.take();
            flags = parseNumber<std::uint8_t>(n.text, "flags");
        }
        // Dispatch by terminator-ness.
        auto const* info = schema_.opcodeInfo(op);
        bool const isTerm = (info != nullptr && info->isTerminator());
        if (isTerm) {
            // Schema-driven dispatch via `info->terminatorKind`. Earlier
            // draft used an operand-emptiness heuristic ("0 successors +
            // 0 operands + result=None → Unreachable, else Return")
            // which silently mis-classified any opcode whose `ret`
            // takes 0 operands (e.g. a future void-only-return target).
            // Now the JSON declares it explicitly and the loader's
            // `validate()` enforces `isTerminator ↔ terminatorKind`.
            std::vector<LirOperand> nonBlock;
            for (auto const& o : operands) {
                if (o.kind != LirOperandKind::BlockRef) {
                    nonBlock.push_back(o);
                }
            }
            // Resolve block-header successor slots into tagged ids
            // (loud on miss — CFG-corruption guard).
            auto resolveTargets = [&]() -> std::optional<std::vector<LirBlockId>> {
                std::vector<LirBlockId> ts;
                ts.reserve(currentBlockSuccSlots_.size());
                for (std::uint32_t slot : currentBlockSuccSlots_) {
                    auto it = blockMap_.find(slot);
                    if (it == blockMap_.end()) {
                        emit(DiagnosticCode::I_TextUnknownName,
                             std::format("block header successor ^b{} not "
                                         "declared in this function "
                                         "(CFG-corruption guard)", slot));
                        return std::nullopt;
                    }
                    ts.push_back(it->second);
                }
                return ts;
            };
            switch (info->terminatorKind) {
                case TargetTerminatorKind::Return:
                    builder_.addReturn(op, nonBlock, payload, flags);
                    break;
                case TargetTerminatorKind::Unreachable:
                    builder_.addUnreachable(op, payload, flags);
                    break;
                case TargetTerminatorKind::Br: {
                    auto ts = resolveTargets();
                    if (!ts.has_value() || ts->size() != 1) {
                        if (ts.has_value()) {
                            emit(DiagnosticCode::I_TextMalformed,
                                 std::format("Br opcode '{}' requires 1 "
                                             "successor; block declared {}",
                                             mnem.text, ts->size()));
                        }
                        return;
                    }
                    builder_.addBr(op, (*ts)[0], payload, flags);
                    break;
                }
                case TargetTerminatorKind::CondBr: {
                    auto ts = resolveTargets();
                    if (!ts.has_value() || ts->size() != 2) {
                        if (ts.has_value()) {
                            emit(DiagnosticCode::I_TextMalformed,
                                 std::format("CondBr opcode '{}' requires 2 "
                                             "successors; block declared {}",
                                             mnem.text, ts->size()));
                        }
                        return;
                    }
                    builder_.addCondBr(op, nonBlock, (*ts)[0], (*ts)[1],
                                       payload, flags);
                    break;
                }
                case TargetTerminatorKind::Switch:
                    emit(DiagnosticCode::I_TextMalformed,
                         std::format("Switch terminator opcode '{}' not yet "
                                     "supported in `.dsslir` round-trip "
                                     "(reserved for LIR Switch lowering)",
                                     mnem.text));
                    return;
                case TargetTerminatorKind::None:
                    // Unreachable: `isTerm` derives from `terminatorKind
                    // != None`, so this arm is unreachable by
                    // construction. Kept as a `default`-style fail-loud
                    // guard in case a future hand-built `Lir` ever lies
                    // about its opcode's role.
                    emit(DiagnosticCode::I_TextMalformed,
                         std::format("opcode '{}' has terminatorKind=none "
                                     "but reached terminator dispatch "
                                     "(substrate invariant violation)",
                                     mnem.text));
                    return;
            }
        } else {
            builder_.addInst(op, result, operands, payload, flags);
        }
    }

    // Consume the rest of the current malformed inst — through the
    // `;` AND the `payload=N flags=M` tail — so the next iteration of
    // the inst loop sees the start of the next inst, not the partial
    // tail of the broken one. Without consuming the tail, downstream
    // parseInst would peek `payload` / `flags`, hit "unknown
    // mnemonic" (since neither is in the opcode table), and emit a
    // SECOND spurious diagnostic per malformed inst.
    void skipToNextInstOrBlockEnd() {
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::End || pk.kind == TokKind::RBrace) break;
            if (pk.kind == TokKind::Semicolon) {
                lex_.take();
                // Tail: `payload=N flags=M`. Use peekIdent + take pairs
                // so non-erroring (the diagnostic for the malformed
                // inst was already emitted by the caller).
                if (peekIdent("payload")) {
                    lex_.take();
                    if (lex_.peek().kind == TokKind::Eq) lex_.take();
                    if (lex_.peek().kind == TokKind::Integer) lex_.take();
                }
                if (peekIdent("flags")) {
                    lex_.take();
                    if (lex_.peek().kind == TokKind::Eq) lex_.take();
                    if (lex_.peek().kind == TokKind::Integer) lex_.take();
                }
                break;
            }
            lex_.take();
        }
    }

    // ── finalize ───────────────────────────────────────────────────
    [[nodiscard]] std::unique_ptr<LirParseResult>
    finalize(std::size_t errBefore) {
        if (errors_) return makeEmptyResult();
        Lir module = std::move(builder_).finish();
        auto result = std::make_unique<LirParseResult>(
            std::move(module), std::move(symbolNames_));
        // Verify-on-load: LIR-only rules (the only ones meaningful
        // without an MIR cross-reference).
        (void)verifyLirText(result->lir, schema_, reporter_);
        result->ok = (reporter_.errorCount() == errBefore);
        return result;
    }
};

} // namespace

std::unique_ptr<LirParseResult>
parseLir(std::string_view text, TargetSchema const& schema,
         DiagnosticReporter& reporter) {
    Parser p{text, schema, reporter};
    return p.run();
}

} // namespace dss
