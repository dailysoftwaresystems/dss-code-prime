/* The OBJECT INPUT (the arm's `dependsOn` compiles it to a relocatable object, which the program's build takes as a
 * link input). Both directions of one link:
 *   - it calls a hidden function and reads a hidden datum that the PROGRAM's own unit defines and never uses itself;
 *   - it defines a hidden function that nothing in THIS object uses — the program's unit calls it. A relocatable
 *     object is completed by a later link, so that definition must survive this object's own release build. */
extern int scale_hidden(int x);
extern int offsets_hidden[2];

__attribute__((visibility("hidden"))) int obj_hidden_bias(void) { return 0; }

int obj_entry(int x) { return scale_hidden(x) + offsets_hidden[0] + offsets_hidden[1]; }
