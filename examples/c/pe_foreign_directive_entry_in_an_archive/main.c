/* A linked object's `/ENTRY:` is a REFERENCE to the function it names (see expected.json). The
   REFERENCE-built object states `/ENTRY:dss_archived_entry` and does not define it; only the
   archive's one member does, and nothing else of the link refers to it. The process starts there,
   with no C runtime startup, and exits with the 42 it returns. This `main` is the witness that it
   never ran: it would exit 7. */
extern int dss_entry_ref_marker(void);

int main(void) { return dss_entry_ref_marker() + 4; }
