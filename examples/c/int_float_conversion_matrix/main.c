/* THE WHOLE INTEGER <-> FLOATING CONVERSION MATRIX, BIT FOR BIT, IN ONE PROGRAM (P68 round 12).
 *
 * The 34 cells {SIToFP, UIToFP} x {8, 16, 32, 64}-bit source x {float, double}, {FPToSI, FPToUI} x
 * {float, double} x {8, 16, 32, 64}-bit result, and float -> double / double -> float. Each cell reads
 * its inputs through `volatile` objects (no compiler folds the conversion), converts with a plain C
 * cast, and compares the result's BIT PATTERN with the correctly rounded (to nearest, ties to even,
 * ONE rounding) or truncated value the lane's generator computed in exact arithmetic. Float -> integer
 * inputs are all in range. Returns 42 when every cell is right, else the number of the first cell
 * that is not (the list below). */
#include <stdint.h>
#include <string.h>

/*  1  fpext_f32_f64 */
/*  2  fptosi_f32_i16 */
/*  3  fptosi_f32_i32 */
/*  4  fptosi_f32_i64 */
/*  5  fptosi_f32_i8 */
/*  6  fptosi_f64_i16 */
/*  7  fptosi_f64_i32 */
/*  8  fptosi_f64_i64 */
/*  9  fptosi_f64_i8 */
/* 10  fptoui_f32_u16 */
/* 11  fptoui_f32_u32 */
/* 12  fptoui_f32_u64 */
/* 13  fptoui_f32_u8 */
/* 14  fptoui_f64_u16 */
/* 15  fptoui_f64_u32 */
/* 16  fptoui_f64_u64 */
/* 17  fptoui_f64_u8 */
/* 18  fptrunc_f64_f32 */
/* 19  sitofp_i16_f32 */
/* 20  sitofp_i16_f64 */
/* 21  sitofp_i32_f32 */
/* 22  sitofp_i32_f64 */
/* 23  sitofp_i64_f32 */
/* 24  sitofp_i64_f64 */
/* 25  sitofp_i8_f32 */
/* 26  sitofp_i8_f64 */
/* 27  uitofp_u16_f32 */
/* 28  uitofp_u16_f64 */
/* 29  uitofp_u32_f32 */
/* 30  uitofp_u32_f64 */
/* 31  uitofp_u64_f32 */
/* 32  uitofp_u64_f64 */
/* 33  uitofp_u8_f32 */
/* 34  uitofp_u8_f64 */

static const uint32_t fpext_f32_f64_in[] = {0x3dcccccdU, 0x7f7fffffU, 0x00000001U, 0xc0200000U};
static const uint64_t fpext_f32_f64_want[] = {0x3fb99999a0000000ULL, 0x47efffffe0000000ULL, 0x36a0000000000000ULL, 0xc004000000000000ULL};

