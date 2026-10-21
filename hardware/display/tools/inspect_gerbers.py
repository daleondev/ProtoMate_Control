#!/usr/bin/env python3
"""Independently parse manufacturing exports with Gerbonara, then render them.

Requires gerbonara, pcbnew and Inkscape. No KiCad plotting code is used to read or
render the exported copper. This checks exports, not only their source PCB.
"""
import json,math,subprocess,sys,warnings
from pathlib import Path
import pcbnew as k
from gerbonara import LayerStack
from gerbonara.graphic_objects import Flash
from gerbonara.utils import MM
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
            assert isinstance(obj,Flash),'Unexpected slot; inspect before release'
            obj=obj.converted(MM);actual.append((round(obj.x,4),round(obj.y,4),round(obj.tool.diameter,4),plated))
    expected=[]
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetDrillSize().x:
                assert p.GetDrillSize().x==p.GetDrillSize().y
                x,y=coord(p.GetPosition());expected.append((round(x,4),round(y,4),round(k.ToMM(p.GetDrillSize().x),4),p.GetAttribute()!=k.PAD_ATTRIB_NPTH))
    for v in b.GetTracks():
        if isinstance(v,k.PCB_VIA):
            x,y=coord(v.GetPosition());expected.append((round(x,4),round(y,4),round(k.ToMM(v.GetDrillValue()),4),True))
    assert len(actual)==len(expected),'Drill count mismatch'
    remaining=list(expected)
    for x,y,diameter,plated in actual:
        index=next((i for i,(xx,yy,dd,pp) in enumerate(remaining) if pp==plated and dd==diameter and math.hypot(x-xx,y-yy)<=.0008),None)
        assert index is not None,('Drill coordinate/tool/plating mismatch',x,y,diameter,plated)
        remaining.pop(index)
    expected_layers={('top',v) for v in ['copper','mask','paste','silk']}|{('bottom',v) for v in ['copper','mask','paste','silk']}|{('mechanical','outline')}
    if BOARDS[name]['layers']==4:expected_layers|={('inner_1','copper'),('inner_2','copper')}
    assert set(stack.graphic_layers)==expected_layers
    outline=stack.graphic_layers['mechanical','outline'];bbox=outline.bounding_box()
    width=55 if name=='display-adapter' else 111;height=35 if name=='display-adapter' else 53.34
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
