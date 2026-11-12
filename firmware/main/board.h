/*
 * pins and parts of every board, picked by RADAR_DISPLAY (core/config.h).
 * wiring is the same as in ViewOwl's firmware for that display, change pins only here.
 * what is checked on hardware and what is not, is written next to each board.
 */
#pragma once
#include "config.h"

/* how the picture gets out:
 *   DISP_LCD_SPI   esp_lcd SPI panel IO, 8 bit commands (ILI9341, ST7796, GC9A01)
 *   DISP_LCD_RGB   a frame in the PSRAM on the S3's 16-bit RGB bus (the 5" boards, lcd_rgb.c)
 *   DISP_ILI9486   raw SPI, every byte padded to 16 bits (RPi LCD with shift registers)
 *   DISP_EPD       SSD1683 e-paper, bit-banged, 1 bit a pixel (core/paper.c)
 *   DISP_LCD_DSI   a frame in the PSRAM on the P4's MIPI-DSI, the panel standing (the Tab5, lcd_dsi.c)
 *   DISP_WAVE7     the Waveshare 7/7B through chipguyhere's library, the moving layer over it (lcd_wave7.c)
 * touch: TOUCH_XPT_BITBANG, TOUCH_XPT_SPI (on the LCD bus), TOUCH_CST816, TOUCH_FT6336, TOUCH_GT911,
 * TOUCH_BSP (the board package's, the Tab5), or none */

#if RADAR_DISPLAY == DISPLAY_ILI9341_320x240
/* ESP32-2432S028R "cheap yellow display". the one this was written on, all of it works */
#define DISP_LCD_SPI   1
#define LCD_CHIP_ILI9341 1
#define PIN_LCD_SCLK  14
#define PIN_LCD_MOSI  13
#define PIN_LCD_MISO  (-1)      // write only, leave strapping pin 12 alone
#define PIN_LCD_DC     2
#define PIN_LCD_CS    15
#define PIN_LCD_RST   (-1)      // tied high, so software reset only
#define PIN_LCD_BL    21        // backlight, PWM
#define LCD_SPI_HZ    (40 * 1000 * 1000)   // 80 MHz APB / 2. ViewOwl asks for 50, the divider ends up here anyway
#define LCD_MADCTL    0xE8      // landscape for this panel: MY|MX|MV|BGR, same as ViewOwl
#define DISP_STRIP    24        // rows a strip: 240 / 24 = 10 strips
/* XPT2046 has its own pins here, we bit-bang it, cheap and the LCD bus stays free.
 * 36 and 39 are input only pins */
#define TOUCH_XPT_BITBANG 1
#define PIN_T_CLK     25
#define PIN_T_MOSI    32
#define PIN_T_MISO    39
#define PIN_T_CS      33
#define PIN_T_IRQ     36
#define PIN_BUTTON     0        // BOOT
// TODO: board speaker for geiger like clicks while hunting
/* the same name is sold in three revisions with the same pins and three different screens
 * (rzeldent/platformio-espressif32-sunton, esp32-2432S028R / Rv2 / Rv3):
 *   v1  micro USB      ILI9341, BGR, our MADCTL 0xE8, no inversion. the one this was written on
 *   v2  USB-C          ILI9341 with its own power and gamma values, colour inversion on
 *   v3  "7789"         ST7789, RGB, mirrored on one axis against v1, inversion on
 * nobody knows which one he has, so all three are in this build (lcd_spi.c, LCD_PROFILES; the
 * list keeps v1 first, boards save its index). a board that never picked one starts as v2: most
 * of them sold are v2. BOOT goes v2 -> v1 -> v3 and round again, see main.c */
#define LCD_PROFILES   3
#define LCD_ORDER      { 1, 0, 2 }

#elif RADAR_DISPLAY == DISPLAY_ST7796_480x320
/* ESP32-3248S035 (3.5" ST7796), ViewOwl's 480x320 build. runs (2026-10-13): ~8 fps, 24 KB
 * heap left. the touch is the R version, XPT2046 on the LCD bus (CS 33), works */
