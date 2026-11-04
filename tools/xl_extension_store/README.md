# XL local extension store

This tool serves `https://xl.extensions.test.com` from loopback, reads metadata
directly from verified CRX3 packages, and supports unchanged Google Web Store
packages and packages countersigned by the pinned XL publisher key.
It uses Node.js 24 built-ins; no npm dependencies are required. The browser still
shows its normal permissions confirmation and performs its own CRX verification.

## Local runtime

From the Chromium source root in PowerShell:

```powershell
node tools/xl_extension_store/setup-runtime.mjs
node tools/xl_extension_store/store.mjs serve
```

`setup-runtime.mjs` restores the checked-in test environment using its bundled
test password, validates the original CA and publisher identities, creates or
renews the HTTPS certificate when needed, and prepares the XL demo and unchanged
Google sample. It also regenerates optional managed-CA configuration with this
checkout's absolute paths. The existing browser build can continue to trust the
same CA and publisher; no browser rebuild is needed.

Use a fresh runtime directory for the initial restore. Rerunning setup on an
initialized matching runtime retains its keys, valid certificate and existing
package bytes. `--data-dir <directory>` selects another runtime directory;
`--openssl <executable>` selects the certificate-generation executable. Setup
prints the next service command but does not start it, edit hosts or install
system certificates.

The committed `test-bundle-password.txt` deliberately makes the accompanying
`xl-store-full.zip.enc` decryptable by anyone with the checkout. These are shared
test credentials, including the CA and publisher signing keys, and must not be
used as production identities. Generated plaintext keys and runtime files remain
under ignored `out/`.

The server binds only `127.0.0.1:443`. `--port 8443` is available for service
testing, but the browser's native XL origin is exactly HTTPS on port 443; changing
ports also requires changing that browser configuration. Use the same port for
`demo`, `serve`, and package `update_url` when testing another port.
`--data-dir` selects another runtime directory. Restart the server after imports
or updates; the catalog is verified when the server starts.

Add the following hosts mapping through the normal administrator workflow:

```text
127.0.0.1 xl.extensions.test.com
```

`init` validates the local CA against the public root built into the browser,
then issues or reuses an HTTPS leaf with `subjectAltName=DNS:xl.extensions.test.com`.
It does not create a random CA, install system certificates, edit hosts, or disable
certificate verification. Setup restores the matching CA private key at
`out/xl_extension_store/keys/local_root_ca_private.pem` for certificate issuance
and renewal. The lower-level `store.mjs init` command requires that key to have
already been provisioned when it needs to issue a certificate.
An existing valid leaf and TLS private key can be reused without retaining that
CA signing key on the server. The public root is copied from
`tools/xl_extension_store/xl_store_root_ca.pem` when absent.

The earlier development CA is migrated only if it is a valid self-signed CA with
the same subject and public key as the built-in root. This replaces its public
certificate with the current built-in root while preserving signing keys and valid
leaf certificates. An unrelated CA or mismatched private key is rejected.
An existing managed snapshot receives a higher generation when its CA certificate
changes or its extra trust constraints are removed. Rerunning `init` preserves
the resulting snapshot bytes.

Generated public runtime files are:

```text
out/xl_extension_store/tls/local-root-ca.pem
out/xl_extension_store/tls/server.crt
out/xl_extension_store/managed-ca.json
out/xl_extension_store/managed-ca-snapshot.json
```

The generated managed config/snapshot support the optional legacy distribution
test described below; the browser's store connection does not use them.

The local CA, TLS and publisher private keys stay in
`out/xl_extension_store/keys/`. `init` prints certificate paths and fingerprints.
Rerunning it retains existing valid keys, the server certificate and immutable
snapshot bytes. Restart a running store after certificate generation/migration.

The default OpenSSL executable is `third_party/perl/c/bin/openssl.exe`.
Pass `--openssl <executable>` if required. No OpenSSL process is needed to run the
server or sign packages after the certificate exists.

Plaintext runtime data and private key files live under ignored
`out/xl_extension_store/`. The shared test bundle and its password are checked in;
the following public publisher and HTTPS CA material is also checked in directly:

```text
tools/xl_extension_store/xl_publisher_public_key.pem
tools/xl_extension_store/xl_publisher_public_key.der
tools/xl_extension_store/xl_publisher_public_key.sha256
tools/xl_extension_store/xl_store_root_ca.pem
```

