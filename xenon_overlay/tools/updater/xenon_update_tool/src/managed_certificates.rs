// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Versioned CA snapshots. All listeners are loopback-only. HTTPS is provided
//! by the optional dedicated listener or deployment proxy; no OS trust changes.
use anyhow::{ensure, Context, Result};
use axum::{
    body::Body,
    extract::Request,
    http::{header, HeaderMap, Method, StatusCode},
    response::Response,
};
use base64::{engine::general_purpose::STANDARD, Engine};
use fs2::FileExt;
use serde::{Deserialize, Deserializer, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::HashSet,
    env, fs,
    io::{Read, Write},
    net::IpAddr,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{SystemTime, UNIX_EPOCH},
};
use x509_parser::{extensions::ParsedExtension, prelude::parse_x509_certificate};

pub mod https;
pub mod publisher;

pub const ENDPOINT: &str = "/api/v1/browser/ca-certificates";
const MAX_BODY: usize = 4 * 1024 * 1024;
const MAX_CERT: usize = 16 * 1024;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Settings {
    #[serde(default = "default_snapshot_file")]
    pub snapshot_file: PathBuf,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bearer_token_env: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub https: Option<https::Settings>,
}

fn default_snapshot_file() -> PathBuf {
    PathBuf::from("managed-ca/current.json")
}

impl Default for Settings {
    fn default() -> Self {
        Self { snapshot_file: default_snapshot_file(), bearer_token_env: None, https: None }
    }
}

impl Settings {
    pub fn validate(&self) -> Result<()> {
        if let Some(https) = &self.https {
            https.validate()?;
        }
        ensure!(
            !self.snapshot_file.as_os_str().is_empty(),
            "managed_certificates.snapshot_file is empty"
        );
        if let Some(name) = &self.bearer_token_env {
            ensure!(
                !name.is_empty()
                    && name.len() <= 128
                    && name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_')
                    && !name.as_bytes()[0].is_ascii_digit(),
                "managed_certificates.bearer_token_env must be an environment variable name"
            );
        }
        Ok(())
    }
}

