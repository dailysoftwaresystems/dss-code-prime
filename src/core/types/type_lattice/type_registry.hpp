#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Per-CU registry of extension type-kinds (SP2). A language schema's
// `typeExtensions[]` are registered here at CU build time; the semantic /
// transpile phases look them up. Extensions are NOMINAL and language-qualified
// (decision 08.5 §4 Q4): `C++::Boxed<int>` and `C#::Boxed<int>` are distinct
// kinds. Core kinds (the TypeKind enum) occupy [0, 256); registered kinds are
// monotonic from kFirstExtensionKind (256).

namespace dss {

class DSS_EXPORT TypeRegistry {
public:
    explicit TypeRegistry(std::string sourceLanguage = {})
        : sourceLanguage_(std::move(sourceLanguage)) {}

    // Register (or look up) an extension by name. Monotonic kindId >= 256.
    // Idempotent for an IDENTICAL re-declaration (same parameter list) — but
    // re-declaring a name with a DIFFERENT parameter list is a conflict and
    // aborts loud (the within-schema dedup catches it per-document; this guards
    // the cross-schema case of two languages registered into one registry).
    TypeKindId registerExtension(std::string_view name, std::vector<TypeParam> parameters);

    [[nodiscard]] std::optional<TypeKindId> findExtension(std::string_view name) const;

    // The descriptor for a previously-minted kindId (release-fatal on an id this
    // registry never minted).
    [[nodiscard]] ExtensionDescriptor const& descriptor(TypeKindId kind) const;

    [[nodiscard]] std::span<ExtensionDescriptor const> extensions() const noexcept {
        return extensions_;
    }
    [[nodiscard]] std::string_view sourceLanguage() const noexcept { return sourceLanguage_; }

private:
    std::string                                 sourceLanguage_;
    std::vector<ExtensionDescriptor>            extensions_;      // index = kindId.v - 256
    std::unordered_map<std::string, TypeKindId> byName_;
    std::uint32_t                               nextKind_ = kFirstExtensionKind;
};

} // namespace dss
