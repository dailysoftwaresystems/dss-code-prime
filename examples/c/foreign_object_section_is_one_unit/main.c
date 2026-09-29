/* The DSS half of the foreign-object unit witness: every store and load that decides the exit code lives
   in the REFERENCE-built archive (see expected.json), so this file only calls into it. */
extern void dss_unit_set(void);
extern int dss_unit_get(void);

int main(void) {
    dss_unit_set();
    return dss_unit_get();
}
