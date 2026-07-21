#include <tcl.h>
#include <stdio.h>

/* D-FFI-DESCRIPTOR-UNION-MEMBER-INJECTION (SQLite testfixture arc, C34b 2026-07-21):
   the RUN witness for the Tcl_HashTable/Tcl_HashEntry/Tcl_HashSearch cluster + the
   hash API, driven by the REAL libtcl8.6 — the surface sqlite src/test_malloc.c needs.

   The NEW mechanism: the `key` field of Tcl_HashEntry is a real TypeKind::Union
   (Tcl_HashKey), and the shipped `unions` surface injects its member NAMES into a
   compositeScopeByType field scope (mirroring struct-field injection). Without that,
   the real Tcl_GetHashKey macro's `h->key.oneWordValue` / `h->key.string` reads fail
   S_NotAComposite. This exercises the REAL Tcl_GetHashKey macro (fail-loud-correct for
   every key type), NOT a sidestep.

   RUNTIME LAYOUT WITNESS (a wrong Tcl_HashTable/HashEntry/HashKey layout misreads
   these bytes): a ONE_WORD_KEYS table stores the key pointer in key.oneWordValue @
   offset 0 of the key union @ offset 32; the value goes in clientData @ offset 24;
   keyType lives @ offset 60 (Tcl_GetHashKey reads it to pick the oneWordValue branch).
   The exit value 42 is the clientData round-tripped through Tcl_Set/GetHashValue, and
   the key pointer must round-trip through Tcl_GetHashKey (else return 3).

   RED-ON-DISABLE: remove the union-member injection (semantic_analyzer.cpp) — the
   `it->key.oneWordValue` read inside Tcl_GetHashKey fails S_NotAComposite and this
   example stops compiling. */

static int keyStorage;   /* a stable, distinctive address to use as a one-word key */

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();          /* initialize the Tcl library */
    if (ip == 0) return 1;

    Tcl_HashTable t;
    Tcl_InitHashTable(&t, TCL_ONE_WORD_KEYS);      /* keys are one-word pointers */

    void *theKey = (void *)&keyStorage;
    int isNew = 0;
    Tcl_HashEntry *e = Tcl_CreateHashEntry(&t, theKey, &isNew);
    if (e == 0 || isNew != 1) return 2;

    Tcl_SetHashValue(e, (ClientData)(long)42);     /* macro -> e->clientData @ 24 */

    /* Iterate the (single-entry) table and read the value + key back out. */
    Tcl_HashSearch search;
    long got = 0;
    void *gotKey = 0;
    Tcl_HashEntry *it;
    for (it = Tcl_FirstHashEntry(&t, &search); it != 0; it = Tcl_NextHashEntry(&search)) {
        got = (long)Tcl_GetHashValue(it);          /* macro  -> it->clientData */
        gotKey = Tcl_GetHashKey(&t, it);           /* REAL macro -> it->key.oneWordValue */
    }

    if (gotKey != theKey) return 3;                /* the key union member round-trips */

    /* String-key leg: exercises Tcl_GetHashKey's OTHER branch, `key.string`. A
       TCL_STRING_KEYS table stores the key bytes INLINE starting at key.string
       (real tcl `char string[1]`, modeled `arr<char,1>`), so Tcl_GetHashKey returns
       &key.string (array->pointer DECAY = the ADDRESS of the stored key), NOT the
       first 8 key bytes read AS a pointer. RED-ON-DISABLE for the arr<char,1> model:
       modeling string as ptr<char> would read the inline "hello" bytes as a garbage
       pointer -> sk[0] != 'h'. test_malloc uses string-keys, so this is the branch
       that actually runs there. */
    Tcl_HashTable ts;
    Tcl_InitHashTable(&ts, TCL_STRING_KEYS);
    int isNew2 = 0;
    Tcl_HashEntry *es = Tcl_CreateHashEntry(&ts, "hello", &isNew2);
    if (es == 0 || isNew2 != 1) return 4;
    char *sk = (char *) Tcl_GetHashKey(&ts, es);   /* string branch -> the inline stored key */
    if (sk == 0 || sk[0] != 'h') return 5;         /* the address of "hello", not garbage */
    Tcl_DeleteHashTable(&ts);

    Tcl_DeleteHashTable(&t);
    Tcl_DeleteInterp(ip);
    puts("tcl-hash-ok");
    return (int)got;                                /* 42 */
}