#define DISP_LCD_SPI   1
#define LCD_CHIP_ST7796 1
#define PIN_LCD_SCLK  14
#define PIN_LCD_MOSI  13
#define PIN_LCD_MISO  12        // only the touch reads it
#define PIN_LCD_DC     2
#define PIN_LCD_CS    15
#define PIN_LCD_RST   (-1)
#define PIN_LCD_BL    27
#define LCD_SPI_HZ    (40 * 1000 * 1000)
#define LCD_MADCTL    0xE8      // MY|MX|MV|BGR, ViewOwl lcd_init.c
#define DISP_STRIP    8         // 480 wide and no PSRAM: low strips, the map's picture needs the heap
/* twice the 2.8"'s pixels on the same chip: in the radar look the second core draws every other
 * strip (main.c). the square map leaves it be, there the SPI is what it waits for */
#define DISP_HELPER   1
#define TOUCH_XPT_SPI 1
#define PIN_T_CS      33
#define PIN_T_IRQ     36
#define PIN_BUTTON     0
/* the C version (ESP32-3248S035C) has the same screen and a capacitive GT911 on I2C instead,
 * SDA 33 SCL 32 (rzeldent/platformio-espressif32-sunton). touch.c asks for it first and gives
 * the pins back when nobody answers: on the R version 33 is the XPT2046's chip select.
 * NOT run on a C version here */
#define TOUCH_GT911_TOO 1
#define PIN_I2C_SDA   33
#define PIN_I2C_SCL   32

#elif RADAR_DISPLAY == DISPLAY_ILI9486_480x320
/* Waveshare RPi LCD (A) 3.5" on the OWLOSAirQuality board, ViewOwl's ILI9486 build.
 * NOT run on hardware yet. no touch wired that ViewOwl knows of, BOOT button only */
#define DISP_ILI9486   1
#define PIN_LCD_SCLK  18
#define PIN_LCD_MOSI  23
#define PIN_LCD_MISO  19
#define PIN_LCD_DC     2
#define PIN_LCD_CS    15        // driven by hand, CS has to go up after every padded byte
#define PIN_LCD_RST    4
#define LCD_SPI_HZ    (20 * 1000 * 1000)   // the 74HC shift registers on the board top out here
#define LCD_MADCTL    0x28      // MV|BGR, ViewOwl's sweep found this one
#define DISP_STRIP    8         // as the 3.5" CYD: the map's picture needs the heap
#define PIN_BUTTON     0

#elif RADAR_DISPLAY == DISPLAY_GC9A01_240x240
/* Elecrow CrowPanel 1.28" round, ESP32-C3 (4 MB on the one here). ViewOwl's C3 build.
 * runs (2026-10-13): ~8 fps, 31 KB heap left. reset and backlight are on a PI4IOE5V6408
 * I2C expander, not on GPIOs. the touch chip is not named anywhere in ViewOwl, CST816 at
 * 0x15 was a guess and it answered; touch.c still goes on without touch if nothing does */
#define DISP_LCD_SPI   1
#define LCD_CHIP_GC9A01 1
#define PIN_LCD_SCLK   6
#define PIN_LCD_MOSI   7
#define PIN_LCD_MISO  (-1)
#define PIN_LCD_DC     2
#define PIN_LCD_CS    10
#define PIN_LCD_RST   (-1)      // on the expander
#define LCD_SPI_HZ    (80 * 1000 * 1000)   // C3 IOMUX pins, ViewOwl runs 80. snow on screen = go to 40
#define LCD_MADCTL    0x68      // MV|MX|BGR, ViewOwl's lcd_gc9a01.c (0x28 came out mirrored)
#define DISP_STRIP    16        // 15 strips: the C3's heap is small, room for the map and TLS
#define PIN_I2C_SDA    4
#define PIN_I2C_SCL    5
#define IOEX_ADDR      0x43
#define IOEX_BIT_BL    (1 << 2)
#define IOEX_BIT_TRST  (1 << 3)  // touch reset
#define IOEX_BIT_RST   (1 << 4)  // LCD reset
#define TOUCH_CST816   1
#define CST816_ADDR    0x15
#define PIN_BUTTON     9        // BOOT on the C3

