/* ═══ DSS PLATFORM RUNTIME — the GENERIC C11 ATOMICS ENTRY POINTS ═══════════
 *
 * D-C-ATOMICS-RUNTIME-IS-OURS-ON-PE64 gave this file its reason to exist;
 * D-C-ATOMICS-RUNTIME-PE64-BUS-LOCKS-EVERY-ACCESS-TO-A-CACHE-LINE-STRADDLING-OBJECT
 * gave it the design it has now. The runtime-sized entry points an
 * UNDER-ALIGNED `_Atomic` scalar access lowers to a CALL of:
 *
 *     void  __atomic_load (size_t n, const void *mem, void *ret, int model);
 *     void  __atomic_store(size_t n,       void *mem, const void *val, int model);
 *     _Bool __atomic_compare_exchange(size_t n, void *mem, void *expected,
 *                                     void *desired, int success, int failure);
 *
 * The third serves the compare-exchange retry loop that compound assignment and
 * `++`/`--` on an under-aligned object lower to:
 * D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE
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
 * not make its source copyable into this repository. The reference runtimes'
 * BEHAVIOUR cited below was measured from their shipped BINARIES, by
 * disassembly and by execution.
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
 * PROVENANCE — the compare-exchange ABI, ✔MEASURED 2026-09-15 the same way,
 * `clang 18.1.3 -O1` on `g.a += 5` and on `g.a++` over the same packed member:
 *     cx   :  edi=4 (size)  rsi=&g.a (mem)  rdx=&slot (expected)
 *             rcx=&slot (desired)  r8d=5 (success)  r9d=5 (failure)
 * then `test %al,%al` on the returned `_Bool`; on false the caller RELOADS its
 * expected slot and retries. So the callee must write the value it OBSERVED
 * into `*expected` — a compare-exchange that only returned false would spin
 * that loop forever on a stale expectation.
 *
 * ★★ THE MEMORY MODEL ARGUMENT IS ACCEPTED AND DELIBERATELY IGNORED. Every path
 * below realizes SEQ_CST. Over-fencing is C11-legal (a stronger order always
 * satisfies a weaker request) and on x86-64 it costs nothing here: a locked
 * `xchg` is already a full barrier, TSO makes a plain load a seq_cst load, and a
 * copy made under a lock is linearized at that lock. Honouring `model` would buy
 * nothing measurable and would add four untested paths.
 *
 * ═══ THE DESIGN ON x86-64 — ONE SPLIT, AND IT IS THE CACHE LINE ══════════════
 *
 * For the two widths DSS can route here (n = 4 and n = 8, THE WIDTH BOUND):
 *
 *   an operand INSIDE one 64-byte cache line is served by the PROCESSOR —
 *     store   one `xchg` at the object's own width: implicit LOCK, exactly the
 *             object's bytes, no retry loop.
 *     load    one plain read — of the aligned 8-byte block when the object fits
 *             one, otherwise of the object itself at its own width. A load
 *             WRITES NOTHING, so it works on read-only memory.
 *     cx      one `lock cmpxchg` at the object's own width; on a mismatch the
 *             value the processor compared is written to `*expected`.
 *   an operand that CROSSES a cache line is served by THE PROCESS LOCK —
 *     all     the lock of the process's default heap, `HeapLock(GetProcessHeap())`,
 *             held across a byte copy — and for cx a byte compare — of exactly
 *             the object's bytes.
 *
 * No locked instruction in this file is ever handed an operand that crosses a
 * line, so no access through it takes a bus lock.
 *
 * ★★★ WHY THE LINE — AND WHY THIS IS A REVERSAL. A LOCKED OPERAND THAT CROSSES
 * A CACHE LINE IS A SPLIT LOCK, AND A SPLIT LOCK TAKES A BUS LOCK THAT STALLS
 * THE WHOLE MACHINE.
 * 📄 docs.kernel.org, `Documentation/arch/x86/buslock.rst`: a split lock is any
 *   atomic operation whose operand crosses two cache lines; because the
 *   operation must stay atomic, the system locks the bus while the CPU accesses
 *   both lines. A bus lock is typically thousands of cycles slower than an
 *   atomic operation within a cache line and disrupts performance on other
 *   cores. Linux can detect one (#AC split-lock detection, #DB bus-lock
 *   detection) and then warn, rate-limit (`split_lock_detect=ratelimit:N`) or
 *   send SIGBUS (`fatal`); hypervisors get a bus-lock VM exit for the same
 *   purpose.
 * ✔MEASURED 2026-09-15 (P66 lane `bl`) on an AMD Ryzen 9 9950X under Windows 11
 *   — a part with NO split-lock or bus-lock detection (the CPUID bits read 0
 *   natively and inside WSL2), so nothing below is an operating system's
 *   throttle; it is the hardware's own cost:
 *   - one `xchg` on a 4-byte operand, 3.54 ns inside a line against 779.5 ns
 *     across one; one `lock cmpxchg`, 3.49 against 748.7 ns;
 *   - four threads each running `xchg` on their OWN straddling object stretched
 *     a single-threaded, lock-free benchmark on another core from 633 ms to
 *     4339 ms; the same threads inside a line cost it nothing;
 *   - THE PREVIOUS DESIGN OF THIS FILE served every straddling access with a
 *     locked instruction — an `xchg` store and a `lock cmpxchg` no-op load — and
 *     `examples/c/packed_atomic_member_concurrency`, which straddles on purpose,
 *     took a median 96.5 ms against 11.8 ms for the same program with its object
 *     inside a line, while stretching that neighbour 8.6× (657 → 5665 ms). On
 *     PR #57's `windows-msvc-release` runner it exceeded the 5000 ms child limit
 *     three times in one job, and the tests finishing beside it ran a median
 *     3.97× and 3.54× slower.
 *
 * ⚠⚠ THE PARAGRAPH THAT STOOD HERE CALLED THAT COST "NOT A REGRESSION AGAINST
 * THE REFERENCE", AND BOTH HALVES OF ITS ARGUMENT WERE WRONG. It compared
 * against gcc's INLINE `xchgl`, but gcc's inline form is not a WORKING reference
 * for this construct: its paired load is a plain `movl`, which tears across a
 * line (✔MEASURED, gcc 13.3.0 -O2 on the witness: rc 11, a torn value, in all
 * 1860 runs — while stretching the neighbour 13.7×). The working reference is
 * clang with libatomic, and it takes NO bus lock: ✔MEASURED by disassembling
 * `libatomic.so.1.2.0`, both generic entries serve an object that straddles
 * their widest aligned block with an address-hashed `pthread_mutex_lock` around
 * a `memcpy`; the witness built by clang 18.1.3 runs a median 7.6 ms (5.9 ms
 * with its object inside a line) and leaves the neighbour at 757 ms against
 * 754 ms alone. mingw-w64's `libatomic-1.dll` has the same shape, importing
 * `pthread_mutex_lock` from `libwinpthread-1.dll`.
 *
 * ★★★ WHY THE PROCESS HEAP'S LOCK, AND NOT A LOCK TABLE OF OUR OWN. This body
 * ships as a STATIC archive member, so every DSS-built image links its own copy
 * — each `.exe` and each `.dll`. A lock table defined here would exist once PER
 * IMAGE, and a DSS-built exe and a DSS-built dll racing one straddling object
 * would take two different locks and tear each other. The reference runtimes
 * escape that by being ONE shared library per process; the equivalent here is
 * an arbiter every image in the process reaches by construction, with no name
 * another process could squat on and no undocumented export.
 * 📄 learn.microsoft.com: `GetProcessHeap` "retrieves a handle to the default
 * heap of the calling process", and `HeapLock` "attempts to acquire the
 * critical section object, or lock, that is associated with a specified heap",
 * each successful call to be matched by `HeapUnlock`. Both are kernel32 exports
 * (✔MEASURED in its export table), declared for DSS in `shippedLibs/windows.json`
 * exactly as `dirent.c` and `unistd.c` get their primitives. ✔MEASURED that the
 * handle really is ONE across images: single-stepping a DSS-built exe and a
 * DSS-built dll, both copies of this body entered `RtlLockHeap` with the same
 * heap handle (`tests/program/test_atomics_runtime_no_split_lock`). ONE lock for
 * every straddling object is also what keeps two VIEWS of one mapping atomic
 * inside a process, where an address-hashed table would hand the two addresses
 * two different locks.
 *
 * ⓘ THE COST OF THE PROCESS LOCK, STATED RATHER THAN BURIED.
 *   - ✔MEASURED at this change, same host, one thread, median of 7 × 10^6
 *     accesses, three runs per cell: a 4-byte store to an object crossing a
 *     line fell from 815–845 ns to 17.9–20.2 ns (baseline) and from 844–864 ns
 *     to 15.0–15.2 ns (`release`); its load from 834–850 ns to 18.0–19.3 ns and
 *     from 842–859 ns to 15.5–15.7 ns. An in-line LOAD got cheaper as well,
 *     13.8–14.9 → 8.5 ns (9.1–10.0 → 6.6–7.0 ns), because it lost a locked
 *     read-modify-write it never needed; an in-line store is unchanged within
 *     noise. The concurrency witness now takes a median 11.8 ms (96.5 ms
 *     before), and the neighbour beside it 680 ms against 655 ms alone.
 *   - Every straddling access in the process, whatever its object, serializes
 *     on this one lock, and shares it with the default heap's own serialized
 *     operations. What is held is a copy of 4 or 8 bytes.
 *   - A thread that holds `HeapLock(GetProcessHeap())` itself — around a
 *     `HeapWalk`, say — delays every straddling access on other threads until it
 *     unlocks, and a thread terminated while holding it blocks them for good.
 *     Both hazards already exist for every allocation from that heap.
 *   - 📄 `HeapLock` on a heap created with `HEAP_NO_SERIALIZE` is undefined, so
 *     a foreign exe whose load configuration made the DEFAULT heap unserialized
 *     would make this lock undefined too. 🧠 No multithreaded program can use
 *     such a heap at all, and a straddling access with a single thread has
 *     nothing to exclude; it is recorded rather than defended against.
 *   - The split assumes an x86-64 cache line of 64 bytes (✔MEASURED: CPUID's
 *     CLFLUSH line size reads 64 on this part). A line that is a MULTIPLE of 64
 *     only makes the split conservative; a narrower one would make the plain
 *     in-line read non-atomic. 🧠 No x86-64 part is known to have one.
 *
 * ★★ WHAT THE REVERSAL KEEPS AND WHAT IT GIVES UP, AXIS BY AXIS. Only an object
 * that crosses a line changed arms; an in-line object kept its store and lost
 * only a load that wrote.
 *   1. DSS-built code in the same image — atomic before (the bus lock), atomic
 *      after (the process lock). KEPT, for load, store and compare-exchange.
 *      ⚠ NOT for compound assignment or `++`/`--`, which in DSS were never ONE
 *      atomic operation on ANY `_Atomic` object, aligned or not — a load and a
 *      separate store, losing updates on every leg (✔MEASURED by that row's
 *      race probe) — so no axis here covers them until their lowering routes
 *      through this compare-exchange:
 *      D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE
 *   2. Other DSS-built images in the same process — atomic before, atomic after,
 *      because every image's copy locks the one default heap. KEPT; raced by
 *      `examples/c/packed_atomic_member_two_image_race` and pinned without any
 *      timing by `tests/program/test_atomics_runtime_no_split_lock`. ✔MEASURED
 *      that the choice matters: with the process lock replaced by a lock
 *      private to each image, that race TORE in both examples runners while the
 *      single-image concurrency witness stayed green.
 *   3. A foreign object calling these entries BY NAME — linked into a DSS-built
 *      image it binds to this body and shares its lock: KEPT. Living in a
 *      foreign image bound to mingw-w64's `libatomic-1.dll`, it was not atomic
 *      with this body before (that runtime serves a straddling object with its
 *      own mutex and a plain `memcpy`, and a plain access beside a bus-locked
 *      `xchg` tears on the measured part) and it is not after. UNCHANGED.
 *   4. gcc-INLINED accessors — ✔MEASURED by disassembly, gcc 13.3.0 -O2 emits an
 *      `xchgl` store and a PLAIN `movl` load, and that load tears against its
 *      own store (rc 11 above), so no working program relies on gcc-inlined
 *      code being atomic against a runtime. A gcc-inlined STORE was mutually
 *      atomic with the old locked arms and is not with the process lock;
 *      libatomic's mutex does not exclude it either. GIVEN UP, matching the
 *      working reference.
 *   5. ANOTHER PROCESS mapping the same memory — DSS-built programs were
 *      mutually atomic across processes before, on WRITABLE shared memory,
 *      because both sides took the bus lock (✔MEASURED 0 torn in 10 of 10
 *      two-process runs), and are not after: each process locks its OWN default
 *      heap, and the same two-process race tore in 10 of 10 runs (269–1 535
 *      torn values each). No user-mode lock sees two address spaces, no
 *      reference runtime provides it (libatomic's table is per process; MSVC's
 *      helpers load with a plain `mov`, ✔MEASURED by the P66 orchestrator), and
 *      it lies outside ISO C's model, where atomic types serve data shared
 *      between THREADS (C11 §7.17.1). GIVEN UP — the one property the old header
 *      promised that this design withdraws, kept withdrawn after the alternative
 *      was measured (WHY NOT ALSO ATOMIC ACROSS PROCESSES, below).
 *   6. An object in READ-ONLY memory that does not fit an aligned 8-byte block —
 *      inside a line OR across one — CRASHED before and WORKS after, because
 *      the old load for both was a `lock cmpxchg` no-op, and a no-op
 *      compare-exchange still WRITES. ✔MEASURED on
 *      `examples/c/packed_atomic_member_readonly`, a `static const` table of 64
 *      packed records in `.rdata`: exit 0xC0000005 on both pe64 arms before this
 *      change, while clang with libatomic and gcc both return 42 on that source;
 *      42 on both arms after it.
 *   7. An image built by an OLDER DSS (the locked arms) beside a newer one, on
 *      one straddling object — not mutually atomic. 🧠 INFERRED from axes 1 and
 *      2; a static runtime of a pre-1.0 compiler carries no cross-version
 *      promise.
 *
 * ★★★ WHY NOT ALSO ATOMIC ACROSS PROCESSES — "OPTION 2", MEASURED AND DECLINED
 * BY THE OPERATOR'S OWN RULE. The operator ruled (P66): make the object atomic
 * between DSS-built processes too — a locked instruction for memory another
 * process can map, the process lock for memory none can — if that can be
 * guaranteed without making access slow or heavy; otherwise keep this design.
 * Every figure below is ✔MEASURED on the part above, with probes built by
 * mingw-w64 gcc 13.2 -O2, over one 4-byte object at byte 62 of a line.
 *   (i) WHAT IT COULD COVER.
 *     - A WRITABLE view of a pagefile-backed or file-backed section: YES. A
 *       locked store in one process and a locked load in another tore 0 times
 *       in 10 of 10 runs of 20 000 stores, 5 of them with the writer's process
 *       also writing the byte beside the object.
 *     - An image section marked SHARED: only with a walk of that image's
 *       section headers (`IMAGE_SCN_MEM_SHARED`). VirtualQuery reports such a
 *       section exactly as it reports a private `.data` page (MEM_IMAGE,
 *       PAGE_READWRITE, before and after a write), so without the walk every
 *       global of every image would be sent back to the bus lock.
 *     - A READ-ONLY view: NO. No LOCK-prefixed instruction reads without
 *       writing, so that load can only be a plain read, and a plain read is not
 *       excluded by a bus-locked store: across two processes, with the writer's
 *       process also writing the byte beside the object, the read-only reader
 *       tore in 5 of 5 runs (234–626 torn values each; 0 in 5 runs without that
 *       write). In one process the same pair tore in 9 of 9 runs (799–2 430).
 *     - A process NOT built by DSS: NO arbiter can. gcc's and MSVC's own loads
 *       are plain reads that tear inside their own process (the concurrency
 *       witness exits 11 — ✔MEASURED for gcc 13.3 here, and for MSVC 19.51 by
 *       the P66 orchestrator, 20 of 20 runs), and libatomic's lock is per
 *       process.
 *     - An address unmapped and remapped: VirtualQuery's answer for ONE address
 *       went from MEM_PRIVATE to MEM_MAPPED after a VirtualFree and a
 *       MapViewOfFileEx there, so no cache of the classification is safe.
 *   (ii) WHAT THE COMMON CASE WOULD PAY. The classification is a system call:
 *     VirtualQuery 268–308 ns per call, QueryWorkingSetEx 311 ns, against
 *     12.3 ns for an uncontended HeapLock/HeapUnlock pair. A process-private
 *     line-crossing store classified first costs 303 ns against this design's
 *     14.4 ns — 21× — and 336 ns for an image object with the section walk.
 *   (iii) WHAT SHARED MEMORY WOULD PAY. 1 176 ns per store against 914 ns for
 *     the replaced design; beside four threads doing it, the neighbour benchmark
 *     took a median 3 798 ms, against 4 878 ms beside the replaced design and
 *     1 100 ms beside this one (1 022 ms idle, on a busier machine than the
 *     figures further up).
 * ⇒ A read-only shared view cannot be made atomic across processes at all, and
 * every private access would pay a system call, so this design stays. It is
 * BORDERLINE by the operator's own example — writable shared memory could be
 * covered — and revisiting it is the operator's decision, not this file's.
 *
 * ★★ WHY A PLAIN READ IS ATOMIC INSIDE A LINE AND NOT ACROSS ONE. 🧠 A store
 * confined to one line reaches every other core as that one line, so a read
 * confined to it sees the whole store or none of it; only an operand spanning
 * TWO lines needs the bus lock to stay atomic (📄 buslock.rst, above). It is
 * the same property every x86-64 compiler relies on when it emits an ALIGNED
 * `_Atomic` load as a plain `mov`, and where inside the line the object sits
 * changes nothing about it. ✔MEASURED with gcc's inline form, whose load is
 * exactly such a read: the witness with its object inside a line (byte 54)
 * returned 42 in all 4949 runs; straddling (byte 62) it returned 11 in all 1860.
 *
 * ★★ aarch64 IS NOT SERVED BY THIS FILE, AND THAT IS A DECISION. Every aarch64
 * atomic primitive (`LDXR`/`STXR`, `LDAR`/`STLR`, and the LSE `CAS`/`SWP`)
 * REQUIRES natural alignment and faults otherwise — ✔MEASURED in P53 on native
 * aarch64 hardware: rc 135, Bus error. So the only correct route there is an
 * address-hashed lock table, and a lock table arbitrates only between its own
 * users. On aarch64 the platform already ships one (`libatomic.so.1` on Linux,
 * libSystem on Darwin) as ONE shared image per process, and a gcc- or
 * clang-built object in the same program uses it; a second, private table would
 * NOT interoperate with the first. So the aarch64 formats keep the platform
 * runtime, and this file exists for the one format family whose platform ships
 * no runtime at all — where the single process-wide arbiter has to be one the
 * operating system already provides.
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
 * line split above. The other sizes are implemented anyway, because the entry
 * points carry names the whole world knows and a foreign object linked into
 * the same program may call them — but they are the residue: a store and a
 * compare-exchange take the aligned-block CAS (an aligned 8-byte operand never
 * crosses a line), a load takes the aligned-block read, and every combination
 * none of them serves (a small object crossing its 8-byte block, and any size
 * above 8) FAILS LOUD rather than quietly performing a non-atomic access.
 * ⓘ The process lock COULD serve every one of those combinations. It does not,
 * because no DSS program reaches them and a path nothing runs would ship
 * untested; the wide case belongs to its own row:
 * [[D-CSUBSET-ATOMIC-NONLOCKFREE]]
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
 * `__atomic_exchange` is DELIBERATELY not defined here. DSS's lowering mints
 * calls to the load, store and compare-exchange entries and to nothing else,
 * and the shipped runtime archive is pulled MEMBER-BY-NAME from the linker's
 * unresolved-symbol worklist — so an entry nothing references would never be
 * linked, never be executed, and never be tested. A runtime body that has never
 * run is a liability, and the failure mode for a caller this runtime does not
 * serve is an UNRESOLVED SYMBOL AT LINK, naming the symbol, at build time. That
 * is loud, early, and actionable; a body that was never exercised is none of
 * those. When a future lowering mints it, it lands here with its own execution
 * witness.
 *
 * ★ `__atomic_compare_exchange` STOOD IN THAT SENTENCE UNTIL P66, AND IT LEFT BY
 * THE RULE THE SENTENCE STATES: it arrived with its consumer — the
 * compare-exchange retry loop compound assignment and `++`/`--` lower to — and
 * with witnesses that call it DIRECTLY, so it runs whether or not that lowering
 * has landed: `examples/c/packed_atomic_member_compare_exchange` (success,
 * failure and the write-back through `expected`, on all four arms and the
 * residue path), `examples/c/packed_atomic_member_compare_exchange_race`
 * (compare-exchange MIXED with store and load on one line-crossing object, with
 * an exact count), and the compare-exchange phases of
 * `tests/program/test_atomics_runtime_no_split_lock`.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

/* The cache-line size the split is drawn at. See THE COST OF THE PROCESS LOCK
 * for why 64 is safe on every x86-64 part and what a different size would do. */
