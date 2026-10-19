# Life of a sample

This page follows one acceleration reading from the MPU6500 to a message on the broker, with the function that handles each step. It also follows a command going the other way. Read it before the per-module guides: it shows how the modules connect, and the later pages explain each one in detail.

Times below are milliseconds since the STM32 booted (`timebase_ms()`).

---

## Part 1: Sensor → broker

### t = 1230 ms: the IMU deadline arrives

`main()` calls `aeris_app_loop()` over and over, many thousands of times per second. Each pass reads the clock once and calls every module's `poll()` function with that time:

```c
// aeris_app.c
void aeris_app_loop(void)
{
    uint32_t now = timebase_ms();
    sensor_manager_poll(now);
    uart_link_poll(now);
    health_monitor_poll(now, telemetry_dropped);
    ...
}
```

Inside `sensor_manager_poll()` → `imu_poll()`, the IMU is in `PHASE_RUN`, and its deadline `c->due_ms` has been reached:

```c
// sensor_manager.c
if (timebase_reached(now, c->due_ms)) {
    advance(&c->due_ms, IMU_PERIOD_MS, now);   // next deadline = +10 ms
    mpu6500_status_t st = mpu6500_read(&s, NULL);
```

Most passes through the loop do nothing here because no deadline has arrived. This is how one loop runs tasks at 100 Hz, 20 Hz, 10 Hz and 1 Hz without a timer per task.

### The I²C read (~0.4 ms)

`mpu6500_read()` asks `i2c_bus_read()` for 14 bytes starting at register `0x3B` (`ACCEL_XOUT_H`). Those 14 bytes are accel X/Y/Z, temperature and gyro X/Y/Z, each a big-endian 16-bit signed integer. One burst read gets all of them from the same sample instant.

