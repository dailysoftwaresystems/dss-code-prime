#include "core/types/type_lattice/type_reintern.hpp"

#include "core/types/anon_member_name.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

namespace {

// ── The mixer ───────────────────────────────────────────────────────────────
// FNV-1a. Every value that reaches a composite's identity goes through one of
// these two, so the encoding has ONE owner and a new channel cannot be added in
// a second spelling.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;

void mix(std::uint64_t& h, std::string_view s) {
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= kFnvPrime;
    }
    h ^= 0xFFu;   // field separator: `("ab","c")` must not digest as `("a","bc")`
    h *= kFnvPrime;
}

void mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= kFnvPrime;
    }
}

// The (kind, tag) key of the ambiguity table. Length-prefixed so that it is
// injective: a separator-joined encoding would fold `("S", "x")` into `("S:x",
// "")`, the same non-injectivity `ffiImportKey` in mir_merge.cpp guards against.
[[nodiscard]] std::string tagKey(TypeKind kind, std::string_view name) {
    return std::format("{}:{}:{}", static_cast<std::uint32_t>(kind),
                       name.size(), name);
}

// ── The abort-message name — ONE OWNER, in the header ───────────────────────
//
// D-TYPEKIND-PASCALCASE-SPELLINGS-HAVE-TWO-OWNERS. This was a forty-one-arm
// exhaustive switch retyping the PascalCase `TypeKind` spellings, and
// `lir/lir_text.cpp` held a second one. The spellings now live in
// `kTypeKindNameTable` beside the enum; this file owns none of them.
//
// ★ THE FORTY-FIRST ARM WAS THE DRIFT. This copy answered `Count_` with
// `"Count_"` while the other answered `"?"` — ✔MEASURED as the one row the two
// owners disagreed on. `Count_` is now unlisted in the table (it is the
// cardinality sentinel, not a type), so it has NO spelling, and this site says
// so with the ORDINAL rather than with a name: `<unnamed kind #40>` is strictly
// more informative than either old answer, it covers an out-of-range ordinal
// with the same sentence, and it stops implying that `Count_` is a kind a
// `TypeRecord` could carry. The `Count_` abort below already names the sentinel
// in its own words, so nothing is lost.
[[noreturn]] void reinternFatal(TypeKind k, char const* why) {
    auto const        name  = typeKindNameOrEmpty(k);
    std::string const shown = name.empty()
        ? std::format("<unnamed kind #{}>", static_cast<std::uint32_t>(k))
        : std::string{name};
    std::fputs("dss::reinternType fatal: TypeKind ", stderr);
    std::fputs(shown.c_str(), stderr);
    std::fputs(" ", stderr);
    std::fputs(why, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

// ── The ACYCLIC half of a composite's identity ──────────────────────────────
//
// `spine_` describes a field's type down to, but NOT through, the composites it
// reaches: every composite becomes a placeholder plus an entry in `refs`. That
// split is what makes the whole thing computable — the walk below cannot cycle,
// because a cycle in a C type graph must pass through a composite, and this walk
// stops at every composite. All the cycles end up in `refs`, where the fixed
// point in `finalize_` handles them as ordinary edges.
void CompositeIdentityIndex::spine_(TypeInterner const& src, TypeId id,
                                    std::uint64_t& h,
                                    std::vector<std::uint32_t>& refs) {
    if (!id.valid()) { mix(h, std::string_view{"!"}); return; }
    // RAW kind, never the transparent `kind()`: a `volatile T` must not describe
    // as a plain `T`, or the cross-CU merge silently drops the qualifier
    // (c27, D-CSUBSET-VOLATILE-POINTEE).
    TypeKind const kind = src.get(id).kind;
    mix(h, static_cast<std::uint64_t>(kind));

    if (kind == TypeKind::Struct || kind == TypeKind::Union) {
        mix(h, std::string_view{"<composite>"});
        refs.push_back(nodeFor_(src, id));
        return;
    }
    if (kind == TypeKind::VolatileQual) {
        mix(h, static_cast<std::uint64_t>(src.qualifierBits(id)));
        spine_(src, src.stripVolatile(id), h, refs);
        return;
    }
    // Every other kind is hash-consed by (kind, name, extensionKind, scalars,
    // operands) in the host, so the signature spells the same tuple.
    mix(h, src.name(id));
    mix(h, static_cast<std::uint64_t>(src.get(id).extensionKind.v));
    if (kind == TypeKind::FnSig) {
        mix(h, static_cast<std::uint64_t>(src.fnIsVariadic(id) ? 1 : 0));
    }
    std::span<std::int64_t const> scalars = src.scalars(id);
    mix(h, static_cast<std::uint64_t>(scalars.size()));
    for (std::int64_t s : scalars) mix(h, static_cast<std::uint64_t>(s));
    std::span<TypeId const> ops = src.operands(id);
    mix(h, static_cast<std::uint64_t>(ops.size()));
    for (TypeId op : ops) spine_(src, op, h, refs);
}

// ★ EVERY CHANNEL `reinternType` CARRIES ACROSS MUST BE HERE, and the argument
// is one line: this signature is a claim that two composites will reintern to
// interchangeable host types, so anything the reintern preserves and this omits
// is a layout difference that would be merged away. Hence the same accessor list
// the composite arm of `reinternType` uses — packed, explicit field offsets,
// member alignas, whole-composite alignas, bit-field widths. ⓘ Adding a channel
// to `completeComposite` without adding it here reopens exactly the silent
// ABI-merge class `D-CSUBSET-PACKED` and `D-CSUBSET-COMPOSITE-ALIGNED` are about.
void CompositeIdentityIndex::localSignature_(TypeInterner const& src, TypeId id,
                                             std::uint64_t& h,
                                             std::vector<std::uint32_t>& refs) {
    TypeKind const kind = src.get(id).kind;
    h = kFnvOffset;
    mix(h, static_cast<std::uint64_t>(kind));
    // ⚠ THE NAME GOES IN WITHOUT ITS DECL SITE. An anonymous member is named
    // `<anon:RULE:NODEID>` and that NODEID is a per-CU AST index, so mixing it
    // raw makes every anonymous composite unique to its CU -- and every named
    // struct that reaches one inherits the split. ✔MEASURED on 103-TU sqlite
    // before this line existed: **98 tags forked, each with a SINGLE local
    // layout signature**, `Parse` / `Table` / `Select` / `Index` among them.
    // See `core/types/anon_member_name.hpp` for the whole argument.
    mix(h, anonNameWithoutDeclSite(src.name(id)));
    mix(h, static_cast<std::uint64_t>(src.isPacked(id) ? 1 : 0));
    mix(h, static_cast<std::uint64_t>(src.explicitCompositeAlign(id)));
    // ★★ TF-C82 (D-PP-PRAGMA-REGISTRY): the `#pragma pack(N)` member-alignment CAP.
    // ✔MEASURED at base `01642ee3`: this channel was ABSENT from both this signature
    // and the composite arm's `completeComposite` call, which is precisely the omission
    // the note above forbids — the same field list under caps 4 and 8 has different
    // offsets AND a different size, so two capped composites were merging onto one host
    // type and a capped composite was reinterning UNCAPPED. Adding it here and at the
    // sink closes it; `PackCapSurvivesReintern` pins it.
    mix(h, static_cast<std::uint64_t>(src.maxFieldAlign(id)));
    std::span<TypeId const>       fields = src.operands(id);
    std::span<std::int64_t const> widths = src.scalars(id);
    bool const hasOffsets = src.hasExplicitOffsets(id);
    bool const hasAligns  = src.hasExplicitAligns(id);
    // D-CSUBSET-PER-MEMBER-PACKED: the per-FIELD packed flags. Omitting them merges a
    // composite whose member is individually packed onto the undecorated one — and on
    // the `{char; int; double}` shape those two agree on size AND alignment and differ
    // only in one offset, so the merge would be invisible to every size-based check.
    bool const hasFieldPk = src.hasFieldPacked(id);
    mix(h, static_cast<std::uint64_t>(fields.size()));
    for (std::size_t i = 0; i < fields.size(); ++i) {
        spine_(src, fields[i], h, refs);
        mix(h, i < widths.size() ? static_cast<std::uint64_t>(widths[i]) : 0ULL);
        if (hasOffsets) mix(h, src.explicitFieldOffset(id, i).value_or(0));
        if (hasAligns)
            mix(h, static_cast<std::uint64_t>(src.explicitFieldAlign(id, i)));
        if (hasFieldPk)
            mix(h, static_cast<std::uint64_t>(src.isFieldPacked(id, i) ? 1 : 0));
    }
}

std::uint32_t CompositeIdentityIndex::nodeFor_(TypeInterner const& src,
                                               TypeId id) {
    std::uint64_t const nk = (static_cast<std::uint64_t>(src.owner().v) << 32)
                             | static_cast<std::uint64_t>(id.v);
    auto const [it, fresh] =
        nodeOf_.try_emplace(nk, static_cast<std::uint32_t>(nodes_.size()));
    if (fresh) {
        nodes_.push_back(Node{});
        Node& n = nodes_.back();
        n.owner = &src;
        n.id    = id.v;
        n.alias = it->second;
    }
    return it->second;
}

// ── observe: one node per composite, plus the tag table ─────────────────────
void CompositeIdentityIndex::observe(TypeInterner const& src) {
    if (finalized_) {
        std::fputs("dss::CompositeIdentityIndex fatal: observe() after keyFor() "
                   "- the identities are a fixed point over the whole observed "
                   "graph, so a late arrival would silently change answers that "
                   "have already been handed out.\n", stderr);
        std::abort();
    }
    // A LINEAR arena walk, not a walk from the reintern roots. Deliberate: the
    // roots are scattered (function signatures, global types, per-instruction
    // types) and a root list that drifts would silently stop observing part of
    // the CU -- "observed less than it should have" degrades into exactly the
    // forking this class removes, with nothing red anywhere. Types the merge
    // never reaches only ever ADD ambiguity, which is the safe direction.
    for (std::uint32_t v = 1; v <= src.size(); ++v) {
        TypeId const   id   = TypeId{v, src.owner().v};
        TypeKind const kind = src.get(id).kind;
        if (kind != TypeKind::Struct && kind != TypeKind::Union) continue;

        std::uint32_t const self = nodeFor_(src, id);
        if (nodes_[self].filled) continue;   // observed twice: harmless
        nodes_[self].filled = true;
        if (src.isIncompleteComposite(id)) continue;   // aliased in finalize_

        std::uint64_t              local = 0;
        std::vector<std::uint32_t> refs;
        localSignature_(src, id, local, refs);
        // ⚠ `nodeFor_` may have REALLOCATED `nodes_` while walking the fields,
        // so the reference is taken AFTER the walk, never before it. A `Node&`
        // held across `localSignature_` is a dangling reference the moment a
        // field mentions an unseen composite -- which is the common case.
        nodes_[self].local = local;
        nodes_[self].refs  = std::move(refs);

        std::string_view const name = anonNameWithoutDeclSite(src.name(id));
        if (name.empty() || isSyntheticAnonymousName(name)) continue;   // no user tag
        // ★ AMBIGUITY IS TESTED ON THE **LOCAL** SIGNATURE, WHICH IS ACYCLIC AND
        // COMPLETENESS-INSENSITIVE, and that is the whole reason `local` and
        // `refs` are split. "Does this tag have one layout?" must be answerable
        // BEFORE the fixed point exists, and it must not be confused by a
        // neighbour's completeness -- a TU that has seen `struct Btree { ... }`
        // and one that has only seen `struct Btree;` describe the SAME
        // `struct BtCursor`, and a completeness-sensitive test would call those
        // two definitions different and mark the tag ambiguous. ✔That is not a
        // hypothetical: it is what kept sqlite's `BtCursor` forked through the
        // first attempt at this fix.
        auto const [it, fresh] = canonical_.try_emplace(
            tagKey(kind, name), TagEntry{nodes_[self].local, &src, v, self, false});
        if (!fresh && it->second.localDigest != nodes_[self].local)
            it->second.ambiguous = true;
    }
}

bool CompositeIdentityIndex::resolveDefinition(TypeInterner const& src,
                                               TypeId srcId,
                                               TypeInterner const*& defIn,
                                               TypeId& defId) const {
    // The SAME normalization `observe` keyed on. Two spellings of one lookup is
    // how the tag table and its readers drift apart, and this file has already
    // paid once for exactly that (the identity and the reintern disagreeing).
    std::string_view const name = anonNameWithoutDeclSite(src.name(srcId));
    if (name.empty() || isSyntheticAnonymousName(name)) return false;
    auto const it = canonical_.find(tagKey(src.get(srcId).kind, name));
    if (it == canonical_.end() || it->second.ambiguous
        || it->second.owner == nullptr) {
        return false;
    }
    defIn = it->second.owner;
    defId = TypeId{it->second.defId, it->second.owner->owner().v};
    return true;
}

// ── THE FIXED POINT ─────────────────────────────────────────────────────────
//
// ⚠⚠ THE OBVIOUS ALGORITHM DOES NOT TERMINATE ON REAL CODE, AND THIS ONE EXISTS
// BECAUSE THAT WAS MEASURED RATHER THAN FEARED. A recursive digest that walks
// through composites and cuts cycles with a de Bruijn backreference cannot
// memoize any subtree that back-references an OUTER composite -- so inside a
// mutually recursive cluster nothing is cacheable and the walk re-expands once
// per path. ✔MEASURED on the 103-TU sqlite corpus, whose
// `sqlite3`/`Vdbe`/`Parse`/`Expr` cluster is exactly that shape: **952 s of CPU,
// 1.2 GB resident, no output**, killed by PID. The same corpus finishes in
// seconds here.
//
// ★ THE ALGORITHM IS ORDINARY AND THAT IS THE POINT: iterative partition
// refinement, the standard way to decide equivalence of recursive types.
// `h(0) = local`, then `h(k+1)[n] = mix(local[n], h(k)[refs...])`. Two
// composites keep the same hash only while nothing within k levels tells them
// apart, so the partition REFINES monotonically and stops when a round adds no
// new class. Cycles need no special handling at all -- they are just edges.
//
// ⓘ ORDER-INDEPENDENT BY CONSTRUCTION, which the merge requires: a hash is
// built from local signatures and neighbours' hashes, never from a node INDEX,
// so observing the CUs in a different order gives the same values.
//
// ⚠ THE ROUND BOUND IS A BACKSTOP, NOT THE TERMINATION CONDITION. Refinement
// converges in at most `nodes` rounds; the cap is `nodes + 2`, so hitting it is
// impossible for a sound graph, and `refinementRounds()` is exposed so a test
// can assert convergence rather than assume it. A bound that BOUND would mean
// two distinct types share a key -- caught loudly by `completeComposite`, never
// silently.
void CompositeIdentityIndex::finalize_() const {
    finalized_ = true;
    auto& nodes  = const_cast<std::vector<Node>&>(nodes_);
    auto& opaque = const_cast<std::unordered_map<std::string, std::uint32_t>&>(
        opaqueNode_);

    // (1) ALIAS every forward declaration onto what it actually denotes.
    std::size_t const observed = nodes.size();
    for (std::uint32_t i = 0; i < observed; ++i) {
        Node& n = nodes[i];
        if (n.owner == nullptr) continue;                 // synthetic
        TypeId const id{n.id, n.owner->owner().v};
        if (!n.owner->isIncompleteComposite(id)) continue;
        TypeInterner const* di = nullptr;
        TypeId              dd{};
        if (resolveDefinition(*n.owner, id, di, dd)) {
            auto const it = nodeOf_.find(
                (static_cast<std::uint64_t>(di->owner().v) << 32)
                | static_cast<std::uint64_t>(dd.v));
            if (it != nodeOf_.end()) { n.alias = it->second; continue; }
        }
        // No definition anywhere, or definitions that disagree. Every forward
        // declaration of the tag still collapses onto ONE shared opaque node --
        // strictly better than one per declaring CU, and it cannot unify with a
        // definition it is not entitled to.
        std::string const key = tagKey(n.owner->get(id).kind,
                                       anonNameWithoutDeclSite(n.owner->name(id)));
        auto const [oit, fresh] =
            opaque.try_emplace(key, static_cast<std::uint32_t>(nodes.size()));
        if (fresh) {
            std::uint64_t lh = kFnvOffset;
            mix(lh, std::string_view{"opaque-composite"});
            mix(lh, key);
            Node syn;
            syn.local = lh;
            syn.alias = static_cast<std::uint32_t>(nodes.size());
            nodes.push_back(std::move(syn));
        }
        nodes[i].alias = oit->second;
    }
    // An alias never points at another alias: only INCOMPLETE nodes are aliased,
    // and a resolution always lands on a COMPLETE one (or on a synthetic node,
    // which is its own alias).

    // (2) REFINE.
    std::vector<std::uint64_t> h(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) h[i] = nodes[i].local;
    std::vector<std::uint64_t> next(nodes.size());
    std::size_t const bound = nodes.size() + 2;
    std::size_t distinct =
        std::unordered_set<std::uint64_t>(h.begin(), h.end()).size();
    std::size_t r = 0;
    for (; r < bound; ++r) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            std::uint64_t x = kFnvOffset;
            mix(x, nodes[i].local);
            for (std::uint32_t ref : nodes[i].refs) mix(x, h[nodes[ref].alias]);
            next[i] = x;
        }
        h.swap(next);
        std::size_t const d =
            std::unordered_set<std::uint64_t>(h.begin(), h.end()).size();
        if (d == distinct) { ++r; break; }   // a round that adds no class: done
        distinct = d;
    }
    rounds_ = r;
    hash_   = std::move(h);

    // ── (3) HOW MANY TAGS THIS MERGE COULD NOT UNIFY ────────────────────────
    //
    // ★ THIS COUNT IS THE INSTRUMENT THAT FOUND THE LAST TWO DEFECTS IN THIS
    // FILE, AND IT IS KEPT AS A FACT RATHER THAN AS A PRINTF FOR EXACTLY THAT
    // REASON. A `stderr` dump behind an environment variable is a tool that dies
    // with the session that wrote it — this project has measured that twice — so
    // the quantity is COMPUTED and EXPOSED, and a test can assert it instead of
    // a human re-deriving it.
    //
    // A tag is FORKED when its complete definitions do not all end with one
    // identity. ✔MEASURED on the 103-TU sqlite corpus: **98 forked, every one of
    // them with a SINGLE local layout signature** — which is what said the split
    // could not be a real layout difference and had to be identity leaking in
    // from somewhere. It was: `<anon:RULE:NODEID>`, a per-CU AST node index
    // inside a TYPE's name. After that fix, **0**.
    // ⓘ A non-zero count is NOT by itself a defect: two `.c` files may legally
    // define a private `struct Node` differently, and those SHOULD fork. It is a
    // number to look at when a merge behaves as if one C type were two.
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> byTag;
    for (auto const& n : nodes) {
        if (n.owner == nullptr) continue;
        TypeId const id{n.id, n.owner->owner().v};
        if (n.owner->isIncompleteComposite(id)) continue;
        std::string_view const nm = anonNameWithoutDeclSite(n.owner->name(id));
        if (nm.empty() || isSyntheticAnonymousName(nm)) continue;
        byTag[tagKey(n.owner->get(id).kind, nm)].insert(hash_[n.alias]);
    }
    forkedTags_ = 0;
    for (auto const& [k, s] : byTag) {
        (void)k;
        if (s.size() > 1) ++forkedTags_;
    }
}

