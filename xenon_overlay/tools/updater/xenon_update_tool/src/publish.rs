// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Import prepared releases and atomically advance the served catalog.

use std::{
    collections::HashSet,
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
};

use anyhow::{bail, ensure, Context, Result};
use fs2::FileExt;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::{catalog, delta};

pub struct PublishOptions {
    pub manifest: PathBuf,
    pub release: PathBuf,
    pub puffin: Option<PathBuf>,
    pub full_only: bool,
}

fn text<'a>(value: &'a Value, key: &str) -> Result<&'a str> {
    value[key].as_str().with_context(|| format!("Missing string field {key}"))
}

fn version(value: &str) -> Result<[u32; 4]> {
    let parts: Vec<_> = value.split('.').collect();
    ensure!(!parts.is_empty() && parts.len() <= 4, "Invalid version");
    let mut result = [0; 4];
    for (index, part) in parts.iter().enumerate() {
        ensure!(
            !part.is_empty()
                && part.bytes().all(|byte| byte.is_ascii_digit())
                && (part.len() == 1 || !part.starts_with('0')),
            "Invalid version"
        );
        result[index] = part.parse()?;
    }
    Ok(result)
}

fn route(value: &Value) -> Result<(String, String, String)> {
    Ok((
        text(value, "appid")?.to_ascii_lowercase(),
        text(value, "platform")?.to_owned(),
        text(value, "arch")?.to_owned(),
    ))
}

fn releases(value: Value) -> Result<Vec<Value>> {
    catalog::validate_manifest(&value)?;
    if let Some(apps) = value.get("apps") {
        return Ok(apps.as_array().context("apps must be an array")?.clone());
    }
    let mut release = value;
    release.as_object_mut().context("Invalid release")?.remove("schema_version");
    Ok(vec![release])
}

fn read_manifest(path: &Path) -> Result<Value> {
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file() && !metadata.file_type().is_symlink() && metadata.len() <= 65536,
        "Manifest must be a regular file of at most 64 KiB"
    );
    Ok(serde_json::from_slice(&fs::read(path)?)?)
}

fn file_hash(path: &Path) -> Result<String> {
    let mut file = File::open(path)?;
    let mut hash = Sha256::new();
    let mut buffer = [0; 65536];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hash.update(&buffer[..count]);
    }
    Ok(hex::encode(hash.finalize()))
}

fn artifact_name(release: &Value, artifact: &Value, extension: &str) -> Result<String> {
    let identity = serde_json::to_vec(&route(release)?)?;
    let route_hash = hex::encode(Sha256::digest(identity));
    // Full route digest avoids colliding app ids after filename sanitization.
    Ok(format!("{}-{}.{}", route_hash, text(artifact, "sha256")?, extension))
}

fn import_artifact(
    source: &Path,
    destination: &Path,
    artifact: &Value,
    created: &mut Vec<PathBuf>,
) -> Result<()> {
    let size = artifact["size"].as_u64().context("Invalid artifact size")?;
    let hash = text(artifact, "sha256")?;
    // Never replace an existing name, even when it is currently unreferenced.
    if destination.try_exists()? {
        let metadata = fs::symlink_metadata(destination)?;
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "Artifact destination is not a regular file"
        );
        ensure!(
            metadata.len() == size && file_hash(destination)? == hash,
            "Immutable artifact name collision: {}",
            destination.display()
        );
        return Ok(());
    }
    let root = destination.parent().context("Artifact has no parent")?;
    let mut temporary = tempfile::NamedTempFile::new_in(root)?;
    let copied = std::io::copy(&mut File::open(source)?, &mut temporary)?;
    ensure!(
        copied == size && file_hash(temporary.path())? == hash,
        "Source artifact changed while copying: {}",
        source.display()
    );
    temporary.as_file().sync_all()?;
    temporary.persist_noclobber(destination)?;
    created.push(destination.to_owned());
    Ok(())
}

fn import_release(
    release: &mut Value,
    source_root: &Path,
    root: &Path,
    manifest: &Path,
    created: &mut Vec<PathBuf>,
) -> Result<()> {
    let mut references = vec![(None, "crx3".to_owned())];
    if let Some(diffs) = release["differential"].as_array() {
        for (index, diff) in diffs.iter().enumerate() {
            references.push((Some(index), text(diff, "type")?.to_owned()));
        }
    }
    for (index, kind) in references {
        let artifact = match index {
            None => &release["full"],
            Some(index) => &release["differential"][index],
        };
        let source = source_root.join(text(artifact, "file")?);
        let name = artifact_name(release, artifact, &kind)?;
        let destination = root.join(&name);
        ensure!(destination != manifest, "Artifact would replace the catalog");
        import_artifact(&source, &destination, artifact, created)?;
        match index {
            None => release["full"]["file"] = json!(name),
            Some(index) => release["differential"][index]["file"] = json!(name),
        }
    }
    Ok(())
}

