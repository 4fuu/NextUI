#!/bin/sh
# Release-side helper: sign an exact nextui-update-v1 manifest with Ed25519.
#
# Usage:
#   sign-manifest.sh <ed25519-private-key.pem> <key-id> <manifest.json> <signature.out>
#
# This does not create or manage production keys. The key id is an audit label;
# the detached signature is Ed25519 over the manifest bytes exactly as written.
# The matching nextui-public-key-v1 sidecar can be produced with:
#
#   printf 'nextui-public-key-v1\n%s\n' "$key_id" > key.pub
#   openssl pkey -in key.pem -pubout -outform DER | tail -c 32 | base64 | tr -d '\n' >> key.pub
#   printf '\n' >> key.pub
#
# Run only on a trusted release host.
set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <private-key.pem> <key-id> <manifest.json> <signature.out>" >&2
    exit 64
fi

key_file=$1
key_id=$2
manifest=$3
output=$4

for tool in openssl base64 mktemp; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "sign-manifest: missing $tool" >&2
        exit 69
    }
done

case "$key_id" in
    ''|*[!A-Za-z0-9._-]*)
        echo "sign-manifest: invalid key id" >&2
        exit 65
        ;;
esac

temporary=$(mktemp "${TMPDIR:-/tmp}/nextui-signature.XXXXXX")
trap 'rm -f "$temporary" "$output.part"' EXIT INT TERM

openssl pkeyutl -sign -rawin -inkey "$key_file" -in "$manifest" -out "$temporary"

{
    printf 'nextui-signature-v1\n'
    printf '%s\n' "$key_id"
    base64 < "$temporary" | tr -d '\n'
    printf '\n'
} > "$output.part"

mv "$output.part" "$output"
