/* The CALLING unit, whose `main` is itself hidden: the entry trampoline reaches it by name, so it is still the
 * program's entry. Calls helper.c's hidden function and reads its hidden datum. Exit 42 iff both arrived intact:
 * scale_hidden(20) + offsets_hidden[0] + offsets_hidden[1] == 40 + 1 + 1. */
__attribute__((visibility("hidden"))) int scale_hidden(int x);
extern int offsets_hidden[2];

__attribute__((visibility("hidden"))) int main(void) {
    return scale_hidden(20) + offsets_hidden[0] + offsets_hidden[1] == 42 ? 42 : 1;
}
