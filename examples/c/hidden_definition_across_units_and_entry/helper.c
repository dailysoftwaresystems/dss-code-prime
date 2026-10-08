/* The DEFINING unit: a hidden function and a hidden datum that NOTHING in this unit uses — the other unit does, by
 * name (the usual shape of a library's internal cross-unit helpers under -fvisibility=hidden). Hidden keeps them out
 * of the image's dynamic surface; it does not keep the image's other units from linking to them. */
__attribute__((visibility("hidden"))) int scale_hidden(int x) { return x * 2; }
__attribute__((visibility("hidden"))) int offsets_hidden[2] = {1, 1};
