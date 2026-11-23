/* Compiler support routines for 32-bit MIPS that libgcc would give (there is no libgcc in the
 * build): 64-bit division, which FixedDiv uses. Shift-and-subtract, so the compiler can't turn
 * it back into a call to itself. */
typedef unsigned long long u64;
typedef long long s64;

static u64 udivmod(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    if (!d) return *rem = 0, 0; /* as MIPS's divu: no trap */
    for (int i = 63; i >= 0; --i) {
        r = r << 1 | (n >> i & 1);
        if (r >= d) r -= d, q |= 1ull << i;
    }
    return *rem = r, q;
}

u64 __udivdi3(u64 n, u64 d);
u64 __umoddi3(u64 n, u64 d);
s64 __divdi3(s64 n, s64 d);
s64 __moddi3(s64 n, s64 d);

u64 __udivdi3(u64 n, u64 d) {
    u64 r;
    return udivmod(n, d, &r);
}
u64 __umoddi3(u64 n, u64 d) {
    u64 r;
    udivmod(n, d, &r);
    return r;
}
s64 __divdi3(s64 n, s64 d) {
    u64 r, q = udivmod(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r);
    return (n < 0) != (d < 0) ? -(s64)q : (s64)q;
}
s64 __moddi3(s64 n, s64 d) {
    u64 r;
    udivmod(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r);
    return n < 0 ? -(s64)r : (s64)r;
}

/* 64-bit integers to and from floating point, as libgcc's: in 32-bit halves, so the compiler
 * has nothing 64-bit to turn back into a call. */
static double u64_to_d(u64 a) { return (double)(unsigned)(a >> 32) * 4294967296.0 + (double)(unsigned)a; }
double __floatundidf(u64 a);
double __floatdidf(s64 a);
float __floatundisf(u64 a);
s64 __fixdfdi(double x);
s64 __fixsfdi(float x);
double __floatundidf(u64 a) { return u64_to_d(a); }
double __floatdidf(s64 a) { return a < 0 ? -u64_to_d(-(u64)a) : u64_to_d((u64)a); }
float __floatundisf(u64 a) { return (float)u64_to_d(a); }
static u64 d_to_u64(double x) { /* x >= 0, below 2^64 */
    unsigned hi = (unsigned)(x / 4294967296.0);
    double rest = x - (double)hi * 4294967296.0;
    if (rest < 0) --hi, rest += 4294967296.0;
    return (u64)hi << 32 | (unsigned)rest;
}
s64 __fixdfdi(double x) { return x < 0 ? -(s64)d_to_u64(-x) : (s64)d_to_u64(x); }
s64 __fixsfdi(float x) { return __fixdfdi((double)x); }
