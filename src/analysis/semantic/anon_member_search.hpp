#pragma once

#include "analysis/semantic/semantic_model.hpp"   // ScopeRecord, SymbolRecord, SymbolNamespace
#include "core/types/strong_ids.hpp"

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

}  // namespace dss::anon_member_search
