#pragma once

#include "analysis/semantic/semantic_model.hpp"   // ScopeRecord, SymbolRecord, SymbolNamespace
#include "core/types/aggregate_layout.hpp"        // AggregateLayoutParams (`memberByteOffset`)
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_layout.hpp"   // computeLayout (`memberByteOffset`)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// ── A MEMBER REACHED THROUGH ANONYMOUS STRUCTURE / UNION MEMBERS ────────────────
//
// C 6.7.2.1p13 (C23 6.7.3.2p15): "The members of an anonymous structure or union are
// considered to be members of the containing structure or union", recursively. A NAME
// that is not a direct member of a composite may therefore name a member of one of its
// anonymous members, however deeply nested — for a member access (`t.a`) and, P68
// round 9 (lane `cs`), for a brace list's DESIGNATOR (`{ .a = 40 }`), which gcc 13.3.0,
// clang 18.1.3, mingw-w64 13.2.0 and MSVC 19.51 all build (✔MEASURED 2026-09-23, the
// lane's `.temp/probe/r9f`, every build RUN) and DSS refused ("field designator names a
// field that doesn't belong to the target struct type").
//
// ★ ONE SEARCH FOR BOTH TIERS. The semantic tier asks it for a member access
// (`findPromotedField`) and to size an array of unknown size from a designated list;
// the HIR tier asks it to place a designated element. Each tier supplies the three
// facts it holds — a scope's record, a symbol's record, and a composite type's member
// scope — through `Access`:
//
//     ScopeRecord const*  scope(ScopeId) const;
//     SymbolRecord const* record(SymbolId) const;
//     ScopeId             compositeScope(TypeId) const;   // invalid when unknown
//
// The result carries the anonymous members' field indices outermost first, so a
// designator's index path is `anonIndices` followed by the member's own `fieldIndex`.
// Two DIFFERENT members of one spelling reachable through sibling anonymous members
// (`struct { union { int x; }; union { int x; }; }`) are AMBIGUOUS — C forbids the
// declaration, and no member is returned.
//
// ★ NOT RECURSIVE: a worklist of (scope, index path) — the nesting depth is the
// user's source (feedback-no-input-proportional-recursion).
namespace dss::anon_member_search {

struct PromotedMember {
    SymbolId                   symbol{};
    std::vector<std::uint32_t> anonIndices;   // the anonymous members crossed, outermost first
    bool                       ambiguous = false;
};

template <class Access>
[[nodiscard]] std::optional<PromotedMember>
findPromotedMember(ScopeId fieldScope, std::string_view name, Access const& access) {
    std::optional<PromotedMember> found;
    struct Item {
        ScopeId                    scope;
        std::vector<std::uint32_t> path;
    };
    std::vector<Item> worklist{Item{fieldScope, {}}};
    for (std::size_t wi = 0; wi < worklist.size(); ++wi) {
        ScopeId const cur = worklist[wi].scope;
        ScopeRecord const* rec = access.scope(cur);
        if (rec == nullptr) continue;
        for (auto const& [bindName, fsym] : rec->bindings) {
            (void)bindName;
            if (!fsym.valid()) continue;
            SymbolRecord const* anon = access.record(fsym);
            if (anon == nullptr || !anon->isAnonymousMember) continue;
            ScopeId const anonScope = access.compositeScope(anon->type);
            if (!anonScope.valid()) continue;
            ScopeRecord const* anonRec = access.scope(anonScope);
            if (anonRec == nullptr) continue;
            std::vector<std::uint32_t> path = worklist[wi].path;
            path.push_back(anon->fieldIndex);
            if (auto const hit = anonRec->bindings.find(name); hit != anonRec->bindings.end()) {
                SymbolRecord const* m = access.record(hit->second);
                if (m != nullptr && !m->isAnonymousMember) {
                    if (found.has_value() && found->symbol.v != hit->second.v)
                        return PromotedMember{{}, {}, /*ambiguous=*/true};
                    found = PromotedMember{hit->second, path, false};
                }
            }
            worklist.push_back(Item{anonScope, std::move(path)});
        }
    }
    return found;
}

// ── A NAMED MEMBER'S BYTE OFFSET — ONE ANSWER FOR EVERY CONSTANT, IN BOTH TIERS ────
//
// "Where in this struct / union does the member called `name` live, and what is its
// type?" is the question a CONSTANT asks: each step of `offsetof(T, a.b)`, the
// `&((T *)0)->name` spelling of the same thing, and the member step of an object-size
// operand (`__builtin_object_size(&s.name, 1)`) in the semantic tier; the `&((T *)0)->name`
// spelling again when the HIR lowering folds a condition or an index designator. This
// function is the answer for ALL of them, and it finds the member the way every member
// access and every brace designator does:
//
//   * in the composite's OWN member scope, NEVER in a scope that encloses it. A scope
//     walk that climbs to the parents answers for a name that is not a member at all —
//     a file-scope object, an enumerator, a typedef name, a member of the ENCLOSING
//     struct — and then reads that symbol's `fieldIndex` as if it were a member's;
//   * else through the composite's ANONYMOUS members (`findPromotedMember`): their
//     members are the containing composite's (C23 6.7.3.2p15), and the offset is the
//     SUM along the anonymous members crossed;
//   * with the composite keyed by its MATERIAL type (`stripVolatile` drops the whole
//     qualifier skin), because that is the key its member scope was registered under;
//   * a symbol that is not a member (`DeclarationKind::Variable`) is no member;
//   * a BIT-FIELD has no byte offset, and is said to be one.
//
// THE ANSWER SAYS WHY when there is none (`MemberOffsetStatus`): `offsetof` refuses
// with a sentence per reason, and a caller that only folds reads `found()`.
//
// P69 (lane `cs`), D-C-A-CONSTANTS-MEMBER-NAME-IS-RESOLVED-BY-A-SCOPE-WALK: until this
// function the three semantic-tier askers each wrote the lookup out, each with
// `ScopeTree::lookup` — which CLIMBS — on a member scope keyed by the QUALIFIED type,
// and none asked the promoted search. ✔MEASURED 2026-10-08 (the lane's probe `mo1`;
// gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0, cl 14.51): `int g; struct S { int a;
// int b; }; int probe[offsetof(struct S, g) + 1];` COMPILED (offset 0) where all four
// refuse; `struct A { char a; struct { int b; int c; }; };` with `offsetof(struct A, c)`
// was REFUSED where all four run; so was `offsetof(volatile struct S, b)`, as
// "incomplete". The second copy in another tier is what this function was written to
// prevent: the lowering's fold must agree with the semantic tier's to the byte, and two
// lookups are two answers.
//
// ★ NOT RECURSIVE: the anonymous members crossed are an index path, walked in a loop.
enum class MemberOffsetStatus : std::uint8_t {
    Found,
    NotAComposite,   // the type the name is looked up in is not a struct or union
    Incomplete,      // the composite has no member scope here (it is incomplete at this point)
    NotAMember,      // no member of that name — its own, or one of its anonymous members'
    Ambiguous,       // two DIFFERENT members of that name through sibling anonymous members
    BitField,        // the member is a bit-field: it has no byte offset
    NoLayout,        // the composite, or an anonymous member on the way, has no computable layout
};

struct MemberOffset {
    MemberOffsetStatus status = MemberOffsetStatus::NotAMember;
    std::uint64_t      offset = 0;   // bytes from the start of the composite the name was looked up in
    TypeId             type{};       // the member's declared type
    [[nodiscard]] bool found() const noexcept { return status == MemberOffsetStatus::Found; }
};

template <class Access>
[[nodiscard]] MemberOffset
memberByteOffset(TypeInterner const& interner, TypeId container, std::string_view name,
                 Access const& access, AggregateLayoutParams const& layout, DataModel dataModel) {
    auto const miss = [](MemberOffsetStatus why) { return MemberOffset{why, 0, TypeId{}}; };
    if (!container.valid()) return miss(MemberOffsetStatus::NotAComposite);
    TypeId cur = interner.stripVolatile(container);
    TypeKind const kind = interner.kind(cur);
    if (kind != TypeKind::Struct && kind != TypeKind::Union)
        return miss(MemberOffsetStatus::NotAComposite);
    ScopeId const memberScope = access.compositeScope(cur);
    ScopeRecord const* const own = memberScope.valid() ? access.scope(memberScope) : nullptr;
    if (own == nullptr) return miss(MemberOffsetStatus::Incomplete);

    SymbolRecord const* member = nullptr;
    std::vector<std::uint32_t> anonIndices;   // the anonymous members crossed, outermost first
    if (auto const hit = own->bindings.find(name); hit != own->bindings.end()) {
        member = access.record(hit->second);
    } else if (auto promoted = findPromotedMember(memberScope, name, access);
               promoted.has_value()) {
        if (promoted->ambiguous) return miss(MemberOffsetStatus::Ambiguous);
        member      = access.record(promoted->symbol);
        anonIndices = std::move(promoted->anonIndices);
    }
    if (member == nullptr || member->kind != DeclarationKind::Variable)
        return miss(MemberOffsetStatus::NotAMember);

    // WHAT the member is, before WHERE it is: the composites on the way are read from
    // the types alone, so a bit-field is named as one even where no layout can be
    // computed (a bit-field's container has none under a layout-less analysis) — the
    // order the member step had before it was this function, kept.
    std::vector<TypeId> containers{cur};   // outermost first; back() declares the member
    for (std::uint32_t const anon : anonIndices) {
        auto const fields = interner.operands(containers.back());
        if (anon >= fields.size()) return miss(MemberOffsetStatus::NoLayout);
        containers.push_back(interner.stripVolatile(fields[anon]));
    }
    std::uint32_t const index = member->fieldIndex;
    if (interner.fieldBitWidth(containers.back(), index).has_value())
        return miss(MemberOffsetStatus::BitField);

    std::uint64_t offset = 0;
    for (std::size_t step = 0; step < containers.size(); ++step) {
        std::uint32_t const field = step < anonIndices.size() ? anonIndices[step] : index;
        auto const at = computeLayout(containers[step], interner, layout, dataModel);
        if (!at.has_value() || field >= at->fieldOffsets.size())
            return miss(MemberOffsetStatus::NoLayout);
        offset += at->fieldOffsets[field];
    }
    return MemberOffset{MemberOffsetStatus::Found, offset, member->type};
}

}  // namespace dss::anon_member_search