#elif RADAR_DISPLAY == DISPLAY_ES3C28P_320x240
/* LCDWIKI ES3C28P, 2.8": ESP32-S3 N16R8, ILI9341V 240x320 IPS, FT6336G capacitive touch. pins
 * from LCDWIKI's pin table and schematic (lcdwiki.com/2.8inch_ESP32-S3_Display), the same in
 * Bruce's port (BruceDevices/firmware, boards/ES3C28P, said to be run on one). NOT run on one
 * here: a viewer asked for it and has one. its USB-C is the S3's own USB, no bridge chip: the
 * site talks to it there (console.c) */
#define DISP_LCD_SPI   1
#define LCD_CHIP_ILI9341 1
#define PIN_LCD_SCLK  12
#define PIN_LCD_MOSI  11
#define PIN_LCD_MISO  13
#define PIN_LCD_DC    46
#define PIN_LCD_CS    10
#define PIN_LCD_RST   (-1)      // on the chip's own reset
#define PIN_LCD_BL    45        // high = lit
#define LCD_SPI_HZ    (40 * 1000 * 1000)   // what LCDWIKI's and Bruce's setups use
#define LCD_MADCTL    0x28      // MV|BGR: Bruce's landscape. the other one is the screen turned over
#define DISP_STRIP    24
/* BGR with the colours inverted, in LCDWIKI's own start-up and in both ports: the CYD's v2 is
 * that. the CYD's three start-ups stay in the build all the same, BOOT in the first seconds goes
 * through them as on the CYD, in case this panel is another */
#define LCD_PROFILES   3
#define LCD_ORDER      { 1, 0, 2 }
#define TOUCH_FT6336   1
#define FT6336_ADDR    0x38
#define PIN_I2C_SDA   16
#define PIN_I2C_SCL   15
#define PIN_T_RST     18
#define PIN_BUTTON     0        // BOOT
#define PIN_LED_WS2812 42       // one WS2812B, green red blue (Bruce's order)

#elif RADAR_DISPLAY == DISPLAY_NMCYDC5_320x240
/* NM-CYD-C5, 2.8" in the CYD's form: ESP32-C5-WROOM-1 N16R8, ST7789 320x240, XPT2046 resistive.
 * pins from its maker's schematic SCH_NM-CYD-C5-v1.0 (RockBase-iot/NM-CYD-C5, gone from GitHub,
 * kept in its forks), the same in Bruce's port (boards/nm-cyd-c5) and in other projects on it.
 * NOT run on one here: a viewer asked for it and has one. two USB-C: one through a CH340 to
 * UART0, one the C5's own USB, the site talks to it on either (console.c). the screen, the touch
 * and the SD card share one SPI bus */
#define DISP_LCD_SPI   1
#define LCD_CHIP_ST7789 1
#define PIN_LCD_SCLK   6
#define PIN_LCD_MOSI   7
#define PIN_LCD_MISO   2        // the touch reads it
#define PIN_LCD_DC    24
#define PIN_LCD_CS    23
#define PIN_LCD_RST   (-1)      // on the chip's own reset
#define PIN_LCD_BL    25        // high = lit
#define PIN_SD_CS     10        // the SD slot on the same bus: held up, so a card in it keeps quiet
/* the maker's demos run 20 MHz, one project saw stripes at 40 on this shared bus and runs 30,
 * another runs 40. 30 asked, the SPI driver takes the nearest it can make below */
#define LCD_SPI_HZ    (30 * 1000 * 1000)
#define LCD_MADCTL    0xA0      // MY|MV, RGB: the landscape most of them use, the other is the screen turned over
#define DISP_STRIP    24
/* no inversion, every source agrees. RGB or BGR they do not: RGB in the maker's Arduino setup and
 * in most ports, BGR in its MicroPython demo. both are in the build, RGB first, BOOT in the first
 * seconds switches (lcd_spi.c) */
