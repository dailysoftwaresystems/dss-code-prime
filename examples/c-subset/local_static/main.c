// D-CSUBSET-LOCAL-STATIC witness: block-scope `static` locals get STATIC storage
// duration (C 6.2.4) — a hidden module-global, function-internal name, once-only
// LOAD-TIME init — so their value PERSISTS across calls. Three facets, each
// value-divergent (a stack-slot or re-init bug yields wrong values -> exit 7):
//
//   tick()    scalar counter: `static int n=0; n=n+1` -> 1,2,3 across 3 calls
//   arr_acc() static ARRAY:   `static int a[3]={...}` persists + indexes (25,30)
//   sib()     two SIBLING statics with the SAME source name `x` in distinct
//             blocks -> DISTINCT storage (distinct SymbolIds), never aliased
//
// RED-ON-DISABLE: route a static local to a stack VarDecl (revert the
// cst_to_hir staticStorage arm) -> each call re-inits -> tick() returns 1,1,1
// and the array/sibling values collapse -> exit 7. Every counter rides global
// memory no ConstFold/Mem2Reg can fold (the stores feed the cross-call state).
//
// Runs on x86_64 (PE+ELF) and arm64 (ELF qemu, Mach-O macos leg): the storage
// is the target-agnostic global-emission path (.data/.bss + GlobalAddr).

int tick(void) { static int n = 0; n = n + 1; return n; }

int arr_acc(void) { static int a[3] = {10, 20, 30}; a[1] = a[1] + 5; return a[1]; }

int sib(void) {
    int r = 0;
    { static int x = 1;   x = x + 1; r = x; }       // x: 1->2->3 ...
    { static int x = 100; x = x + 1; r = r + x; }   // a DISTINCT x: 100->101->102
    return r;
}

int main(void) {
    int t1 = tick(); int t2 = tick(); int t3 = tick();  // 1, 2, 3
    int a1 = arr_acc(); int a2 = arr_acc();             // 25, 30
    int s1 = sib(); int s2 = sib();                     // 2+101=103, 3+102=105
    return (t1 == 1 && t2 == 2 && t3 == 3
            && a1 == 25 && a2 == 30
            && s1 == 103 && s2 == 105) ? 42 : 7;
}
