#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "hir/hir_op.hpp"   // kFirstHirExtensionOp, HirOpArity

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Registry of extension HIR operators (HR2) — the operator analog of
// `HirKindRegistry`, and the same open-core counterpart the type lattice's
// `TypeRegistry` is. A language/domain registers an operator its lowering emits
// beyond the universal core (the rare genuinely novel operator); the core
// engine and verifier are written against the `HirOpKind` core enum + this
// registry, never a hardcoded extension operator. Extensions are NOMINAL and
// language/domain-qualified (e.g. "APL::Rotate"); core operators occupy
// `HirOpKind`'s [0, 256), registered operators are monotonic from
// kFirstHirExtensionOp (256).

namespace dss {

// A HIR extension-operator as registered: its qualified name, minted opId, its
// arity, and the owning language/domain. Construction is gated by a passkey type
// only the registry can default-construct — preventing any code outside the
// registry's minting path from fabricating a descriptor whose `opId` violates
// the >= 256 invariant. (The "::"-qualified `name` is a naming convention, not
// something the passkey enforces.) The passkey idiom (rather than `friend` +
// private ctor) is
// what lets `std::vector::emplace_back` build the descriptor in place:
// allocator::construct goes through `std::construct_at`, which can't see
// friend-only access. (Identical discipline to `HirKindDescriptor`.)
class DSS_EXPORT HirOpDescriptor {
public:
    class MintToken {
        friend class HirOpRegistry;
        MintToken() = default;
    };

    HirOpDescriptor(MintToken, std::string name, HirOpId opId, HirOpArity arity,
                    std::string sourceLanguage) noexcept
        : name_(std::move(name)), opId_(opId), arity_(arity),
          sourceLanguage_(std::move(sourceLanguage)) {}

    [[nodiscard]] std::string_view name()           const noexcept { return name_; }
    [[nodiscard]] HirOpId          opId()           const noexcept { return opId_; }
    [[nodiscard]] HirOpArity       arity()          const noexcept { return arity_; }
    [[nodiscard]] std::string_view sourceLanguage() const noexcept { return sourceLanguage_; }

private:
    std::string name_;
    HirOpId     opId_;
    HirOpArity  arity_;
    std::string sourceLanguage_;
};

class DSS_EXPORT HirOpRegistry {
public:
    HirOpRegistry() = default;

    // Register (or look up) an extension operator by name. Monotonic opId >= 256.
    // Idempotent on re-declaration with the same owning language AND arity — the
    // name is the lookup key, so returning the existing opId is correct. Re-
    // declaring a name under a DIFFERENT owning language OR a different arity is
    // a collision and aborts loud (two domains must not silently share one
    // operator, and an operator's arity is part of its identity). Aborts on
    // counter exhaustion (uint32 overflow) rather than wrapping to the core
    // range and producing an InvalidHirOp silently.
    HirOpId registerExtension(std::string_view name, HirOpArity arity,
                              std::string_view sourceLanguage = {});

    [[nodiscard]] std::optional<HirOpId> findExtension(std::string_view name) const;

    // The descriptor for a previously-minted opId. Release-fatal on a core-range
    // (< 256) or never-minted id — a caller asking for an operator this registry
    // never produced is a logic error, not a recoverable miss.
    [[nodiscard]] HirOpDescriptor const& descriptor(HirOpId op) const;

    [[nodiscard]] std::span<HirOpDescriptor const> extensions() const noexcept {
        return extensions_;
    }

private:
    std::vector<HirOpDescriptor>             extensions_;   // index = opId.v - 256
    std::unordered_map<std::string, HirOpId> byName_;
    std::uint32_t                            nextOp_ = kFirstHirExtensionOp;
};

} // namespace dss