std::uint32_t CompositeIdentityIndex::nodeIndex_(TypeInterner const& src,
                                                 TypeId id) const {
    auto const it = nodeOf_.find(
        (static_cast<std::uint64_t>(src.owner().v) << 32)
        | static_cast<std::uint64_t>(id.v));
    if (it == nodeOf_.end()) {
        // The composite was never observed. That is a CALLER contract breach --
        // every source interner the merge reinterns from must be observed first
        // -- and guessing a key here is how one C type forks again, silently.
        std::fputs("dss::CompositeIdentityIndex fatal: keyFor() on a composite "
                   "from an interner that was never observe()d - every source of "
                   "the merge must be observed before the first reintern.\n",
                   stderr);
        std::abort();
    }
    return it->second;
}

std::uint64_t CompositeIdentityIndex::keyFor(TypeInterner const& src,
                                             TypeId srcId) const {
    // ⚠ ONE CALL, NO SPECIAL CASES, AND THAT IS THE POINT. An earlier draft
    // branched here -- own digest for a definition, canonical digest for a
    // forward declaration -- and that branch is where the identity rule and the
    // reintern drifted apart, which `completeComposite` reported by ABORTING
    // (*"composite re-completed with different fields"*) rather than by giving a
    // wrong answer. The alias edge performs the substitution ONCE, in the graph,
    // so there is no second rule left to disagree with the first.
    if (!finalized_) finalize_();
    return hash_[nodes_[nodeIndex_(src, srcId)].alias];
}

