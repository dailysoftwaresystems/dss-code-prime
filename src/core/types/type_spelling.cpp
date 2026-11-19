#include "core/types/type_spelling.hpp"

#include "core/types/anon_member_name.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

namespace {

// The word for a base type known by its kind and its vocabulary tag: the tag the
// type carries (`long`, `unsigned long`, `__int128` …), else the table's word for
// its kind; nullopt when the table has none. The load refuses a kind the
// document's OWN rows reach unspelled, so a kind without a word is one reached
// from outside them (a descriptor's `f16` in a language with no half-precision
// type): the language cannot name it, and the sentence says so by its form
// without a type. The ONE home of the kind's word, for `spellType` and
// `spellCoreKind` alike.
[[nodiscard]] std::optional<std::string> kindWord(TypeKind k, std::string_view vocabularyName,
                                                  TypeSpelling const& table) {
    if (!vocabularyName.empty()) return std::string{vocabularyName};
    std::string const& s = table.kind(k);
    if (s.empty()) return std::nullopt;
    return s;
}

[[nodiscard]] std::string fill(std::string_view pattern, std::string_view value) {
    std::string out{pattern};
    std::size_t const at = out.find("{}");
    if (at != std::string::npos) out.replace(at, 2, value);
    return out;
}

// The words of a qualifier skin's bits, in the table's order: volatile, then _Atomic.
[[nodiscard]] std::string qualifierWords(TypeInterner const& in, TypeId t, TypeSpelling const& table) {
    std::string out;
    auto add = [&](std::string const& w) {
        if (!out.empty()) out += ' ';
        out += w;
    };
    if (in.isVolatileQualified(t)) add(table.volatileWord);
    if (in.isAtomicQualified(t)) add(table.atomicWord);
    return out;
}

// A base type (no declarator form): its specifiers; nullopt when the table has no
// word for a piece of it.
[[nodiscard]] std::optional<std::string> baseSpelling(TypeInterner const& in, TypeId t,
                                                      TypeSpelling const& table) {
    TypeKind const k = in.kind(t);
    switch (k) {
    case TypeKind::Struct:
    case TypeKind::Union:
    case TypeKind::Enum: {
        std::string const& tag = k == TypeKind::Struct ? table.structTag
                               : k == TypeKind::Union  ? table.unionTag
                                                       : table.enumTag;
        if (tag.empty()) return std::nullopt;
        std::string_view const name = in.name(in.stripVolatile(t));
        bool const anonymous = name.empty() || isSyntheticAnonymousName(name);
        return tag + ' ' + (anonymous ? table.anonymousTag : std::string{name});
    }
    case TypeKind::BitInt: {
        std::string const& pattern = in.bitIntIsSigned(t) ? table.bitIntSigned : table.bitIntUnsigned;
        if (pattern.empty()) return std::nullopt;
        return fill(pattern, std::to_string(in.bitIntWidth(t)));
    }
    case TypeKind::Complex: {
        if (table.complex.empty()) return std::nullopt;
        std::optional<std::string> const element = baseSpelling(in, in.complexElement(t), table);
        if (!element) return std::nullopt;
        return fill(table.complex, *element);
    }
    default:
        break;
    }
    return kindWord(k, in.vocabularyName(t), table);
}

// Prefix the qualifier words of `t`'s skin (if any) to `text`.
[[nodiscard]] std::string qualified(TypeInterner const& in, TypeId t, TypeSpelling const& table,
                                    std::string text) {
    std::string const q = qualifierWords(in, t, table);
    return q.empty() ? text : q + ' ' + text;
}

// One function form whose parameters are being spelled: its own prefix of the
// declarator, the types still to spell, and the text spelled so far.
struct FunctionFrame {
    TypeId      result;
    std::string declarator;       // the inner text the function's suffix follows
    std::vector<TypeId> params;
    std::size_t next = 0;
    std::string spelled;
    bool        variadic = false;
    bool        withoutPrototype = false;
};

} // namespace

