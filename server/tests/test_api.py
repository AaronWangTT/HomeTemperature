import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from fastapi.testclient import TestClient

from app.database import insert_telemetry
from app.main import create_app
from app.settings import Settings


class ApiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.database_path = str(Path(self.temporary_directory.name) / "telemetry.db")
        settings = Settings(
            database_path=self.database_path,
            device_api_key="device-test-key",
            dashboard_username="viewer",
            dashboard_password="dashboard-test-password",
        )
        self.client_context = TestClient(create_app(settings))
        self.client = self.client_context.__enter__()
        self.dashboard_auth = ("viewer", "dashboard-test-password")

    def tearDown(self) -> None:
        self.client_context.__exit__(None, None, None)
        self.temporary_directory.cleanup()

    def test_ingest_latest_and_history(self) -> None:
        payload = {
            "deviceId": "az3166-01",
            "temperature": 26.5,
            "humidity": 58.3,
            "pressure": 1008.4,
        }

        unauthorized = self.client.post("/api/v1/telemetry", json=payload)
        accepted = self.client.post(
            "/api/v1/telemetry",
            json=payload,
            headers={"X-Device-Key": "device-test-key"},
        )
        latest = self.client.get("/api/v1/telemetry/latest", auth=self.dashboard_auth)
        history = self.client.get("/api/v1/telemetry/history", auth=self.dashboard_auth)

        self.assertEqual(unauthorized.status_code, 401)
        self.assertEqual(accepted.status_code, 201)
        self.assertEqual(accepted.json()["deviceId"], "az3166-01")
        self.assertEqual(latest.status_code, 200)
        self.assertEqual(latest.json()["temperature"], 26.5)
        self.assertEqual(history.status_code, 200)
        self.assertEqual(len(history.json()), 1)

    def test_defaults_to_last_reporting_device(self) -> None:
        now = datetime.now(timezone.utc)
        received_at = [
            (now - timedelta(minutes=2)).isoformat(timespec="seconds").replace("+00:00", "Z"),
            (now - timedelta(minutes=1)).isoformat(timespec="seconds").replace("+00:00", "Z"),
            now.isoformat(timespec="seconds").replace("+00:00", "Z"),
        ]
        insert_telemetry(
            self.database_path,
            device_id="az3166-01",
            temperature=25.0,
            humidity=50.0,
            pressure=1008.0,
            received_at=received_at[0],
        )
        insert_telemetry(
            self.database_path,
            device_id="az3166-UID",
            temperature=26.0,
            humidity=51.0,
            pressure=1007.0,
            received_at=received_at[1],
        )
        insert_telemetry(
            self.database_path,
            device_id="az3166-UID",
            temperature=27.0,
            humidity=52.0,
            pressure=1006.0,
            received_at=received_at[2],
        )

        latest = self.client.get("/api/v1/telemetry/latest", auth=self.dashboard_auth)
        history = self.client.get("/api/v1/telemetry/history", auth=self.dashboard_auth)
        count = self.client.get(
            "/api/v1/telemetry/count?hours=168",
            auth=self.dashboard_auth,
        )
        status_response = self.client.get("/api/v1/status", auth=self.dashboard_auth)
        explicit = self.client.get(
            "/api/v1/telemetry/latest?deviceId=az3166-01",
            auth=self.dashboard_auth,
        )

        self.assertEqual(latest.status_code, 200)
        self.assertEqual(latest.json()["deviceId"], "az3166-UID")
        self.assertEqual(latest.json()["temperature"], 27.0)
        self.assertEqual(history.status_code, 200)
        self.assertEqual([record["deviceId"] for record in history.json()], ["az3166-UID"] * 2)
        self.assertEqual(count.status_code, 200)
        self.assertEqual(
            count.json(),
            {"deviceId": "az3166-UID", "recordCount": 2},
        )
        self.assertEqual(status_response.status_code, 200)
        self.assertEqual(status_response.json()["activeDeviceId"], "az3166-UID")
        self.assertNotIn("defaultDeviceId", status_response.json())
        self.assertEqual(explicit.status_code, 200)
        self.assertEqual(explicit.json()["deviceId"], "az3166-01")

    def test_rejects_invalid_values_and_protects_reads(self) -> None:
        invalid = self.client.post(
            "/api/v1/telemetry",
            json={
                "deviceId": "az3166-01",
                "temperature": 26.5,
                "humidity": 110,
                "pressure": 1008.4,
            },
            headers={"X-Device-Key": "device-test-key"},
        )
        protected = self.client.get("/api/v1/status")

        self.assertEqual(invalid.status_code, 422)
        self.assertEqual(protected.status_code, 401)
        self.assertIn("Basic", protected.headers["www-authenticate"])

    def test_health_check_is_public(self) -> None:
        response = self.client.get("/healthz")

        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json(), {"status": "ok"})

    def test_dashboard_requires_authentication_and_serves_ui(self) -> None:
        unauthorized = self.client.get("/dashboard")
        authorized = self.client.get("/dashboard", auth=self.dashboard_auth)

        self.assertEqual(unauthorized.status_code, 401)
        self.assertEqual(authorized.status_code, 200)
        self.assertIn("text/html", authorized.headers["content-type"])
        self.assertIn("Environment telemetry", authorized.text)
        self.assertNotIn('id="device"', authorized.text)
        self.assertNotIn("/api/v1/devices", authorized.text)
        self.assertIn("/api/v1/telemetry/latest", authorized.text)
        self.assertIn("/api/v1/telemetry/count", authorized.text)


if __name__ == "__main__":
    unittest.main()