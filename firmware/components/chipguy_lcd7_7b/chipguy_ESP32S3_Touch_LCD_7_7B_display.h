/*
 * chipguy_ESP32S3_Touch_LCD_7_7B - Display driver for the Waveshare
 * ESP32-S3-Touch-LCD-7 and ESP32-S3-Touch-LCD-7B boards.
 *
 * One driver for both boards' RGB LCDs: the -7's 800x480 ST7262 panel and
 * the -7B's 1024x600 panel.  They use the same RGB data/sync pins, so the
 * only differences are the panel size, its timing and pixel clock, and how
 * the backlight is driven.  begin() picks them from the board that
 * ESP32S3_Touch_LCD_7_7B_Board::begin() identified (see its header).  These
 * panels are plain RGB-interface TFTs with no command controller, so there is
 * no SPI init sequence: the ESP32-S3 RGB LCD peripheral drives them directly.
 * The panel reset and backlight (and on the -7B, the panel supply and PWM
 * dimming) are reached through the board's I2C expander.
 *
 * Merged 2026-11-03 from the chipguy -7B driver (whose features it keeps) and
 * the -7 driver.  Figures quoted "measured" are from the -7B unless they say
 * otherwise.
 *
 * Copyright (c) 2025-2026 chipguyhere
 * MIT License
 */

#ifndef CHIPGUY_ESP32S3_TOUCH_LCD_7_7B_H
#define CHIPGUY_ESP32S3_TOUCH_LCD_7_7B_H

#include <Arduino.h>
#include "esp_lcd_panel_io.h"
#include <ESP32S3_Touch_LCD_7_7B_Board.h>   // shared I2C bus + expander (RST/BL)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// This display requires PSRAM for its framebuffers (two or three RGB565
// buffers: 1.2 MB each on the -7B, 750 KB on the -7). The Arduino ESP32 core defines BOARD_HAS_PSRAM only when PSRAM
// is enabled in Tools > PSRAM. Fail the build early with a clear message rather
// than letting framebuffer allocation fail at runtime.
#if !defined(BOARD_HAS_PSRAM)
#error "chipguy_ESP32S3_Touch_LCD_7_7B_display requires PSRAM. In the Arduino IDE, set Tools > PSRAM to \"OPI PSRAM\" (Board: ESP32S3 Dev Module)."
#endif


// Sprite system constants
#define SPRITE_MAX_COUNT 16          // Maximum number of sprites (compile-time adjustable)
#define SPRITE_DISABLED  0x80000000  // yx value indicating sprite slot is disabled

// Sprite flags
#define SPRITE_FLAG_COLLISION_DETECT 0x00000001  // Enable collision detection for this sprite

// Sprite structure - compact for ISR efficiency
// All sprites are rendered in display coordinates (unaffected by scrolling)
//
// PERFORMANCE NOTE: Sprite pixel data is read by the bounce buffer ISR which runs
// at high priority during display refresh. For best performance, sprite data should
// be stored in SRAM (internal RAM), not in flash or PSRAM. Reading from flash or
// PSRAM during the ISR adds significant latency due to cache misses and bus
// contention, which may cause display artifacts or timing issues with many sprites.
//
// Recommended: Declare sprite data as static arrays or use heap_caps_malloc()
// with MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT for SRAM allocation.
struct Sprite {
    const uint16_t* data;   // Pointer to RGB565 pixel data (SRAM recommended, see note above)
    uint32_t yx;            // Y in upper 16 bits, X in lower 16 bits (signed coordinates)
    uint32_t hw;            // Height in upper 16 bits, Width in lower 16 bits
    uint16_t transparent;   // Color value to skip (transparency key)
    uint16_t _reserved;     // Padding for 32-bit alignment
    uint32_t flags;         // Reserved for future use (e.g., flip, rotate, blend modes)
};



// Normalize a rotation argument to degrees.  Accepts degrees (0/90/180/270)
// directly, and the index aliases 1/2/3 as 90/180/270 respectively.  Anything
// else passes through unchanged (treated as no rotation downstream).
static inline uint16_t cglcd7_7b_normalize_rotation(uint16_t rotation) {
    switch (rotation) {
        case 1: return 90;
        case 2: return 180;
        case 3: return 270;
        default: return rotation;
    }
}

// Panel geometry and RGB timing, per board (Waveshare's values), chosen by
// begin() from the board the expander probe found:
//
//   -7   800x480 ST7262.  Porches h 4/8/8, v 4/8/8: htotal 820, vtotal 500,
//        so refresh = pclk / 410000.  Default pclk 14 MHz (~34.1 Hz).
//   -7B  1024x600.  Porches h 162/152/48, v 45/13/3: htotal 1386, vtotal 661,
//        so refresh = pclk / 916146.  Default pclk 30 MHz (~32.7 Hz).
//
// Arrays that the driver sizes at compile time use the larger panel.
#define LCD7_7B_MAX_WIDTH  1024
#define LCD7_7B_MAX_HEIGHT 600

// Pixel clock.  0 means the board's default (above).  See begin() for what
// it buys and costs.
#ifndef LCD7_7B_DEFAULT_PCLK_HZ
#define LCD7_7B_DEFAULT_PCLK_HZ 0
#endif

// Height of each of the two SRAM bounce buffers, in rows (2 KB per row on
// the -7B, 1.6 KB on the -7).  Both 600 and 480 must divide into an even
// number of them.
#ifndef LCD7_7B_BOUNCE_ROWS
#define LCD7_7B_BOUNCE_ROWS 20
#endif
static_assert(600 % LCD7_7B_BOUNCE_ROWS == 0 && (600 / LCD7_7B_BOUNCE_ROWS) % 2 == 0 &&
              480 % LCD7_7B_BOUNCE_ROWS == 0 && (480 / LCD7_7B_BOUNCE_ROWS) % 2 == 0,
              "LCD7_7B_BOUNCE_ROWS must divide 600 and 480 into even numbers of chunks");

