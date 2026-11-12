/* the main page. left: the scope, home in the middle, real distances and bearings. the rim is a
 * compass: north is up unless cfg.up turns the map (so it can match the way the screen stands).
 * a beam sweeps for the looks, a plane glows a little brighter just after the beam crossed it.
 * planes are silhouettes turned along their track, the colour says how high (alt_colour), a
 * short trail shows where they came from. right: the panel, the nearest ones or the picked one */
#include "ui_int.h"
#include <stdlib.h>

/* --- silhouettes, seen from above, nose up, in units of the size: a few convex parts each --- */

typedef struct { int8_t n; float xy[14]; } part_t;
typedef struct { const part_t *parts; int8_t np; float size; } shape_t;

static const part_t JET[] = {
    { 7, { 0, -1.0f, 0.10f, -0.82f, 0.10f, 0.80f, 0.05f, 0.98f, -0.05f, 0.98f, -0.10f, 0.80f, -0.10f, -0.82f } },
    { 4, { -0.08f, -0.22f, -0.98f, 0.26f, -0.98f, 0.38f, -0.08f, 0.12f } },
    { 4, { 0.08f, -0.22f, 0.98f, 0.26f, 0.98f, 0.38f, 0.08f, 0.12f } },
    { 4, { -0.06f, 0.64f, -0.40f, 0.88f, -0.40f, 0.98f, -0.06f, 0.86f } },
    { 4, { 0.06f, 0.64f, 0.40f, 0.88f, 0.40f, 0.98f, 0.06f, 0.86f } },
};
static const part_t BIZ[] = {
    { 6, { 0, -1.0f, 0.08f, -0.8f, 0.08f, 0.85f, 0, 0.98f, -0.08f, 0.85f, -0.08f, -0.8f } },
    { 4, { -0.07f, -0.05f, -0.80f, 0.28f, -0.80f, 0.38f, -0.07f, 0.22f } },
    { 4, { 0.07f, -0.05f, 0.80f, 0.28f, 0.80f, 0.38f, 0.07f, 0.22f } },
    { 4, { -0.08f, 0.40f, -0.22f, 0.40f, -0.22f, 0.62f, -0.08f, 0.62f } },   /* engines at the tail */
    { 4, { 0.08f, 0.40f, 0.22f, 0.40f, 0.22f, 0.62f, 0.08f, 0.62f } },
    { 4, { -0.38f, 0.86f, 0.38f, 0.86f, 0.38f, 0.96f, -0.38f, 0.96f } },      /* T-tail */
};
static const part_t LIGHT[] = {
    { 5, { 0, -0.95f, 0.10f, -0.78f, 0.08f, 0.92f, -0.08f, 0.92f, -0.10f, -0.78f } },
    { 4, { -1.0f, -0.40f, 1.0f, -0.40f, 1.0f, -0.18f, -1.0f, -0.18f } },      /* straight wing */
    { 4, { -0.38f, 0.72f, 0.38f, 0.72f, 0.38f, 0.88f, -0.38f, 0.88f } },
};
static const part_t FAST[] = {
    { 6, { 0, -1.0f, 0.09f, -0.7f, 0.09f, 0.95f, 0, 1.0f, -0.09f, 0.95f, -0.09f, -0.7f } },
    { 3, { 0.05f, -0.45f, 0.05f, 0.70f, 0.78f, 0.75f } },                     /* delta */
    { 3, { -0.05f, -0.45f, -0.05f, 0.70f, -0.78f, 0.75f } },
};
static const part_t HELI[] = {
    { 7, { 0, -0.55f, 0.22f, -0.38f, 0.24f, 0.10f, 0.10f, 0.30f, -0.10f, 0.30f, -0.24f, 0.10f, -0.22f, -0.38f } },
    { 4, { -0.05f, 0.25f, 0.05f, 0.25f, 0.04f, 1.0f, -0.04f, 1.0f } },        /* tail boom */
    { 4, { -0.20f, 0.88f, 0.20f, 0.88f, 0.20f, 0.96f, -0.20f, 0.96f } },
};
static const part_t GLIDER[] = {
    { 5, { 0, -0.85f, 0.06f, -0.7f, 0.04f, 0.95f, -0.04f, 0.95f, -0.06f, -0.7f } },
    { 4, { -1.0f, -0.32f, 1.0f, -0.32f, 1.0f, -0.20f, -1.0f, -0.20f } },
    { 4, { -0.28f, 0.84f, 0.28f, 0.84f, 0.28f, 0.92f, -0.28f, 0.92f } },
};
static const part_t DRONE[] = {
    { 4, { -0.75f, -0.65f, -0.65f, -0.75f, 0.75f, 0.65f, 0.65f, 0.75f } },
    { 4, { 0.65f, -0.75f, 0.75f, -0.65f, -0.65f, 0.75f, -0.75f, 0.65f } },
};
static const part_t GROUND[] = {
    { 4, { -0.5f, -0.8f, 0.5f, -0.8f, 0.5f, 0.8f, -0.5f, 0.8f } },
};

static const shape_t SHAPES[KINDS] = {
    [KIND_JET]     = { JET, 5, 9.0f },
    [KIND_LIGHT]   = { LIGHT, 3, 6.5f },
    [KIND_BIZJET]  = { BIZ, 6, 7.5f },
    [KIND_HEAVY]   = { JET, 5, 11.5f },
    [KIND_FAST]    = { FAST, 3, 8.0f },
    [KIND_HELI]    = { HELI, 3, 7.0f },
    [KIND_GLIDER]  = { GLIDER, 3, 8.0f },
    [KIND_BALLOON] = { NULL, 0, 5.0f },
    [KIND_DRONE]   = { DRONE, 2, 5.0f },
    [KIND_GROUND]  = { GROUND, 1, 4.0f },
};

void silhouette(canvas_t *cv, int kind, float x, float y, float heading, float size, uint32_t c, int a)
{
    if (kind < 0 || kind >= KINDS) kind = KIND_JET;
    const shape_t *sh = &SHAPES[kind];
    float s = sh->size * size;
    if (!gfx_touches(cv, (int)(y - s - 2), (int)(y + s + 2))) return;
    if (kind == KIND_BALLOON) { gfx_disc(cv, x, y, s, c, a); return; }
    float ch = cosf(heading * D2R), sn = sinf(heading * D2R);
    for (int i = 0; i < sh->np; i++) {
        const part_t *p = &sh->parts[i];
        float xy[14];
        for (int k = 0; k < p->n; k++) {
            float px = p->xy[2 * k], py = p->xy[2 * k + 1];
            xy[2 * k] = x + (px * ch - py * sn) * s;
            xy[2 * k + 1] = y + (px * sn + py * ch) * s;
        }
        gfx_poly(cv, xy, p->n, c, a);
    }
    if (kind == KIND_HELI) gfx_ring(cv, x, y - 0.12f * s * ch, 0.9f * s, 1.0f, c, a / 2);   /* the rotor */
}

/* --- the disc: glass, rings, ticks, the cross and the beam in one pass over its pixels, the
 * WiFi radar's way, in whole numbers (the C3 has no floating point unit). the radius of a pixel
 * in 1/16 px follows from the one before it, the angle comes from a table of an eighth of a
 * turn, a byte a pixel: the other eighth of the quadrant is its mirror (dx and dy swapped, 90
 * degrees less the angle), so the table is half a quadrant and the heap keeps the rest for the
 * map. the light of what never turns (glass, rings, their glow) is a value by radius. the ticks
 * and the cross turn with the map: the angle only says which one is near, the exact distance to
 * it comes from that line's sine and cosine */

static uint8_t *s_phi8;                  /* angle from straight up, 1/256 of 90 degrees, dx <= dy only */
static uint16_t s_tri[G_RQ];             /* where row dy starts in it */
static CORE_BIG uint8_t s_ring[G_RQ * 16], s_dash[G_RQ * 16], s_glass[G_RQ + 1];

static float cover(float r, float R, float w) { return clampf(w * 0.5f + 0.5f - fabsf(r - R), 0, 1); }

int scope_init(void)
{
    if (!s_phi8) s_phi8 = malloc(G_RQ * (G_RQ + 1) / 2);
    if (!s_phi8) return 0;
    int at = 0;
    for (int dy = 0; dy < G_RQ; dy++) {
        s_tri[dy] = (uint16_t)at;
        for (int dx = 0; dx <= dy; dx++) {
            float a = atan2f((float)dx, (float)dy) / (0.5f * 3.14159265f) * 256.0f;
            s_phi8[at++] = (uint8_t)(a + 0.5f > 255.0f ? 255.0f : a + 0.5f);
        }
    }
    for (int i = 0; i < G_RQ * 16; i++) {
        float r = (float)i / 16.0f, a = 0, d = 0;
        const float in = GFX_PL(10.0f);
        a += 150.0f * cover(r, (float)G_PR, GFX_PL(1.0f));          // the range ring
        a += 230.0f * cover(r, (float)G_SR, GFX_PL(1.3f));          // the rim
        if (r > G_SR) { float h = clampf(1.0f - (r - G_SR) / GFX_PL(5.0f), 0, 1); a += 30.0f * h * h; }   // glow outside it
        if (r < G_PR && r > G_PR - in) { float h = (r - (G_PR - in)) / in; a += 22.0f * h * h; }         // and inside the ring
        for (int k = 1; k <= 3; k++) d += 95.0f * cover(r, G_PR * (float)k / 4.0f, GFX_PL(1.0f));         // the inner rings, dashed
        s_ring[i] = (uint8_t)clampf(a, 0, 255);
        s_dash[i] = (uint8_t)clampf(d, 0, 255);
    }
    for (int r = 0; r <= G_RQ; r++) {
        float t = clampf((float)r / G_PR, 0, 1);
        s_glass[r] = (uint8_t)(70.0f + 100.0f * t * t * t);         // dark in the middle, lighter to the ring
    }
    return 1;
}

/* the 72 tick lines, every 5 degrees of the compass, as they stand on the screen: cosine and
 * sine in 1/16384. made again only when the map turns */
static int32_t s_tc[72], s_ts[72];
static int     s_tick_up = -1;

static void tick_lines(int up)
{
    if (up == s_tick_up) return;
    for (int k = 0; k < 72; k++) {
        float th = (float)(k * 5 - up) * D2R;
        s_tc[k] = (int32_t)lroundf(cosf(th) * 16384.0f);
        s_ts[k] = (int32_t)lroundf(sinf(th) * 16384.0f);
    }
    s_tick_up = up;
}

// how far a pixel (dx, dy from the middle) is from tick line k, in 1/256 px
static inline int32_t off_line(int dx, int dy, int k)
{
    int32_t d = (dx * s_tc[k] + dy * s_ts[k]) >> 6;
    return d < 0 ? -d : d;
}

/* --- the ground: the map (map.h) under the glass. the radar lights it with its beam like the
 * echoes of the land, the flat map shows it steady, both in the theme's colours; the classic
 * look paints OpenStreetMap's own --- */

static float k_px(const ui_t *u);
static float k_glass(const ui_t *u) { return GFX_PL(k_px(u)); }   // pixels of the glass a km

// the map, when there is one for this home and the user wants it
static const map_t *ground(const ui_t *u)
{
    const map_t *m = u->map;
    if (!m || !m->ready || !m->tiles || !u->cfg.map || m->lat != u->cfg.lat || m->lon != u->cfg.lon) return NULL;
    int w, h, disc;
    ui_ground_size(&u->cfg, &w, &h, &disc);
    return m->w == w && m->h == h ? m : NULL;          // the look just changed: its map is still being made
}

// a colour and a strength for each kind of ground. strength 0..255 of the colour, added to the glass
static void ground_paint(const ui_t *u, uint32_t *col, uint8_t *lev)
{
    static const uint8_t CLASSIC[M_CLASSES][3] = {
        { 242, 239, 233 }, { 170, 211, 223 }, { 173, 209, 158 }, { 205, 235, 176 }, { 224, 223, 223 },
        { 217, 208, 201 }, { 255, 255, 255 }, { 252, 214, 164 }, { 232, 146, 162 }, { 112, 112, 112 },
        { 172, 70, 172 },  { 40, 40, 40 },    { 187, 187, 204 },
    };
    /* the land a whisper, water darker than it, the towns and roads brighter, motorways and
     * runways in the colour of what you read first. names are painted as the land: gone */
    static const uint8_t LEV[M_CLASSES] = { 14, 0, 34, 24, 46, 62, 66, 100, 120, 110, 95, 14, 140 };
    for (int i = 0; i < M_CLASSES; i++) {
        if (u->cfg.look == LOOK_CLASSIC) {
            col[i] = RGB(CLASSIC[i][0], CLASSIC[i][1], CLASSIC[i][2]);
        } else {
            col[i] = i == M_MOTOR || i == M_AERO ? C_FOCUS : i == M_RAIL ? C_QUIET : i == M_BORDER ? C_LOW : C_PRIM;
            lev[i] = LEV[i];
        }
    }
}

#ifdef _MSC_VER
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE inline __attribute__((always_inline))
#endif

/* the pass itself. disc() below makes it twice: once for the plain radar, where the compiler
 * drops everything of the map, and once for the rest. a loop that may meet the map runs out of
 * registers and is slower by a sixth even with no map in it */
/* beam and ticks: 0 for the background layer of the boards that put the sweep and the compass ring
 * over the frame as the panel is fed (config.h RADAR_OVERLAY), 1 everywhere else */
