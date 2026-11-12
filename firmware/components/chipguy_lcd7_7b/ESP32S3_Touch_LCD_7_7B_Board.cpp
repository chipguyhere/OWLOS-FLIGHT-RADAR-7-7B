// ESP32S3_Touch_LCD_7_7B_Board - shared I2C bus + I/O expander for the
// Waveshare ESP32-S3-Touch-LCD-7 (CH422G) and -7B (IO-extension MCU).
//
// Copyright (c) 2025-2026 chipguyhere
// MIT License

#include "ESP32S3_Touch_LCD_7_7B_Board.h"
#include <math.h>

ESP32S3_Touch_LCD_7_7B_Board::ESP32S3_Touch_LCD_7_7B_Board()
    : Wire(::Wire, SDA_PIN, SCL_PIN), _model(MODEL_UNKNOWN), _outShadow(0xFF),
      _modeShadow(0xFF), _begun(false) {}

bool ESP32S3_Touch_LCD_7_7B_Board::_writeOut() {
    if (_model == MODEL_7B) return Wire.writeRegister(EXP_ADDR, EXP_REG_OUT, _outShadow);
    if (_model == MODEL_7)  return Wire.writeByte(CH422G_WR_IO, _outShadow);
    return false;
}

bool ESP32S3_Touch_LCD_7_7B_Board::begin() {
    if (_begun) return true;

    Wire.begin();   // ThreadsafeWire on this board's SDA/SCL
    delay(50);

    // Identify the board before writing anything (see the header).  The
    // probe is address-only -- START, address, STOP -- so it writes no data.
    // 0x38 is the CH422G's output-latch WRITE address, so it ACKs a write
    // probe (0x26 is its read address, and might not).  Fail-safe order: a
    // board that answers 0x38 is never treated as a 7B, because a 7B-style
    // write to 0x24 is the harmful mistake.
#if defined(LCD7_7B_FORCE_MODEL)
    _model = (LCD7_7B_FORCE_MODEL == 0x7B) ? MODEL_7B : MODEL_7;
#else
    if (Wire.probe(CH422G_WR_IO))  _model = MODEL_7;
    else if (Wire.probe(EXP_ADDR)) _model = MODEL_7B;
    else                           _model = MODEL_UNKNOWN;
#endif

    bool ok = false;
    {
        ThreadsafeWire::Guard g(Wire);
        if (_model == MODEL_7B) {
            // Idle: every line HIGH (resets de-asserted, SD CS idle, panel
            // supply on) EXCEPT the backlight, left OFF so the display driver
            // can render a first frame before revealing it, and USB_SEL, left
            // LOW so GPIO19/20 stay on the USB-C port (matching R117's
            // pull-down, the state the board powers up in).  Output latch
            // first, then mode, so no pin glitches through the wrong level as
            // it becomes an output.
            _outShadow = 0xFF & ~(1 << EXIO_LCD_BL) & ~(1 << EXIO_USB_SEL);
            _modeShadow = 0xFF;
            ok = Wire.writeRegister(EXP_ADDR, EXP_REG_OUT, _outShadow) &&
                 Wire.writeRegister(EXP_ADDR, EXP_REG_MODE, _modeShadow);
        } else if (_model == MODEL_7) {
            // Enable IO0..7 as push-pull outputs (WR_SET bit0 only: bit1 is
            // A_SCAN, bit2 OD_EN, bit3 SLEEP), then drive the output byte, the
            // same idle state as the 7B: all HIGH except the backlight and
            // USB_SEL (U9 on USB, matching R117's pull-down).  The old -7
            // library drove USB_SEL HIGH too, which selected CAN.
            _outShadow = 0xFF & ~(1 << EXIO_LCD_BL) & ~(1 << EXIO_USB_SEL);
            ok = Wire.writeByte(CH422G_WR_SET, 0x01) && Wire.writeByte(CH422G_WR_IO, _outShadow);
        }
    }
    if (!ok) return false;

    _begun = true;
    setRs485Guard(_rs485GuardWanted);   // -7: keep 5 V off GPIO15 (errata 21)
    return true;
}

bool ESP32S3_Touch_LCD_7_7B_Board::setRs485Guard(bool on) {
    _rs485GuardWanted = on;
    if (_model != MODEL_7) return _rs485GuardOn = false;   // 7B, or not begun yet
    if (on) {
        pinMode(RS485_TX_PIN, OUTPUT);
        digitalWrite(RS485_TX_PIN, LOW);
    } else if (_rs485GuardOn) {
        pinMode(RS485_TX_PIN, INPUT);   // back to the board's R78 pull-up
    }
    return _rs485GuardOn = on;
}

