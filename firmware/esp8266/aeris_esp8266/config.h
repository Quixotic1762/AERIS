/*
 * config.h - AERIS ESP8266 bridge settings. EDIT THIS FILE before flashing.
 *
 * Do not commit real Wi-Fi or broker passwords to a public repository.
 */
#ifndef AERIS_ESP_CONFIG_H
#define AERIS_ESP_CONFIG_H

/* ---- Wi-Fi ---------------------------------------------------------- */
#define WIFI_SSID          "Ashwin's CMF Nothing phone 2 pro"
#define WIFI_PASSWORD      "123456789"

/* ---- MQTT broker -----------------------------------------------------
 * Use the LAN IP of the computer running Mosquitto, NOT "localhost"
 * (on the ESP8266, localhost is the ESP8266 itself).
 */
#define MQTT_HOST          "10.87.80.227"
#define MQTT_PORT          1883
#define MQTT_USER          "aeris"        /* "" if the broker allows anonymous */
#define MQTT_PASSWORD      "aeris"
#define MQTT_CLIENT_ID     "aeris-01-esp8266"

/* ---- Identity / topics ---------------------------------------------- */
#define DEVICE_ID          "aeris-01"
#define TOPIC_TELEMETRY    "aeris/telemetry"     /* STM32 'T' frames */
#define TOPIC_STATUS       "aeris/status"        /* STM32 'S' frames */
#define TOPIC_EVENT        "aeris/event"         /* STM32 'E' frames */
#define TOPIC_ESP_STATUS   "aeris/status/esp"    /* this bridge (retained + last will) */
#define TOPIC_COMMAND      "aeris/command"       /* forwarded to the STM32 */

/* ---- UART link to STM32 ----------------------------------------------
 * 1 = call Serial.swap(): UART0 moves to GPIO13 (RX, D7) / GPIO15 (TX, D8).
 *     The USB-serial chip stays free and boot messages don't reach the
 *     STM32. Debug log goes to Serial1 (GPIO2 / D4, TX only).
 * 0 = use the default GPIO3 (RX) / GPIO1 (TX) pins. Unplug USB while
 *     running, and debug logging is disabled.
 */
#define LINK_BAUD          115200
#define USE_SERIAL_SWAP    1
#define DEBUG_ENABLED      1
#define DEBUG_BAUD         115200

/* ---- Timing --------------------------------------------------------- */
#define HEARTBEAT_PERIOD_MS     1000   /* heartbeat frame to STM32 */
#define ESP_STATUS_PERIOD_MS    5000   /* publish on TOPIC_ESP_STATUS */
#define MQTT_RETRY_MIN_MS       1000   /* reconnect back-off: 1 s ... 16 s */
#define MQTT_RETRY_MAX_MS       16000

#endif /* AERIS_ESP_CONFIG_H */
