// Copyright 2026 The Xenon Overlay Authors. All rights reserved.

use anyhow::{bail, ensure, Context, Result};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::ffi::OsString;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use tempfile::{NamedTempFile, TempDir};

#[derive(Debug, Clone)]
pub struct DeltaOptions {
    pub old_crx: PathBuf,
    pub new_crx: PathBuf,
    pub from_version: String,
    pub to_version: String,
    pub puffin: PathBuf,
    pub out: PathBuf,
    pub metadata: Option<PathBuf>,
    pub opaque: bool,
}

fn version(value: &str) -> Result<[u32; 4]> {
    let mut result = [0; 4];
    let parts: Vec<_> = value.split('.').collect();
    ensure!((1..=4).contains(&parts.len()), "Expected one to four version components");
    for (index, part) in parts.iter().enumerate() {
        ensure!(
            !part.is_empty()
                && part.bytes().all(|c| c.is_ascii_digit())
                && (part.len() == 1 || !part.starts_with('0')),
            "Invalid version: {value}"
        );
        result[index] = part.parse().context("Version component exceeds uint32")?;
    }
    Ok(result)
}

pub fn sha256(path: &Path) -> Result<String> {
    let mut file = File::open(path).with_context(|| format!("Opening {}", path.display()))?;
    let mut hash = Sha256::new();
    let mut buffer = vec![0; 1024 * 1024];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            return Ok(hex::encode(hash.finalize()));
        }
        hash.update(&buffer[..count]);
    }
}

fn check_crx(path: &Path) -> Result<()> {
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file() && !metadata.file_type().is_symlink(),
        "Delta inputs must be regular CRX3 files: {}",
        path.display()
    );
    let mut file = File::open(path)?;
    let mut header = [0; 12];
    file.read_exact(&mut header).context("Truncated CRX3 header")?;
    ensure!(
        &header[..4] == b"Cr24" && u32::from_le_bytes(header[4..8].try_into()?) == 3,
        "Delta inputs must use the CRX3 format"
    );
    let header_size = u32::from_le_bytes(header[8..12].try_into()?) as u64;
    ensure!(
        header_size > 0 && header_size <= 16 * 1024 * 1024 && header_size + 12 < metadata.len(),
        "Invalid CRX3 input header length"
    );
    Ok(())
}

fn equal_files(left: &Path, right: &Path) -> Result<bool> {
    if fs::metadata(left)?.len() != fs::metadata(right)?.len() {
        return Ok(false);
    }
    let mut a = File::open(left)?;
    let mut b = File::open(right)?;
    let mut aa = vec![0; 1024 * 1024];
    let mut bb = vec![0; 1024 * 1024];
    loop {
        let count = a.read(&mut aa)?;
        if count == 0 {
            return Ok(true);
        }
        b.read_exact(&mut bb[..count])?;
        if aa[..count] != bb[..count] {
            return Ok(false);
        }
    }
}

// Resolve existing ancestors so aliases through parent-directory symlinks are
// detected even before a destination file or directory exists.
fn resolved_output(path: &Path) -> Result<PathBuf> {
    let absolute = std::path::absolute(path)?;
    if absolute.exists() {
        let metadata = fs::symlink_metadata(&absolute)?;
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "Output must be a regular file: {}",
            path.display()
        );
        return Ok(fs::canonicalize(absolute)?);
    }
    let mut ancestor = absolute.as_path();
    let mut missing = Vec::new();
    while !ancestor.exists() {
        missing.push(ancestor.file_name().context("Output has no filename")?.to_owned());
        ancestor = ancestor.parent().context("Output has no existing ancestor")?;
    }
    let mut resolved = fs::canonicalize(ancestor)?;
    for component in missing.iter().rev() {
        resolved.push(component);
    }
    Ok(resolved)
}

