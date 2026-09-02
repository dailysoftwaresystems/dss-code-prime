// D-CSUBSET-C11-THREADS-MACHO-MTX-PLAIN-RECURSIVE +
// D-CSUBSET-C11-THREADS-MTX-PLAIN-RECURSIVE — `mtx_init`'s C11 `type` argument, and
// what each synthesis vehicle does with it.
//
// ── THE DEFECT, AND WHY IT WAS THE WORST SHAPE OF ONE ───────────────────────
// The macho `mtx_init` recipe passed a NULL mutexattr. On Darwin a NULL attr is
// PTHREAD_MUTEX_DEFAULT == PTHREAD_MUTEX_NORMAL — NON-recursive — so
// `mtx_init(&m, mtx_recursive)` produced a mutex whose same-thread re-lock BLOCKS
// FOREVER. Not a wrong value, not a diagnostic: a HANG, from a `type` argument
// that was accepted and discarded. C11 7.26.4.2 / C23 7.28.4.2 make
// `mtx_plain | mtx_recursive` a mutex the owning thread MAY re-lock, and BOTH
// non-DSS references that ship <threads.h> honour it (✔MEASURED: glibc 2.39
// through gcc 13.3.0 and clang 18.1.3, probed separately, and MSVC 19.51's own
// <threads.h>), so DSS deadlocking there sat below (gcc ∪ clang ∪ MSVC) ∪ ISO C.
//
// ── WHY THE pe HALF IS A CONTROL AND NOT A SECOND FIX ───────────────────────
// A Win32 CRITICAL_SECTION is always recursion-capable, so pe already satisfies
// the direction the standard actually constrains. The other direction —
// same-thread re-lock of an `mtx_plain` mutex — is UNDEFINED (C11 7.26.4.3 /
// C23 7.28.4.3: "If the mutex is non-recursive, it shall not be locked by the
// calling thread", a `shall` on the caller outside a Constraints clause), and
// ✔MEASURED the three references disagree three ways on that exact program:
// glibc BLOCKS, MSVC FAIL-FAST ABORTS (0xC0000409), pe proceeds. So the win32
// arm's job here is to keep NOT growing a recursion guard, which is why it is
// pinned as an explicit negative rather than left unmentioned.
//
// ── WHY THIS PIN IS NOT REDUNDANT WITH THE CORPUS WITNESS ───────────────────
// `examples/c/c11_mtx_recursive` proves the BEHAVIOUR and is the stronger
// instrument — a mutex of the wrong kind hangs it. But its macho arm RUNS ONLY
// ON A MAC. This file asserts both vehicles' emitted bodies from ANY host, so a
// regression to the NULL attr reds on the Windows leg instead of waiting for
// someone to reach Darwin hardware.
//
// ── WHY THE ASSERTIONS ARE OPERAND-LEVEL ────────────────────────────────────
// "the body contains a pthread_mutexattr_settype call" is inert: a body could
// call it on the right attr with a CONSTANT kind and never read `type` at all,
// which is precisely the defect being fixed, one indirection along. So the kind
// operand is traced back through its whole chain — Mul(ZExt(ICmpNe(And(Arg#1,
// mtx_recursive), 0)), RECURSIVE-NORMAL) — and BOTH literals are asserted BY
// VALUE. The mask literal is the C11 side (mtx_recursive == 1, so
// `mtx_timed | mtx_recursive` == 3 also selects recursive, which an `== 1`
// equality would miss); the multiplier is the Darwin side
// (PTHREAD_MUTEX_RECURSIVE == 2, ✔MEASURED on macOS 26.6.2 arm64 alongside
// NORMAL == 0, DEFAULT == 0, sizeof(pthread_mutexattr_t) == 16).
// ⚠ The two constants are restated here from the platform contract rather than
// imported from the pass, whose values are file-local `constexpr`s — importing
// them would let any future change to them approve itself.
//
// ── AND THE ROW'S STATED OBSTACLE, PINNED AS REFUTED ────────────────────────
// Both rows were opened 2026-07-14 on the premise that honouring `type` needs an
// `Alloca` and would "break the pass's single-block/alloca-free invariant",
// offering "multi-block shim recipes OR a branchless attr-select" as the two
// ways out. That invariant no longer exists — Cycle 2's `thrd_join` and P49's
// timed recipes already Alloca and already create blocks on both vehicles — and
// neither escape hatch was needed: `SinglBlock` below asserts the fixed recipe
// is still ONE block, so a future rewrite that reaches for a CFG has to argue
// for it.
//
// ── RED-ON-DISABLE (each mutation applied to
//    src/mir/merge/synth_threads_shim.cpp ONE AT A TIME, this binary re-run,
//    then the source restored and verified byte-identical) ──
//   * the whole attr dance -> `call2("pthread_mutex_init", …, m, nullPtr())`
//     (the literal pre-fix body) -> reds `HonoursTheTypeArgument` on the
//     mutexattr imports and `AttrIsTheAllocaNotANullConstant` on the operand.
//   * `kPthreadMutexRecursive` 2 -> 1 (glibc's value, the plausible typo) ->
//     reds `HonoursTheTypeArgument` on the multiplier literal alone.
//   * `kC11MtxRecursiveBit` 1 -> 3 -> reds the same test on the mask literal.
//   * `And(type, …)` -> `And(i32c(1), …)` (kind no longer reads the parameter)
//     -> reds the Arg-ordinal assertion.
//   * a `pthread_mutexattr_settype` on the win32 arm -> reds
//     `Win32VehicleTouchesNoMutexAttr`.

