// P63 (D-CSUBSET-ALIGNMENT-CEILING-REFUSES-WHAT-TWO-REFERENCES-RUN): the NEGATIVE
// half. A statically allocated object faces a SECOND ceiling, narrower than the
// target's declared `maxRequestedAlignment` and genuinely per-FORMAT: what the
// image can PLACE it at. A PE image's sections begin at multiples of the format
// document's declared `SectionAlignment` (4096 on the shipped `pe64-*` documents)
// and nothing stronger, so an 8192-aligned static cannot be guaranteed.
//
// ⚠⚠ WITHOUT THIS GATE THE BUILD IS CLEAN AND THE OBJECT IS MISPLACED. ✔MEASURED
// 2026-09-07 with the ceiling raised and the gate absent: this program's four
// over-aligned statics built rc 0 and the FIRST one failed its own `% 8192` check
// at run time. A clean build placing an over-aligned object misaligned is a silent
// miscompile — the one outcome this project ranks below every diagnostic.
//
// ★ THE UNION AGREES THERE IS A CEILING HERE, and says so in the references' own
// words. ✔MEASURED, each PE reference probed SEPARATELY: mingw-w64 gcc 13.2.0
// refuses a static above 8192 — "alignment of 'g' is greater than maximum object
// file alignment 8192" — and MSVC 19.51 refuses `__declspec(align(16384))` with
// `error C2345`. ★★ THE CONTROLS ARE WHAT MAKE IT A STATEMENT ABOUT STORAGE: the
// SAME compiler at the SAME value BUILDS AND RUNS the request on a TYPE and on an
// AUTOMATIC object. That is why this ceiling lives in the format writer and must
// never move into the semantic ladder — there it would refuse `aligned(65536)`
// types that every reference runs.
//
// ⓘ THE BOUND IS THE DOCUMENT'S OWN NUMBER, not a hardcoded platform constant: a
// `.format.json` declaring a larger `optionalHeader.sectionAlignment` raises the
// ceiling with no code change, which is the lever `link.exe` spells `/ALIGN`.
// ⓘ PE ONLY, deliberately. ✔MEASURED on the same program: the ELF exec target
// places all four objects correctly at 8192, 16384 and 65536 (run under WSL, exit
// 42), so an ELF gate would refuse what DSS demonstrably gets right.

struct __attribute__((aligned(8192))) page2 { char c; };

static char   filler0[7] = {1};
static struct page2 g0;
static char   filler1[13] = {2};
static struct page2 g1;

int main(int argc, char **argv) {
    (void)argv; (void)filler0; (void)filler1;
    unsigned long long const a0 = (unsigned long long)(void *)&g0;
    unsigned long long const a1 = (unsigned long long)(void *)&g1;
    if (a0 % 8192ull) return 50;
    if (a1 % 8192ull) return 51;
    g0.c = (char)argc;
    return (int)g0.c * 42;
}
