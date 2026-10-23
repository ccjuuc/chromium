// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Local browser publishing. Installers are staged, never executed.
use crate::{
    config::{self, Config, Packaging},
    history,
    server::ServerState,
};
use anyhow::{ensure, Context, Result};
use axum::{
    body::{to_bytes, Body},
    extract::{FromRequest, Multipart, Request},
    http::{header, Method, StatusCode},
    response::Response,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration,
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    sync::Mutex,
};

pub const MAX_INSTALLER: u64 = 8 * 1024 * 1024 * 1024;
const MAX_JSON: usize = 64 * 1024;

#[derive(Clone, Serialize)]
pub struct Job {
    id: String,
    platform: String,
    status: String,
    stage: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    result: Option<Value>,
}

struct Inner {
    config: Config,
    job: Option<Job>,
}

pub struct Admin {
    token: String,
    config_path: PathBuf,
    root: PathBuf,
    inner: Mutex<Inner>,
}

impl Admin {
    pub fn new(config_path: &Path, mut config: Config) -> Result<Arc<Self>> {
        let config_path = config::external_path(&fs::canonicalize(config_path)?);
        let root = config_path.parent().context("Config has no directory")?.join("publisher");
        if config.packaging.tools_dir.is_none() {
            config.packaging.tools_dir =
                config.puffin.as_ref().and_then(|p| p.parent()).map(Path::to_owned);
        }
        Ok(Arc::new(Self {
            token: random_id()?,
            config_path,
            root,
            inner: Mutex::new(Inner { config, job: None }),
        }))
    }

    pub fn token(&self) -> &str {
        &self.token
    }

    pub fn windows_root(&self) -> PathBuf {
        self.root.join("windows")
    }

    pub async fn windows_default_arch(&self) -> String {
        self.inner.lock().await.config.windows.default_arch.clone()
    }

    async fn begin(&self, platform: &str) -> Result<Job> {
        let mut inner = self.inner.lock().await;
        ensure!(!is_busy(&inner.job), "已有发布任务正在处理，请等待完成");
        let job = Job {
            id: random_id()?,
            platform: platform.to_owned(),
            status: "queued".into(),
            stage: "准备接收安装包".into(),
            error: None,
            result: None,
        };
        inner.job = Some(job.clone());
        Ok(job)
    }

    async fn stage(&self, stage: &str) {
        if let Some(job) = &mut self.inner.lock().await.job {
            job.status = "running".into();
            job.stage = stage.into();
        }
    }

    async fn finish(&self, result: Result<Value>) {
        if let Some(job) = &mut self.inner.lock().await.job {
            match result {
                Ok(result) => {
                    job.status = "succeeded".into();
                    job.stage = "发布完成".into();
                    job.result = Some(result);
                }
                Err(error) => {
                    job.status = "failed".into();
                    job.stage = "发布失败".into();
                    job.error = Some(format!("{error:#}"));
                }
            }
        }
    }

    async fn state(&self, server: &ServerState) -> Result<Value> {
        let inner = self.inner.lock().await;
        let config = &inner.config;
        let path = self.root.join("history");
        let entries = tokio::task::spawn_blocking(move || history::list(&path)).await??;
        let mut windows = server.windows_summary().await;
        windows["config"] = serde_json::to_value(&config.windows)?;
        windows["endpoint"] = json!(format!("{}/api/v1/update/check", server.origin()));
        Ok(json!({ "enabled": true, "busy": is_busy(&inner.job), "job": inner.job,
            "config": {"key_path": config.packaging.key_path, "key_hash": config.crx_public_key_sha256,
                "tools_dir": config.packaging.tools_dir, "python": config.packaging.python,
                "openssl": config.packaging.openssl,
                "team_id": config.packaging.team_id, "development": config.packaging.development,
                "host": std::env::consts::OS}, "history": entries, "windows": windows,
                "max_upload_bytes": MAX_INSTALLER }))
    }

