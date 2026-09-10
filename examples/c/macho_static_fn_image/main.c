/* D-LINK-MACHO-IMAGE-STATIC-FN-EMITTED-N-EXT runtime witness.
 *
 * A Mach-O IMAGE whose nlist carries a `static` function beside an exported
 * one. Both image nlist builders used to stamp `N_SECT|N_EXT` on EVERY defined
 * function, so `static_helper` reached the image under its real name with the
 * EXTERNAL bit set and LC_DYSYMTAB published `nlocalsym 0`; Apple's ld64
 * emits it `N_SECT` with no `N_EXT` as the FIRST nlist record, under
 * `ilocalsym 0 nlocalsym 1 iextdefsym 1` (measured 2026-09-04 on Apple
 * Silicon, Apple clang 21.0.0 / ld-1267). The structural pin lives in
 * tests/link/test_macho_image_symtab_bands.cpp; THIS example is the RUN
 * witness: the image with a local-band symbol still loads, passes its ad-hoc
 * signature, and exits with the asserted code on the darwin leg, and it BUILDS
 * on every host.
 *
 * The two statics are reached through a const function-pointer table indexed
 * by a RUNTIME value (argc - 1 == 0 with no arguments), so the index is not a
 * constant any pass can see through — the shape is chosen to keep them
 * address-taken through the release arm rather than let a dead-code pass drop
 * them. That is the INTENT of the shape, not a measurement: what this example
 * asserts is the exit code on both arms; the nlist itself is pinned by
 * tests/link/test_macho_image_symtab_bands.cpp, which compiles the default
 * pipeline. `_main` and
 * `global_helper` are the externally-defined band; the linker-injected entry
 * trampoline is the second Local (it has no declared name).
 *
 * Exit arithmetic: table[argc - 1](40) = table[0](40) = static_helper(40)
 *                  = 40 + 2 = 42. */
static int static_helper(int v) { return v + 2; }
static int other_static(int v)  { return v - 1; }

int (*const table[2])(int) = { static_helper, other_static };

int global_helper(int v, int which) { return table[which & 1](v); }

int main(int argc, char** argv) {
    (void)argv;
    return global_helper(40, argc - 1);
}
