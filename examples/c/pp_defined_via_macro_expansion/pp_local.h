// D-PP-DEFINED-VIA-MACRO-EXPANSION corpus example: the real, resolvable header
// the `__has_include`-via-macro-expansion shapes in `main.c` ask about.
//
// It has to EXIST for those shapes to answer 1, and it is deliberately a QUOTE
// include (resolved relative to the including file) rather than an angle one:
// the angle path resolves shipped descriptors whose availability is
// PER-OBJECT-FORMAT, so an angle probe could answer differently on the four
// targets this example declares and there would be no single `exitCode`.
#define PP_LOCAL_MARKER 1
