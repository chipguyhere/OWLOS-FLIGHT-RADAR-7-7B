/* config.h - which display this build is for (the same set of screens as OWLOS WiFi Radar).
 * pick one here, or pass it from the build: BUILD.BAT <display> does -DRADAR_DISPLAY=...
 * pins and drivers for each one are in firmware/main/board.h */
#pragma once

#define DISPLAY_ILI9341_320x240  1   // ESP32-2432S028R "cheap yellow display", every revision (v1 v2 v3: board.h)
#define DISPLAY_ST7796_480x320   2   // 3.5" ST7796 on classic ESP32 (ViewOwl 480x320)
#define DISPLAY_ILI9486_480x320  3   // Waveshare RPi LCD (A) 3.5" on OWLOSAirQuality board
#define DISPLAY_GC9A01_240x240   4   // round 1.28", ESP32-C3
#define DISPLAY_EPD_400x300      5   // e-paper 4.2" SSD1683, ESP32-S3
#define DISPLAY_EPD_792x272      6   // wide e-paper 5.79", two SSD1683, ESP32-S3
#define DISPLAY_ES3C28P_320x240  7   // LCDWIKI ES3C28P 2.8": ESP32-S3, ILI9341V, FT6336 touch
#define DISPLAY_NMCYDC5_320x240  8   // NM-CYD-C5 2.8": ESP32-C5, ST7789, XPT2046 touch
#define DISPLAY_CROWADV28_320x240 9  // Elecrow CrowPanel Advance 2.8": ESP32-S3, ST7789 IPS, FT6336 touch
#define DISPLAY_CROWADV50_800x480 10 // Elecrow CrowPanel Advance 5.0": ESP32-S3, RGB bus, GT911 touch
#define DISPLAY_JC8048W550_800x480 11 // Guition JC8048W550 5": ESP32-S3, RGB bus, GT911 touch
#define DISPLAY_TAB5_1280x720   12   // M5Stack Tab5 5": ESP32-P4, 720x1280 standing on MIPI-DSI, its touch, through Espressif's board package
#define DISPLAY_4827S043_480x272 13  // ESP32-4827S043C 4.3": ESP32-S3, 480x272 on the RGB bus, GT911 touch
#define DISPLAY_CROW70_800x480  14   // Elecrow CrowPanel 7.0" (the plain one, not the Advance): ESP32-S3, RGB bus, GT911 touch
#define DISPLAY_WAVE7_800x480   15   // Waveshare ESP32-S3-Touch-LCD-7: RGB bus, GT911, chipguy's 7/7B driver (fork addition)
#define DISPLAY_WAVE7B_1024x600 16   // Waveshare ESP32-S3-Touch-LCD-7B: 1024x600, the same driver (fork addition)

#ifndef RADAR_DISPLAY
#define RADAR_DISPLAY DISPLAY_ILI9341_320x240
#endif

#if RADAR_DISPLAY == DISPLAY_ILI9341_320x240 || RADAR_DISPLAY == DISPLAY_ES3C28P_320x240 || \
    RADAR_DISPLAY == DISPLAY_NMCYDC5_320x240 || RADAR_DISPLAY == DISPLAY_CROWADV28_320x240
#define GFX_W 320
#define GFX_H 240
#elif RADAR_DISPLAY == DISPLAY_ST7796_480x320 || RADAR_DISPLAY == DISPLAY_ILI9486_480x320
#define GFX_W 480
#define GFX_H 320
#elif RADAR_DISPLAY == DISPLAY_GC9A01_240x240
#define GFX_W 240
#define GFX_H 240
#define RADAR_ROUND 1                // glass is a circle, nothing may sit in the corners
#elif RADAR_DISPLAY == DISPLAY_EPD_400x300
#define GFX_W 400
#define GFX_H 300
#define RADAR_EPAPER 1               // slow refresh, 4 greys, no animation
#elif RADAR_DISPLAY == DISPLAY_EPD_792x272
#define GFX_W 792
#define GFX_H 272
#define RADAR_EPAPER 1
#elif RADAR_DISPLAY == DISPLAY_CROWADV50_800x480 || RADAR_DISPLAY == DISPLAY_JC8048W550_800x480 || \
      RADAR_DISPLAY == DISPLAY_CROW70_800x480 || RADAR_DISPLAY == DISPLAY_WAVE7_800x480