#define DSS_ATOMICS_CACHE_LINE_BYTES 64u

/* ── THE PRIMITIVES ────────────────────────────────────────────────────────
 *
 * Each is one DSS construct with a known x86-64 realization, and each is
 * ✔MEASURED by disassembly of a DSS-built pe64 image rather than assumed:
 *
 *   *(volatile _Atomic uint64_t *)p       →  `mov (%rcx),%rax`   (a plain read)
 *   *(volatile _Atomic uint32_t *)p       →  `mov (%rcx),%eax`   (a plain read)
 *   *(volatile _Atomic uint32_t *)p = v   →  `xchg %edx,(%rcx)`
 *   *(volatile _Atomic uint64_t *)p = v   →  `xchg %rdx,(%rcx)`
 *   _InterlockedCompareExchange(p,e,c)    →  `lock cmpxchg %r8d,(%rcx)`
 *   _InterlockedCompareExchange64(p,e,c)  →  `lock cmpxchg %r8,(%rcx)`
 *
 * ★ XCHG WITH A MEMORY OPERAND NEEDS NO `LOCK` PREFIX — the processor asserts
 * the lock protocol automatically (Intel SDM Vol 2B, XCHG). That is why the
 * in-line store is a single instruction with no retry loop — and why it may
 * only ever be handed an operand inside one cache line.
 *
 * ⚠ THE POINTER-MEDIATED SPELLING IS LOAD-BEARING AND MUST NOT BECOME A MEMBER
 * ACCESS. DSS routes an atomic access through THESE VERY ENTRY POINTS when it
 * can PROVE the lvalue is under-aligned; a `*(_Atomic T *)p` deref has no
 * provable alignment, so it keeps the native inline form. Writing this file
 * against a packed struct member instead would make the runtime call itself.
 */

