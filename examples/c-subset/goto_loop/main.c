// FC5 — a loop built ENTIRELY from `goto`/labels (no while/for): integer
// multiply via repeated addition. Exercises a BACKWARD goto (`loop`) forming the
// cycle and a FORWARD exit goto (`end`). The CFG is a real loop the optimizer's
// Mem2Reg / SimplifyCfg / LICM must handle as unstructured control flow.
//
// Fold-resistant: runtime args, so `acc` accumulates through a live loop. A
// mis-wired back-edge or a dropped phi on the loop join would change the count.
int mul(int a, int b) {
    int acc = 0;
    int i = 0;
loop:
    if (i >= b) goto end;       // forward exit goto
    acc = acc + a;
    i = i + 1;
    goto loop;                  // backward goto -> the loop back-edge
end:
    return acc;                 // a * b
}

int main() {
    return mul(6, 7);           // 6 * 7 = 42
}
