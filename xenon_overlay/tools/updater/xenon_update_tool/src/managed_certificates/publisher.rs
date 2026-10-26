// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Offline publication of the embedded, audited CA catalog for the Axum server.
//! This module does not access the network or modify any certificate trust store.
use super::{
    decode_certificate, read_bounded, sidecar, snapshot_directory, timestamp, utc_now,
    validate_depth, Certificate, Document, Snapshot, MAX_BODY,
};
use anyhow::{ensure, Context, Result};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::HashSet,
    fs,
    io::{ErrorKind, Write},
    path::{Path, PathBuf},
};
use x509_parser::prelude::parse_x509_certificate;

const MANIFEST: &str = include_str!("../../managed_certificates/certificates/manifest.json");
const ASSETS: &[(&str, &str)] = &[
    (
        "gdca-trustauth-e5-root.pem",
        include_str!("../../managed_certificates/certificates/gdca-trustauth-e5-root.pem"),
    ),
    (
        "gdca-chinese-r5-root.pem",
        include_str!("../../managed_certificates/certificates/gdca-chinese-r5-root.pem"),
    ),
    (
        "bjca-global-root-ca1.pem",
        include_str!("../../managed_certificates/certificates/bjca-global-root-ca1.pem"),
    ),
    (
        "bjca-global-root-ca2.pem",
        include_str!("../../managed_certificates/certificates/bjca-global-root-ca2.pem"),
    ),
    (
        "trustasia-global-root-ca-g3.pem",
        include_str!("../../managed_certificates/certificates/trustasia-global-root-ca-g3.pem"),
    ),
    (
        "trustasia-global-root-ca-g4.pem",
        include_str!("../../managed_certificates/certificates/trustasia-global-root-ca-g4.pem"),
    ),
];