// Optional fields must be omitted, rather than explicitly set to JSON null.
fn present<'de, D: Deserializer<'de>, T: Deserialize<'de>>(d: D) -> Result<Option<T>, D::Error> {
    T::deserialize(d).map(Some)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Document {
    schema_version: u32,
    source_id: String,
    generation: String,
    issued_at: String,
    // Accept legacy metadata without applying a snapshot lifetime.
    #[serde(default, deserialize_with = "present")]
    expires_at: Option<String>,
    certificates: Vec<Certificate>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Certificate {
    #[serde(default, deserialize_with = "present")]
    name: Option<String>,
    role: String,
    encoding: String,
    data: String,
    #[serde(default, deserialize_with = "present")]
    constraints: Option<Constraints>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Constraints {
    #[serde(default)]
    permitted_dns_names: Vec<String>,
    #[serde(default)]
    permitted_cidrs: Vec<String>,
}

struct Snapshot {
    source: String,
    generation: u64,
    bytes: Vec<u8>,
    etag: String,
}

fn timestamp(value: &str) -> Result<i64> {
    ensure!(value.len() == 20, "Timestamp must be UTC YYYY-MM-DDTHH:mm:ssZ");
    let format = time::format_description::parse_borrowed::<2>(
        "[year]-[month]-[day]T[hour]:[minute]:[second]Z",
    )?;
    let parsed = time::PrimitiveDateTime::parse(value, &format).context("Invalid UTC timestamp")?;
    ensure!(parsed.format(&format)? == value, "Timestamp is not canonical");
    Ok(parsed.assume_utc().unix_timestamp())
}

fn validate_depth(bytes: &[u8]) -> Result<()> {
    let (mut depth, mut string, mut escaped) = (0usize, false, false);
    for &byte in bytes {
        if string {
            if escaped {
                escaped = false;
            } else if byte == b'\\' {
                escaped = true;
            } else if byte == b'"' {
                string = false;
            }
        } else {
            match byte {
                b'"' => string = true,
                b'{' | b'[' => {
                    depth += 1;
                    ensure!(depth <= 16, "JSON nesting exceeds 16");
                }
                b'}' | b']' => depth = depth.saturating_sub(1),
                _ => {}
            }
        }
    }
    Ok(())
}

fn valid_dns(value: &str) -> bool {
    let domain = value.strip_prefix('.').unwrap_or(value);
    !domain.is_empty()
        && domain.len() <= 253
        && domain.split('.').all(|label| {
            !label.is_empty()
                && label.len() <= 63
                && !label.starts_with('-')
                && !label.ends_with('-')
                && label.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        })
}

fn valid_cidr(value: &str) -> bool {
    let Some((address, prefix)) = value.split_once('/') else { return false };
    if prefix.is_empty() || !prefix.bytes().all(|b| b.is_ascii_digit()) {
        return false;
    }
    let (Ok(address), Ok(prefix)) = (address.parse::<IpAddr>(), prefix.parse::<u32>()) else {
        return false;
    };
    // Match Chromium's URL/IP formatter, including hexadecimal IPv4-mapped
    // IPv6 addresses (std::net::Ipv6Addr's Display uses dotted decimal there).
    let canonical = match address {
        IpAddr::V4(address) => address.to_string(),
        IpAddr::V6(address) => url::Host::<&str>::Ipv6(address)
            .to_string()
            .trim_start_matches('[')
            .trim_end_matches(']')
            .to_owned(),
    };
    if value != format!("{canonical}/{prefix}") {
        return false;
    }
    match address {
        IpAddr::V4(address) if prefix <= 32 => {
            let host_mask = if prefix == 32 { 0 } else { u32::MAX >> prefix };
            u32::from(address) & host_mask == 0
        }
        IpAddr::V6(address) if prefix <= 128 => {
            let host_mask = if prefix == 128 { 0 } else { u128::MAX >> prefix };
            u128::from(address) & host_mask == 0
        }
        _ => false,
    }
}

fn decode_certificate(cert: &Certificate) -> Result<Vec<u8>> {
    let data = match cert.encoding.as_str() {
        "der_base64" => cert.data.clone(),
        "pem" => {
            let pem = cert.data.trim_matches(|c: char| c.is_ascii_whitespace());
            let inner = pem
                .strip_prefix("-----BEGIN CERTIFICATE-----")
                .and_then(|s| s.strip_suffix("-----END CERTIFICATE-----"))
                .context("Expected exactly one CERTIFICATE PEM block")?;
            inner.chars().filter(|c| !matches!(c, ' ' | '\t' | '\r' | '\n')).collect()
        }
        _ => anyhow::bail!("Unsupported certificate encoding"),
    };
    ensure!(data.len() <= MAX_CERT.div_ceil(3) * 4, "Certificate exceeds size limit");
    let der = STANDARD.decode(data).context("Invalid certificate base64")?;
    ensure!(!der.is_empty() && der.len() <= MAX_CERT, "Certificate exceeds size limit");
    let (rest, parsed) =
        parse_x509_certificate(&der).map_err(|_| anyhow::anyhow!("Invalid X.509 DER"))?;
    ensure!(rest.is_empty(), "Trailing bytes after X.509 certificate");
    parsed.extensions_map().map_err(|_| anyhow::anyhow!("Duplicate X.509 extensions"))?;
    ensure!(
        !parsed
            .extensions()
            .iter()
            .any(|e| matches!(e.parsed_extension(), ParsedExtension::ParseError { .. })),
        "Malformed X.509 extension"
    );
    ensure!(parsed.basic_constraints()?.is_some_and(|bc| bc.value.ca), "Certificate is not a CA");
    ensure!(
        parsed.key_usage()?.is_none_or(|ku| ku.value.key_cert_sign()),
        "CA keyUsage lacks keyCertSign"
    );
    Ok(der)
}

impl Snapshot {
    fn parse(bytes: Vec<u8>) -> Result<Self> {
        ensure!(bytes.len() <= MAX_BODY, "Snapshot exceeds 4 MiB");
        validate_depth(&bytes)?;
        // Struct deserialization rejects unknown and duplicate keys at every
        // schema level; do not round-trip through serde_json::Value first.
        let doc: Document = serde_json::from_slice(&bytes).context("Invalid CA snapshot JSON")?;
        ensure!(doc.schema_version == 1, "Unsupported snapshot schema_version");
        ensure!(
            !doc.source_id.is_empty()
                && doc.source_id.len() <= 64
                && doc.source_id.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)),
            "Invalid source_id"
        );
        ensure!(
            !doc.generation.starts_with('0')
                && !doc.generation.is_empty()
                && doc.generation.bytes().all(|b| b.is_ascii_digit()),
            "Invalid generation"
        );
        let generation: u64 = doc.generation.parse().context("Generation exceeds uint64")?;
        timestamp(&doc.issued_at)?;
        if let Some(expires_at) = &doc.expires_at {
            timestamp(expires_at)?;
        }
        ensure!(doc.certificates.len() <= 256, "Too many certificates");
        let mut seen = HashSet::new();
        for cert in &doc.certificates {
            ensure!(
                cert.name.as_ref().is_none_or(|s| s.chars().count() <= 128),
                "Certificate name too long"
            );
            ensure!(
                matches!(cert.role.as_str(), "trust_anchor" | "intermediate"),
                "Invalid certificate role"
            );
            if let Some(constraints) = &cert.constraints {
                ensure!(cert.role == "trust_anchor", "Intermediate cannot carry trust constraints");
                ensure!(
                    !constraints.permitted_dns_names.is_empty()
                        || !constraints.permitted_cidrs.is_empty(),
                    "Empty constraints"
                );
                ensure!(
                    constraints.permitted_dns_names.len() <= 64
                        && constraints.permitted_cidrs.len() <= 64,
                    "Too many constraints"
                );
                ensure!(
                    constraints.permitted_dns_names.iter().collect::<HashSet<_>>().len()
                        == constraints.permitted_dns_names.len()
                        && constraints.permitted_cidrs.iter().collect::<HashSet<_>>().len()
                            == constraints.permitted_cidrs.len(),
                    "Duplicate constraints"
                );
                ensure!(
                    constraints.permitted_dns_names.iter().all(|s| valid_dns(s)),
                    "Invalid DNS constraint"
                );
                ensure!(
                    constraints.permitted_cidrs.iter().all(|s| valid_cidr(s)),
                    "Invalid CIDR constraint"
                );
            }
            let der = decode_certificate(cert)?;
            ensure!(seen.insert(Sha256::digest(&der).to_vec()), "Duplicate certificate");
        }
        let etag = format!("\"{}\"", hex::encode(Sha256::digest(&bytes)));
        Ok(Self { source: doc.source_id, generation, bytes, etag })
    }
}

