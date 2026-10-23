// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Validated Omaha releases and pinned, immutable artifact handles.
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{HashMap, HashSet},
    fs::{self, File, OpenOptions},
    io::Read,
    path::Path,
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};

pub const MAX_MANIFEST: u64 = 64 * 1024;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ArtifactSpec {
    pub file: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Differential {
    pub file: String,
    pub size: u64,
    pub sha256: String,
    pub from_version: String,
    pub previous_sha256: String,
    #[serde(rename = "type")]
    pub kind: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Release {
    pub appid: String,
    pub version: String,
    pub minimum_version: Option<String>,
    pub platform: String,
    pub arch: String,
    pub full: ArtifactSpec,
    #[serde(default)]
    pub differential: Vec<Differential>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Snapshot {
    identity: (u64, u64),
    size: u64,
    modified: (u64, u64),
    changed: (u64, u64),
    links: u64,
}

#[cfg(unix)]
fn snapshot(file: &File) -> Result<Snapshot> {
    use std::os::unix::fs::MetadataExt;
    let m = file.metadata()?;
    ensure!(m.is_file(), "Expected a regular file");
    Ok(Snapshot {
        identity: (m.dev(), m.ino()),
        size: m.len(),
        modified: (m.mtime() as u64, m.mtime_nsec() as u64),
        changed: (m.ctime() as u64, m.ctime_nsec() as u64),
        links: m.nlink(),
    })
}

#[cfg(windows)]
fn snapshot(file: &File) -> Result<Snapshot> {
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Storage::FileSystem::{
        GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION, FILE_ATTRIBUTE_DIRECTORY,
        FILE_ATTRIBUTE_REPARSE_POINT,
    };
    let mut info: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
    // The borrowed handle stays alive throughout the call.
    ensure!(
        unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut info) } != 0,
        "Cannot inspect file: {}",
        std::io::Error::last_os_error()
    );
    ensure!(
        info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) == 0,
        "Expected a regular file without a reparse point"
    );
    let pair = |hi: u32, lo: u32| ((hi as u64) << 32) | lo as u64;
    Ok(Snapshot {
        identity: (info.dwVolumeSerialNumber as u64, pair(info.nFileIndexHigh, info.nFileIndexLow)),
        size: pair(info.nFileSizeHigh, info.nFileSizeLow),
        modified: (
            pair(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime),
            0,
        ),
        changed: (pair(info.ftCreationTime.dwHighDateTime, info.ftCreationTime.dwLowDateTime), 0),
        links: info.nNumberOfLinks as u64,
    })
}

fn open_regular(path: &Path) -> Result<File> {
    let before = fs::symlink_metadata(path).with_context(|| format!("Open {}", path.display()))?;
    ensure!(
        before.is_file() && !before.file_type().is_symlink(),
        "Expected regular file: {}",
        path.display()
    );
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::FILE_FLAG_OPEN_REPARSE_POINT;
        options.custom_flags(FILE_FLAG_OPEN_REPARSE_POINT);
    }
    let file = options.open(path)?;
    ensure!(snapshot(&file)?.links == 1, "Hard-linked files are not allowed: {}", path.display());
    Ok(file)
}

pub struct Artifact {
    pub spec: ArtifactSpec,
    file: File,
    verified: Snapshot,
}

impl Artifact {
    pub fn unchanged(&self) -> Result<bool> {
        Ok(snapshot(&self.file)? == self.verified)
    }

    /// Positional reads never share a cursor, including concurrent HTTP ranges.
    pub fn read_at(&self, buffer: &mut [u8], offset: u64) -> std::io::Result<usize> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::FileExt;
            self.file.read_at(buffer, offset)
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::FileExt;
            self.file.seek_read(buffer, offset)
        }
    }
}

pub struct Catalog {
    pub releases: Vec<Release>,
    pub artifacts: HashMap<String, Arc<Artifact>>,
    pub manifest_bytes: Vec<u8>,
}

pub fn parse_version(value: &str) -> Result<[u32; 4]> {
    ensure!(!value.is_empty() && value.len() <= 43, "Invalid version");
    let parts: Vec<_> = value.split('.').collect();
    ensure!(parts.len() <= 4, "Invalid version");
    let mut result = [0; 4];
    for (index, part) in parts.into_iter().enumerate() {
        ensure!(
            !part.is_empty()
                && part.bytes().all(|b| b.is_ascii_digit())
                && (part.len() == 1 || !part.starts_with('0')),
            "Invalid version"
        );
        result[index] = part.parse().context("Version component exceeds uint32")?;
    }
    Ok(result)
}

