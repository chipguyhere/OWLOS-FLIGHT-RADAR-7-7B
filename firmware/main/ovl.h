/*
 * the moving layer (fork addition): the planes, the compass ring and the sweep, put over the frame
 * in memory as the panel is fed, every panel frame. nothing in here knows the board: any S3 board
 * whose RGB panel is fed through bounce buffers by a fill that can call ovl_chunk after a chunk's
 * rows have landed can use it (lcd_wave7.c does, through chipguyhere's 7/7B library).
 *
 * the ui task draws the layer (ui_top_strip) band by band on a canvas of nothing (gfx.h GFX_ALPHA)
 * and hands each band here: what was drawn is kept as runs of colour and coverage, in the PSRAM.
 * three such buffers: one shown, one ready, one being made. the fill takes the newest ready one at
 * each panel frame, so a frame shows one whole layer or the one before, never half of each.
 * the sweep is not drawn by the ui task: its trail and its edge are worked out here, for every
 * row, from the time the panel frame starts. it turns smoothly even when the layer is late.
 */
#pragma once
#include <stdint.h>
#include "gfx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    int      on;            // the sweep turns
    int      cx, cy, r;     // its middle and the range ring, glass pixels
    uint32_t prim, focus;   // the trail's colour and the edge's, 0x00RRGGBB
    uint32_t turn_ms;       // once round in this
    int      holes;         // boxes the app draws over the scope (popups): neither the sweep nor the layer in them
    int16_t  hx0[4], hy0[4], hx1[4], hy1[4];   // glass pixels, x1 and y1 past the end
} ovl_beam_t;

int  ovl_init(void);                          // 0: no memory for it
void ovl_flip(int on);                        // the picture turned 180 degrees (disp_flip)
void ovl_waiter(TaskHandle_t t);              // notified at the start of every panel frame
// the background's row py as the panel shows it now: the busiest rows are made from it
void ovl_source(const uint16_t *(*row)(int py));

// the ui task, once a panel frame: begin, the bands top to bottom, end
void ovl_build_begin(const ovl_beam_t *b);     // b: the holes the bands are cleared in
// takes the drawn pixels x0..x1-1 out of the band and leaves all of it at 0 again for the next
void ovl_build_band(canvas_t *cv, int x0, int x1);
void ovl_build_end(const ovl_beam_t *b);      // shown from the next panel frame on

// the fill, a chunk of rows of the panel (as it stands: turned, when it is) just copied in
void ovl_chunk(uint16_t *dst, int y, int rows, uint32_t frame, void *ctx);
// the fill, row y before its pixels have landed (dst unused, rows 1): what needs no pixels. the
// ovl_chunk for that row that follows uses it
void ovl_prepare(uint16_t *dst, int y, int rows, uint32_t frame, void *ctx);
// the fill, first for each chunk: 0 when nothing goes over rows y..y+rows-1 this frame
#include <stdbool.h>
bool ovl_want(int y, int rows, uint32_t frame, void *ctx);
// the fill, before anything else for each chunk: the chunk composed beforehand, or NULL
const uint16_t *ovl_src(int y, int rows, uint32_t frame, void *ctx);
// core 0, as each panel frame starts: plan the next one and compose its busiest chunks beforehand
void ovl_plan_next(uint32_t period_us);
// how the composed chunks get out of the cache to the PSRAM, where the DMA reads them
void ovl_writeback(void (*wb)(const void *addr, size_t len));
void ovl_log_chunks(void);                    // the chunks losing rows most, for the 10 s log
// a task of its own that plans each next frame as the panel frame starts (ovl_plan_next)
void ovl_planner_start(uint32_t (*period_us)(void), int core, int prio);

typedef struct {
    uint32_t frames, built, skipped, overflow, words_max;
    uint32_t beam_cyc, layer_cyc, words;      // the fill's time on the sweep and on the layer, and the layer's words put on
    uint32_t layer_dropped;                   // rows that showed the sweep alone, the layer not fitting before their deadline
    uint32_t busy;                            // busiest rows made whole when their layer was made
    uint32_t late_by_beam[12];
    uint32_t pre_made, pre_used;              // chunks composed beforehand on core 0, and copied from there by the fill                // rows lost, by where the beam pointed (0: 12 o'clock, 3: 3 o'clock, ...)
} ovl_stats_t;
void ovl_stats(ovl_stats_t *s, int reset);
