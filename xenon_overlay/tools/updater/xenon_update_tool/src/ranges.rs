// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Locate nested gzip streams without rewriting signed CRX or PKG bytes.

use anyhow::{ensure, Context, Result};
use flate2::{Decompress, FlushDecompress, Status};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use zip::{CompressionMethod, ZipArchive};

const MAX_TOC_SIZE: u64 = 8 * 1024 * 1024;

fn read_at(source: &mut File, offset: u64, length: u64, end: u64) -> Result<Vec<u8>> {
    ensure!(offset <= end && length <= end - offset, "Archive member lies outside its container");
    let mut data = vec![0; usize::try_from(length)?];
    source.seek(SeekFrom::Start(offset))?;
    source.read_exact(&mut data).context("Truncated archive member")?;
    Ok(data)
}

fn child_text<'a>(node: roxmltree::Node<'a, 'a>, tag: &str) -> Option<&'a str> {
    node.children().find(|child| child.has_tag_name(tag)).and_then(|child| child.text())
}

fn check_pkg_directory(source: &mut File, directory_start: u64) -> Result<()> {
    // zip's name map can collapse duplicate central-directory entries. Inspect
    // the raw directory too, so an ambiguous signed PKG is never optimized.
    let end = source.metadata()?.len();
    let mut position = directory_start;
    let mut packages = 0;
    loop {
        let magic = read_at(source, position, 4, end)?;
        if magic != b"PK\x01\x02" {
            ensure!(
                magic == b"PK\x05\x06" || magic == b"PK\x06\x06" || magic == b"PK\x05\x05",
                "Invalid ZIP central directory terminator"
            );
            break;
        }
        let header = read_at(source, position, 46, end)?;
        let name_size = u16::from_le_bytes(header[28..30].try_into()?) as u64;
        let extra_size = u16::from_le_bytes(header[30..32].try_into()?) as u64;
        let comment_size = u16::from_le_bytes(header[32..34].try_into()?) as u64;
        let name = read_at(source, position + 46, name_size, end)?;
        if name == b"application.pkg" {
            packages += 1;
            ensure!(packages == 1, "Duplicate application.pkg in CRX");
            ensure!(
                u16::from_le_bytes(header[8..10].try_into()?) & 1 == 0,
                "Encrypted application.pkg is not supported"
            );
        }
        position = position
            .checked_add(46 + name_size + extra_size + comment_size)
            .context("ZIP central directory extent overflow")?;
    }
    ensure!(packages == 1, "Missing application.pkg ZIP directory entry");
    Ok(())
}

