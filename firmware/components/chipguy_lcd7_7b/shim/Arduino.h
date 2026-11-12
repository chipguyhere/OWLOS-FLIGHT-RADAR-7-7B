/*
 * The few Arduino calls the vendored chipguyhere 7/7B board and display libraries make, on plain
 * ESP-IDF. Not part of those libraries: it lets their sources be dropped in unchanged.
 * (OWLOS Flight Radar fork, Waveshare 7/7B port.)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#ifdef __cplusplus
#include <algorithm>
using std::max;   // as the Arduino core does
using std::min;
#endif

// The display header refuses to build without it; PSRAM is a hard requirement of these boards
// (sdkconfig.defaults.s3rgb), checked again when the framebuffers are allocated.
#if CONFIG_SPIRAM && !defined(BOARD_HAS_PSRAM)
#define BOARD_HAS_PSRAM 1
#endif

#define HIGH   1
#define LOW    0
#define INPUT  0x01
#define OUTPUT 0x03

static inline uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline uint32_t micros(void) { return (uint32_t)esp_timer_get_time(); }
static inline void delay(uint32_t ms) { vTaskDelay(ms ? (ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS : 0); }
static inline void delayMicroseconds(uint32_t us) { esp_rom_delay_us(us); }
static inline void yield(void) { taskYIELD(); }
static inline uint8_t lowByte(uint16_t w) { return (uint8_t)(w & 0xFF); }
static inline uint8_t highByte(uint16_t w) { return (uint8_t)(w >> 8); }

static inline void pinMode(int pin, int mode)
{
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, mode == OUTPUT ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
}
static inline void digitalWrite(int pin, int level) { gpio_set_level((gpio_num_t)pin, level ? 1 : 0); }
static inline int digitalRead(int pin) { return gpio_get_level((gpio_num_t)pin); }

// Serial: the libraries only print diagnostics, so it goes to the ESP-IDF console.
struct ShimSerial {
    template <typename... A> size_t printf(const char *f, A... a) { return (size_t)::printf(f, a...); }
    size_t print(const char *s) { return (size_t)::printf("%s", s); }
    size_t println(const char *s = "") { return (size_t)::printf("%s\n", s); }
};
extern ShimSerial Serial;
