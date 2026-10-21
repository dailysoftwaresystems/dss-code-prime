/* A linked object's directive that no reference linker honours is warned and ignored (see
   expected.json): the REFERENCE-built member states `/LARGEADDRESSAWARE`, and the program links
   and runs. */
extern int dss_warned_member(void);

int main(void) { return dss_warned_member(); }
