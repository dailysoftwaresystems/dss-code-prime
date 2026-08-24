/* LANE-O (D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR, the PE fail-loud WITNESS half of
   roadmap C2): taking the ADDRESS of an extern CRT DATA export in a file-scope
   (static) initializer must be REJECTED on the PE image arm.

   ★ WHY THE SUBJECT CHANGED. This used to name `_fmode`, and briefly used an
   invented form that appended an image-name string literal after the declarator
   and is not C at all (ISO/IEC 9899:2024 6.7.1 admits only `declarator` or
   `declarator = initializer`). The rejected shape is deliberately NOT spelled
   out here: a grep-based sweep for it must not match this explanation —
   [[D-CSUBSET-EXTERN-LIBRARY-SYNTAX-IS-NOT-VALID-C]]. `_fmode` cannot serve:
   ✔MEASURED on two instruments over all 2,484 ucrtbase.dll exports, it is
   ABSENT — UCRT publishes it only through `__p__fmode`, so on this branch a
   bare `extern int _fmode;` is unbound and this example failed with
   K_SymbolUndefined, i.e. FOR THE WRONG REASON, with its real subject MASKED.
   `_mbcasemap` IS a real ucrtbase DATA export (ordinal 527, RVA 0x001397E0, in
   READ-WRITE `.data`; also msvcrt ordinal 520) and ships from ctype.json as a
   pe-gated `kind: object` row, so the extern BINDS and the data-item reloc this
   example is actually about is reached again.

   `_mbcasemap` is typed `unsigned char *` and NOT `unsigned char []` because
   ✔MEASURED the UCRT export holds a POINTER to the case map, not the map — see
   examples/c/extern_data_import_pe for the probe and the matched msvcrt
   control. That detail does not change this example's mechanism: the subject
   here is `&<extern DATA object>` in a static initializer, whatever the
   object's own type.

   A bare `extern unsigned char *_mbcasemap;` binds the descriptor corpus's
   per-format library (pe: ucrtbase.dll) as an ExternImport{isData}; the static
   initializer `unsigned char **p = &_mbcasemap;` lowers to a data-item abs64
   relocation whose target is that extern DATA import.

   On a PE image the extern's symbolVa is its .idata IAT SLOT (a loader-filled
   indirection cell), NOT the imported object — so BAKING it into the pointer
   slot would leave `p` one indirection off at runtime (pointing at the IAT
   slot, not at `_mbcasemap`). MSVC itself rejects the identical shape as a
   non-constant C initializer (C2099), and PE has NO symbol-based image
   relocation to fold it the ELF/Mach-O way. So pe::encodeExec FAILS LOUD with
   K_RelocationKindMismatch naming the extern + the anchor + the C2099 parity,
   and emits NO binary.

   This is a source-level DIAGNOSTIC test (expectDiagnostics): it drives the
   FULL pipeline through BOTH the in-process (Program::compileFiles) AND the
   CLI-subprocess harness, asserts a REJECTED compile (non-zero exit), and
   produces NO artifact. The reject is raised at the linker tier with no source
   span, so it renders code-only `error[K_RelocationKindMismatch]`
   (positioned:false). The message-text pins (anchor + the extern name + C2099)
   live in the structural unit test PeExecWriterExternSlot.* .

   RED-ON-DISABLE: drop the pe.cpp data-item extern-DATA reject -> the bad
   IAT-slot bake compiles silently (one indirection off) -> zero diagnostics ->
   this manifest fails. Un-ship ctype.json's `_mbcasemap` row -> the extern is
   unbound and the failure regresses to K_SymbolUndefined, i.e. back to failing
   for the WRONG reason, which is the state this example was rescued from — and
   note that a manifest asserting only "some error" would have hidden that, so
   the code pin is the load-bearing part. Contrast extern_data_addr_static_init
   (the ELF/Mach-O positive arms, which HAVE a symbol-based fold) and addr_import
   (a FUNCTION extern's address stays legal on PE — its symbolVa is a callable
   import thunk). */
extern unsigned char *_mbcasemap;
unsigned char **p = &_mbcasemap;

int main(void) {
    return 0;
}
