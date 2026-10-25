// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Loopback Omaha endpoint. TLS is terminated by the deployment proxy.
use crate::catalog::{self, Artifact, Catalog};
use anyhow::{ensure, Context, Result};
use axum::{
    body::{to_bytes, Body},
    extract::{DefaultBodyLimit, Request, State},
    http::{header, HeaderMap, Method, StatusCode},
    response::Response,
    Router,
};
use serde_json::{json, Value};
use std::{collections::HashMap, fs, io::Read, path::PathBuf, sync::Arc, time::Duration};
use tokio::sync::{mpsc, RwLock};

pub const RESPONSE_PREFIX: &str = ")]}'\n";
pub const ENDPOINT: &str = "/service/update2/json";
const MAX_BODY: usize = 1024 * 1024;

struct Active {
    catalog: Arc<Catalog>,
    windows: Option<Arc<crate::windows::Catalog>>,
    windows_default_arch: String,
    // A client may download a URL returned before a manifest reload. Retain
    // those verified handles for this server's lifetime; publish unique names.
    artifacts: HashMap<String, Arc<Artifact>>,
    windows_artifacts: HashMap<String, Arc<Artifact>>,
}

#[derive(Clone)]
pub struct ServerState {
    active: Arc<RwLock<Active>>,
    port: u16,
    origin: String,
    admin: Option<Arc<crate::admin::Admin>>,
    crashes: Option<Arc<crate::crashes::Store>>,
}

pub fn validate_public_origin(value: &str) -> Result<()> {
    let url = url::Url::parse(value)?;
    ensure!(
        url.scheme() == "https"
            && url.origin().ascii_serialization() == value
            && url.username().is_empty()
            && url.password().is_none(),
        "public_origin must be an HTTPS origin without credentials, path, query, or fragment"
    );
    Ok(())
}

impl ServerState {
    pub fn new(catalog: Catalog, port: u16, public_origin: Option<String>) -> Result<Self> {
        if let Some(origin) = &public_origin {
            validate_public_origin(origin)?;
        }
        let artifacts = catalog.artifacts.clone();
        Ok(Self {
            active: Arc::new(RwLock::new(Active {
                catalog: Arc::new(catalog),
                artifacts,
                windows: None,
                windows_default_arch: "x86_64".into(),
                windows_artifacts: HashMap::new(),
            })),
            port,
            origin: public_origin.unwrap_or_else(|| format!("http://127.0.0.1:{port}")),
            admin: None,
            crashes: None,
        })
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn origin(&self) -> &str {
        &self.origin
    }

    pub async fn windows_summary(&self) -> Value {
        let active = self.active.read().await;
        match &active.windows {
            Some(catalog) => json!({"releases": catalog.releases, "history": catalog.history}),
            None => json!({"releases": [], "history": []}),
        }
    }

    pub async fn set_windows_default_arch(&self, arch: String) {
        self.active.write().await.windows_default_arch = arch;
    }

    pub async fn replace_windows_catalog(&self, next: crate::windows::Catalog) -> Result<()> {
        let mut active = self.active.write().await;
        Self::install_windows_catalog(&mut active, next)
    }

    async fn replace_windows_catalog_if_current(
        &self,
        next: crate::windows::Catalog,
        expected: &Option<Arc<crate::windows::Catalog>>,
    ) -> Result<bool> {
        let mut active = self.active.write().await;
        let unchanged = match (&active.windows, expected) {
            (Some(current), Some(expected)) => Arc::ptr_eq(current, expected),
            (None, None) => true,
            _ => false,
        };
        if !unchanged {
            return Ok(false);
        }
        Self::install_windows_catalog(&mut active, next)?;
        Ok(true)
    }

    fn install_windows_catalog(active: &mut Active, next: crate::windows::Catalog) -> Result<()> {
        for (name, artifact) in &next.artifacts {
            if let Some(previous) = active.windows_artifacts.get(name) {
                ensure!(
                    previous.spec.sha256 == artifact.spec.sha256
                        && previous.spec.size == artifact.spec.size,
                    "Cannot replace published Windows artifact {name}"
                );
            }
        }
        active.windows_artifacts.extend(next.artifacts.clone());
        active.windows = Some(Arc::new(next));
        Ok(())
    }

    pub async fn replace_catalog(&self, next: Catalog) -> Result<()> {
        let mut active = self.active.write().await;
        for (name, artifact) in &next.artifacts {
            if let Some(previous) = active.artifacts.get(name) {
                ensure!(
                    previous.spec.sha256 == artifact.spec.sha256
                        && previous.spec.size == artifact.spec.size,
                    "Cannot replace published artifact {name}; use a unique immutable filename"
                );
            }
        }
        active.artifacts.extend(next.artifacts.clone());
        active.catalog = Arc::new(next);
        Ok(())
    }
}

pub fn router(state: ServerState) -> Router {
    Router::new()
        .fallback(handle)
        .layer(DefaultBodyLimit::max(crate::admin::MAX_INSTALLER as usize + 64 * 1024))
        .with_state(state)
}

fn response(status: StatusCode, body: impl Into<Body>, content_type: &str) -> Response {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, content_type)
        .header(header::CACHE_CONTROL, "no-store")
        .header("x-content-type-options", "nosniff")
        .body(body.into())
        .unwrap()
}

