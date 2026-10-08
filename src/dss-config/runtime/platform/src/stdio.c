/* <stdio.h>'s formatted input/output — the C23 entry points DSS supplies where the platform's C library lacks
 * C23's conversions (D-C-C23-CONVERSIONS-MISSING-ON-THE-UCRT-AND-LIBSYSTEM).
 *
 * The descriptor binds each public name to `__dss_isoc23_<name>` (its `linkName`) and realizes it from this file —
 * glibc's own design, where C23 callers reach `__isoc23_<name>` and the platform's plain name is never redefined.
 * The platform's own implementation is reached through the private `__dss_platform_*` rows stdio.json declares: on
 * pe the v-primitives are stdio_ucrt.c's bodies over the UCRT's `__stdio_common_v*` cores (ucrtbase exports no
 * printf family at all), on Mach-O imports of libSystem's plain names.
 *
 * THE ENGINE OWNS THE PARSE, RENDERS ONLY WHAT THE PLATFORM LACKS, AND DELEGATES EVERYTHING ELSE.
 *   * FAST PATH — a format with no specification the platform lacks goes to the platform WHOLE, with the caller's
 *     va_list untouched: byte-identical to calling the platform directly. What the platform lacks: printf's
 *     conversions `b`/`B` and length modifiers `wN`/`wfN` (C23 7.23.6.1), and `n` where the platform refuses it;
 *     scanf's conversion `b`, conversion `i` (its `0b` form) and `wN`/`wfN` (C23 7.23.6.2).
 *   * SLOW PATH — only such formats. The engine walks the format keeping the running count; renders `b`/`B` itself;
 *     rewrites `wN`/`wfN` to the platform's own length modifier taken from <inttypes.h> (the PRI/SCN macros, so the
 *     engine and the header cannot disagree); performs `n` itself; and hands every other specification to the
 *     platform ONE AT A TIME with the argument the engine fetched by its class — so every floating, wide,
 *     locale-dependent and vendor conversion keeps the platform's exact bytes. An unknown specification goes to the
 *     platform verbatim, consuming no argument (the platforms' own rule, measured). The path is decided BEFORE the
 *     va_list is touched, so no fallback ever needs a second walk of it (DSS has no va_copy yet).
 *   * The platform-specific pieces — which vendor length modifiers and conversions exist and what argument each
 *     takes, whether the platform performs `%n`, whether it has positional arguments, how it reads a base prefix with
 *     no digit after it — are asked of the format's own unit (stdio_ucrt.c / stdio_libsystem.c), so this file names
 *     no object format, C runtime or processor. The argument-class protocol those units answer in is one letter:
 *     i int · l long · L long long · j intmax_t · z size_t · t ptrdiff_t · d double · D long double · p pointer.
 *   * NO FIXED BOUND (P69 round 4): a delegated specification is handed over as its own text, as long as it is — a
 *     hundred repeated flags, a ten-digit width, a 200-character scanset — in a local buffer when it fits and on the
 *     heap otherwise; the references read every such specification whole (✔MEASURED, lane lm findings). */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The platform primitives and answers are stdio.json's private `__dss_platform_*` rows, declared by <stdio.h> like
 * every other name this file calls — so each definition in stdio_ucrt.c / stdio_libsystem.c, and each libSystem
 * import, is checked against the one declaration the descriptor states. */

enum { A_NONE = 0, A_INT, A_LONG, A_LLONG, A_INTMAX, A_SIZE, A_PTRDIFF, A_DOUBLE, A_LDOUBLE, A_PTR };
enum { L_NONE, L_HH, L_H, L_L, L_LL, L_J, L_Z, L_T, L_BIGL, L_W, L_WF, L_EXT };
enum { F_MINUS = 1, F_PLUS = 2, F_SPACE = 4, F_HASH = 8, F_ZERO = 16 };
/* A decimal number in a specification larger than an int holds: read_number saturates there instead of wrapping or
 * dropping digits, so a ten-digit width is never read as a different width. */
#define DSS_NUMBER_OVER ((long long)INT_MAX + 1)

static int class_of_letter(char c) {
    switch (c) {
        case 'i': return A_INT;
        case 'l': return A_LONG;
        case 'L': return A_LLONG;
        case 'j': return A_INTMAX;
        case 'z': return A_SIZE;
        case 't': return A_PTRDIFF;
        case 'd': return A_DOUBLE;
        case 'D': return A_LDOUBLE;
        case 'p': return A_PTR;
        default: return A_NONE;
    }
}

struct dss_spec {
    const char *begin, *end;          /* '%' .. one past the conversion (or the scanset's ']') */
    const char *flagsBegin, *flagsEnd;
    const char *widthBegin, *widthEnd;   /* a literal width's digits (empty for a `*` or no width) */
    const char *precBegin, *precEnd;     /* a literal precision's digits, after its '.' */
    const char *lenBegin, *lenEnd;
    unsigned    flags;
    int         hasWidth;             /* a width is given: digits or a `*` */
    long long   width;                /* a literal width, saturated at DSS_NUMBER_OVER */
    long long   prec;                 /* -1: none (or a `*`); else a literal precision, saturated likewise */
    int         widthStar, precStar;
    int         widthPos, precPos, argPos;   /* n$ positions, 0: none */
    int         len, bits;            /* length modifier; wN/wfN's N */
    int         lenArg;               /* L_EXT: the class the platform's length names (A_NONE: the conversion's own) */
    int         arg;                  /* the class of the argument the specification consumes */
    int         suppress;             /* scanf '*' */
    char        conv;
    int         known;                /* a conversion the engine can classify */
};

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static long long read_number(const char **pp) {
    const char *p = *pp;
    long long v = 0;
    while (is_digit(*p)) {
        if (v < DSS_NUMBER_OVER) v = v * 10 + (*p - '0');
        if (v > DSS_NUMBER_OVER) v = DSS_NUMBER_OVER;
        ++p;
    }
    *pp = p;
    return v;
}

/* `n$` at p: its position with the cursor moved past it; 0 when absent or when the platform has no positions. A
 * position past an int is INT_MAX — past every plan's DSS_MAX_POSITIONS, so the format stays the platform's. */
