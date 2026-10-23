// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Preserve published CRX bytes and build latest releases from older baselines.
use anyhow::{ensure, Context, Result};
use fs2::FileExt;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, HashSet},
    fs::{self, File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
};

use crate::{catalog, delta, publish};

const MAX_INDEX_BYTES: u64 = 4 * 1024 * 1024;
const MAX_HISTORY_ENTRIES: usize = 4096;
const MAX_DELTAS: usize = 32;
type Route = (String, String, String);

pub struct ImportOptions {
    pub history: PathBuf,
    pub manifest: PathBuf,
    pub release: PathBuf,
    pub puffin: Option<PathBuf>,
}

fn text<'a>(value: &'a Value, key: &str) -> Result<&'a str> {
    value[key].as_str().with_context(|| format!("Missing history field {key}"))
}

fn route(value: &Value) -> Result<Route> {
    Ok((
        text(value, "appid")?.to_ascii_lowercase(),
        text(value, "platform")?.to_owned(),
        text(value, "arch")?.to_owned(),
    ))
}

fn normalized_version(value: &Value) -> Result<String> {
    Ok(catalog::parse_version(text(value, "version")?)?.map(|part| part.to_string()).join("."))
}

fn directory(root: &Path, release: &Value) -> Result<PathBuf> {
    let id = hex::encode(Sha256::digest(serde_json::to_vec(&route(release)?)?));
    Ok(root.join(id).join(normalized_version(release)?))
}

fn document(release: &Value) -> Value {
    let mut value = release.clone();
    value["schema_version"] = json!(1);
    value
}

fn releases(path: &Path) -> Result<Vec<Value>> {
    // Parse the exact bytes already validated, avoiding a second path lookup.
    let loaded = catalog::load(path)?;
    let mut value: Value = serde_json::from_slice(&loaded.manifest_bytes)?;
    if let Some(apps) = value.get("apps") {
        return Ok(apps.as_array().context("Invalid apps array")?.clone());
    }
    value.as_object_mut().context("Invalid release")?.remove("schema_version");
    Ok(vec![value])
}

fn real_directory(path: &Path) -> Result<()> {
    fs::create_dir_all(path)?;
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_dir() && !metadata.file_type().is_symlink(),
        "History directory must not be a link: {}",
        path.display()
    );
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        ensure!(
            metadata.file_attributes() & 0x400 == 0,
            "History directory must not be a reparse point: {}",
            path.display()
        );
    }
    Ok(())
}

fn regular(path: &Path, limit: u64) -> Result<()> {
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file() && !metadata.file_type().is_symlink() && metadata.len() <= limit,
        "Expected regular history file: {}",
        path.display()
    );
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        ensure!(metadata.file_attributes() & 0x400 == 0, "History file is a reparse point");
    }
    Ok(())
}

fn write_json(path: &Path, value: &Value) -> Result<()> {
    if path.try_exists()? {
        regular(path, MAX_INDEX_BYTES)?;
    }
    let mut file = tempfile::NamedTempFile::new_in(path.parent().context("Missing parent")?)?;
    serde_json::to_writer_pretty(&mut file, value)?;
    file.write_all(b"\n")?;
    file.as_file().sync_all()?;
    file.persist(path)?;
    Ok(())
}

fn read_index(root: &Path) -> Result<Vec<Value>> {
    let path = root.join("index.json");
    if !path.try_exists()? {
        return Ok(vec![]);
    }
    regular(&path, MAX_INDEX_BYTES)?;
    let value: Value = serde_json::from_slice(&fs::read(path)?)?;
    let object = value.as_object().context("Invalid history index")?;
    ensure!(object.len() == 2 && value["schema_version"] == 1, "Unsupported history index");
    let entries = value["entries"].as_array().context("Missing history entries")?.clone();
    ensure!(entries.len() <= MAX_HISTORY_ENTRIES, "History entry limit exceeded");
    let mut identities = HashSet::new();
    for entry in &entries {
        catalog::validate_manifest(&document(entry))?;
        ensure!(
            entry.get("differential").is_none(),
            "Index must not contain differential metadata"
        );
        ensure!(
            identities.insert((route(entry)?, normalized_version(entry)?)),
            "Duplicate history version"
        );
    }
    Ok(entries)
}

fn summary(release: &Value) -> Value {
    json!({"appid":release["appid"],"platform":release["platform"],"arch":release["arch"],
        "version":release["version"],"full_size":release["full"]["size"],
        "sha256":release["full"]["sha256"]})
}

