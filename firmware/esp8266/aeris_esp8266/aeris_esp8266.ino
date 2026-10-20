/*
 * AERIS ESP8266 bridge - UART (STM32) <-> Wi-Fi / MQTT.
 *
 * Arduino IDE setup:
 *   Board package : "esp8266 by ESP8266 Community" (Boards Manager URL
 *                   https://arduino.esp8266.com/stable/package_esp8266com_index.json)
 *   Board         : NodeMCU 1.0 (ESP-12E Module) or LOLIN(WEMOS) D1 mini
 *   Library       : "PubSubClient" by Nick O'Leary (Library Manager)
 *   Settings      : edit config.h (Wi-Fi, broker IP, credentials)
 *
 * Responsibilities (spec section 2.2): connect to Wi-Fi, publish telemetry.
 * This sketch does no sensor processing. It:
 *   - receives "$K:<json>*HH\n" frames from the STM32, checks length and
 *     checksum, and publishes the JSON on the topic for frame kind K;
 *   - sends a heartbeat frame to the STM32 every second so the STM32 can
 *     tell whether the radio link is alive;
 *   - forwards messages from aeris/command to the STM32 as 'C' frames;
 *   - publishes its own diagnostics on aeris/status/esp (retained, with a
 *     last-will "online":false so the dashboard sees the bridge drop).
 *
 * Telemetry that arrives while MQTT is down is dropped and counted (it is
 * live data; the sequence gap shows up on the dashboard). Up to 8 events
 * and the latest STM32 status are held and published after reconnecting.
 */
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

#include "config.h"

#define FW_VERSION          "1.0.0"
#define MAX_FRAME           1024    /* must match LINK_MAX_FRAME on the STM32 */
#define MAX_COMMAND_LEN     256
#define EVENT_QUEUE_LEN     8
#define MQTT_BUFFER_SIZE    (MAX_FRAME + 128)

#if DEBUG_ENABLED
#define DBG(...) dbg.printf(__VA_ARGS__)
#else
#define DBG(...) do { } while (0)
#endif

#define link Serial            /* STM32 link: hardware UART0 on D7/D8 */
static SoftwareSerial dbg;     /* debug log: TX-only on GPIO1 -> USB */
static WiFiClient   net;
static PubSubClient mqtt(net);

/* ---- Counters (published on aeris/status/esp) ---- */
static uint32_t frames_ok, frames_bad, frames_oversize, frames_unknown;
static uint32_t published, publish_failed, dropped_offline;
static uint32_t commands_forwarded, commands_rejected;
static uint32_t mqtt_connects, wifi_connects;
static uint32_t last_frame_ms;
static bool     frame_seen;

/* ---- Frame parser ---- */
static char   rx_buf[MAX_FRAME];
static size_t rx_len;
static bool   rx_in_frame, rx_discarding;

/* ---- Offline hold for events / status ---- */
static String   event_queue[EVENT_QUEUE_LEN];
static uint8_t  event_head, event_count;
static String   held_status;

/* ---- Timers ---- */
static uint32_t next_heartbeat_ms, next_esp_status_ms, next_mqtt_try_ms;
static uint32_t mqtt_backoff_ms = MQTT_RETRY_MIN_MS;
static bool     wifi_was_up;

static const char HEXCHARS[] = "0123456789ABCDEF";

/* ======================================================================
 * UART link
 * ====================================================================== */

static uint8_t checksum(const char *data, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) {
        x ^= (uint8_t)data[i];
    }
    return x;
}

/* Send "$K:<payload>*HH\n" to the STM32. */
static void link_send(char kind, const char *payload, size_t len)
{
    char head[3] = { '$', kind, ':' };
    uint8_t cs = (uint8_t)(kind ^ ':') ^ checksum(payload, len);
    char tail[4] = { '*', HEXCHARS[cs >> 4], HEXCHARS[cs & 0x0F], '\n' };

    /* ESP->STM32 traffic is small (heartbeat ~60 B/s, rare commands), so a
     * brief wait on the 128-byte TX FIFO is acceptable (<= ~25 ms). */
    link.write((const uint8_t *)head, sizeof head);
    link.write((const uint8_t *)payload, len);
    link.write((const uint8_t *)tail, sizeof tail);
}

