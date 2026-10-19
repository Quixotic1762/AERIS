import json

import pytest

from telemetry import SeqResult, SequenceTracker, ValidationError, parse_telemetry

# Payload exactly as produced by the STM32 firmware (host-tested output).
FIRMWARE_PAYLOAD = (
    '{"schema_version":1,"device_id":"aeris-01","sequence":42,"device_time_ms":123460,'
    '"imu_time_ms":123456,"barometer_time_ms":123400,"accel_x_g":0.0213,"accel_y_g":-0.0005,'
    '"accel_z_g":-1.0000,"gyro_x_dps":-123.46,"gyro_y_dps":0.00,"gyro_z_dps":250.00,'
    '"pressure_hpa":1008.40,"temperature_c":25.10,"altitude_m":40.46,"sea_level_hpa":1013.25,'
    '"validity":{"imu":true,"barometer":true}}'
)

FIRMWARE_INVALID = (
    '{"schema_version":1,"device_id":"aeris-01","sequence":43,"device_time_ms":123510,'
    '"imu_time_ms":null,"barometer_time_ms":null,"accel_x_g":null,"accel_y_g":null,'
    '"accel_z_g":null,"gyro_x_dps":null,"gyro_y_dps":null,"gyro_z_dps":null,'
    '"pressure_hpa":null,"temperature_c":null,"altitude_m":null,"sea_level_hpa":1013.25,'
    '"validity":{"imu":false,"barometer":false}}'
)


def base(**over):
    d = json.loads(FIRMWARE_PAYLOAD)
    d.update(over)
    return json.dumps(d)


def test_parses_firmware_payload():
    r = parse_telemetry(FIRMWARE_PAYLOAD.encode(), "aeris/telemetry")
    assert r.device_id == "aeris-01"
    assert r.sequence == 42
    assert r.device_time_ms == 123460
    assert r.imu_time_ms == 123456 and r.barometer_time_ms == 123400
    assert r.accel_z_g == -1.0
    assert r.gyro_y_dps == 0.0          # zero is a valid measurement
    assert r.pressure_hpa == 1008.4
    assert r.imu_valid and r.barometer_valid
    assert r.mqtt_topic == "aeris/telemetry"
    assert r.host_receive_time_unix_ms > 0
    assert "T" in r.host_receive_time_iso


def test_invalid_groups_are_none_not_zero():
    r = parse_telemetry(FIRMWARE_INVALID, "aeris/telemetry")
    assert not r.imu_valid and not r.barometer_valid
    assert r.accel_x_g is None and r.pressure_hpa is None and r.altitude_m is None
    assert r.sea_level_hpa == 1013.25


@pytest.mark.parametrize("payload", [
    "{not json",
    "[1,2,3]",
    '"just a string"',
    base(schema_version=2),
    base(schema_version=True),
    base(device_id=""),
    base(device_id=5),
    base(sequence=-1),
    base(sequence=1.5),
    base(sequence=True),
    base(device_time_ms="123"),
    base(validity=[True]),
    '{"schema_version":1,"device_id":"a","sequence":1}',   # device_time_ms missing
    b"\xff\xfe",
])
def test_rejects_bad_messages(payload):
    with pytest.raises(ValidationError):
        parse_telemetry(payload, "aeris/telemetry")


def test_rejects_nan_constant():
    with pytest.raises(ValidationError):
        parse_telemetry(FIRMWARE_PAYLOAD.replace("0.0213", "NaN"), "aeris/telemetry")


def test_rejects_oversized_payload():
    with pytest.raises(ValidationError):
        parse_telemetry(b"{" + b" " * 5000 + b"}", "aeris/telemetry")


def test_non_numeric_field_invalidates_its_group_only():
    r = parse_telemetry(base(gyro_z_dps="fast"), "t")
    assert not r.imu_valid and r.accel_x_g is None
    assert r.barometer_valid and r.pressure_hpa == 1008.4


def test_missing_field_invalidates_group_even_if_device_claims_valid():
    d = json.loads(FIRMWARE_PAYLOAD)
    del d["pressure_hpa"]
    r = parse_telemetry(json.dumps(d), "t")
    assert not r.barometer_valid and r.temperature_c is None
    assert r.imu_valid


def test_device_validity_false_hides_values():
    r = parse_telemetry(base(validity={"imu": False, "barometer": True}), "t")
    assert not r.imu_valid and r.accel_x_g is None
    assert r.barometer_valid


def test_missing_validity_means_invalid():
    d = json.loads(FIRMWARE_PAYLOAD)
    del d["validity"]
    r = parse_telemetry(json.dumps(d), "t")
    assert not r.imu_valid and not r.barometer_valid


def test_sequence_tracker():
    t = SequenceTracker()
    assert t.update("d", 0, 0) is SeqResult.FIRST
    assert t.update("d", 1, 50) is SeqResult.OK
    assert t.update("d", 4, 200) is SeqResult.GAP
    assert t.missing == 2
    assert t.update("d", 4, 200) is SeqResult.DUPLICATE
    assert t.duplicates == 1
    assert t.update("d", 3, 150) is SeqResult.OUT_OF_ORDER
    assert t.out_of_order == 1
    assert t.update("d", 5, 250) is SeqResult.OK      # reference not moved back
    assert t.update("d", 6, 60_000) is SeqResult.OK
    # Reboot: sequence and clock restart.
    assert t.update("d", 0, 120) is SeqResult.RESET
    assert t.resets == 1
    assert t.update("d", 1, 170) is SeqResult.OK
    # Devices are tracked independently.
    assert t.update("other", 100, 9999) is SeqResult.FIRST


def test_sequence_tracker_reboot_detected_from_sequence_jump():
    t = SequenceTracker()
    t.update("d", 150, 3000)
    # Clock only went back 1.2 s, but the sequence restarted.
    assert t.update("d", 2, 1800) is SeqResult.RESET