#define LCD_PROFILES   2
#define LCD_ORDER      { 0, 1 }
#define TOUCH_XPT_SPI  1
#define PIN_T_CS       1
#define PIN_T_IRQ      3        // on the schematic; most ports ask the chip instead, touch.c finds out
#define PIN_BUTTON    28        // BOOT
#define PIN_LED_WS2812 27       // one WS2812C behind a level shifter, green red blue (Bruce's order)

#elif RADAR_DISPLAY == DISPLAY_CROWADV28_320x240
/* Elecrow CrowPanel Advance 2.8" HMI, ESP32-S3-WROOM-1 N16R8, ST7789 240x320 IPS, FT6336U
 * capacitive touch. pins from Elecrow's repository (Elecrow-RD/CrowPanel-Advance-2.8-HMI-ESP32-AI-
 * Display-320x240): its LovyanGFX setup in example/V1.0/Arduino_Code/lesson-03, checked against
 * the nets of its V1.2 schematic, the same from V1.0 to V1.2. NOT run on one here: two viewers
 * asked for it. two USB-C: one through a CH340K to UART0, one the S3's own USB, the site talks
 * to it on either (console.c). the screen has its bus to itself, the SD card is on other pins */
#define DISP_LCD_SPI   1
#define LCD_CHIP_ST7789 1
#define PIN_LCD_SCLK  42
#define PIN_LCD_MOSI  39
#define PIN_LCD_MISO  (-1)      // not wired, the screen is only written
#define PIN_LCD_DC    41
#define PIN_LCD_CS    40
#define PIN_LCD_RST   (-1)      // a reset circuit of its own on the board
#define PIN_LCD_BL    38        // high = lit, through a transistor
/* Elecrow runs 80 MHz; these pins go through the GPIO matrix, 40 is what the other S3 runs */
#define LCD_SPI_HZ    (40 * 1000 * 1000)
#define LCD_MADCTL    0xA0      // MY|MV: Elecrow's landscape (LovyanGFX rotation 3), the other is the screen turned over
#define DISP_STRIP    24
/* an IPS panel: colours inverted, as Elecrow's setup has it (invert = true). BGR there too
 * (rgb_order = false is LovyanGFX's BGR bit), so BGR first, RGB behind BOOT in case */
#define LCD_INVERT     1
#define LCD_PROFILES   2
#define LCD_ORDER      { 1, 0 }
#define TOUCH_FT6336   1
#define FT6336_ADDR    0x38
#define PIN_I2C_SDA   15        // the touch and the I2C socket on the side
#define PIN_I2C_SCL   16
#define PIN_T_RST     48        // TP_RST on the schematic; its TP_INT is 47, touch.c asks the chip instead
#define PIN_BUTTON     0        // BOOT

#elif RADAR_DISPLAY == DISPLAY_CROWADV50_800x480
/* Elecrow CrowPanel Advance 5.0" HMI (DIS02050A), ESP32-S3-WROOM-1 N16R8, 800x480 IPS on the RGB bus
 * (ST7262), GT911 capacitive touch. pins from Elecrow's repository (Elecrow-RD/CrowPanel-Advance-5-
 * HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480): its LovyanGFX setups for V1.0 and V1.2, the
 * same but for the clock, 21 MHz and 16; 16 in the V1.2 and V1.3 examples, Arduino and IDF. a
 * viewer's V1.3 runs it (2026-11-05): picture, touch, 3 fps on the radar with one core. V1.0 to
 * V1.2 not seen yet. the backlight and the touch's reset are behind a chip of the board's own,
 * lcd_rgb.c. its speaker, microphone and SD card are on pins shared through a switch on the board,
 * none of them is driven here */
