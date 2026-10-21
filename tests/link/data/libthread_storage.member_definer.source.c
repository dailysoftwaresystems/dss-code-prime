/* SOURCE of one of the two members of the committed fixtures
 * `tests/link/data/libthread_storage_x86_64_pe.a`,
 * `tests/link/data/libthread_storage_x86_64_elf.a`,
 * `tests/link/data/libthread_storage_aarch64_elf.a` and
 * `tests/link/data/libthread_storage_arm64_macho.a`, kept beside them as
 * `tests/ffi/data/libtls.source.c` is kept beside its own. The other member,
 * and the commands that build the four archives from the two, are
 * `libthread_storage.member_reader.source.c`.
 *
 * WHAT IT IS. An ORDINARY definition of `shared`: the member the archive
 * search pulls for a program that REFERS to `shared` as a thread-local, which
 * the link then refuses by name --
 * `examples/c/ordinary_definition_for_a_thread_local_reference_refused`.
 */
int shared = 7;