fn read_bounded(path: &Path) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    fs::File::open(path)?.take(MAX_BODY as u64 + 1).read_to_end(&mut bytes)?;
    ensure!(bytes.len() <= MAX_BODY, "Snapshot exceeds 4 MiB");
    Ok(bytes)
}

fn sidecar(path: &Path, suffix: &str) -> PathBuf {
    let mut value = path.as_os_str().to_owned();
    value.push(suffix);
    value.into()
}

fn snapshot_directory(path: &Path) -> &Path {
    path.parent().filter(|parent| !parent.as_os_str().is_empty()).unwrap_or(Path::new("."))
}

fn utc_now() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs().min(i64::MAX as u64)
        as i64
}

struct Active {
    snapshot: Option<Arc<Snapshot>>,
    last_error: Option<String>,
}

pub struct Store {
    settings: Settings,
    token_hash: Option<[u8; 32]>,
    active: Mutex<Active>,
    // The accepted state and in-memory watermark have one owner.
    _lock: fs::File,
}

impl Store {
    pub fn new(settings: Settings) -> Result<Self> {
        settings.validate()?;
        let token_hash =
            settings
                .bearer_token_env
                .as_ref()
                .map(|name| -> Result<[u8; 32]> {
                    let token = env::var(name)
                        .context("Managed CA bearer token environment variable is unavailable")?;
                    ensure!(!token.is_empty() && token.len() <= 4096
                && token.bytes().all(|b| b.is_ascii_alphanumeric() || b"-._~+/=".contains(&b)),
                "Managed CA bearer token must be a nonempty RFC6750 token (maximum 4096 bytes)");
                    Ok(Sha256::digest(token.as_bytes()).into())
                })
                .transpose()?;
        fs::create_dir_all(snapshot_directory(&settings.snapshot_file))
            .context("Cannot create managed CA snapshot directory")?;
        let lock = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(sidecar(&settings.snapshot_file, ".serve.lock"))
            .context("Cannot open managed CA state lock")?;
        lock.try_lock_exclusive().context("Another server owns this managed CA snapshot")?;
        let accepted = sidecar(&settings.snapshot_file, ".accepted.json");
        let snapshot = if accepted.try_exists()? {
            Some(Arc::new(Snapshot::parse(read_bounded(&accepted)?)
                .context("Managed CA accepted state is corrupt; restore it rather than resetting the watermark")?))
        } else {
            None
        };
        Ok(Self {
            settings,
            token_hash,
            active: Mutex::new(Active { snapshot, last_error: None }),
            _lock: lock,
        })
    }

