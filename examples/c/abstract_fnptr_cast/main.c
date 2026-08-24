// c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME — the abstract fn-pointer CAST,
// end-to-end. This is the exact sqlite3.c:40989 `osGeteuid` shape:
//
//     #define osGeteuid ((uid_t(*)(void))aSyscall[21].pCurrent)
//     ... osGeteuid() ...
//
// a function's address is stored as a `void*` slot, cast to an ABSTRACT
// function-pointer type `(T (*)(args))` (a type-name with NO declarator name —
// the c26 feature), and CALLED through the resulting pointer. Before c26 the
// cast `(int (*)(int))p` failed to parse (P9006/P0009); now it types as
// `Ptr<FnSig(int)->int>` and the call lowers to a call-through-register.
//
// Exit arithmetic: the slot holds &dbl; ((int(*)(int))slot)(20) = 40, + 2 = 42.
// A broken abstract cast (mistyped as the bare base `int`, dropped name, or a
// bad indirect call) cannot produce 42 — it fails to compile or crashes.
int dbl(int v) { return v + v; }

int main() {
    void* slot = &dbl;
    int r = ((int (*)(int))slot)(20);
    return r + 2;
}
