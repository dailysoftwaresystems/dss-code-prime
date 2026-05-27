#include "hir/hir_text.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "hir/attributes/diagnostic_info.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/attributes/shader_intrinsic.hpp"
#include "hir/attributes/source_span.hpp"
#include "hir/attributes/transpile_hints.hpp"
#include "hir/hir_intrinsic_registry.hpp"
#include "hir/hir_kind_registry.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_op_registry.hpp"
#include "hir/hir_verifier.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// `.dsshir` text format (HR7) — see hir_text.hpp for the contract. Grammar at a
// glance (whitespace-insignificant; LF only; canonical formatting below):
//
//   file       := "dsshir" INT preamble module
//   preamble   := ext_kinds? ext_ops? intrinsics? symbols?   (non-empty sections only)
//   module     := "module" flags? STR "{" decl* "}"
//   decl       := function | global | type_decl | extern_function
//               | extern_global | import_group | ext_node | error
//   stmt       := block | if | while | do | for | switch
//               | break | continue | return | expr | var | assign
//               | unreachable | ext_node | error
//   expr       := lit | ref | call | intrinsic | binop | unop | cast | member
//               | index | swizzle | construct | ternary | logical_and
//               | logical_or | sizeof | addressof | deref | typeref
//               | ext_node | error
//   type       := bool|i8..|u8..|f16..|char|byte|void | ptr<T> | ref<T>
//               | nullable<T> | optional<T> | slice<T> | vec<T,N> | mat<T,R,C>
//               | arr<T,N> | tuple<T,...> | struct STR {T,...} | union STR {T,...}
//               | fn(T,...) -> T [cc NAME] | ext STR (T,...) [N,...] | invalid
//
// Symbols are positional handles (`%1`) bound to a name in the `symbols` preamble;
// the handle number IS the rebuilt SymbolId.v, so the body and preamble agree by
// construction. Types render structurally (CU-ephemeral TypeId.v never appears).
// Extension kinds/ops/intrinsics are referenced by name and re-registered from the
// preamble. Side-tables attach inline (`@loc(...)`, `@ffi(...)`, …) before a node;
// the lone cross-node reference (DiagnosticInfo.origin) is a pre-order node index.

