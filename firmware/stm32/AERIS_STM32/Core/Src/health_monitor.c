/* health_monitor.c - state-change events and status LED (see health_monitor.h). */
#include "health_monitor.h"
#include "main.h"
#include "sensor_manager.h"
#include "timebase.h"
#include "uart_link.h"

#include <stdbool.h>
#include <stdio.h>

static health_event_cb emit_cb;
static sensor_state_t last_imu;
static sensor_state_t last_baro;
static bool last_esp;
static uint32_t last_dropped;
static uint32_t next_drop_check_ms;

static void emit(const char *ev, const char *detail)
{
    if (emit_cb) {
        emit_cb(ev, detail);
    }
}

void health_monitor_init(health_event_cb emit_fn)
{
    emit_cb = emit_fn;
    last_imu = SENSOR_STATE_INIT;
    last_baro = SENSOR_STATE_INIT;
    last_esp = false;
    last_dropped = 0;
    next_drop_check_ms = 0;
}

static void check_sensor(const char *name, sensor_state_t now_state, sensor_state_t *last)
{
    char ev[24];
    if (now_state == *last) {
        return;
    }
    if (now_state == SENSOR_STATE_FAULT) {
        snprintf(ev, sizeof ev, "%s_fault", name);
        emit(ev, *last == SENSOR_STATE_INIT ? "init failed" : "read/config errors");
    } else if (now_state == SENSOR_STATE_OK) {
        snprintf(ev, sizeof ev, "%s_%s", name, *last == SENSOR_STATE_INIT ? "ready" : "recovered");
        emit(ev, "");
    }
    /* FAULT -> INIT (retry in progress) is not reported. */
    if (now_state != SENSOR_STATE_INIT) {
        *last = now_state;
    }
}

static void update_led(uint32_t now, sensor_state_t imu, sensor_state_t baro, bool esp)
{
    uint32_t half_period;   /* 0 = solid on */

    if (imu == SENSOR_STATE_FAULT || baro == SENSOR_STATE_FAULT) {
        half_period = 500;
    } else if (imu == SENSOR_STATE_INIT || baro == SENSOR_STATE_INIT) {
        half_period = 250;
    } else if (!esp) {
        half_period = 100;
    } else {
        half_period = 0;
    }

    GPIO_PinState on = (half_period == 0 || ((now / half_period) & 1U)) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, on);
}

void health_monitor_poll(uint32_t now, uint32_t telemetry_dropped)
{
    sensor_state_t imu = sensor_manager_imu_state();
    sensor_state_t baro = sensor_manager_baro_state();
    bool esp = uart_link_esp_alive(now);

    check_sensor("imu", imu, &last_imu);
    check_sensor("baro", baro, &last_baro);

    if (esp != last_esp) {
        emit(esp ? "esp_link_restored" : "esp_link_lost",
             esp ? "" : "no ESP8266 heartbeat; acquisition continues");
        last_esp = esp;
    }

    /* Report UART congestion at most once every 5 s. */
    if (timebase_reached(now, next_drop_check_ms)) {
        next_drop_check_ms = now + 5000U;
        if (telemetry_dropped != last_dropped) {
            char detail[48];
            snprintf(detail, sizeof detail, "%lu frames dropped in last 5 s",
                     (unsigned long)(telemetry_dropped - last_dropped));
            emit("telemetry_dropping", detail);
            last_dropped = telemetry_dropped;
        }
    }

    update_led(now, imu, baro, esp);
}
