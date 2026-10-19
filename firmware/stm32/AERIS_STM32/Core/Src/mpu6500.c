/* mpu6500.c - MPU6500 IMU driver (see mpu6500.h). */
#include "mpu6500.h"
#include "aeris_config.h"
#include "i2c_bus.h"

/* Register map (MPU-6500 Register Map rev 2.1) */
#define REG_SMPLRT_DIV     0x19
#define REG_CONFIG         0x1A
#define REG_GYRO_CONFIG    0x1B
#define REG_ACCEL_CONFIG   0x1C
#define REG_ACCEL_CONFIG2  0x1D
#define REG_INT_PIN_CFG    0x37
#define REG_ACCEL_XOUT_H   0x3B
#define REG_PWR_MGMT_1     0x6B
#define REG_PWR_MGMT_2     0x6C
#define REG_WHO_AM_I       0x75

#define PWR1_RESET         0x80
#define PWR1_SLEEP         0x40
#define PWR1_CLK_PLL       0x01

/* Range selection -> register bits and scale factors. */
#if MPU6500_ACCEL_RANGE_G == 2
#define ACCEL_FS_BITS 0x00
#define ACCEL_LSB_PER_G 16384.0f
#elif MPU6500_ACCEL_RANGE_G == 4
#define ACCEL_FS_BITS 0x08
#define ACCEL_LSB_PER_G 8192.0f
#elif MPU6500_ACCEL_RANGE_G == 8
#define ACCEL_FS_BITS 0x10
#define ACCEL_LSB_PER_G 4096.0f
#elif MPU6500_ACCEL_RANGE_G == 16
#define ACCEL_FS_BITS 0x18
#define ACCEL_LSB_PER_G 2048.0f
#else
#error "MPU6500_ACCEL_RANGE_G must be 2, 4, 8 or 16"
#endif

#if MPU6500_GYRO_RANGE_DPS == 250
#define GYRO_FS_BITS 0x00
#define GYRO_LSB_PER_DPS 131.0f
#elif MPU6500_GYRO_RANGE_DPS == 500
#define GYRO_FS_BITS 0x08
#define GYRO_LSB_PER_DPS 65.5f
#elif MPU6500_GYRO_RANGE_DPS == 1000
#define GYRO_FS_BITS 0x10
#define GYRO_LSB_PER_DPS 32.8f
#elif MPU6500_GYRO_RANGE_DPS == 2000
#define GYRO_FS_BITS 0x18
#define GYRO_LSB_PER_DPS 16.4f
#else
#error "MPU6500_GYRO_RANGE_DPS must be 250, 500, 1000 or 2000"
#endif

#define SMPLRT_DIV_100HZ   9      /* 1 kHz / (1 + 9) */
#define CONFIG_DLPF_41HZ   0x03
#define ACCEL2_DLPF_45HZ   0x03

static uint8_t dev_addr = MPU6500_I2C_ADDR;
static uint8_t who_am_i;

static bool supported_id(uint8_t id)
{
    /* 0x70 = MPU6500. Many "MPU6500" breakouts actually carry an MPU9250
     * (0x71) or MPU9255 (0x73) die, whose accel/gyro registers are the same. */
    return id == 0x70 || id == 0x71 || id == 0x73;
}

mpu6500_status_t mpu6500_begin_reset(uint8_t addr7)
{
    dev_addr = addr7;
    if (!i2c_bus_read(dev_addr, REG_WHO_AM_I, &who_am_i, 1)) {
        who_am_i = 0;
        return MPU6500_ERR_BUS;
    }
    if (!supported_id(who_am_i)) {
        return MPU6500_ERR_ID;
    }
    if (!i2c_bus_write(dev_addr, REG_PWR_MGMT_1, PWR1_RESET)) {
        return MPU6500_ERR_BUS;
    }
    return MPU6500_OK;
}

mpu6500_status_t mpu6500_finish_init(void)
{
    static const uint8_t cfg[][2] = {
        { REG_PWR_MGMT_1,    PWR1_CLK_PLL },     /* wake, PLL clock */
        { REG_PWR_MGMT_2,    0x00 },             /* all axes enabled */
        { REG_SMPLRT_DIV,    SMPLRT_DIV_100HZ },
        { REG_CONFIG,        CONFIG_DLPF_41HZ },
        { REG_GYRO_CONFIG,   GYRO_FS_BITS },
        { REG_ACCEL_CONFIG,  ACCEL_FS_BITS },
        { REG_ACCEL_CONFIG2, ACCEL2_DLPF_45HZ },
        { REG_INT_PIN_CFG,   0x00 },
    };

    for (unsigned i = 0; i < sizeof cfg / sizeof cfg[0]; i++) {
        if (!i2c_bus_write(dev_addr, cfg[i][0], cfg[i][1])) {
            return MPU6500_ERR_BUS;
        }
    }
    return mpu6500_check_config();
}

mpu6500_status_t mpu6500_check_config(void)
{
    uint8_t pwr, gyro, accel;

    if (!i2c_bus_read(dev_addr, REG_PWR_MGMT_1, &pwr, 1) ||
        !i2c_bus_read(dev_addr, REG_GYRO_CONFIG, &gyro, 1) ||
        !i2c_bus_read(dev_addr, REG_ACCEL_CONFIG, &accel, 1)) {
        return MPU6500_ERR_BUS;
    }
    if ((pwr & PWR1_SLEEP) || (gyro & 0x18) != GYRO_FS_BITS || (accel & 0x18) != ACCEL_FS_BITS) {
        return MPU6500_ERR_CONFIG;
    }
    return MPU6500_OK;
}

mpu6500_status_t mpu6500_read(mpu6500_sample_t *out, mpu6500_raw_t *raw_out)
{
    uint8_t b[14];

    if (!i2c_bus_read(dev_addr, REG_ACCEL_XOUT_H, b, sizeof b)) {
        return MPU6500_ERR_BUS;
    }

    /* A floating/disconnected bus with pull-ups reads all 0xFF. */
    bool all_ff = true;
    for (unsigned i = 0; i < sizeof b; i++) {
        if (b[i] != 0xFF) {
            all_ff = false;
            break;
        }
    }
    if (all_ff) {
        return MPU6500_ERR_DATA;
    }

    mpu6500_raw_t r;
    r.ax   = (int16_t)((b[0] << 8) | b[1]);
    r.ay   = (int16_t)((b[2] << 8) | b[3]);
    r.az   = (int16_t)((b[4] << 8) | b[5]);
    r.temp = (int16_t)((b[6] << 8) | b[7]);
    r.gx   = (int16_t)((b[8] << 8) | b[9]);
    r.gy   = (int16_t)((b[10] << 8) | b[11]);
    r.gz   = (int16_t)((b[12] << 8) | b[13]);

    out->accel_g[0]  = (float)r.ax / ACCEL_LSB_PER_G;
    out->accel_g[1]  = (float)r.ay / ACCEL_LSB_PER_G;
    out->accel_g[2]  = (float)r.az / ACCEL_LSB_PER_G;
    out->gyro_dps[0] = (float)r.gx / GYRO_LSB_PER_DPS;
    out->gyro_dps[1] = (float)r.gy / GYRO_LSB_PER_DPS;
    out->gyro_dps[2] = (float)r.gz / GYRO_LSB_PER_DPS;
    out->temp_c      = (float)r.temp / 333.87f + 21.0f;

    if (raw_out) {
        *raw_out = r;
    }
    return MPU6500_OK;
}

uint8_t mpu6500_who_am_i(void)
{
    return who_am_i;
}
