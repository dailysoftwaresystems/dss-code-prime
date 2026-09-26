/* The DATA a static archive exports (D-LK-SIBLING-DATA-IMPORT-SLOT-BOUND-TO-THE-OBJECT,
 * P68 round 9, routed from lane lm): an int, an array, and a pointer whose own
 * value is an address the archive's initializer computes. Built into a static
 * library by the manifest's `dependsOn`; `main.c` reads all three. */
int  dss_lib_int      = 30;
int  dss_lib_array[3] = {4, 5, 3};
int *dss_lib_ptr      = &dss_lib_array[1];
