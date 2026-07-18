/* SQLite testfixture arc (F001A blocker clear, 2026-07-18) — shipped <sched.h>
 * witness. `#include <sched.h>` resolves to shippedLibs/sched.json: the single
 * POSIX symbol `sched_yield` (dynamically linked from libc.so.6 / libSystem).
 * test4.c is the real consumer (`while( p->opnum<=p->completed ) sched_yield();`).
 *
 * sched_yield() is a REAL runtime call here — it yields the CPU and returns 0 on
 * success on every POSIX runtime — so the witness is its return value: exit 42
 * iff sched_yield() succeeded. RED-ON-DISABLE: drop sched_yield from sched.json
 * (or delete the descriptor) → `sched_yield` undeclared → this example stops
 * compiling. sched_yield is a WEAK libc export (nm -D: `W sched_yield`), which
 * links + loads exactly like the weak close/read/write in the shipped_unistd
 * witness — a broken import would fail LOUD at load (exit 127), not silently. */
#include <sched.h>

int main(void) {
    int r = sched_yield();      /* links + calls sched_yield (dynamic libc.so.6); 0 on success */
    return (r == 0) ? 42 : 1;
}
