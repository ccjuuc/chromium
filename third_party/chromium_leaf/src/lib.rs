// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Chromium-facing shim: Leaf VLESS sync framing (`sync_framing`) + cxx FFI.

const CHROMIUM_NET_ERR_NOT_IMPLEMENTED: i32 = -11;

pub const CHROMIUM_LEAF_FFI_ABI_VERSION: i32 = 6;

mod sync_framing;

#[cxx::bridge(namespace = "net::chromium_leaf")]
mod ffi {
    extern "Rust" {
        type ChromiumLeafVisionParser;

        fn chromium_leaf_ffi_abi_version() -> i32;

        fn chromium_leaf_vless_plain_tcp_header(
            uuid: [u8; 16],
            dest_host: String,
            dest_port: u16,
        ) -> Vec<u8>;

        fn chromium_leaf_vless_vision_tcp_header(
            uuid: [u8; 16],
            dest_host: String,
            dest_port: u16,
        ) -> Vec<u8>;

        fn chromium_leaf_vision_parser_new(
            uuid: [u8; 16],
        ) -> Box<ChromiumLeafVisionParser>;

        fn chromium_leaf_vision_parser_feed(
            parser: Pin<&mut ChromiumLeafVisionParser>,
            data: &[u8],
        ) -> Vec<u8>;

        fn chromium_leaf_vision_parser_direct_copy(
            parser: &ChromiumLeafVisionParser,
        ) -> bool;

        fn chromium_leaf_vision_parser_vision_done(
            parser: &ChromiumLeafVisionParser,
        ) -> bool;
    }
}

pub fn chromium_leaf_ffi_abi_version() -> i32 {
    CHROMIUM_LEAF_FFI_ABI_VERSION
}

pub fn chromium_leaf_vless_plain_tcp_header(
    uuid: [u8; 16],
    dest_host: String,
    dest_port: u16,
) -> Vec<u8> {
    sync_framing::build_vless_tcp_header_plain(&uuid, &dest_host, dest_port, 2)
}

pub fn chromium_leaf_vless_vision_tcp_header(
    uuid: [u8; 16],
    dest_host: String,
    dest_port: u16,
) -> Vec<u8> {
    if dest_host.len() > 255 {
        return Vec::new();
    }
    sync_framing::build_vless_tcp_header(&uuid, &dest_host, dest_port, 2)
}

pub struct ChromiumLeafVisionParser {
    inner: sync_framing::VisionParser,
}

pub fn chromium_leaf_vision_parser_new(
    uuid: [u8; 16],
) -> Box<ChromiumLeafVisionParser> {
    Box::new(ChromiumLeafVisionParser {
        inner: sync_framing::VisionParser::new(uuid),
    })
}

pub fn chromium_leaf_vision_parser_feed(
    parser: std::pin::Pin<&mut ChromiumLeafVisionParser>,
    data: &[u8],
) -> Vec<u8> {
    parser.get_mut().inner.parse(data)
}

pub fn chromium_leaf_vision_parser_direct_copy(
    parser: &ChromiumLeafVisionParser,
) -> bool {
    parser.inner.v_direct_copy_rx
}

pub fn chromium_leaf_vision_parser_vision_done(
    parser: &ChromiumLeafVisionParser,
) -> bool {
    parser.inner.v_vision_done
}

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
    CHROMIUM_NET_ERR_NOT_IMPLEMENTED
}
