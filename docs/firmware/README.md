# AERIS firmware: a guided tour

These documents walk through the two AERIS firmwares: what each file does, how the parts fit together at runtime, and why they were built that way. They are meant for someone who has to understand, demo, extend or debug the code, such as a teammate, a lab examiner, or you in three months.

For wiring, build and flash steps, see [`../../firmware/README.md`](../../firmware/README.md). For the exact wire formats (frames, JSON fields, MQTT topics), see [`../telemetry_protocol.md`](../telemetry_protocol.md). These guides cover the *how* and the *why*. They link to those two files rather than repeat them.

## Reading order

| # | Document | What you get out of it |
|---|---|---|
| 1 | **This page** | The system in one picture, the design principles, and a file map |
| 2 | [`01_life_of_a_sample.md`](01_life_of_a_sample.md) | One IMU reading followed from the sensor register to an MQTT message. Read this first if you only read one file. |
| 3 | [`02_stm32_architecture.md`](02_stm32_architecture.md) | STM32 layers, boot sequence, the cooperative main loop, the timebase, interrupts |
| 4 | [`03_stm32_sensors.md`](03_stm32_sensors.md) | I²C bus layer and recovery, the MPU6500 and BMP280 drivers, the sensor state machine, IMU averaging |
| 5 | [`04_stm32_comms_and_health.md`](04_stm32_comms_and_health.md) | Ring-buffered UART driver, frame format, JSON writer, commands, health events, the LED, the debug log |
| 6 | [`05_esp8266_bridge.md`](05_esp8266_bridge.md) | The Wi-Fi/MQTT bridge: frame parser, publishing, offline hold, reconnect back-off, command forwarding |
| 7 | [`06_design_decisions.md`](06_design_decisions.md) | Every major choice, the alternatives, the trade-off, and known limitations and next steps |

## The system in one picture

```text
                ┌──────────────────────────── STM32F446RE (Nucleo) ─────────────────────────────┐
 MPU6500 ──┐    │                                                                                │
  (IMU)    │I²C1│  i2c_bus ─► mpu6500 ─┐                                                         │
           ├────┤                     ├─► sensor_manager ─► telemetry (JSON) ─► uart_link ─► serial ├──USART1──┐
 BMP280 ───┘    │  i2c_bus ─► bmp280 ──┘   100 Hz IMU         20 Hz T frames    $K:..*HH   TX ring │ 115200 8N1│
  (baro)        │                          10 Hz baro         1 Hz  S frames                      │           │
                │                                             on change: E frames                 │           │
                │  health_monitor ─► LD2 LED              ◄── on_command ◄── uart_link ◄── serial ◄┼───────────┤
                │  debug_log ─► USART2 ─► ST-LINK USB (PC terminal)        H / C frames  RX ring   │           │
                └────────────────────────────────────────────────────────────────────────────────┘           │
                                                                                                             │
                ┌──────────────── ESP8266 (NodeMCU / D1 mini) ────────────────┐                              │
                │  link_poll ─► handle_frame ─► mqtt_publish ──────────────── ├──Wi-Fi──► Mosquitto ──► dashboard
                │  send_heartbeat (1 Hz)  ◄── on_mqtt_message (aeris/command) │ ◄────────  broker  ◄── (QNH cmd)
                │  publish_esp_status (5 s, retained, last will)              │                              │
                └─────────────────────────────────────────────────────────────┘◄─────────────────────────────┘
```

One sentence per board:

- **STM32** owns *measurement*: it samples the sensors on a fixed clock, validates every reading, timestamps it, numbers it, and packs it into JSON.
- **ESP8266** owns *networking*: it moves frames between the UART and MQTT and does no sensor processing at all.

## Design principles

Most decisions in the code follow from five rules. They come from the project spec (sections 2.3, 5.3, 10 and 13), and the code comments state them repeatedly.