static ALWAYS_INLINE void disc_pass(ui_t *u, canvas_t *cv, const int radar, const int classic, const map_t *m,
                                    const int beam_on, const int ticks_on)
{
    int ya = G_SCY - G_RQ + 1, yb = G_SCY + G_RQ - 1;
    if (ya < cv->y0) ya = cv->y0;
    if (yb > cv->y0 + cv->h - 1) yb = cv->y0 + cv->h - 1;
    if (ya > yb) return;
    /* which texel lies under a pixel: the map is north up and a texel a pixel at the range it
     * was made for, the scope turns and stretches it. 1/65536 texel, a step per pixel along x */
    uint32_t gcol[M_CLASSES], gpaint[M_CLASSES];   // gpaint: what a texel adds, or is (classic), off the beam's trail
    uint8_t  glev[M_CLASSES], echo_lut[257];
    float    gk = 0, gcu = 1, gsu = 0;
    int32_t  gdu = 0, gdv = 0, gmu = 0, gmv = 0;
    if (m) {
        gk = m->kpx / k_glass(u);
        gcu = cosf(u->cfg.up * D2R);
        gsu = sinf(u->cfg.up * D2R);
        gdu = (int32_t)lroundf(gcu * gk * 65536.0f);
        gdv = (int32_t)lroundf(gsu * gk * 65536.0f);
        gmu = m->w * 32768;                          // home: the middle of the map
        gmv = m->h * 32768;
        ground_paint(u, gcol, glev);
        for (int i = 0; i < M_CLASSES; i++)
            gpaint[i] = classic ? gcol[i] : gfx_scale(gcol[i], radar ? glev[i] * 110u >> 8 : glev[i]);
        for (int i = 0; i <= 256; i++) {               // the radar's ground: lit by the beam, then fading
            int f = 256 - i;
            echo_lut[i] = (uint8_t)(110 + (146 * f * f >> 16));
        }
    }
    const uint16_t beam = (uint16_t)(u->beam / 360.0f * 65536.0f);
    const uint16_t up16 = (uint16_t)((uint32_t)u->cfg.up * 65536u / 360u);
    const uint16_t trail = 16384;                                  /* 90 degrees of afterglow */
    const uint32_t RP16 = G_PR * 16, RQ16 = G_RQ * 16, RIM16 = G_SR * 16 + 4;
    const uint32_t glass = C_GLASS, prim = C_PRIM, focus = C_FOCUS;
    const int32_t bc = (int32_t)lroundf(cosf(u->beam * D2R) * 16384.0f), bs = (int32_t)lroundf(sinf(u->beam * D2R) * 16384.0f);
    uint8_t trail_lut[257];                                        // by distance behind the beam: no divide a pixel
    for (int i = 0; i <= 256; i++) {
        int f = 256 - i;
        trail_lut[i] = (uint8_t)(80 * f * f >> 16);
    }
    const uint32_t trail_k = (256u << 16) / trail;
    /* the ground of a row is looked up first, in a loop of its own: the pixel loop below has
     * all the registers it can use already */
    uint8_t ground_row[2 * G_RQ + 2];
    memset(ground_row, M_NONE, sizeof ground_row);
    /* the radar without its panel has the scope in the middle of the screen: x below stays counted
     * from where the scope stands beside the panel (G_SCX, a constant the loop keeps), the row is
     * moved under it instead */
    const int shift = scope_glass_cx(u) - G_SCX;

    for (int y = ya; y <= yb; y++) {
        int dy = y - G_SCY, ady = dy < 0 ? -dy : dy;
        if (ady >= G_RQ) continue;
        uint32_t *row = gfx_row(cv, y) + shift;
        const uint8_t *lp = s_phi8 + s_tri[ady];
        int span = (int)sqrtf((float)((G_RQ - 1) * (G_RQ - 1) - ady * ady));
        int xa = G_SCX - span < -shift ? -shift : G_SCX - span;
        int xb = G_SCX + span > GFX_PW - 1 - shift ? GFX_PW - 1 - shift : G_SCX + span;
        /* a pixel's radius (1/16 px) and angle hang on |dx| only: worked out once a row for both
         * halves of it, not once a pixel. a radius past the disc is RQ16 */
        uint16_t rad[G_RQ + 1], phi[G_RQ + 1];
        const int amax = G_SCX - xa > xb - G_SCX ? G_SCX - xa : xb - G_SCX;
#if GFX_SOFT_FLOAT
        int32_t r2 = dy * dy;                                      // the radius squared, whole px
        uint32_t r16 = gfx_isqrt((uint32_t)r2 << 8);
#endif
        for (int a = 0; a <= amax; a++) {
#if GFX_SOFT_FLOAT
            if (a) {                                               // a pixel out: the radius moves under 1 px
                r2 += 2 * a - 1;
                uint32_t t = (uint32_t)r2 << 8;
                while ((r16 + 1) * (r16 + 1) <= t) r16++;
                while (r16 * r16 > t) r16--;
            }
#else
            uint32_t r16 = (uint32_t)(gfx_sqrt((float)(a * a + dy * dy)) * 16.0f);   // with a float unit this is quicker
#endif
            rad[a] = (uint16_t)(r16 < RQ16 ? r16 : RQ16);
            phi[a] = (uint16_t)(a <= ady ? lp[a] : 256u - s_phi8[s_tri[a] + ady]);
        }
        const uint8_t *cl = ground_row - xa;                       // the ground under pixel x: cl[x]
        if (m && ady < G_PR) {
            int gs = (int)sqrtf((float)(G_PR * G_PR - ady * ady)), ga = G_SCX - gs, gb = G_SCX + gs;   // the range ring's chord
            float right = (float)(ga - G_SCX), up = (float)-dy;   // to_screen backwards: east and north
            int32_t gu = gmu + (int32_t)lroundf((right * gcu + up * gsu) * gk * 65536.0f);
            int32_t gv = gmv - (int32_t)lroundf((up * gcu - right * gsu) * gk * 65536.0f);
            const uint32_t mw = (uint32_t)m->w, mh = (uint32_t)m->h;
            memset(ground_row, M_NONE, sizeof ground_row);
            for (int x = ga; x <= gb; x++, gu += gdu, gv += gdv) {
                uint32_t iu = (uint32_t)(gu >> 16), iv = (uint32_t)(gv >> 16);
                if (iu < mw && iv < mh) {
                    ground_row[x - xa] = (uint8_t)((m->row[iv][iu >> 3] >> ((iu & 7) * 4)) & 15);
                }
            }
        } else if (m) {
            memset(ground_row, M_NONE, sizeof ground_row);
        }
        for (int x = xa; x <= xb; x++) {
            int dx = x - G_SCX, adx = dx < 0 ? -dx : dx;
            const uint32_t r16 = rad[adx];
            if (r16 >= RQ16) continue;
            unsigned ph = phi[adx], q = ph << 6;                   /* a quarter turn = 16384 */
            uint16_t ang;
            if (dx >= 0) ang = dy < 0 ? (uint16_t)q : (uint16_t)(32768u - q);
            else ang = dy >= 0 ? (uint16_t)(32768u + q) : (uint16_t)(0u - q);
            const uint16_t brg = (uint16_t)(ang + up16);           // the compass direction of the pixel

            uint32_t p = row[x];
            const int cls = m ? cl[x] : M_NONE;            // no map: the compiler drops what follows from it
            if (r16 <= RP16) {
                p = gfx_mix(p, glass, s_glass[r16 >> 4]);
                if (cls != M_NONE && !radar) p = classic ? gpaint[cls] : gfx_add(p, gpaint[cls]);
            }
            unsigned st = s_ring[r16];
            if (radar && (ph >> 3) & 1) st += s_dash[r16];
            if (ticks_on && r16 >= (G_SR - GFX_Z(6)) * 16 && r16 <= RIM16) {   // the ticks, in from the rim
                int k = (int)(((uint32_t)brg * 72u + 32768u) >> 16) % 72;
                int len = k % 6 == 0 ? GFX_Z(6) : k % 2 == 0 ? GFX_Z(4) : GFX_Z(2);
                int32_t cov = GFX_Z(192) - off_line(dx, dy, k);
                if (GFX_SCALED && cov > 255) cov = 255;
                if (cov > 0 && r16 >= (uint32_t)(G_SR - len) * 16) st += (unsigned)((k % 6 == 0 ? 240 : k % 2 == 0 ? 150 : 90) * cov >> 8);
            } else if (radar && r16 >= GFX_Z(7) * 16 && r16 < RP16 - 16) {   // the cross along the compass, the diagonals fainter
                int k8 = (int)(((uint32_t)brg * 8u + 32768u) >> 16) & 7;
                int32_t cov = GFX_Z(256) - off_line(dx, dy, k8 * 9);
                if (GFX_SCALED && cov > 256) cov = 256;
                if (cov > 0) st += (unsigned)((k8 & 1 ? 20 : 36) * cov >> 8);
            }
            if (st) p = gfx_add(p, gfx_scale(prim, st > 255 ? 255 : st));
            if (radar && r16 <= RP16 && !beam_on) {
                if (cls != M_NONE) p = gfx_add(p, gpaint[cls]);    // the ground as it is off the beam's trail
            } else if (radar && r16 <= RP16) {
                uint16_t d = (uint16_t)(beam - ang);                /* how far behind the beam */
                if (d < trail) {
                    uint32_t behind = ((uint32_t)d * trail_k) >> 16;
                    if (cls != M_NONE)                              // the ground echoes, brightest just after the beam
                        p = gfx_add(p, gfx_scale(gcol[cls], (uint32_t)glev[cls] * echo_lut[behind] >> 8));
                    p = gfx_add(p, gfx_scale(prim, trail_lut[behind]));
                } else if (cls != M_NONE) {
                    p = gfx_add(p, gpaint[cls]);
                }
                int16_t e = (int16_t)(uint16_t)(ang - beam);        /* near the beam's edge: how near, exactly */
                if (e > -1200 && e < 1200 && r16 > 32) {
                    int32_t off = (dx * bc + dy * bs) >> 6, cov = GFX_Z(333) - (off < 0 ? -off : off);
                    if (GFX_SCALED && cov > 333) cov = 333;
                    if (cov > 0) p = gfx_add(p, gfx_scale(focus, (uint32_t)(190 * cov >> 8)));
                }
            }
            row[x] = p;
        }
    }
}

CORE_HOT static void disc(ui_t *u, canvas_t *cv)
{
    const map_t *m = ground(u);
    const int look = u->cfg.look;
#if RADAR_OVERLAY
    if (LAYER_BG) {                                    // the sweep and the ticks go over it later
        if (look == LOOK_RADAR && !m) disc_pass(u, cv, 1, 0, NULL, 0, 0);
        else disc_pass(u, cv, look == LOOK_RADAR, look == LOOK_CLASSIC, m, 0, 0);
        return;
    }
#endif
    if (look == LOOK_RADAR && !m) disc_pass(u, cv, 1, 0, NULL, 1, 1);
    else disc_pass(u, cv, look == LOOK_RADAR, look == LOOK_CLASSIC, m, 1, 1);
}

#if RADAR_OVERLAY

/* the compass ring's ticks for the moving layer: what disc_pass adds for them, worked out only in a
 * box round each tick instead of over the whole band. light added, as there */
static void ticks_top(ui_t *u, canvas_t *cv)
{
    const int cx = scope_glass_cx(u), cy = G_SCY;
    const uint32_t prim = C_PRIM, RIM16 = G_SR * 16 + 4;
    for (int k = 0; k < 72; k++) {
        const int len = k % 6 == 0 ? GFX_Z(6) : k % 2 == 0 ? GFX_Z(4) : GFX_Z(2);
        const unsigned strength = k % 6 == 0 ? 240 : k % 2 == 0 ? 150 : 90;
        const float sn = s_ts[k] / 16384.0f, cs = s_tc[k] / 16384.0f;   // the tick runs out along (sn, -cs)
        const float ri = (float)(G_SR - len) - 1.0f, ro = (float)G_SR + 1.0f;
        float xa = cx + sn * ri, xb = cx + sn * ro, ya = cy - cs * ri, yb = cy - cs * ro;
        int x0 = (int)floorf(fminf(xa, xb)) - 2, x1 = (int)ceilf(fmaxf(xa, xb)) + 2;
        int y0 = (int)floorf(fminf(ya, yb)) - 2, y1 = (int)ceilf(fmaxf(ya, yb)) + 2;
        if (y0 < cv->y0) y0 = cv->y0;
        if (y1 > cv->y0 + cv->h - 1) y1 = cv->y0 + cv->h - 1;
        if (x0 < 0) x0 = 0;
        if (x1 > GFX_PW - 1) x1 = GFX_PW - 1;
        for (int y = y0; y <= y1; y++) {
            uint32_t *row = gfx_row(cv, y);
            const int dy = y - cy;
            for (int x = x0; x <= x1; x++) {
                const int dx = x - cx;
                if (dx * s_ts[k] - dy * s_tc[k] <= 0) continue;   // the other half of the line is tick k+36's
                const uint32_t r16 = (uint32_t)(gfx_sqrt((float)(dx * dx + dy * dy)) * 16.0f);
                if (r16 < (uint32_t)(G_SR - len) * 16 || r16 > RIM16) continue;
                int32_t cov = GFX_Z(192) - off_line(dx, dy, k);
                if (GFX_SCALED && cov > 255) cov = 255;
                if (cov <= 0) continue;
                unsigned st = strength * (unsigned)cov >> 8;
                row[x] = gfx_add(row[x], gfx_scale(prim, st > 255 ? 255 : st));
            }
        }
    }
}
#endif

/* --- where things are on the screen --- */

static float k_px(const ui_t *u) { return (float)PR / range_km(&u->cfg); }

// km east/north of home -> screen pixel, the map turned so view_up is at the top
static void to_screen(const ui_t *u, float x, float y, float *sx, float *sy)
{
    static float cu = 1.0f, su = 0.0f;                 // the turn of the map, worked out when it changes
    static int at = 0;
    const int vu = view_up(u);
    if (vu != at) { at = vu; cu = cosf(at * D2R); su = sinf(at * D2R); }
    float k = k_px(u);
    float right = x * cu - y * su, up = x * su + y * cu;
    *sx = view_cx(u) + right * k;
    *sy = SCY - up * k;
}

// a compass bearing -> the angle on the screen, clockwise from straight up
static float screen_ang(const ui_t *u, float brg) { return wrap360(brg - (float)view_up(u)); }

// inside what shows the map: the range ring, or on the flat square the whole left side
static int in_view(const ui_t *u, float sx, float sy)
{
    if (square_look(u)) return sx >= 0 && sx < GFX_W && sy >= 0 && sy < GFX_H;
    const float dx = sx - view_cx(u);
    return dx * dx + (sy - SCY) * (sy - SCY) <= (float)(PR * PR);
}

static float plane_size(void) { return (float)GFX_H / 240.0f; }

typedef struct { int x, y, w, h; } box_t;

#if !RADAR_ROUND
/* the square map's corners and what sits in them: home's name and how the sky is; the look, the
 * three dots, north; the height legend; the scale and OpenStreetMap's name. labels keep off.
 * the right ones move in with the panel */
#define SQ_BOXES 4
static box_t sq_box(const ui_t *u, int k)
{
    const int r = view_right(u), lx = look_x(u);
    switch (k) {
    case 0:  return (box_t){ 0, 0, 98, 30 };
    case 1:  return (box_t){ lx - 4, 0, r - lx + 4, LOOK_Y + LOOK_H + 22 };
    case 2:  return (box_t){ 0, GFX_H - 24, 156, 24 };
    default: return (box_t){ r - 100, GFX_H - 36, 100, 36 };
    }
}
#endif

