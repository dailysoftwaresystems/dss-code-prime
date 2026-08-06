#pragma once

#include "core/export.hpp"
#include "link/object_format_backend.hpp"

// The five shipped `ObjectFormatBackend` implementations, and NOTHING ELSE.
//
// This header is the realization tier's own index. It is included by the five
// backend TUs (each defining its accessor) and by `object_format_backends.cpp`
// (which assembles them into the one table the substrate is handed). Shared
// substrate does NOT include it — `src/link/object_format_backend.hpp` is the
// only thing the schema tier sees, and that header names no format at all.
//
// ★ WHY THESE ARE FUNCTIONS AND NOT `inline` OBJECTS, and why nobody
// self-registers: see the long note on `objectFormatBackendTable()` in
// `link/object_format_backend.hpp`. In short — a static self-registration
// idiom is silently broken by MSVC, which discards an object file none of
// whose symbols are referenced. A format would then vanish from the build with
// NO link error, and the only symptom would be "that format stopped loading",
// arbitrarily far from the cause. One table that references each accessor
// makes the dependency a LINK-TIME fact: delete a backend TU and the build
// fails immediately, naming the missing symbol.

namespace dss::link::format {

[[nodiscard]] DSS_EXPORT ObjectFormatBackend const& elfBackend()   noexcept;
[[nodiscard]] DSS_EXPORT ObjectFormatBackend const& peBackend()    noexcept;
[[nodiscard]] DSS_EXPORT ObjectFormatBackend const& machoBackend() noexcept;
[[nodiscard]] DSS_EXPORT ObjectFormatBackend const& wasmBackend()  noexcept;
[[nodiscard]] DSS_EXPORT ObjectFormatBackend const& spirvBackend() noexcept;

} // namespace dss::link::format
