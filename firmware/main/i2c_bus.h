// the one I2C bus of a board: the round one's IO expander and touch chip, the 3.5" C's touch,
// the ES3C28P's and the CrowPanel Advance's touch
#pragma once
#include "board.h"
#ifdef PIN_I2C_SDA
#include "driver/i2c_master.h"
#ifdef __cplusplus
extern "C" {
#endif

// a device on the bus at 400 kHz, the bus comes up on the first call
i2c_master_dev_handle_t i2c_dev(uint8_t addr);
// 1 if something acks at addr
int i2c_probe(uint8_t addr);
// the bus itself, for a driver that adds its own devices (the Waveshare 7/7B's, lcd_wave7.cpp)
i2c_master_bus_handle_t i2c_bus_get(void);
// take the bus down and give the pins back (only when no device was added)
void i2c_bus_free(void);
#ifdef __cplusplus
}
#endif
#endif
