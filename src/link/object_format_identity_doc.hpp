#pragma once

#include "link/object_format_backend.hpp"

#include <nlohmann/json.hpp>

// Completes the opaque `ObjectFormatIdentityDoc` that `ObjectFormatBackend::
// readIdentity` takes. D-LINK-…-KIND-IDENTITY-BRANCHES (TF-C125).
//
// The whole reason this type exists is stated on the forward declaration in
// `object_format_backend.hpp`: `link/object_format_schema.hpp` is included by
// targets that have no nlohmann include directory at all (MEASURED — the first
// build of this refactor broke `src/mir/merge/synth_threads_shim.cpp` and
// `src/ffi/ingest.cpp` on `nlohmann/json_fwd.hpp: No such file or directory`),
// so the JSON type cannot appear in the substrate header even as a forward
// declaration. Hand-rolling nlohmann's own forward declaration is worse than
// including it: `json` is `basic_json<>` inside an ABI-VERSIONED inline
// namespace, so a hand-written copy rots silently on a library upgrade and the
// symptom is a link error in an unrelated TU.
//
// This header is included by exactly six TUs: the loader, and the five
// backends. Nothing else needs it.
//
// It is a non-owning view — the document outlives the call by construction
// (the loader holds it on the stack across `readIdentity`).

namespace dss::link {

class ObjectFormatIdentityDoc {
public:
    explicit ObjectFormatIdentityDoc(nlohmann::json const& doc) noexcept
        : doc_(doc) {}

    [[nodiscard]] nlohmann::json const& json() const noexcept { return doc_; }

private:
    nlohmann::json const& doc_;
};

} // namespace dss::link
