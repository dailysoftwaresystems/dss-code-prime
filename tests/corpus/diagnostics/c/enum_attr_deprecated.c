// D-CSUBSET-ATTRIBUTE-TYPE-POSITION: the AFTER-KEYWORD attribute slot on an
// `enum`, in both spellings. C23 6.7.3.1 puts a tag's attribute there, and gcc
// 13.3.0 and clang 19.1.1 both warn at every USE (MEASURED 2026-08-25). Before
// P34 `enumSpec` was the only composite row with no such slot, so the C23 form
// was error[P0009] and the GNU form error[P0001] — a PARSE failure, not a
// semantic one. Each tag is used twice (a type position and a tag reference) so
// a slot that parses but drops the fact still goes red.
enum [[deprecated]] E1 { A1 = 1 };
enum __attribute__((deprecated)) E2 { A2 = 2 };
int main(void) {
    enum E1 e1 = A1;
    enum E2 e2 = A2;
    return (int)e1 + (int)e2;
}
