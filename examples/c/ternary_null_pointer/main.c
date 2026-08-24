/* c49 (D-CSUBSET-TERNARY-NULL-POINTER, C 6.5.15p6): a `cond ? 0 : ptr` (or
 * `cond ? ptr : 0`) conditional has the POINTER type — the int-literal-0 arm is a
 * null pointer constant. Frontier sqlite3.c:31769 `return n<=0 ? 0 : sqlite3Malloc(n);`
 * (S0008). RED-ON-DISABLE: revert the combineTernary null-ptr arms -> S0008. */
int obj;
void *gp;
void *f(int n){ return n<=0 ? 0 : gp; }     /* null-const FIRST (the sqlite shape) */
void *g(int n){ return n>0  ? gp : 0; }     /* null-const SECOND (reverse order) */
int pick(int x){ return x; }                /* keep n runtime-opaque */
int main(void){
    gp = &obj;
    int a = pick(5), b = pick(-1);
    /* f(5)=gp=&obj, f(-1)=0; g(5)=gp, g(-1)=0 */
    if (f(a) != &obj) return 1;
    if (f(b) != 0)    return 2;
    if (g(a) != &obj) return 3;
    if (g(b) != 0)    return 4;
    return 42;
}
