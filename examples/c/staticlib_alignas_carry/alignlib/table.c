/* THE ARCHIVE MEMBER — the half that DECLARES an alignment.
 *
 * D-FORMAT-MACHO-SECTION-ALIGN-EMITTED-RAW-NOT-LOG2 (the read-side half).
 *
 * `_Alignas(64)` makes the writer stamp this section's declared alignment into
 * the object it archives — `IMAGE_SCN_ALIGN_64BYTES` in a COFF `.obj`,
 * `sh_addralign = 64` in an ELF one, `section_64.align = 6` in a Mach-O one.
 * The consumer static-links that archive, so an OBJECT READER has to
 * reconstruct the field before the merge can honour it.
 *
 * The probe RETURNS the address verdict rather than asserting it, so the
 * consumer's exit code is a function of where the linker actually put these
 * bytes. */
_Alignas(64) const int dss_align_head[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
_Alignas(64) const int dss_align_tail[8]  = { 10, 11, 12, 13, 14, 15, 16, 17 };

int dss_align_probe(int n) {
    unsigned long long addr = (unsigned long long)(unsigned long)&dss_align_tail[0];
    if ((addr & 63ull) != 0ull) return 0;      /* under-aligned -> not 42 */
    return dss_align_tail[0] + dss_align_head[n];
}
