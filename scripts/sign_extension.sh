#!/bin/bash

# Sign a DuckDB extension binary for our self-hosted extension repository

# Usage: ./sign_extension.sh <input> <output> [<public_key>]
# <input>      : Unsigned extension binary (as produced by the distribution build)
# <output>     : Where to write the signed binary
# <public_key> : (optional) PEM public key to verify the signature against
#
# The private key is read from the DUCKDB_EXTENSION_SIGNING_PK environment variable (PEM).
# The signature scheme mirrors duckdb/scripts/extension-upload-single.sh: the last 256 bytes of
# the binary hold an RSA signature over sha256(sha256(chunk_1) || ... || sha256(chunk_n)), where
# the chunks are 1MiB slices of everything before the signature.

set -euo pipefail

if [ "$#" -lt 2 ]; then
	echo "Usage: $0 <input> <output> [<public_key>]"
	exit 1
fi

input="$1"
output="$2"
public_key="${3:-}"

if [ -z "${DUCKDB_EXTENSION_SIGNING_PK:-}" ]; then
	echo "DUCKDB_EXTENSION_SIGNING_PK is not set"
	exit 1
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/gsheets-sign.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

file_size() {
	if stat -c%s "$1" >/dev/null 2>&1; then
		stat -c%s "$1"
	else
		stat -f%z "$1"
	fi
}

# strip the (empty) signature placeholder
size=$(file_size "$input")
head -c $((size - 256)) "$input" > "$work_dir/payload"

# two-level hash
split -b 1M "$work_dir/payload" "$work_dir/chunk_"
for f in "$work_dir"/chunk_*; do
	openssl dgst -binary -sha256 "$f" >> "$work_dir/hash_concats"
done
openssl dgst -binary -sha256 "$work_dir/hash_concats" > "$work_dir/hash"

printf '%s\n' "$DUCKDB_EXTENSION_SIGNING_PK" > "$work_dir/key.pem"
openssl pkeyutl -sign -in "$work_dir/hash" -inkey "$work_dir/key.pem" -pkeyopt digest:sha256 -out "$work_dir/sig"

if [ "$(file_size "$work_dir/sig")" -ne 256 ]; then
	echo "Signature is not 256 bytes - is the signing key a 2048-bit RSA key?"
	exit 1
fi

if [ -n "$public_key" ]; then
	openssl pkeyutl -verify -pubin -inkey "$public_key" -in "$work_dir/hash" -sigfile "$work_dir/sig" \
		-pkeyopt digest:sha256 >/dev/null
fi

mkdir -p "$(dirname "$output")"
cat "$work_dir/payload" "$work_dir/sig" > "$output"
