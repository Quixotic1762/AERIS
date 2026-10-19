# ESP8266 bridge

`firmware/esp8266/aeris_esp8266/aeris_esp8266.ino` + `config.h`. Arduino core for ESP8266 (tested 3.1.2) and the PubSubClient MQTT library (2.8).

---

## 1. What it does, and what it doesn't

The bridge is deliberately **simple**. It:

- reads `$K:<json>*HH\n` frames from the STM32, checks them, and publishes the JSON on the MQTT topic for kind `K`;
- sends a heartbeat frame to the STM32 every second;
- forwards messages from `aeris/command` to the STM32 as `C` frames;
- publishes its own diagnostics on `aeris/status/esp`, retained, with a last-will message.

It does **not** parse the telemetry JSON, look at values, add timestamps, buffer telemetry, or make decisions about the data.

**Why keep it simple?** The spec puts measurement on the STM32 and networking on the ESP8266 (section 2.3). The ESP8266 is the part most likely to misbehave: Wi-Fi drops, TCP stalls, a single core shared with the Wi-Fi stack. Keeping data logic off it means:

- network trouble can lose data in transit but can never corrupt or reinterpret it;
- the published bytes are exactly the bytes the STM32 produced, so the sequence numbers and timestamps the dashboard checks are the STM32's own;
- the protocol can change, for example by adding a field, by reflashing only the STM32.

---

## 2. Program structure

```text
setup()
 ├─ Serial.setRxBufferSize(4096)   ← must come BEFORE begin()
 ├─ Serial.begin(115200); Serial.swap()      UART0 → GPIO13 (RX) / GPIO15 (TX)
 ├─ Serial1.begin(115200)                    debug log, TX only, GPIO2 (D4)
 ├─ WiFi: station mode, no sleep, auto-reconnect, begin(SSID, PASS)
 └─ mqtt: server, callback, 1152 B buffer, keepalive 15 s, socket timeout 2 s

loop()                              (runs continuously, never blocks for long)
 ├─ link_poll()                     drain UART, publish complete frames
 ├─ Wi-Fi edge detection            count connects, trigger immediate MQTT try
 ├─ mqtt_maintain(now)              mqtt.loop() or a back-off reconnect attempt
 ├─ link_poll()                     drain again (mqtt_maintain may have taken a while)
 ├─ every 1 s: send_heartbeat()
 ├─ every 5 s: publish_esp_status()
 └─ yield()                         let the Wi-Fi stack run
```

Like the STM32, this is a non-blocking loop with deadlines, using the same wrap-safe check `(int32_t)(now - deadline) >= 0`.

**`link_poll()` is called twice per pass.** `mqtt_maintain()` is the one call that can take a while, up to ~2 s inside `connect()`. Draining the UART right before and right after it keeps the RX buffer as empty as possible on both sides of that call.

---

## 3. The UART: `Serial.swap()` and pins

The ESP8266 has one full UART (UART0). By default it uses GPIO1/GPIO3, which are **also wired to the USB-serial chip** on NodeMCU and D1 mini boards. Connecting the STM32 there causes two problems:

1. The USB chip and the STM32 both drive the ESP8266's RX line, so neither works reliably, and uploading firmware fails.
2. The ESP8266 boot ROM prints messages at 74880 baud on GPIO1 at every reset. The STM32 would receive them as garbage.

`Serial.swap()` moves UART0 to **GPIO13 (RX, pin D7)** and **GPIO15 (TX, pin D8)**:

- the USB port stays free, so you can upload with the STM32 still wired up;
- boot noise goes to the USB side and not to the STM32;
- debug logging moves to **Serial1** on GPIO2 (D4), which can only transmit. That is enough for a log, but you need a second USB-UART adapter to read it, and the Arduino Serial Monitor shows nothing.

