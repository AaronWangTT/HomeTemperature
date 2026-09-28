from __future__ import annotations

import hashlib
import re
import struct
from dataclasses import dataclass
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)

from production_key_ids import PRODUCTION_KEY_IDS

PACKAGE_MAGIC = b"AZPKG001"
DESCRIPTOR_MAGIC = b"AZOTA001"
PACKAGE_PREFIX_SIZE = 64
DESCRIPTOR_OFFSET = 0x200
DESCRIPTOR_SIZE = 256
PACKAGE_HEADER_SIZE = PACKAGE_PREFIX_SIZE + DESCRIPTOR_SIZE
SIGNATURE_SIZE = 64
PAYLOAD_OFFSET = PACKAGE_HEADER_SIZE + SIGNATURE_SIZE
PACKAGE_FORMAT_VERSION = 1
SIGNATURE_ALGORITHM = 1
SECURITY_PROFILE = 1
APPLICATION_ADDRESS = 0x0800C000
APPLICATION_CAPACITY = 0x000F4000
RAM_START_EXCLUSIVE = 0x200001C4
RAM_END_INCLUSIVE = 0x20040000
PRODUCT_ID = "HomeTemperature"
BOARD_ID = "MXCHIP_AZ3166"
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]

_VERSION_PATTERN = re.compile(
    r"(?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,4})\Z"
)
_SOURCE_PATTERN = re.compile(r"[0-9a-f]{40}\Z")


class PackageError(ValueError):
    pass


@dataclass(frozen=True)
class Descriptor:
    product_id: str
    board_id: str
    firmware_version: str
    source_commit: str
    application_address: int
    application_capacity: int
    key_id: bytes


@dataclass(frozen=True)
class VerifiedPackage:
    descriptor: Descriptor
    payload_length: int
    payload_sha256: bytes


def _canonical_string(field: bytes, name: str) -> str:
    try:
        terminator = field.index(0)
    except ValueError as error:
        raise PackageError(f"{name} is not NUL-terminated") from error
    if terminator == 0 or any(field[terminator + 1 :]):
        raise PackageError(f"{name} is not canonically padded")
    try:
        return field[:terminator].decode("ascii")
    except UnicodeDecodeError as error:
        raise PackageError(f"{name} is not ASCII") from error


def _parse_version(version: str) -> tuple[int, int, int]:
    if _VERSION_PATTERN.fullmatch(version) is None:
        raise PackageError("firmware version must be canonical MAJOR.MINOR.PATCH")
    components = tuple(int(component) for component in version.split("."))
    if any(component > 65535 for component in components):
        raise PackageError("firmware version components must not exceed 65535")
    return components


def parse_descriptor(data: bytes) -> Descriptor:
    if len(data) != DESCRIPTOR_SIZE:
        raise PackageError("descriptor must be exactly 256 bytes")
    if data[:8] != DESCRIPTOR_MAGIC:
        raise PackageError("invalid descriptor magic")
    descriptor_version, descriptor_size = struct.unpack_from("<HH", data, 8)
    security_profile = struct.unpack_from("<I", data, 12)[0]
    package_version = struct.unpack_from("<I", data, 160)[0]
    if (
        descriptor_version != 1
        or descriptor_size != DESCRIPTOR_SIZE
        or security_profile != SECURITY_PROFILE
        or package_version != PACKAGE_FORMAT_VERSION
    ):
        raise PackageError("unsupported descriptor format")
    if any(data[196:]):
        raise PackageError("descriptor reserved bytes must be zero")

    product_id = _canonical_string(data[16:48], "product ID")
    board_id = _canonical_string(data[48:80], "board ID")
    firmware_version = _canonical_string(data[80:112], "firmware version")
    _parse_version(firmware_version)
    try:
        source_commit = data[112:152].decode("ascii")
    except UnicodeDecodeError as error:
        raise PackageError("source commit is not ASCII") from error
    if _SOURCE_PATTERN.fullmatch(source_commit) is None:
        raise PackageError("source commit must be 40 lowercase hexadecimal characters")

    application_address, application_capacity = struct.unpack_from("<II", data, 152)
    return Descriptor(
        product_id=product_id,
        board_id=board_id,
        firmware_version=firmware_version,
        source_commit=source_commit,
        application_address=application_address,
        application_capacity=application_capacity,
        key_id=data[164:196],
    )


