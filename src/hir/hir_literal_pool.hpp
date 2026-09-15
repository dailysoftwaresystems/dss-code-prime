#pragma once

#include "core/export.hpp"
#include "core/types/bit_int_value.hpp"            // BitIntValue (C23 _BitInt const-fold arm)
#include "core/types/wide_float_value.hpp"         // WideFloatValue (LD-3 F80/F128 const-fold arm)
#include "core/types/type_lattice/core_type.hpp"   // TypeKind
#include "core/types/strong_ids.hpp"               // TypeId (HirAddressValue.pointeeType)

#include <cstdint>
#include <string>
#include <utility>   // std::pair — the iterative copy's (source, destination) work list
#include <variant>
#include <vector>

// Per-CU literal value pool (plan 09 HR8). HIR `Literal` nodes carry only a
// `literalIndex` (a per-occurrence ordinal); the decoded VALUE lives here,
// indexed by that ordinal. The value is decoded ONCE at lowering time (from the
// CST token text, honoring the language's `numberStyle`) and is first-class IR
// data — NOT recovered from a source span, so synthetic HIR (transpile /
// constant-fold, which has no CST) can still carry literal values. This is the
// store HR7's reserved inline-`.dsshir`-value syntax and MIR/codegen read from.
//
// The variant covers the c literal surface (bool / signed / unsigned /
// floating / string). A char literal is stored as its decoded codepoint in the
// `uint64_t` arm; a string literal's decoded bytes (escapes resolved, NOT
// NUL-terminated — the NUL is implied by the Array<Char,N+1> type) live in the
// `std::string` arm. Both carry `core = Char`, so disambiguate char vs string
// by the VARIANT ARM (`uint64_t` vs `std::string`) — NOT by `core` (which is
// redundant pool-level metadata mirroring the node's `typeId`; the node's type,
// Char vs Array<Char,N+1>, is the real authority). 128-bit integers remain
// additive when a language needs them.

namespace dss {

struct HirLiteralValue;

// Aggregate literal: a struct / union / array constant value, recursively
// composed of field/element `HirLiteralValue`s in positional declaration
// order. Produced by the const-eval engine (plan 12.5) when folding a
// `HirKind::ConstructAggregate` whose every child folds to a constant.
// MIR-globals reads this to materialize an aggregate `constInit` (D5.3
// closes the prior "aggregate globals always route to runtime-init"
// gap). The recursive shape mirrors the HIR `ConstructAggregate` tree
// exactly — nested aggregates are nested `HirAggregateValue`s.
//
// LWG 2596 makes `std::vector<HirLiteralValue>` legal even though
// `HirLiteralValue` is incomplete at the wrapper-struct point: vector
// accepts incomplete element types so long as the element type is
// complete before any vector member is instantiated, which it is by
// the time anything constructs or reads an aggregate value.
struct HirAggregateValue {
    std::vector<HirLiteralValue> fields;

