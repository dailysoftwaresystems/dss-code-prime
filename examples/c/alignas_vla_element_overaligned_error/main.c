// C11/C23 6.7.5 + 6.7.6.2 (D-CSUBSET-VLA): a VARIABLE-LENGTH array whose ELEMENT
// type is over-aligned — the member `alignas(32)` raises `struct Over`'s alignment
// to 32, above the 16-byte stack alignment the dynamic `sub sp` guarantees.
//
// THIS IS THE REMAINING HALF OF THE `L_OverAlignedStackLocal` CLASS, AND IT IS
// DELIBERATELY STILL REFUSED. A FIXED-size over-aligned local is now honoured
// (D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL, closed): its slot gets
// `align - stackAlignment` bytes of headroom and its address is rounded up. A VLA
// does not go through that path at all — it lowers to a runtime `sub sp, <size>` and
// takes the post-sub stack pointer as its base, which is only as aligned as the
// stack itself. Honouring it needs the SIZE to carry the headroom and the captured
// base to be rounded, which is a change to the dynamic-stack lowering and belongs to
// the VLA row that owns it.
//
// This example exists so that boundary stays PINNED rather than assumed: the
// diagnostic is still reachable, still an error, and still names the row that owns
// it. It replaces the pre-close `alignas_local_overaligned_error`, whose subject —
// `alignas(32) int loc;` — now compiles and runs (see `alignas_local_over32`).
//
// The diagnostic is raised at the MIR->LIR lowering tier from a pass that carries no
// source span, so the CLI renders it CODE-ONLY with no `:line:col` — hence
// `positioned:false`. The load-bearing pin is the CODE plus a rejected compile.
//
// Red-on-disable: remove the `elemAlign > stackAlign` gate in `lowerVlaAlloca` and
// this compiles, silently under-aligning every element of the array.

struct Over {
    alignas(32) int head;
    int body[3];
};

int use(void *p);

int use(void *p) { return (int)(((unsigned long long)p) & 31ull); }

int check(int n) {
    struct Over a[n];
    a[0].head = 1;
    return use(&a[0]);
}

int main(void) {
    if (check(4) != 0)
        return 1;
    return 42;
}
