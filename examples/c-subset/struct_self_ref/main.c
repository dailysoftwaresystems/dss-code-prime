/* c24 (D-CSUBSET-SELF-REFERENTIAL-STRUCT): a SELF-REFERENTIAL struct — the node
 * holds a `Node*` to the next node. Build a 3-node linked list on the stack,
 * traverse it summing the values, and return the sum. This genuinely exercises
 * the self-referential pointer LAYOUT: `next` must sit at the correct offset
 * (after `value`) and the traversal must step node→node through that pointer.
 *
 * Sum = 7 + 14 + 21 = 42.  RED-ON-DISABLE: a wrong `next` field offset (e.g.
 * `next` resolved to offset 0, aliasing `value`) derails the traversal off 42.
 * The struct is defined with a self-referential field `struct Node *next` — the
 * exact construct this cycle enables. */
struct Node {
    int value;
    struct Node *next;
};

int sum_list(struct Node *head) {
    int total = 0;
    struct Node *cur = head;
    while (cur != 0) {
        total = total + cur->value;
        cur = cur->next;     /* step through the self-referential pointer */
    }
    return total;
}

int main(void) {
    struct Node a;
    struct Node b;
    struct Node c;
    a.value = 7;   a.next = &b;
    b.value = 14;  b.next = &c;
    c.value = 21;  c.next = 0;
    return sum_list(&a);
}
