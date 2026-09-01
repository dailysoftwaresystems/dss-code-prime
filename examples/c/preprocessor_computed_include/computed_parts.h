// [[D-PP-COMPUTED-INCLUDE-SILENT-DROP]] runtime witness (header leg 1).
//
// This header is named by a MACRO, never by a string literal in the includer:
// `#define HDR "computed_parts.h"` then `#include HDR` (C23 6.10.2p4). If the
// computed form is dropped instead of resolved, PART_A is undefined in main.c
// and the program FAILS TO COMPILE -- which is the point. The defect this
// closes produced NO diagnostic at all, so a witness that merely exits wrong
// would be weaker than one that cannot be built.
#define PART_A 30
