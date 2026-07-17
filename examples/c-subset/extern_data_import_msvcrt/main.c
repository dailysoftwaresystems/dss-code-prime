/* c149 (D-LK-EXTERN-DATA-IMPORT, the PE half — the LAST missing image
   binding model): source-level extern library DATA objects on Windows.

   `_fmode` and `_environ` are REAL msvcrt.dll DATA exports (verified on
   this box via GetProcAddress(msvcrt, "_fmode"/"_environ") — both
   resolve to data addresses, _fmode reads 0x0 at CRT init, _environ a
   non-null char** after msvcrt's DllMain). A bare `extern int _fmode;`
   at file scope (no definition in any CU) lowers to HIR ExternGlobal;
   FF5 binds the c-subset per-format default library (pe: msvcrt.dll);
   the row survives the LK11 merge as ExternImport{isData} and binds per
   the format's declared `dataImportBinding: "got-indirect"` — PE
   `__imp_<name>` semantics: the loader fills the extern's .idata IAT
   slot with the imported OBJECT's address (the same slot mechanism
   function imports use; for data there is NO `FF 25` thunk — a data
   object is not callable). MIR->LIR materializes the object's address
   as lea-of-slot + deref (the shared got-indirect lowering the Mach-O
   c117 arm uses; zero lowering changes for PE).

   The assertion ladder discriminates every wrong-binding class:
   - `_environ != 0`: the deref'd slot points at a live CRT object (a
     dead/unfilled slot reads 0 -> exit 2).
   - `_fmode == 0`: the exact initial VALUE of the msvcrt data export
     (empirical, above). Bound to a thunk VA instead, the read returns
     `FF 25 xx xx` jump-stub BYTES (nonzero) -> exit 3; bound to the
     slot without the deref (bare lea), the read returns the low 32
     bits of the object's msvcrt ADDRESS (nonzero in practice) -> 3.
   - write 0x4000 (_O_TEXT) THROUGH the import + read it back, then
     restore 0: a thunk-VA binding makes the store hit read-only
     executable .text -> 0xC0000005, never exit 42. The read-back
     proves store and load agree on the same loader-filled address.

   RED-ON-DISABLE: drop `dataImportBinding` from
   pe64-x86_64-windows-exec.format.json -> the linker's pre-walker gate
   rejects loud (K_FormatLacksImportSupport) at compile time; bind data
   externs to thunk VAs -> exit 3 (or AV on the store); skip the
   got-indirect deref -> exit 3. pe64-ONLY: `_fmode`/`_environ` are
   msvcrt names (the elf/macho data-object witness is
   stdio_stream_objects via libc stdout/stderr/stdin). */
extern int _fmode;
extern char **_environ;

int main(void) {
    if (_environ == 0) return 2;   /* loader filled the slot; deref chain live */
    if (_fmode != 0) return 3;     /* exact initial value of the real object */
    _fmode = 0x4000;               /* store THROUGH the import (_O_TEXT) */
    if (_fmode != 0x4000) return 4;
    _fmode = 0;                    /* restore the CRT default */
    if (_fmode != 0) return 5;
    return 42;
}
