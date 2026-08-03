#include "ffi/shipped_type_consistency.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/shipped_lib_descriptor.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace dss::ffi {

namespace {

// The hir-text spelling of a core kind — deliberately the SAME vocabulary
// `parseTypeFromText` accepts, so a diagnostic quoting a rendered type quotes
// something the author can paste straight back into the descriptor. A kind with
// no hir-text spelling renders as `<kind#N>` (diagnostic-only; never parsed
// back), so this needs no lockstep maintenance with the emitter.
[[nodiscard]] std::string_view coreSpelling(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool: return "bool";
        case TypeKind::I8:   return "i8";   case TypeKind::I16:  return "i16";
        case TypeKind::I32:  return "i32";  case TypeKind::I64:  return "i64";
        case TypeKind::I128: return "i128";
        case TypeKind::U8:   return "u8";   case TypeKind::U16:  return "u16";
        case TypeKind::U32:  return "u32";  case TypeKind::U64:  return "u64";
        case TypeKind::U128: return "u128";
        case TypeKind::F16:  return "f16";  case TypeKind::F32:  return "f32";
        case TypeKind::F64:  return "f64";  case TypeKind::F80:  return "f80";
        case TypeKind::F128: return "f128";
        case TypeKind::Char: return "char"; case TypeKind::Byte: return "byte";
        case TypeKind::Void: return "void";
        default: return {};
    }
}

// A composite kind whose `name` is a nominal TAG (the tag namespace), as opposed
// to a primitive whose `name` is a vocabulary identity tag. The two share the
// interner's one `name` slot, so the walk must tell them apart before deciding
// which invariant applies.
[[nodiscard]] bool isTagKind(TypeKind k) noexcept {
    return k == TypeKind::Struct || k == TypeKind::Union || k == TypeKind::Enum;
}

void emitConflict(DiagnosticReporter& reporter, std::string what) {
    dss::report(reporter, DiagnosticCode::F_ShippedTypeIdentityConflict,
                DiagnosticSeverity::Error, std::move(what));
}

} // namespace

std::string ShippedTypeConsistency::render(TypeId t, int depth) const {
    if (!t.valid()) return "<invalid>";
    // A self-referential type (a struct holding a pointer back to itself) has no
    // finite rendering; the cap keeps the DIAGNOSTIC finite. `walk`'s termination
    // is separate (the `visited_` memo).
    if (depth > 6) return "…";
    TypeKind const k = in_->kind(t);
    auto const kids = in_->operands(t);
    auto list = [&](std::string_view open, std::string_view close) {
        std::string s{open};
        for (std::size_t i = 0; i < kids.size(); ++i) {
            if (i != 0) s += ", ";
            s += render(kids[i], depth + 1);
        }
        s += close;
        return s;
    };
    switch (k) {
        case TypeKind::Ptr:   return "ptr<"   + render(kids.empty() ? InvalidType : kids[0], depth + 1) + ">";
        case TypeKind::Ref:   return "ref<"   + render(kids.empty() ? InvalidType : kids[0], depth + 1) + ">";
        case TypeKind::Array: {
            auto const sc = in_->scalars(t);
            return "arr<" + render(kids.empty() ? InvalidType : kids[0], depth + 1)
                 + ", " + std::to_string(sc.empty() ? 0 : sc[0]) + ">";
        }
        case TypeKind::Struct: return "struct \"" + std::string{in_->name(t)} + "\" "
                                    + list("{", "}");
        case TypeKind::Union:  return "union \"" + std::string{in_->name(t)} + "\" "
                                    + list("{", "}");
        case TypeKind::Enum:   return "enum \"" + std::string{in_->name(t)} + "\"";
        case TypeKind::FnSig: {
            std::string s = "fn(";
            auto const ps = in_->fnParams(t);
            for (std::size_t i = 0; i < ps.size(); ++i) {
                if (i != 0) s += ", ";
                s += render(ps[i], depth + 1);
            }
            s += ") -> " + render(in_->fnResult(t), depth + 1);
            return s;
        }
        default: break;
    }
    std::string_view const core = coreSpelling(k);
    std::string s = core.empty()
        ? std::format("<kind#{}>", static_cast<unsigned>(k))
        : std::string{core};
    if (!isTagKind(k)) {
        std::string_view const vocab = in_->vocabularyName(t);
        if (!vocab.empty()) { s += " \""; s += vocab; s += '"'; }
    }
    return s;
}

void ShippedTypeConsistency::recordNamed(
    std::unordered_map<std::string, Decl>& into, char const* what,
    std::string name, TypeId t, std::string_view origin,
    DiagnosticReporter& reporter, bool& ok) {
    auto const [it, inserted] =
        into.try_emplace(std::move(name), Decl{t, std::string{origin}});
    if (inserted || it->second.type.v == t.v) return;   // absent, or IDENTICAL

    // A real divergence. Name BOTH descriptors, and — when both declarations are
    // composites of the same shape — the FIRST differing member, which is the
    // single most actionable fact (`tv_usec` is `i64` here, `i64 "long"` there).
    std::string detail;
    auto const mine  = in_->operands(t);
    auto const other = in_->operands(it->second.type);
    if (in_->kind(t) == in_->kind(it->second.type) && mine.size() == other.size()) {
        for (std::size_t i = 0; i < mine.size(); ++i) {
            if (mine[i].v == other[i].v) continue;
            detail = std::format(" — first difference at member {}: `{}` here vs "
                                 "`{}` there", i, render(mine[i]), render(other[i]));
            break;
        }
    }
    emitConflict(
        reporter,
        std::format(
            "shipped-lib descriptor '{}' declares {} '{}' as `{}`, but '{}' "
            "already declared it as `{}` for this target{}. Every declaration of "
            "a {} selected for the SAME target must be BYTE-IDENTICAL: injection "
            "is FIRST-WINS BY NAME, so the loser interns a SECOND type whose "
            "members have no field scope (an include-order-dependent 'member "
            "access requires a composite-typed operand')",
            origin, what, it->first, render(t), it->second.origin,
            render(it->second.type), detail, what));
    ok = false;
}