// Idle share (0..100) of core 0 and core 1 since the previous call, from the
// FreeRTOS idle tasks' run-time counters.  For LVGL's performance monitor:
// lv_conf.h sets LV_SYSMON_GET_IDLE / LV_SYSMON_GET_PROC_IDLE to these, so it
// shows "FPS | CPU (core 0 % | core 1 %)".  (LVGL's own measure times its loop
// with lv_tick, which the frame clock advances only once per refresh.)
extern "C" uint32_t cglcd7_7b_idle_percent_core0(void);
extern "C" uint32_t cglcd7_7b_idle_percent_core1(void);

class ESP32S3_Touch_LCD_7_7B {
public:
    // Constructor.  Takes the board that owns the shared I2C bus and the
    // expander; the display's reset and backlight (and on the -7B, its
    // supply) are reached through it, and it says which board this is.  fb_pad_x/fb_pad_y are optional pre-draw scroll padding.
    ESP32S3_Touch_LCD_7_7B(ESP32S3_Touch_LCD_7_7B_Board& board,
                        uint16_t fb_pad_x = 20, uint16_t fb_pad_y = 60);

    // --- Bounce-buffer fill mode (where the per-frame PSRAM->bounce copy runs) ---
    // Selects the fill strategy at RUNTIME, so a sketch can opt into a higher-
    // performance mode WITHOUT editing this shared driver or recompiling it with
    // -D flags.  Call setBounceFillMode() BEFORE begin(); begin() reads it once,
    // when it creates the fill task.
    //
    //   BOUNCE_FILL_DEFAULT    - BOUNCE_FILL_GDMA, or BOUNCE_FILL_SAME_CORE when
    //                            the panel is flipped 180 (DMA can't reverse
    //                            pixel order).  LCD7_7B_BOUNCE_FILL_OTHER_CORE=1
    //                            makes it BOUNCE_FILL_OTHER_CORE instead.
    //   BOUNCE_FILL_SAME_CORE  - one high-priority fill task on the begin() core,
    //                            copying with the CPU.  With the row cache on,
    //                            only rows without an op list are copied.  No
    //                            cache write-back is ever needed, so a flip
    //                            never stalls the other core: the choice when
    //                            interrupt latency matters (radio, audio, fast
    //                            UARTs) more than spare CPU on the display core.
    //   BOUNCE_FILL_OTHER_CORE - one high-priority fill task on the OTHER core.
    //                            More UI headroom, but that core normally runs
    //                            WiFi/BT and this task outranks them, so it will
    //                            impair the radio.  Use only if WiFi/BT is unused.
    //   BOUNCE_FILL_GDMA       - the default.  The fill task (begin() core) hands
    //                            each chunk to a memory-to-memory GDMA channel
    //                            (64-byte bursts) instead of copying it, so the
    //                            CPU only sets up the transfer.  Falls back to the
    //                            CPU copy for 180-degree rotation, RGB332 mode,
    //                            chunks with sprites, and unaligned scroll
    //                            offsets.  Writes the dcache back to PSRAM at
    //                            every frame flip and in the drawing calls, since
    //                            DMA reads PSRAM behind the cache.  See
    //                            g_bbDma* for slack instrumentation.
    enum BounceFillMode : uint8_t {
        BOUNCE_FILL_DEFAULT = 0,
        BOUNCE_FILL_SAME_CORE,
        BOUNCE_FILL_OTHER_CORE,
        BOUNCE_FILL_GDMA,
    };
    void setBounceFillMode(BounceFillMode mode) { _bounceFillMode = mode; }
    BounceFillMode getBounceFillMode() const { return _bounceFillMode; }

    // --- Framebuffer colour mode (opt-in 8-bit) ---
    // COLOR_RGB565 (default): 2 bytes per pixel, what LVGL renders natively.
    // COLOR_RGB332: 1 byte per pixel.  The per-frame PSRAM read -- the cost that
    //   dominates this driver -- halves, at the price of 256 colours.  Each byte
    //   is expanded to RGB565 through a 256-entry table as the bounce buffer is
    //   filled; by default the table is plain RGB332 (3 bits red, 3 green, 2
    //   blue), and setPalette() can replace it.  The drawing calls below still
    //   take RGB565 colours and quantize them; getFramebuffer() then points at
    //   bytes, not uint16_t pixels.  scrollFramebuffer() is not supported in
    //   this mode.  Fine for UIs made of text and flat widgets; poor for photos
    //   and gradients.
    // Call setColorMode() BEFORE begin().
    enum ColorMode : uint8_t { COLOR_RGB565 = 0, COLOR_RGB332 };
    void setColorMode(ColorMode mode) { _rgb332 = (mode == COLOR_RGB332); }
    ColorMode getColorMode() const { return _rgb332 ? COLOR_RGB332 : COLOR_RGB565; }
    // Replace the 8-bit mode's byte -> RGB565 table (256 entries, copied).
    void setPalette(const uint16_t* rgb565x256);
    // The byte the 8-bit mode stores for an RGB565 colour (plain RGB332).
    static inline uint8_t rgb565to332(uint16_t c) {
        return ((c >> 8) & 0xE0) | ((c >> 6) & 0x1C) | ((c >> 3) & 0x03);
    }