    async fn save_windows_settings(
        &self,
        mut settings: crate::windows::Settings,
        server: &ServerState,
    ) -> Result<Value> {
        settings.validate()?;
        let mut inner = self.inner.lock().await;
        ensure!(!is_busy(&inner.job), "任务进行中，暂时不能修改配置");
        for path in [&mut settings.node, &mut settings.seven_zip, &mut settings.zucchini]
            .into_iter()
            .flatten()
        {
            if path.is_relative() {
                *path = self.config_path.parent().unwrap().join(&*path);
            }
            ensure!(path.is_file(), "工具路径不存在或不是文件：{}", path.display());
            *path = config::external_path(&fs::canonicalize(&*path)?);
        }
        let mut next = inner.config.clone();
        next.windows = settings;
        persist_config(&self.config_path, &next).await?;
        server.set_windows_default_arch(next.windows.default_arch.clone()).await;
        inner.config = next;
        Ok(json!({"saved":true}))
    }

    async fn save_settings(&self, value: Value) -> Result<Value> {
        let settings: Settings = serde_json::from_value(value)?;
        let mut inner = self.inner.lock().await;
        ensure!(!is_busy(&inner.job), "任务进行中，暂时不能修改配置");
        let root = self.config_path.parent().unwrap();
        let resolve = |input: Option<String>, directory: bool| -> Result<Option<PathBuf>> {
            let Some(input) = input.filter(|s| !s.trim().is_empty()) else { return Ok(None) };
            let path = PathBuf::from(input.trim());
            let path = if path.is_relative() { root.join(path) } else { path };
            ensure!(
                if directory { path.is_dir() } else { path.is_file() },
                "路径不存在或类型不正确：{}",
                path.display()
            );
            Ok(Some(config::external_path(&fs::canonicalize(path)?)))
        };
        let mut next = inner.config.clone();
        next.packaging = Packaging {
            key_path: resolve(settings.key_path, false)?,
            tools_dir: resolve(settings.tools_dir, true)?,
            python: resolve(settings.python, false)?,
            openssl: resolve(settings.openssl, false)?,
            team_id: settings.team_id.filter(|s| !s.is_empty()),
            development: settings.development,
        };
        if let Some(team) = &next.packaging.team_id {
            ensure!(
                team.len() == 10 && team.bytes().all(|b| b.is_ascii_alphanumeric()),
                "Apple Team ID 格式不正确"
            );
        }
        next.crx_public_key_sha256 = settings
            .key_hash
            .filter(|s| !s.trim().is_empty())
            .map(|s| config::normalize_hash(s.trim()))
            .transpose()?;
        if next.crx_public_key_sha256.is_none() {
            if let Some(key) = next.packaging.key_path.clone() {
                let openssl = next.packaging.openssl.clone();
                next.crx_public_key_sha256 = Some(
                    tokio::task::spawn_blocking(move || key_hash(&key, openssl.as_deref()))
                        .await??,
                );
            }
        }
        persist_config(&self.config_path, &next).await?;
        inner.config = next;
        Ok(json!({"saved":true}))
    }

    async fn process(
        self: Arc<Self>,
        source: Source,
        work: tempfile::TempDir,
        state: ServerState,
        platform: &str,
    ) {
        let result = if platform == "win" {
            self.process_windows(source, &work, &state).await
        } else {
            self.process_inner(source, &work, &state).await
        };
        self.finish(result).await;
    }

    async fn process_windows(
        &self,
        source: Source,
        work: &tempfile::TempDir,
        state: &ServerState,
    ) -> Result<Value> {
        let settings = self.inner.lock().await.config.windows.clone();
        self.stage("正在准备 Windows 安装包").await;
        let (input, version, arch) = self.stage_source(source, work, "win").await?;
        self.stage("正在解包、生成 ZIP 和历史版本差分并回放校验").await;
        let root = self.windows_root();
        let result = crate::windows::publish(
            &root,
            &settings,
            &input,
            work.path(),
            version.as_deref().filter(|s| !s.is_empty()),
            arch.as_deref().filter(|s| !s.is_empty()),
        )
        .await?;
        self.stage("正在加载 Windows 更新清单").await;
        let current = tokio::task::spawn_blocking(move || crate::windows::load(&root)).await??;
        state.replace_windows_catalog(current).await?;
        Ok(result)
    }