static int fpext_f32_f64(void) {
    for (int i = 0; i < (int)(sizeof fpext_f32_f64_in / sizeof fpext_f32_f64_in[0]); ++i) {
        volatile uint32_t src = fpext_f32_f64_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != fpext_f32_f64_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t fptosi_f32_i16_in[] = {0xc7000080U, 0x46ffff00U};
static const int16_t fptosi_f32_i16_want[] = {-32768, 32767};

static int fptosi_f32_i16(void) {
    for (int i = 0; i < (int)(sizeof fptosi_f32_i16_in / sizeof fptosi_f32_i16_in[0]); ++i) {
        volatile uint32_t src = fptosi_f32_i16_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        int16_t r = (int16_t)x;
        int16_t got = (int16_t)r;
        if (got != fptosi_f32_i16_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t fptosi_f32_i32_in[] = {0xcf000000U, 0x4effffffU, 0xbfc00000U};
static const int32_t fptosi_f32_i32_want[] = {(-2147483647 - 1), 2147483520, -1};

static int fptosi_f32_i32(void) {
    for (int i = 0; i < (int)(sizeof fptosi_f32_i32_in / sizeof fptosi_f32_i32_in[0]); ++i) {
        volatile uint32_t src = fptosi_f32_i32_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        int32_t r = (int32_t)x;
        int32_t got = (int32_t)r;
        if (got != fptosi_f32_i32_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t fptosi_f32_i64_in[] = {0xdf000000U, 0x5effffffU};
static const int64_t fptosi_f32_i64_want[] = {(-9223372036854775807LL - 1), 9223371487098961920LL};

static int fptosi_f32_i64(void) {
    for (int i = 0; i < (int)(sizeof fptosi_f32_i64_in / sizeof fptosi_f32_i64_in[0]); ++i) {
        volatile uint32_t src = fptosi_f32_i64_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        int64_t r = (int64_t)x;
        int64_t got = (int64_t)r;
        if (got != fptosi_f32_i64_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t fptosi_f32_i8_in[] = {0xc300e000U, 0x42ffc000U, 0xbf600000U};
static const int8_t fptosi_f32_i8_want[] = {-128, 127, 0};

static int fptosi_f32_i8(void) {
    for (int i = 0; i < (int)(sizeof fptosi_f32_i8_in / sizeof fptosi_f32_i8_in[0]); ++i) {
        volatile uint32_t src = fptosi_f32_i8_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        int8_t r = (int8_t)x;
        int8_t got = (int8_t)r;
        if (got != fptosi_f32_i8_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptosi_f64_i16_in[] = {0xc0e0001ccccccccdULL, 0x40dffff99999999aULL};
static const int16_t fptosi_f64_i16_want[] = {-32768, 32767};

static int fptosi_f64_i16(void) {
    for (int i = 0; i < (int)(sizeof fptosi_f64_i16_in / sizeof fptosi_f64_i16_in[0]); ++i) {
        volatile uint64_t src = fptosi_f64_i16_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        int16_t r = (int16_t)x;
        int16_t got = (int16_t)r;
        if (got != fptosi_f64_i16_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptosi_f64_i32_in[] = {0xc1e00000001ccccdULL, 0x41dffffffff9999aULL};
static const int32_t fptosi_f64_i32_want[] = {(-2147483647 - 1), 2147483647};

static int fptosi_f64_i32(void) {
    for (int i = 0; i < (int)(sizeof fptosi_f64_i32_in / sizeof fptosi_f64_i32_in[0]); ++i) {
        volatile uint64_t src = fptosi_f64_i32_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        int32_t r = (int32_t)x;
        int32_t got = (int32_t)r;
        if (got != fptosi_f64_i32_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptosi_f64_i64_in[] = {0xc3e0000000000000ULL, 0x43dfffffffffffffULL};
static const int64_t fptosi_f64_i64_want[] = {(-9223372036854775807LL - 1), 9223372036854774784LL};

static int fptosi_f64_i64(void) {
    for (int i = 0; i < (int)(sizeof fptosi_f64_i64_in / sizeof fptosi_f64_i64_in[0]); ++i) {
        volatile uint64_t src = fptosi_f64_i64_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        int64_t r = (int64_t)x;
        int64_t got = (int64_t)r;
        if (got != fptosi_f64_i64_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptosi_f64_i8_in[] = {0xc0601ccccccccccdULL, 0x405ff9999999999aULL, 0xbfeccccccccccccdULL};
static const int8_t fptosi_f64_i8_want[] = {-128, 127, 0};

static int fptosi_f64_i8(void) {
    for (int i = 0; i < (int)(sizeof fptosi_f64_i8_in / sizeof fptosi_f64_i8_in[0]); ++i) {
        volatile uint64_t src = fptosi_f64_i8_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        int8_t r = (int8_t)x;
        int8_t got = (int8_t)r;
        if (got != fptosi_f64_i8_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t fptoui_f32_u16_in[] = {0x477fffe0U, 0x47000080U};
static const uint16_t fptoui_f32_u16_want[] = {65535U, 32768U};

static int fptoui_f32_u16(void) {
    for (int i = 0; i < (int)(sizeof fptoui_f32_u16_in / sizeof fptoui_f32_u16_in[0]); ++i) {
        volatile uint32_t src = fptoui_f32_u16_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        uint16_t r = (uint16_t)x;
        uint16_t got = (uint16_t)r;
        if (got != fptoui_f32_u16_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t fptoui_f32_u32_in[] = {0x4f7fffffU, 0x4f000000U, 0x40780000U};
static const uint32_t fptoui_f32_u32_want[] = {4294967040U, 2147483648U, 3U};

static int fptoui_f32_u32(void) {
    for (int i = 0; i < (int)(sizeof fptoui_f32_u32_in / sizeof fptoui_f32_u32_in[0]); ++i) {
        volatile uint32_t src = fptoui_f32_u32_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        uint32_t r = (uint32_t)x;
        uint32_t got = (uint32_t)r;
        if (got != fptoui_f32_u32_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t fptoui_f32_u64_in[] = {0x5f7fffffU, 0x5f000000U};
static const uint64_t fptoui_f32_u64_want[] = {18446742974197923840ULL, 9223372036854775808ULL};

static int fptoui_f32_u64(void) {
    for (int i = 0; i < (int)(sizeof fptoui_f32_u64_in / sizeof fptoui_f32_u64_in[0]); ++i) {
        volatile uint32_t src = fptoui_f32_u64_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        uint64_t r = (uint64_t)x;
        uint64_t got = (uint64_t)r;
        if (got != fptoui_f32_u64_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t fptoui_f32_u8_in[] = {0x437fe000U, 0x43008000U};
static const uint8_t fptoui_f32_u8_want[] = {255U, 128U};

static int fptoui_f32_u8(void) {
    for (int i = 0; i < (int)(sizeof fptoui_f32_u8_in / sizeof fptoui_f32_u8_in[0]); ++i) {
        volatile uint32_t src = fptoui_f32_u8_in[i];
        float x;
        uint32_t raw = src;
        memcpy(&x, &raw, sizeof x);
        uint8_t r = (uint8_t)x;
        uint8_t got = (uint8_t)r;
        if (got != fptoui_f32_u8_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptoui_f64_u16_in[] = {0x40effffccccccccdULL, 0x40e0001000000000ULL};
static const uint16_t fptoui_f64_u16_want[] = {65535U, 32768U};

static int fptoui_f64_u16(void) {
    for (int i = 0; i < (int)(sizeof fptoui_f64_u16_in / sizeof fptoui_f64_u16_in[0]); ++i) {
        volatile uint64_t src = fptoui_f64_u16_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        uint16_t r = (uint16_t)x;
        uint16_t got = (uint16_t)r;
        if (got != fptoui_f64_u16_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptoui_f64_u32_in[] = {0x41effffffffccccdULL, 0x41e0000000100000ULL};
static const uint32_t fptoui_f64_u32_want[] = {4294967295U, 2147483648U};

static int fptoui_f64_u32(void) {
    for (int i = 0; i < (int)(sizeof fptoui_f64_u32_in / sizeof fptoui_f64_u32_in[0]); ++i) {
        volatile uint64_t src = fptoui_f64_u32_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        uint32_t r = (uint32_t)x;
        uint32_t got = (uint32_t)r;
        if (got != fptoui_f64_u32_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptoui_f64_u64_in[] = {0x43efffffffffffffULL, 0x43e0000000000000ULL, 0x43e0000000000001ULL};
static const uint64_t fptoui_f64_u64_want[] = {18446744073709549568ULL, 9223372036854775808ULL, 9223372036854777856ULL};

static int fptoui_f64_u64(void) {
    for (int i = 0; i < (int)(sizeof fptoui_f64_u64_in / sizeof fptoui_f64_u64_in[0]); ++i) {
        volatile uint64_t src = fptoui_f64_u64_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        uint64_t r = (uint64_t)x;
        uint64_t got = (uint64_t)r;
        if (got != fptoui_f64_u64_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptoui_f64_u8_in[] = {0x406ffccccccccccdULL, 0x4060100000000000ULL};
static const uint8_t fptoui_f64_u8_want[] = {255U, 128U};

static int fptoui_f64_u8(void) {
    for (int i = 0; i < (int)(sizeof fptoui_f64_u8_in / sizeof fptoui_f64_u8_in[0]); ++i) {
        volatile uint64_t src = fptoui_f64_u8_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        uint8_t r = (uint8_t)x;
        uint8_t got = (uint8_t)r;
        if (got != fptoui_f64_u8_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t fptrunc_f64_f32_in[] = {0x3fb999999999999aULL, 0x3ff0000010000000ULL, 0x3ff0000010000001ULL, 0xbec8000000000000ULL, 0x36a0000000000000ULL};
static const uint32_t fptrunc_f64_f32_want[] = {0x3dcccccdU, 0x3f800000U, 0x3f800001U, 0xb6400000U, 0x00000001U};

static int fptrunc_f64_f32(void) {
    for (int i = 0; i < (int)(sizeof fptrunc_f64_f32_in / sizeof fptrunc_f64_f32_in[0]); ++i) {
        volatile uint64_t src = fptrunc_f64_f32_in[i];
        double x;
        uint64_t raw = src;
        memcpy(&x, &raw, sizeof x);
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != fptrunc_f64_f32_want[i]) return i + 1;
    }
    return 0;
}

static const int16_t sitofp_i16_f32_in[] = {-32768, -1, 32767};
static const uint32_t sitofp_i16_f32_want[] = {0xc7000000U, 0xbf800000U, 0x46fffe00U};

static int sitofp_i16_f32(void) {
    for (int i = 0; i < (int)(sizeof sitofp_i16_f32_in / sizeof sitofp_i16_f32_in[0]); ++i) {
        volatile int16_t src = sitofp_i16_f32_in[i];
        int16_t x = src;
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != sitofp_i16_f32_want[i]) return i + 1;
    }
    return 0;
}

static const int16_t sitofp_i16_f64_in[] = {-32768, -1, 32767};
static const uint64_t sitofp_i16_f64_want[] = {0xc0e0000000000000ULL, 0xbff0000000000000ULL, 0x40dfffc000000000ULL};

static int sitofp_i16_f64(void) {
    for (int i = 0; i < (int)(sizeof sitofp_i16_f64_in / sizeof sitofp_i16_f64_in[0]); ++i) {
        volatile int16_t src = sitofp_i16_f64_in[i];
        int16_t x = src;
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != sitofp_i16_f64_want[i]) return i + 1;
    }
    return 0;
}

static const int32_t sitofp_i32_f32_in[] = {(-2147483647 - 1), 2147483647, 16777217, 16777219, -16777217};
static const uint32_t sitofp_i32_f32_want[] = {0xcf000000U, 0x4f000000U, 0x4b800000U, 0x4b800002U, 0xcb800000U};

static int sitofp_i32_f32(void) {
    for (int i = 0; i < (int)(sizeof sitofp_i32_f32_in / sizeof sitofp_i32_f32_in[0]); ++i) {
        volatile int32_t src = sitofp_i32_f32_in[i];
        int32_t x = src;
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != sitofp_i32_f32_want[i]) return i + 1;
    }
    return 0;
}

static const int32_t sitofp_i32_f64_in[] = {(-2147483647 - 1), 2147483647, 16777217, 16777219, -16777217};
static const uint64_t sitofp_i32_f64_want[] = {0xc1e0000000000000ULL, 0x41dfffffffc00000ULL, 0x4170000010000000ULL, 0x4170000030000000ULL, 0xc170000010000000ULL};

static int sitofp_i32_f64(void) {
    for (int i = 0; i < (int)(sizeof sitofp_i32_f64_in / sizeof sitofp_i32_f64_in[0]); ++i) {
        volatile int32_t src = sitofp_i32_f64_in[i];
        int32_t x = src;
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != sitofp_i32_f64_want[i]) return i + 1;
    }
    return 0;
}

static const int64_t sitofp_i64_f32_in[] = {(-9223372036854775807LL - 1), 9223372036854775807LL, 1152921573326323713LL, -1152921573326323713LL, 9007199254740993LL, 9007199254740995LL};
static const uint32_t sitofp_i64_f32_want[] = {0xdf000000U, 0x5f000000U, 0x5d800001U, 0xdd800001U, 0x5a000000U, 0x5a000000U};

static int sitofp_i64_f32(void) {
    for (int i = 0; i < (int)(sizeof sitofp_i64_f32_in / sizeof sitofp_i64_f32_in[0]); ++i) {
        volatile int64_t src = sitofp_i64_f32_in[i];
        int64_t x = src;
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != sitofp_i64_f32_want[i]) return i + 1;
    }
    return 0;
}

static const int64_t sitofp_i64_f64_in[] = {(-9223372036854775807LL - 1), 9223372036854775807LL, 1152921573326323713LL, -1152921573326323713LL, 9007199254740993LL, 9007199254740995LL};
static const uint64_t sitofp_i64_f64_want[] = {0xc3e0000000000000ULL, 0x43e0000000000000ULL, 0x43b0000010000000ULL, 0xc3b0000010000000ULL, 0x4340000000000000ULL, 0x4340000000000002ULL};

static int sitofp_i64_f64(void) {
    for (int i = 0; i < (int)(sizeof sitofp_i64_f64_in / sizeof sitofp_i64_f64_in[0]); ++i) {
        volatile int64_t src = sitofp_i64_f64_in[i];
        int64_t x = src;
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != sitofp_i64_f64_want[i]) return i + 1;
    }
    return 0;
}

static const int8_t sitofp_i8_f32_in[] = {-128, -1, 0, 127};
static const uint32_t sitofp_i8_f32_want[] = {0xc3000000U, 0xbf800000U, 0x00000000U, 0x42fe0000U};

static int sitofp_i8_f32(void) {
    for (int i = 0; i < (int)(sizeof sitofp_i8_f32_in / sizeof sitofp_i8_f32_in[0]); ++i) {
        volatile int8_t src = sitofp_i8_f32_in[i];
        int8_t x = src;
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != sitofp_i8_f32_want[i]) return i + 1;
    }
    return 0;
}

static const int8_t sitofp_i8_f64_in[] = {-128, -1, 0, 127};
static const uint64_t sitofp_i8_f64_want[] = {0xc060000000000000ULL, 0xbff0000000000000ULL, 0x0000000000000000ULL, 0x405fc00000000000ULL};

static int sitofp_i8_f64(void) {
    for (int i = 0; i < (int)(sizeof sitofp_i8_f64_in / sizeof sitofp_i8_f64_in[0]); ++i) {
        volatile int8_t src = sitofp_i8_f64_in[i];
        int8_t x = src;
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != sitofp_i8_f64_want[i]) return i + 1;
    }
    return 0;
}

static const uint16_t uitofp_u16_f32_in[] = {32768U, 65535U};
static const uint32_t uitofp_u16_f32_want[] = {0x47000000U, 0x477fff00U};

static int uitofp_u16_f32(void) {
    for (int i = 0; i < (int)(sizeof uitofp_u16_f32_in / sizeof uitofp_u16_f32_in[0]); ++i) {
        volatile uint16_t src = uitofp_u16_f32_in[i];
        uint16_t x = src;
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != uitofp_u16_f32_want[i]) return i + 1;
    }
    return 0;
}

static const uint16_t uitofp_u16_f64_in[] = {32768U, 65535U};
static const uint64_t uitofp_u16_f64_want[] = {0x40e0000000000000ULL, 0x40efffe000000000ULL};

static int uitofp_u16_f64(void) {
    for (int i = 0; i < (int)(sizeof uitofp_u16_f64_in / sizeof uitofp_u16_f64_in[0]); ++i) {
        volatile uint16_t src = uitofp_u16_f64_in[i];
        uint16_t x = src;
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != uitofp_u16_f64_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t uitofp_u32_f32_in[] = {2147483648U, 4294967295U, 16777217U, 4294967040U};
static const uint32_t uitofp_u32_f32_want[] = {0x4f000000U, 0x4f800000U, 0x4b800000U, 0x4f7fffffU};

static int uitofp_u32_f32(void) {
    for (int i = 0; i < (int)(sizeof uitofp_u32_f32_in / sizeof uitofp_u32_f32_in[0]); ++i) {
        volatile uint32_t src = uitofp_u32_f32_in[i];
        uint32_t x = src;
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != uitofp_u32_f32_want[i]) return i + 1;
    }
    return 0;
}

static const uint32_t uitofp_u32_f64_in[] = {2147483648U, 4294967295U, 16777217U, 4294967040U};
static const uint64_t uitofp_u32_f64_want[] = {0x41e0000000000000ULL, 0x41efffffffe00000ULL, 0x4170000010000000ULL, 0x41efffffe0000000ULL};

static int uitofp_u32_f64(void) {
    for (int i = 0; i < (int)(sizeof uitofp_u32_f64_in / sizeof uitofp_u32_f64_in[0]); ++i) {
        volatile uint32_t src = uitofp_u32_f64_in[i];
        uint32_t x = src;
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != uitofp_u32_f64_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t uitofp_u64_f32_in[] = {18446744073709551615ULL, 9223372036854775808ULL, 1152921573326323713ULL, 9223372586610589697ULL, 9223372036854776833ULL, 9007199254740993ULL};
static const uint32_t uitofp_u64_f32_want[] = {0x5f800000U, 0x5f000000U, 0x5d800001U, 0x5f000001U, 0x5f000000U, 0x5a000000U};

static int uitofp_u64_f32(void) {
    for (int i = 0; i < (int)(sizeof uitofp_u64_f32_in / sizeof uitofp_u64_f32_in[0]); ++i) {
        volatile uint64_t src = uitofp_u64_f32_in[i];
        uint64_t x = src;
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != uitofp_u64_f32_want[i]) return i + 1;
    }
    return 0;
}

static const uint64_t uitofp_u64_f64_in[] = {18446744073709551615ULL, 9223372036854775808ULL, 1152921573326323713ULL, 9223372586610589697ULL, 9223372036854776833ULL, 9007199254740993ULL};
static const uint64_t uitofp_u64_f64_want[] = {0x43f0000000000000ULL, 0x43e0000000000000ULL, 0x43b0000010000000ULL, 0x43e0000010000000ULL, 0x43e0000000000001ULL, 0x4340000000000000ULL};

static int uitofp_u64_f64(void) {
    for (int i = 0; i < (int)(sizeof uitofp_u64_f64_in / sizeof uitofp_u64_f64_in[0]); ++i) {
        volatile uint64_t src = uitofp_u64_f64_in[i];
        uint64_t x = src;
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != uitofp_u64_f64_want[i]) return i + 1;
    }
    return 0;
}

static const uint8_t uitofp_u8_f32_in[] = {0U, 128U, 255U};
static const uint32_t uitofp_u8_f32_want[] = {0x00000000U, 0x43000000U, 0x437f0000U};

static int uitofp_u8_f32(void) {
    for (int i = 0; i < (int)(sizeof uitofp_u8_f32_in / sizeof uitofp_u8_f32_in[0]); ++i) {
        volatile uint8_t src = uitofp_u8_f32_in[i];
        uint8_t x = src;
        float r = (float)x;
        uint32_t got;
        memcpy(&got, &r, sizeof got);
        if (got != uitofp_u8_f32_want[i]) return i + 1;
    }
    return 0;
}

static const uint8_t uitofp_u8_f64_in[] = {0U, 128U, 255U};
static const uint64_t uitofp_u8_f64_want[] = {0x0000000000000000ULL, 0x4060000000000000ULL, 0x406fe00000000000ULL};

static int uitofp_u8_f64(void) {
    for (int i = 0; i < (int)(sizeof uitofp_u8_f64_in / sizeof uitofp_u8_f64_in[0]); ++i) {
        volatile uint8_t src = uitofp_u8_f64_in[i];
        uint8_t x = src;
        double r = (double)x;
        uint64_t got;
        memcpy(&got, &r, sizeof got);
        if (got != uitofp_u8_f64_want[i]) return i + 1;
    }
    return 0;
}

int main(void) {
    if (fpext_f32_f64() != 0) return 1;
    if (fptosi_f32_i16() != 0) return 2;
    if (fptosi_f32_i32() != 0) return 3;
    if (fptosi_f32_i64() != 0) return 4;
    if (fptosi_f32_i8() != 0) return 5;
    if (fptosi_f64_i16() != 0) return 6;
    if (fptosi_f64_i32() != 0) return 7;
    if (fptosi_f64_i64() != 0) return 8;
    if (fptosi_f64_i8() != 0) return 9;
    if (fptoui_f32_u16() != 0) return 10;
    if (fptoui_f32_u32() != 0) return 11;
    if (fptoui_f32_u64() != 0) return 12;
    if (fptoui_f32_u8() != 0) return 13;
    if (fptoui_f64_u16() != 0) return 14;
    if (fptoui_f64_u32() != 0) return 15;
    if (fptoui_f64_u64() != 0) return 16;
    if (fptoui_f64_u8() != 0) return 17;
    if (fptrunc_f64_f32() != 0) return 18;
    if (sitofp_i16_f32() != 0) return 19;
    if (sitofp_i16_f64() != 0) return 20;
    if (sitofp_i32_f32() != 0) return 21;
    if (sitofp_i32_f64() != 0) return 22;
    if (sitofp_i64_f32() != 0) return 23;
    if (sitofp_i64_f64() != 0) return 24;
    if (sitofp_i8_f32() != 0) return 25;
    if (sitofp_i8_f64() != 0) return 26;
    if (uitofp_u16_f32() != 0) return 27;
    if (uitofp_u16_f64() != 0) return 28;
    if (uitofp_u32_f32() != 0) return 29;
    if (uitofp_u32_f64() != 0) return 30;
    if (uitofp_u64_f32() != 0) return 31;
    if (uitofp_u64_f64() != 0) return 32;
    if (uitofp_u8_f32() != 0) return 33;
    if (uitofp_u8_f64() != 0) return 34;
    return 42;
}