fn error(status: StatusCode, message: &str) -> Response {
    response(status, json!({ "error": message }).to_string(), "application/json; charset=utf-8")
}

fn header_text<'a>(headers: &'a HeaderMap, key: &str) -> Option<&'a str> {
    headers.get(key).and_then(|value| value.to_str().ok())
}

fn range_for(header: Option<&str>, size: u64) -> Result<(u64, u64, StatusCode)> {
    let Some(header) = header else {
        return Ok((0, size - 1, StatusCode::OK));
    };
    let value = header.strip_prefix("bytes=").context("Invalid range")?;
    let (first, last) = value.split_once('-').context("Invalid range")?;
    ensure!(
        first.bytes().all(|b| b.is_ascii_digit())
            && last.bytes().all(|b| b.is_ascii_digit())
            && (!first.is_empty() || !last.is_empty()),
        "Invalid range"
    );
    let (start, end) = if first.is_empty() {
        let suffix: u64 = last.parse()?;
        ensure!(suffix > 0, "Invalid range");
        (size.saturating_sub(suffix), size - 1)
    } else {
        (
            first.parse()?,
            if last.is_empty() { size - 1 } else { last.parse::<u64>()?.min(size - 1) },
        )
    };
    ensure!(start <= end && start < size, "Invalid range");
    Ok((start, end, StatusCode::PARTIAL_CONTENT))
}

async fn handle(State(state): State<ServerState>, request: Request) -> Response {
    let method = request.method().clone();
    let path = request.uri().path().to_owned();
    let result = handle_inner(state, request).await;
    tracing::info!(method = %method, path = %path, status = result.status().as_u16(), "request");
    result
}

