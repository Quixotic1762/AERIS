"""End-to-end test through a real broker. Skipped if none is reachable.

    AERIS_TEST_BROKER=localhost:1883 pytest tests/test_mqtt_integration.py
"""

import json
import os
import socket
import time

import paho.mqtt.client as mqtt
import pytest

from config import MqttConfig
from mqtt_client import MqttReceiver
from recorder import CsvRecorder
from store import TelemetryStore
from test_telemetry import FIRMWARE_PAYLOAD

HOST, _, PORT = os.environ.get("AERIS_TEST_BROKER", "localhost:1883").partition(":")
PORT = int(PORT or 1883)
USER = os.environ.get("AERIS_TEST_USER")
PASSWORD = os.environ.get("AERIS_TEST_PASSWORD")


def broker_available() -> bool:
    try:
        with socket.create_connection((HOST, PORT), timeout=0.5):
            return True
    except OSError:
        return False


pytestmark = pytest.mark.skipif(not broker_available(), reason=f"no MQTT broker at {HOST}:{PORT}")


def wait_for(cond, timeout=5.0):
    end = time.monotonic() + timeout
    while not cond() and time.monotonic() < end:
        time.sleep(0.02)
    return cond()


def test_publish_receive_record(tmp_path):
    prefix = f"aeris-test-{os.getpid()}"
    cfg = MqttConfig(host=HOST, port=PORT, username=USER, password=PASSWORD,
                     client_id=f"aeris-test-rx-{os.getpid()}", topic_prefix=prefix)
    store = TelemetryStore(max_points=100)
    rec = CsvRecorder(tmp_path)
    rx = MqttReceiver(cfg, store, rec)
    rx.start()
    try:
        assert wait_for(lambda: store.snapshot(include_records=False).mqtt_connected)
        time.sleep(0.2)   # let the subscription settle
        rec.start()

        pub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"aeris-test-tx-{os.getpid()}")
        if USER:
            pub.username_pw_set(USER, PASSWORD)
        pub.connect(HOST, PORT)
        pub.loop_start()
        base = json.loads(FIRMWARE_PAYLOAD)
        for seq in (0, 1, 2, 5):                 # 3 and 4 missing
            msg = dict(base, sequence=seq, device_time_ms=1000 + 50 * seq)
            pub.publish(f"{prefix}/telemetry", json.dumps(msg), qos=1).wait_for_publish()
        pub.publish(f"{prefix}/telemetry", "{garbage", qos=1).wait_for_publish()
        pub.publish(f"{prefix}/status", json.dumps({"source": "stm32", "imu_state": "ok"}),
                    qos=1).wait_for_publish()
        pub.loop_stop()
        pub.disconnect()

        assert wait_for(lambda: store.snapshot().counters.received == 5)
        snap = store.snapshot()
        assert snap.counters.accepted == 4
        assert snap.counters.invalid == 1
        assert snap.counters.missing == 2
        assert snap.latest.sequence == 5
        assert wait_for(lambda: store.snapshot().stm32_status is not None)

        path = rec.stop()
        with open(path, encoding="utf-8") as fh:
            assert len(fh.readlines()) == 1 + 4
    finally:
        rx.stop()
