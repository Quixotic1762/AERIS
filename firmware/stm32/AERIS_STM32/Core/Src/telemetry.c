/* telemetry.c - JSON document builders (see telemetry.h). */
#include "telemetry.h"
#include "aeris_config.h"
#include "bmp280.h"
#include "mpu6500.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

/* ---------------- Minimal JSON writer ----------------
 * Floats are formatted with integer arithmetic so newlib-nano does not
 * need printf float support (-u _printf_float). */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;
    bool   need_comma;
} jw_t;

static void jw_append(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void jw_append(jw_t *w, const char *fmt, ...)
{
    if (w->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->overflow = true;
        return;
    }
    w->len += (size_t)n;
}

static void jw_begin(jw_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = (cap == 0);
    w->need_comma = false;
    jw_append(w, "{");
}

static void jw_key(jw_t *w, const char *key)
{
    jw_append(w, "%s\"%s\":", w->need_comma ? "," : "", key);
    w->need_comma = true;
}

static void jw_u32(jw_t *w, const char *key, uint32_t v)
{
    jw_key(w, key);
    jw_append(w, "%lu", (unsigned long)v);
}

static void jw_u32_or_null(jw_t *w, const char *key, uint32_t v, bool valid)
{
    jw_key(w, key);
    if (valid) {
        jw_append(w, "%lu", (unsigned long)v);
    } else {
        jw_append(w, "null");
    }
}

static void jw_bool(jw_t *w, const char *key, bool v)
{
    jw_key(w, key);
    jw_append(w, v ? "true" : "false");
}

/* Strings here are internal constants; escape quotes/backslashes anyway. */
static void jw_str(jw_t *w, const char *key, const char *s)
{
    jw_key(w, key);
    jw_append(w, "\"");
    for (; s && *s && !w->overflow; s++) {
        char c = *s;
        if (c == '"' || c == '\\') {
            jw_append(w, "\\%c", c);
        } else if ((unsigned char)c < 0x20) {
            jw_append(w, " ");
        } else {
            jw_append(w, "%c", c);
        }
    }
    jw_append(w, "\"");
}

static void jw_float(jw_t *w, const char *key, float v, unsigned decimals, bool valid)
{
    static const uint32_t pow10[] = { 1, 10, 100, 1000, 10000, 100000 };

    jw_key(w, key);
    if (!valid || !isfinite(v) || fabsf(v) > 1.0e7f || decimals > 5) {
        jw_append(w, "null");
        return;
    }
    uint32_t scale = pow10[decimals];
    double scaled = (double)v * scale;
    bool neg = scaled < 0;
    uint64_t q = (uint64_t)((neg ? -scaled : scaled) + 0.5);
    uint32_t ip = (uint32_t)(q / scale);
    uint32_t fp = (uint32_t)(q % scale);
    if (q == 0) {
        neg = false;   /* avoid "-0.00" */
    }
    if (decimals == 0) {
        jw_append(w, "%s%lu", neg ? "-" : "", (unsigned long)ip);
    } else {
        jw_append(w, "%s%lu.%0*lu", neg ? "-" : "", (unsigned long)ip, (int)decimals,
                  (unsigned long)fp);
    }
}

static void jw_obj_open(jw_t *w, const char *key)
{
    jw_key(w, key);
    jw_append(w, "{");
    w->need_comma = false;
}

static void jw_obj_close(jw_t *w)
{
    jw_append(w, "}");
    w->need_comma = true;
}

static size_t jw_end(jw_t *w)
{
    jw_append(w, "}");
    return w->overflow ? 0 : w->len;
}

static void jw_header(jw_t *w, uint32_t now)
{
    jw_u32(w, "schema_version", AERIS_SCHEMA_VERSION);
    jw_str(w, "device_id", AERIS_DEVICE_ID);
    jw_u32(w, "device_time_ms", now);
}

/* ---------------- Documents ---------------- */

size_t telemetry_build_sample(char *buf, size_t cap, uint32_t sequence, uint32_t now,
                              const imu_reading_t *imu, const baro_reading_t *baro,
                              float sea_level_hpa)
{
    jw_t w;
    bool iv = imu->valid;
    bool bv = baro->valid;
    float alt = bv ? bmp280_altitude_m(baro->pressure_hpa, sea_level_hpa) : 0.0f;

    jw_begin(&w, buf, cap);
    jw_u32(&w, "schema_version", AERIS_SCHEMA_VERSION);
    jw_str(&w, "device_id", AERIS_DEVICE_ID);
    jw_u32(&w, "sequence", sequence);
    jw_u32(&w, "device_time_ms", now);
    jw_u32_or_null(&w, "imu_time_ms", imu->time_ms, iv);
    jw_u32_or_null(&w, "barometer_time_ms", baro->time_ms, bv);
    jw_float(&w, "accel_x_g", imu->accel_g[0], 4, iv);
    jw_float(&w, "accel_y_g", imu->accel_g[1], 4, iv);
    jw_float(&w, "accel_z_g", imu->accel_g[2], 4, iv);
    jw_float(&w, "gyro_x_dps", imu->gyro_dps[0], 2, iv);
    jw_float(&w, "gyro_y_dps", imu->gyro_dps[1], 2, iv);
    jw_float(&w, "gyro_z_dps", imu->gyro_dps[2], 2, iv);
    jw_float(&w, "pressure_hpa", baro->pressure_hpa, 2, bv);
    jw_float(&w, "temperature_c", baro->temperature_c, 2, bv);
    jw_float(&w, "altitude_m", alt, 2, bv);
    jw_float(&w, "sea_level_hpa", sea_level_hpa, 2, true);
    jw_obj_open(&w, "validity");
    jw_bool(&w, "imu", iv);
    jw_bool(&w, "barometer", bv);
    jw_obj_close(&w);
    return jw_end(&w);
}

size_t telemetry_build_status(char *buf, size_t cap, uint32_t now,
                              const sensor_stats_t *ss, const telemetry_diag_t *d,
                              float sea_level_hpa)
{
    jw_t w;
    char hex[8];

    jw_begin(&w, buf, cap);
    jw_header(&w, now);
    jw_str(&w, "source", "stm32");
    jw_str(&w, "fw_version", AERIS_FW_VERSION);
    jw_u32(&w, "uptime_s", now / 1000U);

    jw_str(&w, "imu_state", sensor_state_name(sensor_manager_imu_state()));
    jw_str(&w, "baro_state", sensor_state_name(sensor_manager_baro_state()));
    snprintf(hex, sizeof hex, "0x%02X", mpu6500_who_am_i());
    jw_str(&w, "imu_who_am_i", hex);
    snprintf(hex, sizeof hex, "0x%02X", bmp280_chip_id());
    jw_str(&w, "baro_chip_id", hex);

    jw_u32(&w, "imu_samples", ss->imu_samples);
    jw_u32(&w, "imu_read_errors", ss->imu_read_errors);
    jw_u32(&w, "imu_reinit_attempts", ss->imu_reinit_attempts);
    jw_u32(&w, "baro_samples", ss->baro_samples);
    jw_u32(&w, "baro_read_errors", ss->baro_read_errors);
    jw_u32(&w, "baro_reinit_attempts", ss->baro_reinit_attempts);
    jw_u32(&w, "i2c_errors", d->i2c_errors);
    jw_u32(&w, "i2c_recoveries", d->i2c_recoveries);

    jw_u32(&w, "telemetry_generated", d->telemetry_generated);
    jw_u32(&w, "telemetry_dropped", d->telemetry_dropped);
    jw_u32(&w, "frames_dropped", d->frames_dropped);
    jw_u32(&w, "link_rx_frames", d->link_rx_frames);
    jw_u32(&w, "link_rx_bad", d->link_rx_bad);
    jw_u32(&w, "link_rx_oversize", d->link_rx_oversize);
    jw_u32(&w, "link_rx_uart_errors", d->link_rx_errors);
    jw_u32(&w, "link_rx_overflows", d->link_rx_overflows);
    jw_bool(&w, "esp_link", d->esp_link);
    jw_bool(&w, "esp_wifi", d->esp_wifi);
    jw_bool(&w, "esp_mqtt", d->esp_mqtt);

    jw_float(&w, "sea_level_hpa", sea_level_hpa, 2, true);
    jw_u32(&w, "imu_rate_hz", 1000U / IMU_PERIOD_MS);
    jw_u32(&w, "baro_rate_hz", 1000U / BARO_PERIOD_MS);
    jw_u32(&w, "telemetry_rate_hz", 1000U / TELEMETRY_PERIOD_MS);
    jw_u32(&w, "accel_range_g", MPU6500_ACCEL_RANGE_G);
    jw_u32(&w, "gyro_range_dps", MPU6500_GYRO_RANGE_DPS);
    return jw_end(&w);
}

size_t telemetry_build_event(char *buf, size_t cap, uint32_t now,
                             const char *event, const char *detail)
{
    jw_t w;
    jw_begin(&w, buf, cap);
    jw_header(&w, now);
    jw_str(&w, "source", "stm32");
    jw_str(&w, "event", event);
    jw_str(&w, "detail", detail ? detail : "");
    return jw_end(&w);
}
