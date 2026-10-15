#!/usr/bin/env python3
"""Package reviewed fabrication data and portable KiCad sources.

Run the export, electrical, mechanical, routing and independent Gerber audits
first. --reviewed records that the exported drawings and 3D assembly were viewed.
It does not claim physical testing or close the conditional timing budget.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import zipfile

from design import ROOT, REVISION, BOARDS, release_name, copper_layers


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_tree(source, destination):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns(
        '__pycache__', '*.pyc', '*.kicad_prl', '~*.lck', '*-backups'), dirs_exist_ok=True)


def checksums(folder):
    entries = [(digest(p), p.relative_to(folder).as_posix())
               for p in sorted(folder.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS']
    (folder / 'SHA256SUMS').write_text(''.join(f'{sha}  {name}\n' for sha, name in entries))


def archive(folder, output):
    checksums(folder)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(folder.rglob('*')):
            if path.is_file():
                z.write(path, folder.name + '/' + path.relative_to(folder).as_posix())
    with zipfile.ZipFile(output) as z:
        assert z.testzip() is None, output
        for line in z.read(folder.name + '/SHA256SUMS').decode().splitlines():
            expected, relative = line.split('  ', 1)
            assert hashlib.sha256(z.read(folder.name + '/' + relative)).hexdigest() == expected


def order_notes(name):
    display = name == 'display-adapter'
    return f'''ProtoMate Smartwin–GIGA / {name} / revision {BOARDS[name]['revision']}
PROTOTYPE FABRICATION AND SMT ASSEMBLY PACKAGE

FABRICATION
{BOARDS[name]['layers']}-layer FR-4, 1 oz copper on every copper layer, ENIG.
Green solder mask, white silkscreen, IPC Class 2, 100% electrical test.
Finished board: {'55 x 35 x 1.0 mm' if display else '111 x 53.34 x 1.6 mm maximum; C-shaped outline'}.
Layer order: {' / '.join(copper_layers(name))}.
Gerbers and separate PTH/NPTH drills in gerbers/ are authoritative.
Minimum track/clearance 0.15 mm; minimum via drill {BOARDS[name]['via_drill']:.2f} mm,
minimum via diameter {BOARDS[name]['via_diameter']:.2f} mm, annular ring >=0.15 mm.
Preserve 0.13 mm fine-pitch mask webs. Tent vias on both sides; no via-in-pad.
Panelize with assembly rails as required without changing the individual outline.
Do not mirror bottom Gerbers. See fabrication-and-assembly-notes.pdf.

ASSEMBLY
Populate exactly bom-pcbway-smt.csv using placement.csv.
{'Both sides populated; DF23 J1 on BOTTOM.' if display else 'Top side SMT only.'}
All SMT BOM entries FIT; no DNP SMT components.
{'J3 power terminal' if display else 'J2–J9 stacking headers'}: HAND installation, not part of PCBWay SMT placement.
Full procurement BOM, including HAND parts: bom-all.csv.
Placement units mm; origin X=0 at lower board edge; positive Y upward.
Bottom-side X is not negated. Native KiCad rotations; use assembly drawings.
No substitutions for Hirose connector suffixes or Samtec header tail lengths.
Hirose FH28 paste: 0.25 x 1.00 mm windows, 0.15 mm stencil at connector.
Use a stepped stencil, 0.10–0.12 mm at 0402/TSSOP, or an explicitly reviewed
equivalent assembly process. Do not silently enlarge the apertures.
{'Trim J3 leads to <=0.8 mm below PCB after hand soldering. J3 pin 1 = +3.3 V.' if display else 'Carrier underside 13.0 mm above GIGA; J2–J8 10 mm tails, J9 14.83 mm tails.'}

CONTENTS
gerbers/: copper, mask, silk, paste, outline, drill files and drill maps.
electrical-test.ipc: IPC-D-356 electrical test data.
assembly-F/B.pdf: component references, pad numbering and orientation.
schematic.pdf, fabrication-and-assembly-notes.pdf and {name}.step.
source/{name}/: editable KiCad project; shared source/libraries/ alongside it.
Cable specification and complete signal/conductor maps are included.
verification/: ERC/DRC, independent Gerber, electrical, mechanical and timing audits.
SHA256SUMS: exact package contents. Each ZIP has a separate archive checksum.

QUALIFICATION STATUS
Digital fabrication checks passed with no ERC/DRC violations or unrouted nets.
This is a prototype release. Physical fit, power isolation, full-brightness
voltage/current/temperature, display/touch and timing have NOT been tested.
The 51.2 MHz timing calculation has sub-nanosecond residual margins before
unmodelled terms; it is not complete worst-case PVT timing closure.
See design-guide.md and verification/timing-budget.json before first-board tests.
No manufacturing order has been placed.
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reviewed', action='store_true', required=True)
    parser.add_argument('--boards', nargs='+', choices=list(BOARDS), default=list(BOARDS))
    args=parser.parse_args()
    manufacturing = ROOT / 'manufacturing'
    verification = manufacturing / 'verification'
    status = json.loads((manufacturing / 'release-status.json').read_text())
    assert status['digital_release_checks'] == 'PASS' and status['revision'] == REVISION
    names = ['display-adapter', 'giga-adapter']
    for name in names:
        assert status['boards'][name]['revision'] == BOARDS[name]['revision']
        assert status['boards'][name]['copper_layers'] == BOARDS[name]['layers']
        erc = json.loads((verification / (name + '-erc.json')).read_text())
        drc = json.loads((verification / (name + '-drc.json')).read_text())
        assert not erc['ignored_checks'] and not drc['ignored_checks']
        assert not any(s['violations'] for s in erc['sheets'])
        assert not drc['violations'] and not drc['unconnected_items']
        for suffix in ['bom-placement', 'gerber-audit']:
            assert json.loads((verification / (name + '-' + suffix + '.json')).read_text())['result'] == 'PASS'
        job = json.loads((manufacturing / release_name(name) / 'gerbers' /
                          (name + '-job.gbrjob')).read_text())
        assert job['GeneralSpecs']['LayerNumber'] == BOARDS[name]['layers']
        project = json.loads((ROOT / name / (name + '.kicad_pro')).read_text())
        assert not project['board']['design_settings']['drc_exclusions']
        assert not project['erc']['erc_exclusions']

    source_files = [p for folder in [*names, 'libraries'] for p in (ROOT / folder).rglob('*')
                    if p.is_file() and p.suffix not in ['.kicad_prl'] and not p.name.startswith('~')]
    (verification / 'source-sha256.json').write_text(json.dumps(
        {p.relative_to(ROOT).as_posix(): digest(p) for p in sorted(source_files)}, indent=2) + '\n')
    releases = ROOT / 'releases'
    releases.mkdir(exist_ok=True)
    # The package guide omits repository-specific archive links.
    guide = (ROOT / 'README.md').read_text()
    start = guide.index('Release files:')
    end = guide.index('| Project |')
    guide = guide[:start] + guide[end:]
    with tempfile.TemporaryDirectory(prefix='display-release-') as scratch:
        temporary = Path(scratch)
        for name in args.boards:
            folder = temporary / release_name(name)
            copy_tree(manufacturing / folder.name, folder)
            copy_tree(verification, folder / 'verification')
            for part in [name, 'libraries', 'references']:
                copy_tree(ROOT / part, folder / 'source' / part)
            for item in ['cable-specification.pdf', 'connections.csv', 'cable-conductors.csv']:
                shutil.copy2(ROOT / item, folder / item)
            shutil.copy2(manufacturing / 'release-status.json', folder / 'release-status.json')
            (folder / 'PCBWAY_ORDER.txt').write_text(order_notes(name))
            (folder / 'design-guide.md').write_text(guide)
            archive(folder, releases / (folder.name + '-pcbway.zip'))

        folder = temporary / ('smartwin-giga-rev-' + REVISION)
        for part in [*names, 'libraries', 'references', 'tools', 'assembly']:
            copy_tree(ROOT / part, folder / part)
        for item in ['README.md', 'cable-specification.pdf', 'connections.csv', 'cable-conductors.csv', '.gitignore']:
            shutil.copy2(ROOT / item, folder / item)
        copy_tree(verification, folder / 'manufacturing' / 'verification')
        shutil.copy2(manufacturing / 'release-status.json', folder / 'manufacturing' / 'release-status.json')
        archive(folder, releases / (folder.name + '-source.zip'))
    checksums(releases)
    print('Verified archives:')
    for path in sorted(releases.glob('*.zip')):
        print(f'{path.name}: {path.stat().st_size / 1024**2:.2f} MiB')


if __name__ == '__main__':
    main()