static int read_position(const char **pp, int positional) {
    const char *p = *pp;
    if (!positional || !is_digit(*p) || *p == '0') return 0;
    const char *q = p;
    long long const n = read_number(&q);
    if (*q != '$') return 0;
    *pp = q + 1;
    return n > INT_MAX ? INT_MAX : (int)n;
}

static int class_of_size(size_t size) {
    if (size <= sizeof(int)) return A_INT;
    if (size == sizeof(long)) return A_LONG;
    return A_LLONG;
}

static size_t fast_size(int bits) {
    switch (bits) {
        case 8: return sizeof(int_fast8_t);
        case 16: return sizeof(int_fast16_t);
        case 32: return sizeof(int_fast32_t);
        default: return sizeof(int_fast64_t);
    }
}

static size_t w_size(struct dss_spec const *s) {
    return s->len == L_W ? (size_t)s->bits / 8 : fast_size(s->bits);
}

/* The length modifier at *pp, read as printf (scan 0) or scanf (scan 1) reads it — a platform's vendor length can
 * depend on which (the UCRT's `I`). Returns 0 on a C23 wN/wfN whose N is none of 8/16/32/64 — no such type here: the
 * specification is handed to the platform verbatim. */
static int parse_length(const char **pp, struct dss_spec *s, int scan) {
    const char *p = *pp;
    s->lenBegin = p;
    s->len = L_NONE;
    s->lenArg = A_NONE;
    if (p[0] == 'h' && p[1] == 'h') { s->len = L_HH; p += 2; }
    else if (p[0] == 'h') { s->len = L_H; p += 1; }
    else if (p[0] == 'l' && p[1] == 'l') { s->len = L_LL; p += 2; }
    else if (p[0] == 'l') { s->len = L_L; p += 1; }
    else if (p[0] == 'j') { s->len = L_J; p += 1; }
    else if (p[0] == 'z') { s->len = L_Z; p += 1; }
    else if (p[0] == 't') { s->len = L_T; p += 1; }
    else if (p[0] == 'L') { s->len = L_BIGL; p += 1; }
    else if (p[0] == 'w' && (is_digit(p[1]) || (p[1] == 'f' && is_digit(p[2])))) {
        /* C23's wN / wfN — told apart from a vendor `w` by what follows it: a digit, or `f` and a digit. */
        int const fast = (p[1] == 'f');
        const char *q = p + (fast ? 2 : 1);
        long long const n = read_number(&q);
        if (n != 8 && n != 16 && n != 32 && n != 64) {
            s->lenEnd = q;
            *pp = q;
            return 0;
        }
        s->len = fast ? L_WF : L_W;
        s->bits = (int)n;
        p = q;
    } else {
        char cls = 0;
        int const used = __dss_platform_length_extension(p, scan, &cls);
        if (used > 0) {
            s->len = L_EXT;
            s->lenArg = class_of_letter(cls);
            p += used;
        }
    }
    s->lenEnd = p;
    *pp = p;
    return 1;
}

static int integer_class(struct dss_spec const *s) {
    switch (s->len) {
        case L_NONE: case L_HH: case L_H: return A_INT;
        case L_L: return A_LONG;
        case L_LL: return A_LLONG;
        case L_J: return A_INTMAX;
        case L_Z: return A_SIZE;
        case L_T: return A_PTRDIFF;
        case L_W: case L_WF: return class_of_size(w_size(s));
        case L_EXT: return s->lenArg;
        default: return A_NONE;
    }
}

/* One printf conversion specification at p ('%'). Returns 0 for text the engine cannot classify — handed to the
 * platform verbatim, consuming no argument — with s->end one past what was read. */
static int parse_printf_spec(const char *p, struct dss_spec *s, int positional) {
    memset(s, 0, sizeof *s);
    s->begin = p;
    s->prec = -1;
    const char *q = p + 1;
    if (*q == '%') {
        s->conv = '%';
        s->end = q + 1;
        s->known = 1;
        return 1;
    }
    s->argPos = read_position(&q, positional);
    s->flagsBegin = q;
    for (;; ++q) {
        if (*q == '-') s->flags |= F_MINUS;
        else if (*q == '+') s->flags |= F_PLUS;
        else if (*q == ' ') s->flags |= F_SPACE;
        else if (*q == '#') s->flags |= F_HASH;
        else if (*q == '0') s->flags |= F_ZERO;
        else if (*q == '\'') { /* a vendor grouping flag: the platform's, passed through */ }
        else break;
    }
    s->flagsEnd = q;
    s->widthBegin = s->widthEnd = q;
    if (*q == '*') {
        ++q;
        s->hasWidth = 1;
        s->widthStar = 1;
        s->widthPos = read_position(&q, positional);
    } else if (is_digit(*q)) {
        s->hasWidth = 1;
        s->width = read_number(&q);
        s->widthEnd = q;
    }
    if (*q == '.') {
        ++q;
        s->precBegin = s->precEnd = q;
        if (*q == '*') {
            ++q;
            s->precStar = 1;
            s->precPos = read_position(&q, positional);
        } else {
            s->prec = read_number(&q);
            s->precEnd = q;
        }
    }
    s->arg = A_NONE;
    if (!parse_length(&q, s, 0)) {
        s->end = q;
        return 0;
    }
    if (*q == '\0') {
        s->end = q;
        return 0;
    }
    s->conv = *q;
    s->end = q + 1;
    switch (s->conv) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'b': case 'B':
            s->arg = integer_class(s);
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            s->arg = (s->len == L_BIGL) ? A_LDOUBLE : A_DOUBLE;
            break;
        case 'c':
            s->arg = A_INT;
            break;
        case 's': case 'p': case 'n':
            s->arg = A_PTR;
            break;
        default: {
            char cls = 0;
            if (!__dss_platform_conversion_extension(s->conv, &cls)) return 0;   /* unknown: verbatim */
            s->arg = class_of_letter(cls);
            break;
        }
    }
    if (s->arg == A_NONE) return 0;
    s->known = 1;
    return 1;
}

