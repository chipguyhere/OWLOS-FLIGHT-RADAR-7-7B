// drawing primitives, everything clips to the current strip (see gfx.h)
#include "gfx.h"
#include <math.h>

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// clip row range to the strip, returns 0 if nothing left
static int clip_rows(const canvas_t *cv, int *ya, int *yb)
{
    if (*ya < cv->y0) *ya = cv->y0;
    if (*yb > cv->y0 + cv->h - 1) *yb = cv->y0 + cv->h - 1;
    return *ya <= *yb;
}

static inline void plot(uint32_t *row, int x, uint32_t c, int a)
{
    if (a >= 256) row[x] = c | GFX_OPAQUE;
    else if (a > 0) row[x] = gfx_mix(row[x], c, (uint32_t)a);
}

CORE_HOT void gfx_fill(canvas_t *cv, int x, int y, int w, int h, uint32_t c, int a)
{
    int x0 = imax(GFX_Z(x), 0), x1 = imin(GFX_Z(x + w), GFX_PW) - 1;
    int ya = GFX_Z(y), yb = GFX_Z(y + h) - 1;
    if (x0 > x1 || !clip_rows(cv, &ya, &yb)) return;
    for (int yy = ya; yy <= yb; yy++) {
        uint32_t *row = gfx_row(cv, yy);
        for (int xx = x0; xx <= x1; xx++) plot(row, xx, c, a);
    }
}

#if GFX_SCALED
/* a line a layout pixel wide is a pixel and a half on the glass: one row full, the one beside it
 * half, the half on the side the layout pixel really covers. two lines side by side then meet */
static void hspan(canvas_t *cv, int x0, int x1, int y, uint32_t c, int a)
{
    uint32_t *row = gfx_row(cv, y);
    if (!row) return;
    for (int x = imax(x0, 0); x <= imin(x1, GFX_PW - 1); x++) plot(row, x, c, a);
}

static void vspan(canvas_t *cv, int x, int y0, int y1, uint32_t c, int a)
{
    if (x < 0 || x >= GFX_PW || !clip_rows(cv, &y0, &y1)) return;
    for (int y = y0; y <= y1; y++) plot(gfx_row(cv, y), x, c, a);
}

/* a layout pixel's run of glass pixels along one axis, from e0 to e1 in 1/2^GFX_SCALE_SHIFT of a
 * glass pixel: the first and last as much as they are covered. fork: the ends used to be rounded
 * down to whole glass pixels, up to 7/8 of one short at 15/8, and the chamfered boxes' cut corners
 * (round-ended lines placed exactly) no longer met their straight sides */
static inline int cover(int g, int e0, int e1)
{
    const int one = 1 << GFX_SCALE_SHIFT, lo = g * one > e0 ? g * one : e0, hi = (g + 1) * one < e1 ? (g + 1) * one : e1;
    return hi > lo ? hi - lo : 0;
}

CORE_HOT void gfx_hline(canvas_t *cv, int x0, int x1, int y, uint32_t c, int a)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    const int f0 = x0 * GFX_SCALE_NUM, f1 = (x1 + 1) * GFX_SCALE_NUM;           // along
    const int xs = f0 >> GFX_SCALE_SHIFT, xe = (f1 - 1) >> GFX_SCALE_SHIFT;
    const int e0 = y * GFX_SCALE_NUM, e1 = e0 + GFX_SCALE_NUM;                 // across
    for (int r = e0 >> GFX_SCALE_SHIFT; r << GFX_SCALE_SHIFT < e1; r++) {
        const int ar = a * cover(r, e0, e1) >> GFX_SCALE_SHIFT;
        if (xs == xe) { hspan(cv, xs, xs, r, c, ar * cover(xs, f0, f1) >> GFX_SCALE_SHIFT); continue; }
        hspan(cv, xs, xs, r, c, ar * cover(xs, f0, f1) >> GFX_SCALE_SHIFT);
        hspan(cv, xs + 1, xe - 1, r, c, ar);
        hspan(cv, xe, xe, r, c, ar * cover(xe, f0, f1) >> GFX_SCALE_SHIFT);
    }
}

