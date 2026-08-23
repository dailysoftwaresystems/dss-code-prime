#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/tree.hpp"
#include "lir/lir.hpp"
// `LirFuncCfi` / `LirCfiOp` — the instruction-anchored unwind carriage the
// calling-convention materializer ALREADY produces for compiled functions. This
// producer fills the same type rather than a parallel assembly-only one; see
// `AsmTextModule::perFuncCfi`.
#include "lir/lir_callconv.hpp"

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

// D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET: one interior-label block
// symbol that ONLY a DATA item names, awaiting the byte offset `assemble()`
// computes for its block.
//
// ★★★ WHY THIS ROW EXISTS AT ALL, AND WHY IT IS NOT A SECOND JUMP-TABLE MODEL.
// The interior-symbol MODEL is already built and already ships:
// `SyntheticBlockSymbol{symbol, blockByteOffset}` (asm.hpp), published per
// function by `assemble()` and turned into an interior VA by
// `link/format/interior_block_symbol_va.hpp` — identically for ELF, PE and
// Mach-O. A `.s` reaching it needs exactly ONE fact the assembler cannot
// supply: which LIR block a symbol names. There are two carriers for that fact
// already, and NEITHER can hold this case:
//   * `walker_util::BlockSymPatch` — the ENCODER's channel. It is recorded when
//     an instruction carries a trailing `BlockRef` operand (the block-address
//     `lea`), so it fires for `adr x0, Lw` and NEVER for a label named only by
//     `.quad Lw`, which emits no instruction at all.
//   * `JumpTableDescriptor` — carries `funcIndex` + a block→symbol map, but it
//     also OWNS THE TABLE'S BYTES: `compile_pipeline.cpp` synthesizes an
//     `AssembledData` of `slotCount * 8` zero bytes for every descriptor. A
//     `.s` already WROTE those bytes with its own data directives (which may
//     interleave `.quad`s with anything else in one item), so populating a
//     descriptor would emit the table TWICE.
// ⇒ what is genuinely missing is the BINDING, not a table. This row carries
// only it, and the pipeline binds it through the SAME two lines the jump-table
// path uses (`bindBlockSymbol` in `program/compile_pipeline.cpp`), so the two
// languages share one implementation of "symbol S is block B of function F".
struct DSS_EXPORT AsmBlockSymbolBinding {
    // Index into `lir.funcAt(i)` / `AssembledModule::functions` of the function
    // that CONTAINS the block. Assembly's function order is source order of
    // function-entry labels, which is also `addFunction` call order.
    std::size_t   funcIndex = 0;
    std::uint32_t lirBlockV = 0;   // LirBlockId.v of the addressed block
    SymbolId      symbol{};        // the interior label's minted symbol
};

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

    // D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET: the interior-label
    // block symbols that only `dataItems`' relocations name. EMPTY unless a
    // data directive named a code label — the ordinary `.s` binds every one of
    // its block symbols through the encoder's `BlockSymPatch`, and this vector
    // exists solely for the jump-table shape, which emits no instruction that
    // could carry a `BlockRef`.
    //
    // ⚠ THE DRIVER MUST BIND THESE AFTER `assemble()` RETURNS. Until it does,
    // the relocation in the data item names a symbol with NO VA, which the
    // linker reports as an undefined symbol rather than silently — but the
    // diagnostic would point at the linker instead of at the missing wiring.
    std::vector<AsmBlockSymbolBinding> blockSymbolBindings;

    // ★★★ D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED: the CALL FRAME INFORMATION
    // this file's `.cfi_*` directives state — one entry per LIR function,
    // parallel-indexed with `lir.funcAt(i)`. Until this existed the 18 declared
    // `.cfi_*` spellings were `ignoredAnnotation`: the file assembled, the
    // program ran correctly, and nothing could walk its stack.
    //
    // ★★ `nullopt` MEANS **UNDESCRIBED**, AND IT IS NOT THE SAME AS AN EMPTY
    // OP LIST. gas lets a file describe some functions and not others, and an
    // undescribed one must get no FDE at all. An engaged entry with ZERO ops is
    // a real description — ✔MEASURED 2026-08-17, gcc 13.3.0 emits
    // `.cfi_startproc` / `ret` / `.cfi_endproc` around a leaf function and gas
    // emits an FDE with no ops for it — so "the ops are empty" cannot be the
    // predicate. Same shape as `elf.cpp`'s and `macho.cpp`'s own
    // `std::vector<std::optional<CfiFunction>>`.
    //
    // ★★ IT IS THE SAME TYPE THE C PATH PRODUCES, DELIBERATELY. `LirFuncCfi`
    // (`lir/lir_callconv.hpp`) is what `materializeCallingConvention` emits for
    // a compiled function, keyed the same way — by the `LirInstId` after which
    // each rule is in effect — and `assemble()` resolves BOTH through one
    // `resolveFuncCfi`. Two producers, one representation, one resolver: a
    // hand-written frame and a compiled frame become `.eh_frame` FDEs (and
    // Win64 `UNWIND_INFO`) through identical code, so neither can drift into
    // describing frames the other cannot.
    //
    // ⚠ ONE STATE IS THIS PRODUCER'S ALONE, AND IT IS NOT A MISS: a rule
    // written ABOVE the function's first instruction anchors to nothing and
    // takes effect at byte offset 0, which is an op with an INVALID `inst` and
    // `atBlockEnd == false`. The callconv producer never emits that shape (its
    // ops always follow an instruction it just wrote), so `resolveFuncCfi`
    // names the case explicitly rather than inferring it from a lookup miss —
    // a miss there is a genuine substrate break and still fails loud.
    std::vector<std::optional<LirFuncCfi>> perFuncCfi;

    // The frame state every function in this unit starts in, DERIVED from the
    // target (see `deriveCfiInitialState`) and engaged only once some `.cfi_*`
    // directive actually needed it. nullopt ⇒ this file described no frame, in
    // which case `perFuncCfi` is all-empty and no unwind table is produced —
    // which is the same "nothing to describe" state a `.o` or a hand-built
    // module is in, and `buildEhFrame` already treats as success.
    std::optional<CfiInitialState> cfiInitial;
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