The pinned SHA256 of DER SubjectPublicKeyInfo is:

```text
788e070876c32df8c745caecf97a05e843d39d0124c77fa65eecd03c95e59c68
```

The matching development publisher private key was generated locally at
`out/xl_extension_store/keys/xl_publisher_private.pem`; its plaintext is never
committed as a standalone PEM. Setup restores it from the checked-in full-runtime
test bundle using the checked-in test password. Run setup on a new checkout before
signing or running `init`. Generating a different publisher key requires an
intentional browser pin and public-key update. The tool fails if the private key
does not match the pin.
Serving/importing existing signed packages does not require that publisher key.

## Move the test environment to another machine

Keep using the same certificate and signing identities when moving machines:
the browser already trusts their public keys. For the shared test environment,
check out the source, install Node.js 24, run `setup-runtime.mjs`, configure the
hosts mapping above and start `store.mjs serve`. No password transfer, manual ZIP
extraction or replacement signing identity is needed. Use the sidebar **EXT**
page's browser command to launch the matching browser build.

The following manual export workflow is useful when transferring additional
packages or a particular existing runtime instead of restoring the checked-in
test bundle.

For a machine that only needs to serve the existing test packages:

```powershell
node tools/xl_extension_store/export-runtime.mjs
```

The default output is `out/xl_extension_store/exports/xl-store-runtime.zip`.
It contains the HTTPS server certificate and TLS private key, the public root
certificate, and the verified CRX catalog. It can run `serve` and import already
signed packages without the CA or XL publisher signing keys.

To also preserve CRX signing, HTTPS renewal and the demo extension identity:

```powershell
node tools/xl_extension_store/export-runtime.mjs --include-signing-keys
```

This produces `out/xl_extension_store/exports/xl-store-full.zip`, adding the XL
publisher private key, CA private key and existing demo developer private key.
Existing demo CRX files are also preserved so rerunning `demo` reuses the
published bytes instead of trying to replace the same ID/version.
Both plaintext ZIP archives contain private material and stay under ignored
`out/`. Transfer them directly to the intended test machine separately from Git,
or encrypt the full archive for the repository workflow below.
Existing archives are never overwritten; use `--output <new-path.zip>` for a
new export. `--data-dir <directory>` selects another source runtime.

On the new machine, use the same source revision and browser build, install
Node.js 24, and extract the archive into a new runtime directory. For example,
from the new Chromium source root:

```powershell
Expand-Archive -LiteralPath 'D:/transfer/xl-store-full.zip' -DestinationPath './out/xl_extension_store'
node tools/xl_extension_store/store.mjs serve
```

Use an empty destination and omit `-Force` so existing keys are not replaced.
Set the hosts entry `127.0.0.1 xl.extensions.test.com`, then use the sidebar
**EXT** page's browser command. The HTTPS certificate is tied to that domain,
not to this computer or checkout path. No system certificate installation or
initial `init` command is needed to serve a still-valid exported certificate.

The exporter deliberately omits browser profiles, logs, process IDs, old TLS
backups and dynamic-CA configuration with machine-specific absolute paths.
If the optional dynamic CA test is needed, run `store.mjs init` after importing
the full archive to recreate its paths on the destination. The full archive's
CA key also permits certificate renewal with `init`; OpenSSL is needed when a
new certificate is issued (`--openssl <executable>` selects its location).

### Encrypted full-runtime bundle in Git

The shared test runtime travels with the source as
`tools/xl_extension_store/xl-store-full.zip.enc`, together with
`tools/xl_extension_store/test-bundle-password.txt`. Setup uses this pair
automatically. Keeping the password beside the bundle removes confidentiality
for these shared test keys; the authenticated archive format still detects a
damaged or incorrectly decrypted bundle. Plaintext ZIPs and extracted private
key files remain outside Git.

To refresh this shared test bundle, encrypt the previously exported full ZIP
using its checked-in test password:

```powershell
node tools/xl_extension_store/protect-runtime.mjs encrypt --input out/xl_extension_store/exports/xl-store-full.zip --password-file tools/xl_extension_store/test-bundle-password.txt --output tools/xl_extension_store/xl-store-full-next.zip.enc
```

Review the new archive before replacing `xl-store-full.zip.enc`; encryption
never overwrites an existing output. Keep the archive and bundled password
consistent so setup can restore them together.