void ShippedTypeConsistency::walk(TypeId t, std::string_view origin,
                                  DiagnosticReporter& reporter, bool& ok) {
    if (!t.valid()) return;
    if (!visited_.insert(t.v).second) return;
    TypeKind const k = in_->kind(t);
    if (isTagKind(k)) {
        std::string_view const tag = in_->name(t);
        if (!tag.empty()) {
            recordNamed(tags_, "struct/union tag", std::string{tag}, t, origin,
                        reporter, ok);
        }
    } else {
        // (B) A vocabulary tag must denote the width the ACTIVE LANGUAGE gives
        // that NAME under the ACTIVE data model. A tag the language does not
        // declare is opaque and skipped — the descriptor may legitimately model
        // a type this source language has no word for.
        std::string_view const vocab = in_->vocabularyName(t);
        if (!vocab.empty()) {
            auto const row = std::find_if(
                vocabulary_.begin(), vocabulary_.end(),
                [&](VocabularyCore const& v) { return v.name == vocab; });
            if (row != vocabulary_.end() && row->core != k) {
                emitConflict(
                    reporter,
                    std::format(
                        "shipped-lib descriptor '{}' spells `{}`, but the active "
                        "language's vocabulary entry '{}' is `{}` under this "
                        "target's data model — that (representation, identity) "
                        "pair is UNPRODUCIBLE here, so the type matches no "
                        "`_Generic` association and no pointer of that spelling. "
                        "A flat vocabulary tag is only valid on a descriptor "
                        "whose formats share ONE data model; otherwise give the "
                        "entry per-format/per-dataModel `variants`",
                        origin, render(t), vocab,
                        coreSpelling(row->core).empty()
                            ? std::string{"<unspellable>"}
                            : std::string{coreSpelling(row->core)}));
                ok = false;
            }
        }
    }
    for (TypeId child : in_->operands(t)) walk(child, origin, reporter, ok);
}

bool ShippedTypeConsistency::add(std::string_view            origin,
                                 ShippedLibDescriptor const& desc,
                                 DiagnosticReporter&         reporter) {
    bool ok = true;
    // TYPEDEF names first — a typedef and a struct tag live in DIFFERENT
    // namespaces (C 6.2.3), hence the two maps.
    for (auto const& td : desc.typedefs) {
        recordNamed(typedefs_, "typedef", td.name, td.type, origin, reporter, ok);
        walk(td.type, origin, reporter, ok);
    }
    // `structs` entries: `st.typeId`'s own tag is recorded by the walk, so the
    // `structs` and INLINE `struct "N" {…}` forms go through ONE code path — the
    // whole point, since the inline form inside `rusage.ru_utime` is exactly the
    // declaration the previous rounds missed.
    for (auto const& st : desc.structs) {
        walk(st.typeId, origin, reporter, ok);
        for (auto const& f : st.fields) walk(f.type, origin, reporter, ok);
    }
    // `unions` entries: the named-member sibling of `structs` — walk the interned
    // union tag + each member type through the SAME per-target byte-identity check
    // (a union `key.objPtr : ptr<Tcl_Obj>` must resolve Tcl_Obj identically to the
    // typedef, or first-wins injection would strand a divergent, field-scope-less type).
    for (auto const& un : desc.unions) {
        walk(un.typeId, origin, reporter, ok);
        for (auto const& f : un.fields) walk(f.type, origin, reporter, ok);
    }
    // PER-SYMBOL availability, exactly as the analyzer gates injection: a symbol
    // absent on this format declares nothing here (threads.json ships three
    // per-format `tss_get` rows whose parameter identity differs BY DESIGN —
    // only one of them is ever a declaration on a given target). A RESTRICTED
    // symbol under an UNKNOWN format (a direct-API / LSP / unit caller) is
    // skipped too: we cannot know whether it is selected, and asserting an
    // invariant over declarations that may not apply would be a false alarm.
    for (auto const& sym : desc.symbols) {
        if (!sym.availableObjectFormats.empty()
            && (!activeFormat_.has_value()
                || !objectFormatInAvailabilitySet(sym.availableObjectFormats,
                                                  *activeFormat_))) continue;
        walk(sym.signature, origin, reporter, ok);
    }
    for (auto const& c   : desc.constants)      walk(c.type,        origin, reporter, ok);
    for (auto const& c   : desc.floatConstants) walk(c.type,        origin, reporter, ok);
    return ok;
}

} // namespace dss::ffi
