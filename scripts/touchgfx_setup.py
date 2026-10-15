#!/usr/bin/env python3
"""Extract the pinned, user-downloaded ST SDK; never download or accept a license implicitly."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    metadata = json.loads((ROOT/'src/ui/gui/assets/sdk.json').read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT/'apps/touchgfx-4.27.0')
    args = parser.parse_args()
    with args.archive.open('rb') as stream:
        checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
    if checksum != metadata['sha256']:
        raise SystemExit('SDK archive checksum differs from the pinned package; inspect before updating sdk.json')
    prefix = metadata['sdk_subdirectory']+'/'
    count = 0
    with zipfile.ZipFile(args.archive) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            if entry.filename.startswith(prefix):
                relative = Path('touchgfx') / entry.filename[len(prefix):]
            elif '/' not in entry.filename and ('license' in entry.filename.lower() or entry.filename.endswith('.pdsc')):
                relative = Path(entry.filename)
            else:
                continue
            if relative.is_absolute() or '..' in relative.parts:
                raise SystemExit('Unsafe archive path')
            target = args.output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(entry))
            count += 1
    for tool in ('fontconvert', 'imageconvert'):
        (args.output/f'touchgfx/framework/tools/{tool}/build/linux/{tool}.out').chmod(0o755)
    (args.output/'package-sha256.txt').write_text(checksum+'\n')
    print(f'Extracted {count} files. TOUCHGFX_ROOT={args.output.resolve()/"touchgfx"}')
    print('Retain and review the ST package license. Install Ruby/nokogiri, SDL2 and librsvg for asset builds.')

if __name__ == '__main__':
    main()
