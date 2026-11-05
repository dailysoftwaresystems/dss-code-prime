/* A member the archive search pulls names no entry (see expected.json). The REFERENCE-built
   member states `/ENTRY:dss_member_entry` and defines it to return 42; link.exe fixes the image's
   entry before it searches an archive, so the process starts in the C runtime's startup and this
   `main` runs, exiting 7. */
extern int dss_member_entry(void);

int main(void) { return dss_member_entry() - 35; }