fn add_delta(
    release: &mut Value,
    old: &Value,
    root: &Path,
    tool: &Path,
    created: &mut Vec<PathBuf>,
) -> Result<bool> {
    if has_delta(release, old)? {
        return Ok(false);
    }
    ensure!(
        release["differential"].as_array().is_none_or(|diffs| diffs.len() < 32),
        "Differential list is already full"
    );
    let stage = tempfile::tempdir_in(root)?;
    let patch = stage.path().join("update.puff");
    let metadata = delta::generate(&delta::DeltaOptions {
        old_crx: root.join(text(&old["full"], "file")?),
        new_crx: root.join(text(&release["full"], "file")?),
        from_version: text(old, "version")?.to_owned(),
        to_version: text(release, "version")?.to_owned(),
        puffin: tool.to_owned(),
        out: patch.clone(),
        metadata: None,
        opaque: false,
    })?;
    ensure!(metadata["roundtrip_verified"] == true, "Delta was not verified");
    ensure!(
        metadata["full_sha256"] == release["full"]["sha256"],
        "Delta target does not match release"
    );
    ensure!(
        metadata["previous_sha256"] == old["full"]["sha256"],
        "Delta baseline does not match published release"
    );
    if metadata["recommended"] != true {
        bail!("Generated delta is not smaller than the full CRX; using full update");
    }
    let mut entry = json!({});
    for key in ["from_version", "previous_sha256", "type", "file", "size", "sha256"] {
        entry[key] = metadata[key].clone();
    }
    let name = artifact_name(release, &entry, "puff")?;
    import_artifact(&patch, &root.join(&name), &entry, created)?;
    entry["file"] = json!(name);
    if release.get("differential").is_none() {
        release["differential"] = json!([]);
    }
    let diffs = release["differential"].as_array_mut().context("Invalid differential list")?;
    ensure!(diffs.len() < 32, "Differential list is already full");
    diffs.push(entry);
    Ok(true)
}

fn has_delta(release: &Value, old: &Value) -> Result<bool> {
    let old_version = version(text(old, "version")?)?;
    Ok(release["differential"].as_array().is_some_and(|diffs| {
        diffs.iter().any(|diff| {
            diff["previous_sha256"] == old["full"]["sha256"]
                && diff["from_version"].as_str().and_then(|v| version(v).ok()) == Some(old_version)
        })
    }))
}

fn preserve_existing_deltas(release: &mut Value, old: &Value) -> Result<()> {
    let Some(previous) = old["differential"].as_array() else {
        return Ok(());
    };
    if release.get("differential").is_none() {
        release["differential"] = json!([]);
    }
    let diffs = release["differential"].as_array_mut().context("Invalid differential list")?;
    for entry in previous {
        let from = version(text(entry, "from_version")?)?;
        if !diffs.iter().any(|candidate| {
            candidate["previous_sha256"] == entry["previous_sha256"]
                && candidate["from_version"].as_str().and_then(|v| version(v).ok()) == Some(from)
        }) {
            ensure!(diffs.len() < 32, "Merged differential list exceeds 32 entries");
            diffs.push(entry.clone());
        }
    }
    Ok(())
}

pub fn publish(options: &PublishOptions) -> Result<Value> {
    publish_with(options, true)
}

/// Publish the complete differential list selected by a history manager.
/// The ordinary CLI publisher continues to merge existing same-version deltas.
pub fn publish_authoritative(options: &PublishOptions) -> Result<Value> {
    publish_with(options, false)
}

