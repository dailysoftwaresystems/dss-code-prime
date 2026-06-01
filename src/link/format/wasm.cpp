#include "link/format/wasm.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"
#include "lir/lir_pass_util.hpp"

#include <array>
#include <cstdint>
#include <vector>

// WebAssembly module writer — plan 14 LK8 skeleton.
//
// Module preamble byte layout (WebAssembly spec §5.5):
//   [0x00]  magic   = 0x00 0x61 0x73 0x6d ('\0' 'a' 's' 'm')
//   [0x04]  version = 0x01 0x00 0x00 0x00 (binary format v1 — MVP)
//   [0x08]  sections[] (each: id u8 + size LEB128 + payload bytes)
//
// LK8 scope deliberately stops at byte 8: plan 18 owns the MIR→WAT
// section emitter. The skeleton's job is to prove the format-blind
// linker dispatch routes correctly + the JSON config + the byte-
// emit substrate work for WASM the same way they work for ELF /
// PE / Mach-O.

namespace dss::wasm {

namespace {

using dss::report;
using link::format::detail::appendU8;
using link::format::detail::emit;

// Spec-fidelity byte layout: reads left-to-right as the WebAssembly
// spec §5.5 prints it ("\0asm" + version 1 LE), unlike a u32 literal
// which would read byte-reversed to a casual reader. (type-design
// fold, LK8 post-fold review.)
constexpr std::array<std::uint8_t, 4> kWasmMagic   = {0x00, 0x61, 0x73, 0x6d};
constexpr std::array<std::uint8_t, 4> kWasmVersion = {0x01, 0x00, 0x00, 0x00};

} // namespace

std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter) {
    (void)targetSchema;  // LK8 skeleton: no target-specific bytes.

    if (objectFormatSchema.kind() != ObjectFormatKind::Wasm) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"wasm::encode called with non-Wasm format '"}
                 + std::string{objectFormatSchema.name()}
                 + "' (kind="
                 + std::string{
                       objectFormatKindName(objectFormatSchema.kind())}
                 + ")");
        return {};
    }

    // Fail-loud guard: the skeleton's caller contract is "empty
    // `module.functions`". A non-empty vector means the caller
    // routed native-ISA assembler output (x86_64 / ARM64 bytes)
    // to the WASM walker — those bytes have no meaning in WASM
    // (stack-machine bytecode, not a register-machine ISA, per
    // plan 18 §2.1). Plan 18 will replace this walker with a
    // MIR→WAT lowerer that produces its own bytes.
    if (!module.functions.empty()) {
        emit(reporter, DiagnosticCode::K_WalkerInputContractViolation,
             std::string{"wasm::encode: AssembledModule carries "}
                 + std::to_string(module.functions.size())
                 + " functions of native-ISA bytes, but the LK8 "
                   "skeleton walker does not consume them. WASM "
                   "bypasses LIR per plan 18 §2.1 — the MIR→WAT "
                   "lowerer (plan 18) is the producer of WASM "
                   "bytes, not the native assembler. Pass an empty "
                   "AssembledModule until plan 18 LK8 walker "
                   "replacement lands.");
        return {};
    }
    // Cross-format symmetry with PE/ELF/MachO (LK6 cycle 2a
    // precedent): WASM has imports (Import section, type idx +
    // module+name strings) but the structure is entirely different
    // from `ExternImport{symbol, mangledName, libraryPath}`. The
    // LK8 skeleton doesn't emit an Import section; passing externs
    // here means the caller routed FFI metadata that plan 18 will
    // ingest through a different surface. Fail loud rather than
    // silently dropping the imports. (silent-failure CRITICAL +
    // type-design + test-analyzer 3-agent convergence, LK8
    // review.)
    if (!module.externImports.empty()) {
        emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
             std::string{"wasm::encode: AssembledModule carries "}
                 + std::to_string(module.externImports.size())
                 + " externImport(s), but the LK8 skeleton walker "
                   "does not emit a WASM Import section. Plan 18 "
                   "(MIR→WAT) owns Import-section emission with a "
                   "WASM-native module+name pair structure that "
                   "doesn't map 1:1 to ExternImport{symbol,"
                   "mangledName,libraryPath}. Pass an empty "
                   "externImports until plan 18 LK8 walker "
                   "replacement lands.");
        return {};
    }
    // Empty `functions` + non-zero `expectedFuncCount` is a
    // caller-contract mismatch: the LinkedImage parallel-index
    // gate would silently fail `ok()` without surfacing the
    // misconfiguration here. Fail loud at the walker so the
    // diagnostic anchors to the WASM dispatch (silent-failure
    // HIGH + test-analyzer Gap 3 fold, LK8 review).
    if (module.expectedFuncCount != 0) {
        emit(reporter, DiagnosticCode::K_WalkerInputContractViolation,
             std::string{"wasm::encode: expectedFuncCount = "}
                 + std::to_string(module.expectedFuncCount)
                 + " but the LK8 skeleton requires "
                   "expectedFuncCount == 0 (it produces a "
                   "function-less module preamble). Plan 18 lifts "
                   "this restriction.");
        return {};
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(8);
    for (std::uint8_t b : kWasmMagic)   appendU8(bytes, b);
    for (std::uint8_t b : kWasmVersion) appendU8(bytes, b);
    return bytes;
}

} // namespace dss::wasm
