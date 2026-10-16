/* P69 (lane `cs`, D-C-A-COMPOUND-LITERAL-IS-ITS-INITIALIZERS-VALUE-NOT-AN-OBJECT): two
 * translation units, each with file-scope compound literals of the SAME shapes, linked into
 * one program. Each literal is its own nameless object with internal linkage (C 6.5.2.5p5:
 * static storage; C 6.2.2: no name, no linkage to share), so the cross-unit merge keeps all
 * four — none collides with its twin, none is folded into it — and a write to one leaves
 * the other alone. Exit 42 = every check passed. */

extern int *other_literal(void);
extern int *other_array(void);

static int *mine = &(int){ 40 };
static int *arr = (int[]){ 1, 2 };

int main(int argc, char **argv) {
    (void)argv;
    int *theirs = other_literal();
    if (theirs == mine) return 1;             /* two units, two objects */
    *mine += argc;                            /* 41 */
    if (*theirs != 40) return 2;              /* theirs is untouched */
    *theirs += 2;
    if (*mine != 41 || *theirs != 42) return 3;
    int *a2 = other_array();
    if (a2 == arr || a2[1] != 2 || arr[1] != 2) return 4;
    a2[1] = 7;
    if (arr[1] != 2) return 5;
    return *mine + 1;                         /* 42 */
}
