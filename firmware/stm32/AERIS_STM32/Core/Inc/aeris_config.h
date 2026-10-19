/*
 * AERIS - build-time configuration for the STM32 acquisition firmware.
 *
 * Every rate, range and timeout used by the firmware lives here so the
 * values reported in the status message always match what the code does.
 */
#ifndef AERIS_CONFIG_H
#define AERIS_CONFIG_H

/* ---- Identity ------------------------------------------------------- */
#define AERIS_DEVICE_ID            "aeris-01"
#define AERIS_SCHEMA_VERSION       1
#define AERIS_FW_VERSION           "1.0.0"

/* ---- Scheduling (milliseconds) ---------------------------------------
 * IMU is sampled at 100 Hz. Each telemetry packet (20 Hz) carries the
 * mean of the IMU samples taken since the previous packet plus the most
 * recent barometer reading (spec 5.2, option A).
 *
 * Link budget: one telemetry line is ~380-400 bytes, a status line up to
 * ~860 bytes (1 Hz). At 115200 baud (8N1)
 * the UART carries ~11 520 bytes/s, so 20 Hz + status uses ~75 % of it.
 * Do not raise TELEMETRY_PERIOD_MS below 50 without shortening the
 * payload or raising the baud rate.
 */
#define IMU_PERIOD_MS              10U     /* 100 Hz */
#define BARO_PERIOD_MS             100U    /* 10 Hz  */
#define TELEMETRY_PERIOD_MS        50U     /* 20 Hz  */
#define STATUS_PERIOD_MS           1000U   /* 1 Hz   */
#define SENSOR_CHECK_PERIOD_MS     1000U   /* config read-back check */
#define DEBUG_SUMMARY_PERIOD_MS    1000U

/* ---- Health limits -------------------------------------------------- */
#define BARO_STALE_MS              500U    /* baro reading older than this -> invalid */
#define ESP_LINK_TIMEOUT_MS        3000U   /* no ESP heartbeat -> link lost */
#define SENSOR_FAIL_THRESHOLD      3U      /* consecutive read errors -> fault */
#define SENSOR_RETRY_PERIOD_MS     1000U   /* re-init attempt interval while faulted */

/* ---- Altitude reference --------------------------------------------- */
#define DEFAULT_SEA_LEVEL_HPA      1013.25f
#define SEA_LEVEL_MIN_HPA          800.0f
#define SEA_LEVEL_MAX_HPA          1100.0f

/* ---- I2C ------------------------------------------------------------ */
#define I2C_TIMEOUT_MS             5U
#define MPU6500_I2C_ADDR           0x68U   /* AD0 low; 0x69 if AD0 high */
#define BMP280_I2C_ADDR_PRIMARY    0x76U   /* SDO low */
#define BMP280_I2C_ADDR_SECONDARY  0x77U   /* SDO high (tried automatically) */

/* ---- IMU full-scale ranges -------------------------------------------
 * Allowed: accel 2/4/8/16 g, gyro 250/500/1000/2000 dps.
 */
#define MPU6500_ACCEL_RANGE_G      4
#define MPU6500_GYRO_RANGE_DPS     500

/* ---- UART link to ESP8266 ------------------------------------------- */
#define LINK_BAUD                  115200U
#define LINK_MAX_FRAME             1024U   /* bytes incl. "$K:", "*HH" and newline */

#endif /* AERIS_CONFIG_H */
