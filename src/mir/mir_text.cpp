#include "mir/mir_text.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"  // callConvName / callConvFromName
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_literal_pool.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace dss {

// ── shared helpers ────────────────────────────────────────────────────

namespace {

constexpr int kVersion = 1;

[[nodiscard]] std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\t') { out += "\\t"; }
        else { out += c; }
    }
    out += '"';
    return out;
}

[[nodiscard]] std::string_view primName(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool: return "bool";
        case TypeKind::Char: return "char";
        case TypeKind::Byte: return "byte";
        case TypeKind::Void: return "void";
        case TypeKind::I8:   return "i8";
        case TypeKind::I16:  return "i16";
        case TypeKind::I32:  return "i32";
        case TypeKind::I64:  return "i64";
        case TypeKind::I128: return "i128";
        case TypeKind::U8:   return "u8";
        case TypeKind::U16:  return "u16";
        case TypeKind::U32:  return "u32";
        case TypeKind::U64:  return "u64";
        case TypeKind::U128: return "u128";
        case TypeKind::F16:  return "f16";
        case TypeKind::F32:  return "f32";
        case TypeKind::F64:  return "f64";
        case TypeKind::F128: return "f128";
        default: return {};
    }
}

[[nodiscard]] std::optional<TypeKind> primKindFromName(std::string_view s) noexcept {
    if (s == "bool") return TypeKind::Bool;
    if (s == "char") return TypeKind::Char;
    if (s == "byte") return TypeKind::Byte;
    if (s == "void") return TypeKind::Void;
    if (s == "i8")   return TypeKind::I8;
    if (s == "i16")  return TypeKind::I16;
    if (s == "i32")  return TypeKind::I32;
    if (s == "i64")  return TypeKind::I64;
    if (s == "i128") return TypeKind::I128;
    if (s == "u8")   return TypeKind::U8;
    if (s == "u16")  return TypeKind::U16;
    if (s == "u32")  return TypeKind::U32;
    if (s == "u64")  return TypeKind::U64;
    if (s == "u128") return TypeKind::U128;
    if (s == "f16")  return TypeKind::F16;
    if (s == "f32")  return TypeKind::F32;
    if (s == "f64")  return TypeKind::F64;
    if (s == "f128") return TypeKind::F128;
    return std::nullopt;
}

[[nodiscard]] std::string_view markerName(StructCfMarker m) noexcept {
    switch (m) {
        case StructCfMarker::Linear:     return "linear";
        case StructCfMarker::EntryBlock: return "entry";
        case StructCfMarker::ExitBlock:  return "exit";
        case StructCfMarker::LoopHeader: return "loopheader";
        case StructCfMarker::LoopLatch:  return "looplatch";
        case StructCfMarker::LoopExit:   return "loopexit";
        case StructCfMarker::IfThen:     return "ifthen";
        case StructCfMarker::IfElse:     return "ifelse";
        case StructCfMarker::IfJoin:     return "ifjoin";
        case StructCfMarker::SwitchHead: return "switchhead";
        case StructCfMarker::SwitchCase: return "switchcase";
        case StructCfMarker::SwitchJoin: return "switchjoin";
    }
    return "linear";
}

[[nodiscard]] std::optional<StructCfMarker> markerFromName(std::string_view s) noexcept {
    if (s == "linear")     return StructCfMarker::Linear;
    if (s == "entry")      return StructCfMarker::EntryBlock;
    if (s == "exit")       return StructCfMarker::ExitBlock;
    if (s == "loopheader") return StructCfMarker::LoopHeader;
    if (s == "looplatch")  return StructCfMarker::LoopLatch;
    if (s == "loopexit")   return StructCfMarker::LoopExit;
    if (s == "ifthen")     return StructCfMarker::IfThen;
    if (s == "ifelse")     return StructCfMarker::IfElse;
    if (s == "ifjoin")     return StructCfMarker::IfJoin;
    if (s == "switchhead") return StructCfMarker::SwitchHead;
    if (s == "switchcase") return StructCfMarker::SwitchCase;
    if (s == "switchjoin") return StructCfMarker::SwitchJoin;
    return std::nullopt;
}

[[nodiscard]] std::optional<MirOpcode> opcodeFromMnemonic(std::string_view s) noexcept {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(MirOpcode::Count_); ++i) {
        auto const op = static_cast<MirOpcode>(i);
        if (opcodeInfo(op).mnemonic == s) return op;
    }
    return std::nullopt;
}

// CallConv name mapping previously hand-rolled here (and in
// hir_text.cpp) — duplication caught in the 2026-06-02 cross-
// codebase audit. Call sites now use `callConvName(cc)` /
// `callConvFromName(s)` directly from the single source of truth
// (`kCallConvTable` in target_schema.hpp).

// ── Emitter ──────────────────────────────────────────────────────────

class Emitter {
public:
    Emitter(Mir const& m, MirTextContext const& ctx, DiagnosticReporter& reporter)
        : mir_(m), ctx_(ctx), reporter_(reporter) {}

    [[nodiscard]] std::string run() {
        // Collect symbols referenced by the module: functions, globals,
        // and GlobalAddr instruction payloads. Each gets a stable handle
        // (its SymbolId.v value, so the parser can re-mint the same id).
        collectSymbols();
        out_ += std::format("dssir {}\n", kVersion);
        emitSymbolsPreamble();
        out_ += "module {\n";
        for (std::uint32_t i = 0; i < mir_.moduleGlobalCount(); ++i) {
            emitGlobal(mir_.globalAt(i));
        }
        for (std::uint32_t i = 0; i < mir_.moduleFuncCount(); ++i) {
            emitFunction(mir_.funcAt(i));
        }
        out_ += "}\n";
        return std::move(out_);
    }

private:
    Mir const&            mir_;
    MirTextContext const& ctx_;
    DiagnosticReporter&   reporter_;
    std::string           out_;
    std::vector<std::uint32_t> symOrder_;            // declaration-order list
    std::unordered_map<std::uint32_t, bool> symSet_; // dedup set

