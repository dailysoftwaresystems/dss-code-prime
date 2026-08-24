/* AP6 — the MODULE half. This file is never named by the consumer's
 * `.dss-project.json`; it arrives because the consumer declares
 * `"dependsOn": [{"path": "scalemod"}]` and THIS directory's own
 * `.dss-project.json` declares `"artifactProfile": "module"` →
 * `DependencyComposition::SourceMerge` → its expanded `sources[]` join the
 * consumer's compilation.
 *
 * The `sources: ["scale.c"]` entry next door is RELATIVE and re-bases against
 * THIS directory, never the process cwd (plan 06 §5.1 B.3 + M4(a)) — the
 * consumer's tree has no `scale.c`, so a resolver that re-based only globs
 * would fail loud here rather than silently reading the wrong file.
 *
 * The multiplier is the only place `7` appears in the example; the consumer
 * supplies the runtime operand and neither file contains the literal 42. */
int dss_scale_by_seven(int v) {
    return v * 7;
}