For a separate private archive, select a distinct output and a new password path
outside Git with `--password-file`. Encryption generates a random password if
that file is absent and reports only paths. Transfer that private password
separately when restoring elsewhere. Without `--password-file`, the default is
`out/xl_extension_store/keys/runtime-bundle-password.txt`; an existing file is
reused and may contain the original shared test password. Setup never creates or
changes that default password file. The checked-in shared password is not
suitable for protecting a private archive.

`protect-runtime.mjs` uses Node.js built-ins, scrypt key derivation and
AES-256-GCM with a fresh random salt and nonce. Decryption authenticates the
bundle before writing the plaintext ZIP. The `.enc` file must be decrypted by
this tool before it can be opened with `Expand-Archive`.

The recommended new-machine command is `setup-runtime.mjs`. To manually restore
the same shared bundle, run these commands from the matching source checkout
with Node.js 24:

```powershell
node tools/xl_extension_store/protect-runtime.mjs decrypt --password-file tools/xl_extension_store/test-bundle-password.txt
Expand-Archive -LiteralPath './out/xl_extension_store/exports/restored-xl-store-full.zip' -DestinationPath './out/xl_extension_store'
node tools/xl_extension_store/store.mjs init
node tools/xl_extension_store/store.mjs serve
```

Decryption reads `tools/xl_extension_store/xl-store-full.zip.enc` by default and
writes `out/xl_extension_store/exports/restored-xl-store-full.zip`. Restore into
a fresh runtime: the destination may already contain the `exports/` directory
created by decryption, and empty `keys/` or `packages/` directories are fine.
It must have no existing runtime key, package or other extracted file to replace.
Omit `-Force`. For a private archive, also pass its path with `--input` and the
separately transferred password with `--password-file`. Then configure the same
hosts mapping and launch the matching browser using the **EXT** page as described
above.

Both encryption and decryption refuse to overwrite an existing output. When
refreshing the committed bundle, export a new full ZIP and use `--output` to
write a new encrypted filename, review it, then replace the tracked `.enc` file:

```powershell
node tools/xl_extension_store/export-runtime.mjs --include-signing-keys --output out/xl_extension_store/exports/xl-store-full-next.zip
node tools/xl_extension_store/protect-runtime.mjs encrypt --input out/xl_extension_store/exports/xl-store-full-next.zip --output tools/xl_extension_store/xl-store-full-next.zip.enc --password-file tools/xl_extension_store/test-bundle-password.txt
```

The shared test password is reused. A subsequent decrypt can similarly
select a new plaintext ZIP path with `--output` instead of overwriting an earlier
restore. The original runtime-only and full plaintext export commands remain
available for direct transfer outside Git.

## Browser connection

The rebuilt custom browser natively recognizes the exact XL origin and routes
first downloads from XL approvals to `/crx/<id>.crx`. Google Web Store remains
available with its original routing. No `apps-gallery-*` URL overrides are needed:

```powershell
& 'H:/chromium_142/src/out/Release_64/xlb153.exe' '--user-data-dir=H:/chromium_142/src/out/xl_extension_store/browser-profile' '--proxy-bypass-list=xl.extensions.test.com' '--disable-xenon-sidebar-auto-hide' 'chrome://xenon-extension-store/'
```

Use this dedicated profile and the rebuilt binary. Stock Chrome does not trust
the XL publisher. Do not enable test-publisher acceptance or globally disable
signature verification. The production Google publisher pin remains supported
alongside XL. The browser build and the checked-in publisher key must agree.
The hostname-specific proxy bypass keeps this hosts-mapped service on loopback.
Replace the absolute source/build paths above with your checkout paths.
`--disable-xenon-sidebar-auto-hide` keeps the sidebar visible for the `EXT` entry.
The last argument opens the packaged extension-store test page.

Open `https://xl.extensions.test.com/` for the catalog, or `/detail/<id>`.
The page checks `chrome.webstorePrivate`, incognito status and
`getExtensionStatus(id, manifest)` and `management.get(id)`. An uninstalled,
`installable` extension shows “添加至 Chrome”; an installed extension, including a
disabled one, shows “卸载”. Administrator-managed extensions that cannot be changed
keep the uninstall button disabled.
Installation calls `beginInstallWithManifest3`, waits for the user's permission
confirmation, then calls `completeInstall`. Uninstallation calls
`management.uninstall` from the user's click and keeps the browser's confirmation
dialog. After completion or cancellation, the page checks the current state and
updates the button automatically. Management events and returning to the page also
update its state, so installs or removals in another tab do not require a reload.

