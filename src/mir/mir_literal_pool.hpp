#pragma once

#include "core/export.hpp"
#include "core/types/bit_int_value.hpp"            // BitIntValue (C23 _BitInt const-fold arm)
#include "core/types/wide_float_value.hpp"         // WideFloatValue (LD-3 F80/F128 const-fold arm)
#include "core/types/type_lattice/core_type.hpp"   // TypeKind

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// Per-module literal value pool (ML1). A MIR `Const` instruction carries only a
// pool index in its `payload`; the decoded VALUE lives here. This mirrors the
// HIR `HirLiteralPool`: MIR keeps its own copy (a parallel IR owns parallel
// data) so the `mir` library depends only on `core`, exactly like `hir`. HIR→MIR
// lowering (ML2) copies each `HirLiteralValue` into a `MirLiteralValue`; the
// optimizer, when it rebuilds a module, copies the surviving entries (and
// constant-folding appends new ones). The `.dssir` text format (ML4) reads the
// value from here, so a synthetic module with no source still carries it.

namespace dss {

struct MirLiteralValue;

// Aggregate literal: mirror of `HirAggregateValue` on the MIR side.
// Produced by MIR-globals when the HIR const-eval folds an aggregate
// `ConstructAggregate` (D5.3) — module-scope `struct Point p = {1, 2};`
// lands as a const-init rather than degrading to a runtime-init
// `__module_init__` synthesis. The recursive shape mirrors the HIR side.
struct MirAggregateValue {
    std::vector<MirLiteralValue> fields;

