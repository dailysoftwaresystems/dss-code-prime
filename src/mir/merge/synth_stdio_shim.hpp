#pragma once

// D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): the <stdio.h> printf-family SHIM synthesis pass.
//
// The modern Universal CRT (`ucrtbase.dll`) exports NO concrete `printf`/`fprintf`/
// `sprintf`/`sscanf`/`vfprintf`. In a real MSVC build those are HEADER INLINES over a
// small family of common cores — `__stdio_common_v{f,s}{printf,scanf}` — so a compiler
// that binds the CRT by export table (as DSS does) finds nothing to import. The decided
// resolution (fork b2) is to SYNTHESIZE the missing bodies here rather than link
// `legacy_stdio_definitions.lib`, which is a VS-toolchain-only static lib absent on Linux
// and would break the standing "build any target from any host" requirement.
//
// WHY THIS IS ITS OWN PASS rather than an arm of `synth_threads_shim`: the two families
// synthesize over UNRELATED vehicles (kernel32 primitives vs the UCRT stdio cores) and
// their helper imports come from different libraries. They share only the recipe-map
// mechanism, which is why the map is family-dispatched at the seam instead of merged here.
//
// ★ THE HELPERS ARE ORDINARY DESCRIPTOR IMPORTS, NOT MINTED HERE (user decision,
// 2026-07-25). `__stdio_common_v*` are declared as pe symbol rows in `stdio.json`, so they
// arrive as normal FFI imports already carrying the right library binding — which means
// the EAGER-IMPORT LAW (D-FFI-DESCRIPTOR-EAGER-IMPORT) verifies their existence for us,
// and this pass needs no helper-import machinery at all. It resolves each by NAME against
// the module's `externImports` and fails loud if one is absent (a descriptor/pass drift).
//
// ★ VARIADIC FORWARDING IS THREE INSTRUCTIONS. On a HomogeneousPointer target a `va_list`
// IS a single pointer into the incoming argument area, and MIR already carries the leaf
// that produces it. Its presence is ALSO lir_callconv's prologue-spill signal, so nothing
// else need be threaded. No `__va_list_tag`, no alloca, no `VaStart`, no `va_end`.
//
// ★★ WHICH leaf is NOT decided by the strategy alone, and getting that wrong is a SILENT
// wrong-output miscompile rather than a diagnostic — which is why this pass takes the
// WHOLE `VaListLayout`, not the bare `VaListStrategy` it used to take. HomogeneousPointer
// is NOT a synonym for Win64: `arm64.target.json`'s `apple_arm64` CC declares
// `homogeneous_pointer` TOO, with `variadicUsesOverflowBase: true` (Apple arm64 has no
// home area — every vararg is stacked, so `ap` starts at the OVERFLOW base). The real
// lowering honours that flag (`hir_to_mir.cpp`, the HomogeneousPointer `va_start` arm):
// `VaOverflowArgAreaAddr` (no payload) when it is set, `VaHomeArgAreaAddr` (payload = the
// named-arg slot count, i.e. `&home[namedArgCount]`) when it is not. A shim that could see
// only the strategy would emit the home leaf on BOTH — pointing `ap` at named-arg storage
// instead of the first stacked vararg, with no diagnostic anywhere. So the layout is
// threaded whole and this pass reads the SAME field from the SAME struct `hir_to_mir`
// reads: ONE source of truth, no rule duplicated in two places to drift apart.
//
// Which arm to take at all is read from the TARGET's declared `VaListLayout::strategy`
// (the same field `hir_to_mir` selects on) — never from a format-name branch; the
// SysVRegisterSave / Aapcs64DualCursor arms are deliberately FAIL-LOUD until a consumer
// exists, because no elf/macho descriptor declares stdio synthesize recipes and building
// those arms now would be a speculative build. A CC that declares NO `vaListLayout` at all
// arrives as `nullopt` and is likewise refused: a defaulted layout would be the identity
// branch the bar forbids (the `librarySynthesis` precedent in `synth_threads_shim`).
//
// ★ MANGLING — THE HONEST STATEMENT OF THIS PASS'S TARGET REACH. The pass resolves each
// UCRT core by a BARE C name (`"__stdio_common_vsprintf"`) against a map keyed on
// `ExternImport::mangledName`. That is correct ONLY because every shipped recipe is
// pe-only, and pe's C mangling is the IDENTITY. It is NOT format-agnostic, and the sibling
// `synthesizeThreadsShim` shows what agnostic costs: it takes an `ObjectFormatKind`
// parameter for the sole purpose of `applyCMangling`-ing its helper names per format
// (macho prepends `_`). That parameter is deliberately NOT taken here — no macho/elf stdio
// recipe exists, so threading it now would be a speculative build. THE PRICE OF THAT
// CHOICE, STATED SO IT IS NOT PAID BY SURPRISE: adding an elf/macho stdio recipe REQUIRES
// threading the format in and mangling the helper name FIRST. Skip that and the lookup
// simply misses, and the resulting diagnostic blames the DESCRIPTOR for what is really a
// mangling bug in this pass — the wrong end of the pipeline.
//
// Mirrors `synthesizePeStartup` / `synthesizeThreadsShim` structurally (whole-Mir rebuild
// via `MirFunctionRebuilder` + an identity clone policy, synth functions appended, then
// `cloneGlobalsVerbatim`) and is wired from the SAME two PRE/POST-optimize driver seams
// (compile_pipeline single-CU + program.cpp multi-CU).

