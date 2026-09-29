#include "core/types/type_lattice/composite_definition.hpp"

#include "core/types/alignment.hpp"

#include <format>
#include <span>
#include <type_traits>

namespace dss {

// ★★ THE CHANNEL LIST IS PINNED TO THE INTERNER'S, AT COMPILE TIME. A new layout channel on
// `completeComposite` changes this signature and stops the build HERE — so `describeComposite`,
// `refuseCompositeDefinition`, `completeCompositeDefinition` and both tiers' spellings grow it together,
// instead of a text silently dropping a channel it never heard of while its round trip stays byte-identical.
static_assert(std::is_same_v<decltype(&TypeInterner::completeComposite),
                             void (TypeInterner::*)(TypeId, std::span<TypeId const>, bool,
                                                    std::span<std::int64_t const>,
                                                    std::span<std::uint64_t const>,
                                                    std::span<std::uint32_t const>, std::uint32_t,
                                                    std::uint32_t, std::span<std::uint8_t const>)>,
              "TypeInterner::completeComposite gained or lost a layout channel — give it a place in "
              "CompositeDefinition, describeComposite, refuseCompositeDefinition and "
              "completeCompositeDefinition, and a spelling in every text tier's `types` table, then "
              "update this signature");

CompositeDefinition describeComposite(TypeInterner const& in, TypeId composite) {
    CompositeDefinition def;
    def.kind = in.kind(composite);
    def.name = std::string{in.name(composite)};
    if (in.isIncompleteComposite(composite)) {
        def.opaque = true;
        return def;
    }
    def.packed        = in.isPacked(composite);
    def.explicitAlign = in.explicitCompositeAlign(composite);
    def.maxFieldAlign = in.maxFieldAlign(composite);
    bool const hasOffsets = in.hasExplicitOffsets(composite);
    bool const hasAligns  = in.hasExplicitAligns(composite);
    bool const hasPacked  = in.hasFieldPacked(composite);
    auto const ops = in.operands(composite);
    std::vector<TypeId> const types(ops.begin(), ops.end());   // a copy: no pool view outlives this line
    def.fields.reserve(types.size());
    for (std::size_t i = 0; i < types.size(); ++i) {
        CompositeFieldDefinition f;
        f.type = types[i];
        if (hasOffsets) {
            auto const off = in.explicitFieldOffset(composite, i);
            f.offset = off ? *off : 0;
        } else if (hasAligns) {
            f.align = in.explicitFieldAlign(composite, i);
        }
        f.bitWidth = in.fieldBitWidth(composite, i);
        f.packed   = hasPacked && in.isFieldPacked(composite, i);
        def.fields.push_back(f);
    }
    return def;
}

std::optional<std::string> refuseCompositeDefinition(CompositeDefinition const& def) {
    if (def.kind != TypeKind::Struct && def.kind != TypeKind::Union) {
        return std::string{"a `types` entry defines a `struct` or a `union` — nothing else is a composite "
                           "in this format"};
    }
    char const* const what = def.kind == TypeKind::Struct ? "struct" : "union";
    if (def.opaque) return std::nullopt;
    // `completeComposite` ABORTS on an alignment the Alignment domain cannot represent, which is right for a
    // compiler pass and wrong for a reader of text it did not write. 0 is the writer's ABSENT value for the
    // two whole-composite markers (it omits them), and a field's "no override".
    for (auto const [marker, bytes] : {std::pair{"aligned", def.explicitAlign},
                                       std::pair{"pack", def.maxFieldAlign}}) {
        if (bytes != 0 && !Alignment::fromBytes(bytes).has_value()) {
            return std::format("`{} {}` is not a composite alignment this build can represent — it must be a "
                               "power of two within the Alignment domain (the writer omits the marker when "
                               "there is none)", marker, bytes);
        }
    }
    std::size_t const n = def.fields.size();
    std::size_t withOffset = 0, withAlign = 0, withWidth = 0, withPacked = 0;
    for (CompositeFieldDefinition const& f : def.fields) {
        withOffset += f.offset.has_value() ? 1u : 0u;
        withAlign  += f.align.has_value() ? 1u : 0u;
        withWidth  += f.bitWidth.has_value() ? 1u : 0u;
        withPacked += f.packed ? 1u : 0u;
        if (f.align.has_value() && *f.align != 0 && !Alignment::fromBytes(*f.align).has_value()) {
            return std::format("member alignment {} is not an alignment this build can represent — a power "
                               "of two from 1 to {}, or 0 for a field with no override",
                               *f.align, Alignment::kMaxBytes);
        }
    }
    if (withOffset != 0 && withAlign != 0) {
        return std::format("{} fields cannot mix explicit offsets (@) and member aligns (~)", what);
    }
    if (withOffset != 0 && withOffset != n) {
        return std::format("{} field offsets must be all-or-none", what);
    }
    if (withAlign != 0 && withAlign != n) {
        return std::format("{} member aligns must be all-or-none", what);
    }
    if (withOffset != 0 && (def.packed || withPacked != 0)) {
        return std::format("a {} carrying explicit offsets (@) cannot also be packed — offsets place its fields "
                           "wholesale", what);
    }
    // Not an interner abort but a layout REFUSAL (`computeLayout` declines the pair), and no compiler path
    // produces it — so it is text no writer of this definition can have written.
    if (withOffset != 0 && withWidth != 0) {
        return std::format("a {} carrying explicit offsets (@) cannot also carry bit-field widths — offsets "
                           "place its fields wholesale", what);
    }
    return std::nullopt;
}

void completeCompositeDefinition(TypeInterner& in, TypeId forward, CompositeDefinition const& def) {
    if (def.opaque) return;   // INCOMPLETE: stays forward-only
    std::size_t const n = def.fields.size();
    std::vector<TypeId>        types;
    std::vector<std::int64_t>  widths;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint32_t> aligns;
    std::vector<std::uint8_t>  packed;
    types.reserve(n);
    bool anyWidth = false, anyOffset = false, anyAlign = false, anyPacked = false;
    for (CompositeFieldDefinition const& f : def.fields) {
        types.push_back(f.type);
        widths.push_back(f.bitWidth.has_value() ? static_cast<std::int64_t>(*f.bitWidth) : kNotBitfield);
        offsets.push_back(f.offset.value_or(0));
        aligns.push_back(f.align.value_or(0));
        packed.push_back(f.packed ? 1u : 0u);
        anyWidth  = anyWidth || f.bitWidth.has_value();
        anyOffset = anyOffset || f.offset.has_value();
        anyAlign  = anyAlign || f.align.has_value();
        anyPacked = anyPacked || f.packed;
    }
    // An EMPTY span is each channel's "absent" — the same encoding `completeComposite`'s every caller uses, so
    // a composite without the channel keeps the TypeId content it has always had.
    in.completeComposite(forward, types, def.packed,
                         anyWidth ? std::span<std::int64_t const>{widths} : std::span<std::int64_t const>{},
                         anyOffset ? std::span<std::uint64_t const>{offsets}
                                   : std::span<std::uint64_t const>{},
                         anyAlign ? std::span<std::uint32_t const>{aligns} : std::span<std::uint32_t const>{},
                         def.explicitAlign, def.maxFieldAlign,
                         anyPacked ? std::span<std::uint8_t const>{packed} : std::span<std::uint8_t const>{});
}

} // namespace dss
