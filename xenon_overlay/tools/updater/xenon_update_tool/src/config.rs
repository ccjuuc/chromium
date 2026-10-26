// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
use anyhow::{ensure, Context, Result};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    env, fs,
    io::Write,
    path::{Path, PathBuf},
};

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Packaging {
    pub key_path: Option<PathBuf>,
    pub tools_dir: Option<PathBuf>,
    pub python: Option<PathBuf>,
    pub openssl: Option<PathBuf>,
    pub team_id: Option<String>,
    #[serde(default)]
    pub development: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub schema_version: u32,
    pub port: u16,
    /// Separate loopback management port when public_origin is configured.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub admin_port: Option<u16>,
    pub manifest: PathBuf,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub public_origin: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub puffin: Option<PathBuf>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub crx_public_key_sha256: Option<String>,
    #[serde(default)]
    pub packaging: Packaging,
    #[serde(default)]
    pub windows: crate::windows::Settings,
    #[serde(default)]
    pub crashes: crate::crashes::Settings,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub managed_certificates: Option<crate::managed_certificates::Settings>,
}

pub struct InitOptions {
    pub public_origin: Option<String>,
    pub port: u16,
    pub build_dir: Option<PathBuf>,
    pub puffin: Option<PathBuf>,
    pub key_hash: Option<String>,
    pub artifacts: Option<PathBuf>,
}

impl Default for InitOptions {
    fn default() -> Self {
        Self {
            public_origin: None,
            port: 9001,
            build_dir: None,
            puffin: None,
            key_hash: None,
            artifacts: None,
        }
    }
}

pub fn validate_origin(origin: &str) -> Result<()> {
    let url = url::Url::parse(origin).context("Invalid public origin")?;
    ensure!(url.scheme() == "https" && url.username().is_empty() && url.password().is_none()
        && url.query().is_none() && url.fragment().is_none()
        && url.origin().ascii_serialization() == origin,
        "public-origin must be an HTTPS origin without credentials, trailing slash, path, query or fragment");
    Ok(())
}

pub fn normalize_hash(input: &str) -> Result<String> {
    let bytes = if input.len() == 64 && input.bytes().all(|b| b.is_ascii_hexdigit()) {
        hex::decode(input)?
    } else {
        STANDARD.decode(input).context("Key hash must be hex or standard base64")?
    };
    ensure!(bytes.len() == 32, "Publisher key hash must contain exactly 32 bytes");
    Ok(STANDARD.encode(bytes))
}

fn absolute(path: &Path) -> Result<PathBuf> {
    Ok(if path.is_absolute() { path.to_owned() } else { env::current_dir()?.join(path) })
}

/// Rust canonicalization uses Windows verbatim paths, which OpenSSL and some
/// other external tools interpret as URI schemes rather than filesystem paths.
pub fn external_path(path: &Path) -> PathBuf {
    #[cfg(windows)]
    {
        use std::{
            ffi::OsString,
            os::windows::ffi::{OsStrExt, OsStringExt},
            path::{Component, Prefix},
        };
        let wide: Vec<u16> = path.as_os_str().encode_wide().collect();
        if let Some(Component::Prefix(prefix)) = path.components().next() {
            match prefix.kind() {
                Prefix::VerbatimDisk(_) => {
                    return OsString::from_wide(&wide[4..]).into();
                }
                Prefix::VerbatimUNC(_, _) => {
                    let mut normal = vec![b'\\' as u16; 2];
                    normal.extend_from_slice(&wide[8..]);
                    return OsString::from_wide(&normal).into();
                }
                _ => {}
            }
        }
    }
    path.to_owned()
}

fn read_json(path: &Path, max: u64) -> Result<Value> {
    ensure!(fs::metadata(path)?.len() <= max, "JSON file is too large: {}", path.display());
    serde_json::from_slice(&fs::read(path)?)
        .with_context(|| format!("Invalid JSON: {}", path.display()))
}

