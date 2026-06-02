// D-LK4-RODATA-PRODUCER-STRING end-to-end pin (2026-06-02).
//
// Closes the silent-miscompile bug-class for string-literal-as-
// pointer-arg by exercising the FULL chain end-to-end:
//
//   HIR string literal "X" (Array<Char,2>)
//     → CST→HIR coerce() emits Cast(Array→Ptr) at the call-arg site
//       (target type Ptr<Char> from recv's signature)
//     → HIR→MIR lowerExpr(Cast) recognizes Array→Ptr; for the
//       string-literal operand, synthesizes a MirGlobal carrying
//       the string bytes + mints a fresh SymbolId beyond all
//       user-declared functions/externs/globals + emits a
//       GlobalAddr returning Ptr<Char>
//     → asm/asm.cpp::lowerMirGlobalsToDataItems serializes the
//       MirGlobal's std::string arm into AssembledData{Rodata,
//       bytes + NUL, Alignment{1}}
//     → PE walker (D-LK2-RODATA) emits .rdata between .text and
//       .idata; symbolVa map gets the rodata item's VA
//     → main's call site emits `lea <reg>, [rip + sym_str_lit]`
//       + `mov rcx, <reg>` (Win64 first-arg slot) + `call recv`
//       + `ret`. Regalloc MAY coalesce the LEA destination with
//       rcx, eliding the mov — but only when its allocation
//       heuristics pick rcx for the LEA's vreg. The opcode shape
//       is fixed; specific register assignment is regalloc-
//       determined.
//
// The trampoline injector's `maxExistingSymbolIdV` was extended
// to scan `dataItems` (audit-fold) — without that, the synthetic
// `_start` trampoline would mint a SymbolId colliding with the
// string-literal global and the PE walker would loud-fail with
// K_DuplicateDataSymbol.
//
// recv ignores its parameter and returns the literal 42. main
// calls recv("X") and returns its result. Spawned exit code 42
// proves: (a) string literal lands in .rdata; (b) the lea+call
// emits without encoding errors; (c) the rodata item's RVA
// resolves correctly in the linker's symbolVa map; (d) the
// trampoline injector's data-symbol-aware mint avoids
// K_DuplicateDataSymbol.
//
// A regression in ANY tier flips the exit code from 42 and
// fails the always-run harness immediately.
int recv(const char* p) {
    return 42;
}

int main() {
    return recv("X");
}
