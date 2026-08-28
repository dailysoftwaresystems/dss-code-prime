// D-CSUBSET-NULLPTR-T-DECLARABLE (C23 §6.2.5): a `nullptr_t` OBJECT, end to end.
//
// `nullptr_t` is an object type with EXACTLY ONE value and the size, alignment and
// representation of `void *`. DSS keeps its semantic IDENTITY distinct — so the
// one-way conversion rules hold and `_Generic` can tell it from `void *` — while
// PROJECTING it to `void *` at the semantic→HIR boundary, which is why it needs no
// register class, no ABI class and no width row of its own further down.
//
// Before this landed, EVERY line below `int r = 0;` failed to compile:
// ✔MEASURED at 301e2a63 on x86_64:pe64-x86_64-windows-exec, `typeof(nullptr) o =
// nullptr;` produced `error[I_NullptrTypeInMir]` + `error[I_StoreValueTypeMismatch]`
// — while gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`), probed
// SEPARATELY, both compile and run it.
//
// `typeof(nullptr)` is the only spelling C has for the type (there is no shipped
// `<stddef.h>` `nullptr_t` typedef yet), and `nullptr` is a LITERAL, so this file
// also stands as the runtime witness that the literal-operand `typeof` form works
// in a scalar local — the one position where it does.
//
// ⚠ THE ADDRESS IS OPAQUE ON PURPOSE. `sink` is a `volatile` pointer the optimizer
// cannot fold, so the `p != nullptr` arm is a genuine RUNTIME pointer comparison
// and the release arm witnesses the real thing rather than a constant. A projection
// that produced the wrong width, dropped the store, or mis-typed the copy cannot
// reach 42.
//
// exit = 1 + 2 + 4 + 8 + 16 + 11 = 42.

int main(int argc, char **argv) {
    (void)argv;

    int r = 0;

    // (1) THE OBJECT AND ITS INITIALIZING STORE — the exact pair that used to fail.
    typeof(nullptr) o = nullptr;
    if (o == nullptr) r |= 1;

    // (2) A SAME-TYPE COPY: a load of a `nullptr_t` slot and a store into another.
    typeof(nullptr) o2 = o;
    if (o2 == nullptr) r |= 2;

    // (3) THE ONE-WAY CONVERSION to a real pointer, then a runtime compare against
    // an OPAQUE non-null address, so the comparison cannot be folded away.
    void *p = o2;
    int local = argc;                      // argc is set by the OS: never foldable
    void *volatile sink = &local;
    if (p == nullptr) r |= 4;
    p = sink;
    if (p != nullptr) r |= 8;

    // (4) THE TRUTH VALUE. C23 6.3.2.4p2: a nullptr_t converts to `_Bool` as FALSE,
    // and a type with one value makes that true at run time as well as on paper.
    r |= (o ? 0 : 16);

    // (5) THE WIDTH. `sizeof(nullptr_t) == sizeof(void *)` on every DSS target
    // (all 64-bit), so this is 8 and is data-model-independent across the four
    // targets below. Contributes the last 11 by construction: 8 + 3.
    r += (int)sizeof(o) + 3;

    return r;                              // 1|2|4|8|16 = 31, + 11 = 42
}