    // Initialize the display.
    // rotation: 0 = normal, 180 = flipped (for a panel mounted upside-down).
    //           The index aliases 1/2/3 are accepted for 90/180/270.  Only 0 and
    //           180 are handled here; 90/270 are intended to be done at the
    //           LVGL->framebuffer level (so the driver keeps reading the
    //           framebuffer forward/sequentially), not by this driver.
    //
    // begin() first brings up the board (ESP32S3_Touch_LCD_7_7B_Board::begin,
    // which identifies it), then sizes everything for that board's panel.
    // Returns false if no supported board answered.  width() and height()
    // are the panel's only after begin() (or once the board's begin() has
    // run, which lv_setup does first).
    //
    // pclk_hz 0 = the board's default.
    bool begin(uint32_t pclk_hz = LCD7_7B_DEFAULT_PCLK_HZ, uint16_t rotation = 0);
    // This is a RAM-less RGB/DPI panel: the ESP32-S3 generates PCLK itself and
    // must re-stream the whole framebuffer every frame, so PCLK *is* the refresh
    // rate:
    //
    //   refresh_hz = pclk_hz / (htotal * vtotal)
    //
    // with htotal/vtotal from the timing above (begin() prints the result).
    // Every refreshed frame costs the display core a full PSRAM->SRAM copy of
    // the panel, so the refresh rate sets the CPU load.
    //
    // -7B, measured 2026-10-26 (Waveshare's porches, dcache autoload off; "app
    // core left" is what remains for LVGL and loop() on the begin() core):
    //
    //   pclk     refresh   app core left   on screen
    //   30 MHz   32.7 Hz   ~28 %           clean (the default)
    //   25 MHz   27.3 Hz   ~40 %           black areas flicker
    //   20 MHz   21.8 Hz   ~52 %           black areas flicker
    //   16 MHz   17.5 Hz   ~62 %           clear flicker
    //   12 MHz   13.1 Hz   ~71 %           clear flicker; still locks
    //
    // So on the -7B the floor is visible flicker, not lock: the panel took
    // every clock tried, and 12 MHz is only the lowest one tested.
    //
    // -7: the ST7262's input PLL stops locking below ~12 MHz and the panel
    // drops into its built-in self-test colour cycle (white/red/green/blue/
    // black), so that is the floor there:
    //
    //   11.5 MHz   ~28 Hz   never locks (shows the self-test pattern)
    //   12 MHz     ~29 Hz   marginal -- may only come up after a cold power cycle
    //   13 MHz     ~32 Hz   the clamp: requests below it are raised to it
    //   14 MHz     ~34 Hz   default; ~10 % margin over the lock floor
    //   16 MHz     ~39 Hz   Waveshare's stock value for this panel
    //   21 MHz     ~51 Hz   near the practical upper limit for this timing
    //
    // Requests below the board's clamp (LCD7_7B_MIN_PCLK_HZ_7 / _7B) are
    // raised to it.  The S3 can only make pclk = clock_source /
    // integer_divider, so the request lands on the nearest step.

    // Backlight control.  0 switches the backlight off.  On the -7B, 1..100
    // switch it on at that brightness (the expander's PWM output); the -7's
    // backlight is on/off only, so any percentage above 0 is full on.
    void setBacklight(int percentage);
    void setBacklight(bool on);

    // Get framebuffer pointer for direct access.  In COLOR_RGB332 mode this
    // points at 1-byte pixels; cast it to uint8_t*.
    uint16_t* getFramebuffer() { return _framebuffer; }

    // Double buffering support (optional)
    // Allocates a second framebuffer for tear-free rendering
    // Returns true on success, false if allocation fails or already allocated
    bool allocateSecondFramebuffer();

    // Get second framebuffer pointer (nullptr if not allocated)
    uint16_t* getFramebuffer2() { return _framebuffer2; }

    // Triple buffering (optional; needs the second framebuffer first).  A third
    // buffer lets a renderer start the next frame while a finished one waits
    // for its flip.  setDrawBuffer(3) and setActiveFramebuffer(3) then work.
    bool allocateThirdFramebuffer();
    uint16_t* getFramebuffer3() { return _framebuffer3; }
    // Framebuffer 1, 2 or 3 by index (nullptr if not allocated).
    uint16_t* framebufferByIndex(uint8_t index) {
        return index == 3 ? _framebuffer3 : index == 2 ? _framebuffer2 : _framebuffer;
    }
    // The buffer being scanned out, and the one queued for the next frame
    // start (equal when nothing is queued).
    uint8_t activeFramebuffer() const { return _active_fb; }
    uint8_t pendingFramebuffer() const { return _pending_fb; }

    // Frame clock: milliseconds at the start of the most recent panel frame.
    // It advances once per refresh, by exactly the frame period, so it
    // suits LVGL's tick when animation steps should land one frame apart.
    // (Stops if the panel stops.)
    uint32_t frameTickMs() const;
    uint32_t frameCount() const;
    // The measured length of the last panel frame, in microseconds (0 until
    // two frames have started).  1e6 / this is the true refresh rate, after
    // the S3's pixel-clock divider has rounded the requested pclk.
    uint32_t framePeriodUs() const;

    // --- App-rendered rows (a "raster band") ---
    // Rows y0..y1-1 of the panel are no longer read from the framebuffers.
    // Instead the bounce-fill task calls fn to draw them, chunk by chunk,
    // straight into the SRAM bounce buffer just before the panel scans it:
    //
    //   fn(dst, y, rows, frame, ctx)
    //     dst    first pixel of row y in the bounce buffer (RGB565, row stride
    //            width() pixels, 4-byte aligned)
    //     y      first panel row to draw; rows: how many (bounceRows())
    //     frame  frameCount() of the panel frame these rows belong to.  It
    //            is the same for every chunk of one frame, so the first call
    //            that sees a new value is where the app advances its state:
    //            everything drawn in that frame then comes from one state,
    //            and nothing tears.
    //
    // fn runs in the fill task -- the display core, top priority -- so it
    // must be quick and must not block: it has until the panel reaches those
    // rows, about one bounce period (bounceRows() scan lines) after its
    // interrupt.  g_rasterLate counts chunks that missed that, and
    // g_rasterMaxUs is the worst interrupt-to-done time.  Keep fn and its
    // data in internal RAM (IRAM_ATTR, ordinary globals): the fill shares
    // the cache and the PSRAM bus with everything else.
    //
    // y0 and y1 are rounded outward to bounceRows() multiples.  The rows'
    // framebuffer contents are still written (LVGL may draw there) but are not
    // shown.  Panel coordinates: the native landscape orientation, unaffected
    // by LVGL's rotation.  Not available in half resolution, RGB332 or with
    // the driver's 180-degree flip; returns false there.  The change takes
    // effect at the next frame start.  fn = nullptr (or y0 >= y1) turns the
    // band off.
    typedef void (*RasterFn)(uint16_t* dst, int y, int rows, uint32_t frame, void* ctx);
    bool setRasterRows(int y0, int y1, RasterFn fn, void* ctx = nullptr);

