/* c158 (D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR, the PE fail-loud WITNESS half of
   roadmap C2): taking the ADDRESS of an extern msvcrt DATA export in a
   file-scope (static) initializer must be REJECTED on the PE image arm.

   `_fmode` is a REAL msvcrt.dll DATA export -- NOT a stdio.h macro. On PE,
   stdin/stdout/stderr route through the __iob_func() accessor, so they are
   FUNCTION imports, not data-object externs; `_fmode` is the honest
   extern-DATA shape. A bare `extern int _fmode;` binds the c-subset default
   library (pe: msvcrt.dll) as an ExternImport{isData}; the static
   initializer `int *p = &_fmode;` lowers to a data-item abs64 relocation
   whose target is that extern DATA import.

   On a PE image the extern's symbolVa is its .idata IAT SLOT (a loader-
   filled indirection cell), NOT the imported object -- so BAKING it into
   the pointer slot would leave `p` one indirection off at runtime (pointing
   at the IAT slot, not at `_fmode`). MSVC itself rejects the identical
   shape as a non-constant C initializer (C2099), and PE has NO symbol-based
   image relocation to fold it the ELF/Mach-O way. So pe::encodeExec FAILS
   LOUD with K_RelocationKindMismatch naming the extern + the anchor + the
   C2099 parity, and emits NO binary.

   This is a source-level DIAGNOSTIC test (expectDiagnostics): it drives the
   FULL pipeline through BOTH the in-process (Program::compileFiles) AND the
   CLI-subprocess harness, asserts a REJECTED compile (non-zero exit), and
   produces NO artifact. The reject is raised at the linker tier with no
   source span, so it renders code-only `error[K_RelocationKindMismatch]`
   (positioned:false). The message-text pins (anchor + `_fmode` + C2099)
   live in the structural unit test PeExecWriterExternSlot.* .

   RED-ON-DISABLE: drop the pe.cpp data-item extern-DATA reject -> the bad
   IAT-slot bake compiles silently (one indirection off) -> zero diagnostics
   -> this manifest fails. Contrast extern_data_addr_static_init (the ELF/
   Mach-O positive arms, which HAVE a symbol-based fold) and addr_import (a
   FUNCTION extern's address stays legal on PE -- its symbolVa is a callable
   import thunk). */
extern int _fmode;
int *p = &_fmode;

int main(void) {
    return 0;
}