TypeId reinternType(TypeInterner const& src, TypeId srcId, TypeLattice& dstHost,
                    std::unordered_map<std::uint32_t, TypeId>& remap) {
    CompositeIdentityIndex scratch;
    scratch.observe(src);
    return reinternType(src, srcId, dstHost, remap, scratch);
}

TypeId reinternType(TypeInterner const& src, TypeId srcId, TypeLattice& dstHost,
                    std::unordered_map<std::uint32_t, TypeId>& remap,
                    CompositeIdentityIndex const& index) {
    // Sentinel / invalid → identity. InvalidType carries no CU provenance, so it
    // is meaningful (as "no type") against any lattice.
    if (!srcId.valid()) return InvalidType;

    // Memo hit: the same source TypeId always maps to the same host TypeId.
    if (auto it = remap.find(srcId.v); it != remap.end()) return it->second;

    TypeInterner& dst        = dstHost.interner();
    // c27 (D-CSUBSET-VOLATILE-POINTEE): read the RAW record kind, NOT the
    // transparent `kind()` — `kind()` sees THROUGH a VolatileQual skin to the
    // material kind, so using it here would reintern a `volatile T` AS a plain `T`,
    // silently DROPPING the qualifier (a miscompile in the cross-CU merge / text
    // round-trip). The raw kind preserves VolatileQual so the wrapper round-trips.
    TypeKind const kind      = src.get(srcId).kind;

    // ── type qualifiers (D-CSUBSET-VOLATILE-POINTEE / D-CSUBSET-QUAL-BITSET) ──
    // A VolatileQual wraps exactly ONE inner type + a QualBit mask. Re-intern the
    // inner into the host, then re-wrap with the SAME mask. Handled HERE (before the
    // transparent operand read below) because `src.operands(VolatileQual(T))`
    // redirects to T's operands (NOT [T]); `stripVolatile` recovers the material
    // inner (the skin never nests). Re-wrap via `qualified(inner, bits)`, NOT
    // `volatileQualified` — the latter sets only the Volatile bit and would DROP an
    // `_Atomic` (or `_Atomic volatile`) qualifier on this cross-CU merge / text
    // round-trip, a silent loss-of-atomicity miscompile.
    if (kind == TypeKind::VolatileQual) {
        TypeId const inner  = reinternType(src, src.stripVolatile(srcId),
                                           dstHost, remap, index);
        TypeId const result = dst.qualified(inner, src.qualifierBits(srcId));
        remap.emplace(srcId.v, result);
        return result;
    }

    // ── NOMINAL composites (D-CSUBSET-SELF-REFERENTIAL-STRUCT) ──
    // A composite's field list may CONTAIN A CYCLE — a self-referential
    // `struct N { struct N *next; }` is a Ptr whose pointee is N's OWN TypeId.
    // Memoizing only AFTER recursing the fields (the DAG path below) would loop
    // forever on that edge. So FORWARD-MINT the host composite, INSERT it into the
    // memo BEFORE recursing the fields, THEN re-intern the fields and complete it —
    // the self-ref field's recursion hits the memo and resolves to the placeholder.
    //
    // ★ HOST IDENTITY IS THE COMPOSITE'S STRUCTURE, NOT ITS DECLARATION SITE
    // (D-MIR-MERGE-COMPOSITE-HOST-IDENTITY-IS-THE-DECLARATION-SITE). This used to
    // be `(srcId.arenaTag << 32) | srcId.v`, documented as "distinct source
    // composites stay distinct" -- true, and it also kept ONE C type apart from
    // ITSELF once per CU that mentioned it, because a CU that only ever sees
    // `typedef struct Bitvec Bitvec;` contributes its own incomplete `Bitvec`.
    // ✔MEASURED on 103-TU sqlite: that fork is what made the release build emit
    // 5 x `I_StoreValueTypeMismatch` and produce no artifact, both sides of every
    // store being one C type spelled two ways. `CompositeIdentityIndex::keyFor`
    // computes the identity from the SOURCE side -- available before the mint the
    // cycle forces -- and same-tag/different-layout composites still get
    // different keys, so nothing C keeps apart is merged. See the header.
    if (kind == TypeKind::Struct || kind == TypeKind::Union) {
        std::uint64_t const declSiteKey = index.keyFor(src, srcId);
        // ⚠ THE NAME IS PART OF THE HOST KEY, SO IT MUST BE THE SAME NAME THE
        // IDENTITY WAS COMPUTED FROM. `forwardComposite` keys on
        // (kind, name, declSiteKey); passing the RAW `<anon:RULE:NODEID>` here
        // while the identity used the decl-site-independent form gives two
        // anonymous composites ONE key and TWO host types, and their shared
        // parent then completes twice with different fields -- which
        // `completeComposite` reports by ABORTING. ✔MEASURED on 103-TU sqlite:
        // exactly that abort, with the type graph otherwise perfectly unified
        // (`forked-tags=0`). The two halves take the same name or neither does.
        TypeId const fwd = dst.forwardComposite(
            kind, anonNameWithoutDeclSite(src.name(srcId)), declSiteKey);
        remap.emplace(srcId.v, fwd);   // BEFORE recursion → breaks the cycle
        // An INCOMPLETE source composite (forward-declared, never defined) stays
        // incomplete in the host: re-intern nothing, leave the placeholder.
        if (src.isIncompleteComposite(srcId)) return fwd;
        std::span<TypeId const>       srcFields = src.operands(srcId);
        std::span<std::int64_t const> srcWidths = src.scalars(srcId);
        std::vector<TypeId> fields;
        fields.reserve(srcFields.size());
        for (TypeId f : srcFields)
            fields.push_back(reinternType(src, f, dstHost, remap, index));
        // Decode the (width+1)/0 scalar form back to the kNotBitfield/width form
        // completeComposite re-encodes (round-trip identity for a bitfield-free
        // composite, whose scalars are empty → no per-field widths).
        std::vector<std::int64_t> widths;
        if (!srcWidths.empty()) {
            widths.reserve(srcWidths.size());
            for (std::int64_t enc : srcWidths)
                widths.push_back(enc <= 0 ? kNotBitfield : enc - 1);
        }
        // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): carry EXPLICIT field offsets across
        // reintern — without this the reinterned composite loses its overlapping
        // layout (HighPart falls back to natural offset 8) and forks the TypeId that
        // `.member` scope keys on. Empty when the source lays out naturally.
        std::vector<std::uint64_t> offsets;
        if (src.hasExplicitOffsets(srcId)) {
            offsets.reserve(srcFields.size());
            for (std::size_t i = 0; i < srcFields.size(); ++i)
                offsets.push_back(src.explicitFieldOffset(srcId, i).value_or(0));
        }
        // D-CSUBSET-MEMBER-ALIGNAS: carry member-alignas overrides across reintern —
        // without this the reinterned composite loses its declared field alignment
        // (falls back to natural) and forks the TypeId. Empty when the source aligns
        // naturally. A source struct can carry offsets OR aligns but not both
        // (completeComposite rejects the pair), so exactly one span is non-empty.
        std::vector<std::uint32_t> aligns;
        if (src.hasExplicitAligns(srcId)) {
            aligns.reserve(srcFields.size());
            for (std::size_t i = 0; i < srcFields.size(); ++i)
                aligns.push_back(src.explicitFieldAlign(srcId, i));
        }
        // D-CSUBSET-PACKED: carry the whole-composite packed flag across reintern.
        // Without it a packed struct crossing a CU/round-trip boundary silently
        // reinterns as UNPACKED — a silent ABI miscompile (the exact reason the
        // `completeComposite` packed parameter is non-defaulted: this call FAILS TO
        // COMPILE if packed is forgotten). packed + explicit offsets never coexist
        // (completeComposite rejects the pair), so `offsets` is empty when packed.
        //
        // D-CSUBSET-COMPOSITE-ALIGNED (TF-C73): carry the WHOLE-COMPOSITE explicit
        // alignment across too. Dropping it here is the same class of silent
        // miscompile as dropping packed, and strictly worse to notice: a
        // `__attribute__((aligned(16)))` struct would cross a CU / static-link merge
        // boundary and come back UNDER-ALIGNED with its size quietly shrunk (16 → 5
        // for the packed+aligned witness), with no diagnostic anywhere. Unlike
        // `packed` this parameter IS defaulted (an undefaulted one cannot follow the
        // defaulted spans — see the header note), so the compile-time forcing does
        // not apply and the guarantee is carried by the reintern round-trip pin
        // instead: `CompositeExplicitAlignSurvivesReintern` asserts the value AND the
        // resulting layout survive the hop.
        //
        // D-CSUBSET-PER-MEMBER-PACKED: carry the PER-FIELD packed flags for the same
        // reason, and this is the channel whose loss is hardest to see: a struct
        // whose one member is individually packed reinterns with the SAME size and
        // the SAME alignment as the undecorated one and only that member's offset
        // moves, so nothing downstream that compares sizes could tell them apart.
        // Empty when no member is individually packed (every composite that predates
        // the channel). Like `packed`, it never coexists with explicit offsets
        // (completeComposite rejects the pair).
        std::vector<std::uint8_t> fieldPacked;
        if (src.hasFieldPacked(srcId)) {
            fieldPacked.reserve(srcFields.size());
            for (std::size_t i = 0; i < srcFields.size(); ++i)
                fieldPacked.push_back(src.isFieldPacked(srcId, i) ? 1u : 0u);
        }
        // ★★ TF-C82: `maxFieldAlign` — the `#pragma pack(N)` cap — was NOT carried
        // here at base `01642ee3`, so a capped composite reinterned UNCAPPED: a
        // silent ABI change of exactly the class this call site's own note names.
        // ✔MEASURED: `{char a; long long z;}` under cap 4 is sizeof 12 / _Alignof 4
        // with z@4, uncapped 16 / 8 with z@8. Pinned by `PackCapSurvivesReintern`.
        dst.completeComposite(fwd, fields, src.isPacked(srcId), widths, offsets, aligns,
                              src.explicitCompositeAlign(srcId),
                              src.maxFieldAlign(srcId), fieldPacked);
        return fwd;
    }

    // ── Termination precondition for the NON-composite DAG: ACYCLIC. ──
    // Every non-composite type is interned bottom-up (children before parents), so
    // an operand TypeId always refers to an ALREADY-interned (lower) id; there are
    // no forward / mutable operand edges among them, hence no cycles. (Composites —
    // the only types that CAN cycle — are handled above with memo-before-recursion.)
    // The GuardedSpan results decay to raw spans here
    // (D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD):
    // SAFE — `src` is `const` and every intern below targets
    // `dst`, a DISTINCT interner, so `src`'s pools are never mutated while these
    // views are live (the dangling hazard the guard exists for cannot arise).
    std::span<TypeId const>       srcOps    = src.operands(srcId);
    std::span<std::int64_t const> srcScalar = src.scalars(srcId);

    // Bottom-up: re-intern every operand TypeId into the host FIRST, so the
    // host already holds the children when we build the parent.
    std::vector<TypeId> ops;
    ops.reserve(srcOps.size());
    for (TypeId op : srcOps)
        ops.push_back(reinternType(src, op, dstHost, remap, index));

    TypeId result{};
    switch (kind) {
        // ── primitives: rebuilt from the kind alone ──
        case TypeKind::Bool:
        case TypeKind::I8:  case TypeKind::I16: case TypeKind::I32:
        case TypeKind::I64: case TypeKind::I128:
        case TypeKind::U8:  case TypeKind::U16: case TypeKind::U32:
        case TypeKind::U64: case TypeKind::U128:
        case TypeKind::F16: case TypeKind::F32: case TypeKind::F64:
        case TypeKind::F80: case TypeKind::F128:
        case TypeKind::Char: case TypeKind::Byte: case TypeKind::Void:
        case TypeKind::NullptrT:   // C23 nullptr_t: operand-less primitive scalar
            // D-LANG-TYPE-IDENTITY-VOCABULARY: carry the VOCABULARY TAG across the
            // boundary. Rebuilding from the kind alone would drop it on EVERY
            // primitive reintern — silently re-collapsing `long` onto `int` (and
            // `long double` onto `double`) the moment a type crosses a CU / static
            // -link merge / text round-trip, exactly the identity-from-representation
            // defect this split removes. `src.name()` is generic: it returns "" for
            // every kind that never populates the slot, so an anonymous primitive
            // round-trips bit-identically through the 2-arg overload.
            result = dst.primitive(kind, src.name(srcId));
            break;

        // ── single-operand indirections: operands=[inner] ──
        case TypeKind::Ptr:       result = dst.pointer(ops[0]);   break;
        case TypeKind::Ref:       result = dst.reference(ops[0]); break;
        case TypeKind::Nullable:  result = dst.nullable(ops[0]);  break;
        case TypeKind::Optional:  result = dst.optional(ops[0]);  break;
        case TypeKind::Slice:     result = dst.slice(ops[0]);     break;

        // ── SIMD: operands=[element], scalars=[lanes] / [rows, cols] ──
        case TypeKind::Vector:
            result = dst.vector(ops[0], srcScalar[0]);
            break;
        case TypeKind::Matrix:
            result = dst.matrix(ops[0], srcScalar[0], srcScalar[1]);
            break;

        // ── array: operands=[element], scalars=[length] ──
        case TypeKind::Array:
            result = dst.array(ops[0], srcScalar[0]);
            break;

        // ── C23 _BitInt(N): NO operands; scalars=[N, signed] (D-CSUBSET-BITINT).
        //    Rebuild via the width+signedness builder so a `_BitInt(N)` crossing a
        //    CU / text round-trip keeps its EXACT width and signedness (a dropped
        //    signedness would silently flip the wrap/compare semantics). ──
        case TypeKind::BitInt:
            result = dst.bitInt(srcScalar[0], srcScalar.size() > 1 && srcScalar[1] != 0);
            break;

        // ── C99 _Complex: operands=[element]; NO scalars (D-CSUBSET-COMPLEX).
        //    Rebuild via the element builder so a `_Complex` crossing a CU / text
        //    round-trip keeps its exact element float type. ──
        case TypeKind::Complex:
            result = dst.complex(ops[0]);
            break;

        // ── tuple: operands=[elements...] ──
        case TypeKind::Tuple:
            result = dst.tuple(ops);
            break;

        // ── nominal aggregates: handled ABOVE (forward-mint + complete, the
        //    cycle-safe path). Reaching here means the early composite return
        //    was bypassed — interner/control-flow corruption. Fail loud. ──
        case TypeKind::Struct:
        case TypeKind::Union:
            reinternFatal(kind, "is a nominal composite and must be re-interned via "
                                "the forward-mint path, not the operand-DAG switch");

        // ── qualifier skin: handled ABOVE (strip + re-wrap with the SAME QualBit
        //    mask). Reaching here means that early return was bypassed, which would
        //    silently DROP `volatile` / `_Atomic`. An explicit arm rather than a
        //    `default:` — a catch-all would re-arm this trap for the next TypeKind
        //    someone adds, which is exactly what -Wswitch exists to prevent. ──
        case TypeKind::VolatileQual:
            reinternFatal(kind, "is a qualifier skin and must be re-interned via the "
                                "strip-and-rewrap path, not the operand-DAG switch");

        // ── enum: NO operands; name + scalars=[(int)underlyingTypeKind] ──
        case TypeKind::Enum:
            result = dst.enumType(src.name(srcId),
                                  static_cast<TypeKind>(srcScalar[0]));
            break;

        // ── fnSig: operands=[result, params...]; scalars=[(int)cc] or
        //    [(int)cc, isVariadic]. Split result vs params (the encoding the
        //    typed decoders expose) and honor the variadic flag exactly. ──
        case TypeKind::FnSig: {
            TypeId const resultTy = ops[0];                  // operands[0]
            std::span<TypeId const> params{ops.data() + 1, ops.size() - 1};
            auto const cc = static_cast<CallConv>(srcScalar[0]);
            result = dst.fnSig(params, resultTy, cc, src.fnIsVariadic(srcId));
            break;
        }

        // ── extension: extensionKind + name + operands=[typeArgs] +
        //    scalars=[scalarArgs] ──
        case TypeKind::Extension:
            result = dst.extension(src.get(srcId).extensionKind, src.name(srcId),
                                   ops, srcScalar);
            break;

        // ── never-interned kinds: no public builder exists, so these cannot
        //    legitimately appear in a TypeInterner arena. Reaching here is
        //    interner corruption — fail loud rather than silently drop them. ──
        case TypeKind::FnPtr:
        case TypeKind::Param:
        case TypeKind::Bind:
            reinternFatal(kind, "has no interner builder and cannot be "
                                "re-interned (never legitimately interned)");

        // Count_ is the enum's cardinality sentinel, not a real type.
        case TypeKind::Count_:
            reinternFatal(kind, "is the TypeKind cardinality sentinel, not a "
                                "real type");
    }

    // Defensive: any future TypeKind added without a case above would leave
    // `result` invalid here — fail loud rather than memoize a bad mapping. (The
    // switch is exhaustive today; this guards a later enum extension.)
    if (!result.valid()) {
        reinternFatal(kind, "produced no host TypeId (unhandled kind — add a "
                            "case to reinternType)");
    }
    if (isPrimitiveTypeKind(kind) && !ops.empty()) {
        // A primitive must have no operands; if one ever does, the encoding
        // assumption above is wrong — fail loud rather than silently ignore it.
        reinternFatal(kind, "is a primitive but carried operands");
    }

    remap.emplace(srcId.v, result);
    return result;
}

} // namespace dss