#include "core/export.hpp"
#include "core/types/extern_import.hpp"   // ExternImport (helper resolution by name)
#include "core/types/target_schema.hpp"   // VaListLayout (strategy + variadicUsesOverflowBase)

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dss {

class Mir;
class TypeInterner;
class DiagnosticReporter;

// Synthesize a definition for every <stdio.h> printf-family shim symbol in
// `recipeBySymbol` (ALREADY FILTERED to the stdio family by the caller). EMPTY map ⇒ clean
// no-op — every elf/macho build and every pe TU that includes no <stdio.h> printf family.
//
// ★ THE FAMILY SHIPS SIX RECIPES: `printf`, `fprintf`, `sprintf`, `snprintf`, `vfprintf`,
// `sscanf` — exactly the set `stdio.json` declares a pe `synthesize` row for, and exactly
// the set the loader's closed recipe table (`shipped_lib_descriptor.cpp`'s `kRecipes`) tags
// Stdio. The two lists are one vocabulary and MUST stay in lock-step: an arm with no
// descriptor row is an un-consumed mechanism, and a descriptor row with no arm fails loud
// below. Each further recipe (the `_s` family, the wide twins) lands TOGETHER WITH its own
// descriptor row, its core's symbol row, and a runtime witness — never ahead of them.
//
// The six reach the UCRT through three cores + one accessor, every one of them an
// ORDINARY `stdio.json` pe symbol row (so the eager-import law has already proven each is
// a real `ucrtbase.dll` export):
//   printf   -> __stdio_common_vfprintf(0, __acrt_iob_func(1), fmt, NULL, ap)
//   fprintf  -> __stdio_common_vfprintf(0, stream, fmt, NULL, ap)
//   vfprintf -> __stdio_common_vfprintf(0, stream, fmt, NULL, ap)   [ap is a real param]
//   sprintf  -> __stdio_common_vsprintf(LEGACY_NULLTERM, buf, (size_t)-1, fmt, NULL, ap)
//   snprintf -> __stdio_common_vsprintf(STANDARD_SNPRINTF, buf, n,          fmt, NULL, ap)
//               then `r < 0 ? -1 : r`   [the ONE multi-block recipe in this family]
//   sscanf   -> __stdio_common_vsscanf (0, buf, (size_t)-1, fmt, NULL, ap)
//
// ★★ `snprintf` SHARES `sprintf`'s CORE — IT DOES NOT GET ITS OWN, AND THAT IS A MEASURED
// FACT, NOT A SHORTCUT. There is no `__stdio_common_vsnprintf`: `objdump -p
// C:/Windows/System32/ucrtbase.dll` (2,484 exports) has `__stdio_common_vsprintf`
// (ordinal 117) and `__stdio_common_vsnprintf_s` (115) and NOTHING between them. The `_s`
// twin is a DIFFERENT function (extra `_MaxCount`, secure-CRT validation), not a spelling
// variant, so it is not a substitute either. Naming a nonexistent core in a descriptor row
// would break EVERY pe binary's LOAD with 0xC0000139 under the eager-import law — the exact
// failure the advice to use that name was trying to prevent. The real UCRT does the same
// thing this pass does: in SDK 10.0.26100.0 `ucrt/stdio.h`, `snprintf` calls `vsnprintf`,
// whose body is one `__stdio_common_vsprintf` call differing from
// `sprintf`'s only in the two arguments that matter — the `_Options` bit and a REAL
// `_BufferCount`. Which is why this recipe adds no import at all.
//
// ★ `vfprintf` IS THE ODD ONE OUT AND THE ONLY ONE WITH NO VA LEAF. It is not variadic —
// C 7.21.6.8 gives it a declared `va_list ap` PARAMETER, already pointing at the caller's
// first unnamed argument — so its shim FORWARDS `Arg 2` verbatim and emits no
// `Va*ArgAreaAddr` at all. Emitting one would be actively wrong (it would re-derive `ap`
// from the SHIM's own frame, which has no varargs) and would additionally hand
// lir_callconv a spurious prologue-spill signal. That `ap` is a plain 8-byte pointer
// argument is not an assumption: the pass refuses every strategy but HomogeneousPointer,
// under which `va_list` IS a single pointer (Win64 `char*`, Apple arm64 the same shape).
//
// `externImports` is READ-ONLY here (const&): unlike `synthesizeThreadsShim`, which MINTS
// its kernel32 helper imports, every core this pass calls is an ordinary descriptor import
// that already exists in the module — so there is nothing to append.
//
// Returns false (fail-loud, reported) on: (a) a recipe id with no switch arm; (b) a core
// (`__stdio_common_v*` / `__acrt_iob_func`) the module does not import — meaning stdio.json
// did not declare it, which would otherwise yield a silently-undefined shim; (c) a
// `vaLayout->strategy` this pass has no arm for (the SysVRegisterSave / Aapcs64DualCursor
// cases above); or (d) `vaLayout == nullopt` — the active CC declared no `vaListLayout` at
// all, so there is no declared variadic model to forward.
[[nodiscard]] DSS_EXPORT bool
synthesizeStdioShim(Mir&                                                  mir,
                    TypeInterner&                                         interner,
                    std::unordered_map<std::uint32_t, std::string> const& recipeBySymbol,
                    std::optional<VaListLayout> const&                    vaLayout,
                    std::vector<ExternImport> const&                      externImports,
                    DiagnosticReporter&                                   reporter);

} // namespace dss