fn xar_gzip_ranges(source: &mut File, start: u64, size: u64) -> Result<Vec<(u64, u64)>> {
    let end = start.checked_add(size).context("PKG extent overflow")?;
    let header = read_at(source, start, 28, end)?;
    let header_size = u16::from_be_bytes(header[4..6].try_into()?) as u64;
    ensure!(
        &header[..4] == b"xar!"
            && header_size >= 28
            && u16::from_be_bytes(header[6..8].try_into()?) == 1,
        "Invalid flat PKG XAR header"
    );
    let compressed_size = u64::from_be_bytes(header[8..16].try_into()?);
    let toc_size = u64::from_be_bytes(header[16..24].try_into()?);
    ensure!(
        (1..=MAX_TOC_SIZE).contains(&toc_size) && (1..=MAX_TOC_SIZE).contains(&compressed_size),
        "PKG table of contents exceeds the size limit"
    );
    let toc_start = start.checked_add(header_size).context("PKG header extent overflow")?;
    let compressed = read_at(source, toc_start, compressed_size, end)?;
    let mut xml = vec![0; (toc_size + 1) as usize];
    let mut decoder = Decompress::new(true);
    let status = decoder
        .decompress(&compressed, &mut xml, FlushDecompress::Finish)
        .context("Invalid compressed PKG table of contents")?;
    ensure!(
        status == Status::StreamEnd
            && decoder.total_in() == compressed_size
            && decoder.total_out() == toc_size,
        "Invalid PKG table of contents size"
    );
    xml.truncate(toc_size as usize);
    let xml = std::str::from_utf8(&xml).context("PKG table of contents is not UTF-8")?;
    ensure!(
        !xml.contains("<!DOCTYPE") && !xml.contains("<!ENTITY"),
        "PKG table of contents must not contain XML entities"
    );
    let document = roxmltree::Document::parse(xml).context("Invalid PKG table of contents XML")?;
    let root = document.root_element();
    ensure!(
        root.has_tag_name("xar") && root.children().any(|node| node.has_tag_name("toc")),
        "Missing PKG table of contents"
    );
    let heap = toc_start.checked_add(compressed_size).context("PKG heap extent overflow")?;
    let mut members = Vec::new();
    let mut ranges = Vec::new();
    for entry in root.descendants().filter(|node| node.has_tag_name("file")) {
        let Some(data) = entry.children().find(|node| node.has_tag_name("data")) else { continue };
        let offset: u64 = child_text(data, "offset")
            .context("Missing PKG member offset")?
            .parse()
            .context("Invalid PKG member extent")?;
        let length: u64 = child_text(data, "length")
            .context("Missing PKG member length")?
            .parse()
            .context("Invalid PKG member extent")?;
        let absolute = heap.checked_add(offset).context("PKG member extent overflow")?;
        ensure!(
            absolute <= end && length <= end - absolute,
            "PKG member lies outside its container"
        );
        if length > 0 {
            members.push((absolute, length));
        }
        let encoding = data.children().find(|node| node.has_tag_name("encoding"));
        if child_text(entry, "name") == Some("Payload")
            && encoding.and_then(|node| node.attribute("style")) == Some("application/octet-stream")
            && length >= 3
            && read_at(source, absolute, 3, end)? == b"\x1f\x8b\x08"
        {
            ensure!(length >= 18, "Truncated gzip Payload");
            ranges.push((absolute, length));
        }
    }
    members.sort_unstable();
    let mut previous_end = heap;
    for (offset, length) in members {
        ensure!(offset >= previous_end, "Overlapping PKG members");
        previous_end = offset + length;
    }
    ranges.sort_unstable();
    Ok(ranges)
}

