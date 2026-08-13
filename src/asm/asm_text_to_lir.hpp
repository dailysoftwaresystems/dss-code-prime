#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/tree.hpp"
#include "lir/lir.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

// Standalone assembly TEXT → LIR (plan 29 P4; the `encode` pipeline tier).
//
// ★★★ WHAT THIS IS AND WHY IT IS NOT THE ORDINARY PIPELINE. A hand-written
// `.s` is ALREADY the machine tier: it names physical registers and writes its
// own frame. Running it through CST→HIR→MIR→LIR would re-derive what the
// programmer wrote, and the three passes that follow MIR→LIR would each rewrite
// it — `allocateRegisters` reassigns the registers, `legalizeTwoAddress`
// rewrites the instruction forms, and `materializeCallingConvention` injects a
// prologue/epilogue OVER the one in the file. So a `.s` enters at `encode`:
// this lowering produces the post-regalloc, post-legalize, post-callconv LIR
// that `assemble()` documents as its input contract, and nothing runs between.
//
// ★★ TARGET-BLIND AND DIALECT-BLIND, WHICH IS THE WHOLE POINT. Every spelling
// this walker understands comes from the GRAMMAR's `assembly` block (the
// dialect) and every name it resolves comes from the TARGET's `opcodes[]` /
// `registers[]`. There is no `if (arch == …)`, no `if (dialect == …)`, and no
// instruction table in this file. A second dialect is a `.lang.json`; a second
// CPU is a `.target.json`.
//
// ★★ FAIL LOUD, NEVER FIT BY GUESSING. An unknown mnemonic, an unknown
// register, an undeclared directive, an operand shape an opcode cannot take, or
// an opcode whose arity the source does not match is a DIAGNOSTIC naming the
// spelling, the dialect and the target. Assembly is the one language where a
// silently-substituted instruction is indistinguishable from what the
// programmer asked for, so there is no fallback anywhere in this file.

namespace dss {

// The lowered module plus the symbol identity the driver needs to finish the
// link. Returned by value; `lir` is move-only.
struct DSS_EXPORT AsmTextModule {
    Lir                       lir;
    // One entry per LIR function, parallel-indexed with `lir.funcAt(i)`. The
    // driver copies these into `AssembledModule::symbols` — the linker matches
    // across CUs by NAME, which the numeric IR does not carry.
    std::vector<ModuleSymbol> symbols;
    // The function a `.globl`-exported entry-named label defined, if any. The
    // driver stamps it onto `AssembledModule::userEntrySymbol` so the trampoline
    // injector calls the right function instead of falling back to functions[0].
    // ★ "EXPORTED" IS ENFORCED, NOT JUST DESCRIBED (2026-08-13). This sentence
    // and the code disagreed: the lowering elected on NAME alone and ignored
    // the `.globl` set entirely, so a `main:` without `.globl` was elected here
    // and then emitted with LOCAL linkage — the exact "entry symbol is local,
    // link failure points nowhere near the cause" outcome the dialect's own
    // `.globl` note warns about. An entry-named label that is not exported is
    // now a DIAGNOSTIC; leaving this nullopt instead would send the trampoline
    // to functions[0], which is the silent arm.
    std::optional<SymbolId>   userEntrySymbol;
    // Every symbol this file REFERENCES but does not DEFINE. The driver hands
    // these to `assemble()` through its `externs` span — the same channel the
    // ordinary pipeline uses for `MirToLirResult::externImports`, so an
    // assembly-sourced import and a C-sourced one are the same row type
    // arriving by the same route.
    //
    // ★★★ HOW A `.s` DECLARES AN EXTERN: IT DOESN'T, AND THAT IS THE SPEC.
    // gas has no extern directive. Its `.extern` is documented as accepted and
    // IGNORED — "as treats all undefined symbols as external" — so a symbol
    // referenced by a call and defined nowhere in the file simply IS one.
    // Requiring a directive would make DSS refuse assembly every reference
    // assembler accepts, which [[feedback_reference_compilers_are_the_spec]]
    // names as a defect in exactly the same way as accepting what none accept.
    //
    // ★★ SO THE FAIL-LOUD MOVES RATHER THAN DISAPPEARS, AND IT LANDS WHERE THE
    // ANSWER ACTUALLY IS. `libraryPath` stays EMPTY — a `.s` states no owning
    // image, and inventing one here would be a second owner of the fact the
    // descriptor corpus owns (the defect UCRT-P4 deleted from the C path by
    // REMOVING a per-language library default, not by repointing it). An
    // unbound import is exactly what a C bare prototype produces, and the
    // linker's existing reference gate already judges it per format: a
    // relocatable `.o` / ELF `.so` keeps it as a legal undefined symbol for a
    // later binder, and an EXEC image — where nothing binds later — rejects
    // LOUD naming the symbol. One policy, one implementation, two source
    // languages.
    std::vector<ExternImport> externImports;

    // D-ASM-NO-DATA-DEFINING-DIRECTIVE: the data items the file's data-defining
    // directives produced (`.data` / `.rodata` + `.byte` / `.quad` / `.zero`),
    // one per data LABEL plus at most one anonymous item per section run.
    //
    // ★ THE SAME ROW TYPE AND THE SAME SECTION VOCABULARY THE C PATH PRODUCES.
    // `lowerMirGlobalsToDataItems` (asm.cpp) hands the driver exactly this
    // vector for a C translation unit; the linker walkers already concatenate
    // items by `DataSectionKind` and map each item's `symbol` to its
    // section-relative address. Nothing here is assembly-specific — which is
    // why the verb set binds `DataSectionKind` instead of minting one.
    //
    // ⚠ THE DRIVER MUST COPY THIS INTO `AssembledModule::dataItems`, and until
    // it does the bytes are computed and DROPPED. `assemble()` builds its
    // module from the LIR, which carries no data; the C path's driver arm
    // assigns `assembled.dataItems = lowerMirGlobalsToDataItems(...)`, and the
    // assembly arm needs the one-line twin
    // (`assembled.dataItems = std::move(lowered->dataItems);` in
    // `program/compile_pipeline.cpp`'s `assembleAsmUnit`). Anchored:
    // D-ASM-DATA-ITEMS-NOT-WIRED-INTO-THE-DRIVER.
    std::vector<AssembledData> dataItems;
};

// Lower one parsed `.s` translation unit to `encode`-tier LIR.
//
// `tree` is the CST as parsed by `grammar` (whose `assembly()` block must be
// `declared` — the caller routes here only for a root declaring the `encode`
// tier, so a non-dialect grammar arriving here is a driver bug and fails loud
// rather than producing an empty module).
//
// `entryNames` are the source language's program-entry spellings, as the driver
// resolved them for the active format; a defined label matching one of them
// becomes `userEntrySymbol`. Empty is legal (a relocatable/object build).
//
// Returns nullopt on any failure, with at least one diagnostic reported.
[[nodiscard]] DSS_EXPORT std::optional<AsmTextModule>
lowerAsmTextToLir(Tree const&                   tree,
                  GrammarSchema const&          grammar,
                  TargetSchema const&           target,
                  std::span<std::string const>  entryNames,
                  DiagnosticReporter&           reporter);

} // namespace dss
