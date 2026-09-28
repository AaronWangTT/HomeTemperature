from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import math
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from ota_package import (
    APPLICATION_ADDRESS,
    APPLICATION_CAPACITY,
    BOARD_ID,
    PRODUCT_ID,
    PackageError,
    VerifiedPackage,
    build_package,
    load_private_key,
    load_public_key,
    public_key_der,
    require_production_key,
    render_build_config,
    validate_image,
    verify_package,
)

PRIVATE_KEY_ENV = "HOME_TEMPERATURE_OTA_PRIVATE_KEY"
CAPABILITY_ENV = "HOME_TEMPERATURE_OTA_CAPABILITY"


class CliError(RuntimeError):
    pass


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_NO_REDIRECT_OPENER = urllib.request.build_opener(_NoRedirectHandler)


def _integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer or 0x-prefixed integer") from error


def _positive_integer(value: str) -> int:
    parsed = _integer(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def _positive_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a number") from error
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return parsed


def _base_url(value: str) -> str:
    parsed = urllib.parse.urlsplit(value)
    try:
        port = parsed.port
    except ValueError as error:
        raise argparse.ArgumentTypeError("base URL contains an invalid port") from error
    if (
        parsed.scheme != "http"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
        or port == 0
    ):
        raise argparse.ArgumentTypeError("base URL must be an HTTP origin without a path")
    return value.rstrip("/")


def _capability(value: str | None) -> str:
    capability = value or os.environ.get(CAPABILITY_ENV)
    if (
        capability is None
        or len(capability) != 32
        or any(character not in "0123456789abcdef" for character in capability)
    ):
        raise CliError(
            f"capability must be 32 lowercase hexadecimal characters; "
            f"use --capability or {CAPABILITY_ENV}"
        )
    return capability


def _request(
    base_url: str,
    method: str,
    path: str,
    *,
    capability: str | None = None,
    body: bytes | None = None,
    content_type: str | None = None,
    timeout: float,
) -> tuple[int, dict[str, Any]]:
    headers = {"Accept": "application/json"}
    if capability is not None:
        headers["Authorization"] = f"OTA {capability}"
    if content_type is not None:
        headers["Content-Type"] = content_type
    request = urllib.request.Request(
        f"{base_url}{path}",
        data=body,
        headers=headers,
        method=method,
    )
    try:
        with _NO_REDIRECT_OPENER.open(request, timeout=timeout) as response:
            encoded = response.read()
            status = response.status
    except urllib.error.HTTPError as error:
        encoded = error.read()
        status = error.code
    except urllib.error.URLError as error:
        raise CliError(f"{method} {path} failed: {error.reason}") from error
    if 300 <= status < 400:
        raise CliError(f"{method} {path} returned HTTP {status}; redirects are disabled")
    try:
        payload = json.loads(encoded.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CliError(f"{method} {path} returned invalid JSON") from error
    if not isinstance(payload, dict):
        raise CliError(f"{method} {path} returned a non-object JSON response")
    if status < 200 or status >= 300:
        message = payload.get("error")
        raise CliError(f"{method} {path} returned HTTP {status}: {message or 'request failed'}")
    return status, payload


def _bodyless_request(
    base_url: str,
    method: str,
    path: str,
    capability: str,
    timeout: float,
) -> dict[str, Any]:
    parsed = urllib.parse.urlsplit(base_url)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=timeout)
    try:
        connection.putrequest(method, path)
        connection.putheader("Accept", "application/json")
        connection.putheader("Authorization", f"OTA {capability}")
        connection.endheaders()
        response = connection.getresponse()
        encoded = response.read()
        status = response.status
    except OSError as error:
        raise CliError(f"{method} {path} failed: {error}") from error
    finally:
        connection.close()
    try:
        payload = json.loads(encoded.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CliError(f"{method} {path} returned invalid JSON") from error
    if not isinstance(payload, dict):
        raise CliError(f"{method} {path} returned a non-object JSON response")
    if status < 200 or status >= 300:
        raise CliError(
            f"{method} {path} returned HTTP {status}: "
            f"{payload.get('error') or 'request failed'}"
        )
    return payload


def _upload_request(
    base_url: str,
    capability: str,
    package: bytes,
    timeout: float,
) -> dict[str, Any]:
    parsed = urllib.parse.urlsplit(base_url)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=timeout)
    try:
        connection.putrequest("POST", "/api/ota")
        connection.putheader("Accept", "application/json")
        connection.putheader("Authorization", f"OTA {capability}")
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(len(package)))
        connection.endheaders()
        sent = 0
        last_percent = -1
        for offset in range(0, len(package), 64 * 1024):
            chunk = package[offset : offset + 64 * 1024]
            connection.send(chunk)
            sent += len(chunk)
            percent = sent * 100 // len(package)
            if percent != last_percent:
                print(f"\rupload {percent:3d}%", end="", file=sys.stderr, flush=True)
                last_percent = percent
        print(file=sys.stderr)
        response = connection.getresponse()
        encoded = response.read()
        status = response.status
    except OSError as error:
        raise CliError(f"POST /api/ota failed: {error}") from error
    finally:
        connection.close()
    try:
        payload = json.loads(encoded.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CliError("POST /api/ota returned invalid JSON") from error
    if not isinstance(payload, dict):
        raise CliError("POST /api/ota returned a non-object JSON response")
    if status < 200 or status >= 300:
        raise CliError(
            f"POST /api/ota returned HTTP {status}: "
            f"{payload.get('error') or 'request failed'}"
        )
    return payload


def _load_and_verify(
    args: argparse.Namespace, *, production: bool = False
) -> tuple[bytes, VerifiedPackage]:
    public_key, public_der = load_public_key(args.public_key)
    if production:
        require_production_key(public_der)
    package = args.package.read_bytes()
    verified = verify_package(
        package,
        public_key,
        public_der,
        expected_version=getattr(args, "version", None),
        expected_source=getattr(args, "source", None),
        expected_product=args.product,
        expected_board=args.board,
        application_address=args.address,
        application_capacity=args.capacity,
    )
    return package, verified


def _add_layout_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--product", default=PRODUCT_ID)
    parser.add_argument("--board", default=BOARD_ID)
    parser.add_argument("--address", type=_integer, default=APPLICATION_ADDRESS)
    parser.add_argument("--capacity", type=_integer, default=APPLICATION_CAPACITY)


def _add_auth_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--base-url", required=True, type=_base_url)
    parser.add_argument("--capability")
    parser.add_argument("--timeout", type=_positive_float, default=10.0)


def _command_build(args: argparse.Namespace) -> None:
    key_path = args.private_key
    if key_path is None:
        configured = os.environ.get(PRIVATE_KEY_ENV)
        if configured:
            key_path = Path(configured)
    if key_path is None:
        raise CliError(
            f"private key path is required via --private-key or {PRIVATE_KEY_ENV}"
        )
    image = args.image.read_bytes()
    private_key = load_private_key(key_path)
    require_production_key(public_key_der(private_key))
    package = build_package(
        image,
        private_key,
        expected_version=args.version,
        expected_source=args.source,
        expected_product=args.product,
        expected_board=args.board,
        application_address=args.address,
        application_capacity=args.capacity,
    )
    args.output.write_bytes(package)
    print(f"wrote {len(package)} bytes to {args.output}")


def _command_verify(args: argparse.Namespace) -> None:
    _, verified = _load_and_verify(args)
    print(
        json.dumps(
            {
                "product": verified.descriptor.product_id,
                "board": verified.descriptor.board_id,
                "version": verified.descriptor.firmware_version,
                "source": verified.descriptor.source_commit,
                "payloadLength": verified.payload_length,
                "sha256": verified.payload_sha256.hex(),
            },
            separators=(",", ":"),
        )
    )


def _command_validate_image(args: argparse.Namespace) -> None:
    _, public_der = load_public_key(args.public_key)
    require_production_key(public_der)
    descriptor = validate_image(
        args.image.read_bytes(),
        key_id=hashlib.sha256(public_der).digest(),
        expected_version=args.version,
        expected_source=args.source,
        expected_product=args.product,
        expected_board=args.board,
        application_address=args.address,
        application_capacity=args.capacity,
    )
    print(
        json.dumps(
            {
                "product": descriptor.product_id,
                "board": descriptor.board_id,
                "version": descriptor.firmware_version,
                "source": descriptor.source_commit,
                "keyId": descriptor.key_id.hex(),
            },
            separators=(",", ":"),
        )
    )


def _command_build_config(args: argparse.Namespace) -> None:
    _, public_der = load_public_key(args.public_key)
    rendered = render_build_config(public_der, args.version, args.source)
    args.output.write_text(rendered, encoding="ascii", newline="\n")
    print(f"wrote OTA build configuration to {args.output}")


def _command_claim(args: argparse.Namespace) -> None:
    _, response = _request(
        args.base_url,
        "POST",
        "/api/ota/session",
        body=json.dumps(
            {"challenge": args.challenge}, separators=(",", ":")
        ).encode("ascii"),
        content_type="application/json",
        timeout=args.timeout,
    )
    capability = response.get("capability")
    if not isinstance(capability, str):
        raise CliError("claim response omitted the capability")
    print(_capability(capability))


def _command_status(args: argparse.Namespace) -> None:
    _, response = _request(
        args.base_url,
        "GET",
        "/api/ota/status",
        capability=_capability(args.capability),
        timeout=args.timeout,
    )
    print(json.dumps(response, separators=(",", ":")))


def _command_upload(args: argparse.Namespace) -> None:
    package, verified = _load_and_verify(args, production=True)
    response = _upload_request(
        args.base_url,
        _capability(args.capability),
        package,
        args.timeout,
    )
    generation = response.get("generation")
    if not isinstance(generation, int) or generation <= 0:
        raise CliError("upload response omitted a valid generation")
    digest = response.get("digest")
    if digest != verified.payload_sha256.hex():
        raise CliError("device staged digest does not match the verified package")
    print(
        json.dumps(
            {
                **response,
                "version": verified.descriptor.firmware_version,
            },
            separators=(",", ":"),
        )
    )


def _command_apply(args: argparse.Namespace) -> None:
    _, verified = _load_and_verify(args, production=True)
    supplied_digest = args.digest
    if (
        len(supplied_digest) != 64
        or any(character not in "0123456789abcdef" for character in supplied_digest)
    ):
        raise CliError("digest must be 64 lowercase hexadecimal characters")
    if supplied_digest != verified.payload_sha256.hex():
        raise CliError("supplied digest does not match the verified package")
    _, status = _request(
        args.base_url,
        "GET",
        "/api/ota/status",
        capability=_capability(args.capability),
        timeout=args.timeout,
    )
    if (
        status.get("state") != "Ready"
        or status.get("generation") != args.generation
        or status.get("digest") != supplied_digest
    ):
        raise CliError("device is not Ready with the requested generation/digest")
    _request(
        args.base_url,
        "POST",
        "/api/ota/apply",
        capability=_capability(args.capability),
        body=json.dumps(
            {
                "generation": args.generation,
                "digest": supplied_digest,
            },
            separators=(",", ":"),
        ).encode("ascii"),
        content_type="application/json",
        timeout=args.timeout,
    )
    _verify_rebooted_version(
        args.verify_url or f"{args.base_url}/api/version",
        args.version or verified.descriptor.firmware_version,
        args.version_field,
        args.reboot_timeout,
        args.timeout,
    )
    print("apply accepted")


def _verify_rebooted_version(
    url: str,
    expected_version: str,
    version_field: str,
    reboot_timeout: float,
    request_timeout: float,
) -> None:
    deadline = time.monotonic() + reboot_timeout
    last_error = "device did not become available"
    while time.monotonic() < deadline:
        try:
            request = urllib.request.Request(url, headers={"Accept": "application/json"})
            with _NO_REDIRECT_OPENER.open(request, timeout=request_timeout) as response:
                if response.status < 200 or response.status >= 300:
                    last_error = f"version endpoint returned HTTP {response.status}"
                else:
                    payload = json.loads(response.read().decode("utf-8"))
                    if not isinstance(payload, dict):
                        raise CliError("version endpoint returned non-object JSON")
                    observed = payload.get(version_field)
                    if observed == expected_version:
                        return
                    if observed is not None:
                        last_error = f"version endpoint reported {observed!r}"
        except urllib.error.HTTPError as error:
            last_error = f"version endpoint returned HTTP {error.code}"
        except (urllib.error.URLError, TimeoutError):
            last_error = "device is unavailable while rebooting"
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            last_error = f"version endpoint returned invalid JSON: {error}"
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(1.0, remaining))
    raise CliError(
        f"reboot/version verification timed out for {expected_version}: {last_error}"
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Build, verify, and upload AZ3166 OTA packages")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build = subparsers.add_parser("build", help="build and sign a package")
    build.add_argument("--image", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--private-key", type=Path)
    build.add_argument("--version", required=True)
    build.add_argument("--source", required=True)
    _add_layout_arguments(build)
    build.set_defaults(handler=_command_build)

    verify = subparsers.add_parser("verify", help="verify a signed package")
    verify.add_argument("--package", type=Path, required=True)
    verify.add_argument("--public-key", type=Path, required=True)
    verify.add_argument("--version")
    verify.add_argument("--source")
    _add_layout_arguments(verify)
    verify.set_defaults(handler=_command_verify)

    validate = subparsers.add_parser(
        "validate-image", help="validate a production firmware image"
    )
    validate.add_argument("--image", type=Path, required=True)
    validate.add_argument("--public-key", type=Path, required=True)
    validate.add_argument("--version", required=True)
    validate.add_argument("--source", required=True)
    _add_layout_arguments(validate)
    validate.set_defaults(handler=_command_validate_image)

    build_config = subparsers.add_parser(
        "build-config", help="generate public firmware build configuration"
    )
    build_config.add_argument("--public-key", type=Path, required=True)
    build_config.add_argument("--output", type=Path, required=True)
    build_config.add_argument("--version", required=True)
    build_config.add_argument("--source", required=True)
    build_config.set_defaults(handler=_command_build_config)

    claim = subparsers.add_parser("claim", help="claim a physical OTA challenge")
    claim.add_argument("--base-url", required=True, type=_base_url)
    claim.add_argument("--challenge", required=True)
    claim.add_argument("--timeout", type=_positive_float, default=10.0)
    claim.set_defaults(handler=_command_claim)

    status = subparsers.add_parser("status", help="read authorized OTA status")
    _add_auth_arguments(status)
    status.set_defaults(handler=_command_status)

    upload = subparsers.add_parser("upload", help="verify and upload a package")
    _add_auth_arguments(upload)
    upload.add_argument("--package", type=Path, required=True)
    upload.add_argument("--public-key", type=Path, required=True)
    upload.add_argument("--version")
    upload.add_argument("--source")
    _add_layout_arguments(upload)
    upload.set_defaults(handler=_command_upload)

    apply = subparsers.add_parser("apply", help="explicitly apply a Ready generation")
    _add_auth_arguments(apply)
    apply.add_argument("--package", type=Path, required=True)
    apply.add_argument("--public-key", type=Path, required=True)
    apply.add_argument("--generation", type=_positive_integer, required=True)
    apply.add_argument("--digest", required=True)
    apply.add_argument("--version")
    apply.add_argument("--source")
    apply.add_argument(
        "--verify-url",
        help="JSON version endpoint (default: BASE_URL/api/version)",
    )
    apply.add_argument("--version-field", default="firmwareVersion")
    apply.add_argument("--reboot-timeout", type=_positive_float, default=120.0)
    _add_layout_arguments(apply)
    apply.set_defaults(handler=_command_apply)
    return parser


def main() -> int:
    try:
        args = _parser().parse_args()
        args.handler(args)
        return 0
    except (CliError, PackageError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
