#include "lir/lir_text.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "lir/lir_literal_pool.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace dss {

namespace {

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
        lir_pass_util::report(
            reporter, DiagnosticCode::L_PhysRegOrdinalOutOfRange,
            DiagnosticSeverity::Warning,
            std::format("emitLir: phys reg ordinal {} not in register table",
                        static_cast<std::uint32_t>(r.id)));
        return std::format("phys#{}", static_cast<std::uint32_t>(r.id));
    }
    return info->name;
}

// Render any register (physical or virtual). Virtual: `%v.<id>`.
[[nodiscard]] std::string
renderReg(LirReg r, TargetSchema const& schema, DiagnosticReporter& reporter) {
    if (!r.valid()) return "<invalid>";
    if (r.isPhysical != 0) return renderPhysReg(r, schema, reporter);
    return std::format("%v.{}", static_cast<std::uint32_t>(r.id));
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
//   * `BlockRef`      — `^b<slot>`
//   * `SymbolRef`     — `@<id>`
//   * `MemBase`       — `*<scale>`
//   * `MemOffset`     — `+<offset>` or `-<offset>` for signed
//   * `LiteralIndex`  — `lit#<index>`
//   * `None`          — `_`
// The (reserved) enum slot 3 (formerly `ImmFloat`) is unreachable; the
// switch is `[[nodiscard]]`-exhaustive over the live variants.
[[nodiscard]] std::string
renderOperand(LirOperand const& op, TargetSchema const& schema,
              DiagnosticReporter& reporter) {
    switch (op.kind) {
        case LirOperandKind::None:
            return "_";
        case LirOperandKind::Reg:
            return renderReg(op.reg, schema, reporter);
        case LirOperandKind::ImmInt:
            return std::format("#{}", op.immInt32);
        case LirOperandKind::BlockRef:
            return std::format("^b{}", op.blockSlot);
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
    }
    // Fall-through is a substrate-corruption signal — the discriminator
    // landed on the reserved slot 3 (formerly ImmFloat) or on an out-of-
    // band byte. Loud so the caller's reporter audit catches it.
    lir_pass_util::report(
        reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
        DiagnosticSeverity::Warning,
        std::format("emitLir: operand kind tag {} is unknown (substrate corruption)",
                    static_cast<std::uint32_t>(op.kind)));
    return "<?>";
}

void emitPreamble(std::string& out, TargetSchema const& schema) {
    // `dsslir 1` pins the FORMAT version (text grammar). `target <name>`
    // pins the TARGET schema (which defines the opcode/register name
    // space). LirModuleId is process-local and deliberately not part of
    // the canonical text contract — two `emitLir()` outputs of the same
    // logical module must compare equal regardless of allocation order.
    out.append("dsslir 1\n");
    out.append(std::format("target {}\n", schema.name()));
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
        if (v < ctx.symbolNames.size() && !ctx.symbolNames[v].empty()) {
            out.append(std::format("  %{} \"{}\"\n", v, ctx.symbolNames[v]));
        } else {
            out.append(std::format("  %{} <synthetic>\n", v));
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
              TargetSchema const& schema, DiagnosticReporter& reporter) {
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
        out.append(renderOperand(operands[i], schema, reporter));
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
        emitInst(out, lir, lir.blockInstAt(b, i), schema, reporter);
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

} // namespace dss