async fn handle_inner(state: ServerState, request: Request) -> Response {
    let valid_host = match header_text(request.headers(), "host") {
        Some(host) => {
            host == format!("127.0.0.1:{}", state.port)
                || host == format!("localhost:{}", state.port)
        }
        None => false,
    };
    if !valid_host {
        return error(StatusCode::FORBIDDEN, "Loopback native clients only");
    }
    if request.uri().path().starts_with("/admin/") {
        let Some(admin) = state.admin.clone() else {
            return error(StatusCode::NOT_FOUND, "Management is unavailable");
        };
        return crate::admin::handle(admin, state, request).await;
    }
    if request.headers().contains_key(header::ORIGIN) {
        return error(StatusCode::FORBIDDEN, "Loopback native clients only");
    }
    if request.uri().path() == crate::crashes::ENDPOINT {
        let Some(store) = state.crashes else {
            return error(StatusCode::NOT_FOUND, "Crash uploads are disabled");
        };
        return crate::crashes::handle(store, request).await;
    }
    if request.uri().path() == "/assets/home.js" && request.method() == Method::GET {
        return response(StatusCode::OK, include_str!("home.js"), "text/javascript; charset=utf-8");
    }
    if request.uri().path() == "/" {
        if request.method() != Method::GET && request.method() != Method::HEAD {
            return error(StatusCode::METHOD_NOT_ALLOWED, "GET or HEAD required");
        }
        let catalog = state.active.read().await.catalog.clone();
        let forwarded =
            ["forwarded", "x-forwarded-for", "x-forwarded-host", "x-forwarded-proto", "x-real-ip"]
                .iter()
                .any(|name| request.headers().contains_key(*name));
        let token = if forwarded { None } else { state.admin.as_ref().map(|admin| admin.token()) };
        let html = crate::homepage::render(&catalog, &state.origin, token);
        let length = html.len();
        let body = if request.method() == Method::HEAD { String::new() } else { html };
        let mut result = response(StatusCode::OK, body, "text/html; charset=utf-8");
        result.headers_mut().insert(header::CONTENT_LENGTH, length.to_string().parse().unwrap());
        result.headers_mut().insert("content-security-policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'".parse().unwrap());
        result.headers_mut().insert("referrer-policy", "no-referrer".parse().unwrap());
        return result;
    }
    if request.uri().path() == ENDPOINT {
        if request.method() != Method::POST {
            return error(StatusCode::METHOD_NOT_ALLOWED, "POST required");
        }
        if url::form_urlencoded::parse(request.uri().query().unwrap_or("").as_bytes())
            .any(|(key, _)| key == "cup2key" || key == "cup2hreq")
        {
            return error(
                StatusCode::NOT_IMPLEMENTED,
                "CUP signing is not configured; use the Xenon client configuration",
            );
        }
        let content_type = header_text(request.headers(), "content-type")
            .unwrap_or("")
            .split(';')
            .next()
            .unwrap_or("")
            .trim();
        if !content_type.eq_ignore_ascii_case("application/json")
            || request.headers().contains_key(header::CONTENT_ENCODING)
        {
            return error(
                StatusCode::UNSUPPORTED_MEDIA_TYPE,
                "Uncompressed application/json required",
            );
        }
        let bytes = match tokio::time::timeout(
            Duration::from_secs(30),
            to_bytes(request.into_body(), MAX_BODY),
        )
        .await
        {
            Ok(Ok(bytes)) => bytes,
            Ok(Err(_)) => {
                return error(
                    StatusCode::PAYLOAD_TOO_LARGE,
                    "Request body is too large or could not be read",
                )
            }
            Err(_) => return error(StatusCode::REQUEST_TIMEOUT, "Request body timed out"),
        };
        let body: Value = match serde_json::from_slice(&bytes) {
            Ok(body) => body,
            Err(_) => return error(StatusCode::BAD_REQUEST, "Invalid JSON"),
        };
        let catalog = state.active.read().await.catalog.clone();
        return match catalog::build_response(&catalog, &body, &state.origin) {
            Ok(reply) => response(
                StatusCode::OK,
                format!("{RESPONSE_PREFIX}{reply}"),
                "application/json; charset=utf-8",
            ),
            Err(err) => error(StatusCode::BAD_REQUEST, &err.to_string()),
        };
    }
    if request.uri().path() == "/api/v1/update/check" {
        if request.method() != Method::GET {
            return error(StatusCode::METHOD_NOT_ALLOWED, "GET required");
        }
        let active = state.active.read().await;
        let Some(catalog) = &active.windows else {
            return error(StatusCode::SERVICE_UNAVAILABLE, "No Windows release has been published");
        };
        return match crate::windows::build_response(
            catalog,
            request.uri().query().unwrap_or(""),
            &state.origin,
            &active.windows_default_arch,
        ) {
            Ok(value) => {
                response(StatusCode::OK, value.to_string(), "application/json; charset=utf-8")
            }
            Err(error_value) => error(StatusCode::BAD_REQUEST, &error_value.to_string()),
        };
    }
    // Do not decode paths or accept query parameters for artifacts. Files are
    // looked up exclusively in a validated allowlist, never joined to a path.
    let raw = request.uri().path_and_query().map(|pq| pq.as_str()).unwrap_or("");
    let artifact = {
        let active = state.active.read().await;
        if let Some(name) = raw.strip_prefix("/windows/artifacts/") {
            active.windows_artifacts.get(name).cloned()
        } else if let Some(name) = raw.strip_prefix("/artifacts/") {
            active.artifacts.get(name).cloned()
        } else {
            None
        }
    };
    let Some(artifact) = artifact else {
        return error(StatusCode::NOT_FOUND, "Not found");
    };
    if request.method() != Method::GET && request.method() != Method::HEAD {
        return error(StatusCode::METHOD_NOT_ALLOWED, "GET or HEAD required");
    }
    match artifact.unchanged() {
        Ok(true) => {}
        _ => {
            return error(
                StatusCode::CONFLICT,
                "Prepared artifact changed; publish a new immutable artifact",
            )
        }
    }
    let range_header = match request.headers().get(header::RANGE) {
        Some(value) => match value.to_str() {
            Ok(value) => Some(value),
            Err(_) => Some("invalid"),
        },
        None => None,
    };
    let (start, end, status) = match range_for(range_header, artifact.spec.size) {
        Ok(range) => range,
        Err(_) => {
            let mut result = error(StatusCode::RANGE_NOT_SATISFIABLE, "Invalid range");
            result.headers_mut().insert(
                header::CONTENT_RANGE,
                format!("bytes */{}", artifact.spec.size).parse().unwrap(),
            );
            return result;
        }
    };
    let mut result = response(status, Body::empty(), "application/octet-stream");
    let headers = result.headers_mut();
    headers.insert(header::CONTENT_LENGTH, (end - start + 1).to_string().parse().unwrap());
    headers.insert(header::ACCEPT_RANGES, "bytes".parse().unwrap());
    headers.insert(header::ETAG, format!("\"{}\"", artifact.spec.sha256).parse().unwrap());
    if status == StatusCode::PARTIAL_CONTENT {
        headers.insert(
            header::CONTENT_RANGE,
            format!("bytes {start}-{end}/{}", artifact.spec.size).parse().unwrap(),
        );
    }
    if request.method() == Method::HEAD {
        return result;
    }
    // A bounded channel gives backpressure without a shared cursor. A cancelled
    // download drops its sender only; the catalog's file handle remains usable.
    let (sender, receiver) = mpsc::channel::<std::io::Result<Vec<u8>>>(2);
    tokio::task::spawn_blocking(move || {
        let mut offset = start;
        while offset <= end {
            let mut buffer = vec![0; (end - offset + 1).min(64 * 1024) as usize];
            let read = artifact.unchanged().and_then(|unchanged| {
                ensure!(unchanged, "Artifact changed during download");
                let n = artifact.read_at(&mut buffer, offset)?;
                ensure!(n > 0, "Artifact truncated during download");
                buffer.truncate(n);
                Ok(n)
            });
            match read {
                Ok(n) => {
                    offset += n as u64;
                    if sender.blocking_send(Ok(buffer)).is_err() {
                        break;
                    }
                }
                Err(err) => {
                    let _ = sender.blocking_send(Err(std::io::Error::other(err.to_string())));
                    break;
                }
            }
        }
    });
    // Adapt the bounded receiver to the HTTP body's streaming interface.
    let stream = futures_stream(receiver);
    *result.body_mut() = Body::from_stream(stream);
    result
}

