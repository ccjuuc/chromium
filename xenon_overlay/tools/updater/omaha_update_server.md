# Xenon Omaha update endpoint

`omaha_update_server.js` is a dependency-free Node server for the Xenon
**Omaha 4.0** macOS clients in this checkout. One catalog can publish browser
and Updater releases for multiple macOS architectures. macOS uses the system
Chromium Updater. Windows clients retain the separate JSON + ZIP update flow
and do not use this Omaha endpoint. The server does not create archives, execute
installers, install services, or change a running server. The legacy JSON
`mock_update_server.js` remains a separate compatibility/testing endpoint.

## Trust contract

The Xenon Omaha clients use **HTTPS plus a mandatory 32-byte
SHA-256 pin of the release key's DER SubjectPublicKeyInfo, with standard CRX3
signature verification**. Xenon does not use Google's CUP keys. Both the
browser payload and Updater self-update payload must satisfy the configured
release-key pin. HTTP is restricted to explicit `allow_insecure_localhost`
development configuration; serving on localhost does not itself opt a client
into HTTP.

Unmodified Chromium defaults are different: `UseCUP()` is true and the CRX
verifier requires a publisher proof. `request_sender.cc` embeds Google's CUP
verification keys (ECDSA/ML-DSA, version 16). Merely pointing that client at this
JSON endpoint will fail authentication. This server returns HTTP 501 for
`cup2key` or `cup2hreq`; it never emits a fake CUP proof. Do not disable the
Xenon release-key pin or substitute an unsigned ZIP/PKG to make a test pass.

The server verifies file size, SHA-256 and container type before listening.
It does **not** replace the client's cryptographic CRX verification, inspect the
embedded app's version, or prove that an installer is correct. Preparing a
release must separately verify the embedded app ID/version, signature,
installer behavior and, when supplied, that the patch reconstructs the exact
full CRX. The manifest is trusted operator input and must describe those
already-prepared artifacts.

## Release manifest

All referenced files must already exist as regular, single-link files in the
manifest's directory. Only basenames are accepted; symlinks, path traversal,
arbitrary URLs, executable paths and installer arguments are rejected. File
names must be unique across the entire manifest. Hashes are 64 lowercase hex
characters and sizes are positive safe integers in bytes.

The single-app format is:

```json
{
  "schema_version": 1,
  "appid": "org.chromium.Chromium",
  "version": "1.0.0.2",
  "platform": "mac",
  "arch": "arm64",
  "full": {
    "file": "xenon-1.0.0.2.crx3",
    "size": 123456,
    "sha256": "REPLACE_WITH_EXACT_LOWERCASE_SHA256"
  },
  "differential": [
    {
      "from_version": "1.0.0.1",
      "previous_sha256": "REPLACE_WITH_OLD_COMPLETE_CRX_SHA256",
      "type": "puff",
      "file": "xenon-1-to-2.puff",
      "size": 1234,
      "sha256": "REPLACE_WITH_EXACT_PATCH_SHA256"
    }
  ]
}
```

The sizes and hashes above are placeholders, not runnable artifacts. Omit
`differential` for a full-only release. `minimum_version` may optionally limit
eligible installed versions; it defaults to `0`. `platform` must be `mac`;
`arch` accepts `arm64` or `x86_64` (`x64` requests normalize to `x86_64`). Versions use
one to four unsigned 32-bit decimal components; omitted trailing zeroes compare
equally. Unknown manifest fields are errors.

A differential is offered only when its size is strictly smaller than the full
CRX. Equal or larger patches may remain in the catalog, but are omitted from
responses even when the client has the matching cached baseline and patch format.

To serve several macOS architecture variants and optional Updater
self-update checks from one endpoint, put the release objects (without their
individual `schema_version`) in `apps`. This example shows the routing fields;
each object must also contain the required `version` and `full` fields shown
above:

```json
{
  "schema_version": 1,
  "apps": [
    { "appid": "org.chromium.Chromium", "platform": "mac", "arch": "arm64" },
    { "appid": "org.chromium.Chromium", "platform": "mac", "arch": "x86_64" },
    { "appid": "com.xenon.XenonUpdater", "platform": "mac", "arch": "arm64" }
  ]
}
```

The tuple `(appid, platform, arch)` must be unique; app IDs compare
case-insensitively. Each tuple has one target version, full CRX and optional
patches. Artifact basenames must remain unique across the entire catalog, so
include platform and architecture in filenames when combining release outputs.
The server matches app ID, OS, then client binary architecture; it never falls
back to another platform or architecture. An unlisted app receives
`error-unknownApplication`; a missing platform or architecture variant receives
`error-osnotsupported` or `error-hwnotsupported`.

Request `arch` identifies the running client binary and takes precedence over
`os.arch`, which can describe different hardware under emulation. For example,
an `arch: "x64"` macOS client on an `os.arch: "arm64"` host receives the
`mac/x86_64` release. When both `@os` and `os.platform` are present, they must
identify macOS. Recognized platform aliases are `mac` and `Mac OS X`; the
manifest platform name is always `mac`. Windows manifests are rejected.

The importable `loadCatalog(absoluteManifestPath)` validates a prepared manifest
without starting a listener; close the returned catalog after validation:

```js
import { loadCatalog } from './omaha_update_server.js';
const catalog = await loadCatalog('/absolute/release/release.json');
console.log(catalog.releases.map(({ appid, version, platform, arch }) =>
  ({ appid, version, platform, arch })));
await catalog.close();
```

