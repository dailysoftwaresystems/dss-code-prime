/* ═══ DSS PLATFORM RUNTIME — the GENERIC C11 ATOMICS ENTRY POINTS ═══════════
 *
 * D-C-ATOMICS-RUNTIME-IS-OURS-ON-PE64. The two runtime-sized entry points an
 * UNDER-ALIGNED `_Atomic` scalar access lowers to a CALL of:
 *
 *     void __atomic_load (size_t n, const void *mem, void *ret, int model);
 *     void __atomic_store(size_t n,       void *mem, const void *val, int model);
 *
 * ★★★ WHY A BODY AND NOT AN IMAGE, AND WHY ONLY HERE. P54 first closed the
 * under-aligned route on pe64 by pointing the format's `atomicsRuntime` role at
 * mingw-w64's `libatomic-1.dll` — the first NON-OS image any pe64 role table had
 * ever named. Every other image those tables name ships with the operating
 * system; that one ships with a third-party toolchain, so a DSS-built Windows
 * program touching a packed `_Atomic` would fail to LOAD on a machine with no
 * mingw on PATH. The operator ruled that dependency a workaround and this file
 * is the replacement: DSS ships the body, exactly as it already ships
 * `dirent.c` and `unistd.c`, and for the same reason — a role the platform does
 * not fill is filled HERE rather than by a runtime we do not control.
 * ⚠ THE elf64 AND macho64 LEGS DELIBERATELY DO NOT MOVE, and the reason is
 * CORRECTNESS rather than inertia: `libatomic.so.1` and `libSystem.B.dylib` are
 * the platform's OWN atomics runtimes, so a gcc-built and a DSS-built
 * translation unit touching the SAME under-aligned object go through the SAME
 * arbiter. Replacing them with a private implementation would be strictly worse
 * wherever the two implementations disagree about how to serialize.
 *
 * ★★★ THIS IS AN INTERFACE PORT, NOT A SOURCE PORT. Every line below is written
 * against gcc's DOCUMENTED generic ABI, confirmed by measuring what a reference
 * compiler EMITS (see PROVENANCE); no libatomic, glibc or compiler-rt source was
 * consulted or copied. gcc's libatomic is GPLv3-with-the-Runtime-Library-
 * Exception: that exception permits LINKING the runtime into programs, it does
 * not make its source copyable into this repository.
 *
 * PROVENANCE — the ABI, ✔MEASURED 2026-09-02 rather than read off a header.
 * `clang 18.1.3 -O1 -S`, one `_Atomic int` at byte offset 1 of a packed struct,
 * `x86_64-pc-linux-gnu`; the register set-up at the call site IS the signature:
 *     load :  edi=4 (size)  rsi=&g.a (mem)  rdx=&tmp (ret)  ecx=5 (model)
 *     store:  edi=4 (size)  rsi=&g.a (mem)  rdx=&tmp (val)  ecx=5 (model)
 * Size FIRST, the atomic object SECOND, the caller's buffer THIRD, the memory
 * model LAST — for BOTH entries. Getting that order wrong produces a program
 * that links, runs, and computes garbage, which is why it is measured.
 *
 * ★★ THE MEMORY MODEL ARGUMENT IS ACCEPTED AND DELIBERATELY IGNORED. Every path
 * below realizes SEQ_CST. Over-fencing is C11-legal (a stronger order always
 * satisfies a weaker request) and on x86-64 it is very nearly free: a
 * `lock`-prefixed read-modify-write is already a full barrier, and TSO makes a
 * plain aligned load a seq_cst load. Honouring `model` would buy nothing
 * measurable and would add four untested paths.
 *
 * ═══ THE DESIGN, PER ARCHITECTURE ══════════════════════════════════════════
 *
 * ★★★ x86-64 — NO LOCK TABLE, AND THE REASON IS AN ARCHITECTURAL GUARANTEE.
 * DOCUMENTED (Intel SDM Vol 3A §8.1.2.1 "Automatic Locking" and §8.1.2.2 "Bus
 * Locking"; AMD APM Vol 2 §7.3): a `lock`-prefixed read-modify-write is atomic
 * REGARDLESS OF ALIGNMENT, including when the operand splits a cache line, and
 * `XCHG` with a memory operand asserts the lock protocol automatically. So the
 * under-aligned access can be served by a locked RMW AT THE OBJECT'S OWN WIDTH,
 * which is atomic against EVERY accessor — inline code from any compiler, a
 * different atomics runtime, another process sharing the mapping — and not
 * merely against callers of our own lock table. A lock table is atomic only
 * against its own users, which is precisely why it is refused here.
 *
 * ⚠⚠ AND WHY A WIDER ALIGNED CAS IS NOT THE STORE PATH. The obvious clever
 * alternative — read-modify-write the containing aligned 8-byte block with a
 * CAS retry loop — is atomic for our field, and it is what the reference does
 * (✔MEASURED by disassembly, below). It is not wrong, but it TOUCHES THE
 * ADJACENT BYTES, and C11 §6.5.1p2 makes adjacent struct members distinct
 * memory locations that may be updated concurrently without a data race. A
 * width-native locked RMW touches only the object's own bytes and needs no
 * retry loop at all, so it is used wherever one exists (n = 4 and n = 8, the
 * only widths DSS can lower an `_Atomic` at — see THE WIDTH BOUND below). The
 * block CAS survives ONLY as the residue path for widths DSS cannot produce.
 *
 * ★★★ WHAT THE REFERENCE ACTUALLY DOES, AND IT IS NOT WHAT THE BRIEF SAID.
 * ✔MEASURED 2026-09-02 by disassembling `libatomic.so.1.2.0` (gcc 13,
 * `objdump -d --disassemble=__atomic_load` / `=__atomic_store`, x86_64): the
 * generic entries do NOT dispatch to a lock merely because the pointer is
 * misaligned. They case-analyse on `size + (addr & (B-1)) <= B` for B = 4, 8,
 * then 16:
 *   - `__atomic_load`, 4 bytes at (addr&7)==1 → `and $-8,%r12; mov (%r12),%rax`
 *     — a PLAIN ALIGNED 8-BYTE LOAD, then an extract. No lock, no lock prefix.
 *   - `__atomic_store`, same operand → `lock orq $0,(%rsp)`, `and $-8,%rbx`,
 *     then a `lock cmpxchg %rdx,(%rbx)` RETRY LOOP over the containing aligned
 *     8-byte block (the 16-byte arm uses `movdqa` + `__atomic_compare_exchange_16`
 *     behind a CPU-feature test).
 *   - the address-hashed LOCK is reached only when the object straddles the
 *     widest usable block, or when the size exceeds 16.
 * ⇒ The claim that gcc's generic entry locks for the under-aligned case is
 * REFUTED. DSS is still better than it here, but for a DIFFERENT reason than
 * expected, and the honest statement is the narrow one: DSS is lock-free in
 * EVERY case the reference is, PLUS the straddling cases where the reference
 * takes its lock, AND its store touches no adjacent byte where the reference's
 * does.
 *
 * ⓘ THE HONEST COST, STATED RATHER THAN BURIED. A `lock`-prefixed RMW whose
 * operand splits a cache line is SLOW (it escalates from a cache lock to a bus
 * lock), and a system configured `split_lock_detect=fatal` turns it into a
 * SIGBUS-equivalent fault. gcc's own inline `xchgl` on a packed member carries
 * exactly the same exposure, so this is not a regression against the reference
 * — but it is real and it belongs in daylight. The LOAD path avoids it entirely
 * whenever the object fits inside an aligned 8-byte block, which is the case
 * for every packed member whose containing object is 8-byte aligned.
 *
 * ★★ aarch64 IS NOT SERVED BY THIS FILE, AND THAT IS A DECISION. Every aarch64
 * atomic primitive (`LDXR`/`STXR`, `LDAR`/`STLR`, and the LSE `CAS`/`SWP`)
 * REQUIRES natural alignment and faults otherwise — ✔MEASURED in P53 on native
 * aarch64 hardware: rc 135, Bus error. There is no bus-lock equivalent, so the
 * only correct route there is an address-hashed lock table, and a lock table
 * arbitrates only between its own users. On aarch64 the platform already ships
 * one (`libatomic.so.1` on Linux, libSystem on Darwin) and a gcc- or
 * clang-built object in the same program uses it; a second, private table would
 * NOT interoperate with the first. So the aarch64 formats keep the platform
 * runtime, and this file exists for the one format family whose platform ships
 * no runtime at all.
 *
 * ═══ THE WIDTH BOUND, AND IT IS A MEASUREMENT ══════════════════════════════
 *
 * ✔MEASURED 2026-09-02 at this tree, `x86_64:pe64-x86_64-windows-exec`: DSS
 * lowers `_Atomic` at widths 32 and 64 ONLY. An `_Atomic unsigned short` or
 * `_Atomic unsigned char` reaches the assembler and dies
 * `A_NoMatchingEncodingVariant: opcode 'load_acquire' … at width 16` (and 8) —
 * `x86_64.target.json` declares `load_acquire` / `store_seqcst` / `lock_cmpxchg`
 * at widths 32 and 64 and at no other width. So the ONLY sizes DSS itself can
 * route through these entries are n = 4 and n = 8, and those two take the
 * width-native locked path with no caveat whatsoever. The other sizes are
 * implemented anyway, because the entry points carry names the whole world
 * knows and a foreign object linked into the same program may call them — but
 * they are the residue, they take the block-CAS path, and the ONE combination
 * no x86-64 primitive can serve atomically (a 2-byte object straddling an
 * 8-byte boundary, and any size above 8) FAILS LOUD rather than quietly
 * performing a non-atomic access.
 *
 * ⓘ THAT 16-BIT / 8-BIT GAP IS A REAL DEFECT, IT IS NOT THIS FILE'S, AND IT IS
 * ALREADY FILED — [[D-CSUBSET-ATOMIC-MONOMORPH-I32]] (P1, ⏳ GATED) records the
 * identical measurement in its own words: *"a sub-word `_Atomic char`/`short`
 * fenced access FAILS LOUD at the encoder — `A_NoMatchingEncodingVariant` on
 * `store_seqcst`/`load_acquire` at width 8 … add the byte/half
 * `store_seqcst`/`load_acquire`/`store_release` slots with the widths."* ⚠ A NEW
 * ROW FOR IT WAS DRAFTED HERE AND DELETED after grepping all three registries
 * ([[feedback-close-do-not-file]]): the defect was known, and a second row would
 * have split its evidence in two. When that row closes, the residue paths below
 * become reachable and their `dssAtomicsRefuse` arm shrinks to the single
 * uncoverable cell.
 *
 * ═══ WHY THESE ENTRY POINTS AND NOT MORE ═══════════════════════════════════
 *
 * `__atomic_exchange` and `__atomic_compare_exchange` are DELIBERATELY not
 * defined here. DSS's lowering mints calls to the load and store entries and to
 * nothing else, and the shipped runtime archive is pulled MEMBER-BY-NAME from
 * the linker's unresolved-symbol worklist — so an entry nothing references
 * would never be linked, never be executed, and never be tested. A runtime body
 * that has never run is a liability, and the failure mode for a caller this
 * runtime does not serve is an UNRESOLVED SYMBOL AT LINK, naming the symbol, at
 * build time. That is loud, early, and actionable; a body that was never
 * exercised is none of those. When a future lowering mints one of them, it
 * lands here with its own execution witness.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* ── THE PRIMITIVES ────────────────────────────────────────────────────────
 *
 * Each is one DSS construct with a known x86-64 realization, and each is
 * ✔MEASURED by disassembly of a DSS-built pe64 image rather than assumed:
 *
 *   *(volatile _Atomic uint32_t *)p = v   →  `xchg %edx,(%rcx)`
 *   *(volatile _Atomic uint64_t *)p = v   →  `xchg %rdx,(%rcx)`
 *   _InterlockedCompareExchange(p,e,c)    →  `lock cmpxchg %r9d,(%rcx)`
 *
 * ★ XCHG WITH A MEMORY OPERAND NEEDS NO `LOCK` PREFIX — the processor asserts
 * the lock protocol automatically (Intel SDM Vol 2B, XCHG). That is why the
 * store path is a single instruction with no retry loop.
 *
 * ⚠ THE POINTER-MEDIATED SPELLING IS LOAD-BEARING AND MUST NOT BECOME A MEMBER
 * ACCESS. DSS routes an atomic access through THESE VERY ENTRY POINTS when it
 * can PROVE the lvalue is under-aligned; a `*(_Atomic T *)p` deref has no
 * provable alignment, so it keeps the native inline form. Writing this file
 * against a packed struct member instead would make the runtime call itself.
 */

