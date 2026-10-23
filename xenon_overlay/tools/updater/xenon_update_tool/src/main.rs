// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
mod admin;
mod catalog;
mod config;
mod delta;
mod history;
mod homepage;
mod publish;
mod ranges;
mod server;
mod windows;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use std::path::PathBuf;

#[derive(Parser)]
#[command(
    version,
    about = "Windows JSON + ZIP and macOS Omaha update service with automatic differential publishing"
)]
struct Cli {
    /// Service configuration. Relative paths inside it resolve beside this file.
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
    /// Serve the catalog; verified catalog changes reload automatically.
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

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .with_writer(std::io::stderr)
        .init();
    let cli = Cli::parse();
    match cli.command {
        Commands::Init { public_origin, port, build_dir, puffin, key_hash, artifacts } => {
            print_json(&config::initialize(
                &cli.config,
                config::InitOptions { public_origin, port, build_dir, puffin, key_hash, artifacts },
            )?)?;
        }
        Commands::Serve => {
            let cfg = config::load(&cli.config)?;
            let admin = admin::Admin::new(&cli.config, cfg.clone())?;
            server::run(cfg.manifest, cfg.port, cfg.public_origin, Some(admin), cfg.admin_port)
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