/* One naturally-aligned 8-byte read. An aligned 8-byte block never crosses a
 * cache line or a page boundary, so reading the block containing a smaller
 * object is atomic and can never fault where the object itself would not. */
static uint64_t dssAtomicsBlockLoad64(uintptr_t alignedAddr) {
    return *(volatile _Atomic uint64_t *)(void *)alignedAddr;
}

/* A plain read at the object's own width. Atomic ONLY because every caller has
 * already established that the operand lies inside one cache line. */
static uint32_t dssAtomicsLineLoad32(uintptr_t addr) {
    return *(volatile _Atomic uint32_t *)(void *)addr;
}

static uint64_t dssAtomicsLineLoad64(uintptr_t addr) {
    return *(volatile _Atomic uint64_t *)(void *)addr;
}

/* A locked store at the object's own width. Handed an in-line operand only. */
static void dssAtomicsXchg32(uintptr_t addr, uint32_t value) {
    *(volatile _Atomic uint32_t *)(void *)addr = value;
}

static void dssAtomicsXchg64(uintptr_t addr, uint64_t value) {
    *(volatile _Atomic uint64_t *)(void *)addr = value;
}

/* A width-native compare-exchange: one `lock cmpxchg`, returning the value the
 * processor COMPARED, which equals `expected` exactly when the exchange
 * happened. Each is handed an aligned 8-byte block or an object inside ONE
 * cache line — never an operand that crosses a line.
 *
 * ⚠ THE CAST SPELLINGS ARE THE INTRINSICS' OWN DECLARED PARAMETER TYPES, NOT
 * `int32_t`/`int64_t`. `c.lang.json` declares the 32-bit CAS as
 * `ptr<i32 "long">` under LLP64 and the 64-bit one as `ptr<i64 "long long">`,
 * and a NAMED vocabulary type is a different TypeId from a bare one
 * ([[D-LANG-TYPE-IDENTITY-VOCABULARY]]) — a bare `int64_t volatile *` is refused
 * by identity in a pointer position even though the representations agree.
 * Spelling the parameter's own type is the fix, not a cast around it. */