/// List history metadata without disclosing local filesystem paths.
pub fn list(history: &Path) -> Result<Value> {
    let entries = read_index(history)?;
    let mut summaries: Vec<_> = entries.iter().map(summary).collect();
    summaries.sort_by_key(|entry| {
        (
            route(entry).unwrap(),
            std::cmp::Reverse(catalog::parse_version(entry["version"].as_str().unwrap()).unwrap()),
        )
    });
    Ok(json!(summaries))
}

fn same_release(first: &Value, second: &Value) -> Result<()> {
    ensure!(
        first["full"]["sha256"] == second["full"]["sha256"]
            && first["full"]["size"] == second["full"]["size"],
        "Refusing different CRX bytes for already archived version {}",
        text(second, "version")?
    );
    Ok(())
}

fn copy_verified(source: &Path, target: &Path, spec: &Value) -> Result<()> {
    regular(source, u64::MAX)?;
    let size = spec["size"].as_u64().context("Invalid artifact size")?;
    ensure!(
        fs::copy(source, target)? == size && delta::sha256(target)? == text(spec, "sha256")?,
        "History source artifact changed during copy"
    );
    OpenOptions::new().write(true).open(target)?.sync_all()?;
    Ok(())
}

fn archive(root: &Path, source_root: &Path, incoming: &Value) -> Result<Value> {
    let target = directory(root, incoming)?;
    let route_dir = target.parent().unwrap();
    real_directory(route_dir)?;
    if target.try_exists()? {
        real_directory(&target)?;
        let existing = releases(&target.join("release.json"))?.remove(0);
        same_release(&existing, incoming)?;
        return Ok(existing);
    }
    let stage = tempfile::tempdir_in(route_dir)?;
    let mut release = incoming.clone();
    copy_verified(
        &source_root.join(text(&incoming["full"], "file")?),
        &stage.path().join("full.crx3"),
        &incoming["full"],
    )?;
    release["full"]["file"] = json!("full.crx3");
    if let Some(diffs) = release.get_mut("differential").and_then(Value::as_array_mut) {
        for diff in diffs {
            let name = format!(
                "delta-{}-{}.{}",
                text(diff, "from_version")?,
                text(diff, "sha256")?,
                text(diff, "type")?
            );
            copy_verified(&source_root.join(text(diff, "file")?), &stage.path().join(&name), diff)?;
            diff["file"] = json!(name);
        }
    }
    write_json(&stage.path().join("release.json"), &document(&release))?;
    catalog::load(&stage.path().join("release.json"))?;
    fs::rename(stage.path(), &target)?;
    Ok(release)
}

fn add_entry(entries: &mut Vec<Value>, release: Value) -> Result<()> {
    let key = (route(&release)?, normalized_version(&release)?);
    if let Some(existing) = entries
        .iter()
        .find(|entry| route(entry).ok().zip(normalized_version(entry).ok()).as_ref() == Some(&key))
    {
        same_release(existing, &release)?;
        return Ok(());
    }
    ensure!(entries.len() < MAX_HISTORY_ENTRIES, "History entry limit exceeded");
    let mut release = release;
    release.as_object_mut().unwrap().remove("differential");
    entries.push(release);
    Ok(())
}

fn matching_diff<'a>(latest: &'a Value, baseline: &Value) -> Option<&'a Value> {
    latest["differential"].as_array()?.iter().find(|diff| {
        diff["previous_sha256"] == baseline["full"]["sha256"]
            && diff["from_version"].as_str().and_then(|v| catalog::parse_version(v).ok())
                == baseline["version"].as_str().and_then(|v| catalog::parse_version(v).ok())
            && diff["size"].as_u64() < latest["full"]["size"].as_u64()
    })
}