static int printf_spec_is_c23(struct dss_spec const *s, int performsN) {
    if (!s->known) return 0;
    if (s->conv == 'b' || s->conv == 'B') return 1;
    if (s->len == L_W || s->len == L_WF) return 1;
    if (s->conv == 'n' && !performsN) return 1;
    return 0;
}

/* ── the positional plan: every argument typed from the format before any is fetched ──────────────────────── */
enum { DSS_MAX_POSITIONS = 64 };

struct dss_plan {
    int need;                          /* the format holds a specification the platform lacks */
    int positional;                    /* ... and uses `n$` */
    int renderable;                    /* the positions are complete and consistent (else: the platform's) */
    int highest;
    int cls[DSS_MAX_POSITIONS + 1];
};

static void plan_printf(const char *format, struct dss_plan *plan) {
    int const performsN = __dss_platform_printf_performs_n();
    int const positional = __dss_platform_printf_positional();
    memset(plan, 0, sizeof *plan);
    plan->renderable = 1;
    int sequential = 0;
    for (const char *p = format; *p != '\0';) {
        if (*p != '%') {
            ++p;
            continue;
        }
        struct dss_spec s;
        int const ok = parse_printf_spec(p, &s, positional);
        if (printf_spec_is_c23(&s, performsN)) plan->need = 1;
        if (ok && s.conv != '%') {
            if (s.argPos == 0) {
                sequential = 1;
            } else {
                plan->positional = 1;
                int const pos[3] = {s.argPos, s.widthStar ? s.widthPos : 0, s.precStar ? s.precPos : 0};
                int const cls[3] = {s.arg, A_INT, A_INT};
                for (int k = 0; k < 3; ++k) {
                    if (k > 0 && pos[k] == 0) {
                        if ((k == 1 && s.widthStar) || (k == 2 && s.precStar)) plan->renderable = 0;
                        continue;
                    }
                    if (pos[k] > DSS_MAX_POSITIONS) {
                        plan->renderable = 0;
                        continue;
                    }
                    if (plan->cls[pos[k]] != A_NONE && plan->cls[pos[k]] != cls[k]) plan->renderable = 0;
                    plan->cls[pos[k]] = cls[k];
                    if (pos[k] > plan->highest) plan->highest = pos[k];
                }
            }
        }
        p = s.end > p ? s.end : p + 1;
    }
    if (plan->positional) {
        if (sequential) plan->renderable = 0;   /* mixed: undefined — the platform's */
        for (int k = 1; k <= plan->highest; ++k) {
            if (plan->cls[k] == A_NONE) plan->renderable = 0;   /* a gap: undefined — the platform's */
        }
    }
}

/* ── output sinks ─────────────────────────────────────────────────────────────────────────────────────────── */
struct dss_sink {
    FILE  *stream;      /* a stream sink, or NULL for a buffer */
    char  *buf;
    size_t cap;         /* the buffer's size (bounded), or SIZE_MAX (sprintf: unbounded) */
    size_t pos;         /* characters produced so far: the would-be length */
    int    failed;
};

static int sink_vemit(struct dss_sink *s, const char *fmt, va_list ap) {
    int r;
    if (s->stream != NULL) {
        r = __dss_platform_vfprintf(s->stream, fmt, ap);
    } else if (s->cap == SIZE_MAX) {
        r = __dss_platform_vsprintf(s->buf + s->pos, fmt, ap);
    } else if (s->pos < s->cap) {
        r = __dss_platform_vsnprintf(s->buf + s->pos, s->cap - s->pos, fmt, ap);
    } else {
        r = __dss_platform_vsnprintf(NULL, 0, fmt, ap);   /* past the bound: count only */
    }
    if (r < 0) {
        s->failed = 1;
        return -1;
    }
    s->pos += (size_t)r;
    return r;
}

static int sink_emit(struct dss_sink *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = sink_vemit(s, fmt, ap);
    va_end(ap);
    return r;
}

static void sink_text(struct dss_sink *s, const char *text, size_t n) {
    while (n > 0 && !s->failed) {
        int const piece = n > 0x7fffffff ? 0x7fffffff : (int)n;
        (void)sink_emit(s, "%.*s", piece, text);
        text += piece;
        n -= (size_t)piece;
    }
}

static void sink_pad(struct dss_sink *s, char c, int n) {
    static const char zeros[] = "0000000000000000000000000000000000000000000000000000000000000000";
    while (n > 0 && !s->failed) {
        int const piece = n > 64 ? 64 : n;
        if (c == '0') (void)sink_emit(s, "%.*s", piece, zeros);
        else (void)sink_emit(s, "%*s", piece, "");
        n -= piece;
    }
}

/* Room for an n-byte piece: `local` when it fits, the heap otherwise — no specification is too long to hand to the
 * platform whole. NULL: no memory (malloc has set errno). */
static char *piece_room(char *local, size_t localSize, size_t n) {
    return n <= localSize ? local : (char *)malloc(n);
}

static void piece_done(char *piece, char const *local) {
    if (piece != local) free(piece);
}

static char *copy_text(char *d, const char *from, const char *to) {
    size_t const n = (size_t)(to - from);
    memcpy(d, from, n);
    return d + n;
}

/* An unknown specification: its text handed to the platform as a format, with no argument, whatever its length — so
 * it prints as the platform prints it when called directly (✔MEASURED for a hundred-flag `%y`: glibc its canonical
 * `%-y`, the UCRT and libSystem `y`), never as the engine's literal copy. */
static void sink_verbatim(struct dss_sink *s, const char *from, const char *to) {
    size_t const n = (size_t)(to - from);
    char local[64];
    char *const piece = piece_room(local, sizeof local, n + 1);
    if (piece == NULL) {
        s->failed = 1;
        return;
    }
    *copy_text(piece, from, to) = '\0';
    (void)sink_emit(s, piece);
    piece_done(piece, local);
}

/* ── the values the engine renders or stores ──────────────────────────────────────────────────────────────── */
union dss_value {
    int         i;
    long        l;
    long long   ll;
    intmax_t    j;
    size_t      z;
    ptrdiff_t   t;
    double      d;
    long double ld;
    void       *p;
};

