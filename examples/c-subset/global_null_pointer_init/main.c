/* c80 Family B repro — TOP-LEVEL scalar pointer global initialized to the
 * NULL POINTER CONSTANT.
 *
 * sqlite3.c:
 *   27703:  static sqlite3_vfs * SQLITE_WSD vfsList = 0;
 *   41195:  static sqlite3_mutex *unixBigLock = 0;
 *   41646:  static unixInodeInfo *inodeList = 0;
 *   72148:  static BtShared *SQLITE_WSD sqlite3SharedCacheList = 0;
 *   186795: SQLITE_API char *sqlite3_temp_directory = 0;
 *   186804: SQLITE_API char *sqlite3_data_directory = 0;
 *
 * `T* g = 0;` is the standard null pointer constant (C 6.3.2.3p3) — a
 * compile-time zero, identical bytes to the no-initializer `.bss` form.
 * Pre-fix: const-eval refuses cast-to-pointer, and c67's
 * tryClassifyNullPointerConst was wired ONLY into the AGGREGATE member loop
 * (`struct {void* p;} g = {0}` folded while bare `void* g = 0;` did not)
 * -> runtimeInit -> K_NoMatchingObjectFormat fail-loud.
 *
 * gcc -O0/-O2 exits 42.
 */
static int *vfsList = 0;              /* static, mutable pointer */
char *tempDirectory = 0;              /* extern-visible, mutable  */
static void (*fnPtr)(void) = 0;       /* function-pointer global  */

static int cell = 7;

static void bump(void) { cell = 9; }

int main(void) {
    if (vfsList != 0) return 1;
    if (tempDirectory != 0) return 2;
    if (fnPtr != 0) return 3;
    /* the globals stay ordinary mutable storage */
    vfsList = &cell;
    if (*vfsList != 7) return 4;
    fnPtr = bump;
    fnPtr();
    if (cell != 9) return 5;
    return 42;
}
