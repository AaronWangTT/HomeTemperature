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
    version_status = 200
    redirected = False
    digest_override = None

    def log_message(self, *_args) -> None:
        pass

    def _json(self, status: int, payload: dict) -> None:
        encoded = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self) -> None:
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/api/version")
            self.end_headers()
        elif self.path == "/api/version":
            if self.headers.get("Authorization") is not None:
                self.__class__.redirected = True
            self._json(
                self.version_status,
                {"firmwareVersion": VERSION if self.applied else "1.0.0"},
            )
        elif self.path == "/api/ota/status":
            self._json(
                200,
                {
                    "state": "Ready",
                    "generation": 7,
                    "acceptedBytes": len(self.package),
                    "totalBytes": len(self.package),
                    "lastError": "OTA_OK",
                    "digest": self.digest_override
                    or hashlib.sha256(self.package[384:]).hexdigest(),
                },
            )
        else:
            self._json(401, {"error": "unauthorized"})

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.path == "/api/ota":
            self.__class__.package = body
            self._json(
                201,
                {
                    "state": "Ready",
                    "generation": 7,
                    "acceptedBytes": length,
                    "digest": self.digest_override
                    or hashlib.sha256(body[384:]).hexdigest(),
                },
            )
        elif (
            self.path == "/api/ota/apply"
            and self.headers.get("Content-Type") == "application/json"
            and json.loads(body)
            == {
                "generation": 7,
                "digest": hashlib.sha256(self.package[384:]).hexdigest(),
            }
        ):
            self.__class__.applied = True
            self._json(202, {"status": "apply queued"})
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
        OtaHandler.version_status = 200
        OtaHandler.redirected = False
        OtaHandler.digest_override = None
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

    def test_upload_status_apply_and_version_verification(self) -> None:
        common = (
            "--base-url",
            self.base_url,
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

    def test_validates_allowlisted_production_image(self) -> None:
        image = Path(self.directory.name) / "firmware.bin"
        image.write_bytes(self.package.read_bytes()[384:])
        with patch(
            "ota_package.PRODUCTION_KEY_IDS",
            frozenset({self.production_key_id}),
        ):
            self.assertEqual(
                self.run_cli(
                    "validate-image",
                    "--image",
                    str(image),
                    "--public-key",
                    str(self.public_key),
                    "--version",
                    VERSION,
                    "--source",
                    SOURCE,
                ),
                0,
            )

    def test_rejects_unallowlisted_upload_key(self) -> None:
        self.assertEqual(
            self.run_cli(
                "upload",
                "--base-url",
                self.base_url,
                "--package",
                str(self.package),
                "--public-key",
                str(self.public_key),
            ),
            1,
        )

    def test_rejects_device_staged_digest_mismatch(self) -> None:
        OtaHandler.digest_override = "0" * 64
        with patch(
            "ota_package.PRODUCTION_KEY_IDS",
            frozenset({self.production_key_id}),
        ):
            self.assertEqual(
                self.run_cli(
                    "upload",
                    "--base-url",
                    self.base_url,
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
                "status",
                "--base-url",
                f"{self.base_url}?token=secret",
            )

    def test_rejects_invalid_port(self) -> None:
        with self.assertRaises(SystemExit):
            self.run_cli(
                "status",
                "--base-url",
                "http://127.0.0.1:not-a-port",
            )

    def test_rejects_nonpositive_and_nonfinite_timeouts(self) -> None:
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value), self.assertRaises(SystemExit):
                self.run_cli(
                    "status",
                    "--base-url",
                    self.base_url,
                    "--timeout",
                    value,
                )

    def test_upload_defaults_to_flash_verification_timeout(self) -> None:
        args = ota_cli._parser().parse_args(
            [
                "upload",
                "--base-url",
                self.base_url,
                "--package",
                str(self.package),
                "--public-key",
                str(self.public_key),
            ]
        )
        self.assertEqual(args.timeout, 120.0)

    def test_requests_do_not_follow_redirects(self) -> None:
        with self.assertRaisesRegex(ota_cli.CliError, "HTTP 302"):
            ota_cli._request(
                self.base_url,
                "GET",
                "/redirect",
                timeout=1,
            )
        self.assertFalse(OtaHandler.redirected)

    def test_reboot_verification_rejects_error_status_with_expected_version(
        self,
    ) -> None:
        OtaHandler.applied = True
        OtaHandler.version_status = 500
        with self.assertRaisesRegex(ota_cli.CliError, "HTTP 500"):
            ota_cli._verify_rebooted_version(
                f"{self.base_url}/api/version",
                VERSION,
                "firmwareVersion",
                0.05,
                0.05,
            )

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
        self.assertLess(script.index(" validate-image `"), upload)
        self.assertIn(" build-config `", script)
        self.assertIn('"hla_serial $StLinkSerial"', script)
        self.assertNotIn("path must not contain whitespace", script)
        self.assertIn('Join-Path $env:PUBLIC "HomeTemperatureOtaBuild"', script)
        self.assertIn("Copy-Item -LiteralPath $sourceLinkerScript", script)
        self.assertIn("Copy-Item -LiteralPath $resolvedOtaBuildConfig", script)
        self.assertIn("compiler.link.script.flags=-T$linkerScript", script)
        self.assertIn("compiler.cpp.extra_flags=-include $stagedOtaBuildConfig", script)
        self.assertIn('$openOcdBinaryPath = $binaryPath.Replace("\\", "/")', script)
        self.assertIn(
            '"program {$openOcdBinaryPath} verify reset 0x800C000; shutdown"',
            script,
        )
        self.assertNotIn("Uploading validated $binaryPath to $Board on $Port", script)
        self.assertIn(
            '$Action -eq "Upload" -and -not $isProductionSketch',
            script,
        )
        self.assertIn(
            "Production Upload and Restore do not accept -OtaBuildConfig",
            script,
        )
        self.assertIn(
            "Production Upload requires -OtaPublicKey, -FirmwareVersion, and -SourceCommit.",
            script,
        )
        self.assertIn('$Action -eq "Upload" -or $Action -eq "Restore"', script)
        self.assertIn("trap {", script)

    def test_hardware_harness_uses_fail_closed_restore_action(self) -> None:
        harness = (
            Path(__file__).resolve().parents[3]
            / "firmware"
            / "tests"
            / "Az3166TestHarness.ps1"
        ).read_text(encoding="utf-8")
        self.assertIn('& $invokeBuild "Restore" $productionSketch', harness)
        self.assertIn(
            "$arguments.StLinkSerial = $env:HOME_TEMPERATURE_STLINK_SERIAL",
            harness,
        )
        preflight = harness.index(
            '$env:HOME_TEMPERATURE_STLINK_SERIAL -notmatch "^[0-9A-Fa-f]{24}$"'
        )
        upload = harness.index('& $invokeBuild "Upload" $testSketch')
        self.assertLess(preflight, upload)

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
