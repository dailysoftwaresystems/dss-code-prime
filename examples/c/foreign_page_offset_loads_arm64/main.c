/* A FOREIGN OBJECT'S PAGE-OFFSET LOADS, ONE PER ACCESS SIZE
 * (D-LK-MACHO-ARM64-PAGEOFF12-LOAD-PATCHED-AS-AN-ADD, P68 round 9).
 *
 * `get` is not compiled by DSS: it comes from the reference-built archive the
 * manifest names, from this source (clang 18 -O2 for Mach-O arm64, aarch64 gcc
 * 13 -O2 -fno-section-anchors for ELF):
 *
 *   typedef int v4 __attribute__((vector_size(16)));
 *   long long pad[3] = {100, 200, 300};
 *   signed char g8 = 5;  short g16 = 7;  int g32 = 20;  long long g64 = 6;
 *   double gd = 2.0;     volatile v4 gv = {0, 0, 0, 2};
 *   int get(void) {
 *       v4 t = gv;
 *       return g8 + g16 + g32 + (int)(g64 >> 1) * 2 + (int)gd + t[3];
 *   }
 *
 * Each global is reached with `adrp` + a LOAD whose 12-bit offset field the
 * link fills in: `ldrsb`, `ldrsh`, `ldr w`, `ldr d` and `ldr q` (and `ldr x`
 * on ELF). That field is SCALED by the access size, so the link must divide the
 * page offset by it. 5 + 7 + 20 + 6 + 2 + 2 = 42 only if every load reads its
 * own object; `pad` keeps each of them off page offset zero, where an
 * unscaled field would happen to be right.
 */
int get(void);

int main(void) { return get(); }
