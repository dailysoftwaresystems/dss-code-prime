/* UCRT-P4 (D-FFI-PE-ABORT-BEHAVIOR-NOOP-MACRO-STALE-AFTER-UCRT-FLIP):
   `_set_abort_behavior` is a REAL ucrtbase.dll binding, not a no-op macro.

   WHAT WAS WRONG. While pe's `library.pe` was msvcrt.dll, stdlib.json shipped
   `_set_abort_behavior` as a pe-gated no-op function-like MACRO `(0u)`, and
   that was HONEST: legacy msvcrt exports no such symbol at all, and its
   abort() does not raise the UCRT WER fault dialog, so "returns old-flags 0
   and changes nothing" was msvcrt's real semantics. TF-C111 flipped the pe CRT
   to ucrtbase.dll and killed both premises. abort() now binds UCRT, whose
   abort fast-fails through Windows Error Reporting, so the no-op stopped being
   semantics and became a real suppression GAP — and its failure mode is a
   HANG (a WER dialog on any host lacking the ambient DontShowUI policy),
   never a diagnostic. This example is the run witness for the fix.

   WHY THE ASSERTIONS ARE SHAPED THIS WAY — three MEASURED facts that refute
   the obvious design, stated so nobody "simplifies" them back:

   (1) THE DEFAULT IS 2, NOT 3. Release UCRT starts with `_CALL_REPORTFAULT`
       alone set (measured `prev=2` on this very binary); debug `ucrtbased`
       returns 1, the OPPOSITE bit. So nothing here asserts on the default —
       every assertion is about a transition THIS program caused.

   (2) EXIT 3 IS NOT "THE" ABORT EXIT CODE. It appears ONLY once
       `_CALL_REPORTFAULT` is CLEARED. Leave that bit set and abort()
       fast-fails 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN) instead. That is
       precisely what makes exit 3 a witness that the knob took effect rather
       than a restatement of "abort() was called".

   (3) `_WRITE_ABORT_MSG` IS BEHAVIOURALLY INERT ON RELEASE UCRT — flags=1
       emits no UCRT abort text whatsoever (the header's "debug only" note is
       accurate). So this example never asserts on abort TEXT, which would
       assert nothing. It witnesses that constant the only honest way: by its
       VALUE and by its being a DISTINCT bit slot in the same word.

   THE WITNESS, and what each step discriminates:
     · the two constants by VALUE (1, 2) and their OR (3) — there is no
       `_ABORT_BEHAVIOR_MASK`-style all-bits macro in the SDK, so "both" has
       to be spelled out;
     · STATEFULNESS: set `_CALL_REPORTFAULT`, then read it back through a
       mask=0 PURE QUERY. The no-op macro returns 0 from that query, so this
       step alone reds the macro arm with exit 20 — no abort, no WER, no
       Windows-version dependence;
     · the two bits are INDEPENDENT slots (set the other one, expect 3);
     · clear BOTH, confirm the query reads 0;
     · abort() -> exit 3. THE EXIT CODE IS THE ASSERTION; the wall clock is
       corroborating evidence and is reported as such. Controlled A/B, one
       source built twice differing ONLY in whether `_CALL_REPORTFAULT` is
       left set, INTERLEAVED 20 runs each so both share the same machine
       load, .NET Process stopwatch: cleared -> exit 3, min/median/max
       14.4/53.4/132.8 ms; set -> exit 0xC0000409, 151.7/337.2/690.2 ms.
       Separated with no overlap, ~6x at the median. The ABSOLUTE figures
       are load-dependent (an idle-machine pair measured 4.2-7.3 vs
       52.9-77.9 ms); the separation is the invariant, which is exactly why
       nothing here asserts a millisecond threshold.
     ⚠ DO NOT re-measure this with PowerShell `Start-Process -Wait`: it has a
       ~1 s floor on this host that swallows the whole effect and reported
       both arms at ~1.0 s.

   BOUNDED TIME IS FREE, AND IT IS THE POINT. The runner spawns through
   `runBinary(..., kRunBudget = 5000 ms)` and treats a timeout as a hard
   failure, so on a host where clearing `_CALL_REPORTFAULT` failed and WER
   raised its UI, this entry REDS at the budget instead of hanging ctest
   forever. A plain exit-code test cannot deliver that property.

   pe64-ONLY IS NOT A COVERAGE GAP: `_set_abort_behavior` is a pe/UCRT
   spelling with no elf or macho analogue, which is exactly why the
   stdlib.json symbol row carries `availableObjectFormats` ["pe"]. Nothing is
   printed, so there is no stdout pin to diverge per platform.

   RED-ON-DISABLE, both arms MEASURED on this example under BOTH harnesses
   (tests/examples/examples_runner AND the integrated_tests CLI mirror):
     · PARTIAL revert — restore the `(0u)` macro, leave the symbol row: this
       example exits 20 (`OS exit code == 3 (got 20)`). ★ AND THIS IS WHY THE
       EXAMPLE IS NOT REDUNDANT WITH THE STRUCTURAL PIN: the symbol row is
       still declared, so DSS still eagerly imports it and the emitted image
       still looks correct — the import-table pin in
       tests/ffi/test_pe_abort_behavior_binding.cpp stays GREEN on this
       mutation (measured, not assumed). Only the behaviour moves, and only
       this example and that file's descriptor audit see it.
     · FULL revert — macro back, symbol row and both constants removed: the
       constants are undeclared and this TU does not compile at all, honest
       `error[S0001] got _WRITE_ABORT_MSG` / `got _CALL_REPORTFAULT`, never a
       silent no-op. The emitted image also stops importing the symbol, which
       is the arm the structural pin catches. */
#include <stdlib.h>

int main(void) {
    unsigned int q;

    /* The two constants the no-op macro's discarded `mask` argument used to
       hide. Values mirror the SDK (ucrt/stdlib.h:63-65), where they are
       unsuffixed hex `int` literals. */
    if (_WRITE_ABORT_MSG != 1) return 22;
    if (_CALL_REPORTFAULT != 2) return 23;
    if ((_WRITE_ABORT_MSG | _CALL_REPORTFAULT) != 3) return 24;

    /* THE KNOB IS REAL AND STATEFUL. mask=0 is a pure query: it changes
       nothing and returns the current flags word. The no-op macro answers 0
       here and this returns 20. */
    (void)_set_abort_behavior(_CALL_REPORTFAULT, _CALL_REPORTFAULT);
    q = _set_abort_behavior(0u, 0u);
    if ((q & (unsigned int)_CALL_REPORTFAULT) == 0u) return 20;

    /* The two bits are INDEPENDENT slots of one word, not aliases. */
    (void)_set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG);
    q = _set_abort_behavior(0u, 0u);
    if (q != 3u) return 25;

    /* Clear BOTH -- this is the `flags=0` cell that measures exit 3. */
    (void)_set_abort_behavior(0u, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    q = _set_abort_behavior(0u, 0u);
    if (q != 0u) return 21;

    abort();     /* _CALL_REPORTFAULT cleared => exit 3, no WER round trip */
    return 26;   /* abort() is noreturn: reaching this is itself a failure */
}
