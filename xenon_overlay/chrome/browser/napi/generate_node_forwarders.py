#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import pathlib
import re


SYMBOL_PATTERN = re.compile(r"\b(?:napi_|uv_|xenon_napi_)[A-Za-z0-9_]+(?=\s*\()")

# Some Electron addons import these Node/V8 C++ symbols in addition to N-API.
DECORATED_COMPATIBILITY_SYMBOLS = (
    "??1CallbackScope@node@@QEAA@XZ",
    "??4CallbackScope@node@@QEAAAEAV01@AEBV01@@Z",
    "?Reallocate@Allocator@ArrayBuffer@v8@@UEAAPEAXPEAX_K1@Z",
)


def collect_symbols(inputs: list[pathlib.Path]) -> list[str]:
    symbols = {"node_module_register"}
    for input_path in inputs:
        symbols.update(SYMBOL_PATTERN.findall(input_path.read_text(encoding="utf-8")))

    # These are macro helpers or addon entry points, not host exports.
    symbols = {
        symbol
        for symbol in symbols
        if not symbol.endswith("__") and symbol != "napi_register_module_v1"
    }
    if len(symbols) < 400:
        raise ValueError(f"Only found {len(symbols)} Node API symbols")
    return sorted(symbols)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate Windows exports that forward Node APIs to xenon.dll"
    )
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("inputs", nargs="+", type=pathlib.Path)
    args = parser.parse_args()

    symbols = collect_symbols(args.inputs)
    lines = ["EXPORTS"]
    lines.extend(f"  {symbol}=xenon.{symbol}" for symbol in symbols)
    lines.extend(
        f'  "{symbol}"="xenon.{symbol}"'
        for symbol in DECORATED_COMPATIBILITY_SYMBOLS
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
