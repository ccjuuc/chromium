# XL store installation fixtures

`xl-signed.crx` and `developer-only.crx` contain the same MV3 extension, ID
`mcjbgmlghdgeocallnajjfbblpondljm`, version `1.0.0`. Both have a valid developer
signature. Only `xl-signed.crx` also has the publisher signature matching the
production XL public key in `tools/xl_extension_store/xl_publisher_public_key.pem`.
`xl-manifest.json` is the original manifest from their ZIP archive.

`xl-signed-1.0.1.crx` and `developer-only-1.0.1.crx` are update fixtures for
the same extension ID. Their manifest differs from version `1.0.0` only in
`version`, now `1.0.1`; all other ZIP file contents are unchanged. Both retain a
valid developer proof for the same developer key. Only `xl-signed-1.0.1.crx`
includes the pinned XL publisher proof. The developer-only update is a negative
test case and must fail the publisher requirement. These public test fixtures
are not automatically imported into the running local store catalog.

The packages were generated with the local store tooling. These files contain
public signatures, not private keys. The browser test uses the production
publisher requirements, without a signature-verification override.

`google-manifest.json` is extracted verbatim from the existing Google-signed
`../signin_screen_manual_test_extension/extension_signed_by_webstore.crx`.
The browser test serves that original CRX directly, retaining its signatures,
extension ID `ngjobkbdodapjbbncmagbccommkggmnj`, version `3.0`, and Google update URL.

SHA-256 of the CRX files:

| Package | SHA-256 |
| --- | --- |
| `xl-signed.crx` | `fa829264419155889239092eeb97153459aa558aa1c65e595ade0cb91243863d` |
| `developer-only.crx` | `7241ac4b26d7988203c506b78e9d6e46188b786c26c963bd7414fca9aa6a70dd` |
| `xl-signed-1.0.1.crx` | `80a42f6967b3d41f491fa4191c73194253b69b16de223123e06f4ca97831ea86` |
| `developer-only-1.0.1.crx` | `be462425268cf651485db469fa502dc148302bef98c16de85017fc272b31d142` |
| Original Google CRX | `9aec65382aaa842d76487db89e2a5551b5b139b112cc3b64a861913d48bbab9b` |
