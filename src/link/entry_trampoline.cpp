#include "link/entry_trampoline.hpp"

#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <format>
#include <limits>
#include <string>
#include <vector>

namespace dss::linker {

namespace {

// Find the max SymbolId in the module (across functions +
// externImports). Caller mints sequential IDs as `maxV+1`,
// `maxV+2`, ... — re-scanning after each mint would silently
// collide when modules are partially-mutated (the bug caught at
// Slice C build).
[[nodiscard]] std::uint32_t maxExistingSymbolIdV(AssembledModule const& mod) {
    std::uint32_t maxV = 0;
    for (auto const& fn : mod.functions) {
        if (fn.symbol.v > maxV) maxV = fn.symbol.v;
    }
    for (auto const& ext : mod.externImports) {
        if (ext.symbol.v > maxV) maxV = ext.symbol.v;
    }
    // D-LK4-RODATA-PRODUCER-STRING audit-fold (2026-06-02): include
    // rodata items so the trampoline's mint can't collide with a
    // string-literal-promoted MirGlobal that landed in dataItems.
    // The collision would surface as `K_DuplicateDataSymbol` in the
    // PE walker's symbolVa loop, NOT here — silent at this site,
    // loud downstream — but the diagnostic would point at the PE
    // walker, away from the actual root cause (the missing scan).
    for (auto const& d : mod.dataItems) {
        if (d.symbol.v > maxV) maxV = d.symbol.v;
    }
    return maxV;
}

// Resolve user-entry SymbolId from the format's `entryPoint` field.
// Empty entryPoint defaults to `functions[0]` (cycle-2 convention
// shared by all 3 walkers — see pe.cpp / elf.cpp / macho.cpp's
// entry-resolution path). MUST be called BEFORE the trampoline is
// prepended; reading `functions[0]` AFTER prepend would
// self-reference the trampoline.
//
// D-LK10-ENTRY-EXTERN-ENTRY-DIAG: the two failure modes (no match
// at all vs. match resolves to an ExternImport) are semantically
// distinct and the user-visible remediation differs — split into a
// tagged result so the caller emits the precise diagnostic. The
// previous shape (`std::optional<SymbolId>` returning nullopt for
// both cases) hid the distinction and forced a generic combined
// message at the call site.
enum class EntryResolutionStatus : std::uint8_t {
    Found,             // entry resolved to a defined AssembledFunction
    NotFound,          // entryName matched neither a function nor an extern
    ResolvedToExtern,  // entryName matched an ExternImport (invalid)
};

struct EntryResolution {
    EntryResolutionStatus status   = EntryResolutionStatus::NotFound;
    SymbolId              symbol{};  // valid iff status == Found
};

[[nodiscard]] EntryResolution resolveUserEntrySymbol(
        AssembledModule const&    module,
        ObjectFormatSchema const& format) {
    if (module.functions.empty()) {
        return {EntryResolutionStatus::NotFound, SymbolId{}};
    }
    auto const entryName = std::string{format.entryPoint()};
    if (entryName.empty()) {
        return {EntryResolutionStatus::Found,
                module.functions[0].symbol};
    }
    // Walker-side synthesized name convention: `sym_<id>` on
    // ELF/PE; `_sym_<id>` on Mach-O. Match either form (real-name
    // resolution closes with D-LK1-1 / LK7).
    //
    // D-LK10-ENTRY-SYNTH-PREFIX-SCHEMA: the two prefix strings are
    // hardcoded here pending move into the format schema (see plan
    // 14 §3.1). Closure trigger: 4th format declares processExit OR
    // D-LK1-1 lands real-symbol-name preservation through the
    // emit pipeline.
    for (auto const& fn : module.functions) {
        std::string const elfPeName =
            "sym_"  + std::to_string(fn.symbol.v);
        std::string const machoName =
            "_sym_" + std::to_string(fn.symbol.v);
        if (entryName == elfPeName || entryName == machoName) {
            return {EntryResolutionStatus::Found, fn.symbol};
        }
    }
    // entryName matches an ExternImport's synthesized name — the
    // schema authored a format that names an imported symbol as
    // the entry point. Semantically invalid: an extern is a
    // SYMBOL REFERENCE; it has no body to call into. Distinct
    // from the not-found case because the schema author named a
    // KNOWN symbol that's just on the wrong table.
    for (auto const& ext : module.externImports) {
        std::string const elfPeName =
            "sym_"  + std::to_string(ext.symbol.v);
        std::string const machoName =
            "_sym_" + std::to_string(ext.symbol.v);
        if (entryName == elfPeName || entryName == machoName) {
            return {EntryResolutionStatus::ResolvedToExtern,
                    SymbolId{}};
        }
    }
    return {EntryResolutionStatus::NotFound, SymbolId{}};
}

void emit(DiagnosticReporter& rep, DiagnosticCode code, std::string msg) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// Wrap a physical register name lookup. Returns nullopt on miss;
// caller emits the diagnostic.
[[nodiscard]] std::optional<LirReg> physRegByName(
        TargetSchema const& target, std::string_view name) {
    auto const ord = target.registerByName(name);
    if (!ord.has_value()) return std::nullopt;
    auto const* info = target.registerInfo(*ord);
    if (info == nullptr) return std::nullopt;
    return makePhysicalReg(*ord,
                           static_cast<LirRegClass>(info->regClass));
}

} // namespace

bool injectEntryTrampoline(AssembledModule&          module,
                           TargetSchema const&       target,
                           ObjectFormatSchema const& format,
                           DiagnosticReporter&       reporter) {
    auto const& peOpt = format.processExit();
    if (!peOpt.has_value()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("entry-trampoline: format '{}' did not "
                         "declare a `processExit` block — runnable "
                         "binaries require it (D-LK10-ENTRY §2.13 "
                         "plan 14).",
                         std::string{format.name()}));
        return false;
    }
    auto const& pe = *peOpt;

