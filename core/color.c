/* Colour science following the DNG specification: matrix interpolation by
 * illuminant temperature, camera neutral <-> temperature, and the matrix from
 * white-balanced camera values to XYZ (D50).
 *
 * Temperature is unbounded. The Planckian locus uses Krystek's (1985)
 * rational approximation in CIE 1960 uv, accurate to ~1e-4 between 1000 K and
 * 15000 K. A physical locus saturates at both ends (T -> 0 and T -> infinity
 * give finite white points), which is not enough for infrared. Outside
 * [TEMP_LO, TEMP_HI] the camera neutral therefore continues log-linearly in
 * log(T) with the slope at the boundary: every T > 0 gives a distinct,
 * monotonically changing white balance, without any limit. */
#include "raw.h"
#include "srgb_icc.h"
#include "p3_icc.h"
#include "adobergb_icc.h"

typedef struct { double m[9]; } M3;
static const double D50[3] = {0.9642, 1.0, 0.8249};

static M3 m3_mul(const M3 *a, const M3 *b) {
    M3 r;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        r.m[i * 3 + j] = a->m[i * 3] * b->m[j] + a->m[i * 3 + 1] * b->m[3 + j] + a->m[i * 3 + 2] * b->m[6 + j];
    return r;
}
static void m3_vec(const M3 *a, const double *v, double *o) {
    double r[3];
    for (int i = 0; i < 3; i++) r[i] = a->m[i * 3] * v[0] + a->m[i * 3 + 1] * v[1] + a->m[i * 3 + 2] * v[2];
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}
static int m3_inv(const M3 *a, M3 *o) {
    const double *m = a->m;
    double c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
    double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (m_abs(det) < 1e-12) return 0;
    double d = 1.0 / det;
    o->m[0] = c0 * d; o->m[1] = (m[2] * m[7] - m[1] * m[8]) * d; o->m[2] = (m[1] * m[5] - m[2] * m[4]) * d;
    o->m[3] = c1 * d; o->m[4] = (m[0] * m[8] - m[2] * m[6]) * d; o->m[5] = (m[2] * m[3] - m[0] * m[5]) * d;
    o->m[6] = c2 * d; o->m[7] = (m[1] * m[6] - m[0] * m[7]) * d; o->m[8] = (m[0] * m[4] - m[1] * m[3]) * d;
    return 1;
}
static M3 m3_diag(const double *v) { M3 r = {{v[0], 0, 0, 0, v[1], 0, 0, 0, v[2]}}; return r; }
static M3 m3_from(const double *p) { M3 r; for (int i = 0; i < 9; i++) r.m[i] = p[i]; return r; }

/* ---- Planckian locus ---- */
static void locus_uv(double t, double *u, double *v) {
    *u = (0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t * t) / (1 + 8.42420235e-4 * t + 7.08145163e-7 * t * t);
    *v = (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t * t) / (1 - 2.89741816e-5 * t + 1.61456053e-7 * t * t);
}
/* Point at temperature t with signed distance d (Duv) from the locus. */
static void temp_xy(double t, double d, double *x, double *y) {
    double u, v, u1, v1, u2, v2;
    locus_uv(t, &u, &v);
    double tn = CLAMP(t, 500.0, 1.0e6), h = tn * 1e-3;
    locus_uv(tn + h, &u1, &v1); locus_uv(tn - h, &u2, &v2);
    double du = u1 - u2, dv = v1 - v2, len = m_sqrt(du * du + dv * dv);
    if (len > 0) { u += -dv / len * d; v += du / len * d; }
    double den = 2 * u - 8 * v + 4;
    *x = 3 * u / den; *y = 2 * v / den;
}
static void xy_to_xyz(double x, double y, double *xyz) { xyz[0] = x / y; xyz[1] = 1; xyz[2] = (1 - x - y) / y; }

static double illuminant_temp(int ill) {
    switch (ill) {
    case 3: case 17: return 2856; case 2: case 14: return 4150; case 12: return 6430; case 13: return 5000;
    case 15: return 3450; case 16: return 2940; case 18: return 4874; case 19: return 6774; case 20: return 5503;
    case 1: case 4: case 9: return 5500; case 10: return 6500; case 11: return 7500; case 22: return 7504;
    case 23: return 5003; case 24: return 3200; case 21: default: return 6504;
    }
}

