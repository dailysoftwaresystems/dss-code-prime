#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Type re-intern walker (Cycle 25, Stage A) — the foundation for a whole-program
// MIR merge that unifies N per-CU type interners into one host TypeLattice.
//
// Each TypeInterner is CU-scoped: its TypeIds are interner-relative and stamped
// with the owning CompilationUnitId. To merge MIR from several CUs into one
// module, every TypeId that crosses a CU boundary must be re-interned into a
// single destination host lattice so the host's hash-consing can canonicalize
// structurally-identical types from different CUs to one TypeId.
//
// `reinternType` does exactly that: given a TypeId from source interner `src`,
// it returns the equivalent TypeId interned into `dstHost`'s lattice, recursing
// bottom-up (types are referential — a pointer's pointee, a fnSig's
// result/params, a struct's fields are themselves TypeIds that must be
// re-interned first). The `remap` memo guarantees a given source TypeId maps to
// a stable destination TypeId (and breaks the recursion's repeated work);
// structurally-identical types collapse in the host because each host builder
// hash-conses.
//
// AGNOSTIC: the walker keys on `TypeKind` alone — no language / target / format
// branch. FAIL-LOUD: every TypeKind in core_type.hpp is handled explicitly; an
// unhandled / never-interned kind aborts with the kind name rather than silently
// mis-reinterning.

namespace dss {

// ── CROSS-CU COMPOSITE IDENTITY (D-MIR-MERGE-COMPOSITE-HOST-IDENTITY-IS-THE-DECLARATION-SITE) ──
//
// A composite's host identity used to be its SOURCE DECLARATION SITE (the
// owning CU's arena tag packed with the source TypeId). That is not an identity
// at all across a merge: it makes the SAME C type fork once per CU that
// mentions it. ✔MEASURED on the 103-TU sqlite corpus (cycle P36): sqlite
// declares `typedef struct Bitvec Bitvec;` in a header and defines it in
// `bitvec.c` only, so every TU that handles a `Bitvec*` without seeing the
// definition contributed its OWN incomplete `Bitvec`, the defining TU
// contributed the complete one, and the merge kept all of them apart. The
// release build then failed with 5 x `I_StoreValueTypeMismatch` and produced NO
// ARTIFACT -- both sides of every one of those stores being the SAME C type,
// spelled two ways.
//
// ★ THE RULE THIS RESTORES IS THE ONE THIS HEADER ALREADY DECLARES two
// paragraphs down: "the host's hash-consing can canonicalize
// structurally-identical types from different CUs to one TypeId". Every
// non-composite kind already obeys it. Composites were the exception ONLY
// because a composite may CONTAIN A CYCLE, so its host id must be minted before
// its fields are known -- and the decl-site key was what stood in for an
// identity that could not be computed yet. It CAN be computed: from the SOURCE
// side, before any minting, by `CompositeIdentityIndex`.
//
// ⚠⚠ TWO SAME-TAG COMPOSITES WITH DIFFERENT LAYOUTS MUST NEVER MERGE, AND
// SKIPPING THAT CONSTRAINT WOULD FIX ONE MISCOMPILE BY INTRODUCING A WORSE ONE.
// It is not a corner case: two `.c` files may each define a private
// `struct Node` differently, and two block-scoped `struct S`s in one CU are
// distinct types C admits by construction. So the key is the composite's full
// RECURSIVE STRUCTURAL DIGEST -- kind, tag, packing, explicit offsets/aligns,
// and every field's own digest -- and NOT its tag. Same tag + different layout
// therefore yields different keys and stays forked, exactly as today.
// ⓘ A digest COLLISION (same tag, same 64 bits, different layout) cannot
// silently miscompile: the two would land on one forward-minted host id and
// `completeComposite` REFUSES a conflicting re-completion, loudly.
//
// ★ THE HALF THAT NEEDS GLOBAL KNOWLEDGE is the forward declaration. An
// INCOMPLETE `struct Bitvec` has no layout to digest, so it cannot find its own
// definition by itself, and whether it may unify depends on CUs it will never
// see. That is what `observe()` is for: it is called once per SOURCE interner
// for ALL of them BEFORE the first reintern, so the answer does not depend on
// the order the CUs are walked. A tag with exactly ONE complete layout anywhere
// unifies its forward declarations onto that layout; a tag with NONE unifies
// them onto one shared opaque placeholder; a tag with TWO OR MORE conflicting
// layouts is AMBIGUOUS and its forward declarations stay opaque and separate
// from every definition -- conservative, and never a merge that C does not
// license.
class DSS_EXPORT CompositeIdentityIndex {
public:
    // Record EVERY Struct/Union in `src` -- complete and forward-declared alike.
    // Call once per SOURCE interner, for EVERY source, BEFORE the first
    // `reinternType`. Observing an interner twice is harmless; observing one
    // after the first `keyFor` is a CONTRACT VIOLATION and fails loud, because
    // the identities are a FIXED POINT over the whole observed graph and a late
    // arrival would silently change answers already handed out.
    void observe(TypeInterner const& src);