    void report(std::string what, DiagnosticSeverity sev = DiagnosticSeverity::Warning) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::I_TextMalformed;
        d.severity = sev;
        d.actual   = std::move(what);
        reporter_.report(std::move(d));
    }

    void noteSymbol(std::uint32_t v) {
        if (v == 0) return;
        if (symSet_.emplace(v, true).second) symOrder_.push_back(v);
    }

    void collectSymbols() {
        for (std::uint32_t i = 0; i < mir_.moduleFuncCount(); ++i) {
            noteSymbol(mir_.funcSymbol(mir_.funcAt(i)).v);
        }
        for (std::uint32_t i = 0; i < mir_.moduleGlobalCount(); ++i) {
            noteSymbol(mir_.globalSymbol(mir_.globalAt(i)).v);
        }
        for (std::uint32_t i = 1; i < mir_.instCount(); ++i) {
            MirInstId const id{i, mir_.id().v};
            if (mir_.instOpcode(id) == MirOpcode::GlobalAddr) {
                noteSymbol(mir_.globalAddrSymbol(id).v);
            }
        }
    }

    void emitSymbolsPreamble() {
        if (symOrder_.empty()) return;
        out_ += "symbols {\n";
        for (std::uint32_t v : symOrder_) {
            std::string_view name;
            if (ctx_.symbolNames != nullptr && v < ctx_.symbolNames->size()) {
                name = (*ctx_.symbolNames)[v];
            }
            out_ += std::format("  %{} {}\n", v, quote(name));
        }
        out_ += "}\n";
    }

    // Recursive structural type emitter — mirrors the HIR text emitter's
    // discipline. The grammar must stay in sync with `parseType` below.
    bool internerWarned_ = false;
    void appendType(TypeId t) {
        if (!t.valid()) { out_ += "invalid"; return; }
        if (ctx_.interner == nullptr) {
            if (!internerWarned_) {
                report("no TypeInterner supplied; types render as '?'",
                       DiagnosticSeverity::Warning);
                internerWarned_ = true;
            }
            out_ += '?';
            return;
        }
        TypeInterner const& in = *ctx_.interner;
        auto args = [&](std::span<TypeId const> ops) {
            bool first = true;
            for (TypeId o : ops) {
                if (!first) out_ += ", ";
                appendType(o);
                first = false;
            }
        };
        switch (in.kind(t)) {
            case TypeKind::Ptr:      out_ += "ptr<";      appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Ref:      out_ += "ref<";      appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Nullable: out_ += "nullable<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Optional: out_ += "optional<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Slice:    out_ += "slice<";    appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Array:
                out_ += "arr<"; appendType(in.operands(t)[0]);
                out_ += std::format(", {}>", in.scalars(t)[0]);
                return;
            case TypeKind::Tuple:
                out_ += "tuple<"; args(in.operands(t)); out_ += '>'; return;
            case TypeKind::Struct:
                out_ += "struct "; out_ += quote(in.name(t)); out_ += " {";
                args(in.operands(t)); out_ += '}'; return;
            case TypeKind::Union:
                out_ += "union "; out_ += quote(in.name(t)); out_ += " {";
                args(in.operands(t)); out_ += '}'; return;
            case TypeKind::Enum: {
                out_ += "enum "; out_ += quote(in.name(t));
                auto sc = in.scalars(t);
                if (!sc.empty() && static_cast<TypeKind>(sc[0]) != TypeKind::I32) {
                    out_ += " : ";
                    out_ += std::to_string(sc[0]);
                }
                return;
            }
            case TypeKind::FnSig: {
                out_ += "fn(";
                args(in.fnParams(t));
                out_ += ") -> ";
                appendType(in.fnResult(t));
                auto sc = in.scalars(t);
                if (!sc.empty()) {
                    auto const cc = static_cast<CallConv>(sc[0]);
                    if (cc != CallConv::CcSysV) {
                        out_ += " cc "; out_ += callConvName(cc);
                    }
                }
                return;
            }
            case TypeKind::Extension: {
                report("MIR carries a TypeKind::Extension type — the HIR→MIR "
                       "boundary should have resolved it", DiagnosticSeverity::Warning);
                out_ += "ext "; out_ += quote(in.name(t));
                return;
            }
            default: {
                std::string_view const p = primName(in.kind(t));
                if (!p.empty()) out_ += p;
                else { report("unprintable type kind"); out_ += '?'; }
                return;
            }
        }
    }

    // Render a `MirLiteralValue` inline. Format mirrors HIR's: `lit
    // <variant-tag> <value>` where `<variant-tag>` disambiguates the
    // variant arm (int / uint / float / bool / str / agg).
    void appendLiteral(MirLiteralValue const& lv) {
        out_ += "lit ";
        std::visit([&](auto const& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out_ += "monostate";
            } else if constexpr (std::is_same_v<T, bool>) {
                out_ += "bool ";
                out_ += v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out_ += std::format("int {}", v);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                out_ += std::format("uint {}", v);
            } else if constexpr (std::is_same_v<T, double>) {
                out_ += std::format("float {}", v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                out_ += "str "; out_ += quote(v);
            } else if constexpr (std::is_same_v<T, MirAggregateValue>) {
                out_ += "agg {";
                bool first = true;
                for (auto const& f : v.fields) {
                    if (!first) out_ += ", ";
                    appendLiteral(f);
                    first = false;
                }
                out_ += '}';
            } else if constexpr (std::is_same_v<T, MirSymbolAddrValue>) {
                // F5: link-time symbol-address literal (`&sym [+ addend]`).
                out_ += std::format("symaddr %{}", v.symbol);
                if (v.addend != 0) out_ += std::format(" + {}", v.addend);
            }
        }, lv.value);
        out_ += " : ";
        std::string_view const p = primName(lv.core);
        if (!p.empty()) out_ += p;
        else if (lv.core == TypeKind::Struct) out_ += "struct";
        else if (lv.core == TypeKind::Union)  out_ += "union";
        else if (lv.core == TypeKind::Array)  out_ += "array";
        else if (lv.core == TypeKind::Ptr)    out_ += "ptr";
        else if (lv.core == TypeKind::Ref)    out_ += "ref";
        else if (lv.core == TypeKind::Enum)   out_ += "enum";
        else { report("unprintable literal core kind"); out_ += '?'; }
    }

    void emitGlobal(MirGlobalId g) {
        out_ += std::format("  global %{} : ", mir_.globalSymbol(g).v);
        appendType(mir_.globalType(g));
        std::uint32_t const litIdx = mir_.globalInitLiteralIndex(g);
        MirFuncId const initFn = mir_.globalInitFunc(g);
        if (litIdx != UINT32_MAX) {
            out_ += " = ";
            appendLiteral(mir_.literalValue(litIdx));
        } else if (initFn.valid()) {
            out_ += std::format(" = initfunc %f{}", initFn.v);
        } else {
            out_ += " = zero";
        }
        out_ += '\n';
    }

    void emitFunction(MirFuncId f) {
        out_ += std::format("  function %{} : ", mir_.funcSymbol(f).v);
        appendType(mir_.funcSignature(f));
        out_ += " {\n";
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            emitBlock(mir_.funcBlockAt(f, bi));
        }
        out_ += "  }\n";
    }

    void emitBlock(MirBlockId b) {
        out_ += std::format("    block %b{}", b.v);
        StructCfMarker const m = mir_.blockMarker(b);
        if (m != StructCfMarker::Linear) {
            out_ += std::format(" [{}]", markerName(m));
        }
        out_ += " {\n";
        std::uint32_t const n = mir_.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            emitInst(mir_.blockInstAt(b, i), b);
        }
        out_ += "    }\n";
    }

    void emitInst(MirInstId id, MirBlockId block) {
        out_ += "      ";
        MirOpcode const op = mir_.instOpcode(id);
        MirOpcodeInfo const& info = opcodeInfo(op);
        bool const hasResult = mir_.instType(id).valid();
        if (hasResult) {
            out_ += std::format("%v{} = ", id.v);
        }
        out_ += info.mnemonic;
        if (hasResult) {
            out_ += " : ";
            appendType(mir_.instType(id));
        }
        // Per-opcode operand rendering.
        switch (op) {
            case MirOpcode::Const: {
                out_ += " (";
                appendLiteral(mir_.literalValue(mir_.constLiteralIndex(id)));
                out_ += ')';
                break;
            }
            case MirOpcode::Arg: {
                // Print the class ordinal; append the flat call-operand
                // position ONLY when it differs (single-class signatures keep
                // the golden `(N)` form; mixed-class emits `(ord, pos)`) so the
                // full payload survives a text round-trip (arg_payload.hpp).
                std::uint32_t const ord = mir_.argIndex(id);
                std::uint32_t const pos = mir_.argPosition(id);
                if (pos == ord) out_ += std::format(" ({})", ord);
                else            out_ += std::format(" ({}, {})", ord, pos);
                break;
            }
            case MirOpcode::GlobalAddr: {
                out_ += std::format(" (%{})", mir_.globalAddrSymbol(id).v);
                break;
            }
            case MirOpcode::IntrinsicCall: {
                out_ += std::format(" (intrinsic {}", mir_.intrinsicId(id));
                for (MirInstId const op2 : mir_.instOperands(id)) {
                    out_ += std::format(", %v{}", op2.v);
                }
                out_ += ')';
                break;
            }
            case MirOpcode::Phi: {
                out_ += " [";
                bool first = true;
                for (MirPhiIncoming const& inc : mir_.phiIncomings(id)) {
                    if (!first) out_ += ", ";
                    out_ += std::format("(%v{}, %b{})", inc.value.v, inc.pred.v);
                    first = false;
                }
                out_ += ']';
                break;
            }
            case MirOpcode::Br: {
                auto succs = mir_.blockSuccessors(block);
                if (!succs.empty()) out_ += std::format(" %b{}", succs[0].v);
                break;
            }
            case MirOpcode::CondBr: {
                auto operands = mir_.instOperands(id);
                auto succs = mir_.blockSuccessors(block);
                if (!operands.empty()) {
                    out_ += std::format(" %v{}", operands[0].v);
                }
                if (succs.size() >= 2) {
                    out_ += std::format(" %b{} %b{}", succs[0].v, succs[1].v);
                }
                break;
            }
            case MirOpcode::Switch: {
                auto operands = mir_.instOperands(id);
                auto succs = mir_.blockSuccessors(block);
                // operands[0] = discriminant; operands[1..N] = case constants.
                // succs[0..N-1] = case targets; succs.back() = default.
                if (!operands.empty()) {
                    out_ += std::format(" %v{}", operands[0].v);
                }
                out_ += " {";
                std::size_t const ncases = (succs.size() > 0) ? succs.size() - 1 : 0;
                for (std::size_t i = 0; i < ncases; ++i) {
                    if (i > 0) out_ += ", ";
                    out_ += std::format("case %v{} -> %b{}",
                        (i + 1 < operands.size()) ? operands[i + 1].v : 0,
                        succs[i].v);
                }
                if (!succs.empty()) {
                    if (ncases > 0) out_ += ", ";
                    out_ += std::format("default -> %b{}", succs.back().v);
                }
                out_ += '}';
                break;
            }
            case MirOpcode::IndirectBr: {
                // D-CSUBSET-COMPUTED-GOTO: render `indirectbr %v{addr} { %b.. }` —
                // the address operand then the full address-taken successor list.
                auto operands = mir_.instOperands(id);
                auto succs = mir_.blockSuccessors(block);
                if (!operands.empty()) out_ += std::format(" %v{}", operands[0].v);
                out_ += " { ";
                bool first = true;
                for (MirBlockId const b : succs) {
                    if (!first) out_ += ", ";
                    out_ += std::format("%b{}", b.v);
                    first = false;
                }
                out_ += " }";
                break;
            }
            case MirOpcode::Return: {
                // FC7 C1c: a by-value struct returned IN REGISTERS carries N piece
                // operands — emit ALL of them (space-separated), not just operand 0,
                // so the round-trip contract (emitMir∘parseMir∘emitMir == emitMir)
                // holds for multi-piece struct returns. Scalar = 1, void = 0.
                for (auto const& o : mir_.instOperands(id)) {
                    out_ += std::format(" %v{}", o.v);
                }
                break;
            }
            case MirOpcode::Unreachable:
                break;
            case MirOpcode::SehTryBegin: {
                // c115 SEH: `seh_try_begin <region> %b<try> %b<filter>`.
                auto succs = mir_.blockSuccessors(block);
                out_ += std::format(" {}", mir_.instPayload(id));
                if (succs.size() >= 2) {
                    out_ += std::format(" %b{} %b{}", succs[0].v, succs[1].v);
                }
                break;
            }
            case MirOpcode::SehFilterReturn: {
                // `seh_filter_return <region> %v<val> %b<handler>`.
                auto operands = mir_.instOperands(id);
                auto succs = mir_.blockSuccessors(block);
                out_ += std::format(" {}", mir_.instPayload(id));
                if (!operands.empty()) out_ += std::format(" %v{}", operands[0].v);
                if (!succs.empty())    out_ += std::format(" %b{}", succs[0].v);
                break;
            }
            case MirOpcode::SehTryEnd:
                // `seh_try_end <region>` — the payload is the region id.
                out_ += std::format(" {}", mir_.instPayload(id));
                break;
            default: {
                // Generic: render all operands.
                auto operands = mir_.instOperands(id);
                if (!operands.empty()) {
                    out_ += " (";
                    bool first = true;
                    for (MirInstId const op2 : operands) {
                        if (!first) out_ += ", ";
                        out_ += std::format("%v{}", op2.v);
                        first = false;
                    }
                    out_ += ')';
                }
                // Payload tail (for ExtractValue / InsertValue which
                // pack indices into Const operands; here we just expose
                // the raw payload integer if non-zero and not already
                // covered).
                std::uint32_t const payload = mir_.instPayload(id);
                if (op == MirOpcode::BlockAddress) {
                    // D-CSUBSET-COMPUTED-GOTO: the payload is the target BLOCK id —
                    // render it as `%b{}` (a raw integer would read as a meaningless
                    // value and break the block-relative reading).
                    out_ += std::format(" %b{}", payload);
                } else if (op == MirOpcode::ByValueStackArg) {
                    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the payload
                    // PACKS the byte size (low 30 bits) + the exhaust class (high 2) —
                    // print the unpacked fields, not the raw integer (which reads as a
                    // ~2.1e9 garbage value when an exhaust bit is set).
                    out_ += std::format(
                        " size {} exhaust {}",
                        payload & kByValueStackArgSizeMask,
                        (payload >> kByValueStackArgExhaustShift) & 0x3u);
                } else if (payload != 0
                 && op != MirOpcode::ExtractValue
                 && op != MirOpcode::InsertValue) {
                    out_ += std::format(" payload {}", payload);
                }
                break;
            }
        }
        out_ += '\n';
    }
};

} // namespace

