#pragma once

#include "core/export.hpp"
#include "core/types/aggregate_layout.hpp"          // AggregateLayoutParams, AggregateClassKind
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// ── FC7: the by-value aggregate ABI CLASSIFIER (D-FC7-STRUCT-BY-VALUE-ARG-RETURN) ──
//
// A GENERIC, target/format-AGNOSTIC engine that decides HOW a struct/union passed
// or returned BY VALUE is realized: in registers (a list of GPR/FPR pieces), or by
// reference (a hidden pointer; for a RETURN that is "sret"). The engine NEVER
// branches on a target/format/cc name — it switches on the CLOSED
// `AggregateClassKind` strategy enum (declared per-CC in `.target.json`, the
// `slotAligned` precedent) and reads bounded per-strategy params. "Config-driven"
// here = params-in-config + a bounded per-strategy ALGORITHM (SysV eightbyte
// classification does not reduce to a flat table).
//
// C1 implements the **SysV AMD64 eightbyte** strategy and C2 the **MS x64
// (Win64) by-size** strategy; AAPCS64 is declared-but-unrealized (the strategy-
// availability guard keeps it fail-loud until C3). The realization (synthesis of
// the register pieces / pointer) lives at HIR→MIR (the §B-locked boundary),
// reusing the existing aggregate primitives; this engine only CLASSIFIES.

namespace dss {

// One register-resident piece of an in-registers aggregate (a SysV "eightbyte").
enum class AbiPieceClass : std::uint8_t { Gpr, Fpr };
struct AbiPiece {
    AbiPieceClass cls;          // GPR (INTEGER eightbyte) or FPR (SSE eightbyte)
    std::uint64_t byteOffset;   // the piece's byte offset within the aggregate
    std::uint32_t widthBytes;   // valid bytes in this eightbyte (1..8)
};

// How an aggregate value is passed/returned.
struct AbiPassing {
    enum class Kind : std::uint8_t {
        InRegisters,   // the `pieces` go in GPR/FPR arg/return registers
        ByReference,   // a hidden pointer (for a RETURN this means sret)
    } kind = Kind::ByReference;
    std::vector<AbiPiece> pieces;   // populated iff InRegisters
};

// Whether the by-value aggregate ABI for `strategy` is IMPLEMENTED (C1 = SysV,
// C2 = Win64; AAPCS64 lands at C3). The strategy-availability guard consults this
// so an un-built CC (None / AAPCS64) fails loud at the by-value site rather than
// mis-passing.
[[nodiscard]] DSS_EXPORT bool
aggregateAbiImplemented(AggregateClassKind strategy) noexcept;

// Classify a complete struct/union `aggTy` passed/returned BY VALUE under
// `strategy` + `maxRegBytes`. nullopt ⇒ the strategy is not implemented OR the
// type is un-sizeable (the caller's fail-loud signal). For a RETURN,
// `Kind::ByReference` means sret. `layoutParams`/`dm` drive the nested
// `computeLayout` the field-overlap walk needs. AGNOSTIC: switches on `strategy`
// (a closed enum), never on identity.
[[nodiscard]] DSS_EXPORT std::optional<AbiPassing>
classifyAggregate(AggregateClassKind strategy, std::uint16_t maxRegBytes,
                  TypeId aggTy, TypeInterner const& interner,
                  AggregateLayoutParams layoutParams, DataModel dm);

} // namespace dss