    fn authorized(&self, headers: &HeaderMap) -> bool {
        let Some(expected) = self.token_hash else { return true };
        if headers.get_all(header::AUTHORIZATION).iter().count() != 1 {
            return false;
        }
        let Some((scheme, token)) = headers
            .get(header::AUTHORIZATION)
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.split_once(' '))
        else {
            return false;
        };
        if !scheme.eq_ignore_ascii_case("Bearer") {
            return false;
        }
        if token.len() > 4096 {
            return false;
        }
        let actual: [u8; 32] = Sha256::digest(token.as_bytes()).into();
        expected.iter().zip(actual).fold(0u8, |diff, (a, b)| diff | (a ^ b)) == 0
    }

    fn reload(&self, active: &mut Active) -> Result<()> {
        let bytes = read_bounded(&self.settings.snapshot_file)?;
        if active.snapshot.as_ref().is_some_and(|s| s.bytes == bytes) {
            return Ok(());
        }
        let candidate = Snapshot::parse(bytes)?;
        let accepted = sidecar(&self.settings.snapshot_file, ".accepted.json");
        // A rename may have succeeded before a directory fsync failed. Keep
        // its version floor even while serving the previous in-memory body.
        let persisted = if accepted.try_exists()? {
            Some(Snapshot::parse(read_bounded(&accepted)?)?)
        } else {
            None
        };
        for previous in active.snapshot.as_deref().into_iter().chain(persisted.as_ref()) {
            ensure!(
                candidate.source == previous.source,
                "source_id changed; configure a separate snapshot path for a new source"
            );
            ensure!(candidate.generation >= previous.generation, "generation_rollback");
            ensure!(
                candidate.generation != previous.generation || candidate.bytes == previous.bytes,
                "generation_conflict"
            );
        }
        let mut file = tempfile::NamedTempFile::new_in(snapshot_directory(&accepted))?;
        file.write_all(&candidate.bytes)?;
        file.as_file().sync_all()?;
        file.persist(&accepted).context("Cannot persist managed CA accepted state")?;
        #[cfg(unix)]
        fs::File::open(snapshot_directory(&accepted))?.sync_all()?;
        active.snapshot = Some(Arc::new(candidate));
        Ok(())
    }

    fn current(&self) -> Option<Arc<Snapshot>> {
        let mut active = self.active.lock().unwrap_or_else(|e| e.into_inner());
        match self.reload(&mut active) {
            Ok(()) => active.last_error = None,
            Err(error) => {
                // Parser errors may include input tokens; keep diagnostics to
                // a generic category without logging the snapshot or secrets.
                let category = if error.to_string().contains("generation_") {
                    "version_rejected"
                } else {
                    "reload_failed"
                };
                if active.last_error.as_deref() != Some(category) {
                    tracing::warn!(category, "Managed CA snapshot rejected; retaining last accepted snapshot until an update or withdrawal");
                    active.last_error = Some(category.into());
                }
            }
        }
        active.snapshot.clone()
    }

    /// Report exactly the accepted snapshot that the endpoint can
    /// serve, including fallback when the publication on disk is invalid.
    pub fn admin_status(&self) -> Result<serde_json::Value> {
        let current = self.current();
        let active = self.active.lock().unwrap_or_else(|error| error.into_inner());
        let serving_error = active
            .last_error
            .as_deref()
            .or_else(|| current.is_none().then_some("no_valid_snapshot"));
        Ok(serde_json::json!({
            "serving_snapshot": current.as_ref().map(|snapshot| publisher::summary(snapshot)).transpose()?,
            "serving_error": serving_error,
        }))
    }
}

fn error(status: StatusCode, message: &'static str) -> Response {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "application/json; charset=utf-8")
        .header(header::CACHE_CONTROL, "no-store")
        .header("x-content-type-options", "nosniff")
        .body(Body::from(serde_json::json!({"error": message}).to_string()))
        .unwrap()
}

pub async fn handle(store: Arc<Store>, request: Request) -> Response {
    if !store.authorized(request.headers()) {
        let mut response = error(StatusCode::UNAUTHORIZED, "Authentication required");
        response.headers_mut().insert(header::WWW_AUTHENTICATE, "Bearer".parse().unwrap());
        return response;
    }
    if request.method() != Method::GET {
        let mut response = error(StatusCode::METHOD_NOT_ALLOWED, "GET required");
        response.headers_mut().insert(header::ALLOW, "GET".parse().unwrap());
        return response;
    }
    if request.uri().query().is_some() {
        return error(StatusCode::BAD_REQUEST, "Query parameters are not supported");
    }
    let tags = request
        .headers()
        .get_all(header::IF_NONE_MATCH)
        .iter()
        .filter_map(|v| v.to_str().ok())
        .collect::<Vec<_>>()
        .join(",");
    let snapshot = tokio::task::spawn_blocking(move || store.current()).await;
    let Some(snapshot) = snapshot.ok().flatten() else {
        let mut response = error(StatusCode::SERVICE_UNAVAILABLE, "No valid managed CA snapshot");
        response.headers_mut().insert(header::RETRY_AFTER, "60".parse().unwrap());
        return response;
    };
    let matched = tags
        .split(',')
        .map(str::trim)
        .any(|tag| tag == "*" || tag.strip_prefix("W/").unwrap_or(tag) == snapshot.etag);
    Response::builder()
        .status(if matched { StatusCode::NOT_MODIFIED } else { StatusCode::OK })
        .header(header::CONTENT_TYPE, "application/json; charset=utf-8")
        .header(header::CACHE_CONTROL, "private, no-cache")
        .header(header::VARY, "Authorization")
        .header(header::ETAG, &snapshot.etag)
        .header("x-content-type-options", "nosniff")
        .body(if matched { Body::empty() } else { Body::from(snapshot.bytes.clone()) })
        .unwrap()
}

