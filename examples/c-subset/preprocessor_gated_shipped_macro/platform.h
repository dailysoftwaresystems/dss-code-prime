/* A QUOTE-included companion header that defines the gating flag. The C19-C21
 * include-gating PRE-SCAN processes this quote include in a child SynthBuilder
 * whose localMacros is DISCARDED, so the parent pre-scan never learns
 * DSS_USE_ERRNO -- exactly like sqlite's SQLITE_OS_UNIX (defined in the
 * quote-included os_setup.h). That blindness is what used to suppress the
 * errno.h shipped-macro splice under the `#if DSS_USE_ERRNO` guard below. */
#define DSS_USE_ERRNO 1
