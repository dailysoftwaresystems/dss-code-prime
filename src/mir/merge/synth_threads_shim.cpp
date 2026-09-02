#include "mir/merge/synth_threads_shim.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "ffi/mangling/c_mangle.hpp"   // applyCMangling (per-format C helper import names)
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
#include <string_view>
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
    // The rebuild DRIVER this policy belongs to — printed by every
    // `MirFunctionRebuilder` fatal.
    // See D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS (one line: a wrapped
    // anchor name mints a second, unregistered anchor).
    // A MIR merge step, not a `kPassNameTable` pipeline pass.
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "SynthThreadsShim";
    }

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
    std::optional<LibrarySynthesis> const&                librarySynthesis,
    CSymbolDecorationScheme                               scheme,
    std::vector<ExternImport>&                            externImports,
    DiagnosticReporter&                                   reporter) {
    // Presence gate: no tagged shim symbol ⇒ clean no-op (every elf + every non-threads
    // pe/macho TU). Keys on the map (a data property), never a format check.
    if (recipeBySymbol.empty()) return true;

    // D-CSUBSET-C11-THREADS-MACHO: a non-empty recipe map means the active format carries
    // synthesize-tagged <threads.h> symbols — so it MUST declare which primitive family to
    // synthesize over. A missing `librarySynthesis` block is a format/descriptor mismatch:
    // fail LOUD, never silently assume a vehicle (a disguised `if (format != known) →
    // win32` default is exactly the identity branch the bar forbids).
    // ⚠ NO DEFERRAL ROW IS NAMED IN THE EMITTED TEXT, AND THAT IS DELIBERATE.
    // This message used to carry `(D-CSUBSET-C11-THREADS-MACHO)`. That row is
    // CLOSED — closing it is how `macho64-arm64` GOT its `librarySynthesis`
    // block — so citing it told an operator "a known deferral, wait for it"
    // about a format whose omission is a standing argued decision (see that
    // format's own `$remainingDeliberateOmissionsComment`), not a queue item.
    // ★ THE REFUSAL WAS REAL AND THE ATTRIBUTION WAS FALSE, which is the
    // harder half of the species to see: nothing about the behaviour was wrong.
    // ⇒ a diagnostic states the CONDITION and the ACTION; provenance lives
    // here, where closing a row cannot turn a comment into compiler output.
    if (!librarySynthesis.has_value()) {
        emitErr(reporter,
                "synthesizeThreadsShim: <threads.h> synth recipes are present but the target "
                "object format declares no `librarySynthesis` vehicle — refusing to assume "
                "a primitive family. Declare one in the format descriptor, or drop the "
                "synthesize-tagged <threads.h> symbols for this format");
        return false;
    }
    LibrarySynthVehicle const vehicle       = librarySynthesis->vehicle;
    std::string const&        importLibrary = librarySynthesis->libraryPath;

    // A DETERMINISTIC emission order (unordered_map iteration is not stable — a shifting
    // function order would make the binary non-reproducible). Sort by pre-minted
    // SymbolId.v.
    std::vector<std::pair<std::uint32_t, std::string>> recipes(
        recipeBySymbol.begin(), recipeBySymbol.end());
    std::sort(recipes.begin(), recipes.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    // ── Types (interned once; shared by both vehicles — the pthread thrd_t/tss_t are u64,
    //    the win32 ones u32/ptr<void>, so the u64 shapes are also interned) ──
    TypeId const voidTy = interner.primitive(TypeKind::Void);
    TypeId const i32Ty  = interner.primitive(TypeKind::I32);
    TypeId const i64Ty  = interner.primitive(TypeKind::I64);
    TypeId const u32Ty  = interner.primitive(TypeKind::U32);
    TypeId const u64Ty  = interner.primitive(TypeKind::U64);   // Cycle 2 CreateThread dwStackSize; pthread thrd_t/tss_t
    TypeId const boolTy = interner.primitive(TypeKind::Bool);
    TypeId const pVoid  = interner.pointer(voidTy);   // ptr<void> (mtx_t*/cnd_t*/HANDLE/attr)
    TypeId const pU32   = interner.pointer(u32Ty);    // win32 tss_t* (u32*) / GetExitCodeThread LPDWORD
    TypeId const pU64   = interner.pointer(u64Ty);    // pthread tss_t* (pthread_key_t*, u64*)
    TypeId const pI32   = interner.pointer(i32Ty);    // Cycle 2: thrd_join int* res

    // The FnSig CC is DOCUMENTARY — the real ABI is applied downstream by the target's
    // callingConventionIndex, not keyed here (the errno/ExitThread precedent). Match the
    // vehicle's host ABI for honesty: win32 → MS-x64, pthread → Apple arm64.
    CallConv const cc = (vehicle == LibrarySynthVehicle::Win32) ? CallConv::CcMS64
                                                                : CallConv::CcApple;
    auto sig = [&](std::vector<TypeId> params, TypeId ret) -> TypeId {
        return interner.fnSig(params, ret, cc);
    };

    // ── win32 (kernel32) helper signatures ──
    // ⚠ SECOND OWNER, STATED NOT HIDDEN — the anchor name must stay on ONE line or the
    // registry guard extracts a truncated identifier that resolves to nothing:
    // D-MIR-SYNTH-SHIM-HELPER-SIGNATURES-DUPLICATE-THE-DESCRIPTOR.
    // Every `hSig_*` / `phSig_*` below re-declares a shape that a shipped
    // descriptor ALSO declares (e.g. `hSig_v_u32` here vs windows.json's
    // `ExitThread: fn(u32) -> void`). They agree today and nothing enforces it — and the
    // post-synthesis MirVerifier CANNOT enforce it, because it reads the callee FnSig off the
    // GlobalAddr this pass itself minted, so the rule is self-consistent within the pass. It
    // catches the INTRA-shim class only (the `thrd_exit` i32→u32 pun below was exactly that).
    TypeId const hSig_v_pV        = sig({pVoid}, voidTy);                 // Init/Enter/Leave/Delete CS; cond-var Init/Wake/WakeAll
    TypeId const hSig_i32_pV      = sig({pVoid}, i32Ty);                  // TryEnterCriticalSection / CloseHandle
    TypeId const hSig_i32_pVpVu32 = sig({pVoid, pVoid, u32Ty}, i32Ty);   // SleepConditionVariableCS
    TypeId const hSig_u32_pV      = sig({pVoid}, u32Ty);                  // FlsAlloc(dtor); GetThreadId(HANDLE)
    TypeId const hSig_pV_u32      = sig({u32Ty}, pVoid);                  // FlsGetValue
    TypeId const hSig_i32_u32pV   = sig({u32Ty, pVoid}, i32Ty);          // FlsSetValue
    TypeId const hSig_i32_u32     = sig({u32Ty}, i32Ty);                 // FlsFree
    TypeId const hSig_i32_void    = sig({}, i32Ty);                      // SwitchToThread / sched_yield
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
    // Cycle 3 (D-CSUBSET-C11-THREADS-TIMED) kernel32 helper signatures. `Sleep` reuses
    // `hSig_v_u32` and `GetSystemTimeAsFileTime` reuses `hSig_v_pV` (identical shapes, and
    // windows.json declares them so); `GetThreadId` reuses `hSig_u32_pV`. Only
    // `GetLastError` needs a new one.
    TypeId const hSig_u32_void     = sig({}, u32Ty);                     // GetLastError

    // ── win32 shim (recipe) signatures (the pe thrd_t is ptr<void>, tss_t u32) ──
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
    // Cycle 3 win32 recipe signatures. thrd_sleep(const timespec*, timespec*),
    // mtx_timedlock(mtx_t*, const timespec*) and thrd_equal(thrd_t, thrd_t) all land on
    // `fn(ptr, ptr) -> i32` — thrd_equal only because the pe thrd_t IS a HANDLE
    // (ptr<void>); its elf/macho twins take u64 and carry their own signature, exactly as
    // thrd_current/thrd_detach/thrd_join already diverge.
    TypeId const rSig_i32_2pV      = sig({pVoid, pVoid}, i32Ty);
    TypeId const rSig_cnd_timedwait = sig({pVoid, pVoid, pVoid}, i32Ty);

    // ── pthread (libSystem) helper signatures ──
    TypeId const phSig_i32_pV     = sig({pVoid}, i32Ty);            // pthread_mutex_lock/unlock/trylock/destroy, cond_signal/broadcast/destroy
    TypeId const phSig_i32_pVpV   = sig({pVoid, pVoid}, i32Ty);     // pthread_mutex_init / cond_init(obj,attr=NULL); cond_wait(cond,mutex)
    TypeId const phSig_i32_pU64pV = sig({pU64, pVoid}, i32Ty);      // pthread_key_create(key*, dtor)
    TypeId const phSig_pV_u64     = sig({u64Ty}, pVoid);            // pthread_getspecific(key)
    TypeId const phSig_i32_u64pV  = sig({u64Ty, pVoid}, i32Ty);     // pthread_setspecific(key, value)
    TypeId const phSig_i32_u64    = sig({u64Ty}, i32Ty);            // pthread_key_delete(key) / pthread_detach(thread)
    TypeId const phSig_u64_void   = sig({}, u64Ty);                 // pthread_self()
    TypeId const phSig_i32_void   = sig({}, i32Ty);                 // sched_yield()
    TypeId const phSig_v_pV       = sig({pVoid}, voidTy);           // pthread_exit(value_ptr)
    TypeId const phSig_i32_4pV    = sig({pVoid, pVoid, pVoid, pVoid}, i32Ty); // pthread_create(thr, attr, start, arg)
    TypeId const phSig_i32_u64pU  = sig({u64Ty, pVoid}, i32Ty);     // pthread_join(thread, void** retval)
    // Cycle 3 (D-CSUBSET-C11-THREADS-TIMED) libSystem helper signatures. `nanosleep(req,
    // rem)` and `pthread_mutex_trylock` reuse `phSig_i32_pVpV` / `phSig_i32_pV`.
    TypeId const phSig_i32_3pV    = sig({pVoid, pVoid, pVoid}, i32Ty);   // pthread_cond_timedwait(c, m, abstime)
    TypeId const phSig_i32_i32pV  = sig({i32Ty, pVoid}, i32Ty);          // clock_gettime(clk_id, timespec*)
    TypeId const phSig_i32_2u64   = sig({u64Ty, u64Ty}, i32Ty);          // pthread_equal(t1, t2)
    // Cycle 4 (D-CSUBSET-C11-THREADS-MACHO-MTX-PLAIN-RECURSIVE) libSystem helper
    // signature. `pthread_mutexattr_init` and `pthread_mutexattr_destroy` reuse
    // `phSig_i32_pV`; only `settype`'s (attr*, int) shape is new.
    TypeId const phSig_i32_pVi32  = sig({pVoid, i32Ty}, i32Ty);          // pthread_mutexattr_settype(attr, kind)

    // ── pthread shim (recipe) signatures (the macho thrd_t/tss_t are u64) ──
    TypeId const rSigP_mtx_init   = sig({pVoid, i32Ty}, i32Ty);
    TypeId const rSigP_i32_pV     = sig({pVoid}, i32Ty);       // mtx_lock/unlock/trylock, cnd_init/signal/broadcast
    TypeId const rSigP_v_pV       = sig({pVoid}, voidTy);      // mtx_destroy, cnd_destroy
    TypeId const rSigP_cnd_wait   = sig({pVoid, pVoid}, i32Ty);
    TypeId const rSigP_tss_create = sig({pU64, pVoid}, i32Ty);
    TypeId const rSigP_pV_u64     = sig({u64Ty}, pVoid);       // tss_get
    TypeId const rSigP_i32_u64pV  = sig({u64Ty, pVoid}, i32Ty);// tss_set
    TypeId const rSigP_v_u64      = sig({u64Ty}, voidTy);      // tss_delete
    TypeId const rSigP_u64_void   = sig({}, u64Ty);            // thrd_current
    TypeId const rSigP_v_void     = sig({}, voidTy);           // thrd_yield
    TypeId const rSigP_v_i32      = sig({i32Ty}, voidTy);      // thrd_exit
    TypeId const rSigP_i32_u64    = sig({u64Ty}, i32Ty);       // thrd_detach
    TypeId const rSigP_thrd_create= sig({pVoid, pVoid, pVoid}, i32Ty);  // (thrd_t*, start, arg)->int
    TypeId const rSigP_call_once  = sig({pVoid, pVoid}, voidTy);        // (once_flag*, void(*)(void))->void
    TypeId const rSigP_thrd_join  = sig({u64Ty, pI32}, i32Ty);         // (thrd_t, int*)->int
    // Cycle 3 pthread recipe signatures. thrd_sleep(const timespec*, timespec*) and
    // mtx_timedlock(mtx_t*, const timespec*) share the two-pointer shape; thrd_equal takes
    // the macho thrd_t (u64) BY VALUE, unlike its pe twin.
    TypeId const rSigP_i32_2pV      = sig({pVoid, pVoid}, i32Ty);
    TypeId const rSigP_cnd_timedwait = sig({pVoid, pVoid, pVoid}, i32Ty);
    TypeId const rSigP_thrd_equal    = sig({u64Ty, u64Ty}, i32Ty);

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
        // C-mangle the helper's canonical C name for the active format (leading-underscore
        // prepends `_`; none is identity) — the SAME `applyCMangling` the FFI ingest
        // applies, so the on-disk undefined-symbol name matches the library's export
        // (libSystem exports `_pthread_mutex_init`, kernel32 `InitializeCriticalSection`).
        // Dedup by the MANGLED name so a TU that ALSO imports the helper via <pthread.h>/
        // <windows.h> (whose ExternImport.mangledName is already mangled) reuses that symbol.
        std::string const mangled = dss::ffi::applyCMangling(name, scheme);
        SymbolId hs;
        if (auto it = helperSyms.find(mangled); it != helperSyms.end()) {
            hs = it->second;
        } else {
            hs = SymbolId{++nextSymV};
            helperSyms.emplace(mangled, hs);
            ExternImport imp;
            imp.symbol      = hs;
            imp.mangledName = mangled;
            imp.libraryPath = importLibrary;   // config-sourced (kernel32.dll / libSystem)
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
    // A Bool = (x != 0), zero-extended to i32 (0 or 1). The pthread return convention is
    // the INVERSE of Win32's: pthread_* return 0 on success + errno (nonzero) on failure,
    // so `(x != 0)` is the failure indicator — thrd_busy=1 for trylock, and *2 = thrd_error
    // for the create/set/detach error paths.
    auto isNonZeroI32 = [&](MirInstId x) -> MirInstId {
        std::array<MirInstId, 2> cmp{x, i32c(0)};
        MirInstId const ne = builder.addInst(MirOpcode::ICmpNe, cmp, boolTy);
        std::array<MirInstId, 1> ze{ne};
        return builder.addInst(MirOpcode::ZExt, ze, i32Ty);
    };
    // A ptr<void> NULL literal (`MirLiteralValue{0, core=Ptr}` — the direct-pointer-literal
    // precedent, hir_to_mir's null-pointer path). The pthread mtx_init/cnd_init/key_create
    // recipes pass it as the default-attr argument.
    auto nullPtr = [&]() -> MirInstId { return konst(0, TypeKind::Ptr, pVoid); };

    // ── Cycle 3 (D-CSUBSET-C11-THREADS-TIMED) shared arithmetic ─────────────────────
    // The timed recipes are the first that READ A SHIPPED STRUCT. `struct timespec` is
    // declared by `shippedLibs/time.json` (and, identically, by `sys/stat.json`) with a
    // PER-FORMAT body, so the field OFFSETS and the tv_nsec WIDTH below are a SECOND
    // OWNER of a fact that config already states — the same stated-not-hidden duplication
    // the `hSig_*` block above carries, tracked as
    // D-MIR-SYNTH-SHIM-HELPER-SIGNATURES-DUPLICATE-THE-DESCRIPTOR, and it cannot be
    // dissolved at this seam: `synthesizeThreadsShim` receives a recipe map and a
    // vehicle, never a descriptor. What DOES bind the two owners is a RUNTIME witness
    // rather than a comment — `examples/c/c11_threads_timed` poisons the struct to 0xFF
    // before assigning its fields, so a shim that read tv_nsec at the wrong width would
    // compute a nonsense millisecond count and fail the elapsed-time assertion on the
    // leg whose descriptor it disagrees with. (A well-initialized timespec would NOT
    // expose it: the pad bytes read back as zero and both widths agree.)
    auto const i64c = [&](std::int64_t v) { return konst(v, TypeKind::I64, i64Ty); };
    auto const u32c = [&](std::int64_t v) { return konst(v, TypeKind::U32, u32Ty); };
    // ⚠ EVERY sub-expression below is HOISTED INTO A NAMED LOCAL before it is combined,
    // never nested as two emitting arguments of one `bin(...)` call. C++ leaves the
    // evaluation order of function arguments UNSPECIFIED, so `bin(Add, bin(...), bin(...))`
    // would emit its two operand chains in whichever order the host compiler chose — and
    // this pass's whole reason for sorting its recipes by SymbolId is that a shifting
    // instruction order makes the produced binary non-reproducible. (The `std::array{…}`
    // operand lists this file already uses are safe: a braced-init-list IS sequenced
    // left-to-right. A bare call is not.)
    auto bin = [&](MirOpcode op, MirInstId a, MirInstId b, TypeId ty) -> MirInstId {
        std::array<MirInstId, 2> ops{a, b};
        return builder.addInst(op, ops, ty);
    };
    auto un = [&](MirOpcode op, MirInstId a, TypeId ty) -> MirInstId {
        std::array<MirInstId, 1> ops{a};
        return builder.addInst(op, ops, ty);
    };
    // `(unsigned char *)p + bytes`, spelled PtrToInt/Add/IntToPtr rather than `Gep`
    // because a Gep is TYPE-driven and this pass holds no `struct timespec` TypeId — the
    // descriptor owns the layout, the pass owns only the byte offset it was told.
    auto byteOffset = [&](MirInstId p, std::int64_t bytes) -> MirInstId {
        MirInstId const asInt = un(MirOpcode::PtrToInt, p, i64Ty);
        return un(MirOpcode::IntToPtr, bin(MirOpcode::Add, asInt, i64c(bytes), i64Ty), pVoid);
    };
    // A Load's ACCESSED type IS its result type (the MirVerifier's own rule), so the
    // width of the read is chosen HERE and nowhere else. This is the line the pe
    // silent-miscompile risk lived on: `tv_nsec` is `long` — FOUR bytes on LLP64, EIGHT
    // on LP64 — while `sizeof(struct timespec)` is 16 and `offsetof(tv_nsec)` is 8 on
    // BOTH, so neither a size nor an offset check can discriminate.
    auto loadAt = [&](MirInstId base, std::int64_t bytes, TypeId ty) -> MirInstId {
        return un(MirOpcode::Load, bytes == 0 ? base : byteOffset(base, bytes), ty);
    };
    // A Bool predicate widened to i64 (0/1), the multiplicand of every branchless
    // select below — the i32 twin (`isZeroI32` / `isNonZeroI32`) is right above.
    auto predI64 = [&](MirOpcode cmp, MirInstId a, MirInstId b) -> MirInstId {
        return un(MirOpcode::ZExt, bin(cmp, a, b, boolTy), i64Ty);
    };
    // Branchless `x < 0 ? 0 : x` and `x > cap ? cap : x`, both as `x - (x - other) * pred`
    // — the `Mul`-by-a-zero-extended-predicate idiom this file already uses for its
    // thrd_error(2) returns. A CondBr pair would cost two blocks each and buy nothing.
    auto clampLow0 = [&](MirInstId x) -> MirInstId {
        return bin(MirOpcode::Sub, x,
                   bin(MirOpcode::Mul, x, predI64(MirOpcode::ICmpSlt, x, i64c(0)), i64Ty), i64Ty);
    };
    auto clampHigh = [&](MirInstId x, std::int64_t cap) -> MirInstId {
        MirInstId const over = bin(MirOpcode::Sub, x, i64c(cap), i64Ty);
        return bin(MirOpcode::Sub, x,
                   bin(MirOpcode::Mul, over,
                       predI64(MirOpcode::ICmpSgt, x, i64c(cap)), i64Ty), i64Ty);
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

        if (vehicle == LibrarySynthVehicle::Win32) {
            // ══ win32 vehicle — bodies over kernel32 primitives (the pe64 shim) ══
            // ── mutex ──
            if (recipe == "mtx_init") {          // InitializeCriticalSection(m); ret success
                // D-CSUBSET-C11-THREADS-MTX-PLAIN-RECURSIVE — CLOSED WITHOUT A CODE
                // CHANGE, on a measurement rather than on symmetry with the macho half.
                //
                // A Win32 CRITICAL_SECTION is ALWAYS recursion-capable, so `type` is
                // accepted and not consulted. That is conforming, and here is the
                // measurement that says so rather than an argument from convenience.
                //
                // The two directions are NOT symmetric. `mtx_plain | mtx_recursive`
                // REQUIRES a re-lockable mutex, and pe already gives one. `mtx_plain`
                // merely forbids the CALLER to re-lock: C11 7.26.4.3 / C23 7.28.4.3 say
                // "If the mutex is non-recursive, it shall not be locked by the calling
                // thread" — a `shall` on the caller outside a Constraints clause, so a
                // program that does it is UNDEFINED, not implementation-defined.
                //
                // ✔MEASURED, and the references prove it is undefined by disagreeing
                // three ways on the same program: same-thread re-lock of an `mtx_plain`
                // mutex BLOCKS FOREVER under glibc 2.39 (identical through gcc 13.3.0
                // and clang 18.1.3, probed separately), FAIL-FAST ABORTS under MSVC
                // 19.51's own <threads.h> (exit 0xC0000409), and proceeds here. No two
                // agree, and none is wrong.
                // ⇒ ADDING an owner/recursion-count guard to force a deadlock would
                // INVENT a fourth behaviour no reference requires, spend a lock word and
                // two branches on every mtx_lock in the program, and convert a class of
                // buggy-but-running programs into hangs. The union governs ACCEPTANCE;
                // it cannot legislate inside undefined behaviour.
                // ⚠ So `Arg 1` is deliberately NOT materialized on this arm — there is
                // nothing to read it for. The macho twin DOES read it, because there the
                // RECURSIVE direction was genuinely broken.
                begin(sym, rSig_mtx_init);
                MirInstId const m = builder.addArg(0, pVoid);
                call1("InitializeCriticalSection", hSig_v_pV, InvalidType, m);
                builder.addReturn(i32c(0));
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

            // ── thread management (the Cycle-1 usable subset) ──
            } else if (recipe == "thrd_current") { // ret GetCurrentThread() [pseudo-handle wart, named]
                begin(sym, rSig_pV_void);
                builder.addReturn(call0("GetCurrentThread", hSig_pV_void, pVoid));
            } else if (recipe == "thrd_yield") { // SwitchToThread(); (void)
                begin(sym, rSig_v_void);
                call0("SwitchToThread", hSig_i32_void, i32Ty);
                builder.addReturn();
            } else if (recipe == "thrd_exit") {  // ExitThread((DWORD)res); (void, noreturn)
                begin(sym, rSig_v_i32);
                // ★ THE CAST IS LOAD-BEARING, not decoration (UCRT-P4, caught the moment a
                // MirVerifier was wired into the single-CU synth seam). C11 declares
                // `_Noreturn void thrd_exit(int res)` — SIGNED — and Win32 declares
                // `VOID ExitThread(DWORD dwExitCode)` — UNSIGNED; windows.json states the
                // latter faithfully (`fn(u32) -> void`). BOTH declarations are right, so the
                // conversion between them belongs HERE, exactly as the pthread arm below
                // spells its own `thrd_exit` widening with an EXPLICIT SExt+IntToPtr instead
                // of punning an i32 into a `ptr<void>` parameter. Handing the raw `i32` Arg
                // to a `u32` parameter was a silent type pun across a declared ABI boundary;
                // a hand-built MIR call must carry every conversion its C equivalent
                // (`ExitThread((DWORD)res)`) would.
                // Same-width int→int is a pure RETAG — no bits move (C 6.3.1.3p2 is modulo
                // 2^32, i.e. bit-identical two's complement) — so `Bitcast` is the opcode
                // hir_to_mir's own `mapCast` picks for `tw == fw` on the int↔int arm, and the
                // thrd_join arm below already uses it for its ptr retag. It lowers to one
                // same-class register move that copy-propagation coalesces.
                std::array<MirInstId, 1> dw{builder.addArg(0, i32Ty)};
                MirInstId const code = builder.addInst(MirOpcode::Bitcast, dw, u32Ty);
                call1("ExitThread", hSig_v_u32, InvalidType, code);
                builder.addReturn();             // dead (ExitThread noreturn) — a terminator is required
            } else if (recipe == "thrd_detach") {// CloseHandle(t); ret success
                begin(sym, rSig_i32_pV);
                call1("CloseHandle", hSig_i32_pV, i32Ty, builder.addArg(0, pVoid));
                builder.addReturn(i32c(0));

            // ── Cycle 2 (D-CSUBSET-C11-THREADS-TRAMPOLINES) — pe64 win32 trampolines ──
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

            // ── Cycle 3 (D-CSUBSET-C11-THREADS-TIMED) — pe64 timed waits + thrd_equal ──
            } else if (recipe == "thrd_sleep") {
                // C11 7.26.5.7 `int thrd_sleep(const struct timespec *duration,
                // struct timespec *remaining)` — suspend for AT LEAST `duration`.
                //   msTotal = tv_sec*1000 + ceil(tv_nsec / 1e6);  then Sleep it out.
                // ★ THE ROUNDING DIRECTION IS THE SEMANTICS, not a taste call: `Sleep` has
                // millisecond resolution and never returns EARLY, so rounding the
                // sub-millisecond remainder UP is what makes "at least the requested
                // interval" true. mingw-w64's own thrd_sleep truncates instead and can
                // therefore sleep short.
                // ★ THE LOOP EXISTS BECAUSE `Sleep` TAKES A DWORD AND `tv_sec` IS A 64-BIT
                // time_t. A single truncating Sleep would be wrong for any duration past
                // ~49.7 days, and — far worse — a duration whose low 32 millisecond bits
                // land on 0xFFFFFFFF would hand `Sleep` the INFINITE sentinel and HANG the
                // thread forever. Capping each iteration at 0xFFFFFFFE makes that sentinel
                // unreachable by construction and keeps the total exact.
                // `remaining` is left untouched: Win32 `Sleep` is not interruptible by a
                // signal, so the sleep always completes and C11 leaves `remaining`
                // meaningful only on the interrupted path. The Arg is still EMITTED so the
                // ordinals stay contiguous (0,1) — DCE keeps every Arg as a root.
                begin(sym, rSig_i32_2pV);
                MirInstId const dur = builder.addArg(0, pVoid);
                (void)builder.addArg(1, pVoid);                     // `remaining` (see above)
                MirInstId const secs  = loadAt(dur, 0, i64Ty);      // tv_sec  — 8 bytes @0
                MirInstId const nsRaw = loadAt(dur, 8, i32Ty);      // tv_nsec — 4 bytes @8 ★
                MirInstId const nsecs = un(MirOpcode::SExt, nsRaw, i64Ty);
                MirInstId const msFromSec = bin(MirOpcode::Mul, secs, i64c(1000), i64Ty);
                MirInstId const nsRoundUp = bin(MirOpcode::Add, nsecs, i64c(999999), i64Ty);
                MirInstId const msFromNs  = bin(MirOpcode::SDiv, nsRoundUp, i64c(1000000), i64Ty);
                MirInstId const msSum     = bin(MirOpcode::Add, msFromSec, msFromNs, i64Ty);
                MirInstId const total     = clampLow0(msSum);
                MirInstId const slot =
                    builder.addInst(MirOpcode::Alloca, {}, pVoid, /*bytes=*/8);
                std::array<MirInstId, 2> seed{total, slot};
                builder.addInst(MirOpcode::Store, seed, InvalidType);
                MirBlockId const headBB = builder.createBlock();   // markers rederived below
                MirBlockId const bodyBB = builder.createBlock();
                MirBlockId const doneBB = builder.createBlock();
                builder.addBr(headBB);
                // head: while (remainingMs > 0)
                builder.beginBlock(headBB);
                MirInstId const left = un(MirOpcode::Load, slot, i64Ty);
                builder.addCondBr(bin(MirOpcode::ICmpSgt, left, i64c(0), boolTy),
                                  bodyBB, doneBB);
                // body: Sleep(min(remaining, 0xFFFFFFFE)); remaining -= that
                builder.beginBlock(bodyBB);
                MirInstId const chunk = clampHigh(left, 0xFFFFFFFELL);
                call1("Sleep", hSig_v_u32, InvalidType,
                      un(MirOpcode::Trunc, chunk, u32Ty));
                std::array<MirInstId, 2> back{bin(MirOpcode::Sub, left, chunk, i64Ty), slot};
                builder.addInst(MirOpcode::Store, back, InvalidType);
                builder.addBr(headBB);
                // done: the whole interval elapsed
                builder.beginBlock(doneBB);
                builder.addReturn(i32c(0));

            } else if (recipe == "mtx_timedlock") {
                // C11 7.26.4.4 — block until the mutex is acquired or the CALENDAR time
                // `time_point` (TIME_UTC) passes; thrd_success / thrd_timedout / thrd_error.
                // ★ WIN32 HAS NO TIMED CRITICAL-SECTION ACQUIRE. `TryEnterCriticalSection`
                // carries no timeout and `EnterCriticalSection` carries no deadline, so a
                // trylock/deadline loop is not a workaround here — it is the only
                // construction the primitive set admits. (Switching mtx_t to a kernel Mutex
                // object would buy a timed WaitForSingleObject and LOSE cnd_wait, which
                // requires a CRITICAL_SECTION for SleepConditionVariableCS.)
                // ★ THE CLOCK IS THE WALL CLOCK ON PURPOSE. C11 defines the deadline as a
                // TIME_UTC calendar time, so `GetSystemTimeAsFileTime` — not the monotonic
                // GetTickCount64 — is the clock that answers the question actually asked;
                // if the wall clock is stepped, the wait must end at the new calendar time.
                //   deadline(100ns since 1601) = (tv_sec + 11644473600) * 1e7 + tv_nsec/100
                // 11644473600 is the seconds between the FILETIME epoch (1601-01-01) and the
                // POSIX epoch that time_t counts from; the product peaks near 1.4e17, four
                // orders inside i64.
                begin(sym, rSig_i32_2pV);
                MirInstId const mtx = builder.addArg(0, pVoid);
                MirInstId const tp  = builder.addArg(1, pVoid);
                MirInstId const tpSec  = loadAt(tp, 0, i64Ty);     // tv_sec  — 8 bytes @0
                MirInstId const tpNsRaw = loadAt(tp, 8, i32Ty);    // tv_nsec — 4 bytes @8 ★
                MirInstId const tpNs     = un(MirOpcode::SExt, tpNsRaw, i64Ty);
                MirInstId const epochSec = bin(MirOpcode::Add, tpSec, i64c(11644473600LL), i64Ty);
                MirInstId const secTicks = bin(MirOpcode::Mul, epochSec, i64c(10000000), i64Ty);
                MirInstId const nsTicks  = bin(MirOpcode::SDiv, tpNs, i64c(100), i64Ty);
                MirInstId const deadline = bin(MirOpcode::Add, secTicks, nsTicks, i64Ty);
                MirInstId const ftSlot =
                    builder.addInst(MirOpcode::Alloca, {}, pVoid, /*bytes=*/8);
                MirBlockId const tryBB  = builder.createBlock();
                MirBlockId const chkBB  = builder.createBlock();
                MirBlockId const napBB  = builder.createBlock();
                MirBlockId const gotBB  = builder.createBlock();
                MirBlockId const lateBB = builder.createBlock();
                builder.addBr(tryBB);
                // try: TryEnterCriticalSection returns NONZERO on acquisition
                builder.beginBlock(tryBB);
                MirInstId const got = call1("TryEnterCriticalSection", hSig_i32_pV, i32Ty, mtx);
                builder.addCondBr(bin(MirOpcode::ICmpNe, got, i32c(0), boolTy), gotBB, chkBB);
                // check: has the calendar deadline passed?
                builder.beginBlock(chkBB);
                call1("GetSystemTimeAsFileTime", hSig_v_pV, InvalidType, ftSlot);
                MirInstId const nowFt = un(MirOpcode::Load, ftSlot, i64Ty);
                builder.addCondBr(bin(MirOpcode::ICmpSge, nowFt, deadline, boolTy),
                                  lateBB, napBB);
                // nap: yield the CPU for a millisecond, then retry (the back edge)
                builder.beginBlock(napBB);
                call1("Sleep", hSig_v_u32, InvalidType, u32c(1));
                builder.addBr(tryBB);
                builder.beginBlock(gotBB);
                builder.addReturn(i32c(0));      // thrd_success
                builder.beginBlock(lateBB);
                builder.addReturn(i32c(4));      // thrd_timedout

            } else if (recipe == "cnd_timedwait") {
                // C11 7.26.3.5 — atomically release the mutex and wait until signalled or
                // until the CALENDAR time `time_point`. `SleepConditionVariableCS` IS a
                // native timed wait, so this is a single block: only the conversion from an
                // ABSOLUTE deadline to Win32's RELATIVE millisecond timeout is ours.
                //   deltaMs = ceil((deadline100ns - now100ns) / 10000), clamped to
                //             [0, 0xFFFFFFFE]
                // The high clamp keeps the INFINITE sentinel (0xFFFFFFFF) unreachable — a
                // deadline far enough out would otherwise turn a TIMED wait into a
                // permanent one; the low clamp turns an already-passed deadline into an
                // immediate poll, which is what C11 asks for.
                // ★ THE VERDICT NEEDS GetLastError AND IS COMPUTED BRANCHLESSLY:
                // SleepConditionVariableCS returns 0 for BOTH a timeout and a real failure,
                // so mapping every zero to thrd_timedout would report a programming error as
                // a timeout. ERROR_TIMEOUT is 1460.
                //   result = (1 - succeeded) * (2 + 2*isTimeout)
                //          → succeeded: 0 (thrd_success) · timeout: 4 · else: 2 (thrd_error)
                // GetLastError is called unconditionally; on the success path its value is
                // multiplied away by (1 - succeeded) == 0, and it has no side effect beyond
                // reading the calling thread's own TLS slot.
                begin(sym, rSig_cnd_timedwait);
                MirInstId const cnd = builder.addArg(0, pVoid);
                MirInstId const mtx = builder.addArg(1, pVoid);
                MirInstId const tp  = builder.addArg(2, pVoid);
                MirInstId const tpSec   = loadAt(tp, 0, i64Ty);    // tv_sec  — 8 bytes @0
                MirInstId const tpNsRaw = loadAt(tp, 8, i32Ty);    // tv_nsec — 4 bytes @8 ★
                MirInstId const tpNs     = un(MirOpcode::SExt, tpNsRaw, i64Ty);
                MirInstId const epochSec = bin(MirOpcode::Add, tpSec, i64c(11644473600LL), i64Ty);
                MirInstId const secTicks = bin(MirOpcode::Mul, epochSec, i64c(10000000), i64Ty);
                MirInstId const nsTicks  = bin(MirOpcode::SDiv, tpNs, i64c(100), i64Ty);
                MirInstId const deadline = bin(MirOpcode::Add, secTicks, nsTicks, i64Ty);
                MirInstId const ftSlot =
                    builder.addInst(MirOpcode::Alloca, {}, pVoid, /*bytes=*/8);
                call1("GetSystemTimeAsFileTime", hSig_v_pV, InvalidType, ftSlot);
                MirInstId const nowFt   = un(MirOpcode::Load, ftSlot, i64Ty);
                MirInstId const leftFt  = bin(MirOpcode::Sub, deadline, nowFt, i64Ty);
                MirInstId const roundUp = bin(MirOpcode::Add, leftFt, i64c(9999), i64Ty);
                MirInstId const deltaMs = bin(MirOpcode::SDiv, roundUp, i64c(10000), i64Ty);
                MirInstId const waitMs  = clampHigh(clampLow0(deltaMs), 0xFFFFFFFELL);
                MirInstId const waitMs32 = un(MirOpcode::Trunc, waitMs, u32Ty);
                MirInstId const woke =
                    call3("SleepConditionVariableCS", hSig_i32_pVpVu32, i32Ty, cnd, mtx, waitMs32);
                MirInstId const succeeded = isNonZeroI32(woke);
                MirInstId const lastErr   = call0("GetLastError", hSig_u32_void, u32Ty);
                MirInstId const timeoutC  = u32c(1460);
                MirInstId const isTimeout =
                    un(MirOpcode::ZExt,
                       bin(MirOpcode::ICmpEq, lastErr, timeoutC, boolTy), i32Ty);
                MirInstId const failed  = bin(MirOpcode::Sub, i32c(1), succeeded, i32Ty);
                MirInstId const doubled = bin(MirOpcode::Mul, isTimeout, i32c(2), i32Ty);
                MirInstId const code    = bin(MirOpcode::Add, i32c(2), doubled, i32Ty);
                builder.addReturn(bin(MirOpcode::Mul, failed, code, i32Ty));

            } else if (recipe == "thrd_equal") {
                // C11 7.26.5.4 — nonzero iff the two thrd_t values name the SAME thread.
                // ★ IT CANNOT COMPARE THE HANDLES, and that is the whole content of this
                // arm. pe's thrd_t IS a HANDLE, but `thrd_current()` answers kernel32's
                // PSEUDO-handle (HANDLE)-2 (see this file's thrd_current arm, which names
                // that wart), so a pointer comparison would report NOT-EQUAL for the very
                // thread doing the asking — a wrong answer with no fault, which is the
                // failure class the bar forbids outright. `GetThreadId` RESOLVES the
                // pseudo-handle to the caller's real thread id, so comparing ids is correct
                // for every pair of handles — and it is what MSVC's own <threads.h> does,
                // storing an id beside the handle and comparing the id.
                // ⚠ BOTH `Arg`s ARE MATERIALIZED BEFORE THE FIRST CALL, and that is a
                // correctness rule rather than a style one: an `Arg` reads the incoming
                // parameter location, which a Call is free to clobber. Emitting `Arg 1`
                // after the first GetThreadId call read rdx AFTER kernel32 had used it, so
                // the two ids compared UNEQUAL for one and the same handle — measured, as
                // an exit 10 out of this cycle's own thrd_equal witness on its first run.
                begin(sym, rSig_i32_2pV);
                MirInstId const lhsHandle = builder.addArg(0, pVoid);
                MirInstId const rhsHandle = builder.addArg(1, pVoid);
                MirInstId const lhs = call1("GetThreadId", hSig_u32_pV, u32Ty, lhsHandle);
                MirInstId const rhs = call1("GetThreadId", hSig_u32_pV, u32Ty, rhsHandle);
                builder.addReturn(
                    un(MirOpcode::ZExt, bin(MirOpcode::ICmpEq, lhs, rhs, boolTy), i32Ty));

            } else {
                // A recipe id present in the descriptor vocabulary but with NO win32 arm — a
                // vocab/switch drift. Fail loud (never a silently-undefined shim). The loader
                // closed-vocab guard makes this unreachable in practice; this is the backstop.
                emitErr(reporter, "synthesizeThreadsShim: no win32 synth arm for recipe id '"
                                      + recipe + "' (D-CSUBSET-C11-THREADS-HEADER vocab/switch drift)");
                return false;
            }
        } else {  // LibrarySynthVehicle::Pthread
            // ══ pthread vehicle — bodies over Darwin libSystem pthread primitives ══
            // pthread_* return 0 on success + errno (nonzero) on failure — the INVERSE of
            // win32's nonzero-on-success (see isNonZeroI32).
            // ── mutex ──
            if (recipe == "mtx_init") {
                // D-CSUBSET-C11-THREADS-MACHO-MTX-PLAIN-RECURSIVE — the recipe now HONOURS
                // the C11 `type` argument instead of passing a NULL mutexattr.
                //
                // ── WHAT WAS WRONG, AND WHY IT WAS THE WORST SHAPE OF WRONG ─────────
                // A NULL attr yields PTHREAD_MUTEX_DEFAULT, and on Darwin that is
                // PTHREAD_MUTEX_NORMAL — non-recursive. So `mtx_init(&m, mtx_recursive)`
                // produced a mutex whose same-thread re-lock BLOCKS FOREVER: not a wrong
                // answer, not a diagnostic, a HANG. ✔MEASURED on the operator's macOS
                // 26.6.2 arm64 host: with a NULL attr the relock did not return inside a
                // 2 s bounded wait; with a RECURSIVE attr it returned immediately.
                // It was filed as unobservable because Cycle 1 had no `thrd_create`;
                // P49 landed `thrd_create` on all three formats, so the program can now
                // be multi-threaded and the deadlock is reachable.
                //
                // ── WHY THIS IS BELOW THE UNION, NOT A FREE CHOICE ─────────────────
                // The RECURSIVE direction is not undefined behaviour — C11 7.26.4.2 /
                // C23 7.28.4.2 make `mtx_plain | mtx_recursive` a mutex the owning
                // thread MAY re-lock, and both non-DSS references that ship
                // <threads.h> honour it: ✔MEASURED, glibc 2.39 (through gcc 13.3.0 AND
                // clang 18.1.3, probed separately) and MSVC 19.51's own <threads.h>
                // each let the relock proceed. DSS deadlocking there is strictly below
                // (gcc ∪ clang ∪ MSVC) ∪ ISO C.
                //
                // ── THE SHAPE: SINGLE BLOCK, ONE ALLOCA, BRANCHLESS SELECT ─────────
                // The row proposed "multi-block shim recipes OR a branchless
                // attr-select", on the 2026-07-14 premise that an `Alloca` would break
                // this pass's single-block/alloca-free invariant. THAT INVARIANT NO
                // LONGER EXISTS — P49's timed recipes (`thrd_sleep`, `mtx_timedlock`,
                // `cnd_timedwait`) and Cycle 2's `thrd_join` already Alloca and already
                // create blocks on BOTH vehicles. Neither escape hatch is needed: the
                // attr dance is straight-line, and the kind is selected with the same
                // Mul-by-a-zero-extended-predicate idiom the clamps above use, so the
                // recipe stays ONE block and adds no CFG for `rederiveStructCfMarkers`.
                //
                // ⚠ THE THREE LITERALS BELOW ARE A SECOND OWNER OF A DARWIN FACT, and
                // that is stated rather than hidden — the same standing seam as this
                // file's `hSig_*` block and its `struct timespec` offsets, tracked as
                // D-MIR-SYNTH-SHIM-HELPER-SIGNATURES-DUPLICATE-THE-DESCRIPTOR. It
                // cannot be dissolved here: `synthesizeThreadsShim` receives a recipe
                // map and a vehicle, never a descriptor, so there is no config channel
                // reaching this seam. What binds the two owners is a RUNTIME witness,
                // not a comment — `examples/c/c11_mtx_recursive` re-locks a
                // `mtx_recursive` mutex on the thread that owns it, so a wrong kind
                // constant HANGS the example instead of passing quietly.
                //   ✔MEASURED on macOS 26.6.2 arm64 (and the x86_64 arm links):
                //     PTHREAD_MUTEX_NORMAL = 0, PTHREAD_MUTEX_RECURSIVE = 2,
                //     PTHREAD_MUTEX_DEFAULT = 0, sizeof(pthread_mutexattr_t) = 16,
                //     _Alignof(pthread_mutexattr_t) = 8.
                // Note NORMAL and RECURSIVE differ from glibc's (1 and 2 there, with
                // NORMAL 0): this constant is a property of the PTHREAD VEHICLE, which
                // today is Darwin's only. An elf target never reaches here — glibc
                // exports the C11 names and elf binds them as ordinary FFI.
                static constexpr std::int32_t kC11MtxRecursiveBit    = 1;  // C11 mtx_recursive
                static constexpr std::int32_t kPthreadMutexNormal    = 0;  // Darwin PTHREAD_MUTEX_NORMAL
                static constexpr std::int32_t kPthreadMutexRecursive = 2;  // Darwin PTHREAD_MUTEX_RECURSIVE
                static constexpr std::int32_t kPthreadMutexAttrBytes = 16; // sizeof(pthread_mutexattr_t)
                // The branchless select is `NORMAL + (RECURSIVE - NORMAL) * wants`, and
                // the Add folds away only because NORMAL is zero. Assert that here, so a
                // future Darwin renumbering is a BUILD failure rather than a mutex
                // silently initialized to kind 2 when it should be kind NORMAL.
                static_assert(kPthreadMutexNormal == 0,
                              "the branchless kind select below drops the NORMAL base term");
                begin(sym, rSigP_mtx_init);
                // ⚠ BOTH `Arg`s ARE MATERIALIZED BEFORE THE FIRST CALL — an `Arg` reads
                // the incoming parameter location and a Call is free to clobber it. This
                // file's `thrd_equal` arm records that as a MEASURED wrong answer, not a
                // style rule, and this recipe is the second one to read a second argument.
                MirInstId const m    = builder.addArg(0, pVoid);
                MirInstId const type = builder.addArg(1, i32Ty);   // the C11 mtx type — now READ
                // The attr slot. `payload2` (the over-alignment channel) is left 0 like
                // every sibling Alloca here: 0 means "no over-alignment recorded", so the
                // frame places the slot on `ccStackAlignment()` = 16, which already
                // exceeds the measured _Alignof(pthread_mutexattr_t) = 8.
                MirInstId const attr =
                    builder.addInst(MirOpcode::Alloca, {}, pVoid, kPthreadMutexAttrBytes);
                call1("pthread_mutexattr_init", phSig_i32_pV, i32Ty, attr);
                // wants = ((type & mtx_recursive) != 0) ? 1 : 0.  The MASK rather than an
                // equality is what C11 asks for: the four legal values are mtx_plain,
                // mtx_timed, and either of those OR'd with mtx_recursive, so `== 1` would
                // miss `mtx_timed | mtx_recursive` (3) — the exact value glibc and MSVC
                // both treat as recursive (✔MEASURED, both).
                MirInstId const recursiveBit =
                    bin(MirOpcode::And, type, i32c(kC11MtxRecursiveBit), i32Ty);
                MirInstId const wants = isNonZeroI32(recursiveBit);
                MirInstId const kind  = bin(MirOpcode::Mul, wants,
                                            i32c(kPthreadMutexRecursive - kPthreadMutexNormal),
                                            i32Ty);
                call2("pthread_mutexattr_settype", phSig_i32_pVi32, i32Ty, attr, kind);
                call2("pthread_mutex_init", phSig_i32_pVpV, i32Ty, m, attr);
                // The attr is a VALUE copied into the mutex at init, so destroying it
                // immediately is correct and leaks nothing (Darwin's destroy only clears
                // the signature word).
                call1("pthread_mutexattr_destroy", phSig_i32_pV, i32Ty, attr);
                builder.addReturn(i32c(0));
            } else if (recipe == "mtx_lock") {   // pthread_mutex_lock(m); ret success (discard)
                begin(sym, rSigP_i32_pV);
                call1("pthread_mutex_lock", phSig_i32_pV, i32Ty, builder.addArg(0, pVoid));
                builder.addReturn(i32c(0));
            } else if (recipe == "mtx_unlock") { // pthread_mutex_unlock(m); ret success (discard)
                begin(sym, rSigP_i32_pV);
                call1("pthread_mutex_unlock", phSig_i32_pV, i32Ty, builder.addArg(0, pVoid));
                builder.addReturn(i32c(0));
            } else if (recipe == "mtx_trylock") {// pthread_mutex_trylock: 0=success, EBUSY else
                begin(sym, rSigP_i32_pV);
                MirInstId const r = call1("pthread_mutex_trylock", phSig_i32_pV, i32Ty,
                                          builder.addArg(0, pVoid));
                builder.addReturn(isNonZeroI32(r));          // r!=0 (EBUSY) → thrd_busy=1; r==0 → success
            } else if (recipe == "mtx_destroy") {// pthread_mutex_destroy(m); (void)
                begin(sym, rSigP_v_pV);
                call1("pthread_mutex_destroy", phSig_i32_pV, i32Ty, builder.addArg(0, pVoid));
                builder.addReturn();

            // ── condition variable ──
            } else if (recipe == "cnd_init") {   // pthread_cond_init(c, NULL); ret success
                begin(sym, rSigP_i32_pV);
                call2("pthread_cond_init", phSig_i32_pVpV, i32Ty, builder.addArg(0, pVoid), nullPtr());
                builder.addReturn(i32c(0));
            } else if (recipe == "cnd_signal") { // pthread_cond_signal(c); ret success
                begin(sym, rSigP_i32_pV);
                call1("pthread_cond_signal", phSig_i32_pV, i32Ty, builder.addArg(0, pVoid));
                builder.addReturn(i32c(0));
            } else if (recipe == "cnd_broadcast") { // pthread_cond_broadcast(c); ret success
                begin(sym, rSigP_i32_pV);
                call1("pthread_cond_broadcast", phSig_i32_pV, i32Ty, builder.addArg(0, pVoid));
                builder.addReturn(i32c(0));
            } else if (recipe == "cnd_wait") {   // pthread_cond_wait(c, m); ret success
                begin(sym, rSigP_cnd_wait);
                MirInstId const c = builder.addArg(0, pVoid);
                MirInstId const m = builder.addArg(1, pVoid);
                call2("pthread_cond_wait", phSig_i32_pVpV, i32Ty, c, m);
                builder.addReturn(i32c(0));
            } else if (recipe == "cnd_destroy") {// pthread_cond_destroy(c); (void)
                begin(sym, rSigP_v_pV);
                call1("pthread_cond_destroy", phSig_i32_pV, i32Ty, builder.addArg(0, pVoid));
                builder.addReturn();

            // ── thread-specific storage (pthread_key_* — key_create's dtor == tss_dtor_t, so
            //    C11 destructor semantics hold; pthread_key_t is u64 on macho) ──
            } else if (recipe == "tss_create") { // pthread_key_create(k, dtor); ret (r!=0)?error:0
                begin(sym, rSigP_tss_create);
                MirInstId const k    = builder.addArg(0, pU64);
                MirInstId const dtor = builder.addArg(1, pVoid);
                MirInstId const r    = call2("pthread_key_create", phSig_i32_pU64pV, i32Ty, k, dtor);
                std::array<MirInstId, 2> mul{isNonZeroI32(r), i32c(2)};
                builder.addReturn(builder.addInst(MirOpcode::Mul, mul, i32Ty));         // r!=0 → thrd_error(2)
            } else if (recipe == "tss_get") {    // ret pthread_getspecific(k)
                begin(sym, rSigP_pV_u64);
                builder.addReturn(call1("pthread_getspecific", phSig_pV_u64, pVoid,
                                        builder.addArg(0, u64Ty)));
            } else if (recipe == "tss_set") {    // ret pthread_setspecific(k,v)?error:0
                begin(sym, rSigP_i32_u64pV);
                MirInstId const k = builder.addArg(0, u64Ty);
                MirInstId const v = builder.addArg(1, pVoid);
                MirInstId const r = call2("pthread_setspecific", phSig_i32_u64pV, i32Ty, k, v);
                std::array<MirInstId, 2> mul{isNonZeroI32(r), i32c(2)};
                builder.addReturn(builder.addInst(MirOpcode::Mul, mul, i32Ty));         // r!=0 → error(2)
            } else if (recipe == "tss_delete") { // pthread_key_delete(k); (void)
                begin(sym, rSigP_v_u64);
                call1("pthread_key_delete", phSig_i32_u64, i32Ty, builder.addArg(0, u64Ty));
                builder.addReturn();

            // ── thread management (the Cycle-1 usable subset; thrd_create/join are Cycle 2) ──
            } else if (recipe == "thrd_current") { // ret pthread_self()
                begin(sym, rSigP_u64_void);
                builder.addReturn(call0("pthread_self", phSig_u64_void, u64Ty));
            } else if (recipe == "thrd_yield") { // sched_yield(); (void)
                begin(sym, rSigP_v_void);
                call0("sched_yield", phSig_i32_void, i32Ty);
                builder.addReturn();
            } else if (recipe == "thrd_exit") {  // pthread_exit((void*)(i64)res); (void, noreturn)
                begin(sym, rSigP_v_i32);
                MirInstId const res = builder.addArg(0, i32Ty);
                // Widen the i32 res to i64, then to ptr<void> — the C idiom
                // `pthread_exit((void*)(intptr_t)res)`. An EXPLICIT SExt (never an implicit
                // width/kind pun): SExt is the sign-preserving widening AND the only i32→64
                // form arm64 realizes this cycle (SXTW; ZExt admits no I32 source —
                // D-CSUBSET-32BIT-ALU-FORMS). The retval is unretrievable without thrd_join (Cycle 2).
                std::array<MirInstId, 1> se{res};
                MirInstId const res64  = builder.addInst(MirOpcode::SExt, se, i64Ty);
                std::array<MirInstId, 1> itp{res64};
                MirInstId const resPtr = builder.addInst(MirOpcode::IntToPtr, itp, pVoid);
                call1("pthread_exit", phSig_v_pV, InvalidType, resPtr);
                builder.addReturn();             // dead (pthread_exit noreturn) — a terminator is required
            } else if (recipe == "thrd_detach") {// pthread_detach(t); ret (r!=0)?error:0
                begin(sym, rSigP_i32_u64);
                MirInstId const r = call1("pthread_detach", phSig_i32_u64, i32Ty,
                                          builder.addArg(0, u64Ty));
                std::array<MirInstId, 2> mul{isNonZeroI32(r), i32c(2)};
                builder.addReturn(builder.addInst(MirOpcode::Mul, mul, i32Ty));         // r!=0 → error(2)

            // ── Cycle 2 (D-CSUBSET-C11-THREADS-TRAMPOLINES) — macho pthread trampolines ──
            } else if (recipe == "thrd_create") {
                // DIRECT-PASS (no closure): the C11 start routine int(*)(void*) is handed
                // straight to pthread_create's void*(*)(void*) — the arg is one ptr (x0) and the
                // int return lands in the low 32 of x0 that thrd_join reads back (validated
                // end-to-end on the Mac). pthread_create(thr, NULL attr, func, arg); *thr is
                // written by pthread_create itself. ret (r!=0)?thrd_error(2):thrd_success(0).
                begin(sym, rSigP_thrd_create);
                MirInstId const thr  = builder.addArg(0, pVoid);   // thrd_t* (out)
                MirInstId const func = builder.addArg(1, pVoid);   // thrd_start_t == start routine
                MirInstId const arg  = builder.addArg(2, pVoid);   // void* arg
                MirInstId const r    = call4("pthread_create", phSig_i32_4pV, i32Ty,
                                             thr, nullPtr(), func, arg);
                std::array<MirInstId, 2> mul{isNonZeroI32(r), i32c(2)};
                builder.addReturn(builder.addInst(MirOpcode::Mul, mul, i32Ty));         // r!=0 → thrd_error(2)

            } else if (recipe == "call_once") {
                // pthread_once(flag, func) — the SAME shape as C11 call_once (once_flag* +
                // void(*)(void)), so a DIRECT pass; NO adapter (unlike pe's InitOnceExecuteOnce
                // via __dss_once_tramp). The macho once_flag is seeded with the macOS
                // PTHREAD_ONCE_INIT magic sig via the ONCE_FLAG_INIT variant (threads.json).
                begin(sym, rSigP_call_once);
                MirInstId const flag = builder.addArg(0, pVoid);
                MirInstId const func = builder.addArg(1, pVoid);
                call2("pthread_once", phSig_i32_pVpV, i32Ty, flag, func);   // int ret discarded (call_once is void)
                builder.addReturn();

            } else if (recipe == "thrd_join") {
                // MULTI-block: pthread_join wants a void** out-slot (8 bytes) but the C11 `res`
                // is an int* (4) — so Alloca an 8-byte slot, join into it, then `if (res)`
                // truncate the returned void* to int and store (a NULL `res` must not fault).
                // ret thrd_success. Blocks get default markers; the module-wide rederive at the
                // end makes them canonical (IfThen/IfJoin).
                begin(sym, rSigP_thrd_join);                       // opens the ENTRY block
                MirInstId const t    = builder.addArg(0, u64Ty);   // thrd_t (pthread_t) by value
                MirInstId const res  = builder.addArg(1, pI32);    // int* (exit-code out; may be NULL)
                MirInstId const slot = builder.addInst(MirOpcode::Alloca, {}, pVoid, /*bytes=*/8); // void* retval slot
                call2("pthread_join", phSig_i32_u64pU, i32Ty, t, slot);   // pthread_join(t, &slot)
                std::array<MirInstId, 1> rp{res};
                MirInstId const resInt = builder.addInst(MirOpcode::PtrToInt, rp, i64Ty);
                std::array<MirInstId, 2> ne{resInt, konst(0, TypeKind::I64, i64Ty)};
                MirInstId const cond   = builder.addInst(MirOpcode::ICmpNe, ne, boolTy);  // res != 0
                MirBlockId const thenBB = builder.createBlock();   // marker set by the module-wide rederive
                MirBlockId const joinBB = builder.createBlock();
                builder.addCondBr(cond, thenBB, joinBB);
                // then: *res = (int)(intptr_t)(*slot)  — the void* the thread returned, truncated
                builder.beginBlock(thenBB);
                std::array<MirInstId, 1> ld{slot};
                MirInstId const rv    = builder.addInst(MirOpcode::Load, ld, pVoid);       // void* retval
                std::array<MirInstId, 1> pti{rv};
                MirInstId const rvInt = builder.addInst(MirOpcode::PtrToInt, pti, i64Ty);
                std::array<MirInstId, 1> tr{rvInt};
                MirInstId const rv32  = builder.addInst(MirOpcode::Trunc, tr, i32Ty);
                std::array<MirInstId, 2> st{rv32, res};
                builder.addInst(MirOpcode::Store, st, InvalidType);   // *res = (int)rv
                builder.addBr(joinBB);
                // join: ret thrd_success(0)
                builder.beginBlock(joinBB);
                builder.addReturn(i32c(0));

            // ── Cycle 3 (D-CSUBSET-C11-THREADS-TIMED) — macho timed waits + thrd_equal ──
            } else if (recipe == "thrd_sleep") {
                // DIRECT PASS to `nanosleep(req, rem)`. The two agree on everything that
                // matters: the same `struct timespec` in, the same optional remainder out,
                // and the same return convention — 0 when the interval elapsed, -1 when a
                // signal cut it short, which is exactly C11's "-1 if it has been interrupted
                // by a signal". No conversion, no rounding, so no place for one to hide.
                // Unlike the pe arm this needs no loop: nanosleep takes the full 64-bit
                // seconds field rather than a 32-bit millisecond count.
                begin(sym, rSigP_i32_2pV);
                MirInstId const dur = builder.addArg(0, pVoid);
                MirInstId const rem = builder.addArg(1, pVoid);
                builder.addReturn(call2("nanosleep", phSig_i32_pVpV, i32Ty, dur, rem));

            } else if (recipe == "mtx_timedlock") {
                // ★ DARWIN HAS NO `pthread_mutex_timedlock`, and that is MEASURED, not
                // assumed: a link probe on macOS 26.6.2 / Apple clang 21 resolved
                // pthread_cond_timedwait, pthread_cond_timedwait_relative_np, nanosleep,
                // pthread_equal, clock_gettime, gettimeofday and mach_absolute_time — and
                // failed on pthread_mutex_timedlock alone. So this arm is the SAME
                // trylock/deadline loop the win32 arm runs, over the Darwin primitives; the
                // two vehicles disagree about the primitives, not about the shape.
                //   deadlineNs = tv_sec*1e9 + tv_nsec   (LP64: BOTH fields 8 bytes)
                // CLOCK_REALTIME is 0 — ✔MEASURED by a RUN probe on the host, not read off
                // a man page — and it is the right clock for the same reason the win32 arm
                // reads the wall clock: C11 states the deadline as a TIME_UTC calendar time.
                begin(sym, rSigP_i32_2pV);
                MirInstId const mtx = builder.addArg(0, pVoid);
                MirInstId const tp  = builder.addArg(1, pVoid);
                MirInstId const tpSec    = loadAt(tp, 0, i64Ty);
                MirInstId const secNs    = bin(MirOpcode::Mul, tpSec, i64c(1000000000), i64Ty);
                MirInstId const tpNs     = loadAt(tp, 8, i64Ty);  // tv_nsec — 8 bytes @8 (LP64) ★
                MirInstId const deadline = bin(MirOpcode::Add, secNs, tpNs, i64Ty);
                MirInstId const nowSlot =
                    builder.addInst(MirOpcode::Alloca, {}, pVoid, /*bytes=*/16);  // struct timespec
                MirInstId const napSlot =
                    builder.addInst(MirOpcode::Alloca, {}, pVoid, /*bytes=*/16);  // {0, 1ms}
                std::array<MirInstId, 2> napSec{i64c(0), napSlot};
                builder.addInst(MirOpcode::Store, napSec, InvalidType);
                std::array<MirInstId, 2> napNsec{i64c(1000000), byteOffset(napSlot, 8)};
                builder.addInst(MirOpcode::Store, napNsec, InvalidType);
                MirBlockId const tryBB  = builder.createBlock();
                MirBlockId const chkBB  = builder.createBlock();
                MirBlockId const napBB  = builder.createBlock();
                MirBlockId const gotBB  = builder.createBlock();
                MirBlockId const lateBB = builder.createBlock();
                builder.addBr(tryBB);
                // try: pthread_mutex_trylock returns 0 on acquisition (the INVERSE of Win32)
                builder.beginBlock(tryBB);
                MirInstId const rc = call1("pthread_mutex_trylock", phSig_i32_pV, i32Ty, mtx);
                builder.addCondBr(bin(MirOpcode::ICmpEq, rc, i32c(0), boolTy), gotBB, chkBB);
                // check: has the calendar deadline passed?
                builder.beginBlock(chkBB);
                call2("clock_gettime", phSig_i32_i32pV, i32Ty, i32c(0), nowSlot);
                MirInstId const nowSec   = loadAt(nowSlot, 0, i64Ty);
                MirInstId const nowSecNs = bin(MirOpcode::Mul, nowSec, i64c(1000000000), i64Ty);
                MirInstId const nowFrac  = loadAt(nowSlot, 8, i64Ty);
                MirInstId const nowNs    = bin(MirOpcode::Add, nowSecNs, nowFrac, i64Ty);
                builder.addCondBr(bin(MirOpcode::ICmpSge, nowNs, deadline, boolTy),
                                  lateBB, napBB);
                // nap: yield the CPU for a millisecond, then retry (the back edge)
                builder.beginBlock(napBB);
                call2("nanosleep", phSig_i32_pVpV, i32Ty, napSlot, nullPtr());
                builder.addBr(tryBB);
                builder.beginBlock(gotBB);
                builder.addReturn(i32c(0));      // thrd_success
                builder.beginBlock(lateBB);
                builder.addReturn(i32c(4));      // thrd_timedout

            } else if (recipe == "cnd_timedwait") {
                // `pthread_cond_timedwait(c, m, abstime)` takes the SAME absolute TIME_UTC
                // timespec C11 does, so — unlike the win32 arm, which must convert to a
                // relative millisecond count — this is a straight pass with only the return
                // convention to map. It returns 0, or an errno: ETIMEDOUT on the deadline,
                // EINVAL/EPERM on a programming error, so collapsing every nonzero to
                // thrd_timedout would report a misuse as a timeout.
                //   result = (1 - succeeded) * (2 + 2*isTimeout)
                //          → 0 (thrd_success) · 4 (thrd_timedout) · 2 (thrd_error)
                // ETIMEDOUT is 60 on Darwin (110 on Linux) — the value errno.json's own
                // macho arm carries, ✔re-measured here by a RUN probe on the host. This is
                // also what glibc's cnd_timedwait does: map the errno, do not guess.
                begin(sym, rSigP_cnd_timedwait);
                MirInstId const cnd = builder.addArg(0, pVoid);
                MirInstId const mtx = builder.addArg(1, pVoid);
                MirInstId const tp  = builder.addArg(2, pVoid);
                MirInstId const rc =
                    call3("pthread_cond_timedwait", phSig_i32_3pV, i32Ty, cnd, mtx, tp);
                MirInstId const succeeded = isZeroI32(rc);
                MirInstId const etimedout = i32c(60);
                MirInstId const isTimeout =
                    un(MirOpcode::ZExt,
                       bin(MirOpcode::ICmpEq, rc, etimedout, boolTy), i32Ty);
                MirInstId const failed  = bin(MirOpcode::Sub, i32c(1), succeeded, i32Ty);
                MirInstId const doubled = bin(MirOpcode::Mul, isTimeout, i32c(2), i32Ty);
                MirInstId const code    = bin(MirOpcode::Add, i32c(2), doubled, i32Ty);
                builder.addReturn(bin(MirOpcode::Mul, failed, code, i32Ty));

            } else if (recipe == "thrd_equal") {
                // DIRECT PASS to `pthread_equal`, which has C11's own contract (nonzero iff
                // the two ids name the same thread) and takes the same by-value u64 the
                // macho thrd_t is. No pseudo-handle problem here: `thrd_current()` on this
                // vehicle is `pthread_self()`, a real id, so the pe arm's GetThreadId
                // indirection has nothing to correct.
                begin(sym, rSigP_thrd_equal);
                MirInstId const lhs = builder.addArg(0, u64Ty);
                MirInstId const rhs = builder.addArg(1, u64Ty);
                builder.addReturn(call2("pthread_equal", phSig_i32_2u64, i32Ty, lhs, rhs));

            } else {
                // A recipe id present in the descriptor vocabulary but with NO pthread arm — a
                // vocab/switch drift. Fail loud (never a silently-undefined shim). The loader
                // closed-vocab guard makes this unreachable in practice; this is the backstop.
                emitErr(reporter, "synthesizeThreadsShim: no pthread synth arm for recipe id '"
                                      + recipe + "' (D-CSUBSET-C11-THREADS-MACHO vocab/switch drift)");
                return false;
            }
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
