"""AERIS live dashboard (Dash + Plotly).

Run from the project root:

    python dashboard/app.py                # uses ./config.yaml
    python dashboard/app.py --config other.yaml

Then open http://127.0.0.1:8050 in a browser.

One process runs the MQTT receiver thread, the CSV recorder thread and the
Dash web server (spec 2.3). Closing the browser tab does not stop MQTT
reception or recording.
"""

from __future__ import annotations

import argparse
import atexit
import logging
import math
from pathlib import Path
from typing import Optional

from dash import Dash, Input, Output, State, ctx, dcc, html, no_update

import plots
from config import AppConfig, load_config
from mqtt_client import MqttReceiver
from recorder import CsvRecorder, RecorderError
from store import Snapshot, TelemetryStore, connection_state

log = logging.getLogger("aeris")

READOUTS = (
    # (element id, label, record attribute, decimals)
    ("ro-ax", "X", "accel_x_g", 3),
    ("ro-ay", "Y", "accel_y_g", 3),
    ("ro-az", "Z", "accel_z_g", 3),
    ("ro-gx", "X", "gyro_x_dps", 2),
    ("ro-gy", "Y", "gyro_y_dps", 2),
    ("ro-gz", "Z", "gyro_z_dps", 2),
    ("ro-p", "Pressure", "pressure_hpa", 2),
    ("ro-t", "Temperature", "temperature_c", 2),
    ("ro-alt", "Altitude (est.)", "altitude_m", 1),
)


# ---------------------------------------------------------------------------
# Small view helpers
# ---------------------------------------------------------------------------

def _fmt(v, decimals: int = 2) -> str:
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "—"
    return f"{v:.{decimals}f}"


def _age(seconds: Optional[float]) -> str:
    if seconds is None:
        return "never"
    if seconds < 1:
        return f"{seconds * 1000:.0f} ms"
    if seconds < 120:
        return f"{seconds:.1f} s"
    return f"{seconds / 60:.1f} min"


def _tile(elem_id: str, label: str, unit: str) -> html.Div:
    return html.Div(className="tile", children=[
        html.Div(label, className="tile-label"),
        html.Div("—", id=elem_id, className="tile-value"),
        html.Div(unit, className="tile-unit"),
    ])


def _kv(label: str, elem_id: str) -> html.Div:
    return html.Div(className="kv", children=[
        html.Span(label, className="kv-label"),
        html.Span("—", id=elem_id, className="kv-value"),
    ])


def _row(label: str, value, level: str = "") -> html.Tr:
    return html.Tr([html.Td(label), html.Td(str(value), className=level)])


def _flag(ok: Optional[bool], yes: str = "VALID", no: str = "INVALID") -> tuple[str, str]:
    if ok is None:
        return "—", "muted"
    return (yes, "ok") if ok else (no, "bad")


def _count_level(n: int) -> str:
    return "warn" if n else ""


# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------