The catalog retains read-only descriptors to the verified files and checks
their identity/size/timestamps before each transfer. Replacing a pathname cannot
redirect a download. Changing an artifact invalidates the running catalog:
prepare a new manifest and restart the service. An in-flight mutation remains
subject to the client's expected-size/hash and CRX signature checks.

## Wire protocol

`POST /service/update2/json`, `Content-Type: application/json`, at most 1 MiB:

```json
{
  "request": {
    "protocol": "4.0",
    "acceptformat": "crx3,download,puff,run,xz,zucc",
    "@os": "mac",
    "arch": "arm64",
    "os": { "platform": "Mac OS X", "arch": "arm64" },
    "apps": [{
      "appid": "org.chromium.Chromium",
      "version": "1.0.0.1",
      "cached_items": [{ "sha256": "EXACT_OLD_COMPLETE_CRX_SHA256" }],
      "updatecheck": {}
    }]
  }
}
```

Successful protocol responses are prefixed with the five-byte anti-XSSI marker
`)]}'\n`, then a `response` object containing `protocol: "4.0"`, `apps`, and
`daystart.elapsed_days`. App pings/events without `updatecheck` receive an app
acknowledgment without any installation instructions; an empty `apps` list is
also acknowledged. This is not a telemetry persistence service.

An eligible full update supplies `status: "ok"`, `nextversion`, and one
pipeline:

1. `download`: exact `size`, `out: {sha256}`, `urls: [{url}]`.
2. `crx3`: `in: {sha256}` and exact full-CRX `size`.

The macOS CRX must contain the Updater installer executables (`.install` and
optionally the pre/post-install equivalents) at its root. Its `crx3` operation
also contains the fixed `path: "."` and `arguments: ""` fields.
The server cannot select a different script, inject `run` operations, or add arguments.

A differential is offered only when `from_version` equals the installed
version, `cached_items` includes `previous_sha256`, and the client advertises
the patch operation. The first pipeline is `download` patch → `puff` or `zucc`
with `previous` and `out` hashes → `crx3`. The second pipeline is always the
complete CRX pipeline. Chromium's pipeline executor falls back after an
operation error; the server does not fake a successful patch or require a
second update check. When no eligible cached base exists, only the full
pipeline is returned.

`previous_sha256` identifies an old **complete CRX in the client's update cache**, not
an installed app tree or an executable. Legacy `fp`/`fingerprint` values may
appear as ignored extra request fields; they do not establish the presence of
cached bytes. The current 4.0 serializer sends `cached_items`, not `fp`.
Existing Xenon `patch.zip` files cannot be renamed to Puffin or Zucchini patches.

Equal/newer installed versions, versions below `minimum_version`,
`updatedisabled: true`, or an excluding `targetversionprefix` produce
`noupdate`. A wrong OS or architecture produces the corresponding unsupported
status. Rollback/same-version flags never cause this development server to
offer an older/equal target. Missing `download`/`crx3` capabilities produce
`error-inexpressible`. Protocol 3.1 requests are explicitly rejected.

`GET`/`HEAD /artifacts/<listed-basename>` supports exact bytes and single HTTP
byte ranges. Unlisted files, query-string path tricks and arbitrary paths are
not served. No artifact is generated on a missing-file request.

## Running and validation

```sh
node xenon_overlay/tools/updater/omaha_update_server.js \
  --manifest /absolute/prepared-release/release.json --port 9001

node --test xenon_overlay/tools/updater/omaha_update_server.test.js
```

Only the first command starts a persistent listener; tests use ephemeral
loopback ports and close them. The server always binds `127.0.0.1`, rejects
browser `Origin` requests and requires a loopback `Host`. It logs a compact
JSON record for each completed request without logging full request bodies.

For an HTTPS reverse proxy, add
`--public-origin https://updates.example.com`. This only changes emitted
artifact URLs; the proxy must provide TLS for both the Omaha and artifact
routes, connect to `127.0.0.1:9001`, and rewrite `Host` to
`127.0.0.1:9001`. Forward browser Origin headers so the native-client-only
restriction remains effective. The public origin cannot contain credentials,
a path, query or fragment. TLS/private-key management, deployment access
controls and production telemetry remain deployment responsibilities.

Tests use the existing Chromium CRX/Puffin fixtures to exercise the protocol,
selection and HTTP/file boundaries. They do not claim an end-to-end browser
installation, publisher proof, Gatekeeper approval, or real Xenon Updater
self-update has succeeded.

## Source contract references

- [Omaha 4.0](../../../docs/updater/protocol_4.md), especially requests and pipelines.
- [Omaha 3.1](../../../docs/updater/protocol_3_1.md) describes the incompatible legacy manifest/packages shape.
- [Mac installer API](../../../docs/updater/installer_api_mac.md).
- [Protocol version](../../../components/update_client/protocol_definition.h) and [serializer](../../../components/update_client/protocol_serializer_json.cc).
- [Response parser](../../../components/update_client/protocol_parser_json.cc) and [pipeline fallback](../../../components/update_client/pipeline.cc).
- [Upstream trust defaults](../../../chrome/updater/external_constants_default.cc), [CUP request signing](../../../components/update_client/request_sender.cc), and [CRX verification](../../../components/crx_file/crx_verifier.h).