    // --- Overlay (OWLOS Flight Radar fork addition, not in the published library) ---
    // fn is called for every chunk, in the fill task, just after the chunk's
    // framebuffer rows have been copied into the bounce buffer: it draws on
    // top of them before the panel scans them.  Same arguments and the same
    // rules as RasterFn (quick, non-blocking, internal RAM, frame number for
    // latching per-frame state).  fn is called one row at a time: with the
    // GDMA fill, each row as soon as the copy has passed it; otherwise after
    // the CPU has copied the chunk.  The row cache is bypassed while fn is set.
    // The panel starts on the buffer one bounce period after its interrupt and
    // scans it row by row, so fn should finish the rows in order: a chunk is
    // safe as long as row r is done before (1 + r / bounceRows()) periods.
    // Each row has a deadline (when the panel starts scanning it, less
    // LCD7_7B_OVL_MARGIN_US); a row that cannot start by then is skipped, left
    // as the copy made it.  g_ovlLate counts chunks with a skipped row.
    // prep, if given, is called for each row before its copy has landed (dst
    // nullptr, rows 1), for the work that needs no pixels; fn follows for the
    // same row once they are there.
    // want, if given, is asked first for each chunk (y, rows, frame): false
    // means nothing goes over those rows this frame, and the chunk is copied
    // as without an overlay (the fill does not follow the copy, the core is
    // free).  It runs before fn and prep in each frame, so per-frame state can
    // be latched there.
    typedef bool (*OverlayWantFn)(int y, int rows, uint32_t frame, void* ctx);
    // src, if given, is asked before anything else for each chunk: a buffer of
    // the chunk's rows to copy from instead of the framebuffer (composed
    // beforehand by the app, 64-byte aligned), or nullptr.  want is asked
    // after it, so a chunk that needs nothing more is a plain copy.
    typedef const uint16_t* (*OverlaySrcFn)(int y, int rows, uint32_t frame, void* ctx);
    void setOverlay(RasterFn fn, void* ctx = nullptr, RasterFn prep = nullptr, OverlayWantFn want = nullptr,
                    OverlaySrcFn src = nullptr) {
        _ovlFn = nullptr; _ovlCtx = ctx; _ovlPrep = prep; _ovlWant = want; _ovlSrc = src; _ovlFn = fn;
    }
    static constexpr int bounceRows() { return LCD7_7B_BOUNCE_ROWS; }

    // Set which framebuffer to draw to (1 = primary, 2 = secondary)
    // Drawing functions (fillRect, pushImage, drawPixel) will use this buffer
    // Default is primary (1). Call after allocateSecondFramebuffer().
    void setDrawBuffer(uint8_t index);

    // Get current draw buffer index (1 or 2)
    uint8_t getDrawBuffer() const { return _draw_fb_index; }

    // Get pointer to current draw target buffer
    uint16_t* getDrawTarget() { return _draw_target; }

    // Set which framebuffer becomes active at next vsync
    // index: 0 = swap between 1 and 2, 1 = primary, 2 = secondary, 3 = third
    // Takes effect when ISR requests row 0 of the next frame
    // If wait=true, blocks until the swap has occurred (safe to draw to other buffer)
    void setActiveFramebuffer(uint8_t index, bool wait = false);

    // The PSRAM clock actually in use, read from the MSPI registers.  The Arduino
    // core's prebuilt libraries run octal PSRAM at 80 MHz; a core rebuilt with
    // CONFIG_SPIRAM_SPEED_120M shows 120 here, with ~1.5x the bus bandwidth.
    // (Assumes the 480 MHz PLL, i.e. a 240 MHz CPU clock.)
    static uint32_t psramClockMHz();

    // --- Pacing PSRAM writes against the bounce fill (BOUNCE_FILL_GDMA) ---
    // Code that writes the framebuffers in bulk (the PARTIAL-mode renderer in
    // lv_setup) calls paceWrites() before each piece it writes.  It returns at
    // once unless the most recent DMA chunk finished with less than
    // LCD7_7B_PACE_SLACK_US of slack, in which case it waits (up to
    // LCD7_7B_PACE_MAX_WAIT_US) for a healthier chunk.  No-op in the CPU fill modes,
    // where the fill and the writer share a core and balance themselves.
    void paceWrites();
    // Make CPU writes to [addr, addr+len) visible to DMA (dcache write-back).
    // No-op unless the GDMA fill is running.
    void syncToPsram(const void* addr, size_t len);
    // Range write-back for a writer that tracks what it wrote (lv_setup's
    // renderer): write [addr, addr+len) out of the data cache now, while the
    // lines are still hot.  With setRangeWriteBack(true) the flip then skips
    // its whole-cache write-back -- and the stall of the other core that goes
    // with it.  No-op without the GDMA fill.
    void writeBackRange(const void* addr, size_t len);
    void setRangeWriteBack(bool on) { _rangeWriteBack = on; }

