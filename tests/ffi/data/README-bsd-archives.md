# BSD `ar` archive fixtures (`D-FF1-AR-BSD-VARIANT`)

Read by `tests/ffi/test_binary_reader_ar_bsd.cpp`.

These two files are **real output from real archivers**, not hand-authored
bytes. That is the point of them: a hand-built BSD archive would only test
`ar_reader.cpp` against the test author's reading of the format, and the two
would agree by construction. Both were produced on 2026-08-24 from the same
two trivial C sources — one with a short base name, one with a base name
longer than the 16-byte `ar` name field, to force the `#1/N` inline-name
shape:

```c
/* s.c */                      int dss_bsd_short(void) { return 20; }
/* a_very_long_member_name.c */ int dss_bsd_long(void)  { return 22; }
```

## `libbsdapple.a` — Apple `ar`, macOS 26.5.2 (arm64, Xcode toolchain)

```sh
clang -c s.c -o s.o
clang -c a_very_long_member_name.c -o a_very_long_member_name.o
ar rcs libbsdapple.a s.o a_very_long_member_name.o
```

`xcrun -f ar` →
`/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/ar`.
1352 bytes, md5 `bbd3ce423b008387a90ce82941e04e90` (verified on the Mac and
again after transfer). Symbol table `__.SYMDEF SORTED`; **Mach-O** members;
symbols carry the Mach-O leading underscore.

## `libbsdllvm.a` — `llvm-ar-19 --format=bsd`, Ubuntu (x86_64)

```sh
gcc -c s.c -o s.o
gcc -c a_very_long_member_name.c -o a_very_long_member_name.o
llvm-ar-19 --format=bsd rcs libbsdllvm.a s.o a_very_long_member_name.o
```

2768 bytes, md5 `b8e36f21b7c854b9133e88bb0c778e19`. Symbol table plain
`__.SYMDEF`; **ELF** members; no leading underscore.
`llvm-ar-19 --format=darwin` produced a **byte-identical** file (md5-compared),
so one parse arm serves both spellings.

## Why two, and why they must disagree

They differ in every way the format permits — symbol-table name, inline-name
padding width, member object format, symbol spelling, and **entry order**
(Apple sorts by symbol name, so its `ranlib[0]` is the *second* member). A
reader that quietly hardcodes one producer's choices passes against that
producer and fails against the other. Neither fixture matches the host that
runs the tests, which is the whole claim: an archive's variant is a property
of its bytes, never of the machine reading them.

## Verifying a fixture has not silently changed

The tests pin both file sizes, so a re-generated archive fails loudly rather
than shifting the offsets out from under the corruption mutants. To re-derive
the pinned offsets after a deliberate regeneration, use the independent
decoder (written from the format description rather than from
`ar_reader.cpp`, so its agreement is evidence and not an echo):

```sh
python scripts/... # or the throwaway probe recorded in the P31 lane-E notes
```

The reference tools' own view, for comparison:

```sh
ar t libbsdapple.a          # __.SYMDEF SORTED / s.o / a_very_long_member_name.o
nm -g libbsdapple.a         # _dss_bsd_short in s.o, _dss_bsd_long in the other
llvm-nm-19 --print-armap libbsdllvm.a
```
