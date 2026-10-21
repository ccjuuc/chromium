#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Validate explicit Xenon Updater build configuration, without network I/O."""

import argparse
import base64
import binascii
import pathlib
import re
import urllib.parse


def validate(update_url, team_id, crx_public_key_hash,
             allow_insecure_localhost=False):
    if not update_url or any(ord(char) <= 32 or ord(char) >= 127
                             for char in update_url):
        raise ValueError('xenon_updater_update_url must be an explicit ASCII URL')
    if any(char in update_url for char in "\\\"'"):
        raise ValueError('update URL must not contain quotes or backslashes')
    try:
        url = urllib.parse.urlsplit(update_url)
        port = url.port
    except ValueError as error:
        raise ValueError('invalid update URL') from error
    if (not url.hostname or url.username is not None or url.password is not None
            or url.fragment or port == 0):
        raise ValueError('update URL must have a host and no credentials/fragment')
    if url.scheme != 'https' and not (
            allow_insecure_localhost and url.scheme == 'http'
            and url.hostname in ('localhost', '127.0.0.1', '::1')):
        raise ValueError('update URL requires HTTPS; loopback HTTP needs explicit '
                         'xenon_updater_allow_insecure_localhost=true')
    if team_id and not re.fullmatch(r'[A-Z0-9]{10}', team_id):
        raise ValueError('xenon_updater_mac_team_id must be an Apple Team ID')
    try:
        decoded = base64.b64decode(crx_public_key_hash, validate=True)
    except (ValueError, binascii.Error) as error:
        raise ValueError('CRX public-key hash must be standard base64') from error
    if (len(decoded) != 32
            or base64.b64encode(decoded).decode('ascii') != crx_public_key_hash):
        raise ValueError('CRX public-key hash must encode exactly 32 bytes')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--update-url', required=True)
    parser.add_argument('--team-id', default='')
    parser.add_argument('--crx-public-key-hash', default='')
    parser.add_argument('--allow-insecure-localhost', action='store_true')
    parser.add_argument('--stamp', type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        validate(args.update_url, args.team_id, args.crx_public_key_hash,
                 args.allow_insecure_localhost)
    except ValueError as error:
        parser.error(str(error))
    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text('Xenon Updater branding validated\n', encoding='utf-8')


if __name__ == '__main__':
    main()
