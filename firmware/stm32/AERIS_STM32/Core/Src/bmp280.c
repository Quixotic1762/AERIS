/* bmp280.c - BMP280 barometer driver (see bmp280.h). */
#include "bmp280.h"
#include "aeris_config.h"
#include "i2c_bus.h"

#include <math.h>

#define REG_CALIB_START  0x88
#define REG_CHIP_ID      0xD0
#define REG_RESET        0xE0
#define REG_STATUS       0xF3
#define REG_CTRL_MEAS    0xF4
#define REG_CONFIG       0xF5
#define REG_PRESS_MSB    0xF7

#define RESET_WORD       0xB6
#define STATUS_IM_UPDATE 0x01

/* ctrl_meas: osrs_t = x2 (010), osrs_p = x16 (101), mode = normal (11) */
#define CTRL_MEAS_VALUE  ((0x2 << 5) | (0x5 << 2) | 0x3)
/* config: t_sb = 0.5 ms (000), filter = x16 (100), spi3w off */
#define CONFIG_VALUE     ((0x0 << 5) | (0x4 << 2))

#define ADC_SKIPPED      0x80000   /* value reported for a skipped/unready conversion */

static struct {
    uint16_t T1;
    int16_t  T2, T3;
    uint16_t P1;
    int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
} cal;

static uint8_t dev_addr;
static uint8_t chip_id;

static bool supported_id(uint8_t id)
{
    return id == 0x58 || id == 0x56 || id == 0x57 || id == 0x60;
}

static bmp280_status_t try_reset(uint8_t addr7)
{
    if (!i2c_bus_read(addr7, REG_CHIP_ID, &chip_id, 1)) {
        chip_id = 0;
        return BMP280_ERR_BUS;
    }
    if (!supported_id(chip_id)) {
        return BMP280_ERR_ID;
    }
    dev_addr = addr7;
    if (!i2c_bus_write(dev_addr, REG_RESET, RESET_WORD)) {
        return BMP280_ERR_BUS;
    }
    return BMP280_OK;
}

bmp280_status_t bmp280_begin_reset(uint8_t addr7)
{
    if (addr7 != 0U) {
        return try_reset(addr7);
    }
    bmp280_status_t st = try_reset(BMP280_I2C_ADDR_PRIMARY);
    if (st == BMP280_ERR_BUS) {
        st = try_reset(BMP280_I2C_ADDR_SECONDARY);
    }
    return st;
}

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bmp280_status_t bmp280_finish_init(void)
{
    uint8_t status;
    if (!i2c_bus_read(dev_addr, REG_STATUS, &status, 1)) {
        return BMP280_ERR_BUS;
    }
    if (status & STATUS_IM_UPDATE) {
        return BMP280_ERR_CALIB;   /* NVM copy still running; caller retries */
    }

    uint8_t c[24];
    if (!i2c_bus_read(dev_addr, REG_CALIB_START, c, sizeof c)) {
        return BMP280_ERR_BUS;
    }
    cal.T1 = u16le(&c[0]);
    cal.T2 = (int16_t)u16le(&c[2]);
    cal.T3 = (int16_t)u16le(&c[4]);
    cal.P1 = u16le(&c[6]);
    cal.P2 = (int16_t)u16le(&c[8]);
    cal.P3 = (int16_t)u16le(&c[10]);
    cal.P4 = (int16_t)u16le(&c[12]);
    cal.P5 = (int16_t)u16le(&c[14]);
    cal.P6 = (int16_t)u16le(&c[16]);
    cal.P7 = (int16_t)u16le(&c[18]);
    cal.P8 = (int16_t)u16le(&c[20]);
    cal.P9 = (int16_t)u16le(&c[22]);

    /* All-zero or all-0xFF calibration means a bad read or a fake part. */
    if (cal.T1 == 0 || cal.P1 == 0 || cal.T1 == 0xFFFF || cal.P1 == 0xFFFF) {
        return BMP280_ERR_CALIB;
    }

    /* config must be written while in sleep mode (state after reset). */
    if (!i2c_bus_write(dev_addr, REG_CONFIG, CONFIG_VALUE) ||
        !i2c_bus_write(dev_addr, REG_CTRL_MEAS, CTRL_MEAS_VALUE)) {
        return BMP280_ERR_BUS;
    }
    return bmp280_check_config();
}

