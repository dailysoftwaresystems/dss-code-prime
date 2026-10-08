/* The product of a relocatable link is an object a DSS link reads (see expected.json).
   `dss_unwind_shared` lives in Apple's `ld -r` product of a two-function unit and returns 7 (its
   own `dss_unwind_first` returns 3, plus 4). The 35 keeps the answer apart from the exit code of a
   program that did nothing. */
extern int dss_unwind_shared(void);

int main(void) { return dss_unwind_shared() + 35; }
