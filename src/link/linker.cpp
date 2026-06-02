#include "link/linker.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/entry_trampoline.hpp"
#include "link/format/elf.hpp"
#include "link/format/macho.hpp"
#include "link/format/pe.hpp"
#include "link/format/spirv.hpp"
#include "link/format/wasm.hpp"
#include "lir/lir_pass_util.hpp"

#include <format>
#include <string>
#include <unordered_set>
#include <utility>

namespace dss::linker {

namespace {

using dss::report;

} // namespace

LinkedImage link(AssembledModule const&    inputModule,
                 TargetSchema const&       targetSchema,
                 ObjectFormatSchema const& objectFormatSchema,
                 DiagnosticReporter&       reporter) {
    LinkedImage image;
    image.format = objectFormatSchema.kind();
    image.expectedFuncCount = inputModule.expectedFuncCount;

    // D-LK10-ENTRY Slice C (plan 14 §2.13): when the format declares
    // a `processExit` block + `entryCallingConvention` (validated by
    // Slice B's cross-field rule), synthesize a `_start` trampoline
    // and prepend it as `functions[0]`. The walker's
    // `imageEntryOverride` path then uses the trampoline as the
    // image entry, while the schema's `entryPoint` continues to name
    // the user fn (the trampoline's call target).
    //
    // Without this hook, the emitted executables point e_entry /
    // AddressOfEntryPoint / LC_MAIN.entryoff directly at the user
    // fn, which SEGVs on its `ret` epilogue (process entry has no
    // return address). See §2.13 for the full design.
    //
    // Bypass conditions: caller-provided `imageEntryOverride` (a
    // pre-injected trampoline; do not re-inject) OR empty functions
    // (no module to wrap).
    AssembledModule moduleCopy;
    AssembledModule const* moduleP = &inputModule;
    bool const wantTrampoline =
        objectFormatSchema.processExit().has_value()
     && !inputModule.functions.empty()
     && !inputModule.imageEntryOverride.has_value();
    if (wantTrampoline) {
        moduleCopy = inputModule;
        if (!injectEntryTrampoline(moduleCopy, targetSchema,
                                    objectFormatSchema, reporter)) {
            // Diagnostic(s) emitted by the trampoline emitter.
            image.resolvedFuncCount = 0;
            return image;
        }
        moduleP = &moduleCopy;
        image.expectedFuncCount = moduleCopy.expectedFuncCount;
    }
    AssembledModule const& module = *moduleP;

    // Snapshot error count so we can detect whether the cross-
    // reference unifier pass (below) added any new K_* diagnostics
    // — if so, the module is not valid input for the format walker
    // and we must skip dispatch. Separation of concerns mirrors
    // ML6 cycle 3a's `LirAllocation::ok()` discipline: linkage
    // VALIDITY (kind + symbol cross-checks) is one stage; format
    // EMISSION (byte serialization) is another; the second runs
    // only when the first passed.
    std::size_t const errorsAtEntry = reporter.errorCount();

    // Index every function symbol the assembler declared (defined
    // intra-module) PLUS every extern import the assembler stamped
    // for FFI resolution. Single-CU resolution; cross-CU symbol-
    // table merge is LK11 (D-LK4-3). Externs are resolved against
    // import-table emission in the per-format walker (LK6 cycle 2
    // — PE IAT lands at cycle 2a; ELF GOT/PLT at 2b; Mach-O
    // chained-fixups at 2c).
    std::unordered_set<SymbolId> declaredSymbols;
    declaredSymbols.reserve(module.functions.size());
    for (auto const& fn : module.functions) {
        declaredSymbols.insert(fn.symbol);
    }
    // D-LK4-RODATA-PRODUCER (2026-06-02): rodata `AssembledData`
    // items are also valid reloc targets. The format walker
    // (pe.cpp::encodeExec for PE) merges them into its symbolVa
    // map at `rdata->rva + section-offset`. The linker's
    // pre-walker cross-reference check must accept them here so
    // a .text → rodata REL32 doesn't fire K_SymbolUndefined.
    std::unordered_set<SymbolId> dataSymbols;
    dataSymbols.reserve(module.dataItems.size());
    for (auto const& di : module.dataItems) {
        dataSymbols.insert(di.symbol);
    }
    std::unordered_set<SymbolId> externSymbols;
    externSymbols.reserve(module.externImports.size());
    for (std::size_t i = 0; i < module.externImports.size(); ++i) {
        auto const& ext = module.externImports[i];
        // Empty mangledName / libraryPath silently produce broken
        // imports (Windows: GetProcAddress("") → null IAT slot;
        // ELF: empty DT_SONAME; Mach-O: empty LC_LOAD_DYLIB path).
        // The loaders accept the binary at link time and crash at
        // first call. Fail loud here so the toolchain owns the
        // diagnostic. (3-agent convergence: silent-failure C2 +
        // type-design O1 + architect O4.)
        if (ext.mangledName.empty()) {
            report(reporter, DiagnosticCode::K_SymbolUndefined,
                   DiagnosticSeverity::Error,
                   "ExternImport[" + std::to_string(i) +
                   "] (symbol #" + std::to_string(ext.symbol.v) +
                   ") has empty mangledName — import-table entries "
                   "require a non-empty symbol name.");
        }
        if (ext.libraryPath.empty()) {
            report(reporter, DiagnosticCode::K_SymbolUndefined,
                   DiagnosticSeverity::Error,
                   "ExternImport[" + std::to_string(i) +
                   "] (symbol #" + std::to_string(ext.symbol.v) +
                   ") has empty libraryPath — import-table entries "
                   "require a non-empty DLL / SO / dylib name.");
        }
        // Cross-table SymbolId uniqueness: a SymbolId cannot appear
        // in BOTH `functions` and `externImports` — the resolution
        // would silently pick whichever the walker's map-emplace
        // saw first (silent-failure C1 + type-design O2 2-agent
        // convergence).
        if (declaredSymbols.contains(ext.symbol)) {
            report(reporter, DiagnosticCode::K_SymbolUndefined,
                   DiagnosticSeverity::Error,
                   "symbol #" + std::to_string(ext.symbol.v) +
                   " is BOTH a defined AssembledFunction AND an "
                   "ExternImport — ambiguous resolution; the same "
                   "SymbolId cannot reference both an intra-module "
                   "function and an external library symbol.");
        }
        // Within-table duplicate ExternImport: two ExternImports
        // with the same SymbolId silently overwrite each other in
        // the walker's emplace.
        if (!externSymbols.insert(ext.symbol).second) {
            report(reporter, DiagnosticCode::K_SymbolUndefined,
                   DiagnosticSeverity::Error,
                   "ExternImport symbol #" +
                   std::to_string(ext.symbol.v) +
                   " is declared more than once in "
                   "AssembledModule.externImports.");
        }
    }

    for (auto const& fn : module.functions) {
        bool funcResolved = true;
        for (auto const& reloc : fn.relocations) {
            // Cross-reference unifier (plan 13 §2.6): the SAME opaque
            // `kind` tag must resolve on BOTH the target side (so the
            // formula is known) AND the format side (so the platform-
            // native name is known). The diagnostic names BOTH sides
            // when both miss — masking either half hides half the
            // misconfiguration from the user and forces a second
            // round-trip after they fix the first error.
            auto const* targetReloc =
                targetSchema.relocationInfo(reloc.kind);
            auto const* formatReloc =
                objectFormatSchema.relocationByKind(reloc.kind);
            bool const kindResolved =
                targetReloc != nullptr && formatReloc != nullptr;
            if (!kindResolved) {
                std::string msg = "relocation kind ";
                msg += std::to_string(reloc.kind.v);
                msg += " in symbol #";
                msg += std::to_string(fn.symbol.v);
                msg += " is not declared by ";
                bool needAnd = false;
                if (targetReloc == nullptr) {
                    msg += "target schema '";
                    msg += std::string{targetSchema.name()};
                    msg += "'";
                    needAnd = true;
                }
                if (formatReloc == nullptr) {
                    if (needAnd) msg += " and ";
                    msg += "object format '";
                    msg += std::string{objectFormatSchema.name()};
                    msg += "'";
                }
                report(reporter,
                       DiagnosticCode::K_RelocationKindMismatch,
                       DiagnosticSeverity::Error,
                       std::move(msg));
                funcResolved = false;
            }
            // Symbol-undefined runs INDEPENDENTLY of kind resolution
            // — a misconfigured reloc whose kind is wrong AND whose
            // target is undefined should surface BOTH diagnostics
            // in one pass, not require two link attempts.
            //
            // A reloc target resolves if ANY of:
            //   (a) it names a defined function in this module, OR
            //   (b) it names an extern import (LK6 cycle 2 —
            //       resolved at link time via the per-format walker's
            //       import-table emission), OR
            //   (c) it names a module-level data item — rodata global,
            //       string literal global, etc. (D-LK4-RODATA-PRODUCER
            //       2026-06-02 — the per-format walker merges these
            //       into its symbolVa map at section-relative offsets).
            // Anything else is a hard undefined.
            if (!declaredSymbols.contains(reloc.target)
             && !externSymbols.contains(reloc.target)
             && !dataSymbols.contains(reloc.target)) {
                std::string msg = "relocation in symbol #";
                msg += std::to_string(fn.symbol.v);
                msg += " references undefined symbol #";
                msg += std::to_string(reloc.target.v);
                msg += " (not declared by any AssembledFunction, "
                       "ExternImport, nor AssembledData item)";
                report(reporter,
                       DiagnosticCode::K_SymbolUndefined,
                       DiagnosticSeverity::Error,
                       std::move(msg));
                funcResolved = false;
            }
        }
        if (funcResolved) ++image.resolvedFuncCount;
    }

    // Skip walker dispatch if the cross-reference unifier failed.
    // A module that produces K_SymbolUndefined / K_RelocationKindMismatch
    // is not valid input for the format walker — emitting partial
    // bytes from a known-invalid module is exactly the silent-failure
    // class the substrate discipline rejects.
    //
    // Resetting `resolvedFuncCount` is load-bearing (architect
    // convergence): without it, a 3-function module where ONLY fn#1
    // had a reloc failure would report `resolvedFuncCount = 2`,
    // `expectedFuncCount = 3` (ok() = false — correct) — but a
    // 1-function module whose ONLY function had a reloc failure
    // would still report `resolvedFuncCount = 0` (ok() = false, also
    // correct). The subtle bug: a 2-function module where fn#0 had
    // no relocs and fn#1's relocs all failed: pre-fix produced
    // resolvedFuncCount=1, expectedFuncCount=2 (ok()=false, correct
    // by accident). But if EVERY function's relocs failed,
    // resolvedFuncCount remained partial. Reset on linkage failure
    // makes "linkage failed → no functions are resolved" structural.
    if (reporter.errorCount() != errorsAtEntry) {
        image.resolvedFuncCount = 0;
        return image;
    }

    // D-LK4-RODATA-SUBSTRATE precondition guard: until the per-
    // format walker arms (D-LK2-RODATA / D-LK1-RODATA / D-LK3-
    // RODATA) close, no walker consumes `module.dataItems`. A
    // future producer (HIR string-literal promotion → MIR global →
    // assembler) that lands `dataItems` BEFORE the matching walker
    // arm would silently emit a binary with NO `.rdata`/`.rodata`/
    // `__cstring` section — every string literal silently drops on
    // the floor; downstream `printf("hello")` reads garbage from
    // an unrelated VA. Fail-loud HERE so the producer cycle is
    // sequenced after at least one walker arm.
    //
    // **D-LK4-RODATA-BSS-INVARIANT + duplicate-SymbolId + zero-
    // alignment guards**: validate the substrate invariants on
    // `dataItems` BEFORE the per-format capability gate. The
    // validate() function fail-louds on each violation so a
    // future producer landing malformed items hits a precise
    // diagnostic naming the violation, not a generic "no walker
    // yet" rejection. Order matters: invariant failures point
    // at the producer; the capability gate points at the missing
    // walker. (INVARIANT — multi-agent audit fold pin: this
    // ordering is load-bearing; swapping the two blocks would
    // surface a missing-walker rejection on an already-malformed
    // dataItems payload and obscure the real producer defect.)
    if (!module.dataItems.empty()) {
        bool const dataItemsValid =
            validateAssembledData(module.dataItems, reporter);
        if (!dataItemsValid) {
            image.resolvedFuncCount = 0;
            return image;
        }
    }

    // ── Per-format walker capability gate for `dataItems` ──────
    //
    // **Schema-declared, not enumerated in C++** (D-LK2-RODATA
    // audit fold — agnosticism rule). Each format's JSON ships a
    // `supportedDataSections: ["rodata", ...]` array advertising
    // which `DataSectionKind` values its walker accepts. The gate
    // below consults that set per-item — formats whose walker arm
    // has not landed (ELF + Mach-O until D-LK1-RODATA / D-LK3-
    // RODATA close), AND format flavors that cannot carry rodata
    // (PE Obj — relocatable .obj emits rodata via the symbol
    // table, not via the dataItems pipeline), reject loudly.
    //
    // Adding a fourth executable format that supports rodata =
    // drop `"supportedDataSections": ["rodata"]` into its JSON;
    // zero source changes in this gate. **NO `kind == Pe` style
    // format-name branches here** — the standing veto on source/
    // target/format-name branching in shared substrate is held.
    for (auto const& d : module.dataItems) {
        if (!objectFormatSchema.acceptsDataSection(d.section)) {
            report(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                   DiagnosticSeverity::Error,
                   std::format(
                       "linker: module carries an AssembledData "
                       "item with section={} but format '{}' "
                       "(kind={}) does not advertise that section "
                       "in its 'supportedDataSections' set. Either "
                       "the format's walker arm has not yet closed "
                       "(see plan §3.1: D-LK1-RODATA / D-LK2-RODATA "
                       "/ D-LK3-RODATA / D-LK4-RODATA-PRODUCER) or "
                       "the format flavor cannot carry that "
                       "section (e.g. relocatable .obj).",
                       dataSectionKindName(d.section),
                       objectFormatSchema.name(),
                       objectFormatKindName(
                           objectFormatSchema.kind())));
            image.resolvedFuncCount = 0;
            return image;
        }
    }

    // Format-keyed dispatch — closed-enum switch, fail-loud on
    // any format whose walker hasn't landed yet so the substrate
    // discipline reports the missing walker instead of silently
    // returning empty bytes.
    switch (objectFormatSchema.kind()) {
    case ObjectFormatKind::Unknown:
        report(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
               DiagnosticSeverity::Error,
               "linker: format kind 'unknown' is the invalid sentinel; "
               "the object format schema was constructed without a valid "
               "format declaration");
        break;
    case ObjectFormatKind::Elf:
        image.bytes = elf::encode(module, targetSchema,
                                  objectFormatSchema, reporter);
        break;
    case ObjectFormatKind::Pe:
        image.bytes = pe::encode(module, targetSchema,
                                 objectFormatSchema, reporter);
        break;
    case ObjectFormatKind::MachO:
        image.bytes = macho::encode(module, targetSchema,
                                     objectFormatSchema, reporter);
        break;
    case ObjectFormatKind::Wasm:
        // LK8 skeleton: emits the 8-byte module preamble (magic +
        // version). Plan 18 fills the section emitters; this
        // walker is the substrate plumb-through that proves the
        // format-blind dispatch routes WASM correctly.
        image.bytes = wasm::encode(module, targetSchema,
                                   objectFormatSchema, reporter);
        break;
    case ObjectFormatKind::Spirv:
        // LK9 skeleton: emits the 5-word SPIR-V module header
        // (magic + version + generator + bound + reserved) per
        // SPIR-V Spec §2.3. Plan 17 fills the instruction stream;
        // this walker is the substrate plumb-through that proves
        // the format-blind dispatch routes SPIR-V correctly.
        image.bytes = spirv::encode(module, targetSchema,
                                    objectFormatSchema, reporter);
        break;
    }

    // Post-walker error gate (architect O1 fold from LK6 cycle 2a):
    // a walker-side fail-loud (e.g. K_FormatLacksImportSupport on
    // ELF/MachO when externImports is non-empty) registers errors
    // on the reporter but leaves `resolvedFuncCount` at the value
    // the pre-walker unifier set. Without this gate, `image.ok()`
    // returns true while `image.bytes` is empty — silent contract
    // violation. Same shape as the pre-walker gate above.
    if (reporter.errorCount() != errorsAtEntry) {
        image.resolvedFuncCount = 0;
    }

    return image;
}

} // namespace dss::linker
