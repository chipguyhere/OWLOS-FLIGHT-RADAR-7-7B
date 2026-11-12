/*
 * chipguy_ESP32S3_Touch_LCD_7_7B - Display driver for the Waveshare
 * ESP32-S3-Touch-LCD-7 (800x480 ST7262) and -7B (1024x600) boards, chosen at
 * begin() from the board ESP32S3_Touch_LCD_7_7B_Board identified.
 *
 * The panels are plain RGB-interface TFTs with no command controller, so unlike
 * the ST7701-based 2.8C board there is no SPI bring-up sequence: the ESP32-S3
 * RGB LCD peripheral drives the panel directly.  The panel reset and the LCD
 * backlight are reached through the board's I2C expander.
 *
 * Copyright (c) 2025-2026 chipguyhere
 * MIT License
 */

#include <Arduino.h>
#include "chipguy_ESP32S3_Touch_LCD_7_7B_display.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_async_memcpy.h"
#include "esp_private/gdma.h"
#include "esp_memory_utils.h"   // esp_ptr_internal() -- bounds-check pointers during GDMA channel scan
#include "hal/dma_types.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_ipc_isr.h"
#include "driver/gptimer.h"
#include "esp_cache.h"
#include "soc/spi_mem_reg.h"
#include "freertos/portmacro.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Data-cache autoload over the framebuffers.  After a miss in a configured
// region, the S3's dcache fetches the following line(s) in the background, so
// the sequential framebuffer read in _fillBounceBuffer overlaps its next miss.
// Measured: ~5 % less fill time (app core left 39.8 % -> 42.8 % at 25 MHz)
// with 1-2 lines; 4-8 lines was slightly worse.  This is cache-global ROM
// state (two regions exist; we take both), so set this to 0 if something else
// in the sketch configures dcache autoload.
#ifndef LCD7_7B_DCACHE_AUTOLOAD
#define LCD7_7B_DCACHE_AUTOLOAD 1
#endif
#if LCD7_7B_DCACHE_AUTOLOAD
#include "esp32s3/rom/cache.h"
// Point autoload region `region` at [addr, addr+size) and (re)enable autoload.
static void enableFramebufferAutoload(uint8_t region, void* addr, uint32_t size) {
    struct autoload_config cfg = {1, CACHE_AUTOLOAD_POSITIVE, CACHE_AUTOLOAD_MISS_TRIGGER, 2};
    struct autoload_region_config r = {region, 1, (uint32_t)addr, size};
    Cache_Disable_DCache_Autoload();
    Cache_Config_DCache_Autoload(&cfg);
    Cache_Config_DCache_Region_Autoload(&r);
    Cache_Enable_DCache_Autoload();
}
#endif

#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_private/gdma.h"
#include "hal/lcd_hal.h"
#include "hal/lcd_ll.h"
#include "soc/gdma_struct.h"
#include "hal/gdma_ll.h"

// Cache writeback function from ROM
#include "esp32s3/rom/cache.h"
extern "C" int Cache_WriteBack_Addr(uint32_t addr, uint32_t size);


// Helper macros for sprite coordinate/dimension packing
#define SPRITE_YX(y, x)     (((uint32_t)(uint16_t)(y) << 16) | (uint16_t)(x))
#define SPRITE_HW(h, w)     (((uint32_t)(uint16_t)(h) << 16) | (uint16_t)(w))
#define SPRITE_GET_Y(yx)    ((int16_t)((yx) >> 16))
#define SPRITE_GET_X(yx)    ((int16_t)((yx) & 0xFFFF))
#define SPRITE_GET_H(hw)    ((uint16_t)((hw) >> 16))
#define SPRITE_GET_W(hw)    ((uint16_t)((hw) & 0xFFFF))

// Pin definitions for the Waveshare ESP32-S3-Touch-LCD-7 and -7B boards (the
// RGB pins are the same on both).
//
// This is a 16-bit parallel RGB panel.  The panel reset and backlight are NOT
// on ESP32 GPIOs -- they hang off the I2C expander (LCD reset = EXIO3,
// backlight enable = EXIO2; on the -7B also panel supply = EXIO6 and the
// expander's PWM output for dimming), driven via ESP32S3_Touch_LCD_7_7B_Board.
// There is no
// chip-select or SPI: the RGB sync/data lines below are the entire interface.
#define PIN_LCD_DE       5
#define PIN_LCD_VSYNC    3
#define PIN_LCD_HSYNC    46
#define PIN_LCD_PCLK     7
// RGB565 data lines.  data_gpio_nums[0..15] = B0..B4, G0..G5, R0..R4.
#define PIN_LCD_B0       14
#define PIN_LCD_B1       38
#define PIN_LCD_B2       18
#define PIN_LCD_B3       17
#define PIN_LCD_B4       10
#define PIN_LCD_G0       39
#define PIN_LCD_G1       0
#define PIN_LCD_G2       45
#define PIN_LCD_G3       48
#define PIN_LCD_G4       47
#define PIN_LCD_G5       21
#define PIN_LCD_R0       1
#define PIN_LCD_R1       2
#define PIN_LCD_R2       42
#define PIN_LCD_R3       41
#define PIN_LCD_R4       40


// Cache writeback helper - writes back a memory region with interrupt protection
// The ISR reads through the same CPU cache, so it sees dirty cache lines automatically.
// This writeback may be unnecessary - disable by uncommenting the return statement.
static portMUX_TYPE s_cache_mux = portMUX_INITIALIZER_UNLOCKED;
// Only needed when something reads PSRAM behind the cache -- BOUNCE_FILL_GDMA.
// The CPU fill reads through the same cache that holds the dirty lines.
static bool s_cacheWriteBackNeeded = false;
// Every cache sync here (the flip's write-back of the whole cache, the drawing
// calls' range write-backs) holds s_cache_mux: the S3's cache sync registers
// take one operation at a time, from either core, and two at once panic
// ("Dcache sync parameter configuration error").
static inline void cacheWriteBack(const void* addr, size_t size) {
    if (!s_cacheWriteBackNeeded) return;
    portENTER_CRITICAL(&s_cache_mux);
    Cache_WriteBack_Addr((uint32_t)addr, size);
    portEXIT_CRITICAL(&s_cache_mux);
}

// ---------------------------------------------------------------------------
// Memory-to-memory GDMA for BOUNCE_FILL_GDMA.  The IDF's esp_async_memcpy()
// writes back the cache over its whole PSRAM source on every call -- about
// 125 us of CPU per 33 KB chunk, measured.  Here that is redundant: the
// framebuffers are written back at every flip (setActiveFramebuffer) and by
// the drawing calls.  So this drives a GDMA channel pair directly, with no
// cache maintenance: each chunk is one descriptor chain, queued in a small
// ring, and each chain's completion interrupt starts the next.
// ---------------------------------------------------------------------------
#define M2M_SLOTS 4
#define M2M_MAXD  24      // descriptors per chunk: 40 KB in 4032-byte pieces, plus splits
#define M2M_PIECE 4032    // the largest multiple of 64 a descriptor can hold
struct M2MSeg { uint8_t* d; uint8_t* s; size_t n; };
struct M2MXfer {
    dma_descriptor_t tx[M2M_MAXD];
    dma_descriptor_t rx[M2M_MAXD];
    async_memcpy_isr_cb_t cb;
    void* arg;
    int nd;            // fork addition: descriptors in the chain (m2mLanded)
};
static M2MXfer* s_m2m = nullptr;   // M2M_SLOTS of them, in DMA-capable internal RAM
static gdma_channel_handle_t s_m2mTx = nullptr, s_m2mRx = nullptr;
static portMUX_TYPE s_m2mMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_m2mHead = 0, s_m2mTail = 0;   // queued: [tail, head); tail is running

static inline void IRAM_ATTR m2mStart(M2MXfer* x) {
    gdma_start(s_m2mRx, (intptr_t)x->rx);
    gdma_start(s_m2mTx, (intptr_t)x->tx);
}

static bool IRAM_ATTR m2mEof(gdma_channel_handle_t, gdma_event_data_t*, void*) {
    portENTER_CRITICAL_ISR(&s_m2mMux);
    M2MXfer* x = &s_m2m[s_m2mTail % M2M_SLOTS];
    async_memcpy_isr_cb_t cb = x->cb;
    void* arg = x->arg;
    s_m2mTail = s_m2mTail + 1;
    if (s_m2mTail != s_m2mHead) m2mStart(&s_m2m[s_m2mTail % M2M_SLOTS]);
    portEXIT_CRITICAL_ISR(&s_m2mMux);
    return cb ? cb(nullptr, nullptr, arg) : false;
}

static bool m2mInit() {
    s_m2m = (M2MXfer*)heap_caps_calloc(M2M_SLOTS, sizeof(M2MXfer), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_m2m) return false;
    gdma_channel_alloc_config_t txc = {};
    txc.direction = GDMA_CHANNEL_DIRECTION_TX;
    txc.flags.reserve_sibling = 1;
    if (gdma_new_ahb_channel(&txc, &s_m2mTx) != ESP_OK) return false;
    gdma_channel_alloc_config_t rxc = {};
    rxc.direction = GDMA_CHANNEL_DIRECTION_RX;
    rxc.sibling_chan = s_m2mTx;
    if (gdma_new_ahb_channel(&rxc, &s_m2mRx) != ESP_OK) return false;
    gdma_trigger_t trig = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
    uint32_t mask = 0;
    if (gdma_get_free_m2m_trig_id_mask(s_m2mTx, &mask) == ESP_OK && mask) trig.instance_id = __builtin_ctz(mask);
    if (gdma_connect(s_m2mRx, trig) != ESP_OK || gdma_connect(s_m2mTx, trig) != ESP_OK) return false;
    gdma_strategy_config_t st = {};   // no owner check, no descriptor write-back: chains are reused
    gdma_apply_strategy(s_m2mTx, &st);
    gdma_apply_strategy(s_m2mRx, &st);
    gdma_transfer_config_t tc = {};
    tc.max_data_burst_size = 64;      // 16 is too slow: ~35 MB/s from PSRAM
    tc.access_ext_mem = true;
    if (gdma_config_transfer(s_m2mTx, &tc) != ESP_OK) return false;
    if (gdma_config_transfer(s_m2mRx, &tc) != ESP_OK) return false;
    gdma_rx_event_callbacks_t cbs = {};
    cbs.on_recv_eof = m2mEof;
    return gdma_register_rx_event_callbacks(s_m2mRx, &cbs, nullptr) == ESP_OK;
}

// Queue one chunk: segments copied in order, cb(arg) from the interrupt when
// all of it has landed.  False (nothing queued) if the ring is full or the
// chunk needs too many descriptors.  Call from one task.
static bool m2mSubmit(const M2MSeg* segs, int ns, async_memcpy_isr_cb_t cb, void* arg) {
    int nd = 0;
    for (int i = 0; i < ns; i++) nd += (segs[i].n + M2M_PIECE - 1) / M2M_PIECE;
    if (nd == 0 || nd > M2M_MAXD) return false;
    if (s_m2mHead - s_m2mTail >= M2M_SLOTS) return false;
    M2MXfer* x = &s_m2m[s_m2mHead % M2M_SLOTS];   // free: only this task advances head
    int k = 0;
    for (int i = 0; i < ns; i++) {
        for (size_t off = 0; off < segs[i].n; off += M2M_PIECE) {
            size_t len = segs[i].n - off;
            if (len > M2M_PIECE) len = M2M_PIECE;
            dma_descriptor_t* t = &x->tx[k];
            dma_descriptor_t* r = &x->rx[k];
            t->dw0.size = len; t->dw0.length = len; t->dw0.err_eof = 0; t->dw0.suc_eof = 0;
            t->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
            t->buffer = segs[i].s + off;
            r->dw0.size = len; r->dw0.length = 0; r->dw0.err_eof = 0; r->dw0.suc_eof = 0;
            r->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
            r->buffer = segs[i].d + off;
            t->next = (k + 1 < nd) ? &x->tx[k + 1] : nullptr;
            r->next = (k + 1 < nd) ? &x->rx[k + 1] : nullptr;
            k++;
        }
    }
    x->tx[nd - 1].dw0.suc_eof = 1;
    x->nd = nd;
    x->cb = cb;
    x->arg = arg;
    portENTER_CRITICAL(&s_m2mMux);
    bool idle = (s_m2mHead == s_m2mTail);
    s_m2mHead = s_m2mHead + 1;
    if (idle) m2mStart(x);
    portEXIT_CRITICAL(&s_m2mMux);
    return true;
}

// ISR timing instrumentation (cycle counts at 240MHz)
volatile uint64_t g_isrCyclesIn = 0;      // Total cycles spent inside ISR
volatile uint64_t g_isrCyclesOut = 0;     // Total cycles spent outside ISR
volatile uint32_t g_isrLastExit = 0;      // Cycle count when ISR last exited (32-bit is fine for deltas)
volatile uint32_t g_isrCallCount = 0;     // Number of ISR calls

// ---------------------------------------------------------------------------
// Bounce buffer desync detection and workaround
// ---------------------------------------------------------------------------
// PROBLEM: The ESP-IDF RGB LCD driver uses two bounce buffers (A and B) in a
// ping-pong arrangement. Normally the callback fills A while DMA sends B, then
// fills B while DMA sends A. Under certain conditions (e.g. flash/LittleFS
// writes introducing delays), the driver's phase can shift so the callback is
// asked to fill the SAME buffer the DMA is currently sending. This manifests
// as the display being vertically offset by the bounce buffer height
// with streaking artifacts where the CPU write races the DMA read.
//
// DETECTION: During begin(), we extract the GDMA channel handle from the
// panel's internal struct and cache a pointer to the GDMA TX out.dscr hardware
// register. In the ISR callback, we read this register to get the address of
// the DMA descriptor currently being processed, then dereference it to get the
// buffer address the DMA is actively reading from. If that address falls within
// the bounce buffer we've been asked to fill, we have a confirmed phase desync.
//
// WORKAROUND: When desync is detected, we redirect all writes (framebuffer
// copy, sprite overlay) to the OTHER bounce buffer, which is idle. This avoids
// corrupting the in-flight DMA transfer and places the data where the DMA will
// read it on its next pass.
//
// ESP-IDF DEPENDENCY: This workaround depends on:
//   1. Finding the GDMA channel handle (dma_chan) inside the opaque esp_rgb_panel_t
//      object. We locate it by scanning for a slot that walks like a real GDMA channel
//      (findGdmaChannel), so it survives struct reordering. No copy of the private
//      struct is kept.
//   2. The GDMA out.dscr register containing the current descriptor address.
//   3. The DMA descriptor layout: word0=flags, word1=buffer ptr, word2=next.
// If a future ESP-IDF version fixes the desync bug or changes any of these
// internals, this workaround can be removed by:
//   - Deleting this block of globals and findGdmaChannel()
//   - Deleting the desync detection + redirect block in bounceBufferFillCallback
//   - Deleting the GDMA setup block in begin()
//   - Removing the #include "soc/gdma_struct.h" and #include "hal/gdma_ll.h"
//   - Removing the g_bbDesyncCount / g_bbDiag_* externs from the header
// ---------------------------------------------------------------------------
volatile uint32_t g_bbDesyncCount = 0;    // Number of desync events detected
volatile uint32_t g_bbDroppedCount = 0;   // Fill requests dropped (queue full -> task fell behind)
volatile uint32_t g_bbDmaChunks = 0;
volatile uint32_t g_bbDmaFallbacks = 0;
volatile uint32_t g_bbDmaLate = 0;
volatile int32_t  g_bbDmaMinSlackUs = INT32_MAX;
volatile uint32_t g_bbPeriodUs = 0;
volatile uint32_t g_bbDmaMaxUs = 0;
volatile int32_t  g_bbDmaLastSlackUs = INT32_MAX;
volatile uint32_t g_bbPaceWaits = 0;
volatile uint32_t g_bbPaceWaitUs = 0;

// These three are used only by the desync workaround (LCD7_7B_BOUNCE_DESYNC_WORKAROUND);
// marked unused so they don't warn when it is compiled out (the default).
static volatile uint32_t* s_gdma_out_dscr_reg __attribute__((unused)) = nullptr;  // GDMA TX out.dscr register
static void* s_bb_addrs[2] __attribute__((unused)) = {nullptr, nullptr};           // Learned bounce buffer addresses
static size_t s_bb_size __attribute__((unused)) = 0;                               // Bounce buffer size in bytes

// Diagnostic: last values seen by ISR (readable from main loop for debugging)
volatile uint32_t g_bbDiag_dscr = 0;        // Last descriptor address read from GDMA
volatile uint32_t g_bbDiag_bufAddr = 0;     // Buffer address extracted from that descriptor
volatile uint32_t g_bbDiag_bounceBuf = 0;   // bounce_buf pointer we were asked to fill

// ---------------------------------------------------------------------------
// Flash write overlay message
// ---------------------------------------------------------------------------
// When s_flashMsgBitmap is non-null, the ISR displays a black screen with a
// centered message, reading only from SRAM (no PSRAM or flash access).
// The bitmap is 1bpp (1 bit per pixel, MSB first, rows padded to byte boundary),
// pre-rendered by beginFlashMessage() and freed by endFlashMessage().
// The ISR expands 1bpp to RGB565 (0=black, 1=white) row by row.
// Double-buffered: main code writes to pending, ISR copies to active at frame start.
// This ensures the transition happens at a frame boundary (no tearing).
static volatile uint8_t* s_flashMsgActive = nullptr;   // active bitmap (ISR reads)
static volatile uint8_t* s_flashMsgPending = nullptr;  // pending bitmap (main code writes)
static int s_flashMsgW = 0;          // width of message in pixels
static int s_flashMsgH = 0;          // height of message in pixels
static int s_flashMsgStride = 0;     // bytes per row (ceil(width/8))
static int s_flashMsgX = 0;          // x offset to center on screen
static int s_flashMsgY = 0;          // y offset to center on screen
static bool s_flashMsgTile = false;  // if true, tile message every bounce buffer band
// Sentinel value used when beginFlashMessage(-1) blanks screen without text.
// Must be non-null so the ISR enters the flash overlay path, but not heap-allocated.
static uint8_t s_blankSentinel = 0;

// Sanity bounds for a discovered channel (fall back to liberal literals on targets
// whose soc_caps spell these differently, so the scan still rejects garbage).
#ifndef SOC_GDMA_NUM_GROUPS_MAX
#define SOC_GDMA_NUM_GROUPS_MAX 4
#endif
#ifndef SOC_GDMA_PAIRS_PER_GROUP_MAX
#define SOC_GDMA_PAIRS_PER_GROUP_MAX 16
#endif