## Sidebar and WebUI test entry

Click the sidebar's **EXT** button, or open `chrome://xenon-extension-store/`.
The page is titled **扩展商店测试** and provides links to the actual HTTPS catalog
and the unchanged Google fixture `ngjobkbdodapjbbncmagbccommkggmnj`. The XL card
opens the catalog, where you select **XL local install test**: its ID comes from
the demo's persistent developer key and varies across new runtimes. Each link
opens a new tab. The samples must
already be imported; see `demo` and the Google import instructions below.
Installation and removal continue to run on the HTTPS store with the browser's
normal confirmation dialogs.

The page displays the current browser executable's absolute path. If the binary
is under the checkout's `out/<build>/` directory, it also prefills the source
root. Otherwise, enter the absolute source root containing
`tools/xl_extension_store/store.mjs`. Editing that field regenerates the commands;
an empty or relative path leaves the service/browser commands unavailable.

Copy the **serve** command into a PowerShell terminal and keep it running:

```powershell
& node 'H:/chromium_142/src/tools/xl_extension_store/store.mjs' 'serve' '--data-dir' 'H:/chromium_142/src/out/xl_extension_store'
```

The command uses Node.js 24 from `PATH` and an explicit runtime directory, so it
can run from any working directory. That directory must already contain the
initialized TLS certificate, its private key and the imported extension catalog.
Run `setup-runtime.mjs` first on a new machine to prepare them.
Stop the service with Ctrl+C and restart it after importing or updating packages.
The WebUI provides the command for terminal execution and does not start a
process or probe service status.

Copy the browser command to another PowerShell terminal to launch the current
binary with a dedicated `out/xl_extension_store/browser-profile`, the exact-host
proxy bypass and the visible sidebar. The page quotes every path and argument
with PowerShell single quotes, doubling embedded straight and typographic single
quotes (U+0027 and U+2018 through U+201B). No working
directory change is required. The editable source path changes the runtime and
profile paths; the executable remains the current browser binary.

Add `127.0.0.1 xl.extensions.test.com` to
`%SystemRoot%\System32\drivers\etc\hosts` through the normal administrator
workflow. Use HTTPS port 443. The rebuilt browser includes the store's public CA,
so this flow does not require installing a certificate into the system store.

For both sample packages, check installation changes the button to **卸载**,
cancelling removal preserves that state, and confirming removal restores
**添加至 Chrome**. Reinstall, remove the extension in `chrome://extensions/`,
and return to the detail page to confirm its state updates without a reload.

The page also exposes a copyable command for the six native
`XlExtensionStoreBrowserTest` cases, using the test executable beside the current
browser. Build `xl_extension_store_browsertests` first:

```powershell
& 'H:/chromium_142/src/out/Release_64/xl_extension_store_browsertests.exe' '--gtest_filter=XlExtensionStoreBrowserTest.*' '--test-launcher-jobs=1' '--lang=en-US'
```

These native tests use their own HTTPS server. They cover real XL/Google
installation, missing-publisher rejection, uninstall/reinstall behavior and
extension updates. The standard `browser_tests` target does not include these
XL-specific cases.

## Built-in store HTTPS trust

The rebuilt Chromium 153 includes the store CA's public certificate in both
profile and system certificate verifiers from their creation. A fresh profile's
first HTTPS connection can validate the store immediately; no managed CA config,
snapshot download, cache warmup, or manual reload is required. System trust stores
are not modified. HTTPS CA trust and CRX publisher signatures remain independent.

The root is an ordinary private CA with critical `CA:TRUE,pathlen:0` and
certificate-signing KeyUsage. It has no extra DNS or IP restrictions. Normal
certificate dates, chain validation and matching DNS/IP subject alternative
names still apply. The native extension-store API separately requires the exact
origin `https://xl.extensions.test.com:443`; trusting another origin's TLS
certificate does not grant it installation APIs.

The built-in root certificate's DER SHA256 is:

```text
9365a8c6469624fc2c720439bd1c65dc56bc82bbd38b10b8813b37dc0c9d1da2
```

Its public-key SPKI SHA256 is:

```text
e685858a1f8beb98eeb4cc7b5a310d7aed0881d2f4385be1a1d9d6c501bacb51
```

