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
    "??0HandleScope@v8@@QEAA@PEAVIsolate@1@@Z",
    "??1HandleScope@v8@@QEAA@XZ",
    "?Call@Function@v8@@QEAA?AV?$MaybeLocal@VValue@v8@@@2@V?$Local@VContext@v8@@@2@V?$Local@VValue@v8@@@2@HQEAV52@@Z",
    "?ClearWeak@V8@v8@@CAPEAXPEA_K@Z",
    "?CreateHandle@HandleScope@v8@@KAPEA_KPEAVIsolate@internal@2@_K@Z",
    "?DisposeGlobal@V8@v8@@CAXPEA_K@Z",
    "?GetCurrent@Isolate@v8@@SAPEAV12@XZ",
    "?GetCurrentContext@Isolate@v8@@QEAA?AV?$Local@VContext@v8@@@2@XZ",
    "?GlobalizeReference@V8@v8@@CAPEA_KPEAVIsolate@internal@2@PEA_K@Z",
    "?MakeWeak@V8@v8@@CAXPEA_KPEAXP6AXAEBV?$WeakCallbackInfo@X@2@@ZW4WeakCallbackType@2@@Z",
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
