"""Telemetry schema validation and sequence tracking (schema_version 1).

The rules follow the AERIS specification, section 6.4:
  * every required field is validated before a message is used;
  * absent, non-numeric or non-finite measurements are invalid;
  * invalid measurements become None - never 0, since 0 is a valid reading.

See docs/telemetry_protocol.md for the message definitions.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from typing import Any, Optional

SCHEMA_VERSION = 1

IMU_FIELDS = ("accel_x_g", "accel_y_g", "accel_z_g", "gyro_x_dps", "gyro_y_dps", "gyro_z_dps")
BARO_FIELDS = ("pressure_hpa", "temperature_c", "altitude_m")

MAX_PAYLOAD_BYTES = 4096
MAX_DEVICE_ID_LEN = 64


class ValidationError(ValueError):
    """Raised when a message cannot be accepted."""


@dataclass(frozen=True)
class TelemetryRecord:
    host_receive_time_iso: str
    host_receive_time_unix_ms: int
    device_id: str
    sequence: int
    device_time_ms: int
    imu_time_ms: Optional[int]
    barometer_time_ms: Optional[int]
    accel_x_g: Optional[float]
    accel_y_g: Optional[float]
    accel_z_g: Optional[float]
    gyro_x_dps: Optional[float]
    gyro_y_dps: Optional[float]
    gyro_z_dps: Optional[float]
    pressure_hpa: Optional[float]
    temperature_c: Optional[float]
    altitude_m: Optional[float]
    sea_level_hpa: Optional[float]
    imu_valid: bool
    barometer_valid: bool
    mqtt_topic: str


def _reject_constant(name: str) -> Any:
    raise ValidationError(f"non-finite JSON constant {name}")


def decode_json_object(payload: bytes | str) -> dict:
    """Decode a JSON object, rejecting NaN/Infinity and oversized payloads."""
    if isinstance(payload, (bytes, bytearray)):
        if len(payload) > MAX_PAYLOAD_BYTES:
            raise ValidationError("payload too large")
        try:
            payload = payload.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValidationError("payload is not UTF-8") from exc
    elif len(payload) > MAX_PAYLOAD_BYTES:
        raise ValidationError("payload too large")
    try:
        obj = json.loads(payload, parse_constant=_reject_constant)
    except ValidationError:
        raise
    except (json.JSONDecodeError, RecursionError) as exc:
        raise ValidationError(f"malformed JSON: {exc}") from exc
    if not isinstance(obj, dict):
        raise ValidationError("payload is not a JSON object")
    return obj


def _is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _required_uint(obj: dict, key: str) -> int:
    v = obj.get(key)
    if not _is_int(v) or v < 0:
        raise ValidationError(f"'{key}' must be a non-negative integer")
    return v


def _optional_uint(obj: dict, key: str) -> tuple[Optional[int], bool]:
    """Returns (value, ok). ok is False if present but not a valid integer."""
    if key not in obj or obj[key] is None:
        return None, True
    v = obj[key]
    if _is_int(v) and v >= 0:
        return v, True
    return None, False


def _optional_float(obj: dict, key: str) -> tuple[Optional[float], bool]:
    """Returns (value, ok). Absent/null -> (None, True); bad type -> (None, False)."""
    if key not in obj or obj[key] is None:
        return None, True
    v = obj[key]
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None, False
    f = float(v)
    if not math.isfinite(f):
        return None, False
    return f, True


def parse_telemetry(payload: bytes | str, topic: str,
                    received_at: Optional[datetime] = None) -> TelemetryRecord:
    """Validate a telemetry payload and return a record, or raise ValidationError."""
    obj = decode_json_object(payload)

    if obj.get("schema_version") != SCHEMA_VERSION or isinstance(obj.get("schema_version"), bool):
        raise ValidationError(f"unsupported schema_version {obj.get('schema_version')!r}")

    device_id = obj.get("device_id")
    if not isinstance(device_id, str) or not device_id or len(device_id) > MAX_DEVICE_ID_LEN:
        raise ValidationError("'device_id' must be a non-empty string")

    sequence = _required_uint(obj, "sequence")
    device_time_ms = _required_uint(obj, "device_time_ms")

    validity = obj.get("validity")
    if validity is None:
        validity = {}
    if not isinstance(validity, dict):
        raise ValidationError("'validity' must be an object")
    imu_valid = validity.get("imu") is True
    baro_valid = validity.get("barometer") is True

    values: dict[str, Optional[float]] = {}
    for key in IMU_FIELDS + BARO_FIELDS + ("sea_level_hpa",):
        values[key], ok = _optional_float(obj, key)
        if not ok:
            if key in IMU_FIELDS:
                imu_valid = False
            elif key in BARO_FIELDS:
                baro_valid = False

    imu_time, ok_imu_t = _optional_uint(obj, "imu_time_ms")
    baro_time, ok_baro_t = _optional_uint(obj, "barometer_time_ms")

    # A group is valid only if the device says so AND every field is usable.
    # altitude_m is derived, so its absence alone does not invalidate the barometer.
    if not ok_imu_t or imu_time is None or any(values[k] is None for k in IMU_FIELDS):
        imu_valid = False
    if not ok_baro_t or baro_time is None or values["pressure_hpa"] is None \
            or values["temperature_c"] is None:
        baro_valid = False

    if not imu_valid:
        for k in IMU_FIELDS:
            values[k] = None
        imu_time = imu_time if ok_imu_t else None
    if not baro_valid:
        for k in BARO_FIELDS:
            values[k] = None
        baro_time = baro_time if ok_baro_t else None

    now = received_at or datetime.now().astimezone()
    if now.tzinfo is None:
        now = now.astimezone()

    return TelemetryRecord(
        host_receive_time_iso=now.isoformat(timespec="milliseconds"),
        host_receive_time_unix_ms=int(now.timestamp() * 1000),
        device_id=device_id,
        sequence=sequence,
        device_time_ms=device_time_ms,
        imu_time_ms=imu_time,
        barometer_time_ms=baro_time,
        accel_x_g=values["accel_x_g"],
        accel_y_g=values["accel_y_g"],
        accel_z_g=values["accel_z_g"],
        gyro_x_dps=values["gyro_x_dps"],
        gyro_y_dps=values["gyro_y_dps"],
        gyro_z_dps=values["gyro_z_dps"],
        pressure_hpa=values["pressure_hpa"],
        temperature_c=values["temperature_c"],
        altitude_m=values["altitude_m"],
        sea_level_hpa=values["sea_level_hpa"],
        imu_valid=imu_valid,
        barometer_valid=baro_valid,
        mqtt_topic=topic,
    )


# ---------------------------------------------------------------------------
# Sequence tracking
# ---------------------------------------------------------------------------

class SeqResult(Enum):
    FIRST = "first"              # first packet seen from this device
    OK = "ok"                    # exactly last + 1
    GAP = "gap"                  # one or more packets missing before this one
    DUPLICATE = "duplicate"      # same sequence and device time as the last packet
    OUT_OF_ORDER = "out_of_order"
    RESET = "reset"              # device rebooted (sequence and clock restarted)


# A packet whose sequence goes backwards is treated as a device reboot when
# the device clock went back by more than REBOOT_CLOCK_JUMP_MS, or the
# sequence went back by more than REBOOT_SEQ_JUMP packets. Otherwise it is
# a late (out-of-order) packet.
REBOOT_CLOCK_JUMP_MS = 2000
REBOOT_SEQ_JUMP = 100


@dataclass
class _DeviceSeq:
    last_seq: int
    last_time_ms: int


class SequenceTracker:
    """Detects missing, repeated, out-of-order packets and device reboots."""

    def __init__(self) -> None:
        self._devices: dict[str, _DeviceSeq] = {}
        self.missing = 0
        self.duplicates = 0
        self.out_of_order = 0
        self.resets = 0

    def update(self, device_id: str, sequence: int, device_time_ms: int) -> SeqResult:
        st = self._devices.get(device_id)
        if st is None:
            self._devices[device_id] = _DeviceSeq(sequence, device_time_ms)
            return SeqResult.FIRST

        if sequence == st.last_seq + 1:
            result = SeqResult.OK
        elif sequence > st.last_seq + 1:
            self.missing += sequence - st.last_seq - 1
            result = SeqResult.GAP
        elif sequence == st.last_seq and device_time_ms == st.last_time_ms:
            self.duplicates += 1
            return SeqResult.DUPLICATE
        elif (device_time_ms + REBOOT_CLOCK_JUMP_MS < st.last_time_ms
              or sequence + REBOOT_SEQ_JUMP < st.last_seq):
            self.resets += 1
            result = SeqResult.RESET
        else:
            self.out_of_order += 1
            return SeqResult.OUT_OF_ORDER   # do not move the reference backwards

        st.last_seq = sequence
        st.last_time_ms = device_time_ms
        return result