    async fn stage_source(
        &self,
        source: Source,
        work: &tempfile::TempDir,
        platform: &str,
    ) -> Result<(PathBuf, Option<String>, Option<String>)> {
        match source {
            Source::Uploaded { path, version, arch } => Ok((path, version, arch)),
            Source::Import(request) => {
                let extension =
                    input_extension(&request.source, request.source_type == "url", platform)?;
                let input = work.path().join(format!("installer.{extension}"));
                match request.source_type.as_str() {
                    "path" => copy_input(Path::new(&request.source), &input).await?,
                    "url" => {
                        self.stage("正在下载安装包").await;
                        download(&request.source, &input).await?;
                    }
                    _ => anyhow::bail!("来源只支持 path 或 url"),
                }
                Ok((input, request.version, request.arch))
            }
        }
    }

    async fn process_inner(
        &self,
        source: Source,
        work: &tempfile::TempDir,
        state: &ServerState,
    ) -> Result<Value> {
        let cfg = self.inner.lock().await.config.clone();
        let key =
            cfg.packaging.key_path.as_ref().context("请先在发布配置中设置 CRX 签名私钥路径")?;
        let pin = cfg
            .crx_public_key_sha256
            .as_ref()
            .context("请先配置 CRX 公钥摘要，或重新保存私钥路径以自动计算")?;
        let tools = cfg
            .packaging
            .tools_dir
            .as_ref()
            .context("请配置原生工具目录（crx3_build_action、xenon_updater_verify、puffin）")?;
        self.stage("正在准备安装包").await;
        let (input, version, arch) = self.stage_source(source, work, "mac").await?;
        validate_options(version.as_deref(), arch.as_deref())?;
        self.stage("正在读取安装包、转换并验证签名 CRX3").await;
        let script = prepare_script()?;
        let python = cfg
            .packaging
            .python
            .clone()
            .or_else(find_python)
            .context("找不到 Python 3，请在发布配置中填写 Python 路径")?;
        let mut command = tokio::process::Command::new(python);
        command
            .arg(&script)
            .arg("--input")
            .arg(&input)
            .arg("--out-dir")
            .arg(work.path().join("release"))
            .arg("--tools-dir")
            .arg(tools)
            .arg("--key")
            .arg(key)
            .arg("--expected-key-sha256")
            .arg(pin)
            .current_dir(work.path())
            .kill_on_drop(true);
        if let Some(path) = &cfg.packaging.openssl {
            command.arg("--openssl").arg(path);
        }
        if let Some(version) = version.filter(|s| !s.is_empty()) {
            command.arg("--version").arg(version);
        }
        if let Some(arch) = arch.filter(|s| !s.is_empty()) {
            command.arg("--arch").arg(arch);
        }
        if let Some(team) = &cfg.packaging.team_id {
            command.arg("--team-id").arg(team);
        }
        if cfg.packaging.development {
            command.arg("--development");
        }
        #[cfg(windows)]
        command.creation_flags(0x08000000);
        let output = tokio::time::timeout(Duration::from_secs(7200), command.output())
            .await
            .context("制包超时（2 小时）")??;
        let stderr = String::from_utf8_lossy(&output.stderr);
        ensure!(output.status.success(), "安装包处理失败：{}", tail(&stderr, 8000));
        let manifest = work.path().join("release/release.json");
        ensure!(manifest.is_file(), "制包没有生成 release.json");
        self.stage("正在生成历史版本差分并发布更新配置").await;
        let history = self.root.join("history");
        let served = cfg.manifest.clone();
        let puffin =
            config::find_puffin(None, cfg.puffin.as_deref(), cfg.packaging.tools_dir.as_deref())
                .ok()
                .flatten();
        let result = tokio::task::spawn_blocking(move || {
            history::import_release(&history::ImportOptions {
                history,
                manifest: served,
                release: manifest,
                puffin,
            })
        })
        .await??;
        let current =
            tokio::task::spawn_blocking(move || crate::catalog::load(&cfg.manifest)).await??;
        state.replace_catalog(current).await?;
        Ok(result)
    }
}

fn random_id() -> Result<String> {
    let mut bytes = [0; 32];
    getrandom::fill(&mut bytes).map_err(|error| anyhow::anyhow!("Random generator: {error}"))?;
    Ok(hex::encode(bytes))
}