/* One naturally-aligned 8-byte atomic read. An aligned 8-byte load is atomic on
 * x86-64, and an aligned 8-byte block never crosses a page boundary, so reading
 * the block containing a smaller object can never fault where the object
 * itself would not. */
static uint64_t dssAtomicsBlockLoad64(uintptr_t alignedAddr) {
    return *(volatile _Atomic uint64_t *)(void *)alignedAddr;
}

static void dssAtomicsXchg32(uintptr_t addr, uint32_t value) {
    *(volatile _Atomic uint32_t *)(void *)addr = value;
}

static void dssAtomicsXchg64(uintptr_t addr, uint64_t value) {
    *(volatile _Atomic uint64_t *)(void *)addr = value;
}

/* A locked no-op read-modify-write: expected == desired, so the memory is
 * unchanged and the ORIGINAL value comes back. This is an atomic LOAD that
 * holds at any alignment, and it touches only the object's own bytes. */
/* ⚠ THE CAST SPELLINGS ARE THE INTRINSICS' OWN DECLARED PARAMETER TYPES, NOT
 * `int32_t`/`int64_t`. `c.lang.json` declares the 32-bit CAS as
 * `ptr<i32 "long">` under LLP64 and the 64-bit one as `ptr<i64 "long long">`,
 * and a NAMED vocabulary type is a different TypeId from a bare one
 * ([[D-LANG-TYPE-IDENTITY-VOCABULARY]]) — a bare `int32_t volatile *` is
 * refused by identity in a pointer position even though the representations
 * agree. Spelling the parameter's own type is the fix, not a cast around it. */