static uintmax_t unsigned_of(struct dss_spec const *s, union dss_value v) {
    switch (s->len) {
        case L_HH: return (unsigned char)v.i;
        case L_H: return (unsigned short)v.i;
        case L_L: return (unsigned long)v.l;
        case L_LL: return (unsigned long long)v.ll;
        case L_J: return (uintmax_t)v.j;
        case L_Z: return v.z;
        case L_T: return (uintmax_t)v.t;
        case L_W:
        case L_WF: {
            size_t const size = w_size(s);
            int const cls = class_of_size(size);
            uintmax_t const raw = cls == A_INT ? (uintmax_t)(unsigned int)v.i
                                : cls == A_LONG ? (uintmax_t)(unsigned long)v.l
                                                : (uintmax_t)(unsigned long long)v.ll;
            return size >= sizeof(uintmax_t) ? raw : (raw & ((((uintmax_t)1) << (size * 8)) - 1));
        }
        case L_EXT:
            switch (s->lenArg) {
                case A_LONG: return (unsigned long)v.l;
                case A_LLONG: return (unsigned long long)v.ll;
                case A_INTMAX: return (uintmax_t)v.j;
                case A_SIZE: return v.z;
                case A_PTRDIFF: return (uintmax_t)v.t;
                default: return (unsigned int)v.i;
            }
        default: return (unsigned int)v.i;
    }
}

/* C23 7.23.6.1's `b` / `B`: an unsigned value in binary. Precision is the minimum number of digits (default 1; a
 * zero value at precision 0 has none), `#` prefixes `0b` / `0B` to a NONZERO value, `0` pads after the prefix
 * unless `-` or a precision is present, and the width pads with spaces (a negative `*` width is `-` plus its
 * magnitude, C 7.23.6.1p5 — INT_MIN's magnitude 2^31 included, computed unsigned). A conversion that would carry the
 * call's count past INT_MAX fails the call, -1 with EOVERFLOW and none of it emitted: what glibc's own %b and every
 * reference's %d do for a `*` width of INT_MIN or INT_MIN + 1 and for a ten-digit width or precision (✔MEASURED,
 * glibc and libSystem with EOVERFLOW, the UCRT with -1). */
static void render_binary(struct dss_sink *s, struct dss_spec const *sp, long long width, long long prec,
                          uintmax_t value) {
    char digits[sizeof(uintmax_t) * CHAR_BIT];
    int nd = 0;
    for (uintmax_t x = value; x != 0; x >>= 1) digits[nd++] = (char)('0' + (int)(x & 1u));
    unsigned long long const minDigits = prec < 0 ? 1u : (unsigned long long)prec;
    unsigned long long const zeros = minDigits > (unsigned long long)nd ? minDigits - (unsigned long long)nd : 0u;
    const char *const prefix = (sp->flags & F_HASH) && value != 0 ? (sp->conv == 'B' ? "0B" : "0b") : "";
    int const plen = (int)strlen(prefix);
    unsigned long long const body = (unsigned long long)plen + zeros + (unsigned long long)nd;
    unsigned flags = sp->flags;
    unsigned long long field = 0;
    if (sp->hasWidth) {
        if (width < 0) {
            flags |= F_MINUS;
            field = 0u - (unsigned long long)width;
        } else {
            field = (unsigned long long)width;
        }
    }
    unsigned long long const total = field > body ? field : body;
    if (s->pos > (size_t)INT_MAX || total > (unsigned long long)INT_MAX - (unsigned long long)s->pos) {
        errno = EOVERFLOW;
        s->failed = 1;
        return;
    }
    int const pad = (int)(total - body);
    int const zeroPad = (flags & F_ZERO) && !(flags & F_MINUS) && prec < 0;
    if (!(flags & F_MINUS) && !zeroPad) sink_pad(s, ' ', pad);
    sink_text(s, prefix, (size_t)plen);
    if (zeroPad) sink_pad(s, '0', pad);
    sink_pad(s, '0', (int)zeros);
    char out[sizeof(uintmax_t) * CHAR_BIT];
    for (int k = 0; k < nd; ++k) out[k] = digits[nd - 1 - k];
    sink_text(s, out, (size_t)nd);
    if (flags & F_MINUS) sink_pad(s, ' ', pad);
}

/* A vendor length's integer object, by the class its platform unit names (A_NONE: the conversion's own int) —
 * modulo the type's width, as strtoul stores. One owner for %n (printf's and scanf's) and scanf's integer stores. */
static void store_by_class(void *p, int cls, uintmax_t v) {
    switch (cls) {
        case A_LONG: *(unsigned long *)p = (unsigned long)v; return;
        case A_LLONG: *(unsigned long long *)p = (unsigned long long)v; return;
        case A_INTMAX: *(uintmax_t *)p = v; return;
        case A_SIZE: *(size_t *)p = (size_t)v; return;
        case A_PTRDIFF: *(ptrdiff_t *)p = (ptrdiff_t)v; return;
        default: *(unsigned int *)p = (unsigned int)v; return;
    }
}

/* C 7.23.6.1's `n`: the count so far, stored through a pointer of the length's type — a vendor length's included
 * (`%I64n` and `%qn` store a long long; the UCRT's `I` before `n` names no length, so `%In` stores an int, as the
 * UCRT's own does: ✔MEASURED, and 📄 its parser). */
static void store_count(struct dss_spec const *s, void *p, long long count) {
    if (p == NULL) return;
    switch (s->len) {
        case L_HH: *(signed char *)p = (signed char)count; return;
        case L_H: *(short *)p = (short)count; return;
        case L_L: *(long *)p = (long)count; return;
        case L_LL: *(long long *)p = count; return;
        case L_J: *(intmax_t *)p = (intmax_t)count; return;
        case L_Z: *(size_t *)p = (size_t)count; return;
        case L_T: *(ptrdiff_t *)p = (ptrdiff_t)count; return;
        case L_W:
        case L_WF: {
            size_t const size = w_size(s);
            if (size == 1) *(int8_t *)p = (int8_t)count;
            else if (size == 2) *(int16_t *)p = (int16_t)count;
            else if (size == 4) *(int32_t *)p = (int32_t)count;
            else *(int64_t *)p = (int64_t)count;
            return;
        }
        case L_EXT: store_by_class(p, s->lenArg, (uintmax_t)count); return;
        default: *(int *)p = (int)count; return;
    }
}

