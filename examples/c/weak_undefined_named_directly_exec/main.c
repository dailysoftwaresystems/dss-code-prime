/* D-LIR-WEAK-FUNCTION-NAMED-DIRECTLY-WHERE-NO-FIELD-REACHES-ZERO (P69 round 4) and
 * D-LK-WEAK-UNDEFINED-SYMBOL-NAMED-DIRECTLY-IS-NOT-ADDRESS-ZERO (P69): DSS's OWN
 * code naming a weak FUNCTION nothing defines — its address taken in code and a
 * guarded call — on EVERY image: ELF x86_64 exec and PIE, ELF aarch64 exec and
 * PIE, pe64, Mach-O arm64 and x86_64.
 *
 * `wf` is declared weak and no translation unit defines it, so it resolves to
 * nothing: `if (wf)` must read its address as NULL and skip the call. DSS reads a
 * WEAK import's address out of a slot (the GOT arm: `movq wf@GOTPCREL(%rip)`,
 * `adrp :got:` + `ldr`), which every image link fills with 0 for a name nothing
 * defines, and calls it THROUGH that address (`call *%reg`, `blr`) — gcc's own
 * shape for a weak symbol on aarch64 and under -fPIE / -fno-plt, clang's on PE,
 * ld64's GOT load. So the address is 0 and the call, never taken, would jump to
 * 0 — the run-time behaviour of the references' PLT entry, whose slot ld.so
 * leaves 0. ✔MEASURED that the references run THIS program to 42 (2026-10-07,
 * dssharness probe-reference-cc, this file's code): gcc 13.3.0 + GNU ld 2.42 and
 * clang 18 + ld.lld 18, -no-pie and -pie (run 20261007-081305-8067e92f), clang +
 * lld-link on PE (the same run), aarch64 gcc -no-pie and -pie (run
 * 20261007-081354-4f1e0abe), Apple clang 21 + ld64 with `-Wl,-U` (run
 * 20261007-130113-4f4f1f1c; by default ld64 refuses the symbols as undefined).
 *
 * WHAT WENT WRONG BEFORE: until P69 a weak FUNCTION resolved to nothing was
 * refused on every image, because the null slot an image gave a weak symbol WAS
 * the symbol and a direct reference would have computed the slot's own address
 * (✔MEASURED in P54: `if (maybe) return maybe();` took the call and SEGFAULTED).
 * P69 round 3 gave a direct reference the image's answer for its field
 * (`weakResolvedToNothing`) — enough on the x86_64 ELF exec, which sits at its
 * link address — and every other image refused DSS's direct `leaq wf(%rip)`,
 * page pair or `call wf` by name. Round 4 stops naming the symbol directly.
 *
 * THE PROGRAM. The weak function's address must be NULL (+40, and the call
 * never runs); the weak DATUM, read through its slot as DSS reads every data
 * import, must be NULL too (+2); and the CONTROL — a defined function, in the
 * same shape — must be present and called (+0, or +100 when its test fails).
 * 40 + 2 + 0 = 42.
 *
 * RED-on-disable: drop the weak-import routing in MIR→LIR (the constructor loop
 * that puts a weak import in the GOT set) -> the PIE, aarch64, pe64 and Mach-O
 * links refuse `wf`'s direct field by name, no artifact; the x86_64 exec, whose
 * every field reaches 0, stays the control.
 */

extern int wf(void) __attribute__((weak));
extern int wd __attribute__((weak));

int present(void)
{
    return 0;
}

int main(void)
{
    int r = 0;
    if (wf) {
        r += wf();
    } else {
        r += 40;
    }
    if (&wd) {
        r += wd;
    } else {
        r += 2;
    }
    if (present) {
        r += present();
    } else {
        r += 100;
    }
    return r;
}