    // --- Row cache (run-length-encoded scanlines; BOUNCE_FILL_GDMA only) ---
    // Each screen row may carry a short op list in PSRAM -- RUN(n, colour) for
    // flat stretches, COPY(n) for "fetch n pixels from the framebuffer here" --
    // so the bounce fill expands it instead of reading the whole 2 KB row from
    // PSRAM every refresh.  Rows are encoded opportunistically by the fill task
    // from the bounce buffer just after their DMA lands (no extra PSRAM reads),
    // only while no fill request is waiting.  A row whose op list plus copied
    // pixels would exceed LCD7_7B_RLE_MAX_READ bytes is marked raw and DMA'd.
    // Whoever writes the framebuffers must report it: lv_setup's renderer calls
    // markRowsChanged() for rows it draws into a back buffer, and the drawing
    // calls below mark their rows themselves.  Bypassed (whole chunk DMA'd)
    // while scrolled, with sprites, at 180 degrees and in RGB332 mode.
    // setRowCache(true) after begin(); it allocates ~150 KB of PSRAM.
    bool setRowCache(bool on);
    // Fill-ahead (row cache on): a periodic timer has the fill task copy the
    // next chunk into the rows of the bounce buffer the panel has already
    // scanned, so when that buffer's interrupt comes only its last few rows
    // remain.  Adds up to about one bounce period to every fill's deadline.
    // Never crosses a frame boundary or vertical blanking.  By default it is
    // on with the CPU fill (it made 5 balls clean there) and off with GDMA
    // (which already has ~2 periods of slack, and where fill-ahead chains
    // delayed the fills that had deadlines).  This call overrides that.
    void setFillAhead(bool on) { _fillAhead = on ? 1 : 0; }
    bool rowCacheEnabled() const { return _rle; }
    // Rows y1..y2 were drawn into a back buffer: they change when framebuffer
    // fbIndex (1..3) is next shown, or with fbIndex 0, at the next flip.
    void markRowsChanged(int y1, int y2, uint8_t fbIndex = 0);

    // Get display dimensions
    uint16_t width() { return _width; }
    uint16_t height() { return _height; }

    // --- Half resolution (opt-in) ---
    // The framebuffers hold half the panel each way (512x300 or 400x240) and the bounce fill scales them up
    // 2x to the panel, interpolating: each new pixel between two source pixels
    // is their average, so edges look soft rather than blocky.  Drawing (LVGL)
    // has a quarter of the pixels to produce, and the fill reads a quarter as
    // much PSRAM.  The fill copies with the CPU, so there is no GDMA, no cache
    // write-back and no row cache.  For walk-up UIs with big buttons, where
    // smooth motion matters more than small text.  Call before begin(); the
    // drawing calls (fillRect, pushImage...), sprites and scrolling are not
    // supported in this mode -- draw through LVGL (lv_setup).
    void setHalfResolution(bool on) {
        _half = on;
        _fb_stride = (on ? _width / 2 : _width) + _fb_pad_x;
        _fb_rows = (on ? _height / 2 : _height) + _fb_pad_y;
        _fb_size = (uint32_t)_fb_stride * _fb_rows;
        _framebuffer_size = _fb_size * sizeof(uint16_t);
    }
    bool halfResolution() const { return _half; }
    // The framebuffers' size: the panel's, or half of it in each direction.
    uint16_t fbWidth() const { return _half ? _width / 2 : _width; }
    uint16_t fbHeight() const { return _half ? _height / 2 : _height; }

    // Copy RGB565 pixels to framebuffer region
    void copyPixels(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* pixels) {
        pushImage(x,y,w,h,pixels);
    }

    void pushImage(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* pixels);

    // Fill rectangle with solid color
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

    // Fill entire screen with color
    void fillScreen(uint16_t color);

    // Draw a single pixel
    void drawPixel(int16_t x, int16_t y, uint16_t color);

    // Flush cache for entire framebuffer
    void flush();

    // Scroll framebuffer by dx,dy pixels, filling exposed area with fill_color
    // Returns true if fill was performed, false if caller should handle exposed area
    bool scrollFramebuffer(int16_t dx, int16_t dy, uint16_t fill_color);

    // Hardware scroll - moves the virtual origin without copying memory
    // After scrolling, you should fill the newly exposed area
    void setScrollOffset(int16_t x, int16_t y);
    uint32_t getScrollOffset() { return _pending_scroll_offset; }
    int16_t getScrollX() { return _pending_scroll_offset % _fb_stride; }
    int16_t getScrollY() { return _pending_scroll_offset / _fb_stride; }

    // Scroll by delta, optionally fill exposed area
    // If fill=true and scroll fits in padding, pre-draws fill before scroll (no tearing)
    // Any fill that doesn't fit in padding is post-drawn after scroll
    void scroll(int16_t dx, int16_t dy, uint16_t fill_color = 0, bool fill = true);

    // Sprite system - hardware-accelerated overlays rendered by ISR
    // Sprites are drawn in display coordinates (unaffected by scrolling)
    // Changes take effect at the next frame boundary (vsync)

    // Set a sprite's properties. The sprite will appear at the next frame.
    // slot: 0 to SPRITE_MAX_COUNT-1
    // data: pointer to RGB565 pixel data (must remain valid; ideally in SRAM)
    // x, y: top-left position in display coordinates (can be negative for partial offscreen)
    // w, h: dimensions in pixels
    // transparent: color value to treat as transparent (pixels with this color are skipped)
    // flags: sprite flags (e.g., SPRITE_FLAG_COLLISION_DETECT)
    void setSprite(uint8_t slot, const uint16_t* data, int16_t x, int16_t y,
                   uint16_t w, uint16_t h, uint16_t transparent = 0xF81F,
                   uint32_t flags = 0);

