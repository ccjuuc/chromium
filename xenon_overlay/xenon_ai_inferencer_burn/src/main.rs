// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg_attr(windows, windows_subsystem = "windows")]

// Burn + llama-burn: Llama 3.2 1B Instruct (pretrained weights).
// Backend (Cargo features, see Cargo.toml):
//   backend_vulkan — Vulkan<f16> + wgpu (default, faster if GPU works)
//   backend_ndarray — CPU ndarray<f32> (smaller binary, much slower)
// stdout protocol: DELTA / DONE (consumed by Chrome XenonAiInferenceClient).
//
// Modes:
//   Default: one-shot (--input path, --task, --instruction) then exit.
//   --stdio-server: load model once, then read one JSON request per line on
//     stdin; each response is DELTA lines + DONE <code> + flushed newline.
//     After loading, prints READY\\n on stdout (Chrome waits for this).

#[cfg(all(feature = "backend_vulkan", feature = "backend_ndarray"))]
compile_error!("enable only one of: backend_vulkan, backend_ndarray");

#[cfg(all(
    not(feature = "backend_vulkan"),
    not(feature = "backend_ndarray")
))]
compile_error!("enable one of: backend_vulkan (default), backend_ndarray");

use llama_burn::llama::Llama;
use llama_burn::tokenizer::Tiktoken;
use llama_burn::{llama::LlamaConfig, sampling::Sampler};
use serde::Deserialize;
use std::fs;
use std::io::{self, BufRead, Write};
use std::path::PathBuf;

#[cfg(feature = "backend_vulkan")]
use burn::backend::wgpu::{Vulkan, WgpuDevice};
#[cfg(feature = "backend_vulkan")]
use burn::tensor::f16;

#[cfg(feature = "backend_ndarray")]
use burn::backend::ndarray::{NdArray, NdArrayDevice};

#[cfg(feature = "backend_vulkan")]
type BurnBackend = Vulkan<f16, i32>;
#[cfg(feature = "backend_ndarray")]
type BurnBackend = NdArray;

fn wants_stdio_server(args: &[String]) -> bool {
    args.iter().any(|a| a == "--stdio-server")
}

fn parse_args() -> (PathBuf, String, String) {
    let args: Vec<String> = std::env::args().collect();
    let mut input = PathBuf::new();
    let mut task = String::from("rewrite");
    let mut instruction = String::new();

    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        if a == "--stdio-server" {
            i += 1;
        } else if let Some(rest) = a.strip_prefix("--input=") {
            input = PathBuf::from(rest);
            i += 1;
        } else if a == "--input" && i + 1 < args.len() {
            input = PathBuf::from(&args[i + 1]);
            i += 2;
        } else if let Some(rest) = a.strip_prefix("--task=") {
            task = rest.to_string();
            i += 1;
        } else if a == "--task" && i + 1 < args.len() {
            task = args[i + 1].clone();
            i += 2;
        } else if let Some(rest) = a.strip_prefix("--instruction=") {
            instruction = rest.to_string();
            i += 1;
        } else if a == "--instruction" && i + 1 < args.len() {
            instruction = args[i + 1].clone();
            i += 2;
        } else {
            i += 1;
        }
    }
    (input, task, instruction)
}

fn emit_delta(text: &str) {
    let flattened: String = text.chars().map(|c| if c == '\n' { ' ' } else { c }).collect();
    println!("DELTA {flattened}");
}

/// Llama 3 instruct-style wrapper (matches tracel-ai/models llama-burn chat example).
fn wrap_llama3_user_prompt(user_content: &str) -> String {
    format!(
        "<|redacted_start_header_id|>system<|redacted_end_header_id|>\n\n\
         You are a writing assistant. When the user gives text to rewrite, rephrase, or change tone, \
         respond with only the revised text, using the same language as that text. \
         Do not translate into English unless the source text is English or the user explicitly asks for another language.\
         <|eot_id|><|redacted_start_header_id|>user<|redacted_end_header_id|>\n\n\
         {user_content}\
         <|eot_id|><|redacted_start_header_id|>assistant<|redacted_end_header_id|>\n\n"
    )
}

