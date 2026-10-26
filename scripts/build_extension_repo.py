"""Lay out signed extension binaries as a DuckDB extension repository.

Usage:
    python3 build_extension_repo.py --out DIR --public-key PEM [--keep-versioned N] TAG=DIR [TAG=DIR ...]

Each TAG=DIR is a release whose DIR holds signed *.duckdb_extension.gz (native) and *.duckdb_extension.wasm
(DuckDB-WASM, uncompressed) files, listed oldest first.
The DuckDB version and platform are read from each binary's metadata footer, so file names don't matter.

Output layout (relative to --out), matching ExtensionHelper::ExtensionUrlTemplate:
    .well-known/duckdb-extension-repo.json                       public keys for CREATE EXTENSION REPOSITORY
    <duckdb_version>/<platform>/<name>.duckdb_extension.gz        newest release per DuckDB version/platform
                                                                  (.duckdb_extension.wasm for wasm_* platforms)
    <name>/<ext_version>/<duckdb_version>/<platform>/...          the last --keep-versioned releases
"""

import argparse
import gzip
import json
import shutil
import sys
from pathlib import Path

FOOTER_SIZE = 512
SIGNATURE_SIZE = 256
FIELD_SIZE = 32


def read_footer(path):
    with (gzip.open(path, 'rb') if path.suffix == '.gz' else open(path, 'rb')) as f:
        data = f.read()
    if len(data) < FOOTER_SIZE:
        raise ValueError(f'{path}: too small to be a DuckDB extension')
    metadata = data[-FOOTER_SIZE:-SIGNATURE_SIZE]
    fields = [metadata[i * FIELD_SIZE : (i + 1) * FIELD_SIZE].rstrip(b'\0').decode('ascii') for i in range(8)]
    fields.reverse()
    signature = data[-SIGNATURE_SIZE:]
    footer = {
        'magic': fields[0],
        'platform': fields[1],
        'duckdb_version': fields[2],
        'extension_version': fields[3],
        'abi_type': fields[4] or 'CPP',
    }
    if footer['magic'] != '4':
        raise ValueError(f'{path}: not a DuckDB extension (bad footer magic)')
    if signature == b'\0' * SIGNATURE_SIZE:
        raise ValueError(f'{path}: extension is not signed')
    return footer


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--public-key', required=True, type=Path, action='append')
    parser.add_argument('--name', default='gsheets')
    parser.add_argument('--keep-versioned', type=int, default=3)
    parser.add_argument('releases', nargs='*', metavar='TAG=DIR')
    args = parser.parse_args()

    releases = []
    for release in args.releases:
        tag, sep, directory = release.partition('=')
        if not sep:
            parser.error(f'expected TAG=DIR, got {release!r}')
        releases.append((tag, Path(directory)))

    well_known = args.out / '.well-known'
    well_known.mkdir(parents=True, exist_ok=True)
    keys = [key.read_text().strip() for key in args.public_key]
    (well_known / 'duckdb-extension-repo.json').write_text(json.dumps({'signature_keys': keys}, indent=2) + '\n')

    versioned_from = len(releases) - args.keep_versioned
    for index, (tag, directory) in enumerate(releases):
        binaries = sorted(directory.glob('*.duckdb_extension.gz')) + sorted(directory.glob('*.duckdb_extension.wasm'))
        if not binaries:
            print(f'{tag}: no extension binaries, skipping', file=sys.stderr)
            continue
        for binary in binaries:
            footer = read_footer(binary)
            if footer['abi_type'] != 'CPP':
                raise ValueError(f'{binary}: unsupported ABI type {footer["abi_type"]}')
            duckdb_version, platform = footer['duckdb_version'], footer['platform']
            filename = f'{args.name}.duckdb_extension{binary.suffix}'
            # newer releases overwrite older ones
            targets = [args.out / duckdb_version / platform / filename]
            if index >= versioned_from:
                targets.append(args.out / args.name / tag / duckdb_version / platform / filename)
            for target in targets:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(binary, target)
            print(f'{tag}: {duckdb_version}/{platform} ({footer["extension_version"]})')


if __name__ == '__main__':
    main()