def build_layout(cfg: AppConfig) -> html.Div:
    graph_cfg = {"displaylogo": False, "responsive": True}

    return html.Div(className="page", children=[
        html.Header(className="header", children=[
            html.Div(className="brand", children=[
                html.H1("AERIS"),
                html.Div("Aviation Experimental Recording & Instrumentation System",
                         className="subtitle"),
            ]),
            html.Div(className="header-status", children=[
                html.Div("DISCONNECTED", id="conn-badge", className="badge bad"),
                _kv("Device", "hdr-device"),
                _kv("Last message", "hdr-age"),
                _kv("Recording", "hdr-rec"),
            ]),
        ]),

        html.Div(id="banner", className="banner hidden"),

        html.Section(className="readouts", children=[
            html.Div(className="group", children=[
                html.Div("Acceleration", className="group-title"),
                html.Div(className="tiles", children=[_tile(i, l, "g") for i, l, _, _ in READOUTS[0:3]]),
            ]),
            html.Div(className="group", children=[
                html.Div("Angular rate", className="group-title"),
                html.Div(className="tiles", children=[_tile(i, l, "deg/s") for i, l, _, _ in READOUTS[3:6]]),
            ]),
            html.Div(className="group", children=[
                html.Div("Barometer", className="group-title"),
                html.Div(className="tiles", children=[
                    _tile("ro-p", "Pressure", "hPa"),
                    _tile("ro-t", "Temperature", "°C"),
                    _tile("ro-alt", "Altitude (est.)", "m"),
                ]),
            ]),
        ]),

        html.Section(className="controls", children=[
            html.Div(className="control-group", children=[
                html.Span("CSV recording", className="control-title"),
                html.Button("Start recording", id="btn-start", className="btn btn-start"),
                html.Button("Stop recording", id="btn-stop", className="btn btn-stop"),
                html.Button("Download last", id="btn-download", className="btn"),
                html.Span(id="rec-msg", className="control-msg"),
            ]),
            html.Div(className="control-group", children=[
                html.Span("Altitude reference (QNH)", className="control-title"),
                dcc.Input(id="qnh-input", type="number", min=800, max=1100, step=0.01,
                          value=1013.25, className="input", debounce=True),
                html.Span("hPa", className="unit"),
                html.Button("Send to device", id="btn-qnh", className="btn"),
                html.Span(id="qnh-msg", className="control-msg"),
            ]),
        ]),

        html.Section(className="plots", children=[
            dcc.Graph(id="g-accel", config=graph_cfg, className="plot"),
            dcc.Graph(id="g-gyro", config=graph_cfg, className="plot"),
            dcc.Graph(id="g-pressure", config=graph_cfg, className="plot"),
            dcc.Graph(id="g-temp", config=graph_cfg, className="plot"),
            dcc.Graph(id="g-alt", config=graph_cfg, className="plot"),
        ]),

        html.Section(className="diagnostics", children=[
            html.Div(className="diag-card", children=[
                html.Div("Telemetry stream", className="group-title"),
                html.Table(id="diag-stream", className="diag-table"),
            ]),
            html.Div(className="diag-card", children=[
                html.Div("Sensors (STM32)", className="group-title"),
                html.Table(id="diag-sensors", className="diag-table"),
            ]),
            html.Div(className="diag-card", children=[
                html.Div("Radio bridge (ESP8266)", className="group-title"),
                html.Table(id="diag-esp", className="diag-table"),
            ]),
            html.Div(className="diag-card", children=[
                html.Div("Recorder", className="group-title"),
                html.Table(id="diag-rec", className="diag-table"),
            ]),
            html.Div(className="diag-card events", children=[
                html.Div("Device events", className="group-title"),
                html.Ul(id="events", className="event-list"),
            ]),
        ]),

        html.Footer(className="footer", children=[
            html.P("A paused or missing dashboard update does not necessarily mean the embedded "
                   "sensors stopped sampling: acquisition runs on the STM32 independently of "
                   "Wi-Fi, the broker and this page. Altitude is estimated from pressure and the "
                   "reference above; it is not a direct measurement. Not a certified instrument."),
        ]),

        dcc.Interval(id="tick", interval=cfg.dashboard.refresh_ms),
        dcc.Interval(id="plot-tick", interval=cfg.dashboard.plot_refresh_ms),
        dcc.Download(id="download"),
    ])


# ---------------------------------------------------------------------------
# Diagnostics rendering
# ---------------------------------------------------------------------------

def _stream_rows(snap: Snapshot, state: str) -> list:
    c = snap.counters
    latest = snap.latest
    rows = [
        _row("Connection", state, {"CONNECTED": "ok", "STALE": "warn"}.get(state, "bad")),
        _row("Last sequence", latest.sequence if latest else "—"),
        _row("Device time", f"{latest.device_time_ms / 1000:.2f} s" if latest else "—"),
        _row("Received / accepted", f"{c.received} / {c.accepted}"),
        _row("Missing (seq gaps)", c.missing, _count_level(c.missing)),
        _row("Duplicates", c.duplicates, _count_level(c.duplicates)),
        _row("Out of order", c.out_of_order, _count_level(c.out_of_order)),
        _row("Device reboots", c.resets, _count_level(c.resets)),
        _row("Invalid messages", c.invalid, "bad" if c.invalid else ""),
    ]
    if snap.last_invalid_reason:
        rows.append(_row("Last rejection", snap.last_invalid_reason, "bad"))
    if snap.mqtt_error:
        rows.append(_row("MQTT", snap.mqtt_error, "bad"))
    return rows