    auto const ccName = std::string{format.entryCallingConvention()};
    if (ccName.empty()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("entry-trampoline: format '{}' declared "
                         "processExit but `entryCallingConvention` "
                         "is empty — Slice C requires the active "
                         "cc to look up argGprs[0] (D-LK10-ENTRY).",
                         std::string{format.name()}));
        return false;
    }
    auto const* cc = target.callingConventionByName(ccName);
    if (cc == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("entry-trampoline: entryCallingConvention "
                         "'{}' does not resolve against target "
                         "'{}' callingConventions[] — typo or "
                         "missing cc declaration on the target.",
                         ccName, std::string{target.name()}));
        return false;
    }
    if (cc->argGprs.empty() || cc->returnGprs.empty()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("entry-trampoline: cc '{}' has empty "
                         "argGprs or returnGprs — trampoline needs "
                         "argGprs[0] (status arg) + returnGprs[0] "
                         "(user fn return).", ccName));
        return false;
    }

    // Look up all needed opcodes from the target schema (Slice A
    // ships `syscall`, `call_indirect_via_extern`, and the existing
    // `call` / `mov` / `unreachable` opcodes). `sub` is required
    // ONLY when alignedSizeWithBias(cc.shadowSpaceBytes,
    // cc.stackAlignment, cc.entryStackPointerBias) > 0 — i.e. when
    // the entry cc declares shadow space OR a non-zero process-
    // entry RSP bias (closes D-LK10-ENTRY-TRAMP-PROLOGUE).
    auto const callOp     = target.opcodeByMnemonic("call");
    auto const movOp      = target.opcodeByMnemonic("mov");
    auto const unreachOp  = target.opcodeByMnemonic("unreachable");
    if (!callOp.has_value() || !movOp.has_value()
     || !unreachOp.has_value()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("entry-trampoline: target '{}' is missing "
                         "one of the required base opcodes "
                         "('call', 'mov', 'unreachable').",
                         std::string{target.name()}));
        return false;
    }

    // Resolve user-entry BEFORE prepend (silent-failure H1 from
    // d642655 audit — reading after prepend would self-reference).
    // D-LK10-ENTRY-EXTERN-ENTRY-DIAG: distinct diagnostics per
    // failure mode — NotFound emits K_SymbolUndefined (the named
    // symbol doesn't exist anywhere in the module);
    // ResolvedToExtern emits K_EntryPointResolvesToExtern (the
    // name resolved to an ExternImport, which is semantically
    // invalid as an entry point — the user almost certainly named
    // the wrong symbol in the format JSON).
    auto const entryRes = resolveUserEntrySymbol(module, format);
    switch (entryRes.status) {
        case EntryResolutionStatus::Found: break;
        case EntryResolutionStatus::ResolvedToExtern:
            emit(reporter,
                 DiagnosticCode::K_EntryPointResolvesToExtern,
                 std::format("entry-trampoline: format '{}' declared "
                             "entryPoint '{}' but that name resolves "
                             "to an ExternImport in the module's "
                             "import table — an extern is a symbol "
                             "REFERENCE to code in another module, "
                             "not a callable definition, so it cannot "
                             "serve as the user entry point. Check "
                             "the format JSON's `entryPoint` field: "
                             "name a declared AssembledFunction, not "
                             "an imported symbol.",
                             std::string{format.name()},
                             std::string{format.entryPoint()}));
            return false;
        case EntryResolutionStatus::NotFound:
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::format("entry-trampoline: format '{}' declared "
                             "entryPoint '{}' but no AssembledFunction "
                             "has the matching synthesized symbol "
                             "name (`sym_<id>` for ELF/PE; "
                             "`_sym_<id>` for Mach-O). Check that the "
                             "user's source declares the named entry "
                             "function and that the SymbolId encoded "
                             "in entryPoint matches.",
                             std::string{format.name()},
                             std::string{format.entryPoint()}));
            return false;
    }
    SymbolId const userEntrySym = entryRes.symbol;

    // Resolve register names from the active cc.
    auto const argRegName    = std::string{cc->argGprs[0]};
    auto const returnRegName = std::string{cc->returnGprs[0]};
    auto const argReg    = physRegByName(target, argRegName);
    auto const returnReg = physRegByName(target, returnRegName);
    if (!argReg.has_value() || !returnReg.has_value()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("entry-trampoline: cc '{}' references "
                         "register '{}' / '{}' that the target "
                         "schema does not declare.",
                         ccName, argRegName, returnRegName));
        return false;
    }

    // Mint trampoline + (ByNameImport) synthetic-extern SymbolIds
    // SEQUENTIALLY (maxV+1, maxV+2, ...). Calling the mint helper
    // twice without intermediate module mutation produces a
    // collision (max() of unchanged input is the same — first bug
    // caught at Slice C build).
    //
    // SymbolId space-exhaustion guard (3-agent convergence:
    // silent-failure + test-analyzer + dim-2 at the Slice C audit
    // fold). `maxV+1` / `maxV+2` are uint32 — at UINT32_MAX they
    // wrap silently to 0/1 (the InvalidSymbol sentinel + low-ID
    // user fns), silently colliding with declared symbols. The
    // BOTH-defined-AND-ExternImport guard at linker.cpp:121 would
    // catch the ByNameImport arm but the pure Syscall arm has no
    // cross-table check. Fail loud HERE before the wrap can
    // corrupt the module.
    std::uint32_t const maxV = maxExistingSymbolIdV(module);
    std::uint32_t const needed =
        (pe.mechanism == ExitMechanism::ByNameImport) ? 2u : 1u;
    if (maxV > std::numeric_limits<std::uint32_t>::max() - needed) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             std::format("entry-trampoline: SymbolId space exhausted "
                         "— module's max SymbolId is {} + {} fresh "
                         "IDs would wrap uint32. Reduce module size "
                         "OR reset CU SymbolId allocator.",
                         maxV, needed));
        return false;
    }
    SymbolId const trampSym{maxV + 1};
    SymbolId exitImportSym{0};
    if (pe.mechanism == ExitMechanism::ByNameImport) {
        // Append the synthetic ExternImport so the PE walker's IAT
        // writer (LK6 cycle 2a) emits the IAT slot. The reloc-apply
        // kernel populates `symbolVa[exitImportSym]` from the IAT
        // slot's VA at link time; the trampoline's
        // `call_indirect_via_extern` patches that VA into its
        // disp32 patch site via the REL32 reloc.
        ExternImport synth;
        synth.symbol      = SymbolId{maxV + 2};
        synth.mangledName = pe.importMangledName;
        synth.libraryPath = pe.importLibraryPath;
        exitImportSym     = synth.symbol;
        module.externImports.push_back(std::move(synth));
    }

    // Build the trampoline as a one-function Lir via LirBuilder.
    LirBuilder b{target};
    (void)b.addFunction(trampSym);
    auto blk = b.createBlock();
    b.beginBlock(blk);

    // 0. ABI prologue (D-LK10-ENTRY-TRAMP-PROLOGUE). Compute the
    //    smallest frame-size adjust satisfying BOTH (a) the cc's
    //    shadow-space requirement and (b) the cc's stack-alignment
    //    at the call sites about to follow, given the process-entry
    //    RSP bias the kernel/loader provides. Algorithm lives ONCE
    //    in `alignedSizeWithBias()` (lir_callconv.hpp) so ML7 and
    //    the trampoline share one source of truth — see header
    //    docblock for the consumers + reasoning.
    //
    //    Result is non-zero only when shadowSpaceBytes != 0 OR the
    //    process-entry RSP is misaligned for the cc (Windows PE:
    //    32+8=40; SysV ELF / Mach-O / ARM64: 0).
    //
    //    No restoration is emitted — the exit mechanism never
    //    returns (the trampoline ends in `unreachable` / `ud2`).
    // Integral promotion: cc fields are uint16_t; the helper takes
    // uint32_t. Explicit casts would obscure that this is widening,
    // not narrowing.
    std::uint32_t const adjustBytes = alignedSizeWithBias(
        cc->shadowSpaceBytes,
        cc->stackAlignment,
        cc->entryStackPointerBias);
    if (adjustBytes > 0) {
        if (!cc->stackPointer.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: cc '{}' has no "
                             "`stackPointer` declared but the computed "
                             "ABI-prologue adjust is {} bytes "
                             "(D-LK10-ENTRY-TRAMP-PROLOGUE). The cc "
                             "must declare its stack-pointer register "
                             "for the trampoline to emit the prologue.",
                             ccName, adjustBytes));
            return false;
        }
        auto const subOp = target.opcodeByMnemonic("sub");
        if (!subOp.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: target '{}' lacks the "
                             "`sub` opcode required by the trampoline "
                             "ABI prologue (D-LK10-ENTRY-TRAMP-PROLOGUE; "
                             "fires when the cc declares shadow space "
                             "OR a non-zero process-entry RSP bias).",
                             std::string{target.name()}));
            return false;
        }
        auto const spReg = makePhysicalReg(cc->stackPointer->ordinal,
                                           LirRegClass::GPR);
        LirOperand const subOps[] = {
            LirOperand::makeReg(spReg),
            LirOperand::makeImmInt32(
                static_cast<std::int32_t>(adjustBytes))
        };
        (void)b.addInst(*subOp, spReg, subOps);
    }

    // 1. call user_entry — produces REL32 reloc on the disp32.
    LirOperand const callOps[] = {
        LirOperand::makeSymbolRef(userEntrySym.v)
    };
    (void)b.addInst(*callOp, InvalidLirReg, callOps);

    // 2. mov argGprs[0], returnGprs[0] — status into syscall/call
    //    arg register from user fn's return register.
    LirOperand const movRegOps[] = {
        LirOperand::makeReg(*returnReg)
    };
    (void)b.addInst(*movOp, *argReg, movRegOps);

    if (pe.mechanism == ExitMechanism::Syscall) {
        // 3. mov syscallNumGpr, syscallNumber — load syscall number.
        auto const syscallNumReg =
            physRegByName(target, pe.syscallNumGpr);
        if (!syscallNumReg.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: processExit."
                             "syscallNumGpr '{}' does not resolve "
                             "against target '{}' registers.",
                             pe.syscallNumGpr,
                             std::string{target.name()}));
            return false;
        }
        LirOperand const movImmOps[] = {
            LirOperand::makeImmInt32(
                static_cast<std::int32_t>(pe.syscallNumber))
        };
        (void)b.addInst(*movOp, *syscallNumReg, movImmOps);

        // 4. syscall — must be declared on the target schema.
        auto const syscallOp = target.opcodeByMnemonic("syscall");
        if (!syscallOp.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: target '{}' lacks "
                             "the `syscall` opcode required by "
                             "Slice A — anchored D-LK10-ENTRY-ARM64.",
                             std::string{target.name()}));
            return false;
        }
        (void)b.addInst(*syscallOp, InvalidLirReg, {});
    } else {  // ByNameImport
        // 3. call_indirect_via_extern <exit_import_symbol>.
        auto const callIndOp =
            target.opcodeByMnemonic("call_indirect_via_extern");
        if (!callIndOp.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: target '{}' lacks "
                             "the `call_indirect_via_extern` "
                             "opcode required by Slice A.",
                             std::string{target.name()}));
            return false;
        }
        LirOperand const exitOps[] = {
            LirOperand::makeSymbolRef(exitImportSym.v)
        };
        (void)b.addInst(*callIndOp, InvalidLirReg, exitOps);
    }

    // 5. unreachable — verifier hint that control never returns
    //    from the exit syscall / indirect call. Encodes to ud2 on
    //    x86_64 (Slice A added the encoding).
    (void)b.addUnreachable(*unreachOp);
    Lir lir = std::move(b).finish();

    // Run the synthetic Lir through `assemble()`. `lirToMir` is a
    // parallel-sized vector of sentinel `MirInstId{}` — the
    // assembler enforces `lirToMir.size() == lir.instCount()` at
    // entry (`asm.cpp:167`), but the synthetic Lir has no MIR
    // provenance to attribute. Anchored D-LK10-ENTRY-LIRTOMIRSENTINEL
    // (factor an `assembleHandBuilt()` wrapper at 2nd synthetic-Lir
    // caller).
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto result = assemble(lir, target, lirToMir, reporter);
    if (result.functions.empty()) {
        // assemble() already emitted diagnostics; surface nothing
        // new here so error wording stays single-sourced.
        return false;
    }

    // Move the assembled trampoline into a fresh `AssembledFunction`
    // and prepend it. The assembler's output for our 1-function Lir
    // is a 1-function AssembledModule; we steal the function row.
    AssembledFunction tramp = std::move(result.functions[0]);
    tramp.symbol = trampSym;

    // Silent-failure HIGH (Slice C audit fold): empty-bytes reject.
    // A target schema declaring `syscall` mnemonic but lacking the
    // encoding bytes would silently emit a 0-byte trampoline → the
    // walker emits an executable whose `_start` is empty → SEGV at
    // OS entry. The acceptance test catches this on Windows but
    // cross-host structural tests would not.
    if (tramp.bytes.empty()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("entry-trampoline: assemble() returned a "
                         "0-byte trampoline for target '{}' — at "
                         "least one of the Slice A opcodes ('call', "
                         "'mov', 'syscall', 'call_indirect_via_extern',"
                         " 'unreachable') is declared without an "
                         "encoding row. Check the target schema's "
                         "opcode encoding blocks.",
                         std::string{target.name()}));
        return false;
    }

    module.functions.insert(module.functions.begin(), std::move(tramp));
    // code-architect FOLD-NOW (Slice C audit fold): unconditional
    // increment. The previous `if (expectedFuncCount > 0)` guard
    // had a latent landmine — a module with expectedFuncCount=0
    // (default-constructed; or a future caller path that doesn't
    // populate it) would leave the field at 0 after prepend,
    // making `LinkedImage::ok()` return false for a structurally
    // valid trampolined module. The linker's `wantTrampoline`
    // guard (linker.cpp) requires !functions.empty() which implies
    // expectedFuncCount >= 1 from the assembler, so the new
    // unconditional form is safe. Pre-condition assert documents
    // the invariant the linker hook enforces.
    if (module.expectedFuncCount == 0) {
        // Defense-in-depth: if a future caller path injects on a
        // module with expectedFuncCount=0, that's a substrate-shape
        // violation. Emit + reject rather than silently incrementing
        // into a still-broken ok()-state.
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "entry-trampoline: module.expectedFuncCount==0 before "
             "trampoline prepend — caller must ensure the input "
             "module is fully-assembled (assemble() populates this) "
             "before invoking injectEntryTrampoline.");
        // Roll back the prepend to keep the module consistent.
        module.functions.erase(module.functions.begin());
        return false;
    }
    ++module.expectedFuncCount;
    module.imageEntryOverride = std::optional<std::size_t>{0};
    return true;
}

} // namespace dss::linker