std::string emitMir(Mir const& mir, MirTextContext const& ctx,
                    DiagnosticReporter& reporter) {
    Emitter e{mir, ctx, reporter};
    return e.run();
}

// ── Lexer ────────────────────────────────────────────────────────────

namespace {

enum class TokKind {
    End,
    Ident,
    Integer,
    Float,
    String,
    Percent,        // `%`
    LBrace, RBrace,
    LParen, RParen,
    LBracket, RBracket,
    LAngle, RAngle,
    Colon,
    Comma,
    Arrow,          // `->`
    Eq,
    Dot,            // `.` (for `icmp.eq` etc.)
    Minus,
    Unknown,
};

struct Tok {
    TokKind     kind = TokKind::End;
    std::string text;
    std::size_t off  = 0;
};

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    Tok take() {
        skipWhitespaceAndComments();
        Tok t;
        t.off = pos_;
        if (pos_ >= text_.size()) { t.kind = TokKind::End; return t; }
        char const c = text_[pos_];
        if (c == '%') { ++pos_; t.kind = TokKind::Percent; return t; }
        if (c == '{') { ++pos_; t.kind = TokKind::LBrace;  return t; }
        if (c == '}') { ++pos_; t.kind = TokKind::RBrace;  return t; }
        if (c == '(') { ++pos_; t.kind = TokKind::LParen;  return t; }
        if (c == ')') { ++pos_; t.kind = TokKind::RParen;  return t; }
        if (c == '[') { ++pos_; t.kind = TokKind::LBracket; return t; }
        if (c == ']') { ++pos_; t.kind = TokKind::RBracket; return t; }
        if (c == '<') { ++pos_; t.kind = TokKind::LAngle;  return t; }
        if (c == '>') { ++pos_; t.kind = TokKind::RAngle;  return t; }
        if (c == ':') { ++pos_; t.kind = TokKind::Colon;   return t; }
        if (c == ',') { ++pos_; t.kind = TokKind::Comma;   return t; }
        if (c == '=') { ++pos_; t.kind = TokKind::Eq;      return t; }
        if (c == '.') { ++pos_; t.kind = TokKind::Dot;     return t; }
        if (c == '-') {
            if (pos_ + 1 < text_.size() && text_[pos_ + 1] == '>') {
                pos_ += 2; t.kind = TokKind::Arrow; return t;
            }
            // Negative numeric literal — fall through to number scan
            // by including the '-' in the text.
            std::size_t const start = pos_;
            ++pos_;
            while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_]))
                                         || text_[pos_] == '.'
                                         || text_[pos_] == 'e' || text_[pos_] == 'E'
                                         || text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            t.text = std::string(text_.substr(start, pos_ - start));
            t.kind = (t.text.find('.') != std::string::npos
                   || t.text.find('e') != std::string::npos
                   || t.text.find('E') != std::string::npos)
                ? TokKind::Float : TokKind::Integer;
            return t;
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
            while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_]))
                                         || text_[pos_] == '.'
                                         || text_[pos_] == 'e' || text_[pos_] == 'E'
                                         || text_[pos_] == '+' || text_[pos_] == '-')) {
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
                && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_')) {
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

    Tok peek() {
        std::size_t const save = pos_;
        Tok t = take();
        pos_ = save;
        return t;
    }

    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
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

// ── Parser ───────────────────────────────────────────────────────────

class Parser {
public:
    Parser(std::string_view text, CompilationUnitId cuId, DiagnosticReporter& reporter)
        : lex_(text), reporter_(reporter), cuId_(cuId), interner_(cuId) {}

    [[nodiscard]] std::unique_ptr<MirParseResult> run() {
        if (!expectIdent("dssir")) return makeEmptyResult();
        Tok const ver = lex_.take();
        if (ver.kind != TokKind::Integer || ver.text != "1") {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::I_TextVersionMismatch;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format("expected version 1, got '{}'", ver.text);
            reporter_.report(std::move(d));
            return makeEmptyResult();
        }
        // Optional `symbols { ... }` preamble.
        if (peekIdent("symbols")) {
            parseSymbolsPreamble();
        }
        if (!expectIdent("module")) return makeEmptyResult();
        if (!expect(TokKind::LBrace)) return makeEmptyResult();
        // Body: zero or more `global` / `function` items. Panic-mode
        // recovery: on an unexpected token, emit ONE diagnostic and
        // skip until the next `global`/`function` keyword or the
        // closing `}` — avoids per-token cascade after a parse
        // failure inside `parseFunction` / `parseGlobal` returned
        // mid-production.
        while (true) {
            Tok t = lex_.peek();
            if (t.kind == TokKind::RBrace || t.kind == TokKind::End) break;
            if (t.kind == TokKind::Ident && t.text == "global") {
                lex_.take();
                parseGlobal();
            } else if (t.kind == TokKind::Ident && t.text == "function") {
                lex_.take();
                parseFunction();
            } else {
                emitMalformed(std::format("expected 'global' or 'function', got '{}'", t.text));
                // Skip tokens until we find a recovery anchor.
                while (true) {
                    Tok pk = lex_.peek();
                    if (pk.kind == TokKind::End) break;
                    if (pk.kind == TokKind::RBrace) break;
                    if (pk.kind == TokKind::Ident
                     && (pk.text == "global" || pk.text == "function")) break;
                    lex_.take();
                }
            }
        }
        (void)expect(TokKind::RBrace);
        return finalize();
    }

private:
    Lexer                lex_;
    DiagnosticReporter&  reporter_;
    CompilationUnitId    cuId_;
    TypeInterner         interner_;
    MirBuilder           builder_;
    std::vector<std::string> symbolNames_;     // SymbolId.v → name
    std::unordered_map<std::uint32_t, MirBlockId> blockMap_;  // text slot v → builder block id
    std::unordered_map<std::uint32_t, MirInstId>  valueMap_;  // text slot v → builder inst id
    std::unordered_map<std::uint32_t, MirFuncId>  funcMap_;   // text slot v → builder func id
    // Globals whose `initfunc` references a function-text-slot that
    // wasn't yet parsed at the time the global was declared. Resolved
    // at finalize() by replaying the global with the resolved MirFuncId.
    struct PendingGlobalInit {
        TypeId        ty;
        SymbolId      sym;
        std::uint32_t initFuncSlot;
    };
    std::vector<PendingGlobalInit> pendingInitFuncGlobals_;
    // Forward-reference book-keeping for phi incomings whose value or
    // pred wasn't yet emitted at parse time. Resolved at finalize().
    struct PendingPhi {
        MirInstId    phi;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> incomings;
    };
    std::vector<PendingPhi> pendingPhis_;
    bool errors_ = false;

    [[nodiscard]] std::unique_ptr<MirParseResult> makeEmptyResult() {
        return std::make_unique<MirParseResult>(
            Mir{}, TypeInterner{cuId_}, std::vector<std::string>{});
    }

    void emitMalformed(std::string what) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::I_TextMalformed;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(what);
        reporter_.report(std::move(d));
        errors_ = true;
    }

    void emitUnknownName(std::string what) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::I_TextUnknownName;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(what);
        reporter_.report(std::move(d));
        errors_ = true;
    }

    bool expect(TokKind k) {
        Tok t = lex_.take();
        if (t.kind != k) {
            emitMalformed(std::format("expected token kind {}, got '{}'",
                static_cast<int>(k), t.text));
            return false;
        }
        return true;
    }

    bool expectIdent(std::string_view name) {
        Tok t = lex_.take();
        if (t.kind != TokKind::Ident || t.text != name) {
            emitMalformed(std::format("expected '{}', got '{}'", name, t.text));
            return false;
        }
        return true;
    }

    // Parse a numeric token; emit an `I_TextMalformed` diagnostic on
    // `from_chars` failure (the silent-zero default that bare
    // `std::from_chars` produces hides malformed numeric input from
    // the user; verify-on-load might catch a downstream structural
    // mismatch but loses the precise blame).
    template <typename T>
    [[nodiscard]] T parseNumber(std::string_view text,
                                std::string_view what) {
        T v{};
        auto [ptr, ec] = std::from_chars(text.data(),
                                         text.data() + text.size(), v);
        if (ec != std::errc{} || ptr != text.data() + text.size()) {
            emitMalformed(std::format("malformed {} value '{}'", what, text));
        }
        return v;
    }
    [[nodiscard]] double parseDouble(std::string_view text) {
        // `std::from_chars` for float is not universally supported
        // pre-GCC 11; fall back to strtod which signals via errno + endptr.
        std::string buf{text};
        char* end = nullptr;
        errno = 0;
        double const d = std::strtod(buf.c_str(), &end);
        if (errno == ERANGE || end == buf.c_str()) {
            emitMalformed(std::format("malformed float value '{}'", text));
        }
        return d;
    }

    bool peekIdent(std::string_view name) {
        Tok t = lex_.peek();
        return t.kind == TokKind::Ident && t.text == name;
    }

    [[nodiscard]] std::uint32_t parsePercentValue() {
        if (!expect(TokKind::Percent)) return 0;
        // Optional kind prefix: 'b' / 'v' / 'f' / 'g' (ignore — the
        // grammar uses position to disambiguate; the prefix is for
        // human readability only). Parse `[bvgf]?digits`.
        Tok t = lex_.take();
        if (t.kind == TokKind::Ident
         && (t.text.front() == 'b' || t.text.front() == 'v'
          || t.text.front() == 'f' || t.text.front() == 'g')) {
            // Could be `b3`, `v12` etc. — strip the prefix letter and
            // parse the rest as integer.
            std::string_view const num = t.text;
            std::uint32_t v = 0;
            std::size_t i = 1;
            while (i < num.size() && std::isdigit(static_cast<unsigned char>(num[i]))) {
                v = v * 10 + static_cast<std::uint32_t>(num[i] - '0');
                ++i;
            }
            return v;
        }
        if (t.kind == TokKind::Integer) {
            return parseNumber<std::uint32_t>(t.text, "% handle");
        }
        emitMalformed(std::format("expected handle after '%', got '{}'", t.text));
        return 0;
    }

    void parseSymbolsPreamble() {
        (void)expectIdent("symbols");
        if (!expect(TokKind::LBrace)) return;
        while (true) {
            Tok t = lex_.peek();
            if (t.kind == TokKind::RBrace || t.kind == TokKind::End) break;
            std::uint32_t const v = parsePercentValue();
            Tok name = lex_.take();
            if (name.kind != TokKind::String) {
                emitMalformed(std::format("expected symbol name string, got '{}'",
                    name.text));
                continue;
            }
            if (symbolNames_.size() <= v) symbolNames_.resize(v + 1);
            symbolNames_[v] = name.text;
        }
        (void)expect(TokKind::RBrace);
    }

    // Parse a structural type. Grammar mirrors `appendType`.
    [[nodiscard]] TypeId parseType() {
        Tok t = lex_.take();
        if (t.kind != TokKind::Ident) {
            emitMalformed(std::format("expected type, got '{}'", t.text));
            return InvalidType;
        }
        if (t.text == "invalid") return InvalidType;
        if (auto k = primKindFromName(t.text); k.has_value()) {
            return interner_.primitive(*k);
        }
        if (t.text == "ptr" || t.text == "ref" || t.text == "nullable"
         || t.text == "optional" || t.text == "slice") {
            if (!expect(TokKind::LAngle)) return InvalidType;
            TypeId const inner = parseType();
            (void)expect(TokKind::RAngle);
            if (t.text == "ptr")      return interner_.pointer(inner);
            if (t.text == "ref")      return interner_.reference(inner);
            if (t.text == "nullable") return interner_.nullable(inner);
            if (t.text == "optional") return interner_.optional(inner);
            if (t.text == "slice")    return interner_.slice(inner);
        }
        if (t.text == "arr") {
            if (!expect(TokKind::LAngle)) return InvalidType;
            TypeId const elem = parseType();
            (void)expect(TokKind::Comma);
            Tok len = lex_.take();
            if (len.kind != TokKind::Integer) {
                emitMalformed("expected array length integer");
                return InvalidType;
            }
            std::int64_t lv = parseNumber<std::int64_t>(len.text, "array length");
            (void)expect(TokKind::RAngle);
            return interner_.array(elem, lv);
        }
        if (t.text == "tuple") {
            if (!expect(TokKind::LAngle)) return InvalidType;
            std::vector<TypeId> ops;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RAngle) break;
                if (!ops.empty()) (void)expect(TokKind::Comma);
                ops.push_back(parseType());
            }
            (void)expect(TokKind::RAngle);
            return interner_.tuple(ops);
        }
        if (t.text == "struct" || t.text == "union") {
            Tok name = lex_.take();
            if (name.kind != TokKind::String) {
                emitMalformed("expected struct/union name");
                return InvalidType;
            }
            if (!expect(TokKind::LBrace)) return InvalidType;
            std::vector<TypeId> fields;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RBrace) break;
                if (!fields.empty()) (void)expect(TokKind::Comma);
                fields.push_back(parseType());
            }
            (void)expect(TokKind::RBrace);
            if (t.text == "struct") return interner_.structType(name.text, fields);
            return interner_.unionType(name.text, fields);
        }
        if (t.text == "enum") {
            Tok name = lex_.take();
            if (name.kind != TokKind::String) {
                emitMalformed("expected enum name"); return InvalidType;
            }
            TypeKind underlying = TypeKind::I32;
            if (lex_.peek().kind == TokKind::Colon) {
                lex_.take();
                Tok n = lex_.take();
                std::int64_t const k = parseNumber<std::int64_t>(n.text,
                    "enum underlying kind ordinal");
                underlying = static_cast<TypeKind>(k);
            }
            return interner_.enumType(name.text, underlying);
        }
        if (t.text == "fn") {
            if (!expect(TokKind::LParen)) return InvalidType;
            std::vector<TypeId> params;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RParen) break;
                if (!params.empty()) (void)expect(TokKind::Comma);
                params.push_back(parseType());
            }
            (void)expect(TokKind::RParen);
            (void)expect(TokKind::Arrow);
            TypeId const ret = parseType();
            CallConv cc = CallConv::CcSysV;
            if (peekIdent("cc")) {
                lex_.take();
                Tok n = lex_.take();
                if (auto c = callConvFromName(n.text); c.has_value()) cc = *c;
            }
            return interner_.fnSig(params, ret, cc);
        }
        emitMalformed(std::format("unknown type '{}'", t.text));
        return InvalidType;
    }

    // Parse `lit <variant-tag> <value> : <type-core>`.
    [[nodiscard]] MirLiteralValue parseLiteral() {
        MirLiteralValue lv;
        if (!expectIdent("lit")) return lv;
        Tok tag = lex_.take();
        if (tag.kind != TokKind::Ident) {
            emitMalformed("expected literal variant tag"); return lv;
        }
        if (tag.text == "bool") {
            Tok v = lex_.take();
            lv.value = (v.text == "true");
        } else if (tag.text == "int") {
            Tok v = lex_.take();
            lv.value = parseNumber<std::int64_t>(v.text, "int literal");
        } else if (tag.text == "uint") {
            Tok v = lex_.take();
            lv.value = parseNumber<std::uint64_t>(v.text, "uint literal");
        } else if (tag.text == "float") {
            Tok v = lex_.take();
            lv.value = parseDouble(v.text);
        } else if (tag.text == "str") {
            Tok v = lex_.take();
            lv.value = v.text;
        } else if (tag.text == "agg") {
            if (!expect(TokKind::LBrace)) return lv;
            MirAggregateValue agg;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RBrace) break;
                if (!agg.fields.empty()) (void)expect(TokKind::Comma);
                agg.fields.push_back(parseLiteral());
            }
            (void)expect(TokKind::RBrace);
            lv.value = std::move(agg);
        } else if (tag.text == "monostate") {
            // monostate already default
        } else {
            emitMalformed(std::format("unknown literal tag '{}'", tag.text));
        }
        (void)expect(TokKind::Colon);
        Tok coreT = lex_.take();
        if (auto k = primKindFromName(coreT.text); k.has_value()) lv.core = *k;
        else if (coreT.text == "struct") lv.core = TypeKind::Struct;
        else if (coreT.text == "union")  lv.core = TypeKind::Union;
        else if (coreT.text == "array")  lv.core = TypeKind::Array;
        else if (coreT.text == "ptr")    lv.core = TypeKind::Ptr;
        else if (coreT.text == "ref")    lv.core = TypeKind::Ref;
        else if (coreT.text == "enum")   lv.core = TypeKind::Enum;
        return lv;
    }

    void parseGlobal() {
        std::uint32_t const sym = parsePercentValue();
        if (!expect(TokKind::Colon)) return;
        TypeId const ty = parseType();
        if (!expect(TokKind::Eq)) return;
        Tok pk = lex_.peek();
        if (pk.kind == TokKind::Ident && pk.text == "zero") {
            lex_.take();
            builder_.addGlobal(ty, SymbolId{sym});
        } else if (pk.kind == TokKind::Ident && pk.text == "initfunc") {
            lex_.take();
            std::uint32_t const fnSlot = parsePercentValue();
            // Resolve via funcMap_ if available (function declared before
            // this global); else defer to finalize() — the function may
            // appear later in the text.
            auto it = funcMap_.find(fnSlot);
            if (it != funcMap_.end()) {
                builder_.addGlobal(ty, SymbolId{sym}, UINT32_MAX, it->second);
            } else {
                pendingInitFuncGlobals_.push_back({ty, SymbolId{sym}, fnSlot});
            }
        } else {
            MirLiteralValue lv = parseLiteral();
            std::uint32_t const litIdx = builder_.literalPoolAdd(std::move(lv));
            builder_.addGlobal(ty, SymbolId{sym}, litIdx);
        }
    }

    void parseFunction() {
        std::uint32_t const sym = parsePercentValue();
        if (!expect(TokKind::Colon)) return;
        TypeId const sig = parseType();
        MirFuncId const f = builder_.addFunction(sig, SymbolId{sym});
        // Text initfunc references use %f<MirFuncId.v>. Track in
        // parse order so deferred-resolution at finalize works even
        // when a global with `initfunc` precedes its target function.
        funcMap_[f.v] = f;
        // Bail on missing `{` — without the function body's opening
        // brace we have nothing to parse; continuing would cascade
        // every subsequent token as malformed.
        if (!expect(TokKind::LBrace)) return;
        // Two-pass: first scan all block headers (with their markers)
        // and create them in declaration order, so forward refs from
        // branch instructions resolve to blocks with the correct
        // marker. Then rewind via the Lexer's `setPos` and parse the
        // bodies.
        std::size_t const bodyStart = lex_.pos();
        scanBlockHeaders();
        lex_.setPos(bodyStart);
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            if (pk.kind != TokKind::Ident || pk.text != "block") {
                emitMalformed(std::format("expected 'block' inside function, got '{}'",
                    pk.text));
                lex_.take();
                continue;
            }
            parseBlock();
        }
        (void)expect(TokKind::RBrace);
    }

    // First-pass scan: walk the function body tokens and CREATE every
    // block (with its declared marker) in declaration order. Doesn't
    // parse instruction bodies; just consumes balanced braces past
    // each block. Stops at the matching `}` for the enclosing function.
    void scanBlockHeaders() {
        int depth = 1;  // we're inside the function's `{`
        while (depth > 0) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::End) break;
            if (pk.kind == TokKind::RBrace) { lex_.take(); --depth; continue; }
            if (pk.kind == TokKind::Ident && pk.text == "block" && depth == 1) {
                lex_.take();  // consume `block`
                std::uint32_t const slot = parsePercentValue();
                StructCfMarker marker = StructCfMarker::Linear;
                if (lex_.peek().kind == TokKind::LBracket) {
                    lex_.take();
                    Tok m = lex_.take();
                    if (auto mk = markerFromName(m.text); mk.has_value()) marker = *mk;
                    (void)expect(TokKind::RBracket);
                }
                // Create the block now with the correct marker. Body
                // tokens will be re-lexed in pass 2.
                if (blockMap_.find(slot) == blockMap_.end()) {
                    blockMap_[slot] = builder_.createBlock(marker);
                }
                // Skip through the block's `{ ... }` to the next
                // sibling block.
                if (lex_.peek().kind == TokKind::LBrace) {
                    lex_.take();
                    ++depth;  // entered block body
                }
                continue;
            }
            if (pk.kind == TokKind::LBrace) { lex_.take(); ++depth; continue; }
            lex_.take();  // skip any other token at this depth
        }
    }

    [[nodiscard]] MirBlockId ensureBlock(std::uint32_t slot,
                                         StructCfMarker marker) {
        auto it = blockMap_.find(slot);
        if (it != blockMap_.end()) return it->second;
        MirBlockId const b = builder_.createBlock(marker);
        blockMap_[slot] = b;
        return b;
    }

    [[nodiscard]] MirBlockId resolveBlockRef(std::uint32_t slot) {
        auto it = blockMap_.find(slot);
        if (it != blockMap_.end()) return it->second;
        // Reached only on MALFORMED input: a `br`/`condbr`/`switch`
        // names a block-slot that was not declared as a `block %bN`
        // header. `scanBlockHeaders` would have pre-created every
        // declared block; the lookup miss here means the reference
        // is to a non-existent block. Create a Linear placeholder
        // so the builder doesn't abort; the verify-on-load pass
        // will flag the orphan (block created but never filled, OR
        // referenced from a terminator whose target doesn't appear
        // in the function's block range).
        MirBlockId const b = builder_.createBlock(StructCfMarker::Linear);
        blockMap_[slot] = b;
        return b;
    }

    void parseBlock() {
        (void)expectIdent("block");
        std::uint32_t const slot = parsePercentValue();
        // Consume optional marker brackets; the BLOCK was already
        // CREATED with this marker during scanBlockHeaders, so we
        // just need to advance past the syntactic marker here.
        if (lex_.peek().kind == TokKind::LBracket) {
            lex_.take();
            lex_.take();  // marker ident
            (void)expect(TokKind::RBracket);
        }
        auto it = blockMap_.find(slot);
        if (it == blockMap_.end()) {
            emitUnknownName(std::format("block %b{} not pre-declared", slot));
            return;
        }
        MirBlockId const b = it->second;
        // Bail BEFORE `beginBlock` — otherwise a missing `{` leaves
        // the builder with an Open-state block that finalize()'s
        // `errors_` short-circuit relies on to avoid `MirBuilder::
        // finish()`'s abort. Bailing first keeps the builder in a
        // clean state regardless of the finalize() path.
        if (!expect(TokKind::LBrace)) return;
        builder_.beginBlock(b);
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            parseInstruction();
        }
        (void)expect(TokKind::RBrace);
    }

    [[nodiscard]] MirInstId resolveValue(std::uint32_t slot) {
        auto it = valueMap_.find(slot);
        if (it != valueMap_.end()) return it->second;
        emitUnknownName(std::format("unknown value handle '%v{}'", slot));
        return InvalidMirInst;
    }

    void parseInstruction() {
        // Two forms:
        //   `%vN = opcode : type (operands)`
        //   `terminator [operands]` (br/condbr/switch/return/unreachable)
        Tok first = lex_.peek();
        std::uint32_t resultSlot = 0;
        if (first.kind == TokKind::Percent) {
            resultSlot = parsePercentValue();
            (void)expect(TokKind::Eq);
        }
        Tok mn = lex_.take();
        if (mn.kind != TokKind::Ident) {
            emitMalformed("expected opcode mnemonic"); return;
        }
        // Mnemonics may contain dots (e.g. icmp.eq). Re-glue.
        std::string mnemonic = mn.text;
        while (lex_.peek().kind == TokKind::Dot) {
            lex_.take();
            Tok part = lex_.take();
            mnemonic += '.';
            mnemonic += part.text;
        }
        auto opOpt = opcodeFromMnemonic(mnemonic);
        if (!opOpt.has_value()) {
            emitMalformed(std::format("unknown opcode '{}'", mnemonic));
            return;
        }
        MirOpcode const op = *opOpt;
        TypeId resultType = InvalidType;
        if (lex_.peek().kind == TokKind::Colon) {
            lex_.take();
            resultType = parseType();
        }
        // Per-opcode operand parsing.
        switch (op) {
            case MirOpcode::Const: {
                if (!expect(TokKind::LParen)) return;
                MirLiteralValue lv = parseLiteral();
                (void)expect(TokKind::RParen);
                MirInstId const id = builder_.addConst(std::move(lv), resultType);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::Arg: {
                if (!expect(TokKind::LParen)) return;
                Tok n = lex_.take();
                std::uint32_t const idx = parseNumber<std::uint32_t>(
                    n.text, "arg ordinal");
                // Optional `, position` (arg_payload.hpp); absent → position
                // defaults to the ordinal (the single-class golden form).
                std::uint32_t position = idx;
                if (lex_.peek().kind == TokKind::Comma) {
                    (void)lex_.take();
                    Tok p = lex_.take();
                    position = parseNumber<std::uint32_t>(p.text, "arg position");
                }
                (void)expect(TokKind::RParen);
                MirInstId const id = builder_.addArg(idx, resultType, position);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::GlobalAddr: {
                if (!expect(TokKind::LParen)) return;
                std::uint32_t const sym = parsePercentValue();
                (void)expect(TokKind::RParen);
                MirInstId const id = builder_.addGlobalAddr(SymbolId{sym}, resultType);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::IntrinsicCall: {
                // Emitter wrote: `(intrinsic <id>, %v1, %v2, ...)`.
                if (!expect(TokKind::LParen)) return;
                if (!expectIdent("intrinsic")) return;
                Tok idTok = lex_.take();
                std::uint32_t const intrinId = parseNumber<std::uint32_t>(
                    idTok.text, "intrinsic id");
                std::vector<MirInstId> operands;
                while (lex_.peek().kind == TokKind::Comma) {
                    lex_.take();
                    operands.push_back(resolveValue(parsePercentValue()));
                }
                (void)expect(TokKind::RParen);
                MirInstId const id = builder_.addInst(op, operands, resultType, intrinId);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::Phi: {
                if (!expect(TokKind::LBracket)) return;
                std::vector<std::pair<std::uint32_t, std::uint32_t>> incs;
                while (true) {
                    Tok pk = lex_.peek();
                    if (pk.kind == TokKind::RBracket) break;
                    if (!incs.empty()) (void)expect(TokKind::Comma);
                    (void)expect(TokKind::LParen);
                    std::uint32_t const v = parsePercentValue();
                    (void)expect(TokKind::Comma);
                    std::uint32_t const p = parsePercentValue();
                    (void)expect(TokKind::RParen);
                    incs.emplace_back(v, p);
                }
                (void)expect(TokKind::RBracket);
                MirInstId const phi = builder_.addPhi(resultType);
                if (resultSlot != 0) valueMap_[resultSlot] = phi;
                pendingPhis_.push_back({phi, std::move(incs)});
                break;
            }
            case MirOpcode::Br: {
                std::uint32_t const target = parsePercentValue();
                MirBlockId const tBB = resolveBlockRef(target);
                builder_.addBr(tBB);
                break;
            }
            case MirOpcode::CondBr: {
                std::uint32_t const condSlot = parsePercentValue();
                std::uint32_t const t1 = parsePercentValue();
                std::uint32_t const t2 = parsePercentValue();
                MirInstId const cond = resolveValue(condSlot);
                MirBlockId const b1 = resolveBlockRef(t1);
                MirBlockId const b2 = resolveBlockRef(t2);
                builder_.addCondBr(cond, b1, b2);
                break;
            }
            case MirOpcode::Switch: {
                std::uint32_t const discSlot = parsePercentValue();
                MirInstId const disc = resolveValue(discSlot);
                if (!expect(TokKind::LBrace)) return;
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                MirBlockId defaultBB{};
                bool sawDefault = false;
                while (true) {
                    Tok pk = lex_.peek();
                    if (pk.kind == TokKind::RBrace) break;
                    if (!cases.empty() || sawDefault) (void)expect(TokKind::Comma);
                    Tok kw = lex_.take();
                    if (kw.kind == TokKind::Ident && kw.text == "case") {
                        std::uint32_t const caseVSlot = parsePercentValue();
                        (void)expect(TokKind::Arrow);
                        std::uint32_t const tgt = parsePercentValue();
                        MirInstId const cv = resolveValue(caseVSlot);
                        MirBlockId const tb = resolveBlockRef(tgt);
                        cases.emplace_back(cv, tb);
                    } else if (kw.kind == TokKind::Ident && kw.text == "default") {
                        (void)expect(TokKind::Arrow);
                        std::uint32_t const tgt = parsePercentValue();
                        defaultBB = resolveBlockRef(tgt);
                        sawDefault = true;
                    } else {
                        emitMalformed(std::format("expected 'case'/'default', got '{}'", kw.text));
                        lex_.take();
                    }
                }
                (void)expect(TokKind::RBrace);
                if (sawDefault) {
                    builder_.addSwitch(disc, cases, defaultBB);
                }
                break;
            }
            case MirOpcode::Return: {
                // FC7 C1c: parse EVERY return-piece operand (multi-piece struct
                // return). 0 → void `addReturn()`; N≥1 → `addReturnMulti` (which
                // covers the scalar 1-operand case identically to `addReturn(v)`).
                std::vector<MirInstId> vals;
                while (lex_.peek().kind == TokKind::Percent) {
                    std::uint32_t const v = parsePercentValue();
                    vals.push_back(resolveValue(v));
                }
                if (vals.empty()) {
                    builder_.addReturn();
                } else {
                    builder_.addReturnMulti(vals);
                }
                break;
            }
            case MirOpcode::Unreachable: {
                builder_.addUnreachable();
                break;
            }
            case MirOpcode::SehTryBegin: {
                // c115 SEH: `seh_try_begin <region> %b<try> %b<filter>`.
                Tok rTok = lex_.take();
                std::uint32_t const region = parseNumber<std::uint32_t>(
                    rTok.text, "seh region id");
                std::uint32_t const t1 = parsePercentValue();
                std::uint32_t const t2 = parsePercentValue();
                builder_.addSehTryBegin(resolveBlockRef(t1), resolveBlockRef(t2),
                                        region);
                break;
            }
            case MirOpcode::SehFilterReturn: {
                // `seh_filter_return <region> %v<val> %b<handler>`.
                Tok rTok = lex_.take();
                std::uint32_t const region = parseNumber<std::uint32_t>(
                    rTok.text, "seh region id");
                std::uint32_t const vSlot = parsePercentValue();
                std::uint32_t const tgt   = parsePercentValue();
                builder_.addSehFilterReturn(resolveValue(vSlot),
                                            resolveBlockRef(tgt), region);
                break;
            }
            case MirOpcode::SehTryEnd: {
                // `seh_try_end <region>` — a payload-carrying 0-operand marker.
                Tok rTok = lex_.take();
                std::uint32_t const region = parseNumber<std::uint32_t>(
                    rTok.text, "seh region id");
                builder_.addInst(op, {}, InvalidType, region);
                break;
            }
            default: {
                // Generic: zero-or-more operands in parens.
                std::vector<MirInstId> operands;
                if (lex_.peek().kind == TokKind::LParen) {
                    lex_.take();
                    while (true) {
                        Tok pk = lex_.peek();
                        if (pk.kind == TokKind::RParen) break;
                        if (!operands.empty()) (void)expect(TokKind::Comma);
                        std::uint32_t const v = parsePercentValue();
                        operands.push_back(resolveValue(v));
                    }
                    (void)expect(TokKind::RParen);
                }
                std::uint32_t payload = 0;
                if (peekIdent("payload")) {
                    lex_.take();
                    Tok n = lex_.take();
                    payload = parseNumber<std::uint32_t>(n.text, "payload");
                }
                MirInstId const id = builder_.addInst(op, operands, resultType, payload);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
        }
    }

    [[nodiscard]] std::unique_ptr<MirParseResult> finalize() {
        // Resolve any pending `initfunc` globals whose target function
        // was declared after the global in the text.
        for (auto const& pg : pendingInitFuncGlobals_) {
            auto it = funcMap_.find(pg.initFuncSlot);
            if (it == funcMap_.end()) {
                emitUnknownName(std::format(
                    "global %{}'s initfunc %f{} references a function "
                    "that was never declared",
                    pg.sym.v, pg.initFuncSlot));
                continue;
            }
            builder_.addGlobal(pg.ty, pg.sym, UINT32_MAX, it->second);
        }
        // Resolve phi incomings now that all blocks + values are known.
        for (auto& pp : pendingPhis_) {
            for (auto const& [vSlot, pSlot] : pp.incomings) {
                MirInstId const v = resolveValue(vSlot);
                auto it = blockMap_.find(pSlot);
                MirBlockId const p = (it != blockMap_.end())
                    ? it->second
                    : ([&] { emitUnknownName(std::format("unknown block '%b{}'", pSlot));
                             return MirBlockId{}; })();
                if (!v.valid() || !p.valid()) continue;
                builder_.addPhiIncoming(pp.phi, MirPhiIncoming{v, p});
            }
        }
        std::size_t const errBefore = reporter_.errorCount();
        // `MirBuilder::finish()` aborts on contract violations (e.g.
        // a block was created but never `beginBlock`'d, or never
        // terminated). If parse errors have already occurred we
        // cannot trust the builder's invariants — return an empty
        // module rather than aborting the process, so the user sees
        // the parse diagnostics instead of a crash. Verify-on-load
        // is skipped because there's nothing meaningful to verify.
        if (errors_) {
            return std::make_unique<MirParseResult>(
                Mir{}, std::move(interner_), std::move(symbolNames_));
        }
        Mir module = std::move(builder_).finish();
        auto result = std::make_unique<MirParseResult>(
            std::move(module), std::move(interner_), std::move(symbolNames_));
        // Verify-on-load.
        MirVerifier verifier{result->mir, &result->interner};
        (void)verifier.verify(reporter_);
        result->ok = (reporter_.errorCount() == errBefore);
        return result;
    }
};

} // namespace

std::unique_ptr<MirParseResult> parseMir(std::string_view text,
                                         CompilationUnitId cuId,
                                         DiagnosticReporter& reporter) {
    Parser p{text, cuId, reporter};
    return p.run();
}

} // namespace dss