/* The platform's own length modifier for a wN / wfN type: <inttypes.h>'s PRI macro (printf) or SCN macro (scanf)
 * minus its last character (the conversion). The macros are the one owner of that fact on every pair. */
static const char *platform_modifier(int len, int bits, int scan) {
    static const char w8[] = PRId8, w16[] = PRId16, w32[] = PRId32, w64[] = PRId64;
    static const char f8[] = PRIdFAST8, f16[] = PRIdFAST16, f32[] = PRIdFAST32, f64[] = PRIdFAST64;
    static const char sw8[] = SCNd8, sw16[] = SCNd16, sw32[] = SCNd32, sw64[] = SCNd64;
    static const char sf8[] = SCNdFAST8, sf16[] = SCNdFAST16, sf32[] = SCNdFAST32, sf64[] = SCNdFAST64;
    if (scan) {
        if (len == L_W) return bits == 8 ? sw8 : bits == 16 ? sw16 : bits == 32 ? sw32 : sw64;
        return bits == 8 ? sf8 : bits == 16 ? sf16 : bits == 32 ? sf32 : sf64;
    }
    if (len == L_W) return bits == 8 ? w8 : bits == 16 ? w16 : bits == 32 ? w32 : w64;
    return bits == 8 ? f8 : bits == 16 ? f16 : bits == 32 ? f32 : f64;
}

/* The length text put_length writes: a C23 wN/wfN as the platform's own modifier, every other length verbatim. */
static size_t length_text_size(struct dss_spec const *s, int scan) {
    if (s->len == L_W || s->len == L_WF) return strlen(platform_modifier(s->len, s->bits, scan)) - 1;
    return (size_t)(s->lenEnd - s->lenBegin);
}

static char *put_length(char *d, struct dss_spec const *s, int scan) {
    if (s->len == L_W || s->len == L_WF) {
        const char *m = platform_modifier(s->len, s->bits, scan);
        size_t const n = strlen(m) - 1;
        memcpy(d, m, n);
        return d + n;
    }
    return copy_text(d, s->lenBegin, s->lenEnd);
}

/* The size of the one-specification format build_piece writes for s, its NUL included. */
static size_t piece_size(struct dss_spec const *s) {
    size_t n = 1 + (size_t)(s->flagsEnd - s->flagsBegin);
    n += s->widthStar ? 1 : (size_t)(s->widthEnd - s->widthBegin);
    if (s->precStar || s->prec >= 0) n += 1 + (s->precStar ? 1 : (size_t)(s->precEnd - s->precBegin));
    return n + length_text_size(s, 0) + 2;   /* the conversion and the NUL */
}

/* The one-specification format the platform renders: the spec's OWN text — every flag, the width's and the
 * precision's digits verbatim — with each `n$` dropped, a `*` kept as `*` (render_spec passes the fetched int after
 * it, so the platform applies its own rule to a negative or INT_MIN value, as when it is called directly), and a C23
 * length rewritten to the platform's. `out` holds piece_size(s) bytes. */
static void build_piece(char *out, struct dss_spec const *s) {
    char *d = out;
    *d++ = '%';
    d = copy_text(d, s->flagsBegin, s->flagsEnd);
    if (s->widthStar) *d++ = '*';
    else d = copy_text(d, s->widthBegin, s->widthEnd);
    if (s->precStar || s->prec >= 0) {
        *d++ = '.';
        if (s->precStar) *d++ = '*';
        else d = copy_text(d, s->precBegin, s->precEnd);
    }
    d = put_length(d, s, 0);
    *d++ = s->conv;
    *d = '\0';
}

/* One piece through the sink, its `*` operands first, in the specification's order (C 7.23.6.1p5). */
#define DSS_EMIT_PIECE(V)                                                                                    \
    (s->widthStar && s->precStar ? sink_emit(sink, piece, starWidth, starPrec, V)                           \
     : s->widthStar              ? sink_emit(sink, piece, starWidth, V)                                     \
     : s->precStar               ? sink_emit(sink, piece, starPrec, V)                                      \
                                 : sink_emit(sink, piece, V))

/* starWidth / starPrec: the int arguments a `*` width / precision fetched (unused without one). */
static void render_spec(struct dss_sink *sink, struct dss_spec const *s, int starWidth, int starPrec,
                        union dss_value v) {
    if (s->conv == 'n') {
        store_count(s, v.p, (long long)sink->pos);
        return;
    }
    if (s->conv == 'b' || s->conv == 'B') {
        long long const width = s->widthStar ? starWidth : s->width;
        long long const prec = s->precStar ? (starPrec < 0 ? -1 : starPrec) : s->prec;   /* negative: absent */
        render_binary(sink, s, width, prec, unsigned_of(s, v));
        return;
    }
    char local[64];
    char *const piece = piece_room(local, sizeof local, piece_size(s));
    if (piece == NULL) {
        sink->failed = 1;
        return;
    }
    build_piece(piece, s);
    switch (s->arg) {
        case A_INT: (void)DSS_EMIT_PIECE(v.i); break;
        case A_LONG: (void)DSS_EMIT_PIECE(v.l); break;
        case A_LLONG: (void)DSS_EMIT_PIECE(v.ll); break;
        case A_INTMAX: (void)DSS_EMIT_PIECE(v.j); break;
        case A_SIZE: (void)DSS_EMIT_PIECE(v.z); break;
        case A_PTRDIFF: (void)DSS_EMIT_PIECE(v.t); break;
        case A_DOUBLE: (void)DSS_EMIT_PIECE(v.d); break;
        case A_LDOUBLE: (void)DSS_EMIT_PIECE(v.ld); break;
        default: (void)DSS_EMIT_PIECE(v.p); break;
    }
    piece_done(piece, local);
}

#undef DSS_EMIT_PIECE

