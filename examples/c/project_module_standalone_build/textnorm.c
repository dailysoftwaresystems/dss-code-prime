/* AP6 / plan 06 §5.1 B.13.3 — the payload of a STANDALONE `module` build.
 *
 * THERE IS NO `main` HERE, AND THE ABSENCE IS THE POINT rather than an
 * omission. A `module` is a LIBRARY: nothing in it is an entry point and
 * nothing about it is executable. Before B.13.3 a project declaring
 * `artifactProfile: "module"` could not be built at all — the AP3
 * profile-vs-format gate refused every target — so a module's own type
 * errors were undiscoverable until some CONSUMER imported it, and they then
 * surfaced inside the consumer's build, blaming the wrong project.
 *
 * These four functions exist so the standalone build has REAL WORK to parse,
 * type-check, lower and emit. A module source that was empty (or a single
 * `return 0;`) would witness the driver's archive plumbing and say nothing
 * about the front end reaching the code — which is the half of the capability
 * the operator's question was actually about ("how will it be tested inside
 * its own root project on its own?"). Between them they exercise parameters,
 * locals, `while` loops, `if`/early return, char literals, pointer indexing,
 * short-circuit `||`, and — via `dss_ascii_equal_fold` calling
 * `dss_ascii_lower` — an INTRA-MODULE call, so the emitted archive member
 * carries a real intra-section relocation rather than four leaf functions.
 *
 * NO `#include`, DELIBERATELY. This file is compiled for pe64-x86_64,
 * elf64-x86_64, elf64-aarch64 AND macho64-arm64 on EVERY host that runs the
 * corpus, so a header would bind the build to one platform's SDK being
 * installed on all four legs. The sibling `foldlib/fold.c` takes the same
 * care for the same reason. Everything below is freestanding C.
 *
 * The API is deliberately plausible — bounded, NUL-safe ASCII helpers are
 * exactly the shape a real project factors into a shared module — because an
 * example whose source is obvious filler invites the next reader to delete it.
 */

/* ASCII-fold one character. Table-free and locale-free on purpose: the fold
 * must be identical on all four targets, and `tolower` would drag in a header
 * plus a libc whose behaviour is locale-dependent. */
int dss_ascii_lower(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

int dss_ascii_is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Length of `s`, capped at `limit`. Bounded rather than `strlen`-shaped
 * because a module is consumed by callers it cannot see: a buffer that is not
 * NUL-terminated must stop the walk, not run off the end. */
int dss_bounded_length(const char *s, int limit) {
    int n = 0;
    while (n < limit && s[n] != '\0') {
        n = n + 1;
    }
    return n;
}

/* Case-insensitive ASCII compare over at most `limit` characters. Returns 1
 * when the two agree (or both terminate first), 0 on the first difference.
 * Calls `dss_ascii_lower` above — the intra-module call this file wants. */
int dss_ascii_equal_fold(const char *a, const char *b, int limit) {
    int i = 0;
    while (i < limit) {
        int ca = dss_ascii_lower(a[i]);
        int cb = dss_ascii_lower(b[i]);
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
        i = i + 1;
    }
    return 1;
}