pub fn load(path: &Path) -> Result<Config> {
    let value = read_json(path, 64 * 1024)
        .with_context(|| format!("Cannot read config {}; run init first", path.display()))?;
    let mut config: Config = serde_json::from_value(value)?;
    ensure!(config.schema_version == 1, "Unsupported config schema_version");
    ensure!(config.port != 0, "port must be between 1 and 65535");
    ensure!(
        config.admin_port.is_none_or(|port| port != 0 && port != config.port),
        "admin_port must be a different port between 1 and 65535"
    );
    if let Some(origin) = &config.public_origin {
        validate_origin(origin)?;
    }
    if let Some(hash) = &config.crx_public_key_sha256 {
        config.crx_public_key_sha256 = Some(normalize_hash(hash)?);
    }
    config.windows.validate()?;
    let root = absolute(path)?.parent().context("Config has no parent directory")?.to_owned();
    if let Some(settings) = &mut config.managed_certificates {
        settings.validate()?;
        if settings.snapshot_file.is_relative() {
            settings.snapshot_file = root.join(&settings.snapshot_file);
        }
        if let Some(https) = &mut settings.https {
            let management_port = config.public_origin.as_ref().map(|_| {
                config.admin_port.unwrap_or(if config.port == 65535 {
                    65534
                } else {
                    config.port + 1
                })
            });
            ensure!(
                https.port != config.port && Some(https.port) != management_port,
                "managed_certificates.https.port must differ from HTTP and management ports"
            );
            for path in [&mut https.certificate_file, &mut https.private_key_file] {
                if path.is_relative() {
                    *path = root.join(&*path);
                }
            }
        }
    }
    config.crashes.validate()?;
    if config.crashes.directory.is_relative() {
        config.crashes.directory = root.join(&config.crashes.directory);
    }
    if config.manifest.is_relative() {
        config.manifest = root.join(config.manifest);
    }
    for path in [
        &mut config.puffin,
        &mut config.packaging.key_path,
        &mut config.packaging.tools_dir,
        &mut config.packaging.python,
        &mut config.packaging.openssl,
        &mut config.windows.node,
        &mut config.windows.seven_zip,
        &mut config.windows.zucchini,
    ]
    .into_iter()
    .flatten()
    {
        if path.is_relative() {
            *path = root.join(&*path);
        }
        *path = external_path(path);
    }
    Ok(config)
}

fn executable_name() -> &'static str {
    if cfg!(windows) {
        "puffin.exe"
    } else {
        "puffin"
    }
}

fn is_executable(path: &Path) -> bool {
    let Ok(metadata) = fs::metadata(path) else { return false };
    if !metadata.is_file() {
        return false;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if metadata.permissions().mode() & 0o111 == 0 {
            return false;
        }
    }
    true
}

pub fn find_puffin(
    explicit: Option<&Path>,
    configured: Option<&Path>,
    build_dir: Option<&Path>,
) -> Result<Option<PathBuf>> {
    if let Some(path) = explicit.or(configured) {
        ensure!(
            is_executable(path),
            "Puffin executable does not exist or is not executable: {}",
            path.display()
        );
        return Ok(Some(external_path(&fs::canonicalize(path)?)));
    }
    let name = executable_name();
    let mut candidates = Vec::new();
    if let Some(dir) = build_dir {
        candidates.push(dir.join(name));
    }
    if let Some(path) = env::var_os("XENON_PUFFIN_TOOL") {
        candidates.push(path.into());
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(parent) = exe.parent() {
            candidates.push(parent.join(name));
        }
    }
    let cwd = env::current_dir()?;
    for root in cwd.ancestors() {
        if root.join("third_party/puffin/BUILD.gn").is_file() {
            let dirs = if cfg!(target_arch = "aarch64") {
                ["updater_tools_arm64", "release_arm64", "Release", "Default", "Release_64"]
            } else {
                ["Release_64", "updater_tools_x64", "Release", "Default", "release_x64"]
            };
            for dir in dirs {
                candidates.push(root.join("out").join(dir).join(name));
            }
            break;
        }
    }
    if let Some(paths) = env::var_os("PATH") {
        for path in env::split_paths(&paths) {
            candidates.push(path.join(name));
        }
    }
    for candidate in candidates {
        if is_executable(&candidate) {
            return Ok(Some(external_path(&fs::canonicalize(candidate)?)));
        }
    }
    Ok(None)
}