/* Profile = colour data of one image in camera channel order. */
typedef struct {
    int n, has_fm;
    double t[2];               /* illuminant temperatures, t[0] <= t[1] */
    M3 cm[2], fm[2], abcc[2];  /* abcc = AnalogBalance * CameraCalibration */
} Profile;

static void profile_from_raw(const Raw *raw, Profile *p) {
    p->n = raw->n_ill < 1 ? 1 : raw->n_ill;
    p->has_fm = raw->has_fm;
    int order[2] = {0, 1};
    if (p->n == 2 && illuminant_temp(raw->ill[0]) > illuminant_temp(raw->ill[1])) { order[0] = 1; order[1] = 0; }
    M3 ab = m3_diag(raw->ab);
    for (int k = 0; k < p->n; k++) {
        int s = order[k];
        p->t[k] = illuminant_temp(raw->ill[s]);
        p->cm[k] = m3_from(raw->cm[s]);
        p->fm[k] = m3_from(raw->fm[s]);
        M3 cc = m3_from(raw->cc[s]);
        p->abcc[k] = m3_mul(&ab, &cc);
    }
}
static double weight(const Profile *p, double t) {
    if (p->n < 2 || p->t[0] == p->t[1]) return 1;
    double g = (1 / t - 1 / p->t[1]) / (1 / p->t[0] - 1 / p->t[1]);
    return CLAMP(g, 0.0, 1.0);
}
static M3 lerp(const M3 *a, const M3 *b, double g) {
    M3 r; for (int i = 0; i < 9; i++) r.m[i] = g * a->m[i] + (1 - g) * b->m[i];
    return r;
}
static M3 xyz_to_camera(const Profile *p, double t) {
    double g = weight(p, t);
    M3 cm = p->n < 2 ? p->cm[0] : lerp(&p->cm[0], &p->cm[1], g);
    M3 abcc = p->n < 2 ? p->abcc[0] : lerp(&p->abcc[0], &p->abcc[1], g);
    return m3_mul(&abcc, &cm);
}
static void normalize3(double *n) {
    double m = MAX(n[0], MAX(n[1], n[2]));
    if (m > 0) for (int i = 0; i < 3; i++) n[i] /= m;
}
/* Camera neutral for an illuminant on (or beside) the locus. 0 if the camera
 * would see a non-positive channel for that light. */
static int model_neutral(const Profile *p, double t, double d, double *n) {
    double x, y, xyz[3];
    temp_xy(t, d, &x, &y);
    if (!(y > 1e-9)) return 0;
    xy_to_xyz(x, y, xyz);
    M3 m = xyz_to_camera(p, t);
    m3_vec(&m, xyz, n);
    if (!(n[0] > 0 && n[1] > 0 && n[2] > 0)) return 0;
    normalize3(n);
    return 1;
}

#define TEMP_LO 1000.0
#define TEMP_HI 15000.0
/* Neutral on the unbounded temperature scale (see top of file). */
static int model_neutral_ext(const Profile *p, double t, double d, double *n) {
    if (t >= TEMP_LO && t <= TEMP_HI) return model_neutral(p, t, d, n);
    double tb = t < TEMP_LO ? TEMP_LO : TEMP_HI, ti = t < TEMP_LO ? TEMP_LO * 1.01 : TEMP_HI / 1.01;
    double a[3], b[3];
    if (!model_neutral(p, tb, d, a) || !model_neutral(p, ti, d, b)) return 0;
    double dl = m_log(t) - m_log(tb), dk = m_log(ti) - m_log(tb), top = -1e300;
    for (int i = 0; i < 3; i++) {
        double la = m_log(a[i]), slope = (m_log(b[i]) - la) / dk;
        n[i] = la + slope * dl;
        if (n[i] > top) top = n[i];
    }
    for (int i = 0; i < 3; i++) {
        double e = n[i] - top;
        n[i] = e < -700 ? 1e-300 : m_exp(e);
    }
    return 1;
}

