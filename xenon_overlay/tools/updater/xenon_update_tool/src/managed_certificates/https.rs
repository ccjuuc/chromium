// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
//! Optional local TLS transport for the same validated CA snapshot store.
use super::Store;
use anyhow::{ensure, Context, Result};
use axum::{
    body::Body,
    extract::{Request, State},
    http::{header, StatusCode},
    response::Response,
    Router,
};
use axum_server::tls_rustls::RustlsConfig;
use serde::{Deserialize, Serialize};
use std::{net::TcpListener, path::PathBuf, sync::Arc};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Settings {
    pub port: u16,
    pub certificate_file: PathBuf,
    pub private_key_file: PathBuf,
}

impl Settings {
    pub fn validate(&self) -> Result<()> {
        ensure!(self.port != 0, "managed_certificates.https.port must be between 1 and 65535");
        ensure!(
            !self.certificate_file.as_os_str().is_empty()
                && !self.private_key_file.as_os_str().is_empty(),
            "managed_certificates.https requires certificate_file and private_key_file"
        );
        Ok(())
    }
}

#[derive(Clone)]
struct HttpsState {
    port: u16,
    store: Arc<Store>,
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

async fn handle_request(State(state): State<HttpsState>, request: Request) -> Response {
    let hosts = request.headers().get_all(header::HOST).iter().collect::<Vec<_>>();
    let valid_authority = |host: &str| {
        host == format!("127.0.0.1:{}", state.port)
            || host == format!("localhost:{}", state.port)
            || (state.port == 443 && matches!(host, "127.0.0.1" | "localhost"))
    };
    let authority = request.uri().authority().map(|value| value.as_str());
    // HTTP/2 carries :authority instead of Host. Reject duplicates or conflicts.
    let valid_host = match hosts.as_slice() {
        [] => authority.is_some_and(valid_authority),
        [host] => host
            .to_str()
            .is_ok_and(|host| valid_authority(host) && authority.is_none_or(|value| value == host)),
        _ => false,
    };
    if !valid_host
        || [
            "origin",
            "forwarded",
            "x-forwarded-for",
            "x-forwarded-host",
            "x-forwarded-proto",
            "x-real-ip",
        ]
        .iter()
        .any(|name| request.headers().contains_key(*name))
    {
        return error(StatusCode::FORBIDDEN, "Loopback native clients only");
    }
    if request.uri().path() != super::ENDPOINT {
        return error(StatusCode::NOT_FOUND, "Not found");
    }
    super::handle(state.store, request).await
}

pub struct Listener {
    listener: TcpListener,
    config: RustlsConfig,
    state: HttpsState,
}

impl Listener {
    /// Validate and bind before any serving task starts: a bad TLS configuration
    /// must not leave a seemingly successful HTTP-only service behind.
    pub async fn prepare(settings: Settings, store: Arc<Store>) -> Result<Self> {
        settings.validate()?;
        let config =
            RustlsConfig::from_pem_file(&settings.certificate_file, &settings.private_key_file)
                .await
                .context("Load managed CA HTTPS certificate and matching private key (PEM)")?;
        let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, settings.port))
            .context("Bind managed CA HTTPS loopback port")?;
        listener.set_nonblocking(true)?;
        Ok(Self { listener, config, state: HttpsState { port: settings.port, store } })
    }

    pub async fn serve(self, handle: axum_server::Handle<std::net::SocketAddr>) -> Result<()> {
        tracing::info!(endpoint = %format!("https://127.0.0.1:{}{}", self.state.port, super::ENDPOINT), "Managed CA HTTPS listener ready");
        axum_server::from_tcp_rustls(self.listener, self.config)?
            .handle(handle)
            .serve(
                Router::new().fallback(handle_request).with_state(self.state).into_make_service(),
            )
            .await
            .context("Serve managed CA HTTPS")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, time::Duration};

    fn fixture() -> (tempfile::TempDir, Settings, Arc<Store>, Vec<u8>) {
        let dir = tempfile::tempdir().unwrap();
        let certificate =
            rcgen::generate_simple_self_signed(vec!["localhost".into(), "127.0.0.1".into()])
                .unwrap();
        let cert = certificate.cert.pem().into_bytes();
        let certificate_file = dir.path().join("localhost.pem");
        let private_key_file = dir.path().join("localhost-key.pem");
        fs::write(&certificate_file, &cert).unwrap();
        fs::write(&private_key_file, certificate.signing_key.serialize_pem()).unwrap();
        let reservation = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).unwrap();
        let port = reservation.local_addr().unwrap().port();
        drop(reservation);
        let now = time::OffsetDateTime::now_utc().replace_nanosecond(0).unwrap();
        let format = time::format_description::well_known::Rfc3339;
        let snapshot_file = dir.path().join("current.json");
        fs::write(
            &snapshot_file,
            serde_json::json!({
                "schema_version": 1, "source_id": "tls-test", "generation": "1",
                "issued_at": now.format(&format).unwrap(),
                "certificates": []
            })
            .to_string(),
        )
        .unwrap();
        let store = Arc::new(
            Store::new(super::super::Settings { snapshot_file, ..Default::default() }).unwrap(),
        );
        (dir, Settings { port, certificate_file, private_key_file }, store, cert)
    }

    #[tokio::test]
    async fn tls_verifies_certificate_and_exposes_only_guarded_ca_get() {
        let (_dir, settings, store, cert) = fixture();
        let base = format!("https://127.0.0.1:{}", settings.port);
        let listener = Listener::prepare(settings, store).await.unwrap();
        let handle = axum_server::Handle::new();
        let task = tokio::spawn(listener.serve(handle.clone()));
        let client = reqwest::Client::builder()
            .no_proxy()
            .add_root_certificate(reqwest::Certificate::from_pem(&cert).unwrap())
            .timeout(Duration::from_secs(5))
            .build()
            .unwrap();
        let response =
            client.get(format!("{base}{}", super::super::ENDPOINT)).send().await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let etag = response.headers()[header::ETAG].clone();
        let bytes = response.bytes().await.unwrap();
        assert_eq!(serde_json::from_slice::<serde_json::Value>(&bytes).unwrap()["generation"], "1");
        assert_eq!(
            client
                .get(format!("{base}{}", super::super::ENDPOINT))
                .header(header::IF_NONE_MATCH, etag)
                .send()
                .await
                .unwrap()
                .status(),
            StatusCode::NOT_MODIFIED
        );
        for path in [
            "/",
            "/certificates",
            "/admin/certificates/state",
            "/admin/certificates/publish",
            "/service/update2/json",
        ] {
            assert_eq!(
                client.get(format!("{base}{path}")).send().await.unwrap().status(),
                StatusCode::NOT_FOUND
            );
        }
        for (name, value) in [
            ("host", "other.example"),
            ("origin", "https://other.example"),
            ("x-forwarded-for", "127.0.0.1"),
        ] {
            assert_eq!(
                client
                    .get(format!("{base}{}", super::super::ENDPOINT))
                    .header(name, value)
                    .send()
                    .await
                    .unwrap()
                    .status(),
                StatusCode::FORBIDDEN
            );
        }
        assert_eq!(
            client.post(format!("{base}{}", super::super::ENDPOINT)).send().await.unwrap().status(),
            StatusCode::METHOD_NOT_ALLOWED
        );
        assert_eq!(
            client
                .get(format!("{base}{}?x=1", super::super::ENDPOINT))
                .send()
                .await
                .unwrap()
                .status(),
            StatusCode::BAD_REQUEST
        );
        let untrusted =
            reqwest::Client::builder().no_proxy().timeout(Duration::from_secs(5)).build().unwrap();
        assert!(untrusted.get(format!("{base}{}", super::super::ENDPOINT)).send().await.is_err());
        handle.shutdown();
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn invalid_key_or_occupied_port_fails_preparation() {
        let (_dir, settings, store, _cert) = fixture();
        let occupied = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, settings.port)).unwrap();
        assert!(Listener::prepare(settings.clone(), store.clone()).await.is_err());
        drop(occupied);
        let other = rcgen::generate_simple_self_signed(vec!["localhost".into()]).unwrap();
        fs::write(&settings.private_key_file, other.signing_key.serialize_pem()).unwrap();
        assert!(Listener::prepare(settings.clone(), store.clone()).await.is_err());
        fs::write(&settings.private_key_file, "invalid private key").unwrap();
        assert!(Listener::prepare(settings, store).await.is_err());
    }
}