#define DISP_LCD_RGB   1
#define LCD_RGB_DATA   21, 47, 48, 45, 38,  9, 10, 11, 12, 13, 14,  7, 17, 18, 3, 46   // blue, green, red, lowest bit first
#define PIN_LCD_HSYNC 40
#define PIN_LCD_VSYNC 41
#define PIN_LCD_DE    42
#define PIN_LCD_PCLK  39
#define LCD_RGB_HZ    (16 * 1000 * 1000)
#define LCD_BOARD_CHIP 1
#define DISP_STRIP    12        // 16 left no room in the RAM for the second core's strip, on a V1.3
#define DISP_HELPER   1
#define LCD_PROFILES   2
#define LCD_ORDER      { 0, 1 }
#define TOUCH_GT911    1
#define PIN_I2C_SDA   15        // the touch, the board's own chip, a clock, the I2C socket
#define PIN_I2C_SCL   16
#define PIN_T_INT      1        // the touch's reset is on the board's own chip, this line only helps it wake
#define PIN_BUTTON     0        // BOOT

#elif RADAR_DISPLAY == DISPLAY_JC8048W550_800x480
/* Guition JC8048W550C, 5": ESP32-S3 N16R8, 800x480 on the RGB bus (ST7262), GT911, a CH340. pins from
 * one source so far, rzeldent/platformio-espressif32-sunton, JC8048W550C.json: the same as its
 * ESP32-8048S050C. that file calls the low five data lines red, and every other setup for those
 * pins has them blue: the bus's low five are blue, so these go in as listed. NOT run on one:
 * experimental, a viewer asked for it. a version without touch is sold too */
#define DISP_LCD_RGB   1
#define LCD_RGB_DATA   8, 3, 46, 9, 1,  5, 6, 7, 15, 16, 4,  45, 48, 47, 21, 14   // blue, green, red, lowest bit first
#define PIN_LCD_HSYNC 39
#define PIN_LCD_VSYNC 41
#define PIN_LCD_DE    40
#define PIN_LCD_PCLK  42
#define LCD_RGB_HZ    (16 * 1000 * 1000)
#define PIN_LCD_BL     2        // high = lit
#define DISP_STRIP    12        // as the CrowPanel's
#define DISP_HELPER   1
#define LCD_PROFILES   2
#define LCD_ORDER      { 0, 1 }
#define TOUCH_GT911    1
#define PIN_I2C_SDA   19        // the S3's own USB pins: this board has a CH340, they are free
#define PIN_I2C_SCL   20
#define PIN_T_RST     38
#define PIN_T_INT     18
#define NO_CHIP_USB    1
#define PIN_BUTTON     0        // BOOT

#elif RADAR_DISPLAY == DISPLAY_CROW70_800x480
/* Elecrow CrowPanel 7.0" HMI (DIS08070H), the plain one: ESP32-S3-WROOM-1 N4R8, 800x480 TN on the RGB
 * bus, GT911, a CH340C. pins and timing from Elecrow's repository (Elecrow-RD/CrowPanel-7.0-HMI-ESP32-
 * Display-800x480): its V3.0 examples, Arduino with LovyanGFX, and its hardware guide from the V3.0
 * schematic. a second source, an ESPHome setup said to work well (Home Assistant's forum, "Elecrow
 * CrowPanel 7" with ESPHome"): the same pins, 15 MHz, the sync polarity esp_lcd's own. the clock is
 * 15 MHz in one of Elecrow's examples and 24 in the other, the rows and frames round it the same in
 * both: 16 here, as the 5" boards run it out of the PSRAM. BOOT is on GPIO 0, the panel's clock: no
 * button the firmware can read, and pressing it while the screen runs shorts the clock. a PCA9557 at
 * 0x18 has the touch's reset (touch.c). an owner's V3.0 ran the CrowPanel Advance 5" build: WiFi and
 * planes, the screen dark. this one NOT run on one yet: experimental, its owner tests it */
