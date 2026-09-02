// THE `"x"` CONSTRAINT WITH A TEMPLATE THAT CAN ACTUALLY NAME AN INSTRUCTION
// (D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME).
//
// Before the SSE block landed in `asm-x86_64-att.lang.json`, `"x"` bound the
// `fpr` register class and NO declared instruction could name it: `__asm__("nop"
// : "=x"(r))` compiled, and every FP mnemonic a template might write was
// `A_AsmTextUnsupported … unknown mnemonic`. Every shape below is a program that
// could not be expressed at all, and each one RUNS.
//
// ⚠ EVERY SEED IS `volatile` SO THE RELEASE ARM STILL REACHES THE TEMPLATE. A
// folded constant would make this file pass without ever encoding an SSE
// instruction, which is the vacuous-green shape this project has paid for.
//
// ⚠ THE ASSERTED VALUES ARE CHOSEN TO DISCRIMINATE THE **WIDTH** OF THE ELECTED
// VARIANT, not merely to be non-zero. The `sd`/`ss` pair is ONE target opcode
// under two width-keyed guards (F2 = scalar double, F3 = scalar single), so a
// row that named the wrong width would still assemble and still run — it would
// just compute on the wrong half of the register. Each arithmetic shape below
// uses a value whose double and float encodings differ in the bits the result
// is read through, and the integer conversions read ABOVE BIT 31 so a 32-bit
// truncation cannot pass as a 64-bit answer.

static int failures = 0;

static void check(int ok, int shape) {
    if (!ok) { failures |= (1 << shape); }
}