CORE_HOT void gfx_vline(canvas_t *cv, int x, int y0, int y1, uint32_t c, int a)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    const int f0 = y0 * GFX_SCALE_NUM, f1 = (y1 + 1) * GFX_SCALE_NUM;           // along
    const int ys = f0 >> GFX_SCALE_SHIFT, ye = (f1 - 1) >> GFX_SCALE_SHIFT;
    const int e0 = x * GFX_SCALE_NUM, e1 = e0 + GFX_SCALE_NUM;                 // across
    for (int r = e0 >> GFX_SCALE_SHIFT; r << GFX_SCALE_SHIFT < e1; r++) {
        const int ar = a * cover(r, e0, e1) >> GFX_SCALE_SHIFT;
        if (ys == ye) { vspan(cv, r, ys, ys, c, ar * cover(ys, f0, f1) >> GFX_SCALE_SHIFT); continue; }
        vspan(cv, r, ys, ys, c, ar * cover(ys, f0, f1) >> GFX_SCALE_SHIFT);
        vspan(cv, r, ys + 1, ye - 1, c, ar);
        vspan(cv, r, ye, ye, c, ar * cover(ye, f0, f1) >> GFX_SCALE_SHIFT);
    }
}
#else
CORE_HOT void gfx_hline(canvas_t *cv, int x0, int x1, int y, uint32_t c, int a)
{
    uint32_t *row = gfx_row(cv, y);
    if (!row) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    x0 = imax(x0, 0); x1 = imin(x1, GFX_W - 1);
    for (int x = x0; x <= x1; x++) plot(row, x, c, a);
}

CORE_HOT void gfx_vline(canvas_t *cv, int x, int y0, int y1, uint32_t c, int a)
{
    if (x < 0 || x >= GFX_W) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (!clip_rows(cv, &y0, &y1)) return;
    for (int y = y0; y <= y1; y++) plot(gfx_row(cv, y), x, c, a);
}
#endif

// diagonal stripes, the "caution tape" bars like on the reference HUDs
CORE_HOT void gfx_hatch(canvas_t *cv, int x, int y, int w, int h, uint32_t c, int a)
{
    int x0 = imax(GFX_Z(x), 0), x1 = imin(GFX_Z(x + w), GFX_PW) - 1;
    int ya = GFX_Z(y), yb = GFX_Z(y + h) - 1;
    if (x0 > x1 || !clip_rows(cv, &ya, &yb)) return;
    for (int yy = ya; yy <= yb; yy++) {
        uint32_t *row = gfx_row(cv, yy);
        for (int xx = x0; xx <= x1; xx++)
            if (GFX_SCALED ? (xx + yy) % 6 < 3 : ((xx + yy) & 3) < 2) plot(row, xx, c, a);
    }
}

uint32_t gfx_isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* the per-pixel work below is in whole numbers, 1/256 of a pixel: the ESP32-C3 has no floating
 * point unit and a float there costs tens of cycles. floats only set things up, once a call */
#define FX 256

/* AA line as a capsule: coverage of a pixel = its distance to the segment. along the line and
 * across it are kept in 1/256 px and step by an add a pixel; only the round ends need a root.
 * each row only walks the span the line can reach, else long steep lines would cost the whole
 * bounding box */