def load_public_key(path: Path) -> tuple[ec.EllipticCurvePublicKey, bytes]:
    der = path.read_bytes()
    try:
        key = serialization.load_der_public_key(der)
    except (TypeError, ValueError) as error:
        raise PackageError("public key must be DER SubjectPublicKeyInfo") from error
    if not isinstance(key, ec.EllipticCurvePublicKey) or not isinstance(
        key.curve, ec.SECP256R1
    ):
        raise PackageError("public key must use P-256")
    canonical_der = key.public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    if canonical_der != der:
        raise PackageError("public key must be canonical DER SubjectPublicKeyInfo")
    return key, der


def load_private_key(path: Path) -> ec.EllipticCurvePrivateKey:
    resolved = path.resolve(strict=True)
    try:
        resolved.relative_to(REPOSITORY_ROOT)
    except ValueError:
        pass
    else:
        raise PackageError("private key must be stored outside the repository")
    encoded = resolved.read_bytes()
    loaders = (
        serialization.load_pem_private_key,
        serialization.load_der_private_key,
    )
    key = None
    for loader in loaders:
        try:
            key = loader(encoded, password=None)
            break
        except (TypeError, ValueError):
            continue
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(
        key.curve, ec.SECP256R1
    ):
        raise PackageError("private key must be an unencrypted P-256 key")
    return key


def public_key_der(private_key: ec.EllipticCurvePrivateKey) -> bytes:
    return private_key.public_key().public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )


def require_production_key(public_der: bytes) -> None:
    key_id = hashlib.sha256(public_der).hexdigest()
    if key_id not in PRODUCTION_KEY_IDS:
        raise PackageError(
            f"signing key ID {key_id} is not in the reviewed production allowlist"
        )


def render_build_config(public_der: bytes, version: str, source: str) -> str:
    require_production_key(public_der)
    _parse_version(version)
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


