/* c37 (D-CSUBSET-FUNCTION-DESIGNATOR-CAST) runtime witness: a function
 * DESIGNATOR cast to a fn-ptr typedef — the sqlite SQLITE_DYNAMIC /
 * destructor-pointer / syscall-pointer shape ((sqlite3_destructor_type)fn) —
 * then CALLED through. Per C 6.3.2.1p4 + 6.3.2.3p8 a function designator
 * decays to the function's address, so the cast is value-preserving (a no-op
 * Bitcast over the GlobalAddr); the call through the cast pointer must reach
 * `helper`. RED-ON-DISABLE: pre-c37 `(fp_t)helper` was a spurious
 * S_InvalidCast, so this corpus would NOT EVEN COMPILE (compile is the pin;
 * exit 42 confirms the address is correct). Uses int (32-bit) ops only and a
 * direct call (no binary p+n). helper(40) + 2 = 42. */
typedef int (*fp_t)(int);

int helper(int v) { return v; }

int main(void) {
    fp_t f = (fp_t)helper;   /* function designator -> fn-ptr: the c37 cast */
    return f(40) + 2;        /* call through the cast pointer: 40 + 2 = 42 */
}