    // The `declSiteKey` to forward-mint `srcId` (a Struct/Union in `src`) under.
    // Computes the fixed point on first call.
    [[nodiscard]] std::uint64_t keyFor(TypeInterner const& src,
                                       TypeId srcId) const;

    // Substitute the DEFINITION for a forward declaration. `srcId` must be an
    // incomplete Struct/Union in `src`; on success `defIn`/`defId` name the one
    // definition of that tag -- WHICH MAY LIVE IN ANOTHER CU'S INTERNER. Returns
    // false when the tag has no definition anywhere (a genuinely opaque type) or
    // when its definitions disagree, in which case the forward declaration stays
    // opaque and unifies only with other declarations of the same tag.
    [[nodiscard]] bool resolveDefinition(TypeInterner const& src, TypeId srcId,
                                         TypeInterner const*& defIn,
                                         TypeId& defId) const;

    // Distinct (kind, tag) pairs with at least one COMPLETE definition. For
    // tests and for a caller that wants to assert the pre-pass actually ran.
    [[nodiscard]] std::size_t observedTagCount() const noexcept {
        return canonical_.size();
    }
    // Composite nodes recorded, and refinement rounds the fixed point took.
    // Exposed so a test can assert the algorithm CONVERGED rather than hit its
    // bound -- a bound that binds is a conflation, and a conflation is exactly
    // what `completeComposite` aborts on.
    [[nodiscard]] std::size_t nodeCount()       const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t refinementRounds() const noexcept { return rounds_; }
    // Tags whose complete definitions did NOT all end with one identity. ⓘ Not
    // by itself a defect -- two `.c` files may legally define a private
    // `struct Node` differently and those SHOULD fork -- but it is the number to
    // look at when a merge behaves as if one C type were two, and it is what
    // found the last two defects in this file. ✔MEASURED on 103-TU sqlite: 98
    // before the anonymous-member-name fix, 0 after.
    [[nodiscard]] std::size_t forkedTagCount() const noexcept {
        if (!finalized_) finalize_();
        return forkedTags_;
    }

private:
    // ── ONE NODE PER COMPOSITE, AND AN ALIAS EDGE FOR EVERY FORWARD DECLARATION ─
    // `local` is the composite's own signature with every composite it reaches
    // replaced by a placeholder; `refs` lists those composites in field order.
    // Splitting them is what makes the identity computable at all: `local` is
    // ACYCLIC (the walk stops at a composite), and the cycles live entirely in
    // `refs`, where a fixed point can handle them.
    struct Node {
        TypeInterner const* owner = nullptr;   // null for a synthetic opaque node
        std::uint32_t       id    = 0;
        std::uint64_t       local = 0;
        std::vector<std::uint32_t> refs;
        std::uint32_t       alias  = 0;        // self, or the node this resolves to
        bool                filled = false;    // its local signature was computed
    };