pub struct PublishOptions {
    pub source: String,
    pub generation: String,
    pub all: bool,
    pub include: Vec<String>,
    pub empty: bool,
    pub encoding: String,
    pub out: PathBuf,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    schema_version: u32,
    certificates: Vec<ManifestEntry>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ManifestEntry {
    name: String,
    file: String,
    sha256: String,
    spki_sha256: String,
    source_url: String,
    not_before: String,
    not_after: String,
}

struct Asset {
    entry: ManifestEntry,
    pem: String,
    der: Vec<u8>,
}

fn load_assets(manifest: &str, embedded: &[(&str, &str)]) -> Result<Vec<Asset>> {
    ensure!(manifest.len() <= MAX_BODY, "Manifest exceeds size limit");
    validate_depth(manifest.as_bytes())?;
    let manifest: Manifest =
        serde_json::from_str(manifest).context("Invalid certificate manifest")?;
    ensure!(manifest.schema_version == 1, "Unsupported manifest schema_version");
    ensure!(
        !manifest.certificates.is_empty()
            && manifest.certificates.len() <= 256
            && manifest.certificates.len() == embedded.len(),
        "Manifest and embedded catalog differ"
    );
    let mut files = HashSet::new();
    let mut hashes = HashSet::new();
    let mut assets = Vec::new();
    for entry in manifest.certificates {
        ensure!(entry.name.chars().count() <= 128, "Invalid certificate name");
        let basename = entry.file.strip_suffix(".pem").context("Invalid asset filename")?;
        ensure!(
            !basename.is_empty()
                && basename.as_bytes()[0].is_ascii_alphanumeric()
                && basename
                    .bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-'),
            "Invalid asset filename"
        );
        let source = url::Url::parse(&entry.source_url).context("Invalid asset source URL")?;
        ensure!(
            source.scheme() == "https" && source.host_str().is_some(),
            "Asset source must be HTTPS"
        );
        ensure!(files.insert(entry.file.clone()), "Duplicate manifest asset filename");
        let matching: Vec<_> = embedded.iter().filter(|(file, _)| *file == entry.file).collect();
        ensure!(matching.len() == 1, "Missing or duplicate embedded asset: {}", entry.file);
        let pem = matching[0].1.replace("\r\n", "\n");
        let der = decode_certificate(&Certificate {
            name: Some(entry.name.clone()),
            role: "trust_anchor".into(),
            encoding: "pem".into(),
            data: pem.clone(),
            constraints: None,
        })?;
        ensure!(
            hex::encode(Sha256::digest(&der)) == entry.sha256,
            "DER fingerprint mismatch: {}",
            entry.file
        );
        ensure!(hashes.insert(entry.sha256.clone()), "Duplicate manifest certificate");
        let (_, certificate) = parse_x509_certificate(&der)
            .map_err(|_| anyhow::anyhow!("Invalid embedded X.509 certificate"))?;
        ensure!(
            hex::encode(Sha256::digest(certificate.public_key().raw)) == entry.spki_sha256,
            "SPKI fingerprint mismatch: {}",
            entry.file
        );
        ensure!(
            certificate.subject() == certificate.issuer(),
            "Manifest asset is not self-issued: {}",
            entry.file
        );
        certificate
            .verify_signature(None)
            .with_context(|| format!("Manifest asset is not self-signed: {}", entry.file))?;
        ensure!(
            certificate.validity().not_before.timestamp() == timestamp(&entry.not_before)?
                && certificate.validity().not_after.timestamp() == timestamp(&entry.not_after)?,
            "Validity mismatch: {}",
            entry.file
        );
        assets.push(Asset { entry, pem, der });
    }
    Ok(assets)
}

// Keep the original publisher's policy: only replace named, unconstrained trust
// anchors. A different publication policy requires a separate output path.
fn validate_published(bytes: Vec<u8>) -> Result<Snapshot> {
    let snapshot = Snapshot::parse(bytes)?;
    let document: Document = serde_json::from_slice(&snapshot.bytes)?;
    ensure!(
        document.certificates.iter().all(|certificate| {
            certificate.name.is_some()
                && certificate.role == "trust_anchor"
                && certificate.constraints.is_none()
        }),
        "Publisher expects named, unconstrained trust anchors"
    );
    Ok(snapshot)
}

fn date(seconds: i64) -> Result<String> {
    let format = time::format_description::parse_borrowed::<2>(
        "[year]-[month]-[day]T[hour]:[minute]:[second]Z",
    )?;
    Ok(time::OffsetDateTime::from_unix_timestamp(seconds)?.format(&format)?)
}

fn create_snapshot(options: &PublishOptions, now: i64) -> Result<Snapshot> {
    ensure!(!options.out.as_os_str().is_empty(), "--out is required");
    ensure!(
        !options.source.is_empty()
            && options.source.len() <= 64
            && options.source.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)),
        "--source must contain 1-64 ASCII letters, digits, dot, underscore or hyphen"
    );
    ensure!(
        !options.generation.is_empty()
            && !options.generation.starts_with('0')
            && options.generation.bytes().all(|b| b.is_ascii_digit()),
        "--generation must be a positive decimal string without leading zeros"
    );
    let _: u64 = options.generation.parse().context("Generation exceeds uint64")?;
    ensure!(
        matches!(options.encoding.as_str(), "pem" | "der_base64"),
        "--encoding must be pem or der_base64"
    );
    ensure!(
        [options.all, options.empty, !options.include.is_empty()]
            .into_iter()
            .filter(|selected| *selected)
            .count()
            == 1,
        "Select exactly one of --all, --empty or --include"
    );
    let selected: HashSet<_> = options.include.iter().collect();
    ensure!(selected.len() == options.include.len(), "Duplicate --include selection");
    // Validate all embedded material even when publishing an empty withdrawal.
    let assets = load_assets(MANIFEST, ASSETS)?;
    for file in &options.include {
        ensure!(
            assets.iter().any(|asset| asset.entry.file == *file),
            "Unknown certificate selection: {file}"
        );
    }
    let certificates: Vec<_> = assets.into_iter()
        .filter(|asset| options.all || selected.contains(&asset.entry.file))
        .map(|asset| json!({
            "name": asset.entry.name,
            "role": "trust_anchor",
            "encoding": options.encoding,
            "data": if options.encoding == "pem" { asset.pem } else { STANDARD.encode(asset.der) },
        }))
        .collect();
    let document = json!({
        "schema_version": 1,
        "source_id": options.source,
        "generation": options.generation,
        "issued_at": date(now)?,
        "certificates": certificates,
    });
    let mut bytes = serde_json::to_vec_pretty(&document)?;
    bytes.push(b'\n');
    validate_published(bytes)
}

// create_new is also honored by the earlier Node publisher. Do not steal even a
// stale marker: an operator must establish the previous publisher has exited.
struct PublicationLock {
    path: Option<PathBuf>,
    file: Option<fs::File>,
}

