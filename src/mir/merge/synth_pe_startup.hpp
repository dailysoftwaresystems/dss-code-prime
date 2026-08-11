#pragma once

// UCRT-P4 — THE ENTRY-SHAPE REALIZATION PASS.
//
// ⓘ FILE NAME. This file shipped at c111 as the pe-only `__getmainargs` synth
// (`synthesizePeStartup`) and the name is kept so `git log --follow` stays
// intact, but the pass is NO LONGER pe-specific and no longer only a
// synthesizer: it now runs for EVERY format and its FIRST job is a refusal gate.
// Nothing inside it names a format.
//
// ── WHAT IT DOES ─────────────────────────────────────────────────
//
// ★★★ IT MATERIALIZES. IT NO LONGER GATES, AND THE GATE'S REMOVAL FROM HERE IS
// THE POINT — read this before adding a check back.
//
// This pass used to OWN the entry-shape refusal: it classified the resolved
// entry's signature out of the merged MIR and refused any signature the FORMAT
// did not declare (`K_EntryShapeNotDeclared`). That gate was in the wrong tier
// for two independent reasons, both measured:
//
//   * NO SOURCE SPAN, EVER. `Mir` carries no `BufferId`/`SourceSpan` for a
//     function, so the refusal could only name the entry by SYMBOL NAME. A
//     missing location is trusted less than a wrong one, but it is still the
//     weakest possible report of a defect that is a plain declaration mistake
//     with an obvious source location. The check is now at the SEMANTIC tier
//     (`S_EntryShapeNotDeclared`), pointing at the declarator.
//   * IT ASKED THE FORMAT A QUESTION ONLY THE LANGUAGE CAN ANSWER. "Is
//     `fn(i32, ptr-ptr-u16) -> i32` an entry signature" is about how C spells an
//     entry, not about what a loader can do. Keying the refusal on the format's
//     table is what let a `wmain`-only ELF build be told that `wmain` WAS the
//     Linux entry and that adding an ELF config row was the remedy.
//
// So the SIGNATURE check moved to the semantic tier and the CANDIDACY decision
// moved to entry resolution, which intersects the language's declared entry rows
// with the format's declared `entryVerbs`. By the time control reaches this
// function the entry is already resolved AND its verb is already known, so this
// pass is handed the VERB and does exactly one job with it.
//
// ⚠ DO NOT REINTRODUCE A CLASSIFICATION HERE. Re-deriving the verb from the
// MIR signature would recreate the two-owner state this cycle deleted: the
// language config would declare the signature→verb mapping and this file would
// independently re-derive it, and the two would drift silently. The verb is an
// INPUT.
//
// 1. MATERIALIZE. Dispatch on the RESOLVED entry's verb × the format's declared
//    `processArgs.mechanism`. The two axes are independent and both are needed:
//    the verb says WHICH arguments the entry needs, the mechanism says HOW this
//    format obtains them.
//      * verb `none`                        → nothing to do (a no-arg entry).
//      * verb `argc-argv` / `argc-wargv`, mechanism `crt-argv-accessors`
//                                           → synthesize the pre-main init
//                                             described below and RETARGET the
//                                             program entry to it.
//      * ditto, mechanism `stack-vector`    → nothing here; the entry trampoline
//                                             materializes from the entry stack.
//      * ditto, no mechanism declared       → nothing here; the loader already
//                                             put them in the argument registers
//                                             (Mach-O LC_MAIN / dyld).
//
// ── THE SYNTHESIZED INIT (mechanism `crt-argv-accessors`) ──────────────────
//
//   int _dss_pe_start() {                       // the wmainCRTStartup role
//       _configure_narrow_argv(<argvMode>);     // _configure_wide_argv for wmain
//       int    argc = *__p___argc();            // ONE dereference — the
//       char **argv = *__p___argv();            // accessors return addresses
//       if (argv == NULL) return <declared status>;   // see the gate note below
//       return main(argc, argv);                // the resolved user entry
//   }
//
// ★ PROBE-0, MEASURED 2026-08-10, AND THE REASON THIS SHAPE IS LEGITIMATE: the
// accessor triple returns the REAL command line in a STARTUP-LESS DSS pe64
// binary, byte-identical to the c111 `__getmainargs` control, at debug AND
// release. No `_initterm` prologue and no `__acrt_initialize` are needed.
// `_configure_narrow_argv` IS load-bearing — without it `*__p___argc() == 0` and
// `*__p___argv() == NULL`.
//
// ★★ THE GATE TESTS argv, NOT THE RETURN VALUE, AND THAT IS MEASURED RATHER
// THAN CAUTIOUS: all three valid `_crt_argv_mode` values return `errno_t` **0**,
// and mode 0 returns 0 *while yielding `argv == NULL`*. So the `errno_t` cannot
// distinguish success from "produced nothing", and a guard on it would assert
// nothing. On failure the init RETURNS the format's declared status instead of
// calling the user entry, so the value leaves through the format's already-wired
// `processExit` path — no second exit import, and no `Unreachable` planted in a
// reachable position where an optimizer could treat the branch as dead.
//
// ⓘ WIDE vs NARROW is still chosen from the RESOLVED ENTRY'S SIGNATURE and never
// from a format flag — the c111 rule, sharpened twice. c111 inspected the argv
// element's `TypeKind` inline here. Then the matched FORMAT row carried the verb.
// Now the matched LANGUAGE row carries it (`main(int, char**)` → `argc-argv`,
// `wmain(int, unsigned short**)` → `argc-wargv`) and this pass is simply TOLD
// which — so the fact is config-declared, derived once, and this file holds no
// opinion about C's spelling of an entry. argc is SHARED between the narrow and
// wide worlds (MEASURED, PROBE-0), so one accessor serves both.