/* the 5" boards and the 7": the 3.5"'s layout, everything half as big again on an 800x480 glass.
 * GFX_W and GFX_H stay the layout's: the pages, the touch and every number in them go on as they are, and
 * core/gfx.c puts it on the glass in the glass's own pixels (GFX_Z). 534 and not 533: its middle
 * is the glass's middle, and the half pixel past the edge is clipped */
#define GFX_W 534
#define GFX_H 320
#define GFX_PW 800
#define GFX_PH 480
#define GFX_SCALED 1
#define GFX_SCALE_NUM 3              // the glass's pixels for one of the layout's: 3/2
#define GFX_SCALE_SHIFT 1
#define RADAR_RGB_BUS 1              // a frame in memory read out by the chip: its own byte order
#elif RADAR_DISPLAY == DISPLAY_WAVE7B_1024x600
/* the Waveshare 7B: the same layout 15/8 as big on a 1024x600 glass. 547 and not 546: 1024 / 1.875 =
 * 546.1, the half pixel past the edge is clipped. the fonts are the 5" boards' 3/2 ones, as on the
 * Tab5: the writing is 4/5 of its size */
#define GFX_W 547
#define GFX_H 320
#define GFX_PW 1024
#define GFX_PH 600
#define GFX_SCALED 1
#define GFX_SCALE_NUM 15             // 15/8
#define GFX_SCALE_SHIFT 3
#define RADAR_RGB_BUS 1
#elif RADAR_DISPLAY == DISPLAY_TAB5_1280x720
/* the Tab5: the same layout 9/4 as big on a 1280x720 glass (the panel stands, 720x1280: the
 * driver turns it, firmware/main/lcd_dsi.c). 569 and not 568: 1280 / 2.25 = 568.9, the half
 * pixel past the edge is clipped. the fonts are the 5" boards' 3/2 ones for now, so the writing
 * is two thirds of its size: a 9/4 bake of them is to come */
#define GFX_W 569
#define GFX_H 320
#define GFX_PW 1280
#define GFX_PH 720
#define GFX_SCALED 1
#define GFX_SCALE_NUM 9              // 9/4
#define GFX_SCALE_SHIFT 2
#define RADAR_RGB_BUS 1
#elif RADAR_DISPLAY == DISPLAY_4827S043_480x272
/* the 4.3" 480x272: a layout of its own, one pixel one pixel. the 2.8"'s choices where they go by the
 * height (the pages, the keyboard, the gear), as everything under 320 rows takes. its panel, 219 wide,
 * takes the wide panels' letters (PANEL_WIDE), in rows a little shorter */
#define GFX_W 480
#define GFX_H 272
#define RADAR_RGB_BUS 1
#else
#error "unknown RADAR_DISPLAY, see config.h"
#endif

/* the glass: on every other screen one pixel of the layout is one pixel of it */
#ifndef GFX_SCALED
#define GFX_SCALED 0
#define GFX_PW GFX_W
#define GFX_PH GFX_H
#endif
#ifndef RADAR_RGB_BUS
#define RADAR_RGB_BUS 0
#endif

/* the moving layer (fork addition): the planes, the sweep and the compass ring are not drawn into
 * the frame in memory but put over it as the panel is fed, every panel frame (firmware/main/
 * lcd_wave7.cpp). the frame in memory holds the rest and is drawn again only as time allows */
#if RADAR_DISPLAY == DISPLAY_WAVE7_800x480 || RADAR_DISPLAY == DISPLAY_WAVE7B_1024x600
#define RADAR_OVERLAY 1
#else
#define RADAR_OVERLAY 0
#endif

#ifndef RADAR_ROUND
#define RADAR_ROUND 0
#endif
#ifndef RADAR_EPAPER
#define RADAR_EPAPER 0
#endif

/* boards with touch. all of them go thru the 5 cross alignment once: the resistive ones
 * need it anyway, and on the round one it finds out how the touch chip's axes sit */
#define RADAR_TOUCH_CAL (RADAR_DISPLAY == DISPLAY_ILI9341_320x240 || RADAR_DISPLAY == DISPLAY_ST7796_480x320 || \
                         RADAR_DISPLAY == DISPLAY_GC9A01_240x240 || RADAR_DISPLAY == DISPLAY_ES3C28P_320x240 || \
                         RADAR_DISPLAY == DISPLAY_NMCYDC5_320x240 || RADAR_DISPLAY == DISPLAY_CROWADV28_320x240 || \
                         RADAR_DISPLAY == DISPLAY_4827S043_480x272 || GFX_SCALED)