static int overlap(box_t a, box_t b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

#define LABEL_W 60
#define LABEL_H 18

static const char *plane_name(const plane_t *p) { return p->flight[0] ? p->flight : p->reg[0] ? p->reg : "------"; }

// what the top left corner calls home
static inline const char *home_name(const ui_t *u) { return u->net.demo ? "DEMO SKY" : u->cfg.place[0] ? u->cfg.place : "HOME"; }

// the numbers on the rings: quarters of the range, as short as they can be written
static void ring_value(float v, char *buf, int sz)
{
    if (fabsf(v - roundf(v)) < 0.05f) snprintf(buf, (size_t)sz, "%d", (int)lroundf(v));
    else snprintf(buf, (size_t)sz, v < 100 ? "%.1f" : "%.0f", (double)v);
}

static void ring_label_at(const ui_t *u, int k, int *x, int *y)
{
    float r = PR * (float)k / 4.0f;
    *x = view_cx(u) + (int)(r * 0.7071f) + 2;
    *y = SCY + (int)(r * 0.7071f) - 2;
}

static void grid_frame(ui_t *u);
static int clip_circle(float cx, float *ax, float *ay, float *bx, float *by, float r);
static int plane_lost(const ui_t *u, const plane_t *p);

/* a label's second line: the height, and what the radar's options add to it, the way a real data
 * block goes: FL280, up or down, >350 where the autopilot is taking it, 45 for 450 knots.
 * cv NULL: only how wide it comes out */
static int data_line(const ui_t *u, canvas_t *cv, int x, int y, const plane_t *p, uint32_t col)
{
    char t[12], s[16];
    fmt_alt(u, p, t, sizeof t, 1);
    if (cv) micro_text(cv, x, y, t, C_DIM, 230, 1);
    int w = micro_width(t, 1);
    if (u->cfg.look != LOOK_RADAR || p->ground) return w;
    if (u->opt.block && abs(p->vrate) >= 200) {        // climbing or coming down
        if (cv) {
            float ax = (float)(x + w + 3), ay = (float)(y + 2);
            if (p->vrate > 0) gfx_tri(cv, ax - 3, ay + 3, ax + 3, ay + 3, ax, ay - 3, col, 240);
            else gfx_tri(cv, ax - 3, ay - 3, ax + 3, ay - 3, ax, ay + 3, col, 240);
        }
        w += 7;
    }
    if (u->opt.autopilot && p->sel_fl && p->has_alt && abs(p->sel_fl * 100 - (int)p->alt) >= 200) {
        fmt_height(u, p->sel_fl * 100, t, sizeof t);   // not there yet: where it is going
        snprintf(s, sizeof s, ">%s", t[0] == 'F' ? t + 2 : t);
        if (cv) micro_text(cv, x + w + 1, y, s, C_PRIM, 235, 1);
        w += 1 + micro_width(s, 1);
    }
    if (u->opt.block && p->has_spd) {
        snprintf(s, sizeof s, "%d", (int)lroundf(p->gs / 10.0f));
        if (cv) micro_text(cv, x + w + 4, y, s, C_DIM, 230, 1);
        w += 4 + micro_width(s, 1);
    }
    if (plane_lost(u, p)) {
        if (cv) micro_text(cv, x + w + 4, y, "CST", C_LOW, 235, 1);
        w += 4 + micro_width("CST", 1);
    }
    return w;
}

/* the speed line on the radar, pixels a knot: a minute's flight at a range of 50 km. the same length
 * at any zoom, so that zooming in does not fill the scope with lines */
#define VECTOR_PX ((float)PR / 1620.0f)
#define CLOCK_Y 26                                     /* under how the sky is: on the right it runs into the rim on the 2.8" */
/* the right edge of what is round the radar: where the panel starts, or the screen's edge without it */
static inline int scope_edge(const ui_t *u) { return panel_shown(u) ? PX : GFX_W; }
#define GEAR_W (GFX_H >= 320 ? 26 : 20)                /* the radar's options: under the scope, on the right */
#define GEAR_X(u) (scope_edge(u) - 9 - GEAR_W)
#define GEAR_Y (GFX_H - 28 - GEAR_W)
#define LOST_MS 12000                                  /* a position older than this is a guess */
/* a conflict: both over the floor, closer than 3 NM and 1000 ft within two minutes. under the floor
 * planes queue for a runway close together on purpose, and a real scope is told where not to warn */
#define ALERT_KM    5.556f
#define ALERT_FT    1000.0f
#define ALERT_AHEAD 120.0f
#define ALERT_FLOOR 5000

// LOWEST and HIGHEST. the picked plane stays: what the panel follows does not vanish by itself
static int in_band(const ui_t *u, const plane_t *p)
{
    const scope_opt_t *o = &u->opt;
    if (u->cfg.look != LOOK_RADAR || (!o->fl_lo && !o->fl_hi) || !p->has_alt || p->icao == u->sel) return 1;
    int fl = p->ground ? 0 : (int)((p->alt + 50) / 100);
    return fl >= o->fl_lo && (!o->fl_hi || fl <= o->fl_hi);
}

static int plane_lost(const ui_t *u, const plane_t *p)
{
    return u->opt.lost && u->cfg.look == LOOK_RADAR && !p->ground && (int32_t)(u->now - p->seen) > LOST_MS;
}

static int can_conflict(const plane_t *p, const spot_t *sp)
{
    return sp->vis == 1 && p->has_alt && p->has_spd && !p->ground && p->alt >= ALERT_FLOOR;
}

/* which planes are closing on each other. every pair is a lot for the C3 to do in every frame, and
 * nothing here changes faster than a second */
static void conflicts(ui_t *u)
{
    static uint32_t at;
    static float px[SKY_MAX], py[SKY_MAX], vx[SKY_MAX], vy[SKY_MAX];
    const sky_t *s = &u->snap;
    if (!u->opt.conflict || u->cfg.look != LOOK_RADAR) {
        for (int i = 0; i < s->n; i++) u->spot[i].alert = 0;
        return;
    }
    if (u->now - at < 1000u) return;
    at = u->now;
    for (int i = 0; i < s->n; i++) {
        const plane_t *p = &s->p[i];
        float km_s = p->gs * 1.852f / 3600.0f;
        u->spot[i].alert = 0;
        plane_now(p, u->now, &px[i], &py[i]);
        vx[i] = km_s * sinf(p->track * D2R);
        vy[i] = km_s * cosf(p->track * D2R);
    }
    for (int i = 0; i < s->n; i++) {
        const plane_t *a = &s->p[i];
        if (!can_conflict(a, &u->spot[i])) continue;
        for (int j = i + 1; j < s->n; j++) {
            const plane_t *b = &s->p[j];
            if (!can_conflict(b, &u->spot[j])) continue;
            float dx = px[j] - px[i], dy = py[j] - py[i], wx = vx[j] - vx[i], wy = vy[j] - vy[i];
            float ww = wx * wx + wy * wy;
            float t = ww > 1e-9f ? clampf(-(dx * wx + dy * wy) / ww, 0.0f, ALERT_AHEAD) : 0.0f;   // when they are closest
            float cx = dx + wx * t, cy = dy + wy * t;
            if (cx * cx + cy * cy > ALERT_KM * ALERT_KM) continue;
            float dz = (float)(b->alt - a->alt) + (float)(b->vrate - a->vrate) * t / 60.0f;
            if (fabsf(dz) >= ALERT_FT) continue;
            u->spot[i].alert = (uint8_t)(j + 1);
            u->spot[j].alert = (uint8_t)(i + 1);
        }
    }
}

/* what the strips share is worked out here, once a frame. the strips only read: on the 3.5" two
 * cores draw them at once (main.c) */
void scope_frame(ui_t *u)
{
    const int radar = u->cfg.look == LOOK_RADAR;
    tick_lines(u->cfg.up);                             // the disc's ticks and the compass letters stand on them
    grid_frame(u);
    sky_t *s = &u->snap;
    // nearest first
    u->n_order = 0;
    for (int i = 0; i < s->n; i++) {
        if (!in_band(u, &s->p[i])) {                   // as if it was not there: not drawn, not counted, not listed
            u->spot[i].vis = 0;
            u->spot[i].lw = 0;
            continue;
        }
        int j = u->n_order++;
        while (j > 0 && s->p[u->order[j - 1]].dist > s->p[i].dist) { u->order[j] = u->order[j - 1]; j--; }
        u->order[j] = (uint8_t)i;
    }
    float range = range_km(&u->cfg);
    int sel = -1;
    for (int k = 0; k < u->n_order; k++) {
        int i = u->order[k];
        const plane_t *p = &s->p[i];
        spot_t *sp = &u->spot[i];
        float x, y, sx, sy;
        plane_now(p, u->now, &x, &y);
        to_screen(u, x, y, &sx, &sy);
        if (radar && u->opt.beam_step) {               // shown where the beam last saw it, till the next turn
            float since = wrap360(u->beam - atan2f(sx - (float)view_cx(u), (float)SCY - sy) / D2R);
            plane_now(p, u->now - (uint32_t)(since * (BEAM_TURN_MS / 360.0f)), &x, &y);
            to_screen(u, x, y, &sx, &sy);
        }
        float d = sqrtf(x * x + y * y);
        sp->lw = 0;
        if (square_look(u)) {                          // the flat square: on it, or a dart at its edge
            int on = sx >= 4 && sx < view_right(u) - 4 && sy >= 4 && sy < GFX_H - 4;
            sp->vis = on ? 1 : d <= range * 2.4f ? 2 : 0;
            if (sp->vis == 2) {
                sx = clampf(sx, 5, (float)(view_right(u) - 6));
                sy = clampf(sy, 5, (float)(GFX_H - 6));
            }
        } else if (d <= range) {
            sp->vis = 1;
        } else if (d <= range * 1.6f) {                 // just outside: a dart in the band past the ring
            sp->vis = 2;
            const float cx = (float)view_cx(u), a = atan2f(sx - cx, (float)SCY - sy);
            sx = cx + (PR + 4) * sinf(a);
            sy = SCY - (PR + 4) * cosf(a);
        } else {
            sp->vis = 0;
        }
        sp->x = sp->vx = (int16_t)lroundf(sx);
        sp->y = sp->vy = (int16_t)lroundf(sy);
        if (radar && u->opt.vectors && sp->vis == 1 && p->has_spd && !p->ground) {
            float a = screen_ang(u, p->track) * D2R, len = p->gs * VECTOR_PX;
            float ax = sx, ay = sy, bx = sx + len * sinf(a), by = sy - len * cosf(a);
            if (clip_circle((float)view_cx(u), &ax, &ay, &bx, &by, (float)PR)) {   // it stops at the range ring
                sp->vx = (int16_t)lroundf(bx);
                sp->vy = (int16_t)lroundf(by);
            }
        }
        if (p->icao == u->sel) sel = i;
        // the trail on the screen, once a frame, not once a strip: the 3.5" has 40 of them
        sp->tn = sp->vis == 1 ? p->tn : 0;
        sp->ty0 = sp->ty1 = sp->y;
        for (int t = 0; t < sp->tn; t++) {
            float tx, ty;
            to_screen(u, p->tx[t] * 0.01f, p->ty[t] * 0.01f, &tx, &ty);
            if (!in_view(u, tx, ty)) {                  // only inside the scope
                sp->tx8[t] = TRAIL_GAP;
                continue;
            }
            sp->tx8[t] = (int16_t)lroundf(tx * 8.0f);
            sp->ty8[t] = (int16_t)lroundf(ty * 8.0f);
            if (ty < sp->ty0) sp->ty0 = (int16_t)ty;
            if (ty + 1 > sp->ty1) sp->ty1 = (int16_t)(ty + 1);
        }
    }
    conflicts(u);

    /* labels: the picked one first, then the nearest, each tries six places round its plane
     * and takes the first that covers nothing: other labels, planes, ring numbers, corners */
    static CORE_BIG box_t used[SKY_MAX * 2 + 12];
    int nu = 0;
#if RADAR_ROUND
    for (int k = 1; k <= 4; k++) {
        int x, y;
        ring_label_at(u, k, &x, &y);
        used[nu++] = (box_t){ x - 1, y - 1, 24, 8 };
    }
    used[nu++] = (box_t){ SCX - 78, SCY + 20, 156, 62 };  // the card or the note under home
    used[nu++] = (box_t){ SCX - 42, SCY - PR + 11, 84, 12 };   // OpenStreetMap's name
#else
    if (square_look(u)) {                               // the flat square: its corners and edges
        for (int k = 0; k < SQ_BOXES; k++) used[nu++] = sq_box(u, k);
    } else {
        const int edge = scope_edge(u);
        for (int k = 1; k <= 4; k++) {
            int x, y;
            ring_label_at(u, k, &x, &y);
            used[nu++] = (box_t){ x - 1, y - 1, 24, 8 };
        }
        used[nu++] = (box_t){ 0, 0, 92, u->opt.clock ? CLOCK_Y + 8 : 30 };   // the corner texts
        int nw = font_width(&FONT_SMALL, home_name(u));   // the town's name, as far as corners() lets it run
        if (nw > view_cx(u) - 40) nw = view_cx(u) - 40;
        if (10 + nw > 92) used[nu++] = (box_t){ 0, 0, 10 + nw, 17 };
        if (panel_shown(u)) used[nu++] = (box_t){ PX - 82, 0, 82, 34 };
        else used[nu++] = (box_t){ look_x(u) - 8, 0, GFX_W - look_x(u) + 8, 34 };   // the look and the three dots
        used[nu++] = (box_t){ 0, GFX_H - 24, 150, 24 };
        used[nu++] = (box_t){ edge - 66, GFX_H - 31, 66, 31 };
        used[nu++] = (box_t){ view_cx(u) - 42, SCY + PR - 18, 84, 12 };
#if RADAR_HAS_TOUCH
        used[nu++] = (box_t){ GEAR_X(u) - 2, GEAR_Y - 2, GEAR_W + 4, GEAR_W + 4 };
#endif
        note_box_t nb;                                  // without the panel the note is under the town's name
        if (!panel_shown(u) && update_note_box(u, &nb)) used[nu++] = (box_t){ nb.x, nb.y, nb.w, nb.h };
    }
#endif
    if (u->cfg.look != LOOK_RADAR) {
        const int s = (int)(plane_size() * 8.0f);       // the pin on home: a label under it can't be read
        used[nu++] = (box_t){ view_cx(u) - s, SCY - s * 5 / 2, s * 2, s * 5 / 2 + 2 };
    }
    for (int k = 0; k < u->n_order; k++) {
        const spot_t *sp = &u->spot[u->order[k]];
        if (sp->vis == 1) used[nu++] = (box_t){ sp->x - 6, sp->y - 6, 12, 12 };
    }
    int most = GFX_H > 240 ? 9 : RADAR_ROUND ? 4 : 6;    // a bigger scope has room for more
    int budget = u->cfg.labels ? 1 : (sel >= 0 ? most - 1 : most), placed = 0;
    for (int j = -1; j < u->n_order && placed < budget; j++) {
        int i = j < 0 ? sel : u->order[j];
        if (i < 0 || (j >= 0 && i == sel)) continue;
        if (u->cfg.labels && i != sel) continue;
        const plane_t *p = &s->p[i];
        spot_t *sp = &u->spot[i];
        if (sp->vis != 1) continue;
        int w = font_width(&FONT_SMALL, plane_name(p));
        if (w > LABEL_W) w = LABEL_W;
        int wa = data_line(u, NULL, 0, 0, p, 0);
        if (wa > w) w = wa;
        w += 5;
        const int cand[6][2] = { { 8, -9 }, { -8 - w, -9 }, { 5, -LABEL_H - 5 }, { 5, 6 }, { -5 - w, -LABEL_H - 5 }, { -5 - w, 6 } };
        for (int t = 0; t < 6; t++) {
            box_t r = { sp->x + cand[t][0], sp->y + cand[t][1], w, LABEL_H };
            if (r.x < 3 || r.x + r.w > (square_look(u) ? view_right(u) : scope_edge(u)) - 5 || r.y < 3 || r.y + r.h > GFX_H - 3) continue;
#if RADAR_ROUND
            int out = 0;                                // every corner of it on the glass
            for (int q = 0; q < 4; q++) {
                int qx = (q & 1 ? r.x + r.w : r.x) - view_cx(u), qy = (q & 2 ? r.y + r.h : r.y) - SCY;
                out |= qx * qx + qy * qy > (SR - 2) * (SR - 2);
            }
            if (out) continue;
#endif
            int clash = 0;
            for (int q = 0; q < nu && !clash; q++) {
                const box_t *o = &used[q];
                // its own plane is in the list too: that one may touch
                if (o->x == sp->x - 6 && o->y == sp->y - 6 && o->w == 12) continue;
                clash = overlap(r, *o);
            }
            if (clash) continue;
            sp->lx = (int16_t)r.x; sp->ly = (int16_t)r.y; sp->lw = (int16_t)r.w;
            if (nu < (int)(sizeof used / sizeof used[0])) used[nu++] = r;
            placed++;
            break;
        }
    }
}

int scope_pick(const ui_t *u, int x, int y)
{
    int best = -1, bd = 18 * 18;
    for (int i = 0; i < u->snap.n; i++) {
        const spot_t *sp = &u->spot[i];
        if (!sp->vis) continue;
        int dx = sp->x - x, dy = sp->y - y, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* --- the scope --- */

// the compass letters and numbers, they turn with the map (its ticks and cross are in disc()). and the mark for the top
static void compass(ui_t *u, canvas_t *cv)
{
    char buf[8];
    if (!gfx_touches(cv, SCY - SR - 10, SCY + SR)) return;
    const int cx = view_cx(u);
    for (int d = 0; d < 360 && !LAYER_BG; d += 30) {   // where they stand comes from the tick lines (scope_frame)
        int k = d / 5;
        int lx = cx + ((RL * s_ts[k] + 8192) >> 14), ly = SCY - ((RL * s_tc[k] + 8192) >> 14);
        if (!gfx_touches(cv, ly - 14, ly + 8)) continue;   // not in this strip: no letters looked up for nothing
        if (d % 90 == 0) {                             // the smooth font: the pixel one doubled reads N as an arch
            const char *L = d == 0 ? "N" : d == 90 ? "E" : d == 180 ? "S" : "W";
            const font_t *f = d == 0 ? &FONT_MEDIUM : &FONT_SMALL;
            font_text(cv, f, lx - font_width(f, L) / 2, ly - FONT_ASCENT(f) + (d == 0 ? 5 : 4), L, d == 0 ? C_FOCUS : C_PRIM, 255);
        } else if (u->cfg.look == LOOK_RADAR) {        // the maps have their grid instead
            snprintf(buf, sizeof buf, "%d", d);
            micro_text(cv, lx - micro_width(buf, 1) / 2, ly - 2, buf, C_PRIM, 200, 1);
        }
    }
    if (LAYER_TOP) return;   // the mark for the top stays put: the background's
    float ty = (float)(SCY - SR - 8);
    if (ty >= 0) gfx_tri(cv, cx - 4.0f, ty, cx + 4.0f, ty, (float)cx, ty + 5.0f, C_FOCUS, 220);   // the top of the screen
}

static void ring_numbers(ui_t *u, canvas_t *cv)
{
    char buf[12];
    float range = u->cfg.range;
    for (int k = u->cfg.look == LOOK_RADAR ? 1 : 4; k <= 4; k++) {   // the maps: only the ring, the grid tells the rest
        int x, y;
        ring_label_at(u, k, &x, &y);
        if (!gfx_touches(cv, y - 1, y + 7)) continue;
        ring_value(range * (float)k / 4.0f, buf, sizeof buf);
        micro_text(cv, x, y, buf, k == 4 ? C_PRIM : C_DIM, 220, 1);
    }
}

#define INK RGB(16, 20, 30)            /* outlines on the classic map's light ground */

/* the middle of the radar: a small ring under the planes. no pin here: the scope's own rings and
 * cross say where home is, and a pin turns it from a radar into a map */
static void home_ring(ui_t *u, canvas_t *cv)
{
    const float cx = (float)view_cx(u);
    gfx_ring(cv, cx, SCY, 4.5f, 1.2f, C_FOCUS, 230);
    gfx_disc(cv, cx, SCY, 1.5f, C_FOCUS, 255);
}

/* where you are on a map: a pin, its tip on home, over the planes. a colour of its own and a dark
 * edge on every ground: the map is full of white roads and white planes, and a small white ring got
 * lost among them. the head's radius, the side and the tip to the head's middle are a 5 12 13 triangle */
static void home(ui_t *u, canvas_t *cv)
{
    const float s = plane_size(), x = (float)view_cx(u), y = (float)SCY;
    const float r = 5.0f * s, hy = y - 13.0f * s;      // the head
    const float tx = 4.615f * s, ty = y - 11.077f * s; // where the sides meet it
    if (!gfx_touches(cv, (int)(hy - r - 3), (int)(y + 3))) return;
    gfx_disc(cv, x, hy, r + 1.4f, INK, 255);
    gfx_line(cv, x, y, x - tx, ty, 2.8f, INK, 255);
    gfx_line(cv, x, y, x + tx, ty, 2.8f, INK, 255);
    gfx_tri(cv, x, y, x - tx, ty, x + tx, ty, C_HOME, 255);
    gfx_disc(cv, x, hy, r, C_HOME, 255);
    gfx_disc(cv, x, hy, r * 0.4f, INK, 255);
}

/* --- the flat looks: meridians and parallels, a round number of degrees or minutes apart --- */

// the part of a line inside the circle round the scope's middle (cx, SCY), 0 if none
static int clip_circle(float cx, float *ax, float *ay, float *bx, float *by, float r)
{
    float x0 = *ax - cx, y0 = *ay - SCY, dx = *bx - *ax, dy = *by - *ay;
    float a = dx * dx + dy * dy, b = 2 * (x0 * dx + y0 * dy), c = x0 * x0 + y0 * y0 - r * r;
    float d = b * b - 4 * a * c;
    if (a < 1e-6f || d <= 0) return 0;
    float s = sqrtf(d), t0 = (-b - s) / (2 * a), t1 = (-b + s) / (2 * a);
    if (t0 < 0) t0 = 0;
    if (t1 > 1) t1 = 1;
    if (t0 >= t1) return 0;
    float ox = *ax, oy = *ay;
    *ax = ox + dx * t0;
    *ay = oy + dy * t0;
    *bx = ox + dx * t1;
    *by = oy + dy * t1;
    return 1;
}

// 48°50'N, or 48°N when the minutes are 0
static void fmt_deg(float v, int lat, char *buf, int sz)
{
    char h = lat ? (v < 0 ? 'S' : 'N') : (v < 0 ? 'W' : 'E');
    int m = (int)lroundf(fabsf(v) * 60.0f), d = m / 60;
    m %= 60;
    if (m) snprintf(buf, (size_t)sz, "%d\xC2\xB0%02d'%c", d, m, h);
    else snprintf(buf, (size_t)sz, "%d\xC2\xB0%c", d, h);
}

/* the grid is worked out once a frame (grid_frame) and kept here: a strip only draws what is in
 * it. worked out in every strip it was 38 ms of the 3.5"'s 110, the C3 did it in soft floats */
#define GRID_LINES 20

typedef struct { float ax, ay, bx, by; } piece_t;
typedef struct {
    piece_t p[6];              /* the line's pieces on the screen */
    int8_t  n;                 /* how many. -1: a whole row, ya (the square map's parallels) */
    int16_t ya, yb;            /* the rows they cover */
    int16_t lx, ly, lw;        /* its name's card, 11 high, the name 2 px in. lw 0: no room for it */
    char    t[16];             /* the name: 48°50'N */
} gline_t;

static CORE_BIG gline_t s_grid[GRID_LINES];
static int     s_ngrid;

// a meridian or a parallel more, NULL when the store is full
static gline_t *grid_add(int8_t n)
{
    if (s_ngrid >= GRID_LINES) return NULL;
    gline_t *g = &s_grid[s_ngrid++];
    g->n = n;
    g->ya = INT16_MAX;
    g->yb = INT16_MIN;
    g->lw = 0;
    return g;
}

static void piece_add(gline_t *g, float ax, float ay, float bx, float by)
{
    g->p[g->n++] = (piece_t){ ax, ay, bx, by };
    int ya = (int)floorf(fminf(ay, by)) - 2, yb = (int)ceilf(fmaxf(ay, by)) + 2;   // a 1 px line reaches 1.5 px out
    if (ya < g->ya) g->ya = (int16_t)ya;
    if (yb > g->yb) g->yb = (int16_t)yb;
}

// meridians this many minutes apart: no closer than this on the screen
static int grid_step(const ui_t *u)
{
    static const int STEP[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200 };
    const int nsteps = (int)(sizeof STEP / sizeof STEP[0]);
    const float k = k_px(u), cl = cosf(u->cfg.lat * D2R);
    for (int i = 0; i < nsteps; i++)
        if (STEP[i] / 60.0f * 111.32f * cl * k >= PR * 0.42f) return STEP[i];
    return STEP[nsteps - 1];
}

#if RADAR_ROUND

// a grid line's name, just inside the ring where the line meets it
static void grid_name_at(gline_t *g, float x, float y)
{
    const int w = font_width(&FONT_SMALL, g->t), h = 9;
    float ux = x - SCX, uy = y - SCY, l = sqrtf(ux * ux + uy * uy);
    if (l < 1.0f) return;
    ux /= l;
    uy /= l;
    float in = fabsf(ux) * w * 0.5f + fabsf(uy) * h * 0.5f + 3.0f;   // the box's middle, this far in from the ring
    int bx = (int)lroundf(x - ux * in - w * 0.5f), by = (int)lroundf(y - uy * in - h * 0.5f);
    g->lx = (int16_t)(bx - 2);
    g->ly = (int16_t)(by - 1);
    g->lw = (int16_t)(w + 4);
}

// the round glass: the lines inside the range ring
static void grid_frame(ui_t *u)
{
    s_ngrid = 0;
    const fr_cfg_t *c = &u->cfg;
    if (u->view != VIEW_MAIN || c->look == LOOK_RADAR) return;
    const float R = range_km(c) * 1.05f, cl = cosf(c->lat * D2R);
    const float sd = grid_step(u) / 60.0f, r = (float)PR - 1.0f;
    // parallels: a latitude is a distance north, straight across. named at the east end
    const float dla = R / 110.574f;
    for (long n = (long)ceilf((c->lat - dla) / sd); n * sd <= c->lat + dla; n++) {
        float la = n * sd, north = (la - c->lat) * 110.574f, ax, ay, bx, by;
        to_screen(u, -R * 1.5f, north, &ax, &ay);
        to_screen(u, R * 1.5f, north, &bx, &by);
        if (!clip_circle(SCX, &ax, &ay, &bx, &by, r)) continue;
        gline_t *g = grid_add(0);
        if (!g) return;
        piece_add(g, ax, ay, bx, by);
        fmt_deg(la, 1, g->t, sizeof g->t);
        grid_name_at(g, bx, by);
    }
    // meridians: a longitude's east shrinks going north (sky.c's projection), so a few pieces. named at the north end
    const float dlo = R / (111.32f * cl);
    for (long n = (long)ceilf((c->lon - dlo) / sd); n * sd <= c->lon + dlo; n++) {
        gline_t *g = grid_add(0);
        if (!g) return;
        float lo = n * sd, px = 0, py = 0, lx = 0, ly = 0;
        for (int s = 0; s <= 6; s++) {
            float la = c->lat - dla + 2.0f * dla * s / 6.0f, x, y;
            to_screen(u, (lo - c->lon) * 111.32f * cosf((c->lat + la) * 0.5f * D2R), (la - c->lat) * 110.574f, &x, &y);
            float ax = px, ay = py, bx = x, by = y;
            if (s && clip_circle(SCX, &ax, &ay, &bx, &by, r)) {
                piece_add(g, ax, ay, bx, by);
                lx = bx;
                ly = by;
            }
            px = x;
            py = y;
        }
        if (!g->n) {                                   // none of it inside the ring
            s_ngrid--;
            continue;
        }
        fmt_deg(lo, 0, g->t, sizeof g->t);
        grid_name_at(g, lx, ly);
    }
}

#endif

// OpenStreetMap's name wherever its map shows: its licence asks for that
static void attribution(ui_t *u, canvas_t *cv)
{
    if (!ground(u)) return;
    const char *s = "\xC2\xA9 OpenStreetMap";
    const int y = RADAR_ROUND ? SCY - PR + 12 : SCY + PR - 17;
    if (!gfx_touches(cv, y, y + 10)) return;
    const int w = font_width(&FONT_SMALL, s), cx = view_cx(u);
    int classic = u->cfg.look == LOOK_CLASSIC;
    gfx_fill(cv, cx - w / 2 - 2, y, w + 4, 10, classic ? RGB(242, 239, 233) : C_BG0, classic ? 170 : 110);
    font_text(cv, &FONT_SMALL, cx - w / 2, y, s, classic ? RGB(60, 60, 60) : C_DIM, 220);
}

#if !RADAR_ROUND

/* --- the square map: the whole screen, north up, home in the middle, a grid in degrees round its
 * edges, a scale in a corner. what the scope's rim and rings say, the grid and the scale say
 * here. the panel slides in over its right side from the three dots, or while a plane is picked:
 * the corners on the right move in with it --- */

static void sky_status(const ui_t *u, char *buf, int sz, int *bad);

static int sq_free(const ui_t *u, int x, int y, int w, int h)   // a box on the map, clear of its corners
{
    box_t b = { x, y, w, h };
    if (x < 2 || x + w > view_right(u) - 2 || y < 2 || y + h > GFX_H - 2) return 0;
    for (int k = 0; k < SQ_BOXES; k++)
        if (overlap(b, sq_box(u, k))) return 0;
    return 1;
}

static void sq_ground(ui_t *u, canvas_t *cv)
{
    const map_t *m = ground(u);
    const int classic = u->cfg.look == LOOK_CLASSIC, cx = GFX_Z(view_cx(u));
    const uint32_t base = classic ? RGB(236, 233, 226) : gfx_mix(C_BG0, C_GLASS, 150);   // where no map is (yet)
    uint32_t lut[16], col[M_CLASSES];
    uint8_t lev[M_CLASSES];
    for (int i = 0; i < 16; i++) lut[i] = base;
    float s = 1.0f;
    if (m) {
        ground_paint(u, col, lev);
        for (int i = 0; i < M_CLASSES; i++) lut[i] = classic ? col[i] : gfx_add(base, gfx_scale(col[i], lev[i]));
        s = m->kpx / k_glass(u);                       // texels a pixel: 1 unless a finger is zooming
    }
    const int32_t du = (int32_t)lroundf(s * 65536.0f);
    for (int y = cv->y0; y < cv->y0 + cv->h; y++) {
        uint32_t *row = gfx_row(cv, y);
        int iv = m ? (int)floorf((y - G_SCY + 0.5f) * s + m->h * 0.5f) : -1;
        if (iv < 0 || iv >= (m ? m->h : 0)) {
            for (int x = 0; x < GFX_PW; x++) row[x] = base;
            continue;
        }
        const uint32_t *tr = m->row[iv];
        const uint32_t mw = (uint32_t)m->w;
        int32_t gu = (int32_t)lroundf(((0.5f - cx) * s + m->w * 0.5f) * 65536.0f);
        for (int x = 0; x < GFX_PW; x++, gu += du) {
            uint32_t iu = (uint32_t)(gu >> 16);
            row[x] = lut[iu < mw ? (tr[iu >> 3] >> ((iu & 7) * 4)) & 15 : M_NONE];
        }
    }
}

// a card for what is written on the map: dark on the theme's map, paper on the classic one
static void sq_card(const ui_t *u, canvas_t *cv, int x, int y, int w, int h)
{
    if (!gfx_touches(cv, y, y + h)) return;
    if (u->cfg.look == LOOK_CLASSIC) paper_card(cv, x, y, w, h);
    else gfx_fill(cv, x, y, w, h, C_BG0, 200);
}

static uint32_t sq_ink(const ui_t *u) { return u->cfg.look == LOOK_CLASSIC ? CL_INK : C_FOCUS; }   // what you read first
static uint32_t sq_dim(const ui_t *u) { return u->cfg.look == LOOK_CLASSIC ? CL_DIM : C_DIM; }

// the square map: the lines across all of it
static void grid_frame(ui_t *u)
{
    s_ngrid = 0;
    const fr_cfg_t *c = &u->cfg;
    if (u->view != VIEW_MAIN || !square_look(u)) return;
    const float k = k_px(u), sd = grid_step(u) / 60.0f;
    const int cx = view_cx(u);
    // parallels: straight across, named at the left edge
    const float top = c->lat + SCY / k / 110.574f, bottom = c->lat - (GFX_H - SCY) / k / 110.574f;
    for (long n = (long)ceilf(bottom / sd); n * sd <= top; n++) {
        gline_t *g = grid_add(-1);
        if (!g) return;
        float la = n * sd, y = SCY - (la - c->lat) * 110.574f * k;
        g->ya = g->yb = (int16_t)lroundf(y);
        fmt_deg(la, 1, g->t, sizeof g->t);
        int w = font_width(&FONT_SMALL, g->t), ly = g->ya - 5;
        if (sq_free(u, 3, ly - 1, w + 4, 11)) {
            g->lx = 3;
            g->ly = (int16_t)(ly - 1);
            g->lw = (int16_t)(w + 4);
        }
    }
    // meridians: a longitude's east shrinks going north (sky.c's projection), a few pieces. named at the top
    const box_t tl = sq_box(u, 0), tr = sq_box(u, 1);
    const int try_y[3] = { 3, tl.y + tl.h + 2, tr.y + tr.h + 2 };   // at the top edge, or under a corner
    const float half = GFX_W * 0.5f / k, dlo = (half + 1.0f) / (111.32f * cosf(fmaxf(fabsf(top), fabsf(bottom)) * D2R));
    for (long n = (long)ceilf((c->lon - dlo) / sd); n * sd <= c->lon + dlo; n++) {
        gline_t *g = grid_add(0);
        if (!g) return;
        float lo = n * sd, px = 0, py = 0;
        for (int s = 0; s <= 6; s++) {
            float la = top + (bottom - top) * s / 6.0f, y = SCY - (la - c->lat) * 110.574f * k;
            float x = cx + (lo - c->lon) * 111.32f * cosf((c->lat + la) * 0.5f * D2R) * k;
            if (s) piece_add(g, px, py, x, y);
            px = x;
            py = y;
        }
        fmt_deg(lo, 0, g->t, sizeof g->t);
        float x = cx + (lo - c->lon) * 111.32f * cosf((c->lat + top) * 0.5f * D2R) * k;
        int w = font_width(&FONT_SMALL, g->t), lx = (int)lroundf(x) - w / 2;
        for (int t = 0; t < 3; t++) {
            if (!sq_free(u, lx - 2, try_y[t], w + 4, 11)) continue;
            g->lx = (int16_t)(lx - 2);
            g->ly = (int16_t)try_y[t];
            g->lw = (int16_t)(w + 4);
            break;
        }
    }
}

// a round distance that comes out 30 to 70 pixels long, in the unit of the settings
static void sq_scale(ui_t *u, canvas_t *cv, int right, int y)
{
    static const float NICE_LEN[] = { 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 20, 50, 100, 200, 500 };
    const float px_unit = unit_km(u->cfg.dist_unit) * k_px(u);   // pixels one unit long
    const int classic = u->cfg.look == LOOK_CLASSIC;
    const uint32_t ink = classic ? CL_INK : C_FOCUS;
    float len = NICE_LEN[0];
    for (int i = 0; i < (int)(sizeof NICE_LEN / sizeof NICE_LEN[0]); i++)
        if (NICE_LEN[i] * px_unit <= 70.0f) len = NICE_LEN[i];
    int w = (int)lroundf(len * px_unit), x = right - w;
    char d[16];
    snprintf(d, sizeof d, len < 1.0f ? "%.1f %s" : "%.0f %s", (double)len, unit_name(u->cfg.dist_unit));
    micro_text(cv, x - 4 - micro_width(d, 1), y - 2, d, classic ? CL_INK : C_PRIM, 240, 1);
    gfx_hline(cv, x, right, y, ink, 240);
    gfx_vline(cv, x, y - 3, y + 1, ink, 240);
    gfx_vline(cv, right, y - 3, y + 1, ink, 240);
    gfx_vline(cv, x + w / 2, y - 2, y, ink, 200);
}

/* the three dots: the panel in and out, lit while it is in. the square map's, and the radar's in the
 * corner of its panel or where that was (w: the 2.8"'s are narrower there) */
static void panel_dots(ui_t *u, canvas_t *cv, int x, int y, int w)
{
    const int pressed = u->t_down && !u->t_moved && u->t_hit == HIT_DOTS, in = panel_shown(u);
    uint32_t dot;
    if (u->cfg.look == LOOK_CLASSIC) {
        map_button(cv, x, y, w, LOOK_H, NULL, pressed ? 2 : in);
        dot = in && !pressed ? CL_BLUE : CL_INK;
        y += pressed;                                  // the dots sink with the button
    } else {
        chamfer_box(cv, x, y, w, LOOK_H, pressed || in ? C_PRIM : C_PANEL, pressed ? 200 : in ? 120 : 200,
                    pressed || in ? C_FOCUS : C_PRIM, 220);
        dot = pressed || in ? C_FOCUS : C_PRIM;
    }
    for (int i = 0; i < 3; i++) gfx_disc(cv, x + w / 2.0f, y + 4.5f + i * 5.5f, 1.6f, dot, 255);
}

static void sq_chrome(ui_t *u, canvas_t *cv)
{
    char buf[48];
    const int right = view_right(u), classic = u->cfg.look == LOOK_CLASSIC;
    const uint32_t ink = sq_ink(u), dim = sq_dim(u);
    if (gfx_touches(cv, 0, 44)) {
        int bad;
        // top left: where home is, and how the sky is
        const char *where = home_name(u);
        sky_status(u, buf, sizeof buf, &bad);
        const box_t tl = sq_box(u, 0);
        int w = font_width(&FONT_SMALL, where), ws = micro_width(buf, 1);
        w = (w > ws ? w : ws) + 12;
        if (w > tl.w - 4) w = tl.w - 4;
        sq_card(u, cv, 2, 2, w, 24);
        font_text_clip(cv, &FONT_SMALL, 7, 4, where, ink, 255, w - 8);
        micro_text(cv, 7, 17, buf, bad ? (classic ? CL_ALARM : C_ALARM) : dim, 230, 1);
        // top right: the look, the three dots, and under them north and how to zoom
        const int lx = look_x(u), pressed = u->t_down && !u->t_moved && u->t_hit == HIT_LOOK;
        const char *name = LOOK_NAME[u->cfg.look < LOOKS ? u->cfg.look : 0];
        if (classic) map_button(cv, lx, LOOK_Y, look_w(u), LOOK_H, name, pressed ? 2 : 0);
        else button(cv, lx, LOOK_Y, look_w(u), LOOK_H, name, pressed ? 2 : 0);
        panel_dots(u, cv, right - 6 - DOTS_W, LOOK_Y, DOTS_W);
        int nx = right - 9 - micro_width("NORTH UP", 1) - 9;
        sq_card(u, cv, nx - 4, LOOK_Y + LOOK_H + 4, right - 9 - nx + 6, RADAR_HAS_TOUCH ? 16 : 9);
        float ty = LOOK_Y + LOOK_H + 11.0f;
        gfx_tri(cv, nx + 0.5f, ty, nx + 6.5f, ty, nx + 3.5f, ty - 6.0f, ink, 240);
        micro_text(cv, nx + 9, LOOK_Y + LOOK_H + 6, "NORTH UP", ink, 235, 1);
        if (RADAR_HAS_TOUCH) micro_text(cv, right - 9 - micro_width("DRAG: ZOOM", 1), LOOK_Y + LOOK_H + 13, "DRAG: ZOOM", dim, 220, 1);
    }
    if (gfx_touches(cv, GFX_H - 40, GFX_H)) {
        // bottom left: the height legend. on paper each dot has a dark edge: the light colours vanish otherwise
        static const char *const L[4] = { "GND", "<10K", "<25K", "HIGH" };
        const uint32_t C[4] = { C_DIM, C_LOW, C_PRIM, C_FOCUS };
        int x = 9;
        sq_card(u, cv, 2, GFX_H - 21, sq_box(u, 2).w - 4, 19);
        for (int i = 0; i < 4; i++) {
            if (classic) gfx_disc(cv, (float)x + 2, GFX_H - 7.5f, 3.2f, INK, 200);
            gfx_disc(cv, (float)x + 2, GFX_H - 7.5f, 2.2f, C[i], 255);
            micro_text(cv, x + 7, GFX_H - 10, L[i], dim, 220, 1);
            x += 9 + micro_width(L[i], 1) + 4;
        }
        micro_text(cv, 9, GFX_H - 17, "HEIGHT", classic ? CL_INK : C_QUIET, 220, 1);
        // bottom right: the scale, and whose map it is (its licence asks for that)
        const char *osm = "\xC2\xA9 OpenStreetMap";
        const box_t br = sq_box(u, 3);
        int wo = font_width(&FONT_SMALL, osm);
        sq_card(u, cv, br.x + 2, GFX_H - 34, br.w - 4, 32);
        sq_scale(u, cv, right - 8, GFX_H - 25);
        if (ground(u)) font_text(cv, &FONT_SMALL, right - 7 - wo, GFX_H - 15, osm, dim, 230);
    }
}

#endif

// the grid grid_frame worked out, the pieces in this strip. a line first, then its name
static void grid_draw(ui_t *u, canvas_t *cv)
{
    const int classic = u->cfg.look == LOOK_CLASSIC;
    const uint32_t ink = classic ? RGB(60, 70, 95) : C_QUIET;
    const int ia = classic ? 120 : 170;
    for (int i = 0; i < s_ngrid; i++) {
        const gline_t *g = &s_grid[i];
        if (g->n < 0) {
            gfx_hline(cv, 0, GFX_W - 1, g->ya, ink, ia);
        } else if (gfx_touches(cv, g->ya, g->yb)) {
            for (int k = 0; k < g->n; k++) gfx_line(cv, g->p[k].ax, g->p[k].ay, g->p[k].bx, g->p[k].by, 1.0f, ink, ia);
        }
        if (!g->lw || !gfx_touches(cv, g->ly, g->ly + 11 + FONT_LINE_H(&FONT_SMALL))) continue;
#if RADAR_ROUND
        gfx_fill(cv, g->lx, g->ly, g->lw, 11, classic ? RGB(242, 239, 233) : C_BG0, classic ? 190 : 150);
        font_text(cv, &FONT_SMALL, g->lx + 2, g->ly, g->t, classic ? RGB(50, 55, 70) : C_DIM, 240);
#else
        sq_card(u, cv, g->lx, g->ly, g->lw, 11);
        font_text(cv, &FONT_SMALL, g->lx + 2, g->ly, g->t, sq_dim(u), 240);
#endif
    }
}

// a plane's colour: red and white in turn while it closes on another, as for an emergency
static uint32_t spot_colour(const ui_t *u, const plane_t *p, const spot_t *sp)
{
    return sp->alert ? ((u->now / 400u) & 1) ? C_ALARM : C_FOCUS : alt_colour(u, p);
}

static void planes(ui_t *u, canvas_t *cv)
{
    const sky_t *s = &u->snap;
    float k = k_px(u), size = plane_size();
    if (!gfx_touches(cv, SCY - RQ - 20, SCY + RQ + 20)) return;
    for (int q = u->n_order - 1; q >= 0; q--) {         // farthest first, the nearest on top
        int i = u->order[q];
        const plane_t *p = &s->p[i];
        const spot_t *sp = &u->spot[i];
        if (!sp->vis) continue;
        uint32_t col = spot_colour(u, p, sp);
        const int gone = plane_lost(u, p);
        if (sp->alert > i + 1 && sp->alert <= s->n) {   // the pair: one line, from the first of the two
            const spot_t *o = &u->spot[sp->alert - 1];
            if (o->vis == 1 && gfx_touches(cv, (sp->y < o->y ? sp->y : o->y) - 2, (sp->y > o->y ? sp->y : o->y) + 2))
                gfx_line(cv, sp->x, sp->y, o->x, o->y, 1.2f, C_ALARM, 210);
        }
        int near = gfx_touches(cv, sp->y - 26, sp->y + 26);   // its glow, shape and rings: only the strips they are in
        if (sp->vis == 2) {                             // outside the range: a dart past the ring
            if (!near) continue;
            float a = atan2f((float)(sp->x - view_cx(u)), (float)(SCY - sp->y));
            float ox = sinf(a), oy = -cosf(a), px = -oy, py = ox;
            gfx_tri(cv, sp->x + ox * 4, sp->y + oy * 4, sp->x - ox * 3 + px * 3, sp->y - oy * 3 + py * 3,
                    sp->x - ox * 3 - px * 3, sp->y - oy * 3 - py * 3, col, 200);
            continue;
        }
        // the trail, older is fainter. worked out in scope_frame: a strip draws only the pieces in it
        uint32_t tc = u->cfg.look == LOOK_CLASSIC ? gfx_mix(col, INK, 110) : col;   // darker on the light map
        int tn = gfx_touches(cv, sp->ty0 - 2, sp->ty1 + 2) ? sp->tn : 0;
        int px8 = 0, py8 = 0, drawn = 0;
        for (int t = 0; t < tn; t++) {
            int x8 = sp->tx8[t], y8 = sp->ty8[t];
            if (x8 == TRAIL_GAP) { drawn = 0; continue; }
            int al = 40 + 120 * (t + 1) / (tn + 1);
            int ya = (drawn && py8 < y8 ? py8 : y8) >> 3, yb = (drawn && py8 > y8 ? py8 : y8) >> 3;
            if (gfx_touches(cv, ya - 2, yb + 2)) {
                if (drawn) gfx_line(cv, px8 * 0.125f, py8 * 0.125f, x8 * 0.125f, y8 * 0.125f, 1.0f, tc, al / 2);
                gfx_dot(cv, x8 * 0.125f, y8 * 0.125f, tc, al);
            }
            px8 = x8; py8 = y8;
            drawn = 1;
        }
        if (drawn) gfx_line(cv, px8 * 0.125f, py8 * 0.125f, sp->x, sp->y, 1.0f, tc, 80);
        int picked = p->icao == u->sel;
        float head = screen_ang(u, p->track);
        int speed_line = sp->vx != sp->x || sp->vy != sp->y;
        if (picked) {                                    // where it will be in a minute
            float km = p->gs * 1.852f / 60.0f, a = head * D2R;
            gfx_line(cv, (float)view_cx(u), SCY, sp->x, sp->y, 1.0f, C_FOCUS, 60);
            if (speed_line) gfx_line(cv, sp->x, sp->y, sp->vx, sp->vy, 1.2f, C_FOCUS, 170);
            else gfx_line(cv, sp->x, sp->y, sp->x + km * k * sinf(a), sp->y - km * k * cosf(a), 1.2f, C_FOCUS, 170);
            gfx_glow(cv, sp->x, sp->y, 16.0f, C_FOCUS, 100);
        } else if (speed_line && gfx_touches(cv, (sp->y < sp->vy ? sp->y : sp->vy) - 2, (sp->y > sp->vy ? sp->y : sp->vy) + 2)) {
            gfx_line(cv, sp->x, sp->y, sp->vx, sp->vy, 1.0f, col, 130);
        }
        if (!near) continue;
        if (u->cfg.look == LOOK_CLASSIC) {               // light ground: a dark edge round it, light does not show
            silhouette(cv, p->kind, sp->x, sp->y, head, size * 1.35f, INK, 200);
        } else if (!gone) {
            // phosphor: brighter for a moment after the beam went over it
            float since = u->cfg.look == LOOK_RADAR ? wrap360(u->beam - wrap360(atan2f((float)(sp->x - view_cx(u)), (float)(SCY - sp->y)) / D2R)) : 180.0f;
            int glow = (int)(70.0f + 100.0f * (1.0f - since / 360.0f));
            gfx_glow(cv, sp->x, sp->y, 10.0f * size, col, glow);
        }
        silhouette(cv, p->kind, sp->x, sp->y, head, size, gfx_mix(col, C_FOCUS, 60), gone ? 110 : 255);
        uint32_t age = u->now - p->first;
        if (age < 1600u) {                               // just showed up: a ring opens round it
            float f = (float)age / 1600.0f;
            gfx_ring(cv, sp->x, sp->y, 5.0f + 16.0f * f, 1.0f, col, (int)(200 * (1.0f - f)));
        }
        if (picked) {                                    // four arcs turning round it, once in 3 s
            float rot = (float)(u->now % 3000u) * (360.0f / 3000.0f);
            for (int a4 = 0; a4 < 4; a4++)
                gfx_arc(cv, sp->x, sp->y, 12.0f, rot + a4 * 90.0f - 20.0f, rot + a4 * 90.0f + 20.0f, 1.4f, C_FOCUS, 230);
        }
    }
    // labels on top of every plane: a dark card, a bar of the plane's colour, a line to it
    for (int i = 0; i < s->n; i++) {
        const plane_t *p = &s->p[i];
        const spot_t *sp = &u->spot[i];
        if (!sp->lw || sp->vis != 1) continue;
        int ta = sp->ly < sp->y ? sp->ly : sp->y, tb = sp->ly + LABEL_H > sp->y ? sp->ly + LABEL_H : sp->y;
        if (!gfx_touches(cv, ta - 2, tb + 2)) continue;  // the card and its line to the plane
        uint32_t col = spot_colour(u, p, sp);
        int picked = p->icao == u->sel;
        gfx_fill(cv, sp->lx, sp->ly, sp->lw, LABEL_H, C_BG0, 150);
        gfx_vline(cv, sp->lx, sp->ly, sp->ly + LABEL_H - 1, col, 230);
        font_text_clip(cv, &FONT_SMALL, sp->lx + 3, sp->ly, plane_name(p), picked ? C_FOCUS : gfx_mix(col, C_FOCUS, 90), 255, LABEL_W);
        data_line(u, cv, sp->lx + 3, sp->ly + 11, p, col);
        float nx = clampf((float)sp->x, (float)sp->lx, (float)(sp->lx + sp->lw - 1));
        float ny = clampf((float)sp->y, (float)sp->ly, (float)(sp->ly + LABEL_H - 1));
        float dx = nx - sp->x, dy = ny - sp->y, l = sqrtf(dx * dx + dy * dy);
        if (l > 7.0f) gfx_line(cv, sp->x + dx / l * 5.5f, sp->y + dy / l * 5.5f, nx, ny, 1.0f, col, 120);
    }
}

// a finger on the scope: the rim lights where it turns the map, a ring shows the zoom under it
static void gesture(ui_t *u, canvas_t *cv)
{
    if (!u->t_down || !u->t_moved || !u->t_turn) return;
    const float cx = (float)view_cx(u), dx = (float)u->t_x - cx, dy = (float)(SCY - u->t_y);
    if (u->t_turn == 1) {
        float a = atan2f(dx, dy) / D2R;
        gfx_arc(cv, cx, SCY, SR + 3.0f, a - 24.0f, a + 24.0f, 2.2f, C_FOCUS, 220);
        float n = screen_ang(u, 0) * D2R;              // where north is now
        gfx_line(cv, cx, SCY, cx + PR * sinf(n), SCY - PR * cosf(n), 1.2f, C_FOCUS, 120);
    } else {
        float r = sqrtf(dx * dx + dy * dy);
        if (r > PR) r = PR;
        gfx_ring(cv, cx, SCY, r, 1.3f, C_FOCUS, 170);
        char d[16];
        fmt_dist(u, range_km(&u->cfg) * r / PR, d, sizeof d);
        int w = micro_width(d, 1);
        int x = u->t_x - w / 2, y = u->t_y - 26;
        gfx_fill(cv, x - 3, y - 2, w + 6, 9, C_BG0, 190);
        micro_text(cv, x, y, d, C_FOCUS, 255, 1);
    }
}

// home as the screen says it: two decimals (a kilometre), N S E W instead of signs
static void home_coords(const ui_t *u, char *buf, int sz)
{
    float la = u->cfg.lat, lo = u->cfg.lon;
    snprintf(buf, (size_t)sz, "%.2f%c %.2f%c", (double)fabsf(la), la < 0 ? 'S' : 'N', (double)fabsf(lo), lo < 0 ? 'W' : 'E');
}

// the radar's clock, 0: it is not shown. dashes till the board has heard the time
static int clock_text(const ui_t *u, char *buf, int sz)
{
    if (!u->opt.clock || u->cfg.look != LOOK_RADAR) return 0;
    uint32_t s = u->net.utc % 86400u;
    if (u->net.utc) snprintf(buf, (size_t)sz, "%02u:%02u UTC", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u));
    else snprintf(buf, (size_t)sz, "--:-- UTC");
    return 1;
}

// where the planes come from and how fresh they are, or what is wrong. bad: say it in the alarm colour
static void sky_status(const ui_t *u, char *buf, int sz, int *bad)
{
    if (u->net.demo) snprintf(buf, (size_t)sz, u->net.configured ? "NO WIFI" : "NO SETUP YET");
    else if (!u->net.wifi) snprintf(buf, (size_t)sz, u->snap.status == SKY_CONNECTING ? "CONNECTING" : "NO WIFI");
    else if (u->snap.status == SKY_OK) {
        uint32_t age = (u->now - u->snap.updated) / 1000u;
        snprintf(buf, (size_t)sz, "%s %uS", u->net.server, (unsigned)age);
    } else snprintf(buf, (size_t)sz, "%s", u->snap.note[0] ? u->snap.note : "WAITING");
    *bad = !u->net.demo && u->snap.status != SKY_CONNECTING && (!u->net.wifi || u->snap.status > SKY_OK);
}

/* --- no WiFi: the demo sky, said in big letters, why, and the way to the WiFi --- */

static void demo_why(const ui_t *u, char *buf, int sz)
{
    if (!u->net.configured) snprintf(buf, (size_t)sz, "NO WIFI SET UP");
    else if (u->net.why == WHY_PASSWORD) snprintf(buf, (size_t)sz, "%s: WRONG PASSWORD?", u->net.ssid);
    else snprintf(buf, (size_t)sz, "CAN'T SEE WIFI %s", u->net.ssid);
}

// a network that is set but not there is a fault; none set yet is only news
static uint32_t demo_why_col(const ui_t *u)
{
    if (u->cfg.look == LOOK_CLASSIC) return u->net.configured ? CL_ALARM : CL_DIM;
    return u->net.configured ? C_ALARM : C_DIM;
}

// the first seconds of a board sold with different screens belong to the card that says how to
// pick one (boot_banner): the demo's card waits till then, the two would cover each other
static int demo_shown(const ui_t *u)
{
    return u->net.demo && !(ui_screen_name && u->now < UI_SCREEN_PICK_MS);
}

#if !RADAR_ROUND
/* under home, in the scope or on the map: DEMO in the pixel font this many times over, so it reads
 * across the room, the reason, and SET UP WIFI (the RPi LCD has no touch: a line about the site) */
demo_box_t demo_box(const ui_t *u)
{
    demo_box_t d;
    d.sc = GFX_H >= 320 ? 5 : 4;
    d.bw = GFX_H >= 320 ? 160 : 132;
    d.bh = UI_WIFI_PAGE ? (GFX_H >= 320 ? 30 : 24) : 7;
    d.w = d.bw + 40;
    d.h = 5 + 5 * d.sc + 5 + 10 + 5 + d.bh + 6;
    d.x = view_cx(u) - d.w / 2;
    d.y = SCY + 8;
    d.bx = view_cx(u) - d.bw / 2;
    d.by = d.y + d.h - 6 - d.bh;
    return d;
}

static void demo_card(ui_t *u, canvas_t *cv)
{
    if (!demo_shown(u)) return;
    const demo_box_t d = demo_box(u);
    if (!gfx_touches(cv, d.y, d.y + d.h)) return;
    const int classic = u->cfg.look == LOOK_CLASSIC, cx = view_cx(u);
    char why[64];
    demo_why(u, why, sizeof why);
    if (classic) paper_card(cv, d.x, d.y, d.w, d.h);
    else chamfer_box(cv, d.x, d.y, d.w, d.h, C_BG0, 215, C_FOCUS, 200);
    micro_text(cv, cx - micro_width("DEMO", d.sc) / 2, d.y + 5, "DEMO", classic ? CL_INK : C_FOCUS, 255, d.sc);
    int ww = font_width(&FONT_SMALL, why);
    if (ww > d.w - 12) ww = d.w - 12;
    font_text_clip(cv, &FONT_SMALL, cx - ww / 2, d.y + 10 + 5 * d.sc, why, demo_why_col(u), 245, d.w - 12);
#if UI_WIFI_PAGE
    const int pressed = u->t_down && !u->t_moved && u->t_hit == HIT_WIFI;
    if (classic) map_button(cv, d.bx, d.by, d.bw, d.bh, "SET UP WIFI", pressed ? 2 : 0);
    else button(cv, d.bx, d.by, d.bw, d.bh, "SET UP WIFI", pressed ? 2 : 1);
#else
    const char *s = "SEND WIFI FROM THE SITE";
    micro_text(cv, cx - micro_width(s, 1) / 2, d.by + 1, s, classic ? CL_INK : C_PRIM, 235, 1);
#endif
}
#endif

#if !RADAR_ROUND

/* --- around the scope and the panel next to it --- */

static void bracket(canvas_t *cv, float x, float y, float sx, float sy)
{
    gfx_line(cv, x, y + sy * 20, x, y + sy * 6, 1.2f, C_PRIM, 170);
    gfx_line(cv, x, y + sy * 6, x + sx * 6, y, 1.2f, C_PRIM, 170);
    gfx_line(cv, x + sx * 6, y, x + sx * 20, y, 1.2f, C_PRIM, 170);
}

static void corners(ui_t *u, canvas_t *cv)
{
    char buf[48];
    const int edge = scope_edge(u), cx = view_cx(u), panel = panel_shown(u);
    const int right = view_right(u) - 4;               // the brackets: off the panel, or as far in as on the left
    bracket(cv, 3, 3, 1, 1);
    if (panel) bracket(cv, (float)right, 3, -1, 1);    // without the panel the three dots are in that corner
    bracket(cv, 3, GFX_H - 4, 1, -1);
    bracket(cv, (float)right, GFX_H - 4, -1, -1);
    gfx_hatch(cv, cx - 31, 3, 62, 4, C_QUIET, 120);
    gfx_hatch(cv, cx - 31, GFX_H - 7, 62, 4, C_QUIET, 120);
    if (gfx_touches(cv, 0, 30)) {
        int bad;
        // where home is, in the smooth font: a town's name may have letters the pixel font hasn't
        font_text_clip(cv, &FONT_SMALL, 9, 6, home_name(u), C_FOCUS, 255, cx - 40);
        sky_status(u, buf, sizeof buf, &bad);
        micro_text(cv, 9, 18, buf, bad ? C_ALARM : C_DIM, 230, 1);
        // the look: a tap goes to the next one
        button(cv, look_x(u), LOOK_Y, look_w(u), LOOK_H, LOOK_NAME[u->cfg.look < LOOKS ? u->cfg.look : 0],
               u->t_down && !u->t_moved && u->t_hit == HIT_LOOK ? 2 : 0);
        const char *up = u->cfg.up ? buf : "NORTH UP";
        if (u->cfg.up) snprintf(buf, sizeof buf, "UP %03d*", u->cfg.up);
        micro_text(cv, edge - 9 - micro_width(up, 1), LOOK_Y + LOOK_H + 4, up, u->cfg.up ? C_FOCUS : C_QUIET, 235, 1);
    }
    if (gfx_touches(cv, CLOCK_Y, CLOCK_Y + 6) && clock_text(u, buf, sizeof buf))
        micro_text(cv, 9, CLOCK_Y, buf, C_DIM, 230, 1);
#if RADAR_HAS_TOUCH
    if (gfx_touches(cv, GEAR_Y, GEAR_Y + GEAR_W)) {       // the radar's options: a cog in a button
        const int gx = GEAR_X(u);
        const float gcx = gx + GEAR_W / 2.0f, gcy = GEAR_Y + GEAR_W / 2.0f, r = GEAR_W * 0.2f;
        button(cv, gx, GEAR_Y, GEAR_W, GEAR_W, "", u->t_down && !u->t_moved && u->t_hit == HIT_GEAR ? 2 : 0);
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 45.0f * D2R;
            gfx_line(cv, gcx + r * sinf(a), gcy - r * cosf(a), gcx + r * 1.65f * sinf(a), gcy - r * 1.65f * cosf(a), 2.0f, C_FOCUS, 245);
        }
        gfx_ring(cv, gcx, gcy, r, 1.8f, C_FOCUS, 245);
    }
#endif
    if (gfx_touches(cv, GFX_H - 31, GFX_H)) {             // the height legend, and how the fingers work the scope
        static const char *const L[4] = { "GND", "<10K", "<25K", "HIGH" };
        const uint32_t C[4] = { C_DIM, C_LOW, C_PRIM, C_FOCUS };
        int x = 9;
        for (int i = 0; i < 4; i++) {
            gfx_disc(cv, (float)x + 2, GFX_H - 7.5f, 2.2f, C[i], 255);
            micro_text(cv, x + 7, GFX_H - 10, L[i], C_DIM, 220, 1);
            x += 9 + micro_width(L[i], 1) + 4;
        }
        micro_text(cv, 9, GFX_H - 17, "HEIGHT", C_QUIET, 220, 1);
        char d[16];
        fmt_dist(u, range_km(&u->cfg), d, sizeof d);
        micro_text(cv, edge - 9 - micro_width(d, 1), GFX_H - 10, d, C_PRIM, 240, 1);
        if (RADAR_HAS_TOUCH) {
            micro_text(cv, edge - 9 - micro_width("DRAG: ZOOM", 1), GFX_H - 17, "DRAG: ZOOM", C_QUIET, 200, 1);
            micro_text(cv, edge - 9 - micro_width("RIM: TURN", 1), GFX_H - 24, "RIM: TURN", C_QUIET, 200, 1);
        }
    }
}

/* --- the panel ---
 * its letters go with its width (PANEL_WIDE, ui_int.h), its rows with the height: a wide panel on the
 * 4.3"'s 272 rows takes them shorter, so all a plane says still fits */
#define WIDE_ROW (GFX_H >= 320 ? 20 : 17)      /* a row of the nearest planes */
#define WIDE_PROW (GFX_H >= 320 ? 19 : 17)     /* a row of what the picked plane says */

/* the title's right end: the radar has its three dots in the panel's corner */
static int title_end(const ui_t *u) { return !square_look(u) && RADAR_HAS_TOUCH ? RDOTS_X - 4 : TX2; }

static int title_width(const char *s) { return PANEL_WIDE ? font_width(&FONT_SMALL, s) : micro_width(s, 1); }

static void title_text(canvas_t *cv, int x, const char *s, uint32_t c)
{
    if (PANEL_WIDE) font_text(cv, &FONT_SMALL, x, 4, s, c, 245);
    else micro_text(cv, x, 7, s, c, 240, 1);
}

static void panel_title(const ui_t *u, canvas_t *cv, const char *title, const char *right)
{
    if (!gfx_touches(cv, 4, 16)) return;
    const int end = title_end(u);
    int w = title_width(title), rw = right ? title_width(right) : 0;
    if (right && TX + w + 6 + rw > end) {              // no room for both: what it says on the right is more
        title = right;
        w = rw;
        right = NULL;
    }
    title_text(cv, TX, title, C_FOCUS);
    if (right) title_text(cv, end - rw, right, C_DIM);
    const int hx = TX + w + 4, hw = end - (right ? rw + 4 : 0) - hx;
    if (hw > 0) gfx_hatch(cv, hx, 7, hw, 5, C_QUIET, 130);
    gfx_hline(cv, TX, end, 15, C_QUIET, 150);
}

static int btn_x(int i) { return TX - 3 + i * (BTN_W + 3); }

// the panel's three buttons, lit: the one shown lit (-1 none)
static void buttons(ui_t *u, canvas_t *cv, const char *a, const char *b, const char *c, int lit)
{
    if (!gfx_touches(cv, BTN_Y - 1, BTN_Y + BTN_H + 1)) return;
#if RADAR_HAS_TOUCH
    const char *L[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        int pressed = u->t_down && !u->t_moved && u->t_hit == HIT_BTN + i;
        button(cv, btn_x(i), BTN_Y, BTN_W, BTN_H, L[i], pressed ? 2 : i == lit);
    }
#else
    // no touch: where the buttons would be, what BOOT does
    (void)a; (void)b; (void)c; (void)lit;
    int x = btn_x(0), w = 3 * BTN_W + 6;
    chamfer_box(cv, x, BTN_Y, w, BTN_H, C_PANEL, 170, C_QUIET, 220);
    const char *l1 = u->sel ? "BOOT: NEXT PLANE" : "BOOT: NEAREST PLANE", *l2 = "HOLD BOOT: RANGE";
    micro_text(cv, x + (w - micro_width(l1, 1)) / 2, BTN_Y + 8, l1, C_FOCUS, 250, 1);
    micro_text(cv, x + (w - micro_width(l2, 1)) / 2, BTN_Y + 18, l2, C_DIM, 240, 1);
#endif
}

/* the nearest planes. a wide panel: a row of the bigger letters, with each one's height (and its
 * type, where there is room for it) between its name and how far it is, under a line that says which
 * column is what */
#define NEAR_Y0 (PANEL_WIDE ? 89 : 72)
#define NEAR_H  (PANEL_WIDE ? WIDE_ROW : 15)
#define NEAR_N  (PANEL_WIDE ? (BTN_Y - 4 - NEAR_Y0) / NEAR_H : 6 + (GFX_H - 240) / NEAR_H)   /* a taller screen lists more */
#define NEAR_TYPE (TX2 - TX >= 200)            /* the type fits in too */
#define NEAR_DIST_R TX2                         /* the columns' right edges */
#define NEAR_ALT_R  (TX2 - 36)
#define NEAR_TYPE_X (TX2 - 102)

/* the smooth letters (font_text) find every glyph even in a strip they are not in, so each line here
 * is drawn only in the strips it is in: the 3.5" draws 40 a frame */
static void panel_overview(ui_t *u, canvas_t *cv)
{
    char buf[24];
    char d[16];
    const int wide = PANEL_WIDE;
    if (!ui_update(u) || square_look(u)) panel_title(u, cv, RADAR_NAME, 0);   // a newer version: over the radar the note is the title (main_draw)
    if (gfx_touches(cv, 19, 45)) {                     // how many, in what range
        int n = 0;
        for (int i = 0; i < u->snap.n; i++) n += u->spot[i].vis == 1;
        snprintf(buf, sizeof buf, "%d", n);
        font_text(cv, &FONT_LARGE, TX - 1, 19, buf, C_FOCUS, 255);
        int w = font_width(&FONT_LARGE, buf);
        fmt_dist(u, range_km(&u->cfg), d, sizeof d);
        snprintf(buf, sizeof buf, "IN %s", d);
        if (wide) {
            font_text(cv, &FONT_SMALL, TX + w + 6, 21, "AIRCRAFT", C_PRIM, 245);
            font_text(cv, &FONT_SMALL, TX + w + 6, 33, buf, C_DIM, 240);
        } else {
            micro_text(cv, TX + w + 4, 22, "AIRCRAFT", C_PRIM, 240, 1);
            micro_text(cv, TX + w + 4, 30, buf, C_DIM, 230, 1);
        }
    }
    // the town is in the corner: its country and the point here
    const int cy = wide ? 48 : 38, py = wide ? 64 : 50;
    if (u->cfg.country[0] && gfx_touches(cv, cy, cy + (wide ? 14 : 10)))
        font_text_clip(cv, wide ? &FONT_MEDIUM : &FONT_SMALL, TX, cy, u->cfg.country, C_DIM, 235, TX2 - TX);
    if (gfx_touches(cv, py, py + (wide ? 10 : 6))) {
        home_coords(u, buf, sizeof buf);
        if (wide) font_text(cv, &FONT_SMALL, TX, py, buf, C_QUIET, 240);
        else micro_text(cv, TX, py, buf, C_QUIET, 230, 1);
    }
    const int hy = wide ? 79 : 60;                     // NEAREST, and on a wide panel what each column is
    if (gfx_touches(cv, hy, hy + 7)) {
        micro_text(cv, TX, hy, "NEAREST", C_QUIET, 230, 1);
        if (wide) {
            const char *du = unit_name(u->cfg.dist_unit);
            if (NEAR_TYPE) micro_text(cv, NEAR_TYPE_X, hy, "TYPE", C_QUIET, 230, 1);
            micro_text(cv, NEAR_ALT_R - micro_width("HEIGHT", 1), hy, "HEIGHT", C_QUIET, 230, 1);
            micro_text(cv, NEAR_DIST_R - micro_width(du, 1), hy, du, C_QUIET, 230, 1);
        }
        gfx_hline(cv, TX, TX2, hy + 7, C_QUIET, 110);
    }
    const int name_w = !wide ? 44 : (NEAR_TYPE ? NEAR_TYPE_X : NEAR_ALT_R - 34) - 6 - (TX + 10);
    const int ny = (NEAR_H - 14) / 2;                  // a wide row: the name in it, the small letters on its line
    for (int k = 0; k < NEAR_N && k < u->n_order; k++) {
        const plane_t *p = &u->snap.p[u->order[k]];
        int y = NEAR_Y0 + k * NEAR_H;
        if (!gfx_touches(cv, y - 1, y + NEAR_H - 2)) continue;
        int pressed = u->t_down && !u->t_moved && u->t_hit == HIT_ROW + k;
        if (pressed) gfx_fill(cv, TX - 2, y - 1, TX2 - TX + 4, NEAR_H - 1, C_PRIM, 60);
        fmt_dist(u, p->dist, d, sizeof d);
        char *sp = strchr(d, ' ');
        if (sp) *sp = 0;                                // just the number: the unit is in the title, and over the column
        if (!wide) {
            gfx_disc(cv, TX + 2.5f, y + 6.0f, 2.2f, alt_colour(u, p), 255);
            font_text_clip(cv, &FONT_SMALL, TX + 8, y + 1, plane_name(p), C_FOCUS, 245, name_w);
            micro_text(cv, TX2 - micro_width(d, 1), y + 3, d, C_DIM, 230, 1);
            continue;
        }
        gfx_disc(cv, TX + 3.0f, y + NEAR_H / 2.0f - 0.5f, 2.8f, alt_colour(u, p), 255);
        font_text_clip(cv, &FONT_MEDIUM, TX + 10, y + ny, plane_name(p), C_FOCUS, 250, name_w);
        if (NEAR_TYPE && p->type[0]) font_text_clip(cv, &FONT_SMALL, NEAR_TYPE_X, y + ny + 3, p->type, C_DIM, 235, 30);
        fmt_alt(u, p, buf, sizeof buf, 1);
        font_text(cv, &FONT_SMALL, NEAR_ALT_R - font_width(&FONT_SMALL, buf), y + ny + 3, buf, C_DIM, 240);
        font_text(cv, &FONT_SMALL, NEAR_DIST_R - font_width(&FONT_SMALL, d), y + ny + 3, d, C_DIM, 240);
    }
    if (!u->n_order && gfx_touches(cv, NEAR_Y0 + 4, NEAR_Y0 + 14)) {
        const char *s = u->snap.status == SKY_OK ? "SKY IS EMPTY" : "WAITING";
        if (wide) font_text(cv, &FONT_SMALL, TX, NEAR_Y0 + 4, s, C_DIM, 240);
        else micro_text(cv, TX, NEAR_Y0 + 4, s, C_DIM, 230, 1);
    }
    buttons(u, cv, "HUE", "LIST", "SET", -1);
}

/* the picked plane's rows: on a wide panel the bigger letters */
#define PROW_H    (PANEL_WIDE ? WIDE_PROW : 13)
#define PROW_KEY  (PANEL_WIDE ? 36 : 20)

// a row: what it is, quiet, and its value. most wanted first: what finds no room stays out
static int panel_row(canvas_t *cv, int y, const char *key, const char *value, uint32_t c)
{
    if (y + PROW_H > BTN_Y) return y;
    if (gfx_touches(cv, y, y + PROW_H)) {
        if (PANEL_WIDE) font_text(cv, &FONT_SMALL, TX, y + 3, key, C_DIM, 235);
        else micro_text(cv, TX, y + 3, key, C_DIM, 235, 1);
        font_text_clip(cv, PANEL_WIDE ? &FONT_MEDIUM : &FONT_SMALL, TX + PROW_KEY,
                       y, value, c, 250, TX2 - TX - PROW_KEY);
    }
    return y + PROW_H;
}

static void panel_plane(ui_t *u, canvas_t *cv, const plane_t *p)
{
    char buf[32], b2[16];
    const int metres = u->cfg.alt_unit == ALT_M, wide = PANEL_WIDE, route = p->orig[0] || p->dest[0];
    const int dots = title_end(u) < TX2;               // the radar's three dots are in the panel's corner
    uint32_t col = alt_colour(u, p);
    if (!wide && dots) panel_title(u, cv, kind_name(p->kind), NULL);   // the 2.8": what kind it is, the same on every plane
    else panel_title(u, cv, "TRACKING", kind_name(p->kind));
    int y = (wide ? 52 : 46) + (route ? 14 : 0);       // the height's caption, under the callsign, the type and the route
    const int vy = y + (wide ? 12 : 9);                // the height's top
    if (gfx_touches(cv, 19, vy + 26)) {                // all above the rows, in the strips it is in
        // the large font is digits only. on the 2.8" the callsign goes under the dots
        font_text_clip(cv, &FONT_MEDIUM, TX, !wide && dots ? 21 : 19, p->flight[0] ? p->flight : "NO CALLSIGN", C_FOCUS, 255, PW - 10);
        snprintf(buf, sizeof buf, "%s %s", p->type[0] ? p->type : "----", p->reg);
        font_text_clip(cv, wide ? &FONT_MEDIUM : &FONT_SMALL, TX, wide ? 36 : 34, buf, C_DIM, 235, PW - 10);
        if (route) {
            snprintf(buf, sizeof buf, "%s > %s", p->orig[0] ? p->orig : "?", p->dest[0] ? p->dest : "?");
            micro_text(cv, TX, y - 13, buf, C_PRIM, 240, 2);
        }
        // the height, big. on a wide panel its caption in letters you can read too
        const char *cap = metres ? "ALTITUDE, M" : "ALTITUDE, FT";
        if (wide) font_text(cv, &FONT_SMALL, TX, y + 1, cap, C_QUIET, 240);
        else micro_text(cv, TX, y + 2, cap, C_QUIET, 230, 1);
        if (p->ground || !p->has_alt) {
            micro_text(cv, TX, vy + 3, p->ground ? "ON GROUND" : "NO DATA", col, 245, 2);
        } else {
            snprintf(buf, sizeof buf, "%d", metres ? (int)lroundf(p->alt * 0.3048f) : (int)p->alt);
            font_text(cv, &FONT_LARGE, TX - 1, vy, buf, col, 255);
        }
    }
    y += wide ? 42 : 38;
    // and what is known of it, most wanted first
    if (p->squawk == 7500 || p->squawk == 7600 || p->squawk == 7700) {
        static const char *const W[3] = { "HIJACK", "RADIO LOST", "EMERGENCY" };
        snprintf(b2, sizeof b2, "%04u", (unsigned)p->squawk);
        y = panel_row(cv, y, b2, W[p->squawk == 7500 ? 0 : p->squawk == 7600 ? 1 : 2], C_ALARM);
    }
    if (!p->ground) {
        int vr = metres ? (int)lroundf(p->vrate * 0.3048f) : p->vrate;
        if (abs(p->vrate) >= 200) snprintf(buf, sizeof buf, "%+d %s", vr, metres ? "M/M" : "FPM");
        else snprintf(buf, sizeof buf, "LEVEL");
        y = panel_row(cv, y, "V/S", buf, col);
    }
    if (p->sel_fl || p->sel_hdg) {                     // what the crew set on the autopilot
        b2[0] = 0;
        if (p->sel_fl) fmt_height(u, p->sel_fl * 100, b2, sizeof b2);
        if (p->sel_hdg) snprintf(buf, sizeof buf, "%s%s%03u\xC2\xB0", b2, b2[0] ? " " : "", (unsigned)p->sel_hdg);
        else snprintf(buf, sizeof buf, "%s", b2);
        y = panel_row(cv, y, "A/P", buf, C_PRIM);
    }
    if (p->has_spd) {
        snprintf(buf, sizeof buf, "%d KT", (int)lroundf(p->gs));
        y = panel_row(cv, y, "GS", buf, C_PRIM);
        snprintf(buf, sizeof buf, "%03d\xC2\xB0", (int)lroundf(p->track) % 360);
        y = panel_row(cv, y, "TRK", buf, C_PRIM);
    }
    if (p->ias || p->mach) {
        b2[0] = 0;
        if (p->ias) snprintf(b2, sizeof b2, "%u ", (unsigned)p->ias);
        if (p->mach) snprintf(buf, sizeof buf, "%sM.%02u", b2, (unsigned)(p->mach + 5) / 10u);
        else snprintf(buf, sizeof buf, "%sKT", b2);
        y = panel_row(cv, y, "IAS", buf, C_PRIM);
    }
    if (p->wind_dir) {
        snprintf(buf, sizeof buf, "%03u\xC2\xB0 %u KT", (unsigned)p->wind_dir, (unsigned)p->wind_kt);
        y = panel_row(cv, y, "WIND", buf, C_PRIM);
    }
    fmt_dist(u, p->dist, b2, sizeof b2);
    snprintf(buf, sizeof buf, "%s %03d\xC2\xB0", b2, (int)lroundf(p->brg) % 360);
    y = panel_row(cv, y, "DIST", buf, C_PRIM);
    if (p->squawk) {
        snprintf(buf, sizeof buf, "%04u", (unsigned)p->squawk);
        y = panel_row(cv, y, "SQK", buf, C_DIM);
    }
    buttons(u, cv, "NEXT", "WATCH", "X", watched(u, p) ? 1 : -1);   // lit: on the watch list
}

#else

/* --- the round glass: no corners, no panel. what the panel says comes in a card under home:
 * the picked plane, the HUE LIST SET row after a tap on nothing, or a note of what is up --- */

static int menu_up(const ui_t *u) { return !u->sel && (int32_t)(u->menu_until - u->now) > 0; }

static void round_card(ui_t *u, canvas_t *cv, const plane_t *p)
{
    char buf[32], d[16];
    uint32_t col = alt_colour(u, p);
    int x = SCX - CARD_W / 2, y = CARD_Y;
    if (!gfx_touches(cv, y, y + CARD_H)) return;
    int pressed = u->t_down && !u->t_moved && u->t_hit == HIT_PANEL;
    chamfer_box(cv, x, y, CARD_W, CARD_H, pressed ? C_PRIM : C_BG0, pressed ? 120 : 215, col, 220);
    font_text_clip(cv, &FONT_MEDIUM, x + 6, y + 2, p->flight[0] ? p->flight : "NO CALLSIGN", C_FOCUS, 255, 84);
    int sos = p->squawk == 7500 || p->squawk == 7600 || p->squawk == 7700;
    if (sos) snprintf(buf, sizeof buf, "%04u", (unsigned)p->squawk);
    const char *tag = sos ? buf : kind_name(p->kind);
    micro_text(cv, x + CARD_W - 6 - micro_width(tag, 1), y + 6, tag, sos ? C_ALARM : C_DIM, 235, 1);
    snprintf(buf, sizeof buf, "%s %s", p->type[0] ? p->type : "----", p->reg);
    font_text_clip(cv, &FONT_SMALL, x + 6, y + 17, buf, C_DIM, 235, CARD_W - 12);
    fmt_alt(u, p, buf, sizeof buf, 0);
    int w = font_text(cv, &FONT_MEDIUM, x + 6, y + 27, buf, col, 255);
    if (abs(p->vrate) >= 200) {                        // climbing or coming down
        float ax = x + 12.0f + w, ay = y + 34.0f;
        if (p->vrate > 0) gfx_tri(cv, ax - 3, ay + 3, ax + 3, ay + 3, ax, ay - 3, col, 240);
        else gfx_tri(cv, ax - 3, ay - 3, ax + 3, ay - 3, ax, ay + 3, col, 240);
    }
    fmt_dist(u, p->dist, d, sizeof d);
    snprintf(buf, sizeof buf, "%d KT  %s %s", (int)lroundf(p->gs), d, compass8(p->brg));
    micro_text(cv, x + 6, y + 45, buf, C_PRIM, 240, 1);
}

// what each button of the row is: HUE, the look (it says which one is on), LIST, SET
static int menu_hit(int i) { return i == 1 ? HIT_LOOK : HIT_BTN + (i ? i - 1 : 0); }

static void round_menu(ui_t *u, canvas_t *cv)
{
    const char *L[MENU_N] = { "HUE", LOOK_NAME[u->cfg.look < LOOKS ? u->cfg.look : 0], "LIST", "SET" };
    if (!gfx_touches(cv, MENU_Y, MENU_Y + MENU_H)) return;
    for (int i = 0; i < MENU_N; i++) {
        int pressed = u->t_down && !u->t_moved && u->t_hit == menu_hit(i);
        if (u->cfg.look == LOOK_CLASSIC) map_button(cv, menu_x(i), MENU_Y, MENU_W, MENU_H, L[i], pressed ? 2 : 0);   // paper on paper
        else button(cv, menu_x(i), MENU_Y, MENU_W, MENU_H, L[i], pressed ? 2 : 0);
    }
}

// no WiFi: DEMO in big letters, why, and that the site sends the WiFi (no room for a keyboard here)
static void round_demo(ui_t *u, canvas_t *cv)
{
    const int y = SCY + 34, h = 46, classic = u->cfg.look == LOOK_CLASSIC;
    if (!gfx_touches(cv, y, y + h)) return;
    char why[64];
    demo_why(u, why, sizeof why);
    const char *hint = "SEND WIFI FROM THE SITE";
    int ww = font_width(&FONT_SMALL, why), wh = micro_width(hint, 1);
    if (ww > 140) ww = 140;
    int w = (ww > wh ? ww : wh) + 14;
    if (classic) paper_card(cv, SCX - w / 2, y, w, h);
    else chamfer_box(cv, SCX - w / 2, y, w, h, C_BG0, 215, C_FOCUS, 200);
    micro_text(cv, SCX - micro_width("DEMO", 3) / 2, y + 4, "DEMO", classic ? CL_INK : C_FOCUS, 255, 3);
    font_text_clip(cv, &FONT_SMALL, SCX - ww / 2, y + 22, why, demo_why_col(u), 245, 140);
    micro_text(cv, SCX - wh / 2, y + 36, hint, classic ? CL_DIM : C_PRIM, 235, 1);
}

// where home is, how many around, where they come from: a small box under home, out of the
// planes' way as it can be
static void round_note(ui_t *u, canvas_t *cv)
{
    char top[80], mid[48], low[48], d[16], c[24];
    int n = 0, bad;
    const int y = SCY + 44, h = 32;
    if (demo_shown(u)) { round_demo(u, cv); return; }
    if (!gfx_touches(cv, y, y + h)) return;
    for (int i = 0; i < u->snap.n; i++) n += u->spot[i].vis == 1;
    const fr_cfg_t *cf = &u->cfg;
    if (cf->place[0] && cf->country[0]) snprintf(top, sizeof top, "%s, %s", cf->place, cf->country);
    else snprintf(top, sizeof top, "%s", cf->place[0] ? cf->place : cf->country[0] ? cf->country : "HOME");
    fmt_dist(u, range_km(&u->cfg), d, sizeof d);
    home_coords(u, c, sizeof c);
    snprintf(mid, sizeof mid, "%s  %d IN %s", c, n, d);
    sky_status(u, low, sizeof low, &bad);
    if (clock_text(u, c, sizeof c)) {                  // the round glass has no corners for it
        size_t l = strlen(low);
        snprintf(low + l, sizeof low - l, "  %s", c);
    }
    int wt = font_width(&FONT_SMALL, top), wm = micro_width(mid, 1), wl = micro_width(low, 1);
    int w = (wt > wm ? wt : wm);
    w = (w > wl ? w : wl) + 12;
    if (u->cfg.look == LOOK_CLASSIC) {                // on the light map: a paper card, dark ink
        paper_card(cv, SCX - w / 2, y, w, h);
        font_text(cv, &FONT_SMALL, SCX - wt / 2, y + 2, top, CL_INK, 255);
        micro_text(cv, SCX - wm / 2, y + 15, mid, CL_INK, 235, 1);
        micro_text(cv, SCX - wl / 2, y + 23, low, bad ? CL_ALARM : CL_DIM, 235, 1);
        return;
    }
    gfx_fill(cv, SCX - w / 2, y, w, h, C_BG0, 170);
    gfx_hline(cv, SCX - w / 2, SCX + w / 2 - 1, y, bad ? C_ALARM : C_QUIET, 160);
    font_text(cv, &FONT_SMALL, SCX - wt / 2, y + 2, top, C_FOCUS, 255);
    micro_text(cv, SCX - wm / 2, y + 15, mid, C_PRIM, 235, 1);
    micro_text(cv, SCX - wl / 2, y + 23, low, bad ? C_ALARM : C_DIM, 235, 1);
}

#endif

/* the site has a newer version: a small note, a tap on it opens ABOUT. over the radar it is the
 * panel's title, over the square map and the radar without its panel a card under the town's, on
 * the round glass one under its box. while a plane is picked or the round menu is up, they have
 * the place */
int update_note_box(const ui_t *u, note_box_t *b)
{
    if (!ui_update(u) || u->view != VIEW_MAIN) return 0;
    char t[24];
    update_text(u, t, sizeof t, 0);
    const int w = micro_width(t, 1) + 12;
#if RADAR_ROUND
    if (u->sel || menu_up(u) || demo_shown(u)) return 0;
    *b = (note_box_t){ SCX - w / 2, SCY + 79, w, 13 };
#else
    if (!square_look(u) && panel_shown(u)) {
        if (u->sel) return 0;
        *b = (note_box_t){ TX - 3, 3, title_end(u) - TX + 6, 14 };
    } else if (!square_look(u)) {                      // under the town's name and the clock, if it is on
        *b = (note_box_t){ 2, u->opt.clock ? CLOCK_Y + 9 : 27, w, 13 };
    } else {
        *b = (note_box_t){ 2, 28, w, 13 };
    }
#endif
    return 1;
}

void update_note(ui_t *u, canvas_t *cv)
{
    note_box_t b;
    if (!update_note_box(u, &b) || !gfx_touches(cv, b.y, b.y + b.h)) return;
    char t[24];
    update_text(u, t, sizeof t, 0);
    if (micro_width(t, 1) > b.w - 6) update_text(u, t, sizeof t, 1);   // the 2.8"'s panel title, next to the dots
    const int pressed = u->t_down && !u->t_moved && u->t_hit == HIT_NEWS;
    const int tx = b.x + (b.w - micro_width(t, 1)) / 2, ty = b.y + (b.h - MICRO_H) / 2 + 1;   // in the middle
    if (u->cfg.look == LOOK_CLASSIC) {                 // on the light map: paper, dark ink
        paper_card(cv, b.x, b.y, b.w, b.h);
        micro_text(cv, tx, ty, t, pressed ? CL_BLUE : CL_INK, 250, 1);
        return;
    }
    chamfer_box(cv, b.x, b.y, b.w, b.h, pressed ? C_PRIM : C_BG0, pressed ? 150 : 200, C_FOCUS, 220);
    micro_text(cv, tx, ty, t, C_FOCUS, 250, 1);
}

void main_draw(ui_t *u, canvas_t *cv)
{
#if !RADAR_ROUND
    if (square_look(u)) {                              // the flat square: no glass, no rim, no rings
        sq_ground(u, cv);
        grid_draw(u, cv);
        ui_lap(u, cv, 1);
        ui_lap(u, cv, 2);
        if (!LAYER_BG) planes(u, cv);
        home(u, cv);
        gesture(u, cv);
        ui_lap(u, cv, 3);
        sq_chrome(u, cv);
        update_note(u, cv);
        demo_card(u, cv);
        if (panel_shown(u)) {                          // slid in over the map: darker, so it reads on any ground
            chamfer_box(cv, PX, 2, PW, GFX_H - 4, C_PANEL, 225, C_QUIET, 220);
            const plane_t *p = sel_plane(u);
            if (p) panel_plane(u, cv, p);
            else panel_overview(u, cv);
        }
        ui_lap(u, cv, 4);
        return;
    }
#endif
    if (gfx_touches(cv, SCY - RQ - 12, SCY + RQ + 12)) {
        disc(u, cv);
        grid_draw(u, cv);
        ui_lap(u, cv, 1);
        compass(u, cv);
        ring_numbers(u, cv);
        ui_lap(u, cv, 2);
        if (u->cfg.look == LOOK_RADAR) home_ring(u, cv);
        if (!LAYER_BG) planes(u, cv);
        if (u->cfg.look != LOOK_RADAR) home(u, cv);    // the round glass: its maps keep the pin
        attribution(u, cv);
        gesture(u, cv);
        ui_lap(u, cv, 3);
    }
    const plane_t *p = sel_plane(u);
#if RADAR_ROUND
    if (p) round_card(u, cv, p);
    else if (menu_up(u)) round_menu(u, cv);
    else {
        round_note(u, cv);
        update_note(u, cv);
    }
#else
    demo_card(u, cv);
    corners(u, cv);
    if (panel_shown(u)) {
        chamfer_box(cv, PX, 2, PW, GFX_H - 4, C_PANEL, 120, C_QUIET, 200);
        if (p) panel_plane(u, cv, p);
        else panel_overview(u, cv);
    }
    update_note(u, cv);                                // a newer version: the panel's title, or a card under the town's name
#if RADAR_HAS_TOUCH
    if (gfx_touches(cv, LOOK_Y, LOOK_Y + LOOK_H)) panel_dots(u, cv, RDOTS_X, LOOK_Y, RDOTS_W);
#endif
#endif
    ui_lap(u, cv, 4);
}

#if RADAR_OVERLAY
/* a finger on the rim turns the compass: the moving layer is then the ring alone, cheap enough to be
 * made again every panel frame so it follows the finger. the planes (and the map under them) catch up
 * once the finger lets go */
static int ring_turning(const ui_t *u) { return u->t_down && u->t_turn == 1 && u->t_moved && !square_look(u); }

/* the moving layer of the main page, drawn on a canvas of nothing (gfx.h GFX_ALPHA): the compass
 * ring (ticks, letters, numbers) and the planes with all that goes with them. the sweep is not here,
 * the display driver adds it as the panel is fed (firmware/main/lcd_wave7.cpp) */
void main_top(ui_t *u, canvas_t *cv)
{
    if (u->view != VIEW_MAIN) return;
    const int was = cv->layer;
    cv->layer = UI_LAYER_TOP;
#if !RADAR_ROUND
    if (square_look(u)) {
        planes(u, cv);
        cv->layer = was;
        return;
    }
#endif
    if (gfx_touches(cv, SCY - RQ - 12, SCY + RQ + 12)) {
        ticks_top(u, cv);
        compass(u, cv);
        if (!ring_turning(u)) planes(u, cv);
    }
    cv->layer = was;
}
#endif

#if RADAR_OVERLAY
void ui_top_info(const ui_t *u, ui_top_t *t)
{
    t->beam = u->view == VIEW_MAIN && u->cfg.look == LOOK_RADAR && !square_look(u);
    t->cx = scope_glass_cx(u);
    t->cy = G_SCY;
    t->r = G_PR;
    t->prim = C_PRIM;
    t->focus = C_FOCUS;
    t->turn_ms = BEAM_TURN_MS;
    t->clip_x = !RADAR_ROUND && square_look(u) && panel_shown(u) ? GFX_Z(PX - 2) : GFX_PW;
    t->any = u->view == VIEW_MAIN;
    t->ring_only = t->any && ring_turning(u);
    /* what the app puts over the scope, drawn after the planes and the sweep there: the moving layer
     * leaves those boxes alone, and the fill saves the work */
    t->holes = 0;
#define HOLE(X, Y, W, H) do { if (t->holes < 4) { const int k = t->holes++; t->hx0[k] = GFX_Z(X); t->hy0[k] = GFX_Z(Y); \
                                                  t->hx1[k] = GFX_Z((X) + (W)); t->hy1[k] = GFX_Z((Y) + (H)); } } while (0)
