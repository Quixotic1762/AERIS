/* aeris_app.c - AERIS application loop (see aeris_app.h). */
#include "aeris_app.h"
#include "aeris_config.h"
#include "debug_log.h"
#include "health_monitor.h"
#include "i2c_bus.h"
#include "sensor_manager.h"
#include "serial.h"
#include "telemetry.h"
#include "timebase.h"
#include "uart_link.h"

#include <stdlib.h>
#include <string.h>

static float    sea_level_hpa = DEFAULT_SEA_LEVEL_HPA;
static uint32_t sequence;
static uint32_t telemetry_generated;
static uint32_t telemetry_dropped;
static uint32_t frames_dropped;

static uint32_t next_telemetry_ms;
static uint32_t next_status_ms;
static uint32_t next_debug_ms;

static char doc[LINK_MAX_FRAME];   /* JSON scratch buffer (main loop only) */

static bool send_doc(char kind, size_t len)
{
    if (len == 0) {
        debug_log("ERR: '%c' document did not fit in buffer", kind);
        frames_dropped++;
        return false;
    }
    if (!uart_link_send(kind, doc, len)) {
        frames_dropped++;
        return false;
    }
    return true;
}

static void send_event(const char *event, const char *detail)
{
    debug_log("EVENT %s %s", event, detail ? detail : "");
    send_doc('E', telemetry_build_event(doc, sizeof doc, timebase_ms(), event, detail));
}

static void send_status(uint32_t now)
{
    sensor_stats_t ss;
    uart_link_stats_t ls;
    serial_stats_t sx;
    i2c_bus_stats_t is;
    telemetry_diag_t d;

    sensor_manager_get_stats(&ss);
    uart_link_get_stats(&ls);
    serial_get_stats(SERIAL_LINK, &sx);
    i2c_bus_get_stats(&is);

    memset(&d, 0, sizeof d);
    d.telemetry_generated = telemetry_generated;
    d.telemetry_dropped   = telemetry_dropped;
    d.frames_dropped      = frames_dropped;
    d.link_rx_frames      = ls.rx_frames;
    d.link_rx_bad         = ls.rx_bad;
    d.link_rx_oversize    = ls.rx_oversize;
    d.link_rx_errors      = sx.rx_errors;
    d.link_rx_overflows   = sx.rx_overflows;
    d.i2c_errors          = is.errors;
    d.i2c_recoveries      = is.recoveries;
    d.esp_link            = uart_link_esp_alive(now);
    d.esp_wifi            = uart_link_esp_wifi();
    d.esp_mqtt            = uart_link_esp_mqtt();

    send_doc('S', telemetry_build_status(doc, sizeof doc, now, &ss, &d, sea_level_hpa));
}

static void send_telemetry(uint32_t now)
{
    imu_reading_t imu;
    baro_reading_t baro;

    sensor_manager_take_imu(&imu);
    sensor_manager_get_baro(now, &baro);

    /* Sequence counts generated packets, so UART drops appear as gaps. */
    size_t len = telemetry_build_sample(doc, sizeof doc, sequence++, now, &imu, &baro,
                                        sea_level_hpa);
    telemetry_generated++;
    if (!send_doc('T', len)) {
        telemetry_dropped++;
    }
}

/* ---- Commands from aeris/command (forwarded by the ESP8266) ----------
 *   {"cmd":"set_sea_level_hpa","value":1013.25}
 *   {"cmd":"request_status"}
 */
static bool json_string(const char *json, const char *key, char *out, size_t cap)
{
    char pat[32];
    size_t kl = strlen(key);
    if (kl + 3U > sizeof pat || cap == 0) {
        return false;
    }
    pat[0] = '"';
    memcpy(&pat[1], key, kl);
    pat[kl + 1] = '"';
    pat[kl + 2] = '\0';

    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += kl + 2;
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return *p == '"';
}

