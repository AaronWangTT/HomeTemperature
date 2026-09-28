from __future__ import annotations

# SHA-256 of each approved canonical DER SubjectPublicKeyInfo, lowercase hex.
# Production release operations fail closed while this reviewed set is empty.
PRODUCTION_KEY_IDS: frozenset[str] = frozenset()
