import tempfile
import unittest
from pathlib import Path

from app.database import (
    get_database_status,
    get_latest_telemetry,
    get_telemetry_count,
    get_telemetry_history,
    initialize_database,
    insert_telemetry,
)


class DatabaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.database_path = str(Path(self.temporary_directory.name) / "telemetry.db")
        initialize_database(self.database_path)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_latest_and_history_are_scoped_to_device(self) -> None:
        insert_telemetry(
            self.database_path,
            device_id="az3166-01",
            temperature=25.0,
            humidity=50.0,
            pressure=1008.0,
            received_at="2026-08-16T10:00:00Z",
        )
        insert_telemetry(
            self.database_path,
            device_id="az3166-02",
            temperature=30.0,
            humidity=60.0,
            pressure=1005.0,
            received_at="2026-08-16T10:01:00Z",
        )
        insert_telemetry(
            self.database_path,
            device_id="az3166-01",
            temperature=26.5,
            humidity=58.3,
            pressure=1008.4,
            received_at="2026-08-16T10:02:00Z",
        )

        latest = get_latest_telemetry(self.database_path, "az3166-01")
        history = get_telemetry_history(
            self.database_path,
            "az3166-01",
            since="2026-08-16T00:00:00Z",
            limit=100,
        )
        status = get_database_status(self.database_path)
        latest_report = get_latest_telemetry(self.database_path)
        recent_count = get_telemetry_count(
            self.database_path,
            "az3166-01",
            since="2026-08-16T10:01:00Z",
        )

        self.assertIsNotNone(latest)
        self.assertEqual(latest["temperature"], 26.5)
        self.assertIsNotNone(latest_report)
        self.assertEqual(latest_report["deviceId"], "az3166-01")
        self.assertEqual(latest_report["temperature"], 26.5)
        self.assertEqual([record["temperature"] for record in history], [25.0, 26.5])
        self.assertEqual(recent_count, 1)
        self.assertEqual(status["recordCount"], 3)
        self.assertEqual(status["lastReceivedAt"], "2026-08-16T10:02:00Z")


if __name__ == "__main__":
    unittest.main()