/* AP6 — the STATICLIB half. Compiled into an ARCHIVE of its own (never merged
 * into the consumer's compilation the way a `module` dependency is), then
 * pulled back in by the consumer's static link.
 *
 * The consumer supplies the runtime operand and this file supplies the shape;
 * neither contains the literal 42, so the exit code cannot survive the
 * dependency being dropped. */
int dss_fold_twice(int v) {
    return v + v + 2;
}
