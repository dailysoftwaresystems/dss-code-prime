/* FC17.9(h) C23 #embed (D-PP-EMBED) — end-to-end runtime witness, RUN on the
 * baseline (debug) AND the `release` shippedPipeline arm.
 *
 * `#embed "answer.bin"` splices the resource's 5 bytes {42, 13, 10, 26, 0} as a
 * comma-separated list of decimal int constants into the brace initializer of a
 * `static const unsigned char blob[]`. The example SELF-WITNESSES two things a
 * green build alone can't: (a) BINARY-mode read — blob[1]==13 (0x0D CR),
 * blob[2]==10 (0x0A LF), blob[3]==26 (0x1A SUB) survive verbatim (a text-mode
 * read would mangle CR/LF on Windows); (b) git BYTE-INTEGRITY — the same
 * blob[1]==13 check reds if the .gitattributes binary override for example
 * .bin resources is lost and a fresh checkout CRLF-normalizes the resource.
 *
 * ANTI-FOLD: g_i is a mutable global 0 (runtime-opaque), so no pass can
 * const-fold `blob[g_i]` to `return 42` — the release arm proves the embedded
 * array is materialized and indexed at runtime.
 *
 * exit = blob[0] = 42 (iff sizeof(blob) == 5 and blob[1..4] == {13, 10, 26, 0}).
 */

/* Mutable global 0 = a runtime-opaque index (defeats const-folding). */
int g_i = 0;

int main(void) {
    static const unsigned char blob[] = {
#embed "answer.bin"
    };

    /* Byte-exact self-witness: the resource is exactly {42, 13, 10, 26, 0}. */
    if (sizeof(blob) != 5) return 1;
    if (blob[1] != 13) return 2;   /* 0x0D CR — binary-mode + git-integrity canary */
    if (blob[2] != 10) return 3;   /* 0x0A LF */
    if (blob[3] != 26) return 4;   /* 0x1A SUB */
    if (blob[4] != 0)  return 5;   /* 0x00 NUL */

    return blob[g_i];              /* blob[0] = 42 */
}
