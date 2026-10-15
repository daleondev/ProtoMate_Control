#!/usr/bin/env python3
"""Report ground-return geometry from the filled two-layer board (requires Shapely).

A geometric audit complements KiCad DRC; it does not replace SI measurements.
"""
import math,json
from pathlib import Path
import pcbnew as k
from shapely.geometry import Polygon,Point,LineString
from shapely.ops import unary_union
k.SwigPyIterator.next=k.SwigPyIterator.__next__
from design import ROOT
p=ROOT/'display-adapter/display-adapter.kicad_pcb';b=k.LoadBoard(str(p))
def point(v):return (k.ToMM(v.x),k.ToMM(v.y))
def loop(line):return [point(line.CPoint(i)) for i in range(line.PointCount())]
def ground(layer):
 polys=[]
 for z in b.Zones():
  if z.GetNetname()!='GND' or z.GetLayer()!=layer:continue
  ps=z.GetFilledPolysList(layer)
  for i in range(ps.OutlineCount()):
   polys.append(Polygon(loop(ps.COutline(i)),[loop(ps.CHole(i,j)) for j in range(ps.HoleCount(i))]).buffer(0))
 return unary_union(polys)
g={l:ground(l) for l in [k.F_Cu,k.B_Cu]}
vias=[v for v in b.GetTracks() if isinstance(v,k.PCB_VIA)]
existing=[point(v.GetPosition()) for v in vias if v.GetNetname()=='GND']
clockvias=[point(v.GetPosition()) for v in vias if v.GetNetname()=='LCD_PCLK']
distances=[]
for t in b.GetTracks():
 if isinstance(t,k.PCB_VIA) or t.GetNetname()!='LCD_PCLK':continue
 line=LineString([point(t.GetStart()),point(t.GetEnd())]);length=line.length
 distances += [line.interpolate(i/max(1,math.ceil(length/.1)),normalized=True).distance(g[t.GetLayer()]) for i in range(math.ceil(length/.1)+1)]
report={'board':'display-adapter','revision':'B','copper_layers':2,'ground_pour_area_mm2':{b.GetLayerName(l):round(shape.area,2) for l,shape in g.items()},'total_ground_vias':len(existing),'clock_signal_vias':clockvias,'clock_via_nearest_ground_via_mm':[round(min(math.hypot(x-a,y-c) for a,c in existing),3) for x,y in clockvias],'maximum_clock_centre_to_same_layer_ground_pour_mm':round(max(distances),3),'sample_spacing_mm':.1,'interpretation':'Geometric return-path review; both ground pours are connected and stitched. This is not field-solver impedance or signal-integrity validation. Confirm panel-side timing and ringing on the first board.'}
(ROOT/'manufacturing/verification/display-adapter-layout-review.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
