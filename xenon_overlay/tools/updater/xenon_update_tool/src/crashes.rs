// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Crashpad-compatible report intake. Reports are private, outside the catalog.
use anyhow::{ensure, Result};
use axum::{
    body::{to_bytes, Body},
    extract::{DefaultBodyLimit, FromRequest, Multipart, Request},
    http::{header, Method, StatusCode},
    response::{IntoResponse, Response},
};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    fs,
    io::{Read, Write},
    path::PathBuf,
    sync::Arc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

pub const ENDPOINT: &str = "/api/v1/crashes";
const DEFAULT_LIMIT: usize = 64 * 1024 * 1024;
// Crashpad can capture 400 annotations per module. Bound retained ancillary
// data independently of the dump so a large module list cannot discard it.
const MAX_ANNOTATIONS: usize = 4096;
const MAX_ATTACHMENTS: usize = 128;
const MAX_ANNOTATION_BYTES: usize = 64 * 1024;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Settings {
    pub enabled: bool,
    pub directory: PathBuf,
    /// Applied independently to the wire body and decompressed body.
    pub max_upload_bytes: usize,
}

impl Default for Settings {
    fn default() -> Self {
        Self { enabled: true, directory: "crashes".into(), max_upload_bytes: DEFAULT_LIMIT }
    }
}

impl Settings {
    pub fn validate(&self) -> Result<()> {
        ensure!(!self.directory.as_os_str().is_empty(), "crashes.directory must not be empty");
        ensure!(
            (1024..=256 * 1024 * 1024).contains(&self.max_upload_bytes),
            "crashes.max_upload_bytes must be between 1024 and 268435456"
        );
        Ok(())
    }
}

pub struct Store {
    settings: Settings,
    slots: Arc<Semaphore>,
}

impl Store {
    pub fn new(settings: Settings) -> Result<Self> {
        settings.validate()?;
        fs::create_dir_all(&settings.directory)?;
        Ok(Self { settings, slots: Arc::new(Semaphore::new(2)) })
    }
}