namespace dss {

namespace {

// ── shared name tables (small fixed enums local to the grammar) ──────────────

[[nodiscard]] std::string_view ccName(CallConv cc) noexcept {
    switch (cc) {
        case CallConv::CcSysV:       return "sysv";
        case CallConv::CcMS64:       return "ms64";
        case CallConv::CcAAPCS64:    return "aapcs64";
        case CallConv::CcApple:      return "apple";
        case CallConv::CcFastcall:   return "fastcall";
        case CallConv::CcThiscall:   return "thiscall";
        case CallConv::CcVectorcall: return "vectorcall";
        case CallConv::CcWasm:       return "wasm";
        case CallConv::CcSpirv:      return "spirv";
    }
    return "sysv";
}
[[nodiscard]] std::optional<CallConv> ccFromName(std::string_view s) noexcept {
    if (s == "sysv") return CallConv::CcSysV;
    if (s == "ms64") return CallConv::CcMS64;
    if (s == "aapcs64") return CallConv::CcAAPCS64;
    if (s == "apple") return CallConv::CcApple;
    if (s == "fastcall") return CallConv::CcFastcall;
    if (s == "thiscall") return CallConv::CcThiscall;
    if (s == "vectorcall") return CallConv::CcVectorcall;
    if (s == "wasm") return CallConv::CcWasm;
    if (s == "spirv") return CallConv::CcSpirv;
    return std::nullopt;
}

[[nodiscard]] std::string_view primName(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool: return "bool";
        case TypeKind::I8:   return "i8";   case TypeKind::I16: return "i16";
        case TypeKind::I32:  return "i32";  case TypeKind::I64: return "i64";
        case TypeKind::I128: return "i128";
        case TypeKind::U8:   return "u8";   case TypeKind::U16: return "u16";
        case TypeKind::U32:  return "u32";  case TypeKind::U64: return "u64";
        case TypeKind::U128: return "u128";
        case TypeKind::F16:  return "f16";  case TypeKind::F32: return "f32";
        case TypeKind::F64:  return "f64";  case TypeKind::F128: return "f128";
        case TypeKind::Char: return "char"; case TypeKind::Byte: return "byte";
        case TypeKind::Void: return "void";
        default: return {};
    }
}
[[nodiscard]] std::optional<TypeKind> primFromName(std::string_view s) noexcept {
    if (s == "bool") return TypeKind::Bool;
    if (s == "i8")   return TypeKind::I8;   if (s == "i16") return TypeKind::I16;
    if (s == "i32")  return TypeKind::I32;  if (s == "i64") return TypeKind::I64;
    if (s == "i128") return TypeKind::I128;
    if (s == "u8")   return TypeKind::U8;   if (s == "u16") return TypeKind::U16;
    if (s == "u32")  return TypeKind::U32;  if (s == "u64") return TypeKind::U64;
    if (s == "u128") return TypeKind::U128;
    if (s == "f16")  return TypeKind::F16;  if (s == "f32") return TypeKind::F32;
    if (s == "f64")  return TypeKind::F64;  if (s == "f128") return TypeKind::F128;
    if (s == "char") return TypeKind::Char; if (s == "byte") return TypeKind::Byte;
    if (s == "void") return TypeKind::Void;
    return std::nullopt;
}

[[nodiscard]] std::optional<HirOpKind> coreOpFromName(std::string_view s) noexcept {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirOpKind::Count_); ++i) {
        auto const op = static_cast<HirOpKind>(i);
        if (opName(op) == s) return op;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view ffiLinkageName(FfiLinkage l) noexcept {
    switch (l) { case FfiLinkage::Strong: return "strong";
                 case FfiLinkage::Weak:   return "weak";
                 case FfiLinkage::Common: return "common"; }
    return "strong";
}
[[nodiscard]] std::optional<FfiLinkage> ffiLinkageFromName(std::string_view s) noexcept {
    if (s == "strong") return FfiLinkage::Strong;
    if (s == "weak")   return FfiLinkage::Weak;
    if (s == "common") return FfiLinkage::Common;
    return std::nullopt;
}
[[nodiscard]] std::string_view ffiVisName(FfiVisibility v) noexcept {
    switch (v) { case FfiVisibility::Default:   return "default";
                 case FfiVisibility::Hidden:    return "hidden";
                 case FfiVisibility::Protected: return "protected"; }
    return "default";
}
[[nodiscard]] std::optional<FfiVisibility> ffiVisFromName(std::string_view s) noexcept {
    if (s == "default")   return FfiVisibility::Default;
    if (s == "hidden")    return FfiVisibility::Hidden;
    if (s == "protected") return FfiVisibility::Protected;
    return std::nullopt;
}

[[nodiscard]] std::string_view stageName(ShaderStage s) noexcept {
    switch (s) { case ShaderStage::None: return "none";
                 case ShaderStage::Vertex: return "vertex";
                 case ShaderStage::Fragment: return "fragment";
                 case ShaderStage::Compute: return "compute";
                 case ShaderStage::Geometry: return "geometry";
                 case ShaderStage::TessControl: return "tess_control";
                 case ShaderStage::TessEval: return "tess_eval"; }
    return "none";
}
[[nodiscard]] std::optional<ShaderStage> stageFromName(std::string_view s) noexcept {
    if (s == "none") return ShaderStage::None;
    if (s == "vertex") return ShaderStage::Vertex;
    if (s == "fragment") return ShaderStage::Fragment;
    if (s == "compute") return ShaderStage::Compute;
    if (s == "geometry") return ShaderStage::Geometry;
    if (s == "tess_control") return ShaderStage::TessControl;
    if (s == "tess_eval") return ShaderStage::TessEval;
    return std::nullopt;
}
[[nodiscard]] std::string_view builtinName(ShaderBuiltin b) noexcept {
    switch (b) { case ShaderBuiltin::None: return "none";
                 case ShaderBuiltin::Position: return "position";
                 case ShaderBuiltin::PointSize: return "point_size";
                 case ShaderBuiltin::VertexIndex: return "vertex_index";
                 case ShaderBuiltin::InstanceIndex: return "instance_index";
                 case ShaderBuiltin::FragCoord: return "frag_coord";
                 case ShaderBuiltin::FragDepth: return "frag_depth";
                 case ShaderBuiltin::FrontFacing: return "front_facing";
                 case ShaderBuiltin::GlobalInvocationId: return "global_invocation_id";
                 case ShaderBuiltin::LocalInvocationId: return "local_invocation_id";
                 case ShaderBuiltin::WorkgroupId: return "workgroup_id";
                 case ShaderBuiltin::NumWorkgroups: return "num_workgroups"; }
    return "none";
}
[[nodiscard]] std::optional<ShaderBuiltin> builtinFromName(std::string_view s) noexcept {
    if (s == "none") return ShaderBuiltin::None;
    if (s == "position") return ShaderBuiltin::Position;
    if (s == "point_size") return ShaderBuiltin::PointSize;
    if (s == "vertex_index") return ShaderBuiltin::VertexIndex;
    if (s == "instance_index") return ShaderBuiltin::InstanceIndex;
    if (s == "frag_coord") return ShaderBuiltin::FragCoord;
    if (s == "frag_depth") return ShaderBuiltin::FragDepth;
    if (s == "front_facing") return ShaderBuiltin::FrontFacing;
    if (s == "global_invocation_id") return ShaderBuiltin::GlobalInvocationId;
    if (s == "local_invocation_id") return ShaderBuiltin::LocalInvocationId;
    if (s == "workgroup_id") return ShaderBuiltin::WorkgroupId;
    if (s == "num_workgroups") return ShaderBuiltin::NumWorkgroups;
    return std::nullopt;
}
[[nodiscard]] std::string_view idiomName(TranspileIdiom i) noexcept {
    switch (i) { case TranspileIdiom::Default: return "default";
                 case TranspileIdiom::EarlyReturn: return "early_return";
                 case TranspileIdiom::GuardClause: return "guard_clause";
                 case TranspileIdiom::TernaryExpr: return "ternary_expr";
                 case TranspileIdiom::RangeFor: return "range_for";
                 case TranspileIdiom::WhileLoop: return "while_loop"; }
    return "default";
}
[[nodiscard]] std::optional<TranspileIdiom> idiomFromName(std::string_view s) noexcept {
    if (s == "default") return TranspileIdiom::Default;
    if (s == "early_return") return TranspileIdiom::EarlyReturn;
    if (s == "guard_clause") return TranspileIdiom::GuardClause;
    if (s == "ternary_expr") return TranspileIdiom::TernaryExpr;
    if (s == "range_for") return TranspileIdiom::RangeFor;
    if (s == "while_loop") return TranspileIdiom::WhileLoop;
    return std::nullopt;
}
[[nodiscard]] std::string_view recoveryName(HirRecovery r) noexcept {
    switch (r) { case HirRecovery::None: return "none";
                 case HirRecovery::Substituted: return "substituted";
                 case HirRecovery::Dropped: return "dropped";
                 case HirRecovery::Synthesized: return "synthesized"; }
    return "none";
}
[[nodiscard]] std::optional<HirRecovery> recoveryFromName(std::string_view s) noexcept {
    if (s == "none") return HirRecovery::None;
    if (s == "substituted") return HirRecovery::Substituted;
    if (s == "dropped") return HirRecovery::Dropped;
    if (s == "synthesized") return HirRecovery::Synthesized;
    return std::nullopt;
}

[[nodiscard]] bool isExprKind(HirKind k) noexcept {
    switch (k) {
        case HirKind::Literal: case HirKind::Ref: case HirKind::Call:
        case HirKind::IntrinsicCall: case HirKind::BinaryOp: case HirKind::UnaryOp:
        case HirKind::Cast: case HirKind::MemberAccess: case HirKind::Index:
        case HirKind::Swizzle: case HirKind::ConstructAggregate: case HirKind::Ternary:
        case HirKind::LogicalAnd: case HirKind::LogicalOr: case HirKind::SizeOf:
        case HirKind::AddressOf: case HirKind::Deref: case HirKind::SeqExpr:
        case HirKind::TypeRef:
            return true;
        default: return false;
    }
}

// Does this node carry a symbol id in its payload? (Used by the symbol pre-pass.)
[[nodiscard]] bool carriesSymbol(HirKind k) noexcept {
    switch (k) {
        case HirKind::Ref: case HirKind::VarDecl: case HirKind::Function:
        case HirKind::Global: case HirKind::TypeDecl: case HirKind::ExternFunction:
        case HirKind::ExternGlobal:
            return true;
        default: return false;
    }
}

[[nodiscard]] std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// ── Emitter ──────────────────────────────────────────────────────────────────

class Emitter {
public:
    Emitter(Hir const& hir, HirTextContext const& ctx, DiagnosticReporter& reporter)
        : hir_(hir), ctx_(ctx), reporter_(reporter) {}

    [[nodiscard]] std::string run() {
        if (!hir_.root().valid()) { out_ += "dsshir 1\n"; return std::move(out_); }
        prepass(hir_.root());

        out_ += "dsshir 1\n";
        emitExtKinds();
        emitExtOps();
        emitIntrinsics();
        emitSymbols();

        HirNodeId const root = hir_.root();
        if (hir_.kind(root) != HirKind::Module) {
            // The grammar's top level is a module; a non-Module root can't be
            // spelled. Fail loud-but-recoverable (a diagnostic, not an abort).
            report("root node is not a Module — cannot serialize");
            out_ += "module \"\" {\n}\n";
            return std::move(out_);
        }
        out_ += "module";
        out_ += flagsStr(hir_.flags(root));
        out_ += ' ';
        out_ += quote(hir_.sourceLanguage());
        out_ += " {\n";
        for (HirNodeId d : hir_.moduleDecls(root)) emitNodeLine(d, 1);
        out_ += "}\n";
        return std::move(out_);
    }

private:
    Hir const&            hir_;
    HirTextContext const& ctx_;
    DiagnosticReporter&   reporter_;
    std::string           out_;

    std::unordered_map<std::uint32_t, std::uint32_t> preIndex_;   // HirNodeId.v -> pre-order index
    std::unordered_map<std::uint32_t, std::uint32_t> symHandle_;  // SymbolId.v  -> handle (1..N)
    std::vector<std::uint32_t>                       symOrder_;   // SymbolId.v in first-encounter order
    bool internerWarned_ = false;

    // Emitter-side diagnostic. Defaults to Error: the cases that reach here
    // (non-Module root, an Extension/IntrinsicCall payload the registry doesn't
    // resolve, an unprintable type) emit a `?`/`error` token that CANNOT be
    // re-parsed — the output is not round-trippable, so `reporter.hasErrors()`
    // must report it. The lone documented exception (a deliberately-absent
    // interner, the `?`-types degraded mode) passes Warning explicitly.
    void report(std::string detail, DiagnosticSeverity sev = DiagnosticSeverity::Error) {
        ParseDiagnostic d;
        d.code = DiagnosticCode::H_TextMalformed;
        d.severity = sev;
        d.actual = std::move(detail);
        reporter_.report(std::move(d));
    }

    // Single children-order pre-order DFS: assigns each node a stable pre-order
    // index (for DiagnosticInfo.origin references) and collects referenced
    // SymbolIds in first-encounter order (their handles). Must visit children in
    // the same order `emitNodeLine` does (children() order) so indices align.
    void prepass(HirNodeId id) {
        std::uint32_t const idx = static_cast<std::uint32_t>(preIndex_.size());
        preIndex_.emplace(id.v, idx);
        if (carriesSymbol(hir_.kind(id))) {
            std::uint32_t const sv = hir_.payload(id);
            if (!symHandle_.contains(sv)) {
                symOrder_.push_back(sv);
                symHandle_.emplace(sv, static_cast<std::uint32_t>(symOrder_.size()));
            }
        }
        for (HirNodeId c : hir_.children(id)) prepass(c);
    }

    [[nodiscard]] std::string indent(int n) const { return std::string(static_cast<std::size_t>(n) * 2, ' '); }

    [[nodiscard]] std::string flagsStr(HirFlags f) const {
        if (!any(f)) return {};
        std::string out = " [";
        bool first = true;
        auto add = [&](char const* n) { if (!first) out += ','; out += n; first = false; };
        if (has(f, HirFlags::HasError))     add("err");
        if (has(f, HirFlags::Synthetic))    add("syn");
        if (has(f, HirFlags::ShaderUsable)) add("shader");
        if (has(f, HirFlags::HostUsable))   add("host");
        out += ']';
        return out;
    }

    [[nodiscard]] std::uint32_t handleOf(std::uint32_t symv) const {
        auto it = symHandle_.find(symv);
        return it == symHandle_.end() ? 0u : it->second;
    }

    // ── type printer (structural; nominal types by interned name) ────────────
    void appendType(TypeId t) {
        if (!t.valid()) { out_ += "invalid"; return; }
        if (ctx_.interner == nullptr) {
            if (!internerWarned_) {
                report("no TypeInterner supplied; types render as '?'", DiagnosticSeverity::Warning);
                internerWarned_ = true;
            }
            out_ += '?';
            return;
        }
        TypeInterner const& in = *ctx_.interner;
        auto args = [&](std::span<TypeId const> ops) {
            bool first = true;
            for (TypeId o : ops) { if (!first) out_ += ", "; appendType(o); first = false; }
        };
        switch (in.kind(t)) {
            case TypeKind::Ptr:      out_ += "ptr<";      appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Ref:      out_ += "ref<";      appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Nullable: out_ += "nullable<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Optional: out_ += "optional<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Slice:    out_ += "slice<";    appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::FnPtr:    out_ += "fnptr<";    appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Vector:
                out_ += "vec<"; appendType(in.operands(t)[0]);
                out_ += std::format(", {}>", in.scalars(t)[0]); return;
            case TypeKind::Matrix:
                out_ += "mat<"; appendType(in.operands(t)[0]);
                out_ += std::format(", {}, {}>", in.scalars(t)[0], in.scalars(t)[1]); return;
            case TypeKind::Array:
                out_ += "arr<"; appendType(in.operands(t)[0]);
                out_ += std::format(", {}>", in.scalars(t)[0]); return;
            case TypeKind::Tuple:  out_ += "tuple<"; args(in.operands(t)); out_ += '>'; return;
            case TypeKind::Struct: out_ += "struct "; out_ += quote(in.name(t)); out_ += " {"; args(in.operands(t)); out_ += '}'; return;
            case TypeKind::Union:  out_ += "union ";  out_ += quote(in.name(t)); out_ += " {"; args(in.operands(t)); out_ += '}'; return;
            case TypeKind::FnSig: {
                out_ += "fn(";
                args(in.fnParams(t));
                out_ += ") -> ";
                appendType(in.fnResult(t));
                auto sc = in.scalars(t);
                if (!sc.empty()) {
                    auto const cc = static_cast<CallConv>(sc[0]);
                    if (cc != CallConv::CcSysV) { out_ += " cc "; out_ += ccName(cc); }
                }
                return;
            }
            case TypeKind::Extension: {
                out_ += "ext "; out_ += quote(in.name(t)); out_ += " (";
                args(in.operands(t)); out_ += ')';
                auto sc = in.scalars(t);
                if (!sc.empty()) {
                    out_ += " [";
                    bool first = true;
                    for (auto s : sc) { if (!first) out_ += ", "; out_ += std::format("{}", s); first = false; }
                    out_ += ']';
                }
                return;
            }
            default: { // primitives (and any unexpected kind)
                std::string_view const p = primName(in.kind(t));
                if (!p.empty()) out_ += p; else { report("unprintable type kind"); out_ += '?'; }
                return;
            }
        }
    }

    // ── inline attribute prefix (for expression-position nodes) ──────────────
    [[nodiscard]] std::string attrsInline(HirNodeId id) {
        std::string s;
        forEachAttr(id, [&](std::string const& a) { s += a; s += ' '; });
        return s;
    }
    void emitAttrsBlock(HirNodeId id, int ind) {
        forEachAttr(id, [&](std::string const& a) { out_ += indent(ind); out_ += a; out_ += '\n'; });
    }

    template <class F> void forEachAttr(HirNodeId id, F&& sink) {
        if (ctx_.sourceMap) if (auto const* v = ctx_.sourceMap->tryGet(id)) sink(fmtLoc(*v));
        if (ctx_.ffiMap) if (auto const* v = ctx_.ffiMap->tryGet(id)) sink(fmtFfi(*v));
        if (ctx_.shaderMap) if (auto const* v = ctx_.shaderMap->tryGet(id)) sink(fmtShader(*v));
        if (ctx_.transpileMap) if (auto const* v = ctx_.transpileMap->tryGet(id)) sink(fmtTranspile(*v));
        if (ctx_.diagnosticMap) if (auto const* v = ctx_.diagnosticMap->tryGet(id)) sink(fmtDiag(*v));
    }

    [[nodiscard]] static std::string fmtLoc(HirSourceLoc const& v) {
        return std::format("@loc(buf {}, {}..{})", v.buffer.v, v.span.start(), v.span.end());
    }
    [[nodiscard]] static std::string fmtFfi(FfiMetadata const& v) {
        std::string s = "@ffi(";
        bool first = true;
        auto field = [&](std::string part) { if (!first) s += ", "; s += std::move(part); first = false; };
        if (!v.mangledName.empty()) field("name " + quote(v.mangledName));
        field(std::string("link ") + std::string(ffiLinkageName(v.linkage)));
        field(std::string("vis ") + std::string(ffiVisName(v.visibility)));
        if (!v.importLibrary.empty()) field("lib " + quote(v.importLibrary));
        if (!v.soname.empty()) field("soname " + quote(v.soname));
        s += ')';
        return s;
    }
    [[nodiscard]] static std::string fmtShader(ShaderIntrinsic const& v) {
        std::string s = "@shader(";
        bool first = true;
        auto field = [&](std::string part) { if (!first) s += ", "; s += std::move(part); first = false; };
        field(std::string("stage ") + std::string(stageName(v.stage)));
        if (v.builtin != ShaderBuiltin::None) field(std::string("builtin ") + std::string(builtinName(v.builtin)));
        if (v.workgroup.x != 1 || v.workgroup.y != 1 || v.workgroup.z != 1)
            field(std::format("wg {} {} {}", v.workgroup.x, v.workgroup.y, v.workgroup.z));
        if (v.binding.set != 0 || v.binding.binding != 0)
            field(std::format("binding {}:{}", v.binding.set, v.binding.binding));
        if (v.location != kUnsetShaderLocation) field(std::format("loc {}", v.location));
        s += ')';
        return s;
    }
    [[nodiscard]] static std::string fmtTranspile(TranspileHint const& v) {
        std::string s = "@transpile(";
        bool first = true;
        auto field = [&](std::string part) { if (!first) s += ", "; s += std::move(part); first = false; };
        if (!v.targetLanguage.empty()) field("target " + quote(v.targetLanguage));
        if (!v.overrideKind.empty()) field("override " + quote(v.overrideKind));
        if (v.idiom != TranspileIdiom::Default) field(std::string("idiom ") + std::string(idiomName(v.idiom)));
        s += ')';
        return s;
    }
    [[nodiscard]] std::string fmtDiag(DiagnosticInfo const& v) {
        std::string s = "@diag(";
        // code as decimal value: the forward `diagnosticCodeName` switch is the
        // single source of truth for the enum; a parallel name->code reverse table
        // would be a second thing to keep in sync (and rot). The numeric value is
        // stable and round-trips exactly.
        s += std::format("code {}", static_cast<std::uint32_t>(v.code));
        if (v.recovery != HirRecovery::None) { s += ", recovery "; s += recoveryName(v.recovery); }
        if (v.origin.valid()) {
            auto it = preIndex_.find(v.origin.v);
            if (it != preIndex_.end()) s += std::format(", origin {}", it->second);
        }
        if (!v.detail.empty()) { s += ", detail "; s += quote(v.detail); }
        s += ')';
        return s;
    }

    // ── per-node emission ────────────────────────────────────────────────────

    // A node on its own line(s) at `ind`. Used for decls, statements, and the
    // Extension/Error wildcards. Expression-kind nodes are emitted inline instead.
    void emitNodeLine(HirNodeId id, int ind) {
        if (isExprKind(hir_.kind(id))) {
            emitAttrsBlock(id, ind);
            out_ += indent(ind);
            emitExpr(id);
            out_ += '\n';
            return;
        }
        emitStmtLike(id, ind);
    }

    void emitStmtLike(HirNodeId id, int ind) {
        emitAttrsBlock(id, ind);
        out_ += indent(ind);
        HirFlags const f = hir_.flags(id);
        switch (hir_.kind(id)) {
            case HirKind::Module:
                report("nested Module is not representable"); out_ += "error\n"; return;
            case HirKind::Function: {
                out_ += "function"; out_ += flagsStr(f);
                out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
                appendType(hir_.functionSignature(id));
                out_ += " {\n";
                for (HirNodeId p : hir_.functionParams(id)) emitParam(p, ind + 1);
                emitNodeLine(hir_.functionBody(id), ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            }
            case HirKind::ExternFunction: {
                out_ += "extern_function"; out_ += flagsStr(f);
                out_ += std::format(" %{}", handleOf(hir_.payload(id)));
                if (hir_.externFunctionSignature(id).valid()) { out_ += " : "; appendType(hir_.externFunctionSignature(id)); }
                out_ += " {\n";
                for (HirNodeId p : hir_.externFunctionParams(id)) emitParam(p, ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            }
            case HirKind::Global: {
                out_ += "global"; out_ += flagsStr(f);
                out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
                appendType(hir_.globalType(id));
                if (auto init = hir_.globalInit(id)) { out_ += " = "; emitExpr(*init); }
                out_ += '\n';
                return;
            }
            case HirKind::TypeDecl:
                out_ += "type_decl"; out_ += flagsStr(f);
                out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
                appendType(hir_.typeDeclType(id)); out_ += '\n';
                return;
            case HirKind::ExternGlobal:
                out_ += "extern_global"; out_ += flagsStr(f);
                out_ += std::format(" %{}", handleOf(hir_.payload(id)));
                if (hir_.externGlobalType(id).valid()) { out_ += " : "; appendType(hir_.externGlobalType(id)); }
                out_ += '\n';
                return;
            case HirKind::ImportGroup:
                out_ += "import_group"; out_ += flagsStr(f); out_ += " {\n";
                for (HirNodeId m : hir_.importGroupMembers(id)) emitNodeLine(m, ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            case HirKind::Block:
                out_ += "block"; out_ += flagsStr(f); out_ += " {\n";
                for (HirNodeId s : hir_.children(id)) emitNodeLine(s, ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            case HirKind::IfStmt: {
                out_ += "if"; out_ += flagsStr(f); out_ += " (";
                emitExpr(hir_.ifCondition(id)); out_ += ")\n";
                emitNodeLine(hir_.ifThen(id), ind + 1);
                if (auto e = hir_.ifElse(id)) { out_ += indent(ind); out_ += "else\n"; emitNodeLine(*e, ind + 1); }
                return;
            }
            case HirKind::WhileStmt:
                out_ += "while"; out_ += flagsStr(f); out_ += " (";
                emitExpr(*hir_.loopCondition(id)); out_ += ")\n";
                emitNodeLine(hir_.loopBody(id), ind + 1);
                return;
            case HirKind::DoWhileStmt:
                out_ += "do"; out_ += flagsStr(f); out_ += "\n";
                emitNodeLine(hir_.loopBody(id), ind + 1);
                out_ += indent(ind); out_ += "while ("; emitExpr(*hir_.loopCondition(id)); out_ += ")\n";
                return;
            case HirKind::ForStmt: {
                out_ += "for"; out_ += flagsStr(f); out_ += " {\n";
                if (auto i = hir_.forInit(id))   { out_ += indent(ind + 1); out_ += "init:\n";   emitNodeLine(*i, ind + 2); }
                if (auto c = hir_.loopCondition(id)) { out_ += indent(ind + 1); out_ += "cond:\n"; emitNodeLine(*c, ind + 2); }
                if (auto u = hir_.forUpdate(id)) { out_ += indent(ind + 1); out_ += "update:\n"; emitNodeLine(*u, ind + 2); }
                out_ += indent(ind + 1); out_ += "body:\n"; emitNodeLine(hir_.loopBody(id), ind + 2);
                out_ += indent(ind); out_ += "}\n";
                return;
            }
            case HirKind::SwitchStmt: {
                out_ += "switch"; out_ += flagsStr(f); out_ += " (";
                emitExpr(hir_.switchDiscriminant(id)); out_ += ") {\n";
                for (HirNodeId arm : hir_.switchArms(id)) emitCaseArm(arm, ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            }
            case HirKind::BreakStmt: {
                out_ += "break"; out_ += flagsStr(f);
                if (hir_.branchDepth(id) != 0) out_ += std::format(" {}", hir_.branchDepth(id));
                out_ += '\n'; return;
            }
            case HirKind::ContinueStmt: {
                out_ += "continue"; out_ += flagsStr(f);
                if (hir_.branchDepth(id) != 0) out_ += std::format(" {}", hir_.branchDepth(id));
                out_ += '\n'; return;
            }
            case HirKind::ReturnStmt:
                out_ += "return"; out_ += flagsStr(f);
                if (auto v = hir_.returnValue(id)) { out_ += ' '; emitExpr(*v); }
                out_ += '\n'; return;
            case HirKind::ExprStmt:
                out_ += "expr"; out_ += flagsStr(f); out_ += ' '; emitExpr(hir_.exprStmtExpr(id)); out_ += '\n';
                return;
            case HirKind::VarDecl:
                out_ += "var"; out_ += flagsStr(f);
                out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
                appendType(hir_.varDeclType(id));
                if (auto init = hir_.varDeclInit(id)) { out_ += " = "; emitExpr(*init); }
                out_ += '\n'; return;
            case HirKind::AssignStmt:
                out_ += "assign"; out_ += flagsStr(f); out_ += ' ';
                emitExpr(hir_.assignTarget(id)); out_ += " = "; emitExpr(hir_.assignValue(id)); out_ += '\n';
                return;
            case HirKind::Unreachable:
                out_ += "unreachable"; out_ += flagsStr(f); out_ += '\n'; return;
            case HirKind::Error: case HirKind::Extension:
                emitExtOrError(id, /*inlineForm=*/false, ind); out_ += '\n'; return;
            default:
                report("unexpected node kind in statement position"); out_ += "error\n"; return;
        }
    }

    // A parameter VarDecl inside a (extern)function body.
    void emitParam(HirNodeId id, int ind) {
        emitAttrsBlock(id, ind);
        out_ += indent(ind);
        out_ += "param"; out_ += flagsStr(hir_.flags(id));
        out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
        appendType(hir_.varDeclType(id));
        if (auto init = hir_.varDeclInit(id)) { out_ += " = "; emitExpr(*init); }
        out_ += '\n';
    }

    void emitCaseArm(HirNodeId id, int ind) {
        emitAttrsBlock(id, ind);
        out_ += indent(ind);
        if (hir_.caseArmIsDefault(id)) {
            out_ += "default"; out_ += flagsStr(hir_.flags(id)); out_ += " {\n";
        } else {
            out_ += "case"; out_ += flagsStr(hir_.flags(id)); out_ += ' ';
            emitExpr(*hir_.caseArmValue(id)); out_ += " {\n";
        }
        for (HirNodeId s : hir_.caseArmBody(id)) emitNodeLine(s, ind + 1);
        out_ += indent(ind); out_ += "}\n";
    }

    // The `ext_node`/`error` wildcard form. Inline form (no indent/newline, for
    // expression position) vs block form (own line); both share the body shape.
    void emitExtOrError(HirNodeId id, bool inlineForm, int ind) {
        HirFlags const f = hir_.flags(id);
        if (hir_.kind(id) == HirKind::Extension) {
            std::string_view name = "?";
            std::uint32_t const p = hir_.payload(id);
            if (p >= kFirstHirExtensionKind) name = hir_.registry().descriptor(HirKindId{p}).name();
            else report("Extension node payload is not an extension kind id");
            out_ += "ext_node"; out_ += flagsStr(f); out_ += ' '; out_ += quote(name);
        } else {
            out_ += "error"; out_ += flagsStr(f);
        }
        if (hir_.typeId(id).valid()) { out_ += " : "; appendType(hir_.typeId(id)); }
        auto kids = hir_.children(id);
        if (!kids.empty()) {
            out_ += " {\n";
            int const childInd = inlineForm ? ind : ind + 1;
            for (HirNodeId c : kids) emitNodeLine(c, childInd);
            out_ += indent(inlineForm ? 0 : ind); out_ += "}";
        }
    }

    // An expression, inline (no leading indent / trailing newline). Children in
    // comma-separated parens.
    void emitExpr(HirNodeId id) {
        out_ += attrsInline(id);
        HirFlags const f = hir_.flags(id);
        auto operands = [&](std::span<HirNodeId const> kids) {
            out_ += '(';
            bool first = true;
            for (HirNodeId k : kids) { if (!first) out_ += ", "; emitExpr(k); first = false; }
            out_ += ')';
        };
        auto header = [&](char const* kw) { out_ += kw; out_ += flagsStr(f); };
        auto typed = [&](char const* kw) { header(kw); out_ += " : "; appendType(hir_.typeId(id)); };
        // The `kw : type (operands...)` family — must mirror the parser's
        // kTypedExprs table so the two sides can't drift.
        auto typedCall = [&](char const* kw) { typed(kw); out_ += ' '; operands(hir_.children(id)); };
        switch (hir_.kind(id)) {
            case HirKind::Literal:
                header("lit"); out_ += std::format(" #{} : ", hir_.payload(id)); appendType(hir_.typeId(id)); return;
            case HirKind::Ref:
                header("ref"); out_ += std::format(" %{} : ", handleOf(hir_.payload(id))); appendType(hir_.typeId(id)); return;
            case HirKind::IntrinsicCall: {
                header("intrinsic"); out_ += ' ';
                std::uint32_t const p = hir_.payload(id);
                if (hir_.intrinsicRegistry().contains(HirIntrinsicId{p}))
                    out_ += quote(hir_.intrinsicRegistry().descriptor(HirIntrinsicId{p}).name());
                else { report("IntrinsicCall payload is not a registered intrinsic"); out_ += "\"?\""; }
                out_ += " : "; appendType(hir_.typeId(id)); out_ += ' '; operands(hir_.children(id)); return;
            }
            case HirKind::BinaryOp: case HirKind::UnaryOp: {
                header(hir_.kind(id) == HirKind::BinaryOp ? "binop" : "unop");
                out_ += ' '; appendOpName(hir_.payload(id));
                out_ += " : "; appendType(hir_.typeId(id)); out_ += ' '; operands(hir_.children(id)); return;
            }
            case HirKind::MemberAccess:
                header("member"); out_ += std::format(" #{} : ", hir_.payload(id)); appendType(hir_.typeId(id));
                out_ += ' '; operands(hir_.children(id)); return;
            case HirKind::Swizzle:
                header("swizzle"); out_ += std::format(" #{} : ", hir_.payload(id)); appendType(hir_.typeId(id));
                out_ += ' '; operands(hir_.children(id)); return;
            case HirKind::Call:               typedCall("call"); return;
            case HirKind::Cast:               typedCall("cast"); return;
            case HirKind::Index:              typedCall("index"); return;
            case HirKind::ConstructAggregate: typedCall("construct"); return;
            case HirKind::Ternary:            typedCall("ternary"); return;
            case HirKind::LogicalAnd:         typedCall("logical_and"); return;
            case HirKind::LogicalOr:          typedCall("logical_or"); return;
            case HirKind::SizeOf:             typedCall("sizeof"); return;
            case HirKind::AddressOf:          typedCall("addressof"); return;
            case HirKind::Deref:              typedCall("deref"); return;
            case HirKind::SeqExpr: {
                // `seq : type { <stmt-lines> yield <resultExpr> }` — the
                // statement children render as normal statement lines; the
                // result (last child) is the yielded value. Mirrors the
                // inline-brace form `error`/`ext_node` use.
                typed("seq"); out_ += " {\n";
                for (HirNodeId s : hir_.seqExprStmts(id)) emitNodeLine(s, 1);
                out_ += indent(1); out_ += "yield "; emitExpr(hir_.seqExprResult(id));
                out_ += "\n}"; return;
            }
            case HirKind::TypeRef:    typed("typeref"); return;
            case HirKind::Error: case HirKind::Extension:
                emitExtOrError(id, /*inlineForm=*/true, 0); return;
            default: report("unexpected node kind in expression position"); out_ += "error"; return;
        }
    }

    void appendOpName(std::uint32_t payload) {
        if (isCoreOp(payload)) out_ += opName(decodeCoreOp(payload));
        else { out_ += "ext "; out_ += quote(hir_.opRegistry().descriptor(decodeExtOp(payload)).name()); }
    }

    // ── preamble sections ────────────────────────────────────────────────────
    void emitExtKinds() {
        auto ext = hir_.registry().extensions();
        if (ext.empty()) return;
        out_ += "ext_kinds {\n";
        for (auto const& d : ext) {
            out_ += "  "; out_ += quote(d.name());
            out_ += " lang "; out_ += quote(d.sourceLanguage()); out_ += '\n';
        }
        out_ += "}\n";
    }
    void emitExtOps() {
        auto ext = hir_.opRegistry().extensions();
        if (ext.empty()) return;
        out_ += "ext_ops {\n";
        for (auto const& d : ext) {
            out_ += "  "; out_ += quote(d.name());
            out_ += (d.arity() == HirOpArity::Binary) ? " binary" : " unary";
            out_ += " lang "; out_ += quote(d.sourceLanguage()); out_ += '\n';
        }
        out_ += "}\n";
    }
    void emitIntrinsics() {
        auto ins = hir_.intrinsicRegistry().intrinsics();
        if (ins.empty()) return;
        out_ += "intrinsics {\n";
        for (auto const& d : ins) {
            out_ += "  "; out_ += quote(d.name());
            out_ += " lang "; out_ += quote(d.sourceLanguage()); out_ += '\n';
        }
        out_ += "}\n";
    }
    void emitSymbols() {
        if (symOrder_.empty()) return;
        out_ += "symbols {\n";
        for (std::uint32_t symv : symOrder_) {
            std::string_view name;
            if (ctx_.symbolNames && symv < ctx_.symbolNames->size()) name = (*ctx_.symbolNames)[symv];
            out_ += std::format("  %{} ", handleOf(symv));
            out_ += quote(name); out_ += '\n';
        }
        out_ += "}\n";
    }
};

} // namespace

std::string emitHir(Hir const& hir, HirTextContext const& ctx, DiagnosticReporter& reporter) {
    return Emitter{hir, ctx, reporter}.run();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parser
// ─────────────────────────────────────────────────────────────────────────────

namespace {

enum class Tk : std::uint8_t {
    Eof, Unknown, Ident, Int, Str,
    LBrace, RBrace, LParen, RParen, LAngle, RAngle, LBrack, RBrack,
    Colon, Comma, Percent, Hash, Equal, Arrow, Minus, DotDot, At,
};

struct Tok {
    Tk          kind = Tk::Eof;
    std::string text;          // ident/str content (unescaped) or int digits
    std::uint64_t num = 0;     // for Int
    std::uint32_t off = 0;     // byte offset (diagnostics)
};

class Lexer {
public:
    explicit Lexer(std::string_view src) : s_(src) { advance(); }

    [[nodiscard]] Tok const& peek() const { return cur_; }
    Tok take() { Tok t = std::move(cur_); advance(); return t; }

private:
    std::string_view s_;
    std::size_t      p_ = 0;
    Tok              cur_;

    void advance() {
        skipTrivia();
        cur_ = Tok{};
        cur_.off = static_cast<std::uint32_t>(p_);
        if (p_ >= s_.size()) { cur_.kind = Tk::Eof; return; }
        char const c = s_[p_];
        if (isIdentStart(c)) { lexIdent(); return; }
        if (isDigit(c))      { lexInt(); return; }
        if (c == '"')        { lexStr(); return; }
        // punctuation
        ++p_;
        switch (c) {
            case '{': cur_.kind = Tk::LBrace; return;
            case '}': cur_.kind = Tk::RBrace; return;
            case '(': cur_.kind = Tk::LParen; return;
            case ')': cur_.kind = Tk::RParen; return;
            case '<': cur_.kind = Tk::LAngle; return;
            case '>': cur_.kind = Tk::RAngle; return;
            case '[': cur_.kind = Tk::LBrack; return;
            case ']': cur_.kind = Tk::RBrack; return;
            case ':': cur_.kind = Tk::Colon; return;
            case ',': cur_.kind = Tk::Comma; return;
            case '%': cur_.kind = Tk::Percent; return;
            case '#': cur_.kind = Tk::Hash; return;
            case '@': cur_.kind = Tk::At; return;
            case '=': cur_.kind = Tk::Equal; return;
            case '-':
                if (p_ < s_.size() && s_[p_] == '>') { ++p_; cur_.kind = Tk::Arrow; }
                else cur_.kind = Tk::Minus;
                return;
            case '.':
                if (p_ < s_.size() && s_[p_] == '.') { ++p_; cur_.kind = Tk::DotDot; return; }
                cur_.kind = Tk::Unknown; return;   // stray '.' — distinct from EOF so it can't truncate the parse
            default: cur_.kind = Tk::Unknown; return;  // unknown byte; the parser reports + recovers, never mistakes it for EOF
        }
    }

    static bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c) || c == '.'; }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }

    void skipTrivia() {
        for (;;) {
            while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
            if (p_ + 1 < s_.size() && s_[p_] == '/' && s_[p_ + 1] == '/') {
                p_ += 2;
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
                continue;
            }
            break;
        }
    }
    void lexIdent() {
        cur_.kind = Tk::Ident;
        std::size_t const start = p_;
        // identifiers may carry '.' (e.g. intrinsic-like keywords aren't used, but
        // type/builtin names stay quoted, so '.' here is harmless and unused).
        while (p_ < s_.size() && isIdentCont(s_[p_])) ++p_;
        cur_.text.assign(s_.substr(start, p_ - start));
    }
    void lexInt() {
        cur_.kind = Tk::Int;
        std::size_t const start = p_;
        std::uint64_t v = 0;
        while (p_ < s_.size() && isDigit(s_[p_])) { v = v * 10 + static_cast<std::uint64_t>(s_[p_] - '0'); ++p_; }
        cur_.num = v;
        cur_.text.assign(s_.substr(start, p_ - start));
    }
    void lexStr() {
        cur_.kind = Tk::Str;
        ++p_;  // opening quote
        std::string out;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\' && p_ < s_.size()) c = s_[p_++];  // \" and \\ (and any escaped char)
            out += c;
        }
        if (p_ < s_.size()) ++p_;  // closing quote
        cur_.text = std::move(out);
    }
};

// Attributes parsed ahead of a node (the node id is known only after building).
struct PendingAttrs {
    std::optional<HirSourceLoc>  loc;
    std::optional<FfiMetadata>   ffi;
    std::optional<ShaderIntrinsic> shader;
    std::optional<TranspileHint> transpile;
    std::optional<DiagnosticInfo> diag;
    bool          diagHasOrigin = false;
    std::uint32_t diagOriginPre = 0;   // pre-order index of the origin node
};

class Parser {
public:
    Parser(std::string_view text, CompilationUnitId cuId, DiagnosticReporter& reporter)
        : interner_(cuId), lex_(text), reporter_(reporter) {}

    HirBuilder              builder_;
    TypeInterner            interner_;
    TypeRegistry            typeReg_;
    std::vector<std::string> symbolNames_;     // SymbolId.v -> name; slot 0 unused

    std::vector<std::pair<HirNodeId, HirSourceLoc>>   pLoc_;
    std::vector<std::pair<HirNodeId, FfiMetadata>>    pFfi_;
    std::vector<std::pair<HirNodeId, ShaderIntrinsic>> pShader_;
    std::vector<std::pair<HirNodeId, TranspileHint>>  pTranspile_;
    struct DiagPend { HirNodeId node; DiagnosticInfo info; bool hasOrigin; std::uint32_t originPre; };
    std::vector<DiagPend> pDiag_;
    std::vector<HirNodeId> indexToId_;          // pre-order index -> built node

    // Parse the whole file. Returns the module root (invalid on a fatal header
    // error). Populates the builder, interner, side-table pending lists.
    [[nodiscard]] HirNodeId parse() {
        // header: "dsshir" INT
        if (!acceptKeyword("dsshir")) { malformed("expected 'dsshir' header"); return InvalidHirNode; }
        if (lex_.peek().kind != Tk::Int) { malformed("expected format version"); return InvalidHirNode; }
        std::uint64_t const version = lex_.take().num;
        if (version != 1) {
            ParseDiagnostic d; d.code = DiagnosticCode::H_TextVersionMismatch;
            d.severity = DiagnosticSeverity::Error;
            d.actual = std::format("dsshir format version {} (this build understands 1)", version);
            reporter_.report(std::move(d));
            return InvalidHirNode;
        }
        parsePreamble();
        return parseModule();
    }

private:
    Lexer               lex_;
    DiagnosticReporter& reporter_;
    std::unordered_map<std::string, HirKindId>      extKindByName_;
    std::unordered_map<std::string, HirOpId>        extOpByName_;
    std::unordered_map<std::string, HirIntrinsicId> intrinsicByName_;
    std::uint32_t preCounter_ = 0;

    // ── token helpers ────────────────────────────────────────────────────────
    [[nodiscard]] Tk peekKind() const { return lex_.peek().kind; }
    [[nodiscard]] bool peekIs(Tk k) const { return lex_.peek().kind == k; }
    [[nodiscard]] bool peekKeyword(std::string_view kw) const {
        return lex_.peek().kind == Tk::Ident && lex_.peek().text == kw;
    }
    bool accept(Tk k) { if (peekIs(k)) { lex_.take(); return true; } return false; }
    bool acceptKeyword(std::string_view kw) { if (peekKeyword(kw)) { lex_.take(); return true; } return false; }
    void expect(Tk k, char const* what) { if (!accept(k)) malformed(std::format("expected {}", what)); }
    [[nodiscard]] std::string takeIdent() {
        if (peekIs(Tk::Ident)) return lex_.take().text;
        malformed("expected identifier"); return {};
    }
    [[nodiscard]] std::string takeStr() {
        if (peekIs(Tk::Str)) return lex_.take().text;
        malformed("expected string literal"); return {};
    }
    [[nodiscard]] std::uint64_t takeInt() {
        if (peekIs(Tk::Int)) return lex_.take().num;
        malformed("expected integer"); return 0;
    }
    void malformed(std::string detail) {
        ParseDiagnostic d; d.code = DiagnosticCode::H_TextMalformed;
        d.severity = DiagnosticSeverity::Error;
        d.span = SourceSpan::empty(lex_.peek().off);
        d.actual = std::move(detail);
        reporter_.report(std::move(d));
    }
    void unknownName(std::string detail) {
        ParseDiagnostic d; d.code = DiagnosticCode::H_TextUnknownName;
        d.severity = DiagnosticSeverity::Error;
        d.span = SourceSpan::empty(lex_.peek().off);
        d.actual = std::move(detail);
        reporter_.report(std::move(d));
    }
    // Resolve a parsed enum-name `opt`; report (not silently default) on a miss so
    // an unrecognized token can never coerce to a wrong value and silently diverge
    // on re-emit. Mirrors parseOp's unknown-operator handling.
    template <class E>
    [[nodiscard]] E orMalformed(std::optional<E> opt, std::string_view name,
                                char const* what, E dflt) {
        if (opt) return *opt;
        malformed(std::format("unknown {} '{}'", what, name));
        return dflt;
    }
    void recordIndex(std::uint32_t idx, HirNodeId id) {
        if (idx >= indexToId_.size()) indexToId_.resize(idx + 1);
        indexToId_[idx] = id;
    }

    // ── preamble ───────────────────────────────────────────────────────────
    void parsePreamble() {
        for (;;) {
            if (acceptKeyword("ext_kinds")) parseExtKinds();
            else if (acceptKeyword("ext_ops")) parseExtOps();
            else if (acceptKeyword("intrinsics")) parseIntrinsics();
            else if (acceptKeyword("symbols")) parseSymbols();
            else break;
        }
    }
    void parseExtKinds() {
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            std::string name = takeStr();
            if (!acceptKeyword("lang")) malformed("expected 'lang'");
            std::string lang = takeStr();
            HirKindId id = builder_.registry().registerExtension(name, lang);
            extKindByName_.emplace(std::move(name), id);
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
    }
    void parseExtOps() {
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            std::string name = takeStr();
            HirOpArity arity = HirOpArity::Binary;
            if (acceptKeyword("binary")) arity = HirOpArity::Binary;
            else if (acceptKeyword("unary")) arity = HirOpArity::Unary;
            else malformed("expected 'binary' or 'unary'");
            if (!acceptKeyword("lang")) malformed("expected 'lang'");
            std::string lang = takeStr();
            HirOpId id = builder_.opRegistry().registerExtension(name, arity, lang);
            extOpByName_.emplace(std::move(name), id);
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
    }
    void parseIntrinsics() {
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            std::string name = takeStr();
            if (!acceptKeyword("lang")) malformed("expected 'lang'");
            std::string lang = takeStr();
            HirIntrinsicId id = builder_.intrinsicRegistry().registerIntrinsic(name, lang);
            intrinsicByName_.emplace(std::move(name), id);
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
    }
    void parseSymbols() {
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            expect(Tk::Percent, "'%'");
            std::uint64_t const handle = takeInt();
            std::string name = takeStr();
            if (handle >= symbolNames_.size()) symbolNames_.resize(handle + 1);
            symbolNames_[handle] = std::move(name);
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
    }

    // ── module ───────────────────────────────────────────────────────────────
    [[nodiscard]] HirNodeId parseModule() {
        if (!acceptKeyword("module")) { malformed("expected 'module'"); return InvalidHirNode; }
        std::uint32_t const idx = preCounter_++;
        HirFlags const flags = parseFlags();
        std::string lang = takeStr();
        builder_.setSourceLanguage(std::move(lang));  // frozen into Hir at finish()
        expect(Tk::LBrace, "'{'");
        std::vector<HirNodeId> decls;
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            decls.push_back(parseNode());
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
        HirNodeId const root = builder_.makeModule(decls, flags);
        recordIndex(idx, root);
        return root;
    }

    // ── flags / attrs ──────────────────────────────────────────────────────
    [[nodiscard]] HirFlags parseFlags() {
        HirFlags f = HirFlags::None;
        if (!accept(Tk::LBrack)) return f;
        while (!peekIs(Tk::RBrack) && !peekIs(Tk::Eof)) {
            std::string n = takeIdent();
            if (n == "err") f |= HirFlags::HasError;
            else if (n == "syn") f |= HirFlags::Synthetic;
            else if (n == "shader") f |= HirFlags::ShaderUsable;
            else if (n == "host") f |= HirFlags::HostUsable;
            else malformed(std::format("unknown flag '{}'", n));
            if (!accept(Tk::Comma)) break;
        }
        expect(Tk::RBrack, "']'");
        return f;
    }

    [[nodiscard]] PendingAttrs parseAttrs() {
        PendingAttrs a;
        while (accept(Tk::At)) {
            std::string kind = takeIdent();
            expect(Tk::LParen, "'('");
            if (kind == "loc") a.loc = parseLoc();
            else if (kind == "ffi") a.ffi = parseFfi();
            else if (kind == "shader") a.shader = parseShader();
            else if (kind == "transpile") a.transpile = parseTranspile();
            else if (kind == "diag") parseDiag(a);
            else { malformed(std::format("unknown attribute '@{}'", kind)); skipToRParen(); }
            expect(Tk::RParen, "')'");
        }
        return a;
    }
    void skipToRParen() { while (!peekIs(Tk::RParen) && !peekIs(Tk::Eof)) lex_.take(); }

    [[nodiscard]] HirSourceLoc parseLoc() {
        if (!acceptKeyword("buf")) malformed("expected 'buf'");
        std::uint32_t buf = static_cast<std::uint32_t>(takeInt());
        expect(Tk::Comma, "','");
        std::uint32_t s = static_cast<std::uint32_t>(takeInt());
        expect(Tk::DotDot, "'..'");
        std::uint32_t e = static_cast<std::uint32_t>(takeInt());
        return HirSourceLoc{BufferId{buf}, SourceSpan::of(s, e)};
    }
    [[nodiscard]] FfiMetadata parseFfi() {
        FfiMetadata m;
        for (;;) {
            if (acceptKeyword("name")) m.mangledName = takeStr();
            else if (acceptKeyword("link")) { std::string n = takeIdent(); m.linkage = orMalformed(ffiLinkageFromName(n), n, "ffi linkage", FfiLinkage::Strong); }
            else if (acceptKeyword("vis")) { std::string n = takeIdent(); m.visibility = orMalformed(ffiVisFromName(n), n, "ffi visibility", FfiVisibility::Default); }
            else if (acceptKeyword("lib")) m.importLibrary = takeStr();
            else if (acceptKeyword("soname")) m.soname = takeStr();
            else break;
            if (!accept(Tk::Comma)) break;
        }
        return m;
    }
    [[nodiscard]] ShaderIntrinsic parseShader() {
        ShaderIntrinsic m;
        for (;;) {
            if (acceptKeyword("stage")) { std::string n = takeIdent(); m.stage = orMalformed(stageFromName(n), n, "shader stage", ShaderStage::None); }
            else if (acceptKeyword("builtin")) { std::string n = takeIdent(); m.builtin = orMalformed(builtinFromName(n), n, "shader builtin", ShaderBuiltin::None); }
            else if (acceptKeyword("wg")) {
                m.workgroup.x = static_cast<std::uint32_t>(takeInt());
                m.workgroup.y = static_cast<std::uint32_t>(takeInt());
                m.workgroup.z = static_cast<std::uint32_t>(takeInt());
            } else if (acceptKeyword("binding")) {
                m.binding.set = static_cast<std::uint32_t>(takeInt());
                expect(Tk::Colon, "':'");
                m.binding.binding = static_cast<std::uint32_t>(takeInt());
            } else if (acceptKeyword("loc")) {
                m.location = static_cast<std::uint32_t>(takeInt());
            } else break;
            if (!accept(Tk::Comma)) break;
        }
        return m;
    }
    [[nodiscard]] TranspileHint parseTranspile() {
        TranspileHint m;
        for (;;) {
            if (acceptKeyword("target")) m.targetLanguage = takeStr();
            else if (acceptKeyword("override")) m.overrideKind = takeStr();
            else if (acceptKeyword("idiom")) { std::string n = takeIdent(); m.idiom = orMalformed(idiomFromName(n), n, "transpile idiom", TranspileIdiom::Default); }
            else break;
            if (!accept(Tk::Comma)) break;
        }
        return m;
    }
    void parseDiag(PendingAttrs& a) {
        DiagnosticInfo info;
        for (;;) {
            if (acceptKeyword("code")) info.code = static_cast<DiagnosticCode>(static_cast<std::uint16_t>(takeInt()));
            else if (acceptKeyword("recovery")) { std::string n = takeIdent(); info.recovery = orMalformed(recoveryFromName(n), n, "diag recovery", HirRecovery::None); }
            else if (acceptKeyword("origin")) { a.diagHasOrigin = true; a.diagOriginPre = static_cast<std::uint32_t>(takeInt()); }
            else if (acceptKeyword("detail")) info.detail = takeStr();
            else break;
            if (!accept(Tk::Comma)) break;
        }
        a.diag = std::move(info);
    }
    void applyAttrs(HirNodeId id, PendingAttrs&& a) {
        if (a.loc) pLoc_.push_back({id, *a.loc});
        if (a.ffi) pFfi_.push_back({id, std::move(*a.ffi)});
        if (a.shader) pShader_.push_back({id, *a.shader});
        if (a.transpile) pTranspile_.push_back({id, std::move(*a.transpile)});
        if (a.diag) pDiag_.push_back({id, std::move(*a.diag), a.diagHasOrigin, a.diagOriginPre});
    }

    // ── node dispatch ─────────────────────────────────────────────────────────

    // Byte offset of the current token — the basis of every list-loop progress
    // guard: if an iteration leaves the cursor unmoved, a malformed/stuck token
    // is force-skipped so collect-all parsing can never spin.
    [[nodiscard]] std::uint32_t cursorOff() const { return lex_.peek().off; }

    [[nodiscard]] static bool isExprKeyword(std::string_view kw) {
        return kw == "lit" || kw == "ref" || kw == "call" || kw == "intrinsic"
            || kw == "binop" || kw == "unop" || kw == "cast" || kw == "member"
            || kw == "index" || kw == "swizzle" || kw == "construct" || kw == "ternary"
            || kw == "logical_and" || kw == "logical_or" || kw == "sizeof"
            || kw == "addressof" || kw == "deref" || kw == "seq" || kw == "typeref";
    }

    // Parse any node (decl/stmt/expr/wildcard). Handles attrs + pre-order index.
    HirNodeId parseNode() {
        PendingAttrs attrs = parseAttrs();
        std::uint32_t const idx = preCounter_++;
        std::string_view kw = peekIs(Tk::Ident) ? std::string_view{lex_.peek().text} : std::string_view{};
        HirNodeId id = isExprKeyword(kw) ? parseExprInner() : parseStmtInner();
        recordIndex(idx, id);
        applyAttrs(id, std::move(attrs));
        return id;
    }

    [[nodiscard]] std::vector<HirNodeId> parseParenOperands() {
        std::vector<HirNodeId> kids;
        expect(Tk::LParen, "'('");
        while (!peekIs(Tk::RParen) && !peekIs(Tk::Eof)) {
            kids.push_back(parseNode());
            if (!accept(Tk::Comma)) break;
        }
        expect(Tk::RParen, "')'");
        return kids;
    }
    [[nodiscard]] std::vector<HirNodeId> parseBraceNodes() {
        std::vector<HirNodeId> kids;
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            kids.push_back(parseNode());
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
        return kids;
    }

    [[nodiscard]] std::uint32_t parseSymHandle() {
        expect(Tk::Percent, "'%'");
        std::uint32_t const h = static_cast<std::uint32_t>(takeInt());
        if (h == 0 || h >= symbolNames_.size())
            unknownName(std::format("symbol handle %{} not declared in 'symbols'", h));
        return h;
    }
    [[nodiscard]] TypeId parseTypeAnnot() { expect(Tk::Colon, "':'"); return parseType(); }

    // ── expressions ──────────────────────────────────────────────────────────
    HirNodeId parseExprInner() {
        std::string kw = takeIdent();
        HirFlags flags = parseFlags();
        if (kw == "lit") { expect(Tk::Hash, "'#'"); std::uint32_t i = static_cast<std::uint32_t>(takeInt());
            TypeId t = parseTypeAnnot(); return builder_.makeLiteral(t, i, flags); }
        if (kw == "ref") { std::uint32_t h = parseSymHandle(); TypeId t = parseTypeAnnot();
            return builder_.makeRef(t, h, flags); }
        if (kw == "call") { TypeId t = parseTypeAnnot(); auto kids = parseParenOperands();
            if (kids.empty()) { malformed("call needs a callee"); return builder_.addLeaf(HirKind::Error, t, 0, flags); }
            HirNodeId callee = kids.front();
            std::vector<HirNodeId> args(kids.begin() + 1, kids.end());
            return builder_.makeCall(callee, args, t, flags); }
        if (kw == "intrinsic") { std::string name = takeStr(); TypeId t = parseTypeAnnot();
            auto kids = parseParenOperands();
            auto it = intrinsicByName_.find(name);
            std::uint32_t iid = 0;
            if (it != intrinsicByName_.end()) iid = it->second.v;
            else unknownName(std::format("intrinsic \"{}\" not declared", name));
            return builder_.makeIntrinsicCall(iid, kids, t, flags); }
        if (kw == "binop" || kw == "unop") {
            std::uint32_t payload = 0; (void)parseOp(payload);
            TypeId t = parseTypeAnnot(); auto kids = parseParenOperands();
            return builder_.addParent(kw == "binop" ? HirKind::BinaryOp : HirKind::UnaryOp, kids, t, payload, flags);
        }
        if (kw == "member") { expect(Tk::Hash, "'#'"); std::uint32_t fi = static_cast<std::uint32_t>(takeInt());
            TypeId t = parseTypeAnnot(); auto k = parseParenOperands();
            return builder_.addParent(HirKind::MemberAccess, k, t, fi, flags); }
        if (kw == "swizzle") { expect(Tk::Hash, "'#'"); std::uint32_t m = static_cast<std::uint32_t>(takeInt());
            TypeId t = parseTypeAnnot(); auto k = parseParenOperands();
            return builder_.addParent(HirKind::Swizzle, k, t, m, flags); }
        if (kw == "typeref") { TypeId t = parseTypeAnnot(); return builder_.makeTypeRef(t, flags); }
        if (kw == "seq") {
            // seq : type { <stmt-lines> yield <resultExpr> }
            TypeId t = parseTypeAnnot();
            expect(Tk::LBrace, "'{'");
            std::vector<HirNodeId> stmts;
            while (!peekKeyword("yield") && !peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
                std::uint32_t const off = cursorOff();
                stmts.push_back(parseNode());
                if (cursorOff() == off) lex_.take();   // progress guard
            }
            if (!acceptKeyword("yield")) malformed("seq body needs a 'yield <expr>'");
            HirNodeId result = parseNode();
            expect(Tk::RBrace, "'}'");
            return builder_.makeSeqExpr(stmts, result, t, flags);
        }
        // The `kw : type (operands...)` family — mirror of the emitter's typedCall
        // list. Payload is always 0; arity is enforced later by the verifier.
        static constexpr std::pair<std::string_view, HirKind> kTypedExprs[] = {
            {"cast", HirKind::Cast}, {"index", HirKind::Index},
            {"construct", HirKind::ConstructAggregate}, {"ternary", HirKind::Ternary},
            {"logical_and", HirKind::LogicalAnd}, {"logical_or", HirKind::LogicalOr},
            {"sizeof", HirKind::SizeOf}, {"addressof", HirKind::AddressOf}, {"deref", HirKind::Deref},
        };
        for (auto const& [k, kind] : kTypedExprs)
            if (kw == k) { TypeId t = parseTypeAnnot(); auto ops = parseParenOperands();
                return builder_.addParent(kind, ops, t, 0, flags); }
        malformed(std::format("unknown expression '{}'", kw));
        return builder_.addLeaf(HirKind::Error, InvalidType, 0, flags);
    }

    // Parse an op name into a node payload (core HirOpKind or registered HirOpId).
    void parseOp(std::uint32_t& payload) {
        if (acceptKeyword("ext")) {
            std::string name = takeStr();
            auto it = extOpByName_.find(name);
            if (it != extOpByName_.end()) payload = it->second.v;
            else { unknownName(std::format("operator \"{}\" not declared", name)); payload = 0; }
            return;
        }
        std::string name = takeIdent();
        if (auto op = coreOpFromName(name)) payload = encodeOp(*op);
        else { malformed(std::format("unknown operator '{}'", name)); payload = 0; }
    }

    // ── statements / decls / wildcards ────────────────────────────────────────
    HirNodeId parseStmtInner() {
        if (!peekIs(Tk::Ident)) { malformed("expected a statement"); return builder_.addLeaf(HirKind::Error); }
        std::string kw = takeIdent();
        HirFlags flags = parseFlags();
        if (kw == "block") { auto k = parseBraceNodes(); return builder_.makeBlock(k, flags); }
        if (kw == "if") {
            expect(Tk::LParen, "'('"); HirNodeId cond = parseNode(); expect(Tk::RParen, "')'");
            HirNodeId then = parseNode();
            std::optional<HirNodeId> els;
            if (acceptKeyword("else")) els = parseNode();
            return builder_.makeIfStmt(cond, then, els, flags);
        }
        if (kw == "while") {
            expect(Tk::LParen, "'('"); HirNodeId cond = parseNode(); expect(Tk::RParen, "')'");
            HirNodeId body = parseNode();
            return builder_.makeWhileStmt(cond, body, flags);
        }
        if (kw == "do") {
            HirNodeId body = parseNode();
            if (!acceptKeyword("while")) malformed("expected 'while'");
            expect(Tk::LParen, "'('"); HirNodeId cond = parseNode(); expect(Tk::RParen, "')'");
            return builder_.makeDoWhileStmt(body, cond, flags);
        }
        if (kw == "for") return parseFor(flags);
        if (kw == "switch") {
            expect(Tk::LParen, "'('"); HirNodeId disc = parseNode(); expect(Tk::RParen, "')'");
            expect(Tk::LBrace, "'{'");
            std::vector<HirNodeId> arms;
            while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
                std::uint32_t const off = cursorOff();
                arms.push_back(parseNode());
                if (cursorOff() == off) lex_.take();
            }
            expect(Tk::RBrace, "'}'");
            return builder_.makeSwitchStmt(disc, arms, flags);
        }
        if (kw == "case") { HirNodeId v = parseNode(); auto body = parseBraceNodes();
            return builder_.makeCaseArm(v, body, flags); }
        if (kw == "default") { auto body = parseBraceNodes(); return builder_.makeCaseArm(std::nullopt, body, flags); }
        if (kw == "break") { std::uint32_t d = peekIs(Tk::Int) ? static_cast<std::uint32_t>(takeInt()) : 0u;
            return builder_.makeBreak(d, flags); }
        if (kw == "continue") { std::uint32_t d = peekIs(Tk::Int) ? static_cast<std::uint32_t>(takeInt()) : 0u;
            return builder_.makeContinue(d, flags); }
        if (kw == "return") {
            // A return value may carry inline attributes (`return @loc(...) expr`).
            // A value-less `return` is always block-terminal (nothing may follow
            // it — checkBlockTermination), so a leading `@` here unambiguously
            // introduces an attributed value, never the next statement's attrs.
            if (peekIs(Tk::At) || startsExpr()) { HirNodeId v = parseNode(); return builder_.makeReturn(v, flags); }
            return builder_.makeReturn(std::nullopt, flags);
        }
        if (kw == "expr") { HirNodeId e = parseNode(); return builder_.makeExprStmt(e, flags); }
        if (kw == "var" || kw == "param") return parseVarLike(flags);
        if (kw == "assign") { HirNodeId tgt = parseNode(); expect(Tk::Equal, "'='"); HirNodeId val = parseNode();
            return builder_.makeAssignStmt(tgt, val, flags); }
        if (kw == "unreachable") return builder_.addLeaf(HirKind::Unreachable, InvalidType, 0, flags);
        if (kw == "function") return parseFunction(flags);
        if (kw == "extern_function") return parseExternFunction(flags);
        if (kw == "global") {
            std::uint32_t sym = parseSymHandle(); TypeId t = parseTypeAnnot();
            std::optional<HirNodeId> init;
            if (accept(Tk::Equal)) init = parseNode();
            return builder_.makeGlobal(t, sym, init, flags);
        }
        if (kw == "type_decl") { std::uint32_t sym = parseSymHandle(); TypeId t = parseTypeAnnot();
            return builder_.makeTypeDecl(t, sym, flags); }
        if (kw == "extern_global") {
            std::uint32_t sym = parseSymHandle();
            TypeId t = accept(Tk::Colon) ? parseType() : InvalidType;
            return builder_.makeExternGlobal(t, sym, flags);
        }
        if (kw == "import_group") { auto m = parseBraceNodes(); return builder_.makeImportGroup(m, flags); }
        if (kw == "ext_node") return parseExtNode(flags);
        if (kw == "error") return parseErrorNode(flags);
        malformed(std::format("unknown statement '{}'", kw));
        return builder_.addLeaf(HirKind::Error, InvalidType, 0, flags);
    }

    [[nodiscard]] bool startsExpr() {
        if (!peekIs(Tk::Ident)) return false;
        return isExprKeyword(lex_.peek().text);
    }

    HirNodeId parseVarLike(HirFlags flags) {
        std::uint32_t sym = parseSymHandle();
        TypeId t = parseTypeAnnot();
        std::optional<HirNodeId> init;
        if (accept(Tk::Equal)) init = parseNode();
        return builder_.makeVarDecl(t, sym, init, flags);
    }

    HirNodeId parseFor(HirFlags flags) {
        expect(Tk::LBrace, "'{'");
        std::optional<HirNodeId> init, cond, update, body;
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            std::string role = takeIdent();
            expect(Tk::Colon, "':'");
            HirNodeId n = parseNode();
            if (role == "init") init = n;
            else if (role == "cond") cond = n;
            else if (role == "update") update = n;
            else if (role == "body") body = n;
            else malformed(std::format("unknown for-clause '{}'", role));
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
        if (!body) { malformed("for is missing a body"); body = builder_.addLeaf(HirKind::Error); }
        return builder_.makeForStmt(init, cond, update, *body, flags);
    }

    HirNodeId parseFunction(HirFlags flags) {
        std::uint32_t sym = parseSymHandle();
        TypeId sig = parseTypeAnnot();
        auto kids = parseBraceNodes();
        if (kids.empty()) { malformed("function has no body"); return builder_.addLeaf(HirKind::Error, sig, sym, flags); }
        HirNodeId body = kids.back();
        std::vector<HirNodeId> params(kids.begin(), kids.end() - 1);
        return builder_.makeFunction(sig, sym, params, body, flags);
    }
    HirNodeId parseExternFunction(HirFlags flags) {
        std::uint32_t sym = parseSymHandle();
        TypeId sig = accept(Tk::Colon) ? parseType() : InvalidType;
        auto params = parseBraceNodes();
        return builder_.makeExternFunction(sig, sym, params, flags);
    }
    HirNodeId parseExtNode(HirFlags flags) {
        std::string name = takeStr();
        auto it = extKindByName_.find(name);
        std::uint32_t payload = 0;
        if (it != extKindByName_.end()) payload = it->second.v;
        else unknownName(std::format("extension kind \"{}\" not declared", name));
        TypeId t = accept(Tk::Colon) ? parseType() : InvalidType;
        if (peekIs(Tk::LBrace)) { auto kids = parseBraceNodes(); return builder_.addParent(HirKind::Extension, kids, t, payload, flags); }
        return builder_.addLeaf(HirKind::Extension, t, payload, flags);
    }
    HirNodeId parseErrorNode(HirFlags flags) {
        TypeId t = accept(Tk::Colon) ? parseType() : InvalidType;
        if (peekIs(Tk::LBrace)) { auto kids = parseBraceNodes(); return builder_.addParent(HirKind::Error, kids, t, 0, flags); }
        return builder_.addLeaf(HirKind::Error, t, 0, flags);
    }

    // ── types ─────────────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<TypeId> parseTypeListUntil(Tk close) {
        std::vector<TypeId> ts;
        while (!peekIs(close) && !peekIs(Tk::Eof)) {
            ts.push_back(parseType());
            if (!accept(Tk::Comma)) break;
        }
        return ts;
    }
    [[nodiscard]] TypeId parseType() {
        if (!peekIs(Tk::Ident)) { malformed("expected a type"); return InvalidType; }
        std::string kw = lex_.take().text;
        if (kw == "invalid") return InvalidType;
        if (auto p = primFromName(kw)) return interner_.primitive(*p);
        auto wrap1 = [&](TypeId (TypeInterner::*fn)(TypeId)) -> TypeId {
            expect(Tk::LAngle, "'<'"); TypeId e = parseType(); expect(Tk::RAngle, "'>'"); return (interner_.*fn)(e);
        };
        if (kw == "ptr") return wrap1(&TypeInterner::pointer);
        if (kw == "ref") return wrap1(&TypeInterner::reference);
        if (kw == "nullable") return wrap1(&TypeInterner::nullable);
        if (kw == "optional") return wrap1(&TypeInterner::optional);
        if (kw == "slice") return wrap1(&TypeInterner::slice);
        if (kw == "fnptr") { expect(Tk::LAngle, "'<'"); (void)parseType(); expect(Tk::RAngle, "'>'");
            malformed("fnptr<> is not constructible in this interner"); return InvalidType; }
        if (kw == "vec") { expect(Tk::LAngle, "'<'"); TypeId e = parseType(); expect(Tk::Comma, "','");
            std::int64_t n = static_cast<std::int64_t>(takeInt()); expect(Tk::RAngle, "'>'");
            return interner_.vector(e, n); }
        if (kw == "mat") { expect(Tk::LAngle, "'<'"); TypeId e = parseType(); expect(Tk::Comma, "','");
            std::int64_t r = static_cast<std::int64_t>(takeInt()); expect(Tk::Comma, "','");
            std::int64_t c = static_cast<std::int64_t>(takeInt()); expect(Tk::RAngle, "'>'");
            return interner_.matrix(e, r, c); }
        if (kw == "arr") { expect(Tk::LAngle, "'<'"); TypeId e = parseType(); expect(Tk::Comma, "','");
            std::int64_t n = static_cast<std::int64_t>(takeInt()); expect(Tk::RAngle, "'>'");
            return interner_.array(e, n); }
        if (kw == "tuple") { expect(Tk::LAngle, "'<'"); auto ts = parseTypeListUntil(Tk::RAngle); expect(Tk::RAngle, "'>'");
            return interner_.tuple(ts); }
        if (kw == "struct") { std::string name = takeStr(); expect(Tk::LBrace, "'{'");
            auto ts = parseTypeListUntil(Tk::RBrace); expect(Tk::RBrace, "'}'");
            return interner_.structType(name, ts); }
        if (kw == "union") { std::string name = takeStr(); expect(Tk::LBrace, "'{'");
            auto ts = parseTypeListUntil(Tk::RBrace); expect(Tk::RBrace, "'}'");
            return interner_.unionType(name, ts); }
        if (kw == "fn") {
            expect(Tk::LParen, "'('"); auto params = parseTypeListUntil(Tk::RParen); expect(Tk::RParen, "')'");
            expect(Tk::Arrow, "'->'"); TypeId result = parseType();
            CallConv cc = CallConv::CcSysV;
            if (acceptKeyword("cc")) { std::string n = takeIdent(); cc = orMalformed(ccFromName(n), n, "calling convention", CallConv::CcSysV); }
            return interner_.fnSig(params, result, cc);
        }
        if (kw == "ext") {
            std::string name = takeStr();
            expect(Tk::LParen, "'('"); auto args = parseTypeListUntil(Tk::RParen); expect(Tk::RParen, "')'");
            std::vector<std::int64_t> scalars;
            if (accept(Tk::LBrack)) {
                while (!peekIs(Tk::RBrack) && !peekIs(Tk::Eof)) {
                    bool neg = accept(Tk::Minus);
                    std::int64_t v = static_cast<std::int64_t>(takeInt());
                    scalars.push_back(neg ? -v : v);
                    if (!accept(Tk::Comma)) break;
                }
                expect(Tk::RBrack, "']'");
            }
            TypeKindId kindId = typeReg_.registerExtension(name, {});
            return interner_.extension(kindId, name, args, scalars);
        }
        malformed(std::format("unknown type '{}'", kw));
        return InvalidType;
    }
};

} // namespace