#if !RADAR_ROUND
    if (t->any && demo_shown(u)) {
        const demo_box_t d = demo_box(u);
        HOLE(d.x, d.y, d.w, d.h);
    }
#endif
    if (t->any && (int32_t)(u->toast_until - u->now) > 0) {
        const int w = font_width(&FONT_MEDIUM, u->toast) + 20;
        HOLE((GFX_W - w) / 2, GFX_H / 2 - 16, w, 26);
    }
    note_box_t nb;
    if (t->any && update_note_box(u, &nb)) HOLE(nb.x, nb.y, nb.w, nb.h);
#undef HOLE
    if (!RADAR_ROUND && square_look(u)) {              // the flat map: planes anywhere on it
        t->x0 = 0; t->y0 = 0; t->x1 = t->clip_x; t->y1 = GFX_PH;
        return;
    }
    /* the scope with what sticks out of it (darts past the ring, glows, the picked plane's arcs), and
     * the planes' labels, which may stand outside it */
    const int cx = view_cx(u);
    int x0 = cx - RQ - 30, x1 = cx + RQ + 30, y0 = SCY - RQ - 30, y1 = SCY + RQ + 30;
    for (int i = 0; i < u->snap.n && !t->ring_only; i++) {
        const spot_t *sp = &u->spot[i];
        if (!sp->vis) continue;
        if (sp->x - 30 < x0) x0 = sp->x - 30;
        if (sp->x + 30 > x1) x1 = sp->x + 30;
        if (sp->y - 30 < y0) y0 = sp->y - 30;
        if (sp->y + 30 > y1) y1 = sp->y + 30;
        if (!sp->lw) continue;
        if (sp->lx - 2 < x0) x0 = sp->lx - 2;
        if (sp->lx + sp->lw + 2 > x1) x1 = sp->lx + sp->lw + 2;
        if (sp->ly - 2 < y0) y0 = sp->ly - 2;
        if (sp->ly + LABEL_H + 2 > y1) y1 = sp->ly + LABEL_H + 2;
    }
    t->x0 = GFX_Z(x0); t->x1 = GFX_Z(x1 + 1); t->y0 = GFX_Z(y0); t->y1 = GFX_Z(y1 + 1);
    if (t->x0 < 0) t->x0 = 0;
    if (t->y0 < 0) t->y0 = 0;
    if (t->x1 > t->clip_x) t->x1 = t->clip_x;
    if (t->y1 > GFX_PH) t->y1 = GFX_PH;
}
#endif