    // The one canonical definition of a (kind, tag), or the record that there is
    // no such thing. `ambiguous` is a FIELD rather than a sentinel digest value
    // because a sentinel would have to be a value the hash can never produce, and
    // "a hash never returns this particular value" is not a property FNV offers.
    //
    // ⚠ `owner` IS A BORROWED POINTER TO A SOURCE INTERNER. The index is a
    // per-merge object and every observed interner must outlive it -- which
    // `mergeCuMirs` guarantees, since it owns the `MergeCuInput` span for the
    // whole call. Stated because a borrowed pointer outliving its referent is the
    // one way this class can go wrong silently.
    struct TagEntry {
        std::uint64_t       localDigest = 0;   // the ambiguity test, acyclic
        TypeInterner const* owner       = nullptr;
        std::uint32_t       defId       = 0;
        std::uint32_t       node        = 0;
        bool                ambiguous   = false;
    };

    void          finalize_() const;
    std::uint32_t nodeIndex_(TypeInterner const& src, TypeId id) const;
    std::uint32_t nodeFor_(TypeInterner const& src, TypeId id);
    void          spine_(TypeInterner const& src, TypeId id, std::uint64_t& h,
                         std::vector<std::uint32_t>& refs);
    void          localSignature_(TypeInterner const& src, TypeId id,
                                  std::uint64_t& h,
                                  std::vector<std::uint32_t>& refs);

    std::unordered_map<std::string, TagEntry>       canonical_;
    std::unordered_map<std::uint64_t, std::uint32_t> nodeOf_;    // (owner<<32|id)
    std::unordered_map<std::string, std::uint32_t>   opaqueNode_;
    std::vector<Node>                                nodes_;
    // The fixed point. `mutable` because it is a pure function of what has been
    // observed, computed on demand so callers cannot forget a `finalize()` call
    // -- the kind of two-step API where the missing step is silent.
    mutable std::vector<std::uint64_t> hash_;
    mutable std::size_t                rounds_     = 0;
    mutable std::size_t                forkedTags_ = 0;
    mutable bool                       finalized_  = false;
};

// Re-intern `srcId` (an interner-relative TypeId from `src`) into `dstHost`,
// returning the host-stamped equivalent TypeId. Recurses on every operand
// TypeId first (bottom-up), then rebuilds via the matching `dstHost` builder
// with the remapped operands plus the same scalars / name / extensionKind.
//
// `remap` is the caller-owned memo keyed by `srcId.v`: a hit returns the stored
// host TypeId; otherwise the result is stored before returning. Re-using one
// `remap` across calls keeps mappings stable AND lets independent source
// TypeIds that turn out structurally identical share a single host TypeId.
//
// An invalid / sentinel `srcId` (`!srcId.valid()`) re-interns to an invalid host
// TypeId (identity — InvalidType is CU-agnostic). Any TypeKind without a known
// interner encoding (FnPtr / Param / Bind — none have a public builder, so none
// can legitimately appear in an interner's arena) aborts loud with the kind
// name: a never-interned kind reaching here is interner corruption, not a type
// to silently pass through.
// `index` supplies the cross-CU composite identity (see above). A merge passes
// ONE index, populated by `observe()` over EVERY source interner before the
// first call here, so that a forward declaration in CU 3 and the definition in
// CU 47 land on one host TypeId whichever is walked first.
[[nodiscard]] DSS_EXPORT TypeId reinternType(
    TypeInterner const& src, TypeId srcId, TypeLattice& dstHost,
    std::unordered_map<std::uint32_t, TypeId>& remap,
    CompositeIdentityIndex const& index);

// Single-source convenience overload: builds a scratch index that has observed
// `src` and nothing else. Correct for a caller with exactly ONE source interner
// (the MIR text round-trip); a MERGE must use the overload above with a shared
// index, or a forward declaration cannot see a definition in another CU.
[[nodiscard]] DSS_EXPORT TypeId reinternType(
    TypeInterner const& src, TypeId srcId, TypeLattice& dstHost,
    std::unordered_map<std::uint32_t, TypeId>& remap);

} // namespace dss