#[cfg(test)]
mod tests {
    use super::*;
    use http_body_util::BodyExt;
    use serde_json::{json, Value};

    const ROOT: &str =
        include_str!("../managed_certificates/certificates/gdca-trustauth-e5-root.pem");

    fn date(value: i64) -> String {
        time::OffsetDateTime::from_unix_timestamp(value)
            .unwrap()
            .format(&time::format_description::well_known::Rfc3339)
            .unwrap()
    }

    fn document(generation: &str) -> Value {
        json!({
            "schema_version": 1, "source_id": "test-enterprise", "generation": generation,
            "issued_at": date(utc_now() - 60),
            "certificates": [{"name": "Test root", "role": "trust_anchor", "encoding": "pem", "data": ROOT}]
        })
    }

    fn settings(path: PathBuf) -> Settings {
        Settings { snapshot_file: path, ..Default::default() }
    }

    fn fixture() -> (tempfile::TempDir, Arc<Store>, Vec<u8>) {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("snapshot.json");
        let bytes = serde_json::to_vec_pretty(&document("10")).unwrap();
        fs::write(&file, &bytes).unwrap();
        let store = Arc::new(Store::new(settings(file)).unwrap());
        (dir, store, bytes)
    }

    async fn get(store: &Arc<Store>, etag: Option<&str>) -> Response {
        let mut builder = Request::builder().uri(ENDPOINT);
        if let Some(etag) = etag {
            builder = builder.header(header::IF_NONE_MATCH, etag);
        }
        handle(store.clone(), builder.body(Body::empty()).unwrap()).await
    }

    #[tokio::test]
    async fn missing_snapshot_directory_is_created_and_waits_for_publication() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("state/managed-ca/current.json");
        assert!(!file.parent().unwrap().exists());
        let store = Arc::new(Store::new(settings(file.clone())).unwrap());
        assert!(file.parent().unwrap().is_dir());
        assert!(!file.exists());
        let response = get(&store, None).await;
        assert_eq!(response.status(), StatusCode::SERVICE_UNAVAILABLE);
        assert_eq!(response.headers()[header::RETRY_AFTER], "60");
        assert!(!response.headers().contains_key(header::ETAG));
        assert!(!file.exists());

