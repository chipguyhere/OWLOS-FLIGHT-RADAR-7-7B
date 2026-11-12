#pragma once

// Board abstraction for the Waveshare ESP32-S3-Touch-LCD-7 and -7B.
//
// The two boards share the ESP32-S3 module's wiring for the RGB panel, the
// I2C bus (SDA 8, SCL 9), the GT911's INT line (GPIO 4) and the expander pin
// numbers below.  They differ in the I2C I/O expander that carries the
// panel reset, backlight enable, touch reset, SD card CS and USB/CAN switch:
//
//   -7    a CH422G.  It has no registers: each function is its own I2C
//         address taking one byte -- 0x24 system (bit0 = IO0..7 are
//         outputs), 0x38 IO0..7 output latches, 0x26 IO0..7 inputs, 0x23
//         the open-drain OC0..3 outputs.  No PWM, no ADC.
//   -7B   a small microcontroller (U10) running Waveshare's "IO extension"
//         firmware at 0x24 alone, with registers:
//           reg 0x02  mode     write: one bit per EXIO pin, 1 = output
//           reg 0x03  output   write: the eight EXIO output latches
//           reg 0x04  input    read:  the eight EXIO pin levels
//           reg 0x05  PWM      write: backlight PWM duty, 0..255 (inverted)
//           reg 0x06  ADC      read:  battery ADC, 16-bit LE, 10-bit range
//
// Both answer at 0x24, and a write there means different things to each (on
// the CH422G it is the system register, where the 7B's register writes would
// set mode bits other than IO_OE).  So begin() identifies the board BEFORE
// writing anything: an address-only probe of 0x38, which only the CH422G
// answers (TODO (bench): confirm on both boards -- a -7 answers 0x38 and the
// 7B's expander doesn't).
// Define LCD7_7B_FORCE_MODEL to 7 or 0x7B to skip detection.
//
// The expander, the GT911 and anything on the I2C header share one bus, so it
// is wrapped in a mutex-protected ThreadsafeWire.
//
// The ThreadsafeWire wrapper lives in chipguy_ThreadsafeWire.h -- a verbatim copy
// of the one in chipguy_Board, vendored so this library depends on nothing but
// Wire.  Its unit of locking is the TRANSACTION, so an ordinary
// beginTransmission/write/endTransmission sequence is already atomic against
// every other task, with no Guard of its own.
//
// Merged from the chipguy -7 and -7B board libraries, 2026-11-03.
//
// Copyright (c) 2025-2026 chipguyhere
// MIT License

#include <Arduino.h>
#include <Wire.h>

// The shared-bus wrapper, vendored from chipguy_Board so this library depends on
// nothing but Wire. Kept in its own header so device drivers can be written
// against ThreadsafeWire without including this board class. Copy it forward
// when chipguy_Board's copy changes; do not fork it.
#include "chipguy_ThreadsafeWire.h"

// -7 only: hold GPIO16 (RS-485 TX) low from begin() so the 5 V RS-485
// transceiver stops driving GPIO15 to 5 V (see setRs485Guard).  Define as 0 to
// make the guard off by default.
#ifndef LCD7_7B_RS485_GUARD
#define LCD7_7B_RS485_GUARD 1
#endif


class ESP32S3_Touch_LCD_7_7B_Board {
public:
    // Which board this is.  The values read as the board names.
    enum Model : uint8_t {
        MODEL_UNKNOWN = 0,     // begin() not called yet, or no expander answered
        MODEL_7       = 0x07,  // ESP32-S3-Touch-LCD-7:  800x480, CH422G expander
        MODEL_7B      = 0x7B,  // ESP32-S3-Touch-LCD-7B: 1024x600, IO-extension MCU
    };

    // Shared I2C bus pins (both boards).
    static constexpr int SDA_PIN = 8;
    static constexpr int SCL_PIN = 9;

    // -7: CH422G command addresses (7-bit).  Not register indices -- each is
    // a distinct I2C address that takes a single data byte.
    static constexpr uint8_t CH422G_WR_SET = 0x24;   // system register
    static constexpr uint8_t CH422G_WR_OC  = 0x23;   // OC0..3 (open drain)
    static constexpr uint8_t CH422G_WR_IO  = 0x38;   // IO0..7 outputs
    static constexpr uint8_t CH422G_RD_IO  = 0x26;   // IO0..7 inputs

