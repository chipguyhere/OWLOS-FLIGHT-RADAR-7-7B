/* the moving layer, see ovl.h. fork addition */
#include "board.h"
#if RADAR_OVERLAY

#include <math.h>
#include <string.h>
#include "ovl.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ovl";

/* a pixel of the layer, as the fill wants it: its colour (already scaled by its coverage) spread out
 * as 565 with room between the fields, 0x07E0F81F, and in the free bits 5..10 how much of what is
 * under it shows, 0..32. a run is a word x | n << 16 and its n pixels. the rows' starts are in the
 * RAM, the runs in the PSRAM */
#define SPREAD   0x07E0F81Fu
#define OVF      0x08010020u
#define OVL_CAP  (96 * 1024)                  // words a layer, 384 KB of PSRAM each
#define LEVELS   32                           // steps of the trail's fading
/* the trail on every row (1) or every other (2): half the sweep's work in the fill, a faint scanline
 * look in the trail. the bright edge is on every row either way */
#ifndef OVL_SWEEP_EVERY
#define OVL_SWEEP_EVERY 2
#endif

/* the busiest rows (fork addition): where putting the layer on would take the fill too long, the row is
 * made whole when the layer is made (the background's row with the layer on it) and kept here; the
 * fill then only copies it in and adds the sweep */
#ifndef OVL_BUSY_ROWS
#define OVL_BUSY_ROWS   192                            // rows a layer may keep made, 2 KB each, PSRAM
#endif
#ifndef OVL_BUSY_CYCLES
#define OVL_BUSY_CYCLES 4000                           // a row whose layer would take the fill more than this (~17 us)
#endif

typedef struct {
    uint32_t  *w;
    uint32_t   n;
    uint32_t   row[GFX_PH + 1];
    ovl_beam_t beam;
    int16_t    busy[GFX_PH];                           // the row's place in rows, -1: not made
    int16_t    bx0[GFX_PH], bx1[GFX_PH];               // the stretch of it the layer covers, as the panel has it
    uint16_t   cost[GFX_PH];                           // what the row's layer costs the fill, cycles / 4: core 0 picks chunks by it without reading the PSRAM
    uint16_t  *rows;                                   // OVL_BUSY_ROWS rows as the panel shows them
    int        nbusy;
} layer_t;

static layer_t      s_l[4];                         // shown, ready, planned for the next frame, being made
static int          s_show = -1, s_ready = -1, s_build = -1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int          s_flip, s_next_y, s_full;
static TaskHandle_t s_waiter;
/* the next panel frame as core 0 planned it (ovl_plan_next): which layer and where the beam is. the
 * fill takes it at that frame's start when it is for that frame, so what core 0 composed beforehand
 * and what the fill puts on agree */
static volatile uint32_t s_plan_frame = 0xFFFFFFFFu;
static volatile int      s_plan_layer = -1;
static float             s_plan_ang;
static volatile int64_t  s_latch_us;                   // when the fill took the frame on the panel
#define PRE_ROWS   10                                  // a chunk: the library's bounce rows (LCD7_7B_BOUNCE_ROWS)
#define PRE_CHUNKS (GFX_PH / PRE_ROWS)
#ifndef PRE_PICK
#define PRE_PICK   8                                   // the heaviest chunks composed a frame: ~40 KB of PSRAM traffic each
#endif
#define PRE_MAX    PRE_PICK                            // their buffers, 2 x 8 x 20 KB of PSRAM (24 of them left the PSRAM short)
#ifndef PRE_CYCLES
#define PRE_CYCLES 40000                               // a chunk that would take the fill more than this (~170 us)
#endif
_Static_assert(GFX_PH % PRE_ROWS == 0, "the chunks must fill the frame");
static uint16_t *s_pre[2][PRE_MAX];                     // by the frame's parity
static volatile int8_t   s_pre_slot[2][PRE_CHUNKS];     // the chunk's buffer, -1: not composed
static volatile uint8_t  s_pre_what[2][PRE_CHUNKS];     // what is in it: PRE_SWEEP, PRE_LAYER
#define PRE_SWEEP 1
#define PRE_LAYER 2
#ifndef PRE_LAYER_CYCLES
#define PRE_LAYER_CYCLES 20000                         // the layer is composed too only where it costs this much (many planes)
#endif
static TaskHandle_t s_planner;
static volatile uint32_t s_pre_for[2];                  // the frame those are for
static void (*s_wb)(const void *addr, size_t len);     // written back from the cache: the DMA reads the PSRAM
static uint32_t s_pre_cost[PRE_CHUNKS];                 // the planner's last estimate of each chunk, for the log
static uint16_t s_pre_count[PRE_CHUNKS];                // how often each was composed beforehand

static volatile uint32_t s_frame = 0xFFFFFFFFu;
static ovl_stats_t  s_st;
static const uint16_t *(*s_src)(int py);              // the background's row py as the panel shows it now (lcd_wave7.c)
static void busy_render(layer_t *L);
#include "esp_cpu.h"

