from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import ota_cli  # noqa: E402
from ota_package import (  # noqa: E402
    APPLICATION_ADDRESS,
    APPLICATION_CAPACITY,
    BOARD_ID,
    DESCRIPTOR_OFFSET,
    DESCRIPTOR_SIZE,
    PRODUCT_ID,
    build_package,
    public_key_der,
)

VERSION = "2.0.0"
SOURCE = "abcdef0123456789abcdef0123456789abcdef01"
CAPABILITY = "0123456789abcdef0123456789abcdef"


def make_package() -> tuple[bytes, bytes]:
    private_key = ec.generate_private_key(ec.SECP256R1())
    public_der = public_key_der(private_key)
    descriptor = bytearray(DESCRIPTOR_SIZE)
    descriptor[:8] = b"AZOTA001"
    struct.pack_into("<HHI", descriptor, 8, 1, DESCRIPTOR_SIZE, 1)
    for offset, value in ((16, PRODUCT_ID), (48, BOARD_ID), (80, VERSION)):
        encoded = value.encode("ascii")
        descriptor[offset : offset + len(encoded)] = encoded
    descriptor[112:152] = SOURCE.encode("ascii")
    struct.pack_into(
        "<III",
        descriptor,
        152,
        APPLICATION_ADDRESS,
        APPLICATION_CAPACITY,
        1,
    )
    descriptor[164:196] = hashlib.sha256(public_der).digest()
    image = bytearray(0x400)
    struct.pack_into("<II", image, 0, 0x20001000, APPLICATION_ADDRESS + 0x101)
    image[DESCRIPTOR_OFFSET : DESCRIPTOR_OFFSET + DESCRIPTOR_SIZE] = descriptor
    return (
        build_package(
            bytes(image),
            private_key,
            expected_version=VERSION,
            expected_source=SOURCE,
        ),
        public_der,
    )


class OtaHandler(BaseHTTPRequestHandler):
    package = b""
    applied = False
    redirected = False

    def log_message(self, *_args) -> None:
        pass

    def _json(self, status: int, payload: dict) -> None:
        encoded = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def _authorized(self) -> bool:
        return self.headers.get("Authorization") == f"OTA {CAPABILITY}"

    def do_GET(self) -> None:
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/api/version")
            self.end_headers()
        elif self.path == "/api/version":
            if self.headers.get("Authorization") is not None:
                self.__class__.redirected = True
            self._json(200, {"firmwareVersion": VERSION if self.applied else "1.0.0"})
        elif self.path == "/api/ota/status" and self._authorized():
            self._json(
                200,
                {
                    "state": "Ready",
                    "generation": 7,
                    "acceptedBytes": len(self.package),
                    "totalBytes": len(self.package),
                    "lastError": "OTA_OK",
                },
            )
        else:
            self._json(401, {"error": "unauthorized"})

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.path == "/api/ota/session":
            if json.loads(body) == {"challenge": "1234abcd"}:
                self._json(201, {"capability": CAPABILITY})
            else:
                self._json(401, {"error": "unauthorized"})
        elif self.path == "/api/ota" and self._authorized():
            self.__class__.package = body
            self._json(201, {"state": "Ready", "generation": 7, "acceptedBytes": length})
        elif (
            self.path == "/api/ota/apply"
            and self._authorized()
            and "Content-Length" not in self.headers
            and not body
        ):
            self.__class__.applied = True
            self._json(202, {"status": "reboot scheduled"})
        else:
            self._json(401, {"error": "unauthorized"})


