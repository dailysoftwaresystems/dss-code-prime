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

// (C)'s emitter. A DIFFERENT code from (A)/(B) on purpose, and the split is the
// same one `validateShippedIncludeClosure` already draws: this fault is not about
// a TYPE at all, and telling an author their type identity conflicts when what
// diverged is which DLL a name imports from sends them to the wrong line.
void emitRealizationConflict(DiagnosticReporter& reporter, std::string what) {
    dss::report(reporter, DiagnosticCode::F_ShippedCorpusInvariantBroken,
                DiagnosticSeverity::Error, std::move(what));
}

// Look one format key up in a descriptor-level map with the per-SYMBOL override
// MERGED OVER it — symbol keys win, an omitted format inherits the descriptor's.
// The IDENTICAL merge `realizeRow` and the semantic injector perform, so the
// three cannot answer differently about the same row; getting this wrong would
// make the checker compare something no build ever binds.
//
// ABSENCE IS A VALUE. It is spelled, not skipped: a row that names an image and a
// row that names none for this format is a real divergence (order would decide
// between an import and an unbound reference), so the sentinel has to compare
// unequal to every image name. It is bracketed for that reason — no config value
// can collide with it, and it reads correctly in the diagnostic.
// ⚠ D-FFI-DESCRIPTOR-KNOWN-NAME-HAS-NO-LIBRARY-FOR-FORMAT — ABSENCE HAS EXACTLY
// ONE RENDERING HERE. The header above states that "absence is encoded as its own
// value and compared like any other"; that was true of a MISSING key and false of
// a key naming the empty string, which rendered as an empty image and therefore
// compared UNEQUAL to a row meaning the identical thing. Both now render `<none>`.
// The empty spelling is refused at descriptor load, so this is the interior half
// of a totality whose boundary half is `decodeLibraryMap` — a rule that holds only
// "by construction elsewhere" is the shape this anchor exists to remove.
//
// The merge is performed the SAME way `realizeRow` performs it — symbol keys win,
// an omitted format inherits the descriptor's — rather than by a two-step lookup,
// so the rendering and the realization can never disagree about which entry is in
// force for a row.
[[nodiscard]] std::string mergedEntry(
    std::unordered_map<std::string, std::string> const& docMap,
    std::unordered_map<std::string, std::string> const& symMap,
    std::string_view formatName) {
    std::unordered_map<std::string, std::string> merged = docMap;
    for (auto const& [fmt, value] : symMap) merged.insert_or_assign(fmt, value);
    std::string_view const entry = shippedLibraryImageForFormat(merged, formatName);
    return entry.empty() ? std::string{"<none>"} : std::string{entry};
}