fn futures_stream(
    receiver: mpsc::Receiver<std::io::Result<Vec<u8>>>,
) -> impl futures_core::Stream<Item = std::io::Result<Vec<u8>>> {
    struct ReceiverStream(mpsc::Receiver<std::io::Result<Vec<u8>>>);
    impl futures_core::Stream for ReceiverStream {
        type Item = std::io::Result<Vec<u8>>;
        fn poll_next(
            mut self: std::pin::Pin<&mut Self>,
            cx: &mut std::task::Context<'_>,
        ) -> std::task::Poll<Option<Self::Item>> {
            self.0.poll_recv(cx)
        }
    }
    ReceiverStream(receiver)
}

fn read_manifest_snapshot(path: &std::path::Path) -> Result<Vec<u8>> {
    let mut file = fs::File::open(path)?;
    ensure!(file.metadata()?.len() <= catalog::MAX_MANIFEST, "Manifest exceeds 64 KiB");
    let mut bytes = Vec::new();
    (&mut file).take(catalog::MAX_MANIFEST + 1).read_to_end(&mut bytes)?;
    ensure!(bytes.len() as u64 <= catalog::MAX_MANIFEST, "Manifest exceeds 64 KiB");
    Ok(bytes)
}

async fn reload_loop(state: ServerState, manifest: PathBuf, initial: Vec<u8>) {
    let mut previous = initial;
    let mut interval = tokio::time::interval(Duration::from_secs(2));
    loop {
        interval.tick().await;
        let path = manifest.clone();
        let read = tokio::task::spawn_blocking(move || read_manifest_snapshot(&path)).await;
        let bytes = match read {
            Ok(Ok(bytes)) => bytes,
            Ok(Err(err)) => {
                tracing::warn!(error = %err, "Cannot read catalog; retaining last good release");
                continue;
            }
            Err(err) => {
                tracing::error!(error = %err, "Catalog reader failed");
                continue;
            }
        };
        if bytes == previous {
            continue;
        }
        previous = bytes;
        let path = manifest.clone();
        match tokio::task::spawn_blocking(move || catalog::load(&path)).await {
            Ok(Ok(next)) => match state.replace_catalog(next).await {
                Ok(()) => tracing::info!("Validated and reloaded catalog"),
                Err(err) => {
                    tracing::warn!(error = %err, "Catalog rejected; retaining last good release")
                }
            },
            Ok(Err(err)) => {
                tracing::warn!(error = %err, "Catalog validation failed; retaining last good release")
            }
            Err(err) => tracing::error!(error = %err, "Catalog validator failed"),
        }
    }
}

