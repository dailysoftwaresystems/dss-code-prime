// ─────────────────────────────────────────────────────────────────────────
// THE TABLE. The one place in the tree that names every object format.
// D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125).
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ THIS FILE IS THE POINT OF THE DESIGN, so read what it is NOT.
//
// It is not a lookup table keyed on `ObjectFormatKind`. That distinction is
// the whole ruling, and this tree has twice argued the other way in prose —
// once defending `kVehicleKinds` as *"expressed as a TABLE … not an if-chain"*
// and once defending the same shape as *"a config-coherence rule, not an
// engine branch"*. Both comments were struck in TF-C125. Replacing N
// `if (kind == X)` with one array indexed by `X` moves the identity dependency
// into a data structure; it does not remove it, because every consumer still
// has to KNOW an `X` to get an answer out.
//
// What this array is instead: an unordered, unkeyed LIST of implementations,
// handed to the substrate as a `span`. Its consumers iterate it. The generic
// resolver (`objectFormatBackendByConfigName`, in the substrate) walks it
// asking each entry "is this your name?" — a question a backend answers about
// ITSELF. The loader's cross-block guard unions every entry's declared blocks.
// The stack-reserve rule asks every entry whether it implements a vehicle.
// None of them index. Reorder these five lines and nothing changes.
//
// ⇒ ADDING A SIXTH FORMAT: write `<name>_backend.cpp`, declare its accessor in
// `object_format_backends.hpp`, add one line here, add the TU to
// `src/link/CMakeLists.txt`. Zero edits to shared substrate. That is the test
// of whether the seam is real, and it is the property the 25 comparisons, the
// Elf-defaulted `kind` field, and the 2 kind-keyed tables this refactor deleted
// did not have.

#include "link/format/object_format_backends.hpp"

#include <array>
#include <span>

namespace dss::link {

std::span<ObjectFormatBackend const* const>
objectFormatBackendTable() noexcept {
    // Function-local static: the accessors below are themselves function-local
    // statics in their own TUs, so taking their addresses here is ordered by
    // first call, never by static-initialization order across TUs.
    static std::array<ObjectFormatBackend const*, 5> const kTable{{
        &format::elfBackend(),
        &format::peBackend(),
        &format::machoBackend(),
        &format::wasmBackend(),
        &format::spirvBackend(),
    }};
    return {kTable.data(), kTable.size()};
}

} // namespace dss::link
