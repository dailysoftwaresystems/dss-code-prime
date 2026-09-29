#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ── WHAT A COMPOSITE DEFINITION CARRIES — ONE OWNER FOR EVERY TEXT TIER ─────────────────────────────────────
//
// A text tier that spells a struct or union ONCE, by handle, in a `types` table — the HIR `.dsshir` (v5) and
// the MIR `.dssir` (v2) — writes a composite's DEFINITION and a reader completes a forward-minted composite
// from it. What that definition carries is not a property of either tier: it is exactly the list of layout
// channels `TypeInterner::completeComposite` takes, and a channel the text drops is a layout the reader
// rebuilds DIFFERENTLY while the round trip stays byte-identical (the re-emission spells the stripped type
// exactly as the first one did) — so the loss is invisible to every check but a layout comparison. HIR v4
// lost four channels that way, and the MIR text, spelling composites inline with their field TYPES only, lost
// all of them.
//
// So the knowledge lives HERE, once, for both tiers:
//   * `describeComposite` — every channel, read off the interner, for a writer;
//   * `refuseCompositeDefinition` — every combination `completeComposite` would ABORT on (fatal is right for a
//     compiler pass and wrong for a reader of text it did not write) or `computeLayout` would decline, named;
//   * `completeCompositeDefinition` — the completion itself;
// and the `static_assert` in the .cpp pins them to `completeComposite`'s signature, so a new channel stops the
// build HERE instead of being silently dropped by one tier. Each tier keeps its own TOKENS around this — the
// spelling is the same by rule (`<kind> "<name>" (opaque | [packed] [aligned N] [pack N] { <type> [@N | ~N]
// [bits N] [packed], … })`), and each tier's type syntax renders a field's type.

namespace dss {

// One field of a definition. The `@N` offset and the `~N` member alignment are ALL-OR-NONE channels over the
// whole field list (the interner takes each as one span); a definition that sets them on some fields only is
// refused, never padded.
struct CompositeFieldDefinition {
    TypeId                       type{};
    std::optional<std::uint64_t> offset;     // `@N` — an explicit byte offset
    std::optional<std::uint32_t> align;      // `~N` — a member alignment; 0 = this field has no override
    std::optional<std::uint32_t> bitWidth;   // `bits N` — a bit-field's declared width, `bits 0` included
    bool                         packed = false;   // per-member `packed`
};

struct CompositeDefinition {
    TypeKind                              kind = TypeKind::Struct;   // Struct or Union — nothing else is one
    std::string                           name;
    bool                                  opaque = false;            // INCOMPLETE: no field list at all
    bool                                  packed = false;            // the whole-composite flag
    std::uint32_t                         explicitAlign = 0;         // `aligned N`; 0 = none
    std::uint32_t                         maxFieldAlign = 0;         // `pack N` (the `#pragma pack` cap); 0 = none
    std::vector<CompositeFieldDefinition> fields;
};

// Every channel of the MATERIAL struct or union `composite` (a qualifier skin must be stripped by the caller —
// a skin is a use, not a definition). An incomplete composite answers `opaque` and no fields.
[[nodiscard]] DSS_EXPORT CompositeDefinition describeComposite(TypeInterner const& in, TypeId composite);

// The sentence refusing `def`, or nullopt when `completeCompositeDefinition` may complete it. Refuses: a kind
// that is not Struct/Union; an `aligned`/`pack` or member alignment the Alignment domain cannot represent; a
// partial `@N` or `~N` channel; `@N` mixed with `~N`, with `packed` (whole or per-member), or with bit-field
// widths. The caller names WHICH entry it refused (its handle is a tier's token, not a definition's).
[[nodiscard]] DSS_EXPORT std::optional<std::string> refuseCompositeDefinition(CompositeDefinition const& def);

// Complete the forward-minted `forward` (from `TypeInterner::forwardComposite`) from `def`. Precondition:
// `refuseCompositeDefinition(def, …)` returned nullopt — an opaque definition leaves `forward` incomplete.
DSS_EXPORT void completeCompositeDefinition(TypeInterner& in, TypeId forward, CompositeDefinition const& def);

} // namespace dss