/* the first seconds after power on: a ring opens with the name, and on a board sold with
 * different screens a card says BOOT picks the next one, with the time it has left */
void boot_banner(ui_t *u, canvas_t *cv)
{
    uint32_t t = u->now - u->boot;
    const int cx = panel_shown(u) ? SCX : GFX_W / 2;   // beside the panel (the radar's, or the map's slid in), or the screen's middle
    if (t < 1800u) {
        float f = (float)t / 1800.0f;
        gfx_ring(cv, (float)cx, SCY, 8.0f + SR * f, 2.0f, C_FOCUS, (int)(230 * (1.0f - f)));
        int w = micro_width(RADAR_NAME, 2);
        gfx_fill(cv, cx - w / 2 - 4, SCY + 14, w + 8, 18, C_BG0, (int)(200 * (1.0f - f * f)));
        micro_text(cv, cx - w / 2, SCY + 18, RADAR_NAME, C_FOCUS, (int)(255 * (1.0f - f * f)), 2);
    }
    if (!ui_screen_name || u->now >= UI_SCREEN_PICK_MS || u->view != VIEW_MAIN) return;
    const int bw = 156, bh = 32, bx = cx - bw / 2, by = SCY + 40;
    if (!gfx_touches(cv, by, by + bh)) return;
    chamfer_box(cv, bx, by, bw, bh, C_BG0, 220, C_FOCUS, 230);
    const char *t1 = "SCREEN WRONG? PRESS BOOT";
    micro_text(cv, cx - micro_width(t1, 1) / 2, by + 7, t1, C_FOCUS, 250, 1);
    char t2[40];
    snprintf(t2, sizeof t2, "NOW %s", ui_screen_name);
    micro_text(cv, cx - micro_width(t2, 1) / 2, by + 16, t2, C_DIM, 235, 1);
    int left = (int)((float)(bw - 16) * (float)(UI_SCREEN_PICK_MS - u->now) / (float)UI_SCREEN_PICK_MS);
    gfx_fill(cv, bx + 8, by + bh - 5, left, 1, C_FOCUS, 200);
}