fn generate_delta(
    root: &Path,
    latest: &Value,
    old: &Value,
    tool: &Path,
    generate: &mut impl FnMut(&delta::DeltaOptions) -> Result<Value>,
) -> Result<Value> {
    let target = directory(root, latest)?;
    let stage = tempfile::tempdir_in(&target)?;
    let patch = stage.path().join("update.puff");
    let metadata = generate(&delta::DeltaOptions {
        old_crx: directory(root, old)?.join(text(&old["full"], "file")?),
        new_crx: target.join(text(&latest["full"], "file")?),
        from_version: text(old, "version")?.to_owned(),
        to_version: text(latest, "version")?.to_owned(),
        puffin: tool.to_owned(),
        out: patch.clone(),
        metadata: None,
        opaque: false,
    })?;
    ensure!(
        metadata["roundtrip_verified"] == true && metadata["recommended"] == true,
        "No smaller round-trip-verified patch; using full CRX for this baseline"
    );
    ensure!(
        metadata["previous_sha256"] == old["full"]["sha256"]
            && metadata["full_sha256"] == latest["full"]["sha256"],
        "Generated patch identity mismatch"
    );
    let mut diff = json!({});
    for key in ["from_version", "previous_sha256", "type", "file", "size", "sha256"] {
        diff[key] = metadata[key].clone();
    }
    ensure!(
        diff["size"].as_u64().context("Invalid patch size")?
            < latest["full"]["size"].as_u64().unwrap(),
        "Patch is not smaller than full CRX"
    );
    let name = format!("delta-{}-{}.puff", text(&diff, "from_version")?, text(&diff, "sha256")?);
    // No existing byte content is overwritten, including abandoned earlier work.
    let destination = target.join(&name);
    if destination.try_exists()? {
        regular(&destination, u64::MAX)?;
        ensure!(
            delta::sha256(&destination)? == text(&diff, "sha256")?,
            "Immutable patch collision"
        );
    } else {
        let mut staged = tempfile::NamedTempFile::new_in(&target)?;
        std::io::copy(&mut File::open(&patch)?, &mut staged)?;
        ensure!(delta::sha256(staged.path())? == text(&diff, "sha256")?, "Generated patch changed");
        staged.as_file().sync_all()?;
        staged.persist_noclobber(&destination)?;
    }
    diff["file"] = json!(name);
    Ok(diff)
}

pub fn import_release(options: &ImportOptions) -> Result<Value> {
    import_with(options, delta::generate)
}

