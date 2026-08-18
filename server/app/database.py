from __future__ import annotations

import sqlite3
from collections.abc import Iterator
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


@contextmanager
def connect(database_path: str) -> Iterator[sqlite3.Connection]:
    connection = sqlite3.connect(database_path, timeout=5.0)
    connection.row_factory = sqlite3.Row
    try:
        yield connection
        connection.commit()
    except Exception:
        connection.rollback()
        raise
    finally:
        connection.close()


def initialize_database(database_path: str) -> None:
    path = Path(database_path)
    path.parent.mkdir(parents=True, exist_ok=True)

    with connect(database_path) as connection:
        connection.execute("PRAGMA journal_mode = WAL")
        connection.execute("PRAGMA synchronous = NORMAL")
        connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                received_at TEXT NOT NULL,
                temperature REAL NOT NULL,
                humidity REAL NOT NULL,
                pressure REAL NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_telemetry_device_received
                ON telemetry (device_id, received_at DESC);
            """
        )


def insert_telemetry(
    database_path: str,
    *,
    device_id: str,
    temperature: float,
    humidity: float,
    pressure: float,
    received_at: str | None = None,
) -> dict[str, Any]:
    timestamp = received_at or utc_now()

    with connect(database_path) as connection:
        cursor = connection.execute(
            """
            INSERT INTO telemetry (
                device_id,
                received_at,
                temperature,
                humidity,
                pressure
            ) VALUES (?, ?, ?, ?, ?)
            """,
            (device_id, timestamp, temperature, humidity, pressure),
        )
        record_id = cursor.lastrowid

    return {
        "id": record_id,
        "deviceId": device_id,
        "receivedAt": timestamp,
        "temperature": temperature,
        "humidity": humidity,
        "pressure": pressure,
    }


def get_latest_telemetry(
    database_path: str,
    device_id: str | None = None,
) -> dict[str, Any] | None:
    with connect(database_path) as connection:
        if device_id is None:
            row = connection.execute(
                """
                SELECT id, device_id, received_at, temperature, humidity, pressure
                FROM telemetry
                ORDER BY id DESC
                LIMIT 1
                """
            ).fetchone()
        else:
            row = connection.execute(
                """
                SELECT id, device_id, received_at, temperature, humidity, pressure
                FROM telemetry
                WHERE device_id = ?
                ORDER BY received_at DESC, id DESC
                LIMIT 1
                """,
                (device_id,),
            ).fetchone()

    return row_to_record(row) if row else None


def get_telemetry_history(
    database_path: str,
    device_id: str,
    *,
    since: str,
    limit: int,
) -> list[dict[str, Any]]:
    with connect(database_path) as connection:
        rows = connection.execute(
            """
            SELECT * FROM (
                SELECT id, device_id, received_at, temperature, humidity, pressure
                FROM telemetry
                WHERE device_id = ? AND received_at >= ?
                ORDER BY received_at DESC, id DESC
                LIMIT ?
            )
            ORDER BY received_at ASC, id ASC
            """,
            (device_id, since, limit),
        ).fetchall()

    return [row_to_record(row) for row in rows]


def get_telemetry_count(
    database_path: str,
    device_id: str,
    *,
    since: str,
) -> int:
    with connect(database_path) as connection:
        row = connection.execute(
            """
            SELECT COUNT(*) AS record_count
            FROM telemetry
            WHERE device_id = ? AND received_at >= ?
            """,
            (device_id, since),
        ).fetchone()

    return int(row["record_count"])


def get_database_status(database_path: str) -> dict[str, Any]:
    with connect(database_path) as connection:
        row = connection.execute(
            """
            SELECT COUNT(*) AS record_count, MAX(received_at) AS last_received_at
            FROM telemetry
            """
        ).fetchone()

    return {
        "recordCount": row["record_count"],
        "lastReceivedAt": row["last_received_at"],
    }


def row_to_record(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": row["id"],
        "deviceId": row["device_id"],
        "receivedAt": row["received_at"],
        "temperature": row["temperature"],
        "humidity": row["humidity"],
        "pressure": row["pressure"],
    }