async fn persist_config(path: &Path, next: &Config) -> Result<()> {
    let bytes = serde_json::to_vec_pretty(next)?;
    let path = path.to_owned();
    tokio::task::spawn_blocking(move || -> Result<()> {
        let mut temp = tempfile::NamedTempFile::new_in(path.parent().unwrap())?;
        temp.write_all(&bytes)?;
        temp.write_all(b"\n")?;
        temp.as_file().sync_all()?;
        temp.persist(&path)?;
        Ok(())
    })
    .await??;
    Ok(())
}

fn is_busy(job: &Option<Job>) -> bool {
    job.as_ref().is_some_and(|j| j.status == "queued" || j.status == "running")
}

fn tail(text: &str, limit: usize) -> String {
    text.chars().rev().take(limit).collect::<String>().chars().rev().collect()
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Settings {
    key_path: Option<String>,
    key_hash: Option<String>,
    tools_dir: Option<String>,
    python: Option<String>,
    openssl: Option<String>,
    team_id: Option<String>,
    #[serde(default)]
    development: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ImportRequest {
    source_type: String,
    source: String,
    version: Option<String>,
    arch: Option<String>,
}

enum Source {
    Import(ImportRequest),
    Uploaded { path: PathBuf, version: Option<String>, arch: Option<String> },
}

fn validate_options(version: Option<&str>, arch: Option<&str>) -> Result<()> {
    if let Some(version) = version.filter(|s| !s.is_empty()) {
        crate::catalog::parse_version(version)?;
    }
    ensure!(
        arch.is_none_or(|arch| matches!(arch, "" | "x86_64" | "arm64")),
        "架构只支持 x86_64 或 arm64"
    );
    Ok(())
}

#[cfg(test)]
fn source_extension(source: &str, is_url: bool) -> Result<String> {
    input_extension(source, is_url, "mac")
}

fn input_extension(source: &str, is_url: bool, platform: &str) -> Result<String> {
    let path = if is_url { url::Url::parse(source)?.path().to_owned() } else { source.to_owned() };
    let extension =
        Path::new(&path).extension().and_then(|p| p.to_str()).unwrap_or("").to_ascii_lowercase();
    if platform == "win" {
        ensure!(
            matches!(extension.as_str(), "exe" | "zip"),
            "Windows 发布只接受 .exe 安装包或安装目录 .zip（链接路径也需要对应扩展名）"
        );
    } else {
        ensure!(extension == "pkg", "macOS 发布只接受 .pkg 安装包；Windows 请使用 Windows 面板（链接路径也需要 .pkg 扩展名）");
    }
    Ok(extension)
}

fn validate_input_options(version: Option<&str>, arch: Option<&str>, platform: &str) -> Result<()> {
    if platform != "win" {
        return validate_options(version, arch);
    }
    if let Some(version) = version.filter(|s| !s.is_empty()) {
        crate::catalog::parse_version(version)?;
        ensure!(version.split('.').count() == 4, "Windows 版本必须为四段数字，例如 1.0.0.2");
    }
    ensure!(
        arch.is_none_or(|arch| matches!(arch, "" | "x86" | "x86_64" | "arm64")),
        "Windows 架构只支持 x86、x86_64 或 arm64"
    );
    Ok(())
}

fn valid_download(url: &url::Url) -> bool {
    url.username().is_empty()
        && url.password().is_none()
        && url.fragment().is_none()
        && (url.scheme() == "https"
            || (url.scheme() == "http"
                && matches!(url.host_str(), Some("127.0.0.1" | "localhost" | "[::1]"))))
}

async fn download(source: &str, output: &Path) -> Result<()> {
    let url = url::Url::parse(source)?;
    ensure!(
        valid_download(&url),
        "安装包链接必须使用 HTTPS（本机测试允许 HTTP）且不能包含账号或片段"
    );
    let client = reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(30))
        .timeout(Duration::from_secs(3600))
        .redirect(reqwest::redirect::Policy::custom(|attempt| {
            if attempt.previous().len() >= 5 || !valid_download(attempt.url()) {
                attempt.error("Unsafe or excessive redirect")
            } else {
                attempt.follow()
            }
        }))
        .build()?;
    let mut response = client.get(url).send().await?.error_for_status()?;
    ensure!(response.content_length().is_none_or(|n| n <= MAX_INSTALLER), "安装包超过 8 GiB 限制");
    let mut file = tokio::fs::OpenOptions::new().write(true).create_new(true).open(output).await?;
    let mut size = 0;
    while let Some(chunk) = response.chunk().await? {
        size += chunk.len() as u64;
        ensure!(size <= MAX_INSTALLER, "安装包超过 8 GiB 限制");
        file.write_all(&chunk).await?;
    }
    ensure!(size > 0, "安装包内容为空");
    file.sync_all().await?;
    Ok(())
}

async fn copy_input(source: &Path, output: &Path) -> Result<()> {
    let meta = tokio::fs::symlink_metadata(source).await?;
    ensure!(
        meta.is_file()
            && !meta.file_type().is_symlink()
            && meta.len() > 0
            && meta.len() <= MAX_INSTALLER,
        "输入必须是 8 GiB 以内的普通安装包文件"
    );
    let mut input = tokio::fs::File::open(source).await?;
    let mut out = tokio::fs::OpenOptions::new().write(true).create_new(true).open(output).await?;
    let mut buffer = vec![0; 1024 * 1024];
    let mut size = 0;
    loop {
        let count = input.read(&mut buffer).await?;
        if count == 0 {
            break;
        }
        size += count as u64;
        ensure!(size <= MAX_INSTALLER, "安装包超过 8 GiB 限制");
        out.write_all(&buffer[..count]).await?;
    }
    let after = tokio::fs::metadata(source).await?;
    ensure!(
        size == meta.len() && after.len() == meta.len() && after.modified()? == meta.modified()?,
        "复制期间输入文件发生变化"
    );
    out.sync_all().await?;
    Ok(())
}

fn on_path(name: &str) -> Option<PathBuf> {
    let paths = std::env::var_os("PATH")?;
    std::env::split_paths(&paths).map(|dir| dir.join(name)).find(|path| path.is_file())
}

fn find_python() -> Option<PathBuf> {
    for name in if cfg!(windows) { ["python3.exe", "python.exe"] } else { ["python3", "python"] } {
        if let Some(path) = on_path(name) {
            if !path.to_string_lossy().contains("WindowsApps") {
                return Some(path);
            }
        }
    }
    None
}

fn prepare_script() -> Result<PathBuf> {
    let exe = std::env::current_exe()?;
    for ancestor in exe.ancestors().skip(1) {
        for relative in ["prepare_installer.py", "updater/prepare_installer.py"] {
            let candidate = ancestor.join(relative);
            if candidate.is_file() {
                return Ok(candidate);
            }
        }
    }
    anyhow::bail!("找不到 prepare_installer.py；部署时请将 updater 制包脚本目录放到程序旁边")
}

fn key_hash(key: &Path, configured: Option<&Path>) -> Result<String> {
    use base64::Engine;
    use sha2::Digest;
    let tool = configured
        .map(Path::to_owned)
        .or_else(|| on_path(if cfg!(windows) { "openssl.exe" } else { "openssl" }))
        .or_else(|| {
            let p = PathBuf::from("C:/Program Files/Git/mingw64/bin/openssl.exe");
            p.is_file().then_some(p)
        })
        .context("自动计算公钥摘要需要 OpenSSL，请设置 OpenSSL 路径")?;
    let mut command = std::process::Command::new(tool);
    command
        .args(["pkey", "-inform", "DER", "-in"])
        .arg(config::external_path(key))
        .args(["-pubout", "-outform", "DER"]);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let result = command.output()?;
    ensure!(
        result.status.success(),
        "无法读取 RSA DER 私钥：{}",
        tail(&String::from_utf8_lossy(&result.stderr), 2000)
    );
    Ok(base64::engine::general_purpose::STANDARD.encode(sha2::Sha256::digest(&result.stdout)))
}

fn json_response(status: StatusCode, value: Value) -> Response {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "application/json; charset=utf-8")
        .header(header::CACHE_CONTROL, "no-store")
        .header("x-content-type-options", "nosniff")
        .body(Body::from(value.to_string()))
        .unwrap()
}

