// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file.

//! Optional local harness to `cargo check` //third_party/leaf from outside GN.
//! Build artifacts go to //out/cargo_chromium_leaf_target (see `.cargo/config.toml`).

#![allow(dead_code)]

/// Placeholder so the lib crate links; add real experiments here as needed.
pub fn chromium_leaf_cargo_test_marker() -> u32 {
    1
}
