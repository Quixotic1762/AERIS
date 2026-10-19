/* uart_link.c - framed STM32 <-> ESP8266 transport (see uart_link.h). */
#include "uart_link.h"
#include "aeris_config.h"
#include "serial.h"
#include "timebase.h"

#include <string.h>

static uart_link_stats_t stats;
static uart_link_command_cb command_cb;

/* RX parser */
static char     rx_buf[LINK_MAX_FRAME];
static size_t   rx_len;
static bool     rx_in_frame;
static bool     rx_discarding;   /* oversize frame: skip to next '\n' */

/* ESP heartbeat state */
static bool     hb_seen;
static uint32_t hb_last_ms;
static bool     esp_wifi;
static bool     esp_mqtt;

static const char HEX[] = "0123456789ABCDEF";

uint8_t uart_link_checksum(const char *data, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) {
        x ^= (uint8_t)data[i];
    }
    return x;
}

void uart_link_init(uart_link_command_cb on_command)
{
    memset(&stats, 0, sizeof stats);
    command_cb = on_command;
    rx_len = 0;
    rx_in_frame = false;
    rx_discarding = false;
    hb_seen = false;
    esp_wifi = esp_mqtt = false;
}

bool uart_link_send(char kind, const char *payload, size_t len)
{
    static char frame[LINK_MAX_FRAME];   /* main-loop only, not reentrant */
    /* "$K:" + payload + "*HH\n" */
    if (len + 7U > sizeof frame) {
        stats.tx_dropped++;
        return false;
    }

    frame[0] = '$';
    frame[1] = kind;
    frame[2] = ':';
    memcpy(&frame[3], payload, len);
    uint8_t cs = uart_link_checksum(&frame[1], len + 2U);
    size_t n = 3U + len;
    frame[n++] = '*';
    frame[n++] = HEX[cs >> 4];
    frame[n++] = HEX[cs & 0x0F];
    frame[n++] = '\n';

    if (!serial_write(SERIAL_LINK, frame, (uint16_t)n)) {
        stats.tx_dropped++;
        return false;
    }
    stats.tx_frames++;
    return true;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Looks for "key":true in a small JSON object. */
static bool json_flag(const char *json, const char *key)
{
    char pat[24];
    size_t kl = strlen(key);
    if (kl + 4U > sizeof pat) {
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
    return strncmp(p, "true", 4) == 0 || *p == '1';
}

/* rx_buf holds "K:<payload>*HH" (no '$', no '\n'). */
static void handle_frame(uint32_t now)
{
    if (rx_len < 5U || rx_buf[1] != ':' || rx_buf[rx_len - 3] != '*') {
        stats.rx_bad++;
        return;
    }
    int hi = hex_val(rx_buf[rx_len - 2]);
    int lo = hex_val(rx_buf[rx_len - 1]);
    if (hi < 0 || lo < 0 ||
        uart_link_checksum(rx_buf, rx_len - 3U) != (uint8_t)((hi << 4) | lo)) {
        stats.rx_bad++;
        return;
    }

    char kind = rx_buf[0];
    char *payload = &rx_buf[2];
    size_t plen = rx_len - 5U;
    payload[plen] = '\0';   /* overwrite '*' */
    stats.rx_frames++;

    switch (kind) {
    case 'H':
        stats.heartbeats++;
        hb_seen = true;
        hb_last_ms = now;
        esp_wifi = json_flag(payload, "wifi");
        esp_mqtt = json_flag(payload, "mqtt");
        break;
    case 'C':
        stats.commands++;
        if (command_cb) {
            command_cb(payload, plen);
        }
        break;
    default:
        stats.rx_bad++;
        break;
    }
}

void uart_link_poll(uint32_t now)
{
    int c;
    /* Bound the work per call so a flood of bytes can't starve sampling. */
    for (int budget = 256; budget > 0 && (c = serial_read_byte(SERIAL_LINK)) >= 0; budget--) {
        char ch = (char)c;

        if (ch == '$') {
            /* Start of frame. A '$' inside a frame means the previous one
             * was truncated: count it and resynchronise here. */
            if (rx_in_frame && !rx_discarding) {
                stats.rx_bad++;
            }
            rx_in_frame = true;
            rx_discarding = false;
            rx_len = 0;
            continue;
        }
        if (!rx_in_frame) {
            continue;   /* noise between frames */
        }
        if (ch == '\n') {
            if (!rx_discarding) {
                if (rx_len > 0 && rx_buf[rx_len - 1] == '\r') {
                    rx_len--;
                }
                handle_frame(now);
            }
            rx_in_frame = false;
            rx_discarding = false;
            rx_len = 0;
            continue;
        }
        if (rx_discarding) {
            continue;
        }
        if (rx_len >= sizeof rx_buf - 1U) {
            stats.rx_oversize++;
            rx_discarding = true;
            continue;
        }
        rx_buf[rx_len++] = ch;
    }
}

bool uart_link_esp_alive(uint32_t now)
{
    return hb_seen && timebase_elapsed(now, hb_last_ms) <= ESP_LINK_TIMEOUT_MS;
}

bool uart_link_esp_wifi(void)
{
    return esp_wifi;
}

bool uart_link_esp_mqtt(void)
{
    return esp_mqtt;
}

void uart_link_get_stats(uart_link_stats_t *out)
{
    *out = stats;
}