#define DISP_LCD_RGB   1
#define LCD_RGB_DATA   15, 7, 6, 5, 4,  9, 46, 3, 8, 16, 1,  14, 21, 47, 48, 45   // blue, green, red, lowest bit first
#define PIN_LCD_HSYNC 39
#define PIN_LCD_VSYNC 40
#define PIN_LCD_DE    41
#define PIN_LCD_PCLK   0
#define LCD_RGB_HZ    (16 * 1000 * 1000)
#define LCD_RGB_H_PULSE 48
#define LCD_RGB_H_BACK  40
#define LCD_RGB_H_FRONT 40
#define LCD_RGB_V_PULSE 31
#define LCD_RGB_V_BACK  13
#define LCD_RGB_V_FRONT  1
#define PIN_LCD_BL     2        // high = lit, an MT9201's enable
#define DISP_STRIP    12        // as the 5" boards'
#define DISP_HELPER   1
#define LCD_PROFILES   2
#define LCD_ORDER      { 0, 1 }
#define TOUCH_GT911    1
#define TOUCH_RESET_PCA9557 0x18 // only when the GT911 does not answer: touch.c
#define PIN_I2C_SDA   19        // the S3's own USB pins: this board has a CH340C, they are free
#define PIN_I2C_SCL   20
#define NO_CHIP_USB    1
#define PIN_BUTTON   (-1)       // BOOT is the panel's clock, see above

#elif RADAR_DISPLAY == DISPLAY_WAVE7_800x480 || RADAR_DISPLAY == DISPLAY_WAVE7B_1024x600
/* Waveshare ESP32-S3-Touch-LCD-7 (800x480, ESP32-S3-WROOM-1 N8R8) and -7B (1024x600, N16R8). fork
 * addition: driven by chipguyhere's 7/7B library, vendored in components/chipguy_lcd7_7b, which owns
 * the panel, its pins and timing, and the board's I2C expander (panel reset and supply, backlight,
 * the GT911's reset). lcd_wave7.c. the panel is fed from the PSRAM through two bounce buffers by
 * the library's fill task, and the moving layer (ovl.c) is put over each chunk as it lands.
 * one library for both boards, but the layout is built for one glass: each has its own build, and
 * the wrong one says so in the log. a -7B ran it here (2026-11-11); the -7 not yet */
#define DISP_WAVE7     1
#define DISP_STRIP     4        // rows a strip of the background, drawn on the second core (main.c bg_task): 16 KB of RAM
#define TOUCH_GT911    1        // its reset goes through the expander: lcd_wave7.c does it before tp_init
#define PIN_I2C_SDA    8        // the expander, the touch, the I2C header
#define PIN_I2C_SCL    9
#define PIN_BUTTON   (-1)       // BOOT is GPIO0, a panel data line

#elif RADAR_DISPLAY == DISPLAY_4827S043_480x272
/* ESP32-4827S043C, 4.3": ESP32-S3 N16R8, 480x272 on the RGB bus, GT911, a CH340. pins and timing from
 * rzeldent/platformio-espressif32-sunton, esp32-4827S043C.json: the pins are the JC8048W550's, and an
 * owner's 4827S043 showed that build's picture, too big for its glass. the panel's timing is its
 * own: 8 MHz, 43 clocks before a row and 12 rows before a frame. NOT run on one: experimental, its
 * owner tests it. the R version has a resistive XPT2046 instead, not in this build */
#define DISP_LCD_RGB   1
#define LCD_RGB_DATA   8, 3, 46, 9, 1,  5, 6, 7, 15, 16, 4,  45, 48, 47, 21, 14   // blue, green, red, lowest bit first
#define PIN_LCD_HSYNC 39
#define PIN_LCD_VSYNC 41
#define PIN_LCD_DE    40
#define PIN_LCD_PCLK  42
#define LCD_RGB_HZ    (8 * 1000 * 1000)
#define LCD_RGB_H_BACK 43
#define LCD_RGB_V_BACK 12
#define LCD_RGB_BOUNCE 8        // rows a bounce buffer: 272 divides by it, not by the 5"'s 10
#define PIN_LCD_BL     2        // high = lit
#define DISP_STRIP    16        // 17 strips
#define DISP_HELPER   1
#define LCD_PROFILES   2
#define LCD_ORDER      { 0, 1 }
#define TOUCH_GT911    1
#define PIN_I2C_SDA   19        // the S3's own USB pins: this board has a CH340, they are free
#define PIN_I2C_SCL   20
#define PIN_T_RST     38
#define PIN_T_INT     18
#define NO_CHIP_USB    1
#define PIN_BUTTON     0        // BOOT

