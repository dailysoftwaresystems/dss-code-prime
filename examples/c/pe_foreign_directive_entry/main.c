/* A linked object's `/ENTRY:` names the function the process starts at (see expected.json). The
   REFERENCE-built member states `/ENTRY:dss_member_entry` and defines it to return 42; the process
   starts there, with no C runtime startup, and exits with what it returns. This `main` is the
   witness that it never ran: it would exit 7. */
extern int dss_member_entry(void);

int main(void) { return dss_member_entry() - 35; }