CORE_HOT void gfx_line(canvas_t *cv, float x0, float y0, float x1, float y1, float w, uint32_t c, int a)
{
    x0 = GFX_PX(x0); y0 = GFX_PX(y0); x1 = GFX_PX(x1); y1 = GFX_PX(y1); w = GFX_PL(w);
    float hw = w * 0.5f;
    if (w < 1.0f) { a = (int)(a * w); hw = 0.5f; }   // thinner than 1px: draw 1px but fainter
    if (a <= 0) return;
    float reach = hw + 1.0f;
    int ya = (int)floorf(fminf(y0, y1) - reach), yb = (int)ceilf(fmaxf(y0, y1) + reach);
    if (!clip_rows(cv, &ya, &yb)) return;
    float minx = fminf(x0, x1) - reach, maxx = fmaxf(x0, x1) + reach;
    float dx = x1 - x0, dy = y1 - y0;
    float ady = fabsf(dy), len = gfx_sqrt(dx * dx + dy * dy);
    float ux = len > 1e-3f ? dx / len : 1.0f, uy = len > 1e-3f ? dy / len : 0.0f;
    // along(x,y) = (x-x0)*ux + (y-y0)*uy, across = (x-x0)*-uy + (y-y0)*ux
    const int32_t a_x = (int32_t)lroundf(ux * FX), a_y = (int32_t)lroundf(uy * FX);
    const int32_t a_0 = (int32_t)lroundf(-(x0 * ux + y0 * uy) * FX);
    const int32_t p_x = -a_y, p_y = a_x, p_0 = (int32_t)lroundf((x0 * uy - y0 * ux) * FX);
    const int32_t L = (int32_t)lroundf(len * FX), edge = (int32_t)lroundf((hw + 0.5f) * FX);
    for (int y = ya; y <= yb; y++) {
        float sa = minx, sb = maxx;
        if (ady > 0.5f && ady * 2.0f > fabsf(dx)) {
            // steep: line crosses this row near one x, only look around it
            float t = ((float)y - y0) / dy;
            float xc = x0 + dx * t;
            float half = reach * len / ady + 1.0f;
            sa = fmaxf(minx, xc - half);
            sb = fminf(maxx, xc + half);
        }
        int xa = imax((int)floorf(sa), 0), xb = imin((int)ceilf(sb), GFX_PW - 1);
        uint32_t *row = gfx_row(cv, y);
        int32_t al = a_x * xa + a_y * y + a_0, ac = p_x * xa + p_y * y + p_0;
        for (int x = xa; x <= xb; x++, al += a_x, ac += p_x) {
            int32_t d;
            if (al < 0 || al > L) {                    // past an end: the distance to that end
                uint32_t e = (uint32_t)(al < 0 ? -al : al - L), q = (uint32_t)(ac < 0 ? -ac : ac);
                if (e >= (uint32_t)edge || q >= (uint32_t)edge) continue;   // that far on either axis: no root needed
                d = (int32_t)gfx_isqrt(e * e + q * q);
            } else {
                d = ac < 0 ? -ac : ac;
            }
            int32_t cov = edge - d;
            if (cov <= 0) continue;
            plot(row, x, c, cov >= FX ? a : (a * cov) >> 8);
        }
    }
}

// a soft dot at a sub-pixel spot, shared out over the 2x2 pixels round it: a trail's point
CORE_HOT void gfx_dot(canvas_t *cv, float x, float y, uint32_t c, int a)
{
    x = GFX_PX(x); y = GFX_PX(y);
    int fx = (int)lroundf(x * 16.0f) - 8, fy = (int)lroundf(y * 16.0f) - 8;   // the dot's centre is a pixel's centre
    int ix = fx >> 4, iy = fy >> 4, wx = fx & 15, wy = fy & 15;
    for (int j = 0; j < 2; j++) {
        uint32_t *row = gfx_row(cv, iy + j);
        if (!row) continue;
        int ay = j ? wy : 16 - wy;
        for (int i = 0; i < 2; i++) {
            int px = ix + i;
            if (px < 0 || px >= GFX_PW) continue;
            int k = a * ay * (i ? wx : 16 - wx) >> 8;
            if (k > 0) plot(row, px, c, k);
        }
    }
}

CORE_HOT void gfx_ring(canvas_t *cv, float cx, float cy, float r, float w, uint32_t c, int a)
{
    cx = GFX_PX(cx); cy = GFX_PX(cy); r = GFX_PL(r); w = GFX_PL(w);
    float hw = w * 0.5f;
    if (w < 1.0f) { a = (int)(a * w); hw = 0.5f; }
    float ro = r + hw + 1.0f, ri = r - hw - 1.0f;
    int ya = (int)floorf(cy - ro), yb = (int)ceilf(cy + ro);
    if (!clip_rows(cv, &ya, &yb)) return;
    for (int y = ya; y <= yb; y++) {
        float dy = (float)y - cy;
        if (fabsf(dy) > ro) continue;
        float xo = gfx_sqrt(ro * ro - dy * dy);
        float xi = (ri > 0.0f && fabsf(dy) < ri) ? gfx_sqrt(ri * ri - dy * dy) : -1.0f;
        int xa = imax((int)floorf(cx - xo), 0), xb = imin((int)ceilf(cx + xo), GFX_PW - 1);
        uint32_t *row = gfx_row(cv, y);
        for (int x = xa; x <= xb; x++) {
            float dx = (float)x - cx;
            if (xi > 0.0f && fabsf(dx) < xi - 1.0f) {     // hack: jump over the hole inside
                x = (int)floorf(cx + xi - 1.0f);
                continue;
            }
            float d = gfx_sqrt(dx * dx + dy * dy);
            float cov = hw + 0.5f - fabsf(d - r);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            plot(row, x, c, (int)(a * cov));
        }
    }
}