std::unique_ptr<HirParseResult> parseHir(std::string_view text, CompilationUnitId cuId,
                                         DiagnosticReporter& reporter) {
    std::size_t const errBefore = reporter.errorCount();
    Parser parser{text, cuId, reporter};
    HirNodeId const root = parser.parse();

    if (!root.valid()) {
        // Header/fatal failure: hand back an empty module (built from the same
        // builder, so finish()'s root-provenance check passes) so the result is
        // still a well-formed object the caller can inspect.
        HirNodeId const empty = parser.builder_.makeModule({});
        auto res = std::make_unique<HirParseResult>(
            std::move(parser.builder_).finish(empty),
            std::move(parser.interner_), std::move(parser.symbolNames_));
        res->ok = false;
        return res;
    }

    Hir hir = std::move(parser.builder_).finish(root);
    auto res = std::make_unique<HirParseResult>(
        std::move(hir), std::move(parser.interner_), std::move(parser.symbolNames_));

    // Apply the collected side-table annotations to the frozen module's maps.
    for (auto& [id, v] : parser.pLoc_)       res->sourceMap.set(id, v);
    for (auto& [id, v] : parser.pFfi_)        res->ffiMap.set(id, std::move(v));
    for (auto& [id, v] : parser.pShader_)     res->shaderMap.set(id, v);
    for (auto& [id, v] : parser.pTranspile_)  res->transpileMap.set(id, std::move(v));
    for (auto& d : parser.pDiag_) {
        DiagnosticInfo info = std::move(d.info);
        if (d.hasOrigin && d.originPre < parser.indexToId_.size()
            && parser.indexToId_[d.originPre].valid()) {
            info.origin = parser.indexToId_[d.originPre];
        }
        res->diagnosticMap.set(d.node, std::move(info));
    }

    // Verify-on-load: the round-trip is only clean if the rebuilt module verifies.
    HirVerifier verifier{res->hir, &res->sourceMap, &res->interner};
    (void)verifier.verify(reporter);

    res->ok = reporter.errorCount() == errBefore;
    return res;
}

} // namespace dss