class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        package, public_der = make_package()
        self.package = root / "firmware.azpkg"
        self.public_key = root / "public.der"
        self.package.write_bytes(package)
        self.public_key.write_bytes(public_der)
        self.production_key_id = hashlib.sha256(public_der).hexdigest()
        OtaHandler.package = b""
        OtaHandler.applied = False
        OtaHandler.redirected = False
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), OtaHandler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def run_cli(self, *arguments: str) -> int:
        with patch.object(sys, "argv", ["ota_cli.py", *arguments]):
            return ota_cli.main()

    def test_claim_upload_status_apply_and_version_verification(self) -> None:
        self.assertEqual(
            self.run_cli(
                "claim",
                "--base-url",
                self.base_url,
                "--challenge",
                "1234abcd",
            ),
            0,
        )
        common = (
            "--base-url",
            self.base_url,
            "--capability",
            CAPABILITY,
        )
        self.assertEqual(self.run_cli("status", *common), 0)
        with patch(
            "ota_package.PRODUCTION_KEY_IDS",
            frozenset({self.production_key_id}),
        ):
            self.assertEqual(
                self.run_cli(
                    "upload",
                    *common,
                    "--package",
                    str(self.package),
                    "--public-key",
                    str(self.public_key),
                ),
                0,
            )
        digest = hashlib.sha256(self.package.read_bytes()[384:]).hexdigest()
        with patch(
            "ota_package.PRODUCTION_KEY_IDS",
            frozenset({self.production_key_id}),
        ):
            self.assertEqual(
                self.run_cli(
                    "apply",
                    *common,
                    "--package",
                    str(self.package),
                    "--public-key",
                    str(self.public_key),
                    "--generation",
                    "7",
                    "--digest",
                    digest,
                    "--reboot-timeout",
                    "2",
                ),
                0,
            )
        self.assertEqual(OtaHandler.package, self.package.read_bytes())
        self.assertTrue(OtaHandler.applied)

    def test_generates_public_build_configuration(self) -> None:
        output = Path(self.directory.name) / "ota-build-config.h"
        with patch(
            "ota_package.PRODUCTION_KEY_IDS",
            frozenset({self.production_key_id}),
        ):
            self.assertEqual(
                self.run_cli(
                    "build-config",
                    "--public-key",
                    str(self.public_key),
                    "--output",
                    str(output),
                    "--version",
                    VERSION,
                    "--source",
                    SOURCE,
                ),
                0,
            )
        rendered = output.read_text(encoding="ascii")
        self.assertIn(
            f'#define HOME_TEMPERATURE_FIRMWARE_VERSION "{VERSION}"',
            rendered,
        )
        self.assertIn("HOME_TEMPERATURE_OTA_PUBLIC_KEY_DER_BYTES", rendered)
        self.assertNotIn("PRIVATE", rendered)

    def test_rejects_wrong_authorization(self) -> None:
        self.assertEqual(
            self.run_cli(
                "status",
                "--base-url",
                self.base_url,
                "--capability",
                "f" * 32,
            ),
            1,
        )

    def test_rejects_unallowlisted_upload_key(self) -> None:
        self.assertEqual(
            self.run_cli(
                "upload",
                "--base-url",
                self.base_url,
                "--capability",
                CAPABILITY,
                "--package",
                str(self.package),
                "--public-key",
                str(self.public_key),
            ),
            1,
        )

    def test_rejects_non_origin_base_url(self) -> None:
        with self.assertRaises(SystemExit):
            self.run_cli(
                "claim",
                "--base-url",
                f"{self.base_url}?token=secret",
                "--challenge",
                "1234abcd",
            )

    def test_rejects_invalid_port(self) -> None:
        with self.assertRaises(SystemExit):
            self.run_cli(
                "claim",
                "--base-url",
                "http://127.0.0.1:not-a-port",
                "--challenge",
                "1234abcd",
            )

    def test_rejects_nonpositive_and_nonfinite_timeouts(self) -> None:
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value), self.assertRaises(SystemExit):
                self.run_cli(
                    "claim",
                    "--base-url",
                    self.base_url,
                    "--challenge",
                    "1234abcd",
                    "--timeout",
                    value,
                )

    def test_authorized_requests_do_not_follow_redirects(self) -> None:
        with self.assertRaisesRegex(ota_cli.CliError, "HTTP 302"):
            ota_cli._request(
                self.base_url,
                "GET",
                "/redirect",
                capability=CAPABILITY,
                timeout=1,
            )
        self.assertFalse(OtaHandler.redirected)

    def test_production_upload_validates_binary_before_openocd(self) -> None:
        script = (
            Path(__file__).resolve().parents[3]
            / "firmware"
            / "tools"
            / "Invoke-Az3166Build.ps1"
        ).read_text(encoding="utf-8")
        validation = script.index(
            "Production binary has no valid OTA descriptor at offset 0x200."
        )
        upload = script.index("Uploading validated $binaryPath")
        self.assertLess(validation, upload)
        self.assertIn(
            '$Action -eq "Upload" -and -not $isProductionSketch',
            script,
        )

    def test_rejects_noncanonical_apply_digest(self) -> None:
        digest = hashlib.sha256(self.package.read_bytes()[384:]).hexdigest().upper()
        with patch(
            "ota_package.PRODUCTION_KEY_IDS",
            frozenset({self.production_key_id}),
        ):
            self.assertEqual(
                self.run_cli(
                    "apply",
                    "--base-url",
                    self.base_url,
                    "--capability",
                    CAPABILITY,
                    "--package",
                    str(self.package),
                    "--public-key",
                    str(self.public_key),
                    "--generation",
                    "7",
                    "--digest",
                    digest,
                    "--reboot-timeout",
                    "1",
                ),
                1,
            )


if __name__ == "__main__":
    unittest.main()
