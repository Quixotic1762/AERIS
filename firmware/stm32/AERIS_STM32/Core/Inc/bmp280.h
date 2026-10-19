/*
 * bmp280.h - Bosch BMP280 barometer driver (I2C).
 *
 * Configuration: normal mode, pressure x16 / temperature x2 oversampling,
 * IIR filter x16, 0.5 ms standby -> ~26 Hz internal output rate, ~0.16 Pa
 * RMS noise. Compensation uses the integer formulas from the datasheet.
 *
 * A BME280 (chip id 0x60) is also accepted: its pressure/temperature
 * registers and compensation are identical. Humidity is not used.
 */
#ifndef BMP280_H
#define BMP280_H

#include <stdbool.h>
#include <stdint.h>

#define BMP280_RESET_WAIT_MS 5U

typedef enum {
    BMP280_OK = 0,
    BMP280_ERR_BUS,       /* I2C transfer failed / no ACK */
    BMP280_ERR_ID,        /* chip id not BMP280/BME280 */
    BMP280_ERR_CALIB,     /* calibration data missing or implausible */
    BMP280_ERR_CONFIG,    /* register read-back mismatch / device reset */
    BMP280_ERR_DATA,      /* conversion not ready / out of physical range */
} bmp280_status_t;

typedef struct {
    int32_t adc_t;     /* raw 20-bit temperature */
    int32_t adc_p;     /* raw 20-bit pressure */
} bmp280_raw_t;

typedef struct {
    float pressure_hpa;
    float temperature_c;
} bmp280_sample_t;

/* Tries the given address; if 0, tries 0x76 then 0x77. */
bmp280_status_t bmp280_begin_reset(uint8_t addr7);
bmp280_status_t bmp280_finish_init(void);
bmp280_status_t bmp280_read(bmp280_sample_t *out, bmp280_raw_t *raw_out);
bmp280_status_t bmp280_check_config(void);

uint8_t bmp280_chip_id(void);
uint8_t bmp280_address(void);

/* Pressure-derived altitude estimate (international barometric formula). */
float bmp280_altitude_m(float pressure_hpa, float sea_level_hpa);

#endif /* BMP280_H */
