# xenon_ai_inferencer_burn

Sidecar binary (Burn + Llama) built **outside** the main `gn` Rust graph, using
Chromium’s toolchain per `docs/rust.md` (“Using cargo”):

- **Build driver**: `build_inferencer_burn.py` → `tools/crates/run_cargo.py` →
  `//third_party/rust-toolchain` (not rustup on `PATH`).
- **Dependencies**: `Cargo.toml` / `Cargo.lock` (may fetch crates from the
  network on first build; full `//third_party/rust` vendoring is optional and
  heavy—see `xenon_overlay/docs/Chromium接入Rust库指南.md`).

## GN

- Target: `//xenon_overlay/xenon_ai_inferencer_burn:xenon_ai_inferencer_burn`
- Flags: `enable_xenon_ai`, `build_xenon_ai_inferencer_burn`,
  `xenon_ai_inferencer_burn_cpu_only` (CPU ndarray backend; smaller, slower).

## Manual build

GN passes absolute paths via `rebase_path`. To run the same driver by hand from
your checkout’s **`src/`** directory (adjust paths if your layout differs):

```bash
python3 xenon_overlay/xenon_ai_inferencer_burn/build_inferencer_burn.py \
  --run-cargo "$PWD/tools/crates/run_cargo.py" \
  --rust-sysroot "$PWD/third_party/rust-toolchain" \
  --crate-root "$PWD/xenon_overlay/xenon_ai_inferencer_burn" \
  --out-dir "/path/to/out/Directory"
```

CPU-only: append `--no-default-features --features backend_ndarray`.

Or call `tools/crates/run_cargo.py` directly (same as `--run-cargo` does):

```bash
python3 tools/crates/run_cargo.py build --release \
  --manifest-path=xenon_overlay/xenon_ai_inferencer_burn/Cargo.toml
```
