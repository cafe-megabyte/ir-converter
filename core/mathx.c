/* Elementary functions without libc, used identically natively and in WASM
 * so that checks compare exactly the same arithmetic. Accuracy ~1e-15. */
#include "base.h"

typedef union { double d; uint64_t u; } F64;
#define LN2_HI 6.93147180369123816490e-01
#define LN2_LO 1.90821492927058770002e-10
#define INV_LN2 1.44269504088896338700e+00

double m_floor(double x) {
    if (!(x == x) || x > 4.5e15 || x < -4.5e15) return x;
    double t = (double)(int64_t)x;
    return t > x ? t - 1.0 : t;
}
double m_exp(double x) {
    if (!(x == x)) return x;
    if (x > 709.78) { F64 inf = {.u = 0x7FF0000000000000ull}; return inf.d; }
    if (x < -745.2) return 0.0;
    double kd = m_floor(x * INV_LN2 + 0.5);
    int k = (int)kd;
    double r = (x - kd * LN2_HI) - kd * LN2_LO;      /* |r| <= 0.35 */
    double p = 1.0 / 6227020800.0;                   /* Taylor to r^13 */
    p = p * r + 1.0 / 479001600.0; p = p * r + 1.0 / 39916800.0; p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;    p = p * r + 1.0 / 40320.0;    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;       p = p * r + 1.0 / 120.0;      p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;         p = p * r + 0.5;              p = p * r + 1.0;
    p = p * r + 1.0;
    /* scale by 2^k in two steps so subnormal results stay representable */
    int k1 = k / 2, k2 = k - k1;
    F64 a = {.u = (uint64_t)(1023 + k1) << 52}, b = {.u = (uint64_t)(1023 + k2) << 52};
    return p * a.d * b.d;
}
double m_expm1(double x) {
    if (x > -0.01 && x < 0.01) {
        double p = 1.0 / 40320.0;
        p = p * x + 1.0 / 5040.0; p = p * x + 1.0 / 720.0; p = p * x + 1.0 / 120.0;
        p = p * x + 1.0 / 24.0;   p = p * x + 1.0 / 6.0;   p = p * x + 0.5; p = p * x + 1.0;
        return p * x;
    }
    return m_exp(x) - 1.0;
}
double m_log(double x) {
    if (!(x > 0)) { F64 n = {.u = x == 0 ? 0xFFF0000000000000ull : 0x7FF8000000000000ull}; return n.d; }
    F64 v = {.d = x};
    int e = (int)((v.u >> 52) & 0x7FF);
    if (e == 0x7FF) return x;
    if (e == 0) { v.d = x * 18014398509481984.0; e = (int)((v.u >> 52) & 0x7FF) - 54; }   /* subnormal */
    e -= 1023;
    v.u = (v.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;   /* m in [1,2) */
    double m = v.d;
    if (m > 1.41421356237309504880) { m *= 0.5; e++; }
    double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    double p = 1.0 / 23.0;
    p = p * s2 + 1.0 / 21.0; p = p * s2 + 1.0 / 19.0; p = p * s2 + 1.0 / 17.0; p = p * s2 + 1.0 / 15.0;
    p = p * s2 + 1.0 / 13.0; p = p * s2 + 1.0 / 11.0; p = p * s2 + 1.0 / 9.0;  p = p * s2 + 1.0 / 7.0;
    p = p * s2 + 1.0 / 5.0;  p = p * s2 + 1.0 / 3.0;  p = p * s2 + 1.0;
    return 2.0 * s * p + e * LN2_HI + e * LN2_LO;
}
double m_pow(double x, double y) {
    if (y == 0) return 1.0;
    if (x <= 0) return 0.0;
    return m_exp(y * m_log(x));
}