Only the public certificate is embedded in the browser binary. The CA signing key
is restored into ignored runtime data from the shared test bundle; the bundled
password makes that test key accessible to anyone with the checkout. Replacing
or removing the built-in trust requires a browser update. Leaf renewal under the
same CA key does not require rebuilding the browser. A new CA key requires
coordinated browser trust and server certificate deployment. `init` will not
silently invent a replacement root for a new checkout.

Open `chrome://xenon-certificates/` to inspect the built-in store certificate.
The existing dynamic managed CA mechanism remains independent: enabling,
disabling or removing its configured snapshot does not remove this built-in root.

## Optional dynamic managed CA distribution

For deployments that separately use dynamic Xenon managed CAs, the browser reads the local
JSON selected by `--xenon-managed-ca-config=<absolute path>` once at browser
startup; without the switch it reads `xenon-managed-ca.json` beside the browser
executable. This mechanism does not write Windows certificate stores. It accepts
CA certificates (`BasicConstraints CA:TRUE`, with `keyCertSign` if KeyUsage is
present), not a self-signed `CA:FALSE` server leaf.

The optional config references `out/xl_extension_store/tls/local-root-ca.pem`.
The same loopback HTTPS service on port 443
provides the managed snapshot at `/managed-ca/snapshot`, independently of the
store's extension installation and update routes. No additional listener is
required. Only public CA material is returned from this endpoint.

The browser configuration at `out/xl_extension_store/managed-ca.json` is:

```json
{
  "xenon_managed_ca": {
    "enabled": true,
    "source_id": "xl-extension-store-local",
    "endpoint": "https://xl.extensions.test.com/managed-ca/snapshot",
    "bootstrap_ca_file": "H:/chromium_142/src/out/xl_extension_store/tls/local-root-ca.pem",
    "auth": {"mode": "none"},
    "refresh_interval_seconds": 300,
    "request_timeout_seconds": 30
  }
}
```

The tool writes real absolute paths for the selected runtime directory. The bootstrap PEM is loaded only for
the independent certificate-download connection; it does not by itself trust
ordinary websites. After that HTTPS connection is verified, the endpoint must
return a complete JSON snapshot such as:

```json
{
  "schema_version": 1,
  "source_id": "xl-extension-store-local",
  "generation": "1",
  "issued_at": "2026-11-04T00:00:00Z",
  "certificates": [{
    "name": "XL local store CA",
    "role": "trust_anchor",
    "encoding": "pem",
    "data": "REPLACE WITH THE COMPLETE LOCAL CA PEM STRING"
  }]
}
```

`init` fills in the real CA PEM and UTC publication time and saves the complete
snapshot. A new optional snapshot starts at generation `1`. Migrating the same CA
identity to the current root certificate, or removing extra snapshot constraints,
increments the existing generation. The generated snapshot uses ordinary CA
trust without additional DNS or IP restrictions.
The example above only illustrates the schema. The generation is a positive decimal
**string**; use a higher value whenever changing the published snapshot. The
extension store's native API authorization remains restricted to its exact HTTPS
origin.

The configuration/snapshot protocol has no offline signature envelope. Its
source authentication comes from verified HTTPS using the locally provisioned
bootstrap CA (and optional Bearer authentication). SHA256 cache hashes and ETags
are integrity/version bookkeeping, not publisher signatures. Config, bootstrap
CA and cached state should be writable only by the intended local account or
deployment administrator.

The optional config is read once at browser startup. Restart the dedicated User
Data after changing it. The server loads snapshot bytes at startup, so restart
the store process when publishing a new generation. The source ID and endpoint
remain stable across a same-key root migration so existing clients can fetch the
new snapshot. Dynamic snapshots are cached in the browser's own User Data.

See `xenon_overlay/docs/企业CA客户端部署.md` and
`xenon_overlay/docs/企业CA下发协议设计.md` for the implemented schema and deployment
details. The existing `managed_certificates/smoke-browser.mjs` demonstrates the
same bootstrap/distribution flow without any OS certificate installation.

## Import an unchanged Google package

```powershell
node tools/xl_extension_store/store.mjs inspect C:/packages/google-extension.crx
node tools/xl_extension_store/store.mjs import C:/packages/google-extension.crx
```