def _sensor_rows(snap: Snapshot) -> list:
    latest = snap.latest
    imu_txt, imu_lvl = _flag(latest.imu_valid if latest else None)
    baro_txt, baro_lvl = _flag(latest.barometer_valid if latest else None)
    rows = [_row("IMU data", imu_txt, imu_lvl), _row("Barometer data", baro_txt, baro_lvl)]
    if latest and latest.sea_level_hpa is not None:
        rows.append(_row("Altitude reference", f"{latest.sea_level_hpa:.2f} hPa"))

    st = snap.stm32_status
    if not st:
        rows.append(_row("STM32 status", "not received yet", "muted"))
        return rows
    stale = snap.stm32_status_age_s is not None and snap.stm32_status_age_s > 5
    state_lvl = {"ok": "ok", "init": "warn"}
    rows += [
        _row("Status age", _age(snap.stm32_status_age_s), "warn" if stale else ""),
        _row("IMU state", f"{st.get('imu_state', '?')} (WHO_AM_I {st.get('imu_who_am_i', '?')})",
             state_lvl.get(st.get("imu_state"), "bad")),
        _row("Baro state", f"{st.get('baro_state', '?')} (chip id {st.get('baro_chip_id', '?')})",
             state_lvl.get(st.get("baro_state"), "bad")),
        _row("IMU / baro read errors",
             f"{st.get('imu_read_errors', '?')} / {st.get('baro_read_errors', '?')}",
             _count_level((st.get("imu_read_errors") or 0) + (st.get("baro_read_errors") or 0))),
        _row("I2C errors / recoveries",
             f"{st.get('i2c_errors', '?')} / {st.get('i2c_recoveries', '?')}"),
        _row("UART TX dropped", st.get("telemetry_dropped", "?"),
             _count_level(st.get("telemetry_dropped") or 0)),
        _row("ESP heartbeat", "present" if st.get("esp_link") else "missing",
             "ok" if st.get("esp_link") else "bad"),
        _row("Rates (IMU/baro/tlm)",
             f"{st.get('imu_rate_hz', '?')}/{st.get('baro_rate_hz', '?')}/"
             f"{st.get('telemetry_rate_hz', '?')} Hz"),
        _row("Ranges", f"±{st.get('accel_range_g', '?')} g, ±{st.get('gyro_range_dps', '?')} deg/s"),
        _row("Uptime / firmware", f"{st.get('uptime_s', '?')} s / {st.get('fw_version', '?')}"),
    ]
    return rows


def _esp_rows(snap: Snapshot) -> list:
    st = snap.esp_status
    if not st:
        return [_row("Bridge status", "not received yet", "muted")]
    online = st.get("online") is True
    rows = [_row("Bridge", "ONLINE" if online else "OFFLINE (last will)", "ok" if online else "bad")]
    if online:
        rows += [
            _row("Status age", _age(snap.esp_status_age_s),
                 "warn" if (snap.esp_status_age_s or 0) > 15 else ""),
            _row("IP / RSSI", f"{st.get('ip', '?')} / {st.get('rssi_dbm', '?')} dBm"),
            _row("Frames ok / bad", f"{st.get('frames_ok', '?')} / {st.get('frames_bad', '?')}",
                 _count_level(st.get("frames_bad") or 0)),
            _row("Oversize frames", st.get("frames_oversize", "?"),
                 _count_level(st.get("frames_oversize") or 0)),
            _row("Dropped while offline", st.get("dropped_offline", "?"),
                 _count_level(st.get("dropped_offline") or 0)),
            _row("MQTT connects", st.get("mqtt_connects", "?")),
            _row("Last STM32 frame", f"{st.get('stm32_last_frame_age_ms', '?')} ms ago"),
        ]
    return rows