// Self-correcting GDMA channel lookup. Rather than hardcode the offset of the
// dma_chan field inside esp_rgb_panel_t (which drifts every time ESP-IDF reorders
// the struct -- see the desync block comment above, and the bug that motivated this),
// scan the opaque panel object for a pointer slot that looks like a real GDMA channel.
//
// IMPORTANT: gdma_get_group_channel_id() does NOT validate its argument -- it blindly
// dereferences dma_chan->pair->group->group_id, so calling it on a non-channel pointer
// can fault. We therefore replicate that exact walk ourselves but bounds-check every
// pointer hop with esp_ptr_internal() first (GDMA channel objects live in internal
// DRAM). The fields we read are all stable, long-lived offset-0 members of ESP-IDF's
// gdma core structs (verified against v5.5.4):
//     gdma_channel_t.pair   @ offset 0
//     gdma_pair_t.group     @ offset 0
//     gdma_pair_t.pair_id   @ offset sizeof(void*)   (the channel id)
//     gdma_group_t.group_id @ offset 0
// Any future layout drift degrades safely: a bad pointer fails esp_ptr_internal() or an
// out-of-range id is rejected, so we return nullptr (workaround disabled) rather than crash.
// Window [8, 40) pointer slots comfortably brackets dma_chan's offset across 5.5.x.
// Returns the channel handle and fills group_id/channel_id/slot, or nullptr if not found.
static gdma_channel_handle_t __attribute__((unused)) findGdmaChannel(void* panel_handle,
                                             int* out_group_id, int* out_channel_id,
                                             int* out_slot) {
    void** slots = (void**)panel_handle;
    for (int i = 8; i < 40; i++) {
        void* cand = slots[i];                              // candidate gdma_channel_t*
        if (!esp_ptr_internal(cand)) continue;
        void* pair = *(void**)cand;                         // gdma_channel_t.pair
        if (!esp_ptr_internal(pair)) continue;
        void* group = *(void**)pair;                        // gdma_pair_t.group
        if (!esp_ptr_internal(group)) continue;
        int group_id   = *(int*)group;                      // gdma_group_t.group_id
        int channel_id = *(int*)((uint8_t*)pair + sizeof(void*)); // gdma_pair_t.pair_id
        if (group_id < 0 || group_id >= SOC_GDMA_NUM_GROUPS_MAX) continue;
        if (channel_id < 0 || channel_id >= SOC_GDMA_PAIRS_PER_GROUP_MAX) continue;
        *out_group_id = group_id;
        *out_channel_id = channel_id;
        *out_slot = i;
        return (gdma_channel_handle_t)cand;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 5x7 font for the "FLASH MEMORY WRITE" overlay
// ---------------------------------------------------------------------------
// Minimal 5x7 font covering only the characters needed for the overlay.
// Each character is 7 bytes (one per row), with pixel data in bits 7..3 (MSB=left).
// Rendered at 2x scale (10x14 per char) centered on the screen.
static const uint8_t s_charA[] = {0x70,0x88,0x88,0xF8,0x88,0x88,0x88};
static const uint8_t s_charE[] = {0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8};
static const uint8_t s_charF[] = {0xF8,0x80,0x80,0xF0,0x80,0x80,0x80};
static const uint8_t s_charH[] = {0x88,0x88,0x88,0xF8,0x88,0x88,0x88};
static const uint8_t s_charI[] = {0x70,0x20,0x20,0x20,0x20,0x20,0x70};
static const uint8_t s_charL[] = {0x80,0x80,0x80,0x80,0x80,0x80,0xF8};
static const uint8_t s_charM[] = {0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88};
static const uint8_t s_charO[] = {0x70,0x88,0x88,0x88,0x88,0x88,0x70};
static const uint8_t s_charR[] = {0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88};
static const uint8_t s_charS[] = {0x70,0x88,0x80,0x70,0x08,0x88,0x70};
static const uint8_t s_charT[] = {0xF8,0x20,0x20,0x20,0x20,0x20,0x20};
static const uint8_t s_charW[] = {0x88,0x88,0x88,0xA8,0xA8,0xD8,0x88};
static const uint8_t s_charY[] = {0x88,0x88,0x50,0x20,0x20,0x20,0x20};
static const uint8_t s_charSP[]= {0x00,0x00,0x00,0x00,0x00,0x00,0x00};

// "FLASH MEMORY WRITE" = 18 characters (including spaces)
// Used by beginFlashMessage() / endFlashMessage() to show an overlay during flash ops.
static const uint8_t* const s_flashWriteMsg[] = {
    s_charF,s_charL,s_charA,s_charS,s_charH,s_charSP,
    s_charM,s_charE,s_charM,s_charO,s_charR,s_charY,s_charSP,
    s_charW,s_charR,s_charI,s_charT,s_charE
};
static const int s_flashWriteMsgLen = sizeof(s_flashWriteMsg) / sizeof(s_flashWriteMsg[0]);

// Pre-render a font message into a 1bpp bitmap allocated in SRAM.
// Returns the buffer (caller must free), and fills in width, height, stride.
// 1bpp format: MSB first, rows padded to byte boundary.
// scale: pixel scale factor (2 = 10x14 per character).
// NOT IRAM_ATTR -- runs from flash at call time, not from ISR.
static uint8_t* renderMessageTo1bpp(const uint8_t* const* msg, int msgLen,
                                     int scale, int* outW, int* outH, int* outStride) {
    int charW = 5 * scale;
    int charH = 7 * scale;
    int gap = scale;
    int totalW = msgLen * (charW + gap) - gap;
    int totalH = charH;
    int stride = (totalW + 7) / 8;  // bytes per row, rounded up
    *outW = totalW;
    *outH = totalH;
    *outStride = stride;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(stride * totalH,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return nullptr;
    memset(buf, 0, stride * totalH);
    for (int y = 0; y < totalH; y++) {
        int src_row = y / scale;
        for (int x = 0; x < totalW; x++) {
            int char_cell = x / (charW + gap);
            int in_cell_x = x % (charW + gap);
            if (char_cell < msgLen && in_cell_x < charW) {
                int src_col = in_cell_x / scale;
                uint8_t row_bits = msg[char_cell][src_row];
                if (row_bits & (0x80 >> src_col)) {
                    buf[y * stride + x / 8] |= (0x80 >> (x % 8));
                }
            }
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
// ISR -> fill-task hand-off
// ---------------------------------------------------------------------------
// The GDMA "bounce buffer empty" callback runs in ISR context.  To keep that
// ISR short (so it does not block other interrupts on its core), it does only
// the cheap, time-critical work -- frame-start latching, desync detection and
// the flash-overlay fill -- and hands the heavy PSRAM->bounce-buffer copy to a
// dedicated high-priority task via this request queue.  Each request snapshots
// the per-frame state (which framebuffer, which scroll offset) so the copy is
// consistent even if the task runs a little behind.
struct BounceFillRequest {
    void*     bounce_buf;     // target bounce buffer (already desync-redirected)
    int       pos_px;         // linear screen pixel position this chunk starts at
    int       len_bytes;      // bytes to fill
    uint16_t* framebuffer;    // active framebuffer to copy from (non-null)
    uint32_t  scroll_offset;  // scroll offset latched for this frame
    uint32_t  isr_cycles;     // CPU cycle count when the bounce interrupt fired
    uint8_t   kind;           // 0 = fill (from the ISR); 1 = row-cache harvest
                              //   (from a DMA completion; bounce_buf = record);
                              //   2 = fill-ahead step (from the timer);
                              //   3 = raster-band chunk (the app draws it)
    uint32_t  frame;          // frameCount() of the frame this chunk belongs to
    ESP32S3_Touch_LCD_7_7B::RasterFn raster_fn;   // kind 3: the band latched for this frame
    void*     raster_ctx;
};

// Bounce-buffer pointer-validation / desync redirect workaround.
//   0 (default) = trust the bounce_buf pointer the ESP-IDF RGB driver hands us.
//   1           = detect the driver's ping-pong phase inversion and redirect the
//                 fill to the idle buffer (see extras/DESYNC_WORKAROUND.md for the
//                 full technique and history -- it took real effort to get right).
// Disabled by default: it was turned off when the old fixed-offset read of the
// private esp_rgb_panel_t returned a null dma_chan on IDF 5.5.4.  The channel is
// now found by findGdmaChannel()'s scan instead, but that has not yet been
// re-verified on this panel.  The artifact it guards against is obvious on-screen
// (streaking that races the scan, typically after flash writes), so flip this
// back on per extras/DESYNC_WORKAROUND.md if it ever returns.
#ifndef LCD7_7B_BOUNCE_DESYNC_WORKAROUND
#define LCD7_7B_BOUNCE_DESYNC_WORKAROUND 0
#endif

// Where the PSRAM->bounce-buffer copy runs.
//   1 (default) = deferred to a dedicated high-priority task, so the GDMA ISR
//                 stays short (it only hands off the request) and doesn't block
//                 other interrupts on its core.  This is the normal mode.
//   0           = inline in the GDMA ISR, like the 2.8C driver -- runs the copy
//                 synchronously at the bounce-empty moment.  Also used as the
//                 automatic fallback if the task/queue can't be created.
// (Watch g_bbDroppedCount in task mode: a non-zero, climbing value means the
// fill task isn't keeping up with the refresh.)
#ifndef LCD7_7B_BOUNCE_FILL_IN_TASK
#define LCD7_7B_BOUNCE_FILL_IN_TASK 1
#endif

// History: an early GDMA offload of this copy (on the -7, through
// esp_async_memcpy) dropped ~9% of chunks and was removed.  The GDMA fill is
// now the default again, and keeps its deadline: it has ~2 bounce periods of
// slack (it writes faster than the panel reads), drives its own GDMA channel
// pair without esp_async_memcpy's per-call cache write-back (m2m* above), and
// the row cache cuts what it moves.  See README-engineering.md, "The fill".

// The bounce-fill task runs at a very high priority, pinned to the core that
// owns the RGB panel, so it can meet the per-bounce-buffer deadline (it must
// finish before the DMA wraps back to the buffer it is filling).  Lower it if it
// starves other work on that core.
#define LCD7_7B_BOUNCE_TASK_PRIORITY (configMAX_PRIORITIES - 1)
// Depth of the ISR->task hand-off queue.  There are only two bounce buffers, so
// a few slots absorb scheduling jitter; an overflow (counted in g_bbDroppedCount)
// means the task could not keep up with the refresh.
#define LCD7_7B_BOUNCE_QUEUE_LEN 8

// Which core runs the bounce-fill task.
//   0 (default) = the same core as begin() (and the GDMA ISR), so the ISR->task
//                 hand-off is a same-core wakeup with the least latency.  That
//                 core (typically the Arduino APP CPU, core 1) is also running
//                 LVGL / loop(), so the copy shares it with the UI work -- which
//                 is fine: the CPU copy meets the ~1 ms deadline with margin.
//   1           = the OTHER core, moving the per-frame copy off the UI core.
//                 Costs a cross-core wakeup (a few microseconds, well within the
//                 deadline) but lets LVGL keep the begin() core to itself.
//
//                 *** WiFi / Bluetooth WARNING ***  The "other" core is normally
//                 the PRO CPU (core 0), where the WiFi driver and the Bluetooth
//                 controller live.  This fill task is configMAX_PRIORITIES-1
//                 (above the WiFi task) and uses ~45% of a core (~936 wakeups/s),
//                 so pinning it to core 0 will degrade WiFi throughput/latency and
//                 can break Bluetooth's hard real-time radio timing (glitches /
//                 drops).  Only enable this if the radio is unused -- or lower
//                 LCD7_7B_BOUNCE_TASK_PRIORITY below the radio tasks (accepting some
//                 drop risk) if you truly need both.
//
// NOTE: this macro is now only the COMPILE-TIME DEFAULT.  Prefer selecting the
// mode at runtime with display.setBounceFillMode(BOUNCE_FILL_OTHER_CORE) before
// begin(), which a sketch can do without editing this shared driver.
#ifndef LCD7_7B_BOUNCE_FILL_OTHER_CORE
#define LCD7_7B_BOUNCE_FILL_OTHER_CORE 0
#endif

// Per-core idle share for LVGL's performance monitor (see the header).
static uint32_t idlePercent(int core) {
    static uint32_t lastIdle[2], lastUs[2];
    uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);   // esp_timer microseconds
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t dIdle = idle - lastIdle[core], dt = now - lastUs[core];
    lastIdle[core] = idle;
    lastUs[core] = now;
    if (!dt) return 100;
    uint32_t pct = (uint32_t)((uint64_t)dIdle * 100 / dt);
    return pct > 100 ? 100 : pct;
}
extern "C" uint32_t cglcd7_7b_idle_percent_core0(void) { return idlePercent(0); }
extern "C" uint32_t cglcd7_7b_idle_percent_core1(void) { return idlePercent(1); }

// Guards the bounce interrupt's snapshot for the fill-ahead (see _prefillStep).
static portMUX_TYPE s_pfMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_bbPrefillRows = 0;    // rows filled ahead of their interrupt
volatile uint32_t g_bbRemainderRows = 0;  // rows still left when the interrupt came
volatile uint32_t g_bbCpuCopyRows = 0;    // rows copied by the CPU (no GDMA)

// Fill-ahead timer: a few ticks per bounce period (~924 us on the -7B at 30 MHz, ~1171 us on the -7 at 14 MHz), each
// asking the fill task for one step unless it already has work queued.
#ifndef LCD7_7B_PREFILL_TICK_US
#define LCD7_7B_PREFILL_TICK_US 300
#endif
bool IRAM_ATTR cglcd7_7b_prefillTick(ESP32S3_Touch_LCD_7_7B* d) {
    bool on = d->_fillAhead < 0 ? (d->_dma == nullptr) : d->_fillAhead;
    if (!d->_rle || !on || !d->_bounceQueue) return false;
    if (uxQueueMessagesWaitingFromISR(d->_bounceQueue) != 0) return false;
    BounceFillRequest r = {};
    r.kind = 2;
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(d->_bounceQueue, &r, &hpw);
    return hpw == pdTRUE;
}
static bool IRAM_ATTR prefillTimerCb(gptimer_handle_t, const gptimer_alarm_event_data_t*, void* arg) {
    return cglcd7_7b_prefillTick((ESP32S3_Touch_LCD_7_7B*)arg);
}

// Frame clock, advanced by the ISR at each frame start: whole milliseconds, and
// the cycles (at 240 MHz) not yet making up one.
static volatile uint32_t s_frameMs = 0, s_frameRemCycles = 0, s_frameCount = 0;
static volatile uint32_t s_framePeriodCycles = 0;
uint32_t ESP32S3_Touch_LCD_7_7B::frameTickMs() const { return s_frameMs; }
uint32_t ESP32S3_Touch_LCD_7_7B::frameCount() const { return s_frameCount; }
uint32_t ESP32S3_Touch_LCD_7_7B::framePeriodUs() const { return s_framePeriodCycles / 240; }

// Bounce buffer callback - runs in ISR context (GDMA "bounce empty" interrupt).
// Does the cheap, time-critical work inline and defers the PSRAM copy to the
// high-priority fill task.  See the block comment above.
bool IRAM_ATTR bounceBufferFillCallback(esp_lcd_panel_handle_t panel, void *bounce_buf,
                                                int pos_px, int len_bytes, void *user_ctx) {
    uint32_t entry_time = esp_cpu_get_cycle_count();

    // Accumulate time spent outside ISR (since last exit)
    if (g_isrCallCount > 0) {
        g_isrCyclesOut += (entry_time - g_isrLastExit);
    }
    g_isrCallCount=g_isrCallCount+1;
    {
        static uint32_t lastEntry = 0;
        if (lastEntry) g_bbPeriodUs = (entry_time - lastEntry) / 240;
        lastEntry = entry_time;
    }

    ESP32S3_Touch_LCD_7_7B* display = (ESP32S3_Touch_LCD_7_7B*)user_ctx;

#if LCD7_7B_BOUNCE_DESYNC_WORKAROUND
    // --- Begin bounce buffer desync detection and workaround ---
    // (See extras/DESYNC_WORKAROUND.md for the full explanation and history.)
    //
    // This MUST stay in the ISR, not the fill task: it reads the GDMA descriptor
    // register at notification time, which is exactly when the driver's
    // ping-pong phase inversion is detectable.  Deferring it to the task would
    // also mistake mere task lateness (DMA reading our buffer because we are slow)
    // for a phase inversion.  It is cheap -- one MMIO read and a compare -- and
    // touches no PSRAM/flash, so it is safe in the ISR.

    // Learn both bounce buffer addresses from the first two distinct callback invocations
    if (s_bb_addrs[0] == nullptr) {
        s_bb_addrs[0] = bounce_buf;
    } else if (s_bb_addrs[1] == nullptr && bounce_buf != s_bb_addrs[0]) {
        s_bb_addrs[1] = bounce_buf;
    }

    // Read the GDMA out.dscr register to find the descriptor currently being DMA'd,
    // then extract the buffer address from that descriptor (word 1 of the DMA descriptor).
    // If the DMA's active buffer is the same one we've been asked to fill, the driver's
    // ping-pong phase is inverted -- redirect our writes to the other (idle) buffer.
    if (s_gdma_out_dscr_reg != nullptr && s_bb_size > 0) {
        uint32_t dscr_addr = *s_gdma_out_dscr_reg;
        g_bbDiag_dscr = dscr_addr;
        g_bbDiag_bounceBuf = (uint32_t)bounce_buf;
        if (dscr_addr != 0) {
            uint32_t* desc = (uint32_t*)dscr_addr;
            uint32_t dma_buf_addr = desc[1];  // DMA descriptor word 1 = buffer pointer
            g_bbDiag_bufAddr = dma_buf_addr;
            uint32_t bb_start = (uint32_t)bounce_buf;
            uint32_t bb_end = bb_start + s_bb_size;
            if (dma_buf_addr >= bb_start && dma_buf_addr < bb_end) {
                g_bbDesyncCount = g_bbDesyncCount + 1;
                // Redirect writes to the other bounce buffer since this one is in-flight
                if (s_bb_addrs[0] != nullptr && s_bb_addrs[1] != nullptr) {
                    bounce_buf = (bounce_buf == s_bb_addrs[0]) ? s_bb_addrs[1] : s_bb_addrs[0];
                }
            }
        }
    }
    // --- End bounce buffer desync detection and workaround ---
#endif // LCD7_7B_BOUNCE_DESYNC_WORKAROUND

    // At frame start (pos_px == 0), copy pending values to active
    // This provides tear-free scrolling/swapping by only changing at frame boundaries
    if (pos_px == 0) {
        {   // frame clock (see frameTickMs)
            static uint32_t last = 0;
            if (last) {
                s_framePeriodCycles = entry_time - last;
                s_frameRemCycles += entry_time - last;
                uint32_t ms = s_frameMs;
                while (s_frameRemCycles >= 240000) { s_frameRemCycles -= 240000; ms++; }
                s_frameMs = ms;
                s_frameCount = s_frameCount + 1;
            }
            last = entry_time;
        }
        display->_scroll_offset = display->_pending_scroll_offset;
        uint8_t prev_fb = display->_active_fb;
        display->_active_fb = display->_pending_fb;
        if (display->_rle && display->_active_fb != prev_fb) display->_rleApplyFlip(display->_active_fb);
        s_flashMsgActive = s_flashMsgPending;
        display->_rasterActive.y0  = display->_rasterPending.y0;   // raster band
        display->_rasterActive.y1  = display->_rasterPending.y1;
        display->_rasterActive.fn  = display->_rasterPending.fn;
        display->_rasterActive.ctx = display->_rasterPending.ctx;

        // Copy pending sprites to active sprites for this frame
        for (int i = 0; i < SPRITE_MAX_COUNT; i++) {
            display->_active_sprites[i] = display->_pending_sprites[i];
        }
    }

    // Select the active framebuffer (1 = primary, 2 = secondary, 3 = third)
    uint16_t* framebuffer = display->framebufferByIndex(display->_active_fb);
    if (framebuffer == nullptr) framebuffer = display->_framebuffer;

    const int screen_width = display->_width;

    // No framebuffer: begin() fails before the panel is created, so this is only a guard.
    if (framebuffer == nullptr) {
        return false;
    }

    // Flash write overlay: if active, fill bounce buffer with black background
    // and expand the 1bpp SRAM bitmap for message rows that overlap this chunk.
    // Row-by-row copy from SRAM only -- no PSRAM or flash access.
    //
    // This path MUST stay in the ISR (not the fill task): it runs during flash
    // writes, when the cache is disabled and no FreeRTOS task can be scheduled.
    // It only touches SRAM, so the IRAM-safe ISR can execute it throughout the
    // stall -- which is the whole reason the overlay exists.
    const volatile uint8_t* flashBmp = s_flashMsgActive;
    if (flashBmp != nullptr) {
        uint16_t* dst = (uint16_t*)bounce_buf;
        int pixel_count = len_bytes / sizeof(uint16_t);
        int start_y = pos_px / screen_width;
        int num_lines = pixel_count / screen_width;

        // Fill entire bounce buffer black (32-bit fill for speed)
        uint32_t* dst32 = (uint32_t*)dst;
        for (int i = 0; i < pixel_count / 2; i++) {
            dst32[i] = 0;
        }

        // Expand 1bpp message rows that fall within this chunk.
        // If tiling, the message repeats every num_lines lines (bounce buffer height),
        // so every chunk shows it at the same offset -- even if the DMA replays a
        // single chunk during a flash stall, the message is always visible.
        int msgX = s_flashMsgX;
        int msgY = s_flashMsgY;
        int msgW = s_flashMsgW;
        int msgH = s_flashMsgH;
        int stride = s_flashMsgStride;
        bool tile = s_flashMsgTile;
        // Under 180-degree rotation the overlay is flipped so the text reads
        // upright on the inverted panel: each physical line maps to logical line
        // (H-1-scr_y) and each message column to physical x (W-1-(msgX+x)).
        bool rot = display->_rotate180;
        for (int line = 0; line < num_lines; line++) {
            int scr_y = start_y + line;                                  // physical line
            int ly = rot ? (display->_height - 1 - scr_y)    // logical line
                         : scr_y;
            int rel_y;
            if (tile) {
                // Tile: use position within the bounce buffer band, offset by msgY
                // which is the offset within a band (set by beginFlashMessage)
                int band_y = ly % num_lines;
                rel_y = band_y - msgY;
            } else {
                rel_y = ly - msgY;
            }
            if (rel_y < 0 || rel_y >= msgH) continue;
            const volatile uint8_t* src_row = flashBmp + rel_y * stride;
            for (int x = 0; x < msgW; x++) {
                if (src_row[x / 8] & (0x80 >> (x % 8))) {
                    int phys_x = rot ? (screen_width - 1 - (msgX + x)) : (msgX + x);
                    dst[line * screen_width + phys_x] = 0xFFFF;
                }
            }
        }
        return false;
    }

    // Normal operation: hand the heavy PSRAM->bounce-buffer copy (and sprite
    // overlay) to the high-priority fill task.  The desync redirect above has
    // already chosen the target buffer; snapshot the per-frame state so the copy
    // is consistent even if the task runs slightly behind.
    BaseType_t hpw = pdFALSE;
    // Snapshot for the fill-ahead: where the scan is from now on.
    portENTER_CRITICAL_ISR(&s_pfMux);
    display->_isrCycles = entry_time;
    display->_isrBuf = bounce_buf;
    display->_isrPos = pos_px;
    display->_isrFb = framebuffer;
    portEXIT_CRITICAL_ISR(&s_pfMux);
    if (display->_bounceQueue != nullptr) {
        BounceFillRequest req;
        req.bounce_buf    = bounce_buf;
        req.pos_px        = pos_px;
        req.len_bytes     = len_bytes;
        req.framebuffer   = framebuffer;
        req.scroll_offset = display->_scroll_offset;
        req.isr_cycles    = entry_time;
        req.kind          = 0;
        req.frame         = s_frameCount;
        req.raster_fn     = nullptr;
        req.raster_ctx    = nullptr;
        {   // A chunk of the raster band: the app draws it (chunk-aligned band).
            const ESP32S3_Touch_LCD_7_7B::RasterBand& rb = display->_rasterActive;
            if (rb.fn) {
                int y = pos_px / screen_width;
                if (y >= rb.y0 && y < rb.y1) {
                    req.kind = 3;
                    req.raster_fn = rb.fn;
                    req.raster_ctx = rb.ctx;
                }
            }
        }
        if (xQueueSendFromISR(display->_bounceQueue, &req, &hpw) != pdTRUE) {
            // Queue full: the fill task could not keep up.  The buffer keeps its
            // previous contents this pass (a transient glitch); count it.
            g_bbDroppedCount = g_bbDroppedCount + 1;
        }
    } else {
        // Fallback (no fill task -- creation failed): do the copy inline, as the
        // original driver did.  Safe during normal operation (cache enabled).
        display->_fillBounceBuffer(bounce_buf, pos_px, len_bytes, framebuffer,
                                   display->_scroll_offset);
    }

    // Accumulate time spent inside ISR and record exit time
    uint32_t exit_time = esp_cpu_get_cycle_count();
    g_isrCyclesIn += (exit_time - entry_time);
    g_isrLastExit = exit_time;

    // The Arduino core builds IDF with CONFIG_LCD_RGB_RESTART_IN_VSYNC: at each
    // VSYNC the RGB driver restarts the DMA and asks for the first two chunks
    // from its VSYNC interrupt, and that path doesn't act on our return value.
    // The fill task then waited for the next tick (up to 1 ms), and the panel
    // scanned the previous frame's last chunks at the top.  Request the switch
    // here, effective when the interrupt returns.  (Not during
    // esp_lcd_panel_init()'s first fills, which run in a task.)
    if (hpw == pdTRUE && xPortInIsrContext()) portYIELD_FROM_ISR();

    return hpw == pdTRUE;
}

// COLOR_RGB332 mode: byte -> RGB565 expansion table.  Internal RAM, because the
// fill reads it ~600k times a frame.  Default contents are plain RGB332 with bit
// replication, so 0x00 -> black and 0xFF -> white exactly; setPalette() swaps it.
static DRAM_ATTR uint16_t s_lut332[256];
static bool s_lut332Ready = false;

static void initLut332() {
    for (int i = 0; i < 256; i++) {
        uint16_t r3 = i >> 5, g3 = (i >> 2) & 7, b2 = i & 3;
        uint16_t r5 = (r3 << 2) | (r3 >> 1);
        uint16_t g6 = (g3 << 3) | g3;
        uint16_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        s_lut332[i] = (r5 << 11) | (g6 << 5) | b5;
    }
    s_lut332Ready = true;
}

void ESP32S3_Touch_LCD_7_7B::setPalette(const uint16_t* rgb565x256) {
    memcpy(s_lut332, rgb565x256, sizeof(s_lut332));
    s_lut332Ready = true;
}

// The high-priority task that drains the request queue and performs the actual
// PSRAM->bounce-buffer copy + sprite overlay.  Runs in task context (cache
// enabled), so it never executes during a flash write -- those frames are
// handled entirely by the ISR's flash-overlay path above.
// Fork addition (setOverlay): how many bytes of m2m chain `slot` (the value
// of s_m2mHead - 1 just after it was submitted) have landed.  The GDMA writes each inbound descriptor back as it completes it
// (its length, and the owner bit back to the CPU), so the overlay can follow
// the copy row by row instead of waiting for all of it.  An empty ring: all.
static int IRAM_ATTR m2mLanded(uint32_t slot) {
    if ((int32_t)(s_m2mTail - slot) > 0) return INT32_MAX;   // that chain is done
    const M2MXfer* x = &s_m2m[slot % M2M_SLOTS];
    int done = 0;
    for (int k = 0; k < x->nd; k++) {
        const volatile dma_descriptor_t* r = &x->rx[k];
        if (r->dw0.owner != DMA_DESCRIPTOR_BUFFER_OWNER_CPU && r->dw0.length == 0) break;
        done += r->dw0.size;
    }
    return done;
}

// Fork addition (setOverlay): the next chunk's request, taken off the queue
// while the overlay was still on the chunk before, so its copy could start at
// once instead of after that overlay.  s_nextSlot: its m2m chain; -1 none
// taken; -2 taken, but its copy could not be started early.
static BounceFillRequest s_next;
static int32_t s_nextSlot = -1;

// Fork addition (setOverlay's src): a chunk composed beforehand is copied from
// there instead of the framebuffer.  framebuffer + pos_px must land on it.
void ESP32S3_Touch_LCD_7_7B::_ovlSource(BounceFillRequest& req) {
    OverlaySrcFn src = _ovlSrc;
    if (req.kind != 0 || !_ovlFn || !src) return;
    const int W = _width;
    const uint16_t* alt = src(req.pos_px / W, req.len_bytes / (int)sizeof(uint16_t) / W, req.frame, _ovlCtx);
    if (alt) req.framebuffer = (uint16_t*)alt - req.pos_px;
}

void ESP32S3_Touch_LCD_7_7B::_bounceFillTask(void* arg) {
    ESP32S3_Touch_LCD_7_7B* self = (ESP32S3_Touch_LCD_7_7B*)arg;
    BounceFillRequest req;
    for (;;) {
        bool early = false;
        uint32_t slot = 0;
        bool got;
        if (s_nextSlot != -1) {   // fork addition: taken early, see above
            req = s_next;
            early = s_nextSlot >= 0;
            slot = (uint32_t)s_nextSlot;
            s_nextSlot = -1;
            got = true;
        } else {
            got = xQueueReceive(self->_bounceQueue, &req, portMAX_DELAY) == pdTRUE;
            if (got) self->_ovlSource(req);
        }
        if (got) {
            if (req.kind == 1) { self->_rleHarvest(req.bounce_buf); continue; }
            if (req.kind == 2) { self->_prefillStep(); continue; }
            if (req.kind == 3) {
                self->_rasterFill(req.bounce_buf, req.pos_px, req.len_bytes, req.raster_fn,
                                  req.raster_ctx, req.frame, req.isr_cycles);
                continue;
            }
            if (self->_half) {
                self->_fillHalf(req.bounce_buf, req.pos_px, req.len_bytes, req.framebuffer);
                continue;
            }
            RasterFn ovl = self->_ovlFn;   // fork addition: the overlay goes over the copied rows
            if (ovl && self->_ovlWant) {  // nothing over these rows this frame: a plain chunk
                const int W = self->_width;
                if (!self->_ovlWant(req.pos_px / W, req.len_bytes / (int)sizeof(uint16_t) / W, req.frame, self->_ovlCtx)) {
                    if (early) continue;   // its copy is already under way
                    ovl = nullptr;
                }
            }
            if (early && ovl) {
                self->_overlayRows(req, ovl, slot, true);
                continue;
            }
            if (!self->_ovlFn && self->_rle && self->_rleFill(req.bounce_buf, req.pos_px, req.len_bytes,
                                             req.framebuffer, req.scroll_offset, req.isr_cycles))
                continue;
            if (self->_dma && self->_dmaFill(req.bounce_buf, req.pos_px, req.len_bytes,
                                             req.framebuffer, req.scroll_offset, req.isr_cycles)) {
                if (!ovl) continue;
                self->_overlayRows(req, ovl, s_m2mHead - 1, true);   // fork addition
                continue;
            }
            if (self->_dma) g_bbDmaFallbacks = g_bbDmaFallbacks + 1;
            self->_fillBounceBuffer(req.bounce_buf, req.pos_px, req.len_bytes,
                                    req.framebuffer, req.scroll_offset);
            if (ovl) self->_overlayRows(req, ovl, 0, false);   // fork addition
        }
    }
}

// Fork addition (setOverlay): the overlay over a chunk's rows, top to bottom.
// After a DMA copy each row waits (spinning: a row is ~26 us of copy) till the
// copy has passed it, so the overlay follows the DMA down the chunk.  The
// prep callback, if set, runs for each row before that wait.
// Every row has a deadline: the panel starts on the buffer one bounce period
// after its interrupt and scans a row every period / rows.  A row that cannot
// be started before its deadline is left as the copy made it, and the work
// goes on with the rows that still can be on time.  Drawn after its deadline,
// a row would land in the buffer after the panel had passed it, or in the next
// chunk that the copy is already putting there.
#ifndef LCD7_7B_OVL_MARGIN_US
#define LCD7_7B_OVL_MARGIN_US 8
#endif
// Rows prep may run ahead of the row being applied, while the copy is not there
// yet.  The overlay must keep at least this many + 1 prepared rows (ovl.c: 3).
#ifndef LCD7_7B_OVL_PREP_AHEAD
#define LCD7_7B_OVL_PREP_AHEAD 2
#endif
volatile uint32_t g_ovlRowsSkipped = 0;
volatile uint32_t g_ovlSkipQ[4] = {0, 0, 0, 0};   // the same, by quarter of the panel, top first
volatile uint16_t g_ovlSkipChunk[64];             // the same, by chunk (y / bounce rows)
volatile uint32_t g_ovlSkipDma = 0;               // of those, the copy had not reached the row by its deadline
volatile uint32_t g_ovlEarly = 0;                 // copies started while the overlay was still on the chunk before
volatile uint32_t g_ovlRowDeadline = 0;
volatile uint32_t g_ovlPrepCycles = 0, g_ovlSpinCycles = 0;   // the fill's time in prep, and spinning on the copy           // the row fn is called for must be done by this cycle count (0: none)
void ESP32S3_Touch_LCD_7_7B::_overlayRows(const BounceFillRequest& req, RasterFn ovl, uint32_t slot, bool dma) {
    const int W = _width, rows = req.len_bytes / (int)sizeof(uint16_t) / W;
    const int y0 = req.pos_px / W;
    const uint32_t P = g_bbPeriodUs * 240u, isr = req.isr_cycles;
    const bool timed = P && isr;
    uint32_t spent = 0, skipped = 0;
    RasterFn prep = _ovlPrep;
    int prepped = -1;   // the last row prep has been called for
    for (int r = 0; r < rows; r++) {
        const uint32_t deadline = isr + P + (uint32_t)((uint64_t)P * r / rows) - LCD7_7B_OVL_MARGIN_US * 240u;
        if (timed && (int32_t)(esp_cpu_get_cycle_count() - deadline) >= 0) {
            skipped++;
            g_ovlSkipQ[((y0 + r) * 4 / _height) & 3]++;
            g_ovlSkipChunk[(y0 / LCD7_7B_BOUNCE_ROWS) & 63]++;
            continue;
        }
        if (prep && prepped < r) {   // what needs no pixels, while the copy is still on its way
            uint32_t p0 = esp_cpu_get_cycle_count();
            prep(nullptr, y0 + r, 1, req.frame, _ovlCtx);
            uint32_t dp = esp_cpu_get_cycle_count() - p0;
            spent += dp;
            g_ovlPrepCycles = g_ovlPrepCycles + dp;
            prepped = r;
        }
        if (dma) {
            const int need = (r + 1) * W * (int)sizeof(uint16_t);
            uint32_t w0 = esp_cpu_get_cycle_count();
            while (m2mLanded(slot) < need) {
                // the copy is not there yet: prepare the rows after this one meanwhile (at most
                // LCD7_7B_OVL_PREP_AHEAD ahead: the overlay keeps that many prepared rows)
                if (prep && prepped < rows - 1 && prepped < r + LCD7_7B_OVL_PREP_AHEAD) {
                    uint32_t p0 = esp_cpu_get_cycle_count();
                    prep(nullptr, y0 + prepped + 1, 1, req.frame, _ovlCtx);
                    uint32_t dp = esp_cpu_get_cycle_count() - p0;
                    spent += dp;
                    g_ovlPrepCycles = g_ovlPrepCycles + dp;
                    prepped++;
                    continue;
                }
                uint32_t now = esp_cpu_get_cycle_count();
                if (now - w0 > 240u * 2000) break;
                if (timed && (int32_t)(now - deadline) >= 0) { g_ovlSkipDma = g_ovlSkipDma + 1; break; }
            }
            g_ovlSpinCycles = g_ovlSpinCycles + (esp_cpu_get_cycle_count() - w0);
        }
        if (timed && (int32_t)(esp_cpu_get_cycle_count() - deadline) >= 0) {
            skipped++;
            g_ovlSkipQ[((y0 + r) * 4 / _height) & 3]++;
            g_ovlSkipChunk[(y0 / LCD7_7B_BOUNCE_ROWS) & 63]++;
            continue;
        }
        uint32_t c0 = esp_cpu_get_cycle_count();
        g_ovlRowDeadline = timed ? deadline + LCD7_7B_OVL_MARGIN_US * 240u : 0;   // fn may drop its costlier parts to make it
        ovl((uint16_t*)req.bounce_buf + r * W, y0 + r, 1, req.frame, _ovlCtx);
        spent += esp_cpu_get_cycle_count() - c0;
        // the next chunk's interrupt may have come meanwhile: start its copy now, not after this
        BounceFillRequest nx;
        if (_dma && s_nextSlot == -1 && xQueuePeek(_bounceQueue, &nx, 0) == pdTRUE && nx.kind == 0 &&
            xQueueReceive(_bounceQueue, &s_next, 0) == pdTRUE) {
            _ovlSource(s_next);
            s_nextSlot = _dmaFill(s_next.bounce_buf, s_next.pos_px, s_next.len_bytes, s_next.framebuffer,
                                  s_next.scroll_offset, s_next.isr_cycles) ? (int32_t)(s_m2mHead - 1) : -2;
            g_ovlEarly = g_ovlEarly + (s_nextSlot >= 0);
        }
    }
    g_ovlCycles = g_ovlCycles + spent;
    g_ovlChunks = g_ovlChunks + 1;
    if (skipped) {
        g_ovlRowsSkipped = g_ovlRowsSkipped + skipped;
        g_ovlLate = g_ovlLate + 1;
    }
    if (timed) {
        uint32_t us = (esp_cpu_get_cycle_count() - isr) / 240;
        if (us > g_ovlMaxUs) g_ovlMaxUs = us;
    }
}

// BOUNCE_FILL_GDMA completion.  ISR context.  Only the chunk's last segment
// carries the bounce interrupt's cycle count.  The deadline is NOT one bounce
// period: the panel starts reading this buffer one period after the interrupt,
// but the DMA started at the interrupt and writes faster (~77 MB/s) than the
// panel reads (~44 MB/s), so it stays ahead of the scan as long as it finishes
// before the panel does -- about two periods.  Slack is measured against that.
static bool IRAM_ATTR dmaDoneCb(async_memcpy_handle_t, async_memcpy_event_t*, void* arg) {
    uint32_t isr = (uint32_t)(uintptr_t)arg;
    if (!isr || !g_bbPeriodUs) return false;   // no period measured yet (first chunks)
    int32_t elapsedUs = (int32_t)((esp_cpu_get_cycle_count() - isr) / 240);
    if ((uint32_t)elapsedUs > g_bbDmaMaxUs) g_bbDmaMaxUs = elapsedUs;
    int32_t slack = 2 * (int32_t)g_bbPeriodUs - elapsedUs;
    g_bbDmaLastSlackUs = slack;
    if (slack < g_bbDmaMinSlackUs) g_bbDmaMinSlackUs = slack;
    if (slack < 0) g_bbDmaLate = g_bbDmaLate + 1;
    return false;
}

// Queue a chunk's PSRAM -> bounce-buffer copy on GDMA.  Returns false, having
// queued nothing, when the chunk needs the CPU path: 180-degree rotation,
// RGB332, an active sprite, or a segment not 64-byte aligned (a scroll offset).
bool ESP32S3_Touch_LCD_7_7B::_dmaFill(void* bounce_buf, int pos_px, int len_bytes,
                                     uint16_t* framebuffer, uint32_t scroll_offset,
                                     uint32_t isr_cycles) {
    if (_rotate180 || _rgb332) return false;
    for (int i = 0; i < SPRITE_MAX_COUNT; i++)
        if (_active_sprites[i].yx != SPRITE_DISABLED && _active_sprites[i].data) return false;

    struct Seg { uint8_t* d; uint8_t* s; size_t n; } segs[32];
    int ns = 0;
    const int W = _width;
    int pixel_count = len_bytes / sizeof(uint16_t);
    int y = pos_px / W, x = pos_px % W;
    uint8_t* dst = (uint8_t*)bounce_buf;
    while (pixel_count > 0) {
        uint32_t fb_pos = (scroll_offset + y * _fb_stride + x) % _fb_size;
        int seg = W - x;
        if ((int)(_fb_size - fb_pos) < seg) seg = _fb_size - fb_pos;
        if (pixel_count < seg) seg = pixel_count;
        uint8_t* src = (uint8_t*)(framebuffer + fb_pos);
        size_t n = seg * sizeof(uint16_t);
        // GDMA's strict rule is on the PSRAM side (address and length a whole
        // number of 64-byte bursts); internal SRAM needs only word alignment, and
        // IDF allocates one of the two bounce buffers only 4-byte aligned.
        if ((((uintptr_t)src | n) & 63) || ((uintptr_t)dst & 3) || ns == 32) return false;
        segs[ns++] = {dst, src, n};
        dst += n;
        pixel_count -= seg;
        x += seg;
        if (x >= W) { x = 0; y++; }
    }
    // Merge contiguous segments (the usual case: whole rows, no padding).
    int m = 0;
    for (int i = 1; i < ns; i++) {
        if (segs[m].s + segs[m].n == segs[i].s && segs[m].d + segs[m].n == segs[i].d) segs[m].n += segs[i].n;
        else segs[++m] = segs[i];
    }
    ns = m + 1;
    if (!m2mSubmit((const M2MSeg*)segs, ns, dmaDoneCb, (void*)(uintptr_t)(isr_cycles ? isr_cycles : 1)))
        return false;
    g_bbDmaChunks = g_bbDmaChunks + 1;
    return true;
}

// ---------------------------------------------------------------------------
// Row cache: run-length-encoded scanlines (see setRowCache in the header).
//
// Each row has a fixed PSRAM slot: a 4-byte header (op-list bytes, words its
// COPY ops fetch), then the op list: a 16-bit header per op, bit 15 set
// for RUN (followed by one RGB565 colour), clear for COPY (fetch that many
// pixels from the framebuffer at the current x).  Runs shorter than
// LCD7_7B_RLE_MINRUN are folded into COPY spans.
// ---------------------------------------------------------------------------
#ifndef LCD7_7B_RLE_SLOT
#define LCD7_7B_RLE_SLOT 256         // bytes of op list per row
#endif
#ifndef LCD7_7B_RLE_MINRUN
#define LCD7_7B_RLE_MINRUN 8         // shorter runs become part of a COPY
#endif
#ifndef LCD7_7B_RLE_MAX_READ
#define LCD7_7B_RLE_MAX_READ W       // ops + copied pixels above this: raw row
#endif                               // (half a row's bytes; W = row width in px)
// ROW_SETTLING: changed recently; seen unchanged by one harvest.  Only a row
// that is still unchanged at its next harvest gets encoded, so rows that
// animate every frame stay on the DMA path instead of being re-encoded.
enum : uint8_t { ROW_DIRTY = 0, ROW_RLE = 1, ROW_RAW = 2, ROW_SETTLING = 3 };
static inline uint8_t rowState(uint8_t v) { return v >> 6; }
static inline uint8_t rowGen(uint8_t v)   { return v & 0x3F; }
static inline uint8_t rowDirtied(uint8_t v) { return (uint8_t)((v + 1) & 0x3F); }   // state DIRTY, gen+1

volatile uint32_t g_flipWriteBackMaxUs = 0;   // longest flip write-back (other core stalled)
volatile uint32_t g_rleRowsExpanded = 0, g_rleRowsDma = 0, g_rleEncoded = 0;
volatile uint32_t g_rleRaw = 0, g_rleHarvestYields = 0, g_rlePsramBytes = 0;
volatile uint32_t g_rleEncodeCycles = 0, g_rleExpandCycles = 0;

static portMUX_TYPE s_rowMux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_fillQueue = nullptr;   // for the DMA callback's harvest notices

// One record per chunk filled through the row cache.  A small ring, so a DMA
// completion that arrives after its buffer was refilled can be recognised (the
// buffer's current record id no longer matches) and its harvest skipped.
struct RleRecord {
    ESP32S3_Touch_LCD_7_7B* self;
    void*    bb;           // bounce buffer filled
    int      bbIdx;        // 0 or 1
    uint32_t id;
    int      row0, nrows;
    uint32_t isr_cycles;
    uint32_t dirtyMask;    // rows DMA'd while DIRTY: harvest candidates
    uint8_t  gen[32];      // their generation at fill time
};
static RleRecord s_rleRec[8];
static uint32_t  s_rleNextRec = 0;
static uint32_t  s_rleNextId = 1;   // content ids: one per chunk put in a bounce buffer

struct RowCacheAccess {   // friend of the display class, for the free functions
    static uint32_t& recId(ESP32S3_Touch_LCD_7_7B* d, int i) { return d->_bbRecId[i]; }
};

// Encode one row (SRAM) into out[cap], working in 32-bit pixel PAIRS: rows
// start word-aligned, so every run and span is whole words and expansion never
// splits one (it also catches 2-pixel patterns such as dithering).  Op header:
// bit 15 set = RUN of n words followed by the 32-bit pair; clear = COPY n words
// from the framebuffer.  Returns op bytes, or -1 if the row isn't worth it;
// *copyW gets the words its COPY ops will fetch.
static int rleEncode(const uint16_t* px, int W, uint8_t* out, int cap, int* copyW) {
    const uint32_t* w = (const uint32_t*)px;
    const int N = W / 2;
    const int minRun = (LCD7_7B_RLE_MINRUN + 1) / 2;   // in words
    int o = 0, cs = -1, cw = 0, i = 0;
    while (i < N) {
        uint32_t c = w[i];
        int j = i + 1;
        while (j < N && w[j] == c) j++;
        if (j - i >= minRun) {
            if (cs >= 0) {
                if (o + 2 > cap) return -1;
                *(uint16_t*)(out + o) = (uint16_t)(i - cs); o += 2; cw += i - cs; cs = -1;
            }
            if (o + 6 > cap) return -1;
            *(uint16_t*)(out + o) = (uint16_t)(0x8000 | (j - i));
            memcpy(out + o + 2, &c, 4);          // op stream is only 2-byte aligned
            o += 6;
        } else if (cs < 0) {
            cs = i;
        }
        i = j;
    }
    if (cs >= 0) {
        if (o + 2 > cap) return -1;
        *(uint16_t*)(out + o) = (uint16_t)(N - cs); o += 2; cw += N - cs;
    }
    if (o + cw * 4 > LCD7_7B_RLE_MAX_READ) return -1;
    *copyW = cw;
    return o;
}

// Fill n words with one 32-bit pair.  Long runs use the S3's 128-bit vector
// store once 16-byte aligned (SRAM write bandwidth is then the limit, ~0.7
// cycles/pixel); short ones an unrolled word loop.
static void IRAM_ATTR fillWords(uint32_t* d, uint32_t c, int n) {
    if (n >= 16) {
        while ((uintptr_t)d & 15) { *d++ = c; n--; }
        int blocks = n >> 2;
        static uint32_t cbuf[4] __attribute__((aligned(16)));
        cbuf[0] = cbuf[1] = cbuf[2] = cbuf[3] = c;
        uint32_t* src = cbuf;
        __asm__ volatile(
            "ee.vld.128.ip q0, %[src], 0\n"
            "1:\n"
            "ee.vst.128.ip q0, %[dst], 16\n"
            "addi %[cnt], %[cnt], -1\n"
            "bnez %[cnt], 1b\n"
            : [dst] "+r"(d), [cnt] "+r"(blocks), [src] "+r"(src)
            :
            : "memory");
        n &= 3;
    }
    for (; n >= 4; n -= 4) { d[0] = c; d[1] = c; d[2] = c; d[3] = c; d += 4; }
    while (n--) *d++ = c;
}

static void IRAM_ATTR rleExpand(uint16_t* dst, const uint8_t* ops, int opBytes, const uint16_t* fbrow) {
    uint32_t* d = (uint32_t*)dst;
    const uint32_t* f = (const uint32_t*)fbrow;
    int x = 0;   // in words
    const uint8_t* end = ops + opBytes;
    while (ops < end) {
        uint16_t h = *(const uint16_t*)ops; ops += 2;
        int n = h & 0x7FFF;
        if (h & 0x8000) {
            uint32_t c = (uint32_t)((const uint16_t*)ops)[0] | ((uint32_t)((const uint16_t*)ops)[1] << 16);
            ops += 4;
            fillWords(d + x, c, n);
        } else {
            memcpy(d + x, f + x, n * 4);
        }
        x += n;
    }
}

// DMA completion for a row-cache chunk: slack bookkeeping as dmaDoneCb, then a
// harvest notice to the fill task if the chunk had dirty rows to encode.
static bool IRAM_ATTR rleDoneCb(async_memcpy_handle_t, async_memcpy_event_t*, void* arg) {
    RleRecord* r = (RleRecord*)arg;
    // No slack books before a period is measured (first chunks) or for a
    // fill-ahead piece, which has no deadline of its own.
    if (g_bbPeriodUs && r->isr_cycles) {
        int32_t elapsedUs = (int32_t)((esp_cpu_get_cycle_count() - r->isr_cycles) / 240);
        if ((uint32_t)elapsedUs > g_bbDmaMaxUs) g_bbDmaMaxUs = elapsedUs;
        int32_t slack = 2 * (int32_t)g_bbPeriodUs - elapsedUs;
        g_bbDmaLastSlackUs = slack;
        if (slack < g_bbDmaMinSlackUs) g_bbDmaMinSlackUs = slack;
        if (slack < 0) g_bbDmaLate = g_bbDmaLate + 1;
    }
    BaseType_t hpw = pdFALSE;
    if (r->dirtyMask && s_fillQueue) {
        BounceFillRequest h = {};
        h.bounce_buf = r;
        h.kind = 1;
        xQueueSendFromISR(s_fillQueue, &h, &hpw);   // dropped if full: rows stay dirty
    }
    return hpw == pdTRUE;
}

bool ESP32S3_Touch_LCD_7_7B::setRowCache(bool on) {
    if (!on) { _rle = false; return true; }
    // GDMA or the CPU fill on the display core (rows copied by the CPU).
    if (_rotate180 || _rgb332 || _bounceFillMode == BOUNCE_FILL_OTHER_CORE) return false;
    if (!_rleSlots) {
        _rleSlots = (uint8_t*)heap_caps_malloc((size_t)_height * LCD7_7B_RLE_SLOT, MALLOC_CAP_SPIRAM);
        if (!_rleSlots) return false;
    }
    portENTER_CRITICAL(&s_rowMux);
    memset(_rowNextDirty, 0, sizeof(_rowNextDirty));
    for (int r = 0; r < _height; r++) _row[r] = rowDirtied(_row[r]);
    portEXIT_CRITICAL(&s_rowMux);
    s_fillQueue = _bounceQueue;
    _rle = true;
    return true;
}

void ESP32S3_Touch_LCD_7_7B::markRowsChanged(int y1, int y2, uint8_t fbIndex) {
    if (!_rle) return;
    if (y1 < 0) y1 = 0;
    if (y2 >= _height) y2 = _height - 1;
    if (fbIndex > 3) fbIndex = 0;
    uint32_t* m = _rowNextDirty[fbIndex];
    portENTER_CRITICAL(&s_rowMux);
    for (int r = y1; r <= y2; r++) m[r >> 5] |= 1u << (r & 31);
    portEXIT_CRITICAL(&s_rowMux);
}

void ESP32S3_Touch_LCD_7_7B::_rleMarkNow(int y1, int y2) {
    if (y1 < 0) y1 = 0;
    if (y2 >= _height) y2 = _height - 1;
    portENTER_CRITICAL(&s_rowMux);
    for (int r = y1; r <= y2; r++) {
        _row[r] = rowDirtied(_row[r]);
        _rowNextDirty[0][r >> 5] |= 1u << (r & 31);   // and in the other buffers at the flip
    }
    portEXIT_CRITICAL(&s_rowMux);
}

void IRAM_ATTR ESP32S3_Touch_LCD_7_7B::_rleApplyFlip(uint8_t fb) {
    if (fb > 3) fb = 0;
    portENTER_CRITICAL_ISR(&s_rowMux);
    for (int w = 0; w < (_height + 31) / 32; w++) {
        uint32_t m = _rowNextDirty[0][w] | _rowNextDirty[fb][w];
        _rowNextDirty[0][w] = 0;
        _rowNextDirty[fb][w] = 0;
        while (m) {
            int b = __builtin_ctz(m);
            m &= m - 1;
            int r = w * 32 + b;
            _row[r] = rowDirtied(_row[r]);
        }
    }
    portEXIT_CRITICAL_ISR(&s_rowMux);
}

// Fill a chunk through the row cache: copy the rows without an op list (GDMA,
// or the CPU when there is none), expand the cached ones on the CPU.  Returns
// false (nothing done) when the chunk needs another path.
//
// prefill = true is a fill-ahead piece (see _prefillStep): rows up to rowTo of
// the chunk this buffer will hold next, written while the panel is still
// scanning the buffer's later rows.  The interrupt's own fill then starts
// where the pre-fill stopped.
bool ESP32S3_Touch_LCD_7_7B::_rleFill(void* bounce_buf, int pos_px, int len_bytes,
                                     uint16_t* framebuffer, uint32_t scroll_offset,
                                     uint32_t isr_cycles, int rowTo, bool prefill) {
    const int W = _width;
    const int npx = len_bytes / (int)sizeof(uint16_t);
    if (scroll_offset || _fb_stride != W || _rotate180 || _rgb332) return false;
    if (pos_px % W || npx % W || npx / W > 32) return false;
    for (int i = 0; i < SPRITE_MAX_COUNT; i++)
        if (_active_sprites[i].yx != SPRITE_DISABLED && _active_sprites[i].data) return false;

    int idx;
    if (_bbAddr[0] == bounce_buf) idx = 0;
    else if (_bbAddr[1] == bounce_buf) idx = 1;
    else if (prefill) return false;
    else if (!_bbAddr[0]) { _bbAddr[0] = bounce_buf; idx = 0; }
    else if (!_bbAddr[1]) { _bbAddr[1] = bounce_buf; idx = 1; }
    else return false;

    const int row0 = pos_px / W, nrows = npx / W;
    if (_rasterActive.fn && row0 >= _rasterActive.y0 && row0 < _rasterActive.y1)
        return false;   // the app draws this chunk (raster band)
    int rFrom = 0;
    uint32_t cid;
    if (prefill) {
        if (_pfPos[idx] != pos_px) {           // first piece of this chunk
            _pfPos[idx] = pos_px;
            _pfRows[idx] = 0;
            _pfId[idx] = s_rleNextId++;
            _bbRecId[idx] = _pfId[idx];        // its old contents are being replaced
        }
        rFrom = _pfRows[idx];
        if (rowTo > nrows) rowTo = nrows;
        if (rowTo <= rFrom) return true;
        _pfRows[idx] = rowTo;
        cid = _pfId[idx];
        g_bbPrefillRows = g_bbPrefillRows + (rowTo - rFrom);
    } else {
        if (_pfPos[idx] == pos_px) { rFrom = _pfRows[idx]; cid = _pfId[idx]; }
        else cid = s_rleNextId++;
        _pfPos[idx] = -1;
        rowTo = nrows;
        _bbRecId[idx] = cid;
        g_bbRemainderRows = g_bbRemainderRows + (rowTo - rFrom);
    }

    RleRecord* rec = &s_rleRec[s_rleNextRec++ & 7];
    rec->self = this; rec->bb = bounce_buf; rec->bbIdx = idx; rec->id = cid;
    rec->row0 = row0; rec->nrows = nrows;
    rec->isr_cycles = prefill ? 0 : (isr_cycles ? isr_cycles : 1);
    rec->dirtyMask = 0;

    uint16_t* bb = (uint16_t*)bounce_buf;
    uint8_t   st[32];
    struct Seg { int r0, n; } segs[32];
    int ns = 0;
    portENTER_CRITICAL(&s_rowMux);
    for (int r = rFrom; r < rowTo; r++) {
        uint8_t v = _row[row0 + r];
        st[r] = rowState(v);
        if (st[r] == ROW_DIRTY || st[r] == ROW_SETTLING) { rec->dirtyMask |= 1u << r; rec->gen[r] = rowGen(v); }
    }
    portEXIT_CRITICAL(&s_rowMux);
    for (int r = rFrom; r < rowTo; r++) {
        if (st[r] == ROW_RLE) continue;
        if (ns && segs[ns - 1].r0 + segs[ns - 1].n == r) segs[ns - 1].n++;
        else segs[ns++] = {r, 1};
    }

    // Rows without an op list: GDMA first, so it runs while the CPU expands
    // the cached rows; the CPU copies them itself when there is no GDMA.
    uint32_t psram = 0;
    M2MSeg ms[32];
    for (int i = 0; i < ns; i++) {
        ms[i].d = (uint8_t*)(bb + segs[i].r0 * W);
        ms[i].s = (uint8_t*)(framebuffer + (row0 + segs[i].r0) * W);
        ms[i].n = (size_t)segs[i].n * W * sizeof(uint16_t);
        psram += ms[i].n;
    }
    bool viaDma = false;
    if (ns && _dma) {
        viaDma = m2mSubmit(ms, ns, rleDoneCb, rec);
        if (!viaDma) rec->dirtyMask = 0;   // CPU copy below; skip the harvest
    }
    if (ns && !viaDma) {
        for (int i = 0; i < ns; i++) memcpy(ms[i].d, ms[i].s, ms[i].n);
        g_bbCpuCopyRows = g_bbCpuCopyRows + (psram / (W * sizeof(uint16_t)));
    }

    int expanded = 0;
    uint32_t t0 = esp_cpu_get_cycle_count();
    for (int r = rFrom; r < rowTo; r++) {
        if (st[r] != ROW_RLE) continue;
        int y = row0 + r;
        const uint8_t* slot = _rleSlots + (size_t)y * LCD7_7B_RLE_SLOT;
        uint16_t opBytes = ((const uint16_t*)slot)[0], copyW = ((const uint16_t*)slot)[1];
        rleExpand(bb + r * W, slot + 4, opBytes, framebuffer + (size_t)y * W);
        psram += 4 + opBytes + copyW * 4;
        expanded++;
    }
    g_rleExpandCycles = g_rleExpandCycles + (esp_cpu_get_cycle_count() - t0);
    g_rleRowsDma = g_rleRowsDma + ((rowTo - rFrom) - expanded);
    g_rleRowsExpanded = g_rleRowsExpanded + expanded;
    g_rlePsramBytes = g_rlePsramBytes + psram;

    // Without a DMA completion, the rows have landed now: queue their harvest
    // here (it yields to any fill that is waiting).
    if (!viaDma && rec->dirtyMask && _dma == nullptr && _bounceQueue) {
        BounceFillRequest h = {};
        h.bounce_buf = rec;
        h.kind = 1;
        xQueueSend(_bounceQueue, &h, 0);   // dropped if full: rows stay dirty
    }
    if (!viaDma && !prefill && g_bbPeriodUs) {   // no DMA completion: slack books here
        int32_t elapsedUs = (int32_t)((esp_cpu_get_cycle_count() - rec->isr_cycles) / 240);
        int32_t slack = 2 * (int32_t)g_bbPeriodUs - elapsedUs;
        g_bbDmaLastSlackUs = slack;
        if (slack < g_bbDmaMinSlackUs) g_bbDmaMinSlackUs = slack;
        if (slack < 0) g_bbDmaLate = g_bbDmaLate + 1;
    }
    if (!prefill) g_bbDmaChunks = g_bbDmaChunks + 1;
    return true;
}

// ---------------------------------------------------------------------------
// Raster band: rows the app draws straight into the bounce buffer (see
// setRasterRows in the header).
// ---------------------------------------------------------------------------
volatile uint32_t g_rasterChunks = 0, g_rasterLate = 0, g_rasterMaxUs = 0, g_rasterCycles = 0;
volatile uint32_t g_rasterDmaWaits = 0;
volatile uint32_t g_rasterMaxFnUs = 0;
volatile uint32_t g_ovlChunks = 0, g_ovlLate = 0, g_ovlMaxUs = 0, g_ovlCycles = 0;   // setOverlay (fork addition)

bool ESP32S3_Touch_LCD_7_7B::setRasterRows(int y0, int y1, RasterFn fn, void* ctx) {
    const int R = LCD7_7B_BOUNCE_ROWS;
    if (y0 < 0) y0 = 0;
    if (y1 > _height) y1 = _height;
    if (!fn || y0 >= y1) {
        _rasterPending.fn = nullptr;   // the interrupt checks fn first
        _rasterPending.y0 = _rasterPending.y1 = 0;
        return true;
    }
    if (_half || _rgb332 || _rotate180) return false;
    y0 = y0 / R * R;
    y1 = (y1 + R - 1) / R * R;
    // fn last: the interrupt may latch between these stores, and ignores
    // the band while fn is null.
    _rasterPending.fn = nullptr;
    _rasterPending.y0 = y0;
    _rasterPending.y1 = y1;
    _rasterPending.ctx = ctx;
    _rasterPending.fn = fn;
    return true;
}

// One chunk of the raster band, in the fill task.  No PSRAM is read.
void IRAM_ATTR ESP32S3_Touch_LCD_7_7B::_rasterFill(void* bounce_buf, int pos_px, int len_bytes,
                                                 RasterFn fn, void* ctx, uint32_t frame,
                                                 uint32_t isr_cycles) {
    // This buffer's old contents are gone: a row-cache harvest still queued
    // for it must not encode the app's pixels as framebuffer rows.
    if (_rle) {
        for (int i = 0; i < 2; i++) {
            if (_bbAddr[i] == bounce_buf) {
                _bbRecId[i] = s_rleNextId++;
                _pfPos[i] = -1;
            }
        }
    }
    // A DMA copy still queued or running targets a bounce buffer too -- if it
    // is late, possibly this one.  Let it finish first, or it would land on
    // top of the app's rows.  (Bounded: a stuck engine must not hang the fill.)
    if (_dma && s_m2mHead != s_m2mTail) {
        uint32_t w0 = esp_cpu_get_cycle_count();
        while (s_m2mHead != s_m2mTail && esp_cpu_get_cycle_count() - w0 < 240u * 4000) {}
        g_rasterDmaWaits = g_rasterDmaWaits + 1;
    }
    const int W = _width;
    uint32_t t0 = esp_cpu_get_cycle_count();
    fn((uint16_t*)bounce_buf, pos_px / W, len_bytes / (int)sizeof(uint16_t) / W, frame, ctx);
    uint32_t t1 = esp_cpu_get_cycle_count();
    g_rasterCycles = g_rasterCycles + (t1 - t0);
    g_rasterChunks = g_rasterChunks + 1;
    if (!g_bbPeriodUs || !isr_cycles) return;   // the first fills, before the panel runs
    // The panel starts on this buffer one bounce period after its interrupt.
    // (A DMA fill gets about two, because it stays ahead of the scan once
    // started; fn's speed is the app's, so the strict deadline is counted.)
    uint32_t us = (t1 - isr_cycles) / 240;
    uint32_t fnUs = (t1 - t0) / 240;
    if (fnUs > g_rasterMaxFnUs) g_rasterMaxFnUs = fnUs;
    if (us > g_rasterMaxUs) g_rasterMaxUs = us;
    if (us > g_bbPeriodUs) g_rasterLate = g_rasterLate + 1;
    // Keep the pacing figure current: the bulk writers gate on the latest
    // chunk's slack, and this chunk put no load on PSRAM.
    g_bbDmaLastSlackUs = 2 * (int32_t)g_bbPeriodUs - (int32_t)us;
}

// One fill-ahead step (timer -> fill task).  The bounce interrupt for buffer X
// means the panel has just started scanning the other buffer, Y; Y will next
// hold the chunk after X's.  Rows of Y the scan has passed (elapsed time /
// line time, less a margin) can already take that chunk.  Skipped when a fill
// is waiting, across the frame boundary (the flip is latched there), and while
// Y holds the frame's first chunk (vertical blanking precedes its scan, so
// elapsed time says nothing about scan progress).
#ifndef LCD7_7B_PREFILL_MARGIN_ROWS
#define LCD7_7B_PREFILL_MARGIN_ROWS 2
#endif
void ESP32S3_Touch_LCD_7_7B::_prefillStep() {
    bool on = _fillAhead < 0 ? (_dma == nullptr) : _fillAhead;
    if (!_rle || !on || !_rowCycles || s_flashMsgActive) return;
    // With GDMA, only while the engine is idle: a fill-ahead chain queued in
    // front of an interrupt's fill would delay the one with a deadline.
    if (_dma && s_m2mHead != s_m2mTail) return;
    BounceFillRequest peek;
    if (xQueuePeek(_bounceQueue, &peek, 0) == pdTRUE && (peek.kind == 0 || peek.kind == 3)) return;
    portENTER_CRITICAL(&s_pfMux);
    uint32_t isr = _isrCycles;
    void* bbX = _isrBuf;
    int posX = _isrPos;
    uint16_t* fb = _isrFb;
    portEXIT_CRITICAL(&s_pfMux);
    if (!bbX || !fb || !_bbAddr[0] || !_bbAddr[1]) return;
    void* bbY = (bbX == _bbAddr[0]) ? _bbAddr[1] : (bbX == _bbAddr[1]) ? _bbAddr[0] : nullptr;
    if (!bbY) return;
    const int chunkPx = _bounce_buffer_size_px;
    const int nrows = chunkPx / _width;
    if (posX == chunkPx) return;                  // Y is scanning chunk 0: blanking first
    int posY = posX + chunkPx;
    if (posY >= _width * _height) return;         // next frame: not latched yet
    uint32_t elapsed = esp_cpu_get_cycle_count() - isr;
    int scanned = (int)(elapsed / _rowCycles) - LCD7_7B_PREFILL_MARGIN_ROWS;
    if (scanned <= 0) return;
    if (scanned > nrows) scanned = nrows;
    _rleFill(bbY, posY, chunkPx * (int)sizeof(uint16_t), fb, 0, 0, scanned, true);
}

// Encode the dirty rows of a just-landed chunk from its bounce buffer, while
// the panel is scanning it.  Stops as soon as a fill request is waiting.
void ESP32S3_Touch_LCD_7_7B::_rleHarvest(void* recp) {
    RleRecord* rec = (RleRecord*)recp;
    if (!_rle || _bbRecId[rec->bbIdx] != rec->id) return;   // buffer refilled since
    const int W = _width;
    static uint8_t slotBuf[LCD7_7B_RLE_SLOT];   // header + ops, built in SRAM
    for (int r = 0; r < rec->nrows; r++) {
        if (!(rec->dirtyMask & (1u << r))) continue;
        BounceFillRequest peek;
        if (xQueuePeek(_bounceQueue, &peek, 0) == pdTRUE && (peek.kind == 0 || peek.kind == 3)) {
            g_rleHarvestYields = g_rleHarvestYields + 1;
            return;
        }
        if (_bbRecId[rec->bbIdx] != rec->id) return;
        int y = rec->row0 + r;
        {   // first sighting after a change: just note it, encode next time
            portENTER_CRITICAL(&s_rowMux);
            uint8_t v = _row[y];
            bool promote = rowGen(v) == rec->gen[r] && rowState(v) == ROW_DIRTY;
            if (promote) _row[y] = (uint8_t)((ROW_SETTLING << 6) | rowGen(v));
            portEXIT_CRITICAL(&s_rowMux);
            if (promote || rowState(v) != ROW_SETTLING || rowGen(v) != rec->gen[r]) continue;
        }
        uint32_t t0 = esp_cpu_get_cycle_count();
        int copyW = 0;
        int nb = rleEncode((const uint16_t*)rec->bb + r * W, W, slotBuf + 4, sizeof(slotBuf) - 4, &copyW);
        if (nb > 0) {
            ((uint16_t*)slotBuf)[0] = (uint16_t)nb;
            ((uint16_t*)slotBuf)[1] = (uint16_t)copyW;
            memcpy(_rleSlots + (size_t)y * LCD7_7B_RLE_SLOT, slotBuf, 4 + nb);
        }
        portENTER_CRITICAL(&s_rowMux);
        uint8_t v = _row[y];
        if (rowGen(v) == rec->gen[r] && rowState(v) == ROW_SETTLING)
            _row[y] = (uint8_t)(((nb > 0 ? ROW_RLE : ROW_RAW) << 6) | rowGen(v));
        portEXIT_CRITICAL(&s_rowMux);
        if (nb > 0) g_rleEncoded = g_rleEncoded + 1; else g_rleRaw = g_rleRaw + 1;
        g_rleEncodeCycles = g_rleEncodeCycles + (esp_cpu_get_cycle_count() - t0);
    }
}

// Copy one bounce-buffer-worth of pixels from the PSRAM framebuffer, then draw
// any sprites that overlap this chunk.  This is the heavy work; by default it
// runs inline in the GDMA ISR (LCD7_7B_BOUNCE_FILL_IN_TASK==0), or in _bounceFillTask
// when that is enabled.  The caller has already done frame-start latching and the
// no-PSRAM / flash-overlay checks.  Marked IRAM_ATTR so the ISR-inline path runs
// from IRAM (deterministic timing, no i-cache miss) -- harmless for the task path.
void IRAM_ATTR ESP32S3_Touch_LCD_7_7B::_fillBounceBuffer(void* bounce_buf, int pos_px, int len_bytes,
                                            uint16_t* framebuffer, uint32_t scroll_offset) {
    const int screen_width = _width;
    if (_rgb332) {
        _fillBounceBuffer332(bounce_buf, pos_px, len_bytes,
                             (const uint8_t*)framebuffer, scroll_offset);
        _overlaySprites(bounce_buf, pos_px, len_bytes);
        return;
    }

    const uint32_t fb_size = _fb_size;
    const int fb_stride = _fb_stride;

    int pixel_count = len_bytes / sizeof(uint16_t);

    if (!_rotate180) {
        // ---- Normal orientation: read PSRAM forward, write bounce forward ----
        uint16_t* dst = (uint16_t*)bounce_buf;

        // Screen position from linear pos_px
        int screen_y = pos_px / screen_width;
        int screen_x = pos_px % screen_width;

        // Linear framebuffer position: scroll_offset + screen_y * fb_stride + screen_x
        // Each screen row is contiguous (_width pixels), with fb_stride spacing between rows
        while (pixel_count > 0) {
            // Compute linear position in framebuffer
            uint32_t fb_pos = (scroll_offset + screen_y * fb_stride + screen_x) % fb_size;
            uint16_t* src = framebuffer + fb_pos;

            // How many pixels until end of screen row or buffer wrap?
            int pixels_to_row_end = screen_width - screen_x;
            int pixels_to_wrap = fb_size - fb_pos;
            int segment = pixels_to_row_end;
            if (pixels_to_wrap < segment) segment = pixels_to_wrap;
            if (pixel_count < segment) segment = pixel_count;

            // Copy segment using 32-bit operations
            int remaining = segment;

            // Align dst if needed
            if (((uintptr_t)dst & 2) && remaining > 0) {
                *dst++ = *src++;
                remaining--;
            }

            // Align src if needed
            if (((uintptr_t)src & 2) && remaining > 0) {
                *dst++ = *src++;
                remaining--;
            }

            // Now both are 32-bit aligned - fast copy
            uint32_t* src32 = (uint32_t*)src;
            uint32_t* dst32 = (uint32_t*)dst;
            while (remaining >= 8) {
                dst32[0] = src32[0];
                dst32[1] = src32[1];
                dst32[2] = src32[2];
                dst32[3] = src32[3];
                src32 += 4;
                dst32 += 4;
                remaining -= 8;
            }
            while (remaining >= 2) {
                *dst32++ = *src32++;
                remaining -= 2;
            }
            dst = (uint16_t*)dst32;
            if (remaining) {
                *dst++ = *(uint16_t*)src32;
            }

            pixel_count -= segment;
            screen_x += segment;
            if (screen_x >= screen_width) {
                screen_x = 0;
                screen_y++;
            }
        }
    } else {
        // ---- 180-degree rotation ----
        // Physical pixel p must show logical pixel (total-1-p).  Read PSRAM
        // FORWARD from the opposite end of the screen (cache-friendly sequential
        // access -- the whole point), and write the SRAM bounce buffer BACKWARD.
        // Only the cheap SRAM side runs in reverse; PSRAM stays sequential.
        const int total_screen_pixels = screen_width * _height;
        int rotated_pos = total_screen_pixels - pos_px - pixel_count;
        int screen_y = rotated_pos / screen_width;
        int screen_x = rotated_pos % screen_width;

        // dst starts at the end of the bounce buffer and decrements
        uint16_t* dst = (uint16_t*)bounce_buf + pixel_count - 1;

        while (pixel_count > 0) {
            uint32_t fb_pos = (scroll_offset + screen_y * fb_stride + screen_x) % fb_size;
            uint16_t* src = framebuffer + fb_pos;

            // How many pixels until end of screen row or buffer wrap?
            int pixels_to_row_end = screen_width - screen_x;
            int pixels_to_wrap = fb_size - fb_pos;
            int segment = pixels_to_row_end;
            if (pixels_to_wrap < segment) segment = pixels_to_wrap;
            if (pixel_count < segment) segment = pixel_count;

            // Read forward from PSRAM, write backward to SRAM.
            //
            // Fast path: read a 32-bit word (two pixels) forward and store it
            // reversed at the mirrored position.  On little-endian, a word is
            // (src[i+1]<<16 | src[i]); the mirrored destination word must be
            // (src[i]<<16 | src[i+1]) -- i.e. a 16-bit halfword rotate of the
            // word -- so we still read PSRAM forward in 32-bit bursts and halve
            // the load/store count.  Requires src word-aligned AND the
            // destination word base (dst-1) word-aligned (the mirror makes this a
            // dual constraint); otherwise fall back to the scalar loop below.
            int i = 0;
            if ((((uintptr_t)src & 3) == 0) && (((uintptr_t)(dst - 1) & 3) == 0)) {
                const uint32_t* s32 = (const uint32_t*)src;
                uint32_t* d32 = (uint32_t*)(dst - 1);  // base of first mirrored word
                int pairs = segment >> 1;
                for (int p = 0; p < pairs; p++) {
                    uint32_t x = s32[p];
                    d32[-p] = (x >> 16) | (x << 16);   // swap the two pixels
                }
                i = pairs << 1;
                dst -= i;                              // advance past the batched pixels
            }
            // Scalar remainder (and the whole segment when unaligned)
            for (; i < segment; i++) {
                *dst-- = src[i];
            }

            pixel_count -= segment;
            screen_x += segment;
            if (screen_x >= screen_width) {
                screen_x = 0;
                screen_y++;
            }
        }
    }

    // Draw any sprites that overlap this chunk (shared with the DMA fill path).
    _overlaySprites(bounce_buf, pos_px, len_bytes);
}

// COLOR_RGB332 fill: same walk as _fillBounceBuffer (row segments, ring-buffer
// wrap, 180-degree mirroring), but the source is one byte per pixel, expanded
// through s_lut332.  PSRAM is read forward 32 bits (4 pixels) at a time in both
// orientations, which is the whole point: half the bytes of the RGB565 fill.
// ---------------------------------------------------------------------------
// Half resolution: scale a 512x300 framebuffer up 2x into the bounce buffer.
//
// At exactly 2x, bilinear interpolation reduces to averages: output pixel
// (2x, 2y) is source (x, y); (2x+1, 2y) averages source x and x+1; odd rows
// average the even rows above and below.  The right and bottom edges repeat
// the last source pixel.  RGB565 pixels are averaged two at a time in a
// 32-bit word: (a & b) + (((a ^ b) & mask) >> 1), where the mask clears the
// lowest bit of each colour field (and of each pixel), so the halving never
// borrows across fields or pixels.  Rows of the next chunk come straight
// from the source framebuffer, so no second bounce buffer is needed.
// ---------------------------------------------------------------------------
static inline uint32_t IRAM_ATTR avg2px(uint32_t a, uint32_t b) {
    return (a & b) + (((a ^ b) & 0xF7DEF7DEu) >> 1);
}

// One source row (n pixels) -> one output row (2n pixels), interpolated.
// Built at -O2 like LVGL (the core builds at -Os); it runs ~10,000 times a
// second.  Each source word is loaded once, and the right edge is outside the
// loop.
__attribute__((optimize("O2")))
static void IRAM_ATTR doubleRow(uint32_t* __restrict out, const uint16_t* src, int n) {
    const uint32_t* w = (const uint32_t*)src;   // two source pixels per word
    const int nw = n / 2;
    uint32_t a = w[0];
    for (int i = 1; i < nw; i++) {
        uint32_t next = w[i];
        uint32_t m = avg2px(a, (a >> 16) | (next << 16));      // avg(s0,s1) | avg(s1,s2) << 16
        out[0] = (a & 0xFFFFu) | (m << 16);                     // s0, avg(s0,s1)
        out[1] = (a >> 16) | (m & 0xFFFF0000u);                 // s1, avg(s1,s2)
        out += 2;
        a = next;
    }
    uint32_t m = avg2px(a, (a >> 16) | (a & 0xFFFF0000u));      // right edge: s2 = s1
    out[0] = (a & 0xFFFFu) | (m << 16);
    out[1] = (a >> 16) | (m & 0xFFFF0000u);
}

// Average two output rows (n words, a multiple of 4) into a third.
__attribute__((optimize("O2")))
static void IRAM_ATTR avgRows(uint32_t* __restrict out, const uint32_t* a, const uint32_t* b, int n) {
    for (int i = 0; i < n; i += 4) {
        out[i]     = avg2px(a[i],     b[i]);
        out[i + 1] = avg2px(a[i + 1], b[i + 1]);
        out[i + 2] = avg2px(a[i + 2], b[i + 2]);
        out[i + 3] = avg2px(a[i + 3], b[i + 3]);
    }
}

// ---- The same two steps with the S3's 128-bit SIMD (PIE) instructions ----
// Measured: doubling 0.98 vs 5.04 cycles per output pixel, averaging 1.42 vs
// 8.55, bit-identical to the C versions on random data.  Notes, from probing
// the instructions:
//   - ee.vsr.32 shifts arithmetically, so the halving is done with
//     ee.vmul.u16 by 1 with SAR = 1 instead: an unsigned per-lane shift.
//   - ee.vadds.s16 saturates as signed, but a correct average can pass
//     0x7FFF; flipping each lane's top bit before and after the add makes the
//     signed add exact.
//   - ee.src.q with SAR_BYTE = 2 gives each pixel's right-hand neighbour;
//     ee.vzip.16 interleaves pixels and averages into output order.
//   - The loops are hardware loops (loopgtz), so these must not be inlined
//     into a loop the compiler has made a hardware loop itself.
// They need 16-byte alignment; the C versions cover anything else.
static const uint16_t DRAM_ATTR kPieMask[8] __attribute__((aligned(16))) =
    {0xF7DE, 0xF7DE, 0xF7DE, 0xF7DE, 0xF7DE, 0xF7DE, 0xF7DE, 0xF7DE};
static const uint16_t DRAM_ATTR kPieSign[8] __attribute__((aligned(16))) =
    {0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000};
static const uint16_t DRAM_ATTR kPieOnes[8] __attribute__((aligned(16))) = {1, 1, 1, 1, 1, 1, 1, 1};

// q5 = ones, q6 = field mask, q7 = sign flip; SAR = 1.
static inline void IRAM_ATTR pieConsts() {
    const uint16_t *m = kPieMask, *s = kPieSign, *o = kPieOnes;
    asm volatile("ee.vld.128.ip q6, %0, 0\n ee.vld.128.ip q7, %1, 0\n ee.vld.128.ip q5, %2, 0\n ssai 1\n"
                 : "+r"(m), "+r"(s), "+r"(o) :: "memory");
}
// QOUT = average of QA and QB (RGB565 per lane); q4 is scratch.
#define PIE_AVG(QA, QB, QOUT) \
    "ee.andq " QOUT ", " QA ", " QB "\n" \
    "ee.xorq q4, " QA ", " QB "\n" \
    "ee.andq q4, q4, q6\n" \
    "ee.vmul.u16 q4, q4, q5\n" \
    "ee.xorq " QOUT ", " QOUT ", q7\n" \
    "ee.vadds.s16 " QOUT ", " QOUT ", q4\n" \
    "ee.xorq " QOUT ", " QOUT ", q7\n"

// doubleRow for an output row at any 4-byte offset (the IDF allocates the
// bounce buffers 4-byte aligned: measured 8 and 12 bytes off).  Output word j
// is source pixel j and its average with pixel j+1.  The first words, up to
// 16-byte alignment, and the last ones (the right edge) are done in C.  The
// SIMD core loads aligned source vectors and pulls out both the pixels and
// their right-hand neighbours with ee.src.q, re-setting SAR_BYTE between the
// two.  Reads up to 32 bytes past the source row.  Measured 1.38 cycles per
// output pixel at a 12-byte offset (C: 5.04), identical results.
static inline uint32_t IRAM_ATTR dblWord(const uint16_t* s, int j, int n) {
    uint32_t a = s[j], b = (j + 1 < n) ? s[j + 1] : s[j];
    return a | ((avg2px(a, b) << 16) & 0xFFFF0000u);
}
__attribute__((noinline))
static void IRAM_ATTR doubleRowPIE(uint32_t* out, const uint16_t* src, int n) {
    int h = ((16 - ((uintptr_t)out & 15)) & 15) / 4;   // C words up to alignment
    for (int j = 0; j < h && j < n; j++) out[j] = dblWord(src, j, n);
    int iters = (n - h) / 8;
    if ((n - h) % 8 == 0 && iters) iters--;            // leave the right edge to C
    const int simdEnd = h + 8 * iters;
    const uint16_t* s = src;                           // aligned; pixel h is 2h bytes in
    uint32_t* o = out + h;
    int sb0 = 2 * h, sb1 = 2 * h + 2;
    pieConsts();
    asm volatile(
        "ee.vld.128.ip q0, %0, 16\n"
        "loopgtz %2, 1f\n"
        "ee.vld.128.ip q1, %0, 16\n"
        "wur.sar_byte %3\n"
        "ee.src.q q2, q0, q1\n"                        // pixels h..h+7
        "wur.sar_byte %4\n"
        "ee.src.q q3, q0, q1\n"                        // their right-hand neighbours
        PIE_AVG("q2", "q3", "q0")
        "ee.vzip.16 q2, q0\n"
        "ee.vst.128.ip q2, %1, 16\n"
        "ee.vst.128.ip q0, %1, 16\n"
        "ee.orq q0, q1, q1\n"
        "1:\n"
        : "+r"(s), "+r"(o), "+r"(iters) : "r"(sb0), "r"(sb1) : "memory");
    for (int j = simdEnd > h ? simdEnd : h; j < n; j++) out[j] = dblWord(src, j, n);
}

// avgRows for n words (a multiple of 4).
__attribute__((noinline))
static void IRAM_ATTR avgRowsPIE(uint32_t* out, const uint32_t* a, const uint32_t* b, int n) {
    int iters = n / 4;
    const uint32_t *pa = a, *pb = b;
    uint32_t* o = out;
    pieConsts();
    asm volatile(
        "loopgtz %3, 1f\n"
        "ee.vld.128.ip q0, %0, 16\n"
        "ee.vld.128.ip q2, %1, 16\n"
        PIE_AVG("q0", "q2", "q3")
        "ee.vst.128.ip q3, %2, 16\n"
        "1:\n"
        : "+r"(pa), "+r"(pb), "+r"(o), "+r"(iters) :: "memory");
}
// avgRows at any 4-byte offset shared by all three rows: C up to alignment,
// SIMD for the rest, C for any remainder.
static void IRAM_ATTR avgRowsAny(uint32_t* out, const uint32_t* a, const uint32_t* b, int n) {
    int h = ((16 - ((uintptr_t)out & 15)) & 15) / 4;
    for (int j = 0; j < h && j < n; j++) out[j] = avg2px(a[j], b[j]);
    int m = (n - h) / 4;
    if (m > 0) avgRowsPIE(out + h, a + h, b + h, m * 4);
    for (int j = h + (m > 0 ? m * 4 : 0); j < n; j++) out[j] = avg2px(a[j], b[j]);
}

volatile uint32_t g_halfFillCycles = 0;   // CPU cycles in _fillHalf (cumulative)
volatile uint32_t g_halfScalarChunks = 0; // chunks done in C (bounce buffer not 16-byte aligned)

void IRAM_ATTR ESP32S3_Touch_LCD_7_7B::_fillHalf(void* bounce_buf, int pos_px, int len_bytes,
                                               const uint16_t* framebuffer) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    const int W = _width, SW = _width / 2, SH = _height / 2;
    const int row0 = pos_px / W;                  // first output row (even)
    const int nrows = len_bytes / (W * (int)sizeof(uint16_t));
    uint32_t* bb = (uint32_t*)bounce_buf;
    const int wpr = W / 2;                        // output words per row
    // The row after the chunk, at the same 16-byte offset as the bounce buffer
    // so the SIMD averaging can treat all three rows alike.
    static uint32_t tailBuf[512 + 4] __attribute__((aligned(16)));
    uint32_t* tail = tailBuf + (((uintptr_t)bb & 15) / 4);
    // SIMD needs 16-byte-aligned source rows (the framebuffers are 64-byte
    // aligned) and 4-byte-aligned output; the C versions cover anything else.
    const bool pie = (((uintptr_t)framebuffer & 15) == 0) && (((uintptr_t)bb & 3) == 0) &&
                     ((_fb_stride * 2) % 16 == 0) && (SW % 8) == 0;
    if (!pie) g_halfScalarChunks = g_halfScalarChunks + 1;
    // Even output rows: source rows, doubled horizontally.  (Doing the odd
    // rows in the same pass measured slower: more branches in the loop.)
    for (int r = 0; r < nrows; r += 2) {
        const uint16_t* s = framebuffer + (size_t)((row0 + r) / 2) * _fb_stride;
        if (pie) doubleRowPIE(bb + r * wpr, s, SW); else doubleRow(bb + r * wpr, s, SW);
    }
    // Odd output rows: the average of the even rows around them.  The last one
    // needs the next chunk's first row, made from the source here; at the
    // bottom edge the last row repeats.
    for (int r = 1; r < nrows; r += 2) {
        const uint32_t* below;
        if (r + 1 < nrows) {
            below = bb + (r + 1) * wpr;
        } else {
            int sy = (row0 + r + 1) / 2;
            if (sy >= SH) sy = SH - 1;
            const uint16_t* s = framebuffer + (size_t)sy * _fb_stride;
            if (pie) doubleRowPIE(tail, s, SW); else doubleRow(tail, s, SW);
            below = tail;
        }
        if (pie) avgRowsAny(bb + r * wpr, bb + (r - 1) * wpr, below, wpr);
        else avgRows(bb + r * wpr, bb + (r - 1) * wpr, below, wpr);
    }
    g_halfFillCycles = g_halfFillCycles + (esp_cpu_get_cycle_count() - t0);
}

void IRAM_ATTR ESP32S3_Touch_LCD_7_7B::_fillBounceBuffer332(void* bounce_buf, int pos_px, int len_bytes,
                                                const uint8_t* framebuffer, uint32_t scroll_offset) {
    const int screen_width = _width;
    const uint32_t fb_size = _fb_size;
    const int fb_stride = _fb_stride;
    const uint16_t* lut = s_lut332;
    int pixel_count = len_bytes / sizeof(uint16_t);

    int start_pos = _rotate180 ? (screen_width * _height - pos_px - pixel_count)
                               : pos_px;
    int screen_y = start_pos / screen_width;
    int screen_x = start_pos % screen_width;
    // Forward: dst walks up from the start.  180: dst walks down from the end.
    uint16_t* dst = _rotate180 ? (uint16_t*)bounce_buf + pixel_count - 1 : (uint16_t*)bounce_buf;

    while (pixel_count > 0) {
        uint32_t fb_pos = (scroll_offset + screen_y * fb_stride + screen_x) % fb_size;
        const uint8_t* src = framebuffer + fb_pos;

        int segment = screen_width - screen_x;
        int pixels_to_wrap = fb_size - fb_pos;
        if (pixels_to_wrap < segment) segment = pixels_to_wrap;
        if (pixel_count < segment) segment = pixel_count;

        int i = 0;
        if (!_rotate180) {
            // Head until src is word-aligned (dst is then too: rows start aligned).
            for (; i < segment && ((uintptr_t)(src + i) & 3); i++) *dst++ = lut[src[i]];
            const uint32_t* s32 = (const uint32_t*)(src + i);
            if (((uintptr_t)dst & 3) == 0) {
                uint32_t* d32 = (uint32_t*)dst;
                for (; i + 8 <= segment; i += 8) {
                    uint32_t a = *s32++, b = *s32++;
                    d32[0] = lut[a & 0xFF] | ((uint32_t)lut[(a >> 8) & 0xFF] << 16);
                    d32[1] = lut[(a >> 16) & 0xFF] | ((uint32_t)lut[a >> 24] << 16);
                    d32[2] = lut[b & 0xFF] | ((uint32_t)lut[(b >> 8) & 0xFF] << 16);
                    d32[3] = lut[(b >> 16) & 0xFF] | ((uint32_t)lut[b >> 24] << 16);
                    d32 += 4;
                }
                dst = (uint16_t*)d32;
            }
            for (; i < segment; i++) *dst++ = lut[src[i]];
        } else {
            for (; i < segment && ((uintptr_t)(src + i) & 3); i++) *dst-- = lut[src[i]];
            const uint32_t* s32 = (const uint32_t*)(src + i);
            for (; i + 4 <= segment; i += 4) {
                uint32_t a = *s32++;
                dst[0]  = lut[a & 0xFF];
                dst[-1] = lut[(a >> 8) & 0xFF];
                dst[-2] = lut[(a >> 16) & 0xFF];
                dst[-3] = lut[a >> 24];
                dst -= 4;
            }
            for (; i < segment; i++) *dst-- = lut[src[i]];
        }

        pixel_count -= segment;
        screen_x += segment;
        if (screen_x >= screen_width) {
            screen_x = 0;
            screen_y++;
        }
    }
}

// Overlay active sprites onto a freshly-filled bounce buffer.  Split out of
// _fillBounceBuffer so the GDMA fill path can reuse it after the DMA copy.
void IRAM_ATTR ESP32S3_Touch_LCD_7_7B::_overlaySprites(void* bounce_buf, int pos_px, int len_bytes) {
    const int screen_width = _width;

    // Sprite overlay - draw sprites on top of framebuffer content
    // Sprites are in display coordinates (not affected by scrolling)
    const int chunk_start_px = pos_px;
    const int chunk_end_px = pos_px + (len_bytes / sizeof(uint16_t)) - 1;
    const int chunk_start_y = chunk_start_px / screen_width;
    const int chunk_end_y = chunk_end_px / screen_width;

    uint16_t* bounce_buf16 = (uint16_t*)bounce_buf;

    for (int spr = 0; spr < SPRITE_MAX_COUNT; spr++) {
        const Sprite& sprite = _active_sprites[spr];

        // Skip disabled sprites
        if (sprite.yx == SPRITE_DISABLED || sprite.data == nullptr) continue;

        // Extract sprite properties
        const int16_t spr_y = SPRITE_GET_Y(sprite.yx);
        const int16_t spr_x = SPRITE_GET_X(sprite.yx);
        const uint16_t spr_h = SPRITE_GET_H(sprite.hw);
        const uint16_t spr_w = SPRITE_GET_W(sprite.hw);
        const uint16_t trans = sprite.transparent;
        const uint16_t* spr_data = sprite.data;
        const bool detect_collision = (sprite.flags & SPRITE_FLAG_COLLISION_DETECT) != 0;

        // Collision detection state for this sprite in this chunk
        bool first_color_seen = false;
        uint16_t first_color = 0;
        bool collision_detected = false;

        // Calculate sprite's logical screen Y range
        const int16_t spr_y_end = spr_y + spr_h - 1;

        // Physical Y span of this sprite (flipped under 180-degree rotation),
        // used to reject sprites that don't fall in this (physical) chunk.
        int16_t phys_y_lo, phys_y_hi;
        if (_rotate180) {
            phys_y_lo = _height - 1 - spr_y_end;
            phys_y_hi = _height - 1 - spr_y;
        } else {
            phys_y_lo = spr_y;
            phys_y_hi = spr_y_end;
        }

        // Quick rejection: sprite doesn't overlap this chunk's (physical) Y range
        if (phys_y_lo > chunk_end_y || phys_y_hi < chunk_start_y) continue;

        // Calculate sprite's screen X range (for horizontal bounds)
        const int16_t spr_x_end = spr_x + spr_w - 1;

        // Quick rejection: sprite is entirely off-screen horizontally
        if (spr_x >= screen_width || spr_x_end < 0) continue;

        // Process each row of the sprite that falls within this chunk
        for (int16_t row = 0; row < spr_h; row++) {
            int16_t scr_y = spr_y + row;          // logical screen row

            // Skip rows off-screen (logical bounds)
            if (scr_y < 0 || scr_y >= _height) continue;

            // Physical row this logical row maps to (flipped under 180)
            int16_t phys_y = _rotate180 ? (_height - 1 - scr_y) : scr_y;

            // Skip rows outside this chunk's (physical) Y range
            if (phys_y < chunk_start_y || phys_y > chunk_end_y) continue;

            // Calculate visible X range for this sprite row
            int16_t vis_x_start = (spr_x < 0) ? 0 : spr_x;
            int16_t vis_x_end = (spr_x_end >= screen_width) ? (screen_width - 1) : spr_x_end;

            // Skip if no visible pixels
            if (vis_x_start > vis_x_end) continue;

            // For each pixel in the visible sprite row
            for (int16_t scr_x = vis_x_start; scr_x <= vis_x_end; scr_x++) {
                // Physical position of this logical pixel (flipped under 180)
                int16_t phys_x = _rotate180 ? (screen_width - 1 - scr_x) : scr_x;
                int linear_pos = phys_y * screen_width + phys_x;

                // Check if this pixel is in the current chunk
                if (linear_pos < chunk_start_px || linear_pos > chunk_end_px) continue;

                // Get sprite pixel (sprite data is addressed in logical space)
                int spr_px_x = scr_x - spr_x;  // X offset into sprite
                uint16_t spr_pixel = spr_data[row * spr_w + spr_px_x];

                // Skip transparent pixels
                if (spr_pixel == trans) continue;

                int buf_offset = linear_pos - chunk_start_px;

                // Collision detection: check if we're overwriting multiple colors
                if (detect_collision && !collision_detected) {
                    uint16_t existing_color = bounce_buf16[buf_offset];
                    if (!first_color_seen) {
                        first_color = existing_color;
                        first_color_seen = true;
                    } else if (existing_color != first_color) {
                        collision_detected = true;
                    }
                }

                // Write to bounce buffer
                bounce_buf16[buf_offset] = spr_pixel;
            }
        }

        // Set collision flag if detected (only sets, never clears)
        if (collision_detected) {
            _sprite_collisions[spr] = 1;
        }
    }
}

ESP32S3_Touch_LCD_7_7B::ESP32S3_Touch_LCD_7_7B(ESP32S3_Touch_LCD_7_7B_Board& board,
                                         uint16_t fb_pad_x, uint16_t fb_pad_y)
    : _board(board)
    , _fb_pad_x(fb_pad_x)
    , _fb_pad_y(fb_pad_y)
    , _fb_stride(_width + fb_pad_x)
    , _fb_rows(_height + fb_pad_y)
    , _fb_size((_width + fb_pad_x) * (_height + fb_pad_y))
{
    // Framebuffer includes padding for pre-drawing scroll content
    _framebuffer_size = _fb_size * sizeof(uint16_t);

    // Initialize all sprite slots as disabled
    for (int i = 0; i < SPRITE_MAX_COUNT; i++) {
        _pending_sprites[i].data = nullptr;
        _pending_sprites[i].yx = SPRITE_DISABLED;
        _pending_sprites[i].hw = 0;
        _pending_sprites[i].transparent = 0;
        _pending_sprites[i].flags = 0;
        _active_sprites[i].data = nullptr;
        _active_sprites[i].yx = SPRITE_DISABLED;
        _active_sprites[i].hw = 0;
        _active_sprites[i].transparent = 0;
        _active_sprites[i].flags = 0;
        _sprite_collisions[i] = 0;
    }
}

bool ESP32S3_Touch_LCD_7_7B::allocateSecondFramebuffer() {
    // Already allocated
    if (_framebuffer2 != nullptr) {
        return false;
    }

    // Allocate second framebuffer with same size and alignment as primary
    _framebuffer2 = (uint16_t*)heap_caps_aligned_alloc(64, _framebuffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_framebuffer2) {
        Serial.println("Failed to allocate second PSRAM framebuffer");
        return false;
    }

    // Clear to black
    _clearPaced(_framebuffer2);

#if LCD7_7B_DCACHE_AUTOLOAD
    if (!_dma)   // CPU fill only; see begin()
        enableFramebufferAutoload(CACHE_AUTOLOAD_REGION1, _framebuffer2, _framebuffer_size);
#endif

    return true;
}

// Clear a framebuffer to black while the panel may already be scanning: in
// pieces, each gated on the bounce fill so the two don't fight over PSRAM.
// One unpaced 1.2 MB memset starved the GDMA fill (~25 late chunks each).
#ifndef LCD7_7B_CLEAR_MAX_WAIT_US
#define LCD7_7B_CLEAR_MAX_WAIT_US 3000   // per piece: never hang if the fill stalls
#endif
void ESP32S3_Touch_LCD_7_7B::_clearPaced(uint16_t* fb) {
    if (!_dma) {
        // CPU fill: paceWrites() has no slack figure to gate on, and the clear
        // usually runs on the other core (Arduino on core 0), where an unpaced
        // memset floods the shared cache and PSRAM while every fill reads
        // through them: 16 late and 36 dropped fills at boot on the -7B.  So
        // write one 4 KB piece at a time, each only while the fill task is
        // idle -- its queue empty and the task blocked, i.e. in the gap
        // after a chunk's fill.  On the fill's own core the fill preempts the
        // clear anyway, and the test passes at once.  No write-back: the CPU
        // fill reads through the cache.
        const size_t piece = 4 * 1024;
        for (size_t off = 0; off < _framebuffer_size; off += piece) {
            if (_bounceQueue && _bounceTask) {
                uint32_t t0 = micros();
                while ((uxQueueMessagesWaiting(_bounceQueue) != 0 ||
                        eTaskGetState(_bounceTask) != eBlocked) &&
                       micros() - t0 < LCD7_7B_CLEAR_MAX_WAIT_US) {}
            }
            size_t n = _framebuffer_size - off < piece ? _framebuffer_size - off : piece;
            memset((uint8_t*)fb + off, 0, n);
        }
        return;
    }
    const size_t piece = 8 * 1024;
    for (size_t off = 0; off < _framebuffer_size; off += piece) {
        paceWrites();
        size_t n = _framebuffer_size - off < piece ? _framebuffer_size - off : piece;
        memset((uint8_t*)fb + off, 0, n);
        writeBackRange((uint8_t*)fb + off, n);
    }
}

bool ESP32S3_Touch_LCD_7_7B::allocateThirdFramebuffer() {
    if (_framebuffer3 != nullptr || _framebuffer2 == nullptr || _rgb332) return false;
    _framebuffer3 = (uint16_t*)heap_caps_aligned_alloc(64, _framebuffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_framebuffer3) {
        Serial.println("Failed to allocate third PSRAM framebuffer");
        return false;
    }
    _clearPaced(_framebuffer3);
    // (No dcache autoload region: the S3 has two, and buffers 1 and 2 take them.)
    return true;
}

void ESP32S3_Touch_LCD_7_7B::setDrawBuffer(uint8_t index) {
    if (index == 1) {
        _draw_target = _framebuffer;
        _draw_fb_index = 1;
    } else if (index == 2 && _framebuffer2 != nullptr) {
        _draw_target = _framebuffer2;
        _draw_fb_index = 2;
    } else if (index == 3 && _framebuffer3 != nullptr) {
        _draw_target = _framebuffer3;
        _draw_fb_index = 3;
    }
}

// Pacing threshold: below this much slack on the last DMA chunk, bulk writers wait.
// Default: about 0.76 of a bounce period (700 us on the -7B at 30 MHz, ~890 us on the -7 at 14 MHz), so it follows the
// pixel clock.  Define LCD7_7B_PACE_SLACK_US to fix it instead.
#ifdef LCD7_7B_PACE_SLACK_US
#define LCD7_7B_PACE_SLACK() (LCD7_7B_PACE_SLACK_US)
#else
#define LCD7_7B_PACE_SLACK() (g_bbPeriodUs ? (int32_t)(g_bbPeriodUs * 76 / 100) : 700)
#endif
#ifndef LCD7_7B_PACE_MAX_WAIT_US
#define LCD7_7B_PACE_MAX_WAIT_US 3000
#endif

uint32_t ESP32S3_Touch_LCD_7_7B::psramClockMHz() {
    static const uint8_t coreMHz[4] = {80, 120, 160, 240};
    uint32_t core = coreMHz[REG_GET_FIELD(SPI_MEM_CORE_CLK_SEL_REG(0), SPI_MEM_CORE_CLK_SEL)];
    uint32_t clk = REG_READ(SPI_MEM_SRAM_CLK_REG(0));
    if (clk & SPI_MEM_SCLK_EQU_SYSCLK) return core;
    return core / (((clk >> SPI_MEM_SCLKCNT_N_S) & SPI_MEM_SCLKCNT_N_V) + 1);
}

void ESP32S3_Touch_LCD_7_7B::paceWrites() {
    const int32_t need = LCD7_7B_PACE_SLACK();
    if (!_dma || g_bbDmaLastSlackUs >= need) return;
    uint32_t t0 = micros();
    g_bbPaceWaits = g_bbPaceWaits + 1;
    while (g_bbDmaLastSlackUs < need && micros() - t0 < LCD7_7B_PACE_MAX_WAIT_US)
        esp_rom_delay_us(20);
    g_bbPaceWaitUs = g_bbPaceWaitUs + (micros() - t0);
}

// LCD7_7B_RANGE_WB_STALL: also stall the other core around each range
// write-back (as the flip's whole-cache one does).  Off by default: range
// write-backs are what ESP-IDF's own esp_cache_msync does unstalled.
#ifndef LCD7_7B_RANGE_WB_STALL
#define LCD7_7B_RANGE_WB_STALL 0
#endif
volatile uint32_t g_wbRangeCycles = 0, g_wbRangeCalls = 0, g_wbRangeMaxUs = 0;
// The cache steps through every line of a range, dirty or not (~70 ns per
// 32-byte line, measured), with interrupts off on this core.  So large ranges
// go in pieces of LCD7_7B_WB_PIECE bytes, each its own short critical section.
#ifndef LCD7_7B_WB_PIECE
#define LCD7_7B_WB_PIECE 4096
#endif
void ESP32S3_Touch_LCD_7_7B::writeBackRange(const void* addr, size_t len) {
    if (!s_cacheWriteBackNeeded || len == 0) return;
    uintptr_t a = (uintptr_t)addr;
    while (len) {
        size_t n = len > LCD7_7B_WB_PIECE ? LCD7_7B_WB_PIECE : len;
        portENTER_CRITICAL(&s_cache_mux);
        uint32_t t0 = esp_cpu_get_cycle_count();   // inside: nothing preempts this
#if LCD7_7B_RANGE_WB_STALL && !CONFIG_FREERTOS_UNICORE
        esp_ipc_isr_stall_other_cpu();
#endif
        Cache_WriteBack_Addr((uint32_t)a, n);
#if LCD7_7B_RANGE_WB_STALL && !CONFIG_FREERTOS_UNICORE
        esp_ipc_isr_release_other_cpu();
#endif
        uint32_t dt = esp_cpu_get_cycle_count() - t0;
        portEXIT_CRITICAL(&s_cache_mux);
        g_wbRangeCycles = g_wbRangeCycles + dt;
        g_wbRangeCalls = g_wbRangeCalls + 1;
        if (dt / 240 > g_wbRangeMaxUs) g_wbRangeMaxUs = dt / 240;
        a += n;
        len -= n;
    }
}

void ESP32S3_Touch_LCD_7_7B::syncToPsram(const void* addr, size_t len) {
    if (!s_cacheWriteBackNeeded || len == 0) return;
    portENTER_CRITICAL(&s_cache_mux);
    esp_cache_msync((void*)addr, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    portEXIT_CRITICAL(&s_cache_mux);
}

void ESP32S3_Touch_LCD_7_7B::setActiveFramebuffer(uint8_t index, bool wait) {
    // DMA reads PSRAM behind the cache: push the finished frame out first.
    // The other core is stalled for the duration.  Both cores share the data
    // cache, and a whole-cache write-back while the other core was reading
    // through it crashed that core in unrelated code (bad pointers, corrupt
    // spinlocks; bench 2026-10-27, Arduino on core 0).  ESP-IDF's flash driver
    // stalls the other core around whole-cache operations in the same way.
    if (s_cacheWriteBackNeeded && !_rangeWriteBack) {
        portENTER_CRITICAL(&s_cache_mux);
        uint32_t t0 = esp_cpu_get_cycle_count();
#if !CONFIG_FREERTOS_UNICORE
        esp_ipc_isr_stall_other_cpu();
#endif
        Cache_WriteBack_All();
#if !CONFIG_FREERTOS_UNICORE
        esp_ipc_isr_release_other_cpu();
#endif
        uint32_t us = (esp_cpu_get_cycle_count() - t0) / 240;
        portEXIT_CRITICAL(&s_cache_mux);
        if (us > g_flipWriteBackMaxUs) g_flipWriteBackMaxUs = us;
    }
    // index: 0 = swap between 1 and 2, 1 = primary, 2 = secondary, 3 = third
    if (index == 0) {
        // Swap: toggle between 1 and 2 (only if framebuffer2 exists)
        if (_framebuffer2 != nullptr) {
            _pending_fb = (_pending_fb == 1) ? 2 : 1;
        } else {
            // No second framebuffer, nothing to swap
            return;
        }
    } else if (index == 1) {
        _pending_fb = 1;
    } else if (index == 2 && _framebuffer2 != nullptr) {
        _pending_fb = 2;
    } else if (index == 3 && _framebuffer3 != nullptr) {
        _pending_fb = 3;
    } else {
        // Invalid request - nothing to wait for
        return;
    }

    // If requested, block until the ISR has applied the pending value
    if (wait) {
        while (_active_fb != _pending_fb) {
            // Yield to other tasks while waiting for vsync
            vTaskDelay(1);
        }
    }
}

// Minimum pixel clock, per board.  A RAM-less RGB panel locks to the incoming
// DE/VSYNC/DCLK timing; drive it too slow and it may drop the input.
//   -7:  the ST7262 shows its self-test colour cycle below ~12 MHz; 12 MHz is
//        marginal from a cold start, so the clamp sits ~10 % above that band.
//   -7B: still locked at 12 MHz, the lowest value tried, so that is the
//        clamp; nothing lower has been tested.  Visible flicker sets in well
//        above it -- see begin() in the header.
#ifndef LCD7_7B_MIN_PCLK_HZ_7
#define LCD7_7B_MIN_PCLK_HZ_7  13000000
#endif
#ifndef LCD7_7B_MIN_PCLK_HZ_7B
#define LCD7_7B_MIN_PCLK_HZ_7B 12000000
#endif

// Geometry and timing for the board _board.begin() identified (see the
// header's timing table).  Recomputes the framebuffer layout to match.
bool ESP32S3_Touch_LCD_7_7B::_applyBoard() {
    switch (_board.model()) {
    case ESP32S3_Touch_LCD_7_7B_Board::MODEL_7:      // 800x480 ST7262
        _width = 800;  _height = 480;
        _hpw = 4;   _hbp = 8;   _hfp = 8;
        _vpw = 4;   _vbp = 8;   _vfp = 8;
        _defaultPclk = 14000000;                  // ~34.1 Hz
        _minPclk = LCD7_7B_MIN_PCLK_HZ_7;
        break;
    case ESP32S3_Touch_LCD_7_7B_Board::MODEL_7B:     // 1024x600
        _width = 1024; _height = 600;
        _hpw = 162; _hbp = 152; _hfp = 48;
        _vpw = 45;  _vbp = 13;  _vfp = 3;
        _defaultPclk = 30000000;                  // ~32.7 Hz
        _minPclk = LCD7_7B_MIN_PCLK_HZ_7B;
        break;
    default:
        return false;
    }
    _bounce_buffer_size_px = _width * LCD7_7B_BOUNCE_ROWS;
    setHalfResolution(_half);   // stride, rows and sizes for this panel
    return true;
}


bool ESP32S3_Touch_LCD_7_7B::begin(uint32_t pclk_hz, uint16_t rotation) {
    // Bring up the board's shared I2C bus + expander first: it identifies the
    // board, and the panel's reset and backlight live on the expander.
    // (Idempotent -- lv_setup and touch.begin() may also call it.)
    _board.begin();
    if (!_applyBoard()) {
        Serial.println("Display: no ESP32-S3-Touch-LCD-7 or -7B expander answered on I2C");
        return false;
    }
    Serial.printf("Display: %s, %ux%u\n", _board.modelName(), (unsigned)_width, (unsigned)_height);

    if (pclk_hz == 0) pclk_hz = _defaultPclk;
    const float frame_px = (float)(_width + _hpw + _hbp + _hfp) *
                           (float)(_height + _vpw + _vbp + _vfp);
    // Guard the RAM-less panel's refresh floor.  Clamp up, don't surprise.
    if (pclk_hz < _minPclk) {
        Serial.printf("Requested pclk %u Hz is below the panel's lock floor; "
                      "clamping to %u Hz (~%.1f Hz refresh).\n",
                      (unsigned)pclk_hz, (unsigned)_minPclk, (float)_minPclk / frame_px);
        pclk_hz = _minPclk;
    }

    // Latch the rotation for the lifetime of the driver.  Normalize index
    // aliases (1/2/3 -> 90/180/270) first; only 180 flips here.  90/270 are not
    // handled by the driver -- they belong at the LVGL->framebuffer level.
    rotation = cglcd7_7b_normalize_rotation(rotation);
    _rotate180 = (rotation == 180);

    // Hardware-reset the panel via the expander (EXIO3).  There is no SPI command
    // controller on this RGB panel, so this is the entire panel bring-up.
    _board.lcdReset();

    // Allocate our own framebuffer in PSRAM (required for no_fb mode with callback)
    // Use 64-byte alignment for optimal cache line access
    if (_half) {
        // Half resolution: the fill interpolates with the CPU (see _fillHalf).
        _rgb332 = false;
        _bounceFillMode = BOUNCE_FILL_SAME_CORE;
    }
    _framebuffer_size = _fb_size * (_rgb332 ? 1 : sizeof(uint16_t));
    if (_rgb332 && !s_lut332Ready) initLut332();
    _framebuffer = (uint16_t*)heap_caps_aligned_alloc(64, _framebuffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // The header refuses builds without PSRAM, but PSRAM can still be missing at run
    // time (e.g. Tools > PSRAM set to the wrong mode for the chip), so fail cleanly.
    if (!_framebuffer) {
        Serial.println("Failed to allocate PSRAM framebuffer - is PSRAM enabled, in the right mode?");
        return false;
    } else {
        // Black, before the panel starts scanning: the same clear done after
        // esp_lcd_panel_init() competed with the first GDMA fills (late chunks).
        memset(_framebuffer, 0, _framebuffer_size);
        Cache_WriteBack_All();
    }

#if LCD7_7B_DCACHE_AUTOLOAD
    // CPU fill only: GDMA reads behind the cache, so autoload can't help it --
    // and a range write-back (syncToPsram) inside an autoload region hangs the
    // cache (bench, 2026-10-26: interrupt-watchdog reset).
    if (_framebuffer && (_bounceFillMode == BOUNCE_FILL_SAME_CORE ||
                         _bounceFillMode == BOUNCE_FILL_OTHER_CORE ||
                         (_bounceFillMode == BOUNCE_FILL_DEFAULT && _rotate180)))
        enableFramebufferAutoload(CACHE_AUTOLOAD_REGION0, _framebuffer, _framebuffer_size);
#endif

    // Set default draw target to primary framebuffer
    _draw_target = _framebuffer;
    _draw_fb_index = 1;

    // Configure the RGB peripheral for this board's panel.
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = pclk_hz,
            .h_res = _width,
            .v_res = _height,
            .hsync_pulse_width = _hpw,
            .hsync_back_porch = _hbp,
            .hsync_front_porch = _hfp,
            .vsync_pulse_width = _vpw,
            .vsync_back_porch = _vbp,
            .vsync_front_porch = _vfp,
            .flags = {
                .hsync_idle_low = 0,
                .vsync_idle_low = 0,
                .de_idle_high = 0,
                .pclk_active_neg = 1,  // data latched on the falling edge (Waveshare's setting)
                .pclk_idle_high = 0,
            },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 0,
        .bounce_buffer_size_px = (size_t)_bounce_buffer_size_px,  // LCD7_7B_BOUNCE_ROWS scanlines in SRAM
        .sram_trans_align = 64,   // bounce buffers 64-byte aligned for GDMA bursts
        .psram_trans_align = 64,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {0},
        .flags = {
            .disp_active_low = 0,
            .refresh_on_demand = false,
            .fb_in_psram = false,
            .double_fb = false,
            .no_fb = true,
            .bb_invalidate_cache = false,
        },
    };

    // Set data GPIO pins individually (RGB565: B=bits 0-4, G=bits 5-10, R=bits 11-15)
    panel_config.data_gpio_nums[0] = PIN_LCD_B0;
    panel_config.data_gpio_nums[1] = PIN_LCD_B1;
    panel_config.data_gpio_nums[2] = PIN_LCD_B2;
    panel_config.data_gpio_nums[3] = PIN_LCD_B3;
    panel_config.data_gpio_nums[4] = PIN_LCD_B4;
    panel_config.data_gpio_nums[5] = PIN_LCD_G0;
    panel_config.data_gpio_nums[6] = PIN_LCD_G1;
    panel_config.data_gpio_nums[7] = PIN_LCD_G2;
    panel_config.data_gpio_nums[8] = PIN_LCD_G3;
    panel_config.data_gpio_nums[9] = PIN_LCD_G4;
    panel_config.data_gpio_nums[10] = PIN_LCD_G5;
    panel_config.data_gpio_nums[11] = PIN_LCD_R0;
    panel_config.data_gpio_nums[12] = PIN_LCD_R1;
    panel_config.data_gpio_nums[13] = PIN_LCD_R2;
    panel_config.data_gpio_nums[14] = PIN_LCD_R3;
    panel_config.data_gpio_nums[15] = PIN_LCD_R4;

    esp_err_t ret = esp_lcd_new_rgb_panel(&panel_config, &_panel_handle);
    if (ret != ESP_OK) {
        Serial.printf("Failed to create RGB panel: %d\n", ret);
        return false;
    }

    // Report the requested pclk and the refresh rate it implies.  On a RAM-less
    // RGB panel refresh = pclk / (htotal * vtotal), so this is the frame rate the
    // bounce-buffer copy runs at.  NOTE: this is the *requested* pclk; the S3
    // quantizes to clock_source/divider, so the real rate is within a step of this.
    {
        const auto& t = panel_config.timings;
        uint32_t htotal = t.h_res + t.hsync_pulse_width + t.hsync_back_porch + t.hsync_front_porch;
        uint32_t vtotal = t.v_res + t.vsync_pulse_width + t.vsync_back_porch + t.vsync_front_porch;
        float refresh = (float)pclk_hz / (float)(htotal * vtotal);
        Serial.printf("RGB panel: pclk=%u Hz, %ux%u total -> ~%.1f Hz refresh; PSRAM clock %u MHz\n",
                      (unsigned)pclk_hz, (unsigned)htotal, (unsigned)vtotal, refresh,
                      (unsigned)psramClockMHz());
    }

#if LCD7_7B_BOUNCE_FILL_IN_TASK
    // Move the PSRAM->bounce-buffer copy into a high-priority task so the GDMA ISR
    // stays short (it only latches frame state and hands off the request).  This
    // is ON by default: the task's cached CPU copy meets the per-bounce-buffer
    // (~1 ms) deadline comfortably (0 drops in testing).  Set LCD7_7B_BOUNCE_FILL_IN_TASK
    // to 0 to copy inline in the ISR instead (like the 2.8C driver), which is also
    // the automatic fallback if the task/queue can't be created.
    //
    // Created BEFORE refresh starts.  By default pinned to the core running
    // begin() -- the same core the GDMA ISR runs on -- so the hand-off is a
    // same-core wakeup; LCD7_7B_BOUNCE_FILL_OTHER_CORE moves it to the other core.
    // If creation fails, _bounceQueue stays null and the ISR copies inline.
    // Resolve BOUNCE_FILL_DEFAULT to a concrete mode using the compile-time
    // macro; an explicit setBounceFillMode() wins.
    if (_bounceFillMode == BOUNCE_FILL_DEFAULT) {
#if LCD7_7B_BOUNCE_FILL_OTHER_CORE
        _bounceFillMode = BOUNCE_FILL_OTHER_CORE;
#else
        // GDMA unless the panel is flipped 180: DMA can't reverse pixel order,
        // so every chunk would fall back to the CPU anyway.
        _bounceFillMode = _rotate180 ? BOUNCE_FILL_SAME_CORE : BOUNCE_FILL_GDMA;
#endif
    }
    if (_bounceFillMode == BOUNCE_FILL_GDMA && !_dma) {
        // On this core, so the completion interrupt lands next to the fill task.
        if (m2mInit()) {
            _dma = s_m2m;   // non-null: the GDMA fill is available
            s_cacheWriteBackNeeded = true;
        } else {
            Serial.println("GDMA fill: GDMA channel setup failed; using the CPU fill");
            _bounceFillMode = BOUNCE_FILL_SAME_CORE;
        }
    }
    BaseType_t fill_core = xPortGetCoreID();
    if (_bounceFillMode == BOUNCE_FILL_OTHER_CORE)
        fill_core ^= 1;   // pin to the opposite core (off the LVGL/UI core)
    _bounceQueue = xQueueCreate(LCD7_7B_BOUNCE_QUEUE_LEN, sizeof(BounceFillRequest));
    if (_bounceQueue != nullptr) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            _bounceFillTask, "lcd77b_bbfill", 4096, this,
            LCD7_7B_BOUNCE_TASK_PRIORITY, &_bounceTask, fill_core);
        if (ok != pdPASS) {
            Serial.println("Bounce-fill task creation failed; copying inline in the ISR");
            vQueueDelete(_bounceQueue);
            _bounceQueue = nullptr;
        }
    } else {
        Serial.println("Bounce-fill queue creation failed; copying inline in the ISR");
    }
#endif

    // Fill-ahead: one scan line in CPU cycles, and a periodic timer (its
    // interrupt on this core, next to the fill task) that asks for a step.
    _rowCycles = (uint32_t)((uint64_t)240000000 *
        (_width + _hpw + _hbp + _hfp) / pclk_hz);
    if (_bounceQueue && !_pfTimer) {
        gptimer_handle_t t = nullptr;
        gptimer_config_t tc = {};
        tc.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        tc.direction = GPTIMER_COUNT_UP;
        tc.resolution_hz = 1000000;
        if (gptimer_new_timer(&tc, &t) == ESP_OK) {
            gptimer_event_callbacks_t cb = {};
            cb.on_alarm = prefillTimerCb;
            gptimer_register_event_callbacks(t, &cb, this);
            gptimer_alarm_config_t ac = {};
            ac.alarm_count = LCD7_7B_PREFILL_TICK_US;
            ac.reload_count = 0;
            ac.flags.auto_reload_on_alarm = true;
            gptimer_set_alarm_action(t, &ac);
            gptimer_enable(t);
            gptimer_start(t);
            _pfTimer = t;
        }
    }

    // Register bounce buffer callback BEFORE init (when refresh starts)
    // With no_fb=true, the callback will be invoked to fill bounce buffers
    // Pass 'this' so callback can access framebuffer and scroll offsets
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_bounce_empty = bounceBufferFillCallback;
    ret = esp_lcd_rgb_panel_register_event_callbacks(_panel_handle, &cbs, this);
    if (ret != ESP_OK) {
        Serial.printf("Failed to register bounce callback: %d\n", ret);
        return false;
    }

    ret = esp_lcd_panel_reset(_panel_handle);
    if (ret != ESP_OK) {
        Serial.printf("Failed to reset panel: %d\n", ret);
        return false;
    }

    ret = esp_lcd_panel_init(_panel_handle);
    if (ret != ESP_OK) {
        Serial.printf("Failed to init panel: %d\n", ret);
        return false;
    }

    // The panel's own GDMA channel outranks the m2m fill pair (all channels
    // default to priority 0, served round-robin): if the copies into the
    // bounce buffers delay the panel's reads, the LCD runs dry mid-line and
    // the picture slips sideways.
    {
        int g = -1, c = -1, slot = -1;
        gdma_channel_handle_t lcdChan = findGdmaChannel(_panel_handle, &g, &c, &slot);
        esp_err_t pe = lcdChan ? gdma_set_priority(lcdChan, GDMA_LL_CHANNEL_MAX_PRIORITY) : ESP_FAIL;
        Serial.printf("LCD GDMA channel: group %d ch %d (slot %d), priority %s\n", g, c, slot,
                      pe == ESP_OK ? "raised" : "NOT raised");
    }

#if LCD7_7B_BOUNCE_DESYNC_WORKAROUND
    // --- Desync detection setup --- (see extras/DESYNC_WORKAROUND.md)
    // Locate the GDMA channel inside the panel object so we can cache a pointer to
    // the hardware register (out.dscr) that reveals which buffer the DMA is reading
    // from. Done after esp_lcd_panel_init(), by which point the channel exists.
    // The bounce callback guards on s_gdma_out_dscr_reg/s_bb_size.
    {
        // The dma_chan offset inside esp_rgb_panel_t drifts across ESP-IDF revisions, so
        // locate it by scanning (findGdmaChannel) rather than through a struct mirror.
        int group_id = -1, channel_id = -1, slot = -1;
        gdma_channel_handle_t dma_chan = findGdmaChannel(_panel_handle, &group_id, &channel_id, &slot);
        if (dma_chan != nullptr) {
            gdma_dev_t* gdma_hw = GDMA_LL_GET_HW(group_id);
            if (gdma_hw != nullptr) {
                s_gdma_out_dscr_reg = &gdma_hw->channel[channel_id].out.dscr;
                Serial.printf("Desync detection: GDMA group=%d ch=%d (slot %d), out.dscr @%p\n",
                              group_id, channel_id, slot, s_gdma_out_dscr_reg);
            }
        } else {
            Serial.printf("Desync detection: no GDMA channel found in panel object "
                          "(esp_rgb_panel_t layout may have changed -- desync workaround disabled)\n");
        }
        s_bb_size = panel_config.bounce_buffer_size_px * sizeof(uint16_t);
    }
    // --- End desync detection setup ---
#endif // LCD7_7B_BOUNCE_DESYNC_WORKAROUND

    // Reveal the display: turn the backlight on (via the expander).
    setBacklight(100);

    return true;
}

void ESP32S3_Touch_LCD_7_7B::setBacklight(int percentage) {
    if (percentage <= 0) {
        _board.lcdBacklight(false);
        return;
    }
    _board.setBacklightBrightness(percentage > 100 ? 100 : (uint8_t)percentage);
    _board.lcdBacklight(true);
}

void ESP32S3_Touch_LCD_7_7B::setBacklight(bool on) {
    setBacklight(on ? 100 : 0);
}

void ESP32S3_Touch_LCD_7_7B::pushImage(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* pixels) {
    if (_draw_target == nullptr) return;
    // Bounds checking against padded framebuffer size (allows pre-drawing scroll content)
    if (x >= _fb_stride || y >= _fb_rows || x + w <= 0 || y + h <= 0) {
        return;
    }

    // Clip to padded framebuffer bounds
    int16_t x1 = max((int16_t)0, x);
    int16_t y1 = max((int16_t)0, y);
    int16_t x2 = min((int16_t)(_fb_stride - 1), (int16_t)(x + w - 1));
    int16_t y2 = min((int16_t)(_fb_rows - 1), (int16_t)(y + h - 1));

    if (_rle) _rleMarkNow(y1, y2);

    int16_t src_offset_x = x1 - x;
    int16_t src_offset_y = y1 - y;
    int16_t clipped_w = x2 - x1 + 1;
    int16_t clipped_h = y2 - y1 + 1;

    // Copy pixels row by row using linear addressing
    for (int16_t row = 0; row < clipped_h; row++) {
        const uint16_t* src = pixels + ((src_offset_y + row) * w) + src_offset_x;

        // Linear position in ring buffer
        uint32_t fb_pos = (_pending_scroll_offset + (y1 + row) * _fb_stride + x1) % _fb_size;

        // Each row is contiguous - only need to handle wrap at buffer end
        int16_t pixels_remaining = clipped_w;
        int16_t src_offset = 0;

        while (pixels_remaining > 0) {
            int pixels_to_wrap = _fb_size - fb_pos;
            int segment = (pixels_remaining < pixels_to_wrap) ? pixels_remaining : pixels_to_wrap;

            if (_rgb332) {
                uint8_t* d8 = (uint8_t*)_draw_target + fb_pos;
                const uint16_t* s16 = src + src_offset;
                for (int i = 0; i < segment; i++) d8[i] = rgb565to332(s16[i]);
                cacheWriteBack(d8, segment);
            } else {
                memcpy(_draw_target + fb_pos, src + src_offset, segment * sizeof(uint16_t));
                cacheWriteBack(_draw_target + fb_pos, segment * sizeof(uint16_t));
            }

            pixels_remaining -= segment;
            src_offset += segment;
            fb_pos = (fb_pos + segment) % _fb_size;
        }
    }
}

void ESP32S3_Touch_LCD_7_7B::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (_draw_target == nullptr) return;
    // Bounds checking against padded framebuffer size (allows pre-drawing scroll content)
    if (x >= _fb_stride || y >= _fb_rows || x + w <= 0 || y + h <= 0) {
        return;
    }

    // Clip to padded framebuffer bounds
    int16_t x1 = max((int16_t)0, x);
    int16_t y1 = max((int16_t)0, y);
    int16_t x2 = min((int16_t)(_fb_stride - 1), (int16_t)(x + w - 1));
    int16_t y2 = min((int16_t)(_fb_rows - 1), (int16_t)(y + h - 1));

    int16_t clipped_w = x2 - x1 + 1;
    int16_t clipped_h = y2 - y1 + 1;
    if (_rle) _rleMarkNow(y1, y2);

    // Fill pixels using linear addressing
    for (int16_t row = 0; row < clipped_h; row++) {
        // Linear position in ring buffer
        uint32_t fb_pos = (_pending_scroll_offset + (y1 + row) * _fb_stride + x1) % _fb_size;

        // Each row is contiguous - only need to handle wrap at buffer end
        int16_t pixels_remaining = clipped_w;

        while (pixels_remaining > 0) {
            int pixels_to_wrap = _fb_size - fb_pos;
            int segment = (pixels_remaining < pixels_to_wrap) ? pixels_remaining : pixels_to_wrap;

            if (_rgb332) {
                uint8_t* d8 = (uint8_t*)_draw_target + fb_pos;
                memset(d8, rgb565to332(color), segment);
                cacheWriteBack(d8, segment);
            } else {
                uint16_t* dst = _draw_target + fb_pos;
                for (int i = 0; i < segment; i++) {
                    dst[i] = color;
                }
                cacheWriteBack(dst, segment * sizeof(uint16_t));
            }

            pixels_remaining -= segment;
            fb_pos = (fb_pos + segment) % _fb_size;
        }
    }
}

void ESP32S3_Touch_LCD_7_7B::fillScreen(uint16_t color) {
    fillRect(0, 0, _width, _height, color);
}

void ESP32S3_Touch_LCD_7_7B::drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (_draw_target == nullptr) return;
    // Bounds checking against padded framebuffer size (allows pre-drawing scroll content)
    if (x < 0 || x >= _fb_stride || y < 0 || y >= _fb_rows) {
        return;
    }

    if (_rle) _rleMarkNow(y, y);

    // Linear position in ring buffer
    uint32_t fb_pos = (_pending_scroll_offset + y * _fb_stride + x) % _fb_size;

    void* pixel;
    if (_rgb332) {
        pixel = (uint8_t*)_draw_target + fb_pos;
        *(uint8_t*)pixel = rgb565to332(color);
    } else {
        pixel = _draw_target + fb_pos;
        *(uint16_t*)pixel = color;
    }

    // Flush this cache line (align to 64-byte boundary for PSRAM)
    uint32_t addr = (uint32_t)pixel & ~63;
    portENTER_CRITICAL(&s_cache_mux);
    Cache_WriteBack_Addr(addr, 64);
    portEXIT_CRITICAL(&s_cache_mux);
}

void ESP32S3_Touch_LCD_7_7B::flush() {
    if (_framebuffer == nullptr) return;
    // Write back entire framebuffer row by row
    const size_t bpp = _rgb332 ? 1 : sizeof(uint16_t);
    const size_t row_bytes = _fb_stride * bpp;
    for (int row = 0; row < _fb_rows; row++) {
        cacheWriteBack((uint8_t*)_framebuffer + row * row_bytes, row_bytes);
    }
}

bool ESP32S3_Touch_LCD_7_7B::scrollFramebuffer(int16_t dx, int16_t dy, uint16_t fill_color) {
    if (!_framebuffer || _rgb332) {   // memory-copy scroll is RGB565-only
        return false;
    }
    if (_rle) _rleMarkNow(0, _height - 1);

    // Note: This function performs memory-copy scrolling (legacy).
    // For hardware scrolling without memory copies, use scroll() instead.

    // Vertical scrolling using SRAM staging buffer (dx == 0)
    if (dx == 0 && dy != 0) {
        // Allocate staging buffer on stack (full stride * 2 bytes)
        uint16_t line_buffer[_fb_stride];

        int16_t src_row_start;
        int16_t dst_row_start;
        int16_t num_rows_to_copy;

        if (dy < 0) {
            // Scroll up: move rows [abs(dy)...height-1] to [0...height-abs(dy)-1]
            src_row_start = -dy;
            dst_row_start = 0;
            num_rows_to_copy = _height - (-dy);
        } else {
            // Scroll down: move rows [0...height-dy-1] to [dy...height-1]
            src_row_start = 0;
            dst_row_start = dy;
            num_rows_to_copy = _height - dy;
        }

        // Copy rows line by line using SRAM staging
        // Copy direction matters for overlapping regions
        if (dy > 0) {
            // Scrolling down: copy from bottom to top to avoid overwriting source
            for (int16_t i = num_rows_to_copy - 1; i >= 0; i--) {
                uint16_t* src_line = _framebuffer + (src_row_start + i) * _fb_stride;
                uint16_t* dst_line = _framebuffer + (dst_row_start + i) * _fb_stride;

                // PSRAM -> SRAM -> PSRAM (copy visible width only)
                memcpy(line_buffer, src_line, _width * 2);
                memcpy(dst_line, line_buffer, _width * 2);
                cacheWriteBack(dst_line, _width * 2);
            }
        } else {
            // Scrolling up: copy from top to bottom to avoid overwriting source
            for (int16_t i = 0; i < num_rows_to_copy; i++) {
                uint16_t* src_line = _framebuffer + (src_row_start + i) * _fb_stride;
                uint16_t* dst_line = _framebuffer + (dst_row_start + i) * _fb_stride;

                // PSRAM -> SRAM -> PSRAM (copy visible width only)
                memcpy(line_buffer, src_line, _width * 2);
                memcpy(dst_line, line_buffer, _width * 2);
                cacheWriteBack(dst_line, _width * 2);
            }
        }

        return false;  // Fill not performed - caller should handle exposed area
    }

    // Horizontal or diagonal scrolling - copy line by line with stack-allocated staging buffer
    // Calculate source and destination regions
    int16_t src_x = (dx > 0) ? 0 : -dx;
    int16_t src_y = (dy > 0) ? 0 : -dy;
    int16_t dst_x = (dx > 0) ? dx : 0;
    int16_t dst_y = (dy > 0) ? dy : 0;
    int16_t copy_width = _width - abs(dx);
    int16_t copy_height = _height - abs(dy);

    // Allocate staging buffer on stack (visible width * 2 bytes)
    uint16_t line_buffer[_width];

    // Pre-fill the line buffer with fill_color once
    // This way we only read pixels that will actually move, and write a complete line
    for (int16_t x = 0; x < _width; x++) {
        line_buffer[x] = fill_color;
    }

    // Copy line by line using optimized PSRAM->SRAM->PSRAM
    if (dy > 0) {
        // Scrolling down: copy from bottom to top
        for (int16_t y = copy_height - 1; y >= 0; y--) {
            uint16_t* src_line = _framebuffer + (src_y + y) * _fb_stride + src_x;
            uint16_t* dst_line = _framebuffer + (dst_y + y) * _fb_stride;

            if (dx > 0) {
                // Scrolling right: read pixels directly to their final position in buffer
                memcpy(line_buffer + dx, src_line, copy_width * 2);
                // Left side (0 to dx-1) already has fill_color from pre-fill
            } else if (dx < 0) {
                // Scrolling left: read pixels directly to their final position
                memcpy(line_buffer, src_line, copy_width * 2);
                // Right side (copy_width to _width-1) already has fill_color from pre-fill
            } else {
                // No horizontal scroll: just copy pixels
                memcpy(line_buffer, src_line, copy_width * 2);
            }

            // Write complete line from SRAM to PSRAM (one burst write)
            memcpy(dst_line, line_buffer, _width * 2);
            cacheWriteBack(dst_line, _width * 2);
        }

        // Fill the top exposed rows with fill_color
        for (int16_t y = 0; y < dy; y++) {
            uint16_t* fill_line = _framebuffer + y * _fb_stride;
            memcpy(fill_line, line_buffer, _width * 2);  // Use pre-filled buffer
            cacheWriteBack(fill_line, _width * 2);
        }
    } else {
        // Scrolling up or no vertical scroll: copy from top to bottom
        for (int16_t y = 0; y < copy_height; y++) {
            uint16_t* src_line = _framebuffer + (src_y + y) * _fb_stride + src_x;
            uint16_t* dst_line = _framebuffer + (dst_y + y) * _fb_stride;

            if (dx > 0) {
                // Scrolling right: read pixels directly to their final position in buffer
                memcpy(line_buffer + dx, src_line, copy_width * 2);
                // Left side (0 to dx-1) already has fill_color from pre-fill
            } else if (dx < 0) {
                // Scrolling left: read pixels directly to their final position
                memcpy(line_buffer, src_line, copy_width * 2);
                // Right side (copy_width to _width-1) already has fill_color from pre-fill
            } else {
                // No horizontal scroll: just copy pixels
                memcpy(line_buffer, src_line, copy_width * 2);
            }

            // Write complete line from SRAM to PSRAM (one burst write)
            memcpy(dst_line, line_buffer, _width * 2);
            cacheWriteBack(dst_line, _width * 2);
        }

        // Fill the bottom exposed rows with fill_color
        if (dy < 0) {
            for (int16_t y = _height + dy; y < _height; y++) {
                uint16_t* fill_line = _framebuffer + y * _fb_stride;
                memcpy(fill_line, line_buffer, _width * 2);  // Use pre-filled buffer
                cacheWriteBack(fill_line, _width * 2);
            }
        }
    }

    return true;  // Fill was performed during scroll
}

void ESP32S3_Touch_LCD_7_7B::setScrollOffset(int16_t x, int16_t y) {
    // Compute linear offset from x,y coordinates
    // Normalize to positive values within buffer bounds
    int32_t offset = y * _fb_stride + x;
    // Handle negative values by adding fb_size until positive
    while (offset < 0) offset += _fb_size;
    _pending_scroll_offset = offset % _fb_size;
    // Cached rows describe screen rows at the old offset.  (The row cache is
    // bypassed while scrolled; this makes coming back to 0 safe.)
    if (_rle) _rleMarkNow(0, _height - 1);
}

void ESP32S3_Touch_LCD_7_7B::scroll(int16_t dx, int16_t dy, uint16_t fill_color, bool fill) {
    if (_framebuffer == nullptr) return;
    if (dx == 0 && dy == 0) return;

    // Calculate new scroll offset: add linear delta to current offset
    // delta = dx + dy * fb_stride (horizontal + vertical contribution)
    int32_t delta = dx + dy * (int32_t)_fb_stride;
    int32_t new_offset = (int32_t)_pending_scroll_offset + delta;

    // Normalize to positive value within buffer
    while (new_offset < 0) new_offset += _fb_size;
    new_offset = new_offset % _fb_size;

    // Track how much we can pre-draw vs post-draw
    int16_t abs_dy = (dy > 0) ? dy : -dy;
    int16_t abs_dx = (dx > 0) ? dx : -dx;
    int16_t predraw_dy = (abs_dy <= _fb_pad_y) ? abs_dy : _fb_pad_y;
    int16_t predraw_dx = (abs_dx <= _fb_pad_x) ? abs_dx : _fb_pad_x;
    int16_t postdraw_dy = abs_dy - predraw_dy;
    int16_t postdraw_dx = abs_dx - predraw_dx;

    // Pre-draw fill into padding area (before changing scroll offset)
    // This writes to framebuffer positions that will be visible after scroll
    if (fill && (predraw_dy > 0 || predraw_dx > 0)) {
        // Pre-fill vertical exposed area
        if (predraw_dy > 0) {
            int16_t screen_y_base = (dy > 0) ? (_height - predraw_dy) : 0;
            for (int16_t row = 0; row < predraw_dy; row++) {
                int16_t screen_y = screen_y_base + row;
                uint32_t fb_pos = (new_offset + screen_y * _fb_stride) % _fb_size;
                for (int16_t x = 0; x < _width; x++) {
                    _framebuffer[(fb_pos + x) % _fb_size] = fill_color;
                }
                cacheWriteBack(_framebuffer + fb_pos, _width * sizeof(uint16_t));
            }
        }

        // Pre-fill horizontal exposed area
        if (predraw_dx > 0) {
            int16_t screen_x_base = (dx > 0) ? (_width - predraw_dx) : 0;
            for (int16_t y = 0; y < _height; y++) {
                uint32_t fb_pos = (new_offset + y * _fb_stride + screen_x_base) % _fb_size;
                for (int16_t col = 0; col < predraw_dx; col++) {
                    _framebuffer[(fb_pos + col) % _fb_size] = fill_color;
                }
                cacheWriteBack(_framebuffer + fb_pos, predraw_dx * sizeof(uint16_t));
            }
        }
    }

    // Update pending scroll offset (takes effect at next frame start)
    _pending_scroll_offset = new_offset;

    // Post-draw any remainder that didn't fit in padding
    if (fill && (postdraw_dy > 0 || postdraw_dx > 0)) {
        if (postdraw_dy > 0) {
            if (dy > 0) {
                // Scrolling down - fill remaining bottom rows
                fillRect(0, _height - abs_dy, _width, postdraw_dy, fill_color);
            } else {
                // Scrolling up - fill remaining top rows (after the pre-drawn ones)
                fillRect(0, predraw_dy, _width, postdraw_dy, fill_color);
            }
        }

        if (postdraw_dx > 0) {
            if (dx > 0) {
                // Scrolling right - fill remaining right columns
                fillRect(_width - abs_dx, 0, postdraw_dx, _height, fill_color);
            } else {
                // Scrolling left - fill remaining left columns (after the pre-drawn ones)
                fillRect(predraw_dx, 0, postdraw_dx, _height, fill_color);
            }
        }
    }
}

// Sprite API implementations

void ESP32S3_Touch_LCD_7_7B::setSprite(uint8_t slot, const uint16_t* data, int16_t x, int16_t y,
                                uint16_t w, uint16_t h, uint16_t transparent,
                                uint32_t flags) {
    if (slot >= SPRITE_MAX_COUNT) return;

    // Temporarily mark as disabled while updating to ensure atomic visibility
    _pending_sprites[slot].yx = SPRITE_DISABLED;

    // Set all properties
    _pending_sprites[slot].data = data;
    _pending_sprites[slot].hw = SPRITE_HW(h, w);
    _pending_sprites[slot].transparent = transparent;
    _pending_sprites[slot].flags = flags;

    // Enable sprite by setting valid coordinates (do this last)
    _pending_sprites[slot].yx = SPRITE_YX(y, x);
}

void ESP32S3_Touch_LCD_7_7B::clearSprite(uint8_t slot) {
    if (slot >= SPRITE_MAX_COUNT) return;
    _pending_sprites[slot].yx = SPRITE_DISABLED;
}

void ESP32S3_Touch_LCD_7_7B::clearAllSprites() {
    for (int i = 0; i < SPRITE_MAX_COUNT; i++) {
        _pending_sprites[i].yx = SPRITE_DISABLED;
    }
}

bool ESP32S3_Touch_LCD_7_7B::checkSpriteCollision(uint8_t slot) {
    if (slot >= SPRITE_MAX_COUNT) return false;

    // Check if collision flag is set and clear it atomically
    if (_sprite_collisions[slot]) {
        _sprite_collisions[slot] = 0;
        return true;
    }
    return false;
}

void ESP32S3_Touch_LCD_7_7B::beginFlashMessage(int16_t y) {
    // Already active or pending -- don't double-allocate
    if (s_flashMsgPending != nullptr) return;

    // y == -1: blank screen only, no message text.
    // Use the sentinel non-null value so the ISR enters the flash overlay path
    // (which fills black first) but finds no message rows to blit.
    if (y == -1) {
        s_flashMsgW = 0;
        s_flashMsgH = 0;
        s_flashMsgStride = 0;
        s_flashMsgX = 0;
        s_flashMsgY = 0;
        s_flashMsgTile = false;
        __asm__ __volatile__("memw" ::: "memory");
        s_flashMsgPending = &s_blankSentinel;
    } else {
        int w = 0, h = 0, stride = 0;
        uint8_t* buf = renderMessageTo1bpp(s_flashWriteMsg, s_flashWriteMsgLen, 2, &w, &h, &stride);
        if (!buf) return;

        int bounceLines = _bounce_buffer_size_px / _width;  // LCD7_7B_BOUNCE_ROWS
        int msgY;
        bool tile = false;

        if (y == -3) {
            // Tile: message appears in every bounce buffer band, so it's visible
            // even if the DMA replays a single chunk during a flash stall.
            // msgY is the offset within each band; center the text vertically.
            msgY = (bounceLines - h) / 2;
            if (msgY < 0) msgY = 0;
            tile = true;
        } else if (y == -2) {
            // Default: center vertically, snapped to bounce buffer boundary
            msgY = ((_height - h) / 2);
            msgY = (msgY / bounceLines) * bounceLines;
        } else {
            msgY = (int)y;
            // Round down to bounce buffer boundary
            msgY = (msgY / bounceLines) * bounceLines;
            // Clamp to screen
            if (msgY < 0) msgY = 0;
            if (msgY + h > _height) msgY = ((_height - h) / bounceLines) * bounceLines;
        }

        s_flashMsgW = w;
        s_flashMsgH = h;
        s_flashMsgStride = stride;
        s_flashMsgX = (_width - w) / 2;
        s_flashMsgY = msgY;
        s_flashMsgTile = tile;

        __asm__ __volatile__("memw" ::: "memory");
        s_flashMsgPending = buf;
    }

    // Block until the ISR has applied the change (next frame boundary),
    // then wait for one more complete frame to ensure every bounce buffer
    // chunk has been rendered in flash-overlay mode before we return.
    // This prevents the caller's flash write from starting while the ISR
    // is still mid-frame reading from PSRAM.
    while (s_flashMsgActive != s_flashMsgPending) {
        vTaskDelay(1);
    }
    // Wait for a full frame: at minimum one ISR call per chunk (30 on the -7B, 24 on the -7).
    // We detect a full frame by waiting for pos_px == 0 to occur again, but since
    // we can't easily observe pos_px from here, we use a simple frame counter.
    uint32_t startCount = g_isrCallCount;
    while ((g_isrCallCount - startCount) < (uint32_t)(_width * _height / _bounce_buffer_size_px + 1)) {
        vTaskDelay(1);
    }
}

void ESP32S3_Touch_LCD_7_7B::endFlashMessage() {
    // Save pointer for freeing after the ISR stops using it
    uint8_t* buf = (uint8_t*)s_flashMsgPending;
    if (!buf) return;

    // Clear pending -- the ISR will copy null to active at the next frame start
    s_flashMsgPending = nullptr;

    // Block until the ISR has applied the change (next frame boundary)
    while (s_flashMsgActive != nullptr) {
        vTaskDelay(1);
    }

    // Now safe to free -- the ISR is no longer referencing the buffer.
    // Don't free the static blank sentinel used by beginFlashMessage(-1).
    if (buf != &s_blankSentinel) {
        heap_caps_free(buf);
    }
}