`i2c_bus_read()` calls `HAL_I2C_Mem_Read()` with a **5 ms timeout**. If the sensor were unplugged, this call would return an error within 5 ms instead of hanging. See [03_stm32_sensors.md](03_stm32_sensors.md#the-i2c-bus-layer).

### Check and scale

The driver first checks whether all 14 bytes are `0xFF`. That is what a disconnected sensor on a bus with pull-up resistors returns, and it would otherwise decode as `-0.0001 g`, which looks like a normal reading. Then it converts raw counts to units:

```c
out->accel_g[0] = (float)r.ax / ACCEL_LSB_PER_G;   // 8192 counts per g at ±4 g
```

Say the raw value was `r.az = -8192`. That becomes `accel_g[2] = -1.0000 g`.

### Add it to the window

Back in `imu_poll()`, the sample is added to a running sum rather than stored:

```c
imu_acc.accel[i] += s.accel_g[i];
imu_acc.n++;
imu_acc.last_ms = t;          // timestamp of the newest sample
```

This happens every 10 ms. By the time a telemetry packet is due, the window holds about 5 samples.

### t = 1250 ms: the telemetry deadline arrives

Back in `aeris_app_loop()`:

```c
if (timebase_reached(now, next_telemetry_ms)) {
    next_telemetry_ms += TELEMETRY_PERIOD_MS;   // +50 ms
    ...
    send_telemetry(now);
}
```

`send_telemetry()` does four things:

1. `sensor_manager_take_imu(&imu)` divides the sums by `n` to get the **mean** of the window, marks it valid, and **resets the window**.
2. `sensor_manager_get_baro(now, &baro)` copies the latest barometer reading, which was taken by its own 10 Hz deadline. It marks the reading invalid if it is older than 500 ms.
3. `telemetry_build_sample(doc, ..., sequence++, ...)` writes the JSON into the static `doc` buffer. Any invalid group becomes `null`.
4. `send_doc('T', len)` hands it to the link layer.

The sequence number is incremented **before** we know whether the send succeeds. If the frame is dropped later, the dashboard sees a gap in the numbering. That is deliberate.

### JSON without `printf("%f")`

`jw_float()` in `telemetry.c` turns `-1.0f` with 4 decimals into the text `-1.0000` using integer arithmetic. The STM32 C library (newlib-nano) leaves out float formatting by default, to save about 10 KB of flash. See [04_stm32_comms_and_health.md](04_stm32_comms_and_health.md#the-json-writer).

The finished document is about 390 bytes:

```json
{"schema_version":1,"device_id":"aeris-01","sequence":24,"device_time_ms":1250,
 "imu_time_ms":1248,"barometer_time_ms":1203,"accel_x_g":0.0213,...,
 "accel_z_g":-1.0000,...,"validity":{"imu":true,"barometer":true}}
```

### Framing

`uart_link_send('T', doc, len)` wraps it:

```text
$T:{"schema_version":1, ... }*HH\n
```

`HH` is two hex digits: the XOR of every byte from `T` up to the final `}`. (For the tiny frame `$T:{}`, it would be `68`.) The ESP8266 recomputes it, so a corrupted byte on the wire is detected.

### Queue and return (microseconds)

`serial_write()` copies the frame into the 4 KB **TX ring buffer**, sets the "transmit-empty interrupt" bit, and **returns immediately**. The main loop goes back to sampling.

From here the USART1 interrupt handler sends the bytes one at a time, at 115200 baud (about 87 µs per byte). A 400-byte frame takes about **35 ms** on the wire. The CPU spends that time sampling, not waiting.

If the ring were full (the ESP8266 can't stall the STM32 UART, so this only happens if we produce more than the link carries), `serial_write()` would refuse the *whole* frame. A partial frame would just become a checksum error at the other end. The refusal increments `telemetry_dropped`.

### On the ESP8266

1. The bytes arrive at UART0 (moved to pins D7/D8 by `Serial.swap()`) and sit in a **4 KB receive buffer**, filled by the ESP8266 core's interrupt.
2. `link_poll()` reads up to 2048 bytes per call. On `$` it starts a new frame. It collects bytes until `\n`.
3. `handle_frame()` checks the frame shape (`K:` … `*HH`), recomputes the XOR, and checks that the payload starts with `{` and ends with `}`.
4. Kind `T` → `mqtt_publish("aeris/telemetry", payload, ...)`, QoS 0, not retained.
5. If MQTT is down at that moment, the telemetry is **dropped** and `dropped_offline` is incremented. It is live data, and a 20 Hz backlog would arrive late and in a burst.

The ESP8266 never parses the JSON. It publishes exactly the bytes the STM32 produced.

### The broker and dashboard

Mosquitto passes the message to the Python subscriber, which records the host receive time, validates the fields, checks `sequence` for gaps, duplicates and reboots, and then plots and records the sample. That is outside the firmware and not covered here.

**Total latency, sensor to broker:** up to 50 ms waiting for the telemetry deadline, plus about 35 ms on the UART, plus a few ms of Wi-Fi. That is roughly 40–100 ms, which is fine for live plots. Device-side timestamps (`imu_time_ms`) mean the latency does not distort the time axis.

---

## Part 2: Dashboard → STM32 (a command)

The user enters a new QNH of `1009.0` hPa on the dashboard.

1. The dashboard publishes `{"cmd":"set_sea_level_hpa","value":1009.0}` on `aeris/command`.
2. On the ESP8266, PubSubClient calls `on_mqtt_message()`. It rejects the message if it is longer than 256 bytes or contains `$`, `\r`, `\n` or NUL. Any of those would break the line framing on the STM32 side.
3. `link_send('C', payload, len)` writes `$C:{...}*HH\n` to the STM32.
4. On the STM32, the USART1 interrupt puts the bytes in the RX ring. `uart_link_poll()` reassembles the frame and checks the checksum, and `handle_frame()` sees kind `C` and calls the registered callback, `on_command()` in `aeris_app.c`.
5. `on_command()` pulls out `"cmd"` and `"value"` with two small key lookups, range-checks the value against 800–1100 hPa, and stores it in `sea_level_hpa`.
6. It sends an `E` frame, `command_ack`, and sets `next_status_ms = now`, so a status message with the new reference goes out on the **next loop pass** instead of up to 1 s later.
7. From the next telemetry packet on, `altitude_m` is computed with the new reference, and `sea_level_hpa` in every packet shows which reference was used.

If the value had been out of range, step 5 would send `command_rejected` with a reason, and the dashboard would show it in the event log.

---

## Part 3: Heartbeats

The STM32 never gets an acknowledgement for telemetry, so it needs another way to know whether anyone is listening. Once a second the ESP8266 sends:

```text
$H:{"wifi":true,"mqtt":true,"rssi":-61}*HH
```

`uart_link.c` records the time and the two flags. `uart_link_esp_alive(now)` returns true if a heartbeat arrived in the last 3 s. That drives:

- `esp_link`, `esp_wifi`, `esp_mqtt` in the 1 Hz status message,
- the `esp_link_lost` / `esp_link_restored` events,
- the LED's 5 Hz blink when the sensors are fine but the bridge is gone.

Acquisition does **not** change when the link is lost. The STM32 keeps sampling and framing at the same rate. Frames sent while no one listens are lost on the wire, but the sequence counter keeps going, so the gap shows up later.
