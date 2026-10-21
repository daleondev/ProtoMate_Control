#!/usr/bin/env python3
"""Read the exact upload ZIP independently; compare geometry, nets and bridges.

This is an export audit, not an AISLER CAM acceptance result.
"""
import hashlib
import json
import math
import re
import sys
import zipfile
import pcbnew as k
from shapely.geometry import Polygon
from design import ROOT, BOARDS, release_name
from aisler import BRIDGES
k.SwigPyIterator.next=k.SwigPyIterator.__next__


def features(text):
    symbols={int(i):s for i,s in re.findall(r'^\$(\d+) (\S+)',text,re.M)}
    return symbols,[l.split(';')[0].split() for l in text.splitlines() if l.startswith(('P ','L '))]


def main(name):
    path=ROOT/'manufacturing'/release_name(name)/(release_name(name)+'-aisler-odb.zip')
    b=k.LoadBoard(str(ROOT/name/(name+'.kicad_pcb')))
    with zipfile.ZipFile(path) as z:
        assert z.testzip() is None
        def read(p):return z.read(p).decode()
        layers=[]
        for block in re.findall(r'LAYER \{(.*?)\}',read('matrix/matrix'),re.S):
            layers.append(dict(re.findall(r'^\s*(\w+)=(.*?)\s*$',block,re.M)))
        copper=[l['NAME'] for l in layers if l['TYPE']=='SIGNAL']
        assert copper==(['F.CU','B.CU'] if BOARDS[name]['layers']==2 else ['F.CU','IN1.CU','IN2.CU','B.CU'])
        assert {l['NAME'] for l in layers if l['TYPE']=='SOLDER_MASK'}=={'F.MASK','B.MASK'}
        bridge=next(l for l in layers if l['NAME']=='AISLER-BRIDGES')
        assert bridge['CONTEXT']=='MISC' and bridge['TYPE']=='DOCUMENT'
        symbols,ff=features(read('steps/pcb/layers/aisler-bridges/features'))
        actual=[]
        for f in ff:
            assert f[0]=='L' and symbols[int(f[5])]=='r55.880'
            actual.append(tuple(round(float(v),6) for v in f[1:5]))
        expected=[(a[0],-a[1],c[0],-c[1]) for a,c in BRIDGES[name]]
        assert sorted(actual)==sorted(expected),('bridge coordinates',actual,expected)
        profile=read('steps/pcb/profile');vertices=[]
        for line in profile.splitlines():
            if line.startswith(('OB ','OS ')):
                _,x,y,*_=line.split();vertices.append((float(x),float(y)))
        assert vertices[0]==vertices[-1]
        contour=Polygon(vertices);assert contour.is_valid and contour.area>1000
        x0,y0,x1,y1=contour.bounds
        assert abs(x1-x0-(55 if BOARDS[name]['layers']==2 else 111))<1e-6
        assert abs(y1-y0-(35 if BOARDS[name]['layers']==2 else 53.34))<1e-6
        nets=read('steps/pcb/netlists/cadnet/netlist')
        names={int(n):value for n,value in re.findall(r'^\$(\d+) (.+)$',nets,re.M)}
        points=set()
        for line in nets.splitlines():
            if not line or not line[0].isdigit():continue
            fields=line.split();points.add((round(float(fields[2]),6),round(float(fields[3]),6),names[int(fields[0])]))
        audited=0
        for f in b.GetFootprints():
            for pad in f.Pads():
                if not pad.GetNetname():continue
                p=pad.GetPosition();key=(round(k.ToMM(p.x),6),round(-k.ToMM(p.y),6),pad.GetNetname())
                assert key in points,(f.GetReference(),pad.GetNumber(),'ODB netlist mismatch',key)
                audited+=1
        drills=[]
        for plated,label in [(True,'plated'),(False,'non-plated')]:
            layer=f'steps/pcb/layers/drill_{label}_f.cu-b.cu/'
            symbols,ff=features(read(layer+'features'))
            tools=read(layer+'tools')
            assert ('TYPE=PLATED' if plated else 'TYPE=NON_PLATED') in tools
            for f in ff:
                assert f[0]=='P'
                symbol=symbols[int(f[3])];assert symbol.startswith('r')
                drills.append((round(float(f[1]),6),round(float(f[2]),6),round(float(symbol[1:])/1000,6),plated))
        expected=[]
        for f in b.GetFootprints():
            for pad in f.Pads():
                if pad.GetDrillSize().x:
                    p=pad.GetPosition();expected.append((round(k.ToMM(p.x),6),round(-k.ToMM(p.y),6),round(k.ToMM(pad.GetDrillSize().x),6),pad.GetAttribute()!=k.PAD_ATTRIB_NPTH))
        for v in b.GetTracks():
            if isinstance(v,k.PCB_VIA):
                p=v.GetPosition();expected.append((round(k.ToMM(p.x),6),round(-k.ToMM(p.y),6),round(k.ToMM(v.GetDrillValue()),6),True))
        assert sorted(drills)==sorted(expected),'ODB drill coordinate/diameter/plating mismatch'
        # All exposed copper contacts must have their own mask aperture.
        mask_counts={}
        for side,layer in [('f',k.F_Mask),('b',k.B_Mask)]:
            _,ff=features(read(f'steps/pcb/layers/{side}.mask/features'))
            locations={(round(float(f[1]),6),round(float(f[2]),6)) for f in ff if f[0]=='P'}
            checked=0
            for f in b.GetFootprints():
                for pad in f.Pads():
                    if not pad.IsOnLayer(layer):continue
                    p=pad.GetPosition();assert (round(k.ToMM(p.x),6),round(-k.ToMM(p.y),6)) in locations
                    checked+=1
            for v in b.GetTracks():
                if isinstance(v,k.PCB_VIA):
                    p=v.GetPosition();assert (round(k.ToMM(p.x),6),round(-k.ToMM(p.y),6)) in locations
            mask_counts[side]=checked
    result=dict(result='PASS',board=name,revision=BOARDS[name]['revision'],upload_zip=path.name,
                upload_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),copper_layers=copper,
                pcb_profile_area_mm2=contour.area,bridges_checked=len(actual),netlist_pad_contacts_checked=audited,
                drill_hits_checked=len(drills),mask_pad_openings_checked=mask_counts,
                coordinates='mm; KiCad absolute X, negated Y; 6 decimal export precision',
                aisler_processing_acceptance='PENDING')
    (ROOT/'manufacturing/verification'/(name+'-odb-audit.json')).write_text(json.dumps(result,indent=2)+'\n')
    print(name,'ODB++ export PASS')


if __name__=='__main__':main(sys.argv[1])
