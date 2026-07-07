/* c79 (D-CSUBSET-VARIADIC-ARG-ARRAY-DECAY): a string literal in a VARIADIC
 * TAIL (an argument position beyond the declared parameters, so there is NO
 * parameter type to coerce against) must still undergo array-to-pointer decay
 * (C 6.5.2.2p6-7 — the default argument promotions include the lvalue
 * conversions; an Array<char,N> argument decays to char*).
 *
 * The exact sqlite construct (pragma foreign_key_list, sqlite3Pragma):
 *   sqlite3VdbeMultiLoad(v, 1, "iissssss", i, j, ..., "NONE");
 * where sqlite3VdbeMultiLoad is (Vdbe*, int, const char*, ...). Every call-arg
 * coercion targets the callee FnSig's DECLARED param types; an arg beyond them
 * had no type to coerce to and passed through UN-DECAYED -> the Array-typed
 * literal lowered as a MIR Const(string) -> a LIR literal-pool `mov` ->
 * error[A_NoMatchingEncodingVariant] "pool slot N is not an integer literal"
 * + error[A_FunctionEncodeAborted] (the LAST encoder-stage sqlite error).
 *
 * FIX: coerceCallArg decays an Array-typed arg with no valid param type via
 * the EXISTING coerce decay arm (the same synthetic Cast every decay site
 * emits). The callee reads the decayed char* via va_arg and verifies the
 * BYTES, so a wrong pointer or missing decay changes the exit.
 * RED-ON-DISABLE: revert the coerceCallArg decay -> the exact A_ error pair.
 * => 42 (success; any byte mismatch => 1). */
#include <stdarg.h>

static int multiLoad(int iDest, const char *zTypes, ...) {
    va_list ap;
    int bad = 0;
    int i;
    va_start(ap, zTypes);
    for (i = 0; zTypes[i] != 0; i++) {
        if (zTypes[i] == 'i') {
            int v = va_arg(ap, int);
            if (v != iDest + i) bad = 1;
        } else {
            const char *z = va_arg(ap, const char *);
            if (z == 0 || z[0] != 'N' || z[1] != 'O' || z[2] != 'N'
                || z[3] != 'E' || z[4] != 0) {
                bad = 1;
            }
        }
    }
    va_end(ap);
    return bad;
}

int main(void) {
    /* "NONE" sits in the variadic tail exactly like the sqlite call. */
    return multiLoad(1, "iis", 1, 2, "NONE") ? 1 : 42;
}