`inspect` verifies all CRX3 proof signatures and reports publisher `Google` only
when a proof contains the production Google publisher public-key hash and its
signature verifies. This is cryptographic verification, not a presence-only
heuristic. A developer proof matching the declared extension ID is also required.
`import` copies the original CRX bytes unchanged, including the ZIP, developer
proof, publisher proof and any `verified_contents` header field. It extracts the
display manifest/icon from that signed ZIP without modifying the archive.

The repository includes a genuine Google-signed MV3 fixture useful for testing:

```powershell
node tools/xl_extension_store/store.mjs import chrome/test/data/extensions/signin_screen_manual_test_extension/extension_signed_by_webstore.crx
```

That fixture has ID `ngjobkbdodapjbbncmagbccommkggmnj` and version `3.0`.
It was created for Chromium sign-in screen tests; its successful local import
does not establish compatibility of every Google extension with this browser.
Manifest support, policies and permissions are still enforced by the browser.

The server accepts production Google or pinned XL publisher proofs. Ordinary
developer-only CRXs and Google's test publisher are rejected on import and again
at catalog startup. CRX2, ZIP64, encrypted ZIP metadata, malformed archives and
packages larger than 256 MiB are unsupported and fail explicitly.

## Countersign an existing developer package with XL

```powershell
node tools/xl_extension_store/store.mjs inspect C:/packages/developer.crx
node tools/xl_extension_store/store.mjs sign C:/packages/developer.crx --output out/xl_extension_store/signed/example.crx
node tools/xl_extension_store/store.mjs import out/xl_extension_store/signed/example.crx
```

`inspect` may report a null publisher for a valid developer-only package. `sign`
requires its valid developer proof, then appends an ECDSA P-256 SHA256 XL publisher
proof to the CRX3 header. It preserves every original header field, original
`signed_header_data`, developer ID/proof and ZIP bytes. It verifies the resulting
file before writing it. Passing a package already signed by this XL key is a
no-op. A Google package can be served unchanged and does not need countersigning.

The demo creates an MV3 extension with a persistent developer key, a
`developer-only.crx` negative test package and an `xl-signed.crx` positive package
under `out/xl_extension_store/demo/`. Only the XL-signed package is imported. It
requests no permissions and provides a toolbar popup. Rerunning `demo` reuses
the same developer ID and existing package bytes.

## First download versus automatic updates

The native XL store installation path downloads `/crx/<id>.crx`, which returns
the latest imported package as `application/x-chrome-extension`, not XML.
The page supplies the manifest JSON string and optional icon URL directly; no
Google metadata/snippet service is involved in this installation path.

For older Chromium API debugging only, `--apps-gallery-download-url` is a URL
template whose **first** `%s` is replaced with the extension ID. An example is
`--apps-gallery-download-url=https://xl.extensions.test.com/crx/%s.crx`.
This global debugging override is unnecessary with native XL routing and changes
the download routing for other stores too.

For XL-authored extensions, set this in the package before the developer signs it:

```json
"update_url": "https://xl.extensions.test.com/updates/<extension-id>.xml"
```

Do not edit an already signed Google ZIP to change `update_url`; that invalidates
the original signatures. Unchanged Google packages keep their original update
configuration. A self-hosted update policy, if needed, is a browser deployment
decision separate from package import.

The browser records XL installation provenance after validating the package.
Updates of those installations require a production Google or pinned XL
publisher proof when the effective update URL is not Google. A Google or empty
default update URL still requires Google's publisher proof. Effective enterprise
update URL overrides are respected; an XL URL alone does not grant XL provenance.
XL installations do not request Google's separate installation attestation.

The server's independent update endpoint implements the non-store XML2 protocol:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<gupdate xmlns="http://www.google.com/update2/response" protocol="2.0">
  <app appid="<extension-id>">
    <updatecheck version="1.0.1"
      codebase="https://xl.extensions.test.com/crx/<extension-id>/1.0.1.crx"
      hash_sha256="<SHA256 of the CRX bytes>"/>
  </app>
