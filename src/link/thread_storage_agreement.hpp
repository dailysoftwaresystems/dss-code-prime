#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"

#include <cstddef>
#include <span>

// ── A LINKED REFERENCE AND THE DEFINITION IT BINDS TO AGREE ON STORAGE DURATION ──
// P69 round 4 (lane `xa`):
//   D-LK-THREAD-STORAGE-DISAGREEMENT-REFUSED-ONLY-BY-THE-WRITER-BACKSTOP
//
// C23 6.7.2p3 requires `thread_local` on every declaration of one object, and
// the two kinds of access cannot stand in for each other: an ordinary reference
// reaches ONE address for the whole process, a thread-local one an offset from
// the thread pointer, one object per thread. A link that binds a unit's
// reference to another unit's definition of the other storage duration is
// therefore refused, as every reference linker refuses it (✔MEASURED
// 2026-10-07 on gcc 13.3.0 objects: GNU ld 2.42 "TLS definition in ...
// mismatches non-TLS reference in ..." and "TLS reference in ... mismatches
// non-TLS definition in ...", for a plain reference, a COMMON and a
// thread-local reference alike; ld.lld 18.1.3 refuses the first two shapes and
// links the third into a program that faults, exit 139).
//
// ★ WHERE: the assembled-tier link, between `resolveCrossCuSymbols` (which
// pairs each reference with the definition its name binds to) and the merge
// (which would act on the pairing). Before this check the disagreement reached
// the image writers' CRIT-1 backstop, which refused it naming SymbolIds — or,
// for a reference the merge reads through a slot, explained it as an address
// constant the program never wrote. A unit reaches this tier as an object
// input, an archive member or a DSS unit linked beside one; DSS's own C units
// linked together are merged one tier up, in the MIR whole-program merge, which
// this file does not reach (D-CSUBSET-THREAD-LOCAL-CROSS-TU-MISMATCH).
//
// ★ WHAT DECIDES EACH SIDE: the DEFINITION is thread-local when its data item
// lives in a thread-local section (`Tdata` / `Tbss`), whatever unit wrote it.
// The REFERENCE is thread-local when its import row says so (a C declaration's
// `thread_local`, `ExternImport::isThreadLocal`) or when a relocation of its own
// unit reaches it through a tls-flagged kind (the target schema's `tls`): an
// object file carries no declaration, only the access its code makes. A COMMON
// that yields to a definition arrives here as the ordinary reference
// `allocateCommonDefinitions` — the link's first pass — made of it, and is
// refused as one; GNU ld's message calls it a "non-TLS reference" too.
//
// ★ WHAT IS NOT JUDGED: a reference no relocation of its unit names. A C
// declaration nothing uses reaches no reference linker either — gcc writes no
// symbol for an extern nothing uses — so refusing it would make DSS reject a
// program every reference accepts.
//
// ★ ...EXCEPT WHERE THE OBJECT'S OWN SYMBOL RECORD STATES THE STORAGE DURATION
// (`ExternImport::recordStatesStorageDuration`). The record is there whether or
// not code reads the name, and the linkers of such a format compare it with the
// definition's. ✔MEASURED 2026-10-08 — a tentative `int c;` that nothing reads,
// beside a thread-local definition of `c`, in both object orders:
//   * ELF (gcc 13.3.0 `-fcommon`; the symbol is `OBJECT GLOBAL COM`): GNU ld
//     2.42 ("c: TLS definition in ... mismatches non-TLS reference in ...") and
//     ld.lld 18.1.3 ("TLS attribute mismatch: c") REFUSE the pair;
//   * PE/COFF (cl 19.44, and clang 19.1.5 `-fcommon`; an UNDEF external whose
//     Value is the size, beside `c` in `.tls$`): link.exe 14.44 and lld-link
//     19.1.5 LINK it, and the program runs — 0, and 5 where a third unit reads
//     `c` as the thread-local it is;
//   * Mach-O (Apple clang 21 `-fcommon`; ld-1267 on both ISAs, ld64-957.1): the
//     same — linked, 0, and 5 with a thread-local reader.
// A COFF symbol and a Mach-O nlist have no type that says thread-local, so
// their linkers have nothing to compare; an ELF symbol has one. The answer is
// therefore the RECORD's, carried on the row by the reader that read it — never
// a question of which format the link writes. (A common its unit READS is
// judged on every format, by the access: Apple's ld links that pair into a
// program that reads the thread-local variable's descriptor as the datum.)
//
// ★ THE CROSS-UNIT MERGE DOES NOT CARRY THAT FACT, AND NEED NOT. This check
// judges each unit's rows as the link was given them, before the merge folds
// the rows of one name into one, and nothing after the merge reads the field.
// A relocatable artifact states the fact again on its own record: an ELF
// common is written as a COMMON symbol, and the reader that reads the artifact
// sets the field — so a foreign ELF common carried through a relocatable link,
// alone or folded with another unit's reference of its name, is still refused
// by the link after it.
//
// The code is `K_ExternImportAttributeConflict`, the code of the same rule where
// the definition is a LIBRARY's (`ffi::reportLibraryThreadStorageDisagreement`):
// one rule, one code, two binders.
namespace dss::linker {

// Report, once per reference, every pairing in `references` whose two sides
// disagree on storage duration; the number reported. Each diagnostic is an
// Error, so the caller's error-count check stops the link before the merge.
DSS_EXPORT std::size_t
reportThreadStorageDisagreements(std::span<AssembledModule const>         modules,
                                 std::span<LinkedImage::CrossCuRef const> references,
                                 TargetSchema const&                      targetSchema,
                                 DiagnosticReporter&                      reporter);

} // namespace dss::linker
