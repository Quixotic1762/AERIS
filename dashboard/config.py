"""Configuration loading for the AERIS dashboard.

Values come from config.yaml (project root by default, or the path in the
AERIS_CONFIG environment variable / --config argument). Missing keys fall
back to the defaults below.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field, fields
from pathlib import Path
from typing import Any, Optional

import yaml

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG_PATH = PROJECT_ROOT / "config.yaml"


@dataclass
class MqttConfig:
    host: str = "localhost"
    port: int = 1883
    username: Optional[str] = None
    password: Optional[str] = None
    client_id: str = "aeris-dashboard"
    topic_prefix: str = "aeris"
    keepalive_s: int = 30

    @property
    def telemetry_topic(self) -> str:
        return f"{self.topic_prefix}/telemetry"

    @property
    def status_topic(self) -> str:
        return f"{self.topic_prefix}/status"

    @property
    def esp_status_topic(self) -> str:
        return f"{self.topic_prefix}/status/esp"

    @property
    def event_topic(self) -> str:
        return f"{self.topic_prefix}/event"

    @property
    def command_topic(self) -> str:
        return f"{self.topic_prefix}/command"


@dataclass
class DashboardConfig:
    host: str = "127.0.0.1"
    port: int = 8050
    refresh_ms: int = 250
    plot_refresh_ms: int = 500
    window_s: float = 60.0
    max_points: int = 6000
    max_plot_points: int = 1500
    stale_timeout_s: float = 2.0


@dataclass
class RecorderConfig:
    data_dir: str = "data"
    flush_interval_s: float = 1.0
    queue_size: int = 50000

    def resolved_dir(self) -> Path:
        p = Path(self.data_dir).expanduser()
        return p if p.is_absolute() else PROJECT_ROOT / p


@dataclass
class AppConfig:
    mqtt: MqttConfig = field(default_factory=MqttConfig)
    dashboard: DashboardConfig = field(default_factory=DashboardConfig)
    recorder: RecorderConfig = field(default_factory=RecorderConfig)


def _apply(section: Any, values: Optional[dict]) -> None:
    if not values:
        return
    if not isinstance(values, dict):
        raise ValueError(f"config section for {type(section).__name__} must be a mapping")
    known = {f.name: f for f in fields(section)}
    for key, value in values.items():
        if key not in known:
            raise ValueError(f"unknown config key '{key}' in {type(section).__name__}")
        default = getattr(section, key)
        # Coerce numbers so "8050" or 2 (for a float) behave as expected.
        if value is not None and isinstance(default, (int, float)) and not isinstance(default, bool):
            value = type(default)(value)
        setattr(section, key, value)


def load_config(path: Optional[str | os.PathLike] = None) -> AppConfig:
    """Load configuration, falling back to defaults if the file is absent."""
    cfg = AppConfig()
    if path is None:
        path = os.environ.get("AERIS_CONFIG", DEFAULT_CONFIG_PATH)
    path = Path(path)
    if path.exists():
        with path.open("r", encoding="utf-8") as fh:
            raw = yaml.safe_load(fh) or {}
        _apply(cfg.mqtt, raw.get("mqtt"))
        _apply(cfg.dashboard, raw.get("dashboard"))
        _apply(cfg.recorder, raw.get("recorder"))
    return cfg