pub fn valid_sha(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn valid_appid(value: &str) -> bool {
    !value.is_empty() && value.len() <= 128 && value.bytes().all(|b| (0x21..=0x7e).contains(&b))
}

pub(crate) fn validate_artifact(file: &str, size: u64, sha256: &str) -> Result<()> {
    ensure!(
        !file.is_empty()
            && file.len() <= 200
            && file.as_bytes()[0].is_ascii_alphanumeric()
            && file.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)),
        "Artifact file must be a basename"
    );
    ensure!(size > 0 && size <= 9_007_199_254_740_991, "Invalid artifact size");
    ensure!(valid_sha(sha256), "Artifact SHA256 must be 64 lowercase hex digits");
    Ok(())
}

fn parse_manifest(value: &Value) -> Result<Vec<Release>> {
    let mut object = value.as_object().context("Manifest must be an object")?.clone();
    ensure!(object.remove("schema_version") == Some(json!(1)), "Unsupported manifest schema");
    let releases: Vec<Release> = if let Some(apps) = object.remove("apps") {
        ensure!(object.is_empty(), "Unsupported catalog field");
        serde_json::from_value(apps)?
    } else {
        vec![serde_json::from_value(Value::Object(object))?]
    };
    ensure!(releases.len() <= 32, "Too many releases");
    let mut keys = HashSet::new();
    let mut filenames = HashSet::new();
    for release in &releases {
        ensure!(valid_appid(&release.appid), "Invalid appid");
        let target = parse_version(&release.version)?;
        ensure!(
            parse_version(release.minimum_version.as_deref().unwrap_or("0"))? < target,
            "minimum_version must be below target"
        );
        ensure!(
            release.platform == "mac",
            "Omaha releases support macOS only; Windows uses JSON + ZIP"
        );
        ensure!(matches!(release.arch.as_str(), "arm64" | "x86_64"), "Unsupported architecture");
        ensure!(
            keys.insert((release.appid.to_ascii_lowercase(), &release.platform, &release.arch)),
            "Duplicate appid/platform/arch"
        );
        validate_artifact(&release.full.file, release.full.size, &release.full.sha256)?;
        ensure!(filenames.insert(&release.full.file), "Duplicate artifact filename");
        ensure!(release.differential.len() <= 32, "Too many differentials");
        let mut bases = HashSet::new();
        for diff in &release.differential {
            validate_artifact(&diff.file, diff.size, &diff.sha256)?;
            ensure!(filenames.insert(&diff.file), "Duplicate artifact filename");
            ensure!(
                matches!(diff.kind.as_str(), "puff" | "zucc"),
                "Unsupported differential format"
            );
            let base = parse_version(&diff.from_version)?;
            ensure!(base < target, "Differential must upgrade its base version");
            ensure!(valid_sha(&diff.previous_sha256), "Invalid previous_sha256");
            ensure!(bases.insert((base, &diff.previous_sha256)), "Duplicate differential base");
        }
    }
    Ok(releases)
}

pub fn validate_manifest(value: &Value) -> Result<()> {
    parse_manifest(value).map(|_| ())
}

pub(crate) fn load_artifact(root: &Path, spec: ArtifactSpec, kind: &str) -> Result<Arc<Artifact>> {
    let mut file = open_regular(&root.join(&spec.file))?;
    let verified = snapshot(&file)?;
    ensure!(verified.size == spec.size, "Artifact size mismatch: {}", spec.file);
    let mut header = [0u8; 12];
    file.read_exact(&mut header).context("Truncated artifact header")?;
    if kind == "crx3" {
        let header_size = u32::from_le_bytes(header[8..12].try_into().unwrap()) as u64;
        ensure!(
            &header[..4] == b"Cr24"
                && u32::from_le_bytes(header[4..8].try_into().unwrap()) == 3
                && header_size > 0
                && header_size <= 16 * 1024 * 1024
                && header_size + 12 < spec.size,
            "Full artifact is not a CRX3 container"
        );
    } else if kind == "zip" {
        ensure!(&header[..4] == b"PK\x03\x04", "Artifact is not a ZIP container");
    } else {
        ensure!(
            &header[..4] == if kind == "puff" { b"PUF1" } else { b"Zucc" },
            "Differential container mismatch"
        );
    }
    let mut hasher = Sha256::new();
    hasher.update(header);
    let mut buffer = vec![0; 1024 * 1024];
    loop {
        let n = file.read(&mut buffer)?;
        if n == 0 {
            break;
        }
        hasher.update(&buffer[..n]);
    }
    ensure!(
        hex::encode(hasher.finalize()) == spec.sha256,
        "Artifact SHA256 mismatch: {}",
        spec.file
    );
    ensure!(snapshot(&file)? == verified, "Artifact changed during verification");
    Ok(Arc::new(Artifact { spec, file, verified }))
}