        let bytes = serde_json::to_vec(&document("1")).unwrap();
        fs::write(&file, &bytes).unwrap();
        let response = get(&store, None).await;
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), bytes);
    }

    #[test]
    fn snapshot_directory_creation_does_not_replace_existing_files() {
        let dir = tempfile::tempdir().unwrap();
        let parent = dir.path().join("managed-ca");
        let existing = b"existing file";
        fs::write(&parent, existing).unwrap();
        let file = parent.join("nested/current.json");
        assert!(Store::new(settings(file)).is_err());
        assert_eq!(fs::read(parent).unwrap(), existing);
    }

    #[tokio::test]
    async fn exact_bytes_etag_and_conditional_responses() {
        let (_dir, store, bytes) = fixture();
        let response = get(&store, None).await;
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()[header::CACHE_CONTROL], "private, no-cache");
        assert_eq!(response.headers()[header::VARY], "Authorization");
        let etag = response.headers()[header::ETAG].to_str().unwrap().to_owned();
        assert_eq!(etag, format!("\"{}\"", hex::encode(Sha256::digest(&bytes))));
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), bytes);
        for condition in [etag.clone(), format!("\"another\", W/{etag}"), "*".into()] {
            let response = get(&store, Some(&condition)).await;
            assert_eq!(response.status(), StatusCode::NOT_MODIFIED);
            assert_eq!(response.headers()[header::ETAG], etag);
            assert!(response.into_body().collect().await.unwrap().to_bytes().is_empty());
        }
        assert_eq!(get(&store, Some("\"different\"")).await.status(), StatusCode::OK);
    }

    #[tokio::test]
    async fn authorization_precedes_snapshot_read_and_etag() {
        let (dir, mut store, _) = fixture();
        Arc::get_mut(&mut store).unwrap().token_hash = Some(Sha256::digest(b"test-secret").into());
        for authorization in [None, Some("Bearer wrong"), Some("Basic test-secret")] {
            let mut request = Request::builder().uri(ENDPOINT).header(header::IF_NONE_MATCH, "*");
            if let Some(value) = authorization {
                request = request.header(header::AUTHORIZATION, value);
            }
            let response = handle(store.clone(), request.body(Body::empty()).unwrap()).await;
            assert_eq!(response.status(), StatusCode::UNAUTHORIZED);
            assert!(!response.headers().contains_key(header::ETAG));
            assert_eq!(response.headers()[header::CACHE_CONTROL], "no-store");
            assert!(!dir.path().join("snapshot.json.accepted.json").exists());
        }
        let request = Request::builder()
            .uri(ENDPOINT)
            .header(header::AUTHORIZATION, "bearer test-secret")
            .body(Body::empty())
            .unwrap();
        assert_eq!(handle(store.clone(), request).await.status(), StatusCode::OK);
        let request = Request::builder()
            .uri(ENDPOINT)
            .header(header::AUTHORIZATION, "Bearer test-secret")
            .header(header::AUTHORIZATION, "Bearer test-secret")
            .body(Body::empty())
            .unwrap();
        assert_eq!(handle(store, request).await.status(), StatusCode::UNAUTHORIZED);
    }

    #[tokio::test]
    async fn malformed_rollback_and_same_version_conflict_keep_accepted_bytes() {
        let (_dir, store, original) = fixture();
        let first = get(&store, None).await;
        let etag = first.headers()[header::ETAG].to_str().unwrap().to_owned();
        let mut conflict = document("10");
        conflict["certificates"] = json!([]);
        let mut source_change = document("11");
        source_change["source_id"] = json!("other-source");
        for invalid in [
            b"{bad".to_vec(),
            serde_json::to_vec(&document("9")).unwrap(),
            serde_json::to_vec(&conflict).unwrap(),
            serde_json::to_vec(&source_change).unwrap(),
        ] {
            fs::write(&store.settings.snapshot_file, invalid).unwrap();
            assert_eq!(get(&store, Some(&etag)).await.status(), StatusCode::NOT_MODIFIED);
            let response = get(&store, None).await;
            assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), original);
        }
        fs::remove_file(&store.settings.snapshot_file).unwrap();
        assert_eq!(get(&store, None).await.status(), StatusCode::OK);
    }

    #[tokio::test]
    async fn newer_empty_snapshot_is_a_revocation() {
        let (_dir, store, _) = fixture();
        let response = get(&store, None).await;
        let old_etag = response.headers()[header::ETAG].to_str().unwrap().to_owned();
        let mut empty = document("11");
        empty["certificates"] = json!([]);
        let bytes = serde_json::to_vec(&empty).unwrap();
        fs::write(&store.settings.snapshot_file, &bytes).unwrap();
        let response = get(&store, Some(&old_etag)).await;
        assert_eq!(response.status(), StatusCode::OK);
        assert_ne!(response.headers()[header::ETAG], old_etag);
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), bytes);
        assert_eq!(get(&store, Some("*")).await.status(), StatusCode::NOT_MODIFIED);
    }

    #[tokio::test]
    async fn legacy_expired_snapshot_keeps_exact_bytes_etag_and_watermark_after_restart() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("snapshot.json");
        let accepted = sidecar(&path, ".accepted.json");
        let mut legacy = document("10");
        legacy["issued_at"] = json!("2000-01-01T00:00:00Z");
        legacy["expires_at"] = json!("2000-01-02T00:00:00Z");
        let bytes = serde_json::to_vec_pretty(&legacy).unwrap();
        fs::write(&accepted, &bytes).unwrap();
        let etag = format!("\"{}\"", hex::encode(Sha256::digest(&bytes)));

        for _ in 0..2 {
            let store = Arc::new(Store::new(settings(path.clone())).unwrap());
            let response = get(&store, None).await;
            assert_eq!(response.status(), StatusCode::OK);
            assert_eq!(response.headers()[header::ETAG], etag);
            assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), bytes);
            assert_eq!(get(&store, Some(&etag)).await.status(), StatusCode::NOT_MODIFIED);
            // Removing legacy metadata must not change an accepted generation.
            for replacement in [document("9"), document("10")] {
                fs::write(&path, serde_json::to_vec(&replacement).unwrap()).unwrap();
                assert_eq!(get(&store, Some(&etag)).await.status(), StatusCode::NOT_MODIFIED);
                assert_eq!(fs::read(&accepted).unwrap(), bytes);
            }
        }

        let store = Arc::new(Store::new(settings(path.clone())).unwrap());
        let mut withdrawal = document("11");
        withdrawal["certificates"] = json!([]);
        fs::write(&path, serde_json::to_vec(&withdrawal).unwrap()).unwrap();
        assert_eq!(get(&store, Some(&etag)).await.status(), StatusCode::OK);
        assert_eq!(store.admin_status().unwrap()["serving_snapshot"]["count"], 0);
    }

    #[tokio::test]
    async fn publication_time_is_metadata_and_missing_initial_snapshot_is_unavailable() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("snapshot.json");
        let store = Arc::new(Store::new(settings(path.clone())).unwrap());
        assert_eq!(get(&store, Some("*")).await.status(), StatusCode::SERVICE_UNAVAILABLE);
        for (generation, issued_at) in
            [("1", "2000-01-01T00:00:00Z"), ("2", "9999-01-01T00:00:00Z")]
        {
            let mut publication = document(generation);
            publication["issued_at"] = json!(issued_at);
            let bytes = serde_json::to_vec(&publication).unwrap();
            fs::write(&path, &bytes).unwrap();
            let response = get(&store, None).await;
            assert_eq!(response.status(), StatusCode::OK);
            assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), bytes);
            assert_eq!(get(&store, Some("*")).await.status(), StatusCode::NOT_MODIFIED);
        }
    }

    #[tokio::test]
    async fn accepted_state_survives_restart_and_rejects_rollback() {
        let (dir, store, original) = fixture();
        assert_eq!(get(&store, None).await.status(), StatusCode::OK);
        let path = store.settings.snapshot_file.clone();
        drop(store);
        fs::write(&path, serde_json::to_vec(&document("1")).unwrap()).unwrap();
        let store = Arc::new(Store::new(settings(path.clone())).unwrap());
        let response = get(&store, None).await;
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), original);
        assert!(Store::new(settings(path.clone())).is_err(), "Only one owner is allowed");
        drop(store);
        fs::write(dir.path().join("snapshot.json.accepted.json"), "corrupt").unwrap();
        assert!(Store::new(settings(path)).is_err(), "Do not silently reset a corrupt watermark");
    }

    #[tokio::test]
    async fn persistence_failure_does_not_activate_candidate() {
        let (_dir, store, original) = fixture();
        assert_eq!(get(&store, None).await.status(), StatusCode::OK);
        let accepted = sidecar(&store.settings.snapshot_file, ".accepted.json");
        fs::remove_file(&accepted).unwrap();
        fs::create_dir(&accepted).unwrap();
        fs::write(&store.settings.snapshot_file, serde_json::to_vec(&document("11")).unwrap())
            .unwrap();
        let response = get(&store, None).await;
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), original);
        assert_eq!(store.active.lock().unwrap().snapshot.as_ref().unwrap().generation, 10);
    }

    #[tokio::test]
    async fn renamed_state_keeps_version_floor_even_before_memory_activation() {
        let (_dir, store, original) = fixture();
        assert_eq!(get(&store, None).await.status(), StatusCode::OK);
        let newer = serde_json::to_vec(&document("20")).unwrap();
        fs::write(sidecar(&store.settings.snapshot_file, ".accepted.json"), &newer).unwrap();
        fs::write(&store.settings.snapshot_file, serde_json::to_vec(&document("15")).unwrap())
            .unwrap();
        let response = get(&store, None).await;
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), original);
        fs::write(&store.settings.snapshot_file, &newer).unwrap();
        let response = get(&store, None).await;
        assert_eq!(response.into_body().collect().await.unwrap().to_bytes(), newer);
    }

    #[tokio::test]
    async fn route_rejects_other_methods_and_query_parameters() {
        let (_dir, store, _) = fixture();
        for method in [Method::POST, Method::HEAD, Method::PUT] {
            let response = handle(
                store.clone(),
                Request::builder().uri(ENDPOINT).method(method).body(Body::empty()).unwrap(),
            )
            .await;
            assert_eq!(response.status(), StatusCode::METHOD_NOT_ALLOWED);
            assert_eq!(response.headers()[header::ALLOW], "GET");
        }
        let response = handle(
            store,
            Request::builder().uri(format!("{ENDPOINT}?token=secret")).body(Body::empty()).unwrap(),
        )
        .await;
        assert_eq!(response.status(), StatusCode::BAD_REQUEST);
    }

    #[test]
    fn strict_document_validation_and_ca_constraints() {
        assert!(
            Snapshot::parse(serde_json::to_vec(&document("18446744073709551615")).unwrap()).is_ok()
        );
        for generation in ["0", "01", "-1", "1.0", "18446744073709551616"] {
            assert!(Snapshot::parse(serde_json::to_vec(&document(generation)).unwrap()).is_err());
        }
        let original = document("1");
        for (key, value) in [
            ("unknown", json!(1)),
            ("schema_version", json!(1.0)),
            ("generation", json!(1)),
            ("source_id", json!("with space")),
            ("issued_at", json!("2026-02-30T00:00:00Z")),
            ("certificates", Value::Null),
        ] {
            let mut bad = original.clone();
            bad[key] = value;
            assert!(Snapshot::parse(serde_json::to_vec(&bad).unwrap()).is_err(), "{key}");
        }
        let raw = serde_json::to_string(&original).unwrap();
        assert!(
            Snapshot::parse(raw.replacen("{", "{\"schema_version\":1,", 1).into_bytes()).is_err()
        );
        for (key, value) in [
            ("data", json!(format!("{ROOT}{ROOT}"))),
            ("data", json!(format!("\u{a0}{ROOT}"))),
            ("data", json!(ROOT.replacen('\n', "\n\u{c}", 1))),
            ("role", json!("leaf")),
            ("name", Value::Null),
            ("constraints", json!({})),
            ("constraints", json!({"permitted_dns_names":["*.example.com"]})),
            ("constraints", json!({"permitted_cidrs":["10.0.0.1/8"]})),
        ] {
            let mut bad = original.clone();
            bad["certificates"][0][key] = value;
            assert!(
                Snapshot::parse(serde_json::to_vec(&bad).unwrap()).is_err(),
                "certificate {key}"
            );
        }
        let mut good = original.clone();
        good["certificates"][0]["constraints"] = json!({"permitted_dns_names":["example.com", ".example.com"],
            "permitted_cidrs":["0.0.0.0/0", "10.0.0.1/32", "2001:db8::/32", "::1/128"]});
        assert!(Snapshot::parse(serde_json::to_vec(&good).unwrap()).is_ok());
        good["certificates"][0]["role"] = json!("intermediate");
        assert!(Snapshot::parse(serde_json::to_vec(&good).unwrap()).is_err());
        let mut duplicate = original.clone();
        duplicate["certificates"].as_array_mut().unwrap().push(original["certificates"][0].clone());
        assert!(Snapshot::parse(serde_json::to_vec(&duplicate).unwrap()).is_err());
    }

    #[test]
    fn limits_and_exact_der_are_enforced() {
        assert!(Snapshot::parse(vec![b' '; MAX_BODY + 1]).is_err());
        assert!(
            validate_depth(format!("{}0{}", "[".repeat(17), "]".repeat(17)).as_bytes()).is_err()
        );
        let mut value = document("1");
        let cert: Certificate = serde_json::from_value(value["certificates"][0].clone()).unwrap();
        let mut der = decode_certificate(&cert).unwrap();
        value["certificates"][0]["encoding"] = json!("der_base64");
        value["certificates"][0]["data"] = json!(STANDARD.encode(&der));
        assert!(Snapshot::parse(serde_json::to_vec(&value).unwrap()).is_ok());
        der.push(0);
        value["certificates"][0]["data"] = json!(STANDARD.encode(der));
        assert!(Snapshot::parse(serde_json::to_vec(&value).unwrap()).is_err());
    }

    #[test]
    fn constraints_match_client_canonicalization_and_duplicate_rules() {
        for valid in ["10.0.0.0/8", "2001:db8::/32", "::ffff:c000:200/120", "::1/128", "0.0.0.0/0"]
        {
            assert!(valid_cidr(valid), "{valid}");
        }
        for invalid in [
            "10.0.0.0/08",
            "2001:DB8::/32",
            "2001:0db8::/32",
            "2001:db8:0:0::/32",
            "::ffff:192.0.2.0/120",
            "10.0.0.1/8",
        ] {
            assert!(!valid_cidr(invalid), "{invalid}");
        }
        for constraints in [
            json!({"permitted_dns_names":["example.com", "example.com"]}),
            json!({"permitted_cidrs":["10.0.0.0/8", "10.0.0.0/8"]}),
        ] {
            let mut doc = document("1");
            doc["certificates"][0]["constraints"] = constraints;
            assert!(Snapshot::parse(serde_json::to_vec(&doc).unwrap()).is_err());
        }
    }

    #[test]
    fn legacy_expiry_is_optional_utc_metadata_without_an_interval_limit() {
        let mut value = document("1");
        value["issued_at"] = json!("2026-01-01T00:00:00Z");
        assert!(Snapshot::parse(serde_json::to_vec(&value).unwrap()).is_ok());
        for date in ["2000-01-01T00:00:00Z", "2026-01-01T00:00:00Z", "9999-12-31T23:59:59Z"] {
            value["expires_at"] = json!(date);
            assert!(Snapshot::parse(serde_json::to_vec(&value).unwrap()).is_ok());
        }
        for invalid in [
            Value::Null,
            json!(0),
            json!(""),
            json!("2026-02-30T00:00:00Z"),
            json!("2026-01-01T00:00:00+00:00"),
        ] {
            value["expires_at"] = invalid;
            assert!(Snapshot::parse(serde_json::to_vec(&value).unwrap()).is_err());
        }
        value["expires_at"] = json!("2000-01-01T00:00:00Z");
        let duplicate =
            value.to_string().replacen('{', "{\"expires_at\":\"2000-01-01T00:00:00Z\",", 1);
        assert!(Snapshot::parse(duplicate.into_bytes()).is_err());
    }
}
