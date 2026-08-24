/* Reached ONLY through a `#include "local_probe.h"` that sits behind a
   `#if __has_include("local_probe.h")` guard, in a TU that first executes the
   real-world `#ifndef __has_include / #define __has_include(x) 0 / #endif`
   portability shim. If the shim were allowed to shadow the operator, the guard
   would read 0, this file would never be spliced, and HAS_INCLUDE_LIVE would
   not exist -- the program would not compile at all. */
#define HAS_INCLUDE_LIVE 40
int has_include_probe_marker(void) { return 2; }
