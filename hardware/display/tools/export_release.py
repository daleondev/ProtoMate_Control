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
from design import ROOT, BOARDS, release_name, copper_layers
from aisler import write_stackup
k.SwigPyIterator.next=k.SwigPyIterator.__next__
def run(args):subprocess.run(['kicad-cli',*map(str,args)],check=True)
def main():
    verify=ROOT/'manufacturing/verification';verify.mkdir(parents=True,exist_ok=True)
    for name in sys.argv[1:] or ['display-adapter','giga-adapter']:
        source=ROOT/name;pcb=source/(name+'.kicad_pcb');sch=source/(name+'.kicad_sch')
        out=ROOT/'manufacturing'/release_name(name);out.mkdir(exist_ok=True)
        b=k.LoadBoard(str(pcb));b.BuildConnectivity();k.ZONE_FILLER(b).Fill(b.Zones())
        k.SaveBoard(str(pcb),b);write_stackup(pcb,name)
        run(['sch','erc','--format','json','--exit-code-violations','-o',verify/(name+'-erc.json'),sch])
        # Check a fresh fill in the CLI itself. Cached/editor Python fills are
        # not authoritative for connectivity. Save exactly the checked board.
        run(['pcb','drc','--refill-zones','--save-board','--format','json','--schematic-parity','--all-track-errors','--exit-code-violations','-o',verify/(name+'-drc.json'),pcb])
        write_stackup(pcb,name)
        run(['pcb','export','odb','--compression','zip','--units','mm','--precision','6',
             '--check-zones','-o',out/(release_name(name)+'-aisler-odb.zip'),pcb])
        gerbers=out/'gerbers';gerbers.mkdir(exist_ok=True)
        layers=copper_layers(name)+['F.Mask','B.Mask','F.SilkS','B.SilkS','F.Paste','B.Paste','Edge.Cuts']
        run(['pcb','export','gerbers','--layers',','.join(layers),'--use-drill-file-origin','--subtract-soldermask','-o',str(gerbers)+'/',pcb])
        run(['pcb','export','drill','--excellon-separate-th','--drill-origin','plot','--generate-map','--generate-report','--report-path',out/'drill-report.txt','--map-format','pdf','-o',str(gerbers)+'/',pcb])
        run(['pcb','export','ipcd356','-o',out/'electrical-test.ipc',pcb])
        run(['sch','export','pdf','-o',out/'schematic.pdf',sch])
        for side in ['F','B']:
            args=['pcb','export','pdf','--layers',f'{side}.Fab,{side}.Courtyard,Edge.Cuts','--mode-single','--black-and-white','--drill-shape-opt','0','--scale','2' if name=='display-adapter' else '1.5','-o',out/('assembly-'+side+'.pdf')]
            if side=='B':args+=['--mirror']
            run([*args,pcb])
        run(['pcb','export','pos','--format','csv','--units','mm','--use-drill-file-origin','--smd-only','--exclude-dnp','-o',out/'placement.csv',pcb])
        run(['pcb','export','step','--force','--drill-origin','-o',out/(name+'.step'),pcb])
        step=out/(name+'.step')
        step.write_text('\n'.join(line.rstrip() for line in step.read_text().splitlines())+'\n')
        parts=json.loads((source/'components.json').read_text())
        grouped={}
        for p in parts:
            if p['assembly']=='PCB':continue
            key=p['mpn'],p['footprint'],p['assembly']
            if key not in grouped:grouped[key]=dict(Designators=[],Quantity=0,Value=p['value'],Manufacturer=p['mfr'],MPN=p['mpn'],Package=p['footprint'],Population='FIT',Assembly=p['assembly'])
            grouped[key]['Designators'].append(p['ref']);grouped[key]['Quantity']+=1
        rows=[]
        for row in grouped.values():row['Designators']=', '.join(row['Designators']);rows.append(row)
        for filename,selected in [('bom-all.csv',rows),('bom-smd.csv',[r for r in rows if r['Assembly']=='SMT'])]:
            with (out/filename).open('w') as f:
                w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator="\n");w.writeheader();w.writerows(selected)
        # KiCad native placement is retained as-is; verify exact SMT reference set.
        positions=list(csv.DictReader((out/'placement.csv').open()))
        assert {p['Ref'] for p in positions}=={p['ref'] for p in parts if p['assembly']=='SMT'},(name,'BOM/placement mismatch')
        fps={f.GetReference():f for f in b.GetFootprints()}
        origin=b.GetDesignSettings().GetAuxOrigin()
        for p in positions:
            f=fps[p['Ref']];pos=f.GetPosition()
            assert abs(float(p['PosX'])-k.ToMM(pos.x-origin.x))<.0001
            assert abs(float(p['PosY'])-k.ToMM(origin.y-pos.y))<.0001
            assert p['Side']==('bottom' if f.IsFlipped() else 'top')
        (verify/(name+'-bom-placement.json')).write_text(json.dumps({'revision':BOARDS[name]['revision'],'SMT_reference_count':len(positions),'result':'PASS','units':'mm','origin':'X=0, lower board edge; positive Y up; bottom X not negated','rotation':'KiCad native degrees, component-side convention; see assembly PDF'},indent=2)+'\n')

if __name__=='__main__':main()
