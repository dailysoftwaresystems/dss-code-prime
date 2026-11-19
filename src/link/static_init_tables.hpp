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
// A module arrives carrying `AssembledModule::staticInitSchedule` — a list, in
// definition order, of (function symbol, priority-per-channel) facts the front end folded from
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
// ★★ EQUAL PRIORITIES RUN IN THE UNITS' INPUT ORDER, THEN EACH UNIT'S DEFINITION
// ORDER -- ONE RULE FOR EVERY FORMAT (P69 fold f26d; the coordinator's decision,
// which the operator may veto). The schedule carries that order already: a unit
// states its constructors as it defines them (HIR->MIR adds each function as it
// lowers it, MIR->LIR one entry per function in that order), an object reader as
// its table lists them, and the link's merge appends the units' schedules in input
// order -- so a tie keeps its place (a STABLE sort, no SymbolId read).
//   * DOCUMENTED: no vendor documents a tie -- GCC and Clang state only that a
//     smaller priority runs first, Microsoft only the alphabetical merge of the
//     `.CRT$` subsections -- so any deterministic order conforms.
//   * MEASURED (probe-reference-cc, runs 20261116-201121-47a61e0c and
//     20261116-201510-1d84face): this is the order of cl 19.52 + link.exe
//     (`.CRT$XCU`; DSS's PE runtime is UCRT, cl's), clang 18 on ELF and Apple
//     clang 21 on Mach-O in every cell, and gcc 13.3 on ELF at the default
//     priority.
//   * THE REFUSAL COST: MinGW-w64 gcc 13.2 on PE runs the exact reverse (its
//     runtime's `.ctors` walk, which DSS does not ship; cl disagrees on the same
//     format), and gcc 13.3 on ELF with an EXPLICIT equal priority runs
//     first-DECLARATION order, where clang on the same format runs definition order.
// Until this fold a tie was broken by SymbolId: first-DECLARATION order within a
// unit (its front end numbers at the first declaration), and across units the
// merged id, minted in the hash order of names -- an order that differed by the
// standard library that built DSS.
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
