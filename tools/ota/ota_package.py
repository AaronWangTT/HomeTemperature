from __future__ import annotations

import hashlib
import re
from pathlib import Path

from cryptography.exceptions import UnsupportedAlgorithm
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

from az3166_ota.keys import generate_key_pair as generate_generic_key_pair
from az3166_ota.package import (
    DESCRIPTOR_OFFSET,
    DESCRIPTOR_SIZE,
    PACKAGE_HEADER_SIZE,
    PAYLOAD_OFFSET,
    Descriptor,
    PackageError,
    VerifiedPackage,
    build_package as build_generic_package,
    load_private_key as load_generic_private_key,
    load_public_key,
    parse_descriptor,
    public_key_der,
    validate_raw_image,
    verify_package as verify_generic_package,
)
from production_key_ids import PRODUCTION_KEY_IDS

APPLICATION_ADDRESS = 0x0800C000
APPLICATION_CAPACITY = 0x000F4000
PRODUCT_ID = "HomeTemperature"
BOARD_ID = "MXCHIP_AZ3166"
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]

_VERSION_PATTERN = re.compile(
    r"(?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,4})\Z"
)
_SOURCE_PATTERN = re.compile(r"[0-9a-f]{40}\Z")


def _validate_version(version: str) -> None:
    if _VERSION_PATTERN.fullmatch(version) is None:
        raise PackageError("firmware version must be canonical MAJOR.MINOR.PATCH")
    if any(int(component) > 65535 for component in version.split(".")):
        raise PackageError("firmware version components must not exceed 65535")


def load_private_key(path: Path) -> ec.EllipticCurvePrivateKey:
    resolved = path.resolve(strict=True)
    try:
        resolved.relative_to(REPOSITORY_ROOT)
    except ValueError:
        pass
    else:
        raise PackageError("private key must be stored outside the repository")
    try:
        return load_generic_private_key(resolved)
    except PackageError:
        encoded = resolved.read_bytes()
        loaders = (
            serialization.load_pem_private_key,
            serialization.load_der_private_key,
        )
        for loader in loaders:
            try:
                key = loader(encoded, None)
            except (TypeError, ValueError, UnsupportedAlgorithm):
                continue
            if isinstance(key, ec.EllipticCurvePrivateKey) and isinstance(
                key.curve, ec.SECP256R1
            ):
                return key
        raise PackageError("private key must be an unencrypted P-256 key")


def generate_key_pair(
    private_path: Path,
    public_path: Path,
    *,
    overwrite: bool = False,
    create_parents: bool = False,
) -> tuple[bytes, str]:
    resolved_private = private_path.resolve(strict=False)
    try:
        resolved_private.relative_to(REPOSITORY_ROOT)
    except ValueError:
        return generate_generic_key_pair(
            private_path,
            public_path,
            overwrite=overwrite,
            create_parents=create_parents,
        )
    raise PackageError("private key must be stored outside the repository")


def require_production_key(public_der: bytes) -> None:
    key_id = hashlib.sha256(public_der).hexdigest()
    if key_id not in PRODUCTION_KEY_IDS:
        raise PackageError(
            f"signing key ID {key_id} is not in the reviewed production allowlist"
        )


def render_build_config(public_der: bytes, version: str, source: str) -> str:
    require_production_key(public_der)
    _validate_version(version)
    if _SOURCE_PATTERN.fullmatch(source) is None:
        raise PackageError("source commit must be 40 lowercase hexadecimal characters")
    key_id = hashlib.sha256(public_der).digest()

    def byte_list(data: bytes) -> str:
        return ", ".join(f"0x{value:02x}" for value in data)

    return (
        "#ifndef HOME_TEMPERATURE_OTA_BUILD_CONFIG_H\n"
        "#define HOME_TEMPERATURE_OTA_BUILD_CONFIG_H\n\n"
        f'#define HOME_TEMPERATURE_FIRMWARE_VERSION "{version}"\n'
        f"#define HOME_TEMPERATURE_OTA_SOURCE_COMMIT_BYTES {byte_list(source.encode('ascii'))}\n"
        f"#define HOME_TEMPERATURE_OTA_KEY_ID_BYTES {byte_list(key_id)}\n"
        f"#define HOME_TEMPERATURE_OTA_PUBLIC_KEY_DER_BYTES {byte_list(public_der)}\n\n"
        "#endif\n"
    )


def validate_image(
    image: bytes,
    *,
    key_id: bytes,
    expected_version: str | None = None,
    expected_source: str | None = None,
    expected_product: str = PRODUCT_ID,
    expected_board: str = BOARD_ID,
    application_address: int = APPLICATION_ADDRESS,
    application_capacity: int = APPLICATION_CAPACITY,
) -> Descriptor:
    return validate_raw_image(
        image,
        key_id=key_id,
        expected_version=expected_version,
        expected_source=expected_source,
        expected_product=expected_product,
        expected_board=expected_board,
        expected_address=application_address,
        expected_capacity=application_capacity,
    )


def build_package(
    image: bytes,
    private_key: ec.EllipticCurvePrivateKey,
    *,
    expected_version: str,
    expected_source: str,
    expected_product: str = PRODUCT_ID,
    expected_board: str = BOARD_ID,
    application_address: int = APPLICATION_ADDRESS,
    application_capacity: int = APPLICATION_CAPACITY,
) -> bytes:
    return build_generic_package(
        image,
        private_key,
        expected_version=expected_version,
        expected_source=expected_source,
        expected_product=expected_product,
        expected_board=expected_board,
        expected_address=application_address,
        expected_capacity=application_capacity,
    )


def verify_package(
    package: bytes,
    public_key: ec.EllipticCurvePublicKey,
    public_der: bytes,
    *,
    expected_version: str | None = None,
    expected_source: str | None = None,
    expected_product: str = PRODUCT_ID,
    expected_board: str = BOARD_ID,
    application_address: int = APPLICATION_ADDRESS,
    application_capacity: int = APPLICATION_CAPACITY,
) -> VerifiedPackage:
    return verify_generic_package(
        package,
        public_key,
        public_der,
        expected_version=expected_version,
        expected_source=expected_source,
        expected_product=expected_product,
        expected_board=expected_board,
        expected_address=application_address,
        expected_capacity=application_capacity,
    )


__all__ = [
    "APPLICATION_ADDRESS",
    "APPLICATION_CAPACITY",
    "BOARD_ID",
    "DESCRIPTOR_OFFSET",
    "DESCRIPTOR_SIZE",
    "PACKAGE_HEADER_SIZE",
    "PAYLOAD_OFFSET",
    "PRODUCT_ID",
    "Descriptor",
    "PackageError",
    "VerifiedPackage",
    "build_package",
    "generate_key_pair",
    "load_private_key",
    "load_public_key",
    "parse_descriptor",
    "public_key_der",
    "render_build_config",
    "require_production_key",
    "validate_image",
    "verify_package",
]