int main(void) {
    // ── Shape 0: addsd — the row the whole block exists for. ────────────────
    {
        volatile double sa = 152.0, sb = 152.0;
        double r = sa;
        __asm__("addsd %1, %0" : "+x"(r) : "x"(sb));
        check(r == 304.0, 0);
    }

    // ── Shape 1: subsd / mulsd / divsd, chained through separate templates. ─
    {
        volatile double sa = 1000.0, sb = 8.0;
        double r = sa;
        __asm__("subsd %1, %0" : "+x"(r) : "x"(sb));   // 992
        __asm__("divsd %1, %0" : "+x"(r) : "x"(sb));   // 124
        __asm__("mulsd %1, %0" : "+x"(r) : "x"(sb));   // 992
        check(r == 992.0, 1);
    }

    // ── Shape 2: the SINGLE-precision twins. ────────────────────────────────
    // If these rows had been declared at width 64 the F2 (double) variant would
    // be elected over a 4-byte value and the result would not be 304.0f.
    {
        volatile float fa = 152.0f, fb = 152.0f;
        float r = fa;
        __asm__("addss %1, %0" : "+x"(r) : "x"(fb));
        check(r == 304.0f, 2);
    }

    // ── Shape 3: movaps — the register-to-register FP move. ─────────────────
    // The one row in the block declaring NO `width`: `movaps`'s encoding guard
    // states none, so the row states none either and the width is derived.
    {
        volatile double sa = 6.25;
        double r = 0.0;
        __asm__("movaps %1, %0" : "=x"(r) : "x"(sa));
        check(r == 6.25, 3);
    }

    // ── Shape 4: movsd, BOTH memory directions through one spelling. ────────
    // `movsd` names `movsd_load` AND `movsd_store`; which one an input denotes
    // is decided by the target's operand-kind guards, never by the dialect.
    {
        volatile double sa = 81.5;
        double src = sa, mid = 0.0, r = 0.0;
        __asm__("movsd %1, %0" : "=x"(r) : "m"(src));    // load  m64 -> xmm
        __asm__("movsd %1, %0" : "=m"(mid) : "x"(r));    // store xmm -> m64
        check(mid == 81.5, 4);
    }

    // ── Shape 5: the width-CHANGING conversions, which need `destWidth`. ────
    // Declared with `width` alone these are refused by the width-honesty gate
    // (`register '%1' is 64 bits wide but register '%0' is 32 bits`). 152.5 is
    // exactly representable in both formats, so a correct round trip is exact.
    {
        volatile double sd = 152.5;
        float  f = 0.0f;
        double back = 0.0;
        __asm__("cvtsd2ss %1, %0" : "=x"(f) : "x"(sd));
        __asm__("cvtss2sd %1, %0" : "=x"(back) : "x"(f));
        check(f == 152.5f && back == 152.5, 5);
    }

    // ── Shape 6: double -> signed, READ ABOVE BIT 31. ───────────────────────
    // 3000000000 does not fit a signed 32-bit int, so a truncating or 32-bit
    // variant cannot produce this answer by accident.
    //
    // ⚠⚠ `long long`, NOT `long`, AND THAT IS A MEASUREMENT RATHER THAN A STYLE
    // CHOICE. `cvttsd2si`/`cvtsi2sdq` are the `q` forms and bind a 64-bit GPR,
    // and this file was first written with `long` — which is 64 bits under
    // LP64 and THIRTY-TWO under Windows' LLP64. ✔MEASURED: the elf64 arm passed
    // while `x86_64:pe64-x86_64-windows-exec` refused with `register '%1' is 64
    // bits wide but register '%0' is 32 bits`. The refusal was CORRECT — the
    // template really did name two widths — and the bug was this example's,
    // reachable on only one of its own two targets. `long long` is 64 bits
    // under both data models, so the operand matches the mnemonic everywhere.
    {
        volatile double sa = 3000000000.0;
        long long r = 0;
        __asm__("cvttsd2si %1, %0" : "=r"(r) : "x"(sa));
        check(r == 3000000000LL, 6);
    }

    // ── Shape 7: signed -> double, the same value back the other way. ───────
    {
        volatile long long sa = 3000000000LL;
        double r = 0.0;
        __asm__("cvtsi2sdq %1, %0" : "=x"(r) : "r"(sa));
        check(r == 3000000000.0, 7);
    }

    // ── Shape 8: THE MATCHED NEGATIVE CONTROL. ──────────────────────────────
    // Shape 2 asserts that the SINGLE-precision row is not the double one. This
    // shape asserts the converse in the direction that would otherwise pass
    // silently: a value whose float and double results AGREE, so it proves
    // nothing on its own and says so. It exists to show that the discriminating
    // power of shapes 0 and 2 comes from the VALUES they chose — 2.0 + 2.0 is
    // 4.0 under either width, and a row declared at the wrong width would sail
    // straight through this one.
    {
        volatile double sa = 2.0, sb = 2.0;
        double r = sa;
        __asm__("addsd %1, %0" : "+x"(r) : "x"(sb));
        check(r == 4.0, 8);
    }

    // ── Shape 9: REGISTER-to-REGISTER movsd — its own opcode, not `movaps`. ─
    // This spelling was refused until `movsd_reg` was declared: `movsd_load`
    // and `movsd_store` guard on MEMORY shapes only, so the one register move
    // gas accepts was reachable here only through a NEIGHBOURING opcode.
    //
    // ⚠ THIS SHAPE CANNOT TELL `movsd` FROM `movaps` AND SAYS SO, exactly as
    // shape 8 does. Both copy the low 64 bits correctly, so a scalar `double`
    // read back through C is identical either way. The difference is real but
    // invisible from here: ✔MEASURED under gcc 13.3.0 with two doubles packed
    // in one xmm, `movsd` leaves the destination's HIGH LANE unchanged (111)
    // while `movaps` clobbers it (222). That discrimination lives where it can
    // be seen — the byte pin in `tests/asm/test_asm_x86_sse_dialect_rows.cpp`,
    // which asserts F2 0F 10 and would redden on 0F 28. What this shape proves
    // is the half a unit test cannot: that the spelling assembles, links and
    // RUNS in a real program.
    {
        volatile double sa = 12.5;
        double r = 0.0;
        __asm__("movsd %1, %0" : "=x"(r) : "x"(sa));
        check(r == 12.5, 9);
    }

    // ── Shape 10: the single-precision twin, F3 0F 10. ──────────────────────
    // 6.25 is exactly representable in both formats, so a wrongly-elected
    // width would have to corrupt the value rather than merely round it.
    {
        volatile float sa = 6.25f;
        float r = 0.0f;
        __asm__("movss %1, %0" : "=x"(r) : "x"(sa));
        check(r == 6.25f, 10);
    }

    return failures == 0 ? 42 : (100 + failures);
}