impl PublicationLock {
    fn acquire(target: &Path) -> Result<Self> {
        let path = sidecar(target, ".publish.lock");
        let mut options = fs::OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let file = options.open(&path).context(
            "Cannot acquire publication lock; another publisher or stale marker owns this output",
        )?;
        let mut lock = Self { path: Some(path), file: Some(file) };
        writeln!(lock.file.as_mut().unwrap(), "pid={}", std::process::id())?;
        Ok(lock)
    }

    fn release(mut self) -> Result<()> {
        drop(self.file.take());
        fs::remove_file(self.path.take().unwrap())
            .context("CA snapshot was published, but its publication lock could not be removed")
    }
}

impl Drop for PublicationLock {
    fn drop(&mut self) {
        drop(self.file.take());
        // Only the marker successfully created by this instance is owned here.
        if let Some(path) = self.path.take() {
            let _ = fs::remove_file(path);
        }
    }
}

fn optional_snapshot(path: &Path) -> Result<Option<Snapshot>> {
    match read_bounded(path) {
        Ok(bytes) => Ok(Some(validate_published(bytes).with_context(|| {
            format!("Existing publication is invalid; restore it instead of resetting the watermark: {}", path.display())
        })?)),
        Err(error) if error.downcast_ref::<std::io::Error>().is_some_and(|error| error.kind() == ErrorKind::NotFound) => Ok(None),
        Err(error) => Err(error).with_context(|| format!("Cannot read existing publication: {}", path.display())),
    }
}

fn publication_history(target: &Path) -> Result<(Option<Snapshot>, Option<Snapshot>)> {
    let current = optional_snapshot(target)?;
    let accepted = optional_snapshot(&sidecar(target, ".accepted.json"))?;
    if let (Some(current), Some(accepted)) = (&current, &accepted) {
        ensure!(
            current.source == accepted.source,
            "Published and accepted source_id conflict; restore the publication history"
        );
        ensure!(
            current.generation != accepted.generation || current.bytes == accepted.bytes,
            "Published and accepted generation conflict; restore the immutable publication history"
        );
    }
    Ok((current, accepted))
}

pub(super) fn summary(snapshot: &Snapshot) -> Result<Value> {
    let document: Document = serde_json::from_slice(&snapshot.bytes)?;
    let assets = load_assets(MANIFEST, ASSETS)?;
    let certificates = document.certificates.iter().map(|certificate| -> Result<Value> {
        let sha256 = hex::encode(Sha256::digest(decode_certificate(certificate)?));
        let file = assets.iter().find(|asset| asset.entry.sha256 == sha256)
            .map(|asset| asset.entry.file.as_str());
        Ok(json!({"name": certificate.name, "role": certificate.role, "sha256": sha256, "file": file}))
    }).collect::<Result<Vec<_>>>()?;
    Ok(json!({"source_id": document.source_id, "generation": document.generation,
        "issued_at": document.issued_at,
        "count": certificates.len(), "certificates": certificates}))
}

/// Read-only publishing diagnostics. Generation remains a decimal string so a
/// browser cannot round a uint64 watermark through JavaScript's Number type.
pub fn publication_state(target: Option<&Path>) -> Result<Value> {
    let assets = load_assets(MANIFEST, ASSETS)?;
    let catalog: Vec<_> = assets
        .iter()
        .map(|asset| {
            json!({
                "file": asset.entry.file, "name": asset.entry.name, "sha256": asset.entry.sha256,
                "spki_sha256": asset.entry.spki_sha256, "not_before": asset.entry.not_before,
                "not_after": asset.entry.not_after,
            })
        })
        .collect();
    let mut state = json!({"catalog": catalog, "published_snapshot": null,
        "source_id": null, "next_generation": null, "publication_error": null,
        "can_publish": false});
    let Some(target) = target else {
        return Ok(state);
    };
    match publication_history(target) {
        Ok((current, accepted)) => {
            let highest = current
                .as_ref()
                .into_iter()
                .chain(accepted.as_ref())
                .max_by_key(|snapshot| snapshot.generation);
            state["source_id"] = json!(highest.map(|snapshot| snapshot.source.as_str()));
            let next = highest.map_or(Some(1), |snapshot| snapshot.generation.checked_add(1));
            state["next_generation"] = json!(next.map(|generation| generation.to_string()));
            state["published_snapshot"] =
                current.as_ref().map(summary).transpose()?.unwrap_or(Value::Null);
            state["can_publish"] = json!(next.is_some());
            if next.is_none() {
                state["publication_error"] = json!("Generation has reached uint64 maximum; use a separate snapshot path and source");
            }
        }
        Err(error) => state["publication_error"] = json!(format!("{error:#}")),
    }
    Ok(state)
}