CORE_HOT void gfx_disc(canvas_t *cv, float cx, float cy, float r, uint32_t c, int a)
{
    cx = GFX_PX(cx); cy = GFX_PX(cy); r = GFX_PL(r);
    float ro = r + 1.0f;
    int ya = (int)floorf(cy - ro), yb = (int)ceilf(cy + ro);
    if (!clip_rows(cv, &ya, &yb)) return;
    for (int y = ya; y <= yb; y++) {
        float dy = (float)y - cy;
        int xa = imax((int)floorf(cx - ro), 0), xb = imin((int)ceilf(cx + ro), GFX_PW - 1);
        uint32_t *row = gfx_row(cv, y);
        for (int x = xa; x <= xb; x++) {
            float dx = (float)x - cx;
            float cov = r + 0.5f - gfx_sqrt(dx * dx + dy * dy);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            plot(row, x, c, (int)(a * cov));
        }
    }
}

// soft additive glow, (1 - d^2/r^2)^2 so no sqrt needed. in 1/16 px, whole numbers
CORE_HOT void gfx_glow(canvas_t *cv, float cx, float cy, float r, uint32_t c, int a)
{
    cx = GFX_PX(cx); cy = GFX_PX(cy); r = GFX_PL(r);
    if (a <= 0 || r <= 0.5f) return;
    int ya = (int)floorf(cy - r), yb = (int)ceilf(cy + r);
    if (!clip_rows(cv, &ya, &yb)) return;
    const int32_t cx16 = (int32_t)lroundf(cx * 16.0f), cy16 = (int32_t)lroundf(cy * 16.0f);
    const int32_t r16 = (int32_t)lroundf(r * 16.0f), r2 = r16 * r16;
    const int32_t inv = (256 << 16) / r2;              // d2 * inv >> 16 = 256 d2 / r2
    int xa = imax((int)floorf(cx - r), 0), xb = imin((int)ceilf(cx + r), GFX_PW - 1);
    for (int y = ya; y <= yb; y++) {
        int32_t dy = y * 16 - cy16, dy2 = dy * dy;
        if (dy2 >= r2) continue;
        uint32_t *row = gfx_row(cv, y);
        for (int x = xa; x <= xb; x++) {
            int32_t dx = x * 16 - cx16, d2 = dx * dx + dy2;
            if (d2 >= r2) continue;
            int32_t f = 256 - ((d2 * inv) >> 16);
            int32_t k = (a * ((f * f) >> 8)) >> 8;
            if (k > 0) row[x] = gfx_add(row[x], gfx_scale(c, (uint32_t)(k > 256 ? 256 : k)));
        }
    }
}

/* convex shape, anti-aliased: the coverage of a pixel is its distance to the nearest edge. the
 * edges are set up once in floats, then each is a value in 1/256 px that steps by an add a pixel */
CORE_HOT static void convex(canvas_t *cv, const float *xy, int n, uint32_t c, int a)
{
#if GFX_SCALED
    float glass[24];
    for (int i = 0; i < 2 * n; i++) glass[i] = GFX_PX(xy[i]);
    xy = glass;
#endif
    float area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }
    if (fabsf(area) < 1e-3f) return;
    float sgn = area > 0 ? 1.0f : -1.0f, minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    int32_t ex[12], ey[12], e0[12];                    // inside when ex*x + ey*y + e0 >= 0
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * j], y1 = xy[2 * j + 1];
        float vx = x1 - x0, vy = y1 - y0, l = gfx_sqrt(vx * vx + vy * vy);
        if (l < 1e-4f) l = 1e-4f;
        float nx = -vy / l * sgn, ny = vx / l * sgn;
        ex[i] = (int32_t)lroundf(nx * FX);
        ey[i] = (int32_t)lroundf(ny * FX);
        e0[i] = (int32_t)lroundf(-(nx * x0 + ny * y0) * FX);
        minx = fminf(minx, x0); maxx = fmaxf(maxx, x0);
        miny = fminf(miny, y0); maxy = fmaxf(maxy, y0);
    }
    int ya = (int)floorf(miny) - 1, yb = (int)ceilf(maxy) + 1;
    if (!clip_rows(cv, &ya, &yb)) return;
    int xa = imax((int)floorf(minx) - 1, 0), xb = imin((int)ceilf(maxx) + 1, GFX_PW - 1);
    for (int y = ya; y <= yb; y++) {
        uint32_t *row = gfx_row(cv, y);
        /* where on this row every edge leaves some coverage: edge i needs ex*x + c > -FX/2. a turned
         * thin part fills a few pixels of its box a row, the rest is not walked. the division
         * rounds towards zero, which only widens the span */
        int sa = xa, sb = xb;
        for (int i = 0; i < n && sa <= sb; i++) {
            int32_t need = -FX / 2 + 1 - (ey[i] * y + e0[i]);
            if (ex[i] > 0) sa = imax(sa, need / ex[i]);
            else if (ex[i] < 0) sb = imin(sb, need / ex[i]);
            else if (need > 0) sb = sa - 1;
        }
        if (sa > sb) continue;
        int32_t e[12];
        for (int i = 0; i < n; i++) e[i] = ex[i] * sa + ey[i] * y + e0[i];
        for (int x = sa; x <= sb; x++) {
            int32_t m = e[0];
            e[0] += ex[0];
            for (int i = 1; i < n; i++) {
                if (e[i] < m) m = e[i];
                e[i] += ex[i];
            }
            int32_t cov = m + FX / 2;
            if (cov <= 0) continue;
            plot(row, x, c, cov >= FX ? a : (a * cov) >> 8);
        }
    }
}

