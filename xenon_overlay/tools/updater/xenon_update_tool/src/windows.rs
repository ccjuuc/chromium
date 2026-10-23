// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Windows updates use installed files as their baseline, with JSON and ZIPs.
use crate::{catalog, config};
use anyhow::{ensure, Context, Result};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, HashMap, HashSet},
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration,
};

const MAX_DOCUMENT: u64 = 16 * 1024 * 1024;
const MAX_HISTORY: usize = 4096;
const MAX_DIFFERENTIALS: usize = 32;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Settings {
    pub node: Option<PathBuf>,
    pub seven_zip: Option<PathBuf>,
    pub zucchini: Option<PathBuf>,
    pub default_arch: String,
    pub changelog: String,
    pub force_update: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            node: None,
            seven_zip: None,
            zucchini: None,
            default_arch: "x86_64".into(),
            changelog: String::new(),
            force_update: false,
        }
    }
}

impl Settings {
    pub fn validate(&self) -> Result<()> {
        validate_arch(&self.default_arch)?;
        ensure!(self.changelog.len() <= 16 * 1024, "更新说明不能超过 16 KiB");
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Differential {
    pub from_version: String,
    pub file: String,
    pub size: u64,
    pub sha256: String,
    #[serde(default)]
    pub deletions: Vec<String>,
}

impl Differential {
    fn spec(&self) -> catalog::ArtifactSpec {
        catalog::ArtifactSpec {
            file: self.file.clone(),
            size: self.size,
            sha256: self.sha256.clone(),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Release {
    pub version: String,
    pub arch: String,
    pub full: catalog::ArtifactSpec,
    pub differential: Vec<Differential>,
    pub changelog: String,
    pub force_update: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct History {
    pub version: String,
    pub arch: String,
    pub full: catalog::ArtifactSpec,
    pub tree_sha256: String,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Document {
    schema_version: u32,
    releases: Vec<Release>,
    history: Vec<History>,
    // Keep every previously served filename reachable after a restart.
    artifacts: Vec<catalog::ArtifactSpec>,
}

impl Default for Document {
    fn default() -> Self {
        Self { schema_version: 1, releases: vec![], history: vec![], artifacts: vec![] }
    }
}

pub struct Catalog {
    pub releases: Vec<Release>,
    pub history: Vec<History>,
    pub artifacts: HashMap<String, Arc<catalog::Artifact>>,
    pub manifest_bytes: Vec<u8>,
}

#[derive(Deserialize)]
struct Prepared {
    version: String,
    arch: String,
    full: catalog::ArtifactSpec,
    #[serde(default)]
    differential: Vec<Differential>,
    #[serde(default)]
    warnings: Vec<String>,
    #[serde(default)]
    tree_sha256: Option<String>,
}

fn version(value: &str) -> Result<[u32; 4]> {
    ensure!(value.split('.').count() == 4, "Windows 版本号必须是四段数字");
    catalog::parse_version(value)
}

fn validate_arch(value: &str) -> Result<()> {
    ensure!(matches!(value, "x86" | "x86_64" | "arm64"), "Windows 架构只支持 x86、x86_64 或 arm64");
    Ok(())
}

fn validate_spec(spec: &catalog::ArtifactSpec) -> Result<()> {
    catalog::validate_artifact(&spec.file, spec.size, &spec.sha256)?;
    ensure!(spec.file.ends_with(".zip"), "Windows 更新包必须是 ZIP");
    Ok(())
}

fn equal_spec(a: &catalog::ArtifactSpec, b: &catalog::ArtifactSpec) -> bool {
    a.file == b.file && a.size == b.size && a.sha256 == b.sha256
}

fn relative_path(value: &str) -> bool {
    !value.is_empty()
        && !value.contains(['\\', ':', '\0'])
        && value.split('/').all(|part| !matches!(part, "" | "." | ".."))
}

fn validate_diffs(diffs: &[Differential], target: &str) -> Result<()> {
    ensure!(diffs.len() <= MAX_DIFFERENTIALS, "Windows 差分数量超过限制");
    let target = version(target)?;
    let mut bases = HashSet::new();
    for diff in diffs {
        validate_spec(&diff.spec())?;
        let base = version(&diff.from_version)?;
        ensure!(base < target && bases.insert(base), "差分基线必须唯一且低于目标版本");
        ensure!(
            diff.deletions.len() <= 100_000 && diff.deletions.iter().all(|p| relative_path(p)),
            "无效的差分删除路径"
        );
    }
    Ok(())
}

fn validate_document(doc: &Document) -> Result<()> {
    ensure!(doc.schema_version == 1, "Unsupported Windows catalog schema");
    ensure!(
        doc.history.len() <= MAX_HISTORY
            && doc.releases.len() <= 3
            && doc.artifacts.len() <= 262_144,
        "Windows catalog exceeds limits"
    );
    let mut artifacts = HashMap::new();
    for spec in &doc.artifacts {
        validate_spec(spec)?;
        ensure!(artifacts.insert(&spec.file, spec).is_none(), "Duplicate Windows artifact");
    }
    let check_artifact = |spec: &catalog::ArtifactSpec| -> Result<()> {
        ensure!(
            artifacts.get(&spec.file).is_some_and(|found| equal_spec(found, spec)),
            "Windows artifact is absent from immutable catalog"
        );
        Ok(())
    };
    let mut history = HashMap::new();
    let mut latest = BTreeMap::<&str, [u32; 4]>::new();
    for entry in &doc.history {
        validate_arch(&entry.arch)?;
        let parsed = version(&entry.version)?;
        ensure!(catalog::valid_sha(&entry.tree_sha256), "Invalid Windows tree fingerprint");
        validate_spec(&entry.full)?;
        check_artifact(&entry.full)?;
        ensure!(
            history.insert((entry.arch.as_str(), entry.version.as_str()), entry).is_none(),
            "Duplicate Windows history version"
        );
        latest.entry(&entry.arch).and_modify(|v| *v = (*v).max(parsed)).or_insert(parsed);
    }
    let mut routes = HashSet::new();
    for release in &doc.releases {
        validate_arch(&release.arch)?;
        ensure!(routes.insert(&release.arch), "Duplicate Windows release architecture");
        let parsed = version(&release.version)?;
        ensure!(
            latest.get(release.arch.as_str()) == Some(&parsed),
            "Windows catalog must publish the highest version per architecture"
        );
        let archived = history
            .get(&(release.arch.as_str(), release.version.as_str()))
            .context("Windows release has no archived baseline")?;
        ensure!(
            equal_spec(&release.full, &archived.full),
            "Windows full release differs from its history"
        );
        ensure!(release.changelog.len() <= 16 * 1024, "Windows changelog exceeds limit");
        validate_diffs(&release.differential, &release.version)?;
        for diff in &release.differential {
            check_artifact(&diff.spec())?;
            ensure!(
                history.contains_key(&(release.arch.as_str(), diff.from_version.as_str())),
                "Windows differential has no matching baseline architecture"
            );
            ensure!(
                diff.size < release.full.size,
                "Windows differential is not smaller than full ZIP"
            );
        }
    }
    ensure!(doc.releases.len() == latest.len(), "Windows catalog has an unpublished architecture");
    Ok(())
}

fn regular(path: &Path, limit: u64) -> Result<fs::Metadata> {
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file() && !metadata.file_type().is_symlink() && metadata.len() <= limit,
        "Expected regular file: {}",
        path.display()
    );
    reject_reparse(&metadata)?;
    Ok(metadata)
}

fn reject_reparse(metadata: &fs::Metadata) -> Result<()> {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        ensure!(metadata.file_attributes() & 0x400 == 0, "Reparse points are not allowed");
    }
    #[cfg(not(windows))]
    let _ = metadata;
    Ok(())
}

fn real_directory(path: &Path) -> Result<()> {
    fs::create_dir_all(path)?;
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_dir() && !metadata.file_type().is_symlink(),
        "Expected regular directory: {}",
        path.display()
    );
    reject_reparse(&metadata)
}

fn read_document(root: &Path) -> Result<(Document, Vec<u8>)> {
    let path = root.join("catalog.json");
    if !path.try_exists()? {
        let doc = Document::default();
        let bytes = serde_json::to_vec(&doc)?;
        return Ok((doc, bytes));
    }
    regular(&path, MAX_DOCUMENT)?;
    let bytes = fs::read(path)?;
    ensure!(bytes.len() as u64 <= MAX_DOCUMENT, "Windows catalog exceeds limit");
    let doc: Document = serde_json::from_slice(&bytes)?;
    validate_document(&doc)?;
    Ok((doc, bytes))
}

fn load_document(root: &Path, doc: Document, bytes: Vec<u8>) -> Result<Catalog> {
    validate_document(&doc)?;
    let mut artifacts = HashMap::new();
    for spec in doc.artifacts {
        let artifact = catalog::load_artifact(&root.join("artifacts"), spec, "zip")?;
        artifacts.insert(artifact.spec.file.clone(), artifact);
    }
    Ok(Catalog { releases: doc.releases, history: doc.history, artifacts, manifest_bytes: bytes })
}

pub fn load(root: &Path) -> Result<Catalog> {
    let (doc, bytes) = read_document(root)?;
    load_document(root, doc, bytes)
}

/// Public metadata only; the management response does not reveal baseline paths.
pub fn list(root: &Path) -> Result<Vec<Value>> {
    let (doc, _) = read_document(root)?;
    let mut history = doc.history;
    history.sort_by_key(|entry| {
        (entry.arch.clone(), std::cmp::Reverse(version(&entry.version).unwrap()))
    });
    Ok(history
        .iter()
        .map(|entry| {
            json!({
                "platform":"win", "arch":entry.arch, "version":entry.version,
                "full_size":entry.full.size, "sha256":entry.full.sha256,
            })
        })
        .collect())
}

pub fn build_response(
    catalog: &Catalog,
    query: &str,
    origin: &str,
    default_arch: &str,
) -> Result<Value> {
    ensure!(query.len() <= 4096, "Windows 更新请求参数过长");
    let mut values = HashMap::new();
    for (key, value) in url::form_urlencoded::parse(query.as_bytes()) {
        if matches!(key.as_ref(), "version" | "platform" | "arch") {
            ensure!(
                values.insert(key.into_owned(), value.into_owned()).is_none(),
                "重复的更新请求参数"
            );
        }
    }
    ensure!(
        values.get("platform").is_none_or(|p| p == "win"),
        "此接口只支持 Windows JSON + ZIP 更新"
    );
    let arch = values.get("arch").map(String::as_str).unwrap_or(default_arch);
    let arch = if arch == "x64" { "x86_64" } else { arch };
    validate_arch(arch)?;
    let current = values.get("version").map(String::as_str).unwrap_or("0.0.0.0");
    let parsed_current = version(current)?;
    let release = catalog
        .releases
        .iter()
        .find(|r| r.arch == arch)
        .with_context(|| format!("该 Windows 架构尚未发布更新：{arch}"))?;
    let package = |spec: &catalog::ArtifactSpec| {
        json!({
            "url":format!("{origin}/windows/artifacts/{}",spec.file), "size":spec.size,
            "sha256":spec.sha256.to_ascii_uppercase(),
        })
    };
    let diff = if parsed_current < version(&release.version)? {
        release
            .differential
            .iter()
            .find(|d| d.from_version == current && d.size < release.full.size)
    } else {
        None
    };
    let mut response = json!({
        "target_version":release.version,"platform":"win","is_diff":diff.is_some(),
        "changelog":release.changelog,"force_update":release.force_update,
        "full_package":package(&release.full), "deletions":[],
    });
    if let Some(diff) = diff {
        response["diff_package"] = package(&diff.spec());
        response["deletions"] = json!(diff.deletions);
    }
    Ok(response)
}

fn tree_path(root: &Path, entry: &History) -> PathBuf {
    root.join("history")
        .join(&entry.arch)
        .join(format!("{}-{}", entry.version, entry.tree_sha256))
        .join("application")
}

fn tree_files(root: &Path) -> Result<Vec<(String, PathBuf, u64)>> {
    fn walk(
        root: &Path,
        at: &Path,
        out: &mut Vec<(String, PathBuf, u64)>,
        total: &mut u64,
    ) -> Result<()> {
        let mut entries = fs::read_dir(at)?.collect::<std::io::Result<Vec<_>>>()?;
        entries.sort_by_key(|entry| entry.file_name());
        for entry in entries {
            let path = entry.path();
            let metadata = fs::symlink_metadata(&path)?;
            reject_reparse(&metadata)?;
            ensure!(!metadata.file_type().is_symlink(), "Baseline links are not supported");
            if metadata.is_dir() {
                walk(root, &path, out, total)?;
            } else {
                ensure!(metadata.is_file(), "Baseline contains a special file");
                let relative = path
                    .strip_prefix(root)?
                    .to_str()
                    .context("Baseline path is not UTF-8")?
                    .replace('\\', "/");
                ensure!(relative_path(&relative), "Invalid baseline path");
                *total = total.checked_add(metadata.len()).context("Baseline size overflow")?;
                ensure!(
                    *total <= 32 * 1024 * 1024 * 1024 && out.len() < 100_000,
                    "Baseline exceeds size/file limits"
                );
                out.push((relative, path, metadata.len()));
            }
        }
        Ok(())
    }
    let metadata = fs::symlink_metadata(root)?;
    ensure!(
        metadata.is_dir() && !metadata.file_type().is_symlink(),
        "Missing application baseline"
    );
    reject_reparse(&metadata)?;
    let mut result = vec![];
    walk(root, root, &mut result, &mut 0)?;
    // Match JavaScript's UTF-16 ordering used by the preparation script.
    result.sort_by(|a, b| a.0.encode_utf16().cmp(b.0.encode_utf16()));
    ensure!(!result.is_empty(), "Application baseline is empty");
    Ok(result)
}

fn tree_hash(root: &Path) -> Result<String> {
    let mut hash = Sha256::new();
    let mut buffer = vec![0; 1024 * 1024];
    for (name, path, size) in tree_files(root)? {
        let mut file_hash = Sha256::new();
        let mut input = File::open(path)?;
        let mut count = 0;
        loop {
            let n = input.read(&mut buffer)?;
            if n == 0 {
                break;
            }
            file_hash.update(&buffer[..n]);
            count += n as u64;
        }
        ensure!(count == size, "Baseline changed during hashing");
        hash.update(serde_json::to_vec(&(name, size, hex::encode(file_hash.finalize())))?);
        hash.update(b"\n");
    }
    Ok(hex::encode(hash.finalize()))
}

fn archive_tree(root: &Path, source: &Path, entry: &History) -> Result<()> {
    let target = tree_path(root, entry);
    if target.try_exists()? {
        ensure!(tree_hash(&target)? == entry.tree_sha256, "Archived Windows baseline changed");
        return Ok(());
    }
    let route = target.parent().unwrap().parent().unwrap();
    real_directory(route)?;
    let staging = tempfile::tempdir_in(route)?;
    let application = staging.path().join("application");
    real_directory(&application)?;
    for (relative, input, _) in tree_files(source)? {
        let output = application.join(relative);
        fs::create_dir_all(output.parent().unwrap())?;
        fs::copy(input, &output)?;
        OpenOptions::new().write(true).open(output)?.sync_all()?;
    }
    ensure!(
        tree_hash(&application)? == entry.tree_sha256,
        "Windows baseline changed while archiving"
    );
    fs::rename(staging.path(), target.parent().unwrap())?;
    Ok(())
}

fn publish_artifact(
    root: &Path,
    source: &Path,
    spec: &catalog::ArtifactSpec,
) -> Result<catalog::ArtifactSpec> {
    validate_spec(spec)?;
    catalog::load_artifact(source, spec.clone(), "zip")?;
    let published = catalog::ArtifactSpec {
        file: format!("{}.zip", spec.sha256),
        size: spec.size,
        sha256: spec.sha256.clone(),
    };
    let directory = root.join("artifacts");
    real_directory(&directory)?;
    let destination = directory.join(&published.file);
    if !destination.try_exists()? {
        let mut temporary = tempfile::NamedTempFile::new_in(&directory)?;
        let mut input = File::open(source.join(&spec.file))?;
        ensure!(
            std::io::copy(&mut input, &mut temporary)? == spec.size,
            "Windows artifact changed while copying"
        );
        temporary.as_file().sync_all()?;
        // Verify the copied bytes before making the immutable name visible.
        ensure!(
            crate::delta::sha256(temporary.path())? == spec.sha256,
            "Windows artifact copy hash mismatch"
        );
        temporary.persist_noclobber(&destination)?;
    }
    catalog::load_artifact(&directory, published.clone(), "zip")?;
    Ok(published)
}

fn add_artifact(doc: &mut Document, spec: catalog::ArtifactSpec) -> Result<()> {
    if let Some(old) = doc.artifacts.iter().find(|old| old.file == spec.file) {
        ensure!(equal_spec(old, &spec), "Cannot replace an immutable Windows artifact");
    } else {
        doc.artifacts.push(spec);
    }
    Ok(())
}

fn read_prepared(root: &Path) -> Result<Prepared> {
    let path = root.join("release.json");
    regular(&path, MAX_DOCUMENT)?;
    let prepared: Prepared = serde_json::from_slice(&fs::read(path)?)?;
    version(&prepared.version)?;
    validate_arch(&prepared.arch)?;
    validate_spec(&prepared.full)?;
    validate_diffs(&prepared.differential, &prepared.version)?;
    ensure!(prepared.warnings.len() <= 128, "Too many preparation warnings");
    catalog::load_artifact(root, prepared.full.clone(), "zip")?;
    for diff in &prepared.differential {
        catalog::load_artifact(root, diff.spec(), "zip")?;
    }
    Ok(prepared)
}

fn archive_prepared(
    root: &Path,
    source: &Path,
    prepared: &Prepared,
    doc: &mut Document,
) -> Result<History> {
    let fingerprint = tree_hash(&source.join("application"))?;
    if let Some(expected) = &prepared.tree_sha256 {
        ensure!(expected == &fingerprint, "Prepared Windows tree fingerprint mismatch");
    }
    if let Some(existing) = doc
        .history
        .iter()
        .find(|entry| entry.version == prepared.version && entry.arch == prepared.arch)
    {
        ensure!(
            existing.tree_sha256 == fingerprint,
            "同版本、同架构已存在不同的 Windows 安装内容，请使用新版本号"
        );
        // ZIP timestamps may differ on a repeated import. Preserve its published bytes.
        archive_tree(root, &source.join("application"), existing)?;
        return Ok(existing.clone());
    }
    ensure!(doc.history.len() < MAX_HISTORY, "Windows 历史版本数量超过限制");
    let full = publish_artifact(root, source, &prepared.full)?;
    let entry = History {
        version: prepared.version.clone(),
        arch: prepared.arch.clone(),
        full,
        tree_sha256: fingerprint,
    };
    archive_tree(root, &source.join("application"), &entry)?;
    add_artifact(doc, entry.full.clone())?;
    doc.history.push(entry.clone());
    Ok(entry)
}

fn commit_release(
    root: &Path,
    source: &Path,
    prepared: &Prepared,
    archived: &History,
    mut doc: Document,
    settings: &Settings,
) -> Result<()> {
    ensure!(
        prepared.version == archived.version && prepared.arch == archived.arch,
        "Prepared Windows release identity mismatch"
    );
    ensure!(
        tree_hash(&source.join("application"))? == archived.tree_sha256,
        "Prepared Windows release changed its archived contents"
    );
    let mut differential = vec![];
    for diff in &prepared.differential {
        if diff.size >= archived.full.size {
            continue;
        }
        ensure!(
            doc.history
                .iter()
                .any(|base| base.arch == archived.arch && base.version == diff.from_version),
            "Prepared Windows delta has no archived baseline"
        );
        let spec = publish_artifact(root, source, &diff.spec())?;
        let mut published = diff.clone();
        published.file = spec.file.clone();
        add_artifact(&mut doc, spec)?;
        differential.push(published);
    }
    doc.releases.retain(|r| r.arch != archived.arch);
    doc.releases.push(Release {
        version: archived.version.clone(),
        arch: archived.arch.clone(),
        full: archived.full.clone(),
        differential,
        changelog: settings.changelog.clone(),
        force_update: settings.force_update,
    });
    doc.releases.sort_by(|a, b| a.arch.cmp(&b.arch));
    validate_document(&doc)?;
    let bytes = serde_json::to_vec_pretty(&doc)?;
    ensure!(bytes.len() as u64 <= MAX_DOCUMENT, "Windows catalog exceeds limit");
    // Do all validation before committing the only mutable publication record.
    load_document(root, doc, bytes.clone())?;
    let mut temporary = tempfile::NamedTempFile::new_in(root)?;
    temporary.write_all(&bytes)?;
    temporary.write_all(b"\n")?;
    temporary.as_file().sync_all()?;
    temporary.persist(root.join("catalog.json"))?;
    Ok(())
}

fn find_node() -> Option<PathBuf> {
    let paths = std::env::var_os("PATH")?;
    let name = if cfg!(windows) { "node.exe" } else { "node" };
    std::env::split_paths(&paths).map(|p| p.join(name)).find(|p| p.is_file())
}

fn script() -> Result<PathBuf> {
    let exe = std::env::current_exe()?;
    for ancestor in exe.ancestors().skip(1) {
        for relative in ["prepare_windows_release.mjs", "updater/prepare_windows_release.mjs"] {
            let path = ancestor.join(relative);
            if path.is_file() {
                return Ok(path);
            }
        }
    }
    anyhow::bail!("找不到 prepare_windows_release.mjs，请将 Windows 制包脚本及 make_directory_diff.js 放到程序旁边")
}

async fn prepare(
    settings: &Settings,
    input: &Path,
    output: &Path,
    baselines: &Path,
    version: Option<&str>,
    arch: Option<&str>,
) -> Result<()> {
    let node = settings
        .node
        .clone()
        .or_else(find_node)
        .context("找不到 Node.js，请在 Windows 发布配置中设置 Node.js 路径")?;
    let mut command = tokio::process::Command::new(node);
    command
        .arg(script()?)
        .arg("--input")
        .arg(input)
        .arg("--out-dir")
        .arg(output)
        .arg("--baselines")
        .arg(baselines)
        .kill_on_drop(true);
    if let Some(value) = version {
        command.arg("--version").arg(value);
    }
    if let Some(value) = arch {
        command.arg("--arch").arg(value);
    }
    if let Some(value) = &settings.seven_zip {
        command.arg("--7zip").arg(value);
    }
    if let Some(value) = &settings.zucchini {
        command.arg("--zucchini").arg(value);
    }
    #[cfg(windows)]
    command.creation_flags(0x08000000);
    let output = tokio::time::timeout(Duration::from_secs(7200), command.output())
        .await
        .context("Windows 制包超时（2 小时）")??;
    ensure!(
        output.status.success(),
        "Windows 安装包处理失败：{}",
        String::from_utf8_lossy(&output.stderr)
            .chars()
            .rev()
            .take(8000)
            .collect::<String>()
            .chars()
            .rev()
            .collect::<String>()
    );
    Ok(())
}

fn baselines(root: &Path, doc: &Document) -> Vec<Value> {
    doc.history.iter().map(|entry| json!({"version":entry.version,"arch":entry.arch,"tree":config::external_path(&tree_path(root,entry)),"tree_sha256":entry.tree_sha256})).collect()
}

/// Importing an older package adds a baseline and rebuilds the highest release's
/// deltas. No running/installed browser or original installer is modified.
pub async fn publish(
    root: &Path,
    settings: &Settings,
    input: &Path,
    work: &Path,
    wanted_version: Option<&str>,
    wanted_arch: Option<&str>,
) -> Result<Value> {
    settings.validate()?;
    if let Some(value) = wanted_version {
        version(value)?;
    }
    if let Some(value) = wanted_arch {
        validate_arch(value)?;
    }
    let root = root.to_owned();
    let open_root = root.clone();
    let (lock, doc) = tokio::task::spawn_blocking(move || -> Result<_> {
        real_directory(&open_root)?;
        let lock_path = open_root.join("publish.lock");
        if lock_path.try_exists()? {
            regular(&lock_path, 0)?;
        }
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(lock_path)?;
        lock.try_lock_exclusive().context("另一个 Windows 发布任务正在处理")?;
        let (doc, _) = read_document(&open_root)?;
        Ok((lock, doc))
    })
    .await??;
    let baseline_path = work.join("windows-baselines.json");
    tokio::fs::write(&baseline_path, serde_json::to_vec(&baselines(&root, &doc))?).await?;
    let incoming_path = work.join("windows-incoming");
    prepare(settings, input, &incoming_path, &baseline_path, wanted_version, wanted_arch).await?;
    let read_root = root.clone();
    let read_path = incoming_path.clone();
    let (mut doc, incoming, archived) = tokio::task::spawn_blocking(move || -> Result<_> {
        let mut doc = doc;
        let incoming = read_prepared(&read_path)?;
        let archived = archive_prepared(&read_root, &read_path, &incoming, &mut doc)?;
        Ok((doc, incoming, archived))
    })
    .await??;
    let latest = doc
        .history
        .iter()
        .filter(|entry| entry.arch == archived.arch)
        .max_by_key(|entry| version(&entry.version).unwrap())
        .unwrap()
        .clone();
    let mut warnings = incoming.warnings.clone();
    let (prepared, source) = if latest.version == archived.version {
        (incoming, incoming_path)
    } else {
        tokio::fs::write(&baseline_path, serde_json::to_vec(&baselines(&root, &doc))?).await?;
        let refreshed_path = work.join("windows-latest");
        prepare(
            settings,
            &root.join("artifacts").join(&latest.full.file),
            &refreshed_path,
            &baseline_path,
            Some(&latest.version),
            Some(&latest.arch),
        )
        .await?;
        let read_path = refreshed_path.clone();
        let refreshed = tokio::task::spawn_blocking(move || read_prepared(&read_path)).await??;
        warnings.extend(refreshed.warnings.clone());
        (refreshed, refreshed_path)
    };
    let release_version = latest.version.clone();
    let published_arch = latest.arch.clone();
    let imported_version = archived.version.clone();
    let history_count = doc.history.len();
    // Keep the lock alive through validation and the atomic catalog replacement.
    let settings = settings.clone();
    let commit_root = root.clone();
    tokio::task::spawn_blocking(move || {
        let _lock = lock;
        commit_release(
            &commit_root,
            &source,
            &prepared,
            &latest,
            std::mem::take(&mut doc),
            &settings,
        )
    })
    .await??;
    Ok(json!({"platform":"win","arch":published_arch,"imported_version":imported_version,
        "version":release_version,"history_count":history_count,"warnings":warnings,
        "endpoint":"/api/v1/update/check","protocol":"json+zip"}))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec(file: &str, size: u64) -> catalog::ArtifactSpec {
        catalog::ArtifactSpec { file: file.into(), size, sha256: "ab".repeat(32) }
    }
    fn catalog() -> Catalog {
        Catalog {
            releases: vec![Release {
                version: "2.0.0.0".into(),
                arch: "x86_64".into(),
                full: spec("full.zip", 200),
                differential: vec![Differential {
                    from_version: "1.0.0.0".into(),
                    file: "patch.zip".into(),
                    size: 40,
                    sha256: "cd".repeat(32),
                    deletions: vec!["1.0.0.0/obsolete.dat".into()],
                }],
                changelog: "Windows 更新".into(),
                force_update: false,
            }],
            history: vec![],
            artifacts: HashMap::new(),
            manifest_bytes: vec![],
        }
    }

    #[test]
    fn json_selects_exact_baseline_and_keeps_full_fallback() {
        let catalog = catalog();
        let response = build_response(
            &catalog,
            "platform=win&version=1.0.0.0",
            "https://updates.example",
            "x86_64",
        )
        .unwrap();
        assert_eq!(response["is_diff"], true);
        assert_eq!(response["deletions"], json!(["1.0.0.0/obsolete.dat"]));
        assert_eq!(
            response["full_package"]["url"],
            "https://updates.example/windows/artifacts/full.zip"
        );
        assert_eq!(response["diff_package"]["sha256"], "CD".repeat(32));
        for current in ["0.9.0.0", "1.0.0.1", "2.0.0.0", "3.0.0.0"] {
            let response = build_response(
                &catalog,
                &format!("version={current}"),
                "https://updates.example",
                "x86_64",
            )
            .unwrap();
            assert_eq!(response["is_diff"], false);
            assert!(response.get("diff_package").is_none());
            assert_eq!(response["deletions"], json!([]));
            assert_eq!(response["target_version"], "2.0.0.0");
        }
    }

    #[test]
    fn requests_never_fall_back_across_architectures() {
        let catalog = catalog();
        for query in [
            "platform=mac",
            "arch=arm64",
            "arch=x86",
            "arch=unknown",
            "version=1.0.0.0&version=2.0.0.0",
            "version=01.0.0.0",
        ] {
            assert!(
                build_response(&catalog, query, "https://updates.example", "x86_64").is_err(),
                "{query}"
            );
        }
        assert!(build_response(
            &catalog,
            "arch=x64&version=1.0.0.0",
            "https://updates.example",
            "arm64"
        )
        .is_ok());
        assert!(build_response(&catalog, "", "https://updates.example", "arm64").is_err());
    }

    fn prepared(root: &Path, version: &str, contents: &[u8]) -> Prepared {
        fs::create_dir_all(root.join("application")).unwrap();
        fs::write(root.join("application/payload"), contents).unwrap();
        let mut zip = b"PK\x03\x04fixture-zip-contents".to_vec();
        zip.extend_from_slice(contents);
        fs::write(root.join("package.zip"), &zip).unwrap();
        Prepared {
            version: version.into(),
            arch: "x86_64".into(),
            full: catalog::ArtifactSpec {
                file: "package.zip".into(),
                size: zip.len() as u64,
                sha256: hex::encode(Sha256::digest(zip)),
            },
            differential: vec![],
            warnings: vec![],
            tree_sha256: None,
        }
    }

    #[test]
    fn publication_is_atomic_reimport_is_idempotent_and_conflicts_are_rejected() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("published");
        fs::create_dir(&root).unwrap();
        let source = temp.path().join("input");
        let prepared = prepared(&source, "2.0.0.0", b"original");
        let mut doc = Document::default();
        let entry = archive_prepared(&root, &source, &prepared, &mut doc).unwrap();
        commit_release(&root, &source, &prepared, &entry, doc.clone(), &Settings::default())
            .unwrap();
        let before = fs::read(root.join("catalog.json")).unwrap();
        assert_eq!(load(&root).unwrap().artifacts.len(), 1);
        assert_eq!(list(&root).unwrap()[0]["version"], "2.0.0.0");
        let again = archive_prepared(&root, &source, &prepared, &mut doc).unwrap();
        assert!(equal_spec(&again.full, &entry.full));
        assert_eq!(doc.history.len(), 1);
        fs::write(source.join("application/payload"), b"different bytes").unwrap();
        assert!(archive_prepared(&root, &source, &prepared, &mut doc).is_err());
        assert_eq!(fs::read(root.join("catalog.json")).unwrap(), before);
        assert!(
            commit_release(&root, &source, &prepared, &entry, doc, &Settings::default()).is_err()
        );
        assert_eq!(fs::read(root.join("catalog.json")).unwrap(), before);
    }

    #[test]
    fn lower_history_cannot_replace_latest_and_artifact_tampering_is_detected() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("published");
        fs::create_dir(&root).unwrap();
        let newest_path = temp.path().join("newest");
        let newest = prepared(&newest_path, "3.0.0.0", b"newest");
        let mut doc = Document::default();
        let entry = archive_prepared(&root, &newest_path, &newest, &mut doc).unwrap();
        commit_release(&root, &newest_path, &newest, &entry, doc, &Settings::default()).unwrap();
        let (mut doc, _) = read_document(&root).unwrap();
        let older_path = temp.path().join("older");
        let older = prepared(&older_path, "1.0.0.0", b"older");
        let older_entry = archive_prepared(&root, &older_path, &older, &mut doc).unwrap();
        let before = fs::read(root.join("catalog.json")).unwrap();
        assert!(commit_release(
            &root,
            &older_path,
            &older,
            &older_entry,
            doc.clone(),
            &Settings::default()
        )
        .is_err());
        assert_eq!(fs::read(root.join("catalog.json")).unwrap(), before);
        commit_release(&root, &newest_path, &newest, &entry, doc, &Settings::default()).unwrap();
        assert_eq!(load(&root).unwrap().artifacts.len(), 2);
        fs::write(root.join("artifacts").join(&older_entry.full.file), b"tampered").unwrap();
        assert!(load(&root).is_err());
    }

    #[test]
    fn unsafe_or_ambiguous_metadata_cannot_be_committed() {
        assert!(!relative_path("../outside"));
        assert!(!relative_path("C:/absolute"));
        assert!(!relative_path("one\\two"));
        assert!(!relative_path("/absolute"));
        let mut doc = Document::default();
        doc.artifacts.push(spec("../outside.zip", 10));
        assert!(validate_document(&doc).is_err());
        let settings = Settings { default_arch: "other".into(), ..Settings::default() };
        assert!(settings.validate().is_err());
    }
}