static void send_heartbeat()
{
    char hb[96];
    int n = snprintf(hb, sizeof hb, "{\"wifi\":%s,\"mqtt\":%s,\"rssi\":%d}",
                     WiFi.status() == WL_CONNECTED ? "true" : "false",
                     mqtt.connected() ? "true" : "false",
                     WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0);
    if (n > 0 && n < (int)sizeof hb) {
        link_send('H', hb, (size_t)n);
    }
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool mqtt_publish(const char *topic, const char *payload, size_t len, bool retained)
{
    if (!mqtt.connected()) {
        return false;
    }
    if (mqtt.publish(topic, (const uint8_t *)payload, (unsigned int)len, retained)) {
        published++;
        return true;
    }
    publish_failed++;
    return false;
}

static void hold_event(const char *payload)
{
    uint8_t slot = (uint8_t)((event_head + event_count) % EVENT_QUEUE_LEN);
    if (event_count == EVENT_QUEUE_LEN) {
        event_head = (uint8_t)((event_head + 1) % EVENT_QUEUE_LEN);   /* overwrite oldest */
        dropped_offline++;
    } else {
        event_count++;
    }
    event_queue[slot] = payload;
}

static void flush_held()
{
    while (event_count > 0 && mqtt.connected()) {
        String &ev = event_queue[event_head];
        if (!mqtt_publish(TOPIC_EVENT, ev.c_str(), ev.length(), false)) {
            return;
        }
        ev = String();
        event_head = (uint8_t)((event_head + 1) % EVENT_QUEUE_LEN);
        event_count--;
    }
    if (held_status.length() > 0 && mqtt.connected()) {
        if (mqtt_publish(TOPIC_STATUS, held_status.c_str(), held_status.length(), false)) {
            held_status = String();
        }
    }
}

/* rx_buf holds "K:<payload>*HH" (no '$', no newline). */
static void handle_frame()
{
    if (rx_len < 5 || rx_buf[1] != ':' || rx_buf[rx_len - 3] != '*') {
        frames_bad++;
        return;
    }
    int hi = hex_val(rx_buf[rx_len - 2]);
    int lo = hex_val(rx_buf[rx_len - 1]);
    if (hi < 0 || lo < 0 || checksum(rx_buf, rx_len - 3) != (uint8_t)((hi << 4) | lo)) {
        frames_bad++;
        return;
    }

    char kind = rx_buf[0];
    char *payload = &rx_buf[2];
    size_t plen = rx_len - 5;
    payload[plen] = '\0';

    /* Cheap sanity check: the payload must look like a JSON object. */
    if (plen < 2 || payload[0] != '{' || payload[plen - 1] != '}') {
        frames_bad++;
        return;
    }
    frames_ok++;
    frame_seen = true;
    last_frame_ms = millis();

    switch (kind) {
    case 'T':
        if (!mqtt_publish(TOPIC_TELEMETRY, payload, plen, false)) {
            dropped_offline++;
        }
        break;
    case 'S':
        if (!mqtt_publish(TOPIC_STATUS, payload, plen, false)) {
            held_status = payload;   /* keep only the latest */
        }
        break;
    case 'E':
        if (!mqtt_publish(TOPIC_EVENT, payload, plen, false)) {
            hold_event(payload);
        }
        break;
    default:
        frames_unknown++;
        break;
    }
}

static void link_poll()
{
    /* Bounded so MQTT keep-alives still get serviced during floods. */
    if (link.hasOverrun()) {
        DBG("link: rx overflow, bytes lost\r\n");
    }
    for (int budget = 2048; budget > 0 && link.available() > 0; budget--) {
        char ch = (char)link.read();

        if (ch == '$') {
            if (rx_in_frame && !rx_discarding) {
                frames_bad++;   /* previous frame truncated */
            }
            rx_in_frame = true;
            rx_discarding = false;
            rx_len = 0;
            continue;
        }
        if (!rx_in_frame) {
            continue;
        }
        if (ch == '\n') {
            if (!rx_discarding) {
                if (rx_len > 0 && rx_buf[rx_len - 1] == '\r') {
                    rx_len--;
                }
                handle_frame();
            }
            rx_in_frame = false;
            rx_discarding = false;
            rx_len = 0;
            continue;
        }
        if (rx_discarding) {
            continue;
        }
        if (rx_len >= sizeof rx_buf - 1) {
            frames_oversize++;
            rx_discarding = true;
            continue;
        }
        rx_buf[rx_len++] = ch;
    }
}

/* ======================================================================
 * MQTT
 * ====================================================================== */

static void on_mqtt_message(char *topic, byte *payload, unsigned int length)
{
    if (strcmp(topic, TOPIC_COMMAND) != 0) {
        return;
    }
    /* Commands are forwarded verbatim; they must fit in one frame and must
     * not contain bytes that would break the line framing. */
    if (length == 0 || length > MAX_COMMAND_LEN) {
        commands_rejected++;
        return;
    }
    for (unsigned int i = 0; i < length; i++) {
        char c = (char)payload[i];
        if (c == '$' || c == '\n' || c == '\r' || c == '\0') {
            commands_rejected++;
            return;
        }
    }
    link_send('C', (const char *)payload, length);
    commands_forwarded++;
    DBG("cmd -> stm32: %.*s\r\n", (int)length, (const char *)payload);
}

static size_t build_esp_status(char *buf, size_t cap, bool online)
{
    uint32_t now = millis();
    long age = frame_seen ? (long)(now - last_frame_ms) : -1;
    int n = snprintf(buf, cap,
        "{\"schema_version\":1,\"source\":\"esp8266\",\"device_id\":\"%s\","
        "\"fw_version\":\"%s\",\"online\":%s,\"uptime_s\":%lu,"
        "\"ip\":\"%s\",\"rssi_dbm\":%d,\"free_heap\":%lu,"
        "\"frames_ok\":%lu,\"frames_bad\":%lu,\"frames_oversize\":%lu,"
        "\"frames_unknown\":%lu,\"published\":%lu,\"publish_failed\":%lu,"
        "\"dropped_offline\":%lu,\"commands_forwarded\":%lu,"
        "\"commands_rejected\":%lu,\"mqtt_connects\":%lu,\"wifi_connects\":%lu,"
        "\"stm32_last_frame_age_ms\":%ld}",
        DEVICE_ID, FW_VERSION, online ? "true" : "false",
        (unsigned long)(now / 1000), WiFi.localIP().toString().c_str(),
        WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0,
        (unsigned long)ESP.getFreeHeap(),
        (unsigned long)frames_ok, (unsigned long)frames_bad,
        (unsigned long)frames_oversize, (unsigned long)frames_unknown,
        (unsigned long)published, (unsigned long)publish_failed,
        (unsigned long)dropped_offline, (unsigned long)commands_forwarded,
        (unsigned long)commands_rejected, (unsigned long)mqtt_connects,
        (unsigned long)wifi_connects, age);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static void publish_esp_status()
{
    char buf[768];
    size_t n = build_esp_status(buf, sizeof buf, true);
    if (n) {
        mqtt_publish(TOPIC_ESP_STATUS, buf, n, true);
    }
}

static void mqtt_maintain(uint32_t now)
{
    if (mqtt.connected()) {
        mqtt.loop();
        return;
    }
    if (WiFi.status() != WL_CONNECTED || (int32_t)(now - next_mqtt_try_ms) < 0) {
        return;
    }

    static const char will[] =
        "{\"schema_version\":1,\"source\":\"esp8266\",\"device_id\":\"" DEVICE_ID "\",\"online\":false}";

    DBG("mqtt: connecting to %s:%d ...\r\n", MQTT_HOST, MQTT_PORT);
    const char *user = (strlen(MQTT_USER) > 0) ? MQTT_USER : nullptr;
    const char *pass = (strlen(MQTT_USER) > 0) ? MQTT_PASSWORD : nullptr;
    bool ok = mqtt.connect(MQTT_CLIENT_ID, user, pass, TOPIC_ESP_STATUS, 1, true, will);

    if (ok) {
        mqtt_connects++;
        mqtt_backoff_ms = MQTT_RETRY_MIN_MS;
        mqtt.subscribe(TOPIC_COMMAND, 0);
        DBG("mqtt: connected\r\n");
        publish_esp_status();
        flush_held();
        send_heartbeat();   /* tell the STM32 right away */
    } else {
        DBG("mqtt: failed, state=%d, retry in %lu ms\r\n", mqtt.state(), (unsigned long)mqtt_backoff_ms);
        next_mqtt_try_ms = now + mqtt_backoff_ms;
        mqtt_backoff_ms = min<uint32_t>(mqtt_backoff_ms * 2, MQTT_RETRY_MAX_MS);
    }
}

/* ======================================================================
 * Arduino entry points
 * ====================================================================== */

void setup()
{
    /* A large RX buffer rides out the short stalls of Wi-Fi / TCP calls:
     * 4096 bytes is ~350 ms of link traffic at 115200 baud. */
    link.setRxBufferSize(4096);
    link.begin(LINK_BAUD);
    link.swap();   /* RX = GPIO13 (D7), TX = GPIO15 (D8); frees GPIO1/3 */

#if DEBUG_ENABLED
    dbg.begin(DEBUG_BAUD, SWSERIAL_8N1, -1, DEBUG_TX_PIN);
    dbg.println();
    dbg.printf("AERIS ESP8266 bridge %s, device %s\r\n", FW_VERSION, DEVICE_ID);
    DBG("link: hardware UART0 on D7/D8 @ %d baud\r\n", LINK_BAUD);
#endif

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);   /* lower latency, steadier throughput */
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    DBG("wifi: connecting to %s\r\n", WIFI_SSID);

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(on_mqtt_message);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    mqtt.setKeepAlive(15);
    mqtt.setSocketTimeout(2);   /* bound blocking time inside connect() */
    net.setTimeout(2000);

    uint32_t now = millis();
    next_heartbeat_ms = now;
    next_esp_status_ms = now + ESP_STATUS_PERIOD_MS;
    next_mqtt_try_ms = now;
}

void loop()
{
    uint32_t now = millis();

    link_poll();

    bool wifi_up = WiFi.status() == WL_CONNECTED;
    if (wifi_up != wifi_was_up) {
        wifi_was_up = wifi_up;
        if (wifi_up) {
            wifi_connects++;
            DBG("wifi: connected, ip=%s rssi=%d\r\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
            next_mqtt_try_ms = now;
        } else {
            DBG("wifi: disconnected\r\n");
        }
    }

    mqtt_maintain(now);
    link_poll();

    if ((int32_t)(now - next_heartbeat_ms) >= 0) {
        next_heartbeat_ms = now + HEARTBEAT_PERIOD_MS;
        send_heartbeat();
    }

    if ((int32_t)(now - next_esp_status_ms) >= 0) {
        next_esp_status_ms = now + ESP_STATUS_PERIOD_MS;
        publish_esp_status();
        DBG("stat: ok=%lu bad=%lu pub=%lu drop=%lu mqtt=%d\r\n",
            (unsigned long)frames_ok, (unsigned long)frames_bad,
            (unsigned long)published, (unsigned long)dropped_offline, mqtt.connected());
    }

    yield();
}
