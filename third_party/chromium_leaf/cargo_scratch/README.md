# cargo_scratch

Optional **standalone Cargo** crate to experiment with the vendored `leaf` library
(`//third_party/leaf`) outside Chromium's `rust_static_library` GN rules.

It lives next to `//third_party/chromium_leaf` (the small `chromium_leaf_ffi` shim) so
all Leaf-related **source** stays under one tree. **Build output** is not written here:
`.cargo/config.toml` sets `target-dir` to `//out/cargo_chromium_leaf_target`.

## Commands

From the Chromium `src` directory:

```sh
cargo check --manifest-path third_party/chromium_leaf/cargo_scratch/Cargo.toml
cargo build --manifest-path third_party/chromium_leaf/cargo_scratch/Cargo.toml
```

Override output directory if needed:

```sh
cargo build --manifest-path third_party/chromium_leaf/cargo_scratch/Cargo.toml \
  --target-dir out/Debug_64/obj/chromium_leaf_cargo_target
```