std::optional<std::string> spellType(TypeInterner const& in, TypeId t, TypeSpelling const& table,
                                     SpellOptions options) {
    if (!table.declared) return std::nullopt;
    // C's declarators read inside out: walk from the outer type to the base,
    // growing the inner text, with an explicit stack for the parameter types of
    // every function form met (no recursion proportional to the type's nesting).
    std::vector<FunctionFrame> frames;
    TypeId cur = t;
    std::string declarator;
    bool innerIsPrefix = false;   // the inner text ends in a prefix form (a pointer)
    bool outermostFunction = true;
    for (;;) {
        TypeKind const k = in.kind(cur);
        if (k == TypeKind::Ptr) {
            std::string const q = qualifierWords(in, cur, table);
            std::string piece = table.pointer;
            if (!q.empty()) piece += q + (declarator.empty() ? "" : " ");
            declarator = piece + declarator;
            innerIsPrefix = true;
            cur = in.operands(cur)[0];
            continue;
        }
        if (k == TypeKind::Array || k == TypeKind::FnSig) {
            if (innerIsPrefix) declarator = table.groupOpen + declarator + table.groupClose;
            innerIsPrefix = false;
        }
        if (k == TypeKind::Array) {
            std::string bound;
            if (!in.isIncompleteArray(cur) && !in.isVlaArray(cur)) bound = std::to_string(in.scalars(cur)[0]);
            else if (in.isVlaArray(cur)) bound = "*";
            declarator += table.arrayOpen + bound + table.arrayClose;
            cur = in.operands(cur)[0];
            continue;
        }
        if (k == TypeKind::FnSig) {
            FunctionFrame f;
            f.result = in.fnResult(cur);
            f.declarator = declarator;
            for (TypeId p : in.fnParams(cur)) f.params.push_back(p);
            f.variadic = in.fnIsVariadic(cur);
            f.withoutPrototype = outermostFunction && options.declaredWithoutPrototype;
            outermostFunction = false;
            frames.push_back(std::move(f));
            if (!frames.back().params.empty()) {
                cur = frames.back().params[0];
                declarator.clear();
                innerIsPrefix = false;
                continue;
            }
            // no parameter to spell: fall through to close the frame below
        } else {
            // a base type: its specifiers, then the declarator built around it
            std::optional<std::string> const specifiers = baseSpelling(in, cur, table);
            if (!specifiers) return std::nullopt;
            std::string base = qualified(in, cur, table, *specifiers);
            std::string text = base;
            if (!declarator.empty()) {
                bool const joins = table.suffixJoinsSpecifiers
                                && declarator.compare(0, table.arrayOpen.size(), table.arrayOpen) == 0;
                text += (joins ? std::string{} : table.declaratorSeparator) + declarator;
            }
            if (frames.empty()) return text;
            FunctionFrame& f = frames.back();
            if (f.next > 0) f.spelled += table.parameterSeparator;
            f.spelled += text;
            ++f.next;
            if (f.next < f.params.size()) {
                cur = f.params[f.next];
                declarator.clear();
                innerIsPrefix = false;
                continue;
            }
        }
        // Close the innermost function frame whose parameters are all spelled,
        // and continue with its result around the completed suffix.
        FunctionFrame f = std::move(frames.back());
        frames.pop_back();
        std::string list = f.spelled;
        if (f.params.empty() && !f.variadic && !f.withoutPrototype) list = table.emptyPrototype;
        if (f.variadic) list += (f.params.empty() ? std::string{} : table.parameterSeparator) + table.variadic;
        declarator = f.declarator + table.functionOpen + list + table.functionClose;
        innerIsPrefix = false;
        cur = f.result;
    }
}

std::optional<std::string> spellCoreKind(TypeKind k, std::string_view vocabularyName, TypeSpelling const& table) {
    if (!table.declared) return std::nullopt;
    return kindWord(k, vocabularyName, table);
}

} // namespace dss
