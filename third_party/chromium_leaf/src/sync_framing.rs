// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file.

//! Sync VLESS Vision framing; logic matches upstream Leaf leaf/src/proxy/vless (see submodule).
mod plain_tcp_header;

pub use plain_tcp_header::build_vless_tcp_header_plain;

pub fn build_vless_tcp_header(
    uuid_bytes: &[u8; 16],
    dst_addr: &str,
    dst_port: u16,
    addr_type: u8,
) -> Vec<u8> {
    let mut vless_header = vec![];
    vless_header.push(0x00);
    vless_header.extend_from_slice(uuid_bytes);

    let flow_str = b"xtls-rprx-vision";
    vless_header.push(18);
    vless_header.push(0x0a);
    vless_header.push(16);
    vless_header.extend_from_slice(flow_str);
    vless_header.push(0x01);

    vless_header.push((dst_port >> 8) as u8);
    vless_header.push((dst_port & 0xFF) as u8);
    vless_header.push(addr_type);

    match addr_type {
        1 => {
            let parts: Vec<u8> = dst_addr.split('.').map(|s| s.parse().unwrap()).collect();
            vless_header.extend_from_slice(&parts);
        }
        2 => {
            vless_header.push(dst_addr.len() as u8);
            vless_header.extend_from_slice(dst_addr.as_bytes());
        }
        3 => {
            let addr: std::net::Ipv6Addr = dst_addr.parse().unwrap();
            vless_header.extend_from_slice(&addr.octets());
        }
        _ => unreachable!(),
    }
    vless_header
}

pub struct VisionParser {
    uuid_bytes: [u8; 16],
    v_remaining_cmd: i32,
    v_remaining_content: i32,
    v_remaining_padding: i32,
    v_current_cmd: u8,
    v_buffer: Vec<u8>,
    vless_response_header_parsed: bool,
    pub v_direct_copy_rx: bool,
    pub v_vision_done: bool,
}

impl VisionParser {
    pub fn new(uuid_bytes: [u8; 16]) -> Self {
        Self {
            uuid_bytes,
            v_remaining_cmd: -1,
            v_remaining_content: -1,
            v_remaining_padding: -1,
            v_current_cmd: 0,
            v_buffer: Vec::new(),
            vless_response_header_parsed: false,
            v_direct_copy_rx: false,
            v_vision_done: false,
        }
    }

    pub fn parse(&mut self, data: &[u8]) -> Vec<u8> {
        self.v_buffer.extend_from_slice(data);
        let mut to_client = Vec::new();
        let mut offset = 0;

        if !self.vless_response_header_parsed {
            if self.v_buffer.len() >= 2 {
                self.vless_response_header_parsed = true;
                offset += 2;
            } else {
                return to_client;
            }
        }

        while offset < self.v_buffer.len() {
            if self.v_direct_copy_rx {
                to_client.extend_from_slice(&self.v_buffer[offset..]);
                offset = self.v_buffer.len();
                break;
            }

            if self.v_remaining_cmd == -1
                && self.v_remaining_content == -1
                && self.v_remaining_padding == -1
            {
                if self.v_buffer.len() - offset >= 21
                    && &self.v_buffer[offset..offset + 16] == self.uuid_bytes.as_slice()
                {
                    offset += 16;
                    self.v_remaining_cmd = 5;
                } else if self.v_buffer.len() - offset < 21 {
                    break;
                } else {
                    self.v_vision_done = true;
                    to_client.extend_from_slice(&self.v_buffer[offset..]);
                    offset = self.v_buffer.len();
                    break;
                }
            }

            while offset < self.v_buffer.len() && self.v_remaining_cmd > 0 {
                let data = self.v_buffer[offset];
                offset += 1;
                match self.v_remaining_cmd {
                    5 => self.v_current_cmd = data,
                    4 => self.v_remaining_content = (data as i32) << 8,
                    3 => self.v_remaining_content |= data as i32,
                    2 => self.v_remaining_padding = (data as i32) << 8,
                    1 => self.v_remaining_padding |= data as i32,
                    _ => {}
                }
                self.v_remaining_cmd -= 1;
            }

            if self.v_remaining_cmd <= 0 && self.v_remaining_content > 0 {
                let available = (self.v_buffer.len() - offset) as i32;
                let consume = if available < self.v_remaining_content {
                    available
                } else {
                    self.v_remaining_content
                };
                if consume > 0 {
                    let consume_usize = consume as usize;
                    to_client.extend_from_slice(&self.v_buffer[offset..offset + consume_usize]);
                    offset += consume_usize;
                    self.v_remaining_content -= consume;
                }
            } else if self.v_remaining_cmd <= 0 && self.v_remaining_padding > 0 {
                let available = (self.v_buffer.len() - offset) as i32;
                let consume = if available < self.v_remaining_padding {
                    available
                } else {
                    self.v_remaining_padding
                };
                if consume > 0 {
                    offset += consume as usize;
                    self.v_remaining_padding -= consume;
                }
            }

            if self.v_remaining_cmd <= 0
                && self.v_remaining_content <= 0
                && self.v_remaining_padding <= 0
            {
                if self.v_current_cmd == 0 {
                    self.v_remaining_cmd = 5;
                } else {
                    self.v_remaining_cmd = -1;
                    self.v_remaining_content = -1;
                    self.v_remaining_padding = -1;
                    if self.v_current_cmd == 2 {
                        self.v_direct_copy_rx = true;
                    } else {
                        self.v_vision_done = true;
                    }
                    if offset < self.v_buffer.len() {
                        to_client.extend_from_slice(&self.v_buffer[offset..]);
                        offset = self.v_buffer.len();
                    }
                    break;
                }
            }
        }

        if offset < self.v_buffer.len() {
            let remaining = self.v_buffer.len() - offset;
            let mut new_vec = Vec::with_capacity(remaining);
            new_vec.extend_from_slice(&self.v_buffer[offset..]);
            self.v_buffer = new_vec;
        } else {
            self.v_buffer.clear();
        }

        to_client
    }
}
