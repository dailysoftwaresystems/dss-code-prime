/* The header the POSITIVE control for `__has_include` names. Its only job is to
   be really present and to carry a marker only a real textual splice delivers —
   see the control block in main.c for why an `__has_include` answer of 0 after
   the shadow is otherwise indistinguishable from a header that was never found.
   That confusion is not hypothetical: it is exactly what MSVC's answer looked
   like while this row was being measured, until the control was run. */
#define FQ_OPS_HEADER_REALLY_SPLICED 1
