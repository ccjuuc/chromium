// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Build-time source composition only. The injected script has no include
// loader, extra closures, or additional runtime filesystem/IPC operations.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const includePattern = /^\s*\/\/ @include "([^"\r\n]+)"\s*$/;

function buildBootstrap(entryPath) {
  const entry = path.resolve(entryPath);
  const root = path.dirname(entry);
  const inputs = new Set();
  const active = new Set();

  function expand(filename) {
    if (active.has(filename)) {
      throw new Error(`Cyclic bootstrap include: ${filename}`);
    }
    if (inputs.has(filename)) {
      throw new Error(`Duplicate bootstrap include: ${filename}`);
    }
    active.add(filename);
    inputs.add(filename);
    const text = fs.readFileSync(filename, 'utf8').replace(/\r\n/g, '\n');
    const lines = text.split(/(?<=\n)/);
    const source = lines.map(line => {
      const directive = includePattern.exec(line);
      if (!directive) {
        if (/^\s*\/\/\s*@include\b/.test(line)) {
          throw new Error(`Malformed bootstrap include in ${filename}: ${line.trim()}`);
        }
        return line;
      }
      if (filename !== entry) {
        throw new Error(`Bootstrap includes belong in the entry only: ${filename}`);
      }
      const relative = directive[1];
      // Use the same root-relative POSIX paths on all build hosts. Reject
      // traversal instead of accidentally importing another source tree.
      if (!/^[a-zA-Z0-9_./-]+\.js$/.test(relative) ||
          relative.startsWith('/') || relative.split('/').some(part =>
            !part || part === '.' || part === '..')) {
        throw new Error(`Invalid bootstrap include path: ${relative}`);
      }
      const included = expand(path.resolve(root, relative));
      if (!included.endsWith('\n')) {
        throw new Error(`Bootstrap fragment must end with a newline: ${relative}`);
      }
      return included;
    }).join('');
    active.delete(filename);
    return source;
  }

  const source = expand(entry);
  // Parse without executing. A misplaced boundary or duplicate declaration
  // must fail the build before GRIT packages the resource.
  new vm.Script(source, {filename: entry});
  return {source, inputs: [...inputs]};
}

function writeIfChanged(filename, contents) {
  if (fs.existsSync(filename) && fs.readFileSync(filename, 'utf8') === contents) {
    return;
  }
  fs.mkdirSync(path.dirname(filename), {recursive: true});
  fs.writeFileSync(filename, contents, 'utf8');
}

function depfilePath(filename) {
  return path.relative(process.cwd(), filename).replace(/\\/g, '/')
      .replace(/\$/g, () => '$$').replace(/([ #:])/g, '\\$1');
}

function main(args) {
  const entries = [];
  let outputDirectory;
  let depfile;
  for (let index = 0; index < args.length; index += 2) {
    const value = args[index + 1];
    if (!value) throw new Error(`Missing value for ${args[index]}`);
    switch (args[index]) {
      case '--entry': entries.push(path.resolve(value)); break;
      case '--output-dir': outputDirectory = path.resolve(value); break;
      case '--depfile': depfile = path.resolve(value); break;
      default: throw new Error(`Unknown argument: ${args[index]}`);
    }
  }
  if (!entries.length || !outputDirectory) {
    throw new Error('Usage: bundle_ipc_bootstrap.cjs --entry FILE [--entry FILE] ' +
        '--output-dir DIR [--depfile FILE]');
  }
  const outputs = new Set();
  const inputs = new Set([__filename]);
  // Validate every bundle before writing any output.
  const bundles = entries.map(entry => {
    const output = path.join(outputDirectory, path.basename(entry));
    if (outputs.has(output)) throw new Error(`Duplicate bundle output: ${output}`);
    if (output === entry) throw new Error('Bundle output must not overwrite its source');
    outputs.add(output);
    const result = buildBootstrap(entry);
    result.inputs.forEach(input => inputs.add(input));
    return {output, source: result.source};
  });
  for (const bundle of bundles) writeIfChanged(bundle.output, bundle.source);
  if (depfile) {
    writeIfChanged(depfile, `${[...outputs].map(depfilePath).join(' ')}: ` +
        `${[...inputs].map(depfilePath).join(' ')}\n`);
  }
}

module.exports = {buildBootstrap};
if (require.main === module) main(process.argv.slice(2));