#include "core/types/arg_payload.hpp"          // Arg payload is PACKED (position<<16)|ordinal
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"   // LibrarySynthesis, LibrarySynthVehicle
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/merge/synth_threads_shim.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_verifier.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kMtxInitSym = 10;
constexpr std::uint32_t kMainSym    = 100;

// ── The two platform constants under test, restated from their contracts ──
// C11 7.26.4.2 / C23 7.28.4.2: `mtx_recursive` is a BIT OR'd into the type, so the
// recipe must MASK rather than compare. threads.json declares its value 1.
constexpr std::int64_t kC11MtxRecursive = 1;
// Darwin <pthread.h>: PTHREAD_MUTEX_NORMAL 0, ERRORCHECK 1, RECURSIVE 2, DEFAULT 0
// — ✔MEASURED on macOS 26.6.2 arm64 (and the x86_64 slice links). Note these are
// NOT glibc's numbering; the pthread VEHICLE is Darwin's alone (elf binds the C11
// names directly out of libc.so.6 and never reaches this pass).
constexpr std::int64_t kPthreadMutexNormal    = 0;
constexpr std::int64_t kPthreadMutexRecursive = 2;
// sizeof(pthread_mutexattr_t) on Darwin — ✔MEASURED, same host.
constexpr std::uint32_t kPthreadMutexAttrBytes = 16;

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// A caller referencing `mtx_init` at its REAL descriptor arity — `fn(ptr<void>, i32)
// -> i32` on every format — through a GlobalAddr against a not-yet-defined callee,
// which is the shape the CST->HIR seam leaves for a `synthesize`-tagged row.
Mir buildMtxInitCaller(TypeInterner& in, CallConv cc) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const pV  = in.pointer(in.primitive(TypeKind::Void));
    std::array<TypeId, 2> const params{pV, i32};
    TypeId const initSig = in.fnSig(params, i32, cc);

    MirBuilder mb;
    mb.addFunction(in.fnSig({}, i32, cc), SymbolId{kMainSym});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pV, 64);   // a macho mtx_t
    MirInstId const ga   = mb.addGlobalAddr(SymbolId{kMtxInitSym}, in.pointer(initSig));
    MirInstId const kind = mb.addConst(i32Lit(kC11MtxRecursive), i32);
    MirInstId const co[] = {ga, slot, kind};
    mb.addInst(MirOpcode::Call, co, i32);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

