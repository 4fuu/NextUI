# Mozilla CA bundle for nextupd (`ca-bundle.crt`)

This is the provenance and license record for the CA trust store shipped as
`skeleton/SYSTEM/res/ca-bundle.crt` and installed on the device as
`.system/res/ca-bundle.crt`.

## Shipped asset

| Field | Value |
| --- | --- |
| Path | `skeleton/SYSTEM/res/ca-bundle.crt` |
| Size | 200,116 bytes |
| SHA-256 | `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285` |
| Format | PEM, Mozilla root CA extract |
| Source | https://curl.se/ca/cacert.pem |
| Retrieved | 2026-11-17 |
| Mozilla certificate data as of | Thu Nov 5 04:12:01 2026 GMT |
| Extraction | `mk-ca-bundle.pl` 1.35 from Mozilla `certdata.txt` (recorded in the bundle header) |
| License | Mozilla Public License 2.0, see `ca-bundle-MPL-2.0.txt` |

The file is an unmodified copy of `cacert.pem` as published by curl.se on the
retrieval date. Its SHA-256 is the downloaded file's digest; the bundle header
also carries the extractor's own source digest.

## Why it ships

The TrimUI Brick Pro image (tg5040, v6.16.0) has no system CA store:
`/etc/ssl`, `/usr/share/ca-certificates`, and `/etc/ca-certificates` are all
absent. nextupd hands HTTPS downloads to the system `curl`, so without a
bundle every download fails with `curl` exit 60 ("unable to get local issuer
certificate"). Shipping the bundle in the shared resource tree and passing it
with `curl --cacert` keeps certificate verification enabled; there is still no
insecure fallback.

`scripts/package-update.sh` stages the shared resource tree into
`.system/res/` for both the base install payload and the transactional update
archive, so this file and its SHA-256 are covered by the update manifest like
every other resource.

## Refresh

Download a new `https://curl.se/ca/cacert.pem`, replace `ca-bundle.crt`, update
the size, SHA-256, and Mozilla date above, and run the packaging tests. Do not
edit the PEM or append private certificates: the file is the public Mozilla
extract, and the update manifest carries its hash.
