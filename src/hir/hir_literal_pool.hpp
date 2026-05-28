#pragma once

#include "core/export.hpp"
#include "core/types/type_lattice/core_type.hpp"   // TypeKind

#include <cstdint>
#include <string>
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
// The variant covers the c-subset literal surface (bool / signed / unsigned /
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
    //   - `core` ∈ float kinds (F16..F128): held in `double`.
    //   - `core == Char` with a STRING literal: held in `std::string`
    //     (the node's typeId is Array<Char,N+1>; disambiguate char-vs-
    //     string by the variant arm, NOT by `core` which is identical
    //     in both cases).
    //   - `core` ∈ {Struct, Union, Array}: held in the `HirAggregateValue`
    //     arm — the recursive `fields` vector carries each element's
    //     own `HirLiteralValue`, positional declaration order, all
    //     elements present (omitted struct fields are zero-filled at
    //     lowering time; HIR's positional discipline holds). D5.3.
    //   - `core == Void`: held as `std::monostate` (decode failure).
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string,
                 HirAggregateValue> value;
    TypeKind core = TypeKind::Void;
};

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