static const DRAM_ATTR uint8_t BAYER[4][4] = {      // gfx_pack565's
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

/* --- runs of the layer as two 16-bit arrays (fork addition): the colour, premultiplied 565, and how
 * much of what is under it shows, 0..32. both start at the same offset within 16 bytes as the panel
 * row's pixel they go to, so the S3's 128-bit SIMD (PIE) can take them 8 pixels at a time. a run
 * word: x | n << 16 (11 bits) | pad << 27 (16-bit entries before the colours) | PLANAR. shorter runs
 * stay one 32-bit word a pixel */
#define PLANAR   0x40000000u
#define PIE_MIN  16                                    // pixels a run needs to be stored for PIE

static inline unsigned planar_q(unsigned n) { return (16u - ((2u * n) & 15u)) & 15u; }   // bytes between the arrays
static inline unsigned planar_words(unsigned p, unsigned n) { return (2u * p + 4u * n + planar_q(n) + 3u) >> 2; }

// one pixel, as the C composite does it: c over *d, *d showing through u/32 of it
static IRAM_ATTR inline void pix(uint16_t *d, uint16_t c, unsigned u)
{
    uint32_t o = (c | ((uint32_t)c << 16)) & SPREAD;
    if (u) o += ((((*d | ((uint32_t)*d << 16)) & SPREAD) * u) >> 5) & SPREAD;
    if (o & OVF) {
        if (o & 0x20u) o |= 0x1Fu;
        if (o & 0x10000u) o |= 0xF800u;
        if (o & 0x08000000u) o |= 0x07E00000u;
    }
    o &= SPREAD;
    *d = (uint16_t)(o | (o >> 16));
}

/* the same, 8 pixels at a time. only the instructions chipguyhere's RasterBouncingBalls already runs on
 * this chip (vld/vst.128, andq, xorq, vmul.u16, vadds.s16, loopgtz, ssai). per field: masked out and
 * moved down with a multiply (every product below 65536, so nothing hangs on how vmul.u16 rounds),
 * scaled by what shows through, the layer's added, held to the field's top with two saturating adds
 * (+(0x7FFF - top), then back), moved up again, and the three fields put together. bit for bit what
 * pix does: checked at start (ovl_init), the C one is used if it differs */
static const uint16_t DRAM_ATTR __attribute__((aligned(16))) PIE_K[15][8] = {
#define V(v) { v, v, v, v, v, v, v, v }
    V(0xF800), V(1), V(0x7FFF - 31), V((uint16_t)-(0x7FFF - 31)), V(2048),     // red: down 11
    V(0x07E0), V(1), V(0x7FFF - 63), V((uint16_t)-(0x7FFF - 63)), V(32),       // green: down 5
    V(0x001F), V(1), V(0x7FFF - 31), V((uint16_t)-(0x7FFF - 31)), V(1),        // blue
#undef V
};
#define PIE_FIELD(DOWN, REWIND) \
    "ee.vld.128.ip q6, %[k], 16\n"  "ee.andq q4, q0, q6\n"  "ee.andq q5, q2, q6\n" \
    "ee.vld.128.ip q7, %[k], 16\n"  "ssai " DOWN "\n" \
    "ee.vmul.u16 q4, q4, q7\n"     "ee.vmul.u16 q5, q5, q7\n" \
    "ssai 5\n"                     "ee.vmul.u16 q4, q4, q1\n"  "ee.vadds.s16 q4, q4, q5\n" \
    "ee.vld.128.ip q7, %[k], 16\n"  "ee.vadds.s16 q4, q4, q7\n" \
    "ee.vld.128.ip q7, %[k], 16\n"  "ee.vadds.s16 q4, q4, q7\n" \
    "ee.vld.128.ip q7, %[k], " REWIND "\n"  "ssai 0\n"  "ee.vmul.u16 q4, q4, q7\n" \
    "ee.xorq q3, q3, q4\n"

__attribute__((noinline)) static IRAM_ATTR void pie_blend(uint16_t *d, const uint16_t *c, const uint16_t *u, int n8)
{
    const uint16_t *k = &PIE_K[0][0];
    asm volatile("loopgtz %[n], 1f\n"
                 "ee.vld.128.ip q0, %[d], 0\n"
                 "ee.vld.128.ip q1, %[u], 16\n"
                 "ee.vld.128.ip q2, %[c], 16\n"
                 "ee.xorq q3, q3, q3\n"
                 PIE_FIELD("11", "16")
                 PIE_FIELD("5", "16")
                 PIE_FIELD("0", "-224")
                 "ee.vst.128.ip q3, %[d], 16\n"
                 "1:\n"
                 : [d] "+r"(d), [u] "+r"(u), [c] "+r"(c), [n] "+r"(n8), [k] "+r"(k) :: "memory");
}

static int s_pie;                                      // the PIE path agreed with the C one at start

static void pie_selftest(void)
{
    static DRAM_ATTR uint16_t __attribute__((aligned(16))) d1[64], d2[64], cc[64], uu[64];
    uint32_t seed = 12345;
    for (int round = 0; round < 64; round++) {
        for (int i = 0; i < 64; i++) {
            seed = seed * 1103515245u + 12345u;
            d1[i] = d2[i] = (uint16_t)(seed >> 8);
            seed = seed * 1103515245u + 12345u;
            cc[i] = (uint16_t)(seed >> 8);
            seed = seed * 1103515245u + 12345u;
            uu[i] = (uint16_t)((seed >> 16) % 33);
        }
        for (int i = 0; i < 64; i++) pix(&d1[i], cc[i], uu[i]);
        pie_blend(d2, cc, uu, 8);
        for (int i = 0; i < 64; i++) {
            if (d1[i] != d2[i]) {
                ESP_LOGW(TAG, "PIE composite differs from C at pixel %d of round %d (colour %04x, shows %u: C %04x, PIE %04x): C only",
                         i, round, (unsigned)cc[i], (unsigned)uu[i], (unsigned)d1[i], (unsigned)d2[i]);
                s_pie = 0;
                return;
            }
        }
    }
    s_pie = 1;
    ESP_LOGI(TAG, "PIE composite matches C on 4096 pixels: in use");
}

static uint32_t *s_tmp;                                // a run's words while they are rearranged (PSRAM)

int ovl_init(void)
{
    pie_selftest();
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < PRE_MAX; k++)
            if (!s_pre[p][k]) s_pre[p][k] = heap_caps_aligned_alloc(64, (size_t)PRE_ROWS * GFX_PW * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    for (int p = 0; p < 2; p++) for (int c = 0; c < PRE_CHUNKS; c++) s_pre_slot[p][c] = -1;
    if (!s_tmp) s_tmp = heap_caps_malloc(GFX_PW * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 4; i++) {
        if (!s_l[i].w) s_l[i].w = heap_caps_malloc(OVL_CAP * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        if (!s_l[i].w) {
            ESP_LOGE(TAG, "no PSRAM for the moving layer");
            return 0;
        }
        s_l[i].n = 0;
        memset(s_l[i].row, 0, sizeof s_l[i].row);
        memset(s_l[i].busy, 0xFF, sizeof s_l[i].busy);
        s_l[i].nbusy = 0;
        if (!s_l[i].rows)
            s_l[i].rows = heap_caps_aligned_alloc(16, (size_t)OVL_BUSY_ROWS * GFX_PW * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    return 1;
}

void ovl_flip(int on) { s_flip = on ? 1 : 0; }
void ovl_source(const uint16_t *(*row)(int py)) { s_src = row; }
void ovl_waiter(TaskHandle_t t) { s_waiter = t; }

void ovl_stats(ovl_stats_t *s, int reset)
{
    *s = s_st;
    if (reset) memset(&s_st, 0, sizeof s_st);
}

/* --- making it (the ui task) --- */

static ovl_beam_t s_bholes;                            // the holes of the layer being made

void ovl_build_begin(const ovl_beam_t *b)
{
    s_bholes = *b;
    portENTER_CRITICAL(&s_mux);
    int f = 0;
    while (f == s_show || f == s_ready || f == s_plan_layer) f++;   // of four, one is always free
    s_build = f;
    portEXIT_CRITICAL(&s_mux);
    s_l[f].n = 0;
    s_next_y = 0;
    s_full = 0;
}

static inline uint32_t to_word_d(uint32_t p, unsigned d)
{
    const unsigned a = p >> 24;
    unsigned r = (((p >> 16) & 255) + (d >> 1)) >> 3, g = (((p >> 8) & 255) + (d >> 2)) >> 2, b = ((p & 255) + (d >> 1)) >> 3;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    const unsigned under = 32 - ((a * 32 + 128) >> 8);
    if (under == 32 && !(r | g | b)) return 0;        // light too faint to show: nothing
    return (g << 21) | (r << 11) | b | (under << 5);
}

static inline uint32_t to_word(uint32_t p, int x, int y) { return to_word_d(p, BAYER[y & 3][x & 3]); }

// a run of n pixel words from x0, its run word at at: closed, and laid out as two arrays when long
static void close_run(layer_t *L, uint32_t at, int x0, uint32_t n)
{
    if (n >= PIE_MIN && s_tmp && L->n == at + 1 + n) {
        const uint8_t *base = (const uint8_t *)(L->w + at + 1);
        const unsigned pad = (((unsigned)(2 * x0) - (unsigned)(uintptr_t)base) & 15u) >> 1;
        const unsigned words = planar_words(pad, n);
        if (at + 1 + words <= OVL_CAP) {
            memcpy(s_tmp, L->w + at + 1, n * sizeof(uint32_t));
            uint16_t *col = (uint16_t *)(L->w + at + 1) + pad, *sh = col + n + planar_q(n) / 2;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t v = s_tmp[i];
                col[i] = (uint16_t)((((v >> 11) & 31) << 11) | (((v >> 21) & 63) << 5) | (v & 31));
                sh[i] = (uint16_t)((v >> 5) & 63);
            }
            L->w[at] = (uint32_t)x0 | (n << 16) | ((uint32_t)pad << 27) | PLANAR;
            L->n = at + 1 + words;
            return;
        }
    }
    L->w[at] = (uint32_t)x0 | (n << 16);
}

void ovl_build_band(canvas_t *cv, int x0, int x1)
{
    if (s_build < 0) return;
    layer_t *L = &s_l[s_build];
    if (x0 < 0) x0 = 0;
    if (x1 > GFX_PW) x1 = GFX_PW;
    x0 &= ~7;                                          // eight words at a time from here
    for (int r = 0; r < cv->h; r++) {
        const int y = cv->y0 + r;
        uint32_t *row = cv->px + r * GFX_PW;
        while (s_next_y <= y && s_next_y <= GFX_PH) L->row[s_next_y++] = L->n;
        /* outside the box nothing should be, but whatever is goes: the band is used again */
        if (x0 > 0) memset(row, 0, x0 * sizeof(uint32_t));
        if (x1 < GFX_PW) memset(row + x1, 0, (GFX_PW - x1) * sizeof(uint32_t));
        for (int k = 0; k < s_bholes.holes; k++) {     // the popups' boxes: nothing of the layer there
            if (y < s_bholes.hy0[k] || y >= s_bholes.hy1[k]) continue;
            int a = s_bholes.hx0[k] < 0 ? 0 : s_bholes.hx0[k], b = s_bholes.hx1[k] > GFX_PW ? GFX_PW : s_bholes.hx1[k];
            if (a < b) memset(row + a, 0, (b - a) * sizeof(uint32_t));
        }
        int x = x0;
        while (x < x1) {
            if (!row[x]) {                             // the most of it: nothing, eight at a time
                if (!(x & 7) && x + 8 <= x1) {
                    const uint32_t *q = row + x;
                    if (!(q[0] | q[1] | q[2] | q[3] | q[4] | q[5] | q[6] | q[7])) { x += 8; continue; }
                }
                x++;
                continue;
            }
            const int rx0 = x;
            const uint32_t at = L->n;
            uint32_t n = 0;
            int room = !s_full && at + 2 <= OVL_CAP, filled = 0;
            if (room) L->n++;                          // the run's word, filled in below
            while (x < x1 && row[x]) {
                uint32_t w = to_word(row[x], x, y);
                if (!w) {                              // nothing to show here: the run ends, the next starts after it
                    row[x++] = 0;
                    break;
                }
                int same = 1;                          // the same pixel again and again (a label's card): a run of its own
                while (x + same < x1 && row[x + same] == row[x] && same < 0x7FFF) same++;
                if (same >= 8) {
                    w = to_word_d(row[x], 8);          // one flat colour: no dither, so one word stands for all
                    if (!w) {                          // too faint without the dither: nothing
                        memset(row + x, 0, same * sizeof(uint32_t));
                        x += same;
                        break;
                    }
                    if (n) {                           // close the run so far, a fill run after it
                        close_run(L, at, rx0, n);
                    } else if (room) {
                        L->n = at;                     // nothing in it yet: the fill run takes its place
                    }
                    if (!s_full && L->n + 2 <= OVL_CAP) {
                        L->w[L->n++] = (uint32_t)x | ((uint32_t)same << 16) | 0x80000000u;
                        L->w[L->n++] = w;
                    } else {
                        s_full = 1;
                    }
                    memset(row + x, 0, same * sizeof(uint32_t));
                    x += same;
                    n = 0;
                    room = 0;                          // the next pixels open a new run
                    filled = 1;
                    break;
                }
                row[x++] = 0;
                if (room && L->n < OVL_CAP) {
                    L->w[L->n++] = w;
                    n++;
                } else {                               // the layer is full: the rest is cleared, not kept
                    room = 0;
                    s_full = 1;
                }
            }
            if (!filled && L->n > at) {
                if (n) close_run(L, at, rx0, n);
                else L->n = at;                        // all of it too faint
            }
        }
    }
}

void ovl_build_end(const ovl_beam_t *b)
{
    if (s_build < 0) return;
    layer_t *L = &s_l[s_build];
    while (s_next_y <= GFX_PH) L->row[s_next_y++] = L->n;
    L->beam = *b;
    busy_render(L);
    if (L->n > s_st.words_max) s_st.words_max = L->n;
    if (s_full) s_st.overflow++;
    s_st.built++;
    portENTER_CRITICAL(&s_mux);
    if (s_ready >= 0) s_st.skipped++;                  // the one before was never shown
    s_ready = s_build;
    s_build = -1;
    portEXIT_CRITICAL(&s_mux);
}

/* --- putting it on (the fill task, at the panel's pace) --- */

#define EDGE_STEPS 22                                  // the edge's brightness, by its coverage / 16
/* the sweep for one panel frame, worked out once at its start. two of them: the fill's for the frame on
 * the panel, and the planner's for the next one (core 0 composes the busiest chunks of it beforehand) */
typedef struct {
    int      on, cx, cy, r;
    float    ang;                                      // the beam, degrees clockwise from up
    float    tlo[2][LEVELS], thi[2][LEVELS];           // tangents of each trail step's edges, the row above/below the middle
    uint8_t  tok[2][LEVELS];
    uint32_t tadd[LEVELS];                             // what each step adds, spread
    float    bc, bs, bw, ibc;                          // the edge: its line, its half width, 1 / its cosine
    int16_t  chord[GFX_PH];                            // half the range ring's chord, by rows from the middle
    int      chord_r;
    uint32_t focus;
    uint32_t eadd[EDGE_STEPS];
} beam_t;
static DRAM_ATTR beam_t s_bf = { .chord_r = -1 }, s_bp = { .chord_r = -1 };

static inline float wrap180(float a)
{
    a = fmodf(a + 180.0f, 360.0f);
    return (a < 0 ? a + 360.0f : a) - 180.0f;
}

static uint32_t spread_scaled(uint32_t c, unsigned k)  // a colour scaled by k/256, rounded to 565 and spread
{
    unsigned r = (((c >> 16) & 255) * k) >> 8, g = (((c >> 8) & 255) * k) >> 8, b = ((c & 255) * k) >> 8;
    r = (r * 31 + 127) / 255;
    g = (g * 63 + 127) / 255;
    b = (b * 31 + 127) / 255;
    return (g << 21) | (r << 11) | b;
}

static void beam_frame(beam_t *M, const ovl_beam_t *b, float ang)
{
    M->on = b->on && b->turn_ms;
    if (!M->on) return;
    M->cx = b->cx;
    M->cy = b->cy;
    M->r = b->r;
    M->ang = ang;
    const float step = 90.0f / LEVELS;
    for (int c = 0; c < 2; c++) {                      // c 0: rows above the middle, angles round 0; 1: below, round 180
        for (int k = 0; k < LEVELS; k++) {
            float hi = wrap180(M->ang - k * step - c * 180.0f), lo = wrap180(M->ang - (k + 1) * step - c * 180.0f);
            int ok = hi > lo && hi > -90.0f && lo < 90.0f;
            if (lo < -89.99f) lo = -89.99f;
            if (hi > 89.99f) hi = 89.99f;
            M->tok[c][k] = (uint8_t)ok;
            M->tlo[c][k] = tanf(lo * (3.14159265f / 180.0f));
            M->thi[c][k] = tanf(hi * (3.14159265f / 180.0f));
        }
    }
    for (int k = 0; k < LEVELS; k++) {                 // ui_scope.c's trail: 80 f^2 at the step's middle
        unsigned behind = (unsigned)((k * 2 + 1) * 256 / (2 * LEVELS)), f = 256 - behind;
        M->tadd[k] = spread_scaled(b->prim, 80 * f * f >> 16);
    }
    const float a = M->ang * (3.14159265f / 180.0f);
    M->bc = cosf(a);
    M->bs = sinf(a);
    M->ibc = fabsf(M->bc) > 1e-4f ? 1.0f / M->bc : 0.0f;
    if (M->chord_r != M->r) {                           // the ring's chord, row by row: again only when the range ring moves
        M->chord_r = M->r;
        for (int d = 0; d < GFX_PH; d++) M->chord[d] = d < M->r ? (int16_t)sqrtf((float)(M->r * M->r - d * d)) : 0;
    }
    M->bw = (float)GFX_Z(333) / 256.0f;
    M->focus = b->focus;
    for (int i = 0; i < EDGE_STEPS; i++) {             // ui_scope.c's edge: focus * 190 * coverage
        int cov = i * 16 + 8;
        if (cov > 333) cov = 333;
        M->eadd[i] = spread_scaled(b->focus, (unsigned)(190 * cov) >> 8);
    }
}

static IRAM_ATTR inline uint16_t sat(uint32_t x)
{
    if (x & OVF) {
        if (x & 0x20u) x |= 0x1Fu;
        if (x & 0x10000u) x |= 0xF800u;
        if (x & 0x08000000u) x |= 0x07E00000u;
    }
    x &= SPREAD;
    return (uint16_t)(x | (x >> 16));
}

static IRAM_ATTR inline uint32_t spread(uint16_t d) { return (d | ((uint32_t)d << 16)) & SPREAD; }

/* a constant added to a stretch of a row, saturating each field: two pixels a word, the fields'
 * top bits kept out of the sum so no carry crosses into the next, then put back and every field that
 * carried filled with ones */
static IRAM_ATTR inline uint32_t sat_add2(uint32_t w, uint32_t a2)
{
    const uint32_t H = 0x84108410u;                    // the top bit of R, G and B, both pixels
    uint32_t t = (w & ~H) + (a2 & ~H);
    uint32_t sum = t ^ ((w ^ a2) & H);
    uint32_t c = ((w & a2) | ((w | a2) & ~sum)) & H;   // which fields carried out
    return sum | ((c & 0x80108010u) >> 4) * 31u | ((c & 0x04000400u) >> 5) * 63u;
}

// pixels x0..x1 of the layout's row; px is its pixel 0, st -1 when the picture is turned
static IRAM_ATTR void add_span(uint16_t *px, int st, int x0, int x1, uint32_t add)
{
    if (!add || x0 > x1) return;
    uint16_t *p = st > 0 ? px + x0 : px - x1;          // the same stretch, left to right in memory
    int n = x1 - x0 + 1;
    const uint32_t a565 = (add | (add >> 16)) & 0xFFFFu;
    if ((uintptr_t)p & 2) {                            // to a word
        *p = sat(spread(*p) + add);
        p++;
        n--;
    }
    uint32_t *w = (uint32_t *)p;
    const uint32_t a2 = a565 | (a565 << 16);
    for (; n >= 2; n -= 2, w++) *w = sat_add2(*w, a2);
    if (n) {
        p = (uint16_t *)w;
        *p = sat(spread(*p) + add);
    }
}

/* no libm in the fill: its code lies in the PSRAM with the rest (XIP), behind the DMA that is busy
 * there. these are inline, in registers */
static inline int iceil(float v) { int i = (int)v; return (float)i < v ? i + 1 : i; }
static inline int ifloor(float v) { int i = (int)v; return (float)i > v ? i - 1 : i; }
static inline float fabs_(float v) { return v < 0 ? -v : v; }

/* --- a row in two halves: what can be worked out before its pixels are there (prepare: the sweep's
 * stretches and edge, the layer's words fetched out of the PSRAM) and what needs them (apply). the
 * fill prepares a row while the copy is still on its way to it, then applies it once it has landed */
#define ROW_SPANS  128                                 // the trail's steps, and the edge's runs of one brightness (both sides of it)
#define ROW_AHEAD  3                                   // rows prepared at once: the library prepares up to 2 ahead of the one it applies

typedef struct {
    int      ly;                                       // the layout's row, -1: nothing prepared
    int      ns, nw;
    int      nh;                                       // the popups' boxes on this row
    int16_t  h0[4], h1[4];                             // x0..x1-1 of each
    int16_t  s0[ROW_SPANS], s1[ROW_SPANS];             // the sweep's stretches, x0..x1
    uint32_t sa[ROW_SPANS];                            // and what each adds
    const uint32_t *w;                                 // the layer's words for the row, in the PSRAM: fetched into the cache by prepare
    int      made;                                     // a busiest row: px holds its stretch m0..m1 made, the words are not needed
    int      m0, m1;
    uint16_t *px;                                      // its pixels, in the RAM
} row_t;

static DRAM_ATTR uint16_t __attribute__((aligned(16))) s_rowpx[ROW_AHEAD][GFX_PW];
static DRAM_ATTR row_t s_rows[ROW_AHEAD] = { { .ly = -1, .px = s_rowpx[0] }, { .ly = -1, .px = s_rowpx[1] }, { .ly = -1, .px = s_rowpx[2] } };

static IRAM_ATTR void span_from(row_t *R, int x0, int x1, uint32_t add, int k)
{
    if (x0 > x1 || !add) return;
    for (; k < R->nh; k++) {                           // a popup's box cuts it: what is left of it each side
        if (x1 < R->h0[k] || x0 >= R->h1[k]) continue;
        span_from(R, x0, R->h0[k] - 1, add, k + 1);
        span_from(R, R->h1[k], x1, add, k + 1);
        return;
    }
    if (R->ns < ROW_SPANS) {
        R->s0[R->ns] = (int16_t)x0;
        R->s1[R->ns] = (int16_t)x1;
        R->sa[R->ns++] = add;
    }
}

static IRAM_ATTR inline void span(row_t *R, int x0, int x1, uint32_t add) { span_from(R, x0, x1, add, 0); }


static IRAM_ATTR void beam_prepare(row_t *R, int y, int trail, const beam_t *M)
{
    const int dy = y - M->cy;
    if (dy <= -M->r || dy >= M->r) return;
    const int s = M->chord[dy < 0 ? -dy : dy];
    int xa = M->cx - s, xb = M->cx + s;
    if (xa < 0) xa = 0;
    if (xb > GFX_PW - 1) xb = GFX_PW - 1;
    if (xa > xb) return;
    const float up = (float)-dy, lim_a = (float)xa - 1.0f, lim_b = (float)xb + 1.0f;
    if (!trail) {
        // the edge alone, below
    } else if (dy == 0) {                              // the middle row: right of it 90 degrees, left 270
        for (int side = 0; side < 2; side++) {
            float d = M->ang - (side ? 270.0f : 90.0f);
            while (d < 0.0f) d += 360.0f;
            while (d >= 360.0f) d -= 360.0f;
            if (d >= 90.0f) continue;
            int k = (int)(d * LEVELS / 90.0f);
            if (side) span(R, xa, M->cx - 1, M->tadd[k]);
            else span(R, M->cx + 1, xb, M->tadd[k]);
        }
    } else {
        const int c = dy > 0;                          // pixels x: x = middle + up * tan(angle)
        for (int k = 0; k < LEVELS; k++) {
            if (!M->tok[c][k]) continue;
            float x1 = M->cx + up * M->tlo[c][k], x2 = M->cx + up * M->thi[c][k];
            if (x1 > x2) { float t = x1; x1 = x2; x2 = t; }
            if (x2 < lim_a || x1 > lim_b) continue;
            if (x1 < lim_a) x1 = lim_a;
            if (x2 > lim_b) x2 = lim_b;
            int i0 = iceil(x1), i1 = iceil(x2) - 1;
            if (i0 < xa) i0 = xa;
            if (i1 > xb) i1 = xb;
            span(R, i0, i1, M->tadd[k]);
        }
    }
    // the edge: a bright line just at the beam, what is within M->bw of it and ahead of the middle
    float e0, e1;
    if (fabs_(M->bc) > 1e-4f) {
        float m = -(float)dy * M->bs * M->ibc, h = M->bw * fabs_(M->ibc);
        e0 = m - h;
        e1 = m + h;
    } else if (fabs_((float)dy * M->bs) < M->bw) {
        e0 = (float)(xa - M->cx);
        e1 = (float)(xb - M->cx);
    } else {
        return;
    }
    int d0 = iceil(e0), d1 = ifloor(e1);
    if (d0 < xa - M->cx) d0 = xa - M->cx;
    if (d1 > xb - M->cx) d1 = xb - M->cx;
    /* across a row the distance to the beam's line changes by its cosine a pixel. so the row is taken a
     * run at a time, each as long as the brightness stays within half a step (the line nearly along the
     * row, at 3 and 9 o'clock: one or two runs for hundreds of pixels; nearly across it: a few pixels
     * anyway), not a pixel at a time, which near 3 and 9 o'clock was the row's most costly work. ahead
     * of the middle is cut once for the row. only near the middle, where the 6.6 degrees matter, a
     * pixel at a time */
    const int zmax = GFX_Z(333) > 333 ? 333 : GFX_Z(333);
    if (fabs_(M->bs) > 1e-4f) {                         // ahead of the middle: dx * sin - dy * cos > 0
        const float cut = (float)dy * M->bc / M->bs;
        if (M->bs > 0.0f) { const int c = ifloor(cut) + 1; if (d0 < c) d0 = c; }
        else             { const int c = iceil(cut) - 1;  if (d1 > c) d1 = c; }
    } else if (-(float)dy * M->bc <= 0.0f) {
        return;
    }
    const float ab = fabs_(M->bc);
    const int step = ab * 256.0f * 256.0f < 16.0f ? 4096 : (int)(16.0f / (ab * 256.0f)) + 1;   // pixels a run: a brightness step
    int dx = d0, run0 = d0;
    uint32_t radd = 0;
    while (dx <= d1) {
        int n;
        uint32_t add = 0;
        if (dx > -22 && dx < 22 && dy > -22 && dy < 22) {   // near the middle: a pixel at a time
            n = 1;
            const float off = fabs_((float)dx * M->bc + (float)dy * M->bs), r2 = (float)(dx * dx + dy * dy);
            if (r2 > 4.0f && off * off <= 0.01318f * r2) {
                int cov = GFX_Z(333) - (int)(off * 256.0f);
                if (cov > zmax) cov = zmax;
                if (cov > 0) add = M->eadd[cov >> 4];
            }
        } else {
            n = step;
            if (dx < 22 && dx + n > -22 && dy > -22 && dy < 22) n = -21 - dx > 0 ? -21 - dx : 1;   // stop where the middle begins
            if (dx + n - 1 > d1) n = d1 - dx + 1;
            const float mid = (float)dx + (float)(n - 1) * 0.5f;
            const float off = fabs_(mid * M->bc + (float)dy * M->bs);
            int cov = GFX_Z(333) - (int)(off * 256.0f);
            if (cov > zmax) cov = zmax;
            if (cov > 0) add = M->eadd[cov >> 4];
        }
        if (add != radd) {                             // runs of one brightness put down as one stretch
            if (radd) span(R, M->cx + run0, M->cx + dx - 1, radd);
            run0 = dx;
            radd = add;
        }
        dx += n;
    }
    if (radd) span(R, M->cx + run0, M->cx + d1, radd);
}

// a new panel frame: the newest layer, the beam now
static int s_planned;                                  // the frame on the panel is the planned one: its composed chunks hold

static IRAM_ATTR void frame_latch(uint32_t frame)
{
    if (frame == s_frame) return;
    s_frame = frame;
    s_latch_us = esp_timer_get_time();
    float ang = -1.0f;
    portENTER_CRITICAL(&s_mux);
    s_planned = s_plan_frame == frame && s_plan_layer >= 0;
    if (s_planned) {                                   // core 0 planned this frame: its layer, its beam
        if (s_ready == s_plan_layer) s_ready = -1;
        s_show = s_plan_layer;
        ang = s_plan_ang;
    } else if (s_ready >= 0) {
        s_show = s_ready;
        s_ready = -1;
    }
    portEXIT_CRITICAL(&s_mux);
    if (s_show >= 0) {
        const ovl_beam_t *b = &s_l[s_show].beam;
        if (ang < 0.0f && b->turn_ms) ang = (float)((uint32_t)(s_latch_us / 1000) % b->turn_ms) * (360.0f / (float)b->turn_ms);
        beam_frame(&s_bf, b, ang < 0.0f ? 0.0f : ang);
    }
    const beam_t *M = &s_bf;
    {                                                  // the rows the last frame lost, by where the beam was
        extern volatile uint32_t g_ovlRowsSkipped;
        static uint32_t was;
        static int sector;
        const uint32_t now = g_ovlRowsSkipped;
        s_st.late_by_beam[sector] += now - was;
        was = now;
        sector = M->on ? ((int)(M->ang / 30.0f + 0.5f)) % 12 : 0;
    }
    s_st.frames++;
    if (s_waiter) xTaskNotifyGive(s_waiter);
    if (s_planner) xTaskNotifyGive(s_planner);
}

#define RP_LAYER 1                                     // the layer's part too (reads the PSRAM)
#define RP_COUNT 2                                     // into the fill's counters
#define RP_SWEEP 4                                     // the sweep's part
static IRAM_ATTR void row_prepare(row_t *R, int ly, const beam_t *M, int li, int what)
{
    uint32_t t0 = esp_cpu_get_cycle_count();
    R->ly = ly;
    R->ns = R->nw = R->nh = 0;
    R->w = NULL;
    const ovl_beam_t *B = &s_l[li].beam;
    for (int k = 0; k < B->holes; k++)
        if (ly >= B->hy0[k] && ly < B->hy1[k]) { R->h0[R->nh] = B->hx0[k]; R->h1[R->nh++] = B->hx1[k]; }
    if (M->on && (what & RP_SWEEP)) beam_prepare(R, ly, OVL_SWEEP_EVERY < 2 || !(ly & 1), M);
    uint32_t t1 = esp_cpu_get_cycle_count();
    const layer_t *L = &s_l[li];
    R->made = 0;
    if (!(what & RP_LAYER)) return;
    R->made = L->busy[ly] >= 0;
    if (R->made) {                                     // made already: its stretch out of the PSRAM now, while the copy runs
        R->m0 = L->bx0[ly];
        R->m1 = L->bx1[ly];
        memcpy(R->px + R->m0, L->rows + (size_t)L->busy[ly] * GFX_PW + R->m0, (size_t)(R->m1 - R->m0 + 1) * sizeof(uint16_t));
        if (what & RP_COUNT) {
            s_st.layer_cyc += esp_cpu_get_cycle_count() - t1;
            s_st.beam_cyc += t1 - t0;
        }
        return;
    }
    const uint32_t n = L->row[ly + 1] - L->row[ly];
    if (n) {
        const uint32_t *src = L->w + L->row[ly];
        volatile uint32_t sink = 0;                    // into the cache now, a word a 64-byte line, while the copy is still busy
        for (uint32_t i = 0; i < n; i += 16) sink += src[i];
        sink += src[n - 1];
        (void)sink;
        R->w = src;
        R->nw = (int)n;
        if (what & RP_COUNT) s_st.words += n;
    }
    if (what & RP_COUNT) {
        s_st.beam_cyc += t1 - t0;
        s_st.layer_cyc += esp_cpu_get_cycle_count() - t1;
    }
}

// the layer's words for one row onto it: px its pixel 0 as the layout counts, st -1 when turned
static IRAM_ATTR void words_apply(const uint32_t *w0, int nw, uint16_t *px, int st)
{
    const uint32_t *w = w0, *end = w0 + nw;
    while (w < end) {
        const uint32_t h = *w++;
        const int x = (int)(h & 0xFFFF), n = (int)((h >> 16) & 0x7FF);
        if (h & PLANAR) {                              // two arrays: PIE 8 at a time where the row lines up
            const unsigned pad = (h >> 27) & 7;
            const uint16_t *col = (const uint16_t *)w + pad, *sh = col + n + planar_q((unsigned)n) / 2;
            int i = 0;
            if (st > 0 && s_pie && !((uintptr_t)px & 15)) {
                uint16_t *d = px + x;
                for (; i < n && ((uintptr_t)(d + i) & 15); i++) pix(d + i, col[i], sh[i]);
                const int n8 = (n - i) >> 3;
                if (n8 > 0) {
                    pie_blend(d + i, col + i, sh + i, n8);
                    i += n8 * 8;
                }
                for (; i < n; i++) pix(d + i, col[i], sh[i]);
            } else {
                for (; i < n; i++) pix(px + st * (x + i), col[i], sh[i]);
            }
            w += planar_words(pad, (unsigned)n);
            continue;
        }
        if (h & 0x80000000u) {                         // n of the same: one word
            const uint32_t v = *w++, under = (v >> 5) & 63, o0 = v & SPREAD;
            uint16_t *p = px + st * x;
            if (!under) {
                const uint16_t c = sat(o0);
                for (int i = 0; i < n; i++, p += st) *p = c;
            } else {
                for (int i = 0; i < n; i++, p += st) *p = sat(o0 + (((spread(*p) * under) >> 5) & SPREAD));
            }
            continue;
        }
        uint16_t *p = px + st * x;
        for (int i = 0; i < n; i++, p += st) {
            const uint32_t v = w[i], under = (v >> 5) & 63;
            uint32_t o = v & SPREAD;
            if (under) o += ((spread(*p) * under) >> 5) & SPREAD;
            *p = sat(o);
        }
        w += n;
    }
}

/* the busiest rows of a layer, made whole now (the ui task): what the fill would spend on them is
 * estimated from their words, and those over OVL_BUSY_CYCLES get the background's row as it shows now
 * with the layer on it. a background put up afterwards shows in them only with the next layer */
static void busy_render(layer_t *L)
{
    memset(L->busy, 0xFF, sizeof L->busy);
    memset(L->cost, 0, sizeof L->cost);
    L->nbusy = 0;
    for (int ly = 0; ly < GFX_PH; ly++) {
        const uint32_t *w = L->w + L->row[ly], *end = L->w + L->row[ly + 1];
        if (w == end) continue;
        /* what the fill would spend: measured with 48 planes, ~60 cycles a stored word all told (the
         * PSRAM reads, the run's set-up, the pixels) and ~300 a run. a cheaper guess, PIE runs nearly
         * free, made almost no row busy */
        uint32_t cost = 0, stored = 0;
        int xa = GFX_PW, xb = -1;
        for (const uint32_t *q = w; q < end;) {
            const uint32_t h = *q++;
            const unsigned n = (h >> 16) & 0x7FF, x = h & 0xFFFF;
            const unsigned words = (h & PLANAR) ? planar_words((h >> 27) & 7, n) : (h & 0x80000000u) ? 1 : n;
            cost += 300 + 60 * words;
            stored += words;
            if ((int)x < xa) xa = (int)x;
            if ((int)(x + n - 1) > xb) xb = (int)(x + n - 1);
            q += words;
        }
        L->cost[ly] = (uint16_t)(cost / 4 > 65535u ? 65535u : cost / 4);
        if (!L->rows || !s_src || L->nbusy >= OVL_BUSY_ROWS) continue;
        /* only where copying the stretch the layer covers is the cheaper way: whole rows (2 KB of PSRAM
         * each) cost the fill more than putting most layers on, and made it far later */
        const int span = xb - xa + 1;
        if (cost < OVL_BUSY_CYCLES || span > 2 * (int)stored) continue;
        const int py = s_flip ? GFX_PH - 1 - ly : ly;
        const uint16_t *src = s_src(py);
        if (!src) return;
        const int p0 = s_flip ? GFX_PW - 1 - xb : xa;  // the stretch on the panel
        uint16_t *row = L->rows + (size_t)L->nbusy * GFX_PW;
        memcpy(row + p0, src + p0, (size_t)span * sizeof(uint16_t));
        words_apply(w, (int)(end - w), s_flip ? row + GFX_PW - 1 : row, s_flip ? -1 : 1);
        L->bx0[ly] = (int16_t)p0;
        L->bx1[ly] = (int16_t)(p0 + span - 1);
        L->busy[ly] = (int16_t)L->nbusy++;
        /* what the fill does with it now: copy the stretch out of the PSRAM, beside the panel's DMA there,
         * ~12 cycles a pixel (priced at 1, such rows near the middle were never picked and lost rows) */
        L->cost[ly] = (uint16_t)((300 + 12 * span) / 4 > 65535 ? 65535 : (300 + 12 * span) / 4);
    }
    s_st.busy += L->nbusy;
}

/* the fill says by when the row must be done (the library's g_ovlRowDeadline, cycles; 0: no limit).
 * the sweep is what has to keep the panel's pace: it goes first, always. the layer (planes, labels,
 * the compass ring) only when its words fit in what time is left, else the row shows the sweep alone
 * this frame. about 30 cycles a word, measured */
extern volatile uint32_t g_ovlRowDeadline;
#define WORD_CYCLES 30

static IRAM_ATTR void row_apply(const row_t *R, uint16_t *row, uint16_t *px, int st, uint32_t dl)
{
    uint32_t t0 = esp_cpu_get_cycle_count();
    if (R->made) memcpy(row + R->m0, R->px + R->m0, (size_t)(R->m1 - R->m0 + 1) * sizeof(uint16_t));   // its layer, then the sweep over it
    for (int i = 0; i < R->ns; i++) add_span(px, st, R->s0[i], R->s1[i], R->sa[i]);
    uint32_t t1 = esp_cpu_get_cycle_count();
    if (R->made) {
        s_st.beam_cyc += t1 - t0;
        return;
    }
    if (dl && R->nw && (int32_t)(dl - t1) < R->nw * WORD_CYCLES) {
        s_st.layer_dropped++;
        s_st.beam_cyc += t1 - t0;
        return;
    }
    words_apply(R->w, R->nw, px, st);
    s_st.beam_cyc += t1 - t0;
    s_st.layer_cyc += esp_cpu_get_cycle_count() - t1;
}

static IRAM_ATTR inline int logical(int py) { return s_flip ? GFX_PH - 1 - py : py; }

// the fill, first for each chunk: does anything go over its rows this frame
/* --- the busiest chunks of the next frame, composed beforehand on core 0 (fork addition) ---
 * core 0, as the frame on the panel starts, plans the next one (its layer, where its beam will be) and
 * works out what each chunk of it would cost the fill. the chunks over PRE_CYCLES, at most PRE_MAX of them
 * and in the order the panel will scan them, it composes whole: the background's rows, the sweep, the
 * layer, into a buffer in the PSRAM. the fill copies such a chunk from there instead of the framebuffer
 * and has nothing more to do. only the busiest: every chunk would double the PSRAM's traffic */

static IRAM_ATTR int pre_slot(int y, uint32_t frame)
{
    const int p = frame & 1;
    if (!s_planned || s_pre_for[p] != frame) return -1;
    return s_pre_slot[p][y / PRE_ROWS];
}

// what the fill still does on physical row py of this frame: RP_SWEEP and RP_LAYER, less what core 0 did
static IRAM_ATTR int fill_what(int py)
{
    const uint32_t frame = s_frame;
    if (pre_slot(py, frame) < 0) return RP_SWEEP | RP_LAYER | RP_COUNT;
    const uint8_t done = s_pre_what[frame & 1][py / PRE_ROWS];
    return ((done & PRE_SWEEP) ? 0 : RP_SWEEP) | ((done & PRE_LAYER) ? 0 : RP_LAYER) | RP_COUNT;
}

// the fill, before anything else for each chunk: composed beforehand? then from there
IRAM_ATTR const uint16_t *ovl_src(int y, int rows, uint32_t frame, void *ctx)
{
    (void)rows; (void)ctx;
    frame_latch(frame);
    const int k = pre_slot(y, frame);
    if (k < 0) return NULL;
    s_st.pre_used++;
    return s_pre[frame & 1][k];
}

IRAM_ATTR bool ovl_want(int y, int rows, uint32_t frame, void *ctx)
{
    (void)ctx;
    frame_latch(frame);
    if (s_show < 0) return false;
    if (pre_slot(y, frame) >= 0 && s_pre_what[frame & 1][y / PRE_ROWS] == (PRE_SWEEP | PRE_LAYER))
        return false;                                  // composed whole beforehand, copied from there
    const beam_t *M = &s_bf;
    int a = y, b = y + rows - 1;                       // its rows as the layout has them
    if (s_flip) { a = GFX_PH - 1 - b; b = GFX_PH - 1 - y; }
    if (M->on && b > M->cy - M->r && a < M->cy + M->r) return true;   // the sweep crosses them
    const layer_t *L = &s_l[s_show];
    return L->row[b + 1] != L->row[a];                 // the layer has words in them
}

// the fill, before the row's pixels are there (dst unused): rows is 1
IRAM_ATTR void ovl_prepare(uint16_t *dst, int y, int rows, uint32_t frame, void *ctx)
{
    (void)dst; (void)rows; (void)ctx;
    frame_latch(frame);
    if (s_show < 0) return;
    const int ly = logical(y);
    row_prepare(&s_rows[ly % ROW_AHEAD], ly, &s_bf, s_show, fill_what(y));
}

void ovl_writeback(void (*wb)(const void *addr, size_t len)) { s_wb = wb; }

/* the planner's task: woken at each panel frame's start, above the ui task, so a layer being made never
 * makes it miss a frame (run from the ui loop, it planned only a frame in three) */
static uint32_t (*s_period)(void);
static void planner_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ovl_plan_next(s_period ? s_period() : 30000);
    }
}

void ovl_planner_start(uint32_t (*period_us)(void), int core, int prio)
{
    s_period = period_us;
    if (!s_planner) xTaskCreatePinnedToCore(planner_task, "planner", 4096, NULL, prio, &s_planner, core);
}

// the chunks that lost the most rows since the last call: which, how many, what the planner thought of them
void ovl_log_chunks(void)
{
    extern volatile uint16_t g_ovlSkipChunk[64];
    char buf[200];
    int at = 0;
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int c = 0; c < PRE_CHUNKS; c++)
            if (g_ovlSkipChunk[c] && (best < 0 || g_ovlSkipChunk[c] > g_ovlSkipChunk[best])) best = c;
        if (best < 0) break;
        at += snprintf(buf + at, sizeof buf - at, " #%d: %u lost, cost %u, composed %u;", best,
                       (unsigned)g_ovlSkipChunk[best], (unsigned)s_pre_cost[best], (unsigned)s_pre_count[best]);
        g_ovlSkipChunk[best] = 0;
    }
    for (int c = 0; c < PRE_CHUNKS; c++) { g_ovlSkipChunk[c] = 0; s_pre_count[c] = 0; }
    ESP_LOGI(TAG, "chunks losing rows most (threshold %u):%s", (unsigned)PRE_CYCLES, at ? buf : " none");
}

