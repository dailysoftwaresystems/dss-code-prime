// C23 §6.5.6 (D-CSUBSET-NULLPTR-TYPED-OPERAND-ESCAPES-THE-GATE): the sibling of
// `nullptr_arithmetic.c`. THAT file pins the operand SPELLED `nullptr`; this one
// pins the operand whose nullptr_t-ness is a property of the TYPE — an array
// element, a dereference, a member, a call result. The fail-loud operator gate
// reached the first kind through a syntactic descent and could not reach these,
// so `typeof(nullptr)` operands escaped it in two different ways at once:
//
//   * `a[1] + 1` / `*p + 1` walled one tier down as
//     `error[H0009] array/pointer index element type has no computable size`
//     — a TRUE refusal with a FALSE reason. `TypeInterner::representationType`
//     projects nullptr_t to `Ptr<Void>` on the way out of the semantic tier, so
//     MIR was answering about `void *`, not about nullptr_t;
//   * `a[1] < a[2]` and `-a[1]` COMPILED AND LINKED, rc=0, no diagnostic at all.
//
// gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`), probed SEPARATELY, reject
// every one of these ("invalid operands to binary +"). This tier is the LAST that
// can tell them apart: below it nullptr_t and `void *` are the same type, which is
// also why this gate is a hard prerequisite for ever admitting GNU `void * + 1`.
//
// RED-ON-DISABLE: drop the `subtreeType` fallback from the gate's
// `isNullptrOperand` (restore the bare syntactic descent) and every row below
// goes silent or turns into H0009.
typedef typeof(nullptr) nptr_t;

int bad(void) {
    nptr_t a[3];
    nptr_t *p = &a[0];
    (void)(a[1] + 1);
    (void)(*p + 1);
    (void)(a[1] < a[2]);
    (void)(-a[1]);
    return 0;
}