pub async fn handle(admin: Arc<Admin>, state: ServerState, request: Request) -> Response {
    // Host checked by the main router. Same-origin tokens gate every read and mutation.
    let headers = request.headers();
    let origin_ok = headers.get(header::ORIGIN).is_none_or(|value| {
        value.to_str().ok().is_some_and(|origin| {
            origin == format!("http://127.0.0.1:{}", state.port())
                || origin == format!("http://localhost:{}", state.port())
        })
    });
    let browser_site_ok =
        headers.get("sec-fetch-site").is_none_or(|value| value == "same-origin" || value == "none");
    let forwarded =
        ["forwarded", "x-forwarded-for", "x-forwarded-host", "x-forwarded-proto", "x-real-ip"]
            .iter()
            .any(|h| headers.contains_key(*h));
    if !origin_ok
        || !browser_site_ok
        || forwarded
        || headers.get("x-xenon-admin").and_then(|v| v.to_str().ok()) != Some(admin.token())
    {
        return json_response(StatusCode::FORBIDDEN, json!({"error":"只允许本机管理页面访问"}));
    }
    let result = handle_authorized(admin, state, request).await;
    match result {
        Ok((status, value)) => json_response(status, value),
        Err(error) => {
            json_response(StatusCode::BAD_REQUEST, json!({"error": format!("{error:#}")}))
        }
    }
}