static DRAM_ATTR uint16_t __attribute__((aligned(16))) s_prowpx[GFX_PW];
static DRAM_ATTR row_t s_prow = { .ly = -1, .px = s_prowpx };

// what the sweep of a prepared row costs the fill (its spans; the internal RAM only)
static uint32_t sweep_cost(const row_t *R)
{
    uint32_t c = 0;
    for (int i = 0; i < R->ns; i++) c += 60 + (uint32_t)(R->s1[i] - R->s0[i] + 1) * 2;
    return c;
}

void ovl_plan_next(uint32_t period_us)
{
    if (!s_pre[0][0] || !period_us) return;
    const uint32_t frame = s_frame, next = frame + 1;
    const int64_t start = s_latch_us;
    int li;
    portENTER_CRITICAL(&s_mux);
    li = s_ready >= 0 ? s_ready : s_show;
    portEXIT_CRITICAL(&s_mux);
    if (li < 0) return;
    const ovl_beam_t *b = &s_l[li].beam;
    const int64_t t_next = start + period_us;          // when the next frame starts
    const float ang = b->turn_ms ? (float)((uint32_t)(t_next / 1000) % b->turn_ms) * (360.0f / (float)b->turn_ms) : 0.0f;
    beam_frame(&s_bp, b, ang);
    const int p = next & 1;
    for (int c = 0; c < PRE_CHUNKS; c++) s_pre_slot[p][c] = -1;
    s_pre_for[p] = next;
    portENTER_CRITICAL(&s_mux);                        // the plan, the frame last: the fill reads it whole or not at all
    s_plan_layer = li;
    s_plan_ang = ang;
    s_plan_frame = next;
    portEXIT_CRITICAL(&s_mux);
    if (!s_src) return;
    /* every chunk's cost first (the internal RAM only), then the heaviest PRE_PICK of those over PRE_CYCLES,
     * composed in scan order, each before the panel gets to it in the next frame. by cost, not all over
     * the line: twenty chunks a frame from core 0 (~800 KB of PSRAM traffic) slowed the panel's DMA */
    const uint32_t chunk_us = period_us / PRE_CHUNKS;
    static uint32_t cost_s[PRE_CHUNKS], cost_l[PRE_CHUNKS];
    for (int c = 0; c < PRE_CHUNKS; c++) {
        /* the sweep's from every other row, twice over (its trail is on every other row anyway): half
         * the planner's time, which the layer on this core wants */
        uint32_t cs = 0, cl = 0;
        for (int r = 0; r < PRE_ROWS; r++) {
            const int ly = logical(c * PRE_ROWS + r);
            if (!(r & 1)) {
                row_prepare(&s_prow, ly, &s_bp, li, RP_SWEEP);
                cs += 2u * sweep_cost(&s_prow);
            }
            cl += 4u * s_l[li].cost[ly];
        }
        cost_s[c] = cs;
        cost_l[c] = cl;
        s_pre_cost[c] = cs + cl;
    }
    static uint8_t pick[PRE_CHUNKS];
    memset(pick, 0, sizeof pick);
    for (int k = 0; k < PRE_PICK; k++) {
        int best = -1;
        for (int c = 0; c < PRE_CHUNKS; c++)
            if (!pick[c] && s_pre_cost[c] >= PRE_CYCLES && (best < 0 || s_pre_cost[c] > s_pre_cost[best])) best = c;
        if (best < 0) break;
        pick[best] = 1;
    }
    int used = 0;
    for (int c = 0; c < PRE_CHUNKS && used < PRE_MAX; c++) {
        if (!pick[c]) continue;
        /* the fill asks for chunk c about a bounce period before the panel scans it, the first two at
         * the frame's start: composed by then, or not used */
        const int64_t due = t_next + (int64_t)(c - 2) * chunk_us;
        if (esp_timer_get_time() > due - 1500) continue;   // too late for this one
        const uint32_t cl = cost_l[c];
        /* the sweep is always composed here; the layer too only where it is heavy (many planes and
         * labels): elsewhere the fill puts it on, it reads its words from the PSRAM anyway */
        const uint8_t what = PRE_SWEEP | (cl >= PRE_LAYER_CYCLES ? PRE_LAYER : 0);
        uint16_t *buf = s_pre[p][used];
        for (int r = 0; r < PRE_ROWS; r++) {           // the background's rows, the sweep, the layer
            const int py = c * PRE_ROWS + r, ly = logical(py);
            uint16_t *row = buf + (size_t)r * GFX_PW;
            const uint16_t *src = s_src(py);
            if (!src) return;
            memcpy(row, src, GFX_PW * sizeof(uint16_t));
            row_prepare(&s_prow, ly, &s_bp, li, RP_SWEEP | ((what & PRE_LAYER) ? RP_LAYER : 0));
            row_apply(&s_prow, row, s_flip ? row + GFX_PW - 1 : row, s_flip ? -1 : 1, 0);
        }
        if (s_wb) s_wb(buf, (size_t)PRE_ROWS * GFX_PW * sizeof(uint16_t));
        if (esp_timer_get_time() > due - 200) continue;   // done too late: not used
        s_pre_what[p][c] = what;
        s_pre_slot[p][c] = (int8_t)used++;
        s_pre_count[c]++;
        s_st.pre_made++;
    }
}

IRAM_ATTR void ovl_chunk(uint16_t *dst, int y, int rows, uint32_t frame, void *ctx)
{
    (void)ctx;
    frame_latch(frame);
    if (s_show < 0) return;
    for (int r = 0; r < rows; r++) {                   // row by row: the panel scans them in this order
        const int ly = logical(y + r);
        uint16_t *row = dst + r * GFX_PW;
        uint16_t *px = s_flip ? row + GFX_PW - 1 : row;
        const int st = s_flip ? -1 : 1;
        row_t *R = &s_rows[ly % ROW_AHEAD];
        if (R->ly != ly) row_prepare(R, ly, &s_bf, s_show, fill_what(y + r));   // not prepared ahead: now
        row_apply(R, row, px, st, g_ovlRowDeadline);
        R->ly = -1;
    }
}

#endif
