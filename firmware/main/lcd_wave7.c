/*
 * the Waveshare ESP32-S3-Touch-LCD-7 and -7B (fork addition). chipguyhere's library feeds the panel
 * from a frame in the PSRAM (lcd_wave7_panel.cpp); here that frame is the background: everything
 * but what moves, drawn strip by strip as time allows into the frame that does not show, and shown
 * from the next panel frame on once it is whole. what moves goes over it as the panel is fed (ovl.c).
 */
#include "display.h"
#if DISP_WAVE7

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ovl.h"
#include "sdkconfig.h"

/* sdkconfig.defaults.wave7 is read only when a build folder's sdkconfig is first made: a folder built
 * before it (or for another board) keeps the old settings, and the board then runs out of internal
 * RAM and hangs on a dark screen after the WiFi starts. seen on the -7, 2026-11-11 */
#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY || CONFIG_MBEDTLS_HARDWARE_AES || CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL > 1024
#error "this build folder's sdkconfig predates sdkconfig.defaults.wave7: delete the folder (or its sdkconfig) and build again"
#endif

static const char *TAG = "lcd";

int         w7_begin(void (*overlay)(uint16_t *, int, int, uint32_t, void *), void (*prepare)(uint16_t *, int, int, uint32_t, void *),
                     bool (*want)(int, int, uint32_t, void *), const uint16_t *(*src)(int, int, uint32_t, void *));
int         w7_width(void);
int         w7_height(void);
uint16_t   *w7_fb(int idx);
int         w7_active(void);
int         w7_pending(void);
void        w7_show(int idx);
void        w7_backlight(int percent);
uint32_t    w7_period_us(void);
const char *w7_model_name(void);
void        w7_log(void);
void        w7_written(const void *addr, size_t len);

static int s_ok, s_flip, s_draw = 1, s_two;

#if RADAR_DISPLAY == DISPLAY_WAVE7B_1024x600
#define WANT_MODEL 0x7B
#define OTHER_BUILD "wave7"
#else
#define WANT_MODEL 0x07
#define OTHER_BUILD "wave7b"
#endif

// the background's row py as it will show from the next panel frame (the one put up, if one is waiting
// to be), for the rows and chunks the moving layer makes beforehand (ovl.c)
static const uint16_t *shown_row(int py)
{
    const uint16_t *fb = w7_fb(w7_pending());
    return fb ? fb + (size_t)py * GFX_PW : NULL;
}

void disp_init(void)
{
    const int layer = ovl_init();
    const int model = w7_begin(layer ? ovl_chunk : NULL, layer ? ovl_prepare : NULL, layer ? ovl_want : NULL,
                               layer ? ovl_src : NULL);
    if (!model) return;
    if (model != WANT_MODEL || w7_width() != GFX_PW || w7_height() != GFX_PH) {
        ESP_LOGE(TAG, "this build is for a %dx%d glass, the board is a %s (%dx%d): flash the %s build. the screen stays dark",
                 GFX_PW, GFX_PH, w7_model_name(), w7_width(), w7_height(), OTHER_BUILD);
        return;
    }
    s_two = w7_fb(2) != NULL;
    ovl_source(shown_row);
    ovl_writeback(w7_written);
    s_draw = s_two ? 2 : 1;
    s_ok = 1;
    ESP_LOGI(TAG, "%s, %dx%d, %s, the moving layer %s", w7_model_name(), GFX_PW, GFX_PH,
             s_two ? "two frames" : "one frame", layer ? "over it" : "NOT THERE");
}

void disp_backlight(int percent) { if (s_ok) w7_backlight(percent < 0 ? 0 : percent > 100 ? 100 : percent); }

int disp_bg_ready(void) { return !s_ok || !s_two || w7_active() == w7_pending(); }

void disp_bg_begin(void)
{
    if (s_two) s_draw = w7_active() == 1 ? 2 : 1;     // the one that does not show
}

void disp_strip(const canvas_t *cv)
{
    if (!s_ok) return;
    uint16_t *fb = w7_fb(s_draw);
    if (!s_flip) {
        gfx_pack565(cv, fb + cv->y0 * GFX_PW);
        w7_written(fb + cv->y0 * GFX_PW, (size_t)cv->h * GFX_PW * sizeof(uint16_t));   // the DMA reads the PSRAM, not the cache
        return;
    }
    static CORE_BIG uint16_t line[GFX_PW];                      // turned over: last row first, each back to front
    for (int r = 0; r < cv->h; r++) {
        const canvas_t one = { cv->px + r * GFX_PW, cv->y0 + r, 1, 0 };
        gfx_pack565(&one, line);
        uint16_t *to = fb + (GFX_PH - 1 - one.y0) * GFX_PW;
        for (int x = 0; x < GFX_PW; x++) to[GFX_PW - 1 - x] = line[x];
        w7_written(to, GFX_PW * sizeof(uint16_t));
    }
}

void disp_frame_end(void) { if (s_ok && s_two) w7_show(s_draw); }   // from the next panel frame on

uint32_t disp_frame_us(void)
{
    uint32_t p = s_ok ? w7_period_us() : 0;
    return p ? p : 30000;
}

void disp_log(void) { if (s_ok) w7_log(); }

void disp_flip(int on)
{
    s_flip = on ? 1 : 0;
    ovl_flip(on);
}

int disp_profiles(void) { return 1; }
void disp_profile(int p) { (void)p; }
const char *disp_profile_name(void) { return "RGB"; }
void disp_pack_under(const canvas_t *under) { disp_strip(under); }
void disp_strip2(const canvas_t *cv, int under_rows) { (void)under_rows; disp_strip(cv); }
int disp_rows(int rows) { (void)rows; return 1; }
uint32_t disp_frame_ms(void) { return 0; }
void disp_full_next(void) { }
uint32_t disp_lost(void) { return 0; }
void disp_wait_idle(void) { }

#endif