bmp280_status_t bmp280_check_config(void)
{
    uint8_t meas, config;
    if (!i2c_bus_read(dev_addr, REG_CTRL_MEAS, &meas, 1) ||
        !i2c_bus_read(dev_addr, REG_CONFIG, &config, 1)) {
        return BMP280_ERR_BUS;
    }
    if (meas != CTRL_MEAS_VALUE || (config & 0xFC) != CONFIG_VALUE) {
        return BMP280_ERR_CONFIG;
    }
    return BMP280_OK;
}

/* Datasheet section 3.11.3. Returns 0.01 degC, sets *t_fine. */
static int32_t compensate_t(int32_t adc_t, int32_t *t_fine)
{
    int32_t var1 = ((((adc_t >> 3) - ((int32_t)cal.T1 * 2))) * (int32_t)cal.T2) >> 11;
    int32_t d    = (adc_t >> 4) - (int32_t)cal.T1;
    int32_t var2 = (((d * d) >> 12) * (int32_t)cal.T3) >> 14;
    *t_fine = var1 + var2;
    return (*t_fine * 5 + 128) >> 8;
}

/* Datasheet 64-bit variant. Returns Pa in Q24.8, or 0 on divide-by-zero.
 * Left shifts of signed values are written as multiplications (no UB). */
static uint32_t compensate_p(int32_t adc_p, int32_t t_fine)
{
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)cal.P6;
    var2 = var2 + var1 * (int64_t)cal.P5 * 131072;          /* << 17 */
    var2 = var2 + (int64_t)cal.P4 * 34359738368LL;          /* << 35 */
    var1 = ((var1 * var1 * (int64_t)cal.P3) / 256) + (var1 * (int64_t)cal.P2 * 4096);
    var1 = ((((int64_t)1) << 47) + var1) * (int64_t)cal.P1 / 8589934592LL;  /* >> 33 */
    if (var1 == 0) {
        return 0;
    }
    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = ((int64_t)cal.P9 * (p / 8192) * (p / 8192)) / 33554432;   /* >> 13, >> 25 */
    var2 = ((int64_t)cal.P8 * p) / 524288;                            /* >> 19 */
    p = ((p + var1 + var2) / 256) + (int64_t)cal.P7 * 16;
    return (uint32_t)p;
}

bmp280_status_t bmp280_read(bmp280_sample_t *out, bmp280_raw_t *raw_out)
{
    uint8_t b[6];
    if (!i2c_bus_read(dev_addr, REG_PRESS_MSB, b, sizeof b)) {
        return BMP280_ERR_BUS;
    }

    int32_t adc_p = (int32_t)(((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | (b[2] >> 4));
    int32_t adc_t = (int32_t)(((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4));
    if (raw_out) {
        raw_out->adc_p = adc_p;
        raw_out->adc_t = adc_t;
    }
    if (adc_p == ADC_SKIPPED || adc_t == ADC_SKIPPED) {
        return BMP280_ERR_DATA;
    }

    int32_t t_fine;
    int32_t t_centi = compensate_t(adc_t, &t_fine);
    uint32_t p_q24_8 = compensate_p(adc_p, t_fine);
    if (p_q24_8 == 0) {
        return BMP280_ERR_DATA;
    }

    float t_c   = (float)t_centi / 100.0f;
    float p_hpa = (float)p_q24_8 / 256.0f / 100.0f;

    /* Sensor operating range is 300-1100 hPa, -40..85 degC. Anything outside
     * is a read error, not a measurement. */
    if (p_hpa < 300.0f || p_hpa > 1100.0f || t_c < -40.0f || t_c > 85.0f) {
        return BMP280_ERR_DATA;
    }

    out->pressure_hpa  = p_hpa;
    out->temperature_c = t_c;
    return BMP280_OK;
}

uint8_t bmp280_chip_id(void)
{
    return chip_id;
}

uint8_t bmp280_address(void)
{
    return dev_addr;
}

float bmp280_altitude_m(float pressure_hpa, float sea_level_hpa)
{
    return 44330.0f * (1.0f - powf(pressure_hpa / sea_level_hpa, 0.1902949f));
}
