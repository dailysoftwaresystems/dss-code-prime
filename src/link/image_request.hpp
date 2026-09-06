#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Per-PROGRAM image requests — knobs the PROGRAM BEING BUILT asks of the
// emitted image, as opposed to the defaults its object format DECLARES in
// its `.format.json`.
//
// The distinction is the whole point. A `.format.json` states what is true
// of *every* image in that format (its page size, its subsystem, its
// conventional 1 MiB stack reserve). But some image properties are
// properties of the PROGRAM, not the format — the stack it needs is a
// function of its own deepest call chain — and those cannot be a fixed
// number in shared config. Every real toolchain exposes them as link-time
// requests (MSVC `/STACK`, GNU ld `-Wl,--stack`); this struct is DSS's.
//
// A request is a REQUEST, never an assumption: `linker::link` asks the
// format whether it declares a CAPABILITY for each populated field (e.g.
// `ObjectFormatSchema::stackReserveControl()`) and fails LOUD when it does
// not. Formats differ genuinely — PE-exec can carry a stack reserve, PE-dll
// cannot (the loader ignores a DLL's SizeOfStackReserve), ELF has no image
// field for it at all — so the engine reads the declared capability and
// never tests a format identity.
//
// Sources + precedence (resolved in `Program`, before this struct is built):
// the CLI flag WINS over the project manifest key when both are present.

namespace dss {

struct DSS_EXPORT ImageRequest {
    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: requested stack reserve in
    // BYTES, or nullopt to take the format's declared default. Validated
    // against the format's `stackReserveControl` bounds at the linker gate
    // — never clamped, never rounded, never silently dropped.
    std::optional<std::uint64_t> stackReserveBytes;

    // ── D-LK-MACHO-DYLIB-INSTALL-NAME-IS-ONE-CONSTANT-FOR-EVERY-ARTIFACT ──
    //
    // The FILE NAME (no directory) of the artifact this emission is producing.
    // A format document may declare an identity string that is a FUNCTION of
    // it — the Mach-O LC_ID_DYLIB install name is the first — and the walker
    // resolves the declaration against this. EMPTY means "the caller could not
    // say", which is a REFUSAL wherever a declaration needs it and a silent
    // no-op wherever none does.
    //
    // ★ WHY IT RIDES THE REQUEST RATHER THAN THE SCHEMA, and it is not a
    // preference. `ObjectFormatSchema`'s copy constructor is deleted and the
    // driver hands the SAME memoized `shared_ptr<ObjectFormatSchema const>` to
    // every artifact of a format inside one build — so an identity stored there
    // would be shared by exactly the artifacts that must differ, which is the
    // collapse this field exists to end. The request is per-emission by
    // construction, and this struct's own docblock already draws the line it
    // falls on: a property of the PROGRAM being built, never of its format.
    //
    // ★ IT IS A FACT, NOT A KNOB, so it is deliberately absent from `empty()`
    // below and from `enforceImageRequest`'s capability gate. The gate asks
    // "did the user REQUEST something this format cannot do?"; the driver
    // supplies this on every emission, so counting it would make every request
    // non-empty and turn a question about user intent into a constant. A format
    // that declares no placeholder ignores it — nothing is dropped, because
    // nothing was asked. What CANNOT happen silently is the converse: a format
    // that DOES declare one and an emission that cannot answer, which the
    // walker refuses loud by name.
    std::string artifactFileName;

    // True iff this request asks for anything at all. Lets a caller skip the
    // whole gate cheaply, and keeps the "did the user request something?"
    // question in ONE place as fields are added.
    // ⚠ `artifactFileName` is NOT consulted — see its docblock: it is a fact
    // the driver supplies unconditionally, not a request a format could refuse.
    [[nodiscard]] bool empty() const noexcept {
        return !stackReserveBytes.has_value();
    }
};

// THE single validator for an `ImageRequest` against a format's DECLARED
// capabilities. Returns true when the request may proceed (including the
// common case of an EMPTY request); returns false after emitting exactly one
// fail-loud diagnostic when it may not.
//
// It lives here, beside the request type, precisely so there is ONE
// implementation rather than one per caller. That is not tidiness — it is the
// bug this function exists to prevent: the first cut of this feature checked
// the CAPABILITY in the PE walker but the declared BOUNDS only at the linker
// gate, so `pe::encode` reached directly with an 8 GiB request (double the
// format's declared `maximumBytes`) wrote it verbatim into the header and
// reported success. Every entry point that can emit an image calls THIS, so a
// half-fastened belt cannot recur:
//   * `linker::link` — the gate every compile-driven emission passes through;
//   * `pe::encode` (and any future walker implementing a vehicle) — public
//     entry points reachable without the gate.
// Calling it twice on one path is harmless only because the first failure
// stops the path; on the success path it is a pure predicate.
//
// `contextLabel` prefixes the diagnostic ("linker", "pe::encode") so the user
// can see which tier refused. The REMEDY prose is read from the format's
// declared `stackReserveUnsupportedReason`, never inferred from a format
// identity.
[[nodiscard]] DSS_EXPORT bool
enforceImageRequest(ImageRequest const&       request,
                    ObjectFormatSchema const& format,
                    std::string_view          contextLabel,
                    DiagnosticReporter&       reporter);

} // namespace dss
