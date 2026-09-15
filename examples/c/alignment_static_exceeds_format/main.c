// D-CSUBSET-THREAD-LOCAL-PE-OVERALIGN (static twin): the NEGATIVE half. A
// statically allocated object faces a SECOND ceiling, narrower than the target's
// declared `maxRequestedAlignment` and genuinely per-FORMAT: what the container
// can ENCODE. PE/COFF spells an object's alignment as a FOUR-BIT
// IMAGE_SCN_ALIGN_* field, so 8192 (IMAGE_SCN_ALIGN_8192BYTES) is the strictest
// request expressible and 16384 is the first that is not.
//
// ⚠⚠ THIS EXAMPLE USED TO SIT AT 8192, AND THAT WAS A DIVERGENCE PINNED AS THE
// CONTRACT. ✔MEASURED (P64), each PE reference probed SEPARATELY, BUILD **and**
// RUN with the runtime address asserted on a multi-object subject: mingw-w64 gcc
// 13.2.0 and MSVC 19.51 BOTH return 42 at 8192 (MSVC with `/link /ALIGN:8192`),
// so refusing it made DSS reject what two working references run. The pe64
// writer now PLACES an 8192 static -- `examples/c/alignment_overaligned_static_
// placed` is the positive witness -- and this example moved up one step, to the
// value both references refuse BY NAME. ✔The messages are quoted from a run on
// THIS FILE, not paraphrased: gcc gives the TYPE-decorated form here, "error:
// alignment of 'g1' is greater than maximum object file alignment 8192" (the
// VARIABLE-decorated spelling gives "requested alignment '16384' exceeds object
// file maximum 8192" — two messages, one number), and MSVC 19.51, probed in its
// own `__declspec(align(16384))` spelling because it casts no vote in a foreign
// one, gives `error C2345: align(16384): illegal alignment value`.
//
// ⚠⚠ WITHOUT A GATE HERE THE BUILD IS CLEAN AND THE OBJECT IS MISPLACED.
// ✔MEASURED in P63 with the ceiling raised and no gate: four over-aligned
// statics built rc 0 and the FIRST one failed its own `%` check at run time. A
// clean build placing an over-aligned object misaligned is a silent miscompile —
// the one outcome this project ranks below every diagnostic.
//
// ★★ THE CONTROLS ARE WHAT MAKE IT A STATEMENT ABOUT STORAGE: the SAME compiler
// at the SAME value BUILDS AND RUNS the request on a TYPE and on an AUTOMATIC
// object. That is why this ceiling lives in the format writer and must never
// move into the semantic ladder — there it would refuse `aligned(65536)` types
// that every reference runs.
//
// ⓘ PE ONLY, deliberately. ✔MEASURED on the same program: the ELF exec target
// places the objects correctly at 8192, 16384 and 65536 (run under WSL, exit
// 42), so an ELF gate would refuse what DSS demonstrably gets right.

struct __attribute__((aligned(16384))) page4 { char c; };

static char   filler0[7] = {1};
static struct page4 g0;
static char   filler1[13] = {2};
static struct page4 g1;

int main(int argc, char **argv) {
    (void)argv; (void)filler0; (void)filler1;
    unsigned long long const a0 = (unsigned long long)(void *)&g0;
    unsigned long long const a1 = (unsigned long long)(void *)&g1;
    if (a0 % 16384ull) return 50;
    if (a1 % 16384ull) return 51;
    g0.c = (char)argc;
    return (int)g0.c * 42;
}
