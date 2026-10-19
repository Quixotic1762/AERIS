"""Central in-memory state shared by the MQTT thread and the Dash callbacks.

The MQTT network thread writes; Dash callbacks read snapshots. Every access
goes through one lock, and the live buffer is a bounded deque, so memory
use does not grow with run time. The CSV recorder gets its own copy of each
record (see recorder.py), so the plot window never limits what is recorded.
"""

from __future__ import annotations

import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Optional

from telemetry import SeqResult, TelemetryRecord


@dataclass
class Counters:
    received: int = 0          # telemetry messages received from MQTT
    accepted: int = 0          # passed validation and not duplicates
    invalid: int = 0           # rejected by validation
    duplicates: int = 0
    missing: int = 0           # inferred from sequence gaps
    out_of_order: int = 0
    resets: int = 0            # device reboots detected
    other_invalid: int = 0     # malformed status/event messages


@dataclass
class Snapshot:
    records: list[TelemetryRecord]
    latest: Optional[TelemetryRecord]
    last_rx_age_s: Optional[float]
    mqtt_connected: bool
    mqtt_error: Optional[str]
    counters: Counters
    last_invalid_reason: Optional[str]
    stm32_status: Optional[dict]
    stm32_status_age_s: Optional[float]
    esp_status: Optional[dict]
    esp_status_age_s: Optional[float]
    events: list[tuple[str, dict]]
    epoch: int


@dataclass
class TelemetryStore:
    max_points: int = 6000
    max_events: int = 50

    _lock: threading.Lock = field(default_factory=threading.Lock, repr=False)
    _buffer: deque = field(init=False, repr=False)
    _events: deque = field(init=False, repr=False)
    _latest: Optional[TelemetryRecord] = None
    _last_rx_mono: Optional[float] = None
    _mqtt_connected: bool = False
    _mqtt_error: Optional[str] = None
    _counters: Counters = field(default_factory=Counters)
    _last_invalid_reason: Optional[str] = None
    _stm32_status: Optional[dict] = None
    _stm32_status_mono: Optional[float] = None
    _esp_status: Optional[dict] = None
    _esp_status_mono: Optional[float] = None
    _epoch: int = 0            # bumps when the plot buffer is cleared

    def __post_init__(self) -> None:
        self._buffer = deque(maxlen=self.max_points)
        self._events = deque(maxlen=self.max_events)

    # ---- writers (MQTT thread) -------------------------------------------

    def add_record(self, rec: TelemetryRecord, seq: SeqResult, tracker_counts: dict) -> None:
        with self._lock:
            self._counters.received += 1
            self._counters.missing = tracker_counts["missing"]
            self._counters.duplicates = tracker_counts["duplicates"]
            self._counters.out_of_order = tracker_counts["out_of_order"]
            self._counters.resets = tracker_counts["resets"]
            self._last_rx_mono = time.monotonic()
            if seq is SeqResult.DUPLICATE:
                return
            if seq is SeqResult.RESET:
                # Device clock restarted: old and new samples can't share an axis.
                self._buffer.clear()
                self._epoch += 1
            self._counters.accepted += 1
            self._buffer.append(rec)
            self._latest = rec

    def add_invalid(self, reason: str, telemetry: bool = True) -> None:
        with self._lock:
            if telemetry:
                self._counters.received += 1
                self._counters.invalid += 1
            else:
                self._counters.other_invalid += 1
            self._last_invalid_reason = reason

    def set_stm32_status(self, status: dict) -> None:
        with self._lock:
            self._stm32_status = status
            self._stm32_status_mono = time.monotonic()

    def set_esp_status(self, status: dict) -> None:
        with self._lock:
            self._esp_status = status
            self._esp_status_mono = time.monotonic()

    def add_event(self, event: dict) -> None:
        stamp = time.strftime("%H:%M:%S")
        with self._lock:
            self._events.appendleft((stamp, event))

    def set_mqtt_state(self, connected: bool, error: Optional[str] = None) -> None:
        with self._lock:
            self._mqtt_connected = connected
            self._mqtt_error = error

    # ---- readers (Dash callbacks) ----------------------------------------

    def snapshot(self, window_s: Optional[float] = None, include_records: bool = True) -> Snapshot:
        now = time.monotonic()
        with self._lock:
            records: list[TelemetryRecord] = []
            if include_records and self._buffer:
                if window_s is None:
                    records = list(self._buffer)
                else:
                    cutoff = self._buffer[-1].device_time_ms - int(window_s * 1000)
                    # Buffer is in arrival order; walk back from the newest.
                    for rec in reversed(self._buffer):
                        if rec.device_time_ms < cutoff:
                            break
                        records.append(rec)
                    records.reverse()
            return Snapshot(
                records=records,
                latest=self._latest,
                last_rx_age_s=None if self._last_rx_mono is None else now - self._last_rx_mono,
                mqtt_connected=self._mqtt_connected,
                mqtt_error=self._mqtt_error,
                counters=Counters(**vars(self._counters)),
                last_invalid_reason=self._last_invalid_reason,
                stm32_status=dict(self._stm32_status) if self._stm32_status else None,
                stm32_status_age_s=None if self._stm32_status_mono is None
                else now - self._stm32_status_mono,
                esp_status=dict(self._esp_status) if self._esp_status else None,
                esp_status_age_s=None if self._esp_status_mono is None
                else now - self._esp_status_mono,
                events=list(self._events),
                epoch=self._epoch,
            )


def connection_state(snap: Snapshot, stale_timeout_s: float) -> str:
    """CONNECTED, STALE or DISCONNECTED (spec 8.4)."""
    if not snap.mqtt_connected:
        return "DISCONNECTED"
    if snap.last_rx_age_s is None or snap.last_rx_age_s > stale_timeout_s:
        return "STALE"
    return "CONNECTED"