std::optional<MirFuncId> findFunc(Mir const& mir, std::uint32_t symV) {
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == symV) return f;
    }
    return std::nullopt;
}

// Every instruction of a function, in block-then-index order.
std::vector<MirInstId> bodyOf(Mir const& mir, MirFuncId f) {
    std::vector<MirInstId> out;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
        MirBlockId const b = mir.funcBlockAt(f, bi);
        for (std::uint32_t j = 0; j < mir.blockInstCount(b); ++j)
            out.push_back(mir.blockInstAt(b, j));
        out.push_back(mir.blockTerminator(b));
    }
    return out;
}

std::optional<std::string>
importNameOf(std::vector<ExternImport> const& externs, std::uint32_t symV) {
    for (auto const& e : externs)
        if (e.symbol.v == symV) return e.mangledName;
    return std::nullopt;
}

// The mangled import name a Call's callee resolves to, or nullopt for an indirect
// call / a call to a defined function.
std::optional<std::string>
calleeName(Mir const& mir, std::vector<ExternImport> const& externs, MirInstId call) {
    auto ops = mir.instOperands(call);
    if (ops.empty()) return std::nullopt;
    if (mir.instOpcode(ops[0]) != MirOpcode::GlobalAddr) return std::nullopt;
    return importNameOf(externs, mir.globalAddrSymbol(ops[0]).v);
}

// The single Call in `f` whose callee import is `name`.
std::optional<MirInstId> findCall(Mir const& mir, std::vector<ExternImport> const& externs,
                                  MirFuncId f, std::string const& name) {
    for (MirInstId id : bodyOf(mir, f)) {
        if (mir.instOpcode(id) != MirOpcode::Call) continue;
        if (calleeName(mir, externs, id) == name) return id;
    }
    return std::nullopt;
}

std::optional<std::int64_t> constValue(Mir const& mir, MirInstId id) {
    if (mir.instOpcode(id) != MirOpcode::Const) return std::nullopt;
    MirLiteralValue const& lit = mir.literalValue(mir.constLiteralIndex(id));
    if (auto const* i = std::get_if<std::int64_t>(&lit.value)) return *i;
    if (auto const* u = std::get_if<std::uint64_t>(&lit.value))
        return static_cast<std::int64_t>(*u);
    return std::nullopt;
}

LibrarySynthesis pthreadVehicle() {
    return LibrarySynthesis{LibrarySynthVehicle::Pthread, RuntimeLibraryRole::CLibrary,
                            "/usr/lib/libSystem.B.dylib"};
}
LibrarySynthesis win32Vehicle() {
    return LibrarySynthesis{LibrarySynthVehicle::Win32, RuntimeLibraryRole::SystemPrimitives,
                            "kernel32.dll"};
}

} // namespace