pub fn publish(options: &PublishOptions) -> Result<Value> {
    let snapshot = create_snapshot(options, utc_now())?;
    let target = std::path::absolute(&options.out).context("Cannot resolve publication path")?;
    let directory = snapshot_directory(&target);
    fs::create_dir_all(directory).context("Cannot create publication directory")?;
    let lock = PublicationLock::acquire(&target)?;
    let (current, accepted) = publication_history(&target)?;
    for previous in current.iter().chain(accepted.iter()) {
        ensure!(
            snapshot.source == previous.source,
            "source_id cannot change at the same output path; use a separate path for a new source"
        );
        ensure!(snapshot.generation > previous.generation, "generation must increase; an existing generation is immutable, including its timestamps");
    }
    let mut output =
        tempfile::Builder::new().prefix(".managed-ca-").suffix(".tmp").tempfile_in(directory)?;
    output.write_all(&snapshot.bytes).context("Cannot write CA snapshot")?;
    output.as_file().sync_all().context("Cannot flush CA snapshot")?;
    output.persist(&target).context("Cannot atomically publish CA snapshot")?;
    #[cfg(unix)]
    fs::File::open(directory)?.sync_all()?;
    lock.release()?;
    let document: Document = serde_json::from_slice(&snapshot.bytes)?;
    Ok(json!({
        "path": target,
        "bytes": snapshot.bytes.len(),
        "sha256": hex::encode(Sha256::digest(&snapshot.bytes)),
        "generation": document.generation,
        "count": document.certificates.len(),
    }))
}

#[cfg(test)]
mod tests {
    use super::super::{handle, Settings, Store, ENDPOINT};
    use super::*;
    use axum::{
        body::Body,
        extract::Request,
        http::{header, StatusCode},
    };
    use http_body_util::BodyExt;
    use std::sync::Arc;

    fn options(out: PathBuf) -> PublishOptions {
        PublishOptions {
            source: "company-ca".into(),
            generation: "1".into(),
            all: true,
            include: vec![],
            empty: false,
            encoding: "der_base64".into(),
            out,
        }
    }

    #[test]
    fn embedded_roots_match_manifest_and_verify_signatures() {
        let assets = load_assets(MANIFEST, ASSETS).unwrap();
        assert_eq!(assets.len(), 6);
        let mut manifest: Value = serde_json::from_str(MANIFEST).unwrap();
        for (field, wrong) in [
            ("sha256", "00"),
            ("spki_sha256", "00"),
            ("not_before", "2020-01-01T00:00:00Z"),
            ("source_url", "http://example.com/root.pem"),
        ] {
            let mut changed = manifest.clone();
            changed["certificates"][0][field] = json!(wrong);
            assert!(load_assets(&changed.to_string(), ASSETS).is_err(), "{field}");
        }
        manifest["certificates"][1] = manifest["certificates"][0].clone();
        assert!(load_assets(&manifest.to_string(), ASSETS).is_err());
        assert!(load_assets(&MANIFEST.replacen('{', "{\"schema_version\":1,", 1), ASSETS).is_err());
        let mut der = assets[0].der.clone();
        *der.last_mut().unwrap() ^= 1;
        let pem = format!(
            "-----BEGIN CERTIFICATE-----\n{}\n-----END CERTIFICATE-----\n",
            STANDARD.encode(&der)
        );
        let mut embedded = ASSETS.to_vec();
        embedded[0].1 = &pem;
        let mut changed: Value = serde_json::from_str(MANIFEST).unwrap();
        changed["certificates"][0]["sha256"] = json!(hex::encode(Sha256::digest(&der)));
        let error = load_assets(&changed.to_string(), &embedded).err().unwrap();
        assert!(error.to_string().contains("self-signed"));
    }

