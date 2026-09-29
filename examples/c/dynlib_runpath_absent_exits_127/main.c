/* D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH -- the CONTROL's client half.
 * The same client as `dynlib_runpath_origin`, linked against the same library
 * built beside it, but with NO runpath request and NO loader variable. The
 * image records `dsslib.so` in DT_NEEDED by bare name and nothing says where it
 * lives, so the loader refuses to start the program: exit 127. If this ever
 * returns 42, the library was found by something other than the image (an
 * inherited LD_LIBRARY_PATH, an installed copy) and the subject example next
 * door would no longer prove what it claims. */
extern int dss_runpath_answer(void);

int main(void) {
    return dss_runpath_answer();
}
