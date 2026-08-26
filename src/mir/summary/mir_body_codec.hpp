#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_text.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
// OPT11 — THE `.dss.mir` BODY PAYLOAD (plan 22 §0.2, D-OPT11-LAZY-IMPORT-EDGE)
// ═══════════════════════════════════════════════════════════════════════════
//
// `.dss.summary` carries the DECISION material — the call graph and the symbol
// facts, no bodies — and `mir_summary.hpp` already encodes it. This file is the
// other half: the payload of the `.dss.mir` section, which is the BODIES a
// per-TU optimizer pages in when the index tells it where one lives. The
// two-section split IS the ThinLTO economy: the global pass reads every summary
// without touching one byte of this.
//
// ── ★★★ THE PAYLOAD IS `.dssir` TEXT, AND THAT IS A DELIBERATE CHOICE ──────
//
// The obvious alternative is a second hand-rolled binary encoder beside
// `encodeModuleSummary`. It was rejected, for three reasons that are about
// correctness rather than taste:
//
//   1. A MODULE'S TYPES ARE THE HARD PART, AND `.dssir` ALREADY SOLVES THEM.
//      Every `TypeId` is CU-scoped and interner-relative, so a body that crosses
//      a module boundary has to carry its types STRUCTURALLY and be re-interned
//      on arrival. `parseMir` does exactly that, into a fresh `TypeInterner`.
//      `encodeModuleSummary` deliberately carries NO types at all, so it offers
//      no precedent to copy — a binary body encoder would have to serialize the
//      type lattice from scratch, which is the whole of `type_reintern` written
//      a second time in a second dialect.
//   2. ONE SERIALIZER CANNOT DRIFT FROM ITSELF. `.dssir` has a byte-identical
//      round-trip contract — `emitMir(parseMir(emitMir(m))) == emitMir(m)` — and
//      it is pinned. A second encoder would have to be taught every new opcode,
//      and the one taught late would silently drop an operand
//      (D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS is that
//      defect, already recorded, on the encoder that exists).
//   3. IT IS DETERMINISTIC FOR FREE. The round-trip contract is exactly the
//      "byte-identical run to run" property this arc has to meet, and it is
//      already enforced by tests that exist.
//
// ⓘ AND THE CHOICE IS REVERSIBLE WITHOUT A FORMAT BREAK. The envelope below
// carries a `payloadKind` byte. A future compressed or binary payload is a new
// kind under the same envelope and the same version discipline — the section,
// the digest binding and the refusal rules do not move.
//
// ── FAIL-LOUD ─────────────────────────────────────────────────────────────
// A body is an input to codegen DECISIONS. A misread one is a miscompile, so
// `decodeModuleBody` returns nullopt — never a partially-filled module — on a
// bad magic, an unknown version, an unknown payload kind, a truncated buffer,
// any length field that overruns, or a payload the MIR parser rejects.
//
// ── ★★ THE DIGEST IS PART OF THE ENVELOPE, NOT AN AFTERTHOUGHT ────────────
// The envelope carries the module's Tier-1 digest and its target identity, so a
// decoded body can be checked against the summary that pointed at it BEFORE it
// is cloned into anything. Keying a fetch on a NAME alone is exactly the class
// of mistake P36 corrected in `CuBuildKey::languageName`: a declared name is a
// lookup key, never a proof of identity.
//
// ── AGNOSTIC ──────────────────────────────────────────────────────────────
// No language / target / object-format branch. Every integer is written
// LITTLE-ENDIAN byte by byte — never a `memcpy` of a struct — because DSS has a
// live big-endian leg (s390x under qemu-s390x) and a body produced there must
// decode identically here.

namespace dss::mirsum {

// The envelope's own version. Bumped whenever the ENVELOPE changes in a way a
// previous reader would misread. A decoder that sees a version it does not know
// REFUSES rather than guessing.
inline constexpr std::uint32_t kBodyFormatVersion = 1;

// What the payload bytes are. A closed vocabulary — an unknown value is a
// refusal, so a newer producer can never be silently misread by an older reader.
enum class BodyPayloadKind : std::uint8_t {
    DssirText = 1,   // canonical `.dssir`, the shipped payload
};

// A decoded module: the bodies, the interner their `TypeId`s were re-interned
// into, and the symbol names — everything a cross-module clone needs, and
// nothing it does not.
//
// Non-movable, because `MirParseResult` is: the `Mir`'s arenas hold tag
// references that must not change address. Held by pointer for that reason.
struct DecodedModuleBody {
    std::unique_ptr<MirParseResult> parsed;
    std::string                     moduleDigest;
    std::string                     targetIdentity;
};

// Encode one module. `symbolNames` is indexed by `SymbolId.v` (slot 0 unused) —
// the same table `MirTextContext` takes and `parseMir` hands back.
//
// `moduleDigest` and `targetIdentity` are OPAQUE strings the caller supplies,
// on the same terms `ModuleSummary` takes them: this is a MIR-tier leaf and must
// not reach up into the driver that owns the key scheme.
//
// Returns an empty vector (with a diagnostic) if the module cannot be rendered
// re-parseably — which `emitMir` reports at Error severity for exactly the
// values it cannot spell.
[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
encodeModuleBody(Mir const& mir, TypeInterner const& interner,
                 std::span<std::string const> symbolNames,
                 std::string_view moduleDigest, std::string_view targetIdentity,
                 DiagnosticReporter& reporter);

// Decode. `cuId` tags the fresh interner the types are re-interned into.
//
// `expectedDigest` / `expectedTarget`, when non-empty, are CHECKED against the
// envelope and a mismatch is a REFUSAL — that is the stale-body guard, and the
// reason to pass them is that a fetch which silently returns the previous
// build's body is indistinguishable from a correct one until the program
// misbehaves.
[[nodiscard]] DSS_EXPORT std::optional<DecodedModuleBody>
decodeModuleBody(std::span<std::uint8_t const> bytes, CompilationUnitId cuId,
                 std::string_view expectedDigest,
                 std::string_view expectedTarget,
                 DiagnosticReporter& reporter);

} // namespace dss::mirsum
