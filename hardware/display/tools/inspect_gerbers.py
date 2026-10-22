#!/usr/bin/env python3
"""Independently parse manufacturing exports with Gerbonara, then render them.

Requires gerbonara, pcbnew and Inkscape. No KiCad plotting code is used to read or
render the exported copper. This checks exports, not only their source PCB.
"""
import json,math,subprocess,sys,warnings
from pathlib import Path
import pcbnew as k
from gerbonara import LayerStack
from gerbonara.graphic_objects import Flash, Line
from gerbonara.utils import MM
from drill_geometry import pad_drill, canonical_drill
from design import ROOT, BOARDS, release_name
k.SwigPyIterator.next=k.SwigPyIterator.__next__

def main(name):
    out=ROOT/'manufacturing'/release_name(name);report=ROOT/'manufacturing/verification';preview=out/'inspection';preview.mkdir(exist_ok=True)
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always');stack=LayerStack.open(out/'gerbers')
    allowed='G90 header statement found after end of header'
    assert all(allowed in str(w.message) for w in caught),[str(w.message) for w in caught]
    b=k.LoadBoard(str(ROOT/name/(name+'.kicad_pcb')));origin=b.GetDesignSettings().GetAuxOrigin()
    def coord(p):return k.ToMM(p.x-origin.x),k.ToMM(origin.y-p.y)
    audited=0;layers={}
    for side,layer in [('top',k.F_Cu),('bottom',k.B_Cu)]:
        gerber=stack.graphic_layers[side,'copper']
        pads={o.attrs['.P']:o for o in gerber.objects if isinstance(o,Flash) and '.P' in o.attrs}
        for f in b.GetFootprints():
            for p in f.Pads():
                if not p.IsOnLayer(layer) or not p.GetNumber():continue
                key=(f.GetReference(),p.GetNumber());assert key in pads,(side,'missing flashed pad',key)
                obj=pads[key].converted(MM);x,y=coord(p.GetPosition())
                assert math.hypot(obj.x-x,obj.y-y)<.0001,(side,key,(x,y),(obj.x,obj.y))
                if p.GetNetname():assert obj.attrs.get('.N')==(p.GetNetname(),),(side,key,obj.attrs)
                audited+=1
    actual=[]
    for file,plated in [(stack.drill_pth,True),(stack.drill_npth,False)]:
        for obj in file.objects:
            obj=obj.converted(MM)
            if isinstance(obj,Flash):a=c=(obj.x,obj.y)
            else:
                assert isinstance(obj,Line),'Only round holes and straight slots supported'
                a,c=(obj.x1,obj.y1),(obj.x2,obj.y2)
            actual.append(canonical_drill(a,c,obj.tool.diameter,plated,4))
    expected=[]
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetDrillSize().x:
                a,c,d=pad_drill(p)
                def convert(v):return v[0]-k.ToMM(origin.x),k.ToMM(origin.y)-v[1]
                expected.append(canonical_drill(convert(a),convert(c),d,p.GetAttribute()!=k.PAD_ATTRIB_NPTH,4))
    for v in b.GetTracks():
        if isinstance(v,k.PCB_VIA):
            a=coord(v.GetPosition());expected.append(canonical_drill(a,a,k.ToMM(v.GetDrillValue()),True,4))
    assert len(actual)==len(expected),'Drill count mismatch'
    remaining=list(expected)
    for drill in actual:
        index=next((i for i,other in enumerate(remaining) if other[4:]==drill[4:]
                    and math.dist(other[:2],drill[:2])<=.0008 and math.dist(other[2:4],drill[2:4])<=.0008),None)
        assert index is not None,('Drill/slot coordinate/tool/plating mismatch',drill)
        remaining.pop(index)
    expected_layers={('top',v) for v in ['copper','mask','paste','silk']}|{('bottom',v) for v in ['copper','mask','paste','silk']}|{('mechanical','outline')}
    if BOARDS[name]['layers']==4:expected_layers|={('inner_1','copper'),('inner_2','copper')}
    assert set(stack.graphic_layers)==expected_layers
    outline=stack.graphic_layers['mechanical','outline'];bbox=outline.bounding_box()
    width=55 if name=='display-adapter' else 111;height=50 if name=='display-adapter' else 53.34
    assert abs(bbox[1][0]-bbox[0][0]-.05-width)<.001
    assert abs(bbox[1][1]-bbox[0][1]-.05-height)<.001
    windows=0
    for o in stack.graphic_layers['top','paste'].objects:
        (x0,y0),(x1,y1)=o.bounding_box(MM)
        if sorted([round(x1-x0,3),round(y1-y0,3)])==[.25,1.0]:windows+=1
    assert windows==80,('Hirose paste aperture mismatch',windows)
    # Summing object bounding boxes overestimates ink (overlaps counted twice),
    # giving a conservative independent bound on AISLER's 25% silk coverage.
    from shapely.geometry import Polygon
    from design import board_outline
    area=Polygon(board_outline(name)).area
    silk_coverage={}
    for side in ['top','bottom']:
        total=0
        for o in stack.graphic_layers[side,'silk'].objects:
            (x0,y0),(x1,y1)=o.bounding_box(MM);total+=(x1-x0)*(y1-y0)
        silk_coverage[side]=100*total/area
        assert total/area<.25,(side,'silk coverage upper bound',total/area)
    for key,g in stack.graphic_layers.items():
        stem='-'.join(key);(preview/(stem+'.svg')).write_text(str(g.to_svg(margin=1)))
        layers[stem]={'objects':len(g.objects),'bounds_mm':g.bounding_box()}
    for side in ['top','bottom']:
        svg=preview/(side+'.svg');png=preview/(side+'.png');svg.write_text(str(stack.to_pretty_svg(side=side,margin=1)))
        subprocess.run(['inkscape',str(svg),'--export-width=2000','--export-filename='+str(png)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    parser_notes=[allowed+'; KiCad emits G90 after %. Absolute coordinates verified against PCB.'] if caught else []
    parser_notes.append('The renderer reports an unused additional argument in KiCad RoundRect aperture macros; Gerbonara renders the defined macro primitives. Pad coordinates and connector paste dimensions were independently checked.')
    result={'board':name,'result':'PASS','exported_pad_coordinates_and_nets_checked':audited,'drill_hits_checked':len(actual),'ffc_paste_windows_checked':windows,'silk_coverage_upper_bound_percent':silk_coverage,'layers':layers,'parser_notes':parser_notes,'visual_inspection':'See inspection/*.svg and top/bottom.png; recorded separately in release status.'}
    (report/(name+'-gerber-audit.json')).write_text(json.dumps(result,indent=2)+'\n');print(name,audited,'pads,',len(actual),'drills: PASS')

if __name__=='__main__':
    for n in sys.argv[1:] or ['display-adapter','giga-adapter']:main(n)
