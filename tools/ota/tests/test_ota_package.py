from __future__ import annotations

import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ota_package import (  # noqa: E402
    APPLICATION_ADDRESS,
    APPLICATION_CAPACITY,
    BOARD_ID,
    DESCRIPTOR_OFFSET,
    DESCRIPTOR_SIZE,
    PACKAGE_HEADER_SIZE,
    PAYLOAD_OFFSET,
    PRODUCT_ID,
    PackageError,
    build_package,
    generate_key_pair,
    load_public_key,
    load_private_key,
    public_key_der,
    require_production_key,
    render_build_config,
    verify_package,
)
from production_key_ids import PRODUCTION_KEY_IDS  # noqa: E402

VERSION = "1.2.3"
SOURCE = "0123456789abcdef0123456789abcdef01234567"
PRODUCTION_PUBLIC_KEY = (
    Path(__file__).resolve().parents[1]
    / "keys"
    / "hometemperature-production.der"
)


def _field(value: str, size: int) -> bytes:
    encoded = value.encode("ascii")
    return encoded + bytes(size - len(encoded))


def make_image(key_der: bytes) -> bytes:
    image = bytearray(0x400)
    struct.pack_into("<II", image, 0, 0x20001000, APPLICATION_ADDRESS + 0x101)
    descriptor = bytearray(DESCRIPTOR_SIZE)
    descriptor[:8] = b"AZOTA001"
    struct.pack_into("<HHI", descriptor, 8, 1, DESCRIPTOR_SIZE, 1)
    descriptor[16:48] = _field(PRODUCT_ID, 32)
    descriptor[48:80] = _field(BOARD_ID, 32)
    descriptor[80:112] = _field(VERSION, 32)
    descriptor[112:152] = SOURCE.encode("ascii")
    struct.pack_into(
        "<III",
        descriptor,
        152,
        APPLICATION_ADDRESS,
        APPLICATION_CAPACITY,
        1,
    )
    descriptor[164:196] = hashlib.sha256(key_der).digest()
    image[DESCRIPTOR_OFFSET : DESCRIPTOR_OFFSET + DESCRIPTOR_SIZE] = descriptor
    return bytes(image)


class PackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.private_key = ec.generate_private_key(ec.SECP256R1())
        self.public_der = public_key_der(self.private_key)
        self.public_key = self.private_key.public_key()
        self.image = make_image(self.public_der)
        self.package = build_package(
            self.image,
            self.private_key,
            expected_version=VERSION,
            expected_source=SOURCE,
        )

    def verify(self, package: bytes | None = None):
        return verify_package(
            package or self.package,
            self.public_key,
            self.public_der,
            expected_version=VERSION,
            expected_source=SOURCE,
        )

    def test_builds_and_verifies_package(self) -> None:
        verified = self.verify()
        self.assertEqual(verified.payload_length, len(self.image))
        self.assertEqual(verified.payload_sha256, hashlib.sha256(self.image).digest())
        self.assertEqual(verified.descriptor.firmware_version, VERSION)

    def test_rejects_header_tampering(self) -> None:
        package = bytearray(self.package)
        package[20] ^= 1
        with self.assertRaisesRegex(PackageError, "SHA-256"):
            self.verify(bytes(package))

    def test_rejects_signature_tampering(self) -> None:
        package = bytearray(self.package)
        package[320] ^= 1
        with self.assertRaisesRegex(PackageError, "signature"):
            self.verify(bytes(package))

    def test_rejects_malformed_raw_signature_scalars(self) -> None:
        for signature in (bytes(64), bytes([0xFF]) * 64):
            with self.subTest(signature=signature[:1]):
                package = bytearray(self.package)
                package[PACKAGE_HEADER_SIZE:PAYLOAD_OFFSET] = signature
                with self.assertRaisesRegex(PackageError, "signature"):
                    self.verify(bytes(package))

    def test_rejects_payload_tampering(self) -> None:
        package = bytearray(self.package)
        package[-1] ^= 1
        with self.assertRaises(PackageError):
            self.verify(bytes(package))

    def test_rejects_wrong_key(self) -> None:
        wrong_key = ec.generate_private_key(ec.SECP256R1())
        wrong_der = public_key_der(wrong_key)
        with self.assertRaisesRegex(PackageError, "key"):
            verify_package(self.package, wrong_key.public_key(), wrong_der)

    def test_rejects_non_p256_signing_key(self) -> None:
        with self.assertRaisesRegex(PackageError, "private key must use P-256"):
            build_package(
                self.image,
                ec.generate_private_key(ec.SECP384R1()),
                expected_version=VERSION,
                expected_source=SOURCE,
            )

    def test_rejects_key_identifier_bound_to_different_verification_key(self) -> None:
        other_key = ec.generate_private_key(ec.SECP256R1())
        other_der = public_key_der(other_key)
        package = bytearray(self.package)
        other_key_id = hashlib.sha256(other_der).digest()
        package[64 + 164 : 64 + 196] = other_key_id
        package[PAYLOAD_OFFSET + DESCRIPTOR_OFFSET + 164 :
                PAYLOAD_OFFSET + DESCRIPTOR_OFFSET + 196] = other_key_id
        package[20:52] = hashlib.sha256(package[PAYLOAD_OFFSET:]).digest()
        der_signature = self.private_key.sign(
            bytes(package[:PACKAGE_HEADER_SIZE]),
            ec.ECDSA(hashes.SHA256()),
        )
        r, s = decode_dss_signature(der_signature)
        package[PACKAGE_HEADER_SIZE:PAYLOAD_OFFSET] = (
            r.to_bytes(32, "big") + s.to_bytes(32, "big")
        )

        with self.assertRaisesRegex(PackageError, "verification key"):
            verify_package(
                bytes(package),
                self.public_key,
                other_der,
                expected_version=VERSION,
                expected_source=SOURCE,
            )

    def test_rejects_malformed_image_vector_table(self) -> None:
        image = bytearray(self.image)
        struct.pack_into("<I", image, 0, 0x20001004)
        with self.assertRaisesRegex(PackageError, "stack pointer"):
            build_package(
                bytes(image),
                self.private_key,
                expected_version=VERSION,
                expected_source=SOURCE,
            )

    def test_rejects_malformed_descriptor(self) -> None:
        image = bytearray(self.image)
        image[DESCRIPTOR_OFFSET + 196] = 1
        with self.assertRaisesRegex(PackageError, "reserved"):
            build_package(
                bytes(image),
                self.private_key,
                expected_version=VERSION,
                expected_source=SOURCE,
            )

    def test_rejects_non_spki_public_material(self) -> None:
        encoded_point = self.public_key.public_bytes(
            serialization.Encoding.X962,
            serialization.PublicFormat.UncompressedPoint,
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "point.bin"
            path.write_bytes(encoded_point)
            with self.assertRaisesRegex(PackageError, "SubjectPublicKeyInfo"):
                load_public_key(path)

    def test_rejects_private_key_inside_repository(self) -> None:
        with self.assertRaisesRegex(PackageError, "outside the repository"):
            load_private_key(Path(__file__))
        with self.assertRaisesRegex(PackageError, "outside the repository"):
            generate_key_pair(
                Path(__file__).parent / "private.pem",
                Path(__file__).parent / "public.der",
            )

    def test_loads_legacy_sec1_private_keys_outside_repository(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            for encoding, suffix in (
                (serialization.Encoding.PEM, "pem"),
                (serialization.Encoding.DER, "der"),
            ):
                with self.subTest(encoding=encoding):
                    encoded = self.private_key.private_bytes(
                        encoding,
                        serialization.PrivateFormat.TraditionalOpenSSL,
                        serialization.NoEncryption(),
                    )
                    path = Path(directory) / f"legacy-private.{suffix}"
                    path.write_bytes(encoded)
                    self.assertEqual(
                        load_private_key(path).private_numbers(),
                        self.private_key.private_numbers(),
                    )

    def test_renders_public_firmware_build_config(self) -> None:
        key_id = hashlib.sha256(self.public_der).hexdigest()
        with patch("ota_package.PRODUCTION_KEY_IDS", frozenset({key_id})):
            rendered = render_build_config(self.public_der, VERSION, SOURCE)
        self.assertIn(
            f'#define HOME_TEMPERATURE_FIRMWARE_VERSION "{VERSION}"', rendered
        )
        self.assertIn(
            "#define HOME_TEMPERATURE_OTA_KEY_ID_BYTES "
            + ", ".join(
                f"0x{value:02x}"
                for value in hashlib.sha256(self.public_der).digest()
            ),
            rendered,
        )
        self.assertNotIn("PRIVATE", rendered)

    def test_rejects_unallowlisted_production_key(self) -> None:
        with self.assertRaisesRegex(PackageError, "production allowlist"):
            require_production_key(self.public_der)

    def test_accepts_reviewed_production_key(self) -> None:
        key_id = hashlib.sha256(self.public_der).hexdigest()
        with patch("ota_package.PRODUCTION_KEY_IDS", frozenset({key_id})):
            require_production_key(self.public_der)

    def test_reviewed_production_key_matches_allowlist(self) -> None:
        public_key, public_der = load_public_key(PRODUCTION_PUBLIC_KEY)
        key_id = hashlib.sha256(public_der).hexdigest()
        self.assertIsInstance(public_key.curve, ec.SECP256R1)
        self.assertEqual(
            PRODUCTION_KEY_IDS,
            frozenset(
                {"dd000f358407dbdeaaedc31e49706cd10aaf18984b223dc11443e07513dc136f"}
            ),
        )
        self.assertIn(key_id, PRODUCTION_KEY_IDS)
        require_production_key(public_der)

if __name__ == "__main__":
    unittest.main()
