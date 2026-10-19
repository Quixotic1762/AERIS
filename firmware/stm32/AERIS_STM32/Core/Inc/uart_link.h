/*
 * uart_link.h - framed message transport between the STM32 and ESP8266.
 *
 * Frame format (both directions, ASCII, one frame per line):
 *
 *     $<K>:<payload>*<HH>\n
 *
 *   K       one-letter frame kind
 *             STM32 -> ESP: T telemetry, S status, E event
 *             ESP -> STM32: H heartbeat, C command
 *   payload JSON document (no newlines)
 *   HH      two uppercase hex digits: XOR of every byte between '$' and
 *           '*' (exclusive), i.e. over "<K>:<payload>"
 *
 * The receiver accumulates bytes until '\n', rejects frames longer than
 * LINK_MAX_FRAME, and discards any frame whose checksum does not match.
 */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t tx_frames;
    uint32_t tx_dropped;       /* TX ring full: frame not queued */
    uint32_t rx_frames;        /* valid frames received */
    uint32_t rx_bad;           /* checksum mismatch / malformed */
    uint32_t rx_oversize;      /* exceeded LINK_MAX_FRAME */
    uint32_t heartbeats;
    uint32_t commands;
} uart_link_stats_t;

/* Called for each valid command frame ('C'); payload is NUL-terminated. */
typedef void (*uart_link_command_cb)(const char *payload, size_t len);

void uart_link_init(uart_link_command_cb on_command);

/* Frame and queue a payload. Returns false if it was dropped. */
bool uart_link_send(char kind, const char *payload, size_t len);

/* Process received bytes. Call every loop pass. */
void uart_link_poll(uint32_t now);

/* ESP state from the latest heartbeat. */
bool uart_link_esp_alive(uint32_t now);
bool uart_link_esp_wifi(void);
bool uart_link_esp_mqtt(void);

void uart_link_get_stats(uart_link_stats_t *out);

/* Exposed for unit testing. */
uint8_t uart_link_checksum(const char *data, size_t len);

#endif /* UART_LINK_H */