static uint32_t dssAtomicsCas32(uintptr_t addr, uint32_t expected,
                                uint32_t desired) {
    return (uint32_t)_InterlockedCompareExchange(
        (long volatile *)(void *)addr, (long)desired, (long)expected);
}

static uint64_t dssAtomicsCas64(uintptr_t addr, uint64_t expected,
                                uint64_t desired) {
    return (uint64_t)_InterlockedCompareExchange64(
        (long long volatile *)(void *)addr, (long long)desired,
        (long long)expected);
}

/* ⚠ THE COMBINATIONS NOTHING HERE SERVES. A small object crossing its 8-byte
 * block, and any size above 8, are unreachable from DSS's own lowering (THE
 * WIDTH BOUND). Refusing LOUDLY is the project's rule: a silently non-atomic
 * access is the exact defect this whole route exists to remove. */
static void dssAtomicsRefuse(void) { abort(); }

static void dssAtomicsCopy(unsigned char *dst, unsigned char const *src,
                           size_t n) {
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
}

static int dssAtomicsBytesEqual(unsigned char const *a, unsigned char const *b,
                                size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Does the n-byte operand at `addr` cross a cache-line boundary? */
static int dssAtomicsCrossesACacheLine(uintptr_t addr, size_t n) {
    uintptr_t const offsetInLine =
        addr & (uintptr_t)(DSS_ATOMICS_CACHE_LINE_BYTES - 1u);
    return offsetInLine + (uintptr_t)n > (uintptr_t)DSS_ATOMICS_CACHE_LINE_BYTES;
}

/* ── THE PROCESS LOCK ──────────────────────────────────────────────────────
 *
 * The default process heap's lock, around a byte copy. WHY THE PROCESS HEAP'S
 * LOCK is argued in the file header; what this function adds is only the
 * fail-loud edges. `GetProcessHeap` cannot return NULL for a running process
 * and `HeapLock` cannot fail on a serialized heap — but a failure to take the
 * arbiter is a failure to be atomic, and the one wrong response to it is to
 * copy anyway. */
static void dssAtomicsCopyUnderTheProcessLock(unsigned char *dst,
                                              unsigned char const *src,
                                              size_t n) {
    void *const processHeap = GetProcessHeap();
    if (processHeap == NULL || HeapLock(processHeap) == 0) dssAtomicsRefuse();
    dssAtomicsCopy(dst, src, n);
    if (HeapUnlock(processHeap) == 0) dssAtomicsRefuse();
}

/* The compare-exchange under the same lock: compare exactly the object's bytes
 * with `*expected`; on a match copy `*desired` in, otherwise copy the object
 * OUT into `*expected` — so the value a failing caller reloads was observed
 * under the very lock every store and load of this object takes. */
static _Bool dssAtomicsCompareExchangeUnderTheProcessLock(
    unsigned char *object, unsigned char *expected,
    unsigned char const *desired, size_t n) {
    void *const processHeap = GetProcessHeap();
    if (processHeap == NULL || HeapLock(processHeap) == 0) dssAtomicsRefuse();
    int const matched = dssAtomicsBytesEqual(object, expected, n);
    if (matched) {
        dssAtomicsCopy(object, desired, n);
    } else {
        dssAtomicsCopy(expected, object, n);
    }
    if (HeapUnlock(processHeap) == 0) dssAtomicsRefuse();
    return matched != 0;
}

/* ── __atomic_load ─────────────────────────────────────────────────────────
 *
 * ★ A LOAD NEVER WRITES. Inside a line it is one plain read — of the containing
 * aligned 8-byte block when the object fits one, else of the object at its own
 * width. Across a line it is a copy under the process lock. Neither needs write
 * access to the object, so a `const` object in read-only memory reads back
 * instead of faulting. */
void __atomic_load(size_t n, void const *mem, void *ret, int model) {
    (void)model;
    uintptr_t const addr = (uintptr_t)mem;

    if (n != 0 && n <= 8 && ((addr & (uintptr_t)7) + (uintptr_t)n) <= 8) {
        uint64_t const block = dssAtomicsBlockLoad64(addr & ~(uintptr_t)7);
        dssAtomicsCopy((unsigned char *)ret,
                       (unsigned char const *)&block + (addr & (uintptr_t)7), n);
        return;
    }
    if (n == 4 || n == 8) {
        if (dssAtomicsCrossesACacheLine(addr, n)) {
            dssAtomicsCopyUnderTheProcessLock((unsigned char *)ret,
                                              (unsigned char const *)mem, n);
            return;
        }
        if (n == 4) {
            uint32_t const v = dssAtomicsLineLoad32(addr);
            dssAtomicsCopy((unsigned char *)ret, (unsigned char const *)&v, 4);
            return;
        }
        uint64_t const v = dssAtomicsLineLoad64(addr);
        dssAtomicsCopy((unsigned char *)ret, (unsigned char const *)&v, 8);
        return;
    }
    dssAtomicsRefuse();
}

/* ── __atomic_store ────────────────────────────────────────────────────────
 *
 * ★ n = 4 and n = 8 — the only widths DSS can route here. Inside a line, a
 * SINGLE `xchg`, which carries x86's implicit lock, touches exactly the
 * object's own bytes and needs no retry loop. Across a line, a copy of exactly
 * those bytes under the process lock — never a locked instruction, because
 * that is a bus lock.
 *
 * ⚠ THE RESIDUE PATH IS THE WIDER BLOCK CAS, AND IT IS NEIGHBOUR-SAFE BY
 * CONSTRUCTION. It reads the containing aligned block, splices our bytes in,
 * and compare-exchanges the whole block back; if any adjacent byte changed in
 * between, the compare FAILS and the loop reloads, so a concurrent write to a
 * neighbour can never be lost. It is used only for the sizes DSS cannot emit
 * (see THE WIDTH BOUND), never for n = 4 or n = 8. */
void __atomic_store(size_t n, void *mem, void const *val, int model) {
    (void)model;
    uintptr_t const addr = (uintptr_t)mem;

    if (n == 4 || n == 8) {
        if (dssAtomicsCrossesACacheLine(addr, n)) {
            dssAtomicsCopyUnderTheProcessLock((unsigned char *)mem,
                                              (unsigned char const *)val, n);
            return;
        }
        if (n == 4) {
            uint32_t v = 0;
            dssAtomicsCopy((unsigned char *)&v, (unsigned char const *)val, 4);
            dssAtomicsXchg32(addr, v);
            return;
        }
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

/* ── __atomic_compare_exchange ─────────────────────────────────────────────
 *
 * ★ THE CONTRACT, AS THE MEASURED CALL SITE READS IT: if the object's bytes
 * equal `*expected`, write `*desired` into the object and return true;
 * otherwise write the object's CURRENT bytes into `*expected` and return false.
 * Either outcome is ONE atomic step against every store, load and
 * compare-exchange of the same object, because all of them take the same arm
 * for the same (address, size): the processor's for an operand inside one
 * cache line, the process lock for one that crosses a line.
 *
 * ★ THE RESULT IS 0 OR 1 AND NOTHING ELSE; the measured caller tests AL alone.
 *
 * ⚠ THE RESIDUE PATH MIRRORS THE STORE'S. A small object inside an aligned
 * 8-byte block is compared inside ONE atomic snapshot of that block, and the
 * whole block is compare-exchanged back with the new bytes spliced in; a
 * neighbour's concurrent write fails that CAS and the loop re-reads rather than
 * losing it. It serves only the sizes DSS cannot emit (THE WIDTH BOUND). */
_Bool __atomic_compare_exchange(size_t n, void *mem, void *expected,
                                void *desired, int successModel,
                                int failureModel) {
    (void)successModel;
    (void)failureModel;
    uintptr_t const addr = (uintptr_t)mem;

    if (n == 4 || n == 8) {
        if (dssAtomicsCrossesACacheLine(addr, n)) {
            return dssAtomicsCompareExchangeUnderTheProcessLock(
                (unsigned char *)mem, (unsigned char *)expected,
                (unsigned char const *)desired, n);
        }
        if (n == 4) {
            uint32_t wanted      = 0;
            uint32_t replacement = 0;
            dssAtomicsCopy((unsigned char *)&wanted,
                           (unsigned char const *)expected, 4);
            dssAtomicsCopy((unsigned char *)&replacement,
                           (unsigned char const *)desired, 4);
            uint32_t const observed = dssAtomicsCas32(addr, wanted, replacement);
            if (observed == wanted) return 1;
            dssAtomicsCopy((unsigned char *)expected,
                           (unsigned char const *)&observed, 4);
            return 0;
        }
        uint64_t wanted      = 0;
        uint64_t replacement = 0;
        dssAtomicsCopy((unsigned char *)&wanted, (unsigned char const *)expected,
                       8);
        dssAtomicsCopy((unsigned char *)&replacement,
                       (unsigned char const *)desired, 8);
        uint64_t const observed = dssAtomicsCas64(addr, wanted, replacement);
        if (observed == wanted) return 1;
        dssAtomicsCopy((unsigned char *)expected,
                       (unsigned char const *)&observed, 8);
        return 0;
    }
    if (n != 0 && n < 8 && ((addr & (uintptr_t)7) + (uintptr_t)n) <= 8) {
        uintptr_t const base = addr & ~(uintptr_t)7;
        size_t const    off  = (size_t)(addr & (uintptr_t)7);
        uint64_t        seen = dssAtomicsBlockLoad64(base);
        for (;;) {
            if (!dssAtomicsBytesEqual((unsigned char const *)&seen + off,
                                      (unsigned char const *)expected, n)) {
                dssAtomicsCopy((unsigned char *)expected,
                               (unsigned char const *)&seen + off, n);
                return 0;
            }
            uint64_t next = seen;
            dssAtomicsCopy((unsigned char *)&next + off,
                           (unsigned char const *)desired, n);
            uint64_t const prior = dssAtomicsCas64(base, seen, next);
            if (prior == seen) return 1;
            seen = prior;
        }
    }
    dssAtomicsRefuse();
    return 0;
}
