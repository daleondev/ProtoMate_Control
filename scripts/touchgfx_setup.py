#!/usr/bin/env python3
"""Reproduce the vendored TouchGFX subset from the pinned ST archive."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    metadata = json.loads((ROOT/'external/touchgfx/sdk.json').read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT/'external/touchgfx')
    args = parser.parse_args()
    with args.archive.open('rb') as stream:
        checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
    if checksum != metadata['sha256']:
        raise SystemExit('SDK archive checksum differs from the pinned package; inspect before updating sdk.json')
    prefix = metadata['sdk_subdirectory']+'/'
    contents = {}
    with zipfile.ZipFile(args.archive) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            if entry.filename.startswith(prefix):
                name = entry.filename[len(prefix):]
                if name not in metadata['files'] and not any(
                        name.startswith(directory + '/') for directory in metadata['directories']):
                    continue
                relative = Path(name)
            elif entry.filename in metadata['license_files']:
                relative = Path(metadata['license_files'][entry.filename])
            else:
                continue
            if relative.is_absolute() or '..' in relative.parts:
                raise SystemExit('Unsafe archive path')
            contents[relative.as_posix()] = archive.read(entry)
    required = metadata['files'] + list(metadata['license_files'].values())
    if any(name not in contents for name in required) or any(
            not any(name.startswith(directory + '/') for name in contents)
            for directory in metadata['directories']):
        raise SystemExit('Incomplete SDK archive; no files written')
    manifest = []
    for name, data in sorted(contents.items()):
        target = args.output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        # Avoid forcing asset regeneration when restoring identical files.
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
        target.chmod(0o755 if name.endswith('.out') else 0o644)
        manifest.append(f'{hashlib.sha256(data).hexdigest()}  {name}\n')
    (args.output/'SHA256SUMS').write_text(''.join(manifest))
    print(f'Extracted {len(contents)} unmodified files. TOUCHGFX_ROOT={args.output.resolve()}')
    print('License files retained. Host tools still require Ruby/nokogiri, SDL2 and librsvg.')

if __name__ == '__main__':
    main()
