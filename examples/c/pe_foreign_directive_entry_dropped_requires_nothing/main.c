/* Only the `/ENTRY:` that STANDS is a reference (see expected.json). Two REFERENCE-built objects
   each state an entry: the first names `dss_member_entry` and defines it to return 42; the second
   names `dss_archived_entry`, which NOTHING in this link defines. link.exe takes the first, drops
   the second with a warning and requires nothing for it: the process starts at `dss_member_entry`,
   with no C runtime startup, and exits with the 42 it returns. This `main` is the witness that it
   never ran: it would exit 7. */
extern int dss_member_entry(void);
extern int dss_entry_ref_marker(void);

int main(void) { return dss_member_entry() - dss_entry_ref_marker() - 32; }
