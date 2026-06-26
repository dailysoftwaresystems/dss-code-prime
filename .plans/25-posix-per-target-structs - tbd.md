# Plan 25 — POSIX headers via per-target struct layouts (the SQLite os_unix surface)

§0 STATUS: ✅ MECHANISM DONE (p18 Cluster G c13, 2026-06-26, 422/422 ctest Release) — `variants` decode/select + `activeTarget` thread + `sys/stat.json` struct stat (x86_64-elf 144B / arm64-elf 128B) + corpus + pins; closure gates 1-9 met. ⏳ c14 IN PROGRESS: errno (c14a) + sys/mman + unistd (c14b) DONE; the **variadic-externs FFI extension** (c14c — `parseTypeFromText` accepts `...` → variadic FnSig + the emitter round-trips it; reuses FC12 variadic-call codegen) DONE → unblocks `open`/`fcntl`. fcntl (O_/F_ + struct flock + variadic open/fcntl, c14d) + sys/stat completion (S_ + S_IS* + fstat/lstat/mkdir + st_*time macros, c14d) DONE — **5/6 linux POSIX headers done**. pthread (c14e) DONE — `pthread_mutex_t` shipped as a FLAT MAX-size (48B = the arm64 size) 8-aligned opaque typedef (`arr<i64,6>`), which SIDESTEPS the per-arch-typedef issue: x86-64 over-allocates 8B, harmless because libc touches only its own 40 and SQLite treats the mutex as opaque. **ALL 6 linux POSIX headers DONE** (errno/sys.mman/unistd/fcntl/sys.stat/pthread) + the 2 engine extensions (per-target structs c13, variadic externs c14c). ✅ c15 macOS-enabling (2026-06-26, user: "why macos deferred? you have the docs"): **c15a** = the per-target `variants` axis EXTENDED from structs to **constants + typedefs + macros** (`matchVariantWhen` shared helper; macros are format-only — arch isn't threaded into preprocess, c9 build-key avoidance; 3 new ambiguous-fail-loud diagnostics 0x501F-0x5021 + kUnsuppressableCodes 64→67; committed 187a54a). **c15b** = the proof — `errno.json` now serves BOTH formats (the `errno` lvalue macro selects `(*__error())` macho / `(*__errno_location())` elf; both accessor externs declared, only the referenced one links; the divergent E* carry per-format `variants` — EAGAIN 35/11, ETIMEDOUT 60/110, ENOTSUP 45/95, EOPNOTSUPP 102/95, EOVERFLOW 84/75, ENOLCK 77/37; corpus `shipped_errno_macho` runs on the macos-latest leg → exit 42; committed 73c5763, 429/429 ctest). ✅ c15c DONE (2026-06-26, 433/433 ctest): **the remaining 5 macOS headers** — sys.mman/unistd/fcntl/sys.stat/pthread macho variants, ALL value-verified against apple-oss-distributions/xnu + libpthread (an independent agent fetched the real headers). The macho STRUCT layouts: `struct stat` 144B Darwin (the SAME SIZE as x86-64-elf but reordered — the corpus pins a FIELD OFFSET st_size@96, not sizeof — the c13 same-size-swap trap), `struct flock` 24B (reordered, off_t first), pthread_mutex_t 64B / mutexattr 16B (typedef variants — the FIRST real consumer of the c15a typedef-variant mechanism). The O_*/F_* divergences (O_CREAT 512, F_RDLCK 1/F_WRLCK 3, …) + PTHREAD_MUTEX_RECURSIVE 2 + the PTHREAD_MUTEX_INITIALIZER Darwin __sig magic 0x32AAABA7. mremap/MREMAP_MAYMOVE/O_TMPFILE elf-only (fail-loud on macho); fdatasync/fallocate unreferenced-externs. 4 corpus witnesses (runOn darwin, macos-latest leg). The pr-review (value-verified vs xnu/libpthread) CAUGHT a hex→decimal typo in the __sig magic (859068327→850045863) → folded + a static-init witness added. **ALL 6 POSIX headers now serve BOTH formats (elf+macho); macOS POSIX matches linux completeness.** ✅ c15d (dbe8b61) = the CI-red repair: DSS imports EVERY declared shipped extern, so errno's two per-format accessor NAMES (`__errno_location`/`__error`) + the Linux-only fdatasync/fallocate/mremap planted undefined imports on the wrong format → a per-symbol `availableObjectFormats` gate (D-SHIPPED-SYMBOL-PER-TARGET-AVAILABILITY); verified by RUNNING the elf corpora in WSL (the local gate is compile-only for cross-targets). ✅ c16 DONE (2026-06-26, 436/436): a sqlite3.c RE-PROBE (the harness, on c15c) revealed the next F001A frontier = 3 more headers — **time.h** (time_t/clock_t + `struct tm` 56B byte-identical glibc/Darwin, flat; time/localtime/localtime_r/gmtime/gmtime_r/strftime/clock), **sys/ioctl.h** (just the variadic `ioctl`), **dlfcn.h** (dlopen/dlsym/dlclose/dlerror + RTLD_NOW=2 flat + RTLD_GLOBAL 256/8 per-format variant — a collision trap with RTLD_DEEPBIND/RTLD_FIRST). All value-verified vs glibc + xnu/Libc/dyld; struct tm + RTLD_GLOBAL runtime-verified (the elf corpora RUN→42 in WSL — sizeof(struct tm)==56 + tm_year@20 via localtime_r). 3 corpus (shipped_time linux+darwin, shipped_dlfcn linux, shipped_dlfcn_macho). No new mechanism (rides the c13/c15a variant + c14c variadic-extern + c15d per-symbol axes; no symbol is format-absent so no per-symbol availability needed). NEXT: **re-probe sqlite3.c again** — the F001A header frontier should now be clear; the re-probe reveals the next gap (the `$`-char lexer P000E may be real or a header-failure cascade; the parse cascade P0001/P0002/P0009 should clear once headers resolve; then the link stage where c15d's per-symbol fix waits). ✅ c17 DONE (2026-06-26, 437/437): **CLOSE `D-PP-CONDITIONAL-INCLUDE-ORDERING`** (trigger fired by sqlite3.c — `#if SQLITE_OS_WIN`→`#include "windows.h"` P0016 + a TCL gen-script in `#if 0` tripping the `$` P000E). The prior session built the §B-chosen Option A (a conditional PRE-SCAN dead-region oracle); the in-cycle **review CAUGHT a CRITICAL SILENT MISCOMPILE** in it — the SynthBuilder pre-scan can't see predefined/header macros, so a `#if __STDC__` (predefined-guarded) LIVE branch was folded dead → a live illegal char consumed before the parser (e.g. a `#define` body) was SILENTLY dropped (reproduced end-to-end via the CLI). §B (user, 2026-06-26): **Option 1 = authoritative dead-regions** — the dead-branch illegal-char (P000E) suppression is now driven by the AUTHORITATIVE `MacroExpander` pass (`deadRanges()` recorded in `run()` from the full `table_`+`predefined_` state), so the oracle can NEVER diverge from the real branch decision; the dead-branch quote-include (P0016) gate stays in the conditional-aware SynthBuilder (`includeResolvable`). The pre-scan's dead-region machinery was REMOVED. Plan-locked GO-WITH-FIXES (byte-interval recorder + per-directive re-sync + proven-RED predefined test/false-green guard). A residual conservative LOUD edge (predefined-VALUE-position guards in the include gate) pinned as `D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE`. Corpus `preprocessor_dead_branch_include` (predefined-guard live arm; RUN→42 in WSL). NEXT: **re-probe sqlite3.c** — the header frontier (F001A) is clear post-c16 and the dead-branch P0016/P000E is fixed, so the parse cascade should clear; the re-probe reveals the next real frontier (deeper C, or the link stage where c15d's per-symbol fix waits). ✅ c18 DONE (2026-06-26, 438/438): a sqlite3.c RE-PROBE (the harness, on c17) cleared F001A/P000E/P0016 and revealed the next root — **NON-POSITIONAL macro expansion**. `MacroExpander::run()` collected the WHOLE file body and expanded it ONCE at EOF with the FINAL macro table, so a `#define name 0` applied RETROACTIVELY to earlier uses (SQLite's omit pattern: declare an API fn, then `#define name 0` → the declaration `void f(void);` became `void 0(void);` → parse cascade, 109 unbalanced scopes). Fixed by making expansion **POSITIONAL** (C 6.10.3): run() now FLUSHES the pending body through `expand()` at each table-mutating directive (`#define`/`#undef`), so each run expands against the table as it stood THERE. Plan-locked GO-WITH-FIXES (the `productText_` multi-flush crux test, the spanning-call fail-loud, `#undef`-as-flush-trigger); independent audit VERIFIED-CLEAN (`productText_` append-only → multi-flush spans valid; hide-sets per-flush; c17 dead-ranges orthogonal; define-before-use byte-identical). 9 unit pins + corpus `preprocessor_positional_define` (declare-then-`#define` omit pattern, exit 42). RE-PROBE confirmed the omit-pattern errors (5541/142679) GONE; the cascade ROOT advanced to sqlite3.c:15516/18852 (a struct-member construct — `volatile int x;` member → "got 'volatile'"). Also found + pinned a SEPARATE pre-existing bug `D-PP-LINE-COMMENT-BEFORE-DIRECTIVE` (`<code> // comment` then a `#` directive desyncs directive recognition; not on the SQLite path). **NEXT = c19 = RE-PROBE → diagnose the 15516/18852 struct-member frontier (the new cascade root).** Owning the per-target-struct-layout mechanism
+ the full POSIX header set SQLite's `os_unix.c` needs. §B decision (2026-06-26, user): build the
**full per-target struct-layout mechanism** (not linux-x86-64-only), so macOS/arm64 SQLite are
reachable without reworking the descriptors. Closes the mechanism half of
`D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH`.

## §1 Why

SQLite's `os_unix.c` (the real unix VFS — the FULL-UNIX-build strategy, run-green in WSL) references
a fixed POSIX surface (probed from the actual amalgamation source):
- **~28 libc functions**: open close read write pread/pwrite(64) fstat lstat stat fcntl mmap munmap
  mremap ftruncate fallocate fchmod fchown unlink mkdir rmdir readlink getcwd geteuid access ioctl
  getpagesize.
- **~40 constants**: O_RDONLY/RDWR/CREAT/EXCL/NOFOLLOW/CLOEXEC/LARGEFILE/TMPFILE, F_GETFD/SETFD/
  GETLK/SETLK/SETLKW/RDLCK/WRLCK/UNLCK/OK, S_IRUSR/IWUSR/IRGRP/…, PROT_READ/WRITE, MAP_SHARED/FAILED,
  SEEK_SET, ~20 errno E* (EACCES EAGAIN EINTR EINVAL EIO ENOENT ENOSPC EPERM …).
- **~12 pthread fns** (pthread_mutex_init/lock/trylock/unlock/destroy, pthread_self/equal,
  pthread_mutexattr_*) + the errno lvalue macro (`errno → (*__errno_location())`).
- **4 field-accessed / size-critical structs**: `struct stat` (st_size/st_mode/st_uid/st_gid/st_ino/
  st_dev/st_nlink/st_blksize/st_mtim[espec]), `struct flock` (l_type/l_whence/l_start/l_len/l_pid),
  `struct timespec` (within stat), `pthread_mutex_t` (allocated → size-critical).

The constants + symbols are mechanical (linux LP64 values, arch-invariant within an OS — the established
sys/types pattern). **The structs are NOT mechanical**: their byte layout diverges per target
(x86-64-linux `struct stat` = 144B; arm64-linux = 128B with different field order; macOS differs again
and even uses different field NAMES — `st_mtimespec` vs `st_mtim`). libc writes the real struct and we
read fields at OUR offsets, so one descriptor layout silently corrupts data on the other targets. The
descriptor `structs` surface is single-layout → the mechanism gap.

## §2 The mechanism (grounded by the c13 code-explorer trace)

KEY de-risk: x86-64-linux and arm64-linux have IDENTICAL `AggregateLayoutParams` (Natural align, max 16)
→ `computeLayout` is purely param-driven, never branches on arch. So the per-target offset difference
comes ENTIRELY from the FIELD LIST (explicit padding fields + field order). Per-target layout =
**per-target field-list selection in the decoder**; injection + layout engine UNCHANGED. Selection is at
semantic time (analyze() runs per-target already) → does NOT multiply the c9 once-per-format-kind build.

Tiers (mirrors the existing `signatureByDataModel` precedent — decode-all-variants, parse-all eagerly so
a malformed non-active variant fails loud at load, select-active):
1. **analyze()** (`semantic_analyzer.hpp:59`) gains `optional<string_view> activeTarget` (the arch name,
   `target.name()`); nullopt = back-compat (LSP/tests skip variant selection → flat-`fields` fallback).
2. **EngineState** (`semantic_analyzer.cpp:139`) gains `optional<string> activeTarget`, set in analyzeImpl.
3. **buildCuMirImpl** (`compile_pipeline.cpp:231`) passes `target.name()` (already in scope).
4. **Schema** (`shipped_lib_descriptor.{hpp,cpp}`): a `ShippedStruct` may carry `variants:
   [{ "when": {arch?, format?}, "fields":[…] }]` INSTEAD of flat `fields`. `when` keys matched against
   (activeTarget arch, activeFormat ObjectFormatKind name); all specified keys must match; FIRST match
   wins. A struct with flat `fields` (no variants) keeps single-layout behavior (existing descriptors
   untouched). The structs `rejectUnknownKeys` allow-list gains `variants`.
5. **Decoder** (`readShippedLibDescriptor`, struct decode ~708-791) gains the (activeTarget, activeFormat)
   params; decodes ALL variants (each field list parsed → fail-loud on any malformed variant), selects the
   matching variant's field list, interns `structType(name, selectedFields)`. NO variant matches + variants
   present → struct NOT injected (referencing it → fail loud, undefined type) — never a silent wrong layout.
6. **Injection** (`semantic_analyzer.cpp:5304-5332`) UNCHANGED — consumes whatever field list the decoder
   produced.

AGNOSTIC: the engine matches CONFIG-provided (arch, format) strings against the active target's
(`target.name()`, format-kind name) — generic string match, no hardcoded `if(arch=="x86_64")` in shared
code. Arch/format names live in the JSON + the target schemas, never the engine.

## §3 Build sequence (staged; the HARD part = the mechanism, lands first with a real proof)

- **c13 (this cycle) — the mechanism + proof**: tiers 1-6 + the FIRST real divergent struct `struct stat`
  authored with x86_64-elf / arm64-elf / arm64-macho variants. PROOF = (a) a synthetic per-target shipped
  struct unit test (different offset per target, red-on-disable) + (b) a runtime corpus that makes a
  per-target field offset observable (swap a variant → wrong offset → wrong exit). Closes the MECHANISM
  half of `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH`.
- **c14+ (mechanical, behind the landed mechanism)**: author the 6 POSIX headers' full surfaces — errno
  (E* + errno macro), fcntl (O_/F_ + struct flock + open/fcntl), sys/stat (S_ + struct stat + fstat/…),
  unistd (symbols + SEEK_), sys/mman (PROT_/MAP_ + mmap/…), pthread (fns + pthread_mutex_t size) — linux
  first (compile-green on the WSL run target), then the macOS variants (struct layouts + per-OS constant
  values + typedef widths via the same per-target axis extended to constants/typedefs). Then re-probe
  sqlite3.c.

## §2.1 Plan-lock folds (c13 audit, GO-WITH-FIXES — crux VERIFIED sound: x86_64/arm64 target.json AggregateLayoutParams byte-identical, computeLayout param-driven no arch branch)

- **F1 (silent-miscompile gap) — selection contract is MATCH-ALL-SPECIFIED + AMBIGUOUS→FAIL-LOUD, not "first wins".** Active target identity = `(arch = target.name(), format = objectFormatKindName(activeFormat))`. A variant's `when` matches iff EVERY key it specifies equals the active value. If >1 variant matches → **fail loud** (config error `F_*`), never silently take the first (an under-specified `when:{arch:"x86_64"}` would otherwise match BOTH x86_64-elf AND x86_64-pe → linux layout silently on windows). 0 match → struct not injected → reference fails loud (`S_UnknownType`). For c13 `struct stat` I author FULLY-specified `when:{arch,format}` for all 3 variants → unambiguous by construction. DataModel axis: authored struct fields are concrete fixed-width/pointer types so the field list captures the per-target delta under the shared dm (LP64 on both ELF arches); a dataModel-distinct layout under the SAME (arch,format) is OUT of scope → fail loud, never silently collapse.
- **F2 (anti-lurking) — eager decode of ALL variants.** Every variant's field list is fully `parseTypeFromText`'d at read time regardless of which is active; a malformed INACTIVE variant fails the whole descriptor read on EVERY target (mirrors the `signatureByDataModel` "no lurking malformed override" property, shipped_lib_descriptor.cpp:497-540).
- **AGNOSTIC**: the selector compares `target.name()` + `objectFormatKindName(*activeFormat)` (config-sourced strings) against config `when` strings via GENERIC equality (no `if(arch=="x86_64")`); unknown `when` keys fail loud against the closed vocabulary (`objectFormatKindFromName` for format; the known target-name set for arch).

## §4 Test posture (plan-lock-strengthened)
- **Selection pin** (`tests/ffi/test_shipped_lib_descriptor.cpp` or `test_type_layout.cpp`): a synthetic
  descriptor with 2 variants whose field lists put the SAME named field at different offsets (e.g. A
  `{i32 pad; i64 x}`→x@8, B `{i64 x}`→x@0); assert selecting arch=A vs arch=B yields the two offsets via
  `computeLayout` on the interned type. RED-ON-DISABLE: neuter the selector (always variant 0) → the
  arch=B assertion fails.
- **Ambiguous-match pin (F1)**: a descriptor with 2 variants BOTH matching the active target → read fails
  loud (not "first wins").
- **Eager-decode pin (F2)**: a descriptor whose NON-active variant has an undecodable field type → the
  read fails loud on the active-OTHER-target compile (anti-lurking).
- **Fail-loud-when-referenced**: variants-present-but-none-match → struct not injected → a c-subset
  program referencing it emits `S_UnknownType`.
- **Runtime corpus, RELEASE-armed, real CI legs (F3/F4.3)**: per-arch witness of the real `struct stat`
  layout via `sizeof`/field-offset (libc-free, compiler-folded): x86_64-elf native asserts the x86_64
  layout (144B), arm64-elf qemu asserts the arm64 layout (128B), each `--config=release`, exit 42 iff
  correct. RED-ON-DISABLE: neuter the selector → arm64 corpus sees the x86_64 size → exit ≠ 42.

## §5 Closure gates (c13 DONE only when ALL hold — from the plan-lock)
1. Agnosticism scan clean (no arch/format literal in the selector or computeLayout).
2. Crux re-verified post-build: an assertion that x86_64-elf & arm64-elf feed identical AggregateLayoutParams (so the only layout delta is the field list; catches a future target.json divergence).
3. Ambiguous-match fails loud (F1 pin).
4. Eager-decode fails loud (F2 pin).
5. Selection pin red-on-disable (demonstrated, not asserted).
6. Runtime corpus release + both legs (x86_64 native + arm64 qemu), red-on-disable demonstrated.
7. Fail-loud-when-referenced preserved (`S_UnknownType`).
8. Back-compat proven: existing flat-`fields` descriptors (timeval/the current opaque structs) + LSP/test callers (activeTarget=nullopt) byte-identical; full ctest green, no example diff.
9. Anchor: `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH` mechanism-half closed only when 1-8 hold; the c14+ full-header authoring carried as an OPEN follow-on priority (recorded deferral, not a silent slice).
