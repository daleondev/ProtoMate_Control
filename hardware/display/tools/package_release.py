#!/usr/bin/env python3
"""Package reviewed fabrication data and portable KiCad sources.

Run the export, electrical, mechanical, routing and independent Gerber audits
first. Packaging requires a complete, current per-region visual review for each
board. There is no blanket flag that can substitute for recorded inspection.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import zipfile

from design import ROOT, REVISION, BOARDS, release_name, copper_layers
from aisler import SUPPORT
from release import design_hashes
from review_atlas import validate as validate_visual_review
from layout_policy import validate_policy


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_tree(source, destination):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns(
        '.history', '.git', '__pycache__', '*.pyc', '*.kicad_prl', '~*.lck',
        '*-backups', '_restore_backup_*'), dirs_exist_ok=True)


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
    return f'''ProtoMate / {name} / revision {BOARDS[name]['revision']}
AISLER BARE-BOARD REVIEW PACKAGE — NOT THE UPLOAD ZIP

UPLOAD: {release_name(name)}-aisler-odb.zip (unchanged KiCad ODB++ archive).
Choose {BOARDS[name]['layers']} layers, 1.6 mm +/-10%, 35 um copper on every layer,
ENIG, green solder mask and white silkscreen. Bare boards only.
No assembly service, stencil or component purchase is included.

Layer order: {' / '.join(copper_layers(name))}.
{'55 x 50 mm display board.' if display else '111 x 53.34 mm maximum, with the drawn service cutouts and power tab.'}
Keep the closed profile; bridge positions are on the AISLER-BRIDGES document layer.
All vias are exposed through-vias. No tenting, filling, plugging or via-in-pad.
Zero mask expansion in source. Check AISLER processed webs >=0.10 mm.
Minimum component-hole ring 0.30 mm; J8 and U8 narrow pads 1.70 mm.
Published dielectric spacing: 0.14 / 1.20 / 0.14 mm; four 35 um copper layers.

BEFORE ORDERING
Confirm detected layer count, ENIG on BOTH jobs, thickness, plated/non-plated
holes, all mask openings, 4/6 bridge positions (display/GIGA), and electrical
netlist/Smart Tests notes on this exact revision. Compare the processed preview
with gerbers/ and assembly drawings. Resolve every discrepancy before ordering.
AISLER processing acceptance is PENDING; these local exports are not proof of it.

SOLDERING
All entries in bom-all.csv are FIT. Purchase/install components separately.
bom-smd.csv, placement.csv and assembly-F/B.pdf are soldering/inspection aids.
{'J1 DF23 is BOTTOM mounted. Clip U1/U2 tails to <=0.8 mm below the board. U3 WSON requires soldered exposed pad, using hot air/reflow.' if display else 'Carrier underside 13mm above GIGA; J2-J8 10mm tails, J9 14.83mm tails. J10 DC-005 barrel jack faces the service opening: 24V centre positive, 5.5/2.1mm. Check three 1.2mm-wide plated slots, lengths 4.0/3.6/4.0mm.'}
Use Molex 15018-0563 Type-A cable and 505110-6091 sockets. Keep the exact
Hirose DF23 suffix at the display connector.
Disconnect power before connecting the cable. See design-guide.md and cable PDF.
The Molex paste windows are 100% land area for a 0.10mm stencil reference.
DF23 paste follows its own drawing. No stencil is ordered.

QUALIFICATION
Local electrical, geometry and export checks: see verification/ and release status.
First-board fit, soldering, power, temperature, panel timing and operation remain
UNTESTED. The timing estimate is conditional, not complete PVT qualification.
See REVIEW.md for the open RGB timing and supply-transient items.
No manufacturing order has been placed. SHA256SUMS identifies these exact files.
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boards', nargs='+', choices=list(BOARDS), default=list(BOARDS))
    args=parser.parse_args()
    manufacturing = ROOT / 'manufacturing'
    verification = manufacturing / 'verification'
    for name in BOARDS:
        policy=validate_policy(ROOT/name/(name+'.kicad_pcb'),
                               ROOT/'references/layout-baselines'/(name+'.json'))
        if policy['result']!='PASS':
            raise RuntimeError(name+': layout feasibility/policy gate has not passed: '+
                               '; '.join(policy['errors']))
    visual = {name: validate_visual_review(verification / (name+'-visual') / 'review.json')
              for name in BOARDS}
    status = json.loads((manufacturing / 'release-status.json').read_text())
    assert status['digital_release_checks'] == 'PASS' and status['revision'] == REVISION
    assert design_hashes() == status['checked_design_sha256'], 'Design changed since verification; rerun release.py'
    names = ['display-adapter', 'giga-adapter']
    for name in names:
        for suffix,expected in status['boards'][name]['checked_source_sha256'].items():
            assert digest(ROOT/name/(name+suffix))==expected,(name,'Source changed since verification',suffix)
        assert status['boards'][name]['revision'] == BOARDS[name]['revision']
        assert status['boards'][name]['copper_layers'] == BOARDS[name]['layers']
        erc = json.loads((verification / (name + '-erc.json')).read_text())
        drc = json.loads((verification / (name + '-drc.json')).read_text())
        assert not erc['ignored_checks'] and not drc['ignored_checks']
        assert not any(s['violations'] for s in erc['sheets'])
        assert not drc['violations'] and not drc['unconnected_items'] and not drc['schematic_parity']
        for suffix in ['bom-placement', 'gerber-audit', 'aisler-audit', 'odb-audit', 'copper-audit']:
            assert json.loads((verification / (name + '-' + suffix + '.json')).read_text())['result'] == 'PASS'
        job = json.loads((manufacturing / release_name(name) / 'gerbers' /
                          (name + '-job.gbrjob')).read_text())
        assert job['GeneralSpecs']['LayerNumber'] == BOARDS[name]['layers']
        project = json.loads((ROOT / name / (name + '.kicad_pro')).read_text())
        assert not project['board']['design_settings']['drc_exclusions']
        assert not project['erc']['erc_exclusions']

    (verification / 'source-sha256.json').write_text(json.dumps(
        design_hashes(), indent=2) + '\n')
    status['visual_review'] = dict(result='PASS — recorded local inspection', boards=visual)
    (manufacturing/'release-status.json').write_text(json.dumps(status,indent=2)+'\n')
    order_path = verification / 'order-review.json'
    order = json.loads(order_path.read_text())
    for relative, expected in order['evidence_sha256'].items():
        assert digest(verification / relative) == expected, ('Order evidence changed', relative)
    for name, result in visual.items():
        assert order['boards'][name]['PCB_sha256'] == digest(ROOT/name/(name+'.kicad_pcb'))
        assert order['boards'][name]['upload_sha256'] == digest(
            manufacturing/release_name(name)/(release_name(name)+'-aisler-odb.zip'))
        order['evidence_sha256'][name+'-visual/review.json'] = result['manifest_sha256']
    order['routing'] = (
        'PASS — local copper/layout checks and complete hash-bound regional visual '
        'inspection. Every retained geometry candidate has an individual reason; '
        'this is not physical signal-integrity qualification or AISLER CAM acceptance.')
    order['visual_review'] = status['visual_review']
    order_path.write_text(json.dumps(order, indent=2)+'\n')

    progress_path = manufacturing / 'layout-unification-status.json'
    progress = json.loads(progress_path.read_text())
    progress.update(status='LOCAL RELEASE CHECKS PASS — AISLER PROCESSING PENDING',
                    local_visual_inspection='PASS — complete hash-bound regional inspection',
                    new_revision_issued=True)
    progress['revisions'] = progress.pop('draft_revisions', progress.get('revisions'))
    for name, result in visual.items():
        progress['boards'][name].update(
            source_sha256=digest(ROOT/name/(name+'.kicad_pcb')),
            release_eligible=True,
            verification='Local electrical, manufacturing, export and visual checks PASS',
            reviewed_regions=result['regions'],
            reviewed_geometry_candidates=result['reviewed_geometry_candidates'],
            review_manifest_sha256=result['manifest_sha256'])
    progress['remaining_work'] = [
        'Inspect AISLER processed jobs for these exact ODB++ archives before ordering.',
        'Qualify first-board mechanical fit, assembly, power, thermal and display operation.']
    progress_path.write_text(json.dumps(progress, indent=2)+'\n')
    releases = ROOT / 'releases'
    releases.mkdir(exist_ok=True)
    # The package guide omits repository-specific archive links.
    guide = (ROOT / 'README.md').read_text()
    start = guide.index('Release files:')
    end = guide.index('| Project |')
    guide = guide[:start] + guide[end:]
    guide = guide.replace('(manufacturing/REVIEW.md)', '(REVIEW.md)')
    with tempfile.TemporaryDirectory(prefix='display-release-') as scratch:
        temporary = Path(scratch)
        for name in args.boards:
            folder = temporary / release_name(name)
            copy_tree(manufacturing / folder.name, folder)
            copy_tree(verification, folder / 'verification')
            upload=manufacturing / folder.name / (folder.name+'-aisler-odb.zip')
            shutil.copy2(upload, releases/upload.name)
            for part in [name, 'libraries', 'references']:
                copy_tree(ROOT / part, folder / 'source' / part)
            for item in ['cable-specification.pdf', 'connections.csv', 'cable-conductors.csv']:
                shutil.copy2(ROOT / item, folder / item)
            shutil.copy2(manufacturing / 'release-status.json', folder / 'release-status.json')
            shutil.copy2(progress_path, folder / progress_path.name)
            (folder/'REVIEW.md').write_text((manufacturing/'REVIEW.md').read_text().replace(
                '`../references/`', '`source/references/`').replace(
                '(../references/', '(source/references/').replace(
                '(../releases/SHA256SUMS)', '(SHA256SUMS)'))
            (folder / 'AISLER_ORDER.txt').write_text(order_notes(name))
            (folder / 'design-guide.md').write_text(guide)
            archive(folder, releases / (folder.name + '-aisler-review.zip'))

        folder = temporary / ('smartwin-giga-rev-' + REVISION)
        for part in [*names, 'libraries', 'references', 'tools', 'assembly', 'procurement']:
            copy_tree(ROOT / part, folder / part)
        for item in ['README.md', 'cable-specification.pdf', 'connections.csv', 'cable-conductors.csv', '.gitignore']:
            shutil.copy2(ROOT / item, folder / item)
        copy_tree(verification, folder / 'manufacturing' / 'verification')
        copy_tree(SUPPORT, folder/'aisler-support')
        (folder/'aisler-support'/'.git').unlink(missing_ok=True)
        shutil.copy2(manufacturing/'aisler-support-lock.json',folder/'manufacturing'/'aisler-support-lock.json')
        shutil.copy2(manufacturing / 'release-status.json', folder / 'manufacturing' / 'release-status.json')
        shutil.copy2(progress_path, folder / 'manufacturing' / progress_path.name)
        shutil.copy2(manufacturing/'REVIEW.md', folder/'manufacturing'/'REVIEW.md')
        archive(folder, releases / (folder.name + '-source.zip'))
    checksums(releases)
    print('Verified archives:')
    for path in sorted(releases.glob('*.zip')):
        print(f'{path.name}: {path.stat().st_size / 1024**2:.2f} MiB')


if __name__ == '__main__':
    main()
