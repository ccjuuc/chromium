// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file.

//! Plain VLESS TCP request header (no add-ons / no Vision `xtls-rprx-vision` block).
//! Kept aligned with upstream Leaf `plain_tcp_header` / `build_vless_tcp_header_plain`.

/// `addr_type`: `1` IPv4, `2` domain, `3` IPv6 (same as Vision builder in `sync_framing.rs`).
pub fn build_vless_tcp_header_plain(
    uuid_bytes: &[u8; 16],
    dst_addr: &str,
    dst_port: u16,
    addr_type: u8,
) -> Vec<u8> {
    let mut vless_header = Vec::new();
    vless_header.push(0x00);
    vless_header.extend_from_slice(uuid_bytes.as_slice());
    vless_header.push(0x00); // add-on length
    vless_header.push(0x01); // command: TCP

    vless_header.push((dst_port >> 8) as u8);
    vless_header.push((dst_port & 0xFF) as u8);
    vless_header.push(addr_type);

    match addr_type {
        1 => {
            let parts: Vec<u8> = dst_addr.split('.').map(|s| s.parse().unwrap()).collect();
            vless_header.extend_from_slice(&parts);
        }
        2 => {
            if dst_addr.len() > 255 {
                return Vec::new();
            }
            vless_header.push(dst_addr.len() as u8);
            vless_header.extend_from_slice(dst_addr.as_bytes());
        }
        3 => {
            let addr: std::net::Ipv6Addr = dst_addr.parse().unwrap();
            vless_header.extend_from_slice(&addr.octets());
        }
        _ => return Vec::new(),
    }
    vless_header
}
