/* gfx - strip renderer for the RGB565 panels (size from config.h).
 * 320x240 frame is 153600 bytes, biggest free block after WiFi is ~110 KB (no PSRAM), so we
 * draw the scene once per strip of rows in 0x00RRGGBB, then dither to 565 and send by DMA.
 * 8 bit channels + dither keeps dark gradients from banding. all primitives clip to the strip */
#pragma once
#include <stdint.h>

#include "config.h"   // first: RADAR_OVERLAY below picks where CORE_BIG puts things (fork: it came after, and did nothing)

/* hot loops go to IRAM on ESP32: a frame runs all this code ten times over
 * and every flash cache miss costs microseconds */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#if RADAR_OVERLAY
/* the Waveshare 7/7B (fork addition): the drawing code and its tables stay out of the internal RAM.
 * what it draws there (the background, the planes' layer) is not on the panel's clock, and the RAM
 * is wanted. the code that is (the fill, ovl.c) has IRAM_ATTR of its own */
#define CORE_HOT
#define CORE_DATA
#else
#define CORE_HOT  IRAM_ATTR
#define CORE_DATA DRAM_ATTR
#endif
/* big state no interrupt touches (fork addition): in the PSRAM on the Waveshare 7/7B, whose internal
 * RAM the 1024-wide bounce buffers and strips leave short (sdkconfig.defaults.wave7 lets the PSRAM
 * take .bss). in the RAM as before everywhere else */
#if RADAR_OVERLAY
#define CORE_BIG  EXT_RAM_BSS_ATTR
#else
#define CORE_BIG
#endif
#else
#define CORE_HOT
#define CORE_DATA
#define CORE_BIG
#endif

#include "config.h"   // GFX_W, GFX_H come from the chosen display

/* the layout's pixels and the glass's (config.h). what is drawn through the calls below is in the
 * layout's, a strip's rows and gfx_row are in the glass's: the code that walks pixels by hand works
 * there. GFX_Z a whole number of them across, GFX_PX where a layout pixel's middle falls on the
 * glass, GFX_PL a length, GFX_UNZ a width on the glass back, rounded up. nothing at all but on
 * the 5" boards */
#if GFX_SCALED
/* GFX_SCALE_NUM / 2^GFX_SCALE_SHIFT glass pixels to a layout pixel (config.h): 3/2 on the 800x480
 * boards, 9/4 on the Tab5. GFX_PX lands on the middle of the scaled pixel: + 0.25 at 3/2 */
#define GFX_SCALE_F ((float)GFX_SCALE_NUM / (float)(1 << GFX_SCALE_SHIFT))
#define GFX_Z(v)   (((v) * GFX_SCALE_NUM) >> GFX_SCALE_SHIFT)
#define GFX_PX(v)  ((v) * GFX_SCALE_F + (GFX_SCALE_F - 1.0f) * 0.5f)
#define GFX_PL(v)  ((v) * GFX_SCALE_F)
#define GFX_UNZ(v) (((v) * (1 << GFX_SCALE_SHIFT) + GFX_SCALE_NUM - 1) / GFX_SCALE_NUM)
#else
#define GFX_Z(v)   (v)
#define GFX_PX(v)  (v)
#define GFX_PL(v)  (v)
#define GFX_UNZ(v) (v)
#endif

/* the ESP32-C3 (RISC-V, no F extension) has no floating point unit: a float costs tens of
 * cycles there, so the busiest loops take a whole number way. the ESP32's float is quicker */
#ifndef GFX_SOFT_FLOAT                // the PC preview may set it, to see the C3's way drawn
#if defined(__riscv) && !defined(__riscv_flen)
#define GFX_SOFT_FLOAT 1
#else
#define GFX_SOFT_FLOAT 0
#endif
#endif

typedef struct {
    uint32_t *px;   /* GFX_PW * h pixels, 0x00RRGGBB, row by row */
    int y0;         /* first screen row of the strip */
    int h;          /* rows in strip */
    int helper;     /* drawn by the second core (main.c): its time is not in ui_t.prof */
    int layer;      /* RADAR_OVERLAY builds: which layer it is drawn for (ui.h UI_LAYER_*), 0 everywhere else */
} canvas_t;

#define RGB(r, g, b) ((uint32_t)((((r) & 255) << 16) | (((g) & 255) << 8) | ((b) & 255)))

/* the moving layer (config.h RADAR_OVERLAY) is drawn on a canvas of nothing, 0, and taken off it as
 * colour and coverage: there the top byte of a pixel is how much of it is covered, 0..255, and the
 * colour is already scaled by it. a colour drawn counts as opaque (GFX_OPAQUE), mixing builds the
 * coverage up as it builds the colour, adding light (glows) leaves it as it was. on every other
 * build the top byte is never looked at and none of this is compiled */
#if RADAR_OVERLAY
#define GFX_ALPHA  1
#define GFX_OPAQUE 0xFF000000u
#else
#define GFX_ALPHA  0
#define GFX_OPAQUE 0u
#endif