1. **Acquisition never waits on the network.** Wi-Fi drops, broker restarts, a slow dashboard or a closed browser tab must not slow down or stop sampling. That is why the STM32 has no blocking calls in its loop, why UART transmission goes through an interrupt-driven ring buffer, and why a full buffer drops a frame instead of waiting.
2. **Every failure is bounded, counted and visible.** Each I²C transfer has a 5 ms timeout. Every dropped frame, bad checksum, sensor error and bus recovery increments a counter, and the counters are published once a second. Nothing fails silently.
3. **Invalid data is `null`, never `0`.** A reading of `0.0 g` is a plausible measurement. Sending `0` for "sensor broken" would corrupt plots and CSV files without anyone noticing. Validity is tracked per sensor group and travels with the data.
4. **Recover by yourself.** Sensors that fail are re-initialised every second. A locked I²C bus is unstuck by hand. The ESP8266 reconnects with exponential back-off. Nobody needs to press reset during a demo.
5. **Keep each part simple enough to test on its own.** One responsibility per module, no RTOS, no dynamic allocation on the STM32, ASCII frames you can read in a terminal, and a bring-up order (STM32 alone → add ESP8266 → add broker → add dashboard) where each step can be checked.

## File map

### STM32 (`firmware/stm32/AERIS_STM32/Core/`)

Grouped by layer, from the hardware up:

| Layer | Files | Role | Lines |
|---|---|---|---|
| Board / HAL glue | `main.c`, `main.h`, `stm32f4xx_hal_msp.c`, `stm32f4xx_it.c`, `stm32f4xx_hal_conf.h` | Clocks, pins, peripheral init, exception handlers | ~440 |
| Configuration | `aeris_config.h` | All rates, ranges, timeouts, addresses | 60 |
| Time | `timebase.c/.h` | 1 ms monotonic clock, wrap-safe comparisons | 36 |
| Bus drivers | `i2c_bus.c/.h`, `serial.c/.h` | I²C with timeouts and recovery; interrupt-driven UART rings | ~320 |
| Device drivers | `mpu6500.c/.h`, `bmp280.c/.h` | Register-level sensor drivers | ~480 |
| Services | `sensor_manager.c/.h`, `uart_link.c/.h`, `telemetry.c/.h`, `health_monitor.c/.h`, `debug_log.c/.h` | Scheduling, framing, JSON, health, logging | ~1000 |
| Application | `aeris_app.c/.h` | Main loop, packet scheduling, command handling | ~260 |

A module only calls modules in the layers below it. The two exceptions are callbacks: `uart_link` calls `aeris_app`'s `on_command`, and `health_monitor` calls `aeris_app`'s `send_event`. Callbacks keep the dependency direction one-way: the lower module never `#include`s the application.

### ESP8266 (`firmware/esp8266/aeris_esp8266/`)

| File | Role | Lines |
|---|---|---|
| `config.h` | Wi-Fi credentials, broker address, topics, timing | 49 |
| `aeris_esp8266.ino` | The whole bridge: UART frame parser, MQTT client, offline hold, diagnostics | 422 |

The ESP8266 sketch is one file because it has one job. Splitting 400 lines across several Arduino tabs would make it harder to follow.

## Glossary

| Term | Meaning here |
|---|---|
| **Frame** | One line on the UART: `$K:<json>*HH\n` |
| **Kind** | The letter `K`: `T` telemetry, `S` status, `E` event (STM32 → ESP), `H` heartbeat, `C` command (ESP → STM32) |
| **Window** | The IMU samples taken between two telemetry packets (about 5), which are averaged into one value |
| **Phase / state** | Internal sensor phases are `RESET → WAIT → RUN ⇄ FAULT`; the published state is `init` / `ok` / `fault` |
| **Deadline** | A `uint32_t` millisecond timestamp at which a periodic task is next due |
| **QNH / sea-level pressure** | Reference pressure used to turn pressure into an altitude estimate. Settable from the dashboard. |
| **Heartbeat** | The `H` frame the ESP8266 sends every second, so the STM32 knows the bridge is alive |
| **Bus recovery** | Manually clocking SCL to free an I²C slave that is holding SDA low |