static bool json_number(const char *json, const char *key, float *out)
{
    char pat[32];
    size_t kl = strlen(key);
    if (kl + 3U > sizeof pat) {
        return false;
    }
    pat[0] = '"';
    memcpy(&pat[1], key, kl);
    pat[kl + 1] = '"';
    pat[kl + 2] = '\0';

    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += kl + 2;
    while (*p == ' ' || *p == ':') {
        p++;
    }
    char *end;
    float v = strtof(p, &end);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

static void on_command(const char *payload, size_t len)
{
    char cmd[32];
    (void)len;

    if (!json_string(payload, "cmd", cmd, sizeof cmd)) {
        send_event("command_rejected", "missing cmd");
        return;
    }

    if (strcmp(cmd, "set_sea_level_hpa") == 0) {
        float v;
        if (!json_number(payload, "value", &v) || !(v >= SEA_LEVEL_MIN_HPA && v <= SEA_LEVEL_MAX_HPA)) {
            send_event("command_rejected", "set_sea_level_hpa: value must be 800-1100");
            return;
        }
        sea_level_hpa = v;
        send_event("command_ack", "set_sea_level_hpa");
        next_status_ms = timebase_ms();   /* publish the new reference now */
    } else if (strcmp(cmd, "request_status") == 0) {
        send_event("command_ack", "request_status");
        next_status_ms = timebase_ms();
    } else {
        send_event("command_rejected", "unknown cmd");
    }
}

/* ---------------- Init / loop ---------------- */

void aeris_app_init(void)
{
    serial_init();

    uint32_t now = timebase_ms();
    debug_log("AERIS STM32 fw %s, device %s", AERIS_FW_VERSION, AERIS_DEVICE_ID);
    debug_log("IMU %lu Hz, baro %lu Hz, telemetry %lu Hz, link %lu baud",
              (unsigned long)(1000U / IMU_PERIOD_MS), (unsigned long)(1000U / BARO_PERIOD_MS),
              (unsigned long)(1000U / TELEMETRY_PERIOD_MS), (unsigned long)LINK_BAUD);

    uart_link_init(on_command);
    sensor_manager_init(now);
    health_monitor_init(send_event);

    send_event("boot", "fw " AERIS_FW_VERSION);

    next_telemetry_ms = now + TELEMETRY_PERIOD_MS;
    next_status_ms    = now + STATUS_PERIOD_MS;
    next_debug_ms     = now + DEBUG_SUMMARY_PERIOD_MS;
}

void aeris_app_loop(void)
{
    uint32_t now = timebase_ms();

    sensor_manager_poll(now);
    uart_link_poll(now);
    health_monitor_poll(now, telemetry_dropped);

    if (timebase_reached(now, next_telemetry_ms)) {
        next_telemetry_ms += TELEMETRY_PERIOD_MS;
        if (timebase_reached(now, next_telemetry_ms)) {
            next_telemetry_ms = now + TELEMETRY_PERIOD_MS;   /* fell behind: resync */
        }
        send_telemetry(now);
    }

    if (timebase_reached(now, next_status_ms)) {
        next_status_ms = now + STATUS_PERIOD_MS;
        send_status(now);
    }

    if (timebase_reached(now, next_debug_ms)) {
        next_debug_ms = now + DEBUG_SUMMARY_PERIOD_MS;
        sensor_stats_t ss;
        sensor_manager_get_stats(&ss);
        debug_log("seq=%lu imu=%s(%lu ok/%lu err) baro=%s(%lu ok/%lu err) esp=%s tx_drop=%lu",
                  (unsigned long)sequence,
                  sensor_state_name(sensor_manager_imu_state()),
                  (unsigned long)ss.imu_samples, (unsigned long)ss.imu_read_errors,
                  sensor_state_name(sensor_manager_baro_state()),
                  (unsigned long)ss.baro_samples, (unsigned long)ss.baro_read_errors,
                  uart_link_esp_alive(now) ? "up" : "down",
                  (unsigned long)telemetry_dropped);
    }
}
