#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/section_kind.hpp"

#include <cstdint>
#include <vector>

// ── D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN ──────────────
//
// THE STATIC-INITIALIZER SCHEDULE, TURNED INTO AN ORDER.
//
// A module arrives carrying `AssembledModule::staticInitSchedule` — an unordered
// set of (function symbol, priority-per-channel) facts the front end folded from
// `__attribute__((constructor))` / `((destructor))`. Which one runs first is a
// WHOLE-PROGRAM question: the answer depends on every translation unit at once,
// so it is computed once, at the only tier that has them all.
//
// ★★ THE ORDER IS ONE COMPARATOR READ IN TWO DIRECTIONS, AND THAT IS MEASURED,
// NOT CHOSEN. ✔2026-08-28, gcc 13.3.0 / clang 18.1.3 / mingw-w64 gcc 13.2.0
// probed SEPARATELY, each function printing its own tag so stdout order IS run
// order — all three produce exactly:
//     c101 c102 cBARE | MAIN | dBARE d102 d101
// Ascending priority with the unprioritized form last; the after-entry channel is
// that same sequence walked BACKWARD. So there is ONE `staticInitOrder` and the
// phase decides how it is consumed — never two orderings that can drift apart.
//
// ⚠⚠ ORDER AMONG EQUAL PRIORITIES IS NOT PORTABLE AND IS NOT PROMISED. ✔MEASURED:
// two bare same-priority constructors run 1,2 under Linux gcc and clang and 2,1
// under mingw-w64 gcc. DSS therefore breaks ties by merged SymbolId — DETERMINISM
// is the contract (the same input must give the same image), not agreement with
// any one reference, and no test may assert a sequence for equal priorities.
//
// ⓘ THERE IS DELIBERATELY NO EMITTED TABLE HERE, AND THAT REFUTES THE OBVIOUS
// DESIGN. `.init_array` / `__mod_init_func` / `.CRT$XCU` are how a program tells
// a C RUNTIME what to run before main — and DSS links no crt: it synthesizes
// `_start` itself and that entry IS the runtime. ✔MEASURED on a DSS
// elf64-x86_64-linux-exec artifact: `readelf -d` shows an eleven-entry dynamic
// section with NO `DT_INIT_ARRAY` and no `DT_INIT`, and ld.so walks the TAG, never
// the section; PE runs no `_initterm` because the UCRT startup object is never
// linked. So on every image DSS produces and starts, such a section would be bytes
// that nothing reads. The format documents declare WHO RUNS the schedule
// (`staticInitializers.runner`) instead, and the entry trampoline reads this
// ordering directly. The `imageLoader` arm of that key is where an emitted section
// would become load-bearing; it needs a writer that emits one, which is recorded
// on the anchor rather than half-built here.
namespace dss::linker {

// One entry of the ORDERED schedule for a single channel.
struct StaticInitOrderEntry {
    SymbolId      symbol{};
    std::uint32_t priority = kUnprioritizedStaticInit;
};

// The module's schedule for `phase`, ordered as the runtime must execute it —
// already reversed for the after-entry channel, so a caller walks the returned
// vector front-to-back in BOTH directions and cannot get the reversal wrong.
//
// ★ THE REVERSAL IS APPLIED HERE, ONCE, rather than left to each caller. A
// `if (phase == AfterEntry) reverse` at each call site is the duplicated-truth-
// table shape that ends with two consumers running a program's destructors in
// different orders.
[[nodiscard]] DSS_EXPORT std::vector<StaticInitOrderEntry>
staticInitOrder(AssembledModule const& module, StaticInitPhase phase);

} // namespace dss::linker
