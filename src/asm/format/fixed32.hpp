#pragma once

#include "asm/asm.hpp"
#include "asm/format/walker_util.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <cstdint>
#include <span>
#include <vector>

// `fixed32` format walker — plan 13 AS3.
//
// Encodes one LIR instruction whose target opcode declares
// `encoding.shape == Fixed32`. Mirrors `x86_variable::encode`'s
// substrate convention but for the FIXED-32-BIT-WORD + BIT-FIELD-SLOT
// encoding shape that AArch64 / RV32 / MIPS-fixed share.
//
// The walker reads the variant's `template.fixedWord` (a `uint32`
// base bit pattern), then for each declared wire OR's the operand's
// `hwEncoding` (register-only in cycle-3 scope) into the slot's bit
// position:
//   * `Rd`  → bits 0..4   (5-bit register field)
//   * `Rn`  → bits 5..9
//   * `Rm`  → bits 16..20
// The resulting word is emitted as 4 little-endian bytes.
//
// Cycle-3 scope is register-only: non-Reg operands (immediates,
// memory addressing modes) are rejected with a hard diagnostic.
// Future immediate slots (`Imm12` / `ImmShift` / etc.) join the
// vocabulary when their consumer cycle lands.
//
// **Owned architectural rules** (plan 13 §2.4):
//   * 5-bit-wide register fields (AArch64 GPR ordinals 0..31).
//   * Little-endian word emission.
//   * `slot bit-position → fixedWord OR mask` derivation.
// All target-specific concerns (which opcodes exist, which fixedWord
// values they carry, which slot positions they wire) live in the
// JSON, NOT in this walker. Adding a new fixed32 target = drop a new
// `*.target.json`; new bit-field slot shapes = one new
// `EncodingSlotKind` entry + one new arm here, in lockstep.

namespace dss::fixed32 {

[[nodiscard]] DSS_EXPORT bool
encode(Lir const&                  lir,
       TargetSchema const&         schema,
       LirInstId                   inst,
       TargetOpcodeInfo const*     info,
       std::span<MirInstId const>  lirToMir,
       std::vector<std::uint8_t>&  out,
       std::vector<Relocation>&    relocs,
       std::vector<SourceMapEntry>& srcMap,
       std::vector<walker_util::BlockRelPatch>& blockPatches,
       // D-CSUBSET-COMPUTED-GOTO: pending synthetic-symbol ↔ block
       // bindings from a block-address `lea` (its trailing BlockRef
       // operand). asm.cpp resolves each against the function's
       // completed block-offset table into a `SyntheticBlockSymbol`.
       std::vector<walker_util::BlockSymPatch>& blockSymPatches,
       // D-CSUBSET-LONG-BRANCH: the LIR instructions this function's
       // branch-relaxation fixed point has PROMOTED to their escape form,
       // as a SORTED span of `LirInstId.v`. Empty on the first encode pass
       // of every function — and permanently empty for every function whose
       // branches all fit, which is why an unrelaxed function encodes
       // byte-for-byte as it did before this parameter existed.
       //
       // ⚠ IT IS AN INPUT TO ENCODING, NOT AN OUTPUT OF IT, and that is the
       // whole architecture: an escape emits REAL INSTRUCTIONS, so it changes
       // the function's byte layout, so it cannot be applied by the patch
       // resolver after the block-offset table is built. `asm.cpp` re-encodes
       // the function with a larger promoted set until the set stops growing.
       //
       // Only `fixed32` takes it. `x86_variable` has no block-relative slot
       // wider than `BlockRel32`, so no escape can be ELECTED there at all
       // (see the election in the .cpp) and a parameter it could never read
       // would be a false promise rather than symmetry.
       std::span<std::uint32_t const> relaxedInsts,
       DiagnosticReporter&         reporter);

} // namespace dss::fixed32