/* ---- neutral <-> (temperature, tint) fitting ---- */
static double fit_error(const Profile *p, double lt, double d, const double *target) {
    double n[3];
    if (!model_neutral_ext(p, m_exp(lt), d, n)) return 1e30;
    double e1 = m_log(n[0] / n[1]) - m_log(target[0] / target[1]);
    double e2 = m_log(n[2] / n[1]) - m_log(target[2] / target[1]);
    return e1 * e1 + e2 * e2;
}
/* Finds (T, Duv) whose model neutral is closest to `n` (exact when the
 * neutral corresponds to a real illuminant). */
static void fit_neutral(const Profile *p, const double *n, double *t_out, double *d_out) {
    double best = 1e31, blt = m_log(5000), bd = 0;
    for (int i = 0; i <= 400; i++) {                  /* T from 1 K to 1e10 K */
        double lt = i * m_log(1e10) / 400;
        for (int j = -40; j <= 40; j++) {
            double d = j * 0.01, e = fit_error(p, lt, d, n);
            if (e < best) { best = e; blt = lt; bd = d; }
        }
    }
    double slt = 0.03, sd = 0.005;
    for (int it = 0; it < 200 && (slt > 1e-10 || sd > 1e-12); it++) {
        int moved = 0;
        const double steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int k = 0; k < 4; k++) {
            double lt = blt + steps[k][0] * slt, d = bd + steps[k][1] * sd;
            double e = fit_error(p, lt, d, n);
            if (e < best) { best = e; blt = lt; bd = d; moved = 1; }
        }
        if (!moved) { slt *= 0.5; sd *= 0.5; }
    }
    *t_out = m_exp(blt); *d_out = bd;
}

/* Matrix from white-balanced camera values (camera / neutral) to XYZ D50,
 * per DNG spec section 6 ("Mapping Camera Color Space to CIE XYZ Space"). */
static M3 camera_to_pcs(const Profile *p, const double *neutral, double t, double fallback_d) {
    M3 res;
    double g = weight(p, t);
    M3 abcc = p->n < 2 ? p->abcc[0] : lerp(&p->abcc[0], &p->abcc[1], g);
    M3 abcc_inv;
    if (!m3_inv(&abcc, &abcc_inv)) abcc_inv = m3_diag((double[3]){1, 1, 1});
    if (p->has_fm) {
        M3 fm = p->n < 2 ? p->fm[0] : lerp(&p->fm[0], &p->fm[1], g);
        /* Normalise so that [1 1 1] maps to D50 (as the DNG SDK does). */
        double one[3] = {1, 1, 1}, xyz[3];
        m3_vec(&fm, one, xyz);
        for (int i = 0; i < 3; i++) { double s = D50[i] / xyz[i]; for (int j = 0; j < 3; j++) fm.m[i * 3 + j] *= s; }
        double ref[3];
        m3_vec(&abcc_inv, neutral, ref);
        double dinv[3] = {1 / ref[0], 1 / ref[1], 1 / ref[2]};
        M3 dm = m3_diag(dinv), t1 = m3_mul(&fm, &dm);
        res = m3_mul(&t1, &abcc_inv);
    } else {
        M3 x2c = xyz_to_camera(p, t), c2x;
        if (!m3_inv(&x2c, &c2x)) c2x = m3_diag((double[3]){1, 1, 1});
        double w[3];
        m3_vec(&c2x, neutral, w);
        double wx, wy;
        if (w[0] > 0 && w[1] > 0 && w[2] > 0) { double s = w[0] + w[1] + w[2]; wx = w[0] / s; wy = w[1] / s; }
        else temp_xy(t, fallback_d, &wx, &wy);
        /* Bradford adaptation from the scene white to D50. */
        static const M3 B = {{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296}};
        M3 Bi; m3_inv(&B, &Bi);
        double ws[3], wd[3], src[3];
        xy_to_xyz(wx, wy, src);
        m3_vec(&B, src, ws); m3_vec(&B, D50, wd);
        double sc[3] = {wd[0] / ws[0], wd[1] / ws[1], wd[2] / ws[2]};
        M3 S = m3_diag(sc), t1 = m3_mul(&S, &B), ca = m3_mul(&Bi, &t1);
        res = m3_mul(&ca, &c2x);
        /* Scale so the neutral maps to Y = 1. */
        double pcs[3]; m3_vec(&res, neutral, pcs);
        if (pcs[1] > 0) for (int i = 0; i < 9; i++) res.m[i] /= pcs[1];
    }
    /* Fold in the neutral: the result maps camera/neutral to XYZ D50. */
    M3 dn = m3_diag(neutral);
    return m3_mul(&res, &dn);
}

