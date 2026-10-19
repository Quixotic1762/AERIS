"""CSV recorder (spec section 9).

Each recording session gets its own file and its own writer thread. The
MQTT thread hands records over through a bounded queue, so a slow disk
never blocks MQTT and every record that cannot be queued is counted.

Columns are the ones listed in spec 9.2, plus `sea_level_hpa` (the altitude
reference pressure, spec section 13) appended at the end so the documented
column order is unchanged.
"""

from __future__ import annotations

import csv
import os
import queue
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

from telemetry import TelemetryRecord

CSV_COLUMNS = (
    "host_receive_time_iso",
    "host_receive_time_unix_ms",
    "device_id",
    "sequence",
    "device_time_ms",
    "imu_time_ms",
    "barometer_time_ms",
    "accel_x_g",
    "accel_y_g",
    "accel_z_g",
    "gyro_x_dps",
    "gyro_y_dps",
    "gyro_z_dps",
    "pressure_hpa",
    "temperature_c",
    "altitude_m",
    "imu_valid",
    "barometer_valid",
    "mqtt_topic",
    "sea_level_hpa",
)

_STOP = object()


class RecorderError(RuntimeError):
    pass


def _cell(value) -> str:
    """Missing values stay empty - never invented as 0 (spec 9.3)."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def record_to_row(rec: TelemetryRecord) -> list[str]:
    return [_cell(getattr(rec, col)) for col in CSV_COLUMNS]


@dataclass
class RecorderStatus:
    recording: bool
    filename: Optional[str]
    rows_written: int
    dropped: int
    queued: int
    error: Optional[str]
    last_file: Optional[str]


class CsvRecorder:
    def __init__(self, data_dir: Path, flush_interval_s: float = 1.0, queue_size: int = 50000):
        self.data_dir = Path(data_dir)
        self.flush_interval_s = flush_interval_s
        self.queue_size = queue_size

        self._lock = threading.Lock()
        self._queue: Optional[queue.Queue] = None
        self._thread: Optional[threading.Thread] = None
        self._path: Optional[Path] = None
        self._last_path: Optional[Path] = None
        self._active = False
        self._rows = 0
        self._dropped = 0
        self._error: Optional[str] = None

    # ---- control (Dash callbacks) ----------------------------------------

    def _new_path(self) -> Path:
        stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        path = self.data_dir / f"aeris_{stamp}.csv"
        n = 1
        while path.exists():
            path = self.data_dir / f"aeris_{stamp}_{n}.csv"
            n += 1
        return path

    def start(self) -> Path:
        with self._lock:
            if self._active:
                raise RecorderError("already recording")
            if self._thread is not None and not self._thread.is_alive():
                # Previous session ended on a write error; retire it.
                self._last_path, self._path, self._thread, self._queue = self._path, None, None, None
            try:
                self.data_dir.mkdir(parents=True, exist_ok=True)
                path = self._new_path()
                fh = path.open("x", newline="", encoding="utf-8")
                writer = csv.writer(fh)
                writer.writerow(CSV_COLUMNS)
                fh.flush()
            except OSError as exc:
                self._error = f"cannot create recording in {self.data_dir}: {exc}"
                raise RecorderError(self._error) from exc

            self._queue = queue.Queue(maxsize=self.queue_size)
            self._path = path
            self._rows = 0
            self._dropped = 0
            self._error = None
            self._active = True
            self._thread = threading.Thread(
                target=self._run, args=(fh, writer, self._queue), name="csv-recorder", daemon=True
            )
            self._thread.start()
            return path

    def stop(self, timeout_s: float = 10.0) -> Optional[Path]:
        """Flush pending rows and close the file. Returns the finished path."""
        with self._lock:
            if not self._active and self._thread is None:
                return None
            self._active = False
            q, thread, path = self._queue, self._thread, self._path
        if thread is not None and thread.is_alive() and q is not None:
            try:
                q.put(_STOP, timeout=timeout_s)   # waits for room if the queue is full
            except queue.Full:
                pass
            thread.join(timeout_s)
        with self._lock:
            self._thread = None
            self._queue = None
            if path is not None:
                self._last_path = path
            self._path = None
        return path

    # ---- data path (MQTT thread) -----------------------------------------

    def submit(self, rec: TelemetryRecord) -> None:
        q = self._queue
        if not self._active or q is None:
            return
        try:
            q.put_nowait(rec)
        except queue.Full:
            with self._lock:
                self._dropped += 1

    # ---- status ------------------------------------------------------------

    def status(self) -> RecorderStatus:
        with self._lock:
            return RecorderStatus(
                recording=self._active,
                filename=str(self._path) if self._path else None,
                rows_written=self._rows,
                dropped=self._dropped,
                queued=self._queue.qsize() if self._queue else 0,
                error=self._error,
                last_file=str(self._last_path) if self._last_path else None,
            )

    # ---- writer thread -----------------------------------------------------

    def _run(self, fh, writer, q: queue.Queue) -> None:
        last_flush = time.monotonic()
        try:
            while True:
                try:
                    item = q.get(timeout=0.25)
                except queue.Empty:
                    item = None
                if item is _STOP:
                    break
                if item is not None:
                    writer.writerow(record_to_row(item))
                    with self._lock:
                        self._rows += 1
                if time.monotonic() - last_flush >= self.flush_interval_s:
                    fh.flush()
                    last_flush = time.monotonic()
            fh.flush()
            os.fsync(fh.fileno())
        except (OSError, ValueError) as exc:
            with self._lock:
                self._error = f"write failed: {exc}"
                self._active = False
                # Whatever was still queued can no longer be written.
                self._dropped += q.qsize()
        finally:
            try:
                fh.close()
            except OSError as exc:
                with self._lock:
                    self._error = self._error or f"close failed: {exc}"
