/* SQLite testfixture-link arc (TF-C39, 2026-07-21) — shipped <zlib.h> witness
 * (D-FFI-ZLIB-DESCRIPTOR). `#include <zlib.h>` resolves to shippedLibs/zlib.json:
 * the z_stream struct (14 fields / 112 bytes, offsetof-verified vs zlib.h) + the
 * raw-deflate API sqlite's ext/misc/zipfile.c uses (deflate/inflate/crc32 +
 * the deflateInit2/inflateInit2 function-macros over deflateInit2_/inflateInit2_).
 *
 * A REAL deflate -> inflate ROUND-TRIP at runtime (linked against libz.so.1):
 * compress a runtime-filled buffer with raw deflate (windowBits -15, exactly as
 * zipfile.c does), decompress it, and confirm the bytes round-tripped AND the
 * crc32 of the original equals the crc32 of the decompressed copy. Returns 42 iff
 * the round-trip is byte-exact. The compression/decompression/crc are opaque libz
 * calls the optimizer cannot fold, so the release arm witnesses the optimizer over
 * real work (not a constant-folded no-op).
 *
 * Why this is a STRONG layout witness (behavioral red-on-disable):
 *   - deflateInit2_/inflateInit2_ receive `(int)sizeof(z_stream)` from the init
 *     macros and REJECT the call (return Z_VERSION_ERROR, != Z_OK) unless it equals
 *     the linked libz's own sizeof(z_stream) — so a wrong z_stream size in the
 *     descriptor makes this example return 1, not 42.
 *   - deflate()/inflate() read next_in/avail_in and write next_out/avail_out/
 *     total_out at the ABI offsets — a wrong field offset corrupts the stream and
 *     the round-trip fails (returns 6/7/8), never 42.
 * RED-ON-DISABLE: delete zlib.json (or drop a symbol) -> undeclared -> compile
 * fails; a broken/unexported import would fail loud at load (127), never silently.
 */
#include <zlib.h>
#include <string.h>   /* memset */

int main(void) {
    unsigned char input[256];
    unsigned char compressed[512];
    unsigned char output[256];
    int i;

    /* Fill at runtime (runs of 4 → compressible, so deflate does real Huffman/LZ
     * work rather than a stored block). */
    for (i = 0; i < 256; i++) {
        input[i] = (unsigned char)((i >> 2) & 0xFF);
    }
    unsigned long origCrc = crc32(0, input, 256);

    /* ---- raw deflate ---- */
    z_stream ds;
    memset(&ds, 0, sizeof(ds));          /* zalloc/zfree/opaque = 0 → default allocator; needs sizeof==112 */
    ds.next_in  = input;                 /* @0  */
    ds.avail_in = 256;                   /* @8  */
    ds.next_out = compressed;            /* @24 */
    ds.avail_out = 512;                  /* @32 */
    if (deflateInit2(&ds, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return 1;                        /* wrong sizeof(z_stream) trips this */
    }
    if (deflateBound(&ds, 256) == 0) {   /* exercises deflateBound (versioned export) */
        deflateEnd(&ds);
        return 2;
    }
    if (deflate(&ds, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&ds);
        return 3;
    }
    unsigned long compressedLen = ds.total_out;   /* @40 */
    deflateEnd(&ds);
    if (compressedLen == 0 || compressedLen > 512) {
        return 4;
    }

    /* ---- raw inflate ---- */
    z_stream is;
    memset(&is, 0, sizeof(is));
    is.next_in  = compressed;
    is.avail_in = (unsigned int)compressedLen;
    is.next_out = output;
    is.avail_out = 256;
    if (inflateInit2(&is, -15) != Z_OK) {
        return 5;
    }
    if (inflate(&is, Z_NO_FLUSH) != Z_STREAM_END) {
        inflateEnd(&is);
        return 6;
    }
    unsigned long outLen = is.total_out;
    inflateEnd(&is);
    if (outLen != 256) {
        return 7;
    }

    /* ---- verify the round-trip is byte-exact ---- */
    for (i = 0; i < 256; i++) {
        if (output[i] != input[i]) {
            return 8;
        }
    }
    if (crc32(0, output, 256) != origCrc) {
        return 9;
    }

    return 42;
}