async fn handle_authorized(
    admin: Arc<Admin>,
    state: ServerState,
    request: Request,
) -> Result<(StatusCode, Value)> {
    let path = request.uri().path().to_owned();
    if request.method() == Method::GET && path == "/admin/state" {
        return Ok((StatusCode::OK, admin.state(&state).await?));
    }
    ensure!(request.method() == Method::POST, "需要 POST 请求");
    if path == "/admin/config" {
        let body =
            tokio::time::timeout(Duration::from_secs(30), to_bytes(request.into_body(), MAX_JSON))
                .await??;
        return Ok((StatusCode::OK, admin.save_settings(serde_json::from_slice(&body)?).await?));
    }
    if path == "/admin/windows/config" {
        let body =
            tokio::time::timeout(Duration::from_secs(30), to_bytes(request.into_body(), MAX_JSON))
                .await??;
        return Ok((
            StatusCode::OK,
            admin.save_windows_settings(serde_json::from_slice(&body)?, &state).await?,
        ));
    }
    let platform = if path.starts_with("/admin/windows/") { "win" } else { "mac" };
    let import_route = if platform == "win" { "/admin/windows/import" } else { "/admin/import" };
    let upload_route = if platform == "win" { "/admin/windows/upload" } else { "/admin/upload" };
    ensure!(path == import_route || path == upload_route, "未知管理接口");
    let mut upload_request = None;
    let parsed = if path == import_route {
        let body =
            tokio::time::timeout(Duration::from_secs(30), to_bytes(request.into_body(), MAX_JSON))
                .await??;
        let value: ImportRequest = serde_json::from_slice(&body)?;
        ensure!(matches!(value.source_type.as_str(), "path" | "url"), "来源只支持 path 或 url");
        input_extension(&value.source, value.source_type == "url", platform)?;
        validate_input_options(value.version.as_deref(), value.arch.as_deref(), platform)?;
        Some(value)
    } else {
        upload_request = Some(request);
        None
    };
    let job = admin.begin(platform).await?;
    let start = async {
        tokio::fs::create_dir_all(&admin.root).await?;
        let work = tempfile::Builder::new().prefix("job-").tempdir_in(&admin.root)?;
        let source = if let Some(value) = parsed {
            Source::Import(value)
        } else {
            admin.stage("正在接收上传文件").await;
            let upload = tokio::time::timeout(
                Duration::from_secs(3600),
                receive_upload(
                    upload_request.context("Missing upload request")?,
                    work.path(),
                    platform,
                ),
            )
            .await??;
            Source::Uploaded { path: upload.0, version: upload.1, arch: upload.2 }
        };
        Ok::<_, anyhow::Error>((source, work))
    }
    .await;
    match start {
        Ok((source, work)) => {
            tokio::spawn(async move {
                admin.process(source, work, state, platform).await;
            });
            Ok((StatusCode::ACCEPTED, json!({"job":job})))
        }
        Err(error) => {
            let message = format!("{error:#}");
            admin.finish(Err(error)).await;
            anyhow::bail!(message)
        }
    }
}