static uint32_t dssAtomicsLockedLoad32(uintptr_t addr) {
    return (uint32_t)_InterlockedCompareExchange((long volatile *)(void *)addr,
                                                 0, 0);
}

static uint64_t dssAtomicsLockedLoad64(uintptr_t addr) {
    return (uint64_t)_InterlockedCompareExchange64(
        (long long volatile *)(void *)addr, 0, 0);
}

static uint64_t dssAtomicsCas64(uintptr_t addr, uint64_t expected,
                                uint64_t desired) {
    return (uint64_t)_InterlockedCompareExchange64(
        (long long volatile *)(void *)addr, (long long)desired,
        (long long)expected);
}

/* ⚠ THE ONE COMBINATION x86-64 CANNOT SERVE ATOMICALLY. A 2-byte object at
 * (addr & 7) == 7 straddles the 8-byte block and is too small for a
 * width-native locked RMW this compiler can emit; a size above 8 needs a
 * 16-byte CAS DSS does not lower. Neither is reachable from DSS's own lowering
 * (THE WIDTH BOUND, above). Refusing LOUDLY is the project's rule: a silently
 * non-atomic access is the exact defect this whole route exists to remove. */
static void dssAtomicsRefuse(void) { abort(); }

static void dssAtomicsCopy(unsigned char *dst, unsigned char const *src,
                           size_t n) {
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
}

