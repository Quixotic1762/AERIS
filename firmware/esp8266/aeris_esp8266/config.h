/*
 * config.h - AERIS ESP8266 bridge settings. EDIT THIS FILE before flashing.
 *
 * Do not commit real Wi-Fi or broker passwords to a public repository.
 */
#ifndef AERIS_ESP_CONFIG_H
#define AERIS_ESP_CONFIG_H

/* ---- Wi-Fi ---------------------------------------------------------- */
#define WIFI_SSID          "NITU"
#define WIFI_PASSWORD      "VIKAS@_25121986"

/* ---- MQTT broker -----------------------------------------------------
 * Use the LAN IP of the computer running Mosquitto, NOT "localhost"
 * (on the ESP8266, localhost is the ESP8266 itself).
 */
#define MQTT_HOST          "192.168.100.13"
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

/* ---- Debug log + STM32 link -----------------------------------------
 * STM32 link: hardware UART0 (Serial), moved by Serial.swap() to
 *            D7 = GPIO13 = RX  <- STM32 PA9  (USART1 TX)
 *            D8 = GPIO15 = TX  -> STM32 PA10 (USART1 RX)
 * Debug log: transmit-only SoftwareSerial on GPIO1, the pin wired to the
 *            board's USB-serial chip, so it shows in the Serial Monitor.
 * LINK_BAUD must match USART1 on the STM32 (main.c).
 */
#define LINK_BAUD          115200
#define DEBUG_ENABLED      1
#define DEBUG_BAUD         115200
#define DEBUG_TX_PIN       1       /* GPIO1 = USB-serial chip RX */

/* ---- Timing --------------------------------------------------------- */
#define HEARTBEAT_PERIOD_MS     1000   /* heartbeat frame to STM32 */
#define ESP_STATUS_PERIOD_MS    5000   /* publish on TOPIC_ESP_STATUS */
#define MQTT_RETRY_MIN_MS       1000   /* reconnect back-off: 1 s ... 16 s */
#define MQTT_RETRY_MAX_MS       16000

#endif /* AERIS_ESP_CONFIG_H */
