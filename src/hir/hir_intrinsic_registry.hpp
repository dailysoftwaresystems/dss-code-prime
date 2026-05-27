#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Registry of HIR intrinsics (HR6) — the resolution table for `IntrinsicCall`
// nodes, whose `payload` carries a `HirIntrinsicId`. An intrinsic is a built-in
// operation a language's lowering emits (e.g. a math `sqrt`, a shader
// `textureSample`) that is neither a core arithmetic operator (`HirOpKind`) nor a
// first-class structured node. Unlike `HirKindRegistry`/`HirOpRegistry` there is
// NO universal core intrinsic set — no intrinsic is shared by every language — so
// ids run monotonically from 1 (0 == `InvalidHirIntrinsic`), with no [0,256)
// core range. Lives in the frozen `Hir` (like the kind/op registries) so an
// `IntrinsicCall` payload is resolvable by every downstream consumer (verifier,
// text format, MIR lowering); the verifier's `checkIntrinsicCalls` rule rejects
// any `IntrinsicCall` whose id this registry never minted (`H_UnknownIntrinsic`).

namespace dss {

// A registered intrinsic: its qualified name, minted id, and owning language/
// domain. Construction is passkey-gated so only the registry can build one with
// a well-formed id — the same discipline as `HirKindDescriptor`/`HirOpDescriptor`
// (the passkey, not `friend`, lets `std::vector::emplace_back` construct it in
// place via `std::construct_at`).
class DSS_EXPORT HirIntrinsicDescriptor {
public:
    class MintToken {
        friend class HirIntrinsicRegistry;
        MintToken() = default;
    };

    HirIntrinsicDescriptor(MintToken, std::string name, HirIntrinsicId id,
                           std::string sourceLanguage) noexcept
        : name_(std::move(name)), id_(id), sourceLanguage_(std::move(sourceLanguage)) {}

    [[nodiscard]] std::string_view name()           const noexcept { return name_; }
    [[nodiscard]] HirIntrinsicId   id()             const noexcept { return id_; }
    [[nodiscard]] std::string_view sourceLanguage() const noexcept { return sourceLanguage_; }

private:
    std::string    name_;
    HirIntrinsicId id_;
    std::string    sourceLanguage_;
};

class DSS_EXPORT HirIntrinsicRegistry {
public:
    HirIntrinsicRegistry() = default;

    // Register (or look up) an intrinsic by name. Monotonic id from 1. Idempotent
    // on re-declaration with the same owning language — the name is the lookup
    // key, so returning the existing id is correct. Re-declaring a name under a
    // DIFFERENT owning language is a cross-domain collision and aborts loud (two
    // domains must not silently share one intrinsic). Aborts on counter
    // exhaustion (uint32 overflow) rather than wrapping to 0 (== InvalidHirIntrinsic).
    HirIntrinsicId registerIntrinsic(std::string_view name, std::string_view sourceLanguage = {});

    [[nodiscard]] std::optional<HirIntrinsicId> findIntrinsic(std::string_view name) const;

    // Non-fatal membership test — the verifier asks "did this registry mint `id`?"
    // and must get a bool, not an abort, for an unregistered/zero id (that is a
    // recoverable `H_UnknownIntrinsic` diagnostic, not a logic error).
    [[nodiscard]] bool contains(HirIntrinsicId id) const noexcept;

    // The descriptor for a previously-minted id. Release-fatal on an invalid or
    // never-minted id — a caller asking for an intrinsic this registry never
    // produced is a logic error. Guard with `contains()` when absence is expected.
    [[nodiscard]] HirIntrinsicDescriptor const& descriptor(HirIntrinsicId id) const;

    [[nodiscard]] std::span<HirIntrinsicDescriptor const> intrinsics() const noexcept {
        return intrinsics_;
    }

private:
    std::vector<HirIntrinsicDescriptor>            intrinsics_;   // index = id.v - 1
    std::unordered_map<std::string, HirIntrinsicId> byName_;
    std::uint32_t                                  nextId_ = 1;   // 0 == InvalidHirIntrinsic
};

} // namespace dss
