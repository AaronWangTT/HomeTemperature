from __future__ import annotations

import secrets
import time
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Annotated, AsyncIterator

from fastapi import Depends, FastAPI, Header, HTTPException, Query, status
from fastapi.responses import FileResponse
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from pydantic import BaseModel, Field

from app.database import (
    get_database_status,
    get_latest_telemetry,
    get_telemetry_count,
    get_telemetry_history,
    initialize_database,
    insert_telemetry,
)
from app.settings import Settings


class TelemetryInput(BaseModel):
    deviceId: str = Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$")
    temperature: float = Field(ge=-50, le=100)
    humidity: float = Field(ge=0, le=100)
    pressure: float = Field(ge=300, le=1200)


class TelemetryRecord(TelemetryInput):
    id: int
    receivedAt: str


class TelemetryCount(BaseModel):
    deviceId: str
    recordCount: int


basic_auth = HTTPBasic(auto_error=False)


def create_app(settings: Settings | None = None) -> FastAPI:
    app_settings = settings or Settings.from_environment()
    app_started_at = time.monotonic()

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncIterator[None]:
        initialize_database(app_settings.database_path)
        yield

    app = FastAPI(
        title="AZ3166 Telemetry Gateway",
        version="1.0.0",
        lifespan=lifespan,
        docs_url=None,
        redoc_url=None,
        openapi_url=None,
    )

    def require_device_key(
        x_device_key: Annotated[str | None, Header()] = None,
    ) -> None:
        if not x_device_key or not secrets.compare_digest(
            x_device_key,
            app_settings.device_api_key,
        ):
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid device key")

    def require_dashboard_auth(
        credentials: Annotated[HTTPBasicCredentials | None, Depends(basic_auth)],
    ) -> str:
        valid_username = credentials is not None and secrets.compare_digest(
            credentials.username,
            app_settings.dashboard_username,
        )
        valid_password = credentials is not None and secrets.compare_digest(
            credentials.password,
            app_settings.dashboard_password,
        )
        if not (valid_username and valid_password):
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Authentication required",
                headers={"WWW-Authenticate": 'Basic realm="AZ3166 Telemetry"'},
            )
        return credentials.username

    @app.get("/healthz")
    def health() -> dict[str, str]:
        return {"status": "ok"}

    @app.post(
        "/api/v1/telemetry",
        response_model=TelemetryRecord,
        status_code=status.HTTP_201_CREATED,
        dependencies=[Depends(require_device_key)],
    )
    def receive_telemetry(payload: TelemetryInput) -> dict[str, object]:
        return insert_telemetry(
            app_settings.database_path,
            device_id=payload.deviceId,
            temperature=payload.temperature,
            humidity=payload.humidity,
            pressure=payload.pressure,
        )

    @app.get(
        "/api/v1/telemetry/latest",
        response_model=TelemetryRecord,
        dependencies=[Depends(require_dashboard_auth)],
    )
    def latest_telemetry(
        deviceId: str | None = Query(default=None, min_length=1, max_length=64),
    ) -> dict[str, object]:
        record = get_latest_telemetry(app_settings.database_path, deviceId)
        if record is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="No telemetry found")
        return record

    @app.get(
        "/api/v1/telemetry/history",
        response_model=list[TelemetryRecord],
        dependencies=[Depends(require_dashboard_auth)],
    )
    def telemetry_history(
        deviceId: str | None = Query(default=None, min_length=1, max_length=64),
        hours: int = Query(default=24, ge=1, le=168),
        limit: int = Query(default=1000, ge=1, le=5000),
    ) -> list[dict[str, object]]:
        if deviceId is None:
            latest = get_latest_telemetry(app_settings.database_path)
            if latest is None:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="No telemetry found")
            deviceId = str(latest["deviceId"])

        since = (
            datetime.now(timezone.utc) - timedelta(hours=hours)
        ).isoformat(timespec="seconds").replace("+00:00", "Z")
        return get_telemetry_history(
            app_settings.database_path,
            deviceId,
            since=since,
            limit=limit,
        )

    @app.get(
        "/api/v1/telemetry/count",
        response_model=TelemetryCount,
        dependencies=[Depends(require_dashboard_auth)],
    )
    def telemetry_count(
        deviceId: str | None = Query(default=None, min_length=1, max_length=64),
        hours: int = Query(default=24, ge=1, le=168),
    ) -> dict[str, object]:
        if deviceId is None:
            latest = get_latest_telemetry(app_settings.database_path)
            if latest is None:
                raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="No telemetry found")
            deviceId = str(latest["deviceId"])

        since = (
            datetime.now(timezone.utc) - timedelta(hours=hours)
        ).isoformat(timespec="seconds").replace("+00:00", "Z")
        return {
            "deviceId": deviceId,
            "recordCount": get_telemetry_count(
                app_settings.database_path,
                deviceId,
                since=since,
            ),
        }

    @app.get(
        "/api/v1/status",
        dependencies=[Depends(require_dashboard_auth)],
    )
    def service_status() -> dict[str, object]:
        database_status = get_database_status(app_settings.database_path)
        latest = get_latest_telemetry(app_settings.database_path)
        return {
            "status": "ok",
            "uptimeSeconds": int(time.monotonic() - app_started_at),
            "activeDeviceId": latest["deviceId"] if latest else None,
            **database_status,
        }

    @app.get(
        "/",
        include_in_schema=False,
        dependencies=[Depends(require_dashboard_auth)],
    )
    @app.get(
        "/dashboard",
        include_in_schema=False,
        dependencies=[Depends(require_dashboard_auth)],
    )
    def dashboard() -> FileResponse:
        return FileResponse(Path(__file__).parent / "static" / "dashboard.html")

    return app