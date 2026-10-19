/* sensor_manager.c - sensor scheduling and health (see sensor_manager.h). */
#include "sensor_manager.h"
#include "aeris_config.h"
#include "bmp280.h"
#include "mpu6500.h"
#include "timebase.h"

#include <string.h>

typedef enum {
    PHASE_RESET = 0,   /* issue reset on next poll */
    PHASE_WAIT,        /* waiting for post-reset delay */
    PHASE_RUN,         /* configured, sampling */
    PHASE_FAULT,       /* waiting to retry */
} phase_t;

typedef struct {
    phase_t  phase;
    uint32_t due_ms;          /* next action time for the current phase */
    uint32_t next_check_ms;   /* next config read-back check */
    uint8_t  consecutive_fail;
} sensor_ctl_t;

static sensor_ctl_t imu_ctl;
static sensor_ctl_t baro_ctl;
static sensor_stats_t stats;

/* IMU accumulation window */
static struct {
    float    accel[3];
    float    gyro[3];
    uint16_t n;
    uint32_t last_ms;
} imu_acc;

static baro_reading_t baro_latest;

/* Advance a periodic deadline; if we fell more than a period behind,
 * resynchronise instead of bursting to catch up. */
static void advance(uint32_t *deadline, uint32_t period, uint32_t now)
{
    *deadline += period;
    if (timebase_reached(now, *deadline)) {
        *deadline = now + period;
    }
}

static void enter_fault(sensor_ctl_t *c, uint32_t now)
{
    c->phase = PHASE_FAULT;
    c->due_ms = now + SENSOR_RETRY_PERIOD_MS;
    c->consecutive_fail = 0;
}

static void enter_run(sensor_ctl_t *c, uint32_t now)
{
    c->phase = PHASE_RUN;
    c->due_ms = now;
    c->next_check_ms = now + SENSOR_CHECK_PERIOD_MS;
    c->consecutive_fail = 0;
}

/* ---------------- IMU ---------------- */

static void imu_poll(uint32_t now)
{
    sensor_ctl_t *c = &imu_ctl;

    switch (c->phase) {
    case PHASE_RESET: {
        stats.imu_reinit_attempts++;
        mpu6500_status_t st = mpu6500_begin_reset(MPU6500_I2C_ADDR);
        if (st == MPU6500_OK) {
            c->phase = PHASE_WAIT;
            c->due_ms = now + MPU6500_RESET_WAIT_MS;
        } else {
            stats.imu_last_error = (uint8_t)st;
            enter_fault(c, now);
        }
        break;
    }
    case PHASE_WAIT:
        if (timebase_reached(now, c->due_ms)) {
            mpu6500_status_t st = mpu6500_finish_init();
            if (st == MPU6500_OK) {
                enter_run(c, now);
            } else {
                stats.imu_last_error = (uint8_t)st;
                enter_fault(c, now);
            }
        }
        break;

    case PHASE_RUN:
        if (timebase_reached(now, c->next_check_ms)) {
            c->next_check_ms = now + SENSOR_CHECK_PERIOD_MS;
            mpu6500_status_t st = mpu6500_check_config();
            if (st != MPU6500_OK) {
                stats.imu_last_error = (uint8_t)st;
                stats.imu_read_errors++;
                enter_fault(c, now);
                break;
            }
        }
        if (timebase_reached(now, c->due_ms)) {
            advance(&c->due_ms, IMU_PERIOD_MS, now);
            mpu6500_sample_t s;
            mpu6500_status_t st = mpu6500_read(&s, NULL);
            uint32_t t = timebase_ms();
            if (st == MPU6500_OK) {
                c->consecutive_fail = 0;
                stats.imu_samples++;
                for (int i = 0; i < 3; i++) {
                    imu_acc.accel[i] += s.accel_g[i];
                    imu_acc.gyro[i]  += s.gyro_dps[i];
                }
                imu_acc.n++;
                imu_acc.last_ms = t;
            } else {
                stats.imu_read_errors++;
                stats.imu_last_error = (uint8_t)st;
                if (++c->consecutive_fail >= SENSOR_FAIL_THRESHOLD) {
                    enter_fault(c, now);
                }
            }
        }
        break;

    case PHASE_FAULT:
        if (timebase_reached(now, c->due_ms)) {
            c->phase = PHASE_RESET;
        }
        break;
    }
}

/* ---------------- Barometer ---------------- */

