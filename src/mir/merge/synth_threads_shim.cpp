#include "mir/merge/synth_threads_shim.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"       // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"   // rederiveStructCfMarkers (Cycle-2 thrd_join multi-block)
#include "opt/passes/mir_rebuild_helper.hpp"

#include <algorithm>   // std::max, std::sort
#include <array>
#include <cstdint>
#include <optional>    // std::optional (the once-adapter symbol, minted iff call_once present)
#include <string>
#include <unordered_map>
#include <utility>     // std::move, std::pair
#include <vector>

namespace dss {

namespace {

// A verbatim clone policy (synthesizePeStartup's IdentityClonePolicy) — keep every
// block so each existing function is re-added unchanged; the shim functions are then
// appended after the clone loop.
class IdentityClonePolicy final : public opt::passes::MirRebuildPolicy {
public:
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::vector<MirBlockId> blocks;
        std::uint32_t const n = src.funcBlockCount(fn);
        blocks.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) blocks.push_back(src.funcBlockAt(fn, i));
        return blocks;
    }
};

void emitErr(DiagnosticReporter& rep, std::string msg) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// Max SymbolId.v across every defined function, module global, and extern import — the
// floor for minting fresh kernel32-helper symbols (mirrors synthesizePeStartup's
// maxSymbolIdV; the globals scan is load-bearing — synthetic string-literal globals
// hold the highest ids).
[[nodiscard]] std::uint32_t
maxSymbolIdV(Mir const& mir, std::vector<ExternImport> const& externs) {
    std::uint32_t maxV = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i)
        maxV = std::max(maxV, mir.funcSymbol(mir.funcAt(i)).v);
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i)
        maxV = std::max(maxV, mir.globalSymbol(mir.globalAt(i)).v);
    for (auto const& e : externs) maxV = std::max(maxV, e.symbol.v);
    return maxV;
}

} // namespace

