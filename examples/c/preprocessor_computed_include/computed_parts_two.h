// [[D-PP-COMPUTED-INCLUDE-SILENT-DROP]] runtime witness (header leg 2).
//
// Reached through a CHAIN of object-like macros -- `#define HDR_NAME HDR2`,
// `#define HDR2 "computed_parts_two.h"`, `#include HDR_NAME` -- so the include
// pre-scan must RESCAN its own substitution, not merely substitute once. gcc,
// clang and MSVC all compile this shape (✔MEASURED 2026-09-01).
#define PART_B 4
