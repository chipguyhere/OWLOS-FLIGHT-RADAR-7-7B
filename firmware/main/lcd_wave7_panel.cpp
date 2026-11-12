/*
 * the Waveshare ESP32-S3-Touch-LCD-7 / -7B through chipguyhere's library (components/chipguy_lcd7_7b),
 * as a few C calls for lcd_wave7.c. fork addition
 */
#include "board.h"
#if DISP_WAVE7

#include "ESP32S3_Touch_LCD_7_7B_Board.h"
#include "chipguy_ESP32S3_Touch_LCD_7_7B_display.h"
#include "esp_log.h"
extern "C" {
#include "i2c_bus.h"
}

static const char *TAG = "wave7";

static ESP32S3_Touch_LCD_7_7B_Board s_board;
// no scroll padding: the frame's rows lie one after the other, as the strips are packed
static ESP32S3_Touch_LCD_7_7B s_disp(s_board, 0, 0);

// the library's I2C goes over the firmware's bus (i2c_bus.c), which the GT911 in touch.c shares
extern "C" i2c_master_bus_handle_t arduino_shim_i2c_bus(int sda, int scl)
{
    (void)sda;
    (void)scl;
    return i2c_bus_get();
}

extern "C" int w7_begin(ESP32S3_Touch_LCD_7_7B::RasterFn overlay, ESP32S3_Touch_LCD_7_7B::RasterFn prepare,
                        ESP32S3_Touch_LCD_7_7B::OverlayWantFn want, ESP32S3_Touch_LCD_7_7B::OverlaySrcFn src)
{
    /* the library's own GDMA fill copies each chunk from the PSRAM; the fill task sleeps till it has
     * landed and then puts the moving layer over it (setOverlay). a CPU copy of every chunk took the
     * whole core at 1024x600 (tried: thousands of late chunks). the DMA reads behind the cache, so
     * every strip packed into a frame is written back from it (w7_written) */
    if (!s_disp.begin(0, 0)) {
        ESP_LOGE(TAG, "the panel did not start, the library says why above");
        return 0;
    }
    if (!s_disp.allocateSecondFramebuffer()) ESP_LOGW(TAG, "no second frame: the background is drawn where it shows");
    s_board.touchReset();                              // the GT911 at 0x5D, before tp_init looks for it
    s_disp.setRangeWriteBack(true);                    // the strips are written back as they are packed: no whole-cache write-back at a flip
    if (overlay) s_disp.setOverlay(overlay, nullptr, prepare, want, src);
    return s_board.model();
}

extern "C" void w7_written(const void *addr, size_t len) { s_disp.writeBackRange(addr, len); }
extern "C" int w7_width(void) { return s_disp.width(); }
extern "C" int w7_height(void) { return s_disp.height(); }
extern "C" uint16_t *w7_fb(int idx) { return s_disp.framebufferByIndex((uint8_t)idx); }
extern "C" int w7_active(void) { return s_disp.activeFramebuffer(); }
extern "C" int w7_pending(void) { return s_disp.pendingFramebuffer(); }
extern "C" void w7_show(int idx) { s_disp.setActiveFramebuffer((uint8_t)idx, false); }
extern "C" void w7_backlight(int percent) { s_disp.setBacklight(percent); }
extern "C" uint32_t w7_period_us(void) { return s_disp.framePeriodUs(); }
extern "C" const char *w7_model_name(void) { return s_board.modelName(); }

extern "C" void w7_log(void)
{
    /* the backlight, said again (it was seen to go dim on its own, 2026-11-11, and not come back):
     * did the expander take it, are its lines where they should be, and the supply the battery ADC
     * sees. a sag there when it dims points at the power, not the firmware */
    if (s_board.model() == ESP32S3_Touch_LCD_7_7B_Board::MODEL_7B) {
        const bool ok = s_board.setBacklightBrightness(100);
        const uint8_t io = s_board.exioReadAll();
        ESP_LOGI(TAG, "backlight: written %s; expander pins 0x%02x (backlight %s, panel reset %s, panel supply %s); "
                      "battery input %.2f V",
                 ok ? "ok" : "NOT ACKNOWLEDGED", io, io & (1 << 2) ? "on" : "OFF", io & (1 << 3) ? "released" : "HELD",
                 io & (1 << 6) ? "on" : "OFF", s_board.batteryVoltage());
    }
    static uint32_t skipped0, q0[4], dma0;
    static uint32_t early0;
    ESP_LOGI(TAG, "of the late rows, %u waited on the copy; %u copies started early", (unsigned)(g_ovlSkipDma - dma0),
             (unsigned)(g_ovlEarly - early0));
    dma0 = g_ovlSkipDma;
    early0 = g_ovlEarly;
    uint32_t q[4];
    for (int i = 0; i < 4; i++) { q[i] = g_ovlSkipQ[i] - q0[i]; q0[i] = g_ovlSkipQ[i]; }
    ESP_LOGI(TAG, "layer rows skipped for being late: %u (by quarter, top first: %u %u %u %u)",
             (unsigned)(g_ovlRowsSkipped - skipped0), (unsigned)q[0], (unsigned)q[1], (unsigned)q[2], (unsigned)q[3]);
    skipped0 = g_ovlRowsSkipped;
    static uint32_t prep0, spin0, fr0, all0;
    const uint32_t fr = s_disp.frameCount(), nf = fr - fr0 ? fr - fr0 : 1;
    ESP_LOGI(TAG, "fill a frame: prepare %u us, waiting on the copy %u us, overlay all told %u us",
             (unsigned)((g_ovlPrepCycles - prep0) / 240u / nf), (unsigned)((g_ovlSpinCycles - spin0) / 240u / nf),
             (unsigned)((g_ovlCycles - all0) / 240u / nf));
    prep0 = g_ovlPrepCycles; spin0 = g_ovlSpinCycles; fr0 = fr; all0 = g_ovlCycles;
    static uint32_t chunks0, cyc0;
    const uint32_t chunks = g_ovlChunks, cyc = g_ovlCycles;
    const uint32_t p = s_disp.framePeriodUs();
    ESP_LOGI(TAG, "panel %.1f Hz; layer: %u chunks, %u with a late row (all told), worst %u us (bounce period %u us), %u us a chunk; "
                  "fills dropped %u, desyncs %u",
             p ? 1e6 / p : 0.0, (unsigned)(chunks - chunks0), (unsigned)g_ovlLate, (unsigned)g_ovlMaxUs,
             (unsigned)g_bbPeriodUs, (unsigned)(chunks != chunks0 ? (cyc - cyc0) / 240u / (chunks - chunks0) : 0),
             (unsigned)g_bbDroppedCount, (unsigned)g_bbDesyncCount);
    chunks0 = chunks;
    cyc0 = cyc;
    g_ovlMaxUs = 0;
}

#endif
