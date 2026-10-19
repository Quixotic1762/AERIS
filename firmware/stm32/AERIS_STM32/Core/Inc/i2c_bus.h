/*
 * i2c_bus.h - shared I2C1 access with bounded timeouts and bus recovery.
 *
 * Every call returns within ~I2C_TIMEOUT_MS. If the peripheral reports a
 * stuck bus (BUSY / timeout), the bus is recovered by clocking SCL nine
 * times and re-initialising I2C1, so a sensor that hangs the bus cannot
 * stall acquisition forever.
 */
#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t errors;      /* failed transfers (NACK, timeout, bus error) */
    uint32_t recoveries;  /* bus recovery sequences performed */
} i2c_bus_stats_t;

/* addr7 is the 7-bit device address. */
bool i2c_bus_read(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len);
bool i2c_bus_write(uint8_t addr7, uint8_t reg, uint8_t value);
bool i2c_bus_probe(uint8_t addr7);
void i2c_bus_recover(void);
void i2c_bus_get_stats(i2c_bus_stats_t *out);

#endif /* I2C_BUS_H */
