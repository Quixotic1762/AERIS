# AERIS telemetry protocol (schema_version 1)

This file defines every message that passes between the STM32, the ESP8266 and the dashboard.

## 1. UART link: STM32 ↔ ESP8266

- USART1 on the STM32 to UART0 on the ESP8266, **115200 baud, 8N1**, no flow control, 3.3 V logic.
- One frame per line, ASCII:

```text
$<K>:<payload>*<HH>\n
```

| Part | Meaning |
|---|---|
| `$` | Start of frame. If `$` arrives in the middle of a frame, the partial frame is discarded and counted as bad. |
| `K` | Frame kind (one letter, see the table below). |
| `payload` | A JSON object with no newlines. |
| `HH` | Two uppercase hex digits: the XOR of every byte between `$` and `*`, i.e. over `K:payload`. |
| `\n` | End of frame. A `\r` just before it is ignored. |

The maximum frame length is **1024 bytes**. Longer frames are discarded up to the next `\n` and counted as oversize. Frames with a bad checksum are discarded and counted. Both receivers collect bytes until `\n`, so a frame split across UART reads is handled correctly.

| Kind | Direction | Payload | MQTT topic |
|---|---|---|---|
| `T` | STM32 → ESP | telemetry (section 3) | `aeris/telemetry` |
| `S` | STM32 → ESP | STM32 status (section 4) | `aeris/status` |
| `E` | STM32 → ESP | event (section 5) | `aeris/event` |
| `H` | ESP → STM32 | heartbeat `{"wifi":true,"mqtt":true,"rssi":-60}` | not published |
| `C` | ESP → STM32 | command (section 6), copied unchanged from `aeris/command` | from MQTT |

Example frame: `$T:{}*68`, where `0x68` = XOR of `T`, `:`, `{`, `}`.

**Link budget.** A telemetry line is about 380–400 bytes and a status line is at most about 860 bytes. At 20 Hz telemetry plus 1 Hz status, the link runs at about 75 % of its 11.5 kB/s capacity.

## 2. MQTT topics

| Topic | Publisher | Retained | Content |
|---|---|---|---|
| `aeris/telemetry` | ESP8266 (STM32 `T` frames) | no | section 3 |
| `aeris/status` | ESP8266 (STM32 `S` frames) | no | section 4, 1 Hz |
| `aeris/event` | ESP8266 (STM32 `E` frames) | no | section 5 |
| `aeris/status/esp` | ESP8266 itself | **yes** | bridge diagnostics every 5 s. The last will is `"online":false`. |
| `aeris/command` | dashboard | no | section 6, QoS 1 |

Telemetry uses QoS 0. Gaps and duplicates are detected from `sequence`, whatever the QoS.

## 3. Telemetry (`aeris/telemetry`)

```json
{"schema_version":1,"device_id":"aeris-01","sequence":42,"device_time_ms":123460,
 "imu_time_ms":123456,"barometer_time_ms":123400,
 "accel_x_g":0.0213,"accel_y_g":-0.0005,"accel_z_g":-1.0000,
 "gyro_x_dps":-123.46,"gyro_y_dps":0.00,"gyro_z_dps":250.00,
 "pressure_hpa":1008.40,"temperature_c":25.10,"altitude_m":40.46,"sea_level_hpa":1013.25,
 "validity":{"imu":true,"barometer":true}}
```

| Field | Type | Unit | Notes |
|---|---|---|---|
| `schema_version` | int | — | Always `1`. |
| `device_id` | string | — | Set by `AERIS_DEVICE_ID` in `aeris_config.h`. |
| `sequence` | uint32 | — | Counts **telemetry packets generated** by the STM32, starting at 0 on boot. It increments even when a packet is dropped at the UART, so every loss shows up as a gap. |
| `device_time_ms` | uint32 | ms | Time the packet was built. Monotonic time **since STM32 boot**, not UTC. Wraps after about 49.7 days. |
| `imu_time_ms` | uint32 / null | ms | Time of the newest IMU sample in the averaging window. |
| `barometer_time_ms` | uint32 / null | ms | Time the barometer reading was taken. It can be up to 100 ms older than the IMU data. |
| `accel_[xyz]_g` | float / null | g | Specific force. Mean of the 100 Hz samples since the previous packet (about 5 samples). Range ±4 g. |
| `gyro_[xyz]_dps` | float / null | deg/s | Angular rate, averaged the same way. Range ±500 deg/s. |
| `pressure_hpa` | float / null | hPa | BMP280 compensated pressure. |
| `temperature_c` | float / null | °C | BMP280 die temperature (not air temperature). |
| `altitude_m` | float / null | m | **Estimate** from pressure: `44330·(1 − (p/p0)^0.1903)`. |
| `sea_level_hpa` | float | hPa | Reference pressure `p0` used for `altitude_m`. |
| `validity.imu` | bool | — | `false` if the IMU is faulted or no good sample arrived in the window. |
| `validity.barometer` | bool | — | `false` if the barometer is faulted or its reading is more than 500 ms old. |