/* Walk the format and render it; `slots` holds the positional arguments already fetched, or is NULL (sequential:
 * each argument is fetched from `ap` where its specification stands). */
static void walk_printf(struct dss_sink *sink, const char *format, va_list ap, union dss_value const *slots) {
    int const positional = __dss_platform_printf_positional();
    const char *text = format;
    for (const char *p = format; *p != '\0' && !sink->failed;) {
        if (*p != '%') {
            ++p;
            continue;
        }
        sink_text(sink, text, (size_t)(p - text));
        struct dss_spec s;
        int const ok = parse_printf_spec(p, &s, positional);
        if (!ok) {
            sink_verbatim(sink, p, s.end > p ? s.end : p + 1);
        } else if (s.conv == '%') {
            (void)sink_emit(sink, "%%");
        } else {
            int starWidth = 0;
            int starPrec = 0;
            union dss_value v;
            if (slots != NULL) {
                if (s.widthStar) starWidth = slots[s.widthPos].i;
                if (s.precStar) starPrec = slots[s.precPos].i;
                v = slots[s.argPos];
            } else {
                if (s.widthStar) starWidth = va_arg(ap, int);
                if (s.precStar) starPrec = va_arg(ap, int);
                switch (s.arg) {
                    case A_INT: v.i = va_arg(ap, int); break;
                    case A_LONG: v.l = va_arg(ap, long); break;
                    case A_LLONG: v.ll = va_arg(ap, long long); break;
                    case A_INTMAX: v.j = va_arg(ap, intmax_t); break;
                    case A_SIZE: v.z = va_arg(ap, size_t); break;
                    case A_PTRDIFF: v.t = va_arg(ap, ptrdiff_t); break;
                    case A_DOUBLE: v.d = va_arg(ap, double); break;
                    case A_LDOUBLE: v.ld = va_arg(ap, long double); break;
                    default: v.p = va_arg(ap, void *); break;
                }
            }
            render_spec(sink, &s, starWidth, starPrec, v);
        }
        p = s.end > p ? s.end : p + 1;
        text = p;
    }
    sink_text(sink, text, strlen(text));
}

/* The engine for a whole call, on a plan that needs it and is renderable. A FILE sink holds the stream's lock across
 * the pieces, so one call is one atomic write, as the platform's own printf is. */
static int engine_vprintf(struct dss_sink *sink, const char *format, va_list ap, struct dss_plan const *plan) {
    union dss_value slots[DSS_MAX_POSITIONS + 1];
    if (plan->positional) {
        for (int k = 1; k <= plan->highest; ++k) {
            switch (plan->cls[k]) {
                case A_INT: slots[k].i = va_arg(ap, int); break;
                case A_LONG: slots[k].l = va_arg(ap, long); break;
                case A_LLONG: slots[k].ll = va_arg(ap, long long); break;
                case A_INTMAX: slots[k].j = va_arg(ap, intmax_t); break;
                case A_SIZE: slots[k].z = va_arg(ap, size_t); break;
                case A_PTRDIFF: slots[k].t = va_arg(ap, ptrdiff_t); break;
                case A_DOUBLE: slots[k].d = va_arg(ap, double); break;
                case A_LDOUBLE: slots[k].ld = va_arg(ap, long double); break;
                default: slots[k].p = va_arg(ap, void *); break;
            }
        }
    }
    if (sink->stream != NULL) __dss_platform_lock_file(sink->stream);
    walk_printf(sink, format, ap, plan->positional ? slots : NULL);
    if (sink->stream != NULL) __dss_platform_unlock_file(sink->stream);
    if (sink->failed) return -1;
    if (sink->pos > (size_t)INT_MAX) {
        errno = EOVERFLOW;   /* the count is not an int: POSIX's EOVERFLOW, as glibc's and libSystem's */
        return -1;
    }
    return (int)sink->pos;
}

/* ── the printf family ────────────────────────────────────────────────────────────────────────────────────── */
int __dss_isoc23_vfprintf(FILE *stream, const char *format, va_list ap) {
    struct dss_plan plan;
    plan_printf(format, &plan);
    if (!plan.need || !plan.renderable) return __dss_platform_vfprintf(stream, format, ap);
    struct dss_sink sink = {stream, NULL, 0, 0, 0};
    return engine_vprintf(&sink, format, ap, &plan);
}

int __dss_isoc23_vprintf(const char *format, va_list ap) {
    return __dss_isoc23_vfprintf(stdout, format, ap);
}

int __dss_isoc23_fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int const written = __dss_isoc23_vfprintf(stream, format, ap);
    va_end(ap);
    return written;
}

int __dss_isoc23_printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int const written = __dss_isoc23_vfprintf(stdout, format, ap);
    va_end(ap);
    return written;
}

int __dss_isoc23_vsprintf(char *buffer, const char *format, va_list ap) {
    struct dss_plan plan;
    plan_printf(format, &plan);
    if (!plan.need || !plan.renderable) return __dss_platform_vsprintf(buffer, format, ap);
    struct dss_sink sink = {NULL, buffer, SIZE_MAX, 0, 0};
    buffer[0] = '\0';
    return engine_vprintf(&sink, format, ap, &plan);
}

int __dss_isoc23_sprintf(char *buffer, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int const written = __dss_isoc23_vsprintf(buffer, format, ap);
    va_end(ap);
    return written;
}

int __dss_isoc23_vsnprintf(char *buffer, size_t count, const char *format, va_list ap) {
    struct dss_plan plan;
    plan_printf(format, &plan);
    if (!plan.need || !plan.renderable) return __dss_platform_vsnprintf(buffer, count, format, ap);
    struct dss_sink sink = {NULL, buffer, count, 0, 0};
    if (count > 0) buffer[0] = '\0';
    return engine_vprintf(&sink, format, ap, &plan);
}

int __dss_isoc23_snprintf(char *buffer, size_t count, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int const written = __dss_isoc23_vsnprintf(buffer, count, format, ap);
    va_end(ap);
    return written;
}

