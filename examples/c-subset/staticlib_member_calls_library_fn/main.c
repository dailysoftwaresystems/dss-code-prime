// The client. Deliberately thin -- one extern declaration and one call, no
// locals and no arithmetic -- so it offers the optimizer nothing of its own
// and the `release` arm's difference is attributable to the ARCHIVE MEMBER.
int dss_lib_answer(void);

int main(void) { return dss_lib_answer(); }
