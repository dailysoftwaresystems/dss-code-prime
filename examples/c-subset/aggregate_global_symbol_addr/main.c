/* c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): a file-scope AGGREGATE global
 * whose initializer carries LINK-TIME-CONSTANT members — a function address, a
 * `&global`, and a string literal nested inside a struct / array-of-struct (the
 * sqlite `aSyscall[]` shape: `{{"open",(sqlite3_syscall_ptr)posixOpen,0},…}`).
 *
 * const-eval cannot fold an address inside the aggregate, so before c67 the whole
 * table fell to the runtime-init path and `emitGlobals_`'s lowerExpr on the
 * aggregate tripped the bit-field-rvalue fail-loud guard (D-CSUBSET-BITFIELD-
 * RVALUE-RUNTIME). c67 emits these as STATIC DATA with abs64 RELOCATIONS at the
 * member offsets — the C-correct, gcc-matching placement, extending the F5 scalar
 * mechanism (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL) from top-level scalars to aggregate
 * MEMBERS. The aggregate lands in writable-at-load .data (the loader patches the
 * resolved, and on a PIE image slid, target VAs into the slots).
 *
 * RUNTIME-verified — every member kind is exercised, not just compiled:
 *   - a string-literal member        -> read its bytes  (name[0])
 *   - a `&global` member             -> dereference it  (*ptr)
 *   - a function-address member      -> CALL through it  (fn())
 *   - a NULL function-pointer member -> compare == 0     (the trailing `0`)
 *   - a NESTED struct member         -> reach &global / string through the nest
 * exit 42 iff ALL members verify. A wrong/unpatched reloc yields a NULL or wrong
 * address -> a SIGSEGV or a non-42 exit, never a silent pass.
 *
 * Red-on-disable: revert the c67 classify arm (tryClassifyAggregateConst /
 * tryClassifyNullPointerConst / the Cast+Ref symbol-addr peels) OR the asm
 * encodeAggregateValue reloc-leaf arm -> the table falls to runtime-init and the
 * compile fails with H_UnsupportedLoweringForKind (D-CSUBSET-BITFIELD-RVALUE-
 * RUNTIME) on every target.
 */

static int target = 30;             /* &target -> abs64 reloc into a .data slot */

static int seven(void) { return 7; }

struct Inner { int *ip; };          /* a nested aggregate carrying a &global     */

struct Row {
    const char  *name;              /* string literal -> rodata, abs64 reloc     */
    int        (*fn)(void);         /* function address -> abs64 reloc           */
    struct Inner in;                /* nested struct (struct-in-struct offsets)  */
};

/* array-of-struct, last row's fn is the null pointer constant (the aSyscall `0`) */
static struct Row table[] = {
    { "ok", seven, { &target } },
    { "no", 0,     { 0 } },
};

int main(void) {
    int ok = 1;
    ok = ok && (table[0].fn() == 7);     /* call through the function-addr member */
    ok = ok && (table[0].name[0] == 'o');/* read the rodata string literal bytes  */
    ok = ok && (table[0].name[1] == 'k');
    ok = ok && (*table[0].in.ip == 30);  /* deref &target through the nested agg  */
    ok = ok && (table[1].fn == 0);       /* the trailing null function pointer    */
    ok = ok && (table[1].name[0] == 'n');
    return ok ? 42 : 1;
}