fn publish_with(options: &PublishOptions, preserve_deltas: bool) -> Result<Value> {
    let absolute_manifest = std::path::absolute(&options.manifest)?;
    let root = absolute_manifest.parent().context("Catalog has no directory")?;
    fs::create_dir_all(root)?;
    let root = root.canonicalize()?;
    let manifest = root.join(absolute_manifest.file_name().context("Catalog has no filename")?);
    let lock_path =
        root.join(format!("{}.publish.lock", manifest.file_name().unwrap().to_string_lossy()));
    if let Ok(metadata) = fs::symlink_metadata(&lock_path) {
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "Publish lock must be a regular file"
        );
    }
    let lock =
        OpenOptions::new().create(true).truncate(false).read(true).write(true).open(lock_path)?;
    lock.try_lock_exclusive().context("Another publisher is updating this catalog")?;

    // Validate all source bytes before changing the destination. The final
    // snapshot is verified again after copying, so concurrent source changes
    // cannot result in a broken served catalog.
    let source = std::path::absolute(&options.release)?;
    let _validated_source = catalog::load(&source).context("Invalid input release")?;
    let mut incoming = releases(read_manifest(&source)?)?;
    ensure!(!incoming.is_empty(), "Input release contains no applications");
    let source_root = source.parent().context("Source release has no directory")?;
    let mut current = if manifest.try_exists()? {
        let _validated_current = catalog::load(&manifest).context("Invalid current catalog")?;
        releases(read_manifest(&manifest)?)?
    } else {
        vec![]
    };

    let mut keys = HashSet::new();
    for release in &incoming {
        let key = route(release)?;
        ensure!(keys.insert(key.clone()), "Duplicate release route");
        if let Some(old) = current.iter().find(|value| route(value).ok().as_ref() == Some(&key)) {
            let new_version = version(text(release, "version")?)?;
            let old_version = version(text(old, "version")?)?;
            ensure!(
                new_version >= old_version,
                "Refusing downgrade from {} to {}",
                text(old, "version")?,
                text(release, "version")?
            );
            ensure!(
                new_version != old_version || release["full"]["sha256"] == old["full"]["sha256"],
                "Refusing different CRX bytes for an already published version"
            );
        }
    }

    let mut created = vec![];
    let mut committed = false;
    let result = (|| {
        let mut warnings = vec![];
        let mut generated = 0;
        let mut published = vec![];
        for release in &mut incoming {
            let key = route(release)?;
            if options.full_only {
                release.as_object_mut().unwrap().remove("differential");
            }
            import_release(release, source_root, &root, &manifest, &mut created)?;
            let index = current.iter().position(|value| route(value).ok().as_ref() == Some(&key));
            if !options.full_only {
                if let Some(index) = index {
                    let old = &current[index];
                    if version(text(release, "version")?)? == version(text(old, "version")?)? {
                        if preserve_deltas {
                            preserve_existing_deltas(release, old)?;
                        }
                    } else if !has_delta(release, old)? {
                        let result = match &options.puffin {
                            Some(tool) => add_delta(release, old, &root, tool, &mut created),
                            None => {
                                Err(anyhow::anyhow!("Puffin was not found; using the full CRX"))
                            }
                        };
                        match result {
                            Ok(true) => generated += 1,
                            Ok(false) => {}
                            Err(error) => {
                                warnings.push(format!("{} {} {}: {error:#}", key.0, key.1, key.2))
                            }
                        }
                    }
                }
            }
            published.push(json!({"appid":release["appid"], "platform":release["platform"], "arch":release["arch"], "version":release["version"]}));
            match index {
                Some(index) => current[index] = release.clone(),
                None => current.push(release.clone()),
            }
        }
        current.sort_by_key(|release| route(release).unwrap());
        let document = json!({"schema_version": 1, "apps":current});
        catalog::validate_manifest(&document)?;
        let mut staged = tempfile::NamedTempFile::new_in(&root)?;
        serde_json::to_writer_pretty(&mut staged, &document)?;
        staged.write_all(b"\n")?;
        staged.as_file().sync_all()?;
        // All references now point to immutable final files; only this final
        // rename makes them visible to update checks. Keep old files for
        // downloads initiated from the previous catalog.
        drop(catalog::load(staged.path()).context("Final catalog verification failed")?);
        staged.persist(&manifest)?;
        committed = true;
        #[cfg(unix)]
        File::open(&root)?.sync_all()?;
        Ok(
            json!({"manifest":manifest, "published":published, "generated_deltas":generated, "warnings":warnings}),
        )
    })();
    if result.is_err() && !committed {
        for path in created {
            let _ = fs::remove_file(path);
        }
    }
    // Dropping the lock also unlocks it on every error path above.
    FileExt::unlock(&lock)?;
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(root: &Path, version: &str, arch: &str, payload: &[u8]) -> PathBuf {
        fs::create_dir_all(root).unwrap();
        let mut bytes = b"Cr24".to_vec();
        bytes.extend(3u32.to_le_bytes());
        bytes.extend(1u32.to_le_bytes());
        bytes.push(0);
        bytes.extend(payload);
        let file = root.join("package.crx3");
        fs::write(&file, &bytes).unwrap();
        let manifest = root.join("release.json");
        let value = json!({"schema_version":1,"appid":"org.example.Browser","platform":"mac","arch":arch,"version":version,"full":{"file":"package.crx3","size":bytes.len(),"sha256":hex::encode(Sha256::digest(&bytes))}});
        fs::write(&manifest, serde_json::to_vec(&value).unwrap()).unwrap();
        manifest
    }

    fn options(root: &Path, release: PathBuf) -> PublishOptions {
        PublishOptions {
            manifest: root.join("served/catalog.json"),
            release,
            puffin: None,
            full_only: false,
        }
    }

    #[test]
    fn first_publish_then_merge_routes_and_fallback_without_puffin() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let first = fixture(&root.join("v1"), "1.0", "x86_64", b"baseline");
        let options1 = options(root, first);
        assert_eq!(publish(&options1).unwrap()["generated_deltas"], 0);
        let first_catalog = read_manifest(&options1.manifest).unwrap();
        let original_name = text(&first_catalog["apps"][0]["full"], "file").unwrap().to_owned();
        let arm64 = fixture(&root.join("arm64"), "2.0", "arm64", b"mac target");
        publish(&options(root, arm64)).unwrap();
        let second = fixture(&root.join("v2"), "2.0", "x86_64", b"new browser");
        let result = publish(&options(root, second)).unwrap();
        assert_eq!(result["warnings"].as_array().unwrap().len(), 1);
        let document = read_manifest(&options1.manifest).unwrap();
        assert_eq!(document["apps"].as_array().unwrap().len(), 2);
        assert!(document["apps"].as_array().unwrap().iter().all(|entry| entry["version"] == "2.0"));
        assert!(root.join("served").join(original_name).is_file());
        catalog::load(&options1.manifest).unwrap();
    }

    #[test]
    fn rejects_downgrade_and_same_version_replacement_without_catalog_change() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let opts = options(root, fixture(&root.join("v2"), "2.0", "x86_64", b"original"));
        publish(&opts).unwrap();
        let bytes = fs::read(&opts.manifest).unwrap();
        for (label, version) in [("downgrade", "1.0"), ("replace", "2.0.0.0")] {
            let bad = options(root, fixture(&root.join(label), version, "x86_64", b"different"));
            assert!(publish(&bad).is_err());
            assert_eq!(fs::read(&opts.manifest).unwrap(), bytes);
        }
    }

    #[test]
    fn invalid_source_does_not_replace_catalog() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let opts = options(root, fixture(&root.join("v1"), "1", "x86_64", b"original"));
        publish(&opts).unwrap();
        let bytes = fs::read(&opts.manifest).unwrap();
        let second = fixture(&root.join("v2"), "2", "x86_64", b"new");
        fs::write(root.join("v2/package.crx3"), b"tampered").unwrap();
        assert!(publish(&options(root, second)).is_err());
        assert_eq!(fs::read(&opts.manifest).unwrap(), bytes);
    }

    #[test]
    fn idempotent_publish_reuses_immutable_name() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let opts = options(root, fixture(&root.join("v1"), "1", "x86_64", b"original"));
        publish(&opts).unwrap();
        let bytes = fs::read(&opts.manifest).unwrap();
        publish(&opts).unwrap();
        assert_eq!(fs::read(&opts.manifest).unwrap(), bytes);
    }

    #[test]
    fn refuses_collision_without_overwriting_unlisted_file() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let opts = options(root, fixture(&root.join("v1"), "1", "x86_64", b"original"));
        let release = releases(read_manifest(&opts.release).unwrap()).unwrap().remove(0);
        let name = artifact_name(&release, &release["full"], "crx3").unwrap();
        fs::create_dir_all(root.join("served")).unwrap();
        let destination = root.join("served").join(name);
        fs::write(&destination, b"do not overwrite").unwrap();
        assert!(publish(&opts).is_err());
        assert!(!opts.manifest.exists());
        assert_eq!(fs::read(destination).unwrap(), b"do not overwrite");
    }

    #[test]
    fn explicit_missing_tool_falls_back_to_full() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        publish(&options(root, fixture(&root.join("v1"), "1", "x86_64", b"original"))).unwrap();
        let mut opts = options(root, fixture(&root.join("v2"), "2", "x86_64", b"target"));
        opts.puffin = Some(root.join("no-such-puffin"));
        let result = publish(&opts).unwrap();
        assert_eq!(result["warnings"].as_array().unwrap().len(), 1);
        assert_eq!(read_manifest(&opts.manifest).unwrap()["apps"][0]["version"], "2");
        catalog::load(&opts.manifest).unwrap();
    }

    #[test]
    fn imported_delta_survives_reimport_and_full_only_removes_it() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let source = fixture(&root.join("v2"), "2", "x86_64", b"target");
        let mut document = read_manifest(&source).unwrap();
        let patch = b"PUF1patch bytes";
        fs::write(root.join("v2/update.puff"), patch).unwrap();
        document["differential"] = json!([{"from_version":"1", "previous_sha256":"a".repeat(64), "type":"puff", "file":"update.puff", "size":patch.len(), "sha256":hex::encode(Sha256::digest(patch))}]);
        fs::write(&source, serde_json::to_vec(&document).unwrap()).unwrap();
        let mut opts = options(root, source.clone());
        publish(&opts).unwrap();
        let first = read_manifest(&opts.manifest).unwrap();
        let patch_name = text(&first["apps"][0]["differential"][0], "file").unwrap().to_owned();
        document.as_object_mut().unwrap().remove("differential");
        fs::write(&source, serde_json::to_vec(&document).unwrap()).unwrap();
        publish(&opts).unwrap();
        assert_eq!(
            read_manifest(&opts.manifest).unwrap()["apps"][0]["differential"],
            first["apps"][0]["differential"]
        );
        opts.full_only = true;
        publish(&opts).unwrap();
        assert!(read_manifest(&opts.manifest).unwrap()["apps"][0].get("differential").is_none());
        assert!(root.join("served").join(patch_name).exists());
    }

    #[test]
    fn case_insensitive_app_id_updates_same_route() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        publish(&options(root, fixture(&root.join("v1"), "1", "x86_64", b"original"))).unwrap();
        let source = fixture(&root.join("v2"), "2", "x86_64", b"target");
        let mut document = read_manifest(&source).unwrap();
        document["appid"] = json!("ORG.EXAMPLE.BROWSER");
        fs::write(&source, serde_json::to_vec(&document).unwrap()).unwrap();
        let opts = options(root, source);
        publish(&opts).unwrap();
        assert_eq!(read_manifest(&opts.manifest).unwrap()["apps"].as_array().unwrap().len(), 1);
    }

    #[test]
    fn failed_final_catalog_validation_removes_new_artifacts() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let opts = options(root, fixture(&root.join("v1"), "1", "x86_64", b"original"));
        publish(&opts).unwrap();
        let before = fs::read(&opts.manifest).unwrap();
        let source = fixture(&root.join("v2"), "2", "x86_64", b"different browser");
        let release = releases(read_manifest(&source).unwrap()).unwrap().remove(0);
        let artifact_name = artifact_name(&release, &release["full"], "crx3").unwrap();
        let source_root = source.parent().unwrap();
        let mut apps = Vec::new();
        for number in 0..32 {
            let mut app = release.clone();
            app["appid"] = json!(format!("org.example.App{number}"));
            let file = format!("package-{number}.crx3");
            fs::copy(source_root.join("package.crx3"), source_root.join(&file)).unwrap();
            app["full"]["file"] = json!(file);
            apps.push(app);
        }
        fs::write(&source, serde_json::to_vec(&json!({"schema_version":1,"apps":apps})).unwrap())
            .unwrap();
        assert!(publish(&options(root, source)).is_err());
        assert_eq!(fs::read(&opts.manifest).unwrap(), before);
        assert!(!root.join("served").join(artifact_name).exists());
        assert_eq!(fs::read_dir(root.join("served")).unwrap().count(), 3);
        catalog::load(&opts.manifest).unwrap();
    }

    #[test]
    fn publishing_lock_rejects_concurrent_writer() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let opts = options(root, fixture(&root.join("v1"), "1", "x86_64", b"original"));
        fs::create_dir_all(root.join("served")).unwrap();
        let lock = File::create(root.join("served/catalog.json.publish.lock")).unwrap();
        lock.lock_exclusive().unwrap();
        let error = publish(&opts).unwrap_err();
        assert!(error.to_string().contains("Another publisher"));
        assert!(!opts.manifest.exists());
    }
}