/* ── __atomic_load ─────────────────────────────────────────────────────────
 *
 * ★ THE FAST PATH IS A PLAIN ALIGNED READ, AND IT IS THE PATH THAT MATTERS.
 * Whenever the object lies wholly inside a naturally-aligned 8-byte block, one
 * aligned 8-byte load takes an atomic snapshot of the whole block and the
 * object's bytes are extracted from it. That is lock-free, needs no `lock`
 * prefix, has no split-lock exposure, and — unlike a locked RMW — WRITES
 * NOTHING, so it works on a read-only mapping exactly as the reference's does.
 * Only a straddling object falls through to the locked form. */
void __atomic_load(size_t n, void const *mem, void *ret, int model) {
    (void)model;
    uintptr_t const addr = (uintptr_t)mem;

    if (n != 0 && n <= 8 && ((addr & (uintptr_t)7) + (uintptr_t)n) <= 8) {
        uint64_t const block = dssAtomicsBlockLoad64(addr & ~(uintptr_t)7);
        dssAtomicsCopy((unsigned char *)ret,
                       (unsigned char const *)&block + (addr & (uintptr_t)7), n);
        return;
    }
    if (n == 4) {
        uint32_t const v = dssAtomicsLockedLoad32(addr);
        dssAtomicsCopy((unsigned char *)ret, (unsigned char const *)&v, 4);
        return;
    }
    if (n == 8) {
        uint64_t const v = dssAtomicsLockedLoad64(addr);
        dssAtomicsCopy((unsigned char *)ret, (unsigned char const *)&v, 8);
        return;
    }
    dssAtomicsRefuse();
}

