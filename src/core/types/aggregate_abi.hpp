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
    AbiPieceClass cls;          // GPR (INTEGER eightbyte) or FPR (SSE eightbyte, or an X87 result's x87 value)
    std::uint64_t byteOffset;   // the piece's byte offset within the aggregate
    std::uint32_t widthBytes;   // valid bytes in this eightbyte (1..8); an X87 result's piece: the value's storage
};

// How an aggregate value is passed/returned: its CLASS. The class is the classifier's; the CARRIER of a
// `Memory` ARGUMENT is the calling convention's (`CallingConvention::aggregateMemoryArgument`: its bytes in the
// outgoing stack argument area, SysV AMD64 psABI 3.2.3, or a pointer to a caller copy, Win64 and AAPCS64), and a
// `Memory` RESULT always travels through the hidden result pointer (sret).
// ★ P69 (lane `cs`, fold f26c — D-TARGET-SYSV-AMD64-MEMORY-CLASS-ARGUMENTS-GO-BY-POINTER-AND-X87-RESULTS-BY-SRET):
// this kind was `ByReference`, which named one carrier, and every SysV MEMORY-class argument went by a pointer
// where gcc and clang read its bytes on the stack; and the X87 class did not exist, so `struct { long double x; }`
// was returned through a hidden pointer where both return it in `%st(0)`.
// ★ P69 (lane `cs`, fold f26c2 — review-csf26c F3): a RESULT of the psABI's COMPLEX_X87 class (a bare
// `long double _Complex`) comes back on the x87 stack as TWO values, the real part in %st0 and the imaginary in
// %st1, where this classifier answered MEMORY by size (sret). So `X87` is "a result on the x87 return stack" and its
// `pieces` are the x87 values in stack order. ✔MEASURED (probe cx): gcc 13.3.0, clang 18.1.3 and Apple clang 21.0.0
// for x86_64 return it so, pass it in memory, and keep a struct holding one MEMORY by size.
struct AbiPassing {
    enum class Kind : std::uint8_t {
        InRegisters,   // the `pieces` go in GPR/FPR arg/return registers
        Memory,        // in memory: an argument by the convention's carrier, a result through sret
        X87,           // a RESULT on the x87 return stack: one x87 `long double` per piece, %st0 first (SysV X87 /
                       // X87UP: one piece; COMPLEX_X87: two, the real part in %st0, the imaginary in %st1)
    } kind = Kind::Memory;
    // InRegisters: the eightbytes, each in a GPR or an FPR. X87: the x87 values, in x87 return-stack order (%st0
    // first), each of class Fpr (an x87 value is a float, `regClassForCoreType`), at its offset, its width the
    // value's storage bytes. Memory: empty.
    std::vector<AbiPiece> pieces;
    // ★ P69 (lane `cs`, fold f26c2 — review-csf26c F14): the ELEMENT's byte size when the classifier answered a
    // homogeneous floating-point aggregate (AAPCS64's HFA: 1-4 leaves of one floating type, one register each),
    // else 0. A calling convention may place such an aggregate on the stack by its element's alignment rather
    // than its own (`StackArgPackingRules::homogeneousAggregateAlignmentOf`): ✔MEASURED (lane `cs`, probe f14),
    // Apple clang 21 puts `struct { _Alignas(32) double a, b, c, d; }` on an 8-byte slot, aarch64 gcc 13.3.0 and
    // clang 18.1.3 on a 16-aligned one.
    std::uint32_t homogeneousElementBytes = 0;
};

// Which side of a call a classification is for: SysV classes an aggregate of class X87 (every leaf one x87
// `long double` at offset 0) and a `long double _Complex` (COMPLEX_X87) as MEMORY when it is passed and returns
// it on the x87 stack (psABI 3.2.3: "If the class is X87, X87UP or COMPLEX_X87, it is passed in memory"; "If the
// class is X87, the value is returned on the X87 stack in %st0"; "If the class is COMPLEX_X87, the real part of the
// value is returned in %st0 and the imaginary part in %st1").
enum class AggregatePosition : std::uint8_t { Argument, Result };

// Whether the by-value aggregate ABI for `strategy` is IMPLEMENTED (C1 = SysV,
// C2 = Win64; AAPCS64 lands at C3). The strategy-availability guard consults this
// so an un-built CC (None / AAPCS64) fails loud at the by-value site rather than
// mis-passing.
[[nodiscard]] DSS_EXPORT bool
aggregateAbiImplemented(AggregateClassKind strategy) noexcept;

// Classify a complete struct/union `aggTy` passed/returned BY VALUE under the convention's
// aggregate `rules` (its strategy, its register-size bound and, for SysV, its X87UP rule).
// nullopt ⇒ the strategy is not implemented OR the
// type is un-sizeable (the caller's fail-loud signal). For a RETURN,
// `Kind::Memory` means sret; `Kind::X87` is answered only for a `Result` (its pieces the x87 values).
// `layoutParams`/`dm` drive the nested `computeLayout` the field-overlap walk
// needs. AGNOSTIC: switches on `strategy` (a closed enum), never on identity.
[[nodiscard]] DSS_EXPORT std::optional<AbiPassing>
classifyAggregate(AggregateConventionRules const& rules,
                  TypeId aggTy, TypeInterner const& interner,
                  AggregateLayoutParams layoutParams, DataModel dm,
                  AggregatePosition position);

} // namespace dss