// ── THE FIX: the macho recipe builds an attr whose kind is DERIVED from `type` ──
TEST(SynthThreadsMtxType, PthreadMtxInitHonoursTheTypeArgument) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildMtxInitCaller(in, CallConv::CcAAPCS64);

    std::unordered_map<std::uint32_t, std::string> recipes{{kMtxInitSym, "mtx_init"}};
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, pthreadVehicle(),
                                      CSymbolDecorationScheme::LeadingUnderscore,
                                      externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    auto const fn = findFunc(mir, kMtxInitSym);
    ASSERT_TRUE(fn.has_value()) << "mtx_init must be a synthesized pthread-shim definition";

    // (1) All FOUR libSystem primitives are imported, macho-C-mangled, from libSystem.
    for (char const* n : {"_pthread_mutexattr_init", "_pthread_mutexattr_settype",
                          "_pthread_mutexattr_destroy", "_pthread_mutex_init"}) {
        bool found = false;
        for (auto const& imp : externs)
            if (imp.mangledName == n) {
                found = true;
                EXPECT_EQ(imp.libraryPath, "/usr/lib/libSystem.B.dylib") << n;
                EXPECT_FALSE(imp.isData) << n;
            }
        EXPECT_TRUE(found) << "the pthread mtx_init body must import " << n;
    }
    // `mtx_init` itself is NEVER an import (the eager-import law — libSystem has no
    // C11 thrd_*/mtx_*; one absent name breaks the LOAD of every binary).
    for (auto const& imp : externs)
        EXPECT_TRUE(imp.mangledName != "mtx_init" && imp.mangledName != "_mtx_init")
            << "mtx_init is a synthesized definition, never an import";

    auto const settype = findCall(mir, externs, *fn, "_pthread_mutexattr_settype");
    auto const initMtx = findCall(mir, externs, *fn, "_pthread_mutex_init");
    auto const initAtt = findCall(mir, externs, *fn, "_pthread_mutexattr_init");
    auto const destroy = findCall(mir, externs, *fn, "_pthread_mutexattr_destroy");
    ASSERT_TRUE(settype.has_value());
    ASSERT_TRUE(initMtx.has_value());
    ASSERT_TRUE(initAtt.has_value());
    ASSERT_TRUE(destroy.has_value());

    // (2) THE ATTR IS A REAL STACK SLOT, NOT THE OLD NULL CONSTANT, and the same slot
    // reaches all four calls. This is the assertion the pre-fix body fails.
    auto stOps = mir.instOperands(*settype);
    auto imOps = mir.instOperands(*initMtx);
    auto iaOps = mir.instOperands(*initAtt);
    auto dsOps = mir.instOperands(*destroy);
    ASSERT_EQ(stOps.size(), 3u) << "settype(attr, kind)";
    ASSERT_EQ(imOps.size(), 3u) << "pthread_mutex_init(m, attr)";
    ASSERT_EQ(iaOps.size(), 2u) << "mutexattr_init(attr)";
    ASSERT_EQ(dsOps.size(), 2u) << "mutexattr_destroy(attr)";
    MirInstId const attr = iaOps[1];
    EXPECT_EQ(mir.instOpcode(attr), MirOpcode::Alloca)
        << "the mutexattr must be a stack slot; the pre-fix body passed a NULL Const";
    EXPECT_EQ(mir.instPayload(attr), kPthreadMutexAttrBytes)
        << "sizeof(pthread_mutexattr_t) on Darwin";
    EXPECT_EQ(stOps[1].v, attr.v) << "settype must configure THAT attr";
    EXPECT_EQ(dsOps[1].v, attr.v) << "destroy must release THAT attr";
    EXPECT_EQ(imOps[2].v, attr.v)
        << "pthread_mutex_init's attr argument must be the configured attr, not NULL";

    // (3) THE KIND IS DERIVED FROM `type`, and the whole chain is pinned by value:
    //     Mul( ZExt( ICmpNe( And(Arg#1, mtx_recursive), 0 ) ), RECURSIVE - NORMAL )
    MirInstId const kind = stOps[2];
    ASSERT_EQ(mir.instOpcode(kind), MirOpcode::Mul) << "the branchless kind select";
    auto mulOps = mir.instOperands(kind);
    ASSERT_EQ(mulOps.size(), 2u);
    auto const multiplier = constValue(mir, mulOps[1]);
    ASSERT_TRUE(multiplier.has_value()) << "the recursive-kind multiplier is a literal";
    EXPECT_EQ(*multiplier, kPthreadMutexRecursive - kPthreadMutexNormal)
        << "PTHREAD_MUTEX_RECURSIVE on Darwin is 2, NOT glibc's 1";

    ASSERT_EQ(mir.instOpcode(mulOps[0]), MirOpcode::ZExt);
    MirInstId const cmp = mir.instOperands(mulOps[0])[0];
    ASSERT_EQ(mir.instOpcode(cmp), MirOpcode::ICmpNe) << "wants-recursive is a != 0 test";
    auto cmpOps = mir.instOperands(cmp);
    ASSERT_EQ(cmpOps.size(), 2u);
    EXPECT_EQ(constValue(mir, cmpOps[1]), std::optional<std::int64_t>{0});

    MirInstId const masked = cmpOps[0];
    ASSERT_EQ(mir.instOpcode(masked), MirOpcode::And)
        << "the type must be MASKED — mtx_timed|mtx_recursive (3) is also recursive, "
           "so an equality against 1 would miss it";
    auto andOps = mir.instOperands(masked);
    ASSERT_EQ(andOps.size(), 2u);
    EXPECT_EQ(constValue(mir, andOps[1]), std::optional<std::int64_t>{kC11MtxRecursive});

    // ★ and the masked value IS the incoming parameter, not a constant: the whole
    //   defect was a `type` that reached the body and was never read.
    ASSERT_EQ(mir.instOpcode(andOps[0]), MirOpcode::Arg)
        << "the kind must be computed from mtx_init's OWN type parameter";
    EXPECT_EQ(arg_payload::ordinal(mir.instPayload(andOps[0])), 1u)
        << "`type` is mtx_init's second parameter";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the attr-select mtx_init module must verify";
}

