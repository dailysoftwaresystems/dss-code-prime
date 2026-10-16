/* AN ARRAY OF UNKNOWN SIZE (C 6.7.6.2): a flexible member, a type name, a pointer to one, and a
 * file-scope tentative definition the unit completes to one element (C 6.9.2p5). `sizeof` of one
 * is 0 here, as cl makes it (C4034) -- an extension DSS accepts on every target and says it does. */

struct dss_packet {
    int           length;
    unsigned char bytes[];
};

int dss_cells[];   /* completed to one element at the end of the unit */

int main(void) {
    if (sizeof(((struct dss_packet *)0)->bytes) != 0) return 1;
    if (sizeof(int[]) != 0) return 2;
    if (sizeof(struct dss_packet) != sizeof(int)) return 3;
    dss_cells[0] = 40;
    int (*whole)[] = &dss_cells;   /* a complete object: a pointer to an array of unknown size */
    (*whole)[0] += 2;
    return dss_cells[0];
}
