#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Preflight public ASAR decoding configuration before replacing a runtime."""

from __future__ import annotations

import base64
import binascii
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


_MAX_HEADER_SIZE = 64 * 1024 * 1024
_MAX_PUBLIC_KEY_SIZE = 16 * 1024
_PUBLIC_KEY = re.compile(
    rb'\s*-----BEGIN (PUBLIC KEY|RSA PUBLIC KEY)-----\s+'
    rb'([A-Za-z0-9+/=\s]+?)\s+-----END \1-----\s*')


def is_standard_asar(path: Path) -> bool:
    """Recognize the public Pickle/JSON index without reading member contents."""
    with path.open('rb') as archive:
        prefix = archive.read(16)
        if len(prefix) != 16:
            return False
        size_payload, header_size, header_payload, json_size = struct.unpack(
            '<IIII', prefix)
        if (size_payload != 4 or header_size < 8 or
                header_size > _MAX_HEADER_SIZE or
                header_size > path.stat().st_size - 8 or
                header_payload != header_size - 4 or
                json_size > header_size - 8):
            return False
        try:
            index = json.loads(archive.read(json_size).decode('utf-8'))
        except (ValueError, UnicodeError):
            return False
        if not isinstance(index, dict) or not isinstance(index.get('files'), dict):
            return False
        pending = [index]
        while pending:
            node = pending.pop()
            if node.get('encrypted') is True:
                return False
            files = node.get('files')
            if isinstance(files, dict):
                pending.extend(value for value in files.values() if isinstance(value, dict))
        return True


def read_public_key(path: Path) -> bytes:
    """Accept one RSA public PEM; never emit its contents or accept private PEM."""
    with path.open('rb') as key_file:
        pem = key_file.read(_MAX_PUBLIC_KEY_SIZE + 1)
    match = _PUBLIC_KEY.fullmatch(pem)
    if len(pem) > _MAX_PUBLIC_KEY_SIZE or not match:
        raise ValueError(f'ASAR key must contain one PUBLIC KEY or RSA PUBLIC KEY: {path}')
    try:
        base64.b64decode(re.sub(rb'\s+', b'', match[2]), validate=True)
    except (ValueError, binascii.Error) as error:
        raise ValueError(f'invalid ASAR public key encoding: {path}') from error
    node = shutil.which('node')
    if not node:
        raise ValueError('Node.js is required to validate the ASAR public key')
    # Read from stdin so validation applies to the exact bytes later published,
    # even if the source key file is replaced during synchronization.
    script = r"""
try {
  const {createPublicKey} = require('node:crypto');
  const key = createPublicKey(require('node:fs').readFileSync(0));
  if (key.asymmetricKeyType !== 'rsa' ||
      key.asymmetricKeyDetails.modulusLength !== 2048) process.exitCode = 1;
} catch { process.exitCode = 1; }
"""
    result = subprocess.run([node, '-e', script], input=pem,
                            capture_output=True, check=False)
    if result.returncode:
        raise ValueError(f'invalid RSA-2048 ASAR public key: {path}')
    return pem


def preflight_asar_public_key(roots: list[Path], explicit: Path | None,
                              default: Path) -> bytes | None:
    """Require a validated public key for any nonstandard copied archive."""
    key_path = explicit.resolve() if explicit is not None else default.resolve()
    pem = read_public_key(key_path) if explicit is not None or key_path.is_file() else None
    if pem is not None:
        return pem
    seen = set()
    for root in roots:
        candidates = [root] if root.is_file() else root.rglob('*')
        for path in candidates:
            if path.suffix.lower() != '.asar' or not path.is_file():
                continue
            canonical = path.resolve()
            if canonical in seen:
                continue
            seen.add(canonical)
            if not is_standard_asar(path):
                raise ValueError(
                    f'encrypted or nonstandard ASAR requires --asar-public-key: {path}')
    return None


def write_asar_public_key(app_directory: Path, pem: bytes | None) -> None:
    if pem is not None:
        app_directory.mkdir(parents=True, exist_ok=True)
        (app_directory / 'asar-public-key.pem').write_bytes(pem)