def _validate_descriptor(
    descriptor: Descriptor,
    *,
    key_id: bytes,
    expected_version: str | None,
    expected_source: str | None,
    expected_product: str,
    expected_board: str,
    application_address: int,
    application_capacity: int,
) -> None:
    if descriptor.product_id != expected_product:
        raise PackageError("descriptor product ID does not match")
    if descriptor.board_id != expected_board:
        raise PackageError("descriptor board ID does not match")
    if expected_version is not None:
        _parse_version(expected_version)
        if descriptor.firmware_version != expected_version:
            raise PackageError("descriptor firmware version does not match")
    if expected_source is not None:
        if _SOURCE_PATTERN.fullmatch(expected_source) is None:
            raise PackageError("expected source must be 40 lowercase hexadecimal characters")
        if descriptor.source_commit != expected_source:
            raise PackageError("descriptor source commit does not match")
    if descriptor.application_address != application_address:
        raise PackageError("descriptor application address does not match")
    if descriptor.application_capacity != application_capacity:
        raise PackageError("descriptor application capacity does not match")
    if descriptor.key_id != key_id:
        raise PackageError("descriptor signing-key identifier does not match")


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
    if len(image) < DESCRIPTOR_OFFSET + DESCRIPTOR_SIZE:
        raise PackageError("image is too short to contain the descriptor")
    if len(image) > application_capacity:
        raise PackageError("image exceeds application capacity")
    if application_address & 0x1FF:
        raise PackageError("application address must be aligned to 512 bytes")

    stack_pointer, reset_vector = struct.unpack_from("<II", image, 0)
    if (
        stack_pointer & 7
        or stack_pointer <= RAM_START_EXCLUSIVE
        or stack_pointer > RAM_END_INCLUSIVE
    ):
        raise PackageError("invalid initial stack pointer")
    if reset_vector & 1 == 0:
        raise PackageError("reset vector is not a Thumb address")
    reset_handler = reset_vector & ~1
    image_end = application_address + len(image)
    if reset_handler < application_address or reset_handler + 2 > image_end:
        raise PackageError("reset handler is outside the image")

    descriptor = parse_descriptor(
        image[DESCRIPTOR_OFFSET : DESCRIPTOR_OFFSET + DESCRIPTOR_SIZE]
    )
    _validate_descriptor(
        descriptor,
        key_id=key_id,
        expected_version=expected_version,
        expected_source=expected_source,
        expected_product=expected_product,
        expected_board=expected_board,
        application_address=application_address,
        application_capacity=application_capacity,
    )
    return descriptor


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
    public_der = public_key_der(private_key)
    key_id = hashlib.sha256(public_der).digest()
    descriptor = validate_image(
        image,
        key_id=key_id,
        expected_version=expected_version,
        expected_source=expected_source,
        expected_product=expected_product,
        expected_board=expected_board,
        application_address=application_address,
        application_capacity=application_capacity,
    )
    descriptor_bytes = image[
        DESCRIPTOR_OFFSET : DESCRIPTOR_OFFSET + DESCRIPTOR_SIZE
    ]
    prefix = struct.pack(
        "<8sHHHHI32s12s",
        PACKAGE_MAGIC,
        PACKAGE_FORMAT_VERSION,
        PACKAGE_HEADER_SIZE,
        SIGNATURE_ALGORITHM,
        SIGNATURE_SIZE,
        len(image),
        hashlib.sha256(image).digest(),
        bytes(12),
    )
    header = prefix + descriptor_bytes
    der_signature = private_key.sign(header, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der_signature)
    signature = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    if descriptor.key_id != key_id:
        raise PackageError("descriptor key identifier changed during build")
    return header + signature + image


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
    if len(package) < PAYLOAD_OFFSET + 1:
        raise PackageError("package is too short")
    prefix = package[:PACKAGE_PREFIX_SIZE]
    (
        magic,
        package_version,
        header_size,
        signature_algorithm,
        signature_size,
        payload_length,
        payload_digest,
        reserved,
    ) = struct.unpack("<8sHHHHI32s12s", prefix)
    if magic != PACKAGE_MAGIC:
        raise PackageError("invalid package magic")
    if (
        package_version != PACKAGE_FORMAT_VERSION
        or header_size != PACKAGE_HEADER_SIZE
        or signature_algorithm != SIGNATURE_ALGORITHM
        or signature_size != SIGNATURE_SIZE
    ):
        raise PackageError("unsupported package format")
    if any(reserved):
        raise PackageError("package reserved bytes must be zero")
    if payload_length + PAYLOAD_OFFSET != len(package):
        raise PackageError("package length does not match payload declaration")

    descriptor_bytes = package[PACKAGE_PREFIX_SIZE:PACKAGE_HEADER_SIZE]
    payload = package[PAYLOAD_OFFSET:]
    key_id = hashlib.sha256(public_der).digest()
    embedded = payload[
        DESCRIPTOR_OFFSET : DESCRIPTOR_OFFSET + DESCRIPTOR_SIZE
    ]
    if len(embedded) != DESCRIPTOR_SIZE or embedded != descriptor_bytes:
        raise PackageError("embedded descriptor does not match package header")
    descriptor = validate_image(
        payload,
        key_id=key_id,
        expected_version=expected_version,
        expected_source=expected_source,
        expected_product=expected_product,
        expected_board=expected_board,
        application_address=application_address,
        application_capacity=application_capacity,
    )
    if hashlib.sha256(payload).digest() != payload_digest:
        raise PackageError("payload SHA-256 does not match")

    raw_signature = package[PACKAGE_HEADER_SIZE:PAYLOAD_OFFSET]
    r = int.from_bytes(raw_signature[:32], "big")
    s = int.from_bytes(raw_signature[32:], "big")
    try:
        public_key.verify(
            encode_dss_signature(r, s),
            package[:PACKAGE_HEADER_SIZE],
            ec.ECDSA(hashes.SHA256()),
        )
    except InvalidSignature as error:
        raise PackageError("package signature is invalid") from error
    return VerifiedPackage(descriptor, payload_length, payload_digest)