fn gn_string(value: &str) -> String {
    format!("\"{}\"", value.replace('\\', "\\\\").replace('"', "\\\"").replace('$', "\\$"))
}

fn gn_settings(config: &Config) -> Option<String> {
    let hash = config.crx_public_key_sha256.as_ref()?;
    let origin =
        config.public_origin.clone().unwrap_or_else(|| format!("http://127.0.0.1:{}", config.port));
    Some(format!(
        "# macOS only: copy into the macOS browser build's args.gn, then run gn gen.\n\
        enable_xenon_updater = true\nxenon_updater_update_url = {}\n\
        xenon_updater_crx_public_key_hash = {}\nxenon_updater_allow_insecure_localhost = {}\n",
        gn_string(&format!("{origin}/service/update2/json")),
        gn_string(hash),
        config.public_origin.is_none()
    ))
}

fn gn_key_from_build(dir: &Path) -> Option<String> {
    let text = fs::read_to_string(dir.join("args.gn")).ok()?;
    text.lines().find_map(|line| {
        let (name, value) = line.trim().split_once('=')?;
        if name.trim() != "xenon_updater_crx_public_key_hash" {
            return None;
        }
        // Only read an unambiguous literal. Never evaluate GN source.
        let value = value.trim().strip_prefix('"')?;
        let (literal, tail) = value.split_once('"')?;
        if !tail.trim().is_empty() && !tail.trim().starts_with('#') {
            return None;
        }
        normalize_hash(literal).ok()
    })
}

fn create_file(path: &Path, bytes: &[u8]) -> Result<()> {
    let root = path.parent().context("Output has no parent")?;
    let mut file = tempfile::NamedTempFile::new_in(root)?;
    file.write_all(bytes)?;
    file.as_file().sync_all()?;
    file.persist_noclobber(path)
        .with_context(|| format!("Refusing to overwrite {}", path.display()))?;
    Ok(())
}

pub fn initialize(path: &Path, options: InitOptions) -> Result<Value> {
    ensure!(options.port != 0, "port must be between 1 and 65535");
    if let Some(origin) = &options.public_origin {
        validate_origin(origin)?;
    }
    let path = absolute(path)?;
    ensure!(
        !path.exists(),
        "Config already exists: {}; edit it or choose another --config",
        path.display()
    );
    let root = path.parent().context("Config has no parent")?;
    let catalog = root.join("releases/catalog.json");
    let gn = root.join("client-updater.gni");
    ensure!(path != gn && path != catalog, "Config path conflicts with a generated file");
    ensure!(
        !catalog.exists() && !gn.exists(),
        "Generated catalog or GN settings already exist; use a new config directory"
    );
    let mut key = options.key_hash.as_deref().map(normalize_hash).transpose()?;
    if let Some(artifact) = &options.artifacts {
        let value = read_json(artifact, 16 * 1024 * 1024)?;
        key = Some(normalize_hash(
            value
                .get("crx_public_key_sha256")
                .or_else(|| value.get("crx").and_then(|crx| crx.get("public_key_sha256")))
                .unwrap_or(&Value::Null)
                .as_str()
                .context("artifacts.json is missing crx_public_key_sha256")?,
        )?);
    }
    if key.is_none() {
        key = options.build_dir.as_deref().and_then(gn_key_from_build);
    }
    let puffin = find_puffin(options.puffin.as_deref(), None, options.build_dir.as_deref())?;
    let managed_certificates = crate::managed_certificates::Settings::default();
    let snapshot = root.join(&managed_certificates.snapshot_file);
    let config = Config {
        schema_version: 1,
        port: options.port,
        admin_port: None,
        manifest: PathBuf::from("releases/catalog.json"),
        public_origin: options.public_origin,
        puffin,
        crx_public_key_sha256: key,
        packaging: Packaging::default(),
        windows: crate::windows::Settings::default(),
        crashes: crate::crashes::Settings::default(),
        managed_certificates: Some(managed_certificates),
    };
    let settings = gn_settings(&config);
    fs::create_dir_all(snapshot.parent().unwrap())?;
    fs::create_dir_all(catalog.parent().unwrap())?;
    create_file(&catalog, b"{\n  \"schema_version\": 1,\n  \"apps\": []\n}\n")?;
    if let Some(settings) = &settings {
        create_file(&gn, settings.as_bytes())?;
    }
    create_file(&path, format!("{}\n", serde_json::to_string_pretty(&config)?).as_bytes())?;
    let mut warnings = vec![];
    if config.puffin.is_none() {
        warnings.push("Puffin was not found; serving and full releases work. Set puffin in the config or use --puffin for differential generation.");
    }
    if settings.is_none() {
        warnings.push("Publisher key hash not supplied; client GN settings were not generated. Use --key-hash or --artifacts when initializing a new config, or configure the browser build explicitly.");
    }
    Ok(json!({ "config": path, "manifest": catalog, "puffin": config.puffin,
        "client_gn": settings.map(|_| gn), "warnings": warnings,
        "endpoint": format!("{}/service/update2/json", config.public_origin.unwrap_or_else(|| format!("http://127.0.0.1:{}", config.port))) }))
}

