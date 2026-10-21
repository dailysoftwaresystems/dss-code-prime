/* DSS's OWN tentative definition beside a definition of its name that may be stated more than once
   (see expected.json). `dss_tentative` is a tentative definition of this unit, which DSS writes as a
   COMMON where its format document states commons; the program's ONE `dss_tentative` holds 0 where
   the common stands and 5 where the other object's definition replaces it. The 37 keeps either
   answer apart from the exit code of a program that did nothing. */
int dss_tentative;

int main(void) { return dss_tentative + 37; }
