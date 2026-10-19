"""MQTT receiver: Paho network thread -> validation -> store + recorder.

The Paho callback never touches Plotly figures (spec 7.3). It validates
each message, then hands a copy to the live store and to the CSV recorder,
so both consumers see every record.
"""

from __future__ import annotations

import json
import logging
from typing import Optional

import paho.mqtt.client as mqtt

from config import MqttConfig
from recorder import CsvRecorder
from store import TelemetryStore
from telemetry import SeqResult, SequenceTracker, ValidationError, decode_json_object, parse_telemetry

log = logging.getLogger("aeris.mqtt")


class MqttReceiver:
    def __init__(self, cfg: MqttConfig, store: TelemetryStore, recorder: Optional[CsvRecorder]):
        self.cfg = cfg
        self.store = store
        self.recorder = recorder
        self.tracker = SequenceTracker()

        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=cfg.client_id,
            reconnect_on_failure=True,
        )
        if cfg.username:
            self.client.username_pw_set(cfg.username, cfg.password)
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message

    # ---- lifecycle -----------------------------------------------------------

    def start(self) -> None:
        log.info("connecting to MQTT broker %s:%d", self.cfg.host, self.cfg.port)
        # connect_async + loop_start: the network thread keeps retrying until
        # the broker is reachable, and reconnects after interruptions.
        self.client.connect_async(self.cfg.host, self.cfg.port, keepalive=self.cfg.keepalive_s)
        self.client.loop_start()

    def stop(self) -> None:
        try:
            self.client.disconnect()
        finally:
            self.client.loop_stop()

    def publish_command(self, command: dict) -> bool:
        if not self.client.is_connected():
            return False
        info = self.client.publish(self.cfg.command_topic, json.dumps(command), qos=1)
        return info.rc == mqtt.MQTT_ERR_SUCCESS

    # ---- callbacks (Paho network thread) -------------------------------------

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code.is_failure:
            msg = f"broker refused connection: {reason_code}"
            log.warning(msg)
            self.store.set_mqtt_state(False, msg)
            return
        log.info("connected; subscribing to %s/#", self.cfg.topic_prefix)
        client.subscribe(f"{self.cfg.topic_prefix}/#", qos=0)
        self.store.set_mqtt_state(True)

    def _on_disconnect(self, client, userdata, flags, reason_code, properties=None):
        msg = None if reason_code == 0 else f"disconnected: {reason_code}"
        if msg:
            log.warning(msg)
        self.store.set_mqtt_state(False, msg)

    def _on_message(self, client, userdata, msg: mqtt.MQTTMessage):
        # Never let an exception escape into Paho's network loop.
        try:
            self._dispatch(msg.topic, msg.payload)
        except Exception as exc:  # noqa: BLE001 - keep receiver alive
            log.exception("unexpected error handling %s", msg.topic)
            self.store.add_invalid(f"internal error: {exc}", telemetry=False)

    def _dispatch(self, topic: str, payload: bytes) -> None:
        cfg = self.cfg
        if topic == cfg.telemetry_topic:
            try:
                rec = parse_telemetry(payload, topic)
            except ValidationError as exc:
                self.store.add_invalid(str(exc))
                return
            seq = self.tracker.update(rec.device_id, rec.sequence, rec.device_time_ms)
            self.store.add_record(rec, seq, {
                "missing": self.tracker.missing,
                "duplicates": self.tracker.duplicates,
                "out_of_order": self.tracker.out_of_order,
                "resets": self.tracker.resets,
            })
            if self.recorder is not None and seq is not SeqResult.DUPLICATE:
                self.recorder.submit(rec)
            return

        if topic in (cfg.status_topic, cfg.esp_status_topic, cfg.event_topic):
            try:
                obj = decode_json_object(payload)
            except ValidationError as exc:
                self.store.add_invalid(f"{topic}: {exc}", telemetry=False)
                return
            if topic == cfg.status_topic:
                self.store.set_stm32_status(obj)
            elif topic == cfg.esp_status_topic:
                self.store.set_esp_status(obj)
                if obj.get("online") is False:
                    self.store.add_event({"source": "esp8266", "event": "bridge_offline",
                                          "detail": "ESP8266 last will received"})
            else:
                self.store.add_event(obj)
            return
        # Other topics (e.g. our own aeris/command) are ignored.