    #[test]
    fn subset_encodings_and_explicit_empty_snapshots() {
        let mut options = options(PathBuf::from("unused.json"));
        options.all = false;
        options.include = vec![ASSETS[3].0.into(), ASSETS[0].0.into()];
        for encoding in ["der_base64", "pem"] {
            options.encoding = encoding.into();
            let snapshot = create_snapshot(&options, 1792886400).unwrap();
            let doc: Value = serde_json::from_slice(&snapshot.bytes).unwrap();
            let certificates = doc["certificates"].as_array().unwrap();
            assert_eq!(certificates.len(), 2);
            assert_eq!(certificates[0]["name"], "GDCA TrustAUTH E5 ROOT");
            assert_eq!(certificates[1]["name"], "BJCA Global Root CA2");
            assert!(certificates.iter().all(|cert| cert["encoding"] == encoding));
            assert_eq!(doc["issued_at"], date(1792886400).unwrap());
            assert!(doc.get("expires_at").is_none());
        }
        options.include.clear();
        options.empty = true;
        options.generation = u64::MAX.to_string();
        let snapshot = create_snapshot(&options, 1792886400).unwrap();
        assert_eq!(snapshot.generation, u64::MAX);
        let doc: Value = serde_json::from_slice(&snapshot.bytes).unwrap();
        assert_eq!(doc["certificates"], json!([]));
    }

    #[test]
    fn invalid_arguments_do_not_create_output_or_lock() {
        let dir = tempfile::tempdir().unwrap();
        let out = dir.path().join("absent/current.json");
        for generation in ["", "0", "01", "-1", "+1", "1.0", "18446744073709551616"] {
            let mut options = options(out.clone());
            options.generation = generation.into();
            assert!(publish(&options).is_err(), "{generation}");
        }
        let mut cases = Vec::new();
        let mut invalid = options(out.clone());
        invalid.source = "bad source".into();
        cases.push(invalid);
        let mut invalid = options(out.clone());
        invalid.encoding = "der".into();
        cases.push(invalid);
        let mut invalid = options(out.clone());
        invalid.all = false;
        cases.push(invalid);
        let mut invalid = options(out.clone());
        invalid.empty = true;
        cases.push(invalid);
        let mut invalid = options(out.clone());
        invalid.include.push(ASSETS[0].0.into());
        cases.push(invalid);
        let mut invalid = options(out.clone());
        invalid.all = false;
        invalid.include = vec![ASSETS[0].0.into(), ASSETS[0].0.into()];
        cases.push(invalid);
        let mut invalid = options(out.clone());
        invalid.all = false;
        invalid.include.push("unknown.pem".into());
        cases.push(invalid);
        let mut invalid = options(PathBuf::new());
        invalid.empty = true;
        invalid.all = false;
        cases.push(invalid);
        for invalid in cases {
            assert!(publish(&invalid).is_err());
        }
        assert!(!out.parent().unwrap().exists());
    }

    #[test]
    fn publication_respects_current_and_accepted_high_watermarks() {
        let dir = tempfile::tempdir().unwrap();
        let mut options = options(dir.path().join("nested/current.json"));
        let first = publish(&options).unwrap();
        let original = fs::read(&options.out).unwrap();
        assert_eq!(first["count"], 6);
        assert_eq!(first["sha256"], hex::encode(Sha256::digest(&original)));
        for generation in ["1", "0"] {
            options.generation = generation.into();
            assert!(publish(&options).is_err());
            assert_eq!(fs::read(&options.out).unwrap(), original);
        }
        options.generation = "10".into();
        let accepted = create_snapshot(&options, utc_now()).unwrap();
        let accepted_path = sidecar(&options.out, ".accepted.json");
        fs::write(&accepted_path, &accepted.bytes).unwrap();
        for generation in ["2", "10"] {
            options.generation = generation.into();
            assert!(publish(&options).is_err());
        }
        options.generation = "11".into();
        options.source = "new-source".into();
        assert!(publish(&options).is_err());
        options.source = "company-ca".into();
        options.all = false;
        options.empty = true;
        assert_eq!(publish(&options).unwrap()["count"], 0);
        assert_eq!(
            fs::read(&accepted_path).unwrap(),
            accepted.bytes,
            "Publisher must not modify server state"
        );
        assert!(!sidecar(&options.out, ".publish.lock").exists());
    }

    #[test]
    fn missing_current_cannot_bypass_accepted_version_or_source() {
        let dir = tempfile::tempdir().unwrap();
        let mut options = options(dir.path().join("current.json"));
        let accepted = create_snapshot(&options, utc_now()).unwrap();
        fs::write(sidecar(&options.out, ".accepted.json"), &accepted.bytes).unwrap();
        assert!(publish(&options).is_err());
        options.generation = "2".into();
        options.source = "other-source".into();
        assert!(publish(&options).is_err());
        assert!(!options.out.exists());
    }