#include "core/export.hpp"
#include "core/types/extern_import.hpp"       // ExternImport
#include "core/types/strong_ids.hpp"          // SymbolId
#include "core/types/entry_shape.hpp"         // EntryMaterialization (the verb)
#include "core/types/target_schema.hpp"       // ProcessArgs

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dss {

class Mir;
class TypeInterner;
class DiagnosticReporter;

// Materialize the resolved entry's arguments for its already-decided verb.
//
// `verb` is the materialization verb of the LANGUAGE row that matched the
// resolved entry's signature, decided at entry resolution. It is an INPUT, not
// something this pass derives — see the header docblock for why re-deriving it
// would recreate a two-owner state. `EntryMaterialization::None` (a no-argument
// entry) is a legitimate value and a clean no-op.
//
// `processArgs` is the format's declared argument mechanism, or nullopt when it
// declares none — which is a real ANSWER on Mach-O, where dyld puts argc/argv in
// the argument registers before any DSS code runs, and NOT an omission.
//
// On success:
//   * when the verb needs the CRT accessor route, `mir` is REBUILT with the
//     synth init appended (Mir is frozen; the rebuild uses the shared
//     `MirFunctionRebuilder` substrate — every existing function is cloned
//     verbatim, then the init is added), the CRT imports are appended to
//     `externImports`, and `userEntrySymbol` is RETARGETED to the init;
//   * otherwise `mir`, `externImports` and `userEntrySymbol` are untouched.
//
// Returns false (fail-loud, already reported) when the format declares a
// mechanism this pass has no arm for, or when the declared mechanism's own
// fields are unusable. NO resolved entry (a library TU with no `main`) is a clean
// no-op: there is nothing to materialize into.
[[nodiscard]] DSS_EXPORT bool
realizeEntryShape(Mir&                              mir,
                  TypeInterner&                     interner,
                  std::optional<SymbolId>&          userEntrySymbol,
                  std::vector<ExternImport>&        externImports,
                  EntryMaterialization              verb,
                  std::optional<ProcessArgs> const& processArgs,
                  CSymbolDecorationScheme           scheme,
                  std::string_view                  formatName,
                  DiagnosticReporter&               reporter);

} // namespace dss
