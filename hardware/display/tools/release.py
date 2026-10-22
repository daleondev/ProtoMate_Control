#!/usr/bin/env python3
"""Regenerate and locally validate the current AISLER release, without ordering.

Run with a Python environment containing pcbnew, NumPy, Pillow, Shapely,
Gerbonara and Matplotlib. KiCad, FreeCAD and Inkscape must also be installed.
Review the outputs, then run package_release.py --reviewed separately.
"""
import json
import hashlib
import subprocess
import sys
from datetime import date
from design import ROOT, BOARDS, REVISION, release_name


def run(script, *args):
    subprocess.run([sys.executable,str(ROOT/'tools'/script),*args],check=True)


def render():
    assembly=ROOT/'assembly';assembly.mkdir(exist_ok=True)
    views=[('display-adapter','top','320,0,25'),('display-adapter','bottom','25,0,25'),
           ('giga-adapter','top','320,0,25')]
    for name,side,rotation in views:
        label='display' if name=='display-adapter' else 'giga'
        output=assembly/f"{label}-{side}-rev-{BOARDS[name]['revision']}.png"
        subprocess.run(['kicad-cli','pcb','render','--width','1600','--height','1100',
            '--side',side,'--background','opaque','--quality','high','--perspective',
            '--rotate',rotation,'--zoom','.8','-o',str(output),
            str(ROOT/name/(name+'.kicad_pcb'))],check=True)
    subprocess.run(['kicad-cli','pcb','render','--width','1800','--height','1200',
        '--background','opaque','--quality','high','--perspective','--rotate','325,0,25',
        '--zoom','.85','-o',str(assembly/f"giga-mated-rev-{BOARDS['giga-adapter']['revision']}.png"),
        '/tmp/giga-mated-review.kicad_pcb'],check=True)


def design_hashes():
    """Include child sheets and shared libraries, not only the top-level files."""
    files = [p for folder in [*BOARDS, 'libraries']
             for p in (ROOT/folder).rglob('*')
             if p.is_file() and p.suffix not in ['.kicad_prl', '.pyc']
             and not p.name.startswith('~')
             and not any(part in {'.history', '.git', '__pycache__'} for part in p.parts)
             and not any(part.startswith('_restore_backup_') or part.endswith('-backups')
                         for part in p.parts)]
    return {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(files)}


def status():
    verification=ROOT/'manufacturing/verification'
    def read(name):return json.loads((verification/name).read_text())
    boards={}
    for name,config in BOARDS.items():
        drc=read(name+'-drc.json');erc=read(name+'-erc.json')
        assert not drc['violations'] and not drc['unconnected_items'] and not drc['schematic_parity']
        assert not any(s['violations'] for s in erc['sheets'])
        assert not drc['ignored_checks'] and not erc['ignored_checks']
        for suffix in ['aisler-audit','odb-audit','gerber-audit','bom-placement']:
            assert read(name+'-'+suffix+'.json')['result']=='PASS'
        gerber=read(name+'-gerber-audit.json');odb=read(name+'-odb-audit.json')
        boards[name]=dict(revision=config['revision'],copper_layers=config['layers'],
            thickness_mm=1.6,copper_um=35,finish='ENIG',template=config['template'],
            ERC_violations=0,DRC_violations=0,schematic_parity_violations=0,
            unrouted_connections=0,excluded_findings=0,ignored_ERC_categories=0,ignored_DRC_categories=0,
            SMT_components=read(name+'-bom-placement.json')['SMT_reference_count'],
            exported_pad_coordinates_checked=gerber['exported_pad_coordinates_and_nets_checked'],
            drill_hits_checked=gerber['drill_hits_checked'],upload=release_name(name)+'-aisler-odb.zip',
            odb_audit=odb)
        boards[name]['checked_source_sha256']={suffix:hashlib.sha256(
            (ROOT/name/(name+suffix)).read_bytes()).hexdigest()
            for suffix in ['.kicad_pcb','.kicad_pro','.kicad_sch','.kicad_dru']}
        assert read(name+'-aisler-audit.json')['source_sha256']==boards[name]['checked_source_sha256']['.kicad_pcb']
    power=read('power-review.json')
    assert power['result'].startswith('PASS')
    assert all(power['source_sha256'][name]==boards[name]['checked_source_sha256']['.kicad_pcb'] for name in BOARDS)
    result=dict(revision=REVISION,date=date.today().isoformat(),release='AISLER BARE-BOARD PROTOTYPE',
        digital_release_checks='PASS',boards=boards,checked_design_sha256=design_hashes(),
        electrical_audit=read('electrical-audit.json'),
        mechanical_review=read('mechanical-audit.json'),power_route_review=power,
        visual_review=dict(result='PENDING',reviewed=[]),
        aisler_processing=dict(status='PENDING',reason='Exact upload CAM preview, layer/options detection, holes, mask, bridge positions and netlist notes require account review. No project has been uploaded by this workflow.'),
        intentional_ERC_DRC_warnings=[],manufacturing_rule_waivers=[],
        physical_qualification=dict(status='NOT PERFORMED',pending=[
            'Connector fit, 1.6mm thickness tolerance, converter lead trim, mounting and strain relief',
            'Supply isolation, inrush, rise time, full-brightness voltage/current and thermal behaviour',
            'Panel clock/data timing, colour order, touch and independent supply sequencing']),
        timing_qualification='40.96MHz configured; bootstrap enables IO compensation. Calculated timing margin is conditional; first-board timing/load qualification remains pending. See timing-budget.json.',
        power_qualification='Independent precision logic and backlight supplies with supervised startup. First-board transient/current/thermal tests remain pending. See power-budget.json.',
        manufacturing_order_placed=False,assembly_order_placed=False,stencil_order_placed=False,firmware_changed=True)
    (ROOT/'manufacturing/release-status.json').write_text(json.dumps(result,indent=2)+'\n')


def main():
    for name in BOARDS:
        # Separate processes avoid persistent pcbnew editor state across boards.
        # Review the authoritative layout without resetting hand-edited silk,
        # outlines or footprint settings. Apply manufacturing profiles explicitly
        # when changing a design; the audits below enforce their requirements.
        run('export_release.py',name)
        run('audit_aisler.py',name)
        run('inspect_odb.py',name)
        run('inspect_gerbers.py',name)
    for script in ['audit.py','routing_report.py','review_layout.py','power_routes.py',
                   'power_budget.py','review_power.py','mechanical.py','drawings.py']:
        run(script)
    render()
    status()
    print('Local checks PASS. Review exported drawings/3D before packaging; AISLER processing remains pending.')


if __name__=='__main__':main()
