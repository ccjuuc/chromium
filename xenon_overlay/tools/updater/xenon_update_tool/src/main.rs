// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
mod admin;
mod catalog;
mod config;
mod crashes;
mod delta;
mod history;
mod homepage;
mod managed_certificates;
mod publish;
mod ranges;
mod server;
mod windows;

use anyhow::{Context, Result};
use clap::{parser::ValueSource, ArgMatches, CommandFactory, FromArgMatches, Parser, Subcommand};
use std::path::{Path, PathBuf};

#[derive(Parser)]
#[command(
    version,
    about = "Update and managed CA service with native release and certificate publishing"
)]
struct Cli {
    /// Service configuration. Serve and certificate publishing initialize a missing default file.
    /// Defaults beside the executable; explicit relative paths use the working directory.
    /// Relative paths inside it resolve beside this file.
    #[arg(long, global = true, default_value = "xenon-update/xenon-update.json")]
    config: PathBuf,
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Discover Puffin and generate configuration, an empty catalog and GN settings.
    Init {
        #[arg(long)]
        public_origin: Option<String>,
        #[arg(long, default_value_t = 9001)]
        port: u16,
        #[arg(long)]
        build_dir: Option<PathBuf>,
        #[arg(long)]
        puffin: Option<PathBuf>,
        /// Publisher SPKI SHA256, hex or base64. No private key is needed.
        #[arg(long)]
        key_hash: Option<String>,
        /// Read publisher key hash from a trusted packaging artifacts.json.
        #[arg(long, conflicts_with = "key_hash")]
        artifacts: Option<PathBuf>,
    },
    /// Serve updates and certificates, initializing the default configuration if missing.
    Serve,
    /// Import a packaged release and automatically generate a delta from its predecessor.
    Publish {
        #[arg(long)]
        release: PathBuf,
        /// Publish only the full CRX, without generating a new differential.
        #[arg(long)]
        full_only: bool,
        #[arg(long)]
        puffin: Option<PathBuf>,
    },
    /// Publish a versioned CA snapshot for the Axum certificate endpoint.
    #[command(group(clap::ArgGroup::new("selection").required(true).args(["all", "include", "empty"])))]
    PublishCertificates {
        #[arg(long)]
        source: String,
        /// Positive decimal uint64; must exceed every previously published/accepted version.
        #[arg(long)]
        generation: String,
        /// Explicitly select all six bundled roots.
        #[arg(long)]
        all: bool,
        /// Select a bundled PEM filename; repeat for multiple roots.
        #[arg(long)]
        include: Vec<String>,
        /// Publish an empty snapshot to withdraw this source's certificates.
        #[arg(long)]
        empty: bool,
        #[arg(long, default_value = "der_base64", value_parser = ["der_base64", "pem"])]
        encoding: String,
        /// Offline output override. Otherwise use managed_certificates.snapshot_file from config.
        #[arg(long)]
        out: Option<PathBuf>,
    },
    /// Generate a Puffin patch and verify it by restoring the complete target CRX.
    Delta {
        #[arg(long)]
        old_crx: PathBuf,
        #[arg(long)]
        new_crx: PathBuf,
        #[arg(long)]
        from_version: String,
        #[arg(long)]
        to_version: String,
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        metadata: Option<PathBuf>,
        #[arg(long)]
        puffin: Option<PathBuf>,
        /// Disable detection of nested gzip Payload streams in macOS PKGs.
        #[arg(long)]
        opaque: bool,
    },
    /// Check configuration, discovered tools and all published artifact hashes.
    Doctor,
}

fn print_json(value: &serde_json::Value) -> Result<()> {
    println!("{}", serde_json::to_string_pretty(value)?);
    Ok(())
}

fn resolve_config_path(matches: &ArgMatches, path: PathBuf, executable: &Path) -> Result<PathBuf> {
    if matches.value_source("config") == Some(ValueSource::DefaultValue) {
        return Ok(executable.parent().context("Executable directory is unavailable")?.join(path));
    }
    Ok(path)
}