async fn shutdown() {
    #[cfg(unix)]
    {
        let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
            .expect("Install SIGTERM handler");
        tokio::select! { _ = tokio::signal::ctrl_c() => {}, _ = term.recv() => {} }
    }
    #[cfg(not(unix))]
    {
        let _ = tokio::signal::ctrl_c().await;
    }
}

async fn reload_windows_loop(state: ServerState, root: PathBuf) {
    let mut interval = tokio::time::interval(Duration::from_secs(2));
    let mut rejected = Vec::new();
    loop {
        interval.tick().await;
        // File verification may take longer than an entire admin publication.
        // Capture the active snapshot before reading, and never replace a newer
        // snapshot installed while this reload was in progress.
        let expected = state.active.read().await.windows.clone();
        let path = root.join("catalog.json");
        let read = tokio::task::spawn_blocking(move || -> Result<Option<Vec<u8>>> {
            if !path.try_exists()? {
                return Ok(None);
            }
            let mut input = fs::File::open(path)?;
            const LIMIT: u64 = 16 * 1024 * 1024;
            ensure!(input.metadata()?.len() <= LIMIT, "Windows catalog exceeds limit");
            let mut bytes = Vec::new();
            (&mut input).take(LIMIT + 1).read_to_end(&mut bytes)?;
            ensure!(bytes.len() as u64 <= LIMIT, "Windows catalog exceeds limit");
            Ok(Some(bytes))
        })
        .await;
        let bytes = match read {
            Ok(Ok(Some(bytes))) => bytes,
            Ok(Ok(None)) => continue,
            error => {
                tracing::warn!(?error, "Cannot read Windows catalog; retaining last good release");
                continue;
            }
        };
        let unchanged = expected.as_ref().is_some_and(|catalog| catalog.manifest_bytes == bytes);
        if unchanged || bytes == rejected {
            continue;
        }
        let next_root = root.clone();
        match tokio::task::spawn_blocking(move || crate::windows::load(&next_root)).await {
            Ok(Ok(catalog)) => {
                // A writer can also change the file between the cheap read and
                // the full validation. Retry a consistent snapshot next tick.
                if catalog.manifest_bytes != bytes {
                    continue;
                }
                match state.replace_windows_catalog_if_current(catalog, &expected).await {
                    Ok(_) => rejected.clear(),
                    Err(error) => {
                        tracing::warn!(%error, "Windows catalog rejected; retaining last good release");
                        rejected = bytes;
                    }
                }
            }
            Ok(Err(error)) => {
                tracing::warn!(%error, "Windows catalog validation failed; retaining last good release");
                rejected = bytes;
            }
            Err(error) => {
                tracing::warn!(%error, "Windows catalog validator failed; retaining last good release");
                rejected = bytes;
            }
        }
    }
}