CORE_HOT void gfx_tri(canvas_t *cv, float x0, float y0, float x1, float y1, float x2, float y2,
             uint32_t c, int a)
{
    const float xy[6] = { x0, y0, x1, y1, x2, y2 };
    convex(cv, xy, 3, c, a);
}

void gfx_arc(canvas_t *cv, float cx, float cy, float r, float deg0, float deg1, float w,
             uint32_t c, int a)
{
    if (!gfx_touches(cv, (int)(cy - r - w - 1), (int)(cy + r + w + 1))) return;
    float span = deg1 - deg0;
    int n = (int)ceilf(fabsf(span) / 4.0f);   // one segment per 4 degrees
    if (n < 1) n = 1;
    float k = 3.14159265f / 180.0f;
    float px = cx + r * sinf(deg0 * k), py = cy - r * cosf(deg0 * k);
    for (int i = 1; i <= n; i++) {
        float d = (deg0 + span * (float)i / (float)n) * k;
        float qx = cx + r * sinf(d), qy = cy - r * cosf(d);
        gfx_line(cv, px, py, qx, qy, w, c, a);
        px = qx; py = qy;
    }
}

/* 4x4 ordered (bayer) dither 888 -> 565, then byte swap to big endian so DMA can send the buffer
 * as is. the RGB bus reads a frame in the chip's own order: no swap */
CORE_HOT void gfx_pack565(const canvas_t *cv, uint16_t *out)
{
    /* NOTE: tried three lookup tables instead of this math, it was slower on the
     * board (10.8 ms a frame vs 9.7). dont go back to tables */
    static const CORE_DATA uint8_t bayer[4][4] = {
        { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
    };
    for (int r = 0; r < cv->h; r++) {
        const uint32_t *row = cv->px + r * GFX_PW;
        const uint8_t *bm = bayer[(cv->y0 + r) & 3];
        uint16_t *o = out + r * GFX_PW;
        for (int x = 0; x < GFX_PW; x++) {
            uint32_t p = row[x];
            unsigned d = bm[x & 3];
            unsigned rr = (((p >> 16) & 255) + (d >> 1)) >> 3;
            unsigned gg = (((p >> 8) & 255) + (d >> 2)) >> 2;
            unsigned bb = ((p & 255) + (d >> 1)) >> 3;
            if (rr > 31) rr = 31;
            if (gg > 63) gg = 63;
            if (bb > 31) bb = 31;
            uint16_t v = (uint16_t)((rr << 11) | (gg << 5) | bb);
            o[x] = RADAR_RGB_BUS ? v : (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

/* convex polygon, any number of corners (up to 12), anti-aliased like gfx_tri: the coverage of
 * a pixel is its distance to the nearest edge. silhouettes are a few of these, overlapping */
CORE_HOT void gfx_poly(canvas_t *cv, const float *xy, int n, uint32_t c, int a)
{
    if (n < 3 || n > 12) return;
    convex(cv, xy, n, c, a);
}