    // Disable a sprite slot (sprite will disappear at next frame)
    void clearSprite(uint8_t slot);

    // Disable all sprites
    void clearAllSprites();

    // Get pointer to pending sprite array for advanced manipulation
    // Note: modify with care; changes take effect at next frame
    Sprite* getPendingSprites() { return _pending_sprites; }

    // Check if a sprite collision was detected and clear the flag
    // Returns true if a collision occurred since the last check
    // Collision detection must be enabled via SPRITE_FLAG_COLLISION_DETECT flag
    // A collision is detected when a sprite overwrites more than one unique color
    bool checkSpriteCollision(uint8_t slot);

    // Flash write overlay: display a "FLASH MEMORY WRITE" message during flash
    // operations that stall PSRAM access. Call beginFlashMessage() before the
    // flash write and endFlashMessage() after. The message is pre-rendered into
    // an SRAM buffer so the ISR can display it without touching PSRAM or flash.
    // The ISR renders a black screen with the message positioned vertically.
    //
    // y: Approximate vertical pixel position for the top of the message.
    //    Rounded down to the nearest bounce buffer boundary (multiple of
    //    LCD7_7B_BOUNCE_ROWS lines).
    //    Default (-2) centers the message vertically on screen.
    //    -1 blanks the screen without displaying any message text.
    //    -3 tiles the message on every bounce buffer band, so it remains visible
    //       even if the DMA replays a single chunk during a flash stall.
    void beginFlashMessage(int16_t y = -3);
    void endFlashMessage();

private:
    // The board that owns the shared I2C bus and the IO-extension expander.  The
    // panel's reset and supply and the backlight are driven through it (see
    // begin/setBacklight).
    ESP32S3_Touch_LCD_7_7B_Board& _board;

    // Friend function for bounce buffer callback (needs access to scroll offsets)
    friend bool bounceBufferFillCallback(esp_lcd_panel_handle_t, void*, int, int, void*);
    friend bool cglcd7_7b_prefillTick(ESP32S3_Touch_LCD_7_7B* d);

    // The heavy PSRAM->bounce-buffer copy (and sprite overlay) runs in a
    // dedicated high-priority task instead of the GDMA ISR, so the ISR stays
    // short and does not block other interrupts on its core.  The ISR
    // (bounceBufferFillCallback) still does the cheap, time-critical work --
    // frame-start latching, the desync detection/redirect, and the flash-overlay
    // fill -- then hands the chosen buffer to this task via _bounceQueue.
    // _bounceFillTask drains the queue and performs the copy.
    static void _bounceFillTask(void* arg);
    void _fillBounceBuffer(void* bounce_buf, int pos_px, int len_bytes,
                           uint16_t* framebuffer, uint32_t scroll_offset);
    void _fillBounceBuffer332(void* bounce_buf, int pos_px, int len_bytes,
                              const uint8_t* framebuffer, uint32_t scroll_offset);
    void _fillHalf(void* bounce_buf, int pos_px, int len_bytes, const uint16_t* framebuffer);
    bool _half = false;   // half resolution (see setHalfResolution)
    // Sprite overlay, factored out of _fillBounceBuffer.
    void _overlaySprites(void* bounce_buf, int pos_px, int len_bytes);
    QueueHandle_t _bounceQueue = nullptr;   // ISR -> fill-task hand-off
    void*         _dma = nullptr;           // non-null once the GDMA fill is set up (BOUNCE_FILL_GDMA)
    bool _dmaFill(void* bounce_buf, int pos_px, int len_bytes,
                  uint16_t* framebuffer, uint32_t scroll_offset, uint32_t isr_cycles);
    TaskHandle_t  _bounceTask  = nullptr;   // the high-priority fill task
    BounceFillMode _bounceFillMode = BOUNCE_FILL_DEFAULT;  // runtime selection; see setBounceFillMode()

    // Display properties (visible screen size).  The larger panel's until
    // begin() (or _applyBoard()) sets the identified board's.
    uint16_t _width = LCD7_7B_MAX_WIDTH;
    uint16_t _height = LCD7_7B_MAX_HEIGHT;
    // The identified board's porches (pixel clocks / lines) and clocks.
    uint16_t _hpw = 162, _hbp = 152, _hfp = 48, _vpw = 45, _vbp = 13, _vfp = 3;
    uint32_t _defaultPclk = 30000000, _minPclk = 12000000;
    bool _applyBoard();   // geometry + timing from _board.model()

    // 180-degree rotation, fixed at begin().  When true, the bounce-buffer fill
    // presents physical pixel p as logical pixel (_width*_height - 1 - p): the
    // PSRAM framebuffer is still read forward/sequentially, but written into the
    // SRAM bounce buffer backward.  Sprites and the flash-write overlay are
    // flipped to match.  Immutable after begin(), so it needs no latching.
    bool _rotate180 = false;

    // COLOR_RGB332 mode (see setColorMode).  When set, _framebuffer and
    // _framebuffer2 hold uint8_t pixels despite their type.
    bool _rgb332 = false;

    // Bounce buffer size in pixels.  The ESP32-S3 RGB driver streams the
    // framebuffer through two SRAM bounce buffers; this is the size of each.
    // It must divide LCD_width*LCD_height an even number of times.  We use
    // width*20 px (20 scanlines): 30 chunks on the -7B, 24 on the -7.  Two
    // buffers then span 40 lines, which keeps the no-PSRAM / flash-overlay
    // banding math the same as the 2.8C driver this is derived from.
    int _bounce_buffer_size_px = LCD7_7B_MAX_WIDTH * LCD7_7B_BOUNCE_ROWS;

