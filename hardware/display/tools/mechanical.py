#!/usr/bin/env python3
"""Nominal mating-stack model and mechanical audit, without changing PCB copper."""
import sys,json
from pathlib import Path
sys.path.append('/usr/lib/freecad/lib')
import FreeCAD as A
import Part
import pcbnew as k
from design import ROOT,components,GEOMETRY
k.SwigPyIterator.next=k.SwigPyIterator.__next__

def main():
    models=ROOT/'libraries/3dmodels';assembly=ROOT/'assembly';assembly.mkdir(exist_ok=True)
    # Official STEP omits the outer Arduino socket bodies. Add nominal socket
    # envelopes solely for fit visualization; keep their provenance explicit.
    bodies=[]
    for p in components('giga-adapter'):
        if p['kind']!='HEADER' or p['ref']=='J9':continue
        xs=[a['x'] for a in p['pads']];ys=[53.34-a['y'] for a in p['pads']]
        vertical=max(ys)-min(ys)>max(xs)-min(xs);ex,ey=(1.205,1.525) if vertical else (1.525,1.205)
        shape=Part.makeBox(max(xs)-min(xs)+2*ex,max(ys)-min(ys)+2*ey,8.5,A.Vector(min(xs)-ex,min(ys)-ey,0))
        for x,y in zip(xs,ys):shape=shape.cut(Part.makeBox(.9,.9,8.6,A.Vector(x-.45,y-.45,0)))
        bodies.append(shape)
    Part.makeCompound(bodies).exportStep(str(models/'GIGA_outer_socket_envelopes.step'))
    giga=Part.read(str(models/'GIGA_R1_WIFI.step'))
    carrier=Part.read(str(ROOT/'manufacturing/giga-adapter-rev-A/giga-adapter.step'));carrier.translate(A.Vector(0,0,13))
    combined=Part.makeCompound([giga,carrier,*bodies]);combined.exportStep(str(assembly/'giga-mated-rev-A.step'))
    # PCB copy for KiCad's renderer. Absolute model paths are only used in /tmp.
    b=k.LoadBoard(str(ROOT/'giga-adapter/giga-adapter.kicad_pcb'))
    for i,model in enumerate(['GIGA_R1_WIFI.step','GIGA_outer_socket_envelopes.step']):
        f=k.FOOTPRINT(b);f.SetReference('MECH'+str(i));f.SetPosition(k.VECTOR2I(0,k.FromMM(53.34)));f.Reference().SetVisible(False);f.Value().SetVisible(False);f.SetAttributes(k.FP_EXCLUDE_FROM_BOM|k.FP_EXCLUDE_FROM_POS_FILES);b.Add(f)
        m=k.FP_3DMODEL();m.m_Filename=str(models/model);m.m_Offset=k.VECTOR3D(0,0,-14.6);m.m_Scale=k.VECTOR3D(1,1,1);f.Add3DModel(m)
    k.SaveBoard('/tmp/giga-mated-review.kicad_pcb',b)
    temporary=Path('/tmp/giga-mated-review.kicad_pcb')
    temporary.write_text(temporary.read_text().replace('${KIPRJMOD}/../libraries',str(models.parent)))
    # Header centres versus the separately retained official CAD extraction.
    checked=0
    fps={f.GetReference():f for f in b.GetFootprints()}
    for p in components('giga-adapter'):
        if p['kind']!='HEADER':continue
        for pad in p['pads']:
            orig=next(v for v in GEOMETRY['headers'][p['value'].split()[0]] if v['pin']==pad['source_pin'])
            actual=next(v for v in fps[p['ref']].Pads() if v.GetNumber()==str(pad['pin'])).GetPosition()
            assert abs(k.ToMM(actual.x)-orig['x'])<.00002 and abs(k.ToMM(actual.y)-orig['y'])<.00002
            checked+=1
    # All non-mating GIGA components are below the 13mm carrier underside.
    assert giga.BoundBox.ZMax<=8.50001
    result={'result':'PASS — nominal geometry','header_contact_centres':checked,'giga_max_modelled_component_height_mm':giga.BoundBox.ZMax,'carrier_underside_above_giga_mm':13,'minimum_vertical_nonmating_body_clearance_mm':13-giga.BoundBox.ZMax,'outer_header_nominal_height_mm':8.5,'outer_pin_nominal_insertion_mm':3.9,'inner_header_modelled_height_mm':3.7052,'inner_pin_tip_below_giga_top_mm':.23,'inner_entry_hole_diameter_mm':1.1,'pin_square_diagonal_mm':.64*2**.5,'display_connector_mated_plane_mm':1.5,'display_terminal_max_tail_below_pcb_mm':.8,'qualifications':['Official GIGA STEP omits outer socket bodies; added 8.5mm nominal envelopes are identified, not manufacturer models.','Hirose models are drawing-based envelopes, not production-tooling models.','Mating contact spring deflection and assembly tolerances require physical first-board fit verification.','Mounts and FFC strain relief belong to the enclosure; neither connector supports cable or PCB weight.']}
    (ROOT/'manufacturing/verification/mechanical-audit.json').write_text(json.dumps(result,indent=2)+'\n')

if __name__=='__main__':main()