pub async fn run(
    manifest: PathBuf,
    port: u16,
    public_origin: Option<String>,
    admin: Option<Arc<crate::admin::Admin>>,
    admin_port: Option<u16>,
    crashes: crate::crashes::Settings,
) -> Result<()> {
    if let Some(origin) = &public_origin {
        validate_public_origin(origin)?;
    }
    let path = manifest.clone();
    let catalog = tokio::task::spawn_blocking(move || catalog::load(&path)).await??;
    let initial = catalog.manifest_bytes.clone();
    let listener = tokio::net::TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, port)).await?;
    let port = listener.local_addr()?.port();
    let separate_management = public_origin.is_some() && admin.is_some();
    let mut state = ServerState::new(catalog, port, public_origin)?;
    if crashes.enabled {
        state.crashes = Some(Arc::new(crate::crashes::Store::new(crashes)?));
    }
    let windows_root = admin.as_ref().map(|admin| admin.windows_root());
    if let Some(admin) = &admin {
        let root = admin.windows_root();
        let windows_catalog =
            tokio::task::spawn_blocking(move || crate::windows::load(&root)).await??;
        state.replace_windows_catalog(windows_catalog).await?;
        state.set_windows_default_arch(admin.windows_default_arch().await).await;
    }
    let management = if separate_management {
        let management_port =
            admin_port.unwrap_or_else(|| if port == 65535 { 65534 } else { port + 1 });
        ensure!(
            management_port > 0 && management_port != port,
            "Management and update ports must differ"
        );
        let listener =
            tokio::net::TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, management_port))
                .await
                .context("Bind local management port; configure admin_port if it is occupied")?;
        let mut management_state = state.clone();
        management_state.port = management_port;
        management_state.admin = admin;
        tracing::info!(homepage = %format!("http://127.0.0.1:{management_port}/"), "Local publishing interface ready; keep this port private");
        Some(tokio::spawn(async move {
            axum::serve(listener, router(management_state)).with_graceful_shutdown(shutdown()).await
        }))
    } else {
        state.admin = admin;
        None
    };
    tracing::info!(homepage = %format!("http://127.0.0.1:{port}/"), listening = %format!("http://127.0.0.1:{port}{ENDPOINT}"), public_origin = %state.origin, "Windows JSON + ZIP and macOS Omaha update service ready");
    let reload = tokio::spawn(reload_loop(state.clone(), manifest, initial));
    let windows_reload =
        windows_root.map(|root| tokio::spawn(reload_windows_loop(state.clone(), root)));
    let result = axum::serve(listener, router(state)).with_graceful_shutdown(shutdown()).await;
    reload.abort();
    if let Some(task) = windows_reload {
        task.abort();
    }
    if let Some(task) = management {
        task.abort();
    }
    result.context("Serve Omaha updates")
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::http::Request;
    use http_body_util::BodyExt;
    use tower::ServiceExt;

    fn req(method: Method, uri: &str, body: Body) -> axum::http::request::Builder {
        drop(body);
        Request::builder().method(method).uri(uri).header("host", "127.0.0.1:9001")
    }

    fn setup() -> (tempfile::TempDir, ServerState, Vec<u8>) {
        let (dir, _, full) = catalog::tests::fixture();
        let catalog = catalog::load(&dir.path().join("release.json")).unwrap();
        let state = ServerState::new(catalog, 9001, None).unwrap();
        (dir, state, full)
    }

    #[tokio::test]
    async fn crash_endpoint_obeys_host_origin_and_enabled_settings() {
        let (dir, mut state, _) = setup();
        let path = dir.path().join("reports");
        let req = || {
            Request::builder()
                .method(Method::POST)
                .uri(crate::crashes::ENDPOINT)
                .header(header::CONTENT_TYPE, "application/octet-stream")
        };
        let mut dump = vec![0; 32];
        dump[..4].copy_from_slice(b"MDMP");
        let response = router(state.clone())
            .oneshot(req().header("host", "127.0.0.1:9001").body(Body::from(dump.clone())).unwrap())
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::NOT_FOUND);
        state.crashes = Some(Arc::new(
            crate::crashes::Store::new(crate::crashes::Settings {
                directory: path.clone(),
                ..Default::default()
            })
            .unwrap(),
        ));
        for builder in [
            req().header("host", "other.example"),
            req().header("host", "127.0.0.1:9001").header("origin", "https://other.example"),
        ] {
            let response = router(state.clone())
                .oneshot(builder.body(Body::from(dump.clone())).unwrap())
                .await
                .unwrap();
            assert_eq!(response.status(), StatusCode::FORBIDDEN);
        }
        assert_eq!(fs::read_dir(&path).unwrap().count(), 0);
        let response = router(state)
            .oneshot(req().header("host", "127.0.0.1:9001").body(Body::from(dump.clone())).unwrap())
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let id = response.into_body().collect().await.unwrap().to_bytes();
        assert_eq!(
            fs::read(path.join(std::str::from_utf8(&id).unwrap()).join("minidump.dmp")).unwrap(),
            dump
        );
    }

    #[tokio::test]
    async fn slow_windows_reload_cannot_replace_a_newer_admin_publication() {
        let (_dir, state, _) = setup();
        let make_catalog = |version: &str| crate::windows::Catalog {
            releases: vec![crate::windows::Release {
                version: version.into(),
                arch: "x86_64".into(),
                full: crate::catalog::ArtifactSpec {
                    file: "full.zip".into(),
                    size: 100,
                    sha256: "ab".repeat(32),
                },
                differential: vec![],
                changelog: String::new(),
                force_update: false,
            }],
            history: vec![],
            artifacts: HashMap::new(),
            manifest_bytes: version.as_bytes().to_vec(),
        };
        state.replace_windows_catalog(make_catalog("1.0.0.1")).await.unwrap();
        let expected = state.active.read().await.windows.clone();
        let (finish, resumed) = tokio::sync::oneshot::channel();
        let reload_state = state.clone();
        let stale = make_catalog("1.0.0.1");
        let reload = tokio::spawn(async move {
            resumed.await.unwrap();
            reload_state.replace_windows_catalog_if_current(stale, &expected).await.unwrap()
        });
        state.replace_windows_catalog(make_catalog("1.0.0.2")).await.unwrap();
        finish.send(()).unwrap();
        assert!(!reload.await.unwrap());
        let response = router(state)
            .oneshot(
                req(
                    Method::GET,
                    "/api/v1/update/check?platform=win&version=1.0.0.1",
                    Body::empty(),
                )
                .body(Body::empty())
                .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let bytes = response.into_body().collect().await.unwrap().to_bytes();
        let update: Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(update["target_version"], "1.0.0.2");
    }

    #[tokio::test]
    async fn browser_homepage_shows_catalog_and_empty_state() {
        let (_dir, state, _) = setup();
        let app = router(state.clone());
        let response = app
            .clone()
            .oneshot(req(Method::GET, "/", Body::empty()).body(Body::empty()).unwrap())
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()[header::CONTENT_TYPE], "text/html; charset=utf-8");
        assert!(response.headers()["content-security-policy"]
            .to_str()
            .unwrap()
            .contains("frame-ancestors 'none'"));
        let expected_length = response.headers()[header::CONTENT_LENGTH].clone();
        let bytes = response.into_body().collect().await.unwrap().to_bytes();
        let html = std::str::from_utf8(&bytes).unwrap();
        assert!(html.contains("Xenon 更新服务"));
        assert!(html.contains("href=\"/artifacts/app.crx3\""));
        let head = app
            .clone()
            .oneshot(req(Method::HEAD, "/", Body::empty()).body(Body::empty()).unwrap())
            .await
            .unwrap();
        assert_eq!(head.headers()[header::CONTENT_LENGTH], expected_length);
        assert!(head.into_body().collect().await.unwrap().to_bytes().is_empty());
        let rejected = app
            .oneshot(req(Method::POST, "/", Body::empty()).body(Body::empty()).unwrap())
            .await
            .unwrap();
        assert_eq!(rejected.status(), StatusCode::METHOD_NOT_ALLOWED);
        let empty = tempfile::tempdir().unwrap();
        fs::write(empty.path().join("catalog.json"), r#"{"schema_version":1,"apps":[]}"#).unwrap();
        state
            .replace_catalog(catalog::load(&empty.path().join("catalog.json")).unwrap())
            .await
            .unwrap();
        let response = router(state)
            .oneshot(req(Method::GET, "/", Body::empty()).body(Body::empty()).unwrap())
            .await
            .unwrap();
        let bytes = response.into_body().collect().await.unwrap().to_bytes();
        assert!(std::str::from_utf8(&bytes).unwrap().contains("还没有发布版本"));
    }

    #[tokio::test]
    async fn http_omaha_prefix_and_pipeline_response() {
        let (_dir, state, _) = setup();
        let body = serde_json::to_vec(&catalog::tests::request()).unwrap();
        let response = router(state)
            .oneshot(
                req(Method::POST, ENDPOINT, Body::empty())
                    .header("content-type", "application/json")
                    .body(Body::from(body))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let bytes = response.into_body().collect().await.unwrap().to_bytes();
        assert!(bytes.starts_with(RESPONSE_PREFIX.as_bytes()));
        let reply: Value = serde_json::from_slice(&bytes[RESPONSE_PREFIX.len()..]).unwrap();
        assert_eq!(
            reply["response"]["apps"][0]["updatecheck"]["pipelines"].as_array().unwrap().len(),
            2
        );
    }

    #[tokio::test]
    async fn host_origin_cup_encoding_and_size_guards() {
        let (_dir, state, _) = setup();
        let router = router(state);
        for (header, value, status) in [
            ("host", "evil.example", 403),
            ("origin", "https://example.com", 403),
            ("content-encoding", "gzip", 415),
        ] {
            let mut request = req(Method::POST, ENDPOINT, Body::empty())
                .header("content-type", "application/json")
                .body(Body::from("{}"))
                .unwrap();
            request
                .headers_mut()
                .insert(header::HeaderName::from_static(header), value.parse().unwrap());
            assert_eq!(router.clone().oneshot(request).await.unwrap().status().as_u16(), status);
        }
        let response = router
            .clone()
            .oneshot(
                req(Method::POST, &format!("{ENDPOINT}?cup2key=x"), Body::empty())
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::NOT_IMPLEMENTED);
        let response = router
            .oneshot(
                req(Method::POST, ENDPOINT, Body::empty())
                    .header("content-type", "application/json")
                    .body(Body::from(vec![b' '; MAX_BODY + 1]))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::PAYLOAD_TOO_LARGE);
    }

    #[tokio::test]
    async fn range_head_and_parallel_reads() {
        let (_dir, state, full) = setup();
        let router = router(state);
        let mut jobs = Vec::new();
        for index in 0..20 {
            let router = router.clone();
            jobs.push(tokio::spawn(async move {
                let start = index * 7;
                let request = req(Method::GET, "/artifacts/app.crx3", Body::empty())
                    .header("range", format!("bytes={start}-{}", start + 20))
                    .body(Body::empty())
                    .unwrap();
                let response = router.oneshot(request).await.unwrap();
                assert_eq!(response.status(), StatusCode::PARTIAL_CONTENT);
                (start, response.into_body().collect().await.unwrap().to_bytes())
            }));
        }
        for job in jobs {
            let (start, data) = job.await.unwrap();
            assert_eq!(&data[..], &full[start..start + 21]);
        }
        let response = router
            .clone()
            .oneshot(
                req(Method::HEAD, "/artifacts/app.crx3", Body::empty())
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.headers()[header::CONTENT_LENGTH], full.len().to_string());
        assert!(response.into_body().collect().await.unwrap().to_bytes().is_empty());
        for range in [
            "bytes=-0",
            "bytes=999999-",
            "bytes=3-2",
            "bytes=0-1,3-4",
            "bytes=18446744073709551616-",
        ] {
            let response = router
                .clone()
                .oneshot(
                    req(Method::GET, "/artifacts/app.crx3", Body::empty())
                        .header("range", range)
                        .body(Body::empty())
                        .unwrap(),
                )
                .await
                .unwrap();
            assert_eq!(response.status(), StatusCode::RANGE_NOT_SATISFIABLE, "{range}");
        }
        let response = router
            .clone()
            .oneshot(
                req(Method::GET, "/artifacts/app.crx3", Body::empty())
                    .header("range", "bytes=-10")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(
            response.into_body().collect().await.unwrap().to_bytes(),
            &full[full.len() - 10..]
        );
        for uri in
            ["/artifacts/../release.json", "/artifacts/%61pp.crx3", "/artifacts/app.crx3?q=x"]
        {
            assert_eq!(
                router
                    .clone()
                    .oneshot(req(Method::GET, uri, Body::empty()).body(Body::empty()).unwrap())
                    .await
                    .unwrap()
                    .status(),
                StatusCode::NOT_FOUND
            );
        }
    }

    #[tokio::test]
    async fn cancellation_and_tamper_do_not_break_other_downloads() {
        let (dir, state, full) = setup();
        let router = router(state);
        let response = router
            .clone()
            .oneshot(
                req(Method::GET, "/artifacts/app.crx3", Body::empty()).body(Body::empty()).unwrap(),
            )
            .await
            .unwrap();
        drop(response);
        let response = router
            .clone()
            .oneshot(
                req(Method::GET, "/artifacts/app.crx3", Body::empty()).body(Body::empty()).unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), full);
        fs::write(dir.path().join("app.crx3"), b"changed").unwrap();
        assert_eq!(
            router
                .oneshot(
                    req(Method::GET, "/artifacts/app.crx3", Body::empty())
                        .body(Body::empty())
                        .unwrap()
                )
                .await
                .unwrap()
                .status(),
            StatusCode::CONFLICT
        );
    }

    #[tokio::test]
    async fn reload_retains_old_urls_and_rejects_replacement() {
        let (_old_dir, state, full) = setup();
        let empty = tempfile::tempdir().unwrap();
        fs::write(empty.path().join("release.json"), r#"{"schema_version":1,"apps":[]}"#).unwrap();
        state
            .replace_catalog(catalog::load(&empty.path().join("release.json")).unwrap())
            .await
            .unwrap();
        let response = router(state.clone())
            .oneshot(
                req(Method::GET, "/artifacts/app.crx3", Body::empty()).body(Body::empty()).unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), full);
        let (next, mut manifest, mut data) = catalog::tests::fixture();
        data[30] ^= 1;
        use sha2::Digest;
        manifest["full"]["sha256"] = json!(hex::encode(sha2::Sha256::digest(&data)));
        fs::write(next.path().join("app.crx3"), data).unwrap();
        fs::write(next.path().join("release.json"), serde_json::to_vec(&manifest).unwrap())
            .unwrap();
        assert!(state
            .replace_catalog(catalog::load(&next.path().join("release.json")).unwrap())
            .await
            .is_err());
        assert!(state.active.read().await.catalog.releases.is_empty());
    }

    #[test]
    fn public_origin_is_explicit_https_only() {
        assert!(validate_public_origin("https://updates.example.com").is_ok());
        for bad in [
            "http://updates.example.com",
            "https://updates.example.com/",
            "https://user@updates.example.com",
            "https://updates.example.com/a",
            "https://updates.example.com?x=1",
            "https://updates.example.com#x",
        ] {
            assert!(validate_public_origin(bad).is_err(), "{bad}");
        }
    }
}
