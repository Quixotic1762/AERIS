# AERIS — Aviation Experimental Recording & Instrumentation System

An STM32F446RE samples an MPU6500 IMU and a BMP280 barometer and sends JSON telemetry over UART (115200 baud) to an ESP8266. The ESP8266 publishes it to a Mosquitto broker over Wi-Fi. A Python Dash app plots the data live and records it to CSV.

> Laboratory/educational demonstrator. Not a certified aircraft instrument.

```text
MPU6500 ─┐ I2C1          USART1 115200 8N1            Wi-Fi / MQTT
BMP280 ──┴─► STM32F446RE ─────────────────► ESP8266 ─────────────► Mosquitto ──► dashboard/app.py
            100 Hz IMU     $T:{json}*HH\n              aeris/telemetry          live plots + CSV
            10 Hz baro     ◄── heartbeat / commands    aeris/status, event
            20 Hz packets                              aeris/command ◄──────────── QNH setting
```

## Contents

| Path | What |
|---|---|
| `firmware/stm32/AERIS_STM32/Core/` | STM32CubeIDE sources (HAL, bare-metal) |
| `firmware/esp8266/aeris_esp8266/` | Arduino IDE sketch for the Wi-Fi/MQTT bridge |
| `firmware/README.md` | **Wiring**, and build and flash steps for both boards |
| `dashboard/` | Python receiver, validation, CSV recorder and Dash web UI |
| `tools/simulate_device.py` | Publishes synthetic telemetry, for testing without hardware |
| `tests/` | pytest suite: validation, sequence tracking, recorder, MQTT end-to-end |
| `docs/telemetry_protocol.md` | UART frames, MQTT topics, JSON fields, CSV columns |
| `docs/mosquitto/aeris.conf` | Password-protected LAN broker configuration |
| `config.yaml` | Dashboard settings (broker, refresh rates, plot window, data directory) |

## Quick start

### 1. Broker (Ubuntu)

```bash
sudo apt install mosquitto mosquitto-clients
sudo cp docs/mosquitto/aeris.conf /etc/mosquitto/conf.d/
sudo mosquitto_passwd -c /etc/mosquitto/aeris_passwd aeris
sudo chown mosquitto: /etc/mosquitto/aeris_passwd
sudo systemctl restart mosquitto
```

### 2. Dashboard

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
# edit config.yaml: mqtt.username / mqtt.password
python dashboard/app.py
```

Open <http://127.0.0.1:8050>.

### 3. Try it without hardware

In a second terminal:

```bash
source .venv/bin/activate
python tools/simulate_device.py --loss 0.01 --invalid 0.005 --pause-at 30
```

You should see live traces, a non-zero *Missing* count, rejected messages, and the dashboard going **STALE** at 30 s.

### 4. Hardware

Follow [`firmware/README.md`](firmware/README.md): wire the boards, build the STM32 project in STM32CubeIDE, set Wi-Fi and broker details in the ESP8266 sketch's `config.h`, and upload it from the Arduino IDE.

## Dashboard features (spec section 8)

- Header: device ID, `CONNECTED` / `STALE` / `DISCONNECTED`, last-message age, recording state and filename.
- Live readouts and five plots: acceleration, angular rate, pressure, temperature, altitude estimate. The X axis is device time since boot, and every Y axis is labelled with its unit.
- Invalid values show as `—` in the readouts and as gaps in the plots, never as 0.
- Diagnostics: sequence number, missing, duplicate and out-of-order packets, reboots, invalid messages and the last rejection reason; STM32 sensor states and error counters; ESP8266 RSSI and frame counters; recorder rows, queued, dropped and errors; device event log.
- Recording: **Start** creates `data/aeris_YYYY-MM-DD_HH-MM-SS.csv` (host local time). **Stop** flushes and closes it. **Download last** sends the finished file to the browser.
- Altitude reference (QNH): sends `set_sea_level_hpa` to the STM32. The reference in use is stored in every packet and CSV row.
- The MQTT client reconnects automatically. Closing the browser does not stop reception or recording.

## Tests

```bash
pytest                                                          # unit tests
AERIS_TEST_BROKER=localhost:1883 AERIS_TEST_USER=aeris AERIS_TEST_PASSWORD=... pytest tests/test_mqtt_integration.py
```

## Known limitations

- Each telemetry packet carries the **mean** of about 5 IMU samples (100 Hz → 20 Hz). Barometer fields can be up to 100 ms older than the IMU fields; each has its own timestamp.
- `device_time_ms` is time since STM32 boot, not wall-clock time, and wraps after about 49.7 days. The host receive time is recorded separately.
- Telemetry produced while Wi-Fi or MQTT is down is **dropped** and shows up as sequence gaps. Only events and the latest status are held. There is no on-device storage (spec 9.4).
- Altitude is a pressure-derived estimate and depends on the QNH you set. The QNH resets to 1013.25 hPa when the STM32 reboots.
- The BMP280 temperature is the sensor die temperature, not air temperature.
- No roll/pitch fusion, calibration offsets or magnetometer heading (spec section 15, future work).
- The dashboard runs on Flask's development server and is meant for single-user lab use.
