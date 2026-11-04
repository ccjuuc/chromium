These public test certificates verify the browser's built-in extension store CA.
No private keys are included. The verifier tests start at 2027-01-01 using a
mock clock, inside the certificates' validity periods. The expiry test advances
that clock beyond the server certificate's validity.

- `store-leaf.pem`: existing server certificate for `xl.extensions.test.com`.
- `store-child.pem`: one DNS SAN, `child.xl.extensions.test.com`.
- `store-outside.pem`: one DNS SAN, `outside.test.com`.
- `store-ip.pem`: one IP SAN, `127.0.0.1`.
- `store-constrained-root.pem`: a reissued certificate with the same CA key and
  critical NameConstraints. This is only a test fixture, never production
  trust. It tests deduplication by public key and native trust precedence.

All four server certificates are trusted when their SAN matches the requested
host and their validity period has not ended. The authoritative public CA is
`tools/xl_extension_store/xl_store_root_ca.pem`. The additional leaves were
generated with `tools/xl_extension_store/tls-fixtures.mjs` using the ignored local
CA and server keys under `out/xl_extension_store/`.