/* XYZ (D65) -> linear RGB for primaries given as xy chromaticities. */
static M3 xyz_to_rgb_primaries(const double *rx, const double *gx, const double *bx, const double *wx) {
    double r[3], g[3], b[3], w[3];
    xy_to_xyz(rx[0], rx[1], r); xy_to_xyz(gx[0], gx[1], g); xy_to_xyz(bx[0], bx[1], b); xy_to_xyz(wx[0], wx[1], w);
    M3 p = {{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]}}, pi, out;
    if (!m3_inv(&p, &pi)) return p;
    double s[3]; m3_vec(&pi, w, s);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) p.m[i * 3 + j] *= s[j];
    m3_inv(&p, &out);
    return out;
}
static M3 xyz_to_display_p3(void) {
    static const double r[2] = {0.680, 0.320}, g[2] = {0.265, 0.690}, b[2] = {0.150, 0.060}, w[2] = {0.3127, 0.3290};
    return xyz_to_rgb_primaries(r, g, b, w);
}

/* ---- output colour spaces, defined by the embedded ICC profiles ----
 * The profiles are the unmodified macOS ones (matrix/TRC). Their colorants
 * (D50) give the RGB <-> XYZ matrices; sRGB and Display P3 use the sRGB
 * transfer curve, Adobe RGB a pure gamma of 563/256 (checked against the
 * profiles' TRC tags in tests/core-check.c). */
enum { SPACE_SRGB = 0, SPACE_P3 = 1, SPACE_ADOBE = 2, SPACE_COUNT = 3 };
typedef struct { const uint8_t *icc; uint32_t icc_len; int gamma563; } OutSpace;
static const OutSpace SPACES[SPACE_COUNT] = {
    {srgb_icc, sizeof srgb_icc, 0}, {p3_icc, sizeof p3_icc, 0}, {adobergb_icc, sizeof adobergb_icc, 1}};

/* Finds an ICC tag; returns its data or NULL. */
static const uint8_t *icc_tag(const uint8_t *p, uint32_t n, const char *sig, uint32_t *size) {
    if (n < 132) return NULL;
    uint32_t count = rd32(p + 128, 1);
    for (uint32_t i = 0; i < count && 132 + 12 * (i + 1) <= n; i++) {
        const uint8_t *e = p + 132 + 12 * i;
        uint32_t off = rd32(e + 4, 1), len = rd32(e + 8, 1);
        if (!xmemcmp(e, sig, 4) && off <= n && len <= n - off) { *size = len; return p + off; }
    }
    return NULL;
}
/* Linear RGB -> XYZ (D50) from the rXYZ/gXYZ/bXYZ colorants. */
static M3 space_to_xyz(int s) {
    static const char *sig[3] = {"rXYZ", "gXYZ", "bXYZ"};
    M3 m = {{0}};
    for (int c = 0; c < 3; c++) {
        uint32_t len = 0;
        const uint8_t *d = icc_tag(SPACES[s].icc, SPACES[s].icc_len, sig[c], &len);
        if (!d || len < 20) return m3_diag((double[3]){1, 1, 1});
        for (int k = 0; k < 3; k++) m.m[k * 3 + c] = (int32_t)rd32(d + 8 + 4 * k, 1) / 65536.0;
    }
    return m;
}
static M3 xyz_to_space(int s) {
    M3 m = space_to_xyz(s), inv;
    if (!m3_inv(&m, &inv)) inv = m3_diag((double[3]){1, 1, 1});
    return inv;
}
/* Exchange of red and blue in the linear RGB of space s, as a map on XYZ. */
static M3 swap_in_space(int s) {
    static const M3 perm = {{0, 0, 1, 0, 1, 0, 1, 0, 0}};
    M3 to = xyz_to_space(s), from = space_to_xyz(s), a = m3_mul(&perm, &to);
    return m3_mul(&from, &a);
}
