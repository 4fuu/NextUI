# Production update source and signing trust chain

Status: the source and the trust chain are **configured as far as they can be
without a release**. The build-side and verifier-side contracts already exist
([`UPDATE-PACKAGING.md`](UPDATE-PACKAGING.md) produces the package sets,
[`UPDATES.md`](UPDATES.md) verifies them); this page records the production
decisions and the procedures that use them. Nothing here publishes a release,
and nothing here is live yet — see [What is not configured yet](#what-is-not-configured-yet).

## Decision

This fork's update source is **this repository's GitHub Releases**, using the
signed assets a release publishes. There is no application-store source, no
store fork, and no store index. The update path is the one already in `main`:

- the client checks a `nextui-feed-v1` feed with `nextupd feed`,
- the feed names a signed `nextui-update-v1` manifest per platform and version,
- only that manifest's detached Ed25519 signature, checked against a trusted
  public key, authorizes staging and installing.

The feed is routing metadata, not a trust anchor. A release that publishes an
unsigned or wrongly signed set produces a feed the client refuses, not an
update it applies.

## What a release publishes

`make update-package PLATFORM=<p> UPDATE_VERSION=vX.Y.Z ...` (see
[`UPDATE-PACKAGING.md`](UPDATE-PACKAGING.md)) publishes one package set per
platform into `releases/`:

| Asset | Meaning |
| --- | --- |
| `nextui-system-<platform>.zip` | the transactional archive (`.system/` plus the signed `.tmp_update/` boot payload) |
| `nextui-system-<platform>.manifest.json` | the `nextui-update-v1` manifest |
| `nextui-system-<platform>.manifest.sig` | the detached Ed25519 signature |
| `nextui-system-<platform>.manifest.set` | the packaging script's commit marker; not uploaded, not read by the client |

The release uploads the first three per platform, plus one more asset:

| Asset | Meaning |
| --- | --- |
| `nextui-feed.json` | the `nextui-feed-v1` document, built by [`scripts/generate-feed.py`](scripts/generate-feed.py) |

Release tags are `vX.Y.Z`, matching the authoritative semver the manifest and
`.system/version.txt` carry.

## Building the feed

```sh
scripts/generate-feed.py \
    --set-dir releases \
    --base-url https://github.com/4fuu/nextui/releases/download/v6.16.0 \
    --previous https://github.com/4fuu/nextui/releases/download/v6.15.0/nextui-feed.json \
    --nextupd workspace/all/nextupd/build/desktop/nextupd.elf \
    --out nextui-feed.json
```

For the **first** release there is no previous feed: omit `--previous` (or pass
a URL that 404s) and the feed starts with this release's rows only.

- Every entry field comes from the signed manifest, and the archive bytes are
  hashed here, so the feed cannot advertise a file the release does not
  contain. A missing signature sidecar, an `app` package, a duplicate
  platform/version, an unsafe asset name, or a manifest without `min_system`
  is refused instead of published.
- `--previous` merges the currently published feed (a file path or an HTTP(S)
  URL), so each release keeps the previous compatible rows: a system too old
  for the newest `min_system` installs the older compatible row, and
  user-initiated downgrades keep working. Rows for the same platform and
  version are replaced, never duplicated, and a previous row is rebuilt from
  the nine known fields, because the client rejects unknown ones.
- `--nextupd` runs the real verifier (`check --integrity-only`) over every
  archive, so a package that cannot install never reaches a feed. That check
  is archive/manifest/content consistency, **not authenticity**: it does not
  look at the signature. Authenticity is verified separately, after signing,
  against the committed public key (see [Signing a release](#signing-a-release)).
- The generator reads no key of any kind. It publishes nothing; the release
  workflow uploads what it writes.

The wiring into `ci.yaml`/`release.yaml` belongs to the CI owner and is not
part of this page.

## The signing trust chain

```text
production Ed25519 key pair
  private key  -> persistent confidential storage, never in this repository,
                  never in logs, argv, or a transcript
  public key   -> nextui-public-key-v1 sidecar, committed here and shipped to
                  the client as NEXTUI_UPDATE_TRUSTED_KEY
signature      -> sign-manifest.sh over the exact manifest bytes
```

The key pair is generated once, on a trusted release host, by the owner. The
committed test seed under `workspace/all/nextupd/tests/fixtures` is test-only:
it reproduces fixtures and must never sign a release, and it is never promoted
to a production key.

### Custody procedure (owner step)

The confidential facility is the Amp secrets store, personal scope, owned by
the repository owner. It persists beyond any machine, and its values are never
returned by any tool — a stored key can be injected at signing time but not
read back, which is what "verify restore" means here: the release host injects
it, signs, and the signature verifies against the committed public key.

On a trusted host (the owner's machine, not a shared or temporary one):

```sh
# 1. Generate the production key pair. From here on the private key never
#    enters the repository, a log, or a conversation.
openssl genpkey -algorithm ed25519 -out nextui-release-1.pem

# 2. Derive the public sidecar and commit it to this repository, at
#    workspace/all/nextupd/release/nextui-release-1.pub. The sidecar is
#    public; the PEM is not.
printf 'nextui-public-key-v1\n%s\n' nextui-release-1 > nextui-release-1.pub
openssl pkey -in nextui-release-1.pem -pubout -outform DER | tail -c 32 \
    | base64 | tr -d '\n' >> nextui-release-1.pub
printf '\n' >> nextui-release-1.pub

# 3. Store the private key in the confidential facility (personal scope).
amp secrets set --user NEXTUI_RELEASE_SIGNING_KEY --secret \
    --data-file nextui-release-1.pem

# 4. Shred the local copy; the facility now holds the only private key.
shred -u nextui-release-1.pem
```

Restore is proven by use, not by reading back: the release host injects the
secret, signs a manifest, and `nextupd check` accepts that signature against
the committed sidecar. That proof happens at the first signed release.

### Signing a release

With the key in the facility, the signing host injects it and never writes it
to the repository:

```sh
amp --no-tui --amp-env sh -c '
    make update-package PLATFORM=tg5040 UPDATE_VERSION=v6.16.0 \
        UPDATE_MIN_SYSTEM=v6.0.0
    workspace/all/nextupd/scripts/sign-manifest.sh \
        --key-env NEXTUI_RELEASE_SIGNING_KEY nextui-release-1 \
        releases/nextui-system-tg5040.manifest.json \
        releases/nextui-system-tg5040.manifest.sig
'
```

`--key-env` reads the PEM from the named environment variable, so the key
never appears in argv, in a repository file, or in any output. The variable
name is validated by rejection before the lookup (empty, a leading digit, or
any byte outside `[A-Za-z0-9_]` is refused), the lookup itself evaluates
nothing, and the variable is unset once read so the key is not inherited by
the openssl process. OpenSSL reads the key from a path, so the PEM is written
to a private mode-0600 temporary file that is removed on every exit path.
`package-update.sh` signs in place when given `UPDATE_KEY`/`UPDATE_KEY_ID`
pointing at a key file on the trusted host.

After signing, verify every manifest against the **committed public key** —
this needs no private key, and it is the only step that proves authenticity,
because the generator's `--nextupd` check is integrity-only:

```sh
nextupd check --package releases/nextui-system-tg5040.zip \
    --manifest releases/nextui-system-tg5040.manifest.json \
    --signature releases/nextui-system-tg5040.manifest.sig \
    --trusted-key workspace/all/nextupd/release/nextui-release-1.pub \
    --platform tg5040 | grep 'signature: verified'
```

`scripts/prepare-release-update-assets.sh` runs exactly this check for every
platform and refuses to publish the set when it fails, so a release signed
with a key the client does not trust never reaches the release page.

Signing after packaging, as above, leaves the packaging script's
`<base>.manifest.set` marker recording the unsigned publish. The marker is an
operator completeness record that the client never reads — `nextupd check` is
the guard — and it is not uploaded to the release, so the stale marker only
makes the next `update-package` run warn and rebuild.

### Client provisioning

The client carries only the public sidecar. The settings System Update page
reads two environment variables and nothing else:

| Variable | Value |
| --- | --- |
| `NEXTUI_UPDATE_FEED` | the published feed URL, `https://github.com/4fuu/nextui/releases/download/<tag>/nextui-feed.json` |
| `NEXTUI_UPDATE_TRUSTED_KEY` | the path of the `nextui-public-key-v1` sidecar on the card |

Neither variable is set by this repository, and neither is built into the
client: until the image provisions them, the page reports "Update source is
not configured." and installs nothing. Provisioning them is part of shipping
the first release, together with copying the sidecar onto the card.

## What is not configured yet

Honest boundaries, so nothing here reads as more ready than it is:

- **No release exists.** This repository has no tags and no GitHub Release,
  so no feed URL is live and `nextupd feed`/`fetch` keep reporting
  `feed_unconfigured` (exit 15) until `NEXTUI_UPDATE_FEED` is set and a
  signed release publishes the feed.
- **No production key exists.** The key pair has not been generated: an orb
  cannot write the confidential facility without the key transiting a
  transcript, which the custody rules forbid, so the procedure above runs on
  the owner's machine. Until then the public sidecar is not committed, no
  manifest is signed with a production key, and the committed test key
  remains test-only.
- **No client provisioning.** `NEXTUI_UPDATE_FEED` and
  `NEXTUI_UPDATE_TRUSTED_KEY` are not set anywhere in this repository.
- **No release workflow wiring.** `ci.yaml`/`release.yaml` do not yet build
  the update package, sign it, generate the feed, or upload assets; that
  belongs to the CI owner and needs the key procedure above first.
- **No store.** There is no store source, fork, or index, and none is planned.

## Tests

```sh
make -C tests test-generate-feed
```

The host tests drive the real signer and the real `nextupd` binary over
synthetic published sets: the generated feed is accepted by `nextupd feed`
and selects the intended row (`update_available`, `up_to_date`,
`system_too_old`, per-platform rows, `--previous` merge and replacement),
every refusal keeps an unusable set out of a feed, and `sign-manifest.sh
--key-env` signs from an environment variable without ever echoing the key.
They use the committed test-only seed, publish nothing, and touch no device.
