#pragma once

// FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER / D-CSUBSET-C11-THREADS-MACHO): the <threads.h>
// SHIM synthesis pass — VEHICLE-PARAMETERIZED. Neither the Windows CRT nor macOS libSystem
// exports `thrd_*`/`mtx_*`/`cnd_*`/`tss_*`, so on those formats the C11 function each user
// program calls is a COMPILER-SYNTHESIZED function whose body this pass emits over the
// host OS's real primitives — the `win32` vehicle over kernel32 (CRITICAL_SECTION,
// CONDITION_VARIABLE, Fls*, the thread APIs) or the `pthread` vehicle over Darwin
// libSystem (pthread_mutex_*, pthread_cond_*, pthread_key_*, pthread_self, sched_yield).
// The vehicle + the import library name are read from the FORMAT descriptor's
// `librarySynthesis` block (`LibrarySynthesis`), never from a format-name branch. On elf
// the SAME <threads.h> symbols are ordinary libc FFI imports and this pass is a clean
// no-op (its recipe map is empty).
//
// This pass mirrors the `synthesizePeStartup` / `synthesizeSehFunclets` structure (a
// whole-Mir rebuild via `MirFunctionRebuilder` + `IdentityClonePolicy`, then the synth
// functions appended, then `cloneGlobalsVerbatim`), and is wired from the SAME two
// PRE/POST-optimize driver seams (compile_pipeline single-CU + program.cpp multi-CU),
// alongside `synthesizePeStartup`. It appends the kernel32 helper imports it needs
// on demand (deduped against any already-imported name — the `__C_specific_handler`
// precedent), so a <threads.h> TU that never `#include`s <windows.h> still links.
//
// `recipeBySymbol` maps each shim function's PRE-MINTED SymbolId.v (minted at semantic
// injection, seeded into HIR→MIR `functionSymbols` by the CST→HIR skip so the user call
// already lowered to `GlobalAddr(sym)`) to its recipe id (== the C11 function name). For
// each entry the pass emits `addFunction(sig, sym, Global, Default)` + a single-block
// body from the recipe switch — the DEFINITION the user's not-yet-bound call resolves
// to. Non-empty ONLY on a pe target (the descriptor tags the pe variants only), so the
// pass keys on a DATA property (the map), never `if (format == pe)`.
//
// AGNOSTIC: lives in `src/mir/merge` (the opt-in format-specific-synthesis
// neighborhood), fires on a config-declared recipe tag, emits target-neutral MIR (the
// MS-x64 ABI is applied downstream by the target's `callingConventionIndex`, not keyed
// here). FAIL-LOUD: a recipe id with no switch arm (a vocab/switch drift) reports and
// returns false — never a silently-undefined shim.

#include "core/export.hpp"
#include "core/types/extern_import.hpp"       // ExternImport (native helper imports)
#include "core/types/object_format_kind.hpp"  // LibrarySynthesis (win32/pthread vehicle)

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dss {

class Mir;
class TypeInterner;
class DiagnosticReporter;

// Synthesize a definition for every <threads.h> shim symbol in `recipeBySymbol`, over the
// primitive family the format's `librarySynthesis` block declares. EMPTY map ⇒ clean no-op
// (every elf + every non-threads TU). On success `mir` is REBUILT with the shim functions
// appended and `externImports` carries the native helpers they call (deduped, imported from
// `librarySynthesis->libraryPath`). Returns false (fail-loud, reported) on: (a) a non-empty
// recipe map with NO `librarySynthesis` — a format carrying synthesize-tagged threads
// symbols but declaring no vehicle (never silently assume one); or (b) a recipe id with no
// switch arm for the active vehicle (the closed-vocab loader guard makes this unreachable
// in practice, but the arm is the anti-silent-gap backstop).
[[nodiscard]] DSS_EXPORT bool
synthesizeThreadsShim(Mir&                                                  mir,
                      TypeInterner&                                         interner,
                      std::unordered_map<std::uint32_t, std::string> const& recipeBySymbol,
                      std::optional<LibrarySynthesis> const&                librarySynthesis,
                      ObjectFormatKind                                      format,
                      std::vector<ExternImport>&                            externImports,
                      DiagnosticReporter&                                   reporter);

} // namespace dss