// alpha is 0..256 everywhere (256 = opaque) so >> 8 is exact
static inline uint32_t gfx_mix(uint32_t d, uint32_t s, uint32_t a)
{
    uint32_t ia = 256 - a;
    uint32_t rb = ((s & 0xFF00FFu) * a + (d & 0xFF00FFu) * ia) >> 8;
#if GFX_ALPHA
    /* green and the coverage together, as red and blue are: green low, coverage in the third byte */
    uint32_t ga = (((s >> 8) & 0xFFu) | 0xFF0000u) * a + ((d >> 8) & 0xFF00FFu) * ia;
    return (rb & 0xFF00FFu) | (ga & 0xFF00FF00u);
#else
    uint32_t g  = ((s & 0x00FF00u) * a + (d & 0x00FF00u) * ia) >> 8;
    return (rb & 0xFF00FFu) | (g & 0x00FF00u);
#endif
}

static inline uint32_t gfx_scale(uint32_t c, uint32_t a)
{
    return ((((c & 0xFF00FFu) * a) >> 8) & 0xFF00FFu) | ((((c & 0x00FF00u) * a) >> 8) & 0x00FF00u);
}

// saturating add per channel, light stops at 255 and never wraps to black
static inline uint32_t gfx_add(uint32_t d, uint32_t s)
{
    /* branchless: add low 7 bits of each channel, put the top bit back with xor,
     * then every channel that carried out gets filled with 0xFF */
    uint32_t t = (d & 0x7F7F7Fu) + (s & 0x7F7F7Fu);
    uint32_t sum = t ^ ((d ^ s) & 0x808080u);
    uint32_t ovf = ((d & s) | ((d | s) & t)) & 0x808080u;
#if GFX_ALPHA
    return sum | ((ovf >> 7) * 0xFFu) | (d & 0xFF000000u);   // light adds no coverage
#else
    return sum | ((ovf >> 7) * 0xFFu);
#endif
}

/* fast sqrt for anti-aliasing: inverse root guess + one Newton step, ~0.2% off,
 * plenty for AA. ESP32 has no sqrt instruction and AA shapes need one per pixel */
static inline float gfx_sqrt(float x)
{
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } u;
    u.f = x;
    u.i = 0x5f3759dfu - (u.i >> 1);
    float y = u.f;
    y = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}

// pointer to row y of the glass, or 0 if its not in this strip
static inline uint32_t *gfx_row(const canvas_t *cv, int y)
{
    int r = y - cv->y0;
    return (r >= 0 && r < cv->h) ? cv->px + r * GFX_PW : 0;
}

// is any of the rows ya..yb inside this strip?
static inline int gfx_touches(const canvas_t *cv, int ya, int yb)
{
    return GFX_Z(yb) + GFX_SCALED >= cv->y0 && GFX_Z(ya) < cv->y0 + cv->h;
}

// the rows ya..yb of a strip as a canvas of their own: what is drawn on it stops there. 0: none here
static inline int gfx_sub(const canvas_t *cv, int ya, int yb, canvas_t *out)
{
    int a = GFX_Z(ya), b = GFX_Z(yb + 1) - 1;
    if (a < cv->y0) a = cv->y0;
    if (b > cv->y0 + cv->h - 1) b = cv->y0 + cv->h - 1;
    if (a > b) return 0;
    *out = (canvas_t){ cv->px + (a - cv->y0) * GFX_PW, a, b - a + 1, cv->helper, cv->layer };
    return 1;
}

void gfx_fill(canvas_t *cv, int x, int y, int w, int h, uint32_t c, int a);
void gfx_hline(canvas_t *cv, int x0, int x1, int y, uint32_t c, int a);
void gfx_vline(canvas_t *cv, int x, int y0, int y1, uint32_t c, int a);
void gfx_hatch(canvas_t *cv, int x, int y, int w, int h, uint32_t c, int a);

/* --- anti-aliased, float coords at pixel centres --- */
void gfx_line(canvas_t *cv, float x0, float y0, float x1, float y1, float w, uint32_t c, int a);
void gfx_ring(canvas_t *cv, float cx, float cy, float r, float w, uint32_t c, int a);
void gfx_disc(canvas_t *cv, float cx, float cy, float r, uint32_t c, int a);
void gfx_glow(canvas_t *cv, float cx, float cy, float r, uint32_t c, int a);   /* additive */
void gfx_dot(canvas_t *cv, float x, float y, uint32_t c, int a);   /* a point, shared over 2x2 pixels */
uint32_t gfx_isqrt(uint32_t v);                                     /* whole number square root */
void gfx_tri(canvas_t *cv, float x0, float y0, float x1, float y1, float x2, float y2,
             uint32_t c, int a);
// convex polygon, xy = x0,y0,x1,y1.. (3..12 corners, either winding)
void gfx_poly(canvas_t *cv, const float *xy, int n, uint32_t c, int a);
// arc as a polyline. degrees, 0 = up, clockwise
void gfx_arc(canvas_t *cv, float cx, float cy, float r, float deg0, float deg1, float w,
             uint32_t c, int a);

// dither strip to RGB565, high byte first like the SPI panels want, low first for the RGB bus
void gfx_pack565(const canvas_t *cv, uint16_t *out);