pub fn load(path: &Path) -> Result<Catalog> {
    let mut manifest = open_regular(path)?;
    let before = snapshot(&manifest)?;
    ensure!(before.size <= MAX_MANIFEST, "Manifest exceeds 64 KiB");
    let mut bytes = Vec::new();
    (&mut manifest).take(MAX_MANIFEST + 1).read_to_end(&mut bytes)?;
    ensure!(
        bytes.len() as u64 <= MAX_MANIFEST && snapshot(&manifest)? == before,
        "Manifest changed while reading"
    );
    let releases = parse_manifest(&serde_json::from_slice(&bytes)?)?;
    let root = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."))
        .canonicalize()?;
    let mut artifacts = HashMap::new();
    for release in &releases {
        let artifact = load_artifact(&root, release.full.clone(), "crx3")?;
        artifacts.insert(artifact.spec.file.clone(), artifact);
        for diff in &release.differential {
            let artifact = load_artifact(
                &root,
                ArtifactSpec {
                    file: diff.file.clone(),
                    size: diff.size,
                    sha256: diff.sha256.clone(),
                },
                &diff.kind,
            )?;
            artifacts.insert(artifact.spec.file.clone(), artifact);
        }
    }
    Ok(Catalog { releases, artifacts, manifest_bytes: bytes })
}

fn platform(value: Option<&str>) -> Option<&'static str> {
    match value {
        Some("mac" | "Mac OS X") => Some("mac"),
        _ => None,
    }
}
fn architecture(value: Option<&str>) -> Option<&'static str> {
    match value {
        Some("x64" | "x86_64") => Some("x86_64"),
        Some("arm64") => Some("arm64"),
        _ => None,
    }
}

