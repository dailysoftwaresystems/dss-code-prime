/* THE CONSUMER — the half that OBSERVES whether the declared alignment
 * survived the archive round trip.
 *
 * D-FORMAT-MACHO-SECTION-ALIGN-EMITTED-RAW-NOT-LOG2 (the read-side half).
 *
 * ★★ `dss_consumer_pad` IS FIVE BYTES, AND WITHOUT IT THIS EXAMPLE MEASURES
 * NOTHING. That is a measurement, not a hunch: three earlier shapes of this
 * file were built with the fix and with it deleted and exited IDENTICALLY.
 * The reason is worth writing down, because it is the trap any future version
 * of this example will fall into --
 *
 *   the reader slices atoms by their VALUE in the section, so the producer's
 *   inter-item PADDING is absorbed into the preceding atom's extent. Relative
 *   offsets inside one module therefore come back correct whether or not the
 *   alignment field survived, and an over-aligned object that is FIRST in its
 *   section is aligned by construction.
 *
 * What the field actually decides is where the member's whole block LANDS when
 * something else already occupies the section. So the consumer contributes its
 * own five bytes of rodata — a non-multiple of 64 — and the archive member's
 * block has to be pushed to the next 64-byte boundary. That push is driven by
 * the block's `maxAlign`, which is exactly what the reader supplies.
 *
 * ✔MEASURED 2026-08-27 on the PE leg, by rebuilding dsscp with
 * `di.alignment = alignFromCharacteristics(sec.chars)` deleted and rebuilding
 * this project with the mutant compiler: `&dss_align_tail` mod 256 is 128
 * (64-aligned) with the reader's carry and 69 without it — misaligned by
 * exactly these five bytes.
 *
 * NON-FOLDING: `argc` is OS-supplied and the address the probe tests is
 * assigned by the linker. Neither source file contains the literal 42.
 *   argc == 1 -> acc == 10, probe(0) == 10 + 1 == 11, pad[0] == 8,
 *                11*4 - 10 + 8 == 42. Under-aligned -> 0*4 - 10 + 8 == -2.
 *
 * ★ THE LOOP AND THE `static` HELPER ARE WHAT MAKE THE `release` ARM BITE, for
 * the reason `project_dependson_staticlib_artifact_link` measured and wrote
 * down: with only an external call to optimize, the two configs emit a
 * byte-identical image and the arm asserts that a no-op stayed a no-op.
 *
 * RED-ON-DISABLE: delete `di.alignment = alignFromCharacteristics(sec.chars)`
 * from the named-atom `AssembledData` site in
 * `src/link/format/coff_object_reader.cpp`. */
extern int dss_align_probe(int n);

const char dss_consumer_pad[5] = { 8, 0, 0, 0, 0 };

static int lift(int v) { return v * 5; }

int main(int argc, char **argv) {
    (void)argv;
    int acc = 0;
    for (int i = 0; i < 2; ++i) acc = acc + lift(argc);   /* argc==1 -> 10 */
    return dss_align_probe(argc - 1) * 4 - acc + dss_consumer_pad[argc - 1];
}