    // -7B: the IO-extension MCU's I2C address and registers.
    static constexpr uint8_t EXP_ADDR     = 0x24;
    static constexpr uint8_t EXP_REG_MODE = 0x02;
    static constexpr uint8_t EXP_REG_OUT  = 0x03;
    static constexpr uint8_t EXP_REG_IN   = 0x04;
    static constexpr uint8_t EXP_REG_PWM  = 0x05;
    static constexpr uint8_t EXP_REG_ADC  = 0x06;

    // Expander pin assignments (0-based EXIO0..EXIO7 / CH422G IO0..IO7).  The
    // same on both boards (the -7's from its V1.2 schematic, 2025-11-06).  The
    // -7's CH422G also has OC0..OC3 outputs (unused here).
    static constexpr uint8_t EXIO_TOUCH_RST  = 1;   // GT911 reset, active low
    static constexpr uint8_t EXIO_LCD_BL     = 2;   // backlight enable, active high
    static constexpr uint8_t EXIO_LCD_RST    = 3;   // panel reset, active low
    static constexpr uint8_t EXIO_SD_CS      = 4;   // microSD chip select, active low
    static constexpr uint8_t EXIO_USB_SEL    = 5;   // GPIO19/20 LOW = USB-C port, HIGH = CAN
    static constexpr uint8_t EXIO_LCD_VDD_EN = 6;   // panel bias supply enable, active high
    // EXIO0 and EXIO7 are not connected on either board (the -7's CH422G
    // also leaves OC0..OC3 unconnected).

    // Real GPIOs that belong to board-level functions.  The same on both
    // boards (the module's pins carry the same nets on both schematics; see
    // schematic-description-7.md / -7B.md).  -7: its RS-485 transceiver runs
    // from 5 V, and may drive RS485_RX_PIN (GPIO15) toward 5 V -- errata 21.
    static constexpr int TOUCH_INT_PIN = 4;     // GT911 INT; also selects its address at reset
    static constexpr int SD_MOSI_PIN   = 11;
    static constexpr int SD_SCK_PIN    = 12;
    static constexpr int SD_MISO_PIN   = 13;
    static constexpr int RS485_TX_PIN  = 16;    // auto-direction transceiver (the 7B
    static constexpr int RS485_RX_PIN  = 15;    // sheet's net labels have these swapped)
    static constexpr int CAN_TX_PIN    = 20;    // only when selectCan(true)
    static constexpr int CAN_RX_PIN    = 19;
    static constexpr int SENSOR_PIN    = 6;     // the 3-pin header ("GPIO" on the 7B, "Sensor AD" on the -7)

    ESP32S3_Touch_LCD_7_7B_Board();

    // Bring up the shared I2C bus, identify the board (see the top of this
    // file), and put its expander in a known state: resets de-asserted, SD
    // CS high, panel supply on (7B), backlight OFF so the display can draw a
    // first frame before it is revealed.  Idempotent: the display and touch
    // drivers both call it, only the first does the work.  Returns false if
    // no expander answers; model() is then MODEL_UNKNOWN.
    bool begin();

    // The board found by begin().
    Model model() const { return _model; }
    bool is7B() const { return _model == MODEL_7B; }
    const char* modelName() const {
        return _model == MODEL_7B ? "ESP32-S3-Touch-LCD-7B"
             : _model == MODEL_7  ? "ESP32-S3-Touch-LCD-7" : "unknown";
    }
    // The panel's native resolution (0 until begin() has identified the board).
    uint16_t panelWidth() const  { return _model == MODEL_7B ? 1024 : _model == MODEL_7 ? 800 : 0; }
    uint16_t panelHeight() const { return _model == MODEL_7B ? 600  : _model == MODEL_7 ? 480 : 0; }

    // The shared, mutex-protected I2C bus.  Use it directly for any other I2C
    // peripheral on the board: every transaction locks itself, so a Guard is
    // needed only to make SEVERAL transactions atomic as a group.
    ThreadsafeWire Wire;

