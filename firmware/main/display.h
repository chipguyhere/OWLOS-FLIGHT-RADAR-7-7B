/* display - one face for every screen. main.c draws a frame strip by strip and hands
 * each strip over, the driver sends it (LCD) or collects it for the next refresh (e-paper).
 * only one driver file is compiled in, see main/CMakeLists.txt */
#pragma once
#include <stdint.h>
#include "gfx.h"
#include "board.h"

void     disp_init(void);
void     disp_backlight(int percent);     // 0..100, does nothing where there is no backlight
// a strip of the frame, top to bottom. last strip of a frame may be shorter
void     disp_strip(const canvas_t *cv);
// frame done. e-paper refreshes here (seconds), LCDs have nothing to do
void     disp_frame_end(void);
// how often a new frame is worth drawing, ms. 0 = as fast as it goes
uint32_t disp_frame_ms(void);
// e-paper: make the next refresh a full one (new screen). LCDs ignore it
void     disp_full_next(void);
// strips that never got the done callback (LCD), refreshes that timed out (e-paper)
uint32_t disp_lost(void);
/* SPI LCDs only (lcd_spi.c): screen start-up profiles. one on most boards, the 2.8" CYD has
 * three (board.h LCD_PROFILES). disp_profile before disp_init picks, after it restarts the screen */
int         disp_profiles(void);
void        disp_profile(int p);
const char *disp_profile_name(void);
// LCDs only: the picture turned 180 degrees, for a screen mounted upside down
void        disp_flip(int on);
/* SPI LCDs only: wait till the strip in flight is out. the 3.5" CYD's touch shares the screen's
 * bus and asks for it only then, never against a strip on its way */
void        disp_wait_idle(void);
/* DISP_HELPER boards (lcd_spi.c): the second core packs its strip under the next one
 * (disp_pack_under), then both go in one transfer (disp_strip2 with its rows); the DMA buffers
 * sized for that (rows = 2 * DISP_STRIP) or back (DISP_STRIP), 0: no memory */
#if DISP_LCD_DSI
/* the Tab5 (lcd_dsi.c): the board package's I2C bus and the power switches on its IO expanders,
 * the radio's among them. before anything asks for the WiFi, from app_main */
void        disp_board_up(void);
#endif
#if DISP_WAVE7
/* the Waveshare 7/7B (lcd_wave7.c): the background goes into the frame that does not show. a new one
 * may start when disp_bg_ready says the last one is up; disp_bg_begin picks the frame, the strips
 * follow, disp_frame_end shows it from the next panel frame on. the moving layer is ovl.h's */
int         disp_bg_ready(void);
void        disp_bg_begin(void);
uint32_t    disp_frame_us(void);      // the panel's frame, as measured
void        disp_log(void);           // the panel's and the layer's counters, for the 10 s log
#endif
void        disp_pack_under(const canvas_t *under);
void        disp_strip2(const canvas_t *cv, int under_rows);
int         disp_rows(int rows);