</gupdate>
```

The server ignores update-check query parameters and advertises the newest
imported version. The browser decides whether it is newer. Import version `1.0.1`
under the same developer ID and restart the server to publish an update. The XML
version must match the version inside the downloaded CRX. Different package bytes
with an existing ID/version are rejected to keep published versions immutable.

Keep `/updates/<id>.xml` distinct from the browser's configured store update
URL. Empty update URLs or URLs matching the store update host and path may take
Chromium's store `UpdateService` route, which uses a different update protocol;
the simple XML2 endpoint here is for explicit non-store `update_url` values.
The initial-download override does not configure automatic updates.

## Verification

```powershell
node --test tools/xl_extension_store/setup-runtime_test.mjs
node --test tools/xl_extension_store/protect-runtime_test.mjs
node --test tools/xl_extension_store/store_test.mjs
node tools/xl_extension_store/store.mjs inspect out/xl_extension_store/demo/developer-only.crx
node tools/xl_extension_store/store.mjs inspect out/xl_extension_store/demo/xl-signed.crx
```

With the HTTPS store already running and the two sample packages imported, run
the real-browser check against the rebuilt Chromium 153 executable:

```powershell
node tools/xl_extension_store/smoke-browser.mjs --browser H:/chromium_142/src/out/Release_64/xlb153.exe
```

The default script creates fresh temporary User Data and explicitly disables
dynamic managed CA delivery. The browser command line opens the XL detail page
immediately. The script attaches to that original startup tab and inspects its
location, secure context, API and enabled button without navigating or reloading
it. Additional tabs verify both detail pages using actual TLS response metadata.
Both actual detail pages must
have secure document TLS, the native `webstorePrivate` API,
`getExtensionStatus === "installable"`, and an enabled “添加至 Chrome” button.
It then confirms there was no dynamic CA attempt, accepted snapshot or cache.
It leaves the existing store process running, closes only its own browser, and
removes its own temporary profile. It does not install extensions, change OS
trust, or bypass certificate verification.

With the local CA signing key available, also run real TLS validation checks:

```powershell
node tools/xl_extension_store/smoke-browser.mjs --browser H:/chromium_142/src/out/Release_64/xlb153.exe --check-tls-validation
```

This issues separate temporary leaf certificates with matching SANs for
`child.xl.extensions.test.com`, `outside.test.com` and `127.0.0.1`, and serves
them from temporary loopback HTTPS listeners. All three must establish secure
TLS without store API access. A separate valid leaf for `different.test.com`
is accessed as `mismatch.test.com`; standard hostname validation must reject it
with `ERR_CERT_COMMON_NAME_INVALID` before any HTTP request reaches its listener.
Each leaf has only its own SAN.
The temporary signing inputs and leaf private key stay under ignored `out/` and
are removed on exit; the existing CA private key is not changed.

To explicitly test the older dynamic distribution path instead, pass
`--config H:/chromium_142/src/out/xl_extension_store/managed-ca.json`. That mode
waits for the configured snapshot and reports its actual generation. It cannot
be combined with the built-in TLS validation checks and is not evidence for
first-navigation built-in trust.

Tests cover cryptographic signatures, ID/ZIP/proof preservation, package tampering,
Google original-byte import, developer-only rejection, metadata extraction,
CA-signed HTTPS catalog/detail/manifest/icon/CRX/update/snapshot routes, generated
optional dynamic configuration, pinned-root checks, same-key CA migration and
snapshot constraint removal with generation advancement, and self-signed-leaf migration
that preserves keys. HTTPS tests use an ephemeral loopback port and
verify the real certificate chain with the generated local CA. Certificate
verification is not disabled. The default browser flow uses its embedded public
store CA for real installations.
The browser smoke validates the actual browser's TLS, API exposure and button
state. Chromium browser tests separately cover actual CRX installation and the
detail page's uninstall confirmation, cancellation, reinstallation and external
uninstall events without a page reload. The smoke script stops before the
user-confirmed install flow.

Focused native checks can be built and run without the complete Chromium test
suites:

```powershell
third_party/ninja/ninja.exe -C out/Release_64 chrome crx_file_unittests xl_extension_store_unittests xl_extension_store_chrome_unittests xl_extension_store_browsertests
out/Release_64/crx_file_unittests.exe
out/Release_64/xl_extension_store_unittests.exe
out/Release_64/xl_extension_store_chrome_unittests.exe
out/Release_64/xl_extension_store_browsertests.exe --test-launcher-jobs=2
```

The native browser fixture runs with strict installation verification and the
normal permission confirmation. It checks real XL and original Google installs,
missing-publisher rejection, XML updates from `1.0.0` to `1.0.1`, and rejection of
developer-only updates while retaining the installed version. It also covers
store API/event exposure, exact HTTPS origin restrictions and frame protection.
The unit targets cover publisher proof verification, approval origin binding,
installation provenance, effective update policy and request protection.
