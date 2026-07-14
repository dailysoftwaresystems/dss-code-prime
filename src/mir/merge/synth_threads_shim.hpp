#pragma once

// FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): the pe64 <threads.h> Win32 SHIM synthesis
// pass — Vehicle A. The Windows CRT exports no `thrd_*`/`mtx_*`/`cnd_*`/`tss_*`, so the
// C11 <threads.h> function each user program calls is a COMPILER-SYNTHESIZED function
// whose body this pass emits over the real kernel32 primitives (CRITICAL_SECTION,
// CONDITION_VARIABLE, Fls*, the thread APIs). On elf/macho the SAME <threads.h> symbols
// are ordinary libc FFI imports and this pass is a clean no-op.
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
#include "core/types/extern_import.hpp"   // ExternImport (kernel32 helper imports)

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dss {

class Mir;
class TypeInterner;
class DiagnosticReporter;

// Synthesize a definition for every pe64 <threads.h> shim symbol in `recipeBySymbol`.
// EMPTY map ⇒ clean no-op (every elf/macho + every non-threads TU). On success `mir` is
// REBUILT with the shim functions appended and `externImports` carries the kernel32
// helpers they call (deduped). Returns false (fail-loud, reported) only on an internal
// invariant breach — a recipe id with no switch arm (the closed-vocab loader guard makes
// this unreachable in practice, but the arm is the anti-silent-gap backstop).
[[nodiscard]] DSS_EXPORT bool
synthesizeThreadsShim(Mir&                                                  mir,
                      TypeInterner&                                         interner,
                      std::unordered_map<std::uint32_t, std::string> const& recipeBySymbol,
                      std::vector<ExternImport>&                            externImports,
                      DiagnosticReporter&                                   reporter);

} // namespace dss