/* the RPi LCD has no touch: BOOT does what it can there, the site sends the rest */
#define RADAR_HAS_TOUCH RADAR_TOUCH_CAL

/* the RGB LED: on three pins on the back of the 2.8" and the 3.5" CYD, a WS2812 on one pin on the
 * ES3C28P and the NM-CYD-C5 (firmware/main/board.h has the pins) */
#define RADAR_HAS_LED (RADAR_DISPLAY == DISPLAY_ILI9341_320x240 || RADAR_DISPLAY == DISPLAY_ST7796_480x320 || \
                       RADAR_DISPLAY == DISPLAY_ES3C28P_320x240 || RADAR_DISPLAY == DISPLAY_NMCYDC5_320x240)
/* the CYDs' red is on one of two pins and nothing tells which: the user picks (core/light.h) */
#define RADAR_LED_RED_PIN (RADAR_DISPLAY == DISPLAY_ILI9341_320x240 || RADAR_DISPLAY == DISPLAY_ST7796_480x320)

/* a BOOT button the firmware can read: every board but the Tab5, whose buttons go to its power
 * chip and its C6, not the P4, and the CrowPanel 7.0, whose BOOT is on the screen's clock line.
 * the pages say what BOOT does only where there is one */
#define RADAR_HAS_BUTTON (RADAR_DISPLAY != DISPLAY_TAB5_1280x720 && RADAR_DISPLAY != DISPLAY_CROW70_800x480 && \
                          !RADAR_OVERLAY)   // the Waveshare 7/7B: BOOT is GPIO0, a panel data line

#define RADAR_NAME "OWLOS FLIGHTS"

/* the board, as ABOUT says it */
#if RADAR_DISPLAY == DISPLAY_ILI9341_320x240
#define RADAR_BOARD "2.8\" CYD, 320x240"
#elif RADAR_DISPLAY == DISPLAY_ST7796_480x320
#define RADAR_BOARD "3.5\" CYD, ST7796 480x320"
#elif RADAR_DISPLAY == DISPLAY_ILI9486_480x320
#define RADAR_BOARD "3.5\" RPI LCD, ILI9486 480x320"
#elif RADAR_DISPLAY == DISPLAY_GC9A01_240x240
#define RADAR_BOARD "1.28\" ROUND, GC9A01"
#elif RADAR_DISPLAY == DISPLAY_ES3C28P_320x240
#define RADAR_BOARD "2.8\" ES3C28P, ESP32-S3"
#elif RADAR_DISPLAY == DISPLAY_NMCYDC5_320x240
#define RADAR_BOARD "2.8\" NM-CYD-C5, ESP32-C5"
#elif RADAR_DISPLAY == DISPLAY_CROWADV28_320x240
#define RADAR_BOARD "2.8\" CrowPanel Advance, S3"
#elif RADAR_DISPLAY == DISPLAY_CROWADV50_800x480
#define RADAR_BOARD "5\" CrowPanel Advance, S3"
#elif RADAR_DISPLAY == DISPLAY_JC8048W550_800x480
#define RADAR_BOARD "5\" Guition JC8048W550, S3"
#elif RADAR_DISPLAY == DISPLAY_TAB5_1280x720
#define RADAR_BOARD "5\" M5STACK TAB5, ESP32-P4"
#elif RADAR_DISPLAY == DISPLAY_4827S043_480x272
#define RADAR_BOARD "4.3\" ESP32-4827S043, S3"
#elif RADAR_DISPLAY == DISPLAY_CROW70_800x480
#define RADAR_BOARD "7\" CrowPanel 7.0, S3"
#elif RADAR_DISPLAY == DISPLAY_WAVE7_800x480
#define RADAR_BOARD "7\" Waveshare S3-Touch-LCD-7"
#elif RADAR_DISPLAY == DISPLAY_WAVE7B_1024x600
#define RADAR_BOARD "7\" Waveshare S3-Touch-LCD-7B"
#else
#define RADAR_BOARD "E-PAPER"
#endif