/* ── __atomic_store ────────────────────────────────────────────────────────
 *
 * ★ n = 4 and n = 8 — the only widths DSS can route here — take a SINGLE
 * `xchg`, which carries x86's implicit lock, is atomic at any alignment, and
 * touches exactly the object's own bytes. No retry loop, no adjacent byte
 * written, no CPU-feature test.
 *
 * ⚠ THE RESIDUE PATH IS THE WIDER BLOCK CAS, AND IT IS NEIGHBOUR-SAFE BY
 * CONSTRUCTION. It reads the containing aligned block, splices our bytes in,
 * and compare-exchanges the whole block back; if any adjacent byte changed in
 * between, the compare FAILS and the loop reloads, so a concurrent write to a
 * neighbour can never be lost. It is used only for the sizes DSS cannot emit
 * (see THE WIDTH BOUND), never for n = 4 or n = 8, where a width-native locked
 * RMW is available and strictly cleaner. */
void __atomic_store(size_t n, void *mem, void const *val, int model) {
    (void)model;
    uintptr_t const addr = (uintptr_t)mem;

    if (n == 4) {
        uint32_t v = 0;
        dssAtomicsCopy((unsigned char *)&v, (unsigned char const *)val, 4);
        dssAtomicsXchg32(addr, v);
        return;
    }
    if (n == 8) {
        uint64_t v = 0;
        dssAtomicsCopy((unsigned char *)&v, (unsigned char const *)val, 8);
        dssAtomicsXchg64(addr, v);
        return;
    }
    if (n != 0 && n < 8 && ((addr & (uintptr_t)7) + (uintptr_t)n) <= 8) {
        uintptr_t const base = addr & ~(uintptr_t)7;
        size_t const    off  = (size_t)(addr & (uintptr_t)7);
        uint64_t        seen = dssAtomicsBlockLoad64(base);
        for (;;) {
            uint64_t next = seen;
            dssAtomicsCopy((unsigned char *)&next + off,
                           (unsigned char const *)val, n);
            uint64_t const prior = dssAtomicsCas64(base, seen, next);
            if (prior == seen) return;
            seen = prior;
        }
    }
    // ⚠ THERE IS NO 4-BYTE-BLOCK ARM, AND ITS ABSENCE IS PROVED RATHER THAN
    // ASSUMED. A first cut carried one below this, as a narrower fallback for a
    // small object that did not fit an aligned 8-byte block — and it was DEAD
    // CODE reading as a safety net. An aligned 4-byte block is CONTAINED IN an
    // aligned 8-byte block, so the 8-byte test above is strictly weaker: every
    // object the 4-byte arm could serve was already served. Worked through on
    // the only candidates ((addr AND 7) in 5,6,7 with n < 4): each one fails the
    // 4-byte test too. It is deleted rather than kept "for safety", because an
    // unreachable arm in a runtime is an arm that is never exercised and can
    // rot silently into a wrong answer.
    dssAtomicsRefuse();
}
