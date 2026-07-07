#pragma once

// c111 (D-RUNTIME-PE-MAIN-ARGS): the Windows CRT out-parameter args mechanism.
//
// The PE OS entry receives NO C argument vector — argc/argv must be ASKED for via
// an msvcrt call. Rather than hand-emit that call in the entry-trampoline (which
// runs no callconv/regalloc pass, so stack locals + calls are manual RSP math),
// the USER §B decision is to SYNTHESIZE a tiny pre-main init function in ORDINARY
// MIR (where the normal lowering materializes the frame, the 5-arg call's shadow
// space, and 16-byte alignment for free):
//
//   int _dss_pe_start() {                 // the wmainCRTStartup role
//       int argc; wchar_t **argv, **env;  // (char** for a narrow `main` entry)
//       int startupinfo = 0;              // msvcrt _startupinfo{int newmode}
//       __wgetmainargs(&argc, &argv, &env, 0, &startupinfo);   // __getmainargs (narrow)
//       return wmain(argc, argv);         // the resolved user entry
//   }
//
// and retarget the program entry to it. The entry-trampoline then calls this
// synth function (its own arg-setup is a no-op for the CrtOutParam mechanism).

#include "core/export.hpp"
#include "core/types/extern_import.hpp"       // ExternImport
#include "core/types/strong_ids.hpp"          // SymbolId
#include "core/types/target_schema.hpp"       // ProcessArgs

#include <optional>
#include <vector>

namespace dss {

class Mir;
class TypeInterner;
class DiagnosticReporter;

// Synthesize the PE pre-main init function when `processArgs.mechanism` is
// `CrtOutParam` and the resolved entry (`userEntrySymbol`) takes (argc, argv).
// On success:
//   * `mir` is REBUILT with the synth function appended (Mir is frozen; the build
//     uses the shared MirFunctionRebuilder substrate — every existing function is
//     cloned verbatim, then the synth function is added);
//   * the msvcrt arg-fetch export is appended to `externImports`;
//   * `userEntrySymbol` is RETARGETED to the synth function.
// A no-arg entry (`main(void)`) needs no setup and is left unchanged. A non-PE /
// non-CrtOutParam format is a no-op. Returns false (fail-loud, reported) only on a
// malformed argv parameter type (an argv whose element is neither char nor the
// pe wide-char). The wide vs narrow msvcrt name is chosen by the entry's argv
// element width (u16 ⇒ wide `__wgetmainargs`, char/i8 ⇒ narrow `__getmainargs`) —
// never a format-level flag, which would mis-call the other entry shape.
[[nodiscard]] DSS_EXPORT bool
synthesizePeStartup(Mir&                        mir,
                    TypeInterner&               interner,
                    std::optional<SymbolId>&    userEntrySymbol,
                    std::vector<ExternImport>&  externImports,
                    ProcessArgs const&          processArgs,
                    DiagnosticReporter&         reporter);

} // namespace dss