/* ── the scanf family ─────────────────────────────────────────────────────────────────────────────────────── */
struct dss_src {
    const char *str;      /* a string source, or NULL */
    size_t      spos;
    FILE       *stream;   /* a stream source, or NULL */
    long long   consumed; /* characters consumed so far: what %n stores */
};

static int src_get(struct dss_src *s) {
    int c;
    if (s->stream != NULL) {
        c = fgetc(s->stream);
    } else {
        c = s->str[s->spos] == '\0' ? EOF : (unsigned char)s->str[s->spos];
        if (c != EOF) ++s->spos;
    }
    if (c != EOF) ++s->consumed;
    return c;
}

static void src_unget(struct dss_src *s, int c) {
    if (c == EOF) return;
    if (s->stream != NULL) (void)ungetc(c, s->stream);
    else --s->spos;
    --s->consumed;
}

static int is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static int skip_space(struct dss_src *s) {
    int c;
    do {
        c = src_get(s);
    } while (c != EOF && is_space(c));
    src_unget(s, c);
    return c;
}

/* One scanf conversion specification at p ('%'). */
static int parse_scanf_spec(const char *p, struct dss_spec *s) {
    memset(s, 0, sizeof *s);
    s->begin = p;
    s->prec = -1;
    const char *q = p + 1;
    if (*q == '%') {
        s->conv = '%';
        s->end = q + 1;
        s->known = 1;
        return 1;
    }
    s->argPos = read_position(&q, 1);
    if (*q == '*') {
        s->suppress = 1;
        ++q;
    }
    if (is_digit(*q)) {
        s->hasWidth = 1;
        s->width = read_number(&q);
    }
    s->arg = A_NONE;
    if (!parse_length(&q, s, 1)) {
        s->end = q;
        return 0;
    }
    if (*q == '\0') {
        s->end = q;
        return 0;
    }
    s->conv = *q;
    if (*q == '[') {
        ++q;
        if (*q == '^') ++q;
        if (*q == ']') ++q;
        while (*q != '\0' && *q != ']') ++q;
        if (*q == '\0') {
            s->end = q;
            return 0;
        }
    }
    s->end = q + 1;
    s->known = 1;
    return 1;
}

static int scanf_format_needs_engine(const char *format) {
    int need = 0;
    for (const char *p = format; *p != '\0';) {
        if (*p != '%') {
            ++p;
            continue;
        }
        struct dss_spec s;
        (void)parse_scanf_spec(p, &s);
        if (s.argPos != 0) return 0;   /* positional scanf: the platform's own */
        if (s.known && (s.conv == 'b' || s.conv == 'i' || s.len == L_W || s.len == L_WF)) need = 1;
        p = s.end > p ? s.end : p + 1;
    }
    return need;
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* Store a converted integer through the length's pointer type (modulo the type's width, as strtoul does). */
static void store_integer(struct dss_spec const *s, void *p, uintmax_t magnitude, int negative) {
    uintmax_t const v = negative ? (uintmax_t)0 - magnitude : magnitude;
    switch (s->len) {
        case L_HH: *(unsigned char *)p = (unsigned char)v; return;
        case L_H: *(unsigned short *)p = (unsigned short)v; return;
        case L_L: *(unsigned long *)p = (unsigned long)v; return;
        case L_LL: *(unsigned long long *)p = (unsigned long long)v; return;
        case L_J: *(uintmax_t *)p = v; return;
        case L_Z: *(size_t *)p = (size_t)v; return;
        case L_T: *(ptrdiff_t *)p = (ptrdiff_t)v; return;
        case L_W:
        case L_WF: {
            size_t const size = w_size(s);
            if (size == 1) *(uint8_t *)p = (uint8_t)v;
            else if (size == 2) *(uint16_t *)p = (uint16_t)v;
            else if (size == 4) *(uint32_t *)p = (uint32_t)v;
            else *(uint64_t *)p = (uint64_t)v;
            return;
        }
        case L_EXT: store_by_class(p, s->lenArg, v); return;
        default: *(unsigned int *)p = (unsigned int)v; return;
    }
}

/* Read one integer item for %i (base 0) or %b (base 2) by C 7.23.6.2p9's longest-prefix rule, bounded by the field
 * width. Returns 1 converted, 0 a matching failure, -1 an input failure (end of input before the item). A base
 * prefix ("0x", "0b") with no digit of its base after it follows the platform's own rule for "0x", measured per
 * platform: the item is the "0" (the prefix letter left in the input), or the directive fails. */
static int read_integer_item(struct dss_src *src, int base, long long width, uintmax_t *magnitude, int *negative) {
    long long budget = width > 0 ? width : LLONG_MAX;
    int any = 0;
    int c = src_get(src);
    *negative = 0;
    *magnitude = 0;
    if (c == EOF) return -1;
    if (c == '+' || c == '-') {
        *negative = (c == '-');
        if (--budget == 0) return 0;
        c = src_get(src);
        if (c == EOF) return 0;
    }
    if (c == '0') {
        any = 1;
        if (--budget == 0) return 1;   /* "0" fills the width */
        int const x = src_get(src);
        int const prefixBase = (x == 'x' || x == 'X') ? 16 : (x == 'b' || x == 'B') ? 2 : 0;
        if (prefixBase != 0 && (base == 0 || base == prefixBase)) {
            int y = EOF;
            if (budget > 1) y = src_get(src);
            if (y != EOF && digit_value(y) < prefixBase) {
                base = prefixBase;
                budget -= 1;   /* the prefix letter */
                c = y;
                any = 0;
            } else if (__dss_platform_prefix_without_digits_is_zero()) {
                src_unget(src, y);
                src_unget(src, x);
                return 1;   /* the item is "0" */
            } else {
                src_unget(src, y);
                return y == EOF && width <= 0 ? -1 : 0;
            }
        } else {
            if (base == 0) base = 8;
            c = x;
        }
    } else if (base == 0) {
        base = 10;
    }
    int pending = 1;   /* `c` was read and is not yet consumed */
    while (c != EOF && digit_value(c) < base) {
        int const dv = digit_value(c);
        if (*magnitude <= (UINTMAX_MAX - (uintmax_t)dv) / (uintmax_t)base) {
            *magnitude = *magnitude * (uintmax_t)base + (uintmax_t)dv;
        } else {
            *magnitude = UINTMAX_MAX;
        }
        any = 1;
        if (--budget == 0) {
            pending = 0;
            break;
        }
        c = src_get(src);
    }
    if (pending) src_unget(src, c);
    return any ? 1 : 0;
}

