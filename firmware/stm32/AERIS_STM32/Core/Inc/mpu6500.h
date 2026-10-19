/*
 * mpu6500.h - MPU6500 IMU driver (I2C).
 *
 * Configuration (see aeris_config.h for ranges):
 *   sample rate 100 Hz (SMPLRT_DIV = 9 with 1 kHz internal rate)
 *   gyro DLPF 41 Hz, accel DLPF 44.8 Hz
 *   accel +/-4 g  (8192 LSB/g), gyro +/-500 dps (65.5 LSB/dps)
 *
 * Axes are the sensor's own axes as printed on the breakout. AERIS keeps
 * this convention end to end (firmware, MQTT, dashboard, CSV).
 *
 * Init is split in two so the 100 ms post-reset wait never blocks the main
 * loop: call mpu6500_begin_reset(), wait MPU6500_RESET_WAIT_MS, then call
 * mpu6500_finish_init().
 */
#ifndef MPU6500_H
#define MPU6500_H

#include <stdbool.h>
#include <stdint.h>

#define MPU6500_RESET_WAIT_MS  100U

typedef enum {
    MPU6500_OK = 0,
    MPU6500_ERR_BUS,      /* I2C transfer failed / no ACK */
    MPU6500_ERR_ID,       /* WHO_AM_I not a supported part */
    MPU6500_ERR_CONFIG,   /* register read-back mismatch / device reset */
    MPU6500_ERR_DATA,     /* data implausible (e.g. all bytes 0xFF) */
} mpu6500_status_t;

typedef struct {
    int16_t ax, ay, az;
    int16_t temp;
    int16_t gx, gy, gz;
} mpu6500_raw_t;

typedef struct {
    float accel_g[3];
    float gyro_dps[3];
    float temp_c;
} mpu6500_sample_t;

mpu6500_status_t mpu6500_begin_reset(uint8_t addr7);
mpu6500_status_t mpu6500_finish_init(void);

/* Read one sample. raw_out may be NULL; it is kept for debugging. */
mpu6500_status_t mpu6500_read(mpu6500_sample_t *out, mpu6500_raw_t *raw_out);

/* Verify the device is still awake and configured (catches power glitches). */
mpu6500_status_t mpu6500_check_config(void);

/* Last WHO_AM_I value read (0 if never read). */
uint8_t mpu6500_who_am_i(void);

#endif /* MPU6500_H */