// The same bracketed-absence convention for a plain optional string field.
[[nodiscard]] std::string orNone(std::string const& s) {
    return s.empty() ? std::string{"<none>"} : s;
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

void ShippedTypeConsistency::recordRealization(
    std::string_view origin, ShippedLibDescriptor const& desc,
    ShippedSymbol const& sym, std::string_view formatName,
    DiagnosticReporter& reporter, bool& ok) {
    // EVERY non-type axis of the row, resolved for THIS format, in one canonical
    // string. The list is the struct's own realization-bearing field set, not a
    // sample: each of these is a fact the FIRST-WINS injection takes from the
    // winner and throws away from the loser, so each is a fact two independently
    // authored descriptors can silently disagree about. `availableObjectFormats`
    // is absent by design — it is the GATE that chose these rows, never an axis
    // they must agree on.
    std::string traits = std::format(
        "library={} source={} synthesize={} linkName={} version={} kind={} "
        "linkage={} noreturn={} returnsTwice={}",
        mergedEntry(desc.library, sym.library, formatName),
        mergedEntry(desc.realization, sym.realization, formatName),
        orNone(sym.synthesize), orNone(sym.linkName), orNone(sym.version),
        sym.kind == ShippedSymbolKind::Function ? "function" : "object",
        sym.linkage == ShippedSymbolLinkage::Weak ? "weak" : "external",
        sym.noreturn ? 1 : 0, sym.returnsTwice ? 1 : 0);

    auto const it = realizations_.find(sym.name);
    if (it == realizations_.end()) {
        realizations_.emplace(
            sym.name,
            Realization{sym.signature, std::move(traits), std::string{origin}});
        return;   // the first declaration selected for this target
    }
    ++duplicatesCompared_;   // a co-live duplicate was really compared
    Realization const& first = it->second;
    bool const sameType   = first.signature.v == sym.signature.v;
    bool const sameTraits = first.traits == traits;
    if (sameType && sameTraits) return;   // a byte-identical duplicate — LEGAL

    // A real divergence. Name BOTH descriptors and print BOTH realizations in
    // full: the author's job is to reconcile two rows, so the message has to be
    // the two rows. When the SAME descriptor declares the name twice the origins
    // coincide — say so rather than printing one filename twice as if it were a
    // cross-file conflict, because the fix is a different one (delete a row, not
    // reconcile two files).
    std::string const where =
        first.origin == origin
            ? std::format("'{}' declares '{}' TWICE for this target and the two "
                          "rows disagree", origin, sym.name)
            : std::format("shipped-lib descriptors '{}' and '{}' both declare "
                          "'{}' for this target and disagree",
                          first.origin, origin, sym.name);
    std::string detail;
    if (!sameType) {
        detail += std::format("\n  signature: `{}` in '{}' vs `{}` in '{}'",
                              render(first.signature), first.origin,
                              render(sym.signature), origin);
    }
    if (!sameTraits) {
        detail += std::format("\n  realization: [{}] in '{}'\n               [{}] in '{}'",
                              first.traits, first.origin, traits, origin);
    }
    emitRealizationConflict(
        reporter,
        std::format(
            "{} on object format '{}'.{}\n"
            "Injection is FIRST-WINS BY NAME, so the winner's realization is the "
            "one the whole program gets and the loser's vanishes with no "
            "diagnostic — and the two paths that pick a winner do not even use the "
            "same order: the `#include` path takes the TU's include closure order, "
            "the hand-declared path (C23 7.1.4p2) takes the corpus index's path "
            "order, so one program can bind two different images for one name "
            "depending on how it spelled the declaration. A duplicate is LEGAL and "
            "is relied on (<memory.h> mirrors <string.h>, <tgmath.h> mirrors "
            "<math.h>) — a duplicate that DISAGREES is not. Either make the two "
            "rows identical for this format, or gate them apart with "
            "`availableObjectFormats` so they never co-exist",
            where, formatName, detail));
    ok = false;
}

// ⚠ THE SENTINEL SPELLS CORRECTLY (`objectFormatKindName`): `Unknown` renders as
// "unknown", which is a perfectly good map key that no descriptor ever writes —
// so accepting it would look up `<none>` for every row, make every pair agree
// trivially, and turn (C) VACUOUSLY GREEN on exactly the callers that have no
// target. `isSelectableObjectFormatKind` is the shared rejection this vocabulary
// requires of every consumer; an empty name here means "no realization to
// compare", the same as nullopt.
std::string_view ShippedTypeConsistency::activeFormatName() const noexcept {
    return activeFormat_.has_value() && isSelectableObjectFormatKind(*activeFormat_)
               ? objectFormatKindName(*activeFormat_)
               : std::string_view{};
}

// PER-SYMBOL availability, exactly as the analyzer gates injection: a symbol
// absent on this format declares nothing here (threads.json ships three
// per-format `tss_get` rows whose parameter identity differs BY DESIGN — only one
// of them is ever a declaration on a given target). A RESTRICTED symbol under an
// UNKNOWN format (a direct-API / LSP / unit caller) is skipped too: we cannot know
// whether it is selected, and asserting an invariant over declarations that may
// not apply would be a false alarm.
bool ShippedTypeConsistency::symbolSelectedHere(
    ShippedSymbol const& sym) const noexcept {
    if (sym.availableObjectFormats.empty()) return true;   // every format
    return activeFormat_.has_value()
        && objectFormatInAvailabilitySet(sym.availableObjectFormats,
                                         *activeFormat_);
}

// ── D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED ────────
// The HAND-DECLARED path's slice — see the header for why it is a SLICE and not
// `add`. Everything it does is `add`'s (C) half, verbatim: same gates, same
// `recordRealization`, same message, same code, same vacuity witness.
bool ShippedTypeConsistency::addRealizationsOf(
    std::string_view origin, ShippedLibDescriptor const& desc,
    std::span<std::string const> names, DiagnosticReporter& reporter) {
    bool ok = true;
    std::string_view const formatName = activeFormatName();
    if (formatName.empty() || names.empty()) return ok;   // nothing to state
    // O(1) per row rather than O(names): the REVERSE arm of the archive/asm
    // binder asks about EVERY name in the corpus, so the linear form would be
    // quadratic in the corpus for the one caller that most needs it cheap.
    std::unordered_set<std::string_view> wanted;
    wanted.reserve(names.size());
    for (auto const& n : names) wanted.insert(std::string_view{n});
    for (auto const& sym : desc.symbols) {
        if (wanted.find(std::string_view{sym.name}) == wanted.end()) continue;
        if (!symbolSelectedHere(sym)) continue;
        recordRealization(origin, desc, sym, formatName, reporter, ok);
    }
    return ok;
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
    std::string_view const formatName = activeFormatName();
    for (auto const& sym : desc.symbols) {
        if (!symbolSelectedHere(sym)) continue;
        walk(sym.signature, origin, reporter, ok);
        // (C) REALIZATION AGREEMENT — the same gated row, one axis over. Only
        // when the format is KNOWN: `library`/`realization` are per-format maps,
        // so without a format there is no entry to select and no realization to
        // hold two rows to. (A direct-API / LSP / unit caller with no target is
        // exactly the nullopt case, and it is the same posture
        // `realizeShippedExternSymbols` takes — "no format ⇒ no realization".)
        if (!formatName.empty())
            recordRealization(origin, desc, sym, formatName, reporter, ok);
    }
    for (auto const& c   : desc.constants)      walk(c.type,        origin, reporter, ok);
    for (auto const& c   : desc.floatConstants) walk(c.type,        origin, reporter, ok);
    return ok;
}

} // namespace dss::ffi
