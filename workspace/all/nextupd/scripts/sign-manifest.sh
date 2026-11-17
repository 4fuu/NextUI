#!/bin/sh
# Release-side helper: sign an exact nextui-update-v1 manifest with Ed25519.
#
# Usage:
#   sign-manifest.sh <ed25519-private-key.pem> <key-id> <manifest.json> <signature.out>
#   sign-manifest.sh --key-env <NAME> <key-id> <manifest.json> <signature.out>
#
# --key-env reads the PEM private key from the named environment variable, for
# a signing host that keeps the key in a confidential store and injects it at
# run time. The key then never appears in argv, in a repository file, or in any
# output. The name is validated by rejection before the lookup, the lookup
# itself evaluates nothing, and the variable is unset once read, so the key is
# not inherited by the openssl process. OpenSSL reads the key from a path, so
# the PEM is written to a private temporary file (mode 0600) that is removed on
# every exit path.
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

usage() {
    echo "usage: $0 <private-key.pem> <key-id> <manifest.json> <signature.out>" >&2
    echo "       $0 --key-env <NAME> <key-id> <manifest.json> <signature.out>" >&2
}

key_env=
key_env_given=0
if [ "$#" -ge 1 ] && [ "$1" = "--key-env" ]; then
    if [ "$#" -lt 2 ]; then
        usage
        exit 64
    fi
    key_env=$2
    key_env_given=1
    shift 2
fi

# The key file is positional, unless --key-env replaced it with the named
# environment variable.
expected_args=4
if [ "$key_env_given" -eq 1 ]; then
    expected_args=3
fi
if [ "$#" -ne "$expected_args" ]; then
    usage
    exit 64
fi

if [ "$key_env_given" -eq 1 ]; then
    key_id=$1
    manifest=$2
    output=$3
else
    key_file=$1
    key_id=$2
    manifest=$3
    output=$4
fi

for tool in openssl base64 mktemp printenv; do
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

key_temp=
signature_temp=
cleanup() {
    rm -f "$key_temp" "$signature_temp" "$output.part"
}
trap cleanup EXIT INT TERM

if [ "$key_env_given" -eq 1 ]; then
    # The name is used to look the key up, so it is validated by rejection:
    # empty, a leading digit, and any byte outside [A-Za-z0-9_] anywhere are
    # refused before the lookup happens. An acceptance pattern would not do:
    # its trailing wildcard would let "AB-C" through.
    case "$key_env" in
        ''|[0-9]*|*[!A-Za-z0-9_]*)
            echo "sign-manifest: invalid --key-env name" >&2
            exit 64
            ;;
    esac
    # printenv looks the name up without evaluating anything, so a name can
    # never become a command. The value itself is never printed.
    if ! key_material=$(printenv "$key_env"); then
        echo "sign-manifest: environment variable $key_env is unset" >&2
        exit 65
    fi
    if [ -z "$key_material" ]; then
        echo "sign-manifest: environment variable $key_env is empty" >&2
        exit 65
    fi
    # The key is now in this shell and in the private temporary file below;
    # removing it from the environment keeps it out of every child process,
    # including the openssl that reads the file.
    unset "$key_env" || {
        echo "sign-manifest: cannot unset $key_env" >&2
        exit 65
    }
    key_temp=$(mktemp "${TMPDIR:-/tmp}/nextui-signing-key.XXXXXX")
    chmod 600 "$key_temp"
    printf '%s\n' "$key_material" > "$key_temp"
    unset key_material
    key_file=$key_temp
fi

signature_temp=$(mktemp "${TMPDIR:-/tmp}/nextui-signature.XXXXXX")

openssl pkeyutl -sign -rawin -inkey "$key_file" -in "$manifest" -out "$signature_temp"

{
    printf 'nextui-signature-v1\n'
    printf '%s\n' "$key_id"
    base64 < "$signature_temp" | tr -d '\n'
    printf '\n'
} > "$output.part"

mv "$output.part" "$output"