void ESP32S3_Touch_LCD_7_7B_Board::exioWrite(uint8_t io, bool level) {
    if (io > 7) return;
    ThreadsafeWire::Guard g(Wire);   // shadow update + write must be atomic
    if (level) _outShadow |=  (1 << io);
    else       _outShadow &= ~(1 << io);
    _writeOut();
}

uint8_t ESP32S3_Touch_LCD_7_7B_Board::exioReadAll() {
    uint8_t v = 0;
    if (_model == MODEL_7B) Wire.readRegister(EXP_ADDR, EXP_REG_IN, v);
    else if (_model == MODEL_7) Wire.readByte(CH422G_RD_IO, v);
    return v;
}

bool ESP32S3_Touch_LCD_7_7B_Board::exioRead(uint8_t io) {
    if (io > 7) return false;
    return (exioReadAll() >> io) & 1;
}

bool ESP32S3_Touch_LCD_7_7B_Board::exioPinMode(uint8_t io, uint8_t mode) {
    if (io > 7 || _model != MODEL_7B) return false;
    ThreadsafeWire::Guard g(Wire);
    if (mode == OUTPUT) _modeShadow |=  (1 << io);
    else                _modeShadow &= ~(1 << io);
    return Wire.writeRegister(EXP_ADDR, EXP_REG_MODE, _modeShadow);
}

void ESP32S3_Touch_LCD_7_7B_Board::lcdReset() {
    exioWrite(EXIO_LCD_RST, LOW);
    delay(10);
    exioWrite(EXIO_LCD_RST, HIGH);
    delay(100);
}

void ESP32S3_Touch_LCD_7_7B_Board::lcdBacklight(bool on) {
    exioWrite(EXIO_LCD_BL, on ? HIGH : LOW);
}

bool ESP32S3_Touch_LCD_7_7B_Board::lcdPower(bool on) {
    if (_model == MODEL_UNKNOWN) return false;
    exioWrite(EXIO_LCD_VDD_EN, on ? HIGH : LOW);
    return true;
}

bool ESP32S3_Touch_LCD_7_7B_Board::setBacklightPwmRaw(uint8_t duty) {
    if (_model != MODEL_7B) return false;
    return Wire.writeRegister(EXP_ADDR, EXP_REG_PWM, duty);
}

bool ESP32S3_Touch_LCD_7_7B_Board::setBacklightBrightness(uint8_t percent) {
    if (_model != MODEL_7B) return false;
    if (percent > 100) percent = 100;
    // Inverted: duty 0 is full brightness.  Waveshare's demo never lets the
    // duty above 97 % "to keep the screen from turning off completely"; the
    // same ceiling applies at percent = 0.
    uint32_t duty = (uint32_t)(100 - percent) * 255 / 100;
    if (duty > 247) duty = 247;
    return setBacklightPwmRaw((uint8_t)duty);
}

void ESP32S3_Touch_LCD_7_7B_Board::touchReset() {
    // Hold INT low while pulsing RST (EXIO1) low->high so the GT911 latches
    // the 0x5D I2C address, then release INT as an input.
    pinMode(TOUCH_INT_PIN, OUTPUT);
    digitalWrite(TOUCH_INT_PIN, LOW);

    exioWrite(EXIO_TOUCH_RST, LOW);
    delay(10);
    exioWrite(EXIO_TOUCH_RST, HIGH);
    delay(200);

    digitalWrite(TOUCH_INT_PIN, HIGH);
    pinMode(TOUCH_INT_PIN, INPUT);
}

bool ESP32S3_Touch_LCD_7_7B_Board::selectCan(bool can) {
    if (_model == MODEL_UNKNOWN) return false;
    exioWrite(EXIO_USB_SEL, can ? HIGH : LOW);
    return true;
}

void ESP32S3_Touch_LCD_7_7B_Board::sdChipSelect(bool selected) {
    exioWrite(EXIO_SD_CS, selected ? LOW : HIGH);
}

uint16_t ESP32S3_Touch_LCD_7_7B_Board::batteryRaw() {
    if (_model != MODEL_7B) return 0;
    uint8_t b[2] = {0, 0};
    if (!Wire.readRegister(EXP_ADDR, EXP_REG_ADC, b, 2)) return 0;
    return (uint16_t)(b[0] | (b[1] << 8));
}

float ESP32S3_Touch_LCD_7_7B_Board::batteryVoltage() {
    if (_model != MODEL_7B) return NAN;
    // 20k over 10k: VBAT = 3 x the ADC pin.  10-bit, 3.3 V reference.
    return batteryRaw() * (3.0f * 3.3f / 1023.0f);
}