**Invalid values are sent as `null`, never as `0`.** The dashboard also treats any value that is missing, non-numeric or non-finite as invalid for its whole group.

**Axes** are the sensor's own axes as printed on the MPU6500 breakout. Lying flat with the component side up, the board reads `accel_z_g ≈ +1`.

**Rates:** IMU 100 Hz (DLPF 41 Hz), BMP280 10 Hz reads (×16 pressure oversampling, IIR ×16), telemetry 20 Hz.

## 4. STM32 status (`aeris/status`, 1 Hz)

```json
{"schema_version":1,"device_id":"aeris-01","device_time_ms":61000,"source":"stm32",
 "fw_version":"1.0.0","uptime_s":61,"imu_state":"ok","baro_state":"ok",
 "imu_who_am_i":"0x70","baro_chip_id":"0x58",
 "imu_samples":6000,"imu_read_errors":0,"imu_reinit_attempts":1,
 "baro_samples":600,"baro_read_errors":0,"baro_reinit_attempts":1,
 "i2c_errors":0,"i2c_recoveries":0,
 "telemetry_generated":1220,"telemetry_dropped":0,"frames_dropped":0,
 "link_rx_frames":61,"link_rx_bad":0,"link_rx_oversize":0,
 "link_rx_uart_errors":0,"link_rx_overflows":0,
 "esp_link":true,"esp_wifi":true,"esp_mqtt":true,"sea_level_hpa":1013.25,
 "imu_rate_hz":100,"baro_rate_hz":10,"telemetry_rate_hz":20,
 "accel_range_g":4,"gyro_range_dps":500}
```

`imu_state` and `baro_state` are each one of `init`, `ok` or `fault`. A sensor goes to `fault` after 3 consecutive read errors, or when a register read-back shows it has lost its configuration (for example after a power glitch). While faulted, the STM32 tries to re-initialise it every 1 s.

## 5. Events (`aeris/event`)

```json
{"schema_version":1,"device_id":"aeris-01","device_time_ms":5,"source":"stm32","event":"boot","detail":"fw 1.0.0"}
```

| Event | When |
|---|---|
| `boot` | STM32 started. |
| `imu_ready` / `baro_ready` | First successful initialisation. |
| `imu_fault` / `baro_fault` | Sensor entered the fault state. |
| `imu_recovered` / `baro_recovered` | Re-initialisation succeeded after a fault. |
| `esp_link_lost` / `esp_link_restored` | No ESP8266 heartbeat for 3 s / heartbeat back. Acquisition continues either way. |
| `telemetry_dropping` | The UART TX buffer was full and telemetry was dropped in the last 5 s. |
| `command_ack` / `command_rejected` | Result of a command (section 6). |

While MQTT is down, the ESP8266 keeps the last 8 events and the latest status, and publishes them after it reconnects. Telemetry received while MQTT is down is dropped and counted in `dropped_offline`.

## 6. Commands (`aeris/command`)

| Command | Effect |
|---|---|
| `{"cmd":"set_sea_level_hpa","value":1013.25}` | Sets the altitude reference (800–1100 hPa). It is reset to 1013.25 when the STM32 reboots. |
| `{"cmd":"request_status"}` | Sends a status message immediately. |

The ESP8266 rejects commands longer than 256 bytes or containing `$`, CR, LF or NUL, because those bytes would break the line framing.

## 7. CSV columns

These are the columns from spec section 9.2, in the same order, plus `sea_level_hpa` added at the end:

```text
host_receive_time_iso, host_receive_time_unix_ms, device_id, sequence, device_time_ms,
imu_time_ms, barometer_time_ms, accel_x_g, accel_y_g, accel_z_g, gyro_x_dps, gyro_y_dps,
gyro_z_dps, pressure_hpa, temperature_c, altitude_m, imu_valid, barometer_valid,
mqtt_topic, sea_level_hpa
```

Invalid or missing values are written as empty cells. Booleans are written as `true` or `false`. `host_receive_time_iso` uses the host's local time with its UTC offset. Duplicate packets (same `sequence` and `device_time_ms`) are not written. Malformed messages are rejected, counted on the dashboard and not written.
