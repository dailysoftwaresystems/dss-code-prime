#include "link/entry_trampoline.hpp"
#include "link/fresh_symbol_ids.hpp"   // maxExistingSymbolIdV — shared with linker.cpp
#include "link/static_init_tables.hpp"

#include "core/types/config_key_vocabulary.hpp"  // renderAllowedList — the ONE closed-set renderer
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"     // kExternCallDispatchTable
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <algorithm>   // std::min -- the argument-register park
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>   // std::pair -- the parked (saved, original) pairs
#include <vector>

namespace dss::linker {

namespace {

// ⓘ `maxExistingSymbolIdV` USED TO LIVE HERE and is now
// `link/fresh_symbol_ids.hpp`, unchanged in behaviour. It gained a SECOND
// caller — the object-carried data-import slot mint
// (D-LK-PE-OBJECT-WEAK-DATA-EXTERN-REL32-TO-AN-ABSOLUTE-TARGET) — and the
// scan has already been widened twice by a collision that reached a walker
// (dataItems, then blockSymbols), so a copy would have inherited the old rule
// and failed the same way from the other pass. The two widenings and the
// sequential-mint caller contract are recorded in full at the new home.

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
    // D-CSUBSET-MULTI-FN-WIN64-CC (step 13.5 cycle 2 post-fold,
    // 2026-06-03): when the compile pipeline plumbed an explicit
    // user-entry symbol from the source language's entry-function
    // name config (c's `implicitReturnZeroForFunctionNames`),
    // verify it resolves to a defined function in the module and
    // use it. Pre-fix, multi-function modules whose entry wasn't
    // declared first in source order silently called the wrong
    // function (the first-declared one was picked as `functions[0]`
    // via the empty-entryPoint fallback below).
    if (module.userEntrySymbol.has_value()) {
        SymbolId const want = *module.userEntrySymbol;
        for (auto const& fn : module.functions) {
            if (fn.symbol.v == want.v) {
                return {EntryResolutionStatus::Found, fn.symbol};
            }
        }
        for (auto const& ext : module.externImports) {
            if (ext.symbol.v == want.v) {
                return {EntryResolutionStatus::ResolvedToExtern,
                        SymbolId{}};
            }
        }
        return {EntryResolutionStatus::NotFound, SymbolId{}};
    }
    auto const entryName = std::string{format.entryPoint()};
    if (entryName.empty()) {
        // Silent-failure HIGH #3 + code-architect Q5 post-fold
        // (2026-06-03): the `functions[0]` fallback is silently
        // wrong-direction for multi-function modules whose entry
        // isn't declared first in source order. The corpus expansion
        // surfaced this via `int helper(int x){return x;}; int
        // main(){return helper(42);}` returning 0 (the trampoline
        // called helper). Single-function modules are unambiguous
        // — keep the silent fallback there (the lone function IS
        // the entry by construction). Multi-function modules
        // without an explicit userEntrySymbol or format.entryPoint
        // are a SUBSTRATE BUG: the compile pipeline failed to
        // stamp userEntrySymbol, OR the language config didn't
        // declare its entry-function name. Fail-loud rather than
        // silently invoke the first-declared function.
        if (module.functions.size() > 1) {
            return {EntryResolutionStatus::NotFound, SymbolId{}};
        }
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
    // ★ THIS CHECK WAS DEAD CODE BY CONSTRUCTION until the
    // D-LK10-ENTRY entry-gate fold. Its only caller
    // (`linker::link`) gated the call on `processExit().has_value()`
    // — the SAME predicate — so a format lacking `processExit` never
    // reached here; the linker just skipped injection with no
    // diagnostic at all. The caller now asks the schema's
    // `isExecFlavor()` instead and refuses separately
    // (K_FormatLacksProcessExit, linker.cpp), which makes this a
    // genuine backstop for the DIRECT-call path: tests and future
    // callers invoke `injectEntryTrampoline` without going through
    // `link` (10+ sites in tests/link/test_lk10_entry_slice_c.cpp
    // alone).
    //
    // The code is `K_FormatLacksProcessExit`, the SAME code the
    // linker gate and the walker-tier resolver fire: one fault class
    // — "the emitted entry would have no process-exit path" — must
    // not present under three different codes depending on which
    // tier noticed. (Was `K_NoMatchingObjectFormat`, a generic
    // catch-all that said nothing about the missing key; no test
    // pinned it here — VERIFIED by grepping every
    // `injectEntryTrampoline` call site, none of which passes a
    // format without `processExit`.)
    auto const& peOpt = format.processExit();
    if (!peOpt.has_value()) {
        emit(reporter, DiagnosticCode::K_FormatLacksProcessExit,
             std::format("entry-trampoline: format '{}' did not "
                         "declare a `processExit` block — DSS always "
                         "synthesises an entry trampoline on an "
                         "exec-flavored format and this one declares "
                         "no mechanism for the trampoline to call "
                         "(D-LK10-ENTRY §2.13 plan 14).",
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
    // BOTH-defined-AND-ExternImport guard — `buildCompoundIndex`'s
    // duplicate-compound-key `declare()` gate in `linker.cpp` — would
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

    // -1. Program-entry argument materialization
    //     (D-RUNTIME-MAIN-ARGC-ARGV, c88). When the format declares a
    //     `processArgs` mechanism, load argc/argv into the entry cc's
    //     first two integer argument registers so
    //     `int main(int argc, char** argv)` sees real values instead
    //     of process-entry register garbage (the c87-witnessed
    //     argc=846361312 class that crashed the sqlite3 shell inside
    //     main). Emitted FIRST — before the ABI prologue's SP adjust —
    //     because the StackVector offsets are defined against the
    //     UNTOUCHED process-entry stack pointer (SysV AMD64 psABI
    //     §3.4.1 / AAPCS64 Linux: [SP]=argc, [SP+8]=argv[0], in
    //     place). Signature-independent: a `main(void)` simply never
    //     reads the two registers (C-legal), so the whole corpus is
    //     unaffected. A format WITHOUT `processArgs` emits nothing
    //     here — Mach-O's LC_MAIN entry already receives argc/argv in
    //     these registers from dyld (pass-through is the correct
    //     mechanism there); PE's CRT-accessor route is
    //     D-FFI-PE-CRT-UCRT-MIGRATION's `crt-argv-accessors`.
    auto const& paOpt = format.processArgs();
    // UCRT-P4 (was c111): the CRT mechanisms perform their argc/argv setup in a
    // MIR-tier SYNTHESIZED pre-main init (`realizeEntryShape`) — the program entry
    // was already retargeted to that synth function, which fetches the args via the
    // CRT exports and forwards (argc, argv) to the user entry through a
    // normally-lowered call. So the trampoline emits NO argument materialization for
    // it here; it just calls the (synth) entry below. Every OTHER declared mechanism
    // still flows through the closed-enum block (the StackVector emitter + the
    // fail-loud "no arm" reject), preserving the "a new ArgsMechanism member must add
    // an emitter arm" discipline.
    bool const argsHandledBySynthInit =
        paOpt.has_value() && paOpt->mechanism == ArgsMechanism::CrtArgvAccessors;
    if (paOpt.has_value() && !argsHandledBySynthInit) {
        auto const& pa = *paOpt;
        if (pa.mechanism != ArgsMechanism::StackVector) {
            // Closed-enum discipline: a new ArgsMechanism member must
            // add its emitter arm HERE — never silently skip setup.
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: format '{}' declares "
                             "processArgs.mechanism '{}' but the "
                             "trampoline emitter has no arm for it — "
                             "argument setup would be silently skipped "
                             "(D-RUNTIME-MAIN-ARGC-ARGV).",
                             std::string{format.name()},
                             argsMechanismName(pa.mechanism)));
            return false;
        }
        if (cc->argGprs.size() < 2) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: cc '{}' declares {} "
                             "argGprs but the stack-vector processArgs "
                             "mechanism needs TWO (argc + argv "
                             "destinations).",
                             ccName, cc->argGprs.size()));
            return false;
        }
        if (!cc->stackPointer.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: cc '{}' has no "
                             "`stackPointer` declared but the "
                             "stack-vector processArgs mechanism reads "
                             "argc/argv relative to it "
                             "(D-RUNTIME-MAIN-ARGC-ARGV).", ccName));
            return false;
        }
        auto const loadOp = target.opcodeByMnemonic("load");
        auto const leaOp  = target.opcodeByMnemonic("lea");
        if (!loadOp.has_value() || !leaOp.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: target '{}' lacks the "
                             "`load` / `lea` opcode required by the "
                             "stack-vector processArgs mechanism "
                             "(argc = load [sp+{}], argv = lea "
                             "[sp+{}]) (D-RUNTIME-MAIN-ARGC-ARGV).",
                             std::string{target.name()},
                             pa.argcStackOffset, pa.argvStackOffset));
            return false;
        }
        auto const argvRegName = std::string{cc->argGprs[1]};
        auto const argvReg     = physRegByName(target, argvRegName);
        if (!argvReg.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: cc '{}' references "
                             "register '{}' (argGprs[1], the argv "
                             "destination) that the target schema "
                             "does not declare.",
                             ccName, argvRegName));
            return false;
        }
        auto const spReg = makePhysicalReg(cc->stackPointer->ordinal,
                                           LirRegClass::GPR);
        // argc: a full-machine-word load — the kernel stores argc as
        // a word-sized value; the callee's `int` parameter reads the
        // low 32 bits (value-correct, argc is non-negative and far
        // below 2^31). Shared 3-op LIR load form
        // [base, MemBase(scale=1), MemOffset(disp)] — encodes as
        // x86_64 `mov r64, [rsp+disp32]` / AArch64 `LDUR Xt, [SP,#d]`.
        LirOperand const argcOps[] = {
            LirOperand::makeReg(spReg),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(
                static_cast<std::int32_t>(pa.argcStackOffset)),
        };
        (void)b.addInst(*loadOp, *argReg, argcOps);
        // argv: the NULL-terminated pointer vector lives IN PLACE on
        // the entry stack — its ADDRESS is the argv value (no copy
        // exists anywhere else), so this is an effective-address
        // computation, never a dereference. Same 3-op form via `lea`
        // — x86_64 `lea r64, [rsp+disp32]` / AArch64
        // `ADD Xd, SP, #imm12`.
        LirOperand const argvOps[] = {
            LirOperand::makeReg(spReg),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(
                static_cast<std::int32_t>(pa.argvStackOffset)),
        };
        (void)b.addInst(*leaOp, *argvReg, argvOps);
    }

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

    // ── D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN ─────────
    //
    // THE STATIC-INITIALIZER CALLS. DSS links no crt — this trampoline IS the
    // program's runtime — so on every format whose document names
    // `entryTrampoline` as the runner, these calls are the only thing that makes
    // `__attribute__((constructor))` mean anything.
    //
    // ★★ THE ARM IS CHOSEN BY THE FORMAT DOCUMENT, NEVER BY A FORMAT NAME, and
    // the other direction is a real platform behaviour rather than a placeholder:
    // on the `imageLoader` arm the platform loader walks a section it recognizes
    // (dyld and `S_MOD_INIT_FUNC_POINTERS`), so emitting calls here would run
    // every initializer TWICE. No shipped document selects that arm today —
    // selecting it needs a writer that emits such a section, which is why
    // `StaticInitSchedule.ImageLoaderRunnerEmitsNoTrampolineCalls` is the only
    // thing standing between this branch and silent deletion.
    //
    // ★ A MODULE WITH NO RUNNER CANNOT REACH THIS POINT CARRYING A SCHEDULE: the
    // linker refuses that program before injection, naming the format. So the
    // `false` arm below means "the loader owns it", never "nobody does".
    //
    // ★ DIRECT CALLS, AND NO EMITTED TABLE AT ALL. The linker knows the whole
    // program here, so the sequence is a straight line of `call`s — no loop, no
    // indirect call through a register, no synthesized boundary symbols, and
    // nothing for a `.init_array` to be read by (see `static_init_tables.hpp` for
    // the measurement that settles it).
    bool const runsSchedule =
        format.staticInitRunner() == StaticInitRunner::EntryTrampoline;
    auto const beforeEntry =
        runsSchedule ? staticInitOrder(module, StaticInitPhase::BeforeEntry)
                     : std::vector<StaticInitOrderEntry>{};
    auto const afterEntry =
        runsSchedule ? staticInitOrder(module, StaticInitPhase::AfterEntry)
                     : std::vector<StaticInitOrderEntry>{};

    // The convention's own callee-saved GENERAL-PURPOSE registers, handed out in
    // declaration order. Every value the trampoline must carry ACROSS a call goes
    // in one of these, and none of them is named in this file.
    //
    // ⚠ FILTERED BY REGISTER CLASS. `calleeSaved` is not GPR-only on every
    // convention (MEASURED: `ms_x64` lists rbx…r15 AND xmm6…xmm15), so taking
    // entries blindly would work on SysV and hand back an XMM register on another
    // table — a `mov` between register files, or a refused encoding.
    std::size_t nextCalleeSaved = 0;
    auto const takeCalleeSavedGpr = [&]() -> std::optional<LirReg> {
        while (nextCalleeSaved < cc->calleeSaved.size()) {
            auto const& rn  = cc->calleeSaved[nextCalleeSaved++];
            auto const  ord = target.registerByName(rn);
            if (!ord.has_value()) continue;
            auto const* info = target.registerInfo(*ord);
            if (info == nullptr || info->regClass != TargetRegClass::GPR) continue;
            if (auto r = physRegByName(target, rn); r.has_value()) return r;
        }
        return std::nullopt;
    };

    // ★★ THE PROGRAM'S ARGUMENTS HAVE TO SURVIVE THE BEFORE-ENTRY CALLS, and
    // this is the half that is easy to miss. The argument registers were loaded
    // ABOVE — before the prologue, because the stack offsets are defined against
    // the untouched process-entry SP — and they are CALLER-saved on every
    // convention here. A `call` to an initializer therefore hands
    // `main(int argc, char **argv)` whatever that initializer left behind.
    //
    // ★ IT IS NOT CONFINED TO FORMATS THAT DECLARE `processArgs`. On the
    // pass-through arm (Mach-O, where dyld already delivers argc/argv in the
    // argument registers) the trampoline never touches them — so the clobber is
    // identical, and the park is gated on there BEING calls, not on how the
    // registers came to hold their values.
    std::vector<std::pair<LirReg, LirReg>> parkedArgs;   // (saved, original)
    if (!beforeEntry.empty()) {
        std::size_t const nArgs = std::min<std::size_t>(2, cc->argGprs.size());
        for (std::size_t i = 0; i < nArgs; ++i) {
            auto const orig = physRegByName(target, cc->argGprs[i]);
            if (!orig.has_value()) continue;
            auto const save = takeCalleeSavedGpr();
            if (!save.has_value()) {
                emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("entry-trampoline: calling convention '{}' does "
                                 "not declare enough callee-saved GENERAL-PURPOSE "
                                 "registers to carry the program's arguments "
                                 "across the before-entry static initializers "
                                 "this program schedules. Widen the target's "
                                 "`calleeSaved`, or `main(argc, argv)` would "
                                 "receive whatever the last initializer left in "
                                 "the argument registers. "
                                 "(D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN.)",
                                 ccName));
                return false;
            }
            LirOperand const saveOps[] = { LirOperand::makeReg(*orig) };
            (void)b.addInst(*movOp, *save, saveOps);
            parkedArgs.emplace_back(*save, *orig);
        }
    }

    // 0. call each before-entry initializer, in schedule order. Emitted AFTER the
    //    ABI prologue above — these are ordinary calls and need the shadow space
    //    and the alignment bias the prologue established, exactly as the user
    //    entry does.
    for (auto const& e : beforeEntry) {
        LirOperand const ctorOps[] = { LirOperand::makeSymbolRef(e.symbol.v) };
        (void)b.addInst(*callOp, InvalidLirReg, ctorOps);
    }

    // …and put the arguments back, immediately before the entry call.
    for (auto const& [save, orig] : parkedArgs) {
        LirOperand const restoreOps[] = { LirOperand::makeReg(save) };
        (void)b.addInst(*movOp, orig, restoreOps);
    }

    // 1. call user_entry — produces REL32 reloc on the disp32.
    LirOperand const callOps[] = {
        LirOperand::makeSymbolRef(userEntrySym.v)
    };
    (void)b.addInst(*callOp, InvalidLirReg, callOps);

    // ★★ THE STATUS HAS TO SURVIVE THE AFTER-ENTRY CALLS, and the return register
    // will not: it is caller-saved on every convention here, so the first
    // destructor would clobber the program's exit code with whatever it returned.
    // Park it in the convention's OWN first callee-saved GPR — declared config,
    // never a register name in this file — and read it back afterwards.
    //
    // ⚠ FILTERED BY REGISTER CLASS. `calleeSaved` is not GPR-only on every
    // convention (MEASURED: `ms_x64` lists rbx…r15 AND xmm6…xmm15), so taking
    // `calleeSaved[0]` blindly would work on SysV and pick an XMM register on
    // some other table — a `mov` between register files, or a refused encoding.
    std::optional<LirReg> statusSaveReg;
    if (!afterEntry.empty()) {
        statusSaveReg = takeCalleeSavedGpr();
        if (!statusSaveReg.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: calling convention '{}' does not "
                             "declare enough callee-saved GENERAL-PURPOSE "
                             "registers, so the entry function's status cannot "
                             "survive the after-entry static initializers this "
                             "program schedules. Widen the target's "
                             "`calleeSaved`, or the exit code would be whatever "
                             "the last destructor happened to leave behind. "
                             "(D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN.)",
                             ccName));
            return false;
        }
        LirOperand const saveOps[] = { LirOperand::makeReg(*returnReg) };
        (void)b.addInst(*movOp, *statusSaveReg, saveOps);
        for (auto const& e : afterEntry) {
            LirOperand const dtorOps[] = { LirOperand::makeSymbolRef(e.symbol.v) };
            (void)b.addInst(*callOp, InvalidLirReg, dtorOps);
        }
    }

    // 2. mov argGprs[0], returnGprs[0] — status into syscall/call
    //    arg register from user fn's return register (or from the callee-saved
    //    register it was parked in while the after-entry initializers ran).
    LirOperand const movRegOps[] = {
        LirOperand::makeReg(statusSaveReg.has_value() ? *statusSaveReg
                                                      : *returnReg)
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
        // 3. Call the exit import. The CALL-SITE SHAPE is the ACTIVE
        //    OBJECT FORMAT's, exactly as MIR→LIR `lowerCall` picks it
        //    for user-level FFI calls — both consult the SAME rule
        //    (`externCallUsesIndirectShape`, object_format_kind.hpp) so
        //    the trampoline can never drift to the opposite shape from
        //    the FFI path (D-FFI-EXTERN-CALL-DISPATCH; the wrong shape
        //    SIGSEGVs). An `indirect-slot` format (PE IAT / Mach-O
        //    __got) DEREFERENCES the import's pointer slot via
        //    `call_indirect_via_extern` (FF 15); a `direct-plt` format
        //    (ELF / Mach-O — symbolVa points at the linker's stub)
        //    makes a PLAIN DIRECT `call` (E8 / BL) to that stub, which
        //    performs the GOT indirection itself. The synthetic
        //    ExternImport appended above means this module HAS an
        //    extern, so a format with no declared dispatch model is a
        //    fail-loud (no silent default to either shape).
        auto const dispatch = format.externCallDispatch();
        if (!dispatch.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 // The accepted set is PROJECTED from the vocabulary's own
                 // table, never retyped beside the check — the same fact had
                 // three owners across this file, the MIR→LIR gate and the
                 // format loader
                 // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
                 std::format("entry-trampoline: format '{}' uses a "
                             "by-name-import exit but declares no "
                             "`externCallDispatch` — the exit call "
                             "has no defined call-site shape. Declare "
                             "`externCallDispatch` ({}) in the format JSON "
                             "(D-FFI-EXTERN-CALL-DISPATCH).",
                             std::string{format.name()},
                             detail::renderAllowedList(
                                 allNames(kExternCallDispatchTable), " or ")));
            return false;
        }
        bool const useIndirect = externCallUsesIndirectShape(*dispatch);
        // direct-plt reuses the universal `call` opcode resolved above;
        // indirect-slot needs the format-specific indirect-call opcode.
        std::optional<std::uint16_t> const exitCallOp = useIndirect
            ? target.opcodeByMnemonic("call_indirect_via_extern")
            : callOp;
        if (!exitCallOp.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("entry-trampoline: target '{}' lacks the "
                             "`{}` opcode required to lower the "
                             "by-name-import exit call under the "
                             "format's `{}` dispatch model "
                             "(D-FFI-EXTERN-CALL-DISPATCH).",
                             std::string{target.name()},
                             useIndirect ? "call_indirect_via_extern"
                                         : "call",
                             externCallDispatchName(*dispatch)));
            return false;
        }
        LirOperand const exitOps[] = {
            LirOperand::makeSymbolRef(exitImportSym.v)
        };
        (void)b.addInst(*exitCallOp, InvalidLirReg, exitOps);
    }

    // 5. unreachable — verifier hint that control never returns
    //    from the exit syscall / indirect call. Encodes to ud2 on
    //    x86_64 (Slice A added the encoding).
    (void)b.addUnreachable(*unreachOp);
    Lir lir = std::move(b).finish();

    // Run the synthetic Lir through `assemble()`. `lirToMir` is a
    // parallel-sized vector of sentinel `MirInstId{}` — the
    // assembler enforces `lirToMir.size() == lir.instCount()` at
    // entry (the `A_LirToMirSizeMismatch` check in
    // `asm.cpp`), but the synthetic Lir has no MIR
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