/// Absolute byte ranges of gzip Payloads in a stored application.pkg ZIP member.
/// Normal ZIP payloads and deflated PKGs use Puffin's own ZIP range detection.
pub fn nested_gzip_ranges(path: &Path) -> Result<Vec<(u64, u64)>> {
    let mut archive = ZipArchive::new(File::open(path)?).context("Invalid CRX ZIP payload")?;
    let mut package = None;
    for index in 0..archive.len() {
        let entry = archive.by_index_raw(index).context("Invalid CRX ZIP member")?;
        if entry.name() == "application.pkg" {
            ensure!(package.is_none(), "Duplicate application.pkg in CRX");
            package = Some((
                entry.compression(),
                entry.compressed_size(),
                entry.size(),
                entry.header_start(),
                entry.data_start(),
            ));
        }
    }
    let Some((compression, compressed_size, size, header_start, data_start)) = package else {
        return Ok(Vec::new());
    };
    if compression != CompressionMethod::Stored {
        return Ok(Vec::new());
    }
    ensure!(compressed_size == size, "Invalid stored application.pkg");
    let mut source = File::open(path)?;
    let directory_start = archive.central_directory_start();
    check_pkg_directory(&mut source, directory_start)?;
    let header = read_at(&mut source, header_start, 30, directory_start)?;
    let flags = u16::from_le_bytes(header[6..8].try_into()?);
    ensure!(
        &header[..4] == b"PK\x03\x04"
            && flags & 1 == 0
            && u16::from_le_bytes(header[8..10].try_into()?) == 0,
        "Invalid stored PKG local ZIP header"
    );
    let name_size = u16::from_le_bytes(header[26..28].try_into()?) as u64;
    let extra_size = u16::from_le_bytes(header[28..30].try_into()?) as u64;
    let start =
        header_start.checked_add(30 + name_size + extra_size).context("PKG ZIP extent overflow")?;
    ensure!(
        start == data_start && start <= directory_start && size <= directory_start - start,
        "PKG overlaps the ZIP directory"
    );
    let name = read_at(&mut source, header_start + 30, name_size, directory_start)?;
    ensure!(name == b"application.pkg", "PKG local ZIP filename does not match directory");
    xar_gzip_ranges(&mut source, start, size)
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{
        write::{GzEncoder, ZlibEncoder},
        Compression,
    };
    use std::fs;
    use std::io::{Cursor, Write};
    use zip::{write::SimpleFileOptions, ZipWriter};

    fn pkg(payload: &[u8], offset: u64, duplicate: bool) -> Vec<u8> {
        let member = format!("<file><name>Payload</name><data><offset>{offset}</offset><length>{}</length><encoding style=\"application/octet-stream\"/></data></file>", payload.len());
        let xml = format!(
            "<xar><toc><file><name>component.pkg</name>{member}{}</file></toc></xar>",
            if duplicate { &member } else { "" }
        );
        pkg_xml(xml.as_bytes(), payload)
    }

    fn pkg_xml(xml: &[u8], payload: &[u8]) -> Vec<u8> {
        let mut encoder = ZlibEncoder::new(Vec::new(), Compression::default());
        encoder.write_all(xml).unwrap();
        let compressed = encoder.finish().unwrap();
        let mut bytes = b"xar!".to_vec();
        bytes.extend_from_slice(&28u16.to_be_bytes());
        bytes.extend_from_slice(&1u16.to_be_bytes());
        bytes.extend_from_slice(&(compressed.len() as u64).to_be_bytes());
        bytes.extend_from_slice(&(xml.len() as u64).to_be_bytes());
        bytes.extend_from_slice(&0u32.to_be_bytes());
        bytes.extend_from_slice(&compressed);
        bytes.extend_from_slice(payload);
        bytes
    }

    fn fixture(path: &Path, name: &str, payload: &[u8], compression: CompressionMethod) {
        let mut zip = ZipWriter::new(Cursor::new(Vec::new()));
        zip.start_file(name, SimpleFileOptions::default().compression_method(compression)).unwrap();
        zip.write_all(payload).unwrap();
        let mut bytes = b"Cr24\x03\0\0\0\x01\0\0\0x".to_vec();
        bytes.extend_from_slice(&zip.finish().unwrap().into_inner());
        fs::write(path, bytes).unwrap();
    }

    fn gzip(data: &[u8]) -> Vec<u8> {
        let mut encoder = GzEncoder::new(Vec::new(), Compression::default());
        encoder.write_all(data).unwrap();
        encoder.finish().unwrap()
    }

    #[test]
    fn locates_absolute_nested_gzip_and_uses_native_zip_for_normal_files() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("payload.crx3");
        let payload = gzip(&[42; 4096]);
        fixture(&path, "application.pkg", &pkg(&payload, 0, false), CompressionMethod::Stored);
        let ranges = nested_gzip_ranges(&path).unwrap();
        assert_eq!(ranges.len(), 1);
        let (offset, length) = ranges[0];
        assert_eq!(&fs::read(&path).unwrap()[offset as usize..(offset + length) as usize], payload);
        fixture(&path, "application.pkg", &pkg(&payload, 0, false), CompressionMethod::Deflated);
        assert!(nested_gzip_ranges(&path).unwrap().is_empty());
        fixture(&path, "payload.bin", &payload, CompressionMethod::Stored);
        assert!(nested_gzip_ranges(&path).unwrap().is_empty());
    }

    #[test]
    fn rejects_overlapping_and_outside_pkg_members() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("payload.crx3");
        for bytes in [pkg(&gzip(b"data"), 0, true), pkg(&gzip(b"data"), u64::MAX, false)] {
            fixture(&path, "application.pkg", &bytes, CompressionMethod::Stored);
            assert!(nested_gzip_ranges(&path).is_err());
        }
    }

    #[test]
    fn rejects_duplicate_pkg_hidden_by_zip_name_map() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("payload.crx3");
        let payload = pkg(&gzip(b"data"), 0, false);
        let mut writer = ZipWriter::new(Cursor::new(Vec::new()));
        for name in ["application.pkg", "application.pkx"] {
            writer
                .start_file(
                    name,
                    SimpleFileOptions::default().compression_method(CompressionMethod::Stored),
                )
                .unwrap();
            writer.write_all(&payload).unwrap();
        }
        let mut bytes = b"Cr24\x03\0\0\0\x01\0\0\0x".to_vec();
        bytes.extend_from_slice(&writer.finish().unwrap().into_inner());
        for index in 0..bytes.len() - 14 {
            if &bytes[index..index + 15] == b"application.pkx" {
                bytes[index + 14] = b'g';
            }
        }
        fs::write(&path, bytes).unwrap();
        assert!(nested_gzip_ranges(&path)
            .unwrap_err()
            .to_string()
            .contains("Duplicate application.pkg"));
    }

    #[test]
    fn rejects_truncated_gzip_and_xml_entities() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("payload.crx3");
        fixture(
            &path,
            "application.pkg",
            &pkg(b"\x1f\x8b\x08short", 0, false),
            CompressionMethod::Stored,
        );
        assert!(nested_gzip_ranges(&path).unwrap_err().to_string().contains("Truncated gzip"));
        fixture(
            &path,
            "application.pkg",
            &pkg_xml(b"<!DOCTYPE xar [<!ENTITY a 'b'>]><xar><toc/></xar>", &[]),
            CompressionMethod::Stored,
        );
        assert!(nested_gzip_ranges(&path).unwrap_err().to_string().contains("entities"));
    }

    #[test]
    fn rejects_oversized_and_mismatched_toc() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("payload.crx3");
        for size in [MAX_TOC_SIZE + 1, 1] {
            let mut bytes = pkg(&gzip(b"data"), 0, false);
            bytes[16..24].copy_from_slice(&size.to_be_bytes());
            fixture(&path, "application.pkg", &bytes, CompressionMethod::Stored);
            assert!(nested_gzip_ranges(&path).is_err());
        }
    }

    #[test]
    #[ignore = "Set XENON_PUFFIN_TOOL and run --ignored to execute native Puffin"]
    fn native_embedded_gzip_roundtrip() {
        let dir = tempfile::tempdir().unwrap();
        let old = dir.path().join("old.crx3");
        let new = dir.path().join("new.crx3");
        let shared: Vec<_> = (0..65536u32).flat_map(u32::to_le_bytes).collect();
        fixture(&old, "application.pkg", &pkg(&gzip(&shared), 0, false), CompressionMethod::Stored);
        let mut updated = shared;
        updated[4096..4103].copy_from_slice(b"updated");
        fixture(
            &new,
            "application.pkg",
            &pkg(&gzip(&updated), 0, false),
            CompressionMethod::Stored,
        );
        let options = crate::delta::DeltaOptions {
            old_crx: old,
            new_crx: new,
            from_version: "1.0.0.1".into(),
            to_version: "1.0.0.2".into(),
            puffin: std::env::var_os("XENON_PUFFIN_TOOL").expect("Set XENON_PUFFIN_TOOL").into(),
            out: dir.path().join("update.puff"),
            metadata: None,
            opaque: false,
        };
        let result = crate::delta::generate(&options).unwrap();
        assert_eq!(result["roundtrip_verified"], true);
        assert_eq!(result["compression"]["source_gzip_streams"], 1);
        assert_eq!(result["compression"]["target_gzip_streams"], 1);
        assert_eq!(result["recommended"], true);
    }
}
