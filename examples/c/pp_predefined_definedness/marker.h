/* Spliced ONLY when the include-gating pre-scan reads the guard above the
 * `#include "marker.h"` as LIVE. Nothing else in this corpus example defines
 * SPLICED, so its definedness IS the pre-scan's verdict, read back by the
 * authoritative pass. */
#define SPLICED 1