#elif RADAR_DISPLAY == DISPLAY_TAB5_1280x720
/* M5Stack Tab5: ESP32-P4 (16 MB flash, 32 MB PSRAM), a 5" 720x1280 IPS panel standing on MIPI-DSI,
 * capacitive touch, Wi-Fi through an ESP32-C6 on SDIO (sdkconfig.defaults.esp32p4). three revisions
 * of the panel exist (ILI9881C with a GT911, ST7123, ST7121 with its own touch): Espressif's board
 * package (espressif/m5stack_tab5, main/idf_component.yml) tells them apart at power on and brings
 * up the one it finds, so no panel or touch pins are named here, and the I2C bus is its. no BOOT
 * button reaches the P4: the power button is the charger chip's. no LED. written on one with the
 * ST7121 (board V1.0), lcd_dsi.c */
#define DISP_LCD_DSI   1
#define DISP_STRIP    12        // 60 strips: at 1280 wide the strip, its turned copy and the two cores' canvases have to share 346 KB of RAM
#define DISP_HELPER   1
#define TOUCH_BSP      1        // the package's esp_lcd_touch, whichever chip it found
#define PIN_BUTTON    (-1)      // none

#elif RADAR_DISPLAY == DISPLAY_EPD_400x300 || RADAR_DISPLAY == DISPLAY_EPD_792x272
/* Elecrow CrowPanel e-paper 4.2" (400x300) and 5.79" (792x272), ESP32-S3. NOT run on
 * hardware yet: panel pins and command sequences are ViewOwl's (ViewOwl.ESP32_EPD.Client),
 * the dial and key pins come from Elecrow's 5.79 key example, the 4.2 is assumed the same */
#define DISP_EPD       1
#define EPD_PIN_SCK   12
#define EPD_PIN_MOSI  11
#define EPD_PIN_RES   47
#define EPD_PIN_DC    46
#define EPD_PIN_CS    45
#define EPD_PIN_BUSY  48
#define EPD_PIN_PWR    7        // panel power, has to be high
#define DISP_STRIP     8        // 792 x 24 rows x 4 bytes was 76 KB in one piece, not there after wifi. paper is slow anyway
#define PIN_KEY_UP     6        // dial up
#define PIN_KEY_DOWN   4        // dial down
#define PIN_KEY_OK     5        // dial pressed
#define PIN_KEY_MENU   2
#define PIN_KEY_BACK   1
#define PIN_BUTTON     0

#endif

#ifndef PIN_LCD_BL
#define PIN_LCD_BL (-1)
#endif

/* the RGB LED on the back of the 2.8" and the 3.5" CYD, each colour pulled to ground = lit. the same
 * pins on both (rzeldent/platformio-espressif32-sunton); on the 2.8" the Thermo Relay drives them.
 * red is GPIO 4 there and GPIO 22 on LCDWIKI's ESP32-32E boards (lcdwiki.com, E32R28T, E32R35T),
 * whose GPIO 4 switches the speaker's amplifier: the user picks, core/light.h LIGHT_RED_.
 * the 3.5" here is one of those (2026-11-03: red lit on 22 alone). the boards with a WS2812 name
 * its one pin above, PIN_LED_WS2812 */
#if RADAR_HAS_LED && !defined(PIN_LED_WS2812)
#define PIN_LED_G     16
#define PIN_LED_B     17
#endif
#ifndef DISP_HELPER
#define DISP_HELPER 0
#endif

// raw touch units across the screen, for the sanity check of a stored alignment
#if TOUCH_CST816
#define TOUCH_RAW_SPAN 240
#elif TOUCH_FT6336
#define TOUCH_RAW_SPAN 320      // it counts screen pixels, 240x320 standing
#elif TOUCH_GT911
#define TOUCH_RAW_SPAN GFX_PW   // so does this one, lying down
#elif TOUCH_BSP
#define TOUCH_RAW_SPAN GFX_PW   // the Tab5's counts the standing glass's pixels, 720 x 1280: the long way is the picture's width
#else
#define TOUCH_RAW_SPAN 3700
#endif