// The row's stated obstacle, pinned as refuted: honouring `type` needed NEITHER a
// multi-block recipe NOR the abandonment of any invariant.
TEST(SynthThreadsMtxType, PthreadMtxInitStaysASingleBlockRecipe) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildMtxInitCaller(in, CallConv::CcAAPCS64);
    std::unordered_map<std::uint32_t, std::string> recipes{{kMtxInitSym, "mtx_init"}};
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, pthreadVehicle(),
                                      CSymbolDecorationScheme::LeadingUnderscore,
                                      externs, rep));
    auto const fn = findFunc(mir, kMtxInitSym);
    ASSERT_TRUE(fn.has_value());
    EXPECT_EQ(mir.funcBlockCount(*fn), 1u)
        << "the attr dance is straight-line — a CFG here would need its own argument";
}

// ── THE CONTROL: the win32 arm must stay exactly as it is ──────────────────
// pe honours the only direction C11 constrains (a CRITICAL_SECTION is always
// recursion-capable). The plain direction is UNDEFINED and the three references
// disagree three ways, so growing a recursion guard here would INVENT a fourth
// behaviour. This test is what makes that a decision rather than an omission.
TEST(SynthThreadsMtxType, Win32MtxInitTouchesNoMutexAttrAndReadsNoTypeArgument) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildMtxInitCaller(in, CallConv::CcMS64);
    std::unordered_map<std::uint32_t, std::string> recipes{{kMtxInitSym, "mtx_init"}};
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, win32Vehicle(),
                                      CSymbolDecorationScheme::None, externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    auto const fn = findFunc(mir, kMtxInitSym);
    ASSERT_TRUE(fn.has_value());

    for (auto const& imp : externs) {
        EXPECT_EQ(imp.mangledName.find("mutexattr"), std::string::npos)
            << "the win32 vehicle must never emit a pthread primitive: " << imp.mangledName;
        EXPECT_EQ(imp.mangledName.find("pthread"), std::string::npos)
            << "the win32 vehicle must never emit a pthread primitive: " << imp.mangledName;
    }
    EXPECT_TRUE(findCall(mir, externs, *fn, "InitializeCriticalSection").has_value())
        << "the pe mtx_init body is one InitializeCriticalSection";

    // No Arg beyond ordinal 0 is materialized: the `type` is deliberately unread,
    // and this is the assertion that makes a silently-added recursion guard red.
    for (MirInstId id : bodyOf(mir, *fn))
        if (mir.instOpcode(id) == MirOpcode::Arg)
            EXPECT_EQ(arg_payload::ordinal(mir.instPayload(id)), 0u)
                << "the pe arm reads only the mtx_t pointer";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the win32 mtx_init module must verify";
}
