/* SOURCE of one of the two members of the committed fixtures
 * `tests/link/data/libthread_storage_x86_64_pe.a`,
 * `tests/link/data/libthread_storage_x86_64_elf.a`,
 * `tests/link/data/libthread_storage_aarch64_elf.a` and
 * `tests/link/data/libthread_storage_arm64_macho.a`, kept beside them as
 * `tests/ffi/data/libtls.source.c` is kept beside its own. The other member is
 * `libthread_storage.member_definer.source.c`.
 *
 * WHAT IT IS. An ORDINARY reference to `shared`, in a function a program
 * calls: the member the archive search pulls for a program that DEFINES
 * `shared` thread-local, which the link then refuses by name --
 * `examples/c/thread_local_definition_for_an_ordinary_reference_refused`.
 *
 * THESE FOUR ARCHIVES ARE DSS's OWN OUTPUT, not a reference compiler's, and
 * are checked in rather than built at test time for one reason: the two
 * examples that link them are REFUSALS (`expectDiagnostics`), and the examples
 * runner refuses `dependsOn` beside one -- a refusal arm builds no image to
 * compare a prerequisite against -- so the library is a `prebuiltLibraries`
 * input instead.
 *
 * REBUILD, with a `dsscp` of this repository, in a directory holding copies of
 * the two member sources under the names `member_reader.c` and
 * `member_definer.c` (the archive's members are named after them), once per
 * format -- any host builds all four, the format is the `--target`:
 *
 *   dsscp --compile member_reader.c member_definer.c --language c --target x86_64:pe64-x86_64-windows-staticlib --output pe-x86_64
 *   dsscp --compile member_reader.c member_definer.c --language c --target x86_64:elf64-x86_64-linux-staticlib --output elf-x86_64
 *   dsscp --compile member_reader.c member_definer.c --language c --target arm64:elf64-aarch64-linux-staticlib --output elf-aarch64
 *   dsscp --compile member_reader.c member_definer.c --language c --target arm64:macho64-arm64-darwin-staticlib --output macho-arm64
 *
 * Each directory then holds ONE archive (`member_reader.lib` for PE,
 * `member_reader.a` otherwise): copy it to its fixture name.
 *
 * PROVENANCE: built 2026-10-07 by the Windows debug `dsscp` of cycle P69's
 * link lane. A later `dsscp` writes the same two members with the same
 * symbols, not necessarily the same bytes; the md5 of the bytes checked in is
 * in each example's `expected.json`, which records WHICH BYTES were measured
 * and is no pin a guard enforces.
 */
extern int shared;
int read_shared(void) { return shared; }
