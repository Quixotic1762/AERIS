import csv
import time

import pytest

from recorder import CSV_COLUMNS, CsvRecorder, RecorderError
from telemetry import parse_telemetry
from test_telemetry import FIRMWARE_INVALID, FIRMWARE_PAYLOAD


def wait_rows(rec, n, timeout=5.0):
    end = time.monotonic() + timeout
    while rec.status().rows_written < n and time.monotonic() < end:
        time.sleep(0.01)


def read_csv(path):
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.reader(fh))


def test_start_write_stop(tmp_path):
    rec = CsvRecorder(tmp_path, flush_interval_s=0.05)
    path = rec.start()
    assert path.name.startswith("aeris_") and path.suffix == ".csv"
    assert rec.status().recording

    good = parse_telemetry(FIRMWARE_PAYLOAD, "aeris/telemetry")
    bad = parse_telemetry(FIRMWARE_INVALID, "aeris/telemetry")
    rec.submit(good)
    rec.submit(bad)
    done = rec.stop()
    assert done == path
    s = rec.status()
    assert not s.recording and s.rows_written == 2 and s.dropped == 0
    assert s.last_file == str(path)

    rows = read_csv(path)
    assert tuple(rows[0]) == CSV_COLUMNS
    row = dict(zip(rows[0], rows[1]))
    assert row["sequence"] == "42"
    assert row["device_time_ms"] == "123460"
    assert row["accel_z_g"] == "-1.0"
    assert row["gyro_y_dps"] == "0.0"
    assert row["imu_valid"] == "true"
    assert row["mqtt_topic"] == "aeris/telemetry"
    assert row["host_receive_time_iso"] == good.host_receive_time_iso

    row2 = dict(zip(rows[0], rows[2]))
    assert row2["accel_x_g"] == "" and row2["pressure_hpa"] == ""   # empty, not 0
    assert row2["imu_valid"] == "false"
    assert row2["imu_time_ms"] == ""


def test_records_ignored_when_not_recording(tmp_path):
    rec = CsvRecorder(tmp_path)
    rec.submit(parse_telemetry(FIRMWARE_PAYLOAD, "t"))
    assert rec.status().rows_written == 0
    assert rec.stop() is None


def test_multiple_recordings_get_separate_files(tmp_path):
    rec = CsvRecorder(tmp_path)
    p1 = rec.start()
    rec.stop()
    p2 = rec.start()
    rec.stop()
    assert p1 != p2 and p1.exists() and p2.exists()


def test_double_start_rejected(tmp_path):
    rec = CsvRecorder(tmp_path)
    rec.start()
    with pytest.raises(RecorderError):
        rec.start()
    rec.stop()


def test_unwritable_directory_reports_error(tmp_path):
    blocker = tmp_path / "not_a_dir"
    blocker.write_text("x")
    rec = CsvRecorder(blocker / "data")
    with pytest.raises(RecorderError):
        rec.start()
    assert rec.status().error and not rec.status().recording


def test_queue_overflow_is_counted(tmp_path):
    rec = CsvRecorder(tmp_path, queue_size=1)
    rec.start()
    r = parse_telemetry(FIRMWARE_PAYLOAD, "t")
    for _ in range(500):
        rec.submit(r)
    rec.stop()
    s = rec.status()
    assert s.rows_written + s.dropped == 500
    assert len(read_csv(s.last_file)) - 1 == s.rows_written


def test_write_failure_is_visible(tmp_path, monkeypatch):
    rec = CsvRecorder(tmp_path)
    rec.start()

    import recorder as recorder_mod

    def boom(_rec):
        raise OSError(28, "No space left on device")

    monkeypatch.setattr(recorder_mod, "record_to_row", boom)
    rec.submit(parse_telemetry(FIRMWARE_PAYLOAD, "t"))
    end = time.monotonic() + 5
    while rec.status().error is None and time.monotonic() < end:
        time.sleep(0.01)
    s = rec.status()
    assert "No space left" in s.error
    assert not s.recording
    rec.stop()
    # A new recording can be started after the failure.
    monkeypatch.undo()
    rec.start()
    rec.submit(parse_telemetry(FIRMWARE_PAYLOAD, "t"))
    rec.stop()
    assert rec.status().rows_written == 1


def test_csv_loads_with_pandas(tmp_path):
    pd = pytest.importorskip("pandas")
    rec = CsvRecorder(tmp_path)
    rec.start()
    rec.submit(parse_telemetry(FIRMWARE_PAYLOAD, "t"))
    rec.submit(parse_telemetry(FIRMWARE_INVALID, "t"))
    path = rec.stop()
    df = pd.read_csv(path)
    assert list(df.columns) == list(CSV_COLUMNS)
    assert df["pressure_hpa"].isna().tolist() == [False, True]