fn import_with(
    options: &ImportOptions,
    mut generate: impl FnMut(&delta::DeltaOptions) -> Result<Value>,
) -> Result<Value> {
    real_directory(&options.history)?;
    let root = options.history.canonicalize()?;
    let lock_path = root.join("history.lock");
    if lock_path.try_exists()? {
        regular(&lock_path, 1024)?;
    }
    let lock =
        OpenOptions::new().create(true).truncate(false).read(true).write(true).open(lock_path)?;
    lock.try_lock_exclusive().context("Another installer import is updating history")?;
    let mut entries = read_index(&root)?;
    let incoming_path = std::path::absolute(&options.release)?;
    let incoming = releases(&incoming_path)?;
    ensure!(!incoming.is_empty(), "Input contains no release");
    let current_path = std::path::absolute(&options.manifest)?;
    let current = if current_path.try_exists()? { releases(&current_path)? } else { vec![] };
    // Reject same-version replacement before archiving any of this import.
    let mut combined = entries.clone();
    for release in current.iter().chain(incoming.iter()) {
        add_entry(&mut combined, release.clone())?;
    }
    for release in &current {
        add_entry(&mut entries, archive(&root, current_path.parent().unwrap(), release)?)?;
    }
    for release in &incoming {
        add_entry(&mut entries, archive(&root, incoming_path.parent().unwrap(), release)?)?;
    }
    entries.sort_by_key(|entry| {
        (route(entry).unwrap(), catalog::parse_version(text(entry, "version").unwrap()).unwrap())
    });
    // History is durable even if publication is interrupted: a retry reuses
    // the exact original CRX rather than requiring an installer repackage.
    write_json(&root.join("index.json"), &json!({"schema_version":1,"entries":entries}))?;
    let mut grouped: BTreeMap<Route, Vec<&Value>> = BTreeMap::new();
    for entry in &entries {
        grouped.entry(route(entry)?).or_default().push(entry);
    }
    let affected: HashSet<_> = incoming.iter().map(route).collect::<Result<_>>()?;
    let mut warnings = vec![];
    let mut generated = 0;
    let prepared = tempfile::tempdir_in(&root)?;
    let mut targets = Vec::new();
    for (key, versions) in grouped {
        if !affected.contains(&key) {
            continue;
        }
        let newest = versions.last().unwrap();
        let target_dir = directory(&root, newest)?;
        let mut latest = releases(&target_dir.join("release.json"))?.remove(0);
        let bases: Vec<_> = versions.iter().rev().skip(1).take(MAX_DELTAS).collect();
        if versions.len() > MAX_DELTAS + 1 {
            warnings.push(format!(
                "{} {} {}: only the newest {MAX_DELTAS} lower versions receive deltas",
                key.0, key.1, key.2
            ));
        }
        let mut selected = Vec::new();
        for old in bases {
            if let Some(existing) = matching_diff(&latest, old) {
                selected.push(existing.clone());
                continue;
            }
            let result = match options.puffin.as_deref() {
                Some(tool) => generate_delta(&root, &latest, old, tool, &mut generate),
                None => {
                    Err(anyhow::anyhow!("Puffin was not found; using full CRX for this baseline"))
                }
            };
            match result {
                Ok(diff) => {
                    selected.push(diff);
                    generated += 1;
                }
                Err(error) => warnings.push(format!(
                    "{} {} {} {} → {}: {error:#}",
                    key.0,
                    key.1,
                    key.2,
                    text(old, "version")?,
                    text(&latest, "version")?
                )),
            }
        }
        // Preserve supplied deltas whose baselines are not in local history.
        // They were verified as artifacts when the source release was loaded.
        if let Some(existing) = latest["differential"].as_array() {
            for diff in existing {
                if selected.len() >= MAX_DELTAS {
                    break;
                }
                if diff["size"].as_u64() >= latest["full"]["size"].as_u64() {
                    continue;
                }
                if !selected.iter().any(|d| {
                    d["previous_sha256"] == diff["previous_sha256"]
                        && d["from_version"] == diff["from_version"]
                }) {
                    selected.push(diff.clone());
                }
            }
        }
        latest["differential"] = json!(selected);
        let temporary_manifest = target_dir.join("prepared.json");
        write_json(&temporary_manifest, &document(&latest))?;
        catalog::load(&temporary_manifest)?;
        write_json(&target_dir.join("release.json"), &document(&latest))?;
        let _ = fs::remove_file(temporary_manifest);
        // Each route uses unique staged names so a multi-platform catalog can
        // be passed to the existing atomic publisher in one transaction.
        let prefix = hex::encode(Sha256::digest(serde_json::to_vec(&key)?));
        let name = format!("{prefix}-full.crx3");
        copy_verified(
            &target_dir.join(text(&latest["full"], "file")?),
            &prepared.path().join(&name),
            &latest["full"],
        )?;
        latest["full"]["file"] = json!(name);
        if let Some(diffs) = latest["differential"].as_array_mut() {
            for (i, diff) in diffs.iter_mut().enumerate() {
                let name = format!("{prefix}-delta-{i}.{}", text(diff, "type")?);
                copy_verified(
                    &target_dir.join(text(diff, "file")?),
                    &prepared.path().join(&name),
                    diff,
                )?;
                diff["file"] = json!(name);
            }
        }
        targets.push(latest);
    }
    let prepared_manifest = prepared.path().join("release.json");
    write_json(&prepared_manifest, &json!({"schema_version":1,"apps":targets}))?;
    let mut result = publish::publish_authoritative(&publish::PublishOptions {
        manifest: options.manifest.clone(),
        release: prepared_manifest,
        puffin: None,
        full_only: false,
    })?;
    if let Some(publish_warnings) = result["warnings"].as_array() {
        warnings.extend(publish_warnings.iter().filter_map(Value::as_str).map(str::to_owned));
    }
    result["generated_deltas"] = json!(generated);
    result["warnings"] = json!(warnings);
    result["history_entries"] = json!(entries.len());
    result["history"] = list(&root)?;
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(root: &Path, name: &str, version: &str, arch: &str) -> PathBuf {
        let dir = root.join(name);
        fs::create_dir_all(&dir).unwrap();
        let mut bytes = b"Cr24\x03\0\0\0\x01\0\0\0x".to_vec();
        bytes.extend(vec![version.as_bytes()[0]; 2048]);
        let hash = hex::encode(Sha256::digest(&bytes));
        fs::write(dir.join("update.crx3"), &bytes).unwrap();
        let value = json!({"schema_version":1,"appid":"org.example.Browser","platform":"mac","arch":arch,"version":version,
            "full":{"file":"update.crx3","size":bytes.len(),"sha256":hash}});
        fs::write(dir.join("release.json"), value.to_string()).unwrap();
        dir.join("release.json")
    }
    fn options(root: &Path, release: PathBuf) -> ImportOptions {
        ImportOptions {
            history: root.join("history"),
            manifest: root.join("served/catalog.json"),
            release,
            puffin: Some(PathBuf::from("mock-puffin")),
        }
    }
    fn mock_delta(options: &delta::DeltaOptions) -> Result<Value> {
        let patch = format!("PUF1roundtrip-fixture-{}", options.from_version);
        fs::write(&options.out, &patch)?;
        Ok(json!({"roundtrip_verified":true,"recommended":true,"from_version":options.from_version,
            "previous_sha256":delta::sha256(&options.old_crx)?,"full_sha256":delta::sha256(&options.new_crx)?,
            "type":"puff","file":"update.puff","size":patch.len(),"sha256":hex::encode(Sha256::digest(patch.as_bytes()))}))
    }

    #[test]
    fn older_import_keeps_latest_and_reuses_verified_patch() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let v2 = fixture(root, "v2", "2.0", "x86_64");
        let v1 = fixture(root, "v1", "1.0", "x86_64");
        import_with(&options(root, v2.clone()), mock_delta).unwrap();
        let result = import_with(&options(root, v1.clone()), mock_delta).unwrap();
        assert_eq!(result["generated_deltas"], 1);
        assert_eq!(result["published"][0]["version"], "2.0");
        let result = import_with(&options(root, v1), |_| panic!("Must reuse patch")).unwrap();
        assert_eq!(result["generated_deltas"], 0);
        assert_eq!(result["history_entries"], 2);
        assert_eq!(
            releases(&options(root, v2).manifest).unwrap()[0]["differential"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
    }

    #[test]
    fn all_lower_versions_get_patches_and_architectures_stay_separate() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        for (name, version, arch) in
            [("x64_1", "1.0", "x86_64"), ("arm64_1", "1.0", "arm64"), ("x64_2", "2.0", "x86_64")]
        {
            import_with(&options(root, fixture(root, name, version, arch)), mock_delta).unwrap();
        }
        let result =
            import_with(&options(root, fixture(root, "x64_3", "3.0", "x86_64")), mock_delta)
                .unwrap();
        assert_eq!(result["generated_deltas"], 2);
        let published = releases(&root.join("served/catalog.json")).unwrap();
        assert_eq!(published.len(), 2);
        let x64 = published.iter().find(|r| r["arch"] == "x86_64").unwrap();
        let arm64 = published.iter().find(|r| r["arch"] == "arm64").unwrap();
        assert_eq!(x64["version"], "3.0");
        assert_eq!(x64["differential"].as_array().unwrap().len(), 2);
        assert_eq!(arm64["version"], "1.0");
        assert!(list(&root.join("history"))
            .unwrap()
            .as_array()
            .unwrap()
            .iter()
            .all(|v| v.get("path").is_none()));
    }

    #[test]
    fn same_version_repackage_is_rejected_before_index_changes() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let original = fixture(root, "original", "2.0", "x86_64");
        import_with(&options(root, original), mock_delta).unwrap();
        let old_index = fs::read(root.join("history/index.json")).unwrap();
        let changed = fixture(root, "changed", "2.0.0.0", "x86_64");
        let path = changed.parent().unwrap().join("update.crx3");
        let mut bytes = fs::read(&path).unwrap();
        bytes[40] ^= 1;
        fs::write(&path, &bytes).unwrap();
        let mut value: Value = serde_json::from_slice(&fs::read(&changed).unwrap()).unwrap();
        value["full"]["sha256"] = json!(hex::encode(Sha256::digest(bytes)));
        fs::write(&changed, value.to_string()).unwrap();
        assert!(import_with(&options(root, changed), mock_delta).is_err());
        assert_eq!(fs::read(root.join("history/index.json")).unwrap(), old_index);
    }

    #[test]
    fn failed_generation_keeps_full_update_and_original_baseline() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let v1 = fixture(root, "v1", "1.0", "x86_64");
        import_with(&options(root, v1.clone()), mock_delta).unwrap();
        let result = import_with(&options(root, fixture(root, "v2", "2.0", "x86_64")), |_| {
            anyhow::bail!("generation failed")
        })
        .unwrap();
        assert_eq!(result["generated_deltas"], 0);
        assert!(!result["warnings"].as_array().unwrap().is_empty());
        let entries = read_index(&root.join("history")).unwrap();
        let first = entries.iter().find(|e| e["version"] == "1.0").unwrap();
        assert_eq!(
            fs::read(directory(&root.join("history"), first).unwrap().join("full.crx3")).unwrap(),
            fs::read(v1.parent().unwrap().join("update.crx3")).unwrap()
        );
        assert_eq!(releases(&root.join("served/catalog.json")).unwrap()[0]["version"], "2.0");
    }

    #[test]
    fn existing_catalog_bootstraps_history() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let old = fixture(root, "v1", "1.0", "x86_64");
        publish::publish(&publish::PublishOptions {
            manifest: root.join("served/catalog.json"),
            release: old,
            puffin: None,
            full_only: true,
        })
        .unwrap();
        let result =
            import_with(&options(root, fixture(root, "v2", "2.0", "x86_64")), mock_delta).unwrap();
        assert_eq!(result["history_entries"], 2);
        assert_eq!(result["generated_deltas"], 1);
    }

    #[test]
    fn bounded_differentials_replace_oldest_without_remerging_it() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        import_with(&options(root, fixture(root, "latest", "40.0", "x86_64")), mock_delta).unwrap();
        for major in 1..=33 {
            let label = format!("v{major}");
            let version = format!("{major}.0");
            let result =
                import_with(&options(root, fixture(root, &label, &version, "x86_64")), mock_delta)
                    .unwrap();
            assert_eq!(result["published"][0]["version"], "40.0");
            if major == 33 {
                assert!(!result["warnings"].as_array().unwrap().is_empty());
            }
        }
        let latest = releases(&root.join("served/catalog.json")).unwrap().remove(0);
        let patches = latest["differential"].as_array().unwrap();
        assert_eq!(patches.len(), MAX_DELTAS);
        assert!(patches.iter().any(|diff| diff["from_version"] == "33.0"));
        assert!(!patches.iter().any(|diff| diff["from_version"] == "1.0"));
        assert_eq!(read_index(&root.join("history")).unwrap().len(), 34);
    }

    #[test]
    #[ignore = "Set XENON_PUFFIN_TOOL and run --ignored to execute native Puffin"]
    fn native_history_generates_multiple_older_baselines() {
        let tool = PathBuf::from(std::env::var_os("XENON_PUFFIN_TOOL").expect("XENON_PUFFIN_TOOL"));
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        for major in [3, 1, 2] {
            let label = format!("native{major}");
            let release_path = fixture(root, &label, &format!("{major}.0"), "x86_64");
            let crx = release_path.parent().unwrap().join("update.crx3");
            let mut payload: Vec<_> = (0..65536).map(|n| ((n * 37) % 251) as u8).collect();
            payload[major * 512] = major as u8;
            let mut zip = zip::ZipWriter::new(std::io::Cursor::new(Vec::new()));
            zip.start_file(
                "payload.bin",
                zip::write::SimpleFileOptions::default()
                    .compression_method(zip::CompressionMethod::Stored),
            )
            .unwrap();
            zip.write_all(&payload).unwrap();
            let mut bytes = b"Cr24\x03\0\0\0\x01\0\0\0x".to_vec();
            bytes.extend(zip.finish().unwrap().into_inner());
            fs::write(&crx, bytes).unwrap();
            let mut release: Value =
                serde_json::from_slice(&fs::read(&release_path).unwrap()).unwrap();
            release["full"]["size"] = json!(fs::metadata(&crx).unwrap().len());
            release["full"]["sha256"] = json!(delta::sha256(&crx).unwrap());
            fs::write(&release_path, release.to_string()).unwrap();
            let mut opts = options(root, release_path);
            opts.puffin = Some(tool.clone());
            let result = import_release(&opts).unwrap();
            assert_eq!(result["published"][0]["version"], "3.0");
            assert_eq!(result["generated_deltas"], if major == 3 { 0 } else { 1 });
            assert!(result["warnings"].as_array().unwrap().is_empty());
        }
        let latest = releases(&root.join("served/catalog.json")).unwrap().remove(0);
        assert_eq!(latest["differential"].as_array().unwrap().len(), 2);
    }

    #[test]
    fn malformed_index_never_resolves_caller_supplied_paths() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        fs::write(
            root.join("index.json"),
            r#"{"schema_version":1,"entries":[{"path":"../../secret"}]}"#,
        )
        .unwrap();
        assert!(list(root).is_err());
    }
}
