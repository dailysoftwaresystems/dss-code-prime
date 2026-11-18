/* A linked object's directive that a DSS image cannot carry is refused by name (see
   expected.json): the REFERENCE-built member states `/MERGE:.rdata=.text`, refused, and a
   `/manifestdependency:` on the common controls, which the image embeds in its manifest. */
extern int dss_unhonourable_member(void);

int main(void) { return dss_unhonourable_member(); }