pub fn doctor(path: &Path) -> Result<Value> {
    let config = load(path)?;
    crate::catalog::load(&config.manifest).context("Published catalog failed validation")?;
    let windows_root = absolute(path)?.parent().unwrap().join("publisher/windows");
    crate::windows::load(&windows_root).context("Published Windows catalog failed validation")?;
    let windows_history = crate::windows::list(&windows_root)?;
    let puffin = find_puffin(None, config.puffin.as_deref(), None)?;
    let mut warnings = vec![];
    if puffin.is_none() {
        warnings.push("Puffin unavailable; automatic publication uses full CRX only");
    }
    if config.crx_public_key_sha256.is_none() {
        warnings.push("No client publisher key hash configured");
    }
    let metadata = read_json(&config.manifest, 64 * 1024)?;
    let apps = metadata.get("apps").cloned().unwrap_or_else(|| json!([metadata]));
    Ok(json!({ "ok": true, "config": absolute(path)?, "manifest": config.manifest,
        "puffin": puffin, "apps": apps, "warnings": warnings, "windows_history": windows_history,
        "note": "Artifact hashes and containers verified. CRX publisher signatures are enforced by Chromium clients; doctor does not replace signature verification." }))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(windows)]
    #[test]
    fn external_tool_paths_support_existing_verbatim_configs() {
        assert_eq!(
            external_path(Path::new(r"\\?\H:\发布 tools\key.pk8")),
            PathBuf::from(r"H:\发布 tools\key.pk8")
        );
        assert_eq!(
            external_path(Path::new(r"\\?\UNC\server\share\key.pk8")),
            PathBuf::from(r"\\server\share\key.pk8")
        );
        assert_eq!(external_path(Path::new(r"\\.\pipe\name")), PathBuf::from(r"\\.\pipe\name"));
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("config.json");
        fs::write(&path, serde_json::to_vec(&json!({
            "schema_version": 1, "port": 9001, "manifest": "catalog.json",
            "packaging": {"key_path": r"\\?\H:\发布 tools\key.pk8", "tools_dir": r"\\?\UNC\server\share"}
        })).unwrap()).unwrap();
        let config = load(&path).unwrap();
        assert_eq!(config.packaging.key_path.unwrap(), PathBuf::from(r"H:\发布 tools\key.pk8"));
        assert_eq!(config.packaging.tools_dir.unwrap(), PathBuf::from(r"\\server\share"));
    }
    fn options() -> InitOptions {
        InitOptions {
            public_origin: None,
            port: 9001,
            build_dir: None,
            puffin: None,
            key_hash: Some("ab".repeat(32)),
            artifacts: None,
        }
    }
    #[test]
    fn origin_requires_exact_https_origin() {
        assert!(validate_origin("https://updates.example.com").is_ok());
        for bad in [
            "http://example.com",
            "https://example.com/",
            "https://u:p@example.com",
            "https://example.com/a",
            "https://example.com?q=1",
            "https://example.com#x",
        ] {
            assert!(validate_origin(bad).is_err(), "{bad}");
        }
    }
    #[test]
    fn hashes_are_exact_and_canonical() {
        assert_eq!(
            normalize_hash(&"ab".repeat(32)).unwrap(),
            normalize_hash(&STANDARD.encode([0xab; 32])).unwrap()
        );
        assert!(normalize_hash(&"ab".repeat(31)).is_err());
        assert!(normalize_hash("$(bad)").is_err());
    }
    #[test]
    fn init_is_nondestructive_and_config_paths_are_relative_to_file() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("state/service.json");
        initialize(&path, options()).unwrap();
        let config = load(&path).unwrap();
        assert_eq!(config.manifest, path.parent().unwrap().join("releases/catalog.json"));
        assert_eq!(config.crashes.directory, path.parent().unwrap().join("crashes"));
        assert!(config.crashes.enabled);
        let certificates = config.managed_certificates.as_ref().unwrap();
        assert_eq!(
            certificates.snapshot_file,
            path.parent().unwrap().join("managed-ca/current.json")
        );
        assert!(certificates.bearer_token_env.is_none());
        assert!(gn_settings(&config)
            .unwrap()
            .contains("xenon_updater_allow_insecure_localhost = true"));
        let before = fs::read(&path).unwrap();
        assert!(initialize(&path, options()).is_err());
        assert_eq!(before, fs::read(path).unwrap());
    }
    #[test]
    fn default_initialization_prepares_managed_certificate_directory_without_a_snapshot() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("state/config.json");
        let options = InitOptions::default();
        assert_eq!(options.port, 9001);
        assert!(options.public_origin.is_none());
        assert!(options.build_dir.is_none());
        assert!(options.puffin.is_none());
        assert!(options.key_hash.is_none());
        assert!(options.artifacts.is_none());
        initialize(&path, options).unwrap();
        let config = load(&path).unwrap();
        assert_eq!(config.port, 9001);
        let snapshot = config.managed_certificates.unwrap().snapshot_file;
        assert_eq!(snapshot, path.parent().unwrap().join("managed-ca/current.json"));
        assert!(snapshot.parent().unwrap().is_dir());
        assert!(!snapshot.exists());
        let serialized: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        assert_eq!(
            serialized["managed_certificates"],
            json!({"snapshot_file": "managed-ca/current.json"})
        );
    }
    #[test]
    fn initialization_preserves_existing_managed_certificate_snapshot() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("config.json");
        let snapshot = temp.path().join("managed-ca/current.json");
        fs::create_dir(snapshot.parent().unwrap()).unwrap();
        let existing = b"existing certificate snapshot";
        fs::write(&snapshot, existing).unwrap();
        initialize(&path, InitOptions::default()).unwrap();
        assert_eq!(fs::read(&snapshot).unwrap(), existing);
    }
    #[test]
    fn trusted_packaging_metadata_generates_production_gn() {
        let temp = tempfile::tempdir().unwrap();
        let artifacts = temp.path().join("artifacts.json");
        fs::write(&artifacts, json!({"crx_public_key_sha256":"cd".repeat(32)}).to_string())
            .unwrap();
        let mut opts = options();
        opts.key_hash = None;
        opts.artifacts = Some(artifacts);
        opts.public_origin = Some("https://updates.example.com".into());
        let path = temp.path().join("state/config.json");
        initialize(&path, opts).unwrap();
        let settings =
            fs::read_to_string(path.parent().unwrap().join("client-updater.gni")).unwrap();
        assert!(settings.contains("https://updates.example.com/service/update2/json"));
        assert!(settings.contains("xenon_updater_allow_insecure_localhost = false"));
        assert!(settings.contains(&STANDARD.encode([0xcd; 32])));
    }
    #[test]
    fn explicit_missing_tool_is_an_error() {
        assert!(find_puffin(Some(Path::new("missing-puffin-binary")), None, None).is_err());
    }
    #[test]
    fn config_rejects_unknown_fields_and_invalid_port() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("config.json");
        fs::write(&path, r#"{"schema_version":1,"port":0,"manifest":"catalog.json"}"#).unwrap();
        assert!(load(&path).is_err());
        fs::write(
            &path,
            r#"{"schema_version":1,"port":9001,"manifest":"catalog.json","host":"0.0.0.0"}"#,
        )
        .unwrap();
        assert!(load(&path).is_err());
    }

    #[test]
    fn managed_certificate_config_resolves_relative_path_and_validates_names() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("config.json");
        let mut value = json!({"schema_version":1, "port":9001, "manifest":"catalog.json",
            "managed_certificates":{"snapshot_file":"managed-ca/current.json", "bearer_token_env":"XENON_CA_TOKEN"}});
        fs::write(&path, value.to_string()).unwrap();
        let config = load(&path).unwrap().managed_certificates.unwrap();
        assert_eq!(config.snapshot_file, temp.path().join("managed-ca/current.json"));
        assert_eq!(config.bearer_token_env.as_deref(), Some("XENON_CA_TOKEN"));
        for invalid in [
            json!({"snapshot_file":""}),
            json!({"snapshot_file":null}),
            json!({"snapshot_file":9001}),
            json!({"snapshot_file":"ca.json", "bearer_token":"secret"}),
            json!({"bearer_token":"secret"}),
            json!({"bearer_token_env":""}),
            json!({"snapshot_file":"ca.json", "bearer_token_env":"1TOKEN"}),
            json!({"snapshot_file":"ca.json", "bearer_token_env":"XENON=TOKEN"}),
        ] {
            value["managed_certificates"] = invalid;
            fs::write(&path, value.to_string()).unwrap();
            assert!(load(&path).is_err());
        }
    }

    #[test]
    fn managed_certificate_https_paths_and_ports_are_validated() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("config.json");
        let mut value = json!({"schema_version":1, "port":9001, "manifest":"catalog.json",
            "managed_certificates":{"https":{"port":9443,
                "certificate_file":"tls/localhost.pem", "private_key_file":"tls/localhost-key.pem"}}});
        fs::write(&path, value.to_string()).unwrap();
        let https = load(&path).unwrap().managed_certificates.unwrap().https.unwrap();
        assert_eq!(https.port, 9443);
        assert_eq!(https.certificate_file, temp.path().join("tls/localhost.pem"));
        assert_eq!(https.private_key_file, temp.path().join("tls/localhost-key.pem"));
        for invalid in [
            json!({"port":0,"certificate_file":"cert.pem","private_key_file":"key.pem"}),
            json!({"port":9001,"certificate_file":"cert.pem","private_key_file":"key.pem"}),
            json!({"port":9443,"certificate_file":"","private_key_file":"key.pem"}),
            json!({"port":9443,"certificate_file":"cert.pem","private_key_file":""}),
            json!({"port":9443,"certificate_file":"cert.pem"}),
            json!({"port":9443,"certificate_file":"cert.pem","private_key_file":"key.pem","host":"0.0.0.0"}),
        ] {
            value["managed_certificates"]["https"] = invalid;
            fs::write(&path, value.to_string()).unwrap();
            assert!(load(&path).is_err());
        }
        value["managed_certificates"]["https"] =
            json!({"port":9002,"certificate_file":"cert.pem","private_key_file":"key.pem"});
        value["public_origin"] = json!("https://updates.example.com");
        fs::write(&path, value.to_string()).unwrap();
        assert!(load(&path).is_err());
        value["admin_port"] = json!(9003);
        fs::write(&path, value.to_string()).unwrap();
        assert!(load(&path).is_ok());
        value["managed_certificates"]["https"]["port"] = json!(9003);
        fs::write(&path, value.to_string()).unwrap();
        assert!(load(&path).is_err());
    }

    #[test]
    fn omitted_managed_certificate_config_keeps_existing_service_disabled() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("config.json");
        fs::write(
            &path,
            json!({"schema_version":1, "port":9001, "manifest":"catalog.json"}).to_string(),
        )
        .unwrap();
        assert!(load(&path).unwrap().managed_certificates.is_none());
        assert!(!temp.path().join("managed-ca").exists());
    }

    #[test]
    fn empty_managed_certificate_config_uses_default_snapshot_file() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("config.json");
        fs::write(
            &path,
            json!({"schema_version":1, "port":9001, "manifest":"catalog.json",
                "managed_certificates":{}})
            .to_string(),
        )
        .unwrap();
        let settings = load(&path).unwrap().managed_certificates.unwrap();
        assert_eq!(settings.snapshot_file, temp.path().join("managed-ca/current.json"));
        assert!(settings.bearer_token_env.is_none());
        assert!(!temp.path().join("managed-ca").exists());
    }
}
