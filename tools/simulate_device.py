#!/usr/bin/env python3
"""Publish synthetic AERIS telemetry to an MQTT broker.

Stands in for the STM32 + ESP8266 so the dashboard, recorder and broker
can be tested without hardware. The generated values are synthetic and
are NOT real sensor measurements.

Examples:
    python tools/simulate_device.py                       # 20 Hz, broker from config.yaml
    python tools/simulate_device.py --loss 0.02 --invalid 0.01
    python tools/simulate_device.py --baro-fault-at 20 --pause-at 40 --pause-for 5
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dashboard"))
from config import load_config  # noqa: E402


def main() -> None:
    cfg = load_config()
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=cfg.mqtt.host)
    p.add_argument("--port", type=int, default=cfg.mqtt.port)
    p.add_argument("--username", default=cfg.mqtt.username)
    p.add_argument("--password", default=cfg.mqtt.password)
    p.add_argument("--prefix", default=cfg.mqtt.topic_prefix)
    p.add_argument("--device-id", default="aeris-sim")
    p.add_argument("--rate", type=float, default=20.0, help="telemetry packets per second")
    p.add_argument("--loss", type=float, default=0.0, help="fraction of packets skipped (sequence gaps)")
    p.add_argument("--invalid", type=float, default=0.0, help="fraction of malformed messages")
    p.add_argument("--baro-fault-at", type=float, help="seconds until a 5 s barometer fault")
    p.add_argument("--pause-at", type=float, help="seconds until publishing pauses (stale test)")
    p.add_argument("--pause-for", type=float, default=5.0)
    p.add_argument("--duration", type=float, help="stop after this many seconds")
    args = p.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"{args.device_id}-{random.randint(0, 9999)}")
    if args.username:
        client.username_pw_set(args.username, args.password)
    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()

    t_topic = f"{args.prefix}/telemetry"
    s_topic = f"{args.prefix}/status"
    e_topic = f"{args.prefix}/event"
    esp_topic = f"{args.prefix}/status/esp"

    def event(name: str, detail: str = "") -> None:
        client.publish(e_topic, json.dumps({
            "schema_version": 1, "device_id": args.device_id, "device_time_ms": now_ms(),
            "source": "stm32", "event": name, "detail": detail}))

    boot = time.monotonic()

    def now_ms() -> int:
        return int((time.monotonic() - boot) * 1000)

    client.publish(esp_topic, json.dumps({"schema_version": 1, "source": "esp8266",
                                          "device_id": args.device_id, "online": True,
                                          "ip": "simulated", "rssi_dbm": -55, "frames_ok": 0,
                                          "frames_bad": 0, "frames_oversize": 0,
                                          "dropped_offline": 0, "mqtt_connects": 1,
                                          "stm32_last_frame_age_ms": 0}), retain=True)
    event("boot", "simulator")

    period = 1.0 / args.rate
    seq = 0
    sent = 0
    next_t = time.monotonic()
    next_status = next_t
    baro_fault = False
    paused = False
    sea_level = 1013.25
    print(f"publishing to {args.host}:{args.port} {t_topic} at {args.rate:g} Hz (Ctrl+C to stop)")

    try:
        while True:
            next_t += period
            time.sleep(max(0.0, next_t - time.monotonic()))
            t = time.monotonic() - boot
            if args.duration and t > args.duration:
                break

            if args.pause_at is not None:
                should_pause = args.pause_at <= t < args.pause_at + args.pause_for
                if should_pause != paused:
                    paused = should_pause
                    print("pausing (dashboard should go STALE)" if paused else "resuming")
            if args.baro_fault_at is not None:
                fault = args.baro_fault_at <= t < args.baro_fault_at + 5
                if fault != baro_fault:
                    baro_fault = fault
                    event("baro_fault" if fault else "baro_recovered", "simulated")

            ms = now_ms()
            this_seq = seq
            seq += 1
            if paused or random.random() < args.loss:
                continue

            if random.random() < args.invalid:
                client.publish(t_topic, random.choice([
                    "{not json", '{"schema_version":1}', '{"schema_version":1,"device_id":"x","sequence":-1}',
                    '{"schema_version":1,"device_id":"x","sequence":1,"device_time_ms":1,"accel_x_g":NaN}',
                ]))
                continue

            # Slow rocking motion about X and Y, with sensor noise.
            roll = math.radians(25) * math.sin(2 * math.pi * 0.2 * t)
            pitch = math.radians(15) * math.sin(2 * math.pi * 0.13 * t + 1.0)
            ax = -math.sin(pitch) + random.gauss(0, 0.004)
            ay = math.sin(roll) * math.cos(pitch) + random.gauss(0, 0.004)
            az = math.cos(roll) * math.cos(pitch) + random.gauss(0, 0.004)
            gx = math.degrees(math.radians(25) * 2 * math.pi * 0.2 * math.cos(2 * math.pi * 0.2 * t)) + random.gauss(0, 0.15)
            gy = math.degrees(math.radians(15) * 2 * math.pi * 0.13 * math.cos(2 * math.pi * 0.13 * t + 1.0)) + random.gauss(0, 0.15)
            gz = random.gauss(0, 0.15)
            pressure = 1008.4 + 0.3 * math.sin(2 * math.pi * t / 60) + random.gauss(0, 0.01)
            temp = 25.0 + 0.5 * math.sin(2 * math.pi * t / 300) + random.gauss(0, 0.01)
            alt = 44330.0 * (1 - (pressure / sea_level) ** 0.1902949)
            baro_ok = not baro_fault
            baro_time = (ms // 100) * 100   # barometer runs at 10 Hz

            msg = {
                "schema_version": 1, "device_id": args.device_id, "sequence": this_seq,
                "device_time_ms": ms, "imu_time_ms": ms - 2,
                "barometer_time_ms": baro_time if baro_ok else None,
                "accel_x_g": round(ax, 4), "accel_y_g": round(ay, 4), "accel_z_g": round(az, 4),
                "gyro_x_dps": round(gx, 2), "gyro_y_dps": round(gy, 2), "gyro_z_dps": round(gz, 2),
                "pressure_hpa": round(pressure, 2) if baro_ok else None,
                "temperature_c": round(temp, 2) if baro_ok else None,
                "altitude_m": round(alt, 2) if baro_ok else None,
                "sea_level_hpa": sea_level,
                "validity": {"imu": True, "barometer": baro_ok},
            }
            client.publish(t_topic, json.dumps(msg, separators=(",", ":")))
            sent += 1

            if time.monotonic() >= next_status:
                next_status += 1.0
                client.publish(s_topic, json.dumps({
                    "schema_version": 1, "device_id": args.device_id, "device_time_ms": ms,
                    "source": "stm32", "fw_version": "sim", "uptime_s": ms // 1000,
                    "imu_state": "ok", "baro_state": "fault" if baro_fault else "ok",
                    "imu_who_am_i": "0x70", "baro_chip_id": "0x58",
                    "imu_read_errors": 0, "baro_read_errors": 0, "i2c_errors": 0, "i2c_recoveries": 0,
                    "telemetry_generated": seq, "telemetry_dropped": 0, "esp_link": True,
                    "sea_level_hpa": sea_level, "imu_rate_hz": 100, "baro_rate_hz": 10,
                    "telemetry_rate_hz": round(args.rate), "accel_range_g": 4, "gyro_range_dps": 500,
                }))
    except KeyboardInterrupt:
        pass
    finally:
        print(f"sent {sent} telemetry messages, last sequence {seq - 1}")
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