    #[test]
    fn publication_state_retains_accepted_floor_and_disables_invalid_history() {
        let dir = tempfile::tempdir().unwrap();
        let mut options = options(dir.path().join("current.json"));
        let empty = publication_state(Some(&options.out)).unwrap();
        assert_eq!(empty["next_generation"], "1");
        assert_eq!(empty["can_publish"], true);
        publish(&options).unwrap();
        options.generation = "9007199254740993".into();
        let accepted = create_snapshot(&options, utc_now()).unwrap();
        let accepted_path = sidecar(&options.out, ".accepted.json");
        fs::write(&accepted_path, &accepted.bytes).unwrap();
        let status = publication_state(Some(&options.out)).unwrap();
        assert_eq!(status["published_snapshot"]["generation"], "1");
        assert_eq!(status["next_generation"], "9007199254740994");
        assert!(status["publication_error"].is_null());
        fs::remove_file(&options.out).unwrap();
        assert_eq!(
            publication_state(Some(&options.out)).unwrap()["next_generation"],
            "9007199254740994"
        );
        options.generation = u64::MAX.to_string();
        fs::write(&accepted_path, &create_snapshot(&options, utc_now()).unwrap().bytes).unwrap();
        let status = publication_state(Some(&options.out)).unwrap();
        assert!(status["next_generation"].is_null());
        assert_eq!(status["can_publish"], false);
        fs::write(&accepted_path, b"corrupt").unwrap();
        let status = publication_state(Some(&options.out)).unwrap();
        assert_eq!(status["can_publish"], false);
        assert!(status["publication_error"].as_str().unwrap().contains("invalid"));
    }

    #[test]
    fn conflicting_immutable_generations_cannot_be_overwritten_via_new_generation() {
        let dir = tempfile::tempdir().unwrap();
        let mut options = options(dir.path().join("current.json"));
        publish(&options).unwrap();
        let before = fs::read(&options.out).unwrap();
        options.encoding = "pem".into();
        fs::write(
            sidecar(&options.out, ".accepted.json"),
            &create_snapshot(&options, utc_now()).unwrap().bytes,
        )
        .unwrap();
        let status = publication_state(Some(&options.out)).unwrap();
        assert_eq!(status["can_publish"], false);
        assert!(status["publication_error"].as_str().unwrap().contains("generation conflict"));
        options.generation = "2".into();
        assert!(publish(&options).is_err());
        assert_eq!(fs::read(&options.out).unwrap(), before);
    }

    #[test]
    fn serving_status_retains_old_publications_and_reports_invalid_file_fallback() {
        let dir = tempfile::tempdir().unwrap();
        let mut options = options(dir.path().join("current.json"));
        let store =
            Store::new(Settings { snapshot_file: options.out.clone(), ..Default::default() })
                .unwrap();
        assert!(store.admin_status().unwrap()["serving_snapshot"].is_null());
        publish(&options).unwrap();
        assert_eq!(store.admin_status().unwrap()["serving_snapshot"]["count"], 6);
        fs::write(&options.out, b"corrupt").unwrap();
        assert_eq!(store.admin_status().unwrap()["serving_snapshot"]["generation"], "1");
        assert_eq!(store.admin_status().unwrap()["serving_error"], "reload_failed");
        assert_eq!(publication_state(Some(&options.out)).unwrap()["can_publish"], false);
        drop(store);
        options.generation = "2".into();
        let old = create_snapshot(&options, utc_now() - 365 * 86400).unwrap();
        fs::write(&options.out, &old.bytes).unwrap();
        fs::write(sidecar(&options.out, ".accepted.json"), &old.bytes).unwrap();
        let store =
            Store::new(Settings { snapshot_file: options.out.clone(), ..Default::default() })
                .unwrap();
        assert_eq!(store.admin_status().unwrap()["serving_snapshot"]["generation"], "2");
        let status = publication_state(Some(&options.out)).unwrap();
        assert_eq!(status["published_snapshot"]["generation"], "2");
        assert_eq!(status["can_publish"], true);
        assert_eq!(status["next_generation"], "3");
    }

