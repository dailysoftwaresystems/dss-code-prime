#include "link/format/spirv.hpp"
#include "link/format/object_format_backends.hpp"

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

    // ── SELF-GUARD (D-LINK-…-KIND-IDENTITY-BRANCHES, TF-C125) ──────────
    //
    // ★★ THIS GUARD SURVIVED THE IDENTITY-BRANCH REMOVAL, AND THE REASON IS
    // MEASURED FOR THIS SITE. The TF-C125 brief expected it to become
    // redundant: with walkers reached only through a backend the loader
    // resolved, a walker "can never be handed a schema of another kind", so
    // the guard would be unreachable by construction and safely deletable.
    //
    // That premise is FALSE here. `spirv::encode` is a PUBLIC free function with
    // 5 direct call sites in `tests/`, none of which route through the
    // linker — and `SpirvWriter.NonSpirvFormatFailsLoud`
    // (tests/link/test_spirv_writer.cpp) hands it a FOREIGN schema on purpose and asserts this
    // exact refusal. Deleting the guard would not remove dead code; it would
    // delete tested behaviour and leave a public entry point that mis-encodes
    // silently. Refused, with evidence.
    //
    // ⚠ THE CITATION ABOVE IS PER-SITE ON PURPOSE. The first version of this
    // comment was one block pasted into all eight guards, every copy naming
    // the ELF writer's test as its proof — so seven of the eight cited a
    // measurement that was not about them. An independent audit caught it.
    // A comment stamped MEASURED that names the wrong measurement is worse
    // than no comment, under this project's own rule.
    //
    // What it stops being is an IDENTITY branch. It no longer compares an
    // enumerator; it compares the schema's resolved backend against the
    // singleton THIS TU implements — a pointer identity on an opaque handle,
    // in the sanctioned realization tier, which is exactly the tier permitted
    // to know which format it is. Unreachable from the linker (the resolver
    // cannot produce a mismatched pair), live for every direct caller.
    if (objectFormatSchema.backend() != &link::format::spirvBackend()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"spirv::encode called with non-Spirv format '"}
                 + std::string{objectFormatSchema.name()}
                 + "' (kind="
                 + std::string{
                       link::objectFormatBackendName(objectFormatSchema.backend())}
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
