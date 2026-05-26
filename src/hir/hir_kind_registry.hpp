#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "hir/hir_node.hpp"   // kFirstHirExtensionKind

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Registry of extension HIR-kinds (HR1) — the open-core counterpart to the type
// lattice's `TypeRegistry`. A language/domain registers the HIR kinds its
// lowering emits beyond the universal core (e.g. SQL's `Query`/`DmlInsert`,
// shader's `WorkgroupBarrier`); the core engine, walker, and (later) verifier
// are written against the core enum + this registry, never against a hardcoded
// shader/SQL kind. Extensions are NOMINAL and language/domain-qualified (e.g.
// "SQL::Query"); core kinds occupy the HirKind enum's [0, 256), registered kinds
// are monotonic from kFirstHirExtensionKind (256).

namespace dss {

// A HIR extension-kind as registered: its qualified name, minted kindId, and
// the owning language/domain. Construction is gated by a passkey type only the
// registry can default-construct — preventing fabrication of a descriptor whose
// `kindId` violates the >= 256 invariant or whose `name` isn't language-
// qualified. The passkey idiom (rather than `friend class HirKindRegistry` +
// private ctor) is what lets `std::vector::emplace_back` build the descriptor
// directly inside the registry's storage: allocator::construct goes through
// `std::construct_at`, which can't see friend-only access. The operand/
// attribute shape + consuming-backend set (09-hir-plan §2.2) are additive
// richness layered on in later PRs (HR5/HR6); HR1 needs only stable identity.
class DSS_EXPORT HirKindDescriptor {
public:
    // Passkey: only `HirKindRegistry` can default-construct one, so the ctor
    // below is effectively callable only from within the registry.
    class MintToken {
        friend class HirKindRegistry;
        MintToken() = default;
    };

    HirKindDescriptor(MintToken, std::string name, HirKindId kindId,
                      std::string sourceLanguage) noexcept
        : name_(std::move(name)), kindId_(kindId), sourceLanguage_(std::move(sourceLanguage)) {}

    [[nodiscard]] std::string_view name()           const noexcept { return name_; }
    [[nodiscard]] HirKindId        kindId()         const noexcept { return kindId_; }
    [[nodiscard]] std::string_view sourceLanguage() const noexcept { return sourceLanguage_; }

private:
    std::string name_;
    HirKindId   kindId_;
    std::string sourceLanguage_;
};

class DSS_EXPORT HirKindRegistry {
public:
    HirKindRegistry() = default;

    // Register (or look up) an extension kind by name. Monotonic kindId >= 256.
    // Idempotent on re-declaration with the same owning language — the name is
    // the lookup key, so returning the existing kindId is correct. Re-declaring
    // a name under a DIFFERENT owning language is a cross-domain collision and
    // aborts loud (two domains must not silently share one HIR kind). Aborts on
    // counter exhaustion (uint32 overflow) rather than wrapping to the core
    // range and producing an InvalidHirKind silently.
    HirKindId registerExtension(std::string_view name, std::string_view sourceLanguage = {});

    [[nodiscard]] std::optional<HirKindId> findExtension(std::string_view name) const;

    // The descriptor for a previously-minted kindId. Release-fatal on a core-range
    // (< 256) or never-minted id — a caller asking for a kind this registry never
    // produced is a logic error, not a recoverable miss.
    [[nodiscard]] HirKindDescriptor const& descriptor(HirKindId kind) const;

    [[nodiscard]] std::span<HirKindDescriptor const> extensions() const noexcept {
        return extensions_;
    }

private:
    std::vector<HirKindDescriptor>            extensions_;   // index = kindId.v - 256
    std::unordered_map<std::string, HirKindId> byName_;
    std::uint32_t                              nextKind_ = kFirstHirExtensionKind;
};

} // namespace dss