    // Framebuffer layout - linear ring buffer with row stride
    // Each screen row is _width contiguous pixels
    // Rows are spaced _fb_stride apart, leaving _fb_pad_x pixel gaps for pre-draw
    // Extra rows at the end provide vertical pre-draw space
    uint16_t _fb_pad_x;    // Horizontal padding per row
    uint16_t _fb_pad_y;    // Extra rows for vertical padding
    uint16_t _fb_stride;   // Row stride in pixels (_width + _fb_pad_x)
    uint16_t _fb_rows;     // Total rows (_height + _fb_pad_y)
    uint32_t _fb_size;     // Total buffer size (_fb_stride * _fb_rows)

    // Framebuffers
    uint16_t* _framebuffer = nullptr;   // Primary framebuffer (always allocated)
    uint16_t* _framebuffer2 = nullptr;  // Secondary framebuffer (optional, for double buffering)
    uint16_t* _framebuffer3 = nullptr;  // Third framebuffer (optional, for triple buffering)
    bool _rangeWriteBack = false;       // the writer writes back its own ranges
    void _clearPaced(uint16_t* fb);     // memset to 0 in pieces gated by paceWrites()
    size_t _framebuffer_size;

    // Draw target - which buffer drawing operations use
    uint16_t* _draw_target = nullptr;   // Points to either _framebuffer or _framebuffer2
    uint8_t _draw_fb_index = 1;         // Which buffer is the draw target (1 or 2)

    // Double buffering state
    // Active is read by ISR, pending is written by main code
    // Values: 1 = primary (_framebuffer), 2 = secondary (_framebuffer2)
    volatile uint8_t _active_fb = 1;    // Currently displayed framebuffer
    volatile uint8_t _pending_fb = 1;   // Framebuffer to display at next vsync

    // Scroll offset - linear position in the ring buffer
    // Double buffered: pending is set by user, copied to active at frame start
    // Screen pixel (x,y) maps to fb[(scroll_offset + y*fb_stride + x) % fb_size]
    volatile uint32_t _scroll_offset = 0;         // Active offset (ISR reads this)
    volatile uint32_t _pending_scroll_offset = 0; // Pending offset (main code writes this)

    // LCD panel handle
    esp_lcd_panel_handle_t _panel_handle = nullptr;

    // Sprite arrays - double buffered for tear-free updates
    // Application writes to _pending_sprites, ISR reads from _active_sprites
    // Copy occurs at frame start (pos_px == 0)
    Sprite _pending_sprites[SPRITE_MAX_COUNT];
    Sprite _active_sprites[SPRITE_MAX_COUNT];

    // Collision detection array - one 32-bit word per sprite slot
    // ISR sets (never clears) when sprite with SPRITE_FLAG_COLLISION_DETECT
    // overwrites more than one unique color during rendering
    volatile uint32_t _sprite_collisions[SPRITE_MAX_COUNT];

    // Row cache state (see setRowCache).  Arrays live in the object, i.e. in
    // internal RAM, because the ISR touches _row at flips.
    friend struct RowCacheAccess;
    bool _rle = false;
    uint8_t*  _rleSlots = nullptr;                 // PSRAM: _height fixed-size op slots
    // Only what the ISR touches lives here (it must work with the cache off,
    // during flash writes); op-list length and the like are in the PSRAM slot.
    // One byte per row: bits 7..6 state (ROW_DIRTY / ROW_RLE / ROW_RAW), bits
    // 5..0 a generation bumped whenever the row changes -- enough to tell
    // whether it changed between its DMA and its harvest (~1 ms apart).
    uint8_t   _row[LCD7_7B_MAX_HEIGHT];   // see ROW_* in the .cpp
    // Rows drawn but not yet shown: [0] at the next flip, [1..3] when that
    // framebuffer is next shown.
    uint32_t  _rowNextDirty[4][(LCD7_7B_MAX_HEIGHT + 31) / 32];
    void*     _bbAddr[2] = {nullptr, nullptr};     // the two bounce buffers, as learned
    uint32_t  _bbRecId[2] = {0, 0};                // harvest record now describing each
    void _rleApplyFlip(uint8_t fb);                // ISR, at a frame-start flip to fb
    void _rleMarkNow(int y1, int y2);              // drawing calls: changed in place
    bool _rleFill(void* bounce_buf, int pos_px, int len_bytes,
                  uint16_t* framebuffer, uint32_t scroll_offset, uint32_t isr_cycles,
                  int rowTo = -1, bool prefill = false);
    // Fill-ahead state.  Per bounce buffer: the chunk being pre-filled, how
    // many of its rows are done, and its harvest content id.
    int8_t    _fillAhead = -1;          // -1 = automatic (on without GDMA)
    int       _pfPos[2] = {-1, -1};
    int       _pfRows[2] = {0, 0};
    uint32_t  _pfId[2] = {0, 0};
    // The latest bounce interrupt, for the pre-fill to work out where the scan is.
    uint32_t  _isrCycles = 0;
    void*     _isrBuf = nullptr;
    int       _isrPos = 0;
    uint16_t* _isrFb = nullptr;
    uint32_t  _rowCycles = 0;          // one scan line (htotal pixel clocks) in CPU cycles
    void*     _pfTimer = nullptr;      // gptimer handle
    void _prefillStep();
    void _rleHarvest(void* rec);

    // Raster band (see setRasterRows).  The app sets _rasterPending; the
    // interrupt latches it into _rasterActive at frame start.
    struct RasterBand { int y0, y1; RasterFn fn; void* ctx; };
    volatile RasterBand _rasterPending = {0, 0, nullptr, nullptr};
    RasterBand _rasterActive = {0, 0, nullptr, nullptr};
    volatile RasterFn _ovlFn = nullptr;   // setOverlay (fork addition)
    void* volatile _ovlCtx = nullptr;
    volatile RasterFn _ovlPrep = nullptr;
    volatile OverlayWantFn _ovlWant = nullptr;
    volatile OverlaySrcFn _ovlSrc = nullptr;
    void _ovlSource(struct BounceFillRequest& req);
    void _overlayRows(const struct BounceFillRequest& req, RasterFn ovl, uint32_t slot, bool dma);
    void _rasterFill(void* bounce_buf, int pos_px, int len_bytes, RasterFn fn,
                     void* ctx, uint32_t frame, uint32_t isr_cycles);
};

