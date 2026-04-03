// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Chromium-facing shim for the upstream Leaf proxy stack (`//third_party/leaf`).
//!
//! **Outbound handshake** is implemented here by calling into the `leaf` crate
//! once it and its Tokio dependency graph are vendored into `//third_party/rust`
//! and linked from `//third_party/chromium_leaf/BUILD.gn`. Until then,
//! `chromium_leaf_outbound_handshake` returns `ERR_NOT_IMPLEMENTED` (-11) so
//! Chromium does not carry a second, hand-rolled VLESS/VMess implementation.

/// Same as `net::ERR_NOT_IMPLEMENTED` in `net/base/net_error_list.h`.
const CHROMIUM_NET_ERR_NOT_IMPLEMENTED: i32 = -11;

/// Increment when the cxx / C ABI surface changes.
pub const CHROMIUM_LEAF_FFI_ABI_VERSION: i32 = 4;

#[cxx::bridge(namespace = net::chromium_leaf)]
mod ffi {
    extern "Rust" {
        fn chromium_leaf_ffi_abi_version() -> i32;
    }
}

pub fn chromium_leaf_ffi_abi_version() -> i32 {
    CHROMIUM_LEAF_FFI_ABI_VERSION
}

/// C ABI for `//net/socket/chromium_leaf_glue.cc`. Handshake must eventually
/// build a Leaf JSON (or session) from the URI pieces and run the matching
/// `leaf::proxy::*::outbound::Handler` on `tokio::net::TcpStream::from_std(...)`
/// created from `transport_socket` (platform-specific `RawFd` / `RawSocket`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn chromium_leaf_outbound_handshake(
    _protocol: u32,
    _transport_socket: u64,
    _dest_host: *const std::ffi::c_char,
    _dest_host_len: usize,
    _dest_port: u16,
    _credential: *const std::ffi::c_char,
    _credential_len: usize,
    _leaf_uri_query: *const std::ffi::c_char,
    _leaf_uri_query_len: usize,
    _leaf_uri_fragment: *const std::ffi::c_char,
    _leaf_uri_fragment_len: usize,
    _leaf_proxy_authority_host: *const std::ffi::c_char,
    _leaf_proxy_authority_host_len: usize,
) -> i32 {
    // TODO: `use leaf::...` after `leaf` + `tokio` are available as GN
    // `rust_static_library` / `cargo_crate` targets (see README.md).
    CHROMIUM_NET_ERR_NOT_IMPLEMENTED
}
