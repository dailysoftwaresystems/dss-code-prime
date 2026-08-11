/* LANE-O (D-LK-EXTERN-DATA-IMPORT, the PE half): source-level extern library
   DATA objects on Windows, declared in PLAIN STANDARD C.

   ★ WHY THIS FILE CHANGED SUBJECT. It used to declare `extern int _fmode;` /
   `extern char **_environ;`, and briefly an invented form that appended an
   image-name string literal after the declarator — deliberately NOT spelled out
   here, so a grep-based sweep for it cannot match this explanation. It is
   NOT C at all (ISO/IEC 9899:2024 6.7.1 admits only
   `declarator` or `declarator = initializer` after a declaration specifier, so
   nothing may follow a declarator but an initializer) —
   [[D-CSUBSET-EXTERN-LIBRARY-SYNTAX-IS-NOT-VALID-C]]. Neither name can be an
   extern DATA witness on UCRT: ✔MEASURED on two instruments over all 2,484
   ucrtbase.dll exports, `_fmode` and `_environ` are ABSENT — UCRT publishes
   them ONLY through the `__p__fmode` / `__p__environ` accessors, which is why
   they now ship from stdlib.json as pe-gated MACROS onto those accessors
   (`_fmode` -> `(*__p__fmode())`, exactly as ucrt/stdlib.h:255 defines it) and
   are reached by `#include <stdlib.h>` with no `extern` at all — witnessed by
   shipped_fmode_environ_pe, not here. The extern-DATA claim needs a name that
   really IS a DATA export, so this example moved to `_mbcasemap`.

   ★ `_mbcasemap` is one of exactly TWO non-code exports in the whole ucrtbase
   image (the other, `_wctype`, is .rdata and appears in no ucrt header).
   ✔MEASURED 2026-08-10, ucrtbase.dll 10.0.26100.8875, GNU objdump 2.42 `-p`
   and MSVC dumpbin 14.44.35207 `-exports` CONCURRING: ordinal 527, RVA
   0x001397E0, inside the `.data` section (RVA 0x138000..0x13A623) whose flags
   are 0xC0000040 = Initialized Data, READ WRITE — which is what makes the
   store-and-restore leg below legal. It is ALSO an msvcrt.dll export (ordinal
   520), so the row survives a revert of the CRT migration. It ships from
   ctype.json as a pe-gated `kind: object` row — THE FIRST `kind: object` row
   the pe axis has ever had; the other seven in the corpus are elf/macho.

   ★★★ WHY IT IS DECLARED `unsigned char *` AND NOT `unsigned char []`, WHICH IS
   THE WHOLE POINT OF THIS WITNESS AND IS A MEASURED REFUTATION OF THE OBVIOUS
   SHAPE. ucrt/mbctype.h:30 declares `extern unsigned char _mbcasemap[];`, so an
   array is what the SDK appears to promise. THE UCRT EXPORT IS NOT AN ARRAY.
   ✔MEASURED by native probe (cl 14.44 /MD, two runs): the 8 bytes AT the export
   are a POINTER, 0x00007FF85DB28730, BYTE-EQUAL to `__p__mbcasemap()`'s return
   on the same run, while the export's OWN address is 0x00007FF85DB297E0 — the
   two differ, so the accessor is not `&_mbcasemap`; the qword at export+8 is a
   second pointer exactly +0x100 away (a POINTER TABLE), and the 256 bytes at
   the pointed-to address are a real case map (125 of 256 nonzero). THE MATCHED
   CONTROL SETTLES IT: legacy msvcrt.dll's `_mbcasemap` (ordinal 520, RVA
   0x9E400) IS the table itself, 125 of 256 nonzero — the same population count.
   The CRT changed the export's SHAPE from array to pointer and the SDK header's
   direct-declaration arm still describes the msvcrt shape
   ([[D-FFI-UCRT-HEADER-DECLARES-MBCASEMAP-AS-ARRAY]], an upstream defect DSS
   records rather than mirrors).
   ⚠ THE ARRAY SHAPE IS SILENTLY WRONG, and the surface that decides it is THIS
   FILE'S DECLARATOR, not the descriptor row's `signature` — realization hands
   over only the LIBRARY, and nothing anywhere compares a shipped row's `kind`
   or type against the source declaration
   ([[D-FFI-SHIPPED-ROW-KIND-NOT-CROSSCHECKED-AGAINST-DECLARATOR]]).
   ✔MEASURED by EXERCISING the arm, not by reasoning about it: swapping the
   declaration below to `extern unsigned char _mbcasemap[];` COMPILED rc=0 with
   ZERO diagnostics and the program RAN to exit 4. Mechanism: got-indirect
   derefs the .idata slot to the OBJECT's address and an array reference decays
   THERE via Gep(...,0), so `_mbcasemap[0]` reads the first byte of the POINTER
   (measured 0x30) instead of the case map's first byte (measured 0x00), while a
   `ptr` object takes the second, C-level load and yields the pointer's VALUE.
   This is the `environ` class exactly (examples/c-subset/shipped_environ): a
   row that LINKS is not a row that WORKS, which is why this example RUNS and
   why exit 4 exists.

   Binding model: `extern unsigned char *_mbcasemap;` at file scope with no
   definition in any CU lowers to HIR ExternGlobal; the shipped-descriptor
   corpus — now the SINGLE owner of realization — supplies the library from
   ctype.json's per-format map (pe: ucrtbase.dll); the row survives the LK11
   merge as ExternImport{isData} and binds per the pe64 exec format's declared
   `dataImportBinding: "got-indirect"` (PE `__imp_` semantics: the loader fills
   the extern's .idata IAT SLOT with the imported OBJECT's address;
   symbolVa[dataExtern] = the slot VA; there is NO `FF 25` thunk for data — a
   data object is not callable). MIR->LIR materializes the object's address as
   lea-of-slot + deref, then the C-level load of a `ptr` object reads the
   pointer's VALUE — two indirections for a scalar, which is what makes the
   agreement check below meaningful.

   ★★ THE LADDER ASSERTS AN AGREEMENT, NOT A MAGIC VALUE, and that is deliberate.
   The predecessor pinned `_fmode == 0` — a hardcoded startup value that is a
   CRT-version fact, and one that reads the same as an unfilled slot. Here the
   oracle is `__p__mbcasemap()`, UCRT's OWN accessor for the same state (ordinal
   89, RVA 0x000C1D00, ✔MEASURED on both instruments), reached as a separate
   FUNCTION import. Every wrong-binding class is discriminated WITHOUT asserting
   any address or byte value, so the pin is host-, locale- and
   CRT-version-independent:
   - exit 2: the data import reads 0 — the loader never filled the slot (a dead
     slot, or `dataImportBinding` dropped so nothing is written).
   - exit 3: the accessor itself returned 0 — guards the ORACLE before trusting
     it, so a broken oracle cannot manufacture a pass at exit 4.
   - exit 4: THE AGREEMENT CHECK. The data import and the accessor must name the
     same address. Bound one indirection short (bare lea, no deref) the read
     returns the export's OWN address (measured 0x...97E0) instead of its
     CONTENTS (measured 0x...8730) -> mismatch. Bound to a thunk VA the read
     returns `FF 25 xx xx`-derived bytes -> mismatch. A stale or duplicated copy
     -> mismatch. None of these can pass this layer.
   - exit 5: the case map must hold at least ONE nonzero byte in 0..255 — a
     live table, not zero-fill. Only its EXISTENCE is asserted, never the count
     (measured 125 on this box, but that is a codepage fact, not an invariant).
   - exit 6/7: store a sentinel THROUGH the data import and read it back through
     the ACCESSOR's own pointer, then restore and re-check through both. This
     proves the two paths reach the SAME LIVE memory rather than agreeing on a
     stale address, and that the store lands somewhere writable — a thunk-VA
     binding would put it in R-X `.text` and raise 0xC0000005, never exit 42.
     Index 0 is the case map for the NUL character, so the write is inert and
     the restore returns the table byte-exact.

   RED-ON-DISABLE: drop `dataImportBinding` from
   pe64-x86_64-windows-exec.format.json -> the linker's pre-walker gate rejects
   loud (K_FormatLacksImportSupport) at compile time; un-ship ctype.json's
   `_mbcasemap` row -> honest K_SymbolUndefined at link (✔MEASURED by exercising
   it: on this example AND on extern_data_addr_reject_pe, which is what proves
   that example's CODE pin is load-bearing); retype the SOURCE DECLARATOR to an
   array -> exit 4 (a RUN failure with no diagnostic, which is why this witness
   runs). ⚠ Retyping the ctype.json ROW instead does NOT red — ✔MEASURED
   (`ptr<u8>` -> `arr<u8,256>`: compile rc=0, exit 42 at debug AND release, still
   passing), because realization hands over only the LIBRARY and nothing compares
   a row's kind or signature against the declarator
   ([[D-FFI-SHIPPED-ROW-KIND-NOT-CROSSCHECKED-AGAINST-DECLARATOR]]). This file
   claimed otherwise until the arm was run; a red-on-disable arm that cannot go
   red asserts nothing. Bind data externs to thunk VAs or drop the deref ->
   exit 4, or 0xC0000005 on the store. pe64-ONLY: `_mbcasemap` is a Windows CRT
   name; the elf/macho extern-DATA run witnesses are stdio_stream_objects (libc
   stdout/stderr/stdin — got-indirect on every format now, ELF included) and
   environ_alias_object_identity (the OBJECT-IDENTITY witness, which needs a
   second image; shipped_environ is its single-image smoke-test sibling).
   The optimized arms prove the slot-deref address materialization survives the
   release pipelines. */
extern unsigned char *_mbcasemap;
extern unsigned char *__p__mbcasemap(void);

int main(void) {
    unsigned char *viaImport;
    unsigned char *viaAccessor;
    unsigned char saved;
    int i;
    int nonzero;

    viaImport = _mbcasemap;
    if (viaImport == 0) return 2;          /* loader never filled the slot */

    viaAccessor = __p__mbcasemap();
    if (viaAccessor == 0) return 3;        /* guard the ORACLE itself */

    if (viaImport != viaAccessor) return 4; /* THE AGREEMENT CHECK */

    nonzero = 0;
    for (i = 0; i < 256; ++i) {
        if (viaImport[i] != 0) nonzero = 1;
    }
    if (nonzero == 0) return 5;            /* a live table, not zero-fill */

    /* Store THROUGH the data import; read back through the ACCESSOR. */
    saved = _mbcasemap[0];
    _mbcasemap[0] = 0x5A;
    if (viaAccessor[0] != 0x5A) return 6;  /* same live memory, or not */
    _mbcasemap[0] = saved;
    if (viaAccessor[0] != saved) return 7; /* restored byte-exact */

    return 42;
}
