# Plan 25 — POSIX headers via per-target struct layouts (the SQLite os_unix surface)

§0 STATUS: ✅ MECHANISM DONE (p18 Cluster G c13, 2026-06-26, 422/422 ctest Release) — `variants` decode/select + `activeTarget` thread + `sys/stat.json` struct stat (x86_64-elf 144B / arm64-elf 128B) + corpus + pins; closure gates 1-9 met. ⏳ c14 IN PROGRESS: errno (c14a) + sys/mman + unistd (c14b) DONE; the **variadic-externs FFI extension** (c14c — `parseTypeFromText` accepts `...` → variadic FnSig + the emitter round-trips it; reuses FC12 variadic-call codegen) DONE → unblocks `open`/`fcntl`. fcntl (O_/F_ + struct flock + variadic open/fcntl, c14d) + sys/stat completion (S_ + S_IS* + fstat/lstat/mkdir + st_*time macros, c14d) DONE — **5/6 linux POSIX headers done**. pthread (c14e) DONE — `pthread_mutex_t` shipped as a FLAT MAX-size (48B = the arm64 size) 8-aligned opaque typedef (`arr<i64,6>`), which SIDESTEPS the per-arch-typedef issue: x86-64 over-allocates 8B, harmless because libc touches only its own 40 and SQLite treats the mutex as opaque. **ALL 6 linux POSIX headers DONE** (errno/sys.mman/unistd/fcntl/sys.stat/pthread) + the 2 engine extensions (per-target structs c13, variadic externs c14c). NEXT: **re-probe sqlite3.c** (user runs the harness) to reveal the next frontier past header-resolution; macOS variants are a separate axis if targeting macOS (the run target is linux/WSL). Owning the per-target-struct-layout mechanism
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