// ISR timing instrumentation (cycle counts at 240MHz CPU)
// Use these to measure bounce buffer callback overhead
extern volatile uint64_t g_isrCyclesIn;   // Total cycles spent inside callback
extern volatile uint64_t g_isrCyclesOut;  // Total cycles spent outside callback
extern volatile uint32_t g_isrCallCount;  // Number of callback invocations
extern volatile uint32_t g_bbDesyncCount; // Number of bounce buffer desync events detected
extern volatile uint32_t g_bbDroppedCount;
// BOUNCE_FILL_GDMA instrumentation.  Slack = two bounce periods minus the time
// from the bounce interrupt to DMA completion (see dmaDoneCb for why two).
extern volatile uint32_t g_bbDmaChunks;     // chunks filled by DMA
extern volatile uint32_t g_bbDmaFallbacks;  // chunks the CPU had to fill instead
extern volatile uint32_t g_bbDmaLate;       // DMA finished after its deadline
extern volatile int32_t  g_bbDmaMinSlackUs; // worst slack seen (reset it to INT32_MAX to re-measure)
extern volatile uint32_t g_bbPeriodUs;      // last measured bounce period
extern volatile uint32_t g_bbDmaMaxUs;
extern volatile int32_t  g_bbDmaLastSlackUs; // slack of the most recent DMA chunk
extern volatile uint32_t g_bbPaceWaits;      // paceWrites() calls that had to wait
extern volatile uint32_t g_bbPaceWaitUs;     // total time spent waiting there
extern volatile uint32_t g_flipWriteBackMaxUs; // longest flip write-back, other core stalled (reset to 0 to re-measure)
extern volatile uint32_t g_halfFillCycles;    // CPU cycles in the half-resolution fill (cumulative)
extern volatile uint32_t g_halfScalarChunks;  // its chunks done in C (bounce buffer not 16-byte aligned)
extern volatile uint32_t g_wbRangeCycles;      // CPU cycles in writeBackRange (cumulative)
extern volatile uint32_t g_wbRangeCalls;       // its calls
extern volatile uint32_t g_wbRangeMaxUs;       // the longest one (reset to 0 to re-measure)
// Raster band (setRasterRows) instrumentation.
extern volatile uint32_t g_rasterChunks;  // chunks drawn by the app's fn (cumulative)
extern volatile uint32_t g_rasterLate;    // of those, finished after the panel reached them
extern volatile uint32_t g_rasterMaxUs;   // worst interrupt-to-done time (reset to 0 to re-measure)
extern volatile uint32_t g_rasterCycles;  // CPU cycles inside fn (cumulative)
extern volatile uint32_t g_rasterMaxFnUs;   // longest fn call (reset to 0 to re-measure)
extern volatile uint32_t g_rasterDmaWaits;  // chunks that first waited for a DMA copy to finish
// Overlay (setOverlay, fork addition) instrumentation.
extern volatile uint32_t g_ovlChunks;     // chunks the overlay fn was called for
extern volatile uint32_t g_ovlLate;       // of those, chunks with a row skipped for being past its deadline
extern volatile uint32_t g_ovlRowsSkipped; // rows skipped so
extern volatile uint32_t g_ovlSkipQ[4];    // the same, by quarter of the panel, top first
extern volatile uint16_t g_ovlSkipChunk[64]; // the same, by chunk
extern volatile uint32_t g_ovlSkipDma;     // of those, the copy had not reached the row by its deadline
extern volatile uint32_t g_ovlEarly;       // copies started while the overlay was still on the chunk before
extern volatile uint32_t g_ovlRowDeadline;
extern volatile uint32_t g_ovlPrepCycles, g_ovlSpinCycles;   // the fill's time in prep, and spinning on the copy // set before each call of the overlay fn: its row's deadline in cycles, 0 none
extern volatile uint32_t g_ovlMaxUs;      // worst interrupt-to-done time (reset to 0 to re-measure)
extern volatile uint32_t g_ovlCycles;     // CPU cycles inside fn (cumulative)
// Row cache counters (cumulative).
extern volatile uint32_t g_rleRowsExpanded;  // rows filled from their op list
extern volatile uint32_t g_rleRowsDma;       // rows DMA'd (dirty or raw)
extern volatile uint32_t g_rleEncoded;       // rows encoded into an op list
extern volatile uint32_t g_rleRaw;           // rows judged not worth encoding
extern volatile uint32_t g_rleHarvestYields; // harvests cut short by a waiting fill
extern volatile uint32_t g_rlePsramBytes;    // bytes the fill read from PSRAM (est.)
extern volatile uint32_t g_rleEncodeCycles;  // CPU cycles spent encoding rows
extern volatile uint32_t g_rleExpandCycles;  // CPU cycles spent expanding rows      // worst interrupt-to-DMA-done time (reset to 0 to re-measure)// Fill requests dropped because the fill task could not keep up (queue full)

// Diagnostic: last GDMA state seen by ISR (for debugging desync detection)
extern volatile uint32_t g_bbDiag_dscr;      // Last descriptor address from GDMA out.dscr
extern volatile uint32_t g_bbDiag_bufAddr;   // Buffer address extracted from that descriptor
extern volatile uint32_t g_bbDiag_bounceBuf; // bounce_buf pointer callback was asked to fill



#endif // CHIPGUY_ESP32S3_TOUCH_LCD_7_7B_H
