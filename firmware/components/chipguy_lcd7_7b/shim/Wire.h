/*
 * Arduino's TwoWire, as much of it as chipguy_ThreadsafeWire calls, on the ESP-IDF i2c_master driver.
 * Not part of the vendored libraries (OWLOS Flight Radar fork, Waveshare 7/7B port).
 *
 * The bus is shared with the firmware's own I2C users (the GT911 in main/touch.c): it comes from
 * arduino_shim_i2c_bus(), which the firmware defines to hand over its bus (main/lcd_wave7.cpp). The
 * weak default makes a bus of its own.
 */
#pragma once
#include "Arduino.h"
#include "driver/i2c_master.h"

extern "C" i2c_master_bus_handle_t arduino_shim_i2c_bus(int sda, int scl);

class TwoWire {
public:
    bool begin(int sda = -1, int scl = -1, uint32_t frequency = 0);
    bool end() { return true; }
    bool setClock(uint32_t f) { _hz = f; return true; }
    uint32_t getClock() const { return _hz; }
    void setTimeOut(uint16_t ms) { _timeout = ms; }
    uint16_t getTimeOut() const { return _timeout; }

    void beginTransmission(uint8_t addr) { _addr = addr; _txLen = 0; }
    void beginTransmission(int addr) { beginTransmission((uint8_t)addr); }
    uint8_t endTransmission(bool sendStop = true);
    size_t requestFrom(uint8_t addr, size_t n, bool sendStop = true);
    size_t requestFrom(int addr, int n) { return requestFrom((uint8_t)addr, (size_t)n, true); }
    size_t requestFrom(int addr, int n, int stop) { return requestFrom((uint8_t)addr, (size_t)n, stop != 0); }

    size_t write(uint8_t b) { if (_txLen >= sizeof _tx) return 0; _tx[_txLen++] = b; return 1; }
    size_t write(const uint8_t *d, size_t n) { size_t k = 0; while (k < n && write(d[k])) k++; return k; }
    int available() { return (int)(_rxLen - _rxPos); }
    int read() { return _rxPos < _rxLen ? _rx[_rxPos++] : -1; }
    int peek() { return _rxPos < _rxLen ? _rx[_rxPos] : -1; }
    void flush() {}
    void onReceive(void (*)(int)) {}
    void onRequest(void (*)()) {}

private:
    i2c_master_dev_handle_t dev(uint8_t addr);
    i2c_master_bus_handle_t _bus = nullptr;
    i2c_master_dev_handle_t _dev[128] = {};
    uint32_t _hz = 400000;
    uint16_t _timeout = 50;
    uint8_t _addr = 0;
    uint8_t _tx[64];
    size_t _txLen = 0;
    bool _held = false;          // endTransmission(false): the write waits for the read it goes with
    uint8_t _rx[64];
    size_t _rxLen = 0, _rxPos = 0;
};

extern TwoWire Wire;