    // ★★★ THE TEARDOWN IS PART OF THE WALK, AND IT WAS THE ONE WALK NOBODY
    // WROTE — D-MIR-LITERAL-VALUE-TEARDOWN-RECURSES-PER-AGGREGATE-LEVEL.
    //
    // Every explicit walk over this tree is on a heap work stack
    // (`forEachLiteralNode` below, and the writer/reader in `mir_text.cpp`), but
    // DESTRUCTION is a walk too, and the compiler generated it: `~MirLiteralValue
    // → ~variant → ~MirAggregateValue → ~vector → ~MirLiteralValue`, ONE host
    // frame chain per brace level, uncapped, on whatever thread happens to drop
    // the value. That is exactly the shape the operator's ruling of 2026-09-02
    // forbids, and it is INVISIBLE to every grep for recursion because no
    // function in this repository names itself in it.
    //
    // ⚠ The recursion census records this site as one that "cannot be flattened
    // from the file that owns the walk". True of `hir_to_mir.cpp`; NOT true of
    // the header that owns the TYPE, which is here — and fixing it here fixes it
    // for every consumer at once rather than per call site.
    //
    // The rule of five is spelled out because a user-provided destructor
    // SUPPRESSES the implicit move operations, and falling back to copies would
    // replace a deep destructor with a deep COPY — the same defect, slower.
    MirAggregateValue();
    MirAggregateValue(MirAggregateValue const&);
    MirAggregateValue(MirAggregateValue&&) noexcept;
    MirAggregateValue& operator=(MirAggregateValue const&);
    MirAggregateValue& operator=(MirAggregateValue&&) noexcept;
    ~MirAggregateValue();
};

// Symbol-address literal (F5 — D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): a global whose
// initializer is the LINK-TIME-CONSTANT address of another symbol — a string
// literal's rodata global (`char* g = "..."`), another global (`int* p = &x`),
// or a function (a function-pointer table). It is NOT a runtime initializer: the
// assembler emits a pointer-width slot + an absolute-64 RELOCATION against
// `symbol` (+ `addend`), and the linker fills the target's VA at link time.
// `symbol` is the target's `SymbolId` underlying value, kept raw so the pool
// stays core-only (the assembler reconstructs `SymbolId{symbol}`).
struct MirSymbolAddrValue {
    std::uint32_t symbol = 0;
    std::int64_t  addend = 0;
};

struct MirLiteralValue {
    // `monostate` = a literal whose value is unknown (carried so a malformed
    // source still lowers + diagnoses). `core` is a denormalized hint enabling
    // pool-level inspection without consulting the interner; the Const
    // instruction's typeId is the authority — disambiguate char vs string by the
    // VARIANT ARM (uint64 vs string), never by `core`. D5.3 adds the
    // `MirAggregateValue` arm for `core` ∈ {Struct, Union, Array}. C4b adds the
    // `BitIntValue` arm (`core == BitInt`) — the SAME host bit-precise value type
    // the HIR pool carries (D-CSUBSET-BITINT-WIDE-LITERAL): a narrow literal's
    // container value + a wide literal's limbs both flow through it; the globals
    // byte-emitter fails loud on it (wide `_BitInt` data-globals are deferred).
    // LD-3 adds the `WideFloatValue` arm (`core` ∈ {F80, F128}) — the SAME host
    // wide-float value type the HIR pool carries (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION):
    // a FOLDED F80/F128 arithmetic result at TRUE 80/128-bit
    // precision. The globals byte-emitter encodes it via `appendWideFloatBits`
    // (its `get_if<WideFloatValue>` branch, checked BEFORE the `double` arm); an
    // UNFOLDED F80/F128 leaf still rides the pre-existing `double` arm's dedicated
    // `appendF80Extended`/`appendF128` widen path.
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string,
                 MirAggregateValue, MirSymbolAddrValue, BitIntValue, WideFloatValue> value;
    TypeKind core = TypeKind::Void;
};

// ── MirAggregateValue's SPECIAL MEMBERS, out of line because they need
//    `MirLiteralValue` COMPLETE ─────────────────────────────────────────────
// Four of the five are the compiler's own; only the destructor differs, and it
// differs only in HOW it reaches the nodes, never in what it destroys.
inline MirAggregateValue::MirAggregateValue()                                   = default;
inline MirAggregateValue::MirAggregateValue(MirAggregateValue const&)           = default;
inline MirAggregateValue::MirAggregateValue(MirAggregateValue&&) noexcept       = default;
inline MirAggregateValue& MirAggregateValue::operator=(MirAggregateValue const&) = default;
inline MirAggregateValue&
MirAggregateValue::operator=(MirAggregateValue&&) noexcept                       = default;

// Destroy the whole subtree with an explicit heap work list: lift each level's
// children OUT before the level is dropped, so every `MirLiteralValue` that
// actually runs its destructor holds an EMPTY aggregate and costs O(1) frames.
// The nested `~MirAggregateValue` those empty values run re-enters this body
// exactly once and returns immediately — bounded depth 2, not depth N.
inline MirAggregateValue::~MirAggregateValue() {
    if (fields.empty()) return;   // the common case, and the recursion's base
    std::vector<MirLiteralValue> pending;
    pending.swap(fields);
    while (!pending.empty()) {
        MirLiteralValue cur = std::move(pending.back());
        pending.pop_back();
        if (auto* agg = std::get_if<MirAggregateValue>(&cur.value)) {
            for (auto& f : agg->fields) pending.push_back(std::move(f));
            // Dropping the moved-from elements here costs nothing: a moved-from
            // `MirLiteralValue` holding an aggregate holds a moved-from (empty)
            // vector, so its destructor takes the `fields.empty()` early return.
            agg->fields.clear();
        }
        // `cur` dies here owning at most an EMPTY aggregate.
    }
}

// ── ONE OWNER FOR "VISIT EVERY NODE OF A LITERAL", ON THE HEAP ──────────────
//
// ★★★ D-MIR-NESTED-AGGREGATE-LITERAL-WALKS-RECURSE-PER-INITIALIZER-LEVEL
//
// `MirAggregateValue::fields` is a `MirLiteralValue` vector, so a literal is a
// TREE whose depth is the brace nesting of the user's initializer. NINE separate
// walks over that tree existed across `src/mir/**`, every one of them written as
// host recursion and every one of them with NO CAP OF ANY KIND — the writer, the
// merge's symbol remap and symbol assignment, the summary's global-init scan, the
// lazy-import name collection and undefined-symbol note, the HIR→MIR literal copy
// and its TLS screen, and the all-zero test. Each was a latent stack overflow on
// a legal, deeply-braced initializer, and being nine copies meant nine chances to
// miss one.
//
// ⇒ ONE walker, an explicit `std::vector` work stack, O(1) host frames per brace
// level. It is a template on the visitor and const-agnostic on the literal so the
// mutating remap and the read-only scans share the SAME traversal — the operator's
// standing ruling of 2026-09-02, *"it's well known to not use recursive structures
// in the compiler because big projects like sqlite will for sure explode the
// stack"*.
//
// ⚠ THE VISITOR SEES **EVERY** NODE, AGGREGATES INCLUDED, and the aggregate's
// children are walked whatever the visitor does. That is deliberate: a visitor
// that used to `return` early after handling a symbol-address arm still behaves
// identically, because a `MirSymbolAddrValue` node has no children — but a
// visitor that wanted to prune a subtree would have to say so, and none does.
// ★ Children are pushed in REVERSE so they pop in field order; the merge's symbol
// ASSIGNMENT order and the summary's `targets` vector are both order-observable.
template <typename Lit, typename Visit>
void forEachLiteralNode(Lit& root, Visit&& visit) {
    std::vector<Lit*> work;
    work.push_back(&root);
    while (!work.empty()) {
        Lit* const cur = work.back();
        work.pop_back();
        visit(*cur);
        if (auto* agg = std::get_if<MirAggregateValue>(&cur->value)) {
            auto& fields = agg->fields;
            for (std::size_t i = fields.size(); i-- > 0;) {
                work.push_back(&fields[i]);
            }
        }
    }
}

class DSS_EXPORT MirLiteralPool {
public:
    // Append a literal value; returns its index (the Const instruction payload).
    // No dedup — every occurrence gets its own slot (dedup is an optimizer
    // concern; keeps add O(1)).
    [[nodiscard]] std::uint32_t add(MirLiteralValue v);

    [[nodiscard]] MirLiteralValue const& at(std::uint32_t index) const;
    [[nodiscard]] std::size_t            size()  const noexcept { return pool_.size(); }
    [[nodiscard]] bool                   empty() const noexcept { return pool_.empty(); }

private:
    std::vector<MirLiteralValue> pool_;
};

} // namespace dss
