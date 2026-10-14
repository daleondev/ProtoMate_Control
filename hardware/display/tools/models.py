#!/usr/bin/env python3
"""Local mechanical envelope models, in mm; no network-dependent library links.

Connector envelopes follow the retained manufacturer drawings. They simplify
contact springs/latches and are not manufacturer production-tooling models.
"""
import sys
sys.path.append('/usr/lib/freecad/lib')
import FreeCAD as A
import Part
from design import ROOT, components
DEST=ROOT/'libraries/3dmodels'
def box(x,y,z,w,d,h):return Part.makeBox(w,d,h,A.Vector(x,y,z))
def save(name,shapes):Part.makeCompound(shapes).exportStep(str(DEST/(name+'.step')))

# KiCad STEP coordinates have +Y up, opposite the footprint editor.
# FH28H: rear solder tails at y=0, insertion toward +Y, contact 1 on +X.
shell=box(-22.45,.55,0,44.9,6,2.55)
slot=box(-20.285,3.1,.55,40.57,3.6,.45)
shell=shell.cut(slot)
contacts=[box(19.75-i*.5-.1,-.5,0,.2,1.3,.15) for i in range(80)]
tabs=[box(x-1.1,4.95,0,2.2,1.6,.15) for x in [-22.175,22.175]]
save('FH28H-80S',[shell,*contacts,*tabs])

# DF23 plug: 14 x 3.1 x 1.2; mated board spacing is 1.5mm.
shell=box(-7,-1.55,0,14,3.1,1.2).cut(box(-6.35,-.85,.35,12.7,1.7,1.0))
contacts=[box(-6+i*.5-.095,y,0,.19,.9,.15) for i in range(25) for y in [-2.15,1.25]]
save('DF23C-50DP',[shell,*contacts])

# Phoenix 1751248: 7.5 x 7.3 x 8.5 above PCB. Supplied tails are
# 3.5mm; the assembly operation clips them to 0.8mm below the 1mm PCB.
body=box(-2,-3.9,0,7.5,7.3,8.5)
for x in [0,3.5]:body=body.cut(box(x-1.2,-4,1,2.4,3,2.5))
save('Terminal_2x3.5',[body,*[box(x-.25,-.45,-1.8,.5,.9,1.8) for x in [0,3.5]]])

for p in components('giga-adapter'):
    if p['kind']!='HEADER':continue
    xs=[v['x'] for v in p['pads']];ys=[-v['y'] for v in p['pads']]
    vertical=max(ys)-min(ys)>max(xs)-min(xs)
    rows=2 if p['ref'] in ['J8','J9'] else 1
    nx=len(set(xs));ny=len(set(ys))
    ex=1.205 if vertical else 1.525
    ey=1.525 if vertical else 1.205
    body=box(min(xs)-ex,min(ys)-ey,0,max(xs)-min(xs)+2*ex,max(ys)-min(ys)+2*ey,8.51)
    tails=[];tail=14.83 if p['ref']=='J9' else 10
    for v in p['pads']:
        x,y=v['x'],-v['y']
        body=body.cut(box(x-.4,y-.4,2,.8,.8,7))
        tails.append(box(x-.32,y-.32,-tail,.64,.64,tail+2))
    save(p['footprint'],[body,*tails])
print('Created connector/header envelope STEP models.')
