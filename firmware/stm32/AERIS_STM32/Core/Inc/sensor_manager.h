/*
 * sensor_manager.h - schedules sensor reads, keeps the latest valid
 * measurements, and tracks sensor health.
 *
 * Each sensor runs a small state machine:
 *   RESET -> WAIT (post-reset delay) -> OK <-> FAULT (retry every 1 s)
 * A sensor enters FAULT after SENSOR_FAIL_THRESHOLD consecutive read
 * errors, or immediately if its configuration read-back fails (e.g. it
 * was power-cycled). Nothing here blocks for longer than one I2C transfer.
 */
#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SENSOR_STATE_INIT = 0,   /* reset issued / waiting for first config */
    SENSOR_STATE_OK,
    SENSOR_STATE_FAULT,
} sensor_state_t;

/* Mean of the IMU samples collected since the previous take. */
typedef struct {
    bool     valid;          /* false if no good sample in the window */
    uint32_t time_ms;        /* timestamp of the newest sample in the window */
    uint16_t samples;        /* number of samples averaged */
    float    accel_g[3];
    float    gyro_dps[3];
} imu_reading_t;

typedef struct {
    bool     valid;          /* false if faulted or older than BARO_STALE_MS */
    uint32_t time_ms;        /* timestamp of the reading */
    float    pressure_hpa;
    float    temperature_c;
} baro_reading_t;

typedef struct {
    uint32_t imu_samples;
    uint32_t imu_read_errors;
    uint32_t imu_reinit_attempts;
    uint32_t baro_samples;
    uint32_t baro_read_errors;
    uint32_t baro_reinit_attempts;
    uint8_t  imu_last_error;   /* mpu6500_status_t of the last failure */
    uint8_t  baro_last_error;  /* bmp280_status_t of the last failure */
} sensor_stats_t;

void sensor_manager_init(uint32_t now);

/* Run any reads / state transitions that are due. Call every loop pass. */
void sensor_manager_poll(uint32_t now);

/* Returns the IMU window mean and starts a new window. */
void sensor_manager_take_imu(imu_reading_t *out);

void sensor_manager_get_baro(uint32_t now, baro_reading_t *out);

sensor_state_t sensor_manager_imu_state(void);
sensor_state_t sensor_manager_baro_state(void);
const char *sensor_state_name(sensor_state_t s);
void sensor_manager_get_stats(sensor_stats_t *out);

#endif /* SENSOR_MANAGER_H */
