#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Create NLP raw X25519 identity files without overwriting keys."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey


def create(private, public):
    if private.resolve() == public.resolve():
        raise ValueError("private and public paths must differ")
    key = X25519PrivateKey.generate()
    raw = key.private_bytes_raw()
    pub = key.public_key().public_bytes_raw()
    private_fd = os.open(private, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    identity = os.fstat(private_fd)
    try:
        public_fd = os.open(public, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except BaseException:
        os.close(private_fd)
        # Only remove the exact file created by this invocation.
        if private.stat().st_ino == identity.st_ino:
            private.unlink()
        raise
    with os.fdopen(private_fd, "wb") as output:
        output.write(raw)
        output.flush()
        os.fsync(output.fileno())
    with os.fdopen(public_fd, "wb") as output:
        output.write(pub)
        output.flush()
        os.fsync(output.fileno())
    return {
        "uid_hex": hashlib.sha256(pub).digest()[:16].hex(),
        "public_key_sha256": hashlib.sha256(pub).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private", type=Path, required=True)
    parser.add_argument("--public", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(create(args.private, args.public)))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"nlp-keygen: {error}", file=sys.stderr)
        sys.exit(1)