    // ★★★ THE TEARDOWN IS PART OF THE WALK, AND IT WAS THE ONE WALK NOBODY
    // WROTE — D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED
    // (the HIR twin of D-MIR-LITERAL-VALUE-TEARDOWN-RECURSES-PER-AGGREGATE-LEVEL).
    //
    // Every explicit walk over this tree runs on a heap work stack, but
    // DESTRUCTION is a walk too, and the compiler generated it:
    // `~HirLiteralValue → ~variant → ~HirAggregateValue → ~vector →
    // ~HirLiteralValue`, ONE host frame chain per brace level, uncapped, on
    // whatever thread happens to drop the value. ✔MEASURED 2026-09-04 (P60,
    // lane `rc`): under MSVC 19.51 Debug that chain is NINETEEN frames of about
    // 60 bytes each — ~1160 bytes per aggregate level — and
    // `LayoutLeverage.NestedStructGlobalInitLowersOnAnOrdinaryThread` (a
    // 1000-level global initializer) died of stack overflow INSIDE THIS
    // DESTRUCTOR at ~878 levels, gdb-attributed frame by frame; under mingw-w64
    // g++ 13.2 the same value is torn down between 1000 and 4000 levels. The
    // fix is the one `mir/mir_literal_pool.hpp` already carries, transferred
    // verbatim, because it belongs to the header that owns the TYPE: fixing it
    // here fixes it for every consumer at once.
    //
    // The rule of five is spelled out because a user-provided destructor
    // SUPPRESSES the implicit move operations, and falling back to copies would
    // replace a deep destructor with a deep COPY — the same defect, slower.
    HirAggregateValue();
    HirAggregateValue(HirAggregateValue const&);
    HirAggregateValue(HirAggregateValue&&) noexcept;
    HirAggregateValue& operator=(HirAggregateValue const&);
    HirAggregateValue& operator=(HirAggregateValue&&) noexcept;
    ~HirAggregateValue();
};

// Address constant (plan p19 c43 — D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A).
// A compile/link-time constant address = a base symbol (or NULL) plus a byte
// offset. Produced by the const-eval engines for: a null-relative offsetof chain
// (`&((T*)0)->M` → base=kNullBase, byteOffset=offsetof), `&global`/`&fn`
// (base=symbol, byteOffset=0), `&global + N` / `&arr[i]` (base=symbol,
// byteOffset=N*stride), and string-literal addresses. `base == kNullBase` (0)
// means a PURE compile-time integer offset — foldable to an int by the final
// `- (char*)0` / cast-to-int; a non-null base is a RELOCATION only the
// MIR-globals emitter materializes (cast-to-int of a symbol-based address fails
// loud — it is not an integer constant). `pointeeType` is FOLD-TRANSIENT metadata
// (the current pointer's pointee, so member/index arms can size strides) — it is
// NOT serialized and is invalid on a deserialized value (a pooled address value
// is only consumed by `toMirLiteral`, which drops it). Mirrors MirSymbolAddrValue
// {symbol,addend} so `toMirLiteral` maps {base,byteOffset} by a field copy.
struct HirAddressValue {
    static constexpr std::uint32_t kNullBase = 0;
    std::uint32_t base       = kNullBase;
    std::int64_t  byteOffset = 0;
    TypeId        pointeeType{};   // fold-transient; not serialized (invalid when parsed)
};

struct HirLiteralValue {
    // `monostate` = a literal whose value could not be decoded (a malformed
    // token); the lowering still emits the node + a diagnostic so analysis
    // continues. `core` records the decoded TypeKind for pool-level inspection
    // without consulting the interner (redundant with the node's typeId).
    //
    // Variant-arm contract by `core`:
    //   - `core == Bool`: held in the `std::int64_t` arm with value 0 or 1.
    //     The native `bool` arm is reserved for source-decoded `true`/`false`
    //     tokens at lowering time AND for round-tripped `.dsshir` text;
    //     anything that flows through the constants-evaluation engine
    //     normalizes to `int64_t` 0/1 so comparison results and integer
    //     values share one arithmetic representation. Consumers reading
    //     bool values MUST handle both arms (use `asInt64` in `const_eval`).
    //   - `core` ∈ signed integer kinds (I8..I64, Char with codepoint
    //     semantics): held in `std::int64_t`.
    //   - `core` ∈ unsigned integer kinds (U8..U64, Byte): held in
    //     `std::uint64_t` at source-decode time; the const-eval engine
    //     may also produce values in `std::int64_t` after arithmetic.
    //     Consumers MUST accept either arm for unsigned cores.
    //   - `core` ∈ float kinds (F16..F64): held in `double`.
    //   - `core` ∈ {F80, F128}: held in EITHER the `double` arm OR the
    //     `WideFloatValue` arm — LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION).
    //     An UNFOLDED F80/F128 literal LEAF stays in the `double` arm (the widen
    //     double→WideFloatValue is always EXACT, 53-bit ⊆ 64/113-bit, so a leaf
    //     need not be promoted). A FOLDED arithmetic RESULT (or an explicit widen
    //     via a cast) is carried in the `WideFloatValue` arm at TRUE 80/128-bit
    //     precision — NEVER the `double` arm (a binary64-rounded value under an
    //     F80/F128 core would silently mis-bind >53-bit-mantissa results). ★
    //     Consumers reading an F80/F128 value MUST check BOTH arms (const-eval's
    //     `toWideFloatOperand` + asm.cpp's `get_if<WideFloatValue>`-before-`double`
    //     branch); the new arm fails loud by construction on any un-updated
    //     consumer (`get_if<double>` → nullptr forces an explicit handle-or-refuse).
    //   - `core == Char` with a STRING literal: held in `std::string`
    //     (the node's typeId is Array<Char,N+1>; disambiguate char-vs-
    //     string by the variant arm, NOT by `core` which is identical
    //     in both cases).
    //   - `core` ∈ {Struct, Union, Array}: held in the `HirAggregateValue`
    //     arm — the recursive `fields` vector carries each element's
    //     own `HirLiteralValue`, positional declaration order, all
    //     elements present (omitted struct fields are zero-filled at
    //     lowering time; HIR's positional discipline holds). D5.3.
    //   - `core == BitInt`: held in the `BitIntValue` arm — a C23 `_BitInt(N)`
    //     bit-precise value (D-CSUBSET-BITINT-WIDE-LITERAL / -CONSTFOLD-LARGE,
    //     C4b). ★ EVERY `_BitInt` literal + const-fold result — narrow (N≤64)
    //     AND wide (N>64) — lives HERE, NEVER the int64 arm (an int64 arm would
    //     fold via the plain wrapping-int64 helpers WITHOUT the mod-2^N wrap = a
    //     silent miscompile; the dedicated arm fails loud by construction on any
    //     un-updated consumer). The narrow-literal MIR lowering extracts the
    //     container value from this arm; the wide path fills limbs from it.
    //   - `core == Void`: held as `std::monostate` (decode failure).
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string,
                 HirAggregateValue, HirAddressValue, BitIntValue, WideFloatValue> value;
    TypeKind core = TypeKind::Void;
};

// ── HirAggregateValue's SPECIAL MEMBERS, out of line because they need
//    `HirLiteralValue` COMPLETE ─────────────────────────────────────────────
// The default constructor and the two MOVES are the compiler's own — a move
// steals the field vector's buffer, O(1) per level. The DESTRUCTOR and the
// two COPIES differ, and each differs only in HOW it reaches the nodes, never
// in what it destroys or what it copies.
inline HirAggregateValue::HirAggregateValue()                                   = default;
inline HirAggregateValue::HirAggregateValue(HirAggregateValue&&) noexcept       = default;
inline HirAggregateValue&
HirAggregateValue::operator=(HirAggregateValue&&) noexcept                       = default;

// ★★★ THE COPY IS A WALK TOO, AND THE DEFAULTED ONE RECURSED EXACTLY AS THE
// DESTRUCTOR DID. `= default` copies the vector, which copy-constructs every
// `HirLiteralValue`, whose variant copy-constructs the nested
// `HirAggregateValue` — one host frame chain per brace level. ✔MEASURED on
// the MIR twin (`mir_literal_pool.hpp`, same cycle): a 1000-level literal's
// copy overflowed a 1 MB stack under MSVC Debug the moment the teardown
// stopped being the first walk to do so. Same explicit work list here: each
// destination level's field vector is reserved to its final size BEFORE any
// pointer into it is taken, so the (source, destination) pairs stay valid
// while their children are still queued.
inline HirAggregateValue::HirAggregateValue(HirAggregateValue const& other) {
    std::vector<std::pair<HirAggregateValue const*, HirAggregateValue*>> pending;
    pending.emplace_back(&other, this);
    while (!pending.empty()) {
        auto const [src, dst] = pending.back();
        pending.pop_back();
        dst->fields.reserve(src->fields.size());   // no reallocation below
        for (HirLiteralValue const& f : src->fields) {
            HirLiteralValue& d = dst->fields.emplace_back();
            d.core = f.core;
            if (auto const* agg = std::get_if<HirAggregateValue>(&f.value)) {
                // An EMPTY aggregate now; its fields are copied when its pair
                // comes off the list — never by this constructor recursing.
                d.value.emplace<HirAggregateValue>();
                pending.emplace_back(agg, &std::get<HirAggregateValue>(d.value));
            } else {
                d.value = f.value;   // a scalar arm: the variant copies a leaf
            }
        }
    }
}

// Copy-and-swap over the iterative copy above and the O(1) move; the value
// being replaced is torn down by the iterative destructor.
inline HirAggregateValue&
HirAggregateValue::operator=(HirAggregateValue const& other) {
    if (this != &other) {
        HirAggregateValue copy{other};
        *this = std::move(copy);
    }
    return *this;
}

// Destroy the whole subtree with an explicit heap work list: lift each level's
// children OUT before the level is dropped, so every `HirLiteralValue` that
// actually runs its destructor holds an EMPTY aggregate and costs O(1) frames.
// The nested `~HirAggregateValue` those empty values run re-enters this body
// exactly once and returns immediately — bounded depth 2, not depth N.
inline HirAggregateValue::~HirAggregateValue() {
    if (fields.empty()) return;   // the common case, and the recursion's base
    std::vector<HirLiteralValue> pending;
    pending.swap(fields);
    while (!pending.empty()) {
        HirLiteralValue cur = std::move(pending.back());
        pending.pop_back();
        if (auto* agg = std::get_if<HirAggregateValue>(&cur.value)) {
            for (auto& f : agg->fields) pending.push_back(std::move(f));
            // Dropping the moved-from elements here costs nothing: a moved-from
            // `HirLiteralValue` holding an aggregate holds a moved-from (empty)
            // vector, so its destructor takes the `fields.empty()` early return.
            agg->fields.clear();
        }
        // `cur` dies here owning at most an EMPTY aggregate.
    }
}

class DSS_EXPORT HirLiteralPool {
public:
    // Append a literal value; returns its index (the `literalIndex` payload of
    // the corresponding HIR `Literal` node). No dedup — every occurrence gets
    // its own slot (dedup is an optimizer concern; keeps add O(1)).
    [[nodiscard]] std::uint32_t add(HirLiteralValue v);

    [[nodiscard]] HirLiteralValue const& at(std::uint32_t index) const;
    [[nodiscard]] std::size_t             size() const noexcept { return pool_.size(); }
    [[nodiscard]] bool                    empty() const noexcept { return pool_.empty(); }

private:
    std::vector<HirLiteralValue> pool_;
};

} // namespace dss