bool synthesizeThreadsShim(
    Mir&                                                  mir,
    TypeInterner&                                         interner,
    std::unordered_map<std::uint32_t, std::string> const& recipeBySymbol,
    std::vector<ExternImport>&                            externImports,
    DiagnosticReporter&                                   reporter) {
    // Presence gate: no tagged shim symbol ⇒ clean no-op (every elf/macho + every
    // non-threads pe TU). Keys on the map (a data property), never a format check.
    if (recipeBySymbol.empty()) return true;

    // A DETERMINISTIC emission order (unordered_map iteration is not stable — a shifting
    // function order would make the binary non-reproducible). Sort by pre-minted
    // SymbolId.v.
    std::vector<std::pair<std::uint32_t, std::string>> recipes(
        recipeBySymbol.begin(), recipeBySymbol.end());
    std::sort(recipes.begin(), recipes.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    // ── Types (interned once; pe64 shapes) ──
    TypeId const voidTy = interner.primitive(TypeKind::Void);
    TypeId const i32Ty  = interner.primitive(TypeKind::I32);
    TypeId const u32Ty  = interner.primitive(TypeKind::U32);
    TypeId const i64Ty  = interner.primitive(TypeKind::I64);   // Cycle 2: PtrToInt(handle/res) compares
    TypeId const u64Ty  = interner.primitive(TypeKind::U64);   // Cycle 2: CreateThread dwStackSize
    TypeId const boolTy = interner.primitive(TypeKind::Bool);
    TypeId const pVoid  = interner.pointer(voidTy);   // ptr<void> (mtx_t*/cnd_t*/HANDLE)
    TypeId const pU32   = interner.pointer(u32Ty);    // tss_t* (u32*) / GetExitCodeThread LPDWORD
    TypeId const pI32   = interner.pointer(i32Ty);    // Cycle 2: thrd_join int* res

    // ── kernel32 helper signatures (CcMS64; the FnSig CC is documentary — the MS-x64
    //    ABI is applied downstream by the target's callingConventionIndex, not keyed
    //    here) ──
    auto sig = [&](std::vector<TypeId> params, TypeId ret) -> TypeId {
        return interner.fnSig(params, ret, CallConv::CcMS64);
    };
    TypeId const hSig_v_pV        = sig({pVoid}, voidTy);                 // Init/Enter/Leave/Delete CS; cond-var Init/Wake/WakeAll
    TypeId const hSig_i32_pV      = sig({pVoid}, i32Ty);                  // TryEnterCriticalSection / CloseHandle
    TypeId const hSig_i32_pVpVu32 = sig({pVoid, pVoid, u32Ty}, i32Ty);   // SleepConditionVariableCS
    TypeId const hSig_u32_pV      = sig({pVoid}, u32Ty);                  // FlsAlloc(dtor)
    TypeId const hSig_pV_u32      = sig({u32Ty}, pVoid);                  // FlsGetValue
    TypeId const hSig_i32_u32pV   = sig({u32Ty, pVoid}, i32Ty);          // FlsSetValue
    TypeId const hSig_i32_u32     = sig({u32Ty}, i32Ty);                 // FlsFree
    TypeId const hSig_i32_void    = sig({}, i32Ty);                      // SwitchToThread
    TypeId const hSig_v_u32       = sig({u32Ty}, voidTy);                // ExitThread
    TypeId const hSig_pV_void     = sig({}, pVoid);                      // GetCurrentThread
    // Cycle 2 (D-CSUBSET-C11-THREADS-TRAMPOLINES) kernel32 helper signatures (match
    // windows.json exactly). CreateThread/WaitForSingleObject/GetExitCodeThread/
    // CloseHandle(=hSig_i32_pV) drive thrd_create/thrd_join; InitOnceExecuteOnce drives
    // call_once.
    TypeId const hSig_CreateThread = sig({pVoid, u64Ty, pVoid, pVoid, u32Ty, pVoid}, pVoid);
    TypeId const hSig_u32_pVu32    = sig({pVoid, u32Ty}, u32Ty);         // WaitForSingleObject
    TypeId const hSig_i32_pVpU32   = sig({pVoid, pU32}, i32Ty);          // GetExitCodeThread(HANDLE,LPDWORD)
    TypeId const hSig_i32_4pV      = sig({pVoid, pVoid, pVoid, pVoid}, i32Ty); // InitOnceExecuteOnce

    // ── shim (recipe) signatures (CcMS64; the pe thrd_t is ptr<void>) ──
    TypeId const rSig_mtx_init  = sig({pVoid, i32Ty}, i32Ty);
    TypeId const rSig_i32_pV    = sig({pVoid}, i32Ty);        // mtx_lock/unlock/trylock, cnd_init/signal/broadcast, thrd_detach
    TypeId const rSig_v_pV      = sig({pVoid}, voidTy);       // mtx_destroy, cnd_destroy
    TypeId const rSig_cnd_wait  = sig({pVoid, pVoid}, i32Ty);
    TypeId const rSig_tss_create= sig({pU32, pVoid}, i32Ty);
    TypeId const rSig_pV_u32    = sig({u32Ty}, pVoid);        // tss_get
    TypeId const rSig_i32_u32pV = sig({u32Ty, pVoid}, i32Ty); // tss_set
    TypeId const rSig_v_u32     = sig({u32Ty}, voidTy);       // tss_delete
    TypeId const rSig_pV_void   = sig({}, pVoid);             // thrd_current
    TypeId const rSig_v_void    = sig({}, voidTy);            // thrd_yield
    TypeId const rSig_v_i32     = sig({i32Ty}, voidTy);       // thrd_exit
    // Cycle 2 recipe signatures (the pe thrd_t is ptr<void>=HANDLE).
    TypeId const rSig_thrd_create = sig({pVoid, pVoid, pVoid}, i32Ty);  // (thrd_t*, start, arg)->int
    TypeId const rSig_thrd_join   = sig({pVoid, pI32}, i32Ty);          // (thrd_t, int*)->int
    TypeId const rSig_call_once   = sig({pVoid, pVoid}, voidTy);        // (once_flag*, void(*)(void))->void
    // The module-scoped InitOnceExecuteOnce adapter (PINIT_ONCE_FN shape, BOOL return).
    TypeId const onceTrampSig     = sig({pVoid, pVoid, pVoid}, i32Ty);  // (InitOnce*, param, ctx*)->BOOL

    // ── Rebuild the module (Mir is frozen): clone every existing function verbatim,
    //    then APPEND each shim function, then clone globals — the shared rebuild idiom. ──
    MirBuilder builder;
    IdentityClonePolicy policy;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        opt::passes::MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(mir.funcAt(i));
    }

    // Floor for fresh kernel32-helper symbols: above every existing symbol AND every
    // pre-minted shim symbol (which is NOT yet a defined function / global / extern, so
    // maxSymbolIdV would miss it → a fresh helper could collide with a shim id).
    std::uint32_t nextSymV = maxSymbolIdV(mir, externImports);
    for (auto const& [symV, _] : recipes) nextSymV = std::max(nextSymV, symV);

    // Cycle 2 (D-CSUBSET-C11-THREADS-TRAMPOLINES): call_once passes InitOnceExecuteOnce a
    // PINIT_ONCE_FN(InitOnce*, param, ctx*)->BOOL, but C11's callback is a bare
    // void(*)(void) with a DIFFERENT arg shape + no return, so we synthesize ONE
    // module-scoped adapter and address-take it. Mint its symbol here (above every
    // existing + recipe id) iff a call_once recipe is present; the kernel32-helper
    // importOf draws from the SAME monotonic `nextSymV` afterward, so no id collides.
    std::optional<SymbolId> onceTrampSym;
    for (auto const& [symV, recipe] : recipes)
        if (recipe == "call_once") { onceTrampSym = SymbolId{++nextSymV}; break; }

    // On-demand kernel32 import, deduped by mangledName. Seed from the existing imports
    // so a TU that ALSO `#include`s <windows.h> (which eagerly imports the cond-var / CS
    // family) reuses that symbol instead of planting a duplicate extern (the LK6 linker
    // rejects extern-table duplicates). Returns a GlobalAddr of the helper for a Call.
    std::unordered_map<std::string, SymbolId> helperSyms;
    for (auto const& e : externImports)
        if (!e.isData) helperSyms.emplace(e.mangledName, e.symbol);
    auto importOf = [&](std::string const& name, TypeId helperSig) -> MirInstId {
        SymbolId hs;
        if (auto it = helperSyms.find(name); it != helperSyms.end()) {
            hs = it->second;
        } else {
            hs = SymbolId{++nextSymV};
            helperSyms.emplace(name, hs);
            ExternImport imp;
            imp.symbol      = hs;
            imp.mangledName = name;
            imp.libraryPath = "kernel32.dll";
            imp.isData      = false;   // a FUNCTION import
            externImports.push_back(std::move(imp));
        }
        return builder.addGlobalAddr(hs, interner.pointer(helperSig));
    };

    auto konst = [&](std::int64_t v, TypeKind core, TypeId ty) -> MirInstId {
        MirLiteralValue lit;
        lit.value = v;
        lit.core  = core;
        return builder.addConst(std::move(lit), ty);
    };
    auto const i32c = [&](std::int32_t v) { return konst(v, TypeKind::I32, i32Ty); };
    // Call helpers — operands are [callee, args...]; a void call takes InvalidType.
    auto call0 = [&](char const* n, TypeId hs, TypeId ret) {
        std::array<MirInstId, 1> ops{importOf(n, hs)};
        return builder.addInst(MirOpcode::Call, ops, ret, /*payload=*/0);
    };
    auto call1 = [&](char const* n, TypeId hs, TypeId ret, MirInstId a) {
        std::array<MirInstId, 2> ops{importOf(n, hs), a};
        return builder.addInst(MirOpcode::Call, ops, ret, /*payload=*/0);
    };
    auto call2 = [&](char const* n, TypeId hs, TypeId ret, MirInstId a, MirInstId b) {
        std::array<MirInstId, 3> ops{importOf(n, hs), a, b};
        return builder.addInst(MirOpcode::Call, ops, ret, /*payload=*/0);
    };
    auto call3 = [&](char const* n, TypeId hs, TypeId ret, MirInstId a, MirInstId b, MirInstId c) {
        std::array<MirInstId, 4> ops{importOf(n, hs), a, b, c};
        return builder.addInst(MirOpcode::Call, ops, ret, /*payload=*/0);
    };
    auto call4 = [&](char const* n, TypeId hs, TypeId ret, MirInstId a, MirInstId b,
                     MirInstId c, MirInstId d) {
        std::array<MirInstId, 5> ops{importOf(n, hs), a, b, c, d};
        return builder.addInst(MirOpcode::Call, ops, ret, /*payload=*/0);
    };
    auto call6 = [&](char const* n, TypeId hs, TypeId ret, MirInstId a, MirInstId b,
                     MirInstId c, MirInstId d, MirInstId e, MirInstId f) {
        std::array<MirInstId, 7> ops{importOf(n, hs), a, b, c, d, e, f};
        return builder.addInst(MirOpcode::Call, ops, ret, /*payload=*/0);
    };
    // A Bool = (x == 0), then zero-extended to i32 (0 or 1). The C11 error codes fall
    // straight out: thrd_success=0, thrd_busy=1, and thrd_error=2 = (that)*2.
    auto isZeroI32 = [&](MirInstId x) -> MirInstId {
        std::array<MirInstId, 2> cmp{x, i32c(0)};
        MirInstId const eq = builder.addInst(MirOpcode::ICmpEq, cmp, boolTy);
        std::array<MirInstId, 1> ze{eq};
        return builder.addInst(MirOpcode::ZExt, ze, i32Ty);
    };

    // Open a shim function + its entry block, stamped EntryBlock. Most recipes are
    // single-block, so this raw marker is already canonical; the one MULTI-block recipe
    // (thrd_join) creates its extra blocks with default markers that the module-wide
    // `rederiveStructCfMarkers` at the end makes canonical (the merge path's per-pass
    // MirVerifier enforces stored==derived; the single-CU post-optimize path has no
    // verifier and codegen ignores markers, so the rederive is idempotent there).
    // Returns nothing; the caller emits the body then a terminator.
    auto begin = [&](SymbolId sym, TypeId fnSig) {
        (void)builder.addFunction(fnSig, sym, SymbolBinding::Global,
                                  SymbolVisibility::Default);
        MirBlockId const entry = builder.createBlock(StructCfMarker::EntryBlock);
        builder.beginBlock(entry);
    };

    // Cycle 2: emit the once-adapter ONCE, before the recipe loop (every call_once
    // GlobalAddr-references it). Global-bound so DCE — which runs AFTER the multi-CU
    // synth, pre-optimize — keeps it: an OS-invoked callback is never a direct-call
    // target, so it would otherwise look dead (the synthesizePeStartup `_dss_pe_start`
    // precedent). Its 3 params match PINIT_ONCE_FN; only `param` (the C11 fn) is used, but
    // all 3 Args are emitted so their ordinals stay contiguous (0,1,2) — DCE keeps every
    // Arg as a root (D-OPT-VARIADIC-RELEASE-ARGINDEX), so the unused io/ctx survive.
    if (onceTrampSym.has_value()) {
        begin(*onceTrampSym, onceTrampSig);
        (void)builder.addArg(0, pVoid);                  // InitOnce* (ignored)
        MirInstId const fn = builder.addArg(1, pVoid);   // the C11 void(*)(void)
        (void)builder.addArg(2, pVoid);                  // Context* (ignored)
        std::array<MirInstId, 1> ind{fn};                // fn() — indirect (D-CSUBSET-FNPTR-INDIRECT-CALL)
        builder.addInst(MirOpcode::Call, ind, InvalidType);
        builder.addReturn(i32c(1));   // TRUE — else InitOnceExecuteOnce treats init as FAILED
    }

    for (auto const& [symV, recipe] : recipes) {
        SymbolId const sym{symV};

        // ── mutex ──
        if (recipe == "mtx_init") {          // InitializeCriticalSection(m); ret success
            begin(sym, rSig_mtx_init);
            MirInstId const m = builder.addArg(0, pVoid);   // (Arg 1 `type` ignored: a
            call1("InitializeCriticalSection", hSig_v_pV, InvalidType, m); // CRITICAL_SECTION
            builder.addReturn(i32c(0));                     // is always recursion-capable)
        } else if (recipe == "mtx_lock") {   // EnterCriticalSection(m); ret success
            begin(sym, rSig_i32_pV);
            call1("EnterCriticalSection", hSig_v_pV, InvalidType, builder.addArg(0, pVoid));
            builder.addReturn(i32c(0));
        } else if (recipe == "mtx_unlock") { // LeaveCriticalSection(m); ret success
            begin(sym, rSig_i32_pV);
            call1("LeaveCriticalSection", hSig_v_pV, InvalidType, builder.addArg(0, pVoid));
            builder.addReturn(i32c(0));
        } else if (recipe == "mtx_trylock") {// TryEnter? success : busy(1)
            begin(sym, rSig_i32_pV);
            MirInstId const b = call1("TryEnterCriticalSection", hSig_i32_pV, i32Ty,
                                      builder.addArg(0, pVoid));
            builder.addReturn(isZeroI32(b));                // b==0 (not acquired) → busy=1
        } else if (recipe == "mtx_destroy") {// DeleteCriticalSection(m); (void)
            begin(sym, rSig_v_pV);
            call1("DeleteCriticalSection", hSig_v_pV, InvalidType, builder.addArg(0, pVoid));
            builder.addReturn();

        // ── condition variable ──
        } else if (recipe == "cnd_init") {   // InitializeConditionVariable(c); ret success
            begin(sym, rSig_i32_pV);
            call1("InitializeConditionVariable", hSig_v_pV, InvalidType, builder.addArg(0, pVoid));
            builder.addReturn(i32c(0));
        } else if (recipe == "cnd_signal") { // WakeConditionVariable(c); ret success
            begin(sym, rSig_i32_pV);
            call1("WakeConditionVariable", hSig_v_pV, InvalidType, builder.addArg(0, pVoid));
            builder.addReturn(i32c(0));
        } else if (recipe == "cnd_broadcast") { // WakeAllConditionVariable(c); ret success
            begin(sym, rSig_i32_pV);
            call1("WakeAllConditionVariable", hSig_v_pV, InvalidType, builder.addArg(0, pVoid));
            builder.addReturn(i32c(0));
        } else if (recipe == "cnd_wait") {   // SleepConditionVariableCS(c,m,INFINITE); ret success
            begin(sym, rSig_cnd_wait);
            MirInstId const c = builder.addArg(0, pVoid);
            MirInstId const m = builder.addArg(1, pVoid);
            MirInstId const inf = konst(static_cast<std::int64_t>(0xFFFFFFFFu),
                                        TypeKind::U32, u32Ty);   // INFINITE
            call3("SleepConditionVariableCS", hSig_i32_pVpVu32, i32Ty, c, m, inf);
            builder.addReturn(i32c(0));
        } else if (recipe == "cnd_destroy") {// no-op (CONDITION_VARIABLE needs none); (void)
            begin(sym, rSig_v_pV);
            builder.addReturn();

        // ── thread-specific storage (Win32 Fls* — FlsAlloc's dtor == tss_dtor_t, so C11
        //    destructor semantics hold) ──
        } else if (recipe == "tss_create") {  // *k = FlsAlloc(dtor); ret (*k!=OOI)?success:error
            begin(sym, rSig_tss_create);
            MirInstId const k    = builder.addArg(0, pU32);
            MirInstId const dtor = builder.addArg(1, pVoid);
            MirInstId const idx  = call1("FlsAlloc", hSig_u32_pV, u32Ty, dtor);
            std::array<MirInstId, 2> st{idx, k};
            builder.addInst(MirOpcode::Store, st, InvalidType);   // *k = idx
            MirInstId const ooi  = konst(static_cast<std::int64_t>(0xFFFFFFFFu),
                                         TypeKind::U32, u32Ty);    // FLS_OUT_OF_INDEXES
            std::array<MirInstId, 2> cmp{idx, ooi};
            MirInstId const eq   = builder.addInst(MirOpcode::ICmpEq, cmp, boolTy); // failed?
            std::array<MirInstId, 1> ze{eq};
            MirInstId const z    = builder.addInst(MirOpcode::ZExt, ze, i32Ty);     // 0/1
            std::array<MirInstId, 2> mul{z, i32c(2)};
            builder.addReturn(builder.addInst(MirOpcode::Mul, mul, i32Ty));         // 0 or thrd_error(2)
        } else if (recipe == "tss_get") {    // ret FlsGetValue(k)
            begin(sym, rSig_pV_u32);
            builder.addReturn(call1("FlsGetValue", hSig_pV_u32, pVoid, builder.addArg(0, u32Ty)));
        } else if (recipe == "tss_set") {    // ret FlsSetValue(k,v)?success:error(2)
            begin(sym, rSig_i32_u32pV);
            MirInstId const k = builder.addArg(0, u32Ty);
            MirInstId const v = builder.addArg(1, pVoid);
            MirInstId const b = call2("FlsSetValue", hSig_i32_u32pV, i32Ty, k, v);
            std::array<MirInstId, 2> mul{isZeroI32(b), i32c(2)};
            builder.addReturn(builder.addInst(MirOpcode::Mul, mul, i32Ty));         // b==0 → error(2)
        } else if (recipe == "tss_delete") { // FlsFree(k); (void)
            begin(sym, rSig_v_u32);
            call1("FlsFree", hSig_i32_u32, i32Ty, builder.addArg(0, u32Ty));
            builder.addReturn();

        // ── thread management ──
        } else if (recipe == "thrd_current") { // ret GetCurrentThread() [pseudo-handle wart, named]
            begin(sym, rSig_pV_void);
            builder.addReturn(call0("GetCurrentThread", hSig_pV_void, pVoid));
        } else if (recipe == "thrd_yield") { // SwitchToThread(); (void)
            begin(sym, rSig_v_void);
            call0("SwitchToThread", hSig_i32_void, i32Ty);
            builder.addReturn();
        } else if (recipe == "thrd_exit") {  // ExitThread((DWORD)res); (void, noreturn)
            begin(sym, rSig_v_i32);
            call1("ExitThread", hSig_v_u32, InvalidType, builder.addArg(0, i32Ty));
            builder.addReturn();             // dead (ExitThread noreturn) — a terminator is required
        } else if (recipe == "thrd_detach") {// CloseHandle(t); ret success
            begin(sym, rSig_i32_pV);
            call1("CloseHandle", hSig_i32_pV, i32Ty, builder.addArg(0, pVoid));
            builder.addReturn(i32c(0));

        // ── Cycle 2 (D-CSUBSET-C11-THREADS-TRAMPOLINES) ──
        } else if (recipe == "thrd_create") {
            // DIRECT-PASS (no closure/trampoline): the C11 start routine int(*)(void*) has
            // the SAME x64 ABI as Win32's LPTHREAD_START_ROUTINE DWORD(*)(void*) — one arg
            // (a ptr, in rcx), a 32-bit return in eax (the thread exit code). So hand
            // `func`+`arg` straight to CreateThread; no malloc, no adapter.
            //   h = CreateThread(NULL, 0, func, arg, 0, NULL); *thr = h;
            //   ret (h == NULL) ? thrd_error(2) : thrd_success(0)     (branchless)
            begin(sym, rSig_thrd_create);
            MirInstId const thr  = builder.addArg(0, pVoid);   // thrd_t* (out)
            MirInstId const func = builder.addArg(1, pVoid);   // thrd_start_t == start routine
            MirInstId const arg  = builder.addArg(2, pVoid);   // void* lpParameter
            MirInstId const nul  = konst(0, TypeKind::Ptr, pVoid);  // lpThreadAttributes / lpThreadId
            MirInstId const z64  = konst(0, TypeKind::U64, u64Ty);  // dwStackSize (default)
            MirInstId const z32  = konst(0, TypeKind::U32, u32Ty);  // dwCreationFlags (run now)
            MirInstId const h    = call6("CreateThread", hSig_CreateThread, pVoid,
                                         nul, z64, func, arg, z32, nul);
            std::array<MirInstId, 2> st{h, thr};
            builder.addInst(MirOpcode::Store, st, InvalidType);    // *thr = h (8-byte handle; value-width driven)
            std::array<MirInstId, 1> hi{h};
            MirInstId const hInt = builder.addInst(MirOpcode::PtrToInt, hi, i64Ty);
            std::array<MirInstId, 2> cmp{hInt, konst(0, TypeKind::I64, i64Ty)};
            MirInstId const eq   = builder.addInst(MirOpcode::ICmpEq, cmp, boolTy);  // h == 0?
            std::array<MirInstId, 1> ze{eq};
            MirInstId const z    = builder.addInst(MirOpcode::ZExt, ze, i32Ty);      // 0/1
            std::array<MirInstId, 2> mul{z, i32c(2)};
            builder.addReturn(builder.addInst(MirOpcode::Mul, mul, i32Ty));          // 0 or thrd_error(2)

        } else if (recipe == "call_once") {
            // InitOnceExecuteOnce(f, &__dss_once_tramp, fn, NULL) — the adapter (minted +
            // emitted above) invokes the C11 void(*)(void) `fn` exactly once.
            if (!onceTrampSym.has_value()) {   // never fires: the pre-loop scan minted it
                emitErr(reporter, "synthesizeThreadsShim: call_once recipe without a "
                                  "synthesized __dss_once_tramp adapter (internal breach)");
                return false;
            }
            begin(sym, rSig_call_once);
            MirInstId const flag  = builder.addArg(0, pVoid);   // once_flag* (INIT_ONCE*)
            MirInstId const fn    = builder.addArg(1, pVoid);   // void(*)(void)
            MirInstId const tramp = builder.addGlobalAddr(*onceTrampSym,
                                                          interner.pointer(onceTrampSig));
            MirInstId const nul   = konst(0, TypeKind::Ptr, pVoid);   // Context = NULL
            call4("InitOnceExecuteOnce", hSig_i32_4pV, i32Ty, flag, tramp, fn, nul);
            builder.addReturn();   // void

        } else if (recipe == "thrd_join") {
            // MULTI-block: WaitForSingleObject(t, INFINITE); if (res) GetExitCodeThread(t,
            // res); CloseHandle(t); ret thrd_success. The `if (res)` guard is a real branch
            // (a NULL `res` must not fault). Blocks are created with default markers; the
            // module-wide rederive at the end makes them canonical (IfThen/IfJoin).
            begin(sym, rSig_thrd_join);                        // opens the ENTRY block
            MirInstId const t   = builder.addArg(0, pVoid);    // thrd_t (HANDLE) by value
            MirInstId const res = builder.addArg(1, pI32);     // int* (exit-code out; may be NULL)
            MirInstId const inf = konst(static_cast<std::int64_t>(0xFFFFFFFFu),
                                        TypeKind::U32, u32Ty); // INFINITE
            call2("WaitForSingleObject", hSig_u32_pVu32, u32Ty, t, inf);
            std::array<MirInstId, 1> rp{res};
            MirInstId const resInt = builder.addInst(MirOpcode::PtrToInt, rp, i64Ty);
            std::array<MirInstId, 2> ne{resInt, konst(0, TypeKind::I64, i64Ty)};
            MirInstId const cond   = builder.addInst(MirOpcode::ICmpNe, ne, boolTy); // res != 0
            MirBlockId const thenBB = builder.createBlock();   // marker set by the module-wide rederive
            MirBlockId const joinBB = builder.createBlock();
            builder.addCondBr(cond, thenBB, joinBB);
            // then: GetExitCodeThread(t, (LPDWORD)res); Br join
            builder.beginBlock(thenBB);
            std::array<MirInstId, 1> rc{res};
            MirInstId const resU32 = builder.addInst(MirOpcode::Bitcast, rc, pU32); // ptr<i32> → ptr<u32>
            call2("GetExitCodeThread", hSig_i32_pVpU32, i32Ty, t, resU32);
            builder.addBr(joinBB);
            // join: CloseHandle(t); ret thrd_success(0)
            builder.beginBlock(joinBB);
            call1("CloseHandle", hSig_i32_pV, i32Ty, t);
            builder.addReturn(i32c(0));

        } else {
            // A recipe id present in the descriptor vocabulary but with NO arm here — a
            // vocab/switch drift. Fail loud (never a silently-undefined shim). The loader
            // closed-vocab guard makes this unreachable in practice; this is the backstop.
            emitErr(reporter, "synthesizeThreadsShim: no synth arm for recipe id '"
                                  + recipe + "' (D-CSUBSET-C11-THREADS-HEADER vocab/switch drift)");
            return false;
        }
    }

    opt::passes::cloneGlobalsVerbatim(mir, builder);
    mir = std::move(builder).finish();
    // Cycle 2: canonicalize StructCfMarkers module-wide from the CFG — REQUIRED for
    // thrd_join's multi-block (its then/join blocks were created with default markers) so
    // the merge-path per-pass MirVerifier's stored==derived check passes; idempotent for
    // every other function (single-block recipes + clones re-derive to their existing
    // markers). Never hand-stamp IfThen/IfJoin (mir_struct_markers is the single source).
    rederiveStructCfMarkers(mir);
    return true;
}

} // namespace dss