type Failure = (StatusCode, &'static str);
type IntakeResult<T> = std::result::Result<T, Failure>;

struct Report {
    dump: Vec<u8>,
    fields: BTreeMap<String, String>,
    raw_fields: BTreeMap<String, String>,
    attachments: Vec<(String, String, Vec<u8>)>,
    dropped_fields: usize,
    query: Vec<(String, String)>,
    format: &'static str,
}

fn reply(status: StatusCode, body: String) -> Response {
    let mut response = (status, body).into_response();
    response
        .headers_mut()
        .insert(header::CONTENT_TYPE, "text/plain; charset=utf-8".parse().unwrap());
    response.headers_mut().insert(header::CACHE_CONTROL, "no-store".parse().unwrap());
    response.headers_mut().insert("x-content-type-options", "nosniff".parse().unwrap());
    response
}

pub async fn handle(store: Arc<Store>, request: Request) -> Response {
    if request.method() != Method::POST {
        let mut response = reply(StatusCode::METHOD_NOT_ALLOWED, "POST required".into());
        response.headers_mut().insert(header::ALLOW, "POST".parse().unwrap());
        return response;
    }
    let Ok(slot) = store.slots.clone().try_acquire_owned() else {
        return reply(StatusCode::SERVICE_UNAVAILABLE, "Crash receiver is busy".into());
    };
    match receive(&store, request, slot).await {
        Ok(id) => reply(StatusCode::OK, id),
        Err((status, message)) => reply(status, message.into()),
    }
}

async fn receive(
    store: &Store,
    request: Request,
    slot: OwnedSemaphorePermit,
) -> IntakeResult<String> {
    let limit = store.settings.max_upload_bytes;
    let (parts, body) = request.into_parts();
    let content_type = parts
        .headers
        .get(header::CONTENT_TYPE)
        .and_then(|v| v.to_str().ok())
        .unwrap_or("")
        .split(';')
        .next()
        .unwrap_or("")
        .trim()
        .to_ascii_lowercase();
    if content_type != "application/octet-stream" && content_type != "multipart/form-data" {
        return Err((StatusCode::UNSUPPORTED_MEDIA_TYPE, "Expected binary dump or multipart form"));
    }
    let encoding = parts
        .headers
        .get(header::CONTENT_ENCODING)
        .map(|v| v.to_str().unwrap_or("invalid"))
        .unwrap_or("identity")
        .trim()
        .to_ascii_lowercase();
    if encoding != "identity" && encoding != "gzip" {
        return Err((StatusCode::UNSUPPORTED_MEDIA_TYPE, "Unsupported content encoding"));
    }
    let query = parts.uri.query().unwrap_or("");
    if query.len() > 16 * 1024 {
        return Err((StatusCode::BAD_REQUEST, "Query exceeds limit"));
    }
    let query = url::form_urlencoded::parse(query.as_bytes()).into_owned().collect();
    let bytes = tokio::time::timeout(Duration::from_secs(120), to_bytes(body, limit))
        .await
        .map_err(|_| (StatusCode::REQUEST_TIMEOUT, "Upload timed out"))?
        .map_err(|_| {
            (StatusCode::PAYLOAD_TOO_LARGE, "Upload exceeds limit or body is incomplete")
        })?;
    let (bytes, slot) = if encoding == "gzip" {
        tokio::task::spawn_blocking(move || {
            let mut output = Vec::new();
            flate2::read::MultiGzDecoder::new(bytes.as_ref())
                .take(limit as u64 + 1)
                .read_to_end(&mut output)
                .map_err(|_| (StatusCode::BAD_REQUEST, "Invalid gzip body"))?;
            if output.len() > limit {
                return Err((StatusCode::PAYLOAD_TOO_LARGE, "Decompressed upload exceeds limit"));
            }
            // Keep the concurrency slot until this blocking task completes,
            // even if the HTTP request is cancelled while awaiting it.
            Ok((output, slot))
        })
        .await
        .map_err(|_| (StatusCode::INTERNAL_SERVER_ERROR, "Decompression failed"))??
    } else {
        (bytes.to_vec(), slot)
    };
    let mut report = Report {
        dump: Vec::new(),
        fields: BTreeMap::new(),
        raw_fields: BTreeMap::new(),
        attachments: Vec::new(),
        dropped_fields: 0,
        query,
        format: "binary",
    };
    if content_type == "application/octet-stream" {
        report.dump = bytes;
    } else {
        report.format = "multipart";
        let mut request = Request::from_parts(parts, Body::from(bytes));
        DefaultBodyLimit::max(limit).apply(&mut request);
        let mut multipart = Multipart::from_request(request, &())
            .await
            .map_err(|_| (StatusCode::BAD_REQUEST, "Invalid multipart boundary"))?;
        while let Some(field) = multipart
            .next_field()
            .await
            .map_err(|_| (StatusCode::BAD_REQUEST, "Invalid multipart body"))?
        {
            let name = field.name().unwrap_or("").to_owned();
            let filename = field.file_name().map(str::to_owned);
            if name.is_empty()
                || name.len() > 256
                || filename.as_ref().is_some_and(|v| v.len() > 1024)
            {
                return Err((StatusCode::BAD_REQUEST, "Invalid form field name"));
            }
            let value =
                field.bytes().await.map_err(|_| (StatusCode::BAD_REQUEST, "Invalid form field"))?;
            if name == "upload_file_minidump" {
                if !report.dump.is_empty() || value.is_empty() {
                    return Err((StatusCode::BAD_REQUEST, "Expected one nonempty minidump"));
                }
                report.dump = value.to_vec();
            } else if let Some(filename) = filename {
                if report.attachments.len() == MAX_ATTACHMENTS {
                    report.dropped_fields += 1;
                    continue;
                }
                report.attachments.push((name, filename, value.to_vec()));
            } else {
                if report.fields.len() == MAX_ANNOTATIONS {
                    report.dropped_fields += 1;
                    continue;
                }
                if value.len() > MAX_ANNOTATION_BYTES || report.fields.contains_key(&name) {
                    return Err((StatusCode::BAD_REQUEST, "Oversized or duplicate annotation"));
                }
                // Crashpad truncates StringAnnotation values by byte count,
                // potentially splitting UTF-8. Keep a readable view and the
                // exact bytes instead of rejecting the accompanying dump.
                let text = match std::str::from_utf8(&value) {
                    Ok(text) => text.to_owned(),
                    Err(_) => {
                        report.raw_fields.insert(name.clone(), STANDARD.encode(&value));
                        String::from_utf8_lossy(&value).into_owned()
                    }
                };
                report.fields.insert(name, text);
            }
        }
    }
    if report.dump.len() < 32 || !report.dump.starts_with(b"MDMP") {
        return Err((StatusCode::BAD_REQUEST, "Missing or invalid minidump header"));
    }
    let root = store.settings.directory.clone();
    tokio::task::spawn_blocking(move || {
        let _slot = slot;
        persist(root, report)
    })
    .await
    .map_err(|_| (StatusCode::INTERNAL_SERVER_ERROR, "Report writer failed"))?
    .map_err(|error| {
        tracing::error!(%error, "Cannot persist crash report");
        (StatusCode::INTERNAL_SERVER_ERROR, "Cannot persist crash report")
    })
}

fn write_file(path: PathBuf, bytes: &[u8]) -> Result<()> {
    let mut options = fs::OpenOptions::new();
    options.create_new(true).write(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = options.open(path)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    Ok(())
}

fn persist(root: PathBuf, report: Report) -> Result<String> {
    let mut random = [0u8; 16];
    getrandom::fill(&mut random).map_err(|e| anyhow::anyhow!("Report ID: {e}"))?;
    let id = hex::encode(random);
    let mut builder = tempfile::Builder::new();
    builder.prefix(".pending-");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        builder.permissions(fs::Permissions::from_mode(0o700));
    }
    let pending = builder.tempdir_in(&root)?;
    write_file(pending.path().join("minidump.dmp"), &report.dump)?;
    let mut attachments = Vec::<Value>::new();
    for (index, (name, original_name, bytes)) in report.attachments.iter().enumerate() {
        // Client names are metadata only, never filesystem paths.
        let filename = format!("attachment-{index:03}.bin");
        write_file(pending.path().join(&filename), bytes)?;
        attachments.push(json!({ "field": name, "original_name": original_name, "file": filename, "size": bytes.len() }));
    }
    let metadata = json!({
        "id": id, "received_at": SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
        "format": report.format, "dump_size": report.dump.len(),
        "annotations": report.fields, "annotations_base64": report.raw_fields,
        "dropped_fields": report.dropped_fields,
        "query": report.query, "attachments": attachments
    });
    write_file(pending.path().join("metadata.json"), &serde_json::to_vec_pretty(&metadata)?)?;
    #[cfg(unix)]
    fs::File::open(pending.path())?.sync_all()?;
    let destination = root.join(&id);
    ensure!(!destination.exists(), "Report ID collision");
    fs::rename(pending.path(), destination)?;
    #[cfg(unix)]
    fs::File::open(&root)?.sync_all()?;
    Ok(id)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dump() -> Vec<u8> {
        let mut bytes = vec![0; 32];
        bytes[..4].copy_from_slice(b"MDMP");
        bytes.extend_from_slice(b"binary\0dump\xffdata!");
        bytes
    }

    fn setup(limit: usize) -> (tempfile::TempDir, Arc<Store>) {
        let root = tempfile::tempdir().unwrap();
        let store = Store::new(Settings {
            directory: root.path().to_owned(),
            max_upload_bytes: limit,
            ..Settings::default()
        })
        .unwrap();
        (root, Arc::new(store))
    }

    fn gzip(bytes: &[u8]) -> Vec<u8> {
        let mut encoder = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::default());
        encoder.write_all(bytes).unwrap();
        encoder.finish().unwrap()
    }

    fn request(content_type: &str, encoding: &str, bytes: Vec<u8>) -> Request {
        Request::builder()
            .method(Method::POST)
            .uri(format!("{ENDPOINT}?product=Xenon&version=1"))
            .header(header::CONTENT_TYPE, content_type)
            .header(header::CONTENT_ENCODING, encoding)
            .body(Body::from(bytes))
            .unwrap()
    }

    fn multipart(fields: &[(&str, Option<&str>, &[u8])]) -> Vec<u8> {
        let mut bytes = Vec::new();
        for (name, filename, data) in fields {
            write!(bytes, "--BOUND\r\nContent-Disposition: form-data; name=\"{name}\"").unwrap();
            if let Some(filename) = filename {
                write!(bytes, "; filename=\"{filename}\"").unwrap();
            }
            bytes.extend_from_slice(b"\r\n\r\n");
            bytes.extend_from_slice(data);
            bytes.extend_from_slice(b"\r\n");
        }
        bytes.extend_from_slice(b"--BOUND--\r\n");
        bytes
    }

    async fn accepted(response: Response, root: &std::path::Path) -> PathBuf {
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()[header::CONTENT_TYPE], "text/plain; charset=utf-8");
        let body = to_bytes(response.into_body(), 128).await.unwrap();
        let id = std::str::from_utf8(&body).unwrap();
        assert_eq!(id.len(), 32);
        assert!(id.bytes().all(|b| b.is_ascii_hexdigit()));
        let directory = root.join(id);
        assert_eq!(fs::read(directory.join("minidump.dmp")).unwrap(), dump());
        let metadata: Value =
            serde_json::from_slice(&fs::read(directory.join("metadata.json")).unwrap()).unwrap();
        assert_eq!(metadata["id"], id);
        assert_eq!(metadata["query"][0], json!(["product", "Xenon"]));
        directory
    }

    #[tokio::test]
    async fn binary_and_gzip_roundtrip_use_distinct_report_ids() {
        let (root, store) = setup(DEFAULT_LIMIT);
        let first = accepted(
            handle(store.clone(), request("application/octet-stream", "identity", dump())).await,
            root.path(),
        )
        .await;
        let second = accepted(
            handle(store, request("application/octet-stream", "gzip", gzip(&dump()))).await,
            root.path(),
        )
        .await;
        assert_ne!(first, second);
        assert_eq!(fs::read_dir(root.path()).unwrap().count(), 2);
    }

    #[tokio::test]
    async fn multipart_and_gzip_preserve_annotations_and_safe_attachments() {
        let (root, store) = setup(DEFAULT_LIMIT);
        let body = multipart(&[
            ("prod", None, b"Xenon"),
            ("upload_file_minidump", Some("../../outside.dmp"), &dump()),
            ("../log", Some("../../outside.log"), b"log\0bytes"),
        ]);
        for compressed in [false, true] {
            let (encoding, bytes) =
                if compressed { ("gzip", gzip(&body)) } else { ("identity", body.clone()) };
            let response = handle(
                store.clone(),
                request("multipart/form-data; boundary=BOUND", encoding, bytes),
            )
            .await;
            let directory = accepted(response, root.path()).await;
            assert_eq!(fs::read(directory.join("attachment-000.bin")).unwrap(), b"log\0bytes");
            assert_eq!(fs::read_dir(&directory).unwrap().count(), 3);
            let metadata: Value =
                serde_json::from_slice(&fs::read(directory.join("metadata.json")).unwrap())
                    .unwrap();
            assert_eq!(metadata["annotations"]["prod"], "Xenon");
            assert_eq!(metadata["attachments"][0]["original_name"], "../../outside.log");
        }
    }

    #[tokio::test]
    async fn byte_truncated_utf8_keeps_dump_and_original_annotation_bytes() {
        let (root, store) = setup(DEFAULT_LIMIT);
        let text = "中".repeat(22);
        let truncated = &text.as_bytes()[..64];
        assert!(std::str::from_utf8(truncated).is_err());
        let body = multipart(&[
            ("switch-1", None, truncated),
            ("prod", None, b"Xenon"),
            ("upload_file_minidump", Some("report.dmp"), &dump()),
        ]);
        let directory = accepted(
            handle(store, request("multipart/form-data; boundary=BOUND", "gzip", gzip(&body)))
                .await,
            root.path(),
        )
        .await;
        let metadata: Value =
            serde_json::from_slice(&fs::read(directory.join("metadata.json")).unwrap()).unwrap();
        assert_eq!(metadata["annotations"]["switch-1"], format!("{}\u{fffd}", "中".repeat(21)));
        assert_eq!(metadata["annotations"]["prod"], "Xenon");
        assert_eq!(
            STANDARD.decode(metadata["annotations_base64"]["switch-1"].as_str().unwrap()).unwrap(),
            truncated
        );
        assert_eq!(metadata["annotations_base64"].as_object().unwrap().len(), 1);
        assert_eq!(metadata["dropped_fields"], 0);
    }

    #[tokio::test]
    async fn accepts_a_full_crashpad_module_annotation_list() {
        let (root, store) = setup(DEFAULT_LIMIT);
        // snapshot_constants.h permits 400 annotation objects per module,
        // in addition to process annotations and the minidump file itself.
        let names: Vec<_> = (0..400).map(|index| format!("feature-{index}")).collect();
        let mut fields: Vec<_> =
            names.iter().map(|name| (name.as_str(), None, &b"enabled"[..])).collect();
        let minidump = dump();
        fields.push(("upload_file_minidump", Some("report.dmp"), &minidump));
        let directory = accepted(
            handle(
                store,
                request("multipart/form-data; boundary=BOUND", "identity", multipart(&fields)),
            )
            .await,
            root.path(),
        )
        .await;
        let metadata: Value =
            serde_json::from_slice(&fs::read(directory.join("metadata.json")).unwrap()).unwrap();
        assert_eq!(metadata["annotations"].as_object().unwrap().len(), 400);
        assert_eq!(metadata["annotations"]["feature-399"], "enabled");
        assert_eq!(metadata["dropped_fields"], 0);
    }

    #[tokio::test]
    async fn ancillary_limits_keep_the_minidump_even_when_it_is_the_last_field() {
        let (root, store) = setup(DEFAULT_LIMIT);
        let names: Vec<_> = (0..MAX_ANNOTATIONS + 1).map(|index| format!("key-{index}")).collect();
        let mut fields: Vec<_> =
            names.iter().map(|name| (name.as_str(), None, &b"value"[..])).collect();
        for _ in 0..MAX_ATTACHMENTS + 1 {
            fields.push(("log", Some("log.txt"), b"log bytes"));
        }
        let minidump = dump();
        fields.push(("upload_file_minidump", Some("report.dmp"), &minidump));
        let directory = accepted(
            handle(
                store,
                request("multipart/form-data; boundary=BOUND", "identity", multipart(&fields)),
            )
            .await,
            root.path(),
        )
        .await;
        let metadata: Value =
            serde_json::from_slice(&fs::read(directory.join("metadata.json")).unwrap()).unwrap();
        assert_eq!(metadata["annotations"].as_object().unwrap().len(), MAX_ANNOTATIONS);
        assert_eq!(metadata["attachments"].as_array().unwrap().len(), MAX_ATTACHMENTS);
        assert_eq!(metadata["dropped_fields"], 2);
        assert_eq!(fs::read_dir(directory).unwrap().count(), MAX_ATTACHMENTS + 2);
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn saved_reports_are_private_to_the_service_user() {
        use std::os::unix::fs::PermissionsExt;
        let (root, store) = setup(DEFAULT_LIMIT);
        let body = multipart(&[
            ("upload_file_minidump", Some("report.dmp"), &dump()),
            ("log", Some("log.txt"), b"log bytes"),
        ]);
        let directory = accepted(
            handle(store, request("multipart/form-data; boundary=BOUND", "identity", body)).await,
            root.path(),
        )
        .await;
        assert_eq!(fs::metadata(&directory).unwrap().permissions().mode() & 0o077, 0);
        for entry in fs::read_dir(directory).unwrap() {
            assert_eq!(entry.unwrap().metadata().unwrap().permissions().mode() & 0o077, 0);
        }
    }

    #[tokio::test]
    async fn rejects_malformed_reports_without_creating_files() {
        let (root, store) = setup(DEFAULT_LIMIT);
        let cases = [
            request("application/octet-stream", "identity", Vec::new()),
            request("application/octet-stream", "identity", vec![0; 48]),
            request("application/octet-stream", "gzip", b"bad gzip".to_vec()),
            request("multipart/form-data", "identity", dump()),
            request(
                "multipart/form-data; boundary=BOUND",
                "identity",
                multipart(&[("prod", None, b"Xenon")]),
            ),
            request(
                "multipart/form-data; boundary=BOUND",
                "identity",
                multipart(&[
                    ("upload_file_minidump", None, &dump()),
                    ("upload_file_minidump", None, &dump()),
                ]),
            ),
            request(
                "multipart/form-data; boundary=BOUND",
                "identity",
                multipart(&[
                    ("upload_file_minidump", None, &dump()),
                    ("prod", None, b"one"),
                    ("prod", None, b"two"),
                ]),
            ),
        ];
        for request in cases {
            assert_eq!(handle(store.clone(), request).await.status(), StatusCode::BAD_REQUEST);
        }
        assert_eq!(fs::read_dir(root.path()).unwrap().count(), 0);
    }

    #[tokio::test]
    async fn compressed_and_wire_limits_are_enforced() {
        let (root, store) = setup(1024);
        let mut oversized = dump();
        oversized.resize(1025, 0);
        for (encoding, bytes) in [("identity", oversized.clone()), ("gzip", gzip(&oversized))] {
            assert_eq!(
                handle(store.clone(), request("application/octet-stream", encoding, bytes))
                    .await
                    .status(),
                StatusCode::PAYLOAD_TOO_LARGE
            );
        }
        assert_eq!(fs::read_dir(root.path()).unwrap().count(), 0);
    }

    #[tokio::test]
    async fn rejects_unsupported_protocol_and_excess_concurrency() {
        let (_root, store) = setup(DEFAULT_LIMIT);
        for req in [
            request("application/json", "identity", dump()),
            request("application/octet-stream", "br", dump()),
        ] {
            assert_eq!(
                handle(store.clone(), req).await.status(),
                StatusCode::UNSUPPORTED_MEDIA_TYPE
            );
        }
        let mut req = request("application/octet-stream", "identity", dump());
        *req.method_mut() = Method::GET;
        assert_eq!(handle(store.clone(), req).await.status(), StatusCode::METHOD_NOT_ALLOWED);
        let _permits = store.slots.acquire_many(2).await.unwrap();
        assert_eq!(
            handle(store.clone(), request("application/octet-stream", "identity", dump()))
                .await
                .status(),
            StatusCode::SERVICE_UNAVAILABLE
        );
    }

    #[tokio::test]
    async fn unavailable_storage_never_returns_success() {
        let (root, store) = setup(DEFAULT_LIMIT);
        fs::remove_dir(root.path()).unwrap();
        assert_eq!(
            handle(store, request("application/octet-stream", "identity", dump())).await.status(),
            StatusCode::INTERNAL_SERVER_ERROR
        );
    }
}
