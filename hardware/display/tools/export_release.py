#!/usr/bin/env python3
"""Export revision-matched fabrication data. Refuses ERC/DRC failures.

Run independent audit.py and inspect_gerbers.py before package_release.py.
"""
import csv
import json
import subprocess
import sys
from pathlib import Path
import pcbnew as k
from design import ROOT, REVISION
k.SwigPyIterator.next=k.SwigPyIterator.__next__
def run(args):subprocess.run(['kicad-cli',*map(str,args)],check=True)
def main():
    verify=ROOT/'manufacturing/verification';verify.mkdir(parents=True,exist_ok=True)
    for name in sys.argv[1:] or ['display-adapter','giga-adapter']:
        source=ROOT/name;pcb=source/(name+'.kicad_pcb');sch=source/(name+'.kicad_sch')
        out=ROOT/'manufacturing'/(name+'-rev-'+REVISION);out.mkdir(exist_ok=True)
        run(['sch','erc','--format','json','--exit-code-violations','-o',verify/(name+'-erc.json'),sch])
        run(['pcb','drc','--format','json','--exit-code-violations','-o',verify/(name+'-drc.json'),pcb])
        gerbers=out/'gerbers';gerbers.mkdir(exist_ok=True)
        run(['pcb','export','gerbers','--layers','F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,B.SilkS,F.Paste,B.Paste,Edge.Cuts','--use-drill-file-origin','--subtract-soldermask','-o',str(gerbers)+'/',pcb])
        run(['pcb','export','drill','--excellon-separate-th','--drill-origin','plot','--generate-map','--generate-report','--report-path',out/'drill-report.txt','--map-format','pdf','-o',str(gerbers)+'/',pcb])
        run(['pcb','export','ipcd356','-o',out/'electrical-test.ipc',pcb])
        run(['sch','export','pdf','-o',out/'schematic.pdf',sch])
        for side in ['F','B']:
            args=['pcb','export','pdf','--layers',f'{side}.Fab,{side}.Courtyard,Edge.Cuts','--sketch-pads-on-fab-layers','--mode-single','--black-and-white','--scale','2' if name=='display-adapter' else '1.5','-o',out/('assembly-'+side+'.pdf')]
            if side=='B':args+=['--mirror']
            run([*args,pcb])
        run(['pcb','export','pos','--format','csv','--units','mm','--use-drill-file-origin','--smd-only','--exclude-dnp','-o',out/'placement.csv',pcb])
        run(['pcb','export','step','--force','--drill-origin','-o',out/(name+'.step'),pcb])
        parts=json.loads((source/'components.json').read_text())
        grouped={}
        for p in parts:
            if p['assembly']=='PCB':continue
            key=p['mpn'],p['footprint'],p['assembly']
            if key not in grouped:grouped[key]=dict(Designators=[],Quantity=0,Value=p['value'],Manufacturer=p['mfr'],MPN=p['mpn'],Package=p['footprint'],Population='FIT',Assembly=p['assembly'])
            grouped[key]['Designators'].append(p['ref']);grouped[key]['Quantity']+=1
        rows=[]
        for row in grouped.values():row['Designators']=', '.join(row['Designators']);rows.append(row)
        for filename,selected in [('bom-all.csv',rows),('bom-pcbway-smt.csv',[r for r in rows if r['Assembly']=='SMT'])]:
            with (out/filename).open('w') as f:
                w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(selected)
        # KiCad native placement is retained as-is; verify exact SMT reference set.
        positions=list(csv.DictReader((out/'placement.csv').open()))
        assert {p['Ref'] for p in positions}=={p['ref'] for p in parts if p['assembly']=='SMT'},(name,'BOM/placement mismatch')
        b=k.LoadBoard(str(pcb));fps={f.GetReference():f for f in b.GetFootprints()}
        origin=b.GetDesignSettings().GetAuxOrigin()
        for p in positions:
            f=fps[p['Ref']];pos=f.GetPosition()
            assert abs(float(p['PosX'])-k.ToMM(pos.x-origin.x))<.0001
            assert abs(float(p['PosY'])-k.ToMM(origin.y-pos.y))<.0001
            assert p['Side']==('bottom' if f.IsFlipped() else 'top')
        (verify/(name+'-bom-placement.json')).write_text(json.dumps({'revision':REVISION,'SMT_reference_count':len(positions),'result':'PASS','units':'mm','origin':'X=0, lower board edge; positive Y up; bottom X not negated','rotation':'KiCad native degrees, component-side convention; see assembly PDF'},indent=2)+'\n')

if __name__=='__main__':main()