pub fn build_response(catalog: &Catalog, body: &Value, origin: &str) -> Result<Value> {
    let request =
        body.get("request").and_then(Value::as_object).context("Missing request object")?;
    ensure!(
        request.get("protocol").and_then(Value::as_str) == Some("4.0"),
        "Only Omaha protocol 4.0 is supported"
    );
    let apps = request.get("apps").and_then(Value::as_array).context("Invalid apps list")?;
    ensure!(apps.len() <= 32, "Too many request apps");
    let formats: HashSet<_> =
        request.get("acceptformat").and_then(Value::as_str).unwrap_or("").split(',').collect();
    let mut seen = HashSet::new();
    let mut replies = Vec::new();
    for app in apps {
        let app = app.as_object().context("Invalid request app")?;
        let appid = app.get("appid").and_then(Value::as_str).context("Invalid request appid")?;
        ensure!(
            valid_appid(appid) && seen.insert(appid.to_ascii_lowercase()),
            "Invalid or duplicate request appid"
        );
        let candidates: Vec<_> =
            catalog.releases.iter().filter(|r| r.appid.eq_ignore_ascii_case(appid)).collect();
        let mut reply = json!({ "appid": appid, "status": if candidates.is_empty() { "error-unknownApplication" } else { "ok" } });
        if candidates.is_empty() || !app.contains_key("updatecheck") {
            replies.push(reply);
            continue;
        }
        let check = app["updatecheck"].as_object().context("Invalid updatecheck")?;
        let no = |status: &str, info: &str| {
            let mut result = reply.clone();
            result["updatecheck"] = json!({ "status": status, "info": info });
            result
        };
        let os = request.get("os");
        let reported = os.and_then(|v| v.get("platform")).or_else(|| request.get("@os"));
        let client_platform = platform(reported.and_then(Value::as_str));
        let candidates: Vec<_> = candidates
            .into_iter()
            .filter(|r| Some(r.platform.as_str()) == client_platform)
            .collect();
        if candidates.is_empty()
            || (request.contains_key("@os") && platform(request["@os"].as_str()) != client_platform)
        {
            replies.push(no("error-osnotsupported", "Release platform mismatch"));
            continue;
        }
        let arch = architecture(
            request.get("arch").or_else(|| os.and_then(|v| v.get("arch"))).and_then(Value::as_str),
        );
        let Some(release) = candidates.into_iter().find(|r| Some(r.arch.as_str()) == arch) else {
            replies.push(no("error-hwnotsupported", "Release architecture mismatch"));
            continue;
        };
        let current = match app.get("version") {
            Some(v) => v.as_str().context("Invalid version")?,
            None => "0",
        };
        let current = parse_version(current)?;
        if check.get("updatedisabled") == Some(&Value::Bool(true)) {
            replies.push(no("noupdate", "Updates disabled by client"));
            continue;
        }
        if current >= parse_version(&release.version)? {
            replies.push(no("noupdate", "Installed version is current or newer"));
            continue;
        }
        if current < parse_version(release.minimum_version.as_deref().unwrap_or("0"))? {
            replies.push(no("noupdate", "Installed version is below the release minimum"));
            continue;
        }
        if let Some(prefix) = check.get("targetversionprefix") {
            let prefix = prefix.as_str().context("Invalid targetversionprefix")?;
            if !release.version.starts_with(prefix) {
                replies.push(no("noupdate", "Target version prefix excludes this release"));
                continue;
            }
        }
        if !formats.contains("download") || !formats.contains("crx3") {
            replies.push(no("error-inexpressible", "Client must support download and crx3"));
            continue;
        }
        let mut cached = HashSet::new();
        if let Some(items) = app.get("cached_items") {
            let items = items.as_array().context("Invalid cached_items")?;
            ensure!(items.len() <= 128, "Too many cached_items");
            for item in items {
                let hash =
                    item.get("sha256").and_then(Value::as_str).context("Invalid cached_items")?;
                ensure!(valid_sha(hash), "Invalid cached_items");
                cached.insert(hash);
            }
        }
        let download = |file: &str, size: u64, sha: &str| json!({ "type": "download", "size": size, "out": { "sha256": sha }, "urls": [{ "url": format!("{origin}/artifacts/{file}") }] });
        let install = json!({ "type": "crx3", "in": { "sha256": release.full.sha256 }, "size": release.full.size, "path": ".", "arguments": "" });
        let mut pipelines = Vec::new();
        if let Some(diff) = release.differential.iter().find(|d| {
            parse_version(&d.from_version).ok() == Some(current)
                && cached.contains(d.previous_sha256.as_str())
                && formats.contains(d.kind.as_str())
                && d.size < release.full.size
        }) {
            pipelines.push(json!({ "pipeline_id": format!("diff-{}-{}", diff.kind, diff.sha256), "operations": [download(&diff.file, diff.size, &diff.sha256),
                { "type": diff.kind, "previous": { "sha256": diff.previous_sha256 }, "out": { "sha256": release.full.sha256 } }, install.clone()] }));
        }
        pipelines.push(json!({ "pipeline_id": format!("full-{}", release.full.sha256), "operations": [download(&release.full.file, release.full.size, &release.full.sha256), install] }));
        reply["updatecheck"] =
            json!({ "status": "ok", "nextversion": release.version, "pipelines": pipelines });
        replies.push(reply);
    }
    let elapsed_days =
        SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs().saturating_sub(1_167_609_600)
            / 86400;
    Ok(
        json!({ "response": { "protocol": "4.0", "server": "xenon-axum", "daystart": { "elapsed_days": elapsed_days }, "apps": replies } }),
    )
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use tempfile::TempDir;

    pub fn fixture() -> (TempDir, Value, Vec<u8>) {
        let dir = tempfile::tempdir().unwrap();
        let mut full = b"Cr24\x03\x00\x00\x00\x01\x00\x00\x00x".to_vec();
        full.extend_from_slice(&[42u8; 1024]);
        let patch = b"PUF1test-patch";
        fs::write(dir.path().join("app.crx3"), &full).unwrap();
        fs::write(dir.path().join("delta.puff"), patch).unwrap();
        let manifest = json!({ "schema_version": 1, "appid": "org.chromium.Chromium", "version": "1.0.0.2", "platform": "mac", "arch": "x86_64",
            "full": { "file": "app.crx3", "size": full.len(), "sha256": hex::encode(Sha256::digest(&full)) },
            "differential": [{ "file": "delta.puff", "size": patch.len(), "sha256": hex::encode(Sha256::digest(patch)), "from_version": "1.0.0.1", "previous_sha256": "a".repeat(64), "type": "puff" }] });
        fs::write(dir.path().join("release.json"), serde_json::to_vec(&manifest).unwrap()).unwrap();
        (dir, manifest, full)
    }

    pub fn request() -> Value {
        json!({ "request": { "protocol": "4.0", "acceptformat": "download,crx3,puff", "@os": "mac", "arch": "x64", "os": { "platform": "Mac OS X", "arch": "arm64" },
            "apps": [{ "appid": "ORG.CHROMIUM.CHROMIUM", "version": "1.0.0.1", "updatecheck": {}, "cached_items": [{ "sha256": "a".repeat(64) }] }] } })
    }

    #[test]
    fn numeric_versions() {
        assert!(parse_version("1.10").unwrap() > parse_version("1.9").unwrap());
        assert_eq!(parse_version("1").unwrap(), parse_version("1.0.0.0").unwrap());
        for bad in ["", "01.2", "1.-1", "1.2.3.4.5", "4294967296", "1e2"] {
            assert!(parse_version(bad).is_err());
        }
    }

    #[test]
    fn strict_manifest_and_empty_catalog() {
        assert!(validate_manifest(&json!({ "schema_version": 1, "apps": [] })).is_ok());
        let (_, manifest, _) = fixture();
        for (field, bad) in [
            ("arch", json!("x64")),
            ("platform", json!("win")),
            ("version", json!("01.2")),
            ("minimum_version", json!("2")),
            ("unexpected", json!(true)),
        ] {
            let mut value = manifest.clone();
            value[field] = bad;
            assert!(validate_manifest(&value).is_err(), "{field}");
        }
        let mut value = manifest.clone();
        value["full"]["file"] = json!("../app.crx3");
        assert!(validate_manifest(&value).is_err());
        let mut release = manifest.clone();
        release.as_object_mut().unwrap().remove("schema_version");
        assert!(validate_manifest(
            &json!({ "schema_version": 1, "apps": [release.clone(), release] })
        )
        .is_err());
    }

    #[test]
    fn mac_full_fallback_and_exact_cache_matching() {
        let (dir, _, _) = fixture();
        let catalog = load(&dir.path().join("release.json")).unwrap();
        let mut request = request();
        let reply = build_response(&catalog, &request, "https://updates.example.com").unwrap();
        let pipelines = &reply["response"]["apps"][0]["updatecheck"]["pipelines"];
        assert_eq!(pipelines.as_array().unwrap().len(), 2);
        assert_eq!(pipelines[0]["operations"][1]["type"], "puff");
        assert_eq!(pipelines[0]["operations"][2]["path"], ".");
        assert_eq!(pipelines[0]["operations"][2]["arguments"], "");
        request["request"]["apps"][0]["cached_items"] = json!([]);
        let reply = build_response(&catalog, &request, "https://updates.example.com").unwrap();
        assert_eq!(
            reply["response"]["apps"][0]["updatecheck"]["pipelines"].as_array().unwrap().len(),
            1
        );
    }

    #[test]
    fn platform_arch_version_and_ping_selection() {
        let (dir, _, _) = fixture();
        let mut catalog = load(&dir.path().join("release.json")).unwrap();
        for (field, value, status) in [
            ("@os", json!("win"), "error-osnotsupported"),
            ("arch", json!("arm64"), "error-hwnotsupported"),
        ] {
            let mut req = request();
            req["request"][field] = value;
            assert_eq!(
                build_response(&catalog, &req, "http://127.0.0.1:9001").unwrap()["response"]
                    ["apps"][0]["updatecheck"]["status"],
                status
            );
        }
        let mut req = request();
        req["request"]["apps"][0]["version"] = json!("1.0.0.2");
        assert_eq!(
            build_response(&catalog, &req, "http://127.0.0.1:9001").unwrap()["response"]["apps"][0]
                ["updatecheck"]["status"],
            "noupdate"
        );
        req["request"]["apps"][0].as_object_mut().unwrap().remove("updatecheck");
        assert!(build_response(&catalog, &req, "http://127.0.0.1:9001").unwrap()["response"]
            ["apps"][0]
            .get("updatecheck")
            .is_none());
        catalog.releases[0].platform = "mac".into();
        let mut req = request();
        req["request"]["@os"] = json!("mac");
        req["request"]["os"]["platform"] = json!("Mac OS X");
        assert_eq!(
            build_response(&catalog, &req, "http://127.0.0.1:9001").unwrap()["response"]["apps"][0]
                ["updatecheck"]["pipelines"][0]["operations"][2]["path"],
            "."
        );
    }

    #[test]
    fn hash_tamper_and_hardlinks_rejected() {
        let (dir, _, full) = fixture();
        let catalog = load(&dir.path().join("release.json")).unwrap();
        let mut modified = full;
        modified[30] ^= 1;
        fs::write(dir.path().join("app.crx3"), modified).unwrap();
        assert!(!catalog.artifacts["app.crx3"].unchanged().unwrap());
        assert!(load(&dir.path().join("release.json")).is_err());
        let (dir, _, _) = fixture();
        fs::hard_link(dir.path().join("app.crx3"), dir.path().join("alias.crx3")).unwrap();
        assert!(load(&dir.path().join("release.json")).is_err());
    }
}
