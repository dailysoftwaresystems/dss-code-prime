#include "link/format/spirv.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"

#include <cstdint>
#include <vector>

// SPIR-V module writer — plan 14 LK9 skeleton.
//
// Module header byte layout (SPIR-V Spec §2.3 — Physical Layout
// of a SPIR-V Module Binary). The module is a stream of 32-bit
// words; the first 5 words form the header:
//   word[0] = 0x07230203   magic (spec-fixed; reading order encodes
//                          the consumer's endianness contract — if
//                          the consumer reads `0x03022307` instead,
//                          the producer's endianness must be flipped)
//   word[1] = 0x00010600   version 1.6 (Major.Minor packed:
//                          0x00 _ major _ minor _ 0x00)
//   word[2] = 0            generator magic (0 = unspecified;
//                          plan 17 picks one when registered)
//   word[3] = 0            bound — `<id>` upper bound (no ids in
//                          skeleton → 0)
//   word[4] = 0            reserved (spec §2.3 — "must be 0")
// After the header: instruction stream. Plan 17 (MIR→SPIR-V)
// owns the instruction stream.
//
// LK9 scope stops at word[5] = byte 20: plan 17 owns the
// `OpCapability` / `OpExtension` / `OpMemoryModel` / `OpEntryPoint`
// / `OpTypeFunction` / `Op*` stream. The skeleton's job is to prove
// format-blind linker dispatch, JSON config, and byte-emit
// substrate all route correctly for SPIR-V (parallel to LK8's WASM
// substrate).

namespace dss::spirv {

namespace {

using link::format::detail::appendU32LE;
using link::format::detail::emit;

constexpr std::uint32_t kSpirvMagic   = 0x07230203u;  // spec §2.3
constexpr std::uint32_t kSpirvVersion = 0x00010600u;  // 1.6 — major
                                                       // in bits 16..23,
                                                       // minor in bits
                                                       // 8..15
constexpr std::uint32_t kSpirvGenerator = 0u;          // unspecified
constexpr std::uint32_t kSpirvBound     = 0u;          // no <id>s yet
constexpr std::uint32_t kSpirvReserved  = 0u;          // spec §2.3

} // namespace

std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter) {
    (void)targetSchema;  // LK9 skeleton: no target-specific bytes.

    if (objectFormatSchema.kind() != ObjectFormatKind::Spirv) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"spirv::encode called with non-Spirv format '"}
                 + std::string{objectFormatSchema.name()}
                 + "' (kind="
                 + std::string{
                       objectFormatKindName(objectFormatSchema.kind())}
                 + ")");
        return {};
    }

    // Walker input-contract guards (LK8 precedent — same shape,
    // distinct diagnostics anchored to plan 17 instead of plan 18).
    if (!module.functions.empty()) {
        emit(reporter, DiagnosticCode::K_WalkerInputContractViolation,
             std::string{"spirv::encode: AssembledModule carries "}
                 + std::to_string(module.functions.size())
                 + " functions of native-ISA bytes, but the LK9 "
                   "skeleton walker does not consume them. SPIR-V "
                   "bypasses LIR per plan 17 §2.5 — the MIR→SPIR-V "
                   "lowerer (plan 17) is the producer of SPIR-V "
                   "bytes, not the native assembler. Pass an empty "
                   "AssembledModule until plan 17 LK9 walker "
                   "replacement lands.");
        return {};
    }
    if (!module.externImports.empty()) {
        emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
             std::string{"spirv::encode: AssembledModule carries "}
                 + std::to_string(module.externImports.size())
                 + " externImport(s), but SPIR-V's import model is "
                   "`OpExtInstImport` (extended-instruction-set "
                   "imports declared inline in the module's "
                   "instruction stream) — entirely different shape "
                   "from `ExternImport{symbol, mangledName, "
                   "libraryPath}`. Plan 17 owns OpExtInstImport "
                   "emission. Pass an empty externImports until "
                   "plan 17 LK9 walker replacement lands.");
        return {};
    }
    if (module.expectedFuncCount != 0) {
        emit(reporter, DiagnosticCode::K_WalkerInputContractViolation,
             std::string{"spirv::encode: expectedFuncCount = "}
                 + std::to_string(module.expectedFuncCount)
                 + " but the LK9 skeleton requires "
                   "expectedFuncCount == 0 (it produces a "
                   "function-less module header). Plan 17 lifts "
                   "this restriction.");
        return {};
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(20);
    appendU32LE(bytes, kSpirvMagic);
    appendU32LE(bytes, kSpirvVersion);
    appendU32LE(bytes, kSpirvGenerator);
    appendU32LE(bytes, kSpirvBound);
    appendU32LE(bytes, kSpirvReserved);
    return bytes;
}

} // namespace dss::spirv
