/* A linked object's directives that a DSS image cannot carry are refused by name (see
   expected.json): the REFERENCE-built member states `/MERGE:.rdata=.text` and a
   `/manifestdependency:` on the common controls. */
extern int dss_unhonourable_member(void);

int main(void) { return dss_unhonourable_member(); }