    // --- Display control lines (on the expander) ---
    void lcdReset();              // pulse the panel reset low->high (EXIO3)
    void lcdBacklight(bool on);   // backlight enable (EXIO2)
    // Panel bias supply enable (EXIO6).  On at begin().  Returns false only
    // when no board was identified.
    bool lcdPower(bool on);

    // Backlight brightness, 0..100 %.  7B only: the expander's PWM output pulls
    // on the backlight converter's feedback node, so its duty is inverted (more
    // duty = dimmer); this takes care of that.  0 does not reach black -- use
    // lcdBacklight(false) for off.  The -7's backlight is on/off only: returns
    // false and does nothing.
    bool setBacklightBrightness(uint8_t percent);
    bool hasBacklightPwm() const { return _model == MODEL_7B; }
    // The raw PWM register (7B), for anyone who wants the hardware's own scale.
    bool setBacklightPwmRaw(uint8_t duty);

    // --- GT911 touch control ---
    // Full power-on reset: holds INT low while pulsing the touch reset (EXIO1),
    // which latches the controller at I2C address 0x5D, then leaves INT as an
    // input.
    void touchReset();

    // --- Other board functions ---
    // GPIO19/20 go either to the second USB-C port (ESP32 native USB) or to
    // the CAN transceiver, through a switch on EXIO5.  false = USB (the
    // power-on default, and begin()'s), true = CAN.  Returns false only when
    // no board was identified.
    bool selectCan(bool can);
    // microSD chip select (EXIO4), for SPI-mode drivers that take a callback
    // or for manual framing.  Active low.
    void sdChipSelect(bool selected);

    // --- -7 RS-485 guard (errata 21) ---
    // The -7's SP3485 runs from 5 V, and while it is receiving its RO output
    // holds GPIO15 at 5 V -- above the ESP32-S3's rating (measured).  Holding
    // GPIO16 (RS485_TX_PIN) low switches the transceiver to transmit, which
    // tri-states RO and lets GPIO15 fall to 3.3 V (measured), at the cost of
    // the driver actively driving the RS-485 line.
    //
    // On by default on the -7 (LCD7_7B_RS485_GUARD): begin() applies it.  A
    // sketch that opens the port with Serial1.begin(..., RS485_RX_PIN,
    // RS485_TX_PIN) takes GPIO16 over and so lifts the guard -- GPIO15 is then
    // at 5 V whenever the port receives, which RS-485 use can't avoid on this
    // board.  Call setRs485Guard(false) -- before lv_setup.begin() or
    // board.begin(), or any time after -- to release GPIO16 instead, e.g. for a
    // -7 wired to a live bus that it won't open the port on (the guard would
    // hold that bus in one state).  No-op on the 7B, whose transceiver runs at
    // 3.3 V.  Returns whether the guard is now holding GPIO16 low.
    bool setRs485Guard(bool on);
    bool rs485Guard() const { return _rs485GuardOn; }

    // --- Battery (7B) ---
    // The raw expander ADC reading (10-bit range, 3.3 V reference) of VBAT
    // through a 20k/10k divider.  0 on the -7: it has a charger (U4) but its
    // CH422G has no ADC, so the board can't read the battery.
    uint16_t batteryRaw();
    // The same, in volts at the battery connector.  With no battery fitted this
    // reads whatever the charger is driving onto the empty connector, not 0.
    // NAN on the -7.
    float batteryVoltage();
    bool hasBatteryAdc() const { return _model == MODEL_7B; }

    // --- Escape hatch: generic expander pin control ---
    // io is 0..7 (EXIO0..EXIO7).
    void exioWrite(uint8_t io, bool level);
    bool exioRead(uint8_t io);
    uint8_t exioReadAll();
    // OUTPUT or INPUT.  7B: per pin.  -7: the CH422G switches IO0..7 between
    // input and output all together, so this is a no-op that returns false.
    bool exioPinMode(uint8_t io, uint8_t mode);

private:
    bool _writeOut();      // push _outShadow to the expander (caller holds the bus)
    Model   _model;
    uint8_t _outShadow;    // shadow of the output latches
    uint8_t _modeShadow;   // 7B: shadow of the mode register
    bool _begun;
    bool _rs485GuardWanted = LCD7_7B_RS485_GUARD;
    bool _rs485GuardOn = false;
};