fn generate_rewrite(
    llama: &mut Llama<BurnBackend, Tiktoken>,
    body: &str,
    instruction: &str,
) -> Result<String, String> {
    let user_line = if instruction.is_empty() {
        format!("Rewrite the following text. Keep the same language as the input.\n\n{body}")
    } else {
        format!(
            "{instruction}\n\nText (preserve this language in your output):\n{body}"
        )
    };
    let prompt = wrap_llama3_user_prompt(&user_line);

    let mut sampler = Sampler::new_top_p(0.9, 42);
    let generated = llama.generate(&prompt, 256, 0.6, &mut sampler);
    Ok(generated.text)
}

fn load_llama_once() -> Result<Llama<BurnBackend, Tiktoken>, String> {
    const MAX_SEQ_LEN: usize = 2048;

    #[cfg(feature = "backend_vulkan")]
    let device = WgpuDevice::default();
    #[cfg(feature = "backend_ndarray")]
    let device = NdArrayDevice::default();

    LlamaConfig::llama3_2_1b_pretrained::<BurnBackend>(MAX_SEQ_LEN, &device)
        .map_err(|e| format!("Llama 3.2 1B load failed: {e:?}"))
}

fn run_stdio_server() -> Result<(), String> {
    let mut llama = load_llama_once()?;

    println!("READY");
    io::stdout().flush().map_err(|e| e.to_string())?;

    let stdin = io::stdin();
    let mut reader = std::io::BufReader::new(stdin.lock());
    let mut line = String::new();

    #[derive(Deserialize)]
    struct Request {
        input: String,
        #[serde(default)]
        instruction: String,
        #[serde(default)]
        task: String,
    }

    loop {
        line.clear();
        let n = reader.read_line(&mut line).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        let req: Request = match serde_json::from_str(trimmed) {
            Ok(r) => r,
            Err(e) => {
                let _ = writeln!(io::stderr(), "bad request JSON: {e}");
                println!("DONE 4");
                io::stdout().flush().map_err(|e2| e2.to_string())?;
                continue;
            }
        };
        let task = if req.task.is_empty() {
            "rewrite".to_string()
        } else {
            req.task
        };
        let body = fs::read_to_string(&req.input).unwrap_or_default();
        if body.is_empty() {
            let _ = io::stderr().write_all(b"empty input\n");
            println!("DONE 1");
            io::stdout().flush().map_err(|e| e.to_string())?;
            continue;
        }
        if task != "rewrite" {
            let _ = writeln!(
                io::stderr(),
                "only task=rewrite implemented in burn build (got {task})"
            );
            println!("DONE 2");
            io::stdout().flush().map_err(|e| e.to_string())?;
            continue;
        }
        match generate_rewrite(&mut llama, &body, &req.instruction) {
            Ok(text) => {
                emit_delta(&text);
                println!("DONE 0");
            }
            Err(e) => {
                let _ = writeln!(io::stderr(), "{e}");
                println!("DONE 3");
            }
        }
        io::stdout().flush().map_err(|e| e.to_string())?;
    }
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if wants_stdio_server(&args) {
        if let Err(e) = run_stdio_server() {
            let _ = writeln!(io::stderr(), "{e}");
            std::process::exit(1);
        }
        return;
    }

    let (input_path, task, instruction) = parse_args();
    if input_path.as_os_str().is_empty() {
        let _ = io::stderr().write_all(b"missing --input\n");
        println!("DONE 1");
        return;
    }

    let body = fs::read_to_string(&input_path).unwrap_or_default();
    if body.is_empty() {
        let _ = io::stderr().write_all(b"empty input\n");
        println!("DONE 1");
        return;
    }

    if task != "rewrite" {
        let _ = writeln!(
            io::stderr(),
            "only task=rewrite implemented in burn build (got {task})"
        );
        println!("DONE 2");
        return;
    }

    let mut llama = match load_llama_once() {
        Ok(x) => x,
        Err(e) => {
            let _ = writeln!(io::stderr(), "{e}");
            println!("DONE 3");
            return;
        }
    };

    match generate_rewrite(&mut llama, &body, &instruction) {
        Ok(text) => {
            emit_delta(&text);
            println!("DONE 0");
        }
        Err(e) => {
            let _ = writeln!(io::stderr(), "{e}");
            println!("DONE 3");
        }
    }
}