static void baro_poll(uint32_t now)
{
    sensor_ctl_t *c = &baro_ctl;

    switch (c->phase) {
    case PHASE_RESET: {
        stats.baro_reinit_attempts++;
        bmp280_status_t st = bmp280_begin_reset(0);   /* auto-detect 0x76/0x77 */
        if (st == BMP280_OK) {
            c->phase = PHASE_WAIT;
            c->due_ms = now + BMP280_RESET_WAIT_MS;
            c->consecutive_fail = 0;
        } else {
            stats.baro_last_error = (uint8_t)st;
            enter_fault(c, now);
        }
        break;
    }
    case PHASE_WAIT:
        if (timebase_reached(now, c->due_ms)) {
            bmp280_status_t st = bmp280_finish_init();
            if (st == BMP280_OK) {
                enter_run(c, now);
            } else if (st == BMP280_ERR_CALIB && ++c->consecutive_fail < 5) {
                c->due_ms = now + BMP280_RESET_WAIT_MS;   /* NVM copy not finished yet */
            } else {
                stats.baro_last_error = (uint8_t)st;
                enter_fault(c, now);
            }
        }
        break;

    case PHASE_RUN:
        if (timebase_reached(now, c->next_check_ms)) {
            c->next_check_ms = now + SENSOR_CHECK_PERIOD_MS;
            bmp280_status_t st = bmp280_check_config();
            if (st != BMP280_OK) {
                stats.baro_last_error = (uint8_t)st;
                stats.baro_read_errors++;
                enter_fault(c, now);
                break;
            }
        }
        if (timebase_reached(now, c->due_ms)) {
            advance(&c->due_ms, BARO_PERIOD_MS, now);
            bmp280_sample_t s;
            bmp280_status_t st = bmp280_read(&s, NULL);
            if (st == BMP280_OK) {
                c->consecutive_fail = 0;
                stats.baro_samples++;
                baro_latest.valid = true;
                baro_latest.time_ms = timebase_ms();
                baro_latest.pressure_hpa = s.pressure_hpa;
                baro_latest.temperature_c = s.temperature_c;
            } else {
                stats.baro_read_errors++;
                stats.baro_last_error = (uint8_t)st;
                if (++c->consecutive_fail >= SENSOR_FAIL_THRESHOLD) {
                    enter_fault(c, now);
                }
            }
        }
        break;

    case PHASE_FAULT:
        if (timebase_reached(now, c->due_ms)) {
            c->phase = PHASE_RESET;
        }
        break;
    }
}

/* ---------------- Public API ---------------- */

void sensor_manager_init(uint32_t now)
{
    memset(&imu_ctl, 0, sizeof imu_ctl);
    memset(&baro_ctl, 0, sizeof baro_ctl);
    memset(&stats, 0, sizeof stats);
    memset(&imu_acc, 0, sizeof imu_acc);
    memset(&baro_latest, 0, sizeof baro_latest);
    imu_ctl.phase = PHASE_RESET;
    baro_ctl.phase = PHASE_RESET;
    imu_ctl.due_ms = baro_ctl.due_ms = now;
}

void sensor_manager_poll(uint32_t now)
{
    imu_poll(now);
    baro_poll(now);
}

void sensor_manager_take_imu(imu_reading_t *out)
{
    memset(out, 0, sizeof *out);
    if (imu_acc.n > 0U && imu_ctl.phase == PHASE_RUN) {
        float inv = 1.0f / (float)imu_acc.n;
        for (int i = 0; i < 3; i++) {
            out->accel_g[i]  = imu_acc.accel[i] * inv;
            out->gyro_dps[i] = imu_acc.gyro[i] * inv;
        }
        out->valid   = true;
        out->samples = imu_acc.n;
        out->time_ms = imu_acc.last_ms;
    } else {
        out->time_ms = imu_acc.last_ms;   /* time of last good sample, if any */
    }
    memset(imu_acc.accel, 0, sizeof imu_acc.accel);
    memset(imu_acc.gyro, 0, sizeof imu_acc.gyro);
    imu_acc.n = 0;
}

void sensor_manager_get_baro(uint32_t now, baro_reading_t *out)
{
    *out = baro_latest;
    if (baro_ctl.phase != PHASE_RUN ||
        !baro_latest.valid ||
        timebase_elapsed(now, baro_latest.time_ms) > BARO_STALE_MS) {
        out->valid = false;
    }
}

static sensor_state_t state_of(const sensor_ctl_t *c)
{
    switch (c->phase) {
    case PHASE_RUN:   return SENSOR_STATE_OK;
    case PHASE_FAULT: return SENSOR_STATE_FAULT;
    default:          return SENSOR_STATE_INIT;
    }
}

sensor_state_t sensor_manager_imu_state(void)
{
    return state_of(&imu_ctl);
}

sensor_state_t sensor_manager_baro_state(void)
{
    return state_of(&baro_ctl);
}

const char *sensor_state_name(sensor_state_t s)
{
    switch (s) {
    case SENSOR_STATE_OK:    return "ok";
    case SENSOR_STATE_FAULT: return "fault";
    default:                 return "init";
    }
}

void sensor_manager_get_stats(sensor_stats_t *out)
{
    *out = stats;
}