fn load_service_config(matches: &ArgMatches, path: &Path) -> Result<config::Config> {
    // An explicitly supplied path must exist: a typo must not create a second
    // service state directory or silently replace the intended configuration.
    if matches.value_source("config") == Some(ValueSource::DefaultValue) && !path.try_exists()? {
        config::initialize(path, config::InitOptions::default())?;
        tracing::info!(config = %path.display(), "Created default service configuration");
    }
    config::load(path)
}

fn certificate_output_path(
    matches: &ArgMatches,
    config_path: &Path,
    output: Option<PathBuf>,
) -> Result<PathBuf> {
    if let Some(output) = output {
        return Ok(output);
    }
    Ok(load_service_config(matches, config_path)?
        .managed_certificates
        .context("Certificate endpoint is disabled: configure managed_certificates or use --out for an offline snapshot")?
        .snapshot_file)
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .with_writer(std::io::stderr)
        .init();
    let matches = Cli::command().get_matches();
    let mut cli = Cli::from_arg_matches(&matches)?;
    cli.config = resolve_config_path(&matches, cli.config, &std::env::current_exe()?)?;
    match cli.command {
        Commands::Init { public_origin, port, build_dir, puffin, key_hash, artifacts } => {
            print_json(&config::initialize(
                &cli.config,
                config::InitOptions { public_origin, port, build_dir, puffin, key_hash, artifacts },
            )?)?;
        }
        Commands::Serve => {
            let cfg = load_service_config(&matches, &cli.config)?;
            let admin = admin::Admin::new(&cli.config, cfg.clone())?;
            server::run(
                cfg.manifest,
                cfg.port,
                cfg.public_origin,
                Some(admin),
                cfg.admin_port,
                cfg.crashes,
                cfg.managed_certificates,
            )
            .await?;
        }
        Commands::Publish { release, full_only, puffin } => {
            let cfg = config::load(&cli.config)?;
            let tool = if full_only {
                None
            } else if puffin.is_some() {
                config::find_puffin(puffin.as_deref(), None, None)?
            } else {
                match config::find_puffin(None, cfg.puffin.as_deref(), None) {
                    Ok(tool) => tool,
                    Err(error) => {
                        tracing::warn!(%error, "Puffin unavailable; publishing full release");
                        None
                    }
                }
            };
            let result = tokio::task::spawn_blocking(move || {
                publish::publish(&publish::PublishOptions {
                    manifest: cfg.manifest,
                    release,
                    puffin: tool,
                    full_only,
                })
            })
            .await??;
            print_json(&result)?;
        }
        Commands::PublishCertificates {
            source,
            generation,
            all,
            include,
            empty,
            encoding,
            out,
        } => {
            let out = certificate_output_path(&matches, &cli.config, out)?;
            let result = tokio::task::spawn_blocking(move || {
                managed_certificates::publisher::publish(
                    &managed_certificates::publisher::PublishOptions {
                        source,
                        generation,
                        all,
                        include,
                        empty,
                        encoding,
                        out,
                    },
                )
            })
            .await??;
            print_json(&result)?;
        }
        Commands::Delta {
            old_crx,
            new_crx,
            from_version,
            to_version,
            out,
            metadata,
            puffin,
            opaque,
        } => {
            let cfg = if cli.config.exists() { Some(config::load(&cli.config)?) } else { None };
            let tool = config::find_puffin(puffin.as_deref(), cfg.as_ref().and_then(|c| c.puffin.as_deref()), None)?
                .context("Puffin not found: use --puffin /path/to/puffin, place it beside xenon-update, or build //third_party/puffin:puffin")?;
            let result = tokio::task::spawn_blocking(move || {
                delta::generate(&delta::DeltaOptions {
                    old_crx,
                    new_crx,
                    from_version,
                    to_version,
                    puffin: tool,
                    out,
                    metadata,
                    opaque,
                })
            })
            .await??;
            print_json(&result)?;
        }
        Commands::Doctor => print_json(&config::doctor(&cli.config)?)?,
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn default_config_is_beside_executable_and_explicit_paths_are_preserved() {
        let dir = tempfile::tempdir().unwrap();
        let executable = dir.path().join("bin/xenon-update.exe");
        for command in ["serve", "doctor", "init"] {
            let matches = Cli::command().try_get_matches_from(["xenon-update", command]).unwrap();
            let cli = Cli::from_arg_matches(&matches).unwrap();
            assert_eq!(
                resolve_config_path(&matches, cli.config, &executable).unwrap(),
                dir.path().join("bin/xenon-update/xenon-update.json")
            );
        }
        for path in [PathBuf::from("custom/config.json"), dir.path().join("config.json")] {
            let matches = Cli::command()
                .try_get_matches_from(["xenon-update", "--config", path.to_str().unwrap(), "serve"])
                .unwrap();
            let cli = Cli::from_arg_matches(&matches).unwrap();
            assert_eq!(resolve_config_path(&matches, cli.config, &executable).unwrap(), path);
        }
    }

    #[test]
    fn serve_initializes_default_configuration_once() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("xenon-update/xenon-update.json");
        let matches = Cli::command().try_get_matches_from(["xenon-update", "serve"]).unwrap();
        let config = load_service_config(&matches, &path).unwrap();
        assert_eq!(config.port, 9001);
        assert!(config.manifest.is_file());
        let certificates = config.managed_certificates.unwrap();
        assert!(certificates.snapshot_file.parent().unwrap().is_dir());
        assert!(!certificates.snapshot_file.exists());
        assert!(certificates.bearer_token_env.is_none());

        let original = fs::read(&path).unwrap();
        load_service_config(&matches, &path).unwrap();
        assert_eq!(fs::read(&path).unwrap(), original);
    }

    #[test]
    fn serve_preserves_existing_default_configuration() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("xenon-update.json");
        let original = br#"{"schema_version":1,"port":9017,"manifest":"releases.json"}"#;
        fs::write(&path, original).unwrap();
        let matches = Cli::command().try_get_matches_from(["xenon-update", "serve"]).unwrap();
        let config = load_service_config(&matches, &path).unwrap();
        assert_eq!(config.port, 9017);
        assert!(config.managed_certificates.is_none());
        assert_eq!(fs::read(&path).unwrap(), original);
        assert!(!dir.path().join("managed-ca").exists());
    }

    #[test]
    fn serve_does_not_replace_invalid_default_configuration() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("xenon-update.json");
        fs::write(&path, "invalid json").unwrap();
        let matches = Cli::command().try_get_matches_from(["xenon-update", "serve"]).unwrap();
        assert!(load_service_config(&matches, &path).is_err());
        assert_eq!(fs::read_to_string(&path).unwrap(), "invalid json");
        assert!(!dir.path().join("releases").exists());
    }

    #[test]
    fn serve_does_not_initialize_missing_explicit_configuration() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("explicit/xenon-update.json");
        let file = path.to_str().unwrap();
        for arguments in [
            vec!["xenon-update", "--config", file, "serve"],
            vec!["xenon-update", "serve", "--config", file],
        ] {
            let matches = Cli::command().try_get_matches_from(arguments).unwrap();
            assert!(load_service_config(&matches, &path).is_err());
            assert!(!path.parent().unwrap().exists());
        }
    }

    #[test]
    fn certificate_cli_requires_one_selection_and_rejects_removed_lifetime_option() {
        let base =
            ["xenon-update", "publish-certificates", "--source", "company-ca", "--generation", "1"];
        for extra in [
            vec![],
            vec!["--all", "--empty"],
            vec!["--all", "--include", "bjca-global-root-ca1.pem"],
            vec!["--empty", "--include", "bjca-global-root-ca1.pem"],
            vec!["--all", "--valid-days", "7"],
            vec!["--all", "--encoding", "der"],
        ] {
            assert!(Cli::try_parse_from(base.into_iter().chain(extra)).is_err());
        }
        let cli = Cli::try_parse_from(base.into_iter().chain([
            "--include",
            "bjca-global-root-ca1.pem",
            "--include",
            "bjca-global-root-ca2.pem",
        ]))
        .unwrap();
        let Commands::PublishCertificates { include, encoding, out, .. } = cli.command else {
            panic!("Expected certificate publisher");
        };
        assert_eq!(include, ["bjca-global-root-ca1.pem", "bjca-global-root-ca2.pem"]);
        assert_eq!(encoding, "der_base64");
        assert!(out.is_none());
    }

    #[test]
    fn certificate_output_uses_config_relative_path_without_rewriting_config() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.json");
        let original = br#"{"schema_version":1,"port":9017,"manifest":"releases.json","managed_certificates":{"snapshot_file":"certificates/active.json"}}"#;
        fs::write(&path, original).unwrap();
        let matches = Cli::command()
            .try_get_matches_from([
                "xenon-update",
                "--config",
                path.to_str().unwrap(),
                "publish-certificates",
                "--source",
                "company-ca",
                "--generation",
                "1",
                "--all",
            ])
            .unwrap();
        assert_eq!(
            certificate_output_path(&matches, &path, None).unwrap(),
            dir.path().join("certificates/active.json")
        );
        assert_eq!(fs::read(&path).unwrap(), original);
    }

    #[test]
    fn certificate_output_initializes_default_config_but_keeps_explicit_paths_strict() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("default/config.json");
        let matches = Cli::command()
            .try_get_matches_from([
                "xenon-update",
                "publish-certificates",
                "--source",
                "company-ca",
                "--generation",
                "1",
                "--all",
            ])
            .unwrap();
        let out = certificate_output_path(&matches, &path, None).unwrap();
        assert_eq!(out, path.parent().unwrap().join("managed-ca/current.json"));
        assert!(path.is_file());
        assert!(!out.exists());

        let explicit = dir.path().join("explicit/config.json");
        let matches = Cli::command()
            .try_get_matches_from([
                "xenon-update",
                "publish-certificates",
                "--config",
                explicit.to_str().unwrap(),
                "--source",
                "company-ca",
                "--generation",
                "1",
                "--empty",
            ])
            .unwrap();
        assert!(certificate_output_path(&matches, &explicit, None).is_err());
        assert!(!explicit.parent().unwrap().exists());
    }

    #[test]
    fn certificate_offline_output_does_not_require_or_create_config() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("unused/config.json");
        let out = dir.path().join("offline.json");
        let matches = Cli::command()
            .try_get_matches_from([
                "xenon-update",
                "publish-certificates",
                "--config",
                path.to_str().unwrap(),
                "--source",
                "company-ca",
                "--generation",
                "1",
                "--all",
                "--out",
                out.to_str().unwrap(),
            ])
            .unwrap();
        assert_eq!(certificate_output_path(&matches, &path, Some(out.clone())).unwrap(), out);
        assert!(!path.parent().unwrap().exists());
    }

    #[test]
    fn certificate_output_rejects_disabled_service_without_enabling_it() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.json");
        let original = br#"{"schema_version":1,"port":9017,"manifest":"releases.json"}"#;
        fs::write(&path, original).unwrap();
        let matches = Cli::command()
            .try_get_matches_from([
                "xenon-update",
                "publish-certificates",
                "--source",
                "company-ca",
                "--generation",
                "1",
                "--all",
            ])
            .unwrap();
        let error = certificate_output_path(&matches, &path, None).unwrap_err();
        assert!(error.to_string().contains("disabled"));
        assert_eq!(fs::read(&path).unwrap(), original);
    }
}
