/*
 * health_monitor.h - turns state changes into events and drives LD2.
 *
 * Events raised (sent on the 'E' frame -> MQTT aeris/event):
 *   imu_ready / imu_fault / imu_recovered,
 *   baro_ready / baro_fault / baro_recovered,
 *   esp_link_lost / esp_link_restored, telemetry_dropping
 *
 * LD2 (PA5) pattern:
 *   solid on ........ both sensors OK and ESP heartbeat present
 *   1 Hz blink ...... at least one sensor faulted
 *   5 Hz blink ...... sensors OK, ESP link lost
 *   2 Hz blink ...... sensors still initialising
 */
#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <stdint.h>

typedef void (*health_event_cb)(const char *event, const char *detail);

void health_monitor_init(health_event_cb emit);

/* telemetry_dropped: running count of telemetry frames dropped at the UART. */
void health_monitor_poll(uint32_t now, uint32_t telemetry_dropped);

#endif /* HEALTH_MONITOR_H */