    #[test]
    fn corrupt_or_different_policy_previous_state_is_not_overwritten() {
        let dir = tempfile::tempdir().unwrap();
        let mut options = options(dir.path().join("current.json"));
        let initial = create_snapshot(&options, utc_now()).unwrap();
        let document: Value = serde_json::from_slice(&initial.bytes).unwrap();
        let mut without_name = document.clone();
        without_name["certificates"][0].as_object_mut().unwrap().remove("name");
        let mut intermediate = document.clone();
        intermediate["certificates"][0]["role"] = json!("intermediate");
        let mut constrained = document.clone();
        constrained["certificates"][0]["constraints"] =
            json!({"permitted_dns_names":["example.com"]});
        let invalid = vec![
            b"corrupt".to_vec(),
            vec![b' '; MAX_BODY + 1],
            document.to_string().replacen('{', "{\"generation\":\"999\",", 1).into_bytes(),
            serde_json::to_vec(&without_name).unwrap(),
            serde_json::to_vec(&intermediate).unwrap(),
            serde_json::to_vec(&constrained).unwrap(),
        ];
        options.generation = "2".into();
        for path in [options.out.clone(), sidecar(&options.out, ".accepted.json")] {
            for bytes in &invalid {
                fs::write(&path, bytes).unwrap();
                assert!(publish(&options).is_err());
                assert_eq!(fs::read(&path).unwrap(), *bytes);
                assert!(!sidecar(&options.out, ".publish.lock").exists());
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn existing_lock_is_preserved_and_failed_io_cleans_owned_marker() {
        let dir = tempfile::tempdir().unwrap();
        let options = options(dir.path().join("current.json"));
        let lock_path = sidecar(&options.out, ".publish.lock");
        fs::write(&lock_path, b"another publisher\n").unwrap();
        assert!(publish(&options).is_err());
        assert_eq!(fs::read(&lock_path).unwrap(), b"another publisher\n");
        assert!(!options.out.exists());
        fs::remove_file(&lock_path).unwrap();
        fs::create_dir(&options.out).unwrap();
        assert!(publish(&options).is_err());
        assert!(options.out.is_dir());
        assert!(!lock_path.exists());
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    }

    #[cfg(windows)]
    #[test]
    fn failed_atomic_replacement_preserves_snapshot_and_cleans_temporary_file() {
        let dir = tempfile::tempdir().unwrap();
        let mut options = options(dir.path().join("current.json"));
        publish(&options).unwrap();
        let original = fs::read(&options.out).unwrap();
        let writable = fs::metadata(&options.out).unwrap().permissions();
        let mut readonly = writable.clone();
        readonly.set_readonly(true);
        fs::set_permissions(&options.out, readonly).unwrap();
        options.generation = "2".into();
        let result = publish(&options);
        // Restore the test fixture before any assertion, including failure.
        fs::set_permissions(&options.out, writable).unwrap();
        assert!(result.is_err());
        drop(result);
        assert_eq!(fs::read(&options.out).unwrap(), original);
        assert!(!sidecar(&options.out, ".publish.lock").exists());
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    }

    #[tokio::test]
    async fn native_publication_is_served_by_axum_with_etag_and_withdrawal() {
        let dir = tempfile::tempdir().unwrap();
        let mut options = options(dir.path().join("current.json"));
        let store = Arc::new(
            Store::new(Settings { snapshot_file: options.out.clone(), ..Default::default() })
                .unwrap(),
        );
        publish(&options).unwrap();
        let request = || Request::builder().uri(ENDPOINT).body(Body::empty()).unwrap();
        let response = handle(store.clone(), request()).await;
        assert_eq!(response.status(), StatusCode::OK);
        let etag = response.headers()[header::ETAG].clone();
        assert_eq!(
            response.into_body().collect().await.unwrap().to_bytes(),
            fs::read(&options.out).unwrap()
        );
        let response = handle(
            store.clone(),
            Request::builder()
                .uri(ENDPOINT)
                .header(header::IF_NONE_MATCH, &etag)
                .body(Body::empty())
                .unwrap(),
        )
        .await;
        assert_eq!(response.status(), StatusCode::NOT_MODIFIED);
        options.generation = "2".into();
        options.all = false;
        options.empty = true;
        publish(&options).unwrap();
        let response = handle(store, request()).await;
        assert_eq!(response.status(), StatusCode::OK);
        assert_ne!(response.headers()[header::ETAG], etag);
        let bytes = response.into_body().collect().await.unwrap().to_bytes();
        let document: Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(document["generation"], "2");
        assert_eq!(document["certificates"], json!([]));
    }
}
