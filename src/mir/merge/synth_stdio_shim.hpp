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
// ★ VARIADIC FORWARDING IS THREE INSTRUCTIONS. On a HomogeneousPointer target (Win64) a
// `va_list` IS a single pointer into the contiguous home+overflow area, and MIR already
// carries the leaf: `VaHomeArgAreaAddr`, whose PAYLOAD is the named-arg slot count and
// whose result is `&home[namedArgCount]` — the first vararg. Its presence is ALSO
// lir_callconv's Win64 prologue-spill signal, so nothing else need be threaded. No
// `__va_list_tag`, no alloca, no `VaStart`, no `va_end`. Which leaf to emit is read from
// the TARGET's declared `VaListLayout::strategy` (the same field `hir_to_mir` selects on)
// — never from a format-name branch; the SysVRegisterSave arm is deliberately FAIL-LOUD
// until a consumer exists, because no elf/macho descriptor declares stdio synthesize
// recipes and building that arm now would be a speculative build. A CC that declares NO
// `vaListLayout` at all arrives as `nullopt` and is likewise refused: a defaulted strategy
// would be the identity branch the bar forbids (the `librarySynthesis` precedent in
// `synth_threads_shim`).
//
// ★ MANGLING — THE HONEST STATEMENT OF THIS PASS'S TARGET REACH. The pass resolves its
// UCRT core by a BARE C name (`"__stdio_common_vsprintf"`) against a map keyed on
// `ExternImport::mangledName`. That is correct ONLY because the single shipped recipe is
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
#include "core/types/target_schema.hpp"   // VaListStrategy

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
// ★ THE FAMILY SHIPS EXACTLY ONE RECIPE: `sprintf`. That is the whole live vocabulary —
// `stdio.json`'s pe `sprintf` row is the only row carrying `synthesize`, and the loader's
// closed recipe table (`shipped_lib_descriptor.cpp`'s `kRecipes`) lists exactly that one
// stdio id. There is deliberately no `snprintf`/`fprintf`/`vfprintf` arm: an arm with no
// descriptor row is an un-consumed mechanism, and (for the `fprintf` pair) it could not
// even work — their `__stdio_common_vfprintf` core is declared NOWHERE, so the arm would
// fail loud at core resolution the first time anything reached it. Each further recipe
// returns TOGETHER WITH its own descriptor row, its core's symbol row, and a runtime
// witness — never ahead of them.
//
// `externImports` is READ-ONLY here (const&): unlike `synthesizeThreadsShim`, which MINTS
// its kernel32 helper imports, every core this pass calls is an ordinary descriptor import
// that already exists in the module — so there is nothing to append.
//
// Returns false (fail-loud, reported) on: (a) a recipe id with no switch arm; (b) a core
// (`__stdio_common_v*`) the module does not import — meaning stdio.json did not declare it,
// which would otherwise yield a silently-undefined shim; (c) a `vaStrategy` this pass has
// no arm for (the SysVRegisterSave case above); or (d) `vaStrategy == nullopt` — the active
// CC declared no `vaListLayout` at all, so there is no declared variadic model to forward.
[[nodiscard]] DSS_EXPORT bool
synthesizeStdioShim(Mir&                                                  mir,
                    TypeInterner&                                         interner,
                    std::unordered_map<std::uint32_t, std::string> const& recipeBySymbol,
                    std::optional<VaListStrategy>                         vaStrategy,
                    std::vector<ExternImport> const&                      externImports,
                    DiagnosticReporter&                                   reporter);

} // namespace dss