fn stage_file(source: &Path, output: &Path) -> Result<NamedTempFile> {
    let parent = output.parent().context("Output has no parent directory")?;
    fs::create_dir_all(parent)?;
    let mut staged = NamedTempFile::new_in(parent)?;
    std::io::copy(&mut File::open(source)?, &mut staged)?;
    staged.flush()?;
    staged.as_file().sync_all()?;
    Ok(staged)
}

fn run_puffin(tool: &Path, arguments: &[OsString], cwd: &Path) -> Result<()> {
    let output = Command::new(tool)
        .args(arguments)
        .current_dir(cwd)
        .output()
        .with_context(|| format!("Starting Puffin {}", tool.display()))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let detail: String =
            stderr.chars().rev().take(16 * 1024).collect::<String>().chars().rev().collect();
        bail!("Puffin failed ({}): {}", output.status, detail.trim());
    }
    Ok(())
}

/// Invoke the existing native Puffin executable, independently apply the patch,
/// and publish only verified bytes. No Python process or signing key is needed.
pub fn generate(options: &DeltaOptions) -> Result<Value> {
    generate_with(options, run_puffin)
}

fn generate_with(
    options: &DeltaOptions,
    mut run: impl FnMut(&Path, &[OsString], &Path) -> Result<()>,
) -> Result<Value> {
    ensure!(
        version(&options.to_version)? > version(&options.from_version)?,
        "Delta target version must be newer than its baseline"
    );
    check_crx(&options.old_crx)?;
    check_crx(&options.new_crx)?;
    let old = fs::canonicalize(&options.old_crx)?;
    let new = fs::canonicalize(&options.new_crx)?;
    let output = resolved_output(&options.out)?;
    ensure!(output != old && output != new, "Output must not replace an input CRX");
    let metadata_output = options.metadata.as_deref().map(resolved_output).transpose()?;
    if let Some(metadata) = &metadata_output {
        ensure!(
            metadata != &old && metadata != &new && metadata != &output,
            "Metadata output must not replace a CRX or patch"
        );
    }
    let tool = fs::canonicalize(&options.puffin)
        .with_context(|| format!("Puffin executable not found: {}", options.puffin.display()))?;
    let temp = TempDir::with_prefix("xenon-updater-delta-")?;
    let baseline = temp.path().join("old.crx3");
    let target = temp.path().join("new.crx3");
    let patch = temp.path().join("update.puff");
    let restored = temp.path().join("restored.crx3");
    let old_hash = sha256(&old)?;
    let new_hash = sha256(&new)?;
    fs::copy(&old, &baseline)?;
    fs::copy(&new, &target)?;
    check_crx(&baseline)?;
    check_crx(&target)?;
    ensure!(
        sha256(&baseline)? == old_hash && sha256(&target)? == new_hash,
        "An input CRX changed while preparing the delta"
    );
    let (old_ranges, new_ranges) = if options.opaque {
        (Vec::new(), Vec::new())
    } else {
        (crate::ranges::nested_gzip_ranges(&baseline)?, crate::ranges::nested_gzip_ranges(&target)?)
    };
    let use_gzip = !old_ranges.is_empty() || !new_ranges.is_empty();
    let mut args: Vec<OsString> = vec![
        if use_gzip { "-puffdiff-gzip" } else { "-puffdiff" }.into(),
        baseline.as_os_str().to_owned(),
        target.as_os_str().to_owned(),
        patch.as_os_str().to_owned(),
    ];
    if use_gzip {
        for ranges in [&old_ranges, &new_ranges] {
            args.push(
                if ranges.is_empty() {
                    "none".to_string()
                } else {
                    ranges
                        .iter()
                        .map(|(start, len)| format!("{start}:{len}"))
                        .collect::<Vec<_>>()
                        .join(",")
                }
                .into(),
            );
        }
    }
    run(&tool, &args, temp.path())?;
    let patch_info = fs::symlink_metadata(&patch).context("Puffin did not produce a patch")?;
    ensure!(
        patch_info.is_file() && !patch_info.file_type().is_symlink() && patch_info.len() > 4,
        "Puffin did not produce a regular, nonempty patch"
    );
    let mut magic = [0; 4];
    File::open(&patch)?.read_exact(&mut magic)?;
    ensure!(&magic == b"PUF1", "Puffin produced an invalid patch container");
    run(
        &tool,
        &[
            "-puffpatch".into(),
            baseline.as_os_str().to_owned(),
            restored.as_os_str().to_owned(),
            patch.as_os_str().to_owned(),
        ],
        temp.path(),
    )?;
    ensure!(
        restored.is_file() && sha256(&restored)? == new_hash && equal_files(&restored, &target)?,
        "Puffin round-trip did not reproduce the target CRX byte for byte"
    );
    ensure!(
        sha256(&old)? == old_hash && sha256(&new)? == new_hash,
        "An input CRX changed during delta generation"
    );
    let full_size = fs::metadata(&target)?.len();
    let smaller = patch_info.len() < full_size;
    let metadata = json!({
        "from_version": options.from_version, "to_version": options.to_version,
        "previous_sha256": old_hash, "type": "puff",
        "file": output.file_name().and_then(|name| name.to_str()).context("Patch filename is not UTF-8")?,
        "size": patch_info.len(), "sha256": sha256(&patch)?,
        "full_file": new.file_name().and_then(|name| name.to_str()).context("CRX filename is not UTF-8")?,
        "full_size": full_size, "full_sha256": new_hash,
        "roundtrip_verified": true,
        "compression": { "mode": if options.opaque { "opaque" } else { "auto" },
            "source_gzip_streams": old_ranges.len(), "target_gzip_streams": new_ranges.len() },
        "recommended": smaller, "default_payload": if smaller { "delta" } else { "full" },
    });
    // Prepare every output before replacing either public filename, so all
    // computation, verification and ordinary staging failures leave them alone.
    let prepared_patch = stage_file(&patch, &output)?;
    let prepared_metadata = if let Some(destination) = &metadata_output {
        let temporary = temp.path().join("metadata.json");
        fs::write(&temporary, format!("{}\n", serde_json::to_string_pretty(&metadata)?))?;
        Some(stage_file(&temporary, destination)?)
    } else {
        None
    };
    prepared_patch.persist(&output).context("Publishing verified patch")?;
    if let (Some(prepared), Some(destination)) = (prepared_metadata, metadata_output) {
        prepared.persist(destination).context("Publishing patch metadata")?;
    }
    Ok(metadata)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;
    use zip::{write::SimpleFileOptions, ZipWriter};

    pub(crate) fn crx_fixture(path: &Path, name: &str, contents: &[u8]) {
        let mut zip = ZipWriter::new(Cursor::new(Vec::new()));
        zip.start_file(
            name,
            SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored),
        )
        .unwrap();
        zip.write_all(contents).unwrap();
        let payload = zip.finish().unwrap().into_inner();
        let mut bytes = b"Cr24\x03\0\0\0\x01\0\0\0x".to_vec();
        bytes.extend_from_slice(&payload);
        fs::write(path, bytes).unwrap();
    }

    fn setup() -> (TempDir, DeltaOptions) {
        let dir = tempfile::tempdir().unwrap();
        let args = DeltaOptions {
            old_crx: dir.path().join("old.crx3"),
            new_crx: dir.path().join("new.crx3"),
            from_version: "1.0.0.1".into(),
            to_version: "1.0.0.2".into(),
            puffin: std::env::current_exe().unwrap(),
            out: dir.path().join("delta.puff"),
            metadata: Some(dir.path().join("delta.json")),
            opaque: false,
        };
        crx_fixture(&args.old_crx, "data.bin", &[1; 4096]);
        crx_fixture(&args.new_crx, "data.bin", &[2; 4096]);
        (dir, args)
    }

    fn fake_success(_: &Path, args: &[OsString], cwd: &Path) -> Result<()> {
        if args[0] == "-puffpatch" {
            fs::copy(cwd.join("new.crx3"), Path::new(&args[2]))?;
        } else {
            fs::write(Path::new(&args[3]), b"PUF1verified-test-patch")?;
        }
        Ok(())
    }

    #[test]
    fn publishes_only_roundtrip_verified_patch_and_metadata() {
        let (_dir, args) = setup();
        let result = generate_with(&args, fake_success).unwrap();
        assert_eq!(result["roundtrip_verified"], true);
        assert_eq!(result["recommended"], true);
        assert_eq!(result["full_sha256"], sha256(&args.new_crx).unwrap());
        assert_eq!(
            result,
            serde_json::from_slice::<Value>(&fs::read(args.metadata.unwrap()).unwrap()).unwrap()
        );
    }

    #[test]
    fn failed_roundtrip_preserves_existing_outputs() {
        let (_dir, args) = setup();
        fs::write(&args.out, b"approved-patch").unwrap();
        fs::write(args.metadata.as_ref().unwrap(), b"approved-metadata").unwrap();
        let error = generate_with(&args, |_, argv, _| {
            fs::write(Path::new(&argv[if argv[0] == "-puffpatch" { 2 } else { 3 }]), b"PUF1wrong")?;
            Ok(())
        })
        .unwrap_err();
        assert!(error.to_string().contains("round-trip"));
        assert_eq!(fs::read(&args.out).unwrap(), b"approved-patch");
        assert_eq!(fs::read(args.metadata.unwrap()).unwrap(), b"approved-metadata");
    }

    #[test]
    fn detects_input_changed_during_generation() {
        let (_dir, args) = setup();
        let result = generate_with(&args, |tool, argv, cwd| {
            fake_success(tool, argv, cwd)?;
            fs::write(&args.new_crx, b"changed")?;
            Ok(())
        });
        assert!(result.unwrap_err().to_string().contains("changed during"));
        assert!(!args.out.exists());
    }

    #[test]
    fn larger_patch_is_marked_for_full_fallback() {
        let (_dir, args) = setup();
        let result = generate_with(&args, |tool, argv, cwd| {
            fake_success(tool, argv, cwd)?;
            if argv[0] == "-puffdiff" {
                let mut patch = b"PUF1".to_vec();
                patch.extend_from_slice(&[0; 8192]);
                fs::write(Path::new(&argv[3]), patch)?;
            }
            Ok(())
        })
        .unwrap();
        assert_eq!(result["recommended"], false);
        assert_eq!(result["default_payload"], "full");
    }

    #[test]
    fn rejects_input_output_alias_and_invalid_versions() {
        let (_dir, mut args) = setup();
        args.out = args.old_crx.clone();
        assert!(generate_with(&args, fake_success)
            .unwrap_err()
            .to_string()
            .contains("replace an input"));
        args.out = args.old_crx.parent().unwrap().join("patch.puff");
        args.to_version = "1.0.00.2".into();
        assert!(generate_with(&args, fake_success).is_err());
        args.to_version = "1.0.0.1".into();
        assert!(generate_with(&args, fake_success).is_err());
    }

    #[test]
    #[ignore = "Set XENON_PUFFIN_TOOL and run --ignored to execute native Puffin"]
    fn native_puffin_roundtrip() {
        let (_dir, mut args) = setup();
        args.puffin = std::env::var_os("XENON_PUFFIN_TOOL").expect("Set XENON_PUFFIN_TOOL").into();
        let shared: Vec<_> = (0..65536u32).flat_map(u32::to_le_bytes).collect();
        crx_fixture(&args.old_crx, "data.bin", &shared);
        let mut updated = shared;
        updated[8192..8208].copy_from_slice(b"modified payload");
        crx_fixture(&args.new_crx, "data.bin", &updated);
        let result = generate(&args).unwrap();
        assert_eq!(result["roundtrip_verified"], true);
        assert_eq!(result["recommended"], true);
    }
}
