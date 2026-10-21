/* The second translation unit: the same compound-literal shapes as main.c, each its own
 * object (see main.c). */

static int *mine = &(int){ 40 };
static int *arr = (int[]){ 1, 2 };

int *other_literal(void) { return mine; }
int *other_array(void) { return arr; }
