#pragma once

#include <cstddef>
#include <string_view>

// The synthetic name an ANONYMOUS aggregate member is bound under.
//
// An anonymous field (`int :3;`, a nested `union { ... };`) binds no user name,
// so the semantic analyzer mints one anchored on the DECLARATION NODE:
// `<anon:RULE:NODEID>` (`SemanticAnalyzer`'s anonymous-declarator arm, c10
// D-CSUBSET-STRUCT-MEMBER-DECLARATOR). `RULE` is the grammar rule that produced
// it — `structSpec`, `unionSpec` — and `NODEID` is the enclosing tree's node
// index, which makes the name UNIQUE WITHIN ONE CU and MEANINGLESS ACROSS TWO.
//
// ⚠⚠ THAT DISTINCTION IS NOT COSMETIC, AND IT COST THIS PROJECT A MEASURED
// DEFECT (D-MIR-MERGE-COMPOSITE-HOST-IDENTITY-IS-THE-DECLARATION-SITE). The
// name reaches the interned TYPE, so a whole-program merge comparing composites
// by name found two copies of one anonymous `union` unequal — and every named
// struct that REACHES one inherited the split. ✔MEASURED on the 103-TU sqlite
// corpus: **98 tags forked, every one of them with a SINGLE local layout
// signature** — `Parse`, `Table`, `Select`, `Index`, `KeyInfo` and 93 others,
// each forked once per CU that declared it, purely because somewhere below them
// sat an anonymous member carrying another CU's node number.
//
// ★ THE SPELLING HAS THREE READERS AND USED TO HAVE THREE COPIES OF THE PREFIX
// TEST. It has one owner now. A predicate written inline at its third call site
// is how the first two go stale without anything failing.
namespace dss {

inline constexpr std::string_view kAnonMemberNamePrefix = "<anon:";

// True for a name the analyzer minted for an anonymous member.
[[nodiscard]] inline bool isSyntheticAnonymousName(std::string_view name) {
    return name.substr(0, kAnonMemberNamePrefix.size()) == kAnonMemberNamePrefix;
}

// The DECL-SITE-INDEPENDENT part of such a name: `<anon:RULE:` with the node id
// and the closing `>` removed, e.g. `<anon:unionSpec:65291>` → `<anon:unionSpec`.
//
// ★ THE RULE IS KEPT AND THE NODE ID IS DROPPED, and both halves of that are
// deliberate. The rule says WHAT the member is — a `structSpec` and a
// `unionSpec` overlay their fields differently, so collapsing them would merge
// two genuinely different layouts. The node id says only WHERE it was written,
// which is exactly the fact that must not survive a CU boundary.
// ⓘ Returns the whole name unchanged when it is not a synthetic anonymous name,
// so a caller may pass any name and get an identity-safe form back.
[[nodiscard]] inline std::string_view anonNameWithoutDeclSite(
        std::string_view name) {
    if (!isSyntheticAnonymousName(name)) return name;
    std::size_t const colon = name.find(':', kAnonMemberNamePrefix.size());
    return colon == std::string_view::npos ? name : name.substr(0, colon);
}

} // namespace dss
