/* DATA FROM A STATIC ARCHIVE, READ BY THE PROGRAM THAT LINKS IT
 * (D-LK-SIBLING-DATA-IMPORT-SLOT-BOUND-TO-THE-OBJECT, P68 round 9, routed from
 * lane lm, whose getopt row it unblocks: `optind` & co. are such data).
 *
 * Every image format reads an extern datum through a slot that holds its
 * address, so when the datum is defined by a member the link pulls from an
 * archive, the link must give the code that slot — and give a DATA
 * initializer here (`via_init`) the object's own address. At the round's base
 * the slot-shaped reference was bound to the object itself, so the code read
 * the object's own bytes as its address: this program exited 1 on pe64, ELF
 * x86_64 and ELF aarch64, and a plain `return x;` of an archive's
 * `int x = 42;` crashed (access violation on pe64, SIGSEGV on both ELF
 * targets).
 *
 * 30 + 5 + 3 + 4 = 42 only if each read reaches its object; the two address
 * checks fail with their own codes if the slot's address and the object's
 * address disagree. */
extern int  dss_lib_int;
extern int  dss_lib_array[3];
extern int *dss_lib_ptr;

static int *const via_init = &dss_lib_array[2];

int main(void) {
    if (dss_lib_ptr != &dss_lib_array[1]) return 1;  /* the archive's pointer == ours */
    if (via_init != &dss_lib_array[2]) return 2;     /* our initializer == our code's address */
    return dss_lib_int + *dss_lib_ptr + *via_init + dss_lib_array[0];
}