/* Whether a string source's next item (after white space and a sign) begins with C23's binary prefix `0b` / `0B`.
 * Only such a %i item is read by the engine; every other %i item of a string goes to the platform's own %i, exact by
 * construction. A stream cannot be peeked that far (C guarantees one character of pushback), so a stream's %i items
 * are all read by the engine, under the same rule the platform's %i follows. */
static int string_item_has_binary_prefix(struct dss_src const *src) {
    const unsigned char *s = (const unsigned char *)src->str + src->spos;
    while (is_space(*s)) ++s;
    if (*s == '+' || *s == '-') ++s;
    return s[0] == '0' && (s[1] == 'b' || s[1] == 'B');
}

/* ONE directive handed to the platform's scanf primitive, with its pointer (if any) and a trailing %n target. */
static int platform_scan(struct dss_src *src, const char *piece, ...) {
    va_list ap;
    va_start(ap, piece);
    int r;
    if (src->stream != NULL) r = __dss_platform_vfscanf(src->stream, piece, ap);
    else r = __dss_platform_vsscanf(src->str + src->spos, piece, ap);
    va_end(ap);
    return r;
}

static int engine_vscanf(struct dss_src *src, const char *format, va_list ap) {
    int assigned = 0;
    int completed = 0;   /* a conversion has completed: EOF is returned only before the first */
    for (const char *p = format; *p != '\0';) {
        if (is_space((unsigned char)*p)) {
            while (is_space((unsigned char)*p)) ++p;
            (void)skip_space(src);
            continue;
        }
        if (*p != '%') {
            int const c = src_get(src);
            if (c == EOF) return completed ? assigned : EOF;
            if (c != (unsigned char)*p) {
                src_unget(src, c);
                return assigned;
            }
            ++p;
            continue;
        }
        struct dss_spec s;
        if (!parse_scanf_spec(p, &s)) return assigned;   /* a malformed directive fails */
        p = s.end;
        if (s.conv == '%') {
            if (skip_space(src) == EOF) return completed ? assigned : EOF;
            int const c = src_get(src);
            if (c != '%') {
                src_unget(src, c);
                return assigned;
            }
            continue;
        }
        if (s.conv == 'n') {
            if (!s.suppress) store_count(&s, va_arg(ap, void *), src->consumed);
            continue;
        }
        if (s.conv == 'b' || (s.conv == 'i' && (src->stream != NULL || string_item_has_binary_prefix(src)))) {
            if (skip_space(src) == EOF) return completed ? assigned : EOF;
            uintmax_t magnitude = 0;
            int negative = 0;
            int const r = read_integer_item(src, s.conv == 'b' ? 2 : 0, s.hasWidth ? s.width : 0, &magnitude,
                                            &negative);
            if (r < 0) return completed ? assigned : EOF;
            if (r == 0) return assigned;
            completed = 1;
            if (!s.suppress) {
                store_integer(&s, va_arg(ap, void *), magnitude, negative);
                ++assigned;
            }
            continue;
        }
        /* Everything else: the platform, ONE directive at a time — its own text (`%`, `*` and the width digits
         * verbatim; this path has no `n$`), a C23 length rewritten, the conversion or scanset verbatim — and a trailing
         * %n saying what it read. The piece is as long as the directive: a scanset has no bound. */
        size_t const head = (size_t)(s.lenBegin - s.begin);
        size_t const tail = (size_t)(s.end - s.lenEnd);
        char local[160];
        char *const piece = piece_room(local, sizeof local, head + length_text_size(&s, 1) + tail + 3);
        if (piece == NULL) return EOF;   /* no memory: an error, errno set by malloc */
        char *d = copy_text(piece, s.begin, s.lenBegin);
        d = put_length(d, &s, 1);
        d = copy_text(d, s.lenEnd, s.end);
        memcpy(d, "%n", 3);
        int consumedHere = -1;
        int r;
        if (s.suppress) r = platform_scan(src, piece, &consumedHere);
        else r = platform_scan(src, piece, va_arg(ap, void *), &consumedHere);
        piece_done(piece, local);
        if (r == EOF) return completed ? assigned : EOF;
        if (consumedHere < 0) return assigned;   /* the directive failed: its %n never ran */
        completed = 1;
        if (src->stream == NULL) src->spos += (size_t)consumedHere;
        src->consumed += consumedHere;
        if (!s.suppress) ++assigned;
    }
    return assigned;
}

int __dss_isoc23_vfscanf(FILE *stream, const char *format, va_list ap) {
    if (!scanf_format_needs_engine(format)) return __dss_platform_vfscanf(stream, format, ap);
    struct dss_src src = {NULL, 0, stream, 0};
    __dss_platform_lock_file(stream);
    int const r = engine_vscanf(&src, format, ap);
    __dss_platform_unlock_file(stream);
    return r;
}

int __dss_isoc23_vscanf(const char *format, va_list ap) {
    return __dss_isoc23_vfscanf(stdin, format, ap);
}

int __dss_isoc23_fscanf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int const assigned = __dss_isoc23_vfscanf(stream, format, ap);
    va_end(ap);
    return assigned;
}

int __dss_isoc23_scanf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int const assigned = __dss_isoc23_vfscanf(stdin, format, ap);
    va_end(ap);
    return assigned;
}

int __dss_isoc23_vsscanf(const char *input, const char *format, va_list ap) {
    if (!scanf_format_needs_engine(format)) return __dss_platform_vsscanf(input, format, ap);
    struct dss_src src = {input, 0, NULL, 0};
    return engine_vscanf(&src, format, ap);
}

int __dss_isoc23_sscanf(const char *input, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int const assigned = __dss_isoc23_vsscanf(input, format, ap);
    va_end(ap);
    return assigned;
}