GPIO15 must be low at boot or the ESP8266 won't start from flash. The boards have a pull-down on it, which is why the STM32's RX pin is set up with no pull-up ([02 §7](02_stm32_architecture.md#7-pins-and-peripheral-setup)).

`USE_SERIAL_SWAP 0` in `config.h` switches back to the default pins and turns debug logging off, for boards where D7/D8 aren't available.

### The 4 KB RX buffer

```c
Serial.setRxBufferSize(4096);   // ~350 ms of link traffic at 115200 baud
```

The default receive buffer is 256 bytes, about 22 ms of data at full speed. Wi-Fi and TCP calls on the ESP8266 routinely take tens of milliseconds (a `publish()` waiting for TCP buffer space, for example). A 256-byte buffer would overflow and lose frames on every hiccup. 4 KB covers about 350 ms of stalls. It must be set **before** `Serial.begin()`, or it is ignored.

---

## 4. Frame parser

`link_poll()` and `handle_frame()` are the same state machine as the STM32's `uart_link.c` (see [04](04_stm32_comms_and_health.md#receiving-a-byte-at-a-time-state-machine)): `$` starts a frame, `\n` ends it, oversize frames are discarded up to the next newline, and a `$` mid-frame counts as bad and restarts. The code is nearly line-for-line identical, on purpose. **Both ends implement the protocol the same way**, so a frame one side accepts the other side accepts too.

There are two differences:

- **Budget of 2048 bytes per call** (the STM32 uses 256). The ESP8266 has less else to do between calls, and it must keep up with ~8.6 kB/s of incoming data. The limit still exists so a flood can't starve `mqtt.loop()`. Without `mqtt.loop()` the broker would see missed keep-alives and drop the connection.
- **An extra shape check:** the payload must start with `{` and end with `}`. It is a quick test that catches frames whose checksum happens to match by chance but whose contents are not JSON, and it costs nothing.

---

## 5. Publishing and the offline policy

| Kind | Topic | If MQTT is down |
|---|---|---|
| `T` telemetry | `aeris/telemetry` | **Dropped**, `dropped_offline++` |
| `S` status | `aeris/status` | **Latest one kept** (`held_status`), older ones overwritten |
| `E` event | `aeris/event` | **Queued**, up to 8; the oldest is overwritten when full |

Each message type gets a different policy because each one *means* something different:

- **Telemetry is a live stream.** Holding a 20 Hz stream during a 10 s outage needs 200 × 400 B = 80 KB, more than the ESP8266's free heap. If it were held, it would arrive late and in a burst, and the dashboard would show it as "live" when it isn't. Dropping it is correct, and the STM32's sequence counter makes every lost packet visible as a gap. This is listed in the README's known limitations, and spec section 9.4 says there is no on-device storage.
- **Status is a snapshot.** Only the newest one matters, because each status contains all the counters. Keeping one is enough.
- **Events are history.** `imu_fault` → `imu_recovered` during an outage is exactly what you want to see afterwards. Eight slots cover the likely bursts. If more arrive, the oldest are overwritten (and counted), so the most recent history survives.

`flush_held()` runs right after a reconnect: events first, oldest first, then the held status. It stops at the first failed publish and leaves the rest queued for the next attempt.

**QoS 0 for everything the bridge publishes.** For telemetry, QoS 1 would add broker acknowledgements and retransmission traffic, and loss is already handled by sequence numbers. PubSubClient can only *publish* at QoS 0 anyway.

---

## 6. MQTT connection management

### Connect with a last will

```c
mqtt.connect(MQTT_CLIENT_ID, user, pass,
             TOPIC_ESP_STATUS, /*willQos*/ 1, /*willRetain*/ true,
             "{...\"online\":false}");
```

The **last will** is a message the *broker* publishes on the client's behalf if the client disappears without disconnecting cleanly (power loss, Wi-Fi gone, crash). The broker notices after about 1.5 × the keep-alive (15 s), so roughly 22 s.

Together with the **retained** `aeris/status/esp` message (published with `"online":true` every 5 s), any subscriber, including a dashboard started later, immediately gets the bridge's latest state:

- bridge running: the last retained message says `"online":true` with fresh counters;
- bridge died: the broker replaced it with the will, `"online":false`.

### Reconnect with exponential back-off

```c
next_mqtt_try_ms = now + mqtt_backoff_ms;
mqtt_backoff_ms  = min(mqtt_backoff_ms * 2, MQTT_RETRY_MAX_MS);   // 1, 2, 4, 8, 16, 16 … s
```

After a failure the bridge waits 1 s, then 2, 4, 8, and then every 16 s. A successful connect resets the wait to 1 s.

- **Why back off?** Each `connect()` attempt can block for up to the 2 s socket timeout. Retrying in a tight loop against a broker that is down would leave the ESP8266 blocked most of the time and starve the UART. Backing off keeps most of the time free for draining the link.
- **Why cap at 16 s?** Once the broker returns, you want the bridge back within seconds, not minutes.
- **Why reset on Wi-Fi reconnect?** When Wi-Fi comes back (`wifi_up` edge in `loop()`), `next_mqtt_try_ms = now` makes the next MQTT attempt immediate. Waiting out a back-off timer that was set while Wi-Fi was down would be pointless.

### Bounding blocking time

Some calls in the ESP8266 networking libraries can block. The sketch limits each one:

| Setting | Value | Effect |
|---|---|---|
| `mqtt.setSocketTimeout(2)` | 2 s | Max wait for a broker response during `connect()` |
| `net.setTimeout(2000)` | 2 s | Max wait on the TCP socket |
| `mqtt.setKeepAlive(15)` | 15 s | Broker detects a dead client in ~22 s |
| `WiFi.setSleepMode(WIFI_NONE_SLEEP)` | | Radio always on: lower and steadier latency, at the cost of ~70 mA |
| `WiFi.setAutoReconnect(true)` | | The SDK reconnects Wi-Fi in the background; the sketch only watches the status |
| `WiFi.persistent(false)` | | Don't write credentials to flash on every boot, which saves flash wear |

### On connect

In this order:

1. `subscribe("aeris/command")`
2. `publish_esp_status()`, which overwrites any stale retained will with `"online":true`
3. `flush_held()`, which sends the queued events and status
4. `send_heartbeat()`, so the STM32 learns `mqtt:true` immediately instead of up to 1 s later

### Buffer size

`mqtt.setBufferSize(MAX_FRAME + 128)` = 1152 B. PubSubClient's default is 256 bytes, and **publishing a larger message just fails** and returns false, with no other sign of what went wrong. A telemetry payload is ~390 B and a status ~860 B, so the default would silently drop every message. The +128 covers the MQTT header and topic name.

---

## 7. Commands: forward without parsing

```c
if (length == 0 || length > MAX_COMMAND_LEN) reject;          // 256 B
for each byte: if '$' or '\n' or '\r' or '\0' → reject;
link_send('C', payload, length);
```

The bridge doesn't know which commands exist. It only checks that the message can be **framed safely**:

- `$` would look like a new frame start to the STM32 parser;
- `\n` would end the frame early;
- `\r` would be stripped and break the checksum;
- NUL would cut the string short on the STM32, which NUL-terminates the payload.

Everything about *meaning* (is this command known, is the value in range) is checked on the STM32, which replies with `command_ack` / `command_rejected`. The rule is to check meaning where it is understood, and framing where framing happens.

**Unlike everything else, `link_send()` on the ESP8266 can block.** `Serial.write()` waits if the 128-byte hardware FIFO is full. The comment explains why that is acceptable: ESP → STM32 traffic is about 60 B/s of heartbeats plus rare commands, so a wait is at most ~25 ms and happens rarely. Adding a software TX ring here would be extra code for no benefit.

---

## 8. Bridge diagnostics

Every 5 s, on `aeris/status/esp` (retained):

| Field | Tells you |
|---|---|
| `online` | `true` here, `false` from the last will |
| `ip`, `rssi_dbm` | Network location and signal strength. Below about -75 dBm, expect trouble. |
| `free_heap` | Falling over time would mean a memory leak (the `String` objects in the event queue) |
| `frames_ok` / `frames_bad` / `frames_oversize` / `frames_unknown` | UART link health from the ESP8266's side |
| `published` / `publish_failed` | MQTT health |
| `dropped_offline` | Telemetry lost while MQTT was down, plus events overwritten in the hold queue |
| `commands_forwarded` / `commands_rejected` | Command path |
| `mqtt_connects` / `wifi_connects` | Should be 1 each on a stable network. Higher means reconnects. |
| `stm32_last_frame_age_ms` | Time since the last good frame from the STM32. `-1` = never. Large = STM32 not sending or wiring broken. |

With these and the STM32's own status message, a fault can usually be placed at one hop. If the STM32 reports `telemetry_generated` rising and `esp_link:true` but the ESP8266 shows `frames_bad` rising, the problem is the UART wiring or baud rate. If `frames_ok` is rising but `published` isn't, the problem is MQTT.

---

## 9. `config.h`

The only file you have to edit before flashing: Wi-Fi SSID and password, broker IP and credentials, client ID, topics and timings.

- **`MQTT_HOST` must be the PC's LAN IP**, not `localhost`. On the ESP8266, `localhost` is the ESP8266 itself. This mistake is common enough that the comment calls it out.
- `MQTT_USER ""` connects anonymously, if the broker allows it.
- The header warns: *"Do not commit real Wi-Fi or broker passwords to a public repository."* For a team project, the usual fix is a `config.example.h` in the repository and a git-ignored `config.h` that each person fills in.
