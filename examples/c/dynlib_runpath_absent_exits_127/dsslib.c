/* D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH -- the CONTROL's library half.
 * Built into a dynamic library BESIDE the executable, exactly as in the
 * subject example `dynlib_runpath_origin`. The library is present and correct;
 * what the executable lacks is any record of where it is. */
int dss_runpath_answer(void) {
    return 42;
}