async fn receive_upload(
    request: Request,
    root: &Path,
    platform: &str,
) -> Result<(PathBuf, Option<String>, Option<String>)> {
    let mut multipart =
        Multipart::from_request(request, &()).await.map_err(|e| anyhow::anyhow!(e.to_string()))?;
    let mut input = None;
    let mut version = None;
    let mut arch = None;
    while let Some(mut field) = multipart.next_field().await? {
        match field.name().unwrap_or("") {
            "file" => {
                ensure!(input.is_none(), "每次只能上传一个安装包");
                let extension = input_extension(
                    field.file_name().context("上传文件缺少文件名")?,
                    false,
                    platform,
                )?;
                let path = root.join(format!("installer.{extension}"));
                let mut file =
                    tokio::fs::OpenOptions::new().create_new(true).write(true).open(&path).await?;
                let mut size = 0;
                while let Some(chunk) = field.chunk().await? {
                    size += chunk.len() as u64;
                    ensure!(size <= MAX_INSTALLER, "安装包超过 8 GiB 限制");
                    file.write_all(&chunk).await?;
                }
                ensure!(size > 0, "上传文件为空");
                file.sync_all().await?;
                input = Some(path);
            }
            "version" | "arch" => {
                let name = field.name().unwrap().to_owned();
                let mut bytes = Vec::new();
                while let Some(chunk) = field.chunk().await? {
                    ensure!(bytes.len() + chunk.len() <= 128, "上传参数太长");
                    bytes.extend_from_slice(&chunk);
                }
                let value = String::from_utf8(bytes)?;
                if name == "version" {
                    ensure!(version.is_none(), "重复版本参数");
                    version = Some(value);
                } else {
                    ensure!(arch.is_none(), "重复架构参数");
                    arch = Some(value);
                }
            }
            _ => anyhow::bail!("不支持的上传字段"),
        }
    }
    validate_input_options(version.as_deref(), arch.as_deref(), platform)?;
    Ok((input.context("请选择安装包文件")?, version, arch))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn downloads_and_input_types_are_explicit() {
        for good in ["https://example.com/application.pkg", "http://127.0.0.1:9000/app.pkg"] {
            assert!(valid_download(&url::Url::parse(good).unwrap()));
            assert!(source_extension(good, true).is_ok());
        }
        for bad in
            ["file:///etc/passwd", "http://example.com/a.exe", "https://u:p@example.com/a.pkg"]
        {
            assert!(!valid_download(&url::Url::parse(bad).unwrap()));
        }
        assert!(source_extension("a.cmd", false).is_err());
        assert!(source_extension("setup.exe", false).is_err());
        assert!(validate_options(Some("1.2.3.4"), Some("x86_64")).is_ok());
        assert!(validate_options(Some("1.2.3.4"), Some("x86")).is_err());
    }
    #[tokio::test]
    async fn upload_streams_to_owned_name_and_checks_fields() {
        let root = tempfile::tempdir().unwrap();
        let body = "--BOUND\r\nContent-Disposition: form-data; name=\"file\"; filename=\"../../application.PKG\"\r\nContent-Type: application/octet-stream\r\n\r\nxar!-fixture\r\n--BOUND\r\nContent-Disposition: form-data; name=\"version\"\r\n\r\n1.2.3.4\r\n--BOUND--\r\n";
        let request = Request::builder()
            .header("content-type", "multipart/form-data; boundary=BOUND")
            .body(Body::from(body))
            .unwrap();
        let (path, version, _) = receive_upload(request, root.path(), "mac").await.unwrap();
        assert_eq!(path, root.path().join("installer.pkg"));
        assert_eq!(fs::read(path).unwrap(), b"xar!-fixture");
        assert_eq!(version.as_deref(), Some("1.2.3.4"));
    }
}