def _recorder_rows(rec: CsvRecorder) -> list:
    s = rec.status()
    rows = [
        _row("State", "RECORDING" if s.recording else "stopped", "rec" if s.recording else "muted"),
        _row("File", Path(s.filename).name if s.filename else "—"),
        _row("Rows written", s.rows_written),
        _row("Queued", s.queued, "warn" if s.queued > 1000 else ""),
        _row("Dropped", s.dropped, "bad" if s.dropped else ""),
        _row("Output directory", str(rec.data_dir)),
    ]
    if s.error:
        rows.append(_row("Error", s.error, "bad"))
    if s.last_file:
        rows.append(_row("Last file", Path(s.last_file).name))
    return rows


def _event_items(snap: Snapshot) -> list:
    if not snap.events:
        return [html.Li("No events received", className="muted")]
    items = []
    for stamp, ev in snap.events[:12]:
        name = str(ev.get("event", "?"))
        level = "bad" if ("fault" in name or "lost" in name or "offline" in name
                          or "rejected" in name or "dropping" in name) else ""
        items.append(html.Li([
            html.Span(stamp, className="ev-time"),
            html.Span(str(ev.get("source", "")), className="ev-src"),
            html.Span(name, className=f"ev-name {level}"),
            html.Span(str(ev.get("detail", "")), className="ev-detail"),
        ]))
    return items


# ---------------------------------------------------------------------------
# App factory
# ---------------------------------------------------------------------------

