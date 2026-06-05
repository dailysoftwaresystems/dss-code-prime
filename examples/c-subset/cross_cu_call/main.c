// LK11/CU6 cross-CU CALL (the first runnable multi-CU binary): main (this CU) calls a
// function DEFINED in a sibling CU (helper.c), referenced via a plain `extern`. The
// linker resolves the reference at link time — a sibling CU's definition shadows the
// library fallback (see helper.c's add5). add5(37) = 42, so the process exits 42 iff the
// cross-CU call resolved to helper.c's add5 (not a spurious library import).
extern int add5(int x);

int main() {
    return add5(37);
}
