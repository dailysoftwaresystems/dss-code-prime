/* The PROGRAM's unit: defines a hidden function and a hidden datum it never uses — only the object input does, by
 * name — calls the object's entry point, and calls the object's own hidden function, which nothing in the object
 * uses. Exit 42 iff obj_entry(20) + obj_hidden_bias() == 40 + 1 + 1 + 0. */
__attribute__((visibility("hidden"))) int scale_hidden(int x) { return x * 2; }
__attribute__((visibility("hidden"))) int offsets_hidden[2] = {1, 1};

extern int obj_entry(int x);
__attribute__((visibility("hidden"))) int obj_hidden_bias(void);

int main(void) { return obj_entry(20) + obj_hidden_bias() == 42 ? 42 : 1; }