int main_hit(const ui_t *u, int x, int y)
{
#if RADAR_ROUND
    int dx = x - SCX, dy = y - SCY;
    if (dx * dx + dy * dy > (SR + 8) * (SR + 8)) return HIT_NONE;
    if (menu_up(u) && y >= MENU_Y - 2 && y < MENU_Y + MENU_H + 2)
        for (int i = 0; i < MENU_N; i++) if (x >= menu_x(i) && x < menu_x(i) + MENU_W) return menu_hit(i);
    if (u->sel && in_box(x, y, SCX - CARD_W / 2, CARD_Y, CARD_W, CARD_H)) return HIT_PANEL;
    note_box_t nb;
    if (update_note_box(u, &nb) && in_box(x, y, nb.x - 6, nb.y - 6, nb.w + 12, nb.h + 12)) return HIT_NEWS;
    return HIT_SCOPE;
#else
    const int lx = look_x(u), ly = LOOK_Y + LOOK_H + 12;
    const int sx = x - view_cx(u), sy = y - SCY;
    const int on_scope = !square_look(u) && sx * sx + sy * sy <= (SR + 6) * (SR + 6);
#if RADAR_HAS_TOUCH
    /* the radar's three dots, in the screen's corner: before the note, whose margin reaches them. over the
     * panel only the dots themselves: under them the panel is its own (a tap there picks the next plane) */
    if (!square_look(u) && y < (panel_shown(u) ? LOOK_Y + LOOK_H + 2 : ly) && x >= RDOTS_X - 4) return HIT_DOTS;
#endif
    note_box_t nb;                                     // a finger is wide: the note answers round it too, off the scope
    if (update_note_box(u, &nb) && in_box(x, y, nb.x - 6, nb.y - 6, nb.w + 12, nb.h + 12) &&
        (!on_scope || in_box(x, y, nb.x, nb.y, nb.w, nb.h))) return HIT_NEWS;
    // the buttons over the map answer further out than they are drawn: a finger is wide, a
    // resistive panel is a few pixels off
#if UI_WIFI_PAGE
    if (demo_shown(u)) {
        const demo_box_t d = demo_box(u);
        if (in_box(x, y, d.bx - 8, d.by - 8, d.bw + 16, d.bh + 16)) return HIT_WIFI;
    }
#endif
    if (square_look(u)) {
        if (y < ly && x >= lx + LOOK_W + 2 && x < view_right(u)) return HIT_DOTS;
        if (y < ly && x >= lx - 10 && x < lx + LOOK_W + 2) return HIT_LOOK;
        if (!panel_shown(u) || x < PX - 2) return HIT_SCOPE;   // the square map: all of it but the panel
    } else if (y < ly && x >= lx - 12 && x < (panel_shown(u) ? PX - 2 : RDOTS_X - 4)) {
        return HIT_LOOK;
    }
#if RADAR_HAS_TOUCH
    if (!square_look(u) && in_box(x, y, GEAR_X(u) - 8, GEAR_Y - 8, GEAR_W + 16, GEAR_W + 16)) return HIT_GEAR;
#endif
    if (x < PX - 2 || !panel_shown(u)) return on_scope ? HIT_SCOPE : HIT_NONE;   // the scope: beside the panel, or anywhere without it
    if (y >= BTN_Y - 2) {
        int b = (x - btn_x(0)) / (BTN_W + 3);
        return b >= 0 && b < 3 ? HIT_BTN + b : HIT_NONE;
    }
    if (!u->sel && y >= NEAR_Y0 && y < NEAR_Y0 + NEAR_N * NEAR_H) {
        int k = (y - NEAR_Y0) / NEAR_H;
        return k < u->n_order ? HIT_ROW + k : HIT_NONE;
    }
    return HIT_PANEL;
#endif
}