def create_app(cfg: AppConfig, store: TelemetryStore, recorder: CsvRecorder,
               receiver: Optional[MqttReceiver]) -> Dash:
    app = Dash(__name__, title="AERIS", update_title=None,
               assets_folder=str(Path(__file__).parent / "assets"))
    app.layout = build_layout(cfg)
    dcfg = cfg.dashboard

    readout_outputs = []
    for elem_id, *_ in READOUTS:
        readout_outputs += [Output(elem_id, "children"), Output(elem_id, "className")]

    @app.callback(
        Output("conn-badge", "children"), Output("conn-badge", "className"),
        Output("hdr-device", "children"), Output("hdr-age", "children"),
        Output("hdr-rec", "children"), Output("hdr-rec", "className"),
        Output("banner", "children"), Output("banner", "className"),
        *readout_outputs,
        Output("diag-stream", "children"), Output("diag-sensors", "children"),
        Output("diag-esp", "children"), Output("diag-rec", "children"),
        Output("events", "children"),
        Input("tick", "n_intervals"),
    )
    def refresh_panel(_n):
        snap = store.snapshot(include_records=False)
        state = connection_state(snap, dcfg.stale_timeout_s)
        latest = snap.latest
        rs = recorder.status()

        badge_cls = {"CONNECTED": "badge ok", "STALE": "badge warn"}.get(state, "badge bad")
        if rs.recording:
            rec_txt, rec_cls = f"● REC  {Path(rs.filename).name}", "kv-value rec"
        elif rs.error:
            rec_txt, rec_cls = "ERROR", "kv-value bad"
        else:
            rec_txt, rec_cls = "stopped", "kv-value muted"

        if state == "DISCONNECTED":
            banner = ("MQTT broker not connected - reconnecting automatically. "
                      "Recorded CSV data is kept.")
            banner_cls = "banner bad"
        elif state == "STALE":
            banner = (f"Telemetry STALE: no message for {_age(snap.last_rx_age_s)}. "
                      "Values below are the last received; the device may still be sampling.")
            banner_cls = "banner warn"
        else:
            banner, banner_cls = "", "banner hidden"

        stale_cls = " stale" if state != "CONNECTED" else ""
        values = []
        for _id, _label, attr, decimals in READOUTS:
            v = getattr(latest, attr) if latest else None
            values += [_fmt(v, decimals), ("tile-value invalid" if v is None else "tile-value") + stale_cls]

        return (
            state, badge_cls,
            latest.device_id if latest else "—",
            _age(snap.last_rx_age_s),
            rec_txt, rec_cls,
            banner, banner_cls,
            *values,
            _stream_rows(snap, state), _sensor_rows(snap), _esp_rows(snap),
            _recorder_rows(recorder), _event_items(snap),
        )

    @app.callback(
        Output("g-accel", "figure"), Output("g-gyro", "figure"),
        Output("g-pressure", "figure"), Output("g-temp", "figure"), Output("g-alt", "figure"),
        Input("plot-tick", "n_intervals"),
    )
    def refresh_plots(_n):
        snap = store.snapshot(window_s=dcfg.window_s)
        stale = connection_state(snap, dcfg.stale_timeout_s) != "CONNECTED" and bool(snap.records)
        recs, mp = snap.records, dcfg.max_plot_points
        return (
            plots.imu_figure(recs, "accel", mp, stale),
            plots.imu_figure(recs, "gyro", mp, stale),
            plots.baro_figure(recs, "pressure_hpa", "Pressure", "Pressure (hPa)",
                              plots.PRESSURE_COLOR, mp, stale),
            plots.baro_figure(recs, "temperature_c", "Temperature (BMP280)", "Temperature (°C)",
                              plots.TEMPERATURE_COLOR, mp, stale),
            plots.baro_figure(recs, "altitude_m", "Altitude (pressure-derived estimate)",
                              "Altitude (m)", plots.ALTITUDE_COLOR, mp, stale),
        )

    @app.callback(
        Output("rec-msg", "children"),
        Input("btn-start", "n_clicks"), Input("btn-stop", "n_clicks"),
        prevent_initial_call=True,
    )
    def record_control(_start, _stop):
        try:
            if ctx.triggered_id == "btn-start":
                path = recorder.start()
                return f"Recording to {path.name}"
            path = recorder.stop()
            if path is None:
                return "Not recording"
            s = recorder.status()
            return f"Saved {path.name} ({s.rows_written} rows, {s.dropped} dropped)"
        except RecorderError as exc:
            return f"Error: {exc}"

    @app.callback(
        Output("download", "data"),
        Input("btn-download", "n_clicks"),
        prevent_initial_call=True,
    )
    def download(_n):
        last = recorder.status().last_file
        if not last or not Path(last).exists():
            return no_update
        return dcc.send_file(last)

    @app.callback(
        Output("qnh-msg", "children"),
        Input("btn-qnh", "n_clicks"),
        State("qnh-input", "value"),
        prevent_initial_call=True,
    )
    def send_qnh(_n, value):
        if receiver is None:
            return "MQTT receiver not running"
        try:
            v = float(value)
        except (TypeError, ValueError):
            return "Enter a pressure in hPa"
        if not 800 <= v <= 1100:
            return "Must be 800-1100 hPa"
        ok = receiver.publish_command({"cmd": "set_sea_level_hpa", "value": round(v, 2)})
        return f"Sent {v:.2f} hPa - see Device events for the ack" if ok else "Not sent: MQTT disconnected"

    return app


def main(argv: Optional[list[str]] = None) -> None:
    parser = argparse.ArgumentParser(description="AERIS live dashboard")
    parser.add_argument("--config", help="path to config.yaml")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    cfg = load_config(args.config)

    store = TelemetryStore(max_points=cfg.dashboard.max_points)
    recorder = CsvRecorder(cfg.recorder.resolved_dir(), cfg.recorder.flush_interval_s,
                           cfg.recorder.queue_size)
    receiver = MqttReceiver(cfg.mqtt, store, recorder)
    receiver.start()

    def shutdown():
        recorder.stop()   # flush and close any open recording
        receiver.stop()

    atexit.register(shutdown)

    app = create_app(cfg, store, recorder, receiver)
    log.info("dashboard on http://%s:%d", cfg.dashboard.host, cfg.dashboard.port)
    # debug=False: the reloader would start a second MQTT client.
    app.run(host=cfg.dashboard.host, port=cfg.dashboard.port, debug=False)


if __name__ == "__main__":
    main()
