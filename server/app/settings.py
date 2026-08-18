from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    database_path: str
    device_api_key: str
    dashboard_username: str
    dashboard_password: str

    @classmethod
    def from_environment(cls) -> "Settings":
        return cls(
            database_path=os.getenv("DATABASE_PATH", "/data/telemetry.db"),
            device_api_key=required_environment_variable("DEVICE_API_KEY"),
            dashboard_username=required_environment_variable("DASHBOARD_USERNAME"),
            dashboard_password=required_environment_variable("DASHBOARD_PASSWORD"),
        )


def required_environment_variable(name: str) -> str:
    value = os.getenv(name)
    if not value:
        raise RuntimeError(f"Required environment variable {name} is not set")
    return value