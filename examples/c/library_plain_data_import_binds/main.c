/* D-FFI-LIBRARY-TLS-EXPORT-BINDS-AS-PLAIN-DATA — the ACCEPTING half.
 *
 * The same committed library the refusal example resolves against, and the
 * same door (`--resolve-library`). What differs is WHICH of its two exports is
 * named: `dss_lib_plain_counter` is ordinary data (STT_OBJECT, .data), so the
 * storage durations agree and the import must BIND.
 *
 * ★★★ THE GUARD IS ONLY A GUARD IF THIS STILL PASSES. A rule keyed on "this
 * library exports a thread-local" rather than on "THIS NAME is a thread-local"
 * would refuse every import from an image that happens to contain one — and
 * .tdata is common enough in real libraries that such a rule would be
 * unusable. This entry is what makes that failure mode visible, and it reaches
 * the refusal's own call site: the library IS TLS-carrying, the binder DOES
 * consult its export table for this name, and the answer is "ordinary".
 *
 * The arithmetic exists so the release arm has something to transform; the
 * value itself is the library's (11), never asserted here, because nothing is
 * spawned — see the manifest's note on why these arms are build-only.
 */

extern int dss_lib_plain_counter;

static int fold(int seed) {
    int acc = seed;
    for (int i = 0; i < 4; ++i) {
        acc = acc * 3 + (i ^ seed);
    }
    return acc;
}

int main(void) {
    return fold(dss_lib_plain_counter) & 0x7F;
}
