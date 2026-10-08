/* SOURCE of the committed fixtures
 * `tests/link/data/compiled_unwind_<isa>_macho.o` and
 * `tests/link/data/relinked_unwind_<isa>_macho.o`, kept beside them as
 * `tests/ffi/data/libtls.source.c` is kept beside its own.
 *
 * WHAT IT IS. Two functions, so an object of it holds TWO call-frame records
 * of different extents, and a reader that attached a record to the wrong
 * function is caught by the extents alone. Each ISA has the object AS
 * COMPILED and Apple's RELOCATABLE LINK of it (`ld -r`): the linker keeps
 * every reference of the DWARF call-frame section (`__TEXT,__eh_frame`) as a
 * PAIR of relocations — a symbol to subtract, then at the same address the
 * symbol it is subtracted from — and stores only the addend in the field.
 * The x86_64 compiler's own object stores the resolved value and carries no
 * relocation there at all; the arm64 compiler's own object already keeps each
 * record's function address as such a pair.
 *
 * REBUILD, in a directory holding a copy of this file (the source is named
 * RELATIVELY, so no object records a directory):
 *
 *   cc -arch x86_64 -O0 -c relinked_unwind.source.c -o compiled_unwind_x86_64_macho.o
 *   cc -arch x86_64 -r -nostdlib -o relinked_unwind_x86_64_macho.o compiled_unwind_x86_64_macho.o
 *
 *   cc -arch arm64 -O0 -fasynchronous-unwind-tables -femit-dwarf-unwind=always -c relinked_unwind.source.c -o compiled_unwind_arm64_macho.o
 *   cc -arch arm64 -r -nostdlib -o relinked_unwind_arm64_macho.o compiled_unwind_arm64_macho.o
 *
 * (An arm64 object carries `__eh_frame` only for a function whose frame the
 * COMPACT encoding cannot describe. By default Apple's compiler describes
 * both of these compactly and writes no such section — with or without
 * `-femit-dwarf-unwind=always`, MEASURED: the two objects are byte-equal.
 * Asynchronous unwind tables describe the epilogue as well, which the compact
 * encoding has no word for, so the section appears; the second flag then
 * states BOTH functions in it.)
 *
 * PROVENANCE, MEASURED at the builds (2026-10-08):
 *
 *   Apple clang 21.0.0 (clang-2100.1.1.101), ld-1267
 *
 * What Apple's own linker makes of each object beside a `main` that returns
 * `dss_unwind_shared() + 35` is 42; the pins that read these bytes on every
 * host are the suite `MachoUnwindRelocations`
 * (`tests/link/test_macho_object_reader.cpp`).
 */
int dss_unwind_first(void) { return 3; }
int dss_unwind_shared(void) { return dss_unwind_first() + 4; }
