# Design decisions

This is a record of the main firmware choices: what was decided, what else was considered, and why. It is useful for a viva or report ("why didn't you use X?") and for anyone who wants to change a decision and needs to know what it was protecting.

Each entry: **Decision → Alternatives → Reasoning → Cost.**

---

## Architecture

### Two microcontrollers instead of one

**Decision:** STM32 for sensors, ESP8266 for networking, connected by UART.

**Alternatives:** One ESP32 doing everything; or the ESP8266 reading the sensors directly.

**Reasoning:**
- The project brief specifies this hardware, and the split matches the spec (section 2.3).
- **Fault isolation:** Wi-Fi stacks are where the timing hiccups come from: TCP retransmits, reconnects, long blocking calls. On a separate chip they cannot disturb 100 Hz sampling. The STM32 keeps a steady schedule whatever the network does.
- The ESP8266 has one core shared with its Wi-Fi stack, so it can't promise steady 10 ms sampling.
- Each half can be built and tested alone: the STM32 with a serial terminal, the ESP8266 with `mosquitto_sub`.

**Cost:** A second firmware, a wire protocol to define and maintain, a UART that limits throughput (see "Link budget" below), and two boards to power.

### Bare-metal superloop, not FreeRTOS

**Decision:** One `while(1)` loop calling non-blocking `poll()` functions on deadlines.

**Alternatives:** FreeRTOS with IMU, barometer, telemetry and health tasks (spec 10.2); or timer interrupts that do the sampling.

**Reasoning:**
- Four short periodic jobs don't need preemption. The worst-case loop time is a few ms, well under the 10 ms IMU period.
- **No locking.** With tasks, the IMU accumulator, barometer reading and counters would all need mutexes or queues. In one loop they are plain variables.
- **Sampling in a timer interrupt** would give lower jitter, but then I²C (with its 5 ms timeout and recovery) would run in interrupt context. That blocks the UART interrupts, and shared data would need protecting. Polling from the loop with ~1 ms jitter is plenty for 100 Hz motion data.
- Smaller, simpler to debug, and nothing to configure (stack sizes, priorities).

**Cost:** One rule everyone must follow: **nothing in the loop may block.** One `HAL_Delay(100)` added anywhere would stall every task. That is why every header repeats it.

### The ESP8266 does not parse the data

**Decision:** The bridge checks frames and publishes payload bytes unchanged.

**Alternatives:** The ESP8266 parses JSON, adds a wall-clock timestamp, maybe re-encodes or batches.

