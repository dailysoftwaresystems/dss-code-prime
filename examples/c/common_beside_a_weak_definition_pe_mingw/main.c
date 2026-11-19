/* A COMMON beside a definition of its name that may be stated more than once (see expected.json).
   `dss_read_tentative` lives in the reference-built object that holds `dss_tentative` as a common
   and returns what the program's ONE `dss_tentative` holds: 0 where the common stands, 5 where the
   other object's definition replaces it. The 37 keeps either answer apart from the exit code of a
   program that did nothing. */
extern int dss_read_tentative(void);

int main(void) { return dss_read_tentative() + 37; }
