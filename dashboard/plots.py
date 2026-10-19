"""Plotly figure builders for the live dashboard.

X axes use device time (seconds since STM32 boot) taken from each sensor's
own timestamp: imu_time_ms for IMU traces, barometer_time_ms for barometer
traces. Invalid samples are plotted as gaps, never as zero. Long windows
are thinned for display only; the CSV always keeps every record.
"""

from __future__ import annotations

import math
from typing import Iterable, Optional, Sequence

import plotly.graph_objects as go

from telemetry import TelemetryRecord

AXIS_COLORS = ("#38bdf8", "#f472b6", "#a3e635")   # X, Y, Z
PRESSURE_COLOR = "#fbbf24"
TEMPERATURE_COLOR = "#fb7185"
ALTITUDE_COLOR = "#a78bfa"

BG = "#0f151d"
GRID = "#223042"
TEXT = "#c9d4e0"
MUTED = "#7b8a9c"

X_TITLE = "Device time since boot (s)"


def _thin(records: Sequence[TelemetryRecord], max_points: int) -> Sequence[TelemetryRecord]:
    if max_points <= 0 or len(records) <= max_points:
        return records
    stride = math.ceil(len(records) / max_points)
    thinned = list(records[::stride])
    if thinned[-1] is not records[-1]:
        thinned.append(records[-1])   # always show the newest sample
    return thinned


def _layout(title: str, y_title: str, stale: bool, show_legend: bool) -> dict:
    layout = dict(
        title=dict(text=title, x=0.01, xanchor="left", font=dict(size=14, color=TEXT)),
        paper_bgcolor=BG,
        plot_bgcolor=BG,
        font=dict(family="JetBrains Mono, Consolas, monospace", size=11, color=TEXT),
        margin=dict(l=60, r=16, t=40, b=44),
        height=260,
        showlegend=show_legend,
        legend=dict(orientation="h", x=1, xanchor="right", y=1.12, bgcolor="rgba(0,0,0,0)"),
        xaxis=dict(title=dict(text=X_TITLE, font=dict(color=MUTED)), gridcolor=GRID,
                   zerolinecolor=GRID, showline=True, linecolor=GRID),
        yaxis=dict(title=dict(text=y_title, font=dict(color=MUTED)), gridcolor=GRID,
                   zerolinecolor="#34465c", showline=True, linecolor=GRID),
        uirevision="keep",   # preserve user zoom/pan between refreshes
        hovermode="x unified",
    )
    if stale:
        layout["annotations"] = [dict(
            text="STALE - no new telemetry", xref="paper", yref="paper", x=0.5, y=0.5,
            showarrow=False, font=dict(size=18, color="#ffb020"), opacity=0.85,
        )]
    return layout


def empty_figure(title: str, y_title: str, message: str = "Waiting for telemetry...") -> go.Figure:
    fig = go.Figure(layout=_layout(title, y_title, stale=False, show_legend=False))
    fig.add_annotation(text=message, xref="paper", yref="paper", x=0.5, y=0.5,
                       showarrow=False, font=dict(size=13, color=MUTED))
    return fig


def _imu_series(records: Iterable[TelemetryRecord], fields: Sequence[str]):
    xs: list[Optional[float]] = []
    ys: list[list[Optional[float]]] = [[] for _ in fields]
    for r in records:
        t = r.imu_time_ms if r.imu_time_ms is not None else r.device_time_ms
        xs.append(t / 1000.0)
        for i, f in enumerate(fields):
            ys[i].append(getattr(r, f) if r.imu_valid else None)
    return xs, ys


def imu_figure(records: Sequence[TelemetryRecord], kind: str, max_points: int,
               stale: bool) -> go.Figure:
    if kind == "accel":
        fields = ("accel_x_g", "accel_y_g", "accel_z_g")
        title, y_title = "Acceleration (specific force)", "Acceleration (g)"
    else:
        fields = ("gyro_x_dps", "gyro_y_dps", "gyro_z_dps")
        title, y_title = "Angular rate", "Angular rate (deg/s)"

    if not records:
        return empty_figure(title, y_title)

    xs, ys = _imu_series(_thin(records, max_points), fields)
    fig = go.Figure(layout=_layout(title, y_title, stale, show_legend=True))
    for axis, y, color in zip("XYZ", ys, AXIS_COLORS):
        fig.add_trace(go.Scatter(x=xs, y=y, mode="lines", name=axis,
                                 line=dict(width=1.4, color=color), connectgaps=False))
    return fig


def baro_figure(records: Sequence[TelemetryRecord], field: str, title: str, y_title: str,
                color: str, max_points: int, stale: bool) -> go.Figure:
    if not records:
        return empty_figure(title, y_title)

    xs: list[float] = []
    ys: list[Optional[float]] = []
    last_t: Optional[int] = None
    for r in _thin(records, max_points):
        if r.barometer_valid:
            # Telemetry runs faster than the barometer: the same reading
            # appears in several packets. Plot each reading once.
            if r.barometer_time_ms == last_t:
                continue
            last_t = r.barometer_time_ms
            xs.append(r.barometer_time_ms / 1000.0)
            ys.append(getattr(r, field))
        else:
            xs.append(r.device_time_ms / 1000.0)
            ys.append(None)   # gap

    fig = go.Figure(layout=_layout(title, y_title, stale, show_legend=False))
    fig.add_trace(go.Scatter(x=xs, y=ys, mode="lines", name=title,
                             line=dict(width=1.6, color=color), connectgaps=False))
    return fig