**Reasoning:** One source of truth for data (the STM32). Network-side bugs can lose data but not corrupt it. Protocol changes only need an STM32 reflash. Less RAM and CPU use on the ESP8266. See [05 §1](05_esp8266_bridge.md#1-what-it-does-and-what-it-doesnt).

**Cost:** Without NTP, the ESP8266 doesn't add wall-clock time. The dashboard records the host receive time instead (spec 5.3).

---

## Data and timing

### Combined packets with averaged IMU (spec 5.2 option A)

**Decision:** Each 20 Hz packet = mean of ~5 IMU samples + latest barometer reading, each with its own timestamp.

**Alternatives:** Option B (separate IMU and barometer topics); sending every IMU sample; decimating (taking every 5th).

**Reasoning:**
- One packet per row makes plotting and CSV logging simple. The spec recommends this for a first version.
- Averaging uses all the samples, cuts noise by ~2.2×, and suppresses aliasing. Taking every 5th sample would throw away 80 % of the data.
- Separate timestamps (`imu_time_ms`, `barometer_time_ms`) make the up-to-100 ms difference between the groups explicit.

**Cost:** No individual 100 Hz samples on the host. Brief events shorter than ~50 ms are smeared out. Documented under "Known limitations".

### `null` for invalid data, per sensor group

**Decision:** Invalid or stale readings become JSON `null`, with `validity.imu` / `validity.barometer` flags.

**Alternatives:** Send 0; send the last good value; drop the packet entirely.

**Reasoning:** 0 is a real reading. The last good value hides failures (a frozen barometer looks like steady altitude). Dropping the packet loses the *other* sensor's good data too. `null` is honest, and JSON, pandas and Plotly handle it as "missing" without extra work.

### Sequence number counts packets *generated*, not *sent*

**Decision:** `sequence++` happens when the packet is built, before the send result is known.

**Reasoning:** Then a packet dropped at the UART on the STM32, lost on the wire, dropped by the ESP8266 while offline, or lost by MQTT QoS 0 all appear the same way: as a gap in `sequence` on the dashboard. One counter accounts for the whole pipeline. A reboot shows up as the sequence going back to 0.

### Monotonic time since boot, not wall-clock

**Decision:** `device_time_ms` = SysTick milliseconds since reset, with wrap-safe comparisons.

**Alternatives:** RTC with time set over NTP via the ESP8266.

**Reasoning:** The spec (5.3) asks for monotonic device time and a host receive time *separately*, and warns against calling time-since-boot "UTC". Clock sync adds a protocol, a failure mode ("is the clock set yet?") and a sync-status flag. For a lab session, relative time plus host time is enough.

**Cost:** The value wraps after 49.7 days, which is handled. It isn't directly comparable between reboots. HSI clock accuracy is about ±1 %.

### Barometer: ×16 oversampling, ×16 IIR filter, read at 10 Hz

**Reasoning:** Lowest noise (±1.3 cm altitude equivalent) for a slowly changing quantity. The sensor runs freely at ~26 Hz internally, so each 10 Hz read gets a recent value.

**Cost:** The IIR filter delays pressure changes by roughly a second. Fine for a trend plot, too slow for quick vertical movement.

### IMU: ±4 g, ±500 °/s, 41 Hz filter, 100 Hz

**Reasoning:** Ranges cover hand-held and bench motion with good resolution. The filter sits below the 50 Hz Nyquist limit to prevent aliasing. 100 Hz is the top of the spec's 50–100 Hz target.

**Cost:** Hard impacts can exceed ±4 g and clip. Change `MPU6500_ACCEL_RANGE_G` to 8 or 16; the compile-time table updates the scale factor, and the status message reports the new range.

---

## The link

### ASCII framed lines `$K:<json>*HH\n`

**Decision:** Human-readable frames with a start marker, kind letter and checksum.

**Alternatives:** Plain newline-delimited JSON; binary packets (COBS/SLIP framing, CBOR or protobuf payload).

**Reasoning:**
- Readable in any serial terminal, which is the main debugging tool in a student lab.
- `$` gives fast resynchronisation after lost bytes. The kind letter allows routing without parsing.
- JSON on the wire means the ESP8266 publishes it unchanged, and the dashboard reads the same format the STM32 writes.
- Binary would be ~3× smaller and allow higher rates, but every step would need a decoder, and you couldn't debug by looking.

**Cost:** About 75 % of the 115200-baud link at 20 Hz. This is the main limit on telemetry rate.

### XOR checksum instead of CRC

**Reasoning:** Catches all single-byte errors and most multi-byte ones. Trivial on both sides and easy to verify by hand. The ESP8266's JSON shape check and the dashboard's field validation sit behind it.

**Cost:** Misses some errors, such as two flips of the same bit position in different bytes. A CRC-16 would make that very unlikely, for a few more lines of code on each side. A reasonable upgrade if `frames_bad` ever shows real noise on the line.

### Link budget: 115200 baud, 20 Hz

**Numbers:** 115200 baud with 8N1 framing (10 bits per byte) ≈ 11,520 B/s. Telemetry 20 × ~390 = 7,800 B/s. Status ~860 B/s. That is about 75 % used.

**Options to go faster:** Raise the baud rate (both chips handle 460800 or 921600 easily on a short wire); shorten the JSON keys; drop decimal places; move to binary. Raising the baud rate is the cheapest. Change `LINK_BAUD` on both sides and `huart1.Init.BaudRate`.

### Ring-buffered, all-or-nothing UART TX

**Reasoning:** The CPU never waits for bytes to leave. A frame is either fully queued or not queued at all, so the receiver never sees half a frame. See [04](04_stm32_comms_and_health.md#serialc-never-block-on-the-uart).

**Alternative:** DMA. Less interrupt overhead, but more setup code, and not needed at 11.5 kB/s.

### Heartbeat from the ESP8266, 1 s period, 3 s timeout

**Reasoning:** Telemetry is fire-and-forget, so without a heartbeat the STM32 couldn't know whether anyone is listening. The heartbeat also carries Wi-Fi and MQTT state, so the STM32's status and LED can tell "bridge dead" apart from "bridge alive but offline". A 3× timeout tolerates one or two late heartbeats.

---

## Robustness

### Sensors as state machines with timed retries

**Reasoning:** Boot never blocks on a missing sensor. Sensors plugged in later get picked up within 1 s. Each sensor fails independently. See [03](03_stm32_sensors.md#the-state-machine).

### Read back the configuration every second

**Reasoning:** A power glitch resets a sensor to its default ranges, and its data still *looks* fine, just at the wrong scale. Reading the configuration back is the only way to catch it. The cost is 3 single-byte reads per second.

### Manual I²C bus recovery

**Reasoning:** A slave stuck holding SDA low survives an MCU reset and blocks the whole bus. The 9-clock + STOP procedure is the standard fix, plus a peripheral reset for the STM32F4's own stuck BUSY flag. It turns a "power-cycle everything" failure into a 100 µs hiccup with a counter.

### Different offline policies on the ESP8266 for telemetry, status and events

**Reasoning:** Drop the live stream (it would arrive late and wouldn't fit in RAM), keep the latest snapshot, queue the history. See [05 §5](05_esp8266_bridge.md#5-publishing-and-the-offline-policy).

### Exponential back-off for MQTT reconnects, capped at 16 s

**Reasoning:** Each attempt blocks for up to 2 s. Back-off keeps the ESP8266 mostly free to drain the UART during long outages, and the cap keeps recovery fast.

---

## Implementation details

### Integer float formatting

**Reasoning:** newlib-nano has no `%f` by default. Enabling it costs ~10 KB of flash. The integer formatter is 25 lines and handles rounding, `-0` and NaN. See [04](04_stm32_comms_and_health.md#the-json-writer).

### Hand-written JSON writer and key lookup

**Reasoning:** The STM32 writes three fixed document shapes and reads two small ones. A JSON library would be bigger than all of AERIS's own code. The writer still guarantees valid output (sticky overflow, escaping, no NaN).

**Cost:** The key lookup isn't a real parser and could be fooled by a key name inside a string value. Acceptable because the command set is small and the ESP8266 filters what it forwards.

### All configuration in `aeris_config.h`, with compile-time checks

**Reasoning:** One place to change behaviour, the status message always reports what the code does, and invalid ranges fail the build (`#error`).

### No dynamic memory on the STM32

**Reasoning:** Every buffer is static, so RAM use is known at link time, there is no fragmentation, and no allocation can fail at runtime. The ESP8266 does use Arduino `String` for its 8-slot event queue, which is convenient and bounded. `free_heap` in the bridge status would show a leak.

### CubeMX-compatible `main.c`

**Reasoning:** Fits the standard STM32CubeIDE workflow. The project can be regenerated from the `.ioc` file, and AERIS code survives because it lives in the `USER CODE` blocks and its own files.

---

## Limitations and next steps

What the current firmware does **not** do, roughly in order of usefulness:

| # | Limitation | Impact | Possible fix |
|---|---|---|---|
| 1 | **No watchdog** | A hard fault or an unexpected infinite loop stops the STM32 until someone presses reset. The fault handlers just spin. | Enable the IWDG (independent watchdog, ~1 s timeout), refreshed from `aeris_app_loop()`. Report the reset cause (`RCC->CSR`) in the `boot` event. |
| 2 | **ESP8266 can block for ~2 s inside `mqtt.connect()`** | The 4 KB RX buffer covers ~470 ms at the current link load. A connect attempt against an unreachable broker overflows it, so frames are lost (including events, which the STM32 does not resend) and `frames_bad` counts truncated ones. | Lower the socket timeout, connect asynchronously, or only attempt a reconnect right after draining the UART. Event loss could be fixed with acknowledged events (the STM32 resends `E` frames until the ESP8266 acks them). |
| 3 | **Command subscription is QoS 0** | The protocol doc says commands use QoS 1, and the dashboard publishes them at QoS 1. The bridge subscribes with `mqtt.subscribe(TOPIC_COMMAND, 0)`, so the broker delivers at QoS 0 (the lower of the two), and a command can be lost during a brief disconnect. | `mqtt.subscribe(TOPIC_COMMAND, 1)`. PubSubClient supports QoS 1 subscriptions. |
| 4 | **XOR checksum** | Weaker error detection than CRC. | CRC-16/CCITT on both ends (4 hex digits). |
| 5 | **QNH resets on reboot** | Altitude reference returns to 1013.25 hPa. It is visible in the data but easy to miss. | Store it in flash (EEPROM emulation), or have the dashboard re-send it when it sees a `boot` event. |
| 6 | **115200 baud caps telemetry at ~20 Hz** | Can't raise the rate or add fields. | 460800 baud on both sides, and/or shorter keys. |
| 7 | **Averaged IMU only** | No raw 100 Hz data on the host. | Batch 5 raw samples per frame at a higher baud rate, or use spec option B. |
| 8 | **No sensor calibration** | Gyro bias and accelerometer offsets are not removed. | Average the readings at rest at startup to estimate gyro bias, and send it in status. |
| 9 | **No attitude estimate** | No roll and pitch. | Complementary or Mahony filter on the STM32 at 100 Hz, labelled as an estimate (spec section 15). |
| 10 | **Credentials in `config.h`** | Easy to commit by accident. | Commit a `config.example.h` and git-ignore `config.h`. |
